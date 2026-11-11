//=============================================================================//
//
// DOD-SP: German soldier NPC.
//
// Stage 2a: a thin subclass of the HL2 Combine soldier (npc_combine_s) that
// defaults to the Day of Defeat: Source German rifleman model. It proves the
// build pipeline works; DoD animations, prone, MG deploy and hand signals are
// layered on in later stages.
//
//=============================================================================//

#include "cbase.h"
#include "npc_combines.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define GERMAN_SOLDIER_DEFAULT_MODEL "models/german/npc_german_rifleman.mdl"

class CNPC_GermanSoldier : public CNPC_CombineS
{
	DECLARE_CLASS( CNPC_GermanSoldier, CNPC_CombineS );

public:
	void Precache( void );
};

LINK_ENTITY_TO_CLASS( npc_german_soldier, CNPC_GermanSoldier );

void CNPC_GermanSoldier::Precache( void )
{
	// A model set in Hammer wins; otherwise use the German rifleman.
	// Must happen before BaseClass::Precache, which would fall back to the Combine model.
	if ( !GetModelName() )
	{
		SetModelName( AllocPooledString( GERMAN_SOLDIER_DEFAULT_MODEL ) );
	}

	BaseClass::Precache();
}
