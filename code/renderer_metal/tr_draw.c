/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_draw.c -- drawing

#include "tr_local.h"

vec4_t r_colorWhite = { 1.0, 1.0, 1.0, 1.0 };

/*
====================================================================

2D LISTS

What a part of the UI draws, kept to be drawn again (the URC widgets' draw
cache, uiwidget.cpp): the 2D exports between Begin2DList and End2DList go
into the list instead of drawing, and the list is drawn at its end;
Draw2DList draws it again by running the same exports with the same
arguments in the same order, so the same pixels (a list drawn while another
is made is copied into it). renderervk keeps a list as its render thread's
commands; here the 2D draws run at once, so a list made again or freed is
freed at once

====================================================================
*/

#define MAX_2D_LISTS	4096

typedef enum {
	R2D_SET_COLOR,
	R2D_STRETCH_PIC,
	R2D_STRETCH_PIC2,
	R2D_TILE_PIC,
	R2D_TILE_PIC_OFFSET,
	R2D_TRIANGLE_PIC,
	R2D_ADD_BOX,
	R2D_DRAW_BOX,
	R2D_DRAW_BOXES,
	R2D_LINE_LOOP,
	R2D_WINDOW,
	R2D_SCISSOR,
	R2D_SHADER_TIME,
	R2D_POLYGON,
	R2D_STRING
} r2dOp_t;

// one recorded export: its arguments; an array or a string after it
typedef struct {
	int			op;
	int			size;		// its bytes, with what follows, padded
	float		f[12];
	int			i[4];
	const void	*p;
} r2dCommand_t;

typedef struct {
	byte	*data;
	int		used, size;
} r2dList_t;

static struct {
	r2dList_t	*lists[MAX_2D_LISTS];
	r2dList_t	*recording;		// the list being made
	int			handle;			// ... its handle
	r2dList_t	*spare;			// the last one freed, for the next made
} r2dl;

static r2dList_t *R_New2DList( void ) {
	r2dList_t *l = r2dl.spare;

	if ( l ) {
		r2dl.spare = NULL;
	} else {
		l = ri.Malloc( sizeof( *l ) );
		l->size = 1024;
		l->data = ri.Malloc( l->size );
	}
	l->used = 0;
	return l;
}

static void R_Free2DListData( r2dList_t *l ) {
	if ( !r2dl.spare ) {
		r2dl.spare = l;
		return;
	}
	ri.Free( l->data );
	ri.Free( l );
}

// room for bytes more in the list being made
static void *R_2DListReserve( int bytes ) {
	r2dList_t *l = r2dl.recording;

	if ( l->used + bytes > l->size ) {
		const int	size = Q_max( l->size * 2, l->used + bytes + 1024 );
		byte		*data = ri.Malloc( size );

		Com_Memcpy( data, l->data, l->used );
		ri.Free( l->data );
		l->data = data;
		l->size = size;
	}
	l->used += bytes;
	return l->data + l->used - bytes;
}

// a command in the list being made, extra bytes after it
static r2dCommand_t *R_2DRecord( int op, int extra ) {
	const int		bytes = PAD( (int)sizeof( r2dCommand_t ) + extra, (int)sizeof( void * ) );
	r2dCommand_t	*cmd = R_2DListReserve( bytes );

	Com_Memset( cmd, 0, sizeof( *cmd ) );
	cmd->op = op;
	cmd->size = bytes;
	return cmd;
}

