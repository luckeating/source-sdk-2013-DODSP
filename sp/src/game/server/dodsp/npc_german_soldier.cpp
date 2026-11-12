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
//   - Hit reactions:
//     * every hit: a procedural kick. The hit direction, hit location and strength
//       are networked; the client (c_npc_german_soldier.cpp) bends the spine / snaps
//       the head away from the shot and springs back. Brief loss of aim.
//     * big or unlucky hits: a CoD pain animation (helmet hit, shoulder, gut, crouch
//       pain ...) as an upper-body overlay - it replaces spine, arms, head and gun
//       while the legs keep DoD's stance (standing, crouched or walking).
//       Thresholds in the script.
//     * big body hits on a soldier standing still sometimes: "fall to a hand",
//       a full-body CoD1 moment via SCHED_BIG_FLINCH.
//
// Test commands (npc_german_test for the list): spawn soldiers, play any pain or
// death animation on the soldier you look at, fake hits by body part, force crouch.
//
//=============================================================================//

#include "cbase.h"
#include "npc_combines.h"
#include "activitylist.h"
#include "animation.h"
#include "npcevent.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "player.h"

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

	// Test commands (npc_german_test_*)
	void		GetTestSequences( bool bDeaths, CUtlVector<int> &list );
	void		TestPain( int iSequence );
	void		TestPainAll( void );
	void		TestHit( int nHitGroup, float flSide, float flDamage, CBaseEntity *pAttacker );
	void		TestDeath( int iSequence, CBaseEntity *pAttacker );

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
	void		StartFallToHand( void );
	void		StartPainOverlay( int iSequence );
	void		UpdatePainRecovery( void );
	bool		IsUpperBodyGesture( Activity act ) const;
	void		UpdateTestPainAll( void );
	// One owner for "the upper body is taken by a hit reaction": no shots, no fire or
	// reload gestures until then (pain overlays, the kick's short stun).
	bool		IsHitReacting( void ) const { return gpGlobals->curtime < m_flFlinchUntil; }
	bool		IsInCrouchPose( void );
	Activity	PickUpperPain( void );
	Activity	ResolveNamedActivity( const char *pszName );
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
	bool				m_bFallToHandPending;	// next full-body flinch is "fall to a hand"
	bool				m_bReloadInterrupted;	// a pain cut a reload short; it restarts when the pain lets go

	// Test commands (not saved)
	bool				m_bTestForcePain;		// next DoHitReaction skips chance and cooldown
	int					m_iTestPainLayer;		// overlay layer started by npc_german_test_pain
	int					m_iForceDeathSeq;		// npc_german_test_death: exact death sequence, -1 = none
	int					m_iTestPainAllNext;		// npc_german_test_pain all: next list entry, -1 = off
	float				m_flTestPainAllTime;

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
	m_bFallToHandPending = false;
	m_bReloadInterrupted = false;
	m_bTestForcePain = false;
	m_iTestPainLayer = -1;
	m_iForceDeathSeq = -1;
	m_iTestPainAllNext = -1;
	m_flTestPainAllTime = 0.0f;
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
	// A pain overlay owns the upper body: the move-and-shoot code gets no fire or reload
	// gesture to lay on top of it (it adds nothing when the activity is invalid).
	if ( ( eNewActivity == ACT_GESTURE_RANGE_ATTACK1 || eNewActivity == ACT_GESTURE_RELOAD ) && IsHitReacting() )
		return ACT_INVALID;

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

	if ( IsHitReacting() )
		return false;		// every shot comes through here: none while a hit reaction has the upper body

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
	UpdatePainRecovery();
	UpdateTestPainAll();
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

			if ( IsHitReacting() )
			{
				// Hit mid-pain: the reload starts when the pain lets go (UpdatePainRecovery).
				m_bReloadInterrupted = true;
				m_flReloadDoneTime = gpGlobals->curtime + 1000.0f;
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

	case TASK_DIE:
		BaseClass::StartTask( pTask );
		if ( m_bDeathAnim && m_iForceDeathSeq >= 0 )
		{
			// npc_german_test_death asked for one exact sequence (an activity can have several)
			SetIdealActivity( ACT_DO_NOT_DISTURB );
			SetSequenceById( m_iForceDeathSeq );
		}
		break;

	case TASK_BIG_FLINCH:
		BaseClass::StartTask( pTask );		// uses GetFlinchActivity -> fall to a hand
		m_bFallToHandPending = false;
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
	if ( m_iForceDeathSeq >= 0 )
	{
		m_actDeath = (Activity)GetSequenceActivity( m_iForceDeathSeq );
		m_DeathInfo = info;
		m_vecDeathForce = vec3_origin;
		m_flDeathAnimGiveUp = gpGlobals->curtime + 8.0f;
		m_bDeathAnim = true;
	}
	else if ( !IsCurSchedule( SCHED_NPC_FREEZE ) && ShouldPlayDeathAnim( info ) )	// frozen NPCs don't die
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

	// --- Pain animation: big or unlucky hits ---------------------------------
	// Played as an upper-body overlay (spine up, arms, head, gun) from the WW2 packs
	// while the legs keep DoD's stance - standing, crouched or walking - so nothing
	// slides or snaps back. The one exception is "fall to a hand" (CoD1), a
	// full-body moment for big body hits on a soldier standing still.
	m_flLastHitDamage = info.GetDamage();
	m_bLastHitCrouched = IsInCrouchPose();

	const char *pszSkip = NULL;
	Activity actPain = ACT_INVALID;
	bool bFallToHand = false;
	bool bForce = m_bTestForcePain;
	m_bTestForcePain = false;
	// (npc_german_test_hit forces a reaction: no cooldown, no chance roll)
	if ( !bForce && gpGlobals->curtime < m_flNextFullFlinch )	pszSkip = "cooldown";
	else if ( IsCurSchedule( SCHED_BIG_FLINCH ) )				pszSkip = "already down";
	else if ( !bForce && info.GetDamage() < m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) > m_flFullFlinchChance )
																pszSkip = "small hit, chance roll failed";
	else
	{
		bFallToHand = !m_bLastHitCrouched && !IsMoving() && LastHitGroup() != HITGROUP_HEAD &&
					  info.GetDamage() >= m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) < 0.35f &&
					  ResolveNamedActivity( "ACT_WW2_PAIN_FALLTOHAND" ) != ACT_INVALID;
		if ( !bFallToHand )
		{
			actPain = PickUpperPain();
			if ( actPain == ACT_INVALID )
				pszSkip = "no animation (is npc_ww2_anims_cod1 included?)";
		}
	}

	if ( npc_german_debug.GetBool() || bForce )
	{
		Msg( "npc_german_soldier %d hit: group %d, %.0f dmg, %s%s, side %.1f -> %s%s\n", entindex(), LastHitGroup(), info.GetDamage(),
			 m_bLastHitCrouched ? "CROUCHED" : "standing", IsMoving() ? " moving" : "", m_flLastHitSide,
			 pszSkip ? "kick only: " : "PAIN ",
			 pszSkip ? pszSkip : ( bFallToHand ? "ACT_WW2_PAIN_FALLTOHAND (full body)" : ActivityList_NameForIndex( actPain ) ) );
	}
	if ( pszSkip )
		return;

	m_flNextFullFlinch = gpGlobals->curtime + m_flFullFlinchCooldown;

	if ( bFallToHand )
	{
		StartFallToHand();
		return;
	}

	int iSeq = SelectWeightedSequence( actPain );
	if ( iSeq != ACTIVITY_NOT_AVAILABLE )
		StartPainOverlay( iSeq );
}

