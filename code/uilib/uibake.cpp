/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// uibake.cpp: what the UI bakes into pictures (Added). TrueType faces become
// font pages the renderer draws as it draws a RitualFont's; the URC's shapes
// (circles, rings, frames, rounded boxes, polygons, gradients, text, PNG
// pictures) are rasterized into one image a widget, at the size drawn. Each
// is baked once (RegisterImage2D) and again only after the renderer restarts;
// the images outlive map loads, their shaders are looked up again by name.
// stb_truetype reads the faces and fills every outline, glyph or shape.

#include "ui_local.h"
#include "uibake.h"

#include <string>
#include <unordered_map>
#include <vector>

#define stbtt_vertex_type float
typedef struct {
    float         x, y, cx, cy;
    unsigned char type, padding;
} uiVertex_t;
#define stbtt_vertex uiVertex_t
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../thirdparty/stb/stb_truetype.h"

typedef struct {
    std::string    file;
    void          *data; // NULL: no such face
    stbtt_fontinfo info;
} uiFace_t;

typedef struct {
    uiFace_t *face;
    float     px; // design pixels
    qboolean  shadow, slant;
} uiFontSpec_t;

typedef struct {
    float scale;      // font units to pixels
    int   lineH;      // a line's pixels
    float base, capH; // the baseline from the line's top (the capitals' middle the line's), the capitals' height
} uiMetrics_t;

typedef struct {
    std::string      name;
    int              seq;
    float            pageScale; // the page's width over 256 (a texel a pixel at this scale)
    fontheader_t     header;
    fontheader_sgl_t sgl;
} uiBakedFont_t;

typedef struct {
    int       seq, gen;
    qhandle_t handle;
    char      image[32];
} uiBaked_t;

static std::vector<uiFace_t *>                    bake_faces;
static std::vector<uiBakedFont_t *>               bake_fonts;
static std::unordered_map<std::string, uiBaked_t> bake_shapes;
static std::vector<uiVertex_t>                    bake_path;
static int                                        bake_gen;

static const float SLANT = 0.21256f; // tan 12 degrees: CSS's text-skew -12deg

static float bake_designScale = 1.0f; // the design's scale times this (cg_hud_scale)
static int   bake_scaleGen;            // ... its changes

float UI_DesignScale(float w, float h)
{
    return Q_min(uid.vidWidth / w, uid.vidHeight / h) * bake_designScale;
}

bool UI_SetDesignScale(float scale)
{
    if (scale == bake_designScale) {
        return false;
    }
    bake_designScale = scale;
    bake_scaleGen++;
    bake_gen++;
    return true;
}

int UI_FontStamp(void)
{
    return uii.GetRefSequence() ^ (bake_scaleGen << 20);
}

void UI_BakeForget(void)
{
    bake_gen++;
}

int UI_BakeStamp(void)
{
    return (bake_gen << 16) ^ uii.GetRefSequence();
}

/*
====================================================================

FACES

====================================================================
*/

static uiFace_t *Bake_Face(const char *file)
{
    void     *buf;
    uiFace_t *f;

    for (uiFace_t *face : bake_faces) {
        if (!Q_stricmp(face->file.c_str(), file)) {
            return face->data ? face : NULL;
        }
    }
    f       = new uiFace_t();
    f->file = file;
    if (uii.File_OpenFile(va("fonts/%s.ttf", file), &buf) > 0) {
        if (stbtt_InitFont(&f->info, (unsigned char *)buf, 0)) {
            f->data = buf;
        } else {
            uii.File_FreeFile(buf);
        }
    }
    bake_faces.push_back(f);
    return f->data ? f : NULL;
}

// "<file>-<px>[s][i]"
static qboolean Bake_ParseFont(const char *name, uiFontSpec_t *spec)
{
    const char *dash = strrchr(name, '-');
    char        file[MAX_QPATH];
    char       *end;

    if (!dash || dash == name || dash - name >= (int)sizeof(file)) {
        return qfalse;
    }
    spec->px = (float)strtod(dash + 1, &end);
    if (end == dash + 1 || spec->px <= 0) {
        return qfalse;
    }
    spec->shadow = strchr(end, 's') ? qtrue : qfalse;
    spec->slant  = strchr(end, 'i') ? qtrue : qfalse;
    Q_strncpyz(file, name, dash - name + 1);
    spec->face = Bake_Face(file);
    return spec->face ? qtrue : qfalse;
}