static void R_Run2DList( const r2dList_t *l ) {
	const byte *data = l->data, *end = l->data + l->used;

	while ( data < end ) {
		const r2dCommand_t	*c = (const r2dCommand_t *)data;
		const void			*after = c + 1;

		switch ( c->op ) {
		case R2D_SET_COLOR:
			Draw_SetColor( c->i[0] ? NULL : c->f );
			break;
		case R2D_STRETCH_PIC:
			Draw_StretchPic( c->f[0], c->f[1], c->f[2], c->f[3], c->f[4], c->f[5], c->f[6], c->f[7], c->i[0] );
			break;
		case R2D_STRETCH_PIC2:
			Draw_StretchPic2( c->f[0], c->f[1], c->f[2], c->f[3], c->f[4], c->f[5], c->f[6], c->f[7], c->f[8], c->f[9], c->i[0] );
			break;
		case R2D_TILE_PIC:
			Draw_TilePic( c->f[0], c->f[1], c->f[2], c->f[3], c->i[0] );
			break;
		case R2D_TILE_PIC_OFFSET:
			Draw_TilePicOffset( c->f[0], c->f[1], c->f[2], c->f[3], c->i[0], c->i[1], c->i[2] );
			break;
		case R2D_TRIANGLE_PIC:
			Draw_TrianglePic( (const vec2_t *)&c->f[0], (const vec2_t *)&c->f[6], c->i[0] );
			break;
		case R2D_ADD_BOX:
			AddBox( c->f[0], c->f[1], c->f[2], c->f[3] );
			break;
		case R2D_DRAW_BOX:
			DrawBox( c->f[0], c->f[1], c->f[2], c->f[3] );
			break;
		case R2D_DRAW_BOXES:
			DrawBoxes( (const float (*)[4])after, c->i[0], c->f );
			break;
		case R2D_LINE_LOOP:
			DrawLineLoop( (const vec2_t *)after, c->i[0], c->i[1], c->i[2] );
			break;
		case R2D_WINDOW:
			Set2DWindow( c->i[0], c->i[1], c->i[2], c->i[3], c->f[0], c->f[1], c->f[2], c->f[3], c->f[4], c->f[5] );
			break;
		case R2D_SCISSOR:
			RE_Scissor( c->i[0], c->i[1], c->i[2], c->i[3] );
			break;
		case R2D_SHADER_TIME:
			Set2DInitialShaderTime( c->f[0] );
			break;
		case R2D_POLYGON:
			DrawPolygon2D( (const float (*)[4])after, c->i[0], c->i[1] );
			break;
		case R2D_STRING:
			R_DrawString( (fontheader_t *)c->p, (const char *)after, c->f[0], c->f[1], c->i[0], c->i[1] ? &c->f[2] : NULL );
			break;
		default:
			ri.Error( ERR_FATAL, "R_Run2DList: bad command %d", c->op );
		}
		data += c->size;
	}
}

qboolean R_2DListRecording( void ) {
	return r2dl.recording != NULL;
}

// R_DrawString's, into the list being made
void R_2DListString( fontheader_t *font, const char *text, float x, float y, int maxlen, const float *pvVirtualScreen ) {
	const int		len = Q_min( (int)strlen( text ), 8191 );
	r2dCommand_t	*c = R_2DRecord( R2D_STRING, len + 1 );

	c->p = font;
	c->f[0] = x;
	c->f[1] = y;
	c->i[0] = maxlen;
	if ( pvVirtualScreen ) {
		c->i[1] = 1;
		c->f[2] = pvVirtualScreen[0];
		c->f[3] = pvVirtualScreen[1];
	}
	Com_Memcpy( c + 1, text, len );
	( (char *)( c + 1 ) )[len] = 0;
}

// a list's handle (a new one for 0): the 2D exports go into it until End2DList
int RE_Begin2DList( int handle ) {
	int i;

	if ( r2dl.recording ) {
		return 0;
	}
	if ( handle <= 0 || handle > MAX_2D_LISTS ) {
		for ( i = 0; i < MAX_2D_LISTS && r2dl.lists[i]; i++ ) {
		}
		if ( i == MAX_2D_LISTS ) {
			return 0;
		}
		handle = i + 1;
	}
	r2dl.recording = R_New2DList();
	r2dl.handle = handle;
	// held while made, so no other Begin2DList takes it
	if ( !r2dl.lists[handle - 1] ) {
		r2dl.lists[handle - 1] = r2dl.recording;
	}
	return handle;
}

