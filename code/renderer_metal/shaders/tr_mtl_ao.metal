// tr_mtl_ao.metal -- the temporal path's ambient occlusion (r_ao; mbTemporalAo in tr_mtl.m, RB_TemporalAo in
// tr_temporal.c): ground truth ambient occlusion (GTAO; Jimenez, Wu, Pesce, Jarabo, "Practical Real-Time Strategies for
// Accurate Indirect Occlusion", 2016) as Intel's XeGTAO does it (MIT license). The ideas, the passes and the fixes are
// JayRewd's Vulkan renderer's (renderervk's ao_prefilter.comp, gtao_main.comp, ao_temporal.comp, ao_denoise.comp,
// ao_apply.frag, ao_common.glsl, ao_result.glsl), written again for Metal. Run in the main view after its opaque
// surfaces (the scene pass ended there, its depth stored), before what is blended over them:
//  - mb_ao_prefilter_cs: the depth to view distances, every pixel (level 0, 32-bit: in half floats a wall close to the
//    eye or at a grazing angle stepped from pixel to pixel, each step a ledge that occluded the next), levels 1 to 4
//    each texel a weighted mean of the 2x2 below it, and the grid's (a texel a 2x2 of pixels, its top-left one's)
//  - mb_ao_main_cs: the occlusion of the grid's texels, noisy (a few slices and samples, rotated by a noise that
//    changes every pixel and every frame)
//  - mb_ao_temporal_cs: blended with the last frames' result and blurred within the depth's edges
//  - mb_ao_denoise_cs: a second blur (quality 3, and without history: the first frames after a cut are the noisiest)
//  - mb_ao_apply_fs: the first draw of the scene's pass after the occlusion: the grid's result upsampled to the pixels
//    and blended into the color (dst * a + the fog's share), the scissor the main view
// View distances are positive, the view weapon's negative (it is in its own depth range below the floor, r_depthClamp,
// and only occludes itself), the sky (depth 1) at the far distance. Positions, the radius and the motion are in the
// target's pixels; the grid's texel is its top-left pixel (its view distance and its position: a plane stays a plane).
//
// Keep MbAoUniforms in sync with mbAoUniforms_t in tr_mtl.m.

#include <metal_stdlib>
using namespace metal;

struct MbAoUniforms
{
	int4		rect;		// the main view in target pixels: x, y (from the top left), width, height
	int4		ints;		// x: slices, y: steps a side, z: the noise's frame index, w: the blur pass (MB_AO_PASS_*)
	float4		proj;		// the projection's [0], [5], [8], [9] (GL's, without the jitter)
	float4		depth;		// [10], [14], the depth floor (below it the view weapon's range; 0: none), the far distance
	float4		view;		// xy: the jitter in NDC, z: 1 with a history (the last frame's), w: 1 with the motion vectors
	float4		ao;			// the radius (world units), the falloff's share of it, the thin occluder compensation, r_aoStrength
	float4		ao2;		// the multi-bounce albedo (0: none), 1 / the target's width, the most frames blended, the blur's center weight
	float4		fogColor;	// rgb, w: 1 when the view is fogged
	float4		fogParams;	// the fog factor's scale and offset of the view distance, z: 1 the occlusion alone (r_temporalShow 3), w: 1 / the target's height
	float4x4	reproject;	// this frame's eye space (GL's: x right, y up, looking down -z) to the last frame's clip space
};

#define MB_AO_LEVELS	5

// the blurs (ints.w): the temporal pass's into the first blur's target (two blurs) or into the result (one), the
// denoise pass's from the first blur's target into the result
#define MB_AO_PASS_HISTORY_TO_BLUR		0
#define MB_AO_PASS_HISTORY_TO_RESULT	1
#define MB_AO_PASS_BLUR_TO_RESULT		2

constant float MB_PI = 3.1415926535897932f;
constant float MB_HALF_PI = 1.5707963267948966f;

// the sky and what is farther at the far distance
static float MB_AoFar( constant MbAoUniforms &u )
{
	return u.depth.w;
}

static bool MB_AoIsFar( constant MbAoUniforms &u, float d )
{
	return d >= MB_AoFar( u ) * 0.999f;
}

// the view distance of a window depth whose range [lo, hi] stands for GL's [-1, 1]
static float MB_AoRangeDistance( constant MbAoUniforms &u, float wz, float lo, float hi )
{
	float ndc = ( wz - lo ) / ( hi - lo ) * 2.0f - 1.0f;
	float denom = ndc + u.depth.x;

	if ( abs( denom ) < 1e-9f ) {
		return MB_AoFar( u );
	}
	return clamp( u.depth.y / denom, 0.0f, MB_AoFar( u ) );
}

// the view distance of a pixel of the depth target: the world's range [floor, 1], the view weapon's [0, floor]
static float MB_AoDistance( constant MbAoUniforms &u, float wz )
{
	if ( wz >= 1.0f ) {
		return MB_AoFar( u );
	}
	if ( wz < u.depth.z ) {
		return -max( MB_AoRangeDistance( u, wz, 0.0f, u.depth.z ), 1e-4f );
	}
	return MB_AoRangeDistance( u, wz, u.depth.z, 1.0f );
}

// the NDC of a target position (pixel centers at .5) without the jitter: where the surface it shows is in the
// unjittered projection
static float2 MB_AoPixelNdc( constant MbAoUniforms &u, float2 pixel )
{
	float2 ndc = ( pixel - float2( u.rect.xy ) ) / float2( u.rect.zw ) * 2.0f - 1.0f;

	ndc.y = -ndc.y;		// the target's y goes down
	return ndc - u.view.xy;
}

