//===================== DOD-SP sun cascades (lighting stage 2) =====================//
//
// One owner for the sun's shadow maps. Three orthographic depth maps looking down the sun:
//   near - around the player, every frame      (dodsp_csm_size_near, half-width in units)
//   mid  - wider, every frame                  (dodsp_csm_size_mid)
//   far  - the whole map, every few seconds    (dodsp_csm_far_interval)
// Stage 3b-2: every frame this file also publishes the sun to DOD-SP's shaders (DoDSP_PublishSun):
// the cascades, their placement and the sun's colour, as rendering parameters (public/dodsp_shaders.h).
// The sun's colour is only non-zero when the baked direct sun is off (dodsp_sun_baked 0) - one sun
// at a time, never both and never neither:
// INVARIANT (watchdog): dodsp_sun_baked 0 while the dynamic sun can't draw (no DOD-SP sun shaders,
// no shader model 3.0, cascades off) would leave the map with no direct sun at all, so the
// watchdog puts the baked sun back and says why.
// Stage 4d: static props. When VRAD baked a map's static props without the sun (the BSP's level flags,
// dodsp_lightstyles.h), those props get the per-pixel sun in BOTH sun modes (DODSP_SUNMODE_STATICPROPS):
// their vertex lighting can't switch the sun on and off like the lightmaps can.
// INVARIANT (watchdog): such a map while the per-pixel sun can't draw leaves its static props with no
// direct sun at all (nothing to put back - the bake has none), so the watchdog says so and why.
//
// Stage 3b-1: dodsp_csm_view draws them top-right with DOD-SP's DODSP_SunCascadeDebug shader and a
// ring on the point under the crosshair (green = the cascade says sunlit, red = in shadow), which
// checks the world -> texture matrix, the depth range and the shadow compare against the scene.
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
// Stage 4b probe (dodsp_csm_probe_intz 1, read at map load): the soft shadows need the raw depth out
// of a cascade, but on NVIDIA our cascades use a format the card compares in hardware and never
// returns. The probe also draws the near cascade into an INTZ map (NVIDIA's readable depth format)
// and shows it as a 4th panel in dodsp_csm_view, read as raw depth. Same picture and ring as the
// near panel = the 2014 engine can make and read INTZ, and 4b can be built on it. Nothing in the
// game's lighting reads the probe.
//
// Cvars: dodsp_csm (1 = render), dodsp_csm_debug (1 = coverage boxes + stats), dodsp_csm_res,
// dodsp_csm_size_near/mid, dodsp_csm_far_interval, dodsp_csm_slopebias, dodsp_csm_bias,
// dodsp_csm_view (1 = draw the cascades on screen), dodsp_csm_view_size, dodsp_csm_view_bias,
// dodsp_sun_scale, dodsp_sun_filter, dodsp_sun_normaloffset, dodsp_sun_bias (the dynamic sun).
// Command: dodsp_csm_report.
//
//===================================================================================//

#include "cbase.h"
#include "c_dodsp_sun_cascades.h"
#include "igamesystem.h"
#include "view_shared.h"
#include "iviewrender.h"
#include "c_world.h"
#include "c_baseplayer.h"
#include "worldlight.h"
#include "engine/ivdebugoverlay.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "mathlib/vmatrix.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "KeyValues.h"
#include "dodsp_shaders.h"
#include "c_dodsp_shaderdll.h"
#include "c_dodsp_sdkshaders.h"
#include "filesystem.h"
#include "bspfile.h"
#include "dodsp_lightstyles.h"

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
static ConVar dodsp_csm_view( "dodsp_csm_view", "0", 0,
	"DOD-SP debug: 1 = draw the sun cascades top-right (near, mid, far) with a ring on the point you aim at: "
	"green = that cascade says sunlit, red = in shadow. Needs DOD-SP's shader DLL and shader files." );
