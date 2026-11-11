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
//   - Aiming drives DoD's body_pitch / body_yaw instead of aim_pitch / aim_yaw,
//     with per-axis sign and offset from the script.
//   - DoD locomotion is a 9-way move_x / move_y blend. While moving we face
//     where the AI wants to look (usually the enemy) and feed the walk direction
//     into move_x / move_y, so soldiers strafe and back-pedal instead of turning.
//   - DoD shoot / reload animations are delta gestures with no fire or reload
//     events, so shooting and reloading are timed in code and the DoD gesture
//     is layered on top. This covers standing fire (TASK_RANGE_ATTACK1), firing
//     on the move (Combine move-and-shoot gestures) and reloading on the move.
//   - Grenades and the kick are off until those animations exist (later stage).
//
// Stage 3a:
//   - Death animations: on death a soldier usually plays a death animation
//     (headshot, gut shot, running stumble, crouched collapse, ...) and becomes
//     a ragdoll from its last frame. Explosions, falls, heavy hits and a random
//     share ("death_anim_chance" in the script) still ragdoll straight away.
//   - Hit reactions, two systems (CoD animations never layered on top of DoD):
//     * every hit: a procedural kick. The hit direction, hit location and strength
//       are networked; the client (c_npc_german_soldier.cpp) bends the spine / snaps
//       the head away from the shot and springs back. Works in any stance, arms
//       ride along so the gun stays in both hands. Brief loss of aim.
//     * big or unlucky hits while standing: a full-body CoD pain animation
//       (helmet hit, shoulder, gut ...) replaces the DoD pose, untouched, via
//       SCHED_BIG_FLINCH. Thresholds in the script.
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

ConVar npc_german_debug( "npc_german_debug", "0", 0, "1 = print activity translations and missing animations. 2 = also print aim values twice a second." );

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
	DECLARE_SERVERCLASS();

	CNPC_GermanSoldier();

	void		Precache( void );
	void		Spawn( void );

	Activity	NPC_TranslateActivity( Activity eNewActivity );
	void		OnChangeActivity( Activity eNewActivity );

	void		PrescheduleThink( void );
	void		OnRangeAttack1( void );

	void		StartTask( const Task_t *pTask );
	void		RunTask( const Task_t *pTask );

	int			MeleeAttack1Conditions( float flDot, float flDist );

	// Death animations
	void		Event_Killed( const CTakeDamageInfo &info );
	bool		BecomeRagdoll( const CTakeDamageInfo &info, const Vector &forceVector );
	bool		CanBecomeRagdoll( void );
	Activity	GetDeathActivity( void );
	int			OnTakeDamage_Dying( const CTakeDamageInfo &info );

	// Hit reactions
	int			OnTakeDamage_Alive( const CTakeDamageInfo &info );
	void		PlayFlinchGesture( void );
	int			SelectSchedule( void );
	Activity	GetFlinchActivity( bool bHeavyDamage, bool bGesture );

	void		SetAim( const Vector &aimDir );
	void		RelaxAim( void );
	void		MaintainLookTargets( float flInterval );
	bool		OverrideMoveFacing( const AILocalMoveGoal_t &move, float flInterval );

protected:
	void		PopulatePoseParameters( void );

