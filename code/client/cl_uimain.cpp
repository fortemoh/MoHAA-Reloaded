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

// cl_uimain.cpp: the Reloaded main menu (Added), ui/reloaded_main.urc, shown
// for the stock "main" (cl_uimenus.cpp's aliases), and its confirmations
// (Quit, Disconnect), ui/reloaded_confirm.urc, shown over it. Project: Omaha's
// screens, stripped down, in our look: a side rail (the star and the
// wordmark, PLAY / HOST / OPTIONS, QUIT at its foot) over maps' photos
// blurred once to the screen's shape (no map loaded behind it); PLAY is
// Omaha's server browser (the game's master server lists, queried only
// while the menu is up) and its profile (the name, each side's model in a
// 3D view, the views being the menu's labels "allies" and "axis").
//
// Sizes are design units (cl_uidraw.h): a 1080-line screen, the width what
// the screen's shape gives; the screens beside the rail from x 480 (420
// under 1700 units across), 48 from the edges, 1640 across at most.

#include "client.h"
#include "cl_ui.h"
#include "cl_uiwidget.h"
#include "../uilib/uibake.h"
#include "../gamespy/goaceng.h"
#include "../gamespy/sv_gamespy.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

// the palette's panels (Omaha's surfaces, ours)
static const rlColor_t RL_PANEL = {10 / 255.0f, 11 / 255.0f, 12 / 255.0f, 218 / 255.0f};
static const rlColor_t RL_HEAD  = {0, 0, 0, 70 / 255.0f};
static const rlColor_t RL_LINE  = {1, 1, 1, 24 / 255.0f};
static const rlColor_t RL_SOFT  = {200 / 255.0f, 196 / 255.0f, 186 / 255.0f, 1};
static const rlColor_t RL_INK   = {8 / 255.0f, 9 / 255.0f, 10 / 255.0f, 1};
static const rlColor_t RL_SEL   = {222 / 255.0f, 178 / 255.0f, 80 / 255.0f, 44 / 255.0f};

#define RL_GLYPH(name) RL_OUTLINES "glyphs/" name ".svg"

// the lines things line up on: the screens 48 from the screen's edges, 16
// inside their panels, their foot a 64-unit row of controls 42 high
// (REFRESH, JOIN, the model pickers) whose middle the rail's foot shares;
// the rail's buttons from x 54 (the logo's left edge too), their labels from 72
#define RL_EDGE   48.0f
#define RL_PAD    16.0f
#define RL_FOOT   64.0f
#define RL_CTRL   42.0f
#define RL_RAILX  54.0f
#define RL_LABELX 72.0f

enum { PAGE_PLAY, PAGE_HOST, PAGE_OPTIONS };
enum { HF_TIME, HF_FRAG, HF_PLAYERS, HF_BOTS, HF_NAME, HF_NUM }; // Host's fields
enum { F_NONE, F_SEARCH, F_NAME, F_HOST };                       // the field typed in (F_HOST + Host's)

// the browser's gametype tabs (Omaha's) and sortable columns
enum { TAB_ALL, TAB_FFA, TAB_TDM, TAB_OBJ, TAB_ROUNDS, TAB_OTHER, TAB_NUM };
static const char *const rl_tabs[TAB_NUM] = {"ALL", "FFA", "TDM", "OBJECTIVE", "ROUNDS", "OTHER"};
enum { COL_NAME, COL_MAP, COL_PLAYERS, COL_MODE, COL_PING, COL_NUM };
static const char *const rl_colNames[COL_NUM] = {"SERVER NAME", "MAP", "PLAYERS", "GAMETYPE", "PING"};

// the items (the hit ids); from RL_QUIET on, a list's rows
enum {
    ID_PLAY,
    ID_HOST,
    ID_OPTIONS,
    ID_QUIT,
    ID_DISCONNECT,
    ID_TAB     = 10,
    ID_SORT    = ID_TAB + TAB_NUM,
    ID_REFRESH = ID_SORT + COL_NUM,
    ID_SEARCH,
    ID_JOIN,
    ID_NAME,
    ID_APPLY,
    ID_SELECT, // + the side
    ID_CANCEL = ID_SELECT + 2,
    ID_CONFIRM,
    ID_MODE,               // + Host's game type
    ID_FIELD = ID_MODE + 4, // + Host's field
    ID_LOCAL = ID_FIELD + HF_NUM,
    ID_ONLINE,
    ID_START,
    ID_ROW  = RL_QUIET, // + the row shown
    ID_FAV  = ID_ROW + 1000,
    ID_PICK = ID_FAV + 1000, // + the model
    ID_MAP  = ID_PICK + 1000, // + Host's map shown
};

/*
====================================================================

SERVERS

The game's master server lists (as the stock browser's, cl_uiserverlist.cpp:
Allied Assault's, or Breakthrough's and Spearhead's unless
dm_omit_spearhead), each server queried for its status; a row each that
answered. Shown through the tab and the search, sorted by a column, the
favourites (ui_favoriteservers: their addresses) first, other versions last

====================================================================
*/

typedef struct {
    unsigned int ip; // the query's address and port: the server's key
    int          queryPort;
    std::string  addr; // ip:port, connected to
    std::string  name, map, mode;
    int          players, maxPlayers, ping;
    int          tab;
    bool         otherVersion; // can't be joined
} rlServer_t;

static struct {
    GServerList             list[2];
    std::vector<rlServer_t> rows;
    int                     serial; // changes with the rows and the lists' states
    bool                    failed; // a master couldn't be reached
} rl_sv;

static struct {
    int         page;
    int         edit; // F_*
    std::string search, name;
    int         tab, sort;
    bool        asc;
    std::string sel;  // the server picked: its address
    int         top;  // the first row shown
    int         fit;  // the rows the list shows (from the layout)
    int         pick; // the side whose model list is open, -1 none
    int         pickTop;
    std::string note; // a word on the status row in place of the lists' state
    int         confirm; // what the confirmation asks: ID_QUIT or ID_DISCONNECT
    int         clickAt;
    int         serial; // changes with all of the above
} rl_mm = {PAGE_PLAY, F_NONE, "", "", TAB_ALL, COL_PING, true, "", 0, 1, -1, 0, "", ID_QUIT, 0, 0};

static std::set<std::string> rl_favs;
static cvar_t               *ui_favoriteservers;

// the rows shown, in their order (rows' indices); made again when dirty
static struct {
    std::vector<int> rows;
    int              players;
    bool             dirty;
} rl_vis;

static void RL_Changed(void)
{
    rl_mm.serial++;
}

static const char *RL_SvString(GServer s, const char *key)
{
    return ServerGetStringValue(s, (char *)key, (char *)"");
}

static int RL_SvInt(GServer s, const char *key, int def)
{
    return ServerGetIntValue(s, (char *)key, def);
}

// a map's display name: its loading screen's (the dm and obj maps' are named with their folder)
static const std::string& RL_SvMapName(const char *map)
{
    static std::map<std::string, std::string> names;
    auto                                       it = names.find(map);

    if (it == names.end()) {
        std::string file = map;

        if (!strchr(map, '/')) {
            for (const char *dir : {"dm/", "obj/"}) {
                if (menuManager.FindMenu(va("%s%s", dir, map))) {
                    file = std::string(dir) + map;
                    break;
                }
            }
        }
        it = names.emplace(map, RL_MapName(file.c_str())).first;
    }
    return it->second;
}

// the tab a server shows under: its g_gametype (OpenMoHAA's gametype_i), else its mode's name
static int RL_SvTab(GServer s)
{
    switch (RL_SvInt(s, "gametype_i", -1)) {
    case GT_FFA:
        return TAB_FFA;
    case GT_TEAM:
        return TAB_TDM;
    case GT_TEAM_ROUNDS:
        return TAB_ROUNDS;
    case GT_OBJECTIVE:
        return TAB_OBJ;
    case -1:
        break;
    default:
        return TAB_OTHER;
    }
    std::string mode = RL_SvString(s, "gametype");

    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return (char)tolower(c); });
    return mode.find("free-for") != std::string::npos || mode == "ffa" ? TAB_FFA
         : mode.find("round") != std::string::npos                     ? TAB_ROUNDS
         : mode.find("obj") != std::string::npos                       ? TAB_OBJ
         : mode.find("team") != std::string::npos                      ? TAB_TDM
                                                                       : TAB_OTHER;
}

// a list's news (ServerListThink's): a server answered (its row made or
// updated) or failed, the list's state changed. instance: the game it lists
static void RL_SvCallback(GServerList list, int msg, void *instance, void *param1, void *param2)
{
    const int game = (int)(intptr_t)instance;
    GServer   s    = (GServer)param1;

    rl_sv.serial++;
    if (msg != LIST_PROGRESS || param2 == (void *)-1) {
        return;
    }
    const unsigned int ip = ServerGetInetAddress(s);
    const int          qp = ServerGetQueryPort(s);
    const char        *ver = RL_SvString(s, "gamever");
    float              v;
    rlServer_t        *row = NULL;

    for (rlServer_t& r : rl_sv.rows) {
        if (r.ip == ip && r.queryPort == qp) {
            row = &r;
            break;
        }
    }
    if (!row) {
        rl_sv.rows.push_back(rlServer_t());
        row            = &rl_sv.rows.back();
        row->ip        = ip;
        row->queryPort = qp;
    }
    row->addr       = va("%s:%i", ServerGetAddress(s), RL_SvInt(s, "hostport", PORT_SERVER));
    row->name       = RL_SvString(s, "hostname");
    // (without the spaces round it some names have)
    row->name.erase(0, row->name.find_first_not_of(' '));
    row->name.erase(row->name.find_last_not_of(' ') + 1);
    row->map        = RL_SvMapName(RL_SvString(s, "mapname"));
    row->mode       = RL_SvString(s, "gametype");
    row->players    = RL_SvInt(s, "numplayers", 0);
    row->maxPlayers = RL_SvInt(s, "maxplayers", 0);
    row->ping       = ServerGetPing(s);
    row->tab        = RL_SvTab(s);
    // (the stock browser's test: Breakthrough's and Spearhead's from 2.3 and
    // 2.1, Allied Assault's its own version; a demo's "d" skipped)
    v = (float)atof(ver[0] == 'd' ? ver + 1 : ver);
    row->otherVersion = game >= TG_MOHTA ? v < (game == TG_MOHTT ? 2.3f : 2.1f) : fabsf(v - com_target_shortversion->value) > 0.1f;
    rl_vis.dirty      = true;
}

