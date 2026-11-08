/*
xb_ragdoll_sv.cpp - Xbox build: ragdolls as bodies in the world
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

The game runs the ragdoll physics (game_shared/xb_rdphys.cpp), so it knows
where every limb is:

  - bullets are checked against every limb: a hit bleeds like a hit on a
    living monster and shoves the limb
  - the player's legs push limbs aside, harder when sprinting, and stepping
    through a fresh body squeezes out a little blood
  - explosions throw bodies (through DeadTakeDamage -> XBRagdoll_Hit)
  - the corpse entity follows the body, so pools, explosions and sounds
    happen where the body really is

The client only draws (cl_dll/xb_ragdoll.cpp), from the positions sent here:
  XBRagdoll  short ent, byte kind
    kind 0 start:   byte np, np x ( byte bone, char parent, char axis, char ref )
    kind 2 clear
    kind 3 update:  byte np, byte asleep, np x 3 short (position * 4)
*/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "player.h"
#include "studio.h"
#include "xb_gore.h"
#include "xb_rdphys.h"

#if XASH_UWP

enum { XB_RD_START = 0, XB_RD_CLEAR = 2, XB_RD_UPDATE = 3 };

#define XB_RD_MAX       24
#define XB_RD_SEND_RATE ( 1.0f / 30.0f )

struct xbRagdoll_t
{
	bool used;
	EHANDLE ent;
	int index;
	string_t model;
	rdpBody phys;
	float accum, nextSend, died, nextSquish;
	bool sentAsleep;
	int sequence;          // the pose the skeleton was read in, held so the
	float frame;           // client starts from the very same pose
};

static xbRagdoll_t s_rd[XB_RD_MAX];
static float s_lastTime;
extern int gmsgXBRagdoll;

static void RD_SvTrace( const rdpVec &from, const rdpVec &to, rdpTrace *out )
{
	TraceResult tr;
	UTIL_TraceLine( Vector( from.x, from.y, from.z ), Vector( to.x, to.y, to.z ), ignore_monsters, NULL, &tr );
	out->fraction = tr.flFraction;
	out->end = rdpVec( tr.vecEndPos.x, tr.vecEndPos.y, tr.vecEndPos.z );
	out->normal = rdpVec( tr.vecPlaneNormal.x, tr.vecPlaneNormal.y, tr.vecPlaneNormal.z );
	out->startsolid = tr.fStartSolid != 0;
	out->allsolid = tr.fAllSolid != 0;
}

static inline rdpVec RV( const Vector &v ) { return rdpVec( v.x, v.y, v.z ); }
static inline Vector VR( const rdpVec &v ) { return Vector( v.x, v.y, v.z ); }

static xbRagdoll_t *RD_Find( int index )
{
	for( int i = 0; i < XB_RD_MAX; i++ )
		if( s_rd[i].used && s_rd[i].index == index )
			return &s_rd[i];
	return NULL;
}

static void RD_SendClear( int index )
{
	if( !gmsgXBRagdoll )
		return;
	MESSAGE_BEGIN( MSG_ALL, gmsgXBRagdoll );
		WRITE_SHORT( index );
		WRITE_BYTE( XB_RD_CLEAR );
	MESSAGE_END();
}

static void RD_SendStart( xbRagdoll_t *r )
{
	MESSAGE_BEGIN( MSG_ALL, gmsgXBRagdoll );
		WRITE_SHORT( r->index );
		WRITE_BYTE( XB_RD_START );
		WRITE_BYTE( r->phys.np );
		for( int n = 0; n < r->phys.np; n++ )
		{
			WRITE_BYTE( r->phys.bone[n] );
			WRITE_CHAR( r->phys.parent[n] );
			WRITE_CHAR( r->phys.axis[n] );
			WRITE_CHAR( r->phys.ref[n] );
		}
	MESSAGE_END();
}

static int RD_Pack( float v )
{
	int i = (int)( v * 4.0f + ( v >= 0 ? 0.5f : -0.5f ));
	return i < -32767 ? -32767 : ( i > 32767 ? 32767 : i );
}

static void RD_SendUpdate( xbRagdoll_t *r )
{
	MESSAGE_BEGIN( MSG_ALL, gmsgXBRagdoll );
		WRITE_SHORT( r->index );
		WRITE_BYTE( XB_RD_UPDATE );
		WRITE_BYTE( r->phys.np );
		WRITE_BYTE( r->phys.moving ? 0 : 1 );
		for( int n = 0; n < r->phys.np; n++ )
		{
			WRITE_SHORT( RD_Pack( r->phys.p[n].x ));
			WRITE_SHORT( RD_Pack( r->phys.p[n].y ));
			WRITE_SHORT( RD_Pack( r->phys.p[n].z ));
		}
	MESSAGE_END();
}

// the corpse entity sits where the body is, on the floor under its middle
static void RD_MoveCorpse( xbRagdoll_t *r, CBaseEntity *pEnt )
{
	Vector mid = VR( r->phys.p[0] );
	TraceResult tr;
	UTIL_TraceLine( mid, mid - Vector( 0, 0, 64 ), ignore_monsters, pEnt->edict(), &tr );
	UTIL_SetOrigin( pEnt->pev, tr.flFraction < 1.0f ? tr.vecEndPos : mid );
}