private:
	void		EnsureAnimMap( void );
	void		BuildAnimMap( void );
	Activity	ResolveActivity( const char *pszName, const char *pszWeaponSet );
	void		ApplyAimPose( void );
	void		HoldMoveForward( void );
	bool		FireOneShot( bool bPlayGesture );
	bool		ShouldPlayDeathAnim( const CTakeDamageInfo &info );
	Activity	PickDeathActivity( void );
	Activity	PickAvailable( const char **ppszNames, int nCount );
	void		FinishDeathAnim( void );
	void		DoHitReaction( const CTakeDamageInfo &info );
	bool		IsHitReacting( void ) const { return gpGlobals->curtime < m_flFlinchUntil; }
	bool		IsInCrouchPose( void );
	void		FinishReload( void );
	void		UpdateMovingReload( void );

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
	float				m_flPitchOffset;
	float				m_flYawOffset;
	float				m_flMoveYSign;
	Activity			m_actLastDebug;

	// Aim in world terms (degrees, Source convention); written to the DoD pose
	// parameters through the script's sign/offset
	float				m_flAimPitch;
	float				m_flAimYaw;
	float				m_flNextAimDebug;
	int					m_nAimCalls;

	// DoD pose parameters
	int					m_poseMoveX;
	int					m_poseMoveY;

	// Shooting / reloading timers
	int					m_nShotsLeft;
	float				m_flShotInterval;
	float				m_flNextShotTime;
	float				m_flReloadDoneTime;
	float				m_flGestureReloadDone;	// 0 = none, >0 = refill time, -1 = done, wait for gesture to end

	// Death animation state (not saved: a save made mid-death just ragdolls on load)
	float				m_flDeathAnimChance;
	bool				m_bDeathAnim;			// playing a death animation; ragdoll when it ends
	CTakeDamageInfo		m_DeathInfo;
	Vector				m_vecDeathForce;
	Activity			m_actDeath;
	float				m_flDeathAnimGiveUp;
	float				m_flDeathAnimSpeed;
	float				m_flDeathRagdollAt;		// cycle at which the death animation hands over to the ragdoll

	// Hit reaction state
	float				m_flFlinchUntil;		// no firing until then
	float				m_flNextHitReaction;	// unused (kept for layout clarity)
	float				m_flNextFullFlinch;		// cooldown for full-body CoD flinches
	float				m_flWantFullFlinch;		// time a full flinch was requested (0 = none)
	float				m_flHitKickScale;		// script tuning
	float				m_flFullFlinchDamage;
	float				m_flFullFlinchChance;
	float				m_flFullFlinchCooldown;
	float				m_flLastHitSide;		// where the last hit landed across his body: + = his right, - = his left (units)
	float				m_flLastHitDamage;
	bool				m_bLastHitCrouched;		// crouch state at the moment of the last hit

	// Networked to the client for the procedural hit kick
	CNetworkVector( m_vecHitDir );			// horizontal direction the bullet travelled
	CNetworkVar( int, m_nHitGroup );
	CNetworkVar( float, m_flHitTime );
	CNetworkVar( float, m_flHitStrength );
};

IMPLEMENT_SERVERCLASS_ST( CNPC_GermanSoldier, DT_NPC_GermanSoldier )
	SendPropVector( SENDINFO( m_vecHitDir ), 10, 0, -1.0f, 1.0f ),
	SendPropInt( SENDINFO( m_nHitGroup ), 4, SPROP_UNSIGNED ),
	SendPropTime( SENDINFO( m_flHitTime ) ),
	SendPropFloat( SENDINFO( m_flHitStrength ), 8, 0, 0.0f, 2.0f ),
END_SEND_TABLE()

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
	m_flPitchOffset = 0.0f;
	m_flYawOffset = 0.0f;
	m_flMoveYSign = 1.0f;
	m_flAimPitch = 0.0f;
	m_flAimYaw = 0.0f;
	m_flNextAimDebug = 0.0f;
	m_nAimCalls = 0;
	m_actLastDebug = ACT_INVALID;
	m_poseMoveX = -1;
	m_poseMoveY = -1;
	m_nShotsLeft = 0;
	m_flShotInterval = 0.1f;
	m_flNextShotTime = 0.0f;
	m_flReloadDoneTime = 0.0f;
	m_flGestureReloadDone = 0.0f;
	m_flDeathAnimChance = 0.75f;
	m_bDeathAnim = false;
	m_vecDeathForce = vec3_origin;
	m_actDeath = ACT_INVALID;
	m_flDeathAnimGiveUp = 0.0f;
	m_flDeathAnimSpeed = 1.0f;
	m_flDeathRagdollAt = 0.8f;
	m_flFlinchUntil = 0.0f;
	m_flNextHitReaction = 0.0f;
	m_flNextFullFlinch = 0.0f;
	m_flWantFullFlinch = 0.0f;
	m_flHitKickScale = 1.0f;
	m_flFullFlinchDamage = 30.0f;
	m_flFullFlinchChance = 0.2f;
	m_flFullFlinchCooldown = 3.0f;
	m_flLastHitSide = 0.0f;
	m_flLastHitDamage = 0.0f;
	m_bLastHitCrouched = false;
	m_vecHitDir.Init( 1, 0, 0 );
	m_nHitGroup = 0;
	m_flHitTime = 0.0f;
	m_flHitStrength = 0.0f;
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

