//===================== DOD-SP sun cascades =====================//
//
// One owner for the sun's shadow maps (lighting stage 2). See c_dodsp_sun_cascades.cpp.
//
//===============================================================//
#ifndef C_DODSP_SUN_CASCADES_H
#define C_DODSP_SUN_CASCADES_H

class CViewSetup;
class ITexture;
class VMatrix;

enum
{
	DODSP_CASCADE_NEAR = 0,	// around the player, every frame
	DODSP_CASCADE_MID,		// wider, every frame
	DODSP_CASCADE_FAR,		// the whole map, refreshed on an interval
	DODSP_CASCADE_COUNT
};

// Called once per frame by the main view (viewrender.cpp ViewDrawScene, VIEW_MAIN) before the
// scene draws, next to the stock shadow depth textures.
void DoDSP_RenderSunCascades( const CViewSetup &mainView );

// For later stages (the shadow mask): the depth texture and world -> texture matrix of a cascade.
// Returns false while the cascade has never rendered (no sun, maps off, map just loaded).
bool DoDSP_GetSunCascade( int nCascade, ITexture **ppDepth, VMatrix *pWorldToTexture );

#endif // C_DODSP_SUN_CASCADES_H
