//====== DOD-SP =====================================================================
//
// dodsp_vehicle - armoured vehicles that drive themselves (stage V1: drive).
//
// One owner per fact:
//   - what a KIND of vehicle is (model, turret, speeds, which way the model faces, seats) lives in
//     scripts/dodsp_vehicles.txt, read here (VehicleType_t). Size comes from the model, never typed.
//   - where a vehicle can drive is decided by CorridorClear (one test, used both to grade the map's
//     node links at load and to straighten a route).
//   - how it moves is DriveThink: it plans over the AI node graph the soldiers use, steers along the
//     route (tracked vehicles pivot, wheeled ones turn like a car), sits on the ground (four corner
//     traces), and is a pusher: the engine pushes players/NPCs out of the way or calls Blocked().
//     Pushers pass through the world, so DriveThink sweeps the vehicle's own collision model ahead
//     and stops for walls itself.
//
// "forward_yaw" = which way the model faces IN GAME (0 = +X, every CoD2/CoD1 vehicle). Entity yaw =
// heading - forward_yaw; DriveThink builds the full orientation from heading + ground (BuildAngles).
// LESSON (V1 test 2026-11-19): read directions from the MDL's hull box, never from the raw mesh (.vvd): an
// animated model's mesh is in the SMD's frame and studiomdl's default 90-degree root rotation lives in
// its animation - the CoD2 meshes face -Y, the models face +X in game. V1 shipped forward_yaw -90 and
// the Panzer II graded as 256 units wide. INVARIANT (Spawn): a vehicle longer sideways than along its
// forward_yaw says so.
//
// INVARIANTs (console, "INVARIANT DOD-SP vehicle"): a vehicle that wants to move and hasn't moved 8 units
// in 3 s is stuck - it says why (world / an entity / no route) by reason code, remembers the bad link for
// 30 s and plans again; a vehicle type that doesn't load says which key is wrong.
//
//==================================================================================
#include "cbase.h"
#include "ai_network.h"
#include "ai_node.h"
#include "ai_link.h"
#include "ai_hull.h"
#include "engine/IStaticPropMgr.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "ai_basenpc.h"
#include "explode.h"
#include "IEffects.h"
#include "ammodef.h"
#include "soundenvelope.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "dodsp_factions.h"
#include "soundchars.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define DODSP_VEHICLE_SCRIPT	"scripts/dodsp_vehicles.txt"
#define VEHICLE_THINK			0.05f
#define VEHICLE_STEP			24.0f		// what it climbs without noticing (kerbs, rubble)
#define VEHICLE_MAX_STEP_PER_32	36.0f		// ground change between two samples 32 units apart (a slope of ~48 degrees)
#define VEHICLE_STUCK_TIME		3.0f
#define VEHICLE_MIN_GROUND_NZ	0.6f		// steepest ground the tracks take (normal z; ~53 degrees)
#define VEHICLE_DIRECT_DIST		768.0f		// a goal this close is driven to straight, off the road
#define VEHICLE_JOIN_NODES		6			// nodes considered for joining the road (and leaving it for the goal)
#define VEHICLE_TURN_COST		512.0f		// an about-turn on the spot counts like this many units of driving
#define VEHICLE_BADLINK_TIME	30.0f

// How far a route may cut past a road node it skips: the blend between "every node" (0) and "any shortcut the
// corridor test passes" (huge). V1d used 48 (owner: "a little too tight"); V1c had no limit ("skips most of the
// nodes"). The corridor test (trees, rocks, slopes) applies either way.
ConVar dodsp_vehicle_road_slack( "dodsp_vehicle_road_slack", "160", 0, "Units a vehicle's route may cut past a road node it skips (0 = drive every node; bigger = straighter, still only where the body fits)." );
ConVar dodsp_vehicle_debug( "dodsp_vehicle_debug", "0", 0, "1 = draw vehicle routes and print their decisions." );

enum VehicleStuck_t
{
	VSTUCK_NONE,
	VSTUCK_WORLD,		// a wall / static prop in front (its own sweep)
	VSTUCK_ENTITY,		// something it can't push (the engine's Blocked)
	VSTUCK_NOROUTE,		// no route over the node graph to the goal
	VSTUCK_NOPROGRESS,	// moving but going nowhere (wedged, sliding)
	NUM_VSTUCK
};
static const char *s_pszStuck[] = { "none", "world", "entity", "no-route", "no-progress" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszStuck ) == NUM_VSTUCK );

//-----------------------------------------------------------------------------
// Vehicle types (scripts/dodsp_vehicles.txt)
//-----------------------------------------------------------------------------
struct VehicleType_t
{
	char	szName[32];
	char	szModel[MAX_PATH];
	char	szTurretModel[MAX_PATH];
	Vector	vecTurretOffset;		// model space
	float	flForwardYaw;			// which way the model faces, in model space (CoD2 models: -90 = -Y)
	bool	bTracked;				// pivots on the spot
	float	flMaxSpeed;				// units/s
	float	flReverseSpeed;
	float	flAccel;				// units/s^2
	float	flBrake;
	float	flTurnRate;				// degrees/s (tracked: on the spot; wheeled: at speed)
	float	flMinTurnRadius;		// wheeled only
	// V2: side and weapons
	int		eFaction;				// DoDSPFaction_t ("faction" - the entity's own key wins)
	float	flTurretRate;			// degrees/s the turret traverses
	float	flPitchRate;			// degrees/s the barrel elevates
	struct Gun_t { bool bHas; float flDamage, flRadius, flReload, flRange, flSpread; char szSound[64]; };
	Gun_t	gun;					// main gun: one shell, explodes where it hits (damage falls off over radius)
	struct Coax_t { bool bHas; float flDamage, flRate, flBurst, flPause, flRange, flSpread; char szSound[64]; };
	Coax_t	coax;					// machine gun beside the main gun: bursts at infantry
	char	szEngineIdle[64], szEngineMove[64], szTurretTurn[64], szTurretStop[64], szReload[64], szExplode[64];
	struct Seat_t { Vector vecOrigin; float flYaw; };
	CUtlVector<Seat_t> seats;		// V3
};
static CUtlVector<VehicleType_t *> s_Types;
static float s_flTypesLoaded = -1.0f;

static void ParseVector( const char *psz, Vector &v )
{
	v.Init();
	if ( psz )
		sscanf( psz, "%f %f %f", &v.x, &v.y, &v.z );
}

// A type with "base" copies everything from that type; its own keys (and whole blocks) win. Returns a new
// KeyValues the caller deletes. The paint variants are only their models - one owner per number.
static KeyValues *MergedVehicleType( KeyValues *pAll, KeyValues *pType, int nDepth )
{
	const char *pszBase = pType->GetString( "base", "" );
	KeyValues *pBase = pszBase[0] ? pAll->FindKey( pszBase ) : NULL;
	if ( pszBase[0] && ( !pBase || nDepth > 4 || pBase == pType ) )
	{
		Warning( "INVARIANT DOD-SP vehicles: type '%s' has base '%s' - %s\n", pType->GetName(), pszBase,
				 !pBase ? "no such type" : "a loop of bases" );
		pBase = NULL;
	}
	KeyValues *pOut = pBase ? MergedVehicleType( pAll, pBase, nDepth + 1 ) : new KeyValues( pType->GetName() );
	pOut->SetName( pType->GetName() );
	for ( KeyValues *pKey = pType->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey() )
	{
		if ( !Q_stricmp( pKey->GetName(), "base" ) )
			continue;
		KeyValues *pOld = pOut->FindKey( pKey->GetName() );
		if ( pOld )
		{
			pOut->RemoveSubKey( pOld );
			pOld->deleteThis();
		}
		pOut->AddSubKey( pKey->MakeCopy() );
	}
	pOut->SetString( "base", "" );
	return pOut;
}

static void LoadVehicleTypes( bool bForce )
{
	if ( !bForce && s_flTypesLoaded >= 0.0f && gpGlobals->curtime >= s_flTypesLoaded )
		return;
	s_Types.PurgeAndDeleteElements();
	s_flTypesLoaded = gpGlobals->curtime;
	KeyValues *pKV = new KeyValues( "DoDSP_Vehicles" );
	if ( !pKV->LoadFromFile( filesystem, DODSP_VEHICLE_SCRIPT, "GAME" ) )
	{
		Warning( "INVARIANT DOD-SP vehicles: %s didn't load - no vehicle can spawn\n", DODSP_VEHICLE_SCRIPT );
		pKV->deleteThis();
		return;
	}
	for ( KeyValues *pRawType = pKV->GetFirstTrueSubKey(); pRawType; pRawType = pRawType->GetNextTrueSubKey() )
	{
		KeyValues *pType = MergedVehicleType( pKV, pRawType, 0 );
		VehicleType_t *t = new VehicleType_t;
		Q_strncpy( t->szName, pType->GetName(), sizeof( t->szName ) );
		Q_strncpy( t->szModel, pType->GetString( "model" ), sizeof( t->szModel ) );
		Q_strncpy( t->szTurretModel, pType->GetString( "turret_model" ), sizeof( t->szTurretModel ) );
		ParseVector( pType->GetString( "turret_offset", "0 0 0" ), t->vecTurretOffset );
		t->flForwardYaw = pType->GetFloat( "forward_yaw", 0.0f );
		t->bTracked = pType->GetBool( "tracked", true );
		t->flMaxSpeed = pType->GetFloat( "max_speed", 160.0f );
		t->flReverseSpeed = pType->GetFloat( "reverse_speed", 60.0f );
		t->flAccel = pType->GetFloat( "accel", 60.0f );
		t->flBrake = pType->GetFloat( "brake", 150.0f );
		t->flTurnRate = pType->GetFloat( "turn_rate", 30.0f );
		t->flMinTurnRadius = pType->GetFloat( "min_turn_radius", 300.0f );
		t->eFaction = DoDSP_FactionFromName( pType->GetString( "faction", "axis" ) );
		if ( t->eFaction < 0 )
		{
			Warning( "INVARIANT DOD-SP vehicles: type '%s' has faction '%s' - not a side (axis / allies) - made axis\n", t->szName, pType->GetString( "faction" ) );
			t->eFaction = DODSP_FACTION_AXIS;
		}
		t->flTurretRate = pType->GetFloat( "turret_rate", 30.0f );
		t->flPitchRate = pType->GetFloat( "pitch_rate", 15.0f );
		KeyValues *pGun = pType->FindKey( "gun" );
		t->gun.bHas = pGun != NULL;
		t->gun.flDamage = pGun ? pGun->GetFloat( "damage", 600.0f ) : 0.0f;
		t->gun.flRadius = pGun ? pGun->GetFloat( "radius", 256.0f ) : 0.0f;
		t->gun.flReload = pGun ? pGun->GetFloat( "reload", 5.0f ) : 0.0f;
		t->gun.flRange = pGun ? pGun->GetFloat( "range", 6000.0f ) : 0.0f;
		t->gun.flSpread = pGun ? pGun->GetFloat( "spread", 0.5f ) : 0.0f;
		Q_strncpy( t->gun.szSound, pGun ? pGun->GetString( "sound", "DoDSP_Vehicle.GunFire1" ) : "", sizeof( t->gun.szSound ) );
		KeyValues *pCoax = pType->FindKey( "coax" );
		t->coax.bHas = pCoax != NULL;
		t->coax.flDamage = pCoax ? pCoax->GetFloat( "damage", 12.0f ) : 0.0f;
		t->coax.flRate = pCoax ? pCoax->GetFloat( "rate", 10.0f ) : 0.0f;
		t->coax.flBurst = pCoax ? pCoax->GetFloat( "burst", 1.5f ) : 0.0f;
		t->coax.flPause = pCoax ? pCoax->GetFloat( "pause", 1.2f ) : 0.0f;
		t->coax.flRange = pCoax ? pCoax->GetFloat( "range", 2500.0f ) : 0.0f;
		t->coax.flSpread = pCoax ? pCoax->GetFloat( "spread", 3.0f ) : 0.0f;
		Q_strncpy( t->coax.szSound, pCoax ? pCoax->GetString( "sound", "Weapon_Mg42.Shoot" ) : "", sizeof( t->coax.szSound ) );
		KeyValues *pSnd = pType->FindKey( "sounds" );
		Q_strncpy( t->szEngineIdle, pSnd ? pSnd->GetString( "engine_idle" ) : "", sizeof( t->szEngineIdle ) );
		Q_strncpy( t->szEngineMove, pSnd ? pSnd->GetString( "engine_move" ) : "", sizeof( t->szEngineMove ) );
		Q_strncpy( t->szTurretTurn, pSnd ? pSnd->GetString( "turret_turn" ) : "", sizeof( t->szTurretTurn ) );
		Q_strncpy( t->szTurretStop, pSnd ? pSnd->GetString( "turret_stop" ) : "", sizeof( t->szTurretStop ) );
		Q_strncpy( t->szReload, pSnd ? pSnd->GetString( "reload" ) : "", sizeof( t->szReload ) );
		Q_strncpy( t->szExplode, pSnd ? pSnd->GetString( "explode", "DoDSP_Vehicle.ShellExplode" ) : "DoDSP_Vehicle.ShellExplode", sizeof( t->szExplode ) );
		KeyValues *pSeats = pType->FindKey( "seats" );
		for ( KeyValues *pSeat = pSeats ? pSeats->GetFirstTrueSubKey() : NULL; pSeat; pSeat = pSeat->GetNextTrueSubKey() )
		{
			VehicleType_t::Seat_t seat;
			ParseVector( pSeat->GetString( "origin" ), seat.vecOrigin );
			seat.flYaw = pSeat->GetFloat( "yaw" );
			t->seats.AddToTail( seat );
		}
		if ( !t->szModel[0] )
		{
			Warning( "INVARIANT DOD-SP vehicles: type '%s' in %s has no \"model\" - skipped\n", t->szName, DODSP_VEHICLE_SCRIPT );
			delete t;
			pType->deleteThis();
			continue;
		}
		pType->deleteThis();
		s_Types.AddToTail( t );
	}
	pKV->deleteThis();
}

