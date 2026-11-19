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
// the side and the model too: the team menus show it), the Esc menu, the
// scoreboard, the load screen (UI_Menus_Loading), the end of a mission's
// level, the main menu and the connect's box (cl_uimain.cpp), each a URC
// menu (ui/reloaded_*.urc) of a ReloadedMenu widget, which lays the screen
// out and draws it, and the stock 3D player model views it places. They are
// shown for the stock menus' names (SelectTeam, ObjSelectTeam,
// SelectFFAModel, SelectPrimaryWeapon[_<nation>], dm_main, DM_Scoreboard,
// DM_Round_Scoreboard, Obj_Scoreboard, Tow_Scoreboard, Lib_Scoreboard,
// main, connecting and its errors, StatsScreen_Success|Failed,
// mission_success|failed_N, the vote menus: UI_Menus_Alias; F3's Controls,
// F4's LoadSave and Training's briefingroom their pages: RL_Page; F2's
// mpoptions nothing), so nothing on the
// server or in the cgame changes. The cursor is RL_Cursor's.
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
#include "cl_uiwidget.h"
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

static cvar_t *ui_reloaded_figure; // the figure's model (the label's linkcvar)
static cvar_t *ui_dm_primary;
static cvar_t *ui_reloaded_nation;

/*
====================================================================

DATA

What the menus show, read when drawn: the serverinfo, the player
configstrings (every connected player's name and, from OpenMoHAA's
servers, team) and the latest "scores" the server sent
(UI_Menus_ServerCommand; each player's team there too, the stock
scoreboard's own source)

====================================================================
*/

enum { NAT_US, NAT_UK, NAT_RU, NAT_DE, NAT_IT };

typedef struct {
    int      team; // TEAM_*: the row's own, or its header's (free-for-all)
    int      kills, deaths; // round modes (from round-based on): this round's kills, the match's (no deaths counted)
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
    int       tow[2][5];         // Tug of War: each side's objectives, 1 held
    int       requested;         // when a score was last asked for
} rl_sb;

typedef struct {
    int  client;
    char name[MAX_NAME_LENGTH];
} rlPlayer_t;

struct rlInfo_t {
    int                     gametype;
    int                     target;   // TG_MOH, TG_MOHTA, TG_MOHTT
    int                     dmflags, fraglimit, maxClients;
    char                    map[MAX_QPATH];
    std::vector<rlPlayer_t> allies, axis, spectators; // in client order
    int                     connected;
    int                     self;
    int                     teamScore[2]; // -1 unknown
};

static int RL_Gametype(void)
{
    return atoi(Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "g_gametype"));
}

// a map's name without the spawn point after a $ (out: MAX_QPATH)
static void RL_MapFile(char *out, const char *map)
{
    char *dollar;

    Q_strncpyz(out, map, MAX_QPATH);
    dollar = strchr(out, '$');
    if (dollar) {
        *dollar = 0;
    }
}

// a player's team: the configstring's "team" (OpenMoHAA's servers add it),
// else the latest "scores" row's (stock servers send no key)
static int RL_Team(int client, const char *cs)
{
    const char *team = Info_ValueForKey(cs, "team");

    return team[0] ? atoi(team) : UI_Menus_ScoreTeam(client);
}

static void RL_Info(rlInfo_t *in)
{
    const char *info = CL_ConfigString(CS_SERVERINFO);

    in->gametype   = atoi(Info_ValueForKey(info, "g_gametype"));
    in->target     = com_target_game->integer;
    in->dmflags    = atoi(Info_ValueForKey(info, "dmflags"));
    in->fraglimit  = atoi(Info_ValueForKey(info, "fraglimit"));
    in->maxClients = atoi(Info_ValueForKey(info, "sv_maxclients"));
    RL_MapFile(in->map, Info_ValueForKey(info, "mapname"));
    in->connected = 0;
    in->self      = cl.snap.valid ? cl.snap.ps.clientNum : clc.clientNum;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const char *cs = CL_ConfigString(CS_PLAYERS + i);
        rlPlayer_t  p;
        int         team;

        if (!cs[0]) {
            continue;
        }
        Q_strncpyz(p.name, Info_ValueForKey(cs, "name"), sizeof(p.name));
        if (!p.name[0]) {
            continue;
        }
        p.client = i;
        team     = RL_Team(i, cs);
        in->connected++;
        if (team == TEAM_ALLIES) {
            in->allies.push_back(p);
        } else if (team == TEAM_AXIS) {
            in->axis.push_back(p);
        } else if (team == TEAM_SPECTATOR) {
            in->spectators.push_back(p);
        }
    }
    // the teams' scores: the server's (CS_TEAMSCORES), else the scoreboard's
    if (sscanf(CL_ConfigString(CS_TEAMSCORES), "%d %d", &in->teamScore[0], &in->teamScore[1]) != 2) {
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

// a player's row of "scores": the kills, deaths, time and ping from the argument at (a client out of range: none)
static void RL_ScoreRow(int client, int at, int team, qboolean dead, int& order)
{
    if (client < 0 || client >= MAX_CLIENTS) {
        return;
    }
    rlScore_t *r = &rl_sb.client[client];

    r->team   = team;
    r->kills  = atoi(Cmd_Argv(at));
    r->deaths = atoi(Cmd_Argv(at + 1));
    Q_strncpyz(r->time, Cmd_Argv(at + 2), sizeof(r->time));
    Q_strncpyz(r->ping, Cmd_Argv(at + 3), sizeof(r->ping));
    r->dead  = dead;
    r->order = order++;
    rl_sb.seen[client] = rl_sb.stamp;
}

/*
================
UI_Menus_ServerCommand

"disconnect"'s reason (the connect's box shows it), and "scores"
(fgame's DM_Manager::Score, read as the cgame's CG_ParseScores reads it):
its entries, the team games' a header per team (-1, the team, [SH/BT: the
players], the score, ...) and a row per player (client, team negative when
dead, kills, deaths, time, ping); free-for-all's rows (client, kills,
deaths, time, ping)
================
*/
void UI_Menus_ServerCommand(void)
{
    // (the server drops the player: its reason for the connect's box)
    if (!strcmp(Cmd_Argv(0), "disconnect")) {
        RL_ConnectDropped(Cmd_Argv(1));
        return;
    }
    // (its votes, in parts: the Esc menu's VOTE page's)
    if (!strncmp(Cmd_Argv(0), "vo", 2) && Cmd_Argv(0)[2] >= '0' && Cmd_Argv(0)[2] <= '2' && !Cmd_Argv(0)[3]) {
        RL_VoteRead(Cmd_Argv(0)[2] - '0', Cmd_Argv(1));
        return;
    }
    if (strcmp(Cmd_Argv(0), "scores")) {
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
            // the objectives' states: the Allies' five, the Axis' five
            for (int i = 0; i < 10; i++) {
                rl_sb.tow[i / 5][i % 5] = atoi(Cmd_Argv(cur + i));
            }
            cur += 10;
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
                    // (below protocol 15 a round mode's line has the wins in a row, then the wins: fgame's BuildTeamInfo_ver6)
                    rl_sb.teamScore[team == TEAM_AXIS] = atoi(Cmd_Argv(cur + 6 * i + (!v15 && gametype >= GT_TEAM_ROUNDS ? 3 : 2)));
                }
                continue;
            }
            RL_ScoreRow(client, cur + 6 * i + 2, team, dead, order);
        }
    } else {
        // (a header's client is -1 - its team: the players', the spectators')
        int team = TEAM_FREEFORALL;

        for (int i = 0; i < n; i++) {
            const int client = atoi(Cmd_Argv(cur + 5 * i));

            if (client < 0) {
                team = -1 - client;
                continue;
            }
            RL_ScoreRow(client, cur + 5 * i + 1, team, qfalse, order);
        }
    }
}

static const rlScore_t *RL_Score(int client)
{
    return rl_sb.stamp && rl_sb.seen[client] == rl_sb.stamp ? &rl_sb.client[client] : NULL;
}

int UI_Menus_ScoreTeam(int client)
{
    const rlScore_t *r = client >= 0 && client < MAX_CLIENTS ? RL_Score(client) : NULL;

    return r ? r->team : 0;
}

// the map's display name: its loading screen's (a menu named after the
// map, its first titled label: "V2 Rocket Facility"), else the file's name
const char *RL_MapName(const char *map)
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

