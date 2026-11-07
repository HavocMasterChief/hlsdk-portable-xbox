/*
xb_gore.h - Xbox build blood modes and Extreme gore (server side)
Copyright (C) 2026 HavocMasterChief

Ported from RealRTCWX cg_gore.c. See xb_gore.cpp.
*/
#pragma once
#if !defined( XB_GORE_H )
#define XB_GORE_H

#if XASH_UWP

// xb_blood cvar values
enum
{
	XB_BLOOD_OFF = 0,
	XB_BLOOD_DEFAULT,
	XB_BLOOD_GREEN,
	XB_BLOOD_EXTREME,
};

void XBGore_RegisterCvars( void );
int  XBGore_Mode( void );
int  XBGore_Color( int color );          // Green mode: human blood becomes alien coloured
bool XBGore_Extreme( void );

class CBaseEntity;
class CBaseMonster;

void XBGore_SurfaceHit( TraceResult *tr, int color );   // a blood decal landed on a wall or ceiling
void XBGore_Wounded( CBaseMonster *pMonster, const Vector &point, int hitgroup, float flDamage );
void XBGore_Killed( CBaseMonster *pMonster );
void XBGore_Gibbed( CBaseMonster *pMonster );
void XBGore_Frame( void );               // from StartFrame
void XBGore_LevelStart( void );

void XBRagdoll_Send( CBaseMonster *pMonster );   // body becomes a ragdoll (cl_dll/xb_ragdoll.cpp)

extern int gmsgXBGore;
extern int gmsgXBRagdoll;                   // screen splats (cl_dll/xb_gore_hud.cpp)

#endif // XASH_UWP
#endif // XB_GORE_H
