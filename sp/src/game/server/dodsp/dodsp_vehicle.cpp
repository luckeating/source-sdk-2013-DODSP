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
#include "dodsp_vehicle_armour.h"
#include "soundchars.h"
#include "smoke_trail.h"
#include "itempents.h"
#include "props_shared.h"
#include "dodsp/dodsp_vehicle_wheels.h"
#include "physics_shared.h"
#include "tier1/fmtstr.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define DODSP_VEHICLE_SCRIPT	"scripts/dodsp_vehicles.txt"
#define DODSP_VEHICLE_CLASS_PREFIX	"dodsp_vehicle_"		// + a type name = that type's own Hammer class (V4j)
// INVARIANT (V4e): the type script and this server.dll come from ONE build. The script's "script_version" must equal
// this; the lint checks the repo copy against it, the loader checks the owner's copy (V4e's first run: the old script
// had no hatches, every crewman got out beside the tank and nothing at bail time said why). Bump both together
// whenever the code starts needing a new key in the script.
#define DODSP_VEHICLE_SCRIPT_VERSION	"H3"
static char s_szVehicleScriptVersion[32] = "not loaded";

// V4h: how a shell's strike on the armour went - what HitFeedback shows and plays (name table guarded below)
enum VehicleHitFx_t { VHITFX_IN, VHITFX_STOPPED, VHITFX_RICOCHET, NUM_VHITFX };
static const char *s_pszHitSound[] = { "DoDSP_Vehicle.HitPenetrate", "DoDSP_Vehicle.HitStopped", "DoDSP_Vehicle.HitRicochet" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszHitSound ) == NUM_VHITFX );
#define VEHICLE_HITFX_MIN_CALIBRE	15.0f		// mm: shells show their strike, bullets keep the engine's impacts
#define VEHICLE_HIT_SOUNDLEVEL		SNDLVL_120dB
#define VEHICLE_THINK			0.05f
#define VEHICLE_STEER_RATE		90.0f		// H1: degrees/s the steered wheels turn at (a driver winding the wheel)
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
	VWHY_IMMOBILE,			// V4: engine / tracks / driver gone - it fights where it stands
	NUM_VWHYS
};
static const char *s_pszTacticWhy[] = { "", "ordered", "no enemy known", "enemy in sight and range", "enemy too far",
	"enemy too close", "no shot from here", "reported by its side", "no node with a shot", "leash", "settling", "immobilised" };
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

// V4: what a vehicle is now, what a penetration broke, why the crew got out (reason codes, printed by name)
enum VehicleState_t
{
	VSTATE_FIGHTING,
	VSTATE_BURNING,			// out of the fight: burns, the crew gets out, it cooks off or burns out
	VSTATE_ABANDONED,		// the crew left (or is dead); the vehicle is intact
	VSTATE_WRECKED,			// the dead model
	NUM_VSTATES
};
static const char *s_pszVehicleState[] = { "fighting", "burning", "abandoned", "wrecked" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszVehicleState ) == NUM_VSTATES );

enum VehicleCrit_t
{
	VCRIT_NONE,
	VCRIT_AMMO,				// ammunition: may set it burning (DoDSPArmour_t::flAmmoFire)
	VCRIT_ENGINE,			// immobilised; may set it burning (flFuelFire)
	VCRIT_DRIVE,			// transmission / tracks / wheels: immobilised
	VCRIT_TURRET,			// turret ring jammed: the gun points where it pointed
	VCRIT_GUN,				// main gun out
	VCRIT_CREW,				// a crewman hit
	NUM_VCRITS
};
static const char *s_pszCrit[] = { "nothing vital", "ammunition", "engine", "transmission / tracks", "turret ring", "main gun", "crew" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszCrit ) == NUM_VCRITS );

enum VehicleBail_t
{
	VBAIL_NONE,
	VBAIL_FIRE,				// it's burning
	VBAIL_CREW,				// one man can't fight a tank
	VBAIL_HELPLESS,			// can't move and can't fight
	VBAIL_UNDER_FIRE,		// can't move and keeps getting penetrated
	VBAIL_KNOCKED_OUT,
	VBAIL_IMMOBILE_HIT,		// V4d: a round got in and left it unable to move - a sitting target
	VBAIL_WRECKED_INSIDE,	// V4d: what got in wrecked most of the inside (structure <= VEHICLE_BAIL_STRUCTURE)
	NUM_VBAILS
};
static const char *s_pszBail[] = { "", "it's burning", "too few crew left to fight it", "it can't move and can't fight",
	"it can't move and keeps getting penetrated", "knocked out", "a round got in and it can't move", "the inside is wrecked" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszBail ) == NUM_VBAILS );

// V4b: the German soldier's bail-out (npc_german_soldier.cpp - one owner for how a man climbs out and jumps off)
extern bool GermanSoldier_BailOut( CBaseEntity *pEnt, const Vector &vecHatch, const Vector &vecGround, const Vector &vecRun, float flYaw );
// V3 riders (npc_german_soldier.cpp owns how a man rides and gets off; the vehicle owns when)
extern bool GermanSoldier_BeginRide( CBaseEntity *pEnt, CBaseEntity *pVehicle, const Vector &vecSeat, float flYaw, const char *pszIdle, const char *pszGetout, const char *pszFidget );
extern int GermanSoldier_RideGetOut( CBaseEntity *pEnt, bool bForce );	// 1 going, 0 can't, -1 his landing spot is taken
extern void GermanSoldier_SetSeatDeath( CBaseEntity *pEnt, const char *pszDeath );	// H2b: he dies in that seat with it
extern void GermanSoldier_SetRideWalk( CBaseEntity *pEnt, const Vector *pvecWalk, float flYaw );	// H3: his get-out starts there (vehicle space), NULL = his seat
extern void GermanSoldier_SetRideRally( CBaseEntity *pEnt, const Vector *pvecRally );		// H3: once down he runs to this spot (vehicle space)
extern void GermanSoldier_RideAbort( CBaseEntity *pEnt );
extern int GermanSoldier_RideState( CBaseEntity *pEnt );		// 0 off, 1 seated, 2 getting off
#define VEHICLE_MAX_SEATS		8
#define VEHICLE_DOOR_KEYS		32			// H3: keys in a "doors" curve
#define VEHICLE_MAX_CREW_SEATS	2			// H2: crew members you can see in their seats (the halftrack's driver)
#define VEHICLE_RIDERS_CONTACT	1200.0f		// riders get off when an enemy it knows of is this close (dismount "contact")
#define VEHICLE_RIDERS_SPAWN	0.2f		// riders are put aboard this long after the vehicle activates
#define VEHICLE_RIDER_WAIT		0.25f		// a rider whose landing spot is taken tries again this often ...
#define VEHICLE_RIDER_WAIT_MAX	3.0f		// ... for this long, then goes anyway (out of the fight: at once)
enum VehicleRiders_t { VRIDERS_NONE, VRIDERS_ABOARD, VRIDERS_STOPPING, VRIDERS_GETTING_OFF, VRIDERS_OFF, NUM_VRIDERS };
static const char *s_pszRiders[] = { "none", "aboard", "stopping to let them off", "getting off", "off" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszRiders ) == NUM_VRIDERS );
enum VehicleDismount_t { VDISMOUNT_CONTACT, VDISMOUNT_ORDER, NUM_VDISMOUNT };
static const char *s_pszDismount[] = { "contact", "order" };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszDismount ) == NUM_VDISMOUNT );

#define VEHICLE_COOKOFF_KG		2.0f		// an ammunition cook-off goes up like this much explosive (game value)
#define VEHICLE_PEN_MEMORY		15.0f
#define VEHICLE_BAIL_STRUCTURE	0.3f		// V4d: a crew gets out once what's got in leaves this fraction of the structure		// two penetrations this close together on an immobile vehicle: the crew gets out

// where an armour box was hit (one owner: CDoDSPVehicle::ShellHitBox)
struct VehicleHit_t
{
	int		ePart, eFace;		// ArmourPart_t / ArmourFace_t
	Vector	vecPoint;
	Vector	vecNormal;			// the plate's outward normal, leaned back by its slope (world)
	float	t;					// distance along the ray
};

