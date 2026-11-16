/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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

// DESCRIPTION:
// Some tools used to drawing 2d stuff

#include "cg_local.h"

/*
================
CG_AdjustFrom640

Adjusted for resolution and screen aspect ratio
================
*/
void CG_AdjustFrom640(float *x, float *y, float *w, float *h)
{
#if 0
	// adjust for wide screens
	if ( cgs.glconfig.vidWidth * 480 > cgs.glconfig.vidHeight * 640 ) {
		*x += 0.5 * ( cgs.glconfig.vidWidth - ( cgs.glconfig.vidHeight * 640 / 480 ) );
	}
#endif
    // scale for screen sizes
    *x *= cgs.screenXScale;
    *y *= cgs.screenYScale;
    *w *= cgs.screenXScale;
    *h *= cgs.screenYScale;
}

/*
=============
CG_TileClearBox

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void CG_TileClearBox(int x, int y, int w, int h, qhandle_t hShader)
{
    float s1, t1, s2, t2;

    s1 = x / 64.0;
    t1 = y / 64.0;
    s2 = (x + w) / 64.0;
    t2 = (y + h) / 64.0;
    cgi.R_DrawStretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
}

/*
==============
CG_TileClear

Clear around a sized down screen
==============
*/
void CG_TileClear(void)
{
    int top, bottom, left, right;
    int w, h;

    w = cgs.glconfig.vidWidth;
    h = cgs.glconfig.vidHeight;

    if (cg.refdef.x == 0 && cg.refdef.y == 0 && cg.refdef.width == w && cg.refdef.height == h) {
        return; // full screen rendering
    }

    top    = cg.refdef.y;
    bottom = top + cg.refdef.height - 1;
    left   = cg.refdef.x;
    right  = left + cg.refdef.width - 1;

    // clear above view screen
    CG_TileClearBox(0, 0, w, top, cgs.media.backTileShader);

    // clear below view screen
    CG_TileClearBox(0, bottom, w, h - bottom, cgs.media.backTileShader);

    // clear left of view screen
    CG_TileClearBox(0, top, left, bottom - top + 1, cgs.media.backTileShader);

    // clear right of view screen
    CG_TileClearBox(right, top, w - right, bottom - top + 1, cgs.media.backTileShader);
}

/*
===============================================================================

LAGOMETER

===============================================================================
*/

#define LAG_SAMPLES 128

typedef struct {
    int frameSamples[LAG_SAMPLES];
    int frameCount;
    int snapshotFlags[LAG_SAMPLES];
    int snapshotSamples[LAG_SAMPLES];
    int snapshotCount;
} lagometer_t;

lagometer_t lagometer;

/*
==============
CG_AddLagometerFrameInfo

Adds the current interpolate / extrapolate bar for this frame
==============
*/
void CG_AddLagometerFrameInfo(void)
{
    int offset;

    offset                                                           = cg.time - cg.latestSnapshotTime;
    lagometer.frameSamples[lagometer.frameCount & (LAG_SAMPLES - 1)] = offset;
    lagometer.frameCount++;
}

/*
==============
CG_AddLagometerSnapshotInfo

Each time a snapshot is received, log its ping time and
the number of snapshots that were dropped before it.

Pass NULL for a dropped packet.
==============
*/
void CG_AddLagometerSnapshotInfo(snapshot_t *snap)
{
    // dropped packet
    if (!snap) {
        lagometer.snapshotSamples[lagometer.snapshotCount & (LAG_SAMPLES - 1)] = -1;
        lagometer.snapshotCount++;
        return;
    }

    // add this snapshot's info
    lagometer.snapshotSamples[lagometer.snapshotCount & (LAG_SAMPLES - 1)] = snap->ping;
    lagometer.snapshotFlags[lagometer.snapshotCount & (LAG_SAMPLES - 1)]   = snap->snapFlags;
    lagometer.snapshotCount++;
}

/*
==============
CG_DrawDisconnect
==============
*/
void CG_DrawDisconnect(void)
{
    float     x, y;
    float     w, h;
    int       cmdNum;
    qhandle_t handle;
    usercmd_t cmd;

    // draw the phone jack if we are completely past our buffers
    cmdNum = cgi.GetCurrentCmdNumber() - CMD_BACKUP + 1;
    cgi.GetUserCmd(cmdNum, &cmd);
    if (!cg.snap || cmd.serverTime <= cg.snap->ps.commandTime
        || cmd.serverTime > cg.time) { // special check for map_restart
        return;
    }

    // blink it
    if ((cg.time >> 9) & 1) {
        return;
    }

    handle = cgi.R_RegisterShader("gfx/2d/net.tga");
    w      = cgi.R_GetShaderWidth(handle) * cgs.uiHiResScale[0];
    h      = cgi.R_GetShaderHeight(handle) * cgs.uiHiResScale[1];
    x      = ((float)cgs.glconfig.vidWidth - w) * 0.5;
    y      = (float)cgs.glconfig.vidHeight - h;

    cgi.R_DrawStretchPic(x, y, w, h, 0, 0, 1, 1, handle);
}

#define MAX_LAGOMETER_PING  900
#define MAX_LAGOMETER_RANGE 300

/*
==============
CG_DrawLagometer
==============
*/
void CG_DrawLagometer(void)
{
    int   a, i;
    float v;
    float ax, ay, aw, ah, mid, range;
    int   color;
    float vscale;

    if (!cg_lagometer->integer) {
        CG_DrawDisconnect();
        return;
    }

    //
    // draw the graph
    //
    ax = 272.0;
    ay = 432.0;
    aw = 96.0;
    ah = 48.0;
    CG_AdjustFrom640(&ax, &ay, &aw, &ah);

    cgi.R_SetColor(NULL);
    cgi.R_DrawStretchPic(ax, ay, aw, ah, 0, 0, 1, 1, cgs.media.lagometerShader);

    color = -1;
    range = ah / 3;
    mid   = ay + range;

    vscale = range / MAX_LAGOMETER_RANGE;

    // draw the frame interpoalte / extrapolate graph
    for (a = 0; a < aw; a++) {
        i = (lagometer.frameCount - 1 - a) & (LAG_SAMPLES - 1);
        v = lagometer.frameSamples[i];
        v *= vscale;
        if (v > 0) {
            if (color != 1) {
                color = 1;
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_YELLOW)]);
            }
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, mid - v, 1, v);
        } else if (v < 0) {
            if (color != 2) {
                color = 2;
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_BLUE)]);
            }
            v = -v;
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, mid, 1, v);
        }
    }

    // draw the snapshot latency / drop graph
    range  = ah / 2;
    vscale = range / MAX_LAGOMETER_PING;

    for (a = 0; a < aw; a++) {
        i = (lagometer.snapshotCount - 1 - a) & (LAG_SAMPLES - 1);
        v = lagometer.snapshotSamples[i];
        if (v > 0) {
            if (lagometer.snapshotFlags[i] & SNAPFLAG_RATE_DELAYED) {
                if (color != 5) {
                    color = 5; // YELLOW for rate delay
                    cgi.R_SetColor(g_color_table[ColorIndex(COLOR_YELLOW)]);
                }
            } else {
                if (color != 3) {
                    color = 3;
                    cgi.R_SetColor(g_color_table[ColorIndex(COLOR_GREEN)]);
                }
            }
            v = v * vscale;
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, ay + ah - v, 1, v);
        } else if (v < 0) {
            if (color != 4) {
                color = 4; // RED for dropped snapshots
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_RED)]);
            }
            cgi.R_DrawBox(ax + aw - a, ay + ah - range, 1, range);
        }
    }

    cgi.R_SetColor(NULL);

    CG_DrawDisconnect();
}