bool CNPC_GermanSoldier::IsUpperBodyGesture( Activity act ) const
{
	return act != ACT_INVALID && ( act == m_actFireGesture || act == m_actReloadGesture || act == m_actReloadCrouchGesture );
}

// Every upper-body pain starts here (real hits and the test commands). The pain owns
// the upper body until its overlay starts fading out: fire and reload gestures stop,
// no shots, and a reload it cut short starts over once it lets go.
void CNPC_GermanSoldier::StartPainOverlay( int iSequence )
{
	for ( int i = 0; i < GetNumAnimOverlays(); i++ )
	{
		CAnimationLayer *pLayer = GetAnimOverlay( i );
		if ( !pLayer || !pLayer->IsActive() || pLayer->IsKillMe() || !IsUpperBodyGesture( pLayer->m_nActivity ) )
			continue;
		if ( pLayer->m_nActivity != m_actFireGesture )
			m_bReloadInterrupted = true;
		RemoveLayer( i, 0.1f );
	}
	if ( m_bReloadInterrupted )
	{
		// hold both reload timers until UpdatePainRecovery restarts the reload
		if ( m_flReloadDoneTime > gpGlobals->curtime )
			m_flReloadDoneTime = gpGlobals->curtime + 1000.0f;
		if ( m_flGestureReloadDone > 0.0f )
			m_flGestureReloadDone = gpGlobals->curtime + 1000.0f;
	}

	Activity act = (Activity)GetSequenceActivity( iSequence );
	int iLayer = FindGestureLayer( act );
	if ( iLayer >= 0 && GetAnimOverlay( iLayer )->m_nSequence == iSequence )
		RestartGesture( act );
	else
	{
		if ( iLayer >= 0 )
			FastRemoveLayer( iLayer );
		iLayer = AddGestureSequence( iSequence );
		if ( iLayer >= 0 )
			GetAnimOverlay( iLayer )->m_nActivity = act;	// so a repeat hit restarts it instead of stacking
	}

	// The overlay fades out over its last ~0.35 s (ww2_upper sequences): he's back in
	// the fight as it starts fading, not before.
	float flDuration = SequenceDuration( iSequence );
	float flRelease = MAX( flDuration - 0.35f, flDuration * 0.6f );
	m_flFlinchUntil = MAX( m_flFlinchUntil, gpGlobals->curtime + flRelease );
}

