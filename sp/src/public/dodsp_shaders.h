//===================== DOD-SP shader names =====================//
//
// One owner for the names DOD-SP's own shader DLL uses (lighting stage 3). Shared by the shader
// DLL (materialsystem/stdshaders/game_shader_dodsp.vpc) and the client, which checks at map load
// that the DLL is there and which DODSP_ shaders it brought.
//
// INVARIANT: every shader in game_shader_dx6.dll is named DODSP_<something>, and every
// compiled shader file it uses is named dodsp_<something>.vcs. Nothing in it may share a name with
// a Mapbase (SDK_*) or Valve shader: the engine stops with a fatal error when a mod shader takes a
// Valve name, and when two mod DLLs share a name the last one loaded silently wins.
//
//===============================================================//
#ifndef DODSP_SHADERS_H
#define DODSP_SHADERS_H

#define DODSP_SHADER_DLL_NAME		"game_shader_dx6"
#define DODSP_SHADER_PREFIX			"DODSP_"
#define DODSP_SHADER_DLL_VERSION	"5a2"	// bump with every DLL change; see dodsp_shaders_version

// Stage 3a: proves the DLL loads next to Mapbase's. It only falls back to UnlitGeneric.
#define DODSP_SHADER_PROBE			"DODSP_Probe"

// Stage 3b-1: draws a sun cascade with the aim-point marker (dodsp_csm_view). Uses dodsp_*.vcs.
#define DODSP_SHADER_CASCADE_DEBUG	"DODSP_SunCascadeDebug"

// Stage 5a: draws one target of the lamps' G-buffer (dodsp_lamps_view). Uses dodsp_*.vcs.
#define DODSP_SHADER_GBUFFER_DEBUG	"DODSP_GBufferDebug"

// Stage 3b-2: the brush shaders with the sun drawn per pixel. Materials using a feature these don't
// support fall back to Mapbase's twin by themselves (SHADER_FALLBACK).
#define DODSP_SHADER_LMG			"DODSP_LightmappedGeneric"
#define DODSP_SHADER_WVT			"DODSP_WorldVertexTransition"

// Stage 4: props and NPCs with the sun drawn per pixel (plain, bump-mapped and phong/skin paths).
// Materials using a feature it doesn't support fall back to Mapbase's SDK_VertexLitGeneric.
#define DODSP_SHADER_VLG			"DODSP_VertexLitGeneric"

//-----------------------------------------------------------------------------
// The sun, from the client to the shaders: rendering parameters set on the render context every
// frame (so they stay in order with queued rendering). Written ONLY by c_dodsp_sun_cascades.cpp
// (DoDSP_PublishSun), read ONLY by materialsystem/stdshaders/dodsp/dodsp_sun_shader.cpp.
// INVARIANT: these are the slots Valve and Mapbase leave free (public/renderparm.h: vectors 0..10,
// ints 0..16 are taken); nothing else may use them. Checked at compile time in dodsp_sun_shader.cpp.
//-----------------------------------------------------------------------------
#define DODSP_RP_VEC_AXIS_R			11	// light-space right axis (world)
#define DODSP_RP_VEC_AXIS_U			12	// light-space up axis
#define DODSP_RP_VEC_AXIS_F			13	// light travel direction scaled by 1 / depth range
#define DODSP_RP_VEC_DEPTH			14	// x = depth offset, y = tan( sun's angular radius ) (4b soft shadows), z = depth bias
#define DODSP_RP_VEC_CASCADE0		15	// near: x = scale (1 / width), y = u offset, z = v offset
#define DODSP_RP_VEC_CASCADE1		16	// mid
#define DODSP_RP_VEC_CASCADE2		17	// far
#define DODSP_RP_VEC_SUNCOLOR		18	// sun colour in lightmap units while any DODSP_SUNMODE_ is on, else 0 0 0
#define DODSP_RP_VEC_FILTER			19	// x = smallest filter radius, y = normal offset (both cascade texture units), z = DODSP_SUNMODE_ bits
#define DODSP_RP_INT_CASCADE0		17	// ITexture * of the near cascade's depth map
#define DODSP_RP_INT_CASCADE1		18	// mid
#define DODSP_RP_INT_CASCADE2		19	// far

// Who takes the per-pixel sun (bits in DODSP_RP_VEC_FILTER.z; every slot above is taken, Valve's
// MAX_VECTOR_RENDER_PARMS is 20). Decided ONLY by the client (PublishSun), read ONLY through
// DoDSP_SunModes in dodsp_sun_shader.cpp.
//   DYNAMIC      brushes and moving/unbaked models - dodsp_sun_baked 0 (the baked direct sun is off)
//   STATICPROPS  models with VRAD's baked vertex lighting - whenever the map's static props were
//                baked without the sun (dodsp_lightstyles.h DODSP_LVLFLAGS_PROPS_NOSUN_*), in BOTH
//                sun modes: vertex lighting can't switch the sun, so the game always draws it there.
#define DODSP_SUNMODE_DYNAMIC		1
#define DODSP_SUNMODE_STATICPROPS	2

#endif // DODSP_SHADERS_H
