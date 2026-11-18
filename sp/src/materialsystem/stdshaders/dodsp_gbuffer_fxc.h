//===================== DOD-SP lamps: the G-buffer (stage 5a) =====================//
//
// Every lamp in a DOD-SP map is drawn live (stage 5). Our shaders have no room for per-lamp
// constants (32 pixel constants, the skin shader's 16 samplers), so the lamps are added AFTER the
// opaque scene, in screen space, from what each opaque pixel is: its colour (albedo), the way it
// faces (world normal) and how far it is from the eye. The brush and model shaders write those
// three to render targets 1..3 next to their normal colour (render target 0). The client binds
// targets 1..3 only for the main view's opaque pass (c_dodsp_lamps.cpp); in every other pass
// nothing is bound there and D3D9 drops the extra outputs - no combo needed.
//
// Layout (.a = the pixel's own alpha everywhere, so a blended decal blends all three). The formats
// follow render target 0 (MRT wants one bit depth): integer HDR (the PC's usual) RGBA8888, RGBA8888,
// R32F; float HDR RGBA16F x3. So every value is stored in 0..1 where it must fit 8 bits:
//   COLOR1  albedo.rgb (linear, before any lighting; the engine's sRGB write may encode it - the
//           reader decides, dodsp_lamps_check shows both readings)
//   COLOR2  world normal.xyz * 0.5 + 0.5; all 0 = "the lamps don't light this pixel" (unlit
//           materials, additive passes, and whatever no DOD-SP shader drew - the client clears to
//           0). A unit normal never encodes to less than 0.63 summed, so "written" = sum > 0.3
//   COLOR3  .x = distance from the eye (world units)
// FIRST PROBE 2026-11-18: the 16F-only version never ran - the owner's HDR is the integer kind.
//
// INVARIANT: each of our opaque pixel shaders returns DoDSP_PS_OUTPUT through DoDSP_GBuffer or
// DoDSP_GBufferNone (checked by devtools/dodsp_build_shaders.py check_gbuffer).
//
//=============================================================================//
#ifndef DODSP_GBUFFER_FXC_H
#define DODSP_GBUFFER_FXC_H

struct DoDSP_PS_OUTPUT
{
	float4 vColor	: COLOR0;
	float4 vAlbedo	: COLOR1;
	float4 vNormal	: COLOR2;
	float4 vDist	: COLOR3;
};

// vWorldNormal must already be unit length (the callers have it normalized)
DoDSP_PS_OUTPUT DoDSP_GBuffer( float4 vColor, float3 vAlbedo, float3 vWorldNormal, float flEyeDist, bool bLit )
{
	DoDSP_PS_OUTPUT o;
	o.vColor = vColor;
	o.vAlbedo = float4( bLit ? vAlbedo : float3( 0.0f, 0.0f, 0.0f ), vColor.a );
	o.vNormal = float4( bLit ? vWorldNormal * 0.5f + 0.5f : float3( 0.0f, 0.0f, 0.0f ), vColor.a );
	o.vDist = float4( flEyeDist, 0.0f, 0.0f, vColor.a );
	return o;
}

// Passes the lamps must not see (additive flashlight passes): add nothing to targets 1..3
DoDSP_PS_OUTPUT DoDSP_GBufferNone( float4 vColor )
{
	DoDSP_PS_OUTPUT o;
	o.vColor = vColor;
	o.vAlbedo = 0.0f;
	o.vNormal = 0.0f;
	o.vDist = 0.0f;
	return o;
}

#endif // DODSP_GBUFFER_FXC_H
