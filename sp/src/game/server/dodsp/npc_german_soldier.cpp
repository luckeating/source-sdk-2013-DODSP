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
//     * standing still, full-body CoD moments via SCHED_BIG_FLINCH: head hits play the
//       CoD2 helmet hit (a duck - it doesn't work as an overlay), big body hits sometimes
//       "fall to a hand" (CoD1). Head hits on the move: the kick's head snap only.
//
// Test commands (npc_german_test for the list): spawn soldiers, play any pain or
// death animation on the soldier you look at, fake hits by body part, force crouch.
//
//=============================================================================//

#include "cbase.h"
#include "ai_baseactor.h"
#include "ai_squad.h"
#include "ai_squadslot.h"
#include "soundent.h"
#include "ai_network.h"
#include "ai_node.h"
#include "ai_navigator.h"
#include "ai_senses.h"
#include "ai_pathfinder.h"
#include "ai_waypoint.h"
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

ConVar npc_german_debug( "npc_german_debug", "0", 0, "1 = print decisions, activity translations and missing animations. 2 = also an overlay over each soldier (schedule, animation, cover) with the cover traces drawn. 3 = also aim values." );

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
// Stage A1: built straight on the engine's NPC actor (navigation, senses, squads, cover search,
// move-and-shoot, behaviours) with his own combat brain - no Combine soldier underneath.
class CNPC_GermanSoldier : public CAI_BaseActor
{
	DECLARE_CLASS( CNPC_GermanSoldier, CAI_BaseActor );
	DEFINE_CUSTOM_AI;

public:
	DECLARE_SERVERCLASS();

	enum
	{
		SCHED_GERMAN_COVER_HIDE = BaseClass::NEXT_SCHEDULE,	// duck behind low cover between bursts
		SCHED_GERMAN_MOVE_TO_COVER,							// run to the best-scoring low cover nearby
		SCHED_GERMAN_HOLD_COVER,							// can't see / reach the enemy: stay in cover, watch his last spot
		NEXT_SCHEDULE
	};
	enum
	{
		TASK_GERMAN_HIDE = BaseClass::NEXT_TASK,
		TASK_GERMAN_PATH_TO_COVER,
		TASK_GERMAN_WATCH,
		NEXT_TASK
	};
	enum
	{
		COND_GERMAN_FLANKED = BaseClass::NEXT_CONDITION,	// the enemy can see where he hides
		NEXT_CONDITION
	};

	CNPC_GermanSoldier();

	void		Precache( void );
	void		Spawn( void );

	Activity	NPC_TranslateActivity( Activity eNewActivity );
	Activity	TranslateToDoD( Activity eNewActivity );
	void		OnChangeActivity( Activity eNewActivity );

	void		PrescheduleThink( void );
	void		OnRangeAttack1( void );

	void		StartTask( const Task_t *pTask );
	void		RunTask( const Task_t *pTask );

	int			MeleeAttack1Conditions( float flDot, float flDist );

	// What the Combine soldier used to provide
	Class_T		Classify( void ) { return CLASS_COMBINE; }		// enemy of the player (a WW2 faction class later)
	int			GetSoundInterests( void );
	float		MaxYawSpeed( void );
	Vector		Weapon_ShootPosition( void );
	int			SelectCombatSchedule( void );
	int			Decide( int iSched, const char *pszWhy );	// every combat choice goes through here (decision log)
	int			SelectFailSchedule( int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode );
	void		MarkSpotBad( const Vector &vecPos );		// unreachable / got stuck: skip it for a while
	bool		IsSpotBad( const Vector &vecPos ) const;
	void		DrawDebugOverlay( void );

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
	bool		TestToggleCrouch( void );

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
	void		StartFullBodyPain( Activity act );
	void		StartPainOverlay( int iSequence );
	void		UpdatePainRecovery( void );
	bool		IsUpperBodyGesture( Activity act ) const;
	void		UpdateTestPainAll( void );
	// One owner for "the upper body is taken by a hit reaction": no shots, no fire or
	// reload gestures until then (pain overlays, the kick's short stun).
	bool		IsHitReacting( void ) const { return gpGlobals->curtime < m_flFlinchUntil; }
	// ...or by a cover transition (rising from / ducking into cover): no shots then either.
	bool		IsUpperBodyBusy( void ) { return IsHitReacting() || IsCoverTransition( GetTranslatedActivity() ); }
	// Low cover (stage 3b-1)
	enum CoverFamily_t { POSE_OTHER, POSE_HIDE, POSE_CROUCH, POSE_STAND };
	CoverFamily_t PoseFamily( Activity act ) const;
	bool		IsCoverTransition( Activity act ) const;
	bool		IsHidingInCover( void );
	enum { COVER_EXPOSED = -1, COVER_FULL = 0, COVER_CROUCH = 1, COVER_STAND = 2 };
	int			LowCoverAgainst( CBaseEntity *pEnemy );	// COVER_*: > 0 means low cover he can fire from
	int			CoverAt( const Vector &vecPos, CBaseEntity *pEnemy, trace_t *pHideTrace = NULL );	// same test at any spot
	bool		FindCoverSpot( CBaseEntity *pEnemy, bool bAdvance = false );	// best-scoring low cover nearby (bAdvance: only spots closer to him)
	bool		IsAtCoverSpot( void ) const { return m_bHasCoverSpot && ( m_vecCoverSpot - GetAbsOrigin() ).Length2D() < 48.0f; }
	bool		IsCoverSpotTaken( const Vector &vecPos );
	bool		IsWounded( void ) const { return GetMaxHealth() > 0 && GetHealth() < GetMaxHealth() * m_flWoundedHealth; }
	void		NoteUnderFire( void );
	void		UpdateCoverAwareness( void );
	void		UpdateCoverTransition( void );
	bool		IsInCrouchPose( void );
	Activity	PickUpperPain( void );
	Activity	PickFullBodyPain( const CTakeDamageInfo &info );
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
	Activity			m_actHide;				// CoD2 low-wall hide pose, and the moves in and out of it
	Activity			m_actHide2Aim;
	Activity			m_actAim2Hide;
	Activity			m_actStand2Hide;
	Activity			m_actHide2Stand;
	float				m_flCoverHideMin;		// script: seconds hidden between bursts
	float				m_flCoverHideMax;
	float				m_flCoverMaxGap;		// script: the cover must be this close to him (units)
	float				m_flCoverSearchRadius;	// script: how far he'll run for cover
	Vector				m_vecCoverSpot;			// where he's going / sitting (claimed: squadmates pick other spots)
	bool				m_bHasCoverSpot;
	float				m_flNextCoverSearch;
	float				m_flNextAdvance;		// bounding forward to closer cover: not more often than this
	float				m_flCoverHoldTime;		// script: enemy out of sight this long -> advance to closer cover
	float				m_flLastCombatTime;		// last think in combat (alert-state holding)
	float				m_flWatchUntil;
	enum { NUM_BAD_SPOTS = 8 };
	Vector				m_vecBadSpot[NUM_BAD_SPOTS];	// refusal memory: spots he couldn't reach
	float				m_flBadSpotUntil[NUM_BAD_SPOTS];
	int					m_iNextBadSpot;
	float				m_flProgressCheck;		// stuck guard while running to cover
	float				m_flProgressDist;
	int					m_iLastDecision;
	const char		   *m_pszLastWhy;
	float				m_flCoverHideUntil;
	float				m_flLastBurstEnd;		// a standing/crouched burst just finished (cover rhythm)
	float				m_flCoverCheckTime;		// LowCoverAgainst cache
	int					m_nCoverCheckResult;
	EHANDLE				m_hCoverCheckEnemy;
	Activity			m_actCoverReload;		// CoD2 reload done hidden behind low cover
	Activity			m_actCoverLowFire;		// CoD2 firing over a low wall (crouched) / a chest-high one (standing)
	Activity			m_actCoverMidFire;
	bool				m_bCoverFireSemi;		// single shots: the firing loop plays once per shot
	float				m_flCoverRecoilUntil;
	bool				m_bInCoverSpot;			// he has hidden at this spot (cleared when he moves)
	float				m_flWantHideTime;		// something asked him to duck (under fire, a pain ended)
	float				m_flUnderFireUntil;		// near misses / hits: stay down until then
	float				m_flUnderFireHide;		// script tuning
	float				m_flWoundedHealth;
	float				m_flWoundedHideScale;
	float				m_flWoundedBurstScale;
	float				m_flHideStarted;
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
	float				m_flStillPainChance;	// chance a hit on a soldier standing/crouched still plays a full-body pain
	float				m_flLastHitSide;		// where the last hit landed across his body: + = his right, - = his left (units)
	float				m_flLastHitDamage;
	bool				m_bLastHitCrouched;		// crouch state at the moment of the last hit
	Activity			m_actFullBodyPain;		// pending full-body pain (fall to a hand, helmet hit); ACT_INVALID = none
	bool				m_bReloadInterrupted;	// a pain cut a reload short; it restarts when the pain lets go

