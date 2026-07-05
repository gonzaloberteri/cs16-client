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
#pragma once
#ifndef SMOKE_VOLUME_H
#define SMOKE_VOLUME_H

class Vector;

// Volumetric smoke grenade effect: a wall-aware voxel volume rendered as a
// cloud of lit billboards. Explosions (HE/C4) carve temporary holes into it.

void SmokeVolume_Init( void );                    // register cvars (CHud::Init)
void SmokeVolume_Reset( void );                   // drop all volumes (map change / vid init)
int SmokeVolume_Enabled( void );                  // cl_smoke_volumetric != 0
void SmokeVolume_Create( const Vector &origin );  // smoke grenade detonation
void SmokeVolume_Explosion( const Vector &origin, float radius ); // carve a hole
void SmokeVolume_Update( double frametime, double time ); // sim (HUD_TempEntUpdate)
void SmokeVolume_ScanExplosions( struct tempent_s *list ); // detect TE_EXPLOSION tempents
void SmokeVolume_Draw( void );                    // render (HUD_DrawTransparentTriangles)

#endif // SMOKE_VOLUME_H
