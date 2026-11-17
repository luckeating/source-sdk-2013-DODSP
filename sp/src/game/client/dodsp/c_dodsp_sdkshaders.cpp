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
// Cvars: dodsp_sdkshaders (1, applies on map load), dodsp_sdkshaders_log (print each swap).
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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_sdkshaders( "dodsp_sdkshaders", "1", 0,
	"DOD-SP: run stock world/model materials on Mapbase's SDK_ shaders (needed for sun shadows). "
	"Applies when a map loads; turning it off needs a game restart." );
static ConVar dodsp_sdkshaders_log( "dodsp_sdkshaders_log", "0", 0, "DOD-SP: print every material moved to an SDK_ shader" );

namespace
{
	struct ShaderSwap_t
	{
		const char *m_pszStock;		// resolved shader name the engine reports
		const char *m_pszSDK;		// Mapbase twin
	};

	// The lit shaders. Unlit/sprite/water/refract materials take no sunlight and stay stock.
	const ShaderSwap_t s_Swaps[] =
	{
		{ "LightmappedGeneric",			"SDK_LightmappedGeneric" },
		{ "LightmappedGeneric_DX9",		"SDK_LightmappedGeneric" },
		{ "WorldVertexTransition",		"SDK_WorldVertexTransition" },
		{ "WorldVertexTransition_DX9",	"SDK_WorldVertexTransition" },
		{ "VertexLitGeneric",			"SDK_VertexLitGeneric" },
		{ "VertexLitGeneric_DX9",		"SDK_VertexLitGeneric" },
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

	bool ShaderLoaded( const char *pszShader )
	{
		int n = materials->ShaderCount();
		CUtlVector<IShader *> list;
		list.SetCount( n );
		n = materials->GetShaders( 0, n, list.Base() );
		for ( int i = 0; i < n; i++ )
			if ( list[i] && !Q_stricmp( list[i]->GetName(), pszShader ) )
				return true;
		return false;
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
			if ( FindSwap( pMat->GetShaderName() ) )
			{
				if ( nStock < 40 )
					Msg( "  still stock: %-48s %s\n", pMat->GetName(), pMat->GetShaderName() );
				nStock++;
			}
		}
		Msg( "  %d lit materials in use are still on stock shaders\n", nStock );
	}

private:
	void ResetCounts()
	{
		m_nMoved[0] = m_nMoved[1] = m_nMoved[2] = 0;
		m_nNoFile = m_nReverted = 0;
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
			for ( int i = 0; i < SWAP_NUM; i++ )
			{
				m_bTargetLoaded[i] = ShaderLoaded( s_Swaps[i].m_pszSDK );
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

		KeyValues *pStockKV = pKV->MakeCopy();
		pKV->SetName( swap.m_pszSDK );
		pMat->SetShaderAndParams( pKV );
		pKV->deleteThis();

		TakeSig( pMat, after );
		const char *pszDiff = SigDiff( before, after );
		if ( !pszDiff && Q_stricmp( pMat->GetShaderName(), swap.m_pszSDK ) )
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
			if ( dodsp_sdkshaders_log.GetBool() )
				Msg( "  %-56s %s -> %s\n", pMat->GetName(), swap.m_pszStock, swap.m_pszSDK );
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
	CUtlRBTree<IMaterial *> m_Refused;
};

static CDODSPShaderSwap s_DODSPShaderSwap;

CON_COMMAND( dodsp_sdkshaders_report, "DOD-SP: how many materials run on Mapbase SDK_ shaders, and which lit ones don't" )
{
	s_DODSPShaderSwap.Report();
}
