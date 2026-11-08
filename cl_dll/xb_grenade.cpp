/*
xb_grenade.cpp - Xbox build: quick grenade on RB
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Hold RB: out comes a hand grenade and the pin is pulled (it cooks, as
usual). Let go: it's thrown, and a moment later the weapon you had is back.
Opening the weapon wheel (LB) while RB is held puts the pin back
(xbgrenade_cancel, dlls/client.cpp) and returns to your weapon.
No grenades: a click and nothing else.
*/

#include <string.h>
#include "hud.h"
#include "cl_util.h"
#include "ammohistory.h"

#if XASH_UWP

#define WEAPON_HANDGRENADE 12
#define GREN_DEPLOY        0.55f   // v_grenade draw
#define GREN_SWITCH_BACK   1.1f    // after the throw

enum { G_IDLE, G_DRAWING, G_PRIMED, G_THROWN, G_CANCELLING };

static int s_state = G_IDLE;
static bool s_held;
static float s_time;
static char s_prev[32];

static bool HaveGrenade( void )
{
	if( !( gHUD.m_iWeaponBits & ( 1 << WEAPON_HANDGRENADE )))
		return false;
	WEAPON *p = gWR.GetWeapon( WEAPON_HANDGRENADE );
	return p && p->iId == WEAPON_HANDGRENADE && gWR.CountAmmo( p->iAmmoType ) > 0;
}

static bool HoldingGrenade( void )
{
	WEAPON *cur = gHUD.m_Ammo.XB_CurrentWeapon();
	return cur && cur->iId == WEAPON_HANDGRENADE;
}

static void Grenade_Down( void )
{
	if( s_state != G_IDLE || gHUD.m_Health.m_iHealth <= 0 )
		return;

	if( !HaveGrenade( ))
	{
		PlaySound( "common/wpn_denyselect.wav", 1.0f );
		return;
	}

	WEAPON *cur = gHUD.m_Ammo.XB_CurrentWeapon();
	s_prev[0] = 0;
	if( cur && cur->iId != WEAPON_HANDGRENADE )
		strncpy( s_prev, cur->szName, sizeof( s_prev ) - 1 );

	s_held = true;
	s_time = gEngfuncs.GetClientTime();

	if( HoldingGrenade( ))
	{
		gEngfuncs.pfnClientCmd( "+attack\n" );
		s_state = G_PRIMED;
	}
	else
	{
		ServerCmd( "weapon_handgrenade" );
		s_state = G_DRAWING;
	}
}

static void Grenade_Up( void )
{
	s_held = false;

	if( s_state == G_PRIMED )
	{
		gEngfuncs.pfnClientCmd( "-attack\n" );  // the grenade goes
		s_state = G_THROWN;
		s_time = gEngfuncs.GetClientTime();
	}
	// still drawing: thrown as soon as it's out (XBGrenade_Frame)
}

void XBGrenade_Init( void )
{
	gEngfuncs.pfnAddCommand( "+xbgrenade", Grenade_Down );
	gEngfuncs.pfnAddCommand( "-xbgrenade", Grenade_Up );
}

void XBGrenade_Reset( void )
{
	s_state = G_IDLE;
	s_held = false;
}

static void BackToWeapon( void )
{
	if( s_prev[0] )
		ServerCmd( s_prev );
	s_state = G_IDLE;
}

void XBGrenade_Frame( void )
{
	if( s_state == G_IDLE )
		return;

	float now = gEngfuncs.GetClientTime();

	// dead or the level changed: let go of everything
	if( gHUD.m_Health.m_iHealth <= 0 || now < s_time )
	{
		gEngfuncs.pfnClientCmd( "-attack\n" );
		XBGrenade_Reset();
		return;
	}

	// the weapon wheel cancels: pin back in, back to the weapon
	if(( s_state == G_DRAWING || s_state == G_PRIMED ) && s_held && gEngfuncs.pfnGetCvarFloat( "xb_wheel_open" ) != 0.0f )
	{
		ServerCmd( "xbgrenade_cancel" );
		s_state = G_CANCELLING;
		s_time = now;
		return;
	}

	switch( s_state )
	{
	case G_DRAWING:
		if( HoldingGrenade() && now - s_time >= GREN_DEPLOY )
		{
			gEngfuncs.pfnClientCmd( "+attack\n" );
			s_state = G_PRIMED;
			s_time = now;
			// already let go: a quick tap, thrown straight away
			if( !s_held )
				Grenade_Up();
		}
		else if( now - s_time > 2.0f )
		{
			// never came out (no grenades after all)
			BackToWeapon();
		}
		break;

	case G_THROWN:
		if( now - s_time >= GREN_SWITCH_BACK )
			BackToWeapon();
		break;

	case G_CANCELLING:
		// the game has put the pin back: now letting go can't throw
		if( now - s_time >= 0.1f )
		{
			gEngfuncs.pfnClientCmd( "-attack\n" );
			BackToWeapon();
		}
		break;
	}
}

#endif // XASH_UWP