static bool RL_SvBusy(void)
{
    for (GServerList l : rl_sv.list) {
        if (l && ServerListState(l) != sl_idle) {
            return true;
        }
    }
    return false;
}

// the lists asked of the masters again (not while they are)
static void RL_SvRefresh(void)
{
    const bool tt = com_target_game->integer >= TG_MOHTT;
    const int  games[2] = {tt ? TG_MOHTT : com_target_game->integer, tt && !Cvar_VariableIntegerValue("dm_omit_spearhead") ? TG_MOHTA : -1};
    char       filter[1] = "";

    if (RL_SvBusy()) {
        return;
    }
    rl_sv.rows.clear();
    rl_sv.failed = false;
    rl_vis.dirty = true;
    rl_mm.note.clear();
    for (int i = 0; i < 2; i++) {
        if (rl_sv.list[i]) {
            ServerListClear(rl_sv.list[i]);
            ServerListFree(rl_sv.list[i]);
            rl_sv.list[i] = NULL;
        }
        if (games[i] < 0) {
            continue;
        }
        const char *name = tt ? GS_GetGameName(games[i]) : GS_GetCurrentGameName();
        const char *key  = tt ? GS_GetGameKey(games[i]) : GS_GetCurrentGameKey();

        // (32 queries at once, shared by the lists)
        rl_sv.list[i] = ServerListNew(name, name, key, games[1] < 0 ? 32 : 16, (void *)&RL_SvCallback, GCALLBACK_FUNCTION,
                                      (void *)(intptr_t)games[i]);
        if (ServerListUpdate2(rl_sv.list[i], true, filter, qt_status) != GE_NOERROR) {
            rl_sv.failed = true;
        }
    }
    rl_sv.serial++;
}

static void RL_SvHalt(void)
{
    for (GServerList l : rl_sv.list) {
        if (l) {
            ServerListHalt(l);
        }
    }
}

// the rows shown: the tab's, the search's, sorted
static void RL_SvShown(void)
{
    std::string find = rl_mm.search;

    if (!rl_vis.dirty) {
        return;
    }
    rl_vis.dirty = false;
    rl_vis.rows.clear();
    rl_vis.players = 0;
    std::transform(find.begin(), find.end(), find.begin(), [](unsigned char c) { return (char)tolower(c); });
    for (int i = 0; i < (int)rl_sv.rows.size(); i++) {
        const rlServer_t& r = rl_sv.rows[i];

        if (rl_mm.tab != TAB_ALL && r.tab != rl_mm.tab) {
            continue;
        }
        if (!find.empty()) {
            std::string name = r.name;

            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
            if (name.find(find) == std::string::npos) {
                continue;
            }
        }
        rl_vis.rows.push_back(i);
        rl_vis.players += r.players;
    }
    std::sort(rl_vis.rows.begin(), rl_vis.rows.end(), [](int a, int b) {
        const rlServer_t &x = rl_sv.rows[a], &y = rl_sv.rows[b];
        const bool        fx = rl_favs.count(x.addr) > 0, fy = rl_favs.count(y.addr) > 0;
        int               c  = 0;

        if (fx != fy) {
            return fx;
        }
        if (x.otherVersion != y.otherVersion) {
            return y.otherVersion;
        }
        switch (rl_mm.sort) {
        case COL_NAME:
            c = Q_stricmp(x.name.c_str(), y.name.c_str());
            break;
        case COL_MAP:
            c = Q_stricmp(x.map.c_str(), y.map.c_str());
            break;
        case COL_PLAYERS:
            c = x.players - y.players;
            break;
        case COL_MODE:
            c = Q_stricmp(x.mode.c_str(), y.mode.c_str());
            break;
        default:
            c = x.ping - y.ping;
            break;
        }
        if (!c) {
            c = Q_stricmp(x.name.c_str(), y.name.c_str());
        }
        if (!c) {
            c = x.addr.compare(y.addr);
        }
        return rl_mm.asc ? c < 0 : c > 0;
    });
    rl_mm.top = Q_max(0, Q_min(rl_mm.top, (int)rl_vis.rows.size() - rl_mm.fit));
}

// the place shown of the server picked, -1 not shown
static int RL_SvPicked(void)
{
    for (int i = 0; i < (int)rl_vis.rows.size(); i++) {
        if (rl_sv.rows[rl_vis.rows[i]].addr == rl_mm.sel) {
            return i;
        }
    }
    return -1;
}

static void RL_SvFavorite(const std::string& addr)
{
    std::string list;

    if (!rl_favs.erase(addr)) {
        rl_favs.insert(addr);
    }
    for (const std::string& a : rl_favs) {
        list += (list.empty() ? "" : " ") + a;
    }
    Cvar_Set("ui_favoriteservers", list.c_str());
    rl_vis.dirty = true;
}

static void RL_SvJoin(void)
{
    const int at = RL_SvPicked();

    if (at < 0) {
        rl_mm.note = "PICK A SERVER";
        return;
    }
    const rlServer_t& r = rl_sv.rows[rl_vis.rows[at]];

    if (r.otherVersion) {
        rl_mm.note = "ANOTHER VERSION'S SERVER";
        return;
    }
    RL_SvHalt();
    // (back here should the server drop the player)
    UI_SetReturnMenuToCurrent();
    Cbuf_AddText(va("connect %s\n", r.addr.c_str()));
}

/*
====================================================================

HOST

Omaha's host panel: a map for the game type, as the stock host menu offers
them (cl_uimpmappicker.cpp: Allied Assault's dm maps for the deathmatch
types and its obj maps for objective; Spearhead's and Breakthrough's dm,
obj and lib maps but obj_team2 and obj_team4 for the deathmatch types,
the obj and lib maps named with an obj or ship part for objective), the
limits, the players and bots, the server's name, who can join. START sets
the stock host menu's cvars (as it left them, read the first time) and
runs its ui_startdmmap, the map the rotation's only one; the bots as
sv_maxbots and sv_numbots

====================================================================
*/

static const char *const rl_modeLabels[] = {"FFA", "TDM", "ROUNDS", "OBJECTIVE"};
// (g_gametype, g_gametypestring as the stock host menu names them)
static const int         rl_modeTypes[]  = {GT_FFA, GT_TEAM, GT_TEAM_ROUNDS, GT_OBJECTIVE};
static const char *const rl_modeNames[]  = {"Free-For-All", "Team-Match", "Round-Based-Match", "Objective-Based-Match"};
static const char *const rl_joinLabels[] = {"LOCAL", "ONLINE"};

static struct {
    bool                     ready;  // read from the cvars
    int                      mode;   // rl_mode*'s
    std::vector<std::string> maps;   // the mode's ("dm/mohdm6"), by the names shown
    std::string              map;    // picked
    int                      top;    // the first shown
    std::string              field[HF_NUM];
    bool                     online;
} rl_host;

// a map fits the mode (dir: its folder in maps/, name: its file's)
static bool RL_HostFits(const char *dir, const char *name, bool objective)
{
    if (com_target_game->integer < TG_MOHTA) {
        return !strcmp(dir, objective ? "obj" : "dm");
    }
    if (!objective) {
        return !strcmp(dir, "dm") || (Q_stricmp(name, "obj_team2") && Q_stricmp(name, "obj_team4"));
    }
    if (!strcmp(dir, "dm")) {
        return false;
    }
    // (a part between underscores obj or ship)
    for (const char *p = name; p; p = strchr(p, '_')) {
        p += *p == '_';
        if ((!Q_stricmpn(p, "obj", 3) && (!p[3] || p[3] == '_')) || (!Q_stricmpn(p, "ship", 4) && (!p[4] || p[4] == '_'))) {
            return true;
        }
    }
    return false;
}