//-----------------------------------------------------------------------------
// Strafing. Mirrors what CAI_Motor::MoveFacing does for models with a move_yaw
// pose parameter: face the blend of path direction and facing targets (the enemy),
// then tell the 9-way blend which way the feet are travelling relative to that.
//-----------------------------------------------------------------------------
bool CNPC_GermanSoldier::OverrideMoveFacing( const AILocalMoveGoal_t &move, float flInterval )
{
	if ( BaseClass::OverrideMoveFacing( move, flInterval ) )
		return true;

	if ( m_poseMoveX < 0 || m_poseMoveY < 0 )
		return false;	// not a DoD model; let the motor turn us to face the path

	float flMoveYaw = UTIL_VecToYaw( move.dir );

	Vector vecFacing;
	float flInfluence = GetMotor()->GetFacingDirection( vecFacing );
	Vector dir = move.facing * ( 1.0f - flInfluence ) + vecFacing * flInfluence;
	VectorNormalize( dir );

	GetMotor()->SetIdealYawAndUpdate( UTIL_AngleMod( UTIL_VecToYaw( dir ) ) );

	// Travel direction relative to where the body now faces (+ = to his left).
	float flDiff = DEG2RAD( UTIL_AngleDiff( flMoveYaw, GetLocalAngles().y ) );
	SetPoseParameter( m_poseMoveX, cosf( flDiff ) );
	SetPoseParameter( m_poseMoveY, sinf( flDiff ) * m_flMoveYSign );

	// Sideways and backwards DoD cycles cover ground at different speeds; move at
	// the speed of the blend we are actually showing so the feet don't slide.
	float flSpeed = GetSequenceGroundSpeed( GetSequence() );
	if ( flSpeed > 0.0f )
		m_flGroundSpeed = flSpeed;

	return true;
}

void CNPC_GermanSoldier::HoldMoveForward( void )
{
	// DoD walk/run sequences blend 9 directions on move_x (forward) / move_y (left).
	// Start every new activity facing forward; OverrideMoveFacing adjusts while moving.
	if ( m_poseMoveX >= 0 )
		SetPoseParameter( m_poseMoveX, 1.0f );
	if ( m_poseMoveY >= 0 )
		SetPoseParameter( m_poseMoveY, 0.0f );
}

void CNPC_GermanSoldier::ApplyAimPose( void )
{
	if ( m_poseAim_Pitch >= 0 )
		SetPoseParameter( m_poseAim_Pitch, m_flAimPitch * m_flPitchSign + m_flPitchOffset );
	if ( m_poseAim_Yaw >= 0 )
		SetPoseParameter( m_poseAim_Yaw, m_flAimYaw * m_flYawSign + m_flYawOffset );
}

// Same smoothing as CAI_BaseNPC::SetAim, but kept in our own variables so the
// script's sign/offset never feed back into the smoothing.
void CNPC_GermanSoldier::SetAim( const Vector &aimDir )
{
	EnsureAnimMap();

	QAngle angDir;
	VectorAngles( aimDir, angDir );

	float flRelativeYaw = UTIL_AngleDiff( angDir.y, GetAbsAngles().y );
	float flRate = GetEnemy() ? 0.8f : 0.6f;		// sweep more slowly when not fighting

	float newPitch = m_flAimPitch + flRate * UTIL_AngleDiff( UTIL_ApproachAngle( angDir.x, m_flAimPitch, 20 ), m_flAimPitch );
	float newYaw = GetEnemy() ? flRelativeYaw : m_flAimYaw + flRate * UTIL_AngleDiff( flRelativeYaw, m_flAimYaw );

	m_flAimPitch = AngleNormalize( newPitch );
	m_flAimYaw = AngleNormalize( newYaw );
	ApplyAimPose();

	// Interaction yaw, as in the base class
	m_flInteractionYaw = ( fabs( m_flAimYaw ) < 20 ) ? angDir.y : GetAbsAngles().y;

	m_nAimCalls++;
	if ( npc_german_debug.GetInt() >= 2 && gpGlobals->curtime >= m_flNextAimDebug )
	{
		m_flNextAimDebug = gpGlobals->curtime + 0.5f;
		Msg( "npc_german_soldier %d aim: enemy %s, target yaw %.0f (body faces %.0f, relative %.0f), target pitch %.0f -> pose body_yaw %.1f [#%d], body_pitch %.1f [#%d], %d aim updates\n",
			entindex(), GetEnemy() ? GetEnemy()->GetClassname() : "none", angDir.y, GetAbsAngles().y, flRelativeYaw, angDir.x,
			m_poseAim_Yaw >= 0 ? GetPoseParameter( m_poseAim_Yaw ) : -999.0f, m_poseAim_Yaw,
			m_poseAim_Pitch >= 0 ? GetPoseParameter( m_poseAim_Pitch ) : -999.0f, m_poseAim_Pitch, m_nAimCalls );
		m_nAimCalls = 0;
	}
}

void CNPC_GermanSoldier::RelaxAim( void )
{
	EnsureAnimMap();
	m_flAimPitch = AngleNormalize( UTIL_ApproachAngle( 0, m_flAimPitch, 3 ) );
	m_flAimYaw = AngleNormalize( UTIL_ApproachAngle( 0, m_flAimYaw, 2 ) );
	ApplyAimPose();
}

