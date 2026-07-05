/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*/
//
// Volumetric smoke grenade effect.
//
// The detonation flood-fills a voxel grid through the world (traces stop the
// spread at walls), and each occupied voxel is rendered as a slowly swirling
// billboard. Lighting:
//   - classic renderers: per-voxel lightmap sampling via TriAPI LightAtPoint
//   - ray-traced engine (xash-rt): billboards are uploaded as world raster
//     geometry and modulated per-pixel by the path tracer's illumination
//     volume, so the CPU tint is left neutral there.
// HE/C4 explosions knock the density out of nearby voxels (a hole that slowly
// regrows), like CS2's smoke response to grenades.
//
#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "triangleapi.h"
#include "r_efx.h"
#include "com_model.h"
#include "pm_defs.h"
#include "com_weapons.h"
#include "smoke_volume.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SV_2PI 6.28318530f

extern vec3_t v_origin, v_angles;
extern float g_flRoundTime;

//
// grid dimensions (cells)
//
#define SV_CELL         24.0f   // voxel edge, world units
#define SV_NX           15      // x: -7..+7
#define SV_NY           15      // y: -7..+7
#define SV_NZ           10      // z: -3..+6
#define SV_CX           7
#define SV_CY           7
#define SV_CZ           3
#define SV_NUMCELLS     ( SV_NX * SV_NY * SV_NZ )

#define SV_MAX_VOLUMES  6

// ellipsoid extents in cells (horizontal radius / up / down)
#define SV_RADIUS_H     7.0f
#define SV_RADIUS_UP    5.6f
#define SV_RADIUS_DOWN  3.2f

//
// timings, seconds
//
#define SV_EXPAND_TIME     1.6f  // wavefront reaches the rim
#define SV_GROW_TIME       0.55f // single puff scale-in
#define SV_HOLD_TIME       13.0f // fully dense until then
#define SV_DISSIPATE_SPREAD 4.5f // staggered per-cell death window after hold
#define SV_SHRINK_TIME     1.6f  // single puff scale-out at death

// explosion holes
#define SV_EXPLO_RADIUS    240.0f
#define SV_REGROW_RATE     0.4f  // density/sec once the delay passed
#define SV_IMPULSE         52.0f // rim push, world units

#define SV_TRACE_BUDGET    1500  // flood-fill traces per frame

typedef struct
{
	byte	state;		// 0 unknown, 1 open (smoke), 2 blocked
	byte	queued;
	short	depth;		// BFS ring, drives the expansion animation
	float	density;	// 0..1 current
	float	regrowDelay;	// seconds until regrowth resumes
	float	nextLightTime;
	float	lightR, lightG, lightB;	// sampled tint, 0..1
	// static per-puff params
	float	jx, jy, jz;	// jitter inside the cell
	float	phase;
	float	rotBase, rotSpeed;
	float	baseGrey;
	float	sizeMul;
	float	deathOfs;	// 0..1, position in the dissipate window
	// transient explosion push
	float	impX, impY, impZ;
} sv_cell_t;

typedef struct
{
	qboolean	active;
	Vector		origin;
	float		spawnTime;
	int		maxDepth;
	int		lightCursor;
	// pending BFS frontier (cell indices)
	unsigned short	queue[ SV_NUMCELLS ];
	int		qHead, qTail;
	sv_cell_t	cells[ SV_NUMCELLS ];
} sv_volume_t;

static sv_volume_t s_volumes[ SV_MAX_VOLUMES ];
static cvar_t *s_cvarEnable;
static int s_rtEngine = -1; // -1 unknown, 0 classic, 1 xash-rt

// recent explosion dedup ring
#define SV_EXPLO_RING 16
static struct { Vector org; float time; } s_recentExplo[ SV_EXPLO_RING ];
static int s_recentExploIdx;

static inline int SV_Index( int x, int y, int z )
{
	return ( z * SV_NY + y ) * SV_NX + x;
}

static inline Vector SV_CellOrigin( const sv_volume_t *vol, int x, int y, int z )
{
	return vol->origin + Vector(( x - SV_CX ) * SV_CELL, ( y - SV_CY ) * SV_CELL, ( z - SV_CZ ) * SV_CELL );
}

