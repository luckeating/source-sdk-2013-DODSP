//===================== DOD-SP shader names =====================//
//
// One owner for the names DOD-SP's own shader DLL uses (lighting stage 3). Shared by the shader
// DLL (materialsystem/stdshaders/game_shader_dodsp.vpc) and the client, which checks at map load
// that the DLL is there and which DODSP_ shaders it brought.
//
// INVARIANT: every shader in game_shader_generic_dodsp.dll is named DODSP_<something>, and every
// compiled shader file it uses is named dodsp_<something>.vcs. Nothing in it may share a name with
// a Mapbase (SDK_*) or Valve shader: the engine stops with a fatal error when a mod shader takes a
// Valve name, and when two mod DLLs share a name the last one loaded silently wins.
//
//===============================================================//
#ifndef DODSP_SHADERS_H
#define DODSP_SHADERS_H

#define DODSP_SHADER_DLL_NAME		"game_shader_generic_dodsp"
#define DODSP_SHADER_PREFIX			"DODSP_"
#define DODSP_SHADER_DLL_VERSION	"3a"	// bumped whenever the DLL changes; see dodsp_shaders_version

// Stage 3a: proves the DLL loads next to Mapbase's. It only falls back to UnlitGeneric.
#define DODSP_SHADER_PROBE			"DODSP_Probe"

#endif // DODSP_SHADERS_H