/*
=============
XBRagdoll_Begin

The monster just died and didn't gib: its skeleton becomes a body.
=============
*/
bool XBRagdoll_Begin( CBaseMonster *pMonster, const Vector &hit, const Vector &push, const Vector &vel )
{
	RDP_Trace = RD_SvTrace;

	if( !gmsgXBRagdoll || !pMonster || CVAR_GET_FLOAT( "xb_ragdoll" ) == 0.0f )
		return false;

	studiohdr_t *hdr = (studiohdr_t *)GET_MODEL_PTR( pMonster->edict( ));
	if( !hdr || hdr->numbones <= 0 || hdr->numbones > RDP_MAX_BONES )
		return false;

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex );
	const char *names[RDP_MAX_BONES];
	int parents[RDP_MAX_BONES];
	rdpVec pos[RDP_MAX_BONES];

	for( int i = 0; i < hdr->numbones; i++ )
	{
		Vector org, ang;
		GET_BONE_POSITION( pMonster->edict(), i, org, ang );
		names[i] = pbones[i].name;
		parents[i] = pbones[i].parent;
		pos[i] = RV( org );
	}

	xbRagdoll_t *r = RD_Find( pMonster->entindex( ));
	if( !r )
	{
		// a free slot, else the body that has lain longest
		r = &s_rd[0];
		for( int i = 0; i < XB_RD_MAX; i++ )
		{
			if( !s_rd[i].used ) { r = &s_rd[i]; break; }
			if( s_rd[i].died < r->died ) r = &s_rd[i];
		}
		if( r->used && r->index != pMonster->entindex( ))
		{
			// that one freezes where it is
			r->used = false;
		}
	}

	memset( r, 0, sizeof( *r ));
	if( !RDP_Build( &r->phys, hdr->numbones, names, parents, pos ))
	{
		ALERT( at_aiconsole, "ragdoll: %s has no usable skeleton\n", STRING( pMonster->pev->model ));
		return false;
	}

	r->used = true;
	r->ent = pMonster;
	r->index = pMonster->entindex();
	r->model = pMonster->pev->model;
	r->died = gpGlobals->time;
	r->nextSend = 0.0f;
	r->sequence = pMonster->pev->sequence;
	r->frame = pMonster->pev->frame;
	pMonster->pev->framerate = 0.0f;

	RDP_Throw( &r->phys, RV( hit ), RV( push ), RV( vel ));

	// bullets meet the limbs from now on, not the corpse's box
	pMonster->pev->solid = SOLID_NOT;
	pMonster->pev->movetype = MOVETYPE_NONE;
	UTIL_SetSize( pMonster->pev, Vector( -8, -8, 0 ), Vector( 8, 8, 8 ));

	ALERT( at_aiconsole, "ragdoll: %s started, %s, %d particles\n", STRING( pMonster->pev->model ), r->phys.human ? "human" : "generic", r->phys.np );

	RD_SendStart( r );
	RD_SendUpdate( r );
	return true;
}

void XBRagdoll_Forget( CBaseEntity *pEnt )
{
	if( !pEnt )
		return;

	xbRagdoll_t *r = RD_Find( pEnt->entindex( ));
	if( r )
		r->used = false;
	RD_SendClear( pEnt->entindex( ));
}

/*
=============
XBRagdoll_Shove

Blasts and other damage on a body already down (DeadTakeDamage).
=============
*/
void XBRagdoll_Shove( CBaseEntity *pEnt, const Vector &hit, const Vector &push )
{
	xbRagdoll_t *r = pEnt ? RD_Find( pEnt->entindex( )) : NULL;
	if( !r )
		return;

	RDP_Impulse( &r->phys, RV( hit ), RV( push ), 20.0f );
	r->sentAsleep = false;
}

/*
=============
XBRagdoll_Bullet

A bullet from src toward end: did it hit a body first? Bleeds and shoves it.
=============
*/
bool XBRagdoll_Bullet( const Vector &src, const Vector &end, const Vector &dir, float damage )
{
	float bestFrac = 2.0f;
	xbRagdoll_t *best = NULL;
	rdpVec bestHit;
	int bestP = 0;

	for( int i = 0; i < XB_RD_MAX; i++ )
	{
		xbRagdoll_t *r = &s_rd[i];
		float frac;
		rdpVec hit;
		int p;

		if( !r->used || !RDP_RayHit( &r->phys, RV( src ), RV( end ), &frac, &hit, &p ))
			continue;

		if( frac < bestFrac )
		{
			bestFrac = frac;
			best = r;
			bestHit = hit;
			bestP = p;
		}
	}

	if( !best )
		return false;

	CBaseEntity *pEnt = best->ent;
	CBaseMonster *pMon = pEnt ? pEnt->MyMonsterPointer() : NULL;
	Vector hit = VR( bestHit );

	if( pMon )
		XBGore_BodyHit( pMon, hit, dir, damage );

	// the limb that was hit takes most of it
	// a little lift, or a limb on the floor just grinds into it
	float speed = ( 150.0f + damage * 10.0f ) * 0.7f;
	Vector push = dir.Normalize() * speed;
	push.z = Q_max( push.z, 0.0f ) + speed * 0.3f;
	RDP_Impulse( &best->phys, bestHit, RV( push ), 10.0f );
	best->sentAsleep = false;
	return true;
}