void RE_Draw2DList( int handle ) {
	const r2dList_t *l = handle > 0 && handle <= MAX_2D_LISTS ? r2dl.lists[handle - 1] : NULL;

	if ( !l || l == r2dl.recording ) {
		return;
	}
	if ( r2dl.recording ) {
		Com_Memcpy( R_2DListReserve( l->used ), l->data, l->used );
		return;
	}
	R_Run2DList( l );
}

// the list made: kept, and drawn
void RE_End2DList( void ) {
	r2dList_t	*l = r2dl.recording;
	r2dList_t	*old;

	if ( !l ) {
		return;
	}
	r2dl.recording = NULL;
	old = r2dl.lists[r2dl.handle - 1];
	r2dl.lists[r2dl.handle - 1] = l;
	if ( old && old != l ) {
		R_Free2DListData( old );
	}
	R_Run2DList( l );
}

void RE_Free2DList( int handle ) {
	if ( handle <= 0 || handle > MAX_2D_LISTS || !r2dl.lists[handle - 1] || r2dl.lists[handle - 1] == r2dl.recording ) {
		return;
	}
	R_Free2DListData( r2dl.lists[handle - 1] );
	r2dl.lists[handle - 1] = NULL;
}

// a list left unfinished (an error in the UI's drawing): dropped, so the next
// frame's 2D draws again
void R_2DListsBeginFrame( void ) {
	if ( r2dl.recording ) {
		if ( r2dl.lists[r2dl.handle - 1] == r2dl.recording ) {
			r2dl.lists[r2dl.handle - 1] = NULL;
		}
		R_Free2DListData( r2dl.recording );
		r2dl.recording = NULL;
	}
}

// the renderer's shutdown: every list
void R_2DListsShutdown( void ) {
	int i;

	R_2DListsBeginFrame();
	for ( i = 0; i < MAX_2D_LISTS; i++ ) {
		if ( r2dl.lists[i] ) {
			ri.Free( r2dl.lists[i]->data );
			ri.Free( r2dl.lists[i] );
		}
	}
	if ( r2dl.spare ) {
		ri.Free( r2dl.spare->data );
		ri.Free( r2dl.spare );
	}
	Com_Memset( &r2dl, 0, sizeof( r2dl ) );
}

/*
================
Draw_SetColor
================
*/
void Draw_SetColor(const vec4_t rgba) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_SET_COLOR, 0);

		if (rgba) {
			Vector4Copy(rgba, c->f);
		} else {
			c->i[0] = 1;
		}
		return;
	}

#if 1
	if (!rgba) {
		rgba = r_colorWhite;
	}

	backEnd.color2D[0] = (byte)(rgba[0] * tr.identityLightByte);
	backEnd.color2D[1] = (byte)(rgba[1] * tr.identityLightByte);
	backEnd.color2D[2] = (byte)(rgba[2] * tr.identityLightByte);
	backEnd.color2D[3] = (byte)(rgba[3] * 255.0);
	mbColor4ubv(backEnd.color2D);
#else
	RE_SetColor(rgba);
#endif
}

/*
================
Draw_StretchPic
================
*/
void Draw_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_STRETCH_PIC, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		c->f[4] = s1; c->f[5] = t1; c->f[6] = s2; c->f[7] = t2;
		c->i[0] = hShader;
		return;
	}

#if 1
	shader_t* shader;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex2f(x, y);

	RB_Texcoord2f(s2, t1);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(s1, t2);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f(s2, t2);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
#else
	RE_StretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
#endif
}

