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

// cg_followeyes.c -- Added in Reloaded: a followed player watched through
// their eyes on any server (cg_spectatefirstperson)
//
// Every server (Allied Assault 1.11, Spearhead, Breakthrough, OpenMoHAA) sends
// a spectator who follows a player a camera behind that player (fgame's
// Player::SetPlayerView and GetSpectateFollowOrientation, the same code in the
// original 1.11 game library):
//
//   ps.camera_angles = the player's view angles, the pitch plus
//                      g_spectatefollow_pitch (2) x the trace's fraction
//   ps.camera_origin = their eye + forward x -56 + up x 24 + right x 0.65 x
//                      their lean (g_spectatefollow_forward / _up), pulled in
//                      toward the top of their head when a wall is in the way
//
// Each snapshot read is rewritten into what a server of this code sends with
// g_spectatefollow_firstperson 1: the playerstate under the followed player's
// number, their origin, view angles, eye height and lean worked back from the
// camera, and their weapon and its first-person animation read from the gun in
// their hands and their upper body's animation. The rest of the cgame then
// draws that player's entity as its first-person model, as for our own player.
// What servers don't send to a spectator stays out: the ammo, the red flash
// and the view shake when hit. A server of this code seen showing followers
// the player's own view itself is asked for the view set here instead
// (spectatemode), which has their ammo too.

#include "cg_local.h"
#include "../corepp/tiki.h"

cvar_t *cg_spectatefirstperson;

// the servers' follow camera (g_spectatefollow_forward, _up, _pitch defaults; the lean's share)
#define FE_CAM_FORWARD -56.0f
#define FE_CAM_UP      24.0f
#define FE_CAM_PITCH   2.0f
#define FE_CAM_LEAN    0.65f

// the player's model's animation slots: the legs in 0 and 1, the upper body in 2 and 3
#define FE_TORSO_SLOT 2

typedef struct {
    int   index;
    float time;
} feAnim_t;

static struct {
    int      follow;     // the player watched through their eyes in the last snapshot read, -1 none
    int      serverTime; // that snapshot's
    vec3_t   origin;     // their origin in it
    float    lean;       // their lean, kept while the camera is pulled in
    int      weaponEnt; // the gun in their hands, its animation and their upper body's
    feAnim_t weaponAnim;
    feAnim_t torsoAnim;
    int      vmAnim; // the first-person animation made from them
    int      vmAnimChanged;
    // the entity the cgame draws as its first-person model, and for each player
    // what is added to their usageIndex (see FE_Salt)
    int      viewEnt;
    int      salt[MAX_CLIENTS];
    // the server shows followers the player's own view itself (see CG_FollowEyes_Snapshot), and the
    // spectatemode last asked of it (-1 none)
    qboolean serverMode;
    int      modeSent;
} fe;

/*
===============
CG_FollowEyes_Init
===============
*/
void CG_FollowEyes_Init(void)
{
    cg_spectatefirstperson = cgi.Cvar_Get("cg_spectatefirstperson", "0", CVAR_ARCHIVE);

    memset(&fe, 0, sizeof(fe));
    fe.follow    = -1;
    fe.weaponEnt = -1;
    fe.viewEnt   = -1;
    fe.modeSent  = -1;
}

/*
===============
FE_ServerInitArg

An argument of the model's server init commands ("name", "weapontype"), read from the client's copy
===============
*/
static const char *FE_ServerInitArg(dtiki_t *tiki, const char *key)
{
    int i;

    if (!tiki || !tiki->a) {
        return NULL;
    }

    for (i = 0; i < tiki->a->num_server_initcmds; i++) {
        const dtikicmd_t *cmd = &tiki->a->server_initcmds[i];

        if (cmd->num_args >= 2 && !Q_stricmp(cmd->args[0], key)) {
            return cmd->args[1];
        }
    }

    return NULL;
}

