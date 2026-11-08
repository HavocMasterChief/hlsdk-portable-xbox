/*
xb_rdphys.cpp - Xbox build: ragdoll physics core (see xb_rdphys.h)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <math.h>
#include <string.h>
#include <ctype.h>
#include "xb_rdphys.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RD_ITER         8
#define RD_GRAVITY      640.0f
#define RD_AIR_DRAG     0.998f   // speed kept per step in the air
#define RD_CONTACT_KEEP 0.85f    // speed kept per step by a particle touching the world
#define RD_GROUND_KEEP  0.30f    // tangential speed kept when it hits the world
#define RD_SLIDE_KEEP   0.88f    // per step while resting on the floor
#define RD_JOINT_FRIC   0.06f    // how much linked particles share speed: joint stiffness
#define RD_SETTLE       0.15f    // units per step below which a body is resting
#define RD_REST_TIME    0.3f
#define RD_MAX_TIME     7.0f

rdpTraceFn RDP_Trace;

static inline float RD_Min( float a, float b ) { return a < b ? a : b; }
static inline float RD_Max( float a, float b ) { return a > b ? a : b; }

float rdpVec::Length() const { return sqrtf( x * x + y * y + z * z ); }
rdpVec rdpVec::Normalize() const
{
	float l = Length();
	return l > 1e-6f ? rdpVec( x / l, y / l, z / l ) : rdpVec( 0, 0, 0 );
}
float RDP_Dot( const rdpVec &a, const rdpVec &b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
rdpVec RDP_Cross( const rdpVec &a, const rdpVec &b )
{
	return rdpVec( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x );
}

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

// tree: parent, axis (the joint the bone points at), ref (fixes a rigid part's twist)
static const signed char s_humanTree[J_NUM][3] =
{
	{ -1,          J_CHEST,  J_LHIP },       // pelvis: rigid with the hips
	{ J_PELVIS,    J_HEAD,   J_LSHOULDER },  // chest: rigid with the shoulders
	{ J_CHEST,     -1,       -1 },
	{ J_CHEST,     J_LELBOW, -1 },
	{ J_LSHOULDER, J_LHAND,  -1 },
	{ J_LELBOW,    -1,       -1 },
	{ J_CHEST,     J_RELBOW, -1 },
	{ J_RSHOULDER, J_RHAND,  -1 },
	{ J_RELBOW,    -1,       -1 },
	{ J_PELVIS,    J_LKNEE,  -1 },
	{ J_LHIP,      J_LFOOT,  -1 },
	{ J_LKNEE,     -1,       -1 },
	{ J_PELVIS,    J_RKNEE,  -1 },
	{ J_RHIP,      J_RFOOT,  -1 },
	{ J_RKNEE,     -1,       -1 },
};

// "Bip01 L Arm1" -> "l arm1"; anything not on a biped -> NULL
static const char *RD_BoneKey( const char *name, char *out, size_t size )
{
	size_t n = 0;

	if( !name )
		return NULL;

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

/*
================
building
================
*/
static void RD_Link( rdpBody *b, int a, int c, bool limb )
{
	if( b->nl >= RDP_MAX_LINKS || a == c )
		return;

	float len = ( b->restP[a] - b->restP[c] ).Length();
	rdpLink *l = &b->links[b->nl++];
	l->a = a;
	l->b = c;
	l->lo = len;
	l->hi = len;
	l->limb = limb;
}

// a joint at mid between ends a and c may bend until its inside angle is
// minAngle degrees: a range link between the two ends, whichever way it bends
static void RD_BendLimit( rdpBody *b, int a, int mid, int c, float minAngle )
{
	if( b->nl >= RDP_MAX_LINKS )
		return;

	float la = ( b->restP[a] - b->restP[mid] ).Length();
	float lc = ( b->restP[c] - b->restP[mid] ).Length();
	float rad = minAngle * (float)( M_PI / 180.0 );
	float lo = sqrtf( RD_Max( 0.0f, la * la + lc * lc - 2.0f * la * lc * cosf( rad )));

	rdpLink *l = &b->links[b->nl++];
	l->a = a;
	l->b = c;
	l->lo = lo;
	l->hi = la + lc;
	l->limb = false;
}

