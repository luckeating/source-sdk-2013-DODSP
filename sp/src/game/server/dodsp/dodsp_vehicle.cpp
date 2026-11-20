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
#define VEHICLE_REVERSE_DIST	768.0f		// a route point behind it this close is reached backwards, not by turning round
#define VEHICLE_REVERSE_ANGLE	110.0f		// "behind" = more than this many degrees off the nose
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

// V2c: how a vehicle fights (Hammer key "behavior")
enum VehicleBehavior_t
{
	VBEHAVE_OFFENSIVE,		// hunts: closes to fighting range, shoots on the move too
	VBEHAVE_DEFENSIVE,		// holds good firing spots at long range, stops to shoot, backs away from what gets close
	VBEHAVE_HOLD,			// stays where it is; the turret still fights
	VBEHAVE_SCRIPTED,		// moves only on map inputs (V1/V2 behaviour)
	NUM_VBEHAVES
};
static const char *s_pszBehaviors[] = { "offensive", "defensive", "hold", "scripted" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszBehaviors ) == NUM_VBEHAVES );

static int VehicleBehaviorFromName( const char *psz )
{
	for ( int i = 0; i < NUM_VBEHAVES; i++ )
		if ( !Q_stricmp( psz, s_pszBehaviors[i] ) )
			return i;
	if ( !Q_stricmp( psz, "hunt" ) || !Q_stricmp( psz, "aggressive" ) )
		return VBEHAVE_OFFENSIVE;
	return -1;
}

// What it's doing and why (reason codes; dodsp_vehicle_debug prints every change)
enum VehicleTactic_t
{
	VTACTIC_NONE,
	VTACTIC_SCRIPTED,		// driving to a map / console order
	VTACTIC_HOLD,
	VTACTIC_ROAM,			// no enemy known: patrols the nodes (offensive) or keeps home (defensive)
	VTACTIC_HUNT,			// enemy out of sight: going where it was last known
	VTACTIC_REPOSITION,		// driving to a node with a shot at the enemy, at its kind of range
	VTACTIC_FIGHT,			// has its shot: stays and fires
	NUM_VTACTICS
};
static const char *s_pszTactics[] = { "none", "scripted", "hold", "roam", "hunt", "reposition", "fight" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszTactics ) == NUM_VTACTICS );

enum VehicleTacticWhy_t
{
	VWHY_NONE,
	VWHY_ORDERED,			// a map input / console goto
	VWHY_NO_ENEMY,
	VWHY_SEES_ENEMY,		// in sight and in its range band
	VWHY_TOO_FAR,
	VWHY_TOO_CLOSE,
	VWHY_NO_SHOT,			// enemy known but not in sight from here
	VWHY_REPORTED,			// its side's soldiers reported the enemy
	VWHY_NO_FIRE_SPOT,		// no node with a shot at its range - goes closer / waits
	VWHY_LEASH,				// would leave its leash
	VWHY_SETTLING,			// just moved: stays a moment before moving again (no dithering)
	NUM_VWHYS
};
static const char *s_pszTacticWhy[] = { "", "ordered", "no enemy known", "enemy in sight and range", "enemy too far",
	"enemy too close", "no shot from here", "reported by its side", "no node with a shot", "leash", "settling" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszTacticWhy ) == NUM_VWHYS );

#define VEHICLE_TACTIC_INTERVAL	1.0f
#define VEHICLE_FORGET_TIME		30.0f		// an enemy unseen and unreported this long is forgotten
#define VEHICLE_SPOTTER_RANGE	3000.0f		// its side's soldiers this close share what they see
#define VEHICLE_ROAM_RADIUS		3000.0f
#define VEHICLE_FIRE_NODE_SEARCH 4000.0f	// firing spots considered within this of the vehicle
#define VEHICLE_SETTLE_TIME		4.0f		// a new firing spot is kept at least this long
#define VEHICLE_BADSPOT_TIME	20.0f		// a firing spot that gave no shot once it got there isn't picked again this long
// V2d: vehicle space (VehicleInTheWay / SpotTakenBy / GiveWay)
#define VEHICLE_SPACE_MARGIN	24.0f		// clearance kept between two bodies while driving
#define VEHICLE_SPOT_MARGIN		48.0f		// between two vehicles' spots (parked, or the goals they drive to)
#define VEHICLE_YIELD_TIME		1.0f		// waiting for a parked / head-on vehicle this long, it plans round it
#define VEHICLE_STALL_TIME		4.0f		// waiting behind one that doesn't move this long, it plans round it
#define VEHICLE_AVOID_TIME		10.0f		// the vehicle it gave way to counts as an obstacle in its plans this long

enum VehicleSpaceWho_t
{
	VSPACE_DRIVING,			// every vehicle (the body ahead, each think)
	VSPACE_PLANNING,		// parked vehicles and the one it is giving way to (moving ones will have moved)
};