static void Bake_Metrics(const uiFace_t *face, float pixels, uiMetrics_t *m)
{
    int ascent, descent, gap, x0, y0, x1, y1;

    m->scale = stbtt_ScaleForMappingEmToPixels(&face->info, pixels);
    stbtt_GetFontVMetrics(&face->info, &ascent, &descent, &gap);
    m->lineH = (int)ceilf((ascent - descent) * m->scale);
    m->capH  = stbtt_GetCodepointBox(&face->info, 'H', &x0, &y0, &x1, &y1) ? y1 * m->scale : ascent * m->scale * 0.7f;
    m->base  = floorf(m->lineH * 0.5f + m->capH * 0.5f + 0.5f);
}

// the game's text is Windows-1252
static int Bake_Unicode(int c)
{
    static const unsigned short high[32] = {
        0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
    };

    return c >= 0x80 && c < 0xA0 ? high[c - 0x80] : c;
}

// a glyph's outline into bake_path: its pen at x, its baseline at y, turned
// by (cs, sn) about (ox, oy)
static float Bake_GlyphPath(const uiFace_t *face, int c, float scale, float slant, float x, float y, float cs, float sn, float ox, float oy)
{
    const int   glyph = stbtt_FindGlyphIndex(&face->info, Bake_Unicode(c));
    uiVertex_t *v;
    int         adv, lsb, n, i;

    stbtt_GetGlyphHMetrics(&face->info, glyph, &adv, &lsb);
    n = stbtt_GetGlyphShape(&face->info, glyph, &v);
    for (i = 0; i < n; i++) {
        uiVertex_t p = v[i];
        float      px[2], py[2];

        px[0] = x + (p.x + p.y * slant) * scale - ox;
        py[0] = y - p.y * scale - oy;
        px[1] = x + (p.cx + p.cy * slant) * scale - ox;
        py[1] = y - p.cy * scale - oy;
        p.x   = ox + px[0] * cs - py[0] * sn;
        p.y   = oy + px[0] * sn + py[0] * cs;
        p.cx  = ox + px[1] * cs - py[1] * sn;
        p.cy  = oy + px[1] * sn + py[1] * cs;
        bake_path.push_back(p);
    }
    if (n > 0) {
        stbtt_FreeShape(&face->info, v);
    }
    return adv * scale;
}

// bake_path's coverage, w x h (y down)
static void Bake_Fill(byte *mask, int w, int h)
{
    stbtt__bitmap bm;

    memset(mask, 0, w * h);
    if (bake_path.empty()) {
        return;
    }
    bm.w      = w;
    bm.h      = h;
    bm.stride = w;
    bm.pixels = mask;
    stbtt_Rasterize(&bm, 0.25f, bake_path.data(), (int)bake_path.size(), 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, 0, NULL);
    bake_path.clear();
}

static float Bake_Sample(const byte *m, int w, int h, float x, float y)
{
    const int   ix = (int)floorf(x), iy = (int)floorf(y);
    const float ax = x - ix, ay = y - iy;
    float       v = 0.0f;

    for (int k = 0; k < 4; k++) {
        const int sx = ix + (k & 1), sy = iy + (k >> 1);

        if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
            v += ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? ay : 1.0f - ay) * m[sy * w + sx];
        }
    }
    return v / 255.0f;
}

// the drop shadow's alpha under a mask (Omaha's: a pixel up, down, left and
// right at 0.5, 1.5 down and right at 0.33, pixels of 1080 lines, times s)
static float Bake_ShadowAt(const byte *m, int w, int h, int x, int y, float s)
{
    static const float passes[5][3] = {
        {0.0f, -1.0f, 0.5f}, {0.0f, 1.0f, 0.5f}, {-1.0f, 0.0f, 0.5f}, {1.0f, 0.0f, 0.5f}, {1.5f, 1.5f, 0.33f}
    };
    float keep = 1.0f;

    for (int p = 0; p < 5; p++) {
        keep *= 1.0f - passes[p][2] * Bake_Sample(m, w, h, x - passes[p][0] * s, y - passes[p][1] * s);
    }
    return 1.0f - keep;
}

