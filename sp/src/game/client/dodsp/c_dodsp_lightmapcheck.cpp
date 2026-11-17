//===================== DOD-SP lightmap check =====================//
//
// INVARIANT: every world material is lit the way the map was compiled for it.
//
// VBSP decides per material, at compile time, whether its faces get a lightmap and whether it is
// a bumped one (4 lightmaps per face instead of 1). It asks the material's shader, through the
// compile tools' own material system. The game asks the same questions of ITS copy of the material
// at run time. If the two answers differ - a different .vmt copy wins in game, or the compile tools
// could not find the shader (Mapbase's SDK_ shaders) - the shader reads lighting the map doesn't
// have, and the face draws dark. Found 2026-11-17: every HL2 material in the cafe map drew dark
// (fine under mat_fullbright, unaffected by cubemaps), DoD and custom materials were fine.
//
// At every map load this file reads the map's faces from the BSP and compares, per material:
//   compiled   - no lightmap / flat / bumped  (BSP: texinfo SURF_NOLIGHT / SURF_BUMPLIGHT, lightofs)
//   game wants - MATERIAL_PROPERTY_NEEDS_LIGHTMAP / NEEDS_BUMPED_LIGHTMAPS of the material in use
// Any mismatch prints one INVARIANT line at load.
//
// dodsp_lightmap_report - every material that doesn't match, with its shader and which file won
// dodsp_lightmap_here   - the same for the surface under the crosshair (matching or not)
//
//=============================================================================//

