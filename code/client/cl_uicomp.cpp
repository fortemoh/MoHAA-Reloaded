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

// cl_uicomp.cpp: the Competitive HUD (Added, cg_hudstyle 1), Project: Omaha's
// design drawn by URC menus (ui/hud_comp_*.urc). What the menus show is
// published here as cvars (ui_hud_*) their labels link to; what changes in
// number or width has a widget: the kill feed, the chat and the game
// messages (HudFeed), the players' cards (HudCards), the weapons carried
// (HudWeapons) and the radar's map (HudRadar).

#include "client.h"
#include "cl_ui.h"
#include "cl_uicomp.h"
#include "cl_uiradar.h"
#include "../uilib/uibake.h"
#include "../qcommon/localization.h"

#include <unordered_map>
#include <vector>

#define COMP_PICTURES "ui/competitive/"

// the stock radar's teammates (cl_uiradar.cpp)
extern radarClient_t *g_radarClients;
extern int            g_radarClientNum;
#define COMP_LINES    8
#define COMP_KILLS    6

static cvar_t *cg_hudstyle;

// the elements one can turn off (as cgame's Competitive HUD); the menus' enabledcvar
static const char *const comp_toggles[] = {
    "cg_hud_radar", "cg_hud_killfeed", "cg_hud_chat", "cg_hud_messages", "cg_hud_health", "cg_hud_ammo",
    "cg_hud_grenades", "cg_hud_grenadecharge", "cg_hud_score", "cg_hud_timer", "cg_hud_alive", "cg_hud_teamicon",
    "cg_hud_killer", "cg_hud_targetinfo", "cg_hud_vote", "cg_hud_spectator", "cg_hud_following", "cg_hud_attacker",
    "cg_hud_stopwatch", "cg_hud_damage",
};

typedef struct {
    int      time;
    char     text[160];
    float    color[3];
    qboolean bold;
} compLine_t;

typedef struct {
    int         time;
    char        killer[MAX_NAME_LENGTH], victim[MAX_NAME_LENGTH];
    int         killerTeam, victimTeam;
    const char *pic;
    float       picW, picH; // design pixels
    qboolean    suicide, headshot;
} compKill_t;

static struct {
    compLine_t chat[COMP_LINES], msgs[COMP_LINES];
    int        numChat, numMsgs;
    compKill_t kills[COMP_KILLS];
    int        numKills;

    // who is alive (round modes): the clients' teams, the scoreboard's and
    // the kills' deaths
    int      team[MAX_CLIENTS];
    qboolean dead[MAX_CLIENTS];
    char     roundStart[64];
    int      sbCount;           // ui_hud_sb's modificationCount read
    int      sbScore[2];        // the scoreboard's teams' scores, -1 unknown
    int      scoreTime;         // the last silent score request
    qboolean wantScores, wasPlaying;
    int      lastTeam;
    char     cards[2][MAX_CLIENTS + 1]; // a card each: m me, a alive, d dead

    // the weapons carried (names as the server's configstrings have them)
    char     primary[64], sidearm[64], pending[64];
    int      follow, loaded[2], frags, smokes, nation;
    char     wbRound[64];
    qboolean charging, chargeSecondary;
    int      chargeStart;

    // the damage's arcs
    int arcStat[3], arcTime[3];
    int lastDmg, lastHealth, flashTime;
} comp;

/*
====================================================================

WEAPONS AND KILLS

====================================================================
*/

enum { WK_PRIMARY, WK_SIDEARM, WK_FRAG, WK_SMOKE };
enum { NAT_ANY, NAT_US, NAT_UK, NAT_RU, NAT_DE, NAT_IT };

typedef struct {
    const char *name; // the weapon's configstring
    const char *pic;  // COMP_PICTURES<pic>.png
    int         kind, nation;
} compWeapon_t;

static const compWeapon_t comp_weapons[] = {
    {"M1 Garand",              "m1_garand_filled",            WK_PRIMARY, NAT_US },
    {"Thompson",               "thompsonsmg_filled",          WK_PRIMARY, NAT_US },
    {"BAR",                    "bar_filled",                  WK_PRIMARY, NAT_ANY},
    {"Springfield '03 Sniper", "springfield_filled",          WK_PRIMARY, NAT_ANY},
    {"Shotgun",                "shotgun_filled",              WK_PRIMARY, NAT_ANY},
    {"Bazooka",                "bazooka_filled",              WK_PRIMARY, NAT_ANY},
    {"Colt 45",                "colt45_filled",               WK_SIDEARM, NAT_US },
    {"Hi-Standard Silenced",   "silencedpistol_filled",       WK_SIDEARM, NAT_ANY},
    {"Frag Grenade",           "m2frag_grenade_filled",       WK_FRAG,    NAT_US },
    {"M18 Smoke Grenade",      "m18_smoke_grenade_filled",    WK_SMOKE,   NAT_ANY},
    {"Lee-Enfield",            "enfield_filled",              WK_PRIMARY, NAT_UK },
    {"Sten Mark II",           "sten_filled",                 WK_PRIMARY, NAT_UK },
    {"Enfield L42A1",          "uk_w_l42a1_filled",           WK_PRIMARY, NAT_UK },
    {"DeLisle",                "delisle_filled",              WK_PRIMARY, NAT_UK },
    {"Vickers-Berthier",       "uk_w_vickers_filled",         WK_PRIMARY, NAT_UK },
    {"PIAT",                   "uk_w_piat_filled",            WK_PRIMARY, NAT_UK },
    {"Webley Revolver",        "webley_revolver_filled",      WK_SIDEARM, NAT_UK },
    {"Mills Grenade",          "mills_grenade_filled",        WK_FRAG,    NAT_UK },
    {"Mosin Nagant Rifle",     "mosin_nagant_rifle_filled",   WK_PRIMARY, NAT_RU },
    {"PPSH SMG",               "ppsh_smg_filled",             WK_PRIMARY, NAT_RU },
    {"SVT 40",                 "svt_rifle_filled",            WK_PRIMARY, NAT_RU },
    {"Nagant Revolver",        "nagant_revolver_filled",      WK_SIDEARM, NAT_RU },
    {"Russian F1 Grenade",     "russian_f1_grenade_filled",   WK_FRAG,    NAT_RU },
    {"RDG-1 Smoke Grenade",    "rdg-1_smoke_grenade_filled",  WK_SMOKE,   NAT_RU },
    {"Mauser KAR 98K",         "kar98_filled",                WK_PRIMARY, NAT_DE },
    {"KAR98 - Sniper",         "kar98sniper_filled",          WK_PRIMARY, NAT_ANY},
    {"G 43",                   "g43_filled",                  WK_PRIMARY, NAT_DE },
    {"MP40",                   "mp40_filled",                 WK_PRIMARY, NAT_DE },
    {"StG 44",                 "mp44_filled",                 WK_PRIMARY, NAT_DE },
    {"Panzerschreck",          "panzerschreck_filled",        WK_PRIMARY, NAT_ANY},
    {"Gewehrgranate",          "kar98_mortar_filled",         WK_PRIMARY, NAT_DE },
    {"Walther P38",            "p38_filled",                  WK_SIDEARM, NAT_DE },
    {"Stielhandgranate",       "steilhandgranate_filled",     WK_FRAG,    NAT_DE },
    {"Nebelhandgranate",       "nebelhandgranate_filled",     WK_SMOKE,   NAT_DE },
    {"Carcano",                "it_w_carcano_filled",         WK_PRIMARY, NAT_IT },
    {"Moschetto",              "it_w_moschetto_filled",       WK_PRIMARY, NAT_IT },
    {"Breda",                  "it_w_breda_filled",           WK_PRIMARY, NAT_IT },
    {"Beretta",                "it_w_beretta_filled",         WK_SIDEARM, NAT_IT },
    {"Bomba A Mano",           "it_w_bomba_filled",           WK_FRAG,    NAT_IT },
    {"Bomba A Mano Breda",     "it_w_bombabreda_filled",      WK_SMOKE,   NAT_IT },
};

// a nation's sidearm, frag and smoke (by NAT_*)
static const char *const comp_issued[6][3] = {
    {"Colt 45",         "Frag Grenade",       "M18 Smoke Grenade"  },
    {"Colt 45",         "Frag Grenade",       "M18 Smoke Grenade"  },
    {"Webley Revolver", "Mills Grenade",      "M18 Smoke Grenade"  },
    {"Nagant Revolver", "Russian F1 Grenade", "RDG-1 Smoke Grenade"},
    {"Walther P38",     "Stielhandgranate",   "Nebelhandgranate"   },
    {"Beretta",         "Bomba A Mano",       "Bomba A Mano Breda" },
};

static const compWeapon_t *Comp_Weapon(const char *name)
{
    for (size_t i = 0; name && name[0] && i < ARRAY_LEN(comp_weapons); i++) {
        if (!Q_stricmp(name, comp_weapons[i].name)) {
            return &comp_weapons[i];
        }
    }
    return NULL;
}

// the obituaries' English keys (fgame's), the weapon each says: s2 the text
// after the attacker it needs ("" none at all), kind p a player's, s a
// suicide, w the world's
typedef struct {
    const char *s1, *s2, *cls;
    char        kind;
} compPhrase_t;

static const compPhrase_t comp_phrases[] = {
    {"was gunned down by",             "",                             "pistol",   'p'},
    {"was rifled by",                  "",                             "rifle",    'p'},
    {"was sniped by",                  "",                             "sniper",   'p'},
    {"was perforated by",              "'s' SMG",                      "smg",      'p'},
    {"was machine-gunned by",          "",                             "mg",       'p'},
    {"was hunted down by",             "",                             "shotgun",  'p'},
    {"was pumped full of buckshot by", "",                             "shotgun",  'p'},
    {"tripped on",                     "'s grenade",                   "grenade",  'p'},
    {"is picking",                     "'s shrapnel out of his teeth", "grenade",  'p'},
    {"took",                           "'s rocket right in the kisser", "rocket",  'p'},
    {"took",                           "'s rocket in the face",        "rocket",   'p'},
    {"stepped on",                     "'s landmine",                  "none",     'p'},
    {"was bashed by",                  "",                             "bash",     'p'},
    {"was clubbed by",                 "",                             "bash",     'p'},
    {"was shot by",                    "",                             "rifle",    'p'},
    {"was pushed over the edge by",    "",                             "none",     'p'},
    {"was burned up by",               "",                             "none",     'p'},
    {"was melted by",                  "",                             "none",     'p'},
    {"was knocked out by",             "",                             "none",     'p'},
    {"was run over by",                "",                             "none",     'p'},
    {"was impaled by",                 "",                             "none",     'p'},
    {"was blown away by",              "",                             "none",     'p'},
    {"was crushed by",                 "",                             "none",     'p'},
    {"was telefragged by",             "",                             "none",     'p'},
    {"was killed by",                  "",                             "none",     'p'},
    {"played catch with himself",      NULL,                           "grenade",  's'},
    {"tripped on his own grenade",     NULL,                           "grenade",  's'},
    {"rocketed himself",               NULL,                           "rocket",   's'},
    {"was hoist on his own pitard",    NULL,                           "none",     's'},
    {"took himself out of commision",  NULL,                           "none",     's'},
    {"blew himself up",                NULL,                           "none",     's'},
    {"caught some shrapnel",           NULL,                           "grenade",  'w'},
    {"caught a rocket",                NULL,                           "rocket",   'w'},
    {"stepped on a land mine",         NULL,                           "none",     'w'},
    {"cratered",                       NULL,                           "none",     's'},
    {"was burned to a crisp",          NULL,                           "none",     's'},
    {"was melted to nothing",          NULL,                           "none",     's'},
    {"blew up",                        NULL,                           "none",     'w'},
    {"died",                           NULL,                           "none",     's'},
};

