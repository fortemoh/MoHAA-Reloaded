// radartest.mm -- the Competitive HUD radar's picture: the GPU's (code/renderer_metal/shaders/tr_mtl_radar.metal, its two
// kernels dispatched as MB_RadarPaint in tr_mtl.m dispatches them) against the UI's own (RM_Paint in
// code/client/cl_uicomp.cpp), byte for byte. port/radartest.sh takes RM_Paint, the floor list's types (tr_public.h),
// Com_Clamp (q_shared.c) and the kernels' parameters (tr_mtl.m) out of the game's source into radartest_src.inc and
// builds this with the game's floating-point flags, so a change to one side alone shows here. Tests:
//   alpha                 every float height distance in [60, 125] by every shade 0-255: a stair's alpha (pictures of a
//                         floor a cell and a texel a cell, painted whole: 2.3 billion cells)
//   shapes                the smoothed edge against no floor: every 3 x 3 pattern of cells at 1-6 texels a cell, each
//                         texel against the marching squares contour drawn independently (sampled), and the GPU's
//   lists [n]             n made-up floor lists (seeded: 1 to 6 texels a cell, up to 5 floors a cell, equal distances,
//                         every stair bit): the whole picture at a height, then one height's change after another (the
//                         cells that can change), each picture against RM_Paint's
//   real file [heights]   a floor list the game gave (port/radartest.sh game's dump: 8 ints, then its memory), at every
//                         texel count it can have, along the heights (a file of numbers, else made up from its floors)
//   bench file [heights [texels]]  the GPU's time of the whole picture and of each height's change, warm (timestamps
//                         at the compute encoder's start and end), at the dump's texel count or the one given
// Exit status 0: every byte the same.

#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <vector>

typedef unsigned char byte;
#define Q_min( a, b ) ( ( a ) < ( b ) ? ( a ) : ( b ) )	// (q_shared.h's)

#include "radartest_src.inc"

#define PAGE 16384

static id<MTLDevice>				dev;
static id<MTLCommandQueue>			queue;
static id<MTLComputePipelineState>	fullPipe, partPipe;

static uint32_t Bits( float f ) { uint32_t u; memcpy( &u, &f, 4 ); return u; }
static float Flt( uint32_t u ) { float f; memcpy( &f, &u, 4 ); return f; }

// a floor list in one page-aligned allocation as RM_Floors lays it out (the cells' floor lists, the floors at a
// 16-byte boundary, the changing cells: room for every cell), its buffer over it as the renderer's (no copy)
struct Arena
{
	void			*memory = nullptr;
	size_t			bytes = 0;
	radar2D_t		r = {};
	id<MTLBuffer>	buffer;

