// tr_mtl_radar.metal -- the Competitive HUD's radar picture made for a storey's height (RE_RegisterRadar2D and
// RE_Radar2DHeight, mbRadarImage and mbRadarHeight in tr_mtl.m): the texels the UI's own picture would have
// (RM_Paint in cl_uicomp.cpp), byte for byte, so a storey's change costs a number instead of a new picture.
//
// A texel's cell (texels x texels of the picture) shows its floor nearest the height, the first of equals in the list's
// order: its inside its shade, its sides with an edge (a wall, a drop) a line, with a stair a stripe, both brighter the
// nearer the floor is to the height; no floor: clear. A whole picture: a thread a texel. A height's change: a thread a
// cell of those that can change (the list the UI made), writing only the texels that do.
//
// Safe math, and the CPU's own operations (clang contracts a + b * c into one fused multiply-add on arm64, as fma()
// here; a correctly rounded division), so every byte is the CPU's.

#include <metal_stdlib>
using namespace metal;

#pragma METAL fp math_mode(safe)

struct MbRadarFloor
{
	float	z;			// its height
	float	fill;		// alpha / 255.0f
	uchar	alpha;		// its inside's alpha
	uchar	sides;		// bits 0-3: an edge toward the left, right, north, south neighbour; 4-7: a stair there
	ushort	pad;
};

struct MbRadarParams
{
	uint	cw, ch, texels;
	float	z, zOld;	// the height to paint for, the one painted before (a height's change)
	uint	pad[3];
};

// the floor of cells[first, last) nearest z
static uint mb_radar_nearest( device const MbRadarFloor *floors, uint first, uint last, float z )
{
	uint best = first;

	for ( uint n = first + 1; n < last; n++ ) {
		if ( fabs( floors[n].z - z ) < fabs( floors[best].z - z ) ) {
			best = n;
		}
	}
	return best;
}

// a floor's alpha at its sides 0-3 and inside (4) for height z
static void mb_radar_alphas( MbRadarFloor f, float z, thread uint a[5] )
{
	const float focus = clamp( ( 120.0f - fabs( f.z - z ) ) / 56.0f, 0.0f, 1.0f );
	const uint line = uint( fma( 0.6f, focus, 0.3f ) * 255.0f );
	const uint stair = uint( fma( fma( 0.3f, focus, 0.22f ) - f.fill, 0.8f, f.fill ) * 255.0f );

	for ( int k = 0; k < 4; k++ ) {
		a[k] = ( f.sides & ( 1u << k ) ) ? line : ( f.sides & ( 16u << k ) ) ? stair : uint( f.alpha );
	}
	a[4] = f.alpha;
}

// the side a cell's texel i, j (j down from its top row) shows: left, right, then the north neighbour's at the bottom
// row and the south's at the top (as the UI's picture has always drawn them), else its inside
static uint mb_radar_side( uint i, uint j, uint t )
{
	return i == 0 ? 0 : i == t - 1 ? 1 : j == t - 1 ? 2 : j == 0 ? 3 : 4;
}

static float4 mb_radar_texel( uint alpha )
{
	return float4( 235.0f, 240.0f, 245.0f, float( alpha ) ) * ( 1.0f / 255.0f );
}

// the whole picture for height z: a thread a texel (the picture's top row is the map's north)
kernel void mb_radar_full_cs( device const uint *cellFloors [[buffer(0)]],
	device const MbRadarFloor *floors [[buffer(1)]],
	constant MbRadarParams &p [[buffer(3)]],
	texture2d<float, access::write> picture [[texture(0)]],
	uint2 pos [[thread_position_in_grid]] )
{
	const uint t = p.texels, c = ( p.ch - 1 - pos.y / t ) * p.cw + pos.x / t;
	const uint first = cellFloors[c], last = cellFloors[c + 1];
	uint a[5];

	if ( first == last ) {
		picture.write( float4( 0.0f ), pos );
		return;
	}
	mb_radar_alphas( floors[mb_radar_nearest( floors, first, last, p.z )], p.z, a );
	picture.write( mb_radar_texel( a[mb_radar_side( pos.x % t, pos.y % t, t )] ), pos );
}

// a height's change from zOld to z: a thread a cell that can change, its texels that do
kernel void mb_radar_cs( device const uint *cellFloors [[buffer(0)]],
	device const MbRadarFloor *floors [[buffer(1)]],
	device const uint *changing [[buffer(2)]],
	constant MbRadarParams &p [[buffer(3)]],
	texture2d<float, access::write> picture [[texture(0)]],
	uint id [[thread_position_in_grid]] )
{
	const uint c = changing[id], t = p.texels, first = cellFloors[c], last = cellFloors[c + 1];
	const uint2 origin = uint2( c % p.cw * t, ( p.ch - 1 - c / p.cw ) * t );
	uint a[5], was[5];

	mb_radar_alphas( floors[mb_radar_nearest( floors, first, last, p.z )], p.z, a );
	mb_radar_alphas( floors[mb_radar_nearest( floors, first, last, p.zOld )], p.zOld, was );
	if ( a[0] == was[0] && a[1] == was[1] && a[2] == was[2] && a[3] == was[3] && a[4] == was[4] ) {
		return;
	}
	for ( uint j = 0; j < t; j++ ) {
		for ( uint i = 0; i < t; i++ ) {
			const uint side = mb_radar_side( i, j, t );

			if ( a[side] != was[side] ) {
				picture.write( mb_radar_texel( a[side] ), origin + uint2( i, j ) );
			}
		}
	}
}
