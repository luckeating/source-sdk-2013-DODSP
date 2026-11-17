//===================== DOD-SP model sun: pixel shader side (stage 4) =====================//
//
// The sun on a model pixel: sun colour * N.L * shadow from the cascades. The vertex shader placed
// the point in the sun's light space (dodsp_modelsun_vs_fxc.h); this side applies each cascade's
// scale and offset and reads the maps through dodsp_sun_common_fxc.h (one owner for the compare,
// filter and cascade pick, shared with the brushes).
//
// Four constants. Each model pixel shader declares them itself, at registers that are free in its
// base pass, BEFORE including this file (the register numbers live in the .fxc and in
// dodsp_sun_shader.cpp's DoDSP_ModelSunPsReg - CI checks the two agree):
//   g_DoDSP_MSun0   near cascade: x = scale, y = u offset, z = v offset;  w = filter offset (texture units)
//   g_DoDSP_MSun1   mid cascade:  x, y, z as above;                       w = direction to the sun .x
//   g_DoDSP_MSun2   far cascade:  x, y, z as above;                       w = direction to the sun .y
//   g_DoDSP_MSun3   rgb = sun colour (the engine's light units),          w = direction to the sun .z
// Only drawn in the base pass (never with FLASHLIGHT) - the flashlight pass owns those registers.
//
//=====================================================================//
#ifndef DODSP_MODELSUN_PS_FXC_H
#define DODSP_MODELSUN_PS_FXC_H

#include "dodsp_sun_common_fxc.h"

float4 DoDSP_ModelCascade( float4 c, float3 lsp )
{
	float2 uv = lsp.xy * c.x + c.yz;
	return float4( uv, lsp.z, DoDSP_Inside( uv ) );
}

float3 DoDSP_ModelToSun()
{
	return float3( g_DoDSP_MSun1.w, g_DoDSP_MSun2.w, g_DoDSP_MSun3.w );
}

// Sun visibility 0..1 for a light-space point from the vertex shader
float DoDSP_ModelSunShadow( float3 lsp )
{
	return DoDSP_ShadowFromCascades( DoDSP_ModelCascade( g_DoDSP_MSun0, lsp ),
									 DoDSP_ModelCascade( g_DoDSP_MSun1, lsp ),
									 DoDSP_ModelCascade( g_DoDSP_MSun2, lsp ), g_DoDSP_MSun0.w );
}

// The sun's diffuse light at a pixel, in the engine's model light units.
//   sunCoord = the vertex shader's DoDSP_ModelSunCoord (w = 0: this model takes no per-pixel sun)
//   flShadow = DoDSP_ModelSunShadow( sunCoord.xyz ), passed in so specular can reuse it
float3 DoDSP_ModelSunDiffuse( float4 sunCoord, float3 worldNormal, float flShadow )
{
	float nDotL = saturate( dot( normalize( worldNormal ), DoDSP_ModelToSun() ) );
	return g_DoDSP_MSun3.rgb * ( sunCoord.w * nDotL * flShadow );
}

#endif // DODSP_MODELSUN_PS_FXC_H
