/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA Metal port contributors

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

// tr_temporal.c -- the temporal path (r_temporal with r_taa, r_ao, r_motionBlur or r_dof): what the 3D view's effects need
// from the back end's view loop: the cameras and the entities of the last frame, the projection's jitter, the time
// between the frames, where the ambient occlusion goes, and the depth of field's lens. The ideas are JayRewd's (the Vulkan renderer's temporal path,
// renderervk/tr_temporal.c and vk_temporal.c), written again for the Metal backend (mbTemporal* in tr_mtl.h; tr_mtl.m,
// tr_mtl_temporal.metal, tr_mtl_ao.metal).
//
// Off (the default: r_temporal 0, or no effect on) the views test one flag (rt.on), the back end's surface list one
// (backEnd.temporalAo) a shader, and the frame's end reads one cvar (r_temporal 0) or five (RB_TemporalFrame): nothing
// else runs and nothing is kept; the back end has no target or pass of it.
//
// With the anti-aliasing (r_taa) or the motion blur (r_motionBlur: MB_TEMPORAL_VECTORS, the scene drawn with the motion
// vectors) the frame's first view of the world begins the scene (mbTemporalBeginView), and the views after it (the main
// view after the sky portal's) continue it; only the anti-aliasing jitters the projection. The main view (not a portal,
// not the sky portal) keeps its camera for the next frame, which has a last frame when the camera didn't jump
// (r_temporalCutDist), the field of view is the same and the game's time didn't go back; the sky portal's camera is kept
// the same way, apart. Other portal views have no motion. Entities: the main view keeps each one's model matrix, known
// by its game entity number, its model and how many entities of the scene had both before it (attached models share
// the number); the next frame finds it there and gives the back end its motion (mbTemporalObject). Entities without a
// game entity number (temporary models, effects) and those that jumped move with the camera only, as what doesn't move
// (the world, static models, sprites); the view weapon without a last frame moves with the view. The main view's game
// time between its frames, smoothed, sets the motion blur's exposure (a fixed shutter in time: the same blur at any
// frame rate).
//
// With the ambient occlusion (r_ao) the main view's camera is kept the same way (its motion moves the occlusion's
// history), and the back end's surface list calls RB_TemporalAo once, before the main view's first blended surface
// (or after its last surface): the occlusion of what is drawn so far (mbTemporalAo). Alone it needs no motion vectors,
// no jitter and no entities: the scene is drawn as without the path.
//
// With the depth of field (r_dof) the main view's camera is kept too (its time between frames eases the focus), and the
// frame's views of the world are a scene that ends before the 2D (RB_TemporalEndScene) with or without the motion
// vectors: the back end blurs it there by the depth (after the anti-aliasing, before the motion blur). Alone it needs
// no motion vectors, no jitter and no entities either.

#include "tr_local.h"

#define RT_TABLE			8192		// power of two, twice MAX_REFENTITIES

typedef struct {
	int			entityNumber;			// the key, with hModel and ordinal
	qhandle_t	hModel;
	int			ordinal;
	unsigned	frame;					// the frame that wrote it (rt.frame), 0: free
	float		model[16];				// object to world
} rtEntity_t;

typedef struct {
	rtEntity_t	tables[2][RT_TABLE];	// this frame's and the last one's
	unsigned	tableFrame[2];
	int			current;
	unsigned	frame;					// the frame whose entities were kept
	rtEntity_t	*prev[MAX_REFENTITIES];	// per entity of the main view: its last frame's, or NULL
} rtEntities_t;

typedef struct {
	unsigned	frame;					// the frame it was kept in, 0: none
	float		view[16];				// world to eye (viewParms.world.modelMatrix)
	float		projection[16];
	float		vmProjection[16];		// the view weapon's (the view's when it has none)
	vec3_t		origin;
	float		fovX, fovY;
	int			time;					// refdef.time
	float		zFar;
	// this frame's: whether it has a last frame, the last frame's projections and its view times this one's inverse
	qboolean	history;
	float		prevView[16], prevProjection[16], prevVmProjection[16], delta[16];
	float		frameMsec;				// the game time between its frames, smoothed (0: none yet)
} rtCamera_t;