	// Test commands (not saved)
	bool				m_bTestForcePain;		// next DoHitReaction skips chance and cooldown
	int					m_iTestPainLayer;		// overlay layer started by npc_german_test_pain
	int					m_iForceDeathSeq;		// npc_german_test_death: exact death sequence, -1 = none
	int					m_iForceFlinchSeq;		// npc_german_test_pain: exact full-body pain sequence, -1 = none
	int					m_iTestPainAllNext;		// npc_german_test_pain all: next list entry, -1 = off
	float				m_flTestPainAllTime;
	bool				m_bTestCrouch;			// npc_german_test_crouch: hold a crouch whatever the AI wants

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
	m_actHide = m_actHide2Aim = m_actAim2Hide = m_actStand2Hide = m_actHide2Stand = ACT_INVALID;
	m_flCoverHideMin = 1.5f;
	m_flCoverHideMax = 4.0f;
	m_flCoverMaxGap = 128.0f;
	m_flCoverSearchRadius = 900.0f;
	m_vecCoverSpot = vec3_origin;
	m_bHasCoverSpot = false;
	m_flNextCoverSearch = 0.0f;
	m_flNextAdvance = 0.0f;
	m_flCoverHoldTime = 8.0f;
	m_flLastCombatTime = -1000.0f;
	m_flWatchUntil = 0.0f;
	for ( int i = 0; i < NUM_BAD_SPOTS; i++ )
	{
		m_vecBadSpot[i] = vec3_origin;
		m_flBadSpotUntil[i] = 0.0f;
	}
	m_iNextBadSpot = 0;
	m_flProgressCheck = 0.0f;
	m_flProgressDist = 0.0f;
	m_iLastDecision = -1;
	m_pszLastWhy = NULL;
	m_flCoverHideUntil = 0.0f;
	m_flLastBurstEnd = 0.0f;
	m_flCoverCheckTime = 0.0f;
	m_nCoverCheckResult = 0;
	m_actCoverReload = ACT_INVALID;
	m_actCoverLowFire = m_actCoverMidFire = ACT_INVALID;
	m_bCoverFireSemi = true;
	m_flCoverRecoilUntil = 0.0f;
	m_bInCoverSpot = false;
	m_flWantHideTime = 0.0f;
	m_flUnderFireUntil = 0.0f;
	m_flUnderFireHide = 2.5f;
	m_flWoundedHealth = 0.5f;
	m_flWoundedHideScale = 1.75f;
	m_flWoundedBurstScale = 0.5f;
	m_flHideStarted = 0.0f;
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
	m_flStillPainChance = 1.0f;
	m_flFullFlinchCooldown = 3.0f;
	m_flLastHitSide = 0.0f;
	m_flLastHitDamage = 0.0f;
	m_bLastHitCrouched = false;
	m_actFullBodyPain = ACT_INVALID;
	m_bReloadInterrupted = false;
	m_bTestForcePain = false;
	m_iTestPainLayer = -1;
	m_iForceDeathSeq = -1;
	m_iForceFlinchSeq = -1;
	m_iTestPainAllNext = -1;
	m_flTestPainAllTime = 0.0f;
	m_bTestCrouch = false;
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
	PrecacheModel( STRING( GetModelName() ) );

	if ( !g_pGermanScript )
	{
		GermanSoldier_LoadScript();
	}

	BaseClass::Precache();
}

void CNPC_GermanSoldier::Spawn( void )
{
	Precache();
	SetModel( STRING( GetModelName() ) );

	SetHullType( HULL_HUMAN );
	SetHullSizeNormal();
	SetSolid( SOLID_BBOX );
	AddSolidFlags( FSOLID_NOT_STANDABLE );
	SetMoveType( MOVETYPE_STEP );
	SetBloodColor( BLOOD_COLOR_RED );
	m_flFieldOfView = -0.2f;		// wide forward view cone
	m_NPCState = NPC_STATE_NONE;

	int iHealth = g_pGermanScript ? g_pGermanScript->GetInt( "health", 50 ) : 50;
	if ( m_iHealth <= 0 || m_iHealth == 1 )		// a health set in Hammer wins
		m_iHealth = iHealth;
	SetMaxHealth( m_iHealth );

	CapabilitiesAdd( bits_CAP_TURN_HEAD | bits_CAP_MOVE_GROUND | bits_CAP_AIM_GUN | bits_CAP_SQUAD | bits_CAP_USE_WEAPONS |
					 bits_CAP_DUCK | bits_CAP_NO_HIT_SQUADMATES | bits_CAP_MOVE_SHOOT | bits_CAP_DOORS_GROUP );

	m_HackedGunPos = Vector( 0, 0, 58 );
	m_MoveAndShootOverlay.SetInitialDelay( 0.75f );

	NPCInit();

	SetViewOffset( Vector( 0, 0, 64 ) );	// DoD German's eyes standing (crouched: GetCrouchEyeOffset)
	// How far he sees. The engine default (2048) made him lose you across a modest field, go
	// "alert" and wander off his cover. Rifles reach further than that.
	GetSenses()->SetDistLook( g_pGermanScript ? clamp( g_pGermanScript->GetFloat( "sight_distance", 16000.0f ), 512.0f, 32768.0f ) : 16000.0f );
	HoldMoveForward();
}

int CNPC_GermanSoldier::GetSoundInterests( void )
{
	return SOUND_WORLD | SOUND_COMBAT | SOUND_PLAYER | SOUND_DANGER | SOUND_PHYSICS_DANGER | SOUND_BULLET_IMPACT | SOUND_MOVE_AWAY;
}

float CNPC_GermanSoldier::MaxYawSpeed( void )
{
	switch ( GetActivity() )
	{
	case ACT_RUN:
	case ACT_RUN_HURT:		return 15;
	case ACT_WALK:
	case ACT_WALK_CROUCH:	return 25;
	default:				return 35;
	}
}

Vector CNPC_GermanSoldier::Weapon_ShootPosition( void )
{
	if ( IsCrouching() || IsCrouchedActivity( GetActivity() ) )
		return GetAbsOrigin() + GetCrouchGunOffset();
	return GetAbsOrigin() + m_HackedGunPos;
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
	if ( npc_german_debug.GetInt() >= 3 && gpGlobals->curtime >= m_flNextAimDebug )
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
	m_actHide = m_actHide2Aim = m_actAim2Hide = m_actStand2Hide = m_actHide2Stand = ACT_INVALID;
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
	m_flCoverHideMin = MAX( 0.2f, pScript->GetFloat( "cover_hide_min", 1.5f ) );
	m_flCoverHideMax = MAX( m_flCoverHideMin, pScript->GetFloat( "cover_hide_max", 4.0f ) );
	m_flCoverMaxGap = clamp( pScript->GetFloat( "cover_max_gap", 128.0f ), 16.0f, 512.0f );
	m_flCoverSearchRadius = clamp( pScript->GetFloat( "cover_search_radius", 900.0f ), 128.0f, 4096.0f );
	m_flCoverHoldTime = clamp( pScript->GetFloat( "cover_hold_time", 8.0f ), 0.0f, 120.0f );
	m_flUnderFireHide = MAX( 0.0f, pScript->GetFloat( "under_fire_hide", 2.5f ) );
	m_flWoundedHealth = clamp( pScript->GetFloat( "wounded_health", 0.5f ), 0.0f, 1.0f );
	m_flWoundedHideScale = MAX( 1.0f, pScript->GetFloat( "wounded_hide_scale", 1.75f ) );
	m_flWoundedBurstScale = clamp( pScript->GetFloat( "wounded_burst_scale", 0.5f ), 0.1f, 1.0f );
	m_flStillPainChance = clamp( pScript->GetFloat( "still_pain_chance", 1.0f ), 0.0f, 1.0f );

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

	// Low cover: CoD2's low-wall set. One hide pose that rises to a crouched firing pose over
	// a low wall or to a standing one over a chest-high wall, fires, ducks back and reloads
	// hidden - all chained. (Built for walls: hides below ~30 units, fires over up to ~42
	// crouched / ~58 standing.)
	m_actHide		= ResolveNamedActivity( "ACT_WW2_COVER_LOW_HIDE" );
	m_actHide2Aim	= ResolveNamedActivity( "ACT_WW2_COVER_LOW_UP" );
	m_actAim2Hide	= ResolveNamedActivity( "ACT_WW2_COVER_LOW_DOWN" );
	m_actStand2Hide	= ResolveNamedActivity( "ACT_WW2_COVER_MID_DOWN" );
	m_actHide2Stand	= ResolveNamedActivity( "ACT_WW2_COVER_MID_UP" );
	m_actCoverReload	= ResolveNamedActivity( "ACT_WW2_COVER_LOW_RELOAD" );
	{
		// automatic weapons fire the burst loop, everything else single shots
		m_bCoverFireSemi = !( Q_stristr( pszSet, "MP40" ) || Q_stristr( pszSet, "MP44" ) || Q_stristr( pszSet, "TOMMY" ) ||
							  Q_stristr( pszSet, "BAR" ) || Q_stristr( pszSet, "30CAL" ) || Q_stristr( pszSet, "MG" ) );
		m_actCoverLowFire = ResolveNamedActivity( m_bCoverFireSemi ? "ACT_WW2_COVER_LOW_SEMI" : "ACT_WW2_COVER_LOW_AUTO" );
		m_actCoverMidFire = ResolveNamedActivity( m_bCoverFireSemi ? "ACT_WW2_COVER_MID_SEMI" : "ACT_WW2_COVER_MID_AUTO" );
	}

	if ( npc_german_debug.GetBool() )
	{
		Msg( "npc_german_soldier %d: weapon %s -> DoD set %s, %d activities mapped, fire event %d\n",
			entindex(), pszWeapon, pszSet, m_AnimMap.Count(), m_iFireEvent );
	}
}

