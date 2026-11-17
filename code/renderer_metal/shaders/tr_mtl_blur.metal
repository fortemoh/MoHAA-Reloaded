// tr_mtl_blur.metal -- the frame blurred, the Reloaded menus' backdrop ("*frameblur", mbFrameBlur in tr_mtl.m):
// each step is one triangle over its target, sampling the step before with bilinear filtering.

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

// Four taps a texel of the source either side of the target texel's place, each the average of the 2x2 texels around
// it: the average of a 4x4 block when the source is four times the target's size. spread 0: one tap (a 2x2 block
// when the source is twice the size, bilinear scaling otherwise). Opaque: the image samples alpha 1 anyway.
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
