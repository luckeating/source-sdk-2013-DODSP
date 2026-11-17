//===================== DOD-SP shader DLL probe =====================//
//
// Lighting stage 3a. The smallest possible DOD-SP shader: it proves that
// game_shader_generic_dodsp.dll loads next to Mapbase's untouched game_shader_dx9.dll, in the
// game and in Hammer, before any real shader work goes into it.
//
// It uses no compiled shader files: a material set to DODSP_Probe simply draws as UnlitGeneric.
//
//==================================================================//

#include "BaseVSShader.h"
#include "convar.h"
#include "dodsp_shaders.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Lets anyone check from the console which build of the DLL the game picked up.
static ConVar dodsp_shaders_version( "dodsp_shaders_version", DODSP_SHADER_DLL_VERSION, FCVAR_NONE,
	"The build of DOD-SP's own shader DLL (" DODSP_SHADER_DLL_NAME ".dll) the game loaded. Missing = the DLL did not load." );

BEGIN_VS_SHADER( DODSP_Probe, "DOD-SP stage 3a: proves DOD-SP's shader DLL loaded. Draws as UnlitGeneric." )

	BEGIN_SHADER_PARAMS
	END_SHADER_PARAMS

	SHADER_FALLBACK
	{
		return "UnlitGeneric";
	}

	SHADER_INIT
	{
	}

	SHADER_DRAW
	{
	}

END_SHADER