void CNPC_GermanSoldier::UpdatePainRecovery( void )
{
	if ( !m_bReloadInterrupted || IsHitReacting() || !IsAlive() )
		return;
	m_bReloadInterrupted = false;

	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	if ( !pWeapon || ( pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() >= pWeapon->GetMaxClip1() ) )
		return;

	const Task_t *pTask = GetTask();
	bool bTask = pTask && pTask->iTask == TASK_RELOAD;
	bool bCrouched = IsInCrouchPose();
	Activity gesture = ( bCrouched && m_actReloadCrouchGesture != ACT_INVALID ) ? m_actReloadCrouchGesture : m_actReloadGesture;
	if ( gesture == ACT_INVALID )
		return;

	RestartGesture( gesture );
	float flDuration = 1.5f;
	int iSeq = SelectWeightedSequence( gesture );
	if ( iSeq != ACTIVITY_NOT_AVAILABLE )
		flDuration = SequenceDuration( iSeq );
	float flDone = gpGlobals->curtime + flDuration * 0.85f;
	if ( bTask )
		m_flReloadDoneTime = flDone;
	else
		m_flGestureReloadDone = flDone;		// UpdateMovingReload fills the clip
}

void CNPC_GermanSoldier::StartFallToHand( void )
{
	m_bFallToHandPending = true;
	m_flWantFullFlinch = gpGlobals->curtime;
	SetCondition( COND_HEAVY_DAMAGE );
	ClearSchedule( "German soldier falls to a hand" );	// SelectSchedule starts it next think
}

Activity CNPC_GermanSoldier::ResolveNamedActivity( const char *pszName )
{
	int iAct = ActivityList_IndexForName( pszName );
	return ( iAct >= 0 && HaveSequenceForActivity( (Activity)iAct ) ) ? (Activity)iAct : ACT_INVALID;
}

