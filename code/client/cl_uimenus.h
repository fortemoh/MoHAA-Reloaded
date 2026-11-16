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

// cl_uimenus.h: the Reloaded menus (Added): team select, weapon select, the
// Esc menu, the scoreboard, the load screen, the end of a mission's level
// and the connect's box (ui/reloaded_*.urc), shown in place of the stock
// menus unless ui_classicmenus is 1, and the cursor

#pragma once

void        UI_Menus_Init(void);
// the menu shown for a stock menu's name, NULL: the stock one
const char *UI_Menus_Alias(const char *name);
// a menu about to be pushed by that name (the weapon menu's nation and
// preview); qfalse: not pushed (the server's push after a side picked in the weapon menu)
qboolean    UI_Menus_Pushing(const char *name);
// the cgame shows (show) or hides a stock menu m was found for: the end of a
// mission's level (reloaded_end) pushed or popped instead (its buttons take
// the mouse and the keys); qfalse: not that one
qboolean    UI_Menus_End(const char *name, class Menu *m, qboolean show);
// a server command just tokenized (the scoreboard's "scores")
void        UI_Menus_ServerCommand(void);
// the scoreboard of this (stock) name is the Reloaded one: the stock list isn't shown
qboolean    UI_Menus_Scoreboard(const char *name);
// the menu up is the Reloaded weapon or Esc menu (or Options over it): no HUD under it (the scoreboard keeps it)
qboolean    UI_Menus_Covering(void);
// a configstring changed
void        UI_Menus_ConfigString(void);
// the menus laid out to bake their faces and shapes (a map's load, a new screen size)
void        UI_Menus_PreBake(void);
// a mission's objectives as the scoreboard's card while TAB is held (shown; the cgame's
// call each frame); qfalse with ui_classicmenus 1: the stock list
qboolean    UI_Menus_DrawObjectives(qboolean shown);
// a map's load begins (UI_BeginLoad): the Reloaded menu that shows it
// (reloaded_loading, a mission's level reloaded_mission; the pictures the
// stock load screen's, stock); NULL: the stock one
const char *UI_Menus_Loading(class Menu *stock, const char *map);
// a connect begins (CL_Connect, its address resolved): the connect's box
// asks the server for its name and map, unless the browser's row gave them (cl_uimain.cpp)
void        UI_Menus_Connect(const char *server, const netadr_t *adr);
// a server's info (infoResponse): qtrue, the answer the connect's box asked for
qboolean    UI_Menus_ServerInfo(const netadr_t *from, const char *info);
