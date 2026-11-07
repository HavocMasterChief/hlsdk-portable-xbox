/*
xb_gore_hud.cpp - Xbox build: blood on the screen (Extreme blood mode)
Copyright (C) 2026 HavocMasterChief

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Half-Life version of RealRTCWX's CG_GoreScreenSplat/CG_DrawGoreScreen:
when something dies or gibs close in front of the player, the server
(dlls/xb_gore.cpp) sends "XBGore" with a count, and that many blood
splats land on the screen and fade over a couple of seconds.
Red only, like RTCW (Extreme is always red).
*/

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "triangleapi.h"
#include "com_model.h"
#include "r_efx.h"
#include "pm_defs.h"
#include "event_api.h"

#if XASH_UWP

/*
===============================================================================

Blood droplets (RealRTCWX CG_GoreSpray / CG_GoreDropLand)

The server sends "XBDrops" (dlls/xb_gore.cpp): a point, a direction (the
way the bullet was travelling, so blood is thrown out the far side), a
count, speed and spread. Each droplet is a small blood sprite that flies
under gravity; when it hits the world it leaves its own splat right there
and disappears. Bigger drops leave splats, the fine mist doesn't.

===============================================================================
*/
static struct model_s *s_dropModel;
static int s_dropDecals[2][6];   // red, yellow

static void XB_DropHit( struct tempent_s *te, struct pmtrace_s *ptr )
{
	// a drop is gone once it lands
	te->die = gEngfuncs.GetClientTime();
	te->entity.curstate.renderamt = 0;
	te->flags &= ~( FTENT_COLLIDEWORLD | FTENT_GRAVITY );

	// fuser1 marks drops big enough to leave a splat, iuser1 the colour
	if( te->entity.curstate.fuser1 <= 0.0f || !ptr )
		return;

	int set = te->entity.curstate.iuser1 ? 1 : 0;
	int decal = s_dropDecals[set][gEngfuncs.pfnRandomLong( 0, 5 )];

	if( decal <= 0 )
		return;

	int ent = gEngfuncs.pEventAPI->EV_IndexFromTrace( ptr );
	gEngfuncs.pEfxAPI->R_DecalShoot( gEngfuncs.pEfxAPI->Draw_DecalIndex( decal ), ent, 0, ptr->endpos, 0 );
}

static int MsgFunc_XBDrops( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	Vector org, dir;
	org.x = READ_COORD();
	org.y = READ_COORD();
	org.z = READ_COORD();
	dir.x = READ_CHAR() / 127.0f;
	dir.y = READ_CHAR() / 127.0f;
	dir.z = READ_CHAR() / 127.0f;
	int count = READ_BYTE();
	float speed = READ_BYTE() * 4.0f;
	float spread = READ_BYTE() / 100.0f;
	int yellow = READ_BYTE();
	int flags = READ_BYTE();

	if( !s_dropModel )
		return 1;

	bool sphere = ( flags & 1 ) || dir.Length() < 0.01f;

	for( int i = 0; i < count; i++ )
	{
		Vector d;

		if( sphere )
			d = Vector( gEngfuncs.pfnRandomFloat( -1, 1 ), gEngfuncs.pfnRandomFloat( -1, 1 ), gEngfuncs.pfnRandomFloat( -0.3f, 1.0f ));
		else
			d = dir + Vector( gEngfuncs.pfnRandomFloat( -spread, spread ), gEngfuncs.pfnRandomFloat( -spread, spread ), gEngfuncs.pfnRandomFloat( -spread, spread ));

		d = d.Normalize();

		// mix of heavy drops that carry and fine mist that falls short
		float big = gEngfuncs.pfnRandomFloat( 0.0f, 1.0f );
		float v = speed * ( 0.45f + 0.75f * gEngfuncs.pfnRandomFloat( 0.0f, 1.0f ));

		TEMPENTITY *te = gEngfuncs.pEfxAPI->CL_TempEntAlloc( org, s_dropModel );
		if( !te )
			break;

		te->entity.baseline.origin = d * v;   // tempent velocity
		te->entity.curstate.rendermode = kRenderTransAlpha;
		te->entity.curstate.renderamt = 230;
		te->entity.curstate.rendercolor.r = yellow ? 160 : 110;
		te->entity.curstate.rendercolor.g = yellow ? 140 : 0;
		te->entity.curstate.rendercolor.b = 0;
		te->entity.curstate.scale = 0.06f + 0.12f * big;
		te->entity.curstate.fuser1 = big > 0.45f ? 1.0f : 0.0f;
		te->entity.curstate.iuser1 = yellow;
		te->entity.curstate.frame = gEngfuncs.pfnRandomLong( 0, 3 );
		te->flags |= FTENT_COLLIDEWORLD | FTENT_GRAVITY | FTENT_COLLIDEKILL;
		te->hitcallback = XB_DropHit;
		te->die = gEngfuncs.GetClientTime() + 4.0f;
	}

	return 1;
}

static void XB_DropsVidInit( void )
{
	static const char *red[6] = { "{blood1", "{blood2", "{blood3", "{blood4", "{blood5", "{blood6" };
	static const char *yel[6] = { "{yblood1", "{yblood2", "{yblood3", "{yblood4", "{yblood5", "{yblood6" };
	int idx = 0;

	s_dropModel = gEngfuncs.CL_LoadModel( "sprites/blood.spr", &idx );

	for( int i = 0; i < 6; i++ )
	{
		s_dropDecals[0][i] = gEngfuncs.pEfxAPI->Draw_DecalIndexFromName( (char *)red[i] );
		s_dropDecals[1][i] = gEngfuncs.pEfxAPI->Draw_DecalIndexFromName( (char *)yel[i] );
	}
}

