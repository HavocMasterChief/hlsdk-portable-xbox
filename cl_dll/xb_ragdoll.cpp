/*
xb_ragdoll.cpp - Xbox build: drawing ragdolls (client side)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

The game runs the ragdoll bodies (dlls/xb_ragdoll_sv.cpp) and sends where
each of their particles is; this file turns that into a pose:

  - Each particle sits on a bone. Its turn is worked out from where it
    points now against where it pointed at death, starting from the turn of
    the particle it hangs from, so a forearm twists with its upper arm and
    elbows and knees stay hinges. Rigid parts (pelvis, chest, a creature's
    core) take their turn from three particles.
  - The whole skeleton is then rebuilt parent to child, every bone keeping
    its length: joints stay attached, nothing tears or pinches.
  - Bones between two particles (spine, neck, collarbones) share the turn
    between them, so the back curves instead of folding at one point.
  - Positions are blended between the game's updates (30 a second).

xb_ragdoll 0 turns ragdolls off (read by the game too).
*/

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

#if XASH_UWP

#define RD_MAX      24
#define RD_MAX_P    24
#define RD_INTERVAL ( 1.0f / 30.0f )

enum { XB_RD_START = 0, XB_RD_CLEAR = 2, XB_RD_UPDATE = 3 };

struct rdDraw_t
{
	bool used, started, haveSnap;
	int ent;
	model_t *model;
	int numbones;

	int np;
	int bone[RD_MAX_P];
	signed char parent[RD_MAX_P], axis[RD_MAX_P], ref[RD_MAX_P];

	// snapshots from the game
	Vector from[RD_MAX_P], to[RD_MAX_P];
	float snapTime;

	// the skeleton at death
	float rest[MAXSTUDIOBONES][3][4];
	float local[MAXSTUDIOBONES][3][4];   // each bone relative to its parent
	Vector restP[RD_MAX_P];
	signed char driver[MAXSTUDIOBONES];  // particle whose turn the bone takes
	signed char blendTo[MAXSTUDIOBONES]; // ...blended toward this one (spine, neck)
	float blend[MAXSTUDIOBONES];
};

static rdDraw_t s_draw[RD_MAX];
static cvar_t *xb_ragdoll;

/*
================
small matrix and quaternion helpers
================
*/
typedef float mat3[3][3];

static void M3_Mul( const mat3 a, const mat3 b, mat3 out )
{
	mat3 t;
	for( int i = 0; i < 3; i++ )
		for( int j = 0; j < 3; j++ )
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
	memcpy( out, t, sizeof( t ));
}

static Vector M3_Apply( const mat3 m, const Vector &v )
{
	return Vector( m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
		m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
		m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z );
}