/*
================
Draw_StretchPic2
================
*/
void Draw_StretchPic2(float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_STRETCH_PIC2, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		c->f[4] = s1; c->f[5] = t1; c->f[6] = s2; c->f[7] = t2;
		c->f[8] = sx; c->f[9] = sy;
		c->i[0] = hShader;
		return;
	}

	shader_t* shader;
	float halfWidth, halfHeight;
	float scaledWidth1, scaledHeight1;
	float scaledWidth2, scaledHeight2;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	halfWidth = w * 0.5f;
	halfHeight = h * 0.5f;
	scaledWidth1 = halfWidth * sy;
	scaledHeight1 = halfHeight * sx;
	scaledWidth2 = halfWidth * sx;
	scaledHeight2 = halfHeight * sy;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex3f(x + halfWidth - (scaledWidth2 + -scaledHeight2), y + halfWidth - scaledHeight1 - scaledWidth1, 0);

	RB_Texcoord2f(t2, t1);
	RB_Vertex3f(scaledWidth2 - -scaledHeight2 + x + halfWidth, scaledWidth1 - scaledHeight1 + y + halfWidth, 0);

	RB_Texcoord2f(s1, s2);
	RB_Vertex3f(x+ halfWidth - (scaledWidth2 + scaledHeight2), scaledHeight1 - scaledWidth1 + y + halfWidth, 0);

	RB_Texcoord2f(t2, s2);
	RB_Vertex3f(scaledWidth2 - scaledHeight2 + x + halfWidth, scaledWidth1 + scaledHeight1 + y + halfWidth, 0);

	RB_StreamEnd();
}


/*
================
Draw_TilePic
================
*/
void Draw_TilePic(float x, float y, float w, float h, qhandle_t hShader) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_TILE_PIC, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		c->i[0] = hShader;
		return;
	}

	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x, y);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
}

/*
================
Draw_TilePicOffset
================
*/
void Draw_TilePicOffset(float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_TILE_PIC_OFFSET, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		c->i[0] = hShader; c->i[1] = offsetX; c->i[2] = offsetY;
		return;
	}

	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x + offsetX, y + offsetY);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX, y + offsetY + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY + h);

	RB_StreamEnd();
}

/*
================
Draw_TrianglePic
================
*/
void Draw_TrianglePic(const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_TRIANGLE_PIC, 0);

		Com_Memcpy(&c->f[0], vPoints, sizeof(vec2_t) * 3);
		Com_Memcpy(&c->f[6], vTexCoords, sizeof(vec2_t) * 3);
		c->i[0] = hShader;
		return;
	}

	int			i;
	shader_t* shader;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_BeginSurface(shader);

	for (i = 0; i < 3; i++) {
		RB_Texcoord2f(vTexCoords[i][0], vTexCoords[i][1]);
		RB_Vertex2f(vPoints[i][0], vPoints[i][1]);
	}

	RB_StreamEnd();
}

/*
================
RE_DrawBackground_TexSubImage
================
*/
void RE_DrawBackground_TexSubImage(int cols, int rows, int bgr, byte* data) {
	int	format;
	int		w, h;

	w = glConfig.vidWidth;
	h = glConfig.vidHeight;

	R_IssuePendingRenderCommands();
	mbFinish();

	if (bgr) {
		format = MB_BGR_EXT;
	}
	else {
		format = MB_RGB;
	}

	R_Bind(tr.scratchImage);

	if (cols == tr.scratchImage->width && rows == tr.scratchImage->height && format == tr.scratchImage->internalFormat)
	{
		mbTexSubImage2D(MB_TEXTURE_2D, 0, 0, 0, cols, rows, format, MB_UNSIGNED_BYTE, data);
	}
	else
	{
		tr.scratchImage->uploadWidth = cols;
		tr.scratchImage->uploadHeight = rows;
		tr.scratchImage->internalFormat = format;
		mbTexImage2D(MB_TEXTURE_2D, 0, MB_RGB, cols, rows, 0, format, MB_UNSIGNED_BYTE, data);
		mbTexParameterf(MB_TEXTURE_2D, MB_TEXTURE_MIN_FILTER, MB_LINEAR);
		mbTexParameterf(MB_TEXTURE_2D, MB_TEXTURE_MAG_FILTER, MB_LINEAR);
	}

	mbDisable(MB_CULL_FACE);
	mbDisable(MB_DEPTH_TEST);
	mbEnable(MB_TEXTURE_2D);

	mbBegin(MB_QUADS);

	mbTexCoord2f(0.5 / (float)cols, ((float)rows - 0.5) / rows);
	mbVertex2f(0, 0);

	mbTexCoord2f(((float)cols - 0.5) / cols, ((float)rows - 0.5) / rows);
	mbVertex2f(w, 0);

	mbTexCoord2f(((float)cols - 0.5) / cols, 0.5 / (float)rows);
	mbVertex2f(w, h);

	mbTexCoord2f(0.5 / (float)rows, 0.5 / (float)rows);
	mbVertex2f(0, h);

	mbEnd();
}