// the mode's maps listed (those with a screen of their own, the stock ones,
// first, by the names shown); the one picked kept if it fits, else the first
static void RL_HostList(void)
{
    const bool objective = rl_modeTypes[rl_host.mode] == GT_OBJECTIVE;

    rl_host.maps.clear();
    for (const char *dir : {"dm", "obj", "lib"}) {
        int    n;
        char **files = FS_ListFiles(va("maps/%s", dir), ".bsp", qfalse, &n);

        for (int i = 0; i < n; i++) {
            std::string name = files[i];

            name.erase(name.size() - 4);
            if (RL_HostFits(dir, name.c_str(), objective)) {
                rl_host.maps.push_back(std::string(dir) + "/" + name);
            }
        }
        FS_FreeFileList(files);
    }
    std::sort(rl_host.maps.begin(), rl_host.maps.end(), [](const std::string& a, const std::string& b) {
        const std::string na = RL_MapName(a.c_str()), nb = RL_MapName(b.c_str());
        // (a map without a screen shows its file's name)
        const bool        sa = na != a.substr(a.find('/') + 1), sb = nb != b.substr(b.find('/') + 1);

        return sa != sb ? sa : Q_stricmp(na.c_str(), nb.c_str()) < 0;
    });
    if (std::find(rl_host.maps.begin(), rl_host.maps.end(), rl_host.map) == rl_host.maps.end()) {
        rl_host.map = rl_host.maps.empty() ? "" : rl_host.maps[0];
    }
    rl_host.top = 0;
}

// a character a server's name may have: printable, not one that ends its
// quoted string or the command, or expands in it
static bool RL_NameChar(int ch)
{
    return ch >= ' ' && ch <= '~' && !strchr("\";\\$%", ch);
}

// the first time Host shows: the settings the stock host menu left
static void RL_HostRead(void)
{
    std::string& name = rl_host.field[HF_NAME];

    const int gametype = atoi(CvarGetForUI("g_gametype", "2"));

    rl_host.ready  = true;
    rl_host.mode   = 1;
    for (int i = 0; i < (int)ARRAY_LEN(rl_modeTypes); i++) {
        if (rl_modeTypes[i] == gametype) {
            rl_host.mode = i;
        }
    }
    rl_host.map                 = CvarGetForUI("ui_dmmap", "");
    rl_host.field[HF_TIME]      = CvarGetForUI("ui_timelimit", "20");
    rl_host.field[HF_FRAG]      = CvarGetForUI("ui_fraglimit", "0");
    rl_host.field[HF_PLAYERS]   = CvarGetForUI("ui_maxclients", "16");
    rl_host.field[HF_BOTS]      = CvarGetForUI("sv_numbots", "0");
    name                        = CvarGetForUI("ui_hostname", "Reloaded Server");
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return !RL_NameChar((unsigned char)c); }), name.end());
    name.resize(Q_min(name.size(), (size_t)31));
    rl_host.online              = atoi(CvarGetForUI("ui_gamespy", "1")) != 0;
    RL_HostList();
}

static void RL_HostStart(void)
{
    const int players = Q_max(1, Q_min(MAX_CLIENTS, atoi(rl_host.field[HF_PLAYERS].c_str())));
    const int bots    = Q_max(0, Q_min(MAX_CLIENTS, atoi(rl_host.field[HF_BOTS].c_str())));

    if (rl_host.map.empty()) {
        return;
    }
    uii.Snd_PlaySound("sound/menu/apply.wav");
    // (the name typed without what ends a quoted string or a command)
    Cbuf_AddText(va("set ui_dmmap \"%s\";set ui_dedicated 0;set ui_maxclients %d;set ui_gamespy %d;set ui_timelimit %d;"
                    "set ui_fraglimit %d;set ui_hostname \"%s\";set sv_maxbots %d;set sv_numbots %d;ui_startdmmap %d %s ui_dmmap\n",
                    rl_host.map.c_str(), players, rl_host.online, Q_max(0, atoi(rl_host.field[HF_TIME].c_str())),
                    Q_max(0, atoi(rl_host.field[HF_FRAG].c_str())), rl_host.field[HF_NAME].c_str(), bots, bots,
                    rl_modeTypes[rl_host.mode], rl_modeNames[rl_host.mode]));
}

/*
----------------
The maps' photos (their stock load screens'), registered whole, one a
frame while Host shows; made again after a renderer's restart, looked up
again after a map's load
----------------
*/
static struct {
    std::map<std::string, qhandle_t> shot; // by map, 0 none
    int                              stamp, refSeq;
} rl_shots;

static std::string RL_ShotName(const std::string& map)
{
    std::string name = "*rl_shot_" + map;

    std::replace(name.begin(), name.end(), '/', '_');
    return name;
}

// a map's photo, 0 none, -1 not made yet
static qhandle_t RL_Shot(const std::string& map)
{
    if (rl_shots.refSeq != uii.GetRefSequence()) {
        rl_shots.shot.clear();
        rl_shots.refSeq = uii.GetRefSequence();
        rl_shots.stamp  = UI_BakeStamp();
    } else if (rl_shots.stamp != UI_BakeStamp()) {
        for (auto& s : rl_shots.shot) {
            s.second = s.second ? re.RegisterShaderNoMip(RL_ShotName(s.first).c_str()) : 0;
        }
        rl_shots.stamp = UI_BakeStamp();
    }
    auto it = rl_shots.shot.find(map);

    return it == rl_shots.shot.end() ? -1 : it->second;
}

static void RL_ShotMake(const std::string& map)
{
    int   w, h;
    byte *pic = RL_MenuPhoto(menuManager.FindMenu(map.c_str()), &w, &h);

    rl_shots.shot[map] = pic ? re.RegisterTexture2D(RL_ShotName(map).c_str(), pic, w, h) : 0;
    if (pic) {
        uii.Rend_FreeRawImage(pic);
    }
}

/*
====================================================================

DRAWING

====================================================================
*/

// a panel: dark, a line round it (square: rounded, a panel this size would be baked as large)
static void RL_Panel(const rlView_t& v, float x, float y, float w, float h, rlColor_t fill)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);

    RL_Box(v, x, y, w, h, fill);
    RL_BoxPx(x0, y0, x1, y0 + 1, RL_LINE);
    RL_BoxPx(x0, y1 - 1, x1, y1, RL_LINE);
    RL_BoxPx(x0, y0 + 1, x0 + 1, y1 - 1, RL_LINE);
    RL_BoxPx(x1 - 1, y0 + 1, x1, y1 - 1, RL_LINE);
}

// a panel's header band, h high, a line under it
static void RL_Head(const rlView_t& v, float x, float y, float w, float h)
{
    RL_Box(v, x + 1, y + 1, w - 2, h - 1, RL_HEAD);
    RL_Box(v, x, y + h, w, 1, RL_LINE);
}

// Omaha's slanted divider between tabs, at x
static void RL_Divider(const rlView_t& v, float x, float y, float h)
{
    const float m = h * 0.18f, k = 12 * (h - 2 * m) / h;

    RL_Skew(v, x - 1, y + m, 2 + k, h - 2 * m, k, k, RL_Rgb(255, 255, 255, 97));
}

static void RL_Button(const rlView_t& v, float x, float y, float w, float h, const char *label, bool primary, float kl, float kr, bool hot,
                      bool off = false)
{
    const rlColor_t fill = primary ? (hot ? RL_Rgb(236, 196, 106) : RL_BRASS) : RL_Rgb(255, 255, 255, hot ? 44 : 26);

    RL_Skew(v, x, y, w, h, kl, kr, off ? RL_Rgb(255, 255, 255, 14) : fill);
    RL_Text(v, x + w / 2 + (kl - kr) * 0.25f, y + h / 2, label, 18, off ? RL_FAINT : primary ? RL_INK : RL_TEXT, true, RL_CENTER, 2,
            RL_MID);
}

// a text field: the text (typed in: its end, should it not fit, and a caret; else cut), the placeholder when empty
static void RL_Field(const rlView_t& v, float x, float y, float w, float h, float kl, float kr, const std::string& text,
                     const char *placeholder, bool editing, bool hot)
{
    const float tx = x + 16 + kl * 0.5f, room = w - 32 - (kl + kr) * 0.5f;
    std::string shown = editing ? text : RL_Ellipsize(v, text.c_str(), 17, false, room);

    RL_Skew(v, x, y, w, h, kl, kr, RL_Rgb(255, 255, 255, editing ? 70 : hot ? 48 : 30));
    RL_Skew(v, x + 1, y + 1, w - 2, h - 2, kl, kr, RL_Rgb(0, 0, 0, 120));
    if (text.empty() && !editing) {
        RL_Text(v, tx, y + h / 2, placeholder, 17, RL_MUTED, false, RL_LEFT, 0, RL_MID, room);
        return;
    }
    while (editing && !shown.empty() && RL_TextW(v, shown.c_str(), 17, false) > room) {
        shown.erase(0, 1);
    }
    {
        const float tw = shown.empty() ? 0.0f : RL_Text(v, tx, y + h / 2, shown.c_str(), 17, RL_TEXT, false, RL_LEFT, 0, RL_MID);

        if (editing && (cls.realtime / 530) % 2 == 0) {
            RL_Box(v, tx + tw + 2, y + h / 2 - 11, 2, 22, RL_BRASS);
        }
    }
}

// a five-pointed star (the favourites') in a box d across
static void RL_Star(const rlView_t& v, float cx, float cy, float d, rlColor_t c)
{
    static std::string poly;
    const int          pd = RL_P(v, d);

    if (poly.empty()) {
        poly = "poly 1 1 1 1";
        for (int i = 0; i < 10; i++) {
            const float a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 0.21f : 0.5f;

            poly += va(" %.3f %.3f", 0.5f + r * cosf(a), 0.52f + r * sinf(a));
        }
    }
    RL_Shape(RL_P(v, cx) - pd / 2, RL_P(v, cy) - pd / 2, pd, pd, poly.c_str(), c, v.s);
}

