/*
xb_leg.cpp - Xbox build: Gordon's leg for the kick
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

The right stick click runs "xbkick" here. The kick is drawn with Gordon's
own player model in front of the camera: every bone but the right leg is
shrunk to nothing, and the leg is posed by hand over about 0.45 s:

  0.00-0.12  knee comes up
  0.12-0.20  leg snaps out straight, sole first  -> the hit (sent to the game)
  0.20-0.28  held
  0.28-0.45  pulled back down out of view

The game (dlls/player.cpp CBasePlayer::XBKick) does the damage and shove
when the leg is fully out. The weapon dips while the leg is up.
*/

#include <string.h>
#include <math.h>
#include <ctype.h>
#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "com_model.h"
#include "studio.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "entity_types.h"

#if XASH_UWP

#define LEG_MARK      0x4C454721   // curstate.iuser4 of the leg entity
#define KICK_LEN      0.45f
#define KICK_HIT      0.18f
#define KICK_COOLDOWN 0.75f

extern vec3_t v_origin, v_angles;

static cl_entity_t s_leg;
static float s_kickStart = -100.0f;
static bool s_hitSent = true;
static int s_bones[4] = { -1, -1, -1, -1 };   // thigh, calf, foot, toe
static model_t *s_bonesFor;

static float LegTime( void )
{
	return gEngfuncs.GetClientTime() - s_kickStart;
}

static void XBLeg_Kick_f( void )
{
	float now = gEngfuncs.GetClientTime();

	if( now - s_kickStart < KICK_COOLDOWN || gHUD.m_Health.m_iHealth <= 0 )
		return;

	s_kickStart = now;
	s_hitSent = false;
}

void XBLeg_Init( void )
{
	gEngfuncs.pfnAddCommand( "xbkick", XBLeg_Kick_f );
}

void XBLeg_VidInit( void )
{
	s_kickStart = -100.0f;
	s_hitSent = true;
	s_bonesFor = NULL;
}

// 0 hidden .. 1 fully out
static float LegExtend( float t, float *chamber )
{
	*chamber = 0.0f;
	if( t < 0.0f || t > KICK_LEN )
		return -1.0f;

	if( t < 0.12f ) { *chamber = t / 0.12f; return 0.0f; }
	if( t < 0.20f ) { *chamber = 1.0f; return ( t - 0.12f ) / 0.08f; }
	if( t < 0.28f ) { *chamber = 1.0f; return 1.0f; }
	float back = ( t - 0.28f ) / ( KICK_LEN - 0.28f );   // 0..1 going away
	*chamber = 1.0f - back;
	return 1.0f - back;
}

/*
=============
XBLeg_AddEntity

From HUD_CreateEntities: the leg while a kick is on, and the hit when it lands.
=============
*/
void XBLeg_AddEntity( void )
{
	float t = LegTime();

	if( !s_hitSent && t >= KICK_HIT )
	{
		s_hitSent = true;
		gEngfuncs.pfnServerCmd( "xbkick" );
	}

	if( t < 0.0f || t > KICK_LEN )
		return;

	cl_entity_t *player = gEngfuncs.GetLocalPlayer();
	int index = 0;
	model_t *mod = gEngfuncs.CL_LoadModel( "models/player.mdl", &index );
	if( !player || !mod )
		return;

	s_leg = *player;
	s_leg.player = 0;
	s_leg.index = 0;
	s_leg.model = mod;
	s_leg.curstate.modelindex = index;
	s_leg.curstate.sequence = 0;
	s_leg.curstate.frame = 0;
	s_leg.curstate.framerate = 0;
	s_leg.curstate.iuser4 = LEG_MARK;
	s_leg.curstate.rendermode = kRenderNormal;
	s_leg.curstate.effects = 0;

	// in front of the eyes, facing where we look, so it's drawn and lit
	vec3_t fwd, right, up, ang;
	VectorCopy( v_angles, ang );
	ang[0] = 0;
	AngleVectors( ang, fwd, right, up );
	for( int i = 0; i < 3; i++ )
		s_leg.origin[i] = v_origin[i] + fwd[i] * 24.0f - up[i] * 30.0f;
	VectorCopy( ang, s_leg.angles );
	VectorCopy( s_leg.origin, s_leg.curstate.origin );
	VectorCopy( ang, s_leg.curstate.angles );

	gEngfuncs.CL_CreateVisibleEntity( ET_NORMAL, &s_leg );
}