/*
================
UI_BakeFont

The page: a cell per character, as wide as its advance and a line tall,
laid out 256 * scale texels across, so a cell's width in texture
coordinates times 256 (what the renderer's strings advance by) is its
width in design units, drawn a texel a pixel. The baseline sits where a
label's text, centered, has its capitals' middle on the label's
================
*/
static void Bake_FontPage(uiBakedFont_t *f, const uiFontSpec_t *spec)
{
    const float       s     = UI_DesignScale(1920.0f, 1080.0f);
    const float       slant = spec->slant ? SLANT : 0.0f;
    const int         pageW = Q_max(32, (int)(256.0f * s + 0.001f));
    const uiFace_t   *face  = spec->face;
    fontheader_sgl_t *sgl   = &f->sgl;
    uiMetrics_t       m;
    int               cells[256][3]; // x, y, w
    int               x = 0, y = 0, pageH, c;
    byte             *page, *mask;

    Bake_Metrics(face, spec->px * s, &m);
    memset(sgl, 0, sizeof(*sgl));
    for (c = 0; c < 256; c++) {
        int adv, lsb;

        sgl->indirection[c] = -1;
        cells[c][2]         = 0;
        if (c < 32 || (c != ' ' && !stbtt_FindGlyphIndex(&face->info, Bake_Unicode(c)))) {
            continue;
        }
        stbtt_GetGlyphHMetrics(&face->info, stbtt_FindGlyphIndex(&face->info, Bake_Unicode(c)), &adv, &lsb);
        cells[c][2] = Q_min(pageW, Q_max(1, (int)(adv * m.scale + 0.5f)));
        if (x + cells[c][2] > pageW) {
            x = 0;
            y += m.lineH + 1;
        }
        cells[c][0] = x;
        cells[c][1] = y;
        x += cells[c][2] + 1;
    }
    pageH = y + m.lineH;
    page  = (byte *)Z_Malloc(pageW * pageH * 4);
    mask  = (byte *)Z_Malloc(pageW * m.lineH);
    memset(page, 0, pageW * pageH * 4);

    for (c = 32; c < 256; c++) {
        const int w = cells[c][2];

        if (!w) {
            continue;
        }
        // the slant leans about the capitals' middle
        Bake_GlyphPath(face, c, m.scale, slant, -m.capH * slant * 0.5f, m.base, 1.0f, 0.0f, 0.0f, 0.0f);
        Bake_Fill(mask, w, m.lineH);
        for (int row = 0; row < m.lineH; row++) {
            byte *out = page + ((cells[c][1] + row) * pageW + cells[c][0]) * 4;

            for (int col = 0; col < w; col++, out += 4) {
                const float a     = mask[row * w + col] / 255.0f;
                const float sh    = spec->shadow ? Bake_ShadowAt(mask, w, m.lineH, col, row, s) : 0.0f;
                const float alpha = a + sh * (1.0f - a);

                out[0] = out[1] = out[2] = (byte)(alpha > 0.0f ? a / alpha * 255.0f + 0.5f : 255.0f);
                out[3]                   = (byte)(alpha * 255.0f + 0.5f);
            }
        }
        sgl->indirection[c]        = c;
        sgl->locations[c].pos[0]   = (float)cells[c][0] / pageW;
        sgl->locations[c].pos[1]   = (float)cells[c][1] / pageH;
        sgl->locations[c].size[0]  = (float)w / pageW;
        sgl->locations[c].size[1]  = (float)m.lineH / pageH;
    }
    f->pageScale     = pageW / 256.0f;
    sgl->height      = m.lineH / s;
    sgl->aspectRatio = 1.0f;
    sgl->trhandle    = -1;
    Q_strncpyz(sgl->name, f->name.c_str(), sizeof(sgl->name));
    // the renderer's font shader: gfx/fonts/<name>
    uii.Rend_RegisterImage2D(va("gfx/fonts/%s", f->name.c_str()), page, pageW, pageH);
    Z_Free(mask);
    Z_Free(page);

    memset(&f->header, 0, sizeof(f->header));
    f->header.sgl[0] = sgl;
    Q_strncpyz(f->header.name, f->name.c_str(), sizeof(f->header.name));
}

