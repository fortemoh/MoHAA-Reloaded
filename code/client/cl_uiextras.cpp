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

// cl_uiextras.cpp: the Competitive HUD's extras (Added).
// Drawn in place of the stock ones, nothing of them while they don't show:
//
// - the zoom overlays: the scopes (a circle, its edge darkening into black,
//   thin lines: a cross, the Kar98's a post up to the middle) and the
//   binoculars (two circles, a soft rim, a small range scale). A mask, the
//   screen's top left quarter at its pixels, is made once a screen size (on
//   the bake threads, while a map loads or as the HUD is turned on), with
//   what is drawn; drawn four times mirrored, the black around it and the
//   lines are boxes;
// - the centre and location prints: a dark box with accent edges above and
//   below, Oswald, as wide as the text;
// - the chat's typing line: a slim bar over the HUD's chat, "ALL:", "TEAM:"
//   or "TO <name>:" and the line typed (the DM console still takes the keys;
//   its window isn't drawn);
// - the round's end banner (the server's g_scoreboardpicover): the winners'
//   emblem, "ALLIES WIN THE ROUND", the teams' score. On the Reloaded
//   scoreboard (with either HUD) it sits above its header (cl_uimenus.cpp);
// - the quick messages' menu (the V key's): the group's name, its lines
//   numbered in the accent, in a dark box on the left;
// - the status icons (the cgame's): PAUSED, CONNECTION INTERRUPTED and SLOW
//   SERVER, an icon and the name in a dark box.
//
// Sizes are design units (cl_uidraw.h): the prints and the banner the
// menus' (1080 lines), the typing line and the quick messages the HUD's
// (cg_hud_scale). Text is Oswald baked at the pixel size drawn, baked ahead
// with the map's load.

#include "client.h"
#include "cl_ui.h"
#include "cl_uicomp.h"
#include "cl_uiextras.h"
#include "../uilib/uibake.h"
#include "../qcommon/localization.h"

#include <algorithm>
#include <string>
#include <vector>

/*
====================================================================

ZOOM OVERLAYS

====================================================================
*/

enum { ZM_SCOPE, ZM_BINOCULARS, ZM_NUM };

// a mask: black, its alpha the overlay's; the screen's top left quarter
// (to its middle row and column), cut to what the shape reaches
typedef struct {
    int               w, h;           // the screen it is for
    int               qw, qh, xl, yt; // its texels across and down, its top left on the screen
    std::vector<byte> texels;         // made (RGBA), not registered yet
    std::future<void> job;            // being made
    bool              making;
    int               refSeq;         // the renderer it was registered with
    int               stamp;          // UI_BakeStamp then: its shader looked up again after a map's load
    int               madeMs;         // how long it took to make (developer)
    qhandle_t         shader;
    // what is drawn, worked out with the mask: its four draws (x, y, w, h,
    // s0, t0, s1, t1), the black round it, the lines (the scope's [1] the
    // Kar98's) and their alpha
    float             quads[4][8], black[4][4], lines[2][24][4], lineAlpha;
    int               numBlack, numLines[2];
} zoomMask_t;

static zoomMask_t zm[ZM_NUM];
static const char *const zm_names[ZM_NUM] = {"ui_bake/zoom_scope", "ui_bake/zoom_binoculars"};

// the binoculars' unit: the screen's height, less on a screen narrower than 1.62 : 1
static float ZM_BinocularUnit(int w, int h)
{
    return Q_min((float)h, w / 1.62f);
}

static void ZM_Quad(zoomMask_t *m, int w, int h, float halfW, float halfH)
{
    const int xm = (w + 1) / 2, ym = (h + 1) / 2;

    m->qw = Q_min(xm, (int)ceilf(halfW) + 1);
    m->qh = Q_min(ym, (int)ceilf(halfH) + 1);
    m->xl = xm - m->qw;
    m->yt = ym - m->qh;
    m->texels.assign((size_t)m->qw * m->qh * 4, 0);
}

static void ZM_Set(zoomMask_t *m, int i, int j, float a)
{
    m->texels[((size_t)j * m->qw + i) * 4 + 3] = (byte)(Com_Clamp(0.0f, 1.0f, a) * 255.0f + 0.5f);
}

// the normal distribution's
static float ZM_Phi(float z)
{
    return 0.5f * erfcf(-z * 0.70710678f);
}

// a scope: black from 0.46 of the height out (its edge smoothed over a
// pixel), darkening into it from 0.44 (a normal curve 0.033 wide: 11 % at
// 0.40, half at 0.44)
static void ZM_Scope(zoomMask_t *m, int w, int h)
{
    const float R = 0.46f * h, mid = 0.44f * h, sig = 0.033f * h;
    const float r0 = mid - 4.0f * sig;
    const int   n = 1024;
    const float step = (R - r0) / n;
    float       lut[n + 2];

    ZM_Quad(m, w, h, R, R);
    for (int i = 0; i <= n + 1; i++) {
        lut[i] = ZM_Phi((r0 + i * step - mid) / sig);
    }
    for (int j = 0; j < m->qh; j++) {
        const float dy = m->yt + j + 0.5f - h * 0.5f;

        for (int i = 0; i < m->qw; i++) {
            const float dx = m->xl + i + 0.5f - w * 0.5f, r = sqrtf(dx * dx + dy * dy);
            const float c = Com_Clamp(0.0f, 1.0f, r - R + 0.5f);
            float       rim = 0.0f;

            if (r > r0) {
                const float t = Q_min((float)n, (r - r0) / step);
                const int   k = (int)t;

                rim = lut[k] + (lut[k + 1] - lut[k]) * (t - k);
            }
            ZM_Set(m, i, j, c + (1.0f - c) * rim);
        }
    }
}