static void CG_DrawPauseIcon()
{
    qhandle_t handle;
    float     x, y, w, h;

    if (paused->integer) {
        handle = cgs.media.pausedShader;
    } else {
        if (cg.predicted_player_state.pm_flags & PMF_LEVELEXIT) {
            // blink it
            if ((cg.time >> 9) & 1) {
                return;
            }
            handle = cgs.media.levelExitShader;
        } else {
            return;
        }
    }
    w = cgi.R_GetShaderWidth(handle);
    h = cgi.R_GetShaderHeight(handle);
    if (cg.snap && cg.snap->ps.blend[3] > 0) {
        y = cgs.glconfig.vidHeight * 0.45f - h / 2.f;
    } else {
        y = cgs.glconfig.vidHeight * 0.75f - h / 2.f;
    }
    x = (cgs.glconfig.vidWidth - w) / 2.f;

    cgi.R_SetColor(colorWhite);
    cgi.R_DrawStretchPic(x, y, w * cgs.uiHiResScale[0], h * cgs.uiHiResScale[1], 0, 0, 1, 1, handle);
}

static void CG_DrawServerLag()
{
    float     x, y;
    float     w, h;
    qhandle_t handle;

    if (cgs.gametype == GT_SINGLE_PLAYER) {
        // Fixed in 2.0
        //  Don't draw server lag in single-player mode
        return;
    }

    if (!cg_drawsvlag->integer) {
        return;
    }

    if (!developer->integer) {
        // Changed in 2.0
        //  Svlag should only be drawn when developer mode is enabled
        return;
    }

    if (!cgs.serverLagTime) {
        return;
    }

    if (cg.time - cgs.serverLagTime > 3000) {
        return;
    }

    // blink it
    if ((cg.time >> 9) & 1) {
        return;
    }

    handle = cgi.R_RegisterShader("gfx/2d/slowserver");
    w      = (float)cgi.R_GetShaderWidth(handle) * cgs.uiHiResScale[0] / 4;
    h      = (float)cgi.R_GetShaderHeight(handle) * cgs.uiHiResScale[1] / 4;
    x      = ((float)cgs.glconfig.vidWidth - w) / 2;
    y      = (float)cgs.glconfig.vidHeight - h;
    cgi.R_DrawStretchPic(x, y, w, h, 0.0, 0.0, 1.0, 1.0, handle);
}

/*
==============
CG_DrawIcons
==============
*/
void CG_DrawIcons(void)
{
    if (!cg_hud->integer) {
        return;
    }

    CG_DrawPauseIcon();
    CG_DrawServerLag();
}

void CG_DrawOverlayTopBottom(qhandle_t handleTop, qhandle_t handleBottom, float fAlpha)
{
    int    iHalfWidth;
    int    iWidthOffset;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfWidth   = cgs.glconfig.vidHeight >> 1;
    iWidthOffset = (cgs.glconfig.vidWidth - cgs.glconfig.vidHeight) >> 1;

    cgi.R_DrawStretchPic(iWidthOffset, 0.0, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handleTop);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, 0.0, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handleTop);
    cgi.R_DrawStretchPic(iWidthOffset, iHalfWidth, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handleBottom);
    cgi.R_DrawStretchPic(
        iWidthOffset + iHalfWidth, iHalfWidth, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handleBottom
    );

    color[0] = 0.0;
    color[1] = 0.0;
    color[2] = 0.0;
    cgi.R_SetColor(color);

    cgi.R_DrawStretchPic(0.0, 0.0, iWidthOffset, cgs.glconfig.vidHeight, 0.0, 0.0, 1.0, 1.0, cgs.media.lagometerShader);
    cgi.R_DrawStretchPic(
        cgs.glconfig.vidWidth - iWidthOffset,
        0.0,
        iWidthOffset,
        cgs.glconfig.vidHeight,
        0.0,
        0.0,
        1.0,
        1.0,
        cgs.media.lagometerShader
    );
}

void CG_DrawOverlayMiddle(qhandle_t handle, float fAlpha)
{
    int    iHalfWidth;
    int    iWidthOffset;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfWidth   = cgs.glconfig.vidHeight >> 1;
    iWidthOffset = (cgs.glconfig.vidWidth - cgs.glconfig.vidHeight) >> 1;

    cgi.R_DrawStretchPic(iWidthOffset, 0.0, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, 0.0, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset, iHalfWidth, iHalfWidth, iHalfWidth, 0.0, 1.0, 1.0, 0.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, iHalfWidth, iHalfWidth, iHalfWidth, 1.0, 1.0, 0.0, 0.0, handle);

    color[0] = 0.0;
    color[1] = 0.0;
    color[2] = 0.0;
    cgi.R_SetColor(color);

    cgi.R_DrawStretchPic(0.0, 0.0, iWidthOffset, cgs.glconfig.vidHeight, 0.0, 0.0, 1.0, 1.0, cgs.media.lagometerShader);
    cgi.R_DrawStretchPic(
        cgs.glconfig.vidWidth - iWidthOffset,
        0.0,
        iWidthOffset,
        cgs.glconfig.vidHeight,
        .0,
        0.0,
        1.0,
        1.0,
        cgs.media.lagometerShader
    );
}

void CG_DrawOverlayFullScreen(qhandle_t handle, float fAlpha)
{
    int    iHalfWidth, iHalfHeight;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfHeight = cgs.glconfig.vidHeight >> 1;
    iHalfWidth  = cgs.glconfig.vidWidth >> 1;

    cgi.R_DrawStretchPic(0.0, 0.0, iHalfWidth, iHalfHeight, 0.0, 0.0, 1.0, 1.0, handle);
    cgi.R_DrawStretchPic(iHalfWidth, 0.0, iHalfWidth, iHalfHeight, 1.0, 0.0, 0.0, 1.0, handle);
    cgi.R_DrawStretchPic(0.0, iHalfHeight, iHalfWidth, iHalfHeight, 0.0, 1.0, 1.0, 0.0, handle);
    cgi.R_DrawStretchPic(iHalfWidth, iHalfHeight, iHalfWidth, iHalfHeight, 1.0, 1.0, 0.0, 0.0, handle);
}