static inline qboolean SV_InsideEllipsoid( int x, int y, int z )
{
	float dx = ( x - SV_CX ) / SV_RADIUS_H;
	float dy = ( y - SV_CY ) / SV_RADIUS_H;
	float dz = ( z - SV_CZ ) / (( z >= SV_CZ ) ? SV_RADIUS_UP : SV_RADIUS_DOWN );

	return ( dx * dx + dy * dy + dz * dz ) <= 1.0f;
}

static qboolean SV_IsRtEngine( void )
{
	if( s_rtEngine < 0 )
		s_rtEngine = gEngfuncs.pfnGetCvarPointer( "rt_volume_illumgrid" ) != NULL ? 1 : 0;
	return s_rtEngine == 1;
}

void SmokeVolume_Init( void )
{
	s_cvarEnable = CVAR_CREATE( "cl_smoke_volumetric", "1", FCVAR_ARCHIVE );
	SmokeVolume_Reset();
}

void SmokeVolume_Reset( void )
{
	memset( s_volumes, 0, sizeof( s_volumes ));
	memset( s_recentExplo, 0, sizeof( s_recentExplo ));
	s_recentExploIdx = 0;
	s_rtEngine = -1;
}

int SmokeVolume_Enabled( void )
{
	return s_cvarEnable && s_cvarEnable->value != 0.0f;
}

static void SV_InitCellParams( sv_cell_t *c )
{
	float jr = SV_CELL * 0.36f;

	c->jx = Com_RandomFloat( -jr, jr );
	c->jy = Com_RandomFloat( -jr, jr );
	c->jz = Com_RandomFloat( -jr, jr );
	c->phase = Com_RandomFloat( 0.0f, SV_2PI );
	c->rotBase = Com_RandomFloat( 0.0f, SV_2PI );
	c->rotSpeed = Com_RandomFloat( 0.12f, 0.3f ) * ( Com_RandomLong( 0, 1 ) ? 1.0f : -1.0f );
	c->baseGrey = Com_RandomFloat( 0.78f, 0.92f );
	c->sizeMul = Com_RandomFloat( 0.85f, 1.25f );
	c->deathOfs = Com_RandomFloat( 0.0f, 1.0f );
	c->lightR = c->lightG = c->lightB = 1.0f;
	c->nextLightTime = 0.0f;
	c->impX = c->impY = c->impZ = 0.0f;
}

static qboolean SV_TraceOpen( const Vector &from, const Vector &to )
{
	pmtrace_t *tr = gEngfuncs.PM_TraceLine((float *)&from, (float *)&to, PM_TRACELINE_PHYSENTSONLY, 2 /* point hull */, -1 );

	return tr && !tr->startsolid && tr->fraction >= 1.0f;
}

// Grow the flood fill, at most `budget` traces this call. The expansion
// animation lags several rings behind, so spreading the traces over a few
// frames is invisible.
static void SV_GrowFloodFill( sv_volume_t *vol, int budget )
{
	static const int ofs[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };

	while( vol->qHead != vol->qTail && budget > 0 )
	{
		int idx = vol->queue[ vol->qHead ];
		vol->qHead = ( vol->qHead + 1 ) % SV_NUMCELLS;

		int x = idx % SV_NX;
		int y = ( idx / SV_NX ) % SV_NY;
		int z = idx / ( SV_NX * SV_NY );
		Vector from = SV_CellOrigin( vol, x, y, z );

		for( int i = 0; i < 6; i++ )
		{
			int nx = x + ofs[i][0], ny = y + ofs[i][1], nz = z + ofs[i][2];

			if( nx < 0 || nx >= SV_NX || ny < 0 || ny >= SV_NY || nz < 0 || nz >= SV_NZ )
				continue;
			if( !SV_InsideEllipsoid( nx, ny, nz ))
				continue;

			int nidx = SV_Index( nx, ny, nz );
			sv_cell_t *nc = &vol->cells[ nidx ];

			if( nc->state != 0 || nc->queued )
				continue;

			budget--;
			if( SV_TraceOpen( from, SV_CellOrigin( vol, nx, ny, nz )))
			{
				nc->state = 1;
				nc->queued = 1;
				nc->depth = vol->cells[ idx ].depth + 1;
				nc->density = 1.0f;
				SV_InitCellParams( nc );
				if( nc->depth > vol->maxDepth )
					vol->maxDepth = nc->depth;

				vol->queue[ vol->qTail ] = (unsigned short)nidx;
				vol->qTail = ( vol->qTail + 1 ) % SV_NUMCELLS;
			}
			else
			{
				nc->state = 2;
			}

			if( budget <= 0 )
			{
				// requeue the current cell to finish its neighbors later
				vol->queue[ vol->qHead == 0 ? ( vol->qHead = SV_NUMCELLS - 1 ) : --vol->qHead ] = (unsigned short)idx;
				return;
			}
		}
	}
}

