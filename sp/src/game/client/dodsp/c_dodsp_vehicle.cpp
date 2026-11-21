//====== DOD-SP =====================================================================
//
// Client side of dodsp_vehicle: the wheels turn (and steer) and the tracks scroll at the speed the server drives.
// The distance driven is one owner (m_flDistance) for both, so wheels and tracks never disagree.
//
// Wheels (H1): the model says which bones are wheels, how big each is and which steer - $keyvalues "dodsp_wheels",
// measured from the mesh by the rig tool (dodsp/dodsp_vehicle_wheels.h, shared with the server). Each turns about the
// axle (the model's up x forward) through its own centre by distance / its radius; the steered ones are turned first
// about the up axis by the server's steering angle (m_flSteer). A model without the block (the tanks) keeps V1's rule:
// every bone with "wheel" in its name, radius = its height above the vehicle's floor, nothing steers. Done after the
// animation has built the skeleton (BuildTransformations), so any model works without an animation of its own.
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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

void ToolFramework_RecordMaterialParams( IMaterial *pMaterial );

#define DODSP_TREADS_FILE	"scripts/dodsp_treads.txt"

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
	float			GetDistance( void );

private:
	struct Wheel_t { int iBone; float flRadius; bool bSteer; };		// flRadius 0 = the height rule
	void			BuildWheelList( CStudioHdr *pStudioHdr );
	void			CheckTreadProxies( CStudioHdr *pStudioHdr );

	float			m_flTrackSpeed;
	float			m_flModelForwardYaw;
	float			m_flSteer;
	float			m_flDistance;			// driven so far (units, signed)
	float			m_flDistanceTime;
	CUtlVector<Wheel_t>	m_Wheels;
	CStudioHdr		*m_pWheelHdr;			// the model the wheel list was made for
};

IMPLEMENT_CLIENTCLASS_DT( C_DoDSPVehicle, DT_DoDSPVehicle, CDoDSPVehicle )
	RecvPropFloat( RECVINFO( m_flTrackSpeed ) ),
	RecvPropFloat( RECVINFO( m_flModelForwardYaw ) ),
	RecvPropFloat( RECVINFO( m_flSteer ) ),
END_RECV_TABLE()

C_DoDSPVehicle::C_DoDSPVehicle()
{
	m_flTrackSpeed = 0.0f;
	m_flModelForwardYaw = 0.0f;
	m_flSteer = 0.0f;
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

void C_DoDSPVehicle::BuildWheelList( CStudioHdr *pStudioHdr )
{
	m_Wheels.RemoveAll();
	DoDSPWheels_t data;
	if ( DoDSP_ReadWheels( GetModel(), data ) )
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
		}
		return;
	}
	for ( int i = 0; i < pStudioHdr->numbones(); i++ )
		if ( Q_stristr( pStudioHdr->pBone( i )->pszName(), "wheel" ) )
		{
			Wheel_t w = { i, 0.0f, false };
			m_Wheels.AddToTail( w );
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
	if ( !m_Wheels.Count() )
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

	for ( int k = 0; k < m_Wheels.Count(); k++ )
	{
		const Wheel_t &w = m_Wheels[k];
		if ( !( pStudioHdr->boneFlags( w.iBone ) & boneMask ) )
			continue;
		matrix3x4_t &bone = GetBoneForWrite( w.iBone );
		Vector vecCentre;
		MatrixGetColumn( bone, 3, vecCentre );
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
