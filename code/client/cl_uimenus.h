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
// Esc menu, the scoreboard and the load screen (ui/reloaded_*.urc), shown in
// place of the stock menus unless ui_classicmenus is 1

#pragma once

void        UI_Menus_Init(void);
// the menu shown for a stock menu's name, NULL: the stock one
const char *UI_Menus_Alias(const char *name);
// a menu about to be pushed by that name (the weapon menu's nation and
// preview); qfalse: not pushed (the server's push after a side picked in the weapon menu)
qboolean    UI_Menus_Pushing(const char *name);
// a server command just tokenized (the scoreboard's "scores")
void        UI_Menus_ServerCommand(void);
// the scoreboard of this (stock) name is the Reloaded one: the stock list isn't shown
qboolean    UI_Menus_Scoreboard(const char *name);
// the menu up is the Reloaded weapon or Esc menu: no HUD under it (the scoreboard keeps it)
qboolean    UI_Menus_Covering(void);
// a configstring changed
void        UI_Menus_ConfigString(void);
// the menus laid out to bake their faces and shapes (a map's load, a new screen size)
void        UI_Menus_PreBake(void);
// a map's load begins (UI_BeginLoad): qtrue, reloaded_loading shows it (the
// picture the stock load screen's, stock); qfalse: the stock one
qboolean    UI_Menus_Loading(class Menu *stock, const char *map);
