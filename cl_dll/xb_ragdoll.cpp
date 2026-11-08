/*
xb_ragdoll.cpp - Xbox build: ragdoll deaths (client side, visual only)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Half-Life version of RealRTCWX's cg_ragdoll.c. When a monster dies the
server sends "XBRagdoll" (dlls/xb_gore.cpp) and the body stops following
its death animation:

  - The skeleton becomes a set of particles with a size each (Verlet),
    held together by links. Humans (Bip01 skeletons) use a hand-made body:
    15 joints, a braced torso, hinge knees and elbows, limited spine, neck,
    shoulders and hips. Anything else (headcrabs, houndeyes, bullsquids...)
    gets a body built from its own bones: a rigid core and limbs whose
    joints only bend so far.
  - Particles have thickness: limbs rest on the floor and on each other
    instead of sinking through.
  - The killing hit throws the body: hardest where it landed, carried
    through the joints, plus whatever speed the monster had. Shots and
    blasts on a body already down push it again.
  - The pose drawn each frame is blended between physics steps, so nothing
    shakes, and bodies go to sleep once still.
  - Purely visual: the server still has the body where it fell.

xb_ragdoll 0 turns ragdolls off.
*/

#include <ctype.h>
#include <string.h>
#include <math.h>
#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "const.h"
#include "com_model.h"
#include "studio.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "pm_defs.h"
#include "event_api.h"

#if XASH_UWP

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RD_MAX         40      // bodies kept in their ragdoll pose
#define RD_MAX_SIM     10      // of those, how many may still be moving
#define RD_MAX_P       24      // particles per body
#define RD_MAX_LINKS   120
#define RD_MAX_PARTS   24

#define RD_STEP        ( 1.0f / 60.0f )
#define RD_ITER        8
#define RD_GRAVITY     640.0f
#define RD_AIR_DRAG    0.998f   // speed kept per step in the air
#define RD_CONTACT_KEEP 0.85f   // speed kept per step by a particle touching the world
#define RD_GROUND_KEEP 0.30f    // tangential speed kept on contact
#define RD_JOINT_FRIC  0.06f    // how much linked particles share speed: joint stiffness
#define RD_SETTLE      0.15f    // units per step below which a body is resting
#define RD_REST_TIME   0.3f
#define RD_MAX_TIME    7.0f

enum { XB_RD_DIED = 0, XB_RD_HIT, XB_RD_CLEAR };

struct rdLink
{
	unsigned char a, b;
	float lo, hi;           // allowed length range
	bool limb;              // shares speed (joint friction)
};

struct rdPart
{
	signed char org, axis, ref;   // ref < 0: carry the twist frame to frame
	float rest[3][3];
	Vector z;
};

struct rdBody_t
{
	bool used, started, moving, human;
	int ent;
	model_t *model;
	int numbones;
	float spawned, wake, lastTime, accum, rest_time;

	// what killed it, applied on the first frame
	Vector hitPos, push, vel;
	bool havePush;

	int np;
	int pBone[RD_MAX_P];
	float radius[RD_MAX_P];
	Vector p[RD_MAX_P], o[RD_MAX_P], prev[RD_MAX_P], safe[RD_MAX_P], restP[RD_MAX_P];
	bool contact[RD_MAX_P];

	int nl;
	rdLink links[RD_MAX_LINKS];

	int nparts;
	rdPart parts[RD_MAX_PARTS];
	signed char boneVert[MAXSTUDIOBONES];   // which particle the bone hangs from
	signed char bonePart[MAXSTUDIOBONES];
	float rest[MAXSTUDIOBONES][3][4];

	// human extras
	float kneeSide;
};

static rdBody_t s_bodies[RD_MAX];
static cvar_t *xb_ragdoll;

/*
================
human skeleton
================
*/
enum
{
	J_PELVIS, J_CHEST, J_HEAD,
	J_LSHOULDER, J_LELBOW, J_LHAND,
	J_RSHOULDER, J_RELBOW, J_RHAND,
	J_LHIP, J_LKNEE, J_LFOOT,
	J_RHIP, J_RKNEE, J_RFOOT,
	J_NUM
};

// bone names per joint, first match wins. Compared without the "Bip01 " prefix,
// case and '_' ignored. Valve's 1998 models use Character Studio's old names
// (L Arm1 = upper arm, L Arm2 = forearm, L Leg = thigh, L Leg1 = calf),
// newer and HD models the current ones.
#define RD_NAMES 5
static const char *s_jointBones[J_NUM][RD_NAMES] =
{
	{ "pelvis", "", NULL },                     // "" = the "Bip01" root itself
	{ "spine2", "spine3", "spine1", "neck", "spine" },
	{ "head", NULL },
	{ "l upperarm", "l arm1", NULL },
	{ "l forearm", "l arm2", NULL },
	{ "l hand", NULL },
	{ "r upperarm", "r arm1", NULL },
	{ "r forearm", "r arm2", NULL },
	{ "r hand", NULL },
	{ "l thigh", "l leg", NULL },
	{ "l calf", "l leg1", NULL },
	{ "l foot", NULL },
	{ "r thigh", "r leg", NULL },
	{ "r calf", "r leg1", NULL },
	{ "r foot", NULL },
};

