//=============================================================================//
//
// DOD-SP: German soldier NPC.
//
// A Combine soldier (npc_combine_s) brain driving the Day of Defeat: Source
// player animations (models/player/dod_player_shared.mdl).
//
// Stage 2a:
//   - Combine activities are translated to DoD activities through a data file,
//     scripts/npc_german_soldier.txt, so animation choices can be tuned without
//     rebuilding (edit, then "npc_german_reload_script" + respawn).
//   - Aiming drives DoD's body_pitch / body_yaw instead of aim_pitch / aim_yaw.
//   - DoD locomotion is a 9-way move_x / move_y blend; we hold it at "forward".
//   - DoD shoot / reload animations are delta gestures with no fire or reload
//     events, so shooting and reloading are timed in code and the DoD gesture
//     is layered on top.
//   - Grenades and the kick are off until those animations exist (later stage).
//
//=============================================================================//

#include "cbase.h"
#include "npc_combines.h"
#include "activitylist.h"
#include "animation.h"
#include "npcevent.h"
#include "filesystem.h"
#include "KeyValues.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define GERMAN_SOLDIER_DEFAULT_MODEL	"models/german/npc_german_rifleman.mdl"
#define GERMAN_SOLDIER_SCRIPT			"scripts/npc_german_soldier.txt"
#define GERMAN_WEAPON_TOKEN				"*"		// replaced by the weapon's DoD animation set (RIFLE, MP40, ...)

ConVar npc_german_debug( "npc_german_debug", "0", 0, "1 = print German soldier activity translations and missing animations." );

//-----------------------------------------------------------------------------
// Shared script data (one copy for all German soldiers)
//-----------------------------------------------------------------------------
static KeyValues	*g_pGermanScript = NULL;
static int			g_iGermanScriptVersion = 0;

static void GermanSoldier_LoadScript()
{
	if ( g_pGermanScript )
	{
		g_pGermanScript->deleteThis();
	}

	g_pGermanScript = new KeyValues( "npc_german_soldier" );
	if ( !g_pGermanScript->LoadFromFile( filesystem, GERMAN_SOLDIER_SCRIPT, "GAME" ) )
	{
		Warning( "npc_german_soldier: could not load %s - German soldiers will have no animations!\n", GERMAN_SOLDIER_SCRIPT );
	}

	g_iGermanScriptVersion++;
}

CON_COMMAND( npc_german_reload_script, "Reload scripts/npc_german_soldier.txt; German soldiers pick up the changes immediately." )
{
	GermanSoldier_LoadScript();
	Msg( "npc_german_soldier: reloaded %s\n", GERMAN_SOLDIER_SCRIPT );
}

//-----------------------------------------------------------------------------
// The NPC
//-----------------------------------------------------------------------------
class CNPC_GermanSoldier : public CNPC_CombineS
{
	DECLARE_CLASS( CNPC_GermanSoldier, CNPC_CombineS );

public:
	CNPC_GermanSoldier();

	void		Precache( void );
	void		Spawn( void );

	Activity	NPC_TranslateActivity( Activity eNewActivity );
	void		OnChangeActivity( Activity eNewActivity );

	void		StartTask( const Task_t *pTask );
	void		RunTask( const Task_t *pTask );

	int			MeleeAttack1Conditions( float flDot, float flDist );

	void		SetAim( const Vector &aimDir );
	void		RelaxAim( void );
	void		MaintainLookTargets( float flInterval );

protected:
	void		PopulatePoseParameters( void );

private:
	void		EnsureAnimMap( void );
	void		BuildAnimMap( void );
	Activity	ResolveActivity( const char *pszName, const char *pszWeaponSet );
	void		FlipAimSigns( void );
	void		HoldMoveForward( void );
	bool		FireOneShot( void );

