// tr_mtl_final.metal -- the last pass of every frame: copies the offscreen scene target into the drawable,
// applies the gamma ramp (GL1 loads the same 256-entry table into the display's hardware gamma), then the
// picture's grade, r_contrast and r_vibrance (vulkan-reloaded's output.frag).

#include <metal_stdlib>
using namespace metal;

struct MbFinalVertexOut
{
	float4 position [[position]];
};

// What the pass reads (tr_mtl.m's mbFinal_t, the same layout): ramp[i].rgb = the gamma table entries for input
// value i (0..255) per channel; the grade, 1 none.
struct MbFinal
{
	uchar4	ramp[256];
	float	contrast;	// r_contrast
	float	vibrance;	// r_vibrance
};

// One triangle covering the whole target: (-1,-1), (3,-1), (-1,3).
vertex MbFinalVertexOut mb_final_vs( uint vid [[vertex_id]] )
{
	float2 uv = float2( ( vid << 1 ) & 2, vid & 2 );
	MbFinalVertexOut out;
	out.position = float4( uv * 2.0f - 1.0f, 0.0f, 1.0f );
	return out;
}

// The scene target and the drawable have the same size, so the copy is a plain texel read.
fragment half4 mb_final_fs( MbFinalVertexOut in [[stage_in]],
	texture2d<half, access::read> scene [[texture(0)]],
	constant MbFinal &fin [[buffer(0)]] )
{
	half3 s = scene.read( uint2( in.position.xy ) ).rgb;
	uint3 i = uint3( s * 255.0h + 0.5h );
	float3 c = float3( fin.ramp[i.r].r, fin.ramp[i.g].g, fin.ramp[i.b].b ) / 255.0f;

	// r_contrast (Project: Omaha's 843a74c0), around mid gray of the gamma-encoded picture, as a monitor's contrast
	if ( fin.contrast != 1.0f ) {
		c = max( ( c - float3( 0.5f ) ) * fin.contrast + float3( 0.5f ), float3( 0.0f ) );
	}
	// r_vibrance: the colour saturation, away from or toward the gray of the same BT.709 luma in linear light
	if ( fin.vibrance != 1.0f ) {
		float3 l = pow( max( c, float3( 0.0f ) ), float3( 2.2f ) );

		l = max( mix( float3( dot( l, float3( 0.2126f, 0.7152f, 0.0722f ) ) ), l, fin.vibrance ), float3( 0.0f ) );
		c = pow( l, float3( 1.0f / 2.2f ) );
	}
	return half4( half3( clamp( c, 0.0f, 1.0f ) ), 1.0h );
}
