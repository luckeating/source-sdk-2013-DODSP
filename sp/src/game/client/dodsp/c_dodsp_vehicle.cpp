//====== DOD-SP =====================================================================
//
// Client side of dodsp_vehicle: the wheels turn (and steer) and the tracks scroll at the speed the server drives.
// The distance driven is one owner (m_flDistance) for both, so wheels and tracks never disagree.
//
// Wheels (H1): the model says which bones are wheels, how big each is and which steer - $keyvalues "dodsp_wheels",
// measured from the mesh by the rig tool (dodsp/dodsp_vehicle_wheels.h, shared with the server). Each turns about the
// axle (the model's up x forward) through its own centre by distance / its radius; the steered ones are turned first
// about the up axis by the server's steering angle (m_flSteer). A model without the block keeps V1's rule: every bone
// with "wheel" in its name, radius = its height above the vehicle's floor, nothing steers. Done after the animation
// has built the skeleton (BuildTransformations), so any model works without an animation of its own.
//
// Suspension (T1): the model's suspension units (susp_*, CoD2's own running-gear rig, travel measured from the mesh)
// each sit on the ground under them, every frame (UpdateSuspension, the ONE writer of a unit's lift), within their
// travel; the unit's bone and every bone under it (its road wheels, the top run where it rests on them) move with it,
// and the track belt bends between units by its vertex weights. On a sprung body the room over a unit shrinks by
// however far the body has come down over it.
//
// Tracks (H1): material proxy "DoDSPTrackScroll" (also answers to the CoD2 conversion's "COD2TreadScroll"). WHICH
// materials are tracks, which texture axis runs along them and how fast is measured from the meshes
// (dodsp_tools/vehicle_preview/tread_scroll.py) and shipped as scripts/dodsp_treads.txt - the .vmt only names the
// proxy:   "Proxies" { "DoDSPTrackScroll" { } }
// The vmt's own axis/scale are ignored (the conversion had them wrong: on the Tiger and Sherman the proxy sat on the
// wheel markings and ran backwards). A proxy on a material that isn't a measured track keeps it still and says so; a
// measured track whose .vmt has no proxy is named at map load with the line to add. The bump map scrolls with it.
//
//==================================================================================
#include "cbase.h"
#include "c_baseanimating.h"
#include "proxyentity.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "toolframework_client.h"
#include "mathlib/vmatrix.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "bone_setup.h"
#include "dodsp/dodsp_vehicle_wheels.h"
#include "c_te_effect_dispatch.h"
#include "physics_shared.h"
#include "decals.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

void ToolFramework_RecordMaterialParams( IMaterial *pMaterial );

#define DODSP_TREADS_FILE	"scripts/dodsp_treads.txt"

// H1 dust: what the running gear throws up, by speed and by what it drives on (HL2's "WheelDust" puff, tinted by the
// light where it rises). 0 = none.
static ConVar dodsp_vehicle_dust( "dodsp_vehicle_dust", "1", 0, "DOD-SP: dust thrown up by vehicles' wheels and tracks (scale, 0 = none)" );

//-----------------------------------------------------------------------------
// The measured tracks (one owner: tread_scroll.py -> scripts/dodsp_treads.txt), keyed by material file name
//-----------------------------------------------------------------------------
struct DoDSPTread_t { bool bAxisU; float flScale; };
static CUtlDict<DoDSPTread_t, int>	s_Treads;
static bool							s_bTreadsLoaded = false;
static CUtlDict<bool, int>			s_TreadProxyOn;		// materials a track proxy initialised on
static CUtlDict<bool, int>			s_TreadWarned;

static void TreadKey( const char *pszMaterial, char *pszOut, int nOut )
{
	char szBase[MAX_PATH];
	Q_strncpy( szBase, V_UnqualifiedFileName( pszMaterial ), sizeof( szBase ) );
	Q_StripExtension( szBase, pszOut, nOut );
	Q_strlower( pszOut );
}

static const DoDSPTread_t *FindTread( const char *pszMaterial )
{
	if ( !s_bTreadsLoaded )
	{
		s_bTreadsLoaded = true;
		KeyValues *pKV = new KeyValues( "DoDSPTreads" );
		if ( pKV->LoadFromFile( filesystem, DODSP_TREADS_FILE, "GAME" ) )
		{
			for ( KeyValues *p = pKV->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey() )
			{
				DoDSPTread_t t;
				t.bAxisU = !Q_stricmp( p->GetString( "axis", "v" ), "u" );
				t.flScale = p->GetFloat( "scale", 0.0f );
				char szKey[MAX_PATH];
				TreadKey( p->GetName(), szKey, sizeof( szKey ) );
				s_Treads.Insert( szKey, t );
			}
		}
		else
			Warning( "INVARIANT DOD-SP track scroll: %s is missing - no track will scroll (it ships in the game artifact's scripts folder)\n", DODSP_TREADS_FILE );
		pKV->deleteThis();
	}
	char szKey[MAX_PATH];
	TreadKey( pszMaterial, szKey, sizeof( szKey ) );
	int i = s_Treads.Find( szKey );
	return i == s_Treads.InvalidIndex() ? NULL : &s_Treads[i];
}