float UI_BakedFontScale(const fontheader_t *font)
{
    for (uiBakedFont_t *b : bake_fonts) {
        if (&b->header == font) {
            return b->pageScale;
        }
    }
    return 0.0f;
}

fontheader_t *UI_BakeFont(const char *name)
{
    uiBakedFont_t *f = NULL;
    uiFontSpec_t   spec;

    for (uiBakedFont_t *b : bake_fonts) {
        if (!Q_stricmp(b->name.c_str(), name)) {
            f = b;
            break;
        }
    }
    if (f && f->seq == UI_FontStamp()) {
        return &f->header;
    }
    if (!Bake_ParseFont(name, &spec)) {
        return NULL;
    }
    if (!f) {
        f       = new uiBakedFont_t();
        f->name = name;
        bake_fonts.push_back(f);
    }
    f->seq = UI_FontStamp();
    Bake_FontPage(f, &spec);
    return &f->header;
}

/*
====================================================================

SHAPES

====================================================================
*/

typedef struct {
    std::vector<float> px; // premultiplied RGBA
    int                w, h;
    float              scale;
} uiCanvas_t;

static void Bake_Over(float *px, const float *c, float cover)
{
    const float a = c[3] * cover;

    px[0] = c[0] * a + px[0] * (1.0f - a);
    px[1] = c[1] * a + px[1] * (1.0f - a);
    px[2] = c[2] * a + px[2] * (1.0f - a);
    px[3] = a + px[3] * (1.0f - a);
}

static void Bake_Mask(uiCanvas_t *cv, const byte *mask, const float *c)
{
    for (int i = 0; i < cv->w * cv->h; i++) {
        if (mask[i]) {
            Bake_Over(&cv->px[i * 4], c, mask[i] / 255.0f);
        }
    }
}

static void Bake_MoveTo(float x, float y, int type)
{
    uiVertex_t v;

    memset(&v, 0, sizeof(v));
    v.x    = x;
    v.y    = y;
    v.type = type;
    bake_path.push_back(v);
}

static void Bake_Arc(float cx, float cy, float r, float a0, float a1, qboolean first)
{
    const int n = Q_max(4, (int)(fabsf(a1 - a0) * r / 2.0f) + 4);

    for (int i = 0; i <= n; i++) {
        const float a = a0 + (a1 - a0) * i / n;

        Bake_MoveTo(cx + r * cosf(a), cy + r * sinf(a), first && !i ? STBTT_vmove : STBTT_vline);
    }
}

static void Bake_Circle(float cx, float cy, float r, qboolean hole)
{
    Bake_Arc(cx, cy, r, 0.0f, hole ? -2.0f * M_PI : 2.0f * M_PI, qtrue);
}

static void Bake_RoundBox(float cx, float cy, float w, float h, float r, float angle, qboolean hole)
{
    std::vector<float> pts;
    const float        cs = cosf(DEG2RAD(angle)), sn = sinf(DEG2RAD(angle));
    int                i, k, n;

    r = Q_min(r, Q_min(w, h) * 0.5f);
    for (k = 0; k < 4; k++) {
        // the corners' arcs, clockwise from the top left
        const float ax = (k == 1 || k == 2) ? w * 0.5f - r : -w * 0.5f + r;
        const float ay = (k >= 2) ? h * 0.5f - r : -h * 0.5f + r;

        for (i = 0; i <= 8; i++) {
            const float a = DEG2RAD(180.0f + 90.0f * k + 90.0f * i / 8.0f);

            pts.push_back(ax + r * cosf(a));
            pts.push_back(ay + r * sinf(a));
        }
    }
    n = (int)pts.size() / 2;
    for (i = 0; i < n; i++) {
        const int   j = hole ? n - 1 - i : i;
        const float x = pts[j * 2], y = pts[j * 2 + 1];

        Bake_MoveTo(cx + x * cs - y * sn, cy + x * sn + y * cs, i ? STBTT_vline : STBTT_vmove);
    }
    Bake_MoveTo(bake_path[bake_path.size() - n].x, bake_path[bake_path.size() - n].y, STBTT_vline);
}