static ConVar dodsp_csm_view_size( "dodsp_csm_view_size", "256", 0, "DOD-SP debug: on-screen size of each cascade in dodsp_csm_view, pixels" );
static ConVar dodsp_sun_scale( "dodsp_sun_scale", "1", 0, "DOD-SP: brightness of the dynamic sun against the baked one (1 = exactly the light_environment)" );
static ConVar dodsp_sun_filter( "dodsp_sun_filter", "1.5", 0, "DOD-SP: softness of the dynamic sun's shadow edge, in cascade texels" );
static ConVar dodsp_sun_normaloffset( "dodsp_sun_normaloffset", "1.5", 0, "DOD-SP: how far (in cascade texels) a surface is pushed out along its normal before the shadow test - fights shadow acne" );
static ConVar dodsp_sun_bias( "dodsp_sun_bias", "0.0005", 0, "DOD-SP: depth bias of the dynamic sun's shadow test (0..1 of the cascade depth range)" );
static ConVar dodsp_csm_probe_intz( "dodsp_csm_probe_intz", "0", 0,
	"DOD-SP stage 4b probe: 1 = also draw the near sun cascade into an NVIDIA INTZ (readable depth) map, shown as a 4th panel "
	"(left) in dodsp_csm_view. Read at map load." );
static ConVar dodsp_csm_view_bias( "dodsp_csm_view_bias", "0.002", 0, "DOD-SP debug: depth bias (0..1 of the cascade's depth) for the aim-point test in dodsp_csm_view" );

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
	CDoDSPSunCascades() : CAutoGameSystem( "CDoDSPSunCascades" ), m_bAllocated( false ), m_nRes( 0 ), m_bHaveSun( false ), m_nLastFrame( -1 ),
		m_bViewMaterials( false ), m_bViewWarned( false ), m_bSunInvariantWarned( false ),
		m_bPropsNoSun( false ), m_bPropsInvariantWarned( false ),
		m_bProbe( false ), m_pProbeMarker( NULL ), m_pProbeParams( NULL ) {}

	virtual void LevelInitPostEntity()	{ Reset(); EnsureTargets(); FindSun(); ReadPropsNoSun(); EnsureProbe(); }
	virtual void LevelShutdownPreEntity() { Reset(); m_bHaveSun = false; m_bPropsNoSun = false; }
	virtual void Shutdown()				{ FreeProbe(); FreeTargets(); FreeViewMaterials(); }

	void Render( const CViewSetup &mainView );
	void PublishSun();
	void DrawView( const CViewSetup &mainView );
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
	bool EnsureViewMaterials();
	bool CanDrawDynamicSun( const char **ppszWhy ) const;
	void WatchSunInvariant();
	void ReadPropsNoSun();
	void EnsureProbe();
	void FreeProbe();
	void FreeViewMaterials();

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

	// dodsp_csm_view: one material per cascade (queued rendering reads material values late, so
	// the three draws must not share one material)
	CMaterialReference	m_ViewMat[DODSP_CASCADE_COUNT];
	IMaterialVar		*m_pViewMarker[DODSP_CASCADE_COUNT];
	IMaterialVar		*m_pViewParams[DODSP_CASCADE_COUNT];
	bool				m_bViewMaterials;
	bool				m_bViewWarned;
	bool				m_bSunInvariantWarned;

	bool				m_bPropsNoSun;			// this map's static props were baked without the direct sun
	bool				m_bPropsInvariantWarned;

	// stage 4b INTZ probe
	CTextureReference	m_ProbeDepth;
	bool				m_bProbe;
	CMaterialReference	m_ProbeMat;
	IMaterialVar		*m_pProbeMarker;
	IMaterialVar		*m_pProbeParams;
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

// The 4b probe's INTZ map: same size as the cascades, point sampled (raw depth is never filtered)
void CDoDSPSunCascades::EnsureProbe()
{
	const bool bWant = dodsp_csm_probe_intz.GetBool() && m_bAllocated;
	if ( bWant == m_bProbe && ( !bWant || m_ProbeDepth->GetActualWidth() == m_nRes ) )
		return;
	FreeProbe();
	if ( !bWant )
		return;

	materials->BeginRenderTargetAllocation();
	ITexture *pTex = materials->CreateNamedRenderTargetTextureEx2( "_rt_DODSP_SunProbeINTZ", m_nRes, m_nRes, RT_SIZE_NO_CHANGE,
		IMAGE_FORMAT_NV_INTZ, MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_POINTSAMPLE );
	materials->EndRenderTargetAllocation();
	if ( !pTex || pTex->IsError() )
	{
		Warning( "DOD-SP INTZ probe: RESULT FAIL - the engine could not make an INTZ render target\n" );
		return;
	}
	m_ProbeDepth.Init( pTex );
	m_bProbe = true;
	Msg( "DOD-SP INTZ probe: made %dx%d, format %d (INTZ = %d, cascades use %d) - compare the left panel of dodsp_csm_view with the near one\n",
		 m_ProbeDepth->GetActualWidth(), m_ProbeDepth->GetActualHeight(), (int)m_ProbeDepth->GetImageFormat(),
		 (int)IMAGE_FORMAT_NV_INTZ, (int)m_Cascades[0].m_Depth->GetImageFormat() );
}