void SmokeVolume_Create( const Vector &origin )
{
	sv_volume_t *vol = NULL;

	for( int i = 0; i < SV_MAX_VOLUMES; i++ )
	{
		if( !s_volumes[i].active )
		{
			vol = &s_volumes[i];
			break;
		}
	}
	if( !vol )
	{
		// steal the oldest
		vol = &s_volumes[0];
		for( int i = 1; i < SV_MAX_VOLUMES; i++ )
			if( s_volumes[i].spawnTime < vol->spawnTime )
				vol = &s_volumes[i];
	}

	memset( vol, 0, sizeof( *vol ));
	vol->active = true;
	vol->origin = origin + Vector( 0, 0, 26 );
	vol->spawnTime = gEngfuncs.GetClientTime();
	vol->maxDepth = 1;

	// if the center starts inside a wall/floor, nudge it up
	if( gEngfuncs.PM_PointContents((float *)&vol->origin, NULL ) == CONTENTS_SOLID )
		vol->origin.z += 20.0f;

	int center = SV_Index( SV_CX, SV_CY, SV_CZ );
	sv_cell_t *c = &vol->cells[ center ];

	c->state = 1;
	c->queued = 1;
	c->depth = 0;
	c->density = 1.0f;
	SV_InitCellParams( c );

	vol->queue[0] = (unsigned short)center;
	vol->qHead = 0;
	vol->qTail = 1;

	// first chunk immediately so the smoke pops with substance
	SV_GrowFloodFill( vol, SV_TRACE_BUDGET );
}

void SmokeVolume_Explosion( const Vector &origin, float radius )
{
	for( int v = 0; v < SV_MAX_VOLUMES; v++ )
	{
		sv_volume_t *vol = &s_volumes[v];

		if( !vol->active )
			continue;

		// cheap reject: volume bounding sphere
		float reach = SV_RADIUS_H * SV_CELL + radius;
		if(( vol->origin - origin ).Length() > reach )
			continue;

		for( int z = 0; z < SV_NZ; z++ )
		{
			for( int y = 0; y < SV_NY; y++ )
			{
				for( int x = 0; x < SV_NX; x++ )
				{
					sv_cell_t *c = &vol->cells[ SV_Index( x, y, z ) ];

					if( c->state != 1 )
						continue;

					Vector cellOrg = SV_CellOrigin( vol, x, y, z );
					Vector delta = cellOrg - origin;
					float dist = delta.Length();

					if( dist >= radius )
						continue;

					float k = 1.0f - dist / radius;	// 1 at core, 0 at rim
					c->density -= 1.6f * k;
					if( c->density < 0.0f )
						c->density = 0.0f;

					c->regrowDelay = 1.0f + 1.2f * k;

					// radial shove on the surviving rim
					if( dist > 1.0f )
					{
						float push = SV_IMPULSE * k / dist;
						c->impX += delta.x * push;
						c->impY += delta.y * push;
						c->impZ += delta.z * push * 0.5f;
					}
				}
			}
		}
	}
}

void SmokeVolume_ScanExplosions( struct tempent_s *list )
{
	float now = gEngfuncs.GetClientTime();

	for( TEMPENTITY *te = list; te; te = te->next )
	{
		// framerate ~30: still near the first frames right after spawn,
		// even if a slow frame delayed this scan
		if( !te->entity.model || te->entity.curstate.frame > 2.0f )
			continue;

		// engine tempents for TE_EXPLOSION / TE_SPRITE fireballs:
		// zerogxplode.spr, eexplo.spr, fexplo.spr, fexplo1.spr
		if( !strstr( te->entity.model->name, "xplo" ))
			continue;

		Vector org = te->entity.origin;
		qboolean seen = false;

		for( int i = 0; i < SV_EXPLO_RING; i++ )
		{
			if( s_recentExplo[i].time > 0.0f &&
				now - s_recentExplo[i].time < 0.7f &&
				( s_recentExplo[i].org - org ).Length() < 24.0f )
			{
				seen = true;
				break;
			}
		}
		if( seen )
			continue;

		s_recentExplo[ s_recentExploIdx ].org = org;
		s_recentExplo[ s_recentExploIdx ].time = now;
		s_recentExploIdx = ( s_recentExploIdx + 1 ) % SV_EXPLO_RING;

		SmokeVolume_Explosion( org, SV_EXPLO_RADIUS );
	}
}

