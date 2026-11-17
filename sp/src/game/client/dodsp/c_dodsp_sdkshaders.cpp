//=============================================================================//
//
// DOD-SP: run stock world/model materials on Mapbase's SDK_ shaders.
//
// Why: our shader work (stage 3 sun shadows) can only live in shaders we compile, and those are
// Mapbase's SDK_ shaders. DoD:S materials name Valve's built-in shaders (LightmappedGeneric,
// VertexLitGeneric, WorldVertexTransition), which run Valve's own shader files and never ours.
//
// What: once a map has loaded, every material in use whose shader is one of those stock shaders is
// re-initialised from its own .vmt (patch files - e.g. the cubemap patches VBSP writes into the BSP
// - expanded) with the SDK_ twin as its shader. Nothing on disk changes. New maps, new props and
// new DoD content are covered without anyone touching a .vmt.
//
// INVARIANT (one owner): this file is the only place that decides which shader a stock material
// runs on. A swap must not change anything the engine already built around the material - vertex
// format, lightmap needs, opacity, textures. Each swap is checked against a signature taken first;
// any difference reverts the material to stock and logs an INVARIANT line.
//
// Stage 3b-2: when DOD-SP's own shader DLL brings a twin with the sun in it (DODSP_LightmappedGeneric,
// DODSP_WorldVertexTransition), the material goes there instead; a material using a feature the
// DOD-SP twin doesn't support is handed back to the SDK_ twin by that shader's own SHADER_FALLBACK,
// and counted here as "handed back". Materials whose .vmt already names Mapbase's SDK_ brush shaders
// (the HL2 materials) are moved to the DOD-SP twin too - only when the sun shaders are in use.
// INVARIANT (map load): with the sun shaders in use, any lit world material left without the per-pixel
// sun gets an INVARIANT line - with dodsp_sun_baked 0 it has no direct sun at all and draws dark.
//
// Cvars: dodsp_sdkshaders (1, applies on map load), dodsp_sdkshaders_log (print each swap),
// dodsp_sunshader (1 = use DOD-SP's sun shaders when present; decided once per game start).
// Command: dodsp_sdkshaders_report (counts, and any lit material still on a stock shader).
//
//=============================================================================//

#include "cbase.h"
#include "igamesystem.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "materialsystem/IShader.h"
#include "utlrbtree.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "dodsp_shaders.h"
#include "c_dodsp_shaderdll.h"
#include "c_dodsp_sdkshaders.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_sdkshaders( "dodsp_sdkshaders", "1", 0,
	"DOD-SP: run stock world/model materials on Mapbase's SDK_ shaders (needed for sun shadows). "
	"Applies when a map loads; turning it off needs a game restart." );
static ConVar dodsp_sdkshaders_log( "dodsp_sdkshaders_log", "0", 0, "DOD-SP: print every material moved to an SDK_ shader" );
static ConVar dodsp_sunshader( "dodsp_sunshader", "1", 0,
	"DOD-SP: move world materials onto DOD-SP's sun shaders (DODSP_*) when game_shader_dx6.dll brings them; "
	"0 = Mapbase's SDK_ shaders only. Decided once per game start (a map never mixes the two)." );

namespace
{
	struct ShaderSwap_t
	{
		const char *m_pszStock;		// resolved shader name the engine reports
		const char *m_pszSDK;		// Mapbase twin
		const char *m_pszDoDSP;		// DOD-SP twin with the sun drawn per pixel, or NULL (not yet: models are stage 4)
		bool		m_bSunOnly;		// already on Mapbase's shader (its .vmt names SDK_): moved only to the sun twin
	};