// a PNG fitted in the canvas (align: 0 left, 1 center, 2 right), its texels
// averaged over each pixel
static void Bake_Picture(uiCanvas_t *cv, const char *file, const float *c, int align, qboolean mirror)
{
    byte *pic;
    int   sw, sh, tw, th, x0, y0, x, y;

    if (!uii.Rend_LoadRawImage(file, &pic, &sw, &sh) || !pic) {
        Com_DPrintf("UI: no picture %s\n", file);
        return;
    }
    {
        const float        fit = Q_min((float)cv->w / sw, (float)cv->h / sh);
        std::vector<float> rows; // across first: tw x sh, premultiplied

        tw = Q_max(1, (int)(sw * fit + 0.5f));
        th = Q_max(1, (int)(sh * fit + 0.5f));
        x0 = align == 0 ? 0 : align == 1 ? (cv->w - tw) / 2 : cv->w - tw;
        y0 = (cv->h - th) / 2;
        rows.assign(tw * sh * 4, 0.0f);
        for (y = 0; y < sh; y++) {
            for (x = 0; x < tw; x++) {
                const float a = x * (float)sw / tw, b = (x + 1) * (float)sw / tw;
                float      *o = &rows[(y * tw + x) * 4];

                for (int i = (int)a; i < b && i < sw; i++) {
                    const byte *p  = pic + (y * sw + i) * 4;
                    const float wt = (Q_min(b, (float)i + 1) - Q_max(a, (float)i)) * (p[3] / 255.0f) / (b - a);

                    o[0] += p[0] / 255.0f * wt;
                    o[1] += p[1] / 255.0f * wt;
                    o[2] += p[2] / 255.0f * wt;
                    o[3] += wt;
                }
            }
        }
        for (y = 0; y < th; y++) {
            const float a = y * (float)sh / th, b = (y + 1) * (float)sh / th;

            for (x = 0; x < tw; x++) {
                float sum[4] = {0, 0, 0, 0};

                for (int i = (int)a; i < b && i < sh; i++) {
                    const float  wt = (Q_min(b, (float)i + 1) - Q_max(a, (float)i)) / (b - a);
                    const float *p  = &rows[(i * tw + x) * 4];

                    for (int k = 0; k < 4; k++) {
                        sum[k] += p[k] * wt;
                    }
                }
                if (sum[3] > 0.0f && x0 + x >= 0 && y0 + y >= 0 && x0 + x < cv->w && y0 + y < cv->h) {
                    const float tint[4] = {c[0] * sum[0] / sum[3], c[1] * sum[1] / sum[3], c[2] * sum[2] / sum[3], c[3]};

                    Bake_Over(&cv->px[((y0 + y) * cv->w + x0 + (mirror ? tw - 1 - x : x)) * 4], tint, sum[3]);
                }
            }
        }
    }
    uii.Rend_FreeRawImage(pic);
}

// the tokens of a layer, a quoted string one
static int Bake_Tokens(const char *text, std::vector<std::string>& out)
{
    out.clear();
    while (*text) {
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        if (!*text) {
            break;
        }
        if (*text == '"') {
            const char *end = strchr(text + 1, '"');

            out.push_back(std::string(text + 1, end ? end - text - 1 : strlen(text + 1)));
            text = end ? end + 1 : text + strlen(text);
        } else {
            const char *start = text;

            while (*text && *text != ' ' && *text != '\t') {
                text++;
            }
            out.push_back(std::string(start, text - start));
        }
    }
    return (int)out.size();
}

