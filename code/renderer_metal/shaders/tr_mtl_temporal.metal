// tr_mtl_temporal.metal -- the temporal path's effects (r_temporal; tr_temporal.c, mbTemporalEndScene in tr_mtl.m),
// drawn when the 3D view's scene ends, before the 2D. Ideas from JayRewd's Vulkan renderer (renderervk's
// taa_resolve.comp, motion_blur.comp, dof.comp and temporal_composite.frag), written for Metal: the scene's targets stay
// render targets, never shader-written (only the history's, the depth of field's and the motion blur's targets are).
//
// mb_taa_cs (r_taa): a compute pass over the 3D view into the history's target. The scene was drawn with its projection
// moved by a different sub-pixel offset each frame (the jitter, tr_temporal.c); each pixel blends this frame's color
// at its center with the last frame's result where the pixel's surface was then:
//  - this frame's color at the center: the 3x3 pixels around it, weighted by their samples' distance to the center
//    (a Gaussian; the weights follow from the jitter and are the same for every pixel, MbTaaUniforms.weights)
//  - where the surface was: the motion vector of the nearest surface of the 3x3 (so the edges of what moves carry its
//    motion), from the scene pass's motion vectors (tr_mtl_stage.metal)
//  - the last frame's result there, read with a Catmull-Rom filter (five bilinear taps) and pulled into the box of the
//    colors around the pixel now (YCoCg, the mean +- the deviation, within their min and max): what the last frame
//    shows and this one doesn't (a surface just uncovered, a light gone out) is let go of
//  - the blend: r_taaFeedback of history, less the faster the surface moves on screen (resampling blurs), both sides
//    weighted by 1 / (1 + luma) so that a bright pixel that comes and goes doesn't flicker
// The result is the next frame's history (RGB10A2: ten bits a channel, so the blend's small steps aren't rounded away
// as they would be in eight).
//
// mb_composite_fs: the first draw of the pass the frame continues in (the 2D follows): the result into the scene's
// other target, sharpened (r_taaSharpen: the result less a quarter of its four neighbours, held within their range;
// the history itself stays unsharpened), and outside the 3D view the scene as it was. r_temporalShow 1 shows the
// motion vectors instead, 2 the scene without the anti-aliasing. Without the anti-aliasing (the motion blur alone) the
// result is the scene itself, unsharpened.
//
// The motion blur (r_motionBlur; McGuire, Hennessy, Bukowski, Osman, "A Reconstruction Filter for Plausible Motion
// Blur", 2012, as Jimenez refined it for Call of Duty: Advanced Warfare, "Next Generation Post Processing in Call of
// Duty", 2014; JayRewd's Vulkan renderer's passes):
//  - mb_motion_tiles_cs: a threadgroup a tile of K x K pixels (K the longest half blur, a share of the target's
//    height): each pixel's blur, the motion of the exposure (half of it: the blur reaches as far on each side of the
//    pixel) in pixels, no longer than K, and apart its length and its view distance (what the gather's samples read:
//    half the bytes); the longest of the tile. The motion: the pixel's motion vector (the scene's draws write the
//    whole motion, the camera's and their own), less the camera's part that r_motionBlurCamera leaves out (the
//    camera's motion from the depth); the view weapon only its own (recoil, bob: its vectors are relative to the
//    view), its distance scaled far down, so that it is in front of everything and keeps its own order
//  - mb_motion_neighbor_cs: a thread a tile, the longest of the 3x3 tiles around it, which reaches every pixel a blur
//    of K or less can cover
//  - mb_composite_motion_fs: the composite (as mb_composite_fs) and then, where the longest blur around the pixel's
//    tile is half a pixel or more, pairs of samples on both sides of the pixel along it (every other pair along the
//    pixel's own blur when that goes another way), at offsets moved by a noise fixed per pixel; a sample adds its
//    color where it is behind the pixel and the pixel's blur reaches it (what the motion uncovers), or in front of it
//    and its own blur reaches the pixel (what moves over it), the depths compared softly; the mirror of Jimenez's for
//    a pair. What the samples don't cover is the pixel's own color. Summed in near-linear light (squared: the game's
//    colors are gamma 2.2, a square the cheap stand-in). The anti-aliasing's history stays unblurred. r_temporalShow
//    4 shows each pixel's blur, 5 the longest around its tile (a tile's side the brightest).
//
// The depth of field (r_dof; a thin lens, the gather of Jimenez's "Next Generation Post Processing in Call of Duty:
// Advanced Warfare", 2014, at half the resolution, the background and the foreground apart; JayRewd's Vulkan renderer's
// passes): after the anti-aliasing, from its result (or the scene) and the depth, before the motion blur, which then
// gathers from its result. A pixel's blur, its circle of confusion (CoC), is a radius in pixels of
// lens.x * (1 / F - 1 / d) for its view distance d and the focus distance F, positive behind the focus (the
// background), negative in front of it (the foreground), no more than a tile's side. The view weapon (its depth range
// below the floor) is never blurred.
//  - mb_dof_focus_cs (one threadgroup): the focus, as 1 / F in a 1x1 target, eased toward the nearest quarter of the
//    view distances of 8x8 points around the main view's center (the crosshair; the sky the far distance)
//  - mb_dof_setup_cs (a threadgroup a tile): each half resolution texel's CoC (its nearest pixel's) and color in
//    near-linear light (its four pixels weighted by how close their CoC is to that one); the tile's largest foreground
//    CoC and largest CoC
//  - mb_dof_gather_cs (half resolution): the largest CoC of the 3x3 tiles around the texel's (which reaches every
//    pixel a blur can cover; kept for the passes after it by the tile's first texel), the background over the texel's own CoC (what is behind doesn't spread over
//    what is in front), the foreground over the largest around its tile (each sample its share of the disc it spreads
//    over: its opacity), premultiplied so that the apply pass filters them bilinearly
//  - mb_dof_apply_cs (full resolution, the tiles a blur reaches, or all with the motion blur): the sharp scene, the
//    background over it by the pixel's own CoC, the foreground over that by its opacity, both upsampled with a tent;
//    alpha 1 where the pixel stays as it was (the composite sharpens only those: r_taaSharpen)
//  - mb_dof_blend_fs: the result over the scene where it changed it, in the tiles a blur reaches (without the motion
//    blur, which reads the result itself in its composite)
// Summed in near-linear light, squared as the motion blur's. r_temporalShow 6 shows each pixel's CoC.
//
// Keep the structs in sync with tr_mtl.m.