void CNPC_GermanSoldier::MaintainLookTargets( float flInterval )
{
	// The actor code also drives a "body_yaw" pose parameter (for head/body turns)
	// and would zero our aim every think. Put the aim back afterwards.
	float flBefore = ( m_poseAim_Yaw >= 0 ) ? GetPoseParameter( m_poseAim_Yaw ) : 0.0f;
	BaseClass::MaintainLookTargets( flInterval );
	if ( npc_german_debug.GetInt() >= 3 && m_poseAim_Yaw >= 0 && GetPoseParameter( m_poseAim_Yaw ) != flBefore )
		Msg( "npc_german_soldier %d: actor code changed body_yaw %.1f -> %.1f (restoring)\n", entindex(), flBefore, GetPoseParameter( m_poseAim_Yaw ) );
	ApplyAimPose();
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
	m_flPitchOffset = pScript->GetFloat( "body_pitch_offset", 0.0f );
	m_flYawOffset = pScript->GetFloat( "body_yaw_offset", 0.0f );
	m_flMoveYSign = ( pScript->GetFloat( "move_y_sign", 1.0f ) < 0.0f ) ? -1.0f : 1.0f;
	m_flDeathAnimChance = clamp( pScript->GetFloat( "death_anim_chance", 0.75f ), 0.0f, 1.0f );
	m_flDeathAnimSpeed = clamp( pScript->GetFloat( "death_anim_speed", 1.0f ), 0.25f, 2.0f );
	m_flDeathRagdollAt = clamp( pScript->GetFloat( "death_ragdoll_at", 0.8f ), 0.3f, 1.0f );
	m_flHitKickScale = clamp( pScript->GetFloat( "hit_kick_scale", 1.0f ), 0.0f, 3.0f );
	m_flFullFlinchDamage = pScript->GetFloat( "full_flinch_damage", 30.0f );
	m_flFullFlinchChance = clamp( pScript->GetFloat( "full_flinch_chance", 0.2f ), 0.0f, 1.0f );
	m_flFullFlinchCooldown = pScript->GetFloat( "full_flinch_cooldown", 3.0f );

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
bool CNPC_GermanSoldier::FireOneShot( bool bPlayGesture )
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

	if ( bPlayGesture && m_actFireGesture != ACT_INVALID )
		RestartGesture( m_actFireGesture );

	return true;
}

void CNPC_GermanSoldier::FinishReload( void )
{
	if ( GetActiveWeapon() )
		GetActiveWeapon()->Reload_NPC( true );

	ClearCondition( COND_LOW_PRIMARY_AMMO );
	ClearCondition( COND_NO_PRIMARY_AMMO );
	ClearCondition( COND_NO_SECONDARY_AMMO );

	if ( GetShotRegulator() )
		GetShotRegulator()->Reset( false );
}

// Firing while moving: the Combine move-and-shoot code calls OnRangeAttack1 and then
// plays ACT_GESTURE_RANGE_ATTACK1 (mapped to the DoD fire gesture), expecting the
// gesture's fire event to shoot. DoD gestures have no event, so shoot here.
void CNPC_GermanSoldier::OnRangeAttack1( void )
{
	BaseClass::OnRangeAttack1();

	const Task_t *pTask = GetTask();
	bool bStandingFire = pTask && pTask->iTask == TASK_RANGE_ATTACK1;	// that task shoots by itself
	if ( !bStandingFire && !IsHitReacting() )
	{
		FireOneShot( false );	// the move-and-shoot code plays the gesture
	}
}

// Reloading while moving: the move-and-shoot code plays ACT_GESTURE_RELOAD (mapped to
// the DoD reload gesture) and expects a reload event to fill the clip. Time it instead.
void CNPC_GermanSoldier::UpdateMovingReload( void )
{
	const Task_t *pTask = GetTask();
	if ( pTask && pTask->iTask == TASK_RELOAD )
		return;	// the reload task handles its own timing

	bool bPlaying = ( m_actReloadGesture != ACT_INVALID && IsPlayingGesture( m_actReloadGesture ) ) ||
					( m_actReloadCrouchGesture != ACT_INVALID && IsPlayingGesture( m_actReloadCrouchGesture ) );

	if ( m_flGestureReloadDone < 0.0f )
	{
		if ( !bPlaying )
			m_flGestureReloadDone = 0.0f;
	}
	else if ( m_flGestureReloadDone == 0.0f )
	{
		if ( bPlaying && m_actReloadGesture != ACT_INVALID )
		{
			float flDuration = 1.5f;
			int iSeq = SelectWeightedSequence( m_actReloadGesture );
			if ( iSeq != ACTIVITY_NOT_AVAILABLE )
				flDuration = SequenceDuration( iSeq );
			m_flGestureReloadDone = gpGlobals->curtime + flDuration * 0.85f;
		}
	}
	else if ( gpGlobals->curtime >= m_flGestureReloadDone )
	{
		FinishReload();
		m_flGestureReloadDone = -1.0f;
	}
}