//-----------------------------------------------------------------------------
// Low cover (stage 3b-1). The Combine AI waits in cover (SCHED_COMBINE_WAIT_IN_COVER,
// crouched) and pops out to shoot when it has a line of fire. While waiting he now
// hides fully behind the cover in CoD1's hide pose; between poses the CoD1 transition
// plays first (hide -> crouched aim, aim -> hide, stand -> hide, hide -> stand), and no
// shots go off until he's up. One rule table, applied wherever an animation is chosen.
//-----------------------------------------------------------------------------
CNPC_GermanSoldier::CoverFamily_t CNPC_GermanSoldier::PoseFamily( Activity act ) const
{
	if ( act == ACT_INVALID )
		return POSE_OTHER;
	if ( act == m_actHide || act == m_actCoverReload )
		return POSE_HIDE;
	if ( act == m_actCoverLowFire )
		return POSE_CROUCH;
	if ( act == m_actCoverMidFire )
		return POSE_STAND;
	const char *psz = ActivityList_NameForIndex( act );
	if ( !psz )
		return POSE_OTHER;
	if ( !Q_strnicmp( psz, "ACT_DOD_CROUCH_", 15 ) )		// crouched aim / idle (not CROUCHWALK)
		return POSE_CROUCH;
	if ( !Q_strnicmp( psz, "ACT_DOD_STAND_", 14 ) )
		return POSE_STAND;
	return POSE_OTHER;
}

bool CNPC_GermanSoldier::IsCoverTransition( Activity act ) const
{
	return act != ACT_INVALID && ( act == m_actHide2Aim || act == m_actAim2Hide || act == m_actStand2Hide || act == m_actHide2Stand );
}

bool CNPC_GermanSoldier::IsHidingInCover( void )
{
	if ( m_actHide == ACT_INVALID || !GetEnemy() || !IsCrouching() )
		return false;
	return IsCurSchedule( SCHED_GERMAN_COVER_HIDE, false );
}

// Is this spot low cover against that enemy? Measured, so it works behind any wall, prop or
// sandbag, with or without hint nodes: the enemy can't see where his hidden helmet would be,
// the thing in the way is right in front of him, and he can see the enemy from a crouched
// (1) or standing (2) firing position.
#define COVER_HIDE_TOP		32.0f	// top of the helmet in the CoD2 hide pose (+2)
#define COVER_CROUCH_EYE	44.0f	// muzzle height firing over a low wall crouched
#define COVER_STAND_EYE		62.0f	// ...and standing
#define COVER_MASK			( CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_BLOCKLOS )	// what stops bullets / sight
int CNPC_GermanSoldier::LowCoverAgainst( CBaseEntity *pEnemy )
{
	if ( !pEnemy )
		return 0;
	if ( pEnemy == m_hCoverCheckEnemy.Get() && gpGlobals->curtime < m_flCoverCheckTime )
		return m_nCoverCheckResult;
	m_hCoverCheckEnemy = pEnemy;
	m_flCoverCheckTime = gpGlobals->curtime + 0.5f;
	m_nCoverCheckResult = COVER_EXPOSED;

	trace_t tr;
	m_nCoverCheckResult = CoverAt( GetAbsOrigin(), pEnemy, &tr );
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	Vector vecEye = pEnemy->EyePosition();
	Vector vecOrg = GetAbsOrigin();
	Vector vecHide = vecOrg + Vector( 0, 0, COVER_HIDE_TOP );
	bool bHidden = m_nCoverCheckResult != COVER_EXPOSED;
	bool bDraw = npc_german_debug.GetInt() >= 2;
	if ( bDraw )
	{
		// Drawn from HIS side so you can see them from where you stand: a post where his helmet
		// would be (green hidden / red seen) with a line from it toward you up to whatever blocks.
		NDebugOverlay::Line( vecOrg, vecHide, bHidden ? 0 : 255, bHidden ? 255 : 0, 0, true, 0.15f );
		NDebugOverlay::Line( vecHide, tr.fraction < 1.0f ? tr.endpos : vecHide + ( vecEye - vecHide ).Normalized() * 160.0f,
							 bHidden ? 0 : 255, bHidden ? 255 : 0, 0, true, 0.15f );
		if ( tr.fraction < 1.0f )
			NDebugOverlay::Box( tr.endpos, Vector( -3, -3, -3 ), Vector( 3, 3, 3 ), 255, 255, 0, 96, 0.15f );

		// What's actually in front of him toward you: scan out to 512 at heights 4..90.
		Vector vecDir = vecEye - vecOrg; vecDir.z = 0; VectorNormalize( vecDir );
		float flTop = 0.0f, flGap = 0.0f;
		const char *pszWhat = "";
		for ( float z = 4.0f; z <= 90.0f; z += 2.0f )
		{
			trace_t tw;
			UTIL_TraceLine( vecOrg + Vector( 0, 0, z ), vecOrg + Vector( 0, 0, z ) + vecDir * 512.0f, COVER_MASK, &filter, &tw );
			if ( tw.fraction < 1.0f && ( flTop == 0.0f || tw.fraction * 512.0f < flGap + 24.0f ) )
			{
				flTop = z; flGap = tw.fraction * 512.0f;
				pszWhat = tw.m_pEnt ? ( tw.m_pEnt->IsWorld() ? "world/static prop" : tw.m_pEnt->GetClassname() ) : "?";
			}
		}
		char sz[200];
		if ( flTop > 0.0f )
			Q_snprintf( sz, sizeof( sz ), "in front of him: %s ~%.0f tall, %.0f away%s", pszWhat, flTop, flGap,
						flGap > m_flCoverMaxGap ? "  (TOO FAR - cover_max_gap)" : flTop < COVER_HIDE_TOP ? "  (TOO LOW to hide behind)" : "" );
		else
			Q_snprintf( sz, sizeof( sz ), "in front of him: nothing solid within 512 toward you" );
		NDebugOverlay::EntityText( entindex(), 5, sz, 0.15f );
		NDebugOverlay::EntityText( entindex(), 6, bHidden ? "hide spot: hidden from you" :
								   tr.fraction < 1.0f ? "hide spot: blocked, but by something too far from him" : "hide spot: YOU CAN SEE IT", 0.15f );
	}
	if ( bDraw && m_bHasCoverSpot )
		NDebugOverlay::Box( m_vecCoverSpot, Vector( -8, -8, 0 ), Vector( 8, 8, 4 ), 0, 128, 255, 96, 0.15f );	// his chosen spot
	return m_nCoverCheckResult;
}

// The cover test at any spot (feet position): is it low cover against this enemy?
int CNPC_GermanSoldier::CoverAt( const Vector &vecPos, CBaseEntity *pEnemy, trace_t *pHideTrace )
{
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	Vector vecEye = pEnemy->EyePosition();
	trace_t tr;
	Vector vecHide = vecPos + Vector( 0, 0, COVER_HIDE_TOP );
	UTIL_TraceLine( vecEye, vecHide, COVER_MASK, &filter, &tr );
	if ( pHideTrace )
		*pHideTrace = tr;
	if ( tr.fraction >= 1.0f || ( vecHide - tr.endpos ).Length2D() > m_flCoverMaxGap )
		return COVER_EXPOSED;	// he'd be seen hiding here, or what's in the way is far off (not his cover)
	UTIL_TraceLine( vecEye, vecPos + Vector( 0, 0, COVER_CROUCH_EYE ), COVER_MASK, &filter, &tr );
	if ( tr.fraction >= 1.0f )
		return COVER_CROUCH;
	UTIL_TraceLine( vecEye, vecPos + Vector( 0, 0, COVER_STAND_EYE ), COVER_MASK, &filter, &tr );
	if ( tr.fraction >= 1.0f )
		return COVER_STAND;
	return COVER_FULL;	// full cover: can't shoot from here at all
}