void CG_DrawZoomOverlay()
{
    static int   zoomType;
    static float fAlpha;
    const char  *weaponstring;
    qboolean     bDrawOverlay;

    weaponstring = "";
    bDrawOverlay = qtrue;

    if (!cg.snap) {
        return;
    }

    if (cg.snap->ps.activeItems[1] >= 0) {
        weaponstring = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }

    if (!Q_stricmp(weaponstring, "Spy Camera")) {
        zoomType = 2;
    } else if (!Q_stricmp(weaponstring, "Binoculars")) {
        zoomType = 3;
    } else {
        if (cg.snap->ps.stats[STAT_INZOOM] && cg.snap->ps.stats[STAT_INZOOM] <= 30) {
            if (!Q_stricmp(weaponstring, "KAR98 - Sniper")) {
                zoomType = 1;
            } else {
                zoomType = 0;
            }
        } else {
            bDrawOverlay = qfalse;
        }
    }

    if (bDrawOverlay) {
        fAlpha += cg.frametime * 0.015;
        if (fAlpha > 1.0) {
            fAlpha = 1.0;
        }
    } else {
        fAlpha -= cg.frametime * 0.015;
        if (fAlpha < 0.0) {
            fAlpha = 0.0;
        }

        if (!fAlpha) {
            return;
        }
    }

    // Added: the Competitive HUD's (cg_hudstyle 1), drawn by the client
    if (cgi.UI_DrawZoomOverlay && cgi.UI_DrawZoomOverlay(zoomType, fAlpha)) {
        return;
    }

    switch (zoomType) {
    case 1:
        CG_DrawOverlayTopBottom(cgs.media.kar98TopOverlayShader, cgs.media.kar98BottomOverlayShader, fAlpha);
        break;
    case 3:
        CG_DrawOverlayFullScreen(cgs.media.binocularsOverlayShader, fAlpha);
        break;
    default:
        CG_DrawOverlayMiddle(cgs.media.zoomOverlayShader, fAlpha);
        break;
    }
}

void CG_HudDrawShader(int iInfo)
{
    if (cgi.HudDrawElements[iInfo].shaderName[0]) {
        cgi.HudDrawElements[iInfo].hShader = cgi.R_RegisterShaderNoMip(cgi.HudDrawElements[iInfo].shaderName);
    } else {
        cgi.HudDrawElements[iInfo].hShader = 0;
    }
}

void CG_HudDrawFont(int iInfo)
{
    if (cgi.HudDrawElements[iInfo].fontName[0]) {
        cgi.HudDrawElements[iInfo].pFont = cgi.R_LoadFont(cgi.HudDrawElements[iInfo].fontName);
    } else {
        cgi.HudDrawElements[iInfo].pFont = nullptr;
    }
}

void CG_RefreshHudDrawElements()
{
    int i;

    for (i = 0; i < MAX_HUDDRAW_ELEMENTS; ++i) {
        CG_HudDrawShader(i);
        CG_HudDrawFont(i);
    }
}

void CG_HudDrawElements()
{
    int    i;
    float  fX, fY;
    float  fWidth, fHeight;
    vec2_t virtualScale;

    if (!cg_huddraw_force->integer && !cg_hud->integer) {
        return;
    }

    virtualScale[0] = cgs.glconfig.vidWidth / 640.0;
    virtualScale[1] = cgs.glconfig.vidHeight / 480.0;

    for (i = 0; i < MAX_HUDDRAW_ELEMENTS; i++) {
        if ((!cgi.HudDrawElements[i].hShader && !cgi.HudDrawElements[i].string[0])
            || !cgi.HudDrawElements[i].vColor[3]) {
            // skip invisible elements
            continue;
        }

        fX      = cgi.HudDrawElements[i].iX;
        fY      = cgi.HudDrawElements[i].iY;
        fWidth  = cgi.HudDrawElements[i].iWidth;
        fHeight = cgi.HudDrawElements[i].iHeight;

        if (!cgi.HudDrawElements[i].bVirtualScreen) {
            fWidth *= cgs.uiHiResScale[0];
            fHeight *= cgs.uiHiResScale[1];
            fX *= cgs.uiHiResScale[0];
            fY *= cgs.uiHiResScale[1];
        }

        if (cgi.HudDrawElements[i].iHorizontalAlign == HUD_ALIGN_X_CENTER) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fX += 320.0 - fWidth * 0.5;
            } else {
                fX += cgs.glconfig.vidWidth * 0.5 - fWidth * 0.5;
            }
        } else if (cgi.HudDrawElements[i].iHorizontalAlign == HUD_ALIGN_X_RIGHT) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fX += 640.0;
            } else {
                fX += cgs.glconfig.vidWidth;
            }
        }

        if (cgi.HudDrawElements[i].iVerticalAlign == HUD_ALIGN_Y_CENTER) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fY += 240.0 - fHeight * 0.5;
            } else {
                fY += cgs.glconfig.vidHeight * 0.5 - fHeight * 0.5;
            }
        } else if (cgi.HudDrawElements[i].iVerticalAlign == HUD_ALIGN_Y_BOTTOM) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fY += 480.0;
            } else {
                fY += cgs.glconfig.vidHeight;
            }
        }

        cgi.R_SetColor(cgi.HudDrawElements[i].vColor);
        if (cgi.HudDrawElements[i].string[0]) {
            fontheader_t *pFont = cgi.HudDrawElements[i].pFont;
            if (!pFont) {
                pFont = cgs.media.hudDrawFont;
            }

            if (cgi.HudDrawElements[i].bVirtualScreen) {
                cgi.R_DrawString(pFont, cgi.LV_ConvertString(cgi.HudDrawElements[i].string), fX, fY, -1, virtualScale);
            } else {
                cgi.R_DrawString(
                    pFont,
                    cgi.LV_ConvertString(cgi.HudDrawElements[i].string),
                    fX / cgs.uiHiResScale[0],
                    fY / cgs.uiHiResScale[1],
                    -1,
                    cgs.uiHiResScale
                );
            }
        } else {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                CG_AdjustFrom640(&fX, &fY, &fWidth, &fHeight);
            }

            cgi.R_DrawStretchPic(fX, fY, fWidth, fHeight, 0.0, 0.0, 1.0, 1.0, cgi.HudDrawElements[i].hShader);
        }
    }
}