//-----------------------------------------------------------------------------
// Vehicle types (scripts/dodsp_vehicles.txt)
//-----------------------------------------------------------------------------
#define VEHICLE_MAX_MGS		4
static const char *s_pszMGMount[] = { "turret", "barrel", "hull" };

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
	// main gun (V4: real ammunition - "ap" for vehicles, "he" for everything else; a magazine fed at a cyclic rate
	// for autocannons, one round + "reload" for the rest; "rounds" carried, 0 = never runs out)
	struct Gun_t { bool bHas; float flReload, flRange, flSpread; int nMagazine; float flCyclic; int nRounds; DoDSPShell_t ap, he; char szSound[64]; };
	Gun_t	gun;
	// V3e: the machine guns, each on its REAL mount (CoD2's tags, dodsp_tools/generated/cod2_vehicle_tags.txt): turret
	// (turret model space, turns with it), barrel (also rises with the gun about vecPivot), hull (hull model space: a
	// ball mount aiming on its own within flArc degrees each side and flPitchMin..Max, manned by iCrew). Bursts at
	// infantry. (Before V3e one "coax" fired from the main gun's muzzle - the Tiger's 3.7 m out, nowhere near its MG.)
	enum { MG_TURRET, MG_BARREL, MG_HULL, NUM_MG_MOUNTS };
	struct MG_t { char szName[16]; int eMount; Vector vecMuzzle, vecPivot; float flArc, flPitchMin, flPitchMax; int iCrew;
				  float flDamage, flRate, flBurst, flPause, flRange, flSpread; char szSound[64]; };
	MG_t	mgs[VEHICLE_MAX_MGS];
	int		nMGs;
	float	flMGRange;				// the longest of them
	float	flSuspAccel;			// degrees the hull pitches per 100 units/s^2 of acceleration (the lurch)
	float	flSuspFreq, flSuspDamp;	// its spring: rocks at this many Hz, this much damping (0..1)
	float	flRecoil;				// the main gun kicks the hull back this many degrees
	float	flSuspRoll;				// H1 sprung bodies: degrees the body rolls outward per 100 units/s^2 sideways
	DoDSPArmour_t armour;			// V4 ("armour" block; invalid = it can't be hurt, and the load says so)
	bool	bVariant;				// has a "base" (a paint scheme): the pen chart lists the base only
	char	szDeadModel[MAX_PATH];	// V4: the wreck
	char	szEngineIdle[64], szEngineMove[64], szTurretTurn[64], szTurretStop[64], szReload[64], szExplode[64];
	// V3: a rider's seat - where (model space), facing (yaw on the vehicle), his looping idle and his get-out (both
	// sequences of npc_ww2_anims_ride), and how long after the dismount order he goes
	// H3: "walk" "x y z yaw" = his get-out starts there, not on his seat (the halftrack's aisle men: CoD2's guy02 walk
	// from tag_out) - he stands into it from his seat over the get-out's fadein (npc_german_soldier.cpp RidePosition);
	// "rally" "x y z" = once down he runs to this spot beside the vehicle (CoD2's tag_walker*), then his AI takes over;
	// "death" = killed in his seat he plays it there and stays (like the crew, H2b)
	struct Seat_t { Vector vecOrigin; float flYaw; char szIdle[64], szGetout[64], szFidget[64], szDeath[64]; float flDelay;	// V3e: fidget = played now and then while seated
					Vector vecWalk; float flWalkYaw; bool bWalk; Vector vecRally; bool bRally; };
	CUtlVector<Seat_t> seats;
	// H2: a crew member you can SEE in his seat ("crew_seats": role -> origin, yaw, idle, steer) - he IS that crew role:
	// shot in his seat = the role is dead; the role killed inside = so is he; abandoned = a crewman climbs out of the
	// role's hatch and he leaves the seat. steer 1 = the idle is a blend on the pose parameter "steer" (-1 full right ..
	// 1 full left) and follows the steering angle.
	struct CrewSeat_t { int eRole; Vector vecOrigin; float flYaw; char szIdle[64], szDeath[64]; bool bSteer; };	// H2b: death = played in the seat, he stays there
	CUtlVector<CrewSeat_t> crewSeats;
	// V4e: where each crew role gets out ("hatches": role -> "turret x y z" in the turret model's space, or "hull x y z"
	// in the hull's) = the roof his feet end up on, measured from the meshes. Roles at the same spot share a hatch
	// (iShare = the first role there): the next man waits until the one before has left it.
	struct Hatch_t { bool bHas, bTurret; Vector vecPos; int iShare; };
	Hatch_t	hatches[NUM_CREW_ROLES];
	// H3: doors the riders get out through ("doors" "curve": frame (30 fps) / degrees from the dismount order - CoD2's
	// halftrack_halftrack_jumpout). Which pose parameter and how far it opens are the model's ($keyvalues "dodsp_doors")
	struct Doors_t { int nKeys; float flTime[VEHICLE_DOOR_KEYS], flDeg[VEHICLE_DOOR_KEYS]; };
	Doors_t	doors;
};
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszMGMount ) == VehicleType_t::NUM_MG_MOUNTS );
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
	Q_strncpy( s_szVehicleScriptVersion, pKV->GetString( "script_version", "older than V4e" ), sizeof( s_szVehicleScriptVersion ) );
	if ( Q_strcmp( s_szVehicleScriptVersion, DODSP_VEHICLE_SCRIPT_VERSION ) )
		Warning( "INVARIANT DOD-SP vehicles: %s is version %s, this server.dll needs %s - copy scripts\\dodsp_vehicles.txt from "
				 "the same build as server.dll (missing keys fall back: crews get out beside the vehicle, ...)\n",
				 DODSP_VEHICLE_SCRIPT, s_szVehicleScriptVersion, DODSP_VEHICLE_SCRIPT_VERSION );
	else
		Msg( "DOD-SP vehicles: server.dll and %s are both %s (one build)\n", DODSP_VEHICLE_SCRIPT, DODSP_VEHICLE_SCRIPT_VERSION );
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
		t->flSuspRoll = pType->GetFloat( "susp_roll", 1.0f );
		t->flTurretRate = pType->GetFloat( "turret_rate", 30.0f );
		t->flPitchRate = pType->GetFloat( "pitch_rate", 15.0f );
		KeyValues *pGun = pType->FindKey( "gun" );
		t->gun.bHas = pGun != NULL;
		t->gun.flReload = pGun ? pGun->GetFloat( "reload", 5.0f ) : 0.0f;
		t->gun.nMagazine = pGun ? MAX( pGun->GetInt( "magazine", 1 ), 1 ) : 1;
		t->gun.flCyclic = pGun ? pGun->GetFloat( "cyclic", 0.0f ) : 0.0f;
		t->gun.nRounds = pGun ? MAX( pGun->GetInt( "rounds", 0 ), 0 ) : 0;
		if ( pGun )
		{
			char szErr[160];
			if ( !DoDSP_ParseShell( pGun->FindKey( "ap" ), t->gun.ap, false, szErr, sizeof( szErr ) ) )
				Warning( "INVARIANT DOD-SP vehicles: type '%s' gun \"ap\": %s - no AP shells\n", t->szName, szErr );
			if ( !DoDSP_ParseShell( pGun->FindKey( "he" ), t->gun.he, true, szErr, sizeof( szErr ) ) )
				Warning( "INVARIANT DOD-SP vehicles: type '%s' gun \"he\": %s - no HE shells\n", t->szName, szErr );
			if ( !t->gun.ap.bValid && !t->gun.he.bValid )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has a \"gun\" with neither \"ap\" nor \"he\" ammunition - main gun off\n", t->szName );
				t->gun.bHas = false;
			}
			if ( pGun->FindKey( "damage" ) || pGun->FindKey( "radius" ) )
				Warning( "INVARIANT DOD-SP vehicles: type '%s' gun still has V2's flat \"damage\"/\"radius\" - ignored (V4: the shells' real numbers decide)\n", t->szName );
			if ( t->gun.nMagazine > 1 && t->gun.flCyclic <= 0.0f )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' gun has a %d-round magazine but no \"cyclic\" rate - 120 rpm\n", t->szName, t->gun.nMagazine );
				t->gun.flCyclic = 120.0f;
			}
		}
		{
			char szErr[160];
			if ( !DoDSP_ParseArmour( pType->FindKey( "armour" ), t->armour, szErr, sizeof( szErr ) ) )
				Warning( "INVARIANT DOD-SP vehicles: type '%s' armour: %s - it can't be damaged\n", t->szName, szErr );
		}
		Q_strncpy( t->szDeadModel, pType->GetString( "dead_model", "" ), sizeof( t->szDeadModel ) );
		t->bVariant = pRawType->GetString( "base", "" )[0] != 0;
		t->gun.flRange = pGun ? pGun->GetFloat( "range", 6000.0f ) : 0.0f;
		t->gun.flSpread = pGun ? pGun->GetFloat( "spread", 0.5f ) : 0.0f;
		Q_strncpy( t->gun.szSound, pGun ? pGun->GetString( "sound", "DoDSP_Vehicle.GunFire1" ) : "", sizeof( t->gun.szSound ) );
		// V3e: the machine guns on their real mounts
		t->nMGs = 0;
		t->flMGRange = 0.0f;
		if ( pType->FindKey( "coax" ) )
			Warning( "INVARIANT DOD-SP vehicles: type '%s' has the old \"coax\" block (before V3e: it fired from the main gun's muzzle) - "
					 "ignored, the type has no machine gun; use \"mgs\" (the script and server.dll come from one build)\n", t->szName );
		KeyValues *pMGs = pType->FindKey( "mgs" );
		for ( KeyValues *pMG = pMGs ? pMGs->GetFirstTrueSubKey() : NULL; pMG; pMG = pMG->GetNextTrueSubKey() )
		{
			if ( t->nMGs >= VEHICLE_MAX_MGS )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has more than %d machine guns - the rest are ignored\n", t->szName, VEHICLE_MAX_MGS );
				break;
			}
			VehicleType_t::MG_t &m = t->mgs[t->nMGs];
			Q_strncpy( m.szName, pMG->GetName(), sizeof( m.szName ) );
			m.eMount = -1;
			for ( int k = 0; k < ARRAYSIZE( s_pszMGMount ); k++ )
				if ( !Q_stricmp( pMG->GetString( "mount", "" ), s_pszMGMount[k] ) )
					m.eMount = k;
			if ( m.eMount < 0 || !pMG->FindKey( "muzzle" ) || ( m.eMount != VehicleType_t::MG_HULL && !t->szTurretModel[0] ) )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' machine gun '%s' needs \"mount\" turret / barrel / hull and a \"muzzle\"%s - ignored\n",
						 t->szName, m.szName, t->szTurretModel[0] ? "" : " (no turret: hull only)" );
				continue;
			}
			ParseVector( pMG->GetString( "muzzle" ), m.vecMuzzle );
			ParseVector( pMG->GetString( "pivot", "0 0 0" ), m.vecPivot );
			m.flArc = pMG->GetFloat( "arc", 15.0f );
			float flLo = -10.0f, flHi = 20.0f;
			sscanf( pMG->GetString( "pitch", "-10 20" ), "%f %f", &flLo, &flHi );
			m.flPitchMin = MIN( flLo, flHi );
			m.flPitchMax = MAX( flLo, flHi );
			m.iCrew = -1;
			for ( int k = 0; k < NUM_CREW_ROLES; k++ )
				if ( !Q_stricmp( pMG->GetString( "crew", "" ), g_pszCrewRole[k] ) )
					m.iCrew = k;
			m.flDamage = pMG->GetFloat( "damage", 12.0f );
			m.flRate = pMG->GetFloat( "rate", 10.0f );
			m.flBurst = pMG->GetFloat( "burst", 1.5f );
			m.flPause = pMG->GetFloat( "pause", 1.2f );
			m.flRange = pMG->GetFloat( "range", 2500.0f );
			m.flSpread = pMG->GetFloat( "spread", 3.0f );
			Q_strncpy( m.szSound, pMG->GetString( "sound", "Weapon_Mg42.Shoot" ), sizeof( m.szSound ) );
			t->flMGRange = MAX( t->flMGRange, m.flRange );
			t->nMGs++;
		}
		KeyValues *pSnd = pType->FindKey( "sounds" );
		Q_strncpy( t->szEngineIdle, pSnd ? pSnd->GetString( "engine_idle" ) : "", sizeof( t->szEngineIdle ) );
		Q_strncpy( t->szEngineMove, pSnd ? pSnd->GetString( "engine_move" ) : "", sizeof( t->szEngineMove ) );
		Q_strncpy( t->szTurretTurn, pSnd ? pSnd->GetString( "turret_turn" ) : "", sizeof( t->szTurretTurn ) );
		Q_strncpy( t->szTurretStop, pSnd ? pSnd->GetString( "turret_stop" ) : "", sizeof( t->szTurretStop ) );
		Q_strncpy( t->szReload, pSnd ? pSnd->GetString( "reload" ) : "", sizeof( t->szReload ) );
		Q_strncpy( t->szExplode, pSnd ? pSnd->GetString( "explode", "DoDSP_Vehicle.ShellExplode" ) : "DoDSP_Vehicle.ShellExplode", sizeof( t->szExplode ) );
		KeyValues *pHatches = pType->FindKey( "hatches" );
		for ( int r = 0; r < NUM_CREW_ROLES; r++ )
		{
			VehicleType_t::Hatch_t &h = t->hatches[r];
			h.bHas = h.bTurret = false;
			h.vecPos.Init();
			h.iShare = r;
			const char *psz = pHatches ? pHatches->GetString( g_pszCrewRole[r], NULL ) : NULL;
			if ( !psz )
				continue;
			char szFrame[16] = "";
			float x, y, z;
			if ( sscanf( psz, "%15s %f %f %f", szFrame, &x, &y, &z ) != 4 || ( Q_stricmp( szFrame, "turret" ) && Q_stricmp( szFrame, "hull" ) ) )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' hatch \"%s\" is '%s' - want \"turret x y z\" or \"hull x y z\"\n", t->szName, g_pszCrewRole[r], psz );
				continue;
			}
			h.bTurret = !Q_stricmp( szFrame, "turret" );
			if ( h.bTurret && !t->szTurretModel[0] )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' hatch \"%s\" is on a turret it hasn't got\n", t->szName, g_pszCrewRole[r] );
				continue;
			}
			h.bHas = true;
			h.vecPos.Init( x, y, z );
			for ( int q = 0; q < r; q++ )
				if ( t->hatches[q].bHas && t->hatches[q].bTurret == h.bTurret && t->hatches[q].vecPos.DistToSqr( h.vecPos ) < 1.0f )
				{
					h.iShare = t->hatches[q].iShare;
					break;
				}
		}
		for ( int i = 0; i < t->armour.nCrew; i++ )
			if ( !t->hatches[t->armour.eCrew[i]].bHas )
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has a %s but no \"hatches\" entry for him - he gets out beside it\n",
						 t->szName, g_pszCrewRole[t->armour.eCrew[i]] );
		KeyValues *pSeats = pType->FindKey( "seats" );
		for ( KeyValues *pSeat = pSeats ? pSeats->GetFirstTrueSubKey() : NULL; pSeat; pSeat = pSeat->GetNextTrueSubKey() )
		{
			VehicleType_t::Seat_t seat;
			ParseVector( pSeat->GetString( "origin" ), seat.vecOrigin );
			seat.flYaw = pSeat->GetFloat( "yaw" );
			Q_strncpy( seat.szIdle, pSeat->GetString( "idle" ), sizeof( seat.szIdle ) );
			Q_strncpy( seat.szGetout, pSeat->GetString( "getout" ), sizeof( seat.szGetout ) );
			Q_strncpy( seat.szFidget, pSeat->GetString( "fidget", "" ), sizeof( seat.szFidget ) );
			Q_strncpy( seat.szDeath, pSeat->GetString( "death", "" ), sizeof( seat.szDeath ) );
			seat.flDelay = pSeat->GetFloat( "delay" );
			seat.vecWalk = seat.vecRally = vec3_origin;
			seat.flWalkYaw = 0.0f;
			seat.bWalk = sscanf( pSeat->GetString( "walk", "" ), "%f %f %f %f", &seat.vecWalk.x, &seat.vecWalk.y, &seat.vecWalk.z, &seat.flWalkYaw ) == 4;
			seat.bRally = sscanf( pSeat->GetString( "rally", "" ), "%f %f %f", &seat.vecRally.x, &seat.vecRally.y, &seat.vecRally.z ) == 3;
			if ( pSeat->GetString( "walk", NULL ) && !seat.bWalk )
				Warning( "INVARIANT DOD-SP vehicles: type '%s': a seat's \"walk\" isn't \"x y z yaw\" - he gets out from his seat\n", t->szName );
			if ( pSeat->GetString( "rally", NULL ) && !seat.bRally )
				Warning( "INVARIANT DOD-SP vehicles: type '%s': a seat's \"rally\" isn't \"x y z\" - he runs on the way he jumped\n", t->szName );
			if ( !seat.szIdle[0] || !seat.szGetout[0] )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has a seat without \"idle\" / \"getout\" - skipped (nobody can ride it)\n", t->szName );
				continue;
			}
			if ( t->seats.Count() >= VEHICLE_MAX_SEATS )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has more than %d seats - the rest skipped\n", t->szName, VEHICLE_MAX_SEATS );
				break;
			}
			t->seats.AddToTail( seat );
		}
		// H3: the doors riders get out through
		t->doors.nKeys = 0;
		if ( KeyValues *pDoors = pType->FindKey( "doors" ) )
		{
			CUtlStringList keys;
			V_SplitString( pDoors->GetString( "curve", "" ), " ", keys );
			for ( int k = 0; k + 1 < keys.Count() && t->doors.nKeys < VEHICLE_DOOR_KEYS; k += 2 )
			{
				t->doors.flTime[t->doors.nKeys] = atof( keys[k] ) / 30.0f;
				t->doors.flDeg[t->doors.nKeys] = atof( keys[k + 1] );
				t->doors.nKeys++;
			}
			bool bOrdered = true;
			for ( int k = 1; k < t->doors.nKeys; k++ )
				bOrdered &= t->doors.flTime[k] > t->doors.flTime[k - 1];
			if ( t->doors.nKeys < 2 || !bOrdered )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' has a \"doors\" curve that isn't at least two frame-degree pairs "
						 "in order - its doors stay shut\n", t->szName );
				t->doors.nKeys = 0;
			}
		}
		KeyValues *pCrewSeats = pType->FindKey( "crew_seats" );
		for ( KeyValues *pSeat = pCrewSeats ? pCrewSeats->GetFirstTrueSubKey() : NULL; pSeat; pSeat = pSeat->GetNextTrueSubKey() )
		{
			VehicleType_t::CrewSeat_t cs;
			cs.eRole = -1;
			for ( int r = 0; r < NUM_CREW_ROLES; r++ )
				if ( !Q_stricmp( pSeat->GetName(), g_pszCrewRole[r] ) )
					cs.eRole = r;
			bool bInCrew = false;
			for ( int i = 0; i < t->armour.nCrew; i++ )
				bInCrew |= ( cs.eRole >= 0 && t->armour.eCrew[i] == cs.eRole );
			ParseVector( pSeat->GetString( "origin" ), cs.vecOrigin );
			cs.flYaw = pSeat->GetFloat( "yaw" );
			Q_strncpy( cs.szIdle, pSeat->GetString( "idle" ), sizeof( cs.szIdle ) );
			Q_strncpy( cs.szDeath, pSeat->GetString( "death", "" ), sizeof( cs.szDeath ) );
			cs.bSteer = pSeat->GetBool( "steer" );
			if ( !bInCrew || !cs.szIdle[0] || t->crewSeats.Count() >= VEHICLE_MAX_CREW_SEATS )
			{
				Warning( "INVARIANT DOD-SP vehicles: type '%s' crew seat \"%s\" skipped - %s\n", t->szName, pSeat->GetName(),
						 !bInCrew ? "not a crew role this vehicle has" : !cs.szIdle[0] ? "no \"idle\"" : "too many crew seats" );
				continue;
			}
			t->crewSeats.AddToTail( cs );
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

// ONE list of every sound a vehicle type plays (V4h: precache and the map-load check used two copies): its own
// script names (empty ones left out) + the shared hit sounds.
#define VEHICLE_MAX_SOUNDS	16
static int VehicleSoundNames( const VehicleType_t *pT, const char **ppszOut )
{
	const char *pszAll[] = { pT->gun.szSound, pT->szEngineIdle, pT->szEngineMove, pT->szTurretTurn,
							 pT->szTurretStop, pT->szReload, pT->szExplode,
							 pT->nMGs > 0 ? pT->mgs[0].szSound : "", pT->nMGs > 1 ? pT->mgs[1].szSound : "",
							 pT->nMGs > 2 ? pT->mgs[2].szSound : "", pT->nMGs > 3 ? pT->mgs[3].szSound : "",
							 s_pszHitSound[VHITFX_IN], s_pszHitSound[VHITFX_STOPPED], s_pszHitSound[VHITFX_RICOCHET] };
	COMPILE_TIME_ASSERT( ARRAYSIZE( pszAll ) <= VEHICLE_MAX_SOUNDS );
	COMPILE_TIME_ASSERT( VEHICLE_MAX_MGS == 4 );		// one slot per machine gun above
	int n = 0;
	for ( int k = 0; k < ARRAYSIZE( pszAll ); k++ )
		if ( pszAll[k] && pszAll[k][0] )
			ppszOut[n++] = pszAll[k];
	return n;
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
		const char *pszNames[VEHICLE_MAX_SOUNDS];
		int nTypeNames = VehicleSoundNames( pT, pszNames );
		for ( int k = 0; k < nTypeNames; k++ )
		{
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
	// one owner: dodsp_factions. Out of the fight (burning / abandoned / wrecked) it's no side: nobody shoots it
	virtual Class_T	Classify( void ) { return m_eState == VSTATE_FIGHTING ? DoDSP_ClassForFaction( m_eFaction ) : CLASS_NONE; }
	virtual void	TraceAttack( const CTakeDamageInfo &info, const Vector &vecDir, trace_t *ptr, CDmgAccumulator *pAccumulator = NULL );
	virtual int		OnTakeDamage( const CTakeDamageInfo &info );
	int				GetFaction( void ) const { return m_eFaction; }

	void			DriveThink( void );
	void			WreckThink( void );
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
	static CDoDSPVehicle *FindShellHit( const Vector &vecFrom, const Vector &vecDir, float flMax, CDoDSPVehicle *pSkip, VehicleHit_t *pHit );

private:
	// V4 armour and damage (one owner for each: whether a hit gets in = TakeArmourHit / BlastHit; what it breaks
	// = ApplyPenetration; who's left = the crew mask; what it is now = m_eState)
	bool			ShellHitBox( const Vector &vecFrom, const Vector &vecDir, float flMax, VehicleHit_t *pHit );
	bool			TakeArmourHit( const VehicleHit_t &hit, const Vector &vecPath, float flPen, float flEffect, float flCalibre,
								   CBaseEntity *pAttacker, const char *pszWhat, bool bQuiet, float flFiller );
	void			BlastHit( const Vector &vecCentre, float flFiller, CBaseEntity *pAttacker );
	void			ExternalBlast( float flFiller, float k, CBaseEntity *pAttacker );
	void			ApplyPenetration( int ePart, int eFace, float flEffect, CBaseEntity *pAttacker, const char *pszWhat );
	int				RollCrit( int ePart, int eFace );
	void			KillCrew( int n, int ePart, const char *pszWhy );
	void			SetImmobile( const char *pszWhy, CBaseEntity *pAttacker );
	void			SetBurning( bool bAmmo, CBaseEntity *pAttacker );
	void			CheckMorale( CBaseEntity *pAttacker );
	void			BailOut( int eWhy, CBaseEntity *pAttacker );
	bool			SpawnCrewman( const Vector &vecAwayFrom, int eRole );
	bool			HatchFor( int eRole, Vector *pvecHatch );
	void			HitFeedback( int eFx, const VehicleHit_t &hit, const Vector &vecPath, float flCalibre );
	void			UpdateDamageSmoke( void );
	void			KnockOut( bool bCatastrophic, CBaseEntity *pAttacker, const char *pszWhy );
	void			Wreck( bool bBlowUp, CBaseEntity *pAttacker );
	void			FinishWreck( void );
	void			ThrowWreckDebris( int nPieces, float flUp );
	void			DamageStateThink( void );
	int				CrewAlive( void ) const;
	bool			CrewHas( int eRole ) const;
	bool			TypeHasRole( int eRole ) const;
	bool			CanAim( void ) const;			// someone in the turret to lay the gun
	float			ReloadScale( void ) const;
	float			AimPenalty( void ) const;		// x spread (no trained gunner left)
	float			TurretRate( void ) const;
	bool			MainGunUsable( void ) const;
	const DoDSPShell_t *ShellFor( CBaseEntity *pTarget ) const;
	bool			CanHurt( CDoDSPVehicle *pOther );
	void			DamageReport( char *psz, int n ) const;
	void			MakeFire( float flSize, bool bEngine );
	void			SetState( int eNew, CBaseEntity *pAttacker );
	void			HaltMotion( void );
	void			CheckHalted( void );
	void			FireKnockedOut( CBaseEntity *pAttacker );
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
	void			FireMG( int iMG, const Vector &vecSrc, const Vector &vecDir );
	Vector			MGMuzzle( int iMG );				// V3e: the one owner of where a machine gun's bullets start
	bool			MGManned( int iMG ) const;
	bool			MGBurstShot( int iMG );				// its burst rhythm: true = a shot is due now
	bool			AnyMGUsable( void ) const;
	void			FireTurretMGs( CBaseEntity *pTarget, const Vector &vecAimPoint, const Vector &vecBarrel );
	void			FireHullMGs( CBaseEntity *pTarget, const Vector &vecAimPoint );
	void			UpdateSounds( float flTurretRate );
	void			StopSounds( void );

	string_t		m_iszVehicleType;
	bool			m_bRiders;					// Hammer "riders": spawn with the full riding complement (V3)
	// V3 riders: one per seat, put aboard after activation; RidersThink is the one owner of when they get off
	string_t		m_iszDismount;				// Hammer "riders_dismount": contact (default) / order
	int				m_eDismount;				// VehicleDismount_t
	int				m_eRiders;					// VehicleRiders_t
	bool			m_bRidersSpawned;
	float			m_flRidersSpawnAt;
	EHANDLE			m_hRider[VEHICLE_MAX_SEATS];
	EHANDLE			m_hRiderCorpse[VEHICLE_MAX_SEATS];	// H3: killed in his seat, he stays slumped in it (gone with the vehicle)
	float			m_flDoorsStart;				// H3: when the doors' curve started (the dismount order; 0 = shut, never opened)
	int				m_iDoorPose;				// the model's door pose parameter (looked up once; -2 = not yet, -1 none)
	float			m_flDoorOpen;				// ... and the degrees it opens them at 1 (the model's "dodsp_doors")
	void			DoorsThink( void );
	static float	DoorPoseFor( float flDeg, float flOpen );
	float			m_flRiderGo[VEHICLE_MAX_SEATS];	// when each gets off (0 = not ordered / gone)
	bool			m_bDismountOrder;
	float			m_flRidersOffAt;			// when they started getting off (a rider waits for his landing spot at most VEHICLE_RIDER_WAIT_MAX)
	COutputEvent	m_OnRidersOff;
	void			RidersThink( void );
	// H2: crew you can see in their seats (the type's "crew_seats") - CrewSeatsThink is the one owner of keeping each
	// in step with his crew role, and of the driver's steering pose
	EHANDLE			m_hCrewSeat[VEHICLE_MAX_CREW_SEATS];
	EHANDLE			m_hCrewCorpse[VEHICLE_MAX_CREW_SEATS];	// H2b: killed in his seat, he stays slumped in it
	bool			m_bCrewSeatsSpawned;
	float			m_flCrewSeatsAt;
	int				m_iSteerPose;				// the driver's "steer" pose parameter (looked up once; -2 = not yet)
	void			CrewSeatsThink( void );
	void			SpawnCrewSeats( void );
	void			LeaveCrewSeat( int eRole );
	void			CrewmanShot( int iCrew, const char *pszWhy );
	int				CrewIndexOf( int eRole ) const;
	void			SpawnRiders( void );
	void			OrderDismount( const char *pszWhy );
	int				RidersAboard( void );
	bool			RidersHold( void ) const { return m_eRiders == VRIDERS_STOPPING || m_eRiders == VRIDERS_GETTING_OFF; }
	void			InputDismount( inputdata_t &inputdata );
	string_t		m_iszInitialGoal;
	VehicleType_t	*m_pType;
	float			m_flHalfWidth, m_flHalfLength, m_flBodyHeight;	// from the model (MeasureBody), not saved
	float			m_flWheelbase;		// H1: from the model's "dodsp_wheels" (MeasureBody); 0 = nothing steers
	void			BodyKick( float flPitchVel, float flRollVel );	// THE way a blow or the gun rocks the body
	EHANDLE			m_hTurret;

	CNetworkVar( float, m_flTrackSpeed );		// signed, units/s (client: wheel spin, track scroll)
	CNetworkVar( float, m_flModelForwardYaw );
	CNetworkVar( float, m_flSteer );			// H1: degrees the steered wheels are turned, + = left (client: wheels)
	// H1 suspension: a model with a body pivot rides its body on springs over planted wheels, run on the CLIENT every
	// frame (smooth; a think-rate network var would step). The server sends the spring's settings and each kick.
	CNetworkVar( bool, m_bBodySprung );
	CNetworkVar( float, m_flBodySuspAccel );
	CNetworkVar( float, m_flBodySuspRoll );
	CNetworkVar( float, m_flBodySuspFreq );
	CNetworkVar( float, m_flBodySuspDamp );
	CNetworkVar( int, m_nBodyKicks );			// +1 per kick
	CNetworkVar( float, m_flBodyKickPitch );	// the last kick (degrees/s, nose up +)
	CNetworkVar( float, m_flBodyKickRoll );		// (degrees/s, right side down +)

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
	float			m_flNextMGShot[VEHICLE_MAX_MGS], m_flMGBurstEnd[VEHICLE_MAX_MGS], m_flMGPauseUntil[VEHICLE_MAX_MGS];
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

	// V4 damage state (saved)
	int				m_eState;					// VehicleState_t
	float			m_flStructure;
	int				m_iCrewAlive;				// bit per crew seat (DoDSPArmour_t::eCrew)
	bool			m_bImmobile, m_bTurretJammed, m_bGunOut;
	float			m_flBailAt, m_flCookOffAt, m_flBurnOutAt;
	float			m_flWreckAt;				// V4i: the secondary explosion went off; the wreck model goes in then
	bool			m_bWreckBlowUp;
	EHANDLE			m_hFire;
	CHandle<SmokeTrail>	m_hDamageSmoke;			// V4h: grey / black smoke while it's hurt (UpdateDamageSmoke)
	int				m_iDamageSmoke;				// 0 none, 1 light, 2 heavy
	EHANDLE			m_hKiller;
	int				m_iRounds, m_iMagLeft;
	float			m_flPenTimes[2];
	bool			m_bOutputKO, m_bOutOfAmmoSaid;
	int				m_nBailPending;				// crewmen still to come out (one at a time)
	int				m_iBailMask;				// their seats (DoDSPArmour_t::eCrew), lowest first
	float			m_flNextBailAt, m_flIgniteAt;
	Vector			m_vecBailFrom;				// where the attack came from (they get out on the far side)
	EHANDLE			m_hLastCrewman;				// not saved: the run order for the man who just got out
	Vector			m_vecLastCrewRun;
	float			m_flHatchFree[NUM_CREW_ROLES];	// V4e: a shared hatch is in use until then (by VehicleType_t::Hatch_t::iShare)
	float			m_flCrewRunAt;
	bool			m_bMovingWarned;
	COutputEvent	m_OnPenetrated;
	COutputEvent	m_OnImmobilized;
	COutputEvent	m_OnBailOut;
	COutputEvent	m_OnKnockedOut;
};

CUtlVector<CDoDSPVehicle *> CDoDSPVehicle::s_All;

LINK_ENTITY_TO_CLASS( dodsp_vehicle, CDoDSPVehicle );

BEGIN_DATADESC( CDoDSPVehicle )
	DEFINE_KEYFIELD( m_iszVehicleType, FIELD_STRING, "vehicletype" ),
	DEFINE_KEYFIELD( m_bRiders, FIELD_BOOLEAN, "riders" ),
	DEFINE_KEYFIELD( m_iszDismount, FIELD_STRING, "riders_dismount" ),
	DEFINE_FIELD( m_eDismount, FIELD_INTEGER ),
	DEFINE_FIELD( m_eRiders, FIELD_INTEGER ),
	DEFINE_FIELD( m_bRidersSpawned, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_flRidersSpawnAt, FIELD_TIME ),
	DEFINE_AUTO_ARRAY( m_hRider, FIELD_EHANDLE ),
	DEFINE_AUTO_ARRAY( m_hRiderCorpse, FIELD_EHANDLE ),
	DEFINE_FIELD( m_flDoorsStart, FIELD_TIME ),
	DEFINE_AUTO_ARRAY( m_hCrewSeat, FIELD_EHANDLE ),
	DEFINE_AUTO_ARRAY( m_hCrewCorpse, FIELD_EHANDLE ),
	DEFINE_FIELD( m_bCrewSeatsSpawned, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_flCrewSeatsAt, FIELD_TIME ),
	DEFINE_AUTO_ARRAY( m_flRiderGo, FIELD_TIME ),
	DEFINE_FIELD( m_bDismountOrder, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_flRidersOffAt, FIELD_TIME ),
	DEFINE_OUTPUT( m_OnRidersOff, "OnRidersOff" ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Dismount", InputDismount ),
	DEFINE_KEYFIELD( m_iszInitialGoal, FIELD_STRING, "goal" ),
	DEFINE_FIELD( m_hTurret, FIELD_EHANDLE ),
	DEFINE_FIELD( m_flSpeed, FIELD_FLOAT ),
	DEFINE_FIELD( m_flSteer, FIELD_FLOAT ),
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
	DEFINE_THINKFUNC( DriveThink ),
	DEFINE_THINKFUNC( WreckThink ),
	// V4
	DEFINE_FIELD( m_eState, FIELD_INTEGER ),
	DEFINE_FIELD( m_flStructure, FIELD_FLOAT ),
	DEFINE_FIELD( m_iCrewAlive, FIELD_INTEGER ),
	DEFINE_FIELD( m_bImmobile, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_bTurretJammed, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_bGunOut, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_flBailAt, FIELD_TIME ),
	DEFINE_FIELD( m_flCookOffAt, FIELD_TIME ),
	DEFINE_FIELD( m_flBurnOutAt, FIELD_TIME ),
	DEFINE_FIELD( m_flWreckAt, FIELD_TIME ),
	DEFINE_FIELD( m_bWreckBlowUp, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_hFire, FIELD_EHANDLE ),
	DEFINE_FIELD( m_hDamageSmoke, FIELD_EHANDLE ),
	DEFINE_FIELD( m_iDamageSmoke, FIELD_INTEGER ),
	DEFINE_FIELD( m_hKiller, FIELD_EHANDLE ),
	DEFINE_FIELD( m_iRounds, FIELD_INTEGER ),
	DEFINE_FIELD( m_iMagLeft, FIELD_INTEGER ),
	DEFINE_ARRAY( m_flPenTimes, FIELD_TIME, 2 ),
	DEFINE_FIELD( m_bOutputKO, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_bOutOfAmmoSaid, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_nBailPending, FIELD_INTEGER ),
	DEFINE_FIELD( m_iBailMask, FIELD_INTEGER ),
	DEFINE_FIELD( m_flNextBailAt, FIELD_TIME ),
	DEFINE_FIELD( m_flIgniteAt, FIELD_TIME ),
	DEFINE_FIELD( m_vecBailFrom, FIELD_POSITION_VECTOR ),
	DEFINE_OUTPUT( m_OnPenetrated, "OnPenetrated" ),
	DEFINE_OUTPUT( m_OnImmobilized, "OnImmobilized" ),
	DEFINE_OUTPUT( m_OnBailOut, "OnBailOut" ),
	DEFINE_OUTPUT( m_OnKnockedOut, "OnKnockedOut" ),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST( CDoDSPVehicle, DT_DoDSPVehicle )
	SendPropFloat( SENDINFO( m_flTrackSpeed ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flModelForwardYaw ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flSteer ), 0, SPROP_NOSCALE ),
	SendPropBool( SENDINFO( m_bBodySprung ) ),
	SendPropFloat( SENDINFO( m_flBodySuspAccel ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flBodySuspRoll ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flBodySuspFreq ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flBodySuspDamp ), 0, SPROP_NOSCALE ),
	SendPropInt( SENDINFO( m_nBodyKicks ), 8, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flBodyKickPitch ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flBodyKickRoll ), 0, SPROP_NOSCALE ),
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
	m_flWheelbase = 0.0f;
	m_flSteer = 0.0f;
	m_bBodySprung = false;
	m_nBodyKicks = 0;
	m_flBodyKickPitch = m_flBodyKickRoll = 0.0f;
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
	m_iDamageSmoke = 0;
	m_bHoldFire = false;
	m_flNextScan = 0.0f;
	m_flTurretYaw = m_flTurretPitch = 0.0f;
	m_flNextGun = 0.0f;
	for ( int k = 0; k < VEHICLE_MAX_MGS; k++ )
		m_flNextMGShot[k] = m_flMGBurstEnd[k] = m_flMGPauseUntil[k] = 0.0f;
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
	m_eState = VSTATE_FIGHTING;
	m_flStructure = 0.0f;
	m_iCrewAlive = 0;
	m_bImmobile = m_bTurretJammed = m_bGunOut = false;
	m_flBailAt = m_flCookOffAt = m_flBurnOutAt = m_flWreckAt = 0.0f;
	m_bWreckBlowUp = false;
	m_iRounds = m_iMagLeft = 0;
	m_flPenTimes[0] = m_flPenTimes[1] = -100.0f;
	m_bOutputKO = m_bOutOfAmmoSaid = false;
	m_nBailPending = 0;
	m_iBailMask = 0;
	m_flNextBailAt = m_flIgniteAt = m_flCrewRunAt = 0.0f;
	for ( int r = 0; r < NUM_CREW_ROLES; r++ )
		m_flHatchFree[r] = 0.0f;
	m_vecBailFrom = m_vecLastCrewRun = vec3_origin;
	m_bMovingWarned = false;
	m_eDismount = VDISMOUNT_CONTACT;
	m_eRiders = VRIDERS_NONE;
	m_bRidersSpawned = m_bDismountOrder = false;
	m_flRidersSpawnAt = m_flRidersOffAt = 0.0f;
	for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
		m_flRiderGo[i] = 0.0f;
	m_bCrewSeatsSpawned = false;
	m_flCrewSeatsAt = 0.0f;
	m_iSteerPose = -2;
	m_flDoorsStart = 0.0f;
	m_iDoorPose = -2;
	m_flDoorOpen = 0.0f;
	s_All.AddToTail( this );
}

void CDoDSPVehicle::UpdateOnRemove( void )
{
	s_All.FindAndRemove( this );
	for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
	{
		if ( m_hRider[i].Get() )
			GermanSoldier_RideAbort( m_hRider[i].Get() );		// still aboard: off where they are, not left on nothing
		if ( m_hRiderCorpse[i].Get() )
			UTIL_Remove( m_hRiderCorpse[i].Get() );			// H3: slumped in his seat - goes with it
	}
	for ( int i = 0; i < VEHICLE_MAX_CREW_SEATS; i++ )
	{
		if ( m_hCrewSeat[i].Get() )
			UTIL_Remove( m_hCrewSeat[i].Get() );				// the vehicle's own crew goes with it
		if ( m_hCrewCorpse[i].Get() )
			UTIL_Remove( m_hCrewCorpse[i].Get() );
	}
	StopSounds();
	if ( m_hTurret.Get() )
		UTIL_Remove( m_hTurret.Get() );
	if ( m_hFire.Get() )
		UTIL_Remove( m_hFire.Get() );
	BaseClass::UpdateOnRemove();
}

void CDoDSPVehicle::Precache( void )
{
	// V4j: the type is the classname's (dodsp_vehicle_<type>, one Hammer class per type so Hammer shows the right
	// model) or, on the plain dodsp_vehicle, its "vehicletype" key. ONE owner: this.
	const char *pszClass = GetClassname();
	if ( !Q_strnicmp( pszClass, DODSP_VEHICLE_CLASS_PREFIX, Q_strlen( DODSP_VEHICLE_CLASS_PREFIX ) ) )
	{
		const char *pszFromClass = pszClass + Q_strlen( DODSP_VEHICLE_CLASS_PREFIX );
		if ( m_iszVehicleType != NULL_STRING && Q_stricmp( STRING( m_iszVehicleType ), pszFromClass ) )
			Warning( "INVARIANT DOD-SP vehicle %s: class %s but vehicletype '%s' - the class wins\n", Label(), pszClass, STRING( m_iszVehicleType ) );
		m_iszVehicleType = AllocPooledString( pszFromClass );
	}
	m_pType = FindVehicleType( STRING( m_iszVehicleType ) );
	if ( m_pType )
	{
		PrecacheModel( m_pType->szModel );
		if ( m_pType->szTurretModel[0] )
			PrecacheModel( m_pType->szTurretModel );
		if ( m_pType->szDeadModel[0] )
		{
			if ( filesystem->FileExists( m_pType->szDeadModel, "GAME" ) )
				PrecacheModel( m_pType->szDeadModel );
			else
				Warning( "INVARIANT DOD-SP vehicles: type '%s' dead_model %s not found - a knocked-out one keeps its live model\n",
						 m_pType->szName, m_pType->szDeadModel );
		}
		UTIL_PrecacheOther( "env_fire" );
		UTIL_PrecacheOther( "npc_german_soldier" );
		VehicleSoundsLevelInit();
		const char *pszNames[VEHICLE_MAX_SOUNDS];
		int nNames = VehicleSoundNames( m_pType, pszNames );
		for ( int k = 0; k < nNames; k++ )
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

	// V3: when its riders get off (they're put aboard after activation, RidersThink)
	m_eDismount = VDISMOUNT_CONTACT;
	if ( m_iszDismount != NULL_STRING && STRING( m_iszDismount )[0] )
	{
		int e = -1;
		for ( int i = 0; i < NUM_VDISMOUNT; i++ )
			if ( !Q_stricmp( STRING( m_iszDismount ), s_pszDismount[i] ) )
				e = i;
		if ( e >= 0 )
			m_eDismount = e;
		else
			Warning( "INVARIANT DOD-SP vehicle %s: riders_dismount '%s' is not contact / order - contact\n", Label(), STRING( m_iszDismount ) );
	}

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
	// V4: hits are judged by its armour (TraceAttack / OnTakeDamage); health is never used
	if ( m_pType->armour.bValid )
	{
		m_takedamage = DAMAGE_YES;
		m_flStructure = m_pType->armour.flStructure;
		m_iCrewAlive = ( 1 << m_pType->armour.nCrew ) - 1;
	}
	else
		m_takedamage = DAMAGE_NO;
	m_iRounds = m_pType->gun.nRounds;
	m_iMagLeft = m_pType->gun.nMagazine;
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
// INVARIANT: a vehicle's collision model covers the same box as its mesh. Every original vehicle model's .phy matches its
// hull box within 1% on every axis; the H1 halftrack rig shipped Crowbar's decompiled collision in another frame and it
// stood on its side across the vehicle (people and props hit air beside it). phy_fit.py now fits a decompile's collision
// onto the compiled .phy before a rig writes it; this catches any model that still gets it wrong. Once per model.
#define VEHICLE_COLLIDE_SLACK	0.10f		// share of an axis the collision may differ from the mesh box
#define VEHICLE_COLLIDE_MIN		12.0f		// units: never flag less than this
static void CheckVehicleCollision( CBaseEntity *pVehicle, const char *pszLabel )
{
	static CUtlDict<bool, int> s_Checked;
	const char *pszModel = STRING( pVehicle->GetModelName() );
	if ( s_Checked.Find( pszModel ) != s_Checked.InvalidIndex() )
		return;
	s_Checked.Insert( pszModel, true );
	vcollide_t *pCollide = modelinfo->GetVCollide( pVehicle->GetModelIndex() );
	if ( !pCollide || pCollide->solidCount <= 0 )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: %s has no collision model - people and shells pass through it (recompile it with its $collisionmodel)\n",
				 pszLabel, pszModel );
		return;
	}
	Vector vecMins( FLT_MAX, FLT_MAX, FLT_MAX ), vecMaxs( -FLT_MAX, -FLT_MAX, -FLT_MAX );
	for ( int i = 0; i < pCollide->solidCount; i++ )
	{
		Vector a, b;
		physcollision->CollideGetAABB( &a, &b, pCollide->solids[i], vec3_origin, vec3_angle );
		VectorMin( vecMins, a, vecMins );
		VectorMax( vecMaxs, b, vecMaxs );
	}
	Vector vecMesh = pVehicle->CollisionProp()->OBBMaxs() - pVehicle->CollisionProp()->OBBMins();
	Vector vecPhys = vecMaxs - vecMins;
	for ( int k = 0; k < 3; k++ )
	{
		// an open-topped vehicle's collision stops short of its highest part (vehicle_rig.py OPEN_TOP: the bay is open, so
		// the top over it is too) - up to half its height lower is fine; anything else is measured both ways
		float flSlack = MAX( VEHICLE_COLLIDE_MIN, VEHICLE_COLLIDE_SLACK * vecMesh[k] );
		bool bOK = ( k == 2 ) ? ( vecPhys[k] <= vecMesh[k] + flSlack && vecPhys[k] >= 0.5f * vecMesh[k] ) : fabs( vecPhys[k] - vecMesh[k] ) <= flSlack;
		if ( !bOK )
		{
			Warning( "INVARIANT DOD-SP vehicle %s: %s's collision is %.0f x %.0f x %.0f but its mesh %.0f x %.0f x %.0f (length x width x height) "
					 "- the collision is turned or the wrong one; recompile the model from its rig\n", pszLabel, pszModel,
					 vecPhys.x, vecPhys.y, vecPhys.z, vecMesh.x, vecMesh.y, vecMesh.z );
			return;
		}
	}
}

void CDoDSPVehicle::MeasureBody( void )
{
	CheckVehicleCollision( this, Label() );
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	float flYawRad = DEG2RAD( m_pType->flForwardYaw );
	Vector vecFwd( cosf( flYawRad ), sinf( flYawRad ), 0.0f );
	m_flHalfLength = 0.5f * fabs( DotProduct( vecMaxs - vecMins, vecFwd ) );
	m_flHalfWidth = 0.5f * fabs( DotProduct( vecMaxs - vecMins, Vector( -vecFwd.y, vecFwd.x, 0.0f ) ) );
	m_flBodyHeight = MAX( vecMaxs.z, VEHICLE_STEP + 8.0f );		// above its origin (the ground point)
	// H1: the wheels' facts come from the model (dodsp_vehicle_wheels.h: vehicle_rig.py measured them from the mesh)
	DoDSPWheels_t wheels;
	bool bWheelData = DoDSP_ReadWheels( GetModel(), wheels );
	int nSteer = 0;
	FOR_EACH_VEC( wheels.wheels, i )
		nSteer += wheels.wheels[i].bSteer ? 1 : 0;
	m_flWheelbase = ( nSteer && !m_pType->bTracked ) ? wheels.flWheelbase : 0.0f;
	// H1 suspension: the body rides on springs over its wheels when the model says where it tips (the client runs it)
	m_bBodySprung = bWheelData && wheels.bBodyPivot;
	m_flBodySuspAccel = m_pType->flSuspAccel;
	m_flBodySuspRoll = m_pType->flSuspRoll;
	m_flBodySuspFreq = m_pType->flSuspFreq;
	m_flBodySuspDamp = m_pType->flSuspDamp;
	static CUtlDict<bool, int> s_Reported;
	const char *pszModel = STRING( GetModelName() );
	if ( s_Reported.Find( pszModel ) == s_Reported.InvalidIndex() )
	{
		s_Reported.Insert( pszModel, true );
		if ( bWheelData )
			Msg( "DOD-SP vehicle wheels: %s - %d wheels from the model, %d steer, wheelbase %.0f, %d suspension units%s\n", pszModel,
				 wheels.wheels.Count(), nSteer, wheels.flWheelbase, wheels.susp.Count(), wheels.bBodyPivot ? ", body on springs over the wheels" : "" );
		if ( bWheelData && !wheels.susp.Count() && !V_stristr( pszModel, "_dead" ) )
			Msg( "DOD-SP vehicle wheels: %s has no suspension units - its wheels can't follow the ground (recompile it from vehicle_rig.py)\n", pszModel );
		else if ( !m_pType->bTracked && !V_stristr( pszModel, "_dead" ) )
			Msg( "DOD-SP vehicle wheels: %s has no wheel data - its wheels can't turn or steer (recompile it from vehicle_rig.py)\n", pszModel );
	}
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
	if ( m_bRiders && !m_bRidersSpawned && m_flRidersSpawnAt <= 0.0f )
		m_flRidersSpawnAt = gpGlobals->curtime + VEHICLE_RIDERS_SPAWN;
	if ( m_pType && m_pType->crewSeats.Count() && !m_bCrewSeatsSpawned && m_flCrewSeatsAt <= 0.0f )
		m_flCrewSeatsAt = gpGlobals->curtime + VEHICLE_RIDERS_SPAWN;
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
	RidersThink();
	DoorsThink();
	CrewSeatsThink();
	if ( m_eState != VSTATE_FIGHTING )
	{
		DamageStateThink();		// burning / abandoned: no driving, no guns
		return;
	}
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

	// V4: an immobilised vehicle doesn't move; a jammed turret is aimed by turning the whole (tracked) tank.
	// V3: nor while its riders get off (it stops for them, then waits until the last is down).
	if ( m_bImmobile || RidersHold() )
	{
		flTargetSpeed = 0.0f;
		flYawRate = 0.0f;
		if ( RidersHold() )
			m_flProgressTime = now;		// standing for its riders isn't stuck (the watchdog would blame the road)
	}
	else if ( m_bTurretJammed && !m_bHasGoal && m_pType->bTracked && m_hTarget.Get() )
	{
		Vector vecTo = m_hTarget->WorldSpaceCenter() - vecPos;
		float flWantHeading = RAD2DEG( atan2f( vecTo.y, vecTo.x ) ) - ( m_flTurretYaw + m_flTurretFwdOffset ) + m_pType->flForwardYaw;
		flYawRate = clamp( AngleDiff( flWantHeading, m_flHeading ) * 2.0f, -m_pType->flTurnRate, m_pType->flTurnRate );
	}

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

	// H1: THE steering angle the client turns the steered wheels by - from the turn it is really making (bicycle model:
	// yaw rate = speed x tan(steer) / wheelbase, so steer = atan(wheelbase x yaw rate / speed); reversing flips it by
	// itself). Full lock = the type's min_turn_radius. Standing still it holds the wheel where it was, and straightens
	// it once there's nowhere to go. Wound at VEHICLE_STEER_RATE, like a driver turning the wheel.
	if ( m_flWheelbase > 0.0f )
	{
		float flLock = RAD2DEG( atanf( m_flWheelbase / MAX( m_pType->flMinTurnRadius, 50.0f ) ) );
		float flWantSteer = m_flSteer;
		if ( fabs( m_flSpeed ) > 2.0f )
			flWantSteer = RAD2DEG( atanf( m_flWheelbase * DEG2RAD( flYawRate ) / m_flSpeed ) );
		else if ( !m_bHasGoal )
			flWantSteer = 0.0f;
		flWantSteer = clamp( flWantSteer, -flLock, flLock );
		float flStep = VEHICLE_STEER_RATE * dt;
		m_flSteer = m_flSteer + clamp( flWantSteer - m_flSteer, -flStep, flStep );
	}
	else if ( m_flSteer != 0.0f )
		m_flSteer = 0.0f;
	Vector vecNext = vecPos + vecFwd * ( m_flSpeed * dt );
	float flGroundZ;
	QAngle angWant = BuildAngles( m_flHeading, vecNext, &flGroundZ );
	// the lurch (owner: "from a complete stop to moving, with a lurch" - the models have no animation for it, only
	// 'idle'): the hull rocks on a damped spring driven by its acceleration - nose up pulling away, nose down
	// braking, a little overshoot - and by the main gun's recoil (FireMainGun kicks m_flSuspVel)
	// H1: a sprung body (the model has a body pivot) is rocked on the CLIENT instead, over wheels that stay down - the
	// whole entity tipping here lifted the halftrack's front tyres off the road (owner 2026-11-20)
	if ( !m_bBodySprung )
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
	float flGun = m_pType->gun.bHas ? m_pType->gun.flRange : m_pType->flMGRange;
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
	if ( m_bImmobile )
	{
		if ( m_bHasGoal )
			Stop();
		SetTactic( VTACTIC_HOLD, VWHY_IMMOBILE );
		return;
	}
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
		 "main gun %s, %d machine gun%s\n", Label(), m_pType->szName, DoDSP_FactionName( m_eFaction ), m_flTurretFwdOffset,
		 m_iPitchPose >= 0 ? "" : "MISSING ", m_flPoseMin, m_flPoseMax, m_flPitchSign > 0 ? "+" : "-",
		 m_iMuzzleAttach > 0 ? "found" : "MISSING", m_pType->gun.bHas ? "yes" : "no", m_pType->nMGs, m_pType->nMGs == 1 ? "" : "s" );
	for ( int k = 0; k < m_pType->nMGs; k++ )
	{
		const VehicleType_t::MG_t &m = m_pType->mgs[k];
		Msg( "DOD-SP vehicle %s: machine gun '%s' on the %s at %.1f %.1f %.1f%s\n", Label(), m.szName, s_pszMGMount[m.eMount],
			 m.vecMuzzle.x, m.vecMuzzle.y, m.vecMuzzle.z, m.eMount == VehicleType_t::MG_HULL ? CFmtStr( " (+-%.0f deg, %.0f..%.0f up, %s)",
			 m.flArc, m.flPitchMin, m.flPitchMax, m.iCrew >= 0 ? g_pszCrewRole[m.iCrew] : "any crew" ).Access() : "" );
	}
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
	float flRange = MAX( m_pType->gun.bHas ? m_pType->gun.flRange : 0.0f, m_pType->flMGRange );
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
		// V4: a vehicle whose armour facing it its gun can't get through counts as 3x as far (it still shoots it
		// when nothing else is there - a hit on the tracks or a lucky angle)
		CDoDSPVehicle *pV = dynamic_cast<CDoDSPVehicle *>( p );
		if ( pV && !CanHurt( pV ) )
			flDist *= 3.0f;
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

// One shell (V4): AP at vehicles, HE at everything else, each with its real numbers. A vehicle on the line is
// found by its ARMOUR BOXES (FindShellHit) - the turrets have no collision model, so the trace alone would fly
// through them - and judged by TakeArmourHit; anything else gets the blast (DoDSP_ShellBlast) or, for solid shot,
// the hole.
void CDoDSPVehicle::FireMainGun( const Vector &vecAim )
{
	CBaseEntity *pTarget = m_hTarget.Get();
	const DoDSPShell_t *pShell = ShellFor( pTarget );
	if ( !pShell )
		return;
	bool bHE = ( pShell == &m_pType->gun.he );
	Vector vecDir = vecAim;
	// a little spread: a shell isn't a laser (wider on the move, and with no trained gunner left)
	float flSpread = DEG2RAD( m_pType->gun.flSpread ) * 0.5f * ( fabs( m_flSpeed ) < 15.0f ? 1.0f : 3.0f ) * AimPenalty();
	vecDir += Vector( random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ), random->RandomFloat( -flSpread, flSpread ) );
	VectorNormalize( vecDir );
	Vector vecMuzzle = MuzzlePos( vecDir );
	trace_t tr;
	CTraceFilterSkipTwoEntities filter( this, m_hTurret.Get(), COLLISION_GROUP_NONE );
	UTIL_TraceLine( vecMuzzle, vecMuzzle + vecDir * m_pType->gun.flRange, MASK_SHOT, &filter, &tr );	// dodsp-lint: not a sight line (the shell's own flight)
	float flTraceDist = tr.fraction * m_pType->gun.flRange;
	VehicleHit_t vh;
	CDoDSPVehicle *pHitV = FindShellHit( vecMuzzle, vecDir, flTraceDist + 8.0f, this, &vh );
	QAngle angDir;
	VectorAngles( vecDir, angDir );
	g_pEffects->MuzzleFlash( vecMuzzle, angDir, 6.0f, MUZZLEFLASH_TYPE_DEFAULT );
	if ( m_pType->gun.szSound[0] )
		EmitSound( m_pType->gun.szSound );
	UTIL_ScreenShake( vecMuzzle, 6.0f, 150.0f, 0.6f, 1024.0f, SHAKE_START );
	// recoil: the hull rocks away from the shot (nose up when firing ahead, down when firing behind, sideways = roll)
	{
		float flRecoilVel = m_pType->flRecoil * 2.0f * M_PI_F * m_pType->flSuspFreq;
		float flGunYaw = DEG2RAD( m_flTurretYaw + m_flTurretFwdOffset );
		BodyKick( flRecoilVel * cosf( flGunYaw ), flRecoilVel * sinf( flGunYaw ) );	// firing left (+90) rolls it right
	}
	char szWhat[64];
	Q_snprintf( szWhat, sizeof( szWhat ), "%s's %.0f mm %s", Label(), pShell->flCalibre, bHE ? "HE" : "AP" );
	const char *pszHit = "nothing";
	float flHitDist = 0.0f;
	if ( pHitV )
	{
		pszHit = pHitV->Label();
		flHitDist = vh.t;
		float flPen = DoDSP_ShellPen( *pShell, vh.t );
		bool bIn = pHitV->TakeArmourHit( vh, vecDir, flPen, pShell->flEffect, pShell->flCalibre, this, szWhat, false, pShell->flFiller );
		// HE (and a shell that stayed outside) bursts on the plate; the vehicle's own hit is judged above, so the
		// blast leaves it out. A shell that got in did its work inside (its effect).
		if ( !bIn || bHE )
			DoDSP_ShellBlast( vh.vecPoint - vecDir * 8.0f, pShell->flFiller, this, pHitV, m_pType->szExplode );
		UTIL_ScreenShake( vh.vecPoint, 12.0f, 150.0f, 1.0f, 768.0f, SHAKE_START );
	}
	else if ( tr.fraction < 1.0f && !tr.startsolid )
	{
		pszHit = VehicleHitName( tr );
		flHitDist = flTraceDist;
		Vector vecBoom = tr.endpos + tr.plane.normal * 8.0f;
		if ( pShell->flFiller > 0.0f )
			DoDSP_ShellBlast( vecBoom, pShell->flFiller, this, NULL, m_pType->szExplode );
		else
			g_pEffects->Sparks( tr.endpos, 2, 2, &tr.plane.normal );
		// solid shot (or a dud) that hits a man: it goes through him
		if ( tr.m_pEnt && ( tr.m_pEnt->IsNPC() || tr.m_pEnt->IsPlayer() ) )
		{
			CTakeDamageInfo dmg( this, this, 300.0f, DMG_BULLET );
			dmg.SetDamagePosition( tr.endpos );
			dmg.SetDamageForce( vecDir * 20000.0f );
			tr.m_pEnt->TakeDamage( dmg );
		}
	}
	// ammunition: a magazine at the cyclic rate, then the reload (longer with the loader gone); rounds carried
	float now = gpGlobals->curtime;
	if ( m_pType->gun.nRounds > 0 )
		m_iRounds = MAX( m_iRounds - 1, 0 );
	m_iMagLeft--;
	bool bRoundsLeft = m_pType->gun.nRounds <= 0 || m_iRounds > 0;
	if ( m_iMagLeft > 0 && bRoundsLeft )
	{
		m_flNextGun = now + 60.0f / MAX( m_pType->gun.flCyclic, 1.0f );
		m_bReloadSoundDone = true;		// no clank between rounds of a magazine
	}
	else
	{
		m_iMagLeft = m_pType->gun.nRounds > 0 ? MIN( m_pType->gun.nMagazine, m_iRounds ) : m_pType->gun.nMagazine;
		m_flNextGun = now + m_pType->gun.flReload * ReloadScale();
		m_bReloadSoundDone = !bRoundsLeft;
	}
	if ( !bRoundsLeft && !m_bOutOfAmmoSaid )
	{
		m_bOutOfAmmoSaid = true;
		Msg( "DOD-SP vehicle %s: out of main-gun ammunition (%d rounds carried) - machine gun only\n", Label(), m_pType->gun.nRounds );
	}
	m_OnFire.FireOutput( m_hTarget.Get(), this );
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s: fired %s at %s - hit %s %.0f units out (%d in the magazine, %d rounds left)\n", Label(), bHE ? "HE" : "AP",
			 VehicleWho( pTarget ), pszHit, flHitDist, m_iMagLeft, m_pType->gun.nRounds > 0 ? m_iRounds : -1 );
}