static const float s_jointRadius[J_NUM] =
{
	8.0f, 9.0f, 5.5f,
	4.5f, 3.5f, 2.5f,
	4.5f, 3.5f, 2.5f,
	5.0f, 4.0f, 3.0f,
	5.0f, 4.0f, 3.0f,
};

// rigid parts: origin joint, axis joint, reference joint for the twist (-1: carried)
enum { S_PELVIS, S_CHEST, S_LUARM, S_LFARM, S_RUARM, S_RFARM, S_LTHIGH, S_LCALF, S_RTHIGH, S_RCALF, S_NUM };
static const int s_humanParts[S_NUM][3] =
{
	{ J_PELVIS,    J_CHEST,  J_LHIP },
	{ J_CHEST,     J_HEAD,   J_LSHOULDER },
	{ J_LSHOULDER, J_LELBOW, -1 },
	{ J_LELBOW,    J_LHAND,  -1 },
	{ J_RSHOULDER, J_RELBOW, -1 },
	{ J_RELBOW,    J_RHAND,  -1 },
	{ J_LHIP,      J_LKNEE,  -1 },
	{ J_LKNEE,     J_LFOOT,  -1 },
	{ J_RHIP,      J_RKNEE,  -1 },
	{ J_RKNEE,     J_RFOOT,  -1 },
};

// which part each joint's bones belong to
static const int s_jointPart[J_NUM] =
{
	S_PELVIS, S_CHEST, S_CHEST,
	S_LUARM, S_LFARM, S_LFARM,
	S_RUARM, S_RFARM, S_RFARM,
	S_LTHIGH, S_LCALF, S_LCALF,
	S_RTHIGH, S_RCALF, S_RCALF,
};

// "Bip01 L Arm1" -> "l arm1"; anything not on a biped -> NULL
static const char *RD_BoneKey( const char *name, char *out, size_t size )
{
	size_t n = 0;

	for( const char *c = name; *c && n + 1 < size; c++ )
		out[n++] = ( *c == '_' ) ? ' ' : (char)tolower( (unsigned char)*c );
	out[n] = 0;

	if( strncmp( out, "bip0", 4 ) || !isdigit( (unsigned char)out[4] ))
		return NULL;

	const char *k = out + 5;
	while( *k == ' ' )
		k++;
	return k;
}

static inline Vector RD_BonePos( const float m[3][4] )
{
	return Vector( m[0][3], m[1][3], m[2][3] );
}

/*
================
building
================
*/
static void RD_Link( rdBody_t *b, int a, int c, bool limb )
{
	if( b->nl >= RD_MAX_LINKS || a == c )
		return;

	float len = ( b->restP[a] - b->restP[c] ).Length();
	rdLink *l = &b->links[b->nl++];
	l->a = a;
	l->b = c;
	l->lo = len;
	l->hi = len;
	l->limb = limb;
}

// a joint at mid between ends a and c may bend until its inside angle is
// minAngle degrees: a range link between the two ends, whichever way it bends
static void RD_BendLimit( rdBody_t *b, int a, int mid, int c, float minAngle )
{
	if( b->nl >= RD_MAX_LINKS )
		return;

	float la = ( b->restP[a] - b->restP[mid] ).Length();
	float lc = ( b->restP[c] - b->restP[mid] ).Length();
	float rad = minAngle * (float)( M_PI / 180.0 );
	float lo = sqrtf( Q_max( 0.0f, la * la + lc * lc - 2.0f * la * lc * cosf( rad )));

	rdLink *l = &b->links[b->nl++];
	l->a = a;
	l->b = c;
	l->lo = lo;
	l->hi = la + lc;
	l->limb = false;
}

static void RD_Basis( const Vector &a, const Vector &axisEnd, const Vector &ref, float m[3][3] )
{
	Vector x = ( axisEnd - a );
	if( x.Length() < 0.001f )
		x = Vector( 1, 0, 0 );
	x = x.Normalize();

	Vector z = CrossProduct( x, ref - a );
	if( z.Length() < 0.001f )
		z = CrossProduct( x, fabs( x.z ) < 0.9f ? Vector( 0, 0, 1 ) : Vector( 1, 0, 0 ));
	z = z.Normalize();

	Vector y = CrossProduct( z, x );

	for( int i = 0; i < 3; i++ )
	{
		m[i][0] = x[i];
		m[i][1] = y[i];
		m[i][2] = z[i];
	}
}