// a weapon class's pictures and their sizes in design pixels, pointing left: the Allies', the Axis'
typedef struct {
    const char *cls, *allies, *axis;
    float       aw, ah, xw, xh;
} compKillPic_t;

static const compKillPic_t comp_killPics[] = {
    {"rifle",   "m1_garand_filled",      "kar98_filled",            90.0f,  16.2f, 90.5f,  16.3f},
    {"pistol",  "colt45_filled",         "p38_filled",              33.9f,  21.9f, 33.9f,  22.2f},
    {"sniper",  "springfield_filled",    "kar98sniper_filled",      89.9f,  17.5f, 90.5f,  17.6f},
    {"smg",     "thompsonsmg_filled",    "mp40_filled",             75.1f,  24.2f, 74.8f,  26.0f},
    {"mg",      "bar_filled",            "mp44_filled",             95.5f,  15.3f, 70.9f,  26.0f},
    {"shotgun", "shotgun_filled",        "shotgun_filled",          85.0f,  15.9f, 85.0f,  15.9f},
    {"grenade", "m2frag_grenade_filled", "steilhandgranate_filled", 12.5f,  22.0f, 5.6f,   26.0f},
    {"rocket",  "bazooka_filled",        "panzerschreck_filled",    110.6f, 20.0f, 113.5f, 26.0f},
    {"bash",    "glove_50",              "glove_50",                20.0f,  20.0f, 20.0f,  20.0f},
    {"none",    "skull_50",              "skull_50",                20.0f,  20.0f, 20.0f,  20.0f},
};

/*
====================================================================

PLAYERS

====================================================================
*/

static int Comp_Gametype(void)
{
    return atoi(Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "g_gametype"));
}

static const char *Comp_Name(int client)
{
    return Info_ValueForKey(CL_ConfigString(CS_PLAYERS + client), "name");
}

static int Comp_Team(int client)
{
    return client >= 0 && client < MAX_CLIENTS ? atoi(Info_ValueForKey(CL_ConfigString(CS_PLAYERS + client), "team")) : 0;
}

// the one client called name, -1 none or more
static int Comp_FindName(const char *name)
{
    int found = -1;

    for (int i = 0; name[0] && i < MAX_CLIENTS; i++) {
        if (!strcmp(Comp_Name(i), name)) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

static const char *Comp_TeamColor(int team)
{
    return team == TEAM_AXIS ? "0.8 0.533 0.533 1" : team == TEAM_ALLIES ? "0.533 0.569 0.8 1" : "1 1 1 1";
}

static void Comp_Trim(const char *src, int len, char *out, int size)
{
    while (len > 0 && *src == ' ') {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\n')) {
        len--;
    }
    Q_strncpyz(out, src, Q_min(len + 1, size));
}

/*
================
Comp_AddKill

A kill as the server printed it, "<victim> <s1> <attacker><s2>": its
phrase gives its weapon, the case of the line's color its victim's team
(lower, MESSAGE_CHAT_GREEN: ours)
================
*/
static void Comp_AddKill(const char *text, qboolean ourVictim)
{
    const compPhrase_t  *ph = NULL;
    const compKillPic_t *pic = &comp_killPics[0];
    compKill_t          *k;
    const char          *hit = NULL, *after, *end = NULL;
    int                  client;
    qboolean             axis;

    for (size_t i = 0; i < ARRAY_LEN(comp_phrases) && !ph; i++) {
        hit = Q_stristr(text, comp_phrases[i].s1);
        if (!hit) {
            continue;
        }
        after = hit + strlen(comp_phrases[i].s1);
        if (comp_phrases[i].kind == 'p' && comp_phrases[i].s2[0]) {
            end = Q_stristr(after, comp_phrases[i].s2);
            if (!end) {
                continue;
            }
        }
        ph = &comp_phrases[i];
    }
    if (comp.numKills == COMP_KILLS) {
        memmove(comp.kills, comp.kills + 1, sizeof(comp.kills[0]) * (COMP_KILLS - 1));
        comp.numKills--;
    }
    k = &comp.kills[comp.numKills++];
    memset(k, 0, sizeof(*k));
    k->time = cls.realtime;
    if (!ph) {
        Comp_Trim(text, (int)strlen(text), k->victim, sizeof(k->victim));
    } else {
        Comp_Trim(text, (int)(hit - text), k->victim, sizeof(k->victim));
        if (ph->kind == 'p') {
            after = hit + strlen(ph->s1);
            if (!end) {
                end = Q_stristr(after, " in the ");
            }
            Comp_Trim(after, end ? (int)(end - after) : (int)strlen(after), k->killer, sizeof(k->killer));
            k->headshot = end && (Q_stristr(end, " in the head") || Q_stristr(end, " in the helmet") || Q_stristr(end, " in the neck"));
        }
        k->suicide = ph->kind == 's';
        for (size_t i = 0; i < ARRAY_LEN(comp_killPics); i++) {
            if (!strcmp(comp_killPics[i].cls, ph->cls)) {
                pic = &comp_killPics[i];
            }
        }
    }
    client        = Comp_FindName(k->victim);
    k->victimTeam = client >= 0 ? Comp_Team(client) : 0;
    if (!k->victimTeam && Comp_Gametype() >= GT_TEAM) {
        const int mine = cl.snap.ps.stats[STAT_TEAM];

        k->victimTeam = ourVictim == (mine != TEAM_AXIS) ? TEAM_ALLIES : TEAM_AXIS;
    }
    k->killerTeam = Comp_Team(Comp_FindName(k->killer));
    // the killer's weapon: of the victim's side without one
    axis    = (k->killer[0] ? k->killerTeam : k->victimTeam) == TEAM_AXIS;
    k->pic  = axis ? pic->axis : pic->allies;
    k->picW = axis ? pic->xw : pic->aw;
    k->picH = axis ? pic->xh : pic->ah;
    if (client >= 0) {
        comp.dead[client] = qtrue;
    }
}

static void Comp_AddLine(compLine_t *lines, int *count, const char *text, float r, float g, float b, qboolean bold)
{
    compLine_t *l;

    if (*count == COMP_LINES) {
        memmove(lines, lines + 1, sizeof(lines[0]) * (COMP_LINES - 1));
        (*count)--;
    }
    l = &lines[(*count)++];
    Comp_Trim(text, (int)strlen(text), l->text, sizeof(l->text));
    for (char *p = l->text; *p; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }
    l->time     = cls.realtime;
    l->color[0] = r;
    l->color[1] = g;
    l->color[2] = b;
    l->bold     = bold;
}

void UI_CompHud_Print(const char *msg)
{
    switch ((byte)msg[0]) {
    case MESSAGE_YELLOW:
        Comp_AddLine(comp.msgs, &comp.numMsgs, msg + 1, 0.702f, 0.6f, 0.051f, qfalse);
        // a weapon picked up: what it is, before it is in hand
        if (!Q_stricmpn(msg + 1, "Picked Up ", 10)) {
            Comp_Trim(msg + 11, (int)strlen(msg + 11), comp.pending, sizeof(comp.pending));
        }
        break;
    case MESSAGE_WHITE:
        Comp_AddLine(comp.msgs, &comp.numMsgs, msg + 1, 1.0f, 1.0f, 1.0f, qtrue);
        break;
    case MESSAGE_CHAT_WHITE:
        Comp_AddLine(comp.chat, &comp.numChat, msg + 1, 1.0f, 1.0f, 1.0f, qfalse);
        break;
    case MESSAGE_CHAT_RED:
    case MESSAGE_CHAT_GREEN:
        Comp_AddKill(msg + 1, (byte)msg[0] == MESSAGE_CHAT_GREEN);
        break;
    }
}

void UI_CompHud_Clear(void)
{
    memset(&comp, 0, sizeof(comp));
    comp.sbScore[0] = comp.sbScore[1] = -1;
    comp.lastDmg                      = -999999;
}

/*
====================================================================

THE FRAME: the ui_hud_* cvars

====================================================================
*/

// a cvar by name, found once (the names are the callers' literals and the few made below)
static cvar_t *Comp_Cvar(const char *name)
{
    static std::unordered_map<const char *, cvar_t *> found;
    cvar_t                                         *&v = found[name];

    if (!v) {
        v = Cvar_Get(name, "", 0);
    }
    return v;
}

static int Comp_Int(const char *name)
{
    return Comp_Cvar(name)->integer;
}

static void Comp_Set(const char *name, const char *value)
{
    cvar_t *v = Comp_Cvar(name);

    if (strcmp(v->string, value)) {
        Cvar_Set(v->name, value);
    }
}

static void Comp_SetF(const char *name, float value)
{
    // (the text made only when the value changed)
    if (fabsf(Comp_Cvar(name)->value - value) >= 0.0005f || !Comp_Cvar(name)->string[0]) {
        Comp_Set(name, va("%.3f", value));
    }
}

// who is alive and the cards (round modes), and the silent score requests:
// a new round, a death, a team joined, and every 2 s without flood protection
static void Comp_Roster(int gametype)
{
    const playerState_t *ps      = &cl.snap.ps;
    const qboolean       playing = !(ps->pm_flags & PMF_SPECTATING) && ps->stats[STAT_HEALTH] > 0;
    const char          *roundStart = CL_ConfigString(CS_WARMUP);
    cvar_t              *sb = Comp_Cvar("ui_hud_sb");
    int                  i, t;

    if (strcmp(roundStart, comp.roundStart)) {
        Q_strncpyz(comp.roundStart, roundStart, sizeof(comp.roundStart));
        memset(comp.dead, 0, sizeof(comp.dead));
        comp.wantScores = qtrue;
    }
    // the scoreboard: "<allies score> <axis score> <a 0 or 1 a client: dead>"
    if (sb->modificationCount != comp.sbCount) {
        char dead[MAX_CLIENTS + 1] = "";

        comp.sbCount = sb->modificationCount;
        sscanf(sb->string, "%d %d %64s", &comp.sbScore[0], &comp.sbScore[1], dead);
        for (i = 0; dead[i] && i < MAX_CLIENTS; i++) {
            comp.dead[i] = dead[i] == '1';
        }
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        const int team = Comp_Team(i);

        if (team != comp.team[i]) {
            // a switch waits for the next round dead
            comp.dead[i]    = comp.team[i] && (team == TEAM_ALLIES || team == TEAM_AXIS);
            comp.team[i]    = team;
            comp.wantScores = qtrue;
        }
    }
    // the players the snapshot shows, and the player watched
    for (i = 0; i < cl.snap.numEntities; i++) {
        const entityState_t *es = &cl.parseEntities[(cl.snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];

        if (es->eType == ET_PLAYER && es->number >= 0 && es->number < MAX_CLIENTS) {
            comp.dead[es->number] = (es->eFlags & EF_DEAD) != 0;
        }
    }
    if (!(ps->pm_flags & PMF_SPECTATING)) {
        comp.dead[ps->clientNum] = ps->stats[STAT_HEALTH] <= 0;
    }
    if (comp.wasPlaying && !playing) {
        comp.wantScores = qtrue;
    }
    comp.wasPlaying = playing;
    if (gametype >= GT_TEAM && (comp.wantScores || !atoi(Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "sv_floodProtect")))
        && (unsigned)(cls.realtime - comp.scoreTime) >= 2000) {
        comp.wantScores = qfalse;
        comp.scoreTime  = cls.realtime;
        CL_AddReliableCommand("score", qfalse);
    }
    for (t = 0; t < 2; t++) {
        char *c = comp.cards[t];
        int   n = 0, pass;

        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (comp.team[i] == (t ? TEAM_AXIS : TEAM_ALLIES) && comp.dead[i] == (pass == 1)) {
                    c[n++] = pass ? 'd' : i == ps->clientNum ? 'm' : 'a';
                }
            }
        }
        c[n] = 0;
    }
}

