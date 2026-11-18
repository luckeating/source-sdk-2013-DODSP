//===================== DOD-SP live lamps =====================//
//
// Lighting stage 5: the lamps the mapper marks live ("_dodsp_live" "1" on a light_spot) are drawn by
// the game, with real-time shadows, the engine's FORWARD way: one projected texture each (the same
// path as env_projectedtexture / the flashlight: an additive pass over every surface the lamp
// reaches, shadowed from a depth map). Forward keeps MSAA - the deferred design (5a) did not and was
// rejected 2026-11-18 (CLAUDE.md).
//
// One owner for each lamp: its light entity. VRAD (lightmap.cpp) bakes the lamp's bounce into style
// 0, puts its direct light in DODSP_LIGHTSTYLE_LAMPS (switched off by the server while the lamps draw
// live), keeps it OUT of the world lights (the engine would light every model with it a second time)
// and writes the lamp as VRAD lit it to the map's pak file: DODSP_LAMPS_FILE_HDR / _LDR. This file
// reads that and nothing else - colour, brightness, cone and falloff are the baked lamp's own.
//
// How the bake's numbers become a projected texture:
//   cone      VRAD: x cos(angle to the axis) x fringe((cos - stopdot2) / (stopdot - stopdot2))^exponent.
//             The projected texture's "cookie" texture holds exactly that, per texel (CDoDSPLampCookie).
//   distance  VRAD: 1 / (c + l d + q d^2). The engine's flashlight shader: c' + l'/d + q'/d^2. A lamp
//             with one term maps exactly (the default light: quadratic only); mixed terms use the
//             strongest at half the lamp's range - dodsp_lamps says which lamps are approximate.
//   colour    VRAD's intensity / the shaders' flashlight scale (0.25 in HDR, dodsp_r_flashlightbrightness)
//             x dodsp_lamps_scale (1 = as baked; for calibration).
//
// INVARIANT: a live lamp shows exactly once - live (style 30 off) or baked (style 30 on, server's
// dodsp_lamps_baked / dodsp_lamps_fallback), never both, never neither. This file is the only writer
// of dodsp_lamps_fallback. Guards: the console line at map load (how many lamps, which file, drawn
// live or why not), `dodsp_lamps` (every lamp: cone, range, colour, attenuation exact/approximate),
// an INVARIANT line if the fallback cvar won't take, lint OWNERS (dodsp_lamps_fallback, the pak names).
//
//=============================================================//

#include "cbase.h"
#include "c_dodsp_sun_cascades.h"	// DoDSP_DrawsHDR
#include "dodsp_lightstyles.h"
#include "igamesystem.h"
#include "iclientshadowmgr.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "vtf/vtf.h"
#include "bitmap/imageformat.h"
#include "pixelwriter.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar dodsp_lamps_scale( "dodsp_lamps_scale", "1", 0,
	"DOD-SP: live lamp brightness x this (1 = as VRAD baked it; for calibration against dodsp_lamps_baked 1)" );
static ConVar dodsp_lamps_shadows( "dodsp_lamps_shadows", "1", 0, "DOD-SP: live lamps cast real-time shadows" );
static ConVar dodsp_lamps_nearz( "dodsp_lamps_nearz", "4", 0, "DOD-SP: live lamps light nothing closer than this to the bulb" );
static ConVar dodsp_lamps_cutoff( "dodsp_lamps_cutoff", "0.0005", 0,
	"DOD-SP: a live lamp without its own falloff distance reaches as far as its light stays above this (lightmap units)" );
static ConVar dodsp_lamps_maxrange( "dodsp_lamps_maxrange", "2048", 0, "DOD-SP: no live lamp reaches further than this" );

#define DODSP_LAMP_COOKIE_RES	512		// 128 drew the cone's hard edge as visible stair steps (2026-11-18)
#define DODSP_LAMP_COOKIE_SS	4		// samples per texel side: an edge texel holds how much of it is inside the cone