static VehicleType_t *FindVehicleType( const char *pszName )
{
	LoadVehicleTypes( false );
	for ( int i = 0; i < s_Types.Count(); i++ )
		if ( !Q_stricmp( s_Types[i]->szName, pszName ) )
			return s_Types[i];
	return NULL;
}

//-----------------------------------------------------------------------------
// Where a vehicle fits: world + static props + brush entities, never people or loose props
//-----------------------------------------------------------------------------
class CTraceFilterVehicleWorld : public CTraceFilter
{
public:
	CTraceFilterVehicleWorld( CBaseEntity *pSelf ) : m_pSelf( pSelf ) {}
	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask )
	{
		if ( staticpropmgr->IsStaticProp( pHandleEntity ) )
			return true;
		CBaseEntity *pEnt = EntityFromEntityHandle( pHandleEntity );
		if ( !pEnt || pEnt == m_pSelf || ( m_pSelf && pEnt->GetRootMoveParent() == m_pSelf ) )
			return false;
		return pEnt->IsWorld() || pEnt->IsBSPModel();
	}
	virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
private:
	CBaseEntity *m_pSelf;
};

// THE rule for "can it drive on this surface" (one owner): CorridorClear grades ground with it and the
// drive sweep uses it to tell ground it climbs from a wall. V1b test (gm_countrysidedrive_d): the sweep
// called rising terrain a wall that CorridorClear had passed, and the tank stopped on a route it was given.
static inline bool VehicleDrivable( float flNormalZ )
{
	return flNormalZ >= VEHICLE_MIN_GROUND_NZ;
}

static bool GroundAt( const Vector &vecPos, float flUp, float flDown, float *pflZ, float *pflNormalZ, CBaseEntity *pSelf )
{
	CTraceFilterVehicleWorld filter( pSelf );
	trace_t tr;
	UTIL_TraceLine( vecPos + Vector( 0, 0, flUp ), vecPos - Vector( 0, 0, flDown ), MASK_SOLID, &filter, &tr );
	if ( tr.fraction >= 1.0f || tr.startsolid )
		return false;
	*pflZ = tr.endpos.z;
	if ( pflNormalZ )
		*pflNormalZ = tr.plane.normal.z;
	return true;
}

// What a trace hit, in words: static props come back as the world entity - say so (a tree is not a wall)
static const char *VehicleHitName( const trace_t &tr )
{
	if ( tr.surface.name && Q_stristr( tr.surface.name, "studio" ) )
		return "static prop";
	return tr.m_pEnt ? tr.m_pEnt->GetClassname() : "world";
}

// Why a stretch isn't drivable (reason codes; the debug line prints the name)
enum CorridorFail_t
{
	CFAIL_NONE,
	CFAIL_NO_GROUND,		// nothing under it within 128 units (a drop, a hole) or it's inside something
	CFAIL_TOO_STEEP,		// ground steeper than VehicleDrivable
	CFAIL_STEP,				// the ground jumps more than VEHICLE_MAX_STEP_PER_32 between two samples
	CFAIL_BLOCKED,			// something in the body's way (wall, tree, rock, a building's edge)
	NUM_CFAILS
};
static const char *s_pszCorridorFail[] = { "clear", "no ground", "too steep", "step too high", "blocked" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszCorridorFail ) == NUM_CFAILS );

struct CorridorWhy_t
{
	int		eFail;			// CorridorFail_t
	Vector	vecAt;			// where (the column's ground)
	char	szWhat[32];		// what it hit (CFAIL_BLOCKED)
};

// What stops the body: a box swept from a to b. Ground it can climb (VehicleDrivable) doesn't stop it - look
// again 48 units higher, so a wall behind the slope is still found. Starting inside something blocks.
static bool VehicleBoxBlocked( const Vector &vecMins, const Vector &vecMaxs, const Vector &a, const Vector &b, ITraceFilter *pFilter, trace_t *ptr )
{
	for ( int k = 0; k < 2; k++ )
	{
		Vector vecUp( 0, 0, k ? 48.0f : 0.0f );
		UTIL_TraceHull( a + vecUp, b + vecUp, vecMins, vecMaxs, MASK_SOLID, pFilter, ptr );
		if ( ptr->startsolid )
			return k == 0;
		if ( ptr->fraction >= 1.0f )
			return false;
		if ( !VehicleDrivable( ptr->plane.normal.z ) )
			return true;
	}
	return false;
}

#define VEHICLE_BODY_COLUMN		16.0f		// half-size of the boxes CorridorClear lines up across the body

// THE test of whether a vehicle can drive straight from a to b (one owner - planning, grading the node
// graph AND, every think, the stretch just ahead of the moving vehicle use it, so what it plans and what it
// drives can't disagree). Its BODY - half-width x height, from step height up to its top - must fit all the
// way, and every column of it must have ground it can drive on: boxes side by side across its width (each
// VEHICLE_BODY_COLUMN wide), every 32 units, each riding its own ground. Finds a tree trunk anywhere across the
// width, a branch at hull height, a bump under one track.
// History: V1c used thin lines (missed a tree); V1d's drive check swept the vehicle's rigid model and called
// a terrain berm a wall (hull traces on displacements return flat normals: "normal z 0.00") although the
// plan had passed it - two tests, two answers. Now one test.
// The first sample (a) never fails: the vehicle (or node) is already standing there.
static bool CorridorClear( const Vector &a, const Vector &b, float flHalfWidth, float flHeight, CBaseEntity *pSelf, CorridorWhy_t *pWhy = NULL )
{
	if ( pWhy )
	{
		pWhy->eFail = CFAIL_NONE;
		pWhy->vecAt = a;
		pWhy->szWhat[0] = 0;
	}
	Vector vecDir = b - a;
	vecDir.z = 0.0f;
	float flLen = VectorNormalize( vecDir );
	if ( flLen < 1.0f )
		return true;
	Vector vecRight( vecDir.y, -vecDir.x, 0.0f );
	float flCol = MIN( VEHICLE_BODY_COLUMN, flHalfWidth );
	int nCols = 1 + (int)ceilf( ( 2.0f * ( flHalfWidth - flCol ) ) / ( 2.0f * flCol ) - 0.001f );
	nCols = clamp( nCols, 1, 16 );
	float flBodyUp = MAX( flHeight - VEHICLE_STEP, 8.0f );
	Vector vecMins( -flCol, -flCol, 0.0f ), vecMaxs( flCol, flCol, flBodyUp );
	int nSteps = (int)ceilf( flLen / 32.0f );
	CTraceFilterVehicleWorld filter( pSelf );
	float flPrevCenterZ = a.z;
	float flPrevZ[16];
	Vector vecPrevCol[16];
#define CORRIDOR_FAIL( e, at ) { if ( pWhy ) { pWhy->eFail = (e); pWhy->vecAt = (at); } return false; }
	for ( int i = 0; i <= nSteps; i++ )
	{
		Vector p = a + vecDir * ( flLen * i / nSteps );
		float flCenterZ, flNZ;
		// the centre line finds the ground first (from up to 64 above the last ground: hills on the way)
		p.z = flPrevCenterZ;
		bool bGround = GroundAt( p, 64.0f, 128.0f, &flCenterZ, &flNZ, pSelf );
		if ( i == 0 )
		{
			if ( !bGround )
				flCenterZ = a.z;
		}
		else
		{
			if ( !bGround )
				CORRIDOR_FAIL( CFAIL_NO_GROUND, p );
			if ( !VehicleDrivable( flNZ ) )
				CORRIDOR_FAIL( CFAIL_TOO_STEEP, Vector( p.x, p.y, flCenterZ ) );
			if ( fabs( flCenterZ - flPrevCenterZ ) > VEHICLE_MAX_STEP_PER_32 )
				CORRIDOR_FAIL( CFAIL_STEP, Vector( p.x, p.y, flCenterZ ) );
		}
		for ( int c = 0; c < nCols; c++ )
		{
			float flSide = ( nCols == 1 ) ? 0.0f : -( flHalfWidth - flCol ) + c * ( 2.0f * ( flHalfWidth - flCol ) / ( nCols - 1 ) );
			Vector vecCol = p + vecRight * flSide;
			vecCol.z = flCenterZ;
			float flZ;
			bool bColGround = GroundAt( vecCol, 64.0f, 128.0f, &flZ, &flNZ, pSelf );
			if ( i == 0 )
			{
				if ( !bColGround )
					flZ = flCenterZ;
			}
			else
			{
				if ( !bColGround )
					CORRIDOR_FAIL( CFAIL_NO_GROUND, vecCol );
				if ( !VehicleDrivable( flNZ ) )
					CORRIDOR_FAIL( CFAIL_TOO_STEEP, Vector( vecCol.x, vecCol.y, flZ ) );
				if ( fabs( flZ - flPrevZ[c] ) > VEHICLE_MAX_STEP_PER_32 )
					CORRIDOR_FAIL( CFAIL_STEP, Vector( vecCol.x, vecCol.y, flZ ) );
			}
			vecCol.z = flZ + VEHICLE_STEP;
			if ( i > 0 )
			{
				trace_t tr;
				if ( VehicleBoxBlocked( vecMins, vecMaxs, vecPrevCol[c], vecCol, &filter, &tr ) )
				{
					if ( pWhy )
						Q_strncpy( pWhy->szWhat, VehicleHitName( tr ), sizeof( pWhy->szWhat ) );
					CORRIDOR_FAIL( CFAIL_BLOCKED, tr.endpos );
				}
			}
			flPrevZ[c] = flZ;
			vecPrevCol[c] = vecCol;
		}
		flPrevCenterZ = flCenterZ;
	}
#undef CORRIDOR_FAIL
	return true;
}

//-----------------------------------------------------------------------------
// Sounds (V2): the owner's tanksounds in scripts/game_sounds_dodsp_vehicles.txt, loaded by the game (the sound
// system drops override scripts at level end, so "is it loaded" is asked of the sound system every time).
// Once per map: every sound any vehicle type names must exist as an entry AND every wave behind it as a file -
// ONE console line either way (the chatter's rule: the owner sees at load what's missing, not in a fight).
//-----------------------------------------------------------------------------
extern ISoundEmitterSystemBase *soundemitterbase;
#define DODSP_VEHICLE_SOUND_SCRIPT	"scripts/game_sounds_dodsp_vehicles.txt"

static bool VehicleSoundEntryExists( const char *pszName )
{
	return soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( pszName ) );
}

// 0 = fine, 1 = no such entry, 2 = a wave file behind it is missing (*pszWave says which)
static int VehicleSoundCheck( const char *pszName, char *pszWave, int nWave )
{
	int i = soundemitterbase->GetSoundIndex( pszName );
	if ( !soundemitterbase->IsValidIndex( i ) )
		return 1;
	CSoundParametersInternal *pParams = soundemitterbase->InternalGetParametersForSound( i );
	if ( !pParams )
		return 1;
	const SoundFile *pFiles = pParams->GetSoundNames();
	for ( int w = 0; w < pParams->NumSoundNames(); w++ )
	{
		const char *pszW = soundemitterbase->GetWaveName( pFiles[w].symbol );
		if ( !pszW || !pszW[0] )
			continue;
		char szPath[MAX_PATH];
		Q_snprintf( szPath, sizeof( szPath ), "sound/%s", PSkipSoundChars( pszW ) );
		if ( !filesystem->FileExists( szPath, "GAME" ) )
		{
			Q_strncpy( pszWave, szPath, nWave );
			return 2;
		}
	}
	return 0;
}