// the eye space position at view distance d of a target position
static float3 MB_AoViewPosition( constant MbAoUniforms &u, float2 pixel, float d )
{
	float2 ndc = MB_AoPixelNdc( u, pixel );

	return float3( ( ndc.x + u.proj.z ) * d / u.proj.x, ( ndc.y + u.proj.w ) * d / u.proj.y, -d );
}

// Jimenez et al. 2016, the multi-bounce fit
static float MB_AoMultiBounce( float v, float albedo )
{
	float a = 2.0404f * albedo - 0.3324f;
	float b = -4.7951f * albedo + 0.6417f;
	float c = 2.7552f * albedo + 0.6903f;

	return max( v, ( ( v * a + b ) * v + c ) * v );
}

// what the apply pass blends into the scene for the blurred visibility v at view distance sd: v to the power
// r_aoStrength, the multi-bounce fit, in the scene's gamma-encoded values (2.2: the game draws them so), undone through
// the fog: the scene is C f + fog (1 - f), the occlusion belongs on C alone; with a the factor, rgb = fog (1 - f)(1 - a),
// blended ONE, SRC_ALPHA: dst a + rgb
static float4 MB_AoResult( constant MbAoUniforms &u, float v, float sd )
{
	float d = abs( sd );

	if ( MB_AoIsFar( u, d ) ) {
		return float4( 0.0f, 0.0f, 0.0f, 1.0f );
	}
	v = pow( saturate( v ), u.ao.w );
	if ( u.ao2.x > 0.0f ) {
		v = MB_AoMultiBounce( v, u.ao2.x );
	}
	float a = pow( saturate( v ), 1.0f / 2.2f );
	float3 fog = float3( 0.0f );

	if ( u.fogColor.w != 0.0f ) {
		float f = saturate( d * u.fogParams.x + u.fogParams.y );

		fog = u.fogColor.rgb * ( 1.0f - f ) * ( 1.0f - a );
	}
	return float4( fog, a );
}

// XeGTAO's edges of a texel of view distance z to its neighbours (left, right, top, bottom): 1 where the neighbour's
// distance continues the surface (the slope across the texel taken out), 0 where it's another surface; continuous (in
// XeGTAO's 2 bits each an edge near a step flipped whenever the depth was rounded anew, and the occlusion wobbled in
// bands). A texel of the grid is two pixels across.
static float4 MB_AoEdges( float z, float l, float r, float t, float b )
{
	float4 e = float4( l, r, t, b ) - z;
	float slopeLR = ( e.y - e.x ) * 0.5f;
	float slopeTB = ( e.w - e.z ) * 0.5f;
	float4 adjusted = e + float4( slopeLR, -slopeLR, slopeTB, -slopeTB );

	e = min( abs( e ), abs( adjusted ) );
	return saturate( 1.25f - e / ( abs( z ) * ( 0.011f * 2.0f ) + 1e-4f ) );
}

// XeGTAO's blur of the texel C: the 3x3 around it, each neighbour weighted by the depth edges between them both ways
// (from the view distances of the 3x3 and the 4 texels two away, LL, RR, TT, BB), so nothing crosses from one surface
// to another; the center weighs ao2.w (the first of two blurs a fifth of it), times 1 + a quarter of the frames
// blended past the first: a converged texel keeps its detail. v: TL, T, TR, L, C, R, BL, B, BR; z: the same, then LL,
// RR, TT, BB
static float MB_AoBlur( constant MbAoUniforms &u, thread const float *v, thread const float *z, float frames, bool final )
{
	float4 eC = MB_AoEdges( z[4], z[3], z[5], z[1], z[7] );
	float4 eL = MB_AoEdges( z[3], z[9], z[4], z[0], z[6] );
	float4 eR = MB_AoEdges( z[5], z[4], z[10], z[2], z[8] );
	float4 eT = MB_AoEdges( z[1], z[0], z[2], z[11], z[4] );
	float4 eB = MB_AoEdges( z[7], z[6], z[8], z[4], z[12] );
	const float diag = 0.85f * 0.5f;

	// both ways
	eC *= float4( eL.y, eR.x, eT.w, eB.z );
	float wTL = diag * ( eC.x * eL.z + eC.z * eT.x );
	float wTR = diag * ( eC.z * eT.y + eC.y * eR.z );
	float wBL = diag * ( eC.w * eB.x + eC.x * eL.w );
	float wBR = diag * ( eC.y * eR.w + eC.w * eB.y );
	float center = u.ao2.w * ( final ? 1.0f : 0.2f ) * ( 1.0f + 0.25f * max( frames - 1.0f, 0.0f ) );
	float sum = v[4] * center + v[3] * eC.x + v[5] * eC.y + v[1] * eC.z + v[7] * eC.w
		+ v[0] * wTL + v[2] * wTR + v[6] * wBL + v[8] * wBR;

	return sum / ( center + dot( eC, float4( 1.0f ) ) + wTL + wTR + wBL + wBR );
}

// the grid's texels of the main view
static int2 MB_AoGridLo( constant MbAoUniforms &u )
{
	return u.rect.xy >> 1;
}

static int2 MB_AoGridHi( constant MbAoUniforms &u )
{
	return ( u.rect.xy + u.rect.zw - 1 ) >> 1;
}