static struct {
	int				on;					// the effects whose back end targets exist (MB_TEMPORAL_*, mbTemporalEnable)
	int				failed;				// those that couldn't be made: not again until they are turned off
	unsigned		frame;				// frames since R_TemporalInit, from 1
	unsigned		aoFrame;			// the frame the occlusion last ran in
	rtCamera_t		camera[2];			// the main view's, the sky portal's
	rtEntities_t	*entities;			// while on
	float			jitter[2];			// this frame's, in pixels
	unsigned		jitterFrame;
	qboolean		dofScene;			// the depth of field's scene began (a view of the world), not ended yet
} rt;

static cvar_t	*r_temporal;
static cvar_t	*r_taa;
static cvar_t	*r_taaFeedback;
static cvar_t	*r_taaSharpen;
static cvar_t	*r_temporalJitterSamples;
static cvar_t	*r_temporalCutDist;
static cvar_t	*r_temporalShow;
static cvar_t	*r_temporalShowScale;
static cvar_t	*r_ao;
static cvar_t	*r_aoQuality;
static cvar_t	*r_aoRadius;
static cvar_t	*r_aoStrength;
static cvar_t	*r_aoFalloff;
static cvar_t	*r_aoThickness;
static cvar_t	*r_aoTemporal;
static cvar_t	*r_aoMultiBounce;
static cvar_t	*r_motionBlur;
static cvar_t	*r_motionBlurQuality;
static cvar_t	*r_motionBlurShutter;
static cvar_t	*r_motionBlurFps;
static cvar_t	*r_motionBlurCamera;
static cvar_t	*r_dof;
static cvar_t	*r_dofAperture;
static cvar_t	*r_dofFocus;
static cvar_t	*r_dofFocusSpeed;
static cvar_t	*r_dofQuality;

// the motion blur's pairs of samples a pixel at r_motionBlurQuality 0, 1, 2 (the Vulkan renderer's)
static const int rt_blurPairs[3] = { 4, 6, 8 };
// the depth of field's samples of each gather at r_dofQuality 0, 1, 2 (the Vulkan renderer's)
static const int rt_dofSamples[3] = { 16, 32, 48 };
// its lens (the Vulkan renderer's): r_dofAperture is the far distance's blur when the focus is this far (128 units, about
// 3 m), at the projection's [5] of the game's default field of view (fov 80 at 4:3; a narrower field, a scope's zoom,
// blurs more, by its zoom)
#define RT_DOF_REFERENCE_FOCUS		128.0f
#define RT_DOF_PROJ_FOV80			1.5890f

static const float rt_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

