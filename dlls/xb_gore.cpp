/*
xb_gore.cpp - Xbox build blood modes and Extreme gore (server side)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Half-Life version of RealRTCWX's cg_gore.c. RTCW draws its gore as its
own polygons on the client; here everything is built from Half-Life's
existing blood effects (decals, blood streams and drip sprites), driven
from the server so it works with any client.

  xb_blood 0  Disabled   no blood at all
           1  Default    stock blood, plus a pool spreading under bodies
           2  Green      as Default, human blood is alien coloured
           3  Extreme    always red, and:
                - blood droplets thrown on along the shot; each one
                  leaves its splat where it lands (client side)
                - wounded enemies keep dripping; now and then a head
                  hit catches an artery and spurts
                - some bodies pump blood for a moment after death
                - splats on walls run down to the floor
                - splats on ceilings drip, leaving spots on the floor
                - blood on the screen when something dies or gibs close by

A count cap per effect type plays the part of RTCW's frame-rate governor:
when a list is full, the oldest effect is replaced.
*/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "player.h"
#include "decals.h"
#include "xb_gore.h"

#if XASH_UWP

static cvar_t xb_blood = { "xb_blood", "1", FCVAR_ARCHIVE };

int gmsgXBGore = 0;
int gmsgXBRagdoll = 0;
int gmsgXBDrops = 0;
int gmsgXBPool = 0;

/*
=============
XB_SendDrops

Blood droplets, simulated and drawn by the client (cl_dll/xb_gore_world.cpp):
they fly under gravity and each leaves its splat where it lands.
kind 0: a bullet hit, the client works out the spray from 'force' like
RealRTCWX CG_GoreSpray; 1: burst every way; 2: jet along dir; 3: drips.
=============
*/
enum { XB_KIND_HIT = 0, XB_KIND_SPHERE, XB_KIND_JET, XB_KIND_DRIP };

static void XB_SendDrops( const Vector &point, const Vector &dir, int kind, int count, float speed, float spread, int color, float force = 0.0f )
{
	if( !gmsgXBDrops || !UTIL_ShouldShowBlood( XBGore_Color( color )))
		return;

	Vector d = dir.Length() > 0.001f ? dir.Normalize() : Vector( 0, 0, 0 );

	MESSAGE_BEGIN( MSG_PVS, gmsgXBDrops, point );
		WRITE_COORD( point.x );
		WRITE_COORD( point.y );
		WRITE_COORD( point.z );
		WRITE_CHAR( (int)( d.x * 127.0f ));
		WRITE_CHAR( (int)( d.y * 127.0f ));
		WRITE_CHAR( (int)( d.z * 127.0f ));
		WRITE_BYTE( kind );
		WRITE_BYTE( Q_min( count, 255 ));
		WRITE_BYTE( (int)Q_min( speed * 0.25f, 255.0f ));
		WRITE_BYTE( (int)Q_min( spread * 100.0f, 255.0f ));
		WRITE_BYTE( XBGore_Color( color ) == BLOOD_COLOR_RED ? 0 : 1 );
		WRITE_BYTE( (int)Q_min( force * 20.0f, 255.0f ));
		WRITE_BYTE( XBGore_Extreme() ? 1 : 0 );
	MESSAGE_END();
}

#define MAX_POOLS     24
#define MAX_RUNS      24
#define MAX_DRIPS     16
#define MAX_TRACKED   1024   // entity indices tracked for bleeding/pumping

struct xbPool_t
{
	bool active;
	int ent;            // follow the body while it settles (0 = fixed point)
	int serial;
	Vector origin;
	int color;
	int step, steps;
	float next, started;
};

struct xbRun_t
{
	bool active;
	Vector point, normal, down;
	int color;
	int step;
	float next, started;
};

struct xbDrip_t
{
	bool active;
	Vector point;
	int color;
	float next, until, started;
};

struct xbTracked_t
{
	int serial;
	int color;
	Vector ofs;                       // wound, relative to the origin
	float bleedUntil, bleedNext;      // wounded and alive: drips
	float arteryUntil, arteryNext;    // head hit: spurts
	float pumpUntil, pumpNext;        // dead: the wound pumps
};