void CG_InitializeObjectives()
{
    int i;

    cg.ObjectivesAlphaTime    = 0.0;
    cg.ObjectivesBaseAlpha    = 0.0;
    cg.ObjectivesDesiredAlpha = 0.0;
    cg.ObjectivesCurrentAlpha = 0.0;

    for (i = 0; i < MAX_OBJECTIVES; i++) {
        cg.Objectives[i].flags   = 0;
        cg.Objectives[i].text[0] = 0;
    }
}

void CG_DrawObjectives()
{
    float        vColor[4];
    float        fX, fY;
    int          iNumObjectives;
    float        fObjectivesTop;
    static float fWidth;
    float        fHeight;
    int          iNumLines[20];
    int          iTotalNumLines;
    int          i;
    int          iCurrentObjective;
    float        fTimeDelta;
    const char  *pszLocalizedText;
    const char  *pszLine;
    float        fShift; // Added: pixels right of the stock place (beside the Competitive HUD's radar)

    iTotalNumLines = 0;
    for (i = CS_OBJECTIVES; i < CS_OBJECTIVES + MAX_OBJECTIVES; ++i) {
        CG_ProcessConfigString(i, qfalse);
    }

    iCurrentObjective         = atoi(CG_ConfigString(CS_CURRENT_OBJECTIVE));
    fTimeDelta                = cg.ObjectivesAlphaTime - cg.time;
    cg.ObjectivesCurrentAlpha = cg.ObjectivesDesiredAlpha;
    if (fTimeDelta > 0) {
        cg.ObjectivesCurrentAlpha =
            (cg.ObjectivesBaseAlpha - cg.ObjectivesDesiredAlpha) * sin(fTimeDelta / (M_PI * 50.f + 2.f))
            + cg.ObjectivesDesiredAlpha;
    }

    if (cg.ObjectivesCurrentAlpha < 0.02) {
        return;
    }

    // Added in 2.0
    //  Get the minimum Y value, it should be below the compass
    fObjectivesTop = cgi.UI_GetObjectivesTop();
    fShift         = cgi.UI_GetObjectivesLeft() - 25;
    iNumObjectives = 0;

    for (i = 0; i < MAX_OBJECTIVES; i++) {
        if ((cg.Objectives[i].flags == OBJ_FLAG_NONE) || (cg.Objectives[i].flags & OBJ_FLAG_HIDDEN)) {
            continue;
        }

        iNumObjectives++;
        iNumLines[i]     = 0;
        pszLocalizedText = cgi.LV_ConvertString(cg.Objectives[i].text);

        for (pszLine = strchr(pszLocalizedText, '\n'); pszLine; pszLine = strchr(pszLine + 1, '\n')) {
            iNumLines[i]++;
        }

        iTotalNumLines += iNumLines[i];
    }

    fX        = 25.0 + fShift;
    fY        = fObjectivesTop + 5;
    fWidth    = (float)(iTotalNumLines * 12 + fObjectivesTop + iNumObjectives * 25 + 32) - fY;
    vColor[2] = 0.2f;
    vColor[1] = 0.2f;
    vColor[0] = 0.2f;
    vColor[3] = cg.ObjectivesCurrentAlpha * 0.75;
    cgi.R_SetColor(vColor);
    cgi.R_DrawStretchPic(
        fX,
        fY,
        450.0 * cgs.uiHiResScale[0],
        fWidth * cgs.uiHiResScale[1],
        0.0,
        0.0,
        1.0,
        1.0,
        cgs.media.objectivesBackShader
    );

    fX        = 30.0 + fShift / cgs.uiHiResScale[0];
    fY        = fObjectivesTop + 10;
    vColor[0] = 1.0;
    vColor[1] = 1.0;
    vColor[2] = 1.0;
    vColor[3] = cg.ObjectivesCurrentAlpha;
    cgi.R_SetColor(vColor);
    cgi.R_DrawString(
        cgs.media.objectiveFont,
        cgi.LV_ConvertString("Mission Objectives:"),
        fX,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    fY = fY + 5.0;

    cgi.R_DrawString(
        cgs.media.objectiveFont,
        "_______________________________________________________",
        fX,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    fHeight = fObjectivesTop + 35 * cgs.uiHiResScale[1];

    for (i = 0; i < MAX_OBJECTIVES; ++i) {
        qhandle_t hBoxShader;

        if ((cg.Objectives[i].flags == OBJ_FLAG_NONE) || (cg.Objectives[i].flags & OBJ_FLAG_HIDDEN)) {
            continue;
        }

        if ((cg.Objectives[i].flags & OBJ_FLAG_CURRENT) != 0) {
            vColor[0]  = 1.0;
            vColor[1]  = 1.0;
            vColor[2]  = 1.0;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.uncheckedBoxShader;
        } else if ((cg.Objectives[i].flags & OBJ_FLAG_COMPLETED) != 0) {
            vColor[0]  = 0.75;
            vColor[1]  = 0.75;
            vColor[2]  = 0.75;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.checkedBoxShader;
        } else {
            vColor[0]  = 1.0;
            vColor[1]  = 1.0;
            vColor[2]  = 1.0;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.uncheckedBoxShader;
        }
        if (i == iCurrentObjective && !(cg.Objectives[i].flags & OBJ_FLAG_COMPLETED)) {
            vColor[0] = 1.0;
            vColor[1] = 1.0;
            vColor[2] = 0.0;
            vColor[3] = cg.ObjectivesCurrentAlpha;
        }

        cgi.R_SetColor(vColor);
        fX = 55.0 + fShift / cgs.uiHiResScale[0];
        fY = fHeight;
        cgi.R_DrawString(
            cgs.media.objectiveFont,
            cgi.LV_ConvertString(cg.Objectives[i].text),
            fX,
            fY / cgs.uiHiResScale[1],
            -1,
            cgs.uiHiResScale
        );

        fX        = 30.0 + fShift / cgs.uiHiResScale[0];
        fY        = fHeight;
        vColor[0] = 1.0;
        vColor[1] = 1.0;
        vColor[2] = 1.0;
        vColor[3] = cg.ObjectivesCurrentAlpha;
        cgi.R_SetColor(vColor);
        cgi.R_DrawStretchPic(
            fX * cgs.uiHiResScale[0],
            fY,
            16.0 * cgs.uiHiResScale[0],
            16.0 * cgs.uiHiResScale[1],
            0.0,
            0.0,
            1.0,
            1.0,
            hBoxShader
        );

        fHeight += iNumLines[i] * 12 + 25 * cgs.uiHiResScale[1];
    }
}

void CG_DrawPlayerTeam()
{
    qhandle_t handle;
    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cgs.gametype <= GT_FFA) {
        return;
    }

    handle = 0;
    if (cg.snap->ps.stats[STAT_TEAM] == 3) {
        handle = cgi.R_RegisterShader("textures/hud/allies");
    } else if (cg.snap->ps.stats[STAT_TEAM] == 4) {
        handle = cgi.R_RegisterShader("textures/hud/axis");
    }

    if (handle) {
        cgi.R_SetColor(NULL);
        cgi.R_DrawStretchPic(
            96.0 * cgs.uiHiResScale[0],
            cgs.glconfig.vidHeight - 46 * cgs.uiHiResScale[1],
            24.0 * cgs.uiHiResScale[0],
            24.0 * cgs.uiHiResScale[1],
            0.0,
            0.0,
            1.0,
            1.0,
            handle
        );
    }
}

void CG_DrawPlayerEntInfo()
{
    int         iClientNum;
    const char *pszClientInfo;
    const char *pszName;
    float       fX, fY;
    float       color[4];
    qhandle_t   handle;

    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cg.snap->ps.stats[STAT_INFOCLIENT] == -1) {
        return;
    }

    iClientNum    = cg.snap->ps.stats[STAT_INFOCLIENT];
    handle        = 0;
    pszClientInfo = CG_ConfigString(iClientNum + CS_PLAYERS);
    pszName       = Info_ValueForKey(pszClientInfo, "name");

    color[0] = 0.5;
    color[1] = 1.0;
    color[2] = 0.5;
    color[3] = 1.0;

    fX = 56.0;
    fY = (float)cgs.glconfig.vidHeight * 0.5;

    if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
        handle = cgi.R_RegisterShader("textures/hud/allies");
    } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
        handle = cgi.R_RegisterShader("textures/hud/axis");
    }

    if (handle) {
        cgi.R_SetColor(0);
        cgi.R_DrawStretchPic(
            fX, fY, 16.0 * cgs.uiHiResScale[0], 16.0 * cgs.uiHiResScale[1], 0.0, 0.0, 1.0, 1.0, handle
        );
    }

    cgi.R_SetColor(color);
    cgi.R_DrawString(
        cgs.media.hudDrawFont,
        (char *)pszName,
        fX / cgs.uiHiResScale[0] + 24.0,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    cgi.R_DrawString(
        cgs.media.hudDrawFont,
        va("%i", cg.snap->ps.stats[STAT_INFOCLIENT_HEALTH]),
        fX / cgs.uiHiResScale[0] + 24.0,
        fY / cgs.uiHiResScale[1] + 20.0,
        -1,
        cgs.uiHiResScale
    );
}

