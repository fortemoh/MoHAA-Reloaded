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

// cl_uicomp.h: the Competitive HUD's data and widgets (Added, cg_hudstyle 1,
// ui/hud_comp_*.urc)

#pragma once

void     UI_CompHud_Init(void);
qboolean UI_CompHud_Active(void);         // cg_hudstyle 1
void     UI_CompHud_Frame(void);          // the ui_hud_* cvars, before the menus draw
void     UI_CompHud_Print(const char *msg); // a console line: the chat, the game messages, the kills
void     UI_CompHud_Clear(void);          // a new map or server
void     UI_CompHud_MapLoaded(void);      // the radar's walk and the pictures, at the load
void     UI_CompHud_PreBake(void);        // the pictures (after the renderer's restart)