#include <metal_stdlib>
using namespace metal;

struct MbTemporalVertexOut
{
	float4 position [[position]];
};

// One triangle covering the whole target: (-1,-1), (3,-1), (-1,3).
vertex MbTemporalVertexOut mb_temporal_vs( uint vid [[vertex_id]] )
{
	float2 uv = float2( ( vid << 1 ) & 2, vid & 2 );
	MbTemporalVertexOut out;
	out.position = float4( uv * 2.0f - 1.0f, 0.0f, 1.0f );
	return out;
}

struct MbTaaUniforms
{
	int4	rect;			// the 3D view in target pixels: x, y (from the top left), width, height
	float4	params;			// x: the history's weight (r_taaFeedback), y: 1 when the history is the last frame's, zw: 1 / the target's size
	float4	weights[3];		// [0..8]: the 3x3's weights for this frame's color at the pixel's center, row by row
};

struct MbCompositeUniforms
{
	int4	rect;			// as MbTaaUniforms.rect
	float4	params;			// x: r_taaSharpen, y: r_temporalShow, z: the motion vectors' scale (r_temporalShow 1, r_temporalShowScale)
};

struct MbMotionUniforms
{
	int4		rect;		// the main view in target pixels: x, y (from the top left), width, height
	int4		tiles;		// xy: the tiles across and down (from the target's top left), z: a tile's side K, w: the gather's pairs
	float4		proj;		// the main view's projection's [0], [5], [8], [9] (GL's, without the jitter)
	float4		depth;		// [10], [14], the depth floor (below it the view weapon's range; 0: none), the far distance
	float4		params;		// x: half the exposure in frames of motion, y: the camera's share (r_motionBlurCamera), zw: the jitter in NDC
	float4x4	reproject;	// this frame's eye space (GL's: x right, y up, looking down -z) to the last frame's clip space
};

static float3 MB_ToYCoCg( float3 c )
{
	return float3( 0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b, -0.25f * c.r + 0.5f * c.g - 0.25f * c.b );
}

static float3 MB_FromYCoCg( float3 c )
{
	return float3( c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z );
}

static float MB_Luma( float3 c )
{
	return dot( c, float3( 0.299f, 0.587f, 0.114f ) );
}

// the history at pos (in target pixels): Catmull-Rom over 4x4 texels, as five bilinear taps (its corners left out)
static float3 MB_HistoryAt( texture2d<float> history, float2 pos, float2 invSize )
{
	constexpr sampler bilinear( filter::linear, address::clamp_to_edge );
	float2 t1 = floor( pos - 0.5f ) + 0.5f;
	float2 f = pos - t1;
	float2 w0 = f * ( -0.5f + f * ( 1.0f - 0.5f * f ) );
	float2 w1 = 1.0f + f * f * ( -2.5f + 1.5f * f );
	float2 w2 = f * ( 0.5f + f * ( 2.0f - 1.5f * f ) );
	float2 w3 = f * f * ( -0.5f + 0.5f * f );
	float2 w12 = w1 + w2;
	float2 p0 = ( t1 - 1.0f ) * invSize;
	float2 p3 = ( t1 + 2.0f ) * invSize;
	float2 p12 = ( t1 + w2 / w12 ) * invSize;
	float wa = w12.x * w0.y, wb = w0.x * w12.y, wc = w12.x * w12.y, wd = w3.x * w12.y, we = w12.x * w3.y;
	float3 c = history.sample( bilinear, float2( p12.x, p0.y ) ).rgb * wa
		+ history.sample( bilinear, float2( p0.x, p12.y ) ).rgb * wb
		+ history.sample( bilinear, p12 ).rgb * wc
		+ history.sample( bilinear, float2( p3.x, p12.y ) ).rgb * wd
		+ history.sample( bilinear, float2( p12.x, p3.y ) ).rgb * we;

	return max( c / ( wa + wb + wc + wd + we ), 0.0f );
}

// p moved toward the box's center until it is inside it
static float3 MB_ClipToBox( float3 lo, float3 hi, float3 p )
{
	float3 center = 0.5f * ( hi + lo );
	float3 extent = 0.5f * ( hi - lo ) + 1e-5f;
	float3 r = p - center;
	float3 q = abs( r ) / extent;
	float m = max( q.x, max( q.y, q.z ) );

	return m > 1.0f ? center + r / m : p;
}

// A workgroup loads its pixels' and the ring around them's colors (YCoCg) and depths once, not each of the 3x3 around
// every pixel: as a fragment pass (nine colors and nine depths a pixel) the anti-aliasing took ~0.68 ms of GPU a frame
// at 1920x1080, as this kernel ~0.2 ms with the history and the passes around it ~0.5 (session 110: it is bound by the
// memory it reads and writes, the motion vectors, depth and history at full size)
#define MB_TAA_GROUP	16		// MB_TAA_GROUP in tr_mtl.m
#define MB_TAA_TILE		( MB_TAA_GROUP + 2 )