void CG_UpdateAttackerDisplay()
{
    int         iClientNum;
    const char *pszClientInfo;
    const char *pszName;
    float       fX, fY;
    float       color[4];

    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cg.snap->ps.stats[STAT_ATTACKERCLIENT] == -1) {
        return;
    }

    iClientNum    = cg.snap->ps.stats[STAT_ATTACKERCLIENT];
    pszClientInfo = CG_ConfigString(CS_PLAYERS + iClientNum);
    pszName       = Info_ValueForKey(pszClientInfo, "name");

    color[3] = 1.0;
    fY       = (float)(cgs.glconfig.vidHeight - 90);
    fX       = 56.0;

    if (cgs.gametype > GT_FFA) {
        qhandle_t handle;

        handle = 0;
        if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
            handle = cgi.R_RegisterShader("textures/hud/allies");
        } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
            handle = cgi.R_RegisterShader("textures/hud/axis");
        }

        if (handle) {
            cgi.R_SetColor(0);
            cgi.R_DrawStretchPic(
                56.0 * cgs.uiHiResScale[0],
                fY,
                24.0 * cgs.uiHiResScale[0],
                24.0 * cgs.uiHiResScale[1],
                0.0,
                0.0,
                1.0,
                1.0,
                handle
            );
        }

        if ((cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS)
            && cg.clientinfo[iClientNum].team == cg.snap->ps.stats[STAT_TEAM]) {
            color[0] = 0.5;
            color[1] = 1.0;
            color[2] = 0.5;
        } else {
            color[0] = 1.0;
            color[1] = 0.5;
            color[2] = 0.5;
        }

        fX = 56.0;
    } else {
        color[0] = 1.0;
        color[1] = 0.5;
        color[2] = 0.5;
    }

    cgi.R_SetColor(color);
    cgi.R_DrawString(
        cgs.media.attackerFont, pszName, fX / cgs.uiHiResScale[0] + 32.0, fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );
}

void CG_UpdateCountdown()
{
    const char *message = "";

    if (!cg.snap) {
        return;
    }

    if (cg.matchStartTime != -1) {
        if (cgs.gametype != GT_LIBERATION) {
            int iSecondsLeft, iMinutesLeft;

            iSecondsLeft = (cgs.matchEndTime - cg.time) / 1000;
            if (iSecondsLeft >= 0) {
                iMinutesLeft = iSecondsLeft / 60;
                message      = va("%s %2i:%02i", cgi.LV_ConvertString("Time Left:"), iMinutesLeft, iSecondsLeft % 60);
            } else if (!cgs.matchEndTime) {
                message = "";
            }
        } else {
            // No clock on liberation game mode
            message = "";
        }
    } else {
        // The match has not started yet
        message = "Waiting For Players";
    }

    if (strcmp(ui_timemessage->string, message)) {
        cgi.Cvar_Set("ui_timemessage", message);
    }
}

static void CG_RemoveStopwatch()
{
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_stopwatch\n");
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse\n");
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse_wet\n");
}

void CG_DrawStopwatch()
{
    int iFraction;

    if (!cg_hud->integer) {
        CG_RemoveStopwatch();
        return;
    }

    if (!cgi.stopWatch->iStartTime) {
        CG_RemoveStopwatch();
        return;
    }

    if (cgi.stopWatch->iStartTime >= cgi.stopWatch->iEndTime) {
        CG_RemoveStopwatch();
        return;
    }

    if (cgi.stopWatch->iEndTime <= cg.time) {
        CG_RemoveStopwatch();
        return;
    }

    if (cg.ObjectivesCurrentAlpha >= 0.02) {
        CG_RemoveStopwatch();
        return;
    }

    if (cg.snap && cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        CG_RemoveStopwatch();
        return;
    }
    if (cgi.stopWatch->eType >= SWT_FUSE_WET) {
        iFraction = cgi.stopWatch->iEndTime - cgi.stopWatch->iStartTime;
    } else {
        iFraction = cgi.stopWatch->iEndTime - cg.time;
    }

    cgi.Cvar_Set("ui_stopwatch", va("%i", iFraction));

    switch (cgi.stopWatch->eType) {
    case SWT_NORMAL:
    default:
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_stopwatch\n");
        break;
    case SWT_FUSE:
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_fuse\n");
        break;
    case SWT_FUSE_WET:
        cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse\n");
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_fuse_wet\n");
        break;
    }
}

