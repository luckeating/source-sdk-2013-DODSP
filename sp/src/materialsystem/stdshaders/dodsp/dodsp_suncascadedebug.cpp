//===================== DOD-SP sun cascade debug view =====================//
// DOD-SP crash hunt: the CI builds extra test DLLs with DODSP_BISECT_* defined to leave parts out. Normal builds are unaffected.
#if !defined( DODSP_BISECT_NO_DEBUG )
//
// Lighting stage 3b-1. The first DOD-SP shader with compiled shader files (dodsp_*.vcs), so it
// proves the ShaderCompile2 -> .vcs -> game pipeline, and it shows the sun cascades on screen with
// a marker that checks the world -> cascade matrix against the scene. See
// dodsp_suncascadedebug_ps30.fxc for what the picture means. Drawn by the client
// (c_dodsp_sun_cascades.cpp, dodsp_csm_view 1); never used on map materials.
//
// $basetexture = the cascade's depth render target, $marker = u v depth draw, $params = texture
// units per screen pixel, depth bias.
//
//=========================================================================//

#include "BaseVSShader.h"
#include "dodsp_screenspace_vs30.inc"
#include "dodsp_suncascadedebug_ps30.inc"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_VS_SHADER( DODSP_SunCascadeDebug, "DOD-SP stage 3b-1: draws a sun cascade with the aim-point marker (debug only)" )

	BEGIN_SHADER_PARAMS
		SHADER_PARAM( MARKER, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0]", "aim point in this cascade: u, v, depth, 1 = draw" )
		SHADER_PARAM( PARAMS, SHADER_PARAM_TYPE_VEC4, "[0.004 0.002 0 0]", "x = texture units per screen pixel, y = depth bias" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if ( !params[MARKER]->IsDefined() )
			params[MARKER]->SetVecValue( 0.0f, 0.0f, 0.0f, 0.0f );
		if ( !params[PARAMS]->IsDefined() )
			params[PARAMS]->SetVecValue( 0.004f, 0.002f, 0.0f, 0.0f );
	}

	SHADER_FALLBACK
	{
		return 0;
	}

	SHADER_INIT
	{
		if ( params[BASETEXTURE]->IsDefined() )
			LoadTexture( BASETEXTURE );
	}

	SHADER_DRAW
	{
		SHADOW_STATE
		{
			pShaderShadow->EnableDepthWrites( false );
			pShaderShadow->EnableDepthTest( false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
			pShaderShadow->SetShadowDepthFiltering( SHADER_SAMPLER0 );
			pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, NULL, 0 );

			DECLARE_STATIC_VERTEX_SHADER( dodsp_screenspace_vs30 );
			SET_STATIC_VERTEX_SHADER( dodsp_screenspace_vs30 );

			DECLARE_STATIC_PIXEL_SHADER( dodsp_suncascadedebug_ps30 );
			SET_STATIC_PIXEL_SHADER_COMBO( FILTERMODE, g_pHardwareConfig->GetShadowFilterMode() );
			SET_STATIC_PIXEL_SHADER( dodsp_suncascadedebug_ps30 );
		}
		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );

			float vMarker[4], vParams[4];
			params[MARKER]->GetVecValue( vMarker, 4 );
			params[PARAMS]->GetVecValue( vParams, 4 );
			pShaderAPI->SetPixelShaderConstant( 0, vMarker, 1 );
			pShaderAPI->SetPixelShaderConstant( 1, vParams, 1 );

			DECLARE_DYNAMIC_VERTEX_SHADER( dodsp_screenspace_vs30 );
			SET_DYNAMIC_VERTEX_SHADER( dodsp_screenspace_vs30 );

			DECLARE_DYNAMIC_PIXEL_SHADER( dodsp_suncascadedebug_ps30 );
			SET_DYNAMIC_PIXEL_SHADER( dodsp_suncascadedebug_ps30 );
		}
		Draw();
	}

END_SHADER
#endif // bisect switch