// The live lamp's light per unit of VRAD's world-light intensity, measured against the bake: build 44344800913,
// cafe hall lamp (quadratic falloff), live / baked = 3.05 and 3.09 on two unclipped floor spots (unlit areas
// 1.00). The factor is constant (it doesn't change across the cone), so it is a units difference between
// VRAD's lightmaps and the engine's flashlight, not a shape difference. dodsp_lamps_scale stays 1 = as baked.
#define DODSP_LAMP_BAKE_MATCH	( 1.0f / 3.07f )
#define DODSP_LAMP_MAX_FOV		170.0f	// a projected texture's frustum must stay well under 180 degrees

//-----------------------------------------------------------------------------
// One lamp as VRAD baked it (DODSP_LAMPS_FILE_*)
//-----------------------------------------------------------------------------
struct DoDSPLampData_t
{
	Vector	m_vecOrigin;
	Vector	m_vecDir;			// where it shines
	Vector	m_vecIntensity;		// world-light units (VRAD's / 255)
	float	m_flAttn[3];		// VRAD: 1 / ( c + l d + q d^2 )
	float	m_flStopDot;		// cos of the inner cone half-angle
	float	m_flStopDot2;		// cos of the outer cone half-angle
	float	m_flExponent;
	float	m_flRadius;			// _distance, 0 = none
	float	m_flFadeStart, m_flFadeEnd;	// hard falloff (end > start when set)
};

//-----------------------------------------------------------------------------
// The cone as a texture: what VRAD multiplies a spot's light by, per direction (see the top)
//-----------------------------------------------------------------------------
class CDoDSPLampCookie : public ITextureRegenerator
{
public:
	CDoDSPLampCookie( float flStopDot, float flStopDot2, float flExponent, float flTanHalfFov )
		: m_flStopDot( flStopDot ), m_flStopDot2( flStopDot2 ), m_flExponent( flExponent ), m_flTan( flTanHalfFov ) {}

	virtual void RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pRect )
	{
		const int w = pVTFTexture->Width(), h = pVTFTexture->Height();
		CPixelWriter writer;
		writer.SetPixelMemory( pVTFTexture->Format(), pVTFTexture->ImageData( 0, 0, 0 ), pVTFTexture->RowSizeInBytes( 0 ) );
		for ( int y = 0; y < h; y++ )
		{
			writer.Seek( 0, y );
			for ( int x = 0; x < w; x++ )
			{
				// the border stays black, so nothing outside the cone is ever lit (the sampler clamps)
				// the texel's value is the average over DODSP_LAMP_COOKIE_SS^2 points in it, so a hard cone
				// edge comes out as a smooth one texel wide ramp instead of whole texels on or off
				float flValue = 0.0f;
				if ( x > 0 && y > 0 && x < w - 1 && y < h - 1 )
				{
					for ( int sy = 0; sy < DODSP_LAMP_COOKIE_SS; sy++ )
					{
						for ( int sx = 0; sx < DODSP_LAMP_COOKIE_SS; sx++ )
						{
							const float u = ( ( x + ( sx + 0.5f ) / DODSP_LAMP_COOKIE_SS ) / w * 2.0f - 1.0f ) * m_flTan;
							const float v = ( ( y + ( sy + 0.5f ) / DODSP_LAMP_COOKIE_SS ) / h * 2.0f - 1.0f ) * m_flTan;
							const float flCos = 1.0f / sqrtf( 1.0f + u * u + v * v );	// cos of the angle to the axis
							flValue += Cone( flCos );
						}
					}
					flValue /= (float)( DODSP_LAMP_COOKIE_SS * DODSP_LAMP_COOKIE_SS );
				}
				// the flashlight samplers read the cookie as sRGB: store gamma so the shader gets this value back
				const int c = clamp( (int)( SrgbLinearToGamma( flValue ) * 255.0f + 0.5f ), 0, 255 );
				writer.WritePixel( c, c, c, 255 );
			}
		}
	}
	virtual void Release() { delete this; }

	// VRAD's spot falloff (lightmap.cpp GatherSampleLightSSE, emit_spotlight), for one direction
	float Cone( float flCos ) const
	{
		if ( flCos <= m_flStopDot2 )
			return 0.0f;
		float flFringe = 1.0f;
		if ( flCos <= m_flStopDot && m_flStopDot > m_flStopDot2 )
		{
			flFringe = clamp( ( flCos - m_flStopDot2 ) / ( m_flStopDot - m_flStopDot2 ), 0.0f, 1.0f );
			if ( m_flExponent != 0.0f && m_flExponent != 1.0f )
				flFringe = powf( flFringe, m_flExponent );
		}
		return flCos * flFringe;
	}