/*
** mb_ao_prefilter_cs
**
** Level 0 the depth target's view distances, per pixel; levels 1 to 4 each texel a weighted mean of the 2x2 below it
** (of 1 / d), as XeGTAO's prefilter: the nearer values lose weight past the radius, so a coarse level keeps the surface
** behind a thin one instead of a depth that is neither; the view weapon (negative) is averaged only where a texel has
** nothing else. The horizon search reads the coarse levels far from the texel. 8x8 threads a threadgroup, 2x2 pixels
** each: 16x16 pixels, down to one texel of level 4. And the grid's view distances: a texel a 2x2, its top-left one's.
*/
static float MB_AoFilter( constant MbAoUniforms &u, float4 d )
{
	bool4 weapon = d < 0.0f;
	float radius = 0.75f * u.ao.x;
	float range = max( u.ao.y * radius, 1e-4f );
	float mul = -1.0f / range;
	float add = radius * ( 1.0f - u.ao.y ) / range + 1.0f;

	if ( all( weapon ) ) {
		d = -d;
	} else if ( any( weapon ) ) {
		d = select( d, float4( -1e30f ), weapon );		// the world only
	}
	float maxD = max( max( d.x, d.y ), max( d.z, d.w ) );
	float4 w = saturate( ( maxD - d ) * mul + add );

	w = select( w, float4( 0.0f ), d < -1e29f );
	// the weighted mean of 1 / d (linear across a plane in screen space, d isn't): a plane's texel is the plane at the
	// texel's center, which the main pass samples it at; the mean of d bent a wall close to the eye toward it
	float r = dot( w, float4( 1.0f ) ) / max( dot( w, 1.0f / max( d, float4( 1e-4f ) ) ), 1e-12f );

	return all( weapon ) ? -r : r;
}

kernel void mb_ao_prefilter_cs( depth2d<float> depth [[texture(0)]],
	texture2d<half, access::write> dist [[texture(1)]],
	texture2d<float, access::write> level1 [[texture(2)]],
	texture2d<float, access::write> level2 [[texture(3)]],
	texture2d<float, access::write> level3 [[texture(4)]],
	texture2d<float, access::write> level4 [[texture(5)]],
	texture2d<float, access::write> grid [[texture(6)]],
	constant MbAoUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]],
	uint2 lid [[thread_position_in_threadgroup]],
	uint2 group [[threadgroup_position_in_grid]] )
{
	constexpr sampler nearest( filter::nearest, address::clamp_to_edge );
	threadgroup float s_level[8 * 8];
	int2 size = int2( depth.get_width(), depth.get_height() );
	int2 local = int2( lid );
	int2 t = int2( gid );		// the texel of level 1
	int2 p = t * 2;
	float4 d;
	float m;

	// the 2x2 pixels (0, 0), (1, 0), (0, 1), (1, 1) from a gather at their shared corner (its order (0, 1), (1, 1),
	// (1, 0), (0, 0)), the edges repeated
	float4 wz = depth.gather( nearest, float2( p + 1 ) / float2( size ) );

	d.x = MB_AoDistance( u, wz.w );
	d.y = MB_AoDistance( u, wz.z );
	d.z = MB_AoDistance( u, wz.x );
	d.w = MB_AoDistance( u, wz.y );
	if ( all( t < int2( grid.get_width(), grid.get_height() ) ) ) {
		grid.write( float4( d.x ), uint2( t ) );
	}
	if ( p.x < size.x && p.y < size.y ) {
		dist.write( half4( d.x ), uint2( p ) );
	}
	if ( p.x + 1 < size.x && p.y < size.y ) {
		dist.write( half4( d.y ), uint2( p + int2( 1, 0 ) ) );
	}
	if ( p.x < size.x && p.y + 1 < size.y ) {
		dist.write( half4( d.z ), uint2( p + int2( 0, 1 ) ) );
	}
	if ( p.x + 1 < size.x && p.y + 1 < size.y ) {
		dist.write( half4( d.w ), uint2( p + int2( 1, 1 ) ) );
	}

	m = MB_AoFilter( u, d );
	if ( all( t < max( size >> 1, int2( 1 ) ) ) ) {
		level1.write( float4( m ), uint2( t ) );
	}
	s_level[local.y * 8 + local.x] = m;
	threadgroup_barrier( mem_flags::mem_threadgroup );

	// levels 2 to 4: 4x4, 2x2, 1 texel of the threadgroup
	for ( int i = 2; i <= MB_AO_LEVELS - 1; i++ ) {
		int n = 8 >> ( i - 1 );		// the level's texels in the threadgroup, a side
		int step = 1 << ( i - 2 );	// in s_level, between the level before's texels
		bool inLevel = all( local < int2( n ) );

		if ( inLevel ) {
			int2 s = local * 2 * step;
			float4 q = float4( s_level[s.y * 8 + s.x], s_level[s.y * 8 + s.x + step],
				s_level[( s.y + step ) * 8 + s.x], s_level[( s.y + step ) * 8 + s.x + step] );
			int2 texel = int2( group ) * n + local;
			int2 levelSize = max( size >> i, int2( 1 ) );

			m = MB_AoFilter( u, q );
			if ( all( texel < levelSize ) ) {
				if ( i == 2 ) {
					level2.write( float4( m ), uint2( texel ) );
				} else if ( i == 3 ) {
					level3.write( float4( m ), uint2( texel ) );
				} else {
					level4.write( float4( m ), uint2( texel ) );
				}
			}
		}
		threadgroup_barrier( mem_flags::mem_threadgroup );
		if ( inLevel ) {
			int2 s = local * 2 * step;

			s_level[s.y * 8 + s.x] = m;
		}
		threadgroup_barrier( mem_flags::mem_threadgroup );
	}
}