// V3e: THE one owner of where a machine gun's bullets start: its muzzle on its mount, in the world, now
Vector CDoDSPVehicle::MGMuzzle( int iMG )
{
	const VehicleType_t::MG_t &m = m_pType->mgs[iMG];
	Vector p = m.vecMuzzle, vecWorld;
	if ( m.eMount == VehicleType_t::MG_HULL || !m_hTurret.Get() )
	{
		VectorTransform( p, EntityToWorldTransform(), vecWorld );
		return vecWorld;
	}
	if ( m.eMount == VehicleType_t::MG_BARREL )
	{
		// it rises with the gun: turned about the gun's pivot by the barrel's elevation (+ = up)
		float c = cosf( DEG2RAD( m_flTurretPitch ) ), sn = sinf( DEG2RAD( m_flTurretPitch ) );
		Vector d = p - m.vecPivot;
		p = m.vecPivot + Vector( d.x * c - d.z * sn, d.y, d.x * sn + d.z * c );
	}
	VectorTransform( p, m_hTurret->EntityToWorldTransform(), vecWorld );
	return vecWorld;
}

// a hull gun needs its own gunner (the Tiger's radio operator); turret guns need someone in the turret
bool CDoDSPVehicle::MGManned( int iMG ) const
{
	const VehicleType_t::MG_t &m = m_pType->mgs[iMG];
	if ( !m_pType->armour.bValid )
		return true;
	if ( m.eMount != VehicleType_t::MG_HULL )
		return CanAim();
	return m.iCrew >= 0 ? CrewHas( m.iCrew ) : m_iCrewAlive != 0;
}

