//====== DOD-SP =====================================================================
//
// Vehicle armour and shells (stage V4) - see dodsp_vehicle_armour.h.
//
//==================================================================================
#include "cbase.h"
#include "KeyValues.h"
#include "ammodef.h"
#include "explode.h"
#include "basecombatcharacter.h"
#include "dodsp_vehicle_armour.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

const char *g_pszArmourPart[NUM_APARTS] = { "hull", "turret" };
const char *g_pszArmourFace[NUM_AFACES] = { "front", "side", "rear", "top", "bottom" };
const char *g_pszCrewRole[NUM_CREW_ROLES] = { "commander", "gunner", "loader", "driver", "radio" };

ConVar dodsp_vehicle_he_scale( "dodsp_vehicle_he_scale", "0.5", 0,
	"High-explosive blast size against people, x the real size (owner 2026-11-20: half). Armour always feels the real blast." );

DoDSPBlast_t g_DoDSPBlast = { false, 0.0f, vec3_origin };

// The 75 mm M48 HE shell: 0.68 kg TNT; kills ~10 m, wounds ~25 m in the open (forum figures, "depends who you
// read"). Every other charge scales by the cube root of its mass (blast radius law).
#define HE_ANCHOR_KG			0.68f
#define HE_ANCHOR_KILL_M		10.0f
#define HE_ANCHOR_WOUND_M		25.0f
// what a charge pierces at contact: anchored on the 152 mm OF-540 (~5.9 kg; its fragments pierce 20-30 mm - ML-20
// article) at 35 mm, cube-root law again. 75 mm HE ~17 mm, a grenade ~9 mm, 20 mm HE ~4 mm.
#define HE_PEN_ANCHOR_KG		5.9f
#define HE_PEN_ANCHOR_MM		35.0f

static bool ParsePlate( KeyValues *pKV, const char *pszKey, ArmourPlate_t &plate, bool bRequired )
{
	plate.flMM = 0.0f;
	plate.flSlope = 0.0f;
	const char *psz = pKV->GetString( pszKey, NULL );
	if ( !psz || !psz[0] )
		return !bRequired;
	sscanf( psz, "%f %f", &plate.flMM, &plate.flSlope );
	return plate.flMM >= 0.0f && plate.flSlope > -80.0f && plate.flSlope < 80.0f;
}

bool DoDSP_ParseArmour( KeyValues *pKV, DoDSPArmour_t &out, char *pszErr, int nErr )
{
	memset( &out, 0, sizeof( out ) );
	pszErr[0] = 0;
	if ( !pKV )
	{
		Q_strncpy( pszErr, "no \"armour\" block", nErr );
		return false;
	}
	static const char *s_pszKeys[NUM_APARTS][NUM_AFACES] =
	{
		{ "hull_front", "hull_side", "hull_rear", "hull_top", "hull_bottom" },
		{ "turret_front", "turret_side", "turret_rear", "turret_top", "turret_bottom" },
	};
	for ( int p = 0; p < NUM_APARTS; p++ )
	{
		for ( int f = 0; f < NUM_AFACES; f++ )
		{
			bool bRequired = ( p == APART_HULL && f != AFACE_BOTTOM );
			if ( !ParsePlate( pKV, s_pszKeys[p][f], out.plates[p][f], bRequired ) )
			{
				Q_snprintf( pszErr, nErr, "\"%s\" is missing or not \"mm [lean]\"", s_pszKeys[p][f] );
				return false;
			}
		}
		// a bottom not given = the top; a turret not given = the hull's top plate all round (no turret: never hit)
		if ( out.plates[p][AFACE_BOTTOM].flMM <= 0.0f )
			out.plates[p][AFACE_BOTTOM] = out.plates[p][AFACE_TOP];
	}
	out.bOpenTop = pKV->GetBool( "open_top", false );
	out.flStructure = pKV->GetFloat( "structure", 0.0f );
	out.flAmmoFire = clamp( pKV->GetFloat( "ammo_fire", 0.3f ), 0.0f, 1.0f );
	out.flFuelFire = clamp( pKV->GetFloat( "fuel_fire", 0.25f ), 0.0f, 1.0f );
	const char *pszEngine = pKV->GetString( "engine", "rear" );
	out.iEngineAt = !Q_stricmp( pszEngine, "front" ) ? AFACE_FRONT : !Q_stricmp( pszEngine, "middle" ) ? AFACE_TOP : AFACE_REAR;
	if ( out.flStructure <= 0.0f )
	{
		Q_strncpy( pszErr, "\"structure\" missing or 0", nErr );
		return false;
	}
	char szCrew[128];
	Q_strncpy( szCrew, pKV->GetString( "crew", "" ), sizeof( szCrew ) );
	for ( char *pTok = strtok( szCrew, " ,\t" ); pTok; pTok = strtok( NULL, " ,\t" ) )
	{
		int e = -1;
		for ( int r = 0; r < NUM_CREW_ROLES; r++ )
			if ( !Q_stricmp( pTok, g_pszCrewRole[r] ) )
				e = r;
		if ( e < 0 )
		{
			Q_snprintf( pszErr, nErr, "crew role '%s' is not commander / gunner / loader / driver / radio", pTok );
			return false;
		}
		if ( out.nCrew < DODSP_MAX_CREW )
			out.eCrew[out.nCrew++] = e;
	}
	if ( out.nCrew <= 0 )
	{
		Q_strncpy( pszErr, "\"crew\" lists nobody", nErr );
		return false;
	}
	out.bValid = true;
	return true;
}

