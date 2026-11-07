/*
xb_wheel.cpp - Xbox build: weapon wheel on LB
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

GTA-style wheel, like RealRTCWX's: hold LB and the game slows down while
the five weapon categories (Half-Life's weapon slots) sit in a ring. Point
the right stick at a category, D-pad left/right cycles the weapons in it,
let go of LB to switch.

The engine (engine/client/input/in_joy.c) does the controller side: while
xb_wheel_open is 1 it writes the right stick to xb_wheel_x/y instead of
turning the view, and sends D-pad left/right here as xb_wheel_cycle.
*/

#include "hud.h"
#include "cl_util.h"
#include "ammohistory.h"
#include "cdll_dll.h"

#if XASH_UWP

extern int g_weaponselect;

static CHudXBWheel *s_wheel;

static const char *s_slotNames[MAX_WEAPON_SLOTS] = { "Melee", "Pistols", "Primary", "Heavy", "Explosives" };

static const struct { const char *cls, *name; } s_weaponNames[] =
{
	{ "weapon_crowbar", "Crowbar" },
	{ "weapon_9mmhandgun", "9mm Pistol" },
	{ "weapon_357", ".357 Magnum" },
	{ "weapon_9mmAR", "MP5" },
	{ "weapon_shotgun", "Shotgun" },
	{ "weapon_crossbow", "Crossbow" },
	{ "weapon_rpg", "Rocket Launcher" },
	{ "weapon_gauss", "Tau Cannon" },
	{ "weapon_egon", "Gluon Gun" },
	{ "weapon_hornetgun", "Hivehand" },
	{ "weapon_handgrenade", "Grenades" },
	{ "weapon_satchel", "Satchel Charges" },
	{ "weapon_tripmine", "Tripmines" },
	{ "weapon_snark", "Snarks" },
};

static const char *XB_WeaponName( WEAPON *w )
{
	for( size_t i = 0; i < sizeof( s_weaponNames ) / sizeof( s_weaponNames[0] ); i++ )
		if( !_stricmp( w->szName, s_weaponNames[i].cls ))
			return s_weaponNames[i].name;

	return !strncmp( w->szName, "weapon_", 7 ) ? w->szName + 7 : w->szName;
}

static void XB_WheelDown( void ) { if( s_wheel ) s_wheel->Open(); }
static void XB_WheelUp( void ) { if( s_wheel ) s_wheel->Close( true ); }
static void XB_WheelCycle( void ) { if( s_wheel ) s_wheel->Cycle( atoi( gEngfuncs.Cmd_Argv( 1 )) < 0 ? -1 : 1 ); }

int CHudXBWheel::Init( void )
{
	s_wheel = this;
	m_bOpen = false;
	m_iFlags = 0;

	m_pOpen = gEngfuncs.pfnRegisterVariable( "xb_wheel_open", "0", 0 );
	m_pStickX = gEngfuncs.pfnRegisterVariable( "xb_wheel_x", "0", 0 );
	m_pStickY = gEngfuncs.pfnRegisterVariable( "xb_wheel_y", "0", 0 );
	m_pSlowmo = gEngfuncs.pfnRegisterVariable( "xb_wheel_slowmo", "1", FCVAR_ARCHIVE );

	gEngfuncs.pfnAddCommand( "+xbwheel", XB_WheelDown );
	gEngfuncs.pfnAddCommand( "-xbwheel", XB_WheelUp );
	gEngfuncs.pfnAddCommand( "xb_wheel_cycle", XB_WheelCycle );

	gHUD.AddHudElem( this );
	return 1;
}

int CHudXBWheel::VidInit( void )
{
	Close( false );
	return 1;
}

bool CHudXBWheel::SlotHasWeapons( int slot )
{
	for( int pos = 0; pos < MAX_WEAPON_POSITIONS; pos++ )
		if( gWR.GetWeaponSlot( slot, pos ))
			return true;
	return false;
}

WEAPON *CHudXBWheel::Shown( int slot )
{
	WEAPON *w = gWR.GetWeaponSlot( slot, m_iPos[slot] );

	if( w )
		return w;

	// first owned weapon in the slot
	for( int pos = 0; pos < MAX_WEAPON_POSITIONS; pos++ )
	{
		if(( w = gWR.GetWeaponSlot( slot, pos )))
		{
			m_iPos[slot] = pos;
			return w;
		}
	}
	return NULL;
}

void CHudXBWheel::Open( void )
{
	if( m_bOpen || gHUD.m_Health.m_iHealth <= 0 || gHUD.m_iIntermission || ( gHUD.m_iHideHUDDisplay & ( HIDEHUD_WEAPONS | HIDEHUD_ALL )))
		return;

	bool any = false;
	for( int s = 0; s < MAX_WEAPON_SLOTS; s++ )
	{
		m_iPos[s] = 0;
		if( SlotHasWeapons( s ))
			any = true;
	}
	if( !any )
		return;

	// start on the weapon in hand
	m_iSlot = 0;
	WEAPON *cur = gHUD.m_Ammo.XB_CurrentWeapon();
	if( cur )
	{
		m_iSlot = cur->iSlot;
		m_iPos[cur->iSlot] = cur->iSlotPos;
	}

	m_bOpen = true;
	m_iFlags |= HUD_ACTIVE;
	gEngfuncs.Cvar_SetValue( "xb_wheel_open", 1 );
	gEngfuncs.Cvar_SetValue( "xb_wheel_x", 0 );
	gEngfuncs.Cvar_SetValue( "xb_wheel_y", 0 );

	if( m_pSlowmo->value )
		gEngfuncs.Cvar_SetValue( "sys_timescale", 0.3f );
}

