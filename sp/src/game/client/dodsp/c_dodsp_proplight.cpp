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
// Values are 0..255 screen brightness (gamma 2.2 of the linear light), the ratio is linear.
// Static props are not covered: VRAD bakes their light per vertex (-StaticPropLighting), there is no
// cube to read. The per-pixel sun is not in any of these numbers (it's added in the shader), so
// measure props in the SHADE.
//
//=============================================================================//

#include "cbase.h"
#include "c_baseplayer.h"
#include "materialsystem/imaterial.h"

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

	// what the prop is and which shader draws it
	const char *pszShader = "?";
	if ( modelinfo->GetModelMaterialCount( pModel ) > 0 )
	{
		IMaterial *pMat = NULL;
		modelinfo->GetModelMaterials( pModel, 1, &pMat );
		if ( pMat && pMat->GetShaderName() )
			pszShader = pMat->GetShaderName();
	}
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

	static ConVarRef s_SunBaked( "dodsp_sun_baked" );
	Msg( "DOD-SP prop light: %s (%s, %s) | facing you: ambient %.0f + lamps %.0f | cube up %.0f down %.0f sides %.0f | "
		 "floor under it %.0f | prop/floor %.2f | sun %s\n",
		 modelinfo->GetModelName( pModel ), pszClass, pszShader,
		 DoDSP_Screen( flAmbient ), DoDSP_Screen( flLamps ),
		 DoDSP_Screen( DoDSP_Lum( cube[4] ) ), DoDSP_Screen( DoDSP_Lum( cube[5] ) ), DoDSP_Screen( flSides ),
		 DoDSP_Screen( flFloor ),
		 flFloor > 1e-5f ? ( flAmbient + flLamps ) / flFloor : 0.0f,
		 ( s_SunBaked.IsValid() && !s_SunBaked.GetBool() ) ? "per-pixel" : "baked" );
}