private:
	float m_flStopDot, m_flStopDot2, m_flExponent, m_flTan;
};

//-----------------------------------------------------------------------------
// The live lamps
//-----------------------------------------------------------------------------
class CDoDSPLamps : public CAutoGameSystemPerFrame
{
public:
	CDoDSPLamps() : CAutoGameSystemPerFrame( "CDoDSPLamps" ), m_bLive( false ), m_bFallbackWarned( false ) { m_szFile[0] = 0; }

	virtual void LevelInitPostEntity();
	virtual void LevelShutdownPreEntity()	{ DestroyLamps(); m_Lamps.Purge(); m_szFile[0] = 0; }
	virtual void PreRender();

	void Report() const;

private:
	struct Lamp_t
	{
		DoDSPLampData_t			m_Data;
		FlashlightState_t		m_State;
		CTextureReference		m_Cookie;
		ClientShadowHandle_t	m_hLight;
		bool					m_bApprox;	// its distance falloff is approximated (mixed attenuation terms)
		bool					m_bFovClipped;
		float					m_flRange;
	};

	bool Load();
	void BuildState( int nLamp, Lamp_t &lamp );
	void CreateLamps();
	void DestroyLamps();
	const char *WhyNotLive() const;
	void SetFallback( bool bFallback );

	CUtlVector< Lamp_t >	m_Lamps;
	char					m_szFile[64];	// where they came from this map
	bool					m_bLive;		// the flashlights exist right now
	bool					m_bFallbackWarned;
};

static CDoDSPLamps s_DoDSPLamps;

static bool DoDSP_ParseVec( KeyValues *pKV, const char *pszKey, Vector &v )
{
	const char *psz = pKV->GetString( pszKey, NULL );
	return psz && sscanf( psz, "%f %f %f", &v.x, &v.y, &v.z ) == 3;
}