static int RD_AddPart( rdBody_t *b, int org, int axis, int ref )
{
	if( b->nparts >= RD_MAX_PARTS )
		return 0;

	rdPart *pt = &b->parts[b->nparts];
	pt->org = org;
	pt->axis = axis;
	pt->ref = ref;

	Vector refPos;
	if( ref >= 0 )
		refPos = b->restP[ref];
	else
		refPos = b->restP[org] + Vector( 0, 0, 1 ); // any side will do for the start

	RD_Basis( b->restP[org], b->restP[axis], refPos, pt->rest );
	pt->z = Vector( pt->rest[0][2], pt->rest[1][2], pt->rest[2][2] );
	return b->nparts++;
}

static bool RD_BuildHuman( rdBody_t *b, studiohdr_t *hdr )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	char keys[MAXSTUDIOBONES][32];
	const char *key[MAXSTUDIOBONES];
	int jointBone[J_NUM];

	for( int i = 0; i < hdr->numbones && i < MAXSTUDIOBONES; i++ )
		key[i] = RD_BoneKey( pbones[i].name, keys[i], sizeof( keys[i] ));

	for( int j = 0; j < J_NUM; j++ )
	{
		jointBone[j] = -1;
		for( int n = 0; n < RD_NAMES && s_jointBones[j][n] && jointBone[j] < 0; n++ )
		{
			for( int i = 0; i < hdr->numbones; i++ )
			{
				if( !key[i] || strcmp( key[i], s_jointBones[j][n] ))
					continue;

				// one bone can't be two joints
				bool taken = false;
				for( int k = 0; k < j; k++ )
					if( jointBone[k] == i ) taken = true;

				if( !taken )
				{
					jointBone[j] = i;
					break;
				}
			}
		}

		if( jointBone[j] < 0 )
			return false;
	}

	b->human = true;
	b->np = J_NUM;
	for( int j = 0; j < J_NUM; j++ )
	{
		b->pBone[j] = jointBone[j];
		b->radius[j] = s_jointRadius[j];
		b->restP[j] = RD_BonePos( b->rest[jointBone[j]] );
	}

	// skeleton
	RD_Link( b, J_PELVIS, J_CHEST, true );
	RD_Link( b, J_CHEST, J_HEAD, true );
	RD_Link( b, J_CHEST, J_LSHOULDER, true );   RD_Link( b, J_LSHOULDER, J_LELBOW, true );  RD_Link( b, J_LELBOW, J_LHAND, true );
	RD_Link( b, J_CHEST, J_RSHOULDER, true );   RD_Link( b, J_RSHOULDER, J_RELBOW, true );  RD_Link( b, J_RELBOW, J_RHAND, true );
	RD_Link( b, J_PELVIS, J_LHIP, true );       RD_Link( b, J_LHIP, J_LKNEE, true );        RD_Link( b, J_LKNEE, J_LFOOT, true );
	RD_Link( b, J_PELVIS, J_RHIP, true );       RD_Link( b, J_RHIP, J_RKNEE, true );        RD_Link( b, J_RKNEE, J_RFOOT, true );

	// braces: the torso is a solid block
	RD_Link( b, J_LSHOULDER, J_RSHOULDER, false );  RD_Link( b, J_LHIP, J_RHIP, false );
	RD_Link( b, J_LSHOULDER, J_PELVIS, false );     RD_Link( b, J_RSHOULDER, J_PELVIS, false );
	RD_Link( b, J_LHIP, J_CHEST, false );           RD_Link( b, J_RHIP, J_CHEST, false );
	RD_Link( b, J_LHIP, J_RSHOULDER, false );       RD_Link( b, J_RHIP, J_LSHOULDER, false );
	RD_Link( b, J_HEAD, J_LSHOULDER, false );       RD_Link( b, J_HEAD, J_RSHOULDER, false );

	// how far each joint bends
	RD_BendLimit( b, J_PELVIS, J_CHEST, J_HEAD, 140.0f );        // spine + neck: about 40 degrees
	RD_BendLimit( b, J_LSHOULDER, J_LELBOW, J_LHAND, 35.0f );    // elbows: up to 145
	RD_BendLimit( b, J_RSHOULDER, J_RELBOW, J_RHAND, 35.0f );
	RD_BendLimit( b, J_LHIP, J_LKNEE, J_LFOOT, 35.0f );          // knees: up to 145
	RD_BendLimit( b, J_RHIP, J_RKNEE, J_RFOOT, 35.0f );
	RD_BendLimit( b, J_CHEST, J_LSHOULDER, J_LELBOW, 25.0f );    // an arm can't fold into the chest
	RD_BendLimit( b, J_CHEST, J_RSHOULDER, J_RELBOW, 25.0f );
	RD_BendLimit( b, J_RHIP, J_LHIP, J_LKNEE, 40.0f );           // legs can't cross far through each other
	RD_BendLimit( b, J_LHIP, J_RHIP, J_RKNEE, 40.0f );

	for( int s = 0; s < S_NUM; s++ )
		RD_AddPart( b, s_humanParts[s][0], s_humanParts[s][1], s_humanParts[s][2] );

	// every bone moves with the part of its nearest joint ancestor
	for( int i = 0; i < hdr->numbones; i++ )
	{
		int part = S_PELVIS, vert = J_PELVIS;

		for( int k = i; k >= 0; k = pbones[k].parent )
		{
			int j;
			for( j = 0; j < J_NUM; j++ )
				if( jointBone[j] == k ) break;

			if( j < J_NUM )
			{
				part = s_jointPart[j];
				vert = j;
				break;
			}
		}
		b->bonePart[i] = part;
		b->boneVert[i] = vert;
	}

	return true;
}