static bool WarnOnce( const char *pszKey )
{
	if ( s_TreadWarned.Find( pszKey ) != s_TreadWarned.InvalidIndex() )
		return false;
	s_TreadWarned.Insert( pszKey, true );
	return true;
}

//-----------------------------------------------------------------------------
class C_DoDSPVehicle : public C_BaseAnimating
{
public:
	DECLARE_CLASS( C_DoDSPVehicle, C_BaseAnimating );
	DECLARE_CLIENTCLASS();

	C_DoDSPVehicle();
	virtual void	BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed );
	virtual void	OnDataChanged( DataUpdateType_t type );
	virtual void	ClientThink( void );
	float			GetDistance( void );
	using BaseClass::GetAttachment;
	virtual bool	GetAttachment( int number, matrix3x4_t &matrix );					// V5: the body attachment
	virtual bool	GetAttachment( int number, Vector &origin, QAngle &angles );

private:
	struct Wheel_t
	{
		int		iBone;
		float	flRadius;		// 0 = the height rule
		bool	bSteer;
	};
	struct Susp_t
	{
		int		iBone;
		float	flUp, flDown;	// its travel (the model's, measured from the mesh)
		float	flLift;			// how far it has risen (+) / dropped (-) to the ground under it - UpdateSuspension only
		Vector	vecRest;		// its ground contact before the lift, world, as of the last skeleton
		bool	bRest;
	};
	void			BuildWheelList( CStudioHdr *pStudioHdr );
	void			UpdateSuspension( float dt );
	void			CheckTreadProxies( CStudioHdr *pStudioHdr );
	void			BodySprings( float dt );
	void			KickDust( float dt );
	float			m_flDustDue[2];			// puffs owed per rear corner (fractional)
	bool			BodyMatrix( matrix3x4_t &matBody );

	float			m_flTrackSpeed;
	float			m_flModelForwardYaw;
	float			m_flSteer;
	float			m_flDistance;			// driven so far (units, signed)
	float			m_flDistanceTime;
	CUtlVector<Wheel_t>	m_Wheels;
	CUtlVector<bool>	m_IsWheel;			// per bone
	CUtlVector<Susp_t>	m_Susp;
	CUtlVector<int>		m_SuspOf;			// per bone: the unit it rides on (itself or an ancestor), -1 = none
	CUtlVector<bool>	m_IsBody;			// per bone: tips with the sprung body (the model's body_bone and its children)
	CStudioHdr		*m_pWheelHdr;			// the model the wheel list was made for
	float			m_flBodyPivot;			// model forward (x) the body tips about (the model's "body_pivot")
	bool			m_bHasPivot;

	// H1 suspension (server: m_bBodySprung): the body rides on springs over planted wheels, run here every frame
	bool			m_bBodySprung;
	float			m_flBodySuspAccel, m_flBodySuspRoll, m_flBodySuspFreq, m_flBodySuspDamp;
	int				m_nBodyKicks, m_nBodyKicksSeen;
	float			m_flBodyKickPitch, m_flBodyKickRoll;
	float			m_flBodyPitch, m_flBodyPitchVel;	// degrees, nose up +
	float			m_flBodyRoll, m_flBodyRollVel;		// degrees, right side down +
	float			m_flSpeedLP, m_flAccelLP, m_flYawRateLP;
	Vector			m_vecPrevOrigin;
	QAngle			m_angPrev;
	bool			m_bHavePrev;
};

IMPLEMENT_CLIENTCLASS_DT( C_DoDSPVehicle, DT_DoDSPVehicle, CDoDSPVehicle )
	RecvPropFloat( RECVINFO( m_flTrackSpeed ) ),
	RecvPropFloat( RECVINFO( m_flModelForwardYaw ) ),
	RecvPropFloat( RECVINFO( m_flSteer ) ),
	RecvPropBool( RECVINFO( m_bBodySprung ) ),
	RecvPropFloat( RECVINFO( m_flBodySuspAccel ) ),
	RecvPropFloat( RECVINFO( m_flBodySuspRoll ) ),
	RecvPropFloat( RECVINFO( m_flBodySuspFreq ) ),
	RecvPropFloat( RECVINFO( m_flBodySuspDamp ) ),
	RecvPropInt( RECVINFO( m_nBodyKicks ) ),
	RecvPropFloat( RECVINFO( m_flBodyKickPitch ) ),
	RecvPropFloat( RECVINFO( m_flBodyKickRoll ) ),
END_RECV_TABLE()

