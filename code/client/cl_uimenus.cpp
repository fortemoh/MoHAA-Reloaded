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

// cl_uimenus.cpp: the Reloaded menus (Added). The weapon select (which picks
// the side and the model too: the team menus show it), the Esc menu and
// the scoreboard, each a URC
// menu (ui/reloaded_*.urc) of a ReloadedMenu widget, which lays the screen
// out and draws it, and the stock 3D player model views it places. They are
// shown for the stock menus' names (SelectTeam, ObjSelectTeam,
// SelectFFAModel, SelectPrimaryWeapon[_<nation>], dm_main, DM_Scoreboard,
// DM_Round_Scoreboard, Obj_Scoreboard: UI_Menus_Alias), so nothing on the
// server or in the cgame changes; ui_classicmenus 1 shows the stock ones.
//
// Sizes are design units: a 1080-line screen (900 below 900 lines, so small
// screens get bigger type), the width what the screen's shape gives. Type is
// Oswald baked at the pixel size drawn, outlines (moh-icons, ui/outlines/)
// filled at the size drawn.

#include "client.h"
#include "cl_ui.h"
#include "cl_uimenus.h"
#include "cl_uicomp.h"
#include "cl_uidraw.h"
#include "cl_uiextras.h"
#include "../uilib/uibake.h"
#include "../qcommon/localization.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

// the weapon menu's figure (ui/reloaded_weapon.urc's label "figure"): its
// modelscale; the height in the middle of its view (48, the middle of the 96
// units the model is fitted to, less modeloffset's height); his head's top
// (a helmet's)
#define RL_FIGSCALE 0.82f
#define RL_FIGMID   63.0f
#define RL_FIGHEAD  98.0f

static cvar_t *ui_classicmenus;
static cvar_t *ui_reloaded_figure; // the figure's model (the label's linkcvar)
static cvar_t *ui_dm_primary;
static cvar_t *ui_reloaded_nation;

/*
====================================================================

DATA

What the menus show, read when drawn: the serverinfo, the player
configstrings (every connected player's name and team) and the latest
"scores" the server sent (UI_Menus_ServerCommand)

====================================================================
*/

enum { NAT_US, NAT_UK, NAT_RU, NAT_DE, NAT_IT };

typedef struct {
    int      kills, deaths;
    char     time[16], ping[16];
    qboolean dead;
    int      order; // its place in the server's list
} rlScore_t;

// the configstrings' changes (UI_Menus_ConfigString)
static int rl_csChanges;

void UI_Menus_ConfigString(void)
{
    rl_csChanges++;
}

static struct {
    int       stamp; // a parse's number, 0 none yet
    rlScore_t client[MAX_CLIENTS];
    int       seen[MAX_CLIENTS]; // the stamp a client's row came with
    int       teamScore[2];      // the headers', -1 none
    int       requested;         // when a score was last asked for
} rl_sb;

typedef struct {
    int  client;
    char name[MAX_NAME_LENGTH];
    int  team;
} rlPlayer_t;

typedef struct {
    int                     gametype;
    int                     target;   // TG_MOH, TG_MOHTA, TG_MOHTT
    int                     dmflags, fraglimit, maxClients;
    char                    map[MAX_QPATH];
    std::vector<rlPlayer_t> allies, axis, spectators; // in client order
    int                     connected;
    int                     self, selfTeam;
    int                     teamScore[2]; // -1 unknown
    bool                    serverScores; // the teams' scores from the server's configstring
} rlInfo_t;

static int RL_Gametype(void)
{
    return atoi(Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "g_gametype"));
}

static void RL_Info(rlInfo_t *in)
{
    const char *info = CL_ConfigString(CS_SERVERINFO);
    char       *dollar;

    in->gametype   = atoi(Info_ValueForKey(info, "g_gametype"));
    in->target     = com_target_game ? com_target_game->integer : TG_MOH;
    in->dmflags    = atoi(Info_ValueForKey(info, "dmflags"));
    in->fraglimit  = atoi(Info_ValueForKey(info, "fraglimit"));
    in->maxClients = atoi(Info_ValueForKey(info, "sv_maxclients"));
    Q_strncpyz(in->map, Info_ValueForKey(info, "mapname"), sizeof(in->map));
    dollar = strchr(in->map, '$');
    if (dollar) {
        *dollar = 0;
    }
    in->allies.clear();
    in->axis.clear();
    in->spectators.clear();
    in->connected = 0;
    in->self      = cl.snap.valid ? cl.snap.ps.clientNum : clc.clientNum;
    in->selfTeam  = cl.snap.valid ? cl.snap.ps.stats[STAT_TEAM] : TEAM_NONE;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const char *cs = CL_ConfigString(CS_PLAYERS + i);
        rlPlayer_t  p;

        if (!cs[0]) {
            continue;
        }
        Q_strncpyz(p.name, Info_ValueForKey(cs, "name"), sizeof(p.name));
        if (!p.name[0]) {
            continue;
        }
        p.client = i;
        p.team   = atoi(Info_ValueForKey(cs, "team"));
        in->connected++;
        if (p.team == TEAM_ALLIES) {
            in->allies.push_back(p);
        } else if (p.team == TEAM_AXIS) {
            in->axis.push_back(p);
        } else if (p.team == TEAM_SPECTATOR) {
            in->spectators.push_back(p);
        }
    }
    // the teams' scores: the server's (CS_TEAMSCORES), else the scoreboard's
    in->serverScores = sscanf(CL_ConfigString(CS_TEAMSCORES), "%d %d", &in->teamScore[0], &in->teamScore[1]) == 2;
    if (!in->serverScores) {
        in->teamScore[0] = rl_sb.teamScore[0];
        in->teamScore[1] = rl_sb.teamScore[1];
    }
}

// the scoreboard asked for now and every 2 s while a menu shows it (the
// HUD's own requests count)
static void RL_WantScores(void)
{
    // (not in a demo: nothing acknowledges the commands there, and 64 unacknowledged drop the client)
    if (clc.state == CA_ACTIVE && !clc.demoplaying && (!rl_sb.requested || (unsigned)(cls.realtime - rl_sb.requested) >= 2000)) {
        rl_sb.requested = cls.realtime;
        CL_AddReliableCommand("score", qfalse);
    }
}

/*
================
UI_Menus_ServerCommand

"scores" (fgame's DM_Manager::Score, read as the cgame's CG_ParseScores
reads it): its entries, the team games' a header per team (-1, the team,
[SH/BT: the players], the score, ...) and a row per player (client, team
negative when dead, kills, deaths, time, ping); free-for-all's rows
(client, kills, deaths, time, ping)
================
*/
void UI_Menus_ServerCommand(void)
{
    if (strcmp(Cmd_Argv(0), "scores") || !ui_classicmenus || ui_classicmenus->integer) {
        return;
    }
    const int  gametype = RL_Gametype();
    const bool v15      = Cvar_VariableIntegerValue("com_protocol") >= PROTOCOL_MOHTA_MIN;
    int        cur      = 2;
    // (Q_min is a macro: the count read once)
    const int  count    = atoi(Cmd_Argv(1));
    const int  n        = Q_min(count, MAX_CLIENTS + 8);
    int        order    = 0;

    rl_sb.stamp++;
    rl_sb.teamScore[0] = rl_sb.teamScore[1] = -1;
    if (gametype > GT_FFA) {
        if (gametype == GT_TOW && v15) {
            cur += 10; // the objectives' states
        }
        if (gametype == GT_LIBERATION && v15) {
            cur += 2;
        }
        for (int i = 0; i < n; i++) {
            const int client = atoi(Cmd_Argv(cur + 6 * i));
            int       team   = atoi(Cmd_Argv(cur + 6 * i + 1));
            const int dead   = team < 0;

            team = abs(team);
            if (client == -1) {
                if (team == TEAM_ALLIES || team == TEAM_AXIS) {
                    if (v15) {
                        cur++; // the team's players
                    }
                    rl_sb.teamScore[team == TEAM_AXIS] = atoi(Cmd_Argv(cur + 6 * i + 2));
                }
                continue;
            }
            if (client < 0 || client >= MAX_CLIENTS) {
                continue;
            }
            rlScore_t *r = &rl_sb.client[client];

            r->kills  = atoi(Cmd_Argv(cur + 6 * i + 2));
            r->deaths = atoi(Cmd_Argv(cur + 6 * i + 3));
            Q_strncpyz(r->time, Cmd_Argv(cur + 6 * i + 4), sizeof(r->time));
            Q_strncpyz(r->ping, Cmd_Argv(cur + 6 * i + 5), sizeof(r->ping));
            r->dead  = dead;
            r->order = order++;
            rl_sb.seen[client] = rl_sb.stamp;
        }
    } else {
        for (int i = 0; i < n; i++) {
            const int client = atoi(Cmd_Argv(cur + 5 * i));

            if (client < 0 || client >= MAX_CLIENTS) {
                continue;
            }
            rlScore_t *r = &rl_sb.client[client];

            r->kills  = atoi(Cmd_Argv(cur + 5 * i + 1));
            r->deaths = atoi(Cmd_Argv(cur + 5 * i + 2));
            Q_strncpyz(r->time, Cmd_Argv(cur + 5 * i + 3), sizeof(r->time));
            Q_strncpyz(r->ping, Cmd_Argv(cur + 5 * i + 4), sizeof(r->ping));
            r->dead  = qfalse;
            r->order = order++;
            rl_sb.seen[client] = rl_sb.stamp;
        }
    }
}

static const rlScore_t *RL_Score(int client)
{
    return rl_sb.stamp && rl_sb.seen[client] == rl_sb.stamp ? &rl_sb.client[client] : NULL;
}

// the map's display name: its loading screen's (a menu named after the
// map, its first titled label: "V2 Rocket Facility"), else the file's name
static const char *RL_MapName(const char *map)
{
    static char last[MAX_QPATH], name[128];
    const char *slash;

    if (!strcmp(last, map)) {
        return name;
    }
    Q_strncpyz(last, map, sizeof(last));
    slash = strrchr(map, '/');
    Q_strncpyz(name, slash ? slash + 1 : map, sizeof(name));
    Menu *loading = menuManager.FindMenu(map);
    UIWidget *c = loading ? loading->GetContainerWidget() : NULL;

    for (UIWidget *w = c ? c->getFirstChild() : NULL; w; w = c->getNextChild(w)) {
        if (w->isSubclassOf(UILabel) && w->getTitle() && w->getTitle()[0]) {
            Q_strncpyz(name, Sys_LV_CL_ConvertString(w->getTitle()), sizeof(name));
            break;
        }
    }
    return name;
}

static const char *RL_ModeName(int gametype, bool upper)
{
    static const char *const names[][2] = {
        {"Single player",   "SINGLE PLAYER"  },
        {"Free-for-all",    "FREE-FOR-ALL"   },
        {"Team deathmatch", "TEAM DEATHMATCH"},
        {"Round-based",     "ROUND-BASED"    },
        {"Objective",       "OBJECTIVE"      },
        {"Tug of war",      "TUG OF WAR"     },
        {"Liberation",      "LIBERATION"     },
    };

    return names[gametype >= 0 && gametype < (int)ARRAY_LEN(names) ? gametype : 0][upper];
}

// the round's or match's time left, "" none (as the HUD's timer); its own buffer
static const char *RL_TimeLeft(int gametype)
{
    static char text[32];

    const int matchEnd = atoi(CL_ConfigString(CS_MATCHEND));
    const int left     = atoi(CL_ConfigString(CS_WARMUP)) != -1 && matchEnd && gametype != GT_LIBERATION
                        && matchEnd > cl.serverTime
                           ? (matchEnd - cl.serverTime) / 1000
                           : -1;

    text[0] = 0;
    if (left >= 0) {
        Com_sprintf(text, sizeof(text), "%d:%02d", left / 60, left % 60);
    }
    return text;
}

