//===================== DOD-SP sun shadows: brush (lightmappedgeneric) removal =====================//
//
// One owner for how much baked sun a brush pixel loses to something dynamic.
//
// INVARIANT: where only static casters (world, static props) block the sun, nothing is removed.
// VRAD already baked those shadows; removing light there darkens the edges of every baked shadow
// (window frames got a dark, saw-toothed fringe when the two maps were filtered differently).
// So both maps are read through the SAME taps: the full map holds every caster the static map
// holds at the same depth, so for a static-only shadow each tap agrees and static - full = 0
// exactly, at any softness. Only a caster that is in the full map alone (NPC, physics prop,
// brush entity) makes them differ - that difference is the removed sun, with a soft edge.
//
// Kept out of dodsp_sun_ps.h on purpose: that header is in every model shader, and a change
// there recompiles the skin shader (2 hours). This one is included by lightmappedgeneric only.
//==================================================================================================//
#ifndef DODSP_SUN_BRUSH_PS_H
#define DODSP_SUN_BRUSH_PS_H

#include "dodsp_sun_ps.h"

// x = static visibility (did VRAD's sun reach here), y = full visibility (does it reach now),
// z = the share of the sun to remove = x - y (never negative). 4x4 taps spaced flSoft texels.
float3 DoDSP_BrushSunRemoval( sampler StaticSampler, sampler FullSampler, float3 vPos, float flTexel,
							  float flSoft, float flManualCompare )
{
	float flStatic = 0.0f, flFull = 0.0f;
	for ( int y = 0; y < 4; y++ )
	{
		for ( int x = 0; x < 4; x++ )
		{
			float2 vUV = vPos.xy + ( float2( x, y ) - 1.5f ) * flTexel * flSoft;
			flStatic += DoDSP_SunTap( StaticSampler, vUV, vPos.z, flManualCompare );
			flFull += DoDSP_SunTap( FullSampler, vUV, vPos.z, flManualCompare );
		}
	}
	flStatic *= ( 1.0f / 16.0f );
	flFull *= ( 1.0f / 16.0f );
	return float3( flStatic, flFull, max( flStatic - flFull, 0.0f ) );
}

#endif // DODSP_SUN_BRUSH_PS_H