bool CDoDSPVehicle::AnyMGUsable( void ) const
{
	for ( int k = 0; k < m_pType->nMGs; k++ )
		if ( MGManned( k ) )
			return true;
	return false;
}

bool CDoDSPVehicle::MGBurstShot( int iMG )
{
	const VehicleType_t::MG_t &m = m_pType->mgs[iMG];
	float now = gpGlobals->curtime;
	if ( now >= m_flMGBurstEnd[iMG] && now >= m_flMGPauseUntil[iMG] )
	{
		m_flMGBurstEnd[iMG] = now + m.flBurst;
		m_flMGPauseUntil[iMG] = m_flMGBurstEnd[iMG] + m.flPause;
	}
	if ( now < m_flMGBurstEnd[iMG] && now >= m_flNextMGShot[iMG] )
	{
		m_flNextMGShot[iMG] = now + 1.0f / MAX( m.flRate, 1.0f );
		return true;
	}
	return false;
}

void CDoDSPVehicle::FireMG( int iMG, const Vector &vecSrc, const Vector &vecDir )
{
	const VehicleType_t::MG_t &m = m_pType->mgs[iMG];
	FireBulletsInfo_t info;
	info.m_iShots = 1;
	info.m_vecSrc = vecSrc;
	info.m_vecDirShooting = vecDir;
	float flHalf = sinf( DEG2RAD( m.flSpread ) * 0.5f );
	info.m_vecSpread = Vector( flHalf, flHalf, flHalf );
	info.m_flDistance = m.flRange;
	info.m_iAmmoType = GetAmmoDef()->Index( "AR2" );
	info.m_iTracerFreq = 2;
	info.m_flDamage = m.flDamage;
	info.m_pAttacker = this;
	FireBullets( info );
	if ( m.szSound[0] )
		EmitSound( m.szSound );
}

// turret / barrel guns: they fire where the gun points, from their own muzzle (infantry only - they can't hurt armour)
void CDoDSPVehicle::FireTurretMGs( CBaseEntity *pTarget, const Vector &vecAimPoint, const Vector &vecBarrel )
{
	if ( dynamic_cast<CDoDSPVehicle *>( pTarget ) )
		return;
	for ( int k = 0; k < m_pType->nMGs; k++ )
	{
		const VehicleType_t::MG_t &m = m_pType->mgs[k];
		if ( m.eMount == VehicleType_t::MG_HULL || !MGManned( k ) )
			continue;
		Vector vecMuzzle = MGMuzzle( k ), vecTo = vecAimPoint - vecMuzzle;
		float flDist = VectorNormalize( vecTo );
		float flErr = RAD2DEG( acosf( clamp( DotProduct( vecBarrel, vecTo ), -1.0f, 1.0f ) ) );
		if ( flErr > VEHICLE_COAX_CONE || flDist > m.flRange || !MGBurstShot( k ) )
			continue;
		CBaseEntity *pFriend = NULL;
		if ( LineOfFire( vecMuzzle, vecAimPoint, pTarget, 0.0f ) && !FriendlyInTheWay( vecMuzzle, vecBarrel, flDist + 64.0f, 0.0f, &pFriend ) )
			FireMG( k, vecMuzzle, vecBarrel );
	}
}

// hull guns (a ball mount: the Tiger's bow MG): the gunner aims on his own, inside the mount's arc
void CDoDSPVehicle::FireHullMGs( CBaseEntity *pTarget, const Vector &vecAimPoint )
{
	if ( dynamic_cast<CDoDSPVehicle *>( pTarget ) )
		return;
	for ( int k = 0; k < m_pType->nMGs; k++ )
	{
		const VehicleType_t::MG_t &m = m_pType->mgs[k];
		if ( m.eMount != VehicleType_t::MG_HULL || !MGManned( k ) )
			continue;
		Vector vecMuzzle = MGMuzzle( k ), vecTo = vecAimPoint - vecMuzzle, vecLocal;
		float flDist = VectorNormalize( vecTo );
		VectorIRotate( vecTo, EntityToWorldTransform(), vecLocal );
		float flYaw = RAD2DEG( atan2f( vecLocal.y, vecLocal.x ) ), flPitch = RAD2DEG( atan2f( vecLocal.z, vecLocal.Length2D() ) );
		if ( fabs( flYaw ) > m.flArc || flPitch < m.flPitchMin || flPitch > m.flPitchMax || flDist > m.flRange || !MGBurstShot( k ) )
			continue;
		CBaseEntity *pFriend = NULL;
		if ( LineOfFire( vecMuzzle, vecAimPoint, pTarget, 0.0f ) && !FriendlyInTheWay( vecMuzzle, vecTo, flDist + 64.0f, 0.0f, &pFriend ) )
			FireMG( k, vecMuzzle, vecTo );
	}
}

void CDoDSPVehicle::WeaponThink( float dt )
{
	if ( !m_pType->gun.bHas && !m_pType->nMGs )
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
		m_flTurretYaw = ApproachAngle( flWantYaw, m_flTurretYaw, TurretRate() * dt );
		float flPitchMin = MIN( m_flPoseMin * m_flPitchSign, m_flPoseMax * m_flPitchSign );
		float flPitchMax = MAX( m_flPoseMin * m_flPitchSign, m_flPoseMax * m_flPitchSign );
		if ( m_iPitchPose < 0 )
			flPitchMin = flPitchMax = 0.0f;
		flWantPitch = clamp( flWantPitch, flPitchMin, flPitchMax );
		m_flTurretPitch = Approach( flWantPitch, m_flTurretPitch, ( CanAim() ? m_pType->flPitchRate : 0.0f ) * dt );
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

	// V3e: hull guns have their own gunner and their own aim (the turret crew may be gone, the turret elsewhere)
	if ( pTarget && !m_bHoldFire && bSeen )
		FireHullMGs( pTarget, vecAimPoint );
	if ( !pTarget || m_bHoldFire || !CanAim() )
		return;
	Vector vecBarrel = BarrelDir();
	Vector vecToTarget = vecAimPoint - MuzzlePos( vecBarrel );
	float flDist = VectorNormalize( vecToTarget );
	float flErr = RAD2DEG( acosf( clamp( DotProduct( vecBarrel, vecToTarget ), -1.0f, 1.0f ) ) );
	if ( !bSeen )
		return;
	// the line from the MUZZLE (it sits forward of and below the pivot the turret sees from): the shell must
	// reach the target or land within half its blast radius of it (each machine gun tests its own line: FireTurretMGs)
	Vector vecMuzzleNow = MuzzlePos( vecBarrel );
	trace_t trShot;
	const DoDSPShell_t *pShell = ShellFor( pTarget );
	float flShellKill = 0.0f, flShellWound = 0.0f;
	if ( pShell && pShell->flFiller > 0.0f && !dynamic_cast<CDoDSPVehicle *>( pTarget ) )
		DoDSP_HERadiiPeople( pShell->flFiller, &flShellKill, &flShellWound );
	bool bShellClear = LineOfFire( vecMuzzleNow, vecAimPoint, pTarget, flShellKill, &trShot );
	CBaseEntity *pFriend = NULL;
	// main gun: at anything hostile, when the barrel is on it. A defensive vehicle stops to shoot; an offensive
	// one shoots on the move too (FireMainGun spreads moving shots wider)
	bool bStill = fabs( m_flSpeed ) < 15.0f;
	if ( MainGunUsable() && pShell && now >= m_flNextGun && flErr <= VEHICLE_GUN_CONE && flDist <= m_pType->gun.flRange &&
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
		else if ( FriendlyInTheWay( MuzzlePos( vecBarrel ), vecBarrel, flDist + 64.0f, flShellWound, &pFriend ) )
		{
			if ( dodsp_vehicle_debug.GetBool() )
				Msg( "DOD-SP vehicle %s: holding fire - %s in the way\n", Label(), VehicleWho( pFriend ) );
			m_flNextGun = now + 1.0f;		// look again in a second
		}
		else
			FireMainGun( vecBarrel );
		return;
	}
	// turret / barrel machine guns: bursts at infantry (not at vehicles - they can't hurt them)
	FireTurretMGs( pTarget, vecAimPoint, vecBarrel );
}

//-----------------------------------------------------------------------------
// V4: armour, damage, crews, wrecks. Owner's calls 2026-11-20: fully real armour and guns, HE at half its real
// size against people, damage to parts AND crew, crews bail out (German crews come out as soldiers; DOD-SP has no
// Allied infantry yet). One owner each:
//   whether a hit gets in  - TakeArmourHit (shells, bullets) / BlastHit (blasts), using dodsp_vehicle_armour
//   what it breaks         - ApplyPenetration (structure, the crit table, crew casualties)
//   who is left            - m_iCrewAlive (KillCrew / BailOut), and what each role does (CanAim, ReloadScale ...)
//   what the vehicle is    - m_eState (fighting / burning / abandoned / wrecked)
//-----------------------------------------------------------------------------

// What a penetration of a plate breaks: weights per crit (AMMO ENGINE DRIVE TURRET GUN CREW) for a tank with its
// engine at the back - ammunition along the hull sides and in the turret, transmission at the front, engine at the
// rear. RollCrit moves the engine for front / middle engines and drops what the vehicle hasn't got. Game values.
static const float s_flCritHull[NUM_AFACES][6] =
{
	{ 25,  0, 35,  0,  0, 40 },		// front
	{ 45, 15, 20,  0,  0, 20 },		// side
	{ 15, 70, 10,  0,  0,  5 },		// rear
	{ 25, 40,  0,  0,  0, 35 },		// top
	{ 20, 20, 30,  0,  0, 30 },		// bottom
};
static const float s_flCritTurret[NUM_AFACES][6] =
{
	{ 15,  0,  0, 20, 35, 30 },
	{ 25,  0,  0, 25, 15, 35 },
	{ 30,  0,  0, 25, 10, 35 },
	{ 15,  0,  0, 15, 10, 60 },
	{ 15,  0,  0, 15, 10, 60 },
};
static const float s_flCritOpenTop[6] = { 5, 10, 0, 0, 0, 85 };	// into an open compartment: mostly the men

int CDoDSPVehicle::RollCrit( int ePart, int eFace )
{
	const DoDSPArmour_t &ar = m_pType->armour;
	float w[6];
	if ( ePart == APART_TURRET )
		memcpy( w, s_flCritTurret[eFace], sizeof( w ) );
	else if ( eFace == AFACE_TOP && ar.bOpenTop )
		memcpy( w, s_flCritOpenTop, sizeof( w ) );
	else
	{
		int eRow = eFace;
		if ( ar.iEngineAt == AFACE_FRONT && ( eFace == AFACE_FRONT || eFace == AFACE_REAR ) )
			eRow = ( eFace == AFACE_FRONT ) ? AFACE_REAR : AFACE_FRONT;		// engine and transmission swap ends
		memcpy( w, s_flCritHull[eRow], sizeof( w ) );
		if ( ar.iEngineAt == AFACE_TOP )		// engine in the middle (carrier): behind the sides, less at the back
		{
			if ( eFace == AFACE_REAR )
			{
				w[1] -= 35.0f;
				w[2] += 15.0f;
				w[5] += 20.0f;
			}
			else if ( eFace == AFACE_SIDE )
			{
				w[1] += 15.0f;
				w[0] -= 15.0f;
			}
		}
	}
	// what it hasn't got can't be hit
	if ( !m_hTurret.Get() )
	{
		w[5] += w[3];
		w[3] = 0.0f;
	}
	if ( !m_pType->gun.bHas )
	{
		w[5] += w[4] + w[0] * 0.5f;
		w[4] = 0.0f;
		w[0] *= 0.5f;
	}
	float flSum = 0.0f;
	for ( int i = 0; i < 6; i++ )
		flSum += MAX( w[i], 0.0f );
	float r = random->RandomFloat( 0.0f, flSum );
	for ( int i = 0; i < 6; i++ )
	{
		r -= MAX( w[i], 0.0f );
		if ( r <= 0.0f && w[i] > 0.0f )
			return VCRIT_AMMO + i;
	}
	return VCRIT_CREW;
}

