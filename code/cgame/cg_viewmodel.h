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

// cg_viewmodel.h: the first-person weapon's position presets and its settings' limits (Added), read by
// CG_ViewModelSettings and by the main menu's options (cl_uimain.cpp), which sets them before any map

#pragma once

typedef struct {
    float x, y, z;
} vmPreset_t;

// cg_viewmodel_presetpos: the offsets x, y, z each sets
static const vmPreset_t vmPresets[] = {
    {0,    0,     0    }, // 0 custom: the offsets as they are
    {1.0f, 1.0f,  -1.0f}, // 1 Desktop: out of the way, right, forward and down
    {0,    -1.5f, 0.5f }, // 2 Couch: closer, so bigger
    {0,    0,     0    }, // 3 Classic: the game's own position
};

#define VM_SETTINGS 5 // offsets x, y, z, then yaw, then the drop while moving
#define VM_YAW      3
#define VM_MOVEDROP 4

static const float vmLimits[VM_SETTINGS][2] = {
    {-2.5f,  2.5f }, // x: left, right
    {-2.0f,  2.0f }, // y: back, forward
    {-2.0f,  1.0f }, // z: down, up (higher, the weapons' ends come into view)
    {-15.0f, 15.0f}, // yaw: the muzzle out, to the middle
    {0.0f,   1.0f }, // the drop while moving: none, the game's own
};