bool DoDSP_ParseShell( KeyValues *pKV, DoDSPShell_t &out, bool bHE, char *pszErr, int nErr )
{
	memset( &out, 0, sizeof( out ) );
	pszErr[0] = 0;
	if ( !pKV )
		return true;		// a gun may lack one kind of ammunition; the caller checks it has some
	out.flCalibre = pKV->GetFloat( "calibre", 0.0f );
	out.flFiller = pKV->GetFloat( "filler", 0.0f );
	out.flEffect = pKV->GetFloat( "effect", 0.0f );
	if ( out.flCalibre <= 0.0f )
	{
		Q_strncpy( pszErr, "\"calibre\" missing", nErr );
		return false;
	}
	if ( bHE )
	{
		if ( out.flFiller <= 0.0f )
		{
			Q_strncpy( pszErr, "an \"he\" block needs \"filler\" (kg of explosive)", nErr );
			return false;
		}
		// what HE does to armour comes from its charge: no table to keep in step
		out.nPen = 0;
		if ( out.flEffect <= 0.0f )
			out.flEffect = 60.0f * powf( out.flFiller / HE_ANCHOR_KG, 1.0f / 3.0f );
		out.bValid = true;
		return true;
	}
	// "pen" "metres mm  metres mm ..." - ranges rising, penetration falling (INVARIANT otherwise)
	const char *psz = pKV->GetString( "pen", "" );
	while ( psz && *psz && out.nPen < DODSP_PEN_POINTS )
	{
		float r, mm;
		int nRead = 0;
		if ( sscanf( psz, " %f %f%n", &r, &mm, &nRead ) != 2 )
			break;
		out.flPenRange[out.nPen] = r;
		out.flPenMM[out.nPen] = mm;
		out.nPen++;
		psz += nRead;
	}
	if ( out.nPen < 1 )
	{
		Q_strncpy( pszErr, "\"pen\" needs \"metres mm\" pairs", nErr );
		return false;
	}
	for ( int i = 1; i < out.nPen; i++ )
	{
		if ( out.flPenRange[i] <= out.flPenRange[i - 1] || out.flPenMM[i] > out.flPenMM[i - 1] )
		{
			Q_snprintf( pszErr, nErr, "\"pen\" must go out in range and down in mm (point %d: %g m %g mm)", i + 1, out.flPenRange[i], out.flPenMM[i] );
			return false;
		}
	}
	if ( out.flEffect <= 0.0f )
	{
		Q_strncpy( pszErr, "\"effect\" missing", nErr );
		return false;
	}
	out.bValid = true;
	return true;
}

