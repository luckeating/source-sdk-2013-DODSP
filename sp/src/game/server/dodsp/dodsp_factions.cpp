//====== DOD-SP =====================================================================
//
// Factions - see dodsp_factions.h. One owner for the class -> side mapping.
//
//==================================================================================
#include "cbase.h"
#include "dodsp_factions.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static const char *s_pszFactionNames[] = { "none", "axis", "allies" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszFactionNames ) == NUM_DODSP_FACTIONS );

const char *DoDSP_FactionName( int eFaction )
{
	return ( eFaction >= 0 && eFaction < NUM_DODSP_FACTIONS ) ? s_pszFactionNames[eFaction] : "?";
}

int DoDSP_FactionFromName( const char *pszName )
{
	if ( !pszName || !pszName[0] )
		return -1;
	static const struct { const char *pszName; int eFaction; } s_Names[] =
	{
		{ "none", DODSP_FACTION_NONE }, { "civilian", DODSP_FACTION_NONE },
		{ "axis", DODSP_FACTION_AXIS }, { "german", DODSP_FACTION_AXIS }, { "ge", DODSP_FACTION_AXIS },
		{ "allies", DODSP_FACTION_ALLIES }, { "allied", DODSP_FACTION_ALLIES }, { "us", DODSP_FACTION_ALLIES },
		{ "american", DODSP_FACTION_ALLIES }, { "uk", DODSP_FACTION_ALLIES }, { "british", DODSP_FACTION_ALLIES },
		{ "ru", DODSP_FACTION_ALLIES }, { "russian", DODSP_FACTION_ALLIES },
	};
	for ( int i = 0; i < ARRAYSIZE( s_Names ); i++ )
		if ( !Q_stricmp( pszName, s_Names[i].pszName ) )
			return s_Names[i].eFaction;
	return -1;
}

// THE mapping (one owner). A class missing here is no side: it is never shot at and never shoots.
int DoDSP_FactionOf( CBaseEntity *pEntity )
{
	if ( !pEntity )
		return DODSP_FACTION_NONE;
	switch ( pEntity->Classify() )
	{
	case CLASS_PLAYER:
	case CLASS_PLAYER_ALLY:
	case CLASS_PLAYER_ALLY_VITAL:
	case CLASS_CITIZEN_REBEL:
	case CLASS_VORTIGAUNT:
		return DODSP_FACTION_ALLIES;
	case CLASS_COMBINE:				// npc_german_soldier (until DOD-SP has its own classes)
	case CLASS_METROPOLICE:
	case CLASS_MILITARY:
		return DODSP_FACTION_AXIS;
	default:
		return DODSP_FACTION_NONE;
	}
}

bool DoDSP_Hostile( int eA, int eB )
{
	return eA != DODSP_FACTION_NONE && eB != DODSP_FACTION_NONE && eA != eB;
}

Class_T DoDSP_ClassForFaction( int eFaction )
{
	switch ( eFaction )
	{
	case DODSP_FACTION_AXIS:	return CLASS_COMBINE;
	case DODSP_FACTION_ALLIES:	return CLASS_PLAYER_ALLY;
	default:					return CLASS_NONE;
	}
}
