//===================== DOD-SP shader DLL check =====================//
//
// Lighting stage 3a. DOD-SP's shaders live in their own DLL, game_shader_generic_dodsp.dll, which
// sits next to Mapbase's game_shader_dx9.dll in DOD-SP\bin and never replaces it. This file
// reports, once per game start (first map load) and on demand (dodsp_shaders_report):
//
//   DOD-SP shaders: game_shader_generic_dodsp.dll loaded (build 3a), 1 shader: DODSP_Probe
//   DOD-SP shaders: game_shader_generic_dodsp.dll NOT loaded - Mapbase SDK_ shaders only
//
// INVARIANT: Mapbase's own shaders must still be there. If SDK_LightmappedGeneric is missing,
// Mapbase's game_shader_dx9.dll is gone or broken (that is what broke Hammer once), and an
// INVARIANT warning says so - whatever the state of DOD-SP's DLL.
//
//==================================================================//

#include "cbase.h"
#include "igamesystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/IShader.h"
#include "dodsp_shaders.h"
#include "c_dodsp_shaderdll.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{
	void GetShaderList( CUtlVector<IShader *> &list )
	{
		int n = materials->ShaderCount();
		list.SetCount( n );
		n = materials->GetShaders( 0, n, list.Base() );
		list.SetCountNonDestructively( MAX( n, 0 ) );
	}

	void ReportShaderDLL()
	{
		CUtlVector<IShader *> list;
		GetShaderList( list );

		bool bMapbase = false;
		CUtlString names;
		int nOurs = 0;
		const int nPrefix = Q_strlen( DODSP_SHADER_PREFIX );
		for ( int i = 0; i < list.Count(); i++ )
		{
			if ( !list[i] )
				continue;
			const char *pszName = list[i]->GetName();
			if ( !Q_stricmp( pszName, "SDK_LightmappedGeneric" ) )
				bMapbase = true;
			if ( !Q_strnicmp( pszName, DODSP_SHADER_PREFIX, nPrefix ) )
			{
				if ( nOurs++ )
					names += ", ";
				names += pszName;
			}
		}

		const ConVarRef version( "dodsp_shaders_version", true );
		if ( DoDSP_ShaderDLLLoaded() )
		{
			Msg( "DOD-SP shaders: " DODSP_SHADER_DLL_NAME ".dll loaded (build %s), %d shader%s: %s\n",
				version.IsValid() ? version.GetString() : "?", nOurs, nOurs == 1 ? "" : "s", names.Get() );
			if ( version.IsValid() && Q_strcmp( version.GetString(), DODSP_SHADER_DLL_VERSION ) )
				Warning( "DOD-SP shaders: the DLL is build %s but client.dll expects build %s - copy both from the same build\n",
					version.GetString(), DODSP_SHADER_DLL_VERSION );
		}
		else
		{
			Msg( "DOD-SP shaders: " DODSP_SHADER_DLL_NAME ".dll NOT loaded - Mapbase SDK_ shaders only\n" );
		}

		if ( !bMapbase )
			Warning( "DOD-SP shaders: INVARIANT - Mapbase's SDK_LightmappedGeneric is missing. "
				"Mapbase's game_shader_dx9.dll in DOD-SP\\bin is gone or broken; put it back.\n" );
	}

	class CDoDSPShaderDLLCheck : public CAutoGameSystem
	{
	public:
		CDoDSPShaderDLLCheck() : CAutoGameSystem( "CDoDSPShaderDLLCheck" ), m_bReported( false ) {}

		virtual void LevelInitPostEntity()
		{
			if ( m_bReported )
				return;
			m_bReported = true;
			ReportShaderDLL();
		}

	private:
		bool m_bReported;
	};

	CDoDSPShaderDLLCheck g_DoDSPShaderDLLCheck;
}

bool DoDSP_ShaderExists( const char *pszShader )
{
	CUtlVector<IShader *> list;
	GetShaderList( list );
	for ( int i = 0; i < list.Count(); i++ )
		if ( list[i] && !Q_stricmp( list[i]->GetName(), pszShader ) )
			return true;
	return false;
}

bool DoDSP_ShaderDLLLoaded()
{
	return DoDSP_ShaderExists( DODSP_SHADER_PROBE );
}

CON_COMMAND( dodsp_shaders_report, "DOD-SP: is DOD-SP's own shader DLL loaded, and are Mapbase's shaders still there" )
{
	ReportShaderDLL();
}