int CDoDSPVehicle::CrewAlive( void ) const
{
	int n = 0;
	for ( int i = 0; i < DODSP_MAX_CREW; i++ )
		if ( m_iCrewAlive & ( 1 << i ) )
			n++;
	return n;
}

bool CDoDSPVehicle::CrewHas( int eRole ) const
{
	for ( int i = 0; i < m_pType->armour.nCrew; i++ )
		if ( ( m_iCrewAlive & ( 1 << i ) ) && m_pType->armour.eCrew[i] == eRole )
			return true;
	return false;
}

bool CDoDSPVehicle::TypeHasRole( int eRole ) const
{
	for ( int i = 0; i < m_pType->armour.nCrew; i++ )
		if ( m_pType->armour.eCrew[i] == eRole )
			return true;
	return false;
}

// someone in the turret to lay the gun (the commander or the loader takes over from a dead gunner)
bool CDoDSPVehicle::CanAim( void ) const
{
	if ( !m_pType->armour.bValid )
		return true;
	return CrewHas( CREW_GUNNER ) || CrewHas( CREW_COMMANDER ) || CrewHas( CREW_LOADER );
}

float CDoDSPVehicle::ReloadScale( void ) const
{
	if ( !m_pType->armour.bValid )
		return 1.0f;
	if ( TypeHasRole( CREW_LOADER ) && !CrewHas( CREW_LOADER ) )
		return 2.0f;		// the gunner or commander loads too
	return 1.0f;
}

float CDoDSPVehicle::AimPenalty( void ) const
{
	if ( m_pType->armour.bValid && TypeHasRole( CREW_GUNNER ) && !CrewHas( CREW_GUNNER ) )
		return 2.0f;
	return 1.0f;
}

float CDoDSPVehicle::TurretRate( void ) const
{
	if ( m_bTurretJammed || !CanAim() )
		return 0.0f;
	return m_pType->flTurretRate * ( AimPenalty() > 1.0f ? 0.6f : 1.0f );
}

bool CDoDSPVehicle::MainGunUsable( void ) const
{
	return m_pType->gun.bHas && !m_bGunOut && CanAim() && ( m_pType->gun.nRounds <= 0 || m_iRounds > 0 );
}

// AP at vehicles, HE at everything else (what a crew loaded), whichever it has if it has only one
const DoDSPShell_t *CDoDSPVehicle::ShellFor( CBaseEntity *pTarget ) const
{
	if ( !m_pType->gun.bHas )
		return NULL;
	const DoDSPShell_t *pAP = m_pType->gun.ap.bValid ? &m_pType->gun.ap : NULL;
	const DoDSPShell_t *pHE = m_pType->gun.he.bValid ? &m_pType->gun.he : NULL;
	if ( dynamic_cast<CDoDSPVehicle *>( pTarget ) )
		return pAP ? pAP : pHE;
	return pHE ? pHE : pAP;
}

// Where a ray meets this vehicle's armour: the turret's box (cut at its own body - the model's box reaches out to
// the muzzle) and the hull's (cut halfway up the turret's lower part, so a shot at the turret meets the turret).
// The face entered, its plate leaned back by its slope. One owner for "which plate" - shells, bullets, the
// can-it-hurt test and the pen chart all ask this.
bool CDoDSPVehicle::ShellHitBox( const Vector &vecFrom, const Vector &vecDir, float flMax, VehicleHit_t *pHit )
{
	if ( !m_pType || !m_pType->armour.bValid || m_eState == VSTATE_WRECKED )
		return false;
	bool bHit = false;
	float flBest = flMax;
	CBaseAnimating *pT = dynamic_cast<CBaseAnimating *>( m_hTurret.Get() );
	CStudioHdr *pTHdr = pT ? pT->GetModelPtr() : NULL;
	for ( int ePart = 0; ePart < NUM_APARTS; ePart++ )
	{
		CBaseEntity *pPart = this;
		Vector vecMins, vecMaxs;
		float flFwd;
		if ( ePart == APART_TURRET )
		{
			if ( !pTHdr )
				continue;
			vecMins = pTHdr->hull_min();
			vecMaxs = pTHdr->hull_max();
			flFwd = m_flTurretFwdOffset;
			Vector f( cosf( DEG2RAD( flFwd ) ), sinf( DEG2RAD( flFwd ) ), 0.0f );
			int a = ( fabs( f.x ) >= fabs( f.y ) ) ? 0 : 1;
			if ( f[a] > 0.0f )
				vecMaxs[a] = MIN( vecMaxs[a], -vecMins[a] * 1.2f );
			else
				vecMins[a] = MAX( vecMins[a], -vecMaxs[a] * 1.2f );
			pPart = pT;
		}
		else
		{
			vecMins = CollisionProp()->OBBMins();
			vecMaxs = CollisionProp()->OBBMaxs();
			if ( pTHdr )
				vecMaxs.z = MIN( vecMaxs.z, pT->GetLocalOrigin().z + 0.5f * pTHdr->hull_min().z );
			flFwd = m_pType->flForwardYaw;
		}
		const matrix3x4_t &xf = pPart->EntityToWorldTransform();
		Vector o, d;
		VectorITransform( vecFrom, xf, o );
		VectorIRotate( vecDir, xf, d );
		float t;
		int iAxis, iSign;
		if ( !DoDSP_RayBox( o, d, vecMins, vecMaxs, &t, &iAxis, &iSign ) || t > flBest )
			continue;
		Vector n( 0.0f, 0.0f, 0.0f );
		n[iAxis] = (float)iSign;
		Vector f( cosf( DEG2RAD( flFwd ) ), sinf( DEG2RAD( flFwd ) ), 0.0f );
		int eFace;
		if ( iAxis == 2 )
			eFace = ( iSign > 0 ) ? AFACE_TOP : AFACE_BOTTOM;
		else
		{
			float dp = DotProduct( n, f );
			eFace = ( dp > 0.7f ) ? AFACE_FRONT : ( dp < -0.7f ) ? AFACE_REAR : AFACE_SIDE;
		}
		const ArmourPlate_t &plate = m_pType->armour.plates[ePart][eFace];
		if ( iAxis != 2 && plate.flSlope != 0.0f )
		{
			float flSlope = DEG2RAD( plate.flSlope );
			n = n * cosf( flSlope ) + Vector( 0.0f, 0.0f, sinf( flSlope ) );
		}
		Vector vecNormal;
		VectorRotate( n, xf, vecNormal );
		pHit->ePart = ePart;
		pHit->eFace = eFace;
		pHit->t = t;
		pHit->vecPoint = vecFrom + vecDir * t;
		pHit->vecNormal = vecNormal;
		flBest = t;
		bHit = true;
	}
	return bHit;
}

CDoDSPVehicle *CDoDSPVehicle::FindShellHit( const Vector &vecFrom, const Vector &vecDir, float flMax, CDoDSPVehicle *pSkip, VehicleHit_t *pHit )
{
	CDoDSPVehicle *pBest = NULL;
	float flBest = flMax;
	for ( int i = 0; i < s_All.Count(); i++ )
	{
		CDoDSPVehicle *pV = s_All[i];
		VehicleHit_t h;
		if ( pV != pSkip && pV->ShellHitBox( vecFrom, vecDir, flBest, &h ) )
		{
			pBest = pV;
			flBest = h.t;
			*pHit = h;
		}
	}
	return pBest;
}

// Does it get in? The plate's effective thickness on this path against the shell's penetration (+-10%); past
// VEHICLE_RICOCHET_ANGLE it skids off unless it overmatches the plate. An open top has no plate. Penetrations are
// always printed (they change the fight); the rest only with dodsp_vehicle_debug.
bool CDoDSPVehicle::TakeArmourHit( const VehicleHit_t &hit, const Vector &vecPath, float flPen, float flEffect, float flCalibre,
								   CBaseEntity *pAttacker, const char *pszWhat, bool bQuiet, float flFiller )
{
	if ( !m_pType || !m_pType->armour.bValid || m_eState == VSTATE_WRECKED )
		return false;
	ArmourPlate_t plate = m_pType->armour.plates[hit.ePart][hit.eFace];
	bool bOpen = hit.ePart == APART_HULL && hit.eFace == AFACE_TOP && m_pType->armour.bOpenTop;
	if ( bOpen )
		plate.flMM = 0.0f;
	float flAngle = 0.0f;
	float flEff = DoDSP_EffectiveArmour( plate, vecPath, hit.vecNormal, &flAngle );
	bool bRicochet = plate.flMM > 0.0f && flAngle > VEHICLE_RICOCHET_ANGLE && flCalibre < 3.0f * plate.flMM;
	float flRoll = flPen * random->RandomFloat( 0.9f, 1.1f );
	bool bIn = !bRicochet && flRoll >= flEff;
	if ( ( bIn && ( !bQuiet || dodsp_vehicle_debug.GetBool() ) ) || ( !bIn && !bQuiet && dodsp_vehicle_debug.GetBool() ) )
		Msg( "DOD-SP vehicle %s: %s hit its %s %s%s at %.0f deg (%.0f mm = %.0f mm effective) with %.0f mm penetration - %s\n", Label(),
			 pszWhat, g_pszArmourPart[hit.ePart], g_pszArmourFace[hit.eFace], bOpen ? " (open)" : "", flAngle, plate.flMM, flEff,
			 flRoll, bIn ? "IN" : bRicochet ? "ricochet" : "stopped" );
	if ( flCalibre >= VEHICLE_HITFX_MIN_CALIBRE )
		HitFeedback( bIn ? VHITFX_IN : bRicochet ? VHITFX_RICOCHET : VHITFX_STOPPED, hit, vecPath, flCalibre );
	if ( bIn )
		ApplyPenetration( hit.ePart, hit.eFace, flEffect, pAttacker, pszWhat );
	else if ( flFiller >= 0.5f )
		ExternalBlast( flFiller, 1.0f, pAttacker );
	UpdateDamageSmoke();
	return bIn;
}

// V4h (owner 2026-11-20: "extremely hard to tell what's happening on screen"): every shell that strikes the armour SHOWS
// how it went, from THE one place every armour hit is judged (TakeArmourHit) - so no outcome can be silent:
//   in        a white-hot flash and a shower of sparks, a deep metallic CLANG, the hull jolts away from the hit
//   stopped   sparks and a high clang (the shell's own burst, if it has a filler, is FireMainGun's as before)
//   ricochet  sparks thrown off along the deflected path and a whine
// plus the damage smoke (UpdateDamageSmoke) that stays on a hurt vehicle. Bullets (< VEHICLE_HITFX_MIN_CALIBRE mm)
// keep the engine's own bullet impacts.
void CDoDSPVehicle::HitFeedback( int eFx, const VehicleHit_t &hit, const Vector &vecPath, float flCalibre )
{
	float k = clamp( flCalibre / 75.0f, 0.3f, 2.0f );
	Vector vecOut = hit.vecNormal;
	if ( eFx == VHITFX_RICOCHET )
	{
		vecOut = vecPath - 2.0f * DotProduct( vecPath, hit.vecNormal ) * hit.vecNormal;	// the deflected path
		VectorNormalize( vecOut );
	}
	g_pEffects->Sparks( hit.vecPoint, (int)( 2 + 4 * k ), (int)( 2 + 3 * k ), &vecOut );
	g_pEffects->MetalSparks( hit.vecPoint, vecOut );
	CPVSFilter filter( hit.vecPoint );
	if ( eFx == VHITFX_IN )
	{
		te->DynamicLight( filter, 0.0f, &hit.vecPoint, 255, 200, 120, 3, 220.0f * k, 0.25f, 0.0f );
		g_pEffects->Sparks( hit.vecPoint, (int)( 4 + 6 * k ), (int)( 3 + 3 * k ), &hit.vecNormal );
		// the hull rocks back from the blow: nose up when hit from ahead, down from behind, a jolt from the side
		// (a sprung body rolls away from a side hit; the hull spring has no roll, so it jolts)
		Vector vecFwd, vecLeft;
		AngleVectors( GetAbsAngles(), &vecFwd, &vecLeft, NULL );
		vecLeft = -vecLeft;		// AngleVectors gives right
		float flAlong = -DotProduct( vecPath, vecFwd ), flSide = DotProduct( vecPath, vecLeft );
		BodyKick( k * ( 30.0f * flAlong + ( m_bBodySprung ? 0.0f : ( random->RandomInt( 0, 1 ) ? 10.0f : -10.0f ) * ( 1.0f - fabs( flAlong ) ) ) ),
				  k * 30.0f * -flSide );
	}
	else
		te->DynamicLight( filter, 0.0f, &hit.vecPoint, 255, 220, 160, 2, 90.0f * k, 0.1f, 0.0f );
	CPASAttenuationFilter snd( hit.vecPoint, VEHICLE_HIT_SOUNDLEVEL );
	EmitSound( snd, entindex(), s_pszHitSound[eFx], &hit.vecPoint );
}

// V4h: smoke from the engine deck while it's hurt - light grey under 2/3 of its structure, thick black under 1/3 - so a
// damaged tank reads as damaged from across the map. One owner of the plume; it stays through burning and the wreck.
void CDoDSPVehicle::UpdateDamageSmoke( void )
{
	if ( !m_pType || !m_pType->armour.bValid || m_pType->armour.flStructure <= 0.0f )
		return;
	float f = m_flStructure / m_pType->armour.flStructure;
	bool bBurning = ( m_eState == VSTATE_BURNING || m_eState == VSTATE_WRECKED );
	int iLevel = ( bBurning || f < 0.34f ) ? 2 : ( f < 0.67f ) ? 1 : 0;		// H2b: abandoned intact = no smoke
	if ( iLevel <= m_iDamageSmoke && m_hDamageSmoke.Get() )
		return;
	if ( iLevel == 0 )
		return;
	if ( !m_hDamageSmoke.Get() )
	{
		SmokeTrail *pSmoke = SmokeTrail::CreateSmokeTrail();
		if ( !pSmoke )
			return;
		float x = m_pType->armour.iEngineAt == AFACE_FRONT ? 0.6f : m_pType->armour.iEngineAt == AFACE_REAR ? -0.6f : 0.0f;
		pSmoke->SetLifetime( -1 );
		pSmoke->SetParent( this );
		pSmoke->SetLocalOrigin( Vector( x * m_flHalfLength, 0.0f, m_flBodyHeight * 0.75f ) );
		pSmoke->SetLocalAngles( QAngle( -90.0f, 0.0f, 0.0f ) );		// "directed" speed = straight up
		pSmoke->SetMoveType( MOVETYPE_NONE );
		m_hDamageSmoke = pSmoke;
	}
	SmokeTrail *pSmoke = m_hDamageSmoke.Get();
	bool bHeavy = iLevel >= 2;
	pSmoke->m_SpawnRate = bHeavy ? 24.0f : 8.0f;
	pSmoke->m_ParticleLifetime = bHeavy ? 4.0f : 3.0f;
	pSmoke->m_StartColor.Init( bHeavy ? 0.12f : 0.45f, bHeavy ? 0.12f : 0.45f, bHeavy ? 0.12f : 0.45f );
	pSmoke->m_EndColor.Init( bHeavy ? 0.05f : 0.6f, bHeavy ? 0.05f : 0.6f, bHeavy ? 0.05f : 0.6f );
	pSmoke->m_StartSize = bHeavy ? 32.0f : 16.0f;
	pSmoke->m_EndSize = bHeavy ? 160.0f : 96.0f;
	pSmoke->m_SpawnRadius = 16.0f;
	pSmoke->m_Opacity = bHeavy ? 0.6f : 0.35f;
	pSmoke->m_MinSpeed = 4.0f;
	pSmoke->m_MaxSpeed = 16.0f;
	pSmoke->m_MinDirectedSpeed = bHeavy ? 60.0f : 40.0f;
	pSmoke->m_MaxDirectedSpeed = bHeavy ? 100.0f : 70.0f;
	pSmoke->SetEmit( true );
	m_iDamageSmoke = iLevel;
}

// A big charge bursting outside: tracks, the gun, the turret ring - and the very big ones (152 mm) stun or kill
// the crew through the plate ("dislodging the turret of a Tiger" - SU-152 article).
void CDoDSPVehicle::ExternalBlast( float flFiller, float k, CBaseEntity *pAttacker )
{
	if ( m_eState == VSTATE_WRECKED )
		return;
	float p = clamp( flFiller / 10.0f, 0.0f, 0.6f ) * k;
	if ( random->RandomFloat() < p )
	{
		int e = random->RandomInt( 0, 2 );
		if ( e == 1 && m_pType->gun.bHas && !m_bGunOut )
		{
			m_bGunOut = true;
			Msg( "DOD-SP vehicle %s: the blast wrecked its main gun\n", Label() );
		}
		else if ( e == 2 && m_hTurret.Get() && !m_bTurretJammed )
		{
			m_bTurretJammed = true;
			Msg( "DOD-SP vehicle %s: the blast jammed its turret\n", Label() );
		}
		else
			SetImmobile( "the blast broke a track / wheel", pAttacker );
	}
	if ( flFiller >= 4.0f && k > 0.8f && random->RandomFloat() < 0.25f )
	{
		KnockOut( false, pAttacker, "the blast knocked the crew out" );
		return;
	}
	CheckMorale( pAttacker );
}

// A blast near the vehicle (our shells' bursts and anything else that explodes): the plate facing it against what
// the charge pierces at that distance (REAL size - armour doesn't care about balance); into an open top the
// fragments reach the crew out to the wound radius.
void CDoDSPVehicle::BlastHit( const Vector &vecCentre, float flFiller, CBaseEntity *pAttacker )
{
	if ( flFiller <= 0.0f )
		return;
	float flKill, flWound;
	DoDSP_HERadiiReal( flFiller, &flKill, &flWound );
	Vector vecLocal;
	VectorITransform( vecCentre, EntityToWorldTransform(), vecLocal );
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	Vector vecNear;
	int iAxis = 2, iSign = 1;
	float flOut = 0.0f;
	for ( int a = 0; a < 3; a++ )
	{
		vecNear[a] = clamp( vecLocal[a], vecMins[a], vecMaxs[a] );
		float o = fabs( vecLocal[a] - vecNear[a] ) / MAX( vecMaxs[a] - vecMins[a], 1.0f );
		if ( o > flOut )
		{
			flOut = o;
			iAxis = a;
			iSign = ( vecLocal[a] > vecMaxs[a] ) ? 1 : -1;
		}
	}
	float flDist = ( vecLocal - vecNear ).Length();
	if ( flDist >= flWound )
		return;
	int eFace;
	if ( iAxis == 2 )
		eFace = ( iSign > 0 ) ? AFACE_TOP : AFACE_BOTTOM;
	else
	{
		Vector f( cosf( DEG2RAD( m_pType->flForwardYaw ) ), sinf( DEG2RAD( m_pType->flForwardYaw ) ), 0.0f );
		Vector n( 0.0f, 0.0f, 0.0f );
		n[iAxis] = (float)iSign;
		float dp = DotProduct( n, f );
		eFace = ( dp > 0.7f ) ? AFACE_FRONT : ( dp < -0.7f ) ? AFACE_REAR : AFACE_SIDE;
	}
	char szWhat[64];
	Q_snprintf( szWhat, sizeof( szWhat ), "a %.2f kg blast %.0f units off", flFiller, flDist );
	// fragments into an open compartment from anywhere above its sides
	if ( m_pType->armour.bOpenTop && vecLocal.z > vecMaxs.z * 0.5f && m_eState != VSTATE_BURNING )
	{
		float k = 1.0f - flDist / flWound;
		if ( random->RandomFloat() < 0.6f * k )
		{
			m_flStructure -= 10.0f * k;
			KillCrew( 1, APART_HULL, "fragments into the open top" );
			CheckMorale( pAttacker );
		}
	}
	if ( flDist >= flKill || m_eState == VSTATE_WRECKED )
		return;
	float k = 1.0f - flDist / flKill;
	float flPen = DoDSP_HEPen( flFiller ) * k;
	const ArmourPlate_t &plate = m_pType->armour.plates[APART_HULL][eFace];
	if ( flPen >= plate.flMM )
	{
		if ( dodsp_vehicle_debug.GetBool() )
			Msg( "DOD-SP vehicle %s: %s pierced its hull %s (%.0f mm vs %.0f mm)\n", Label(), szWhat, g_pszArmourFace[eFace], flPen, plate.flMM );
		ApplyPenetration( APART_HULL, eFace, 60.0f * powf( flFiller / 0.68f, 1.0f / 3.0f ) * k, pAttacker, szWhat );
	}
	else if ( flFiller >= 0.5f )
		ExternalBlast( flFiller, k, pAttacker );
}