void CDoDSPSunCascades::FreeProbe()
{
	m_ProbeMat.Shutdown();
	m_pProbeMarker = m_pProbeParams = NULL;
	if ( m_bProbe )
		m_ProbeDepth.Shutdown();
	m_bProbe = false;
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

	WatchSunInvariant();
	if ( !dodsp_csm.GetBool() || !m_bHaveSun )
	{
		PublishSun();	// publishes "no dynamic sun"
		return;
	}
	EnsureTargets();
	if ( !m_bAllocated )
	{
		PublishSun();
		return;
	}

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
		if ( i == DODSP_CASCADE_NEAR && m_bProbe )
			view->UpdateShadowDepthTexture( m_DummyColor, m_ProbeDepth, shadowView );	// 4b probe: same view, INTZ
		c.m_flCostMs = (float)( ( Plat_FloatTime() - t0 ) * 1000.0 );
		c.m_flLastRender = gpGlobals->curtime;
		c.m_nRenders++;
		c.m_bValid = true;
	}

	PublishSun();

	if ( dodsp_csm_debug.GetBool() )
		DrawDebug( mainView );
}

// Why the dynamic sun can't draw on this map / machine (NULL reason = it can)
bool CDoDSPSunCascades::CanDrawDynamicSun( const char **ppszWhy ) const
{
	const char *pszWhy = NULL;
	if ( !m_bHaveSun )
		pszWhy = "this map has no light_environment";
	else if ( !dodsp_csm.GetBool() )
		pszWhy = "the sun cascades are off (dodsp_csm 0)";
	else if ( !DoDSP_SunShadersInUse() )
		pszWhy = "the world isn't on DOD-SP's sun shaders (see the 'DOD-SP sun shaders' line at map load)";
	if ( ppszWhy )
		*ppszWhy = pszWhy;
	return !pszWhy;
}

// Reads the map's level flags (LUMP_MAP_FLAGS) for VRAD's "static props baked without the sun" mark,
// for the lighting the engine draws with (HDR or LDR pass)
void CDoDSPSunCascades::ReadPropsNoSun()
{
	m_bPropsNoSun = false;
	m_bPropsInvariantWarned = false;
	const model_t *pWorld = modelinfo->GetModel( 1 );
	const char *pszMap = pWorld ? modelinfo->GetModelName( pWorld ) : NULL;
	FileHandle_t h = ( pszMap && *pszMap ) ? g_pFullFileSystem->Open( pszMap, "rb" ) : FILESYSTEM_INVALID_HANDLE;
	if ( !h )
		return;
	dheader_t hdr;
	dflagslump_t flags;
	flags.m_LevelFlags = 0;
	bool bRead = g_pFullFileSystem->Read( &hdr, sizeof( hdr ), h ) == sizeof( hdr );
	const lump_t &l = hdr.lumps[LUMP_MAP_FLAGS];
	if ( bRead && l.filelen >= (int)sizeof( flags ) && *(const int *)l.fourCC == 0 )
	{
		g_pFullFileSystem->Seek( h, l.fileofs, FILESYSTEM_SEEK_HEAD );
		bRead = g_pFullFileSystem->Read( &flags, sizeof( flags ), h ) == sizeof( flags );
	}
	g_pFullFileSystem->Close( h );
	if ( !bRead )
		return;

	const bool bHDR = g_pMaterialSystemHardwareConfig->GetHDREnabled() && engine->MapHasHDRLighting();
	m_bPropsNoSun = ( flags.m_LevelFlags & ( bHDR ? DODSP_LVLFLAGS_PROPS_NOSUN_HDR : DODSP_LVLFLAGS_PROPS_NOSUN_NONHDR ) ) != 0;
	if ( m_bPropsNoSun )
		Msg( "DOD-SP sun: this map's static props were baked without the direct sun - the game draws it on them per pixel\n" );
}