C_DoDSPVehicle::C_DoDSPVehicle()
{
	m_flTrackSpeed = 0.0f;
	m_flModelForwardYaw = 0.0f;
	m_flSteer = 0.0f;
	m_flDistance = 0.0f;
	m_flDistanceTime = 0.0f;
	m_pWheelHdr = NULL;
	m_flBodyPivot = 0.0f;
	m_bHasPivot = false;
	m_bBodySprung = false;
	m_flBodySuspAccel = m_flBodySuspRoll = m_flBodySuspDamp = 0.0f;
	m_flBodySuspFreq = 1.0f;
	m_nBodyKicks = 0;
	m_nBodyKicksSeen = -1;
	m_flBodyKickPitch = m_flBodyKickRoll = 0.0f;
	m_flBodyPitch = m_flBodyPitchVel = m_flBodyRoll = m_flBodyRollVel = 0.0f;
	m_flSpeedLP = m_flAccelLP = m_flYawRateLP = 0.0f;
	m_vecPrevOrigin.Init();
	m_angPrev.Init();
	m_bHavePrev = false;
	m_flDustDue[0] = m_flDustDue[1] = 0.0f;
}

void C_DoDSPVehicle::OnDataChanged( DataUpdateType_t type )
{
	BaseClass::OnDataChanged( type );
	if ( type == DATA_UPDATE_CREATED )
		SetNextClientThink( CLIENT_THINK_ALWAYS );
}

void C_DoDSPVehicle::ClientThink( void )
{
	BodySprings( gpGlobals->frametime );
	UpdateSuspension( gpGlobals->frametime );
	KickDust( gpGlobals->frametime );
}

// how much dust a surface gives (1 = dry dirt): by the game material of the ground under the running gear
static float DustFromGround( const trace_t &tr )
{
	const surfacedata_t *pSurf = physprops ? physprops->GetSurfaceData( tr.surface.surfaceProps ) : NULL;
	if ( !pSurf )
		return 0.3f;
	switch ( pSurf->game.material )
	{
	case CHAR_TEX_DIRT:		return 1.0f;
	case CHAR_TEX_SAND:		return 1.0f;
	case 'J':				return 0.7f;		// grass in the DoD / CS surface sets
	case CHAR_TEX_FOLIAGE:	return 0.5f;
	case CHAR_TEX_CONCRETE:
	case CHAR_TEX_TILE:		return 0.25f;		// stone and paving: a little
	case CHAR_TEX_WOOD:		return 0.2f;
	case CHAR_TEX_METAL:
	case CHAR_TEX_GRATE:	return 0.1f;
	case CHAR_TEX_SLOSH:	return 0.0f;		// water and mud splash, not dust
	default:				return 0.3f;
	}
}

// H1: the rear corners of the running gear (the front ones when reversing) throw a puff each, more and bigger the
// faster it goes. Client only, nothing networked; one trace per puff.
void C_DoDSPVehicle::KickDust( float dt )
{
	float flScale = dodsp_vehicle_dust.GetFloat();
	float flSpeed = m_flTrackSpeed;
	if ( flScale <= 0.0f || dt <= 0.0f || dt > 0.25f || fabs( flSpeed ) < 20.0f || IsDormant() )
		return;
	matrix3x4_t matEnt;
	AngleMatrix( GetAbsAngles(), GetAbsOrigin(), matEnt );
	float flFY = DEG2RAD( m_flModelForwardYaw );
	Vector vecFwd( cosf( flFY ), sinf( flFY ), 0.0f ), vecLeft( -vecFwd.y, vecFwd.x, 0.0f );
	Vector vecMins = CollisionProp()->OBBMins(), vecMaxs = CollisionProp()->OBBMaxs();
	// the box's extent along the model's forward and left (works for any forward_yaw)
	float flBack = FLT_MAX, flFront = -FLT_MAX, flSide = 0.0f;
	for ( int c = 0; c < 8; c++ )
	{
		Vector v( ( c & 1 ) ? vecMaxs.x : vecMins.x, ( c & 2 ) ? vecMaxs.y : vecMins.y, 0.0f );
		float f = DotProduct( v, vecFwd );
		flBack = MIN( flBack, f );
		flFront = MAX( flFront, f );
		flSide = MAX( flSide, fabs( DotProduct( v, vecLeft ) ) );
	}
	float flAlong = ( flSpeed > 0.0f ) ? flBack * 0.85f : flFront * 0.85f;
	float flRate = clamp( fabs( flSpeed ) / 25.0f, 0.0f, 12.0f );		// puffs a second per corner
	float flSize = clamp( fabs( flSpeed ) / 150.0f, 0.4f, 1.4f );
	for ( int k = 0; k < 2; k++ )
	{
		m_flDustDue[k] += flRate * dt;
		if ( m_flDustDue[k] < 1.0f )
			continue;
		m_flDustDue[k] -= floorf( m_flDustDue[k] );
		Vector vecLocal = vecFwd * flAlong + vecLeft * ( ( k ? -1.0f : 1.0f ) * flSide * 0.75f ), vecAt;
		VectorTransform( vecLocal, matEnt, vecAt );
		trace_t tr;
		UTIL_TraceLine( vecAt + Vector( 0, 0, 24 ), vecAt - Vector( 0, 0, 48 ), MASK_SOLID_BRUSHONLY, this, COLLISION_GROUP_NONE, &tr );
		if ( tr.fraction >= 1.0f || tr.startsolid )
			continue;
		float flAmount = DustFromGround( tr ) * flSize * flScale;
		if ( flAmount < 0.05f )
			continue;
		CEffectData data;
		data.m_vOrigin = tr.endpos;
		data.m_vNormal = tr.plane.normal;
		data.m_flScale = flAmount;
		DispatchEffect( "WheelDust", data );
	}
}

