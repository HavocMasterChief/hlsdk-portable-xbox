/*
xb_gore_world.cpp - Xbox build: blood droplets, splats, pools and footprints
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Client side of the gore, after RealRTCWX cg_gore.c. Everything here is
drawn as the game's own polygons (HUD_DrawTransparentTriangles), not
engine decals, so it can move and grow:

  - Droplets: every bullet hit throws a jet of drops on along the bullet
    (out the far side of the target) plus a little back-spatter. Each drop
    flies under gravity and air drag, drawn as a streak; where it hits the
    world it leaves a splat sized by how big and fast it was.
  - Pools: under a body a pool spreads for several seconds, under the
    ragdoll's pelvis once it has settled.
  - Footprints: walk through a pool and you leave a fading trail of
    bloody prints.
  - A count cap per list: when full, the oldest mark goes (RTCW's
    governor dropped the oldest when the frame rate fell).

Server messages: XBDrops and XBPool, from dlls/xb_gore.cpp.
Sprites: HalfLife/sprites/xb_*.spr (uwp/tools/make_gore_sprites.py).
*/

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "const.h"
#include "com_model.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "triangleapi.h"
#include "pm_defs.h"
#include "event_api.h"

#if XASH_UWP

extern vec3_t v_origin;

#define MAX_DROPS   384
#define MAX_SPLATS  768
#define MAX_POOLS   32
#define MAX_FEET    64

#define DROP_GRAVITY 800.0f
#define DROP_DRAG    1.6f     // per second

enum { KIND_HIT = 0, KIND_SPHERE, KIND_JET, KIND_DRIP };

struct xbDrop_t
{
	bool used;
	Vector pos, vel;
	float size, born;
	bool yellow;
};

struct xbMark_t
{
	bool used;
	Vector org, right, up, normal;
	float born, alpha;
	int frame;
	bool yellow;
};

struct xbPool_t
{
	bool used, placed;
	int ent;
	Vector org, normal;
	float born, start, grow, maxRadius;
	int frame;
	bool yellow;
};

static xbDrop_t s_drops[MAX_DROPS];
static xbMark_t s_splats[MAX_SPLATS];
static xbMark_t s_feet[MAX_FEET];
static xbPool_t s_pools[MAX_POOLS];
static int s_nextSplat, s_nextFoot, s_nextDrop;

static model_s *s_sprSplat, *s_sprDrop, *s_sprPool, *s_sprFoot;
static float s_lastTime;

// footprints
static Vector s_lastStep;
static int s_bloodySteps;
static bool s_leftFoot;

bool XBRagdoll_BodyPelvis( int ent, float *out, bool *settled );

static float RandF( float a, float b ) { return gEngfuncs.pfnRandomFloat( a, b ); }

static model_s *XB_LoadSprite( const char *name )
{
	// world sprites only come back if the server precached them (dlls/weapons.cpp),
	// otherwise load it as a HUD sprite, which needs no precache
	int idx = 0;
	model_s *m = gEngfuncs.CL_LoadModel( name, &idx );
	if( !m )
	{
		HSPRITE h = SPR_Load( name );
		if( h )
			m = (model_s *)gEngfuncs.GetSpritePointer( h );
	}
	return m;
}

/*
================
tracing
================
*/
static bool XB_Trace( const Vector &from, const Vector &to, pmtrace_t *tr )
{
	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
	gEngfuncs.pEventAPI->EV_PlayerTrace( (float *)&from, (float *)&to, PM_WORLD_ONLY, -1, tr );
	return tr->fraction < 1.0f && !tr->startsolid;
}

static void XB_BeginTraces( void )
{
	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers( -1 );
}

static void XB_EndTraces( void )
{
	gEngfuncs.pEventAPI->EV_PopPMStates();
}

static void XB_PlaneAxes( const Vector &normal, const Vector &along, Vector &right, Vector &up )
{
	// "up" of the mark follows 'along' projected onto the surface
	Vector a = along - normal * DotProduct( along, normal );
	if( a.Length() < 0.01f )
		a = fabs( normal.z ) < 0.9f ? CrossProduct( normal, Vector( 0.0f, 0.0f, 1.0f )) : Vector( 1.0f, 0.0f, 0.0f );
	up = a.Normalize();
	right = CrossProduct( up, normal ).Normalize();
}