/*
** mb_ao_main_cs
**
** For each of the slices (planes through the view vector, rotated by a noise that changes every texel and every frame)
** the highest horizon on both sides within the radius, from the steps' depth samples a side spread toward the texel
** (squared), coarser levels of the view distances farther away; then the visible part of the hemisphere above the
** normal projected into the slice, cosine weighted, analytically. Samples farther than the radius fade to the lowest
** horizon (the falloff), and a thin occluder compensation lets them fade sooner along the view axis. A thread a texel of
** the grid: its pixel, the view distances of the full resolution, into the grid. The slices and the steps (r_aoQuality)
** are function constants, so the loops unroll and a slice's reads go out together.
*/
constant int MB_AO_SLICES [[function_constant(0)]];
constant int MB_AO_STEPS [[function_constant(1)]];

constant float MB_AO_LEVEL_OFFSET = 3.3f;		// log2 of the sample's distance in pixels, less this: its level
constant float MB_AO_PIXEL_TOO_CLOSE = 1.3f;	// pixels: the first sample at least this far

// acos, a few ULP off: the pass spends most of its arithmetic here
static float MB_FastAcos( float x )
{
	float a = abs( x );
	float r = -0.156583f * a + MB_HALF_PI;

	r *= sqrt( max( 1.0f - a, 0.0f ) );
	return x >= 0.0f ? r : MB_PI - r;
}

// XeGTAO's spatio-temporal noise: the texel's index on a 64x64 Hilbert curve, then the R2 sequence (so neighbouring
// texels' slices differ the most), shifted with the frame
static uint MB_HilbertIndex( uint x, uint y )
{
	uint index = 0u;

	for ( uint level = 32u; level > 0u; level >>= 1u ) {
		uint rx = ( x & level ) > 0u ? 1u : 0u;
		uint ry = ( y & level ) > 0u ? 1u : 0u;

		index += level * level * ( ( 3u * rx ) ^ ry );
		if ( ry == 0u ) {
			if ( rx == 1u ) {
				x = 63u - x;
				y = 63u - y;
			}
			uint tmp = x;
			x = y;
			y = tmp;
		}
	}
	return index;
}

static float2 MB_AoNoise( constant MbAoUniforms &u, int2 p )
{
	uint index = MB_HilbertIndex( uint( p.x ) & 63u, uint( p.y ) & 63u ) + 288u * ( uint( u.ints.z ) & 63u );

	return fract( 0.5f + float( index ) * float2( 0.75487766624669276005f, 0.5698402909980532659114f ) );
}

// level 0 from the depth target itself (as the prefilter makes it: the same bytes read as a copy, none written)
static float MB_AoFetch( constant MbAoUniforms &u, depth2d<float, access::read> depth, texture2d<float, access::read> levels,
	int2 texel, int level )
{
	return level == 0 ? MB_AoDistance( u, depth.read( uint2( texel ) ) ) : levels.read( uint2( texel ), uint( level - 1 ) ).r;
}