enum VehicleGiveWay_t
{
	VGIVE_NONE,
	VGIVE_PARKED,			// it doesn't move: plan round it
	VGIVE_HEAD_ON,			// both wait for each other: the higher entity index gives way
	VGIVE_STALLED,			// the one ahead stopped (it's waiting itself, or stuck)
	NUM_VEHICLE_GIVEWAY
};
static const char *s_pszGiveWay[] = { "", "it is parked", "head-on, this one gives way", "it isn't moving" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszGiveWay ) == NUM_VEHICLE_GIVEWAY );

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
	float	flSuspAccel;			// degrees the hull pitches per 100 units/s^2 of acceleration (the lurch)
	float	flSuspFreq, flSuspDamp;	// its spring: rocks at this many Hz, this much damping (0..1)
	float	flRecoil;				// the main gun kicks the hull back this many degrees
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
		t->flSuspAccel = pType->GetFloat( "susp_accel", t->bTracked ? 1.6f : 1.0f );
		t->flSuspFreq = pType->GetFloat( "susp_freq", 1.6f );
		t->flSuspDamp = pType->GetFloat( "susp_damp", 0.35f );
		t->flRecoil = pType->GetFloat( "recoil", 2.0f );
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
	UTIL_TraceLine( vecPos + Vector( 0, 0, flUp ), vecPos - Vector( 0, 0, flDown ), MASK_SOLID, &filter, &tr );	// dodsp-lint: not a sight line (ground below a point)
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
		CUtlSymbol sym = pFiles[w].symbol;		// GetWaveName takes a non-const reference (C2664 on the const member)
		const char *pszW = soundemitterbase->GetWaveName( sym );
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
	bool			DriveTo( const Vector &vecGoal, bool bScripted = true );	// scripted: a map input / console order (tactics wait)
	void			Stop( void );
	void			Report( void );
	void			SetTurretOffset( const Vector &vecOffset );
	const char		*Label( void );			// the log's name: targetname (or dodsp_vehicle) # entity index

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
	// V2d vehicle space
	bool			PathClear( const Vector &a, const Vector &b );
	CDoDSPVehicle	*VehicleInTheWay( const Vector &a, const Vector &b, float flHalfWidth, int eWho );
	CDoDSPVehicle	*SpotTakenBy( const Vector &vecSpot );
	void			GiveWay( CDoDSPVehicle *pOther );
	bool			BadSpot( int iNode );
	// V2c tactics
	void			TacticsThink( void );
	void			UpdateQuarry( void );
	int				ChooseFirePosition( const Vector &vecTarget, float flMin, float flMax );
	void			SetTactic( int eTactic, int eWhy );
	bool			InBand( float flDist, float *pflMin, float *pflMax );
	// V2 weapons
	void			WeaponThink( float dt );
	bool			CalibrateTurret( void );
	CBaseEntity		*FindTarget( void );
	bool			TargetValid( CBaseEntity *pTarget, float flRange, bool bNeedSight );
	bool			CanSee( CBaseEntity *pTarget, Vector *pvecPoint = NULL );
	bool			LineOfFire( const Vector &vecFrom, const Vector &vecTo, CBaseEntity *pTarget, float flNear, trace_t *pTr = NULL );
	float			TurretHeight( void ) { return AimPivot().z - GetAbsOrigin().z; }
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

	// V2c: behaviour (how it fights) and what it knows
	string_t		m_iszBehavior;				// Hammer key "behavior": offensive / defensive / hold / scripted
	int				m_eBehavior;
	float			m_flLeash;					// Hammer key "leash": how far from home it may go (0 = anywhere)
	Vector			m_vecHome;
	bool			m_bScriptedGoal;			// driving to a map / console order: tactics wait till it arrives
	EHANDLE			m_hQuarry;					// the enemy it's after (seen itself or reported by its side)
	Vector			m_vecQuarryPos;				// where that enemy was last known to be
	float			m_flQuarryTime;				// when
	int				m_eTactic, m_eTacticWhy;
	float			m_flNextTactic;
	int				m_iFireNode;				// the node it's driving to for a shot (-1 none)
	float			m_flRoamWaitUntil;
	float			m_flRepositions[6];			// ring of the last reposition times (dithering guard)
	int				m_iReposition;
	float			m_flNoRepositionUntil;
	float			m_flSuspPitch, m_flSuspVel, m_flPrevSpeed;	// the hull's rock on its suspension (degrees, nose up +)
	bool			m_bPreferReverse;			// keep the front to the enemy: back away rather than turn round
	bool			m_bReversing;
	struct BadSpot_t { int iNode; float flUntil; };
	BadSpot_t		m_BadSpots[8];				// firing spots that gave no shot (ring), not saved
	int				m_iBadSpot;

	// V2d: other vehicles (not saved: re-found within a think after a load)
	EHANDLE			m_hWaitFor;					// the vehicle it's braking for
	float			m_flWaitSince, m_flWaitSeen;
	float			m_flNextGiveWay;
	EHANDLE			m_hAvoid;					// the vehicle it gave way to: an obstacle in its plans till m_flAvoidUntil
	float			m_flAvoidUntil;
	float			m_flNextHoldMsg;

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
	DEFINE_KEYFIELD( m_iszBehavior, FIELD_STRING, "behavior" ),
	DEFINE_KEYFIELD( m_flLeash, FIELD_FLOAT, "leash" ),
	DEFINE_FIELD( m_eBehavior, FIELD_INTEGER ),
	DEFINE_FIELD( m_vecHome, FIELD_POSITION_VECTOR ),
	DEFINE_FIELD( m_bScriptedGoal, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_hQuarry, FIELD_EHANDLE ),
	DEFINE_FIELD( m_vecQuarryPos, FIELD_POSITION_VECTOR ),
	DEFINE_FIELD( m_flQuarryTime, FIELD_TIME ),
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
	m_eBehavior = 0;
	m_flLeash = 0.0f;
	m_vecHome = vec3_origin;
	m_bScriptedGoal = false;
	m_vecQuarryPos = vec3_origin;
	m_flQuarryTime = -1000.0f;
	m_eTactic = m_eTacticWhy = 0;
	m_flNextTactic = 0.0f;
	m_iFireNode = -1;
	m_flRoamWaitUntil = 0.0f;
	for ( int i = 0; i < ARRAYSIZE( m_flRepositions ); i++ )
		m_flRepositions[i] = -1000.0f;
	m_iReposition = 0;
	m_flNoRepositionUntil = 0.0f;
	m_bPreferReverse = m_bReversing = false;
	m_flSuspPitch = m_flSuspVel = m_flPrevSpeed = 0.0f;
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
	for ( int i = 0; i < ARRAYSIZE( m_BadSpots ); i++ )
	{
		m_BadSpots[i].iNode = -1;
		m_BadSpots[i].flUntil = 0.0f;
	}
	m_iBadSpot = 0;
	m_flWaitSince = m_flWaitSeen = m_flNextGiveWay = m_flAvoidUntil = m_flNextHoldMsg = 0.0f;
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
		Warning( "INVARIANT DOD-SP vehicle %s: vehicletype '%s' is not in %s - removed\n", Label(), STRING( m_iszVehicleType ), DODSP_VEHICLE_SCRIPT );
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
			Warning( "INVARIANT DOD-SP vehicle %s: faction '%s' is not a side (axis / allies) - kept %s\n", Label(), STRING( m_iszFaction ), DoDSP_FactionName( m_eFaction ) );
	}
	// guard: what it answers to Classify must map back to its side (dodsp_factions is the one owner)
	if ( DoDSP_FactionOf( this ) != m_eFaction )
		Warning( "INVARIANT DOD-SP vehicle %s: side %s but its class counts as %s (dodsp_factions.cpp tables disagree)\n",
				 Label(), DoDSP_FactionName( m_eFaction ), DoDSP_FactionName( DoDSP_FactionOf( this ) ) );

	// how it fights (V2c): offensive unless the map says otherwise
	m_eBehavior = VBEHAVE_OFFENSIVE;
	if ( m_iszBehavior != NULL_STRING && STRING( m_iszBehavior )[0] )
	{
		int e = VehicleBehaviorFromName( STRING( m_iszBehavior ) );
		if ( e >= 0 )
			m_eBehavior = e;
		else
			Warning( "INVARIANT DOD-SP vehicle %s: behavior '%s' is not offensive / defensive / hold / scripted - offensive\n", Label(), STRING( m_iszBehavior ) );
	}
	if ( m_flLeash <= 0.0f && m_eBehavior == VBEHAVE_DEFENSIVE )
		m_flLeash = 2000.0f;
	m_vecHome = GetAbsOrigin();

	SetSolid( SOLID_VPHYSICS );
	SetMoveType( MOVETYPE_PUSH );
	m_takedamage = DAMAGE_NO;	// V4
	if ( !VPhysicsInitShadow( false, false ) )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: %s has no physics model (.phy) - it can't push or be hit\n", Label(), m_pType->szModel );
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
				 Label(), m_pType->szName, m_flHalfLength * 2.0f, m_flHalfWidth * 2.0f, m_pType->flForwardYaw );
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
		if ( PathClear( bFromHere ? vecPos : vecNode, bFromHere ? vecNode : vecPos ) )
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
	float flLen = VectorNormalize( vecDir );
	if ( flLen < 1.0f )
		return 0.0f;
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	float flDot = DotProduct( vecDir, vecFwd );
	float flCost = VEHICLE_TURN_COST * 0.5f * ( 1.0f - flDot );
	// a node close behind is reached backwards (the drive code reverses to it): cost = lining the tail up,
	// plus the slower drive
	if ( flLen <= VEHICLE_REVERSE_DIST && m_pType->flReverseSpeed > 0.0f )
		flCost = MIN( flCost, VEHICLE_TURN_COST * 0.5f * ( 1.0f + flDot ) + flLen * 0.5f );
	return flCost;
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
		 PathClear( vecFrom, m_vecGoal ) )
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
			// a parked vehicle (or the one it is giving way to) on the link: it isn't graded for that - the graph
			// grading is the world only, per map
			if ( bBad || VehicleInTheWay( vecCur, g_pBigAINet->GetNode( nb, false )->GetOrigin(), m_flHalfWidth, VSPACE_PLANNING ) )
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
			if ( bOnRoad && PathClear( pts[iAt], pts[j] ) )
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