/*
================
marks
================
*/
static void XB_AddSplat( const Vector &org, const Vector &normal, const Vector &along, float size, float stretch, bool yellow )
{
	xbMark_t *m = &s_splats[s_nextSplat];
	s_nextSplat = ( s_nextSplat + 1 ) % MAX_SPLATS;

	Vector right, up;
	XB_PlaneAxes( normal, along, right, up );

	m->used = true;
	m->org = org + normal * 0.3f;
	m->normal = normal;
	m->right = right * size;
	m->up = up * size * stretch;
	m->born = s_lastTime;
	m->alpha = RandF( 0.75f, 0.95f );
	m->frame = gEngfuncs.pfnRandomLong( 0, 5 );
	m->yellow = yellow;
}

static void XB_AddFoot( const Vector &org, const Vector &normal, const Vector &forward, bool left, float alpha, bool yellow )
{
	xbMark_t *m = &s_feet[s_nextFoot];
	s_nextFoot = ( s_nextFoot + 1 ) % MAX_FEET;

	Vector right, up;
	XB_PlaneAxes( normal, forward, right, up );

	m->used = true;
	m->org = org + right * ( left ? -4.0f : 4.0f ) + normal * 0.35f;
	m->normal = normal;
	m->right = right * 3.6f;      // a boot is about 7 x 18 units, the sprite is 32 x 80
	m->up = up * -9.0f;           // sprite's toe is at the top
	m->born = s_lastTime;
	m->alpha = alpha;
	m->frame = left ? 0 : 1;
	m->yellow = yellow;
}

/*
================
droplets
================
*/
static void XB_NewDrop( const Vector &pos, const Vector &vel, float size, bool yellow )
{
	xbDrop_t *d = NULL;

	for( int i = 0; i < MAX_DROPS; i++ )
	{
		xbDrop_t *c = &s_drops[( s_nextDrop + i ) % MAX_DROPS];
		if( !c->used ) { d = c; break; }
	}
	if( !d )
		d = &s_drops[s_nextDrop];  // full: take the oldest-ish
	s_nextDrop = ( s_nextDrop + 1 ) % MAX_DROPS;

	d->used = true;
	d->pos = pos;
	d->vel = vel;
	d->size = size;
	d->born = s_lastTime;
	d->yellow = yellow;
}

static void XB_DropLand( xbDrop_t *d, pmtrace_t *tr )
{
	Vector n = Vector( tr->plane.normal );
	float speed = d->vel.Length();
	// faster drops smear into longer streaks along their path
	float stretch = 1.0f + Q_min( speed / 500.0f, 1.6f ) * ( 1.0f - fabs( DotProduct( d->vel.Normalize(), n )));

	XB_AddSplat( Vector( tr->endpos ), n, d->vel, d->size * RandF( 1.6f, 2.6f ), stretch, d->yellow );
}

static void XB_UpdateDrops( float dt )
{
	pmtrace_t tr;

	for( int i = 0; i < MAX_DROPS; i++ )
	{
		xbDrop_t *d = &s_drops[i];

		if( !d->used )
			continue;

		if( s_lastTime - d->born > 4.0f )
		{
			d->used = false;
			continue;
		}

		Vector old = d->pos;
		d->vel = d->vel * ( 1.0f - Q_min( DROP_DRAG * dt, 0.5f ));
		d->vel.z -= DROP_GRAVITY * dt;
		d->pos = d->pos + d->vel * dt;

		if( XB_Trace( old, d->pos, &tr ))
		{
			XB_DropLand( d, &tr );
			d->used = false;
		}
	}
}

/*
================
pools
================
*/
static void XB_UpdatePools( void )
{
	pmtrace_t tr;

	for( int i = 0; i < MAX_POOLS; i++ )
	{
		xbPool_t *p = &s_pools[i];

		if( !p->used || p->placed || s_lastTime < p->start )
			continue;

		// under the ragdoll's pelvis once it has come to rest
		Vector at = p->org;
		bool settled = true;
		float pelvis[3];

		if( p->ent > 0 && XBRagdoll_BodyPelvis( p->ent, pelvis, &settled ))
		{
			at = Vector( pelvis );
			if( !settled && s_lastTime - p->born < 4.0f )
				continue; // still tumbling
		}

		if( XB_Trace( at + Vector( 0.0f, 0.0f, 16.0f ), at - Vector( 0.0f, 0.0f, 96.0f ), &tr ) && tr.plane.normal[2] > 0.6f )
		{
			p->org = Vector( tr.endpos ) + Vector( tr.plane.normal ) * 0.2f;
			p->normal = Vector( tr.plane.normal );
			p->placed = true;
			p->start = s_lastTime;
		}
		else if( s_lastTime - p->born > 6.0f )
			p->used = false; // nowhere to pool (body in the air, water)
	}
}