kernel void mb_ao_main_cs( depth2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> levels [[texture(1)]],
	texture2d<float, access::write> raw [[texture(2)]],
	constant MbAoUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]] )
{
	int2 lo = u.rect.xy;
	int2 hi = u.rect.xy + u.rect.zw - 1;
	int2 grid = MB_AoGridLo( u ) + int2( gid );		// the grid's texel
	int2 p = clamp( grid * 2, lo, hi );				// its pixel
	int2 size = int2( depth.get_width(), depth.get_height() );
	float2 pixel = float2( p ) + 0.5f;

	if ( any( grid > MB_AoGridHi( u ) ) ) {
		return;
	}

	// the pixel and its 4 neighbours (the target's edges repeated)
	float sd = MB_AoDistance( u, depth.read( uint2( p ) ) );
	float zl = MB_AoDistance( u, depth.read( uint2( max( p.x - 1, 0 ), p.y ) ) );
	float zr = MB_AoDistance( u, depth.read( uint2( min( p.x + 1, size.x - 1 ), p.y ) ) );
	float zt = MB_AoDistance( u, depth.read( uint2( p.x, max( p.y - 1, 0 ) ) ) );
	float zb = MB_AoDistance( u, depth.read( uint2( p.x, min( p.y + 1, size.y - 1 ) ) ) );
	bool weapon = sd < 0.0f;
	float z = abs( sd );

	if ( MB_AoIsFar( u, z ) ) {
		raw.write( float4( 1.0f ), uint2( grid ) );
		return;
	}

	// MB_AoViewPosition as x = ( pixel * mul + add ) * d: the same for every sample of the texel, without its divisions
	float2 posMul = float2( 2.0f / ( float( u.rect.z ) * u.proj.x ), -2.0f / ( float( u.rect.w ) * u.proj.y ) );
	float2 posAdd = float2( ( -2.0f * float( u.rect.x ) / float( u.rect.z ) - 1.0f - u.view.x + u.proj.z ) / u.proj.x,
		( 2.0f * float( u.rect.y ) / float( u.rect.w ) + 1.0f - u.view.y + u.proj.w ) / u.proj.y );
	float3 pos = float3( ( pixel * posMul + posAdd ) * z, -z );
	float3 viewVec = normalize( -pos );
	// the normal of the depth (the horizons are searched in it): of the neighbours the nearer in depth, so the
	// weapon's edge has its own
	float3 normal;
	{
		float al = abs( zl ), ar = abs( zr ), at = abs( zt ), ab = abs( zb );
		float3 ex = abs( al - z ) < abs( ar - z ) ? pos - float3( ( ( pixel - float2( 1.0f, 0.0f ) ) * posMul + posAdd ) * al, -al )
			: float3( ( ( pixel + float2( 1.0f, 0.0f ) ) * posMul + posAdd ) * ar, -ar ) - pos;
		float3 ey = abs( at - z ) < abs( ab - z ) ? pos - float3( ( ( pixel - float2( 0.0f, 1.0f ) ) * posMul + posAdd ) * at, -at )
			: float3( ( ( pixel + float2( 0.0f, 1.0f ) ) * posMul + posAdd ) * ab, -ab ) - pos;
		float3 n = cross( ey, ex );
		float len = dot( n, n );

		normal = len < 1e-20f ? float3( 0.0f, 0.0f, 1.0f ) : n * rsqrt( len );
		normal = dot( normal, pos ) > 0.0f ? -normal : normal;
	}
	// a normal facing away from the eye (across a silhouette) would see nothing
	if ( dot( normal, viewVec ) < 0.0f ) {
		normal = normalize( normal - viewVec * ( dot( normal, viewVec ) - 0.01f ) );
	}

	float radius = u.ao.x;
	float falloffRange = max( u.ao.y * radius, 1e-4f );
	float falloffMul = -1.0f / falloffRange;
	float falloffAdd = radius * ( 1.0f - u.ao.y ) / falloffRange + 1.0f;
	float thin = u.ao.z;

	// the radius in pixels, at the texel's distance
	float pixelSize = 2.0f * z / ( u.proj.x * float( u.rect.z ) );
	float screenRadius = radius / pixelSize;
	float minS = MB_AO_PIXEL_TOO_CLOSE / screenRadius;
	float2 noise = MB_AoNoise( u, grid );
	float visibility = 0.0f;

	// less occlusion when the radius covers only a few pixels (far away): too few samples to tell
	visibility += saturate( ( 10.0f - screenRadius ) / 100.0f ) * 0.5f;

	// the slices' angles ( slice + noise ) / slices * pi: the first one's cosine and sine, then a rotation each
	float phi0 = noise.x / float( MB_AO_SLICES ) * MB_PI;
	float2 cs = float2( cos( phi0 ), sin( phi0 ) );
	const float2 rot = float2( cos( MB_PI / float( MB_AO_SLICES ) ), sin( MB_PI / float( MB_AO_SLICES ) ) );

	for ( int slice = 0; slice < MB_AO_SLICES; slice++ ) {
		float cosPhi = cs.x, sinPhi = cs.y;

		cs = float2( cs.x * rot.x - cs.y * rot.y, cs.x * rot.y + cs.y * rot.x );
		// the target's y goes down, eye space y up
		float2 omega = float2( cosPhi, -sinPhi ) * screenRadius;
		float3 dirVec = float3( cosPhi, sinPhi, 0.0f );
		float3 orthoDir = dirVec - dot( dirVec, viewVec ) * viewVec;
		float3 axis = normalize( cross( orthoDir, viewVec ) );
		float3 projNormal = normal - axis * dot( normal, axis );
		float projLen = length( projNormal );
		float signN = sign( dot( orthoDir, projNormal ) );
		float cosN = saturate( dot( projNormal, viewVec ) / max( projLen, 1e-6f ) );
		float n = signN * MB_FastAcos( cosN );
		float sinN = signN * sqrt( max( 1.0f - cosN * cosN, 0.0f ) );	// sin( n )
		float lowCos0 = -sinN;		// cos( n + pi / 2 )
		float lowCos1 = sinN;		// cos( n - pi / 2 )
		float horizonCos0 = lowCos0;
		float horizonCos1 = lowCos1;

		for ( int step = 0; step < MB_AO_STEPS; step++ ) {
			float stepNoise = fract( noise.y + float( slice + step * MB_AO_STEPS ) * 0.6180339887498948f );
			float s = ( float( step ) + stepNoise ) / float( MB_AO_STEPS );

			s = s * s + minS;		// XeGTAO's sample power 2
			float2 offset = s * omega;
			float level = clamp( 0.5f * log2( dot( offset, offset ) ) - MB_AO_LEVEL_OFFSET, 0.0f, float( MB_AO_LEVELS - 1 ) );
			int l = int( level + 0.5f );

			offset = round( offset );
			for ( int side = 0; side < 2; side++ ) {
				float2 sp = pixel + ( side == 0 ? offset : -offset );

				// off the view: nothing known there (clamped to its edge the sample left the slice, and close to a wall,
				// the radius thousands of pixels, it went dark)
				if ( any( sp < float2( lo ) ) || any( sp >= float2( hi + 1 ) ) ) {
					continue;
				}
				// a texel of a coarse level is its pixels' surface at its center (the prefilter): the sample is there
				int2 texel = min( int2( sp ) >> l, max( size >> l, int2( 1 ) ) - 1 );
				float sdist = MB_AoFetch( u, depth, levels, texel, l );

				sp = l == 0 ? sp : ( float2( texel ) + 0.5f ) * float( 1 << l );
				// the weapon and the world don't occlude each other
				if ( ( sdist < 0.0f ) != weapon ) {
					continue;
				}
				float sz = abs( sdist );
				float3 delta = float3( ( sp * posMul + posAdd ) * sz, -sz ) - pos;
				// the thin occluder compensation: what is in front along the view axis fades out sooner
				float falloffBase = length( float3( delta.xy, delta.z * ( 1.0f + thin ) ) );
				float weight = saturate( falloffBase * falloffMul + falloffAdd );
				float shc = dot( delta, viewVec ) * rsqrt( max( dot( delta, delta ), 1e-12f ) );

				if ( side == 0 ) {
					horizonCos0 = max( horizonCos0, mix( lowCos0, shc, weight ) );
				} else {
					horizonCos1 = max( horizonCos1, mix( lowCos1, shc, weight ) );
				}
			}
		}

		// the visible arc on both sides of the normal, cosine weighted
		projLen = mix( projLen, 1.0f, 0.05f );
		float h0 = -MB_FastAcos( horizonCos1 );
		float h1 = MB_FastAcos( horizonCos0 );

		h0 = n + clamp( h0 - n, -MB_HALF_PI, MB_HALF_PI );
		h1 = n + clamp( h1 - n, -MB_HALF_PI, MB_HALF_PI );
		float iarc0 = ( cosN + 2.0f * h0 * sinN - cos( 2.0f * h0 - n ) ) / 4.0f;
		float iarc1 = ( cosN + 2.0f * h1 * sinN - cos( 2.0f * h1 - n ) ) / 4.0f;

		visibility += projLen * ( iarc0 + iarc1 );
	}
	visibility = clamp( visibility / float( MB_AO_SLICES ), 0.03f, 1.0f );

	raw.write( float4( visibility ), uint2( grid ) );
}