// the weapons carried: the primary's and the sidearm's names (as seen in
// hand or picked up), the grenades, the grenade's charge
static void Comp_Weapons(const char *inHand, qboolean following)
{
    const playerState_t *ps       = &cl.snap.ps;
    const int            owned    = ps->stats[STAT_WEAPONS] & 0x3F;
    const int            equipped = ps->stats[STAT_EQUIPPED_WEAPON] & 0x3F;
    const compWeapon_t  *w        = Comp_Weapon(inHand);
    const qboolean       grenade  = (equipped & WEAPON_CLASS_GRENADE) != 0;
    const int            follow   = (ps->pm_flags & PMF_SPECTATING) ? ps->stats[STAT_INFOCLIENT] : -2;
    int                  frags = 0, smokes = 0, i;

    if (follow != comp.follow) {
        comp.primary[0] = comp.sidearm[0] = comp.pending[0] = 0;
        comp.loaded[0] = comp.loaded[1] = -1;
        comp.follow                     = follow;
    }
    if (w && w->nation != NAT_ANY) {
        comp.nation = w->nation;
    } else if (com_target_game->integer < TG_MOHTA || !comp.nation) {
        comp.nation = ps->stats[STAT_TEAM] == TEAM_AXIS ? NAT_DE : NAT_US;
    }
    if (Comp_Weapon(comp.pending)) {
        Q_strncpyz(Comp_Weapon(comp.pending)->kind == WK_SIDEARM ? comp.sidearm : comp.primary, comp.pending, 64);
        comp.pending[0] = 0;
    }
    if (w && w->kind == WK_PRIMARY) {
        Q_strncpyz(comp.primary, w->name, sizeof(comp.primary));
    } else if (w && w->kind == WK_SIDEARM) {
        Q_strncpyz(comp.sidearm, w->name, sizeof(comp.sidearm));
    }
    if (!(owned & ~(WEAPON_CLASS_PISTOL | WEAPON_CLASS_GRENADE))) {
        comp.primary[0] = 0;
    }
    if (!(owned & WEAPON_CLASS_PISTOL)) {
        comp.sidearm[0] = 0;
    } else if (!comp.sidearm[0]) {
        Q_strncpyz(comp.sidearm, comp_issued[comp.nation][0], sizeof(comp.sidearm));
    }

    // the grenades: the pouch's (by the ammo's name), and the one a grenade
    // keeps loaded, which a spawn or a round gives back
    for (i = 0; i < MAX_AMMO; i++) {
        const char *name = ps->ammo_name_index[i] > 0 ? CL_ConfigString(ps->ammo_name_index[i]) : "";

        if (Q_stristr(name, "smoke")) {
            smokes = Q_max(smokes, ps->ammo_amount[i]);
        } else if ((Q_stristr(name, "grenade") && !Q_stristr(name, "riflegrenade")) || !Q_stricmp(name, "agrenade")) {
            frags = Q_max(frags, ps->ammo_amount[i]);
        }
    }
    if (ps->stats[STAT_HEALTH] <= 0 || (ps->pm_flags & PMF_RESPAWNED) || strcmp(comp.wbRound, CL_ConfigString(CS_WARMUP))) {
        comp.loaded[0] = comp.loaded[1] = -1;
        Q_strncpyz(comp.wbRound, CL_ConfigString(CS_WARMUP), sizeof(comp.wbRound));
    } else if (grenade) {
        comp.loaded[w && w->kind == WK_SMOKE] = ps->stats[STAT_CLIPAMMO] > 0;
    }
    // Allied Assault's start ammo loads one, Spearhead's and Breakthrough's none
    frags += comp.loaded[0] >= 0 ? comp.loaded[0] : (owned & WEAPON_CLASS_GRENADE) && com_target_game->integer < TG_MOHTA;
    smokes += Q_max(0, comp.loaded[1]);
    if (grenade) {
        // in hand, the ammo stat is the pouch and the loaded one
        int *n = w && w->kind == WK_SMOKE ? &smokes : &frags;

        *n = Q_max(*n, ps->stats[STAT_AMMO]);
    }
    comp.frags  = (owned & WEAPON_CLASS_GRENADE) ? frags : 0;
    comp.smokes = (owned & WEAPON_CLASS_GRENADE) ? smokes : 0;

    // the charge: the charge animation's rising edge with a grenade in hand
    if (grenade && ps->iViewModelAnim == VM_ANIM_CHARGE) {
        if (!comp.charging) {
            const usercmd_t *cmd = &cl.cmds[cl.cmdNumber & CMD_MASK];

            comp.charging        = qtrue;
            comp.chargeStart     = cl.serverTime;
            comp.chargeSecondary = (cmd->buttons & BUTTON_ATTACKRIGHT) && !(cmd->buttons & BUTTON_ATTACKLEFT);
        }
    } else {
        comp.charging = qfalse;
    }
}

static const char *Comp_SpectatorText(void)
{
    const int pmFlags = cl.snap.ps.pm_flags;
    const int team    = cl.snap.ps.stats[STAT_TEAM];
    int       key1, key2;

    if (!(pmFlags & PMF_SPECTATING)) {
        return "";
    }
    Key_GetKeysForCommand(team != TEAM_ALLIES && team != TEAM_AXIS && com_target_game->integer >= TG_MOHTA ? "+attackprimary" : "+use", &key1, &key2);
    if (com_target_game->integer >= TG_MOHTA) {
        if (team != TEAM_ALLIES && team != TEAM_AXIS) {
            return Sys_LV_CL_ConvertString(va("Press Fire(%s) to join the battle!", Key_KeynumToBindString(key1)));
        }
        return Sys_LV_CL_ConvertString(va(
            (pmFlags & PMF_CAMERA_VIEW) ? "Press Use(%s) to enter free spectate mode." : "Press Use(%s) to enter player following spectate mode.",
            Key_KeynumToBindString(key1)
        ));
    }
    return Sys_LV_CL_ConvertString(va(
        (pmFlags & PMF_CAMERA_VIEW) ? "Press Use(%s) to follow a different player." : "Press Use(%s) to follow a player.",
        Key_KeynumToBindString(key1)
    ));
}

static const char *const comp_arcAlpha[3] = {"ui_hud_arc0_alpha", "ui_hud_arc1_alpha", "ui_hud_arc2_alpha"};
static const char *const comp_arcAngle[3] = {"ui_hud_arc0_angle", "ui_hud_arc1_angle", "ui_hud_arc2_angle"};
static const char *const comp_objOn[3]    = {"ui_hud_obj0_on", "ui_hud_obj1_on", "ui_hud_obj2_on"};
static const char *const comp_obj[3]      = {"ui_hud_obj0", "ui_hud_obj1", "ui_hud_obj2"};

// the angle on screen (clockwise, up the view) of a world yaw
static float Comp_ScreenAngle(float viewYaw, float yaw)
{
    return AngleSubtract(viewYaw, yaw);
}