// a small triangle, pointing down (or up), in w x h
static void RL_Arrow(const rlView_t& v, float x, float y, float w, float h, bool up, rlColor_t c)
{
    const int x0 = RL_P(v, x), y0 = RL_P(v, y);

    RL_Shape(x0, y0, RL_P(v, x + w) - x0, RL_P(v, y + h) - y0, up ? "poly 1 1 1 1 0.5 0 1 1 0 1" : "poly 1 1 1 1 0 0 1 0 0.5 1", c, v.s);
}

// a photo (shown 4:3, as the stock load screens show theirs) filling a box, cut to its shape
static void RL_Photo(const rlView_t& v, qhandle_t shot, float x, float y, float w, float h)
{
    const int   x0 = RL_P(v, x), y0 = RL_P(v, y), x1 = RL_P(v, x + w), y1 = RL_P(v, y + h);
    const float k  = (w / h) / (4.0f / 3.0f); // over 1: the photo's top and bottom cut
    const float cs = k < 1.0f ? (1.0f - k) / 2 : 0.0f, ct = k > 1.0f ? (1.0f - 1.0f / k) / 2 : 0.0f;

    if (shot <= 0) {
        RL_Box(v, x, y, w, h, RL_Rgb(22, 24, 26));
        return;
    }
    if (!rl_bakeOnly) {
        RL_SetColor(RL_Rgb(255, 255, 255));
        uii.Rend_DrawPicStretched(x0, y0, x1 - x0, y1 - y0, cs, ct, 1.0f - cs, 1.0f - ct, shot);
    }
}

// Omaha's segmented choice: a box a label, the one chosen brass, 40 high
static void RL_Segments(UIReloadedMenu *m, const rlView_t& v, float x, float y, const char *const *labels, int n, int on, int id)
{
    for (int i = 0; i < n; i++) {
        const float w = RL_TextW(v, labels[i], 15, true, 1.6f) + 36;
        const bool  hot = m->Hot(id + i);

        RL_Box(v, x, y, w, 40, i == on ? RL_BRASS : RL_Rgb(255, 255, 255, hot ? 36 : 18));
        RL_Text(v, x + w / 2, y + 20, labels[i], 15, i == on ? RL_INK : hot ? RL_TEXT : RL_SOFT, true, RL_CENTER, 1.6f, RL_MID);
        m->Hit(v, x, y, w, 40, id + i);
        x += w + 4;
    }
}

// a label over its control: the label's middle at y, the control 16 under it
static void RL_Label(const rlView_t& v, float x, float y, const char *label)
{
    RL_Text(v, x, y, label, 14, RL_MUTED, true, RL_LEFT, 1.6f, RL_MID);
}

/*
================
The background: the stock load screens' photos (dm and obj maps,
Stalingrad first), each 8 s, then a 2 s fade into the next. Each is
blurred once to the screen's shape, 54 texels high, its light 0.55 (the
load screen's 36 and 0.42: RL_Blur), into one of two textures: the one
shown, and the next, made while the one shown holds. Made again at a new
screen size or a renderer's restart, looked up again after a map's load;
the photo shown again from its start when the menu opens
================
*/
static const char *const rl_photos[] = {"dmloading/mohdm6", "dmloading/mohdm1", "objloading/objdm2", "dmloading/mohdm4",
                                        "objloading/objdm1", "dmloading/mohdm7", "dmloading/mohdm2", "objloading/objdm5",
                                        "dmloading/mohdm3", "objloading/objdm4", "dmloading/mohdm5"};

#define RL_HOLD 8000
#define RL_FADE 2000

static struct {
    qhandle_t shader[2]; // the slots' textures ("*rl_menublur0", "1"), 0 none
    int       made[2];   // the photo each holds, -1 none
    int       cur;       // the slot shown
    int       photo;     // the photo shown
    int       since;     // cls.realtime it began showing
    int       w, h;          // the screen's when made
    int       stamp, refSeq; // UI_BakeStamp and the renderer's sequence then
} rl_bg = {{0, 0}, {-1, -1}, 0, 0, 0, 0, 0, 0, 0};

// a slot holding a photo
static void RL_BgMake(int slot, int photo)
{
    byte *pic;
    int   pw, ph;

    if (rl_bg.made[slot] == photo) {
        return;
    }
    rl_bg.made[slot]   = photo;
    rl_bg.shader[slot] = 0;
    if (uii.Rend_LoadRawImage(va("textures/mohmenu/%s.tga", rl_photos[photo]), &pic, &pw, &ph)) {
        rl_bg.shader[slot] = RL_Blur(va("*rl_menublur%d", slot), pic, pw, ph, 54, 0.55f);
        uii.Rend_FreeRawImage(pic);
    }
}

// each frame the menu shows: the photos moved on, made
static void RL_BgTick(bool opened)
{
    const int n = (int)ARRAY_LEN(rl_photos);

    if (rl_bg.w != uid.vidWidth || rl_bg.h != uid.vidHeight || rl_bg.refSeq != uii.GetRefSequence()) {
        rl_bg.w       = uid.vidWidth;
        rl_bg.h       = uid.vidHeight;
        rl_bg.refSeq  = uii.GetRefSequence();
        rl_bg.stamp   = UI_BakeStamp();
        rl_bg.made[0] = rl_bg.made[1] = -1;
    } else if (rl_bg.stamp != UI_BakeStamp()) {
        for (int i = 0; i < 2; i++) {
            rl_bg.shader[i] = rl_bg.shader[i] ? re.RegisterShaderNoMip(va("*rl_menublur%d", i)) : 0;
        }
        rl_bg.stamp = UI_BakeStamp();
    }
    if (opened || cls.realtime - rl_bg.since >= 2 * (RL_HOLD + RL_FADE)) {
        rl_bg.since = cls.realtime;
    } else if (cls.realtime - rl_bg.since >= RL_HOLD + RL_FADE) {
        rl_bg.cur ^= 1;
        rl_bg.photo = (rl_bg.photo + 1) % n;
        rl_bg.since += RL_HOLD + RL_FADE;
    }
    RL_BgMake(rl_bg.cur, rl_bg.photo);
    RL_BgMake(!rl_bg.cur, (rl_bg.photo + 1) % n);
}

// how far the next photo has faded in, in 64ths (the back layer's draw key)
static int RL_BgFade(void)
{
    return Q_max(0, Q_min(64, (cls.realtime - rl_bg.since - RL_HOLD) * 64 / RL_FADE));
}

static void RL_Background(const rlView_t& v)
{
    const float t = RL_BgFade() / 64.0f;

    if (!rl_bg.shader[rl_bg.cur]) {
        RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(8, 9, 10));
    }
    if (rl_bakeOnly) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        const qhandle_t shader = rl_bg.shader[i ? !rl_bg.cur : rl_bg.cur];
        // (eased in and out)
        const float     a      = i ? t * t * (3 - 2 * t) : 1.0f;

        if (shader && a > 0.0f) {
            RL_SetColor(RL_Rgb(255, 255, 255, (int)(a * 255)));
            uii.Rend_DrawPicStretched(0, 0, uid.vidWidth, uid.vidHeight, 0, 0, 1, 1, shader);
        }
    }
}

/*
================
The rail: 440 units, dark, fading out past 400; the star and the wordmark
(92 high) at its head, level with the screens' top and the buttons' left
edge; PLAY / HOST / OPTIONS (the page shown filled brass); QUIT at its
foot, level with the screens' foot row, its label under the buttons' and
its icon ending at their edge; DISCONNECT over it while in a game (level
with the browser's status row)
================
*/
static const char *const rl_pages[] = {"PLAY", "HOST", "OPTIONS"};

static void RL_Rail(UIReloadedMenu *m, const rlView_t& v)
{
    const float qy = v.dh - RL_EDGE - RL_FOOT / 2;
    const bool  connected = clc.state > CA_PRIMED;

    RL_Box(v, 0, 0, 400, v.dh, RL_Rgb(10, 11, 12, 232));
    RL_HGradient(v, 400, 0, 240, v.dh, RL_Rgb(10, 11, 12, 232), RL_Rgb(10, 11, 12, 0));
    {
        // the lockup: the star 92 across, a gap of a fifth, the wordmark as high
        float sw, sh;

        RL_Outline(v, RL_GLYPH("star"), RL_RAILX + 46, RL_EDGE + 46, 92, 92, RL_Rgb(255, 255, 255));
        if (UI_OutlineSize(RL_GLYPH("wordmark"), &sw, &sh)) {
            const float ww = 92 * sw / sh;

            RL_Outline(v, RL_GLYPH("wordmark"), RL_RAILX + 92 * 1.2f + ww / 2, RL_EDGE + 46, ww, 92, RL_Rgb(255, 255, 255));
        }
    }
    for (int i = 0; i < 3; i++) {
        const float y = 380 + i * 78;
        const bool  on = rl_mm.page == i, hot = m->Hot(ID_PLAY + i);

        if (on || hot) {
            RL_Skew(v, RL_RAILX, y, 300, 64, 0, 14, on ? RL_BRASS : RL_Rgb(255, 255, 255, 18));
        }
        RL_Text(v, RL_LABELX, y + 32, rl_pages[i], 28, on ? RL_INK : hot ? RL_TEXT : RL_SOFT, true, RL_LEFT, 3, RL_MID);
        m->Hit(v, RL_RAILX, y, 300, 64, ID_PLAY + i);
    }
    for (int i = connected ? 0 : 1; i < 2; i++) {
        // (DISCONNECT over QUIT)
        const int       id  = i ? ID_QUIT : ID_DISCONNECT;
        const float     y   = qy - (1 - i) * 52;
        const rlColor_t c   = m->Hot(id) ? RL_TEXT : i ? RL_MUTED : RL_Rgb(214, 120, 100);

        RL_Outline(v, i ? RL_GLYPH("quit") : RL_GLYPH("disconnect"), RL_RAILX - 12, y, 24, 24, c);
        RL_Text(v, RL_LABELX, y, i ? "QUIT" : "DISCONNECT", 20, c, true, RL_LEFT, 2.4f, RL_MID);
        m->Hit(v, RL_RAILX - 24, y - 24, 260, 48, id);
    }
}