bool CDoDSPVehicle::DriveTo( const Vector &vecGoal, bool bScripted )
{
	m_bScriptedGoal = bScripted;
	if ( bScripted )
		m_iFireNode = -1;
	float flZ;
	if ( GroundAt( vecGoal, 32.0f, 256.0f, &flZ, NULL, this ) )
		m_vecGoal = Vector( vecGoal.x, vecGoal.y, flZ );
	else
		m_vecGoal = vecGoal;
	// another vehicle's spot (where it's parked, or where it's driving to): stop short of it, on this side
	// (V2c test: two tanks picked the same spot and drove into each other)
	if ( CDoDSPVehicle *pTaken = SpotTakenBy( m_vecGoal ) )
	{
		Vector vecClaim = pTaken->m_bHasGoal ? pTaken->m_vecGoal : pTaken->GetAbsOrigin();
		Vector vecAway = GetAbsOrigin() - vecClaim;
		vecAway.z = 0.0f;
		if ( VectorNormalize( vecAway ) < 1.0f )
			vecAway = Vector( 1, 0, 0 );
		Vector vecShort = vecClaim + vecAway * ( m_flHalfLength + pTaken->m_flHalfLength + VEHICLE_SPOT_MARGIN );
		if ( GroundAt( vecShort, 64.0f, 256.0f, &flZ, NULL, this ) )
			vecShort.z = flZ;
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: %.0f %.0f %.0f is %s's spot - stopping short at %.0f %.0f %.0f\n", Label(),
				 m_vecGoal.x, m_vecGoal.y, m_vecGoal.z, pTaken->Label(), vecShort.x, vecShort.y, vecShort.z );
		m_vecGoal = vecShort;
	}
	m_bHasGoal = true;
	m_eStuck = VSTUCK_NONE;
	m_vecProgressPos = GetAbsOrigin();
	m_flProgressHeading = m_flHeading;
	m_flProgressTime = gpGlobals->curtime;
	if ( !PlanRoute() )
	{
		m_bHasGoal = false;
		m_bScriptedGoal = false;
		SetStuck( VSTUCK_NOROUTE, NULL );
		m_OnNoRoute.FireOutput( this, this );
		return false;
	}
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s (%s): driving to %.0f %.0f %.0f, %d route points\n", Label(), m_pType->szName,
			 m_vecGoal.x, m_vecGoal.y, m_vecGoal.z, m_Route.Count() );
	return true;
}

void CDoDSPVehicle::Stop( void )
{
	m_bHasGoal = false;
	m_bScriptedGoal = false;
	m_Route.RemoveAll();
}