void UI_CompHud_Frame(void)
{
    const playerState_t *ps       = &cl.snap.ps;
    const int            gametype = Comp_Gametype();
    const qboolean       teams    = gametype >= GT_TEAM;
    const qboolean       rounds   = gametype == GT_TEAM_ROUNDS || gametype == GT_OBJECTIVE;
    const qboolean       spectating = (ps->pm_flags & PMF_SPECTATING) != 0;
    const qboolean       following = spectating && (ps->pm_flags & PMF_CAMERA_VIEW) && ps->stats[STAT_INFOCLIENT] >= 0
                                && ps->stats[STAT_INFOCLIENT] < MAX_CLIENTS;
    const char *weapon = ps->activeItems[ITEM_WEAPON] >= 0 && ps->activeItems[ITEM_WEAPON] < MAX_WEAPONS
                           ? CL_ConfigString(CS_WEAPONS + ps->activeItems[ITEM_WEAPON])
                           : "";
    char        spec[256];
    const char *banner1 = "";
    const int   team   = ps->stats[STAT_TEAM];
    const int   attacker = ps->stats[STAT_ATTACKERCLIENT];
    vec3_t      angles;
    float       viewYaw;
    int         health = ps->stats[STAT_HEALTH], maxHealth = Q_max(1, ps->stats[STAT_MAXHEALTH]);
    int         i;

    if (!cl.snap.valid) {
        return;
    }
    // (copied: va's buffer is used again below)
    Q_strncpyz(spec, Comp_Int("cg_hud_spectator") ? Comp_SpectatorText() : "", sizeof(spec));
    cge->CG_EyeAngles(&angles);
    viewYaw = angles[YAW];
    Comp_Roster(gametype);
    Comp_Weapons(weapon, following);

    // the strip: health, badge, ammo, weapons (not while free spectating)
    Comp_Set("ui_hud_strip", !spectating || following ? "1" : "0");
    {
        const qboolean icon = teams && (!spectating || following) && Comp_Int("cg_hud_badge") && Comp_Int("cg_hud_teamicon");

        Comp_Set("ui_hud_allies", icon && team == TEAM_ALLIES ? "1" : "0");
        Comp_Set("ui_hud_axis", icon && team == TEAM_AXIS ? "1" : "0");
    }
    if (following && !(ps->stats[STAT_WEAPONS] & 0x3F) && ps->stats[STAT_INFOCLIENT_HEALTH] > 0) {
        health    = ps->stats[STAT_INFOCLIENT_HEALTH];
        maxHealth = 100;
    }
    Comp_Set("ui_hud_health", va("%d", health));
    Comp_Set("ui_hud_health_col", health < Comp_Cvar("cg_hud_lowhealth")->value * maxHealth ? "0.753 0.188 0.129 1" : "0.965 0.965 0.478 1");
    Comp_Set("ui_hud_ammo_on", weapon[0] ? "1" : "0");
    Comp_Set("ui_hud_clip", va("%d", ps->stats[STAT_CLIPAMMO]));
    Comp_Set("ui_hud_ammo", va("%d", ps->stats[STAT_AMMO]));

    // the score panel: Allies | timer | Axis, or self | timer | the leader
    {
        const int matchEnd = atoi(CL_ConfigString(CS_MATCHEND));
        int       score[2] = {ps->stats[STAT_KILLS], ps->stats[STAT_HIGHEST_SCORE]};

        if (teams) {
            const int mine = team == TEAM_AXIS;

            score[0] = comp.sbScore[0];
            score[1] = comp.sbScore[1];
            if (team == TEAM_ALLIES || team == TEAM_AXIS) {
                score[mine] = ps->stats[STAT_KILLS];
                if (ps->stats[STAT_HIGHEST_SCORE] > ps->stats[STAT_KILLS]) {
                    score[!mine] = ps->stats[STAT_HIGHEST_SCORE];
                }
            }
        }
        Comp_Set("ui_hud_score_l", score[0] >= 0 ? va("%d", score[0]) : "");
        Comp_Set("ui_hud_score_r", score[1] >= 0 ? va("%d", score[1]) : "");
        Comp_Set("ui_hud_score_lc", teams ? "0.804 0.937 0.98 1" : "0.988 0.988 0.988 1");
        Comp_Set("ui_hud_score_rc", teams ? "0.961 0.812 0.784 1" : "0.988 0.988 0.988 1");
        if (atoi(CL_ConfigString(CS_WARMUP)) != -1 && matchEnd && gametype != GT_LIBERATION && matchEnd > cl.serverTime) {
            const int s = (matchEnd - cl.serverTime) / 1000;

            Comp_Set("ui_hud_timer", va("%d:%02d", s / 60, s % 60));
            banner1 = "";
        } else {
            Comp_Set("ui_hud_timer", "");
            banner1 = Comp_Int("cg_hud_timer") ? Sys_LV_CL_ConvertString(Comp_Cvar("ui_timemessage")->string) : "";
        }
        if (rounds) {
            const int alive[2] = {(int)strlen(comp.cards[0]) - (int)(strchr(comp.cards[0], 'd') ? strlen(strchr(comp.cards[0], 'd')) : 0),
                                  (int)strlen(comp.cards[1]) - (int)(strchr(comp.cards[1], 'd') ? strlen(strchr(comp.cards[1], 'd')) : 0)};

            Comp_Set("ui_hud_vs", va("%d vs %d", alive[0], alive[1]));
        } else {
            Comp_Set("ui_hud_vs", "");
        }
        Comp_Set("ui_hud_cards_allies", rounds ? comp.cards[0] : "");
        Comp_Set("ui_hud_cards_axis", rounds ? comp.cards[1] : "");
    }
    // the banners, the time message's first
    Comp_Set("ui_hud_banner1", banner1[0] ? banner1 : spec);
    Comp_Set("ui_hud_banner2", banner1[0] ? spec : "");
    Comp_Set("ui_hud_banner1_on", banner1[0] || spec[0] ? "1" : "0");
    Comp_Set("ui_hud_banner2_on", banner1[0] && spec[0] ? "1" : "0");

    // the stopwatch (Omaha's bar outlasts the wait by a quarter second) and the grenade's charge
    {
        const stopWatch_t *sw = &cls.stopwatch;
        float              frac = 0.0f;

        if (sw->iStartTime && sw->iStartTime < sw->iEndTime && ps->stats[STAT_HEALTH] > 0) {
            frac = sw->eType >= SWT_FUSE_WET ? (sw->iEndTime > cl.serverTime)
                 : Com_Clamp(0.0f, 1.0f, (float)(sw->iEndTime + 250 - cl.serverTime) / (sw->iEndTime - sw->iStartTime + 250));
        }
        Comp_SetF("ui_hud_stopwatch", frac);
        if (comp.charging) {
            // the stock scripts' charge: 0.6 to 4 s, 0.5 to 1.5 for the secondary throw
            const float lo = comp.chargeSecondary ? 0.5f : 0.6f, hi = comp.chargeSecondary ? 1.5f : 4.0f;

            Comp_SetF("ui_hud_charge", Com_Clamp(0.0f, 1.0f, ((cl.serverTime - comp.chargeStart) / 1000.0f - lo) / (hi - lo)));
        }
    }

    // who killed you, the player aimed at, who you follow, who last hurt you
    {
        const qboolean killer = health <= 0 && !spectating && attacker >= 0 && attacker < MAX_CLIENTS && attacker != ps->clientNum;
        const int      info   = ps->stats[STAT_INFOCLIENT], infoHealth = ps->stats[STAT_INFOCLIENT_HEALTH];
        const qboolean target = info >= 0 && info < MAX_CLIENTS && infoHealth > 0 && !following;

        Comp_Set("ui_hud_killer", killer ? Comp_Name(attacker) : "");
        Comp_Set("ui_hud_killer_col", killer ? Comp_TeamColor(Comp_Team(attacker)) : "1 1 1 1");
        Comp_Set("ui_hud_target", target ? Comp_Name(info) : "");
        Comp_Set("ui_hud_target_col", target ? Comp_TeamColor(Comp_Team(info)) : "1 1 1 1");
        Comp_SetF("ui_hud_target_hp", target ? Com_Clamp(0, 100, infoHealth) / 100.0f : 0.0f);
        Comp_Set("ui_hud_target_hpcol", infoHealth < 40 ? "0.753 0.188 0.129 1" : "1 1 1 0.612");
        Comp_Set("ui_hud_follow", following ? Comp_Name(info) : "");
        Comp_Set("ui_hud_follow_col", following ? Comp_TeamColor(Comp_Team(info)) : "1 1 1 1");
        Comp_Set("ui_hud_hurt", attacker >= 0 && attacker < MAX_CLIENTS ? Comp_Name(attacker) : "");
        Comp_Set("ui_hud_hurt_col", Comp_TeamColor(Comp_Team(attacker)));
    }

    // a vote
    {
        const int voteTime = atoi(CL_ConfigString(CS_VOTE_TIME));

        Comp_Set("ui_hud_vote_on", voteTime ? "1" : "0");
        if (voteTime) {
            Comp_Set("ui_hud_vote_time", va("%ds", Q_max(0, (30000 - (cl.serverTime - voteTime)) / 1000 + 1)));
            static UIFont *font;
            const char    *p = CL_ConfigString(CS_VOTE_STRING);
            str            text;

            // the subject wrapped in the panel (two lines)
            const float    vs[2] = {UI_DesignScale(1920.0f, 1080.0f), UI_DesignScale(1920.0f, 1080.0f)};

            if (!font) {
                font = new UIFont("oswald-medium-16s");
            }
            font->setVirtualScale(vs);
            for (int line = 0; *p && line < 2; line++) {
                const char *brk = NULL, *q = p;

                for (; *q; q++) {
                    if (*q == ' ') {
                        brk = q;
                    }
                    if (q > p && font->getWidth(p, (int)(q - p + 1)) > 252.0f) {
                        break;
                    }
                }
                if (*q && brk && brk > p && line == 0) {
                    q = brk;
                }
                text += str(p, 0, (int)(q - p));
                text += line == 0 && *q ? "\n" : "";
                for (p = q; *p == ' '; p++) {
                }
            }
            Comp_Set("ui_hud_vote_text", text);
            Comp_Set("ui_hud_vote_tally", va("%s  %s     %s  %s", Sys_LV_CL_ConvertString("Yes"), CL_ConfigString(CS_VOTE_YES),
                                             Sys_LV_CL_ConvertString("No"), CL_ConfigString(CS_VOTE_NO)));
            Comp_Set("ui_hud_vote_prompt", ps->voted ? "" : Sys_LV_CL_ConvertString("To vote Yes, press F1. To vote No, press F2."));
        }
    }

    // the damage: three crescents round the crosshair, a hit renewing the one
    // within 30 degrees or the oldest, 1.5 s each; the radar's and compass's wedge
    {
        const int dmg = ps->stats[STAT_DAMAGEDIR];

        if (dmg != comp.lastDmg || (dmg > 0 && ps->stats[STAT_HEALTH] < comp.lastHealth)) {
            int slot = 0;

            for (i = 0; i < 3; i++) {
                if (comp.arcTime[i] && fabs(AngleSubtract(dmg / 10.0f, comp.arcStat[i] / 10.0f)) < 30.0f) {
                    slot = i;
                    break;
                }
                if (comp.arcTime[i] < comp.arcTime[slot]) {
                    slot = i;
                }
            }
            if (comp.lastDmg != -999999) {
                comp.arcStat[slot] = dmg;
                comp.arcTime[slot] = comp.flashTime = cls.realtime;
            }
        }
        comp.lastDmg    = dmg;
        comp.lastHealth = ps->stats[STAT_HEALTH];
        for (i = 0; i < 3; i++) {
            const int age = cls.realtime - comp.arcTime[i];

            Comp_SetF(comp_arcAlpha[i], comp.arcTime[i] && age < 1500 ? 1.0f - age / 1500.0f : 0.0f);
            Comp_SetF(comp_arcAngle[i], Comp_ScreenAngle(viewYaw, comp.arcStat[i] / 10.0f + 180.0f));
        }
        Comp_SetF("ui_hud_dmg_alpha", dmg > 0 && comp.flashTime ? Com_Clamp(0.0f, 1.0f, 1.0f - (cls.realtime - comp.flashTime) / 1500.0f) : 0.0f);
        Comp_SetF("ui_hud_dmg_angle", Comp_ScreenAngle(viewYaw, dmg / 10.0f + 180.0f));
    }

    // the elements' flags (enabledcvar): shown and turned on
    {
        const qboolean strip = !(ps->pm_flags & PMF_SPECTATING) || following;
        const qboolean radar = Comp_Int("cg_hud_radar") != 0;
        const qboolean msgs  = Comp_Int("cg_hud_messages") != 0;
        auto           flag  = [](const char *name, qboolean on) { Comp_Set(name, on ? "1" : "0"); };

        flag("ui_hud_radar_on", radar);
        flag("ui_hud_compass_on", !radar && Comp_Int("ui_compass"));
        flag("ui_hud_msgs_r", msgs && radar);
        flag("ui_hud_msgs_c", msgs && !radar);
        flag("ui_hud_score_on", Comp_Int("cg_hud_score") && gametype != GT_SINGLE_PLAYER);
        flag("ui_hud_vs_on", rounds && Comp_Int("cg_hud_alive"));
        // the panel with the other team's score, or without it (not known yet): narrower
        flag("ui_hud_p3", Comp_Cvar("ui_hud_score_r")->string[0] != 0);
        flag("ui_hud_health_on", strip && Comp_Int("cg_hud_health"));
        flag("ui_hud_badge_on", strip && Comp_Int("cg_hud_badge"));
        flag("ui_hud_ammo_on", strip && weapon[0] && Comp_Int("cg_hud_ammo"));
        flag("ui_hud_weapons_on", strip && Comp_Int("cg_hud_ammo"));
        flag("ui_hud_killer_on", Comp_Cvar("ui_hud_killer")->string[0] && Comp_Int("cg_hud_killer"));
        flag("ui_hud_target_on", Comp_Cvar("ui_hud_target")->string[0] && Comp_Int("cg_hud_targetinfo"));
        flag("ui_hud_follow_on", following && Comp_Int("cg_hud_following"));
        flag("ui_hud_hurt_on", Comp_Cvar("ui_hud_hurt")->string[0] && Comp_Int("cg_hud_attacker"));
        flag("ui_hud_charge_on", comp.charging && Comp_Int("cg_hud_grenadecharge"));
        flag("ui_hud_vote_on", atoi(CL_ConfigString(CS_VOTE_TIME)) && Comp_Int("cg_hud_vote"));
        if (!Comp_Int("cg_hud_stopwatch")) {
            Comp_Set("ui_hud_stopwatch", "0");
        }
        if (!Comp_Int("cg_hud_damage")) {
            for (i = 0; i < 3; i++) {
                Comp_Set(comp_arcAlpha[i], "0");
            }
            Comp_Set("ui_hud_dmg_alpha", "0");
        }
    }

    // the compass: its bearings turn with the view, the objectives' balls on its rim
    {
        const int stats[3] = {ps->stats[STAT_OBJECTIVECENTER], ps->stats[STAT_OBJECTIVELEFT], ps->stats[STAT_OBJECTIVERIGHT]};
        const int rest[3]  = {1800, 1730, 1870};

        Comp_SetF("ui_hud_north", anglemod(viewYaw - ps->stats[STAT_COMPASSNORTH] / 182.0f));
        for (i = 0; i < 3; i++) {
            Comp_Set(comp_objOn[i], stats[i] != rest[i] ? "1" : "0");
            Comp_SetF(comp_obj[i], (stats[i] - 1800) / 10.0f);
        }
    }
}