static int FE_WeaponClass(const char *type)
{
    static const struct {
        const char *name;
        int         wclass;
    } classes[] = {
        {"pistol",  WEAPON_CLASS_PISTOL },
        {"rifle",   WEAPON_CLASS_RIFLE  },
        {"smg",     WEAPON_CLASS_SMG    },
        {"mg",      WEAPON_CLASS_MG     },
        {"grenade", WEAPON_CLASS_GRENADE},
        {"heavy",   WEAPON_CLASS_HEAVY  },
        {"cannon",  WEAPON_CLASS_CANNON },
        {"item",    WEAPON_CLASS_ITEM   },
        {"item1",   WEAPON_CLASS_ITEM1  },
        {"item2",   WEAPON_CLASS_ITEM2  },
        {"item3",   WEAPON_CLASS_ITEM3  },
        {"item4",   WEAPON_CLASS_ITEM4  },
    };
    int i;

    for (i = 0; type && i < (int)ARRAY_LEN(classes); i++) {
        if (!Q_stricmp(type, classes[i].name)) {
            return classes[i].wclass;
        }
    }

    return 0;
}

/*
===============
FE_Crosshair

Whether the player sees a crosshair with that gun, as the server works it out (Player::UpdateStats,
Weapon::GetUseCrosshair): not through a scope, nor with an item; always in Allied Assault; in Spearhead
and Breakthrough the gun's own setting (dmcrosshair over crosshair, off when not given), and off while
moving faster than the gun can be fired at (maxfiremovement x movementspeed of the run speed)
===============
*/
static int FE_Crosshair(dtiki_t *tiki, const playerState_t *ps)
{
    const char *arg;
    float       maxFire, moveSpeed;

    if (ps->stats[STAT_INZOOM] && ps->stats[STAT_INZOOM] <= 30) {
        return 0;
    }

    arg = FE_ServerInitArg(tiki, "classname");
    if (arg && !Q_stricmp(arg, "InventoryItem")) {
        return 0;
    }

    if (cg_protocol < PROTOCOL_MOHTA_MIN) {
        return 1;
    }

    arg = FE_ServerInitArg(tiki, "dmcrosshair");
    if (!arg) {
        arg = FE_ServerInitArg(tiki, "crosshair");
    }
    if (!arg || !atoi(arg)) {
        return 0;
    }

    arg     = FE_ServerInitArg(tiki, "maxfiremovement");
    maxFire = arg ? atof(arg) : 1;
    arg     = FE_ServerInitArg(tiki, "dmmovementspeed");
    if (!arg) {
        arg = FE_ServerInitArg(tiki, "movementspeed");
    }
    moveSpeed = arg ? atof(arg) : 1;

    // (of Spearhead's and Breakthrough's run speed, sv_runspeed 287)
    if (maxFire < 1 && sqrt(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]) / 287.0f > maxFire * moveSpeed) {
        return 0;
    }

    return 1;
}

/*
===============
FE_ItemIndex

The weapon's item number (playerState_t activeItems), from the names the server registered
===============
*/
static int FE_ItemIndex(const char *name)
{
    int i;

    for (i = 1; CS_WEAPONS + i < CS_SOUNDTRACK; i++) {
        if (!Q_stricmp(CG_ConfigString(CS_WEAPONS + i), name)) {
            return i;
        }
    }

    return -1;
}

/*
===============
FE_AnimChanged

Whether an animation started since it was last read (another one, or the same one again)
===============
*/
static qboolean FE_AnimChanged(feAnim_t *last, int index, float time)
{
    qboolean changed = index != last->index || time + 0.01f < last->time;

    last->index = index;
    last->time  = time;
    return changed;
}

/*
===============
FE_WeaponVMAnim

The first-person animation for a gun's own (Weapon::SetWeaponAnim), -1 for none
===============
*/
static int FE_WeaponVMAnim(const char *anim)
{
    if (!Q_stricmpn(anim, "secondaryfire", 13)) {
        return VM_ANIM_FIRE_SECONDARY;
    }
    if (!Q_stricmpn(anim, "secondarycharge", 15) || !Q_stricmpn(anim, "charge", 6)) {
        return VM_ANIM_CHARGE;
    }
    if (!Q_stricmpn(anim, "fire", 4)) {
        return VM_ANIM_FIRE;
    }
    if (!Q_stricmp(anim, "reload_single")) {
        return VM_ANIM_RELOAD_SINGLE;
    }
    if (!Q_stricmp(anim, "reload_end")) {
        return VM_ANIM_RELOAD_END;
    }
    if (!Q_stricmpn(anim, "reload", 6)) {
        return VM_ANIM_RELOAD;
    }
    if (!Q_stricmp(anim, "raise")) {
        return VM_ANIM_PULLOUT;
    }
    if (!Q_stricmp(anim, "putaway")) {
        return VM_ANIM_PUTAWAY;
    }
    if (!Q_stricmpn(anim, "idle", 4)) {
        return VM_ANIM_IDLE;
    }

    return -1;
}