void CDoDSPVehicle::SetStuck( VehicleStuck_t eWhy, CBaseEntity *pWho )
{
	m_eStuck = eWhy;
	Vector p = GetAbsOrigin();
	Warning( "INVARIANT DOD-SP vehicle %s (%s): stuck at %.0f %.0f %.0f - %s%s%s\n", Label(), m_pType->szName, p.x, p.y, p.z,
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
// V2d: vehicle space. Pushers drive through each other (MOVETYPE_PUSH bodies just overlap) and CorridorClear
// sees only the world, so nothing kept two vehicles apart (V2c test: two Panzer IIs picked the same path and
// drove into each other). One owner each:
//   VehicleInTheWay - would a body of half-width flHalfWidth going from a to b come within VEHICLE_SPACE_MARGIN
//     of another vehicle's body (a capsule: its length, its half-width)? Driving asks it every think for the
//     stretch it covers before it could stop (every vehicle); planning asks it for each leg (parked vehicles and
//     the one it is giving way to - moving ones will have moved). A body already touching another may always
//     move AWAY from it (else two that met could never part).
//   SpotTakenBy - is a spot another vehicle's (where it's parked, or the goal it's driving to)? Firing spots,
//     patrol nodes and hunt goals skip taken spots; DriveTo stops short of one.
//   GiveWay - the right of way when one brakes for another: a parked one is planned round (after
//     VEHICLE_YIELD_TIME), head-on the higher entity index gives way, one ahead that stalled is planned round
//     after VEHICLE_STALL_TIME; one ahead that drives on is simply followed.
//-----------------------------------------------------------------------------
static float SegSegDist2D( const Vector &a0, const Vector &a1, const Vector &b0, const Vector &b1 )
{
	Vector2D p1( a0.x, a0.y ), p2( b0.x, b0.y );
	Vector2D d1( a1.x - a0.x, a1.y - a0.y ), d2( b1.x - b0.x, b1.y - b0.y ), r = p1 - p2;
	float a = d1.LengthSqr(), e = d2.LengthSqr(), f = DotProduct2D( d2, r );
	float s = 0.0f, t = 0.0f;
	if ( a <= 1e-4f && e <= 1e-4f )
		return r.Length();
	if ( a <= 1e-4f )
		t = clamp( f / e, 0.0f, 1.0f );
	else
	{
		float c = DotProduct2D( d1, r );
		if ( e <= 1e-4f )
			s = clamp( -c / a, 0.0f, 1.0f );
		else
		{
			float b = DotProduct2D( d1, d2 ), flDenom = a * e - b * b;
			s = ( flDenom > 1e-4f ) ? clamp( ( b * f - c * e ) / flDenom, 0.0f, 1.0f ) : 0.0f;
			t = ( b * s + f ) / e;
			if ( t < 0.0f )
			{
				t = 0.0f;
				s = clamp( -c / a, 0.0f, 1.0f );
			}
			else if ( t > 1.0f )
			{
				t = 1.0f;
				s = clamp( ( b - c ) / a, 0.0f, 1.0f );
			}
		}
	}
	Vector2D c1 = p1 + d1 * s, c2 = p2 + d2 * t;
	return ( c1 - c2 ).Length();
}

// the log's name for a vehicle: every vehicle placed without a name was "dodsp_vehicle" (V2c log: three
// vehicles, one name - which one dithered couldn't be told)
const char *CDoDSPVehicle::Label( void )
{
	static char s_szBuf[4][64];
	static int s_iBuf = 0;
	s_iBuf = ( s_iBuf + 1 ) & 3;
	Q_snprintf( s_szBuf[s_iBuf], sizeof( s_szBuf[0] ), "%s#%d", GetEntityName() != NULL_STRING ? STRING( GetEntityName() ) : "dodsp_vehicle",
				entindex() );
	return s_szBuf[s_iBuf];
}

static const char *VehicleWho( CBaseEntity *pEnt )
{
	CDoDSPVehicle *pV = dynamic_cast<CDoDSPVehicle *>( pEnt );
	return pV ? pV->Label() : ( pEnt ? pEnt->GetClassname() : "?" );
}

// THE planning test for a leg: the world (CorridorClear) and the vehicles that will still be there
bool CDoDSPVehicle::PathClear( const Vector &a, const Vector &b )
{
	return CorridorClear( a, b, m_flHalfWidth, m_flBodyHeight, this ) && !VehicleInTheWay( a, b, m_flHalfWidth, VSPACE_PLANNING );
}

CDoDSPVehicle *CDoDSPVehicle::VehicleInTheWay( const Vector &a, const Vector &b, float flHalfWidth, int eWho )
{
	float now = gpGlobals->curtime;
	for ( int i = 0; i < s_All.Count(); i++ )
	{
		CDoDSPVehicle *pV = s_All[i];
		if ( pV == this || pV->m_flHalfWidth <= 0.0f )
			continue;
		if ( eWho == VSPACE_PLANNING && pV->m_bHasGoal && !( m_hAvoid.Get() == pV && now < m_flAvoidUntil ) )
			continue;
		const Vector &c = pV->GetAbsOrigin();
		if ( fabs( c.z - 0.5f * ( a.z + b.z ) ) > 256.0f )
			continue;		// a bridge above / below
		Vector f( cosf( DEG2RAD( pV->m_flHeading ) ), sinf( DEG2RAD( pV->m_flHeading ) ), 0.0f );
		float flSeg = MAX( pV->m_flHalfLength - pV->m_flHalfWidth, 0.0f );
		Vector c0 = c - f * flSeg, c1 = c + f * flSeg;
		float flReach = flHalfWidth + pV->m_flHalfWidth + VEHICLE_SPACE_MARGIN;
		float flDist = SegSegDist2D( a, b, c0, c1 );
		if ( flDist >= flReach )
			continue;
		// already touching at the start and moving away (the closest point is where it stands): let it go
		float flStart = SegSegDist2D( a, a, c0, c1 );
		if ( flStart < flReach && flDist >= flStart - 1.0f )
			continue;
		return pV;
	}
	return NULL;
}

CDoDSPVehicle *CDoDSPVehicle::SpotTakenBy( const Vector &vecSpot )
{
	for ( int i = 0; i < s_All.Count(); i++ )
	{
		CDoDSPVehicle *pV = s_All[i];
		if ( pV == this )
			continue;
		Vector vecClaim = pV->m_bHasGoal ? pV->m_vecGoal : pV->GetAbsOrigin();
		if ( ( vecClaim - vecSpot ).Length2D() < m_flHalfLength + pV->m_flHalfLength + VEHICLE_SPOT_MARGIN )
			return pV;
	}
	return NULL;
}

void CDoDSPVehicle::GiveWay( CDoDSPVehicle *pV )
{
	float now = gpGlobals->curtime;
	if ( m_hWaitFor.Get() != pV )
	{
		m_hWaitFor = pV;
		m_flWaitSince = now;
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: waiting for %s (%s)\n", Label(), pV->Label(), pV->m_bHasGoal ? "driving" : "parked" );
	}
	m_flWaitSeen = now;
	m_flProgressTime = now;		// waiting for a vehicle isn't stuck: the stuck watchdog would blame the road (bad links)
	if ( now < m_flNextGiveWay )
		return;
	float flWaited = now - m_flWaitSince;
	bool bHeadOn = pV->m_hWaitFor.Get() == this && now - pV->m_flWaitSeen < 0.5f;
	int eWhy = VGIVE_NONE;
	if ( !pV->m_bHasGoal && flWaited > VEHICLE_YIELD_TIME )
		eWhy = VGIVE_PARKED;
	else if ( bHeadOn && entindex() > pV->entindex() && flWaited > VEHICLE_YIELD_TIME )
		eWhy = VGIVE_HEAD_ON;
	else if ( !bHeadOn && fabs( pV->m_flSpeed ) < 5.0f && flWaited > VEHICLE_STALL_TIME )
		eWhy = VGIVE_STALLED;
	if ( eWhy == VGIVE_NONE )
		return;
	m_flNextGiveWay = now + 3.0f;
	// its goal is where that one stands: that's as close as it gets
	if ( !pV->m_bHasGoal && ( m_vecGoal - pV->GetAbsOrigin() ).Length2D() < m_flHalfLength + pV->m_flHalfLength + VEHICLE_SPOT_MARGIN * 2.0f )
	{
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: arrived - %s is parked at its goal\n", Label(), pV->Label() );
		Stop();
		m_OnArrived.FireOutput( this, this );
		return;
	}
	m_hAvoid = pV;
	m_flAvoidUntil = now + VEHICLE_AVOID_TIME;
	if ( PlanRoute() )
	{
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: giving way to %s (%s) - new route, %d points\n", Label(), pV->Label(), s_pszGiveWay[eWhy], m_Route.Count() );
		return;
	}
	// no way round it
	if ( m_bScriptedGoal )
	{
		m_hAvoid = NULL;			// a map order is kept: it waits on its old route
		PlanRoute();
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: no way round %s (%s) - waiting (map order)\n", Label(), pV->Label(), s_pszGiveWay[eWhy] );
	}
	else
	{
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: no way round %s (%s) - gives up this spot\n", Label(), pV->Label(), s_pszGiveWay[eWhy] );
		Stop();
	}
}

bool CDoDSPVehicle::BadSpot( int iNode )
{
	for ( int i = 0; i < ARRAYSIZE( m_BadSpots ); i++ )
		if ( m_BadSpots[i].iNode == iNode && m_BadSpots[i].flUntil > gpGlobals->curtime )
			return true;
	return false;
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
		Warning( "INVARIANT DOD-SP vehicle %s: body not measured (width %.0f, height %.0f) - measuring now\n", Label(), m_flHalfWidth * 2.0f, m_flBodyHeight );
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
				Warning( "DOD-SP vehicle %s: goal '%s' not found\n", Label(), STRING( m_iszInitialGoal ) );
		}
	}
	if ( m_bHasGoal && !m_Route.Count() && !PlanRoute() )		// the route isn't saved: plan again after a load
		Stop();
	if ( m_flNavCheckTime <= 0.0f )
		TacticsThink();

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
				Msg( "DOD-SP vehicle %s: arrived\n", Label() );
		}
		else
		{
			float flWant = RAD2DEG( atan2f( vecTo.y, vecTo.x ) );
			float flDiff = AngleDiff( flWant, m_flHeading );
			float flMax = m_pType->flMaxSpeed * m_flMaxSpeedScale;
			// backwards (owner, V2c): a point behind it close by - or anywhere when it keeps its front to an enemy -
			// is reached in reverse instead of turning round. Sticky until the point is well off its tail.
			float flPointDist = vecTo.Length2D();
			bool bBehind = fabs( flDiff ) > ( m_bReversing ? 70.0f : VEHICLE_REVERSE_ANGLE );
			m_bReversing = bBehind && ( flPointDist <= VEHICLE_REVERSE_DIST || m_bPreferReverse ) && m_pType->flReverseSpeed > 0.0f;
			if ( m_bReversing )
			{
				flWant = AngleNormalize( flWant + 180.0f );
				flDiff = AngleDiff( flWant, m_flHeading );
				flMax = m_pType->flReverseSpeed * m_flMaxSpeedScale;
			}
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
			if ( m_bReversing )
				flTargetSpeed = -flTargetSpeed;
		}
	}
	else
		m_bReversing = false;

	// other vehicles (V2d): it brakes for any vehicle its body would reach before it could stop, and gives way
	// by the right of way (GiveWay). Checked in the direction it WANTS to go, so it never sets off into one.
	if ( fabs( flTargetSpeed ) > 1.0f )
	{
		float flSign = ( flTargetSpeed > 0.0f ) ? 1.0f : -1.0f;
		float flStopDist = m_flSpeed * m_flSpeed / ( 2.0f * MAX( m_pType->flBrake, 1.0f ) );
		Vector vecEnd = vecPos + vecFwd * flSign * ( m_flHalfLength + flStopDist + 32.0f );
		if ( CDoDSPVehicle *pInWay = VehicleInTheWay( vecPos, vecEnd, m_flHalfWidth, VSPACE_DRIVING ) )
		{
			flTargetSpeed = 0.0f;
			flYawRate = 0.0f;
			GiveWay( pInWay );
		}
	}
	if ( m_hWaitFor.Get() && now - m_flWaitSeen > 0.5f )
	{
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: %s is out of the way\n", Label(), VehicleWho( m_hWaitFor.Get() ) );
		m_hWaitFor = NULL;
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
					Msg( "DOD-SP vehicle %s: can't go on - %s%s%s at %.0f %.0f %.0f\n", Label(), s_pszCorridorFail[why.eFail],
						 why.szWhat[0] ? ", " : "", why.szWhat, why.vecAt.x, why.vecAt.y, why.vecAt.z );
			}
		}
	}

	// heading, then the orientation and height the ground gives at the next spot
	m_flHeading = AngleNormalize( m_flHeading + flYawRate * dt );
	Vector vecNext = vecPos + vecFwd * ( m_flSpeed * dt );
	float flGroundZ;
	QAngle angWant = BuildAngles( m_flHeading, vecNext, &flGroundZ );
	// the lurch (owner: "from a complete stop to moving, with a lurch" - the models have no animation for it, only
	// 'idle'): the hull rocks on a damped spring driven by its acceleration - nose up pulling away, nose down
	// braking, a little overshoot - and by the main gun's recoil (FireMainGun kicks m_flSuspVel)
	{
		float flAccel = ( m_flSpeed - m_flPrevSpeed ) / dt;
		m_flPrevSpeed = m_flSpeed;
		float flTarget = clamp( flAccel / 100.0f * m_pType->flSuspAccel, -6.0f, 6.0f );
		float w = 2.0f * M_PI_F * MAX( m_pType->flSuspFreq, 0.1f );
		float flSpring = w * w * ( flTarget - m_flSuspPitch ) - 2.0f * m_pType->flSuspDamp * w * m_flSuspVel;
		m_flSuspVel += flSpring * dt;
		m_flSuspPitch = clamp( m_flSuspPitch + m_flSuspVel * dt, -8.0f, 8.0f );
		if ( fabs( m_flSuspPitch ) > 0.01f )
		{
			// about the hull's own sideways axis (model space), nose up = +
			float flMY = DEG2RAD( m_pType->flForwardYaw );
			Vector vecSide( -sinf( flMY ), cosf( flMY ), 0.0f );
			matrix3x4_t matHull, matRock, matOut;
			AngleMatrix( angWant, matHull );
			MatrixBuildRotationAboutAxis( vecSide, -m_flSuspPitch, matRock );
			ConcatTransforms( matHull, matRock, matOut );
			MatrixAngles( matOut, angWant );
		}
	}
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