/*
================
PLAY: Omaha's server browser. The gametype tabs, the columns (the one
sorted by marked, clicked again for the other way), a row a server (a
star to keep it a favourite; picked: brass; joined with a double click
or JOIN), the status row (servers and players shown, the lists' state),
REFRESH, the search, JOIN. Narrower than 800 units (4:3), no GAMETYPE
================
*/
static void RL_Browser(UIReloadedMenu *m, const rlView_t& v, float x, float y, float w, float h)
{
    const bool  narrow = w < 800;
    // the columns' shares of the width, the star's first
    const float wide[COL_NUM + 1] = {0.05f, 0.42f, 0.19f, 0.11f, 0.14f, 0.09f}, small[COL_NUM + 1] = {0.06f, 0.46f, 0.25f, 0.12f, 0, 0.11f};
    const float *share = narrow ? small : wide;
    const float iw = w - 2 * RL_PAD, hy = y + 49, rh = 36, foot = 40 + RL_FOOT, ry = hy + 41;
    float       cx[COL_NUM + 1];

    RL_Panel(v, x, y, w, h, RL_PANEL);
    RL_Head(v, x, y, w, 48);
    {
        // the tabs
        const float size = narrow ? 15.0f : 17.0f, pad = narrow ? 12.0f : 20.0f;
        float       tx   = x + 4;

        for (int i = 0; i < TAB_NUM; i++) {
            const float tw = RL_TextW(v, rl_tabs[i], size, true, 2) + 2 * pad;
            const bool  on = rl_mm.tab == i, hot = m->Hot(ID_TAB + i);

            if (on || hot) {
                RL_Skew(v, tx, y, tw, 48, 12, 12, on ? RL_BRASS : RL_Rgb(255, 255, 255, 16));
            }
            RL_Text(v, tx + tw / 2, y + 24, rl_tabs[i], size, on ? RL_INK : hot ? RL_TEXT : RL_MUTED, true, RL_CENTER, 2, RL_MID);
            m->Hit(v, tx, y, tw, 48, ID_TAB + i);
            tx += tw;
            if (i < TAB_NUM - 1) {
                RL_Divider(v, tx - 6, y, 48);
            }
        }
    }
    // the columns' heads
    cx[0] = x + RL_PAD;
    for (int c = 1; c <= COL_NUM; c++) {
        cx[c] = cx[c - 1] + iw * share[c - 1];
    }
    RL_Box(v, x + 1, hy, w - 2, 40, RL_Rgb(0, 0, 0, 50));
    RL_Box(v, x, hy + 40, w, 1, RL_LINE);
    RL_Star(v, cx[0] + iw * 0.025f, hy + 20, 18, RL_Rgb(255, 255, 255, 90));
    for (int c = 0; c < COL_NUM; c++) {
        const bool hot = m->Hot(ID_SORT + c);

        if (!share[c + 1]) {
            continue;
        }
        const float lw = RL_Text(v, cx[c + 1], hy + 20, rl_colNames[c], 14, hot ? RL_TEXT : RL_MUTED, true, RL_LEFT, 1.6f, RL_MID);

        if (rl_mm.sort == c) {
            // (down: from the least, Omaha's)
            RL_Arrow(v, cx[c + 1] + lw + 8, hy + 16, 10, 8, !rl_mm.asc, RL_BRASS);
        }
        m->Hit(v, cx[c + 1] - 6, hy, iw * share[c + 1], 40, ID_SORT + c);
    }
    // the rows
    RL_SvShown();
    rl_mm.fit = Q_max(1, (int)((y + h - foot - ry) / rh));
    rl_mm.top = Q_max(0, Q_min(rl_mm.top, (int)rl_vis.rows.size() - rl_mm.fit));
    for (int i = 0; i < rl_mm.fit && rl_mm.top + i < (int)rl_vis.rows.size(); i++) {
        const int         at  = rl_mm.top + i;
        const rlServer_t& r   = rl_sv.rows[rl_vis.rows[at]];
        const bool        sel = r.addr == rl_mm.sel, fav = rl_favs.count(r.addr) > 0;
        const float       yy = ry + i * rh, mid = yy + rh / 2;
        const rlColor_t   soft = r.otherVersion ? RL_FAINT : RL_SOFT;

        if (sel) {
            RL_Box(v, x + 2, yy, w - 4, rh, RL_SEL);
            RL_Box(v, x + 2, yy, 3, rh, RL_BRASS);
        } else if (m->Hot(ID_ROW + i) || m->Hot(ID_FAV + i)) {
            RL_Box(v, x + 2, yy, w - 4, rh, RL_Rgb(255, 255, 255, 14));
        } else if (at % 2) {
            RL_Box(v, x + 2, yy, w - 4, rh, RL_Rgb(255, 255, 255, 6));
        }
        RL_Star(v, cx[0] + iw * 0.025f, mid, 18, fav ? RL_BRASS : m->Hot(ID_FAV + i) ? RL_Rgb(255, 255, 255, 120) : RL_Rgb(255, 255, 255, 40));
        RL_Text(v, cx[1], mid, RL_Ellipsize(v, r.name.empty() ? "?" : r.name.c_str(), 17, sel, iw * share[1] - 12).c_str(), 17,
                r.otherVersion ? RL_FAINT : RL_TEXT, sel, RL_LEFT, 0, RL_MID);
        RL_Text(v, cx[2], mid, RL_Ellipsize(v, r.map.c_str(), 17, false, iw * share[2] - 12).c_str(), 17, soft, false, RL_LEFT, 0, RL_MID);
        RL_Text(v, cx[3], mid, va("%d/%d", r.players, r.maxPlayers), 17, soft, false, RL_LEFT, 0, RL_MID);
        if (share[4]) {
            RL_Text(v, cx[4], mid, RL_Ellipsize(v, r.mode.c_str(), 17, false, iw * share[4] - 12).c_str(), 17, soft, false, RL_LEFT, 0, RL_MID);
        }
        RL_Text(v, cx[5], mid, va("%d", r.ping), 17,
                r.otherVersion ? RL_FAINT
                : r.ping < 60  ? RL_Rgb(150, 196, 120)
                : r.ping < 100 ? RL_Rgb(222, 178, 80)
                               : RL_Rgb(214, 120, 100),
                false, RL_LEFT, 0, RL_MID);
        m->Hit(v, x + 2, yy, w - 4, rh, ID_ROW + i);
        m->Hit(v, cx[0], yy, iw * share[0], rh, ID_FAV + i);
    }
    if (rl_vis.rows.empty()) {
        const char *empty = RL_SvBusy()            ? "Looking for servers\x85"
                          : rl_sv.failed           ? "The master server can't be reached."
                          : !rl_sv.rows.empty()    ? "No server matches."
                                                   : "No servers.";

        RL_Text(v, x + w / 2, ry + 3 * rh, empty, 17, RL_MUTED, false, RL_CENTER, 0, RL_MID);
    } else if ((int)rl_vis.rows.size() > rl_mm.fit) {
        // the scroll bar
        const float th = (y + h - foot - ry) - 8, n = (float)rl_vis.rows.size();

        RL_Box(v, x + w - 8, ry + 4 + th * rl_mm.top / n, 3, Q_max(24.0f, th * rl_mm.fit / n), RL_Rgb(255, 255, 255, 60));
    }
    {
        // the status row: the servers and players shown, the lists' state (or a word)
        const float sy = y + h - foot;
        const char *state;
        int         answered = 0, total = 0;

        for (GServerList l : rl_sv.list) {
            int ok, late;

            if (l) {
                ServerListGetQueryStats(l, &ok, &late);
                answered += ok + late;
                total += ServerListCount(l);
            }
        }
        if (!rl_mm.note.empty()) {
            state = rl_mm.note.c_str();
        } else if (rl_sv.failed && rl_sv.rows.empty()) {
            state = "NO MASTER SERVER";
        } else if (RL_SvBusy()) {
            state = total ? va("QUERYING  %d / %d", answered, total) : "GETTING THE LIST";
        } else {
            state = "READY";
        }
        RL_Box(v, x + 1, sy, w - 2, 40, RL_Rgb(255, 255, 255, 12));
        RL_Box(v, x, sy, w, 1, RL_LINE);
        RL_Text(v, x + RL_PAD, sy + 20, va("%d SERVERS", (int)rl_vis.rows.size()), 15, RL_TEXT, true, RL_LEFT, 1.6f, RL_MID);
        RL_Text(v, x + w / 2, sy + 20, state, 15, RL_BRASS, true, RL_CENTER, 2, RL_MID);
        RL_Text(v, x + w - RL_PAD, sy + 20, va("%d PLAYERS", rl_vis.players), 15, RL_TEXT, true, RL_RIGHT, 1.6f, RL_MID);
    }
    {
        // the actions
        const float ay = y + h - RL_FOOT, bh = RL_CTRL, by = ay + (RL_FOOT - RL_CTRL) / 2, jw = 130, fx = x + RL_PAD + 154;
        const float fw = w - 2 * RL_PAD - 150 - jw - 8;
        const bool  busy = RL_SvBusy();

        RL_Box(v, x + 1, ay, w - 2, RL_FOOT - 1, RL_HEAD);
        RL_Button(v, x + RL_PAD, by, 150, bh, "REFRESH", false, 0, 12, m->Hot(ID_REFRESH) && !busy, busy);
        RL_Field(v, fx, by, fw, bh, 12, 12, rl_mm.search, "Search server name\x85", rl_mm.edit == F_SEARCH, m->Hot(ID_SEARCH));
        RL_Button(v, x + w - RL_PAD - jw, by, jw, bh, "JOIN", true, 12, 0, m->Hot(ID_JOIN));
        m->Hit(v, x + RL_PAD, by, 150, bh, ID_REFRESH);
        m->Hit(v, fx, by, fw, bh, ID_SEARCH);
        m->Hit(v, x + w - RL_PAD - jw, by, jw, bh, ID_JOIN);
    }
}

