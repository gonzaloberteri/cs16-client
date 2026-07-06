/*
scopetune.cpp - mouse-driven on-screen slider panel for the ray-traced
viewmodel scope lens.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.
*/

// The convex ray-traced lens on a held sniper's scope is driven by a handful of
// engine cvars (rt_scope_vm_* + rt_scope_thin, registered in xash-rt ref/gl).
// This element draws a slider panel overlaid on the game and lets you drag the
// values with the mouse: while the panel is open the input code (input_xash3d)
// feeds the look deltas into a virtual cursor and the mouse buttons here instead
// of to the view/weapon. Toggle it with the "o" bind (or "scopetune" in console).
// Only reads/writes cvars by name, so it is a no-op on the classic renderer.

#include "hud.h"
#include "cl_util.h"
#include "draw_util.h"

static struct scopeparam_t
{
	const char *label;
	const char *cvar;
	float       lo, hi;
} s_params[] =
{
	{ "enable", "rt_scope_vm",         0.0f,  1.0f   },
	{ "mirror", "rt_scope_vm_mirror",  0.0f,  1.0f   },
	{ "thin",   "rt_scope_thin",       0.0f,  1.0f   },
	{ "face",   "rt_scope_vm_face",    0.0f,  1.0f   },
	{ "scale",  "rt_scope_vm_scale",   0.10f, 4.0f   },
	{ "bulge",  "rt_scope_vm_bulge",   0.0f,  0.95f  },
	{ "push",   "rt_scope_vm_push",   -2.0f,  2.0f   },
	{ "red",    "rt_scope_vm_r",       0.0f,  255.0f },
	{ "green",  "rt_scope_vm_g",       0.0f,  255.0f },
	{ "blue",   "rt_scope_vm_b",       0.0f,  255.0f },
	{ "opacity","rt_scope_vm_a",       0.0f,  255.0f },
};
#define SCOPE_NPARAMS ( (int)( sizeof( s_params ) / sizeof( s_params[0] ) ) )

// panel geometry, computed each frame from the resolution
struct layout_t
{
	int x0, y0, w;
	int rowH, rowTop, trackX, trackW;
	int dumpX, dumpY, dumpW, dumpH;
};

static void ScopeTune_Layout( layout_t *L )
{
	L->w      = 460;
	L->rowH   = 34;
	L->x0     = 60;
	L->rowTop = ScreenHeight / 2 - ( SCOPE_NPARAMS * L->rowH ) / 2 - 10;
	L->y0     = L->rowTop - L->rowH; // title row above the first slider
	L->trackX = L->x0 + 150;
	L->trackW = L->w - 150 - 70;
	L->dumpW  = 150;
	L->dumpH  = 24;
	L->dumpX  = L->x0;
	L->dumpY  = L->rowTop + SCOPE_NPARAMS * L->rowH + 12;
}

static inline bool PointIn( float px, float py, int x, int y, int w, int h )
{
	return px >= x && px <= x + w && py >= y && py <= y + h;
}

int CHudScopeTune::Init( void )
{
	HOOK_COMMAND( gHUD.m_ScopeTune, "scopetune",      Toggle );
	HOOK_COMMAND( gHUD.m_ScopeTune, "scopetune_dump", Dump );

	m_pShow   = CVAR_CREATE( "rt_scope_tune", "0", FCVAR_ARCHIVE );
	m_pCursor = CVAR_CREATE( "rt_scope_cursor", "40", FCVAR_ARCHIVE ); // degrees of mouse travel to cross the screen; lower = faster

	m_curX = m_curY = 0.0f;
	m_iDrag = -1;
	m_bDown = m_bPrev = m_bInit = false;

	gHUD.AddHudElem( this );
	m_iFlags = HUD_DRAW;
	return 1;
}

int CHudScopeTune::VidInit( void )
{
	return 1;
}

bool CHudScopeTune::IsActive( void )
{
	return m_pShow && m_pShow->value != 0.0f;
}

void CHudScopeTune::MoveCursor( float dyaw, float dpitch )
{
	if( !m_bInit )
	{
		m_curX = ScreenWidth  * 0.5f;
		m_curY = ScreenHeight * 0.5f;
		m_bInit = true;
	}

	float k    = m_pCursor ? m_pCursor->value : 40.0f;
	if( k < 5.0f ) k = 5.0f;
	float perDeg = (float)ScreenWidth / k;

	// look deltas: dyaw = -rawX*m_yaw, dpitch = +rawY*m_pitch (degrees)
	m_curX -= dyaw   * perDeg;
	m_curY += dpitch * perDeg;

	if( m_curX < 0 ) m_curX = 0;
	if( m_curY < 0 ) m_curY = 0;
	if( m_curX > ScreenWidth  ) m_curX = ScreenWidth;
	if( m_curY > ScreenHeight ) m_curY = ScreenHeight;
}

void CHudScopeTune::SetMouse( int mstate )
{
	m_bDown = ( mstate & 1 ) != 0; // bit 0 = MOUSE1
}

static void ScopeTune_DumpValues()
{
	gEngfuncs.Con_Printf( "// scope lens tuner - current values\n" );
	for( int i = 0; i < SCOPE_NPARAMS; i++ )
		gEngfuncs.Con_Printf( "%s %.3f\n", s_params[i].cvar, gEngfuncs.pfnGetCvarFloat( s_params[i].cvar ) );
}

