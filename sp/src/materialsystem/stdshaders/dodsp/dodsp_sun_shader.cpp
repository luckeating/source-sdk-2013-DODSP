//===================== DOD-SP sun (shader DLL side) =====================//
//
// See dodsp_sun_shader.h. Lighting stage 3b-2.
//
//=========================================================================//

#include "BaseVSShader.h"
#include "renderparm.h"
#include "lightmappedgeneric_dx9_helper.h"
#include "dodsp_shaders.h"
#include "dodsp_sun_shader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// INVARIANT: DOD-SP's rendering parameters sit in the slots Valve and Mapbase leave free.
COMPILE_TIME_ASSERT( DODSP_RP_VEC_AXIS_R > VECTOR_RENDERPARM_WIND_DIRECTION );
COMPILE_TIME_ASSERT( DODSP_RP_VEC_FILTER < MAX_VECTOR_RENDER_PARMS );
COMPILE_TIME_ASSERT( DODSP_RP_INT_CASCADE0 > INT_FLASHLIGHT_DEPTHTEXTURE_FALLBACK_LAST );
COMPILE_TIME_ASSERT( DODSP_RP_INT_CASCADE2 < MAX_INT_RENDER_PARMS );

// Shader registers - must match dodsp_sun_fxc.h. INVARIANT: all within c0..c31 (the 2013 engine
// keeps 32 pixel constants; above that a write overflows its storage - see dodsp_sun_fxc.h).
enum
{
	DODSP_PSREG_SUNCOLOR	= 13,
	DODSP_PSREG_AXIS_R		= 14,
	DODSP_PSREG_AXIS_U		= 15,
	DODSP_PSREG_AXIS_F		= 16,
	DODSP_PSREG_CASCADE0	= 17,	// .. 19
	DODSP_PSREG_TOSUN		= 20,
	DODSP_PSREG_COUNT		= 8,
};
COMPILE_TIME_ASSERT( DODSP_PSREG_SUNCOLOR + DODSP_PSREG_COUNT <= 32 );
static const Sampler_t s_CascadeSampler[3] = { SHADER_SAMPLER13, SHADER_SAMPLER14, SHADER_SAMPLER15 };

int DoDSP_SunFilterMode()
{
	return ( g_pHardwareConfig->GetShadowFilterMode() == 0 ) ? 0 : 1;	// 0 = NVIDIA_PCF_POISSON
}

void DoDSP_SunShadowState( IShaderShadow *pShaderShadow )
{
	for ( int i = 0; i < 3; i++ )
	{
		pShaderShadow->EnableTexture( s_CascadeSampler[i], true );
		pShaderShadow->SetShadowDepthFiltering( s_CascadeSampler[i] );
		pShaderShadow->EnableSRGBRead( s_CascadeSampler[i], false );
	}
}