void UI_CompHud_Init(void)
{
    cg_hudstyle = Cvar_Get("cg_hudstyle", "0", CVAR_ARCHIVE);
    for (size_t i = 0; i < ARRAY_LEN(comp_toggles); i++) {
        Cvar_Get(comp_toggles[i], "1", CVAR_ARCHIVE);
    }
    Cvar_Get("cg_hud_lowhealth", "0.4", CVAR_ARCHIVE);
    UI_CompHud_Clear();
}

qboolean UI_CompHud_Active(void)
{
    return cg_hudstyle && cg_hudstyle->integer == 1;
}

/*
====================================================================

WIDGETS

====================================================================
*/

// a picture's texels across and down (read once)
static void Comp_PictureSize(const char *pic, int *w, int *h)
{
    static std::vector<std::pair<str, std::pair<int, int>>> sizes;
    byte                                                    *data;

    for (auto& e : sizes) {
        if (e.first == pic) {
            *w = e.second.first;
            *h = e.second.second;
            return;
        }
    }
    *w = *h = 1;
    if (uii.Rend_LoadRawImage(va(COMP_PICTURES "%s.png", pic), &data, w, h) && data) {
        uii.Rend_FreeRawImage(data);
    }
    sizes.push_back({str(pic), {*w, *h}});
}

// a picture baked at its size (w x h design pixels at x y), tinted
static void Comp_Picture(const char *pic, float x, float y, float w, float h, float s, const char *align, qboolean mirror, const UColor& c)
{
    char         layer[MAX_QPATH * 2];
    const char  *layers[1] = {layer};
    const int    pw = (int)(w * s + 0.5f), ph = (int)(h * s + 0.5f);
    const vec4_t color = {c.r, c.g, c.b, c.a};
    qhandle_t    shader;

    Com_sprintf(layer, sizeof(layer), "picture 1 1 1 1 " COMP_PICTURES "%s.png %s%s", pic, align, mirror ? " mirror" : "");
    shader = UI_BakeShape("comp", layers, 1, pw, ph, s);
    uii.Rend_SetColor(color);
    uii.Rend_DrawPicStretched(floorf(x * s + 0.5f), floorf(y * s + 0.5f), pw, ph, 0, 0, 1, 1, shader);
}

// text with its capitals' middle on cy (the baked faces' lines are centered so)
static float Comp_Text(UIFont *font, float x, float cy, const char *text, const UColor& c, const float *scale)
{
    font->setColor(c);
    font->Print(x, cy - font->getHeight((const float *)NULL) * 0.5f, text, -1, scale);
    return font->getWidth(text, -1);
}

static float Comp_Fade(int time, int life, int fade)
{
    return Com_Clamp(0.0f, 1.0f, (float)(life - (cls.realtime - time)) / fade);
}

/*
================
UIHudFeed

The kill feed (feed kills: right aligned, the newest at the bottom, 6 s,
fading in the last 1.5), the chat (feed chat: from the bottom up) and the
game messages (feed messages: from the top), 8 s fading in the last 2;
lines wrapped between words, the newest that fit. boldfont: the white
game messages'
================
*/
class UIHudFeed : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudFeed);

    int     m_kind; // 0 kills, 1 chat, 2 messages
    UIFont *m_bold;

    UIHudFeed() : m_kind(2), m_bold(NULL) {}
    void SetFeed(Event *ev);
    void SetBoldFont(Event *ev);
    void Draw(void) override;
    void DrawKills(float w, float h, float s);
};

Event EV_HudFeed_Feed("feed", EV_DEFAULT, "s", "kind", "kills, chat or messages");
Event EV_HudFeed_BoldFont("boldfont", EV_DEFAULT, "s", "font", "the bold lines' font");

CLASS_DECLARATION(UIWidget, UIHudFeed, NULL) {
    {&EV_HudFeed_Feed,     &UIHudFeed::SetFeed    },
    {&EV_HudFeed_BoldFont, &UIHudFeed::SetBoldFont},
    {NULL,                 NULL                   }
};

void UIHudFeed::SetFeed(Event *ev)
{
    const str kind = ev->GetString(1);

    m_kind = kind == "kills" ? 0 : kind == "chat" ? 1 : 2;
}

void UIHudFeed::SetBoldFont(Event *ev)
{
    m_bold = new UIFont(ev->GetString(1));
}

void UIHudFeed::DrawKills(float w, float h, float s)
{
    const float lineH = m_font->getHeight((const float *)NULL), gap = 6.0f;
    float       used = 0.0f, y = 0.0f;
    int         first, i, count = comp.numKills, oldest = 0;

    while (oldest < count && cls.realtime - comp.kills[oldest].time >= 6000) {
        oldest++;
    }
    for (first = count; first > oldest; first--) {
        const float rowH = Q_max(lineH, comp.kills[first - 1].picH) + (used > 0.0f ? 3.0f : 0.0f);

        if (used + rowH > h) {
            break;
        }
        used += rowH;
    }
    for (i = first; i < count; i++) {
        const compKill_t *k     = &comp.kills[i];
        const float       alpha = Comp_Fade(k->time, 6000, 1500);
        const float       rowH  = Q_max(lineH, k->picH), mid = y + rowH * 0.5f;
        const qboolean    mark  = k->suicide && strcmp(k->pic, "skull_50");
        UColor            kc, vc, white(1, 1, 1, alpha);
        float             x;

        sscanf(Comp_TeamColor(k->killerTeam), "%f %f %f", &kc.r, &kc.g, &kc.b);
        sscanf(Comp_TeamColor(k->victimTeam), "%f %f %f", &vc.r, &vc.g, &vc.b);
        kc.a = vc.a = alpha;
        x = w - m_font->getWidth(k->victim, -1) - k->picW - gap - (mark ? 18.0f + gap : 0.0f) - (k->headshot ? 18.0f + gap : 0.0f)
          - (k->killer[0] ? m_font->getWidth(k->killer, -1) + gap : 0.0f);
        if (k->killer[0]) {
            x += Comp_Text(m_font, x, mid, k->killer, kc, getVirtualScale()) + gap;
        }
        // the pictures point left: the feed's at the victim
        Comp_Picture(k->pic, x, mid - k->picH * 0.5f, k->picW, k->picH, s, "center", strcmp(k->pic, "skull_50") && strcmp(k->pic, "glove_50"), white);
        x += k->picW + gap;
        if (mark) {
            Comp_Picture("skull_50", x, mid - 9.0f, 18.0f, 18.0f, s, "center", qfalse, white);
            x += 18.0f + gap;
        }
        if (k->headshot) {
            Comp_Picture("headshot_50", x, mid - 9.0f, 18.0f, 18.0f, s, "center", qfalse, white);
            x += 18.0f + gap;
        }
        Comp_Text(m_font, x, mid, k->victim, vc, getVirtualScale());
        y += rowH + 3.0f;
    }
}