	// Animation map (rebuilt when the script reloads or the weapon changes)
	CUtlMap<int, int>	m_AnimMap;
	int					m_iAnimMapVersion;
	string_t			m_iszAnimMapWeapon;
	Activity			m_actFireGesture;
	Activity			m_actReloadGesture;
	Activity			m_actReloadCrouchGesture;
	int					m_iFireEvent;
	float				m_flPitchSign;
	float				m_flYawSign;
	Activity			m_actLastDebug;

	// DoD pose parameters
	int					m_poseMoveX;
	int					m_poseMoveY;

	// Shooting / reloading timers
	int					m_nShotsLeft;
	float				m_flShotInterval;
	float				m_flNextShotTime;
	float				m_flReloadDoneTime;
};

LINK_ENTITY_TO_CLASS( npc_german_soldier, CNPC_GermanSoldier );

CNPC_GermanSoldier::CNPC_GermanSoldier() : m_AnimMap( DefLessFunc( int ) )
{
	m_iAnimMapVersion = -1;
	m_iszAnimMapWeapon = NULL_STRING;
	m_actFireGesture = ACT_INVALID;
	m_actReloadGesture = ACT_INVALID;
	m_actReloadCrouchGesture = ACT_INVALID;
	m_iFireEvent = 0;
	m_flPitchSign = 1.0f;
	m_flYawSign = 1.0f;
	m_actLastDebug = ACT_INVALID;
	m_poseMoveX = -1;
	m_poseMoveY = -1;
	m_nShotsLeft = 0;
	m_flShotInterval = 0.1f;
	m_flNextShotTime = 0.0f;
	m_flReloadDoneTime = 0.0f;
}

void CNPC_GermanSoldier::Precache( void )
{
	// A model set in Hammer wins; otherwise use the German rifleman.
	// Must happen before BaseClass::Precache, which would fall back to the Combine model.
	if ( !GetModelName() )
	{
		SetModelName( AllocPooledString( GERMAN_SOLDIER_DEFAULT_MODEL ) );
	}

	if ( !g_pGermanScript )
	{
		GermanSoldier_LoadScript();
	}

	BaseClass::Precache();
}

void CNPC_GermanSoldier::Spawn( void )
{
	BaseClass::Spawn();

	// No grenade-throw or kick animations yet (they need animation events). Stage 2e.
	KeyValue( "NumGrenades", "0" );

	HoldMoveForward();
}

//-----------------------------------------------------------------------------
// Pose parameters
//-----------------------------------------------------------------------------
void CNPC_GermanSoldier::PopulatePoseParameters( void )
{
	BaseClass::PopulatePoseParameters();

	// DoD aims with body_pitch / body_yaw. Point the NPC aim code at them.
	if ( m_poseAim_Pitch < 0 )
		m_poseAim_Pitch = LookupPoseParameter( "body_pitch" );
	if ( m_poseAim_Yaw < 0 )
		m_poseAim_Yaw = LookupPoseParameter( "body_yaw" );

	m_poseMoveX = LookupPoseParameter( "move_x" );
	m_poseMoveY = LookupPoseParameter( "move_y" );
}

void CNPC_GermanSoldier::HoldMoveForward( void )
{
	// DoD walk/run sequences blend 9 directions on move_x (forward) / move_y (left).
	// The NPC always turns to face where it walks, so "straight forward" is right.
	if ( m_poseMoveX >= 0 )
		SetPoseParameter( m_poseMoveX, 1.0f );
	if ( m_poseMoveY >= 0 )
		SetPoseParameter( m_poseMoveY, 0.0f );
}

// The base aim code assumes positive = the model's convention. If the script says
// a DoD axis is reversed, flip the stored pose value around the base call so the
// base smoothing still sees its own numbers.
void CNPC_GermanSoldier::FlipAimSigns( void )
{
	if ( m_flPitchSign < 0.0f && m_poseAim_Pitch >= 0 )
		SetPoseParameter( m_poseAim_Pitch, -GetPoseParameter( m_poseAim_Pitch ) );
	if ( m_flYawSign < 0.0f && m_poseAim_Yaw >= 0 )
		SetPoseParameter( m_poseAim_Yaw, -GetPoseParameter( m_poseAim_Yaw ) );
}

