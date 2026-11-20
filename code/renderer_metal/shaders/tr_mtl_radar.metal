// tr_mtl_radar.metal -- the Competitive HUD's radar picture made for a storey's height (RE_RegisterRadar2D and
// RE_Radar2DHeight, mbRadarImage and mbRadarHeight in tr_mtl.m): the texels the UI's own picture would have
// (RM_Paint in cl_uicomp.cpp), byte for byte, so a storey's change costs a number instead of a new picture.
//
// A texel's cell (texels x texels of the picture) shows its floor nearest the height, the first of equals in the list's
// order: its inside its shade, its sides with a stair a stripe, brighter the nearer the floor is to the height (no
// outlines: a wall is the shade's edge); no floor: clear, its colour kept. A whole picture: a thread a texel. A height's
// change: a thread a cell of those that can change (the list the UI made), writing only the texels that do.
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
	uchar	sides;		// bits 4-7: a stair toward the left, right, north, south neighbour (0-3: 0)
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

// a floor's look for height z: its stair's alpha, its inside's, its sides
struct MbRadarLook
{
	uint	stair, alpha, sides;
};

static MbRadarLook mb_radar_look( MbRadarFloor f, float z )
{
	const float focus = clamp( ( 120.0f - fabs( f.z - z ) ) / 56.0f, 0.0f, 1.0f );
	MbRadarLook l;

	l.stair = uint( fma( fma( 0.3f, focus, 0.22f ) - f.fill, 0.8f, f.fill ) * 255.0f );
	l.alpha = f.alpha;
	l.sides = f.sides;
	return l;
}

// the alpha of a cell's texel i, j (j down from its top row, the map's north up): on its left, right, top or bottom
// row that side's stair (a stripe), if it has one; else its inside
static uint mb_radar_alpha( MbRadarLook l, uint i, uint j, uint t )
{
	const uint at = uint( i == 0 ) | uint( i == t - 1 ) << 1 | uint( j == 0 ) << 2 | uint( j == t - 1 ) << 3;

	return ( l.sides & at << 4 ) ? l.stair : l.alpha;
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

	if ( first == last ) {
		picture.write( mb_radar_texel( 0u ), pos );
		return;
	}
	const MbRadarLook l = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.z )], p.z );
	picture.write( mb_radar_texel( mb_radar_alpha( l, pos.x % t, pos.y % t, t ) ), pos );
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
	const MbRadarLook l = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.z )], p.z );
	const MbRadarLook was = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.zOld )], p.zOld );

	if ( l.alpha == was.alpha && l.sides == was.sides && ( !( l.sides & 240u ) || l.stair == was.stair ) ) {
		return;	// (its texels the same)
	}
	for ( uint j = 0; j < t; j++ ) {
		for ( uint i = 0; i < t; i++ ) {
			const uint alpha = mb_radar_alpha( l, i, j, t );

			if ( alpha != mb_radar_alpha( was, i, j, t ) ) {
				picture.write( mb_radar_texel( alpha ), origin + uint2( i, j ) );
			}
		}
	}
}