// H1: THE body's springs (sprung bodies only; tanks keep the server's lurch). Driven by what the vehicle is visibly
// doing - its drawn (interpolated) origin and yaw, so it can't step with the 20 Hz thinks - plus the server's kicks.
// Pitch: nose up per 100 units/s^2 of acceleration (susp_accel); roll: outward per 100 units/s^2 sideways (speed x yaw
// rate, susp_roll); spring susp_freq Hz with susp_damp damping. (The wheels follow the ground in UpdateSuspension.)
void C_DoDSPVehicle::BodySprings( float dt )
{
	Vector vecOrigin = GetAbsOrigin();
	QAngle ang = GetAbsAngles();
	if ( !m_bBodySprung || !m_bHasPivot )
	{
		m_bHavePrev = false;
		return;
	}
	if ( m_nBodyKicksSeen < 0 )
		m_nBodyKicksSeen = m_nBodyKicks;		// kicks before we saw it are history
	if ( !m_bHavePrev || dt <= 0.0f || dt > 0.25f )
	{
		m_vecPrevOrigin = vecOrigin;
		m_angPrev = ang;
		m_bHavePrev = true;
		return;
	}
	matrix3x4_t matEnt;
	AngleMatrix( ang, matEnt );
	float flFY = DEG2RAD( m_flModelForwardYaw );
	Vector vecFwd;
	VectorRotate( Vector( cosf( flFY ), sinf( flFY ), 0.0f ), matEnt, vecFwd );
	float v = DotProduct( vecOrigin - m_vecPrevOrigin, vecFwd ) / dt;
	float a = MIN( 1.0f, dt / 0.08f );
	float flPrevSpeed = m_flSpeedLP;
	m_flSpeedLP += ( v - m_flSpeedLP ) * a;
	m_flAccelLP += ( ( m_flSpeedLP - flPrevSpeed ) / dt - m_flAccelLP ) * a;
	m_flYawRateLP += ( DEG2RAD( AngleDiff( ang.y, m_angPrev.y ) ) / dt - m_flYawRateLP ) * a;
	m_vecPrevOrigin = vecOrigin;
	m_angPrev = ang;

	if ( m_nBodyKicks != m_nBodyKicksSeen )
	{
		m_flBodyPitchVel += m_flBodyKickPitch;
		m_flBodyRollVel += m_flBodyKickRoll;
		m_nBodyKicksSeen = m_nBodyKicks;
	}
	float flPitchTarget = clamp( m_flAccelLP / 100.0f * m_flBodySuspAccel, -6.0f, 6.0f );
	float flRollTarget = clamp( m_flSpeedLP * m_flYawRateLP / 100.0f * m_flBodySuspRoll, -6.0f, 6.0f );
	float w = 2.0f * M_PI_F * MAX( m_flBodySuspFreq, 0.1f );
	for ( float t = 0.0f; t < dt; t += 1.0f / 120.0f )
	{
		float h = MIN( 1.0f / 120.0f, dt - t );
		m_flBodyPitchVel += ( w * w * ( flPitchTarget - m_flBodyPitch ) - 2.0f * m_flBodySuspDamp * w * m_flBodyPitchVel ) * h;
		m_flBodyRollVel += ( w * w * ( flRollTarget - m_flBodyRoll ) - 2.0f * m_flBodySuspDamp * w * m_flBodyRollVel ) * h;
		m_flBodyPitch = clamp( m_flBodyPitch + m_flBodyPitchVel * h, -8.0f, 8.0f );
		m_flBodyRoll = clamp( m_flBodyRoll + m_flBodyRollVel * h, -8.0f, 8.0f );
	}
	// V5: what sits or is mounted on the body (parented to DODSP_BODY_ATTACHMENT) follows it: the engine keeps a child's
	// world position until its parent MOVES, and a body rocking over a parked chassis isn't a move - so say so
	for ( C_BaseEntity *pChild = FirstMoveChild(); pChild; pChild = pChild->NextMovePeer() )
		if ( pChild->GetParentAttachment() == DODSP_BODY_ATTACHMENT )
			pChild->InvalidatePhysicsRecursive( POSITION_CHANGED | ANGLES_CHANGED );
}

