/*
xb_ragdoll.cpp - Xbox build: ragdoll deaths (client side, visual only)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Half-Life version of RealRTCWX's cg_ragdoll.c. When a monster with a
human skeleton dies, the server sends "XBRagdoll" (dlls/xb_gore.cpp) and
the body stops following its death animation:

  - 15 joints (pelvis, chest, head, shoulders, elbows, hands, hips, knees,
    feet) are read from the Bip01 skeleton every Half-Life human model
    shares, and simulated as Verlet particles held together by distance
    constraints, with braces that keep the torso rigid.
  - The body is split into rigid parts (pelvis, chest+head, upper arms,
    forearms+hands, thighs, calves+feet). Each frame every part's rotation
    is worked out from its joints and applied to every bone of that part,
    starting from the pose at the moment of death.
  - Purely visual: the server still has the body where it fell.

Bodies that settle stop simulating. xb_ragdoll 0 turns ragdolls off.
Monsters without the Bip01 joints (aliens, headcrabs...) keep their
normal death animation.
*/

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

// dlls/monsters.h
#define HITGROUP_HEAD      1
#define HITGROUP_STOMACH   3
#define HITGROUP_LEFTARM   4
#define HITGROUP_RIGHTARM  5
#define HITGROUP_LEFTLEG   6
#define HITGROUP_RIGHTLEG  7

#define RD_MAX        48      // bodies kept in their ragdoll pose
#define RD_MAX_SIM    12      // of those, how many may still be moving
#define RD_STEP       ( 1.0f / 45.0f )
#define RD_ITER       6
#define RD_GRAVITY    600.0f
#define RD_DRAG       0.985f
#define RD_FRICTION   0.55f   // tangential speed kept on contact
#define RD_SETTLE     0.08f   // units per step below which a body is resting

enum
{
	J_PELVIS, J_CHEST, J_HEAD,
	J_LSHOULDER, J_LELBOW, J_LHAND,
	J_RSHOULDER, J_RELBOW, J_RHAND,
	J_LHIP, J_LKNEE, J_LFOOT,
	J_RHIP, J_RKNEE, J_RFOOT,
	J_NUM
};

// bone names per joint, first match wins
static const char *s_jointBones[J_NUM][3] =
{
	{ "Bip01 Pelvis", NULL, NULL },
	{ "Bip01 Spine2", "Bip01 Spine3", "Bip01 Spine1" },
	{ "Bip01 Head", NULL, NULL },
	{ "Bip01 L UpperArm", NULL, NULL },
	{ "Bip01 L Forearm", NULL, NULL },
	{ "Bip01 L Hand", NULL, NULL },
	{ "Bip01 R UpperArm", NULL, NULL },
	{ "Bip01 R Forearm", NULL, NULL },
	{ "Bip01 R Hand", NULL, NULL },
	{ "Bip01 L Thigh", NULL, NULL },
	{ "Bip01 L Calf", NULL, NULL },
	{ "Bip01 L Foot", NULL, NULL },
	{ "Bip01 R Thigh", NULL, NULL },
	{ "Bip01 R Calf", NULL, NULL },
	{ "Bip01 R Foot", NULL, NULL },
};

// rigid parts: origin joint, axis joint, reference joint for the twist
enum { S_PELVIS, S_CHEST, S_LUARM, S_LFARM, S_RUARM, S_RFARM, S_LTHIGH, S_LCALF, S_RTHIGH, S_RCALF, S_NUM };
static const int s_segs[S_NUM][3] =
{
	{ J_PELVIS,    J_CHEST,  J_LHIP },
	{ J_CHEST,     J_HEAD,   J_LSHOULDER },
	{ J_LSHOULDER, J_LELBOW, J_CHEST },
	{ J_LELBOW,    J_LHAND,  J_CHEST },
	{ J_RSHOULDER, J_RELBOW, J_CHEST },
	{ J_RELBOW,    J_RHAND,  J_CHEST },
	{ J_LHIP,      J_LKNEE,  J_PELVIS },
	{ J_LKNEE,     J_LFOOT,  J_PELVIS },
	{ J_RHIP,      J_RKNEE,  J_PELVIS },
	{ J_RKNEE,     J_RFOOT,  J_PELVIS },
};

