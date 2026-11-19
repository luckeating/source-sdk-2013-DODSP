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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define DODSP_VEHICLE_SCRIPT	"scripts/dodsp_vehicles.txt"
#define VEHICLE_THINK			0.05f
#define VEHICLE_STEP			24.0f		// what it climbs without noticing (kerbs, rubble)
#define VEHICLE_MAX_STEP_PER_32	36.0f		// ground change between two samples 32 units apart (a slope of ~48 degrees)
#define VEHICLE_STUCK_TIME		3.0f
#define VEHICLE_BADLINK_TIME	30.0f

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
	for ( KeyValues *pType = pKV->GetFirstTrueSubKey(); pType; pType = pType->GetNextTrueSubKey() )
	{
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
			continue;
		}
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

// THE test of whether a vehicle of half-width flHalfWidth can drive straight from a to b (both on
// the ground): ground all the way (no drop or wall steeper than ~48 degrees, no slope the tracks can't
// take), and nothing solid in a corridor 2 x half-width wide from step height to 64 units up.
// Used to grade the node graph's links at load and to straighten routes - one owner.
static bool CorridorClear( const Vector &a, const Vector &b, float flHalfWidth, CBaseEntity *pSelf )
{
	Vector vecDir = b - a;
	vecDir.z = 0.0f;
	float flLen = VectorNormalize( vecDir );
	if ( flLen < 1.0f )
		return true;
	Vector vecRight( vecDir.y, -vecDir.x, 0.0f );
	int nSteps = (int)ceilf( flLen / 32.0f );
	CTraceFilterVehicleWorld filter( pSelf );
	float flPrevZ = a.z;
	Vector vecPrev = a;
	for ( int i = 0; i <= nSteps; i++ )
	{
		Vector p = a + vecDir * ( flLen * i / nSteps );
		float flZ, flNZ;
		if ( !GroundAt( p, 64.0f, 128.0f, &flZ, &flNZ, pSelf ) || flNZ < 0.6f )
			return false;
		if ( i > 0 && fabs( flZ - flPrevZ ) > VEHICLE_MAX_STEP_PER_32 )
			return false;
		p.z = flZ;
		if ( i > 0 )
		{
			static const float s_flHeights[2] = { VEHICLE_STEP, 64.0f };
			static const float s_flSides[3] = { -1.0f, 0.0f, 1.0f };
			for ( int h = 0; h < 2; h++ )
			{
				for ( int s = 0; s < 3; s++ )
				{
					Vector vecOff = vecRight * ( flHalfWidth * s_flSides[s] ) + Vector( 0, 0, s_flHeights[h] );
					trace_t tr;
					UTIL_TraceLine( vecPrev + vecOff, p + vecOff, MASK_SOLID, &filter, &tr );
					if ( tr.fraction < 1.0f || tr.startsolid )
						return false;
				}
				// and across, so a post between the three lines is found
				trace_t tr;
				UTIL_TraceLine( p - vecRight * flHalfWidth + Vector( 0, 0, s_flHeights[h] ),
								p + vecRight * flHalfWidth + Vector( 0, 0, s_flHeights[h] ), MASK_SOLID, &filter, &tr );
				if ( tr.fraction < 1.0f || tr.startsolid )
					return false;
			}
		}
		flPrevZ = flZ;
		vecPrev = p;
	}
	return true;
}