bool CDoDSPLamps::Load()
{
	m_Lamps.Purge();
	m_szFile[0] = 0;
	// the set VRAD lit the lightmaps in use with; the other set if the map has only that one
	const bool bHDR = DoDSP_DrawsHDR();
	const char *pszFiles[2] = { bHDR ? DODSP_LAMPS_FILE_HDR : DODSP_LAMPS_FILE_LDR, bHDR ? DODSP_LAMPS_FILE_LDR : DODSP_LAMPS_FILE_HDR };
	KeyValues *pKV = new KeyValues( "DODSP_Lamps" );
	bool bLoaded = false;
	for ( int i = 0; i < 2 && !bLoaded; i++ )
	{
		if ( pKV->LoadFromFile( g_pFullFileSystem, pszFiles[i], "GAME" ) )
		{
			Q_strncpy( m_szFile, pszFiles[i], sizeof( m_szFile ) );
			bLoaded = true;
		}
	}
	if ( bLoaded )
	{
		for ( KeyValues *pLamp = pKV->GetFirstTrueSubKey(); pLamp; pLamp = pLamp->GetNextTrueSubKey() )
		{
			DoDSPLampData_t d;
			Vector vecAttn, vecCone, vecFade( 0, 0, 0 );
			if ( !DoDSP_ParseVec( pLamp, "origin", d.m_vecOrigin ) || !DoDSP_ParseVec( pLamp, "normal", d.m_vecDir ) ||
				 !DoDSP_ParseVec( pLamp, "intensity", d.m_vecIntensity ) || !DoDSP_ParseVec( pLamp, "attn", vecAttn ) ||
				 !DoDSP_ParseVec( pLamp, "cone", vecCone ) )
			{
				Warning( "INVARIANT: DOD-SP lamps: a lamp in %s is missing a value - skipped (recompile with this build's VRAD)\n", m_szFile );
				continue;
			}
			sscanf( pLamp->GetString( "fade", "0 0" ), "%f %f", &vecFade.x, &vecFade.y );
			d.m_flAttn[0] = vecAttn.x; d.m_flAttn[1] = vecAttn.y; d.m_flAttn[2] = vecAttn.z;
			d.m_flStopDot = vecCone.x; d.m_flStopDot2 = vecCone.y; d.m_flExponent = vecCone.z;
			d.m_flRadius = pLamp->GetFloat( "radius", 0.0f );
			d.m_flFadeStart = vecFade.x; d.m_flFadeEnd = vecFade.y;
			VectorNormalize( d.m_vecDir );
			Lamp_t &lamp = m_Lamps[m_Lamps.AddToTail()];
			lamp.m_Data = d;
			lamp.m_hLight = CLIENTSHADOW_INVALID_HANDLE;
			lamp.m_bApprox = lamp.m_bFovClipped = false;
			lamp.m_flRange = 0.0f;
		}
	}
	pKV->deleteThis();
	return m_Lamps.Count() > 0;
}