static xbPool_t  s_pools[MAX_POOLS];
static xbRun_t   s_runs[MAX_RUNS];
static xbDrip_t  s_drips[MAX_DRIPS];
static xbTracked_t s_tracked[MAX_TRACKED];

void XBGore_RegisterCvars( void )
{
	CVAR_REGISTER( &xb_blood );
}

int XBGore_Mode( void )
{
	int mode = (int)xb_blood.value;

	if( mode < XB_BLOOD_OFF || mode > XB_BLOOD_EXTREME )
		mode = XB_BLOOD_DEFAULT;

	return mode;
}

bool XBGore_Extreme( void )
{
	return XBGore_Mode() == XB_BLOOD_EXTREME;
}

int XBGore_Color( int color )
{
	if( color == BLOOD_COLOR_RED && XBGore_Mode() == XB_BLOOD_GREEN )
		return BLOOD_COLOR_YELLOW;

	return color;
}

void XBGore_LevelStart( void )
{
	memset( s_pools, 0, sizeof( s_pools ));
	memset( s_runs, 0, sizeof( s_runs ));
	memset( s_drips, 0, sizeof( s_drips ));
	memset( s_tracked, 0, sizeof( s_tracked ));
}

// a free slot, or the oldest one when the list is full
template <typename T>
static T *XB_NewSlot( T *list, int count )
{
	T *oldest = &list[0];

	for( int i = 0; i < count; i++ )
	{
		if( !list[i].active )
			return &list[i];

		if( list[i].started < oldest->started )
			oldest = &list[i];
	}

	return oldest;
}

static xbTracked_t *XB_Track( CBaseEntity *pEnt )
{
	int idx = pEnt->entindex();

	if( idx <= 0 || idx >= MAX_TRACKED )
		return NULL;

	xbTracked_t *t = &s_tracked[idx];
	int serial = pEnt->edict()->serialnumber;

	if( t->serial != serial )
	{
		memset( t, 0, sizeof( *t ));
		t->serial = serial;
	}

	return t;
}

static void XB_FloorDecal( const Vector &point, int color, float maxDrop = 128.0f )
{
	TraceResult tr;

	UTIL_TraceLine( point + Vector( 0, 0, 4 ), point - Vector( 0, 0, maxDrop ), ignore_monsters, NULL, &tr );

	if( tr.flFraction < 1.0f && tr.vecPlaneNormal.z > 0.6f )
		UTIL_BloodDecalTrace( &tr, color );
}

static void XB_ScreenSplat( CBaseEntity *pVictim, float range, int count )
{
	CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( 1 );

	if( !gmsgXBGore || !pPlayer || !pPlayer->IsAlive() || !XBGore_Extreme() )
		return;

	Vector eye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
	Vector toVictim = pVictim->Center() - eye;
	float dist = toVictim.Length();

	if( dist > range )
		return;

	// only when it happened in front of the player
	UTIL_MakeVectors( pPlayer->pev->v_angle );
	if( DotProduct( toVictim.Normalize(), gpGlobals->v_forward ) < 0.4f )
		return;

	if( dist > range * 0.5f )
		count = Q_max( 1, count / 2 );

	MESSAGE_BEGIN( MSG_ONE, gmsgXBGore, NULL, pPlayer->pev );
		WRITE_BYTE( Q_min( count, 12 ));
	MESSAGE_END();
}

static void XB_StartPool( CBaseEntity *pBody, int color )
{
	// drawn and grown by the client, under the ragdoll once it settles
	if( !gmsgXBPool || !UTIL_ShouldShowBlood( XBGore_Color( color )))
		return;

	MESSAGE_BEGIN( MSG_ALL, gmsgXBPool );
		WRITE_SHORT( pBody->entindex() );
		WRITE_COORD( pBody->pev->origin.x );
		WRITE_COORD( pBody->pev->origin.y );
		WRITE_COORD( pBody->pev->origin.z );
		WRITE_BYTE( XBGore_Color( color ) == BLOOD_COLOR_RED ? 0 : 1 );
		WRITE_BYTE( XBGore_Extreme() ? 1 : 0 );
	MESSAGE_END();
}

