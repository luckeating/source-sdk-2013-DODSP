//===================== DOD-SP lamps =====================//
//
// Lighting stage 5: every lamp in the map is drawn live, the way stage 4 draws the sun. VRAD puts
// the lamps' direct light in a light style the game switches off (5b); their bounce stays baked.
//
// Our shaders have no room left for per-lamp constants (32 pixel constants; the skin shader uses
// all 16 samplers), so the lamps are added AFTER the opaque scene, in screen space ("deferred"):
// the brush and model shaders also write each opaque pixel's albedo, world normal and distance
// from the eye to three extra render targets (dodsp_gbuffer_fxc.h). This file owns those targets.
//
// STAGE 5a (this file so far) is the probe: the G-buffer only, no lamp light yet. The risk it
// checks first: that the 2014 engine lets our shaders write render targets 1..3 during the main
// view's opaque pass, and which surfaces end up without G-buffer data.
//
// INVARIANT: the G-buffer is bound only around the main view's opaque pass (DoDSP_LampsBeginGBuffer
// / DoDSP_LampsEndGBuffer, viewrender.cpp) - never while anything translucent, a shadow map or
// another view draws - and is cleared to 0 ("not lit by the lamps") before it. One owner: this file.
// Guards: the console line at map load (formats and sizes the card gave), an INVARIANT line if
// they are wrong (the G-buffer then stays off), and dodsp_lamps_check (the pixel you aim at, read
// back and compared with a trace).
//
//========================================================//

#include "cbase.h"
#include "c_dodsp_lamps.h"
#include "c_dodsp_sdkshaders.h"
#include "c_dodsp_shaderdll.h"
#include "c_dodsp_sun_cascades.h"
#include "igamesystem.h"
#include "c_baseplayer.h"
#include "view_shared.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "KeyValues.h"
#include "dodsp_shaders.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_lamps_gbuffer( "dodsp_lamps_gbuffer", "1", 0,
	"DOD-SP stage 5: the main view writes the lamps' G-buffer (albedo, normal, distance) - 0 = off" );
static ConVar dodsp_lamps_view( "dodsp_lamps_view", "0", 0,
	"DOD-SP stage 5a debug: show the lamps' G-buffer on screen (bottom: albedo | normal | distance)" );
static ConVar dodsp_lamps_view_far( "dodsp_lamps_view_far", "2000", 0,
	"DOD-SP stage 5a debug: the distance drawn white in dodsp_lamps_view" );

enum
{
	DODSP_GBUF_ALBEDO = 0,
	DODSP_GBUF_NORMAL,
	DODSP_GBUF_DIST,
	DODSP_GBUF_COUNT
};

static const char *s_pszGBufferName[DODSP_GBUF_COUNT] = { "_rt_DODSP_GAlbedo", "_rt_DODSP_GNormal", "_rt_DODSP_GDist" };
static const char *s_pszGBufferLabel[DODSP_GBUF_COUNT] = { "albedo", "normal", "distance" };
#define DODSP_GBUF_FORMAT	IMAGE_FORMAT_RGBA16161616F

// A half float (IEEE 754 binary16) to a float
static float DoDSP_HalfToFloat( unsigned short h )
{
	const int nSign = ( h >> 15 ) & 1, nExp = ( h >> 10 ) & 31, nMant = h & 1023;
	float f;
	if ( nExp == 0 )
		f = ldexpf( (float)nMant, -24 );						// subnormal
	else if ( nExp == 31 )
		f = nMant ? 0.0f : FLT_MAX;								// NaN reads as 0, infinity as huge
	else
		f = ldexpf( (float)( nMant | 1024 ), nExp - 25 );
	return nSign ? -f : f;
}

class CDoDSPLamps : public CAutoGameSystem
{
public:
	CDoDSPLamps() : CAutoGameSystem( "CDoDSPLamps" ), m_bAllocated( false ), m_bReady( false ), m_bBound( false ),
		m_bViewMaterials( false ), m_bViewWarned( false ), m_nBoundFrames( 0 ) {}

	virtual void LevelInitPostEntity()	{ EnsureTargets(); m_nBoundFrames = 0; }
	virtual void Shutdown()				{ FreeViewMaterials(); FreeTargets(); }

