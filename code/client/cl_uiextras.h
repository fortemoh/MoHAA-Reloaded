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

// cl_uiextras.h: the Competitive HUD's extras (Added, cg_hudstyle 1): the
// zoom overlays

#pragma once

// the zoom overlay drawn (the cgame's: 0 a scope, 1 the Kar98's, 2 the spy
// camera's, 3 the binoculars'), faded by alpha; qfalse: the stock one is drawn
qboolean UI_DrawZoomOverlay(int type, float alpha);
// what is baked (a map's load, a new screen size); ahead: the overlays' masks begun on other threads
void UI_Extras_PreBake(qboolean ahead);
