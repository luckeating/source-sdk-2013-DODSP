//===================== DOD-SP sun cascades (lighting stage 2) =====================//
//
// One owner for the sun's shadow maps. Three orthographic depth maps looking down the sun:
//   near - around the player, every frame      (dodsp_csm_size_near, half-width in units)
//   mid  - wider, every frame                  (dodsp_csm_size_mid)
//   far  - the whole map, every few seconds    (dodsp_csm_far_interval)
// Nothing reads them yet: stage 3 turns them into a screen-space shadow mask.
//
// The sun comes from the map's light_environment (the world lights VRAD wrote): Hammer owns its
// direction. No env_global_light, no flashlight state, no engine flashlight passes - the stock
// flashlight/projected-texture system is untouched.
//
// INVARIANT: a cascade is only published (DoDSP_GetSunCascade) after it rendered with the sun
// direction and bounds it publishes. Every cascade's centre snaps to its own texel grid so the
// map does not crawl as the player moves.
//
// Render targets carry DOD-SP-only names (_rt_DODSP_*) so they can never collide with Valve's or
// Mapbase's.
//
// Cvars: dodsp_csm (1 = render), dodsp_csm_debug (1 = coverage boxes + stats), dodsp_csm_res,
// dodsp_csm_size_near/mid, dodsp_csm_far_interval, dodsp_csm_slopebias, dodsp_csm_bias.
// Command: dodsp_csm_report.
//
//===================================================================================//

#include "cbase.h"
#include "c_dodsp_sun_cascades.h"
#include "igamesystem.h"
#include "view_shared.h"
#include "iviewrender.h"
#include "c_world.h"
#include "worldlight.h"
#include "engine/ivdebugoverlay.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "mathlib/vmatrix.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_csm( "dodsp_csm", "1", 0, "DOD-SP: render the sun's cascaded shadow maps (stage 2: nothing uses them yet)" );
static ConVar dodsp_csm_debug( "dodsp_csm_debug", "0", 0, "DOD-SP debug: 1 = draw each sun cascade's coverage box and stats" );
static ConVar dodsp_csm_res( "dodsp_csm_res", "2048", 0, "DOD-SP: sun shadow map size in texels (applies on the next map load)" );
static ConVar dodsp_csm_size_near( "dodsp_csm_size_near", "256", 0, "DOD-SP: half-width of the near sun cascade, in world units" );
static ConVar dodsp_csm_size_mid( "dodsp_csm_size_mid", "1024", 0, "DOD-SP: half-width of the mid sun cascade, in world units" );
static ConVar dodsp_csm_far_interval( "dodsp_csm_far_interval", "2", 0, "DOD-SP: seconds between redraws of the whole-map sun cascade" );
static ConVar dodsp_csm_slopebias( "dodsp_csm_slopebias", "4", 0, "DOD-SP: slope-scaled depth bias when drawing the sun cascades" );
static ConVar dodsp_csm_bias( "dodsp_csm_bias", "0.00001", 0, "DOD-SP: constant depth bias when drawing the sun cascades" );

namespace
{
	const char *s_pszCascadeNames[DODSP_CASCADE_COUNT] = { "near", "mid", "far" };

	struct Cascade_t
	{
		CTextureReference	m_Depth;
		bool				m_bValid;			// rendered at least once with the current sun and map
		float				m_flHalfSize;		// half-width, world units
		float				m_flTexel;			// world units per texel
		Vector				m_vecOrigin;		// view origin (on the sun side of the whole map)
		QAngle				m_angView;
		float				m_flNear, m_flFar;
		float				m_flLastRender;		// gpGlobals->curtime
		float				m_flCostMs;			// CPU time of the last render call
		int					m_nRenders;
		VMatrix				m_matWorldToTexture;
	};
}

class CDoDSPSunCascades : public CAutoGameSystem
{
public:
	CDoDSPSunCascades() : CAutoGameSystem( "CDoDSPSunCascades" ), m_bAllocated( false ), m_nRes( 0 ), m_bHaveSun( false ), m_nLastFrame( -1 ) {}

	virtual void LevelInitPostEntity()	{ Reset(); EnsureTargets(); FindSun(); }
	virtual void LevelShutdownPreEntity() { Reset(); m_bHaveSun = false; }
	virtual void Shutdown()				{ FreeTargets(); }