	Arena( int cw, int ch, int texels, int numFloors, const void *from = nullptr, size_t fromBytes = 0, size_t floorsAt = 0,
		size_t changingAt = 0 )
	{
		const int cells = cw * ch;

		if ( !from ) {
			floorsAt = ( (size_t)( cells + 1 ) * sizeof( unsigned int ) + 15 ) & ~(size_t)15;
			changingAt = floorsAt + (size_t)numFloors * sizeof( radarFloor_t );
			bytes = ( changingAt + (size_t)cells * sizeof( unsigned int ) + PAGE - 1 ) & ~(size_t)( PAGE - 1 );
		} else {
			bytes = ( std::max( fromBytes, changingAt + (size_t)cells * sizeof( unsigned int ) ) + PAGE - 1 ) & ~(size_t)( PAGE - 1 );
		}
		if ( posix_memalign( &memory, PAGE, bytes ) ) {
			fprintf( stderr, "radartest: out of memory\n" );
			exit( 2 );
		}
		memset( memory, 0, bytes );
		if ( from ) {
			memcpy( memory, from, fromBytes );
		}
		r.cw = cw;
		r.ch = ch;
		r.texels = texels;
		r.cellFloors = (const unsigned int *)memory;
		r.floors = (const radarFloor_t *)( (byte *)memory + floorsAt );
		r.changing = (const unsigned int *)( (byte *)memory + changingAt );
		r.numFloors = numFloors;
		r.memory = memory;
		r.memoryBytes = (int)bytes;
		buffer = [dev newBufferWithBytesNoCopy:memory length:bytes options:MTLResourceStorageModeShared deallocator:nil];
	}
	~Arena()
	{
		buffer = nil;
		free( memory );
	}
	unsigned int *CellFloors() { return (unsigned int *)r.cellFloors; }
	radarFloor_t *Floors() { return (radarFloor_t *)r.floors; }
	unsigned int *Changing() { return (unsigned int *)r.changing; }
	NSUInteger FloorsAt() const { return (NSUInteger)( (const byte *)r.floors - (const byte *)memory ); }
	NSUInteger ChangingAt() const { return (NSUInteger)( (const byte *)r.changing - (const byte *)memory ); }
	// cell x, y's floors (outside the picture none)
	int Count( int x, int y ) const
	{
		return x < 0 || y < 0 || x >= r.cw || y >= r.ch ? 0 : (int)( r.cellFloors[y * r.cw + x + 1] - r.cellFloors[y * r.cw + x] );
	}
	// the floors' cut corners (sides bits 0-3) by RM_Floors' rule (no floor on either side of a corner nor across
	// it), their stairs kept; true when every one already was
	bool SetCuts()
	{
		bool same = true;

		for ( int c = 0; c < r.cw * r.ch; c++ ) {
			const int x = c % r.cw, y = c / r.cw;
			int cuts = 0;

			for ( int q = 0; q < 4; q++ ) {
				const int sx = x + ( ( q & 1 ) ? 1 : -1 ), sy = y + ( ( q & 2 ) ? -1 : 1 );

				cuts |= ( !Count( sx, y ) && !Count( x, sy ) && !Count( sx, sy ) ) << q;
			}
			for ( unsigned int n = r.cellFloors[c]; n < r.cellFloors[c + 1]; n++ ) {
				same &= ( Floors()[n].sides & 15 ) == cuts;
				Floors()[n].sides = (byte)( ( Floors()[n].sides & 0xf0 ) | cuts );
			}
		}
		return same;
	}
	// the cells a height can change, by RM_Floors' rule (more than one floor, or a stair; then those without a floor
	// with a corner filled, floors on both sides of it, by a cell of more than one), into list
	int ListChanging( unsigned int *list ) const
	{
		int m = 0;

		for ( int c = 0; c < r.cw * r.ch; c++ ) {
			bool can = Count( c % r.cw, c / r.cw ) > 1;

			for ( unsigned int n = r.cellFloors[c]; n < r.cellFloors[c + 1]; n++ ) {
				can |= ( r.floors[n].sides & 0xf0 ) != 0;
			}
			if ( can ) {
				list[m++] = (unsigned int)c;
			}
		}
		for ( int c = 0; c < r.cw * r.ch; c++ ) {
			const int x = c % r.cw, y = c / r.cw;
			bool can = false;

			for ( int q = 0; q < 4 && !Count( x, y ) && !can; q++ ) {
				const int h = Count( x + ( ( q & 1 ) ? 1 : -1 ), y ), v = Count( x, y + ( ( q & 2 ) ? -1 : 1 ) );

				can = h && v && ( h > 1 || v > 1 );
			}
			if ( can ) {
				list[m++] = (unsigned int)c;
			}
		}
		return m;
	}
	void ListChanging() { r.numChanging = ListChanging( Changing() ); }
};

// the radar's texture as mbRadarImage makes it
static id<MTLTexture> Picture( const radar2D_t *r )
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:(NSUInteger)( r->cw * r->texels ) height:(NSUInteger)( r->ch * r->texels ) mipmapped:NO];

	desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	desc.storageMode = MTLStorageModePrivate;
	return [dev newTextureWithDescriptor:desc];
}

// MB_RadarPaint's dispatch: the whole picture a thread a texel, a height's change a thread a cell that can change
static void Encode( id<MTLComputeCommandEncoder> enc, Arena *a, id<MTLTexture> tex, float z, float zOld, bool full )
{
	const mbRadarParams_t params = { (uint32_t)a->r.cw, (uint32_t)a->r.ch, (uint32_t)a->r.texels, z, zOld, { 0, 0, 0 } };

	[enc setComputePipelineState:full ? fullPipe : partPipe];
	[enc setBuffer:a->buffer offset:0 atIndex:0];
	[enc setBuffer:a->buffer offset:a->FloorsAt() atIndex:1];
	[enc setBuffer:a->buffer offset:a->ChangingAt() atIndex:2];
	[enc setBytes:&params length:sizeof( params ) atIndex:3];
	[enc setTexture:tex atIndex:0];
	if ( full ) {
		[enc dispatchThreads:MTLSizeMake( tex.width, tex.height, 1 ) threadsPerThreadgroup:MTLSizeMake( 16, 4, 1 )];
	} else {
		[enc dispatchThreads:MTLSizeMake( (NSUInteger)a->r.numChanging, 1, 1 ) threadsPerThreadgroup:MTLSizeMake( 64, 1, 1 )];
	}
}

