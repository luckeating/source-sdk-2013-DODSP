//=============================================================================//
//
// DOD-SP: client side of npc_german_soldier.
//
// Procedural hit kick: the server networks the direction a bullet travelled,
// the hit location and a strength. After the animation has built the skeleton,
// we bend the spine / snap the head away from the shot and let it spring back.
// Because it pushes whatever pose the animation produced (standing, running,
// crouched, prone, reloading ...), it works in every stance, and since the arms
// are children of the spine they ride along - the gun stays in both hands.
//
// Ragdoll settle: a ragdoll that starts from a death-animation pose can begin
// slightly inside the floor or past its joint limits, and the physics solver
// throws it clear at huge speed. For the first moments of its life our ragdoll's
// pieces are speed-limited, so it settles instead of flying across the map.
//
// Support hand: CoD animations put the left hand where CoD's rifle had its fore-end,
// a few units off DoD's guns (and DoD's own left-hand IK is released under them).
// While a CoD animation plays and its left hand is on (or near) the gun, the arm is
// bent - two-bone IK - so the hand lands exactly on DoD's grip for the gun he carries.
// When the CoD hand lets go (clutching a wound, waving) it is left alone.
//
// Smoother hand-offs: when a full-body CoD animation (pain, death, cover) hands
// back to DoD's animations, the cross-fade length is the shorter of the two
// sequences' fade times. DoD's are the 0.2 s default, which reads as a snap, so
// DoD's full-body sequences get a longer fade-in (cl_npc_german_blend).
//
//=============================================================================//

#include "cbase.h"
#include "c_ai_basenpc.h"
#include "bone_setup.h"
#include "ragdoll.h"
#include "vphysics_interface.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar cl_npc_german_hitkick( "cl_npc_german_hitkick", "1", FCVAR_ARCHIVE, "Scale of the German soldiers' procedural hit reactions (0 = off)." );
ConVar cl_npc_german_hitkick_dir( "cl_npc_german_hitkick_dir", "1", FCVAR_ARCHIVE, "1 = hits push the body away from the shot, -1 = reverse (if the reactions look backwards)." );

ConVar cl_npc_german_ragdoll_maxspeed( "cl_npc_german_ragdoll_maxspeed", "180", FCVAR_ARCHIVE, "Speed limit (units/s) for a German soldier's ragdoll right after it is created (0 = no limit)." );
ConVar cl_npc_german_ragdoll_settle( "cl_npc_german_ragdoll_settle", "0.8", FCVAR_ARCHIVE, "Seconds the ragdoll speed limit stays on." );
ConVar cl_npc_german_blend( "cl_npc_german_blend", "0.4", FCVAR_ARCHIVE, "Minimum cross-fade (s) into DoD full-body animations; applied when a German soldier model loads." );

ConVar cl_npc_german_lhand_ik( "cl_npc_german_lhand_ik", "1", FCVAR_ARCHIVE, "Put the German soldiers' support hand on the gun during CoD animations (0 = off)." );

// DoD's own support-hand grips: the left hand in the gun bone's space, from DoD's aim
// poses (StandAim_BOLT for guns hung on weapon_bone_left, StandAim_MP40 for weapon_bone).
static const matrix3x4_t s_GripLeftHung(	-0.58605f, 0.40105f, 0.70406f, 2.90256f,
											-0.23640f, -0.91574f, 0.32486f, 2.18926f,
											0.77502f, 0.02394f, 0.63148f, 13.63848f );
static const matrix3x4_t s_GripRightHung(	-0.18726f, 0.97583f, 0.11260f, 2.64628f,
											0.26099f, -0.06109f, 0.96341f, -0.17044f,
											0.94701f, 0.20980f, -0.24324f, 12.66309f );
#define LHAND_FULL_DIST		8.0f	// CoD hand this close to DoD's grip: put it right on
#define LHAND_FREE_DIST		15.0f	// this far: the CoD hand has let go, leave it