#include "cbase.h"
#include "igamesystem.h"
#include "filesystem.h"
#include "tier1/utlbuffer.h"
#include "bspfile.h"
#include "bspflags.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "c_baseplayer.h"
#include "utldict.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{
	struct MatLighting_t
	{
		int		m_nFaces;
		int		m_nNoLight;		// compiled with no lightmap
		int		m_nFlat;		// compiled with one lightmap
		int		m_nBumped;		// compiled with bumped lightmaps
		bool	m_bFound;		// the game could load the material
		bool	m_bWantsLight;
		bool	m_bWantsBump;
		char	m_szShader[64];
	};

	enum Verdict_t { LM_OK, LM_NEED_LIGHTMAP, LM_NEED_BUMP, LM_MISSING };

	class CDoDSPLightmapCheck : public CAutoGameSystem
	{
	public:
		CDoDSPLightmapCheck() : CAutoGameSystem( "DoDSPLightmapCheck" ) {}

		virtual void LevelInitPostEntity()		{ Check(); }
		virtual void LevelShutdownPostEntity()	{ m_Mats.Purge(); }

		void Report();
		void Here();

	private:
		bool ReadLump( FileHandle_t h, const dheader_t &hdr, int nLump, CUtlBuffer &buf );
		void Check();
		Verdict_t Judge( const MatLighting_t &m ) const;
		void PrintOne( const char *pszName, const MatLighting_t &m ) const;

		CUtlDict< MatLighting_t, int >	m_Mats;
	};

	CDoDSPLightmapCheck s_LightmapCheck;

	bool CDoDSPLightmapCheck::ReadLump( FileHandle_t h, const dheader_t &hdr, int nLump, CUtlBuffer &buf )
	{
		const lump_t &l = hdr.lumps[nLump];
		buf.Purge();
		if ( l.filelen <= 0 )
			return false;
		if ( *(const int *)l.fourCC != 0 )	// LZMA-compressed lump (bspzip -repack); not handled
		{
			Warning( "DOD-SP lightmap check: BSP lump %d is compressed - check skipped\n", nLump );
			return false;
		}
		buf.EnsureCapacity( l.filelen );
		g_pFullFileSystem->Seek( h, l.fileofs, FILESYSTEM_SEEK_HEAD );
		if ( g_pFullFileSystem->Read( buf.Base(), l.filelen, h ) != l.filelen )
			return false;
		buf.SeekPut( CUtlBuffer::SEEK_HEAD, l.filelen );
		return true;
	}

	void CDoDSPLightmapCheck::Check()
	{
		m_Mats.Purge();
		const model_t *pWorld = modelinfo->GetModel( 1 );
		const char *pszMap = pWorld ? modelinfo->GetModelName( pWorld ) : NULL;
		if ( !pszMap || !*pszMap )
			return;
		FileHandle_t h = g_pFullFileSystem->Open( pszMap, "rb" );
		if ( !h )
		{
			Warning( "DOD-SP lightmap check: can't open %s\n", pszMap );
			return;
		}
		dheader_t hdr;
		g_pFullFileSystem->Read( &hdr, sizeof( hdr ), h );

		// The face set the engine draws with: HDR faces when HDR is on and the map has them
		bool bHDR = g_pMaterialSystemHardwareConfig->GetHDREnabled() && hdr.lumps[LUMP_FACES_HDR].filelen > 0;
		CUtlBuffer faces, texinfo, texdata, strData, strTable;
		bool bOK = ReadLump( h, hdr, bHDR ? LUMP_FACES_HDR : LUMP_FACES, faces ) &&
				   ReadLump( h, hdr, LUMP_TEXINFO, texinfo ) &&
				   ReadLump( h, hdr, LUMP_TEXDATA, texdata ) &&
				   ReadLump( h, hdr, LUMP_TEXDATA_STRING_DATA, strData ) &&
				   ReadLump( h, hdr, LUMP_TEXDATA_STRING_TABLE, strTable );
		g_pFullFileSystem->Close( h );
		if ( !bOK )
			return;

		const dface_t *pFaces = (const dface_t *)faces.Base();
		const texinfo_t *pTexinfo = (const texinfo_t *)texinfo.Base();
		const dtexdata_t *pTexdata = (const dtexdata_t *)texdata.Base();
		const int *pStrTable = (const int *)strTable.Base();
		const char *pStrData = (const char *)strData.Base();
		int nFaces = faces.TellPut() / sizeof( dface_t );
		int nTexinfo = texinfo.TellPut() / sizeof( texinfo_t );
		int nTexdata = texdata.TellPut() / sizeof( dtexdata_t );
		int nStrings = strTable.TellPut() / sizeof( int );

		for ( int i = 0; i < nFaces; i++ )
		{
			const dface_t &f = pFaces[i];
			if ( f.texinfo < 0 || f.texinfo >= nTexinfo )
				continue;
			const texinfo_t &ti = pTexinfo[f.texinfo];
			if ( ti.flags & ( SURF_SKY | SURF_SKY2D | SURF_NODRAW | SURF_HINT | SURF_SKIP | SURF_TRIGGER ) )
				continue;
			if ( ti.texdata < 0 || ti.texdata >= nTexdata )
				continue;
			int nStr = pTexdata[ti.texdata].nameStringTableID;
			if ( nStr < 0 || nStr >= nStrings || pStrTable[nStr] < 0 || pStrTable[nStr] >= strData.TellPut() )
				continue;
			const char *pszName = pStrData + pStrTable[nStr];

			int idx = m_Mats.Find( pszName );
			if ( idx == m_Mats.InvalidIndex() )
			{
				MatLighting_t m;
				Q_memset( &m, 0, sizeof( m ) );
				IMaterial *pMat = materials->FindMaterial( pszName, TEXTURE_GROUP_WORLD, false );
				m.m_bFound = pMat && !pMat->IsErrorMaterial();
				if ( m.m_bFound )
				{
					m.m_bWantsLight = pMat->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_LIGHTMAP );
					m.m_bWantsBump = pMat->GetPropertyFlag( MATERIAL_PROPERTY_NEEDS_BUMPED_LIGHTMAPS );
					Q_strncpy( m.m_szShader, pMat->GetShaderName(), sizeof( m.m_szShader ) );
				}
				idx = m_Mats.Insert( pszName, m );
			}
			MatLighting_t &m = m_Mats[idx];
			m.m_nFaces++;
			if ( ( ti.flags & SURF_NOLIGHT ) || f.lightofs < 0 )
				m.m_nNoLight++;
			else if ( ti.flags & SURF_BUMPLIGHT )
				m.m_nBumped++;
			else
				m.m_nFlat++;
		}

		int nBad = 0, nMissing = 0;
		for ( int i = m_Mats.First(); i != m_Mats.InvalidIndex(); i = m_Mats.Next( i ) )
		{
			Verdict_t v = Judge( m_Mats[i] );
			if ( v == LM_MISSING )
				nMissing++;
			else if ( v != LM_OK )
				nBad++;
		}
		if ( nBad )
			Warning( "INVARIANT: DOD-SP lightmaps: %d world material%s lit differently from how the map was compiled "
					 "(they draw dark) - dodsp_lightmap_report lists them\n", nBad, nBad == 1 ? " is" : "s are" );
		else
			Msg( "DOD-SP lightmaps: all %d world materials match how the map was compiled\n", m_Mats.Count() - nMissing );
		if ( nMissing )
			Warning( "DOD-SP lightmaps: %d world material%s missing in game - dodsp_lightmap_report lists them\n",
					 nMissing, nMissing == 1 ? " is" : "s are" );
	}

	Verdict_t CDoDSPLightmapCheck::Judge( const MatLighting_t &m ) const
	{
		if ( !m.m_bFound )
			return LM_MISSING;
		if ( m.m_bWantsLight && m.m_nNoLight )
			return LM_NEED_LIGHTMAP;
		if ( m.m_bWantsBump && m.m_nFlat )
			return LM_NEED_BUMP;
		return LM_OK;	// bumped faces drawn flat are fine: the flat lightmap is stored first
	}

	void CDoDSPLightmapCheck::PrintOne( const char *pszName, const MatLighting_t &m ) const
	{
		char szWants[32];
		if ( !m.m_bFound )
			Q_strncpy( szWants, "MISSING in game", sizeof( szWants ) );
		else
			Q_strncpy( szWants, !m.m_bWantsLight ? "no lightmap" : ( m.m_bWantsBump ? "bumped" : "flat" ), sizeof( szWants ) );

		char szFile[MAX_PATH], szFull[MAX_PATH];
		Q_snprintf( szFile, sizeof( szFile ), "materials/%s.vmt", pszName );
		if ( !g_pFullFileSystem->RelativePathToFullPath( szFile, "GAME", szFull, sizeof( szFull ) ) )
			Q_strncpy( szFull, "(file not found - patched into the map, or in a VPK)", sizeof( szFull ) );

		const char *pszVerdict = "ok";
		switch ( Judge( m ) )
		{
		case LM_NEED_LIGHTMAP:	pszVerdict = "MISMATCH: compiled without a lightmap"; break;
		case LM_NEED_BUMP:		pszVerdict = "MISMATCH: compiled flat, game wants bumped"; break;
		case LM_MISSING:		pszVerdict = "MISSING"; break;
		default: break;
		}
		Msg( "  %s\n    %s | faces %d: none %d, flat %d, bumped %d | game: %s on %s\n    %s\n",
			 pszName, pszVerdict, m.m_nFaces, m.m_nNoLight, m.m_nFlat, m.m_nBumped, szWants,
			 m.m_szShader[0] ? m.m_szShader : "?", szFull );
	}

	void CDoDSPLightmapCheck::Report()
	{
		if ( !m_Mats.Count() )
		{
			Msg( "DOD-SP lightmaps: no map loaded (or the map couldn't be read)\n" );
			return;
		}
		int nShown = 0;
		Msg( "DOD-SP lightmaps: materials that don't match how the map was compiled\n" );
		for ( int i = m_Mats.First(); i != m_Mats.InvalidIndex(); i = m_Mats.Next( i ) )
		{
			if ( Judge( m_Mats[i] ) == LM_OK )
				continue;
			if ( nShown++ < 40 )
				PrintOne( m_Mats.GetElementName( i ), m_Mats[i] );
		}
		if ( !nShown )
			Msg( "  none - all %d match\n", m_Mats.Count() );
		else if ( nShown > 40 )
			Msg( "  ... and %d more\n", nShown - 40 );
	}

	void CDoDSPLightmapCheck::Here()
	{
		C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
		if ( !pPlayer )
			return;
		Vector vecForward;
		AngleVectors( pPlayer->EyeAngles(), &vecForward );
		trace_t tr;
		UTIL_TraceLine( pPlayer->EyePosition(), pPlayer->EyePosition() + vecForward * 8192.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr );
		if ( tr.fraction >= 1.0f || !tr.surface.name )
		{
			Msg( "DOD-SP lightmaps: nothing under the crosshair\n" );
			return;
		}
		int idx = m_Mats.Find( tr.surface.name );
		if ( idx == m_Mats.InvalidIndex() )
		{
			Msg( "DOD-SP lightmaps: %s is not a lit world surface (a model, or not in the map's faces)\n", tr.surface.name );
			return;
		}
		Msg( "DOD-SP lightmaps, under the crosshair:\n" );
		PrintOne( m_Mats.GetElementName( idx ), m_Mats[idx] );
	}
}

CON_COMMAND( dodsp_lightmap_report, "DOD-SP: world materials lit differently from how the map was compiled" )
{
	s_LightmapCheck.Report();
}

CON_COMMAND( dodsp_lightmap_here, "DOD-SP: how the surface under the crosshair was compiled vs what the game wants" )
{
	s_LightmapCheck.Here();
}