void SmokeVolume_Update( double frametime, double time )
{
	float dt = (float)frametime;
	float now = (float)time;
	qboolean rt = SV_IsRtEngine();
	float impDecay = expf( -2.2f * dt );

	for( int v = 0; v < SV_MAX_VOLUMES; v++ )
	{
		sv_volume_t *vol = &s_volumes[v];

		if( !vol->active )
			continue;

		// round restart kills lingering smoke, same rule as tempents
		if( g_flRoundTime > 0.0f && vol->spawnTime < g_flRoundTime )
		{
			vol->active = false;
			continue;
		}

		float age = now - vol->spawnTime;

		if( age > SV_HOLD_TIME + SV_DISSIPATE_SPREAD + SV_SHRINK_TIME )
		{
			vol->active = false;
			continue;
		}

		SV_GrowFloodFill( vol, SV_TRACE_BUDGET );

		for( int i = 0; i < SV_NUMCELLS; i++ )
		{
			sv_cell_t *c = &vol->cells[i];

			if( c->state != 1 )
				continue;

			// hole regrowth (not while dissipating)
			if( c->density < 1.0f && age < SV_HOLD_TIME )
			{
				if( c->regrowDelay > 0.0f )
					c->regrowDelay -= dt;
				else
				{
					c->density += SV_REGROW_RATE * dt;
					if( c->density > 1.0f )
						c->density = 1.0f;
				}
			}

			c->impX *= impDecay;
			c->impY *= impDecay;
			c->impZ *= impDecay;
		}

		// staggered lightmap sampling on classic renderers; the RT engine
		// lights the billboards per-pixel on the GPU instead
		if( !rt )
		{
			int lightBudget = 48;

			for( int n = 0; n < SV_NUMCELLS && lightBudget > 0; n++ )
			{
				int idx = ( vol->lightCursor + n ) % SV_NUMCELLS;
				sv_cell_t *c = &vol->cells[ idx ];

				if( c->state != 1 || now < c->nextLightTime )
					continue;

				int x = idx % SV_NX;
				int y = ( idx / SV_NX ) % SV_NY;
				int z = idx / ( SV_NX * SV_NY );
				Vector org = SV_CellOrigin( vol, x, y, z );
				float light[4] = { 255, 255, 255, 255 };

				gEngfuncs.pTriAPI->LightAtPoint( org, light );

				// ambient floor so shadowed smoke stays readable
				c->lightR = bound( 0.0f, light[0] / 255.0f * 1.1f + 0.18f, 1.0f );
				c->lightG = bound( 0.0f, light[1] / 255.0f * 1.1f + 0.18f, 1.0f );
				c->lightB = bound( 0.0f, light[2] / 255.0f * 1.1f + 0.18f, 1.0f );
				c->nextLightTime = now + Com_RandomFloat( 0.35f, 0.55f );
				lightBudget--;
			}

			vol->lightCursor = ( vol->lightCursor + 97 ) % SV_NUMCELLS;
		}
	}
}

//
// rendering
//

typedef struct
{
	Vector	org;
	float	radius;
	float	rot;
	float	r, g, b;
	float	distSq;
} sv_puff_t;

#define SV_MAX_PUFFS ( SV_MAX_VOLUMES * 1400 )
static sv_puff_t s_puffs[ SV_MAX_PUFFS ];

static int SV_PuffCompare( const void *a, const void *b )
{
	float da = ((const sv_puff_t *)a)->distSq;
	float db = ((const sv_puff_t *)b)->distSq;

	return da < db ? 1 : ( da > db ? -1 : 0 ); // far first
}

