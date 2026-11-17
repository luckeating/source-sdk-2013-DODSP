//=============================================================================//
//
// DOD-SP: the runtime swap of stock materials onto Mapbase's SDK_ / DOD-SP's DODSP_ shaders.
// See c_dodsp_sdkshaders.cpp.
//
//=============================================================================//
#ifndef C_DODSP_SDKSHADERS_H
#define C_DODSP_SDKSHADERS_H

// True when this game's world materials go onto DOD-SP's sun shaders (DODSP_LightmappedGeneric /
// DODSP_WorldVertexTransition) - the condition for the dynamic sun to be visible at all.
bool DoDSP_SunShadersInUse();

#endif // C_DODSP_SDKSHADERS_H