	// The lit shaders. Unlit/sprite/water/refract materials take no sunlight and stay stock.
	const ShaderSwap_t s_Swaps[] =
	{
		{ "LightmappedGeneric",			"SDK_LightmappedGeneric",		DODSP_SHADER_LMG,	false },
		{ "LightmappedGeneric_DX9",		"SDK_LightmappedGeneric",		DODSP_SHADER_LMG,	false },
		{ "WorldVertexTransition",		"SDK_WorldVertexTransition",	DODSP_SHADER_WVT,	false },
		{ "WorldVertexTransition_DX9",	"SDK_WorldVertexTransition",	DODSP_SHADER_WVT,	false },
		{ "VertexLitGeneric",			"SDK_VertexLitGeneric",			NULL,				false },
		{ "VertexLitGeneric_DX9",		"SDK_VertexLitGeneric",			NULL,				false },
		// Materials whose .vmt already names Mapbase's shader (the HL2 materials in DOD-SP\content). Without
		// these they never got the per-pixel sun, so with dodsp_sun_baked 0 they lost the sun completely and
		// drew dark (2026-11-17, the cafe steps). Only used when DOD-SP's sun shaders are in use.
		{ "SDK_LightmappedGeneric",		"SDK_LightmappedGeneric",		DODSP_SHADER_LMG,	true },
		{ "SDK_LightmappedGeneric_DX9",	"SDK_LightmappedGeneric",		DODSP_SHADER_LMG,	true },
		{ "SDK_WorldVertexTransition",	"SDK_WorldVertexTransition",	DODSP_SHADER_WVT,	true },
		{ "SDK_WorldVertexTransition_DX9", "SDK_WorldVertexTransition", DODSP_SHADER_WVT,	true },
	};

	// Engine-internal materials keep exactly what the engine gave them.
	const char *s_pszSkipPrefixes[] = { "engine/", "debug/", "tools/", "__", "//" };

	enum { SWAP_NUM = ARRAYSIZE( s_Swaps ) };

	const ShaderSwap_t *FindSwap( const char *pszShader )
	{
		for ( int i = 0; i < SWAP_NUM; i++ )
			if ( !Q_stricmp( pszShader, s_Swaps[i].m_pszStock ) )
				return &s_Swaps[i];
		return NULL;
	}

	// The shader a material ends up on after its fallbacks: the requested twin, or (for a DOD-SP
	// twin) the Mapbase twin it handed the material back to. Mapbase's own twins may resolve to
	// their "_DX9" shader.
	bool IsShaderOrDX9( const char *pszResolved, const char *pszWanted )
	{
		if ( !pszWanted )
			return false;
		if ( !Q_stricmp( pszResolved, pszWanted ) )
			return true;
		char szDX9[128];
		Q_snprintf( szDX9, sizeof( szDX9 ), "%s_DX9", pszWanted );
		return !Q_stricmp( pszResolved, szDX9 );
	}

	bool SkipByName( const char *pszName )
	{
		for ( int i = 0; i < (int)ARRAYSIZE( s_pszSkipPrefixes ); i++ )
			if ( !Q_strnicmp( pszName, s_pszSkipPrefixes[i], Q_strlen( s_pszSkipPrefixes[i] ) ) )
				return true;
		return false;
	}

	//-------------------------------------------------------------------------
	// .vmt loading with patch expansion (same rules as the engine: "include" names the base
	// file, "insert" adds or overwrites keys, "replace" only overwrites existing ones).
	//-------------------------------------------------------------------------
	void MergePatch( KeyValues *pDst, KeyValues *pSrc, bool bInsert )
	{
		if ( !pSrc )
			return;
		for ( KeyValues *p = pSrc->GetFirstSubKey(); p; p = p->GetNextKey() )
		{
			KeyValues *pExisting = pDst->FindKey( p->GetName() );
			if ( p->GetFirstSubKey() )
			{
				if ( !pExisting )
				{
					if ( !bInsert )
						continue;
					pExisting = pDst->FindKey( p->GetName(), true );
				}
				MergePatch( pExisting, p, bInsert );
			}
			else
			{
				if ( !pExisting && !bInsert )
					continue;
				pDst->SetString( p->GetName(), p->GetString() );
			}
		}
	}

