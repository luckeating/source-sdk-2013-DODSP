//===================== DOD-SP prop light check =====================//
//
// Owner's report 2026-11-17: in the shade, props look darker than the room around them ("black holes").
// A moving or physics prop is lit by an AMBIENT CUBE (six colours, one per direction) that VRAD baked
// for the spot it stands in, plus up to a few lamps; the room's walls and floor are lit by their
// lightmaps. If the two disagree, either VRAD's ambient samples are wrong, or the engine/shader loses
// light on the way. This command measures instead of guessing:
//
//   dodsp_prop_light   (aim at a prop) - ONE console line:
//     the prop, its class and the shader its first material draws with
//     "facing you": the ambient cube in the direction of the viewer, and what lamps add on top
//     the cube's up / down / sides
//     the floor's lightmap right under the prop (the same lighting the room's surfaces get)
//     prop/floor: the ratio of the two (about 1 = the engine hands the prop the room's light, so any
//     darkness on screen comes later, in the shader or the material; well below 1 = the baked
//     ambient light itself is dark)
//
// Second version (2026-11-18): the first one sampled at the prop's bounding-box centre and printed "?" for
// the shader. The owner then showed chair2 drawing near-black in the shade while sunlit fine, next to boxes
// that look right - so it also prints:
//     at the engine's own lighting origin (the model's $illumposition / its attachment, through the
//     prop's transform): the light there, how high above the floor it sits, and whether it is in solid
//     every material of the model: the shader it draws with and the features that pick a shader path
//     (bump, phong, halflambert, lightwarp, selfillum, envmap)
// Values are 0..255 screen brightness (gamma 2.2 of the linear light), the ratio is linear.
// Static props are not covered: VRAD bakes their light per vertex (-StaticPropLighting), there is no
// cube to read. The per-pixel sun is not in any of these numbers (it's added in the shader), so
// measure props in the SHADE.
//
//=============================================================================//

#include "cbase.h"
#include "c_baseplayer.h"
#include "c_baseanimating.h"
#include "studio.h"
#include "model_types.h"	// mod_studio
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"	// IMaterialVar is only forward-declared in imaterial.h

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{
	float DoDSP_Screen( float flLinear )
	{
		return 255.0f * powf( clamp( flLinear, 0.0f, 1.0f ), 1.0f / 2.2f );
	}

	float DoDSP_Lum( const Vector &c )
	{
		return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
	}

	// The cube's light in direction n, the way the model shaders read it (squared-normal weights)
	// Cube order: +x, -x, +y, -y, +z, -z
	Vector DoDSP_CubeLight( const Vector *pCube, const Vector &n )
	{
		const Vector sq( n.x * n.x, n.y * n.y, n.z * n.z );
		return pCube[n.x >= 0.0f ? 0 : 1] * sq.x + pCube[n.y >= 0.0f ? 2 : 3] * sq.y + pCube[n.z >= 0.0f ? 4 : 5] * sq.z;
	}

	bool DoDSP_VarOn( IMaterial *pMat, const char *pszVar, bool bTexture )
	{
		bool bFound = false;
		IMaterialVar *pVar = pMat->FindVar( pszVar, &bFound, false );
		if ( !bFound || !pVar || !pVar->IsDefined() )
			return false;
		return bTexture ? true : pVar->GetIntValue() != 0;
	}

	// Every material of the model: "name[shader bump phong ...]", appended to buf
	void DoDSP_DescribeMaterials( const studiohdr_t *pS, char *buf, int nBuf )
	{
		buf[0] = 0;
		for ( int t = 0; t < pS->numtextures; t++ )
		{
			IMaterial *pMat = NULL;
			for ( int c = 0; c < pS->numcdtextures && IsErrorMaterial( pMat ); c++ )
			{
				char szPath[MAX_PATH];
				Q_snprintf( szPath, sizeof( szPath ), "%s%s", pS->pCdtexture( c ), pS->pTexture( t )->pszName() );
				pMat = materials->FindMaterial( szPath, TEXTURE_GROUP_MODEL, false );
			}
			char szOne[256];
			if ( IsErrorMaterial( pMat ) )
				Q_snprintf( szOne, sizeof( szOne ), "%s%s[missing]", t ? " " : "", pS->pTexture( t )->pszName() );
			else
				Q_snprintf( szOne, sizeof( szOne ), "%s%s[%s%s%s%s%s%s%s]", t ? " " : "", pS->pTexture( t )->pszName(), pMat->GetShaderName(),
							DoDSP_VarOn( pMat, "$bumpmap", true ) ? " bump" : "",
							DoDSP_VarOn( pMat, "$phong", false ) ? " phong" : "",
							DoDSP_VarOn( pMat, "$halflambert", false ) ? " halflambert" : "",
							DoDSP_VarOn( pMat, "$lightwarptexture", true ) ? " lightwarp" : "",
							DoDSP_VarOn( pMat, "$selfillum", false ) ? " selfillum" : "",
							DoDSP_VarOn( pMat, "$envmap", true ) ? " envmap" : "" );
			Q_strncat( buf, szOne, nBuf, COPY_ALL_CHARACTERS );
		}
	}
}