// the binoculars: black outside two circles (0.46 of the unit, their
// middles 0.33 either side), and inside them 0.8 of the outside blurred
// (0.025 of the unit): a soft rim, darker where the circles meet. The blur
// on a grid of cells about a sixth of it, read between them
static void ZM_Binoculars(zoomMask_t *m, int w, int h)
{
    const float u = ZM_BinocularUnit(w, h), r = 0.46f * u, a = 0.33f * u, sig = 0.025f * u;
    const float cx = w * 0.5f, cy = h * 0.5f;
    auto        dist = [&](float x, float y) {
        const float dy2 = (y - cy) * (y - cy);
        const float dA = sqrtf((x - (cx - a)) * (x - (cx - a)) + dy2), dB = sqrtf((x - (cx + a)) * (x - (cx + a)) + dy2);

        return Q_min(dA, dB) - r;
    };

    ZM_Quad(m, w, h, a + r, r);

    const int   f   = Q_max(1, (int)floorf(sig / 6.0f + 0.5f));
    const float sl  = sig / f;
    const int   pad = (int)ceilf(3.0f * sl) + 1;
    const int   gw = (m->qw + f - 1) / f + 2 * pad, gh = (m->qh + f - 1) / f + 2 * pad;
    const int   kr = (int)ceilf(3.0f * sl);
    std::vector<float> g((size_t)gw * gh), t((size_t)gw * gh), k(2 * kr + 1);
    float              ks = 0.0f;

    // the outside's cover of each cell (mirrored past the screen's middle)
    for (int gj = 0; gj < gh; gj++) {
        float y = m->yt + (gj - pad + 0.5f) * f;

        if (y > cy) {
            y = 2.0f * cy - y;
        }
        for (int gi = 0; gi < gw; gi++) {
            float x = m->xl + (gi - pad + 0.5f) * f;

            if (x > cx) {
                x = 2.0f * cx - x;
            }
            g[(size_t)gj * gw + gi] = Com_Clamp(0.0f, 1.0f, dist(x, y) / f + 0.5f);
        }
    }
    for (int i = -kr; i <= kr; i++) {
        ks += k[i + kr] = expf(-0.5f * i * i / (sl * sl));
    }
    for (float& v : k) {
        v /= ks;
    }
    for (int gj = 0; gj < gh; gj++) {
        for (int gi = 0; gi < gw; gi++) {
            float s = 0.0f;

            for (int i = -kr; i <= kr; i++) {
                s += k[i + kr] * g[(size_t)gj * gw + Q_min(gw - 1, Q_max(0, gi + i))];
            }
            t[(size_t)gj * gw + gi] = s;
        }
    }
    for (int gj = 0; gj < gh; gj++) {
        for (int gi = 0; gi < gw; gi++) {
            float s = 0.0f;

            for (int i = -kr; i <= kr; i++) {
                s += k[i + kr] * t[(size_t)Q_min(gh - 1, Q_max(0, gj + i)) * gw + gi];
            }
            g[(size_t)gj * gw + gi] = s;
        }
    }
    for (int j = 0; j < m->qh; j++) {
        const float y  = m->yt + j + 0.5f;
        const float gy = Q_min(gh - 1.001f, Q_max(0.0f, (j + 0.5f) / f + pad - 0.5f));
        const int   gj = (int)gy;
        const float fy = gy - gj;

        for (int i = 0; i < m->qw; i++) {
            const float  x  = m->xl + i + 0.5f;
            const float  c  = Com_Clamp(0.0f, 1.0f, dist(x, y) + 0.5f);
            const float  gx = Q_min(gw - 1.001f, Q_max(0.0f, (i + 0.5f) / f + pad - 0.5f));
            const int    gi = (int)gx;
            const float  fx = gx - gi;
            const float *p  = &g[(size_t)gj * gw + gi];
            const float  b  = (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[gw] * (1 - fx) + p[gw + 1] * fx) * fy;

            ZM_Set(m, i, j, c + (1.0f - c) * 0.8f * b);
        }
    }
}

static void ZM_Box(float (*boxes)[4], int *n, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    boxes[*n][0] = x;
    boxes[*n][1] = y;
    boxes[*n][2] = w;
    boxes[*n][3] = h;
    (*n)++;
}