/*
================
any other skeleton: built from its own bones
================
*/
static bool RD_BuildGeneric( rdBody_t *b, studiohdr_t *hdr )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	int boneP[MAXSTUDIOBONES];   // particle of the bone itself, or -1
	int parentP[RD_MAX_P];

	b->human = false;
	b->np = 0;
	b->nl = 0;
	b->nparts = 0;

	// a particle for every bone that sits apart from its parent; bones on top of
	// their parent (twist helpers, roots) share the parent's particle
	for( int i = 0; i < hdr->numbones; i++ )
	{
		boneP[i] = -1;
		int par = pbones[i].parent;
		Vector pos = RD_BonePos( b->rest[i] );

		if( par >= 0 )
		{
			int pp = -1;
			for( int k = par; k >= 0 && pp < 0; k = pbones[k].parent )
				pp = boneP[k];

			if( pp >= 0 && ( pos - b->restP[pp] ).Length() < 2.5f )
				continue;
		}

		if( b->np >= RD_MAX_P )
			continue;

		int n = b->np++;
		boneP[i] = n;
		b->pBone[n] = i;
		b->restP[n] = pos;

		parentP[n] = -1;
		for( int k = par; k >= 0 && parentP[n] < 0; k = pbones[k].parent )
			parentP[n] = boneP[k];
	}

	if( b->np < 2 )
		return false;

	// the creature's size sets particle sizes and the core
	Vector mins = b->restP[0], maxs = b->restP[0];
	for( int n = 1; n < b->np; n++ )
	{
		for( int a = 0; a < 3; a++ )
		{
			mins[a] = Q_min( mins[a], b->restP[n][a] );
			maxs[a] = Q_max( maxs[a], b->restP[n][a] );
		}
	}
	float extent = ( maxs - mins ).Length();
	float coreRadius = Q_max( 6.0f, extent * 0.22f );

	// the core: the root and everything close to it is one rigid body
	bool core[RD_MAX_P];
	for( int n = 0; n < b->np; n++ )
	{
		core[n] = ( n == 0 ) || ( b->restP[n] - b->restP[0] ).Length() <= coreRadius;
		b->radius[n] = core[n] ? Q_max( 2.5f, extent * 0.08f ) : Q_max( 1.5f, extent * 0.04f );
	}

	for( int n = 0; n < b->np; n++ )
	{
		if( parentP[n] >= 0 )
			RD_Link( b, parentP[n], n, true );

		// a joint bends at most to 40 degrees inside
		if( parentP[n] >= 0 && parentP[parentP[n]] >= 0 && !core[n] )
			RD_BendLimit( b, parentP[parentP[n]], parentP[n], n, 40.0f );
	}

	for( int a = 0; a < b->np; a++ )
		for( int c = a + 1; c < b->np; c++ )
			if( core[a] && core[c] && parentP[c] != a && parentP[a] != c )
				RD_Link( b, a, c, false );

	// parts: the core moves as one, every other particle turns with its own bone
	int coreRef1 = -1, coreRef2 = -1;
	for( int n = 1; n < b->np; n++ )
	{
		if( !core[n] ) continue;
		if( coreRef1 < 0 ) { coreRef1 = n; continue; }
		Vector d1 = ( b->restP[coreRef1] - b->restP[0] ).Normalize();
		Vector d2 = ( b->restP[n] - b->restP[0] ).Normalize();
		if( CrossProduct( d1, d2 ).Length() > 0.3f ) { coreRef2 = n; break; }
	}

	int partOf[RD_MAX_P];
	int corePart;
	if( coreRef1 >= 0 )
	{
		corePart = RD_AddPart( b, 0, coreRef1, coreRef2 );
	}
	else
	{
		// no core to speak of: the root turns with its first child
		int child = -1;
		for( int n = 1; n < b->np && child < 0; n++ )
			if( parentP[n] == 0 ) child = n;
		corePart = RD_AddPart( b, 0, child >= 0 ? child : 1, -1 );
	}

	for( int n = 0; n < b->np; n++ )
	{
		if( core[n] )
		{
			partOf[n] = corePart;
			continue;
		}

		int child = -1;
		for( int c = 0; c < b->np && child < 0; c++ )
			if( parentP[c] == n ) child = c;

		if( child >= 0 )
			partOf[n] = RD_AddPart( b, n, child, -1 );
		else if( parentP[n] >= 0 )
			partOf[n] = RD_AddPart( b, parentP[n], n, -1 ); // a tip follows the bone leading to it
		else
			partOf[n] = corePart;
	}

	for( int i = 0; i < hdr->numbones; i++ )
	{
		int v = 0;
		for( int k = i; k >= 0; k = pbones[k].parent )
		{
			if( boneP[k] >= 0 ) { v = boneP[k]; break; }
		}
		b->boneVert[i] = v;
		b->bonePart[i] = partOf[v];
	}

	return true;
}

