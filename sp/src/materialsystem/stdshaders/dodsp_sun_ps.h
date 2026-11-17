//===================== DOD-SP sun shadows: shared pixel shader code =====================//
//
// One owner for how every DOD-SP shader reads the sun's shadow maps (rendered by the client in
// clientshadowmgr.cpp DoDSP_RenderSunMaps):
//   static map = world + static props (what VRAD already baked)
//   full map   = everything: world, static props, NPCs, physics props, brush entities
// Brushes (lightmappedgeneric) remove the baked sun where static says lit and full says blocked.
// Models (vertexlitgeneric, skin) add the sun per pixel where the full map says lit.
//
// Brushes pass their own constants (lightmappedgeneric_ps2_3_x.h, DODSP_PSREG_* in its helper).
// Models use the flashlight's own resources, which their base pass (FLASHLIGHT == 0) never touches:
//   g_FlashlightWorldToTexture   world -> sun map (x, y across the map 0..1, z depth 0..1), set
//                                exactly like a flashlight's matrix. Orthographic: column 2's xyz
//                                points the way the light travels.
//   cFlashlightColor (c28)       rgb = baked sun colour (light_environment, linear), w = 1 / map size
//   cFlashlightScreenScale (c31) x = 1: compare depth here (AMD formats return depth, NVIDIA PCF
//                                the comparison), y = debug view (0 off, 1 verdict, 2 coordinates),
//                                z = softness (tap spacing in texels), w = 1: sun on for this draw
//   ShadowDepthSampler           the full map
// Written by the model helpers on every base-pass draw (DoDSP_SetModelSunState).
//
// Every fetch here is gradient-based (tex2Dproj, the way Valve's flashlight shadows read the
// same maps), so none of these may be called inside a dynamic branch.
//=========================================================================================//
#ifndef DODSP_SUN_PS_H
#define DODSP_SUN_PS_H

// One shadow map tap: 1 = lit, 0 = blocked
float DoDSP_SunTap( sampler DepthSampler, float2 vUV, float flDepth, float flManualCompare )
{
	float flTap = tex2Dproj( DepthSampler, float4( vUV, flDepth, 1.0f ) ).x;
	return ( flManualCompare > 0.5f ) ? ( ( flTap > flDepth ) ? 1.0f : 0.0f ) : flTap;
}

// 2x2 taps half a texel apart: a cheap smoothed test (with hardware PCF each tap is bilinear too)
float DoDSP_SunVisibility2x2( sampler DepthSampler, float3 vPos, float flTexel, float flManualCompare )
{
	float flSum = 0.0f;
	for ( int y = 0; y < 2; y++ )
		for ( int x = 0; x < 2; x++ )
			flSum += DoDSP_SunTap( DepthSampler, vPos.xy + ( float2( x, y ) - 0.5f ) * flTexel, vPos.z, flManualCompare );
	return flSum * 0.25f;
}

// 3x3 taps, spacing = flSoft texels: soft edges for models
float DoDSP_SunVisibility3x3( sampler DepthSampler, float3 vPos, float flTexel, float flSoft, float flManualCompare )
{
	float flSum = 0.0f;
	for ( int y = -1; y <= 1; y++ )
		for ( int x = -1; x <= 1; x++ )
			flSum += DoDSP_SunTap( DepthSampler, vPos.xy + float2( x, y ) * flTexel * flSoft, vPos.z, flManualCompare );
	return flSum * ( 1.0f / 9.0f );
}

// 4x4 taps, spacing = flSoft texels: the soft shadow edge of everything dynamic on brushes
float DoDSP_SunVisibility4x4( sampler DepthSampler, float3 vPos, float flTexel, float flSoft, float flManualCompare )
{
	float flSum = 0.0f;
	for ( int y = 0; y < 4; y++ )
		for ( int x = 0; x < 4; x++ )
			flSum += DoDSP_SunTap( DepthSampler, vPos.xy + ( float2( x, y ) - 1.5f ) * flTexel * flSoft, vPos.z, flManualCompare );
	return flSum * ( 1.0f / 16.0f );
}

float3 DoDSP_SunCoords( float3 vWorldPos, float4 vRow0, float4 vRow1, float4 vRow2 )
{
	float4 vWorld = float4( vWorldPos, 1.0f );
	return float3( dot( vWorld, vRow0 ), dot( vWorld, vRow1 ), dot( vWorld, vRow2 ) );
}

// 1 well inside the sun map, 0 outside, fading over the outer 5% so nothing pops at its edge
float DoDSP_SunInside( float3 vSun )
{
	float2 vEdge = 0.5f - abs( vSun.xy - 0.5f );
	float flXY = saturate( min( vEdge.x, vEdge.y ) * 20.0f );
	float flZ = ( vSun.z > 0.0f && vSun.z < 1.0f ) ? 1.0f : 0.0f;
	return flXY * flZ;
}

// Model pixels: how much sun reaches this point (0..1). Inside the sun map the full map decides;
// outside it, the engine's own per-model verdict (flEngineSun: 1 if it found the sun visible from
// the model's centre) - the stock behaviour, so distant models keep their sun.
// The lookup point moves 1 texel along the normal so curved model surfaces don't shadow themselves.
float DoDSP_ModelSunVisibility( sampler FullDepthSampler, float3 vWorldPos, float3 vNormal, float flEngineSun,
								float4x4 mWorldToSun, float4 vColorTexel, float4 vTweaks,
								out float3 vSunOut, out float flInsideOut )
{
	float flTexelWorld = vColorTexel.w / max( length( mWorldToSun._11_21_31 ), 1e-6f );
	float3 vSun = mul( float4( vWorldPos + vNormal * flTexelWorld, 1.0f ), mWorldToSun ).xyz;
	float flInside = DoDSP_SunInside( vSun );
	float3 vTap = float3( saturate( vSun.xy ), min( vSun.z, 0.99999f ) );
	float flMap = DoDSP_SunVisibility3x3( FullDepthSampler, vTap, vColorTexel.w, max( vTweaks.z, 0.25f ), vTweaks.x );
	vSunOut = vSun;
	flInsideOut = flInside;
	return lerp( saturate( flEngineSun ), flMap, flInside );
}

// Direction toward the sun for models (column 2 of the world -> sun matrix is the light's forward axis)
float3 DoDSP_ModelSunToLight( float4x4 mWorldToSun )
{
	return -normalize( mWorldToSun._13_23_33 );
}

// Debug view for models (dodsp_sunshadow_debug), replaces the diffuse lighting:
//  1 = green: sunlit, red: in the map but blocked, purple: outside the map (bright = the engine
//      says sunlit there), grey: faces away from the sun
//  2 = raw sun map coordinates (black outside)
float3 DoDSP_ModelSunDebug( float flMode, float flNdotL, float flVis, float flInside, float flEngineSun, float3 vSun )
{
	if ( flMode > 1.5f )
		return saturate( vSun ) * ( flInside > 0.0f ? 1.0f : 0.0f );
	if ( flNdotL <= 0.0f )
		return float3( 0.06f, 0.06f, 0.06f );
	if ( flInside < 0.5f )
		return float3( 0.3f, 0.0f, 0.3f ) * ( 0.4f + 0.6f * saturate( flEngineSun ) );
	return float3( 1.0f - flVis, flVis, 0.0f );
}

#endif // DODSP_SUN_PS_H