// a paint (a height's change only when the height changed and a cell can: as mbRadarHeight), its texels read back
static void Paint( Arena *a, id<MTLTexture> tex, float z, float zOld, bool full, id<MTLBuffer> readback )
{
	@autoreleasepool {
		id<MTLCommandBuffer> cb = [queue commandBuffer];

		if ( full || ( z != zOld && a->r.numChanging ) ) {
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

			Encode( enc, a, tex, z, zOld, full );
			[enc endEncoding];
		}
		if ( readback ) {
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

			[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake( 0, 0, 0 )
				sourceSize:MTLSizeMake( tex.width, tex.height, 1 ) toBuffer:readback destinationOffset:0
				destinationBytesPerRow:tex.width * 4 destinationBytesPerImage:tex.width * tex.height * 4];
			[blit endEncoding];
		}
		[cb commit];
		[cb waitUntilCompleted];
	}
}

static id<MTLBuffer> Readback( const radar2D_t *r )
{
	return [dev newBufferWithLength:(NSUInteger)r->cw * r->texels * r->ch * r->texels * 4 options:MTLResourceStorageModeShared];
}

// the GPU's texels against RM_Paint's for height z; the first difference told
static long long Compare( const radar2D_t *r, id<MTLBuffer> gpu, float z, const char *what )
{
	std::vector<byte> cpu;
	const byte *g = (const byte *)gpu.contents;
	long long bad = 0, first = -1;

	RM_Paint( r, z, &cpu );
	if ( cpu.size() != gpu.length ) {
		printf( "  %s: RM_Paint's picture is %zu bytes, the GPU's %lu\n", what, cpu.size(), (unsigned long)gpu.length );
		return 1;
	}
	for ( size_t i = 0; i < cpu.size(); i++ ) {
		if ( g[i] != cpu[i] ) {
			if ( first < 0 ) {
				first = (long long)i;
			}
			bad++;
		}
	}
	if ( bad ) {
		const int w = r->cw * r->texels, t = r->texels;
		const long long px = first / 4, x = px % w, y = px / w;
		const int cx = (int)( x / t ), cy = r->ch - 1 - (int)( y / t ), c = cy * r->cw + cx;

		printf( "  %s z %.9g: %lld bytes differ; the first at texel %lld,%lld (cell %d,%d: texel %lld,%lld of it, %u floors, "
			"sides", what, z, bad, x, y, cx, cy, x % t, y % t, r->cellFloors[c + 1] - r->cellFloors[c] );
		for ( unsigned int n = r->cellFloors[c]; n < r->cellFloors[c + 1]; n++ ) {
			printf( " %02x@%.9g/%u", r->floors[n].sides, r->floors[n].z, r->floors[n].alpha );
		}
		printf( "): GPU %u %u %u %u, CPU %u %u %u %u\n", g[px * 4], g[px * 4 + 1], g[px * 4 + 2], g[px * 4 + 3], cpu[px * 4],
			cpu[px * 4 + 1], cpu[px * 4 + 2], cpu[px * 4 + 3] );
	}
	return bad;
}

