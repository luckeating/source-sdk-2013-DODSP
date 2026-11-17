//===================== DOD-SP sun (shader DLL side) =====================//
//
// One owner, in the shader DLL, for feeding the sun to a DOD-SP shader (lighting stage 3b-2):
// shadow state (cascade samplers), dynamic state (cascade textures + constants from the rendering
// parameters the client writes, see public/dodsp_shaders.h) and the list of brush features the
// DOD-SP brush shaders hand back to Mapbase. The pixel side is dodsp_sun_fxc.h.
//
//=========================================================================//
#ifndef DODSP_SUN_SHADER_H
#define DODSP_SUN_SHADER_H

class CBaseVSShader;
class IShaderShadow;
class IShaderDynamicAPI;
class IMaterialVar;
struct LightmappedGeneric_DX9_Vars_t;

// DODSP_SUNFILTER combo: 0 = hardware depth compare (NVIDIA), 1 = raw depth compared in the shader
int DoDSP_SunFilterMode();

// Shadow state: samplers 13..15 hold the near/mid/far cascades
void DoDSP_SunShadowState( IShaderShadow *pShaderShadow );

// Dynamic state: binds the cascades and sets c32..c39. lightmap scale = what the shader multiplies
// lightmaps by (pShaderAPI->GetLightMapScaleFactor()), so the sun lands in the same units.
void DoDSP_SunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, float flLightmapScale );

// NULL when DODSP_LightmappedGeneric / DODSP_WorldVertexTransition can draw this material,
// otherwise the feature that sends it back to Mapbase's SDK_ twin (used by SHADER_FALLBACK).
const char *DoDSP_BrushUnsupported( IMaterialVar **params, const LightmappedGeneric_DX9_Vars_t &info );

#endif // DODSP_SUN_SHADER_H
