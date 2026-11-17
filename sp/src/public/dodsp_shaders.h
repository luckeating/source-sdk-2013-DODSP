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
#define DODSP_SHADER_DLL_VERSION	"3b1"	// bump with every DLL change; see dodsp_shaders_version

// Stage 3a: proves the DLL loads next to Mapbase's. It only falls back to UnlitGeneric.
#define DODSP_SHADER_PROBE			"DODSP_Probe"

// Stage 3b-1: draws a sun cascade with the aim-point marker (dodsp_csm_view). Uses dodsp_*.vcs.
#define DODSP_SHADER_CASCADE_DEBUG	"DODSP_SunCascadeDebug"

#endif // DODSP_SHADERS_H