	void Render( const CViewSetup &mainView );
	bool Get( int n, ITexture **ppDepth, VMatrix *pMat );
	void Report() const;

private:
	void Reset()
	{
		for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
		{
			m_Cascades[i].m_bValid = false;
			m_Cascades[i].m_flLastRender = -1.0f;
			m_Cascades[i].m_nRenders = 0;
			m_Cascades[i].m_flCostMs = 0.0f;
		}
	}
	void EnsureTargets();
	void FreeTargets();
	void FindSun();
	bool SetupCascade( Cascade_t &c, float flHalfSize, const Vector2D &vecCentreLS );
	void DrawDebug( const CViewSetup &mainView );

	bool				m_bAllocated;
	int					m_nRes;
	CTextureReference	m_DummyColor;
	Cascade_t			m_Cascades[DODSP_CASCADE_COUNT];

	bool				m_bHaveSun;
	Vector				m_vecSunDir;			// the way the light travels (from the world lights)
	Vector				m_vecRight, m_vecUp;	// light-space axes
	QAngle				m_angSun;
	float				m_flWorldMinF, m_flWorldMaxF;	// world bounds along the sun
	Vector2D			m_vecWorldMinLS, m_vecWorldMaxLS;	// world bounds across the sun
	int					m_nLastFrame;
};

static CDoDSPSunCascades s_DoDSPSunCascades;

void CDoDSPSunCascades::EnsureTargets()
{
	int nRes = clamp( dodsp_csm_res.GetInt(), 256, 4096 );
	if ( m_bAllocated && nRes == m_nRes )
		return;
	FreeTargets();

	ImageFormat depthFormat = materials->GetShadowDepthTextureFormat();
	ImageFormat nullFormat = materials->GetNullTextureFormat();
	materials->BeginRenderTargetAllocation();
	m_DummyColor.InitRenderTarget( nRes, nRes, RT_SIZE_NO_CHANGE, nullFormat, MATERIAL_RT_DEPTH_NONE, false, "_rt_DODSP_SunCascadeDummy" );
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		char szName[64];
		Q_snprintf( szName, sizeof( szName ), "_rt_DODSP_SunCascade%d", i );
		m_Cascades[i].m_Depth.InitRenderTarget( nRes, nRes, RT_SIZE_NO_CHANGE, depthFormat, MATERIAL_RT_DEPTH_NONE, false, szName );
	}
	materials->EndRenderTargetAllocation();

	m_nRes = m_Cascades[0].m_Depth->GetActualWidth();
	m_bAllocated = true;
	if ( m_nRes != nRes )
		Warning( "DOD-SP sun cascades: asked for %d texels, the card gave %d\n", nRes, m_nRes );
	Reset();
}

void CDoDSPSunCascades::FreeTargets()
{
	if ( !m_bAllocated )
		return;
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
		m_Cascades[i].m_Depth.Shutdown();
	m_DummyColor.Shutdown();
	m_bAllocated = false;
	Reset();
}

void CDoDSPSunCascades::FindSun()
{
	m_bHaveSun = g_pWorldLights && g_pWorldLights->GetSunDirection( m_vecSunDir ) && m_vecSunDir.LengthSqr() > 0.5f;
	C_World *pWorld = GetClientWorldEntity();
	if ( !m_bHaveSun || !pWorld )
	{
		m_bHaveSun = false;
		Msg( "DOD-SP sun cascades: no sun in this map (no light_environment) - nothing to render\n" );
		return;
	}
	m_vecSunDir.NormalizeInPlace();
	VectorAngles( m_vecSunDir, m_angSun );
	AngleVectors( m_angSun, NULL, &m_vecRight, &m_vecUp );

	// The world's bounds in light space: how far the maps must reach along and across the sun
	Vector vecMins = pWorld->m_WorldMins, vecMaxs = pWorld->m_WorldMaxs;
	m_flWorldMinF = FLT_MAX; m_flWorldMaxF = -FLT_MAX;
	m_vecWorldMinLS.Init( FLT_MAX, FLT_MAX ); m_vecWorldMaxLS.Init( -FLT_MAX, -FLT_MAX );
	for ( int i = 0; i < 8; i++ )
	{
		Vector p( ( i & 1 ) ? vecMaxs.x : vecMins.x, ( i & 2 ) ? vecMaxs.y : vecMins.y, ( i & 4 ) ? vecMaxs.z : vecMins.z );
		float f = DotProduct( p, m_vecSunDir ), r = DotProduct( p, m_vecRight ), u = DotProduct( p, m_vecUp );
		m_flWorldMinF = MIN( m_flWorldMinF, f ); m_flWorldMaxF = MAX( m_flWorldMaxF, f );
		m_vecWorldMinLS.x = MIN( m_vecWorldMinLS.x, r ); m_vecWorldMaxLS.x = MAX( m_vecWorldMaxLS.x, r );
		m_vecWorldMinLS.y = MIN( m_vecWorldMinLS.y, u ); m_vecWorldMaxLS.y = MAX( m_vecWorldMaxLS.y, u );
	}
	Msg( "DOD-SP sun cascades: sun %.2f %.2f %.2f, map %.0f units deep along it\n",
		 m_vecSunDir.x, m_vecSunDir.y, m_vecSunDir.z, m_flWorldMaxF - m_flWorldMinF );
}