void CNPC_GermanSoldier::PrescheduleThink( void )
{
	BaseClass::PrescheduleThink();
	UpdateMovingReload();
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
				// Settle the aim first, and respect the gun's own cycle time (bolt-action rifles
				// set m_flNextPrimaryAttack after each NPC shot; HL2 guns leave it alone).
				m_flNextShotTime = MAX( gpGlobals->curtime + 0.15f, (float)pWeapon->m_flNextPrimaryAttack );
			}
		}
		break;

	case TASK_RELOAD:
		{
			EnsureAnimMap();

			bool bCrouched = IsCrouching() || TranslateCrouchActivity( ACT_RELOAD ) == ACT_RELOAD_LOW;
			Activity gesture = ( bCrouched && m_actReloadCrouchGesture != ACT_INVALID ) ? m_actReloadCrouchGesture : m_actReloadGesture;

			// One reload at a time, and never a reload with a full clip. Combine
			// "hide and reload" runs to cover first; the move-and-shoot code often starts
			// a reload gesture on the way, so by the time this task runs the clip may be
			// full already or the reload half done.
			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			if ( !pWeapon || ( pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() >= pWeapon->GetMaxClip1() ) )
			{
				ClearCondition( COND_LOW_PRIMARY_AMMO );
				ClearCondition( COND_NO_PRIMARY_AMMO );
				TaskComplete();
				break;
			}

			ResetIdealActivity( bCrouched ? ACT_RELOAD_LOW : ACT_RELOAD );

			if ( m_flGestureReloadDone > 0.0f )
			{
				// A reload started while moving is still playing: let it finish and
				// take over its timer instead of starting a second one.
				m_flReloadDoneTime = m_flGestureReloadDone;
				m_flGestureReloadDone = -1.0f;
				break;
			}

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

			if ( IsHitReacting() )
				m_flNextShotTime = MAX( m_flNextShotTime, m_flFlinchUntil );	// can't shoot mid-flinch

			if ( gpGlobals->curtime >= m_flNextShotTime )
			{
				if ( m_nShotsLeft > 0 && FireOneShot( true ) )
				{
					m_nShotsLeft--;
					OnRangeAttack1();	// shot regulator bookkeeping (doesn't fire again inside this task)
					SetLastAttackTime( gpGlobals->curtime );
					m_flNextShotTime = gpGlobals->curtime + m_flShotInterval;
					if ( GetActiveWeapon() )
						m_flNextShotTime = MAX( m_flNextShotTime, (float)GetActiveWeapon()->m_flNextPrimaryAttack );
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
				FinishReload();
				m_flGestureReloadDone = -1.0f;	// the gesture's tail is still playing; don't count it twice
				TaskComplete();
			}
		}
		break;

	case TASK_DIE:
		// Hand over to physics a little before the animation ends: the last frames
		// are the body already lying in its final pose, which (a) looks stiff and
		// (b) can sit outside the ragdoll's joint limits or dip into the floor, and
		// the physics solver "springs" such a pose apart - launching the body.
		if ( m_bDeathAnim && ( GetCycle() >= m_flDeathRagdollAt || ( IsActivityFinished() && GetCycle() >= 1.0f ) ||
							   gpGlobals->curtime > m_flDeathAnimGiveUp ) )
		{
			FinishDeathAnim();
			break;
		}
		if ( m_bDeathAnim )
			SetPlaybackRate( m_flDeathAnimSpeed );
		BaseClass::RunTask( pTask );
		break;

	default:
		BaseClass::RunTask( pTask );
		break;
	}
}

