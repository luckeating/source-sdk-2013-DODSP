//===================== DOD-SP sun shadows: model shader state =====================//
//
// One owner for the sun state of a model draw (VertexLitGeneric, its bump variant, and skin).
// Called on every base-pass (non-flashlight) draw of the shader model 3 path. It reads what the
// client published (clientshadowmgr.cpp DoDSP_RenderSunMaps, renderparm.h) and drives:
//   - the pixel shader's flashlight resources, which the base pass never uses (dodsp_sun_ps.h):
//     the world-to-texture matrix, cFlashlightColor (c28), cFlashlightScreenScale (c31) and the
//     shadow depth sampler, now carrying the sun and its full shadow map
//   - the vertex shader's g_DoDSPSunVSFlags (common_vs_fxc.h, c51): x = 1 drops the engine's
//     directional light, its one-trace-per-model sun, so the sun is never counted twice
// Everything is written on every call, off or on, so no stale value from another draw leaks in.
//===================================================================================//
#ifndef DODSP_MODELSUN_H
#define DODSP_MODELSUN_H

#include "renderparm.h"
#include "mathlib/lightdesc.h"
#include "tier0/dbg.h"

enum
{
	DODSP_VSREG_SUN_FLAGS = 51,			// = SHADER_SPECIFIC_CONST_3 = g_DoDSPSunVSFlags (lint-checked)
	DODSP_PSREG_MODEL_SUN_COLOR = 28,	// = cFlashlightColor in common_ps_fxc.h
	DODSP_PSREG_MODEL_SUN_TWEAKS = 31,	// = cFlashlightScreenScale in common_ps_fxc.h
};

// Why a model draw does or does not get the sun - counted per draw for dodsp_modelsun_report
// (vertexlitgeneric_dx9_helper.cpp), so "it makes no difference" is answered by numbers.
enum DoDSPModelSun_t
{
	DODSP_MS_ON = 0,			// lit per pixel
	DODSP_MS_STATIC_LIT,		// static prop with baked vertex lighting
	DODSP_MS_NOT_LIT,			// unlit / ambient-only material
	DODSP_MS_NO_LIGHT_STATE,	// the engine gave this draw no lighting at all
	DODSP_MS_SETTING_OFF,		// dodsp_sunshadow_models 0 or dodsp_sun_darken 0
	DODSP_MS_NO_MAP,			// no sun shadow map this frame (no env_global_light with shadows)
	DODSP_MS_COUNT
};

inline int *DoDSP_ModelSunCounts()
{
	static int s_nCounts[DODSP_MS_COUNT];
	return s_nCounts;
}

// The model sun exists on the shader model 3 path only (the ps30/vs30 shaders DOD-SP compiles)
inline bool DoDSP_ModelSunAvailable()
{
	return g_pHardwareConfig->SupportsShaderModel_3_0() && !IsX360();
}

// Shadow state of a base pass that may carry the sun: its shadow depth sampler reads the full map
inline void DoDSP_EnableModelSunSampler( IShaderShadow *pShaderShadow, Sampler_t depthSampler )
{
	pShaderShadow->EnableTexture( depthSampler, true );
	pShaderShadow->SetShadowDepthFiltering( depthSampler );
	pShaderShadow->EnableSRGBRead( depthSampler, false );
}

// INVARIANT: the engine's own directional light for models and the published sun must be the
// same light. If their colour or direction disagree, the per-pixel sun on models would not
// match the baked sun on the world - say so once, with the numbers needed to fix it.
inline void DoDSP_CheckEngineSun( IShaderDynamicAPI *pShaderAPI, const Vector &vSunColor, const Vector &vSunDir )
{
	static bool s_bWarnedColor = false, s_bWarnedDir = false;
	if ( s_bWarnedColor && s_bWarnedDir )
		return;
	int nLights = pShaderAPI->GetMaxLights();
	for ( int i = 0; i < nLights; i++ )
	{
		const LightDesc_t &light = pShaderAPI->GetLight( i );
		if ( light.m_Type != MATERIAL_LIGHT_DIRECTIONAL )
			continue;
		float flEngine = light.m_Color.Length(), flOurs = vSunColor.Length();
		if ( !s_bWarnedColor && flOurs > 0.0f && fabsf( flEngine / flOurs - 1.0f ) > 0.05f )
		{
			s_bWarnedColor = true;
			Warning( "INVARIANT: DOD-SP model sun: the engine's sun for models is %.3f %.3f %.3f but the published sun is %.3f %.3f %.3f (ratio %.3f) - models will be lit differently from the world\n",
					 light.m_Color.x, light.m_Color.y, light.m_Color.z, vSunColor.x, vSunColor.y, vSunColor.z, flOurs > 0.0f ? flEngine / flOurs : 0.0f );
		}
		if ( !s_bWarnedDir && DotProduct( light.m_Direction.Normalized(), vSunDir.Normalized() ) < 0.9994f )	// 2 degrees
		{
			s_bWarnedDir = true;
			Warning( "INVARIANT: DOD-SP model sun: the engine's sun for models points %.2f %.2f %.2f but the published sun points %.2f %.2f %.2f\n",
					 light.m_Direction.x, light.m_Direction.y, light.m_Direction.z, vSunDir.x, vSunDir.y, vSunDir.z );
		}
	}
}