/*
===============
FE_TorsoVMAnim

The first-person animation for the upper body's (the player's state machine: "kar98_stand_rechamber"),
for what the gun's own don't show, -1 for none
===============
*/
static int FE_TorsoVMAnim(const char *anim)
{
    if (strstr(anim, "rechamber")) {
        return VM_ANIM_RECHAMBER;
    }
    if (strstr(anim, "reload_end")) {
        return VM_ANIM_RELOAD_END;
    }
    if (strstr(anim, "ladder")) {
        return VM_ANIM_LADDERSTEP;
    }

    return -1;
}

/*
===============
FE_FindWeapon

The gun in the player's hands: an entity on them whose model is a weapon
===============
*/
static const entityState_t *FE_FindWeapon(const snapshot_t *snap, int follow, dtiki_t **tikiOut)
{
    int i;

    for (i = 0; i < snap->numEntities; i++) {
        const entityState_t *es = &snap->entities[i];
        dtiki_t             *tiki;

        if (es->parent != follow || !es->modelindex) {
            continue;
        }

        tiki = cgi.R_Model_GetHandle(cgs.model_draw[es->modelindex]);
        if (FE_ServerInitArg(tiki, "weapontype") && FE_ServerInitArg(tiki, "name")) {
            *tikiOut = tiki;
            return es;
        }
    }

    return NULL;
}

/*
===============
FE_Camera

Works the followed player's view angles, eye height and lean back from the follow camera.
Returns qfalse when the camera isn't one behind them.
===============
*/
static qboolean
FE_Camera(const playerState_t *ps, const entityState_t *pl, vec3_t viewangles, float *viewheight, float *lean)
{
    vec3_t ang, fwd, right, up, d, mins, maxs, start, ideal;
    float  vh, ln, resid, frac, len;

    VectorCopy(ps->camera_angles, ang);
    ang[ROLL] = 0;

    // the camera in the open, as the servers' defaults put it
    ang[PITCH] -= FE_CAM_PITCH;
    AngleVectors(ang, fwd, right, up);

    VectorSubtract(ps->camera_origin, pl->origin, d);
    VectorMA(d, -FE_CAM_FORWARD, fwd, d);
    VectorMA(d, -FE_CAM_UP, up, d);
    ln = DotProduct(d, right) / FE_CAM_LEAN;
    VectorMA(d, -FE_CAM_LEAN * ln, right, d);
    vh    = d[2];
    resid = sqrt(d[0] * d[0] + d[1] * d[1]);

    if (resid < 4 && vh > 4 && vh < 100 && fabs(ln) < 50) {
        VectorCopy(ang, viewangles);
        *viewheight = vh;
        *lean       = ln;
        return qtrue;
    }

    // pulled in toward the top of their head: the eye height from their box, the lean as it was,
    // and the pitch less the trace's share
    IntegerToBoundingBox(pl->solid, mins, maxs);
    vh = maxs[2] < (CROUCH_MAXS_Z + MAXS_Z) / 2 ? CROUCH_VIEWHEIGHT : DEFAULT_VIEWHEIGHT;
    ln = fe.lean;

    VectorCopy(pl->origin, start);
    start[2] += maxs[2] - 2;
    VectorCopy(pl->origin, ideal);
    ideal[2] += vh;
    VectorMA(ideal, FE_CAM_FORWARD, fwd, ideal);
    VectorMA(ideal, FE_CAM_UP, up, ideal);
    VectorMA(ideal, FE_CAM_LEAN * ln, right, ideal);

    VectorSubtract(ideal, start, d);
    len = VectorLength(d);
    VectorSubtract(ps->camera_origin, start, d);
    if (len < 1 || VectorLength(d) > len + 16) {
        // not a camera behind them
        return qfalse;
    }

    frac = VectorLength(d) / len;
    if (frac > 1) {
        frac = 1;
    }

    VectorCopy(ps->camera_angles, viewangles);
    viewangles[PITCH] -= FE_CAM_PITCH * frac;
    viewangles[ROLL] = 0;
    *viewheight      = vh;
    *lean            = ln;
    return qtrue;
}