void UIHudFeed::Draw(void)
{
    const float       s = m_vVirtualScale[0], w = m_frame.size.width / s, h = m_frame.size.height / s;
    const compLine_t *lines = m_kind == 1 ? comp.chat : comp.msgs;
    const int         count = m_kind == 1 ? comp.numChat : comp.numMsgs;
    std::vector<str>  rows;
    std::vector<int>  rowLine;
    float             lineH = m_font->getHeight((const float *)NULL), y;
    int               i;

    if (m_bold) {
        m_bold->setVirtualScale(getVirtualScale());
    }
    if (!m_kind) {
        DrawKills(w, h, s);
        return;
    }
    // the newest lines that fit, wrapped
    for (i = count - 1; i >= 0 && cls.realtime - lines[i].time < 8000; i--) {
        UIFont          *font = lines[i].bold && m_bold ? m_bold : m_font;
        std::vector<str> wrapped;
        const char      *p = lines[i].text;

        while (*p) {
            const char *brk = NULL, *q = p;
            float       width = 0.0f;

            for (; *q; q++) {
                if (*q == ' ') {
                    brk = q;
                }
                width += font->getCharWidth((byte)*q);
                if (q > p && width > w) {
                    break;
                }
            }
            if (*q && brk && brk > p) {
                q = brk;
            }
            wrapped.push_back(str(p, 0, (int)(q - p)));
            for (p = q; *p == ' '; p++) {
            }
        }
        if ((rows.size() + wrapped.size()) * lineH + 2.0f > h) {
            break;
        }
        rows.insert(rows.begin(), wrapped.begin(), wrapped.end());
        rowLine.insert(rowLine.begin(), wrapped.size(), i);
    }
    y = m_kind == 1 ? h - rows.size() * lineH : 0.0f;
    for (size_t r = 0; r < rows.size(); r++) {
        const compLine_t *l = &lines[rowLine[r]];
        const UColor      c(l->color[0], l->color[1], l->color[2], Comp_Fade(l->time, 8000, 2000));

        Comp_Text(l->bold && m_bold ? m_bold : m_font, 0.0f, y + lineH * 0.5f, rows[r], c, getVirtualScale());
        y += lineH;
        if (r + 1 < rows.size() && rowLine[r + 1] != rowLine[r]) {
            y += 2.0f;
        }
    }
}

/*
================
UIHudCards

A card for each player of a team (team allies or axis: ui_hud_cards_<team>,
alive first, then dead), laid out from the widget's right edge leftward
(side left) or its left edge rightward: the team's icon (icons: the widgets
whose shapes are the Allies' and the Axis' icons), framed yellow-green for
you, a white bar under it while alive; a skull once dead. They shrink so
the bigger team fits in about 400 px and in the widget, the rest a count
================
*/
class UIHudCards : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudCards);

    int       m_team;
    str       m_icons[2];
    int       m_size, m_stamp;
    qhandle_t m_cards[3]; // alive, you, dead

    UIHudCards() : m_team(TEAM_ALLIES), m_size(0), m_stamp(-1) {}
    void SetTeam(Event *ev) { m_team = !str::icmp(ev->GetString(1), "axis") ? TEAM_AXIS : TEAM_ALLIES; }
    void SetIcons(Event *ev)
    {
        m_icons[0] = ev->GetString(1);
        m_icons[1] = ev->GetString(2);
    }
    void Draw(void) override;
};

Event EV_HudCards_Team("team", EV_DEFAULT, "s", "team", "allies or axis");
Event EV_HudCards_Icons("icons", EV_DEFAULT, "ss", "allies axis", "the widgets whose shapes are the teams' icons");

CLASS_DECLARATION(UIWidget, UIHudCards, NULL) {
    {&EV_HudCards_Team,  &UIHudCards::SetTeam },
    {&EV_HudCards_Icons, &UIHudCards::SetIcons},
    {NULL,               NULL                 }
};

void UIHudCards::Draw(void)
{
    const float s     = m_vVirtualScale[0], w = m_frame.size.width / s;
    const char *cards = Cvar_VariableString(m_team == TEAM_AXIS ? "ui_hud_cards_axis" : "ui_hud_cards_allies");
    const int   n     = (int)strlen(cards);
    const int   most  = Q_max(1, Q_max((int)strlen(Cvar_VariableString("ui_hud_cards_allies")), (int)strlen(Cvar_VariableString("ui_hud_cards_axis"))));
    int         size  = Q_max(20, Q_min(48, 400 / most - 4)), fit, i;

    if (!n) {
        return;
    }
    if ((size + 4) * most > w) {
        size = Q_max(8, (int)(w / most) - 4);
    }
    fit = Q_min(n, Q_max(1, (int)(w / (size + 4))));
    if (fit < n) {
        fit--;
    }
    // a card of each kind (alive, you, dead) baked once at its size: the
    // box, its frame, the team's icon or a skull, the bar under it
    if (size != m_size || m_stamp != UI_BakeStamp()) {
        UIWidget   *icon = m_parent ? m_parent->FindWidget(m_icons[m_team == TEAM_AXIS]) : NULL;
        const float f    = size / (size + 8.0f), bar = (size + 4.0f) / (size + 8.0f);
        const float px = 5.0f / size, py = 5.0f / (size + 8.0f), dx = 6.0f / size, dy = 6.0f / (size + 8.0f);

        for (int k = 0; k < 3; k++) {
            std::vector<str> lines;
            const char      *layers[40];
            int              numLayers = 0;

            if (k == 2) {
                lines.push_back(va("in 0 0 1 %g fill 0.11 0.125 0.141 0.702", f));
                lines.push_back(va("in 0 0 1 %g frame 1 1 1 1 0.2", f));
                if (size >= 16) {
                    lines.push_back(va("in %g %g %g %g picture 1 1 1 0.5 " COMP_PICTURES "skull_50.png center", dx, dy, 1.0f - dx, f - dy));
                }
            } else {
                lines.push_back(va("in 0 0 1 %g fill %s", f, m_team == TEAM_AXIS ? "0.549 0.333 0.294 0.851" : "0.294 0.435 0.549 0.851"));
                for (int l = 1; size >= 16 && icon && l <= icon->getShapes().NumObjects(); l++) {
                    lines.push_back(va("in %g %g %g %g %s", px, py, 1.0f - px, f - py, icon->getShapes().ObjectAt(l).c_str()));
                }
                lines.push_back(va("in 0 0 1 %g frame %d %s", f, k ? 2 : 1, k ? "0.812 0.875 0.49 1" : "1 1 1 0.2"));
                lines.push_back(va("in 0 %g 1 1 fill 0.988 0.988 0.988 1", bar));
            }
            for (str& l : lines) {
                layers[numLayers++] = l.c_str();
            }
            m_cards[k] = UI_BakeShape("comp_card", layers, numLayers, (int)(size * s + 0.5f), (int)((size + 8) * s + 0.5f), s);
        }
        m_size  = size;
        m_stamp = UI_BakeStamp();
    }
    {
        const vec4_t white = {1, 1, 1, 1};

        uii.Rend_SetColor(white);
    }
    for (i = 0; i < fit; i++) {
        const float x = m_team == TEAM_AXIS ? 2.0f + i * (size + 4) : w - 2.0f - size - i * (size + 4);

        uii.Rend_DrawPicStretched(floorf(x * s + 0.5f), 0, (int)(size * s + 0.5f), (int)((size + 8) * s + 0.5f), 0, 0, 1, 1,
                                  m_cards[cards[i] == 'd' ? 2 : cards[i] == 'm' ? 1 : 0]);
    }
    if (fit < n) {
        const char *more = va("+%d", n - fit);

        Comp_Text(
            m_font,
            m_team == TEAM_AXIS ? 2.0f + fit * (size + 4) : w - 2.0f - fit * (size + 4) - m_font->getWidth(more, -1),
            size * 0.5f, more, UColor(0.988f, 0.988f, 0.988f, 1.0f), getVirtualScale()
        );
    }
}

/*
================
UIHudWeapons

The weapons carried, up from the widget's bottom right: the sidearm (1),
the primary (its slot), the grenades (5, frags then smokes, each with how
many), each with its key at the right; the one in hand bright and named
under it. countfont: the grenades' counts'
================
*/
class UIHudWeapons : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudWeapons);

    UIFont *m_countFont;

    UIHudWeapons() : m_countFont(NULL) {}
    void SetCountFont(Event *ev) { m_countFont = new UIFont(ev->GetString(1)); }
    void Item(float right, float *bottom, const char **pics, const int *counts, int n, int hand, const char *key, float boxW, float boxH);
    void Draw(void) override;
};

Event EV_HudWeapons_CountFont("countfont", EV_DEFAULT, "s", "font", "the grenades' counts' font");

CLASS_DECLARATION(UIWidget, UIHudWeapons, NULL) {
    {&EV_HudWeapons_CountFont, &UIHudWeapons::SetCountFont},
    {NULL,                     NULL                       }
};

void UIHudWeapons::Item(float right, float *bottom, const char **pics, const int *counts, int n, int hand, const char *key, float boxW, float boxH)
{
    const float  s      = m_vVirtualScale[0];
    const float  lineH  = m_font->getHeight((const float *)NULL);
    const UColor white(0.988f, 0.988f, 0.988f, 1.0f);
    float        pw[2], ph = 0.0f, w = 0.0f, top, x;
    int          i;

    // each picture's size fitted in the box, the row's width
    for (i = 0; i < n; i++) {
        int sw, sh;

        Comp_PictureSize(pics[i], &sw, &sh);
        pw[i] = floorf(Q_min(boxW / sw, boxH / sh) * sw * s + 0.5f) / s;
        ph    = Q_max(ph, floorf(Q_min(boxW / sw, boxH / sh) * sh * s + 0.5f) / s);
        w += pw[i] + (counts ? 4.0f + m_countFont->getWidth(va("%d", counts[i]), -1) : 0.0f) + (i + 1 < n ? (counts ? 8.0f : 0.0f) + 4.0f : 0.0f);
    }
    top = *bottom - ph - (hand >= 0 ? 8.0f + lineH : 0.0f);
    Comp_Text(m_font, m_frame.size.width / s - m_font->getWidth(key, -1), top - 8.0f + lineH * 0.5f, key, white, getVirtualScale());
    x = right - w;
    for (i = 0; i < n; i++) {
        const float alpha = hand == i ? 1.0f : 0.55f;

        Comp_Picture(pics[i], x, top, pw[i], ph, s, "center", qfalse, UColor(0.98f, 0.98f, 0.565f, alpha));
        x += pw[i];
        if (counts) {
            x += 4.0f;
            x += Comp_Text(m_countFont, x, top + ph - m_countFont->getHeight((const float *)NULL) * 0.5f, va("%d", counts[i]),
                           UColor(0.965f, 0.965f, 0.478f, alpha), getVirtualScale());
            x += 8.0f;
        }
        x += 4.0f;
    }
    if (hand >= 0) {
        const char *name = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[ITEM_WEAPON]);

        Comp_Text(m_font, right - m_font->getWidth(name, -1), top + ph + 8.0f + lineH * 0.5f, name, white, getVirtualScale());
    }
    *bottom = top - 14.0f;
}

