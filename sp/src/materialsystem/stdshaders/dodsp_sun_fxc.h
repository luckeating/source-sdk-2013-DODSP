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
//   c32       rgb = sun colour in lightmap units, 0 = no dynamic sun; w = 1 / lightmap scale
//   c33       light-space right axis, w = depth offset
//   c34       light-space up axis, w = 1 / cascade texels
//   c35       light-space forward axis (scaled to 0..1 over the depth range), w = depth bias
//   c36..c38  per cascade: x = scale (1 / width), y = u offset, z = v offset, w = normal offset (units)
//   c39       xyz = direction TOWARD the sun, w = filter spread in texels
//
// DODSP_SUNFILTER (static combo): 0 = the card compares depth in hardware (NVIDIA),
//                                 1 = raw depth, compared here (ATI/AMD paths).
//
//=====================================================================//
#ifndef DODSP_SUN_FXC_H
#define DODSP_SUN_FXC_H

sampler DoDSP_CascadeNear			: register( s13 );
sampler DoDSP_CascadeMid			: register( s14 );
sampler DoDSP_CascadeFar			: register( s15 );

const float4 g_DoDSP_SunColor		: register( c32 );
const float4 g_DoDSP_AxisR			: register( c33 );
const float4 g_DoDSP_AxisU			: register( c34 );
const float4 g_DoDSP_AxisF			: register( c35 );
const float4 g_DoDSP_CascadeNear	: register( c36 );
const float4 g_DoDSP_CascadeMid		: register( c37 );
const float4 g_DoDSP_CascadeFar		: register( c38 );
const float4 g_DoDSP_ToSun			: register( c39 );

// 1 = lit, 0 = shadowed, for one depth-map texel neighbourhood
float DoDSP_Compare( sampler s, float2 uv, float z )
{
#if DODSP_SUNFILTER == 0
	return tex2Dproj( s, float4( uv, z, 1.0f ) ).x;		// hardware compare, bilinear between 4 texels
#else
	return ( z <= tex2D( s, uv ).x ) ? 1.0f : 0.0f;
#endif
}

// Four taps half a filter-spread apart: with the hardware compare each tap is already a 2x2
// bilinear compare, so the edge is about 3 texels soft.
float DoDSP_Filter( sampler s, float2 uv, float z )
{
	float o = 0.5f * g_DoDSP_ToSun.w * g_DoDSP_AxisU.w;
	return 0.25f * ( DoDSP_Compare( s, uv + float2( -o, -o ), z ) +
					 DoDSP_Compare( s, uv + float2(  o, -o ), z ) +
					 DoDSP_Compare( s, uv + float2( -o,  o ), z ) +
					 DoDSP_Compare( s, uv + float2(  o,  o ), z ) );
}

// Where a world point lands in one cascade: xy = texture, z = depth (minus bias), w = 1 if inside
float4 DoDSP_CascadeCoord( float4 c, float3 worldPos, float3 geoNormal )
{
	float3 p = worldPos + geoNormal * c.w;				// normal offset: one texel off the surface
	float u = dot( p, g_DoDSP_AxisR.xyz ) * c.x + c.y;
	float v = -dot( p, g_DoDSP_AxisU.xyz ) * c.x + c.z;
	float z = dot( p, g_DoDSP_AxisF.xyz ) + g_DoDSP_AxisR.w - g_DoDSP_AxisF.w;
	float2 edge = min( float2( u, v ), 1.0f - float2( u, v ) );
	float inside = ( min( edge.x, edge.y ) > 0.02f ) ? 1.0f : 0.0f;		// keep the filter off the border
	return float4( u, v, z, inside );
}

// Sun visibility 0..1 at a world point: the finest cascade that holds the point wins.
// (Every cascade is sampled and the right one selected without branching - gradient-based
// texture reads can't sit inside dynamic branches in shader model 3.)
float DoDSP_SunShadow( float3 worldPos, float3 geoNormal )
{
	float4 cn = DoDSP_CascadeCoord( g_DoDSP_CascadeNear, worldPos, geoNormal );
	float4 cm = DoDSP_CascadeCoord( g_DoDSP_CascadeMid, worldPos, geoNormal );
	float4 cf = DoDSP_CascadeCoord( g_DoDSP_CascadeFar, worldPos, geoNormal );

	float sn = DoDSP_Filter( DoDSP_CascadeNear, cn.xy, cn.z );
	float sm = DoDSP_Filter( DoDSP_CascadeMid, cm.xy, cm.z );
	float sf = DoDSP_Filter( DoDSP_CascadeFar, cf.xy, cf.z );

	float wn = cn.w;
	float wm = ( 1.0f - wn ) * cm.w;
	float wf = ( 1.0f - wn ) * ( 1.0f - cm.w ) * cf.w;
	float wOutside = 1.0f - wn - wm - wf;				// outside every cascade: lit (open sky)
	return sn * wn + sm * wm + sf * wf + wOutside;
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