void CNPC_GermanSoldier::SetAim( const Vector &aimDir )
{
	EnsureAnimMap();
	FlipAimSigns();
	BaseClass::SetAim( aimDir );
	FlipAimSigns();
}

void CNPC_GermanSoldier::RelaxAim( void )
{
	EnsureAnimMap();
	FlipAimSigns();
	BaseClass::RelaxAim();
	FlipAimSigns();
}

void CNPC_GermanSoldier::MaintainLookTargets( float flInterval )
{
	// The actor code also drives a "body_yaw" pose parameter (for head/body turns) and
	// would zero our aim every think. Keep the aim value.
	float flAimYaw = ( m_poseAim_Yaw >= 0 ) ? GetPoseParameter( m_poseAim_Yaw ) : 0.0f;

	BaseClass::MaintainLookTargets( flInterval );

	if ( m_poseAim_Yaw >= 0 )
		SetPoseParameter( m_poseAim_Yaw, flAimYaw );
}

//-----------------------------------------------------------------------------
// Activity translation: Combine activity -> DoD activity (from the script)
//-----------------------------------------------------------------------------
Activity CNPC_GermanSoldier::ResolveActivity( const char *pszName, const char *pszWeaponSet )
{
	if ( !pszName || !pszName[0] )
		return ACT_INVALID;

	char szName[128];
	if ( !V_StrSubst( pszName, GERMAN_WEAPON_TOKEN, pszWeaponSet, szName, sizeof( szName ) ) )
		V_strncpy( szName, pszName, sizeof( szName ) );

	// Make sure the model's DoD activity names are registered before looking them up.
	VerifySequenceIndex( GetModelPtr() );

	int iAct = ActivityList_IndexForName( szName );
	if ( iAct < 0 || !HaveSequenceForActivity( (Activity)iAct ) )
	{
		Warning( "npc_german_soldier: no animation for \"%s\" on %s\n", szName, STRING( GetModelName() ) );
		return ACT_INVALID;
	}

	return (Activity)iAct;
}

void CNPC_GermanSoldier::EnsureAnimMap( void )
{
	if ( !GetModelPtr() )
		return;

	string_t iszWeapon = GetActiveWeapon() ? GetActiveWeapon()->m_iClassname : NULL_STRING;

	if ( m_iAnimMapVersion != g_iGermanScriptVersion || m_iszAnimMapWeapon != iszWeapon )
	{
		m_iAnimMapVersion = g_iGermanScriptVersion;
		m_iszAnimMapWeapon = iszWeapon;
		BuildAnimMap();
	}
}

