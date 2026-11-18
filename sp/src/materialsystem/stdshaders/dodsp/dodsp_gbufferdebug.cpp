//===================== DOD-SP lamps: G-buffer debug view (stage 5a) =====================//
//
// Draws one target of the lamps' G-buffer on screen (dodsp_lamps_view, c_dodsp_lamps.cpp). See
// dodsp_gbufferdebug_ps30.fxc for what the pictures mean. Never used on map materials.
//
// $basetexture = the G-buffer target, $params = x mode (0 albedo, 1 normal, 2 distance),
// y = 1 / the distance drawn white.
//
//=========================================================================//

#include "BaseVSShader.h"
#include "dodsp_screenspace_vs30.inc"
#include "dodsp_gbufferdebug_ps30.inc"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_VS_SHADER( DODSP_GBufferDebug, "DOD-SP stage 5a: draws one target of the lamps' G-buffer (debug only)" )

	BEGIN_SHADER_PARAMS
		SHADER_PARAM( PARAMS, SHADER_PARAM_TYPE_VEC4, "[0 0.001 0 0]", "x = mode (0 albedo, 1 normal, 2 distance), y = 1 / distance drawn white" )
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
		if ( !params[PARAMS]->IsDefined() )
			params[PARAMS]->SetVecValue( 0.0f, 0.001f, 0.0f, 0.0f );
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
			pShaderShadow->VertexShaderVertexFormat( VERTEX_POSITION, 1, NULL, 0 );

			DECLARE_STATIC_VERTEX_SHADER( dodsp_screenspace_vs30 );
			SET_STATIC_VERTEX_SHADER( dodsp_screenspace_vs30 );

			DECLARE_STATIC_PIXEL_SHADER( dodsp_gbufferdebug_ps30 );
			SET_STATIC_PIXEL_SHADER( dodsp_gbufferdebug_ps30 );
		}
		DYNAMIC_STATE
		{
			BindTexture( SHADER_SAMPLER0, BASETEXTURE, -1 );

			float vParams[4];
			params[PARAMS]->GetVecValue( vParams, 4 );
			pShaderAPI->SetPixelShaderConstant( 0, vParams, 1 );

			DECLARE_DYNAMIC_VERTEX_SHADER( dodsp_screenspace_vs30 );
			SET_DYNAMIC_VERTEX_SHADER( dodsp_screenspace_vs30 );

			DECLARE_DYNAMIC_PIXEL_SHADER( dodsp_gbufferdebug_ps30 );
			SET_DYNAMIC_PIXEL_SHADER( dodsp_gbufferdebug_ps30 );
		}
		Draw();
	}

END_SHADER