// which part each joint's bones belong to
static const int s_jointSeg[J_NUM] =
{
	S_PELVIS, S_CHEST, S_CHEST,
	S_LUARM, S_LFARM, S_LFARM,
	S_RUARM, S_RFARM, S_RFARM,
	S_LTHIGH, S_LCALF, S_LCALF,
	S_RTHIGH, S_RCALF, S_RCALF,
};

// distance constraints: the skeleton plus braces that keep the torso a solid block
static const int s_links[][2] =
{
	{ J_PELVIS, J_CHEST }, { J_CHEST, J_HEAD },
	{ J_CHEST, J_LSHOULDER }, { J_LSHOULDER, J_LELBOW }, { J_LELBOW, J_LHAND },
	{ J_CHEST, J_RSHOULDER }, { J_RSHOULDER, J_RELBOW }, { J_RELBOW, J_RHAND },
	{ J_PELVIS, J_LHIP }, { J_LHIP, J_LKNEE }, { J_LKNEE, J_LFOOT },
	{ J_PELVIS, J_RHIP }, { J_RHIP, J_RKNEE }, { J_RKNEE, J_RFOOT },
	// braces
	{ J_LSHOULDER, J_RSHOULDER }, { J_LHIP, J_RHIP },
	{ J_LSHOULDER, J_PELVIS }, { J_RSHOULDER, J_PELVIS },
	{ J_LHIP, J_CHEST }, { J_RHIP, J_CHEST },
	{ J_HEAD, J_LSHOULDER }, { J_HEAD, J_RSHOULDER },
	{ J_LHIP, J_RSHOULDER }, { J_RHIP, J_LSHOULDER },
	// keep limbs from folding flat
	{ J_LSHOULDER, J_LHAND }, { J_RSHOULDER, J_RHAND },
	{ J_LHIP, J_LFOOT }, { J_RHIP, J_RFOOT },
};
#define RD_NUM_LINKS ( sizeof( s_links ) / sizeof( s_links[0] ))
// limb "reach" links may shorten (bend) but never stretch past the rest length
#define RD_FIRST_REACH ( RD_NUM_LINKS - 4 )

struct rdBody_t
{
	bool used, started, moving;
	int ent;
	model_t *model;
	int sequence;
	float spawned, lastTime, accum, rest_time;
	Vector impulse;
	int hitJoint;

	int numbones;
	int jointBone[J_NUM];
	signed char boneSeg[MAXSTUDIOBONES];
	float rest[MAXSTUDIOBONES][3][4];
	Vector restJoint[J_NUM];
	float linkLen[RD_NUM_LINKS];

	Vector p[J_NUM], o[J_NUM];
};

static rdBody_t s_bodies[RD_MAX];
static cvar_t *xb_ragdoll;

static rdBody_t *RD_Find( int ent )
{
	for( int i = 0; i < RD_MAX; i++ )
	{
		if( s_bodies[i].used && s_bodies[i].ent == ent )
			return &s_bodies[i];
	}
	return NULL;
}