	bool BeginGBuffer();
	void EndGBuffer();
	void DrawView( const CViewSetup &mainView );
	void Check();

private:
	void EnsureTargets();
	void FreeTargets();
	bool EnsureViewMaterials();
	void FreeViewMaterials();
	bool ReadCentre( int nTarget, float flOut[4] );

	bool				m_bAllocated;
	bool				m_bReady;			// all three targets exist in the format and size asked for
	bool				m_bBound;			// between Begin and End
	CTextureReference	m_Targets[DODSP_GBUF_COUNT];

	CMaterialReference	m_ViewMat[DODSP_GBUF_COUNT];
	IMaterialVar		*m_pViewParams[DODSP_GBUF_COUNT];
	bool				m_bViewMaterials;
	bool				m_bViewWarned;
	int					m_nBoundFrames;		// frames the G-buffer was written this map (dodsp_lamps_check)
};

static CDoDSPLamps s_DoDSPLamps;

void CDoDSPLamps::EnsureTargets()
{
	if ( m_bAllocated )
		return;
	if ( !g_pMaterialSystemHardwareConfig->SupportsShaderModel_3_0() )
	{
		Warning( "DOD-SP lamps: this graphics card has no shader model 3.0 - no live lamps\n" );
		m_bAllocated = true;
		return;
	}

	materials->BeginRenderTargetAllocation();
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
	{
		ITexture *pTex = materials->CreateNamedRenderTargetTextureEx( s_pszGBufferName[i], 1, 1, RT_SIZE_FULL_FRAME_BUFFER,
			DODSP_GBUF_FORMAT, MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_POINTSAMPLE );
		m_Targets[i].Init( pTex );
		if ( pTex )
			pTex->DecrementReferenceCount();	// created with one reference; the CTextureReference holds it now
	}
	materials->EndRenderTargetAllocation();
	m_bAllocated = true;

	int nScreenW, nScreenH;
	materials->GetBackBufferDimensions( nScreenW, nScreenH );
	const char *pszWhy = NULL;
	for ( int i = 0; i < DODSP_GBUF_COUNT && !pszWhy; i++ )
	{
		ITexture *pTex = m_Targets[i];
		if ( !pTex || pTex->IsError() )
			pszWhy = "a target could not be created";
		else if ( pTex->GetImageFormat() != DODSP_GBUF_FORMAT )
			pszWhy = "the card gave another format than RGBA16F";
		else if ( pTex->GetActualWidth() != nScreenW || pTex->GetActualHeight() != nScreenH )
			pszWhy = "a target is not the size of the screen";
	}
	m_bReady = !pszWhy;
	if ( pszWhy )
		Warning( "INVARIANT: DOD-SP lamps G-buffer: %s (screen %d x %d) - the G-buffer stays off\n", pszWhy, nScreenW, nScreenH );
	else
		Msg( "DOD-SP lamps G-buffer: %d targets, %d x %d RGBA16F (stage 5a probe: dodsp_lamps_view 1 shows it, dodsp_lamps_check reads your aim point)\n",
			 DODSP_GBUF_COUNT, nScreenW, nScreenH );
}

void CDoDSPLamps::FreeTargets()
{
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
		m_Targets[i].Shutdown();
	m_bAllocated = m_bReady = false;
}

bool CDoDSPLamps::BeginGBuffer()
{
	Assert( !m_bBound );
	m_bBound = false;
	// HDR only: render target 0 is then RGBA16F like ours (MRT wants one bit depth); LDR is not supported
	if ( !dodsp_lamps_gbuffer.GetBool() || !m_bReady || !DoDSP_SunShadersInUse() || !DoDSP_DrawsHDR() ||
		 g_pMaterialSystemHardwareConfig->GetHDRType() != HDR_TYPE_FLOAT )
		return false;

	CMatRenderContextPtr pRenderContext( materials );
	// Cleared to 0 = "the lamps don't light this pixel" wherever no DOD-SP shader draws
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
	{
		pRenderContext->PushRenderTargetAndViewport( m_Targets[i] );
		pRenderContext->ClearColor4ub( 0, 0, 0, 0 );
		pRenderContext->ClearBuffers( true, false, false );
		pRenderContext->PopRenderTargetAndViewport();
	}
	// A copy of the current targets and viewport with ours added as 1..3; End's pop takes them off
	pRenderContext->PushRenderTargetAndViewport();
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
		pRenderContext->SetRenderTargetEx( i + 1, m_Targets[i] );
	m_bBound = true;
	m_nBoundFrames++;
	return true;
}