/*
====================================================================

DRAWING

====================================================================
*/

rlColor_t RL_Rgb(int r, int g, int b, int a)
{
    rlColor_t c = {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    return c;
}

rlColor_t RL_Alpha(rlColor_t c, int a)
{
    c.a = a / 255.0f;
    return c;
}

// top drawn over bottom, as one color (two flat layers blended make one)
static rlColor_t RL_Over(rlColor_t top, rlColor_t bottom)
{
    rlColor_t c;

    c.a = 1.0f - (1.0f - top.a) * (1.0f - bottom.a);
    c.r = c.a > 0.0f ? (top.r * top.a + bottom.r * bottom.a * (1.0f - top.a)) / c.a : 0.0f;
    c.g = c.a > 0.0f ? (top.g * top.a + bottom.g * bottom.a * (1.0f - top.a)) / c.a : 0.0f;
    c.b = c.a > 0.0f ? (top.b * top.a + bottom.b * bottom.a * (1.0f - top.a)) / c.a : 0.0f;
    return c;
}

rlColor_t RL_T(const int *c, int a)
{
    return RL_Rgb(c[0], c[1], c[2], a);
}

rlView_t RL_View(void)
{
    rlView_t v;

    v.dh     = uid.vidHeight >= 900 ? 1080.0f : 900.0f;
    v.s      = uid.vidHeight / v.dh;
    v.dw     = uid.vidWidth / v.s;
    v.layout = v.dw < 1500.0f ? 0 : v.dw > 2200.0f ? 2 : 1;
    return v;
}

// laid out to bake what it draws ahead (UI_Menus_PreBake): nothing drawn
bool rl_bakeOnly;

int RL_P(const rlView_t& v, float d)
{
    return (int)floorf(d * v.s + 0.5f);
}

void RL_SetColor(rlColor_t c)
{
    const vec4_t col = {c.r, c.g, c.b, c.a};

    uii.Rend_SetColor(col);
}

void RL_Box(const rlView_t& v, float x, float y, float w, float h, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    if (x1 <= x0 || y1 <= y0 || c.a <= 0.0f || rl_bakeOnly) {
        return;
    }
    RL_SetColor(c);
    uii.Rend_DrawBox(x0, y0, x1 - x0, y1 - y0);
}

// a box in pixels (x1, y1 past its edge)
void RL_BoxPx(int x0, int y0, int x1, int y1, rlColor_t c)
{
    if (x1 <= x0 || y1 <= y0 || c.a <= 0.0f || rl_bakeOnly) {
        return;
    }
    RL_SetColor(c);
    uii.Rend_DrawBox(x0, y0, x1 - x0, y1 - y0);
}

// a baked shape (white, tinted) over pixels
void RL_Shape(int x, int y, int w, int h, const char *text, rlColor_t c, float s)
{
    // (copied: the strings va gives are reused while it bakes, a font page's name)
    const std::string layer(text);
    const char       *layers[1] = {layer.c_str()};
    qhandle_t         shader;

    if (w <= 0 || h <= 0 || c.a <= 0.0f) {
        return;
    }
    shader = UI_BakeShape("rlmenu", layers, 1, w, h, s);
    if (!shader || rl_bakeOnly) {
        return;
    }
    RL_SetColor(c);
    uii.Rend_DrawPicStretched(x, y, w, h, 0, 0, 1, 1, shader);
}

static void RL_Round(const rlView_t& v, float x, float y, float w, float h, float r, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    if (r * v.s < 1.0f) {
        RL_Box(v, x, y, w, h, c);
        return;
    }
    RL_Shape(x0, y0, x1 - x0, y1 - y0, va("roundbox %g 1 1 1 1", r), c, v.s);
}

static void RL_RoundFrame(const rlView_t& v, float x, float y, float w, float h, float r, float width, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    RL_Shape(x0, y0, x1 - x0, y1 - y0, va("roundframe %g %g 1 1 1 1", r, width), c, v.s);
}

// a horizontal gradient as strips (a and b at the left and right edges);
// under: the alpha of a black layer under it, drawn with it as one layer (two
// flat layers blended make one: the screen filled once, not twice)
static void RL_HGradient(const rlView_t& v, float x, float y, float w, float h, rlColor_t a, rlColor_t b, float under = 0.0f)
{
    const int x0 = RL_P(v, x), x1 = RL_P(v, x + w), y0 = RL_P(v, y), y1 = RL_P(v, y + h);
    const int n  = Q_max(1, Q_min(64, (x1 - x0) / 8));

    for (int i = 0; i < n; i++) {
        const int   sx = x0 + (x1 - x0) * i / n, ex = x0 + (x1 - x0) * (i + 1) / n;
        const float f  = (i + 0.5f) / n;
        rlColor_t   c;

        // (between premultiplied colors)
        c.a = a.a + (b.a - a.a) * f;
        c.r = c.a > 0 ? (a.r * a.a + (b.r * b.a - a.r * a.a) * f) / c.a : 0;
        c.g = c.a > 0 ? (a.g * a.a + (b.g * b.a - a.g * a.a) * f) / c.a : 0;
        c.b = c.a > 0 ? (a.b * a.a + (b.b * b.a - a.b * a.a) * f) / c.a : 0;
        if (under > 0.0f) {
            // (black under, the strip over it: one color, premultiplied the same)
            const float A = 1.0f - (1.0f - under) * (1.0f - c.a);

            c.r = A > 0.0f ? c.r * c.a / A : 0.0f;
            c.g = A > 0.0f ? c.g * c.a / A : 0.0f;
            c.b = A > 0.0f ? c.b * c.a / A : 0.0f;
            c.a = A;
        }
        if (c.a <= 0.002f || rl_bakeOnly) {
            continue;
        }
        RL_SetColor(c);
        uii.Rend_DrawBox(sx, y0, ex - sx, y1 - y0);
    }
}

// an outline fitted in a box (its middle at cx cy), tinted; false: no such file
bool RL_Outline(const rlView_t& v, const char *name, float cx, float cy, float bw, float bh, rlColor_t c, bool mirror)
{
    const std::string file(name);
    float             sw, sh, k;
    int               pw, ph;

    if (!UI_OutlineSize(file.c_str(), &sw, &sh)) {
        return false;
    }
    k  = Q_min(bw / sw, bh / sh);
    pw = Q_max(1, (int)floorf(sw * k * v.s + 0.5f));
    ph = Q_max(1, (int)floorf(sh * k * v.s + 0.5f));
    RL_Shape((int)floorf(cx * v.s - pw * 0.5f + 0.5f), (int)floorf(cy * v.s - ph * 0.5f + 0.5f), pw, ph,
             va("svg 1 1 1 1 %s center%s", file.c_str(), mirror ? " mirror" : ""), c, v.s);
    return true;
}

void RL_TeamIcon(const rlView_t& v, int t, float cx, float cy, float d, float alpha)
{
    RL_Outline(v, rl_teams[t].icon, cx, cy, d, d, {1, 1, 1, alpha});
}

/*
----------------
Text: Oswald Medium (m) and SemiBold (b), baked at the pixel size drawn
("oswald-medium-<px>p"); a size is design pixels (the em), 9 pixels at
least on the screen
----------------
*/

// the type scale (design pixels): three header sizes, two for what's under them
#define RL_H1 60.0f // the scores
#define RL_H2 36.0f // a screen's title
#define RL_H3 28.0f // the map's name, the time left
#define RL_S1 20.0f // an item's name: weapons, tiles, buttons, teams in the lists, players
#define RL_S2 14.0f // captions: modes, counts, classes, columns

rlFace_t RL_Face(const rlView_t& v, float size, bool bold)
{
    rlFace_t face;

    char name[64];

    face.px = Q_max(9, (int)floorf(size * v.s + 0.5f));
    Com_sprintf(name, sizeof(name), "oswald-%s-%dp", bold ? "semibold" : "medium", face.px);
    face.f    = UI_BakeFont(name);
    face.page = face.f ? UI_BakedFontScale(face.f) : 0.0f;
    return face;
}

float RL_Advance(const rlFace_t& face, unsigned char ch)
{
    const fontheader_sgl_t *g = face.f->sgl[0];
    int                     i = g->indirection[ch];

    if (i < 0) {
        i = g->indirection['?'];
    }
    return i < 0 ? 0.0f : g->locations[i].size[0] * 256.0f * face.page;
}

// pixels across
float RL_WidthPx(const rlFace_t& face, const char *text, float trackPx)
{
    float w = 0.0f;
    int   n = 0;

    if (!face.f || face.page <= 0.0f) {
        return 0.0f;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++, n++) {
        w += RL_Advance(face, *p);
    }
    return w + (n > 1 ? trackPx * (n - 1) : 0.0f);
}

float RL_TextW(const rlView_t& v, const char *str, float size, bool bold, float track)
{
    const std::string text(str); // (a string from va: kept while faces are baked)

    return RL_WidthPx(RL_Face(v, size, bold), text.c_str(), track * v.s) / v.s;
}

// the text drawn; its width in units. maxw: drawn smaller to fit
float RL_Text(
    const rlView_t& v,
    float           x,
    float           y,
    const char     *str,
    float           size,
    rlColor_t       c,
    bool            bold,
    int             anchor,
    float           track,
    int             valign,
    float           maxw
)
{
    const std::string copy(str); // (a string from va: kept while faces are baked)
    const char       *text = copy.c_str();
    rlFace_t          face = RL_Face(v, size, bold);
    float             w, lineH, capH, base, px, py;

    if (!face.f || face.page <= 0.0f) {
        return 0.0f;
    }
    while (maxw > 0.0f && size > 8.0f && RL_WidthPx(face, text, track * v.s) > maxw * v.s) {
        size -= 1.0f;
        face = RL_Face(v, size, bold);
        if (face.px <= 9) {
            break;
        }
    }
    w     = RL_WidthPx(face, text, track * v.s);
    lineH = face.f->sgl[0]->height * face.page;
    capH  = face.px * 0.81f; // Oswald's capitals
    base  = floorf(lineH * 0.5f + capH * 0.5f + 0.5f);
    px    = x * v.s - (anchor == RL_CENTER ? w * 0.5f : anchor == RL_RIGHT ? w : 0.0f);
    py    = y * v.s - (valign == RL_TOP ? base - capH : valign == RL_MID ? base - capH * 0.5f : base);
    px    = floorf(px + 0.5f);
    py    = floorf(py + 0.5f);

    if (!rl_bakeOnly) {
        const float vs[2] = {face.page, face.page};

        RL_SetColor(c);
        if (track <= 0.0f) {
            uii.Rend_DrawString(face.f, text, px / face.page, py / face.page, -1, vs);
        } else {
            char one[2] = {0, 0};

            for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
                one[0] = *p;
                uii.Rend_DrawString(face.f, one, floorf(px + 0.5f) / face.page, py / face.page, -1, vs);
                px += RL_Advance(face, *p) + track * v.s;
            }
        }
    }
    return w / v.s;
}

// a name cut with "…" to fit
static std::string RL_Ellipsize(const rlView_t& v, const char *text, float size, bool bold, float maxw)
{
    std::string s(text);

    if (RL_TextW(v, text, size, bold) <= maxw) {
        return s;
    }
    while (!s.empty() && RL_TextW(v, (s + "\x85").c_str(), size, bold) > maxw) {
        s.erase(s.size() - 1);
    }
    return s + "\x85";
}

std::string RL_Upper(const char *s)
{
    std::string u(s);

    for (char& c : u) {
        c = toupper((unsigned char)c);
    }
    return u;
}

/*
====================================================================

WEAPONS

The classes primarydmweapon takes, and what each nation gets for them
(fgame's Player::EquipWeapons)

====================================================================
*/

enum { WC_RIFLE, WC_SNIPER, WC_SMG, WC_MG, WC_HEAVY, WC_SHOTGUN, WC_LANDMINE, WC_NUM };

