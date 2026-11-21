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

// cl_mouseturn.cpp -- the turn distance: m_dpi, the mouse's counts an inch
// (its software shows it), and m_turn, the centimetres of mouse travel for a
// full turn, kept when the Field of View or the DPI changes (0: not kept,
// the game's way); cl_mouseturn_logic.h has the rule. Options' Turn Distance
// sets m_turn; its Sensitivity sets m_turn too while it is kept.

#include "client.h"
#include "cl_mouseturn.h"
#include "cl_mouseturn_logic.h"

static cvar_t *mt_sens, *mt_yaw, *mt_fov, *mt_dpi, *mt_turn;
// the modification counts the last sync left (its own writes in them)
static int     mt_seen[5] = {-1, -1, -1, -1, -1};

void CL_MouseTurn_Init(void)
{
    mt_sens = Cvar_Get("sensitivity", "5", CVAR_ARCHIVE);
    mt_yaw  = Cvar_Get("m_yaw", "0.022", CVAR_ARCHIVE);
    // (the cgame's, made here too as it makes it: Options and the rule read it from the main menu on)
    mt_fov  = Cvar_Get("cg_fov", "80", CVAR_ARCHIVE);
    mt_dpi  = Cvar_Get("m_dpi", "800", CVAR_ARCHIVE);
    mt_turn = Cvar_Get("m_turn", "0", CVAR_ARCHIVE);
}

float CL_MouseTurn_Distance(void)
{
    if (!mt_sens) {
        return 0.0f;
    }
    return (float)MouseTurn_Cm(mt_sens->value, mt_yaw->value, mt_fov->value, mt_dpi->value);
}

void CL_MouseTurn_Sync(void)
{
    cvar_t *const   vars[5] = {mt_sens, mt_fov, mt_dpi, mt_turn, mt_yaw};
    mouseTurnState_t now;
    double           sens = 0.0, turn = 0.0;
    int              writes;

    if (!mt_sens) {
        return;
    }
    if (mt_seen[0] < 0) {
        // the first: only the baseline
        for (int i = 0; i < 5; i++) {
            mt_seen[i] = vars[i]->modificationCount;
        }
        return;
    }
    now.sens        = mt_sens->value;
    now.yaw         = mt_yaw->value;
    now.fov         = mt_fov->value;
    now.dpi         = mt_dpi->value;
    now.turn        = mt_turn->value;
    now.sensChanged = mt_sens->modificationCount != mt_seen[0];
    now.fovChanged  = mt_fov->modificationCount != mt_seen[1] || mt_yaw->modificationCount != mt_seen[4];
    now.dpiChanged  = mt_dpi->modificationCount != mt_seen[2];
    now.turnChanged = mt_turn->modificationCount != mt_seen[3];

    writes = MouseTurn_Decide(&now, &sens, &turn);
    if (writes & MOUSETURN_WRITE_SENS) {
        Cvar_Set("sensitivity", va("%g", sens));
    }
    if (writes & MOUSETURN_WRITE_TURN) {
        Cvar_Set("m_turn", va("%g", turn));
    }
    for (int i = 0; i < 5; i++) {
        mt_seen[i] = vars[i]->modificationCount;
    }
}