kernel void mb_taa_cs( texture2d<float, access::read> scene [[texture(0)]],
	texture2d<float, access::read> velocity [[texture(1)]],
	depth2d<float, access::read> depth [[texture(2)]],
	texture2d<float> history [[texture(3)]],
	texture2d<float, access::write> result [[texture(4)]],
	constant MbTaaUniforms &u [[buffer(0)]],
	uint2 group [[threadgroup_position_in_grid]],
	uint2 local [[thread_position_in_threadgroup]],
	uint index [[thread_index_in_threadgroup]] )
{
	threadgroup half4 tileColor[MB_TAA_TILE * MB_TAA_TILE];
	threadgroup float tileDepth[MB_TAA_TILE * MB_TAA_TILE];
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;
	int2 base = lo + int2( group ) * MB_TAA_GROUP - 1;
	int2 p = base + 1 + int2( local );

	// the tile, clamped to the view (outside it the edge repeats)
	for ( uint i = index; i < MB_TAA_TILE * MB_TAA_TILE; i += MB_TAA_GROUP * MB_TAA_GROUP ) {
		uint2 q = uint2( clamp( base + int2( i % MB_TAA_TILE, i / MB_TAA_TILE ), lo, hi ) );

		tileColor[i] = half4( half3( MB_ToYCoCg( scene.read( q ).rgb ) ), 0.0h );
		tileDepth[i] = depth.read( q );
	}
	threadgroup_barrier( mem_flags::mem_threadgroup );
	if ( any( p > hi ) ) {
		return;
	}

	// the colors around the pixel and its nearest surface (depth: 0 near, 1 far; the view weapon nearest of all)
	half3 filtered = half3( 0.0h ), sum = half3( 0.0h ), sum2 = half3( 0.0h );
	half3 cmin = half3( 64.0h ), cmax = half3( -64.0h );
	float nearest = 2.0f;
	int2 nearestPixel = p;

	for ( int y = 0; y < 3; y++ ) {
		for ( int x = 0; x < 3; x++ ) {
			uint k = ( local.y + y ) * MB_TAA_TILE + local.x + x;
			int j = y * 3 + x;
			half3 c = tileColor[k].rgb;
			float d = tileDepth[k];

			filtered += c * half( u.weights[j >> 2][j & 3] );
			sum += c;
			sum2 += c * c;
			cmin = min( cmin, c );
			cmax = max( cmax, c );
			if ( d < nearest ) {
				nearest = d;
				nearestPixel = clamp( p + int2( x - 1, y - 1 ), lo, hi );
			}
		}
	}
	float3 cur = max( MB_FromYCoCg( float3( filtered ) ), 0.0f );

	if ( u.params.y == 0.0f ) {
		result.write( float4( cur, 1.0f ), uint2( p ) );
		return;
	}

	// where the surface was in the last frame
	float2 motion = velocity.read( uint2( nearestPixel ) ).xy;
	float2 prevPos = float2( p ) + 0.5f - motion * float2( u.rect.zw );

	if ( any( prevPos < float2( lo ) ) || any( prevPos > float2( hi + 1 ) ) ) {
		result.write( float4( cur, 1.0f ), uint2( p ) );
		return;
	}

	// the history, pulled into what is around the pixel now; at a texel's center (a still view: the vectors have no
	// jitter) Catmull-Rom's weights are 0, 1, 0, 0, the texel itself
	half3 mean = sum * ( 1.0h / 9.0h );
	half3 sigma = sqrt( max( sum2 * ( 1.0h / 9.0h ) - mean * mean, 0.0h ) );
	float3 boxLo = float3( max( mean - sigma, cmin ) );
	float3 boxHi = float3( min( mean + sigma, cmax ) );
	float2 t = prevPos - 0.5f;
	float2 tr = round( t );
	float3 h = all( abs( t - tr ) < 1.0f / 1024.0f ) ? history.read( uint2( tr ) ).rgb
		: MB_HistoryAt( history, prevPos, u.params.zw );

	h = MB_FromYCoCg( MB_ClipToBox( boxLo, boxHi, MB_ToYCoCg( h ) ) );

	// less history the faster it moves
	float speed = length( motion * float2( u.rect.zw ) );
	float feedback = u.params.x * ( 1.0f - clamp( speed / 64.0f, 0.0f, 0.25f ) );
	float wc = ( 1.0f - feedback ) / ( 1.0f + MB_Luma( cur ) );
	float wh = feedback / ( 1.0f + MB_Luma( h ) );

	result.write( float4( max( ( cur * wc + h * wh ) / ( wc + wh ), 0.0f ), 1.0f ), uint2( p ) );
}

// the motion in pixels times the scale, 32 pixels a frame the most shown at 1: right red, down green, none grey
static float4 MB_ShowVelocity( texture2d<float, access::read> velocity, int2 p, constant MbCompositeUniforms &u )
{
	float2 v = velocity.read( uint2( p ) ).xy * float2( u.rect.zw ) * ( u.params.z / 64.0f );

	return float4( saturate( 0.5f + v.x ), saturate( 0.5f + v.y ), 0.5f, 1.0f );
}

// the result at p, sharpened by sharpen times its alpha (0: as it is; the history's alpha is 1, the depth of field's
// result's 0 where it blurred the pixel)
static float3 MB_CompositeColor( texture2d<float, access::read> result, int2 p, int2 lo, int2 hi, float sharpen )
{
	float4 c4 = result.read( uint2( p ) );
	float3 c = c4.rgb;

	sharpen *= c4.a;

	if ( sharpen > 0.0f ) {
		float3 l = result.read( uint2( max( p.x - 1, lo.x ), p.y ) ).rgb;
		float3 r = result.read( uint2( min( p.x + 1, hi.x ), p.y ) ).rgb;
		float3 t = result.read( uint2( p.x, max( p.y - 1, lo.y ) ) ).rgb;
		float3 b = result.read( uint2( p.x, min( p.y + 1, hi.y ) ) ).rgb;
		float3 nmin = min( min( min( l, r ), min( t, b ) ), c );
		float3 nmax = max( max( max( l, r ), max( t, b ) ), c );

		c = clamp( c + ( c - 0.25f * ( l + r + t + b ) ) * sharpen, nmin, nmax );
	}
	return c;
}

fragment float4 mb_composite_fs( MbTemporalVertexOut in [[stage_in]],
	texture2d<float, access::read> result [[texture(0)]],		// the anti-aliased scene (the history just written)
	texture2d<float, access::read> scene [[texture(1)]],		// the scene as drawn
	texture2d<float, access::read> velocity [[texture(2)]],
	constant MbCompositeUniforms &u [[buffer(0)]] )
{
	int2 p = int2( in.position.xy );
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;

	if ( any( p < lo ) || any( p > hi ) || u.params.y == 2.0f ) {
		return float4( scene.read( uint2( p ) ).rgb, 1.0f );
	}
	if ( u.params.y == 1.0f ) {
		return MB_ShowVelocity( velocity, p, u );
	}
	return float4( MB_CompositeColor( result, p, lo, hi, u.params.x ), 1.0f );
}