static void VehicleSoundsLevelInit( void )
{
	if ( !VehicleSoundEntryExists( "DoDSP_Vehicle.ShellExplode" ) )
		soundemitterbase->AddSoundOverrides( DODSP_VEHICLE_SOUND_SCRIPT, true );
	static char s_szCheckedMap[64] = "";
	if ( !Q_strcmp( s_szCheckedMap, STRING( gpGlobals->mapname ) ) )
		return;
	Q_strncpy( s_szCheckedMap, STRING( gpGlobals->mapname ), sizeof( s_szCheckedMap ) );
	int nNames = 0, nNoEntry = 0, nNoWave = 0;
	char szFirst[MAX_PATH] = "";
	for ( int t = 0; t < s_Types.Count(); t++ )
	{
		VehicleType_t *pT = s_Types[t];
		const char *pszNames[] = { pT->gun.szSound, pT->coax.szSound, pT->szEngineIdle, pT->szEngineMove, pT->szTurretTurn,
								   pT->szTurretStop, pT->szReload, pT->szExplode };
		for ( int k = 0; k < ARRAYSIZE( pszNames ); k++ )
		{
			if ( !pszNames[k][0] )
				continue;
			nNames++;
			char szWave[MAX_PATH] = "";
			int eWhy = VehicleSoundCheck( pszNames[k], szWave, sizeof( szWave ) );
			if ( eWhy == 1 )
			{
				if ( !nNoEntry && !nNoWave )
					Q_snprintf( szFirst, sizeof( szFirst ), "%s: no sound entry '%s'", pT->szName, pszNames[k] );
				nNoEntry++;
			}
			else if ( eWhy == 2 )
			{
				if ( !nNoEntry && !nNoWave )
					Q_snprintf( szFirst, sizeof( szFirst ), "%s: '%s' -> %s not found", pT->szName, pszNames[k], szWave );
				nNoWave++;
			}
		}
	}
	if ( nNoEntry || nNoWave )
		Warning( "INVARIANT DOD-SP vehicle sounds: %d of %d missing (%d no entry, %d wave file missing) - first: %s. Entries live in %s, waves in DOD-SP\\sound\\dodsp\\vehicles\\\n",
				 nNoEntry + nNoWave, nNames, nNoEntry, nNoWave, szFirst, DODSP_VEHICLE_SOUND_SCRIPT );
	else
		Msg( "DOD-SP vehicle sounds: all %d found\n", nNames );
}

//-----------------------------------------------------------------------------
// The node graph, graded per vehicle width (built once per map per width, printed once)
//-----------------------------------------------------------------------------
struct VehicleNav_t
{
	int		iHalfWidth;
	int		iHeight;				// the body's height (CorridorClear tests the whole body)
	char	szMap[64];
	int		nNodes;
	int		nGraphLinks;
	CUtlVector< CUtlVector<int> > links;	// per node: the nodes a vehicle this wide can drive to
};
static CUtlVector<VehicleNav_t *> s_Navs;

static bool NodeUsable( CAI_Node *pNode )
{
	return pNode && pNode->GetType() == NODE_GROUND;
}

static VehicleNav_t *GetVehicleNav( float flHalfWidth, float flHeight, const char *pszTypeName )
{
	if ( !g_pBigAINet )
		return NULL;
	int iHalf = (int)( ceilf( flHalfWidth / 8.0f ) * 8.0f );
	int iHeight = (int)( ceilf( flHeight / 8.0f ) * 8.0f );
	const char *pszMap = STRING( gpGlobals->mapname );
	// the graph as it is now: a grading is for one map, node count and link count (the engine builds or
	// rebuilds the links AFTER entities activate - V1 graded the unbuilt graph: "0 of 0 node links")
	int nGraphLinks = 0;
	for ( int i = 0; i < g_pBigAINet->NumNodes(); i++ )
		nGraphLinks += g_pBigAINet->GetNode( i, false )->NumLinks();
	for ( int i = 0; i < s_Navs.Count(); i++ )
	{
		VehicleNav_t *pNav = s_Navs[i];
		if ( pNav->iHalfWidth == iHalf && pNav->iHeight == iHeight && pNav->nNodes == g_pBigAINet->NumNodes() && pNav->nGraphLinks == nGraphLinks && !Q_strcmp( pNav->szMap, pszMap ) )
			return pNav;
	}
	// a new map or a rebuilt graph: forget the old grading
	for ( int i = s_Navs.Count() - 1; i >= 0; i-- )
	{
		if ( Q_strcmp( s_Navs[i]->szMap, pszMap ) || s_Navs[i]->nNodes != g_pBigAINet->NumNodes() || s_Navs[i]->nGraphLinks != nGraphLinks )
		{
			delete s_Navs[i];
			s_Navs.Remove( i );
		}
	}
	double flStart = Plat_FloatTime();
	VehicleNav_t *pNav = new VehicleNav_t;
	pNav->iHalfWidth = iHalf;
	pNav->iHeight = iHeight;
	Q_strncpy( pNav->szMap, pszMap, sizeof( pNav->szMap ) );
	pNav->nNodes = g_pBigAINet->NumNodes();
	pNav->nGraphLinks = nGraphLinks;
	pNav->links.SetCount( pNav->nNodes );
	int nLinks = 0, nOK = 0, nNodesReach = 0;
	for ( int i = 0; i < pNav->nNodes; i++ )
	{
		CAI_Node *pNode = g_pBigAINet->GetNode( i, false );
		if ( !NodeUsable( pNode ) )
			continue;
		for ( int l = 0; l < pNode->NumLinks(); l++ )
		{
			CAI_Link *pLink = pNode->GetLinkByIndex( l );
			if ( !pLink || pLink->m_iSrcID != i )
				continue;		// each link once (it's listed on both of its nodes)
			int j = pLink->DestNodeID( i );
			CAI_Node *pOther = g_pBigAINet->GetNode( j, false );
			if ( !NodeUsable( pOther ) )
				continue;
			nLinks++;
			if ( !CorridorClear( pNode->GetOrigin(), pOther->GetOrigin(), (float)iHalf, (float)iHeight, NULL ) )
				continue;
			nOK++;
			pNav->links[i].AddToTail( j );
			pNav->links[j].AddToTail( i );
		}
	}
	for ( int i = 0; i < pNav->nNodes; i++ )
		if ( pNav->links[i].Count() )
			nNodesReach++;
	s_Navs.AddToTail( pNav );
	Msg( "DOD-SP vehicles: %s (%d units wide, %d high, rounded up) can drive %d of %d node links (%d of %d nodes) on %s - graded in %.2f s\n",
		 pszTypeName, iHalf * 2, iHeight, nOK, nLinks, nNodesReach, pNav->nNodes, pszMap, Plat_FloatTime() - flStart );
	if ( pNav->nNodes > 1 && nLinks == 0 )
		Warning( "INVARIANT DOD-SP vehicles: %s has %d nodes but no links between them yet (graph not built, or nodes out of each other's sight)\n", pszMap, pNav->nNodes );
	if ( nLinks > 0 && nOK == 0 )
		Warning( "INVARIANT DOD-SP vehicles: no node link on %s is wide enough for %s - it can only drive straight to a goal in plain view\n", pszMap, pszTypeName );
	return pNav;
}

//-----------------------------------------------------------------------------
// The vehicle
//-----------------------------------------------------------------------------
class CDoDSPVehicle : public CBaseAnimating
{
public:
	DECLARE_CLASS( CDoDSPVehicle, CBaseAnimating );
	DECLARE_DATADESC();
	DECLARE_SERVERCLASS();

	CDoDSPVehicle();
	virtual void	Precache( void );
	virtual void	Spawn( void );
	virtual void	Activate( void );
	virtual void	OnRestore( void );
	virtual void	UpdateOnRemove( void );
	virtual void	Blocked( CBaseEntity *pOther );
	virtual int		DrawDebugTextOverlays( void );
	virtual Class_T	Classify( void ) { return DoDSP_ClassForFaction( m_eFaction ); }	// one owner: dodsp_factions
	int				GetFaction( void ) const { return m_eFaction; }

	void			DriveThink( void );
	bool			DriveTo( const Vector &vecGoal );
	void			Stop( void );
	void			Report( void );
	void			SetTurretOffset( const Vector &vecOffset );

	void			InputDriveTo( inputdata_t &inputdata );
	void			InputStop( inputdata_t &inputdata );
	void			InputSetMaxSpeed( inputdata_t &inputdata );
	void			InputSetTarget( inputdata_t &inputdata );
	void			InputClearTarget( inputdata_t &inputdata );
	void			InputHoldFire( inputdata_t &inputdata );
	void			InputOpenFire( inputdata_t &inputdata );

	static CUtlVector<CDoDSPVehicle *> s_All;

private:
	bool			PlanRoute( void );
	int				NearestNode( const Vector &vecPos, bool bFromHere );
	int				NodeCandidates( const Vector &vecPos, bool bFromHere, int nMax, CUtlVector<int> &out );
	float			TurnCost( const Vector &vecTo ) const;
	void			SetStuck( VehicleStuck_t eWhy, CBaseEntity *pWho );
	QAngle			BuildAngles( float flHeading, const Vector &vecPos, float *pflGroundZ );
	void			MeasureBody( void );
	float			HeadingFromAngles( void ) const;
	// V2 weapons
	void			WeaponThink( float dt );
	bool			CalibrateTurret( void );
	CBaseEntity		*FindTarget( void );
	bool			TargetValid( CBaseEntity *pTarget, float flRange, bool bNeedSight );
	bool			CanSee( CBaseEntity *pTarget );
	Vector			AimPivot( void );
	Vector			BarrelDir( void );
	Vector			MuzzlePos( const Vector &vecDir );
	bool			FriendlyInTheWay( const Vector &vecFrom, const Vector &vecDir, float flRange, float flRadius, CBaseEntity **ppWho );
	void			FireMainGun( const Vector &vecDir );
	void			FireCoax( const Vector &vecDir );
	void			UpdateSounds( float flTurretRate );
	void			StopSounds( void );

	string_t		m_iszVehicleType;
	string_t		m_iszInitialGoal;
	VehicleType_t	*m_pType;
	float			m_flHalfWidth, m_flHalfLength, m_flBodyHeight;	// from the model (MeasureBody), not saved
	EHANDLE			m_hTurret;

	CNetworkVar( float, m_flTrackSpeed );		// signed, units/s (client: wheel spin, track scroll)
	CNetworkVar( float, m_flModelForwardYaw );

	float			m_flSpeed;
	float			m_flHeading;			// degrees, world yaw of the vehicle's front
	float			m_flMaxSpeedScale;
	bool			m_bHasGoal;
	Vector			m_vecGoal;
	CUtlVector<Vector> m_Route;
	int				m_iRoute;

	// stuck watchdog + refusal memory
	Vector			m_vecProgressPos;
	float			m_flProgressHeading;
	float			m_flProgressTime;
	bool			m_bInitialGoalDone;
	float			m_flNavCheckTime;			// >0: grade the graph (and drive to the initial goal) at this time
	VehicleStuck_t	m_eStuck;
	EHANDLE			m_hBlocker;
	float			m_flBlockedTime;
	struct BadLink_t { int a, b; float flUntil; };
	CUtlVector<BadLink_t> m_BadLinks;
	int				m_iLastFrom, m_iLastTo;		// the node link it was driving when stuck

	// V2: side, target, turret
	string_t		m_iszFaction;				// Hammer key "faction" (empty = the type's)
	int				m_eFaction;
	EHANDLE			m_hTarget;
	EHANDLE			m_hForcedTarget;			// SetTarget input
	bool			m_bHoldFire;
	float			m_flNextScan;
	float			m_flTurretYaw;				// local to the hull, degrees (turret model's own front = 0)
	float			m_flTurretPitch;			// degrees up
	float			m_flNextGun;
	float			m_flNextCoaxShot, m_flCoaxBurstEnd, m_flCoaxPauseUntil;
	bool			m_bReloadSoundDone;
	// measured from the turret model (CalibrateTurret), not saved
	bool			m_bTurretCalibrated;
	float			m_flTurretFwdOffset;		// where the barrel points in the turret model at yaw 0
	float			m_flPitchSign;				// barrel_pitch value per degree up (+1 or -1)
	float			m_flPoseMin, m_flPoseMax;
	int				m_iMuzzleAttach, m_iPitchPose;
	// sounds (not saved: made again after a load)
	CSoundPatch		*m_pEngineIdle, *m_pEngineMove, *m_pTurretTurn;
	bool			m_bTurretTurning;
	COutputEvent	m_OnFire;
	COutputEvent	m_OnTargetSeen;

	COutputEvent	m_OnArrived;
	COutputEvent	m_OnStuck;
	COutputEvent	m_OnNoRoute;
};