static const char *const rl_classCmd[WC_NUM]  = {"rifle", "sniper", "smg", "mg", "heavy", "shotgun", "landmine"};
static const char *const rl_classCvar[WC_NUM] = {"cg_weapon_rifle",  "cg_weapon_sniper",  "cg_weapon_smg",     "cg_weapon_mg",
                                                 "cg_weapon_rocket", "cg_weapon_shotgun", "cg_weapon_landmine"};

typedef struct {
    const char *cls, *name, *svg;
} rlWeapon_t;

static rlWeapon_t RL_W(const char *cls, const char *name, const char *svg)
{
    rlWeapon_t w = {cls, name, svg};
    return w;
}

// what a nation gets for a class; axisModel: the Axis model it is for (the
// German sniper's), NULL the player's (dm_playergermanmodel)
static rlWeapon_t RL_WeaponFor(int nation, int wc, int target, int dmflags, const char *axisModel = NULL)
{
    const bool bt = target >= TG_MOHTT, aa = target < TG_MOHTA;

    switch (wc) {
    case WC_RIFLE:
        switch (nation) {
        case NAT_UK:
            return RL_W("RIFLE", "LEE-ENFIELD", "enfield");
        case NAT_RU:
            return RL_W("RIFLE", "MOSIN-NAGANT", "mosin_nagant_rifle");
        case NAT_DE:
            return RL_W("RIFLE", "KAR98K", "kar98");
        case NAT_IT:
            return RL_W("RIFLE", "CARCANO", "it_w_carcano");
        default:
            return RL_W("RIFLE", "M1 GARAND", "m1_garand");
        }
    case WC_SNIPER:
        if (nation == NAT_UK && bt) {
            return RL_W("SNIPER", "L42A1", "uk_w_l42a1");
        }
        if (nation == NAT_RU && !(dmflags & DF_OLD_SNIPER)) {
            return RL_W("SNIPER", "SVT-40", "svt_rifle");
        }
        if (nation == NAT_DE) {
            if (aa || (dmflags & DF_OLD_SNIPER)
                || !Q_stricmpn(axisModel ? axisModel : Cvar_VariableString("dm_playergermanmodel"), "german_panzer", 13)) {
                return RL_W("SNIPER", "KAR98K SNIPER", "kar98sniper");
            }
            return RL_W("SNIPER", "GEWEHR 43", "g43");
        }
        if (nation == NAT_IT) {
            return RL_W("SNIPER", "KAR98K SNIPER", "kar98sniper");
        }
        return RL_W("SNIPER", "SPRINGFIELD '03", "springfield");
    case WC_SMG:
        switch (nation) {
        case NAT_UK:
            return RL_W("SUBMACHINE GUN", "STEN MK II", "sten");
        case NAT_RU:
            return RL_W("SUBMACHINE GUN", "PPSH-41", "ppsh_smg");
        case NAT_DE:
            return RL_W("SUBMACHINE GUN", "MP40", "mp40");
        case NAT_IT:
            return RL_W("SUBMACHINE GUN", "MOSCHETTO", "it_w_moschetto");
        default:
            return RL_W("SUBMACHINE GUN", "THOMPSON", "thompsonsmg");
        }
    case WC_MG:
        if (nation == NAT_UK && bt) {
            return RL_W("MACHINE GUN", "VICKERS-BERTHIER", "uk_w_vickers");
        }
        if (nation == NAT_DE) {
            return RL_W("MACHINE GUN", "STG 44", "mp44");
        }
        if (nation == NAT_IT) {
            return RL_W("MACHINE GUN", "BREDA", "it_w_breda");
        }
        return RL_W("MACHINE GUN", "BAR", "bar");
    case WC_HEAVY:
        if (nation == NAT_UK && bt) {
            return RL_W("ANTI-TANK", "PIAT", "uk_w_piat");
        }
        if (nation == NAT_DE || nation == NAT_IT) {
            return RL_W("ANTI-TANK", "PANZERSCHRECK", "panzerschreck");
        }
        return RL_W("ANTI-TANK", "BAZOOKA", "bazooka");
    case WC_SHOTGUN:
        if (nation == NAT_UK && bt) {
            return RL_W("SILENCED CARBINE", "DE LISLE", "delisle");
        }
        if (nation == NAT_DE && !aa && !(dmflags & DF_DISALLOW_KAR98_MORTAR)) {
            return RL_W("RIFLE GRENADE", "GEWEHRGRANATE", "kar98_mortar");
        }
        return RL_W("SHOTGUN", "TRENCH GUN", "shotgun");
    default:
        if (nation == NAT_DE) {
            return RL_W("MINE SWEEPER", "MINENSUCHGER\xc4T", "gr_w_minedetector");
        }
        return RL_W("MINE SWEEPER", "MINE DETECTOR", "us_w_minedetector");
    }
}

// the nations as the weapon menus' names have them (SelectPrimaryWeapon_<nation>)
static const char *const rl_nationNames[] = {"american", "british", "russian", "german", "italian"};

// the nation the weapon menu was opened for (ui_reloaded_nation)
static int RL_Nation(void)
{
    for (int i = 0; i < (int)ARRAY_LEN(rl_nationNames); i++) {
        if (!Q_stricmp(ui_reloaded_nation->string, rl_nationNames[i])) {
            return i;
        }
    }
    return NAT_US;
}

static int RL_NationTeam(int nation)
{
    return nation == NAT_DE || nation == NAT_IT;
}

/*
====================================================================

PLAYER MODELS

The weapon menu's side tabs and model cycler. Each side's models are the
stock picker's (PlayerModelPickerClass::SetupFiles: models/player's .tik
but the view models and hidden ones, the Axis' german*, axis*, it*, sc*),
in the order of its names ("US - Soldier"). What the menu previews (the
side, each side's model) is kept here for both layers (the back one places
the live model) and set again each time the menu opens; nothing is sent
until a weapon is picked (UIReloadedMenu::Choose)

====================================================================
*/

typedef struct {
    std::string file;  // models/player/<file>.tik
    std::string order; // the picker's name ("DE - Wehrmacht Soldier")
    std::string name;  // shown ("WEHRMACHT SOLDIER")
} rlModel_t;

static struct {
    std::vector<rlModel_t> models[2];
    int                    own[2];  // the player's (dm_playermodel, dm_playergermanmodel), -1 not listed
    int                    pick[2]; // the one previewed
    int                    tab;     // the side previewed
    int                    serial;  // the lists' (the draw cache's key)
    bool                   swallow; // a switch of side sent: the server's push of the weapon menu not shown
    int                    swallowAt;
} rl_wp = {{}, {-1, -1}, {-1, -1}, 0, 0, false, 0};

// the picker's name without its nation ("DE - Wehrmacht Soldier": "WEHRMACHT
// SOLDIER"), else the file's
static std::string RL_ModelName(const char *file)
{
    const char *shown = PM_FilenameToDisplayname(file);
    std::string s;

    if (shown == file) {
        s = file;
        std::replace(s.begin(), s.end(), '_', ' ');
    } else if (strlen(shown) > 5 && shown[2] == ' ' && shown[3] == '-' && shown[4] == ' ') {
        s = shown + 5;
    } else {
        s = shown;
    }
    return RL_Upper(s.c_str());
}

// the nation a side's model gives (fgame's GetPlayerAlliedTeamType and GetPlayerAxisTeamType)
static int RL_ModelNation(int side, const char *file, int target)
{
    if (target < TG_MOHTA) {
        return side ? NAT_DE : NAT_US;
    }
    if (side) {
        if (target >= TG_MOHTT && (!Q_stricmpn(file, "it", 2) || !Q_stricmpn(file, "sc", 2))) {
            return NAT_IT;
        }
        return NAT_DE;
    }
    if (!Q_stricmpn(file, "allied_russian", 14)) {
        return NAT_RU;
    }
    if (!Q_stricmpn(file, "allied_british", 14) || !Q_stricmpn(file, "allied_sas", 10)) {
        return NAT_UK;
    }
    return NAT_US;
}

// a side's own model (dm_playermodel, dm_playergermanmodel): its file (the old
// Waffen-SS names the files' german_waffenss_, as UI_GetPlayerModel_f shows them)
static std::string RL_OwnModel(int side)
{
    const char *own = Cvar_VariableString(side ? "dm_playergermanmodel" : "dm_playermodel");

    if (side && !Q_stricmpn(own, "german_waffen_", 14)) {
        return std::string("german_waffenss_") + (own + 14);
    }
    return own;
}

// the model a side previews: its file (no extension)
static std::string RL_PreviewModel(int side)
{
    const int pick = rl_wp.pick[side];

    if (pick >= 0 && pick < (int)rl_wp.models[side].size()) {
        return rl_wp.models[side][pick].file;
    }
    return RL_OwnModel(side);
}

/*
The weapon menu's figure stands in the game's own unarmed idle (models/human/
animation/idle/generic_idle_B.skc, a loop), which the player models don't
have. So he is a copy of the player model previewed made here, ui/figures/
<model>.tik, read as a file compiled in: the model's TIKI and that idle. The
models the game shows in play aren't touched
*/
// (a name ending in digits would be one of a set played at random)
#define RL_FIGANIM "reloadedidle"

static const char rl_figAnims[] = "\n"
                                  "animations\n"
                                  "{\n"
                                  "\t$path models/human/animation\n"
                                  "\t" RL_FIGANIM "\tidle/generic_idle_B.skc\n"
                                  "}\n";

typedef struct {
    std::string name; // ui/figures/<model>.tik
    std::string text;
} rlFigure_t;

static std::map<std::string, rlFigure_t> rl_figures; // by the model's file, lower case

// the figure of a model's file (no extension): its TIKI's name; the model's own
// when it can't be made
static std::string RL_Figure(const std::string& file)
{
    const std::string own = "models/player/" + file + ".tik";
    std::string       key = file;

    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
    auto it = rl_figures.find(key);
    if (it == rl_figures.end()) {
        rlFigure_t fig;
        void      *buf;
        const long len = FS_ReadFile(own.c_str(), &buf);

        if (len <= 0 || !buf) {
            return own;
        }
        fig.name = "ui/figures/" + key + ".tik";
        fig.text.assign((const char *)buf, len);
        fig.text += rl_figAnims;
        FS_FreeFile(buf);
        it = rl_figures.emplace(key, fig).first;
        if (!UI_CompHud_AddFile(it->second.name.c_str(), it->second.text.data(), (int)it->second.text.size())) {
            rl_figures.erase(it);
            return own;
        }
    }
    return it->second.name;
}

// the nation previewed: the server's (the menu's name) for the side and model it
// was opened for, else what the model gives
static int RL_PreviewNation(int target)
{
    const int t = rl_wp.tab;

    if (RL_NationTeam(RL_Nation()) == t && rl_wp.pick[t] == rl_wp.own[t]) {
        return RL_Nation();
    }
    return RL_ModelNation(t, RL_PreviewModel(t).c_str(), target);
}

// the side with fewer players (Allies on a tie)
static int RL_SmallerSide(void)
{
    int n[2] = {0, 0};

    for (int i = 0; i < MAX_CLIENTS; i++) {
        const char *cs = CL_ConfigString(CS_PLAYERS + i);
        int         team;

        if (!cs[0]) {
            continue;
        }
        team = atoi(Info_ValueForKey(cs, "team"));
        if (team == TEAM_ALLIES) {
            n[0]++;
        } else if (team == TEAM_AXIS) {
            n[1]++;
        }
    }
    return n[1] < n[0];
}

