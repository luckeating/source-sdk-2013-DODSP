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

// Called by the main view (viewrender.cpp RenderView) after the 2D debug overlays: draws the
// cascades on screen when dodsp_csm_view is on (stage 3b-1 debug).
void DoDSP_DrawSunCascadeView( const CViewSetup &mainView );

// For later stages (the shadow mask): the depth texture and world -> texture matrix of a cascade.
// Returns false while the cascade has never rendered (no sun, maps off, map just loaded).
bool DoDSP_GetSunCascade( int nCascade, ITexture **ppDepth, VMatrix *pWorldToTexture );

// The one answer to "does the world draw with the per-pixel sun right now?" (DODSP_SUNMODE_DYNAMIC as
// last published: dodsp_sun_baked 0 and the dynamic sun able to draw). Stage 4c: the prop shadows
// (clientshadowmgr.cpp) become contact shadows while this is true.
bool DoDSP_PerPixelSunDraws();

// The one answer to "does the engine draw this map with its HDR lighting?" (HDR on AND the map compiled with
// HDR lighting, VRAD -both/-hdr). DOD-SP is lit for HDR: in LDR the engine's model lighting draws props in the
// shade ~10x too dark (found 2026-11-18). The sun's brightness and the static-prop mark follow this.
bool DoDSP_DrawsHDR();

#endif // C_DODSP_SUN_CASCADES_H