//-----------------------------------------------------------------------------
// V2c: tactics - one owner for where a vehicle WANTS to be (the driving code gets it there, the turret code
// shoots). Once a second:
//   what it knows: its own sight (the turret's target) first, else what its side's soldiers within
//     VEHICLE_SPOTTER_RANGE are fighting (their enemy and where they last knew him) - no wallhacks.
//   offensive: closes to 600..1800 units, shoots on the move as well; with no enemy known it patrols the nodes.
//   defensive: holds spots 1500..(gun range) away with a shot, stops to shoot, backs off from what gets close,
//     never leaves its leash (2000 units from where it started unless the map says otherwise).
//   hold: stays; the turret fights. scripted: moves only on inputs.
// A firing spot is a graded node (it can drive there) in its range band from the enemy, inside its leash,
// with a clear line from turret height to the enemy - nearest first. It keeps a spot while it has its shot
// (no hunting for a better one), and keeps any new spot VEHICLE_SETTLE_TIME at least; six new spots inside
// 30 s prints an INVARIANT (dithering). Map orders (DriveTo inputs, console goto) come first: tactics wait
// till it arrives.
//-----------------------------------------------------------------------------
void CDoDSPVehicle::SetTactic( int eTactic, int eWhy )
{
	if ( eTactic == m_eTactic && eWhy == m_eTacticWhy )
		return;
	m_eTactic = eTactic;
	m_eTacticWhy = eWhy;
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s (%s): %s%s%s\n", Label(), s_pszBehaviors[m_eBehavior], s_pszTactics[eTactic],
			 s_pszTacticWhy[eWhy][0] ? " - " : "", s_pszTacticWhy[eWhy] );
}

bool CDoDSPVehicle::InBand( float flDist, float *pflMin, float *pflMax )
{
	float flGun = m_pType->gun.bHas ? m_pType->gun.flRange : m_pType->coax.flRange;
	if ( m_eBehavior == VBEHAVE_DEFENSIVE )
	{
		*pflMin = 1500.0f;
		*pflMax = MAX( 1600.0f, MIN( flGun * 0.8f, 4000.0f ) );
	}
	else
	{
		*pflMin = 600.0f;
		*pflMax = MIN( 1800.0f, MAX( flGun * 0.8f, 700.0f ) );
	}
	return flDist >= *pflMin && flDist <= *pflMax;
}

void CDoDSPVehicle::UpdateQuarry( void )
{
	float now = gpGlobals->curtime;
	// its own eyes first
	if ( m_hTarget.Get() && m_hTarget->IsAlive() )
	{
		m_hQuarry = m_hTarget.Get();
		m_vecQuarryPos = m_hTarget->WorldSpaceCenter();
		m_flQuarryTime = now;
		return;
	}
	// its side's soldiers nearby: what they are fighting, where they last knew it
	CAI_BaseNPC **ppAIs = g_AI_Manager.AccessAIs();
	float flBest = FLT_MAX;
	CBaseEntity *pBest = NULL;
	Vector vecBest;
	for ( int i = 0; i < g_AI_Manager.NumAIs(); i++ )
	{
		CAI_BaseNPC *pNPC = ppAIs[i];
		if ( !pNPC || !pNPC->IsAlive() || DoDSP_FactionOf( pNPC ) != m_eFaction )
			continue;
		if ( ( pNPC->GetAbsOrigin() - GetAbsOrigin() ).Length() > VEHICLE_SPOTTER_RANGE )
			continue;
		CBaseEntity *pEnemy = pNPC->GetEnemy();
		if ( !pEnemy || !pEnemy->IsAlive() || ( pEnemy->GetFlags() & FL_NOTARGET ) || !DoDSP_Hostile( m_eFaction, DoDSP_FactionOf( pEnemy ) ) )
			continue;
		Vector vecLKP = pNPC->GetEnemyLKP();
		float flDist = ( vecLKP - GetAbsOrigin() ).Length();
		if ( flDist < flBest )
		{
			flBest = flDist;
			pBest = pEnemy;
			vecBest = vecLKP;
		}
	}
	if ( pBest )
	{
		bool bNews = m_hQuarry.Get() != pBest || now - m_flQuarryTime > 2.0f;
		m_hQuarry = pBest;
		m_vecQuarryPos = vecBest;
		m_flQuarryTime = now;
		if ( bNews && dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: %s reported at %.0f %.0f %.0f\n", Label(), VehicleWho( pBest ), vecBest.x, vecBest.y, vecBest.z );
		return;
	}
	// forget a stale one
	if ( m_hQuarry.Get() && ( !m_hQuarry->IsAlive() || now - m_flQuarryTime > VEHICLE_FORGET_TIME ) )
		m_hQuarry = NULL;
}

int CDoDSPVehicle::ChooseFirePosition( const Vector &vecTarget, float flMin, float flMax )
{
	VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_flBodyHeight, m_pType->szName );
	if ( !pNav )
		return -1;
	struct Cand_t { float flScore; int i; };
	CUtlVector<Cand_t> cands;
	Vector vecMe = GetAbsOrigin();
	float flMid = 0.5f * ( flMin + flMax );
	for ( int i = 0; i < pNav->nNodes; i++ )
	{
		if ( !pNav->links[i].Count() )
			continue;
		const Vector &vecNode = g_pBigAINet->GetNode( i, false )->GetOrigin();
		float flToTarget = ( vecNode - vecTarget ).Length2D();
		if ( flToTarget < flMin || flToTarget > flMax )
			continue;
		float flToMe = ( vecNode - vecMe ).Length2D();
		if ( flToMe > VEHICLE_FIRE_NODE_SEARCH )
			continue;
		if ( m_flLeash > 0.0f && ( vecNode - m_vecHome ).Length2D() > m_flLeash )
			continue;
		// not where it stands (it's choosing because it has no shot from here), not a spot that gave no shot,
		// not another vehicle's spot
		if ( flToMe < m_flHalfLength * 1.5f || BadSpot( i ) || SpotTakenBy( vecNode ) )
			continue;
		Cand_t c = { flToMe + 0.5f * fabs( flToTarget - flMid ), i };
		cands.AddToTail( c );
	}
	// the best few by score get the (costlier) line-of-fire test - THE sight test the turret uses (LineOfFire),
	// from turret height. V2c test: a world-only trace here passed through the forest's trees (TRACE_WORLD_ONLY
	// skips static props), the turret's own test didn't, so it re-picked the same spot every 4 s.
	float flEyeZ = TurretHeight();
	for ( int k = 0; k < cands.Count() && k < 24; k++ )
	{
		int iMin = k;
		for ( int m = k + 1; m < cands.Count(); m++ )
			if ( cands[m].flScore < cands[iMin].flScore )
				iMin = m;
		Cand_t tmp = cands[k]; cands[k] = cands[iMin]; cands[iMin] = tmp;
		Vector vecEye = g_pBigAINet->GetNode( cands[k].i, false )->GetOrigin() + Vector( 0, 0, flEyeZ );
		if ( LineOfFire( vecEye, vecTarget, m_hQuarry.Get(), 0.0f ) )
			return cands[k].i;
	}
	return -1;
}