void UIHudWeapons::Draw(void)
{
    static const int     slots[6] = {WEAPON_CLASS_PISTOL, WEAPON_CLASS_RIFLE, WEAPON_CLASS_SMG, WEAPON_CLASS_MG, WEAPON_CLASS_GRENADE, WEAPON_CLASS_HEAVY};
    const playerState_t *ps       = &cl.snap.ps;
    const int            owned    = ps->stats[STAT_WEAPONS] & 0x3F, equipped = ps->stats[STAT_EQUIPPED_WEAPON] & 0x3F;
    const float          s        = m_vVirtualScale[0];
    const float          right    = m_frame.size.width / s - 12.0f - 6.0f;
    const compWeapon_t  *inHand   = Comp_Weapon(CL_ConfigString(CS_WEAPONS + ps->activeItems[ITEM_WEAPON]));
    float                bottom   = m_frame.size.height / s;
    int                  handSlot = 0, primarySlot = 0, i;

    if (!m_countFont) {
        m_countFont = m_font;
    }
    m_countFont->setVirtualScale(getVirtualScale());
    for (i = 0; i < 6; i++) {
        if (!handSlot && (equipped & slots[i])) {
            handSlot = i + 1;
        }
        if (!primarySlot && (owned & slots[i]) && i != 0 && i != 4) {
            primarySlot = i + 1;
        }
    }
    if (comp.sidearm[0] && Comp_Weapon(comp.sidearm)) {
        const char *pic = Comp_Weapon(comp.sidearm)->pic;

        Item(right, &bottom, &pic, NULL, 1, handSlot == 1 ? 0 : -1, "1", 110.0f, 30.0f);
    }
    if (primarySlot && Comp_Weapon(comp.primary)) {
        const char *pic = Comp_Weapon(comp.primary)->pic;

        Item(right, &bottom, &pic, NULL, 1, handSlot == primarySlot ? 0 : -1, va("%d", primarySlot), 160.0f, 34.0f);
    }
    if (Comp_Int("cg_hud_grenades") && (comp.frags > 0 || comp.smokes > 0)) {
        const char *pics[2];
        int         counts[2], n = 0, hand = -1;

        for (i = 0; i < 2; i++) {
            if (i ? comp.smokes > 0 : comp.frags > 0) {
                if ((equipped & WEAPON_CLASS_GRENADE) && inHand && (inHand->kind == WK_SMOKE) == (i == 1)) {
                    hand = n;
                }
                pics[n]     = Comp_Weapon(comp_issued[comp.nation][1 + i])->pic;
                counts[n++] = i ? comp.smokes : comp.frags;
            }
        }
        Item(right, &bottom, pics, counts, n, hand, "5", 200.0f, 38.0f);
    }
}

/*
====================================================================

THE RADAR'S MAP

The floors a player can reach, walked from the spawn points over a grid of
16 units at the map's load (box traces down from a jump's height above each
floor); drawn top down into a picture once, again when the player changes
storey: each cell's floor nearest the player's height filled, brighter the
higher, its edges (no floor beside, or a ledge) as lines, the player's
storey's bright

====================================================================
*/

#define RM_CELL  16.0f
#define RM_RANGE 1000.0f // units from the radar's middle to its edge

typedef struct {
    float z;
    int   next; // the column's next floor, -1 none
} rmNode_t;

static struct {
    char                  map[MAX_QPATH];
    float                 x0, y0, zMin, zMax;
    int                   cw, ch;
    std::vector<int>      head; // a column's first floor, -1 none
    std::vector<rmNode_t> nodes;
    // the picture
    int       texels; // a cell's, across
    float     focusZ;
    qhandle_t shader;
    int       stamp;
} rm;

static qboolean RM_Floor(float x, float y, float top, float bottom, float *z)
{
    static const vec3_t mins = {-7, -7, 0}, maxs = {7, 7, 1};
    const vec3_t        start = {x, y, top}, end = {x, y, bottom};
    trace_t             tr;

    CM_BoxTrace(&tr, start, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse);
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal[2] < 0.7f) {
        return qfalse;
    }
    *z = tr.endpos[2];
    {
        // a crouched player has room there
        static const vec3_t cmins = {-15, -15, 18}, cmaxs = {15, 15, 50};

        CM_BoxTrace(&tr, tr.endpos, tr.endpos, cmins, cmaxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse);
    }
    return !tr.startsolid && !tr.allsolid;
}

// the column's floor within 24 units of z, else a new one; -1 when full
static int RM_Node(int c, float z, qboolean *added)
{
    *added = qfalse;
    for (int n = rm.head[c]; n >= 0; n = rm.nodes[n].next) {
        if (fabsf(rm.nodes[n].z - z) < 24.0f) {
            return n;
        }
    }
    if (rm.nodes.size() >= 400000) {
        return -1;
    }
    rm.nodes.push_back({z, rm.head[c]});
    rm.head[c] = (int)rm.nodes.size() - 1;
    *added     = qtrue;
    return rm.head[c];
}

static void RM_Walk(void)
{
    const char       *p = CM_EntityString();
    std::vector<int>  queue, cellOf;
    vec3_t            mins, maxs;
    qboolean          added;
    const int         start = Sys_Milliseconds();

    CM_ModelBounds(CM_InlineModel(0), mins, maxs);
    rm.x0 = mins[0];
    rm.y0 = mins[1];
    rm.cw = Q_min(2048, (int)((maxs[0] - mins[0]) / RM_CELL) + 1);
    rm.ch = Q_min(2048, (int)((maxs[1] - mins[1]) / RM_CELL) + 1);
    rm.head.assign(rm.cw * rm.ch, -1);
    rm.nodes.clear();
    rm.zMin = 99999.0f;
    rm.zMax = -99999.0f;
    // the spawn points
    for (const char *tok = COM_Parse((char **)&p); p && tok[0] == '{'; tok = COM_Parse((char **)&p)) {
        char   classname[64] = "";
        vec3_t origin        = {0, 0, 0};

        for (tok = COM_Parse((char **)&p); p && tok[0] != '}'; tok = COM_Parse((char **)&p)) {
            char key[64];

            Q_strncpyz(key, tok, sizeof(key));
            tok = COM_Parse((char **)&p);
            if (!Q_stricmp(key, "classname")) {
                Q_strncpyz(classname, tok, sizeof(classname));
            } else if (!Q_stricmp(key, "origin")) {
                sscanf(tok, "%f %f %f", &origin[0], &origin[1], &origin[2]);
            }
        }
        if (!Q_stricmpn(classname, "info_player_", 12) && Q_stricmp(classname, "info_player_intermission")) {
            const int cx = (int)((origin[0] - rm.x0) / RM_CELL), cy = (int)((origin[1] - rm.y0) / RM_CELL);
            float     z;

            if (cx >= 0 && cy >= 0 && cx < rm.cw && cy < rm.ch
                && RM_Floor(rm.x0 + (cx + 0.5f) * RM_CELL, rm.y0 + (cy + 0.5f) * RM_CELL, origin[2] + 40.0f, origin[2] - 200.0f, &z)) {
                const int n = RM_Node(cy * rm.cw + cx, z, &added);

                if (added) {
                    queue.push_back(n);
                    cellOf.push_back(cy * rm.cw + cx);
                }
            }
        }
    }
    // out from them: onto what a jump reaches, and down from any height
    for (size_t q = 0; q < queue.size(); q++) {
        const int   c = cellOf[q], cx = c % rm.cw, cy = c / rm.cw;
        const float z = rm.nodes[queue[q]].z;

        rm.zMin = Q_min(rm.zMin, z);
        rm.zMax = Q_max(rm.zMax, z);
        for (int d = 0; d < 4; d++) {
            const int nx = cx + (d == 0) - (d == 1), ny = cy + (d == 2) - (d == 3);
            float     nz;

            if (nx < 0 || ny < 0 || nx >= rm.cw || ny >= rm.ch
                || !RM_Floor(rm.x0 + (nx + 0.5f) * RM_CELL, rm.y0 + (ny + 0.5f) * RM_CELL, z + 72.0f, z - 4096.0f, &nz)) {
                continue;
            }
            const int n = RM_Node(ny * rm.cw + nx, nz, &added);

            if (added) {
                queue.push_back(n);
                cellOf.push_back(ny * rm.cw + nx);
            }
        }
    }
    Com_DPrintf("Radar: %d floors in %d ms\n", (int)rm.nodes.size(), Sys_Milliseconds() - start);
}

// a column's floor nearest z, -1 none
static int RM_Nearest(int cx, int cy, float z)
{
    int best = -1;

    if (cx < 0 || cy < 0 || cx >= rm.cw || cy >= rm.ch) {
        return -1;
    }
    for (int n = rm.head[cy * rm.cw + cx]; n >= 0; n = rm.nodes[n].next) {
        if (best < 0 || fabsf(rm.nodes[n].z - z) < fabsf(rm.nodes[best].z - z)) {
            best = n;
        }
    }
    return best;
}