/*
================
simulation
================
*/
static Vector RD_Forward( const Vector *p )
{
	Vector up = p[J_CHEST] - p[J_PELVIS];
	Vector right = p[J_RHIP] - p[J_LHIP];
	Vector f = CrossProduct( up, right );
	return f.Length() > 0.001f ? f.Normalize() : Vector( 1, 0, 0 );
}

static Vector RD_JointOffset( const Vector *p, int a, int mid, int c )
{
	Vector line = p[c] - p[a];
	float len2 = DotProduct( line, line );
	if( len2 < 0.001f )
		return Vector( 0, 0, 0 );
	float t = DotProduct( p[mid] - p[a], line ) / len2;
	return p[mid] - ( p[a] + line * t );
}

static void RD_Throw( rdBody_t *b, const Vector &hit, const Vector &push, const Vector &vel )
{
	// most of the push where it hit, passed on through the body with distance
	for( int n = 0; n < b->np; n++ )
	{
		float d = ( b->p[n] - hit ).Length();
		float w = 0.3f + 0.9f * expf( -d / 14.0f );
		Vector v = vel + push * w;
		b->o[n] = b->p[n] - v * RD_STEP;
	}
}

static void RD_Collide( rdBody_t *b, int n )
{
	pmtrace_t tr;
	Vector from = b->safe[n], to = b->p[n];

	b->contact[n] = false;

	if(( to - from ).Length() > 0.01f )
	{
		gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
		gEngfuncs.pEventAPI->EV_PlayerTrace( from, to, PM_WORLD_ONLY, -1, &tr );

		if( tr.allsolid )
		{
			b->p[n] = b->o[n] = from;
			return;
		}

		if( tr.fraction < 1.0f )
		{
			Vector nrm = Vector( tr.plane.normal );
			Vector hit = Vector( tr.endpos ) + nrm * 0.25f;
			Vector v = b->p[n] - b->o[n];
			Vector vt = v - nrm * DotProduct( v, nrm );

			b->p[n] = hit;
			b->o[n] = hit - vt * RD_GROUND_KEEP;
			b->contact[n] = true;
		}
	}

	// keep the particle's own thickness off whatever is under it
	if( b->radius[n] > 0.5f )
	{
		Vector down = b->p[n] - Vector( 0.0f, 0.0f, b->radius[n] );
		gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
		gEngfuncs.pEventAPI->EV_PlayerTrace( b->p[n], down, PM_WORLD_ONLY, -1, &tr );
		if( !tr.startsolid && tr.fraction < 1.0f )
		{
			float lift = b->radius[n] * ( 1.0f - tr.fraction );
			b->p[n].z += lift;
			b->o[n].z += lift;   // resting, not bouncing
			Vector v = b->p[n] - b->o[n];
			b->o[n].x = b->p[n].x - v.x * RD_GROUND_KEEP;
			b->o[n].y = b->p[n].y - v.y * RD_GROUND_KEEP;
			b->contact[n] = true;
		}
	}

	b->safe[n] = b->p[n];
}