bool CNPC_GermanSoldier::IsCoverSpotTaken( const Vector &vecPos )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "npc_german_soldier" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "npc_german_soldier" ) )
	{
		if ( pEnt == this || !pEnt->IsAlive() )
			continue;
		CNPC_GermanSoldier *pOther = static_cast<CNPC_GermanSoldier *>( pEnt );
		if ( ( pOther->m_bHasCoverSpot && ( pOther->m_vecCoverSpot - vecPos ).Length2D() < 40.0f ) ||
			 ( pOther->GetAbsOrigin() - vecPos ).Length2D() < 32.0f )
			return true;
	}
	return false;
}

// Position evaluation (Killzone-style): score every ground node within reach by the cover it
// gives against this enemy, how far he has to run, and how close it puts him to the enemy.
// Squadmates' spots are skipped. Picks the best; the move schedule finds the path.
void CNPC_GermanSoldier::MarkSpotBad( const Vector &vecPos )
{
	m_vecBadSpot[m_iNextBadSpot] = vecPos;
	m_flBadSpotUntil[m_iNextBadSpot] = gpGlobals->curtime + 15.0f;
	m_iNextBadSpot = ( m_iNextBadSpot + 1 ) % NUM_BAD_SPOTS;
}

bool CNPC_GermanSoldier::IsSpotBad( const Vector &vecPos ) const
{
	for ( int i = 0; i < NUM_BAD_SPOTS; i++ )
		if ( gpGlobals->curtime < m_flBadSpotUntil[i] && ( m_vecBadSpot[i] - vecPos ).Length2D() < 32.0f )
			return true;
	return false;
}

// Position evaluation (Killzone-style): score every ground node within reach by the cover it
// gives against this enemy, how far he has to run, and how close it puts him to the enemy.
// Squadmates' spots and spots he recently failed to reach are skipped. The best few are then
// checked with a real route (the node graph), and the run is scored by the route's length -
// a spot on the far side of a wall is a long run, not a short one, or no run at all.
bool CNPC_GermanSoldier::FindCoverSpot( CBaseEntity *pEnemy, bool bAdvance )
{
	if ( !pEnemy || !g_pBigAINet )
		return false;
	Vector vecMe = GetAbsOrigin(), vecFoe = pEnemy->GetAbsOrigin();
	float flMyFoe = ( vecMe - vecFoe ).Length2D();

	struct Cand_t { Vector pos; float cover; float foe; float score; };
	const int MAX_CANDS = 6;
	Cand_t cands[MAX_CANDS];
	int nCands = 0, nTested = 0;
	for ( int i = 0; i < g_pBigAINet->NumNodes(); i++ )
	{
		CAI_Node *pNode = g_pBigAINet->GetNode( i, false );
		if ( !pNode || pNode->GetType() != NODE_GROUND || pNode->IsLocked() )
			continue;
		Vector vecPos = pNode->GetPosition( GetHullType() );
		float flDist = ( vecPos - vecMe ).Length();
		if ( flDist > m_flCoverSearchRadius || fabsf( vecPos.z - vecMe.z ) > 160.0f || flDist < 24.0f )
			continue;
		float flFoe = ( vecPos - vecFoe ).Length2D();
		if ( flFoe < 192.0f || IsSpotBad( vecPos ) || IsCoverSpotTaken( vecPos ) )
			continue;
		if ( bAdvance && flFoe > flMyFoe - 128.0f )
			continue;	// bounding forward: only cover that actually gains ground
		nTested++;
		int nCover = CoverAt( vecPos, pEnemy );
		if ( nCover <= COVER_FULL )
			continue;
		Cand_t c;
		c.pos = vecPos;
		c.cover = ( nCover == COVER_CROUCH ? 100.0f : 80.0f )
				- ( flFoe < 400.0f ? ( 400.0f - flFoe ) * 0.1f : 0.0f );	// not right in his face
		c.foe = flFoe;
		c.score = c.cover - flDist * 0.08f;		// straight-line estimate (a route is never shorter)
		// keep the best MAX_CANDS by estimate
		int iSlot = nCands < MAX_CANDS ? nCands++ : -1;
		if ( iSlot < 0 )
		{
			int iWorst = 0;
			for ( int k = 1; k < MAX_CANDS; k++ )
				if ( cands[k].score < cands[iWorst].score )
					iWorst = k;
			if ( cands[iWorst].score >= c.score )
				continue;
			iSlot = iWorst;
		}
		cands[iSlot] = c;
	}

	// Check the candidates best-first with a real route; score by route length.
	float flBest = -FLT_MAX;
	Vector vecBest = vec3_origin;
	int nNoRoute = 0;
	for ( int pass = 0; pass < nCands; pass++ )
	{
		int iTop = -1;
		for ( int k = 0; k < nCands; k++ )
			if ( cands[k].score > -FLT_MAX && ( iTop < 0 || cands[k].score > cands[iTop].score ) )
				iTop = k;
		if ( iTop < 0 || cands[iTop].score <= flBest )
			break;	// estimates only get worse from here; a route can't beat its estimate
		Cand_t &c = cands[iTop];
		c.score = -FLT_MAX;		// taken
		AI_Waypoint_t *pRoute = GetPathfinder()->BuildRoute( vecMe, c.pos, NULL, 16.0f, NAV_GROUND );
		if ( !pRoute )
		{
			nNoRoute++;
			MarkSpotBad( c.pos );
			continue;
		}
		float flLen = 0.0f;
		Vector vecPrev = vecMe;
		for ( AI_Waypoint_t *pWp = pRoute; pWp; pWp = pWp->GetNext() )
		{
			flLen += ( pWp->GetPos() - vecPrev ).Length();
			vecPrev = pWp->GetPos();
		}
		DeleteAll( pRoute );
		if ( flLen > m_flCoverSearchRadius * 1.5f )
			continue;	// reachable, but only the long way round
		float flScore = c.cover - flLen * 0.08f;
		if ( flScore > flBest )
		{
			flBest = flScore;
			vecBest = c.pos;
		}
	}
	if ( npc_german_debug.GetBool() )
		Msg( "npc_german_soldier %d: %s - %d spots tested, %d gave cover, %d had no route: %s\n", entindex(),
			 bAdvance ? "advance search (closer cover)" : "cover search", nTested, nCands, nNoRoute,
			 flBest > -FLT_MAX ? "going" : "nothing usable" );
	if ( flBest == -FLT_MAX )
		return false;
	m_vecCoverSpot = vecBest;
	m_bHasCoverSpot = true;
	return true;
}

// Rounds hitting near him (or into him): keep his head down for a while; if he's up in
// low cover, duck right now.
void CNPC_GermanSoldier::NoteUnderFire( void )
{
	m_flUnderFireUntil = gpGlobals->curtime + m_flUnderFireHide;
	if ( !GetEnemy() || IsCurSchedule( SCHED_GERMAN_COVER_HIDE, false ) || IsCurSchedule( SCHED_BIG_FLINCH, false ) ||
		 m_actFullBodyPain != ACT_INVALID || LowCoverAgainst( GetEnemy() ) <= COVER_FULL )
		return;
	m_flWantHideTime = gpGlobals->curtime;
	const Task_t *pTask = GetTask();
	if ( pTask && ( pTask->iTask == TASK_RANGE_ATTACK1 || pTask->iTask == TASK_FACE_ENEMY || pTask->iTask == TASK_WAIT_RANDOM ) )
		ClearSchedule( "German soldier ducks under fire" );
}

void CNPC_GermanSoldier::UpdateCoverAwareness( void )
{
	if ( !IsAlive() )
		return;
	if ( IsMoving() )
		m_bInCoverSpot = false;		// left his spot

	// Near misses: bullet impacts from someone he's hostile to, close to him.
	if ( HasCondition( COND_HEAR_BULLET_IMPACT ) )
	{
		CSound *pSound = GetBestSound( SOUND_BULLET_IMPACT );
		CBaseEntity *pOwner = pSound ? pSound->m_hOwner.Get() : NULL;
		if ( pSound && pOwner && pOwner != this && IRelationType( pOwner ) == D_HT &&
			 ( pSound->GetSoundOrigin() - WorldSpaceCenter() ).Length() < 96.0f )
			NoteUnderFire();
	}

	// Flanked: the enemy can now see where he hides. Get out (SelectSchedule picks new cover).
	if ( m_bInCoverSpot && GetEnemy() && !IsCurSchedule( SCHED_BIG_FLINCH, false ) && m_actFullBodyPain == ACT_INVALID &&
		 LowCoverAgainst( GetEnemy() ) == COVER_EXPOSED )
	{
		if ( npc_german_debug.GetBool() )
			Msg( "npc_german_soldier %d: cover - flanked, moving\n", entindex() );
		m_bInCoverSpot = false;
		m_bHasCoverSpot = false;
		m_flNextCoverSearch = 0.0f;
		m_flWantHideTime = 0.0f;
		SetCondition( COND_GERMAN_FLANKED );
		ClearSchedule( "German soldier flanked" );
	}
}