// THE MOTION BLUR

#define MB_MOTION_GROUP				16		// the tiles pass's threadgroups (MB_MOTION_GROUP in tr_mtl.m)
#define MB_MOTION_NEIGHBOR_GROUP	8		// the neighbour pass's (MB_MOTION_NEIGHBOR_GROUP in tr_mtl.m)

// the view weapon's view distances are scaled by this: in front of the world
constant float MB_MOTION_WEAPON_SCALE = 1.0f / 1024.0f;
// the depths a comparison fades over: this share of the nearer one, plus 1 unit
constant float MB_MOTION_SOFT_DEPTH = 0.05f;

// the view distance of a window depth whose range [lo, hi] stands for GL's [-1, 1], at most the far distance
static float MB_MotionRangeDistance( constant MbMotionUniforms &u, float wz, float lo, float hi )
{
	float ndc = ( wz - lo ) / ( hi - lo ) * 2.0f - 1.0f;
	float denom = ndc + u.depth.x;

	return abs( denom ) < 1e-9f ? u.depth.w : clamp( u.depth.y / denom, 0.0f, u.depth.w );
}

// the camera's motion of what is at view distance d at a target position (pixel centers at .5), in the view's
// texture coordinates as the motion vectors (this frame's place minus the last, x right, y down)
static float2 MB_MotionCamera( constant MbMotionUniforms &u, float2 pixel, float d )
{
	float2 ndc = ( pixel - float2( u.rect.xy ) ) / float2( u.rect.zw ) * 2.0f - 1.0f;

	ndc.y = -ndc.y;			// the target's y goes down
	ndc -= u.params.zw;		// where the surface is in the unjittered projection
	float4 prev = u.reproject * float4( ( ndc.x + u.proj.z ) * d / u.proj.x, ( ndc.y + u.proj.w ) * d / u.proj.y, -d, 1.0f );
	float2 prevNdc = prev.w > 1e-4f ? prev.xy / prev.w : ndc;

	return ( ndc - prevNdc ) * float2( 0.5f, -0.5f );
}

kernel void mb_motion_tiles_cs( depth2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> velocity [[texture(1)]],
	texture2d<float, access::write> pixelBlur [[texture(2)]],		// the half vector in pixels
	texture2d<float, access::write> samples [[texture(3)]],		// its length, the view distance
	texture2d<float, access::write> tiles [[texture(4)]],			// the longest half vector of each tile
	constant MbMotionUniforms &u [[buffer(0)]],
	uint2 group [[threadgroup_position_in_grid]],
	uint2 local [[thread_position_in_threadgroup]],
	uint index [[thread_index_in_threadgroup]],
	uint lane [[thread_index_in_simdgroup]],
	uint simd [[simdgroup_index_in_threadgroup]],
	uint simds [[simdgroups_per_threadgroup]] )
{
	threadgroup float2 simdBest[32];		// a SIMD group's longest (32 groups: at least 8 threads each)
	int k = u.tiles.z;
	int2 origin = int2( group ) * k;
	int2 size = int2( pixelBlur.get_width(), pixelBlur.get_height() );
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;
	float far = min( u.depth.w, 65000.0f );		// the far distance can be more than a half float holds
	float2 best = float2( 0.0f );
	float bestLen = 0.0f;

	for ( int y = int( local.y ); y < k; y += MB_MOTION_GROUP ) {
		for ( int x = int( local.x ); x < k; x += MB_MOTION_GROUP ) {
			int2 p = origin + int2( x, y );
			float2 h = float2( 0.0f );
			float d = far;

			if ( any( p >= size ) ) {
				continue;
			}
			if ( all( p >= lo ) && all( p <= hi ) ) {
				float wz = depth.read( uint2( p ) );
				float2 motion = velocity.read( uint2( p ) ).xy;
				float len;

				if ( wz >= 1.0f ) {
					d = far;
				} else if ( wz < u.depth.z ) {
					// the view weapon (its own depth range below the floor): its own motion only
					d = MB_MotionRangeDistance( u, wz, 0.0f, u.depth.z ) * MB_MOTION_WEAPON_SCALE;
				} else {
					d = min( MB_MotionRangeDistance( u, wz, u.depth.z, 1.0f ), far );
				}
				if ( u.params.y < 1.0f && wz >= u.depth.z ) {
					motion -= MB_MotionCamera( u, float2( p ) + 0.5f, d ) * ( 1.0f - u.params.y );
				}
				h = motion * float2( u.rect.zw ) * u.params.x;
				len = length( h );
				if ( len > float( k ) ) {
					h *= float( k ) / len;
				}
			}
			pixelBlur.write( float4( h, 0.0f, 0.0f ), uint2( p ) );
			samples.write( float4( length( h ), d, 0.0f, 0.0f ), uint2( p ) );
			if ( dot( h, h ) > bestLen ) {
				bestLen = dot( h, h );
				best = h;
			}
		}
	}

	// the longest of the threadgroup: of each SIMD group, then of those
	float most = simd_max( bestLen );
	uint holder = simd_min( bestLen == most ? lane : 0xffffffffu );
	float2 simdLongest = simd_shuffle( best, ushort( holder ) );

	if ( lane == 0 ) {
		simdBest[simd] = simdLongest;
	}
	threadgroup_barrier( mem_flags::mem_threadgroup );
	if ( index == 0 ) {
		float2 t = simdBest[0];

		for ( uint i = 1; i < simds; i++ ) {
			if ( dot( simdBest[i], simdBest[i] ) > dot( t, t ) ) {
				t = simdBest[i];
			}
		}
		tiles.write( float4( t, 0.0f, 0.0f ), group );
	}
}