#define HITKICK_RISE		0.06f	// seconds to full deflection
#define HITKICK_DURATION	0.75f	// total length of the reaction
#define HITKICK_MAX_DEG		16.0f	// deflection at strength 1

//-----------------------------------------------------------------------------
// Ragdoll with a short speed limit
//-----------------------------------------------------------------------------
class C_GermanRagdoll : public C_ClientRagdoll
{
	DECLARE_CLASS( C_GermanRagdoll, C_ClientRagdoll );
public:
	C_GermanRagdoll( bool bRestoring ) : C_ClientRagdoll( bRestoring )
	{
		m_flSettleUntil = gpGlobals->curtime + cl_npc_german_ragdoll_settle.GetFloat();
	}

	virtual void ClientThink( void )
	{
		BaseClass::ClientThink();
		if ( !m_pRagdoll || gpGlobals->curtime > m_flSettleUntil )
			return;

		float flMax = cl_npc_german_ragdoll_maxspeed.GetFloat();
		if ( flMax > 0.0f )
		{
			for ( int i = 0; i < m_pRagdoll->RagdollBoneCount(); i++ )
			{
				IPhysicsObject *pObj = m_pRagdoll->GetElement( i );
				if ( !pObj )
					continue;
				Vector vel;
				AngularImpulse angVel;
				pObj->GetVelocity( &vel, &angVel );
				float flSpeed = vel.Length();
				float flSpin = angVel.Length();
				if ( flSpeed > flMax || flSpin > 720.0f )
				{
					if ( flSpeed > flMax ) vel *= flMax / flSpeed;
					if ( flSpin > 720.0f ) angVel *= 720.0f / flSpin;
					pObj->SetVelocity( &vel, &angVel );
				}
			}
		}
		SetNextClientThink( CLIENT_THINK_ALWAYS );	// every frame while settling
	}

private:
	float m_flSettleUntil;
};

class C_NPC_GermanSoldier : public C_AI_BaseNPC
{
	DECLARE_CLASS( C_NPC_GermanSoldier, C_AI_BaseNPC );
public:
	DECLARE_CLIENTCLASS();

	C_NPC_GermanSoldier();

	virtual void BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed );
	virtual C_ClientRagdoll *CreateClientRagdoll( bool bRestoring = false );
	virtual void OnDataChanged( DataUpdateType_t updateType );

private:
	void	RotateSubtree( CStudioHdr *pStudioHdr, int iBone, const Vector &vecAxis, float flDegrees, int iSkip = -1 );
	void	RotateSubtreeTo( CStudioHdr *pStudioHdr, int iBone, const Vector &vecFrom, const Vector &vecTo, int iSkip );
	void	ApplyHitKick( CStudioHdr *pStudioHdr );
	void	SupportHandIK( CStudioHdr *pStudioHdr );
	bool	IsCoDSequence( CStudioHdr *pStudioHdr, int iSeq );
	float	CoDWeight( CStudioHdr *pStudioHdr );
	int		Bone( CStudioHdr *pStudioHdr, const char *pszName ) { return Studio_BoneIndexByName( pStudioHdr, pszName ); }

	Vector	m_vecHitDir;
	int		m_nHitGroup;
	float	m_flHitTime;
	float	m_flHitStrength;

	CUtlVector<signed char>	m_SeqIsCoD;		// per sequence: -1 unknown, 0 DoD, 1 CoD pack
	float	m_flLHandWeight;				// smoothed share of CoD animation in the pose
	float	m_flLHandTime;

	C_NPC_GermanSoldier( const C_NPC_GermanSoldier & );
};

IMPLEMENT_CLIENTCLASS_DT( C_NPC_GermanSoldier, DT_NPC_GermanSoldier, CNPC_GermanSoldier )
	RecvPropVector( RECVINFO( m_vecHitDir ) ),
	RecvPropInt( RECVINFO( m_nHitGroup ) ),
	RecvPropTime( RECVINFO( m_flHitTime ) ),
	RecvPropFloat( RECVINFO( m_flHitStrength ) ),
END_RECV_TABLE()

