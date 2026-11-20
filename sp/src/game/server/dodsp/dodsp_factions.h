//====== DOD-SP =====================================================================
//
// Factions - one owner for "who is hostile to whom" in DOD-SP's own code (vehicles first; soldiers later).
//
// Two sides: the AXIS (German) and the ALLIES (American, British, Russian - all hostile to the Axis, friendly
// to each other). A nation is not a side: the chatter's "us/uk/ru" picks which words the Germans use, the
// faction picks who gets shot. Civilians belong to no side and are never targets.
//
// Today every entity's side comes from its AI class (Classify): the player and his allies are Allied, the
// German soldier (CLASS_COMBINE until DOD-SP has its own classes) is Axis, a dodsp_vehicle says which side it
// is on. DoDSP_FactionOf is the ONLY place that mapping lives - add a class here, never next to a caller.
//
//==================================================================================
#ifndef DODSP_FACTIONS_H
#define DODSP_FACTIONS_H
#ifdef _WIN32
#pragma once
#endif

enum DoDSPFaction_t
{
	DODSP_FACTION_NONE,			// civilians, props, the world: never a target
	DODSP_FACTION_AXIS,
	DODSP_FACTION_ALLIES,
	NUM_DODSP_FACTIONS
};

const char		*DoDSP_FactionName( int eFaction );
int				DoDSP_FactionFromName( const char *pszName );		// "axis"/"german" ... "allies"/"us"/"uk"/"ru"; -1 = unknown
int				DoDSP_FactionOf( CBaseEntity *pEntity );
bool			DoDSP_Hostile( int eA, int eB );
Class_T			DoDSP_ClassForFaction( int eFaction );			// what a faction's own entity answers to Classify

#endif // DODSP_FACTIONS_H