/*
================
PLAY's profile: the name (APPLY sets it), each side's model in a 3D view
(the back layer places the views over the columns) under the side's
emblem, and a list to pick it from (open: over the view, in the front
layer), the pickers on the foot
row's line
================
*/
static void RL_Profile(UIReloadedMenu *m, const rlView_t& v, float x, float y, float w, float h)
{
    static cvar_t *views[2];
    const float    aw = 110, cw = (w - 2 * RL_PAD - 10) / 2, fy = y + h - RL_FOOT + (RL_FOOT - RL_CTRL) / 2;
    // the views: as tall as the figures fit the columns' width (arms out), on the pickers
    // (the emblems 44 across, 26 over the views, 16 under the name's line at least)
    const float    vh = Q_min(fy - 14 - (y + 148 + 16 + 70), cw * 2.5f), vy = fy - 14 - vh;

    if (!views[0]) {
        views[0] = Cvar_Get("ui_reloaded_allies", "", 0);
        views[1] = Cvar_Get("ui_reloaded_axis", "", 0);
    }
    if (m->m_back) {
        RL_Panel(v, x, y, w, h, RL_PANEL);
        RL_Head(v, x, y, w, 48);
        for (int t = 0; t < 2; t++) {
            const std::string figure = RL_Figure(RL_OwnModel(t));

            if (Q_stricmp(views[t]->string, figure.c_str())) {
                Cvar_Set(views[t]->name, figure.c_str());
            }
            m->PlaceView(v, t ? "axis" : "allies", x + RL_PAD + t * (cw + 10), vy, cw, vh);
        }
        return;
    }
    RL_Text(v, x + RL_PAD, y + 24, "PROFILE", 20, RL_TEXT, true, RL_LEFT, 2, RL_MID);
    RL_Text(v, x + RL_PAD, y + 76, "NAME", 14, RL_MUTED, true, RL_LEFT, 1.6f, RL_MID);
    RL_Field(v, x + RL_PAD, y + 92, w - 2 * RL_PAD - aw - 4, 40, 0, 12, rl_mm.name, "", rl_mm.edit == F_NAME, m->Hot(ID_NAME));
    RL_Button(v, x + w - RL_PAD - aw, y + 92, aw, 40, "APPLY", true, 12, 0, m->Hot(ID_APPLY));
    m->Hit(v, x + RL_PAD, y + 92, w - 2 * RL_PAD - aw - 4, 40, ID_NAME);
    m->Hit(v, x + w - RL_PAD - aw, y + 92, aw, 40, ID_APPLY);
    RL_Box(v, x, y + 148, w, 1, RL_LINE);
    for (int t = 0; t < 2; t++) {
        const float     cx  = x + RL_PAD + t * (cw + 10), fh = RL_CTRL;
        const int       own = RL_OwnIndex(t);
        const bool      hot = m->Hot(ID_SELECT + t) || rl_mm.pick == t;
        const rlColor_t edge = hot ? RL_Rgb(255, 255, 255, 70) : RL_Rgb(255, 255, 255, 30);

        RL_TeamIcon(v, t, cx + cw / 2, vy - 48, 44);
        RL_Box(v, cx, fy, cw, fh, RL_Rgb(0, 0, 0, 110));
        RL_BoxPx(RL_P(v, cx), RL_P(v, fy), RL_P(v, cx + cw), RL_P(v, fy) + 1, edge);
        RL_BoxPx(RL_P(v, cx), RL_P(v, fy + fh) - 1, RL_P(v, cx + cw), RL_P(v, fy + fh), edge);
        RL_BoxPx(RL_P(v, cx), RL_P(v, fy), RL_P(v, cx) + 1, RL_P(v, fy + fh), edge);
        RL_BoxPx(RL_P(v, cx + cw) - 1, RL_P(v, fy), RL_P(v, cx + cw), RL_P(v, fy + fh), edge);
        RL_Text(v, cx + 14, fy + fh / 2,
                RL_Ellipsize(v, own >= 0 ? rl_models[t][own].name.c_str() : RL_ModelName(RL_OwnModel(t).c_str()).c_str(), 16, false, cw - 48).c_str(),
                16, RL_TEXT, false, RL_LEFT, 0, RL_MID);
        RL_Arrow(v, cx + cw - 26, fy + fh / 2 - 4, 12, 8, rl_mm.pick == t, hot ? RL_TEXT : RL_MUTED);
        m->Hit(v, cx, fy, cw, fh, ID_SELECT + t);
    }
    if (rl_mm.pick >= 0) {
        // the side's models, over its view (scrolled by the wheel when they don't fit)
        const std::vector<rlModel_t>& list = rl_models[rl_mm.pick];
        const float                   ih = 32, lx = x + RL_PAD + rl_mm.pick * (cw + 10), lw = Q_max(cw, 200.0f);
        const int                     n  = (int)list.size(), fit = Q_max(1, Q_min(n, (int)((fy - 8 - (y + 160)) / ih)));
        const float                   lh = fit * ih + 8, ly = fy - 4 - lh, px = Q_min(lx, x + w - RL_PAD - lw);
        const int                     own = RL_OwnIndex(rl_mm.pick);

        rl_mm.pickTop = Q_max(0, Q_min(rl_mm.pickTop, n - fit));
        RL_Panel(v, px, ly, lw, lh, RL_Rgb(12, 13, 15, 250));
        for (int i = 0; i < fit && rl_mm.pickTop + i < n; i++) {
            const int   k  = rl_mm.pickTop + i;
            const float iy = ly + 4 + i * ih;

            if (k == own) {
                RL_Box(v, px + 2, iy, lw - 4, ih, RL_SEL);
                RL_Box(v, px + 2, iy, 3, ih, RL_BRASS);
            } else if (m->Hot(ID_PICK + k)) {
                RL_Box(v, px + 2, iy, lw - 4, ih, RL_Rgb(255, 255, 255, 18));
            }
            RL_Text(v, px + 14, iy + ih / 2, RL_Ellipsize(v, list[k].name.c_str(), 16, k == own, lw - 28).c_str(), 16,
                    k == own ? RL_BRASS : RL_TEXT, k == own, RL_LEFT, 0, RL_MID);
            m->Hit(v, px + 2, iy, lw - 4, ih, ID_PICK + k);
        }
    }
}