C_NPC_GermanSoldier::C_NPC_GermanSoldier()
{
	m_vecHitDir.Init( 1, 0, 0 );
	m_nHitGroup = 0;
	m_flHitTime = 0.0f;
	m_flHitStrength = 0.0f;
	m_flLHandWeight = 0.0f;
	m_flLHandTime = 0.0f;
}

C_ClientRagdoll *C_NPC_GermanSoldier::CreateClientRagdoll( bool bRestoring )
{
	C_GermanRagdoll *pRagdoll = new C_GermanRagdoll( bRestoring );
	pRagdoll->SetNextClientThink( CLIENT_THINK_ALWAYS );
	return pRagdoll;
}

void C_NPC_GermanSoldier::OnDataChanged( DataUpdateType_t updateType )
{
	BaseClass::OnDataChanged( updateType );

	// Lengthen DoD's full-body fade-ins once per loaded model (the sequence data is
	// shared by every soldier using it). Gestures (delta) are left alone.
	CStudioHdr *pHdr = GetModelPtr();
	if ( !pHdr )
		return;
	static CUtlVector<const void *> s_Done;		// models already adjusted
	const void *pKey = pHdr->GetRenderHdr();
	if ( s_Done.Find( pKey ) != s_Done.InvalidIndex() )
		return;
	s_Done.AddToTail( pKey );

	float flBlend = cl_npc_german_blend.GetFloat();
	for ( int i = 0; i < pHdr->GetNumSeq(); i++ )
	{
		mstudioseqdesc_t &seq = pHdr->pSeqdesc( i );
		if ( seq.flags & STUDIO_DELTA )
			continue;
		if ( seq.fadeintime < flBlend )
			seq.fadeintime = flBlend;
		if ( seq.fadeouttime < flBlend )
			seq.fadeouttime = flBlend;
	}
}

// Rotate a bone and everything below it about a world-space axis through the bone.
void C_NPC_GermanSoldier::RotateSubtree( CStudioHdr *pStudioHdr, int iBone, const Vector &vecAxis, float flDegrees, int iSkip )
{
	if ( iBone < 0 || fabsf( flDegrees ) < 0.01f )
		return;

	matrix3x4_t rot;
	MatrixBuildRotationAboutAxis( vecAxis, flDegrees, rot );

	Vector vecPivot;
	MatrixGetColumn( GetBoneForWrite( iBone ), 3, vecPivot );

	int nBones = pStudioHdr->numbones();
	for ( int i = iBone; i < nBones; i++ )
	{
		// i is in the subtree if walking up its parents reaches iBone
		// (bones are stored parents-first, so the subtree lies at or after iBone).
		int p = i;
		bool bSkipped = false;
		while ( p > iBone )
		{
			if ( p == iSkip )
				bSkipped = true;
			p = pStudioHdr->boneParent( p );
		}
		if ( p != iBone || bSkipped )
			continue;	// not below iBone, or below the bone that must stay put

		matrix3x4_t &m = GetBoneForWrite( i );
		Vector vecOrigin, vecNewOrigin;
		MatrixGetColumn( m, 3, vecOrigin );

		matrix3x4_t out;
		ConcatTransforms( rot, m, out );
		VectorRotate( vecOrigin - vecPivot, rot, vecNewOrigin );
		MatrixSetColumn( vecNewOrigin + vecPivot, 3, out );
		m = out;
	}
}

void C_NPC_GermanSoldier::BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed )
{
	BaseClass::BuildTransformations( pStudioHdr, pos, q, cameraTransform, boneMask, boneComputed );
	if ( !pStudioHdr )
		return;

	ApplyHitKick( pStudioHdr );		// moves the torso; arms and gun ride along
	SupportHandIK( pStudioHdr );	// then the support hand onto the gun
}

