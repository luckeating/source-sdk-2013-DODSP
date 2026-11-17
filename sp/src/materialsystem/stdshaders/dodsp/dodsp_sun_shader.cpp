//===================== DOD-SP sun (shader DLL side) =====================//
//
// See dodsp_sun_shader.h. Lighting stage 3b-2 (brushes) and stage 4 (models).
//
//=========================================================================//

#include "BaseVSShader.h"
#include "renderparm.h"
#include "lightmappedgeneric_dx9_helper.h"
#include "vertexlitgeneric_dx9_helper.h"
#include "tier1/utldict.h"
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

// Binds the three cascades (or black when the client published none); true if all three are there
static bool DoDSP_BindCascades( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI )
{
	ITexture *pCascade[3];
	bool bAll = true;
	for ( int i = 0; i < 3; i++ )
	{
		pCascade[i] = (ITexture *)(intp)pShaderAPI->GetIntRenderingParameter( DODSP_RP_INT_CASCADE0 + i );
		bAll = bAll && pCascade[i];
	}
	for ( int i = 0; i < 3; i++ )
	{
		if ( bAll )
			pShader->BindTexture( s_CascadeSampler[i], pCascade[i], 0 );
		else
			pShaderAPI->BindStandardTexture( s_CascadeSampler[i], TEXTURE_BLACK );
	}
	return bAll;
}

void DoDSP_SunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, float flLightmapScale )
{
	const bool bAll = DoDSP_BindCascades( pShader, pShaderAPI );

	const Vector &vecSun = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_SUNCOLOR );
	float vSunColor[4] = { vecSun.x, vecSun.y, vecSun.z, flLightmapScale > 0.0f ? 1.0f / flLightmapScale : 1.0f };
	if ( !bAll )
		vSunColor[0] = vSunColor[1] = vSunColor[2] = 0.0f;	// no maps = no dynamic sun (the client then keeps the baked one)

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

//---------------------------------------------------------------------------------------------
// Models (stage 4)
//---------------------------------------------------------------------------------------------
COMPILE_TIME_ASSERT( DODSP_MSUN_PSREG_PLAIN + DODSP_MSUN_PSREG_COUNT <= 32 );
COMPILE_TIME_ASSERT( DODSP_MSUN_PSREG_BUMP + DODSP_MSUN_PSREG_COUNT <= 32 );
COMPILE_TIME_ASSERT( DODSP_MSUN_PSREG_SKIN + DODSP_MSUN_PSREG_COUNT <= 32 );
COMPILE_TIME_ASSERT( DODSP_MSUN_VSREG > 220 );			// bones end at 216, the engine's clip planes are 219/220
COMPILE_TIME_ASSERT( DODSP_MSUN_VSREG + DODSP_MSUN_VSREG_COUNT <= 256 );

static bool DoDSP_ModelSunConstantsFit( int nPsReg )
{
	return g_pHardwareConfig->NumPixelShaderConstants() >= nPsReg + DODSP_MSUN_PSREG_COUNT &&
		   g_pHardwareConfig->NumVertexShaderConstants() >= DODSP_MSUN_VSREG + DODSP_MSUN_VSREG_COUNT;
}

