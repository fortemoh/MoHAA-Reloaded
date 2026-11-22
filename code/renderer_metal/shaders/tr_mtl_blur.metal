// tr_mtl_blur.metal -- the frame blurred, the Reloaded menus' backdrop ("*frameblur", mbFrameBlur in tr_mtl.m):
// each step is one triangle over its target: the frame averaged down to a quarter of its size (mb_blur_fs), then a
// Gaussian there, a pass each way (mb_gauss_fs), as the Vulkan renderer's (vk_blur.c, frame_blur.comp).

#include <metal_stdlib>
using namespace metal;

struct MbBlurVertexOut
{
	float4 position [[position]];
	float2 uv;			// 0..1 over the target, top down as texture coordinates are
};

// One triangle covering the whole target: (-1,-1), (3,-1), (-1,3).
vertex MbBlurVertexOut mb_blur_vs( uint vid [[vertex_id]] )
{
	float2 uv = float2( ( vid << 1 ) & 2, vid & 2 );
	MbBlurVertexOut out;
	out.position = float4( uv * 2.0f - 1.0f, 0.0f, 1.0f );
	out.uv = float2( uv.x, 1.0f - uv.y );
	return out;
}

// Four taps spread either side of the target texel's place, each bilinear. A texel of the source: the average of a 4x4
// block when the source is four times the target's size; 0: one tap (a 2x2 block at half the size); half a texel at
// twice the size: a tent over 3x3 source texels, smooth where they meet. Opaque: the image samples alpha 1 anyway.
fragment float4 mb_blur_fs( MbBlurVertexOut in [[stage_in]],
	texture2d<float> source [[texture(0)]],
	constant float2 &spread [[buffer(0)]] )
{
	constexpr sampler bilinear( filter::linear, address::clamp_to_edge );
	float3 c = source.sample( bilinear, in.uv - spread ).rgb
		+ source.sample( bilinear, in.uv + float2( spread.x, -spread.y ) ).rgb
		+ source.sample( bilinear, in.uv + float2( -spread.x, spread.y ) ).rgb
		+ source.sample( bilinear, in.uv + spread ).rgb;
	return float4( c * 0.25f, 1.0f );
}

// The Gaussian's pass: across, from the quarter into a half-float target the same size (step ( 1, 0 )), or down, from
// it back into the quarter (step ( 0, 1 )). radius texels each side (three sigmas), each weighted
// exp( -i^2 / 2 sigma^2 ); past the edges the edge texel. Opaque, as above.
struct MbGaussParams
{
	int2	step;
	float	sigma;		// texels of the quarter
	int		radius;
};

fragment float4 mb_gauss_fs( MbBlurVertexOut in [[stage_in]],
	texture2d<float> source [[texture(0)]],
	constant MbGaussParams &params [[buffer(0)]] )
{
	const int2 p = int2( in.position.xy );
	const int2 hi = int2( source.get_width(), source.get_height() ) - 1;
	const float k = -0.5f / ( params.sigma * params.sigma );
	float3 sum = source.read( uint2( p ) ).rgb;
	float wsum = 1.0f;

	for ( int i = 1; i <= params.radius; i++ ) {
		const float w = exp( float( i * i ) * k );
		const int2 o = params.step * i;

		sum += w * ( source.read( uint2( clamp( p + o, int2( 0 ), hi ) ) ).rgb
			+ source.read( uint2( clamp( p - o, int2( 0 ), hi ) ) ).rgb );
		wsum += 2.0f * w;
	}
	return float4( sum / wsum, 1.0f );
}