CUtlVector<CDoDSPVehicle *> CDoDSPVehicle::s_All;

LINK_ENTITY_TO_CLASS( dodsp_vehicle, CDoDSPVehicle );

BEGIN_DATADESC( CDoDSPVehicle )
	DEFINE_KEYFIELD( m_iszVehicleType, FIELD_STRING, "vehicletype" ),
	DEFINE_KEYFIELD( m_iszInitialGoal, FIELD_STRING, "goal" ),
	DEFINE_FIELD( m_hTurret, FIELD_EHANDLE ),
	DEFINE_FIELD( m_flSpeed, FIELD_FLOAT ),
	DEFINE_FIELD( m_flHeading, FIELD_FLOAT ),
	DEFINE_FIELD( m_flMaxSpeedScale, FIELD_FLOAT ),
	DEFINE_FIELD( m_bHasGoal, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_vecGoal, FIELD_POSITION_VECTOR ),
	DEFINE_FIELD( m_bInitialGoalDone, FIELD_BOOLEAN ),
	DEFINE_KEYFIELD( m_iszFaction, FIELD_STRING, "faction" ),
	DEFINE_FIELD( m_eFaction, FIELD_INTEGER ),
	DEFINE_FIELD( m_hForcedTarget, FIELD_EHANDLE ),
	DEFINE_FIELD( m_bHoldFire, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_flTurretYaw, FIELD_FLOAT ),
	DEFINE_FIELD( m_flTurretPitch, FIELD_FLOAT ),
	DEFINE_FIELD( m_flNextGun, FIELD_TIME ),
	DEFINE_INPUTFUNC( FIELD_STRING, "SetTarget", InputSetTarget ),
	DEFINE_INPUTFUNC( FIELD_VOID, "ClearTarget", InputClearTarget ),
	DEFINE_INPUTFUNC( FIELD_VOID, "HoldFire", InputHoldFire ),
	DEFINE_INPUTFUNC( FIELD_VOID, "OpenFire", InputOpenFire ),
	DEFINE_OUTPUT( m_OnFire, "OnFire" ),
	DEFINE_OUTPUT( m_OnTargetSeen, "OnTargetSeen" ),
	DEFINE_FUNCTION( DriveThink ),
	DEFINE_INPUTFUNC( FIELD_STRING, "DriveTo", InputDriveTo ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Stop", InputStop ),
	DEFINE_INPUTFUNC( FIELD_FLOAT, "SetMaxSpeed", InputSetMaxSpeed ),
	DEFINE_OUTPUT( m_OnArrived, "OnArrived" ),
	DEFINE_OUTPUT( m_OnStuck, "OnStuck" ),
	DEFINE_OUTPUT( m_OnNoRoute, "OnNoRoute" ),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST( CDoDSPVehicle, DT_DoDSPVehicle )
	SendPropFloat( SENDINFO( m_flTrackSpeed ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flModelForwardYaw ), 0, SPROP_NOSCALE ),
END_SEND_TABLE()

CDoDSPVehicle::CDoDSPVehicle()
{
	m_pType = NULL;
	m_flSpeed = 0.0f;
	m_flHeading = 0.0f;
	m_flMaxSpeedScale = 1.0f;
	m_bHasGoal = false;
	m_iRoute = 0;
	m_eStuck = VSTUCK_NONE;
	m_flBlockedTime = 0.0f;
	m_flProgressTime = 0.0f;
	m_flProgressHeading = 0.0f;
	m_bInitialGoalDone = false;
	m_flNavCheckTime = 0.0f;
	m_flHalfWidth = m_flHalfLength = m_flBodyHeight = 0.0f;
	m_eFaction = DODSP_FACTION_AXIS;
	m_bHoldFire = false;
	m_flNextScan = 0.0f;
	m_flTurretYaw = m_flTurretPitch = 0.0f;
	m_flNextGun = 0.0f;
	m_flNextCoaxShot = m_flCoaxBurstEnd = m_flCoaxPauseUntil = 0.0f;
	m_bReloadSoundDone = true;
	m_bTurretCalibrated = false;
	m_flTurretFwdOffset = 0.0f;
	m_flPitchSign = 1.0f;
	m_flPoseMin = m_flPoseMax = 0.0f;
	m_iMuzzleAttach = m_iPitchPose = -1;
	m_pEngineIdle = m_pEngineMove = m_pTurretTurn = NULL;
	m_bTurretTurning = false;
	m_iLastFrom = m_iLastTo = -1;
	s_All.AddToTail( this );
}

void CDoDSPVehicle::UpdateOnRemove( void )
{
	s_All.FindAndRemove( this );
	StopSounds();
	if ( m_hTurret.Get() )
		UTIL_Remove( m_hTurret.Get() );
	BaseClass::UpdateOnRemove();
}

void CDoDSPVehicle::Precache( void )
{
	m_pType = FindVehicleType( STRING( m_iszVehicleType ) );
	if ( m_pType )
	{
		PrecacheModel( m_pType->szModel );
		if ( m_pType->szTurretModel[0] )
			PrecacheModel( m_pType->szTurretModel );
		VehicleSoundsLevelInit();
		const char *pszNames[] = { m_pType->gun.szSound, m_pType->coax.szSound, m_pType->szEngineIdle, m_pType->szEngineMove,
								   m_pType->szTurretTurn, m_pType->szTurretStop, m_pType->szReload, m_pType->szExplode };
		for ( int k = 0; k < ARRAYSIZE( pszNames ); k++ )
			if ( pszNames[k][0] )
				PrecacheScriptSound( pszNames[k] );
	}
	BaseClass::Precache();
}

void CDoDSPVehicle::Spawn( void )
{
	Precache();
	if ( !m_pType )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: vehicletype '%s' is not in %s - removed\n", GetDebugName(), STRING( m_iszVehicleType ), DODSP_VEHICLE_SCRIPT );
		UTIL_Remove( this );
		return;
	}
	SetModel( m_pType->szModel );
	m_flModelForwardYaw = m_pType->flForwardYaw;

	MeasureBody();

	// its side: the Hammer key wins, else the type's (a captured tank is a key away)
	m_eFaction = m_pType->eFaction;
	if ( m_iszFaction != NULL_STRING && STRING( m_iszFaction )[0] )
	{
		int e = DoDSP_FactionFromName( STRING( m_iszFaction ) );
		if ( e >= 0 )
			m_eFaction = e;
		else
			Warning( "INVARIANT DOD-SP vehicle %s: faction '%s' is not a side (axis / allies) - kept %s\n", GetDebugName(), STRING( m_iszFaction ), DoDSP_FactionName( m_eFaction ) );
	}
	// guard: what it answers to Classify must map back to its side (dodsp_factions is the one owner)
	if ( DoDSP_FactionOf( this ) != m_eFaction )
		Warning( "INVARIANT DOD-SP vehicle %s: side %s but its class counts as %s (dodsp_factions.cpp tables disagree)\n",
				 GetDebugName(), DoDSP_FactionName( m_eFaction ), DoDSP_FactionName( DoDSP_FactionOf( this ) ) );

	SetSolid( SOLID_VPHYSICS );
	SetMoveType( MOVETYPE_PUSH );
	m_takedamage = DAMAGE_NO;	// V4
	if ( !VPhysicsInitShadow( false, false ) )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: %s has no physics model (.phy) - it can't push or be hit\n", GetDebugName(), m_pType->szModel );
		SetSolid( SOLID_BBOX );
	}

	// heading from the map's angles (the mapper turned the model the way it should face)
	m_flHeading = HeadingFromAngles();
	float flZ;
	SetAbsAngles( BuildAngles( m_flHeading, GetAbsOrigin(), &flZ ) );

	if ( m_pType->szTurretModel[0] )
	{
		CBaseAnimating *pTurret = static_cast<CBaseAnimating *>( CreateEntityByName( "prop_dynamic_override" ) );
		if ( pTurret )
		{
			pTurret->KeyValue( "model", m_pType->szTurretModel );
			pTurret->KeyValue( "solid", "0" );
			DispatchSpawn( pTurret );
			pTurret->SetParent( this );
			pTurret->SetLocalOrigin( m_pType->vecTurretOffset );
			pTurret->SetLocalAngles( vec3_angle );
			m_hTurret = pTurret;
		}
	}
	m_vecProgressPos = GetAbsOrigin();
	m_flProgressTime = gpGlobals->curtime;
	SetThink( &CDoDSPVehicle::DriveThink );
	SetNextThink( gpGlobals->curtime + VEHICLE_THINK );
	SetMoveDoneTime( 1.0f );
}

// The body's size from the model, in the vehicle's own frame (forward / sideways / up) - one owner, called on
// Spawn AND after a save is loaded (the sizes aren't saved; before this they were 0 after a load).
void CDoDSPVehicle::MeasureBody( void )
{
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	float flYawRad = DEG2RAD( m_pType->flForwardYaw );
	Vector vecFwd( cosf( flYawRad ), sinf( flYawRad ), 0.0f );
	m_flHalfLength = 0.5f * fabs( DotProduct( vecMaxs - vecMins, vecFwd ) );
	m_flHalfWidth = 0.5f * fabs( DotProduct( vecMaxs - vecMins, Vector( -vecFwd.y, vecFwd.x, 0.0f ) ) );
	m_flBodyHeight = MAX( vecMaxs.z, VEHICLE_STEP + 8.0f );		// above its origin (the ground point)
	if ( m_flHalfWidth > m_flHalfLength * 1.05f )
		Warning( "INVARIANT DOD-SP vehicle %s (%s): the model is %.0f long but %.0f wide along forward_yaw %.0f - a vehicle is longer than it is wide, so forward_yaw is probably wrong (90 off)\n",
				 GetDebugName(), m_pType->szName, m_flHalfLength * 2.0f, m_flHalfWidth * 2.0f, m_pType->flForwardYaw );
}

void CDoDSPVehicle::OnRestore( void )
{
	BaseClass::OnRestore();
	if ( !m_pType )
		m_pType = FindVehicleType( STRING( m_iszVehicleType ) );
	if ( m_pType )
		MeasureBody();
}

void CDoDSPVehicle::Activate( void )
{
	BaseClass::Activate();
	m_flNavCheckTime = gpGlobals->curtime + 1.5f;		// grade and set off once the node graph is built (DriveThink)
}

float CDoDSPVehicle::HeadingFromAngles( void ) const
{
	return AngleNormalize( GetAbsAngles().y + ( m_pType ? m_pType->flForwardYaw : 0.0f ) );
}

// The orientation for a heading at a spot: the model's front along the heading, its up along the
// ground under its four corners. *pflGroundZ = the ground height under its middle.
QAngle CDoDSPVehicle::BuildAngles( float flHeading, const Vector &vecPos, float *pflGroundZ )
{
	float flRad = DEG2RAD( flHeading );
	Vector vecF( cosf( flRad ), sinf( flRad ), 0.0f );
	Vector vecR( vecF.y, -vecF.x, 0.0f );
	float flL = m_flHalfLength * 0.75f, flW = m_flHalfWidth * 0.75f;
	float zF, zB, zL, zR;
	float flUp = VEHICLE_STEP + 24.0f, flDown = 96.0f;
	bool bF = GroundAt( vecPos + vecF * flL, flUp, flDown, &zF, NULL, this );
	bool bB = GroundAt( vecPos - vecF * flL, flUp, flDown, &zB, NULL, this );
	bool bL = GroundAt( vecPos - vecR * flW, flUp, flDown, &zL, NULL, this );
	bool bR = GroundAt( vecPos + vecR * flW, flUp, flDown, &zR, NULL, this );
	float z0 = vecPos.z;
	if ( !bF ) zF = z0;
	if ( !bB ) zB = z0;
	if ( !bL ) zL = z0;
	if ( !bR ) zR = z0;
	*pflGroundZ = ( bF || bB || bL || bR ) ? 0.5f * ( MAX( zF, zB ) + MAX( zL, zR ) ) : z0;

	Vector vecF3 = vecF * ( 2.0f * flL ) + Vector( 0, 0, zF - zB );
	Vector vecR3 = vecR * ( 2.0f * flW ) + Vector( 0, 0, zR - zL );
	VectorNormalize( vecF3 );
	VectorNormalize( vecR3 );
	Vector vecU = CrossProduct( vecR3, vecF3 );
	VectorNormalize( vecU );
	if ( vecU.z < 0.5f )
		vecU = Vector( 0, 0, 1 );
	vecR3 = CrossProduct( vecF3, vecU );
	VectorNormalize( vecR3 );
	vecF3 = CrossProduct( vecU, vecR3 );

	// model axes: its forward (forward_yaw in model space) -> F, model Z -> U
	float flMY = DEG2RAD( m_pType->flForwardYaw );
	// model X and Y in terms of (F, R): model forward mf = (cos, sin); model right mr = (sin, -cos)
	// model X = cos*F_model... solve: X_model = mf.x * F + mr.x * R ; Y_model = mf.y * F + mr.y * R
	Vector vecMF( cosf( flMY ), sinf( flMY ), 0.0f );
	Vector vecMR( vecMF.y, -vecMF.x, 0.0f );
	Vector vecX = vecF3 * vecMF.x + vecR3 * vecMR.x;
	Vector vecY = vecF3 * vecMF.y + vecR3 * vecMR.y;
	matrix3x4_t mat;
	MatrixSetColumn( vecX, 0, mat );
	MatrixSetColumn( vecY, 1, mat );
	MatrixSetColumn( vecU, 2, mat );
	MatrixSetColumn( vec3_origin, 3, mat );
	QAngle ang;
	MatrixAngles( mat, ang );
	return ang;
}