kernel void mb_motion_neighbor_cs( texture2d<float, access::read> tiles [[texture(0)]],
	texture2d<float, access::write> neighbor [[texture(1)]],
	constant MbMotionUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]] )
{
	int2 t = int2( gid );
	float2 best = float2( 0.0f );
	float bestLen = 0.0f;

	if ( any( t >= u.tiles.xy ) ) {
		return;
	}
	for ( int y = -1; y <= 1; y++ ) {
		for ( int x = -1; x <= 1; x++ ) {
			float2 v = tiles.read( uint2( clamp( t + int2( x, y ), int2( 0 ), u.tiles.xy - 1 ) ) ).xy;

			if ( dot( v, v ) > bestLen ) {
				bestLen = dot( v, v );
				best = v;
			}
		}
	}
	neighbor.write( float4( best, 0.0f, 0.0f ), gid );
}

// interleaved gradient noise (Jimenez 2014), fixed per pixel: nothing blends it over frames after this
static float MB_MotionNoise( float2 pixel )
{
	return fract( 52.9829189f * fract( dot( pixel, float2( 0.06711056f, 0.00583715f ) ) ) );
}

// x: the sample is behind the center, y: in front of it
static float2 MB_MotionDepthCompare( float center, float s )
{
	float extent = MB_MOTION_SOFT_DEPTH * min( center, s ) + 1.0f;

	return saturate( 0.5f + float2( 1.0f, -1.0f ) * ( s - center ) / extent );
}

// whether blurs of these half lengths (the center's, the sample's) reach the offset, over half a pixel: a still pixel
// (under half a pixel) reaches only itself, so in a tile where something moves what doesn't stays as it is
static float2 MB_MotionSpread( float offset, float2 spread )
{
	return saturate( spread - offset + 0.5f );
}

static float3 MB_MotionLinear( float3 c )
{
	return c * c;
}

fragment float4 mb_composite_motion_fs( MbTemporalVertexOut in [[stage_in]],
	texture2d<float, access::read> result [[texture(0)]],		// the anti-aliased scene, or the scene as drawn
	texture2d<float, access::read> scene [[texture(1)]],		// the scene as drawn
	texture2d<float, access::read> velocity [[texture(2)]],
	texture2d<float, access::read> pixelBlur [[texture(3)]],
	texture2d<float, access::read> neighbor [[texture(4)]],
	texture2d<float, access::read> samples [[texture(5)]],
	constant MbCompositeUniforms &u [[buffer(0)]],
	constant MbMotionUniforms &m [[buffer(1)]] )
{
	int2 p = int2( in.position.xy );
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;
	float k = float( m.tiles.z );

	if ( any( p < lo ) || any( p > hi ) || u.params.y == 2.0f ) {
		return float4( scene.read( uint2( p ) ).rgb, 1.0f );
	}
	if ( u.params.y == 1.0f ) {
		return MB_ShowVelocity( velocity, p, u );
	}
	if ( u.params.y >= 4.0f ) {
		// a pixel's blur or the longest around its tile, a tile's side the brightest: right red, down green
		float2 v = ( u.params.y == 4.0f ? pixelBlur.read( uint2( p ) ).xy : neighbor.read( uint2( p / m.tiles.z ) ).xy ) / k;

		return float4( saturate( 0.5f + 0.5f * v.x ), saturate( 0.5f + 0.5f * v.y ), 0.5f, 1.0f );
	}

	float3 center = MB_CompositeColor( result, p, lo, hi, u.params.x );
	float2 vn = neighbor.read( uint2( p / m.tiles.z ) ).xy;
	float nLen = length( vn );

	if ( nLen < 0.5f ) {
		return float4( center, 1.0f );
	}

	float2 pixel = in.position.xy;
	float2 vc = pixelBlur.read( uint2( p ) ).xy;
	float2 cs = samples.read( uint2( p ) ).xy;
	float cLen = cs.x, cd = cs.y;
	int pairs = m.tiles.w;
	float jitter = MB_MotionNoise( pixel ) - 0.5f;
	// its own blur differs from the tile's by more than about 25 degrees
	bool own = cLen >= 0.5f && dot( vc, vn ) < 0.9f * cLen * nLen;
	float4 sum = float4( 0.0f );

	for ( int i = 0; i < pairs; i++ ) {
		float2 v = own && ( i & 1 ) != 0 ? vc : vn;
		float2 offset = v * ( ( float( i ) + 0.5f + jitter ) / float( pairs ) );
		float offsetLen = length( offset );
		uint2 p1 = uint2( clamp( int2( pixel + offset ), lo, hi ) );
		uint2 p2 = uint2( clamp( int2( pixel - offset ), lo, hi ) );
		float2 s1 = samples.read( p1 ).xy;
		float2 s2 = samples.read( p2 ).xy;
		float w1 = dot( MB_MotionDepthCompare( cd, s1.y ), MB_MotionSpread( offsetLen, float2( cLen, s1.x ) ) );
		float w2 = dot( MB_MotionDepthCompare( cd, s2.y ), MB_MotionSpread( offsetLen, float2( cLen, s2.x ) ) );
		// the mirror: the background behind what moves is estimated from the other side
		bool2 mirror = bool2( s1.y > s2.y, s2.x > s1.x );

		w1 = all( mirror ) ? w2 : w1;
		w2 = any( mirror ) ? w2 : w1;
		sum += w1 * float4( MB_MotionLinear( result.read( p1 ).rgb ), 1.0f );
		sum += w2 * float4( MB_MotionLinear( result.read( p2 ).rgb ), 1.0f );
	}
	if ( sum.a <= 0.0f ) {
		return float4( center, 1.0f );
	}
	sum /= float( 2 * pairs );
	return float4( sqrt( max( sum.rgb + ( 1.0f - sum.a ) * MB_MotionLinear( center ), 0.0f ) ), 1.0f );
}

// THE DEPTH OF FIELD

#define MB_DOF_GROUP	8		// its threadgroups' width and height (MB_DOF_GROUP in tr_mtl.m)