// The projected texture that draws one lamp the way VRAD baked it (see the top of the file)
void CDoDSPLamps::BuildState( int nLamp, Lamp_t &lamp )
{
	const DoDSPLampData_t &d = lamp.m_Data;
	FlashlightState_t &state = lamp.m_State;
	state = FlashlightState_t();

	// cone -> frustum just wider than the outer cone; the cookie draws the cone inside it
	const float flOuterDeg = RAD2DEG( acosf( clamp( d.m_flStopDot2, -1.0f, 1.0f ) ) );
	const float flFov = MIN( 2.0f * flOuterDeg * 1.05f + 2.0f, DODSP_LAMP_MAX_FOV );
	lamp.m_bFovClipped = ( 2.0f * flOuterDeg > DODSP_LAMP_MAX_FOV );
	const float flTanHalf = tanf( DEG2RAD( flFov * 0.5f ) );

	// distance: VRAD's 1/(c + l d + q d^2) as the shader's c' + l'/d + q'/d^2
	const float c = d.m_flAttn[0], l = d.m_flAttn[1], q = d.m_flAttn[2];
	const float flBright = MAX( d.m_vecIntensity.x, MAX( d.m_vecIntensity.y, d.m_vecIntensity.z ) );
	const float flCutoff = MAX( dodsp_lamps_cutoff.GetFloat(), 1e-6f );
	float flRange = dodsp_lamps_maxrange.GetFloat();
	if ( d.m_flRadius > 0.0f )
		flRange = MIN( flRange, d.m_flRadius );
	if ( d.m_flFadeEnd > d.m_flFadeStart && d.m_flFadeEnd > 0.0f )
		flRange = MIN( flRange, d.m_flFadeEnd );
	// where VRAD's light drops below the cutoff: c + l d + q d^2 = flBright / flCutoff
	const float k = flBright / flCutoff;
	if ( q > 0.0f )
		flRange = MIN( flRange, ( -l + sqrtf( MAX( l * l - 4.0f * q * ( c - k ), 0.0f ) ) ) / ( 2.0f * q ) );
	else if ( l > 0.0f )
		flRange = MIN( flRange, ( k - c ) / l );
	flRange = MAX( flRange, dodsp_lamps_nearz.GetFloat() + 16.0f );
	lamp.m_flRange = flRange;

	const int nTerms = ( c > 0.0f ? 1 : 0 ) + ( l > 0.0f ? 1 : 0 ) + ( q > 0.0f ? 1 : 0 );
	lamp.m_bApprox = nTerms > 1;
	float flC = 0.0f, flL = 0.0f, flQ = 0.0f;
	if ( nTerms <= 1 )
	{
		if ( q > 0.0f )			flQ = 1.0f / q;
		else if ( l > 0.0f )	flL = 1.0f / l;
		else					flC = 1.0f / MAX( c, 1e-6f );
	}
	else
	{
		// the strongest term at half the lamp's range carries it
		const float dm = flRange * 0.5f;
		const float tc = c, tl = l * dm, tq = q * dm * dm;
		if ( tq >= tl && tq >= tc )	flQ = 1.0f / q;
		else if ( tl >= tc )		flL = 1.0f / l;
		else						flC = 1.0f / c;
	}

	// colour: undo the shaders' flashlight scale so the lamp lands in the bake's units
	static ConVarRef s_FlashScale( "dodsp_r_flashlightbrightness" );
	float flShaderScale = DoDSP_DrawsHDR() ? ( s_FlashScale.IsValid() ? s_FlashScale.GetFloat() : 0.25f ) : 2.0f;
	flShaderScale = MAX( flShaderScale, 1e-3f );
	const float flScale = DODSP_LAMP_BAKE_MATCH * MAX( dodsp_lamps_scale.GetFloat(), 0.0f ) / flShaderScale;

	Vector vecForward = d.m_vecDir, vecRight, vecUp;
	VectorVectors( vecForward, vecRight, vecUp );
	state.m_vecLightOrigin = d.m_vecOrigin;
	BasisToQuaternion( vecForward, vecRight, vecUp, state.m_quatOrientation );
	state.m_NearZ = dodsp_lamps_nearz.GetFloat();
	state.m_FarZ = flRange;
	state.m_fHorizontalFOVDegrees = state.m_fVerticalFOVDegrees = flFov;
	state.m_fConstantAtten = flC;
	state.m_fLinearAtten = flL;
	state.m_fQuadraticAtten = flQ;
	state.m_FarZAtten = flRange;
	state.m_fBrightnessScale = 1.0f;
	state.m_Color[0] = d.m_vecIntensity.x * flScale;
	state.m_Color[1] = d.m_vecIntensity.y * flScale;
	state.m_Color[2] = d.m_vecIntensity.z * flScale;
	state.m_Color[3] = 0.0f;
	state.m_bEnableShadows = dodsp_lamps_shadows.GetBool();
	state.m_flShadowAtten = 0.0f;	// full shadow at every distance (the shader would fade it with the light otherwise)
	// the same shadow-map biases env_projectedtexture uses (Mapbase's cvars, owned by clientshadowmgr.cpp)
	static ConVarRef s_SlopeBias( "mat_slopescaledepthbias_shadowmap" ), s_DepthBias( "mat_depthbias_shadowmap" );
	state.m_flShadowSlopeScaleDepthBias = s_SlopeBias.IsValid() ? s_SlopeBias.GetFloat() : 4.0f;
	state.m_flShadowDepthBias = s_DepthBias.IsValid() ? s_DepthBias.GetFloat() : 0.00001f;
	state.m_nShadowQuality = 1;

	if ( !lamp.m_Cookie.IsValid() )
	{
		char szName[64];
		Q_snprintf( szName, sizeof( szName ), "_rt_DODSP_LampCookie%d", nLamp );
		ITexture *pTex = materials->CreateProceduralTexture( szName, TEXTURE_GROUP_OTHER, DODSP_LAMP_COOKIE_RES, DODSP_LAMP_COOKIE_RES,
			IMAGE_FORMAT_RGBA8888, TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD |
			TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY );
		if ( pTex )
		{
			pTex->SetTextureRegenerator( new CDoDSPLampCookie( d.m_flStopDot, d.m_flStopDot2, d.m_flExponent, flTanHalf ) );
			pTex->Download();
			lamp.m_Cookie.Init( pTex );
			pTex->DecrementReferenceCount();	// the CTextureReference holds it now
		}
	}
	state.m_pSpotlightTexture = lamp.m_Cookie;
	state.m_nSpotlightTextureFrame = 0;
}

