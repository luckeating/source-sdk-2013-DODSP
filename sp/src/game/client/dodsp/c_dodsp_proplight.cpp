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
// The per-pixel sun is not in any of these numbers (it's added in the shader), so measure props in the SHADE.
//
// Third version (2026-11-18): the owner found the black surfaces are the ones FACING UP (chair seats,
// table and counter tops) on moving props AND static props (cafe_bar1, VRAD's per-vertex bake), on our
// shaders and on Mapbase's. So:
//   static props: the engine's light at the point you aim at (GetStaticPropMaterialColorAndLighting:
//     what the engine itself thinks that spot of the prop is lit with) next to the floor where you stand
//   dodsp_prop_materials: every shader parameter of every material on the prop under the crosshair,
//     written to DOD-SP\dodsp_prop_materials.txt - to compare a black prop's materials with a good one's
//
//=============================================================================//

#include "cbase.h"
#include "c_baseplayer.h"
#include "c_baseanimating.h"
#include "studio.h"
#include "model_types.h"	// mod_studio
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"	// IMaterialVar is only forward-declared in imaterial.h
#include "engine/IStaticPropMgr.h"
#include "filesystem.h"

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

	// What the crosshair is on: a moving prop (pEnt set) or a static prop (*pbStatic), and its model
	const model_t *DoDSP_PropUnderCrosshair( trace_t &tr, C_BaseEntity **ppEnt, bool *pbStatic )
	{
		*ppEnt = NULL;
		*pbStatic = false;
		C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
		if ( !pPlayer )
			return NULL;
		Vector vecEye = pPlayer->EyePosition();
		Vector vecForward;
		AngleVectors( pPlayer->EyeAngles(), &vecForward );
		UTIL_TraceLine( vecEye, vecEye + vecForward * 4096.0f, MASK_SHOT, pPlayer, COLLISION_GROUP_NONE, &tr );
		if ( tr.fraction >= 1.0f )
			return NULL;
		if ( tr.DidHitWorld() && tr.hitbox > 0 )
		{
			// a static prop: find its collideable at the hit point for the model
			*pbStatic = true;
			CUtlVector< ICollideable * > props;
			staticpropmgr->GetAllStaticPropsInAABB( tr.endpos - Vector( 1, 1, 1 ), tr.endpos + Vector( 1, 1, 1 ), &props );
			for ( int i = 0; i < props.Count(); i++ )
			{
				const model_t *pModel = props[i] ? props[i]->GetCollisionModel() : NULL;
				if ( pModel && modelinfo->GetModelType( pModel ) == mod_studio )
					return pModel;
			}
			return NULL;
		}
		C_BaseEntity *pEnt = tr.m_pEnt;
		const model_t *pModel = pEnt ? pEnt->GetModel() : NULL;
		if ( !pEnt || pEnt->IsWorld() || !pModel || modelinfo->GetModelType( pModel ) != mod_studio )
			return NULL;
		*ppEnt = pEnt;
		return pModel;
	}
}

//-----------------------------------------------------------------------------
// Every shader parameter of every material of the prop under the crosshair, to a file
//-----------------------------------------------------------------------------
CON_COMMAND( dodsp_prop_materials, "DOD-SP: write every material setting of the prop under the crosshair to DOD-SP\\dodsp_prop_materials.txt" )
{
	trace_t tr;
	C_BaseEntity *pEnt = NULL;
	bool bStatic = false;
	const model_t *pModel = DoDSP_PropUnderCrosshair( tr, &pEnt, &bStatic );
	const studiohdr_t *pStudio = pModel ? modelinfo->GetStudiomodel( pModel ) : NULL;
	if ( !pStudio )
	{
		Msg( "DOD-SP prop materials: no prop under the crosshair\n" );
		return;
	}
	FileHandle_t h = g_pFullFileSystem->Open( "dodsp_prop_materials.txt", "a", "MOD" );
	if ( !h )
	{
		Warning( "DOD-SP prop materials: can't write dodsp_prop_materials.txt\n" );
		return;
	}
	g_pFullFileSystem->FPrintf( h, "==== %s (%s) ====\n", modelinfo->GetModelName( pModel ), bStatic ? "static prop" : "moving prop" );
	for ( int t = 0; t < pStudio->numtextures; t++ )
	{
		IMaterial *pMat = NULL;
		for ( int c = 0; c < pStudio->numcdtextures && IsErrorMaterial( pMat ); c++ )
		{
			char szPath[MAX_PATH];
			Q_snprintf( szPath, sizeof( szPath ), "%s%s", pStudio->pCdtexture( c ), pStudio->pTexture( t )->pszName() );
			pMat = materials->FindMaterial( szPath, TEXTURE_GROUP_MODEL, false );
		}
		if ( IsErrorMaterial( pMat ) )
		{
			g_pFullFileSystem->FPrintf( h, "material %s: missing\n", pStudio->pTexture( t )->pszName() );
			continue;
		}
		g_pFullFileSystem->FPrintf( h, "material %s  shader %s\n", pMat->GetName(), pMat->GetShaderName() );
		const int nParams = pMat->ShaderParamCount();
		IMaterialVar **ppParams = pMat->GetShaderParams();
		for ( int i = 0; i < nParams; i++ )
		{
			if ( !ppParams[i] || !ppParams[i]->IsDefined() )
				continue;
			const char *pszValue = ppParams[i]->GetStringValue();
			g_pFullFileSystem->FPrintf( h, "    %s = %s\n", ppParams[i]->GetName(), pszValue ? pszValue : "" );
		}
	}
	g_pFullFileSystem->Close( h );
	Msg( "DOD-SP prop materials: %s written to DOD-SP\\dodsp_prop_materials.txt (%d materials)\n", modelinfo->GetModelName( pModel ), pStudio->numtextures );
}