// Upper-body pain by where he was hit (both packs; a random pick among what exists).
Activity CNPC_GermanSoldier::PickUpperPain( void )
{
	static const char *s_Crouch[] = { "ACT_WW2_UPPER_CROUCH_PAIN" };
	static const char *s_Head[]   = { "ACT_WW2_UPPER_PAIN_HEAD" };
	static const char *s_Left[]   = { "ACT_WW2_UPPER_PAIN_LARM", "ACT_WW2_UPPER_PAIN_LSHOULDER" };
	static const char *s_Right[]  = { "ACT_WW2_UPPER_PAIN_RARM", "ACT_WW2_UPPER_PAIN_RSHOULDER" };
	static const char *s_Gut[]    = { "ACT_WW2_UPPER_PAIN_GUT", "ACT_WW2_UPPER_PAIN_HUNCH" };

	Activity a = ACT_INVALID;
	if ( m_bLastHitCrouched )
		a = PickAvailable( s_Crouch, ARRAYSIZE( s_Crouch ) );
	if ( a != ACT_INVALID )
		return a;

	const float flCentre = 3.0f;	// chest hits within this many units of the middle count as centre
	switch ( LastHitGroup() )
	{
	case HITGROUP_HEAD:		a = PickAvailable( s_Head, ARRAYSIZE( s_Head ) ); break;
	case HITGROUP_LEFTARM:	a = PickAvailable( s_Left, ARRAYSIZE( s_Left ) ); break;
	case HITGROUP_RIGHTARM:	a = PickAvailable( s_Right, ARRAYSIZE( s_Right ) ); break;
	case HITGROUP_CHEST:
	case HITGROUP_GENERIC:
	case HITGROUP_GEAR:
		if ( m_flLastHitSide > flCentre )		a = PickAvailable( s_Right, ARRAYSIZE( s_Right ) );
		else if ( m_flLastHitSide < -flCentre )	a = PickAvailable( s_Left, ARRAYSIZE( s_Left ) );
		break;
	default:
		break;
	}
	if ( a == ACT_INVALID )
		a = PickAvailable( s_Gut, ARRAYSIZE( s_Gut ) );	// stomach, legs, centre chest, fallback
	return a;
}

// Full-body flinch schedules are only used for "fall to a hand"; everything else is
// the upper-body overlay above. Returning nothing here also keeps the base AI from
// starting its own full-body flinch on heavy damage (one owner for pain reactions).
Activity CNPC_GermanSoldier::GetFlinchActivity( bool bHeavyDamage, bool bGesture )
{
	if ( bGesture )
		return BaseClass::GetFlinchActivity( bHeavyDamage, bGesture );
	if ( m_bFallToHandPending )
		return ResolveNamedActivity( "ACT_WW2_PAIN_FALLTOHAND" );
	return ACT_INVALID;
}

int CNPC_GermanSoldier::SelectSchedule( void )
{
	if ( m_flWantFullFlinch > 0.0f )
	{
		bool bFresh = gpGlobals->curtime - m_flWantFullFlinch < 0.5f;
		m_flWantFullFlinch = 0.0f;
		if ( bFresh && IsAlive() && m_bFallToHandPending && HaveSequenceForActivity( GetFlinchActivity( true, false ) ) )
			return SCHED_BIG_FLINCH;
		m_bFallToHandPending = false;
	}
	return BaseClass::SelectSchedule();
}

int CNPC_GermanSoldier::MeleeAttack1Conditions( float flDot, float flDist )
{
	// No rifle-butt animation with a hit event yet. Stage 2e.
	return COND_NONE;
}

//-----------------------------------------------------------------------------
// Test commands. Look at a German soldier (or have one roughly in view) and:
//   npc_german_test_spawn [weapon]        a soldier where you aim, facing you
//   npc_german_test_pain  [next|prev|N|name|all|list]
//   npc_german_test_death [next|prev|N|name|list]
//   npc_german_test_hit   <part> [damage]  a fake hit through the real reaction code
//   npc_german_test_crouch                toggle forced crouch
// "notarget" keeps soldiers from fighting you while you watch.
//-----------------------------------------------------------------------------
static bool IsPainTestActivity( const char *pszAct )
{
	return pszAct && ( Q_stristr( pszAct, "_UPPER_" ) || !Q_stricmp( pszAct, "ACT_WW2_PAIN_FALLTOHAND" ) );
}

static bool IsDeathTestActivity( const char *pszAct )
{
	if ( !pszAct || Q_stristr( pszAct, "UPPER" ) || Q_stristr( pszAct, "GESTURE" ) )
		return false;
	return !Q_strnicmp( pszAct, "ACT_DIE", 7 ) || Q_stristr( pszAct, "DEATH" ) != NULL;
}