// the menu opened: the models listed, the player's side and models previewed
static void RL_WeaponPreviewReset(void)
{
    const int team = cl.snap.valid ? cl.snap.ps.stats[STAT_TEAM] : TEAM_NONE;
    char    **files;
    int       count;

    for (int t = 0; t < 2; t++) {
        rl_wp.models[t].clear();
        rl_wp.own[t] = rl_wp.pick[t] = -1;
    }
    files = FS_ListFiles("models/player", ".tik", qfalse, &count);
    for (int i = 0; i < count; i++) {
        char      work[128];
        rlModel_t m;
        size_t    len;

        Q_strncpyz(work, files[i], sizeof(work));
        len = strlen(work);
        // (not the view models, not the hidden ones)
        if (len <= 4 || strstr(work, "_fps") || work[0] == '_') {
            continue;
        }
        work[len - 4] = 0;
        m.file  = work;
        m.order = PM_FilenameToDisplayname(work);
        m.name  = RL_ModelName(work);
        rl_wp.models[!Q_stricmpn(work, "german", 6) || !Q_stricmpn(work, "axis", 4) || !Q_stricmpn(work, "it", 2)
                     || !Q_stricmpn(work, "sc", 2)]
            .push_back(m);
    }
    FS_FreeFileList(files);
    for (int t = 0; t < 2; t++) {
        std::vector<rlModel_t>& list = rl_wp.models[t];
        const std::string       own = RL_OwnModel(t);

        std::sort(list.begin(), list.end(), [](const rlModel_t& a, const rlModel_t& b) {
            return Q_stricmp(a.order.c_str(), b.order.c_str()) < 0;
        });
        for (int i = 0; i < (int)list.size(); i++) {
            if (!Q_stricmp(list[i].file.c_str(), own.c_str())) {
                rl_wp.own[t] = rl_wp.pick[t] = i;
                break;
            }
        }
    }
    // the player's side (FFA's too); a spectator's in a team game the one with
    // fewer players, else the menu's nation's
    rl_wp.tab = team == TEAM_ALLIES ? 0
              : team == TEAM_AXIS   ? 1
              : RL_Gametype() >= GT_TEAM ? RL_SmallerSide()
                                         : RL_NationTeam(RL_Nation());
    rl_wp.serial++;
}

/*
====================================================================

ALIASES

====================================================================
*/

static const struct {
    const char *stock, *ours;
} rl_aliases[] = {
    // (the team menus too: the weapon menu picks the side)
    {"SelectTeam",                  "reloaded_weapon"    },
    {"ObjSelectTeam",               "reloaded_weapon"    },
    {"SelectFFAModel",              "reloaded_weapon"    },
    {"SelectPrimaryWeapon",         "reloaded_weapon"    },
    {"SelectPrimaryWeapon_british", "reloaded_weapon"    },
    {"SelectPrimaryWeapon_russian", "reloaded_weapon"    },
    {"SelectPrimaryWeapon_german",  "reloaded_weapon"    },
    {"SelectPrimaryWeapon_italian", "reloaded_weapon"    },
    {"dm_main",                     "reloaded_esc"       },
    {"DM_Scoreboard",               "reloaded_scoreboard"},
    {"DM_Round_Scoreboard",         "reloaded_scoreboard"},
    {"Obj_Scoreboard",              "reloaded_scoreboard"},
};

const char *UI_Menus_Alias(const char *name)
{
    if (!ui_classicmenus || ui_classicmenus->integer || !name) {
        return NULL;
    }
    // (every menu lookup asks, the HUD's 13 each frame: the stock names start with s, o or d)
    switch (name[0]) {
    case 'S':
    case 's':
    case 'O':
    case 'o':
    case 'D':
    case 'd':
        break;
    default:
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_LEN(rl_aliases); i++) {
        if (!Q_stricmp(name, rl_aliases[i].stock)) {
            return rl_aliases[i].ours;
        }
    }
    return NULL;
}

qboolean UI_Menus_Scoreboard(const char *name)
{
    return name && UI_Menus_Alias(name) && menuManager.FindMenuNamed("reloaded_scoreboard") ? qtrue : qfalse;
}