/*
=============
XBLeg_ViewModel

From V_CalcNormalRefdef: the weapon dips while the leg is up.
=============
*/
void XBLeg_ViewModel( cl_entity_t *view, const float *up )
{
	float chamber;
	float ext = LegExtend( LegTime(), &chamber );
	if( ext < 0.0f || !view )
		return;

	float dip = chamber > ext ? chamber : ext;
	for( int i = 0; i < 3; i++ )
		view->origin[i] -= up[i] * dip * 6.0f;
	view->angles[0] -= dip * 8.0f;
}

static const char *LegKey( const char *name, char *out, size_t size )
{
	size_t n = 0;
	for( const char *c = name; *c && n + 1 < size; c++ )
		out[n++] = ( *c == '_' ) ? ' ' : (char)tolower( (unsigned char)*c );
	out[n] = 0;
	return out;
}

static void LegFindBones( studiohdr_t *hdr, model_t *mod )
{
	static const char *want[4][3] =
	{
		{ "bip01 r thigh", "bip01 r leg", NULL },
		{ "bip01 r calf", "bip01 r leg1", NULL },
		{ "bip01 r foot", NULL },
		{ "bip01 r toe0", "bip01 r toe", NULL },
	};
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	for( int j = 0; j < 4; j++ )
	{
		s_bones[j] = -1;
		for( int w = 0; w < 3 && want[j][w] && s_bones[j] < 0; w++ )
		{
			for( int i = 0; i < hdr->numbones; i++ )
			{
				char k[64];
				if( !strcmp( LegKey( pbones[i].name, k, sizeof( k )), want[j][w] ))
				{
					s_bones[j] = i;
					break;
				}
			}
		}
	}
	s_bonesFor = mod;

	if( s_bones[0] < 0 || s_bones[1] < 0 || s_bones[2] < 0 )
		gEngfuncs.Con_DPrintf( "xb_leg: player model has no right leg bones\n" );
}

static void Arc( const Vector &a, const Vector &b, float m[3][3] )
{
	Vector v = CrossProduct( a, b );
	float c = DotProduct( a, b );
	if( c < -0.999f ) c = -0.999f;
	float k = 1.0f / ( 1.0f + c );
	m[0][0] = c + v.x * v.x * k;     m[0][1] = v.x * v.y * k - v.z; m[0][2] = v.x * v.z * k + v.y;
	m[1][0] = v.y * v.x * k + v.z;   m[1][1] = c + v.y * v.y * k;   m[1][2] = v.y * v.z * k - v.x;
	m[2][0] = v.z * v.x * k - v.y;   m[2][1] = v.z * v.y * k + v.x; m[2][2] = c + v.z * v.z * k;
}

// bone = turn * bone, about the bone's own position, then moved to pos
static void SetBone( float b[3][4], const float r[3][3], const Vector &pos )
{
	float o[3][3];
	for( int i = 0; i < 3; i++ )
		for( int j = 0; j < 3; j++ )
			o[i][j] = r[i][0] * b[0][j] + r[i][1] * b[1][j] + r[i][2] * b[2][j];
	for( int i = 0; i < 3; i++ )
	{
		for( int j = 0; j < 3; j++ )
			b[i][j] = o[i][j];
	}
	b[0][3] = pos.x; b[1][3] = pos.y; b[2][3] = pos.z;
}