//-----------------------------------------------------------------------------
// Routes
//-----------------------------------------------------------------------------
// The graded nodes it can drive to (bFromHere: from vecPos) or from (to vecPos) straight - candidates by
// distance within 1024 units, the first 16 tested, at most nMax returned (nearest first). One owner for "which
// nodes join a spot to the road"; NearestNode is its first answer.
int CDoDSPVehicle::NodeCandidates( const Vector &vecPos, bool bFromHere, int nMax, CUtlVector<int> &out )
{
	out.RemoveAll();
	VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_flBodyHeight, m_pType->szName );
	if ( !pNav )
		return 0;
	struct Cand_t { float flDist; int i; };
	CUtlVector<Cand_t> cands;
	for ( int i = 0; i < pNav->nNodes; i++ )
	{
		if ( !pNav->links[i].Count() )
			continue;
		float flDist = ( g_pBigAINet->GetNode( i, false )->GetOrigin() - vecPos ).Length();
		if ( flDist <= 1024.0f )
		{
			Cand_t c = { flDist, i };
			cands.AddToTail( c );
		}
	}
	for ( int k = 0; k < cands.Count() && k < 16 && out.Count() < nMax; k++ )
	{
		int iMin = k;
		for ( int m = k + 1; m < cands.Count(); m++ )
			if ( cands[m].flDist < cands[iMin].flDist )
				iMin = m;
		Cand_t tmp = cands[k]; cands[k] = cands[iMin]; cands[iMin] = tmp;
		const Vector &vecNode = g_pBigAINet->GetNode( cands[k].i, false )->GetOrigin();
		if ( CorridorClear( bFromHere ? vecPos : vecNode, bFromHere ? vecNode : vecPos, m_flHalfWidth, m_flBodyHeight, this ) )
			out.AddToTail( cands[k].i );
	}
	return out.Count();
}

int CDoDSPVehicle::NearestNode( const Vector &vecPos, bool bFromHere )
{
	CUtlVector<int> nodes;
	return NodeCandidates( vecPos, bFromHere, 1, nodes ) ? nodes[0] : -1;
}

// What setting off toward vecTo costs in turning, in units of driving: 0 straight ahead, VEHICLE_TURN_COST
// for a full about-turn (tracked vehicles pivot on the spot; either way it's time spent not getting anywhere).
float CDoDSPVehicle::TurnCost( const Vector &vecTo ) const
{
	Vector vecDir( vecTo.x, vecTo.y, 0.0f );
	if ( VectorNormalize( vecDir ) < 1.0f )
		return 0.0f;
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	return VEHICLE_TURN_COST * 0.5f * ( 1.0f - DotProduct( vecDir, vecFwd ) );
}

bool CDoDSPVehicle::PlanRoute( void )
{
	m_Route.RemoveAll();
	m_iRoute = 0;
	Vector vecFrom = GetAbsOrigin();
	// straight there only when the goal is close (or the map has no nodes): further away the nodes are the
	// mapper's roads and the vehicle keeps to them (V1c test: it cut across country and through trees)
	// which nodes join it to the road, and the road to the goal: SEVERAL each (V1e test: it took the nearest
	// node even when that lay behind it - "rotated all the way around to line up with a node, then all the
	// way back"; and the end node before the goal made a hook). The search picks the pair that makes the
	// cheapest whole drive, counting the turn it would have to make on the spot to reach the first node.
	CUtlVector<int> starts, ends;
	NodeCandidates( vecFrom, true, VEHICLE_JOIN_NODES, starts );
	bool bNoNodes = !starts.Count();
	if ( ( bNoNodes || ( m_vecGoal - vecFrom ).Length2D() <= VEHICLE_DIRECT_DIST ) &&
		 CorridorClear( vecFrom, m_vecGoal, m_flHalfWidth, m_flBodyHeight, this ) )
	{
		m_Route.AddToTail( m_vecGoal );		// straight there
		return true;
	}
	VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_flBodyHeight, m_pType->szName );
	NodeCandidates( m_vecGoal, false, VEHICLE_JOIN_NODES, ends );
	if ( !pNav || !starts.Count() || !ends.Count() )
		return false;

	// A* over the graded links from every start node (cost: the drive there + the turn) to any end node (cost:
	// + the drive from it to the goal), skipping links it got stuck on lately
	int n = pNav->nNodes;
	CUtlVector<float> g; g.SetCount( n );
	CUtlVector<float> flEndCost; flEndCost.SetCount( n );
	CUtlVector<int> from; from.SetCount( n );
	CUtlVector<char> closed; closed.SetCount( n );
	for ( int i = 0; i < n; i++ ) { g[i] = FLT_MAX; flEndCost[i] = -1.0f; from[i] = -1; closed[i] = 0; }
	for ( int k = 0; k < ends.Count(); k++ )
		flEndCost[ends[k]] = ( m_vecGoal - g_pBigAINet->GetNode( ends[k], false )->GetOrigin() ).Length();
	CUtlVector<int> open;
	for ( int k = 0; k < starts.Count(); k++ )
	{
		Vector vecTo = g_pBigAINet->GetNode( starts[k], false )->GetOrigin() - vecFrom;
		g[starts[k]] = vecTo.Length() + TurnCost( vecTo );
		open.AddToTail( starts[k] );
	}
	float flBestTotal = FLT_MAX;
	int iEnd = -1;
	float now = gpGlobals->curtime;
	while ( open.Count() )
	{
		int iBestOpen = 0;
		float flBestF = FLT_MAX;
		for ( int k = 0; k < open.Count(); k++ )
		{
			float f = g[open[k]] + ( g_pBigAINet->GetNode( open[k], false )->GetOrigin() - m_vecGoal ).Length();
			if ( f < flBestF ) { flBestF = f; iBestOpen = k; }
		}
		if ( flBestF >= flBestTotal )
			break;		// nothing left can beat the best whole drive found
		int cur = open[iBestOpen];
		open.FastRemove( iBestOpen );
		closed[cur] = 1;
		if ( flEndCost[cur] >= 0.0f && g[cur] + flEndCost[cur] < flBestTotal )
		{
			flBestTotal = g[cur] + flEndCost[cur];
			iEnd = cur;
		}
		const Vector &vecCur = g_pBigAINet->GetNode( cur, false )->GetOrigin();
		for ( int k = 0; k < pNav->links[cur].Count(); k++ )
		{
			int nb = pNav->links[cur][k];
			if ( closed[nb] )
				continue;
			bool bBad = false;
			for ( int b = 0; b < m_BadLinks.Count(); b++ )
				if ( m_BadLinks[b].flUntil > now && ( ( m_BadLinks[b].a == cur && m_BadLinks[b].b == nb ) || ( m_BadLinks[b].a == nb && m_BadLinks[b].b == cur ) ) )
					bBad = true;
			if ( bBad )
				continue;
			float flG = g[cur] + ( g_pBigAINet->GetNode( nb, false )->GetOrigin() - vecCur ).Length();
			if ( flG < g[nb] )
			{
				if ( g[nb] == FLT_MAX )
					open.AddToTail( nb );
				g[nb] = flG;
				from[nb] = cur;
			}
		}
	}
	if ( iEnd < 0 )
		return false;

	CUtlVector<int> nodes;
	for ( int i = iEnd; i >= 0; i = from[i] )
		nodes.AddToHead( i );
	// straighten - but keep to the road: from each point, the furthest later point it can drive to directly
	// AND whose shortcut passes every node it skips within dodsp_vehicle_road_slack (a straight road loses its
	// zig-zag; a bend keeps its nodes). V1c test: "it seems to skip most of the nodes".
	CUtlVector<Vector> pts;
	pts.AddToTail( vecFrom );
	for ( int i = 0; i < nodes.Count(); i++ )
		pts.AddToTail( g_pBigAINet->GetNode( nodes[i], false )->GetOrigin() );
	pts.AddToTail( m_vecGoal );
	int iAt = 0;
	while ( iAt < pts.Count() - 1 )
	{
		int iNext = iAt + 1;
		for ( int j = pts.Count() - 1; j > iAt + 1; j-- )
		{
			bool bOnRoad = true;
			for ( int k = iAt + 1; k < j && bOnRoad; k++ )
			{
				Vector vecSeg = pts[j] - pts[iAt];
				vecSeg.z = 0.0f;
				Vector vecTo = pts[k] - pts[iAt];
				vecTo.z = 0.0f;
				float flSegLen2 = vecSeg.LengthSqr();
				float t = flSegLen2 > 1.0f ? clamp( DotProduct( vecTo, vecSeg ) / flSegLen2, 0.0f, 1.0f ) : 0.0f;
				if ( ( vecTo - vecSeg * t ).Length() > dodsp_vehicle_road_slack.GetFloat() )
					bOnRoad = false;
			}
			if ( bOnRoad && CorridorClear( pts[iAt], pts[j], m_flHalfWidth, m_flBodyHeight, this ) )
			{
				iNext = j;
				break;
			}
		}
		m_Route.AddToTail( pts[iNext] );
		iAt = iNext;
	}
	return m_Route.Count() > 0;
}

bool CDoDSPVehicle::DriveTo( const Vector &vecGoal )
{
	float flZ;
	if ( GroundAt( vecGoal, 32.0f, 256.0f, &flZ, NULL, this ) )
		m_vecGoal = Vector( vecGoal.x, vecGoal.y, flZ );
	else
		m_vecGoal = vecGoal;
	m_bHasGoal = true;
	m_eStuck = VSTUCK_NONE;
	m_vecProgressPos = GetAbsOrigin();
	m_flProgressHeading = m_flHeading;
	m_flProgressTime = gpGlobals->curtime;
	if ( !PlanRoute() )
	{
		m_bHasGoal = false;
		SetStuck( VSTUCK_NOROUTE, NULL );
		m_OnNoRoute.FireOutput( this, this );
		return false;
	}
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s (%s): driving to %.0f %.0f %.0f, %d route points\n", GetDebugName(), m_pType->szName,
			 m_vecGoal.x, m_vecGoal.y, m_vecGoal.z, m_Route.Count() );
	return true;
}

void CDoDSPVehicle::Stop( void )
{
	m_bHasGoal = false;
	m_Route.RemoveAll();
}

void CDoDSPVehicle::SetStuck( VehicleStuck_t eWhy, CBaseEntity *pWho )
{
	m_eStuck = eWhy;
	Vector p = GetAbsOrigin();
	Warning( "INVARIANT DOD-SP vehicle %s (%s): stuck at %.0f %.0f %.0f - %s%s%s\n", GetDebugName(), m_pType->szName, p.x, p.y, p.z,
			 s_pszStuck[eWhy], pWho ? ": " : "", pWho ? pWho->GetDebugName() : "" );
	if ( eWhy != VSTUCK_NOROUTE )
		m_OnStuck.FireOutput( pWho ? pWho : this, this );
}

void CDoDSPVehicle::Blocked( CBaseEntity *pOther )
{
	// The engine couldn't push pOther out of the way: wait for it (a man steps aside); the stuck
	// watchdog takes over if it doesn't.
	if ( m_hBlocker.Get() != pOther )
		m_flBlockedTime = gpGlobals->curtime;
	m_hBlocker = pOther;
}