void R_TemporalInit( void ) {
	r_temporal = ri.Cvar_Get( "r_temporal", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_temporal, "1: the 3D view's effects (r_taa, r_ao, r_motionBlur, r_dof): Enhanced Rendering in Options. Off, or with none of them on, nothing of it exists or runs" );
	r_taa = ri.Cvar_Get( "r_taa", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taa, "with r_temporal 1: temporal anti-aliasing (the projection is jittered with it)" );
	r_taaFeedback = ri.Cvar_Get( "r_taaFeedback", "0.75", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaFeedback, "with r_taa 1: the weight of the last frames in the result; more is smoother and blurs more in motion" );
	ri.Cvar_CheckRange( r_taaFeedback, 0.5f, 0.98f, qfalse );
	r_taaSharpen = ri.Cvar_Get( "r_taaSharpen", "0.8", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaSharpen, "with r_taa 1: sharpens the anti-aliased image, 0 off to 1" );
	ri.Cvar_CheckRange( r_taaSharpen, 0.0f, 1.0f, qfalse );
	r_temporalJitterSamples = ri.Cvar_Get( "r_temporalJitterSamples", "8", 0 );
	ri.Cvar_SetDescription( r_temporalJitterSamples, "with r_taa 1: the jitter's sub-pixel offsets before they repeat (Halton 2, 3)" );
	ri.Cvar_CheckRange( r_temporalJitterSamples, 1, 64, qtrue );
	r_temporalCutDist = ri.Cvar_Get( "r_temporalCutDist", "256", 0 );
	ri.Cvar_SetDescription( r_temporalCutDist, "with r_temporal 1: a camera or an entity that moves farther than this in a frame has no last frame (a cut, a teleport)" );
	r_temporalShow = ri.Cvar_Get( "r_temporalShow", "0", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_temporalShow, "instead of the 3D view: with r_taa or r_motionBlur 1, 1 the motion vectors (right red, down green), 2 the scene without the anti-aliasing, the depth of field and the motion blur; with r_ao 1, 3 the occlusion alone (white none); with r_motionBlur 1, 4 each pixel's blur, 5 the longest blur around each tile (a tile's side the brightest); with r_dof 1, 6 each pixel's blur, in front of the focus red, behind it blue (8 pixels the brightest), over the scene in grey, the focus's square green" );
	ri.Cvar_CheckRange( r_temporalShow, 0, 6, qtrue );
	r_temporalShowScale = ri.Cvar_Get( "r_temporalShowScale", "1", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_temporalShowScale, "with r_temporalShow 1: the motion vectors' scale; at 1 a move of 32 pixels a frame is the brightest" );
	// the Vulkan renderer's cvars and defaults (vk_temporal.c; not its r_aoNormals: no normals are drawn here)
	r_ao = ri.Cvar_Get( "r_ao", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_ao, "with r_temporal 1: ambient occlusion (ground truth ambient occlusion, GTAO), the contact shadows of corners, creases and what stands on a floor" );
	r_aoQuality = ri.Cvar_Get( "r_aoQuality", "2", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoQuality, "with r_ao 1: 0 low, 1 medium, 2 high, 3 ultra (directions and samples per pixel)" );
	ri.Cvar_CheckRange( r_aoQuality, 0, 3, qtrue );
	r_aoRadius = ri.Cvar_Get( "r_aoRadius", "48", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoRadius, "with r_ao 1: how far occluders reach, in world units" );
	ri.Cvar_CheckRange( r_aoRadius, 4.0f, 256.0f, qfalse );
	r_aoStrength = ri.Cvar_Get( "r_aoStrength", "1.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoStrength, "with r_ao 1: the occlusion's strength (a power of the visibility), 1 as computed" );
	ri.Cvar_CheckRange( r_aoStrength, 0.0f, 4.0f, qfalse );
	r_aoFalloff = ri.Cvar_Get( "r_aoFalloff", "0.6", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoFalloff, "with r_ao 1: the share of the radius over which occluders fade out" );
	ri.Cvar_CheckRange( r_aoFalloff, 0.05f, 1.0f, qfalse );
	r_aoThickness = ri.Cvar_Get( "r_aoThickness", "0.25", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoThickness, "with r_ao 1: the thin occluder compensation: occluders in front along the view fade sooner (less dark halos behind thin objects)" );
	ri.Cvar_CheckRange( r_aoThickness, 0.0f, 4.0f, qfalse );
	r_aoTemporal = ri.Cvar_Get( "r_aoTemporal", "16", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoTemporal, "with r_ao 1: the most frames blended (less noise, slower to react); 1: none" );
	ri.Cvar_CheckRange( r_aoTemporal, 1, 64, qtrue );
	r_aoMultiBounce = ri.Cvar_Get( "r_aoMultiBounce", "0.4", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoMultiBounce, "with r_ao 1: the albedo of the light bounced back by occluders (lightens the occlusion), 0 none" );
	ri.Cvar_CheckRange( r_aoMultiBounce, 0.0f, 0.9f, qfalse );
	// the Vulkan renderer's cvars and defaults (vk_temporal.c)
	r_motionBlur = ri.Cvar_Get( "r_motionBlur", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlur, "with r_temporal 1: motion blur of the camera's motion and of what moves, from the motion vectors and the depth" );
	r_motionBlurQuality = ri.Cvar_Get( "r_motionBlurQuality", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurQuality, "with r_motionBlur 1: 0 low, 1 medium, 2 high (8, 12, 16 samples per pixel)" );
	ri.Cvar_CheckRange( r_motionBlurQuality, 0, 2, qtrue );
	r_motionBlurShutter = ri.Cvar_Get( "r_motionBlurShutter", "0.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurShutter, "with r_motionBlur 1: the exposure, in frames at r_motionBlurFps (0.5: half a frame, 1/120 s at 60)" );
	ri.Cvar_CheckRange( r_motionBlurShutter, 0.0f, 2.0f, qfalse );
	r_motionBlurFps = ri.Cvar_Get( "r_motionBlurFps", "60", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurFps, "with r_motionBlur 1: the frame rate whose frames r_motionBlurShutter counts, so the blur is the same at any frame rate; 0: the frames as they come (shorter blur the higher the frame rate)" );
	ri.Cvar_CheckRange( r_motionBlurFps, 0.0f, 1000.0f, qfalse );
	r_motionBlurCamera = ri.Cvar_Get( "r_motionBlurCamera", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurCamera, "with r_motionBlur 1: the share of the camera's motion (turning, moving) in the blur; 0: only what moves" );
	ri.Cvar_CheckRange( r_motionBlurCamera, 0.0f, 1.0f, qfalse );
	// the Vulkan renderer's cvars and defaults (vk_temporal.c)
	r_dof = ri.Cvar_Get( "r_dof", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dof, "with r_temporal 1: depth of field, focused on what is under the crosshair (or at r_dofFocus); the view weapon stays sharp" );
	r_dofAperture = ri.Cvar_Get( "r_dofAperture", "0.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofAperture, "with r_dof 1: the lens' aperture: the blur of the far distance when focused 128 units (3 m) away, in percent of the view's height; a scope's zoom blurs more" );
	ri.Cvar_CheckRange( r_dofAperture, 0.0f, 5.0f, qfalse );
	r_dofFocus = ri.Cvar_Get( "r_dofFocus", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofFocus, "with r_dof 1: 0 focus on what is under the crosshair; more: a fixed focus distance in units (screenshots)" );
	ri.Cvar_CheckRange( r_dofFocus, 0.0f, 65536.0f, qfalse );
	r_dofFocusSpeed = ri.Cvar_Get( "r_dofFocusSpeed", "0.25", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofFocusSpeed, "with r_dof 1: how long the focus takes to follow, in seconds (the time constant of its easing); 0 at once" );
	ri.Cvar_CheckRange( r_dofFocusSpeed, 0.0f, 10.0f, qfalse );
	r_dofQuality = ri.Cvar_Get( "r_dofQuality", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofQuality, "with r_dof 1: 0 low, 1 medium, 2 high (16, 32, 48 samples per gather where something is blurred)" );
	ri.Cvar_CheckRange( r_dofQuality, 0, 2, qtrue );

	// the back end starts without the path (mbInit); the frames are counted again
	if ( rt.entities ) {
		ri.Free( rt.entities );
	}
	Com_Memset( &rt, 0, sizeof( rt ) );
}

// a * b, column-major
static void R_TemporalMultiply( const float *a, const float *b, float *out ) {
	int c, r;

	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
				+ a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
}

// the inverse of an affine matrix (column-major, the last row 0 0 0 1); qfalse if it has none
static qboolean R_TemporalInvertAffine( const float *m, float *out ) {
	float a00 = m[0], a10 = m[1], a20 = m[2];
	float a01 = m[4], a11 = m[5], a21 = m[6];
	float a02 = m[8], a12 = m[9], a22 = m[10];
	float c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
	float det = a00 * c00 + a10 * c01 + a20 * c02;
	float inv;
	int r;

	if ( fabsf( det ) < 1e-12f ) {
		return qfalse;
	}
	inv = 1.0f / det;
	out[0] = c00 * inv;
	out[4] = c01 * inv;
	out[8] = c02 * inv;
	out[1] = ( a12 * a20 - a10 * a22 ) * inv;
	out[5] = ( a00 * a22 - a02 * a20 ) * inv;
	out[9] = ( a02 * a10 - a00 * a12 ) * inv;
	out[2] = ( a10 * a21 - a11 * a20 ) * inv;
	out[6] = ( a01 * a20 - a00 * a21 ) * inv;
	out[10] = ( a00 * a11 - a01 * a10 ) * inv;
	out[3] = out[7] = out[11] = 0.0f;
	for ( r = 0; r < 3; r++ ) {
		out[12 + r] = -( out[0 * 4 + r] * m[12] + out[1 * 4 + r] * m[13] + out[2 * 4 + r] * m[14] );
	}
	out[15] = 1.0f;
	return qtrue;
}

// an entity's object to world matrix (R_RotateForEntity without the view)
static void R_TemporalModelMatrix( const refEntity_t *e, float *m ) {
	int i;

	for ( i = 0; i < 3; i++ ) {
		m[i * 4 + 0] = e->axis[i][0];
		m[i * 4 + 1] = e->axis[i][1];
		m[i * 4 + 2] = e->axis[i][2];
		m[i * 4 + 3] = 0.0f;
	}
	m[12] = e->origin[0];
	m[13] = e->origin[1];
	m[14] = e->origin[2];
	m[15] = 1.0f;
}

static unsigned R_TemporalHash( int entityNumber, qhandle_t hModel, int ordinal ) {
	unsigned h = (unsigned)entityNumber * 2654435761u ^ (unsigned)hModel * 40503u ^ (unsigned)ordinal * 2246822519u;

	h ^= h >> 15;
	return h & ( RT_TABLE - 1 );
}

// the key's slot in a table of that frame; insert: a free one for it (NULL when the table is full) when it isn't there
static rtEntity_t *R_TemporalSlot( rtEntity_t *table, unsigned frame, int entityNumber, qhandle_t hModel, int ordinal, qboolean insert ) {
	unsigned i = R_TemporalHash( entityNumber, hModel, ordinal ), n;

	for ( n = 0; n < RT_TABLE; n++, i = ( i + 1 ) & ( RT_TABLE - 1 ) ) {
		rtEntity_t *t = &table[i];

		if ( t->frame != frame ) {
			if ( !insert ) {
				return NULL;
			}
			t->frame = frame;
			t->entityNumber = entityNumber;
			t->hModel = hModel;
			t->ordinal = ordinal;
			return t;
		}
		if ( t->entityNumber == entityNumber && t->hModel == hModel && t->ordinal == ordinal ) {
			return insert ? NULL : t;
		}
	}
	return NULL;
}

/*
===============
R_TemporalCamera

A camera's last frame, once a frame (slot 0 the main view's, 1 the sky portal's); this frame's is kept
===============
*/
static void R_TemporalCamera( rtCamera_t *c ) {
	const float *view = backEnd.viewParms.world.modelMatrix;
	float inv[16];
	vec3_t delta;

	VectorSubtract( backEnd.viewParms.ori.origin, c->origin, delta );
	c->history = c->frame && c->frame + 1 == rt.frame && VectorLength( delta ) <= r_temporalCutDist->value
		&& fabsf( backEnd.refdef.fov_x - c->fovX ) < 0.01f && fabsf( backEnd.refdef.fov_y - c->fovY ) < 0.01f
		&& backEnd.refdef.time >= c->time && R_TemporalInvertAffine( view, inv );
	if ( c->history ) {
		float dt = (float)( backEnd.refdef.time - c->time );

		Com_Memcpy( c->prevView, c->view, sizeof( c->prevView ) );
		Com_Memcpy( c->prevProjection, c->projection, sizeof( c->prevProjection ) );
		Com_Memcpy( c->prevVmProjection, c->vmProjection, sizeof( c->prevVmProjection ) );
		R_TemporalMultiply( c->view, inv, c->delta );
		// the motion covers dt; a paused game (dt 0) keeps the last value, its motion is none anyway
		if ( dt > 0.0f ) {
			dt = Com_Clamp( 1.0f, 100.0f, dt );
			c->frameMsec = c->frameMsec > 0.0f ? c->frameMsec + ( dt - c->frameMsec ) * 0.125f : dt;
		}
	} else {
		c->frameMsec = 0.0f;
	}
	Com_Memcpy( c->view, view, sizeof( c->view ) );
	Com_Memcpy( c->projection, backEnd.viewParms.projectionMatrix, sizeof( c->projection ) );
	Com_Memcpy( c->vmProjection, backEnd.viewParms.vmProjection ? backEnd.viewParms.vmProjectionMatrix
		: backEnd.viewParms.projectionMatrix, sizeof( c->vmProjection ) );
	VectorCopy( backEnd.viewParms.ori.origin, c->origin );
	c->fovX = backEnd.refdef.fov_x;
	c->fovY = backEnd.refdef.fov_y;
	c->time = backEnd.refdef.time;
	c->zFar = backEnd.viewParms.zFar;
	c->frame = rt.frame;
}

/*
===============
R_TemporalEntities

The main view's entities, once a frame: each one's last frame, and this frame's kept
===============
*/
static void R_TemporalEntities( void ) {
	rtEntities_t *ents = rt.entities;
	rtEntity_t *table, *last;
	int i, ordinal, prevTable;
	float cut = r_temporalCutDist->value;

	prevTable = ents->current;
	ents->current ^= 1;
	ents->tableFrame[ents->current] = rt.frame;
	table = ents->tables[ents->current];
	last = rt.camera[0].history && ents->tableFrame[prevTable] + 1 == rt.frame ? ents->tables[prevTable] : NULL;
	ents->frame = rt.frame;

	for ( i = 0; i < backEnd.refdef.num_entities && i < MAX_REFENTITIES; i++ ) {
		const refEntity_t *e = &backEnd.refdef.entities[i].e;
		rtEntity_t *t, *p;
		vec3_t delta;

		ents->prev[i] = NULL;
		if ( e->reType != RT_MODEL || e->entityNumber == ENTITYNUM_NONE ) {
			continue;
		}
		for ( ordinal = 0; R_TemporalSlot( table, rt.frame, e->entityNumber, e->hModel, ordinal, qfalse ); ordinal++ ) {
		}
		t = R_TemporalSlot( table, rt.frame, e->entityNumber, e->hModel, ordinal, qtrue );
		if ( !t ) {
			continue;
		}
		R_TemporalModelMatrix( e, t->model );

		p = last ? R_TemporalSlot( last, rt.frame - 1, e->entityNumber, e->hModel, ordinal, qfalse ) : NULL;
		if ( p ) {
			delta[0] = t->model[12] - p->model[12];
			delta[1] = t->model[13] - p->model[13];
			delta[2] = t->model[14] - p->model[14];
			if ( VectorLength( delta ) <= cut ) {
				ents->prev[i] = p;
			}
		}
	}
}

// this frame's jitter in pixels (x right, y down): the Halton sequence of bases 2 and 3, centered
static float R_TemporalHalton( int index, int base ) {
	float f = 1.0f, r = 0.0f;

	while ( index > 0 ) {
		f /= base;
		r += f * ( index % base );
		index /= base;
	}
	return r;
}

static void R_TemporalJitter( void ) {
	int i;

	if ( rt.jitterFrame == rt.frame ) {
		return;
	}
	i = (int)( rt.frame % (unsigned)r_temporalJitterSamples->integer ) + 1;
	rt.jitter[0] = R_TemporalHalton( i, 2 ) - 0.5f;
	rt.jitter[1] = R_TemporalHalton( i, 3 ) - 0.5f;
	rt.jitterFrame = rt.frame;
}

/*
===============
RB_TemporalFrame

At the end of every frame (RB_SwapBuffers, after the back end's): the path on or off as its cvars say
===============
*/
void RB_TemporalFrame( void ) {
	int want = r_temporal->integer ? ( r_taa->integer ? MB_TEMPORAL_TAA : 0 ) | ( r_ao->integer ? MB_TEMPORAL_AO : 0 )
		| ( r_motionBlur->integer ? MB_TEMPORAL_MB : 0 ) | ( r_dof->integer ? MB_TEMPORAL_DOF : 0 ) : 0;

	rt.frame++;
	backEnd.temporalAo = qfalse;
	rt.failed &= want;
	want &= ~rt.failed;
	if ( want == rt.on ) {
		return;
	}
	rt.on = mbTemporalEnable( want );
	rt.failed |= want & ~rt.on;
	// the entities' last frames: for the motion vectors only
	if ( ( rt.on & MB_TEMPORAL_VECTORS ) && !rt.entities ) {
		rt.entities = ri.Malloc( sizeof( *rt.entities ) );
		Com_Memset( rt.entities, 0, sizeof( *rt.entities ) );
	} else if ( !( rt.on & MB_TEMPORAL_VECTORS ) && rt.entities ) {
		ri.Free( rt.entities );
		rt.entities = NULL;
	}
	// a new start: no last frame
	rt.camera[0].frame = rt.camera[1].frame = 0;
	rt.dofScene = qfalse;
}

/*
===============
RB_TemporalBeginView

The back end starts drawing a view (RB_DrawSurfs, RB_SpriteSurfs): a view of the world begins or continues the
frame's scene, another one ends it
===============
*/
void RB_TemporalBeginView( void ) {
	mbTemporalView_t view;
	rtCamera_t *camera = NULL;
	qboolean primary;

	if ( !rt.on ) {
		return;
	}
	backEnd.temporalAo = qfalse;
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_HYPERSPACE | RDF_HUD ) ) {
		RB_TemporalEndScene();
		return;
	}

	primary = !backEnd.viewParms.isPortal && !backEnd.viewParms.isPortalSky;
	if ( primary ) {
		camera = &rt.camera[0];
	} else if ( backEnd.viewParms.isPortalSky ) {
		camera = &rt.camera[1];
	}
	if ( camera && camera->frame != rt.frame ) {
		R_TemporalCamera( camera );
	}
	// the occlusion, once a frame in the main view (RB_TemporalAo)
	backEnd.temporalAo = primary && ( rt.on & MB_TEMPORAL_AO ) && rt.aoFrame != rt.frame;

	if ( rt.on & MB_TEMPORAL_VECTORS ) {
		if ( primary && rt.entities->frame != rt.frame ) {
			R_TemporalEntities();
		}
		if ( rt.on & MB_TEMPORAL_TAA ) {
			R_TemporalJitter();
		}
	}

	Com_Memcpy( view.cameraDelta, camera && camera->history ? camera->delta : rt_identity, sizeof( view.cameraDelta ) );
	view.jitter[0] = ( rt.on & MB_TEMPORAL_TAA ) ? rt.jitter[0] : 0.0f;
	view.jitter[1] = ( rt.on & MB_TEMPORAL_TAA ) ? rt.jitter[1] : 0.0f;
	view.viewport[0] = backEnd.viewParms.viewportX;
	view.viewport[1] = backEnd.viewParms.viewportY;
	view.viewport[2] = backEnd.viewParms.viewportWidth;
	view.viewport[3] = backEnd.viewParms.viewportHeight;
	view.primary = primary;
	view.history = rt.camera[0].history;
	mbTemporalBeginView( &view );

	// the scene with the motion vectors: the draws' motion
	if ( rt.on & MB_TEMPORAL_VECTORS ) {
		backEnd.temporalScene = qtrue;
		backEnd.temporalCamera = camera ? camera - rt.camera : -1;
	}
	// the depth of field's scene, which ends with it (with or without the motion vectors)
	if ( rt.on & MB_TEMPORAL_DOF ) {
		rt.dofScene = qtrue;
	}
}

/*
===============
RB_TemporalProjection

After the back end loads a view's projection (the view's, or the view weapon's own: depthHack): the last frame's
===============
*/
void RB_TemporalProjection( qboolean depthHack ) {
	const rtCamera_t *c = backEnd.temporalCamera >= 0 ? &rt.camera[backEnd.temporalCamera] : NULL;

	if ( c && c->history ) {
		mbTemporalPrevProjection( depthHack ? c->prevVmProjection : c->prevProjection );
	} else {
		// no motion: this frame's
		mbTemporalPrevProjection( depthHack && backEnd.viewParms.vmProjection ? backEnd.viewParms.vmProjectionMatrix
			: backEnd.viewParms.projectionMatrix );
	}
}

/*
===============
RB_TemporalModelview

After the back end loads an entity's, a static model's or the world's modelview (RB_RenderDrawSurfList): what moves it
===============
*/
void RB_TemporalModelview( void ) {
	const trRefEntity_t *ent = backEnd.currentEntity;
	const rtEntity_t *p;
	float prevModelview[16], inv[16], delta[16];
	int i;

	if ( backEnd.currentStaticModel || !ent || ent == &tr.worldEntity || backEnd.temporalCamera != 0 ) {
		mbTemporalObject( NULL );
		return;
	}
	i = ent - backEnd.refdef.entities;
	p = i >= 0 && i < backEnd.refdef.num_entities && i < MAX_REFENTITIES ? rt.entities->prev[i] : NULL;
	if ( p && rt.camera[0].history && R_TemporalInvertAffine( backEnd.ori.modelMatrix, inv ) ) {
		// its last frame's modelview times this one's inverse
		R_TemporalMultiply( rt.camera[0].prevView, p->model, prevModelview );
		R_TemporalMultiply( prevModelview, inv, delta );
		mbTemporalObject( delta );
	} else if ( ent->e.renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK ) ) {
		// the view weapon without a last frame: with the view
		mbTemporalObject( rt_identity );
	} else {
		mbTemporalObject( NULL );
	}
}