qboolean UI_Menus_Covering(void)
{
    static const char *const covering[] = {"reloaded_weapon", "reloaded_esc"};
    Menu *const              m          = menuManager.CurrentMenu();

    // (asked each frame: most menus' names don't start with r)
    if (!m || tolower((unsigned char)m->m_name.c_str()[0]) != 'r') {
        return qfalse;
    }
    for (size_t i = 0; i < ARRAY_LEN(covering); i++) {
        if (!Q_stricmp(m->m_name.c_str(), covering[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

void UI_GetPlayerModel_f(void);

qboolean UI_Menus_Pushing(const char *name)
{
    const char *alias = UI_Menus_Alias(name);

    if (!alias) {
        return qtrue;
    }
    if (!strcmp(alias, "reloaded_weapon")) {
        // (a weapon menu's name, not a team menu's: the team menus open it too)
        const bool weapon = !Q_stricmpn(name, "SelectPrimaryWeapon", 19);
        const int  side   = cl.snap.valid && cl.snap.ps.stats[STAT_TEAM] == TEAM_AXIS;

        // a side picked in the weapon menu: the server pushes the new side's
        // weapon menu (fgame's Player::Join_DM_Team), the weapon already sent.
        // (Not a team menu: the server's after a refused switch, shown.)
        if (rl_wp.swallow) {
            rl_wp.swallow = false;
            if (weapon && cls.realtime - rl_wp.swallowAt < 3000) {
                return qfalse;
            }
        }
        // already up (a push again, the team menu's or the weapon menu's: not
        // pushed twice): what it previews left as it is
        {
            Menu *const up = menuManager.CurrentMenu();

            if (up && !Q_stricmp(up->m_name.c_str(), "reloaded_weapon")) {
                return qtrue;
            }
        }
        // the nation: the name's (SH/BT servers send each nation its own), else
        // what the player's side's model gives (Allied Assault's Americans and Germans)
        if (weapon && strchr(name, '_')) {
            Cvar_Set("ui_reloaded_nation", strchr(name, '_') + 1);
        } else {
            Cvar_Set("ui_reloaded_nation",
                     rl_nationNames[RL_ModelNation(side, Cvar_VariableString(side ? "dm_playergermanmodel" : "dm_playermodel"),
                                                   com_target_game->integer)]);
        }
        UI_GetPlayerModel_f(); // (called, not executed: the push's own arguments stay)
        RL_WeaponPreviewReset();
        return qtrue;
    }
    // another menu opened since: the push isn't the switch's
    rl_wp.swallow = false;
    return qtrue;
}

void UI_Menus_Init(void)
{
    extern const char *(*ui_menuAlias)(const char *name);

    ui_classicmenus    = Cvar_Get("ui_classicmenus", "0", CVAR_ARCHIVE);
    ui_dm_primary      = Cvar_Get("ui_dm_primary", "", CVAR_ARCHIVE);
    ui_reloaded_nation = Cvar_Get("ui_reloaded_nation", "american", 0);
    ui_reloaded_figure = Cvar_Get("ui_reloaded_figure", "", 0);
    ui_menuAlias       = UI_Menus_Alias;
}

/*
====================================================================

THE WIDGET

ReloadedMenu: "screen weapon|esc|scoreboard", "layer back|front".
The back layer draws what is under the 3D model view (a dimming, the
weapon menu's warm haze) and places the view (the weapon menu's label
named figure); the front layer draws the rest and takes the mouse and the
keys. Both lay the screen out alike from the same data

====================================================================
*/

enum { SCR_WEAPON, SCR_ESC, SCR_SCOREBOARD };

// the vote tile's state, as the stock menu's widgets show it (UI_MultiplayerMainMenuWidgetsUpdate)
enum { VOTE_CALL, VOTE_NOW, VOTE_VOTED, VOTE_OFF };

static int RL_VoteState(void)
{
    if (!Cvar_VariableIntegerValue("cg_allowvote")) {
        return VOTE_OFF;
    }
    if (atoi(CL_ConfigString(CS_VOTE_TIME))) {
        return cl.snap.ps.voted ? VOTE_VOTED : VOTE_NOW;
    }
    return VOTE_CALL;
}


// the front layer's hot item, for the back layer (a frame later)
static int rl_hot[3] = {-1, -1, -1};

typedef struct {
    int   x, y, w, h; // pixels
    int   id;
} rlHit_t;

class UIReloadedMenu : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIReloadedMenu);

    int                  m_screen, m_back;
    int                  m_focus;     // the item hot (hovered or moved to), -1 none
    int                  m_mouse[2];  // where the mouse was
    int                  m_shown;     // the frame last drawn
    std::vector<rlHit_t> m_hits;
    std::vector<int>     m_order;     // the items in order (the arrow keys)
    int                  m_cols;      // the items a row (left and right)

    int                  m_tick;      // the frame Tick ran

    UIReloadedMenu() : m_screen(SCR_WEAPON), m_back(0), m_focus(-1), m_shown(-1), m_cols(1), m_tick(-1)
    {
        m_mouse[0] = m_mouse[1] = -1;
        AllowActivate(true);
    }

    void SetScreen(Event *ev)
    {
        const str s = ev->GetString(1);

        m_screen = !str::icmp(s, "esc") ? SCR_ESC : !str::icmp(s, "scoreboard") ? SCR_SCOREBOARD : SCR_WEAPON;
        AllowActivate(m_screen != SCR_SCOREBOARD);
    }
    void SetLayer(Event *ev) { m_back = !str::icmp(ev->GetString(1), "back"); }
    void MouseUp(Event *ev);
    void Draw(void) override;
    qboolean KeyEvent(int key, unsigned int time) override;
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

    void DrawWeapon(const rlView_t& v, const rlInfo_t& in);
    void DrawEsc(const rlView_t& v, const rlInfo_t& in);
    void DrawScoreboard(const rlView_t& v, const rlInfo_t& in);
};

Event EV_ReloadedMenu_Screen("screen", EV_DEFAULT, "s", "screen", "team, weapon, esc or scoreboard");
Event EV_ReloadedMenu_Layer("layer", EV_DEFAULT, "s", "layer", "back (under the model views) or front");

CLASS_DECLARATION(UIWidget, UIReloadedMenu, NULL) {
    {&EV_ReloadedMenu_Screen, &UIReloadedMenu::SetScreen},
    {&EV_ReloadedMenu_Layer,  &UIReloadedMenu::SetLayer },
    {&W_LeftMouseUp,          &UIReloadedMenu::MouseUp  },
    {NULL,                    NULL                      }
};

void UIReloadedMenu::Hit(const rlView_t& v, float x, float y, float w, float h, int id)
{
    rlHit_t hit;

    hit.x  = RL_P(v, x);
    hit.y  = RL_P(v, y);
    hit.w  = RL_P(v, x + w) - hit.x;
    hit.h  = RL_P(v, y + h) - hit.y;
    hit.id = id;
    m_hits.push_back(hit);
}

int UIReloadedMenu::HitAt(int x, int y)
{
    // (the last added on top)
    for (int i = (int)m_hits.size() - 1; i >= 0; i--) {
        const rlHit_t& h = m_hits[i];

        if (x >= h.x && y >= h.y && x < h.x + h.w && y < h.y + h.h) {
            return h.id;
        }
    }
    return -1;
}

void UIReloadedMenu::MouseUp(Event *ev)
{
    const UIPoint2D p  = MouseEventToClientPoint(ev);
    const int       id = HitAt((int)p.x, (int)p.y);

    if (id >= 0 && !m_back) {
        Choose(id);
    }
}

qboolean UIReloadedMenu::KeyEvent(int key, unsigned int time)
{
    int at, n = (int)m_order.size();

    if (m_back || m_screen == SCR_SCOREBOARD || !n) {
        return qfalse;
    }
    if (key == K_ENTER || key == K_KP_ENTER || key == K_SPACE) {
        if (m_focus >= 0) {
            Choose(m_focus);
        }
        return qtrue;
    }
    if (key != K_UPARROW && key != K_DOWNARROW && key != K_LEFTARROW && key != K_RIGHTARROW) {
        return qfalse;
    }
    // (m_order is the grid, row by row; -1: a place that can't be chosen, a
    // banned class or a vote that can't be cast, stepped over)
    for (at = 0; at < n && (m_focus < 0 || m_order[at] != m_focus); at++) {
    }
    if (at == n) {
        // nothing hot yet: the first that can be chosen
        for (at = 0; at < n && m_order[at] < 0; at++) {
        }
        if (at < n) {
            m_focus = m_order[at];
        }
        return qtrue;
    }
    {
        const bool across = key == K_LEFTARROW || key == K_RIGHTARROW;
        const int  step   = across ? (key == K_LEFTARROW ? -1 : 1) : (key == K_UPARROW ? -m_cols : m_cols);
        const int  row    = at / m_cols;
        int        to     = at;

        // the next place that can be chosen that way (across: within the row; a
        // wide item's other places stepped over)
        do {
            to += step;
        } while (to >= 0 && to < n && (!across || to / m_cols == row) && (m_order[to] < 0 || m_order[to] == m_order[at]));
        if (to < 0 || to >= n || (across && to / m_cols != row)) {
            // down past the last row: the last that can be chosen
            if (!across && step > 0) {
                for (to = n - 1; to > at && m_order[to] < 0; to--) {
                }
            } else {
                to = at;
            }
        }
        at = to;
    }
    m_focus = m_order[at];
    uii.Snd_PlaySound("sound/menu/scroll.wav");
    return qtrue;
}

// the figure from the knees up, CS2's: the view's foot on the screen's, his
// head's top at headY, width wide about cx. The view (as wide as its frame:
// modelaspect) shows RL_FIGSCALE of the 96 units the model is fitted to,
// RL_FIGMID up from his feet in its middle
void UIReloadedMenu::PlaceFigure(const rlView_t& v, float cx, float headY, float width)
{
    UIWidget   *w = m_parent ? m_parent->FindWidget("figure") : NULL;
    const float k = (v.dh - headY) / (RL_FIGHEAD - RL_FIGMID + 48.0f * RL_FIGSCALE); // units a model unit
    const int   x = RL_P(v, cx - width / 2), y = RL_P(v, v.dh - 96.0f * RL_FIGSCALE * k);

    if (w) {
        w->setFrame(UIRect2D(x, y, RL_P(v, cx + width / 2) - x, RL_P(v, v.dh) - y));
    }
}

/*
================
Tick

Each frame the menu shows (from DrawKey, its list drawn again, or from
Draw): the keys to it, what the mouse is over hot, the scoreboard asked for
================
*/
void UIReloadedMenu::Tick(void)
{
    if (m_tick == cls.framecount) {
        return;
    }
    // shown again: nothing hot until the mouse moves or a key is pressed
    if (m_tick != cls.framecount - 1) {
        m_focus    = -1;
        m_mouse[0] = uid.mouseX;
        m_mouse[1] = uid.mouseY;
        if (!m_back) {
            rl_hot[m_screen] = -1;
        }
    }
    m_tick = cls.framecount;
    if (m_back) {
        m_focus = rl_hot[m_screen];
        return;
    }
    if (m_screen == SCR_SCOREBOARD) {
        RL_WantScores();
        return;
    }
    // the keys come here while the menu is up
    if (uWinMan.ActiveControl() != this && menuManager.CurrentMenu() && getParent()
        && menuManager.CurrentMenu()->GetContainerWidget() == getParent()) {
        uWinMan.ActivateControl(this);
    }
    // the mouse moved: what it is over is hot
    if (uid.mouseX != m_mouse[0] || uid.mouseY != m_mouse[1]) {
        const int id = HitAt(uid.mouseX - (int)m_screenframe.pos.x, uid.mouseY - (int)m_screenframe.pos.y);

        m_mouse[0] = uid.mouseX;
        m_mouse[1] = uid.mouseY;
        if (id != m_focus && id >= 0) {
            uii.Snd_PlaySound("sound/menu/scroll.wav");
        }
        m_focus = id;
    }
    rl_hot[m_screen] = m_focus;
}

int UIReloadedMenu::DrawKey(void)
{
    struct {
        int cs, scores, seconds, focus, self, team, vote, gametype, banner;
        int wtab, wpick[2], wserial; // the weapon menu's preview
    } k;
    int h = 0;

    Tick();
    memset(&k, 0, sizeof(k));
    k.cs    = rl_csChanges;
    k.focus = m_focus;
    if (cl.snap.valid) {
        k.self = cl.snap.ps.clientNum;
        k.team = cl.snap.ps.stats[STAT_TEAM];
    }
    if (m_screen == SCR_SCOREBOARD) {
        k.scores = rl_sb.stamp;
    }
    if (m_screen == SCR_WEAPON || m_screen == SCR_ESC || m_screen == SCR_SCOREBOARD) {
        // (the time left's seconds)
        k.seconds = (atoi(CL_ConfigString(CS_MATCHEND)) - cl.serverTime) / 1000;
    }
    if (m_screen == SCR_ESC) {
        k.vote = RL_VoteState();
    }
    if (m_screen == SCR_WEAPON) {
        k.wtab     = rl_wp.tab;
        k.wpick[0] = rl_wp.pick[0];
        k.wpick[1] = rl_wp.pick[1];
        k.wserial  = rl_wp.serial;
    }
    if (m_screen == SCR_SCOREBOARD) {
        // (the round's end banner over the header)
        k.banner = UI_Extras_BannerKey();
    }
    for (size_t i = 0; i + 4 <= sizeof(k); i += 4) {
        int w;

        memcpy(&w, (const byte *)&k + i, 4);
        h = (h ^ w) * 16777619;
    }
    return h;
}

void UIReloadedMenu::Draw(void)
{
    const rlView_t v = RL_View();
    rlInfo_t       in;

    if (clc.state < CA_PRIMED) {
        return;
    }
    if (!rl_bakeOnly) {
        Tick();
    }
    RL_Info(&in);
    m_hits.clear();
    m_order.clear();
    m_cols = 1;
    switch (m_screen) {
    case SCR_WEAPON:
        DrawWeapon(v, in);
        break;
    case SCR_ESC:
        DrawEsc(v, in);
        break;
    default:
        DrawScoreboard(v, in);
        break;
    }
    {
        const vec4_t white = {1, 1, 1, 1};

        uii.Rend_SetColor(white);
    }
}

/*
================
RL_HeaderPanel

The header of the weapon menu, the Esc menu and the scoreboard: a dark
panel, the title (H2 or H3, an emblem before it: team 0 or 1) over its
caption on the left, the time left over "TIME LEFT" on the right; k scales
it (the scoreboard's slimmer header); the title and the caption fit
leftW (0: all the panel but the time)
================
*/
static void RL_HeaderPanel(const rlView_t& v, float x, float y, float w, float h, const char *titleText, float titleSize, int team,
                           const char *captionText, const char *leftText, float k = 1.0f, float leftW = 0.0f)
{
    // (copied: the strings va gives are reused while it draws)
    const std::string title(titleText), caption(captionText), left(leftText ? leftText : "");
    const float       iconD = 40 * k;
    float             tx    = x + 28, room;

    RL_Round(v, x, y, w, h, 6, RL_Rgb(10, 11, 12, 236));
    if (team >= 0) {
        RL_TeamIcon(v, team, tx + iconD / 2, y + 32 * k, iconD);
        tx += iconD + 14;
    }
    room = leftW > 0.0f ? leftW - (tx - x - 28)
                        : x + w - 28 - tx - (left.empty() ? 0.0f : RL_TextW(v, left.c_str(), RL_H3 * k, true) + 24);
    RL_Text(v, tx, y + 32 * k, title.c_str(), titleSize * k, RL_TEXT, true, RL_LEFT, 0, RL_MID, room);
    RL_Text(v, tx, y + 68 * k, caption.c_str(), RL_S2 * Q_max(k, 0.85f), RL_MUTED, false, RL_LEFT, 2.4f, RL_MID, room);
    if (!left.empty()) {
        RL_Text(v, x + w - 28, y + 32 * k, left.c_str(), RL_H3 * k, RL_TEXT, true, RL_RIGHT, 0, RL_MID);
        RL_Text(v, x + w - 28, y + 68 * k, "TIME LEFT", RL_S2 * Q_max(k, 0.85f), RL_MUTED, false, RL_RIGHT, 2.4f, RL_MID);
    }
}

/*
================
Weapon select: one menu for the side, the model and the weapon. The header
panel's emblem and side tabs (ALLIES, AXIS: the one previewed underlined in
its colour) and the time left; the Esc menu's grid, two tiles a row (rifle
and sniper, SMG and MG, anti-tank and shotgun; Breakthrough's mine detector
a row of its own), each the weapon's name in its corner, the nation's
outline, the class as its label, the last pick marked, a banned class
dimmed in its place; a footer panel cycling the side's models (the name,
its place, a dash each); the live model beside. The tabs and chevrons only
preview: a weapon picked sends them (Choose)
================
*/

// the weapon menu's items past its tiles (WC_*)
enum { ID_WTAB = 10, ID_WTAB_AXIS, ID_WPREV, ID_WNEXT };

// the Esc menu's grid: its tiles' size and its width
static void RL_Grid(const rlView_t& v, float *tw, float *th, float *gap, float *gw)
{
    *gap = 14;
    *tw  = v.layout == 0 ? 200.0f : 210.0f;
    *th  = 168;
    *gw  = 3 * *tw + 2 * *gap;
}

void UIReloadedMenu::DrawWeapon(const rlView_t& v, const rlInfo_t& in)
{
    const int                     tab    = rl_wp.tab;
    const int                     nation = RL_PreviewNation(in.target);
    const rlTeam_t&               T      = rl_teams[tab];
    const std::vector<rlModel_t>& models = rl_wp.models[tab];
    const int                     nm = (int)models.size(), pick = rl_wp.pick[tab] < nm ? rl_wp.pick[tab] : -1;
    // (the player's own model unlisted: the one listed is another)
    const bool                    cycle     = nm > 1 || (nm == 1 && pick < 0);
    const std::string             axisModel = RL_PreviewModel(1); // (the German sniper's)
    const int                     n         = in.target >= TG_MOHTT ? 7 : 6;
    const int                     rows      = (n + 1) / 2;
    const float                   head = 104, foot = 104, modelW = 440;
    float                         tw, th, gap, gw;

    RL_Grid(v, &tw, &th, &gap, &gw);
    {
        // the tiles shorter where the rows don't fit (Breakthrough's four at 900 lines)
        const float fit = (v.dh - 80 - head - foot - (rows + 1) * gap) / rows;

        th = Q_min(th, fit);
    }
    const float wtw   = (gw - gap) / 2; // two a row, as wide as the Esc menu's three
    const float total = head + foot + rows * th + (rows + 1) * gap;
    // the block centred, then 150 units right as far as the model's slot stays
    // on screen: the figure is narrower than his slot and lighter than the
    // grid, so centred on the slot the block reads left of the middle
    const float spare = Q_max(0.0f, (v.dw - (gw + 40 + modelW)) / 2);
    const float x0 = spare + Q_min(150.0f, spare), y0 = (v.dh - total) / 2;
    const float gy = y0 + head + gap, fy = y0 + total - foot;

    // (the cvars it draws from: the cache draws again when one changes)
    UI_DrawDep(ui_reloaded_nation);
    UI_DrawDep(ui_dm_primary);
    UI_DrawDep(Cvar_Get("dm_playermodel", "", 0));
    UI_DrawDep(Cvar_Get("dm_playergermanmodel", "", 0));
    for (int i = 0; i < WC_NUM; i++) {
        UI_DrawDep(Cvar_Get(rl_classCvar[i], "1", 0));
    }
    if (m_back) {
        const std::string figure = RL_Figure(RL_PreviewModel(tab));
        const float       cx     = x0 + gw + 40 + modelW / 2;

        // the scene under a warm haze (one layer, CS2's)
        RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(58, 48, 30, 195));
        // the figure beside, of the side and model previewed, from the knees up,
        // his head's top level with the header's; his view as wide as an
        // arm's reach, on screen (he stays in its middle)
        if (Q_stricmp(ui_reloaded_figure->string, figure.c_str())) {
            Cvar_Set("ui_reloaded_figure", figure.c_str());
        }
        PlaceFigure(v, cx, y0 - 6, 2 * Q_min(280.0f, Q_min(cx, v.dw - cx)));
        return;
    }

    // the header: the emblem of the side previewed, the tabs, the time left
    RL_HeaderPanel(v, x0, y0, gw, head, "", RL_H2, -1, "", RL_TimeLeft(in.gametype));
    RL_TeamIcon(v, tab, x0 + 28 + 22, y0 + 52, 44);
    {
        float tx = x0 + 28 + 44 + 24;

        for (int t = 0; t < 2; t++) {
            const bool  on = t == tab, hot = Hot(ID_WTAB + t);
            const float w  = RL_TextW(v, rl_teams[t].name, RL_H2, true);

            RL_Text(v, tx, y0 + 48, rl_teams[t].name, RL_H2, on ? RL_TEXT : hot ? RL_MUTED : RL_FAINT, true, RL_LEFT, 0, RL_MID);
            // the side previewed underlined in its colour, the hot one in brass
            if (on || hot) {
                RL_Box(v, tx, y0 + 69, w, 3, hot ? RL_BRASS : RL_T(rl_teams[t].light));
            }
            Hit(v, tx - 14, y0 + 10, w + 28, head - 20, ID_WTAB + t);
            tx += w + 40;
        }
    }

    // the keys: the tabs, the tiles two a row (a last one alone the whole row), the chevrons
    m_order.push_back(ID_WTAB);
    m_order.push_back(ID_WTAB_AXIS);
    for (int i = 0; i < n; i++) {
        const rlWeapon_t w       = RL_WeaponFor(nation, i, in.target, in.dmflags, axisModel.c_str());
        // (the cgame's cg_weapon_<class>: 0 banned)
        const char      *on      = Cvar_VariableString(rl_classCvar[i]);
        const bool       allowed = !(on[0] == '0' && !on[1]);
        const bool       hot     = Hot(i) && allowed;
        const int        col = i % 2, row = i / 2;
        // (a last tile alone takes the row)
        const float      x = x0 + col * (wtw + gap), y = gy + row * (th + gap), cw = i == n - 1 && !col ? gw : wtw;

        RL_Round(v, x, y, cw, th, 4, hot ? RL_T(T.mid, 150) : RL_Rgb(14, 16, 18, 226));
        if (hot) {
            RL_RoundFrame(v, x, y, cw, th, 4, 2, RL_BRASS);
        }
        // the weapon's name in the corner, the class the tile's label (a banned
        // class's "OFF ON THIS SERVER" in its place)
        RL_Text(v, x + 18, y + 24, w.name, RL_S2, !allowed ? RL_FAINT : hot ? RL_T(T.pale, 220) : RL_MUTED, false, RL_LEFT, 1.8f, RL_MID, cw - 50);
        if (!Q_stricmp(ui_dm_primary->string, rl_classCmd[i])) {
            // the last pick
            RL_Round(v, x + cw - 26, y + 20, 8, 8, 4, RL_BRASS);
        }
        RL_Outline(v, va(RL_OUTLINES "weapons/%s.svg", w.svg), x + cw / 2, y + th * 0.47f, Q_min(wtw * 0.8f, cw - 40), th * 0.38f,
                   allowed ? RL_T(hot ? T.pale : T.light, hot ? 255 : 220) : RL_T(T.light, 70));
        RL_Text(v, x + cw / 2, y + th - 30, allowed ? w.cls : "OFF ON THIS SERVER", RL_S1, allowed ? RL_TEXT : RL_Rgb(200, 120, 100), true, RL_CENTER, 1.6f,
                RL_MID, cw - 20);
        if (allowed) {
            Hit(v, x, y, cw, th, i);
        }
        m_order.push_back(allowed ? i : -1);
    }
    if (n % 2) {
        m_order.push_back(m_order.back());
    }

    // the footer: the side's models, cycled by the chevrons
    RL_Round(v, x0, fy, gw, foot, 6, RL_Rgb(10, 11, 12, 236));
    for (int d = 0; d < 2; d++) {
        const int   id  = d ? ID_WNEXT : ID_WPREV;
        const bool  hot = cycle && Hot(id);
        const float cx  = d ? x0 + gw - 46 : x0 + 46;

        if (hot) {
            RL_Round(v, cx - 30, fy + 22, 60, foot - 44, 4, RL_Rgb(255, 255, 255, 18));
        }
        RL_Outline(v, RL_OUTLINES "glyphs/chevron.svg", cx, fy + foot / 2, 16, 26, !cycle ? RL_Alpha(RL_FAINT, 110) : hot ? RL_BRASS : RL_MUTED,
                   !d);
        if (cycle) {
            Hit(v, cx - 46, fy, 92, foot, id);
        }
        m_order.push_back(cycle ? id : -1);
    }
    {
        const std::string name   = pick >= 0 ? models[pick].name : RL_ModelName(RL_PreviewModel(tab).c_str());
        // a dash a model, while they fit between the chevrons
        const bool        dashes = nm > 1 && nm * 20 - 6 <= gw - 220;
        const float       ny     = fy + (dashes ? 36 : 42);
        char              caption[64];

        if (pick >= 0) {
            Com_sprintf(caption, sizeof(caption), "PLAYER MODEL  \xb7  %d / %d", pick + 1, nm);
        } else {
            Q_strncpyz(caption, "PLAYER MODEL", sizeof(caption));
        }
        RL_Text(v, x0 + gw / 2, ny, name.c_str(), RL_H2, RL_TEXT, true, RL_CENTER, 0, RL_MID, gw - 200);
        RL_Text(v, x0 + gw / 2, ny + 30, caption, RL_S2, RL_MUTED, false, RL_CENTER, 2.4f, RL_MID);
        if (dashes) {
            const float dx = x0 + gw / 2 - (nm * 20 - 6) / 2.0f;

            for (int i = 0; i < nm; i++) {
                RL_Box(v, dx + i * 20, fy + 82, 14, 3, i == pick ? RL_T(T.light) : RL_Rgb(255, 255, 255, 40));
            }
        }
    }
    m_cols = 2;
}

/*
================
The Esc menu: the map, mode and players, the time left; two rows of
tiles (Team & weapon two wide, as the weapon menu picks the side too, and
Call a vote; Options, Spectate, Disconnect), Resume under them and a small
Main menu link
================
*/

enum { ID_WEAPON, ID_VOTE, ID_OPTIONS, ID_SPECTATOR, ID_DISCONNECT, ID_RESUME, ID_MAINMENU };

static const struct {
    int         id, col, row, span; // span: the columns it takes
    const char *label;              // (the vote's from its state)
} rl_escTiles[] = {
    {ID_WEAPON,     0, 0, 2, "TEAM & WEAPON"},
    {ID_VOTE,       2, 0, 1, ""             },
    {ID_OPTIONS,    0, 1, 1, "OPTIONS"      },
    {ID_SPECTATOR,  1, 1, 1, "SPECTATE"     },
    {ID_DISCONNECT, 2, 1, 1, "DISCONNECT"   },
};

void UIReloadedMenu::DrawEsc(const rlView_t& v, const rlInfo_t& in)
{
    const int      team = in.selfTeam == TEAM_AXIS ? 1 : 0;
    const rlTeam_t& T   = rl_teams[team];
    float          tw, th, gap, gw;

    RL_Grid(v, &tw, &th, &gap, &gw);
    const float    head = 104, rb = 56;
    const float    total = head + gap + 2 * th + gap + 22 + rb + 50;
    const float    x0 = (v.dw - gw) / 2, y0 = (v.dh - total) / 2;
    const float    gy = y0 + head + gap;
    const int      vote = RL_VoteState();
    static const char *const voteLabels[] = {"CALL A VOTE", "VOTE NOW", "VOTED", "VOTING OFF"};

    if (m_back) {
        return;
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0, 120));
    {
        char line[128];

        // (bots take slots past sv_maxclients, which the client isn't told: then no limit shown)
        if (in.connected <= in.maxClients) {
            Com_sprintf(line, sizeof(line), "%s  \xb7  %d / %d PLAYERS", RL_ModeName(in.gametype, true), in.connected, in.maxClients);
        } else {
            Com_sprintf(line, sizeof(line), "%s  \xb7  %d PLAYERS", RL_ModeName(in.gametype, true), in.connected);
        }
        RL_HeaderPanel(v, x0, y0, gw, head, RL_MapName(in.map), RL_H3, -1, line, RL_TimeLeft(in.gametype));
    }
    for (const auto& t : rl_escTiles) {
        const int   i = t.id;
        const float x = x0 + t.col * (tw + gap), y = gy + t.row * (th + gap), w = t.span * tw + (t.span - 1) * gap;
        const bool  inert = i == ID_VOTE && (vote == VOTE_VOTED || vote == VOTE_OFF);
        const bool  hot   = Hot(i) && !inert;
        const rlColor_t gc = inert ? RL_FAINT : hot ? RL_TEXT : RL_Rgb(200, 196, 186);
        const float gcx = x + w / 2, gcy = y + th * 0.44f, S = th * 0.30f;

        RL_Round(v, x, y, w, th, 4, hot ? RL_T(T.mid, 150) : RL_Rgb(14, 16, 18, 226));
        if (hot) {
            RL_RoundFrame(v, x, y, w, th, 4, 2, RL_BRASS);
        }
        switch (i) {
        case ID_WEAPON:
            {
                // both emblems, then the nation's SMG
                const int nation = in.target < TG_MOHTA ? (team ? NAT_DE : NAT_US) : RL_Nation();

                RL_TeamIcon(v, 0, gcx - S * 1.12f, gcy, S * 0.62f);
                RL_TeamIcon(v, 1, gcx - S * 0.72f, gcy, S * 0.62f);
                RL_Outline(v, va(RL_OUTLINES "weapons/%s.svg", RL_WeaponFor(nation, WC_SMG, in.target, in.dmflags).svg), gcx + S * 0.68f, gcy,
                           S * 1.5f, S * 0.8f, gc);
            }
            break;
        case ID_VOTE:
            RL_Outline(v, RL_OUTLINES "glyphs/vote.svg", gcx, gcy, S, S, gc);
            break;
        case ID_OPTIONS:
            RL_Outline(v, RL_OUTLINES "glyphs/options.svg", gcx, gcy, S, S, gc);
            break;
        case ID_SPECTATOR:
            RL_Outline(v, RL_OUTLINES "glyphs/spectate.svg", gcx, gcy, S, S, gc);
            break;
        default:
            RL_Outline(v, RL_OUTLINES "glyphs/disconnect.svg", gcx, gcy, S, S, gc);
            break;
        }
        RL_Text(v, x + w / 2, y + th - 30, i == ID_VOTE ? voteLabels[vote] : t.label, RL_S1, inert ? RL_FAINT : RL_TEXT, true, RL_CENTER, 1.6f,
                RL_MID, w - 20);
        if (!inert) {
            Hit(v, x, y, w, th, i);
        }
    }
    {
        const float ry = gy + 2 * th + gap + 22;
        const bool  hot = Hot(ID_RESUME), mhot = Hot(ID_MAINMENU);
        const float mw = RL_TextW(v, "MAIN MENU", RL_S2, false, 2.5f) + 24;

        RL_Round(v, x0, ry, gw, rb, 4, RL_Rgb(255, 255, 255, hot ? 26 : 16));
        RL_RoundFrame(v, x0, ry, gw, rb, 4, hot ? 2 : 1, hot ? RL_BRASS : RL_Rgb(255, 255, 255, 50));
        RL_Text(v, x0 + gw / 2, ry + rb / 2, "RESUME", RL_S1, RL_TEXT, true, RL_CENTER, 2, RL_MID);
        Hit(v, x0, ry, gw, rb, ID_RESUME);
        RL_Text(v, x0 + gw / 2, ry + rb + 30, "MAIN MENU", RL_S2, mhot ? RL_BRASS : RL_MUTED, false, RL_CENTER, 2.5f, RL_MID);
        Hit(v, x0 + gw / 2 - mw / 2, ry + rb + 14, mw, 32, ID_MAINMENU);
    }
    // the keys: the tiles three a row (Team & weapon two places), then Resume, then Main menu
    m_order.push_back(ID_WEAPON);
    m_order.push_back(ID_WEAPON);
    for (int i = ID_VOTE; i <= ID_DISCONNECT; i++) {
        m_order.push_back(i == ID_VOTE && (vote == VOTE_VOTED || vote == VOTE_OFF) ? -1 : i);
    }
    m_order.push_back(ID_RESUME);
    m_order.push_back(ID_RESUME);
    m_order.push_back(ID_RESUME);
    m_order.push_back(ID_MAINMENU);
    m_order.push_back(ID_MAINMENU);
    m_order.push_back(ID_MAINMENU);
    m_cols = 3;
}

/*
================
Choose: what an item does (the stock menus' commands)
================
*/
void UIReloadedMenu::Choose(int id)
{
    const char *cmd = NULL;
    std::string sent;

    switch (m_screen) {
    case SCR_WEAPON:
        if (id == ID_WTAB || id == ID_WTAB_AXIS) {
            // a side previewed
            if (id - ID_WTAB != rl_wp.tab) {
                rl_wp.tab = id - ID_WTAB;
                uii.Snd_PlaySound("sound/menu/apply.wav");
            }
        } else if (id == ID_WPREV || id == ID_WNEXT) {
            // its next or previous model previewed (the figure follows)
            const int t = rl_wp.tab, nm = (int)rl_wp.models[t].size();
            int       p = rl_wp.pick[t];

            if (nm) {
                p = p < 0 || p >= nm ? (id == ID_WNEXT ? 0 : nm - 1) : (p + (id == ID_WNEXT ? 1 : nm - 1)) % nm;
                rl_wp.pick[t] = p;
                uii.Snd_PlaySound("sound/menu/apply.wav");
            }
        } else if (id >= 0 && id < WC_NUM) {
            // the lot sent: the models cycled to, the side previewed, the weapon
            const int team = cl.snap.valid ? cl.snap.ps.stats[STAT_TEAM] : TEAM_NONE;
            bool      models = false;

            sent = "popmenu 0\n";
            for (int t = 0; t < 2; t++) {
                const int p = rl_wp.pick[t];

                if (p >= 0 && p < (int)rl_wp.models[t].size() && p != rl_wp.own[t]) {
                    Cvar_Set(t ? "dm_playergermanmodel" : "dm_playermodel", rl_wp.models[t][p].file.c_str());
                    models = true;
                }
            }
            if (models) {
                // (the userinfo the models are in sent first: CL_CheckUserinfo, a frame on)
                sent += "wait 50\n";
            }
            // the other side (a spectator's any): joined, then the weapon taken there; the
            // server pushes that side's weapon menu again, not shown (UI_Menus_Pushing)
            if (team != (rl_wp.tab ? TEAM_AXIS : TEAM_ALLIES)) {
                sent += rl_wp.tab ? "join_team axis\n" : "join_team allies\n";
                rl_wp.swallow   = true;
                rl_wp.swallowAt = cls.realtime;
            }
            sent += va("primarydmweapon %s\n", rl_classCmd[id]);
            Cvar_Set("ui_dm_primary", rl_classCmd[id]);
            cmd = sent.c_str();
        }
        break;
    case SCR_ESC:
        switch (id) {
        case ID_WEAPON:
            cmd = com_target_game->integer >= TG_MOHTA ? "popmenu 0;wait 50;pickweapon\n" : "popmenu 0;wait 250;pushmenu_weaponselect\n";
            break;
        case ID_VOTE:
            cmd = RL_VoteState() == VOTE_NOW ? "pushvote\n" : RL_VoteState() == VOTE_CALL ? "pushcallvote\n" : NULL;
            break;
        case ID_OPTIONS:
            cmd = "popmenu 0;wait 250;ui_getplayermodel;pushmenu mpoptions\n";
            break;
        case ID_SPECTATOR:
            cmd = "popmenu 0;wait 250;spectator\n";
            break;
        case ID_DISCONNECT:
            cmd = "disconnect;wait 5;popmenu 0;pushmenu main;pushmenu multiplayer\n";
            break;
        case ID_RESUME:
            cmd = "popmenu 0\n";
            break;
        case ID_MAINMENU:
            cmd = "popmenu 0;wait 250;pushmenu main\n";
            break;
        }
        break;
    }
    if (cmd) {
        const std::string text(cmd);

        uii.Snd_PlaySound("sound/menu/apply.wav");
        Cbuf_AddText(text.c_str());
    }
}

/*
================
The scoreboard: CS2's structure in our look. A header band (the map and
mode, ALLIES 4 : 3 AXIS, the time left), a section per team, the leading
one first (its band: emblem, name, players, alive in round modes, the
columns), a row per player (name, K, D, K/D, TIME, PING), the spectators
under it. Rows from the player configstrings, filled from the latest
"scores" ("\x96" where it has no row). Rows tighten from 34 to 22, then the
header slims from 104 to 72, then rows go to 18; past that each team keeps
its top players and folds the rest into "+ N more". FFA: one table ranked
by kills, plain rows with the side's emblem
================
*/

typedef struct {
    const rlPlayer_t *p;
    const rlScore_t  *sc;
    int               side; // 0 allies, 1 axis
} rlRow_t;

static void RL_SortRows(std::vector<rlRow_t>& rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const rlRow_t& a, const rlRow_t& b) {
        // the server's order (kills), then those it hasn't sent
        if (!!a.sc != !!b.sc) {
            return a.sc != NULL;
        }
        return a.sc && a.sc->order < b.sc->order;
    });
}

