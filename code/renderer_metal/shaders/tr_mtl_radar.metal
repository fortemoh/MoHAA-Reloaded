// tr_mtl_radar.metal -- the Competitive HUD's radar picture made for a storey's height (RE_RegisterRadar2D and
// RE_Radar2DHeight, mbRadarImage and mbRadarHeight in tr_mtl.m): the texels the UI's own picture would have
// (RM_Paint in cl_uicomp.cpp), byte for byte, so a storey's change costs a number instead of a new picture.
//
// A texel's cell (texels x texels of the picture) shows its floor nearest the height, the first of equals in the list's
// order: its inside its shade, its sides with a stair a stripe, brighter the nearer the floor is to the height (no
// outlines: a wall is the shade's edge); no floor: clear, its colour kept. The floors' edge against no floor is smoothed
// as marching squares draws it: a floor's corner with no floor on either side or across it cut, a corner without a
// floor between floors on both sides filled with their mean, each a triangle of half a cell's sides, a texel's alpha
// its floor's times the share of it covered, in exact eighths. The shapes don't change with the height, only the
// shades. A whole picture: a thread a texel. A height's change: a thread a cell of those that can change (the list the
// UI made: a floor's, or one without a floor beside a cell of more than one), writing only its texels that do.
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
	uchar	sides;		// bits 4-7: a stair toward the left, right, north, south neighbour; 0-3: its cell's corners cut
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

// a cell without a floor at x, y: its corners q (bit 0 east, bit 1 south) where floors on both sides of it fill it,
// their alphas summed, else 0; for height z (now) and zOld (was). The four cells beside it read at once (outside the
// picture: none)
static void mb_radar_fills( device const uint *cellFloors, device const MbRadarFloor *floors, constant MbRadarParams &p,
	int x, int y, float z, float zOld, thread uint4 &now, thread uint4 &was )
{
	const int4 sx = int4( x - 1, x + 1, x, x ), sy = int4( y, y, y + 1, y - 1 );	// west, east, north, south
	const bool4 in = sx >= 0 && sy >= 0 && sx < int( p.cw ) && sy < int( p.ch );
	const uint4 c = select( uint4( 0u ), uint4( sy ) * p.cw + uint4( sx ), in );
	const uint4 first = uint4( cellFloors[c.x], cellFloors[c.y], cellFloors[c.z], cellFloors[c.w] );
	const uint4 last = select( first, uint4( cellFloors[c.x + 1], cellFloors[c.y + 1], cellFloors[c.z + 1], cellFloors[c.w + 1] ), in );
	const bool4 floor = last != first;
	int4 a = int4( 0 ), b = int4( 0 );

	for ( uint k = 0; k < 4; k++ ) {
		if ( floor[k] ) {
			a[k] = floors[mb_radar_nearest( floors, first[k], last[k], z )].alpha;
			b[k] = zOld == z ? a[k] : floors[mb_radar_nearest( floors, first[k], last[k], zOld )].alpha;
		}
	}
	const bool4 filled = floor.xyxy && floor.zzww;

	now = select( uint4( 0u ), uint4( a.xyxy + a.zzww ), filled );
	was = select( uint4( 0u ), uint4( b.xyxy + b.zzww ), filled );
}

// a floor's corners q (bit 0 east, bit 1 south): 1 where its cell's are cut (sides bits 0-3, RM_Floors: no floor on
// either side nor across), else 0
static uint4 mb_radar_cuts( MbRadarLook l )
{
	return ( uint4( l.sides ) >> uint4( 0u, 1u, 2u, 3u ) ) & 1u;
}

// the eighths of a cell's texel i, j under its corner q's triangle (half the cell's sides, its edge at 45 degrees): by
// its reach, the cell's texels across less twice the texel's steps from the corner, 0 1 4 7 8 for 0 to 4 and on
static uint mb_radar_under( uint q, uint i, uint j, uint t )
{
	const int reach = int( t ) - 2 * int( ( ( q & 1u ) ? t - 1u - i : i ) + ( ( q & 2u ) ? t - 1u - j : j ) );

	return uint( clamp( 3 * reach - 2, 0, 8 ) );
}

// a texel's alpha from its cell's corners: a floor's (its alpha less the cut corners' share), or without one the
// filled corners' share of their floors' mean
static uint mb_radar_cover( uint4 corner, uint i, uint j, uint t, bool floor, uint alpha )
{
	const uint covered = mb_radar_under( 0u, i, j, t ) * corner.x + mb_radar_under( 1u, i, j, t ) * corner.y
		+ mb_radar_under( 2u, i, j, t ) * corner.z + mb_radar_under( 3u, i, j, t ) * corner.w;

	return floor ? ( alpha * ( 8u - covered ) + 4u ) >> 3 : ( covered + 8u ) >> 4;
}

