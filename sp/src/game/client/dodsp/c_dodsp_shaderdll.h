//===================== DOD-SP shader DLL check =====================//
//
// One owner for "did DOD-SP's own shader DLL (game_shader_generic_dodsp.dll) load, and which
// DODSP_ shaders did it bring". Lighting stage 3. Names live in public/dodsp_shaders.h.
//
//==================================================================//
#ifndef C_DODSP_SHADERDLL_H
#define C_DODSP_SHADERDLL_H

// True when the material system knows a shader by this exact name (any DLL).
bool DoDSP_ShaderExists( const char *pszShader );

// True when DOD-SP's shader DLL loaded (its probe shader is registered).
bool DoDSP_ShaderDLLLoaded();

#endif // C_DODSP_SHADERDLL_H