/*
** mb_ao_temporal_cs
**
** The occlusion blended over frames and blurred, one pass: the main pass's noise changes every frame, so the mean of
** the last frames converges where the surface stays.
**  - the last frame's result moved by the texel's motion (the motion vectors with the anti-aliasing; else the camera's,
**    from the view distance; the view weapon's none), read bilinear among the 2x2 texels whose last view distance is
**    the surface's (no history from what was in front or behind; the texels picked here, not by a gather: a point a
**    thousandth of a pixel before a texel's center came back as the texels one further, and the history moved a pixel
**    a frame), clamped to the range of this frame's 3x3 (a shadow that moved away leaves no trail). Each texel counts
**    its frames: the first ones weigh as much as the rest together (1 / n), up to the most frames (a quarter of it for
**    what moves). Written to the history before the blur: the blur doesn't pile up over frames
**  - then the blur (MB_AoBlur) into the result, or into the first blur's target for the second blur (ints.w)
** 16x16 texels a threadgroup. The blur needs the blended values of the texels around its own: the threadgroup loads
** the raw occlusion and the view distances of its texels and 2 more around them (20x20), blends 18x18 (the ring around
** twice, in the neighbouring threadgroups too), then blurs its 16x16 from there: the same values as a pass each,
** without writing the blend out and reading it back.
*/
#define MB_AO_GROUP		16
#define MB_AO_RT		( MB_AO_GROUP + 4 )		// the raw occlusion's and the view distances' tile
#define MB_AO_HT		( MB_AO_GROUP + 2 )		// the blended tile