void DoDSP_SunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, float flLightmapScale )
{
	ITexture *pCascade[3];
	bool bAll = true;
	for ( int i = 0; i < 3; i++ )
	{
		pCascade[i] = (ITexture *)(intp)pShaderAPI->GetIntRenderingParameter( DODSP_RP_INT_CASCADE0 + i );
		bAll = bAll && pCascade[i];
	}

	const Vector &vecSun = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_SUNCOLOR );
	float vSunColor[4] = { vecSun.x, vecSun.y, vecSun.z, flLightmapScale > 0.0f ? 1.0f / flLightmapScale : 1.0f };
	if ( !bAll )
		vSunColor[0] = vSunColor[1] = vSunColor[2] = 0.0f;	// no maps = no dynamic sun (the client then keeps the baked one)

	for ( int i = 0; i < 3; i++ )
	{
		if ( bAll )
			pShader->BindTexture( s_CascadeSampler[i], pCascade[i], 0 );
		else
			pShaderAPI->BindStandardTexture( s_CascadeSampler[i], TEXTURE_BLACK );
	}

	const Vector &vecR = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_R );
	const Vector &vecU = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_U );
	const Vector &vecF = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_F );
	const Vector &vecDepth = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_DEPTH );
	const Vector &vecFilter = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_FILTER );

	float c[8][4];
	Q_memcpy( c[0], vSunColor, sizeof( c[0] ) );
	c[1][0] = vecR.x; c[1][1] = vecR.y; c[1][2] = vecR.z; c[1][3] = vecDepth.x;		// axis R, depth offset
	c[2][0] = vecU.x; c[2][1] = vecU.y; c[2][2] = vecU.z; c[2][3] = vecDepth.y;		// axis U, 1 / texels
	c[3][0] = vecF.x; c[3][1] = vecF.y; c[3][2] = vecF.z; c[3][3] = vecDepth.z;		// axis F, depth bias
	for ( int i = 0; i < 3; i++ )
	{
		const Vector &vecC = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_CASCADE0 + i );
		c[4 + i][0] = vecC.x;
		c[4 + i][1] = vecC.y;
		c[4 + i][2] = vecC.z;
		// normal offset in world units: N texels of this cascade
		c[4 + i][3] = ( vecC.x > 0.0f ) ? vecFilter.y * vecDepth.y / vecC.x : 0.0f;
	}
	Vector vecToSun = -vecF;
	VectorNormalize( vecToSun );
	c[7][0] = vecToSun.x; c[7][1] = vecToSun.y; c[7][2] = vecToSun.z; c[7][3] = vecFilter.x;
	// INVARIANT: never write past the constants the hardware layer actually keeps
	if ( g_pHardwareConfig->NumPixelShaderConstants() < DODSP_PSREG_SUNCOLOR + DODSP_PSREG_COUNT )
	{
		static bool s_bWarned = false;
		if ( !s_bWarned )
			Warning( "DOD-SP sun: only %d pixel shader constants - the dynamic sun is off\n", g_pHardwareConfig->NumPixelShaderConstants() );
		s_bWarned = true;
		return;
	}
	pShaderAPI->SetPixelShaderConstant( DODSP_PSREG_SUNCOLOR, c[0], DODSP_PSREG_COUNT );
}

const char *DoDSP_BrushUnsupported( IMaterialVar **params, const LightmappedGeneric_DX9_Vars_t &info )
{
	if ( !g_pHardwareConfig->SupportsShaderModel_3_0() )
		return "no shader model 3.0";

	#define DEFINED( n )	( (n) != -1 && params[n]->IsDefined() )
	#define INTVAL( n )		( (n) != -1 && params[n]->IsDefined() && params[n]->GetIntValue() != 0 )
	if ( INTVAL( info.m_nSelfShadowedBumpFlag ) )						return "$ssbump";
	if ( DEFINED( info.m_nSeamlessMappingScale ) && params[info.m_nSeamlessMappingScale]->GetFloatValue() != 0.0f )
																		return "$seamless_scale";
	if ( DEFINED( info.m_nLightWarpTexture ) )							return "$lightwarptexture";
	if ( DEFINED( info.m_nBlendModulateTexture ) )						return "$blendmodulatetexture";
	if ( INTVAL( info.m_nMaskedBlending ) )								return "$maskedblending";
	if ( DEFINED( info.m_nBumpMask ) )									return "$bumpmask";
#ifdef MAPBASE
	if ( DEFINED( info.m_nBaseTexture2Transform ) )						return "$basetexturetransform2";
#endif
#ifdef PARALLAX_CORRECTED_CUBEMAPS
	if ( INTVAL( info.m_nEnvmapParallax ) )								return "parallax cubemap";
#endif
	if ( INTVAL( info.m_nPhong ) )										return "$phong";
	if ( INTVAL( info.m_nOutline ) || INTVAL( info.m_nSoftEdges ) )		return "distance-alpha edges";
	if ( DEFINED( info.m_nDetail ) && DEFINED( info.m_nDetailTextureCombineMode ) &&
		 params[info.m_nDetailTextureCombineMode]->GetIntValue() > 1 )	return "$detailblendmode above 1";
	#undef DEFINED
	#undef INTVAL
	return NULL;
}
