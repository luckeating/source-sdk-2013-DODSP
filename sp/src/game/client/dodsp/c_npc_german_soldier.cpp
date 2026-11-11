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
//=============================================================================//

#include "cbase.h"
#include "c_ai_basenpc.h"
#include "bone_setup.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar cl_npc_german_hitkick( "cl_npc_german_hitkick", "1", FCVAR_ARCHIVE, "Scale of the German soldiers' procedural hit reactions (0 = off)." );
ConVar cl_npc_german_hitkick_dir( "cl_npc_german_hitkick_dir", "1", FCVAR_ARCHIVE, "1 = hits push the body away from the shot, -1 = reverse (if the reactions look backwards)." );

#define HITKICK_RISE		0.06f	// seconds to full deflection
#define HITKICK_DURATION	0.75f	// total length of the reaction
#define HITKICK_MAX_DEG		16.0f	// deflection at strength 1

class C_NPC_GermanSoldier : public C_AI_BaseNPC
{
	DECLARE_CLASS( C_NPC_GermanSoldier, C_AI_BaseNPC );
public:
	DECLARE_CLIENTCLASS();

	C_NPC_GermanSoldier();

	virtual void BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed );

private:
	void	RotateSubtree( CStudioHdr *pStudioHdr, int iBone, const Vector &vecAxis, float flDegrees );
	int		Bone( CStudioHdr *pStudioHdr, const char *pszName ) { return Studio_BoneIndexByName( pStudioHdr, pszName ); }

	Vector	m_vecHitDir;
	int		m_nHitGroup;
	float	m_flHitTime;
	float	m_flHitStrength;

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
}

// Rotate a bone and everything below it about a world-space axis through the bone.
void C_NPC_GermanSoldier::RotateSubtree( CStudioHdr *pStudioHdr, int iBone, const Vector &vecAxis, float flDegrees )
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
		while ( p > iBone )
			p = pStudioHdr->boneParent( p );
		if ( p != iBone )
			continue;

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

	if ( !pStudioHdr || m_flHitTime <= 0.0f || cl_npc_german_hitkick.GetFloat() <= 0.0f )
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
