//===================== DOD-SP: where a model's light is read =====================//
//
// The engine lights every moving model (props, NPCs) from ONE point: the model's $illumposition (or
// its illumination attachment) through the model's transform. That point is baked into each .mdl
// by whoever compiled it, and for some content it is plainly wrong. Found 2026-11-18 (cafe map,
// dodsp_prop_light): models/ww2/ro1-dish.mdl, a plate lying on the floor, reads its light from a
// point inside solid about 378 units away - the engine gave it ambient 169 where the room around it
// has 43, so the plates glowed in the shade.
//
// INVARIANT: a model is lit from a point inside (or within DODSP_LIGHTORIGIN_SLACK of) its own
// bounds, in open space. When the model's own point breaks that, the model is lit from the middle
// of its bounds instead, lifted out of solid if needed. One owner: this function, called from the
// one place that hands the engine a model's draw info (C_BaseAnimating::InternalDrawModel).
// A mapper's own lighting origin (the "LightingOrigin" key, info_lighting_relative) is left alone.
// Guards: dodsp_lint_client.py (every write of pLightingOrigin is tagged 'dodsp-lint: lighting
// origin'); the console names each model the first time it is corrected on a map, and prints an
// INVARIANT line if no open point inside its bounds exists at all (then the engine's point stays).
// dodsp_lightorigin_fix 0 = the engine's points everywhere (for comparison).
//
//=============================================================================//

#include "cbase.h"
#include "c_baseanimating.h"
#include "c_world.h"
#include "studio.h"
#include "igamesystem.h"
#include "utldict.h"
#include "collisionutils.h"	// IsPointInBox

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_lightorigin_fix( "dodsp_lightorigin_fix", "1", 0,
	"DOD-SP: models whose built-in lighting point is outside their bounds or inside solid are lit from their middle instead (0 = the engine's point)" );

#define DODSP_LIGHTORIGIN_SLACK	8.0f	// units outside the bounds the model's own point may sit

namespace
{
	// Models reported this map, so each one is named once
	class CDoDSPLightOriginReport : public CAutoGameSystem
	{
	public:
		CDoDSPLightOriginReport() : CAutoGameSystem( "DoDSPLightOriginReport" ) {}
		virtual void LevelShutdownPostEntity() { m_Reported.Purge(); }

		// true the first time this model is reported on this map
		bool First( const char *pszModel )
		{
			if ( m_Reported.Find( pszModel ) != m_Reported.InvalidIndex() )
				return false;
			m_Reported.Insert( pszModel, 0 );
			return true;
		}

	private:
		CUtlDict< int, int > m_Reported;
	};
	CDoDSPLightOriginReport s_LightOriginReport;

	// World brushes only: a point inside the model's own collision hull is fine to read light at
	bool DoDSP_InWorldSolid( const Vector &v )
	{
		C_World *pWorld = GetClientWorldEntity();
		if ( !pWorld || !pWorld->GetCollideable() )
			return false;
		return ( enginetrace->GetPointContents_Collideable( pWorld->GetCollideable(), v ) & CONTENTS_SOLID ) != 0;
	}
}

//-----------------------------------------------------------------------------
// The point this model should be lit from, or NULL for the engine's own. The returned pointer lives
// in this entity (the engine reads it during the draw).
//-----------------------------------------------------------------------------
const Vector *C_BaseAnimating::DoDSP_LightingOrigin( const matrix3x4_t &modelToWorld, const char **ppszWhy )
{
	if ( ppszWhy )
		*ppszWhy = "the model's own point";
	if ( !dodsp_lightorigin_fix.GetBool() || IsViewModel() )
		return NULL;
	CStudioHdr *pHdr = GetModelPtr();
	if ( !pHdr || !pHdr->IsValid() )
		return NULL;
	const studiohdr_t *pStudio = pHdr->GetRenderHdr();

	// the engine's point, worked out the way the engine does
	Vector vecEngine;
	matrix3x4_t attachment;
	const int nAttach = pStudio->IllumPositionAttachmentIndex();
	const bool bAttached = nAttach > 0 && GetAttachment( nAttach, attachment );
	VectorTransform( pStudio->illumposition, bAttached ? attachment : modelToWorld, vecEngine );

	// same point as last time: same answer
	if ( m_nDoDSPLightOrigin != DODSP_LIGHTORIGIN_UNKNOWN && vecEngine.DistToSqr( m_vecDoDSPLightCheckedAt ) < 0.25f )
	{
		if ( ppszWhy && m_nDoDSPLightOrigin == DODSP_LIGHTORIGIN_MOVED )
			*ppszWhy = m_pszDoDSPLightWhy;
		return ( m_nDoDSPLightOrigin == DODSP_LIGHTORIGIN_MOVED ) ? &m_vecDoDSPLightingOrigin : NULL;
	}
	m_vecDoDSPLightCheckedAt = vecEngine;

	Vector vecMins, vecMaxs;
	CollisionProp()->WorldSpaceAABB( &vecMins, &vecMaxs );
	const Vector vecSlack( DODSP_LIGHTORIGIN_SLACK, DODSP_LIGHTORIGIN_SLACK, DODSP_LIGHTORIGIN_SLACK );
	const bool bOutside = !IsPointInBox( vecEngine, vecMins - vecSlack, vecMaxs + vecSlack );
	const bool bSolid = DoDSP_InWorldSolid( vecEngine );
	if ( !bOutside && !bSolid )
	{
		m_nDoDSPLightOrigin = DODSP_LIGHTORIGIN_ENGINE;
		return NULL;
	}

	// the middle of its bounds, lifted out of solid if it has to be (never above its own top)
	const char *pszWhy = bOutside ? "its own point is outside the model" : "its own point is inside solid";
	const Vector vecMid = 0.5f * ( vecMins + vecMaxs );
	static const float s_flLift[] = { 0.0f, 4.0f, 8.0f, 16.0f, 32.0f };
	for ( int i = 0; i < ARRAYSIZE( s_flLift ); i++ )
	{
		Vector v = vecMid;
		v.z = MIN( v.z + s_flLift[i], vecMaxs.z );
		if ( DoDSP_InWorldSolid( v ) )
			continue;
		m_vecDoDSPLightingOrigin = v;
		m_nDoDSPLightOrigin = DODSP_LIGHTORIGIN_MOVED;
		m_pszDoDSPLightWhy = pszWhy;
		if ( ppszWhy )
			*ppszWhy = pszWhy;
		const char *pszModel = modelinfo->GetModelName( GetModel() );
		if ( s_LightOriginReport.First( pszModel ) )
			Msg( "DOD-SP lighting point: %s - %s (%.0f units from the model's middle), lit from its middle instead\n",
				 pszModel, pszWhy, vecEngine.DistTo( vecMid ) );
		return &m_vecDoDSPLightingOrigin;
	}

	m_nDoDSPLightOrigin = DODSP_LIGHTORIGIN_ENGINE;
	const char *pszModel = modelinfo->GetModelName( GetModel() );
	if ( s_LightOriginReport.First( pszModel ) )
		Warning( "INVARIANT: DOD-SP lighting point: %s - %s and no open point inside its bounds - the engine's point stays\n",
				 pszModel, pszWhy );
	return NULL;
}