//-----------------------------------------------------------------------------
// Driving
//-----------------------------------------------------------------------------
void CDoDSPVehicle::DriveThink( void )
{
	SetNextThink( gpGlobals->curtime + VEHICLE_THINK );
	SetMoveDoneTime( 1.0f );
	if ( !m_pType )
		return;
	float dt = VEHICLE_THINK;
	if ( m_flHalfWidth <= 0.0f || m_flBodyHeight <= 0.0f )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: body not measured (width %.0f, height %.0f) - measuring now\n", GetDebugName(), m_flHalfWidth * 2.0f, m_flBodyHeight );
		MeasureBody();
	}
	float now = gpGlobals->curtime;
	Vector vecPos = GetAbsOrigin();
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	if ( m_flNavCheckTime > 0.0f && now >= m_flNavCheckTime )
	{
		m_flNavCheckTime = 0.0f;
		GetVehicleNav( m_flHalfWidth, m_flBodyHeight, m_pType->szName );		// the load line, before the fight
		if ( m_iszInitialGoal != NULL_STRING && !m_bInitialGoalDone )
		{
			m_bInitialGoalDone = true;		// once - not again after a save is loaded
			CBaseEntity *pGoal = gEntList.FindEntityByName( NULL, m_iszInitialGoal );
			if ( pGoal )
				DriveTo( pGoal->GetAbsOrigin() );
			else
				Warning( "DOD-SP vehicle %s: goal '%s' not found\n", GetDebugName(), STRING( m_iszInitialGoal ) );
		}
	}
	if ( m_bHasGoal && !m_Route.Count() && !PlanRoute() )		// the route isn't saved: plan again after a load
		Stop();

	// where to: the route point it's on (passed when within a vehicle half-length of it)
	float flTargetSpeed = 0.0f, flYawRate = 0.0f;
	if ( m_bHasGoal && m_Route.Count() )
	{
		while ( m_iRoute < m_Route.Count() - 1 && ( m_Route[m_iRoute] - vecPos ).Length2D() < m_flHalfLength )
			m_iRoute++;
		Vector vecTo = m_Route[m_iRoute] - vecPos;
		vecTo.z = 0.0f;
		float flDistGoal = ( m_vecGoal - vecPos ).Length2D();
		if ( m_iRoute == m_Route.Count() - 1 && flDistGoal < 48.0f )
		{
			Stop();
			m_OnArrived.FireOutput( this, this );
			if ( dodsp_vehicle_debug.GetBool() )
				Msg( "DOD-SP vehicle %s: arrived\n", GetDebugName() );
		}
		else
		{
			float flWant = RAD2DEG( atan2f( vecTo.y, vecTo.x ) );
			float flDiff = AngleDiff( flWant, m_flHeading );
			float flMax = m_pType->flMaxSpeed * m_flMaxSpeedScale;
			// slow for the corner ahead and for the goal (it can stop in time)
			float flCos = cosf( DEG2RAD( MIN( fabs( flDiff ), 90.0f ) ) );
			flTargetSpeed = flMax * MAX( flCos, 0.25f );
			flTargetSpeed = MIN( flTargetSpeed, sqrtf( 2.0f * m_pType->flBrake * MAX( flDistGoal - 24.0f, 0.0f ) ) + 20.0f );
			if ( m_pType->bTracked )
			{
				if ( fabs( flDiff ) > 50.0f )
					flTargetSpeed = 0.0f;		// pivot on the spot first
				flYawRate = clamp( flDiff * 2.0f, -m_pType->flTurnRate, m_pType->flTurnRate );
			}
			else
			{
				// a car turns only while it rolls: yaw rate = speed / radius
				float flRateMax = RAD2DEG( fabs( m_flSpeed ) / MAX( m_pType->flMinTurnRadius, 50.0f ) );
				flRateMax = MIN( flRateMax, m_pType->flTurnRate );
				flYawRate = clamp( flDiff * 2.0f, -flRateMax, flRateMax );
				if ( fabs( flDiff ) > 60.0f )
					flTargetSpeed = MIN( flTargetSpeed, 60.0f );		// slow, tight turn
			}
		}
	}

	// waiting for someone it can't push
	if ( m_hBlocker.Get() && now - m_flBlockedTime < 1.0f )
		flTargetSpeed = 0.0f;
	else
		m_hBlocker = NULL;

	// speed
	float flRate = ( fabs( flTargetSpeed ) > fabs( m_flSpeed ) ) ? m_pType->flAccel : m_pType->flBrake;
	m_flSpeed = Approach( flTargetSpeed, m_flSpeed, flRate * dt );

	// what's ahead: THE corridor test over the stretch the front of the body covers next (pushers go through
	// the world, so this is what stops it) - the same test the route was planned with
	if ( fabs( m_flSpeed ) > 1.0f )
	{
		float flSign = ( m_flSpeed > 0.0f ) ? 1.0f : -1.0f;
		Vector vecAheadEnd = vecPos + vecFwd * flSign * ( m_flHalfLength + fabs( m_flSpeed ) * dt * 2.0f + 16.0f );
		CorridorWhy_t why;
		if ( !CorridorClear( vecPos, vecAheadEnd, m_flHalfWidth, m_flBodyHeight, this, &why ) )
		{
			m_flSpeed = 0.0f;
			if ( m_eStuck != VSTUCK_WORLD && m_bHasGoal && now - m_flProgressTime > 1.0f )
			{
				m_eStuck = VSTUCK_WORLD;
				if ( dodsp_vehicle_debug.GetBool() )
					Msg( "DOD-SP vehicle %s: can't go on - %s%s%s at %.0f %.0f %.0f\n", GetDebugName(), s_pszCorridorFail[why.eFail],
						 why.szWhat[0] ? ", " : "", why.szWhat, why.vecAt.x, why.vecAt.y, why.vecAt.z );
			}
		}
	}

	// heading, then the orientation and height the ground gives at the next spot
	m_flHeading = AngleNormalize( m_flHeading + flYawRate * dt );
	Vector vecNext = vecPos + vecFwd * ( m_flSpeed * dt );
	float flGroundZ;
	QAngle angWant = BuildAngles( m_flHeading, vecNext, &flGroundZ );
	QAngle angNow = GetLocalAngles();
	QAngle angVel( AngleDiff( angWant.x, angNow.x ), AngleDiff( angWant.y, angNow.y ), AngleDiff( angWant.z, angNow.z ) );
	angVel *= ( 1.0f / dt ) * 0.5f;
	Vector vecVel = vecFwd * m_flSpeed;
	vecVel.z = clamp( ( flGroundZ - vecPos.z ) / dt * 0.5f, -300.0f, 300.0f );
	SetAbsVelocity( vecVel );
	SetLocalAngularVelocity( angVel );
	m_flTrackSpeed = m_flSpeed;

	// the stuck watchdog: wants to move, hasn't
	if ( m_bHasGoal )
	{
		// progress = it moved 8 units or turned 10 degrees (a tank pivoting on the spot isn't stuck)
		if ( ( vecPos - m_vecProgressPos ).Length2D() > 8.0f || fabs( AngleDiff( m_flHeading, m_flProgressHeading ) ) > 10.0f )
		{
			m_vecProgressPos = vecPos;
			m_flProgressHeading = m_flHeading;
			m_flProgressTime = now;
			if ( m_eStuck == VSTUCK_WORLD )
				m_eStuck = VSTUCK_NONE;
		}
		else if ( now - m_flProgressTime > VEHICLE_STUCK_TIME )
		{
			VehicleStuck_t eWhy = m_hBlocker.Get() ? VSTUCK_ENTITY : ( m_eStuck == VSTUCK_WORLD ? VSTUCK_WORLD : VSTUCK_NOPROGRESS );
			SetStuck( eWhy, m_hBlocker.Get() );
			// remember the stretch it failed on, plan around it
			Vector vecAt = m_Route.IsValidIndex( m_iRoute ) ? m_Route[m_iRoute] : m_vecGoal;
			int a = NearestNode( vecPos, false ), b = NearestNode( vecAt, false );
			if ( a >= 0 && b >= 0 && a != b )
			{
				BadLink_t bad = { a, b, now + VEHICLE_BADLINK_TIME };
				m_BadLinks.AddToTail( bad );
			}
			m_vecProgressPos = vecPos;
			m_flProgressTime = now;
			if ( !PlanRoute() )
			{
				SetStuck( VSTUCK_NOROUTE, NULL );
				m_OnNoRoute.FireOutput( this, this );
				Stop();
			}
		}
	}

	WeaponThink( dt );

	if ( dodsp_vehicle_debug.GetBool() && m_Route.Count() )
	{
		Vector vecLast = vecPos;
		for ( int i = m_iRoute; i < m_Route.Count(); i++ )
		{
			NDebugOverlay::Line( vecLast + Vector( 0, 0, 16 ), m_Route[i] + Vector( 0, 0, 16 ), 0, 255, 0, true, VEHICLE_THINK * 2 );
			vecLast = m_Route[i];
		}
	}
}

//-----------------------------------------------------------------------------
// V2: aim and fire
//
// Who it shoots: anyone of a side hostile to its own (dodsp_factions - one owner), alive, not notarget, in
// range and in sight from the turret. It keeps its target while that stays valid (no flicking between two
// men), and a SetTarget input overrides the choice.
// How it aims: the turret turns about the hull's up axis (turret_rate) and the barrel elevates (pitch_rate)
// toward the target, both in the hull's own frame, so a tank on a slope still aims true. The gun fires only
// along where the barrel REALLY points (never "at" the target): if the turret is still turning, it waits.
// The turret model is measured once (CalibrateTurret): which way its barrel faces, which sign of the
// barrel_pitch pose raises it, and its muzzle - so no model's convention is guessed.
// It never fires when a friend is in the line of fire or near the impact (the shell kills everyone there).
//-----------------------------------------------------------------------------
#define VEHICLE_SCAN_INTERVAL	0.3f
#define VEHICLE_GUN_CONE		1.5f		// degrees: the barrel must be this close to the target to fire the main gun
#define VEHICLE_COAX_CONE		5.0f

bool CDoDSPVehicle::CalibrateTurret( void )
{
	CBaseAnimating *pT = dynamic_cast<CBaseAnimating *>( m_hTurret.Get() );
	if ( !pT || !pT->GetModelPtr() )
		return false;
	m_bTurretCalibrated = true;
	m_iMuzzleAttach = pT->LookupAttachment( "muzzle" );
	m_iPitchPose = pT->LookupPoseParameter( "barrel_pitch" );
	m_flPoseMin = m_flPoseMax = 0.0f;
	if ( m_iPitchPose >= 0 )
		pT->GetPoseParameterRange( m_iPitchPose, m_flPoseMin, m_flPoseMax );
	// measure in the turret's own frame, turret straight, barrel level
	QAngle angSave = pT->GetLocalAngles();
	pT->SetLocalAngles( vec3_angle );
	Vector vecLevel( 0, 0, 0 ), vecHigh( 0, 0, 0 ), vecLow( 0, 0, 0 );
	if ( m_iMuzzleAttach > 0 )
	{
		Vector vecWorld;
		if ( m_iPitchPose >= 0 )
			pT->SetPoseParameter( m_iPitchPose, 0.0f );
		pT->InvalidateBoneCache();
		pT->GetAttachment( m_iMuzzleAttach, vecWorld );
		VectorITransform( vecWorld, pT->EntityToWorldTransform(), vecLevel );
		if ( m_iPitchPose >= 0 )
		{
			pT->SetPoseParameter( m_iPitchPose, m_flPoseMax );
			pT->InvalidateBoneCache();
			pT->GetAttachment( m_iMuzzleAttach, vecWorld );
			VectorITransform( vecWorld, pT->EntityToWorldTransform(), vecHigh );
			pT->SetPoseParameter( m_iPitchPose, m_flPoseMin );
			pT->InvalidateBoneCache();
			pT->GetAttachment( m_iMuzzleAttach, vecWorld );
			VectorITransform( vecWorld, pT->EntityToWorldTransform(), vecLow );
			pT->SetPoseParameter( m_iPitchPose, 0.0f );
			pT->InvalidateBoneCache();
		}
		m_flTurretFwdOffset = ( vecLevel.Length2D() > 1.0f ) ? RAD2DEG( atan2f( vecLevel.y, vecLevel.x ) ) : 0.0f;
	}
	pT->SetLocalAngles( angSave );
	m_flPitchSign = ( vecHigh.z >= vecLow.z ) ? 1.0f : -1.0f;
	Msg( "DOD-SP vehicle %s (%s, %s): turret barrel faces %.0f deg in its model, barrel_pitch %s%.0f..%.0f (%s = up), muzzle %s; "
		 "main gun %s, coax %s\n", GetDebugName(), m_pType->szName, DoDSP_FactionName( m_eFaction ), m_flTurretFwdOffset,
		 m_iPitchPose >= 0 ? "" : "MISSING ", m_flPoseMin, m_flPoseMax, m_flPitchSign > 0 ? "+" : "-",
		 m_iMuzzleAttach > 0 ? "found" : "MISSING", m_pType->gun.bHas ? "yes" : "no", m_pType->coax.bHas ? "yes" : "no" );
	if ( m_iMuzzleAttach <= 0 )
		Warning( "INVARIANT DOD-SP vehicle %s: turret model %s has no \"muzzle\" attachment - shots start at the turret's middle\n",
				 GetDebugName(), m_pType->szTurretModel );
	if ( m_iPitchPose < 0 )
		Warning( "INVARIANT DOD-SP vehicle %s: turret model %s has no \"barrel_pitch\" pose - the barrel can't elevate\n",
				 GetDebugName(), m_pType->szTurretModel );
	else if ( fabs( vecHigh.z - vecLow.z ) < 1.0f )
		Warning( "INVARIANT DOD-SP vehicle %s: barrel_pitch %.0f..%.0f doesn't move the muzzle (%.1f units) - the barrel can't elevate\n",
				 GetDebugName(), m_flPoseMin, m_flPoseMax, fabs( vecHigh.z - vecLow.z ) );
	return true;
}