static void RM_Picture(void)
{
    const int         t = rm.texels, w = rm.cw * t, h = rm.ch * t;
    std::vector<byte> px(w * h * 4, 0);
    const int         start = Sys_Milliseconds();

    for (int cy = 0; cy < rm.ch; cy++) {
        for (int cx = 0; cx < rm.cw; cx++) {
            const int n = RM_Nearest(cx, cy, rm.focusZ);
            float     z, focus;
            byte      fill, line, edge[4]; // left, right, north, south

            if (n < 0) {
                continue;
            }
            z     = rm.nodes[n].z;
            focus = Com_Clamp(0.0f, 1.0f, (120.0f - fabsf(z - rm.focusZ)) / 56.0f);
            fill  = (byte)((0.2f + 0.24f * (rm.zMax > rm.zMin ? (z - rm.zMin) / (rm.zMax - rm.zMin) : 1.0f)) * 255.0f);
            line  = (byte)((0.3f + 0.6f * focus) * 255.0f);
            // the edges: no floor beside within a step (a wall, a drop, a ledge)
            for (int k = 0; k < 4; k++) {
                const int nb = RM_Nearest(cx + (k == 1) - (k == 0), cy + (k == 2) - (k == 3), z);

                edge[k] = nb < 0 || fabsf(rm.nodes[nb].z - z) > 40.0f;
            }
            // the picture's top row is the map's north (y up)
            for (int j = 0; j < t; j++) {
                byte *o = &px[(((rm.ch - 1 - cy) * t + j) * w + cx * t) * 4];

                for (int i = 0; i < t; i++, o += 4) {
                    const int side = i == 0 ? 0 : i == t - 1 ? 1 : j == 0 ? 2 : j == t - 1 ? 3 : -1;

                    o[0] = 235;
                    o[1] = 240;
                    o[2] = 245;
                    o[3] = side >= 0 && edge[side] ? line : fill;
                }
            }
        }
    }
    rm.shader = uii.Rend_RegisterImage2D("ui_bake/radar", px.data(), w, h);
    rm.stamp  = UI_BakeStamp();
    Com_DPrintf("Radar: a picture %d x %d in %d ms\n", w, h, Sys_Milliseconds() - start);
}

void UI_CompHud_MapLoaded(void)
{
    const char *map = Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "mapname");

    if (!UI_CompHud_Active() || !Comp_Int("cg_hud_radar") || !strcmp(map, rm.map)) {
        return;
    }
    Q_strncpyz(rm.map, map, sizeof(rm.map));
    RM_Walk();
    rm.stamp = -1;
}

/*
================
UIHudRadar

The radar: the map turned with the view (up is where the player looks),
the objectives, the teammates the stock radar knows (the snapshot's and the
server's radar's: never enemies), the player's arrow, N E S W inside the
edge. Its disc and ring are its URC shapes
================
*/
class UIHudRadar : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudRadar);
    void Draw(void) override;
};

CLASS_DECLARATION(UIWidget, UIHudRadar, NULL) {
    {NULL, NULL}
};

void UIHudRadar::Draw(void)
{
    const float s = m_vVirtualScale[0], r = m_frame.size.width * 0.5f, k = m_frame.size.width / s / 220.0f;
    const float upp = RM_RANGE / r; // world units a pixel
    vec3_t      angles, eye;
    float       fx, fy, sn, cs;
    const char *map = Info_ValueForKey(CL_ConfigString(CS_SERVERINFO), "mapname");

    if (!cl.snap.valid) {
        return;
    }
    cge->CG_EyeAngles(&angles);
    cge->CG_EyePosition(&eye);
    sn = sinf(DEG2RAD(angles[YAW]));
    cs = cosf(DEG2RAD(angles[YAW]));
    // world offset to screen: up the view's forward, right its right
    auto place = [&](float wx, float wy, float *x, float *y) {
        const float dx = wx - eye[0], dy = wy - eye[1];

        fx = (dx * sn - dy * cs) / upp;
        fy = -(dx * cs + dy * sn) / upp;
        *x = fx;
        *y = fy;
    };

    // the map: walked once a map (at its load: UI_CompHud_MapLoaded), drawn
    // again a storey away or after a restart
    if (strcmp(map, rm.map)) {
        UI_CompHud_MapLoaded();
    }
    if (!rm.nodes.empty()) {
        const int texels = Q_max(1, Q_min(6, (int)(RM_CELL / upp + 0.5f)));

        if (rm.stamp != UI_BakeStamp() || texels != rm.texels || fabsf(eye[2] - 64.0f - rm.focusZ) > 24.0f) {
            rm.texels = Q_min(texels, 4096 / Q_max(rm.cw, rm.ch));
            rm.focusZ = eye[2] - 64.0f;
            RM_Picture();
        }
        // a fan of the circle's points, their texels the world's under them
        {
            const float  tw = rm.cw * RM_CELL, th = rm.ch * RM_CELL;
            const vec4_t white = {1, 1, 1, 1};
            vec2_t       pts[3], st[3];

            uii.Rend_SetColor(white);
            for (int i = 0; i < 48; i++) {
                for (int v = 0; v < 3; v++) {
                    const float a  = (i + (v == 2)) * 2.0f * M_PI / 48.0f;
                    const float px = v ? cosf(a) * r : 0.0f, py = v ? sinf(a) * r : 0.0f;
                    // the screen offset back to the world
                    const float wx = eye[0] + (px * sn - py * cs) * upp, wy = eye[1] + (-px * cs - py * sn) * upp;

                    pts[v][0] = r + px;
                    pts[v][1] = r + py;
                    st[v][0]  = (wx - rm.x0) / tw;
                    st[v][1]  = 1.0f - (wy - rm.y0) / th;
                }
                re.DrawTrianglePic(pts, st, rm.shader);
            }
        }
    }
    (void)place;

    // the objectives: a diamond, white for the current one
    for (int i = 0; i < MAX_OBJECTIVES; i++) {
        const char *o     = CL_ConfigString(CS_OBJECTIVES + i);
        const int   flags = atoi(Info_ValueForKey(o, "flags"));
        vec3_t      loc;
        float       x, y;

        if (!(flags & OBJ_FLAG_CURRENT) || (flags & (OBJ_FLAG_HIDDEN | OBJ_FLAG_COMPLETED))
            || sscanf(Info_ValueForKey(o, "loc"), "%f %f %f", &loc[0], &loc[1], &loc[2]) != 3) {
            continue;
        }
        {
            const qboolean current = i == atoi(CL_ConfigString(CS_CURRENT_OBJECTIVE));
            const float    half    = (current ? 5.5f : 4.25f) * k;
            const char    *layers[2] = {"poly 0.047 0.055 0.071 0.9 0.5 0 1 0.5 0.5 1 0 0.5",
                                        current ? "poly 1 1 1 0.95 0.5 0.12 0.88 0.5 0.5 0.88 0.12 0.5" : "poly 0.75 0.75 0.75 0.8 0.5 0.12 0.88 0.5 0.5 0.88 0.12 0.5"};
            const int      d       = (int)((half + 1.75f * k) * 2.0f * s + 0.5f);

            place(loc[0], loc[1], &x, &y);
            if (sqrtf(x * x + y * y) > r - 7.0f * k * s) {
                const float f = (r - 7.0f * k * s) / sqrtf(x * x + y * y);

                x *= f;
                y *= f;
            }
            uii.Rend_DrawPicStretched(floorf(r + x - d * 0.5f), floorf(r + y - d * 0.5f), d, d, 0, 0, 1, 1, UI_BakeShape("comp_obj", layers, 2, d, d, s));
        }
    }

    // the teammates: a dot with a dark edge and a point where they look
    if (g_radarClients && g_radarClientNum >= 0) {
        const char *color = cl.snap.ps.stats[STAT_TEAM] == TEAM_AXIS ? "0.8 0.533 0.533 1" : "0.533 0.569 0.8 1";
        char        dotLayers[3][96];
        const char *layers[3] = {dotLayers[0], dotLayers[1], dotLayers[2]};

        for (int i = 0; i < MAX_CLIENTS; i++) {
            const radarClient_t *c = &g_radarClients[i];
            float                x, y, dist;
            qboolean             clamped;

            if (i == g_radarClientNum || !c->time || cl.snap.serverTime - c->time >= 5000) {
                continue;
            }
            place(c->origin[0], c->origin[1], &x, &y);
            dist    = sqrtf(x * x + y * y);
            clamped = dist > r - 7.0f * k * s;
            if (clamped) {
                x *= (r - 7.0f * k * s) / dist;
                y *= (r - 7.0f * k * s) / dist;
            }
            {
                // in a box of 2 x 2.6 radii: the pointer up, the edge, the dot
                const float rad = (clamped ? 3.9f : 5.5f) * k, rim = rad + 1.25f * k, half = Q_max(2.6f * rad, rim) + 1.0f;
                const int   d   = (int)(half * 2.0f * s + 0.5f);
                const float yaw = RAD2DEG(atan2f(c->axis[1], c->axis[0]));

                Com_sprintf(dotLayers[0], 96, clamped ? "fill 0 0 0 0" : "poly %s 0.5 %g %g %g %g %g", color, 0.5f - 2.6f * rad / half * 0.5f,
                            0.5f + 0.85f * rim / half * 0.5f, 0.5f - 0.55f * rim / half * 0.5f, 0.5f - 0.85f * rim / half * 0.5f, 0.5f - 0.55f * rim / half * 0.5f);
                Com_sprintf(dotLayers[1], 96, "circle 0.047 0.055 0.071 0.9 %g", rim / half);
                Com_sprintf(dotLayers[2], 96, "circle %s %g", color, rad / half);
                {
                    const vec4_t white = {1, 1, 1, 1};
                    const float  a     = DEG2RAD(AngleSubtract(angles[YAW], yaw));

                    uii.Rend_SetColor(white);
                    uii.Rend_DrawPicStretched2(floorf(r + x - d * 0.5f), floorf(r + y - d * 0.5f), d, d, 0, 0, 1, 1, cosf(a), sinf(a),
                                               UI_BakeShape("comp_dot", layers, 3, d, d, s));
                }
            }
        }
    }

    // the player: an arrow in the middle, up
    {
        const char *layers[2] = {"poly 0.047 0.055 0.071 0.9 0.5 0.0625 0.8125 0.8125 0.1875 0.8125", "poly 1 1 1 1 0.5 0.1875 0.7232 0.7232 0.2768 0.7232"};
        const int   d         = (int)(16.0f * 1.4f * k * s + 0.5f);

        uii.Rend_DrawPicStretched(floorf(r - d * 0.5f), floorf(r - d * 0.5f), d, d, 0, 0, 1, 1, UI_BakeShape("comp_arrow", layers, 2, d, d, s));
    }

    // N E S W inside the edge, turning with the view
    {
        static const char *const letters[4] = {"N", "E", "S", "W"};
        const float               north     = cl.snap.ps.stats[STAT_COMPASSNORTH] / 182.0f;

        for (int i = 0; i < 4; i++) {
            const float a  = DEG2RAD(north - 90.0f * i - angles[YAW]);
            const float lx = (r - sinf(a) * (r - 10.0f * k * s)) / s, ly = (r - cosf(a) * (r - 10.0f * k * s)) / s;
            const float lw = m_font->getWidth(letters[i], -1);

            Comp_Text(m_font, lx - lw * 0.5f, ly, letters[i], i ? UColor(0.75f, 0.75f, 0.75f, 1.0f) : UColor(1, 1, 1, 1), getVirtualScale());
        }
    }
}