/*
================
ZM_Layout

What is drawn with a mask, worked out with it: the mask four times
(mirrored right and down: an odd size's middle column and row once), the
black round it, and the lines as boxes on whole pixels, the crosshair's
way. The scope's a pixel at 1080 lines (85 % black): a cross, and the
Kar98's, the line open 0.047 of the height either side of the middle, a
post twice as thick from below up to the middle. The binoculars' two
(90 %): a line 0.5 of the unit across, ticks every 0.05 up from it (the
middle and the ends twice as tall), and down from its middle a line 0.1
long crossed every 0.025
================
*/
static void ZM_Layout(zoomMask_t *m, int kind, int w, int h)
{
    const int   xm = (w + 1) / 2, ym = (h + 1) / 2, rw = m->qw - (w & 1), bh = m->qh - (h & 1);
    const float s1 = (float)rw / m->qw, t1 = (float)bh / m->qh;
    const float quads[4][8] = {
        {(float)m->xl, (float)m->yt, (float)m->qw, (float)m->qh, 0, 0, 1, 1},
        {(float)xm, (float)m->yt, (float)rw, (float)m->qh, s1, 0, 0, 1},
        {(float)m->xl, (float)ym, (float)m->qw, (float)bh, 0, t1, 1, 0},
        {(float)xm, (float)ym, (float)rw, (float)bh, s1, t1, 0, 0},
    };

    memcpy(m->quads, quads, sizeof(quads));
    m->numBlack = m->numLines[0] = m->numLines[1] = 0;
    ZM_Box(m->black, &m->numBlack, 0, 0, m->xl, h);
    ZM_Box(m->black, &m->numBlack, w - m->xl, 0, m->xl, h);
    ZM_Box(m->black, &m->numBlack, m->xl, 0, w - 2 * m->xl, m->yt);
    ZM_Box(m->black, &m->numBlack, m->xl, h - m->yt, w - 2 * m->xl, m->yt);

    if (kind == ZM_SCOPE) {
        const int   t  = Q_max(1, (int)floorf(h / 1080.0f + 0.5f));
        const float R  = 0.46f * h;
        const int   x0 = (int)floorf(w * 0.5f - R), x1 = (int)ceilf(w * 0.5f + R);
        const int   y0 = (int)floorf(h * 0.5f - R), y1 = (int)ceilf(h * 0.5f + R);
        const int   lx = (w - t) / 2, ly = (h - t) / 2;
        const int   g = (w - 2 * (int)floorf(0.047f * h + 0.5f)) / 2;

        // (the vertical line in two, the crossing drawn once)
        ZM_Box(m->lines[0], &m->numLines[0], x0, ly, x1 - x0, t);
        ZM_Box(m->lines[0], &m->numLines[0], lx, y0, t, ly - y0);
        ZM_Box(m->lines[0], &m->numLines[0], lx, ly + t, t, y1 - ly - t);
        ZM_Box(m->lines[1], &m->numLines[1], x0, ly, g - x0, t);
        ZM_Box(m->lines[1], &m->numLines[1], w - g, ly, x1 - (w - g), t);
        ZM_Box(m->lines[1], &m->numLines[1], (w - 2 * t) / 2, h / 2, 2 * t, y1 - h / 2);
        m->lineAlpha = 0.85f;
    } else {
        const float u  = ZM_BinocularUnit(w, h);
        const int   t  = Q_max(1, (int)floorf(2.0f * h / 1080.0f + 0.5f));
        const int   lx = (w - t) / 2, ly = (h - t) / 2;
        const int   half = (int)floorf(0.25f * u + 0.5f);
        const int   tick = Q_max(t, (int)floorf(0.015f * u + 0.5f)), big = Q_max(t, (int)floorf(0.03f * u + 0.5f));
        const int   down = (int)floorf(0.1f * u + 0.5f), cross = Q_max(1, (int)floorf(0.008f * u + 0.5f));

        ZM_Box(m->lines[0], &m->numLines[0], lx - half, ly, 2 * half + t, t);
        for (int i = -5; i <= 5; i++) {
            const int x = lx + (int)floorf(i * 0.05f * u + 0.5f), th = i % 5 ? tick : big;

            ZM_Box(m->lines[0], &m->numLines[0], x, ly - th, t, th);
        }
        ZM_Box(m->lines[0], &m->numLines[0], lx, ly + t, t, down - t);
        for (int j = 1; j <= 4; j++) {
            const int y = ly + (int)floorf(j * 0.025f * u + 0.5f);

            ZM_Box(m->lines[0], &m->numLines[0], lx - cross, y, cross, t);
            ZM_Box(m->lines[0], &m->numLines[0], lx + t, y, cross, t);
        }
        m->lineAlpha = 230.0f / 255.0f;
    }
}

static void ZM_Make(zoomMask_t *m, int kind, int w, int h)
{
    const int start = Sys_Milliseconds();

    if (kind == ZM_SCOPE) {
        ZM_Scope(m, w, h);
    } else {
        ZM_Binoculars(m, w, h);
    }
    ZM_Layout(m, kind, w, h);
    m->madeMs = Sys_Milliseconds() - start;
}

// a mask for the screen begun on the bake threads, unless made or begun
static void ZM_Begin(int kind)
{
    zoomMask_t *m = &zm[kind];
    const int   w = uid.vidWidth, h = uid.vidHeight;

    if (w <= 0 || h <= 0 || (m->w == w && m->h == h && (m->making || (m->shader && m->refSeq == uii.GetRefSequence())))) {
        return;
    }
    if (m->making) {
        // (a mask for another size: waited for, then made again)
        m->job.wait();
    }
    m->w      = w;
    m->h      = h;
    m->shader = 0;
    m->making = true;
    m->job    = UI_BakeRun([m, kind, w, h] { ZM_Make(m, kind, w, h); });
}

// the mask registered once made (a texture of its own: it would make the
// atlas grow), or its shader looked up again after a map's load
static void ZM_Finish(int kind)
{
    zoomMask_t *m = &zm[kind];

    if (m->making) {
        const int start = Sys_Milliseconds();
        int       made;

        try {
            m->job.get();
        } catch (const std::future_error&) {
            // (dropped before it ran: the bake threads' queue let go)
            ZM_Make(m, kind, m->w, m->h);
        }
        m->making = false;
        made      = Sys_Milliseconds();
        m->shader = re.RegisterTexture2D(zm_names[kind], m->texels.data(), m->qw, m->qh);
        m->refSeq = uii.GetRefSequence();
        m->stamp  = UI_BakeStamp();
        std::vector<byte>().swap(m->texels);
        Com_DPrintf("HUD extras: %s %dx%d made in %d ms, waited for %d, registered in %d\n", zm_names[kind], m->qw, m->qh, m->madeMs,
                    made - start, Sys_Milliseconds() - made);
    } else if (m->shader && m->stamp != UI_BakeStamp() && m->refSeq == uii.GetRefSequence()) {
        m->shader = re.RegisterShaderNoMip(zm_names[kind]);
        m->stamp  = UI_BakeStamp();
    }
}