kernel void mb_ao_temporal_cs( texture2d<float, access::read> raw [[texture(0)]],
	texture2d<float, access::read> grid [[texture(1)]],
	texture2d<float, access::read> prevGrid [[texture(2)]],
	texture2d<float, access::read> history [[texture(3)]],
	texture2d<float, access::write> outHistory [[texture(4)]],
	texture2d<float, access::write> outBlur [[texture(5)]],
	texture2d<float, access::read> velocity [[texture(6)]],
	constant MbAoUniforms &u [[buffer(0)]],
	uint2 group [[threadgroup_position_in_grid]],
	uint2 lid [[thread_position_in_threadgroup]],
	uint index [[thread_index_in_threadgroup]] )
{
	threadgroup float s_raw[MB_AO_RT * MB_AO_RT];
	threadgroup float s_z[MB_AO_RT * MB_AO_RT];
	threadgroup float s_vis[MB_AO_HT * MB_AO_HT];
	threadgroup float s_frames[MB_AO_HT * MB_AO_HT];
	int2 lo = MB_AoGridLo( u );		// the grid's texels
	int2 hi = MB_AoGridHi( u );
	int2 plo = u.rect.xy;			// the view's pixels
	int2 phi = u.rect.xy + u.rect.zw - 1;
	int2 origin = lo + int2( group ) * MB_AO_GROUP;
	int2 local = int2( lid );
	int2 p = origin + local;
	int2 maxT = int2( grid.get_width(), grid.get_height() ) - 1;
	uint i;

	// the raw occlusion and the view distances, the view's edges repeated
	for ( i = index; i < MB_AO_RT * MB_AO_RT; i += MB_AO_GROUP * MB_AO_GROUP ) {
		uint2 q = uint2( clamp( origin - 2 + int2( i % MB_AO_RT, i / MB_AO_RT ), lo, hi ) );

		s_raw[i] = raw.read( q ).r;
		s_z[i] = grid.read( q ).r;
	}
	threadgroup_barrier( mem_flags::mem_threadgroup );

	// blended: the threadgroup's texels and the ring around them (a texel off the view as its nearest one in it); the
	// threadgroup's own to the history
	for ( i = index; i < MB_AO_HT * MB_AO_HT; i += MB_AO_GROUP * MB_AO_GROUP ) {
		int2 h = int2( i % MB_AO_HT, i / MB_AO_HT );
		int2 q = clamp( origin - 1 + h, lo, hi );
		int2 r = q - ( origin - 2 );		// in the raw tile
		float cur = s_raw[r.y * MB_AO_RT + r.x];
		float sd = s_z[r.y * MB_AO_RT + r.x];
		float d = abs( sd );
		bool weapon = sd < 0.0f;
		float vmin = 1.0f, vmax = 0.0f;
		float2 b = float2( cur, 1.0f );

		for ( int y = -1; y <= 1; y++ ) {
			for ( int x = -1; x <= 1; x++ ) {
				float v = s_raw[( r.y + y ) * MB_AO_RT + r.x + x];

				vmin = min( vmin, v );
				vmax = max( vmax, v );
			}
		}

		if ( !MB_AoIsFar( u, d ) && u.view.z != 0.0f ) {
			// the texel's motion in units of the view (this frame's position less the last one's, x right, y down), the
			// view distance its surface had in the last frame, and whether it moves (of itself, not with the camera)
			float2 pixel = float2( clamp( q * 2, plo, phi ) ) + 0.5f;
			float2 cam = float2( 0.0f );
			float expected = d;
			bool valid = true, moves = false;

			if ( !weapon ) {
				float4 prevClip = u.reproject * float4( MB_AoViewPosition( u, pixel, d ), 1.0f );

				if ( prevClip.w <= 1e-6f ) {
					valid = false;		// behind the camera
				} else {
					cam = ( MB_AoPixelNdc( u, pixel ) - prevClip.xy / prevClip.w ) * float2( 0.5f, -0.5f );
					expected = prevClip.w;
				}
			}
			float2 motion = cam;

			if ( u.view.w != 0.0f ) {
				// every opaque draw wrote its motion: the camera's, or its own where it moves
				float2 v = velocity.read( uint2( pixel ) ).xy;

				moves = length( ( v - cam ) * float2( u.rect.zw ) ) > 0.125f;
				if ( moves ) {
					motion = v;
					expected = d;
					valid = true;
				}
			}
			float2 prevPixel = pixel - motion * float2( u.rect.zw );

			if ( valid && all( prevPixel >= float2( plo ) ) && all( prevPixel <= float2( phi + 1 ) ) ) {
				// the 2x2 texels around the point, picked here: (0, 1), (1, 1), (1, 0), (0, 0)
				float2 t = ( prevPixel - 0.5f ) * 0.5f;
				int2 b0 = int2( floor( t ) );
				float2 f = t - float2( b0 );
				int2 q00 = clamp( b0, int2( 0 ), maxT ), q11 = clamp( b0 + 1, int2( 0 ), maxT );
				int2 q01 = int2( q00.x, q11.y ), q10 = int2( q11.x, q00.y );
				float4 bilinear = float4( ( 1.0f - f.x ) * f.y, f.x * f.y, f.x * ( 1.0f - f.y ), ( 1.0f - f.x ) * ( 1.0f - f.y ) );
				float4 prevD = float4( prevGrid.read( uint2( q01 ) ).r, prevGrid.read( uint2( q11 ) ).r,
					prevGrid.read( uint2( q10 ) ).r, prevGrid.read( uint2( q00 ) ).r );
				float2 h01 = history.read( uint2( q01 ) ).rg, h11 = history.read( uint2( q11 ) ).rg;
				float2 h10 = history.read( uint2( q10 ) ).rg, h00 = history.read( uint2( q00 ) ).rg;
				float4 histV = float4( h01.x, h11.x, h10.x, h00.x );
				float4 histN = float4( h01.y, h11.y, h10.y, h00.y );
				float tolerance = ( moves ? 0.1f : 0.03f ) * expected + 1.0f;
				float4 match;

				for ( int k = 0; k < 4; k++ ) {
					float pd = prevD[k];

					match[k] = ( pd < 0.0f ) == weapon && abs( abs( pd ) - expected ) <= tolerance ? 1.0f : 0.0f;
				}
				float4 w = bilinear * match;
				float sum = dot( w, float4( 1.0f ) );

				if ( sum >= 1e-3f ) {
					float hv = dot( w, histV ) / sum;
					float frames = dot( w, histN ) / sum;
					float maxFrames = moves ? max( u.ao2.z * 0.25f, 2.0f ) : u.ao2.z;

					// what this frame doesn't have around the texel: not from here
					hv = clamp( hv, vmin - 0.05f, vmax + 0.05f );
					frames = min( frames + 1.0f, maxFrames );
					b = float2( mix( hv, cur, 1.0f / frames ), frames );
				}
			}
		}
		s_vis[i] = b.x;
		s_frames[i] = b.y;
		if ( all( h >= int2( 1 ) ) && all( h <= int2( MB_AO_GROUP ) ) && all( origin - 1 + h <= hi ) ) {
			outHistory.write( float4( b, 0.0f, 0.0f ), uint2( origin - 1 + h ) );
		}
	}
	threadgroup_barrier( mem_flags::mem_threadgroup );

	if ( any( p > hi ) ) {
		return;
	}

	// the blur from the tiles
	int2 hc = local + 1;		// the texel in the blended tile
	int2 rc = local + 2;		// and in the raw one
	float v[9], z[13];

	for ( int y = -1; y <= 1; y++ ) {
		for ( int x = -1; x <= 1; x++ ) {
			v[( y + 1 ) * 3 + x + 1] = s_vis[( hc.y + y ) * MB_AO_HT + hc.x + x];
			z[( y + 1 ) * 3 + x + 1] = s_z[( rc.y + y ) * MB_AO_RT + rc.x + x];
		}
	}
	z[9] = s_z[rc.y * MB_AO_RT + rc.x - 2];
	z[10] = s_z[rc.y * MB_AO_RT + rc.x + 2];
	z[11] = s_z[( rc.y - 2 ) * MB_AO_RT + rc.x];
	z[12] = s_z[( rc.y + 2 ) * MB_AO_RT + rc.x];

	float frames = s_frames[hc.y * MB_AO_HT + hc.x];
	float blurred = MB_AoBlur( u, v, z, frames, u.ints.w == MB_AO_PASS_HISTORY_TO_RESULT );

	outBlur.write( float4( blurred, frames, 0.0f, 0.0f ), uint2( p ) );
}

