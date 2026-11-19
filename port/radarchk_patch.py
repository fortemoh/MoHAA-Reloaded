#!/usr/bin/env python3
# port/radartest.sh build: the check flavor radarchk. Applied to an export of the work tree (build.noindex/radarchk-src),
# never committed into the game. With +set radarchk 1, every picture the renderer makes of the radar (the floors
# given, each height's change) is read back from the GPU and compared with RM_Paint's from a copy of the same floors:
#   RADARCHK check <n> <register|height> z <z> <w>x<h>: differ <bytes>
# With +set radarchk_dump 1, each floor list given is written to $OMM_BUILD/radartest/floors-<cw>x<ch>.bin (8 ints:
# cw, ch, texels, floors, changing, the floors' and the changing's offsets, bytes; then its memory) and every height
# painted for a map of that size added to floors-<cw>x<ch>.heights (port/radartest.sh real and bench read them).
# (Session 89's check, build.noindex/s89/inst89.py, and session 92's copy, build.noindex/s92/inst89m.py.)
import sys, pathlib

src = pathlib.Path(sys.argv[1])

def patch(rel, anchor, text, before=False):
    p = src / rel
    s = p.read_text()
    n = s.count(anchor)
    if n != 1:
        sys.exit(f"radarchk_patch: {rel}: anchor found {n} times, want 1: {anchor!r}")
    p.write_text(s.replace(anchor, (text + anchor) if before else (anchor + text)))

CHECK = r'''
extern "C" int RADARCHK_Read(unsigned char *out, int w, int h);
static std::vector<byte> radarchk_mem;
static radar2D_t         radarchk_floors;

// a copy of the floors given (the renderer's from then on)
static void RADARCHK_Keep(const radar2D_t *r)
{
    const byte *m = (const byte *)r->memory;

    radarchk_mem.assign(m, m + r->memoryBytes);
    radarchk_floors            = *r;
    radarchk_floors.cellFloors = (const unsigned int *)(radarchk_mem.data() + ((const byte *)r->cellFloors - m));
    radarchk_floors.floors     = (const radarFloor_t *)(radarchk_mem.data() + ((const byte *)r->floors - m));
    radarchk_floors.changing   = (const unsigned int *)(radarchk_mem.data() + ((const byte *)r->changing - m));
    radarchk_floors.memory     = radarchk_mem.data();
    if (Cvar_VariableIntegerValue("radarchk_dump") && getenv("OMM_BUILD")) {
        FILE *f = fopen(va("%s/radartest/floors-%dx%d.bin", getenv("OMM_BUILD"), r->cw, r->ch), "wb");

        if (f) {
            const int head[8] = {r->cw, r->ch, r->texels, r->numFloors, r->numChanging, (int)((const byte *)r->floors - m),
                                 (int)((const byte *)r->changing - m), r->memoryBytes};

            fwrite(head, sizeof(head), 1, f);
            fwrite(m, r->memoryBytes, 1, f);
            fclose(f);
        }
    }
}

static void RADARCHK_Check(float z, const char *what)
{
    static int  checks;
    static char started[64][32];
    static int  numStarted;

    if (!Cvar_VariableIntegerValue("radarchk") || !radarchk_floors.memory) {
        return;
    }
    const int         t = radarchk_floors.texels, w = radarchk_floors.cw * t, h = radarchk_floors.ch * t;
    std::vector<byte> gpu((size_t)w * h * 4), cpu;
    long              differ = 0;

    if (Cvar_VariableIntegerValue("radarchk_dump") && getenv("OMM_BUILD")) {
        // the heights painted, a file a map size (begun anew each run)
        const char *name = va("%dx%d", radarchk_floors.cw, radarchk_floors.ch);
        int         k    = 0;
        FILE       *f;

        while (k < numStarted && strcmp(started[k], name)) {
            k++;
        }
        f = fopen(va("%s/radartest/floors-%s.heights", getenv("OMM_BUILD"), name), k < numStarted ? "a" : "w");
        if (k == numStarted && numStarted < 64) {
            Q_strncpyz(started[numStarted++], name, sizeof(started[0]));
        }
        if (f) {
            fprintf(f, "%.9g\n", z);
            fclose(f);
        }
    }
    if (!RADARCHK_Read(gpu.data(), w, h)) {
        Com_Printf("RADARCHK check %d %s z %.2f: no readback\n", ++checks, what, z);
        return;
    }
    RM_Paint(&radarchk_floors, z, &cpu);
    for (size_t i = 0; i < gpu.size(); i++) {
        differ += gpu[i] != cpu[i];
    }
    Com_Printf("RADARCHK check %d %s z %.2f %dx%d: differ %ld\n", ++checks, what, z, w, h, differ);
}
'''
patch("code/client/cl_uicomp.cpp", "// the floors given to the renderer (its memory from now on): its picture for\n", CHECK, before=True)
patch("code/client/cl_uicomp.cpp", '        rm.shader = re.RegisterRadar2D("ui_bake/radar", &rm.floors, rm.focusZ);\n',
      '        RADARCHK_Keep(&rm.floors);\n', before=True)
patch("code/client/cl_uicomp.cpp", '        rm.shader = re.RegisterRadar2D("ui_bake/radar", &rm.floors, rm.focusZ);\n',
      '        if (rm.shader) {\n            RADARCHK_Check(rm.focusZ, "register");\n        }\n')
patch("code/client/cl_uicomp.cpp", "            rm.focusZ = focusZ;\n            re.Radar2DHeight(rm.shader, focusZ);\n",
      '            RADARCHK_Check(focusZ, "height");\n')

READ = r'''
// port/radartest.sh's check: the radar's texels read back (the uploads committed first, then waited for)
int RADARCHK_Read( unsigned char *out, int w, int h )
{
	mbTexture_t *t;

	if ( !mb_radar.name ) {
		return 0;
	}
	t = &mb_textures[mb_radar.name];
	if ( t->width != w || t->height != h ) {
		return 0;
	}
	MB_CommitUploads();
	@autoreleasepool {
		id<MTLBuffer> buffer = [mb_device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [mb_queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

		[blit copyFromTexture:MB_OBJ( t->texture ) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake( 0, 0, 0 )
			sourceSize:MTLSizeMake( w, h, 1 ) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4
			destinationBytesPerImage:(NSUInteger)w * h * 4];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		memcpy( out, buffer.contents, (size_t)w * h * 4 );
	}
	return 1;
}

'''
patch("code/renderer_metal/tr_mtl.m", "// its texels for height z: all of them (full: a thread a texel), else those of the cells that can change that do (a\n",
      READ, before=True)
print("radarchk_patch: patched", src)