// The one answer to "may the live lamps draw now?" - NULL = yes, else why not
const char *CDoDSPLamps::WhyNotLive() const
{
	if ( !m_Lamps.Count() )
		return "the map has no live lamps";
	static ConVarRef s_Baked( "dodsp_lamps_baked" );
	if ( s_Baked.IsValid() && s_Baked.GetBool() )
		return "dodsp_lamps_baked is 1 (your choice: the baked lamps)";
	if ( !g_pMaterialSystemHardwareConfig->SupportsShaderModel_3_0() )
		return "this graphics card has no shader model 3.0";
	return NULL;
}

void CDoDSPLamps::CreateLamps()
{
	for ( int i = 0; i < m_Lamps.Count(); i++ )
	{
		Lamp_t &lamp = m_Lamps[i];
		BuildState( i, lamp );
		if ( lamp.m_hLight == CLIENTSHADOW_INVALID_HANDLE )
			lamp.m_hLight = g_pClientShadowMgr->CreateFlashlight( lamp.m_State );
		else
			g_pClientShadowMgr->UpdateFlashlightState( lamp.m_hLight, lamp.m_State );
		if ( lamp.m_hLight != CLIENTSHADOW_INVALID_HANDLE )
			g_pClientShadowMgr->SetFlashlightLightWorld( lamp.m_hLight, true );
	}
	m_bLive = true;
}

void CDoDSPLamps::DestroyLamps()
{
	for ( int i = 0; i < m_Lamps.Count(); i++ )
	{
		if ( m_Lamps[i].m_hLight != CLIENTSHADOW_INVALID_HANDLE )
			g_pClientShadowMgr->DestroyFlashlight( m_Lamps[i].m_hLight );
		m_Lamps[i].m_hLight = CLIENTSHADOW_INVALID_HANDLE;
		if ( m_Lamps[i].m_Cookie.IsValid() )
		{
			m_Lamps[i].m_Cookie->SetTextureRegenerator( NULL );	// deletes the regenerator
			m_Lamps[i].m_Cookie.Shutdown();
		}
	}
	m_bLive = false;
}

void CDoDSPLamps::LevelInitPostEntity()
{
	DestroyLamps();
	m_bFallbackWarned = false;
	if ( !Load() )
	{
		Msg( "DOD-SP lamps: no live lamps on this map (mark a light_spot with \"_dodsp_live\" \"1\" and compile with this build's VRAD)\n" );
		return;
	}
	const char *pszWhy = WhyNotLive();
	if ( pszWhy )
		Msg( "DOD-SP lamps: %d live lamp%s from %s - shown baked: %s\n", m_Lamps.Count(), m_Lamps.Count() == 1 ? "" : "s", m_szFile, pszWhy );
	else
		Msg( "DOD-SP lamps: %d live lamp%s from %s, drawn live with real-time shadows (dodsp_lamps lists them)\n",
			 m_Lamps.Count(), m_Lamps.Count() == 1 ? "" : "s", m_szFile );
}

