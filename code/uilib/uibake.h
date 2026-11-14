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

// uibake.h: what the UI bakes into pictures (Added): TrueType faces, shapes
// and PNG pictures, rasterized once at the size they are drawn

#pragma once

// a face: "<file>-<px>[s][i]", fonts/<file>.ttf at px pixels of a 1080-line
// screen (UI_DesignScale), s a drop shadow, i slanted; NULL when not a face
fontheader_t *UI_BakeFont(const char *name);

// a shape's layers (the URC's "shape" lines) baked at w x h pixels, scale
// pixels a design unit; key names it among the others
qhandle_t UI_BakeShape(const char *key, const char *const *layers, int numLayers, int w, int h, float scale);

// pixels a unit of a w x h design: the screen fitted both ways
float UI_DesignScale(float w, float h);

// a map load: the shaders of the baked images are looked up again
void UI_BakeForget(void);
// changes when a baked handle may have: a map load, a renderer's restart
int UI_BakeStamp(void);