void CDoDSPSunCascades::WatchSunInvariant()
{
	if ( m_bPropsNoSun )
	{
		const char *pszWhy = NULL;
		if ( !CanDrawDynamicSun( &pszWhy ) )
		{
			if ( !m_bPropsInvariantWarned )
				Warning( "INVARIANT: DOD-SP sun: this map's static props were baked without the direct sun, but the per-pixel "
						 "sun can't draw (%s) - static props have no direct sun. Fix that, or recompile without -dodsp_sunstyle\n", pszWhy );
			m_bPropsInvariantWarned = true;
		}
		else
			m_bPropsInvariantWarned = false;
	}

	static ConVarRef s_SunBaked( "dodsp_sun_baked" );
	if ( !s_SunBaked.IsValid() || s_SunBaked.GetBool() )
	{
		m_bSunInvariantWarned = false;
		return;
	}
	const char *pszWhy = NULL;
	if ( CanDrawDynamicSun( &pszWhy ) )
		return;
	if ( !m_bSunInvariantWarned )
		Warning( "INVARIANT: DOD-SP sun: dodsp_sun_baked 0 but the dynamic sun can't draw (%s) - putting the baked sun back\n", pszWhy );
	m_bSunInvariantWarned = true;
	engine->ClientCmd_Unrestricted( "dodsp_sun_baked 1\n" );
}

// Publishes everything DOD-SP's shaders need for the sun, every frame (see public/dodsp_shaders.h).
// The placement comes straight out of each cascade's world -> texture matrix (SetupCascade owns it).
void CDoDSPSunCascades::PublishSun()
{
	bool bValid = m_bAllocated && m_bHaveSun && dodsp_csm.GetBool();
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
		bValid = bValid && m_Cascades[i].m_bValid;

	static ConVarRef s_SunBaked( "dodsp_sun_baked" );
	const bool bCanDraw = bValid && CanDrawDynamicSun( NULL );
	const bool bDynamic = bCanDraw && s_SunBaked.IsValid() && !s_SunBaked.GetBool();
	const bool bStaticProps = bCanDraw && m_bPropsNoSun;	// both sun modes (see the top of this file)
	const int nModes = ( bDynamic ? DODSP_SUNMODE_DYNAMIC : 0 ) | ( bStaticProps ? DODSP_SUNMODE_STATICPROPS : 0 );

	Vector vecSun( 0.0f, 0.0f, 0.0f );
	if ( nModes )
	{
		const bool bHDR = g_pMaterialSystemHardwareConfig->GetHDREnabled() && engine->MapHasHDRLighting();
		Vector vecIntensity;
		if ( g_pWorldLights && g_pWorldLights->GetSunIntensity( bHDR, vecIntensity ) )
			vecSun = vecIntensity * MAX( dodsp_sun_scale.GetFloat(), 0.0f );
	}

	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_SUNCOLOR, vecSun );
	// the modes go with the filter values (no free slot left); set here too so "no sun" clears them
	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_FILTER,
		Vector( MAX( dodsp_sun_filter.GetFloat(), 0.0f ), MAX( dodsp_sun_normaloffset.GetFloat(), 0.0f ), (float)nModes ) );
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
		pRenderContext->SetIntRenderingParameter( DODSP_RP_INT_CASCADE0 + i, bValid ? (int)(intp)(ITexture *)m_Cascades[i].m_Depth : 0 );
	if ( !bValid )
		return;

	// The depth row is shared by all three cascades (they all span the whole map along the sun)
	const VMatrix &m0 = m_Cascades[0].m_matWorldToTexture;
	for ( int i = 1; i < DODSP_CASCADE_COUNT; i++ )
	{
		const VMatrix &m = m_Cascades[i].m_matWorldToTexture;
		if ( fabsf( m[2][3] - m0[2][3] ) > 1e-4f || fabsf( m[2][0] - m0[2][0] ) > 1e-6f )
		{
			Warning( "INVARIANT: DOD-SP sun: cascade %d's depth range differs from the near one's - the shader assumes one\n", i );
			break;
		}
	}

	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_AXIS_R, m_vecRight );
	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_AXIS_U, m_vecUp );
	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_AXIS_F, Vector( m0[2][0], m0[2][1], m0[2][2] ) );
	pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_DEPTH, Vector( m0[2][3], 1.0f / m_nRes, MAX( dodsp_sun_bias.GetFloat(), 0.0f ) ) );
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		const VMatrix &m = m_Cascades[i].m_matWorldToTexture;
		float flScale = Vector( m[0][0], m[0][1], m[0][2] ).Length();
		pRenderContext->SetVectorRenderingParameter( DODSP_RP_VEC_CASCADE0 + i, Vector( flScale, m[0][3], m[1][3] ) );
	}
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