float DoDSP_ShellPen( const DoDSPShell_t &shell, float flDistUnits )
{
	if ( !shell.bValid )
		return 0.0f;
	if ( shell.nPen == 0 )
		return DoDSP_HEPen( shell.flFiller );		// HE pierces the same at any range
	float m = flDistUnits / DODSP_UNITS_PER_METRE;
	if ( shell.nPen == 1 || m <= shell.flPenRange[0] )
		return shell.flPenMM[0];
	for ( int i = 1; i < shell.nPen; i++ )
	{
		if ( m <= shell.flPenRange[i] )
		{
			float t = ( m - shell.flPenRange[i - 1] ) / ( shell.flPenRange[i] - shell.flPenRange[i - 1] );
			return shell.flPenMM[i - 1] + t * ( shell.flPenMM[i] - shell.flPenMM[i - 1] );
		}
	}
	// past the table: the last slope on, but never below 30% of the last figure
	int n = shell.nPen;
	float flSlope = ( shell.flPenMM[n - 1] - shell.flPenMM[n - 2] ) / ( shell.flPenRange[n - 1] - shell.flPenRange[n - 2] );
	return MAX( shell.flPenMM[n - 1] + flSlope * ( m - shell.flPenRange[n - 1] ), 0.3f * shell.flPenMM[n - 1] );
}

float DoDSP_HEPen( float flFillerKg )
{
	if ( flFillerKg <= 0.0f )
		return 0.0f;
	return HE_PEN_ANCHOR_MM * powf( flFillerKg / HE_PEN_ANCHOR_KG, 1.0f / 3.0f );
}

void DoDSP_HERadiiReal( float flFillerKg, float *pflKill, float *pflWound )
{
	float c = ( flFillerKg > 0.0f ) ? powf( flFillerKg / HE_ANCHOR_KG, 1.0f / 3.0f ) : 0.0f;
	*pflKill = HE_ANCHOR_KILL_M * c * DODSP_UNITS_PER_METRE;
	*pflWound = HE_ANCHOR_WOUND_M * c * DODSP_UNITS_PER_METRE;
}

void DoDSP_HERadiiPeople( float flFillerKg, float *pflKill, float *pflWound )
{
	DoDSP_HERadiiReal( flFillerKg, pflKill, pflWound );
	float s = MAX( dodsp_vehicle_he_scale.GetFloat(), 0.0f );
	*pflKill *= s;
	*pflWound *= s;
}

// 7.92 mm / .30 ball: ~6 mm of plate up close (an 8 mm halftrack side was proof against it - which is what it
// was built for); pistol / SMG ~2 mm. Ammo DOD-SP doesn't know by name: the damage tells rifle from pistol.
float DoDSP_BulletPen( int iAmmoType, float flDamage, float flDistUnits )
{
	static const struct { const char *pszName; float flMM; } s_Ammo[] =
	{
		{ "AR2", 6.0f }, { "SniperRound", 7.0f }, { "SniperPenetratedRound", 7.0f }, { "357", 4.0f },
		{ "Pistol", 2.0f }, { "SMG1", 2.0f }, { "AlyxGun", 2.0f }, { "Buckshot", 1.0f },
	};
	float flMM = ( flDamage >= 40.0f ) ? 6.0f : 2.0f;
	Ammo_t *pAmmo = GetAmmoDef()->GetAmmoOfIndex( iAmmoType );
	if ( pAmmo && pAmmo->pName )
		for ( int i = 0; i < ARRAYSIZE( s_Ammo ); i++ )
			if ( !Q_stricmp( pAmmo->pName, s_Ammo[i].pszName ) )
				flMM = s_Ammo[i].flMM;
	// full to 100 m, half by 500 m
	float m = flDistUnits / DODSP_UNITS_PER_METRE;
	float k = ( m <= 100.0f ) ? 1.0f : MAX( 0.5f, 1.0f - 0.5f * ( m - 100.0f ) / 400.0f );
	return flMM * k;
}

