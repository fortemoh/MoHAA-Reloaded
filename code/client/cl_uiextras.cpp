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

// cl_uiextras.cpp: the Competitive HUD's extras (Added, cg_hudstyle 1).
// Drawn in place of the stock ones, nothing of them while they don't show:
//
// - the zoom overlays: the scopes (a circle, its edge darkening into black,
//   thin lines: a cross, the Kar98's without its upper half) and the
//   binoculars (two circles, a soft rim, a small range scale). A mask, the
//   screen's top left quarter at its pixels, is made once a screen size (on
//   the bake threads, while a map loads) and drawn four times mirrored; the
//   black around it and the lines are boxes.

#include "client.h"
#include "cl_ui.h"
#include "cl_uicomp.h"
#include "cl_uiextras.h"
#include "../uilib/uibake.h"

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
    qhandle_t         shader;
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

static void ZM_Make(zoomMask_t *m, int kind, int w, int h)
{
    if (kind == ZM_SCOPE) {
        ZM_Scope(m, w, h);
    } else {
        ZM_Binoculars(m, w, h);
    }
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
        try {
            m->job.get();
        } catch (const std::future_error&) {
            // (dropped before it ran: the bake threads' queue let go)
            ZM_Make(m, kind, m->w, m->h);
        }
        m->making = false;
        m->shader = re.RegisterTexture2D(zm_names[kind], m->texels.data(), m->qw, m->qh);
        m->refSeq = uii.GetRefSequence();
        m->stamp  = UI_BakeStamp();
        std::vector<byte>().swap(m->texels);
    } else if (m->shader && m->stamp != UI_BakeStamp() && m->refSeq == uii.GetRefSequence()) {
        m->shader = re.RegisterShaderNoMip(zm_names[kind]);
        m->stamp  = UI_BakeStamp();
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
UI_DrawZoomOverlay

The cgame's zoom overlay (its fade kept), with cg_hudstyle 1: the mask
four times (mirrored right and down: an odd size's middle column and row
once), black boxes round it, then the lines as boxes on whole pixels, the
crosshair's way: the scopes' a pixel at 1080 lines (85 % black), the
binoculars' two (90 %): a line 0.5 of the unit across, ticks every 0.05 up
from it (the middle and the ends twice as tall), and down from its middle a
line 0.1 long crossed every 0.025
================
*/
qboolean UI_DrawZoomOverlay(int type, float alpha)
{
    const int   kind = type == 3 ? ZM_BINOCULARS : ZM_SCOPE;
    zoomMask_t *m = &zm[kind];
    const int   w = uid.vidWidth, h = uid.vidHeight;
    const int   xm = (w + 1) / 2, ym = (h + 1) / 2;
    float       boxes[24][4];
    vec4_t      color = {1, 1, 1, alpha};
    int         n = 0;

    if (!UI_CompHud_Active() || alpha <= 0.0f) {
        return qfalse;
    }
    // (made now only if the load didn't: a new size, the HUD turned on)
    if (m->w != w || m->h != h || (!m->making && (!m->shader || m->refSeq != uii.GetRefSequence()))) {
        ZM_Begin(kind);
    }
    ZM_Finish(kind);
    if (!m->shader) {
        return qfalse;
    }

    {
        const int   rw = m->qw - (w & 1), bh = m->qh - (h & 1);
        const float s1 = (float)rw / m->qw, t1 = (float)bh / m->qh;

        re.SetColor(color);
        re.DrawStretchPic(m->xl, m->yt, m->qw, m->qh, 0, 0, 1, 1, m->shader);
        re.DrawStretchPic(xm, m->yt, rw, m->qh, s1, 0, 0, 1, m->shader);
        re.DrawStretchPic(m->xl, ym, m->qw, bh, 0, t1, 1, 0, m->shader);
        re.DrawStretchPic(xm, ym, rw, bh, s1, t1, 0, 0, m->shader);
    }

    // the black round it
    ZM_Box(boxes, &n, 0, 0, m->xl, h);
    ZM_Box(boxes, &n, w - m->xl, 0, m->xl, h);
    ZM_Box(boxes, &n, m->xl, 0, w - 2 * m->xl, m->yt);
    ZM_Box(boxes, &n, m->xl, h - m->yt, w - 2 * m->xl, m->yt);
    color[0] = color[1] = color[2] = 0.0f;
    if (n) {
        re.DrawBoxes((const float (*)[4])boxes, n, color);
    }

    n = 0;
    if (kind == ZM_SCOPE) {
        const int   t  = Q_max(1, (int)floorf(h / 1080.0f + 0.5f));
        const float R  = 0.46f * h;
        const int   x0 = (int)floorf(w * 0.5f - R), x1 = (int)ceilf(w * 0.5f + R);
        const int   y0 = (int)floorf(h * 0.5f - R), y1 = (int)ceilf(h * 0.5f + R);
        const int   lx = (w - t) / 2, ly = (h - t) / 2;

        ZM_Box(boxes, &n, x0, ly, x1 - x0, t);
        // (the vertical line in two, the crossing drawn once; the Kar98's without its upper half)
        if (type != 1) {
            ZM_Box(boxes, &n, lx, y0, t, ly - y0);
        }
        ZM_Box(boxes, &n, lx, ly + t, t, y1 - ly - t);
        color[3] = 0.85f * alpha;
    } else {
        const float u  = ZM_BinocularUnit(w, h);
        const int   t  = Q_max(1, (int)floorf(2.0f * h / 1080.0f + 0.5f));
        const int   lx = (w - t) / 2, ly = (h - t) / 2;
        const int   half = (int)floorf(0.25f * u + 0.5f);
        const int   tick = Q_max(t, (int)floorf(0.015f * u + 0.5f)), big = Q_max(t, (int)floorf(0.03f * u + 0.5f));
        const int   down = (int)floorf(0.1f * u + 0.5f), cross = Q_max(1, (int)floorf(0.008f * u + 0.5f));

        ZM_Box(boxes, &n, lx - half, ly, 2 * half + t, t);
        for (int i = -5; i <= 5; i++) {
            const int x = lx + (int)floorf(i * 0.05f * u + 0.5f), th = i % 5 ? tick : big;

            ZM_Box(boxes, &n, x, ly - th, t, th);
        }
        ZM_Box(boxes, &n, lx, ly + t, t, down - t);
        for (int j = 1; j <= 4; j++) {
            const int y = ly + (int)floorf(j * 0.025f * u + 0.5f);

            ZM_Box(boxes, &n, lx - cross, y, cross, t);
            ZM_Box(boxes, &n, lx + t, y, cross, t);
        }
        color[3] = 230.0f / 255.0f * alpha;
    }
    if (n) {
        re.DrawBoxes((const float (*)[4])boxes, n, color);
    }
    re.SetColor(NULL);
    return qtrue;
}

/*
====================================================================

BAKED AHEAD

====================================================================
*/

void UI_Extras_PreBake(qboolean ahead)
{
    if (!UI_CompHud_Active() || !uii.IsRendererLoaded()) {
        return;
    }
    // the masks made while the map loads, registered at its end
    ZM_Begin(ZM_SCOPE);
    ZM_Begin(ZM_BINOCULARS);
    if (!ahead) {
        ZM_Finish(ZM_SCOPE);
        ZM_Finish(ZM_BINOCULARS);
    }
}