static void RD_Constrain( rdBody_t *b )
{
	for( int it = 0; it < RD_ITER; it++ )
	{
		for( int i = 0; i < b->nl; i++ )
		{
			rdLink *l = &b->links[i];
			Vector &a = b->p[l->a];
			Vector &c = b->p[l->b];
			Vector d = c - a;
			float len = d.Length();

			if( len < 0.001f || ( len >= l->lo && len <= l->hi ))
				continue;

			float target = len < l->lo ? l->lo : l->hi;
			Vector corr = d * ( 0.5f * ( len - target ) / len );
			a = a + corr;
			c = c - corr;
		}

		// limbs keep out of each other and the torso
		for( int i = 0; i < b->np; i++ )
		{
			for( int j = i + 1; j < b->np; j++ )
			{
				float r = ( b->radius[i] + b->radius[j] ) * 0.8f;
				Vector d = b->p[j] - b->p[i];
				float len = d.Length();
				float rest = ( b->restP[j] - b->restP[i] ).Length();

				// neighbours that start out overlapping are allowed to
				if( len >= r || rest < r || len < 0.001f )
					continue;

				Vector corr = d * ( 0.25f * ( r - len ) / len );
				b->p[i] = b->p[i] - corr;
				b->p[j] = b->p[j] + corr;
			}
		}

		if( b->human )
		{
			Vector fwd = RD_Forward( b->p );
			Vector up = ( b->p[J_CHEST] - b->p[J_PELVIS] ).Normalize();

			// knees bend the way they did at death (forward if straight)
			static const int legs[2][3] = { { J_LHIP, J_LKNEE, J_LFOOT }, { J_RHIP, J_RKNEE, J_RFOOT } };
			for( int k = 0; k < 2; k++ )
			{
				Vector off = RD_JointOffset( b->p, legs[k][0], legs[k][1], legs[k][2] );
				float side = DotProduct( off, fwd ) * b->kneeSide;
				// ease it back to straight, never flip it: a flip adds energy
				if( side < 0.0f )
					b->p[legs[k][1]] = b->p[legs[k][1]] - fwd * ( 0.5f * side * b->kneeSide );
			}

			// elbows point back while the arm hangs
			static const int arms[2][3] = { { J_LSHOULDER, J_LELBOW, J_LHAND }, { J_RSHOULDER, J_RELBOW, J_RHAND } };
			for( int k = 0; k < 2; k++ )
			{
				Vector upper = ( b->p[arms[k][1]] - b->p[arms[k][0]] ).Normalize();
				if( DotProduct( upper, up ) > -0.3f )
					continue;
				Vector off = RD_JointOffset( b->p, arms[k][0], arms[k][1], arms[k][2] );
				float side = DotProduct( off, fwd );
				if( side > 0.0f )
					b->p[arms[k][1]] = b->p[arms[k][1]] - fwd * ( 0.5f * side );
			}

			// legs swing forward freely, only a little backward
			static const int hips[2][2] = { { J_LHIP, J_LKNEE }, { J_RHIP, J_RKNEE } };
			for( int k = 0; k < 2; k++ )
			{
				Vector thigh = b->p[hips[k][1]] - b->p[hips[k][0]];
				float len = thigh.Length();
				if( len < 0.001f )
					continue;
				Vector dir = thigh / len;
				float back = -DotProduct( dir, fwd );
				// past ~35 degrees behind the body
				if( back > 0.57f )
				{
					Vector fix = dir + fwd * ( 0.5f * ( back - 0.57f ));
					b->p[hips[k][1]] = b->p[hips[k][0]] + fix.Normalize() * len;
				}
			}
		}
	}
}

static void RD_Step( rdBody_t *b )
{
	float moved = 0.0f;

	for( int n = 0; n < b->np; n++ )
		b->prev[n] = b->p[n];

	// joints are a little stiff: linked particles share some of their speed
	for( int i = 0; i < b->nl; i++ )
	{
		rdLink *l = &b->links[i];
		if( !l->limb )
			continue;
		Vector va = b->p[l->a] - b->o[l->a];
		Vector vc = b->p[l->b] - b->o[l->b];
		Vector dv = ( vc - va ) * RD_JOINT_FRIC;
		b->o[l->a] = b->o[l->a] - dv;
		b->o[l->b] = b->o[l->b] + dv;
	}

	// once it has landed, the body winds down instead of fidgeting forever
	float since = gEngfuncs.GetClientTime() - b->wake;
	float settle = since < 1.5f ? 0.0f : Q_min(( since - 1.5f ) / 2.0f, 1.0f );

	for( int n = 0; n < b->np; n++ )
	{
		Vector cur = b->p[n];
		float keep = b->contact[n] ? RD_CONTACT_KEEP : RD_AIR_DRAG;
		keep *= 1.0f - 0.25f * settle;
		Vector v = ( b->p[n] - b->o[n] ) * keep;

		b->o[n] = cur;
		b->p[n] = cur + v + Vector( 0.0f, 0.0f, -RD_GRAVITY * RD_STEP * RD_STEP );
	}

	// hold together, then settle on the world: the world has the last word
	RD_Constrain( b );
	for( int n = 0; n < b->np; n++ )
		RD_Collide( b, n );

	for( int n = 0; n < b->np; n++ )
		moved = Q_max( moved, ( b->p[n] - b->o[n] ).Length());

	if( moved < RD_SETTLE )
		b->rest_time += RD_STEP;
	else b->rest_time = 0.0f;

	if( b->rest_time > RD_REST_TIME || gEngfuncs.GetClientTime() - b->wake > RD_MAX_TIME )
	{
		b->moving = false;
		for( int n = 0; n < b->np; n++ )
			b->prev[n] = b->p[n];
	}
}

/*
================
messages
================
*/
static rdBody_t *RD_Find( int ent )
{
	for( int i = 0; i < RD_MAX; i++ )
	{
		if( s_bodies[i].used && s_bodies[i].ent == ent )
			return &s_bodies[i];
	}
	return NULL;
}

static Vector RD_ReadVec( void )
{
	Vector v;
	v.x = READ_COORD();
	v.y = READ_COORD();
	v.z = READ_COORD();
	return v;
}