static const char *RL_KD(const rlScore_t *sc, char *buf, int size)
{
    if (!sc) {
        return "\x96";
    }
    Com_sprintf(buf, size, "%.2f", sc->deaths ? (float)sc->kills / sc->deaths : (float)sc->kills);
    return buf;
}

static void RL_SbHeader(const rlView_t& v, float x, float y, float w, float h, const rlInfo_t& in, const std::vector<rlRow_t>& ffaRows)
{
    const float k = h / 104;
    const char *left = RL_TimeLeft(in.gametype);
    const float cx = x + w / 2;

    // (the title and caption left of the score in the middle)
    RL_HeaderPanel(v, x, y, w, h, RL_MapName(in.map), RL_H3, -1, RL_ModeName(in.gametype, true), left, k, w / 2 - 260 * k - 28);
    if (in.gametype == GT_FFA) {
        const rlRow_t *lead = !ffaRows.empty() && ffaRows[0].sc ? &ffaRows[0] : NULL;
        std::string    line = lead ? "LEADER: " + RL_Upper(lead->p->name) : std::string("NO SCORES YET");

        if (in.fraglimit > 0) {
            line += va("  \xb7  FRAG LIMIT %d", in.fraglimit);
        }
        RL_Text(v, cx, y + 40 * k, lead ? va("%d", lead->sc->kills) : "\x96", RL_H1 * k, RL_TEXT, true, RL_CENTER, 0, RL_MID);
        RL_Text(v, cx, y + 82 * k, line.c_str(), RL_S2 * Q_max(k, 0.85f), RL_MUTED, false, RL_CENTER, 2, RL_MID, w / 2 - 80);
        return;
    }
    for (int t = 0; t < 2; t++) {
        const float sgn = t ? 1.0f : -1.0f;
        const int   s = in.teamScore[t], o = in.teamScore[!t];
        const float nx = cx + sgn * 120 * k;

        RL_Text(v, cx + sgn * 34 * k, y + h / 2 - 2, s >= 0 ? va("%d", s) : "\x96", RL_H1 * k, s >= o ? RL_TEXT : RL_Rgb(200, 196, 186), true,
                t ? RL_LEFT : RL_RIGHT, 0, RL_MID);
        RL_TeamIcon(v, t, nx + sgn * 22 * k, y + h / 2 - 2, 40 * k);
        RL_Text(v, nx + sgn * 52 * k, y + h / 2 - 2, rl_teams[t].name, RL_S1 * k, RL_T(rl_teams[t].pale), true, t ? RL_LEFT : RL_RIGHT, 2, RL_MID);
    }
    RL_Text(v, cx, y + h / 2 - 2, ":", RL_H2 * k, RL_FAINT, true, RL_CENTER, 0, RL_MID);
    RL_Box(v, x, y + h - 4, w / 2, 4, RL_T(rl_teams[0].mid));
    RL_Box(v, x + w / 2, y + h - 4, w / 2, 4, RL_T(rl_teams[1].mid));
}