void CDoDSPVehicle::TacticsThink( void )
{
	float now = gpGlobals->curtime;
	if ( now < m_flNextTactic )
		return;
	m_flNextTactic = now + VEHICLE_TACTIC_INTERVAL;
	UpdateQuarry();
	m_bPreferReverse = false;
	if ( m_bScriptedGoal )
	{
		SetTactic( VTACTIC_SCRIPTED, VWHY_ORDERED );
		return;
	}
	if ( m_eBehavior == VBEHAVE_SCRIPTED )
	{
		SetTactic( VTACTIC_SCRIPTED, VWHY_NONE );
		return;
	}
	if ( m_eBehavior == VBEHAVE_HOLD )
	{
		SetTactic( VTACTIC_HOLD, m_hQuarry.Get() ? VWHY_SEES_ENEMY : VWHY_NO_ENEMY );
		return;
	}
	Vector vecMe = GetAbsOrigin();
	if ( !m_hQuarry.Get() )
	{
		m_iFireNode = -1;
		if ( m_eBehavior == VBEHAVE_DEFENSIVE )
		{
			// keep home (a failed route waits before trying again - no OnNoRoute every second)
			if ( !m_bHasGoal && now >= m_flRoamWaitUntil && ( vecMe - m_vecHome ).Length2D() > 256.0f )
			{
				DriveTo( m_vecHome, false );
				m_flRoamWaitUntil = now + 5.0f;
			}
			SetTactic( VTACTIC_ROAM, VWHY_NO_ENEMY );
			return;
		}
		// patrol: a random node within reach, a pause at each
		if ( !m_bHasGoal && now >= m_flRoamWaitUntil )
		{
			VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_flBodyHeight, m_pType->szName );
			if ( pNav && pNav->nNodes > 0 )
			{
				for ( int tries = 0; tries < 12; tries++ )
				{
					int i = random->RandomInt( 0, pNav->nNodes - 1 );
					if ( !pNav->links[i].Count() )
						continue;
					const Vector &vecNode = g_pBigAINet->GetNode( i, false )->GetOrigin();
					float flDist = ( vecNode - vecMe ).Length2D();
					if ( flDist < 512.0f || flDist > VEHICLE_ROAM_RADIUS )
						continue;
					if ( m_flLeash > 0.0f && ( vecNode - m_vecHome ).Length2D() > m_flLeash )
						continue;
					if ( SpotTakenBy( vecNode ) )
						continue;
					if ( DriveTo( vecNode, false ) )
						break;
				}
			}
			m_flRoamWaitUntil = now + random->RandomFloat( 4.0f, 8.0f );
		}
		if ( m_bHasGoal )
			m_flRoamWaitUntil = now + random->RandomFloat( 4.0f, 8.0f );		// the pause starts when it arrives
		SetTactic( VTACTIC_ROAM, VWHY_NO_ENEMY );
		return;
	}

	// an enemy is known
	bool bSees = m_hTarget.Get() == m_hQuarry.Get() && m_hTarget.Get() != NULL;
	float flDist = ( m_vecQuarryPos - vecMe ).Length2D();
	float flMin, flMax;
	bool bInBand = InBand( flDist, &flMin, &flMax );
	// defensive: what gets close is backed away from, front to it
	m_bPreferReverse = ( m_eBehavior == VBEHAVE_DEFENSIVE );
	if ( bSees && ( bInBand || ( m_eBehavior == VBEHAVE_OFFENSIVE && flDist < flMin ) ) )
	{
		// it has its shot: stay (an offensive tank this close doesn't back off - it's winning)
		if ( m_bHasGoal && m_eBehavior == VBEHAVE_DEFENSIVE )
			Stop();
		if ( m_bHasGoal && m_eBehavior == VBEHAVE_OFFENSIVE && flDist <= flMax )
			Stop();
		m_iFireNode = -1;
		SetTactic( VTACTIC_FIGHT, VWHY_SEES_ENEMY );
		return;
	}
	int eWhy = !bSees ? ( m_hTarget.Get() ? VWHY_NO_SHOT : VWHY_REPORTED ) : ( flDist > flMax ? VWHY_TOO_FAR : VWHY_TOO_CLOSE );
	// still on the way to a spot, or just got there: keep it a while (no dithering)
	if ( ( m_bHasGoal && m_iFireNode >= 0 ) || now < m_flNoRepositionUntil )
	{
		SetTactic( m_iFireNode >= 0 ? VTACTIC_REPOSITION : VTACTIC_FIGHT, m_bHasGoal ? eWhy : VWHY_SETTLING );
		return;
	}
	// it got to the spot it chose and still has no shot from there: don't pick it again for a while (else it
	// goes back and forth between two spots that pass the planning test but give no shot)
	if ( m_iFireNode >= 0 && !m_bHasGoal && !bSees &&
		 ( g_pBigAINet->GetNode( m_iFireNode, false )->GetOrigin() - vecMe ).Length2D() < m_flHalfLength * 1.5f )
	{
		m_BadSpots[m_iBadSpot].iNode = m_iFireNode;
		m_BadSpots[m_iBadSpot].flUntil = now + VEHICLE_BADSPOT_TIME;
		m_iBadSpot = ( m_iBadSpot + 1 ) % ARRAYSIZE( m_BadSpots );
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: no shot from the spot it chose (node %d) - not picking it again for %.0f s\n", Label(), m_iFireNode,
				 VEHICLE_BADSPOT_TIME );
	}
	int iNode = ChooseFirePosition( m_vecQuarryPos, flMin, flMax );
	if ( iNode >= 0 && DriveTo( g_pBigAINet->GetNode( iNode, false )->GetOrigin(), false ) )
	{
		m_iFireNode = iNode;
		m_flNoRepositionUntil = now + VEHICLE_SETTLE_TIME;
		// dithering guard: six new spots in 30 s
		m_flRepositions[m_iReposition] = now;
		m_iReposition = ( m_iReposition + 1 ) % ARRAYSIZE( m_flRepositions );
		if ( now - m_flRepositions[m_iReposition] < 30.0f )
			Warning( "INVARIANT DOD-SP vehicle %s: %d new firing spots in %.0f s - it can't settle (enemy at %.0f %.0f %.0f, %s)\n",
					 Label(), ARRAYSIZE( m_flRepositions ), now - m_flRepositions[m_iReposition],
					 m_vecQuarryPos.x, m_vecQuarryPos.y, m_vecQuarryPos.z, s_pszTacticWhy[eWhy] );
		SetTactic( VTACTIC_REPOSITION, eWhy );
		return;
	}
	m_iFireNode = -1;
	// no spot with a shot at its range: offensive goes to where the enemy was; defensive waits, turret on him
	if ( m_eBehavior == VBEHAVE_OFFENSIVE && !bSees && !m_bHasGoal && flDist > 256.0f )
	{
		if ( DriveTo( m_vecQuarryPos, false ) )
			m_flNoRepositionUntil = now + VEHICLE_SETTLE_TIME;
		SetTactic( VTACTIC_HUNT, VWHY_NO_FIRE_SPOT );
		return;
	}
	SetTactic( bSees ? VTACTIC_FIGHT : VTACTIC_HUNT, VWHY_NO_FIRE_SPOT );
}

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
		 "main gun %s, coax %s\n", Label(), m_pType->szName, DoDSP_FactionName( m_eFaction ), m_flTurretFwdOffset,
		 m_iPitchPose >= 0 ? "" : "MISSING ", m_flPoseMin, m_flPoseMax, m_flPitchSign > 0 ? "+" : "-",
		 m_iMuzzleAttach > 0 ? "found" : "MISSING", m_pType->gun.bHas ? "yes" : "no", m_pType->coax.bHas ? "yes" : "no" );
	if ( m_iMuzzleAttach <= 0 )
		Warning( "INVARIANT DOD-SP vehicle %s: turret model %s has no \"muzzle\" attachment - shots start at the turret's middle\n",
				 Label(), m_pType->szTurretModel );
	if ( m_iPitchPose < 0 )
		Warning( "INVARIANT DOD-SP vehicle %s: turret model %s has no \"barrel_pitch\" pose - the barrel can't elevate\n",
				 Label(), m_pType->szTurretModel );
	else if ( fabs( vecHigh.z - vecLow.z ) < 1.0f )
		Warning( "INVARIANT DOD-SP vehicle %s: barrel_pitch %.0f..%.0f doesn't move the muzzle (%.1f units) - the barrel can't elevate\n",
				 Label(), m_flPoseMin, m_flPoseMax, fabs( vecHigh.z - vecLow.z ) );
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