void CDoDSPLamps::EndGBuffer()
{
	if ( !m_bBound )
		return;
	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->PopRenderTargetAndViewport();
	m_bBound = false;
}

bool CDoDSPLamps::EnsureViewMaterials()
{
	if ( m_bViewMaterials )
		return true;
	const char *pszWhy = NULL;
	if ( !m_bReady )
		pszWhy = "the G-buffer is off (see the DOD-SP lamps line at map load)";
	else if ( !DoDSP_ShaderExists( DODSP_SHADER_GBUFFER_DEBUG ) )
		pszWhy = "DOD-SP's shader DLL (" DODSP_SHADER_DLL_NAME ".dll) with " DODSP_SHADER_GBUFFER_DEBUG " is not loaded";
	if ( pszWhy )
	{
		if ( !m_bViewWarned )
			Warning( "dodsp_lamps_view: can't draw the G-buffer - %s\n", pszWhy );
		m_bViewWarned = true;
		return false;
	}

	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
	{
		char szMat[64];
		Q_snprintf( szMat, sizeof( szMat ), "__dodsp_lamps_view%d", i );
		KeyValues *pKV = new KeyValues( DODSP_SHADER_GBUFFER_DEBUG );
		pKV->SetString( "$basetexture", s_pszGBufferName[i] );
		m_ViewMat[i].Init( szMat, pKV );
		bool bFound;
		m_pViewParams[i] = m_ViewMat[i]->FindVar( "$params", &bFound, false );
		if ( !bFound )
			m_pViewParams[i] = NULL;
	}
	m_bViewMaterials = true;
	return true;
}

void CDoDSPLamps::FreeViewMaterials()
{
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++ )
	{
		m_ViewMat[i].Shutdown();
		m_pViewParams[i] = NULL;
	}
	m_bViewMaterials = false;
}

// The three targets side by side along the bottom of the screen: albedo | normal | distance
void CDoDSPLamps::DrawView( const CViewSetup &mainView )
{
	if ( !dodsp_lamps_view.GetBool() )
	{
		m_bViewWarned = false;
		return;
	}
	if ( !EnsureViewMaterials() )
		return;

	const int nGap = 8;
	const int nW = ( mainView.width - 4 * nGap ) / DODSP_GBUF_COUNT;
	const int nH = nW * mainView.height / MAX( mainView.width, 1 );
	int x = mainView.x + nGap;
	const int y = mainView.y + mainView.height - nH - nGap;
	const float flFar = MAX( dodsp_lamps_view_far.GetFloat(), 1.0f );

	CMatRenderContextPtr pRenderContext( materials );
	for ( int i = 0; i < DODSP_GBUF_COUNT; i++, x += nW + nGap )
	{
		ITexture *pTex = m_Targets[i];
		if ( !pTex || !m_ViewMat[i].IsValid() )
			continue;
		if ( m_pViewParams[i] )
			m_pViewParams[i]->SetVecValue( (float)i, 1.0f / flFar, 0.0f, 0.0f );
		const int nTexW = pTex->GetActualWidth(), nTexH = pTex->GetActualHeight();
		pRenderContext->DrawScreenSpaceRectangle( m_ViewMat[i], x, y, nW, nH, 0, 0, nTexW - 1, nTexH - 1, nTexW, nTexH );
	}
	engine->Con_NPrintf( 7, "DOD-SP lamps G-buffer (bottom): albedo | normal | distance. Magenta = no data there (the lamps won't light it)" );
}