// npc_german_debug 2: what he's doing and why, floating over his head; cover traces drawn
// in the world (see LowCoverAgainst).
void CNPC_GermanSoldier::DrawDebugOverlay( void )
{
	int line = 0;
	char sz[256];
	const CAI_Schedule *pSched = GetCurSchedule();
	const Task_t *pTask = GetTask();
	Q_snprintf( sz, sizeof( sz ), "%s / %s", pSched ? pSched->GetName() : "-", pTask ? TaskName( pTask->iTask ) : "-" );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	Q_snprintf( sz, sizeof( sz ), "anim %s%s", ActivityList_NameForIndex( GetTranslatedActivity() ) ? ActivityList_NameForIndex( GetTranslatedActivity() ) : "?",
				IsCrouching() ? " (crouched)" : "" );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	if ( GetEnemy() )
	{
		static const char *s_Cover[] = { "EXPOSED", "full cover", "low wall (crouch fire)", "chest-high (stand fire)" };
		m_flCoverCheckTime = 0.0f;		// fresh check so the traces get drawn
		Q_snprintf( sz, sizeof( sz ), "cover: %s%s%s%s", s_Cover[ LowCoverAgainst( GetEnemy() ) + 1 ],
					gpGlobals->curtime < m_flUnderFireUntil ? "  UNDER FIRE" : "", IsWounded() ? "  WOUNDED" : "",
					m_iMySquadSlot != SQUAD_SLOT_NONE ? "  [firing slot]" : "" );
		NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	}
	Q_snprintf( sz, sizeof( sz ), "health %d/%d", GetHealth(), GetMaxHealth() );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
}

// A transition animation has finished: pick the real animation again (the translation
// sees we're no longer in a hide/aim pose and returns the target directly).
void CNPC_GermanSoldier::UpdateCoverTransition( void )
{
	if ( IsCoverTransition( GetTranslatedActivity() ) && IsSequenceFinished() )
		ResetActivity();
}

Activity CNPC_GermanSoldier::NPC_TranslateActivity( Activity eNewActivity )
{
	// Waiting in low cover: hide instead of crouching in the open.
	if ( IsHidingInCover() && ( eNewActivity == ACT_IDLE || eNewActivity == ACT_COVER_LOW || eNewActivity == ACT_COVER ||
								eNewActivity == ACT_COVER_MED || eNewActivity == ACT_RANGE_AIM_LOW || eNewActivity == ACT_CROUCHIDLE ) )
		eNewActivity = m_actHide;

	// Firing from low cover: CoD2's over-the-wall firing poses instead of DoD's open-ground aim.
	if ( m_actCoverLowFire != ACT_INVALID && GetEnemy() && !IsMoving() && GetState() == NPC_STATE_COMBAT &&
		 LowCoverAgainst( GetEnemy() ) > COVER_FULL )
	{
		if ( IsCrouching() && ( eNewActivity == ACT_RANGE_ATTACK1_LOW || eNewActivity == ACT_RANGE_AIM_LOW ) )
			eNewActivity = m_actCoverLowFire;
		else if ( !IsCrouching() && m_actCoverMidFire != ACT_INVALID &&
				  ( eNewActivity == ACT_RANGE_ATTACK1 || eNewActivity == ACT_IDLE_ANGRY || eNewActivity == ACT_IDLE_AIM_AGITATED ) )
			eNewActivity = m_actCoverMidFire;
	}

	Activity eResult = TranslateToDoD( eNewActivity );

	// Moving between hiding and a static pose: the CoD1 transition first.
	static const struct { CoverFamily_t from, to; int which; } s_Rules[] = {
		{ POSE_HIDE, POSE_CROUCH, 0 }, { POSE_CROUCH, POSE_HIDE, 1 }, { POSE_STAND, POSE_HIDE, 2 }, { POSE_HIDE, POSE_STAND, 3 } };
	CoverFamily_t from = PoseFamily( GetTranslatedActivity() ), to = PoseFamily( eResult );
	if ( from != to )
	{
		for ( int k = 0; k < (int)ARRAYSIZE( s_Rules ); k++ )
		{
			if ( s_Rules[k].from != from || s_Rules[k].to != to )
				continue;
			Activity t = s_Rules[k].which == 0 ? m_actHide2Aim : s_Rules[k].which == 1 ? m_actAim2Hide :
						 s_Rules[k].which == 2 ? m_actStand2Hide : m_actHide2Stand;
			if ( t != ACT_INVALID )
			{
				if ( npc_german_debug.GetBool() )
					Msg( "npc_german_soldier %d: cover %s (on the way to %s)\n", entindex(), ActivityList_NameForIndex( t ),
						 ActivityList_NameForIndex( eResult ) );
				return t;
			}
		}
	}
	return eResult;
}