// nMatrixReg: the pixel register of this shader's g_FlashlightWorldToTexture.
// depthSampler: this shader's ShadowDepthSampler.
// nVerdict: DODSP_MS_ON if this draw may take the sun, else why it keeps stock lighting
// (unlit, ambient-only, static props whose baked vertex lighting already contains the sun).
inline void DoDSP_SetModelSunState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI,
									int nMatrixReg, Sampler_t depthSampler, int nVerdict )
{
	ITexture *pFull = (ITexture *)pShaderAPI->GetIntRenderingParameter( INT_RENDERPARM_DODSP_SUN_FULL_DEPTH );
	Vector vParams = pShaderAPI->GetVectorRenderingParameter( VECTOR_RENDERPARM_DODSP_SUN_PARAMS );
	Vector vTweaks = pShaderAPI->GetVectorRenderingParameter( VECTOR_RENDERPARM_DODSP_SUN_TWEAKS );
	if ( nVerdict == DODSP_MS_ON && ( !pFull || vParams.y <= 0.0f ) )
		nVerdict = DODSP_MS_NO_MAP;
	else if ( nVerdict == DODSP_MS_ON && vTweaks.y <= 0.0f )
		nVerdict = DODSP_MS_SETTING_OFF;
	DoDSP_ModelSunCounts()[nVerdict]++;
	const bool bOn = ( nVerdict == DODSP_MS_ON );

	float matrix[16], color[4], tweaks[4], vsFlags[4];
	memset( matrix, 0, sizeof( matrix ) );
	memset( color, 0, sizeof( color ) );
	memset( tweaks, 0, sizeof( tweaks ) );
	memset( vsFlags, 0, sizeof( vsFlags ) );
	if ( bOn )
	{
		pShader->BindTexture( depthSampler, pFull, 0 );
		// Rows 0-2 of the orthographic world-to-sun matrix (row 3 is 0 0 0 1), uploaded the way a
		// flashlight's world-to-texture matrix is, so mul( float4( pos, 1 ), matrix ) projects.
		float rows[12];
		for ( int r = 0; r < 4; r++ )
		{
			Vector v = pShaderAPI->GetVectorRenderingParameter( VECTOR_RENDERPARM_DODSP_SUN_MATRIX_0 + r );
			rows[r * 3 + 0] = v.x;
			rows[r * 3 + 1] = v.y;
			rows[r * 3 + 2] = v.z;
		}
		memcpy( matrix, rows, sizeof( rows ) );
		matrix[15] = 1.0f;
		Vector vColor = pShaderAPI->GetVectorRenderingParameter( VECTOR_RENDERPARM_DODSP_SUN_COLOR );
		Vector vDir = pShaderAPI->GetVectorRenderingParameter( VECTOR_RENDERPARM_DODSP_SUN_DIR );
		color[0] = vColor.x; color[1] = vColor.y; color[2] = vColor.z; color[3] = vParams.x;
		tweaks[0] = ( g_pHardwareConfig->GetShadowFilterMode() != 0 ) ? 1.0f : 0.0f;
		tweaks[1] = vTweaks.x;
		tweaks[2] = vTweaks.z;
		tweaks[3] = 1.0f;
		vsFlags[0] = 1.0f;
		DoDSP_CheckEngineSun( pShaderAPI, vColor, vDir );
	}
	else
	{
		pShaderAPI->BindStandardTexture( depthSampler, TEXTURE_WHITE );
	}
	pShaderAPI->SetPixelShaderConstant( nMatrixReg, matrix, 4 );
	pShaderAPI->SetPixelShaderConstant( DODSP_PSREG_MODEL_SUN_COLOR, color, 1 );
	pShaderAPI->SetPixelShaderConstant( DODSP_PSREG_MODEL_SUN_TWEAKS, tweaks, 1 );
	pShaderAPI->SetVertexShaderConstant( DODSP_VSREG_SUN_FLAGS, vsFlags, 1 );
}

#endif // DODSP_MODELSUN_H