	KeyValues *LoadExpandedVMTFile( const char *pszFile, int nDepth )
	{
		if ( nDepth > 8 )
			return NULL;
		KeyValues *pKV = new KeyValues( "vmt" );
		if ( !pKV->LoadFromFile( g_pFullFileSystem, pszFile, "GAME" ) )
		{
			pKV->deleteThis();
			return NULL;
		}
		if ( Q_stricmp( pKV->GetName(), "patch" ) )
			return pKV;

		const char *pszInclude = pKV->GetString( "include", NULL );
		KeyValues *pBase = ( pszInclude && *pszInclude ) ? LoadExpandedVMTFile( pszInclude, nDepth + 1 ) : NULL;
		if ( pBase )
		{
			MergePatch( pBase, pKV->FindKey( "insert" ), true );
			MergePatch( pBase, pKV->FindKey( "replace" ), false );
		}
		pKV->deleteThis();
		return pBase;
	}

	KeyValues *LoadExpandedVMT( const char *pszMaterial )
	{
		char szFile[MAX_PATH];
		Q_snprintf( szFile, sizeof( szFile ), "materials/%s.vmt", pszMaterial );
		return LoadExpandedVMTFile( szFile, 0 );
	}

	//-------------------------------------------------------------------------
	// What the engine has already built around a material. A swap must leave it all unchanged.
	//-------------------------------------------------------------------------
	struct MatSig_t
	{
		VertexFormat_t	m_Format;
		bool			m_bLightmap;
		bool			m_bBumpedLightmap;
		int				m_nOpacity;
		char			m_szTex[4][MAX_PATH];
	};

	const char *s_pszSigTextures[4] = { "$basetexture", "$bumpmap", "$basetexture2", "$envmap" };

	void TextureName( IMaterial *pMat, const char *pszVar, char *pszOut, int nOut )
	{
		pszOut[0] = 0;
		bool bFound = false;
		IMaterialVar *pVar = pMat->FindVar( pszVar, &bFound, false );
		if ( !bFound || !pVar || !pVar->IsDefined() )
			return;
		const char *psz = NULL;
		if ( pVar->IsTexture() )
		{
			ITexture *pTex = pVar->GetTextureValue();
			psz = pTex ? pTex->GetName() : NULL;
		}
		else
		{
			psz = pVar->GetStringValue();
		}
		Q_strncpy( pszOut, psz ? psz : "", nOut );
		Q_FixSlashes( pszOut, '/' );
		Q_strlower( pszOut );
	}

	void TakeSig( IMaterial *pMat, MatSig_t &sig )
	{
		sig.m_Format = pMat->GetVertexFormat();
		sig.m_bLightmap = pMat->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP );
		sig.m_bBumpedLightmap = pMat->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_BUMPED_LIGHTMAPS );
		sig.m_nOpacity = pMat->GetPropertyFlag( MATERIAL_PROPERTY_OPACITY );
		for ( int i = 0; i < 4; i++ )
			TextureName( pMat, s_pszSigTextures[i], sig.m_szTex[i], sizeof( sig.m_szTex[i] ) );
	}

	// Returns NULL when equal, otherwise what differs.
	const char *SigDiff( const MatSig_t &a, const MatSig_t &b )
	{
		if ( a.m_Format != b.m_Format )					return "vertex format";
		if ( a.m_bLightmap != b.m_bLightmap )			return "lightmap need";
		if ( a.m_bBumpedLightmap != b.m_bBumpedLightmap ) return "bumped lightmap need";
		if ( a.m_nOpacity != b.m_nOpacity )				return "opacity";
		for ( int i = 0; i < 4; i++ )
			if ( Q_strcmp( a.m_szTex[i], b.m_szTex[i] ) )
				return s_pszSigTextures[i];
		return NULL;
	}
}

//-----------------------------------------------------------------------------
// The swapper
//-----------------------------------------------------------------------------
class CDODSPShaderSwap : public CAutoGameSystemPerFrame
{
public:
	CDODSPShaderSwap() : CAutoGameSystemPerFrame( "CDODSPShaderSwap" ), m_Refused( 0, 0, DefLessFunc( IMaterial * ) )
	{
		m_bInLevel = false;
		m_bCheckedShaders = false;
		m_bUseDoDSP = false;
		Q_strncpy( m_szNoDoDSP, "not decided yet", sizeof( m_szNoDoDSP ) );
		for ( int i = 0; i < SWAP_NUM; i++ )
			m_bTargetLoaded[i] = false;
		m_flNextSweep = 0.0f;
		ResetCounts();
	}