/*
=============
alpha: every height distance d (each float in [60, 125], the focus's whole ramp, 64 to 120, and past it) by every
shade: cells of one floor at height d, painted for height 0, a texel a cell (its left side shown): a stair (sides 16)
of each shade. Workers paint batches of 1024 x 1024 cells at once
=============
*/
static int TestAlpha( void )
{
	const uint32_t first = Bits( 60.0f ), count = Bits( 125.0f ) - first + 1;
	const uint64_t total = (uint64_t)count * 256;	// per distance: a stair of shade 0-255
	const int side = 1024, cells = side * side;
	const long long batches = (long long)( ( total + cells - 1 ) / cells );
	const int workers = std::max( 1, (int)[NSProcessInfo processInfo].activeProcessorCount - 2 );
	std::atomic<long long> bad( 0 ), tested( 0 ), *badp = &bad, *testedp = &tested;	// (a block copies what it uses)

	printf( "alpha: %u distances x 256 stair shades = %llu cells, %lld pictures of %dx%d, %d workers\n", count,
		(unsigned long long)total, batches, side, side, workers );
	dispatch_apply( (size_t)workers, dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ), ^( size_t worker ) {
		Arena a( side, side, 1, cells );
		id<MTLTexture> tex = Picture( &a.r );
		id<MTLBuffer> readback = Readback( &a.r );

		for ( long long b = (long long)worker; b < batches; b += workers ) {
			unsigned int *cf = a.CellFloors();
			radarFloor_t *fl = a.Floors();
			int f = 0;

			for ( int c = 0; c < cells; c++ ) {
				const uint64_t k = (uint64_t)b * cells + c;

				cf[c] = (unsigned int)f;
				if ( k < total ) {
					const uint32_t di = (uint32_t)( k / 256 );
					const byte shade = (byte)( k % 256 );

					fl[f].z = Flt( first + di );
					fl[f].fill = shade / 255.0f;
					fl[f].alpha = shade;
					fl[f].sides = 16;
					fl[f].pad = 0;
					f++;
				}
			}
			cf[cells] = (unsigned int)f;
			a.r.numFloors = f;
			a.SetCuts();
			Paint( &a, tex, 0.0f, 0.0f, true, readback );
			const long long n = Compare( &a.r, readback, 0.0f, "alpha" );
			if ( n ) {
				*badp += n;
			}
			*testedp += f;
		}
	} );
	printf( "alpha: %lld cells tested, %lld bytes differ\n", tested.load(), bad.load() );
	return bad != 0 || tested.load() != (long long)total;
}

// splitmix64
struct Rng
{
	uint64_t s;
	uint32_t Next()
	{
		uint64_t z = ( s += 0x9e3779b97f4a7c15ull );
		z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
		z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebull;
		return (uint32_t)( ( z ^ ( z >> 31 ) ) >> 32 );
	}
	float Range( float lo, float hi ) { return lo + ( hi - lo ) * ( Next() / 4294967296.0f ); }
};

// along heights: the whole picture at the first, then each change (and every 8th a whole picture beside it), each
// (or every n-th, and the last) against RM_Paint
static long long Walk( Arena *a, const std::vector<float> &heights, const char *what, std::atomic<int> *pictures, size_t every = 1 )
{
	id<MTLTexture> tex = Picture( &a->r ), other = Picture( &a->r );
	id<MTLBuffer> readback = Readback( &a->r );
	float zOld = heights[0];
	long long bad = 0;

	Paint( a, tex, zOld, zOld, true, readback );
	bad += Compare( &a->r, readback, zOld, what );
	(*pictures)++;
	for ( size_t k = 1; k < heights.size() && bad < 1000; k++ ) {
		const bool compared = k % every == 0 || k == heights.size() - 1;

		Paint( a, tex, heights[k], zOld, false, compared ? readback : nil );
		if ( compared ) {
			bad += Compare( &a->r, readback, heights[k], what );
			(*pictures)++;
		}
		zOld = heights[k];
		if ( k % ( 8 * every ) == 0 ) {
			Paint( a, other, zOld, zOld, true, readback );
			bad += Compare( &a->r, readback, zOld, what );
			(*pictures)++;
		}
	}
	return bad;
}