float DoDSP_EffectiveArmour( const ArmourPlate_t &plate, const Vector &vecPath, const Vector &vecNormal, float *pflAngle )
{
	float flCos = -DotProduct( vecPath, vecNormal );
	flCos = clamp( flCos, 0.0f, 1.0f );
	if ( pflAngle )
		*pflAngle = RAD2DEG( acosf( flCos ) );
	return plate.flMM / MAX( flCos, 0.17f );		// 80 degrees caps it at ~6x
}

bool DoDSP_RayBox( const Vector &o, const Vector &d, const Vector &mins, const Vector &maxs, float *pt, int *pAxis, int *pSign )
{
	float tNear = -FLT_MAX, tFar = FLT_MAX;
	int iNearAxis = 0, iNearSign = -1, iFarAxis = 0, iFarSign = 1;
	for ( int a = 0; a < 3; a++ )
	{
		if ( fabs( d[a] ) < 1e-6f )
		{
			if ( o[a] < mins[a] || o[a] > maxs[a] )
				return false;
			continue;
		}
		float t1 = ( mins[a] - o[a] ) / d[a], t2 = ( maxs[a] - o[a] ) / d[a];
		int s1 = -1, s2 = 1;		// entering at mins = the -a face
		if ( t1 > t2 )
		{
			float f = t1; t1 = t2; t2 = f;
			s1 = 1; s2 = -1;
		}
		if ( t1 > tNear ) { tNear = t1; iNearAxis = a; iNearSign = s1; }
		if ( t2 < tFar ) { tFar = t2; iFarAxis = a; iFarSign = s2; }
		if ( tNear > tFar || tFar < 0.0f )
			return false;
	}
	if ( tNear >= 0.0f )
	{
		*pt = tNear;
		*pAxis = iNearAxis;
		*pSign = iNearSign;
	}
	else
	{
		*pt = 0.0f;			// started inside
		*pAxis = iFarAxis;
		*pSign = iFarSign;
	}
	return true;
}

void DoDSP_ShellBlast( const Vector &vecCentre, float flFillerKg, CBaseEntity *pAttacker, CBaseEntity *pIgnore, const char *pszBigSound )
{
	if ( flFillerKg <= 0.0f )
		return;
	float c = powf( flFillerKg / HE_ANCHOR_KG, 1.0f / 3.0f );
	bool bBig = flFillerKg >= 0.1f && pszBigSound && pszBigSound[0];
	int nMagnitude = (int)clamp( 30.0f + 90.0f * c, 30.0f, 400.0f );		// the fireball's size only
	int nFlags = SF_ENVEXPLOSION_NODAMAGE | ( bBig ? SF_ENVEXPLOSION_NOSOUND : 0 );
	ExplosionCreate( vecCentre, vec3_angle, pAttacker, nMagnitude, 0, nFlags, 0.0f, pAttacker );
	if ( bBig )
	{
		CPASAttenuationFilter filter( vecCentre, SNDLVL_140dB );
		CBaseEntity::EmitSound( filter, 0, pszBigSound, &vecCentre );
	}
	float flKill, flWound;
	DoDSP_HERadiiPeople( flFillerKg, &flKill, &flWound );
	UTIL_ScreenShake( vecCentre, 4.0f + 10.0f * c, 150.0f, 1.0f, MAX( flWound * 3.0f, 256.0f ), SHAKE_START );
	if ( flWound < 8.0f )
		return;
	// damage falls off in a straight line to 0 at the wound radius (the game's RadiusDamage); 100 (a man) at the
	// kill radius
	float flCentre = 100.0f / MAX( 1.0f - flKill / flWound, 0.05f );
	CTakeDamageInfo info( pAttacker, pAttacker, flCentre, DMG_BLAST );
	info.SetDamagePosition( vecCentre );
	// a blast can set off another (a tank cooking off next to one that's hit): keep the outer one's description
	DoDSPBlast_t outer = g_DoDSPBlast;
	g_DoDSPBlast.bActive = true;
	g_DoDSPBlast.flFiller = flFillerKg;
	g_DoDSPBlast.vecCentre = vecCentre;
	RadiusDamage( info, vecCentre, flWound, CLASS_NONE, pIgnore );
	g_DoDSPBlast = outer;
}
