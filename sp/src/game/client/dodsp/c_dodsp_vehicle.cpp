//====== DOD-SP =====================================================================
//
// Client side of dodsp_vehicle (stage V1): the road wheels turn and the tracks scroll at the speed
// the server drives.
//
// Wheels: every bone with "wheel" in its name turns about the axle (the model's up x forward) through
// its own centre, by distance / radius - the radius is the bone's height above the vehicle's floor.
// Done after the animation has built the skeleton (BuildTransformations), so any model works without
// an animation of its own.
//
// Tracks: material proxy "DoDSPTrackScroll" - add to the tread material (the track texture is one
// strip; "axis" says which way it runs, "scale" texture repeats per world unit):
//     "Proxies"
//     {
//         "DoDSPTrackScroll"
//         {
//             "textureScrollVar"	"$basetexturetransform"
//             "axis"				"v"
//             "scale"				"0.01"
//         }
//     }
// The distance driven is one owner (m_flDistance) for both, so wheels and tracks never disagree.
//
//==================================================================================
#include "cbase.h"
#include "c_baseanimating.h"
#include "proxyentity.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "toolframework_client.h"
#include "mathlib/vmatrix.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

void ToolFramework_RecordMaterialParams( IMaterial *pMaterial );

class C_DoDSPVehicle : public C_BaseAnimating
{
public:
	DECLARE_CLASS( C_DoDSPVehicle, C_BaseAnimating );
	DECLARE_CLIENTCLASS();

	C_DoDSPVehicle();
	virtual void	BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed );
	float			GetDistance( void );

private:
	float			m_flTrackSpeed;
	float			m_flModelForwardYaw;
	float			m_flDistance;			// driven so far (units, signed)
	float			m_flDistanceTime;
	CUtlVector<int>	m_WheelBones;
	CStudioHdr		*m_pWheelHdr;			// the model the wheel list was made for
};

IMPLEMENT_CLIENTCLASS_DT( C_DoDSPVehicle, DT_DoDSPVehicle, CDoDSPVehicle )
	RecvPropFloat( RECVINFO( m_flTrackSpeed ) ),
	RecvPropFloat( RECVINFO( m_flModelForwardYaw ) ),
END_RECV_TABLE()

C_DoDSPVehicle::C_DoDSPVehicle()
{
	m_flTrackSpeed = 0.0f;
	m_flModelForwardYaw = -90.0f;
	m_flDistance = 0.0f;
	m_flDistanceTime = 0.0f;
	m_pWheelHdr = NULL;
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

void C_DoDSPVehicle::BuildTransformations( CStudioHdr *pStudioHdr, Vector *pos, Quaternion q[], const matrix3x4_t &cameraTransform, int boneMask, CBoneBitList &boneComputed )
{
	BaseClass::BuildTransformations( pStudioHdr, pos, q, cameraTransform, boneMask, boneComputed );
	if ( !pStudioHdr )
		return;
	if ( m_pWheelHdr != pStudioHdr )
	{
		m_pWheelHdr = pStudioHdr;
		m_WheelBones.RemoveAll();
		for ( int i = 0; i < pStudioHdr->numbones(); i++ )
			if ( Q_stristr( pStudioHdr->pBone( i )->pszName(), "wheel" ) )
				m_WheelBones.AddToTail( i );
	}
	if ( !m_WheelBones.Count() )
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

	for ( int k = 0; k < m_WheelBones.Count(); k++ )
	{
		int iBone = m_WheelBones[k];
		if ( !( pStudioHdr->boneFlags( iBone ) & boneMask ) )
			continue;
		matrix3x4_t &bone = GetBoneForWrite( iBone );
		Vector vecCentre;
		MatrixGetColumn( bone, 3, vecCentre );
		float flRadius = clamp( DotProduct( vecCentre - vecOrigin, vecUp ), 6.0f, 60.0f );
		float flDeg = RAD2DEG( flDist / flRadius );
		matrix3x4_t rot, moved, out;
		MatrixBuildRotationAboutAxis( vecAxle, flDeg, rot );
		moved = bone;
		MatrixSetColumn( vec3_origin, 3, moved );		// turn about the wheel's own centre
		ConcatTransforms( rot, moved, out );
		MatrixSetColumn( vecCentre, 3, out );
		bone = out;
	}
}

//-----------------------------------------------------------------------------
// DoDSPTrackScroll
//-----------------------------------------------------------------------------
class CDoDSPTrackScrollProxy : public CEntityMaterialProxy
{
public:
	CDoDSPTrackScrollProxy() : m_pScrollVar( NULL ), m_flScale( 0.01f ), m_bAxisU( false ) {}
	virtual bool Init( IMaterial *pMaterial, KeyValues *pKeyValues )
	{
		bool bFound;
		m_pScrollVar = pMaterial->FindVar( pKeyValues->GetString( "textureScrollVar", "$basetexturetransform" ), &bFound, false );
		m_flScale = pKeyValues->GetFloat( "scale", 0.01f );
		m_bAxisU = !Q_stricmp( pKeyValues->GetString( "axis", "v" ), "u" );
		return bFound;
	}
	virtual void OnBind( C_BaseEntity *pEntity )
	{
		if ( !m_pScrollVar || !pEntity )
			return;
		C_DoDSPVehicle *pVehicle = dynamic_cast<C_DoDSPVehicle *>( pEntity );
		if ( !pVehicle && pEntity->GetMoveParent() )
			pVehicle = dynamic_cast<C_DoDSPVehicle *>( pEntity->GetMoveParent() );
		float flOffset = pVehicle ? pVehicle->GetDistance() * m_flScale : 0.0f;
		flOffset -= floorf( flOffset );
		if ( m_pScrollVar->GetType() == MATERIAL_VAR_TYPE_MATRIX )
		{
			VMatrix mat;
			MatrixBuildTranslation( mat, m_bAxisU ? flOffset : 0.0f, m_bAxisU ? 0.0f : flOffset, 0.0f );
			m_pScrollVar->SetMatrixValue( mat );
		}
		else
			m_pScrollVar->SetVecValue( m_bAxisU ? flOffset : 0.0f, m_bAxisU ? 0.0f : flOffset, 0.0f );
		if ( ToolsEnabled() )
			ToolFramework_RecordMaterialParams( GetMaterial() );
	}
	virtual IMaterial *GetMaterial()
	{
		return m_pScrollVar ? m_pScrollVar->GetOwningMaterial() : NULL;
	}
private:
	IMaterialVar	*m_pScrollVar;
	float			m_flScale;
	bool			m_bAxisU;
};

EXPOSE_INTERFACE( CDoDSPTrackScrollProxy, IMaterialProxy, "DoDSPTrackScroll" IMATERIAL_PROXY_INTERFACE_VERSION );