/*
================
UI_DrawZoomOverlay

The cgame's zoom overlay (its fade kept): the mask and its lines as
ZM_Layout worked them out
================
*/
void UI_DrawZoomOverlay(int type, float alpha)
{
    const int   kind = type == 3 ? ZM_BINOCULARS : ZM_SCOPE;
    const int   kar = type == 1;
    zoomMask_t *m = &zm[kind];
    vec4_t      color = {1, 1, 1, alpha};

    if (alpha <= 0.0f) {
        return;
    }
    // (made now only if not begun ahead)
    if (m->w != uid.vidWidth || m->h != uid.vidHeight || (!m->making && (!m->shader || m->refSeq != uii.GetRefSequence()))) {
        ZM_Begin(kind);
    }
    ZM_Finish(kind);
    if (!m->shader) {
        return;
    }

    re.SetColor(color);
    for (const float *q : m->quads) {
        re.DrawStretchPic(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], m->shader);
    }
    color[0] = color[1] = color[2] = 0.0f;
    re.DrawBoxes((const float (*)[4])m->black, m->numBlack, color);
    color[3] = m->lineAlpha * alpha;
    re.DrawBoxes((const float (*)[4])m->lines[kar], m->numLines[kar], color);
    re.SetColor(NULL);
}

/*
====================================================================

COLORS AND TEXT

====================================================================
*/

// the design's dark box, its pale text
static const rlColor_t EX_FILL = {14 / 255.0f, 16 / 255.0f, 20 / 255.0f, 200 / 255.0f};
static const rlColor_t EX_TEXT = {245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1};
// the team line's blue, a private line's violet
static const rlColor_t EX_TEAM    = {150 / 255.0f, 186 / 255.0f, 222 / 255.0f, 1};
static const rlColor_t EX_PRIVATE = {200 / 255.0f, 170 / 255.0f, 230 / 255.0f, 1};

// the accent's pale shade (cg_hud_accent's, ui_hud_acc_line: the design's
// pale yellow), read again when it changes
static rlColor_t EX_Accent(void)
{
    static cvar_t   *cv;
    static int       read = -1;
    static rlColor_t c = {238 / 255.0f, 240 / 255.0f, 168 / 255.0f, 1};

    if (!cv) {
        cv = Cvar_Get("ui_hud_acc_line", "0.933 0.941 0.659", 0);
    }
    if (cv->modificationCount != read) {
        float r, g, b;

        read = cv->modificationCount;
        if (sscanf(cv->string, "%f %f %f", &r, &g, &b) == 3) {
            c.r = r;
            c.g = g;
            c.b = b;
        }
    }
    return c;
}

static rlColor_t EX_Fade(rlColor_t c, float alpha)
{
    c.a *= alpha;
    return c;
}

// a box with the accent's edges above and below (2 units)
static void EX_EdgedBox(const rlView_t& v, float x, float y, float w, float h, float alpha)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);
    const int e  = Q_max(1, RL_P(v, 2.0f));

    RL_BoxPx(x0, y0 + e, x1, y1 - e, EX_Fade(EX_FILL, alpha));
    RL_BoxPx(x0, y0, x1, y0 + e, EX_Fade(EX_Accent(), alpha));
    RL_BoxPx(x0, y1 - e, x1, y1, EX_Fade(EX_Accent(), alpha));
}

/*
====================================================================

STATUS ICONS

The cgame's (CG_DrawIcons, CG_DrawDisconnect: when, and their blinking):
a dark box, its left edge and its icon in the state's colour, the name in
capitals. PAUSED (the Pause key in a mission) in the middle at three
quarters of the screen's height (0.45 over a faded screen); CONNECTION
INTERRUPTED (nothing from the server for the commands' backup) at the
bottom's middle, SLOW SERVER (developer) over it. The icons are Phosphor's,
filled (MIT, ui/outlines/glyphs/PHOSPHOR-LICENSE.txt). The HUD's units

====================================================================
*/

static const struct {
    const char *glyph, *name;
    rlColor_t   color; // 0 alpha: the accent's
} ex_icons[UI_ICON_NUM] = {
    {RL_OUTLINES "glyphs/pause.svg",     "PAUSED",                 {0, 0, 0, 0}                                 },
    {RL_OUTLINES "glyphs/plugs.svg",     "CONNECTION INTERRUPTED", {226 / 255.0f, 104 / 255.0f, 88 / 255.0f, 1} },
    {RL_OUTLINES "glyphs/hourglass.svg", "SLOW SERVER",            {232 / 255.0f, 170 / 255.0f, 72 / 255.0f, 1}},
};

#define EX_ICON_H    40.0f // the box's height
#define EX_ICON_SIZE 20.0f // the icon's
#define EX_ICON_TEXT 17.0f // the name's (bold)

// the HUD's units (cg_hud_scale)
static rlView_t EX_HudView(void)
{
    rlView_t v = RL_View();

    v.s  = UI_DesignScale(1920.0f, 1080.0f);
    v.dw = uid.vidWidth / v.s;
    v.dh = uid.vidHeight / v.s;
    return v;
}