static float XB_PoolRadius( const xbPool_t *p )
{
	float t = ( s_lastTime - p->start ) / p->grow;
	t = t > 1.0f ? 1.0f : t;
	// fast at first, slowing as it spreads
	return p->maxRadius * ( 0.12f + 0.88f * ( 1.0f - ( 1.0f - t ) * ( 1.0f - t )));
}

/*
================
footprints
================
*/
static void XB_UpdateFootprints( void )
{
	cl_entity_t *player = gEngfuncs.GetLocalPlayer();
	pmtrace_t tr;

	if( !player || gHUD.m_Health.m_iHealth <= 0 )
		return;

	Vector org = player->origin;
	Vector moved = org - s_lastStep;
	moved.z = 0;

	if( moved.Length() < 30.0f )
		return;

	Vector forward = moved.Normalize();
	s_lastStep = org;

	// on the ground?
	if( !XB_Trace( org, org - Vector( 0.0f, 0.0f, 48.0f ), &tr ) || tr.plane.normal[2] < 0.7f )
		return;

	Vector floor = Vector( tr.endpos );
	Vector normal = Vector( tr.plane.normal );
	bool yellow = false;

	// stepped in a pool: the next steps carry blood
	for( int i = 0; i < MAX_POOLS; i++ )
	{
		xbPool_t *p = &s_pools[i];
		if( !p->used || !p->placed )
			continue;

		Vector d = floor - p->org;
		if( fabs( d.z ) < 16.0f && d.Make2D().Length() < XB_PoolRadius( p ) * 0.8f )
		{
			s_bloodySteps = 12;
			yellow = p->yellow;
			break;
		}
	}

	if( s_bloodySteps <= 0 )
		return;

	XB_AddFoot( floor, normal, forward, s_leftFoot, 0.25f + 0.65f * ( s_bloodySteps / 12.0f ), yellow );
	s_leftFoot = !s_leftFoot;
	s_bloodySteps--;
}

/*
================
drawing
================
*/
static void XB_Light( const Vector &org, float rgb[3], bool yellow )
{
	float light[3] = { 255.0f, 255.0f, 255.0f };

	if( gEngfuncs.pTriAPI->LightAtPoint )
		gEngfuncs.pTriAPI->LightAtPoint( (float *)&org, light );

	float l = ( light[0] + light[1] + light[2] ) / ( 3.0f * 255.0f );
	l = l < 0.18f ? 0.18f : ( l > 1.0f ? 1.0f : l );

	if( yellow ) { rgb[0] = 0.62f * l; rgb[1] = 0.58f * l; rgb[2] = 0.05f * l; }
	else         { rgb[0] = 0.42f * l; rgb[1] = 0.02f * l; rgb[2] = 0.02f * l; }
}

static void XB_DrawQuad( const Vector &c, const Vector &right, const Vector &up )
{
	triangleapi_t *t = gEngfuncs.pTriAPI;

	t->Begin( TRI_QUADS );
		t->TexCoord2f( 0, 0 ); t->Vertex3fv( c - right + up );
		t->TexCoord2f( 1, 0 ); t->Vertex3fv( c + right + up );
		t->TexCoord2f( 1, 1 ); t->Vertex3fv( c + right - up );
		t->TexCoord2f( 0, 1 ); t->Vertex3fv( c - right - up );
	t->End();
}

static void XB_DrawMarks( xbMark_t *list, int count, model_s *spr )
{
	triangleapi_t *t = gEngfuncs.pTriAPI;
	float rgb[3];

	for( int i = 0; i < count; i++ )
	{
		xbMark_t *m = &list[i];
		if( !m->used )
			continue;

		XB_Light( m->org, rgb, m->yellow );
		t->SpriteTexture( spr, m->frame );
		t->Color4f( rgb[0], rgb[1], rgb[2], m->alpha );
		XB_DrawQuad( m->org, m->right, m->up );
	}
}