// What got in: the structure takes the round's effect (+-25%); spall kills crew; one part in the crit table may be
// hit (big rounds always, bullets seldom). Every penetration prints one line.
void CDoDSPVehicle::ApplyPenetration( int ePart, int eFace, float flEffect, CBaseEntity *pAttacker, const char *pszWhat )
{
	if ( m_eState == VSTATE_WRECKED )
		return;
	float now = gpGlobals->curtime;
	if ( pAttacker )
		m_hKiller = pAttacker;
	m_flStructure -= flEffect * random->RandomFloat( 0.75f, 1.25f );
	m_flPenTimes[1] = m_flPenTimes[0];
	m_flPenTimes[0] = now;
	m_OnPenetrated.FireOutput( pAttacker, this );
	int nCas = 0;
	if ( random->RandomFloat() < MIN( 0.8f, flEffect / 150.0f ) )
		nCas++;
	if ( flEffect > 150.0f && random->RandomFloat() < 0.5f )
		nCas++;
	int eCrit = VCRIT_NONE;
	if ( random->RandomFloat() < clamp( flEffect / 40.0f, 0.05f, 1.0f ) )
		eCrit = RollCrit( ePart, eFace );
	int eFire = 0;		// 1 ammunition, 2 engine
	switch ( eCrit )
	{
	case VCRIT_AMMO:
		if ( random->RandomFloat() < m_pType->armour.flAmmoFire )
			eFire = 1;
		else
			nCas++;
		break;
	case VCRIT_ENGINE:
		SetImmobile( "engine hit", pAttacker );
		if ( random->RandomFloat() < m_pType->armour.flFuelFire )
			eFire = 2;
		break;
	case VCRIT_DRIVE:
		SetImmobile( "transmission / tracks hit", pAttacker );
		break;
	case VCRIT_TURRET:
		m_bTurretJammed = true;
		break;
	case VCRIT_GUN:
		m_bGunOut = true;
		break;
	case VCRIT_CREW:
		nCas++;
		break;
	}
	if ( flEffect >= 10.0f || eCrit != VCRIT_NONE || nCas || dodsp_vehicle_debug.GetBool() )		// not every bullet
		Msg( "DOD-SP vehicle %s: PENETRATED (%s %s) by %s - %s%s; structure %.0f of %.0f\n", Label(), g_pszArmourPart[ePart],
			 g_pszArmourFace[eFace], pszWhat, s_pszCrit[eCrit], eFire == 1 ? " - it catches fire" : eFire == 2 ? " - the engine catches fire" : "",
			 m_flStructure, m_pType->armour.flStructure );
	if ( nCas )
		KillCrew( nCas, ePart, "hit by the round / spall" );
	if ( m_flStructure <= 0.0f )
	{
		KnockOut( m_flStructure < -0.5f * m_pType->armour.flStructure, pAttacker, "too much damage inside" );
		return;
	}
	if ( eFire )
	{
		SetBurning( eFire == 1, pAttacker );
		return;
	}
	CheckMorale( pAttacker );
}

void CDoDSPVehicle::KillCrew( int n, int ePart, const char *pszWhy )
{
	const DoDSPArmour_t &ar = m_pType->armour;
	for ( int k = 0; k < n; k++ )
	{
		int iPick = -1;
		// the men in the part that was hit first (turret: commander, gunner, loader; hull: driver, radio)
		for ( int pass = 0; pass < 2 && iPick < 0; pass++ )
		{
			int nCand = 0;
			for ( int i = 0; i < ar.nCrew; i++ )
			{
				if ( !( m_iCrewAlive & ( 1 << i ) ) )
					continue;
				int e = ar.eCrew[i];
				bool bTurretMan = ( e == CREW_COMMANDER || e == CREW_GUNNER || e == CREW_LOADER );
				if ( pass == 0 && bTurretMan != ( ePart == APART_TURRET ) )
					continue;
				if ( random->RandomInt( 0, nCand++ ) == 0 )
					iPick = i;
			}
		}
		if ( iPick < 0 )
			break;
		m_iCrewAlive &= ~( 1 << iPick );
		Msg( "DOD-SP vehicle %s: its %s is killed (%s) - crew %d of %d left\n", Label(), g_pszCrewRole[ar.eCrew[iPick]], pszWhy,
			 CrewAlive(), ar.nCrew );
	}
	if ( !CrewHas( CREW_DRIVER ) && !CrewHas( CREW_RADIO ) )
		SetImmobile( "no one left to drive it", NULL );
}

void CDoDSPVehicle::SetImmobile( const char *pszWhy, CBaseEntity *pAttacker )
{
	if ( m_bImmobile )
		return;
	m_bImmobile = true;
	if ( m_bHasGoal )
		Stop();
	Msg( "DOD-SP vehicle %s: IMMOBILISED - %s\n", Label(), pszWhy );
	m_OnImmobilized.FireOutput( pAttacker ? pAttacker : this, this );
}

void CDoDSPVehicle::FireKnockedOut( CBaseEntity *pAttacker )
{
	if ( m_bOutputKO )
		return;
	m_bOutputKO = true;
	m_OnKnockedOut.FireOutput( pAttacker ? pAttacker : this, this );
}

// One env_fire on the vehicle: the engine deck for an engine fire, the fighting compartment otherwise
void CDoDSPVehicle::MakeFire( float flSize, bool bEngine )
{
	if ( m_hFire.Get() )
		return;
	CBaseEntity *pFire = CreateEntityByName( "env_fire" );
	if ( !pFire )
		return;
	float flAlong = 0.0f;
	if ( bEngine )
		flAlong = ( m_pType->armour.iEngineAt == AFACE_FRONT ? 0.6f : m_pType->armour.iEngineAt == AFACE_REAR ? -0.6f : 0.0f ) * m_flHalfLength;
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	Vector vecAt = GetAbsOrigin() + vecFwd * flAlong + Vector( 0.0f, 0.0f, m_flBodyHeight * 0.6f );
	char szSize[16];
	Q_snprintf( szSize, sizeof( szSize ), "%.0f", flSize );
	pFire->KeyValue( "firesize", szSize );
	pFire->KeyValue( "fireattack", "1" );
	pFire->KeyValue( "firetype", "0" );
	pFire->KeyValue( "damagescale", "1.0" );
	pFire->KeyValue( "spawnflags", "13" );		// infinite (we put it out), start on, start full
	pFire->SetAbsOrigin( vecAt );
	DispatchSpawn( pFire );
	pFire->Activate();
	pFire->SetParent( this );
	m_hFire = pFire;
}

// THE state change (one owner): leaving the fight stops it dead - the vehicle moves by its velocity (pusher), so a
// knocked-out tank that kept its last velocity drove on through the ground forever (V4 test 2026-11-20). An
// abandoned vehicle catches fire a few seconds later: every vehicle out of the fight ends as a burning wreck the
// player can see (owner: "I didn't even know a tank was downed").
void CDoDSPVehicle::SetState( int eNew, CBaseEntity *pAttacker )
{
	if ( m_eState == eNew )
		return;
	int eOld = m_eState;
	m_eState = eNew;
	if ( dodsp_vehicle_debug.GetBool() )
		Msg( "DOD-SP vehicle %s: %s -> %s\n", Label(), s_pszVehicleState[eOld], s_pszVehicleState[eNew] );
	if ( eNew == VSTATE_FIGHTING )
		return;
	if ( eOld == VSTATE_FIGHTING )
	{
		Stop();
		StopSounds();
	}
	m_bImmobile = true;
	HaltMotion();
	FireKnockedOut( pAttacker );
	// H2b (owner 2026-11-21): an ABANDONED vehicle stays as it is - intact, stranded, for a crew to take over later
	// (Battlefield-style). V4b set every abandoned vehicle alight 4-8 s on; now only a fire it really has burns it.
	UpdateDamageSmoke();		// smoke as damaged as it is (thick only when burning or badly hit)
}

// H1: THE way a blow or the main gun rocks the body (lint: nothing else adds to m_flSuspVel). A sprung body is rocked
// on the client: the kick is networked (m_nBodyKicks counts them). Pitch nose up +, roll right side down +.
void CDoDSPVehicle::BodyKick( float flPitchVel, float flRollVel )
{
	if ( m_bBodySprung )
	{
		m_flBodyKickPitch = flPitchVel;
		m_flBodyKickRoll = flRollVel;
		m_nBodyKicks = ( m_nBodyKicks + 1 ) & 0xff;
		return;
	}
	m_flSuspVel += flPitchVel;
}

void CDoDSPVehicle::HaltMotion( void )
{
	m_flSpeed = 0.0f;
	m_flTrackSpeed = 0.0f;
	m_flSuspVel = 0.0f;
	SetAbsVelocity( vec3_origin );
	SetLocalAngularVelocity( vec3_angle );
}

//-----------------------------------------------------------------------------
// V3 riders. The vehicle owns WHEN they get off (this); the soldier owns HOW (npc_german_soldier.cpp: the seat's
// idle, the get-out on its ride path). Seats, animations and delays: the type's "seats" (dodsp_vehicles.txt),
// previewed against the meshes with dodsp_tools/vehicle_preview/rider_preview.py before they go in.
//-----------------------------------------------------------------------------
int CDoDSPVehicle::RidersAboard( void )
{
	int n = 0;
	for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
	{
		CBaseEntity *pRider = m_hRider[i].Get();
		if ( pRider && GermanSoldier_RideState( pRider ) == 0 )
		{
			// dead, or off and on his own; H3: dead IN his seat (still on us) = slumped there, gone with the vehicle
			if ( !pRider->IsAlive() && pRider->GetMoveParent() == this )
				m_hRiderCorpse[i] = pRider;
			m_hRider[i] = NULL;
		}
		if ( m_hRider[i].Get() )
			n++;
	}
	return n;
}

void CDoDSPVehicle::SpawnRiders( void )
{
	m_bRidersSpawned = true;
	if ( !m_pType->seats.Count() )
	{
		Msg( "DOD-SP vehicle %s: \"riders\" is set but a %s has no seats in %s\n", Label(), m_pType->szName, DODSP_VEHICLE_SCRIPT );
		return;
	}
	// riders are soldiers of its side - DOD-SP has German soldiers only so far
	if ( m_eFaction != DoDSP_FactionFromName( "axis" ) )
	{
		Msg( "DOD-SP vehicle %s: \"riders\" is set, but its side (%s) has no soldier NPC yet - riders on Allied vehicles come with the Allied soldiers\n",
			 Label(), DoDSP_FactionName( m_eFaction ) );
		return;
	}
	char szSquad[32];
	Q_snprintf( szSquad, sizeof( szSquad ), "riders%d", entindex() );
	int n = 0;
	for ( int i = 0; i < m_pType->seats.Count(); i++ )
	{
		const VehicleType_t::Seat_t &seat = m_pType->seats[i];
		CBaseEntity *pNPC = CreateEntityByName( "npc_german_soldier" );
		if ( !pNPC )
			break;
		Vector vecWorld;
		VectorTransform( seat.vecOrigin, EntityToWorldTransform(), vecWorld );
		pNPC->KeyValue( "additionalequipment", i == 0 ? "weapon_mp40" : "weapon_k98" );	// the first seat: the squad leader's MP40
		pNPC->KeyValue( "squadname", szSquad );
		pNPC->AddSpawnFlags( SF_NPC_FALL_TO_GROUND );		// no snap to the floor: he's on the vehicle
		pNPC->SetAbsOrigin( vecWorld );
		pNPC->SetAbsAngles( QAngle( 0.0f, GetAbsAngles().y + seat.flYaw, 0.0f ) );
		DispatchSpawn( pNPC );
		pNPC->Activate();
		if ( !GermanSoldier_BeginRide( pNPC, this, seat.vecOrigin, seat.flYaw, seat.szIdle, seat.szGetout, seat.szFidget ) )
		{
			UTIL_Remove( pNPC );		// the soldier said why (once per map)
			break;
		}
		// H3: the aisle men's walk, the run to the side, the death in the seat
		GermanSoldier_SetRideWalk( pNPC, seat.bWalk ? &seat.vecWalk : NULL, seat.flWalkYaw );
		GermanSoldier_SetRideRally( pNPC, seat.bRally ? &seat.vecRally : NULL );
		GermanSoldier_SetSeatDeath( pNPC, seat.szDeath );
		m_hRider[i] = pNPC;
		m_flRiderGo[i] = 0.0f;
		n++;
	}
	m_eRiders = n ? VRIDERS_ABOARD : VRIDERS_NONE;
	Msg( "DOD-SP vehicle %s: %d of %d riders aboard (they get off: %s)\n", Label(), n, m_pType->seats.Count(),
		 m_eDismount == VDISMOUNT_ORDER ? "on the Dismount input, or when it's knocked out" : "when an enemy is close, on the Dismount input, or when it's knocked out" );
}

void CDoDSPVehicle::OrderDismount( const char *pszWhy )
{
	if ( m_eRiders != VRIDERS_ABOARD )
		return;
	m_eRiders = VRIDERS_STOPPING;
	Msg( "DOD-SP vehicle %s: riders get off - %s\n", Label(), pszWhy );
}

void CDoDSPVehicle::InputDismount( inputdata_t &inputdata )
{
	if ( m_eRiders == VRIDERS_ABOARD )
		OrderDismount( "ordered (Dismount input)" );
	else
		m_bDismountOrder = true;		// riders not aboard yet (spawning): they get off as soon as they are
}

//-----------------------------------------------------------------------------
// H3: the doors the riders get out through. THE owner of the door pose: the type's "doors" curve (CoD2's own -
// shut to frame 46, kicked open in 5 frames, bounce to 117 degrees, settle at 101), started by the dismount order
// like CoD2 starts it with the door men's get-outs. The model blends "doors" 0..1 between its shut and open frames
// (vehicle_rig.py: 0 .. DOOR_OPEN degrees) - DoorPoseFor turns degrees into that blend's weight.
//-----------------------------------------------------------------------------
float CDoDSPVehicle::DoorPoseFor( float flDeg, float flOpen )
{
	// the engine blends the two frames' rotations by a normalised lerp of their quaternions (bone_setup.cpp
	// BlendBones -> QuaternionBlend), not by angle: a weight w turns the door by 2 atan2(w sin(h), 1 - w + w cos(h)),
	// h = half the open angle. This is its inverse, so the door is at the curve's angle (at w 0.25 of 120 degrees
	// the plain ratio would be 4 degrees off)
	flDeg = clamp( flDeg, 0.0f, flOpen );
	float h = DEG2RAD( flOpen ) * 0.5f, t = tanf( DEG2RAD( flDeg ) * 0.5f );
	float flDen = sinf( h ) + t * ( 1.0f - cosf( h ) );
	return flDen > 1e-6f ? clamp( t / flDen, 0.0f, 1.0f ) : 0.0f;
}

void CDoDSPVehicle::DoorsThink( void )
{
	const VehicleType_t::Doors_t &d = m_pType->doors;
	if ( !d.nKeys || m_flDoorsStart <= 0.0f || m_eState == VSTATE_WRECKED )
		return;
	float t = gpGlobals->curtime - m_flDoorsStart;
	if ( t > d.flTime[d.nKeys - 1] + 1.0f )
		return;			// settled on the curve's last key (the pose parameter is saved with the entity)
	if ( m_iDoorPose == -2 )
	{
		// one owner of the doors' pose and range: the model (vehicle_rig.py writes "dodsp_doors" with DOOR_OPEN)
		m_iDoorPose = -1;
		const char *pszText = modelinfo->GetModelKeyValueText( GetModel() );
		KeyValues *pAll = new KeyValues( "" );
		KeyValues *pD = ( pszText && pszText[0] && pAll->LoadFromBuffer( STRING( GetModelName() ), pszText ) ) ? pAll->FindKey( "dodsp_doors" ) : NULL;
		if ( pD )
		{
			m_flDoorOpen = pD->GetFloat( "open" );
			m_iDoorPose = m_flDoorOpen > 0.0f ? LookupPoseParameter( pD->GetString( "pose", "" ) ) : -1;
		}
		if ( m_iDoorPose < 0 )
			Warning( "INVARIANT DOD-SP vehicle %s: %s %s - its doors stay shut and the riders walk through them (recompile the model "
					 "from vehicle_rig.py's output: dodsp_halftrack_h3_doors.zip)\n", Label(), STRING( GetModelName() ),
					 pD ? "names a door pose parameter it hasn't got" : "has no \"dodsp_doors\" facts (an older rig)" );
		pAll->deleteThis();
	}
	if ( m_iDoorPose < 0 )
		return;
	float flDeg = d.flDeg[d.nKeys - 1];
	for ( int k = 0; k + 1 < d.nKeys; k++ )
	{
		if ( t < d.flTime[0] )
		{
			flDeg = d.flDeg[0];
			break;
		}
		if ( t <= d.flTime[k + 1] )
		{
			flDeg = Lerp( ( t - d.flTime[k] ) / ( d.flTime[k + 1] - d.flTime[k] ), d.flDeg[k], d.flDeg[k + 1] );
			break;
		}
	}
	if ( flDeg > m_flDoorOpen + 0.5f )
	{
		Warning( "INVARIANT DOD-SP vehicle %s: its doors' curve wants %.0f degrees, the model opens them %.0f at most - they stop short\n",
				 Label(), flDeg, m_flDoorOpen );
		m_flDoorOpen = flDeg;		// say it once (the pose clamps)
	}
	SetPoseParameter( m_iDoorPose, DoorPoseFor( flDeg, m_flDoorOpen ) );
}

// THE owner of when riders get off: put them aboard, decide, stop, then send each off at his seat's delay.
void CDoDSPVehicle::RidersThink( void )
{
	float now = gpGlobals->curtime;
	if ( m_bRiders && !m_bRidersSpawned && m_flRidersSpawnAt > 0.0f && now >= m_flRidersSpawnAt )
		SpawnRiders();
	if ( m_eRiders == VRIDERS_NONE || m_eRiders == VRIDERS_OFF )
		return;
	int nAboard = RidersAboard();
	if ( m_eRiders == VRIDERS_ABOARD )
	{
		if ( !nAboard )
		{
			m_eRiders = VRIDERS_OFF;		// all shot off it
			return;
		}
		if ( m_eState != VSTATE_FIGHTING )
			OrderDismount( "it's out of the fight" );
		else if ( m_bDismountOrder )
		{
			m_bDismountOrder = false;
			OrderDismount( "ordered (Dismount input)" );
		}
		else if ( m_eDismount == VDISMOUNT_CONTACT && m_hQuarry.Get() &&
				  ( m_hQuarry->GetAbsOrigin() - GetAbsOrigin() ).Length() < VEHICLE_RIDERS_CONTACT )
			OrderDismount( CFmtStr( "an enemy (%s) is %.0f units away", m_hQuarry->GetClassname(), ( m_hQuarry->GetAbsOrigin() - GetAbsOrigin() ).Length() ) );
	}
	if ( m_eRiders == VRIDERS_STOPPING )
	{
		// it stops first (DriveThink holds it while RidersHold); out of the fight it's already halted
		if ( fabs( m_flSpeed ) > 4.0f && m_eState == VSTATE_FIGHTING )
			return;
		for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
			m_flRiderGo[i] = m_hRider[i].Get() ? now + ( i < m_pType->seats.Count() ? m_pType->seats[i].flDelay : 0.0f ) : 0.0f;
		m_flRidersOffAt = now;
		m_eRiders = VRIDERS_GETTING_OFF;
		if ( m_pType->doors.nKeys && m_flDoorsStart <= 0.0f )
			m_flDoorsStart = now;		// H3: the doors kicked open on CoD2's curve, in step with the seats' delays
	}
	if ( m_eRiders == VRIDERS_GETTING_OFF )
	{
		bool bWaiting = false;
		for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
		{
			CBaseEntity *pRider = m_hRider[i].Get();
			if ( !pRider )
				continue;
			int eState = GermanSoldier_RideState( pRider );
			if ( eState == 1 && m_flRiderGo[i] > 0.0f && now >= m_flRiderGo[i] )
			{
				// waits at most VEHICLE_RIDER_WAIT_MAX past HIS OWN go time (H3: the halftrack's last man goes 3.15 s in)
				float flDelay = i < m_pType->seats.Count() ? m_pType->seats[i].flDelay : 0.0f;
				bool bForce = m_eState != VSTATE_FIGHTING || now - m_flRidersOffAt > flDelay + VEHICLE_RIDER_WAIT_MAX;
				int r = GermanSoldier_RideGetOut( pRider, bForce );
				if ( r < 0 )
				{
					m_flRiderGo[i] = now + VEHICLE_RIDER_WAIT;		// someone's where he'd land: in a moment
					if ( dodsp_vehicle_debug.GetBool() )
						Msg( "DOD-SP vehicle %s: rider on seat %d waits - someone is where he'd land\n", Label(), i + 1 );
				}
				else
				{
					m_flRiderGo[i] = 0.0f;
					if ( r == 0 )
						Warning( "INVARIANT DOD-SP vehicle %s: its rider on seat %d couldn't get off - he was put down where he sat\n", Label(), i + 1 );
				}
			}
			if ( GermanSoldier_RideState( pRider ) != 0 )
				bWaiting = true;
		}
		if ( !bWaiting )
		{
			m_eRiders = VRIDERS_OFF;
			m_OnRidersOff.FireOutput( this, this );
			Msg( "DOD-SP vehicle %s: riders off\n", Label() );
		}
	}
}