static int MsgFunc_XBRagdoll( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	int ent = READ_SHORT();
	Vector dir;
	dir.x = READ_COORD();
	dir.y = READ_COORD();
	dir.z = READ_COORD();
	int force = READ_BYTE();
	int hitgroup = READ_BYTE();

	if( !xb_ragdoll || !xb_ragdoll->value )
		return 1;

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
	b->spawned = gEngfuncs.GetClientTime();
	b->impulse = dir.Normalize() * (float)force;

	switch( hitgroup )
	{
	case HITGROUP_HEAD:     b->hitJoint = J_HEAD; break;
	case HITGROUP_STOMACH:  b->hitJoint = J_PELVIS; break;
	case HITGROUP_LEFTARM:  b->hitJoint = J_LELBOW; break;
	case HITGROUP_RIGHTARM: b->hitJoint = J_RELBOW; break;
	case HITGROUP_LEFTLEG:  b->hitJoint = J_LKNEE; break;
	case HITGROUP_RIGHTLEG: b->hitJoint = J_RKNEE; break;
	default:                b->hitJoint = J_CHEST; break;
	}

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

static inline Vector RD_BonePos( const float m[3][4] )
{
	return Vector( m[0][3], m[1][3], m[2][3] );
}

static bool RD_Start( rdBody_t *b, cl_entity_t *ent, studiohdr_t *hdr, float (*bones)[MAXSTUDIOBONES][3][4] )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	b->numbones = hdr->numbones;

	for( int j = 0; j < J_NUM; j++ )
	{
		b->jointBone[j] = -1;
		for( int n = 0; n < 3 && s_jointBones[j][n] && b->jointBone[j] < 0; n++ )
		{
			for( int i = 0; i < hdr->numbones; i++ )
			{
				if( !_stricmp( pbones[i].name, s_jointBones[j][n] ))
				{
					b->jointBone[j] = i;
					break;
				}
			}
		}

		if( b->jointBone[j] < 0 )
			return false; // not a human skeleton
	}

	// every bone moves with the part of its nearest joint ancestor
	for( int i = 0; i < hdr->numbones; i++ )
	{
		int seg = S_PELVIS;

		for( int k = i; k >= 0; k = pbones[k].parent )
		{
			int j;
			for( j = 0; j < J_NUM; j++ )
				if( b->jointBone[j] == k ) break;

			if( j < J_NUM )
			{
				seg = s_jointSeg[j];
				break;
			}
		}
		b->boneSeg[i] = seg;
	}

	memcpy( b->rest, *bones, sizeof( float ) * 12 * hdr->numbones );

	float now = gEngfuncs.GetClientTime();

	for( int j = 0; j < J_NUM; j++ )
	{
		b->restJoint[j] = RD_BonePos( b->rest[b->jointBone[j]] );
		b->p[j] = b->restJoint[j];

		// the hit throws the body: hardest where it was hit, some everywhere
		float share = ( j == b->hitJoint ) ? 1.0f : 0.45f;
		b->o[j] = b->p[j] - b->impulse * share * RD_STEP;
	}

	for( unsigned l = 0; l < RD_NUM_LINKS; l++ )
		b->linkLen[l] = ( b->restJoint[s_links[l][0]] - b->restJoint[s_links[l][1]] ).Length();

	b->model = ent->model;
	b->sequence = ent->curstate.sequence;
	b->started = true;
	b->moving = true;
	b->lastTime = now;
	return true;
}

static void RD_Collide( rdBody_t *b, int j )
{
	pmtrace_t tr;
	Vector from = b->o[j], to = b->p[j];

	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
	gEngfuncs.pEventAPI->EV_PlayerTrace( from, to, PM_WORLD_ONLY, -1, &tr );

	if( tr.startsolid || tr.allsolid )
	{
		b->p[j] = b->o[j];
		return;
	}

	if( tr.fraction < 1.0f )
	{
		Vector n = Vector( tr.plane.normal );
		Vector hit = Vector( tr.endpos ) + n * 1.0f;
		Vector vel = b->p[j] - b->o[j];
		Vector vn = n * DotProduct( vel, n );
		Vector vt = vel - vn;

		b->p[j] = hit;
		// slide with friction, a little bounce
		b->o[j] = hit - vt * RD_FRICTION + vn * 0.15f;
	}
}