// V5: the body attachment (dodsp_vehicle_wheels.h) - the vehicle's transform tipped by the body's spring, so the riders,
// the crew and the guns on a sprung body rock with it. The server answers the same index with the untipped vehicle.
bool C_DoDSPVehicle::GetAttachment( int number, matrix3x4_t &matrix )
{
	if ( number != DODSP_BODY_ATTACHMENT )
		return BaseClass::GetAttachment( number, matrix );
	matrix3x4_t matBody;
	if ( BodyMatrix( matBody ) )
		ConcatTransforms( matBody, EntityToWorldTransform(), matrix );
	else
		MatrixCopy( EntityToWorldTransform(), matrix );
	return true;
}

bool C_DoDSPVehicle::GetAttachment( int number, Vector &origin, QAngle &angles )
{
	if ( number != DODSP_BODY_ATTACHMENT )
		return BaseClass::GetAttachment( number, origin, angles );
	matrix3x4_t matrix;
	GetAttachment( number, matrix );
	MatrixAngles( matrix, angles, origin );
	return true;
}

// T1: THE suspension. Every unit sits on the ground straight under its contact (its bone's rest spot, on the ground),
// as a height along the vehicle's up, within its measured travel, followed at VEHICLE_SUSP_RATE. Over a sprung body the
// room above a unit shrinks by however far the body has come down there (the halftrack's wings over its front tyres).
#define VEHICLE_SUSP_RATE	160.0f		// units/s
void C_DoDSPVehicle::UpdateSuspension( float dt )
{
	if ( !m_Susp.Count() || dt <= 0.0f )
		return;
	matrix3x4_t matEnt;
	AngleMatrix( GetAbsAngles(), matEnt );
	Vector vecUp;
	VectorRotate( Vector( 0, 0, 1 ), matEnt, vecUp );
	matrix3x4_t matBody;
	bool bTipped = BodyMatrix( matBody );
	float flStep = VEHICLE_SUSP_RATE * MIN( dt, 0.1f );
	FOR_EACH_VEC( m_Susp, k )
	{
		Susp_t &su = m_Susp[k];
		if ( !su.bRest )
			continue;
		float flUp = su.flUp;
		if ( bTipped && !m_IsBody[su.iBone] )
		{
			Vector vecOver = su.vecRest + vecUp * su.flUp, vecOverNow;
			VectorTransform( vecOver, matBody, vecOverNow );
			flUp = MIN( flUp, su.flUp + DotProduct( vecOverNow - vecOver, vecUp ) );
		}
		trace_t tr;
		UTIL_TraceLine( su.vecRest + vecUp * ( su.flUp + 8.0f ), su.vecRest - vecUp * ( su.flDown + 2.0f ),
						MASK_SOLID_BRUSHONLY, this, COLLISION_GROUP_NONE, &tr );
		float flWant = tr.startsolid ? flUp : ( tr.fraction < 1.0f ? DotProduct( tr.endpos - su.vecRest, vecUp ) : -su.flDown );	// inside a bank = as high as it goes
		flWant = clamp( flWant, -su.flDown, MAX( flUp, -su.flDown ) );
		su.flLift += clamp( flWant - su.flLift, -flStep, flStep );
	}
}

float C_DoDSPVehicle::GetDistance( void )
{
	// integrate once per frame however often it's asked
	float now = gpGlobals->curtime;
	float dt = now - m_flDistanceTime;
	if ( dt > 0.0f && dt < 0.5f )
		m_flDistance += m_flTrackSpeed * dt;
	m_flDistanceTime = now;
	return m_flDistance;
}