void SmokeVolume_Draw( void )
{
	if( !SmokeVolume_Enabled( ))
		return;

	const model_t *gasPuff = gEngfuncs.GetSpritePointer( gHUD.m_hGasPuff );
	if( !gasPuff )
		return;

	float now = gEngfuncs.GetClientTime();
	qboolean rt = SV_IsRtEngine();
	int numPuffs = 0;

	for( int v = 0; v < SV_MAX_VOLUMES; v++ )
	{
		sv_volume_t *vol = &s_volumes[v];

		if( !vol->active )
			continue;

		float age = now - vol->spawnTime;
		float expandDepth = ( age / SV_EXPAND_TIME ) * ( SV_RADIUS_H + 1.0f );

		for( int z = 0; z < SV_NZ; z++ )
		{
			for( int y = 0; y < SV_NY; y++ )
			{
				for( int x = 0; x < SV_NX; x++ )
				{
					int idx = SV_Index( x, y, z );
					sv_cell_t *c = &vol->cells[ idx ];

					if( c->state != 1 || c->density <= 0.02f )
						continue;

					// expansion wavefront
					float sinceActive = ( expandDepth - c->depth ) * ( SV_EXPAND_TIME / ( SV_RADIUS_H + 1.0f ));
					if( sinceActive <= 0.0f )
						continue;

					// scale-in, then staggered dissolve at end of life
					float scale = bound( 0.0f, sinceActive / SV_GROW_TIME, 1.0f );
					float deathStart = SV_HOLD_TIME + c->deathOfs * SV_DISSIPATE_SPREAD;

					if( age > deathStart )
					{
						float s = 1.0f - ( age - deathStart ) / SV_SHRINK_TIME;
						if( s <= 0.0f )
							continue;
						scale *= s;
					}

					scale *= 0.55f + 0.45f * c->density;

					if( numPuffs >= SV_MAX_PUFFS )
						break;

					sv_puff_t *p = &s_puffs[ numPuffs++ ];

					float sway = 5.0f;
					Vector org = SV_CellOrigin( vol, x, y, z );
					org.x += c->jx + sway * sinf( now * 0.5f + c->phase ) + c->impX;
					org.y += c->jy + sway * cosf( now * 0.42f + c->phase * 1.7f ) + c->impY;
					org.z += c->jz + sway * 0.6f * sinf( now * 0.35f + c->phase * 2.3f ) + c->impZ;

					p->org = org;
					p->radius = SV_CELL * 1.55f * c->sizeMul * scale;
					p->rot = c->rotBase + now * c->rotSpeed;

					float grey = c->baseGrey;
					if( rt )
					{
						// the illumination volume tints it on the GPU
						p->r = p->g = p->b = grey;
					}
					else
					{
						p->r = grey * c->lightR;
						p->g = grey * c->lightG;
						p->b = grey * c->lightB;
					}

					Vector toEye = org - v_origin;
					p->distSq = DotProduct( toEye, toEye );
				}
			}
		}
	}

	if( !numPuffs )
		return;

	qsort( s_puffs, numPuffs, sizeof( sv_puff_t ), SV_PuffCompare );

	Vector forward, right, up;
	gEngfuncs.pfnAngleVectors( v_angles, forward, right, up );

	gEngfuncs.pTriAPI->RenderMode( kRenderTransAlpha );
	gEngfuncs.pTriAPI->CullFace( TRI_NONE );
	gEngfuncs.pTriAPI->SpriteTexture((struct model_s *)gasPuff, 0 );

	gEngfuncs.pTriAPI->Begin( TRI_QUADS );

	for( int i = 0; i < numPuffs; i++ )
	{
		sv_puff_t *p = &s_puffs[i];
		float c = cosf( p->rot ) * p->radius;
		float s = sinf( p->rot ) * p->radius;
		Vector d1 = right * c - up * s;	// rotated billboard axes
		Vector d2 = right * s + up * c;

		Vector v0 = p->org - d1 - d2;
		Vector v1 = p->org - d1 + d2;
		Vector v2 = p->org + d1 + d2;
		Vector v3 = p->org + d1 - d2;

		gEngfuncs.pTriAPI->Color4f( p->r, p->g, p->b, 1.0f );

		gEngfuncs.pTriAPI->TexCoord2f( 0.0f, 1.0f );
		gEngfuncs.pTriAPI->Vertex3fv( v0 );
		gEngfuncs.pTriAPI->TexCoord2f( 0.0f, 0.0f );
		gEngfuncs.pTriAPI->Vertex3fv( v1 );
		gEngfuncs.pTriAPI->TexCoord2f( 1.0f, 0.0f );
		gEngfuncs.pTriAPI->Vertex3fv( v2 );
		gEngfuncs.pTriAPI->TexCoord2f( 1.0f, 1.0f );
		gEngfuncs.pTriAPI->Vertex3fv( v3 );
	}

	gEngfuncs.pTriAPI->End();
	gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
}