static bool RD_BuildHuman( rdpBody *b, int numbones, const char *const *names, const rdpVec *bonePos )
{
	char keys[RDP_MAX_BONES][32];
	const char *key[RDP_MAX_BONES];
	int jointBone[J_NUM];

	for( int i = 0; i < numbones; i++ )
		key[i] = RD_BoneKey( names ? names[i] : NULL, keys[i], sizeof( keys[i] ));

	for( int j = 0; j < J_NUM; j++ )
	{
		jointBone[j] = -1;
		for( int n = 0; n < RD_NAMES && s_jointBones[j][n] && jointBone[j] < 0; n++ )
		{
			for( int i = 0; i < numbones; i++ )
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
		b->bone[j] = jointBone[j];
		b->radius[j] = s_jointRadius[j];
		b->restP[j] = bonePos[jointBone[j]];
		b->parent[j] = s_humanTree[j][0];
		b->axis[j] = s_humanTree[j][1];
		b->ref[j] = s_humanTree[j][2];
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

	return true;
}

static bool RD_BuildGeneric( rdpBody *b, int numbones, const int *parents, const rdpVec *bonePos )
{
	int boneP[RDP_MAX_BONES];   // particle of the bone itself, or -1

	b->human = false;
	b->np = 0;
	b->nl = 0;

	// a particle for every bone that sits apart from its parent; bones on top of
	// their parent (twist helpers, roots) share the parent's particle
	for( int i = 0; i < numbones; i++ )
	{
		boneP[i] = -1;
		int par = parents[i];

		int pp = -1;
		for( int k = par; k >= 0 && pp < 0; k = parents[k] )
			pp = boneP[k];

		if( pp >= 0 && ( bonePos[i] - b->restP[pp] ).Length() < 2.5f )
			continue;

		if( b->np >= RDP_MAX_P )
			continue;

		int n = b->np++;
		boneP[i] = n;
		b->bone[n] = i;
		b->restP[n] = bonePos[i];
		b->parent[n] = pp;
		b->axis[n] = -1;
		b->ref[n] = -1;
	}

	if( b->np < 2 )
		return false;

	// the creature's size sets particle sizes and the core
	rdpVec mins = b->restP[0], maxs = b->restP[0];
	for( int n = 1; n < b->np; n++ )
	{
		for( int a = 0; a < 3; a++ )
		{
			mins[a] = RD_Min( mins[a], b->restP[n][a] );
			maxs[a] = RD_Max( maxs[a], b->restP[n][a] );
		}
	}
	float extent = ( maxs - mins ).Length();
	float coreRadius = RD_Max( 6.0f, extent * 0.22f );

	// the core: the root and everything close to it is one rigid body
	bool core[RDP_MAX_P];
	for( int n = 0; n < b->np; n++ )
	{
		core[n] = ( n == 0 ) || ( b->restP[n] - b->restP[0] ).Length() <= coreRadius;
		b->radius[n] = core[n] ? RD_Max( 2.5f, extent * 0.08f ) : RD_Max( 1.5f, extent * 0.04f );
	}

	for( int n = 0; n < b->np; n++ )
	{
		int par = b->parent[n];
		if( par >= 0 )
			RD_Link( b, par, n, true );

		// a joint bends at most to 40 degrees inside
		if( par >= 0 && b->parent[par] >= 0 && !core[n] )
			RD_BendLimit( b, b->parent[par], par, n, 40.0f );
	}

	for( int a = 0; a < b->np; a++ )
		for( int c = a + 1; c < b->np; c++ )
			if( core[a] && core[c] && b->parent[c] != a && b->parent[a] != c )
				RD_Link( b, a, c, false );

	// the core turns as one: the root's frame from two other core particles
	int ref1 = -1, ref2 = -1;
	for( int n = 1; n < b->np; n++ )
	{
		if( !core[n] ) continue;
		if( ref1 < 0 ) { ref1 = n; continue; }
		rdpVec d1 = ( b->restP[ref1] - b->restP[0] ).Normalize();
		rdpVec d2 = ( b->restP[n] - b->restP[0] ).Normalize();
		if( RDP_Cross( d1, d2 ).Length() > 0.3f ) { ref2 = n; break; }
	}

	// every other particle's bone points at its first child
	for( int n = 0; n < b->np; n++ )
	{
		for( int c = 0; c < b->np; c++ )
		{
			if( b->parent[c] == n && !( core[n] && core[c] ))
			{
				b->axis[n] = c;
				break;
			}
		}
	}

	if( ref1 >= 0 && ref2 >= 0 )
	{
		b->axis[0] = ref1;
		b->ref[0] = ref2;
	}
	else if( ref1 >= 0 )
	{
		b->axis[0] = ref1;
	}

	// core particles turn with the root
	for( int n = 1; n < b->np; n++ )
	{
		if( core[n] )
		{
			b->axis[n] = -1;
			b->ref[n] = -1;
			b->parent[n] = 0;
		}
	}

	return true;
}

bool RDP_Build( rdpBody *b, int numbones, const char *const *names, const int *parents, const rdpVec *bonePos )
{
	memset( b, 0, sizeof( *b ));

	if( numbones <= 0 || numbones > RDP_MAX_BONES )
		return false;

	if( !RD_BuildHuman( b, numbones, names, bonePos ))
	{
		memset( b, 0, sizeof( *b ));
		if( !RD_BuildGeneric( b, numbones, parents, bonePos ))
			return false;
	}

	for( int n = 0; n < b->np; n++ )
		b->p[n] = b->o[n] = b->safe[n] = b->restP[n];

	b->kneeSide = 1.0f;
	b->moving = true;
	return true;
}

/*
================
simulation
================
*/
static rdpVec RD_Forward( const rdpVec *p )
{
	rdpVec up = p[J_CHEST] - p[J_PELVIS];
	rdpVec right = p[J_RHIP] - p[J_LHIP];
	rdpVec f = RDP_Cross( up, right );
	return f.Length() > 0.001f ? f.Normalize() : rdpVec( 1, 0, 0 );
}

static rdpVec RD_JointOffset( const rdpVec *p, int a, int mid, int c )
{
	rdpVec line = p[c] - p[a];
	float len2 = RDP_Dot( line, line );
	if( len2 < 0.001f )
		return rdpVec( 0, 0, 0 );
	float t = RDP_Dot( p[mid] - p[a], line ) / len2;
	return p[mid] - ( p[a] + line * t );
}

void RDP_Throw( rdpBody *b, const rdpVec &hit, const rdpVec &push, const rdpVec &vel )
{
	if( b->human )
	{
		rdpVec fwd = RD_Forward( b->restP );
		float side = RDP_Dot( RD_JointOffset( b->restP, J_LHIP, J_LKNEE, J_LFOOT ), fwd )
			+ RDP_Dot( RD_JointOffset( b->restP, J_RHIP, J_RKNEE, J_RFOOT ), fwd );
		b->kneeSide = side < -0.5f ? -1.0f : 1.0f;
	}

	// most of the push where it hit, passed on through the body with distance
	for( int n = 0; n < b->np; n++ )
	{
		float d = ( b->p[n] - hit ).Length();
		float w = 0.3f + 0.9f * expf( -d / 14.0f );
		rdpVec v = vel + push * w;
		b->o[n] = b->p[n] - v * RDP_STEP;
	}

	b->moving = true;
	b->awake = 0.0f;
	b->restTime = 0.0f;
}

void RDP_Impulse( rdpBody *b, const rdpVec &hit, const rdpVec &push, float reach )
{
	for( int n = 0; n < b->np; n++ )
	{
		float d = ( b->p[n] - hit ).Length();
		float w = 0.1f + 0.9f * expf( -d / reach );
		b->o[n] = b->o[n] - push * ( w * RDP_STEP );
	}

	b->moving = true;
	b->awake = 0.0f;
	b->restTime = 0.0f;
}

static void RD_Collide( rdpBody *b, int n )
{
	rdpTrace tr;
	rdpVec from = b->safe[n], to = b->p[n];

	b->contact[n] = false;

	if(( to - from ).Length() > 0.01f )
	{
		RDP_Trace( from, to, &tr );

		if( tr.allsolid )
		{
			b->p[n] = b->o[n] = from;
			return;
		}

		if( tr.fraction < 1.0f )
		{
			rdpVec nrm = tr.normal;
			rdpVec hit = tr.end + nrm * 0.25f;
			rdpVec v = b->p[n] - b->o[n];
			rdpVec vt = v - nrm * RDP_Dot( v, nrm );

			b->p[n] = hit;
			b->o[n] = hit - vt * RD_GROUND_KEEP;
			b->contact[n] = true;
		}
	}

	// keep the particle's own thickness off whatever is under it
	if( b->radius[n] > 0.5f )
	{
		rdpVec down = b->p[n] - rdpVec( 0.0f, 0.0f, b->radius[n] );
		RDP_Trace( b->p[n], down, &tr );
		if( !tr.startsolid && tr.fraction < 1.0f )
		{
			float lift = b->radius[n] * ( 1.0f - tr.fraction );
			b->p[n].z += lift;
			b->o[n].z = b->p[n].z;   // resting: no speed into the floor, no bounce
			// sliding along the floor: friction, gentler than an impact
			rdpVec v = b->p[n] - b->o[n];
			b->o[n].x = b->p[n].x - v.x * RD_SLIDE_KEEP;
			b->o[n].y = b->p[n].y - v.y * RD_SLIDE_KEEP;
			b->contact[n] = true;
		}
	}

	b->safe[n] = b->p[n];
}

static void RD_Constrain( rdpBody *b )
{
	for( int it = 0; it < RD_ITER; it++ )
	{
		for( int i = 0; i < b->nl; i++ )
		{
			rdpLink *l = &b->links[i];
			rdpVec &a = b->p[l->a];
			rdpVec &c = b->p[l->b];
			rdpVec d = c - a;
			float len = d.Length();

			if( len < 0.001f || ( len >= l->lo && len <= l->hi ))
				continue;

			float target = len < l->lo ? l->lo : l->hi;
			rdpVec corr = d * ( 0.5f * ( len - target ) / len );
			a = a + corr;
			c = c - corr;
		}

		// limbs keep out of each other and the torso
		for( int i = 0; i < b->np; i++ )
		{
			for( int j = i + 1; j < b->np; j++ )
			{
				float r = ( b->radius[i] + b->radius[j] ) * 0.8f;
				rdpVec d = b->p[j] - b->p[i];
				float len = d.Length();
				float rest = ( b->restP[j] - b->restP[i] ).Length();

				// neighbours that start out overlapping are allowed to
				if( len >= r || rest < r || len < 0.001f )
					continue;

				rdpVec corr = d * ( 0.25f * ( r - len ) / len );
				b->p[i] = b->p[i] - corr;
				b->p[j] = b->p[j] + corr;
			}
		}

		if( !b->human )
			continue;

		rdpVec fwd = RD_Forward( b->p );
		rdpVec up = ( b->p[J_CHEST] - b->p[J_PELVIS] ).Normalize();

		// knees bend the way they did at death (forward if straight): eased
		// back to straight, never flipped, a flip adds energy
		static const int legs[2][3] = { { J_LHIP, J_LKNEE, J_LFOOT }, { J_RHIP, J_RKNEE, J_RFOOT } };
		for( int k = 0; k < 2; k++ )
		{
			rdpVec off = RD_JointOffset( b->p, legs[k][0], legs[k][1], legs[k][2] );
			float side = RDP_Dot( off, fwd ) * b->kneeSide;
			if( side < 0.0f )
				b->p[legs[k][1]] = b->p[legs[k][1]] - fwd * ( 0.5f * side * b->kneeSide );
		}

		// elbows point back while the arm hangs
		static const int arms[2][3] = { { J_LSHOULDER, J_LELBOW, J_LHAND }, { J_RSHOULDER, J_RELBOW, J_RHAND } };
		for( int k = 0; k < 2; k++ )
		{
			rdpVec upper = ( b->p[arms[k][1]] - b->p[arms[k][0]] ).Normalize();
			if( RDP_Dot( upper, up ) > -0.3f )
				continue;
			rdpVec off = RD_JointOffset( b->p, arms[k][0], arms[k][1], arms[k][2] );
			float side = RDP_Dot( off, fwd );
			if( side > 0.0f )
				b->p[arms[k][1]] = b->p[arms[k][1]] - fwd * ( 0.5f * side );
		}

		// legs swing forward freely, only a little backward (~35 degrees)
		static const int hips[2][2] = { { J_LHIP, J_LKNEE }, { J_RHIP, J_RKNEE } };
		for( int k = 0; k < 2; k++ )
		{
			rdpVec thigh = b->p[hips[k][1]] - b->p[hips[k][0]];
			float len = thigh.Length();
			if( len < 0.001f )
				continue;
			rdpVec dir = thigh / len;
			float back = -RDP_Dot( dir, fwd );
			if( back > 0.57f )
			{
				rdpVec fix = dir + fwd * ( 0.5f * ( back - 0.57f ));
				b->p[hips[k][1]] = b->p[hips[k][0]] + fix.Normalize() * len;
			}
		}
	}
}

void RDP_Step( rdpBody *b )
{
	float moved = 0.0f;

	b->awake += RDP_STEP;

	// joints are a little stiff: linked particles share some of their speed
	for( int i = 0; i < b->nl; i++ )
	{
		rdpLink *l = &b->links[i];
		if( !l->limb )
			continue;
		rdpVec va = b->p[l->a] - b->o[l->a];
		rdpVec vc = b->p[l->b] - b->o[l->b];
		rdpVec dv = ( vc - va ) * RD_JOINT_FRIC;
		b->o[l->a] = b->o[l->a] - dv;
		b->o[l->b] = b->o[l->b] + dv;
	}

	// once it has landed, the body winds down instead of fidgeting forever
	float settle = b->awake < 1.5f ? 0.0f : RD_Min(( b->awake - 1.5f ) / 2.0f, 1.0f );

	for( int n = 0; n < b->np; n++ )
	{
		rdpVec cur = b->p[n];
		float keep = b->contact[n] ? RD_CONTACT_KEEP : RD_AIR_DRAG;
		keep *= 1.0f - 0.25f * settle;
		rdpVec v = ( b->p[n] - b->o[n] ) * keep;

		b->o[n] = cur;
		b->p[n] = cur + v + rdpVec( 0.0f, 0.0f, -RD_GRAVITY * RDP_STEP * RDP_STEP );
	}

	// hold together, then settle on the world: the world has the last word
	RD_Constrain( b );
	for( int n = 0; n < b->np; n++ )
		RD_Collide( b, n );

	for( int n = 0; n < b->np; n++ )
		moved = RD_Max( moved, ( b->p[n] - b->o[n] ).Length());

	if( moved < RD_SETTLE )
		b->restTime += RDP_STEP;
	else b->restTime = 0.0f;

	if( b->restTime > RD_REST_TIME || b->awake > RD_MAX_TIME )
	{
		b->moving = false;
		for( int n = 0; n < b->np; n++ )
			b->o[n] = b->p[n];
	}
}

int RDP_Nearest( const rdpBody *b, const rdpVec &pt )
{
	int best = 0;
	float bestD = 1e9f;
	for( int n = 0; n < b->np; n++ )
	{
		float d = ( b->p[n] - pt ).Length();
		if( d < bestD ) { bestD = d; best = n; }
	}
	return best;
}

// closest points between segments p1-q1 and p2-q2: returns squared distance, s along the first
static float RD_SegSeg( const rdpVec &p1, const rdpVec &q1, const rdpVec &p2, const rdpVec &q2, float *sOut, float *tOut )
{
	rdpVec d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
	float a = RDP_Dot( d1, d1 ), e = RDP_Dot( d2, d2 ), f = RDP_Dot( d2, r );
	float s, t;

	if( a < 1e-6f && e < 1e-6f ) { s = t = 0; }
	else if( a < 1e-6f ) { s = 0; t = RD_Min( RD_Max( f / e, 0.0f ), 1.0f ); }
	else
	{
		float c = RDP_Dot( d1, r );
		if( e < 1e-6f ) { t = 0; s = RD_Min( RD_Max( -c / a, 0.0f ), 1.0f ); }
		else
		{
			float bb = RDP_Dot( d1, d2 ), denom = a * e - bb * bb;
			s = denom > 1e-6f ? RD_Min( RD_Max(( bb * f - c * e ) / denom, 0.0f ), 1.0f ) : 0.0f;
			t = ( bb * s + f ) / e;
			if( t < 0 ) { t = 0; s = RD_Min( RD_Max( -c / a, 0.0f ), 1.0f ); }
			else if( t > 1 ) { t = 1; s = RD_Min( RD_Max(( bb - c ) / a, 0.0f ), 1.0f ); }
		}
	}

	rdpVec c1 = p1 + d1 * s, c2 = p2 + d2 * t;
	*sOut = s;
	*tOut = t;
	return RDP_Dot( c1 - c2, c1 - c2 );
}

bool RDP_RayHit( const rdpBody *b, const rdpVec &from, const rdpVec &to, float *frac, rdpVec *hitPos, int *particle )
{
	float best = 2.0f;
	int bestP = -1;
	float rayLen = ( to - from ).Length();

	if( rayLen < 0.001f )
		return false;

	// every bone as a capsule from its particle to its parent's
	for( int n = 0; n < b->np; n++ )
	{
		int par = b->parent[n] >= 0 ? b->parent[n] : n;
		float r = RD_Max( b->radius[n], b->radius[par] ) * 1.1f;
		float s, t;
		float d2 = RD_SegSeg( from, to, b->p[par], b->p[n], &s, &t );

		if( d2 > r * r )
			continue;

		// step back from the closest point to where the ray enters the capsule
		float back = sqrtf( RD_Max( 0.0f, r * r - d2 )) / rayLen;
		float hit = RD_Max( 0.0f, s - back );
		if( hit < best )
		{
			best = hit;
			bestP = t < 0.5f ? par : n;
		}
	}

	if( bestP < 0 )
		return false;

	*frac = best;
	*hitPos = from + ( to - from ) * best;
	*particle = bestP;
	return true;
}