void XBGore_SurfaceHit( TraceResult *tr, int color )
{
	if( !XBGore_Extreme() || tr->flFraction >= 1.0f )
		return;

	Vector n = tr->vecPlaneNormal;

	if( n.z < -0.7f )
	{
		// ceiling: drips for a while, each drop spotting the floor
		xbDrip_t *d = XB_NewSlot( s_drips, MAX_DRIPS );

		memset( d, 0, sizeof( *d ));
		d->active = true;
		d->point = tr->vecEndPos + n * 2.0f;
		d->color = color;
		d->started = gpGlobals->time;
		d->next = gpGlobals->time + RANDOM_FLOAT( 0.3f, 0.8f );
		d->until = gpGlobals->time + RANDOM_FLOAT( 6.0f, 12.0f );
	}
	else if( fabs( n.z ) < 0.35f && RANDOM_LONG( 0, 99 ) < 45 )
	{
		// wall: some splats run down to the floor
		xbRun_t *r = XB_NewSlot( s_runs, MAX_RUNS );
		Vector down = Vector( 0, 0, -1 ) - n * DotProduct( Vector( 0, 0, -1 ), n );

		memset( r, 0, sizeof( *r ));
		r->active = true;
		r->point = tr->vecEndPos;
		r->normal = n;
		r->down = down.Normalize();
		r->color = color;
		r->started = gpGlobals->time;
		r->next = gpGlobals->time + RANDOM_FLOAT( 0.2f, 0.5f );
	}
}

void XBGore_Wounded( CBaseMonster *pMonster, const Vector &point, const Vector &shotDir, int hitgroup, float flDamage )
{
	int color = pMonster->BloodColor();

	if( color == DONT_BLEED || XBGore_Mode() == XB_BLOOD_OFF )
		return;

	xbTracked_t *t = XB_Track( pMonster );
	if( !t )
		return;

	t->color = color;
	t->ofs = point - pMonster->pev->origin;

	// the exit spray: droplets thrown on along the bullet's path
	XB_SendDrops( point, shotDir, XB_KIND_HIT, 0, 0, 0, color, Q_max( 0.3f, Q_min( flDamage / 12.0f, 4.0f )));

	if( !XBGore_Extreme() )
		return;

	if( pMonster->IsAlive() )
	{
		t->bleedUntil = gpGlobals->time + 4.0f + flDamage * 0.05f;

		// only now and then: a head/neck hit that catches an artery
		if( hitgroup == HITGROUP_HEAD && RANDOM_LONG( 0, 99 ) < 20 )
			t->arteryUntil = gpGlobals->time + RANDOM_FLOAT( 0.8f, 1.4f );
	}
}

void XBGore_Killed( CBaseMonster *pMonster )
{
	int color = pMonster->BloodColor();

	if( color == DONT_BLEED || XBGore_Mode() == XB_BLOOD_OFF )
		return;

	XB_StartPool( pMonster, color );

	if( !XBGore_Extreme() )
		return;

	xbTracked_t *t = XB_Track( pMonster );
	if( t )
	{
		t->color = color;
		if( t->ofs == g_vecZero )
			t->ofs = pMonster->Center() - pMonster->pev->origin;
		t->bleedUntil = 0;
		t->arteryUntil = 0;
		// some bodies keep pumping for a moment, most just bleed out into the pool
		if( RANDOM_LONG( 0, 99 ) < 30 )
		{
			t->pumpUntil = gpGlobals->time + RANDOM_FLOAT( 1.5f, 2.5f );
			t->pumpNext = gpGlobals->time + 0.3f;
		}
	}

	XB_ScreenSplat( pMonster, 140.0f, RANDOM_LONG( 2, 4 ));
}