void CNPC_GermanSoldier::BuildAnimMap( void )
{
	m_AnimMap.RemoveAll();
	m_actFireGesture = ACT_INVALID;
	m_actReloadGesture = ACT_INVALID;
	m_actReloadCrouchGesture = ACT_INVALID;
	m_iFireEvent = 0;
	m_actLastDebug = ACT_INVALID;

	if ( !g_pGermanScript )
		GermanSoldier_LoadScript();

	KeyValues *pScript = g_pGermanScript;

	m_flPitchSign = ( pScript->GetFloat( "body_pitch_sign", 1.0f ) < 0.0f ) ? -1.0f : 1.0f;
	m_flYawSign = ( pScript->GetFloat( "body_yaw_sign", 1.0f ) < 0.0f ) ? -1.0f : 1.0f;

	// Which DoD animation set does this weapon use?
	const char *pszWeapon = GetActiveWeapon() ? GetActiveWeapon()->GetClassname() : "none";
	KeyValues *pWeapons = pScript->FindKey( "weapons" );
	KeyValues *pWeapon = pWeapons ? pWeapons->FindKey( pszWeapon ) : NULL;
	if ( !pWeapon && pWeapons )
		pWeapon = pWeapons->FindKey( "default" );

	const char *pszSet = pWeapon ? pWeapon->GetString( "anims", "RIFLE" ) : "RIFLE";
	m_iFireEvent = pWeapon ? pWeapon->GetInt( "fire_event", 0 ) : 0;

	// Body activities
	KeyValues *pActs = pScript->FindKey( "activities" );
	if ( pActs )
	{
		for ( KeyValues *pAct = pActs->GetFirstValue(); pAct; pAct = pAct->GetNextValue() )
		{
			int iFrom = ActivityList_IndexForName( pAct->GetName() );
			if ( iFrom < 0 )
			{
				Warning( "npc_german_soldier: unknown activity \"%s\" in %s\n", pAct->GetName(), GERMAN_SOLDIER_SCRIPT );
				continue;
			}

			Activity to = ResolveActivity( pAct->GetString(), pszSet );
			if ( to != ACT_INVALID )
				m_AnimMap.InsertOrReplace( iFrom, to );
		}
	}

	// Gestures layered on top
	KeyValues *pGestures = pScript->FindKey( "gestures" );
	if ( pGestures )
	{
		m_actFireGesture = ResolveActivity( pGestures->GetString( "fire" ), pszSet );
		m_actReloadGesture = ResolveActivity( pGestures->GetString( "reload" ), pszSet );
		m_actReloadCrouchGesture = ResolveActivity( pGestures->GetString( "reload_crouch" ), pszSet );
	}

	if ( npc_german_debug.GetBool() )
	{
		Msg( "npc_german_soldier %d: weapon %s -> DoD set %s, %d activities mapped, fire event %d\n",
			entindex(), pszWeapon, pszSet, m_AnimMap.Count(), m_iFireEvent );
	}
}

Activity CNPC_GermanSoldier::NPC_TranslateActivity( Activity eNewActivity )
{
	Activity eCombine = BaseClass::NPC_TranslateActivity( eNewActivity );

	EnsureAnimMap();

	unsigned short i = m_AnimMap.Find( eCombine );
	if ( i == m_AnimMap.InvalidIndex() )
		i = m_AnimMap.Find( eNewActivity );

	if ( i != m_AnimMap.InvalidIndex() )
	{
		Activity eDoD = (Activity)m_AnimMap[i];
		if ( npc_german_debug.GetBool() && eCombine != m_actLastDebug )
		{
			m_actLastDebug = eCombine;
			Msg( "npc_german_soldier %d: %s -> %s\n", entindex(), ActivityList_NameForIndex( eCombine ), ActivityList_NameForIndex( eDoD ) );
		}
		return eDoD;
	}

	if ( npc_german_debug.GetBool() && eCombine != m_actLastDebug && !HaveSequenceForActivity( eCombine ) )
	{
		m_actLastDebug = eCombine;
		Msg( "npc_german_soldier %d: NO MAPPING for %s (add it to %s)\n", entindex(), ActivityList_NameForIndex( eCombine ), GERMAN_SOLDIER_SCRIPT );
	}

	return eCombine;
}

void CNPC_GermanSoldier::OnChangeActivity( Activity eNewActivity )
{
	BaseClass::OnChangeActivity( eNewActivity );
	HoldMoveForward();
}

//-----------------------------------------------------------------------------
// Shooting and reloading
//-----------------------------------------------------------------------------
bool CNPC_GermanSoldier::FireOneShot( void )
{
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	if ( !pWeapon )
		return false;

	if ( pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() <= 0 )
		return false;

	if ( m_iFireEvent > 0 )
	{
		// Same path a Combine shoot animation's fire event takes: aimed at the enemy.
		animevent_t event;
		event.event = m_iFireEvent;
		event.options = "";
		event.cycle = 0.0f;
		event.eventtime = gpGlobals->curtime;
		event.type = 0;
		event.pSource = this;
		pWeapon->Operator_HandleAnimEvent( &event, this );
	}
	else
	{
		pWeapon->Operator_ForceNPCFire( this, false );
	}

	if ( m_actFireGesture != ACT_INVALID )
		RestartGesture( m_actFireGesture );

	return true;
}