// The one writer of the server's dodsp_lamps_fallback
void CDoDSPLamps::SetFallback( bool bFallback )
{
	ConVarRef fallback( "dodsp_lamps_fallback" );	// looked up each time: the server DLL may load after us
	if ( !fallback.IsValid() )
	{
		if ( !m_bFallbackWarned )
			Warning( "INVARIANT: DOD-SP lamps: the server has no dodsp_lamps_fallback - client.dll and server.dll must come from one build\n" );
		m_bFallbackWarned = true;
		return;
	}
	if ( fallback.GetBool() != bFallback )
		fallback.SetValue( bFallback ? 1 : 0 );
	if ( fallback.GetBool() != bFallback && !m_bFallbackWarned )
	{
		Warning( "INVARIANT: DOD-SP lamps: dodsp_lamps_fallback didn't take %d - the lamps may show twice or not at all\n", bFallback ? 1 : 0 );
		m_bFallbackWarned = true;
	}
}

void CDoDSPLamps::PreRender()
{
	const char *pszWhy = WhyNotLive();
	const bool bLive = ( pszWhy == NULL );
	if ( bLive && !m_bLive )
		CreateLamps();
	else if ( !bLive && m_bLive )
		DestroyLamps();
	// fallback = the baked lamps show because they CAN'T draw live; the player's own choice stays dodsp_lamps_baked
	static ConVarRef s_Baked( "dodsp_lamps_baked" );
	const bool bPlayerChoseBaked = s_Baked.IsValid() && s_Baked.GetBool();
	SetFallback( m_Lamps.Count() > 0 && !bLive && !bPlayerChoseBaked );

	if ( !m_bLive )
		return;
	// the engine works out which surfaces and models each lamp reaches from these, every frame
	// (moving things walk in and out of the cone)
	const bool bShadows = dodsp_lamps_shadows.GetBool();
	for ( int i = 0; i < m_Lamps.Count(); i++ )
	{
		Lamp_t &lamp = m_Lamps[i];
		if ( lamp.m_hLight == CLIENTSHADOW_INVALID_HANDLE )
			continue;
		if ( lamp.m_State.m_bEnableShadows != bShadows )
		{
			lamp.m_State.m_bEnableShadows = bShadows;
			g_pClientShadowMgr->UpdateFlashlightState( lamp.m_hLight, lamp.m_State );
		}
		g_pClientShadowMgr->UpdateProjectedTexture( lamp.m_hLight, true );
	}
}

void CDoDSPLamps::Report() const
{
	if ( !m_Lamps.Count() )
	{
		Msg( "DOD-SP lamps: none on this map\n" );
		return;
	}
	const char *pszWhy = WhyNotLive();
	Msg( "DOD-SP lamps: %d from %s, %s\n", m_Lamps.Count(), m_szFile, pszWhy ? pszWhy : "drawn live" );
	for ( int i = 0; i < m_Lamps.Count(); i++ )
	{
		const Lamp_t &lamp = m_Lamps[i];
		const DoDSPLampData_t &d = lamp.m_Data;
		Msg( "  lamp %d at %.0f %.0f %.0f: cone %.0f/%.0f deg, range %.0f, colour %.3f %.3f %.3f, distance falloff %s%s%s\n", i,
			 d.m_vecOrigin.x, d.m_vecOrigin.y, d.m_vecOrigin.z,
			 2.0f * RAD2DEG( acosf( clamp( d.m_flStopDot, -1.0f, 1.0f ) ) ), 2.0f * RAD2DEG( acosf( clamp( d.m_flStopDot2, -1.0f, 1.0f ) ) ),
			 lamp.m_flRange, d.m_vecIntensity.x, d.m_vecIntensity.y, d.m_vecIntensity.z,
			 lamp.m_bApprox ? "APPROXIMATE (mixed constant/linear/quadratic terms)" : "exact",
			 lamp.m_bFovClipped ? ", cone CLIPPED to 170 degrees" : "",
			 ( lamp.m_hLight == CLIENTSHADOW_INVALID_HANDLE && !pszWhy ) ? ", NOT CREATED" : "" );
	}
}

CON_COMMAND( dodsp_lamps, "DOD-SP: list this map's live lamps and how each is drawn" )
{
	s_DoDSPLamps.Report();
}
