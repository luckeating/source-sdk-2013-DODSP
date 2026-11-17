//===================== DOD-SP VertexLitGeneric (lighting stage 4) =====================//
//
// Mapbase's SDK_VertexLitGeneric (vertexlitgeneric_dx9.cpp, Mapbase 8.0) under DOD-SP's own name,
// with the sun drawn per pixel on props and NPCs (dodsp_vertexlitgeneric_helper.cpp,
// dodsp_skin_helper.cpp; shader side dodsp_modelsun_*_fxc.h). Same parameters as Mapbase's, so the
// client's swap (c_dodsp_sdkshaders.cpp) moves a material here without changing anything about it.
//
// Only the standard pass: the cloak, emissive scroll, flesh interior and weapon sheen passes are
// left out, and a material that uses one of them - or a feature DoDSP_ModelUnsupported lists - is
// handed back to Mapbase's SDK_VertexLitGeneric by SHADER_FALLBACK.
//
//=====================================================================================//

#include "BaseVSShader.h"
#include "vertexlitgeneric_dx9_helper.h"
#include "dodsp_sun_shader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_VS_SHADER( DODSP_VertexLitGeneric, "DOD-SP: VertexLitGeneric with the sun drawn per pixel" )
	BEGIN_SHADER_PARAMS
		SHADER_PARAM( ALBEDO, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "albedo (Base texture with no baked lighting)" )
		SHADER_PARAM( COMPRESS, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "compression wrinklemap" )
		SHADER_PARAM( STRETCH, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "expansion wrinklemap" )
		SHADER_PARAM( SELFILLUMTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Self-illumination tint" )
		SHADER_PARAM( DETAIL, SHADER_PARAM_TYPE_TEXTURE, "shadertest/detail", "detail texture" )
		SHADER_PARAM( DETAILFRAME, SHADER_PARAM_TYPE_INTEGER, "0", "frame number for $detail" )
		SHADER_PARAM( DETAILSCALE, SHADER_PARAM_TYPE_FLOAT, "4", "scale of the detail texture" )
		SHADER_PARAM( ENVMAP, SHADER_PARAM_TYPE_TEXTURE, "shadertest/shadertest_env", "envmap" )
		SHADER_PARAM( ENVMAPFRAME, SHADER_PARAM_TYPE_INTEGER, "0", "envmap frame number" )
		SHADER_PARAM( ENVMAPMASK, SHADER_PARAM_TYPE_TEXTURE, "shadertest/shadertest_envmask", "envmap mask" )
		SHADER_PARAM( ENVMAPMASKFRAME, SHADER_PARAM_TYPE_INTEGER, "0", "" )
		SHADER_PARAM( ENVMAPMASKTRANSFORM, SHADER_PARAM_TYPE_MATRIX, "center .5 .5 scale 1 1 rotate 0 translate 0 0", "$envmapmask texcoord transform" )
		SHADER_PARAM( ENVMAPTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "envmap tint" )
		SHADER_PARAM( BUMPMAP, SHADER_PARAM_TYPE_TEXTURE, "models/shadertest/shader1_normal", "bump map" )
		SHADER_PARAM( BUMPCOMPRESS, SHADER_PARAM_TYPE_TEXTURE, "models/shadertest/shader3_normal", "compression bump map" )
		SHADER_PARAM( BUMPSTRETCH, SHADER_PARAM_TYPE_TEXTURE, "models/shadertest/shader1_normal", "expansion bump map" )
		SHADER_PARAM( BUMPFRAME, SHADER_PARAM_TYPE_INTEGER, "0", "frame number for $bumpmap" )
		SHADER_PARAM( BUMPTRANSFORM, SHADER_PARAM_TYPE_MATRIX, "center .5 .5 scale 1 1 rotate 0 translate 0 0", "$bumpmap texcoord transform" )
		SHADER_PARAM( ENVMAPCONTRAST, SHADER_PARAM_TYPE_FLOAT, "0.0", "contrast 0 == normal 1 == color*color" )
		SHADER_PARAM( ENVMAPSATURATION, SHADER_PARAM_TYPE_FLOAT, "1.0", "saturation 0 == greyscale 1 == normal" )
 	    SHADER_PARAM( SELFILLUM_ENVMAPMASK_ALPHA, SHADER_PARAM_TYPE_FLOAT,"0.0","defines that self illum value comes from env map mask alpha" )
		SHADER_PARAM( SELFILLUMFRESNEL, SHADER_PARAM_TYPE_BOOL, "0", "Self illum fresnel" )
		SHADER_PARAM( SELFILLUMFRESNELMINMAXEXP, SHADER_PARAM_TYPE_VEC4, "0", "Self illum fresnel min, max, exp" )
		SHADER_PARAM( ALPHATESTREFERENCE, SHADER_PARAM_TYPE_FLOAT, "0.0", "" )	
		SHADER_PARAM( FLASHLIGHTNOLAMBERT, SHADER_PARAM_TYPE_BOOL, "0", "Flashlight pass sets N.L=1.0" )

		// Debugging term for visualizing ambient data on its own
		SHADER_PARAM( AMBIENTONLY, SHADER_PARAM_TYPE_INTEGER, "0", "Control drawing of non-ambient light ()" )

		SHADER_PARAM( PHONGEXPONENT, SHADER_PARAM_TYPE_FLOAT, "5.0", "Phong exponent for local specular lights" )
		SHADER_PARAM( PHONGTINT, SHADER_PARAM_TYPE_VEC3, "5.0", "Phong tint for local specular lights" )
		SHADER_PARAM( PHONGALBEDOTINT, SHADER_PARAM_TYPE_BOOL, "1.0", "Apply tint by albedo (controlled by spec exponent texture" )
		SHADER_PARAM( LIGHTWARPTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "1D ramp texture for tinting scalar diffuse term" )
		SHADER_PARAM( PHONGWARPTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "warp the specular term" )
		SHADER_PARAM( PHONGFRESNELRANGES, SHADER_PARAM_TYPE_VEC3, "[0  0.5  1]", "Parameters for remapping fresnel output" )
		SHADER_PARAM( PHONGBOOST, SHADER_PARAM_TYPE_FLOAT, "1.0", "Phong overbrightening factor (specular mask channel should be authored to account for this)" )
		SHADER_PARAM( PHONGEXPONENTTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "Phong Exponent map" )
		SHADER_PARAM( PHONG, SHADER_PARAM_TYPE_BOOL, "0", "enables phong lighting" )
		SHADER_PARAM( BASEMAPALPHAPHONGMASK, SHADER_PARAM_TYPE_INTEGER, "0", "indicates that there is no normal map and that the phong mask is in base alpha" )
		SHADER_PARAM( INVERTPHONGMASK, SHADER_PARAM_TYPE_INTEGER, "0", "invert the phong mask (0=full phong, 1=no phong)" )
		SHADER_PARAM( ENVMAPFRESNEL, SHADER_PARAM_TYPE_FLOAT, "0", "Degree to which Fresnel should be applied to env map" )
		SHADER_PARAM( SELFILLUMMASK, SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", "If we bind a texture here, it overrides base alpha (if any) for self illum" )

	    // detail (multi-) texturing
	    SHADER_PARAM( DETAILBLENDMODE, SHADER_PARAM_TYPE_INTEGER, "0", "mode for combining detail texture with base. 0=normal, 1= additive, 2=alpha blend detail over base, 3=crossfade" )
		SHADER_PARAM( DETAILBLENDFACTOR, SHADER_PARAM_TYPE_FLOAT, "1", "blend amount for detail texture." )
		SHADER_PARAM( DETAILTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "detail texture tint" )
		SHADER_PARAM( DETAILTEXTURETRANSFORM, SHADER_PARAM_TYPE_MATRIX, "center .5 .5 scale 1 1 rotate 0 translate 0 0", "$detail texcoord transform" )

		// Rim lighting terms
		SHADER_PARAM( RIMLIGHT, SHADER_PARAM_TYPE_BOOL, "0", "enables rim lighting" )
		SHADER_PARAM( RIMLIGHTEXPONENT, SHADER_PARAM_TYPE_FLOAT, "4.0", "Exponent for rim lights" )
		SHADER_PARAM( RIMLIGHTBOOST, SHADER_PARAM_TYPE_FLOAT, "1.0", "Boost for rim lights" )
		SHADER_PARAM( RIMMASK, SHADER_PARAM_TYPE_BOOL, "0", "Indicates whether or not to use alpha channel of exponent texture to mask the rim term" )

	    // Seamless mapping scale
		SHADER_PARAM( SEAMLESS_BASE, SHADER_PARAM_TYPE_BOOL, "0", "whether to apply seamless mapping to the base texture. requires a smooth model." )
		SHADER_PARAM( SEAMLESS_DETAIL, SHADER_PARAM_TYPE_BOOL, "0", "where to apply seamless mapping to the detail texture." )
	    SHADER_PARAM( SEAMLESS_SCALE, SHADER_PARAM_TYPE_FLOAT, "1.0", "the scale for the seamless mapping. # of repetions of texture per inch." )

		// Emissive Scroll Pass
		SHADER_PARAM( EMISSIVEBLENDENABLED, SHADER_PARAM_TYPE_BOOL, "0", "Enable emissive blend pass" )
		SHADER_PARAM( EMISSIVEBLENDBASETEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "self-illumination map" )
		SHADER_PARAM( EMISSIVEBLENDSCROLLVECTOR, SHADER_PARAM_TYPE_VEC2, "[0.11 0.124]", "Emissive scroll vec" )
		SHADER_PARAM( EMISSIVEBLENDSTRENGTH, SHADER_PARAM_TYPE_FLOAT, "1.0", "Emissive blend strength" )
		SHADER_PARAM( EMISSIVEBLENDTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "self-illumination map" )
		SHADER_PARAM( EMISSIVEBLENDTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Self-illumination tint" )
		SHADER_PARAM( EMISSIVEBLENDFLOWTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "flow map" )
		SHADER_PARAM( TIME, SHADER_PARAM_TYPE_FLOAT, "0.0", "Needs CurrentTime Proxy" )

		// Cloak Pass
		SHADER_PARAM( CLOAKPASSENABLED, SHADER_PARAM_TYPE_BOOL, "0", "Enables cloak render in a second pass" )
		SHADER_PARAM( CLOAKFACTOR, SHADER_PARAM_TYPE_FLOAT, "0.0", "" )
		SHADER_PARAM( CLOAKCOLORTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Cloak color tint" )
		SHADER_PARAM( REFRACTAMOUNT, SHADER_PARAM_TYPE_FLOAT, "2", "" )

#ifdef GAME_SHADER_DLL
		// Weapon Sheen Pass
		SHADER_PARAM( SHEENPASSENABLED, SHADER_PARAM_TYPE_BOOL, "0", "Enables weapon sheen render in a second pass" )
		SHADER_PARAM( SHEENMAP, SHADER_PARAM_TYPE_TEXTURE, "shadertest/shadertest_env", "sheenmap" )
		SHADER_PARAM( SHEENMAPMASK, SHADER_PARAM_TYPE_TEXTURE, "shadertest/shadertest_envmask", "sheenmap mask" )
		SHADER_PARAM( SHEENMAPMASKFRAME, SHADER_PARAM_TYPE_INTEGER, "0", "" )
		SHADER_PARAM( SHEENMAPTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "sheenmap tint" )
		SHADER_PARAM( SHEENMAPMASKSCALEX, SHADER_PARAM_TYPE_FLOAT, "1", "X Scale the size of the map mask to the size of the target" )
		SHADER_PARAM( SHEENMAPMASKSCALEY, SHADER_PARAM_TYPE_FLOAT, "1", "Y Scale the size of the map mask to the size of the target" )
		SHADER_PARAM( SHEENMAPMASKOFFSETX, SHADER_PARAM_TYPE_FLOAT, "0", "X Offset of the mask relative to model space coords of target" )
		SHADER_PARAM( SHEENMAPMASKOFFSETY, SHADER_PARAM_TYPE_FLOAT, "0", "Y Offset of the mask relative to model space coords of target" )
		SHADER_PARAM( SHEENMAPMASKDIRECTION, SHADER_PARAM_TYPE_INTEGER, "0", "The direction the sheen should move (length direction of weapon) XYZ, 0,1,2" )
		SHADER_PARAM( SHEENINDEX, SHADER_PARAM_TYPE_INTEGER, "0", "Index of the Effect Type (Color Additive, Override etc...)" )
#endif

		// Flesh Interior Pass
		SHADER_PARAM( FLESHINTERIORENABLED, SHADER_PARAM_TYPE_BOOL, "0", "Enable Flesh interior blend pass" )
		SHADER_PARAM( FLESHINTERIORTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh color texture" )
		SHADER_PARAM( FLESHINTERIORNOISETEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh noise texture" )
		SHADER_PARAM( FLESHBORDERTEXTURE1D, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh border 1D texture" )
		SHADER_PARAM( FLESHNORMALTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh normal texture" )
		SHADER_PARAM( FLESHSUBSURFACETEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh subsurface texture" )
		SHADER_PARAM( FLESHCUBETEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Flesh cubemap texture" )
		SHADER_PARAM( FLESHBORDERNOISESCALE, SHADER_PARAM_TYPE_FLOAT, "1.5", "Flesh Noise UV scalar for border" )
		SHADER_PARAM( FLESHDEBUGFORCEFLESHON, SHADER_PARAM_TYPE_BOOL, "0", "Flesh Debug full flesh" )
		SHADER_PARAM( FLESHEFFECTCENTERRADIUS1, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0.001]", "Flesh effect center and radius" )
		SHADER_PARAM( FLESHEFFECTCENTERRADIUS2, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0.001]", "Flesh effect center and radius" )
		SHADER_PARAM( FLESHEFFECTCENTERRADIUS3, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0.001]", "Flesh effect center and radius" )
		SHADER_PARAM( FLESHEFFECTCENTERRADIUS4, SHADER_PARAM_TYPE_VEC4, "[0 0 0 0.001]", "Flesh effect center and radius" )
		SHADER_PARAM( FLESHSUBSURFACETINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Subsurface Color" )
		SHADER_PARAM( FLESHBORDERWIDTH, SHADER_PARAM_TYPE_FLOAT, "0.3", "Flesh border" )
		SHADER_PARAM( FLESHBORDERSOFTNESS, SHADER_PARAM_TYPE_FLOAT, "0.42", "Flesh border softness (> 0.0 && <= 0.5)" )
		SHADER_PARAM( FLESHBORDERTINT, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Flesh border Color" )
		SHADER_PARAM( FLESHGLOBALOPACITY, SHADER_PARAM_TYPE_FLOAT, "1.0", "Flesh global opacity" )
		SHADER_PARAM( FLESHGLOSSBRIGHTNESS, SHADER_PARAM_TYPE_FLOAT, "0.66", "Flesh gloss brightness" )
		SHADER_PARAM( FLESHSCROLLSPEED, SHADER_PARAM_TYPE_FLOAT, "1.0", "Flesh scroll speed" )

		SHADER_PARAM( SEPARATEDETAILUVS, SHADER_PARAM_TYPE_BOOL, "0", "Use texcoord1 for detail texture" )
		SHADER_PARAM( LINEARWRITE, SHADER_PARAM_TYPE_INTEGER, "0", "Disables SRGB conversion of shader results." )
		SHADER_PARAM( DEPTHBLEND, SHADER_PARAM_TYPE_INTEGER, "0", "fade at intersection boundaries. Only supported without bumpmaps" )
		SHADER_PARAM( DEPTHBLENDSCALE, SHADER_PARAM_TYPE_FLOAT, "50.0", "Amplify or reduce DEPTHBLEND fading. Lower values make harder edges." )

		SHADER_PARAM( BLENDTINTBYBASEALPHA, SHADER_PARAM_TYPE_BOOL, "0", "Use the base alpha to blend in the $color modulation")
		SHADER_PARAM( BLENDTINTCOLOROVERBASE, SHADER_PARAM_TYPE_FLOAT, "0", "blend between tint acting as a multiplication versus a replace" )

#ifdef MAPBASE
		SHADER_PARAM( ALLOWDIFFUSEMODULATION, SHADER_PARAM_TYPE_BOOL, "1", "Allow per-instance color modulation" )

		SHADER_PARAM( ENVMAPFRESNELMINMAXEXP, SHADER_PARAM_TYPE_VEC3, "[0.0 1.0 2.0]", "Min/max fresnel range and exponent for vertexlitgeneric" )
		SHADER_PARAM( BASEALPHAENVMAPMASKMINMAXEXP, SHADER_PARAM_TYPE_VEC3, "[1.0 0.0 1.0]", "" )

		SHADER_PARAM( PHONGDISABLEHALFLAMBERT, SHADER_PARAM_TYPE_BOOL, "0", "Disable half lambert for phong" )
#endif

		// vertexlitgeneric tree sway animation control
		SHADER_PARAM( TREESWAY, SHADER_PARAM_TYPE_INTEGER, "0", "" )
		SHADER_PARAM( TREESWAYHEIGHT, SHADER_PARAM_TYPE_FLOAT, "1000", "" )
		SHADER_PARAM( TREESWAYSTARTHEIGHT, SHADER_PARAM_TYPE_FLOAT, "0.2", "" )
		SHADER_PARAM( TREESWAYRADIUS, SHADER_PARAM_TYPE_FLOAT, "300", "" )
		SHADER_PARAM( TREESWAYSTARTRADIUS, SHADER_PARAM_TYPE_FLOAT, "0.1", "" )
		SHADER_PARAM( TREESWAYSPEED, SHADER_PARAM_TYPE_FLOAT, "1", "" )
		SHADER_PARAM( TREESWAYSPEEDHIGHWINDMULTIPLIER, SHADER_PARAM_TYPE_FLOAT, "2", "" )
		SHADER_PARAM( TREESWAYSTRENGTH, SHADER_PARAM_TYPE_FLOAT, "10", "" )
		SHADER_PARAM( TREESWAYSCRUMBLESPEED, SHADER_PARAM_TYPE_FLOAT, "0.1", "" )
		SHADER_PARAM( TREESWAYSCRUMBLESTRENGTH, SHADER_PARAM_TYPE_FLOAT, "0.1", "" )
		SHADER_PARAM( TREESWAYSCRUMBLEFREQUENCY, SHADER_PARAM_TYPE_FLOAT, "0.1", "" )
		SHADER_PARAM( TREESWAYFALLOFFEXP, SHADER_PARAM_TYPE_FLOAT, "1.5", "" )
		SHADER_PARAM( TREESWAYSCRUMBLEFALLOFFEXP, SHADER_PARAM_TYPE_FLOAT, "1.0", "" )
		SHADER_PARAM( TREESWAYSPEEDLERPSTART, SHADER_PARAM_TYPE_FLOAT, "3", "" )
		SHADER_PARAM( TREESWAYSPEEDLERPEND, SHADER_PARAM_TYPE_FLOAT, "6", "" )
		SHADER_PARAM( TREESWAYSTATIC, SHADER_PARAM_TYPE_BOOL, "0", "" )
		SHADER_PARAM( TREESWAYSTATICVALUES, SHADER_PARAM_TYPE_VEC2, "[0.5 0.5]", "" )
	END_SHADER_PARAMS

	void SetupVars( VertexLitGeneric_DX9_Vars_t& info )
	{
		info.m_nBaseTexture = BASETEXTURE;
		info.m_nWrinkle = COMPRESS;
		info.m_nStretch = STRETCH;
		info.m_nBaseTextureFrame = FRAME;
		info.m_nBaseTextureTransform = BASETEXTURETRANSFORM;
		info.m_nAlbedo = ALBEDO;
		info.m_nSelfIllumTint = SELFILLUMTINT;
		info.m_nDetail = DETAIL;
		info.m_nDetailFrame = DETAILFRAME;
		info.m_nDetailScale = DETAILSCALE;
		info.m_nEnvmap = ENVMAP;
		info.m_nEnvmapFrame = ENVMAPFRAME;
		info.m_nEnvmapMask = ENVMAPMASK;
		info.m_nEnvmapMaskFrame = ENVMAPMASKFRAME;
		info.m_nEnvmapMaskTransform = ENVMAPMASKTRANSFORM;
		info.m_nEnvmapTint = ENVMAPTINT;
		info.m_nBumpmap = BUMPMAP;
		info.m_nNormalWrinkle = BUMPCOMPRESS;
		info.m_nNormalStretch = BUMPSTRETCH;
		info.m_nBumpFrame = BUMPFRAME;
		info.m_nBumpTransform = BUMPTRANSFORM;
		info.m_nEnvmapContrast = ENVMAPCONTRAST;
		info.m_nEnvmapSaturation = ENVMAPSATURATION;
		info.m_nAlphaTestReference = ALPHATESTREFERENCE;
		info.m_nFlashlightNoLambert = FLASHLIGHTNOLAMBERT;

		info.m_nFlashlightTexture = FLASHLIGHTTEXTURE;
		info.m_nFlashlightTextureFrame = FLASHLIGHTTEXTUREFRAME;
		info.m_nSelfIllumEnvMapMask_Alpha = SELFILLUM_ENVMAPMASK_ALPHA;
		info.m_nSelfIllumFresnel = SELFILLUMFRESNEL;
		info.m_nSelfIllumFresnelMinMaxExp = SELFILLUMFRESNELMINMAXEXP;

		info.m_nAmbientOnly = AMBIENTONLY;
		info.m_nPhongExponent = PHONGEXPONENT;
		info.m_nPhongExponentTexture = PHONGEXPONENTTEXTURE;
		info.m_nPhongTint = PHONGTINT;
		info.m_nPhongAlbedoTint = PHONGALBEDOTINT;
		info.m_nDiffuseWarpTexture = LIGHTWARPTEXTURE;
		info.m_nPhongWarpTexture = PHONGWARPTEXTURE;
		info.m_nPhongBoost = PHONGBOOST;
		info.m_nPhongFresnelRanges = PHONGFRESNELRANGES;
		info.m_nPhong = PHONG;
		info.m_nBaseMapAlphaPhongMask = BASEMAPALPHAPHONGMASK;
		info.m_nEnvmapFresnel = ENVMAPFRESNEL;
		info.m_nDetailTextureCombineMode = DETAILBLENDMODE;
		info.m_nDetailTextureBlendFactor = DETAILBLENDFACTOR;
		info.m_nDetailTextureTransform = DETAILTEXTURETRANSFORM;

		// Rim lighting parameters
		info.m_nRimLight = RIMLIGHT;
		info.m_nRimLightPower = RIMLIGHTEXPONENT;
		info.m_nRimLightBoost = RIMLIGHTBOOST;
		info.m_nRimMask = RIMMASK;

		// seamless
		info.m_nSeamlessScale = SEAMLESS_SCALE;
		info.m_nSeamlessDetail = SEAMLESS_DETAIL;
		info.m_nSeamlessBase = SEAMLESS_BASE;

		info.m_nSeparateDetailUVs = SEPARATEDETAILUVS;

		info.m_nLinearWrite = LINEARWRITE;
		info.m_nDetailTint = DETAILTINT;
		info.m_nInvertPhongMask = INVERTPHONGMASK;

		info.m_nDepthBlend = DEPTHBLEND;
		info.m_nDepthBlendScale = DEPTHBLENDSCALE;

		info.m_nSelfIllumMask = SELFILLUMMASK;
		info.m_nBlendTintByBaseAlpha = BLENDTINTBYBASEALPHA;
		info.m_nTintReplacesBaseColor = BLENDTINTCOLOROVERBASE;

#ifdef MAPBASE
		info.m_nAllowDiffuseModulation = ALLOWDIFFUSEMODULATION;

		info.m_nEnvMapFresnelMinMaxExp = ENVMAPFRESNELMINMAXEXP;
		info.m_nBaseAlphaEnvMapMaskMinMaxExp = BASEALPHAENVMAPMASKMINMAXEXP;

		info.m_nPhongDisableHalfLambert = PHONGDISABLEHALFLAMBERT;
#endif

		info.m_nTreeSway = TREESWAY;
		info.m_nTreeSwayHeight = TREESWAYHEIGHT;
		info.m_nTreeSwayStartHeight = TREESWAYSTARTHEIGHT;
		info.m_nTreeSwayRadius = TREESWAYRADIUS;
		info.m_nTreeSwayStartRadius = TREESWAYSTARTRADIUS;
		info.m_nTreeSwaySpeed = TREESWAYSPEED;
		info.m_nTreeSwaySpeedHighWindMultiplier = TREESWAYSPEEDHIGHWINDMULTIPLIER;
		info.m_nTreeSwayStrength = TREESWAYSTRENGTH;
		info.m_nTreeSwayScrumbleSpeed = TREESWAYSCRUMBLESPEED;
		info.m_nTreeSwayScrumbleStrength = TREESWAYSCRUMBLESTRENGTH;
		info.m_nTreeSwayScrumbleFrequency = TREESWAYSCRUMBLEFREQUENCY;
		info.m_nTreeSwayFalloffExp = TREESWAYFALLOFFEXP;
		info.m_nTreeSwayScrumbleFalloffExp = TREESWAYSCRUMBLEFALLOFFEXP;
		info.m_nTreeSwaySpeedLerpStart = TREESWAYSPEEDLERPSTART;
		info.m_nTreeSwaySpeedLerpEnd = TREESWAYSPEEDLERPEND;
		info.m_nTreeSwayStatic = TREESWAYSTATIC;
		info.m_nTreeSwayStaticValues = TREESWAYSTATICVALUES;
	}

	SHADER_INIT_PARAMS()
	{
		VertexLitGeneric_DX9_Vars_t vars;
		SetupVars( vars );
		InitParamsVertexLitGeneric_DX9( this, params, pMaterialName, true, vars );

		// the extra passes stay off here (SHADER_FALLBACK hands materials that enable them to Mapbase)
		if ( !params[CLOAKPASSENABLED]->IsDefined() )
			params[CLOAKPASSENABLED]->SetIntValue( 0 );
		if ( !params[EMISSIVEBLENDENABLED]->IsDefined() )
			params[EMISSIVEBLENDENABLED]->SetIntValue( 0 );
		if ( !params[FLESHINTERIORENABLED]->IsDefined() )
			params[FLESHINTERIORENABLED]->SetIntValue( 0 );
#ifdef GAME_SHADER_DLL
		if ( !params[SHEENPASSENABLED]->IsDefined() )
			params[SHEENPASSENABLED]->SetIntValue( 0 );
#endif
	}

	SHADER_FALLBACK
	{
		if ( params[CLOAKPASSENABLED]->IsDefined() && params[CLOAKPASSENABLED]->GetIntValue() )
			return "SDK_VertexLitGeneric";
		if ( params[EMISSIVEBLENDENABLED]->IsDefined() && params[EMISSIVEBLENDENABLED]->GetIntValue() )
			return "SDK_VertexLitGeneric";
		if ( params[FLESHINTERIORENABLED]->IsDefined() && params[FLESHINTERIORENABLED]->GetIntValue() )
			return "SDK_VertexLitGeneric";
#ifdef GAME_SHADER_DLL
		if ( params[SHEENPASSENABLED]->IsDefined() && params[SHEENPASSENABLED]->GetIntValue() )
			return "SDK_VertexLitGeneric";
#endif
		VertexLitGeneric_DX9_Vars_t vars;
		const_cast< CShader * >( this )->SetupVars( vars );
		if ( DoDSP_ModelUnsupported( params, vars ) )
			return "SDK_VertexLitGeneric";
		return 0;
	}

	SHADER_INIT
	{
		VertexLitGeneric_DX9_Vars_t vars;
		SetupVars( vars );
		InitVertexLitGeneric_DX9( this, params, true, vars );
	}

	SHADER_DRAW
	{
		VertexLitGeneric_DX9_Vars_t vars;
		SetupVars( vars );
		DrawVertexLitGeneric_DX9( this, params, pShaderAPI, pShaderShadow, true, vars, vertexCompression, pContextDataPtr );
	}
END_SHADER
