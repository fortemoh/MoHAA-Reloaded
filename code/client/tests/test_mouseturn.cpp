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

// Unit tests for the mouse's turn and the turn distance kept (cl_mouseturn_logic.h). The degrees a count turns were
// measured in the game (2026-11-21: 1000 counts sent as the mouse's, cl.viewangles read before and after)

#include "../cl_mouseturn_logic.h"

#include <math.h>
#include <stdio.h>

static int g_failures;

static void expect_near(double value, double expected, double tolerance, const char *message)
{
    if (!(fabs(value - expected) <= tolerance)) {
        fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", message, value, expected);
        g_failures++;
    }
}

static void expect_int(int value, int expected, const char *message)
{
    if (value != expected) {
        fprintf(stderr, "FAIL: %s (got %d expected %d)\n", message, value, expected);
        g_failures++;
    }
}

static mouseTurnState_t state(double sens, double fov, double dpi, double turn)
{
    mouseTurnState_t s;

    s.sens        = sens;
    s.yaw         = 0.022;
    s.fov         = fov;
    s.dpi         = dpi;
    s.turn        = turn;
    s.sensChanged = s.fovChanged = s.dpiChanged = s.turnChanged = 0;
    return s;
}

int main(void)
{
    double sens, turn;

    // the cgame's scale (CG_SensitivityScale, measured: 0.85822, 0.98320, 1.39762)
    expect_near(MouseTurn_FovScale(80), 0.85822, 0.00001, "fov_y / 75 at cg_fov 80");
    expect_near(MouseTurn_FovScale(90), 0.98320, 0.00001, "fov_y / 75 at cg_fov 90");
    expect_near(MouseTurn_FovScale(120), 1.39762, 0.00001, "fov_y / 75 at cg_fov 120");
    // the degrees 1000 counts turned in the game
    expect_near(1000 * MouseTurn_Degrees(5, 0.022, 80), 94.4041, 0.0001, "sensitivity 5, cg_fov 80");
    expect_near(1000 * MouseTurn_Degrees(5, 0.022, 90), 108.1517, 0.0001, "sensitivity 5, cg_fov 90");
    expect_near(1000 * MouseTurn_Degrees(1, 0.022, 120), 30.7477, 0.0001, "sensitivity 1, cg_fov 120");
    // the turn distance: 360 / 0.0944041 counts at 800 an inch, 2.54 cm an inch
    expect_near(MouseTurn_Cm(5, 0.022, 80, 800), 12.1075, 0.0001, "12.1 cm a turn at 5, cg_fov 80, 800 DPI");
    expect_near(MouseTurn_Sens(MouseTurn_Cm(3.7, 0.022, 95, 1600), 0.022, 95, 1600), 3.7, 1e-9, "the sensitivity for a distance, back");
    expect_near(MouseTurn_Cm(5, 0.022, 80, 0), 0, 0, "no DPI: no distance");
    expect_near(MouseTurn_Sens(30, 0.022, 80, 0), 0, 0, "no DPI: no sensitivity");

    // not kept (m_turn 0): nothing, whatever changed
    {
        mouseTurnState_t s = state(5, 100, 800, 0);

        s.fovChanged = s.sensChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), 0, "not kept: nothing");
    }
    // 1. the distance set: the sensitivity for it
    {
        mouseTurnState_t s = state(5, 80, 800, 30);

        s.turnChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), MOUSETURN_WRITE_SENS, "m_turn set: sensitivity");
        expect_near(MouseTurn_Cm(sens, 0.022, 80, 800), 30, 1e-9, "m_turn set: the sensitivity turns 30 cm");
    }
    // 2. the sensitivity set while kept: the distance follows
    {
        mouseTurnState_t s = state(2, 80, 800, 30);

        s.sensChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), MOUSETURN_WRITE_TURN, "sensitivity set: m_turn");
        expect_near(turn, MouseTurn_Cm(2, 0.022, 80, 800), 1e-9, "sensitivity set: its distance");
    }
    // 3. the Field of View changed while kept: the hand moves as far for a turn
    {
        mouseTurnState_t s = state(MouseTurn_Sens(30, 0.022, 80, 800), 110, 800, 30);

        s.fovChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), MOUSETURN_WRITE_SENS, "FOV changed: sensitivity");
        expect_near(MouseTurn_Cm(sens, 0.022, 110, 800), 30, 1e-9, "FOV 80 -> 110: still 30 cm");
        expect_near(sens, MouseTurn_Sens(30, 0.022, 80, 800) * MouseTurn_FovScale(80) / MouseTurn_FovScale(110), 1e-9, "FOV 80 -> 110: the sensitivity lower");
    }
    // ... and the DPI
    {
        mouseTurnState_t s = state(4, 80, 1600, 30);

        s.dpiChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), MOUSETURN_WRITE_SENS, "DPI changed: sensitivity");
        expect_near(MouseTurn_Cm(sens, 0.022, 80, 1600), 30, 1e-9, "DPI 800 -> 1600: still 30 cm");
    }
    // the sensitivity and the Field of View at once (Options' DEFAULTS, a config): the sensitivity wins
    {
        mouseTurnState_t s = state(5, 80, 800, 30);

        s.sensChanged = s.fovChanged = 1;
        expect_int(MouseTurn_Decide(&s, &sens, &turn), MOUSETURN_WRITE_TURN, "sensitivity and FOV: m_turn follows");
    }
    // nothing changed: nothing
    {
        mouseTurnState_t s = state(5, 80, 800, 30);

        expect_int(MouseTurn_Decide(&s, &sens, &turn), 0, "nothing changed");
    }

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_mouseturn: all passed\n");
    return 0;
}