//-----------------------------------------------------------------------------
// H2: crew you can see in their seats. A seated man IS his crew role (one owner: m_iCrewAlive), so the two never
// disagree: shot in his seat -> CrewmanShot kills the role; the role killed inside (spall, the wreck) -> he dies;
// the vehicle abandoned -> the role's crewman climbs out of its hatch (SpawnCrewman) and LeaveCrewSeat removes him first.
//-----------------------------------------------------------------------------
int CDoDSPVehicle::CrewIndexOf( int eRole ) const
{
	for ( int i = 0; i < m_pType->armour.nCrew; i++ )
		if ( m_pType->armour.eCrew[i] == eRole )
			return i;
	return -1;
}

void CDoDSPVehicle::SpawnCrewSeats( void )
{
	m_bCrewSeatsSpawned = true;
	if ( m_eFaction != DoDSP_FactionFromName( "axis" ) || m_eState != VSTATE_FIGHTING )
		return;		// German soldiers only so far (like the riders); a vehicle already out of the fight has no one to show
	char szSquad[32];
	Q_snprintf( szSquad, sizeof( szSquad ), "tankcrew%d", entindex() );
	for ( int i = 0; i < m_pType->crewSeats.Count() && i < VEHICLE_MAX_CREW_SEATS; i++ )
	{
		const VehicleType_t::CrewSeat_t &cs = m_pType->crewSeats[i];
		int iCrew = CrewIndexOf( cs.eRole );
		if ( iCrew < 0 || !( m_iCrewAlive & ( 1 << iCrew ) ) )
			continue;
		CBaseEntity *pNPC = CreateEntityByName( "npc_german_soldier" );
		if ( !pNPC )
			break;
		Vector vecWorld;
		VectorTransform( cs.vecOrigin, EntityToWorldTransform(), vecWorld );
		pNPC->KeyValue( "additionalequipment", "weapon_mp40" );
		pNPC->KeyValue( "squadname", szSquad );
		pNPC->AddSpawnFlags( SF_NPC_FALL_TO_GROUND );
		pNPC->SetAbsOrigin( vecWorld );
		pNPC->SetAbsAngles( QAngle( 0.0f, GetAbsAngles().y + cs.flYaw, 0.0f ) );
		DispatchSpawn( pNPC );
		pNPC->Activate();
		if ( !GermanSoldier_BeginRide( pNPC, this, cs.vecOrigin, cs.flYaw, cs.szIdle, "", "" ) )
		{
			UTIL_Remove( pNPC );		// the soldier said why (once per map): the ride pack's driver sequence is missing
			continue;
		}
		GermanSoldier_SetSeatDeath( pNPC, cs.szDeath );
		m_hCrewSeat[i] = pNPC;
		Msg( "DOD-SP vehicle %s: its %s is in his seat%s\n", Label(), g_pszCrewRole[cs.eRole], cs.bSteer ? " (steering)" : "" );
	}
}

void CDoDSPVehicle::LeaveCrewSeat( int eRole )
{
	for ( int i = 0; i < m_pType->crewSeats.Count() && i < VEHICLE_MAX_CREW_SEATS; i++ )
		if ( m_pType->crewSeats[i].eRole == eRole && m_hCrewSeat[i].Get() )
		{
			UTIL_Remove( m_hCrewSeat[i].Get() );
			m_hCrewSeat[i] = NULL;
		}
}

void CDoDSPVehicle::CrewmanShot( int iCrew, const char *pszWhy )
{
	if ( iCrew < 0 || !( m_iCrewAlive & ( 1 << iCrew ) ) )
		return;
	m_iCrewAlive &= ~( 1 << iCrew );
	Msg( "DOD-SP vehicle %s: its %s is killed (%s) - crew %d of %d left\n", Label(), g_pszCrewRole[m_pType->armour.eCrew[iCrew]], pszWhy,
		 CrewAlive(), m_pType->armour.nCrew );
	if ( !CrewHas( CREW_DRIVER ) && !CrewHas( CREW_RADIO ) )
		SetImmobile( "no one left to drive it", NULL );
	if ( m_eState == VSTATE_FIGHTING )
		CheckMorale( NULL );
}

void CDoDSPVehicle::CrewSeatsThink( void )
{
	if ( !m_pType->crewSeats.Count() )
		return;
	if ( !m_bCrewSeatsSpawned && m_flCrewSeatsAt > 0.0f && gpGlobals->curtime >= m_flCrewSeatsAt )
		SpawnCrewSeats();
	float flLock = ( m_flWheelbase > 0.0f ) ? RAD2DEG( atanf( m_flWheelbase / MAX( m_pType->flMinTurnRadius, 50.0f ) ) ) : 0.0f;
	for ( int i = 0; i < m_pType->crewSeats.Count() && i < VEHICLE_MAX_CREW_SEATS; i++ )
	{
		CBaseEntity *pMan = m_hCrewSeat[i].Get();
		if ( !pMan )
			continue;
		const VehicleType_t::CrewSeat_t &cs = m_pType->crewSeats[i];
		int iCrew = CrewIndexOf( cs.eRole );
		if ( GermanSoldier_RideState( pMan ) == 0 )
		{
			m_hCrewSeat[i] = NULL;			// dead (or off his seat some other way): the role goes with him
			m_hCrewCorpse[i] = pMan;		// H2b: slumped in the seat (gone with the vehicle)
			CrewmanShot( iCrew, "shot in his seat" );
			continue;
		}
		if ( iCrew < 0 || ( !( m_iCrewAlive & ( 1 << iCrew ) ) && !( m_iBailMask & ( 1 << iCrew ) ) ) )
		{
			m_hCrewSeat[i] = NULL;			// his role was killed inside the vehicle - so is he, where he sits
			m_hCrewCorpse[i] = pMan;
			CTakeDamageInfo info( this, this, 10000.0f, DMG_BLAST );
			pMan->TakeDamage( info );
			continue;
		}
		if ( cs.bSteer && flLock > 0.0f )
		{
			CBaseAnimating *pAnim = pMan->GetBaseAnimating();
			if ( m_iSteerPose == -2 )
			{
				m_iSteerPose = pAnim ? pAnim->LookupPoseParameter( "steer" ) : -1;
				if ( m_iSteerPose < 0 )
					Warning( "INVARIANT DOD-SP vehicle %s: its %s's model has no \"steer\" pose parameter - he won't turn the wheel "
							 "(recompile npc_ww2_anims_ride from the H2 pack)\n", Label(), g_pszCrewRole[cs.eRole] );
			}
			if ( pAnim && m_iSteerPose >= 0 )
				pAnim->SetPoseParameter( m_iSteerPose, clamp( m_flSteer / flLock, -1.0f, 1.0f ) );	// + = left, like m_flSteer
		}
	}
}

void CDoDSPVehicle::SetBurning( bool bAmmo, CBaseEntity *pAttacker )
{
	if ( m_eState == VSTATE_BURNING || m_eState == VSTATE_WRECKED )
		return;
	float now = gpGlobals->curtime;
	SetState( VSTATE_BURNING, pAttacker );
	m_flIgniteAt = 0.0f;
	// the moment it's knocked out shows: a burst at the hatches, then the fire
	ExplosionCreate( GetAbsOrigin() + Vector( 0.0f, 0.0f, m_flBodyHeight ), vec3_angle, this, 60, 0,
					 SF_ENVEXPLOSION_NODAMAGE | SF_ENVEXPLOSION_NODECAL, 0.0f, this );
	MakeFire( bAmmo ? 160.0f : 112.0f, !bAmmo );
	m_flBailAt = CrewAlive() ? now + random->RandomFloat( 0.8f, 2.0f ) : 0.0f;
	if ( bAmmo )
	{
		m_flCookOffAt = now + random->RandomFloat( 5.0f, 10.0f );
		m_flBurnOutAt = 0.0f;
	}
	else
	{
		m_flBurnOutAt = now + random->RandomFloat( 12.0f, 20.0f );
		m_flCookOffAt = ( random->RandomFloat() < 0.4f ) ? now + random->RandomFloat( 8.0f, 16.0f ) : 0.0f;	// the fire reaches the ammunition
	}
	Msg( "DOD-SP vehicle %s: BURNING - %s fire%s\n", Label(), bAmmo ? "ammunition" : "engine",
		 m_flCookOffAt > 0.0f ? ", it will cook off" : ", it will burn out" );
}

// The crew decides it's had enough (reason codes): fire, too few men, can't move AND can't fight, can't move and
// keeps getting penetrated.
void CDoDSPVehicle::CheckMorale( CBaseEntity *pAttacker )
{
	if ( m_eState != VSTATE_FIGHTING || !m_pType->armour.bValid )
		return;
	if ( CrewAlive() == 0 )
	{
		Msg( "DOD-SP vehicle %s: the whole crew is dead - out of the fight\n", Label() );
		SetState( VSTATE_ABANDONED, pAttacker );
		return;
	}
	bool bCanFight = MainGunUsable() || AnyMGUsable();
	bool bTwoPens = gpGlobals->curtime - m_flPenTimes[1] < VEHICLE_PEN_MEMORY;
	bool bJustPenetrated = gpGlobals->curtime - m_flPenTimes[0] < 0.5f;
	// V4d (first V4c runs, 2026-11-20): a Panzer II hit by a Sherman was immobilised, lost its driver and was left at
	// 7 of 120 - and the crew sat in it until the next round blew it up (no bail-out ever seen). Real crews got out of
	// a tank that was badly holed or couldn't move under fire.
	int eWhy = VBAIL_NONE;
	if ( CrewAlive() <= 1 && m_pType->armour.nCrew > 2 )
		eWhy = VBAIL_CREW;
	else if ( m_bImmobile && !bCanFight )
		eWhy = VBAIL_HELPLESS;
	else if ( m_bImmobile && bJustPenetrated )
		eWhy = VBAIL_IMMOBILE_HIT;
	else if ( m_flStructure <= VEHICLE_BAIL_STRUCTURE * m_pType->armour.flStructure )
		eWhy = VBAIL_WRECKED_INSIDE;
	else if ( m_bImmobile && bTwoPens )
		eWhy = VBAIL_UNDER_FIRE;
	if ( eWhy != VBAIL_NONE )
		BailOut( eWhy, pAttacker );
}

// Everyone alive gets out (a burning vehicle costs some of them), ONE AT A TIME (DamageStateThink lets one out every
// 0.6-1 s and sends him running from the vehicle). German crews come out as soldiers with MP40s on the side away
// from whoever hit them; DOD-SP has no Allied infantry NPC yet - an Allied crew leaves the fight.
void CDoDSPVehicle::BailOut( int eWhy, CBaseEntity *pAttacker )
{
	const DoDSPArmour_t &ar = m_pType->armour;
	int nOut = 0, nLost = 0;
	bool bAxis = ( m_eFaction == DODSP_FACTION_AXIS );
	m_vecBailFrom = pAttacker ? pAttacker->GetAbsOrigin() : GetAbsOrigin() + Vector( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f ) * 512.0f;
	for ( int i = 0; i < ar.nCrew; i++ )
	{
		if ( !( m_iCrewAlive & ( 1 << i ) ) )
			continue;
		m_iCrewAlive &= ~( 1 << i );
		if ( m_eState == VSTATE_BURNING && random->RandomFloat() > 0.75f )
			nLost++;
		else if ( bAxis )
		{
			m_nBailPending++;
			m_iBailMask |= ( 1 << i );
		}
		else
			nOut++;
	}
	if ( m_nBailPending + nOut + nLost > 0 )
	{
		Msg( "DOD-SP vehicle %s: CREW BAILS OUT (%s) - %d %s%s\n", Label(), s_pszBail[eWhy], bAxis ? m_nBailPending : nOut,
			 bAxis ? "coming out" : "got out (DOD-SP has no Allied infantry NPC yet: they leave the fight)", nLost ? ", some didn't make it" : "" );
		m_OnBailOut.FireOutput( pAttacker ? pAttacker : this, this );
	}
	m_flNextBailAt = gpGlobals->curtime + 0.3f;
	if ( m_eState == VSTATE_FIGHTING )
		SetState( VSTATE_ABANDONED, pAttacker );
}

// V4e: how long a hatch is in use by one crewman: c1_tank_climb_out (41 frames, 1.37 s) + c1_tank_jump_off until he
// moves off the hatch (frame 10, 0.33 s) + a little. The next man through the same hatch starts after that.
#define VEHICLE_HATCH_BUSY	1.9f

// Where a crewman gets out: his hatch from the type's "hatches" (V4e: measured from the meshes - V4c guessed the turret
// roof from its HULL BOX, whose top is the raised barrel's tip: the Tiger's crew climbed out 67 units over the roof).
// The point is the roof his feet end up on; the climb starts with his legs below it, inside.
bool CDoDSPVehicle::HatchFor( int eRole, Vector *pvecHatch )
{
	if ( eRole < 0 || eRole >= NUM_CREW_ROLES || !m_pType->hatches[eRole].bHas )
		return false;
	const VehicleType_t::Hatch_t &h = m_pType->hatches[eRole];
	CBaseEntity *pFrame = h.bTurret ? m_hTurret.Get() : this;
	if ( !pFrame )
		return false;
	VectorTransform( h.vecPos, pFrame->EntityToWorldTransform(), *pvecHatch );
	return true;
}

bool CDoDSPVehicle::SpawnCrewman( const Vector &vecAwayFrom, int eRole )
{
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	Vector vecRight( vecFwd.y, -vecFwd.x, 0.0f );
	Vector vecAway = GetAbsOrigin() - vecAwayFrom;
	vecAway.z = 0.0f;
	VectorNormalize( vecAway );
	Vector vecSide = ( DotProduct( vecAway, vecRight ) >= 0.0f ) ? vecRight : -vecRight;
	Vector vecEnd = ( DotProduct( vecAway, vecFwd ) >= 0.0f ) ? vecFwd : -vecFwd;
	Vector vecDirs[4] = { vecSide, vecEnd, -vecSide, -vecEnd };
	float flOut[4] = { m_flHalfWidth, m_flHalfLength, m_flHalfWidth, m_flHalfLength };
	CTraceFilterSimple filter( this, COLLISION_GROUP_NPC );
	Vector vecHatch;
	bool bHatch = HatchFor( eRole, &vecHatch );
	if ( !bHatch )
		Msg( "DOD-SP vehicle %s: its %s has no hatch in %s (script version %s, this server.dll wants %s) - he gets out beside it\n",
			 Label(), ( eRole >= 0 && eRole < NUM_CREW_ROLES ) ? g_pszCrewRole[eRole] : "?", DODSP_VEHICLE_SCRIPT,
			 s_szVehicleScriptVersion, DODSP_VEHICLE_SCRIPT_VERSION );
	for ( int d = 0; d < 4; d++ )
	{
		// V4e: out of his hatch he jumps straight off the side, landing just clear of the hull (a short leap, not a
		// spot anywhere along the tank); the ends and a crewman without a hatch get out beside it as before
		bool bJump = bHatch && ( d == 0 || d == 2 );
		for ( int k = 0; k < 3; k++ )
		{
			Vector p;
			if ( bJump )
			{
				float flOff = DotProduct( vecHatch - GetAbsOrigin(), vecDirs[d] );
				p = vecHatch + vecDirs[d] * ( m_flHalfWidth - flOff + 24.0f + 24.0f * k );
				p.z = GetAbsOrigin().z;
			}
			else
				p = GetAbsOrigin() + vecDirs[d] * ( flOut[d] + 32.0f + 24.0f * k ) +
					( ( d & 1 ) ? vecSide : vecFwd ) * random->RandomFloat( -0.5f, 0.5f ) * ( ( d & 1 ) ? m_flHalfWidth : m_flHalfLength );
			float flZ;
			if ( !GroundAt( p, 64.0f, 256.0f, &flZ, NULL, this ) )
				continue;
			p.z = flZ + 4.0f;
			trace_t tr;
			UTIL_TraceHull( p, p, NAI_Hull::Mins( HULL_HUMAN ), NAI_Hull::Maxs( HULL_HUMAN ), MASK_NPCSOLID, &filter, &tr );
			if ( tr.startsolid || tr.allsolid )
				continue;
			CBaseEntity *pNPC = CreateEntityByName( "npc_german_soldier" );
			if ( !pNPC )
				return false;
			char szSquad[32];
			Q_snprintf( szSquad, sizeof( szSquad ), "tankcrew%d", entindex() );
			pNPC->KeyValue( "additionalequipment", "weapon_mp40" );
			pNPC->KeyValue( "squadname", szSquad );
			// V4g: no snap to the floor at his first think - a bailing man is put in the hatch, inside the hull, and the
			// snap printed "NPC npc_german_soldier stuck in wall--level design error" (he's on the ground here anyway)
			pNPC->AddSpawnFlags( SF_NPC_FALL_TO_GROUND );
			pNPC->SetAbsOrigin( p );
			pNPC->SetAbsAngles( QAngle( 0.0f, RAD2DEG( atan2f( vecAway.y, vecAway.x ) ), 0.0f ) );
			DispatchSpawn( pNPC );
			pNPC->Activate();
			Vector vecRun = p + ( vecDirs[d] * 0.5f + vecAway ) * 256.0f;
			// V4b: out of his hatch and off the side (CoD1 tank_climb_out / tank_jump_off, the soldier's own
			// schedule) when the jump is to the side; else (or a model without the animations) he's already on the
			// ground here and just runs (ordered on the next think, once his AI has started)
			if ( bJump && vecHatch.z - p.z > 24.0f && GermanSoldier_BailOut( pNPC, vecHatch, p, vecRun, RAD2DEG( atan2f( vecDirs[d].y, vecDirs[d].x ) ) ) )
			{
				m_flHatchFree[m_pType->hatches[eRole].iShare] = gpGlobals->curtime + VEHICLE_HATCH_BUSY;
				if ( dodsp_vehicle_debug.GetBool() )
					Msg( "DOD-SP vehicle %s: its %s climbs out of the %s hatch, %.0f units over the ground, and jumps %.0f units out\n",
						 Label(), g_pszCrewRole[eRole], m_pType->hatches[eRole].bTurret ? "turret" : "hull", vecHatch.z - p.z,
						 ( vecHatch - p ).Length2D() );
				return true;
			}
			m_hLastCrewman = pNPC;
			m_vecLastCrewRun = vecRun;
			m_flCrewRunAt = gpGlobals->curtime + 0.2f;
			return true;
		}
	}
	return false;
}

void CDoDSPVehicle::KnockOut( bool bCatastrophic, CBaseEntity *pAttacker, const char *pszWhy )
{
	if ( m_eState == VSTATE_WRECKED )
		return;
	Msg( "DOD-SP vehicle %s: KNOCKED OUT - %s%s\n", Label(), pszWhy, bCatastrophic ? " - it blows up" : "" );
	if ( bCatastrophic )
	{
		Wreck( true, pAttacker );
		return;
	}
	if ( m_eState == VSTATE_BURNING )
		return;
	SetBurning( random->RandomFloat() < m_pType->armour.flAmmoFire, pAttacker );
}

