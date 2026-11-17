//===================== DOD-SP sun (shader side) =====================//
//
// One owner for how a pixel gets its sunlight. Lighting stage 3b-2 (brushes); stage 4 (models)
// includes this same file.
//
// The sun is the map's light_environment. VRAD put its direct light in light style 31
// (-dodsp_sunstyle); with dodsp_sun_baked 0 the lightmaps carry everything EXCEPT the direct sun,
// and this file adds it back per pixel:
//
//     sun = sun colour * max(0, N . to-sun) * shadow from the cascades
//
// Nothing is subtracted from the lightmap, so the shadow can never disagree with the baked light
// (that was the darken-mode failure).
//
// Inputs (set every draw from the rendering parameters the client fills, see
// public/dodsp_shaders.h - the register and sampler numbers below must match it):
//   s13..s15  the three cascade depth maps (near, mid, far)
//   c13       rgb = sun colour in lightmap units, 0 = no dynamic sun; w = 1 / lightmap scale
//   c14       light-space right axis, w = depth offset
//   c15       light-space up axis, w = 1 / cascade texels
//   c16       light-space forward axis (scaled to 0..1 over the depth range), w = depth bias
//   c17..c19  per cascade: x = scale (1 / width), y = u offset, z = v offset, w = normal offset (units)
//   c20       xyz = direction TOWARD the sun, w = filter spread in texels
//
// INVARIANT: pixel shader constants c0..c31 only. The 2013 engine keeps 32 pixel constants per draw;
// writing above c31 overflows its storage and corrupts unrelated memory (stage 3b-2 used c32..c39 and
// mat_reloadallmaterials crashed later inside the file system). c13..c20 are the brush shader's
// Xbox-360-only flashlight constants, which the PC build never uses. The CI fails on any register above
// c31 in a dodsp_ shader, and the C++ side checks the hardware's count before setting them.
//
// DODSP_SUNFILTER (static combo): 0 = the card compares depth in hardware (NVIDIA),
//                                 1 = raw depth, compared here (ATI/AMD paths).
//
//=====================================================================//
#ifndef DODSP_SUN_FXC_H
#define DODSP_SUN_FXC_H

#include "dodsp_sun_common_fxc.h"	// samplers s13..s15, depth compare, filter, cascade pick

const float4 g_DoDSP_SunColor		: register( c13 );
const float4 g_DoDSP_AxisR			: register( c14 );
const float4 g_DoDSP_AxisU			: register( c15 );
const float4 g_DoDSP_AxisF			: register( c16 );
const float4 g_DoDSP_CascadeNear	: register( c17 );
const float4 g_DoDSP_CascadeMid		: register( c18 );
const float4 g_DoDSP_CascadeFar		: register( c19 );
const float4 g_DoDSP_ToSun			: register( c20 );

// Where a world point lands in one cascade: xy = texture, z = depth (minus bias), w = 1 if inside
float4 DoDSP_CascadeCoord( float4 c, float3 worldPos, float3 geoNormal )
{
	float3 p = worldPos + geoNormal * c.w;				// normal offset: one texel off the surface
	float u = dot( p, g_DoDSP_AxisR.xyz ) * c.x + c.y;
	float v = -dot( p, g_DoDSP_AxisU.xyz ) * c.x + c.z;
	float z = dot( p, g_DoDSP_AxisF.xyz ) + g_DoDSP_AxisR.w - g_DoDSP_AxisF.w;
	return float4( u, v, z, DoDSP_Inside( float2( u, v ) ) );
}

// Sun visibility 0..1 at a world point (the cascade pick lives in dodsp_sun_common_fxc.h)
float DoDSP_SunShadow( float3 worldPos, float3 geoNormal )
{
	float4 cn = DoDSP_CascadeCoord( g_DoDSP_CascadeNear, worldPos, geoNormal );
	float4 cm = DoDSP_CascadeCoord( g_DoDSP_CascadeMid, worldPos, geoNormal );
	float4 cf = DoDSP_CascadeCoord( g_DoDSP_CascadeFar, worldPos, geoNormal );

	return DoDSP_ShadowFromCascades( cn, cm, cf, 0.5f * g_DoDSP_ToSun.w * g_DoDSP_AxisU.w );
}

// The sun's light at a pixel, in the same units as the lightmap colour it is added to.
//   bumpNormal = the (bumped) world normal used for lighting, geoNormal = the surface's own normal
float3 DoDSP_SunLight( float3 worldPos, float3 bumpNormal, float3 geoNormal )
{
	float3 toSun = g_DoDSP_ToSun.xyz;
	float nDotL = saturate( dot( normalize( bumpNormal ), toSun ) );
	float facing = saturate( dot( geoNormal, toSun ) * 8.0f );		// faces turned from the sun get none
	// (no early-out when the sun is off: the colour is 0 then, and a branch around the texture
	// reads would not compile)
	return g_DoDSP_SunColor.rgb * ( nDotL * facing * DoDSP_SunShadow( worldPos, geoNormal ) );
}

#endif // DODSP_SUN_FXC_H