static void XB_DrawPools( void )
{
	triangleapi_t *t = gEngfuncs.pTriAPI;
	float rgb[3];

	for( int i = 0; i < MAX_POOLS; i++ )
	{
		xbPool_t *p = &s_pools[i];
		if( !p->used || !p->placed )
			continue;

		Vector right, up;
		float r = XB_PoolRadius( p );

		XB_PlaneAxes( p->normal, Vector( 1.0f, 0.3f, 0.0f ), right, up );
		XB_Light( p->org, rgb, p->yellow );
		t->SpriteTexture( s_sprPool, p->frame );
		t->Color4f( rgb[0] * 0.8f, rgb[1] * 0.8f, rgb[2] * 0.8f, 0.92f );
		XB_DrawQuad( p->org, right * r, up * r );
	}
}

static void XB_DrawDrops( void )
{
	triangleapi_t *t = gEngfuncs.pTriAPI;
	float rgb[3];
	Vector view = Vector( v_origin );

	t->SpriteTexture( s_sprDrop, 0 );

	for( int i = 0; i < MAX_DROPS; i++ )
	{
		xbDrop_t *d = &s_drops[i];
		if( !d->used )
			continue;

		float speed = d->vel.Length();
		Vector dir = speed > 1.0f ? d->vel / speed : Vector( 0.0f, 0.0f, -1.0f );
		Vector side = CrossProduct( dir, d->pos - view );
		if( side.Length() < 0.001f )
			continue;
		side = side.Normalize() * ( d->size * 0.6f );

		// a short streak trailing behind the drop
		float len = Q_max( d->size * 1.5f, Q_min( speed * 0.025f, 14.0f ));
		Vector head = d->pos, tail = d->pos - dir * len;

		XB_Light( d->pos, rgb, d->yellow );
		t->Color4f( rgb[0] * 1.3f, rgb[1] * 1.3f, rgb[2] * 1.3f, 0.95f );
		t->Begin( TRI_QUADS );
			t->TexCoord2f( 0, 0 ); t->Vertex3fv( head - side );
			t->TexCoord2f( 1, 0 ); t->Vertex3fv( head + side );
			t->TexCoord2f( 1, 1 ); t->Vertex3fv( tail + side );
			t->TexCoord2f( 0, 1 ); t->Vertex3fv( tail - side );
		t->End();
	}
}

void XBGoreWorld_Draw( void )
{
	if( !s_sprSplat || !s_sprDrop || !s_sprPool || !s_sprFoot )
		return;

	float now = gEngfuncs.GetClientTime();
	float dt = now - s_lastTime;
	s_lastTime = now;

	if( dt < 0.0f || dt > 0.25f )
		dt = 0.0f; // level change, pause, menu

	XB_BeginTraces();
	if( dt > 0.0f )
		XB_UpdateDrops( dt );
	XB_UpdatePools();
	XB_UpdateFootprints();
	XB_EndTraces();

	triangleapi_t *t = gEngfuncs.pTriAPI;
	t->RenderMode( kRenderTransAlpha );
	t->CullFace( TRI_NONE );

	XB_DrawPools();
	XB_DrawMarks( s_feet, MAX_FEET, s_sprFoot );
	XB_DrawMarks( s_splats, MAX_SPLATS, s_sprSplat );
	XB_DrawDrops();

	t->CullFace( TRI_FRONT );
	t->RenderMode( kRenderNormal );
}

