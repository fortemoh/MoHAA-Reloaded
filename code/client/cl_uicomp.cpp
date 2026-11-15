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

#include <atomic>
#include <thread>
#include <vector>

#define COMP_PICTURES "ui/competitive/"
#define COMP_OUTLINES "ui/outlines/weapons/"

// the stock radar's teammates (cl_uiradar.cpp)
extern radarClient_t *g_radarClients;
extern int            g_radarClientNum;
#define COMP_LINES    8
#define COMP_KILLS    6

static cvar_t *cg_hudstyle;

const char *const ui_compHudMenus[UI_COMPHUD_MENUS] = {
    "hud_comp_topleft", "hud_comp_topleft_compass", "hud_comp_score",   "hud_comp_killfeed", "hud_comp_chat",
    "hud_comp_strip",   "hud_comp_weapons",         "hud_comp_damage",  "hud_comp_banners",  "hud_comp_bars",
    "hud_comp_killer",  "hud_comp_target",          "hud_comp_vote",
};

// the elements one can turn off (as cgame's Competitive HUD); the menus' enabledcvar
static const char *const comp_toggles[] = {
    "cg_hud_radar", "cg_hud_killfeed", "cg_hud_chat", "cg_hud_messages", "cg_hud_health", "cg_hud_ammo",
    "cg_hud_grenades", "cg_hud_grenadecharge", "cg_hud_score", "cg_hud_timer", "cg_hud_alive", "cg_hud_teamicon",
    "cg_hud_killer", "cg_hud_targetinfo", "cg_hud_vote", "cg_hud_spectator", "cg_hud_following", "cg_hud_attacker",
    "cg_hud_stopwatch", "cg_hud_damage",
};

// what the menus (ui/hud_comp_*.urc) read and this file sets
static const char *const comp_menuCvars[] = {
    "ui_hud_acc", "ui_hud_acc_banner", "ui_hud_acc_line", "ui_hud_acc_radar", "ui_hud_acc_ring",
    "ui_hud_allies", "ui_hud_ammo", "ui_hud_ammo_on", "ui_hud_arc0_alpha", "ui_hud_arc0_angle", "ui_hud_arc1_alpha",
    "ui_hud_arc1_angle", "ui_hud_arc2_alpha", "ui_hud_arc2_angle", "ui_hud_axis", "ui_hud_badge_on", "ui_hud_banner1",
    "ui_hud_banner1_on", "ui_hud_banner2", "ui_hud_banner2_on", "ui_hud_charge", "ui_hud_charge_on", "ui_hud_clip",
    "ui_hud_compass_on", "ui_hud_dmg_alpha", "ui_hud_dmg_angle", "ui_hud_follow", "ui_hud_follow_col",
    "ui_hud_follow_on", "ui_hud_health", "ui_hud_health_col", "ui_hud_health_on", "ui_hud_hurt", "ui_hud_hurt_col",
    "ui_hud_hurt_on", "ui_hud_killer", "ui_hud_killer_col", "ui_hud_killer_on", "ui_hud_msgs_c", "ui_hud_msgs_r",
    "ui_hud_north", "ui_hud_obj0", "ui_hud_obj0_on", "ui_hud_obj1", "ui_hud_obj1_on", "ui_hud_obj2", "ui_hud_obj2_on",
    "ui_hud_radar_on", "ui_hud_score_l", "ui_hud_score_lc", "ui_hud_score_on", "ui_hud_score_r",
    "ui_hud_score_rc", "ui_hud_stopwatch", "ui_hud_stopwatch_on", "ui_hud_target", "ui_hud_target_col",
    "ui_hud_target_hp", "ui_hud_target_hpcol", "ui_hud_target_on", "ui_hud_timer", "ui_hud_vote_on",
    "ui_hud_vote_prompt", "ui_hud_vote_tally", "ui_hud_vote_text", "ui_hud_vote_time", "ui_hud_vs_on",
    "ui_hud_weapons_on", "ui_hud_cards_allies", "ui_hud_cards_axis", "ui_hud_never",
};

// the accent (cg_hud_accent, the HUD's pale yellow by default) and its
// shades: each moves with the accent as much as it has of the accent's color
// (its spread between its brightest and darkest channel against the
// accent's), so a paler shade stays a paler shade of a new accent. The menus
// read them as ui_hud_acc* (a shape's $cvar, colorcvar)
enum { ACC_MAIN, ACC_LINE, ACC_RING, ACC_RADAR, ACC_BANNER, ACC_WEAPON };

static const struct {
    const char *cvar;
    float       color[3]; // with the default accent (the first)
} comp_shades[] = {
    {"ui_hud_acc",        {0.965f, 0.965f, 0.478f}}, // health, ammo, the bullet, the grenades' counts
    {"ui_hud_acc_line",   {0.933f, 0.941f, 0.659f}}, // the badge's lines, the ammo's separator
    {"ui_hud_acc_ring",   {0.91f, 0.922f, 0.643f} }, // the badge's ring
    {"ui_hud_acc_radar",  {0.863f, 0.894f, 0.643f}}, // the radar's ring
    {"ui_hud_acc_banner", {0.706f, 0.753f, 0.549f}}, // the banners' edges
    {NULL,                {0.98f, 0.98f, 0.565f}  }, // the weapons carried
};

static float comp_accent[ARRAY_LEN(comp_shades)][3];
static int   comp_accentStamp;

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
    qboolean    myKill, myDeath; // the player's kill (black, a red edge), the player's death (dark red)
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
    char     cards[2][MAX_CLIENTS + 1]; // a card each: b alive with a bar, a alive, d dead

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
    const char *pic;  // COMP_OUTLINES<pic>.svg
    int         kind, nation;
} compWeapon_t;

static const compWeapon_t comp_weapons[] = {
    {"M1 Garand",              "m1_garand",            WK_PRIMARY, NAT_US },
    {"Thompson",               "thompsonsmg",          WK_PRIMARY, NAT_US },
    {"BAR",                    "bar",                  WK_PRIMARY, NAT_ANY},
    {"Springfield '03 Sniper", "springfield",          WK_PRIMARY, NAT_ANY},
    {"Shotgun",                "shotgun",              WK_PRIMARY, NAT_ANY},
    {"Bazooka",                "bazooka",              WK_PRIMARY, NAT_ANY},
    {"Colt 45",                "colt45",               WK_SIDEARM, NAT_US },
    {"Hi-Standard Silenced",   "silencedpistol",       WK_SIDEARM, NAT_ANY},
    {"Frag Grenade",           "m2frag_grenade",       WK_FRAG,    NAT_US },
    {"M18 Smoke Grenade",      "m18_smoke_grenade",    WK_SMOKE,   NAT_ANY},
    {"Lee-Enfield",            "enfield",              WK_PRIMARY, NAT_UK },
    {"Sten Mark II",           "sten",                 WK_PRIMARY, NAT_UK },
    {"Enfield L42A1",          "uk_w_l42a1",           WK_PRIMARY, NAT_UK },
    {"DeLisle",                "delisle",              WK_PRIMARY, NAT_UK },
    {"Vickers-Berthier",       "uk_w_vickers",         WK_PRIMARY, NAT_UK },
    {"PIAT",                   "uk_w_piat",            WK_PRIMARY, NAT_UK },
    {"Webley Revolver",        "webley_revolver",      WK_SIDEARM, NAT_UK },
    {"Mills Grenade",          "mills_grenade",        WK_FRAG,    NAT_UK },
    {"Mosin Nagant Rifle",     "mosin_nagant_rifle",   WK_PRIMARY, NAT_RU },
    {"PPSH SMG",               "ppsh_smg",             WK_PRIMARY, NAT_RU },
    {"SVT 40",                 "svt_rifle",            WK_PRIMARY, NAT_RU },
    {"Nagant Revolver",        "nagant_revolver",      WK_SIDEARM, NAT_RU },
    {"Russian F1 Grenade",     "russian_f1_grenade",   WK_FRAG,    NAT_RU },
    {"RDG-1 Smoke Grenade",    "rdg-1_smoke_grenade",  WK_SMOKE,   NAT_RU },
    {"Mauser KAR 98K",         "kar98",                WK_PRIMARY, NAT_DE },
    {"KAR98 - Sniper",         "kar98sniper",          WK_PRIMARY, NAT_ANY},
    {"G 43",                   "g43",                  WK_PRIMARY, NAT_DE },
    {"MP40",                   "mp40",                 WK_PRIMARY, NAT_DE },
    {"StG 44",                 "mp44",                 WK_PRIMARY, NAT_DE },
    {"Panzerschreck",          "panzerschreck",        WK_PRIMARY, NAT_ANY},
    {"Gewehrgranate",          "kar98_mortar",         WK_PRIMARY, NAT_DE },
    {"Walther P38",            "p38",                  WK_SIDEARM, NAT_DE },
    {"Stielhandgranate",       "steilhandgranate",     WK_FRAG,    NAT_DE },
    {"Nebelhandgranate",       "nebelhandgranate",     WK_SMOKE,   NAT_DE },
    {"Carcano",                "it_w_carcano",         WK_PRIMARY, NAT_IT },
    {"Moschetto",              "it_w_moschetto",       WK_PRIMARY, NAT_IT },
    {"Breda",                  "it_w_breda",           WK_PRIMARY, NAT_IT },
    {"Beretta",                "it_w_beretta",         WK_SIDEARM, NAT_IT },
    {"Bomba A Mano",           "it_w_bomba",           WK_FRAG,    NAT_IT },
    {"Bomba A Mano Breda",     "it_w_bombabreda",      WK_SMOKE,   NAT_IT },
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

// the kill feed's look (CS2's, as a 1080-line screen's design pixels): a
// row 36 tall, its names' capitals a third of it (the URC's face), the
// weapon a half; padding, gaps, the rows' spacing; the boxes' corners and
// the player's kill's red edge
#define KF_ROW    36.0f
#define KF_ICON   18.0f
#define KF_PAD    12.0f
#define KF_GAP    6.0f
#define KF_SPACE  3.0f
#define KF_RADIUS 3.0f
#define KF_EDGE   2.0f

static const compKillPic_t comp_killPics[] = {
    {"rifle",   "m1_garand",      "kar98",            90.0f,  16.2f, 90.5f,  16.3f},
    {"pistol",  "colt45",         "p38",              33.9f,  21.9f, 33.9f,  22.2f},
    {"sniper",  "springfield",    "kar98sniper",      89.9f,  17.5f, 90.5f,  17.6f},
    {"smg",     "thompsonsmg",    "mp40",             75.1f,  24.2f, 74.8f,  26.0f},
    {"mg",      "bar",            "mp44",             95.5f,  15.3f, 70.9f,  26.0f},
    {"shotgun", "shotgun",        "shotgun",          85.0f,  15.9f, 85.0f,  15.9f},
    {"grenade", "m2frag_grenade", "steilhandgranate", 12.5f,  22.0f, 5.6f,   26.0f},
    {"rocket",  "bazooka",        "panzerschreck",    110.6f, 20.0f, 113.5f, 26.0f},
    {"bash",    "glove_50",              "glove_50",                20.0f,  20.0f, 20.0f,  20.0f},
    {"none",    "skull_50",              "skull_50",                20.0f,  20.0f, 20.0f,  20.0f},
};

/*
====================================================================

PLAYERS

====================================================================
*/

// what the HUD reads of the configstrings, parsed again only after one
// changed (UI_CompHud_ConfigString), not each frame
static struct {
    int      changes, parsed;
    int      gametype;
    qboolean floodProtect;
    char     map[MAX_QPATH];
    char     name[MAX_CLIENTS][MAX_NAME_LENGTH];
    int      team[MAX_CLIENTS];
    int      objFlags[MAX_OBJECTIVES], objCurrent;
    vec3_t   objLoc[MAX_OBJECTIVES];
    qboolean teamScoresSent;
    int      teamScores[2]; // the server's (CS_TEAMSCORES)
} cs = {1};

void UI_CompHud_ConfigString(void)
{
    cs.changes++;
}

static void Comp_ConfigStrings(void)
{
    const char *info = CL_ConfigString(CS_SERVERINFO);

    if (cs.parsed == cs.changes) {
        return;
    }
    cs.parsed       = cs.changes;
    cs.gametype     = atoi(Info_ValueForKey(info, "g_gametype"));
    cs.floodProtect = atoi(Info_ValueForKey(info, "sv_floodProtect")) != 0;
    Q_strncpyz(cs.map, Info_ValueForKey(info, "mapname"), sizeof(cs.map));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Q_strncpyz(cs.name[i], Info_ValueForKey(CL_ConfigString(CS_PLAYERS + i), "name"), sizeof(cs.name[i]));
        cs.team[i] = atoi(Info_ValueForKey(CL_ConfigString(CS_PLAYERS + i), "team"));
    }
    for (int i = 0; i < MAX_OBJECTIVES; i++) {
        const char *o = CL_ConfigString(CS_OBJECTIVES + i);

        cs.objFlags[i] = atoi(Info_ValueForKey(o, "flags"));
        if (sscanf(Info_ValueForKey(o, "loc"), "%f %f %f", &cs.objLoc[i][0], &cs.objLoc[i][1], &cs.objLoc[i][2]) != 3) {
            cs.objFlags[i] = 0;
        }
    }
    cs.objCurrent = atoi(CL_ConfigString(CS_CURRENT_OBJECTIVE));
    cs.teamScoresSent = sscanf(CL_ConfigString(CS_TEAMSCORES), "%d %d", &cs.teamScores[0], &cs.teamScores[1]) == 2;
}