	virtual void LevelInitPostEntity()
	{
		m_bInLevel = true;
		m_flNextSweep = 0.0f;
		ResetCounts();
		m_Refused.RemoveAll();	// material pointers are only meaningful within one map
		Sweep( "map load" );
		Msg( "DOD-SP SDK shaders: %d materials moved to Mapbase shaders (world %d, blend %d, models %d); "
			 "%d left stock (no .vmt), %d reverted\n",
			 m_nMoved[0] + m_nMoved[1] + m_nMoved[2], m_nMoved[0], m_nMoved[1], m_nMoved[2], m_nNoFile, m_nReverted );
		if ( m_bUseDoDSP )
		{
			Msg( "DOD-SP sun shaders: %d world materials on DOD-SP's sun shaders, %d handed back to Mapbase's "
				 "(a feature DOD-SP's brush shader leaves out - see dodsp_sdkshaders_report)\n", m_nOnDoDSP, m_nHandedBack );
			// INVARIANT: with dodsp_sun_baked 0 every lit world material must get the sun per pixel, or it
			// has no direct sun at all. Say so at load instead of leaving a dark surface to be hunted.
			int nNoSun = CountNoSun( false );
			if ( nNoSun )
				Warning( "INVARIANT: DOD-SP sun: %d world material%s can't take the per-pixel sun - with dodsp_sun_baked 0 "
						 "they get no direct sun and draw dark (dodsp_sdkshaders_report lists them)\n", nNoSun, nNoSun == 1 ? "" : "s" );
		}
		else
			Msg( "DOD-SP sun shaders: not used (%s)\n", m_szNoDoDSP );
	}

	virtual void LevelShutdownPreEntity()
	{
		m_bInLevel = false;
	}

	// Watchdog: props and effects loaded after map start (spawned models, gibs) get the same
	// treatment. Only materials already in use are looked at, so nothing is loaded early.
	virtual void Update( float frametime )
	{
		if ( !m_bInLevel || gpGlobals->realtime < m_flNextSweep )
			return;
		m_flNextSweep = gpGlobals->realtime + 2.0f;
		Sweep( NULL );
	}

	bool SunShadersInUse() const { return m_bCheckedShaders && m_bUseDoDSP && dodsp_sdkshaders.GetBool(); }

	void Report()
	{
		Msg( "dodsp_sdkshaders %d: this map moved %d (world %d, blend %d, models %d), %d without .vmt, %d reverted\n",
			 dodsp_sdkshaders.GetInt(), m_nMoved[0] + m_nMoved[1] + m_nMoved[2], m_nMoved[0], m_nMoved[1], m_nMoved[2],
			 m_nNoFile, m_nReverted );
		int nStock = 0;
		for ( MaterialHandle_t h = materials->FirstMaterial(); h != materials->InvalidMaterial(); h = materials->NextMaterial( h ) )
		{
			IMaterial *pMat = materials->GetMaterial( h );
			if ( !pMat || !pMat->IsPrecached() || pMat->IsErrorMaterial() || SkipByName( pMat->GetName() ) )
				continue;
			const ShaderSwap_t *pStock = FindSwap( pMat->GetShaderName() );
			if ( pStock && !pStock->m_bSunOnly )
			{
				if ( nStock < 40 )
					Msg( "  still stock: %-48s %s\n", pMat->GetName(), pMat->GetShaderName() );
				nStock++;
			}
		}
		Msg( "  %d lit materials in use are still on stock shaders\n", nStock );

		// World materials that run without the per-pixel sun: with dodsp_sun_baked 0 they get no direct sun
		Msg( "DOD-SP sun shaders: %s; %d on them, %d handed back\n", m_bUseDoDSP ? "in use" : m_szNoDoDSP, m_nOnDoDSP, m_nHandedBack );
		int nBack = CountNoSun( true );
		if ( m_bUseDoDSP )
			Msg( "  %d world materials in use draw without the per-pixel sun\n", nBack );
	}

private:
	// Lit world materials still on Mapbase's brush shaders, i.e. without the per-pixel sun
	int CountNoSun( bool bPrint )
	{
		int nBack = 0;
		for ( MaterialHandle_t h = materials->FirstMaterial(); h != materials->InvalidMaterial(); h = materials->NextMaterial( h ) )
		{
			IMaterial *pMat = materials->GetMaterial( h );
			if ( !pMat || !pMat->IsPrecached() || pMat->IsErrorMaterial() || SkipByName( pMat->GetName() ) )
				continue;
			const char *pszShader = pMat->GetShaderName();
			if ( IsShaderOrDX9( pszShader, "SDK_LightmappedGeneric" ) || IsShaderOrDX9( pszShader, "SDK_WorldVertexTransition" ) )
			{
				if ( bPrint && nBack < 40 )
					Msg( "  no per-pixel sun: %-48s %s\n", pMat->GetName(), pszShader );
				nBack++;
			}
		}
		return nBack;
	}