/*
===============
RB_TemporalAo

The back end's surface list in the main view (backEnd.temporalAo), before its first surface sorted at SS_UNDERWATER or
later (what is blended over the opaque surfaces, decals, see-through ones and banners: the effects in front of a
corner aren't darkened), or after its last surface: the ambient occlusion of what is drawn so far, under the view's
fog (RB_SetupFog), once a frame
===============
*/
void RB_TemporalAo( void ) {
	const rtCamera_t *c = &rt.camera[0];
	mbTemporalAo_t ao;

	backEnd.temporalAo = qfalse;
	rt.aoFrame = rt.frame;
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	Com_Memcpy( ao.projection, backEnd.viewParms.projectionMatrix, sizeof( ao.projection ) );
	ao.history = c->history && c->frame == rt.frame;
	if ( ao.history ) {
		R_TemporalMultiply( c->prevProjection, c->delta, ao.reproject );
	} else {
		Com_Memcpy( ao.reproject, rt_identity, sizeof( ao.reproject ) );
	}
	ao.zFar = backEnd.viewParms.zFar;
	ao.fog = ( glState.externalSetState & GLS_FOG ) != 0;
	ao.fogStart = backEnd.viewParms.farplane_bias;
	ao.fogEnd = backEnd.viewParms.farplane_distance;
	ao.fogColor[0] = backEnd.viewParms.farplane_color[0] * tr.identityLight;
	ao.fogColor[1] = backEnd.viewParms.farplane_color[1] * tr.identityLight;
	ao.fogColor[2] = backEnd.viewParms.farplane_color[2] * tr.identityLight;
	ao.radius = r_aoRadius->value;
	ao.falloff = r_aoFalloff->value;
	ao.thickness = r_aoThickness->value;
	ao.strength = r_aoStrength->value;
	ao.multiBounce = r_aoMultiBounce->value;
	ao.quality = r_aoQuality->integer;
	ao.frames = r_aoTemporal->integer;
	ao.show = r_temporalShow->integer == 3;
	mbTemporalAo( &ao );
}