/*
================
HOST: Omaha's host panel. The maps (the mode's) on the left, a row each:
its photo, its name, its file; the one picked large on the right with its
name over it, then a row each: the game type, the time and frag limits,
the players and bots, the server's name, who can join; START on the foot
row
================
*/
static void RL_Host(UIReloadedMenu *m, const rlView_t& v, float x, float y, float w, float h)
{
    // (the rows on the right 74 apart: a label, its control 16 under it, 40 high)
    const float lw = floorf(w * 0.40f + 0.5f), rh = 76, ly = y + 96, foot = y + h - RL_FOOT;
    const float rx = x + lw + RL_PAD, rw = w - lw - 2 * RL_PAD, col = (rw - RL_PAD) / 2;
    // (the picture's top on the first map's photo's)
    const float py = ly + 8, ph = floorf(Q_min(Q_min(rw * 9 / 16, h * 0.42f), foot - py - 398));
    const int   fit = Q_max(1, (int)((foot - ly) / rh)), n = (int)rl_host.maps.size();
    float       fy  = py + ph + RL_PAD + 14;

    RL_Panel(v, x, y, w, h, RL_PANEL);
    RL_Head(v, x, y, w, 48);
    RL_Text(v, x + RL_PAD, y + 24, "HOST A GAME", 20, RL_TEXT, true, RL_LEFT, 2, RL_MID);
    RL_Box(v, x + lw, y + 49, 1, foot - (y + 49), RL_LINE);
    // the maps
    RL_Label(v, x + RL_PAD, y + 76, "MAP");
    rl_host.top = Q_max(0, Q_min(rl_host.top, n - fit));
    for (int i = 0; i < fit && rl_host.top + i < n; i++) {
        const std::string& map = rl_host.maps[rl_host.top + i];
        const float        yy = ly + i * rh, th = rh - 16, tw = floorf(th * 16 / 9 + 0.5f), tx = x + RL_PAD + tw + 16;
        const bool         sel = map == rl_host.map;

        if (sel) {
            RL_Box(v, x + 2, yy, lw - 3, rh, RL_SEL);
            RL_Box(v, x + 2, yy, 3, rh, RL_BRASS);
        } else if (m->Hot(ID_MAP + i)) {
            RL_Box(v, x + 2, yy, lw - 3, rh, RL_Rgb(255, 255, 255, 14));
        }
        RL_Photo(v, RL_Shot(map), x + RL_PAD, yy + 8, tw, th);
        RL_Text(v, tx, yy + rh / 2 - 11, RL_Ellipsize(v, RL_MapName(map.c_str()), 19, sel, x + lw - RL_PAD - tx).c_str(), 19, RL_TEXT, sel,
                RL_LEFT, 0, RL_MID);
        RL_Text(v, tx, yy + rh / 2 + 15, map.c_str(), 14, RL_MUTED, false, RL_LEFT, 0.6f, RL_MID, x + lw - RL_PAD - tx);
        m->Hit(v, x + 2, yy, lw - 3, rh, ID_MAP + i);
    }
    if (!n) {
        RL_Text(v, x + RL_PAD, ly + rh / 2, "No maps for this mode.", 17, RL_MUTED, false, RL_LEFT, 0, RL_MID);
    } else if (n > fit) {
        const float th = foot - ly - 8;

        RL_Box(v, x + lw - 8, ly + 4 + th * rl_host.top / n, 3, Q_max(24.0f, th * fit / n), RL_Rgb(255, 255, 255, 60));
    }
    // the map picked, its name over its foot
    RL_Photo(v, rl_host.map.empty() ? 0 : RL_Shot(rl_host.map), rx, py, rw, ph);
    if (!rl_host.map.empty()) {
        const int x0 = RL_P(v, rx), y0 = RL_P(v, py + ph - 72);

        RL_Shape(x0, y0, RL_P(v, rx + rw) - x0, RL_P(v, py + ph) - y0, "vgradient 0 1 0 0 0 0 0 0 0 0.75", RL_Rgb(255, 255, 255), v.s);
        RL_Text(v, rx + RL_PAD, py + ph - RL_PAD - 8, RL_MapName(rl_host.map.c_str()), 28, RL_TEXT, true, RL_LEFT, 0, RL_BASE, rw - 2 * RL_PAD);
    }
    // the settings
    RL_Label(v, rx, fy, "GAMETYPE");
    RL_Segments(m, v, rx, fy + 16, rl_modeLabels, 4, rl_host.mode, ID_MODE);
    fy += 74;
    for (int f = HF_TIME; f < HF_NUM; f++) {
        static const char *const labels[HF_NUM] = {"TIME LIMIT", "FRAG LIMIT", "PLAYERS", "BOTS", "SERVER NAME"};
        // (two a row; the name the whole row)
        const float fx = rx + (f % 2) * (col + RL_PAD), fw = f == HF_NAME ? rw : col;

        RL_Label(v, fx, fy, labels[f]);
        RL_Field(v, fx, fy + 16, fw, 40, 0, 0, rl_host.field[f], "", rl_mm.edit == F_HOST + f, m->Hot(ID_FIELD + f));
        m->Hit(v, fx, fy + 16, fw, 40, ID_FIELD + f);
        fy += f % 2 || f == HF_NAME ? 74 : 0;
    }
    RL_Label(v, rx, fy, "WHO CAN JOIN");
    RL_Segments(m, v, rx, fy + 16, rl_joinLabels, 2, rl_host.online, ID_LOCAL);
    // the foot row
    RL_Box(v, x + 1, foot, w - 2, RL_FOOT - 1, RL_HEAD);
    RL_Box(v, x, foot, w, 1, RL_LINE);
    RL_Button(v, x + w - RL_PAD - 190, foot + (RL_FOOT - RL_CTRL) / 2, 190, RL_CTRL, "START", true, 12, 0, m->Hot(ID_START), rl_host.map.empty());
    m->Hit(v, x + w - RL_PAD - 190, foot + (RL_FOOT - RL_CTRL) / 2, 190, RL_CTRL, ID_START);
}

/*
================
DrawMain: the back layer the photo and what is under the model views (the
profile's panel), the front the rest
================
*/
void UIReloadedMenu::DrawMain(const rlView_t& v)
{
    const float ax = v.dw >= 1700 ? 480.0f : 420.0f, ay = RL_EDGE, aw = Q_min(v.dw - ax - RL_EDGE, 1640.0f), ah = v.dh - 2 * RL_EDGE;
    const float pw = Q_max(270.0f, floorf(aw * 0.28f + 0.5f));

    // (the cvars it draws from: the cache draws again when one changes)
    UI_DrawDep(Cvar_Get("dm_playermodel", "", 0));
    UI_DrawDep(Cvar_Get("dm_playergermanmodel", "", 0));
    if (m_back) {
        RL_Background(v);
        if (rl_mm.page == PAGE_PLAY) {
            RL_Profile(this, v, ax + aw - pw, ay, pw, ah);
        } else {
            PlaceView(v, "allies", 0, 0, 0, 0);
            PlaceView(v, "axis", 0, 0, 0, 0);
        }
        return;
    }
    RL_Rail(this, v);
    if (rl_mm.page == PAGE_PLAY) {
        RL_Browser(this, v, ax, ay, aw - pw - 20, ah);
        RL_Profile(this, v, ax + aw - pw, ay, pw, ah);
    } else if (rl_mm.page == PAGE_HOST) {
        RL_Host(this, v, ax, ay, aw, ah);
    }
}

/*
================
DrawConfirm: Omaha's confirmation, over the main menu: Quit's, or
Disconnect's in a game
================
*/
void UIReloadedMenu::DrawConfirm(const rlView_t& v)
{
    const float w = 560, h = 230, x = (v.dw - w) / 2, y = (v.dh - h) / 2, by = y + h - 24 - 48;
    const bool  quit = rl_mm.confirm == ID_QUIT;

    if (m_back) {
        return;
    }
    RL_Box(v, 0, 0, v.dw, v.dh, RL_Rgb(0, 0, 0, 140));
    RL_Panel(v, x, y, w, h, RL_Rgb(12, 13, 15, 250));
    RL_Head(v, x, y, w, 56);
    RL_Text(v, x + 24, y + 28, quit ? "QUIT" : "DISCONNECT", 22, RL_TEXT, true, RL_LEFT, 2, RL_MID);
    RL_Text(v, x + 24, y + 100, quit ? "Close the game?" : "Leave the game?", 22, RL_SOFT, false, RL_LEFT, 0, RL_MID);
    RL_Button(v, x + w - 24 - 328, by, 150, 48, "CANCEL", false, 0, 12, Hot(ID_CANCEL));
    RL_Button(v, x + w - 24 - 170, by, 170, 48, quit ? "QUIT" : "DISCONNECT", true, 12, 0, Hot(ID_CONFIRM));
    Hit(v, x + w - 24 - 328, by, 150, 48, ID_CANCEL);
    Hit(v, x + w - 24 - 170, by, 170, 48, ID_CONFIRM);
}

/*
====================================================================

INPUT

====================================================================
*/

// the text of the field typed in, NULL none
static std::string *RL_Edited(void)
{
    return rl_mm.edit == F_SEARCH ? &rl_mm.search : rl_mm.edit == F_NAME ? &rl_mm.name : rl_mm.edit >= F_HOST ? &rl_host.field[rl_mm.edit - F_HOST] : NULL;
}

static void RL_ApplyName(void)
{
    if (!rl_mm.name.empty()) {
        Cvar_Set("name", rl_mm.name.c_str());
        uii.Snd_PlaySound("sound/menu/apply.wav");
    }
    rl_mm.edit = F_NONE;
}

// the server picked moved to (the list scrolled to show it)
static void RL_SvPick(int at)
{
    const int n = (int)rl_vis.rows.size();

    if (!n) {
        return;
    }
    at        = Q_max(0, Q_min(at, n - 1));
    rl_mm.sel = rl_sv.rows[rl_vis.rows[at]].addr;
    rl_mm.top = Q_min(rl_mm.top, at);
    rl_mm.top = Q_max(rl_mm.top, at - rl_mm.fit + 1);
    rl_mm.note.clear();
}

