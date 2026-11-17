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
struct VertexLitGeneric_DX9_Vars_t;

// DODSP_SUNFILTER combo: 0 = hardware depth compare (NVIDIA), 1 = raw depth compared in the shader
int DoDSP_SunFilterMode();

// Shadow state: samplers nFirstSampler..+2 hold the near/mid/far cascades (13..15; the skin shader 9..11)
void DoDSP_SunShadowState( IShaderShadow *pShaderShadow, int nFirstSampler = 13 );

// Dynamic state: binds the cascades and sets c13..c20 (dodsp_sun_fxc.h). lightmap scale = what the shader multiplies
// lightmaps by (pShaderAPI->GetLightMapScaleFactor()), so the sun lands in the same units.
void DoDSP_SunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, float flLightmapScale );

// NULL when DODSP_LightmappedGeneric / DODSP_WorldVertexTransition can draw this material,
// otherwise the feature that sends it back to Mapbase's SDK_ twin (used by SHADER_FALLBACK).
const char *DoDSP_BrushUnsupported( IMaterialVar **params, const LightmappedGeneric_DX9_Vars_t &info );

//---------------------------------------------------------------------------------------------
// Models (lighting stage 4). The constants each model shader family uses - they must match the
// registers in the .fxc files (CI checks; see dodsp_modelsun_vs_fxc.h / dodsp_modelsun_ps_fxc.h).
// INVARIANT: pixel registers inside c0..c31, vertex registers clear of the bones (c58..c216) and
// the engine's clip planes (c217..c220).
//---------------------------------------------------------------------------------------------
enum
{
	DODSP_MSUN_PSREG_PLAIN	= 15,	// dodsp_vertexlit_and_unlit_generic_ps30: c15..c18, unused there
	DODSP_MSUN_PSREG_BUMP	= 22,	// dodsp_vertexlit_and_unlit_generic_bump_ps30: the flashlight's c22..c25
	DODSP_MSUN_PSREG_SKIN	= 15,	// dodsp_skin_ps30: the flashlight matrix's c15..c18
	DODSP_MSUN_PSREG_COUNT	= 4,
	DODSP_MSUN_VSREG		= 221,	// c221..c224 in every model vertex shader
	DODSP_MSUN_VSREG_COUNT	= 4,
	DODSP_MSUN_SAMPLER		= 13,	// cascades on s13..s15 (plain and bump)
	DODSP_MSUN_SAMPLER_SKIN	= 9,	// the skin shader uses all 16 samplers: its wrinkle ones (WRINKLEMAP pinned to 0)
};

// Dynamic state for a model draw. bBasePass = false for the flashlight pass: then only the vertex
// shader's "our sun draws" flag is cleared (the pixel registers belong to the flashlight there).
void DoDSP_ModelSunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, int nPsReg, bool bBasePass,
								 int nFirstSampler = DODSP_MSUN_SAMPLER );

// NULL when DODSP_VertexLitGeneric can draw this material, otherwise the feature that sends it
// back to Mapbase's SDK_VertexLitGeneric (used by SHADER_FALLBACK). One owner: this list and the
// combos pinned to 0 in the dodsp_vertexlit*/dodsp_skin* .fxc files describe the same features.
const char *DoDSP_ModelUnsupported( IMaterialVar **params, const VertexLitGeneric_DX9_Vars_t &info );

// INVARIANT watchdog for pinned combos: a combo the DOD-SP shader file only compiled as 0..nMax
// must never be set higher (that selects the wrong shader variant). Returns the value clamped and
// prints one INVARIANT line per combo name if it ever happens.
int DoDSP_Pinned( const char *pszCombo, int nValue, int nMax );

#endif // DODSP_SUN_SHADER_H