void DoDSP_ModelSunDynamicState( CBaseVSShader *pShader, IShaderDynamicAPI *pShaderAPI, int nPsReg, bool bBasePass )
{
	// INVARIANT: never write past the constants the hardware layer keeps (SHADER_FALLBACK already
	// sends materials elsewhere when they don't fit; this is the second line).
	if ( !DoDSP_ModelSunConstantsFit( nPsReg ) )
		return;

	float v[3][4];
	Q_memset( v, 0, sizeof( v ) );	// flag 0: the engine's lights are untouched
	if ( !bBasePass )
	{
		pShaderAPI->SetVertexShaderConstant( DODSP_MSUN_VSREG, v[0], DODSP_MSUN_VSREG_COUNT );
		return;
	}

	const bool bAll = DoDSP_BindCascades( pShader, pShaderAPI );
	const Vector &vecSun = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_SUNCOLOR );
	const bool bSun = bAll && ( vecSun.x > 0.0f || vecSun.y > 0.0f || vecSun.z > 0.0f );

	const Vector &vecR = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_R );
	const Vector &vecU = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_U );
	const Vector &vecF = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_AXIS_F );
	const Vector &vecDepth = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_DEPTH );	// offset, 1/texels, bias
	const Vector &vecFilter = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_FILTER );	// spread, normal offset (texels)
	Vector vecCascade[3];
	for ( int i = 0; i < 3; i++ )
		vecCascade[i] = pShaderAPI->GetVectorRenderingParameter( DODSP_RP_VEC_CASCADE0 + i );

	// Vertex shader: light-space axes; one normal offset for all cascades, sized for the mid one
	v[0][0] = vecR.x;  v[0][1] = vecR.y;  v[0][2] = vecR.z;  v[0][3] = bSun ? 1.0f : 0.0f;
	v[1][0] = -vecU.x; v[1][1] = -vecU.y; v[1][2] = -vecU.z;
	v[1][3] = ( vecCascade[1].x > 0.0f ) ? vecFilter.y * vecDepth.y / vecCascade[1].x : 0.0f;
	v[2][0] = vecF.x;  v[2][1] = vecF.y;  v[2][2] = vecF.z;  v[2][3] = vecDepth.x - vecDepth.z;
	pShaderAPI->SetVertexShaderConstant( DODSP_MSUN_VSREG, v[0], DODSP_MSUN_VSREG_COUNT );

	// Pixel shader: per cascade scale and offsets, filter offset, direction to the sun, colour
	Vector vecToSun = -vecF;
	VectorNormalize( vecToSun );
	float c[4][4];
	for ( int i = 0; i < 3; i++ )
	{
		c[i][0] = vecCascade[i].x; c[i][1] = vecCascade[i].y; c[i][2] = vecCascade[i].z;
	}
	c[0][3] = 0.5f * vecFilter.x * vecDepth.y;		// same filter spread as the brushes
	c[1][3] = vecToSun.x;
	c[2][3] = vecToSun.y;
	c[3][0] = bSun ? vecSun.x : 0.0f;			// the engine's light units: what it would give the model for the sun
	c[3][1] = bSun ? vecSun.y : 0.0f;
	c[3][2] = bSun ? vecSun.z : 0.0f;
	c[3][3] = vecToSun.z;
	pShaderAPI->SetPixelShaderConstant( nPsReg, c[0], DODSP_MSUN_PSREG_COUNT );
}

const char *DoDSP_ModelUnsupported( IMaterialVar **params, const VertexLitGeneric_DX9_Vars_t &info )
{
	if ( !g_pHardwareConfig->SupportsShaderModel_3_0() )
		return "no shader model 3.0";
	if ( !DoDSP_ModelSunConstantsFit( MAX( MAX( DODSP_MSUN_PSREG_PLAIN, DODSP_MSUN_PSREG_BUMP ), DODSP_MSUN_PSREG_SKIN ) ) )
		return "too few shader constants";

	#define DEFINED( n )	( (n) != -1 && params[n]->IsDefined() )
	#define TEXTURE( n )	( (n) != -1 && params[n]->IsDefined() && params[n]->IsTexture() )
	#define INTVAL( n )		( (n) != -1 && params[n]->IsDefined() && params[n]->GetIntValue() != 0 )
	// pinned in the plain shaders
	if ( INTVAL( info.m_nSeamlessBase ) || INTVAL( info.m_nSeamlessDetail ) )	return "$seamless_base / $seamless_detail";
	if ( INTVAL( info.m_nDistanceAlpha ) )										return "$distancealpha";
	if ( INTVAL( info.m_nDepthBlend ) )											return "$depthblend";
	if ( INTVAL( info.m_nTreeSway ) )											return "$treesway";
	// pinned in the bump and skin shaders
	if ( DEFINED( info.m_nDiffuseWarpTexture ) )								return "$lightwarptexture";
	if ( INTVAL( info.m_nSelfIllumFresnel ) )									return "$selfillumfresnel";
	if ( DEFINED( info.m_nPhongWarpTexture ) )									return "$phongwarptexture";
	if ( DEFINED( info.m_nWrinkle ) || DEFINED( info.m_nStretch ) ||
		 DEFINED( info.m_nNormalWrinkle ) || DEFINED( info.m_nNormalStretch ) )	return "wrinkle maps";
	// pinned everywhere
	if ( DEFINED( info.m_nDetail ) && DEFINED( info.m_nDetailTextureCombineMode ) &&
		 params[info.m_nDetailTextureCombineMode]->GetIntValue() > 1 )			return "$detailblendmode above 1";
	#undef DEFINED
	#undef TEXTURE
	#undef INTVAL
	return NULL;
}

int DoDSP_Pinned( const char *pszCombo, int nValue, int nMax )
{
	if ( nValue <= nMax )
		return nValue;
	static CUtlDict< bool, int > s_Warned;
	if ( s_Warned.Find( pszCombo ) == s_Warned.InvalidIndex() )
	{
		s_Warned.Insert( pszCombo, true );
		Warning( "INVARIANT: DOD-SP model shader: combo %s = %d, but DOD-SP compiled it only as 0..%d - drawn as %d. "
				 "DoDSP_ModelUnsupported should have sent this material to Mapbase's shader.\n", pszCombo, nValue, nMax, nMax );
	}
	return nMax;
}