void UIReloadedMenu::ChooseMain(int id)
{
    if (m_screen == SCR_CONFIRM) {
        if (id == ID_CONFIRM) {
            uii.Snd_PlaySound("sound/menu/apply.wav");
            // (the confirmation closed first: the main menu stays, out of the game)
            Cbuf_AddText(rl_mm.confirm == ID_QUIT ? "quit\n" : "popmenu 0\ndisconnect\n");
        } else if (id == ID_CANCEL) {
            uii.Snd_PlaySound("sound/menu/back.wav");
            Cbuf_AddText("popmenu 0\n");
        }
        return;
    }
    RL_Changed();
    // a click elsewhere closes the model list and ends the typing
    if (rl_mm.pick >= 0 && (id < ID_PICK || id >= ID_PICK + (int)rl_models[rl_mm.pick].size())) {
        const int was = rl_mm.pick;

        rl_mm.pick = -1;
        if (id == ID_SELECT + was) {
            return;
        }
    }
    if (id != ID_SEARCH && id != ID_NAME && id != ID_APPLY && (id < ID_FIELD || id >= ID_FIELD + HF_NUM)) {
        rl_mm.edit = F_NONE;
    }
    if (id >= ID_MAP) {
        if (rl_host.top + id - ID_MAP < (int)rl_host.maps.size()) {
            rl_host.map = rl_host.maps[rl_host.top + id - ID_MAP];
            uii.Snd_PlaySound("sound/menu/apply.wav");
        }
        return;
    }
    if (id >= ID_PICK) {
        const int t = rl_mm.pick, k = id - ID_PICK;

        Cvar_Set(t ? "dm_playergermanmodel" : "dm_playermodel", rl_models[t][k].file.c_str());
        rl_mm.pick = -1;
        uii.Snd_PlaySound("sound/menu/apply.wav");
        return;
    }
    if (id >= ID_FAV) {
        if (rl_mm.top + id - ID_FAV < (int)rl_vis.rows.size()) {
            RL_SvFavorite(rl_sv.rows[rl_vis.rows[rl_mm.top + id - ID_FAV]].addr);
        }
        return;
    }
    if (id >= ID_ROW) {
        const int  at    = rl_mm.top + id - ID_ROW;
        const bool again = at < (int)rl_vis.rows.size() && rl_sv.rows[rl_vis.rows[at]].addr == rl_mm.sel;

        RL_SvPick(at);
        // a double click joins
        if (again && cls.realtime - rl_mm.clickAt < 400) {
            RL_SvJoin();
        }
        rl_mm.clickAt = cls.realtime;
        return;
    }
    if (id >= ID_TAB && id < ID_TAB + TAB_NUM) {
        rl_mm.tab    = id - ID_TAB;
        rl_mm.top    = 0;
        rl_vis.dirty = true;
        uii.Snd_PlaySound("sound/menu/apply.wav");
        return;
    }
    if (id >= ID_SORT && id < ID_SORT + COL_NUM) {
        // the same column the other way; another its own way first (players: the most first)
        rl_mm.asc    = rl_mm.sort == id - ID_SORT ? !rl_mm.asc : id - ID_SORT != COL_PLAYERS;
        rl_mm.sort   = id - ID_SORT;
        rl_vis.dirty = true;
        return;
    }
    if (id >= ID_MODE && id < ID_MODE + (int)ARRAY_LEN(rl_modeTypes)) {
        rl_host.mode = id - ID_MODE;
        RL_HostList();
        uii.Snd_PlaySound("sound/menu/apply.wav");
        return;
    }
    if (id >= ID_FIELD && id < ID_FIELD + HF_NUM) {
        rl_mm.edit = F_HOST + id - ID_FIELD;
        return;
    }
    switch (id) {
    case ID_LOCAL:
    case ID_ONLINE:
        rl_host.online = id == ID_ONLINE;
        uii.Snd_PlaySound("sound/menu/apply.wav");
        break;
    case ID_START:
        RL_HostStart();
        break;
    case ID_PLAY:
    case ID_HOST:
    case ID_OPTIONS:
        if (rl_mm.page != id - ID_PLAY) {
            rl_mm.page = id - ID_PLAY;
            uii.Snd_PlaySound("sound/menu/apply.wav");
            if (rl_mm.page == PAGE_PLAY && !rl_sv.list[0]) {
                RL_SvRefresh();
            }
        }
        break;
    case ID_QUIT:
    case ID_DISCONNECT:
        uii.Snd_PlaySound("sound/menu/apply.wav");
        rl_mm.confirm = id;
        menuManager.ShowMenu("reloaded_confirm");
        break;
    case ID_REFRESH:
        if (!RL_SvBusy()) {
            uii.Snd_PlaySound("sound/menu/apply.wav");
            RL_SvRefresh();
        }
        break;
    case ID_SEARCH:
        rl_mm.edit = F_SEARCH;
        break;
    case ID_NAME:
        rl_mm.edit = F_NAME;
        break;
    case ID_APPLY:
        RL_ApplyName();
        break;
    case ID_JOIN:
        uii.Snd_PlaySound("sound/menu/apply.wav");
        RL_SvJoin();
        break;
    case ID_SELECT:
    case ID_SELECT + 1:
        {
            // open at its model
            const int t = id - ID_SELECT;

            rl_mm.pick    = t;
            rl_mm.pickTop = Q_max(0, RL_OwnIndex(t) - 3);
        }
        break;
    }
}

qboolean UIReloadedMenu::MainKey(int key)
{
    const int step = key == K_MWHEELUP ? -3 : key == K_MWHEELDOWN ? 3 : 0;

    if (m_screen == SCR_CONFIRM) {
        if (key == K_ENTER || key == K_KP_ENTER) {
            ChooseMain(ID_CONFIRM);
            return qtrue;
        }
        return qfalse;
    }
    if (step) {
        // the wheel: the model list open, else the page's list
        if (rl_mm.pick >= 0) {
            rl_mm.pickTop += step;
        } else if (rl_mm.page == PAGE_HOST) {
            rl_host.top += step;
        } else {
            rl_mm.top += step;
        }
        RL_Changed();
        return qtrue;
    }
    if (RL_Edited()) {
        std::string& text = *RL_Edited();

        if (key == K_BACKSPACE) {
            if (!text.empty()) {
                text.erase(text.size() - 1);
                if (rl_mm.edit == F_SEARCH) {
                    rl_vis.dirty = true;
                }
            }
            RL_Changed();
            return qtrue;
        }
        if (key == K_ENTER || key == K_KP_ENTER) {
            if (rl_mm.edit == F_NAME) {
                RL_ApplyName();
            } else {
                rl_mm.edit = F_NONE;
            }
            RL_Changed();
            return qtrue;
        }
    }
    if (rl_mm.page != PAGE_PLAY) {
        return qfalse;
    }
    switch (key) {
    case K_UPARROW:
    case K_DOWNARROW:
    case K_PGUP:
    case K_PGDN:
        {
            const int at = RL_SvPicked(), d = key == K_UPARROW ? -1 : key == K_DOWNARROW ? 1 : key == K_PGUP ? -rl_mm.fit : rl_mm.fit;

            RL_SvShown();
            RL_SvPick(at < 0 ? rl_mm.top : at + d);
            RL_Changed();
        }
        return qtrue;
    case K_ENTER:
    case K_KP_ENTER:
        RL_SvJoin();
        RL_Changed();
        return qtrue;
    }
    return qfalse;
}

void UIReloadedMenu::MainChar(int ch)
{
    std::string *text = RL_Edited();
    // (the printable ones; the player's name as long as the game keeps it, a
    // server's as the stock host menu's, Host's numbers three digits)
    const bool number = rl_mm.edit >= F_HOST && rl_mm.edit != F_HOST + HF_NAME;
    const int  most   = rl_mm.edit == F_NAME ? MAX_NAME_LENGTH - 1 : rl_mm.edit == F_HOST + HF_NAME ? 31 : number ? 3 : 40;

    if (!text || ch < ' ' || ch > '~' || (int)text->size() >= most || (number && !isdigit(ch))
        || (rl_mm.edit == F_HOST + HF_NAME && !RL_NameChar(ch))) {
        return;
    }
    *text += (char)ch;
    if (rl_mm.edit == F_SEARCH) {
        rl_mm.top    = 0;
        rl_vis.dirty = true;
    }
    RL_Changed();
}

/*
================
MainTick

Each frame the menu shows: the back layer's photos moved on; the front's
lists' queries run on, Host's maps' photos made. Opened: the models
listed, the name as it is, the favourites read, the servers asked for the
first time
================
*/
void UIReloadedMenu::MainTick(bool opened)
{
    if (m_back) {
        RL_BgTick(opened);
        return;
    }
    if (opened) {
        const char *favs;

        if (!ui_favoriteservers) {
            ui_favoriteservers = Cvar_Get("ui_favoriteservers", "", CVAR_ARCHIVE);
        }
        RL_ListModels();
        rl_mm.name = Cvar_VariableString("name");
        rl_mm.edit = F_NONE;
        rl_mm.pick = -1;
        rl_mm.note.clear();
        rl_favs.clear();
        favs = ui_favoriteservers->string;
        while (*favs) {
            const char *end = strchr(favs, ' ');
            const size_t n  = end ? (size_t)(end - favs) : strlen(favs);

            if (n) {
                rl_favs.insert(std::string(favs, n));
            }
            favs += n + (end ? 1 : 0);
        }
        rl_vis.dirty = true;
        if (rl_mm.page == PAGE_PLAY && !rl_sv.list[0]) {
            RL_SvRefresh();
        }
        RL_Changed();
    }
    for (GServerList l : rl_sv.list) {
        if (l && ServerListState(l) != sl_idle) {
            ServerListThink(l);
        }
    }
    if (rl_mm.page == PAGE_HOST) {
        // Host's settings read the first time, the maps' photos made one a frame (the one picked first)
        if (!rl_host.ready) {
            RL_HostRead();
            RL_Changed();
        }
        if (!rl_host.map.empty() && RL_Shot(rl_host.map) < 0) {
            RL_ShotMake(rl_host.map);
            RL_Changed();
            return;
        }
        for (const std::string& map : rl_host.maps) {
            if (RL_Shot(map) < 0) {
                RL_ShotMake(map);
                RL_Changed();
                break;
            }
        }
    }
}

int UIReloadedMenu::MainDrawKey(void)
{
    if (m_back) {
        // the photos (made, faded), the profile's page
        return (RL_BgFade() * 2 + rl_bg.cur) * 4099 + rl_bg.shader[0] * 31 + rl_bg.shader[1] * 7 + rl_mm.page;
    }
    // (the caret's blink while typed in; in a game, DISCONNECT)
    return rl_mm.serial * 31 + rl_sv.serial * 7 + (rl_mm.edit != F_NONE ? (cls.realtime / 530) % 2 : 0) * 3 + (clc.state > CA_PRIMED);
}