void C_NPC_GermanSoldier::RotateSubtreeTo( CStudioHdr *pStudioHdr, int iBone, const Vector &vecFrom, const Vector &vecTo, int iSkip )
{
	Vector a = vecFrom, b = vecTo;
	if ( VectorNormalize( a ) < 0.001f || VectorNormalize( b ) < 0.001f )
		return;
	Vector vecAxis = CrossProduct( a, b );
	if ( VectorNormalize( vecAxis ) < 1e-6f )
		return;
	float flDeg = RAD2DEG( acosf( clamp( DotProduct( a, b ), -1.0f, 1.0f ) ) );
	RotateSubtree( pStudioHdr, iBone, vecAxis, flDeg, iSkip );
}

bool C_NPC_GermanSoldier::IsCoDSequence( CStudioHdr *pStudioHdr, int iSeq )
{
	int n = pStudioHdr->GetNumSeq();
	if ( iSeq < 0 || iSeq >= n )
		return false;
	if ( m_SeqIsCoD.Count() != n )
	{
		m_SeqIsCoD.SetCount( n );
		for ( int i = 0; i < n; i++ )
			m_SeqIsCoD[i] = -1;
	}
	if ( m_SeqIsCoD[iSeq] < 0 )
	{
		const char *pszLabel = pStudioHdr->pSeqdesc( iSeq ).pszLabel();
		m_SeqIsCoD[iSeq] = ( !Q_strnicmp( pszLabel, "ww2_", 4 ) || !Q_strnicmp( pszLabel, "c1_", 3 ) ) ? 1 : 0;
	}
	return m_SeqIsCoD[iSeq] == 1;
}

// How much of the current pose comes from CoD animations (base sequence or overlays).
float C_NPC_GermanSoldier::CoDWeight( CStudioHdr *pStudioHdr )
{
	float w = IsCoDSequence( pStudioHdr, GetSequence() ) ? 1.0f : 0.0f;
	for ( int i = 0; i < m_AnimOverlay.Count() && w < 1.0f; i++ )
	{
		C_AnimationLayer &layer = m_AnimOverlay[i];
		if ( layer.m_flWeight > 0.0f && IsCoDSequence( pStudioHdr, layer.m_nSequence ) )
			w = MAX( w, MIN( (float)layer.m_flWeight, 1.0f ) );
	}
	return w;
}