/*
===============
FE_Salt

When the entity drawn as the first-person model changes, the old one goes back to its world model and
the new one takes the first-person model, each keeping the other model's animations; a new usageIndex
makes the cgame start both afresh (sv_snapshot.c does the same for g_spectatefollow_firstperson)
===============
*/
static void FE_Salt(snapshot_t *snap, int viewEnt)
{
    int i;

    if (viewEnt != fe.viewEnt) {
        if (fe.viewEnt >= 0 && fe.viewEnt < MAX_CLIENTS) {
            fe.salt[fe.viewEnt] = fe.salt[fe.viewEnt] % 65534 + 1;
        }
        if (viewEnt >= 0 && viewEnt < MAX_CLIENTS) {
            fe.salt[viewEnt] = fe.salt[viewEnt] % 65534 + 1;
        }
        fe.viewEnt = viewEnt;
    }

    for (i = 0; i < snap->numEntities; i++) {
        entityState_t *es = &snap->entities[i];

        if (es->number < MAX_CLIENTS && fe.salt[es->number] && es->usageIndex > 0) {
            es->usageIndex = (es->usageIndex - 1 + fe.salt[es->number]) % 65535 + 1;
        }
    }
}

/*
===============
CG_FollowEyes_Snapshot

Called for each snapshot read from the client, before the cgame uses it
===============
*/
void CG_FollowEyes_Snapshot(snapshot_t *snap)
{
    playerState_t       *ps = &snap->ps;
    const entityState_t *pl = NULL, *weapon;
    dtiki_t             *tiki;
    vec3_t               viewangles;
    float                viewheight, lean, dt;
    int                  follow, i, item, vmAnim;

    // A server of this code shows a follower the player's own view itself (g_spectatefollow_firstperson 1:
    // the snapshot under their number), with their ammo: one seen doing it is asked for the view set here
    // (its spectatemode), and does it from then on
    if (!cg.demoPlayback && (ps->pm_flags & PMF_SPECTATING)) {
        const int want = cg_spectatefirstperson->integer ? 1 : 0;

        if (ps->clientNum != cg.clientNum) {
            fe.serverMode = qtrue;
        }
        if (fe.serverMode && want != fe.modeSent) {
            cgi.SendClientCommand(va("spectatemode %i", want));
            fe.modeSent = want;
        }
    }

    follow = -1;
    if (cg_spectatefirstperson->integer && !fe.serverMode && (ps->pm_flags & PMF_SPECTATING)
        && (ps->pm_flags & PMF_CAMERA_VIEW) && !(ps->pm_flags & PMF_INTERMISSION) && ps->clientNum == cg.clientNum) {
        follow = ps->stats[STAT_INFOCLIENT];
    }

    if (follow >= 0 && follow < MAX_CLIENTS && follow != cg.clientNum) {
        for (i = 0; i < snap->numEntities; i++) {
            if (snap->entities[i].number == follow && snap->entities[i].eType == ET_PLAYER) {
                pl = &snap->entities[i];
                break;
            }
        }
    }

    if (!pl || !FE_Camera(ps, pl, viewangles, &viewheight, &lean)) {
        fe.follow = -1;
        fe.lean   = 0;
        FE_Salt(snap, -1);
        return;
    }

    if (follow != fe.follow) {
        // someone new: no speed from the last one, and their animations as they are now
        fe.serverTime       = 0;
        fe.weaponEnt        = -1;
        fe.torsoAnim.index  = -1;
        fe.weaponAnim.index = -1;
        fe.vmAnim           = VM_ANIM_IDLE;
    }

    //
    // where they are and look
    //
    ps->clientNum = follow;
    ps->pm_flags &= ~(PMF_CAMERA_VIEW | PMF_DUCKED);
    ps->pm_flags |= PMF_NO_PREDICTION;
    if (viewheight <= CROUCH_VIEWHEIGHT) {
        ps->pm_flags |= PMF_DUCKED;
    }

    VectorCopy(pl->origin, ps->origin);
    VectorCopy(viewangles, ps->viewangles);
    ps->viewheight      = (int)(viewheight + 0.5f);
    ps->fLeanAngle      = lean;
    ps->groundEntityNum = pl->groundEntityNum;
    ps->walking         = pl->groundEntityNum != ENTITYNUM_NONE;
    VectorClear(ps->damage_angles);

    // their speed: the server's when it sends it (g_smoothClients), else from their last origin
    dt = (snap->serverTime - fe.serverTime) / 1000.0f;
    if (pl->pos.trDelta[0] || pl->pos.trDelta[1] || pl->pos.trDelta[2]) {
        VectorCopy(pl->pos.trDelta, ps->velocity);
    } else if (fe.serverTime && dt > 0 && dt < 0.25f) {
        VectorSubtract(pl->origin, fe.origin, ps->velocity);
        VectorScale(ps->velocity, 1.0f / dt, ps->velocity);
    } else {
        VectorClear(ps->velocity);
    }

    fe.follow     = follow;
    fe.serverTime = snap->serverTime;
    fe.lean       = lean;
    VectorCopy(pl->origin, fe.origin);

    //
    // what they hold
    //
    ps->activeItems[1]                = -1;
    ps->stats[STAT_EQUIPPED_WEAPON]   = 0;
    ps->stats[STAT_CROSSHAIR]         = 0;
    ps->stats[STAT_AMMO]              = 0;
    ps->stats[STAT_CLIPAMMO]          = 0;
    ps->stats[STAT_HEALTH]            = ps->stats[STAT_INFOCLIENT_HEALTH] > 0 ? ps->stats[STAT_INFOCLIENT_HEALTH] : 1;
    ps->stats[STAT_MAXHEALTH]         = 100;
    ps->stats[STAT_INZOOM]            = ps->fov > 0 && ps->fov < 60 ? (int)ps->fov : 0;

    tiki   = NULL;
    weapon = FE_FindWeapon(snap, follow, &tiki);
    vmAnim = -1;
    if (weapon) {
        const frameInfo_t *fi = NULL;

        item = FE_ItemIndex(FE_ServerInitArg(tiki, "name"));
        if (item > 0) {
            ps->activeItems[1] = item;
        }
        ps->stats[STAT_EQUIPPED_WEAPON] = FE_WeaponClass(FE_ServerInitArg(tiki, "weapontype"));
        ps->stats[STAT_CROSSHAIR]       = FE_Crosshair(tiki, ps);

        // the gun's own animation, its heaviest
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if (weapon->frameInfo[i].weight > 0 && (!fi || weapon->frameInfo[i].weight > fi->weight)) {
                fi = &weapon->frameInfo[i];
            }
        }

        if (weapon->number != fe.weaponEnt) {
            fe.weaponEnt        = weapon->number;
            fe.weaponAnim.index = -1;
        }

        if (fi && FE_AnimChanged(&fe.weaponAnim, fi->index, fi->time)) {
            vmAnim = FE_WeaponVMAnim(cgi.Anim_NameForNum(tiki, fi->index));
        }
    } else {
        fe.weaponEnt = -1;
    }

    // the upper body's, the newest of its two slots
    {
        dtiki_t           *ptiki = cgi.R_Model_GetHandle(cgs.model_draw[pl->modelindex]);
        const frameInfo_t *fi    = NULL;

        for (i = FE_TORSO_SLOT; i < FE_TORSO_SLOT + 2; i++) {
            if (pl->frameInfo[i].weight > 0 && (!fi || pl->frameInfo[i].time < fi->time)) {
                fi = &pl->frameInfo[i];
            }
        }

        if (ptiki && fi && FE_AnimChanged(&fe.torsoAnim, fi->index, fi->time)) {
            int torso = FE_TorsoVMAnim(cgi.Anim_NameForNum(ptiki, fi->index));

            if (torso != -1) {
                vmAnim = torso;
            }
        }
    }

    if (vmAnim != -1) {
        fe.vmAnim        = vmAnim;
        fe.vmAnimChanged = (fe.vmAnimChanged + 1) & 3;
    }
    ps->iViewModelAnim        = fe.vmAnim;
    ps->iViewModelAnimChanged = fe.vmAnimChanged;

    FE_Salt(snap, follow);
}