// The G-buffer pixel at the middle of the screen (where the crosshair is), as four floats
bool CDoDSPLamps::ReadCentre( int nTarget, float flOut[4] )
{
	ITexture *pTex = m_Targets[nTarget];
	if ( !pTex )
		return false;
	unsigned short data[4] = { 0, 0, 0, 0 };
	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->PushRenderTargetAndViewport( pTex );
	pRenderContext->ReadPixels( pTex->GetActualWidth() / 2, pTex->GetActualHeight() / 2, 1, 1, (unsigned char *)data, DODSP_GBUF_FORMAT );
	pRenderContext->PopRenderTargetAndViewport();
	for ( int k = 0; k < 4; k++ )
		flOut[k] = DoDSP_HalfToFloat( data[k] );
	return true;
}

// One line: what the G-buffer holds at the crosshair next to what a trace from the eye finds there
void CDoDSPLamps::Check()
{
	if ( !m_bReady )
	{
		Msg( "DOD-SP lamps check: the G-buffer is off (see the DOD-SP lamps line at map load)\n" );
		return;
	}
	if ( m_nBoundFrames == 0 )
	{
		Msg( "DOD-SP lamps check: the G-buffer was never written on this map (%s)\n",
			 !dodsp_lamps_gbuffer.GetBool() ? "dodsp_lamps_gbuffer is 0" :
			 !DoDSP_SunShadersInUse() ? "the DOD-SP shaders are not in use" :
			 !DoDSP_DrawsHDR() ? "HDR is off - see the DOD-SP HDR line" : "HDR is not the float kind" );
		return;
	}

	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return;
	Vector vecEye = pPlayer->EyePosition(), vecForward;
	AngleVectors( pPlayer->EyeAngles(), &vecForward );
	trace_t tr;
	UTIL_TraceLine( vecEye, vecEye + vecForward * MAX_TRACE_LENGTH, MASK_OPAQUE, pPlayer, COLLISION_GROUP_NONE, &tr );
	const bool bHit = tr.fraction < 1.0f && !tr.startsolid && !( tr.surface.flags & SURF_SKY );
	const char *pszWhat = !bHit ? "sky / nothing" :
		( tr.m_pEnt && !tr.DidHitWorld() && tr.m_pEnt->GetModel() ) ? modelinfo->GetModelName( tr.m_pEnt->GetModel() ) : tr.surface.name;

	float vAlbedo[4], vNormal[4], vDist[4];
	if ( !ReadCentre( DODSP_GBUF_ALBEDO, vAlbedo ) || !ReadCentre( DODSP_GBUF_NORMAL, vNormal ) || !ReadCentre( DODSP_GBUF_DIST, vDist ) )
		return;
	const float flNormalLen = sqrtf( vNormal[0] * vNormal[0] + vNormal[1] * vNormal[1] + vNormal[2] * vNormal[2] );
	if ( flNormalLen < 0.5f )
	{
		Msg( "DOD-SP lamps check: NOT written at your aim point (%s, trace distance %.0f) - the lamps won't light it; "
			 "distance there %.0f, albedo %.2f %.2f %.2f\n",
			 pszWhat, bHit ? ( tr.endpos - vecEye ).Length() : 0.0f, vDist[0], vAlbedo[0], vAlbedo[1], vAlbedo[2] );
		return;
	}
	Msg( "DOD-SP lamps check: %s | distance %.0f (trace %.0f) | normal %.2f %.2f %.2f (trace %.2f %.2f %.2f) | albedo %.2f %.2f %.2f\n",
		 pszWhat, vDist[0], bHit ? ( tr.endpos - vecEye ).Length() : 0.0f,
		 vNormal[0], vNormal[1], vNormal[2], tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z,
		 vAlbedo[0], vAlbedo[1], vAlbedo[2] );
}

CON_COMMAND( dodsp_lamps_check, "DOD-SP stage 5a: what the lamps' G-buffer holds at your aim point, next to a trace" )
{
	s_DoDSPLamps.Check();
}

bool DoDSP_LampsBeginGBuffer()
{
	return s_DoDSPLamps.BeginGBuffer();
}

void DoDSP_LampsEndGBuffer()
{
	s_DoDSPLamps.EndGBuffer();
}

void DoDSP_DrawLampsView( const CViewSetup &mainView )
{
	s_DoDSPLamps.DrawView( mainView );
}
