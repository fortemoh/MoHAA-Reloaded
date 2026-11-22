// tr_mtl_temporal.metal -- the temporal path's effects (r_temporal; tr_temporal.c, mbTemporalEndScene in tr_mtl.m),
// drawn when the 3D view's scene ends, before the 2D. Ideas from JayRewd's Vulkan renderer (renderervk's
// taa_resolve.comp and temporal_composite.frag), written for Metal: the scene's targets stay render targets, never
// shader-written (only the history is).
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
// motion vectors instead, 2 the scene without the anti-aliasing.
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
		// the motion in pixels times the scale, 32 pixels a frame the most shown at 1: right red, down green, none grey
		float2 v = velocity.read( uint2( p ) ).xy * float2( u.rect.zw ) * ( u.params.z / 64.0f );

		return float4( saturate( 0.5f + v.x ), saturate( 0.5f + v.y ), 0.5f, 1.0f );
	}

	float3 c = result.read( uint2( p ) ).rgb;

	if ( u.params.x > 0.0f ) {
		float3 l = result.read( uint2( max( p.x - 1, lo.x ), p.y ) ).rgb;
		float3 r = result.read( uint2( min( p.x + 1, hi.x ), p.y ) ).rgb;
		float3 t = result.read( uint2( p.x, max( p.y - 1, lo.y ) ) ).rgb;
		float3 b = result.read( uint2( p.x, min( p.y + 1, hi.y ) ) ).rgb;
		float3 nmin = min( min( min( l, r ), min( t, b ) ), c );
		float3 nmax = max( max( max( l, r ), max( t, b ) ), c );

		c = clamp( c + ( c - 0.25f * ( l + r + t + b ) ) * u.params.x, nmin, nmax );
	}
	return float4( c, 1.0f );
}