void C_NPC_GermanSoldier::SupportHandIK( CStudioHdr *pStudioHdr )
{
	// Smooth the CoD share so a full-body CoD animation handing back to DoD doesn't pop.
	float flTarget = CoDWeight( pStudioHdr );
	float dt = clamp( gpGlobals->curtime - m_flLHandTime, 0.0f, 0.1f );
	m_flLHandTime = gpGlobals->curtime;
	m_flLHandWeight = Approach( flTarget, m_flLHandWeight, dt * 4.0f );
	if ( flTarget > m_flLHandWeight )
		m_flLHandWeight = flTarget;		// going in, the overlay's own fade-in is smooth already

	float flW = m_flLHandWeight * clamp( cl_npc_german_lhand_ik.GetFloat(), 0.0f, 1.0f );
	if ( flW <= 0.001f )
		return;

	// Which gun bone carries his weapon, and DoD's grip for it.
	C_BaseCombatWeapon *pWeapon = GetActiveWeapon();
	CStudioHdr *pWeaponHdr = pWeapon ? pWeapon->GetModelPtr() : NULL;
	if ( !pWeaponHdr )
		return;
	const matrix3x4_t *pGrip = NULL;
	int iGun = -1;
	if ( Studio_BoneIndexByName( pWeaponHdr, "ValveBiped.weapon_bone_left" ) >= 0 )
	{
		pGrip = &s_GripLeftHung;
		iGun = Bone( pStudioHdr, "ValveBiped.weapon_bone_left" );
	}
	else if ( Studio_BoneIndexByName( pWeaponHdr, "ValveBiped.weapon_bone" ) >= 0 )
	{
		pGrip = &s_GripRightHung;
		iGun = Bone( pStudioHdr, "ValveBiped.weapon_bone" );
	}
	const int iUpper = Bone( pStudioHdr, "ValveBiped.Bip01_L_UpperArm" );
	const int iFore  = Bone( pStudioHdr, "ValveBiped.Bip01_L_Forearm" );
	const int iHand  = Bone( pStudioHdr, "ValveBiped.Bip01_L_Hand" );
	if ( !pGrip || iGun < 0 || iUpper < 0 || iFore < 0 || iHand < 0 )
		return;

	matrix3x4_t matTarget;
	ConcatTransforms( GetBone( iGun ), *pGrip, matTarget );

	Vector S, E, H, T;
	MatrixGetColumn( GetBone( iUpper ), 3, S );
	MatrixGetColumn( GetBone( iFore ), 3, E );
	MatrixGetColumn( GetBone( iHand ), 3, H );
	MatrixGetColumn( matTarget, 3, T );

	// Only while the animation has the hand on (or near) the gun.
	float flNear = clamp( ( LHAND_FREE_DIST - ( T - H ).Length() ) / ( LHAND_FREE_DIST - LHAND_FULL_DIST ), 0.0f, 1.0f );
	flW *= flNear * flNear * ( 3.0f - 2.0f * flNear );
	if ( flW <= 0.001f )
		return;

	// Two-bone solve, elbow kept in the plane the animation put it in.
	Vector vecGoal = H + ( T - H ) * flW;
	float a = ( E - S ).Length(), b = ( H - E ).Length();
	Vector vecToGoal = vecGoal - S;
	float c = vecToGoal.Length();
	if ( a < 0.01f || b < 0.01f || c < 0.01f )
		return;
	Vector vecDir = vecToGoal / c;
	c = clamp( c, fabsf( a - b ) + 0.01f, a + b - 0.01f );
	Vector vecPole = ( E - S ) - vecDir * DotProduct( E - S, vecDir );
	if ( VectorNormalize( vecPole ) < 0.001f )
		return;
	float flCos = clamp( ( a * a + c * c - b * b ) / ( 2.0f * a * c ), -1.0f, 1.0f );
	Vector vecElbow = S + vecDir * ( a * flCos ) + vecPole * ( a * sqrtf( 1.0f - flCos * flCos ) );

	RotateSubtreeTo( pStudioHdr, iUpper, E - S, vecElbow - S, iGun );		// the gun stays where it is
	Vector H1;
	MatrixGetColumn( GetBone( iHand ), 3, H1 );
	RotateSubtreeTo( pStudioHdr, iFore, H1 - vecElbow, ( S + vecDir * c ) - vecElbow, iGun );

	// Hand orientation: DoD's grip.
	Quaternion qCur, qWant, qNew, qInv, qDelta;
	MatrixQuaternion( GetBone( iHand ), qCur );
	MatrixQuaternion( matTarget, qWant );
	QuaternionSlerp( qCur, qWant, flW, qNew );
	QuaternionConjugate( qCur, qInv );
	QuaternionMult( qNew, qInv, qDelta );
	Vector vecAxis;
	float flDeg;
	QuaternionAxisAngle( qDelta, vecAxis, flDeg );
	if ( vecAxis.LengthSqr() > 1e-8f )
	{
		VectorNormalize( vecAxis );
		RotateSubtree( pStudioHdr, iHand, vecAxis, flDeg, iGun );
	}
}

