//===================== DOD-SP model sun: vertex shader side (stage 4) =====================//
//
// Models (props and NPCs) get the sun per pixel like the brushes. The pixel shaders of the model
// families have only 4 free constants, so the vertex shader does the cascade maths it can: it
// places each vertex in the sun's light space (the cascades share their axes) and hands that to the
// pixel shader in one interpolator (DoDSP_ModelSunCoord). The pixel side is dodsp_modelsun_ps_fxc.h.
//
// One sun at a time: the engine lights a model with its own directional light for the sun, traced
// once for the whole model. While DOD-SP's sun draws (g_DoDSP_VSun0.w = 1), every directional light
// the engine hands the shader is dropped (DoDSP_* replacements of common_vs_fxc.h's lighting calls).
// With dodsp_sun_baked 1 the flag is 0 and the engine's lights are untouched.
//
// Constants (set by DoDSP_ModelSunDynamicState, dodsp_sun_shader.cpp - the numbers must match it):
//   c221  xyz = light-space right axis, w = 1 while DOD-SP's sun draws
//   c222  xyz = MINUS the light-space up axis (so v needs no sign), w = normal offset in units
//   c223  xyz = light-space forward axis (0..1 over the depth range), w = depth offset - bias
//   c224  x = 1 when vertices with VRAD's baked lighting (STATIC_LIGHT: static props) take the per-pixel
//         sun - the map's static props were baked without it (stage 4d); yzw unused
// Which flag a vertex follows: baked-lit vertices c224.x, every other vertex c221.w. A vertex that
// takes our sun drops the engine's directional lights, so it never gets two suns.
// INVARIANT: c217..c220 are the engine's (clip planes), bones end at c216; c221..c224 are free in
// every Mapbase vertex shader (CI checks the vertex range, the C++ checks the hardware's count).
//
//=====================================================================//
#ifndef DODSP_MODELSUN_VS_FXC_H
#define DODSP_MODELSUN_VS_FXC_H

const float4 g_DoDSP_VSun0			: register( c221 );
const float4 g_DoDSP_VSun1			: register( c222 );
const float4 g_DoDSP_VSun2			: register( c223 );
const float4 g_DoDSP_VSun3			: register( c224 );

// 1 if this vertex takes the per-pixel sun. bStaticLit: the vertex has VRAD's baked lighting
// (STATIC_LIGHT). Its sun follows the map: baked in (old maps, c224.x = 0) or left to us (c224.x = 1).
float DoDSP_TakesSun( bool bStaticLit )
{
	return bStaticLit ? g_DoDSP_VSun3.x : g_DoDSP_VSun0.w;
}

// xy = light-space position across the sun, z = depth, w = 1 if this vertex takes the per-pixel sun
float4 DoDSP_ModelSunCoord( float3 worldPos, float3 worldNormal, bool bStaticLit )
{
	float3 p = worldPos + worldNormal * g_DoDSP_VSun1.w;	// normal offset: off the surface by about a texel
	return float4( dot( p, g_DoDSP_VSun0.xyz ), dot( p, g_DoDSP_VSun1.xyz ),
				   dot( p, g_DoDSP_VSun2.xyz ) + g_DoDSP_VSun2.w,
				   DoDSP_TakesSun( bStaticLit ) );
}

// 0 for the engine's directional lights on a vertex that takes our sun, 1 otherwise
// (cLightInfo[].color.w is 1 for a directional light, 0 for point and spot)
float DoDSP_EngineLightKeep( int lightNum, bool bStaticLit )
{
	return 1.0f - DoDSP_TakesSun( bStaticLit ) * cLightInfo[lightNum].color.w;
}

// The bump and skin shaders: no baked vertex lighting
float DoDSP_GetVertexAttenForLight( const float3 worldPos, int lightNum, bool bUseStaticControlFlow )
{
	return GetVertexAttenForLight( worldPos, lightNum, bUseStaticControlFlow ) * DoDSP_EngineLightKeep( lightNum, false );
}

float3 DoDSP_DoLightInternal( const float3 worldPos, const float3 worldNormal, int lightNum, bool bHalfLambert, bool bStaticLit )
{
	return DoLightInternal( worldPos, worldNormal, lightNum, bHalfLambert ) * DoDSP_EngineLightKeep( lightNum, bStaticLit );
}

// common_vs_fxc.h's DoLighting / DoLightingUnrolled with the engine's sun dropped while ours draws
float3 DoDSP_DoLighting( const float3 worldPos, const float3 worldNormal,
						 const float3 staticLightingColor, const bool bStaticLight,
						 const bool bDynamicLight, bool bHalfLambert )
{
	float3 linearColor = float3( 0.0f, 0.0f, 0.0f );
	if ( bStaticLight )
		linearColor += GammaToLinear( staticLightingColor * cOverbright );
	if ( bDynamicLight )
	{
		for ( int i = 0; i < g_nLightCount; i++ )
			linearColor += DoDSP_DoLightInternal( worldPos, worldNormal, i, bHalfLambert, bStaticLight );
		linearColor += AmbientLight( worldNormal );
	}
	return linearColor;
}

float3 DoDSP_DoLightingUnrolled( const float3 worldPos, const float3 worldNormal,
								 const float3 staticLightingColor, const bool bStaticLight,
								 const bool bDynamicLight, bool bHalfLambert, const int nNumLights )
{
	float3 linearColor = float3( 0.0f, 0.0f, 0.0f );
	if ( bStaticLight )
		linearColor += GammaToLinear( staticLightingColor * cOverbright );
	if ( bDynamicLight )
	{
		if ( nNumLights >= 1 )
			linearColor += DoDSP_DoLightInternal( worldPos, worldNormal, 0, bHalfLambert, bStaticLight );
		if ( nNumLights >= 2 )
			linearColor += DoDSP_DoLightInternal( worldPos, worldNormal, 1, bHalfLambert, bStaticLight );
		if ( nNumLights >= 3 )
			linearColor += DoDSP_DoLightInternal( worldPos, worldNormal, 2, bHalfLambert, bStaticLight );
		if ( nNumLights >= 4 )
			linearColor += DoDSP_DoLightInternal( worldPos, worldNormal, 3, bHalfLambert, bStaticLight );
		linearColor += AmbientLight( worldNormal );
	}
	return linearColor;
}

#endif // DODSP_MODELSUN_VS_FXC_H