//-----------------------------------------------------------------------------
// Death animations
//
// Flow: Event_Killed decides; the base kill code then asks BecomeRagdoll, which we
// decline while a death animation is pending (and CanBecomeRagdoll says no, so the
// dead-schedule picks SCHED_DIE instead of ragdolling). TASK_DIE plays
// GetDeathActivity; when it ends (or after a safety timeout) we ragdoll for real,
// starting from the animation's last pose.
//-----------------------------------------------------------------------------
bool CNPC_GermanSoldier::ShouldPlayDeathAnim( const CTakeDamageInfo &info )
{
	EnsureAnimMap();

	if ( random->RandomFloat( 0.0f, 1.0f ) > m_flDeathAnimChance )
		return false;

	// Things that should throw a body, not play a canned fall.
	const int nRagdollNow = DMG_BLAST | DMG_CRUSH | DMG_VEHICLE | DMG_FALL | DMG_DISSOLVE |
							DMG_REMOVENORAGDOLL | DMG_PHYSGUN | DMG_ALWAYSGIB;
	if ( info.GetDamageType() & nRagdollNow )
		return false;
	if ( info.GetDamageForce().Length() > 30000.0f )
		return false;
	if ( !( GetFlags() & FL_ONGROUND ) || GetWaterLevel() >= 2 )
		return false;
	if ( IsInAScript() || GetParent() )
		return false;

	return PickDeathActivity() != ACT_INVALID;
}

Activity CNPC_GermanSoldier::PickAvailable( const char **ppszNames, int nCount )
{
	Activity candidates[16];
	int nFound = 0;
	for ( int i = 0; i < nCount && nFound < (int)ARRAYSIZE( candidates ); i++ )
	{
		int iAct = ActivityList_IndexForName( ppszNames[i] );
		if ( iAct >= 0 && HaveSequenceForActivity( (Activity)iAct ) )
			candidates[nFound++] = (Activity)iAct;
	}
	return nFound ? candidates[random->RandomInt( 0, nFound - 1 )] : ACT_INVALID;
}

Activity CNPC_GermanSoldier::PickDeathActivity( void )
{
	// Crouch-running: CoD1 crouch-run deaths
	if ( IsMoving() && IsInCrouchPose() )
	{
		static const char *s_CrouchRun[] = { "ACT_WW2_DEATH_CROUCHRUN" };
		Activity a = PickAvailable( s_CrouchRun, ARRAYSIZE( s_CrouchRun ) );
		if ( a != ACT_INVALID )
			return a;
	}

	// Running: the CoD running deaths (stumble, crumple, fall forward ...), CoD2 and CoD1
	if ( IsMoving() && m_flGroundSpeed > 100.0f )
	{
		static const char *s_Run[] = { "ACT_WW2_DEATH_RUN_FRONT", "ACT_WW2_DEATH_RUN_STUMBLE", "ACT_WW2_DEATH_RUN_CRUMPLE",
									   "ACT_WW2_DEATH_RUN_LEFT", "ACT_WW2_DEATH_RUN_RIGHT", "ACT_WW2_DEATH_RUN_BACK",
									   "ACT_WW2_DEATH_RUN_ONFRONT", "ACT_WW2_DEATH_RUN_ONLEFT" };
		Activity a = PickAvailable( s_Run, ARRAYSIZE( s_Run ) );
		if ( a != ACT_INVALID )
			return a;
	}

	// Crouched or in low cover
	if ( IsInCrouchPose() )
	{
		bool bHead = ( LastHitGroup() == HITGROUP_HEAD );
		static const char *s_CrouchHead[] = { "ACT_WW2_CROUCH_DEATH_HEAD", "ACT_WW2_DEATH_CROUCH_HEADSHOT" };
		if ( bHead )
		{
			Activity a = PickAvailable( s_CrouchHead, ARRAYSIZE( s_CrouchHead ) );
			if ( a != ACT_INVALID )
				return a;
		}
		static const char *s_Crouch[] = { "ACT_DIE_CROUCH", "ACT_WW2_DEATH_CROUCH_HANDS", "ACT_WW2_DEATH_CROUCH_FETAL",
										  "ACT_WW2_DEATH_CROUCH_FLIP", "ACT_WW2_CROUCH_DEATH" };
		Activity a = PickAvailable( s_Crouch, ARRAYSIZE( s_Crouch ) );
		if ( a != ACT_INVALID )
			return a;
	}

	// Standing: the base picks by hit location and direction (headshot, gut shot, fall back ...)
	Activity a = BaseClass::GetDeathActivity();
	if ( a != ACT_INVALID && HaveSequenceForActivity( a ) )
		return a;
	return HaveSequenceForActivity( ACT_DIESIMPLE ) ? ACT_DIESIMPLE : ACT_INVALID;
}

void CNPC_GermanSoldier::Event_Killed( const CTakeDamageInfo &info )
{
	m_bDeathAnim = false;
	m_actDeath = ACT_INVALID;
	if ( !IsCurSchedule( SCHED_NPC_FREEZE ) && ShouldPlayDeathAnim( info ) )	// frozen NPCs don't die
	{
		m_actDeath = PickDeathActivity();
		m_DeathInfo = info;
		m_vecDeathForce = vec3_origin;		// a finished death animation settles; see FinishDeathAnim
		m_flDeathAnimGiveUp = gpGlobals->curtime + 8.0f;
		m_bDeathAnim = true;
	}

	BaseClass::Event_Killed( info );
}