void CNPC_GermanSoldier::StartTask( const Task_t *pTask )
{
	switch ( pTask->iTask )
	{
	case TASK_RANGE_ATTACK1:
		{
			BaseClass::StartTask( pTask );	// Combine bookkeeping + aim pose

			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			if ( pWeapon && !TaskIsComplete() )
			{
				m_nShotsLeft = MAX( 1, pWeapon->GetRandomBurst() );
				m_flShotInterval = pWeapon->GetFireRate();
				m_flNextShotTime = gpGlobals->curtime + 0.15f;	// settle the aim first
			}
		}
		break;

	case TASK_RELOAD:
		{
			EnsureAnimMap();

			bool bCrouched = IsCrouching() || TranslateCrouchActivity( ACT_RELOAD ) == ACT_RELOAD_LOW;
			Activity gesture = ( bCrouched && m_actReloadCrouchGesture != ACT_INVALID ) ? m_actReloadCrouchGesture : m_actReloadGesture;

			ResetIdealActivity( bCrouched ? ACT_RELOAD_LOW : ACT_RELOAD );

			float flDuration = 1.5f;
			if ( gesture != ACT_INVALID )
			{
				RestartGesture( gesture );
				int iSeq = SelectWeightedSequence( gesture );
				if ( iSeq != ACTIVITY_NOT_AVAILABLE )
					flDuration = SequenceDuration( iSeq );
			}

			// Fill the clip a little before the gesture ends, like the Combine event timing.
			m_flReloadDoneTime = gpGlobals->curtime + flDuration * 0.85f;
		}
		break;

	default:
		BaseClass::StartTask( pTask );
		break;
	}
}

void CNPC_GermanSoldier::RunTask( const Task_t *pTask )
{
	switch ( pTask->iTask )
	{
	case TASK_RANGE_ATTACK1:
		{
			AutoMovement();

			Vector vecEnemyLKP = GetEnemyLKP();
			if ( !FInAimCone( vecEnemyLKP ) )
				GetMotor()->SetIdealYawToTargetAndUpdate( vecEnemyLKP, AI_KEEP_YAW_SPEED );
			else
				GetMotor()->SetIdealYawAndUpdate( GetMotor()->GetIdealYaw(), AI_KEEP_YAW_SPEED );

			if ( gpGlobals->curtime >= m_flNextShotTime )
			{
				if ( m_nShotsLeft > 0 && FireOneShot() )
				{
					m_nShotsLeft--;
					SetLastAttackTime( gpGlobals->curtime );
					m_flNextShotTime = gpGlobals->curtime + m_flShotInterval;
				}
				else
				{
					TaskComplete();
				}
			}
		}
		break;

	case TASK_RELOAD:
		{
			Vector vecEnemyLKP = GetEnemyLKP();
			if ( GetEnemy() && vecEnemyLKP != vec3_origin )
				GetMotor()->SetIdealYawToTargetAndUpdate( vecEnemyLKP, AI_KEEP_YAW_SPEED );

			if ( gpGlobals->curtime >= m_flReloadDoneTime )
			{
				if ( GetActiveWeapon() )
					GetActiveWeapon()->Reload_NPC( true );

				ClearCondition( COND_LOW_PRIMARY_AMMO );
				ClearCondition( COND_NO_PRIMARY_AMMO );
				ClearCondition( COND_NO_SECONDARY_AMMO );

				if ( GetShotRegulator() )
					GetShotRegulator()->Reset( false );

				TaskComplete();
			}
		}
		break;

	default:
		BaseClass::RunTask( pTask );
		break;
	}
}

int CNPC_GermanSoldier::MeleeAttack1Conditions( float flDot, float flDist )
{
	// No rifle-butt animation with a hit event yet. Stage 2e.
	return COND_NONE;
}