// the numeric columns, right-aligned, from the right
static const struct {
    const char *label;
    float       w;
} rl_cols[] = {
    {"K",    58},
    {"D",    58},
    {"K/D",  72},
    {"TIME", 88},
    {"PING", 78},
};

static float RL_ColsW(void)
{
    float w = 0;

    for (const auto& c : rl_cols) {
        w += c.w;
    }
    return w;
}

static void RL_SbLabels(const rlView_t& v, float x, float y, float w, rlColor_t c)
{
    float r = x + w - 16;

    for (int i = (int)ARRAY_LEN(rl_cols) - 1; i >= 0; i--) {
        RL_Text(v, r, y, rl_cols[i].label, RL_S2, c, true, RL_RIGHT, 1.5f, RL_MID);
        r -= rl_cols[i].w;
    }
}

static void RL_SbRow(const rlView_t& v, float x, float y, float w, float rh, const rlRow_t& r, int i, int rank, bool rounds, int self)
{
    const rlTeam_t& T    = rl_teams[r.side];
    const bool      me   = r.p->client == self;
    const bool      dead = rounds && r.sc && r.sc->dead;
    const int       a    = dead ? 110 : 255;
    const rlColor_t col  = me ? RL_Alpha(RL_BRASS, a) : dead ? RL_Rgb(150, 146, 136, a) : RL_TEXT;
    const float     fs   = Q_min(RL_S1, rh * 0.66f);
    const float     cy   = y + rh / 2 - 1;
    float           nx   = x + 14;

    // (square: a rounded row would be baked again at each row height; FFA's
    // without the team's colour, the side by its emblem; every other row a
    // little lighter, in the same box)
    {
        const rlColor_t row = rank > 0 ? (me ? RL_Rgb(44, 47, 50, 240) : RL_Rgb(22, 24, 26, 232)) : me ? RL_T(T.base, 235) : RL_T(T.deep, 232);

        RL_Box(v, x, y, w, rh - 2, i % 2 == 0 ? RL_Over(RL_Rgb(255, 255, 255, 9), row) : row);
    }
    if (me) {
        RL_Box(v, x, y, 4, rh - 2, RL_BRASS);
    }
    if (rank > 0) {
        RL_Text(v, x + 14, cy, va("%d", rank), fs, rank == 1 ? RL_BRASS : RL_MUTED, true, RL_LEFT, 0, RL_MID);
        RL_TeamIcon(v, r.side, x + 50, cy, Q_min(18.0f, rh * 0.55f));
        nx = x + 66;
    }
    {
        const float nameW = x + w - 16 - RL_ColsW() - 14 - nx;
        std::string name  = RL_Ellipsize(v, r.p->name, fs, me, nameW);

        RL_Text(v, nx, cy, name.c_str(), fs, col, me, RL_LEFT, 0, RL_MID);
    }
    {
        const char *vals[5];
        float       rx = x + w - 16;
        char        k[16], d[16], ping[16], kd[16];

        Com_sprintf(k, sizeof(k), "%d", r.sc ? r.sc->kills : 0);
        Com_sprintf(d, sizeof(d), "%d", r.sc ? r.sc->deaths : 0);
        Q_strncpyz(ping, r.sc ? (!Q_stricmp(r.sc->ping, "bot") ? "BOT" : r.sc->ping) : "\x96", sizeof(ping));
        vals[0] = r.sc ? k : "\x96";
        vals[1] = r.sc ? d : "\x96";
        vals[2] = RL_KD(r.sc, kd, sizeof(kd));
        vals[3] = r.sc ? r.sc->time : "\x96";
        vals[4] = ping;
        for (int c = 4; c >= 0; c--) {
            rlColor_t vc = col;

            if (c == 2 && r.sc && !dead && !me) {
                const float kd = r.sc->deaths ? (float)r.sc->kills / r.sc->deaths : (float)r.sc->kills;

                vc = kd >= 1.0f ? RL_Rgb(150, 205, 140) : RL_Rgb(220, 140, 120);
            }
            if (c >= 3 && !me) {
                vc = RL_Alpha(RL_MUTED, a);
            }
            RL_Text(v, rx, cy, vals[c], fs, vc, c == 0, RL_RIGHT, 0, RL_MID);
            rx -= rl_cols[c].w;
        }
    }
}