	void ResetCounts()
	{
		m_nMoved[0] = m_nMoved[1] = m_nMoved[2] = 0;
		m_nNoFile = m_nReverted = 0;
		m_nOnDoDSP = m_nHandedBack = 0;
	}

	int Family( const char *pszSDK )
	{
		if ( !Q_stricmp( pszSDK, "SDK_LightmappedGeneric" ) )	return 0;
		if ( !Q_stricmp( pszSDK, "SDK_WorldVertexTransition" ) ) return 1;
		return 2;
	}

	void Sweep( const char *pszWhy )
	{
		if ( !dodsp_sdkshaders.GetBool() )
			return;

		if ( !m_bCheckedShaders )
		{
			m_bCheckedShaders = true;

			// DOD-SP's sun shaders: one decision per game start, so a map never mixes the two
			m_bUseDoDSP = false;
			if ( !dodsp_sunshader.GetBool() )
				Q_strncpy( m_szNoDoDSP, "dodsp_sunshader 0", sizeof( m_szNoDoDSP ) );
			else if ( !g_pMaterialSystemHardwareConfig->SupportsShaderModel_3_0() )
				Q_strncpy( m_szNoDoDSP, "no shader model 3.0", sizeof( m_szNoDoDSP ) );
			else if ( !DoDSP_ShaderExists( DODSP_SHADER_LMG ) || !DoDSP_ShaderExists( DODSP_SHADER_WVT ) )
				Q_strncpy( m_szNoDoDSP, DODSP_SHADER_DLL_NAME ".dll without the sun shaders (older build?)", sizeof( m_szNoDoDSP ) );
			else
				m_bUseDoDSP = true;
			for ( int i = 0; i < SWAP_NUM; i++ )
			{
				m_bTargetLoaded[i] = DoDSP_ShaderExists( s_Swaps[i].m_pszSDK );
				if ( !m_bTargetLoaded[i] )
					Warning( "INVARIANT: DOD-SP SDK shaders: %s is not loaded - %s materials stay stock (Mapbase shader DLL missing?)\n",
							 s_Swaps[i].m_pszSDK, s_Swaps[i].m_pszStock );
			}
		}

		// Collect first: re-initialising materials while walking the material list is not allowed.
		CUtlVector<IMaterial *> todo;
		CUtlVector<int> todoSwap;
		for ( MaterialHandle_t h = materials->FirstMaterial(); h != materials->InvalidMaterial(); h = materials->NextMaterial( h ) )
		{
			IMaterial *pMat = materials->GetMaterial( h );
			if ( !pMat || !pMat->IsPrecached() || pMat->IsErrorMaterial() )
				continue;
			const ShaderSwap_t *pSwap = FindSwap( pMat->GetShaderName() );
			if ( !pSwap || !m_bTargetLoaded[pSwap - s_Swaps] || SkipByName( pMat->GetName() ) )
				continue;
			if ( pSwap->m_bSunOnly && !m_bUseDoDSP )
				continue;	// already on Mapbase's shader, and no sun twin to move it to
			if ( m_Refused.Find( pMat ) != m_Refused.InvalidIndex() )
				continue;
			todo.AddToTail( pMat );
			todoSwap.AddToTail( pSwap - s_Swaps );
		}

		for ( int i = 0; i < todo.Count(); i++ )
			SwapOne( todo[i], s_Swaps[todoSwap[i]] );

		if ( !pszWhy && todo.Count() )
			DevMsg( "DOD-SP SDK shaders: %d late materials moved\n", todo.Count() );
	}