void C_DoDSPVehicle::BuildWheelList( CStudioHdr *pStudioHdr )
{
	m_Wheels.RemoveAll();
	m_Susp.RemoveAll();
	m_SuspOf.SetCount( pStudioHdr->numbones() );
	FOR_EACH_VEC( m_SuspOf, i )
		m_SuspOf[i] = -1;
	m_IsWheel.SetCount( pStudioHdr->numbones() );
	FOR_EACH_VEC( m_IsWheel, i )
		m_IsWheel[i] = false;
	DoDSPWheels_t data;
	bool bData = DoDSP_ReadWheels( GetModel(), data );
	m_flBodyPivot = data.flBodyPivot;
	m_bHasPivot = bData && data.bBodyPivot;
	// the sprung body: the named bone and everything under it (the doors); without one, every bone but the wheels
	int iBody = ( bData && data.szBodyBone[0] ) ? Studio_BoneIndexByName( pStudioHdr, data.szBodyBone ) : -1;
	if ( bData && data.szBodyBone[0] && iBody < 0 )
		Warning( "INVARIANT DOD-SP wheels: %s's body_bone %s isn't in the model - its body won't ride on springs\n",
				 modelinfo->GetModelName( GetModel() ), data.szBodyBone );
	if ( bData && data.szBodyBone[0] && iBody < 0 )
		m_bHasPivot = false;
	m_IsBody.SetCount( pStudioHdr->numbones() );
	for ( int i = 0; i < pStudioHdr->numbones(); i++ )
	{
		bool bBody = false;
		if ( iBody >= 0 )
			for ( int j = i; j >= 0; j = pStudioHdr->pBone( j )->parent )
				if ( j == iBody ) { bBody = true; break; }
		m_IsBody[i] = bBody;
	}
	bool bByName = iBody < 0;		// no body bone named: the body is every bone that isn't a wheel (set below)
	if ( bData )
	{
		FOR_EACH_VEC( data.wheels, i )
		{
			Wheel_t w;
			w.iBone = Studio_BoneIndexByName( pStudioHdr, data.wheels[i].szBone );
			w.flRadius = data.wheels[i].flRadius;
			w.bSteer = data.wheels[i].bSteer;
			if ( w.iBone < 0 )
			{
				char szKey[MAX_PATH];
				Q_snprintf( szKey, sizeof( szKey ), "%s:%s", modelinfo->GetModelName( GetModel() ), data.wheels[i].szBone );
				if ( WarnOnce( szKey ) )
					Warning( "INVARIANT DOD-SP wheels: %s's dodsp_wheels names bone %s, which the model hasn't got - recompile it from its rig\n",
							 modelinfo->GetModelName( GetModel() ), data.wheels[i].szBone );
				continue;
			}
			m_Wheels.AddToTail( w );
			m_IsWheel[w.iBone] = true;
		}
		FOR_EACH_VEC( data.susp, i )
		{
			Susp_t su;
			su.iBone = Studio_BoneIndexByName( pStudioHdr, data.susp[i].szBone );
			su.flUp = data.susp[i].flUp;
			su.flDown = data.susp[i].flDown;
			su.flLift = 0.0f;
			su.vecRest.Init();
			su.bRest = false;
			if ( su.iBone < 0 )
			{
				char szKey[MAX_PATH];
				Q_snprintf( szKey, sizeof( szKey ), "%s:%s", modelinfo->GetModelName( GetModel() ), data.susp[i].szBone );
				if ( WarnOnce( szKey ) )
					Warning( "INVARIANT DOD-SP wheels: %s's dodsp_wheels names suspension unit %s, which the model hasn't got - recompile it from its rig\n",
							 modelinfo->GetModelName( GetModel() ), data.susp[i].szBone );
				continue;
			}
			m_Susp.AddToTail( su );
		}
		// each bone rides on the nearest unit at or above it (its road wheels, the top run resting on them)
		for ( int b = 0; b < pStudioHdr->numbones(); b++ )
			for ( int j = b; j >= 0 && m_SuspOf[b] < 0; j = pStudioHdr->pBone( j )->parent )
				FOR_EACH_VEC( m_Susp, k )
					if ( m_Susp[k].iBone == j ) { m_SuspOf[b] = k; break; }
		if ( bByName )
			FOR_EACH_VEC( m_IsBody, i )
				m_IsBody[i] = !m_IsWheel[i] && m_SuspOf[i] < 0;
		return;
	}
	for ( int i = 0; i < pStudioHdr->numbones(); i++ )
		if ( Q_stristr( pStudioHdr->pBone( i )->pszName(), "wheel" ) )
		{
			Wheel_t w;
			w.iBone = i;
			w.flRadius = 0.0f;
			w.bSteer = false;
			m_Wheels.AddToTail( w );
			m_IsWheel[i] = true;
		}
}

// a measured track of this model whose .vmt names no scroll proxy: say which file and what to add (once per material)
void C_DoDSPVehicle::CheckTreadProxies( CStudioHdr *pStudioHdr )
{
	const studiohdr_t *pHdr = pStudioHdr->GetRenderHdr();
	if ( !pHdr )
		return;
	for ( int i = 0; i < pHdr->numtextures; i++ )
	{
		const char *pszName = pHdr->pTexture( i )->pszName();
		if ( !FindTread( pszName ) )
			continue;
		char szKey[MAX_PATH];
		TreadKey( pszName, szKey, sizeof( szKey ) );
		if ( s_TreadProxyOn.Find( szKey ) != s_TreadProxyOn.InvalidIndex() || !WarnOnce( szKey ) )
			continue;
		Warning( "DOD-SP track scroll: %s is %s's track but its .vmt has no scroll proxy, so it stays still - add inside its \"VertexLitGeneric\" { }:\n"
				 "    \"Proxies\" { \"DoDSPTrackScroll\" { } }\n", szKey, modelinfo->GetModelName( GetModel() ) );
	}
}