void CNPC_GermanSoldier::GetTestSequences( bool bDeaths, CUtlVector<int> &list )
{
	list.RemoveAll();
	CStudioHdr *pHdr = GetModelPtr();
	if ( !pHdr )
		return;
	for ( int i = 0; i < pHdr->GetNumSeq(); i++ )
	{
		const char *pszAct = GetSequenceActivityName( i );
		if ( bDeaths ? IsDeathTestActivity( pszAct ) : IsPainTestActivity( pszAct ) )
			list.AddToTail( i );
	}
}

void CNPC_GermanSoldier::TestPain( int iSequence )
{
	const char *pszAct = GetSequenceActivityName( iSequence );
	Msg( "npc_german_soldier %d: pain %s (%s, %.1f s)\n", entindex(), GetSequenceName( iSequence ), pszAct, SequenceDuration( iSequence ) );

	if ( pszAct && !Q_stricmp( pszAct, "ACT_WW2_PAIN_FALLTOHAND" ) )
	{
		StartFallToHand();		// the full-body one goes through its real schedule
		return;
	}

	// cycling through pains: drop the previous test pain so they don't stack
	if ( m_iTestPainLayer >= 0 && IsValidLayer( m_iTestPainLayer ) )
		FastRemoveLayer( m_iTestPainLayer );
	m_flFlinchUntil = 0.0f;
	StartPainOverlay( iSequence );
	Activity act = (Activity)GetSequenceActivity( iSequence );
	m_iTestPainLayer = FindGestureLayer( act );
}

void CNPC_GermanSoldier::TestPainAll( void )
{
	m_iTestPainAllNext = 0;
	m_flTestPainAllTime = gpGlobals->curtime;
	Msg( "npc_german_soldier %d: playing every pain animation in turn\n", entindex() );
}

void CNPC_GermanSoldier::UpdateTestPainAll( void )
{
	if ( m_iTestPainAllNext < 0 || gpGlobals->curtime < m_flTestPainAllTime )
		return;
	CUtlVector<int> list;
	GetTestSequences( false, list );
	if ( m_iTestPainAllNext >= list.Count() )
	{
		Msg( "npc_german_soldier %d: done, %d pain animations\n", entindex(), list.Count() );
		m_iTestPainAllNext = -1;
		return;
	}
	int iSeq = list[m_iTestPainAllNext++];
	Msg( "  %d/%d ", m_iTestPainAllNext, list.Count() );
	TestPain( iSeq );
	m_flTestPainAllTime = gpGlobals->curtime + SequenceDuration( iSeq ) + 1.0f;	// a second of rest between
}

void CNPC_GermanSoldier::TestHit( int nHitGroup, float flSide, float flDamage, CBaseEntity *pAttacker )
{
	Vector vecRight;
	GetVectors( NULL, &vecRight, NULL );
	Vector vecPos = ( nHitGroup == HITGROUP_HEAD ) ? EyePosition() : WorldSpaceCenter();
	vecPos += vecRight * flSide;

	Vector vecDir = vecPos - ( pAttacker ? pAttacker->EyePosition() : vecPos - BodyDirection3D() );
	VectorNormalize( vecDir );

	CTakeDamageInfo info( pAttacker, pAttacker, flDamage, DMG_BULLET );
	info.SetDamagePosition( vecPos );
	info.SetDamageForce( vecDir * 1000.0f );
	SetLastHitGroup( nHitGroup );
	m_bTestForcePain = true;
	DoHitReaction( info );		// the reaction only; no health is taken
}

void CNPC_GermanSoldier::TestDeath( int iSequence, CBaseEntity *pAttacker )
{
	Msg( "npc_german_soldier %d: death %s (%s, %.1f s)\n", entindex(), GetSequenceName( iSequence ),
		 GetSequenceActivityName( iSequence ), SequenceDuration( iSequence ) );
	m_iForceDeathSeq = iSequence;
	m_iTestPainAllNext = -1;
	SetLastHitGroup( HITGROUP_CHEST );
	CTakeDamageInfo info( pAttacker, pAttacker, GetHealth() + 100.0f, DMG_BULLET );
	info.SetDamagePosition( WorldSpaceCenter() );
	info.SetDamageForce( vec3_origin );
	TakeDamage( info );
}