static int MsgFunc_XBRagdoll( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	int ent = READ_SHORT();
	int kind = READ_BYTE();

	if( kind == XB_RD_CLEAR )
	{
		rdBody_t *old = RD_Find( ent );
		if( old )
			old->used = false;
		return 1;
	}

	Vector hit = RD_ReadVec();
	Vector push = RD_ReadVec();

	if( kind == XB_RD_HIT )
	{
		rdBody_t *b = RD_Find( ent );
		if( !b || !xb_ragdoll || !xb_ragdoll->value )
			return 1;

		if( !b->started )
		{
			// hit again before it was drawn: add to the death push
			b->push = b->push + push;
			return 1;
		}

		// shot while down: wake it and shove it
		for( int n = 0; n < b->np; n++ )
		{
			float d = ( b->p[n] - hit ).Length();
			float w = 0.15f + 0.9f * expf( -d / 12.0f );
			b->o[n] = b->o[n] - push * ( w * RD_STEP );
			b->prev[n] = b->p[n];
		}
		b->moving = true;
		b->rest_time = 0.0f;
		b->accum = 0.0f;
		b->wake = gEngfuncs.GetClientTime();
		b->lastTime = b->wake;
		return 1;
	}

	Vector vel = RD_ReadVec();
	READ_BYTE(); // hitgroup: the hit position says more

	if( !xb_ragdoll || !xb_ragdoll->value )
		return 1;

	gEngfuncs.Con_DPrintf( "ragdoll: ent %d died (push %.0f)\n", ent, push.Length());

	rdBody_t *b = RD_Find( ent );
	if( !b )
	{
		// free slot, else the oldest body
		b = &s_bodies[0];
		for( int i = 0; i < RD_MAX; i++ )
		{
			if( !s_bodies[i].used ) { b = &s_bodies[i]; break; }
			if( s_bodies[i].spawned < b->spawned ) b = &s_bodies[i];
		}
	}

	memset( b, 0, sizeof( *b ));
	b->used = true;
	b->ent = ent;
	b->spawned = b->wake = gEngfuncs.GetClientTime();
	b->hitPos = hit;
	b->push = push;
	b->vel = vel;
	b->havePush = true;
	return 1;
}

void XBRagdoll_Init( void )
{
	xb_ragdoll = gEngfuncs.pfnRegisterVariable( "xb_ragdoll", "1", FCVAR_ARCHIVE );
	gEngfuncs.pfnHookUserMsg( "XBRagdoll", MsgFunc_XBRagdoll );
	memset( s_bodies, 0, sizeof( s_bodies ));
}

void XBRagdoll_Reset( void )
{
	memset( s_bodies, 0, sizeof( s_bodies ));
}

/*
================
start
================
*/
static bool RD_Start( rdBody_t *b, cl_entity_t *ent, studiohdr_t *hdr, float (*bones)[MAXSTUDIOBONES][3][4] )
{
	b->numbones = hdr->numbones;
	memcpy( b->rest, *bones, sizeof( float ) * 12 * hdr->numbones );

	if( !RD_BuildHuman( b, hdr ) && !RD_BuildGeneric( b, hdr ))
	{
		gEngfuncs.Con_DPrintf( "ragdoll: ent %d (%s) has no usable skeleton\n", ent->index, ent->model ? ent->model->name : "?" );
		return false;
	}

	for( int n = 0; n < b->np; n++ )
	{
		b->p[n] = b->prev[n] = b->safe[n] = b->restP[n];
		b->o[n] = b->p[n];
	}

	if( b->human )
	{
		Vector fwd = RD_Forward( b->restP );
		float side = DotProduct( RD_JointOffset( b->restP, J_LHIP, J_LKNEE, J_LFOOT ), fwd )
			+ DotProduct( RD_JointOffset( b->restP, J_RHIP, J_RKNEE, J_RFOOT ), fwd );
		b->kneeSide = side < -0.5f ? -1.0f : 1.0f;
	}

	if( b->havePush )
		RD_Throw( b, b->hitPos, b->push, b->vel );

	gEngfuncs.Con_DPrintf( "ragdoll: ent %d (%s) started, %s, %d particles %d links\n", ent->index,
		ent->model ? ent->model->name : "?", b->human ? "human" : "generic", b->np, b->nl );

	b->model = ent->model;
	b->started = true;
	b->moving = true;
	b->lastTime = gEngfuncs.GetClientTime();
	return true;
}