CON_COMMAND( dodsp_prop_light, "DOD-SP: the light the engine gives the prop under the crosshair, next to the floor's lightmap" )
{
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return;

	Vector vecEye = pPlayer->EyePosition();
	Vector vecForward;
	AngleVectors( pPlayer->EyeAngles(), &vecForward );
	trace_t tr;
	UTIL_TraceLine( vecEye, vecEye + vecForward * 4096.0f, MASK_SHOT, pPlayer, COLLISION_GROUP_NONE, &tr );

	C_BaseEntity *pEnt = tr.m_pEnt;
	const model_t *pModel = pEnt ? pEnt->GetModel() : NULL;
	if ( tr.fraction >= 1.0f || !pEnt || pEnt->IsWorld() || !pModel || modelinfo->GetModelType( pModel ) != mod_studio )
	{
		Msg( "DOD-SP prop light: no moving or physics prop under the crosshair (static props are baked per vertex by VRAD - this can't read them)\n" );
		return;
	}

	// what the prop is and which shaders draw it
	C_BaseAnimating *pAnim = pEnt->GetBaseAnimating();
	CStudioHdr *pHdr = pAnim ? pAnim->GetModelPtr() : NULL;
	const studiohdr_t *pStudio = pHdr ? pHdr->GetRenderHdr() : NULL;
	char szMaterials[1024];
	Q_strncpy( szMaterials, "?", sizeof( szMaterials ) );
	if ( pStudio )
		DoDSP_DescribeMaterials( pStudio, szMaterials, sizeof( szMaterials ) );
	ClientClass *pClass = pEnt->GetClientClass();
	const char *pszClass = ( pClass && pClass->m_pNetworkName ) ? pClass->m_pNetworkName : "?";

	// the engine's lighting at the prop's centre: ambient cube + lamps, towards the viewer
	const Vector vecCentre = pEnt->WorldSpaceCenter();
	Vector vecToEye = vecEye - vecCentre;
	VectorNormalize( vecToEye );
	Vector vecTotal( 0, 0, 0 );
	Vector cube[6];
	for ( int i = 0; i < 6; i++ )
		cube[i].Init();
	engine->ComputeLighting( vecCentre, &vecToEye, false, vecTotal, cube );

	const Vector vecAmbient = DoDSP_CubeLight( cube, vecToEye );
	const float flAmbient = DoDSP_Lum( vecAmbient );
	const float flLamps = MAX( DoDSP_Lum( vecTotal ) - flAmbient, 0.0f );
	const float flSides = 0.25f * ( DoDSP_Lum( cube[0] ) + DoDSP_Lum( cube[1] ) + DoDSP_Lum( cube[2] ) + DoDSP_Lum( cube[3] ) );

	// the room: the floor's lightmap straight under the prop (light styles as they are now)
	const float flFloor = DoDSP_Lum( engine->GetLightForPoint( vecCentre, false ) );

	// where the ENGINE samples the prop's light: the model's illumination position through its transform
	// (or its illumination attachment), not the box centre
	Vector vecIllum = vecCentre;
	const char *pszIllum = "box centre";
	if ( pStudio && pAnim )
	{
		matrix3x4_t mat;
		const int nAttach = pStudio->IllumPositionAttachmentIndex();
		if ( nAttach > 0 && pAnim->GetAttachment( nAttach, mat ) )
		{
			MatrixGetColumn( mat, 3, vecIllum );
			pszIllum = "attachment";
		}
		else
		{
			VectorTransform( pStudio->illumposition, pEnt->EntityToWorldTransform(), vecIllum );
			pszIllum = "$illumposition";
		}
	}
	Vector vecIllumTotal( 0, 0, 0 );
	Vector cubeIllum[6];
	for ( int i = 0; i < 6; i++ )
		cubeIllum[i].Init();
	engine->ComputeLighting( vecIllum, &vecToEye, false, vecIllumTotal, cubeIllum );
	const float flIllumAmbient = DoDSP_Lum( DoDSP_CubeLight( cubeIllum, vecToEye ) );
	const float flIllumLamps = MAX( DoDSP_Lum( vecIllumTotal ) - flIllumAmbient, 0.0f );
	const bool bInSolid = ( enginetrace->GetPointContents( vecIllum ) & CONTENTS_SOLID ) != 0;
	trace_t trDown;
	UTIL_TraceLine( vecIllum + Vector( 0, 0, 1 ), vecIllum - Vector( 0, 0, 512 ), MASK_SOLID_BRUSHONLY, pEnt, COLLISION_GROUP_NONE, &trDown );
	const float flAboveFloor = ( vecIllum.z + 1.0f ) - trDown.endpos.z - 1.0f;

	static ConVarRef s_SunBaked( "dodsp_sun_baked" );
	Msg( "DOD-SP prop light: %s (%s) | at box centre: ambient %.0f + lamps %.0f, cube up %.0f down %.0f sides %.0f | "
		 "at the engine's origin (%s, %.0f units above the floor%s): ambient %.0f + lamps %.0f | floor under it %.0f | prop/floor %.2f | "
		 "materials: %s | sun %s\n",
		 modelinfo->GetModelName( pModel ), pszClass,
		 DoDSP_Screen( flAmbient ), DoDSP_Screen( flLamps ),
		 DoDSP_Screen( DoDSP_Lum( cube[4] ) ), DoDSP_Screen( DoDSP_Lum( cube[5] ) ), DoDSP_Screen( flSides ),
		 pszIllum, flAboveFloor, bInSolid ? ", INSIDE SOLID" : "", DoDSP_Screen( flIllumAmbient ), DoDSP_Screen( flIllumLamps ),
		 DoDSP_Screen( flFloor ),
		 flFloor > 1e-5f ? ( flAmbient + flLamps ) / flFloor : 0.0f,
		 szMaterials,
		 ( s_SunBaked.IsValid() && !s_SunBaked.GetBool() ) ? "per-pixel" : "baked" );
}