int CHudScopeTune::Draw( float flTime )
{
	if( !IsActive() )
	{
		m_bPrev = m_bDown; // keep edge state fresh so a click on open isn't stale
		return 1;
	}

	layout_t L;
	ScopeTune_Layout( &L );

	const bool press   = m_bDown && !m_bPrev;
	const bool release = !m_bDown && m_bPrev;
	if( release )
		m_iDrag = -1;

	char line[128];

	// backdrop
	gEngfuncs.pfnFillRGBABlend( L.x0 - 16, L.y0 - 12, L.w + 32,
		( SCOPE_NPARAMS + 1 ) * L.rowH + L.dumpH + 40, 0, 0, 0, 170 );

	// title + held gun
	const char *gun = "-";
	WEAPON *w = gHUD.m_Ammo.GetCurrentWeapon();
	if( w && w->szName[0] )
	{
		gun = w->szName;
		if( !strncmp( gun, "weapon_", 7 ) )
			gun += 7;
	}
	snprintf( line, sizeof( line ), "SCOPE LENS TUNER   gun: %s", gun );
	DrawUtils::DrawHudString( L.x0, L.y0 + 6, ScreenWidth, line, 120, 200, 255 );

	for( int i = 0; i < SCOPE_NPARAMS; i++ )
	{
		const int   rowY   = L.rowTop + i * L.rowH;
		const int   trackY = rowY + L.rowH / 2 - 4;
		scopeparam_t *p     = &s_params[i];

		// begin dragging this slider on a press anywhere along its row
		if( press && m_curY >= rowY && m_curY < rowY + L.rowH &&
			m_curX >= L.trackX - 12 && m_curX <= L.trackX + L.trackW + 12 )
			m_iDrag = i;

		float v = gEngfuncs.pfnGetCvarFloat( p->cvar );

		if( m_bDown && m_iDrag == i )
		{
			float t = ( m_curX - L.trackX ) / (float)L.trackW;
			if( t < 0 ) t = 0;
			if( t > 1 ) t = 1;
			v = p->lo + ( p->hi - p->lo ) * t;
			gEngfuncs.Cvar_SetValue( p->cvar, v );
		}

		float t  = ( v - p->lo ) / ( p->hi - p->lo );
		if( t < 0 ) t = 0; if( t > 1 ) t = 1;
		int   hx = L.trackX + (int)( L.trackW * t );

		// label, track, fill, handle, value
		DrawUtils::DrawHudString( L.x0, rowY + 4, ScreenWidth, p->label,
			( m_iDrag == i ) ? 255 : 210, ( m_iDrag == i ) ? 220 : 210, ( m_iDrag == i ) ? 40 : 210 );
		gEngfuncs.pfnFillRGBABlend( L.trackX, trackY, L.trackW, 8, 70, 70, 70, 230 );
		gEngfuncs.pfnFillRGBABlend( L.trackX, trackY, hx - L.trackX, 8, 255, 190, 0, 230 );
		gEngfuncs.pfnFillRGBABlend( hx - 3, trackY - 5, 6, 18, 255, 255, 255, 255 );

		snprintf( line, sizeof( line ), "%.3f", v );
		DrawUtils::DrawHudString( L.trackX + L.trackW + 12, rowY + 4, ScreenWidth, line, 235, 235, 235 );
	}

	// dump-to-console button
	bool dumpHot = PointIn( m_curX, m_curY, L.dumpX, L.dumpY, L.dumpW, L.dumpH );
	gEngfuncs.pfnFillRGBABlend( L.dumpX, L.dumpY, L.dumpW, L.dumpH, dumpHot ? 90 : 50, dumpHot ? 90 : 50, dumpHot ? 120 : 60, 230 );
	DrawUtils::DrawHudString( L.dumpX + 12, L.dumpY + 4, ScreenWidth, "DUMP TO CONSOLE", 200, 220, 255 );
	if( press && dumpHot )
		ScopeTune_DumpValues();

	// hint
	DrawUtils::DrawHudString( L.x0, L.dumpY + L.dumpH + 8, ScreenWidth,
		"drag the sliders   -   press O to close", 150, 150, 150 );

	// cursor (dark outline + white cross so it reads over any background)
	int cx = (int)m_curX, cy = (int)m_curY;
	gEngfuncs.pfnFillRGBABlend( cx - 9, cy - 1, 19, 3, 0, 0, 0, 220 );
	gEngfuncs.pfnFillRGBABlend( cx - 1, cy - 9, 3, 19, 0, 0, 0, 220 );
	gEngfuncs.pfnFillRGBABlend( cx - 8, cy,     17, 1, 255, 255, 255, 255 );
	gEngfuncs.pfnFillRGBABlend( cx,     cy - 8, 1, 17, 255, 255, 255, 255 );

	m_bPrev = m_bDown;
	return 1;
}

void CHudScopeTune::UserCmd_Toggle()
{
	bool on = !( m_pShow && m_pShow->value != 0.0f );
	gEngfuncs.Cvar_SetValue( "rt_scope_tune", on ? 1.0f : 0.0f );
	if( on )
	{
		m_curX  = ScreenWidth  * 0.5f;
		m_curY  = ScreenHeight * 0.5f;
		m_bInit = true;
		m_iDrag = -1;
	}
}

void CHudScopeTune::UserCmd_Dump()
{
	ScopeTune_DumpValues();
}
