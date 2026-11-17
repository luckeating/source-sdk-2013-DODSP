//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//
#include "cbase.h"
#include "model_types.h"
#include "vcollide.h"
#include "vcollide_parse.h"
#include "solidsetdefaults.h"
#include "bone_setup.h"
#include "engine/ivmodelinfo.h"
#include "physics.h"
#include "view.h"
#include "clienteffectprecachesystem.h"
#include "c_physicsprop.h"
#include "tier0/vprof.h"
#include "ivrenderview.h"
#include "dodsp/c_dodsp_sdkshaders.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IMPLEMENT_CLIENTCLASS_DT(C_PhysicsProp, DT_PhysicsProp, CPhysicsProp)
	RecvPropBool( RECVINFO( m_bAwake ) ),
END_RECV_TABLE()

ConVar r_PhysPropStaticLighting( "r_PhysPropStaticLighting", "1" );


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
C_PhysicsProp::C_PhysicsProp( void )
{
	m_pPhysicsObject = NULL;
	m_takedamage = DAMAGE_YES;

	// default true so static lighting will get recomputed when we go to sleep
	m_bAwakeLastTime = true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
C_PhysicsProp::~C_PhysicsProp( void )
{
}


// @MULTICORE (toml 9/18/2006): this visualization will need to be implemented elsewhere
ConVar r_visualizeproplightcaching( "r_visualizeproplightcaching", "0" );

//-----------------------------------------------------------------------------
// Purpose: Draws the object
// Input  : flags - 
//-----------------------------------------------------------------------------
bool C_PhysicsProp::OnInternalDrawModel( ClientModelRenderInfo_t *pInfo )
{
	CreateModelInstance();

	// DOD-SP. INVARIANT: no model's lighting depends on a runtime bake while DOD-SP's sun shaders are
	// in use. The bake (RecomputeStaticLighting, done once as the prop falls asleep) reads the sun's
	// light style at that moment: baked with dodsp_sun_baked 0 the prop has no sun at all, and the
	// STATIC_LIGHT model shader gets no per-pixel sun either, so a bumped prop went dark once it
	// settled. Baked with dodsp_sun_baked 1 it stays stale after the switch. Sleeping props are lit
	// like moving ones instead (the engine's live lighting + the per-pixel sun), so nothing changes
	// when a prop settles. CI (devtools/dodsp_lint_client.py) fails the build on any other use of
	// static lighting in the client that doesn't carry this check.
	const bool bStaticLighting = r_PhysPropStaticLighting.GetBool() && !DoDSP_SunShadersInUse();	// dodsp-lint: static lighting guarded

	if ( bStaticLighting && m_bAwakeLastTime != m_bAwake )
	{
		if ( m_bAwakeLastTime && !m_bAwake )
		{
			// transition to sleep, bake lighting now, once
			if ( !modelrender->RecomputeStaticLighting( GetModelInstance() ) )	// dodsp-lint: static lighting guarded
			{
				// not valid for drawing
				return false;
			}

			if ( r_visualizeproplightcaching.GetBool() )
			{
				float color[] = { 0.0f, 1.0f, 0.0f, 1.0f };
				render->SetColorModulation( color );
			}
		}
		else if ( r_visualizeproplightcaching.GetBool() )
		{
			float color[] = { 1.0f, 0.0f, 0.0f, 1.0f };
			render->SetColorModulation( color );
		}
	}

	if ( !m_bAwake && bStaticLighting )
	{
		// going to sleep, have static lighting
		pInfo->flags |= STUDIO_STATIC_LIGHTING;	// dodsp-lint: static lighting guarded
	}
	
	// track state. Without static lighting, stay "awake" so that if it comes back on, the prop bakes
	// afresh when it next sleeps instead of drawing an old bake.
	m_bAwakeLastTime = bStaticLighting ? m_bAwake : true;

	return true;
}