//-----------------------------------------------------------------------------
// The node graph, graded per vehicle width (built once per map per width, printed once)
//-----------------------------------------------------------------------------
struct VehicleNav_t
{
	int		iHalfWidth;
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

static VehicleNav_t *GetVehicleNav( float flHalfWidth, const char *pszTypeName )
{
	if ( !g_pBigAINet )
		return NULL;
	int iHalf = (int)( ceilf( flHalfWidth / 8.0f ) * 8.0f );
	const char *pszMap = STRING( gpGlobals->mapname );
	// the graph as it is now: a grading is for one map, node count and link count (the engine builds or
	// rebuilds the links AFTER entities activate - V1 graded the unbuilt graph: "0 of 0 node links")
	int nGraphLinks = 0;
	for ( int i = 0; i < g_pBigAINet->NumNodes(); i++ )
		nGraphLinks += g_pBigAINet->GetNode( i, false )->NumLinks();
	for ( int i = 0; i < s_Navs.Count(); i++ )
	{
		VehicleNav_t *pNav = s_Navs[i];
		if ( pNav->iHalfWidth == iHalf && pNav->nNodes == g_pBigAINet->NumNodes() && pNav->nGraphLinks == nGraphLinks && !Q_strcmp( pNav->szMap, pszMap ) )
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
			if ( !CorridorClear( pNode->GetOrigin(), pOther->GetOrigin(), (float)iHalf, NULL ) )
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
	Msg( "DOD-SP vehicles: %s (%d units wide, rounded up) can drive %d of %d node links (%d of %d nodes) on %s - graded in %.2f s\n",
		 pszTypeName, iHalf * 2, nOK, nLinks, nNodesReach, pNav->nNodes, pszMap, Plat_FloatTime() - flStart );
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
	virtual void	UpdateOnRemove( void );
	virtual void	Blocked( CBaseEntity *pOther );
	virtual int		DrawDebugTextOverlays( void );

	void			DriveThink( void );
	bool			DriveTo( const Vector &vecGoal );
	void			Stop( void );
	void			Report( void );
	void			SetTurretOffset( const Vector &vecOffset );

	void			InputDriveTo( inputdata_t &inputdata );
	void			InputStop( inputdata_t &inputdata );
	void			InputSetMaxSpeed( inputdata_t &inputdata );

	static CUtlVector<CDoDSPVehicle *> s_All;

private:
	bool			PlanRoute( void );
	int				NearestNode( const Vector &vecPos, bool bFromHere );
	void			SetStuck( VehicleStuck_t eWhy, CBaseEntity *pWho );
	QAngle			BuildAngles( float flHeading, const Vector &vecPos, float *pflGroundZ );
	float			HeadingFromAngles( void ) const;

	string_t		m_iszVehicleType;
	string_t		m_iszInitialGoal;
	VehicleType_t	*m_pType;
	float			m_flHalfWidth, m_flHalfLength;
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
	m_iLastFrom = m_iLastTo = -1;
	s_All.AddToTail( this );
}

void CDoDSPVehicle::UpdateOnRemove( void )
{
	s_All.FindAndRemove( this );
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

	// size from the model, in the vehicle's own frame (forward / sideways)
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	float flYawRad = DEG2RAD( m_pType->flForwardYaw );
	Vector vecFwd( cosf( flYawRad ), sinf( flYawRad ), 0.0f );
	m_flHalfLength = 0.5f * fabs( DotProduct( vecMaxs - vecMins, vecFwd ) );
	m_flHalfWidth = 0.5f * fabs( DotProduct( vecMaxs - vecMins, Vector( -vecFwd.y, vecFwd.x, 0.0f ) ) );
	if ( m_flHalfWidth > m_flHalfLength * 1.05f )
		Warning( "INVARIANT DOD-SP vehicle %s (%s): the model is %.0f long but %.0f wide along forward_yaw %.0f - a vehicle is longer than it is wide, so forward_yaw is probably wrong (90 off)\n",
				 GetDebugName(), m_pType->szName, m_flHalfLength * 2.0f, m_flHalfWidth * 2.0f, m_pType->flForwardYaw );

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
int CDoDSPVehicle::NearestNode( const Vector &vecPos, bool bFromHere )
{
	VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_pType->szName );
	if ( !pNav )
		return -1;
	// the nearest graded node it can drive to (or from) straight: candidates by distance, the first 16 tested
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
	for ( int k = 0; k < cands.Count() && k < 16; k++ )
	{
		int iMin = k;
		for ( int m = k + 1; m < cands.Count(); m++ )
			if ( cands[m].flDist < cands[iMin].flDist )
				iMin = m;
		Cand_t tmp = cands[k]; cands[k] = cands[iMin]; cands[iMin] = tmp;
		const Vector &vecNode = g_pBigAINet->GetNode( cands[k].i, false )->GetOrigin();
		if ( CorridorClear( bFromHere ? vecPos : vecNode, bFromHere ? vecNode : vecPos, m_flHalfWidth, this ) )
			return cands[k].i;
	}
	return -1;
}

bool CDoDSPVehicle::PlanRoute( void )
{
	m_Route.RemoveAll();
	m_iRoute = 0;
	Vector vecFrom = GetAbsOrigin();
	if ( CorridorClear( vecFrom, m_vecGoal, m_flHalfWidth, this ) )
	{
		m_Route.AddToTail( m_vecGoal );		// straight there
		return true;
	}
	VehicleNav_t *pNav = GetVehicleNav( m_flHalfWidth, m_pType->szName );
	int iStart = NearestNode( vecFrom, true );
	int iEnd = NearestNode( m_vecGoal, false );
	if ( !pNav || iStart < 0 || iEnd < 0 )
		return false;

	// A* over the graded links, skipping links it got stuck on lately
	int n = pNav->nNodes;
	CUtlVector<float> g; g.SetCount( n );
	CUtlVector<int> from; from.SetCount( n );
	CUtlVector<char> closed; closed.SetCount( n );
	for ( int i = 0; i < n; i++ ) { g[i] = FLT_MAX; from[i] = -1; closed[i] = 0; }
	CUtlVector<int> open;
	g[iStart] = 0.0f;
	open.AddToTail( iStart );
	const Vector &vecEnd = g_pBigAINet->GetNode( iEnd, false )->GetOrigin();
	float now = gpGlobals->curtime;
	while ( open.Count() )
	{
		int iBestOpen = 0;
		float flBestF = FLT_MAX;
		for ( int k = 0; k < open.Count(); k++ )
		{
			float f = g[open[k]] + ( g_pBigAINet->GetNode( open[k], false )->GetOrigin() - vecEnd ).Length();
			if ( f < flBestF ) { flBestF = f; iBestOpen = k; }
		}
		int cur = open[iBestOpen];
		open.FastRemove( iBestOpen );
		if ( cur == iEnd )
			break;
		closed[cur] = 1;
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
	if ( g[iEnd] == FLT_MAX )
		return false;

	CUtlVector<int> nodes;
	for ( int i = iEnd; i >= 0; i = from[i] )
		nodes.AddToHead( i );
	// straighten: from each point, the furthest later point it can drive to directly
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
			if ( CorridorClear( pts[iAt], pts[j], m_flHalfWidth, this ) )
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
	float now = gpGlobals->curtime;
	Vector vecPos = GetAbsOrigin();
	Vector vecFwd( cosf( DEG2RAD( m_flHeading ) ), sinf( DEG2RAD( m_flHeading ) ), 0.0f );
	if ( m_flNavCheckTime > 0.0f && now >= m_flNavCheckTime )
	{
		m_flNavCheckTime = 0.0f;
		GetVehicleNav( m_flHalfWidth, m_pType->szName );		// the load line, before the fight
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

	// walls: sweep its own collision model ahead (pushers go through the world)
	if ( fabs( m_flSpeed ) > 1.0f )
	{
		Vector vecLift( 0, 0, VEHICLE_STEP );
		Vector vecAhead = vecFwd * ( m_flSpeed * dt * 2.0f + ( m_flSpeed > 0 ? 6.0f : -6.0f ) );
		CTraceFilterVehicleWorld filter( this );
		trace_t tr;
		UTIL_TraceEntity( this, vecPos + vecLift, vecPos + vecLift + vecAhead, MASK_SOLID, &filter, &tr );
		if ( tr.fraction < 1.0f && !tr.startsolid )
		{
			m_flSpeed = 0.0f;
			if ( m_eStuck != VSTUCK_WORLD && m_bHasGoal && now - m_flProgressTime > 1.0f )
			{
				m_eStuck = VSTUCK_WORLD;
				if ( dodsp_vehicle_debug.GetBool() )
					Msg( "DOD-SP vehicle %s: wall ahead (%s)\n", GetDebugName(), tr.m_pEnt ? tr.m_pEnt->GetClassname() : "world" );
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