struct MbDofUniforms
{
	int4	rect;		// the main view in target pixels: x, y (from the top left), width, height
	int4	tiles;		// xy: the tiles across and down (from the target's top left), z: a tile's side in pixels (even), w: the gathers' samples
	float4	depth;		// the main view's projection's [10], [14], the depth floor (below it the view weapon's range; 0: none), the far distance
	float4	lens;		// x: the CoC's scale (pixels times units), y: the largest CoC (a tile's side), z: 1 / r_dofFocus (0: the crosshair's), w: the focus's step (1: at once)
	float4	params;		// x: the focus area's half side in pixels, y: 1 r_temporalShow 6, z: 1 the apply pass writes every tile, w: 1 the focus has no last value
};

// the view distance of a window depth, 0 for the view weapon (below the floor: in front of all, never blurred); the
// sky (and what nothing drew) the far distance
static float MB_DofDistance( constant MbDofUniforms &u, float wz )
{
	if ( wz < u.depth.z ) {
		return 0.0f;
	}
	float ndc = ( wz - u.depth.z ) / ( 1.0f - u.depth.z ) * 2.0f - 1.0f;
	float denom = ndc + u.depth.x;

	return abs( denom ) < 1e-9f ? u.depth.w : clamp( u.depth.y / denom, 0.0f, u.depth.w );
}

// the CoC of a view distance in full pixels, signed: behind the focus positive
static float MB_DofCoc( constant MbDofUniforms &u, float d, float invFocus )
{
	if ( d <= 0.0f ) {
		return 0.0f;
	}
	return clamp( u.lens.x * ( invFocus - 1.0f / max( d, 1.0f ) ), -u.lens.y, u.lens.y );
}

// near-linear light (squared, as the motion blur's) and back
static float3 MB_DofLinear( float3 c )
{
	c = max( c, 0.0f );
	return c * c;
}

static float3 MB_DofEncode( float3 lin )
{
	return sqrt( max( lin, 0.0f ) );
}

// 8x8 points over a square of 2 * params.x around the main view's center; the view weapon left out. The thread whose
// distance ranks at a quarter of the others' writes the focus: the nearest of what is under the crosshair wins, a stray
// pixel doesn't, and a sliver of roof at the edge of the sky doesn't either
kernel void mb_dof_focus_cs( depth2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read_write> focus [[texture(1)]],
	constant MbDofUniforms &u [[buffer(0)]],
	uint2 local [[thread_position_in_threadgroup]],
	uint index [[thread_index_in_threadgroup]] )
{
	threadgroup float dists[MB_DOF_GROUP * MB_DOF_GROUP];
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;
	float2 center = float2( u.rect.xy ) + float2( u.rect.zw ) * 0.5f;
	float2 offset = ( float2( local ) - 3.5f ) / 3.5f * u.params.x;
	int2 p = clamp( int2( center + offset ), lo, hi );
	float wz = depth.read( uint2( p ) );
	float d = wz < u.depth.z ? -1.0f : MB_DofDistance( u, wz );
	int valid = 0, rank = 0;

	dists[index] = d;
	threadgroup_barrier( mem_flags::mem_threadgroup );
	for ( uint j = 0; j < MB_DOF_GROUP * MB_DOF_GROUP; j++ ) {
		float o = dists[j];

		if ( o >= 0.0f ) {
			valid++;
			if ( o < d || ( o == d && j < index ) ) {
				rank++;
			}
		}
	}
	// only the view weapon there: the focus stays (the far distance when there is none yet)
	if ( valid == 0 ? index == 0 : d >= 0.0f && rank == valid / 4 ) {
		bool reset = u.params.w != 0.0f;
		float last = reset ? 0.0f : focus.read( uint2( 0 ) ).r;
		float target = valid > 0 ? 1.0f / max( d, 8.0f ) : last > 0.0f ? last : 1.0f / u.depth.w;

		if ( u.lens.z > 0.0f ) {
			target = u.lens.z;
		}
		// eased in 1 / F, so near and far changes take as long
		focus.write( float4( last > 0.0f ? mix( last, target, u.lens.w ) : target, 0.0f, 0.0f, 0.0f ), uint2( 0 ) );
	}
}

kernel void mb_dof_setup_cs( depth2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],		// the sharp scene: the anti-aliased one with r_taa
	texture2d<float, access::read> focus [[texture(2)]],
	texture2d<float, access::write> halfRes [[texture(3)]],	// near-linear color, the CoC in full pixels
	texture2d<float, access::write> tiles [[texture(4)]],		// the largest foreground CoC (as a radius), the largest |CoC|
	constant MbDofUniforms &u [[buffer(0)]],
	uint2 group [[threadgroup_position_in_grid]],
	uint2 local [[thread_position_in_threadgroup]],
	uint index [[thread_index_in_threadgroup]],
	uint lane [[thread_index_in_simdgroup]],
	uint simd [[simdgroup_index_in_threadgroup]],
	uint simds [[simdgroups_per_threadgroup]] )
{
	threadgroup float2 simdBest[MB_DOF_GROUP * MB_DOF_GROUP];
	int side = u.tiles.z / 2;		// a tile's half resolution texels across
	int2 origin = int2( group ) * side;
	int2 size = int2( halfRes.get_width(), halfRes.get_height() );
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;
	float invFocus = focus.read( uint2( 0 ) ).r;
	float2 best = float2( 0.0f );

	for ( int y = int( local.y ); y < side; y += MB_DOF_GROUP ) {
		for ( int x = int( local.x ); x < side; x += MB_DOF_GROUP ) {
			int2 h = origin + int2( x, y );
			float3 colors[4];
			float cocs[4];
			bool inside[4];
			float nearest = 1e30f, coc = 0.0f;
			float4 sum = float4( 0.0f );

			if ( any( h >= size ) ) {
				continue;
			}
			for ( int k = 0; k < 4; k++ ) {
				int2 p = h * 2 + int2( k & 1, k >> 1 );

				inside[k] = all( p >= lo ) && all( p <= hi );
				if ( inside[k] ) {
					float d = MB_DofDistance( u, depth.read( uint2( p ) ) );

					colors[k] = MB_DofLinear( color.read( uint2( p ) ).rgb );
					cocs[k] = MB_DofCoc( u, d, invFocus );
					if ( d < nearest ) {
						nearest = d;
						coc = cocs[k];
					}
				}
			}
			// the texel is its nearest pixel's: of the others, what is at another depth counts less (no background color
			// in the foreground's blur at its outline)
			for ( int k = 0; k < 4; k++ ) {
				if ( inside[k] ) {
					sum += float4( colors[k], 1.0f ) / ( 1.0f + abs( cocs[k] - coc ) );
				}
			}
			halfRes.write( sum.a > 0.0f ? float4( sum.rgb / sum.a, coc ) : float4( 0.0f ), uint2( h ) );
			best = max( best, float2( -coc, abs( coc ) ) );
		}
	}

	// the largest of the threadgroup: of each SIMD group, then of those
	float2 most = float2( simd_max( best.x ), simd_max( best.y ) );

	if ( lane == 0 ) {
		simdBest[simd] = most;
	}
	threadgroup_barrier( mem_flags::mem_threadgroup );
	if ( index == 0 ) {
		for ( uint i = 1; i < simds; i++ ) {
			most = max( most, simdBest[i] );
		}
		tiles.write( float4( most, 0.0f, 0.0f ), group );
	}
}