bool CDoDSPSunCascades::EnsureViewMaterials()
{
	if ( m_bViewMaterials )
		return true;
	if ( !m_bAllocated )
		return false;

	const char *pszWhy = NULL;
	if ( !g_pMaterialSystemHardwareConfig->SupportsShaderModel_3_0() )
		pszWhy = "this graphics card has no shader model 3.0";
	else if ( !DoDSP_ShaderExists( DODSP_SHADER_CASCADE_DEBUG ) )
		pszWhy = "DOD-SP's shader DLL (" DODSP_SHADER_DLL_NAME ".dll) with " DODSP_SHADER_CASCADE_DEBUG " is not loaded";
	if ( pszWhy )
	{
		if ( !m_bViewWarned )
			Warning( "dodsp_csm_view: can't draw the sun cascades - %s\n", pszWhy );
		m_bViewWarned = true;
		return false;
	}

	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		char szMat[64], szTex[64];
		Q_snprintf( szMat, sizeof( szMat ), "__dodsp_csm_view%d", i );
		Q_snprintf( szTex, sizeof( szTex ), "_rt_DODSP_SunCascade%d", i );
		KeyValues *pKV = new KeyValues( DODSP_SHADER_CASCADE_DEBUG );
		pKV->SetString( "$basetexture", szTex );
		m_ViewMat[i].Init( szMat, pKV );
		bool bFound;
		m_pViewMarker[i] = m_ViewMat[i]->FindVar( "$marker", &bFound, false );
		if ( !bFound ) m_pViewMarker[i] = NULL;
		m_pViewParams[i] = m_ViewMat[i]->FindVar( "$params", &bFound, false );
		if ( !bFound ) m_pViewParams[i] = NULL;
	}
	m_bViewMaterials = true;
	return true;
}

void CDoDSPSunCascades::FreeViewMaterials()
{
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++ )
	{
		m_ViewMat[i].Shutdown();
		m_pViewMarker[i] = m_pViewParams[i] = NULL;
	}
	m_bViewMaterials = false;
}