/*
=============
shapes: the smoothed edge against an independent drawing of it. Every pattern of a cell and its eight neighbours (512,
each the middle of a block of 3 x 3 cells, the blocks side by side), one floor a cell of alpha 255 (RM_Paint's alpha
is then 255 times the share of the texel the shape covers): each texel against the marching squares contour (the
cells' middles a grid, a line between the middles of a square's sides where its corners differ, a saddle's middle
inside) sampled 64 x 64 a texel, within 4 (a wrong eighth is 32 off); at 1 to 6 texels a cell, the GPU's picture
against RM_Paint's too
=============
*/
static bool ShapeInside( const std::vector<int> &occupied, int cw, int ch, double x, double y )
{
	const double gx = x - 0.5, gy = y - 0.5;	// (the cells' middles on whole numbers)
	const int bx = (int)floor( gx ), by = (int)floor( gy );
	const double fx = gx - bx, fy = gy - by;
	auto at = [&]( int cx, int cy ) { return cx >= 0 && cy >= 0 && cx < cw && cy < ch && occupied[cy * cw + cx]; };
	const bool o00 = at( bx, by ), o10 = at( bx + 1, by ), o01 = at( bx, by + 1 ), o11 = at( bx + 1, by + 1 );
	const int n = o00 + o10 + o01 + o11;

	if ( n == 0 || n == 4 ) {
		return n == 4;
	}
	if ( n == 1 || n == 3 ) {	// the odd one out's corner: a triangle to the middles of its sides, inside for one floor
		const bool odd = n == 1;
		const int cx = o10 == odd || o11 == odd, cy = o01 == odd || o11 == odd;

		return ( fabs( fx - cx ) + fabs( fy - cy ) < 0.5 ) == odd;
	}
	if ( o00 == o10 ) {	// two beside each other: a straight line across the middle
		return o00 ? fy < 0.5 : fy > 0.5;
	}
	if ( o00 == o01 ) {
		return o00 ? fx < 0.5 : fx > 0.5;
	}
	return o00 ? fabs( fx - fy ) < 0.5 : fabs( fx + fy - 1.0 ) < 0.5;	// a saddle: its middle inside, a band
}

static int TestShapes( void )
{
	const int blocks = 512, across = 32, cw = across * 3, ch = blocks / across * 3;
	std::vector<int> occupied( (size_t)cw * ch );
	long long worst = 0, bad = 0, tested = 0;
	std::atomic<int> pictures( 0 );
	int numFloors = 0;

	for ( int b = 0; b < blocks; b++ ) {
		for ( int k = 0; k < 9; k++ ) {
			occupied[( b / across * 3 + k / 3 ) * cw + b % across * 3 + k % 3] = ( b >> k ) & 1;
		}
	}
	for ( int v : occupied ) {
		numFloors += v;
	}
	for ( int t = 1; t <= 6; t++ ) {
		Arena a( cw, ch, t, numFloors );
		std::vector<byte> cpuOf;
		std::vector<long long> rowWorst( (size_t)ch * t ), rowBad( (size_t)ch * t );
		long long *rw = rowWorst.data(), *rb = rowBad.data();	// (a block copies what it uses: pointers)
		const std::vector<int> *occ = &occupied;
		const byte *cpu;
		int f = 0;

		for ( int c = 0; c < cw * ch; c++ ) {
			a.CellFloors()[c] = (unsigned int)f;
			if ( occupied[c] ) {
				radarFloor_t *fl = &a.Floors()[f++];

				fl->z = 0.0f;
				fl->fill = 1.0f;
				fl->alpha = 255;
				fl->sides = 0;
				fl->pad = 0;
			}
		}
		a.CellFloors()[cw * ch] = (unsigned int)f;
		a.SetCuts();
		a.ListChanging();
		RM_Paint( &a.r, 0.0f, &cpuOf );
		cpu = cpuOf.data();
		dispatch_apply( (size_t)ch * t, dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ), ^( size_t row ) {
			const int S = 64;

			for ( int px = 0; px < cw * t; px++ ) {
				int inside = 0;

				for ( int sy = 0; sy < S; sy++ ) {
					for ( int sx = 0; sx < S; sx++ ) {
						// (cells, y up; the picture's top row the north; the samples a little off the texels' diagonals)
						const double x = ( px + ( sx + 0.37 ) / S ) / t, y = ch - ( row + ( sy + 0.61 ) / S ) / t;

						inside += ShapeInside( *occ, cw, ch, x, y );
					}
				}
				const long long off = llabs( (long long)cpu[( row * cw * t + px ) * 4 + 3] * S * S - 255LL * inside );	// (x S^2)

				rw[row] = std::max( rw[row], off );
				rb[row] += off > 4LL * S * S;
			}
		} );
		for ( size_t row = 0; row < rowWorst.size(); row++ ) {
			worst = std::max( worst, rowWorst[row] );
			bad += rowBad[row];
		}
		tested += (long long)cw * t * ch * t;
		bad += Walk( &a, { 0.0f }, "shapes", &pictures ) != 0;
	}
	printf( "shapes: 512 patterns at 1-6 texels a cell, %lld texels against the contour sampled 64 x 64 (worst %.2f of 255 "
		"off), %d GPU pictures against RM_Paint: %lld differ\n", tested, worst / 4096.0, pictures.load(), bad );
	return bad != 0;
}