static void M3_Identity( mat3 m )
{
	memset( m, 0, sizeof( mat3 ));
	m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

// the turn taking unit vector a onto unit vector b, the short way round
static void M3_Arc( const Vector &a, const Vector &b, mat3 out )
{
	Vector v = CrossProduct( a, b );
	float c = DotProduct( a, b );

	if( c < -0.9999f )
	{
		// opposite: half a turn about any axis square to a
		Vector ax = CrossProduct( a, fabs( a.x ) < 0.9f ? Vector( 1, 0, 0 ) : Vector( 0, 1, 0 )).Normalize();
		for( int i = 0; i < 3; i++ )
			for( int j = 0; j < 3; j++ )
				out[i][j] = 2.0f * ax[i] * ax[j] - ( i == j ? 1.0f : 0.0f );
		return;
	}

	float k = 1.0f / ( 1.0f + c );
	out[0][0] = c + v.x * v.x * k;        out[0][1] = v.x * v.y * k - v.z;  out[0][2] = v.x * v.z * k + v.y;
	out[1][0] = v.y * v.x * k + v.z;      out[1][1] = c + v.y * v.y * k;    out[1][2] = v.y * v.z * k - v.x;
	out[2][0] = v.z * v.x * k - v.y;      out[2][1] = v.z * v.y * k + v.x;  out[2][2] = c + v.z * v.z * k;
}

// frame from a joint, the joint its bone points at and a third to fix the twist
static void M3_Basis( const Vector &a, const Vector &axisEnd, const Vector &ref, mat3 m )
{
	Vector x = axisEnd - a;
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

struct rdQuat { float x, y, z, w; };

static rdQuat Q_FromM3( const mat3 m )
{
	rdQuat q;
	float tr = m[0][0] + m[1][1] + m[2][2];
	if( tr > 0.0f )
	{
		float s = sqrtf( tr + 1.0f ) * 2.0f;
		q.w = 0.25f * s;
		q.x = ( m[2][1] - m[1][2] ) / s;
		q.y = ( m[0][2] - m[2][0] ) / s;
		q.z = ( m[1][0] - m[0][1] ) / s;
	}
	else if( m[0][0] > m[1][1] && m[0][0] > m[2][2] )
	{
		float s = sqrtf( 1.0f + m[0][0] - m[1][1] - m[2][2] ) * 2.0f;
		q.w = ( m[2][1] - m[1][2] ) / s;
		q.x = 0.25f * s;
		q.y = ( m[0][1] + m[1][0] ) / s;
		q.z = ( m[0][2] + m[2][0] ) / s;
	}
	else if( m[1][1] > m[2][2] )
	{
		float s = sqrtf( 1.0f + m[1][1] - m[0][0] - m[2][2] ) * 2.0f;
		q.w = ( m[0][2] - m[2][0] ) / s;
		q.x = ( m[0][1] + m[1][0] ) / s;
		q.y = 0.25f * s;
		q.z = ( m[1][2] + m[2][1] ) / s;
	}
	else
	{
		float s = sqrtf( 1.0f + m[2][2] - m[0][0] - m[1][1] ) * 2.0f;
		q.w = ( m[1][0] - m[0][1] ) / s;
		q.x = ( m[0][2] + m[2][0] ) / s;
		q.y = ( m[1][2] + m[2][1] ) / s;
		q.z = 0.25f * s;
	}
	return q;
}

static void Q_ToM3( const rdQuat &q, mat3 m )
{
	float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	m[0][0] = 1 - 2 * ( yy + zz ); m[0][1] = 2 * ( xy - wz );     m[0][2] = 2 * ( xz + wy );
	m[1][0] = 2 * ( xy + wz );     m[1][1] = 1 - 2 * ( xx + zz ); m[1][2] = 2 * ( yz - wx );
	m[2][0] = 2 * ( xz - wy );     m[2][1] = 2 * ( yz + wx );     m[2][2] = 1 - 2 * ( xx + yy );
}

static void M3_Slerp( const mat3 a, const mat3 b, float t, mat3 out )
{
	rdQuat qa = Q_FromM3( a ), qb = Q_FromM3( b );
	float d = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
	if( d < 0.0f )
	{
		qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; qb.w = -qb.w;
		d = -d;
	}

	rdQuat q;
	if( d > 0.9995f )
	{
		q.x = qa.x + ( qb.x - qa.x ) * t; q.y = qa.y + ( qb.y - qa.y ) * t;
		q.z = qa.z + ( qb.z - qa.z ) * t; q.w = qa.w + ( qb.w - qa.w ) * t;
	}
	else
	{
		float th = acosf( d ), s = sinf( th );
		float wa = sinf(( 1.0f - t ) * th ) / s, wb = sinf( t * th ) / s;
		q.x = qa.x * wa + qb.x * wb; q.y = qa.y * wa + qb.y * wb;
		q.z = qa.z * wa + qb.z * wb; q.w = qa.w * wa + qb.w * wb;
	}

	float l = sqrtf( q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w );
	q.x /= l; q.y /= l; q.z /= l; q.w /= l;
	Q_ToM3( q, out );
}

// 3x4 bone matrices
static void B_Invert( const float in[3][4], float out[3][4] )
{
	for( int i = 0; i < 3; i++ )
		for( int j = 0; j < 3; j++ )
			out[i][j] = in[j][i];
	for( int i = 0; i < 3; i++ )
		out[i][3] = -( out[i][0] * in[0][3] + out[i][1] * in[1][3] + out[i][2] * in[2][3] );
}

static void B_Concat( const float a[3][4], const float b[3][4], float out[3][4] )
{
	for( int i = 0; i < 3; i++ )
	{
		for( int j = 0; j < 3; j++ )
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		out[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
	}
}

/*
================
messages
================
*/
static rdDraw_t *RD_Find( int ent )
{
	for( int i = 0; i < RD_MAX; i++ )
		if( s_draw[i].used && s_draw[i].ent == ent )
			return &s_draw[i];
	return NULL;
}

static void RD_Current( const rdDraw_t *d, Vector *out )
{
	float t = ( gEngfuncs.GetClientTime() - d->snapTime ) / RD_INTERVAL;
	if( t < 0.0f ) t = 0.0f;
	if( t > 1.0f ) t = 1.0f;
	for( int n = 0; n < d->np; n++ )
		out[n] = d->from[n] + ( d->to[n] - d->from[n] ) * t;
}

static int MsgFunc_XBRagdoll( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	int ent = READ_SHORT();
	int kind = READ_BYTE();

	if( kind == XB_RD_CLEAR )
	{
		rdDraw_t *d = RD_Find( ent );
		if( d )
			d->used = false;
		return 1;
	}

	if( kind == XB_RD_START )
	{
		rdDraw_t *d = RD_Find( ent );
		if( !d )
		{
			d = &s_draw[0];
			for( int i = 0; i < RD_MAX; i++ )
				if( !s_draw[i].used ) { d = &s_draw[i]; break; }
		}

		memset( d, 0, sizeof( *d ));
		d->used = true;
		d->ent = ent;
		d->np = READ_BYTE();
		if( d->np > RD_MAX_P )
			d->np = RD_MAX_P;
		for( int n = 0; n < d->np; n++ )
		{
			d->bone[n] = READ_BYTE();
			d->parent[n] = READ_CHAR();
			d->axis[n] = READ_CHAR();
			d->ref[n] = READ_CHAR();
		}
		gEngfuncs.Con_DPrintf( "ragdoll: ent %d, %d particles\n", ent, d->np );
		return 1;
	}

	if( kind == XB_RD_UPDATE )
	{
		rdDraw_t *d = RD_Find( ent );
		int np = READ_BYTE();
		READ_BYTE(); // asleep: the last update simply holds
		if( !d || np != d->np )
			return 1;

		Vector cur[RD_MAX_P];
		if( d->haveSnap )
			RD_Current( d, cur );

		for( int n = 0; n < np; n++ )
		{
			Vector v;
			v.x = READ_SHORT() * 0.25f;
			v.y = READ_SHORT() * 0.25f;
			v.z = READ_SHORT() * 0.25f;
			d->from[n] = d->haveSnap ? cur[n] : v;
			d->to[n] = v;
		}
		d->snapTime = gEngfuncs.GetClientTime();
		d->haveSnap = true;
	}
	return 1;
}

void XBRagdoll_Init( void )
{
	xb_ragdoll = gEngfuncs.pfnRegisterVariable( "xb_ragdoll", "1", FCVAR_ARCHIVE );
	gEngfuncs.pfnHookUserMsg( "XBRagdoll", MsgFunc_XBRagdoll );
	memset( s_draw, 0, sizeof( s_draw ));
}

void XBRagdoll_Reset( void )
{
	memset( s_draw, 0, sizeof( s_draw ));
}

/*
================
the skeleton at death
================
*/
static void RD_Start( rdDraw_t *d, cl_entity_t *ent, studiohdr_t *hdr, float (*bones)[MAXSTUDIOBONES][3][4] )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	d->numbones = hdr->numbones;
	d->model = ent->model;
	memcpy( d->rest, *bones, sizeof( float ) * 12 * hdr->numbones );

	for( int i = 0; i < hdr->numbones; i++ )
	{
		int par = pbones[i].parent;
		if( par >= 0 )
		{
			float inv[3][4];
			B_Invert( d->rest[par], inv );
			B_Concat( inv, d->rest[i], d->local[i] );
		}
		else memcpy( d->local[i], d->rest[i], sizeof( d->local[i] ));
	}

	for( int n = 0; n < d->np; n++ )
	{
		int b = d->bone[n] < hdr->numbones ? d->bone[n] : 0;
		d->restP[n] = Vector( d->rest[b][0][3], d->rest[b][1][3], d->rest[b][2][3] );
	}

	// each bone takes the turn of the particle on it or on its nearest
	// ancestor, blended toward a particle further down its own chain
	for( int i = 0; i < hdr->numbones; i++ )
	{
		int drv = -1, depthDrv = 0, depth = 0;
		for( int k = i; k >= 0 && drv < 0; k = pbones[k].parent, depth++ )
		{
			for( int n = 0; n < d->np; n++ )
				if( d->bone[n] == k ) { drv = n; depthDrv = depth; break; }
		}
		d->driver[i] = drv < 0 ? 0 : drv;
		d->blendTo[i] = -1;
		d->blend[i] = 0.0f;

		if( drv < 0 || d->bone[drv] == i )
			continue;

		// a particle further down this bone's own chain: share the turn
		for( int n = 0; n < d->np; n++ )
		{
			int down = 0, k;
			for( k = d->bone[n]; k >= 0 && k != i; k = pbones[k].parent )
				down++;
			if( k == i && down > 0 )
			{
				// depthDrv steps up to the driver, down steps on to that particle
				d->blendTo[i] = n;
				d->blend[i] = (float)depthDrv / (float)( depthDrv + down );
				break;
			}
		}
	}

	d->started = true;
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

	rdDraw_t *d = RD_Find( ent->index );
	if( !d || !d->haveSnap || hdr->numbones > MAXSTUDIOBONES )
		return;

	if( d->started && ( ent->model != d->model || hdr->numbones != d->numbones ))
	{
		d->used = false;
		return;
	}

	if( !d->started )
		RD_Start( d, ent, hdr, bones );

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	Vector pos[RD_MAX_P];
	RD_Current( d, pos );

	// each particle's turn since death, parents before children
	mat3 rot[RD_MAX_P];
	bool done[RD_MAX_P];
	memset( done, 0, sizeof( done ));

	for( int pass = 0; pass < 4; pass++ )
	{
		for( int n = 0; n < d->np; n++ )
		{
			if( done[n] )
				continue;

			int par = d->parent[n];
			if( par >= 0 && !done[par] )
				continue;

			if( d->ref[n] >= 0 && d->axis[n] >= 0 )
			{
				// rigid: from three particles
				mat3 restM, nowM, restT;
				M3_Basis( d->restP[n], d->restP[d->axis[n]], d->restP[d->ref[n]], restM );
				M3_Basis( pos[n], pos[d->axis[n]], pos[d->ref[n]], nowM );
				for( int i = 0; i < 3; i++ )
					for( int j = 0; j < 3; j++ )
						restT[i][j] = restM[j][i];
				M3_Mul( nowM, restT, rot[n] );
			}
			else
			{
				mat3 base;
				if( par >= 0 ) memcpy( base, rot[par], sizeof( mat3 ));
				else M3_Identity( base );

				if( d->axis[n] >= 0 )
				{
					// the parent's turn, then the short way onto where the bone points now
					Vector was = M3_Apply( base, ( d->restP[d->axis[n]] - d->restP[n] )).Normalize();
					Vector now = ( pos[d->axis[n]] - pos[n] ).Normalize();
					mat3 arc;
					M3_Arc( was, now, arc );
					M3_Mul( arc, base, rot[n] );
				}
				else memcpy( rot[n], base, sizeof( mat3 ));
			}
			done[n] = true;
		}
	}
	for( int n = 0; n < d->np; n++ )
		if( !done[n] ) M3_Identity( rot[n] );

	// rebuild parent to child: every bone keeps its length
	static float out[MAXSTUDIOBONES][3][4];
	for( int i = 0; i < hdr->numbones; i++ )
	{
		int drv = d->driver[i];
		mat3 r;

		if( d->blendTo[i] >= 0 )
			M3_Slerp( rot[drv], rot[d->blendTo[i]], d->blend[i], r );
		else
			memcpy( r, rot[drv], sizeof( mat3 ));

		// orientation: the turn applied to the bone at death
		for( int a = 0; a < 3; a++ )
			for( int b = 0; b < 3; b++ )
				out[i][a][b] = r[a][0] * d->rest[i][0][b] + r[a][1] * d->rest[i][1][b] + r[a][2] * d->rest[i][2][b];

		int par = pbones[i].parent;
		Vector p;
		if( par >= 0 && par < i )
		{
			// hangs off its parent, at the same offset as in life
			p.x = out[par][0][0] * d->local[i][0][3] + out[par][0][1] * d->local[i][1][3] + out[par][0][2] * d->local[i][2][3] + out[par][0][3];
			p.y = out[par][1][0] * d->local[i][0][3] + out[par][1][1] * d->local[i][1][3] + out[par][1][2] * d->local[i][2][3] + out[par][1][3];
			p.z = out[par][2][0] * d->local[i][0][3] + out[par][2][1] * d->local[i][1][3] + out[par][2][2] * d->local[i][2][3] + out[par][2][3];
		}
		else
		{
			// a root: placed by the body's root particle
			Vector rel = Vector( d->rest[i][0][3], d->rest[i][1][3], d->rest[i][2][3] ) - d->restP[drv];
			p = pos[drv] + M3_Apply( r, rel );
		}
		out[i][0][3] = p.x;
		out[i][1][3] = p.y;
		out[i][2][3] = p.z;
	}

	memcpy( *bones, out, sizeof( float ) * 12 * hdr->numbones );
	if( lights )
		memcpy( *lights, out, sizeof( float ) * 12 * hdr->numbones );
}

/*
=============
XBRagdoll_BodyPelvis

Where a ragdolled body's middle is, for the blood pool under it.
=============
*/
bool XBRagdoll_BodyPelvis( int ent, float *out, bool *settled )
{
	rdDraw_t *d = RD_Find( ent );

	if( !d || !d->haveSnap )
		return false;

	out[0] = d->to[0].x;
	out[1] = d->to[0].y;
	out[2] = d->to[0].z;
	*settled = ( gEngfuncs.GetClientTime() - d->snapTime ) > 0.5f;
	return true;
}

#endif // XASH_UWP
