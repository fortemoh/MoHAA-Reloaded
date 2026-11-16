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

// cl_uiwidget.h: the Reloaded menus' widget (Added), ReloadedMenu, defined
// in cl_uimenus.cpp, its main menu's screens in cl_uimain.cpp, and what
// those share: the player models, the blurred photos, the maps' names

#pragma once

#include "cl_uidraw.h"

#include <string>
#include <vector>

/*
ReloadedMenu: "screen weapon|esc|scoreboard|loading|main|confirm", "layer
back|front". The back layer draws what is under the 3D model views and
places them (labels the menu names); the front layer draws what is over
them and takes the mouse and the keys. Both lay the screen out alike from
the same data
*/

enum { SCR_WEAPON, SCR_ESC, SCR_SCOREBOARD, SCR_LOADING, SCR_MAIN, SCR_CONFIRM, SCR_NUM };

// items from this id on are hot without a sound (a list's rows)
#define RL_QUIET 1000

typedef struct {
    int x, y, w, h; // pixels
    int id;
} rlHit_t;

struct rlInfo_t;

class UIReloadedMenu : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIReloadedMenu);

    int                  m_screen, m_back;
    int                  m_focus;    // the item hot (hovered or moved to), -1 none
    int                  m_mouse[2]; // where the mouse was
    std::vector<rlHit_t> m_hits;
    std::vector<int>     m_order;    // the items in order (the arrow keys)
    int                  m_cols;     // the items a row (left and right)

    int                  m_tick;     // the frame Tick ran (-2: none, nor the frame before the first)

    UIReloadedMenu() : m_screen(SCR_WEAPON), m_back(0), m_focus(-1), m_cols(1), m_tick(-2)
    {
        m_mouse[0] = m_mouse[1] = -1;
        AllowActivate(true);
    }

    void SetScreen(Event *ev);
    void SetLayer(Event *ev) { m_back = !str::icmp(ev->GetString(1), "back"); }
    void MouseUp(Event *ev);
    void Draw(void) override;
    qboolean KeyEvent(int key, unsigned int time) override;
    void CharEvent(int ch) override;
    // the draw cache: drawn again from its list while DrawKey stays (the data's
    // changes, the hot item, the clock's seconds) and the cvars it read
    bool CacheDraw(void) override { return true; }
    bool DrawKeyed(void) override { return true; }
    int  DrawKey(void) override;
    void Tick(void);

    void Hit(const rlView_t& v, float x, float y, float w, float h, int id);
    int  HitAt(int x, int y);
    bool Hot(int id) const { return m_focus == id; }
    void Choose(int id);
    void PlaceFigure(const rlView_t& v, float cx, float headY, float width);
    // a view (a label of the menu's) placed over x y w h, in units (none across: hidden)
    void PlaceView(const rlView_t& v, const char *name, float x, float y, float w, float h);

    void DrawWeapon(const rlView_t& v, const rlInfo_t& in);
    void DrawEsc(const rlView_t& v, const rlInfo_t& in);
    void DrawScoreboard(const rlView_t& v, const rlInfo_t& in);

    // the main menu and its confirmations (cl_uimain.cpp)
    void     DrawMain(const rlView_t& v);
    void     DrawConfirm(const rlView_t& v);
    void     ChooseMain(int id);
    qboolean MainKey(int key);
    void     MainChar(int ch);
    void     MainTick(bool opened);
    int      MainDrawKey(void);
};

// the main menu shown next on a page (the Esc menu's OPTIONS, LOAD, MAIN MENU in a mission;
// cl_uimain.cpp): false, not there (CAMPAIGN is Allied Assault's)
enum { RL_SHOW_OPTIONS, RL_SHOW_MISSIONS, RL_SHOW_SAVES };
bool        RL_MainShow(int what);
// the Esc menu's caption in a mission ("MISSION 2  ·  SCUTTLING THE U-529  ·  MEDIUM")
std::string RL_MissionLine(const char *map);

// the player models a side can pick, the stock picker's, in its order
typedef struct {
    std::string file;  // models/player/<file>.tik
    std::string order; // the picker's name ("DE - Wehrmacht Soldier")
    std::string name;  // shown ("Wehrmacht Soldier")
} rlModel_t;

extern std::vector<rlModel_t> rl_models[2];

// the models listed again (a menu showing them opens)
void        RL_ListModels(void);
// a side's own model (dm_playermodel, dm_playergermanmodel): its file; its place in the list, -1 not listed
std::string RL_OwnModel(int side);
int         RL_OwnIndex(int side);
// a model's name shown, from its file
std::string RL_ModelName(const char *file);
// the figure of a model's file: a TIKI of it standing in an idle (the views' linkcvar)
std::string RL_Figure(const std::string& file);
// a map's display name ("dm/mohdm6": "Stalingrad"); its own buffer
const char *RL_MapName(const char *map);
// a stock load screen's photo (the menu named after a dm or obj map), RGBA,
// opaque, NULL none; freed with uii.Rend_FreeRawImage
byte       *RL_MenuPhoto(class Menu *stock, int *width, int *height);
// a photo (RGBA) as a screen-shaped texture rows texels high, blurred, its
// light times dim, darker to the corners, registered as name
qhandle_t   RL_Blur(const char *name, const byte *pic, int pw, int ph, int rows, float dim);