/*
================
RE_DrawBackground_DrawPixels
================
*/
void RE_DrawBackground_DrawPixels(int cols, int rows, int bgr, byte* data) {
	R_IssuePendingRenderCommands();

	R_State(0);
	mbDisable(MB_TEXTURE_2D);

	mbPixelZoom(glConfig.vidWidth / rows, glConfig.vidHeight / cols);

	if (bgr) {
		mbDrawPixels(cols, rows, MB_BGR, MB_UNSIGNED_BYTE, data);
	} else {
		mbDrawPixels(cols, rows, MB_RGB, MB_UNSIGNED_BYTE, data);
	}

	mbPixelZoom(1.0, 1.0);

	mbEnable(MB_TEXTURE_2D);
}

/*
================
AddBox
================
*/
void AddBox(float x, float y, float w, float h) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_ADD_BOX, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		return;
	}

	R_IssuePendingRenderCommands();

	mbColor4ubv(backEnd.color2D);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

	mbBegin(MB_QUADS);

	mbVertex2f(x, y);
	mbVertex2f(x + w, y);
	mbVertex2f(x + w, y + h);
	mbVertex2f(x, y + h);

	mbEnd();

	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawBoxes

DrawBox for count boxes in one draw, after the stretch pics before it (the Metal port's crosshair)
================
*/
void DrawBoxes(const float (*boxes)[4], int count, const float *color) {
	if (r2dl.recording) {
		r2dCommand_t *c;

		if (count <= 0) {
			return;
		}
		c = R_2DRecord(R2D_DRAW_BOXES, (int)sizeof(float) * 4 * count);
		Vector4Copy(color, c->f);
		c->i[0] = count;
		Com_Memcpy(c + 1, boxes, sizeof(float) * 4 * count);
		return;
	}

	int i;

	R_IssuePendingRenderCommands();
	if (tess.numIndexes) {
		RB_EndSurface();	// the stretch pics before it, in the 2D color of now
	}

	mbColor4f(color[0], color[1], color[2], color[3]);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

	mbBegin(MB_QUADS);
	for (i = 0; i < count; i++) {
		const float *b = boxes[i];

		mbVertex2f(b[0], b[1]);
		mbVertex2f(b[0] + b[2], b[1]);
		mbVertex2f(b[0] + b[2], b[1] + b[3]);
		mbVertex2f(b[0], b[1] + b[3]);
	}
	mbEnd();

	mbColor4ubv(backEnd.color2D);	// as DrawBox leaves it
	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawPolygon2D

A convex polygon of count points (x, y, s, t each) as a fan, in one draw
(the Competitive HUD's radar), in the 2D color as a stretch pic
================
*/
void DrawPolygon2D(const float (*points)[4], int count, qhandle_t hShader) {
	shader_t* shader;
	int i;

	if (count < 3 || count > SHADER_MAX_VERTEXES / 2 || 3 * (count - 2) > SHADER_MAX_INDEXES / 2) {
		return;
	}
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_POLYGON, (int)sizeof(float) * 4 * count);

		c->i[0] = count;
		c->i[1] = hShader;
		Com_Memcpy(c + 1, points, sizeof(float) * 4 * count);
		return;
	}

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	for (i = 0; i < count; i++) {
		RB_Texcoord2f(points[i][2], points[i][3]);
		RB_Vertex2f(points[i][0], points[i][1]);
	}
	for (i = 2; i < count; i++) {
		tess.indexes[tess.numIndexes++] = 0;
		tess.indexes[tess.numIndexes++] = i - 1;
		tess.indexes[tess.numIndexes++] = i;
	}

	RB_EndSurface();
}

/*
================
DrawBox
================
*/
void DrawBox(float x, float y, float w, float h) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_DRAW_BOX, 0);

		c->f[0] = x; c->f[1] = y; c->f[2] = w; c->f[3] = h;
		return;
	}

	R_IssuePendingRenderCommands();

	mbColor4ubv(backEnd.color2D);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

	mbBegin(MB_QUADS);

	mbVertex2f(x, y);
	mbVertex2f(x + w, y);
	mbVertex2f(x + w, y + h);
	mbVertex2f(x, y + h);

	mbEnd();

	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawLineLoop
================
*/
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask) {
	if (r2dl.recording) {
		r2dCommand_t *c;

		if (count <= 0) {
			return;
		}
		c = R_2DRecord(R2D_LINE_LOOP, (int)sizeof(vec2_t) * count);
		c->i[0] = count; c->i[1] = stipple_factor; c->i[2] = stipple_mask;
		Com_Memcpy(c + 1, points, sizeof(vec2_t) * count);
		return;
	}

	int		i;

	R_IssuePendingRenderCommands();

	mbDisable(MB_TEXTURE_2D);

	if (stipple_factor) {
		mbEnable(MB_LINE_STIPPLE);
		mbLineStipple(stipple_factor, stipple_mask);
	}

	mbBegin(MB_LINE_LOOP);

	for (i = 0; i < count; i++) {
		mbVertex2f(points[i][0], points[i][1]);
	}

	mbEnd();

	mbEnable(MB_TEXTURE_2D);

	if (stipple_factor) {
		mbDisable(MB_LINE_STIPPLE);
	}
}

/*
================
Set2DWindow
================
*/
void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_WINDOW, 0);

		c->i[0] = x; c->i[1] = y; c->i[2] = w; c->i[3] = h;
		c->f[0] = left; c->f[1] = right; c->f[2] = bottom; c->f[3] = top; c->f[4] = n; c->f[5] = f;
		return;
	}

	R_IssuePendingRenderCommands();
	mbViewport(x, y, w, h);
	mbScissor(x, y, w, h);
	mbMatrixMode(MB_PROJECTION);
	mbLoadIdentity();
	mbOrtho(left, right, bottom, top, n, f);
	mbMatrixMode(MB_MODELVIEW);

	mbLoadIdentity();
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);
	mbEnable(MB_BLEND);
	mbDisable(MB_CULL_FACE);
	mbDisable(MB_CLIP_PLANE0);
	// Make sure to disable the fog to avoid messing up with the UI
	mbDisable(MB_FOG);
	mbFogf(MB_FOG_START, 0);

	if (r_reset_tc_array->integer) {
		mbDisableClientState(MB_TEXTURE_COORD_ARRAY);
	}

	if (!backEnd.in2D)
	{
		backEnd.refdef.time = ri.Milliseconds();
		backEnd.in2D = qtrue;
		backEnd.refdef.floatTime = backEnd.refdef.time / 1000.0;
        backEnd.shaderStartTime = 0; 
	}
}

/*
================
RE_Scissor
================
*/
void RE_Scissor(int x, int y, int width, int height) {
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_SCISSOR, 0);

		c->i[0] = x; c->i[1] = y; c->i[2] = width; c->i[3] = height;
		return;
	}

	mbEnable(MB_SCISSOR_TEST);
	mbScissor(x, y, width, height);
}

/*
================
Set2DInitialShaderTime
================
*/
void Set2DInitialShaderTime(float startTime)
{
	if (r2dl.recording) {
		r2dCommand_t *c = R_2DRecord(R2D_SHADER_TIME, 0);

		c->f[0] = startTime;
		return;
	}

	if (backEnd.in2D)
	{
		backEnd.shaderStartTime = startTime;
	}
}