static int Comp_Gametype(void)
{
    Comp_ConfigStrings();
    return cs.gametype;
}

static const char *Comp_Name(int client)
{
    Comp_ConfigStrings();
    return client >= 0 && client < MAX_CLIENTS ? cs.name[client] : "";
}

static int Comp_Team(int client)
{
    Comp_ConfigStrings();
    return client >= 0 && client < MAX_CLIENTS ? cs.team[client] : 0;
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

// a kill picture's size in the feed: its box, no taller than KF_ICON
static void Comp_KillIconSize(const compKillPic_t *p, qboolean axis, float *w, float *h)
{
    *w = axis ? p->xw : p->aw;
    *h = axis ? p->xh : p->ah;
    if (*h > KF_ICON) {
        *w *= KF_ICON / *h;
        *h = KF_ICON;
    }
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
    Comp_KillIconSize(pic, axis, &k->picW, &k->picH);
    // (a suicide is the player's death)
    k->myDeath = client == cl.snap.ps.clientNum;
    k->myKill  = !k->myDeath && k->killer[0] && Comp_FindName(k->killer) == cl.snap.ps.clientNum;
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
    cs.changes++;
    memset(&comp, 0, sizeof(comp));
    comp.sbScore[0] = comp.sbScore[1] = -1;
    comp.lastDmg                      = -999999;
}

/*
====================================================================

THE FRAME: the ui_hud_* cvars

====================================================================
*/

// a cvar by name, found once: the names are the callers' literals, a table
// by their addresses
static cvar_t *Comp_Cvar(const char *name)
{
    static struct {
        const char *name;
        cvar_t     *cvar;
    } found[512];
    unsigned i = (unsigned)((uintptr_t)name >> 2) % ARRAY_LEN(found);

    while (found[i].name && found[i].name != name) {
        i = (i + 1) % ARRAY_LEN(found);
    }
    if (!found[i].name) {
        found[i].name = name;
        found[i].cvar = Cvar_Get(name, "", 0);
    }
    return found[i].cvar;
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

static void Comp_SetI(const char *name, int value)
{
    cvar_t *v = Comp_Cvar(name);

    if (v->integer != value || !v->string[0]) {
        Comp_Set(name, va("%d", value));
    }
}

static void Comp_SetF(const char *name, float value)
{
    cvar_t *v = Comp_Cvar(name);

    // (the text made only when the value changed: "%.3f")
    if (fabsf(v->value - value) >= 0.0005f || !v->string[0]) {
        const int k = (int)(fabsf(value) * 1000.0f + 0.5f);
        char      text[32];

        Com_sprintf(text, sizeof(text), "%s%d.%03d", value < 0 && k ? "-" : "", k / 1000, k % 1000);
        Comp_Set(name, text);
    }
}

// a color as "f6f67a", "#f6f67a" or "R G B" (0 to 1), to three decimals
static qboolean Comp_ParseColor(const char *text, float out[3])
{
    int n = 0;

    while (*text == ' ') {
        text++;
    }
    if (*text == '#') {
        text++;
    }
    while (isxdigit((byte)text[n])) {
        n++;
    }
    if (n == 6 && (!text[n] || text[n] == ' ')) {
        const unsigned long v = strtoul(text, NULL, 16);

        for (int k = 0; k < 3; k++) {
            out[k] = ((v >> (16 - 8 * k)) & 255) / 255.0f;
        }
    } else if (sscanf(text, "%f %f %f", &out[0], &out[1], &out[2]) != 3) {
        return qfalse;
    }
    for (int k = 0; k < 3; k++) {
        out[k] = roundf(Com_Clamp(0.0f, 1.0f, out[k]) * 1000.0f) / 1000.0f;
    }
    return qtrue;
}

// the accent's shades from cg_hud_accent (the default's when it can't be read)
static void Comp_Accent(void)
{
    const float *base = comp_shades[ACC_MAIN].color;
    const float  spread = Q_max(base[0], Q_max(base[1], base[2])) - Q_min(base[0], Q_min(base[1], base[2]));
    float        accent[3];

    if (!Comp_ParseColor(Comp_Cvar("cg_hud_accent")->string, accent)) {
        VectorCopy(base, accent);
    }
    for (size_t i = 0; i < ARRAY_LEN(comp_shades); i++) {
        const float *c     = comp_shades[i].color;
        const float  share = (Q_max(c[0], Q_max(c[1], c[2])) - Q_min(c[0], Q_min(c[1], c[2]))) / spread;

        for (int k = 0; k < 3; k++) {
            comp_accent[i][k] = roundf(Com_Clamp(0.0f, 1.0f, c[k] + share * (accent[k] - base[k])) * 1000.0f) / 1000.0f;
        }
        if (comp_shades[i].cvar) {
            Comp_Set(comp_shades[i].cvar, va("%g %g %g", comp_accent[i][0], comp_accent[i][1], comp_accent[i][2]));
        }
    }
    comp_accentStamp++;
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
    // the players the snapshot shows (not our own entity: a dead player's
    // spectator is sent, not dead), and the player watched
    for (i = 0; i < cl.snap.numEntities; i++) {
        const entityState_t *es = &cl.parseEntities[(cl.snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];

        if (es->eType == ET_PLAYER && es->number >= 0 && es->number < MAX_CLIENTS && es->number != ps->clientNum) {
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
    if (gametype >= GT_TEAM && (comp.wantScores || !cs.floodProtect)
        && (unsigned)(cls.realtime - comp.scoreTime) >= 2000) {
        comp.wantScores = qfalse;
        comp.scoreTime  = cls.realtime;
        CL_AddReliableCommand("score", qfalse);
    }
    // in the clients' order, a dead player's card in its place; the bars
    // under the player's own team (both while spectating)
    for (t = 0; t < 2; t++) {
        const int side = t ? TEAM_AXIS : TEAM_ALLIES;
        const int own  = ps->stats[STAT_TEAM];
        const int bars = own == side || (own != TEAM_ALLIES && own != TEAM_AXIS);
        char     *c    = comp.cards[t];
        int       n    = 0;

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (comp.team[i] == side) {
                c[n++] = comp.dead[i] ? 'd' : bars ? 'b' : 'a';
            }
        }
        c[n] = 0;
    }
}

// the weapons carried: the primary's and the sidearm's names (as seen in
// hand or picked up), the grenades, the grenade's charge
static void Comp_Weapons(const char *inHand)
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
        static char kinds[MAX_CONFIGSTRINGS]; // 1 none, 2 smoke, 3 frag (0: not read since a change)
        static int  read = -1;
        const int   idx  = ps->ammo_name_index[i];

        Comp_ConfigStrings();
        if (read != cs.parsed) {
            memset(kinds, 0, sizeof(kinds));
            read = cs.parsed;
        }
        if (idx <= 0 || idx >= MAX_CONFIGSTRINGS) {
            continue;
        }
        if (!kinds[idx]) {
            const char *name = CL_ConfigString(idx);

            kinds[idx] = Q_stristr(name, "smoke") ? 2
                      : (Q_stristr(name, "grenade") && !Q_stristr(name, "riflegrenade")) || !Q_stricmp(name, "agrenade") ? 3 : 1;
        }
        if (kinds[idx] == 2) {
            smokes = Q_max(smokes, ps->ammo_amount[i]);
        } else if (kinds[idx] == 3) {
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

// text broken between words into lines of a width (design units), up to
// lines of them
static str Comp_Wrap(UIFont *font, const char *p, float width, int lines)
{
    str text;

    for (int line = 0; *p && line < lines; line++) {
        const char *brk = NULL, *q = p;

        for (; *q; q++) {
            if (*q == ' ') {
                brk = q;
            }
            if (q > p && font->getWidth(p, (int)(q - p + 1)) > width) {
                break;
            }
        }
        if (*q && brk && brk > p && line + 1 < lines) {
            q = brk;
        }
        text += str(p, 0, (int)(q - p));
        text += line + 1 < lines && *q ? "\n" : "";
        for (p = q; *p == ' '; p++) {
        }
    }
    return text;
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

    static int  lastSnap = -1, lastChange = -1, lastFull;

    if (!cl.snap.valid) {
        return;
    }
    // cg_hud_scale: the menus at the new size, their faces and shapes baked again
    {
        static int  read = -1;
        cvar_t     *cv = Comp_Cvar("cg_hud_scale");

        if (cv->modificationCount != read) {
            const char *v = cv->string;
            char       *end;
            float       scale = strtof(v, &end);

            read  = cv->modificationCount;
            scale = end == v || scale != scale ? 1.0f : Com_Clamp(0.25f, 2.0f, scale);
            if (UI_SetDesignScale(scale)) {
                menuManager.RealignMenus();
                UI_CompHud_PreBake(qfalse);
            }
        }
    }
    // cg_hud_accent: the shades again (what reads them is drawn and baked
    // again), the health's color with them
    {
        static int read = -1;

        if (Comp_Cvar("cg_hud_accent")->modificationCount != read) {
            read = Comp_Cvar("cg_hud_accent")->modificationCount;
            Comp_Accent();
            lastChange = -1;
        }
    }
    cge->CG_EyeAngles(&angles);
    viewYaw = angles[YAW];

    // what comes with a snapshot or a configstring: made again with a new
    // one, and every 100 ms (the toggles, the keys); the view's and time's each frame
    const bool full = cl.snap.messageNum != lastSnap || cs.changes != lastChange || (unsigned)(cls.realtime - lastFull) >= 100;

    if (full) {
    lastSnap   = cl.snap.messageNum;
    lastChange = cs.changes;
    lastFull   = cls.realtime;
    // (copied: va's buffer is used again below)
    Q_strncpyz(spec, Comp_Int("cg_hud_spectator") ? Comp_SpectatorText() : "", sizeof(spec));
    Comp_Roster(gametype);
    Comp_Weapons(weapon);

    // the strip: health, badge, ammo, weapons (not while free spectating)
    {
        const qboolean icon = teams && (!spectating || following) && Comp_Int("cg_hud_badge") && Comp_Int("cg_hud_teamicon");

        Comp_Set("ui_hud_allies", icon && team == TEAM_ALLIES ? "1" : "0");
        Comp_Set("ui_hud_axis", icon && team == TEAM_AXIS ? "1" : "0");
    }
    if (following && !(ps->stats[STAT_WEAPONS] & 0x3F) && ps->stats[STAT_INFOCLIENT_HEALTH] > 0) {
        health    = ps->stats[STAT_INFOCLIENT_HEALTH];
        maxHealth = 100;
    }
    Comp_SetI("ui_hud_health", health);
    Comp_Set("ui_hud_health_col", health < Comp_Cvar("cg_hud_lowhealth")->value * maxHealth ? "0.753 0.188 0.129 1" : Comp_Cvar("ui_hud_acc")->string);
    Comp_Set("ui_hud_ammo_on", weapon[0] ? "1" : "0");
    Comp_SetI("ui_hud_clip", ps->stats[STAT_CLIPAMMO]);
    Comp_SetI("ui_hud_ammo", ps->stats[STAT_AMMO]);

    // the score box: Allies | Axis under the timer, or self | the leader
    // ("-" while a score isn't known; a score can be below 0). The teams'
    // scores: the server's (CS_TEAMSCORES, a server of this code); else our
    // team's and the highest (the server's stats, every frame), the
    // scoreboard's (asked for at times, -1 not read yet) for the other team's
    // below ours, and 0 for it in round modes while both are 0 (a team's
    // score there is its rounds won)
    {
        const int matchEnd = atoi(CL_ConfigString(CS_MATCHEND));
        int       score[2] = {ps->stats[STAT_KILLS], ps->stats[STAT_HIGHEST_SCORE]};
        qboolean  known[2] = {qtrue, qtrue};

        if (teams && cs.teamScoresSent) {
            score[0] = cs.teamScores[0];
            score[1] = cs.teamScores[1];
        } else if (teams) {
            const int mine = team == TEAM_AXIS;

            score[0] = comp.sbScore[0];
            score[1] = comp.sbScore[1];
            known[0] = score[0] >= 0;
            known[1] = score[1] >= 0;
            if (team == TEAM_ALLIES || team == TEAM_AXIS) {
                score[mine] = ps->stats[STAT_KILLS];
                known[mine] = qtrue;
                if (ps->stats[STAT_HIGHEST_SCORE] > ps->stats[STAT_KILLS]) {
                    score[!mine] = ps->stats[STAT_HIGHEST_SCORE];
                    known[!mine] = qtrue;
                } else if (gametype >= GT_TEAM_ROUNDS && !ps->stats[STAT_HIGHEST_SCORE]) {
                    score[!mine] = 0;
                    known[!mine] = qtrue;
                }
            }
        }
        Comp_Set("ui_hud_score_l", known[0] ? va("%d", score[0]) : "-");
        Comp_Set("ui_hud_score_r", known[1] ? va("%d", score[1]) : "-");
        Comp_Set("ui_hud_score_lc", teams ? "0.588 0.729 0.871 1" : "0.922 0.933 0.949 1");
        Comp_Set("ui_hud_score_rc", teams ? "0.902 0.478 0.408 1" : "0.922 0.933 0.949 1");
        if (!(atoi(CL_ConfigString(CS_WARMUP)) != -1 && matchEnd && gametype != GT_LIBERATION && matchEnd > cl.serverTime)) {
            banner1 = Comp_Int("cg_hud_timer") ? Sys_LV_CL_ConvertString(Comp_Cvar("ui_timemessage")->string) : "";
        }
        Comp_Set("ui_hud_cards_allies", rounds ? comp.cards[0] : "");
        Comp_Set("ui_hud_cards_axis", rounds ? comp.cards[1] : "");
    }
    // the banners, the time message's first
    Comp_Set("ui_hud_banner1", banner1[0] ? banner1 : spec);
    Comp_Set("ui_hud_banner2", banner1[0] ? spec : "");
    Comp_Set("ui_hud_banner1_on", banner1[0] || spec[0] ? "1" : "0");
    Comp_Set("ui_hud_banner2_on", banner1[0] && spec[0] ? "1" : "0");
    }

    // the timer: its seconds
    {
        static int  shown = -2;
        const int   matchEnd = atoi(CL_ConfigString(CS_MATCHEND));
        const int   left = atoi(CL_ConfigString(CS_WARMUP)) != -1 && matchEnd && gametype != GT_LIBERATION && matchEnd > cl.serverTime
                         ? (matchEnd - cl.serverTime) / 1000 : -1;

        if (left != shown || !Comp_Cvar("ui_hud_timer")->modificationCount) {
            shown = left;
            Comp_Set("ui_hud_timer", left >= 0 ? va("%d:%02d", left / 60, left % 60) : "");
        }
    }

    // the stopwatch (Omaha's bar outlasts the wait by a quarter second) and the grenade's charge
    {
        const stopWatch_t *sw = &cls.stopwatch;
        float              frac = 0.0f;

        if (sw->iStartTime && sw->iStartTime < sw->iEndTime && ps->stats[STAT_HEALTH] > 0) {
            frac = sw->eType >= SWT_FUSE_WET ? (sw->iEndTime > cl.serverTime)
                 : Com_Clamp(0.0f, 1.0f, (float)(sw->iEndTime + 250 - cl.serverTime) / (sw->iEndTime - sw->iStartTime + 250));
        }
        Comp_SetF("ui_hud_stopwatch", frac);
        Comp_Set("ui_hud_stopwatch_on", frac > 0.0f && Comp_Int("cg_hud_stopwatch") ? "1" : "0");
        if (comp.charging) {
            // the stock scripts' charge: 0.6 to 4 s, 0.5 to 1.5 for the secondary throw
            const float lo = comp.chargeSecondary ? 0.5f : 0.6f, hi = comp.chargeSecondary ? 1.5f : 4.0f;

            Comp_SetF("ui_hud_charge", Com_Clamp(0.0f, 1.0f, ((cl.serverTime - comp.chargeStart) / 1000.0f - lo) / (hi - lo)));
        }
    }

    // who killed you, the player aimed at, who you follow, who last hurt you
    if (full) {
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
    if (full) {
        const int voteTime = atoi(CL_ConfigString(CS_VOTE_TIME));

        Comp_Set("ui_hud_vote_on", voteTime ? "1" : "0");
        if (voteTime) {
            Comp_Set("ui_hud_vote_time", va("%ds", Q_max(0, (30000 - (cl.serverTime - voteTime)) / 1000 + 1)));
            static UIFont *font, *small;
            const float    vs[2] = {UI_DesignScale(1920.0f, 1080.0f), UI_DesignScale(1920.0f, 1080.0f)};

            if (!font) {
                font  = new UIFont("oswald-medium-16s");
                small = new UIFont("oswald-medium-12s");
            }
            font->setVirtualScale(vs);
            small->setVirtualScale(vs);
            // the subject and the prompt wrapped in the panel (up to six lines each)
            Comp_Set("ui_hud_vote_text", Comp_Wrap(font, CL_ConfigString(CS_VOTE_STRING), 252.0f, 6));
            Comp_Set("ui_hud_vote_tally", va("%s  %s     %s  %s", Sys_LV_CL_ConvertString("Yes"), CL_ConfigString(CS_VOTE_YES),
                                             Sys_LV_CL_ConvertString("No"), CL_ConfigString(CS_VOTE_NO)));
            Comp_Set("ui_hud_vote_prompt", ps->voted ? "" : Comp_Wrap(small, Sys_LV_CL_ConvertString("To vote Yes, press F1. To vote No, press F2."), 252.0f, 6));
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
            if (comp.arcTime[i] && age < 1500) {
                Comp_SetF(comp_arcAngle[i], Comp_ScreenAngle(viewYaw, comp.arcStat[i] / 10.0f + 180.0f));
            }
        }
        Comp_SetF("ui_hud_dmg_alpha", dmg > 0 && comp.flashTime ? Com_Clamp(0.0f, 1.0f, 1.0f - (cls.realtime - comp.flashTime) / 1500.0f) : 0.0f);
        if (Comp_Cvar("ui_hud_dmg_alpha")->value > 0.0f) {
            Comp_SetF("ui_hud_dmg_angle", Comp_ScreenAngle(viewYaw, dmg / 10.0f + 180.0f));
        }
    }

    // the elements' flags (enabledcvar): shown and turned on
    if (full) {
        // through the intermission (and with no HUD) only the messages, the
        // chat, the kills and the vote (cl_ui.cpp: the other menus hidden)
        const qboolean show  = !(ps->pm_flags & (PMF_NO_HUD | PMF_INTERMISSION));
        const qboolean strip = (!(ps->pm_flags & PMF_SPECTATING) || following) && show;
        const qboolean radar = Comp_Int("cg_hud_radar") != 0;
        const qboolean msgs  = Comp_Int("cg_hud_messages") != 0;
        auto           flag  = [](const char *name, qboolean on) { Comp_Set(name, on ? "1" : "0"); };

        flag("ui_hud_radar_on", radar && show);
        flag("ui_hud_compass_on", !radar && Comp_Int("ui_compass") && show);
        flag("ui_hud_msgs_r", msgs && radar);
        flag("ui_hud_msgs_c", msgs && !radar);
        flag("ui_hud_score_on", Comp_Int("cg_hud_score") && gametype != GT_SINGLE_PLAYER);
        // the cards
        flag("ui_hud_vs_on", rounds && Comp_Int("cg_hud_alive"));
        flag("ui_hud_health_on", strip && Comp_Int("cg_hud_health"));
        flag("ui_hud_badge_on", strip && Comp_Int("cg_hud_badge"));
        flag("ui_hud_ammo_on", strip && weapon[0] && Comp_Int("cg_hud_ammo"));
        flag("ui_hud_weapons_on", strip && Comp_Int("cg_hud_ammo"));
        flag("ui_hud_killer_on", Comp_Cvar("ui_hud_killer")->string[0] && Comp_Int("cg_hud_killer"));
        flag("ui_hud_target_on", Comp_Cvar("ui_hud_target")->string[0] && Comp_Int("cg_hud_targetinfo"));
        flag("ui_hud_follow_on", following && Comp_Int("cg_hud_following") && show);
        flag("ui_hud_hurt_on", Comp_Cvar("ui_hud_hurt")->string[0] && Comp_Int("cg_hud_attacker") && show);
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
    if (Comp_Cvar("ui_hud_compass_on")->integer) {
        const int stats[3] = {ps->stats[STAT_OBJECTIVECENTER], ps->stats[STAT_OBJECTIVELEFT], ps->stats[STAT_OBJECTIVERIGHT]};
        const int rest[3]  = {1800, 1730, 1870};

        Comp_SetF("ui_hud_north", anglemod(viewYaw - ps->stats[STAT_COMPASSNORTH] / 182.0f));
        for (i = 0; i < 3; i++) {
            Comp_Set(comp_objOn[i], stats[i] != rest[i] ? "1" : "0");
            Comp_SetF(comp_obj[i], (stats[i] - 1800) / 10.0f);
        }
    }
}

// the HUD's menus, faces, pictures, outlines and shader script compiled in (cmake/utils/embed_files.cmake)
extern "C" const builtinFile_t builtin_files[];

void UI_CompHud_Files(void)
{
    // (a file on disk or in a pak of the name is read instead)
    FS_SetBuiltinFiles(builtin_files);
}

/*
================
UI_CompHud_HeadIcons

The pictures of the team icons above team mates' heads (scripts/
reloaded_hud.shader, cgame's with cg_hudstyle 1): the HUD's team outlines
baked at 128 px into TGAs, given to the filesystem with the files compiled
in. The discs fill as much of the picture as the game's 32 px allies.tga
(28 px) and axis.tga (30 px) do; the renderer mipmaps them
================
*/
static std::vector<builtinFile_t> comp_files;
static std::vector<byte>          comp_headIcons[2];

void UI_CompHud_HeadIcons(void)
{
    static const char *const icons[2][2] = {
        {"textures/hud/reloaded_allies.tga", "in 0.0625 0.0625 0.9375 0.9375 svg 1 1 1 1 ui/outlines/team/allies.svg center"   },
        {"textures/hud/reloaded_axis.tga",   "in 0.03125 0.03125 0.96875 0.96875 svg 1 1 1 1 ui/outlines/team/axis.svg center"},
    };
    const int         size = 128;
    std::vector<byte> rgba(size * size * 4);

    if (!comp_files.empty()) {
        return;
    }
    for (const builtinFile_t *f = builtin_files; f->name; f++) {
        comp_files.push_back(*f);
    }
    for (int i = 0; i < 2; i++) {
        std::vector<byte>& tga = comp_headIcons[i];

        UI_BakeTexels(&icons[i][1], 1, size, size, 1.0f, rgba.data());
        // uncompressed, 32 bits, 8 of alpha, the rows from the bottom up
        tga.assign(18, 0);
        tga[2]  = 2;
        tga[12] = size & 255;
        tga[13] = size >> 8;
        tga[14] = size & 255;
        tga[15] = size >> 8;
        tga[16] = 32;
        tga[17] = 8;
        for (int y = size - 1; y >= 0; y--) {
            for (int x = 0; x < size; x++) {
                const byte *p = &rgba[(y * size + x) * 4];

                tga.push_back(p[2]);
                tga.push_back(p[1]);
                tga.push_back(p[0]);
                tga.push_back(p[3]);
            }
        }
        comp_files.push_back({icons[i][0], tga.data(), (int)tga.size()});
    }
    comp_files.push_back({NULL, NULL, 0});
    FS_SetBuiltinFiles(comp_files.data());
}

void UI_CompHud_Init(void)
{
    cg_hudstyle = Cvar_Get("cg_hudstyle", "0", CVAR_ARCHIVE);
    for (size_t i = 0; i < ARRAY_LEN(comp_toggles); i++) {
        Cvar_Get(comp_toggles[i], "1", CVAR_ARCHIVE);
    }
    Cvar_Get("cg_hud_lowhealth", "0.4", CVAR_ARCHIVE);
    // the HUD's size: its 1920x1080 design fitted in the screen, times this (0.25 to 2)
    Cvar_Get("cg_hud_scale", "1", CVAR_ARCHIVE);
    // the accent: "f6f67a", "#f6f67a" or "R G B" (0 to 1)
    Cvar_Get("cg_hud_accent", "f6f67a", CVAR_ARCHIVE);
    // what the menus read, made now, empty (ui_hud_never stays so: the never
    // shown widgets, the teams' icons and the cards' shapes): a widget
    // reading a cvar that isn't there draws every frame (the draw cache
    // can't tell when it comes), and some are set only now and then (a
    // vote's, the compass's, a charge)
    for (size_t i = 0; i < ARRAY_LEN(comp_menuCvars); i++) {
        Cvar_Get(comp_menuCvars[i], "", 0);
    }
    Comp_Accent();
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

// a weapon's outline (COMP_OUTLINES<pic>.svg), not one of the kill feed's
// pictures (the glove, the headshot and the skull: COMP_PICTURES<pic>.png)
static qboolean Comp_Outline(const char *pic)
{
    return strcmp(pic, "glove_50") && strcmp(pic, "headshot_50") && strcmp(pic, "skull_50") ? qtrue : qfalse;
}

// a picture baked at pw x ph pixels
static qhandle_t Comp_PictureShader(const char *pic, int pw, int ph, float s, qboolean mirror)
{
    char        layer[MAX_QPATH * 2];
    const char *layers[1] = {layer};

    if (Comp_Outline(pic)) {
        Com_sprintf(layer, sizeof(layer), "svg 1 1 1 1 " COMP_OUTLINES "%s.svg center%s", pic, mirror ? " mirror" : "");
    } else {
        Com_sprintf(layer, sizeof(layer), "picture 1 1 1 1 " COMP_PICTURES "%s.png center%s", pic, mirror ? " mirror" : "");
    }
    return UI_BakeShape("comp", layers, 1, pw, ph, s);
}

// a picture baked at its size (w x h design pixels at x y), tinted
static void Comp_Picture(const char *pic, float x, float y, float w, float h, float s, qboolean mirror, const UColor& c)
{
    const int    pw = (int)(w * s + 0.5f), ph = (int)(h * s + 0.5f);
    const vec4_t color = {c.r, c.g, c.b, c.a};
    qhandle_t    shader = Comp_PictureShader(pic, pw, ph, s, mirror);

    uii.Rend_SetColor(color);
    uii.Rend_DrawPicStretched(floorf(x * s + 0.5f), floorf(y * s + 0.5f), pw, ph, 0, 0, 1, 1, shader);
}

// a weapon's picture fitted in a box of design pixels: its size there
static void Comp_Fit(const char *pic, float boxW, float boxH, float s, float *w, float *h)
{
    float sw, sh, fit;

    if (!UI_OutlineSize(va(COMP_OUTLINES "%s.svg", pic), &sw, &sh)) {
        sw = sh = 1.0f;
    }
    fit = Q_min(boxW / sw, boxH / sh);
    *w  = floorf(fit * sw * s + 0.5f) / s;
    *h  = floorf(fit * sh * s + 0.5f) / s;
}

// a text's width, smaller to fit a width
static float Comp_TextW(UIFont *font, const char *text, float fit = 0.0f)
{
    const float k = font->FitScale(text, fit);

    return font->getWidth(text, -1) * k;
}

// text with its capitals' middle on cy (the baked faces' lines are centered
// so), smaller to fit a width; its width
static float Comp_Text(UIFont *font, float x, float cy, const char *text, const UColor& c, const float *scale, float fit = 0.0f)
{
    float w;

    font->setFit(font->FitScale(text, fit));
    font->setColor(c);
    font->Print(x, cy - font->getHeight((const float *)NULL) * 0.5f, text, -1, scale);
    w = font->getWidth(text, -1);
    font->setFit(1.0f);
    return w;
}

static float Comp_Fade(int time, int life, int fade)
{
    return Com_Clamp(0.0f, 1.0f, (float)(life - (cls.realtime - time)) / fade);
}

/*
================
UIHudFeed

The kill feed (feed kills: right aligned pad px before the widget's right
edge, the newest at the bottom, 6 s, fading in the last 1.5; each line in a
box reaching pad px either side of it, at least 30 tall, 4 apart: dark, red
with a red edge for the player's kill or death), the chat (feed chat: from
the bottom up) and the game messages (feed messages: from the top), 8 s
fading in the last 2; lines wrapped between words, the newest that fit.
boldfont: the white game messages'
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
    // kept while no line fades: the lines shown
    bool Lines(int *key);
    bool CacheDraw(void) override { return Lines(NULL); }
    bool DrawKeyed(void) override { return true; }
    int  DrawKey(void) override
    {
        int key = 0;

        Lines(&key);
        return key;
    }
    void DrawKills(float w, float h, float s);
};

Event EV_HudFeed_Feed("feed", EV_DEFAULT, "s", "kind", "kills, chat or messages");
Event EV_HudFeed_BoldFont("boldfont", EV_DEFAULT, "s", "font", "the bold lines' font");

CLASS_DECLARATION(UIWidget, UIHudFeed, NULL) {
    {&EV_HudFeed_Feed,     &UIHudFeed::SetFeed    },
    {&EV_HudFeed_BoldFont, &UIHudFeed::SetBoldFont},
    {NULL,                 NULL                   }
};

bool UIHudFeed::Lines(int *key)
{
    const int life = m_kind ? 8000 : 6000, fade = m_kind ? 2000 : 1500;
    const int count = m_kind == 0 ? comp.numKills : m_kind == 1 ? comp.numChat : comp.numMsgs;
    bool      steady = true;
    int       h = 0;

    for (int i = 0; i < count; i++) {
        const int time = m_kind == 0 ? comp.kills[i].time : m_kind == 1 ? comp.chat[i].time : comp.msgs[i].time;

        if (cls.realtime - time < life) {
            steady = steady && cls.realtime - time < life - fade;
            h      = UI_DrawHash(h, &time, sizeof(time));
        }
    }
    if (key) {
        *key = h;
    }
    return steady;
}

void UIHudFeed::SetFeed(Event *ev)
{
    const str kind = ev->GetString(1);

    m_kind = kind == "kills" ? 0 : kind == "chat" ? 1 : 2;
}

void UIHudFeed::SetBoldFont(Event *ev)
{
    m_bold = new UIFont(ev->GetString(1));
}

// a box of design pixels, its edges on whole pixels
static void Comp_Box(float x0, float y0, float x1, float y1, float s)
{
    const float px = floorf(x0 * s + 0.5f), py = floorf(y0 * s + 0.5f);

    uii.Rend_DrawBox(px, py, floorf(x1 * s + 0.5f) - px, floorf(y1 * s + 0.5f) - py);
}

// the kill feed's box: a small rounded box (or its edge, KF_EDGE wide) baked
// at the HUD's scale, its corners as they are, its middle texel stretched
static qhandle_t Comp_FeedBoxShader(float s, qboolean edge, int *corner, int *size)
{
    char        layer[64];
    const char *layers[1] = {layer};
    const int   c = Q_max((int)ceilf(KF_RADIUS * s), edge ? Q_max(1, (int)floorf(KF_EDGE * s + 0.5f)) : 1);

    *corner = c;
    *size   = 2 * c + 2;
    if (edge) {
        Com_sprintf(layer, sizeof(layer), "roundframe %g %g 1 1 1 1", KF_RADIUS, KF_EDGE);
    } else {
        Com_sprintf(layer, sizeof(layer), "roundbox %g 1 1 1 1", KF_RADIUS);
    }
    return UI_BakeShape("comp_kf", layers, 1, *size, *size, s);
}

// a box of design pixels with KF_RADIUS corners (or its edge), in nine parts
static void Comp_FeedBox(float x0, float y0, float x1, float y1, float s, qboolean edge)
{
    int             c, n;
    const qhandle_t shader = Comp_FeedBoxShader(s, edge, &c, &n);
    const float     px[4]  = {floorf(x0 * s + 0.5f), floorf(x0 * s + 0.5f) + c, floorf(x1 * s + 0.5f) - c, floorf(x1 * s + 0.5f)};
    const float     py[4]  = {floorf(y0 * s + 0.5f), floorf(y0 * s + 0.5f) + c, floorf(y1 * s + 0.5f) - c, floorf(y1 * s + 0.5f)};
    // the texture's parts: a corner, the middle texel (its centre: no neighbour bleeds in), the other corner
    const float     t[4][2] = {{0.0f, (float)c / n}, {(c + 0.5f) / n, (c + 1.5f) / n}, {(c + 2.0f) / n, 1.0f}};

    if (!shader || px[2] < px[1] || py[2] < py[1]) {
        if (!edge) {
            Comp_Box(x0, y0, x1, y1, s);
        }
        return;
    }
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            if (edge && row == 1 && col == 1) {
                continue; // (the edge's middle is clear)
            }
            if (px[col + 1] > px[col] && py[row + 1] > py[row]) {
                uii.Rend_DrawPicStretched(px[col], py[row], px[col + 1] - px[col], py[row + 1] - py[row], t[col][0], t[row][0], t[col][1], t[row][1], shader);
            }
        }
    }
}

// a name's color in the feed: the Allies' CS2's blue, the Axis' the HUD's
// red (Comp_TeamColor's, as the top bar's), else white
static UColor Comp_FeedNameColor(int team, float alpha)
{
    return team == TEAM_ALLIES ? UColor(0.435f, 0.612f, 0.902f, alpha) : team == TEAM_AXIS ? UColor(0.8f, 0.533f, 0.533f, alpha) : UColor(1, 1, 1, alpha);
}

void UIHudFeed::DrawKills(float w, float h, float s)
{
    float used = 0.0f, y = 0.0f;
    int   first, i, count = comp.numKills, oldest = 0;

    while (oldest < count && cls.realtime - comp.kills[oldest].time >= 6000) {
        oldest++;
    }
    // the newest that fit, the oldest on top
    for (first = count; first > oldest; first--) {
        const float rowH = KF_ROW + (used > 0.0f ? KF_SPACE : 0.0f);

        if (used + rowH > h) {
            break;
        }
        used += rowH;
    }
    for (i = first; i < count; i++) {
        const compKill_t *k     = &comp.kills[i];
        const float       alpha = Comp_Fade(k->time, 6000, 1500);
        const float       mid   = y + KF_ROW * 0.5f;
        const qboolean    mark  = k->suicide && strcmp(k->pic, "skull_50");
        const UColor      kc = Comp_FeedNameColor(k->killerTeam, alpha), vc = Comp_FeedNameColor(k->victimTeam, alpha), white(1, 1, 1, alpha);
        // a long name smaller, to fit 150 px
        const float       kw = k->killer[0] ? Comp_TextW(m_font, k->killer, 150.0f) + KF_GAP : 0.0f;
        const float       inner = kw + k->picW + KF_GAP + (mark ? KF_ICON + KF_GAP : 0.0f) + (k->headshot ? KF_ICON + KF_GAP : 0.0f)
                                + Comp_TextW(m_font, k->victim, 150.0f);
        const float       x0 = w - inner - 2 * KF_PAD;
        float             x  = x0 + KF_PAD;

        // the box: the player's death dark red, the player's kill black with a red edge, the rest dark
        {
            const vec4_t death = {0.384f, 0.016f, 0.016f, 0.80f * alpha}, kill = {0.0f, 0.0f, 0.0f, 0.90f * alpha};
            const vec4_t other = {0.0f, 0.0f, 0.0f, 0.64f * alpha}, edge = {0.882f, 0.0f, 0.0f, alpha};

            uii.Rend_SetColor(k->myDeath ? death : k->myKill ? kill : other);
            Comp_FeedBox(x0, y, w, y + KF_ROW, s, qfalse);
            if (k->myKill) {
                uii.Rend_SetColor(edge);
                Comp_FeedBox(x0, y, w, y + KF_ROW, s, qtrue);
            }
        }
        if (k->killer[0]) {
            x += Comp_Text(m_font, x, mid, k->killer, kc, getVirtualScale(), 150.0f) + KF_GAP;
        }
        // the outlines point left: the feed's at the victim
        Comp_Picture(k->pic, x, mid - k->picH * 0.5f, k->picW, k->picH, s, Comp_Outline(k->pic), white);
        x += k->picW + KF_GAP;
        if (mark) {
            Comp_Picture("skull_50", x, mid - KF_ICON * 0.5f, KF_ICON, KF_ICON, s, qfalse, white);
            x += KF_ICON + KF_GAP;
        }
        if (k->headshot) {
            Comp_Picture("headshot_50", x, mid - KF_ICON * 0.5f, KF_ICON, KF_ICON, s, qfalse, white);
            x += KF_ICON + KF_GAP;
        }
        Comp_Text(m_font, x, mid, k->victim, vc, getVirtualScale(), 150.0f);
        y += KF_ROW + KF_SPACE;
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

// a shape's layer with its color grey: the color's luminance times k (the
// color after the kind, or after the kind's one number, as uibake reads it)
static str Comp_Grey(const char *layer, float k)
{
    char        kind[32];
    const char *p = layer;
    float       c[4], v;
    int         n;
    str         out;

    if (sscanf(p, "%31s%n", kind, &n) != 1) {
        return layer;
    }
    p += n;
    out = kind;
    if (!strcmp(kind, "ring") || !strcmp(kind, "frame") || !strcmp(kind, "roundbox")) {
        if (sscanf(p, "%f%n", &v, &n) != 1) {
            return layer;
        }
        out += va(" %g", v);
        p += n;
    }
    if (sscanf(p, "%f %f %f %f%n", &c[0], &c[1], &c[2], &c[3], &n) != 4) {
        return layer;
    }
    v = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) * k;
    out += va(" %g %g %g %g", v, v, v, c[3]);
    out += p + n;
    // (an outline's parts greyed each by its own brightness, as a shape's layers are)
    if (!strcmp(kind, "svg")) {
        out += " grey";
    }
    return out;
}

// a team's cards (alive, alive with the bar under it, dead: greyed and
// darker) size design pixels wide: the box, its frame, the team's icon
// (icon: the widget whose shapes it is)
static void Comp_CardShaders(int team, UIWidget *icon, int size, float s, qhandle_t out[3])
{
    const float f  = size / (size + 8.0f), bar = (size + 4.0f) / (size + 8.0f);
    const float px = 5.0f / size, py = 5.0f / (size + 8.0f);

    for (int k = 0; k < 3; k++) {
        auto             grey = [k](const char *layer) { return k == 2 ? Comp_Grey(layer, 0.42f) : str(layer); };
        std::vector<str> lines;
        const char      *layers[40];
        int              numLayers = 0;

        lines.push_back(va("in 0 0 1 %g %s", f, grey(team == TEAM_AXIS ? "fill 0.549 0.333 0.294 0.851" : "fill 0.294 0.435 0.549 0.851").c_str()));
        for (int l = 1; size >= 16 && icon && l <= icon->getShapes().NumObjects() && l < 30; l++) {
            lines.push_back(va("in %g %g %g %g %s", px, py, 1.0f - px, f - py, grey(icon->getShapes().ObjectAt(l).c_str()).c_str()));
        }
        lines.push_back(va("in 0 0 1 %g %s", f, grey("frame 1 1 1 1 0.2").c_str()));
        if (k == 1) {
            lines.push_back(va("in 0 %g 1 1 fill 0.988 0.988 0.988 1", bar));
        }
        for (str& l : lines) {
            layers[numLayers++] = l.c_str();
        }
        out[k] = UI_BakeShape("comp_card", layers, numLayers, (int)(size * s + 0.5f), (int)((size + 8) * s + 0.5f), s);
    }
}

/*
================
UIHudCards

A card for each player of a team (team allies or axis: ui_hud_cards_<team>,
in the clients' order), laid out from the widget's right edge leftward
(allies) or its left edge rightward (axis): the team's icon (icons: the
widgets whose shapes are the Allies' and the Axis' icons), a white bar under
it while alive on the player's own team, greyed out once dead. They shrink
so the bigger team fits in about 400 px and in the widget, the rest a count
================
*/
class UIHudCards : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudCards);

    int       m_team;
    str       m_icons[2];
    int       m_size, m_stamp;
    qhandle_t m_cards[3]; // alive, alive with a bar, dead

    UIHudCards() : m_team(TEAM_ALLIES), m_size(0), m_stamp(-1) {}
    void SetTeam(Event *ev) { m_team = !str::icmp(ev->GetString(1), "axis") ? TEAM_AXIS : TEAM_ALLIES; }
    void SetIcons(Event *ev)
    {
        m_icons[0] = ev->GetString(1);
        m_icons[1] = ev->GetString(2);
    }
    void Draw(void) override;
    bool CacheDraw(void) override { return true; }
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
    static cvar_t *const allies = Cvar_Get("ui_hud_cards_allies", "", 0), *const axis = Cvar_Get("ui_hud_cards_axis", "", 0);
    const float          s      = m_vVirtualScale[0], w = m_frame.size.width / s;
    const char          *cards  = (m_team == TEAM_AXIS ? axis : allies)->string;
    const int            n      = (int)strlen(cards);
    const int            most   = Q_max(1, Q_max((int)strlen(allies->string), (int)strlen(axis->string)));

    UI_DrawDep(allies);
    UI_DrawDep(axis);
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
    // a card of each kind baked once at its size
    if (size != m_size || m_stamp != UI_BakeStamp()) {
        Comp_CardShaders(m_team, m_parent ? m_parent->FindWidget(m_icons[m_team == TEAM_AXIS]) : NULL, size, s, m_cards);
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
                                  m_cards[cards[i] == 'd' ? 2 : cards[i] == 'b' ? 1 : 0]);
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

The weapons carried, up from the widget's bottom right: the sidearm, the
primary, the grenades (frags then smokes, each with how many); the one in
hand bright and named under it. countfont: the grenades' counts'
================
*/
class UIHudWeapons : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIHudWeapons);

    UIFont *m_countFont;

    UIHudWeapons() : m_countFont(NULL) {}
    void SetCountFont(Event *ev) { m_countFont = new UIFont(ev->GetString(1)); }
    void Item(float right, float *bottom, const char **pics, const int *counts, int n, int hand, float boxW, float boxH);
    void Draw(void) override;
    bool CacheDraw(void) override { return true; }
    bool DrawKeyed(void) override { return true; }
    int  DrawKey(void) override
    {
        const playerState_t *ps   = &cl.snap.ps;
        const int            v[7] = {comp.frags, comp.smokes, comp.nation, ps->stats[STAT_WEAPONS], ps->stats[STAT_EQUIPPED_WEAPON],
                                     Comp_Int("cg_hud_grenades"), comp_accentStamp};
        const char          *hand = CL_ConfigString(CS_WEAPONS + ps->activeItems[ITEM_WEAPON]);
        int                  h    = UI_DrawHash(0, v, sizeof(v));

        h = UI_DrawHash(h, comp.primary, strlen(comp.primary));
        h = UI_DrawHash(h, comp.sidearm, strlen(comp.sidearm));
        return UI_DrawHash(h, hand, strlen(hand));
    }
};

Event EV_HudWeapons_CountFont("countfont", EV_DEFAULT, "s", "font", "the grenades' counts' font");

CLASS_DECLARATION(UIWidget, UIHudWeapons, NULL) {
    {&EV_HudWeapons_CountFont, &UIHudWeapons::SetCountFont},
    {NULL,                     NULL                       }
};

void UIHudWeapons::Item(float right, float *bottom, const char **pics, const int *counts, int n, int hand, float boxW, float boxH)
{
    const float  s      = m_vVirtualScale[0];
    const float  lineH  = m_font->getHeight((const float *)NULL);
    const UColor white(0.988f, 0.988f, 0.988f, 1.0f);
    float        pw[2], phi[2], ph = 0.0f, w = 0.0f, top, x;
    int          i;

    // each picture's size fitted in the box, the row's width and height
    for (i = 0; i < n; i++) {
        Comp_Fit(pics[i], boxW, boxH, s, &pw[i], &phi[i]);
        ph = Q_max(ph, phi[i]);
        w += pw[i] + (counts ? 4.0f + m_countFont->getWidth(va("%d", counts[i]), -1) : 0.0f) + (i + 1 < n ? (counts ? 8.0f : 0.0f) + 4.0f : 0.0f);
    }
    top = *bottom - ph - (hand >= 0 ? 8.0f + lineH : 0.0f);
    x = right - w;
    for (i = 0; i < n; i++) {
        const float alpha = hand == i ? 1.0f : 0.55f;

        Comp_Picture(pics[i], x, top + floorf((ph - phi[i]) * 0.5f * s) / s, pw[i], phi[i], s, qfalse,
                     UColor(comp_accent[ACC_WEAPON][0], comp_accent[ACC_WEAPON][1], comp_accent[ACC_WEAPON][2], alpha));
        x += pw[i];
        if (counts) {
            x += 4.0f;
            x += Comp_Text(m_countFont, x, top + ph - m_countFont->getHeight((const float *)NULL) * 0.5f, va("%d", counts[i]),
                           UColor(comp_accent[ACC_MAIN][0], comp_accent[ACC_MAIN][1], comp_accent[ACC_MAIN][2], alpha), getVirtualScale());
            x += 8.0f;
        }
        x += 4.0f;
    }
    if (hand >= 0) {
        const char *name = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[ITEM_WEAPON]);

        // a long name smaller, to fit 200 px
        Comp_Text(m_font, right - Comp_TextW(m_font, name, 200.0f), top + ph + 8.0f + lineH * 0.5f, name, white, getVirtualScale(), 200.0f);
    }
    *bottom = top - 14.0f;
}

void UIHudWeapons::Draw(void)
{
    static const int     slots[6] = {WEAPON_CLASS_PISTOL, WEAPON_CLASS_RIFLE, WEAPON_CLASS_SMG, WEAPON_CLASS_MG, WEAPON_CLASS_GRENADE, WEAPON_CLASS_HEAVY};
    const playerState_t *ps       = &cl.snap.ps;
    const int            owned    = ps->stats[STAT_WEAPONS] & 0x3F, equipped = ps->stats[STAT_EQUIPPED_WEAPON] & 0x3F;
    const float          s        = m_vVirtualScale[0];
    const float          right    = m_frame.size.width / s - 18.0f;
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

        Item(right, &bottom, &pic, NULL, 1, handSlot == 1 ? 0 : -1, 110.0f, 30.0f);
    }
    if (primarySlot && Comp_Weapon(comp.primary)) {
        const char *pic = Comp_Weapon(comp.primary)->pic;

        Item(right, &bottom, &pic, NULL, 1, handSlot == primarySlot ? 0 : -1, 160.0f, 34.0f);
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
        Item(right, &bottom, pics, counts, n, hand, 200.0f, 38.0f);
    }
}

/*
====================================================================

THE RADAR'S MAP

The floors a player can reach, walked over a grid of 16 units (box traces
down from a jump's height above each floor, on the world and the brush
entities: crates, bridges) from the spawn points and the tops of the
ladders at the map's load, and again from where a player stands that the
walk didn't reach (every 250 ms, a few thousand cells a frame). Drawn top
down into a picture on a thread of its own, again when the player changes
storey or the walk grew: each cell's floor nearest the player's height
filled, brighter the higher, its edges (no floor beside, or a ledge) as
lines, stairs and ramps striped across the way up, the player's storey's
bright

====================================================================
*/

#define RM_CELL     16.0f
#define RM_RANGE    1000.0f // units from the radar's middle to its edge
#define RM_STAIR    7.0f    // a floor this much higher beside: a stair (to a ledge's 40)
#define RM_PER_STEP 2000    // cells walked a frame after the load

typedef struct {
    float z;
    int   next; // the column's next floor, -1 none
} rmNode_t;

typedef struct {
    clipHandle_t model;
    vec3_t       origin, mins, maxs; // its bounds in the world
} rmSolid_t;

typedef struct {
    float    top, bottom;
    vec3_t   at;
    qboolean ok;
} rmDown_t;

static struct {
    char                   map[MAX_QPATH];
    float                  x0, y0, zMin, zMax;
    int                    cw, ch;
    std::vector<int>       head; // a column's first floor, -1 none
    std::vector<rmNode_t>  nodes;
    std::vector<rmSolid_t> solids;          // the brush entities stood on
    std::vector<rmDown_t>  down;            // each column's last trace down (asked again: its answer)
    std::vector<int>       queue, queueCell; // the floors still to walk out from
    size_t                 queued;
    int                    walked, seedTime; // the floors the picture has, the players' last check
    // the picture
    int       texels; // a cell's, across
    float     focusZ;
    qhandle_t shader;
    int       stamp;
    // ... made on its thread
    std::thread       worker;
    std::atomic<bool> done{false};
    std::vector<byte> px;
    int               pxW, pxH;
} rm;

// the first floor straight down from top to bottom (the world's or a brush
// entity's, the higher), not too steep, not one of column c's (within 24
// units: found again it adds nothing), with room for a crouched player. A
// column's trace down asked again (its neighbours on a flat floor ask the
// same) gives the answer it gave
static qboolean RM_Floor(int c, float x, float y, float top, float bottom, float *z)
{
    static const vec3_t mins = {-7, -7, 0}, maxs = {7, 7, 1}, cmins = {-15, -15, 18}, cmaxs = {15, 15, 50};
    const vec3_t        start = {x, y, top}, end = {x, y, bottom};
    trace_t             tr, t;
    rmDown_t            own, *d = c < (int)rm.down.size() ? &rm.down[c] : &own;

    if (d == &own || d->top != top || d->bottom != bottom) {
        CM_BoxTrace(&tr, start, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse);
        for (const rmSolid_t& s : rm.solids) {
            if (x < s.mins[0] - 8 || x > s.maxs[0] + 8 || y < s.mins[1] - 8 || y > s.maxs[1] + 8 || s.mins[2] > top || s.maxs[2] < bottom) {
                continue;
            }
            CM_TransformedBoxTrace(&t, start, end, mins, maxs, s.model, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, s.origin, vec3_origin, qfalse);
            if (t.startsolid || t.fraction < tr.fraction) {
                tr = t;
            }
        }
        *d = {top, bottom, {tr.endpos[0], tr.endpos[1], tr.endpos[2]}, !(tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal[2] < 0.7f)};
    }
    if (!d->ok) {
        return qfalse;
    }
    *z = d->at[2];
    for (int n = rm.head[c]; n >= 0; n = rm.nodes[n].next) {
        if (fabsf(rm.nodes[n].z - *z) < 24.0f) {
            return qfalse;
        }
    }
    // a crouched player has room there
    CM_BoxTrace(&t, d->at, d->at, cmins, cmaxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse);
    for (const rmSolid_t& s : rm.solids) {
        if (t.startsolid || t.allsolid) {
            break;
        }
        if (x >= s.mins[0] - 16 && x <= s.maxs[0] + 16 && y >= s.mins[1] - 16 && y <= s.maxs[1] + 16) {
            CM_TransformedBoxTrace(&t, d->at, d->at, cmins, cmaxs, s.model, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, s.origin, vec3_origin, qfalse);
        }
    }
    return !t.startsolid && !t.allsolid;
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

// a floor to walk out from: the one under x y below z
static void RM_Seed(float x, float y, float top, float bottom)
{
    const int cx = (int)((x - rm.x0) / RM_CELL), cy = (int)((y - rm.y0) / RM_CELL);
    qboolean  added;
    float     z;

    if (cx >= 0 && cy >= 0 && cx < rm.cw && cy < rm.ch
        && RM_Floor(cy * rm.cw + cx, rm.x0 + (cx + 0.5f) * RM_CELL, rm.y0 + (cy + 0.5f) * RM_CELL, top, bottom, &z)) {
        const int n = RM_Node(cy * rm.cw + cx, z, &added);

        if (added) {
            rm.queue.push_back(n);
            rm.queueCell.push_back(cy * rm.cw + cx);
        }
    }
}

// out from the floors queued: onto what a jump reaches, and down from any height
static void RM_Step(size_t most)
{
    for (; rm.queued < rm.queue.size() && most; rm.queued++, most--) {
        const int   c = rm.queueCell[rm.queued], cx = c % rm.cw, cy = c / rm.cw;
        const float z = rm.nodes[rm.queue[rm.queued]].z;
        qboolean    added;

        rm.zMin = Q_min(rm.zMin, z);
        rm.zMax = Q_max(rm.zMax, z);
        for (int d = 0; d < 4; d++) {
            const int nx = cx + (d == 0) - (d == 1), ny = cy + (d == 2) - (d == 3);
            float     nz;

            if (nx < 0 || ny < 0 || nx >= rm.cw || ny >= rm.ch
                || !RM_Floor(ny * rm.cw + nx, rm.x0 + (nx + 0.5f) * RM_CELL, rm.y0 + (ny + 0.5f) * RM_CELL, z + 72.0f, z - 4096.0f, &nz)) {
                continue;
            }
            const int n = RM_Node(ny * rm.cw + nx, nz, &added);

            if (added) {
                rm.queue.push_back(n);
                rm.queueCell.push_back(ny * rm.cw + nx);
            }
        }
    }
    if (rm.queued == rm.queue.size()) {
        rm.queue.clear();
        rm.queueCell.clear();
        rm.queued = 0;
    }
}

static void RM_Walk(void)
{
    const char *p = CM_EntityString();
    vec3_t      mins, maxs;
    const int   start = Sys_Milliseconds();

    CM_ModelBounds(CM_InlineModel(0), mins, maxs);
    rm.x0 = mins[0];
    rm.y0 = mins[1];
    rm.cw = Q_min(2048, (int)((maxs[0] - mins[0]) / RM_CELL) + 1);
    rm.ch = Q_min(2048, (int)((maxs[1] - mins[1]) / RM_CELL) + 1);
    rm.head.assign(rm.cw * rm.ch, -1);
    // (up to a million columns: 24 MB)
    rm.down.assign(rm.cw * rm.ch <= (1 << 20) ? rm.cw * rm.ch : 0, {NAN, NAN, {0, 0, 0}, qfalse});
    rm.nodes.clear();
    rm.solids.clear();
    rm.queue.clear();
    rm.queueCell.clear();
    rm.queued = 0;
    rm.zMin   = 99999.0f;
    rm.zMax   = -99999.0f;
    // the brush entities one stands on (not the triggers, ladders, doors or
    // windows) first, then the spawn points and the tops of the ladders
    for (int pass = 0; pass < 2; pass++) {
        p = CM_EntityString();
        for (const char *tok = COM_Parse((char **)&p); p && tok[0] == '{'; tok = COM_Parse((char **)&p)) {
            char   classname[64] = "", model[16] = "";
            vec3_t origin        = {0, 0, 0};

            for (tok = COM_Parse((char **)&p); p && tok[0] != '}'; tok = COM_Parse((char **)&p)) {
                char key[64];

                Q_strncpyz(key, tok, sizeof(key));
                tok = COM_Parse((char **)&p);
                if (!Q_stricmp(key, "classname")) {
                    Q_strncpyz(classname, tok, sizeof(classname));
                } else if (!Q_stricmp(key, "origin")) {
                    sscanf(tok, "%f %f %f", &origin[0], &origin[1], &origin[2]);
                } else if (!Q_stricmp(key, "model")) {
                    Q_strncpyz(model, tok, sizeof(model));
                }
            }
            const int index = model[0] == '*' ? atoi(model + 1) : 0;

            if (index > 0 && index < CM_NumInlineModels()) {
                rmSolid_t s;

                s.model = CM_InlineModel(index);
                VectorCopy(origin, s.origin);
                CM_ModelBounds(s.model, s.mins, s.maxs);
                VectorAdd(s.mins, origin, s.mins);
                VectorAdd(s.maxs, origin, s.maxs);
                if (!pass && Q_stricmpn(classname, "trigger_", 8) && Q_stricmp(classname, "func_ladder") && !Q_stristr(classname, "door")
                    && Q_stricmp(classname, "func_window") && Q_stricmp(classname, "func_areaportal")) {
                    rm.solids.push_back(s);
                } else if (pass && !Q_stricmp(classname, "func_ladder")) {
                    // its top: the floors beside it
                    for (float x = s.mins[0] - RM_CELL; x <= s.maxs[0] + RM_CELL; x += RM_CELL) {
                        for (float y = s.mins[1] - RM_CELL; y <= s.maxs[1] + RM_CELL; y += RM_CELL) {
                            RM_Seed(x, y, s.maxs[2] + 72.0f, s.maxs[2] - 64.0f);
                        }
                    }
                }
            }
            if (pass && !Q_stricmpn(classname, "info_player_", 12) && Q_stricmp(classname, "info_player_intermission")) {
                RM_Seed(origin[0], origin[1], origin[2] + 40.0f, origin[2] - 200.0f);
            }
        }
    }
    RM_Step((size_t)-1);
    rm.walked = (int)rm.nodes.size();
    Com_DPrintf("Radar: %d floors (%d brush entities) in %d ms\n", (int)rm.nodes.size(), (int)rm.solids.size(), Sys_Milliseconds() - start);
}

// where a player stands that the walk didn't reach: walked out from too
static void RM_SeedPlayers(const vec3_t eye)
{
    if ((unsigned)(cls.realtime - rm.seedTime) < 250) {
        return;
    }
    rm.seedTime = cls.realtime;
    RM_Seed(eye[0], eye[1], eye[2] - 32.0f, eye[2] - 160.0f);
    for (int i = 0; g_radarClients && i < MAX_CLIENTS; i++) {
        const radarClient_t *c = &g_radarClients[i];

        if (c->time && cl.snap.serverTime - c->time < 5000) {
            RM_Seed(c->origin[0], c->origin[1], c->origin[2] + 32.0f, c->origin[2] - 96.0f);
        }
    }
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

// the picture's texels (on the worker thread: the walk doesn't change meanwhile)
static void RM_Draw(void)
{
    const int t = rm.texels, w = rm.cw * t, h = rm.ch * t;

    rm.px.assign(w * h * 4, 0);
    for (int cy = 0; cy < rm.ch; cy++) {
        for (int cx = 0; cx < rm.cw; cx++) {
            const int n = RM_Nearest(cx, cy, rm.focusZ);
            float     z, focus;
            byte      fill, line, stairLine, edge[4], stair[4]; // left, right, north, south

            if (n < 0) {
                continue;
            }
            z         = rm.nodes[n].z;
            focus     = Com_Clamp(0.0f, 1.0f, (120.0f - fabsf(z - rm.focusZ)) / 56.0f);
            fill      = (byte)((0.2f + 0.24f * (rm.zMax > rm.zMin ? (z - rm.zMin) / (rm.zMax - rm.zMin) : 1.0f)) * 255.0f);
            line      = (byte)((0.3f + 0.6f * focus) * 255.0f);
            stairLine = (byte)((fill / 255.0f + (0.22f + 0.3f * focus - fill / 255.0f) * 0.8f) * 255.0f);
            // the edges: no floor beside within a step (a wall, a drop, a
            // ledge); a stair: one a little higher, striped every other cell
            for (int k = 0; k < 4; k++) {
                const int nb = RM_Nearest(cx + (k == 1) - (k == 0), cy + (k == 2) - (k == 3), z);
                const float dz = nb < 0 ? 0.0f : rm.nodes[nb].z - z;

                edge[k]  = nb < 0 || fabsf(dz) > 40.0f;
                stair[k] = !edge[k] && dz >= RM_STAIR && ((k < 2 ? cx : cy) & 1) == 0;
            }
            // the picture's top row is the map's north (y up)
            for (int j = 0; j < t; j++) {
                byte *o = &rm.px[(((rm.ch - 1 - cy) * t + j) * w + cx * t) * 4];

                for (int i = 0; i < t; i++, o += 4) {
                    const int side = i == 0 ? 0 : i == t - 1 ? 1 : j == t - 1 ? 2 : j == 0 ? 3 : -1;

                    o[0] = 235;
                    o[1] = 240;
                    o[2] = 245;
                    o[3] = side >= 0 && edge[side] ? line : side >= 0 && stair[side] ? stairLine : fill;
                }
            }
        }
    }
    rm.pxW = w;
    rm.pxH = h;
    rm.done = true;
}

// a picture begun on its thread (none while one is being made)
static void RM_Picture(void)
{
    if (rm.worker.joinable()) {
        return;
    }
    rm.done   = false;
    rm.worker = std::thread(RM_Draw);
}

void UI_CompHud_Shutdown(void)
{
    if (rm.worker.joinable()) {
        rm.worker.join();
    }
    // (no picture's texels made after the UI)
    UI_BakeRelease();
}

// a picture made: the radar's (the main thread's: the renderer)
static void RM_PictureDone(void)
{
    if (!rm.worker.joinable() || !rm.done) {
        return;
    }
    rm.worker.join();
    rm.shader = uii.Rend_RegisterImage2D("ui_bake/radar", rm.px.data(), rm.pxW, rm.pxH);
    rm.stamp  = UI_BakeStamp();
}

// an objective's diamond (white for the current one), its box d pixels
// a mark's shader and size kept while its scale and the baked images stay
typedef struct {
    int       stamp, d;
    float     k, s;
    qhandle_t h;
} compMark_t;

static bool Comp_MarkKept(compMark_t *m, float k, float s, int *d)
{
    if (m->h && m->stamp == UI_BakeStamp() && m->k == k && m->s == s) {
        *d = m->d;
        return true;
    }
    return false;
}

static qhandle_t Comp_MarkKeep(compMark_t *m, float k, float s, int d, qhandle_t h)
{
    *m = {UI_BakeStamp(), d, k, s, h};
    return h;
}

static qhandle_t Comp_ObjectiveShader(qboolean current, float k, float s, int *d)
{
    static compMark_t kept[2];

    if (Comp_MarkKept(&kept[current], k, s, d)) {
        return kept[current].h;
    }
    const char *layers[2] = {"poly 0.047 0.055 0.071 0.9 0.5 0 1 0.5 0.5 1 0 0.5",
                             current ? "poly 1 1 1 0.95 0.5 0.12 0.88 0.5 0.5 0.88 0.12 0.5" : "poly 0.75 0.75 0.75 0.8 0.5 0.12 0.88 0.5 0.5 0.88 0.12 0.5"};

    *d = (int)(((current ? 5.5f : 4.25f) + 1.75f) * k * 2.0f * s + 0.5f);
    return Comp_MarkKeep(&kept[current], k, s, *d, UI_BakeShape("comp_obj", layers, 2, *d, *d, s));
}

// a teammate's dot, its dark edge and (on the radar, not at its edge) its
// pointer up, in a box of 2 x 2.6 radii, d pixels
static qhandle_t Comp_DotShader(int axis, qboolean clamped, float k, float s, int *d)
{
    static compMark_t kept[2][2];

    if (Comp_MarkKept(&kept[axis][clamped], k, s, d)) {
        return kept[axis][clamped].h;
    }
    const char *color = axis ? "0.8 0.533 0.533 1" : "0.533 0.569 0.8 1";
    const float rad = (clamped ? 3.9f : 5.5f) * k, rim = rad + 1.25f * k, half = Q_max(2.6f * rad, rim) + 1.0f;
    char        lines[3][96];
    const char *layers[3] = {lines[0], lines[1], lines[2]};

    Com_sprintf(lines[0], 96, clamped ? "fill 0 0 0 0" : "poly %s 0.5 %g %g %g %g %g", color, 0.5f - 2.6f * rad / half * 0.5f,
                0.5f + 0.85f * rim / half * 0.5f, 0.5f - 0.55f * rim / half * 0.5f, 0.5f - 0.85f * rim / half * 0.5f, 0.5f - 0.55f * rim / half * 0.5f);
    Com_sprintf(lines[1], 96, "circle 0.047 0.055 0.071 0.9 %g", rim / half);
    Com_sprintf(lines[2], 96, "circle %s %g", color, rad / half);
    *d = (int)(half * 2.0f * s + 0.5f);
    return Comp_MarkKeep(&kept[axis][clamped], k, s, *d, UI_BakeShape("comp_dot", layers, 3, *d, *d, s));
}

// the player's arrow, up
static qhandle_t Comp_ArrowShader(float k, float s, int *d)
{
    static compMark_t kept;

    if (Comp_MarkKept(&kept, k, s, d)) {
        return kept.h;
    }
    const char *layers[2] = {"poly 0.047 0.055 0.071 0.9 0.5 0.0625 0.8125 0.8125 0.1875 0.8125", "poly 1 1 1 1 0.5 0.1875 0.7232 0.7232 0.2768 0.7232"};

    *d = (int)(16.0f * 1.4f * k * s + 0.5f);
    return Comp_MarkKeep(&kept, k, s, *d, UI_BakeShape("comp_arrow", layers, 2, *d, *d, s));
}

static void Comp_PreBakeWidget(UIWidget *w);

/*
================
UI_CompHud_PreBake

At a map's load and after the renderer's restart: every picture the HUD can
draw baked now (the menus' shapes and faces, the kill feed's and the weapons'
pictures, the cards, the radar's marks), none at its first draw. Ahead (at
the UI's start and a map load's): their texels begun on threads of their
own, made while the rest loads, so the bake at the load's end finds them made
================
*/
void UI_CompHud_PreBake(qboolean ahead)
{
    const float s = UI_DesignScale(1920.0f, 1080.0f), k = 248.0f / 220.0f;
    const int   start = Sys_Milliseconds();
    UIWidget   *icons[2] = {NULL, NULL};
    qhandle_t   cards[3];
    int         d;
    size_t      i;

    if (!UI_CompHud_Active() || !uii.IsRendererLoaded()) {
        return;
    }
    UI_BakeAhead(ahead);
    for (i = 0; i < UI_COMPHUD_MENUS; i++) {
        Menu *m = menuManager.FindMenu(ui_compHudMenus[i]);

        if (m && m->GetContainerWidget()) {
            Comp_PreBakeWidget(m->GetContainerWidget());
        }
    }
    for (i = 0; i < ARRAY_LEN(comp_killPics); i++) {
        const compKillPic_t *p = &comp_killPics[i];
        const qboolean       mirror = Comp_Outline(p->allies);
        float                w, h;

        Comp_KillIconSize(p, qfalse, &w, &h);
        Comp_PictureShader(p->allies, (int)(w * s + 0.5f), (int)(h * s + 0.5f), s, mirror);
        Comp_KillIconSize(p, qtrue, &w, &h);
        Comp_PictureShader(p->axis, (int)(w * s + 0.5f), (int)(h * s + 0.5f), s, mirror);
    }
    Comp_PictureShader("skull_50", (int)(KF_ICON * s + 0.5f), (int)(KF_ICON * s + 0.5f), s, qfalse);
    Comp_PictureShader("headshot_50", (int)(KF_ICON * s + 0.5f), (int)(KF_ICON * s + 0.5f), s, qfalse);
    {
        int c, n;

        Comp_FeedBoxShader(s, qfalse, &c, &n);
        Comp_FeedBoxShader(s, qtrue, &c, &n);
    }
    for (i = 0; i < ARRAY_LEN(comp_weapons); i++) {
        const int kind = comp_weapons[i].kind;
        float     w, h;

        Comp_Fit(comp_weapons[i].pic, kind == WK_SIDEARM ? 110.0f : kind == WK_PRIMARY ? 160.0f : 200.0f,
                 kind == WK_SIDEARM ? 30.0f : kind == WK_PRIMARY ? 34.0f : 38.0f, s, &w, &h);
        Comp_PictureShader(comp_weapons[i].pic, (int)(w * s + 0.5f), (int)(h * s + 0.5f), s, qfalse);
    }
    if (menuManager.FindMenu("hud_comp_score")) {
        UIWidget *score = menuManager.FindMenu("hud_comp_score")->GetContainerWidget();

        icons[0] = score ? score->FindWidget("iconallies") : NULL;
        icons[1] = score ? score->FindWidget("iconaxis") : NULL;
    }
    Comp_CardShaders(TEAM_ALLIES, icons[0], 48, s, cards);
    Comp_CardShaders(TEAM_AXIS, icons[1], 48, s, cards);
    for (int axis = 0; axis < 2; axis++) {
        Comp_DotShader(axis, qfalse, k, s, &d);
        Comp_DotShader(axis, qtrue, k, s, &d);
    }
    Comp_ArrowShader(k, s, &d);
    Comp_ObjectiveShader(qtrue, k, s, &d);
    Comp_ObjectiveShader(qfalse, k, s, &d);
    UI_BakeAhead(0);
    Com_DPrintf("Competitive HUD: %s in %d ms\n", ahead ? "begun ahead" : "baked", Sys_Milliseconds() - start);
}

void UI_CompHud_MapLoaded(void)
{
    const char *map = (Comp_ConfigStrings(), cs.map);

    UI_CompHud_PreBake(qfalse);
    UI_BakeRelease();
    if (!UI_CompHud_Active() || !Comp_Int("cg_hud_radar") || !strcmp(map, rm.map)) {
        return;
    }
    if (rm.worker.joinable()) {
        rm.worker.join();
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
    float       sn, cn;
    const char *map = (Comp_ConfigStrings(), cs.map);

    if (!cl.snap.valid) {
        return;
    }
    cge->CG_EyeAngles(&angles);
    cge->CG_EyePosition(&eye);
    sn = sinf(DEG2RAD(angles[YAW]));
    cn = cosf(DEG2RAD(angles[YAW]));
    // world offset to screen: up the view's forward, right its right
    auto place = [&](float wx, float wy, float *x, float *y) {
        const float dx = wx - eye[0], dy = wy - eye[1];

        *x = (dx * sn - dy * cn) / upp;
        *y = -(dx * cn + dy * sn) / upp;
    };

    // the map: walked once a map (at its load: UI_CompHud_MapLoaded), drawn
    // again a storey away or after a restart
    if (strcmp(map, rm.map)) {
        UI_CompHud_MapLoaded();
    }
    if (!rm.nodes.empty()) {
        const int texels = Q_max(1, Q_min(6, (int)(RM_CELL / upp + 0.5f)));

        // (the walk changes only while no picture is being made of it)
        RM_PictureDone();
        if (!rm.worker.joinable()) {
            RM_SeedPlayers(eye);
            RM_Step(RM_PER_STEP);
        }
        if (!rm.worker.joinable()
            && (rm.stamp != UI_BakeStamp() || texels != rm.texels || fabsf(eye[2] - 64.0f - rm.focusZ) > 24.0f
                || (rm.walked != (int)rm.nodes.size() && rm.queue.empty()))) {
            rm.texels = Q_min(texels, 4096 / Q_max(rm.cw, rm.ch));
            rm.focusZ = eye[2] - 64.0f;
            rm.walked = (int)rm.nodes.size();
            RM_Picture();
        }
    }
    if (!rm.nodes.empty() && rm.shader) {
        // the circle's points, their texels the world's under them
        {
            static float circle[48][2];
            const float  tw = rm.cw * RM_CELL, th = rm.ch * RM_CELL;
            const vec4_t white = {1, 1, 1, 1};
            float        pts[48][4];

            if (!circle[0][0]) {
                for (int i = 0; i < 48; i++) {
                    circle[i][0] = cosf(i * 2.0f * M_PI / 48.0f);
                    circle[i][1] = sinf(i * 2.0f * M_PI / 48.0f);
                }
            }
            for (int i = 0; i < 48; i++) {
                const float px = circle[i][0] * r, py = circle[i][1] * r;
                // the screen offset back to the world
                const float wx = eye[0] + (px * sn - py * cn) * upp, wy = eye[1] + (-px * cn - py * sn) * upp;

                pts[i][0] = r + px;
                pts[i][1] = r + py;
                pts[i][2] = (wx - rm.x0) / tw;
                pts[i][3] = 1.0f - (wy - rm.y0) / th;
            }
            uii.Rend_SetColor(white);
            re.DrawPolygon2D(pts, 48, rm.shader);
        }
    }

    // the objectives: a diamond, white for the current one
    for (int i = 0; i < MAX_OBJECTIVES; i++) {
        const int flags = cs.objFlags[i];
        float     x, y;

        if (!(flags & OBJ_FLAG_CURRENT) || (flags & (OBJ_FLAG_HIDDEN | OBJ_FLAG_COMPLETED))) {
            continue;
        }
        {
            int             d;
            const qhandle_t sh = Comp_ObjectiveShader(i == cs.objCurrent, k, s, &d);

            place(cs.objLoc[i][0], cs.objLoc[i][1], &x, &y);
            if (sqrtf(x * x + y * y) > r - 7.0f * k * s) {
                const float f = (r - 7.0f * k * s) / sqrtf(x * x + y * y);

                x *= f;
                y *= f;
            }
            uii.Rend_DrawPicStretched(floorf(r + x - d * 0.5f), floorf(r + y - d * 0.5f), d, d, 0, 0, 1, 1, sh);
        }
    }

    // the teammates: a dot with a dark edge and a point where they look
    if (g_radarClients && g_radarClientNum >= 0) {
        const int axis = cl.snap.ps.stats[STAT_TEAM] == TEAM_AXIS;

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
                int             d;
                const qhandle_t sh  = Comp_DotShader(axis, clamped, k, s, &d);
                const float     a   = DEG2RAD(AngleSubtract(angles[YAW], RAD2DEG(atan2f(c->axis[1], c->axis[0]))));
                const vec4_t    white = {1, 1, 1, 1};

                uii.Rend_SetColor(white);
                uii.Rend_DrawPicStretched2(floorf(r + x - d * 0.5f), floorf(r + y - d * 0.5f), d, d, 0, 0, 1, 1, cosf(a), sinf(a), sh);
            }
        }
    }

    // the player: an arrow in the middle, up
    {
        int             d;
        const qhandle_t sh = Comp_ArrowShader(k, s, &d);

        uii.Rend_DrawPicStretched(floorf(r - d * 0.5f), floorf(r - d * 0.5f), d, d, 0, 0, 1, 1, sh);
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

// a menu's widgets: their shapes and faces (the widgets' own faces too)
static void Comp_PreBakeWidget(UIWidget *w)
{
    const bool faces = !UI_BakeAhead(); // (baked where they are now, not made ahead)

    w->BakeShapes();
    if (w->getFont() && faces) {
        w->getFont()->getHeight((const float *)NULL);
    }
    if (w->isSubclassOf(UIHudFeed) && ((UIHudFeed *)w)->m_bold && faces) {
        ((UIHudFeed *)w)->m_bold->getHeight((const float *)NULL);
    }
    if (w->isSubclassOf(UIHudWeapons) && ((UIHudWeapons *)w)->m_countFont && faces) {
        ((UIHudWeapons *)w)->m_countFont->getHeight((const float *)NULL);
    }
    for (UIWidget *c = w->getFirstChild(); c; c = w->getNextChild(c)) {
        Comp_PreBakeWidget(c);
    }
}