// the golden angle's cosine and sine: a sample's direction is the one before turned by it (no sin and cos a sample)
constant float2 MB_DOF_GOLDEN_TURN = float2( -0.73736887f, 0.67549029f );

// the next sample of a golden angle spiral over a disc (rScale: its radius over the root of the samples, in half
// resolution texels), as dense at the rim as in the middle: sample i at i golden angles plus the texel's turn (dir:
// its cosine and sine, turned on to the next sample's), at rScale sqrt(i + 0.5)
static float2 MB_DofDisc( int i, float rScale, thread float2 &dir, thread float &dist )
{
	float2 o;

	dist = rScale * sqrt( float( i ) + 0.5f );
	o = dist * dir;
	dir = float2( dir.x * MB_DOF_GOLDEN_TURN.x - dir.y * MB_DOF_GOLDEN_TURN.y, dir.x * MB_DOF_GOLDEN_TURN.y + dir.y * MB_DOF_GOLDEN_TURN.x );
	return o;
}

kernel void mb_dof_gather_cs( texture2d<float, access::read> halfRes [[texture(0)]],
	texture2d<float, access::read> tiles [[texture(1)]],
	texture2d<float, access::write> farOut [[texture(2)]],		// the background's color times k, k: the texel's share of background
	texture2d<float, access::write> nearOut [[texture(3)]],	// the foreground's color times its opacity, its opacity
	texture2d<float, access::write> neighbor [[texture(4)]],	// the largest of the 3x3 tiles around each tile
	constant MbDofUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]] )
{
	int2 h = int2( gid );
	int2 size = int2( halfRes.get_width(), halfRes.get_height() );
	int2 lo = u.rect.xy / 2, hi = ( u.rect.xy + u.rect.zw - 1 ) / 2;
	int side = u.tiles.z / 2;
	int2 t = h / side;
	int n = u.tiles.w;
	float invN = rsqrt( float( n ) );
	float2 around = float2( 0.0f );

	if ( any( h >= size ) ) {
		return;
	}
	// the largest of the 3x3 tiles around the texel's (the Vulkan renderer's pass of its own folded in here: a dispatch
	// and a pipeline fewer, the same GPU time): the tile's first texel keeps it for the apply pass and the blend
	for ( int y = -1; y <= 1; y++ ) {
		for ( int x = -1; x <= 1; x++ ) {
			around = max( around, tiles.read( uint2( clamp( t + int2( x, y ), int2( 0 ), u.tiles.xy - 1 ) ) ).xy );
		}
	}
	if ( all( h == t * side ) ) {
		neighbor.write( float4( around, 0.0f, 0.0f ), uint2( t ) );
	}
	around *= 0.5f;		// in half resolution texels
	float4 c = halfRes.read( uint2( h ) );
	float cc = c.a * 0.5f;
	float rot = MB_MotionNoise( float2( h ) ) * 6.2831853f;
	float2 turn = float2( cos( rot ), sin( rot ) );
	float4 far = float4( c.rgb, 1.0f ), near = float4( 0.0f );

	// the background: the texel's own blur
	if ( cc >= 0.5f ) {
		float2 dir = turn;

		for ( int i = 0; i < n; i++ ) {
			float r;
			float2 o = MB_DofDisc( i, ( cc + 0.5f ) * invN, dir, r );
			float4 s = halfRes.read( uint2( clamp( h + int2( round( o ) ), lo, hi ) ) );
			float sc = s.a * 0.5f;

			if ( sc > 0.0f ) {
				far += saturate( min( sc, cc ) - r + 1.0f ) * float4( s.rgb, 1.0f );
			}
		}
	}

	// the foreground: what spreads over the texel from around it, each sample standing for the disc's area over n, its
	// share of its own disc
	float nearR = around.x;

	if ( nearR >= 0.5f ) {
		float2 dir = turn;
		float area;

		nearR += 0.5f;
		area = nearR * nearR / float( n );
		for ( int i = 0; i < n; i++ ) {
			float r;
			float2 o = MB_DofDisc( i, nearR * invN, dir, r );
			float4 s = halfRes.read( uint2( clamp( h + int2( round( o ) ), lo, hi ) ) );
			float sr = -s.a * 0.5f;

			if ( sr > 0.0f ) {
				near += saturate( sr - r + 1.0f ) * area / max( sr * sr, 1.0f ) * float4( s.rgb, 1.0f );
			}
		}
	}

	// premultiplied, for the apply pass's bilinear taps: the background by the texel's share of background (what the
	// apply pass's tent counts it with), the foreground by its opacity
	float k = saturate( c.a - 0.5f );
	float a = min( near.a, 1.0f );

	farOut.write( float4( far.rgb / far.a * k, k ), uint2( h ) );
	nearOut.write( near.a > 0.0f ? float4( near.rgb / near.a * a, a ) : float4( 0.0f ), uint2( h ) );
}