/*
=============
lists: made-up floor lists: storeys with floors on whole multiples of 8 (equal distances: the first of equals) and
anywhere, every side bit, 0 to 5 floors a cell, 1 to 6 texels a cell (a picture at most 4096 across, as RM_Texels
keeps it), heights at floors, past them, halfway between and anywhere
=============
*/
static int TestLists( int lists )
{
	static const float storeys[] = { -320.0f, -128.0f, 0.0f, 64.0f, 136.0f, 200.0f, 384.0f };
	Rng rng = { 93 };
	long long bad = 0;
	std::atomic<int> pictures( 0 );
	int floorsAll = 0;

	for ( int l = 0; l < lists; l++ ) {
		const int texels = 1 + l % 6;
		const int cw = l == 0 ? 1 : l == 1 ? 3 : l == lists - 1 ? 4096 / 6 : 1 + (int)( rng.Next() % 160 );
		const int ch = l == 0 ? 1 : l == 1 ? 1 : l == lists - 1 ? 500 : 1 + (int)( rng.Next() % 160 );
		std::vector<int> counts( (size_t)cw * ch );
		int numFloors = 0;

		for ( int &n : counts ) {
			const uint32_t k = rng.Next() % 10;

			n = k < 3 ? 0 : k < 7 ? 1 : k == 7 ? 2 : k == 8 ? 3 : 4 + (int)( rng.Next() % 2 );
			numFloors += n;
		}
		Arena a( cw, ch, texels, numFloors );
		std::vector<float> zs, heights;
		int f = 0;

		for ( int c = 0; c < cw * ch; c++ ) {
			a.CellFloors()[c] = (unsigned int)f;
			for ( int n = 0; n < counts[c]; n++, f++ ) {
				radarFloor_t *fl = &a.Floors()[f];
				const byte shade = (byte)( rng.Next() % 256 );

				fl->z = rng.Next() % 2 ? storeys[rng.Next() % 7] + 8.0f * ( rng.Next() % 5 ) : rng.Range( -400.0f, 500.0f );
				fl->fill = shade / 255.0f;
				fl->alpha = shade;
				fl->sides = rng.Next() % 2 ? (byte)( rng.Next() & 0xf0 ) : 0;
				fl->pad = 0;
				zs.push_back( fl->z );
			}
		}
		a.CellFloors()[cw * ch] = (unsigned int)f;
		a.SetCuts();
		a.ListChanging();
		floorsAll += numFloors;
		if ( zs.empty() ) {
			zs.push_back( 0.0f );
		}
		for ( int k = 0; k < 40; k++ ) {
			const float at = zs[rng.Next() % zs.size()];
			static const float off[] = { 0.0f, 24.0f, -24.0f, 56.0f, 64.0f, -64.0f, 120.0f, 121.0f, 400.0f };

			switch ( rng.Next() % 5 ) {
			case 0: heights.push_back( at ); break;
			case 1: heights.push_back( at + off[rng.Next() % 9] ); break;
			case 2: heights.push_back( ( storeys[rng.Next() % 7] + storeys[rng.Next() % 7] ) * 0.5f ); break;
			case 3: heights.push_back( rng.Range( -500.0f, 600.0f ) ); break;
			default: heights.push_back( heights.empty() ? at : heights.back() ); break;	// (no change)
			}
		}
		bad += Walk( &a, heights, "lists", &pictures );
	}
	printf( "lists: %d floor lists, %d floors, %d pictures compared, %lld bytes differ\n", lists, floorsAll, pictures.load(), bad );
	return bad != 0;
}

// a floor list the game gave: 8 ints (cw, ch, texels, floors, changing, the floors' and the changing's offsets,
// bytes), then its memory
static bool Load( const char *path, int head[8], std::vector<byte> *memory )
{
	FILE *f = fopen( path, "rb" );
	bool ok = f && fread( head, sizeof( int ), 8, f ) == 8 && head[7] > 0;

	if ( ok ) {
		memory->resize( (size_t)head[7] );
		ok = fread( memory->data(), 1, memory->size(), f ) == memory->size();
	}
	if ( f ) {
		fclose( f );
	}
	if ( !ok ) {
		printf( "radartest: can't read %s\n", path );
	}
	return ok;
}

