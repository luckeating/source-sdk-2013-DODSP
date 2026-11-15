//=============================================================================//
//
// DOD-SP: marker shader. It draws nothing; it only exists so the game can tell that DOD-SP's own
// game_shader_dx9.dll is the one loaded (dodsp_shadercheck reports "DOD-SP shader DLL: ours").
// Stage 3 changes (darken-only sun) live in this DLL, so this is the first thing to check.
//
//=============================================================================//

#include "BaseVSShader.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

BEGIN_VS_SHADER_FLAGS( DODSP_ShaderDLL, "DOD-SP marker: present only in DOD-SP's own shader DLL", SHADER_NOT_EDITABLE )

	BEGIN_SHADER_PARAMS
	END_SHADER_PARAMS

	SHADER_INIT_PARAMS()
	{
	}

	SHADER_FALLBACK
	{
		return 0;
	}

	SHADER_INIT
	{
	}

	SHADER_DRAW
	{
		Draw( false );
	}

END_SHADER