kernel void mb_dof_apply_cs( depth2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::read> focus [[texture(2)]],
	texture2d<float, access::read> neighbor [[texture(3)]],
	texture2d<float> farIn [[texture(4)]],
	texture2d<float> nearIn [[texture(5)]],
	texture2d<float, access::write> result [[texture(6)]],		// gamma-encoded, alpha 1 where the pixel is as it was
	constant MbDofUniforms &u [[buffer(0)]],
	uint2 gid [[thread_position_in_grid]] )
{
	constexpr sampler bilinear( filter::linear, address::clamp_to_edge );
	int2 p = u.rect.xy + int2( gid );
	int2 lo = u.rect.xy, hi = u.rect.xy + u.rect.zw - 1;

	if ( any( p > hi ) ) {
		return;
	}
	// no blur reaches the tile: what is shown there is the sharp scene (but with the motion blur, whose samples then
	// read this everywhere, or r_temporalShow 6)
	float2 around = neighbor.read( uint2( p / u.tiles.z ) ).xy;

	if ( around.y < 0.5f && u.params.y == 0.0f && u.params.z == 0.0f ) {
		return;
	}
	float3 sharp = color.read( uint2( p ) ).rgb;
	float d = MB_DofDistance( u, depth.read( uint2( p ) ) );
	float cc = MB_DofCoc( u, d, focus.read( uint2( 0 ) ).r );

	if ( u.params.y != 0.0f ) {
		// r_temporalShow 6: the foreground's CoC red, the background's blue (8 pixels the brightest), in focus the scene
		// in grey; the focus area's square green
		float2 off = abs( float2( p ) + 0.5f - ( float2( u.rect.xy ) + float2( u.rect.zw ) * 0.5f ) );
		float3 c = float3( dot( MB_DofLinear( sharp ), float3( 0.2126f, 0.7152f, 0.0722f ) ) * 0.4f );

		c += float3( saturate( -cc / 8.0f ), 0.0f, saturate( cc / 8.0f ) );
		if ( abs( max( off.x, off.y ) - u.params.x ) < 1.0f ) {
			c = float3( 0.0f, 1.0f, 0.0f );
		}
		result.write( float4( MB_DofEncode( c ), 0.0f ), uint2( p ) );
		return;
	}
	if ( around.y < 0.5f || d <= 0.0f ) {
		result.write( float4( sharp, 1.0f ), uint2( p ) );
		return;
	}
	float3 lin = MB_DofLinear( sharp );

	// the 3x3 half resolution texels around the nearest, a tent of 1.5 texels (the background only from texels that are
	// background: farIn is premultiplied by their share of it), as 2x2 bilinear taps: on each axis the weights 0.5 - f,
	// 1.5 - |f|, 0.5 + f of the texels b - 1, b, b + 1 (f the pixel's offset from b), the middle one split between the
	// two taps as the outer ones are. Taps clamped to the view's texels as the texels were
	float2 hp = ( float2( p ) + 0.5f ) * 0.5f - 0.5f;
	float2 b = round( hp );
	float2 f = hp - b;
	float2 wl = 0.5f - f, wc = 1.5f - abs( f ), wr = 0.5f + f;
	float2 split = wc * wl;		// of the middle weight, to the first tap
	float2 w0 = wl + split, w1 = wc - split + wr;
	float2 t0 = b - 1.0f + split / max( w0, 1e-6f ), t1 = b + wr / max( w1, 1e-6f );
	float2 halfSize = float2( farIn.get_width(), farIn.get_height() );
	float2 tlo = float2( lo / 2 ), thi = float2( hi / 2 );
	float2 uv0 = ( clamp( t0, tlo, thi ) + 0.5f ) / halfSize, uv1 = ( clamp( t1, tlo, thi ) + 0.5f ) / halfSize;
	float4 far = farIn.sample( bilinear, uv0, level( 0 ) ) * ( w0.x * w0.y )
		+ farIn.sample( bilinear, float2( uv1.x, uv0.y ), level( 0 ) ) * ( w1.x * w0.y )
		+ farIn.sample( bilinear, float2( uv0.x, uv1.y ), level( 0 ) ) * ( w0.x * w1.y )
		+ farIn.sample( bilinear, uv1, level( 0 ) ) * ( w1.x * w1.y );
	float4 near = nearIn.sample( bilinear, uv0, level( 0 ) ) * ( w0.x * w0.y )
		+ nearIn.sample( bilinear, float2( uv1.x, uv0.y ), level( 0 ) ) * ( w1.x * w0.y )
		+ nearIn.sample( bilinear, float2( uv0.x, uv1.y ), level( 0 ) ) * ( w0.x * w1.y )
		+ nearIn.sample( bilinear, uv1, level( 0 ) ) * ( w1.x * w1.y );
	float wn = ( wl.x + wc.x + wr.x ) * ( wl.y + wc.y + wr.y );
	bool changed = false;

	if ( cc > 0.5f && far.a > 1e-4f ) {
		lin = mix( lin, far.rgb / far.a, smoothstep( 0.5f, 2.0f, cc ) );
		changed = true;
	}
	if ( near.a > 1e-4f ) {
		// a pixel of the foreground takes its blur whole: at its edge the coverage is about half, which would leave its
		// sharp outline
		float alpha = max( saturate( near.a / wn ), smoothstep( 0.5f, 2.0f, -cc ) );

		lin = mix( lin, near.rgb / near.a, alpha );
		changed = true;
	}
	result.write( changed ? float4( MB_DofEncode( lin ), 0.0f ) : float4( sharp, 1.0f ), uint2( p ) );
}

// the depth of field's result over the scene where it changed a pixel, in the tiles a blur reaches (r_temporalShow 6:
// everywhere); the scissor is the main view
fragment float4 mb_dof_blend_fs( MbTemporalVertexOut in [[stage_in]],
	texture2d<float, access::read> result [[texture(0)]],
	texture2d<float, access::read> neighbor [[texture(1)]],
	constant MbDofUniforms &u [[buffer(0)]] )
{
	uint2 p = uint2( in.position.xy );

	if ( u.params.y == 0.0f && neighbor.read( p / uint( u.tiles.z ) ).y < 0.5f ) {
		discard_fragment();
	}
	float4 c = result.read( p );

	if ( c.a > 0.5f ) {
		discard_fragment();
	}
	return float4( c.rgb, 1.0f );
}