bool CNPC_GermanSoldier::BecomeRagdoll( const CTakeDamageInfo &info, const Vector &forceVector )
{
	if ( m_bDeathAnim )
		return true;	// ragdoll later, from the death animation's last pose
	return BaseClass::BecomeRagdoll( info, forceVector );
}

bool CNPC_GermanSoldier::CanBecomeRagdoll( void )
{
	if ( m_bDeathAnim )
		return false;
	return BaseClass::CanBecomeRagdoll();
}

Activity CNPC_GermanSoldier::GetDeathActivity( void )
{
	if ( m_actDeath != ACT_INVALID )
		return m_actDeath;
	return BaseClass::GetDeathActivity();
}

void CNPC_GermanSoldier::FinishDeathAnim( void )
{
	if ( !m_bDeathAnim )
		return;

	m_bDeathAnim = false;

	// The animation already did the falling. Hand over to physics with no push at
	// all (the bullet's force was meant for the moment of the hit, not seconds later)
	// so the body just settles from the final pose.
	CTakeDamageInfo info = m_DeathInfo;
	info.SetDamageForce( m_vecDeathForce );
	BaseClass::BecomeRagdoll( info, m_vecDeathForce );
}

int CNPC_GermanSoldier::OnTakeDamage_Dying( const CTakeDamageInfo &info )
{
	// Shot again mid-fall: a big hit or an explosion turns the fall into a ragdoll now.
	if ( m_bDeathAnim && ( ( info.GetDamageType() & DMG_BLAST ) || info.GetDamage() >= 50.0f ) )
	{
		m_DeathInfo = info;
		m_vecDeathForce = info.GetDamageForce();	// this hit happens now, so it does push the body
		FinishDeathAnim();
		return 1;
	}
	return BaseClass::OnTakeDamage_Dying( info );
}

//-----------------------------------------------------------------------------
// Hit reactions
//-----------------------------------------------------------------------------
int CNPC_GermanSoldier::OnTakeDamage_Alive( const CTakeDamageInfo &info )
{
	int nResult = BaseClass::OnTakeDamage_Alive( info );
	if ( nResult && IsAlive() && info.GetDamage() > 0.0f )
		DoHitReaction( info );
	return nResult;
}

void CNPC_GermanSoldier::PlayFlinchGesture( void )
{
	// No gesture flinches: CoD flinch deltas don't fit DoD poses. Hit reactions are
	// the procedural kick (every hit) plus full-body CoD flinches (big hits).
}

// Is he crouched right now? Judged by the animation actually playing (its DoD/CoD
// activity name), not by AI state: the Combine AI often crouches through activity
// translation alone, so GetActivity()/IsCrouching() can say "standing" while the
// body is down. One owner for this question - deaths and flinches both ask here.
bool CNPC_GermanSoldier::IsInCrouchPose( void )
{
	if ( IsCrouching() )
		return true;

	if ( GetModelPtr() && GetSequence() >= 0 )
	{
		const char *pszAct = ActivityList_NameForIndex( GetSequenceActivity( GetSequence() ) );
		if ( pszAct && ( Q_stristr( pszAct, "CROUCH" ) || Q_stristr( pszAct, "_LOW" ) || Q_stristr( pszAct, "HIDE" ) ) )
			return true;
	}

	Activity act = GetActivity();
	return act == ACT_RANGE_ATTACK1_LOW || act == ACT_RANGE_AIM_LOW || act == ACT_COVER_LOW ||
		   act == ACT_CROUCHIDLE || act == ACT_CROUCH || act == ACT_RELOAD_LOW || act == ACT_WALK_CROUCH ||
		   act == ACT_WALK_CROUCH_AIM || act == ACT_RUN_CROUCH || act == ACT_RUN_CROUCH_AIM;
}

