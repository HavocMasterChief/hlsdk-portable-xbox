/*
xb_grenade.cpp - Xbox build: quick grenade on RB
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Press RB: the fuse starts. Release: a grenade is tossed from the eyes, no
weapon switch and no animation (the game does it: dlls/player.cpp
CBasePlayer::XBGrenade). Held over three seconds it goes off on release.
Opening the weapon wheel while RB is held calls it off. No grenades: a click.
*/

#include <string.h>
#include "hud.h"
#include "cl_util.h"
#include "ammohistory.h"

#if XASH_UWP

#define WEAPON_HANDGRENADE 12

static bool s_held;

static bool HaveGrenade( void )
{
	if( !( gHUD.m_iWeaponBits & ( 1 << WEAPON_HANDGRENADE )))
		return false;
	WEAPON *p = gWR.GetWeapon( WEAPON_HANDGRENADE );
	return p && p->iId == WEAPON_HANDGRENADE && gWR.CountAmmo( p->iAmmoType ) > 0;
}

static void Grenade_Down( void )
{
	if( s_held || gHUD.m_Health.m_iHealth <= 0 )
		return;

	if( !HaveGrenade( ))
	{
		PlaySound( "common/wpn_denyselect.wav", 1.0f );
		return;
	}

	s_held = true;
	ServerCmd( "xbgrenade start" );
}

static void Grenade_Up( void )
{
	if( !s_held )
		return;

	s_held = false;
	ServerCmd( "xbgrenade throw" );
}

void XBGrenade_Init( void )
{
	gEngfuncs.pfnAddCommand( "+xbgrenade", Grenade_Down );
	gEngfuncs.pfnAddCommand( "-xbgrenade", Grenade_Up );
}

void XBGrenade_Reset( void )
{
	s_held = false;
}

void XBGrenade_Frame( void )
{
	// the weapon wheel calls it off
	if( s_held && gEngfuncs.pfnGetCvarFloat( "xb_wheel_open" ) != 0.0f )
	{
		s_held = false;
		ServerCmd( "xbgrenade cancel" );
	}
}

#endif // XASH_UWP