// the whole picture for height z: a thread a texel (the picture's top row is the map's north). Without a floor, only
// a cell with one to its west or east can have a filled corner: those beside it read first (in its own cache line)
kernel void mb_radar_full_cs( device const uint *cellFloors [[buffer(0)]],
	device const MbRadarFloor *floors [[buffer(1)]],
	constant MbRadarParams &p [[buffer(3)]],
	texture2d<float, access::write> picture [[texture(0)]],
	uint2 pos [[thread_position_in_grid]] )
{
	const uint t = p.texels, i = pos.x % t, j = pos.y % t;
	const int x = int( pos.x / t ), y = int( p.ch - 1u - pos.y / t );
	const uint c = uint( y ) * p.cw + uint( x ), first = cellFloors[c], last = cellFloors[c + 1];
	const uint west = cellFloors[x > 0 ? c - 1u : c], east = cellFloors[x + 1 < int( p.cw ) ? c + 2u : c + 1u];	// (read at once)

	if ( first == last ) {
		if ( west == first && east == last ) {
			picture.write( mb_radar_texel( 0u ), pos );
			return;
		}
		uint4 fill, same;

		mb_radar_fills( cellFloors, floors, p, x, y, p.z, p.z, fill, same );
		picture.write( mb_radar_texel( mb_radar_cover( fill, i, j, t, false, 0u ) ), pos );
		return;
	}
	const MbRadarLook l = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.z )], p.z );

	if ( l.sides & 15u ) {
		picture.write( mb_radar_texel( mb_radar_cover( mb_radar_cuts( l ), i, j, t, true, mb_radar_alpha( l, i, j, t ) ) ), pos );
		return;
	}
	picture.write( mb_radar_texel( mb_radar_alpha( l, i, j, t ) ), pos );
}

// a height's change from zOld to z: a thread a cell that can change (a floor's, or one without a floor whose filled
// corners can), its texels that do
kernel void mb_radar_cs( device const uint *cellFloors [[buffer(0)]],
	device const MbRadarFloor *floors [[buffer(1)]],
	device const uint *changing [[buffer(2)]],
	constant MbRadarParams &p [[buffer(3)]],
	texture2d<float, access::write> picture [[texture(0)]],
	uint id [[thread_position_in_grid]] )
{
	const uint c = changing[id], t = p.texels, first = cellFloors[c], last = cellFloors[c + 1];
	const int x = int( c % p.cw ), y = int( c / p.cw );
	const uint2 origin = uint2( uint( x ) * t, ( p.ch - 1u - uint( y ) ) * t );

	if ( first == last ) {
		uint4 fill, was;

		mb_radar_fills( cellFloors, floors, p, x, y, p.z, p.zOld, fill, was );
		if ( all( fill == was ) ) {
			return;	// (its texels the same)
		}
		for ( uint j = 0; j < t; j++ ) {
			for ( uint i = 0; i < t; i++ ) {
				const uint alpha = mb_radar_cover( fill, i, j, t, false, 0u );

				if ( alpha != mb_radar_cover( was, i, j, t, false, 0u ) ) {
					picture.write( mb_radar_texel( alpha ), origin + uint2( i, j ) );
				}
			}
		}
		return;
	}
	const MbRadarLook l = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.z )], p.z );
	const MbRadarLook was = mb_radar_look( floors[mb_radar_nearest( floors, first, last, p.zOld )], p.zOld );

	if ( l.alpha == was.alpha && l.sides == was.sides && ( !( l.sides & 240u ) || l.stair == was.stair ) ) {
		return;	// (its texels the same)
	}
	const uint4 cut = mb_radar_cuts( l );	// (the same at every height)

	if ( !any( cut != 0u ) ) {
		for ( uint j = 0; j < t; j++ ) {
			for ( uint i = 0; i < t; i++ ) {
				const uint alpha = mb_radar_alpha( l, i, j, t );

				if ( alpha != mb_radar_alpha( was, i, j, t ) ) {
					picture.write( mb_radar_texel( alpha ), origin + uint2( i, j ) );
				}
			}
		}
		return;
	}
	for ( uint j = 0; j < t; j++ ) {
		for ( uint i = 0; i < t; i++ ) {
			const uint alpha = mb_radar_cover( cut, i, j, t, true, mb_radar_alpha( l, i, j, t ) );

			if ( alpha != mb_radar_cover( cut, i, j, t, true, mb_radar_alpha( was, i, j, t ) ) ) {
				picture.write( mb_radar_texel( alpha ), origin + uint2( i, j ) );
			}
		}
	}
}