void CNPC_GermanSoldier::DoHitReaction( const CTakeDamageInfo &info )
{
	EnsureAnimMap();

	// --- Procedural kick, every hit ---------------------------------------
	Vector vecDir = info.GetDamageForce();
	if ( vecDir.LengthSqr() < 1.0f && info.GetAttacker() )
		vecDir = GetAbsOrigin() - info.GetAttacker()->GetAbsOrigin();
	vecDir.z = 0.0f;
	if ( VectorNormalize( vecDir ) < 0.001f )
		vecDir = -BodyDirection2D();

	// Which side of his body was hit (for picking a full-body flinch)?
	Vector vecRight;
	GetVectors( NULL, &vecRight, NULL );
	m_flLastHitSide = DotProduct( info.GetDamagePosition() - WorldSpaceCenter(), vecRight );

	float flStrength = clamp( info.GetDamage() / 25.0f, 0.35f, 1.5f ) * m_flHitKickScale;
	m_vecHitDir = vecDir;
	m_nHitGroup = clamp( LastHitGroup(), 0, 15 );
	m_flHitStrength = MIN( flStrength, 2.0f );
	m_flHitTime = gpGlobals->curtime;

	// Knocked off aim for a moment; a reload in progress takes that much longer.
	float flStunned = 0.15f + 0.2f * MIN( flStrength, 1.0f );
	m_flFlinchUntil = MAX( m_flFlinchUntil, gpGlobals->curtime + flStunned );
	if ( m_flReloadDoneTime > gpGlobals->curtime )
		m_flReloadDoneTime += flStunned;
	if ( m_flGestureReloadDone > gpGlobals->curtime )
		m_flGestureReloadDone += flStunned;

	// --- Full-body CoD flinch: big or unlucky hits, not while moving ---------
	// (standing: CoD2 + CoD1 pains by hit location; crouched: CoD1 crouch pain)
	m_flLastHitDamage = info.GetDamage();
	m_bLastHitCrouched = IsInCrouchPose();
	if ( gpGlobals->curtime < m_flNextFullFlinch || IsMoving() || IsCurSchedule( SCHED_BIG_FLINCH ) )
		return;
	if ( info.GetDamage() < m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) > m_flFullFlinchChance )
		return;
	if ( !HaveSequenceForActivity( GetFlinchActivity( true, false ) ) )
		return;

	m_flWantFullFlinch = gpGlobals->curtime;
	m_flNextFullFlinch = gpGlobals->curtime + m_flFullFlinchCooldown;
	SetCondition( COND_HEAVY_DAMAGE );		// interrupts the current schedule -> SelectSchedule
}

// The pack has four full-body pains: helmet hit, left shoulder, right shoulder and
// the gut hunch. Chest and generic hits would all get the hunch, so pick by which
// side of the chest was hit: off-centre hits use that side's shoulder animation.
Activity CNPC_GermanSoldier::GetFlinchActivity( bool bHeavyDamage, bool bGesture )
{
	if ( !bGesture )
	{
		// Crouched when hit: the CoD1 crouch pain, whatever was hit.
		if ( m_bLastHitCrouched )
		{
			int iAct = ActivityList_IndexForName( "ACT_WW2_CROUCH_PAIN" );
			if ( iAct >= 0 && HaveSequenceForActivity( (Activity)iAct ) )
				return (Activity)iAct;
		}

		// A really big body hit sometimes drops him to one hand (CoD1).
		int nGroup = LastHitGroup();
		if ( nGroup != HITGROUP_HEAD && m_flLastHitDamage >= m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) < 0.35f )
		{
			int iAct = ActivityList_IndexForName( "ACT_WW2_PAIN_FALLTOHAND" );
			if ( iAct >= 0 && HaveSequenceForActivity( (Activity)iAct ) )
				return (Activity)iAct;
		}

		if ( nGroup == HITGROUP_CHEST || nGroup == HITGROUP_GENERIC || nGroup == HITGROUP_GEAR )
		{
			const float flCentre = 3.0f;	// within this many units of the middle counts as centre
			if ( m_flLastHitSide > flCentre && HaveSequenceForActivity( ACT_FLINCH_RIGHTARM ) )
				return ACT_FLINCH_RIGHTARM;
			if ( m_flLastHitSide < -flCentre && HaveSequenceForActivity( ACT_FLINCH_LEFTARM ) )
				return ACT_FLINCH_LEFTARM;
		}
	}
	return BaseClass::GetFlinchActivity( bHeavyDamage, bGesture );
}

int CNPC_GermanSoldier::SelectSchedule( void )
{
	if ( m_flWantFullFlinch > 0.0f )
	{
		bool bFresh = gpGlobals->curtime - m_flWantFullFlinch < 0.5f;
		m_flWantFullFlinch = 0.0f;
		if ( bFresh && IsAlive() && HaveSequenceForActivity( GetFlinchActivity( true, false ) ) )
			return SCHED_BIG_FLINCH;	// plays GetFlinchActivity(): head / shoulder / gut by hit location
	}
	return BaseClass::SelectSchedule();
}

int CNPC_GermanSoldier::MeleeAttack1Conditions( float flDot, float flDist )
{
	// No rifle-butt animation with a hit event yet. Stage 2e.
	return COND_NONE;
}