void UI_DrawStatusIcon(int icon, qboolean raised)
{
    const rlView_t v = EX_HudView();
    float          w, x, y;
    rlColor_t      c;

    if (icon < 0 || icon >= UI_ICON_NUM) {
        return;
    }
    c = ex_icons[icon].color.a > 0 ? ex_icons[icon].color : EX_Accent();
    w = 14.0f + EX_ICON_SIZE + 10.0f + RL_TextW(v, ex_icons[icon].name, EX_ICON_TEXT, true, 1.6f) + 18.0f;
    x = floorf((v.dw - w) * 0.5f);
    if (icon == UI_ICON_PAUSED) {
        y = v.dh * (raised ? 0.45f : 0.75f) - EX_ICON_H * 0.5f;
    } else {
        y = v.dh - 24.0f - EX_ICON_H - (icon == UI_ICON_SLOWSERVER ? EX_ICON_H + 8.0f : 0.0f);
    }
    y = floorf(y);
    {
        const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + EX_ICON_H);

        RL_BoxPx(x0, y0, x1, y1, EX_FILL);
        RL_BoxPx(x0, y0, x0 + Q_max(1, RL_P(v, 3.0f)), y1, c);
    }
    RL_Outline(v, ex_icons[icon].glyph, x + 14.0f + EX_ICON_SIZE * 0.5f, y + EX_ICON_H * 0.5f, EX_ICON_SIZE, EX_ICON_SIZE, c);
    RL_Text(v, x + 14.0f + EX_ICON_SIZE + 10.0f, y + EX_ICON_H * 0.5f, ex_icons[icon].name, EX_ICON_TEXT, EX_TEXT, true, RL_LEFT, 1.6f, RL_MID);
}

/*
====================================================================

THE ROUND'S END BANNER

The server names the winners' picture in g_scoreboardpicover (the cgame's
cg_scoreboardpicover) from a round's end to the next round (cleared at the
level's restart): the emblem, "<TEAM> WIN THE ROUND" (THE MATCH at the
intermission and in team deathmatch, where a win ends the match) and the
teams' score, Allies' first as everywhere else, under it

====================================================================
*/

static const float EX_BANNER_Y = 254.0f; // its top over the game (its middle at 300)

// the winners (0 Allies, 1 Axis), -1 none
static int EX_Winner(void)
{
    static cvar_t *cv;
    static int     read = -1, winner = -1;

    if (!cv) {
        cv = Cvar_Get("cg_scoreboardpicover", "", 0);
    }
    if (cv->modificationCount != read) {
        read   = cv->modificationCount;
        winner = strstr(cv->string, "allieswin") ? 0 : strstr(cv->string, "axiswin") ? 1 : -1;
    }
    return winner;
}

static bool EX_MatchWon(void)
{
    // (the intermission: the scores shown and the players frozen; the scores
    // alone are the round's end)
    const int both = PMF_INTERMISSION | PMF_FROZEN;

    return (cl.snap.valid && (cl.snap.ps.pm_flags & both) == both)
        || atoi(Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "g_gametype")) == GT_TEAM;
}

// the score line ("" none)
static const char *EX_Score(void)
{
    int allies, axis;

    if (sscanf(CL_ConfigString(CS_TEAMSCORES), "%d %d", &allies, &axis) != 2) {
        return "";
    }
    return va("%d  -  %d", allies, axis);
}

static const char *EX_BannerText(int winner, bool match)
{
    return winner ? (match ? "AXIS WIN THE MATCH" : "AXIS WIN THE ROUND") : (match ? "ALLIES WIN THE MATCH" : "ALLIES WIN THE ROUND");
}

float UI_Extras_BannerHeight(const rlView_t& v)
{
    if (EX_Winner() < 0) {
        return 0.0f;
    }
    return EX_Score()[0] ? 92.0f : 56.0f;
}

int UI_Extras_BannerKey(void)
{
    const int winner = EX_Winner();

    // (the accent's changes with it)
    return winner < 0 ? 0 : (winner + 1) | (EX_MatchWon() ? 4 : 0) | (Cvar_Get("ui_hud_acc_line", "", 0)->modificationCount << 3);
}

// drawn with its top at y, across the middle (as wide as its line, the
// emblem in front)
static void EX_Banner(const rlView_t& v, int winner, float y, float alpha)
{
    const std::string score = EX_Score();
    const char       *text  = EX_BannerText(winner, EX_MatchWon());
    const float       icon = 38.0f, tw = RL_TextW(v, text, 34, true);
    const float       bw = tw + icon + 14 + 48, bh = score.empty() ? 56.0f : 92.0f;
    const float       x  = floorf((v.dw - bw) * 0.5f);

    EX_EdgedBox(v, x, y, bw, bh, alpha);
    RL_TeamIcon(v, winner, x + 24 + icon * 0.5f, y + 28, icon, alpha);
    RL_Text(v, x + 24 + icon + 14, y + 28, text, 34, EX_Fade(EX_TEXT, alpha), true, RL_LEFT, 0, RL_MID);
    if (!score.empty()) {
        RL_Text(v, v.dw * 0.5f, y + 70, score.c_str(), 24, EX_Fade(EX_Accent(), alpha), false, RL_CENTER, 0, RL_MID);
    }
}

void UI_Extras_DrawBanner(const rlView_t& v, float y)
{
    const int winner = EX_Winner();

    if (winner >= 0) {
        EX_Banner(v, winner, y, 1.0f);
    }
}

