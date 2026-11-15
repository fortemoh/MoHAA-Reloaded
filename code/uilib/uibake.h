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

// uibake.h: what the UI bakes into pictures (Added): TrueType faces, shapes,
// PNG pictures and SVG outlines, rasterized once at the size they are drawn

#pragma once

#include <functional>
#include <future>

// a face: "<file>-<px>[s][i][p]", fonts/<file>.ttf at px pixels of a 1080-line
// screen (UI_DesignScale; p: px pixels of the screen), s a drop shadow, i
// slanted; NULL when not a face
fontheader_t *UI_BakeFont(const char *name);
// a baked face's page scale: drawn across at it, a texel is a pixel (0: not baked)
float UI_BakedFontScale(const fontheader_t *font);

// a shape's layers (the URC's "shape" lines) baked at w x h pixels, scale
// pixels a design unit; key names it among the others
qhandle_t UI_BakeShape(const char *key, const char *const *layers, int numLayers, int w, int h, float scale);
// the same layers' texels (RGBA, w x h, rows down) into out, nothing registered
void UI_BakeTexels(const char *const *layers, int numLayers, int w, int h, float scale, byte *out);

// pixels a unit of a w x h design: the screen fitted both ways, times the
// scale set (UI_SetDesignScale: true when it changed, the faces and shapes
// then baked again; the menus are to be aligned again)
float UI_DesignScale(float w, float h);
bool  UI_SetDesignScale(float scale);
// changes when a face is to be baked again: the renderer's restart, the scale
int UI_FontStamp(void);

// a map load: the shaders of the baked images are looked up again
void UI_BakeForget(void);
// changes when a baked handle may have: a map load, a renderer's restart
int UI_BakeStamp(void);

// between true and false the faces asked for are baked on other threads,
// their pages registered at false (the UI's first frame after its start, or
// before a shape) in the order asked for
void UI_BakeBatch(bool begin);
// while on (1; 0 off, -1 asks), UI_BakeShape only begins the shapes' texels
// on other threads (it registers nothing and returns 0): the same bake after
// finds them made
bool UI_BakeAhead(int on = -1);
// the shapes made ahead and not baked, and the pictures read: let go
void UI_BakeRelease(void);
// a job on the bake threads (it reads only what it was given); its future
// is ready when it has run (on the main thread only)
std::future<void> UI_BakeRun(std::function<void()> job);

// a picture's texels across and down (read once); false: none
bool UI_PictureSize(const char *file, int *w, int *h);
// an outline's (an SVG's) size in its own units, its viewBox (read once); false: none
bool UI_OutlineSize(const char *file, float *w, float *h);