// --- console side ------------------------------------------------------------
static CNPC_GermanSoldier *GermanTestTarget( void )
{
	CBasePlayer *pPlayer = UTIL_GetLocalPlayer();
	if ( !pPlayer )
		return NULL;

	Vector vecEye = pPlayer->EyePosition(), vecFwd;
	pPlayer->EyeVectors( &vecFwd );

	trace_t tr;
	UTIL_TraceLine( vecEye, vecEye + vecFwd * 8192.0f, MASK_SHOT, pPlayer, COLLISION_GROUP_NONE, &tr );
	if ( tr.m_pEnt && FClassnameIs( tr.m_pEnt, "npc_german_soldier" ) && tr.m_pEnt->IsAlive() )
		return static_cast<CNPC_GermanSoldier *>( tr.m_pEnt );

	// Not right under the crosshair: the living soldier closest to it.
	CNPC_GermanSoldier *pBest = NULL;
	float flBest = 0.7f;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "npc_german_soldier" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "npc_german_soldier" ) )
	{
		if ( !pEnt->IsAlive() )
			continue;
		Vector vecTo = pEnt->WorldSpaceCenter() - vecEye;
		VectorNormalize( vecTo );
		float flDot = DotProduct( vecTo, vecFwd );
		if ( flDot > flBest )
		{
			flBest = flDot;
			pBest = static_cast<CNPC_GermanSoldier *>( pEnt );
		}
	}
	if ( !pBest )
		Msg( "No living German soldier in view. Look at one, or spawn one with npc_german_test_spawn.\n" );
	return pBest;
}

static void GermanTestPrintList( CNPC_GermanSoldier *pNPC, const CUtlVector<int> &list, const char *pszWhat )
{
	Msg( "%d %s animations (ww2_ = CoD2 pack, c1_ = CoD1 pack):\n", list.Count(), pszWhat );
	for ( int i = 0; i < list.Count(); i++ )
		Msg( "  %3d  %-40s %-40s %.1f s\n", i + 1, pNPC->GetSequenceName( list[i] ), pNPC->GetSequenceActivityName( list[i] ),
			 pNPC->SequenceDuration( list[i] ) );
}

// next / prev / a number from the list / part of a sequence or activity name. -1 = nothing.
static int GermanTestPick( CNPC_GermanSoldier *pNPC, const CUtlVector<int> &list, const char *pszArg, int &iCursor )
{
	int n = list.Count();
	if ( !n )
	{
		Msg( "This model has no such animations (are the WW2 animation packs compiled and included?)\n" );
		return -1;
	}
	if ( !pszArg[0] || !Q_stricmp( pszArg, "next" ) )
		iCursor = ( iCursor + 1 ) % n;
	else if ( !Q_stricmp( pszArg, "prev" ) )
		iCursor = ( iCursor - 1 + n ) % n;
	else if ( pszArg[0] >= '0' && pszArg[0] <= '9' )
	{
		int i = atoi( pszArg ) - 1;
		if ( i < 0 || i >= n )
		{
			Msg( "Pick 1 to %d (\"list\" shows them).\n", n );
			return -1;
		}
		iCursor = i;
	}
	else
	{
		int iFound = -1;
		for ( int pass = 0; pass < 2 && iFound < 0; pass++ )
		{
			for ( int i = 0; i < n && iFound < 0; i++ )
			{
				const char *psz = pass == 0 ? pNPC->GetSequenceName( list[i] ) : pNPC->GetSequenceActivityName( list[i] );
				if ( psz && Q_stristr( psz, pszArg ) )
					iFound = i;
			}
		}
		if ( iFound < 0 )
		{
			Msg( "Nothing matches \"%s\" (\"list\" shows them).\n", pszArg );
			return -1;
		}
		iCursor = iFound;
	}
	Msg( "[%d/%d] ", iCursor + 1, n );
	return list[iCursor];
}