void CHudXBWheel::Close( bool select )
{
	if( !m_bOpen )
		return;

	m_bOpen = false;
	m_iFlags &= ~HUD_ACTIVE;
	gEngfuncs.Cvar_SetValue( "xb_wheel_open", 0 );
	gEngfuncs.Cvar_SetValue( "sys_timescale", 1.0f );

	if( !select || gHUD.m_Health.m_iHealth <= 0 )
		return;

	WEAPON *w = Shown( m_iSlot );
	WEAPON *cur = gHUD.m_Ammo.XB_CurrentWeapon();

	if( w && w != cur )
	{
		ServerCmd( w->szName );
		g_weaponselect = w->iId;
	}
}

void CHudXBWheel::Cycle( int dir )
{
	if( !m_bOpen )
		return;

	for( int n = 1; n <= MAX_WEAPON_POSITIONS; n++ )
	{
		int pos = ( m_iPos[m_iSlot] + dir * n + MAX_WEAPON_POSITIONS * 2 ) % MAX_WEAPON_POSITIONS;

		if( gWR.GetWeaponSlot( m_iSlot, pos ))
		{
			m_iPos[m_iSlot] = pos;
			PlaySound( "common/wpn_moveselect.wav", 1 );
			return;
		}
	}
}

int CHudXBWheel::Draw( float flTime )
{
	if( !m_bOpen )
		return 1;

	// lost the right to have it open (died, cutscene)
	if( gHUD.m_Health.m_iHealth <= 0 || ( gHUD.m_iHideHUDDisplay & ( HIDEHUD_WEAPONS | HIDEHUD_ALL )))
	{
		Close( false );
		return 1;
	}

	const float pi = 3.14159265f;
	int cx = ScreenWidth / 2, cy = ScreenHeight / 2;
	float radius = ScreenHeight * 0.26f;

	// right stick picks the category; slot 0 at the top, clockwise
	float sx = m_pStickX->value, sy = m_pStickY->value;
	if( sx * sx + sy * sy > 0.45f * 0.45f )
	{
		float ang = atan2( sx, -sy ); // 0 = up, clockwise
		if( ang < 0 ) ang += 2.0f * pi;
		int slot = (int)( ang / ( 2.0f * pi / MAX_WEAPON_SLOTS ) + 0.5f ) % MAX_WEAPON_SLOTS;

		if( slot != m_iSlot && SlotHasWeapons( slot ))
		{
			m_iSlot = slot;
			PlaySound( "common/wpn_hudoff.wav", 1 );
		}
	}

	// dim the screen behind the wheel
	FillRGBA( 0, 0, ScreenWidth, ScreenHeight, 0, 0, 0, 90 );

	for( int s = 0; s < MAX_WEAPON_SLOTS; s++ )
	{
		float ang = s * ( 2.0f * pi / MAX_WEAPON_SLOTS );
		int x = cx + (int)( sin( ang ) * radius );
		int y = cy - (int)( cos( ang ) * radius );
		bool hovered = ( s == m_iSlot );
		int boxW = 190, boxH = 70;
		int r, g, b;

		UnpackRGB( r, g, b, RGB_YELLOWISH );

		FillRGBA( x - boxW / 2, y - boxH / 2, boxW, boxH, hovered ? r : 30, hovered ? g / 3 : 30, hovered ? 0 : 30, hovered ? 110 : 140 );

		WEAPON *w = Shown( s );
		if( !w )
		{
			gHUD.DrawHudString( x - 40, y - 8, x + 90, s_slotNames[s], 90, 90, 90 );
			continue;
		}

		HSPRITE spr = hovered ? w->hActive : w->hInactive;
		wrect_t *rc = hovered ? &w->rcActive : &w->rcInactive;
		int iw = rc->right - rc->left, ih = rc->bottom - rc->top;

		if( spr )
		{
			ScaleColors( r, g, b, hovered ? 255 : 150 );
			SPR_Set( spr, r, g, b );
			SPR_DrawAdditive( 0, x - iw / 2, y - ih / 2 - 6, rc );
		}

		// how many weapons this category holds
		int count = 0;
		for( int pos = 0; pos < MAX_WEAPON_POSITIONS; pos++ )
			if( gWR.GetWeaponSlot( s, pos )) count++;

		if( count > 1 )
		{
			char buf[16];
			snprintf( buf, sizeof( buf ), "+%d", count - 1 );
			gHUD.DrawHudString( x + boxW / 2 - 30, y - boxH / 2 + 2, x + boxW / 2, buf, 200, 200, 200 );
		}
	}

	// name of the selected weapon in the middle, with the D-pad hint
	WEAPON *sel = Shown( m_iSlot );
	if( sel )
	{
		const char *name = XB_WeaponName( sel );
		int len = gHUD.DrawHudStringLen( name );
		gHUD.DrawHudString( cx - len / 2, cy - 12, cx + len, name, 255, 220, 120 );

		const char *hint = "D-pad < >  more in this slot";
		len = gHUD.DrawHudStringLen( hint );
		gHUD.DrawHudString( cx - len / 2, cy + 12, cx + len, hint, 160, 160, 160 );
	}

	return 1;
}

#endif // XASH_UWP