/*
====================================================================

CENTRE AND LOCATION PRINTS

A dark box, the accent's edges above and below, Oswald Medium (24 units, a
line 32 apart), as wide as the widest line. A centre print's middle 300
units down (under the round's banner while it shows); a location print
where the stock one is: its text from x (centred at -1), its bottom y up
from the bottom (in 640 x 480 units; the middle at -1). A picture ("@")
stays stock; the centre print of the round's winners isn't drawn while the
banner says it

====================================================================
*/

static std::vector<std::string> EX_Lines(const char *text)
{
    std::vector<std::string> lines;
    const char              *p = text;

    while (*p) {
        const char *e = strchr(p, '\n');
        std::string l(p, e ? e - p : strlen(p));

        while (!l.empty() && (l.back() == ' ' || l.back() == '\r' || l.back() == '\t')) {
            l.pop_back();
        }
        if (!l.empty() && l.find_first_not_of(" \t") != std::string::npos) {
            lines.push_back(l.substr(l.find_first_not_of(" \t")));
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
    return lines;
}

// the round's winners told by the server's centre print ("Allies win!")
static bool EX_SaysWinners(const std::vector<std::string>& lines, int winner)
{
    const char *say = winner ? "Axis win!" : "Allies win!";

    return lines.size() == 1 && (!Q_stricmp(lines[0].c_str(), say) || !Q_stricmp(lines[0].c_str(), Sys_LV_CL_ConvertString(say)));
}

static bool EX_BannerOverGame(void);

void UI_Extras_Print(const char *text, float alpha, int x, int y)
{
    const rlView_t                 v = RL_View();
    const std::vector<std::string> lines = EX_Lines(text);
    const float                    size = 24.0f, step = 32.0f;
    float                          tw = 0.0f, bw, bh, bx, by;

    if (lines.empty() || alpha <= 0.0f) {
        return;
    }
    if (x == -2 && EX_Winner() >= 0 && EX_SaysWinners(lines, EX_Winner())) {
        return;
    }
    for (const std::string& l : lines) {
        tw = Q_max(tw, RL_TextW(v, l.c_str(), size, false));
    }
    bw = tw + 48.0f;
    bh = 24.0f + step * lines.size();
    if (x == -2) {
        // a centre print: under the banner while it shows
        bx = (v.dw - bw) * 0.5f;
        by = 300.0f - bh * 0.5f;
        if (EX_BannerOverGame()) {
            by = Q_max(by, EX_BANNER_Y + UI_Extras_BannerHeight(v) + 12.0f);
        }
    } else {
        bx = x == -1 ? (v.dw - bw) * 0.5f : x / 640.0f * v.dw - 24.0f;
        // (the stock text's middle, about 14 units over its bottom)
        by = y == -1 ? (v.dh - bh) * 0.5f : (480 - y) / 480.0f * v.dh - 14.0f - bh * 0.5f;
        bx = Com_Clamp(0.0f, Q_max(0.0f, v.dw - bw), bx);
        by = Com_Clamp(0.0f, Q_max(0.0f, v.dh - bh), by);
    }
    bx = floorf(bx);
    by = floorf(by);
    EX_EdgedBox(v, bx, by, bw, bh, alpha);
    for (size_t i = 0; i < lines.size(); i++) {
        RL_Text(v, bx + bw * 0.5f, by + 28.0f + step * i, lines[i].c_str(), size, EX_Fade(EX_TEXT, alpha), false, RL_CENTER, 0, RL_MID);
    }
}

/*
====================================================================

THE CHAT'S TYPING LINE

The DM console takes the keys as before (its modes, its history, its
commands); its window isn't drawn (ui_hud_dmconsole 0: its alphacvar).
A bar 520 x 32 of the HUD's units, 32 over the chat's menu (as it is
placed), its edge on the left and its label in the mode's color: ALL: the
accent, TEAM: the Allies' blue, TO <name>: (a private line) violet. The
line from the caret back as much as fits, the caret a 2 unit bar

====================================================================
*/

static bool ex_typing;

qboolean UI_Extras_Typing(void)
{
    return ex_typing ? qtrue : qfalse;
}

static void EX_TypingLine(void)
{
    const float s = UI_DesignScale(1920.0f, 1080.0f);
    const char *line;
    int         mode, caret;
    Menu       *menu;
    UIWidget   *chat;
    float       left, top;
    rlView_t    v;
    rlColor_t   col;
    std::string label;

    ex_typing = UI_DMConsoleState(&mode, &line, &caret);
    if (!ex_typing) {
        return;
    }
    // over the chat's menu, as it is placed (its own file may move it)
    menu = menuManager.FindMenu("hud_comp_chat");
    chat = menu ? menu->GetContainerWidget() : NULL;
    left = chat ? chat->getFrame().pos.x : 0.0f;
    top  = chat ? chat->getFrame().pos.y : uid.vidHeight - 256.0f * s;

    v.s      = s;
    v.dw     = uid.vidWidth / s;
    v.dh     = uid.vidHeight / s;
    v.layout = 1;

    if (mode == 100) {
        label = "ALL:";
        col   = EX_Accent();
    } else if (mode == 200) {
        label = "TEAM:";
        col   = EX_TEAM;
    } else {
        const char *cs = mode >= 1 && mode <= MAX_CLIENTS ? CL_ConfigString(CS_PLAYERS + mode - 1) : "";
        const char *name = cs[0] ? Info_ValueForKey(cs, "name") : "";

        label = name[0] ? std::string("TO ") + name + ":" : va("TO #%d:", mode);
        col   = EX_PRIVATE;
    }

    {
        const int   x0 = (int)floorf(left + 25.0f * s + 0.5f), x1 = (int)floorf(left + 545.0f * s + 0.5f);
        const int   y1 = (int)floorf(top - 32.0f * s + 0.5f), y0 = y1 - Q_max(12, (int)floorf(32.0f * s + 0.5f));
        const int   edge = Q_max(1, (int)floorf(3.0f * s + 0.5f)), bar = Q_max(1, (int)floorf(2.0f * s + 0.5f));
        const float mid = (y0 + y1) * 0.5f / s;
        const float lx  = x0 / s + 12.0f;
        const float tx  = lx + RL_TextW(v, label.c_str(), 17, true) + 8.0f;
        const float room = x1 / s - 12.0f - tx;
        rlFace_t    face = RL_Face(v, 17, false);
        size_t      first = 0, last;
        float       cw;

        RL_BoxPx(x0, y0, x1, y1, EX_FILL);
        RL_BoxPx(x0, y0, x0 + edge, y1, col);
        RL_Text(v, lx, mid, label.c_str(), 17, col, true, RL_LEFT, 0, RL_MID);
        if (!face.f || face.page <= 0.0f) {
            return;
        }
        // as much as fits, from the caret back (then on past it)
        caret = Q_min((int)strlen(line), Q_max(0, caret));
        cw    = 0.0f;
        first = caret;
        while (first > 0 && cw + RL_Advance(face, (byte)line[first - 1]) <= room * s) {
            cw += RL_Advance(face, (byte)line[--first]);
        }
        last = caret;
        while (line[last] && cw + RL_Advance(face, (byte)line[last]) <= room * s) {
            cw += RL_Advance(face, (byte)line[last++]);
        }
        {
            const std::string shown(line + first, last - first), before(line + first, caret - first);
            const float       cx = tx + RL_WidthPx(face, before.c_str(), 0.0f) / s;
            const int         px = (int)floorf(cx * s + 0.5f) + Q_max(1, bar / 2);

            if (!shown.empty()) {
                RL_Text(v, tx, mid, shown.c_str(), 17, EX_TEXT, false, RL_LEFT, 0, RL_MID);
            }
            RL_BoxPx(px, y0 + (int)floorf(7.0f * s + 0.5f), px + bar, y1 - (int)floorf(7.0f * s + 0.5f), EX_TEXT);
        }
    }
}

/*
====================================================================

THE QUICK MESSAGES' MENU

The V key's (the cgame's keys and commands as before): the title, the
lines numbered in the accent, "0 Cancel" muted, in the dark box with the
accent's edge on its left. Left as the radar and the chat, its top 425 of
the HUD's units down: under the game messages, over the chat (up from it as
much as it must, smaller if it can't fit under the messages' first line
and over the chat: a large cg_hud_scale). Its texts translated, measured
and placed once a page and a HUD scale

====================================================================
*/

#define IM_LEFT    25.0f
#define IM_TOP     425.0f // under the game messages (hud_comp_topleft_compass: 289 to 409)
#define IM_HIGHEST 321.0f // ... at least under their first line
#define IM_LOWEST  330.0f // up from the bottom: over the chat (CS:GO's corners: 312 up)
#define IM_EDGE    3.0f
#define IM_TEXT    19.0f // the numbers' left from the box's
#define IM_NUMBER  22.0f // the lines' from the numbers'
#define IM_RIGHT   22.0f
#define IM_HEAD    45.0f // the title's band, the rule under it
#define IM_STEP    26.0f
#define IM_SIZE    17.0f // the lines' (the title 2 smaller)

static struct {
    const char *const *lines; // the page (the cgame's static texts)
    float              s;     // ... at the HUD's scale
    rlView_t           v;     // the HUD's, smaller to fit
    std::string        head, cancel, text[9];
    int                count;      // up to the last line shown
    float              x, y, w, h; // the box (v's units)
} im;

// a key's text in the game's language, a lone "&" spaced (1.11's "Statements&Responses")
static std::string IM_Text(const char *key)
{
    std::string t = Sys_LV_CL_ConvertString(key);

    for (size_t i = t.find('&'); i != std::string::npos; i = t.find('&', i + 2)) {
        if (i > 0 && t[i - 1] != ' ') {
            t.insert(i++, " ");
        }
        if (i + 1 < t.size() && t[i + 1] != ' ') {
            t.insert(i + 1, " ");
        }
    }
    return t;
}

static void IM_Make(const char *title, const char *const *lines, int count, float s)
{
    const float dh = uid.vidHeight / s; // the HUD's units down
    float       k, tw;

    im.lines = lines;
    im.s     = s;
    im.head  = RL_Upper(title ? IM_Text(title).c_str() : "Quick Messages");
    im.count = 0;
    for (int i = 0; i < Q_min(count, (int)ARRAY_LEN(im.text)); i++) {
        im.text[i] = lines[i] && lines[i][0] ? IM_Text(lines[i]) : "";
        if (!im.text[i].empty()) {
            im.count = i + 1;
        }
    }

    im.h = IM_HEAD + 8.0f + IM_STEP * (im.count + 1) + 12.0f;
    k    = Com_Clamp(0.5f, 1.0f, (dh - IM_LOWEST - IM_HIGHEST) / im.h);
    im.v.s      = s * k;
    im.v.dw     = uid.vidWidth / im.v.s;
    im.v.dh     = uid.vidHeight / im.v.s;
    im.v.layout = 1;
    im.x        = IM_LEFT / k;
    im.y        = Q_max(IM_HIGHEST, Q_min(IM_TOP, dh - IM_LOWEST - im.h * k)) / k;

    tw = RL_TextW(im.v, im.head.c_str(), IM_SIZE - 2.0f, true, 1.2f);
    for (int i = 0; i < im.count; i++) {
        tw = Q_max(tw, IM_NUMBER + RL_TextW(im.v, im.text[i].c_str(), IM_SIZE, false));
    }
    im.cancel = Sys_LV_CL_ConvertString("Cancel");
    im.w      = ceilf(Q_max(240.0f, IM_TEXT + tw + IM_RIGHT));
}

void UI_DrawInstaMessage(const char *title, const char *const *lines, int count)
{
    const float s = UI_DesignScale(1920.0f, 1080.0f);

    if (lines != im.lines || s != im.s) {
        IM_Make(title, lines, count, s);
    }

    {
        const rlView_t& v  = im.v;
        const rlColor_t ac = EX_Accent();
        const int       x0 = RL_P(v, im.x), y0 = RL_P(v, im.y), x1 = RL_P(v, im.x + im.w), y1 = RL_P(v, im.y + im.h);
        const int       rule = RL_P(v, im.y + IM_HEAD);
        const float     tx = im.x + IM_TEXT;
        float           cy = im.y + IM_HEAD + 8.0f + IM_STEP * 0.5f;

        RL_BoxPx(x0 + Q_max(1, RL_P(v, IM_EDGE)), y0, x1, y1, EX_FILL);
        RL_BoxPx(x0, y0, x0 + Q_max(1, RL_P(v, IM_EDGE)), y1, ac);
        RL_BoxPx(RL_P(v, tx), rule, x1 - RL_P(v, IM_RIGHT), rule + Q_max(1, RL_P(v, 1.0f)), RL_Rgb(255, 255, 255, 38));
        RL_Text(v, tx, im.y + 12.0f + (IM_HEAD - 12.0f) * 0.5f, im.head.c_str(), IM_SIZE - 2.0f, EX_TEXT, true, RL_LEFT, 1.2f, RL_MID);
        for (int i = 0; i < im.count; i++, cy += IM_STEP) {
            const char number[2] = {(char)('1' + i), 0};

            if (!im.text[i].empty()) {
                RL_Text(v, tx, cy, number, IM_SIZE, ac, true, RL_LEFT, 0, RL_MID);
                RL_Text(v, tx + IM_NUMBER, cy, im.text[i].c_str(), IM_SIZE, EX_TEXT, false, RL_LEFT, 0, RL_MID);
            }
        }
        cy += 6.0f;
        RL_Text(v, tx, cy, "0", IM_SIZE, RL_MUTED, true, RL_LEFT, 0, RL_MID);
        RL_Text(v, tx + IM_NUMBER, cy, im.cancel.c_str(), IM_SIZE, RL_MUTED, false, RL_LEFT, 0, RL_MID);
    }
}

/*
====================================================================

EACH FRAME, BAKED AHEAD

====================================================================
*/

// the banner over the game: not under a scoreboard (the Reloaded one shows it above its header)
static bool EX_BannerOverGame(void)
{
    return EX_Winner() >= 0 && !UI_ScoreboardShown();
}

void UI_Extras_Draw2D(void)
{
    if (EX_BannerOverGame()) {
        EX_Banner(RL_View(), EX_Winner(), EX_BANNER_Y, 1.0f);
    }
    EX_TypingLine();
}

void UI_Extras_PreBake(qboolean ahead)
{
    const int start = Sys_Milliseconds();
    int       masks;

    if (!uii.IsRendererLoaded()) {
        return;
    }
    // the masks made while the map loads, registered at its end; at the
    // game's start (a screen size, no map yet) only begun, made meanwhile
    ZM_Begin(ZM_SCOPE);
    ZM_Begin(ZM_BINOCULARS);
    if (!ahead && clc.state >= CA_LOADING) {
        ZM_Finish(ZM_SCOPE);
        ZM_Finish(ZM_BINOCULARS);
    }
    if (ahead) {
        return;
    }
    masks = Sys_Milliseconds();

    // the faces and the emblems the prints, the banner, the typing line and the status icons draw
    {
        const rlView_t v = RL_View();
        const float    s = UI_DesignScale(1920.0f, 1080.0f);
        rlView_t       hv = v;

        hv.s  = s;
        hv.dw = uid.vidWidth / s;
        hv.dh = uid.vidHeight / s;
        UI_BakeBatch(true);
        RL_Face(v, 24, false);
        RL_Face(v, 34, true);
        RL_Face(hv, 15, true); // (the quick messages' title)
        RL_Face(hv, 17, false);
        RL_Face(hv, 17, true);
        UI_BakeBatch(false);
        rl_bakeOnly = true;
        RL_TeamIcon(v, 0, 0, 0, 38);
        RL_TeamIcon(v, 1, 0, 0, 38);
        for (int i = 0; i < UI_ICON_NUM; i++) {
            RL_Outline(EX_HudView(), ex_icons[i].glyph, 0, 0, EX_ICON_SIZE, EX_ICON_SIZE, EX_TEXT);
        }
        rl_bakeOnly = false;
    }
    Com_DPrintf("HUD extras: baked in %d ms (the masks registered in %d)\n", Sys_Milliseconds() - start, masks - start);
}

void UI_Extras_Init(void)
{
    // the DM console's window not drawn (its alphacvar): the typing line instead
    Cvar_Get("ui_hud_dmconsole", "0", 0);
}