/*
================
Bake_Layer

A layer: what it is, then its color (r g b a, 0 to 1), then the rest.
Lengths are design units (times the canvas's scale); positions are
fractions of the box. The shapes:
  fill COLOR
  vgradient Y0 Y1 COLOR COLOR / hgradient X0 X1 COLOR COLOR
  circle COLOR [R [X Y]]        (R: of half the box, 1 by default; X Y its middle)
  ring W COLOR                  (W thick, inside the circle's edge)
  frame W COLOR                 (W thick, inside the box's edge)
  roundbox R COLOR [W H ANGLE]  (the box, or W x H in its middle turned ANGLE)
  poly COLOR x y x y ...
  crescent COLOR X0 Y0 RF T0 T1 T2 T3  (Omaha's damage crescent: the box's
                                circle from X0 Y0 over the top and back by a
                                circle of RF, seen through a radial band in
                                from T0 to T1, out from T2 to T3)
  text COLOR FONT X Y ANGLE STRING   (FONT a face's name; its capitals'
                                middle at X Y, turned ANGLE about the middle)
  picture COLOR FILE [left|center|right] [mirror]
A layer can start with "in X0 Y0 X1 Y1": drawn in that part of the box
(fractions, or design units with "px", from the right or bottom edge when
negative: "in -2px 0 1 1" the box's 2 units at its right).
================
*/
static void Bake_Layer(uiCanvas_t *cv, const char *text)
{
    std::vector<std::string> t;

    // in X0 Y0 X1 Y1 <layer>: the layer in that part of the box, over the rest
    if (!strncmp(text, "in ", 3)) {
        float      b[4];
        int        used = 0;
        uiCanvas_t sub;

        {
            const char *p = text + 3;

            for (int k = 0; k < 4; k++) {
                char *end;

                b[k] = strtof(p, &end);
                if (end == p) {
                    return;
                }
                if (!strncmp(end, "px", 2)) {
                    // design units, from the far edge when negative
                    const float px = b[k] * cv->scale, side = (k & 1) ? cv->h : cv->w;

                    b[k] = (px < 0.0f || (px == 0.0f && *p == '-') ? side + px : px) / side;
                    end += 2;
                }
                p = end;
            }
            while (*p == ' ') {
                p++;
            }
            used = (int)(p - (text + 3));
        }
        {
            const int x0 = (int)floorf(b[0] * cv->w + 0.5f), y0 = (int)floorf(b[1] * cv->h + 0.5f);

            sub.w     = (int)floorf(b[2] * cv->w + 0.5f) - x0;
            sub.h     = (int)floorf(b[3] * cv->h + 0.5f) - y0;
            sub.scale = cv->scale;
            if (sub.w <= 0 || sub.h <= 0) {
                return;
            }
            sub.px.assign(sub.w * sub.h * 4, 0.0f);
            Bake_Layer(&sub, text + 3 + used);
            for (int y = 0; y < sub.h; y++) {
                for (int x = 0; x < sub.w; x++) {
                    const float *p = &sub.px[(y * sub.w + x) * 4];
                    float       *d = &cv->px[((y0 + y) * cv->w + x0 + x) * 4];

                    if (x0 + x < 0 || y0 + y < 0 || x0 + x >= cv->w || y0 + y >= cv->h) {
                        continue;
                    }
                    for (int k = 0; k < 4; k++) {
                        d[k] = p[k] + d[k] * (1.0f - p[3]);
                    }
                }
            }
        }
        return;
    }
    const int                n  = Bake_Tokens(text, t);
    const float              w  = (float)cv->w, h = (float)cv->h;
    const float              cx = w * 0.5f, cy = h * 0.5f, r = Q_min(w, h) * 0.5f;
    std::vector<byte>        mask(cv->w * cv->h);
    float                    c[4];
    int                      a = 5; // the first token after the color
    auto                     num = [&](int i) { return i < n ? (float)atof(t[i].c_str()) : 0.0f; };

    if (n < 5) {
        return;
    }
    // the gradients' stops come first, their colors after
    if (t[0] == "vgradient" || t[0] == "hgradient") {
        const qboolean v  = t[0] == "vgradient";
        const float    e0 = num(1) * (v ? h : w), e1 = num(2) * (v ? h : w);

        for (int y = 0; y < cv->h; y++) {
            for (int x = 0; x < cv->w; x++) {
                const float at = (v ? y : x) + 0.5f;
                const float f  = (at - e0) / Q_max(0.001f, e1 - e0);
                float       col[4];

                if (at < e0 || at >= e1) {
                    continue;
                }
                // premultiplied between the stops, back to straight
                col[3] = num(6) + (num(10) - num(6)) * f;
                for (int k = 0; k < 3; k++) {
                    col[k] = col[3] > 0.0f ? (num(3 + k) * num(6) + (num(7 + k) * num(10) - num(3 + k) * num(6)) * f) / col[3] : 0.0f;
                }
                Bake_Over(&cv->px[(y * cv->w + x) * 4], col, 1.0f);
            }
        }
        return;
    }
    // the rest: COLOR after the kind, or after the kind's one number
    if (t[0] == "ring" || t[0] == "frame" || t[0] == "roundbox") {
        a = 6;
    }
    for (int k = 0; k < 4; k++) {
        c[k] = num(a - 4 + k);
    }

    if (t[0] == "fill") {
        for (int i = 0; i < cv->w * cv->h; i++) {
            Bake_Over(&cv->px[i * 4], c, 1.0f);
        }
        return;
    } else if (t[0] == "circle") {
        // [R [X Y]]: of half the box, its middle
        Bake_Circle(n > a + 2 ? num(a + 1) * w : cx, n > a + 2 ? num(a + 2) * h : cy, r * (n > a ? num(a) : 1.0f), qfalse);
    } else if (t[0] == "ring") {
        // (edges on whole pixels, at least one)
        Bake_Circle(cx, cy, r, qfalse);
        Bake_Circle(cx, cy, r - Q_max(1.0f, floorf(num(1) * cv->scale + 0.5f)), qtrue);
    } else if (t[0] == "frame") {
        const float p = Q_max(1.0f, floorf(num(1) * cv->scale + 0.5f));

        Bake_RoundBox(cx, cy, w, h, 0.0f, 0.0f, qfalse);
        Bake_RoundBox(cx, cy, w - p * 2.0f, h - p * 2.0f, 0.0f, 0.0f, qtrue);
    } else if (t[0] == "roundbox") {
        if (n > a + 2) {
            Bake_RoundBox(cx, cy, num(a) * cv->scale, num(a + 1) * cv->scale, num(1) * cv->scale, num(a + 2), qfalse);
        } else {
            Bake_RoundBox(cx, cy, w, h, num(1) * cv->scale, 0.0f, qfalse);
        }
    } else if (t[0] == "poly") {
        for (int i = a; i + 1 < n; i += 2) {
            Bake_MoveTo(num(i) * w, num(i + 1) * h, i == a ? STBTT_vmove : STBTT_vline);
        }
        Bake_MoveTo(num(a) * w, num(a + 1) * h, STBTT_vline);
    } else if (t[0] == "crescent") {
        const float x0 = num(a) * w, y0 = num(a + 1) * w, rf = num(a + 2) * w;
        const float hh = sqrtf(rf * rf - (cx - x0) * (cx - x0)), fcy = y0 + hh;

        Bake_Arc(cx, cx, cx, atan2f(y0 - cx, x0 - cx), atan2f(y0 - cx, w - x0 - cx), qtrue);
        Bake_Arc(cx, fcy, rf, atan2f(y0 - fcy, w - x0 - cx), atan2f(y0 - fcy, x0 - cx), qfalse);
        Bake_Fill(mask.data(), cv->w, cv->h);
        for (int y = 0; y < cv->h; y++) {
            for (int x = 0; x < cv->w; x++) {
                const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cx) * (y + 0.5f - cx)) / cx;
                const float m = d < num(a + 3)   ? 0.0f
                              : d < num(a + 4) ? (d - num(a + 3)) / (num(a + 4) - num(a + 3))
                              : d < num(a + 5) ? 1.0f
                              : d < num(a + 6) ? (num(a + 6) - d) / (num(a + 6) - num(a + 5))
                                               : 0.0f;

                mask[y * cv->w + x] = (byte)(mask[y * cv->w + x] * m + 0.5f);
            }
        }
        Bake_Mask(cv, mask.data(), c);
        return;
    } else if (t[0] == "text" && n > a + 4) {
        uiFontSpec_t spec;
        uiMetrics_t  m;
        const float  ang = DEG2RAD(num(a + 3));
        float        width = 0.0f, pen;

        if (!Bake_ParseFont(t[a].c_str(), &spec)) {
            return;
        }
        Bake_Metrics(spec.face, spec.px * cv->scale, &m);
        for (const char *p = t[a + 4].c_str(); *p; p++) {
            int adv, lsb;

            stbtt_GetGlyphHMetrics(&spec.face->info, stbtt_FindGlyphIndex(&spec.face->info, Bake_Unicode((byte)*p)), &adv, &lsb);
            width += adv * m.scale;
        }
        pen = num(a + 1) * w - width * 0.5f;
        for (const char *p = t[a + 4].c_str(); *p; p++) {
            pen += Bake_GlyphPath(spec.face, (byte)*p, m.scale, spec.slant ? SLANT : 0.0f, pen, num(a + 2) * h + m.capH * 0.5f, cosf(ang), sinf(ang), cx, cy);
        }
        Bake_Fill(mask.data(), cv->w, cv->h);
        if (spec.shadow) {
            static const float black[4] = {0, 0, 0, 1};

            for (int y = 0; y < cv->h; y++) {
                for (int x = 0; x < cv->w; x++) {
                    Bake_Over(&cv->px[(y * cv->w + x) * 4], black, Bake_ShadowAt(mask.data(), cv->w, cv->h, x, y, cv->scale) * c[3]);
                }
            }
        }
        Bake_Mask(cv, mask.data(), c);
        return;
    } else if (t[0] == "picture") {
        const int align = n > a + 1 ? (t[a + 1] == "left" ? 0 : t[a + 1] == "center" ? 1 : 2) : 1;

        Bake_Picture(cv, t[a].c_str(), c, align, n > a + 2 && t[a + 2] == "mirror");
        return;
    } else {
        Com_DPrintf("UI: unknown shape '%s'\n", t[0].c_str());
        return;
    }
    Bake_Fill(mask.data(), cv->w, cv->h);
    Bake_Mask(cv, mask.data(), c);
}