static void RD_Step( rdBody_t *b )
{
	float moved = 0.0f;

	for( int j = 0; j < J_NUM; j++ )
	{
		Vector cur = b->p[j];
		Vector vel = ( b->p[j] - b->o[j] ) * RD_DRAG;

		b->o[j] = cur;
		b->p[j] = cur + vel + Vector( 0.0f, 0.0f, -RD_GRAVITY * RD_STEP * RD_STEP );
	}

	for( int it = 0; it < RD_ITER; it++ )
	{
		for( unsigned l = 0; l < RD_NUM_LINKS; l++ )
		{
			Vector &a = b->p[s_links[l][0]];
			Vector &c = b->p[s_links[l][1]];
			Vector d = c - a;
			float len = d.Length();

			if( len < 0.001f )
				continue;

			// reach links only stop over-stretching, so limbs can bend
			if( l >= RD_FIRST_REACH && len <= b->linkLen[l] )
				continue;

			Vector corr = d * ( 0.5f * ( len - b->linkLen[l] ) / len );
			a = a + corr;
			c = c - corr;
		}
	}

	for( int j = 0; j < J_NUM; j++ )
	{
		RD_Collide( b, j );
		moved = Q_max( moved, ( b->p[j] - b->o[j] ).Length());
	}

	if( moved < RD_SETTLE )
		b->rest_time += RD_STEP;
	else b->rest_time = 0.0f;

	if( b->rest_time > 0.75f || gEngfuncs.GetClientTime() - b->spawned > 12.0f )
		b->moving = false;
}

// orthonormal basis (columns) from a part's joints
static void RD_Basis( const Vector &a, const Vector &axisEnd, const Vector &ref, float m[3][3] )
{
	Vector x = ( axisEnd - a ).Normalize();
	Vector r = ref - a;
	Vector z = CrossProduct( x, r );

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
	if( b->started && ( ent->model != b->model || ent->curstate.sequence != b->sequence || hdr->numbones != b->numbones ))
	{
		b->used = false;
		return;
	}

	if( !b->started )
	{
		// count bodies already moving: past the limit the oldest one freezes
		int moving = 0;
		rdBody_t *oldest = NULL;

		for( int i = 0; i < RD_MAX; i++ )
		{
			if( s_bodies[i].used && s_bodies[i].moving )
			{
				moving++;
				if( !oldest || s_bodies[i].spawned < oldest->spawned )
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
	}

	// rotation of every part from the death pose to now
	float rot[S_NUM][3][3];
	for( int s = 0; s < S_NUM; s++ )
	{
		float restM[3][3], nowM[3][3];
		const int *sg = s_segs[s];

		RD_Basis( b->restJoint[sg[0]], b->restJoint[sg[1]], b->restJoint[sg[2]], restM );
		RD_Basis( b->p[sg[0]], b->p[sg[1]], b->p[sg[2]], nowM );

		// rot = now * rest^T
		for( int i = 0; i < 3; i++ )
			for( int k = 0; k < 3; k++ )
				rot[s][i][k] = nowM[i][0] * restM[k][0] + nowM[i][1] * restM[k][1] + nowM[i][2] * restM[k][2];
	}

	for( int i = 0; i < b->numbones; i++ )
	{
		int s = b->boneSeg[i];
		const float ( *R )[3] = rot[s];
		const float ( *M )[4] = b->rest[i];
		Vector restOrigin = b->restJoint[s_segs[s][0]];
		Vector nowOrigin = b->p[s_segs[s][0]];
		Vector rel = RD_BonePos( M ) - restOrigin;
		float out[3][4];

		for( int r = 0; r < 3; r++ )
		{
			for( int c = 0; c < 3; c++ )
				out[r][c] = R[r][0] * M[0][c] + R[r][1] * M[1][c] + R[r][2] * M[2][c];

			out[r][3] = R[r][0] * rel.x + R[r][1] * rel.y + R[r][2] * rel.z + nowOrigin[r];
		}

		memcpy( (*bones)[i], out, sizeof( out ));
		if( lights )
			memcpy( (*lights)[i], out, sizeof( out ));
	}
}

#endif // XASH_UWP