DECLARE_MESSAGE( m_XBGore, XBGore )

int CHudXBGore::Init( void )
{
	HOOK_MESSAGE( XBGore );
	gEngfuncs.pfnHookUserMsg( "XBDrops", MsgFunc_XBDrops );
	XBRagdoll_Init();

	memset( m_splats, 0, sizeof( m_splats ));
	m_iFlags = 0;

	gHUD.AddHudElem( this );
	return 1;
}

int CHudXBGore::VidInit( void )
{
	m_hSprite = SPR_Load( "sprites/bloodspray.spr" );
	XBRagdoll_Reset();
	XB_DropsVidInit();
	memset( m_splats, 0, sizeof( m_splats ));
	return 1;
}

int CHudXBGore::MsgFunc_XBGore( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	int count = READ_BYTE();

	for( int n = 0; n < count; n++ )
	{
		xbSplat_t *s = &m_splats[0];

		// reuse a free slot, else the oldest
		for( int i = 0; i < XB_MAX_SPLATS; i++ )
		{
			if( m_splats[i].start == 0.0f ) { s = &m_splats[i]; break; }
			if( m_splats[i].start < s->start ) s = &m_splats[i];
		}

		s->start = gHUD.m_flTime + n * 0.03f;
		s->life = gEngfuncs.pfnRandomFloat( 1.8f, 3.0f );
		s->x = gEngfuncs.pfnRandomFloat( 0.1f, 0.9f );
		s->y = gEngfuncs.pfnRandomFloat( 0.1f, 0.85f );
		s->size = gEngfuncs.pfnRandomFloat( 0.10f, 0.26f );
		s->angle = gEngfuncs.pfnRandomFloat( 0.0f, 6.2831853f );
		s->frame = gEngfuncs.pfnRandomLong( 0, 9 );
		s->slide = gEngfuncs.pfnRandomFloat( 0.0f, 0.05f );
	}

	m_iFlags |= HUD_ACTIVE;
	return 1;
}

int CHudXBGore::Draw( float flTime )
{
	if( !m_hSprite )
		return 1;

	const model_s *pSprite = gEngfuncs.GetSpritePointer( m_hSprite );
	if( !pSprite )
		return 1;

	int frames = SPR_Frames( m_hSprite );
	bool any = false;

	gEngfuncs.pTriAPI->RenderMode( kRenderTransAlpha );
	gEngfuncs.pTriAPI->CullFace( TRI_NONE );

	for( int i = 0; i < XB_MAX_SPLATS; i++ )
	{
		xbSplat_t *s = &m_splats[i];

		if( s->start == 0.0f )
			continue;

		float t = ( flTime - s->start ) / s->life;
		if( t < 0.0f )
		{
			any = true;
			continue;
		}
		if( t >= 1.0f )
		{
			s->start = 0.0f;
			continue;
		}

		any = true;

		// fade in quickly, hold, fade out; runs down the screen a little
		float alpha = t < 0.05f ? t / 0.05f : ( t > 0.6f ? ( 1.0f - t ) / 0.4f : 1.0f );
		float cx = s->x * ScreenWidth;
		float cy = ( s->y + s->slide * t ) * ScreenHeight;
		float r = s->size * ScreenHeight;
		float c = cos( s->angle ) * r, sn = sin( s->angle ) * r;

		gEngfuncs.pTriAPI->SpriteTexture( (model_s *)pSprite, frames > 0 ? s->frame % frames : 0 );
		gEngfuncs.pTriAPI->Color4f( 0.45f, 0.0f, 0.0f, 0.85f * alpha );
		gEngfuncs.pTriAPI->Begin( TRI_QUADS );
			gEngfuncs.pTriAPI->TexCoord2f( 0, 0 ); gEngfuncs.pTriAPI->Vertex3f( cx - c + sn, cy - sn - c, 0 );
			gEngfuncs.pTriAPI->TexCoord2f( 1, 0 ); gEngfuncs.pTriAPI->Vertex3f( cx + c + sn, cy + sn - c, 0 );
			gEngfuncs.pTriAPI->TexCoord2f( 1, 1 ); gEngfuncs.pTriAPI->Vertex3f( cx + c - sn, cy + sn + c, 0 );
			gEngfuncs.pTriAPI->TexCoord2f( 0, 1 ); gEngfuncs.pTriAPI->Vertex3f( cx - c - sn, cy - sn + c, 0 );
		gEngfuncs.pTriAPI->End();
	}

	gEngfuncs.pTriAPI->RenderMode( kRenderNormal );

	if( !any )
		m_iFlags &= ~HUD_ACTIVE;

	return 1;
}

/*
=============
XB_Rumble

Controller rumble for weapon kick (RealRTCWX CG_PadRumbleEx): body
motors scaled by the kick, plus the right trigger motor on the Xbox
controller. Runs through the engine's xb_rumble command, which honours
the Audio menu's vibration setting.
=============
*/
void XB_Rumble( float low, float high, int ms, float rightTrigger )
{
	char cmd[96];

	snprintf( cmd, sizeof( cmd ), "xb_rumble %.2f %.2f %d %.2f 0\n", low, high, ms, rightTrigger );
	gEngfuncs.pfnClientCmd( cmd );
}

#endif // XASH_UWP