// where the barrel turns about (the turret's middle; the hull's top when it has no turret)
Vector CDoDSPVehicle::AimPivot( void )
{
	if ( m_hTurret.Get() )
		return m_hTurret->GetAbsOrigin() + Vector( 0, 0, 8 );
	return GetAbsOrigin() + Vector( 0, 0, m_flBodyHeight );
}

// where the barrel really points now (world)
Vector CDoDSPVehicle::BarrelDir( void )
{
	float flYaw = DEG2RAD( m_flTurretYaw + m_flTurretFwdOffset ), flPitch = DEG2RAD( m_flTurretPitch );
	Vector vecLocal( cosf( flYaw ) * cosf( flPitch ), sinf( flYaw ) * cosf( flPitch ), sinf( flPitch ) );
	Vector vecWorld;
	VectorRotate( vecLocal, EntityToWorldTransform(), vecWorld );
	return vecWorld;
}

Vector CDoDSPVehicle::MuzzlePos( const Vector &vecDir )
{
	CBaseAnimating *pT = dynamic_cast<CBaseAnimating *>( m_hTurret.Get() );
	Vector vecPos;
	if ( pT && m_iMuzzleAttach > 0 && pT->GetAttachment( m_iMuzzleAttach, vecPos ) )
		return vecPos;
	return AimPivot() + vecDir * 64.0f;
}

bool CDoDSPVehicle::CanSee( CBaseEntity *pTarget )
{
	trace_t tr;
	CTraceFilterSkipTwoEntities filter( this, m_hTurret.Get(), COLLISION_GROUP_NONE );
	Vector vecFrom = AimPivot();
	// the target's middle or its eyes
	Vector vecPoints[2] = { pTarget->WorldSpaceCenter(), pTarget->EyePosition() };
	for ( int i = 0; i < 2; i++ )
	{
		UTIL_TraceLine( vecFrom, vecPoints[i], MASK_SHOT, &filter, &tr );
		if ( tr.fraction >= 1.0f || ( tr.m_pEnt && ( tr.m_pEnt == pTarget || tr.m_pEnt->GetRootMoveParent() == pTarget ) ) )
			return true;
	}
	return false;
}

bool CDoDSPVehicle::TargetValid( CBaseEntity *pTarget, float flRange, bool bNeedSight )
{
	if ( !pTarget || pTarget == this || !pTarget->IsAlive() || ( pTarget->GetFlags() & FL_NOTARGET ) )
		return false;
	if ( !DoDSP_Hostile( m_eFaction, DoDSP_FactionOf( pTarget ) ) )
		return false;
	if ( ( pTarget->WorldSpaceCenter() - AimPivot() ).Length() > flRange )
		return false;
	return !bNeedSight || CanSee( pTarget );
}

CBaseEntity *CDoDSPVehicle::FindTarget( void )
{
	float flRange = MAX( m_pType->gun.bHas ? m_pType->gun.flRange : 0.0f, m_pType->coax.bHas ? m_pType->coax.flRange : 0.0f );
	if ( m_hForcedTarget.Get() )
		return TargetValid( m_hForcedTarget.Get(), flRange, false ) ? m_hForcedTarget.Get() : NULL;
	// keep the one it has while it's still good
	CBaseEntity *pKeep = m_hTarget.Get();
	if ( pKeep && TargetValid( pKeep, flRange, true ) )
		return pKeep;
	CUtlVector<CBaseEntity *> cands;
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		if ( UTIL_PlayerByIndex( i ) )
			cands.AddToTail( UTIL_PlayerByIndex( i ) );
	CAI_BaseNPC **ppAIs = g_AI_Manager.AccessAIs();
	for ( int i = 0; i < g_AI_Manager.NumAIs(); i++ )
		cands.AddToTail( ppAIs[i] );
	for ( int i = 0; i < s_All.Count(); i++ )
		cands.AddToTail( s_All[i] );
	CBaseEntity *pBest = NULL;
	float flBest = FLT_MAX;
	Vector vecPivot = AimPivot();
	for ( int i = 0; i < cands.Count(); i++ )
	{
		CBaseEntity *p = cands[i];
		if ( !TargetValid( p, flRange, false ) )
			continue;
		float flDist = ( p->WorldSpaceCenter() - vecPivot ).Length();
		if ( flDist < flBest && CanSee( p ) )
		{
			flBest = flDist;
			pBest = p;
		}
	}
	return pBest;
}

// A friend on the line of fire, or within flRadius of where the shot lands (flRadius 0 = line only)
bool CDoDSPVehicle::FriendlyInTheWay( const Vector &vecFrom, const Vector &vecDir, float flRange, float flRadius, CBaseEntity **ppWho )
{
	trace_t tr;
	CTraceFilterSkipTwoEntities filter( this, m_hTurret.Get(), COLLISION_GROUP_NONE );
	UTIL_TraceLine( vecFrom, vecFrom + vecDir * flRange, MASK_SHOT, &filter, &tr );
	if ( tr.m_pEnt && !tr.m_pEnt->IsWorld() && DoDSP_FactionOf( tr.m_pEnt ) == m_eFaction )
	{
		*ppWho = tr.m_pEnt;
		return true;
	}
	if ( flRadius > 0.0f && tr.fraction < 1.0f )
	{
		CBaseEntity *pList[64];
		int n = UTIL_EntitiesInSphere( pList, ARRAYSIZE( pList ), tr.endpos, flRadius, FL_CLIENT | FL_NPC );
		for ( int i = 0; i < n; i++ )
		{
			if ( pList[i] != this && pList[i]->IsAlive() && DoDSP_FactionOf( pList[i] ) == m_eFaction )
			{
				*ppWho = pList[i];
				return true;
			}
		}
	}
	return false;
}

void CDoDSPVehicle::FireMainGun( const Vector &vecAim )
{
	Vector vecDir = vecAim;
	// a little spread: a shell isn't a laser
	float flSpread = DEG2RAD( m_pType->gun.flSpread ) * 0.5f;
	vecDir += Vector( random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ) );
	VectorNormalize( vecDir );
	Vector vecMuzzle = MuzzlePos( vecDir );
	trace_t tr;
	CTraceFilterSkipTwoEntities filter( this, m_hTurret.Get(), COLLISION_GROUP_NONE );
	UTIL_TraceLine( vecMuzzle, vecMuzzle + vecDir * m_pType->gun.flRange, MASK_SHOT, &filter, &tr );
	QAngle angDir;
	VectorAngles( vecDir, angDir );
	g_pEffects->MuzzleFlash( vecMuzzle, angDir, 6.0f, MUZZLEFLASH_TYPE_DEFAULT );
	if ( m_pType->gun.szSound[0] )
		EmitSound( m_pType->gun.szSound );
	UTIL_ScreenShake( vecMuzzle, 6.0f, 150.0f, 0.6f, 1024.0f, SHAKE_START );
	if ( tr.fraction < 1.0f && !tr.startsolid )
	{
		Vector vecBoom = tr.endpos + tr.plane.normal * 8.0f;
		// the game's explosion does the damage (falls off over the radius) and the fireball; ours is the sound
		ExplosionCreate( vecBoom, angDir, this, (int)m_pType->gun.flDamage, (int)m_pType->gun.flRadius, true, 0.0f, false, true );
		if ( m_pType->szExplode[0] )
		{
			CPASAttenuationFilter boomFilter( vecBoom, SNDLVL_140dB );
			EmitSound( boomFilter, entindex(), m_pType->szExplode, &vecBoom );
		}
		UTIL_ScreenShake( vecBoom, 12.0f, 150.0f, 1.0f, m_pType->gun.flRadius * 3.0f, SHAKE_START );
	}
	m_flNextGun = gpGlobals->curtime + m_pType->gun.flReload;
	m_bReloadSoundDone = false;
	m_OnFire.FireOutput( m_hTarget.Get(), this );
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s: main gun fired at %s - hit %s %.0f units out\n", GetDebugName(),
			 m_hTarget.Get() ? m_hTarget->GetClassname() : "?", tr.fraction < 1.0f ? VehicleHitName( tr ) : "nothing",
			 ( tr.endpos - vecMuzzle ).Length() );
}

void CDoDSPVehicle::FireCoax( const Vector &vecDir )
{
	Vector vecMuzzle = MuzzlePos( vecDir );
	FireBulletsInfo_t info;
	info.m_iShots = 1;
	info.m_vecSrc = vecMuzzle;
	info.m_vecDirShooting = vecDir;
	float flHalf = sinf( DEG2RAD( m_pType->coax.flSpread ) * 0.5f );
	info.m_vecSpread = Vector( flHalf, flHalf, flHalf );
	info.m_flDistance = m_pType->coax.flRange;
	info.m_iAmmoType = GetAmmoDef()->Index( "AR2" );
	info.m_iTracerFreq = 2;
	info.m_flDamage = m_pType->coax.flDamage;
	info.m_pAttacker = this;
	FireBullets( info );
	if ( m_pType->coax.szSound[0] )
		EmitSound( m_pType->coax.szSound );
}

void CDoDSPVehicle::WeaponThink( float dt )
{
	if ( !m_pType->gun.bHas && !m_pType->coax.bHas )
		return;
	if ( m_hTurret.Get() && !m_bTurretCalibrated )
		CalibrateTurret();
	float now = gpGlobals->curtime;

	// target
	if ( now >= m_flNextScan )
	{
		m_flNextScan = now + VEHICLE_SCAN_INTERVAL;
		CBaseEntity *pOld = m_hTarget.Get();
		m_hTarget = FindTarget();
		if ( m_hTarget.Get() && m_hTarget.Get() != pOld )
		{
			m_OnTargetSeen.FireOutput( m_hTarget.Get(), this );
			if ( dodsp_vehicle_debug.GetBool() )
				Msg( "DOD-SP vehicle %s (%s): target %s (%s) at %.0f units\n", GetDebugName(), DoDSP_FactionName( m_eFaction ),
					 m_hTarget->GetClassname(), DoDSP_FactionName( DoDSP_FactionOf( m_hTarget.Get() ) ),
					 ( m_hTarget->WorldSpaceCenter() - AimPivot() ).Length() );
		}
	}
	CBaseEntity *pTarget = m_hTarget.Get();

	// turn the turret and raise the barrel toward it (in the hull's frame), else back to the front
	float flWantYaw = 0.0f, flWantPitch = 0.0f;
	Vector vecAimPoint = vec3_origin;
	if ( pTarget )
	{
		vecAimPoint = pTarget->WorldSpaceCenter();
		Vector vecLocal;
		VectorIRotate( vecAimPoint - AimPivot(), EntityToWorldTransform(), vecLocal );
		flWantYaw = RAD2DEG( atan2f( vecLocal.y, vecLocal.x ) ) - m_flTurretFwdOffset;
		flWantPitch = RAD2DEG( atan2f( vecLocal.z, vecLocal.Length2D() ) );
	}
	float flOldYaw = m_flTurretYaw;
	if ( m_hTurret.Get() )
	{
		m_flTurretYaw = ApproachAngle( flWantYaw, m_flTurretYaw, m_pType->flTurretRate * dt );
		float flPitchMin = MIN( m_flPoseMin * m_flPitchSign, m_flPoseMax * m_flPitchSign );
		float flPitchMax = MAX( m_flPoseMin * m_flPitchSign, m_flPoseMax * m_flPitchSign );
		if ( m_iPitchPose < 0 )
			flPitchMin = flPitchMax = 0.0f;
		flWantPitch = clamp( flWantPitch, flPitchMin, flPitchMax );
		m_flTurretPitch = Approach( flWantPitch, m_flTurretPitch, m_pType->flPitchRate * dt );
		m_hTurret->SetLocalAngles( QAngle( 0, m_flTurretYaw, 0 ) );
		CBaseAnimating *pT = dynamic_cast<CBaseAnimating *>( m_hTurret.Get() );
		if ( pT && m_iPitchPose >= 0 )
			pT->SetPoseParameter( m_iPitchPose, m_flTurretPitch * m_flPitchSign );
	}
	UpdateSounds( fabs( AngleDiff( m_flTurretYaw, flOldYaw ) ) / MAX( dt, 0.001f ) );

	// reload clank shortly before the gun is ready again
	if ( !m_bReloadSoundDone && m_pType->szReload[0] && now >= m_flNextGun - 2.0f )
	{
		m_bReloadSoundDone = true;
		EmitSound( m_pType->szReload );
	}

	if ( !pTarget || m_bHoldFire )
		return;
	Vector vecBarrel = BarrelDir();
	Vector vecToTarget = vecAimPoint - MuzzlePos( vecBarrel );
	float flDist = VectorNormalize( vecToTarget );
	float flErr = RAD2DEG( acosf( clamp( DotProduct( vecBarrel, vecToTarget ), -1.0f, 1.0f ) ) );
	bool bSeen = CanSee( pTarget );
	if ( !bSeen )
		return;
	CBaseEntity *pFriend = NULL;
	// main gun: at anything hostile, when the barrel is on it
	if ( m_pType->gun.bHas && now >= m_flNextGun && flErr <= VEHICLE_GUN_CONE && flDist <= m_pType->gun.flRange )
	{
		if ( FriendlyInTheWay( MuzzlePos( vecBarrel ), vecBarrel, flDist + 64.0f, m_pType->gun.flRadius, &pFriend ) )
		{
			if ( dodsp_vehicle_debug.GetBool() )
				Msg( "DOD-SP vehicle %s: holding fire - %s in the way\n", GetDebugName(), pFriend->GetClassname() );
			m_flNextGun = now + 1.0f;		// look again in a second
		}
		else
			FireMainGun( vecBarrel );
		return;
	}
	// coax: bursts at infantry (not at vehicles - it can't hurt them)
	if ( m_pType->coax.bHas && !dynamic_cast<CDoDSPVehicle *>( pTarget ) && flErr <= VEHICLE_COAX_CONE && flDist <= m_pType->coax.flRange )
	{
		if ( now >= m_flCoaxBurstEnd && now >= m_flCoaxPauseUntil )
		{
			m_flCoaxBurstEnd = now + m_pType->coax.flBurst;
			m_flCoaxPauseUntil = m_flCoaxBurstEnd + m_pType->coax.flPause;
		}
		if ( now < m_flCoaxBurstEnd && now >= m_flNextCoaxShot )
		{
			m_flNextCoaxShot = now + 1.0f / MAX( m_pType->coax.flRate, 1.0f );
			if ( !FriendlyInTheWay( MuzzlePos( vecBarrel ), vecBarrel, flDist + 64.0f, 0.0f, &pFriend ) )
				FireCoax( vecBarrel );
		}
	}
}