void CG_DrawInstantMessageMenu()
{
    float     w, h;
    float     x, y;
    qhandle_t handle;

    if (!cg.iInstaMessageMenu) {
        return;
    }

    // Added: the client's list (the Competitive HUD's) with 1.11's groups and
    // lines; 2.0's and later's groups' names aren't known (their pictures')
    if (cg_protocol < PROTOCOL_MOHTA_MIN && cgi.UI_DrawInstaMessage
        && (cg.iInstaMessageMenu < 0
                ? cgi.UI_DrawInstaMessage(NULL, pInstantMsgGroupsEng_ver6, ARRAY_LEN(pInstantMsgGroupsEng_ver6))
                : cgi.UI_DrawInstaMessage(
                      pInstantMsgGroupsEng_ver6[cg.iInstaMessageMenu - 1],
                      pInstantMsgEng_ver6[cg.iInstaMessageMenu - 1],
                      ARRAY_LEN(pInstantMsgEng_ver6[0])
                  ))) {
        return;
    }

    if (cg.iInstaMessageMenu > 0) {
        handle = cgi.R_RegisterShader(va("textures/hud/instamsg_group_%c", cg.iInstaMessageMenu + 96));
    } else {
        handle = cgi.R_RegisterShader("textures/hud/instamsg_main");
    }

    w = cgi.R_GetShaderWidth(handle);
    h = cgi.R_GetShaderHeight(handle);
    x = 8.0;
    y = ((float)cgs.glconfig.vidHeight - h) * 0.5;
    cgi.R_SetColor(0);
    cgi.R_DrawStretchPic(
        x * cgs.uiHiResScale[0], y, w * cgs.uiHiResScale[0], h * cgs.uiHiResScale[1], 0.0, 0.0, 1.0, 1.0, handle
    );
}

void CG_DrawSpectatorView_ver_15()
{
    const char *pszString;
    int         iKey1, iKey2;
    int         iKey1b, iKey2b;
    float       fX, fY;
    qboolean    bOnTeam;

    if (!(cg.predicted_player_state.pm_flags & PMF_SPECTATING)) {
        return;
    }

    bOnTeam = qfalse;
    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
        bOnTeam = 1;
    }

    if (!bOnTeam) {
        cgi.Key_GetKeysForCommand("+attackprimary", &iKey1, &iKey2);
        pszString = cgi.LV_ConvertString(va("Press Fire(%s) to join the battle!", cgi.Key_KeynumToBindString(iKey1)));
        fX        = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = cgs.glconfig.vidHeight - 64.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(NULL);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) {
        cgi.Key_GetKeysForCommand("+moveup", &iKey1, &iKey2);
        cgi.Key_GetKeysForCommand("+movedown", &iKey1b, &iKey2b);

        pszString = cgi.LV_ConvertString(
            va("Press Jump(%s) or Duck(%s) to follow a different player.",
               cgi.Key_KeynumToBindString(iKey1),
               cgi.Key_KeynumToBindString(iKey1b))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 40.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (!bOnTeam && (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);
        pszString =
            cgi.LV_ConvertString(va("Press Use(%s) to enter free spectate mode.", cgi.Key_KeynumToBindString(iKey1)));

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);

        pszString = cgi.LV_ConvertString(
            va("Press Use(%s) to enter player following spectate mode.", cgi.Key_KeynumToBindString(iKey1))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if ((cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) != 0 && cg.snap
        && cg.snap->ps.stats[STAT_INFOCLIENT] != -1) {
        int       iClientNum;
        qhandle_t hShader;
        vec4_t    color;
        char      buf[128];

        iClientNum = cg.snap->ps.stats[STAT_INFOCLIENT];
        Com_sprintf(
            buf, sizeof(buf), "%s : %i", cg.clientinfo[iClientNum].name, cg.snap->ps.stats[STAT_INFOCLIENT_HEALTH]
        );

        hShader  = 0;
        color[0] = 0.5;
        color[1] = 1.0;
        color[2] = 0.5;
        color[3] = 1.0;

        fX = (float)(cgs.glconfig.vidWidth
                     - (cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) - 16) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 80.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(color);
        cgi.R_DrawString(
            cgs.media.attackerFont, buf, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );

        if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
            hShader = cgi.R_RegisterShader("textures/hud/allies");
        } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
            hShader = cgi.R_RegisterShader("textures/hud/axis");
        }

        if (hShader) {
            cgi.R_SetColor(NULL);
            cgi.R_DrawStretchPic(
                fX - 20.0 * cgs.uiHiResScale[0],
                fY,
                16.0 * cgs.uiHiResScale[0],
                16.0 * cgs.uiHiResScale[1],
                0.0,
                0.0,
                1.0,
                1.0,
                hShader
            );
        }
    }
}