static const char *RL_ModeName(int gametype)
{
    static const char *const names[] = {"SINGLE PLAYER", "FREE-FOR-ALL", "TEAM DEATHMATCH", "ROUND-BASED", "OBJECTIVE", "TUG OF WAR", "LIBERATION"};

    return names[gametype >= 0 && gametype < (int)ARRAY_LEN(names) ? gametype : 0];
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

    v.dh = uid.vidHeight >= 900 ? 1080.0f : 900.0f;
    v.s  = uid.vidHeight / v.dh;
    v.dw = uid.vidWidth / v.s;
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
    RL_BoxPx(RL_P(v, x), RL_P(v, y), RL_P(v, x + w), RL_P(v, y + h), c);
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

void RL_Round(const rlView_t& v, float x, float y, float w, float h, float r, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    if (r * v.s < 1.0f) {
        RL_Box(v, x, y, w, h, c);
        return;
    }
    RL_Shape(x0, y0, x1 - x0, y1 - y0, va("roundbox %g 1 1 1 1", r), c, v.s);
}

void RL_RoundFrame(const rlView_t& v, float x, float y, float w, float h, float r, float width, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    RL_Shape(x0, y0, x1 - x0, y1 - y0, va("roundframe %g %g 1 1 1 1", r, width), c, v.s);
}

// a horizontal gradient as strips (a and b at the left and right edges)
void RL_HGradient(const rlView_t& v, float x, float y, float w, float h, rlColor_t a, rlColor_t b)
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

void RL_TeamIcon(const rlView_t& v, int t, float cx, float cy, float d)
{
    RL_Outline(v, rl_teams[t].icon, cx, cy, d, d, {1, 1, 1, 1});
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

rlFace_t RL_Face(const rlView_t& v, float size, bool bold, bool shadow)
{
    rlFace_t face;

    char name[64];

    face.px = Q_max(9, (int)floorf(size * v.s + 0.5f));
    Com_sprintf(name, sizeof(name), "oswald-%s-%dp%s", bold ? "semibold" : "medium", face.px, shadow ? "s" : "");
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
    float           maxw,
    bool            shadow
)
{
    const std::string copy(str); // (a string from va: kept while faces are baked)
    const char       *text = copy.c_str();
    rlFace_t          face = RL_Face(v, size, bold, shadow);
    float             w, lineH, capH, base, px, py;

    if (!face.f || face.page <= 0.0f) {
        return 0.0f;
    }
    while (maxw > 0.0f && size > 8.0f && RL_WidthPx(face, text, track * v.s) > maxw * v.s) {
        size -= 1.0f;
        face = RL_Face(v, size, bold, shadow);
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
std::string RL_Ellipsize(const rlView_t& v, const char *text, float size, bool bold, float maxw)
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

// what a nation gets for a class; axisModel: the Axis model it is for (the German sniper's)
static rlWeapon_t RL_WeaponFor(int nation, int wc, int target, int dmflags, const char *axisModel)
{
    const bool bt = target >= TG_MOHTT, aa = target < TG_MOHTA;

    switch (wc) {
    case WC_RIFLE:
        switch (nation) {
        case NAT_UK:
            return {"RIFLE", "LEE-ENFIELD", "enfield"};
        case NAT_RU:
            return {"RIFLE", "MOSIN-NAGANT", "mosin_nagant_rifle"};
        case NAT_DE:
            return {"RIFLE", "KAR98K", "kar98"};
        case NAT_IT:
            return {"RIFLE", "CARCANO", "it_w_carcano"};
        default:
            return {"RIFLE", "M1 GARAND", "m1_garand"};
        }
    case WC_SNIPER:
        if (nation == NAT_UK && bt) {
            return {"SNIPER", "L42A1", "uk_w_l42a1"};
        }
        if (nation == NAT_RU && !(dmflags & DF_OLD_SNIPER)) {
            return {"SNIPER", "SVT-40", "svt_rifle"};
        }
        if (nation == NAT_DE) {
            if (aa || (dmflags & DF_OLD_SNIPER) || !Q_stricmpn(axisModel, "german_panzer", 13)) {
                return {"SNIPER", "KAR98K SNIPER", "kar98sniper"};
            }
            return {"SNIPER", "GEWEHR 43", "g43"};
        }
        if (nation == NAT_IT) {
            return {"SNIPER", "KAR98K SNIPER", "kar98sniper"};
        }
        return {"SNIPER", "SPRINGFIELD '03", "springfield"};
    case WC_SMG:
        switch (nation) {
        case NAT_UK:
            return {"SUBMACHINE GUN", "STEN MK II", "sten"};
        case NAT_RU:
            return {"SUBMACHINE GUN", "PPSH-41", "ppsh_smg"};
        case NAT_DE:
            return {"SUBMACHINE GUN", "MP40", "mp40"};
        case NAT_IT:
            return {"SUBMACHINE GUN", "MOSCHETTO", "it_w_moschetto"};
        default:
            return {"SUBMACHINE GUN", "THOMPSON", "thompsonsmg"};
        }
    case WC_MG:
        if (nation == NAT_UK && bt) {
            return {"MACHINE GUN", "VICKERS-BERTHIER", "uk_w_vickers"};
        }
        if (nation == NAT_DE) {
            return {"MACHINE GUN", "STG 44", "mp44"};
        }
        if (nation == NAT_IT) {
            return {"MACHINE GUN", "BREDA", "it_w_breda"};
        }
        return {"MACHINE GUN", "BAR", "bar"};
    case WC_HEAVY:
        if (nation == NAT_UK && bt) {
            return {"ANTI-TANK", "PIAT", "uk_w_piat"};
        }
        if (nation == NAT_DE || nation == NAT_IT) {
            return {"ANTI-TANK", "PANZERSCHRECK", "panzerschreck"};
        }
        return {"ANTI-TANK", "BAZOOKA", "bazooka"};
    case WC_SHOTGUN:
        if (nation == NAT_UK && bt) {
            return {"SILENCED CARBINE", "DE LISLE", "delisle"};
        }
        if (nation == NAT_DE && !aa && !(dmflags & DF_DISALLOW_KAR98_MORTAR)) {
            return {"RIFLE GRENADE", "GEWEHRGRANATE", "kar98_mortar"};
        }
        return {"SHOTGUN", "TRENCH GUN", "shotgun"};
    default:
        if (nation == NAT_DE) {
            return {"MINE SWEEPER", "MINENSUCHGER\xc4T", "gr_w_minedetector"};
        }
        return {"MINE SWEEPER", "MINE DETECTOR", "us_w_minedetector"};
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

The weapon menu's side tabs and model cycler, the main menu's profile.
Each side's models are models/player's .tik but the view models and hidden
ones (the Axis' german*, axis*, it*, sc*), in the order of their names
("US - Soldier": PM_FilenameToDisplayname), listed when a menu showing them
opens. What the weapon menu previews (the side, each side's model) is kept
here for both layers (the back one places the live model) and set again
each time the menu opens; nothing is sent until a weapon is picked
(UIReloadedMenu::Choose)

====================================================================
*/

std::vector<rlModel_t> rl_models[2];

static struct {
    int                    own[2];  // the player's (dm_playermodel, dm_playergermanmodel), -1 not listed
    int                    pick[2]; // the one previewed
    int                    tab;     // the side previewed
    int                    serial;  // the lists' (the draw cache's key)
    bool                   swallow; // a switch of side sent: the server's push of the weapon menu not shown
    int                    swallowAt;
} rl_wp = {{-1, -1}, {-1, -1}, 0, 0, false, 0};

// the models' names (the stock model picker's), "US - Soldier"
static const struct {
    const char *fileName;
    const char *displayName;
} pickerModels[] = {
    // 2.0 skins
    {"allied_101st_captain",                    "US - 101st Airborne Captain"    },
    {"allied_101st_infantry",                   "US - 101st Airborne Private 1"  },
    {"allied_101st_scout",                      "US - 101st Airborne Private 2"  },
    {"allied_501st_pir_scout",                  "US - 501st Paratrooper 1"       },
    {"allied_501st_pir_soldier",                "US - 501st Paratrooper 2"       },
    // 1.0 skins
    {"allied_airborne",                         "US - 501st Paratrooper 3"       },
    // Readded in OPM (was removed in 2.0)
    {"allied_manon",                            "US - Manon"                     },
    // 2.0 skins
    {"allied_british_6th_airborne_captain",     "UK - 6th Airborne Captain"      },
    {"allied_british_6th_airborne_paratrooper", "UK - 6th Airborne Paratrooper"  },
    // 1.0 skins
    {"allied_pilot",                            "US - Army Pilot"                },
    // 2.0 skins
    {"allied_russian_corporal",                 "RA - Corporal"                  },
    {"allied_russian_crazy_boris",              "RA - Crazy Boris"               },
    {"allied_russian_recon_scout",              "RA - Junior Lieutenant"         },
    {"allied_russian_recon_soldier",            "RA - Black Sea Petty Officer"   },
    // 1.0 skins
    {"allied_sas",                              "UK - SAS Officer"               },
    {"american_army",                           "US - Soldier"                   },
    {"american_ranger",                         "US - Ranger"                    },
    // 1.0 skins
    {"german_afrika_officer",                   "DE - Afrika Officer"            },
    {"german_afrika_private",                   "DE - Afrika Private"            },
    // 2.0 skins
    {"german_ardennes_artillery_commander",     "DE - Artillery Commander"       },
    {"german_dday_colonel",                     "DE - Normandy Colonel"          },
    // 1.0 skins
    {"german_elite_officer",                    "DE - Elite Officer"             },
    {"german_elite_sentry",                     "DE - Elite Sentry"              },
    {"german_kradshutzen",                      "DE - Kradshutzen"               },
    {"german_panzer_grenadier",                 "DE - Panzer Grenadier"          },
    {"german_panzer_obershutze",                "DE - Panzer Obershutze"         },
    {"german_panzer_shutze",                    "DE - Panzer Shutze"             },
    {"german_panzer_tankcommander",             "DE - Panzer Tank Commander"     },
    {"german_scientist",                        "DE - German Scientist"          },
    // 1.0 skins
    {"german_waffenss_officer",                 "DE - Waffen Officer"            },
    {"german_waffenss_shutze",                  "DE - Waffen Shutze"             },
    {"german_wehrmacht_officer",                "DE - Wehrmacht Officer"         },
    {"german_wehrmacht_soldier",                "DE - Wehrmacht Soldier"         },
    {"german_winter_1",                         "DE - Winter Infantry 1"         },
    {"german_winter_2",                         "DE - Winter Infantry 2"         },
    {"german_worker",                           "DE - German Mechanic"           },
    // 2.11 skins
    {"allied_british_tank_corporal",            "UK - 6th Royal Tank Regiment"   },
    {"allied_russian_seaman",                   "RA - Red Navy Seaman"           },
    {"german_Panzer_Corporal",                  "DE - 1st Panzer Regiment"       },
    {"allied_technician",                       "US - Air Force Technician"      },
    {"german_stukageschwader",                  "DE - Stukageschwader"           },
    // 2.30 skins
    {"It_AX_Ital_Vol",                          "IT - 10th Army XXI Corps"       },
    {"SC_AX_Ital_Inf",                          "IT - 6th Army 202nd Coastal Div"},
    {"SC_AX_Ital_Inf2",                         "IT - 114th Inf Regiment"        },
    {"SC_AX_ITAL_PARA",                         "IT - Paracadutista Militare"    },
    // 2.40 skins
    {"allied_Wheathers",                        "US - General Wheathers"         },
    {"allied_US_Tank",                          "US - II Corps Tank Commander"   },
    {"allied_US_Mask",                          "US - II Corps Infantry"         },
    {"allied_british_Cmd",                      "UK - 10th Corps"                },
    {"allied_british_Tank",                     "UK - 8th Army"                  },
    {NULL,                                      NULL                             }
};

// Allied Assault never named its models: its paratrooper is "Airborne" there,
// not Spearhead's "501st Paratrooper 3" (the third beside its own 1 and 2)
static const char pmAirborneAA[] = "US - Airborne";

const char *PM_FilenameToDisplayname(const char *fileName)
{
    int i;

    if (com_target_game->integer == target_game_e::TG_MOH && !Q_stricmp(fileName, "allied_airborne")) {
        return pmAirborneAA;
    }

    for (i = 0; pickerModels[i].fileName; i++) {
        if (!Q_stricmp(pickerModels[i].fileName, fileName)) {
            return pickerModels[i].displayName;
        }
    }

    return fileName;
}

// the name without its nation ("DE - Wehrmacht Soldier": "Wehrmacht
// Soldier"), else the file's
std::string RL_ModelName(const char *file)
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
    return s;
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
std::string RL_OwnModel(int side)
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

    if (pick >= 0 && pick < (int)rl_models[side].size()) {
        return rl_models[side][pick].file;
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
std::string RL_Figure(const std::string& file)
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
        team = RL_Team(i, cs);
        if (team == TEAM_ALLIES) {
            n[0]++;
        } else if (team == TEAM_AXIS) {
            n[1]++;
        }
    }
    return n[1] < n[0];
}

void RL_ListModels(void)
{
    char **files;
    int    count;

    rl_models[0].clear();
    rl_models[1].clear();
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
        rl_models[!Q_stricmpn(work, "german", 6) || !Q_stricmpn(work, "axis", 4) || !Q_stricmpn(work, "it", 2)
                  || !Q_stricmpn(work, "sc", 2)]
            .push_back(m);
    }
    FS_FreeFileList(files);
    for (std::vector<rlModel_t>& list : rl_models) {
        std::sort(list.begin(), list.end(), [](const rlModel_t& a, const rlModel_t& b) {
            return Q_stricmp(a.order.c_str(), b.order.c_str()) < 0;
        });
    }
}

int RL_OwnIndex(int side)
{
    const std::string own = RL_OwnModel(side);

    for (int i = 0; i < (int)rl_models[side].size(); i++) {
        if (!Q_stricmp(rl_models[side][i].file.c_str(), own.c_str())) {
            return i;
        }
    }
    return -1;
}

// the menu opened: the models listed, the player's side and models previewed
static void RL_WeaponPreviewReset(void)
{
    const int team = cl.snap.valid ? cl.snap.ps.stats[STAT_TEAM] : TEAM_NONE;

    RL_ListModels();
    for (int t = 0; t < 2; t++) {
        rl_wp.own[t] = rl_wp.pick[t] = RL_OwnIndex(t);
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
    {"Tow_Scoreboard",              "reloaded_scoreboard"},
    {"Lib_Scoreboard",              "reloaded_scoreboard"},
    {"main",                        "reloaded_main"      },
    // a connect and how it ends: the connect's box (cl_uimain.cpp)
    {"connecting",                  "reloaded_connect"   },
    {"errormessage",                "reloaded_connect"   },
    {"servertimeout",               "reloaded_connect"   },
    {"serverdisconnected",          "reloaded_connect"   },
    {"disconnected",                "reloaded_connect"   },
    {"badserveraddy",               "reloaded_connect"   },
    {"wrongversion",                "reloaded_connect"   },
    // the end of a mission's level (Spearhead's and Breakthrough's a menu a mission)
    {"StatsScreen_Success",         "reloaded_end"       },
    {"StatsScreen_Failed",          "reloaded_end"       },
    {"mission_success_1",           "reloaded_end"       },
    {"mission_success_2",           "reloaded_end"       },
    {"mission_success_3",           "reloaded_end"       },
    {"mission_failed_1",            "reloaded_end"       },
    {"mission_failed_2",            "reloaded_end"       },
    {"mission_failed_3",            "reloaded_end"       },
};

// Spearhead's and Breakthrough's vote menus (the cgame's: votemain, votecast, votesub*): the Esc menu's VOTE page
// (the page over it), shown in their place (UI_Menus_Pushing)
#define RL_VOTEMENUS "reloaded_options"

// the menu shown for a stock menu's name, NULL: none (the menu of that name)
static const char *UI_Menus_Alias(const char *name)
{
    if (!name) {
        return NULL;
    }
    // (every menu lookup asks, the HUD's 13 each frame: the stock names start with s, o, d, t, l, m, c, e, b, w or v)
    switch (name[0]) {
    case 'V':
    case 'v':
        return Q_stricmpn(name, "vote", 4) ? NULL : RL_VOTEMENUS;
    case 'S':
    case 's':
    case 'O':
    case 'o':
    case 'D':
    case 'd':
    case 'T':
    case 't':
    case 'L':
    case 'l':
    case 'M':
    case 'm':
    case 'C':
    case 'c':
    case 'E':
    case 'e':
    case 'B':
    case 'b':
    case 'W':
    case 'w':
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
    return UI_Menus_Alias(name) && menuManager.FindMenuNamed("reloaded_scoreboard") ? qtrue : qfalse;
}

qboolean UI_Menus_Covering(void)
{
    static const char *const covering[] = {"reloaded_weapon", "reloaded_esc", "reloaded_options"};
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

// F3 and F4 (default.cfg's "pushmenu Controls" and "pushmenu_sp LoadSave", the stock menus): Options' KEYS (in a game
// over the Esc menu, unless the main menu is up), Campaign's saved games (over a game as the Esc menu's LOAD); the end of
// Allied Assault's Training (training.scr's "disconnect; pushmenu briefingroom", the stock briefing room): Campaign's
// missions
static const struct {
    const char *stock;
    int         what; // RL_SHOW_*
} rl_pages[] = {
    {"Controls",     RL_SHOW_KEYS    },
    {"LoadSave",     RL_SHOW_SAVES   },
    {"briefingroom", RL_SHOW_MISSIONS},
};

static void RL_Page(int what)
{
    Menu *const up = menuManager.CurrentMenu();

    RL_MainShow(what);
    if (up == menuManager.FindMenu("main")) {
        return;
    }
    if (what == RL_SHOW_KEYS && clc.state > CA_PRIMED) {
        RL_OverShow(RL_OVER_OPTIONS);
    } else if (up && clc.state > CA_PRIMED) {
        Cbuf_AddText("popmenu 0;wait 250;pushmenu main\n");
    } else {
        UI_PushMenu("main");
    }
}

qboolean UI_Menus_Pushing(const char *name)
{
    const char *alias = UI_Menus_Alias(name);

    if (!alias) {
        for (size_t i = 0; name && i < ARRAY_LEN(rl_pages); i++) {
            if (!Q_stricmp(name, rl_pages[i].stock)) {
                RL_Page(rl_pages[i].what);
                return qfalse;
            }
        }
        // F2's (default.cfg's "pushmenu_dm mpoptions", the stock Multiplayer Options): nothing
        return name && !Q_stricmp(name, "mpoptions") ? qfalse : qtrue;
    }
    if (!strcmp(alias, RL_VOTEMENUS)) {
        // (the cgame's pushcallvote and pushvote, its votes' arrival: the page, not pushed again)
        RL_OverShow(RL_OVER_VOTE);
        return qfalse;
    }
    if (!strcmp(alias, "reloaded_connect")) {
        // a connect's end: what happened, in its box
        if (Q_stricmp(name, "connecting")) {
            RL_ConnectError(name);
        }
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

/*
====================================================================

THE CURSOR

A brass dart (ui/outlines/glyphs/cursor.svg, in its colours: the tip of
its dark edge, the top-left, the hot spot) 22 units high, baked at its
size for the UI's draw and the system's cursor (a window's) when the size
changes, registered again after a map's load or a renderer's restart

====================================================================
*/

#define RL_CURSOR RL_OUTLINES "glyphs/cursor.svg"

static struct {
    int               w, h;   // its size, 0 not baked
    std::vector<byte> texels;
    uihandle_t        shader;
    int               stamp;  // UI_BakeStamp when registered
} rl_cur;

static const byte *RL_Cursor(int *w, int *h, uihandle_t *shader)
{
    static const char *const layer = "svg 1 1 1 1 " RL_CURSOR " left";
    const int                ph    = Q_max(12, RL_P(RL_View(), 22.0f));
    float                    sw, sh;

    if (ph != rl_cur.h) {
        if (!UI_OutlineSize(RL_CURSOR, &sw, &sh)) {
            return NULL;
        }
        rl_cur.h = ph;
        rl_cur.w = (int)ceilf(ph * sw / sh);
        rl_cur.texels.resize(rl_cur.w * rl_cur.h * 4);
        UI_BakeTexels(&layer, 1, rl_cur.w, rl_cur.h, 1.0f, rl_cur.texels.data());
        rl_cur.stamp = UI_BakeStamp() + 1;
    }
    // (registered only for a draw: shader NULL, the system's cursor, wants the
    // texels alone, and a registration at a map's load would move the HUD's
    // pictures in the renderer's 2D atlas)
    if (shader && rl_cur.stamp != UI_BakeStamp()) {
        rl_cur.shader = uii.Rend_RegisterImage2D("*rl_cursor", rl_cur.texels.data(), rl_cur.w, rl_cur.h);
        rl_cur.stamp  = UI_BakeStamp();
    }
    *w = rl_cur.w;
    *h = rl_cur.h;
    if (shader) {
        *shader = rl_cur.shader;
    }
    return rl_cur.texels.data();
}

rlColor_t RL_BRASS = {222 / 255.0f, 178 / 255.0f, 80 / 255.0f, 1}, RL_BRASS_HOT = {236 / 255.0f, 196 / 255.0f, 106 / 255.0f, 1};

void UI_Menus_Init(void)
{
    extern const char *(*ui_menuAlias)(const char *name);
    // the game's accent and, under the mouse, a lighter one (the logos' badges the same)
    static const int accents[3][2][3] = {
        {{222, 178, 80},  {236, 196, 106}},
        {{150, 168, 96},  {172, 190, 122}},
        {{198, 134, 74},  {216, 158, 104}},
    };
    const int(&a)[2][3] = accents[Q_max(0, Q_min(2, com_target_game->integer))];

    RL_BRASS     = RL_Rgb(a[0][0], a[0][1], a[0][2]);
    RL_BRASS_HOT = RL_Rgb(a[1][0], a[1][1], a[1][2]);

    ui_dm_primary      = Cvar_Get("ui_dm_primary", "", CVAR_ARCHIVE);
    ui_reloaded_nation = Cvar_Get("ui_reloaded_nation", "american", 0);
    ui_reloaded_figure = Cvar_Get("ui_reloaded_figure", "", 0);
    ui_menuAlias       = UI_Menus_Alias;
    ui_cursorPicture   = RL_Cursor;
}

/*
====================================================================

THE LOAD SCREEN

Shown while a map loads in place of the stock one (the menu named after
the map, else loading_default). A mission's
level (single player): its orders letter, the stock screen's pictures,
unstretched, full height, black beside it; in the stock CONTINUE's place
what loads and how far, then CONTINUE (reloaded_mission: the game waits
for its continuebutton). Else the map's picture as a 4:3 card in the
middle, over a dimmed, blurred copy
of it filling the screen; the map's name above it on the left, the mode on
the right (the name made smaller, then cut, to leave it room), a brass
bar and what loads under it. The picture is the stock screen's: the dm
and obj maps' photo (a label of its own, its image in textures/mohmenu/
dmloading/ or objloading/), read from its file in colour (its shader fades
it to sepia over white with the alpha); without one (loading_default's
painting, a custom map's own screen) the stock screen's pictures are drawn
into the card, on black

====================================================================
*/

#define RL_LOADSHOT "*rl_loadshot"
#define RL_LOADBLUR "*rl_loadblur"

static struct {
    char      map[MAX_QPATH];
    int       gametype;
    Menu     *stock;         // the stock load screen
    qhandle_t shot, blur;    // the photo and its blurred copy, 0 none
    int       stamp, refSeq; // UI_BakeStamp and the renderer's sequence then
    bool      saveShot;      // a saved game's shot of the next frame (a level's autosave): no CONTINUE in it
} rl_ld;

void UI_Menus_SaveShot(void)
{
    Menu *const m = menuManager.FindMenuNamed("reloaded_mission");

    rl_ld.saveShot = m && m->isVisible();
}

// the photo shrunk to rows texels high in the screen's shape (the part of it
// a screen filled with it shows), blurred, dimmed and darker to the corners:
// stretched over the screen, a blur that costs nothing a frame (the load
// screen's, the main menu's background)
qhandle_t RL_Blur(const char *name, const byte *pic, int pw, int ph, int rows, float dim, float photo)
{
    const float aspect = (float)uid.vidWidth / (float)uid.vidHeight;
    const int   bh = rows, bw = Q_max(1, Q_min(4 * rows, (int)(bh * aspect + 0.5f)));
    float       sx0 = 0, sx1 = (float)pw, sy0 = 0, sy1 = (float)ph;

    if (aspect > photo) {
        sy0 = ph * (1.0f - photo / aspect) * 0.5f;
        sy1 = ph - sy0;
    } else {
        sx0 = pw * (1.0f - aspect / photo) * 0.5f;
        sx1 = pw - sx0;
    }
    std::vector<float> a(bw * bh * 3), b(bw * bh * 3);
    std::vector<byte>  out(bw * bh * 4);

    // each texel the mean of its part of the photo
    for (int y = 0; y < bh; y++) {
        const int ya = (int)(sy0 + (sy1 - sy0) * y / bh), yb = Q_max(ya + 1, (int)(sy0 + (sy1 - sy0) * (y + 1) / bh));

        for (int x = 0; x < bw; x++) {
            const int xa = (int)(sx0 + (sx1 - sx0) * x / bw), xb = Q_max(xa + 1, (int)(sx0 + (sx1 - sx0) * (x + 1) / bw));
            float     sum[3] = {0, 0, 0};

            for (int sy = ya; sy < yb; sy++) {
                for (int sx = xa; sx < xb; sx++) {
                    const byte *p = pic + (sy * pw + sx) * 4;

                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            for (int c = 0; c < 3; c++) {
                a[(y * bw + x) * 3 + c] = sum[c] / ((yb - ya) * (xb - xa));
            }
        }
    }
    // twice 1 2 1 across, then down (the edges clamped)
    for (int pass = 0; pass < 4; pass++) {
        const bool across = pass < 2;

        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                const int i0 = across ? y * bw + Q_max(x - 1, 0) : Q_max(y - 1, 0) * bw + x;
                const int i2 = across ? y * bw + Q_min(x + 1, bw - 1) : Q_min(y + 1, bh - 1) * bw + x;

                for (int c = 0; c < 3; c++) {
                    b[(y * bw + x) * 3 + c] = (a[i0 * 3 + c] + 2 * a[(y * bw + x) * 3 + c] + a[i2 * 3 + c]) * 0.25f;
                }
            }
        }
        a.swap(b);
    }
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            const float r = sqrtf(Square((x + 0.5f) / bw * 2 - 1) + Square((y + 0.5f) / bh * 2 - 1)) * 0.7071f;
            const float k = dim * Q_max(0.0f, 1.0f - 0.7f * powf(r, 1.8f));

            for (int c = 0; c < 3; c++) {
                out[(y * bw + x) * 4 + c] = (byte)Q_min(255.0f, a[(y * bw + x) * 3 + c] * k + 0.5f);
            }
            out[(y * bw + x) * 4 + 3] = 255;
        }
    }
    return re.RegisterTexture2D(name, out.data(), bw, bh);
}

// a stock load screen's photo: of its pictures covering a third to nine
// tenths of the screen (the dm maps': the photo, its white mount, its sepia
// tint), the one with the most texels where the stock maps keep their
// photos, opaque (in colour)
byte *RL_MenuPhoto(Menu *stock, int *width, int *height)
{
    UIWidget *c   = stock ? stock->GetContainerWidget() : NULL;
    byte     *pic = NULL;
    int       w = 0, h = 0;

    for (UIWidget *l = c ? c->getFirstChild() : NULL; l; l = c->getNextChild(l)) {
        const UIRect2D f = l->getFrame(), cf = c->getFrame();
        const float    k = f.size.width * f.size.height / (cf.size.width * cf.size.height);

        if (!l->getMaterial() || k < 0.3f || k > 0.9f) {
            continue;
        }
        for (const char *dir : {"dmloading", "objloading"}) {
            byte *p;
            int   pw, ph;

            if (uii.Rend_LoadRawImage(va("textures/mohmenu/%s/%s.tga", dir, l->getMaterial()->GetName().c_str()), &p, &pw, &ph)) {
                if (pw * ph > w * h) {
                    std::swap(pic, p);
                    w = pw;
                    h = ph;
                }
                if (p) {
                    uii.Rend_FreeRawImage(p);
                }
                break;
            }
        }
    }
    for (int i = 0; pic && i < w * h; i++) {
        pic[i * 4 + 3] = 255;
    }
    *width  = w;
    *height = h;
    return pic;
}

// the load screen's photo, registered, with its blurred copy
static void RL_LoadPictures(void)
{
    int   w, h;
    byte *pic = RL_MenuPhoto(rl_ld.stock, &w, &h);

    rl_ld.shot = rl_ld.blur = 0;
    rl_ld.stamp             = UI_BakeStamp();
    rl_ld.refSeq            = uii.GetRefSequence();
    if (!pic) {
        return;
    }
    rl_ld.shot = re.RegisterTexture2D(RL_LOADSHOT, pic, w, h);
    // (36 texels high, dimmed to 0.42)
    rl_ld.blur = RL_Blur(RL_LOADBLUR, pic, w, h, 36, 0.42f);
    uii.Rend_FreeRawImage(pic);
}

const char *UI_Menus_Loading(Menu *stock, const char *map)
{
    // (a local server's own: its configstring isn't here yet when it loads a map)
    const char *info = com_sv_running->integer ? Cvar_InfoString(CVAR_SERVERINFO) : CL_ConfigString(CS_SERVERINFO);
    const char *ours;
    char        name[MAX_QPATH];

    rl_ld.gametype = atoi(Info_ValueForKey(info, "g_gametype"));
    // (a mission's level waits for CONTINUE as its stock screen does; loading_default's doesn't)
    ours = rl_ld.gametype == GT_SINGLE_PLAYER && stock && Q_stricmp(stock->m_name.c_str(), "loading_default") ? "reloaded_mission"
                                                                                                             : "reloaded_loading";
    if (!menuManager.FindMenuNamed(ours)) {
        return NULL;
    }
    RL_MapFile(name, map);
    if (rl_ld.gametype == GT_SINGLE_PLAYER) {
        Q_strncpyz(rl_ld.map, name, sizeof(rl_ld.map));
        rl_ld.stock = stock;
        rl_ld.shot = rl_ld.blur = 0;
        return ours;
    }
    if (stock != rl_ld.stock || Q_stricmp(name, rl_ld.map)) {
        // (the same map's pictures kept)
        Q_strncpyz(rl_ld.map, name, sizeof(rl_ld.map));
        rl_ld.stock = stock;
        RL_LoadPictures();
    }
    // (the server's name and map, should it drop the player)
    RL_ConnectLoaded(info, name);
    return ours;
}

// a stock screen's pictures (not its bar, button and text) in the box x0 y0
// x1 y1, as on its screen, clipped to it; tall: only those its height (an end
// of a level's painting, not the photos Spearhead's fade in over it)
static void RL_StockPictures(Menu *stock, int x0, int y0, int x1, int y1, bool tall = false)
{
    UIWidget *c = stock ? stock->GetContainerWidget() : NULL;

    RL_SetColor(RL_Rgb(255, 255, 255));
    for (UIWidget *l = c ? c->getFirstChild() : NULL; l; l = c->getNextChild(l)) {
        const UIRect2D f = l->getFrame(), cf = c->getFrame();
        const float    u0 = f.pos.x / cf.size.width, v0 = f.pos.y / cf.size.height;
        const float    u1 = (f.pos.x + f.size.width) / cf.size.width, v1 = (f.pos.y + f.size.height) / cf.size.height;
        const float    cu0 = Q_max(u0, 0.0f), cv0 = Q_max(v0, 0.0f), cu1 = Q_min(u1, 1.0f), cv1 = Q_min(v1, 1.0f);

        if (!l->getMaterial() || !l->isEnabled() || !Q_stricmpn(l->getName(), "loading", 7) || !l->isSubclassOf(UILabel)
            || (l->getTitle() && l->getTitle()[0]) || cu1 <= cu0 || cv1 <= cv0 || (tall && (v0 > 0.0f || v1 < 0.99f))) {
            continue;
        }
        uii.Rend_DrawPicStretched(
            x0 + (x1 - x0) * cu0, y0 + (y1 - y0) * cv0, (x1 - x0) * (cu1 - cu0), (y1 - y0) * (cv1 - cv0), (cu0 - u0) / (u1 - u0),
            (cv0 - v0) / (v1 - v0), (cu1 - u0) / (u1 - u0), (cv1 - v0) / (v1 - v0), l->getMaterial()->GetMaterial()
        );
    }
}

// what loads, from its name's start (UI_LoadResource's callers)
static const char *RL_LoadStage(const char *what)
{
    switch (what[0]) {
    case 'n':
        return "Loading textures";
    case 'k':
        return "Loading sounds";
    case 'm':
    case 'o':
        return "Loading scripts";
    case '*':
        // (*14x: the level's entities spawned; the rest the world's steps)
        return what[1] == '1' && what[2] == '4' ? "Spawning entities" : "Loading the map";
    case 0:
        return "Loading the map";
    default:
        // (a to e, h: the TIKIs and their animations; g, l: the skeletons)
        return "Loading models";
    }
}

// the load bar, 4 high: the part loaded, or (the map's first load: not known) a dash sweeping across
static void RL_LoadBar(const rlView_t& v, float x, float y, float w, float frac)
{
    RL_Box(v, x, y, w, 4, RL_Rgb(255, 255, 255, 30));
    if (frac >= 0.0f) {
        RL_Box(v, x, y, w * Q_min(frac, 1.0f), 4, RL_BRASS);
    } else {
        const float dw = w * 0.2f, dx = (w + dw) * (cls.realtime % 1400) / 1400.0f - dw;

        RL_Box(v, x + Q_max(dx, 0.0f), y, Q_min(dx + dw, w) - Q_max(dx, 0.0f), 4, RL_BRASS);
    }
}

// a mission's level: its letter 4:3 (the stock screen's 640 x 480), black
// beside it; in the stock CONTINUE's box (its middle 460 of 480 down) what
// loads over the bar, then the main menu's brass button (the menu's clear
// continuebutton placed over it takes the click and Enter)
static void RL_DrawMission(UIReloadedMenu *m, const rlView_t& v, float frac, const char *what)
{
    const float lw = v.dh * 4 / 3, lx = (v.dw - lw) / 2, bw = 260, bh = 48, bx = (v.dw - bw) / 2, by = v.dh * 460 / 480 - bh / 2;
    UIWidget   *button = m->getParent() ? m->getParent()->FindWidget("continuebutton") : NULL;

    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0));
    RL_StockPictures(rl_ld.stock, RL_P(v, lx), 0, RL_P(v, lx + lw), RL_P(v, v.dh));
    if (rl_ld.saveShot) {
        // (the painting alone in a saved game's shot)
        rl_ld.saveShot = false;
    } else if (button && button->isEnabled()) {
        const bool hot = uid.mouseX >= RL_P(v, bx) && uid.mouseX < RL_P(v, bx + bw) && uid.mouseY >= RL_P(v, by) && uid.mouseY < RL_P(v, by + bh);

        RL_Box(v, bx, by, bw, bh, hot ? RL_BRASS_HOT : RL_BRASS);
        RL_Text(v, bx + bw / 2, by + bh / 2, "CONTINUE", RL_S1, RL_INK, true, RL_CENTER, 2.4f, RL_MID);
    } else {
        RL_Box(v, bx, by, bw, bh, RL_Rgb(10, 11, 12, 228));
        RL_Text(v, bx + 16, by + (bh - 4) / 2, va("%s\x85", RL_LoadStage(what)), 17, RL_MUTED, false, RL_LEFT, 0, RL_MID);
        if (frac >= 0.0f) {
            RL_Text(v, bx + bw - 16, by + (bh - 4) / 2, va("%d%%", (int)(Q_min(frac, 1.0f) * 100)), 17, RL_TEXT, true, RL_RIGHT, 0, RL_MID);
        }
        RL_LoadBar(v, bx, by + bh - 4, bw, frac);
    }
    m->PlaceView(v, "continuebutton", bx, by, bw, bh);
}

static void RL_DrawLoading(UIReloadedMenu *m, const rlView_t& v)
{
    const char *what = "";
    const float frac = UI_LoadProgress(&what);
    const float cw = Q_min(720.0f, v.dw * 0.88f), ch = cw * 0.75f, head = 80, foot = 70;
    const float x = (v.dw - cw) / 2, y = (v.dh - head - ch - foot) / 2 + head, by = y + ch + 22;
    const int   x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + cw), y1 = RL_P(v, y + ch);
    const rlColor_t white = RL_Rgb(255, 255, 255), line = RL_Rgb(255, 255, 255, 40);

    // a mission: its letter as it loads
    if (rl_ld.gametype == GT_SINGLE_PLAYER) {
        RL_DrawMission(m, v, frac, what);
        return;
    }
    // the renderer restarted since: made again; the map's load reset its shaders: looked up again
    if (rl_ld.shot && rl_ld.refSeq != uii.GetRefSequence()) {
        RL_LoadPictures();
    } else if (rl_ld.shot && rl_ld.stamp != UI_BakeStamp()) {
        rl_ld.shot  = re.RegisterShaderNoMip(RL_LOADSHOT);
        rl_ld.blur  = re.RegisterShaderNoMip(RL_LOADBLUR);
        rl_ld.stamp = UI_BakeStamp();
    }
    if (rl_ld.blur) {
        RL_SetColor(white);
        uii.Rend_DrawPicStretched(0, 0, uid.vidWidth, uid.vidHeight, 0, 0, 1, 1, rl_ld.blur);
    } else {
        RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(8, 9, 10));
    }
    if (rl_ld.shot) {
        RL_SetColor(white);
        uii.Rend_DrawPicStretched(x0, y0, x1 - x0, y1 - y0, 0, 0, 1, 1, rl_ld.shot);
    } else {
        RL_StockPictures(rl_ld.stock, x0, y0, x1, y1);
    }
    RL_BoxPx(x0, y0, x1, y0 + 1, line);
    RL_BoxPx(x0, y1 - 1, x1, y1, line);
    RL_BoxPx(x0, y0 + 1, x0 + 1, y1 - 1, line);
    RL_BoxPx(x1 - 1, y0 + 1, x1, y1 - 1, line);
    {
        // the name in what the mode leaves: smaller (to H2 at least), then cut
        const char *mode = RL_ModeName(rl_ld.gametype);
        const char *name = RL_MapName(rl_ld.map);
        const float room = cw - RL_TextW(v, mode, 16, true, 2.6f) - 32, w = RL_TextW(v, name, RL_H1, true);
        const float size = w > room ? Q_max(RL_H2, floorf(RL_H1 * room / w)) : RL_H1;

        RL_Text(v, x, y - 26, RL_Ellipsize(v, name, size, true, room).c_str(), size, RL_TEXT, true, RL_LEFT, 0, RL_BASE);
        RL_Text(v, x + cw, y - 26, mode, 16, RL_BRASS, true, RL_RIGHT, 2.6f, RL_BASE);
    }
    // the bar, and how far (the map's first load: not known, a dash sweeping across)
    RL_LoadBar(v, x, by, cw, frac);
    if (frac >= 0.0f) {
        RL_Text(v, x + cw, by + 28, va("%d%%", (int)(Q_min(frac, 1.0f) * 100)), RL_S1, RL_TEXT, true, RL_RIGHT, 0, RL_TOP);
    }
    RL_Text(v, x, by + 28, va("%s\x85", RL_LoadStage(what)), RL_S1, RL_MUTED, false, RL_LEFT, 0, RL_TOP);
}

/*
====================================================================

THE WIDGET

ReloadedMenu (cl_uiwidget.h)

====================================================================
*/

// the front layer's hot item, for the back layer (a frame later)
static int rl_hot[SCR_NUM] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

Event EV_ReloadedMenu_Screen(
    "screen", EV_DEFAULT, "s", "screen", "weapon, esc, scoreboard, loading, main, confirm, end, connect, options, hdrblack or hdrpeak"
);
Event EV_ReloadedMenu_Layer("layer", EV_DEFAULT, "s", "layer", "back (under the model views) or front");

CLASS_DECLARATION(UIWidget, UIReloadedMenu, NULL) {
    {&EV_ReloadedMenu_Screen, &UIReloadedMenu::SetScreen},
    {&EV_ReloadedMenu_Layer,  &UIReloadedMenu::SetLayer },
    {&W_LeftMouseUp,          &UIReloadedMenu::MouseUp     },
    {&W_AnyMouseDown,         &UIReloadedMenu::MouseButtons},
    {NULL,                    NULL                         }
};

void UIReloadedMenu::SetScreen(Event *ev)
{
    const str s = ev->GetString(1);

    m_screen = !str::icmp(s, "esc")          ? SCR_ESC
             : !str::icmp(s, "scoreboard") ? SCR_SCOREBOARD
             : !str::icmp(s, "loading")    ? SCR_LOADING
             : !str::icmp(s, "main")       ? SCR_MAIN
             : !str::icmp(s, "confirm")    ? SCR_CONFIRM
             : !str::icmp(s, "end")        ? SCR_END
             : !str::icmp(s, "connect")    ? SCR_CONNECT
             : !str::icmp(s, "options")    ? SCR_OPTIONS
             : !str::icmp(s, "hdrblack")   ? SCR_HDRBLACK
             : !str::icmp(s, "hdrpeak")    ? SCR_HDRPEAK
                                           : SCR_WEAPON;
    AllowActivate(m_screen != SCR_SCOREBOARD && m_screen != SCR_LOADING);
}

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

    if (m_back) {
        return;
    }
    if (Main()) {
        // (a click on nothing too: it closes what is open)
        ChooseMain(id);
    } else if (Hdr()) {
        ChooseHdr(id);
    } else if (id >= 0) {
        Choose(id);
    }
}

// the mouse's buttons as they changed (the buttons down): Options' KEYS take one as a key
void UIReloadedMenu::MouseButtons(Event *ev)
{
    if (!m_back && (m_screen == SCR_MAIN || m_screen == SCR_OPTIONS)) {
        MainButtons(ev->GetInteger(3));
    }
}

void UIReloadedMenu::CharEvent(int ch)
{
    if (!m_back && (m_screen == SCR_MAIN || m_screen == SCR_CONNECT || m_screen == SCR_OPTIONS)) {
        MainChar(ch);
    }
}

qboolean UIReloadedMenu::KeyEvent(int key, unsigned int time)
{
    int at, n = (int)m_order.size();

    if (!m_back && Main()) {
        return MainKey(key);
    }
    if (Hdr()) {
        return HdrKey(key);
    }
    if (m_back || m_screen == SCR_SCOREBOARD || !n) {
        return qfalse;
    }
    if (key == K_ENTER || key == K_KP_ENTER || key == K_SPACE) {
        if (m_focus >= 0) {
            Choose(m_focus);
        } else if (m_screen == SCR_END) {
            // (the end of a level, nothing picked: its last item, CONTINUE)
            Choose(m_order.back());
        }
        return qtrue;
    }
    if (key != K_UPARROW && key != K_DOWNARROW && key != K_LEFTARROW && key != K_RIGHTARROW) {
        return qfalse;
    }
    // (m_order is the grid, row by row; -1: a place that can't be chosen, a
    // banned class, stepped over)
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
    return qtrue;
}

// the figure from the knees up, CS2's: the view's foot on the screen's, his
// head's top at headY, width wide about cx. The view (as wide as its frame:
// modelaspect) shows RL_FIGSCALE of the 96 units the model is fitted to,
// RL_FIGMID up from his feet in its middle
void UIReloadedMenu::PlaceFigure(const rlView_t& v, float cx, float headY, float width)
{
    const float k = (v.dh - headY) / (RL_FIGHEAD - RL_FIGMID + 48.0f * RL_FIGSCALE); // units a model unit
    const float y = v.dh - 96.0f * RL_FIGSCALE * k;

    PlaceView(v, "figure", cx - width / 2, y, width, v.dh - y);
}

void UIReloadedMenu::PlaceView(const rlView_t& v, const char *name, float x, float y, float w, float h)
{
    UIWidget *view = m_parent ? m_parent->FindWidget(name) : NULL;
    const int px = RL_P(v, x), py = RL_P(v, y);

    if (view) {
        view->setFrame(UIRect2D(px, py, RL_P(v, x + w) - px, RL_P(v, y + h) - py));
        view->setShow(w > 0 && h > 0);
    }
}

/*
================
Tick

Each frame the menu shows (from DrawKey, its list drawn again, or from
Draw): the keys to it, what the mouse is over hot (nothing while another
menu is shown over it), the scoreboard asked for, the main menu's servers
queried
================
*/
void UIReloadedMenu::Tick(void)
{
    const bool opened = m_tick != cls.framecount - 1;

    if (m_tick == cls.framecount) {
        return;
    }
    // shown again: nothing hot until the mouse moves or a key is pressed
    if (opened) {
        m_focus    = -1;
        m_mouse[0] = uid.mouseX;
        m_mouse[1] = uid.mouseY;
        if (!m_back) {
            rl_hot[m_screen] = -1;
        }
    }
    m_tick = cls.framecount;
    if (m_screen == SCR_MAIN || m_screen == SCR_CONNECT || m_screen == SCR_OPTIONS) {
        MainTick(opened);
    }
    if (m_screen == SCR_ESC && !m_back) {
        // (a page asked for before it was up: over it now)
        RL_OverTick();
    }
    if (m_back) {
        m_focus = rl_hot[m_screen];
        return;
    }
    if (m_screen == SCR_SCOREBOARD) {
        RL_WantScores();
        return;
    }
    if (!menuManager.CurrentMenu() || !getParent() || menuManager.CurrentMenu()->GetContainerWidget() != getParent()) {
        // another menu over it (Options over the Esc menu: its rail still hot under the mouse, RL_EscClick)
        if (m_screen != SCR_ESC || menuManager.CurrentMenu() != menuManager.FindMenu("reloaded_options")) {
            m_focus = rl_hot[m_screen] = -1;
            return;
        }
    } else if (uWinMan.ActiveControl() != this && !UI_ConsoleHasKeys()) {
        // the keys come here while the menu is up, but for the console or the
        // chat over it: theirs until it closes or the menu is clicked
        uWinMan.ActivateControl(this);
    }
    // the mouse moved: what it is over is hot
    if (uid.mouseX != m_mouse[0] || uid.mouseY != m_mouse[1]) {
        const int id = HitAt(uid.mouseX - (int)m_screenframe.pos.x, uid.mouseY - (int)m_screenframe.pos.y);

        m_mouse[0] = uid.mouseX;
        m_mouse[1] = uid.mouseY;
        m_focus = id;
    }
    rl_hot[m_screen] = m_focus;
    if (Hdr()) {
        HdrTick();
    }
}

int UIReloadedMenu::DrawKey(void)
{
    struct {
        int cs, scores, seconds, focus, self, team, gametype, banner;
        int wtab, wpick[2], wserial; // the weapon menu's preview
        int main;                    // the main menu's
        int over;                    // the Esc menu's: the page over it
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
        // (the round's end banner over the header)
        k.banner = UI_Extras_BannerKey();
    }
    if (m_screen == SCR_WEAPON || m_screen == SCR_ESC || m_screen == SCR_SCOREBOARD) {
        // (the time left's seconds)
        k.seconds = (atoi(CL_ConfigString(CS_MATCHEND)) - cl.serverTime) / 1000;
    }
    if (m_screen == SCR_WEAPON) {
        k.wtab     = rl_wp.tab;
        k.wpick[0] = rl_wp.pick[0];
        k.wpick[1] = rl_wp.pick[1];
        k.wserial  = rl_wp.serial;
    }
    if (Main()) {
        k.main = MainDrawKey();
    }
    if (m_screen == SCR_ESC) {
        k.over = RL_OverPage();
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
    const vec4_t   white = {1, 1, 1, 1};
    rlInfo_t       in;

    if (m_screen == SCR_LOADING) {
        m_hits.clear();
        RL_DrawLoading(this, v);
        uii.Rend_SetColor(white);
        return;
    }
    // (the main menu's, the connect's and the HDR calibration's before any map)
    if (clc.state < CA_PRIMED && m_screen != SCR_MAIN && m_screen != SCR_CONFIRM && m_screen != SCR_CONNECT && !Hdr()) {
        return;
    }
    if (!rl_bakeOnly) {
        Tick();
    }
    m_hits.clear();
    m_order.clear();
    m_cols = 1;
    switch (m_screen) {
    case SCR_MAIN:
        DrawMain(v);
        break;
    case SCR_CONFIRM:
        DrawConfirm(v);
        break;
    case SCR_CONNECT:
        DrawConnect(v);
        break;
    case SCR_OPTIONS:
        DrawOptions(v);
        break;
    case SCR_HDRBLACK:
    case SCR_HDRPEAK:
        DrawHdr(v);
        break;
    case SCR_WEAPON:
        RL_Info(&in);
        DrawWeapon(v, in);
        break;
    case SCR_ESC:
        RL_Info(&in);
        DrawEsc(v, in);
        break;
    case SCR_END:
        RL_Info(&in);
        DrawEnd(v, in);
        break;
    default:
        RL_Info(&in);
        DrawScoreboard(v, in);
        break;
    }
    uii.Rend_SetColor(white);
}

/*
================
RL_HeaderPanel

The header of the weapon menu, the Esc menu and the scoreboard: a dark
panel, the title (H2 or H3) over its
caption on the left, the time left over "TIME LEFT" on the right; k scales
it (the scoreboard's slimmer header); the title and the caption fit
leftW (0: all the panel but the time)
================
*/
static void RL_HeaderPanel(const rlView_t& v, float x, float y, float w, float h, const char *titleText, float titleSize,
                           const char *captionText, const char *leftText, float k = 1.0f, float leftW = 0.0f)
{
    // (copied: the strings va gives are reused while it draws)
    const std::string title(titleText), caption(captionText), left(leftText ? leftText : "");
    const float       tx = x + 28;
    float             room;

    RL_Round(v, x, y, w, h, 6, RL_Rgb(10, 11, 12, 236));
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
its colour) and the time left; the menus' grid, two tiles a row (rifle
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

// the menus' grid (the weapon menu's, the end of a level's column): three tiles' size and its width
static void RL_Grid(const rlView_t& v, float *tw, float *th, float *gap, float *gw)
{
    *gap = 14;
    *tw  = v.dw < 1500.0f ? 200.0f : 210.0f; // (narrower on a narrow screen)
    *th  = 168;
    *gw  = 3 * *tw + 2 * *gap;
}

void UIReloadedMenu::DrawWeapon(const rlView_t& v, const rlInfo_t& in)
{
    const int                     tab    = rl_wp.tab;
    const int                     nation = RL_PreviewNation(in.target);
    const rlTeam_t&               T      = rl_teams[tab];
    const std::vector<rlModel_t>& models = rl_models[tab];
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
    const float wtw   = (gw - gap) / 2; // two a row, as wide as three
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
    RL_HeaderPanel(v, x0, y0, gw, head, "", RL_H2, "", RL_TimeLeft(in.gametype));
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
        const std::string name   = RL_Upper((pick >= 0 ? models[pick].name : RL_ModelName(RL_PreviewModel(tab).c_str())).c_str());
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
The Esc menu: the main menu's rail over the game, blurred (the renderer's
"*frameblur", made where it is drawn: nothing while the menu is closed) and
dimmed; under the lockup the map, the mode and players, the time left;
RESUME, MAIN MENU, TEAM & WEAPON (the weapon menu picks the side too),
OPTIONS (shown over it, lit while it is: ui/reloaded_options.urc),
SPECTATE, in Spearhead and Breakthrough VOTE (its page shown over it as
Options is); DISCONNECT and QUIT at its foot. In a mission (single player):
the level's name, the mission and the difficulty; RESUME, MAIN MENU, SAVE,
LOAD (Campaign's saved games), OPTIONS; LEAVE MISSION and QUIT
================
*/

enum { ID_MAINMENU, ID_WEAPON, ID_OPTIONS, ID_SPECTATOR, ID_DISCONNECT, ID_QUIT, ID_RESUME, ID_SAVE, ID_LOAD, ID_CONTINUE, ID_VOTE };

// multiplayer's (VOTE Spearhead's and Breakthrough's), single player's
static const rlRailItem_t rl_escItems[2][6] = {
    {{ID_RESUME, "RESUME", NULL}, {ID_MAINMENU, "MAIN MENU", NULL}, {ID_WEAPON, "TEAM & WEAPON", NULL}, {ID_OPTIONS, "OPTIONS", NULL},
     {ID_SPECTATOR, "SPECTATE", NULL}, {ID_VOTE, "VOTE", NULL}},
    {{ID_RESUME, "RESUME", NULL}, {ID_MAINMENU, "MAIN MENU", NULL}, {ID_SAVE, "SAVE", NULL}, {ID_LOAD, "LOAD", NULL},
     {ID_OPTIONS, "OPTIONS", NULL}},
};

// the game under it blurred: the renderer's "*frameblur", 0 without one (the dim alone: the Metal renderer for now)
static qhandle_t RL_FrameBlur(void)
{
    static qhandle_t shader;
    static int       stamp = -1, refSeq = -1;

    if (stamp != UI_BakeStamp() || refSeq != uii.GetRefSequence()) {
        stamp  = UI_BakeStamp();
        refSeq = uii.GetRefSequence();
        shader = uii.Rend_ImageExists("*frameblur.tga") ? uii.Rend_RegisterMaterial("*frameblur") : 0;
    }
    return shader;
}

void UIReloadedMenu::DrawEsc(const rlView_t& v, const rlInfo_t& in)
{
    const bool         sp      = in.gametype == GT_SINGLE_PLAYER;
    const qhandle_t    blur    = RL_FrameBlur();
    const rlRailItem_t foot[2] = {{ID_DISCONNECT, sp ? "LEAVE MISSION" : "DISCONNECT", RL_OUTLINES "glyphs/disconnect.svg"},
                                  {ID_QUIT, "QUIT", RL_OUTLINES "glyphs/quit.svg"}};
    std::string        head[3];

    if (m_back) {
        return;
    }
    if (blur && !rl_bakeOnly) {
        RL_SetColor(RL_Rgb(255, 255, 255));
        uii.Rend_DrawPicStretched(0, 0, uid.vidWidth, uid.vidHeight, 0, 0, 1, 1, blur);
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0, 120));
    if (sp) {
        // (the mission's line, its difficulty under it)
        const char        *level = CL_ConfigString(CS_MESSAGE);
        const std::string  line  = RL_MissionLine(in.map);
        const size_t       cut   = line.rfind("  \xb7  ");

        head[0] = level[0] ? level : in.map;
        head[1] = line.substr(0, cut);
        head[2] = cut == std::string::npos ? "" : line.substr(cut + 5);
    } else {
        const char *left = RL_TimeLeft(in.gametype);

        head[0] = RL_MapName(in.map);
        head[1] = RL_ModeName(in.gametype);
        // (bots take slots past sv_maxclients, which the client isn't told: then no limit shown)
        head[2] = in.connected <= in.maxClients ? va("%d / %d PLAYERS", in.connected, in.maxClients) : va("%d PLAYERS", in.connected);
        if (left[0]) {
            head[2] += va("  \xb7  %s LEFT", left);
        }
    }
    {
        const int n = !sp && in.target >= TG_MOHTA ? 6 : 5, over = RL_OverPage();

        RL_Rail(this, v, rl_escItems[sp], n, over == RL_OVER_VOTE ? ID_VOTE : over == RL_OVER_OPTIONS ? ID_OPTIONS : -1, foot, 2, head);
        // the keys: down the items, then the foot's
        for (int i = 0; i < n; i++) {
            m_order.push_back(rl_escItems[sp][i].id);
        }
    }
    m_order.push_back(ID_DISCONNECT);
    m_order.push_back(ID_QUIT);
}

/*
================
The end of a mission's level (single player), shown for the statistics
screens the cgame shows at the intermission (StatsScreen_Success and
_Failed; Spearhead's and Breakthrough's mission_success_N and
mission_failed_N): the stock screen's painting (Spearhead's and
Breakthrough's: the level's load screen's, theirs being black or
letterboxed), unstretched, full height, black beside it (not the photos
Spearhead's fade in over it), dimmed as the Esc menu dims the game; on
it a column as wide as the grid: the level's name, the mission and the difficulty, the outcome; the stock screen's numbers
(the ui_* cvars of the stats the cgame asks for: CG_ParseStats); the
weapon most hits came from and where they landed; the medal won, if one
was; MAIN MENU and CONTINUE (TRY AGAIN when failed), which sends the fire
the game waits for (not taken in its first 4 s: Player's intermission).
Pushed, not just shown, so that its buttons take the mouse and the keys
(UI_Menus_End). The stock screens' sounds as their labels play them:
Spearhead's poem and its music, Breakthrough's music
================
*/

#define RL_ENDWAIT 4000 // what the game waits before it takes the fire

static struct {
    Menu *stock;      // the stock screen it is shown for
    bool  ok;         // a success
    int   mission;    // Spearhead's and Breakthrough's number, 0 Allied Assault's
    int   at, drawn;  // when it was pushed, last drawn
    int   sound;      // the music: 0 not begun, 1 held, 2 raised
} rl_end;

// the medals a level can award: the stock medal cases' (ui/medals.urc) cvar
// and picture, the name on its sign
static const struct {
    int         game;
    const char *cvar, *pic, *name;
} rl_medals[] = {
    {TG_MOH,   "g_medal0",    "armycommendation_med",                 "Army Commendation Medal"        },
    {TG_MOH,   "g_medal1",    "distinguishedservice_med",             "Distinguished Service Medal"    },
    {TG_MOH,   "g_medal2",    "legionmerit_med",                      "The Legion of Merit"            },
    {TG_MOH,   "g_medal3",    "goodconduct_med",                      "The Good Conduct Medal"         },
    {TG_MOH,   "g_medal4",    "norwegiancross",                       "Norwegian War Cross With Sword" },
    {TG_MOH,   "g_medal5",    "americanmedal",                        "American Campaign Medal"        },
    {TG_MOH,   "g_eogmedal0", "distinguishedcross_med",               "Distinguished Service Cross"    },
    {TG_MOH,   "g_eogmedal1", "silverstar_med",                       "Silver Star"                    },
    {TG_MOH,   "g_eogmedal2", "bronzestar_med",                       "Bronze Star"                    },
    {TG_MOHTA, "g_medal0",    "distinguishedcross_med",               "Distinguished Service Cross"    },
    {TG_MOHTA, "g_medal1",    "silverstar_med",                       "Silver Star"                    },
    {TG_MOHTA, "g_medal2",    "bronzestar_med",                       "Bronze Star"                    },
    {TG_MOHTA, "g_medal3",    "textures/mohmenu/frangermstar_med",    "The France And Germany Star"    },
    {TG_MOHTA, "g_medal4",    "textures/mohmenu/croixdeguerre_med",   "Belgian Croix De Guerre"        },
    {TG_MOHTA, "g_medal5",    "textures/mohmenu/captureofberlin_med", "Medal For The Capture Of Berlin"},
    {TG_MOHTT, "g_medalbt0",  "distinguishedcross_med",               "Distinguished Service Cross"    },
    {TG_MOHTT, "g_medalbt1",  "silverstar_med",                       "Silver Star"                    },
    {TG_MOHTT, "g_medalbt2",  "bronzestar_med",                       "Bronze Star"                    },
    {TG_MOHTT, "g_medalbt3",  "africastar_med",                       "Africa Star"                    },
    {TG_MOHTT, "g_medalbt4",  "distinguishedflyingcross_med",         "Distinguished Flying Cross"     },
    {TG_MOHTT, "g_medalbt5",  "airmedal_med",                         "Air Medal"                      },
};

qboolean UI_Menus_End(const char *name, Menu *m, qboolean show)
{
    // (the menu found for the stock name: UI_Menus_Alias's)
    if (Q_stricmp(m->m_name.c_str(), "reloaded_end")) {
        return qfalse;
    }
    if (!show) {
        if (menuManager.CurrentMenu() == m) {
            menuManager.PopMenu(qfalse);
            UI_FocusMenuIfExists();
        }
        return qtrue;
    }
    // (shown each frame of the intermission: pushed once, not over a menu opened from it)
    if (m->isVisible() || menuManager.CurrentMenu()) {
        return qtrue;
    }
    // (taken down by Esc a moment ago: its wait and its sounds go on)
    if (cls.realtime - rl_end.drawn > 1000) {
        UIWidget *c;

        rl_end.stock   = menuManager.FindMenuNamed(name);
        rl_end.ok      = !Q_stricmp(name, "StatsScreen_Success") || !Q_stricmpn(name, "mission_success_", 16);
        rl_end.mission = Q_stricmpn(name, "mission_", 8) ? 0 : atoi(strrchr(name, '_') + 1);
        rl_end.at      = cls.realtime;
        rl_end.sound   = 0;
        // (a stock menu's widgets load when it is first shown)
        c = rl_end.stock ? rl_end.stock->GetContainerWidget() : NULL;
        if (c && c->isSubclassOf(UIWidgetContainer) && static_cast<UIWidgetContainer *>(c)->m_layout) {
            static_cast<UIWidgetContainer *>(c)->m_layout->ForceLoad();
        }
    }
    UI_PushMenu("reloaded_end");
    return qtrue;
}

// the stock screen's sounds, as its labels play them: 2 s on, Spearhead's
// poem (the game's sounds stopped) and the music at 0.35, held (by the
// mission) then raised to full over 2 s; Breakthrough's music so
static void RL_EndSounds(void)
{
    static const float hold[3] = {120, 70, 77};
    const int          t       = cls.realtime - rl_end.at - 2000;
    const bool         sh      = com_target_game->integer == TG_MOHTA;

    if (!rl_end.ok || rl_end.mission < 1 || rl_end.mission > 3 || t < 0 || rl_end.sound == 2) {
        return;
    }
    if (!rl_end.sound) {
        if (sh) {
            uii.Snd_StopAllSound();
            uii.Snd_FadeSound(0);
            uii.Snd_PlaySoundDialog(va("sound/menu/mission%dpoem.mp3", rl_end.mission));
        }
        Cbuf_AddText(va("tmstartloop sound/music/mus_stat_%c%d.mp3\ntmvolume 0.35\n", sh ? 't' : 'e', rl_end.mission));
        rl_end.sound = 1;
    } else if (t > hold[rl_end.mission - 1] * 1000) {
        const float k = Q_min(1.0f, (t - hold[rl_end.mission - 1] * 1000) / 2000.0f);

        Cbuf_AddText(va("tmvolume %4.2f\n", 0.35f + 0.65f * k));
        rl_end.sound = k < 1.0f ? 1 : 2;
    }
}

void UIReloadedMenu::DrawEnd(const rlView_t& v, const rlInfo_t& in)
{
    static const char *const hitNames[5] = {"HEAD", "TORSO", "ARMS", "GROIN", "LEGS"};
    const float hits[5] = {Cvar_VariableValue("ui_HeadShots"), Cvar_VariableValue("ui_TorsoShots"),
                           Cvar_VariableValue("ui_LeftArmShots") + Cvar_VariableValue("ui_RightArmShots"), Cvar_VariableValue("ui_GroinShots"),
                           Cvar_VariableValue("ui_LeftLegShots") + Cvar_VariableValue("ui_RightLegShots")};
    const std::string numbers[6][2] = {
        {va("%d", Cvar_VariableIntegerValue("ui_NumEnemysKilled")), "ENEMIES KILLED" },
        {va("%d%%", (int)(Cvar_VariableValue("ui_Accuracy") + 0.5f)), "ACCURACY"       },
        {va("%d", Cvar_VariableIntegerValue("ui_NumComplete")), "OBJECTIVES DONE"},
        {va("%d", Cvar_VariableIntegerValue("ui_NumShotsFired")), "SHOTS FIRED"    },
        {va("%d", Cvar_VariableIntegerValue("ui_NumHits")), "SHOTS HIT"      },
        {va("%d", Cvar_VariableIntegerValue("ui_NumHitsTaken")), "HITS TAKEN"     },
    };
    const std::string weapon(Cvar_VariableString("ui_PreferredWeapon"));
    // (copied: va's buffers are reused while it draws)
    const char       *found = UI_CompHud_WeaponOutline(weapon.c_str());
    const std::string outline(found ? found : "");
    const char       *level   = CL_ConfigString(CS_MESSAGE);
    const float       wait    = (cls.realtime - rl_end.at) / (float)RL_ENDWAIT;
    const float       lw = v.dh * 4 / 3, lx = (v.dw - lw) / 2;
    const float       th = 112, mh = 76, bh = 48, head = 104;
    int               medal = -1;
    float             tw, wh, gap, gw; // (the grid; wh its tiles' height)

    // the medal won: the first whose cvar the level set (as the game tells, g_gotmedal: ui_gotmedal)
    for (int i = 0; medal < 0 && i < (int)ARRAY_LEN(rl_medals) && Cvar_VariableIntegerValue("ui_gotmedal"); i++) {
        const cvar_t *c = rl_medals[i].game == Q_min(in.target, TG_MOHTT) ? Cvar_FindVar(rl_medals[i].cvar) : NULL;

        if (c && c->modificationCount > 1) {
            medal = i;
        }
    }
    RL_Grid(v, &tw, &wh, &gap, &gw);
    const float x0 = (v.dw - gw) / 2;
    float       y  = (v.dh - (head + gap + 2 * th + 2 * gap + wh + (medal < 0 ? 0 : gap + mh) + 22 + bh)) / 2;
    float       aspect   = 1;
    qhandle_t   painting = 0; // (Spearhead's and Breakthrough's: the level's)

    if (!rl_bakeOnly) {
        rl_end.drawn = cls.realtime;
        RL_EndSounds();
        painting = RL_LevelPainting(in.map, &aspect);
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0));
    if (painting > 0) {
        // (as wide as its shape gives, its sides cut on a narrower screen)
        const float pw = Q_min(v.dh * aspect, v.dw), cut = (1.0f - pw / (v.dh * aspect)) / 2;

        RL_SetColor(RL_Rgb(255, 255, 255));
        uii.Rend_DrawPicStretched(RL_P(v, (v.dw - pw) / 2), 0, RL_P(v, (v.dw + pw) / 2) - RL_P(v, (v.dw - pw) / 2), RL_P(v, v.dh), cut, 0,
                                  1.0f - cut, 1, painting);
    } else {
        RL_StockPictures(rl_end.stock, RL_P(v, lx), 0, RL_P(v, lx + lw), RL_P(v, v.dh), true);
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0, 120));
    // the header: the level, the mission; the outcome a tag the buttons' height and
    // shape (CONTINUE's), as far in from the panel's top, bottom and right as the title from its left
    {
        const char *tag = rl_end.ok ? "SUCCESS" : "FAILED";
        const float cw  = RL_TextW(v, tag, 18, true, 2) + 56;

        RL_HeaderPanel(v, x0, y, gw, head, level[0] ? level : in.map, RL_H3, RL_MissionLine(in.map).c_str(), NULL, 1.0f,
                       gw - 56 - cw - 16);
        RL_Box(v, x0 + gw - 28 - cw, y + (head - bh) / 2, cw, bh, rl_end.ok ? RL_BRASS : RL_Rgb(214, 104, 88));
        RL_Text(v, x0 + gw - 28 - cw / 2, y + head / 2, tag, 18, RL_INK, true, RL_CENTER, 2, RL_MID);
    }
    y += head + gap;
    // the numbers, three a row
    for (int k = 0; k < 6; k++) {
        const float x = x0 + (k % 3) * (tw + gap), ty = y + (k / 3) * (th + gap);

        RL_Round(v, x, ty, tw, th, 4, RL_Rgb(14, 16, 18, 226));
        RL_Text(v, x + tw / 2, ty + th / 2 - 12, numbers[k][0].c_str(), 44, RL_TEXT, true, RL_CENTER, 0, RL_MID);
        RL_Text(v, x + tw / 2, ty + th - 22, numbers[k][1].c_str(), RL_S2, RL_MUTED, true, RL_CENTER, 1.6f, RL_MID);
    }
    y += 2 * th + 2 * gap;
    // the weapon most hits came from (none without a hit), where they landed
    {
        const float hx = x0 + tw + gap, hw = 2 * tw + gap, top = y + 50, rh = (wh - 66) / 5;
        const float bx0 = hx + 96, bx1 = hx + hw - 76;

        RL_Round(v, x0, y, tw, wh, 4, RL_Rgb(14, 16, 18, 226));
        if (!outline.empty()) {
            RL_Outline(v, outline.c_str(), x0 + tw / 2, y + wh * 0.36f, tw * 0.74f, wh * 0.34f, RL_Rgb(200, 196, 186));
        }
        RL_Text(v, x0 + tw / 2, y + wh - 52, Q_stricmp(weapon.c_str(), "none") ? RL_Upper(weapon.c_str()).c_str() : "\x97", RL_S1, RL_TEXT, true,
                RL_CENTER, 1.6f, RL_MID, tw - 20);
        RL_Text(v, x0 + tw / 2, y + wh - 22, "PREFERRED WEAPON", RL_S2, RL_MUTED, true, RL_CENTER, 1.6f, RL_MID);
        RL_Round(v, hx, y, hw, wh, 4, RL_Rgb(14, 16, 18, 226));
        RL_Text(v, hx + 20, y + 24, "WHERE YOUR HITS LANDED", RL_S2, RL_MUTED, true, RL_LEFT, 1.6f, RL_MID);
        for (int k = 0; k < 5; k++) {
            const float cy = top + rh * (k + 0.5f), f = Q_max(0.0f, Q_min(hits[k], 100.0f)) / 100;

            RL_Text(v, hx + 20, cy, hitNames[k], 15, RL_Rgb(200, 196, 186), true, RL_LEFT, 1.6f, RL_MID);
            RL_Box(v, bx0, cy - 2, bx1 - bx0, 4, RL_Rgb(255, 255, 255, 30));
            RL_Box(v, bx0, cy - 2, (bx1 - bx0) * f, 4, RL_BRASS);
            RL_Text(v, hx + hw - 20, cy, va("%d%%", (int)(hits[k] + 0.5f)), 17, RL_TEXT, true, RL_RIGHT, 0, RL_MID);
        }
    }
    y += wh;
    // the medal won: its picture, its name
    if (medal >= 0) {
        const qhandle_t pic = re.RegisterShaderNoMip(rl_medals[medal].pic);
        const float     ph  = mh - 20;
        const float     pw  = pic ? ph * uii.Rend_GetShaderWidth(pic) / Q_max(1, uii.Rend_GetShaderHeight(pic)) : 0.0f;

        y += gap;
        RL_Round(v, x0, y, gw, mh, 4, RL_Rgb(14, 16, 18, 226));
        if (pic && !rl_bakeOnly) {
            RL_SetColor(RL_Rgb(255, 255, 255));
            uii.Rend_DrawPicStretched(RL_P(v, x0 + 14), RL_P(v, y + 10), RL_P(v, x0 + 14 + pw) - RL_P(v, x0 + 14), RL_P(v, y + 10 + ph) - RL_P(v, y + 10),
                                      0, 0, 1, 1, pic);
        }
        RL_RoundFrame(v, x0 + 14, y + 10, pw, ph, 0, 1, RL_Rgb(255, 255, 255, 40));
        RL_Text(v, x0 + 30 + pw, y + mh / 2 - 12, "MEDAL AWARDED", RL_S2, RL_BRASS, true, RL_LEFT, 2.6f, RL_MID);
        RL_Text(v, x0 + 30 + pw, y + mh / 2 + 13, rl_medals[medal].name, RL_S1, RL_TEXT, true, RL_LEFT, 0, RL_MID);
        y += mh;
    }
    y += 22;
    // MAIN MENU, CONTINUE (off, a brass bar filling under it, while the game waits)
    {
        const float cx = x0 + tw + gap, cw = 2 * tw + gap;

        RL_Button(this, v, x0, y, tw, bh, "MAIN MENU", false, ID_MAINMENU);
        RL_Button(this, v, cx, y, cw, bh, rl_end.ok ? "CONTINUE" : "TRY AGAIN", true, ID_CONTINUE, wait < 1.0f);
        if (wait < 1.0f) {
            RL_Box(v, cx, y + bh - 4, cw * wait, 4, RL_BRASS);
        }
        if (!rl_end.ok) {
            RL_Text(v, x0 + gw, y + bh + 24, *Cvar_VariableString("g_lastsave") ? "TRY AGAIN loads your last save." : "TRY AGAIN starts the level again.",
                    15, RL_MUTED, false, RL_RIGHT, 0, RL_MID);
        }
    }
    m_order.push_back(ID_MAINMENU);
    m_order.push_back(ID_CONTINUE);
    m_cols = 2;
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
            rl_wp.tab = id - ID_WTAB;
        } else if (id == ID_WPREV || id == ID_WNEXT) {
            // its next or previous model previewed (the figure follows)
            const int t = rl_wp.tab, nm = (int)rl_models[t].size();
            int       p = rl_wp.pick[t];

            if (nm) {
                p = p < 0 || p >= nm ? (id == ID_WNEXT ? 0 : nm - 1) : (p + (id == ID_WNEXT ? 1 : nm - 1)) % nm;
                rl_wp.pick[t] = p;
            }
        } else if (id >= 0 && id < WC_NUM) {
            // the lot sent: the models cycled to, the side previewed, the weapon
            const int team = cl.snap.valid ? cl.snap.ps.stats[STAT_TEAM] : TEAM_NONE;
            bool      models = false;

            sent = "popmenu 0\n";
            for (int t = 0; t < 2; t++) {
                const int p = rl_wp.pick[t];

                if (p >= 0 && p < (int)rl_models[t].size() && p != rl_wp.own[t]) {
                    Cvar_Set(t ? "dm_playergermanmodel" : "dm_playermodel", rl_models[t][p].file.c_str());
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
        case ID_QUIT:
            cmd = "quit\n";
            break;
        case ID_OPTIONS:
        case ID_VOTE:
            // Options or the vote's page over it (BACK and Esc back to it)
            RL_OverShow(id == ID_VOTE ? RL_OVER_VOTE : RL_OVER_OPTIONS);
            break;
        case ID_SPECTATOR:
            cmd = "popmenu 0;wait 250;spectator\n";
            break;
        case ID_DISCONNECT:
            // (the main menu next: a mission left on Campaign, a match on Multiplayer)
            RL_MainShow(RL_Gametype() == GT_SINGLE_PLAYER ? RL_SHOW_MISSIONS : RL_SHOW_PLAY);
            cmd = "disconnect;wait 5;popmenu 0;pushmenu main\n";
            break;
        case ID_RESUME:
            cmd = "popmenu 0\n";
            break;
        case ID_MAINMENU:
            if (RL_Gametype() == GT_SINGLE_PLAYER) {
                RL_MainShow(RL_SHOW_MISSIONS);
            }
            cmd = "popmenu 0;wait 250;pushmenu main\n";
            break;
        case ID_SAVE:
            // (back in the mission: the shot taken of it, not of the menu)
            cmd = "popmenu 0;savegame\n";
            break;
        case ID_LOAD:
            // Campaign's saved games
            RL_MainShow(RL_SHOW_SAVES);
            cmd = "popmenu 0;wait 250;pushmenu main\n";
            break;
        }
        break;
    case SCR_END:
        if (id == ID_MAINMENU) {
            // (over it: it is there again when the main menu closes)
            RL_MainShow(RL_SHOW_MISSIONS);
            cmd = "pushmenu main\n";
        } else if (id == ID_CONTINUE && cls.realtime - rl_end.at >= RL_ENDWAIT) {
            // the fire the game waits for (down and up at once: still a press in the next command)
            cmd = "+attackprimary\n-attackprimary\n";
        }
        break;
    }
    if (cmd) {
        Cbuf_AddText(cmd);
    }
}

bool RL_EscClick(int x, int y)
{
    Menu *const     esc = menuManager.FindMenu("reloaded_esc");
    UIWidget *const w   = esc && esc->GetContainerWidget() ? esc->GetContainerWidget()->FindWidget("front") : NULL;
    const int       id  = w && w->isSubclassOf(UIReloadedMenu) ? static_cast<UIReloadedMenu *>(w)->HitAt(x, y) : -1;

    if (id < 0) {
        return false;
    }
    // (the page's item, lit: the page closed; the other page's: shown in its place; another: chosen, the page closed first)
    const int lit = RL_OverPage() == RL_OVER_VOTE ? ID_VOTE : ID_OPTIONS;

    if (id == lit || (id != ID_OPTIONS && id != ID_VOTE)) {
        Cbuf_AddText("popmenu 0\n");
    }
    if (id != lit) {
        static_cast<UIReloadedMenu *>(w)->Choose(id);
    }
    return true;
}

/*
================
The scoreboard: CS2's structure in our look. A header band (the map and
mode, ALLIES 4 : 3 AXIS, the time left), a section per team, the leading
one first (its band: emblem, name, players, alive in round modes, the
columns), a row per player (name, K, D, K/D, TIME, PING; from round-based
on the stock board's name, KILLS, TOTAL, TIME, PING), the spectators under
it. Rows from the player configstrings, filled from the latest
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

static void RL_SbHeader(const rlView_t& v, float x, float y, float w, float h, const rlInfo_t& in, const std::vector<rlRow_t>& ffaRows)
{
    const float k = h / 104;
    const char *left = RL_TimeLeft(in.gametype);
    const float cx = x + w / 2;

    // (the title and caption left of the score in the middle)
    RL_HeaderPanel(v, x, y, w, h, RL_MapName(in.map), RL_H3, RL_ModeName(in.gametype), left, k, w / 2 - 260 * k - 28);
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

// an objective map's objectives, each side's as one line (the server's g_obj_alliedtext1-5, g_obj_axistext1-5:
// the stock scoreboard's narrow lines joined again, a "-" starting each objective, the objectives dot-separated)
static void RL_MapObjectives(std::string objs[2])
{
    static const char *const names[2] = {"g_obj_alliedtext%d", "g_obj_axistext%d"};
    const char              *info     = CL_ConfigString(CS_SERVERINFO);

    for (int t = 0; t < 2; t++) {
        objs[t].clear();
        for (int i = 1; i <= 5; i++) {
            std::string piece = Info_ValueForKey(info, va(names[t], i));
            bool        next;

            piece.erase(0, piece.find_first_not_of(' '));
            next = !piece.empty() && piece[0] == '-';
            piece.erase(0, next ? piece.find_first_not_of(' ', 1) : 0);
            piece.erase(piece.find_last_not_of(' ') + 1);
            if (!piece.empty()) {
                objs[t] += objs[t].empty() ? piece : (next ? "  \xb7  " : " ") + piece;
            }
        }
    }
}

// the numeric columns from the right, each label and its numbers centered in
// its width
typedef struct {
    const char *label;
    float       w;
} rlCol_t;

static const rlCol_t rl_cols[] = {
    {"K",    58},
    {"D",    58},
    {"K/D",  72},
    {"TIME", 88},
    {"PING", 78},
};

// from round-based on the server counts no deaths: a row's numbers are the
// player's kills this round and in the match (fgame's DM_Team::AddKills and
// its sort), the stock board's "Kills" and "Total"
static const rlCol_t rl_colsTotals[] = {
    {"KILLS", 72},
    {"TOTAL", 84},
    {"TIME",  88},
    {"PING",  78},
};

static const rlCol_t *RL_Cols(bool totals, int *n)
{
    *n = totals ? (int)ARRAY_LEN(rl_colsTotals) : (int)ARRAY_LEN(rl_cols);
    return totals ? rl_colsTotals : rl_cols;
}

static float RL_ColsW(bool totals)
{
    int            n;
    const rlCol_t *cols = RL_Cols(totals, &n);
    float          w    = 0;

    for (int i = 0; i < n; i++) {
        w += cols[i].w;
    }
    return w;
}

static void RL_SbLabels(const rlView_t& v, float x, float y, float w, rlColor_t c, bool totals)
{
    int            n;
    const rlCol_t *cols = RL_Cols(totals, &n);
    float          r    = x + w - 16;

    for (int i = n - 1; i >= 0; i--) {
        RL_Text(v, r - cols[i].w / 2, y, cols[i].label, RL_S2, c, true, RL_CENTER, 1.5f, RL_MID);
        r -= cols[i].w;
    }
}

static void RL_SbRow(const rlView_t& v, float x, float y, float w, float rh, const rlRow_t& r, int i, int rank, bool rounds, bool totals, int self)
{
    const rlTeam_t& T    = rl_teams[r.side];
    const bool      me   = r.p->client == self;
    const bool      dead = rounds && r.sc && r.sc->dead;
    const int       a    = dead ? 110 : 255;
    // (the player's own: the accent's lighter shade, dimmed less while dead, readable on the team's colour in each game)
    const rlColor_t col  = me ? RL_Alpha(RL_BRASS_HOT, dead ? 150 : 255) : dead ? RL_Rgb(150, 146, 136, a) : RL_TEXT;
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
        const float nameW = x + w - 16 - RL_ColsW(totals) - 14 - nx;
        std::string name  = RL_Ellipsize(v, r.p->name, fs, me, nameW);

        RL_Text(v, nx, cy, name.c_str(), fs, col, me, RL_LEFT, 0, RL_MID);
    }
    {
        int            n;
        const rlCol_t *cols = RL_Cols(totals, &n);
        const char    *vals[5];
        float          rx = x + w - 16;
        char           k[16], d[16], ping[16], kd[16];
        const float    ratio = !r.sc ? 0.0f : r.sc->deaths ? (float)r.sc->kills / r.sc->deaths : (float)r.sc->kills;

        Q_strncpyz(ping, r.sc ? (!Q_stricmp(r.sc->ping, "bot") ? "BOT" : r.sc->ping) : "\x96", sizeof(ping));
        if (totals) {
            // (this round's kills, the match's)
            Com_sprintf(k, sizeof(k), "%d", r.sc ? r.sc->kills : 0);
            Com_sprintf(d, sizeof(d), "%d", r.sc ? r.sc->deaths : 0);
            vals[0] = r.sc ? k : "\x96";
            vals[1] = r.sc ? d : "\x96";
            vals[2] = r.sc ? r.sc->time : "\x96";
            vals[3] = ping;
        } else {
            Com_sprintf(k, sizeof(k), "%d", r.sc ? r.sc->kills : 0);
            Com_sprintf(d, sizeof(d), "%d", r.sc ? r.sc->deaths : 0);
            Com_sprintf(kd, sizeof(kd), "%.2f", ratio);
            vals[0] = r.sc ? k : "\x96";
            vals[1] = r.sc ? d : "\x96";
            vals[2] = r.sc ? kd : "\x96";
            vals[3] = r.sc ? r.sc->time : "\x96";
            vals[4] = ping;
        }
        for (int c = n - 1; c >= 0; c--) {
            rlColor_t vc = col;

            if (!totals && c == 2 && r.sc && !dead && !me) {
                vc = ratio >= 1.0f ? RL_Rgb(150, 205, 140) : RL_Rgb(220, 140, 120);
            }
            // (the time and the ping)
            if (c >= n - 2 && !me) {
                vc = RL_Alpha(RL_MUTED, a);
            }
            RL_Text(v, rx - cols[c].w / 2, cy, vals[c], fs, vc, c == 0, RL_CENTER, 0, RL_MID);
            rx -= cols[c].w;
        }
    }
}

// a team's section: its band, then its players; over most, its lowest-ranked fold into "+ N more"; its height
static float RL_SbTeam(const rlView_t& v, float x, float y, float w, int t, const std::vector<rlRow_t>& rows, bool rounds, bool totals, float rh, int most,
                       int self, const std::string& objective)
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
        const float iw = RL_TextW(v, T.name, RL_S1, true, 2), end = x + 52 + iw + 16 + RL_TextW(v, info, RL_S2, false, 2);
        const float cols = x + w - 16 - RL_ColsW(totals) - 12;

        if (end <= cols) {
            RL_Text(v, x + 52 + iw + 16, y + bh / 2, info, RL_S2, RL_T(T.pale, 220), false, RL_LEFT, 2, RL_MID);
        }
        // an objective game's: the side's objectives after a hairline, the two sides' level when they fit
        if (!objective.empty() && end <= cols) {
            const float ox = Q_max(x + 320, end + 24);

            RL_Box(v, ox, y + 12, 1, bh - 24, RL_Rgb(255, 255, 255, 64));
            RL_Text(v, ox + 18, y + bh / 2, objective.c_str(), 18, RL_T(T.pale), false, RL_LEFT, 0, RL_MID, cols - ox - 18);
        }
    }
    RL_SbLabels(v, x, y + bh / 2, w, RL_T(T.pale, 200), totals);
    for (int i = 0; i < shown; i++) {
        RL_SbRow(v, x, y + bh + 4 + i * rh, w, rh, rows[i], i, 0, rounds, totals, self);
    }
    if (shown < n) {
        RL_Text(v, x + 14, y + bh + 4 + shown * rh + rh / 2 - 1, va("+ %d more", n - shown), Q_min(RL_S1, rh * 0.66f), RL_FAINT, false, RL_LEFT, 0,
                RL_MID);
        return bh + 4 + (shown + 1) * rh;
    }
    return bh + 4 + n * rh;
}

// Tug of War's objectives (the map's g_obj_alliedtext1-5, g_obj_axistext1-5, one each), the sides side by side
// under the header: a band each (emblem, ALLIED or AXIS OBJECTIVES, how many held), a row each, its box ticked while the side
// holds it (the "scores" states, rl_sb.tow). RL_TowObjectives: the rows, 0 without objectives
#define RL_TOWBAND 38.0f
#define RL_TOWROW  30.0f

static int RL_TowObjectives(std::string text[2][5])
{
    static const char *const names[2] = {"g_obj_alliedtext%d", "g_obj_axistext%d"};
    const char              *info     = CL_ConfigString(CS_SERVERINFO);
    int                      n        = 0;

    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 5; i++) {
            text[t][i] = Info_ValueForKey(info, va(names[t], i + 1));
            text[t][i].erase(0, text[t][i].find_first_not_of(' '));
            text[t][i].erase(text[t][i].find_last_not_of(' ') + 1);
            if (!text[t][i].empty()) {
                n = Q_max(n, i + 1);
            }
        }
    }
    return n;
}

static void RL_SbTow(const rlView_t& v, float x, float y, float w, const std::string text[2][5], int n)
{
    const float cw = (w - 12) / 2;

    for (int t = 0; t < 2; t++) {
        const rlTeam_t& T  = rl_teams[t];
        const float     cx = x + t * (cw + 12);
        int             held = 0, all = 0;

        for (int i = 0; i < n; i++) {
            all += !text[t][i].empty();
            held += !text[t][i].empty() && rl_sb.tow[t][i];
        }
        RL_HGradient(v, cx, y, cw, RL_TOWBAND, RL_T(T.mid, 235), RL_T(T.base, 235));
        RL_TeamIcon(v, t, cx + 24, y + RL_TOWBAND / 2, 24);
        RL_Text(v, cx + 44, y + RL_TOWBAND / 2, t ? "AXIS OBJECTIVES" : "ALLIED OBJECTIVES", RL_S2, RL_TEXT, true, RL_LEFT, 2, RL_MID);
        RL_Text(v, cx + cw - 16, y + RL_TOWBAND / 2, va("%d / %d HELD", held, all), RL_S2, RL_T(T.pale, 220), false, RL_RIGHT, 2, RL_MID);
        for (int i = 0; i < n; i++) {
            const float     ry  = y + RL_TOWBAND + 4 + i * RL_TOWROW, cy = ry + RL_TOWROW / 2 - 1;
            const bool      on  = rl_sb.tow[t][i] != 0;
            const rlColor_t row = RL_Rgb(22, 24, 26, 232);

            // (every other row a little lighter, as the scoreboard's)
            RL_Box(v, cx, ry, cw, RL_TOWROW - 2, i % 2 == 0 ? RL_Over(RL_Rgb(255, 255, 255, 9), row) : row);
            if (text[t][i].empty()) {
                continue;
            }
            if (on) {
                RL_Box(v, cx + 14, cy - 9, 18, 18, RL_T(T.light));
                RL_Outline(v, RL_OUTLINES "glyphs/check.svg", cx + 23, cy, 13, 13, RL_INK);
            } else {
                RL_RoundFrame(v, cx + 14, cy - 9, 18, 18, 2, 2, RL_Rgb(120, 117, 110));
            }
            RL_Text(v, cx + 46, cy, text[t][i].c_str(), 18, on ? RL_TEXT : RL_MUTED, false, RL_LEFT, 0, RL_MID, cw - 60);
        }
    }
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
        RL_SbLabels(v, x, y0 + 20, w, RL_MUTED, false);
        for (int j = 0; j < shown; j++) {
            RL_SbRow(v, x, y0 + 44 + j * rh, w, rh, ffaRows[j], j, j + 1, false, false, in.self);
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
        const float banner = UI_Extras_BannerHeight(), over = banner > 0.0f ? banner + 16.0f : 0.0f;
        std::string tow[2][5];
        const int   towN = in.gametype == GT_TOW ? RL_TowObjectives(tow) : 0;
        const float towH = towN ? RL_TOWBAND + 4 + towN * RL_TOWROW + 14 : 0.0f;
        const float base = availAll - 2 * band - gapT - over - towH;
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
        const float used     = over + hdrH + 14 + towH + 2 * band + gapT + rowsUsed * rh + specH;
        const float top      = Q_max(topMin, (v.dh - used) / 2);
        float       y        = top + over + hdrH + 14;

        if (banner > 0.0f) {
            UI_Extras_DrawBanner(v, top);
        }
        RL_SbHeader(v, x, top + over, w, hdrH, in, ffaRows);
        if (towN) {
            RL_SbTow(v, x, y, w, tow, towN);
            y += towH;
        }
        // (an objective game's objectives in the bands: the other modes' maps may set the same lines)
        std::string objs[2];
        if (in.gametype == GT_OBJECTIVE) {
            RL_MapObjectives(objs);
        }
        for (int k = 0; k < 2; k++) {
            y += RL_SbTeam(v, x, y, w, order[k], teams[order[k]], rounds, in.gametype >= GT_TEAM_ROUNDS, rh, caps[k], in.self, objs[order[k]]) + gapT;
        }
        spectators(y + 4);
    }
}

/*
================
UI_Menus_DrawObjectives

A mission's objectives while TAB is held (the cgame's call), as the
scoreboard's card: its header (the level, the mission's line, how many
are done) over a row each, the box ticked when done, the current one
marked in brass
================
*/
void UI_Menus_DrawObjectives(qboolean shown)
{
    struct rlObj_t {
        std::string text;
        bool        done, now;
    };
    std::vector<rlObj_t> objs;
    int                  done = 0, current;
    char                 map[MAX_QPATH];
    const char          *level;

    if (!shown || !uii.IsRendererLoaded()) {
        return;
    }
    current = atoi(CL_ConfigString(CS_CURRENT_OBJECTIVE));
    for (int i = 0; i < MAX_OBJECTIVES; i++) {
        const char *cs    = CL_ConfigString(CS_OBJECTIVES + i);
        const int   flags = atoi(Info_ValueForKey(cs, "flags"));
        std::string text;

        if (flags == OBJ_FLAG_NONE || (flags & OBJ_FLAG_HIDDEN)) {
            continue;
        }
        text = Info_ValueForKey(cs, "text");
        text = Sys_LV_CL_ConvertString(text.c_str());
        std::replace(text.begin(), text.end(), '\n', ' ');
        objs.push_back({text, (flags & OBJ_FLAG_COMPLETED) != 0, i == current && !(flags & OBJ_FLAG_COMPLETED)});
        done += objs.back().done;
    }
    RL_MapFile(map, Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "mapname"));
    level = CL_ConfigString(CS_MESSAGE);
    {
        const rlView_t v = RL_View();
        const float    w = Q_min(900.0f, v.dw - 64), x = (v.dw - w) / 2, head = 104, band = 40, rh = 52;
        const float    y0 = Q_max(48.0f, (v.dh - (head + 14 + band + 4 + objs.size() * rh)) / 2);
        const float    count = objs.empty() ? 0.0f : RL_TextW(v, "OBJECTIVES DONE", RL_S2, false, 2.4f) + 28;
        float          y = y0 + head + 14;

        RL_HeaderPanel(v, x, y0, w, head, level[0] ? level : map, RL_H3, RL_MissionLine(map).c_str(), NULL, 1.0f, w - 56 - count);
        RL_Box(v, x, y0 + head - 4, w, 4, RL_BRASS);
        if (objs.empty()) {
            return;
        }
        RL_Text(v, x + w - 28, y0 + 32, va("%d / %d", done, (int)objs.size()), RL_H3, RL_TEXT, true, RL_RIGHT, 0, RL_MID);
        RL_Text(v, x + w - 28, y0 + 68, "OBJECTIVES DONE", RL_S2, RL_MUTED, false, RL_RIGHT, 2.4f, RL_MID);
        RL_Round(v, x, y, w, band, 4, RL_Rgb(30, 33, 36, 235));
        RL_Text(v, x + 14, y + band / 2, "MISSION OBJECTIVES", RL_S2, RL_MUTED, true, RL_LEFT, 1.5f, RL_MID);
        y += band + 4;
        for (size_t i = 0; i < objs.size(); i++, y += rh) {
            const rlObj_t&  o   = objs[i];
            const float     cy  = y + rh / 2 - 1;
            const rlColor_t row = RL_Rgb(22, 24, 26, 232);

            // (every other row a little lighter, as the scoreboard's)
            RL_Box(v, x, y, w, rh - 2, i % 2 == 0 ? RL_Over(RL_Rgb(255, 255, 255, 9), row) : row);
            if (o.now) {
                RL_Box(v, x, y, 4, rh - 2, RL_BRASS);
            }
            if (o.done) {
                RL_Box(v, x + 22, cy - 10, 20, 20, RL_BRASS);
                RL_Outline(v, RL_OUTLINES "glyphs/check.svg", x + 32, cy, 14, 14, RL_INK);
            } else {
                RL_RoundFrame(v, x + 22, cy - 10, 20, 20, 2, 2, o.now ? RL_BRASS : RL_Rgb(120, 117, 110));
            }
            RL_Text(v, x + 58, cy, o.text.c_str(), RL_S1, o.done ? RL_MUTED : o.now ? RL_TEXT : RL_Rgb(200, 196, 186), false, RL_LEFT, 0,
                    RL_MID, w - 80);
        }
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
    static const char *const menus[] = {"reloaded_weapon", "reloaded_esc", "reloaded_options", "reloaded_scoreboard"};

    if (clc.state < CA_PRIMED || !uii.IsRendererLoaded()) {
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