/*
=============
player contact: legs push limbs aside
=============
*/
static void RD_PlayerContact( xbRagdoll_t *r, CBaseEntity *pl )
{
	Vector org = pl->pev->origin;
	float feet = org.z + pl->pev->mins.z;
	Vector vel = pl->pev->velocity;
	vel.z = 0;
	float speed = vel.Length();
	bool touched = false;
	int touchedP = 0;

	for( int n = 0; n < r->phys.np; n++ )
	{
		rdpVec &p = r->phys.p[n];
		if( p.z < feet - 6.0f || p.z > feet + 30.0f )
			continue;

		float dx = p.x - org.x, dy = p.y - org.y;
		float d = sqrtf( dx * dx + dy * dy );
		float reach = 16.0f + r->phys.radius[n] * 0.5f;
		if( d >= reach || d < 0.01f )
			continue;

		// out of the way, sideways and along with the step
		float out = ( reach - d ) * 0.5f;
		p.x += dx / d * out;
		p.y += dy / d * out;

		float shove = speed > 230.0f ? 0.55f : 0.3f;   // sprinting kicks harder
		rdpVec v( vel.x * shove, vel.y * shove, speed > 50.0f ? 25.0f : 0.0f );
		r->phys.o[n] = r->phys.o[n] - v * RDP_STEP;
		touched = true;
		touchedP = n;
	}

	if( !touched )
		return;

	r->phys.moving = true;
	r->phys.restTime = 0.0f;
	if( r->phys.awake > 2.0f )
		r->phys.awake = 2.0f;   // a little more time to move, then it settles again
	r->sentAsleep = false;

	// stepping through a fresh body squeezes out some blood
	CBaseEntity *pEnt = r->ent;
	CBaseMonster *pMon = pEnt ? pEnt->MyMonsterPointer() : NULL;
	if( pMon && speed > 40.0f && gpGlobals->time - r->died < 60.0f && gpGlobals->time > r->nextSquish )
	{
		r->nextSquish = gpGlobals->time + RANDOM_FLOAT( 0.35f, 0.7f );
		XBGore_BodySquish( pMon, VR( r->phys.p[touchedP] ), vel.Normalize());
	}
}

/*
=============
XBRagdoll_Frame

From StartFrame: step the bodies at 60 Hz, send them at 30 Hz.
=============
*/
void XBRagdoll_Frame( void )
{
	RDP_Trace = RD_SvTrace;

	float dt = gpGlobals->time - s_lastTime;
	s_lastTime = gpGlobals->time;
	if( dt < 0.0f || dt > 0.25f )
		dt = 0.0f;

	CBaseEntity *pl = UTIL_PlayerByIndex( 1 );

	for( int i = 0; i < XB_RD_MAX; i++ )
	{
		xbRagdoll_t *r = &s_rd[i];
		if( !r->used )
			continue;

		// gone (gibbed, removed, a new model in the slot). Not "alive": a
		// monster that just died still counts as alive for a moment, Half-Life
		// hands its corpse some health to be shot at. A respawn sends its own
		// clear (XBRagdoll_Forget from MonsterInit).
		CBaseEntity *pEnt = r->ent;
		if( !pEnt || pEnt->pev->model != r->model || FBitSet( pEnt->pev->effects, EF_NODRAW ) || FBitSet( pEnt->pev->flags, FL_KILLME ))
		{
			r->used = false;
			continue;
		}

		// the death animation never plays: the pose stays the one the body started from
		pEnt->pev->sequence = r->sequence;
		pEnt->pev->frame = r->frame;
		pEnt->pev->framerate = 0.0f;

		if( pl && pl->IsAlive() && pl->pev->movetype != MOVETYPE_NOCLIP )
			RD_PlayerContact( r, pl );

		if( r->phys.moving )
		{
			r->accum += dt;
			while( r->accum >= RDP_STEP && r->phys.moving )
			{
				RDP_Step( &r->phys );
				r->accum -= RDP_STEP;
			}
			if( r->accum > RDP_STEP )
				r->accum = 0.0f;
		}

		bool asleep = !r->phys.moving;
		if(( !asleep && gpGlobals->time >= r->nextSend ) || ( asleep && !r->sentAsleep ))
		{
			RD_SendUpdate( r );
			RD_MoveCorpse( r, pEnt );
			r->nextSend = gpGlobals->time + XB_RD_SEND_RATE;
			r->sentAsleep = asleep;
		}
	}
}

void XBRagdoll_LevelStart( void )
{
	memset( s_rd, 0, sizeof( s_rd ));
	s_lastTime = gpGlobals->time;
}

#endif // XASH_UWP