static std::vector<float> Heights( const char *path, const radar2D_t *r )
{
	std::vector<float> h;
	FILE *f = path ? fopen( path, "r" ) : NULL;
	float z;

	if ( f ) {
		while ( fscanf( f, "%f", &z ) == 1 ) {
			h.push_back( z );
		}
		fclose( f );
	}
	if ( h.empty() ) {
		// across its floors: each 7th floor's height, then 24 above it
		for ( int n = 0; n < r->numFloors && h.size() < 300; n += std::max( 1, r->numFloors / 150 ) ) {
			h.push_back( r->floors[n].z );
			h.push_back( r->floors[n].z + 24.0f );
		}
	}
	return h;
}

// the dump's cut corners and changing cells against RM_Floors' rule (SetCuts, ListChanging): the same, else told (a
// dump of an older build: the rule's used)
static bool SameRule( Arena *a, int numChanging, const char *path )
{
	std::vector<unsigned int> rule( (size_t)a->r.cw * a->r.ch );
	const bool cuts = a->SetCuts();
	const int m = a->ListChanging( rule.data() );
	const bool same = m == numChanging && !memcmp( rule.data(), a->r.changing, (size_t)m * sizeof( unsigned int ) );

	if ( ( !same || !cuts ) && path ) {
		printf( "  %s: its %d changing cells %s RM_Floors' rule's %d, its cut corners %s (an older dump? port/radartest.sh "
			"game makes new ones): the rule's used\n", path, numChanging, same ? "are" : "aren't", m, cuts ? "are" : "aren't" );
	}
	if ( !same ) {
		memcpy( a->Changing(), rule.data(), (size_t)m * sizeof( unsigned int ) );
	}
	a->r.numChanging = m;
	return same && cuts;
}

// at its own texel count every height compared, at the others (a thread each) every 4th and the last
static int TestReal( const char *path, const char *heightsPath )
{
	int headOf[8], *head = headOf;	// (a block copies what it uses: a pointer)
	std::vector<byte> memoryOf, *memory = &memoryOf;
	std::atomic<long long> bad( 0 ), *badp = &bad;
	std::atomic<int> pictures( 0 ), *picturesp = &pictures;
	std::atomic<bool> same( true ), *samep = &same;
	int most;

	if ( !Load( path, head, memory ) ) {
		return 1;
	}
	most = std::min( 6, 4096 / std::max( head[0], head[1] ) );
	dispatch_apply( (size_t)most, dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ), ^( size_t i ) {
		const int t = (int)i + 1;
		Arena a( head[0], head[1], t, head[3], memory->data(), memory->size(), (size_t)head[5], (size_t)head[6] );
		const std::vector<float> heights = Heights( heightsPath, &a.r );

		if ( !SameRule( &a, head[4], t == 1 ? path : NULL ) ) {
			*samep = false;
		}
		*badp += Walk( &a, heights, "real", picturesp, t == head[2] ? 1 : 4 );
	} );
	printf( "real %s: %dx%d cells, %d floors, %d changing%s, texels 1-%d (%d its own): %d pictures compared, %lld bytes differ\n",
		path, head[0], head[1], head[3], head[4], same ? "" : " (not the rule's: the rule's used)", most, head[2], pictures.load(), bad.load() );
	return bad != 0 || !same;
}