void CG_DrawSpectatorView_ver_6()
{
    const char *pszString;
    int         iKey1, iKey2;
    int         iKey1b, iKey2b;
    float       fX, fY;
    qboolean    bOnTeam;

    if (!(cg.predicted_player_state.pm_flags & PMF_SPECTATING)) {
        return;
    }

    bOnTeam = qfalse;
    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
        bOnTeam = 1;
    }

    // retrieve keys for +use
    cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);

    if (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) {
        pszString =
            cgi.LV_ConvertString(va("Press Use(%s) to follow a different player.", cgi.Key_KeynumToBindString(iKey1)));
    } else {
        pszString = cgi.LV_ConvertString(va("Press Use(%s) to follow a player.", cgi.Key_KeynumToBindString(iKey1)));
    }

    fX = (float)(cgs.glconfig.vidWidth
                 - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
       * 0.5;
    fY = (float)cgs.glconfig.vidHeight - 40.0 * cgs.uiHiResScale[1];
    cgi.R_SetColor(0);
    cgi.R_DrawString(
        cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    if (!bOnTeam && (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+moveup", &iKey1, &iKey2);
        cgi.Key_GetKeysForCommand("+movedown", &iKey1b, &iKey2b);
        pszString = cgi.LV_ConvertString(
            va("Press Jump(%s) or Duck(%s) to free spectate.",
               cgi.Key_KeynumToBindString(iKey1),
               cgi.Key_KeynumToBindString(iKey1b))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }
}

void CG_DrawSpectatorView()
{
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        CG_DrawSpectatorView_ver_15();
    } else {
        CG_DrawSpectatorView_ver_6();
    }
}

// The crosshair: its boxes in whole pixels, laid out again on a resolution change and when a setting changes
#define CROSSHAIR_PIECES 5  // four arms and the center dot
#define CROSSHAIR_BOXES  64 // the pieces and their outline strips as boxes that don't overlap

static struct {
    vec4_t color, outlineColor;
    float  boxes[CROSSHAIR_BOXES][4];   // x, y, width, height: the pieces
    float  outline[CROSSHAIR_BOXES][4]; // the outline, drawn first in its own color
    int    count, outlineCount;
    int    settings; // the settings' modification counts added up, as laid out
} crosshairLayout;

static cvar_t **const crosshairSettings[] = {
    &cg_crosshair_length, &cg_crosshair_gap, &cg_crosshair_thickness, &cg_crosshair_color, &cg_crosshair_alpha,
    &cg_crosshair_outline, &cg_crosshair_outline_color, &cg_crosshair_outline_alpha, &cg_crosshair_dot, &cg_crosshair_t
};

// modification counts only grow: their sum changes whenever a setting does
static int CG_CrosshairSettings(void)
{
    int i, sum = 0;

    for (i = 0; i < (int)ARRAY_LEN(crosshairSettings); i++) {
        sum += (*crosshairSettings[i])->modificationCount;
    }
    return sum;
}

// a size in pixels at 1080 lines in whole pixels at this height, as CS2 rescales its sizes: rounded half away from
// zero, and at least 1 if above 0 (0 below 0 and for NaN: a negative gap draws as 0, as on CS2's static cross)
static int CG_CrosshairPixels(float value)
{
    if (!(value > 0.0f)) {
        return 0;
    }
    return Q_max((int)(Q_min(value, 4096.0f) * cgs.glconfig.vidHeight / 1080.0f + 0.5f), 1);
}

static void CG_CrosshairColor(const cvar_t *hex, unsigned int fallback, const cvar_t *alpha, vec4_t color)
{
    unsigned int rgb = Com_HexColor(hex->string, fallback);

    VectorSet4(
        color,
        (rgb >> 16) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        Q_min(Q_max(alpha->integer, 0), 255) / 255.0f
    );
}

// the spans of the boxes (x, y, width, height) across the band y0 to y1, sorted and joined where they touch: how many
static int CG_CrosshairSpans(int (*boxes)[4], int n, int y0, int y1, int (*spans)[2])
{
    int ns = 0, m = 0, i, j, x0, x1;

    for (i = 0; i < n; i++) {
        if (boxes[i][1] <= y0 && boxes[i][1] + boxes[i][3] >= y1 && boxes[i][2] > 0) {
            for (x0 = boxes[i][0], x1 = x0 + boxes[i][2], j = ns++; j > 0 && spans[j - 1][0] > x0; j--) {
                spans[j][0] = spans[j - 1][0];
                spans[j][1] = spans[j - 1][1];
            }
            spans[j][0] = x0;
            spans[j][1] = x1;
        }
    }
    for (i = 0; i < ns; i++) {
        if (m && spans[i][0] <= spans[m - 1][1]) {
            spans[m - 1][1] = Q_max(spans[m - 1][1], spans[i][1]);
        } else {
            spans[m][0] = spans[i][0];
            spans[m][1] = spans[i][1];
            m++;
        }
    }
    return m;
}

// the span x0 to x1 across the band y0 to y1 as a box: a box of the bands above going on down when just as wide, else
// a new one if it fits in max. How many boxes there are
static int CG_CrosshairBox(float (*out)[4], int count, int above, int max, int x0, int x1, int y0, int y1)
{
    int i;

    for (i = 0; i < above; i++) {
        if (out[i][0] == x0 && out[i][2] == x1 - x0 && out[i][1] + out[i][3] == y0) {
            out[i][3] += y1 - y0;
            return count;
        }
    }
    if (count < max) {
        VectorSet4(out[count], x0, y0, x1 - x0, y1 - y0);
        count++;
    }
    return count;
}

// the pixels of n boxes less those of nm others (x, y, width, height), as boxes that don't overlap (at most max):
// in each band between the boxes' top and bottom edges, the first boxes' spans less the others'. CS2 draws each pixel
// once where pieces (or outline strips) meet, and the outline nowhere under a piece
static int CG_CrosshairUnion(int (*in)[4], int n, int (*minus)[4], int nm, float (*out)[4], int max)
{
    int edges[CROSSHAIR_PIECES * 10], spans[CROSSHAIR_PIECES * 4][2], cut[CROSSHAIR_PIECES][2];
    int ne = 0, count = 0, i, j, k, e, ns, nc, x0, x1;

    for (i = 0; i < n + nm; i++) {
        const int *b = i < n ? in[i] : minus[i - n];

        edges[ne++] = b[1];
        edges[ne++] = b[1] + b[3];
    }
    for (i = 1; i < ne; i++) {
        for (e = edges[i], j = i; j > 0 && edges[j - 1] > e; j--) {
            edges[j] = edges[j - 1];
        }
        edges[j] = e;
    }

    for (k = 0; k + 1 < ne; k++) {
        const int y0 = edges[k], y1 = edges[k + 1], above = count;

        if (y0 == y1) {
            continue;
        }
        ns = CG_CrosshairSpans(in, n, y0, y1, spans);
        nc = CG_CrosshairSpans(minus, nm, y0, y1, cut);
        for (i = 0; i < ns; i++) {
            // the span up to each cut, then on from its end
            for (x0 = spans[i][0], j = 0; j <= nc && x0 < spans[i][1]; j++) {
                x1 = j < nc ? Q_min(Q_max(cut[j][0], x0), spans[i][1]) : spans[i][1];
                if (x1 > x0) {
                    count = CG_CrosshairBox(out, count, above, max, x0, x1, y0, y1);
                }
                if (j < nc) {
                    x0 = Q_max(x0, cut[j][1]);
                }
            }
        }
    }
    return count;
}

/*
====================
CG_UpdateCrosshair

As CS2 draws its static cross, the sizes pixels at 1080 lines scaled with the screen's height (CG_CrosshairPixels).
Around the screen's middle pixel (width / 2, height / 2), a line t pixels thick has (t + 1) / 2 of them before it.
The gap counts from that pixel: the left arm ends gap pixels before it, the right arm starts gap pixels after it,
a pixel sooner when t is odd (the same for the top and bottom); at gap 0 the arms meet. Length 0 draws no arms, the
dot is the middle t by t, cg_crosshair_t has no top arm. The outline is always one pixel, round every piece (1) or
above and left of it only (2, CS2's half outline), and nowhere under a piece. By default CS2's default crosshair
without its outline, white: arms two pixels thick and eight long, a gap of four. Out-of-range settings are clamped
here, never rewritten: crosshair_export gives back the code they were imported from
====================
*/
void CG_UpdateCrosshair(void)
{
    int pieces[CROSSHAIR_PIECES][4], strips[CROSSHAIR_PIECES * 4][4];
    int length, thickness, gap, before, odd, x, y, n, ns, mode, i;

    length    = CG_CrosshairPixels(cg_crosshair_length->value);
    thickness = CG_CrosshairPixels(cg_crosshair_thickness->value);
    gap       = CG_CrosshairPixels(cg_crosshair_gap->value);
    before    = (thickness + 1) / 2;
    odd       = thickness & 1;
    x         = cgs.glconfig.vidWidth / 2;
    y         = cgs.glconfig.vidHeight / 2;

    CG_CrosshairColor(cg_crosshair_color, 0xFFFFFF, cg_crosshair_alpha, crosshairLayout.color);
    CG_CrosshairColor(cg_crosshair_outline_color, 0x000000, cg_crosshair_outline_alpha, crosshairLayout.outlineColor);

    n = 0;
    if (thickness && length) {
        VectorSet4(pieces[n], x - gap - length, y - before, length, thickness);
        n++;
        VectorSet4(pieces[n], x + gap - odd, y - before, length, thickness);
        n++;
        VectorSet4(pieces[n], x - before, y + gap - odd, thickness, length);
        n++;
        if (!cg_crosshair_t->integer) {
            VectorSet4(pieces[n], x - before, y - gap - length, thickness, length);
            n++;
        }
    }
    if (thickness && cg_crosshair_dot->integer) {
        VectorSet4(pieces[n], x - before, y - before, thickness, thickness);
        n++;
    }

    mode = Q_min(Q_max(cg_crosshair_outline->integer, 0), 2);
    for (ns = 0, i = 0; mode && i < n; i++) {
        const int *p = pieces[i];

        // above (over the corners) and left; full: below and right too
        VectorSet4(strips[ns], p[0] - 1, p[1] - 1, p[2] + (mode == 1 ? 2 : 1), 1);
        ns++;
        VectorSet4(strips[ns], p[0] - 1, p[1], 1, p[3]);
        ns++;
        if (mode == 1) {
            VectorSet4(strips[ns], p[0] - 1, p[1] + p[3], p[2] + 2, 1);
            ns++;
            VectorSet4(strips[ns], p[0] + p[2], p[1], 1, p[3]);
            ns++;
        }
    }

    crosshairLayout.count        = CG_CrosshairUnion(pieces, n, NULL, 0, crosshairLayout.boxes, CROSSHAIR_BOXES);
    crosshairLayout.outlineCount = CG_CrosshairUnion(strips, ns, pieces, n, crosshairLayout.outline, CROSSHAIR_BOXES);
    crosshairLayout.settings     = CG_CrosshairSettings();
}

void CG_DrawCrosshair()
{
    if (!cg_hud->integer || !ui_crosshair->integer || !cg.snap) {
        return;
    }

    // where the stock image was drawn, without its friend check (the teammate image is in Spearhead and
    // Breakthrough only), leaving the 2D color white as it did
    if ((cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) || !cg.snap->ps.stats[STAT_CROSSHAIR]) {
        return;
    }

    if (CG_CrosshairSettings() != crosshairLayout.settings) {
        CG_UpdateCrosshair();
    }

    cgi.R_SetColor(NULL);
    if (crosshairLayout.outlineCount) {
        cgi.R_DrawBoxes(crosshairLayout.outline, crosshairLayout.outlineCount, crosshairLayout.outlineColor);
    }
    if (crosshairLayout.count) {
        cgi.R_DrawBoxes(crosshairLayout.boxes, crosshairLayout.count, crosshairLayout.color);
    }
}

void CG_DrawVote()
{
    const char *text;
    int         seconds;
    int         percentYes;
    int         percentNo;
    int         percentUndecided;
    float       x, y;
    vec4_t      col;

    if (!cgs.voteTime) {
        return;
    }

    if (cgs.voteRefreshed) {
        cgs.voteRefreshed = qfalse;
    }

    seconds = (30000 - (cg.time - cgs.voteTime)) / 1000 + 1;
    if (seconds < 0) {
        seconds = 0;
    }

    percentYes       = cgs.numVotesYes * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);
    percentNo        = cgs.numVotesNo * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);
    percentUndecided = cgs.numUndecidedVotes * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);

    x = 8 * cgs.uiHiResScale[0];
    y = ((cgs.glconfig.vidHeight > 480) ? (cgs.glconfig.vidHeight * 0.725f) : (cgs.glconfig.vidHeight * 0.75f));

    cgi.R_SetColor(NULL);

    text = va("%s: %s", cgi.LV_ConvertString("Vote Running"), cgs.voteString);
    cgi.R_DrawString(
        cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    y += 12 * cgs.uiHiResScale[1];

    text =
        va("%s: %isec  %s: %i%%  %s: %i%%  %s: %i%%",
           cgi.LV_ConvertString("Time"),
           seconds,
           cgi.LV_ConvertString("Yes"),
           percentYes,
           cgi.LV_ConvertString("No"),
           percentNo,
           cgi.LV_ConvertString("Undecided"),
           percentUndecided);
    cgi.R_DrawString(
        cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    if (cg.snap && !cg.snap->ps.voted) {
        col[0] = 0.5;
        col[1] = 1.0;
        col[2] = 0.5;
        col[3] = 1.0;
        cgi.R_SetColor(col);

        y += 12 * cgs.uiHiResScale[1];

        text = cgi.LV_ConvertString("Vote now, it's your patriotic duty!");
        cgi.R_DrawString(
            cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );

        y += 12 * cgs.uiHiResScale[1];

        text = cgi.LV_ConvertString("To vote Yes, press F1. To vote No, press F2.");
        cgi.R_DrawString(
            cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
        cgi.R_SetColor(NULL);
    }
}

/*
==============
CG_Draw2D
==============
*/
void CG_Draw2D(void)
{
    // Added: no HUD under the Reloaded weapon and Esc menus (the client's
    // ui_menucover); a scope's mask stays, being the view
    static cvar_t *ui_menucover = cgi.Cvar_Get("ui_menucover", "0", 0);

    CG_UpdateCountdown();
    CG_DrawZoomOverlay();
    if (ui_menucover->integer) {
        return;
    }
    CG_DrawLagometer();
    CG_HudDrawElements();
    CG_DrawObjectives();
    CG_DrawIcons();
    // Added: the Competitive HUD's URC menus (cg_hudstyle 1 or 2, the client's
    // cl_uicomp.cpp) show the stopwatch, the spectator's keys, the team, the
    // player aimed at, the attacker and the vote their own way
    static cvar_t *cg_hudstyle = cgi.Cvar_Get("cg_hudstyle", "1", CVAR_ARCHIVE);
    if (cg_hudstyle->integer <= 0) {
        CG_DrawStopwatch();
        CG_DrawSpectatorView();
        CG_DrawPlayerTeam();
        CG_DrawPlayerEntInfo();
        CG_UpdateAttackerDisplay();
        CG_DrawVote();
    }
    CG_DrawInstantMessageMenu();
    CG_DrawCrosshair();
}