CON_COMMAND( npc_german_test, "List the German soldier test commands." )
{
	Msg( "German soldier test commands (aim at a soldier; \"notarget\" stops them fighting you):\n"
		 "  npc_german_test_spawn [weapon]       spawn one where you aim, facing you (default weapon_k98)\n"
		 "  npc_german_test_pain  [arg]          play a pain animation on him\n"
		 "  npc_german_test_death [arg]          kill him with a chosen death animation\n"
		 "     arg: next (default) | prev | a number | part of a name (head, gut, falltohand ...) | list\n"
		 "     npc_german_test_pain all          plays every pain animation in turn on him\n"
		 "  npc_german_test_hit <part> [damage]  fake a hit through the real reaction code (no damage taken)\n"
		 "     part: head chest chestleft chestright stomach leftarm rightarm leftleg rightleg\n"
		 "  npc_german_test_crouch               toggle: force him to crouch (crouched pains and deaths)\n"
		 "Tip: bind keys, e.g.  bind KP_PLUS \"npc_german_test_pain next\"\n" );

	// Which animation packs did the soldier in view actually load?
	CNPC_GermanSoldier *pNPC = GermanTestTarget();
	CStudioHdr *pHdr = pNPC ? pNPC->GetModelPtr() : NULL;
	if ( !pHdr )
		return;
	const studiohdr_t *pRoot = pHdr->GetRenderHdr();
	Msg( "\nSoldier %d uses %s, which includes:\n", pNPC->entindex(), pRoot->pszName() );
	for ( int i = 0; i < pRoot->numincludemodels; i++ )
	{
		const char *pszInc = pRoot->pModelGroup( i )->pszName();
		Msg( "  %-45s %s\n", pszInc, filesystem->FileExists( pszInc, "GAME" ) ? "found" : "FILE NOT FOUND" );
	}
	int nCoD2 = 0, nCoD1 = 0;
	for ( int i = 0; i < pHdr->GetNumSeq(); i++ )
	{
		const char *pszSeq = pNPC->GetSequenceName( i );
		if ( !Q_strnicmp( pszSeq, "ww2_", 4 ) ) nCoD2++;
		else if ( !Q_strnicmp( pszSeq, "c1_", 3 ) ) nCoD1++;
	}
	Msg( "Sequences: %d total, CoD2 pack %d%s, CoD1 pack %d%s\n", pHdr->GetNumSeq(),
		 nCoD2, nCoD2 ? "" : " (NOT LOADED)", nCoD1, nCoD1 ? "" : " (NOT LOADED)" );
}

CON_COMMAND( npc_german_test_spawn, "Spawn a German soldier where you aim, facing you. Optional: weapon classname (default weapon_k98)." )
{
	CBasePlayer *pPlayer = UTIL_GetLocalPlayer();
	if ( !pPlayer )
		return;

	Vector vecEye = pPlayer->EyePosition(), vecFwd;
	pPlayer->EyeVectors( &vecFwd );
	trace_t tr;
	UTIL_TraceLine( vecEye, vecEye + vecFwd * 2048.0f, MASK_NPCSOLID, pPlayer, COLLISION_GROUP_NONE, &tr );

	// Step back from walls, then find the floor below.
	Vector vecPos = tr.endpos;
	if ( tr.plane.normal.z < 0.7f )
		vecPos -= vecFwd * 32.0f;
	UTIL_TraceLine( vecPos + Vector( 0, 0, 16 ), vecPos - Vector( 0, 0, 512 ), MASK_NPCSOLID, NULL, COLLISION_GROUP_NONE, &tr );
	vecPos = tr.endpos + Vector( 0, 0, 1 );

	CAI_BaseNPC *pNPC = dynamic_cast<CAI_BaseNPC *>( CreateEntityByName( "npc_german_soldier" ) );
	if ( !pNPC )
		return;
	pNPC->KeyValue( "additionalequipment", args.ArgC() > 1 ? args[1] : "weapon_k98" );
	Vector vecToPlayer = pPlayer->GetAbsOrigin() - vecPos;
	QAngle angFace( 0, UTIL_VecToYaw( vecToPlayer ), 0 );
	pNPC->SetAbsOrigin( vecPos );
	pNPC->SetAbsAngles( angFace );
	DispatchSpawn( pNPC );
	pNPC->Activate();
	Msg( "Spawned npc_german_soldier %d\n", pNPC->entindex() );
}