/*
=============
XBLeg_Apply

From StudioSetupBones: is this the leg? Pose it, hide the rest.
=============
*/
bool XBLeg_Apply( cl_entity_t *ent, void *studiohdr, float (*bones)[MAXSTUDIOBONES][3][4], float (*lights)[MAXSTUDIOBONES][3][4] )
{
	studiohdr_t *hdr = (studiohdr_t *)studiohdr;

	if( !ent || ent->curstate.iuser4 != LEG_MARK || !hdr )
		return false;

	if( s_bonesFor != ent->model )
		LegFindBones( hdr, ent->model );

	int thigh = s_bones[0], calf = s_bones[1], foot = s_bones[2], toe = s_bones[3];
	if( thigh < 0 || calf < 0 || foot < 0 )
		return true;

	float chamber;
	float ext = LegExtend( LegTime(), &chamber );
	if( ext < 0.0f )
		ext = chamber = 0.0f;

	// the leg as the model has it standing
	Vector H = Vector( (*bones)[thigh][0][3], (*bones)[thigh][1][3], (*bones)[thigh][2][3] );
	Vector K = Vector( (*bones)[calf][0][3], (*bones)[calf][1][3], (*bones)[calf][2][3] );
	Vector F = Vector( (*bones)[foot][0][3], (*bones)[foot][1][3], (*bones)[foot][2][3] );
	float lThigh = ( K - H ).Length(), lCalf = ( F - K ).Length();

	// where it goes: from under the view, the knee up, then straight out
	vec3_t fwdv, rightv, upv, ang;
	VectorCopy( v_angles, ang );
	ang[0] = 0;
	AngleVectors( ang, fwdv, rightv, upv );
	Vector fwd( fwdv ), right( rightv ), up( upv ), eye( v_origin );

	// a little lower when looking up, so it still comes from below
	float pitch = v_angles[0];   // + down
	Vector hip = eye + right * 6.0f - up * ( 34.0f - Q_min( pitch, 0.0f ) * 0.15f ) - fwd * 4.0f;

	// thigh: hanging -> knee raised forward -> level with the kick
	Vector thighDown = up * -1.0f;
	Vector thighUp = ( fwd * 0.8f + up * 0.6f ).Normalize();
	Vector thighOut = ( fwd * 0.95f + up * 0.3f ).Normalize();
	Vector thighDir = ( thighDown * ( 1.0f - chamber ) + thighUp * chamber ).Normalize();
	thighDir = ( thighDir * ( 1.0f - ext ) + thighOut * ext ).Normalize();

	// calf: folded back under the knee, then straight in line
	Vector calfFold = ( up * -0.85f - fwd * 0.5f ).Normalize();
	Vector calfDir = ( calfFold * ( 1.0f - ext ) + thighOut * ext ).Normalize();
	if( chamber < 1.0f && ext <= 0.0f )
		calfDir = ( up * -1.0f * ( 1.0f - chamber ) + calfFold * chamber ).Normalize();

	Vector knee = hip + thighDir * lThigh;
	Vector ankle = knee + calfDir * lCalf;

	// turn the bones: thigh and calf onto their new directions, the foot
	// with the calf so the sole leads
	float rT[3][3], rC[3][3];
	Arc(( K - H ).Normalize(), thighDir, rT );
	Arc(( F - K ).Normalize(), calfDir, rC );

	float keepFoot[3][4], keepToe[3][4];
	memcpy( keepFoot, (*bones)[foot], sizeof( keepFoot ));
	if( toe >= 0 )
		memcpy( keepToe, (*bones)[toe], sizeof( keepToe ));

	SetBone( (*bones)[thigh], rT, hip );
	SetBone( (*bones)[calf], rC, knee );
	SetBone( (*bones)[foot], rC, ankle );
	if( toe >= 0 )
	{
		Vector rel = Vector( keepToe[0][3], keepToe[1][3], keepToe[2][3] ) - Vector( keepFoot[0][3], keepFoot[1][3], keepFoot[2][3] );
		Vector tp = ankle + Vector( rC[0][0] * rel.x + rC[0][1] * rel.y + rC[0][2] * rel.z,
			rC[1][0] * rel.x + rC[1][1] * rel.y + rC[1][2] * rel.z,
			rC[2][0] * rel.x + rC[2][1] * rel.y + rC[2][2] * rel.z );
		SetBone( (*bones)[toe], rC, tp );
	}

	// everything else shrinks into the hip and disappears
	for( int i = 0; i < hdr->numbones; i++ )
	{
		if( i == thigh || i == calf || i == foot || i == toe )
			continue;
		memset( (*bones)[i], 0, sizeof( (*bones)[i] ));
		(*bones)[i][0][3] = hip.x;
		(*bones)[i][1][3] = hip.y;
		(*bones)[i][2][3] = hip.z;
	}

	if( lights )
		memcpy( *lights, *bones, sizeof( float ) * 12 * hdr->numbones );
	return true;
}

#endif // XASH_UWP