// Places one cascade: centred on vecCentreLS (light space), snapped to its own texel grid, and
// reaching from the sun side of the whole map to its far side so every caster is inside.
bool CDoDSPSunCascades::SetupCascade( Cascade_t &c, float flHalfSize, const Vector2D &vecCentreLS )
{
	c.m_flHalfSize = flHalfSize;
	c.m_flTexel = ( 2.0f * flHalfSize ) / m_nRes;
	float r = floorf( vecCentreLS.x / c.m_flTexel + 0.5f ) * c.m_flTexel;
	float u = floorf( vecCentreLS.y / c.m_flTexel + 0.5f ) * c.m_flTexel;

	const float flMargin = 64.0f;
	float flStartF = m_flWorldMinF - flMargin;
	c.m_vecOrigin = m_vecRight * r + m_vecUp * u + m_vecSunDir * flStartF;
	c.m_angView = m_angSun;
	c.m_flNear = 1.0f;
	c.m_flFar = ( m_flWorldMaxF + flMargin ) - flStartF;

	// world -> texture: x, y across the map 0..1, z = depth 0..1 between near and far.
	// The x/y orientation must match how the engine rasterises the ortho view; stage 3's debug
	// view checks it against the scene before anything reads it.
	float flInvSize = 1.0f / ( 2.0f * flHalfSize ), flInvDepth = 1.0f / ( c.m_flFar - c.m_flNear );
	Vector vecR = m_vecRight * flInvSize, vecU = m_vecUp * -flInvSize, vecF = m_vecSunDir * flInvDepth;
	c.m_matWorldToTexture.Init(
		vecR.x, vecR.y, vecR.z, 0.5f - DotProduct( c.m_vecOrigin, vecR ),
		vecU.x, vecU.y, vecU.z, 0.5f - DotProduct( c.m_vecOrigin, vecU ),
		vecF.x, vecF.y, vecF.z, -DotProduct( c.m_vecOrigin, vecF ) - c.m_flNear * flInvDepth,
		0.0f, 0.0f, 0.0f, 1.0f );
	return true;
}

void CDoDSPSunCascades::Render( const CViewSetup &mainView )
{
	if ( m_nLastFrame == gpGlobals->framecount )
		return;	// one render per frame, whichever view path calls first
	m_nLastFrame = gpGlobals->framecount;

	if ( !dodsp_csm.GetBool() || !m_bHaveSun )
		return;
	EnsureTargets();
	if ( !m_bAllocated )
		return;

	Vector2D vecEyeLS( DotProduct( mainView.origin, m_vecRight ), DotProduct( mainView.origin, m_vecUp ) );
	Vector2D vecMapCentreLS = ( m_vecWorldMinLS + m_vecWorldMaxLS ) * 0.5f;
	float flMapHalf = 0.5f * MAX( m_vecWorldMaxLS.x - m_vecWorldMinLS.x, m_vecWorldMaxLS.y - m_vecWorldMinLS.y ) + 64.0f;

	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->SetShadowDepthBiasFactors( dodsp_csm_slopebias.GetFloat(), dodsp_csm_bias.GetFloat() );
	pRenderContext.SafeRelease();

	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		Cascade_t &c = m_Cascades[i];
		if ( i == DODSP_CASCADE_FAR )
		{
			if ( c.m_bValid && gpGlobals->curtime - c.m_flLastRender < MAX( dodsp_csm_far_interval.GetFloat(), 0.0f ) )
				continue;
			SetupCascade( c, flMapHalf, vecMapCentreLS );
		}
		else
		{
			float flHalf = ( i == DODSP_CASCADE_NEAR ) ? dodsp_csm_size_near.GetFloat() : dodsp_csm_size_mid.GetFloat();
			SetupCascade( c, MAX( flHalf, 16.0f ), vecEyeLS );
		}

		CViewSetup shadowView;
		shadowView.x = shadowView.y = 0;
		shadowView.width = shadowView.height = m_nRes;
		shadowView.m_bOrtho = true;
		shadowView.m_OrthoLeft = -c.m_flHalfSize;
		shadowView.m_OrthoTop = -c.m_flHalfSize;
		shadowView.m_OrthoRight = c.m_flHalfSize;
		shadowView.m_OrthoBottom = c.m_flHalfSize;
		shadowView.m_flAspectRatio = 1.0f;
		shadowView.m_bDoBloomAndToneMapping = false;
		shadowView.fov = shadowView.fovViewmodel = 90.0f;
		shadowView.origin = c.m_vecOrigin;
		shadowView.angles = c.m_angView;
		shadowView.zNear = shadowView.zNearViewmodel = c.m_flNear;
		shadowView.zFar = shadowView.zFarViewmodel = c.m_flFar;

		double t0 = Plat_FloatTime();
		view->UpdateShadowDepthTexture( m_DummyColor, c.m_Depth, shadowView );
		c.m_flCostMs = (float)( ( Plat_FloatTime() - t0 ) * 1000.0 );
		c.m_flLastRender = gpGlobals->curtime;
		c.m_nRenders++;
		c.m_bValid = true;
	}

	if ( dodsp_csm_debug.GetBool() )
		DrawDebug( mainView );
}