/*
** mb_ao_denoise_cs
**
** The second blur (quality 3, and without history: the first frames after a cut are the noisiest): the temporal pass's
** lighter first one blurred again into the result. 8x8 texels a threadgroup.
*/
kernel void mb_ao_denoise_cs( texture2d<float, access::read> blur [[texture(0)]],
	texture2d<float, access::read> grid [[texture(1)]],
	texture2d<float, access::write> result [[texture(2)]],
	constant MbAoUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]] )
{
	int2 lo = MB_AoGridLo( u );
	int2 hi = MB_AoGridHi( u );
	int2 p = lo + int2( gid );
	float v[9], z[13];

	if ( any( p > hi ) ) {
		return;
	}
	for ( int y = -1; y <= 1; y++ ) {
		for ( int x = -1; x <= 1; x++ ) {
			uint2 q = uint2( clamp( p + int2( x, y ), lo, hi ) );

			v[( y + 1 ) * 3 + x + 1] = blur.read( q ).r;
			z[( y + 1 ) * 3 + x + 1] = grid.read( q ).r;
		}
	}
	z[9] = grid.read( uint2( clamp( p - int2( 2, 0 ), lo, hi ) ) ).r;
	z[10] = grid.read( uint2( clamp( p + int2( 2, 0 ), lo, hi ) ) ).r;
	z[11] = grid.read( uint2( clamp( p - int2( 0, 2 ), lo, hi ) ) ).r;
	z[12] = grid.read( uint2( clamp( p + int2( 0, 2 ), lo, hi ) ) ).r;

	float frames = blur.read( uint2( p ) ).g;

	result.write( float4( MB_AoBlur( u, v, z, frames, true ) ), uint2( p ) );
}

/*
** mb_ao_apply_fs
**
** The first draw of the scene's pass after the occlusion, over the main view (its scissor), blended ONE, SRC_ALPHA
** into the color only: dst a + rgb. Each pixel's visibility from the 2x2 texels of the grid around it, bilinear
** weights times how close each texel's view distance is to the pixel's own, so the occlusion of a surface doesn't
** spread over the one in front of it or behind; where no texel is of the pixel's surface, the nearest in view distance.
** The pixel's view distance is the prefilter's level 0 (the depth target is this pass's attachment again). Then the
** result (MB_AoResult). Nothing to change (most of the view): no blend, no write. r_temporalShow 3: the factor alone.
*/
struct MbAoFragmentIn
{
	float4 position [[position]];
};

fragment float4 mb_ao_apply_fs( MbAoFragmentIn in [[stage_in]],
	texture2d<half, access::read> dist [[texture(0)]],
	texture2d<float> grid [[texture(1)]],
	texture2d<float> result [[texture(2)]],
	constant MbAoUniforms &u [[buffer(0)]] )
{
	constexpr sampler nearest( filter::nearest, address::clamp_to_edge );
	int2 p = int2( in.position.xy );
	float sd = float( dist.read( uint2( p ) ).r );
	float d = abs( sd );
	bool weapon = sd < 0.0f;
	bool show = u.fogParams.z != 0.0f;

	// (the far distance in half floats: 0.999 of it is two of their steps below)
	if ( MB_AoIsFar( u, d ) ) {
		if ( show ) {
			return float4( 1.0f, 1.0f, 1.0f, 0.0f );
		}
		discard_fragment();
		return float4( 0.0f );
	}

	// the 2x2 texels around the pixel (a texel is at its top-left pixel: the pixel itself, or between two or four of
	// them), each a gather at their shared corner: (0, 1), (1, 1), (1, 0), (0, 0) (nine reads a pixel took 0.19 ms of
	// GPU at 1920x1080, the reads' rate)
	int2 b = p >> 1;
	float2 f = float2( p & 1 ) * 0.5f;
	float2 corner = float2( b + 1 ) / float2( grid.get_width(), grid.get_height() );
	float4 td = grid.gather( nearest, corner );
	float4 v = result.gather( nearest, corner );
	float4 bilinear = float4( ( 1.0f - f.x ) * f.y, f.x * f.y, f.x * ( 1.0f - f.y ), ( 1.0f - f.x ) * ( 1.0f - f.y ) );
	float tolerance = 0.03f * d + 1.0f;
	float4 e = select( abs( abs( td ) - d ), float4( 3.0e38f ), ( td < 0.0f ) != weapon );
	float4 w = bilinear * max( 1.0f - e / tolerance, 0.0f );
	float wsum = dot( w, float4( 1.0f ) );
	float vis;

	if ( wsum > 1e-4f ) {
		vis = dot( w, v ) / wsum;
	} else {
		// no texel of the pixel's surface: the nearest in view distance
		float2 m01 = e.x < e.y ? float2( e.x, v.x ) : float2( e.y, v.y );
		float2 m23 = e.z < e.w ? float2( e.z, v.z ) : float2( e.w, v.w );

		vis = m01.x < m23.x ? m01.y : m23.y;
	}

	float4 ao = MB_AoResult( u, vis, sd );

	if ( show ) {
		return float4( ao.aaa, 0.0f );
	}
	if ( ao.a >= 254.5f / 255.0f ) {
		discard_fragment();
	}
	return ao;
}