void XBGore_Gibbed( CBaseMonster *pMonster )
{
	int color = pMonster->BloodColor();

	if( color == DONT_BLEED || !XBGore_Extreme() )
		return;

	Vector c = pMonster->Center();

	XB_SendDrops( c, g_vecZero, XB_KIND_SPHERE, 90, 360.0f, 1.0f, color );

	// throw splatter in every direction
	for( int i = 0; i < 10; i++ )
	{
		TraceResult tr;
		Vector dir = Vector( RANDOM_FLOAT( -1, 1 ), RANDOM_FLOAT( -1, 1 ), RANDOM_FLOAT( -0.6f, 1.0f )).Normalize();

		UTIL_TraceLine( c, c + dir * 256.0f, ignore_monsters, pMonster->edict(), &tr );
		if( tr.flFraction < 1.0f )
		{
			UTIL_BloodDecalTrace( &tr, color );
			XBGore_SurfaceHit( &tr, color );
		}
	}

	for( int i = 0; i < 4; i++ )
		XB_FloorDecal( c + Vector( RANDOM_FLOAT( -40, 40 ), RANDOM_FLOAT( -40, 40 ), 0 ), color );

	XB_ScreenSplat( pMonster, 220.0f, RANDOM_LONG( 5, 9 ));
}

static void XB_UpdateRuns( void )
{
	for( int i = 0; i < MAX_RUNS; i++ )
	{
		xbRun_t *r = &s_runs[i];
		TraceResult tr;

		if( !r->active || gpGlobals->time < r->next )
			continue;

		r->point = r->point + r->down * 6.0f;

		// still on the wall? (stop at edges, never hang in the air)
		UTIL_TraceLine( r->point + r->normal * 4.0f, r->point - r->normal * 6.0f, ignore_monsters, NULL, &tr );
		if( tr.flFraction >= 1.0f || DotProduct( tr.vecPlaneNormal, r->normal ) < 0.9f )
		{
			r->active = false;
			continue;
		}

		UTIL_BloodDecalTrace( &tr, r->color );

		// reached the floor: a small pool where it meets it
		TraceResult floor;
		UTIL_TraceLine( r->point + r->normal * 4.0f, r->point + r->normal * 4.0f - Vector( 0, 0, 10 ), ignore_monsters, NULL, &floor );
		if( floor.flFraction < 1.0f || ++r->step >= 14 )
		{
			XB_FloorDecal( r->point + r->normal * 6.0f, r->color, 24.0f );
			XB_FloorDecal( r->point + r->normal * 12.0f, r->color, 24.0f );
			r->active = false;
			continue;
		}

		r->next = gpGlobals->time + RANDOM_FLOAT( 0.12f, 0.25f );
	}
}

static void XB_UpdateDrips( void )
{
	for( int i = 0; i < MAX_DRIPS; i++ )
	{
		xbDrip_t *d = &s_drips[i];

		if( !d->active || gpGlobals->time < d->next )
			continue;

		if( gpGlobals->time > d->until )
		{
			d->active = false;
			continue;
		}

		XB_SendDrops( d->point, Vector( 0, 0, -1 ), XB_KIND_DRIP, 1, 4.0f, 0.02f, d->color );

		// drips slow down as the blood runs out
		float left = ( d->until - gpGlobals->time ) / 10.0f;
		d->next = gpGlobals->time + RANDOM_FLOAT( 0.4f, 0.8f ) + ( 1.0f - Q_min( left, 1.0f )) * 0.8f;
	}
}