/*
================
messages
================
*/
static int MsgFunc_XBDrops( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	Vector org, dir;
	org.x = READ_COORD();
	org.y = READ_COORD();
	org.z = READ_COORD();
	dir.x = READ_CHAR() / 127.0f;
	dir.y = READ_CHAR() / 127.0f;
	dir.z = READ_CHAR() / 127.0f;
	int kind = READ_BYTE();
	int count = READ_BYTE();
	float speed = READ_BYTE() * 4.0f;
	float spread = READ_BYTE() / 100.0f;
	bool yellow = READ_BYTE() != 0;
	float force = READ_BYTE() / 20.0f;
	bool extreme = READ_BYTE() != 0;

	if( dir.Length() > 0.01f )
		dir = dir.Normalize();

	switch( kind )
	{
	case KIND_HIT:
	{
		// RealRTCWX CG_GoreSpray: a tight fast jet out along the bullet,
		// carrying further for heavier rounds, plus a fine back-spatter
		if( force < 0.2f ) force = 0.2f;
		float k = powf( force, 0.7f );
		int n = (int)(( extreme ? 14.0f : 6.0f ) * k + RandF( 0, 2 ));
		n = n < ( extreme ? 5 : 2 ) ? ( extreme ? 5 : 2 ) : n;
		n = n > ( extreme ? 40 : 16 ) ? ( extreme ? 40 : 16 ) : n;

		Vector exitPt = org + dir * ( force >= 2.5f ? 10.0f : 0.0f );

		for( int i = 0; i < n; i++ )
		{
			float sp = 300.0f + 120.0f * force + RandF( 0, 150 );
			float sprd = 0.08f + RandF( 0, 0.1f );
			Vector v = dir + Vector( RandF( -sprd, sprd ), RandF( -sprd, sprd ), RandF( -sprd, sprd ));
			XB_NewDrop( exitPt + dir * RandF( 2, 5 ), v.Normalize() * sp, ( extreme ? 1.3f : 1.0f ) * ( 0.8f + RandF( 0, 1.3f )), yellow );
		}

		int back = extreme ? 3 + gEngfuncs.pfnRandomLong( 0, 2 ) : 1 + gEngfuncs.pfnRandomLong( 0, 1 );
		for( int i = 0; i < back; i++ )
		{
			Vector v = -dir + Vector( RandF( -0.5f, 0.5f ), RandF( -0.5f, 0.5f ), RandF( -0.2f, 0.6f ));
			XB_NewDrop( org, v.Normalize() * RandF( 50, 120 ), RandF( 0.5f, 1.0f ), yellow );
		}
		break;
	}
	case KIND_SPHERE:
		for( int i = 0; i < count; i++ )
		{
			Vector v = Vector( RandF( -1, 1 ), RandF( -1, 1 ), RandF( -0.3f, 1.0f )).Normalize();
			XB_NewDrop( org, v * speed * RandF( 0.4f, 1.1f ), RandF( 0.9f, 2.2f ), yellow );
		}
		break;
	case KIND_JET:
	case KIND_DRIP:
	default:
		for( int i = 0; i < count; i++ )
		{
			Vector v = dir + Vector( RandF( -spread, spread ), RandF( -spread, spread ), RandF( -spread, spread ));
			XB_NewDrop( org, v.Normalize() * speed * RandF( 0.7f, 1.1f ), kind == KIND_DRIP ? RandF( 1.0f, 1.6f ) : RandF( 0.8f, 1.8f ), yellow );
		}
		break;
	}

	return 1;
}

static int MsgFunc_XBPool( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	int ent = READ_SHORT();
	Vector org;
	org.x = READ_COORD();
	org.y = READ_COORD();
	org.z = READ_COORD();
	bool yellow = READ_BYTE() != 0;
	bool extreme = READ_BYTE() != 0;

	xbPool_t *p = &s_pools[0];
	for( int i = 0; i < MAX_POOLS; i++ )
	{
		if( !s_pools[i].used ) { p = &s_pools[i]; break; }
		if( s_pools[i].born < p->born ) p = &s_pools[i];
	}

	memset( p, 0, sizeof( *p ));
	p->used = true;
	p->ent = ent;
	p->org = org;
	p->born = gEngfuncs.GetClientTime();
	p->start = p->born + 0.8f;          // let the body land
	p->grow = extreme ? RandF( 9.0f, 13.0f ) : RandF( 7.0f, 10.0f );
	p->maxRadius = extreme ? RandF( 34.0f, 44.0f ) : RandF( 24.0f, 32.0f );
	p->frame = gEngfuncs.pfnRandomLong( 0, 2 );
	p->yellow = yellow;
	return 1;
}

void XBGoreWorld_Init( void )
{
	gEngfuncs.pfnHookUserMsg( "XBDrops", MsgFunc_XBDrops );
	gEngfuncs.pfnHookUserMsg( "XBPool", MsgFunc_XBPool );
}

void XBGoreWorld_VidInit( void )
{
	memset( s_drops, 0, sizeof( s_drops ));
	memset( s_splats, 0, sizeof( s_splats ));
	memset( s_feet, 0, sizeof( s_feet ));
	memset( s_pools, 0, sizeof( s_pools ));
	s_nextSplat = s_nextFoot = s_nextDrop = 0;
	s_bloodySteps = 0;
	s_lastTime = 0.0f;

	s_sprSplat = XB_LoadSprite( "sprites/xb_splat.spr" );
	s_sprDrop = XB_LoadSprite( "sprites/xb_drop.spr" );
	s_sprPool = XB_LoadSprite( "sprites/xb_pool.spr" );
	s_sprFoot = XB_LoadSprite( "sprites/xb_foot.spr" );

	gEngfuncs.Con_DPrintf( "xb_gore: sprites splat %p drop %p pool %p foot %p\n", s_sprSplat, s_sprDrop, s_sprPool, s_sprFoot );
}

#endif // XASH_UWP
