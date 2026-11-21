//========= DOD-SP ============================================================
// dodsp_vehicle_wheels.cpp - see dodsp_vehicle_wheels.h
//=============================================================================
#include "cbase.h"
#include "dodsp_vehicle_wheels.h"
#include "KeyValues.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

bool DoDSP_ReadWheels( const model_t *pModel, DoDSPWheels_t &out )
{
	out.flWheelbase = 0.0f;
	out.flBodyPivot = 0.0f;
	out.bBodyPivot = false;
	out.szBodyBone[0] = 0;
	out.wheels.RemoveAll();
	out.susp.RemoveAll();
	if ( !pModel )
		return false;
	const char *pszText = modelinfo->GetModelKeyValueText( pModel );
	if ( !pszText || !pszText[0] )
		return false;
	const char *pszModel = modelinfo->GetModelName( pModel );
	KeyValues *pAll = new KeyValues( "" );
	bool bFound = false;
	if ( pAll->LoadFromBuffer( pszModel, pszText ) )
	{
		KeyValues *pW = pAll->FindKey( DODSP_WHEELS_KEY );
		if ( pW )
		{
			bFound = true;
			for ( KeyValues *pKey = pW->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey() )
			{
				if ( !Q_stricmp( pKey->GetName(), "wheelbase" ) )
				{
					out.flWheelbase = pKey->GetFloat();
					continue;
				}
				if ( !Q_stricmp( pKey->GetName(), "body_bone" ) )
				{
					Q_strncpy( out.szBodyBone, pKey->GetString(), sizeof( out.szBodyBone ) );
					continue;
				}
				if ( !Q_stricmp( pKey->GetName(), "body_pivot" ) )
				{
					out.flBodyPivot = pKey->GetFloat();
					out.bBodyPivot = true;
					continue;
				}
				// "word value" pairs: wheels "radius <r> [steer 1]", suspension units "up <u> down <d>"
				float flRadius = 0.0f, flUp = -1.0f, flDown = -1.0f;
				bool bSteer = false;
				char szWord[32];
				const char *p = pKey->GetString();
				while ( p && *p )
				{
					while ( *p == ' ' || *p == '\t' ) p++;
					int n = 0;
					while ( *p && *p != ' ' && *p != '\t' && n < (int)sizeof( szWord ) - 1 ) szWord[n++] = *p++;
					szWord[n] = 0;
					while ( *p == ' ' || *p == '\t' ) p++;
					const char *pszValue = p;
					while ( *p && *p != ' ' && *p != '\t' ) p++;
					if ( !Q_stricmp( szWord, "radius" ) )
						flRadius = Q_atof( pszValue );
					else if ( !Q_stricmp( szWord, "steer" ) )
						bSteer = Q_atoi( pszValue ) != 0;
					else if ( !Q_stricmp( szWord, "up" ) )
						flUp = Q_atof( pszValue );
					else if ( !Q_stricmp( szWord, "down" ) )
						flDown = Q_atof( pszValue );
				}
				if ( !Q_strnicmp( pKey->GetName(), "susp_", 5 ) )
				{
					if ( flUp < 0.0f || flDown < 0.0f )
					{
						Warning( "INVARIANT DOD-SP wheels: %s: \"%s\" \"%s\" has no up/down travel - that unit stays put\n",
								 pszModel, pKey->GetName(), pKey->GetString() );
						continue;
					}
					DoDSPSusp_t su;
					Q_strncpy( su.szBone, pKey->GetName(), sizeof( su.szBone ) );
					su.flUp = flUp;
					su.flDown = flDown;
					out.susp.AddToTail( su );
					continue;
				}
				if ( flRadius <= 0.5f )
				{
					Warning( "INVARIANT DOD-SP wheels: %s: \"%s\" \"%s\" has no radius - that wheel keeps the height rule\n",
							 pszModel, pKey->GetName(), pKey->GetString() );
					continue;
				}
				DoDSPWheel_t w;
				Q_strncpy( w.szBone, pKey->GetName(), sizeof( w.szBone ) );
				w.flRadius = flRadius;
				w.bSteer = bSteer;
				out.wheels.AddToTail( w );
			}
			bool bSteers = false;
			FOR_EACH_VEC( out.wheels, i )
				bSteers |= out.wheels[i].bSteer;
			if ( bSteers && out.flWheelbase <= 1.0f )
				Warning( "INVARIANT DOD-SP wheels: %s has steered wheels but no wheelbase - they won't turn\n", pszModel );
		}
	}
	pAll->deleteThis();
	return bFound;
}