// What blocks a vehicle's sight and its shells (one owner, LineOfFire): the world, brush entities, static props
// (trees, rocks, houses - TRACE_WORLD_ONLY skips them), and vehicles. Not people or loose props: an enemy behind
// a soldier is still seen (FriendlyInTheWay keeps friends out of the line).
class CTraceFilterVehicleSight : public CTraceFilter
{
public:
	CTraceFilterVehicleSight( CBaseEntity *pSelf ) : m_pSelf( pSelf ) {}
	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask )
	{
		if ( staticpropmgr->IsStaticProp( pHandleEntity ) )
			return true;
		CBaseEntity *pEnt = EntityFromEntityHandle( pHandleEntity );
		if ( !pEnt || pEnt == m_pSelf || pEnt->GetRootMoveParent() == m_pSelf )
			return false;
		return pEnt->IsWorld() || pEnt->IsBSPModel() || dynamic_cast<CDoDSPVehicle *>( pEnt->GetRootMoveParent() ) != NULL;
	}
	virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
private:
	CBaseEntity *m_pSelf;
};

// THE sight / line-of-fire test (V2d): firing spots are chosen with it, the turret sees with it, the guns fire
// only when it passes from the muzzle. Clear = nothing in the way, or it reaches pTarget, or it ends within
// flNear of vecTo (a shell landing that close still does its work). V2c test: three different tests (a
// world-only trace for spots, MASK_SHOT for sight, none from the muzzle) disagreed - a tank went back and forth
// between spots with no shot, and fired five shells into the same static prop.
bool CDoDSPVehicle::LineOfFire( const Vector &vecFrom, const Vector &vecTo, CBaseEntity *pTarget, float flNear, trace_t *pTr )
{
	CTraceFilterVehicleSight filter( this );
	trace_t tr;
	UTIL_TraceLine( vecFrom, vecTo, MASK_SHOT, &filter, &tr );
	if ( pTr )
		*pTr = tr;
	if ( tr.fraction >= 0.99f && !tr.startsolid )
		return true;
	if ( pTarget && tr.m_pEnt && ( tr.m_pEnt == pTarget || tr.m_pEnt->GetRootMoveParent() == pTarget ) )
		return true;
	return flNear > 0.0f && !tr.startsolid && ( tr.endpos - vecTo ).Length() <= flNear;
}