// The dead model, the fire, and whoever was still inside. A cook-off goes up like VEHICLE_COOKOFF_KG of explosive.
// V4i (owner 2026-11-20: "a secondary explosion just before the tank switches models, to mask the sudden model change",
// and HL2's metal debris): the burst and the debris go off FIRST, the wreck model goes in VEHICLE_WRECK_SWAP_DELAY later
// (FinishWreck, from DamageStateThink), under the fireball. An ammunition cook-off is VEHICLE_COOKOFF_KG; a burnt-out
// vehicle ends with its fuel / last rounds going up (VEHICLE_BURNOUT_KG). Called again while the swap is pending, it
// does nothing (the cook-off and burn-out timers can both fire).
#define VEHICLE_WRECK_SWAP_DELAY	0.15f
#define VEHICLE_BURNOUT_KG			0.4f
#define VEHICLE_DEBRIS_CHUNKS		"MetalChunks"		// HL2's propdata.txt: what metal breakables throw (models/gibs/metal_gib*)
void CDoDSPVehicle::Wreck( bool bBlowUp, CBaseEntity *pAttacker )
{
	if ( m_eState == VSTATE_WRECKED || m_flWreckAt > 0.0f )
		return;
	float now = gpGlobals->curtime;
	if ( CrewAlive() + m_nBailPending )
		Msg( "DOD-SP vehicle %s: %d crew were still inside\n", Label(), CrewAlive() + m_nBailPending );
	m_iCrewAlive = 0;
	m_nBailPending = 0;
	m_iBailMask = 0;
	// H2: whoever you could see in a seat (alive or slumped there) goes with it, under the fireball - the wreck model
	// that replaces it 0.15 s later has no seats
	for ( int i = 0; i < VEHICLE_MAX_CREW_SEATS; i++ )
	{
		if ( m_hCrewSeat[i].Get() )
			UTIL_Remove( m_hCrewSeat[i].Get() );
		if ( m_hCrewCorpse[i].Get() )
			UTIL_Remove( m_hCrewCorpse[i].Get() );
		m_hCrewSeat[i] = m_hCrewCorpse[i] = NULL;
	}
	for ( int i = 0; i < VEHICLE_MAX_SEATS; i++ )
	{
		if ( m_hRiderCorpse[i].Get() )
			UTIL_Remove( m_hRiderCorpse[i].Get() );		// H3: riders slumped in their seats
		m_hRiderCorpse[i] = NULL;
	}
	if ( pAttacker )
		m_hKiller = pAttacker;
	SetState( VSTATE_BURNING, pAttacker );		// out of the fight now (halted, nobody shoots it) - wrecked at the swap
	m_bWreckBlowUp = bBlowUp;
	m_flWreckAt = now + VEHICLE_WRECK_SWAP_DELAY;
	Vector vecBurst = WorldSpaceCenter() + Vector( 0.0f, 0.0f, 16.0f );
	DoDSP_ShellBlast( vecBurst, bBlowUp ? VEHICLE_COOKOFF_KG : VEHICLE_BURNOUT_KG, pAttacker ? pAttacker : this, this, m_pType->szExplode );
	ThrowWreckDebris( bBlowUp ? 14 : 7, bBlowUp ? 420.0f : 260.0f );
	SetNextThink( m_flWreckAt );
}

// HL2's metal debris (client-side pieces that bounce and fade, like a broken metal breakable's)
void CDoDSPVehicle::ThrowWreckDebris( int nPieces, float flUp )
{
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	Vector vecSize = ( vecMaxs - vecMins ) * 0.5f;
	Vector vecSpot = WorldSpaceCenter() + Vector( 0.0f, 0.0f, m_flBodyHeight * 0.3f );
	CPVSFilter filter( vecSpot );
	int nThrown = 0;
	for ( int i = 0; i < nPieces; i++ )
	{
		const char *pszModel = g_PropDataSystem.GetRandomChunkModel( VEHICLE_DEBRIS_CHUNKS );
		int iModel = pszModel ? modelinfo->GetModelIndex( pszModel ) : -1;
		if ( iModel < 0 )
			continue;
		Vector vecVel( random->RandomFloat( -1.0f, 1.0f ), random->RandomFloat( -1.0f, 1.0f ), 0.0f );
		vecVel = vecVel * ( flUp * 0.6f ) + Vector( 0.0f, 0.0f, flUp * random->RandomFloat( 0.7f, 1.2f ) );
		te->BreakModel( filter, 0.0f, vecSpot, GetAbsAngles(), vecSize, vecVel, iModel, 100, 1, random->RandomFloat( 12.0f, 20.0f ),
						BREAK_METAL | ( nThrown ? BREAK_SLAVE : 0 ) );
		nThrown++;
	}
	static char s_szWarnedMap[64] = "";
	if ( nPieces > 0 && !nThrown && Q_strcmp( s_szWarnedMap, STRING( gpGlobals->mapname ) ) )
	{
		Q_strncpy( s_szWarnedMap, STRING( gpGlobals->mapname ), sizeof( s_szWarnedMap ) );
		Warning( "INVARIANT DOD-SP vehicle %s: no \"%s\" debris models in scripts/propdata.txt - wrecks throw no debris\n", Label(), VEHICLE_DEBRIS_CHUNKS );
	}
}

// The wreck model, the fire - under the secondary explosion's fireball (Wreck set it off VEHICLE_WRECK_SWAP_DELAY ago)
void CDoDSPVehicle::FinishWreck( void )
{
	float now = gpGlobals->curtime;
	bool bBlowUp = m_bWreckBlowUp;
	m_flWreckAt = 0.0f;
	SetState( VSTATE_WRECKED, m_hKiller.Get() );
	if ( m_hTurret.Get() )
	{
		UTIL_Remove( m_hTurret.Get() );
		m_hTurret = NULL;
	}
	bool bSwapped = false;
	if ( m_pType->szDeadModel[0] && modelinfo->GetModelIndex( m_pType->szDeadModel ) >= 0 )
	{
		VPhysicsDestroyObject();
		SetModel( m_pType->szDeadModel );
		SetMoveType( MOVETYPE_NONE );
		SetSolid( SOLID_VPHYSICS );
		if ( !VPhysicsInitStatic() )
			SetSolid( SOLID_BBOX );
		MeasureBody();
		bSwapped = true;
	}
	else
		Warning( "INVARIANT DOD-SP vehicle %s: wrecked but its dead_model '%s' isn't loaded - it keeps the live model\n", Label(), m_pType->szDeadModel );
	HaltMotion();
	MakeFire( 128.0f, false );
	m_flBurnOutAt = now + random->RandomFloat( 40.0f, 90.0f );
	Msg( "DOD-SP vehicle %s: WRECKED%s%s\n", Label(), bBlowUp ? " - the ammunition went up" : " - burnt out", bSwapped ? "" : " (no wreck model)" );
	SetThink( &CDoDSPVehicle::WreckThink );
	SetNextThink( now + 1.0f );
}

// INVARIANT: a vehicle out of the fight doesn't move (it's a pusher: whatever velocity it has, it keeps)
void CDoDSPVehicle::CheckHalted( void )
{
	if ( GetAbsVelocity().LengthSqr() > 1.0f || GetLocalAngularVelocity() != vec3_angle )
	{
		if ( !m_bMovingWarned )
		{
			m_bMovingWarned = true;
			Warning( "INVARIANT DOD-SP vehicle %s: %s but still moving (%.0f units/s) - stopped\n", Label(), s_pszVehicleState[m_eState],
					 GetAbsVelocity().Length() );
		}
		HaltMotion();
	}
}

void CDoDSPVehicle::WreckThink( void )
{
	CheckHalted();
	if ( m_flBurnOutAt > 0.0f && gpGlobals->curtime >= m_flBurnOutAt )
	{
		m_flBurnOutAt = 0.0f;
		if ( m_hFire.Get() )
			UTIL_Remove( m_hFire.Get() );
		m_hFire = NULL;
	}
	if ( m_flBurnOutAt > 0.0f )
		SetNextThink( gpGlobals->curtime + 1.0f );
}

// burning / abandoned: no engine, no guns; the crew coming out one by one; the fire's clock
void CDoDSPVehicle::DamageStateThink( void )
{
	float now = gpGlobals->curtime;
	CheckHalted();
	if ( m_flWreckAt > 0.0f && now >= m_flWreckAt )
	{
		FinishWreck();		// V4i: the wreck model goes in under the secondary explosion
		return;
	}
	UpdateDamageSmoke();
	if ( m_hLastCrewman.Get() && m_flCrewRunAt > 0.0f && now >= m_flCrewRunAt )
	{
		m_flCrewRunAt = 0.0f;
		CAI_BaseNPC *pAI = m_hLastCrewman->MyNPCPointer();
		if ( pAI && pAI->IsAlive() )
		{
			pAI->m_vecLastPosition = m_vecLastCrewRun;
			pAI->SetSchedule( SCHED_FORCED_GO_RUN );
		}
	}
	if ( m_nBailPending > 0 && now >= m_flNextBailAt )
	{
		// V4e: the first seat whose hatch is free (a man waits while the one before him is still in a shared hatch;
		// the loader's own hatch doesn't wait for the commander's)
		int iSeat = -1;
		float flSoonest = FLT_MAX;
		for ( int i = 0; i < DODSP_MAX_CREW && iSeat < 0; i++ )
		{
			if ( !( m_iBailMask & ( 1 << i ) ) )
				continue;
			const VehicleType_t::Hatch_t &hatch = m_pType->hatches[m_pType->armour.eCrew[i]];
			if ( hatch.bHas && m_flHatchFree[hatch.iShare] > now )
				flSoonest = MIN( flSoonest, m_flHatchFree[hatch.iShare] );
			else
				iSeat = i;
		}
		if ( iSeat < 0 && flSoonest == FLT_MAX )
		{
			Warning( "INVARIANT DOD-SP vehicle %s: %d crew to get out but no seat marked - dropped\n", Label(), m_nBailPending );
			m_nBailPending = 0;
		}
		else if ( iSeat < 0 )
			m_flNextBailAt = flSoonest;
		else
		{
			m_nBailPending--;
			m_iBailMask &= ~( 1 << iSeat );
			LeaveCrewSeat( m_pType->armour.eCrew[iSeat] );		// H2: the man you saw in the seat is the one climbing out
			if ( !SpawnCrewman( m_vecBailFrom, m_pType->armour.eCrew[iSeat] ) )
				Msg( "DOD-SP vehicle %s: no room beside it for a crewman to get out\n", Label() );
			m_flNextBailAt = now + random->RandomFloat( 0.6f, 1.0f );
		}
	}
	if ( m_eState == VSTATE_BURNING )
	{
		if ( m_flBailAt > 0.0f && now >= m_flBailAt )
		{
			m_flBailAt = 0.0f;
			BailOut( VBAIL_FIRE, m_hKiller.Get() );
		}
		if ( m_flCookOffAt > 0.0f && now >= m_flCookOffAt )
		{
			m_flCookOffAt = 0.0f;
			Wreck( true, m_hKiller.Get() );
			return;
		}
		if ( m_flBurnOutAt > 0.0f && now >= m_flBurnOutAt )
		{
			m_flBurnOutAt = 0.0f;
			Wreck( false, m_hKiller.Get() );
			return;
		}
	}
	SetNextThink( now + 0.1f );
}

// Bullets: judged by the armour where they strike. Nothing goes into the engine's health (the base class would).
void CDoDSPVehicle::TraceAttack( const CTakeDamageInfo &info, const Vector &vecDir, trace_t *ptr, CDmgAccumulator *pAccumulator )
{
	if ( !m_pType || !m_pType->armour.bValid || m_eState == VSTATE_WRECKED || !( info.GetDamageType() & DMG_BULLET ) || info.GetAttacker() == this )
		return;
	Vector vecPath = vecDir;
	VectorNormalize( vecPath );
	VehicleHit_t hit;
	if ( !ShellHitBox( ptr->endpos - vecPath * 48.0f, vecPath, 96.0f, &hit ) )
		return;
	float flPen = DoDSP_BulletPen( info.GetAmmoType(), info.GetDamage(), ( ptr->endpos - ptr->startpos ).Length() );
	TakeArmourHit( hit, vecPath, flPen, 2.0f, 8.0f, info.GetAttacker(), "a bullet", true, 0.0f );
}

// Blasts: our shells say their real charge (g_DoDSPBlast); anything else (grenades, env_explosion) is guessed from
// its damage (a 125-damage grenade ~ 0.1 kg).
int CDoDSPVehicle::OnTakeDamage( const CTakeDamageInfo &info )
{
	if ( !m_pType || !m_pType->armour.bValid || m_eState == VSTATE_WRECKED )
		return 0;
	if ( info.GetDamageType() & DMG_BLAST )
	{
		bool bOurs = g_DoDSPBlast.bActive;
		BlastHit( bOurs ? g_DoDSPBlast.vecCentre : info.GetDamagePosition(), bOurs ? g_DoDSPBlast.flFiller : info.GetDamage() * 0.0008f,
				  info.GetAttacker() );
		UpdateDamageSmoke();
	}
	return 0;
}

// Can its gun get through the plate facing it from here? (the target choice prefers what it can hurt)
bool CDoDSPVehicle::CanHurt( CDoDSPVehicle *pOther )
{
	const DoDSPShell_t *pShell = ShellFor( pOther );
	if ( !pShell )
		return false;
	if ( !pOther->m_pType || !pOther->m_pType->armour.bValid )
		return true;
	Vector vecFrom = AimPivot();
	Vector vecDir = pOther->WorldSpaceCenter() - vecFrom;
	float flDist = VectorNormalize( vecDir );
	VehicleHit_t h;
	if ( !pOther->ShellHitBox( vecFrom, vecDir, flDist + 256.0f, &h ) )
		return true;
	ArmourPlate_t plate = pOther->m_pType->armour.plates[h.ePart][h.eFace];
	float flEff = DoDSP_EffectiveArmour( plate, vecDir, h.vecNormal, NULL );
	return DoDSP_ShellPen( *pShell, h.t ) >= flEff * 0.9f;
}

void CDoDSPVehicle::DamageReport( char *psz, int n ) const
{
	if ( !m_pType->armour.bValid )
	{
		Q_strncpy( psz, "armour: none (the type's \"armour\" block didn't load) - it can't be damaged", n );
		return;
	}
	char szCrew[96] = "";
	for ( int i = 0; i < m_pType->armour.nCrew; i++ )
	{
		if ( !( m_iCrewAlive & ( 1 << i ) ) )
			continue;
		if ( szCrew[0] )
			Q_strncat( szCrew, " ", sizeof( szCrew ), COPY_ALL_CHARACTERS );
		Q_strncat( szCrew, g_pszCrewRole[m_pType->armour.eCrew[i]], sizeof( szCrew ), COPY_ALL_CHARACTERS );
	}
	char szAmmo[48] = "";
	if ( m_pType->gun.bHas && m_pType->gun.nRounds > 0 )
		Q_snprintf( szAmmo, sizeof( szAmmo ), ", %d rounds left", m_iRounds );
	Q_snprintf( psz, n, "%s: structure %.0f of %.0f, crew %d of %d (%s)%s%s%s%s", s_pszVehicleState[m_eState], m_flStructure,
				m_pType->armour.flStructure, CrewAlive(), m_pType->armour.nCrew, szCrew[0] ? szCrew : "none", m_bImmobile ? ", IMMOBILISED" : "",
				m_bTurretJammed ? ", turret jammed" : "", m_bGunOut ? ", main gun out" : "", szAmmo );
}

// Every gun against every vehicle (real numbers): how far out it gets through each plate, head-on.
CON_COMMAND( dodsp_vehicle_penchart, "V4: every vehicle gun against every vehicle's armour - how far out (metres) it gets through each plate head-on." )
{
	LoadVehicleTypes( false );
	for ( int g = 0; g < s_Types.Count(); g++ )
	{
		VehicleType_t *pG = s_Types[g];
		if ( pG->bVariant || !pG->gun.bHas || !pG->gun.ap.bValid )
			continue;
		const DoDSPShell_t &ap = pG->gun.ap;
		Msg( "%s %.0f mm AP: %.0f mm at 100 m, %.0f at 500 m, %.0f at 1000 m, %.0f at 2000 m (1 m = %.0f units)\n", pG->szName, ap.flCalibre,
			 DoDSP_ShellPen( ap, 100.0f * DODSP_UNITS_PER_METRE ), DoDSP_ShellPen( ap, 500.0f * DODSP_UNITS_PER_METRE ),
			 DoDSP_ShellPen( ap, 1000.0f * DODSP_UNITS_PER_METRE ), DoDSP_ShellPen( ap, 2000.0f * DODSP_UNITS_PER_METRE ), DODSP_UNITS_PER_METRE );
		for ( int a = 0; a < s_Types.Count(); a++ )
		{
			VehicleType_t *pA = s_Types[a];
			if ( pA->bVariant || !pA->armour.bValid )
				continue;
			char szLine[256] = "";
			static const int s_iFaces[][2] = { { APART_HULL, AFACE_FRONT }, { APART_HULL, AFACE_SIDE }, { APART_HULL, AFACE_REAR },
											   { APART_TURRET, AFACE_FRONT }, { APART_TURRET, AFACE_SIDE } };
			for ( int f = 0; f < ARRAYSIZE( s_iFaces ); f++ )
			{
				if ( s_iFaces[f][0] == APART_TURRET && !pA->szTurretModel[0] )
					continue;
				const ArmourPlate_t &plate = pA->armour.plates[s_iFaces[f][0]][s_iFaces[f][1]];
				float flEff = plate.flMM / MAX( cosf( DEG2RAD( plate.flSlope ) ), 0.17f );
				int nBest = -1;
				for ( int m = 0; m <= 3000; m += 50 )
					if ( DoDSP_ShellPen( ap, m * DODSP_UNITS_PER_METRE ) >= flEff )
						nBest = m;
				char szRange[24], szOne[64];
				if ( nBest < 0 )
					Q_strncpy( szRange, "never", sizeof( szRange ) );
				else if ( nBest >= 3000 )
					Q_strncpy( szRange, "any range", sizeof( szRange ) );
				else
					Q_snprintf( szRange, sizeof( szRange ), "<= %d m", nBest );
				Q_snprintf( szOne, sizeof( szOne ), "%s%s %s %s", szLine[0] ? ", " : "", g_pszArmourPart[s_iFaces[f][0]], g_pszArmourFace[s_iFaces[f][1]], szRange );
				Q_strncat( szLine, szOne, sizeof( szLine ), COPY_ALL_CHARACTERS );
			}
			Msg( "    vs %-12s %s\n", pA->szName, szLine );
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
	char szDamage[256];
	DamageReport( szDamage, sizeof( szDamage ) );
	Msg( "    %s\n", szDamage );
	if ( m_bRiders || m_eRiders != VRIDERS_NONE )
		Msg( "    riders: %s, %d aboard of %d seats (get off: %s)\n", s_pszRiders[m_eRiders], RidersAboard(),
			 m_pType ? m_pType->seats.Count() : 0, s_pszDismount[m_eDismount] );
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

//-----------------------------------------------------------------------------
// V4j: one entity class per vehicle type - dodsp_vehicle_<type> for every block in scripts/dodsp_vehicles.txt - so
// the FGD (generated from the same script: dodsp_tools/gen_vehicle_fgd.py) gives each its own Hammer preview model.
// Registered when the DLL loads (before any map or save is read) and again before each map (a type added to the script
// since). CDoDSPVehicle::Precache takes the type from the class name.
//-----------------------------------------------------------------------------
static void RegisterVehicleClasses( void )
{
	KeyValues *pKV = new KeyValues( "DoDSP_Vehicles" );
	if ( !pKV->LoadFromFile( filesystem, DODSP_VEHICLE_SCRIPT, "GAME" ) )
	{
		pKV->deleteThis();
		return;		// LoadVehicleTypes says so at the first vehicle
	}
	static CUtlVector<char *> s_Names;
	for ( KeyValues *pType = pKV->GetFirstTrueSubKey(); pType; pType = pType->GetNextTrueSubKey() )
	{
		char szClass[128];
		Q_snprintf( szClass, sizeof( szClass ), "%s%s", DODSP_VEHICLE_CLASS_PREFIX, pType->GetName() );
		Q_strlower( szClass );
		if ( EntityFactoryDictionary()->FindFactory( szClass ) )
			continue;
		char *pszName = new char[Q_strlen( szClass ) + 1];
		Q_strcpy( pszName, szClass );
		s_Names.AddToTail( pszName );
		new CEntityFactory<CDoDSPVehicle>( pszName );		// installs itself; lives as long as the DLL
	}
	pKV->deleteThis();
}

class CDoDSPVehicleClasses : public CAutoGameSystem
{
public:
	CDoDSPVehicleClasses() : CAutoGameSystem( "CDoDSPVehicleClasses" ) {}
	virtual bool Init( void ) { RegisterVehicleClasses(); return true; }
	virtual void LevelInitPreEntity( void ) { RegisterVehicleClasses(); }
};
static CDoDSPVehicleClasses s_DoDSPVehicleClasses;