static qhandle_t Bake_Shape(const char *image, const char *const *layers, int numLayers, int w, int h, float scale)
{
    uiCanvas_t        cv;
    std::vector<byte> out(w * h * 4);
    int               i;

    cv.w     = w;
    cv.h     = h;
    cv.scale = scale;
    cv.px.assign(w * h * 4, 0.0f);
    for (i = 0; i < numLayers; i++) {
        Bake_Layer(&cv, layers[i]);
    }
    for (i = 0; i < w * h; i++) {
        const float *p = &cv.px[i * 4];
        const float  a = p[3];

        for (int k = 0; k < 3; k++) {
            out[i * 4 + k] = (byte)(a > 0.0f ? Q_min(1.0f, p[k] / a) * 255.0f + 0.5f : 255.0f);
        }
        out[i * 4 + 3] = (byte)(Q_min(1.0f, a) * 255.0f + 0.5f);
    }
    // a texel left clear takes the color of one beside it: no dark fringe when drawn turned
    for (i = 0; i < w * h; i++) {
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};

        if (out[i * 4 + 3]) {
            continue;
        }
        for (int k = 0; k < 4; k++) {
            const int x = i % w + dx[k], y = i / w + dy[k];

            if (x >= 0 && y >= 0 && x < w && y < h && out[(y * w + x) * 4 + 3]) {
                memcpy(&out[i * 4], &out[(y * w + x) * 4], 3);
                break;
            }
        }
    }
    return uii.Rend_RegisterImage2D(image, out.data(), w, h);
}

qhandle_t UI_BakeShape(const char *key, const char *const *layers, int numLayers, int w, int h, float scale)
{
    std::string k = key;
    uiBaked_t  *b;

    if (w <= 0 || h <= 0 || numLayers <= 0) {
        return 0;
    }
    k += va(" %dx%d %g", w, h, scale);
    for (int i = 0; i < numLayers; i++) {
        k += '|';
        k += layers[i];
    }
    auto it = bake_shapes.find(k);
    if (it == bake_shapes.end()) {
        uiBaked_t nb;

        memset(&nb, 0, sizeof(nb));
        nb.seq = -1;
        Com_sprintf(nb.image, sizeof(nb.image), "ui_bake/%d", (int)bake_shapes.size());
        it = bake_shapes.emplace(k, nb).first;
    }
    b = &it->second;
    if (b->seq != uii.GetRefSequence()) {
        b->seq    = uii.GetRefSequence();
        b->gen    = bake_gen;
        b->handle = Bake_Shape(b->image, layers, numLayers, w, h, scale);
    } else if (b->gen != bake_gen) {
        b->gen    = bake_gen;
        b->handle = uii.Rend_RegisterMaterial(b->image);
    }
    return b->handle;
}
