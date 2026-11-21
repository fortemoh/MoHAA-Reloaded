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

// cl_mouseturn_logic.h -- how far the view turns for the mouse, and the
// turn distance (cm/360) kept when the Field of View changes, free of
// engine state so they can be unit-tested (tests/test_mouseturn.cpp).
// cl_mouseturn.cpp applies the rule to the cvars.
//
// A count of the mouse turns the view sensitivity * m_yaw * fov_y / 75
// degrees (cl_input.cpp's CL_MouseLook times the cgame's CG_SensitivityScale,
// cg.zoomSensitivity = fov_y / 75), fov_y the view's vertical field of view:
// cg_fov is the horizontal one of a 4:3 screen, so fov_y is
// 2 atan(3/4 tan(cg_fov / 2)) whatever the screen (cg_view.c, CG_CalcFov).
// Unlike CS's, the turn grows with the Field of View: cg_fov 80 turns 0.858
// of sensitivity * m_yaw a count, 90 0.983, 120 1.398. Zoomed, fov_y is
// the server's zoom's (not the turn distance's business: hip fire only).
//
// The turn distance, m_turn: 0 not kept (the game's way: the Field of View
// changes the turn). Set, it is kept:
//  1. m_turn set: the sensitivity for it;
//  2. else the sensitivity set: m_turn its distance;
//  3. else the Field of View or the mouse's DPI changed: the sensitivity
//     for m_turn again (the hand moves as far for a turn).

#pragma once

#include <math.h>

#define MOUSETURN_CM_INCH 2.54

// fov_y / 75 at a cg_fov: the cgame's scale of the turn (unzoomed)
static inline double MouseTurn_FovScale(double fov)
{
    const double rad = 3.14159265358979323846 / 180.0;

    return 2.0 * atan(0.75 * tan(fov * rad / 2.0)) / rad / 75.0;
}

// degrees a count turns the view
static inline double MouseTurn_Degrees(double sens, double yaw, double fov)
{
    return sens * fabs(yaw) * MouseTurn_FovScale(fov);
}

// centimetres of mouse travel for a full turn at dpi counts an inch (0: none)
static inline double MouseTurn_Cm(double sens, double yaw, double fov, double dpi)
{
    const double degrees = MouseTurn_Degrees(sens, yaw, fov);

    return degrees > 0.0 && dpi > 0.0 ? 360.0 / degrees / dpi * MOUSETURN_CM_INCH : 0.0;
}

// the sensitivity for a full turn in cm centimetres (0: none)
static inline double MouseTurn_Sens(double cm, double yaw, double fov, double dpi)
{
    const double perSens = MouseTurn_Degrees(1.0, yaw, fov);

    return cm > 0.0 && dpi > 0.0 && perSens > 0.0 ? 360.0 * MOUSETURN_CM_INCH / (cm * dpi * perSens) : 0.0;
}

// what the cvars hold now, and which changed since the last sync (not by it)
typedef struct {
    double sens, yaw, fov, dpi;
    double turn; // m_turn: 0 not kept
    int    sensChanged, fovChanged, dpiChanged, turnChanged;
} mouseTurnState_t;

// writes to make (MouseTurn_Decide's result)
#define MOUSETURN_WRITE_SENS 1 // sensitivity: *sens
#define MOUSETURN_WRITE_TURN 2 // m_turn: *turn

static inline int MouseTurn_Decide(const mouseTurnState_t *now, double *sens, double *turn)
{
    if (now->turn <= 0.0 || now->dpi <= 0.0) {
        // not kept
        return 0;
    }
    if (now->turnChanged || ((now->fovChanged || now->dpiChanged) && !now->sensChanged)) {
        *sens = MouseTurn_Sens(now->turn, now->yaw, now->fov, now->dpi);
        return *sens > 0.0 ? MOUSETURN_WRITE_SENS : 0;
    }
    if (now->sensChanged) {
        *turn = MouseTurn_Cm(now->sens, now->yaw, now->fov, now->dpi);
        return *turn > 0.0 ? MOUSETURN_WRITE_TURN : 0;
    }
    return 0;
}