/*
=============
XBRagdoll_Apply

Called at the end of CStudioModelRenderer::StudioSetupBones for every
studio entity. Replaces the bone transforms of ragdolled bodies.
=============
*/
void XBRagdoll_Apply( cl_entity_t *ent, void *studiohdr, float (*bones)[MAXSTUDIOBONES][3][4], float (*lights)[MAXSTUDIOBONES][3][4] )
{
	studiohdr_t *hdr = (studiohdr_t *)studiohdr;

	if( !ent || !hdr || !xb_ragdoll || !xb_ragdoll->value )
		return;

	rdBody_t *b = RD_Find( ent->index );
	if( !b )
		return;

	// slot taken over by something else (new monster, revived by a script)
	if( b->started && ( ent->model != b->model || hdr->numbones != b->numbones ))
	{
		b->used = false;
		return;
	}

	if( hdr->numbones > MAXSTUDIOBONES )
	{
		b->used = false;
		return;
	}

	if( !b->started )
	{
		// past the limit the oldest moving body freezes
		int moving = 0;
		rdBody_t *oldest = NULL;

		for( int i = 0; i < RD_MAX; i++ )
		{
			if( s_bodies[i].used && s_bodies[i].moving )
			{
				moving++;
				if( !oldest || s_bodies[i].wake < oldest->wake )
					oldest = &s_bodies[i];
			}
		}
		if( moving >= RD_MAX_SIM && oldest )
			oldest->moving = false;

		if( !RD_Start( b, ent, hdr, bones ))
		{
			b->used = false;
			return;
		}
	}

	float blend = 1.0f;

	if( b->moving )
	{
		float now = gEngfuncs.GetClientTime();
		float dt = now - b->lastTime;

		b->lastTime = now;
		b->accum += dt < 0.0f ? 0.0f : ( dt > 0.1f ? 0.1f : dt );

		if( b->accum >= RD_STEP )
		{
			gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );
			gEngfuncs.pEventAPI->EV_PushPMStates();
			gEngfuncs.pEventAPI->EV_SetSolidPlayers( -1 );

			while( b->accum >= RD_STEP && b->moving )
			{
				RD_Step( b );
				b->accum -= RD_STEP;
			}

			gEngfuncs.pEventAPI->EV_PopPMStates();
		}

		// draw between the last two steps so motion is smooth at any frame rate
		blend = b->moving ? b->accum / RD_STEP : 1.0f;
	}

	Vector pos[RD_MAX_P];
	for( int n = 0; n < b->np; n++ )
		pos[n] = b->prev[n] + ( b->p[n] - b->prev[n] ) * blend;

	// rotation of every part from the death pose to now
	float rot[RD_MAX_PARTS][3][3];
	for( int s = 0; s < b->nparts; s++ )
	{
		rdPart *pt = &b->parts[s];
		float nowM[3][3];

		if( pt->ref >= 0 )
		{
			RD_Basis( pos[pt->org], pos[pt->axis], pos[pt->ref], nowM );
		}
		else
		{
			// keep last frame's twist, only turned onto the new bone axis,
			// so a limb lining up with its reference can't flip over
			Vector x = pos[pt->axis] - pos[pt->org];
			if( x.Length() < 0.001f )
				x = Vector( pt->rest[0][0], pt->rest[1][0], pt->rest[2][0] );
			x = x.Normalize();

			Vector z = pt->z - x * DotProduct( pt->z, x );
			if( z.Length() < 0.01f )
				z = CrossProduct( x, fabs( x.z ) < 0.9f ? Vector( 0, 0, 1 ) : Vector( 1, 0, 0 ));
			z = z.Normalize();
			pt->z = z;

			Vector y = CrossProduct( z, x );
			for( int i = 0; i < 3; i++ )
			{
				nowM[i][0] = x[i];
				nowM[i][1] = y[i];
				nowM[i][2] = z[i];
			}
		}

		// rot = now * rest^T
		for( int i = 0; i < 3; i++ )
			for( int k = 0; k < 3; k++ )
				rot[s][i][k] = nowM[i][0] * pt->rest[k][0] + nowM[i][1] * pt->rest[k][1] + nowM[i][2] * pt->rest[k][2];
	}

	for( int i = 0; i < b->numbones; i++ )
	{
		int s = b->bonePart[i];
		int org = b->parts[s].org;
		const float ( *R )[3] = rot[s];
		const float ( *M )[4] = b->rest[i];
		Vector rel = RD_BonePos( M ) - b->restP[org];
		Vector now = pos[org];
		float out[3][4];

		for( int r = 0; r < 3; r++ )
		{
			for( int c = 0; c < 3; c++ )
				out[r][c] = R[r][0] * M[0][c] + R[r][1] * M[1][c] + R[r][2] * M[2][c];

			out[r][3] = R[r][0] * rel.x + R[r][1] * rel.y + R[r][2] * rel.z + now[r];
		}

		memcpy( (*bones)[i], out, sizeof( out ));
		if( lights )
			memcpy( (*lights)[i], out, sizeof( out ));
	}
}

/*
=============
XBRagdoll_BodyPelvis

Where a ragdolled body's middle is, for the blood pool under it.
=============
*/
bool XBRagdoll_BodyPelvis( int ent, float *out, bool *settled )
{
	rdBody_t *b = RD_Find( ent );

	if( !b || !b->started )
		return false;

	out[0] = b->p[0].x;
	out[1] = b->p[0].y;
	out[2] = b->p[0].z;
	*settled = !b->moving;
	return true;
}

#endif // XASH_UWP