// THE body's tip as a world transform: pitch about the model's left axis, roll about its forward axis, both through the
// pivot line (the model's body_pivot, on the ground). false = not a sprung body, or level.
bool C_DoDSPVehicle::BodyMatrix( matrix3x4_t &matBody )
{
	if ( !m_bBodySprung || !m_bHasPivot || ( fabs( m_flBodyPitch ) < 0.001f && fabs( m_flBodyRoll ) < 0.001f ) )
		return false;
	matrix3x4_t matEnt;
	AngleMatrix( GetAbsAngles(), matEnt );
	float flFY = DEG2RAD( m_flModelForwardYaw );
	Vector vecModelFwd( cosf( flFY ), sinf( flFY ), 0.0f ), vecFwd, vecAxle;
	VectorRotate( vecModelFwd, matEnt, vecFwd );
	VectorRotate( CrossProduct( Vector( 0, 0, 1 ), vecModelFwd ), matEnt, vecAxle );
	Vector vecPivot = GetAbsOrigin() + vecFwd * m_flBodyPivot;
	matrix3x4_t matPitch, matRoll;
	MatrixBuildRotationAboutAxis( vecAxle, -m_flBodyPitch, matPitch );		// about the left axis: - = nose up
	MatrixBuildRotationAboutAxis( vecFwd, m_flBodyRoll, matRoll );			// about forward: + = right side down
	ConcatTransforms( matRoll, matPitch, matBody );
	Vector vecTurned;
	VectorRotate( vecPivot, matBody, vecTurned );
	MatrixSetColumn( vecPivot - vecTurned, 3, matBody );			// about the pivot, not the world origin
	return true;
}

void C_DoDSPVehicle::BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed )
{
	BaseClass::BuildTransformations( pStudioHdr, pos, q, cameraTransform, boneMask, boneComputed );
	if ( !pStudioHdr )
		return;
	if ( m_pWheelHdr != pStudioHdr )
	{
		m_pWheelHdr = pStudioHdr;
		BuildWheelList( pStudioHdr );
		CheckTreadProxies( pStudioHdr );
	}
	if ( !m_Wheels.Count() && !m_Susp.Count() )
		return;

	float flDist = GetDistance();
	// the axle in world space: the model's up x its forward
	float flRad = DEG2RAD( m_flModelForwardYaw );
	Vector vecModelFwd( cosf( flRad ), sinf( flRad ), 0.0f );
	Vector vecModelAxle = CrossProduct( Vector( 0, 0, 1 ), vecModelFwd );
	matrix3x4_t matEnt;
	AngleMatrix( GetAbsAngles(), matEnt );
	Vector vecAxle, vecUp;
	VectorRotate( vecModelAxle, matEnt, vecAxle );
	VectorRotate( Vector( 0, 0, 1 ), matEnt, vecUp );
	Vector vecOrigin = GetAbsOrigin();
	matrix3x4_t matSteer;
	MatrixBuildRotationAboutAxis( vecUp, m_flSteer, matSteer );		// + = the front of the wheel turns left

	// T1: each suspension unit and everything riding on it moves up/down by the unit's lift (UpdateSuspension)
	FOR_EACH_VEC( m_Susp, k )
	{
		Susp_t &su = m_Susp[k];
		if ( !( pStudioHdr->boneFlags( su.iBone ) & boneMask ) )
			continue;
		MatrixGetColumn( GetBoneForWrite( su.iBone ), 3, su.vecRest );		// UpdateSuspension traces from here
		su.bRest = true;
	}
	for ( int i = 0; i < pStudioHdr->numbones() && i < m_SuspOf.Count(); i++ )
	{
		if ( m_SuspOf[i] < 0 || !( pStudioHdr->boneFlags( i ) & boneMask ) )
			continue;
		matrix3x4_t &bone = GetBoneForWrite( i );
		Vector vecPos;
		MatrixGetColumn( bone, 3, vecPos );
		MatrixSetColumn( vecPos + vecUp * m_Susp[m_SuspOf[i]].flLift, 3, bone );
	}

	for ( int k = 0; k < m_Wheels.Count(); k++ )
	{
		Wheel_t &w = m_Wheels[k];
		if ( !( pStudioHdr->boneFlags( w.iBone ) & boneMask ) )
			continue;
		matrix3x4_t &bone = GetBoneForWrite( w.iBone );
		Vector vecCentre;
		MatrixGetColumn( bone, 3, vecCentre );		// already moved with its suspension unit
		float flRadius = w.flRadius;
		if ( flRadius <= 0.0f )
		{
			float flHeight = DotProduct( vecCentre - vecOrigin, vecUp );
			if ( flHeight < 4.0f )
				continue;		// a marker at ground level (CoD's tag_wheel_*), not a wheel
			flRadius = MIN( flHeight, 60.0f );
		}
		float flDeg = RAD2DEG( flDist / flRadius );
		matrix3x4_t rot, moved, out;
		MatrixBuildRotationAboutAxis( vecAxle, flDeg, rot );
		if ( w.bSteer )
		{
			matrix3x4_t spun = rot;
			ConcatTransforms( matSteer, spun, rot );		// spin about its own axle, then the axle turns with the steering
		}
		moved = bone;
		MatrixSetColumn( vec3_origin, 3, moved );		// turn about the wheel's own centre
		ConcatTransforms( rot, moved, out );
		MatrixSetColumn( vecCentre, 3, out );
		bone = out;
	}

	// H1 suspension: the body (the model's body_bone and its children - hull, doors) tips on its springs about the pivot
	// line (the rear running gear, on the ground); the running gear (root chassis with the tread belt, wheels) stays
	matrix3x4_t matBody;
	if ( BodyMatrix( matBody ) )
	{
		for ( int i = 0; i < pStudioHdr->numbones() && i < m_IsBody.Count(); i++ )
		{
			if ( !m_IsBody[i] || !( pStudioHdr->boneFlags( i ) & boneMask ) )
				continue;
			matrix3x4_t &bone = GetBoneForWrite( i );
			matrix3x4_t out;
			ConcatTransforms( matBody, bone, out );
			bone = out;
		}
	}
}