// a team's section: its band, then its players; over most, its lowest-ranked fold into "+ N more"; its height
static float RL_SbTeam(const rlView_t& v, float x, float y, float w, int t, const std::vector<rlRow_t>& rows, bool rounds, float rh, int most, int self)
{
    const rlTeam_t& T  = rl_teams[t];
    const float     bh = 46;
    const int       n  = (int)rows.size();
    const int       shown = most >= 0 && n > most ? Q_max(0, most - 1) : n;
    char            info[64];
    int             alive = 0;

    for (const rlRow_t& r : rows) {
        alive += !(r.sc && r.sc->dead);
    }
    // the band: a gradient from the team's mid to its base
    RL_HGradient(v, x, y, w, bh, RL_T(T.mid, 235), RL_T(T.base, 235));
    RL_TeamIcon(v, t, x + 28, y + bh / 2, 30);
    RL_Text(v, x + 52, y + bh / 2, T.name, RL_S1, RL_TEXT, true, RL_LEFT, 2, RL_MID);
    Com_sprintf(info, sizeof(info), "%d PLAYER%s", n, n == 1 ? "" : "S");
    if (rounds) {
        Q_strcat(info, sizeof(info), va("  \xb7  %d ALIVE", alive));
    }
    {
        const float iw = RL_TextW(v, T.name, RL_S1, true, 2);

        if (x + 52 + iw + 16 + RL_TextW(v, info, RL_S2, false, 2) <= x + w - 16 - RL_ColsW() - 12) {
            RL_Text(v, x + 52 + iw + 16, y + bh / 2, info, RL_S2, RL_T(T.pale, 220), false, RL_LEFT, 2, RL_MID);
        }
    }
    RL_SbLabels(v, x, y + bh / 2, w, RL_T(T.pale, 200));
    for (int i = 0; i < shown; i++) {
        RL_SbRow(v, x, y + bh + 4 + i * rh, w, rh, rows[i], i, 0, rounds, self);
    }
    if (shown < n) {
        RL_Text(v, x + 14, y + bh + 4 + shown * rh + rh / 2 - 1, va("+ %d more", n - shown), Q_min(RL_S1, rh * 0.66f), RL_FAINT, false, RL_LEFT, 0,
                RL_MID);
        return bh + 4 + (shown + 1) * rh;
    }
    return bh + 4 + n * rh;
}

// the spectators' names wrapped to the board's width: every one, on as many lines as it takes
static std::vector<std::string> RL_SpecLines(const rlView_t& v, float w, const std::vector<rlPlayer_t>& names)
{
    std::vector<std::string> lines;
    const float              lead = RL_TextW(v, "SPECTATING", RL_S2, true, 2.5f) + 20;
    std::string              cur;
    float                    cw = lead;

    for (const rlPlayer_t& p : names) {
        const float nw = RL_TextW(v, va("%s  \xb7  ", p.name), RL_S2, false);

        if (!cur.empty() && cw + nw > w - 8) {
            lines.push_back(cur);
            cur.clear();
            cw = lead;
        }
        if (!cur.empty()) {
            cur += "  \xb7  ";
        }
        cur += p.name;
        cw += nw;
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

void UIReloadedMenu::DrawScoreboard(const rlView_t& v, const rlInfo_t& in)
{
    const bool  rounds = in.gametype == GT_TEAM_ROUNDS || in.gametype == GT_OBJECTIVE;
    const float topMin = v.dh >= 1000 ? 48.0f : 28.0f, botMax = v.dh - (v.dh >= 1000 ? 48.0f : 24.0f);
    const float w = Q_min(1180.0f, v.dw - 64), x = (v.dw - w) / 2;
    const float band = 50, gapT = 18;
    std::vector<rlRow_t>     teams[2], ffaRows;
    std::vector<std::string> spec = RL_SpecLines(v, w, in.spectators);
    const float specH  = 26 + (spec.empty() ? 0.0f : 18.0f + 24.0f * spec.size());
    float       hdrH   = 104;
    const float availAll = botMax - topMin - hdrH - 14 - specH;

    if (m_back) {
        return;
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0, 90));
    for (int t = 0; t < 2; t++) {
        for (const rlPlayer_t& p : t ? in.axis : in.allies) {
            rlRow_t r = {&p, RL_Score(p.client), t};

            (in.gametype == GT_FFA ? ffaRows : teams[t]).push_back(r);
        }
    }
    RL_SortRows(teams[0]);
    RL_SortRows(teams[1]);
    RL_SortRows(ffaRows);

    auto spectators = [&](float y) {
        if (spec.empty()) {
            return;
        }
        const float lead = RL_TextW(v, "SPECTATING", RL_S2, true, 2.5f) + 20;

        RL_Text(v, x + 4, y, "SPECTATING", RL_S2, RL_FAINT, true, RL_LEFT, 2.5f, RL_MID);
        for (size_t k = 0; k < spec.size(); k++) {
            RL_Text(v, x + 4 + lead, y + k * 24, spec[k].c_str(), RL_S2, RL_MUTED, false, RL_LEFT, 0, RL_MID);
        }
    };

    if (in.gametype == GT_FFA) {
        const int n = Q_max(1, (int)ffaRows.size());
        float     avail = availAll - 44;

        if (avail / n < 22) {
            hdrH = 72;
            avail += 32;
        }
        const float rh    = Q_max(18.0f, Q_min(34.0f, avail / n));
        const int   cap   = Q_max(1, (int)(avail / rh));
        const int   shown = (int)ffaRows.size() <= cap ? (int)ffaRows.size() : cap - 1;
        const int   per   = shown + (shown < (int)ffaRows.size() ? 1 : 0);
        const float used  = hdrH + 14 + 44 + per * rh + specH;
        const float top   = Q_max(topMin, (v.dh - used) / 2);
        const float y0    = top + hdrH + 14;

        RL_SbHeader(v, x, top, w, hdrH, in, ffaRows);
        RL_Round(v, x, y0, w, 40, 4, RL_Rgb(30, 33, 36, 235));
        RL_Text(v, x + 14, y0 + 20, "#", RL_S2, RL_MUTED, true, RL_LEFT, 1.5f, RL_MID);
        RL_Text(v, x + 66, y0 + 20, "PLAYER", RL_S2, RL_MUTED, true, RL_LEFT, 1.5f, RL_MID);
        RL_SbLabels(v, x, y0 + 20, w, RL_MUTED);
        for (int j = 0; j < shown; j++) {
            RL_SbRow(v, x, y0 + 44 + j * rh, w, rh, ffaRows[j], j, j + 1, false, in.self);
        }
        if (shown < (int)ffaRows.size()) {
            RL_Text(v, x + 14, y0 + 44 + shown * rh + rh / 2 - 1, va("+ %d more", (int)ffaRows.size() - shown), Q_min(RL_S1, rh * 0.66f), RL_FAINT,
                    false, RL_LEFT, 0, RL_MID);
        }
        spectators(y0 + 44 + per * rh + 22);
        return;
    }
    // stacked, the leading team first; over it the round's end banner while
    // it shows (cl_uiextras.cpp), the board moved down for it
    {
        const int first = in.teamScore[1] > in.teamScore[0] ? 1 : 0;
        const int order[2] = {first, !first};
        const int n1 = Q_max(1, (int)teams[order[0]].size()), n2 = Q_max(1, (int)teams[order[1]].size());
        const float banner = UI_Extras_BannerHeight(v), over = banner > 0.0f ? banner + 16.0f : 0.0f;
        const float base = availAll - 2 * band - gapT - over;
        float       avail = base;
        int         caps[2] = {-1, -1};

        if (avail / (n1 + n2) < 22) {
            hdrH  = 72;
            avail = base + 32;
        }
        const float rh  = Q_max(18.0f, Q_min(34.0f, avail / (n1 + n2)));
        const int   cap = Q_max(2, (int)(avail / rh));

        if (n1 + n2 > cap) {
            // the rows shared out: each team up to half, the rest to whichever needs it
            const int h = cap / 2;
            int       m1 = Q_min(n1, h), m2 = Q_min(n2, cap - h), left = cap - m1 - m2;

            m1 += Q_min(left, n1 - m1);
            left = cap - m1 - m2;
            m2 += Q_min(left, n2 - m2);
            caps[0] = m1;
            caps[1] = m2;
        }
        const int   rowsUsed = (caps[0] >= 0 ? caps[0] : n1) + (caps[1] >= 0 ? caps[1] : n2);
        const float used     = over + hdrH + 14 + 2 * band + gapT + rowsUsed * rh + specH;
        const float top      = Q_max(topMin, (v.dh - used) / 2);
        float       y        = top + over + hdrH + 14;

        if (banner > 0.0f) {
            UI_Extras_DrawBanner(v, top);
        }
        RL_SbHeader(v, x, top + over, w, hdrH, in, ffaRows);
        for (int k = 0; k < 2; k++) {
            y += RL_SbTeam(v, x, y, w, order[k], teams[order[k]], rounds, rh, caps[k], in.self) + gapT;
        }
        spectators(y + 4);
    }
}

/*
================
UI_Menus_PreBake

At a map's load (and at a new screen size): each menu laid out with the
data as it is, drawing nothing, so the faces (on the other threads) and
shapes it draws are baked then, not when it opens
================
*/
void UI_Menus_PreBake(void)
{
    static const char *const menus[] = {"reloaded_weapon", "reloaded_esc", "reloaded_scoreboard"};

    if (!ui_classicmenus || ui_classicmenus->integer || clc.state < CA_PRIMED || !uii.IsRendererLoaded()) {
        return;
    }
    UI_BakeBatch(true);
    rl_bakeOnly = true;
    for (const char *name : menus) {
        Menu     *m = menuManager.FindMenu(name);
        UIWidget *c = m ? m->GetContainerWidget() : NULL;

        for (UIWidget *w = c ? c->getFirstChild() : NULL; w; w = c->getNextChild(w)) {
            if (w->isSubclassOf(UIReloadedMenu)) {
                static_cast<UIReloadedMenu *>(w)->Draw();
            }
        }
    }
    rl_bakeOnly = false;
    UI_BakeBatch(false);
    // the figures of the player's own models (both sides) loaded with the map,
    // so the weapon menu's first opening loads nothing
    for (int t = 0; t < 2; t++) {
        re.RegisterModel(RL_Figure(RL_OwnModel(t)).c_str());
    }
}
