//========= Copyright (C) 2018, CSProMod Team, All rights reserved. =========//
//
// Purpose: provide world light related functions to the client
// 
// Written: November 2011
// Author: Saul Rennison
//
//===========================================================================//

#pragma once

#include "igamesystem.h" // CAutoGameSystem

class Vector;
struct dworldlight_t;

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
class CWorldLights : public CAutoGameSystem
{
public:
	CWorldLights();
	~CWorldLights() { Clear(); }

	//-------------------------------------------------------------------------
	// Find the brightest light source at a point
	//-------------------------------------------------------------------------
	bool GetBrightestLightSource(const Vector &vecPosition, Vector &vecLightPos, Vector &vecLightBrightness);

	// DOD-SP: the sun (light_environment) as VRAD wrote it - the way its light travels.
	// False when the map has no sun.
	bool GetSunDirection( Vector &vecDir ) const;
	// DOD-SP: the sun's colour x brightness in lightmap units, for the LDR or HDR lightmaps
	// (whichever set the map lacks falls back to the other). False when the map has no sun.
	bool GetSunIntensity( bool bHDR, Vector &vecIntensity ) const;
	void FindBrightestLightSourceOld( const Vector &vecPosition, Vector &vecLightPos, Vector &vecLightBrightness, int nCluster );
#ifdef MAPBASE
	void FindBrightestLightSourceNew(const Vector &vecPosition, Vector &vecLightPos, Vector &vecLightBrightness, int nCluster);
	bool GetCumulativeLightSource(const Vector &vecPosition, Vector &vecLightPos, float flMinBrightnessSqr);
#endif

	// CAutoGameSystem overrides
public:
	virtual bool Init();
	virtual void LevelInitPreEntity();
	virtual void LevelShutdownPostEntity() { Clear(); }

private:
	void Clear();

	int m_nWorldLights;
	dworldlight_t *m_pWorldLights;

	// DOD-SP: the sun's intensity in the LDR [0] and HDR [1] world light lumps
	Vector m_vecSunIntensity[2];
	bool m_bHaveSunIntensity[2];

#ifdef MAPBASE
	int m_iSunIndex = -1; // The sun's personal index

	struct clusterLightList_t
	{
		unsigned short	lightCount;
		unsigned short	firstLight;
	};

	CUtlVector<clusterLightList_t>		m_WorldLightsInCluster;
	CUtlVector<unsigned short>			m_WorldLightsIndexList;
#endif
};

//-----------------------------------------------------------------------------
// Singleton exposure
//-----------------------------------------------------------------------------
extern CWorldLights *g_pWorldLights; 