CON_COMMAND( dodsp_prop_light, "DOD-SP: the light the engine gives the prop under the crosshair, next to the floor's lightmap" )
{
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return;
	const Vector vecEye = pPlayer->EyePosition();

	trace_t tr;
	C_BaseEntity *pEnt = NULL;
	bool bStatic = false;
	const model_t *pModel = DoDSP_PropUnderCrosshair( tr, &pEnt, &bStatic );

	// the floor where you stand: its light and its material colour, to compare against
	Vector vecFloorLight( 0, 0, 0 ), vecFloorColour( 0, 0, 0 );
	engine->TraceLineMaterialAndLighting( vecEye, vecEye - Vector( 0, 0, 512 ), vecFloorLight, vecFloorColour );

	if ( bStatic )
	{
		// the engine's own light at the aimed spot of the static prop (VRAD's per-vertex bake)
		Vector vecLight( 0, 0, 0 ), vecColour( 0, 0, 0 );
		staticpropmgr->GetStaticPropMaterialColorAndLighting( &tr, tr.hitbox - 1, vecLight, vecColour );
		const char *pszFacing = tr.plane.normal.z > 0.7f ? "facing up" : ( tr.plane.normal.z < -0.7f ? "facing down" : "facing sideways" );
		Msg( "DOD-SP prop light: static prop %s | the spot you aim at (%s): light %.0f, material colour %.0f | "
			 "floor where you stand: light %.0f, material colour %.0f | prop/floor light %.2f\n",
			 pModel ? modelinfo->GetModelName( pModel ) : "?", pszFacing,
			 DoDSP_Screen( DoDSP_Lum( vecLight ) ), 255.0f * DoDSP_Lum( vecColour ),
			 DoDSP_Screen( DoDSP_Lum( vecFloorLight ) ), 255.0f * DoDSP_Lum( vecFloorColour ),
			 DoDSP_Lum( vecFloorLight ) > 1e-6f ? DoDSP_Lum( vecLight ) / DoDSP_Lum( vecFloorLight ) : 0.0f );
		return;
	}
	if ( !pEnt )
	{
		Msg( "DOD-SP prop light: no prop under the crosshair\n" );
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
			VectorTransform( pStudio->illumposition, mat, vecIllum );	// as the engine does
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

	// what DOD-SP hands the engine instead (c_dodsp_lightorigin.cpp), if anything
	char szFix[160];
	Q_strncpy( szFix, "DOD-SP keeps the model's own point", sizeof( szFix ) );
	if ( pAnim )
	{
		const char *pszWhy = "";
		const Vector *pFix = pAnim->DoDSP_LightingOrigin( pEnt->EntityToWorldTransform(), &pszWhy );
		if ( pFix )
		{
			Vector vecFixTotal( 0, 0, 0 );
			Vector cubeFix[6];
			for ( int i = 0; i < 6; i++ )
				cubeFix[i].Init();
			engine->ComputeLighting( *pFix, &vecToEye, false, vecFixTotal, cubeFix );
			Q_snprintf( szFix, sizeof( szFix ), "DOD-SP lights it from its middle (%s): ambient %.0f", pszWhy,
						DoDSP_Screen( DoDSP_Lum( DoDSP_CubeLight( cubeFix, vecToEye ) ) ) );
		}
	}

	static ConVarRef s_SunBaked( "dodsp_sun_baked" );
	Msg( "DOD-SP prop light: %s (%s) | at box centre: ambient %.0f + lamps %.0f, cube up %.0f down %.0f sides %.0f | "
		 "at the model's own lighting point (%s, %.0f units above the floor%s): ambient %.0f + lamps %.0f | %s | floor under it %.0f | prop/floor %.2f | "
		 "materials: %s | sun %s\n",
		 modelinfo->GetModelName( pModel ), pszClass,
		 DoDSP_Screen( flAmbient ), DoDSP_Screen( flLamps ),
		 DoDSP_Screen( DoDSP_Lum( cube[4] ) ), DoDSP_Screen( DoDSP_Lum( cube[5] ) ), DoDSP_Screen( flSides ),
		 pszIllum, flAboveFloor, bInSolid ? ", INSIDE SOLID" : "", DoDSP_Screen( flIllumAmbient ), DoDSP_Screen( flIllumLamps ), szFix,
		 DoDSP_Screen( flFloor ),
		 flFloor > 1e-5f ? ( flAmbient + flLamps ) / flFloor : 0.0f,
		 szMaterials,
		 ( s_SunBaked.IsValid() && !s_SunBaked.GetBool() ) ? "per-pixel" : "baked" );
}
