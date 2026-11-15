//=============================================================================//
//
// DOD-SP: client-side stand-ins for the DoD NPC weapons (server: weapon_dodsp_guns.cpp).
//
//=============================================================================//

#include "cbase.h"
#include "c_weapon__stubs.h"
#include "basehlcombatweapon_shared.h"
#include "c_basehlcombatweapon.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/IShader.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "filesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

STUB_WEAPON_CLASS( weapon_k98, WeaponK98, C_BaseHLCombatWeapon );
STUB_WEAPON_CLASS( weapon_mp40, WeaponMP40, C_BaseHLCombatWeapon );
STUB_WEAPON_CLASS( weapon_mg42, WeaponMG42, C_BaseHLCombatWeapon );

//-----------------------------------------------------------------------------
// DOD-SP diagnostic: which shaders the engine actually has, and what each test
// material resolved to. "dodsp_shadercheck" or "dodsp_shadercheck <material> ..."
//-----------------------------------------------------------------------------
CON_COMMAND( dodsp_shadercheck, "DOD-SP: list loaded shaders and how materials resolve (shader, lightmap)" )
{
	int nShaders = materials->ShaderCount();
	CUtlVector<IShader*> shaders;
	shaders.SetCount( nShaders );
	int nGot = materials->GetShaders( 0, nShaders, shaders.Base() );
	int nSDK = 0;
	Msg( "dodsp_shadercheck: %d shaders loaded\n", nGot );
	static const char *s_pszWatch[] = { "SDK_LightmappedGeneric", "SDK_VertexLitGeneric", "SDK_UnlitGeneric", "SDK_WorldVertexTransition", "LightmappedGeneric", "VertexLitGeneric" };
	bool bFound[ARRAYSIZE( s_pszWatch )] = {};
	for ( int i = 0; i < nGot; i++ )
	{
		const char *pszName = shaders[i] ? shaders[i]->GetName() : "";
		if ( !Q_strnicmp( pszName, "SDK_", 4 ) )
			nSDK++;
		for ( int w = 0; w < (int)ARRAYSIZE( s_pszWatch ); w++ )
			if ( !Q_stricmp( pszName, s_pszWatch[w] ) )
				bFound[w] = true;
	}
	Msg( "  SDK_ (Mapbase) shaders: %d\n", nSDK );
	for ( int w = 0; w < (int)ARRAYSIZE( s_pszWatch ); w++ )
		Msg( "  %-26s %s\n", s_pszWatch[w], bFound[w] ? "LOADED" : "missing" );

	// Which shader model the card runs (ps30 vs ps20b files), and which copy of each of our
	// compiled .vcs files the engine opens (it loads "shaders/fxc/<name>.vcs" through GAME).
	bool bSM3 = g_pMaterialSystemHardwareConfig && g_pMaterialSystemHardwareConfig->SupportsShaderModel_3_0();
	Msg( "  dx level %d, shader model 3: %s\n",
		 g_pMaterialSystemHardwareConfig ? g_pMaterialSystemHardwareConfig->GetDXSupportLevel() : -1, bSM3 ? "yes (uses *_ps30/_vs30)" : "NO (uses *_ps20b/_vs20)" );
	static const char *s_pszVcs[] = { "SDK_lightmappedgeneric_flashlight_ps30", "SDK_lightmappedgeneric_flashlight_ps20b",
									  "SDK_lightmappedgeneric_flashlight_vs30", "SDK_lightmappedgeneric_flashlight_vs20" };
	for ( int v = 0; v < (int)ARRAYSIZE( s_pszVcs ); v++ )
	{
		char szRel[MAX_PATH], szFull[MAX_PATH];
		Q_snprintf( szRel, sizeof( szRel ), "shaders\\fxc\\%s.vcs", s_pszVcs[v] );
		szFull[0] = 0;
		g_pFullFileSystem->RelativePathToFullPath( szRel, "GAME", szFull, sizeof( szFull ) );
		Msg( "  %-42s %8u bytes  %s\n", s_pszVcs[v], g_pFullFileSystem->Size( szRel, "GAME" ), szFull[0] ? szFull : "(inside a VPK or missing)" );
	}

	static const char *s_pszDefault[] = { "wood/woodboards02", "wood/woodboards04", "wood/woodfloor002", "plaster/plasterwall043c", "plaster/wallpaper004" };
	int nMats = args.ArgC() > 1 ? args.ArgC() - 1 : ARRAYSIZE( s_pszDefault );
	for ( int m = 0; m < nMats; m++ )
	{
		const char *pszMat = args.ArgC() > 1 ? args[m + 1] : s_pszDefault[m];
		IMaterial *pMat = materials->FindMaterial( pszMat, TEXTURE_GROUP_WORLD, false );
		if ( !pMat || pMat->IsErrorMaterial() )
		{
			Msg( "  material %-28s NOT FOUND\n", pszMat );
			continue;
		}
		Msg( "  material %-28s shader %-24s needs lightmap: %s\n", pszMat, pMat->GetShaderName(),
			 pMat->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP ) ? "yes" : "NO" );
	}
}