	void SwapOne( IMaterial *pMat, const ShaderSwap_t &swap )
	{
		KeyValues *pKV = LoadExpandedVMT( pMat->GetName() );
		if ( !pKV )
		{
			// Made in code or missing on disk: nothing to rebuild from, leave it alone for good.
			m_Refused.Insert( pMat );
			m_nNoFile++;
			return;
		}

		MatSig_t before, after;
		TakeSig( pMat, before );

		const char *pszTarget = ( m_bUseDoDSP && swap.m_pszDoDSP ) ? swap.m_pszDoDSP : swap.m_pszSDK;

		KeyValues *pStockKV = pKV->MakeCopy();
		pKV->SetName( pszTarget );
		pMat->SetShaderAndParams( pKV );
		pKV->deleteThis();

		TakeSig( pMat, after );
		const char *pszDiff = SigDiff( before, after );
		const char *pszResolved = pMat->GetShaderName();
		const bool bOnTarget = IsShaderOrDX9( pszResolved, pszTarget );
		const bool bHandedBack = !bOnTarget && pszTarget != swap.m_pszSDK && IsShaderOrDX9( pszResolved, swap.m_pszSDK );
		if ( !pszDiff && !bOnTarget && !bHandedBack )
			pszDiff = "shader did not take";

		if ( pszDiff )
		{
			// Put it back exactly as it was and never try this one again.
			pStockKV->SetName( swap.m_pszStock );
			pMat->SetShaderAndParams( pStockKV );
			m_Refused.Insert( pMat );
			m_nReverted++;
			Warning( "INVARIANT: DOD-SP SDK shaders: %s reverted to %s - swap changed its %s\n",
					 pMat->GetName(), swap.m_pszStock, pszDiff );
		}
		else
		{
			m_nMoved[Family( swap.m_pszSDK )]++;
			if ( pszTarget != swap.m_pszSDK )
			{
				if ( bOnTarget )
					m_nOnDoDSP++;
				else
				{
					m_nHandedBack++;
					m_Refused.Insert( pMat );	// stays on Mapbase's shader for good; don't re-try every sweep
				}
			}
			if ( dodsp_sdkshaders_log.GetBool() )
				Msg( "  %-56s %s -> %s\n", pMat->GetName(), swap.m_pszStock, pszResolved );
		}
		pStockKV->deleteThis();
	}

	bool m_bInLevel;
	bool m_bCheckedShaders;
	bool m_bTargetLoaded[SWAP_NUM];
	float m_flNextSweep;
	int m_nMoved[3];
	int m_nNoFile;
	int m_nReverted;
	bool m_bUseDoDSP;
	char m_szNoDoDSP[128];
	int m_nOnDoDSP;
	int m_nHandedBack;
	CUtlRBTree<IMaterial *> m_Refused;
};

static CDODSPShaderSwap s_DODSPShaderSwap;

bool DoDSP_SunShadersInUse()
{
	return s_DODSPShaderSwap.SunShadersInUse();
}

CON_COMMAND( dodsp_sdkshaders_report, "DOD-SP: how many materials run on Mapbase SDK_ shaders, and which lit ones don't" )
{
	s_DODSPShaderSwap.Report();
}