void CDoDSPSunCascades::DrawDebug( const CViewSetup &mainView )
{
	static const Color s_Edge[DODSP_CASCADE_COUNT] = { Color( 0, 255, 0, 255 ), Color( 255, 200, 0, 255 ), Color( 255, 0, 0, 255 ) };
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		const Cascade_t &c = m_Cascades[i];
		if ( !c.m_bValid )
			continue;
		// The cascade's volume: x along the sun from its origin, y/z across it
		if ( debugoverlay )
		{
			debugoverlay->AddBoxOverlay2( c.m_vecOrigin, Vector( c.m_flNear, -c.m_flHalfSize, -c.m_flHalfSize ),
										  Vector( c.m_flFar, c.m_flHalfSize, c.m_flHalfSize ), c.m_angView,
										  Color( 0, 0, 0, 0 ), s_Edge[i], 0.0f );
		}
		engine->Con_NPrintf( 2 + i, "DOD-SP sun cascade %-4s  %5.0f units wide  %.2f units/texel  %.2f ms  %d renders",
							 s_pszCascadeNames[i], 2.0f * c.m_flHalfSize, c.m_flTexel, c.m_flCostMs, c.m_nRenders );
	}
}

bool CDoDSPSunCascades::Get( int n, ITexture **ppDepth, VMatrix *pMat )
{
	if ( n < 0 || n >= DODSP_CASCADE_COUNT || !m_bAllocated || !m_Cascades[n].m_bValid )
		return false;
	if ( ppDepth )
		*ppDepth = m_Cascades[n].m_Depth;
	if ( pMat )
		*pMat = m_Cascades[n].m_matWorldToTexture;
	return true;
}

void CDoDSPSunCascades::Report() const
{
	Msg( "DOD-SP sun cascades: %s, %d x %d texels per map (%s)\n", dodsp_csm.GetBool() ? "ON" : "off", m_nRes, m_nRes,
		 m_bAllocated ? "allocated" : "not allocated" );
	if ( !m_bHaveSun )
	{
		Msg( "  no sun in this map\n" );
		return;
	}
	Msg( "  sun (from the light_environment) %.2f %.2f %.2f; map %.0f units deep along it\n",
		 m_vecSunDir.x, m_vecSunDir.y, m_vecSunDir.z, m_flWorldMaxF - m_flWorldMinF );
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		const Cascade_t &c = m_Cascades[i];
		Msg( "  %-4s %s  %6.0f units wide, %.2f units/texel, last %.2f ms, %d renders\n", s_pszCascadeNames[i],
			 c.m_bValid ? "ok  " : "none", 2.0f * c.m_flHalfSize, c.m_flTexel, c.m_flCostMs, c.m_nRenders );
	}
}

void DoDSP_RenderSunCascades( const CViewSetup &mainView )
{
	s_DoDSPSunCascades.Render( mainView );
}

bool DoDSP_GetSunCascade( int nCascade, ITexture **ppDepth, VMatrix *pWorldToTexture )
{
	return s_DoDSPSunCascades.Get( nCascade, ppDepth, pWorldToTexture );
}

CON_COMMAND( dodsp_csm_report, "DOD-SP: the sun cascades - size, coverage and cost" )
{
	s_DoDSPSunCascades.Report();
}