// Engines: idle fades into move with speed (move pitch rises); the turret's turn loop while it traverses
void CDoDSPVehicle::UpdateSounds( float flTurretRate )
{
	CSoundEnvelopeController &ctl = CSoundEnvelopeController::GetController();
	if ( !m_pEngineIdle && m_pType->szEngineIdle[0] )
	{
		CPASAttenuationFilter filter( this );
		m_pEngineIdle = ctl.SoundCreate( filter, entindex(), m_pType->szEngineIdle );
		if ( m_pEngineIdle )
			ctl.Play( m_pEngineIdle, 1.0f, 100 );
	}
	if ( !m_pEngineMove && m_pType->szEngineMove[0] )
	{
		CPASAttenuationFilter filter( this );
		m_pEngineMove = ctl.SoundCreate( filter, entindex(), m_pType->szEngineMove );
		if ( m_pEngineMove )
			ctl.Play( m_pEngineMove, 0.0f, 85 );
	}
	float t = clamp( fabs( m_flSpeed ) / MAX( m_pType->flMaxSpeed, 1.0f ), 0.0f, 1.0f );
	if ( m_pEngineIdle )
		ctl.SoundChangeVolume( m_pEngineIdle, 1.0f - 0.8f * t, 0.2f );
	if ( m_pEngineMove )
	{
		ctl.SoundChangeVolume( m_pEngineMove, MIN( 1.0f, t * 1.5f ), 0.2f );
		ctl.SoundChangePitch( m_pEngineMove, 85.0f + 30.0f * t, 0.2f );
	}
	bool bTurning = flTurretRate > 2.0f;
	if ( bTurning && !m_bTurretTurning && m_pType->szTurretTurn[0] )
	{
		if ( !m_pTurretTurn )
		{
			CPASAttenuationFilter filter( this );
			m_pTurretTurn = ctl.SoundCreate( filter, entindex(), m_pType->szTurretTurn );
		}
		if ( m_pTurretTurn )
			ctl.Play( m_pTurretTurn, 0.8f, 100 );
	}
	else if ( !bTurning && m_bTurretTurning )
	{
		if ( m_pTurretTurn )
			ctl.SoundChangeVolume( m_pTurretTurn, 0.0f, 0.1f );
		if ( m_pType->szTurretStop[0] )
			EmitSound( m_pType->szTurretStop );
	}
	m_bTurretTurning = bTurning;
}

void CDoDSPVehicle::StopSounds( void )
{
	CSoundEnvelopeController &ctl = CSoundEnvelopeController::GetController();
	CSoundPatch **ppAll[] = { &m_pEngineIdle, &m_pEngineMove, &m_pTurretTurn };
	for ( int i = 0; i < ARRAYSIZE( ppAll ); i++ )
	{
		if ( *ppAll[i] )
		{
			ctl.SoundDestroy( *ppAll[i] );
			*ppAll[i] = NULL;
		}
	}
}

void CDoDSPVehicle::InputSetTarget( inputdata_t &inputdata )
{
	m_hForcedTarget = gEntList.FindEntityByName( NULL, inputdata.value.String(), this, inputdata.pActivator, inputdata.pCaller );
	if ( !m_hForcedTarget.Get() )
		Warning( "DOD-SP vehicle %s: SetTarget '%s' - no such entity\n", GetDebugName(), inputdata.value.String() );
	m_flNextScan = 0.0f;
}

void CDoDSPVehicle::InputClearTarget( inputdata_t &inputdata )
{
	m_hForcedTarget = NULL;
	m_flNextScan = 0.0f;
}

void CDoDSPVehicle::InputHoldFire( inputdata_t &inputdata )
{
	m_bHoldFire = true;
}

void CDoDSPVehicle::InputOpenFire( inputdata_t &inputdata )
{
	m_bHoldFire = false;
}

int CDoDSPVehicle::DrawDebugTextOverlays( void )
{
	int text = BaseClass::DrawDebugTextOverlays();
	if ( m_debugOverlays & OVERLAY_TEXT_BIT )
	{
		char sz[128];
		Q_snprintf( sz, sizeof( sz ), "%s speed %.0f heading %.0f %s", m_pType ? m_pType->szName : "?", m_flSpeed, m_flHeading, s_pszStuck[m_eStuck] );
		EntityText( text++, sz, 0 );
	}
	return text;
}

void CDoDSPVehicle::Report( void )
{
	Vector p = GetAbsOrigin();
	Msg( "DOD-SP vehicle %s (%s, %.0f x %.0f): at %.0f %.0f %.0f, heading %.0f, speed %.0f, %s%s, route %d/%d\n", GetDebugName(),
		 m_pType ? m_pType->szName : "?", m_flHalfLength * 2.0f, m_flHalfWidth * 2.0f, p.x, p.y, p.z, m_flHeading, m_flSpeed,
		 m_bHasGoal ? "driving" : "parked", m_eStuck != VSTUCK_NONE ? ( m_eStuck == VSTUCK_WORLD ? ", wall ahead" : ", was stuck" ) : "",
		 m_iRoute, m_Route.Count() );
	Msg( "    side %s, target %s%s, turret %.0f deg / barrel %.0f deg, main gun %s\n", DoDSP_FactionName( m_eFaction ),
		 m_hTarget.Get() ? m_hTarget->GetClassname() : "none", m_bHoldFire ? " (holding fire)" : "", m_flTurretYaw, m_flTurretPitch,
		 !m_pType || !m_pType->gun.bHas ? "none" : ( gpGlobals->curtime >= m_flNextGun ? "loaded" : "reloading" ) );
}

// Fine-tuning the script's estimate in game: move the turret, print the line for the script
void CDoDSPVehicle::SetTurretOffset( const Vector &vecOffset )
{
	if ( !m_hTurret.Get() )
	{
		Msg( "DOD-SP vehicle %s (%s) has no turret\n", GetDebugName(), m_pType ? m_pType->szName : "?" );
		return;
	}
	m_hTurret->SetLocalOrigin( vecOffset );
	Msg( "DOD-SP vehicle %s: turret at %g %g %g - for scripts/dodsp_vehicles.txt, under \"%s\":\n\t\t\"turret_offset\"\t\t\"%g %g %g\"\n",
		 GetDebugName(), vecOffset.x, vecOffset.y, vecOffset.z, m_pType->szName, vecOffset.x, vecOffset.y, vecOffset.z );
}

void CDoDSPVehicle::InputDriveTo( inputdata_t &inputdata )
{
	CBaseEntity *pGoal = gEntList.FindEntityByName( NULL, inputdata.value.String(), NULL, inputdata.pActivator, inputdata.pCaller );
	if ( pGoal )
		DriveTo( pGoal->GetAbsOrigin() );
	else
		Warning( "DOD-SP vehicle %s: DriveTo - no entity named '%s'\n", GetDebugName(), inputdata.value.String() );
}

void CDoDSPVehicle::InputStop( inputdata_t &inputdata )
{
	Stop();
}

void CDoDSPVehicle::InputSetMaxSpeed( inputdata_t &inputdata )
{
	m_flMaxSpeedScale = clamp( inputdata.value.Float(), 0.0f, 2.0f );
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
CON_COMMAND( dodsp_vehicle_goto, "Send the nearest DOD-SP vehicle (or all with 'all') to where you're aiming." )
{
	CBasePlayer *pPlayer = UTIL_GetCommandClient();
	if ( !pPlayer )
		return;
	Vector vecFwd;
	pPlayer->EyeVectors( &vecFwd );
	trace_t tr;
	UTIL_TraceLine( pPlayer->EyePosition(), pPlayer->EyePosition() + vecFwd * 8192.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr );
	bool bAll = args.ArgC() > 1 && !Q_stricmp( args[1], "all" );
	CDoDSPVehicle *pBest = NULL;
	float flBest = FLT_MAX;
	for ( int i = 0; i < CDoDSPVehicle::s_All.Count(); i++ )
	{
		CDoDSPVehicle *pV = CDoDSPVehicle::s_All[i];
		if ( bAll )
		{
			pV->DriveTo( tr.endpos );
			continue;
		}
		float flDist = ( pV->GetAbsOrigin() - pPlayer->GetAbsOrigin() ).LengthSqr();
		if ( flDist < flBest )
		{
			flBest = flDist;
			pBest = pV;
		}
	}
	if ( pBest )
		pBest->DriveTo( tr.endpos );
	else if ( !bAll )
		Msg( "dodsp_vehicle_goto: no dodsp_vehicle on the map\n" );
}

CON_COMMAND( dodsp_vehicles, "Where every DOD-SP vehicle is and what it's doing." )
{
	if ( !CDoDSPVehicle::s_All.Count() )
		Msg( "DOD-SP vehicles: none on the map\n" );
	for ( int i = 0; i < CDoDSPVehicle::s_All.Count(); i++ )
		CDoDSPVehicle::s_All[i]->Report();
}

CON_COMMAND( dodsp_vehicle_reload, "Read scripts/dodsp_vehicles.txt again (for vehicles spawned after this)." )
{
	LoadVehicleTypes( true );
	Msg( "DOD-SP vehicles: %d types loaded from %s\n", s_Types.Count(), DODSP_VEHICLE_SCRIPT );
}

CON_COMMAND( dodsp_vehicle_turret, "Move the nearest DOD-SP vehicle's turret: dodsp_vehicle_turret <x> <y> <z> (model space; prints the script line)." )
{
	CBasePlayer *pPlayer = UTIL_GetCommandClient();
	if ( !pPlayer || args.ArgC() < 4 )
	{
		Msg( "usage: dodsp_vehicle_turret <x> <y> <z>   (x sideways, y along the hull - front is -y, z up)\n" );
		return;
	}
	CDoDSPVehicle *pBest = NULL;
	float flBest = FLT_MAX;
	for ( int i = 0; i < CDoDSPVehicle::s_All.Count(); i++ )
	{
		float flDist = ( CDoDSPVehicle::s_All[i]->GetAbsOrigin() - pPlayer->GetAbsOrigin() ).LengthSqr();
		if ( flDist < flBest )
		{
			flBest = flDist;
			pBest = CDoDSPVehicle::s_All[i];
		}
	}
	if ( pBest )
		pBest->SetTurretOffset( Vector( atof( args[1] ), atof( args[2] ), atof( args[3] ) ) );
}