bool CDoDSPVehicle::CanSee( CBaseEntity *pTarget, Vector *pvecPoint )
{
	Vector vecFrom = AimPivot();
	// the target's middle or its eyes: the point it sees is the point it aims at
	Vector vecPoints[2] = { pTarget->WorldSpaceCenter(), pTarget->EyePosition() };
	for ( int i = 0; i < 2; i++ )
	{
		if ( LineOfFire( vecFrom, vecPoints[i], pTarget, 0.0f ) )
		{
			if ( pvecPoint )
				*pvecPoint = vecPoints[i];
			return true;
		}
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
	UTIL_TraceLine( vecFrom, vecFrom + vecDir * flRange, MASK_SHOT, &filter, &tr );	// dodsp-lint: not a sight line (friends on the line: hits people on purpose)
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
	float flSpread = DEG2RAD( m_pType->gun.flSpread ) * 0.5f * ( fabs( m_flSpeed ) < 15.0f ? 1.0f : 3.0f );
	vecDir += Vector( random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ) );
	VectorNormalize( vecDir );
	Vector vecMuzzle = MuzzlePos( vecDir );
	trace_t tr;
	CTraceFilterSkipTwoEntities filter( this, m_hTurret.Get(), COLLISION_GROUP_NONE );
	UTIL_TraceLine( vecMuzzle, vecMuzzle + vecDir * m_pType->gun.flRange, MASK_SHOT, &filter, &tr );	// dodsp-lint: not a sight line (the shell's own flight)
	QAngle angDir;
	VectorAngles( vecDir, angDir );
	g_pEffects->MuzzleFlash( vecMuzzle, angDir, 6.0f, MUZZLEFLASH_TYPE_DEFAULT );
	if ( m_pType->gun.szSound[0] )
		EmitSound( m_pType->gun.szSound );
	UTIL_ScreenShake( vecMuzzle, 6.0f, 150.0f, 0.6f, 1024.0f, SHAKE_START );
	// recoil: the hull rocks away from the shot (nose up when firing ahead, down when firing behind)
	m_flSuspVel += m_pType->flRecoil * 2.0f * M_PI_F * m_pType->flSuspFreq * cosf( DEG2RAD( m_flTurretYaw + m_flTurretFwdOffset ) );
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
		Msg( "DOD-SP vehicle %s: main gun fired at %s - hit %s %.0f units out\n", Label(),
			 VehicleWho( m_hTarget.Get() ), tr.fraction < 1.0f ? VehicleHitName( tr ) : "nothing",
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
				Msg( "DOD-SP vehicle %s (%s): target %s (%s) at %.0f units\n", Label(), DoDSP_FactionName( m_eFaction ),
					 VehicleWho( m_hTarget.Get() ), DoDSP_FactionName( DoDSP_FactionOf( m_hTarget.Get() ) ),
					 ( m_hTarget->WorldSpaceCenter() - AimPivot() ).Length() );
		}
	}
	CBaseEntity *pTarget = m_hTarget.Get();

	// turn the turret and raise the barrel toward it (in the hull's frame), else back to the front
	float flWantYaw = 0.0f, flWantPitch = 0.0f;
	Vector vecAimPoint = vec3_origin, vecSeenPoint = vec3_origin;
	bool bSeen = pTarget && CanSee( pTarget, &vecSeenPoint );
	bool bAimAtQuarry = !pTarget && m_hQuarry.Get() && gpGlobals->curtime - m_flQuarryTime < VEHICLE_FORGET_TIME;
	if ( pTarget || bAimAtQuarry )
	{
		// a target in sight (at the point it sees); else where its enemy was last known (ready when he shows)
		vecAimPoint = pTarget ? ( bSeen ? vecSeenPoint : pTarget->WorldSpaceCenter() ) : m_vecQuarryPos;
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
	if ( !bSeen )
		return;
	// the line from the MUZZLE (it sits forward of and below the pivot the turret sees from): the shell must
	// reach the target or land within half its blast radius of it; the coax must reach it
	Vector vecMuzzleNow = MuzzlePos( vecBarrel );
	trace_t trShot;
	bool bShellClear = LineOfFire( vecMuzzleNow, vecAimPoint, pTarget, m_pType->gun.flRadius * 0.5f, &trShot );
	bool bBulletClear = bShellClear && LineOfFire( vecMuzzleNow, vecAimPoint, pTarget, 0.0f );
	CBaseEntity *pFriend = NULL;
	// main gun: at anything hostile, when the barrel is on it. A defensive vehicle stops to shoot; an offensive
	// one shoots on the move too (FireMainGun spreads moving shots wider)
	bool bStill = fabs( m_flSpeed ) < 15.0f;
	if ( m_pType->gun.bHas && now >= m_flNextGun && flErr <= VEHICLE_GUN_CONE && flDist <= m_pType->gun.flRange &&
		 ( bStill || m_eBehavior != VBEHAVE_DEFENSIVE ) )
	{
		if ( !bShellClear )
		{
			if ( dodsp_vehicle_debug.GetBool() && now >= m_flNextHoldMsg )
			{
				m_flNextHoldMsg = now + 3.0f;
				Msg( "DOD-SP vehicle %s: holding fire - no line from the muzzle to %s (%s %.0f units out, %.0f short)\n", Label(),
					 VehicleWho( pTarget ), VehicleHitName( trShot ), ( trShot.endpos - vecMuzzleNow ).Length(), ( vecAimPoint - trShot.endpos ).Length() );
			}
			m_flNextGun = now + 0.5f;		// look again shortly (the turret or the target moves)
		}
		else if ( FriendlyInTheWay( MuzzlePos( vecBarrel ), vecBarrel, flDist + 64.0f, m_pType->gun.flRadius, &pFriend ) )
		{
			if ( dodsp_vehicle_debug.GetBool() )
				Msg( "DOD-SP vehicle %s: holding fire - %s in the way\n", Label(), VehicleWho( pFriend ) );
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
			if ( bBulletClear && !FriendlyInTheWay( MuzzlePos( vecBarrel ), vecBarrel, flDist + 64.0f, 0.0f, &pFriend ) )
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
		Warning( "DOD-SP vehicle %s: SetTarget '%s' - no such entity\n", Label(), inputdata.value.String() );
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
	Msg( "DOD-SP vehicle %s (%s, %.0f x %.0f): at %.0f %.0f %.0f, heading %.0f, speed %.0f, %s%s, route %d/%d\n", Label(),
		 m_pType ? m_pType->szName : "?", m_flHalfLength * 2.0f, m_flHalfWidth * 2.0f, p.x, p.y, p.z, m_flHeading, m_flSpeed,
		 m_bHasGoal ? "driving" : "parked", m_eStuck != VSTUCK_NONE ? ( m_eStuck == VSTUCK_WORLD ? ", wall ahead" : ", was stuck" ) : "",
		 m_iRoute, m_Route.Count() );
	Msg( "    %s: %s%s%s, enemy known: %s (%.0f s ago)%s\n", s_pszBehaviors[m_eBehavior], s_pszTactics[m_eTactic],
		 s_pszTacticWhy[m_eTacticWhy][0] ? " - " : "", s_pszTacticWhy[m_eTacticWhy],
		 m_hQuarry.Get() ? VehicleWho( m_hQuarry.Get() ) : "none", m_hQuarry.Get() ? gpGlobals->curtime - m_flQuarryTime : 0.0f,
		 m_bReversing ? ", reversing" : "" );
	if ( m_hWaitFor.Get() )
		Msg( "    waiting for %s (%.0f s)\n", VehicleWho( m_hWaitFor.Get() ), gpGlobals->curtime - m_flWaitSince );
	Msg( "    side %s, target %s%s, turret %.0f deg / barrel %.0f deg, main gun %s\n", DoDSP_FactionName( m_eFaction ),
		 m_hTarget.Get() ? VehicleWho( m_hTarget.Get() ) : "none", m_bHoldFire ? " (holding fire)" : "", m_flTurretYaw, m_flTurretPitch,
		 !m_pType || !m_pType->gun.bHas ? "none" : ( gpGlobals->curtime >= m_flNextGun ? "loaded" : "reloading" ) );
}

// Fine-tuning the script's estimate in game: move the turret, print the line for the script
void CDoDSPVehicle::SetTurretOffset( const Vector &vecOffset )
{
	if ( !m_hTurret.Get() )
	{
		Msg( "DOD-SP vehicle %s (%s) has no turret\n", Label(), m_pType ? m_pType->szName : "?" );
		return;
	}
	m_hTurret->SetLocalOrigin( vecOffset );
	Msg( "DOD-SP vehicle %s: turret at %g %g %g - for scripts/dodsp_vehicles.txt, under \"%s\":\n\t\t\"turret_offset\"\t\t\"%g %g %g\"\n",
		 Label(), vecOffset.x, vecOffset.y, vecOffset.z, m_pType->szName, vecOffset.x, vecOffset.y, vecOffset.z );
}

void CDoDSPVehicle::InputDriveTo( inputdata_t &inputdata )
{
	CBaseEntity *pGoal = gEntList.FindEntityByName( NULL, inputdata.value.String(), NULL, inputdata.pActivator, inputdata.pCaller );
	if ( pGoal )
		DriveTo( pGoal->GetAbsOrigin() );
	else
		Warning( "DOD-SP vehicle %s: DriveTo - no entity named '%s'\n", Label(), inputdata.value.String() );
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
	UTIL_TraceLine( pPlayer->EyePosition(), pPlayer->EyePosition() + vecFwd * 8192.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr );	// dodsp-lint: not a sight line (the player's aim (console))
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