CON_COMMAND( npc_german_test_pain, "Play a pain animation on the German soldier you look at: next | prev | N | name | all | list" )
{
	CNPC_GermanSoldier *pNPC = GermanTestTarget();
	if ( !pNPC )
		return;
	CUtlVector<int> list;
	pNPC->GetTestSequences( false, list );
	const char *pszArg = args.ArgC() > 1 ? args[1] : "";
	if ( !Q_stricmp( pszArg, "list" ) )
	{
		GermanTestPrintList( pNPC, list, "pain" );
		return;
	}
	if ( !Q_stricmp( pszArg, "all" ) )
	{
		pNPC->TestPainAll();
		return;
	}
	static int s_iCursor = -1;
	int iSeq = GermanTestPick( pNPC, list, pszArg, s_iCursor );
	if ( iSeq >= 0 )
		pNPC->TestPain( iSeq );
}

CON_COMMAND( npc_german_test_death, "Kill the German soldier you look at with a chosen death animation: next | prev | N | name | list" )
{
	CNPC_GermanSoldier *pNPC = GermanTestTarget();
	if ( !pNPC )
		return;
	CUtlVector<int> list;
	pNPC->GetTestSequences( true, list );
	const char *pszArg = args.ArgC() > 1 ? args[1] : "";
	if ( !Q_stricmp( pszArg, "list" ) )
	{
		GermanTestPrintList( pNPC, list, "death" );
		return;
	}
	static int s_iCursor = -1;
	int iSeq = GermanTestPick( pNPC, list, pszArg, s_iCursor );
	if ( iSeq >= 0 )
		pNPC->TestDeath( iSeq, UTIL_GetLocalPlayer() );
}

CON_COMMAND( npc_german_test_hit, "Fake a hit on the German soldier you look at: <head|chest|chestleft|chestright|stomach|leftarm|rightarm|leftleg|rightleg> [damage]" )
{
	static const struct { const char *pszName; int nGroup; float flSide; } s_Parts[] = {
		{ "head", HITGROUP_HEAD, 0.0f }, { "chest", HITGROUP_CHEST, 0.0f },
		{ "chestleft", HITGROUP_CHEST, -6.0f }, { "chestright", HITGROUP_CHEST, 6.0f },
		{ "stomach", HITGROUP_STOMACH, 0.0f }, { "leftarm", HITGROUP_LEFTARM, -10.0f },
		{ "rightarm", HITGROUP_RIGHTARM, 10.0f }, { "leftleg", HITGROUP_LEFTLEG, -4.0f },
		{ "rightleg", HITGROUP_RIGHTLEG, 4.0f },
	};
	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: npc_german_test_hit <head|chest|chestleft|chestright|stomach|leftarm|rightarm|leftleg|rightleg> [damage, default 35]\n" );
		return;
	}
	int iPart = -1;
	for ( int i = 0; i < (int)ARRAYSIZE( s_Parts ); i++ )
	{
		if ( !Q_stricmp( args[1], s_Parts[i].pszName ) )
			iPart = i;
	}
	if ( iPart < 0 )
	{
		Msg( "Unknown body part \"%s\".\n", args[1] );
		return;
	}
	CNPC_GermanSoldier *pNPC = GermanTestTarget();
	if ( !pNPC )
		return;
	float flDamage = args.ArgC() > 2 ? atof( args[2] ) : 35.0f;
	pNPC->TestHit( s_Parts[iPart].nGroup, s_Parts[iPart].flSide, MAX( flDamage, 1.0f ), UTIL_GetLocalPlayer() );
}

CON_COMMAND( npc_german_test_crouch, "Toggle forced crouching on the German soldier you look at." )
{
	CNPC_GermanSoldier *pNPC = GermanTestTarget();
	if ( !pNPC )
		return;
	if ( pNPC->CrouchIsDesired() )
	{
		pNPC->ClearForceCrouch();
		Msg( "npc_german_soldier %d: stands\n", pNPC->entindex() );
	}
	else
	{
		pNPC->ForceCrouch();
		Msg( "npc_german_soldier %d: forced to crouch\n", pNPC->entindex() );
	}
}