static int CompareDoubles( const void *a, const void *b )
{
	const double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void Stats( const char *what, std::vector<double> v )
{
	double sum = 0.0;

	for ( double x : v ) {
		sum += x;
	}
	qsort( v.data(), v.size(), sizeof( double ), CompareDoubles );
	printf( "  %-30s n %4zu  mean %7.1f  median %7.1f  p95 %7.1f  max %7.1f us\n", what, v.size(), sum / v.size(),
		v[v.size() / 2], v[(size_t)( v.size() * 0.95 )], v.back() );
}

static int Bench( const char *path, const char *heightsPath, int texels )
{
	int head[8];
	std::vector<byte> memory;
	MTLCounterSampleBufferDescriptor *desc = [MTLCounterSampleBufferDescriptor new];
	id<MTLCounterSampleBuffer> samples;
	std::vector<double> full, part;

	if ( !Load( path, head, &memory ) ) {
		return 1;
	}
	for ( id<MTLCounterSet> set in dev.counterSets ) {
		if ( [set.name isEqualToString:MTLCommonCounterSetTimestamp] ) {
			desc.counterSet = set;
		}
	}
	desc.storageMode = MTLStorageModeShared;
	desc.sampleCount = 2;
	samples = desc.counterSet ? [dev newCounterSampleBufferWithDescriptor:desc error:nil] : nil;
	if ( !samples ) {
		printf( "bench: no GPU timestamps\n" );
		return 1;
	}
	Arena a( head[0], head[1], texels > 0 ? texels : head[2], head[3], memory.data(), memory.size(), (size_t)head[5], (size_t)head[6] );
	const std::vector<float> heights = Heights( heightsPath, &a.r );
	id<MTLTexture> tex = Picture( &a.r );
	auto timed = [&]( float z, float zOld, bool isFull ) {
		@autoreleasepool {
			MTLComputePassDescriptor *pass = [MTLComputePassDescriptor computePassDescriptor];
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLComputeCommandEncoder> enc;
			const MTLCounterResultTimestamp *ts;

			pass.sampleBufferAttachments[0].sampleBuffer = samples;
			pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
			pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
			enc = [cb computeCommandEncoderWithDescriptor:pass];
			Encode( enc, &a, tex, z, zOld, isFull );
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			ts = (const MTLCounterResultTimestamp *)[samples resolveCounterRange:NSMakeRange( 0, 2 )].bytes;
			return ( ts[1].timestamp - ts[0].timestamp ) / 1000.0;	// (nanoseconds on Apple GPUs)
		}
	};

	SameRule( &a, head[4], path );
	printf( "bench %s: %dx%d cells x %d texels = %lux%lu, %d floors, %d changing, %zu heights\n", path, head[0], head[1],
		a.r.texels, (unsigned long)tex.width, (unsigned long)tex.height, head[3], a.r.numChanging, heights.size() );
	for ( int i = 0; i < 200; i++ ) {
		timed( heights[0], heights[0], true );	// (the GPU clocked up)
	}
	for ( int i = 0; i < 50; i++ ) {
		full.push_back( timed( heights[0], heights[0], true ) );
	}
	for ( size_t k = 1; k < heights.size(); k++ ) {
		if ( heights[k] != heights[k - 1] ) {
			part.push_back( timed( heights[k], heights[k - 1], false ) );
		}
	}
	Stats( "the whole picture", full );
	Stats( "a height's change", part );
	return 0;
}

int main( int argc, char **argv )
{
	@autoreleasepool {
		NSError *error = nil;
		const char *mode = argc > 2 ? argv[2] : "";
		id<MTLLibrary> library;

		dev = MTLCreateSystemDefaultDevice();
		queue = [dev newCommandQueue];
		library = argc > 1 ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@( argv[1] )] error:&error] : nil;
		if ( library ) {
			id<MTLFunction> full = [library newFunctionWithName:@"mb_radar_full_cs"], cells = [library newFunctionWithName:@"mb_radar_cs"];

			fullPipe = full ? [dev newComputePipelineStateWithFunction:full error:&error] : nil;
			partPipe = cells ? [dev newComputePipelineStateWithFunction:cells error:&error] : nil;
		}
		if ( !fullPipe || !partPipe ) {
			fprintf( stderr, "radartest: no radar kernels in %s%s%s\n", argc > 1 ? argv[1] : "(no library given)", error ? ": " : "",
				error ? error.localizedDescription.UTF8String : "" );
			return 2;
		}
		if ( !strcmp( mode, "alpha" ) ) {
			return TestAlpha();
		}
		if ( !strcmp( mode, "shapes" ) ) {
			return TestShapes();
		}
		if ( !strcmp( mode, "lists" ) ) {
			return TestLists( argc > 3 ? atoi( argv[3] ) : 60 );
		}
		if ( !strcmp( mode, "real" ) && argc > 3 ) {
			return TestReal( argv[3], argc > 4 ? argv[4] : NULL );
		}
		if ( !strcmp( mode, "bench" ) && argc > 3 ) {
			return Bench( argv[3], argc > 4 ? argv[4] : NULL, argc > 5 ? atoi( argv[5] ) : 0 );
		}
		fprintf( stderr, "usage: radartest <metallib> alpha | shapes | lists [n] | real <floors.bin> [heights] | bench <floors.bin> [heights [texels]]\n" );
		return 2;
	}
}