static void XB_UpdateTracked( void )
{
	for( int i = 1; i < MAX_TRACKED && i < gpGlobals->maxEntities; i++ )
	{
		xbTracked_t *t = &s_tracked[i];

		if( !t->serial )
			continue;

		bool busy = t->bleedUntil > gpGlobals->time || t->arteryUntil > gpGlobals->time || t->pumpUntil > gpGlobals->time;
		if( !busy )
			continue;

		edict_t *e = INDEXENT( i );
		if( !e || e->free || e->serialnumber != t->serial || FBitSet( e->v.effects, EF_NODRAW ))
		{
			t->serial = 0;
			continue;
		}

		Vector wound = e->v.origin + t->ofs;

		// head hit: short arterial spurts
		if( t->arteryUntil > gpGlobals->time && gpGlobals->time >= t->arteryNext )
		{
			Vector dir = Vector( RANDOM_FLOAT( -0.6f, 0.6f ), RANDOM_FLOAT( -0.6f, 0.6f ), RANDOM_FLOAT( 0.3f, 0.9f ));
			XB_SendDrops( wound, dir, XB_KIND_JET, 6, RANDOM_FLOAT( 220.0f, 320.0f ), 0.12f, t->color );
			t->arteryNext = gpGlobals->time + RANDOM_FLOAT( 0.18f, 0.3f );
		}

		// dead: the wound pumps, weaker as it goes
		if( t->pumpUntil > gpGlobals->time && gpGlobals->time >= t->pumpNext )
		{
			float left = ( t->pumpUntil - gpGlobals->time ) / 3.0f;
			Vector dir = Vector( RANDOM_FLOAT( -0.4f, 0.4f ), RANDOM_FLOAT( -0.4f, 0.4f ), RANDOM_FLOAT( 0.4f, 1.0f ));

			XB_SendDrops( wound, dir, XB_KIND_JET, (int)( 2 + 5 * Q_min( left, 1.0f )), 80.0f + 140.0f * Q_min( left, 1.0f ), 0.2f, t->color );
			t->pumpNext = gpGlobals->time + RANDOM_FLOAT( 0.3f, 0.5f );
		}

		// wounded and alive: drips as they move
		if( t->bleedUntil > gpGlobals->time && gpGlobals->time >= t->bleedNext && e->v.health > 0 )
		{
			XB_SendDrops( wound, Vector( 0, 0, -1 ), XB_KIND_DRIP, 1, 12.0f, 0.2f, t->color );
			t->bleedNext = gpGlobals->time + RANDOM_FLOAT( 0.35f, 0.7f );
		}
	}
}

void XBGore_Frame( void )
{
	static float last;

	// level change resets time
	if( gpGlobals->time < last )
		XBGore_LevelStart();
	last = gpGlobals->time;

	if( XBGore_Mode() == XB_BLOOD_OFF )
		return;

	if( !XBGore_Extreme() )
		return;

	XB_UpdateRuns();
	XB_UpdateDrips();
	XB_UpdateTracked();
}

/*
=============
XBRagdoll_Send

The client turns the body into a ragdoll if its model has the Bip01
skeleton; others keep their death animation. Pushed away from the attacker.
=============
*/
void XBRagdoll_Send( CBaseMonster *pMonster )
{
	if( !gmsgXBRagdoll || !pMonster )
		return;

	// g_vecAttackDir points from the victim toward whatever hurt it
	Vector push = -g_vecAttackDir;
	if( push == g_vecZero )
		push = Vector( 0, 0, 1 );
	push.z += 0.25f;

	int force = (int)( 120 + pMonster->m_lastDamageAmount * 5.0f );
	if( force > 255 ) force = 255;

	MESSAGE_BEGIN( MSG_ALL, gmsgXBRagdoll );
		WRITE_SHORT( pMonster->entindex() );
		WRITE_COORD( push.x * 100.0f );
		WRITE_COORD( push.y * 100.0f );
		WRITE_COORD( push.z * 100.0f );
		WRITE_BYTE( force );
		WRITE_BYTE( pMonster->m_LastHitGroup );
	MESSAGE_END();
}

void XBRagdoll_Clear( CBaseEntity *pEnt )
{
	if( !gmsgXBRagdoll || !pEnt )
		return;

	MESSAGE_BEGIN( MSG_ALL, gmsgXBRagdoll );
		WRITE_SHORT( pEnt->entindex() );
		WRITE_COORD( 0 );
		WRITE_COORD( 0 );
		WRITE_COORD( 0 );
		WRITE_BYTE( 0 );
		WRITE_BYTE( 255 );
	MESSAGE_END();
}

#endif // XASH_UWP