/*
===============
RB_TemporalEndScene

Before what isn't a view of the world is drawn (Set2DWindow, a view without the world, the end of the frame): the
effects
===============
*/
void RB_TemporalEndScene( void ) {
	const rtCamera_t *c = &rt.camera[0];
	mbTemporalEffects_t effects;
	float exposure, zoom;

	if ( !backEnd.temporalScene && !rt.dofScene ) {
		return;
	}
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	backEnd.temporalScene = qfalse;
	backEnd.temporalCamera = -1;
	rt.dofScene = qfalse;

	Com_Memset( &effects, 0, sizeof( effects ) );
	effects.taa = ( rt.on & MB_TEMPORAL_TAA ) != 0;
	effects.taaFeedback = r_taaFeedback->value;
	effects.taaSharpen = r_taaSharpen->value;
	effects.show = r_temporalShow->integer;
	effects.showScale = r_temporalShowScale->value;

	// the motion blur: an exposure of r_motionBlurShutter frames at r_motionBlurFps is that time's share of the motion
	// between the frames (the main view's game time between them); with r_motionBlurFps 0 the frame's own motion times
	// r_motionBlurShutter. None after a cut
	effects.motionBlur = ( rt.on & MB_TEMPORAL_MB ) && c->frame == rt.frame && c->history;
	if ( effects.motionBlur ) {
		if ( r_motionBlurFps->value > 0.0f ) {
			exposure = c->frameMsec > 0.0f ? r_motionBlurShutter->value * 1000.0f / ( r_motionBlurFps->value * c->frameMsec ) : 0.0f;
		} else {
			exposure = r_motionBlurShutter->value;
		}
		effects.halfExposure = 0.5f * exposure;		// the blur reaches half of it on each side
		effects.cameraShare = Com_Clamp( 0.0f, 1.0f, r_motionBlurCamera->value );
		effects.blurPairs = rt_blurPairs[(int)Com_Clamp( 0, 2, r_motionBlurQuality->integer )];
		R_TemporalMultiply( c->prevProjection, c->delta, effects.reproject );
	}

	// the depth of field: a thin lens, its blur r_dofAperture percent of the view's height at the far distance when the
	// focus is 128 units away, times the zoom (no more than 4 times); the focus eased toward its target with the time
	// constant r_dofFocusSpeed over the main view's game time between the frames, at once after a cut
	effects.dof = ( rt.on & MB_TEMPORAL_DOF ) && c->frame == rt.frame;
	if ( effects.dof ) {
		zoom = Com_Clamp( 0.5f, 4.0f, fabsf( c->projection[5] ) / RT_DOF_PROJ_FOV80 );
		effects.dofLens = r_dofAperture->value * 0.01f * RT_DOF_REFERENCE_FOCUS * zoom;
		effects.dofFocus = r_dofFocus->value > 0.0f ? 1.0f / ( r_dofFocus->value > 8.0f ? r_dofFocus->value : 8.0f ) : 0.0f;
		if ( !c->history || c->frameMsec <= 0.0f || r_dofFocusSpeed->value <= 0.0f ) {
			effects.dofStep = 1.0f;
		} else {
			effects.dofStep = 1.0f - expf( -c->frameMsec / ( 1000.0f * r_dofFocusSpeed->value ) );
		}
		effects.dofSamples = rt_dofSamples[(int)Com_Clamp( 0, 2, r_dofQuality->integer )];
	}
	if ( effects.motionBlur || effects.dof ) {
		Com_Memcpy( effects.projection, c->projection, sizeof( effects.projection ) );
		effects.zFar = c->zFar;
	}
	mbTemporalEndScene( &effects );
}