void C_NPC_GermanSoldier::ApplyHitKick( CStudioHdr *pStudioHdr )
{
	if ( m_flHitTime <= 0.0f || cl_npc_german_hitkick.GetFloat() <= 0.0f )
		return;

	float t = gpGlobals->curtime - m_flHitTime;
	if ( t < 0.0f || t > HITKICK_DURATION )
		return;

	// Fast rise, then a damped spring back with a little overshoot.
	float flEnvelope = ( t < HITKICK_RISE ) ? ( t / HITKICK_RISE )
										   : expf( -( t - HITKICK_RISE ) * 7.0f ) * cosf( ( t - HITKICK_RISE ) * 9.0f );
	float flDeg = HITKICK_MAX_DEG * m_flHitStrength * cl_npc_german_hitkick.GetFloat() * flEnvelope *
				  ( cl_npc_german_hitkick_dir.GetFloat() < 0.0f ? -1.0f : 1.0f );

	Vector vecUp( 0, 0, 1 );
	Vector vecShot = m_vecHitDir;					// direction the bullet travelled
	Vector vecBendAway = CrossProduct( vecUp, vecShot );	// rotating +deg about this tips the top along the shot
	VectorNormalize( vecBendAway );

	Vector vecForward;
	AngleVectors( GetAbsAngles(), &vecForward );
	vecForward.z = 0.0f;
	VectorNormalize( vecForward );
	Vector vecDoubleOver = CrossProduct( vecUp, vecForward );	// +deg folds him forward

	// Which side of him was hit (+ = his left)?
	Vector vecRight = CrossProduct( vecForward, vecUp );
	float flSide = -DotProduct( vecShot, vecRight );		// shot travelling to his left came from his right

	const int iSpine  = Bone( pStudioHdr, "ValveBiped.Bip01_Spine" );
	const int iSpine1 = Bone( pStudioHdr, "ValveBiped.Bip01_Spine1" );
	const int iSpine2 = Bone( pStudioHdr, "ValveBiped.Bip01_Spine2" );
	const int iSpine4 = Bone( pStudioHdr, "ValveBiped.Bip01_Spine4" );
	const int iNeck   = Bone( pStudioHdr, "ValveBiped.Bip01_Neck1" );
	const int iHead   = Bone( pStudioHdr, "ValveBiped.Bip01_Head1" );

	// Apply parents before children so each rotation carries the ones above it.
	switch ( m_nHitGroup )
	{
	case HITGROUP_HEAD:
		RotateSubtree( pStudioHdr, iSpine4, vecBendAway, flDeg * 0.25f );
		RotateSubtree( pStudioHdr, iNeck,   vecBendAway, flDeg * 0.6f );
		RotateSubtree( pStudioHdr, iHead,   vecBendAway, flDeg * 1.4f );
		break;

	case HITGROUP_STOMACH:
		RotateSubtree( pStudioHdr, iSpine,  vecDoubleOver, flDeg * 0.5f );
		RotateSubtree( pStudioHdr, iSpine1, vecDoubleOver, flDeg * 0.5f );
		RotateSubtree( pStudioHdr, iSpine2, vecBendAway,   flDeg * 0.3f );
		RotateSubtree( pStudioHdr, iHead,   vecDoubleOver, flDeg * 0.3f );
		break;

	case HITGROUP_LEFTARM:
	case HITGROUP_RIGHTARM:
		{
			// Shoulder hit: the torso twists away from the hit side.
			float flTwist = ( m_nHitGroup == HITGROUP_LEFTARM ? -1.0f : 1.0f ) * flDeg;
			RotateSubtree( pStudioHdr, iSpine2, vecUp,       flTwist * 0.6f );
			RotateSubtree( pStudioHdr, iSpine4, vecUp,       flTwist * 0.6f );
			RotateSubtree( pStudioHdr, iSpine4, vecBendAway, flDeg * 0.3f );
		}
		break;

	case HITGROUP_LEFTLEG:
	case HITGROUP_RIGHTLEG:
		// Leg hit: a dip and lean toward the hurt side, legs left alone.
		RotateSubtree( pStudioHdr, iSpine,  vecDoubleOver, flDeg * 0.4f );
		RotateSubtree( pStudioHdr, iSpine1, vecForward, ( m_nHitGroup == HITGROUP_LEFTLEG ? -1.0f : 1.0f ) * flDeg * 0.4f );
		break;

	default:	// chest / generic
		RotateSubtree( pStudioHdr, iSpine1, vecBendAway, flDeg * 0.35f );
		RotateSubtree( pStudioHdr, iSpine2, vecBendAway, flDeg * 0.35f );
		RotateSubtree( pStudioHdr, iSpine4, vecBendAway, flDeg * 0.35f );
		RotateSubtree( pStudioHdr, iSpine4, vecUp, flSide * flDeg * 0.3f );
		RotateSubtree( pStudioHdr, iHead,   vecBendAway, flDeg * 0.4f );
		break;
	}
}
