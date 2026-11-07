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
                - more and bigger wall splatter, thrown farther
                - wounded enemies keep dripping; head hits spurt
                - bodies pump blood for a few seconds after death
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
	xbPool_t *p = XB_NewSlot( s_pools, MAX_POOLS );

	memset( p, 0, sizeof( *p ));
	p->active = true;
	p->ent = pBody->entindex();
	p->serial = pBody->edict()->serialnumber;
	p->origin = pBody->pev->origin;
	p->color = color;
	p->steps = XBGore_Extreme() ? 14 : 8;
	p->started = gpGlobals->time;
	p->next = gpGlobals->time + 0.8f; // let the body hit the floor first
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

void XBGore_Wounded( CBaseMonster *pMonster, const Vector &point, int hitgroup, float flDamage )
{
	int color = pMonster->BloodColor();

	if( color == DONT_BLEED || XBGore_Mode() == XB_BLOOD_OFF )
		return;

	xbTracked_t *t = XB_Track( pMonster );
	if( !t )
		return;

	t->color = color;
	t->ofs = point - pMonster->pev->origin;

	if( !XBGore_Extreme() )
		return;

	if( pMonster->IsAlive() )
	{
		t->bleedUntil = gpGlobals->time + 4.0f + flDamage * 0.05f;

		if( hitgroup == HITGROUP_HEAD )
			t->arteryUntil = gpGlobals->time + 1.5f;
	}
	else
	{
		// shooting a body re-opens the wound
		t->pumpUntil = Q_max( t->pumpUntil, gpGlobals->time + 1.5f );
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
		t->pumpUntil = gpGlobals->time + RANDOM_FLOAT( 2.5f, 4.0f );
		t->pumpNext = gpGlobals->time + 0.3f;
		// a head shot kill keeps spurting for a moment
		if( pMonster->m_LastHitGroup == HITGROUP_HEAD )
			t->arteryUntil = gpGlobals->time + 1.0f;
	}

	XB_ScreenSplat( pMonster, 140.0f, RANDOM_LONG( 2, 4 ));
}

void XBGore_Gibbed( CBaseMonster *pMonster )
{
	int color = pMonster->BloodColor();

	if( color == DONT_BLEED || !XBGore_Extreme() )
		return;

	Vector c = pMonster->Center();

	for( int i = 0; i < 6; i++ )
		UTIL_BloodStream( c, Vector( RANDOM_FLOAT( -1, 1 ), RANDOM_FLOAT( -1, 1 ), RANDOM_FLOAT( 0.2f, 1.0f )), color, RANDOM_LONG( 120, 200 ));

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

static void XB_UpdatePools( void )
{
	for( int i = 0; i < MAX_POOLS; i++ )
	{
		xbPool_t *p = &s_pools[i];

		if( !p->active || gpGlobals->time < p->next )
			continue;

		// follow the body until it's settled, then stay put
		if( p->ent > 0 )
		{
			edict_t *e = INDEXENT( p->ent );

			if( e && !e->free && e->serialnumber == p->serial )
				p->origin = e->v.origin;
			else p->ent = 0;
		}

		// rings of splats a little wider each step, so the pool spreads
		float radius = 4.0f + p->step * ( XBGore_Extreme() ? 3.5f : 2.5f );
		int splats = p->step < 2 ? 1 : 2;

		for( int j = 0; j < splats; j++ )
		{
			float a = RANDOM_FLOAT( 0.0f, 6.2831853f );
			Vector at = p->origin + Vector( cos( a ) * radius, sin( a ) * radius, 8.0f );

			XB_FloorDecal( at, p->color, 64.0f );
		}

		if( ++p->step >= p->steps )
			p->active = false;
		else p->next = gpGlobals->time + RANDOM_FLOAT( 0.35f, 0.6f );
	}
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

		UTIL_BloodDrips( d->point, Vector( 0, 0, -1 ), d->color, 6 );
		XB_FloorDecal( d->point + Vector( RANDOM_FLOAT( -3, 3 ), RANDOM_FLOAT( -3, 3 ), 0 ), d->color, 512.0f );

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
			UTIL_BloodStream( wound, dir, t->color, RANDOM_LONG( 90, 140 ));
			t->arteryNext = gpGlobals->time + RANDOM_FLOAT( 0.12f, 0.2f );
		}

		// dead: the wound pumps, weaker as it goes
		if( t->pumpUntil > gpGlobals->time && gpGlobals->time >= t->pumpNext )
		{
			float left = ( t->pumpUntil - gpGlobals->time ) / 3.0f;
			Vector dir = Vector( RANDOM_FLOAT( -0.4f, 0.4f ), RANDOM_FLOAT( -0.4f, 0.4f ), RANDOM_FLOAT( 0.4f, 1.0f ));

			UTIL_BloodStream( wound, dir, t->color, (int)( 40 + 80 * Q_min( left, 1.0f )));
			t->pumpNext = gpGlobals->time + RANDOM_FLOAT( 0.25f, 0.45f );
		}

		// wounded and alive: drips as they move
		if( t->bleedUntil > gpGlobals->time && gpGlobals->time >= t->bleedNext && e->v.health > 0 )
		{
			UTIL_BloodDrips( wound, Vector( 0, 0, -1 ), t->color, 4 );
			if( RANDOM_LONG( 0, 2 ) == 0 )
				XB_FloorDecal( wound, t->color );
			t->bleedNext = gpGlobals->time + RANDOM_FLOAT( 0.3f, 0.6f );
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

	XB_UpdatePools();

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

#endif // XASH_UWP