Activity CNPC_GermanSoldier::TranslateToDoD( Activity eNewActivity )
{
	// In a fight he stands at the ready, not relaxed (what the Combine soldier used to do).
	if ( eNewActivity == ACT_IDLE && GetState() == NPC_STATE_COMBAT )
		eNewActivity = ACT_IDLE_ANGRY;

	// A pain overlay owns the upper body: the move-and-shoot code gets no fire or reload
	// gesture to lay on top of it (it adds nothing when the activity is invalid).
	if ( ( eNewActivity == ACT_GESTURE_RANGE_ATTACK1 || eNewActivity == ACT_GESTURE_RELOAD ) && IsUpperBodyBusy() )
		return ACT_INVALID;

	// npc_german_test_crouch: every idle becomes the crouched aim (the base only crouches plain ACT_IDLE)
	if ( m_bTestCrouch && ( eNewActivity == ACT_IDLE || eNewActivity == ACT_IDLE_RELAXED || eNewActivity == ACT_IDLE_STIMULATED ||
							eNewActivity == ACT_IDLE_AGITATED || eNewActivity == ACT_IDLE_ANGRY || eNewActivity == ACT_IDLE_SMG1 ||
							eNewActivity == ACT_IDLE_AIM_RELAXED || eNewActivity == ACT_IDLE_AIM_STIMULATED || eNewActivity == ACT_IDLE_AIM_AGITATED ) )
		eNewActivity = ACT_RANGE_AIM_LOW;

	Activity eCombine = BaseClass::NPC_TranslateActivity( eNewActivity );

	EnsureAnimMap();

	unsigned short i = m_AnimMap.Find( eCombine );
	if ( i == m_AnimMap.InvalidIndex() )
		i = m_AnimMap.Find( eNewActivity );

	if ( i != m_AnimMap.InvalidIndex() )
	{
		Activity eDoD = (Activity)m_AnimMap[i];
		if ( npc_german_debug.GetInt() >= 3 && eCombine != m_actLastDebug )
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

	if ( IsUpperBodyBusy() )
		return false;		// every shot comes through here: none while a hit reaction or a cover move has the upper body

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

	Activity actNow = GetTranslatedActivity();
	if ( actNow != ACT_INVALID && ( actNow == m_actCoverLowFire || actNow == m_actCoverMidFire ) )
	{
		// The CoD2 firing pose has its own recoil: replay it for this shot.
		if ( m_bCoverFireSemi )
		{
			SetCycle( 0.0f );
			m_flCoverRecoilUntil = gpGlobals->curtime + SequenceDuration( GetSequence() );
		}
	}
	else if ( bPlayGesture && m_actFireGesture != ACT_INVALID )
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
	if ( !bStandingFire && !IsUpperBodyBusy() )
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
	UpdateCoverTransition();
	UpdateCoverAwareness();
	if ( npc_german_debug.GetInt() >= 2 )
		DrawDebugOverlay();
	if ( m_bCoverFireSemi && GetTranslatedActivity() != ACT_INVALID &&
		 ( GetTranslatedActivity() == m_actCoverLowFire || GetTranslatedActivity() == m_actCoverMidFire ) )
		SetPlaybackRate( gpGlobals->curtime < m_flCoverRecoilUntil ? 1.0f : 0.0f );	// aim steady between shots
	UpdateTestPainAll();
	if ( m_bTestCrouch && !IsCrouching() )
	{
		Crouch();
		ResetActivity();
	}
}

void CNPC_GermanSoldier::StartTask( const Task_t *pTask )
{
	switch ( pTask->iTask )
	{
	case TASK_RANGE_ATTACK1:
		{
			BaseClass::StartTask( pTask );	// base bookkeeping (attack time, activity)

			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			if ( pWeapon && !TaskIsComplete() )
			{
				m_nShotsLeft = MAX( 1, pWeapon->GetRandomBurst() );
				if ( IsWounded() )
					m_nShotsLeft = MAX( 1, (int)( m_nShotsLeft * m_flWoundedBurstScale + 0.5f ) );	// hurt: short bursts
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

			// Behind low cover: reload hidden (CoD2's low-wall reload keeps his head below ~32).
			if ( m_actCoverReload != ACT_INVALID && GetEnemy() && LowCoverAgainst( GetEnemy() ) > COVER_FULL && !IsHitReacting() )
			{
				Crouch();
				ResetIdealActivity( m_actCoverReload );
				int iSeq = SelectWeightedSequence( m_actCoverReload );
				m_flReloadDoneTime = gpGlobals->curtime + ( iSeq != ACTIVITY_NOT_AVAILABLE ? SequenceDuration( iSeq ) : 2.0f ) * 0.85f;
				m_flGestureReloadDone = 0.0f;
				m_bInCoverSpot = true;
				if ( npc_german_debug.GetBool() )
					Msg( "npc_german_soldier %d: cover - reloading hidden\n", entindex() );
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

	case TASK_GERMAN_PATH_TO_COVER:
		{
			AI_NavGoal_t goal( m_vecCoverSpot, ACT_RUN, 16.0f );
			if ( !m_bHasCoverSpot || !GetNavigator()->SetGoal( goal ) )
			{
				if ( m_bHasCoverSpot )
					MarkSpotBad( m_vecCoverSpot );
				m_bHasCoverSpot = false;
				m_flNextCoverSearch = gpGlobals->curtime + 1.0f;
				if ( npc_german_debug.GetBool() )
					Msg( "npc_german_soldier %d: no path to his cover spot - skipping it\n", entindex() );
				TaskFail( FAIL_NO_ROUTE );
				break;
			}
			Stand();
			m_flProgressCheck = gpGlobals->curtime + 1.5f;
			m_flProgressDist = ( m_vecCoverSpot - GetAbsOrigin() ).Length2D();
			TaskComplete();
		}
		break;

	case TASK_GERMAN_WATCH:
		{
			// Overwatch from his cover: up at the wall (crouched behind low cover, standing behind
			// chest-high), aimed at where he last saw you. Short, so the brain re-decides often.
			int nCover = GetEnemy() ? LowCoverAgainst( GetEnemy() ) : COVER_CROUCH;
			if ( nCover == COVER_STAND )
				Stand();
			else
				Crouch();
			SetIdealActivity( ACT_IDLE_ANGRY );
			m_flWatchUntil = gpGlobals->curtime + random->RandomFloat( 1.5f, 3.0f );
		}
		break;

	case TASK_GERMAN_HIDE:
		if ( IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && GetEnemy() && LowCoverAgainst( GetEnemy() ) <= COVER_FULL )
		{
			// Arrived, but it doesn't cover him where he actually stopped: don't come back here.
			if ( npc_german_debug.GetBool() )
				Msg( "npc_german_soldier %d: reached his spot but it doesn't cover him - skipping it\n", entindex() );
			MarkSpotBad( m_vecCoverSpot );
			m_bHasCoverSpot = false;
			m_flNextCoverSearch = 0.0f;
			TaskComplete();
			break;
		}
		if ( m_actHide == ACT_INVALID )
		{
			TaskComplete();
			break;
		}
		Crouch();
		SetIdealActivity( m_actHide );	// the translation plays the duck-down transition first
		{
			float flHide = random->RandomFloat( m_flCoverHideMin, m_flCoverHideMax );
			if ( IsWounded() )
				flHide *= m_flWoundedHideScale;		// hurt: keeps his head down longer
			m_flHideStarted = gpGlobals->curtime;
			m_flCoverHideUntil = gpGlobals->curtime + flHide;
			m_bInCoverSpot = true;
			if ( npc_german_debug.GetBool() )
				Msg( "npc_german_soldier %d: cover - hiding %.1f s (%s cover)%s%s\n", entindex(), flHide,
					 LowCoverAgainst( GetEnemy() ) == COVER_STAND ? "stand-fire" : "crouch-fire",
					 IsWounded() ? ", wounded" : "", gpGlobals->curtime < m_flUnderFireUntil ? ", under fire" : "" );
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
		BaseClass::StartTask( pTask );		// uses GetFlinchActivity -> the pending full-body pain
		m_actFullBodyPain = ACT_INVALID;
		if ( m_iForceFlinchSeq >= 0 )
		{
			// npc_german_test_pain asked for one exact sequence (an activity can have several)
			SetIdealActivity( ACT_DO_NOT_DISTURB );
			SetSequenceById( m_iForceFlinchSeq );
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
	case TASK_WAIT_FOR_MOVEMENT:
		// Stuck guard on the run to cover: every 1.5 s he must have closed 24 units, or the
		// spot goes on his refusal list and he re-decides (no humping walls).
		if ( IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && m_bHasCoverSpot && gpGlobals->curtime >= m_flProgressCheck )
		{
			float flDist = ( m_vecCoverSpot - GetAbsOrigin() ).Length2D();
			if ( flDist > m_flProgressDist - 24.0f && flDist > 32.0f )
			{
				if ( npc_german_debug.GetBool() )
					Msg( "npc_german_soldier %d: stuck on the way to cover (%.0f units left) - giving up on that spot\n", entindex(), flDist );
				MarkSpotBad( m_vecCoverSpot );
				m_bHasCoverSpot = false;
				GetNavigator()->StopMoving();
				TaskFail( FAIL_NO_ROUTE );
				break;
			}
			m_flProgressDist = flDist;
			m_flProgressCheck = gpGlobals->curtime + 1.5f;
		}
		BaseClass::RunTask( pTask );
		break;

	case TASK_GERMAN_WATCH:
		if ( GetEnemy() )
			GetMotor()->SetIdealYawToTargetAndUpdate( GetEnemyLKP(), AI_KEEP_YAW_SPEED );
		if ( gpGlobals->curtime >= m_flWatchUntil )
			TaskComplete();
		break;

	case TASK_GERMAN_HIDE:
		if ( GetEnemy() )
			GetMotor()->SetIdealYawToTargetAndUpdate( GetEnemyLKP(), AI_KEEP_YAW_SPEED );	// keep facing his cover
		if ( !GetEnemy() || LowCoverAgainst( GetEnemy() ) <= COVER_FULL )
			TaskComplete();	// the cover stopped covering (flanked) - SelectSchedule moves him
		else if ( gpGlobals->curtime >= m_flCoverHideUntil &&
				  ( gpGlobals->curtime >= m_flUnderFireUntil || gpGlobals->curtime - m_flHideStarted > 10.0f ) )
		{
			// time to pop up again (rounds still cracking past keep him down, 10 s at most -
			// then he comes up anyway, or suppression would pin him for good)
			m_flUnderFireUntil = 0.0f;
			TaskComplete();
		}
		break;

	case TASK_BIG_FLINCH:
		AutoMovement();		// full-body pains carry their stagger as extracted motion
		if ( m_iForceFlinchSeq >= 0 )
		{
			if ( IsSequenceFinished() )
			{
				m_iForceFlinchSeq = -1;
				SetIdealActivity( GetStoppedActivity() );
				TaskComplete();
			}
			break;
		}
		BaseClass::RunTask( pTask );
		break;

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
			else if ( IsUpperBodyBusy() )
				m_flNextShotTime = MAX( m_flNextShotTime, gpGlobals->curtime + 0.1f );	// still rising out of cover

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
					m_flLastBurstEnd = gpGlobals->curtime;	// cover rhythm: duck after a burst
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
	{
		DoHitReaction( info );
		NoteUnderFire();
	}
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
	Activity actPain = ACT_INVALID;		// upper-body overlay
	Activity actFull = ACT_INVALID;		// or a full-body pain
	bool bForce = m_bTestForcePain;
	m_bTestForcePain = false;
	bool bStill = !IsMoving();
	// Standing or crouched still, a hit is a big deal: he plays a full-body CoD pain and is
	// out of the fight for it (the cooldown keeps him from being stun-locked). On the move:
	// big hits (or the chance roll) give an upper-body pain over his running legs, else the
	// kick alone. npc_german_test_hit forces a reaction: no cooldown, no chance roll.
	if ( !bForce && gpGlobals->curtime < m_flNextFullFlinch )	pszSkip = "cooldown";
	else if ( IsCurSchedule( SCHED_BIG_FLINCH ) )				pszSkip = "already down";
	else if ( bStill )
	{
		if ( !bForce && random->RandomFloat( 0.0f, 1.0f ) > m_flStillPainChance )
			pszSkip = "still, chance roll failed";
		else if ( ( actFull = PickFullBodyPain( info ) ) == ACT_INVALID )
			pszSkip = "no full-body pain animation (are both WW2 packs included?)";
	}
	else if ( !bForce && info.GetDamage() < m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) > m_flFullFlinchChance )
		pszSkip = "small hit on the move, chance roll failed";
	else if ( LastHitGroup() == HITGROUP_HEAD && !m_bLastHitCrouched )
		pszSkip = "head hit on the move";	// the helmet hit is a full-body duck - no overlay version
	else if ( ( actPain = PickUpperPain() ) == ACT_INVALID )
		pszSkip = "no animation (is npc_ww2_anims_cod1 included?)";

	if ( npc_german_debug.GetBool() || bForce )
	{
		Msg( "npc_german_soldier %d hit: group %d, %.0f dmg, %s%s, side %.1f -> %s%s%s\n", entindex(), LastHitGroup(), info.GetDamage(),
			 m_bLastHitCrouched ? "CROUCHED" : "standing", IsMoving() ? " moving" : "", m_flLastHitSide,
			 pszSkip ? "kick only: " : "PAIN ",
			 pszSkip ? pszSkip : ActivityList_NameForIndex( actFull != ACT_INVALID ? actFull : actPain ),
			 ( !pszSkip && actFull != ACT_INVALID ) ? " (full body)" : "" );
	}
	if ( pszSkip )
		return;

	m_flNextFullFlinch = gpGlobals->curtime + m_flFullFlinchCooldown;

	if ( actFull != ACT_INVALID )
	{
		StartFullBodyPain( actFull );
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

// Full-body pains (fall to a hand, the helmet hit) run as SCHED_BIG_FLINCH, so the AI
// stops everything else and DoD's animations take over again with a cross-fade after.
void CNPC_GermanSoldier::StartFullBodyPain( Activity act )
{
	m_actFullBodyPain = act;
	m_flWantFullFlinch = gpGlobals->curtime;
	// Pains that end on one knee (fall to a hand, the leg hit, the crouch pain) leave him
	// crouched, so DoD's crouch takes over instead of him popping up; the AI stands him later.
	const char *pszAct = ActivityList_NameForIndex( act );
	if ( pszAct && ( Q_stristr( pszAct, "FALLTOHAND" ) || Q_stristr( pszAct, "LEG" ) || Q_stristr( pszAct, "CROUCH" ) ) )
		Crouch();
	SetCondition( COND_HEAVY_DAMAGE );
	ClearSchedule( "German soldier full-body pain" );	// SelectSchedule starts it next think
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

// Full-body pain for a soldier standing or crouched still, by where he was hit. Several
// sequences can share an activity (CoD2 and CoD1 shoulder pains) - the engine picks one.
Activity CNPC_GermanSoldier::PickFullBodyPain( const CTakeDamageInfo &info )
{
	Activity a = ACT_INVALID;
	if ( m_bLastHitCrouched )
		return ResolveNamedActivity( "ACT_WW2_CROUCH_PAIN" );

	const float flCentre = 3.0f;
	switch ( LastHitGroup() )
	{
	case HITGROUP_HEAD:		a = ResolveNamedActivity( "ACT_FLINCH_HEAD" ); break;
	case HITGROUP_LEFTARM:	a = ResolveNamedActivity( "ACT_FLINCH_LEFTARM" ); break;
	case HITGROUP_RIGHTARM:	a = ResolveNamedActivity( "ACT_FLINCH_RIGHTARM" ); break;
	case HITGROUP_LEFTLEG:	a = ResolveNamedActivity( "ACT_FLINCH_LEFTLEG" ); break;
	case HITGROUP_RIGHTLEG:	a = ResolveNamedActivity( "ACT_FLINCH_RIGHTLEG" ); break;
	case HITGROUP_CHEST:
	case HITGROUP_GENERIC:
	case HITGROUP_GEAR:
		if ( m_flLastHitSide > flCentre )		a = ResolveNamedActivity( "ACT_FLINCH_RIGHTARM" );
		else if ( m_flLastHitSide < -flCentre )	a = ResolveNamedActivity( "ACT_FLINCH_LEFTARM" );
		break;
	default:
		break;
	}
	if ( a == ACT_INVALID )
	{
		// gut and centre chest: the hunch, or for a big hit sometimes falling to a hand
		if ( info.GetDamage() >= m_flFullFlinchDamage && random->RandomFloat( 0.0f, 1.0f ) < 0.35f )
			a = ResolveNamedActivity( "ACT_WW2_PAIN_FALLTOHAND" );
		if ( a == ACT_INVALID )
			a = ResolveNamedActivity( "ACT_FLINCH_STOMACH" );
	}
	return a;
}

// Full-body flinch schedules are only used for "fall to a hand"; everything else is
// the upper-body overlay above. Returning nothing here also keeps the base AI from
// starting its own full-body flinch on heavy damage (one owner for pain reactions).
Activity CNPC_GermanSoldier::GetFlinchActivity( bool bHeavyDamage, bool bGesture )
{
	if ( bGesture )
		return BaseClass::GetFlinchActivity( bHeavyDamage, bGesture );
	return m_actFullBodyPain;
}

int CNPC_GermanSoldier::SelectSchedule( void )
{
	if ( m_flWantFullFlinch > 0.0f )
	{
		bool bFresh = gpGlobals->curtime - m_flWantFullFlinch < 0.5f;
		m_flWantFullFlinch = 0.0f;
		if ( bFresh && IsAlive() && m_actFullBodyPain != ACT_INVALID && HaveSequenceForActivity( m_actFullBodyPain ) )
			return SCHED_BIG_FLINCH;
		m_actFullBodyPain = ACT_INVALID;
	}
	m_iForceFlinchSeq = -1;		// a test pain that never started (or was cut short) is over

	if ( IsAlive() && GetState() == NPC_STATE_COMBAT && GetEnemy() )
	{
		m_flLastCombatTime = gpGlobals->curtime;
		return SelectCombatSchedule();
	}
	// Lost track of the enemy after a fight: hold the cover he's in for a while instead of
	// wandering off it (the engine's alert schedules walk to sounds and patrol).
	if ( IsAlive() && GetState() == NPC_STATE_ALERT && IsAtCoverSpot() &&
		 gpGlobals->curtime - m_flLastCombatTime < 4.0f * m_flCoverHoldTime && !HasCondition( COND_HEAR_DANGER ) )
		return SCHED_GERMAN_HOLD_COVER;
	return BaseClass::SelectSchedule();		// idle / alert: the engine's own (patrol, investigate, face)
}

//-----------------------------------------------------------------------------
// Decision log: at npc_german_debug 1+ every change of mind prints once, with its reason.
//-----------------------------------------------------------------------------
int CNPC_GermanSoldier::Decide( int iSched, const char *pszWhy )
{
	if ( npc_german_debug.GetBool() && ( iSched != m_iLastDecision || pszWhy != m_pszLastWhy ) )
		Msg( "npc_german_soldier %d: decide %s - %s\n", entindex(), GetSchedulingSymbols()->ScheduleIdToSymbol( GetGlobalScheduleId( iSched ) ), pszWhy );
	m_iLastDecision = iSched;
	m_pszLastWhy = pszWhy;
	return iSched;
}

// A failed schedule (no route, stuck) never falls back to the engine's stand-around SCHED_FAIL
// in a fight: he gets low and keeps facing you while the brain re-decides next think.
int CNPC_GermanSoldier::SelectFailSchedule( int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode )
{
	if ( IsAlive() && GetState() == NPC_STATE_COMBAT && GetEnemy() )
	{
		Crouch();
		return Decide( SCHED_COMBAT_FACE, "last plan failed - crouch and face" );
	}
	return BaseClass::SelectFailSchedule( failedSchedule, failedTask, taskFailCode );
}

//-----------------------------------------------------------------------------
// The combat brain. A prioritized list - the first rule that applies wins - so it is
// readable and doesn't dither. Every movement is ours: run to scored, route-checked low
// cover (never the engine's take-cover / establish-line-of-fire, which don't understand low
// walls and send him into them). With nowhere better to go he holds where he is.
//-----------------------------------------------------------------------------
int CNPC_GermanSoldier::SelectCombatSchedule( void )
{
	bool bJustFired = m_flLastBurstEnd > 0.0f && gpGlobals->curtime - m_flLastBurstEnd < 1.5f;
	bool bWantHide = m_flWantHideTime > 0.0f && gpGlobals->curtime - m_flWantHideTime < 3.0f;
	bool bFlanked = HasCondition( COND_GERMAN_FLANKED );
	bool bUnderFire = gpGlobals->curtime < m_flUnderFireUntil;
	m_flLastBurstEnd = 0.0f;
	m_flWantHideTime = 0.0f;
	ClearCondition( COND_GERMAN_FLANKED );
	VacateStrategySlot();		// every decision re-claims its squad slot

	int nCover = LowCoverAgainst( GetEnemy() );
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	bool bEmpty = HasCondition( COND_NO_PRIMARY_AMMO );
	bool bLow = pWeapon && pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() < pWeapon->GetMaxClip1() && HasCondition( COND_LOW_PRIMARY_AMMO );
	bool bInCover = nCover > COVER_FULL;
	bool bCanHide = bInCover && m_actHide != ACT_INVALID;
	bool bCanSearch = gpGlobals->curtime >= m_flNextCoverSearch;
	if ( bInCover )
	{
		m_vecCoverSpot = GetAbsOrigin();	// he's in cover here: keep squadmates off his spot
		m_bHasCoverSpot = true;
	}

	// 1. A grenade or anything dangerous: get away from it.
	if ( HasCondition( COND_HEAR_DANGER ) )
		return Decide( SCHED_TAKE_COVER_FROM_BEST_SOUND, "danger heard" );
	// 2. His cover stopped covering (you flanked him): new low cover.
	if ( bFlanked )
	{
		m_flNextCoverSearch = gpGlobals->curtime + 3.0f;
		if ( FindCoverSpot( GetEnemy() ) )
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, "flanked - moving to new cover" );
	}
	// 3. Empty: reload hidden in cover; otherwise get to cover first (reloads on the run);
	//    nowhere to go - crouch and reload right here.
	if ( bEmpty || ( bLow && bCanHide ) )
	{
		if ( bCanHide )
			return Decide( SCHED_RELOAD, "reload, hidden in cover" );
		if ( bCanSearch )
		{
			m_flNextCoverSearch = gpGlobals->curtime + 3.0f;
			if ( FindCoverSpot( GetEnemy() ) )
				return Decide( SCHED_GERMAN_MOVE_TO_COVER, "empty, no cover here - run to cover" );
		}
		Crouch();
		return Decide( SCHED_RELOAD, "empty, nowhere to go - crouch and reload" );
	}
	// 4. Not in low cover he can fight from: run to the best reachable spot (firing on the move).
	if ( !bInCover && bCanSearch )
	{
		m_flNextCoverSearch = gpGlobals->curtime + 3.0f;
		if ( FindCoverSpot( GetEnemy() ) )
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, "in the open - run to cover" );
	}
	// 5. Just fired, under fire, or asked to duck - and he's in low cover: hide.
	if ( bCanHide && ( bJustFired || bWantHide || bUnderFire ) )
		return Decide( SCHED_GERMAN_COVER_HIDE, bUnderFire ? "under fire - duck" : "burst done - duck" );
	// 6. He can shoot: if the squad has a free firing slot, shoot (crouched or standing to
	//    suit his cover; crouched at range in the open); otherwise wait hidden / watching.
	if ( HasCondition( COND_CAN_RANGE_ATTACK1 ) )
	{
		if ( !m_pSquad || OccupyStrategySlotRange( SQUAD_SLOT_ATTACK1, SQUAD_SLOT_ATTACK2 ) )
		{
			if ( nCover == COVER_CROUCH )
				Crouch();
			else if ( nCover == COVER_STAND )
				Stand();
			else if ( GetEnemy() && ( GetEnemy()->GetAbsOrigin() - GetAbsOrigin() ).Length2D() > 600.0f )
				Crouch();
			else
				Stand();
			return Decide( SCHED_RANGE_ATTACK1, bInCover ? "fire from cover" : "fire (no cover)" );
		}
		if ( bCanHide )
			return Decide( SCHED_GERMAN_COVER_HIDE, "squad firing slots full - stay down" );
		return Decide( SCHED_COMBAT_FACE, "squad firing slots full - watch" );
	}
	// 7. Can't see him (or can't shoot him): hold where he is and watch his last spot; once
	//    he's been gone a while, bound forward to closer cover (two at a time - the squad's
	//    attack slots - so the rest keep watching).
	float flUnseen = gpGlobals->curtime - GetEnemyLastTimeSeen();
	if ( flUnseen > m_flCoverHoldTime && gpGlobals->curtime >= m_flNextAdvance &&
		 ( !m_pSquad || OccupyStrategySlotRange( SQUAD_SLOT_ATTACK1, SQUAD_SLOT_ATTACK2 ) ) )
	{
		m_flNextAdvance = gpGlobals->curtime + 4.0f;
		if ( FindCoverSpot( GetEnemy(), true ) )
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, "lost sight a while - bound forward" );
		VacateStrategySlot();
	}
	if ( HasCondition( COND_SEE_ENEMY ) )
		return Decide( SCHED_COMBAT_FACE, "sees him, can't shoot - face" );
	return Decide( SCHED_GERMAN_HOLD_COVER, bInCover ? "can't see him - hold cover, watch" : "can't see him - hold, watch" );
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
	if ( !pszAct )
		return false;
	static const char *s_Full[] = { "ACT_WW2_PAIN_FALLTOHAND", "ACT_FLINCH_HEAD", "ACT_FLINCH_LEFTARM", "ACT_FLINCH_RIGHTARM",
									"ACT_FLINCH_STOMACH", "ACT_FLINCH_LEFTLEG", "ACT_FLINCH_RIGHTLEG", "ACT_WW2_CROUCH_PAIN" };
	for ( int i = 0; i < (int)ARRAYSIZE( s_Full ); i++ )
	{
		if ( !Q_stricmp( pszAct, s_Full[i] ) )
			return true;
	}
	return Q_stristr( pszAct, "_UPPER_" ) != NULL;
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

	if ( pszAct && !Q_stristr( pszAct, "_UPPER_" ) )
	{
		m_iForceFlinchSeq = iSequence;		// full-body ones go through their real schedule
		StartFullBodyPain( (Activity)GetSequenceActivity( iSequence ) );
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

bool CNPC_GermanSoldier::TestToggleCrouch( void )
{
	m_bTestCrouch = !m_bTestCrouch;
	if ( m_bTestCrouch )
		ForceCrouch();
	else
		ClearForceCrouch();
	ResetActivity();		// re-pick the animation now, not at the next activity change
	return m_bTestCrouch;
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
	Msg( "npc_german_soldier %d: %s\n", pNPC->entindex(), pNPC->TestToggleCrouch() ? "forced to crouch" : "stands" );
}


//-----------------------------------------------------------------------------
// Schedules
//-----------------------------------------------------------------------------
AI_BEGIN_CUSTOM_NPC( npc_german_soldier, CNPC_GermanSoldier )

	DECLARE_TASK( TASK_GERMAN_HIDE )
	DECLARE_TASK( TASK_GERMAN_PATH_TO_COVER )
	DECLARE_TASK( TASK_GERMAN_WATCH )
	DECLARE_CONDITION( COND_GERMAN_FLANKED )

	//=========================================================
	// SCHED_GERMAN_COVER_HIDE: duck behind low cover for a few seconds after a burst.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_COVER_HIDE,

		"	Tasks"
		"		TASK_STOP_MOVING		0"
		"		TASK_GERMAN_HIDE		0"
		""
		"	Interrupts"
		"		COND_NEW_ENEMY"
		"		COND_ENEMY_DEAD"
		"		COND_HEAR_DANGER"
		"		COND_HEAR_MOVE_AWAY"
	)

	//=========================================================
	// SCHED_GERMAN_MOVE_TO_COVER: run to the chosen low cover (firing on the move), then duck.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_MOVE_TO_COVER,

		"	Tasks"
		"		TASK_GERMAN_PATH_TO_COVER	0"
		"		TASK_WAIT_FOR_MOVEMENT		0"
		"		TASK_GERMAN_HIDE			0"
		""
		"	Interrupts"
		"		COND_ENEMY_DEAD"
		"		COND_HEAR_DANGER"
	)

	//=========================================================
	// SCHED_GERMAN_HOLD_COVER: stay at the cover, up and watching his last known spot.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_HOLD_COVER,

		"	Tasks"
		"		TASK_STOP_MOVING			0"
		"		TASK_GERMAN_WATCH			0"
		""
		"	Interrupts"
		"		COND_NEW_ENEMY"
		"		COND_ENEMY_DEAD"
		"		COND_CAN_RANGE_ATTACK1"
		"		COND_LIGHT_DAMAGE"
		"		COND_HEAVY_DAMAGE"
		"		COND_HEAR_DANGER"
		"		COND_GERMAN_FLANKED"
	)

AI_END_CUSTOM_NPC()
