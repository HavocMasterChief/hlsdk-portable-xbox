/*
xb_rdphys.h - Xbox build: ragdoll physics core
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Particles with a size each (Verlet), held together by links. Humans (Bip01
skeletons) get a hand-made body: 15 joints, a braced torso, hinge knees and
elbows, a spine and neck that bend only so far. Anything else gets a body
built from its own bones: a rigid core and limbs that bend only so far.

Run by the game dll (dlls/xb_ragdoll_sv.cpp), so bullets, the player and
explosions can touch the bodies; the client only draws them
(cl_dll/xb_ragdoll.cpp). No engine headers here: the world is reached
through the trace callback.
*/

#pragma once
#ifndef XB_RDPHYS_H
#define XB_RDPHYS_H

#define RDP_MAX_P      24
#define RDP_MAX_LINKS  120
#define RDP_MAX_BONES  128
#define RDP_STEP       ( 1.0f / 60.0f )

struct rdpVec
{
	float x, y, z;
	rdpVec() : x( 0 ), y( 0 ), z( 0 ) { }
	rdpVec( float a, float b, float c ) : x( a ), y( b ), z( c ) { }
	rdpVec operator+( const rdpVec &v ) const { return rdpVec( x + v.x, y + v.y, z + v.z ); }
	rdpVec operator-( const rdpVec &v ) const { return rdpVec( x - v.x, y - v.y, z - v.z ); }
	rdpVec operator*( float f ) const { return rdpVec( x * f, y * f, z * f ); }
	rdpVec operator/( float f ) const { return rdpVec( x / f, y / f, z / f ); }
	float operator[]( int i ) const { return i == 0 ? x : ( i == 1 ? y : z ); }
	float &operator[]( int i ) { return i == 0 ? x : ( i == 1 ? y : z ); }
	float Length() const;
	rdpVec Normalize() const;
};

float RDP_Dot( const rdpVec &a, const rdpVec &b );
rdpVec RDP_Cross( const rdpVec &a, const rdpVec &b );

struct rdpTrace
{
	float fraction;
	rdpVec end, normal;
	bool startsolid, allsolid;
};

// the world: a point trace, monsters ignored
typedef void (*rdpTraceFn)( const rdpVec &from, const rdpVec &to, rdpTrace *tr );
extern rdpTraceFn RDP_Trace;

struct rdpLink
{
	unsigned char a, b;
	float lo, hi;           // allowed length range
	bool limb;              // shares speed (joint friction)
};

struct rdpBody
{
	bool human, moving;
	int np;
	// per particle: the bone it sits on, the particle it hangs from, the one
	// its bone points at, and (rigid parts) a third one fixing its twist
	int bone[RDP_MAX_P];
	signed char parent[RDP_MAX_P], axis[RDP_MAX_P], ref[RDP_MAX_P];
	float radius[RDP_MAX_P];
	rdpVec p[RDP_MAX_P], o[RDP_MAX_P], safe[RDP_MAX_P], restP[RDP_MAX_P];
	bool contact[RDP_MAX_P];

	int nl;
	rdpLink links[RDP_MAX_LINKS];

	float kneeSide;
	float awake;            // seconds since last woken
	float restTime;
};

// build from the skeleton at the moment of death; names may be NULL for unnamed bones
bool RDP_Build( rdpBody *b, int numbones, const char *const *names, const int *parents, const rdpVec *bonePos );

// set every particle moving: vel for all, push mostly where it hit
void RDP_Throw( rdpBody *b, const rdpVec &hit, const rdpVec &push, const rdpVec &vel );

// a hit on a body: add speed around the hit point, wake it
void RDP_Impulse( rdpBody *b, const rdpVec &hit, const rdpVec &push, float reach );

// one physics step; clears b->moving once it has settled
void RDP_Step( rdpBody *b );

// closest particle to a point, and the distance from a ray to the body
int RDP_Nearest( const rdpBody *b, const rdpVec &pt );
bool RDP_RayHit( const rdpBody *b, const rdpVec &from, const rdpVec &to, float *frac, rdpVec *hitPos, int *particle );

#endif // XB_RDPHYS_H