// Draws the three cascades side by side, top-right: near, mid, far. The ring marks the point
// under the crosshair, put into each cascade with the same matrix the shadow will use.
void CDoDSPSunCascades::DrawView( const CViewSetup &mainView )
{
	if ( !dodsp_csm_view.GetBool() )
	{
		m_bViewWarned = false;
		return;
	}
	if ( !dodsp_csm.GetBool() || !m_bHaveSun || !EnsureViewMaterials() )
		return;

	Vector vecForward;
	AngleVectors( mainView.angles, &vecForward );
	trace_t tr;
	UTIL_TraceLine( mainView.origin, mainView.origin + vecForward * MAX_TRACE_LENGTH, MASK_OPAQUE,
					C_BasePlayer::GetLocalPlayer(), COLLISION_GROUP_NONE, &tr );
	bool bHit = tr.fraction < 1.0f && !tr.startsolid && !tr.allsolid && !( tr.surface.flags & SURF_SKY );

	int nSize = clamp( dodsp_csm_view_size.GetInt(), 64, 1024 );
	int nGap = 8;
	int x = mainView.x + mainView.width - DODSP_CASCADE_COUNT * ( nSize + nGap );
	int y = mainView.y + nGap + 64;	// below the console notify lines

	CMatRenderContextPtr pRenderContext( materials );
	for ( int i = 0; i < DODSP_CASCADE_COUNT; i++, x += nSize + nGap )
	{
		const Cascade_t &c = m_Cascades[i];
		if ( !c.m_bValid || !m_ViewMat[i].IsValid() )
			continue;

		Vector vecTex = c.m_matWorldToTexture.VMul4x3( tr.endpos );
		bool bInside = bHit && vecTex.x >= 0.0f && vecTex.x <= 1.0f && vecTex.y >= 0.0f && vecTex.y <= 1.0f &&
					   vecTex.z >= 0.0f && vecTex.z <= 1.0f;
		if ( m_pViewMarker[i] )
			m_pViewMarker[i]->SetVecValue( vecTex.x, vecTex.y, vecTex.z, bInside ? 1.0f : 0.0f );
		if ( m_pViewParams[i] )
			m_pViewParams[i]->SetVecValue( 1.0f / nSize, dodsp_csm_view_bias.GetFloat(), 0.0f, 0.0f );

		pRenderContext->DrawScreenSpaceRectangle( m_ViewMat[i], x, y, nSize, nSize,
												  0, 0, m_nRes - 1, m_nRes - 1, m_nRes, m_nRes );
	}

	// 4b probe: the near cascade's view drawn into INTZ, read as raw depth, left of the near panel
	if ( m_bProbe && m_Cascades[DODSP_CASCADE_NEAR].m_bValid )
	{
		if ( !m_ProbeMat.IsValid() )
		{
			KeyValues *pKV = new KeyValues( DODSP_SHADER_CASCADE_DEBUG );
			pKV->SetString( "$basetexture", "_rt_DODSP_SunProbeINTZ" );
			pKV->SetInt( "$rawdepth", 1 );
			m_ProbeMat.Init( "__dodsp_csm_probe", pKV );
			bool bFound;
			m_pProbeMarker = m_ProbeMat->FindVar( "$marker", &bFound, false );
			if ( !bFound ) m_pProbeMarker = NULL;
			m_pProbeParams = m_ProbeMat->FindVar( "$params", &bFound, false );
			if ( !bFound ) m_pProbeParams = NULL;
		}
		const Cascade_t &c = m_Cascades[DODSP_CASCADE_NEAR];
		Vector vecTex = c.m_matWorldToTexture.VMul4x3( tr.endpos );
		bool bInside = bHit && vecTex.x >= 0.0f && vecTex.x <= 1.0f && vecTex.y >= 0.0f && vecTex.y <= 1.0f &&
					   vecTex.z >= 0.0f && vecTex.z <= 1.0f;
		if ( m_pProbeMarker )
			m_pProbeMarker->SetVecValue( vecTex.x, vecTex.y, vecTex.z, bInside ? 1.0f : 0.0f );
		if ( m_pProbeParams )
			m_pProbeParams->SetVecValue( 1.0f / nSize, dodsp_csm_view_bias.GetFloat(), 0.0f, 0.0f );
		int xProbe = mainView.x + mainView.width - ( DODSP_CASCADE_COUNT + 1 ) * ( nSize + nGap );
		pRenderContext->DrawScreenSpaceRectangle( m_ProbeMat, xProbe, y, nSize, nSize,
												  0, 0, m_nRes - 1, m_nRes - 1, m_nRes, m_nRes );
	}

	engine->Con_NPrintf( 6, "DOD-SP sun cascades on screen (top-right): %snear | mid | far. Ring = your aim point: green sunlit, red shadowed%s",
						 m_bProbe ? "INTZ probe | " : "", bHit ? "" : " (aim at something)" );
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

void DoDSP_DrawSunCascadeView( const CViewSetup &mainView )
{
	s_DoDSPSunCascades.DrawView( mainView );
}

bool DoDSP_GetSunCascade( int nCascade, ITexture **ppDepth, VMatrix *pWorldToTexture )
{
	return s_DoDSPSunCascades.Get( nCascade, ppDepth, pWorldToTexture );
}

CON_COMMAND( dodsp_csm_report, "DOD-SP: the sun cascades - size, coverage and cost" )
{
	s_DoDSPSunCascades.Report();
}