//-----------------------------------------------------------------------------
// DoDSPTrackScroll
//-----------------------------------------------------------------------------
class CDoDSPTrackScrollProxy : public CEntityMaterialProxy
{
public:
	CDoDSPTrackScrollProxy() : m_pScrollVar( NULL ), m_pBumpVar( NULL ), m_flScale( 0.0f ), m_bAxisU( false ), m_bTrack( false ) {}
	virtual bool Init( IMaterial *pMaterial, KeyValues *pKeyValues )
	{
		bool bFound;
		m_pScrollVar = pMaterial->FindVar( pKeyValues->GetString( "textureScrollVar", "$basetexturetransform" ), &bFound, false );
		if ( !bFound )
			return false;
		// the bump map runs with the colour (a still normal map on a moving track looks like it slides under the light)
		bool bBump = false, bBumpXform = false;
		IMaterialVar *pBumpMap = pMaterial->FindVar( "$bumpmap", &bBump, false );
		IMaterialVar *pBumpXform = pMaterial->FindVar( "$bumptransform", &bBumpXform, false );
		m_pBumpVar = ( bBump && pBumpMap && pBumpMap->IsDefined() && bBumpXform ) ? pBumpXform : NULL;
		char szKey[MAX_PATH];
		TreadKey( pMaterial->GetName(), szKey, sizeof( szKey ) );
		const DoDSPTread_t *pTread = FindTread( pMaterial->GetName() );
		m_bTrack = pTread != NULL;
		if ( pTread )
		{
			m_bAxisU = pTread->bAxisU;
			m_flScale = pTread->flScale;
			if ( s_TreadProxyOn.Find( szKey ) == s_TreadProxyOn.InvalidIndex() )
				s_TreadProxyOn.Insert( szKey, true );
		}
		else if ( WarnOnce( szKey ) )
			Warning( "INVARIANT DOD-SP track scroll: %s carries the track scroll proxy but isn't a vehicle's track (%s) - it stays still; "
					 "take the \"Proxies\" block out of its .vmt\n", pMaterial->GetName(), DODSP_TREADS_FILE );
		return true;
	}
	void Set( IMaterialVar *pVar, float flOffset )
	{
		if ( pVar->GetType() == MATERIAL_VAR_TYPE_MATRIX )
		{
			VMatrix mat;
			MatrixBuildTranslation( mat, m_bAxisU ? flOffset : 0.0f, m_bAxisU ? 0.0f : flOffset, 0.0f );
			pVar->SetMatrixValue( mat );
		}
		else
			pVar->SetVecValue( m_bAxisU ? flOffset : 0.0f, m_bAxisU ? 0.0f : flOffset, 0.0f );
	}
	virtual void OnBind( C_BaseEntity *pEntity )
	{
		if ( !m_pScrollVar || !pEntity || !m_bTrack )
			return;
		C_DoDSPVehicle *pVehicle = dynamic_cast<C_DoDSPVehicle *>( pEntity );
		if ( !pVehicle && pEntity->GetMoveParent() )
			pVehicle = dynamic_cast<C_DoDSPVehicle *>( pEntity->GetMoveParent() );
		float flOffset = pVehicle ? pVehicle->GetDistance() * m_flScale : 0.0f;
		flOffset -= floorf( flOffset );
		Set( m_pScrollVar, flOffset );
		if ( m_pBumpVar )
			Set( m_pBumpVar, flOffset );
		if ( ToolsEnabled() )
			ToolFramework_RecordMaterialParams( GetMaterial() );
	}
	virtual IMaterial *GetMaterial()
	{
		return m_pScrollVar ? m_pScrollVar->GetOwningMaterial() : NULL;
	}
private:
	IMaterialVar	*m_pScrollVar;
	IMaterialVar	*m_pBumpVar;
	float			m_flScale;
	bool			m_bAxisU;
	bool			m_bTrack;
};

EXPOSE_INTERFACE( CDoDSPTrackScrollProxy, IMaterialProxy, "DoDSPTrackScroll" IMATERIAL_PROXY_INTERFACE_VERSION );

// The CoD2 conversion's tread materials already name a "COD2TreadScroll" proxy (V1 test: "proxy not found").
// Same proxy under that name - one implementation, two names; the measured table decides what scrolls either way.
class CDoDSPCoD2TreadScrollProxy : public CDoDSPTrackScrollProxy {};
EXPOSE_INTERFACE( CDoDSPCoD2TreadScrollProxy, IMaterialProxy, "COD2TreadScroll" IMATERIAL_PROXY_INTERFACE_VERSION );
