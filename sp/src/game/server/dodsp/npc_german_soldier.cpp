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
#include "german_chatter.h"

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

#define COVER_HIDE_TOP		32.0f	// top of the helmet in the CoD2 hide pose (+2)
#define HIDE_EYE			28.0f	// his eyes while hidden (he can't see over the wall)
#define COVER_CROUCH_EYE	44.0f	// muzzle height firing over a low wall crouched (CoD2 low-wall fire)
#define COVER_STAND_EYE		58.0f	// ...and standing (CoD2 / DoD standing fire)
#define OPEN_CROUCH_GUN		36.0f	// DoD crouched aim in the open
#define OPEN_CROUCH_EYE		40.0f
#define COVER_MASK			( CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_BLOCKLOS )	// what stops bullets / sight


// Squad slots: who may fire at once (squad_shooters of the FIRE slots) and who may bound
// forward at once (two). Separate, so a man moving up never blocks a man shooting.
enum GermanSquadSlot_T
{
	SQUAD_SLOT_GERMAN_FIRE1 = LAST_SHARED_SQUADSLOT,
	SQUAD_SLOT_GERMAN_FIRE2,
	SQUAD_SLOT_GERMAN_FIRE3,
	SQUAD_SLOT_GERMAN_FIRE4,
	SQUAD_SLOT_GERMAN_FIRE5,
	SQUAD_SLOT_GERMAN_BOUND1,
	SQUAD_SLOT_GERMAN_BOUND2,
};

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
		SCHED_GERMAN_COVER = BaseClass::NEXT_SCHEDULE,		// at low cover: the cover loop owns him (hide / rise / shoot / look / reload)
		SCHED_GERMAN_MOVE_TO_COVER,							// run to the best-scoring low cover nearby, then the cover loop
		SCHED_GERMAN_HOLD_COVER,							// no enemy to fight from cover (alert) or a failed plan: watch his last spot
		SCHED_GERMAN_OPEN,									// no cover he can reach: the open-ground loop owns him
		NEXT_SCHEDULE
	};
	enum
	{
		TASK_GERMAN_COVER_LOOP = BaseClass::NEXT_TASK,
		TASK_GERMAN_PATH_TO_COVER,
		TASK_GERMAN_WATCH,
		TASK_GERMAN_OPEN_LOOP,
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
	void		OnRestore( void );
	void		PainSound( const CTakeDamageInfo &info ) { m_Chatter.PlayPain(); }
	void		DeathSound( const CTakeDamageInfo &info ) { m_Chatter.PlayDeath( LastHitGroup() == HITGROUP_HEAD ); }

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
	Vector		GetShootEnemyDir( const Vector &shootOrigin, bool bNoisy = true );
	Vector		GetCrouchEyeOffset( void );
	Vector		GetCrouchGunOffset( void );
	int			SelectCombatSchedule( void );
	int			SelectScheduleInner( void );
	int			Decide( int iSched, const char *pszWhy );	// every combat choice goes through here (decision log)
	int			SelectFailSchedule( int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode );
	void		MarkSpotBad( const Vector &vecPos );		// unreachable / got stuck: skip it for a while
	bool		IsSpotBad( const Vector &vecPos ) const;
	void		DrawDebugOverlay( void );
	void		NPCThink( void );		// keeps the debug overlay up while the AI is paused (ai_disable)

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
	bool		IsUpperBodyBusy( void ) { return IsHitReacting() || IsInStanceTransition(); }
	// Low cover (stage 3b-1)
	enum CoverFamily_t { POSE_OTHER, POSE_HIDE, POSE_CROUCH, POSE_STAND };
	CoverFamily_t PoseFamily( Activity act ) const;
	bool		IsCoverTransition( Activity act ) const;
	bool		IsHidingInCover( void ) const { return m_eStance == STANCE_HIDE; }

	//-------------------------------------------------------------------------
	// Stance (R2). ONE owner: SetStance is the only thing that crouches or stands him.
	// Everything else asks it. The animation translation and the eye/gun heights are
	// pure functions of the stance - never of what he senses this think.
	//-------------------------------------------------------------------------
	enum Stance_t { STANCE_STAND, STANCE_CROUCH, STANCE_HIDE, STANCE_LOW_UP, STANCE_MID_UP, STANCE_CORNER_HIDE, STANCE_CORNER_AIM, NUM_STANCES };
	bool		StanceIsCrouched( Stance_t s ) const
	{
		if ( s == STANCE_CORNER_HIDE || s == STANCE_CORNER_AIM )
			return m_Cover.bCorner && m_Cover.corner.iStance == 1;	// a crouched corner set
		return s == STANCE_CROUCH || s == STANCE_HIDE || s == STANCE_LOW_UP;
	}
	static bool	IsCornerStance( Stance_t s ) { return s == STANCE_CORNER_HIDE || s == STANCE_CORNER_AIM; }
	bool		SetStance( Stance_t s, const char *pszWhy, bool bForce = false );
	// The move between stances (duck down / rise up) is owned by the stance owner too: a
	// timed state, not something the animation code works out from what happens to be playing.
	bool		IsInStanceTransition( void ) const { return m_actStanceTransition != ACT_INVALID; }
	void		UpdatePlaybackRate( void );		// the one owner of the animation's playback rate
	void		SyncStance( void );
	static bool	IsCoverStance( Stance_t s ) { return s == STANCE_HIDE || s == STANCE_LOW_UP || s == STANCE_MID_UP || IsCornerStance( s ); }
	bool		CoverStanceAllowed( int iSchedule );	// the cover stances belong to the cover loop
	void		OnStartSchedule( int scheduleType );

	//-------------------------------------------------------------------------
	// The cover he occupies (R2): measured once when he takes it - type, and the exposed
	// point he fires from. "Can I see / shoot him" at cover is asked from that point,
	// with memory (CoD4): if he couldn't see you from there and you haven't moved, no.
	//-------------------------------------------------------------------------
	//-------------------------------------------------------------------------
	// Corner cover (R6). The CoD1 corner sets: hide beside a corner, step out to fire, step
	// back. Where his head goes and which way he aims in each set is MEASURED from his own
	// skeleton (CornerCalibrate), never assumed - the search and the loop both use those
	// numbers (one owner for corner geometry).
	//-------------------------------------------------------------------------
	enum CornerAct_t { CN_ALERT, CN_OUT, CN_IN, CN_ENTER, CN_SEMI, CN_AUTO, CN_RELOAD, CN_DEATH, CN_NUM };
	struct CornerSet_t
	{
		bool		bValid;
		Activity	act[CN_NUM];
		Vector		vecHideEye;		// his eyes in the hide pose, in his own frame
		Vector		vecAimEye;		// ...stepped out aiming (corner_yaw 0)
		float		flAimYaw;		// where that aim points, relative to his facing (degrees)
		float		flOffLo, flOffHi;	// the aim at corner_yaw -1 / +1, relative to flAimYaw (never wraps)
	};
	struct CornerFit_t
	{
		bool		bValid;
		int			iWeapon, iStance, iSide;	// set: [bolt/auto][stand/crouch][right/left]
		float		flYaw;			// his facing at the corner
		Vector		vecHideEye;		// world
		Vector		vecAimEye;		// world
	};
	void		CornerCalibrate( void );
	bool		MeasurePose( Activity act, float flCornerYaw, bool bNeutralAim, Vector &vecEyeLocal, matrix3x4_t &matRHand, float &flHandsYaw, float *pflMuzzleYaw = NULL );
	float		GunYaw( const matrix3x4_t &matRHand ) const;	// where the gun in this right hand points, relative to his facing
	float		CornerAimOffset( const CornerSet_t &set, float flFacing, const Vector &vecFrom, const Vector &vecTarget ) const;
	bool		CornerFitAt( const Vector &vecPos, CBaseEntity *pEnemy, CornerFit_t &fit, const Vector *pvecEye = NULL );
	const CornerSet_t *CornerSet( void ) const;		// the set of the corner he occupies
	Activity	CornerAct( CornerAct_t e ) const { const CornerSet_t *p = CornerSet(); return p ? p->act[e] : ACT_INVALID; }
	Activity	CornerFireAct( void ) const;
	void		UpdateCornerAim( void );

	struct CoverSpot_t
	{
		bool	bValid;
		Vector	vecPos;
		bool	bCorner;			// corner cover (R6): corner holds the set and facing
		CornerFit_t corner;
		int		nType;				// COVER_CROUCH, COVER_STAND or COVER_CORNER
		Vector	vecExposedEye;		// where his eyes/gun are when up firing over it
		bool	bCouldntSee;		// last look from the exposed point failed...
		Vector	vecCouldntSeeAt;	// ...while the enemy's eyes were here
		float	flExposedSince;		// the hide spot has been visible to the enemy since (0 = it isn't)
	};
	bool		OccupyCover( void );
	bool		CanSeeEnemyFromExposed( void );
	bool		IsFlankedAtCover( void );		// hide spot exposed for 0.5 s straight

	//-------------------------------------------------------------------------
	// The cover loop (R3, after CoD4's cover_behavior): one task owns him at his cover.
	// Every state takes time; he leaves only when flanked, bored, or interrupted.
	//-------------------------------------------------------------------------
	enum CoverState_t { CS_HIDE, CS_RISE, CS_SHOOT, CS_LOOK, CS_DUCK, CS_RELOAD };
	enum CoverExit_t { CEXIT_NONE, CEXIT_FLANKED, CEXIT_BORED, CEXIT_FOUND_COVER, CEXIT_ORDER };

	//-------------------------------------------------------------------------
	// The open-ground loop (R4): no cover he can reach. Stance chosen once (crouched far off or
	// under fire, else standing; only ever goes lower), bursts with rests, reload in place, a
	// look for cover every 3 s, bored when he hasn't seen you for cover_hold_time.
	//-------------------------------------------------------------------------
	enum OpenState_t { OS_AIM, OS_FIRE, OS_RELOAD };
	void		SetOpenState( OpenState_t eState, const char *pszWhy );
	void		RunOpenLoop( void );
	void		OpenPose( void );
	bool		TryFireShot( void );			// one owner for pulling the trigger (both loops)
	bool		ClaimFiringSlot( void );
	bool		ClaimBoundSlot( void );
	float		RouteExposure( const Vector &vecStart, AI_Waypoint_t *pRoute, CBaseEntity *pEnemy, const Vector *pvecEye = NULL );

	//-------------------------------------------------------------------------
	// Battle chatter (A3a): the component owns who talks when; these are the moments.
	//-------------------------------------------------------------------------
public:
	CGermanChatter		m_Chatter;
private:
	CNPC_GermanSoldier *NearestSquadmate( float flMaxDist, bool bMustSeeMe = false );

	//-------------------------------------------------------------------------
	// Squad coordinator (A3b). One owner per squad for who does what: the suppressor, the
	// bounder, the flanker, falling back. It picks WHO; the man picks WHERE (his own cover
	// search) and reports back if he can't.
	//-------------------------------------------------------------------------
public:
	enum SquadOrder_t { ORDER_NONE, ORDER_BOUND, ORDER_FLANK_LEFT, ORDER_FLANK_RIGHT, ORDER_FALLBACK, ORDER_ASSAULT, ORDER_OVERWATCH, NUM_ORDERS };
	enum MoveKind_t { MOVE_COVER, MOVE_BOUND, MOVE_FLANK_LEFT, MOVE_FLANK_RIGHT, MOVE_FALLBACK, MOVE_ASSAULT, MOVE_OVERWATCH };
	void		SquadTick( void );
	void		GiveSquadOrder( SquadOrder_t eOrder, float flDelay, const Vector &vecEnemyToSquad );
	bool		CanTakeSquadOrder( void );
	bool		CanSuppressEnemy( const Vector &vecTarget );
	bool		IsInCoverLoop( void ) { return IsCurSchedule( SCHED_GERMAN_COVER, false ); }
	bool		IsInOpenLoop( void ) { return IsCurSchedule( SCHED_GERMAN_OPEN, false ); }
	bool		IsSquadSuppressor( void );
	MoveKind_t	m_eMoveKind;			// what the current run to cover is
	SquadOrder_t m_eSquadOrder;		// pending order from the coordinator
	float		m_flSquadOrderAt;		// not before
	float		m_flSquadOrderIssued;
	float		m_flOrderRefusedUntil;	// refusal memory: he couldn't carry one out; ask someone else
	Vector		m_vecFlankFrom;			// unit 2D: from the enemy toward the squad (the squad's front line)
	float		m_flLastSearchRun;		// route length of the last spot found
	bool		m_bSuppressing;			// cover loop: up firing at the enemy's position for the squad
	float		m_flSquadTempo;			// script: how often the squad bounds / flanks
	float		m_flFlankRadius;		// script
	float		m_flSquadAggression;	// script: 0.5 cautious defenders .. 2 assault troops (A3c)
	float		m_flAssaultAfter;		// script: seconds the squad has lost you before it comes after you
	int			SquadCrowding( const Vector &vecPos );	// squadmates already at / heading for a spot this close
private:
	void		SquadOrderFailed( SquadOrder_t eOrder );
	void		ChatterOrderCoverMove( void );		// a squadmate calls out his run to cover
	void		ChatterReloading( void );
	void		ChatterThreat( void );
	void		SetCoverState( CoverState_t eState, const char *pszWhy );
	void		CoverPose( Activity act ) { if ( GetIdealActivity() != act ) SetIdealActivity( act ); }	// never restarts the pose he's in
	void		RunCoverLoop( void );
	void		CoverDecideWhileHidden( void );

	//-------------------------------------------------------------------------
	// Watchdog (R1): passive. Prints INVARIANT lines when a rule is broken; changes nothing.
	//-------------------------------------------------------------------------
	enum Invariant_t { INV_STANCE_FLIPS, INV_STANCE_OWNER, INV_SCHED_CHURN, INV_DECISION_CHURN, INV_COVER_CHURN, INV_POSE_FLIPS, INV_ANIM_STUCK, INV_COVER_STANCE, INV_ORDER_STUCK, INV_FLANK_TIMEOUT, INV_CORNER_YAW, INV_ASSAULT_TIMEOUT, NUM_INVARIANTS };
	void		WatchdogPose( void );		// what the body actually plays: down / crouched / standing flips
	bool		IsClipEmpty( void );		// one owner for "he has no rounds in the gun"
	void		Invariant( Invariant_t eWhich, const char *pszFormat, ... );
	void		WatchdogNoteSchedule( void );
	enum { COVER_EXPOSED = -1, COVER_FULL = 0, COVER_CROUCH = 1, COVER_STAND = 2, COVER_CORNER = 3 };
	int			LowCoverAgainst( CBaseEntity *pEnemy );	// COVER_*: > 0 means low cover he can fire from
	int			CoverAt( const Vector &vecPos, CBaseEntity *pEnemy, trace_t *pHideTrace = NULL, const Vector *pvecEye = NULL );	// same test at any spot (pvecEye: against where his eyes were)
	enum SearchMode_t { SEARCH_COVER, SEARCH_ADVANCE, SEARCH_FLANK_LEFT, SEARCH_FLANK_RIGHT, SEARCH_RETREAT, SEARCH_ASSAULT };
	// best-scoring cover for this kind of move; pvecThreatEye: against where the enemy was last
	// seen (his eyes there) instead of where he is - for a squad that has lost sight of him
	bool		FindCoverSpot( CBaseEntity *pEnemy, int nMode = SEARCH_COVER, const Vector *pvecThreatEye = NULL );
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
	Vector				m_vecCoverSearchEye;	// where the enemy's eyes were when that spot was judged
	bool				m_bHasCoverSpot;
	float				m_flNextCoverSearch;
	float				m_flNextAdvance;		// bounding forward to closer cover: not more often than this
	float				m_flCoverHoldTime;		// script: enemy out of sight this long -> advance to closer cover
	float				m_flLastCombatTime;		// last think in combat (alert-state holding)
	float				m_flWatchUntil;
	// Stance owner
	Stance_t			m_eStance;
	float				m_flStanceSince;
	const char		   *m_pszStanceWhy;
	Activity			m_actStanceTransition;	// playing the move between stances (ACT_INVALID = none)
	float				m_flStanceTransitionEnd;
	float				m_flSeqStarted;			// watchdog: when the playing sequence began
	float				m_flStanceFlipTimes[4];	// watchdog: last few stance changes
	int					m_iStanceFlip;
	// Cover spot + loop
	CoverSpot_t			m_Cover;
	CoverState_t		m_eCoverState;
	float				m_flCoverStateStart;
	float				m_flCoverStateUntil;	// the state's own time (min / max, per state)
	bool				m_bRiseToShoot;			// CS_RISE leads to CS_SHOOT (else CS_LOOK)
	float				m_flCoverNextLook;
	float				m_flCoverBoredAt;
	float				m_flCoverLastSaw;		// last time he saw the enemy from the exposed point
	float				m_flCoverLostSight;		// up and can't see: since when (grace before ducking)
	float				m_flCoverHiddenSince;	// continuous time hidden (suppression can't pin him forever)
	float				m_flLastSuppressed;		// last near miss / hit
	float				m_flCoverStateTimes[6];	// watchdog: last few cover state changes
	int					m_iCoverStateFlip;
	CoverExit_t			m_eCoverExit;
	float				m_flCoverExposeMax;		// script: longest he stays up firing (s)
	float				m_flCoverLookMin;		// script: look-over-the-wall cooldown (s)
	float				m_flCoverLookMax;
	OpenState_t			m_eOpenState;
	float				m_flOpenStateUntil;
	float				m_flOpenNextCoverCheck;
	float				m_flOpenLastSaw;
	int					m_nSquadShooters;		// script: squad members firing at once
	float				m_flBoundMax;			// script: longest bound forward (route length)
	char				m_szCoverLandmark[32];	// what his chosen cover spot is behind (for the callout)
	float				m_flChatterSawEnemy;	// last think he saw the enemy (threat callouts)
	// Watchdog
	float				m_flInvariantNext[NUM_INVARIANTS];
	float				m_flSchedStarted;
	int					m_nShortScheds;
	int					m_iLastShortSched;
	float				m_flDecisionTimes[5];
	struct PoseSample_t { float t; int nClass; int iSeq; Activity actIdeal; int iSched; int iTask; int nStance; int nCover; };
	PoseSample_t		m_PoseLog[6];			// last body-height changes (watchdog)
	int					m_iPoseLog;
	int					m_nPoseClass;
	int					m_iPoseSeq;
	int					m_iDecision;
	enum { NUM_BAD_SPOTS = 8 };
	Vector				m_vecBadSpot[NUM_BAD_SPOTS];	// refusal memory: spots he couldn't reach
	float				m_flBadSpotUntil[NUM_BAD_SPOTS];
	int					m_iNextBadSpot;
	float				m_flProgressCheck;		// stuck guard while running to cover
	float				m_flProgressDist;
	int					m_iLastDecision;
	const char		   *m_pszLastWhy;
	float				m_flCoverHideUntil;
	float				m_flLastBurstEnd;		// a burst in the open just finished (unused by the brain now)
	float				m_flCoverCheckTime;		// LowCoverAgainst cache
	int					m_nCoverCheckResult;
	EHANDLE				m_hCoverCheckEnemy;
	Vector				m_vecCoverCheckPos;		// ...valid only where it was measured
	Activity			m_actCoverReload;		// CoD2 reload done hidden behind low cover
	Activity			m_actCoverLowFire;		// CoD2 firing over a low wall (crouched) / a chest-high one (standing)
	Activity			m_actCoverMidFire;
	bool				m_bCoverFireSemi;		// single shots: the firing loop plays once per shot
	float				m_flCoverRecoilUntil;
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
	int					m_poseCornerYaw;		// CoD1 corner sets blend their aim on this

	// Corner cover (R6)
	CornerSet_t			m_CornerSets[2][2][2];	// [bolt/auto][stand/crouch][right/left], measured
	bool				m_bCornerCalDone;
	CornerFit_t			m_PendingCorner;		// the corner fit of the spot he's running to
	Vector				m_vecGunAxis;			// the gun's direction in his right hand's own frame (measured)
	float				m_flCornerYawBad;		// watchdog: facing off the corner since

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
	m_vecCoverSearchEye = vec3_origin;
	m_bHasCoverSpot = false;
	m_flNextCoverSearch = 0.0f;
	m_flNextAdvance = 0.0f;
	m_flCoverHoldTime = 8.0f;
	m_flLastCombatTime = -1000.0f;
	m_flWatchUntil = 0.0f;
	m_eStance = STANCE_STAND;
	m_flStanceSince = 0.0f;
	m_pszStanceWhy = "spawn";
	m_actStanceTransition = ACT_INVALID;
	m_flStanceTransitionEnd = 0.0f;
	m_flSeqStarted = 0.0f;
	for ( int i = 0; i < 4; i++ )
		m_flStanceFlipTimes[i] = -100.0f;
	m_iStanceFlip = 0;
	memset( &m_Cover, 0, sizeof( m_Cover ) );
	m_eCoverState = CS_HIDE;
	m_flCoverStateStart = m_flCoverStateUntil = 0.0f;
	m_bRiseToShoot = false;
	m_flCoverNextLook = m_flCoverBoredAt = m_flCoverLastSaw = 0.0f;
	m_flCoverLostSight = m_flCoverHiddenSince = 0.0f;
	m_flLastSuppressed = -100.0f;
	for ( int i = 0; i < 6; i++ )
		m_flCoverStateTimes[i] = -100.0f;
	m_iCoverStateFlip = 0;
	m_eCoverExit = CEXIT_NONE;
	m_flCoverExposeMax = 4.0f;
	m_flCoverLookMin = 4.0f;
	m_flCoverLookMax = 15.0f;
	m_eOpenState = OS_AIM;
	m_flOpenStateUntil = m_flOpenNextCoverCheck = m_flOpenLastSaw = 0.0f;
	m_nSquadShooters = 2;
	m_flBoundMax = 700.0f;
	m_szCoverLandmark[0] = 0;
	m_eMoveKind = MOVE_COVER;
	m_eSquadOrder = ORDER_NONE;
	m_flSquadOrderAt = m_flSquadOrderIssued = 0.0f;
	m_flOrderRefusedUntil = 0.0f;
	m_vecFlankFrom = Vector( 1, 0, 0 );
	m_flLastSearchRun = 0.0f;
	m_bSuppressing = false;
	m_flSquadTempo = 1.0f;
	m_flSquadAggression = 1.0f;
	m_flAssaultAfter = 6.0f;
	m_flFlankRadius = 1200.0f;
	m_flChatterSawEnemy = -100.0f;
	for ( int i = 0; i < NUM_INVARIANTS; i++ )
		m_flInvariantNext[i] = 0.0f;
	m_flSchedStarted = 0.0f;
	m_nShortScheds = 0;
	m_iLastShortSched = -1;
	for ( int i = 0; i < 5; i++ )
		m_flDecisionTimes[i] = -100.0f;
	m_iDecision = 0;
	memset( m_PoseLog, 0, sizeof( m_PoseLog ) );
	for ( int i = 0; i < 6; i++ )
		m_PoseLog[i].t = -100.0f;
	m_iPoseLog = 0;
	m_nPoseClass = -1;
	m_iPoseSeq = -1;
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
	m_vecCoverCheckPos = vec3_origin;
	m_actCoverReload = ACT_INVALID;
	m_actCoverLowFire = m_actCoverMidFire = ACT_INVALID;
	m_bCoverFireSemi = true;
	m_flCoverRecoilUntil = 0.0f;
	m_poseCornerYaw = -1;
	m_bCornerCalDone = false;
	memset( m_CornerSets, 0, sizeof( m_CornerSets ) );
	memset( &m_PendingCorner, 0, sizeof( m_PendingCorner ) );
	m_flCornerYawBad = 0.0f;
	m_vecGunAxis = Vector( 1, 0, 0 );
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
	if ( g_pGermanScript )
		CGermanChatter::SetTuning( g_pGermanScript->GetBool( "chatter", true ), g_pGermanScript->GetString( "chatter_enemy_nation", "us" ),
								   g_pGermanScript->GetFloat( "chatter_frequency", 1.0f ), g_pGermanScript->GetString( "cry_speakers", "n m o" ) );
	CGermanChatter::LevelPrecache();

	BaseClass::Precache();
}

// After a save is loaded Spawn doesn't run: the chatter needs its owner and voice again.
void CNPC_GermanSoldier::OnRestore( void )
{
	BaseClass::OnRestore();
	m_Chatter.Init( this, m_SquadName != NULL_STRING ? STRING( m_SquadName ) : NULL );
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

	m_HackedGunPos = Vector( 0, 0, COVER_STAND_EYE );
	m_MoveAndShootOverlay.SetInitialDelay( 0.75f );

	NPCInit();

	SetViewOffset( Vector( 0, 0, 64 ) );	// DoD German's eyes standing (crouched: GetCrouchEyeOffset)
	m_Chatter.Init( this, m_SquadName != NULL_STRING ? STRING( m_SquadName ) : NULL );	// voice: round-robin in his squad
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

//-----------------------------------------------------------------------------
// Heights. ONE owner for every "how high is his gun / eye / helmet" fact: the cover test, the
// engine's sight check (eye) and its firing-line check (gun) all read these, so a wall the
// cover test calls "fire over it" is a wall the engine lets him fire over. (They disagreed:
// the cover test fired crouched at 44, the engine's crouched gun sat at 36 - behind a 38 wall
// he saw you but was never allowed to shoot.)
//-----------------------------------------------------------------------------
// Eye and gun heights come from the stance alone (one owner): hidden his eyes are below
// the wall, up over low cover they are where the cover test fired from, crouched in the
// open DoD's crouched aim.
Vector CNPC_GermanSoldier::GetCrouchEyeOffset( void )
{
	float z = m_eStance == STANCE_HIDE ? HIDE_EYE : m_eStance == STANCE_LOW_UP ? COVER_CROUCH_EYE + 2.0f : OPEN_CROUCH_EYE;
	return Vector( 0, 0, z );
}

Vector CNPC_GermanSoldier::GetCrouchGunOffset( void )
{
	float z = m_eStance == STANCE_HIDE ? HIDE_EYE : m_eStance == STANCE_LOW_UP ? COVER_CROUCH_EYE : OPEN_CROUCH_GUN;
	return Vector( 0, 0, z );
}

// Firing over cover: the engine aims at a random point on the target's body (legs included),
// which from 4 units above a wall puts the round into his own wall. Aim at a part of the enemy
// this muzzle can actually see - head and upper chest first - the same kind of line the
// "can I see him from my firing point" check used. In the open: the engine's own aim.
Vector CNPC_GermanSoldier::GetShootEnemyDir( const Vector &shootOrigin, bool bNoisy )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( pEnemy && ( m_eStance == STANCE_LOW_UP || m_eStance == STANCE_MID_UP || m_eStance == STANCE_CORNER_AIM ) )
	{
		Vector vecFeet = GetEnemyLKP();
		float flEye = pEnemy->EyePosition().z - pEnemy->GetAbsOrigin().z;
		static const float s_flHeights[] = { 0.8f, 0.65f, 0.95f, 0.5f };	// share of his eye height
		CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
		for ( int i = 0; i < (int)ARRAYSIZE( s_flHeights ); i++ )
		{
			Vector vecTarget = vecFeet + Vector( 0, 0, flEye * s_flHeights[i] );
			trace_t tr;
			UTIL_TraceLine( shootOrigin, vecTarget, COVER_MASK, &filter, &tr );
			if ( tr.fraction >= 1.0f )
			{
				Vector vecDir = vecTarget - shootOrigin;
				VectorNormalize( vecDir );
				return vecDir;		// the weapon's spread is added on top by GetActualShootTrajectory
			}
		}
	}
	return BaseClass::GetShootEnemyDir( shootOrigin, bNoisy );
}

Vector CNPC_GermanSoldier::Weapon_ShootPosition( void )
{
	if ( m_eStance == STANCE_CORNER_AIM && m_Cover.bCorner && !IsInStanceTransition() )
		return m_Cover.vecExposedEye - Vector( 0, 0, 3 );	// stepped out: where the corner set (measured) puts his rifle
	if ( IsCrouching() || IsCrouchedActivity( GetActivity() ) )
		return GetAbsOrigin() + GetCrouchGunOffset();
	return GetAbsOrigin() + Vector( 0, 0, COVER_STAND_EYE );
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
	m_poseCornerYaw = LookupPoseParameter( "corner_yaw" );
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
	if ( !IsAlive() && m_iAnimMapVersion == g_iGermanScriptVersion )
		return;		// a dead man keeps the animations he died with (his rifle is already on the ground)

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
	m_bCornerCalDone = false;		// corner sets are measured again for the new animations
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
	m_flCoverExposeMax = clamp( pScript->GetFloat( "cover_expose_max", 4.0f ), 0.5f, 30.0f );
	m_flCoverLookMin = clamp( pScript->GetFloat( "cover_look_min", 4.0f ), 0.5f, 60.0f );
	m_flCoverLookMax = MAX( m_flCoverLookMin, clamp( pScript->GetFloat( "cover_look_max", 15.0f ), 0.5f, 60.0f ) );
	m_nSquadShooters = clamp( pScript->GetInt( "squad_shooters", 2 ), 1, 5 );
	m_flBoundMax = clamp( pScript->GetFloat( "bound_max", 700.0f ), 128.0f, 4096.0f );
	m_flSquadTempo = clamp( pScript->GetFloat( "squad_tempo", 1.0f ), 0.1f, 10.0f );
	m_flFlankRadius = clamp( pScript->GetFloat( "flank_radius", 1200.0f ), 256.0f, 4096.0f );
	m_flSquadAggression = clamp( pScript->GetFloat( "squad_aggression", 1.0f ), 0.25f, 4.0f );
	m_flAssaultAfter = clamp( pScript->GetFloat( "assault_after", 6.0f ), 1.0f, 60.0f );
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
	if ( act == ACT_INVALID )
		return false;
	if ( act == m_actHide2Aim || act == m_actAim2Hide || act == m_actStand2Hide || act == m_actHide2Stand )
		return true;
	const char *psz = ActivityList_NameForIndex( act );	// any corner step out / back / settle
	return psz && !Q_strnicmp( psz, "ACT_WW2_CN_", 11 ) && ( Q_stristr( psz, "_OUT" ) || Q_stristr( psz, "_IN" ) || Q_stristr( psz, "_ENTER" ) );
}


// Is this spot low cover against that enemy? Measured, so it works behind any wall, prop or
// sandbag, with or without hint nodes: the enemy can't see where his hidden helmet would be,
// the thing in the way is right in front of him, and he can see the enemy from a crouched
// (1) or standing (2) firing position.
int CNPC_GermanSoldier::LowCoverAgainst( CBaseEntity *pEnemy )
{
	if ( !pEnemy )
		return 0;
	if ( pEnemy == m_hCoverCheckEnemy.Get() && gpGlobals->curtime < m_flCoverCheckTime &&
		 ( GetAbsOrigin() - m_vecCoverCheckPos ).LengthSqr() < 8.0f * 8.0f )
		return m_nCoverCheckResult;
	m_hCoverCheckEnemy = pEnemy;
	m_vecCoverCheckPos = GetAbsOrigin();
	m_flCoverCheckTime = gpGlobals->curtime + 0.5f;
	m_nCoverCheckResult = COVER_EXPOSED;

	trace_t tr;
	if ( m_Cover.bValid && m_Cover.bCorner && ( GetAbsOrigin() - m_Cover.vecPos ).Length2D() < 24.0f )
	{
		// At his corner: hidden = his head in the hide pose is out of the enemy's sight.
		CTraceFilterSkipTwoEntities filterCorner( this, pEnemy, COLLISION_GROUP_NONE );
		UTIL_TraceLine( pEnemy->EyePosition(), m_Cover.corner.vecHideEye, COVER_MASK, &filterCorner, &tr );
		m_nCoverCheckResult = tr.fraction < 1.0f ? COVER_CORNER : COVER_EXPOSED;
		return m_nCoverCheckResult;
	}
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
								   tr.fraction < 1.0f ? "hide spot: only something far off hides it - open ground" : "hide spot: YOU CAN SEE IT", 0.15f );
	}
	if ( bDraw && m_bHasCoverSpot )
		NDebugOverlay::Box( m_vecCoverSpot, Vector( -8, -8, 0 ), Vector( 8, 8, 4 ), 0, 128, 255, 96, 0.15f );	// his chosen spot
	return m_nCoverCheckResult;
}

// The cover test at any spot (feet position): is it low cover against this enemy?
int CNPC_GermanSoldier::CoverAt( const Vector &vecPos, CBaseEntity *pEnemy, trace_t *pHideTrace, const Vector *pvecEye )
{
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	Vector vecEye = pvecEye ? *pvecEye : pEnemy->EyePosition();
	trace_t tr;
	Vector vecHide = vecPos + Vector( 0, 0, COVER_HIDE_TOP );
	UTIL_TraceLine( vecEye, vecHide, COVER_MASK, &filter, &tr );
	if ( pHideTrace )
		*pHideTrace = tr;
	if ( tr.fraction >= 1.0f )
		return COVER_EXPOSED;	// he'd be seen hiding here
	bool bAdjacent = ( vecHide - tr.endpos ).Length2D() <= m_flCoverMaxGap;	// the thing in the way is HIS cover
	trace_t trFire;
	UTIL_TraceLine( vecEye, vecPos + Vector( 0, 0, COVER_CROUCH_EYE ), COVER_MASK, &filter, &trFire );
	int nFire = trFire.fraction >= 1.0f ? COVER_CROUCH : COVER_FULL;
	if ( nFire == COVER_FULL )
	{
		UTIL_TraceLine( vecEye, vecPos + Vector( 0, 0, COVER_STAND_EYE ), COVER_MASK, &filter, &trFire );
		if ( trFire.fraction >= 1.0f )
			nFire = COVER_STAND;
	}
	if ( nFire == COVER_FULL )
		return COVER_FULL;		// out of your sight at every height: a corner, a house, a tall wall - can't shoot from it
	// He could shoot from here. That's low cover only if what hides him is right in front of
	// him; hidden just by something far off (a distant wall's top) is standing in the open.
	return bAdjacent ? nFire : COVER_EXPOSED;
}

//=============================================================================
// R2 - stance owner
//=============================================================================
#define STANCE_MIN_HOLD		0.5f	// a stance holds at least this long (commitment), unless forced

static const char *s_pszStance[] = { "stand", "crouch", "hide", "up over low wall", "up over chest-high", "behind the corner", "out at the corner" };
static const char *s_pszCoverState[] = { "hide", "rise", "shoot", "look", "duck", "reload" };

bool CNPC_GermanSoldier::SetStance( Stance_t s, const char *pszWhy, bool bForce )
{
	if ( s == m_eStance )
		return true;
	if ( !bForce && gpGlobals->curtime - m_flStanceSince < STANCE_MIN_HOLD )
		return false;	// asked too soon: the caller tries again next think

	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: stance %s -> %s (%s)\n", gpGlobals->curtime, entindex(), s_pszStance[m_eStance], s_pszStance[s], pszWhy );

	// Watchdog: 4 changes inside 2 s is bobbing, whoever asked.
	m_flStanceFlipTimes[m_iStanceFlip] = gpGlobals->curtime;
	m_iStanceFlip = ( m_iStanceFlip + 1 ) % 4;
	float flSpan = gpGlobals->curtime - m_flStanceFlipTimes[m_iStanceFlip];
	if ( flSpan < 2.0f )
		Invariant( INV_STANCE_FLIPS, "4 stance changes in %.1f s (now %s -> %s: %s; before that: %s)", flSpan,
				   s_pszStance[m_eStance], s_pszStance[s], pszWhy, m_pszStanceWhy ? m_pszStanceWhy : "?" );

	// The move between the poses (CoD1/2 transitions): into hiding from crouched or standing,
	// and up out of it. Decided here, once, from the old and new stance.
	Activity actTrans = ACT_INVALID;
	if ( IsCornerStance( s ) || IsCornerStance( m_eStance ) )
	{
		// corner: step out / step back / settle in beside it (CoD1 corner set)
		if ( s == STANCE_CORNER_AIM && m_eStance == STANCE_CORNER_HIDE )
			actTrans = CornerAct( CN_OUT );
		else if ( s == STANCE_CORNER_HIDE && m_eStance == STANCE_CORNER_AIM )
			actTrans = CornerAct( CN_IN );
		else if ( s == STANCE_CORNER_HIDE && !IsCornerStance( m_eStance ) )
			actTrans = CornerAct( CN_ENTER );
	}
	else
	{
		bool bFromHide = m_eStance == STANCE_HIDE, bToHide = s == STANCE_HIDE;
		if ( bToHide && !bFromHide )
			actTrans = StanceIsCrouched( m_eStance ) ? m_actAim2Hide : m_actStand2Hide;
		else if ( bFromHide && !bToHide )
			actTrans = StanceIsCrouched( s ) ? m_actHide2Aim : m_actHide2Stand;
	}
	m_actStanceTransition = ACT_INVALID;
	if ( actTrans != ACT_INVALID && !IsMoving() )
	{
		int iSeq = SelectWeightedSequence( actTrans );
		if ( iSeq != ACTIVITY_NOT_AVAILABLE )
		{
			m_actStanceTransition = actTrans;
			m_flStanceTransitionEnd = gpGlobals->curtime + SequenceDuration( iSeq );
		}
	}

	m_eStance = s;
	m_flStanceSince = gpGlobals->curtime;
	m_pszStanceWhy = pszWhy;
	if ( StanceIsCrouched( s ) )
		Crouch();
	else
		Stand();
	if ( !IsMoving() )
		ResetActivity();	// re-pick the animation: the translation reads the stance (and the transition)
	return true;
}

// The engine stands NPCs up when they start running (CAI_BaseNPC::OnChangeActivity) - that's
// the one outside change we accept: moving means standing. Anything else is a bug.
void CNPC_GermanSoldier::SyncStance( void )
{
	if ( IsCoverStance( m_eStance ) && IsAlive() )
	{
		const CAI_Schedule *pSched = GetCurSchedule();
		int iLocal = pSched ? GetClassScheduleIdSpace()->ScheduleGlobalToLocal( pSched->GetId() ) : -1;
		if ( pSched && !CoverStanceAllowed( iLocal ) )
			Invariant( INV_COVER_STANCE, "in cover stance '%s' during %s (not the cover loop)", s_pszStance[m_eStance], pSched->GetName() );
	}
	bool bCrouched = IsCrouching();
	if ( bCrouched == StanceIsCrouched( m_eStance ) )
		return;
	if ( !IsMoving() )
		Invariant( INV_STANCE_OWNER, "engine says %s but the stance owner says %s (%s) - something crouched/stood him directly",
				   bCrouched ? "crouched" : "standing", s_pszStance[m_eStance], m_pszStanceWhy ? m_pszStanceWhy : "?" );
	m_eStance = bCrouched ? STANCE_CROUCH : STANCE_STAND;
	m_flStanceSince = gpGlobals->curtime;
	m_pszStanceWhy = IsMoving() ? "moving (engine stands movers)" : "resynced";
}

// The cover stances (hidden, up over a wall) exist only where there is a wall: in the cover
// loop, on the run into it (its last task), watching over his own cover, and in a pain he takes
// there. Any other schedule starts by bringing him back to an ordinary crouch / stand -
// otherwise a flanked man stays huddled in the open with his gun below his own eyes.
bool CNPC_GermanSoldier::CoverStanceAllowed( int iSchedule )
{
	return iSchedule == SCHED_GERMAN_COVER || iSchedule == SCHED_GERMAN_MOVE_TO_COVER || iSchedule == SCHED_BIG_FLINCH ||
		   ( iSchedule == SCHED_GERMAN_HOLD_COVER && IsAtCoverSpot() && m_eStance != STANCE_HIDE );
}

void CNPC_GermanSoldier::OnStartSchedule( int scheduleType )
{
	BaseClass::OnStartSchedule( scheduleType );
	if ( IsCoverStance( m_eStance ) && !CoverStanceAllowed( scheduleType ) )
		SetStance( m_eStance == STANCE_MID_UP || ( IsCornerStance( m_eStance ) && !StanceIsCrouched( m_eStance ) ) ? STANCE_STAND : STANCE_CROUCH, "left the cover loop", true );
}

//=============================================================================
// R1 - watchdog
//=============================================================================
void CNPC_GermanSoldier::Invariant( Invariant_t eWhich, const char *pszFormat, ... )
{
	if ( gpGlobals->curtime < m_flInvariantNext[eWhich] )
		return;
	m_flInvariantNext[eWhich] = gpGlobals->curtime + 2.0f;
	static const char *s_pszNames[] = { "STANCE_FLIPS", "STANCE_OWNER", "SCHED_CHURN", "DECISION_CHURN", "COVER_CHURN", "POSE_FLIPS", "ANIM_STUCK", "COVER_STANCE", "ORDER_STUCK", "FLANK_TIMEOUT", "CORNER_YAW", "ASSAULT_TIMEOUT" };
	char sz[512];
	va_list ap;
	va_start( ap, pszFormat );
	Q_vsnprintf( sz, sizeof( sz ), pszFormat, ap );
	va_end( ap );
	Warning( "[%.2f] INVARIANT %s - npc_german_soldier %d: %s\n", gpGlobals->curtime, s_pszNames[eWhich], entindex(), sz );
}

// Called each time the AI picks a new schedule: one that ended within 0.1 s five times in a
// row is a zero-time loop (every action must take time).
void CNPC_GermanSoldier::WatchdogNoteSchedule( void )
{
	float flRan = gpGlobals->curtime - m_flSchedStarted;
	if ( m_iLastShortSched >= 0 && flRan < 0.1f )
	{
		if ( ++m_nShortScheds >= 5 )
		{
			Invariant( INV_SCHED_CHURN, "%s ended after %.0f ms, 5 times in a row",
					   GetSchedulingSymbols()->ScheduleIdToSymbol( GetGlobalScheduleId( m_iLastShortSched ) ), flRan * 1000.0f );
			m_nShortScheds = 0;
		}
	}
	else
		m_nShortScheds = 0;
	m_flSchedStarted = gpGlobals->curtime;
}

// What the body is actually doing, whatever any code thinks it asked for: classify the playing
// sequence as down (hidden / hidden reload), crouched, or standing. Transitions are skipped
// (they're the moves between). 4 class changes in 2 s is bobbing: print the last changes with
// everything that could have caused them (ideal activity, schedule, task, stance, cover state).
// npc_german_debug 3 prints every sequence change as it happens.
void CNPC_GermanSoldier::WatchdogPose( void )
{
	if ( !GetModelPtr() || GetSequence() < 0 || !IsAlive() )
		return;
	int iSeq = GetSequence();
	if ( iSeq == m_iPoseSeq )
		return;
	Activity actSeq = GetSequenceActivity( iSeq );
	const char *pszSeq = GetSequenceName( iSeq );
	if ( npc_german_debug.GetInt() >= 3 )
		Msg( "[%.2f] npc_german_soldier %d: sequence %s -> %s (ideal %s, sched %s, task %s, stance %s)\n", gpGlobals->curtime, entindex(),
			 m_iPoseSeq >= 0 ? GetSequenceName( m_iPoseSeq ) : "-", pszSeq, ActivityList_NameForIndex( GetIdealActivity() ) ? ActivityList_NameForIndex( GetIdealActivity() ) : "?",
			 GetCurSchedule() ? GetCurSchedule()->GetName() : "-", GetTask() ? TaskName( GetTask()->iTask ) : "-", s_pszStance[m_eStance] );
	m_iPoseSeq = iSeq;
	m_flSeqStarted = gpGlobals->curtime;
	if ( IsCoverTransition( actSeq ) )
		return;

	const char *pszAct = ActivityList_NameForIndex( actSeq );
	int nClass;
	if ( actSeq == m_actHide || actSeq == m_actCoverReload || ( pszAct && Q_stristr( pszAct, "HIDE" ) ) )
		nClass = 0;
	else if ( ( pszAct && ( Q_stristr( pszAct, "CROUCH" ) || Q_stristr( pszAct, "_LOW" ) || Q_stristr( pszAct, "PRONE" ) ) ) ||
			  ( pszSeq && Q_stristr( pszSeq, "crouch" ) ) )
		nClass = 1;
	else
		nClass = 2;
	if ( nClass == m_nPoseClass )
		return;
	m_nPoseClass = nClass;

	PoseSample_t &e = m_PoseLog[m_iPoseLog];
	e.t = gpGlobals->curtime;
	e.nClass = nClass;
	e.iSeq = iSeq;
	e.actIdeal = GetIdealActivity();
	e.iSched = GetCurSchedule() ? GetCurSchedule()->GetId() : -1;
	e.iTask = GetTask() ? GetTask()->iTask : -1;
	e.nStance = m_eStance;
	e.nCover = m_eCoverState;
	m_iPoseLog = ( m_iPoseLog + 1 ) % 6;

	// the 5th-newest entry: 4 changes since then
	const PoseSample_t &old = m_PoseLog[( m_iPoseLog + 1 ) % 6];
	if ( gpGlobals->curtime - old.t < 2.0f )
	{
		static const char *s_pszClass[] = { "DOWN", "CROUCHED", "STANDING" };
		char sz[1024] = "";
		for ( int k = 1; k <= 5; k++ )
		{
			const PoseSample_t &p = m_PoseLog[( m_iPoseLog + k ) % 6];
			char line[200];
			Q_snprintf( line, sizeof( line ), "\n      %.2f %-8s %s  [ideal %s | %s | %s | stance %s | cover %s]", p.t, s_pszClass[p.nClass],
						GetSequenceName( p.iSeq ), ActivityList_NameForIndex( p.actIdeal ) ? ActivityList_NameForIndex( p.actIdeal ) : "?",
						p.iSched >= 0 ? GetSchedulingSymbols()->ScheduleIdToSymbol( p.iSched ) : "-", p.iTask >= 0 ? TaskName( p.iTask ) : "-",
						s_pszStance[p.nStance], s_pszCoverState[p.nCover] );
			Q_strncat( sz, line, sizeof( sz ), COPY_ALL_CHARACTERS );
		}
		Invariant( INV_POSE_FLIPS, "body went down/up 4 times in %.2f s:%s", gpGlobals->curtime - old.t, sz );
	}
}

bool CNPC_GermanSoldier::IsClipEmpty( void )
{
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	if ( pWeapon && pWeapon->UsesClipsForAmmo1() )
		return pWeapon->Clip1() <= 0;		// the gun itself (the condition can be a think old)
	return HasCondition( COND_NO_PRIMARY_AMMO );
}

// How much of a route the enemy can see: sample every 48 units at chest height (a running
// man) and trace from the enemy's eyes. Returns the exposed length in units.
float CNPC_GermanSoldier::RouteExposure( const Vector &vecStart, AI_Waypoint_t *pRoute, CBaseEntity *pEnemy, const Vector *pvecEye )
{
	if ( !pEnemy )
		return 0.0f;
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	Vector vecEye = pvecEye ? *pvecEye : pEnemy->EyePosition();
	const float STEP = 48.0f;
	float flExposed = 0.0f, flCarry = 0.0f;
	int nTraces = 0;
	Vector vecA = vecStart;
	for ( AI_Waypoint_t *pWp = pRoute; pWp && nTraces < 40; pWp = pWp->GetNext() )
	{
		Vector vecB = pWp->GetPos();
		Vector vecSeg = vecB - vecA;
		float flSeg = vecSeg.Length();
		if ( flSeg > 0.1f )
		{
			vecSeg /= flSeg;
			for ( float d = STEP - flCarry; d <= flSeg && nTraces < 40; d += STEP )
			{
				trace_t tr;
				UTIL_TraceLine( vecEye, vecA + vecSeg * d + Vector( 0, 0, 48 ), COVER_MASK, &filter, &tr );
				nTraces++;
				if ( tr.fraction >= 1.0f )
					flExposed += STEP;
			}
			flCarry = fmodf( flCarry + flSeg, STEP );
		}
		vecA = vecB;
	}
	return flExposed;
}

bool CNPC_GermanSoldier::ClaimFiringSlot( void )
{
	return !m_pSquad || OccupyStrategySlotRange( SQUAD_SLOT_GERMAN_FIRE1, SQUAD_SLOT_GERMAN_FIRE1 + m_nSquadShooters - 1 );
}

bool CNPC_GermanSoldier::ClaimBoundSlot( void )
{
	return !m_pSquad || OccupyStrategySlotRange( SQUAD_SLOT_GERMAN_BOUND1, SQUAD_SLOT_GERMAN_BOUND2 );
}

// One owner for pulling the trigger, from cover or in the open: the gun's own cycle, the shot
// interval, and no shots while the upper body is busy (a hit reaction, ducking or rising).
bool CNPC_GermanSoldier::TryFireShot( void )
{
	float now = gpGlobals->curtime;
	if ( now < m_flNextShotTime || IsUpperBodyBusy() || !FireOneShot( true ) )
		return false;
	m_nShotsLeft--;
	BaseClass::OnRangeAttack1();	// shot regulator bookkeeping only (ours would fire again)
	SetLastAttackTime( now );
	m_flNextShotTime = now + m_flShotInterval;
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	if ( pWeapon )
		m_flNextShotTime = MAX( m_flNextShotTime, (float)pWeapon->m_flNextPrimaryAttack );
	return true;
}

//=============================================================================
// A3b - squad coordinator
//
// One owner per squad for who does what. Ticks twice a second (whichever member thinks first).
//   shared knowledge  where the enemy was last seen by anyone, and how long he's stayed put
//   fall back         lost half the squad and he's close: up to two men give ground ("Zurueck!")
//   flank             he's sat in one spot 8 s and there are 3+ of us: the man already furthest
//                     out to one side works round that side, out of his sight ("Links flankieren!"),
//                     while another keeps his head down ("Niederhalten!")
//   bound             every 8-14 s the rear-most man in cover moves up while another fires on
//                     the enemy's position ("Feuerschutz!" - "Vorwaerts!")
// Roles live only here; a man asks "am I the suppressor?" instead of keeping his own copy.
// He picks his own spot; if he can't find one he says so and is left alone for 15 s.
//=============================================================================
struct GermanSquadState_t
{
	float	flNextTick;
	EHANDLE	hEnemy;
	Vector	vecLKP;
	float	flLastSeen;
	Vector	vecStillPos;
	float	flStillSince;
	int		nMaxAlive;
	EHANDLE	hSuppressor;
	float	flSuppressUntil;
	EHANDLE	hBounder;
	EHANDLE	hFlanker;
	float	flFlankDeadline;
	float	flNextBound;
	float	flNextFlank;
	float	flNextFallback;
	float	flLastFlank;		// A3c: when the last flank went out (flanks come on tempo, not only when you sit still)
	EHANDLE	hAssault[2];		// A3c: up to two men coming after you where they lost you...
	EHANDLE	hOverwatch;			// ...and one watching that spot for them
	float	flAssaultDeadline;
	float	flNextAssault;
};
static CUtlDict<GermanSquadState_t, int>	s_GermanSquads;
static float								s_flGermanSquadsTime = 0.0f;

static GermanSquadState_t *GetGermanSquad( string_t iszSquad )
{
	if ( iszSquad == NULL_STRING )
		return NULL;
	float now = gpGlobals->curtime;
	if ( now < s_flGermanSquadsTime - 1.0f )
		s_GermanSquads.RemoveAll();		// new map
	s_flGermanSquadsTime = now;
	int i = s_GermanSquads.Find( STRING( iszSquad ) );
	if ( i == s_GermanSquads.InvalidIndex() )
	{
		GermanSquadState_t sq;
		memset( &sq, 0, sizeof( sq ) );
		sq.hEnemy = NULL;
		sq.hSuppressor = sq.hBounder = sq.hFlanker = NULL;
		sq.hAssault[0] = sq.hAssault[1] = sq.hOverwatch = NULL;
		sq.flLastFlank = now;
		sq.flNextBound = now + 6.0f;
		sq.flNextFlank = now + 10.0f;
		i = s_GermanSquads.Insert( STRING( iszSquad ), sq );
	}
	return &s_GermanSquads[i];
}

static const char *s_pszSquadOrder[] = { "none", "bound forward", "flank left", "flank right", "fall back", "go after you where you were last seen", "watch where you were last seen" };

bool CNPC_GermanSoldier::IsSquadSuppressor( void )
{
	GermanSquadState_t *pSq = m_pSquad ? GetGermanSquad( m_SquadName ) : NULL;
	return pSq && pSq->hSuppressor.Get() == this && gpGlobals->curtime < pSq->flSuppressUntil;
}

bool CNPC_GermanSoldier::CanTakeSquadOrder( void )
{
	return IsAlive() && m_eSquadOrder == ORDER_NONE && gpGlobals->curtime >= m_flOrderRefusedUntil &&
		   ( IsInCoverLoop() || IsInOpenLoop() || IsCurSchedule( SCHED_GERMAN_HOLD_COVER, false ) );
}

void CNPC_GermanSoldier::GiveSquadOrder( SquadOrder_t eOrder, float flDelay, const Vector &vecEnemyToSquad )
{
	m_eSquadOrder = eOrder;
	m_flSquadOrderIssued = gpGlobals->curtime;
	m_flSquadOrderAt = gpGlobals->curtime + flDelay;
	m_vecFlankFrom = vecEnemyToSquad;
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: squad orders him to %s\n", gpGlobals->curtime, entindex(), s_pszSquadOrder[eOrder] );
}

void CNPC_GermanSoldier::SquadOrderFailed( SquadOrder_t eOrder )
{
	m_flOrderRefusedUntil = gpGlobals->curtime + 15.0f;
	GermanSquadState_t *pSq = m_pSquad ? GetGermanSquad( m_SquadName ) : NULL;
	if ( pSq )
	{
		if ( pSq->hBounder.Get() == this )
			pSq->hBounder = NULL;
		if ( pSq->hFlanker.Get() == this )
			pSq->hFlanker = NULL;
		for ( int k = 0; k < 2; k++ )
			if ( pSq->hAssault[k].Get() == this )
				pSq->hAssault[k] = NULL;
		if ( pSq->hOverwatch.Get() == this )
			pSq->hOverwatch = NULL;
	}
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: can't %s - no spot for it; the squad will ask someone else\n", gpGlobals->curtime, entindex(), s_pszSquadOrder[eOrder] );
}

// Can he put rounds near the enemy's position from where he'd fire (his cover's firing point,
// else his eyes)? The line must reach the enemy's spot or what's right in front of it - not
// his own wall.
bool CNPC_GermanSoldier::CanSuppressEnemy( const Vector &vecTarget )
{
	Vector vecFrom = ( IsInCoverLoop() && m_Cover.bValid ) ? m_Cover.vecExposedEye : EyePosition();
	Vector vecAim = vecTarget + Vector( 0, 0, 40 );
	CTraceFilterSkipTwoEntities filter( this, GetEnemy(), COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( vecFrom, vecAim, COVER_MASK, &filter, &tr );
	if ( tr.fraction >= 1.0f )
		return true;
	return ( tr.endpos - vecAim ).Length() < 128.0f && ( tr.endpos - vecFrom ).Length2D() > m_flCoverMaxGap + 16.0f;
}

void CNPC_GermanSoldier::SquadTick( void )
{
	GermanSquadState_t *pSq = m_pSquad ? GetGermanSquad( m_SquadName ) : NULL;
	float now = gpGlobals->curtime;
	if ( !pSq || now < pSq->flNextTick )
		return;
	pSq->flNextTick = now + 0.5f;
	GermanSquadState_t &sq = *pSq;

	// The living members, and whoever sees the enemy.
	CNPC_GermanSoldier *apMembers[16];
	int nAlive = 0;
	Vector vecCentroid = vec3_origin;
	AISquadIter_t iter;
	for ( CAI_BaseNPC *pMember = m_pSquad->GetFirstMember( &iter ); pMember && nAlive < 16; pMember = m_pSquad->GetNextMember( &iter ) )
	{
		if ( !pMember->IsAlive() || !FClassnameIs( pMember, "npc_german_soldier" ) )
			continue;
		CNPC_GermanSoldier *pG = static_cast<CNPC_GermanSoldier *>( pMember );
		apMembers[nAlive++] = pG;
		vecCentroid += pG->GetAbsOrigin();
		if ( pG->GetEnemy() && pG->HasCondition( COND_SEE_ENEMY ) )
		{
			sq.hEnemy = pG->GetEnemy();
			sq.vecLKP = pG->GetEnemy()->GetAbsOrigin();
			sq.flLastSeen = now;
		}
	}
	if ( nAlive == 0 )
		return;
	vecCentroid /= nAlive;
	sq.nMaxAlive = MAX( sq.nMaxAlive, nAlive );
	if ( ( sq.vecLKP - sq.vecStillPos ).Length2D() > 128.0f )
	{
		sq.vecStillPos = sq.vecLKP;
		sq.flStillSince = now;
	}

	// Watchdog + upkeep: orders nobody took, and roles whose holder is gone or done.
	for ( int i = 0; i < nAlive; i++ )
	{
		CNPC_GermanSoldier *pG = apMembers[i];
		if ( pG->m_eSquadOrder != ORDER_NONE && now - pG->m_flSquadOrderIssued > 5.0f )
		{
			pG->Invariant( INV_ORDER_STUCK, "order '%s' not taken after 5 s (schedule %s) - cancelled",
						   s_pszSquadOrder[pG->m_eSquadOrder], pG->GetCurSchedule() ? pG->GetCurSchedule()->GetName() : "-" );
			pG->m_eSquadOrder = ORDER_NONE;
		}
	}
	CNPC_GermanSoldier *pSup = static_cast<CNPC_GermanSoldier *>( sq.hSuppressor.Get() );
	if ( pSup && ( !pSup->IsAlive() || now >= sq.flSuppressUntil ) )
		sq.hSuppressor = NULL;
	CNPC_GermanSoldier *pBound = static_cast<CNPC_GermanSoldier *>( sq.hBounder.Get() );
	if ( pBound && ( !pBound->IsAlive() ||
		 ( pBound->m_eSquadOrder != ORDER_BOUND && !( pBound->IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && pBound->m_eMoveKind == MOVE_BOUND ) ) ) )
		sq.hBounder = NULL;		// arrived, or gave up
	CNPC_GermanSoldier *pFlank = static_cast<CNPC_GermanSoldier *>( sq.hFlanker.Get() );
	if ( pFlank )
	{
		bool bMoving = pFlank->IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) &&
					   ( pFlank->m_eMoveKind == MOVE_FLANK_LEFT || pFlank->m_eMoveKind == MOVE_FLANK_RIGHT );
		bool bPending = pFlank->m_eSquadOrder == ORDER_FLANK_LEFT || pFlank->m_eSquadOrder == ORDER_FLANK_RIGHT;
		if ( !pFlank->IsAlive() || ( !bMoving && !bPending ) )
			sq.hFlanker = NULL;	// arrived (now fighting from the side), or gave up
		else if ( now > sq.flFlankDeadline )
		{
			pFlank->Invariant( INV_FLANK_TIMEOUT, "flanker still not at his spot after the deadline - giving up on it" );
			pFlank->MarkSpotBad( pFlank->m_vecCoverSpot );
			pFlank->m_eSquadOrder = ORDER_NONE;
			if ( bMoving )
				pFlank->ClearSchedule( "flank took too long" );
			sq.hFlanker = NULL;
		}
	}

	// Assault roles: done when the man has arrived (or given up); too slow -> give up on it.
	bool bAssaultActive = false;
	for ( int k = 0; k < 2; k++ )
	{
		CNPC_GermanSoldier *pA = static_cast<CNPC_GermanSoldier *>( sq.hAssault[k].Get() );
		if ( !pA )
			continue;
		bool bMoving = pA->IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && pA->m_eMoveKind == MOVE_ASSAULT;
		bool bPending = pA->m_eSquadOrder == ORDER_ASSAULT;
		if ( !pA->IsAlive() || ( !bMoving && !bPending ) )
			sq.hAssault[k] = NULL;
		else if ( now > sq.flAssaultDeadline )
		{
			pA->Invariant( INV_ASSAULT_TIMEOUT, "still not at his assault spot after the deadline - giving up on it" );
			pA->MarkSpotBad( pA->m_vecCoverSpot );
			pA->m_eSquadOrder = ORDER_NONE;
			if ( bMoving )
				pA->ClearSchedule( "assault took too long" );
			sq.hAssault[k] = NULL;
		}
		else
			bAssaultActive = true;
	}
	CNPC_GermanSoldier *pWatch = static_cast<CNPC_GermanSoldier *>( sq.hOverwatch.Get() );
	if ( pWatch && ( !pWatch->IsAlive() || ( pWatch->m_eSquadOrder != ORDER_OVERWATCH &&
		 !( pWatch->IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && pWatch->m_eMoveKind == MOVE_OVERWATCH ) ) ) )
		sq.hOverwatch = NULL;

	CBaseEntity *pEnemy = sq.hEnemy.Get();
	if ( !pEnemy || !pEnemy->IsAlive() || now - sq.flLastSeen > 45.0f )
		return;		// nobody knows where he is any more
	float flAggr = m_flSquadAggression;

	// A3c - he's out of sight (in a house, round a corner): don't wait for him. Up to two men work
	// their way to cover next to where he was last seen, by routes he can't see; one other man
	// takes cover watching that spot. ("Vorwaerts!")
	if ( now - sq.flLastSeen > m_flAssaultAfter / flAggr )
	{
		if ( !bAssaultActive && nAlive >= 2 && now >= sq.flNextAssault )
		{
			sq.flNextAssault = now + 20.0f / flAggr;
			int nWant = nAlive >= 4 ? 2 : 1;
			int nGot = 0;
			for ( int k = 0; k < nWant; k++ )
			{
				CNPC_GermanSoldier *pBest = NULL;
				float flBest = FLT_MAX;
				for ( int i = 0; i < nAlive; i++ )
				{
					CNPC_GermanSoldier *pG = apMembers[i];
					if ( !pG->CanTakeSquadOrder() || pG == sq.hAssault[0].Get() || pG == sq.hOverwatch.Get() )
						continue;
					float flDist = ( pG->GetAbsOrigin() - sq.vecLKP ).Length2D();
					if ( flDist < flBest )
					{
						flBest = flDist;
						pBest = pG;
					}
				}
				if ( !pBest )
					break;
				Vector vecTo = pBest->GetAbsOrigin() - sq.vecLKP;
				vecTo.z = 0.0f;
				VectorNormalize( vecTo );
				pBest->GiveSquadOrder( ORDER_ASSAULT, 0.4f + 0.8f * k, vecTo );
				sq.hAssault[k] = pBest;
				nGot++;
			}
			if ( nGot )
			{
				sq.flAssaultDeadline = now + 25.0f;
				if ( !sq.hOverwatch.Get() )
				{
					for ( int i = 0; i < nAlive; i++ )
					{
						CNPC_GermanSoldier *pG = apMembers[i];
						if ( pG->CanTakeSquadOrder() && pG != sq.hAssault[0].Get() && pG != sq.hAssault[1].Get() )
						{
							Vector vecTo = pG->GetAbsOrigin() - sq.vecLKP;
							vecTo.z = 0.0f;
							VectorNormalize( vecTo );
							pG->GiveSquadOrder( ORDER_OVERWATCH, 0.2f, vecTo );
							sq.hOverwatch = pG;
							break;
						}
					}
				}
				if ( npc_german_debug.GetBool() )
					Msg( "[%.2f] npc_german_soldier %d: squad - lost you %.0f s ago: %d going after you, %s watching\n", now, entindex(),
						 now - sq.flLastSeen, nGot, sq.hOverwatch.Get() ? "one" : "nobody" );
			}
		}
		return;		// the rest needs him in sight
	}
	if ( now - sq.flLastSeen > 10.0f )
		return;		// nothing fresh to coordinate against
	Vector vecFromEnemy = vecCentroid - sq.vecLKP;
	vecFromEnemy.z = 0.0f;
	if ( VectorNormalize( vecFromEnemy ) < 1.0f )
		return;
	Vector vecFacing = -vecFromEnemy;
	Vector vecLeft( -vecFacing.y, vecFacing.x, 0.0f );
	float flTempo = m_flSquadTempo;

	// Who can keep his head down (in cover, not already given a job, line to his position).
	auto PickSuppressor = [&]( CNPC_GermanSoldier *pExclude ) -> CNPC_GermanSoldier *
	{
		for ( int i = 0; i < nAlive; i++ )
		{
			CNPC_GermanSoldier *pG = apMembers[i];
			if ( pG != pExclude && pG != sq.hFlanker.Get() && pG != sq.hBounder.Get() && pG->IsInCoverLoop() &&
				 pG->m_eSquadOrder == ORDER_NONE && pG->CanSuppressEnemy( sq.vecLKP ) )
				return pG;
		}
		return NULL;
	};
	auto AssignSuppressor = [&]( CNPC_GermanSoldier *pAsker, float flTime )
	{
		if ( sq.hSuppressor.Get() )
			return;
		CNPC_GermanSoldier *pS = PickSuppressor( pAsker );
		if ( !pS )
			return;
		sq.hSuppressor = pS;
		sq.flSuppressUntil = now + flTime;
		pAsker->m_Chatter.Request( CHAT_ORDER_SUPPRESS, NULL, pS );		// "Niederhalten!" - "Jawohl!"
		if ( npc_german_debug.GetBool() )
			Msg( "[%.2f] npc_german_soldier %d: squad - covering fire on your position for %.0f s\n", now, pS->entindex(), flTime );
	};

	// 1. Fall back: lost half the squad and he's close.
	if ( sq.nMaxAlive >= 2 && nAlive * 2 <= sq.nMaxAlive && ( vecCentroid - sq.vecLKP ).Length2D() < 500.0f && now >= sq.flNextFallback )
	{
		sq.flNextFallback = now + 25.0f;
		int nOrdered = 0;
		for ( int i = 0; i < nAlive && nOrdered < 2; i++ )
		{
			CNPC_GermanSoldier *pG = apMembers[i];
			if ( pG->CanTakeSquadOrder() && ( pG->GetAbsOrigin() - sq.vecLKP ).Length2D() < 700.0f )
			{
				pG->GiveSquadOrder( ORDER_FALLBACK, 0.3f * nOrdered, vecFromEnemy );
				nOrdered++;
			}
		}
		if ( nOrdered )
			return;
	}

	// 2. Flank: three or more of us, and either he's stayed put 8 s, or it's simply time (A3c: a squad
	//    works round your side every so often whatever you do - squad_tempo x squad_aggression).
	bool bFlankTime = now - sq.flStillSince > 8.0f / flTempo || now - sq.flLastFlank > 20.0f / ( flTempo * flAggr );
	if ( nAlive >= 3 && !sq.hFlanker.Get() && now >= sq.flNextFlank && bFlankTime )
	{
		CNPC_GermanSoldier *pBest = NULL;
		float flBestSide = -1.0f, flSide = 0.0f;
		for ( int i = 0; i < nAlive; i++ )
		{
			CNPC_GermanSoldier *pG = apMembers[i];
			if ( !pG->CanTakeSquadOrder() || pG == sq.hSuppressor.Get() || pG == sq.hBounder.Get() )
				continue;
			Vector vecRel = pG->GetAbsOrigin() - vecCentroid;
			float flLat = vecRel.x * vecLeft.x + vecRel.y * vecLeft.y;	// how far out to one side he already is
			if ( fabsf( flLat ) > flBestSide )
			{
				flBestSide = fabsf( flLat );
				flSide = flLat;
				pBest = pG;
			}
		}
		if ( pBest )
		{
			bool bLeft = flSide > 0.0f || ( flSide == 0.0f && random->RandomInt( 0, 1 ) );
			pBest->GiveSquadOrder( bLeft ? ORDER_FLANK_LEFT : ORDER_FLANK_RIGHT, 0.8f, vecFromEnemy );
			sq.hFlanker = pBest;
			sq.flFlankDeadline = now + 30.0f;
			sq.flNextFlank = now + 12.0f / flTempo;		// at least this between flanks
			sq.flLastFlank = now;
			AssignSuppressor( pBest, 7.0f );
			return;
		}
	}

	// 3. Bound: the rear-most man in cover moves up while another keeps his head down.
	if ( nAlive >= 2 && !sq.hBounder.Get() && !sq.hFlanker.Get() && now >= sq.flNextBound )
	{
		sq.flNextBound = now + random->RandomFloat( 8.0f, 14.0f ) / flTempo;
		CNPC_GermanSoldier *pBest = NULL;
		float flBest = 450.0f;		// no need to move up on a man who's already close
		for ( int i = 0; i < nAlive; i++ )
		{
			CNPC_GermanSoldier *pG = apMembers[i];
			if ( !pG->CanTakeSquadOrder() || !pG->IsInCoverLoop() || pG == sq.hSuppressor.Get() )
				continue;
			float flDist = ( pG->GetAbsOrigin() - sq.vecLKP ).Length2D();
			if ( flDist > flBest )
			{
				flBest = flDist;
				pBest = pG;
			}
		}
		if ( pBest )
		{
			AssignSuppressor( pBest, 5.0f );
			pBest->GiveSquadOrder( ORDER_BOUND, sq.hSuppressor.Get() ? 1.2f : 0.3f, vecFromEnemy );
			sq.hBounder = pBest;
		}
	}
}

//=============================================================================
// A3a - battle chatter moments
//=============================================================================
CNPC_GermanSoldier *CNPC_GermanSoldier::NearestSquadmate( float flMaxDist, bool bMustSeeMe )
{
	if ( !m_pSquad )
		return NULL;
	CNPC_GermanSoldier *pBest = NULL;
	float flBest = flMaxDist;
	AISquadIter_t iter;
	for ( CAI_BaseNPC *pMember = m_pSquad->GetFirstMember( &iter ); pMember; pMember = m_pSquad->GetNextMember( &iter ) )
	{
		if ( pMember == this || !pMember->IsAlive() || !FClassnameIs( pMember, "npc_german_soldier" ) )
			continue;
		float flDist = ( pMember->GetAbsOrigin() - GetAbsOrigin() ).Length();
		if ( flDist < flBest && ( !bMustSeeMe || pMember->FVisible( this ) ) )
		{
			flBest = flDist;
			pBest = static_cast<CNPC_GermanSoldier *>( pMember );
		}
	}
	return pBest;
}

// He's off to cover 250+ units away: the nearest squadmate (within 800) tells him where -
// "Hinter die Fässer!" - and he answers. A bound forward gets "Vorwärts!" instead. (CoD rule.)
void CNPC_GermanSoldier::ChatterOrderCoverMove( void )
{
	if ( m_eMoveKind == MOVE_COVER && ( m_vecCoverSpot - GetAbsOrigin() ).Length2D() < 250.0f )
		return;
	CNPC_GermanSoldier *pMate = NearestSquadmate( 800.0f );
	if ( !pMate )
		return;
	switch ( m_eMoveKind )
	{
	case MOVE_BOUND:
	case MOVE_ASSAULT:		pMate->m_Chatter.Request( CHAT_ORDER_FORWARD, NULL, this ); break;
	case MOVE_FLANK_LEFT:	pMate->m_Chatter.Request( CHAT_ORDER_FLANK_LEFT, NULL, this ); break;
	case MOVE_FLANK_RIGHT:	pMate->m_Chatter.Request( CHAT_ORDER_FLANK_RIGHT, NULL, this ); break;
	case MOVE_FALLBACK:		pMate->m_Chatter.Request( CHAT_ORDER_BACK, NULL, this ); break;
	default:				pMate->m_Chatter.Request( CHAT_ORDER_COVER, m_szCoverLandmark, this ); break;
	}
}

// Reloading: "cover me" to the nearest squadmate (who answers), or just "reloading".
void CNPC_GermanSoldier::ChatterReloading( void )
{
	CNPC_GermanSoldier *pMate = NearestSquadmate( 600.0f );
	m_Chatter.Request( pMate ? CHAT_RELOADING_COVERME : CHAT_RELOADING, NULL, pMate );
}

// Seeing the enemy after not seeing him for 15 s: call it out - out in the open, or just
// there, sometimes "attack!" (the squad's minimum waits keep it to one voice at a time).
void CNPC_GermanSoldier::ChatterThreat( void )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( !pEnemy || !HasCondition( COND_SEE_ENEMY ) )
		return;
	float now = gpGlobals->curtime;
	bool bFresh = now - m_flChatterSawEnemy > 15.0f;
	m_flChatterSawEnemy = now;
	if ( !bFresh )
		return;
	if ( random->RandomFloat( 0.0f, 1.0f ) < 0.3f )
	{
		m_Chatter.Request( CHAT_ATTACK );
		return;
	}
	// in the open: his feet are visible too (nothing in front of him)
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( EyePosition(), pEnemy->GetAbsOrigin() + Vector( 0, 0, 8 ), COVER_MASK, &filter, &tr );
	m_Chatter.Request( tr.fraction >= 1.0f ? CHAT_THREAT_EXPOSED : CHAT_THREAT );
}

//=============================================================================
// R4 - the open-ground loop
//=============================================================================
static const char *s_pszOpenState[] = { "aim", "fire", "reload" };

// The pose for the open loop: DoD's aim, crouched or standing (the stance decides); the same
// activity aiming and firing so nothing restarts between shots.
void CNPC_GermanSoldier::OpenPose( void )
{
	Activity act = m_eOpenState == OS_RELOAD ? ( IsCrouching() ? ACT_RELOAD_LOW : ACT_RELOAD ) :
				   ( IsCrouching() ? ACT_RANGE_ATTACK1_LOW : ACT_RANGE_ATTACK1 );
	if ( GetIdealActivity() != act )
		SetIdealActivity( act );
}

void CNPC_GermanSoldier::SetOpenState( OpenState_t eState, const char *pszWhy )
{
	m_flCoverStateTimes[m_iCoverStateFlip] = gpGlobals->curtime;		// same churn watchdog as the cover loop
	m_iCoverStateFlip = ( m_iCoverStateFlip + 1 ) % 6;
	float flSpan = gpGlobals->curtime - m_flCoverStateTimes[m_iCoverStateFlip];
	if ( flSpan < 1.5f )
		Invariant( INV_COVER_CHURN, "6 open-loop states in %.2f s (now %s -> %s: %s)", flSpan,
				   s_pszOpenState[m_eOpenState], s_pszOpenState[eState], pszWhy );
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: open %s -> %s (%s)\n", gpGlobals->curtime, entindex(), s_pszOpenState[m_eOpenState], s_pszOpenState[eState], pszWhy );

	float now = gpGlobals->curtime;
	m_eOpenState = eState;
	switch ( eState )
	{
	case OS_AIM:
		break;
	case OS_FIRE:
		{
			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			int nShots = pWeapon ? MAX( 1, pWeapon->GetRandomBurst() ) : 1;
			if ( IsWounded() )
				nShots = MAX( 1, (int)( nShots * m_flWoundedBurstScale + 0.5f ) );
			m_nShotsLeft = nShots;
			m_flShotInterval = pWeapon ? pWeapon->GetFireRate() : 0.1f;
			m_flNextShotTime = MAX( MAX( m_flNextShotTime, now + 0.15f ), pWeapon ? (float)pWeapon->m_flNextPrimaryAttack : 0.0f );
		}
		break;
	case OS_RELOAD:
		{
			VacateStrategySlot();
			ChatterReloading();
			Activity gesture = ( IsCrouching() && m_actReloadCrouchGesture != ACT_INVALID ) ? m_actReloadCrouchGesture : m_actReloadGesture;
			float flDuration = 1.5f;
			if ( gesture != ACT_INVALID )
			{
				RestartGesture( gesture );
				int iSeq = SelectWeightedSequence( gesture );
				if ( iSeq != ACTIVITY_NOT_AVAILABLE )
					flDuration = SequenceDuration( iSeq );
			}
			m_flReloadDoneTime = now + flDuration * 0.85f;
		}
		break;
	}
	OpenPose();
}

void CNPC_GermanSoldier::RunOpenLoop( void )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( !pEnemy )
	{
		TaskComplete();
		return;
	}
	float now = gpGlobals->curtime;
	GetMotor()->SetIdealYawToTargetAndUpdate( GetEnemyLKP(), AI_KEEP_YAW_SPEED );

	if ( HasCondition( COND_SEE_ENEMY ) )
		m_flOpenLastSaw = now;

	if ( m_eSquadOrder != ORDER_NONE && now >= m_flSquadOrderAt && m_eOpenState != OS_RELOAD )
	{
		VacateStrategySlot();
		m_eCoverExit = CEXIT_ORDER;
		TaskComplete();
		return;
	}

	// Leaving: cover turned up (looks every 3 s, not mid-reload), or nothing to shoot for a while.
	if ( m_eOpenState != OS_RELOAD && now >= m_flOpenNextCoverCheck )
	{
		m_flOpenNextCoverCheck = now + 3.0f;
		m_flNextCoverSearch = now + 3.0f;
		if ( FindCoverSpot( pEnemy ) )
		{
			VacateStrategySlot();
			m_eCoverExit = CEXIT_FOUND_COVER;
			TaskComplete();
			return;
		}
	}
	if ( m_eOpenState != OS_RELOAD && now - m_flOpenLastSaw > m_flCoverHoldTime )
	{
		VacateStrategySlot();
		m_eCoverExit = CEXIT_BORED;
		TaskComplete();
		return;
	}

	// Rounds cracking past a standing man: he gets low. One way only - he never stands back up
	// in this loop (no bobbing).
	if ( m_eStance == STANCE_STAND && now - m_flLastSuppressed < 0.5f )
		SetStance( STANCE_CROUCH, "rounds cracking past - gets low" );
	OpenPose();

	switch ( m_eOpenState )
	{
	case OS_AIM:
		if ( IsClipEmpty() )
		{
			SetOpenState( OS_RELOAD, "clip empty" );
			break;
		}
		if ( now >= m_flOpenStateUntil && HasCondition( COND_CAN_RANGE_ATTACK1 ) )
		{
			if ( ClaimFiringSlot() )
				SetOpenState( OS_FIRE, "has a shot" );
			else
				m_flOpenStateUntil = now + random->RandomFloat( 0.4f, 0.9f );	// squad's firing turns are taken: check again shortly
		}
		break;

	case OS_FIRE:
		if ( IsClipEmpty() )
		{
			SetOpenState( OS_RELOAD, "clip empty" );
			break;
		}
		if ( !HasCondition( COND_CAN_RANGE_ATTACK1 ) || m_nShotsLeft <= 0 )
		{
			VacateStrategySlot();
			m_flOpenStateUntil = now + random->RandomFloat( 0.4f, 1.2f );		// rest between bursts
			SetOpenState( OS_AIM, m_nShotsLeft <= 0 ? "burst done" : "lost the shot" );
			break;
		}
		TryFireShot();
		break;

	case OS_RELOAD:
		if ( IsHitReacting() )
			m_flReloadDoneTime = MAX( m_flReloadDoneTime, m_flFlinchUntil + 0.5f );
		if ( now >= m_flReloadDoneTime )
		{
			FinishReload();
			m_flGestureReloadDone = -1.0f;
			m_flOpenStateUntil = now + 0.2f;
			SetOpenState( OS_AIM, "reloaded" );
		}
		break;
	}
}

//=============================================================================
// R6 - corner cover
//
// The CoD1 corner sets (c1_cn_<weapon><stance>_<side>_*): bolt-action or automatic, standing or
// crouched, corner on his right or left. Each has a hide pose (alert), the step out, an aim/fire
// pose blended across three directions by corner_yaw, the step back, a reload and a death.
//
// Nothing about where those poses put him is assumed. CornerCalibrate puts his own skeleton
// into each pose for an instant and reads where his head is and which way his rifle points
// (from the right hand toward the left, minus the same reading for DoD's plain standing aim).
// The cover search and the cover loop both use those numbers.
//=============================================================================
static const char *s_pszCornerAct[] = { "ALERT", "OUT", "IN", "ENTER", "SEMI", "AUTO", "RELOAD" };

bool CNPC_GermanSoldier::MeasurePose( Activity act, float flCornerYaw, bool bNeutralAim, Vector &vecEyeLocal, matrix3x4_t &matRHand, float &flHandsYaw, float *pflMuzzleYaw )
{
	if ( act == ACT_INVALID || !GetModelPtr() )
		return false;
	int iSeq = SelectWeightedSequence( act );
	int iHead = LookupBone( "ValveBiped.Bip01_Head1" );
	int iRHand = LookupBone( "ValveBiped.Bip01_R_Hand" );
	int iLHand = LookupBone( "ValveBiped.Bip01_L_Hand" );
	if ( iSeq == ACTIVITY_NOT_AVAILABLE || iHead < 0 || iRHand < 0 || iLHand < 0 )
		return false;

	// Pose him for an instant (same think - nothing is sent to the client), read, put him back.
	int iOldSeq = GetSequence();
	float flOldCycle = GetCycle();
	float flOldPose = m_poseCornerYaw >= 0 ? GetPoseParameter( m_poseCornerYaw ) : 0.0f;
	float flOldAimYaw = m_poseAim_Yaw >= 0 ? GetPoseParameter( m_poseAim_Yaw ) : 0.0f;
	float flOldAimPitch = m_poseAim_Pitch >= 0 ? GetPoseParameter( m_poseAim_Pitch ) : 0.0f;
	SetSequence( iSeq );
	SetCycle( 0.0f );
	if ( m_poseCornerYaw >= 0 )
		SetPoseParameter( m_poseCornerYaw, flCornerYaw );
	if ( bNeutralAim )
	{
		// DoD's aim blend at "straight ahead" (what the script's offsets say a 0-degree aim is)
		if ( m_poseAim_Yaw >= 0 )
			SetPoseParameter( m_poseAim_Yaw, m_flYawOffset );
		if ( m_poseAim_Pitch >= 0 )
			SetPoseParameter( m_poseAim_Pitch, m_flPitchOffset );
	}
	InvalidateBoneCache();
	Vector vecHead, vecL;
	QAngle ang;
	GetBonePosition( iHead, vecHead, ang );
	GetBonePosition( iLHand, vecL, ang );
	GetBoneTransform( iRHand, matRHand );
	// The gun as it's actually drawn: his weapon is bone-merged to him, so its muzzle follows
	// this pose. Grip (right hand) to muzzle is the barrel, whatever the animation's bone axes.
	bool bMuzzle = false;
	Vector vecMuzzle;
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	if ( pflMuzzleYaw && pWeapon && pWeapon->GetModelPtr() )
	{
		int iAtt = pWeapon->LookupAttachment( "muzzle" );
		if ( iAtt <= 0 )
			iAtt = 1;
		pWeapon->InvalidateBoneCache();
		QAngle angMuzzle;
		bMuzzle = pWeapon->GetAttachment( iAtt, vecMuzzle, angMuzzle );
	}
	SetSequence( iOldSeq );
	SetCycle( flOldCycle );
	if ( m_poseCornerYaw >= 0 )
		SetPoseParameter( m_poseCornerYaw, flOldPose );
	if ( m_poseAim_Yaw >= 0 )
		SetPoseParameter( m_poseAim_Yaw, flOldAimYaw );
	if ( m_poseAim_Pitch >= 0 )
		SetPoseParameter( m_poseAim_Pitch, flOldAimPitch );
	InvalidateBoneCache();
	if ( pWeapon )
		pWeapon->InvalidateBoneCache();

	WorldToEntitySpace( vecHead, &vecEyeLocal );
	vecEyeLocal.z += 3.0f;		// eyes sit a little above the head bone
	Vector vecR;
	MatrixPosition( matRHand, vecR );
	Vector vecHands = vecL - vecR;		// the old reading, printed alongside for comparison
	flHandsYaw = AngleNormalize( RAD2DEG( atan2f( vecHands.y, vecHands.x ) ) - GetAbsAngles().y );
	if ( pflMuzzleYaw )
	{
		Vector vecBarrel = vecMuzzle - vecR;
		if ( bMuzzle && vecBarrel.Length2D() > 4.0f )
			*pflMuzzleYaw = AngleNormalize( RAD2DEG( atan2f( vecBarrel.y, vecBarrel.x ) ) - GetAbsAngles().y );
		else
			*pflMuzzleYaw = FLT_MAX;	// no gun to read
	}
	return true;
}

// The gun is fixed to his right hand (it's drawn from the hand's attachment), so the direction
// it points is one fixed axis of the hand - found once from DoD's plain straight-ahead aim.
float CNPC_GermanSoldier::GunYaw( const matrix3x4_t &matRHand ) const
{
	Vector vecGun;
	VectorRotate( m_vecGunAxis, matRHand, vecGun );
	return AngleNormalize( RAD2DEG( atan2f( vecGun.y, vecGun.x ) ) - GetAbsAngles().y );
}

// How far (degrees) a target is from a corner set's centre aim, for a man facing flFacing.
// Everything about corner aim angles goes through here: relative to the centre, so an aim
// that happens to point near +-180 degrees can never wrap.
float CNPC_GermanSoldier::CornerAimOffset( const CornerSet_t &set, float flFacing, const Vector &vecFrom, const Vector &vecTarget ) const
{
	Vector vecTo = vecTarget - vecFrom;
	return AngleDiff( RAD2DEG( atan2f( vecTo.y, vecTo.x ) ), flFacing + set.flAimYaw );
}

void CNPC_GermanSoldier::CornerCalibrate( void )
{
	if ( !GetModelPtr() )
		return;
	EnsureAnimMap();		// (a rebuild resets the measurements - do it before, not in the middle)
	if ( m_bCornerCalDone )
		return;
	m_bCornerCalDone = true;
	memset( m_CornerSets, 0, sizeof( m_CornerSets ) );

	// DoD's plain standing aim, aim blend centred, points straight ahead: that tells us which way
	// the gun lies in his right hand. Every corner pose's aim is then read off the same hand axis.
	Vector vecRef;
	matrix3x4_t matRef;
	float flRefHands = 0.0f, flRefMuzzle = FLT_MAX;
	if ( !MeasurePose( TranslateToDoD( ACT_IDLE_ANGRY ), 0.0f, true, vecRef, matRef, flRefHands, &flRefMuzzle ) )
		return;
	Vector vecForward;
	AngleVectors( QAngle( 0, GetAbsAngles().y, 0 ), &vecForward );
	VectorIRotate( vecForward, matRef, m_vecGunAxis );

	static const char s_cWeapon[] = { 'B', 'A' };
	static const char s_cStance[] = { 'S', 'C' };
	static const char s_cSide[] = { 'R', 'L' };
	for ( int w = 0; w < 2; w++ )
	for ( int st = 0; st < 2; st++ )
	for ( int sd = 0; sd < 2; sd++ )
	{
		CornerSet_t &set = m_CornerSets[w][st][sd];
		char sz[64];
		for ( int e = 0; e < CN_DEATH; e++ )
		{
			Q_snprintf( sz, sizeof( sz ), "ACT_WW2_CN_%c%c_%c_%s", s_cWeapon[w], s_cStance[st], s_cSide[sd], s_pszCornerAct[e] );
			set.act[e] = ResolveNamedActivity( sz );
		}
		Q_snprintf( sz, sizeof( sz ), "ACT_WW2_CN_DEATH_%c_%c", s_cStance[st], s_cSide[sd] );
		set.act[CN_DEATH] = ResolveNamedActivity( sz );
		Activity actFire = set.act[CN_SEMI] != ACT_INVALID ? set.act[CN_SEMI] : set.act[CN_AUTO];
		if ( set.act[CN_ALERT] == ACT_INVALID || set.act[CN_OUT] == ACT_INVALID || set.act[CN_IN] == ACT_INVALID || actFire == ACT_INVALID )
			continue;
		Vector vecDummy;
		matrix3x4_t matHide, matAim, matLo, matHi;
		float flHands, flDummy, flMuzzle = FLT_MAX, flMuzzleLo = FLT_MAX, flMuzzleHi = FLT_MAX;
		if ( !MeasurePose( set.act[CN_ALERT], 0.0f, false, set.vecHideEye, matHide, flDummy ) ||
			 !MeasurePose( actFire, 0.0f, false, set.vecAimEye, matAim, flHands, &flMuzzle ) ||
			 !MeasurePose( actFire, -1.0f, false, vecDummy, matLo, flDummy, &flMuzzleLo ) ||
			 !MeasurePose( actFire, 1.0f, false, vecDummy, matHi, flDummy, &flMuzzleHi ) )
			continue;
		// Best reading first: the drawn gun's muzzle (against the same reading in DoD's straight aim);
		// without a gun, the hand axis.
		bool bByMuzzle = flMuzzle != FLT_MAX && flMuzzleLo != FLT_MAX && flMuzzleHi != FLT_MAX && flRefMuzzle != FLT_MAX;
		if ( bByMuzzle )
		{
			set.flAimYaw = AngleNormalize( flMuzzle - flRefMuzzle );
			set.flOffLo = AngleDiff( flMuzzleLo, flMuzzle );
			set.flOffHi = AngleDiff( flMuzzleHi, flMuzzle );
		}
		else
		{
			set.flAimYaw = GunYaw( matAim );
			set.flOffLo = AngleDiff( GunYaw( matLo ), set.flAimYaw );
			set.flOffHi = AngleDiff( GunYaw( matHi ), set.flAimYaw );
		}
		set.bValid = true;
		if ( npc_german_debug.GetBool() )
			Msg( "npc_german_soldier %d: corner set %c%c %s - hide eyes (%.0f %.0f %.0f), out eyes (%.0f %.0f %.0f), aims %.0f deg by %s (blend %+.0f..%+.0f) [muzzle %.0f, hand axis %.0f, hands %.0f]\n",
				 entindex(), s_cWeapon[w], s_cStance[st], sd == 0 ? "right" : "left",
				 set.vecHideEye.x, set.vecHideEye.y, set.vecHideEye.z, set.vecAimEye.x, set.vecAimEye.y, set.vecAimEye.z,
				 set.flAimYaw, bByMuzzle ? "the muzzle" : "the hand axis", set.flOffLo, set.flOffHi,
				 bByMuzzle ? AngleNormalize( flMuzzle - flRefMuzzle ) : 999.0f, GunYaw( matAim ), AngleNormalize( flHands - flRefHands ) );
	}
}

const CNPC_GermanSoldier::CornerSet_t *CNPC_GermanSoldier::CornerSet( void ) const
{
	if ( !m_Cover.bCorner )
		return NULL;
	const CornerSet_t &set = m_CornerSets[m_Cover.corner.iWeapon][m_Cover.corner.iStance][m_Cover.corner.iSide];
	return set.bValid ? &set : NULL;
}

Activity CNPC_GermanSoldier::CornerFireAct( void ) const
{
	const CornerSet_t *p = CornerSet();
	if ( !p )
		return ACT_INVALID;
	Activity actWant = m_bCoverFireSemi ? p->act[CN_SEMI] : p->act[CN_AUTO];
	return actWant != ACT_INVALID ? actWant : ( p->act[CN_SEMI] != ACT_INVALID ? p->act[CN_SEMI] : p->act[CN_AUTO] );
}

static Vector RotateLocal( const Vector &vecLocal, float flYaw )
{
	float s, c;
	SinCos( DEG2RAD( flYaw ), &s, &c );
	return Vector( vecLocal.x * c - vecLocal.y * s, vecLocal.x * s + vecLocal.y * c, vecLocal.z );
}

// Can he use this spot as a corner against that enemy? For each corner set he has (standing
// first, then crouched; corner on his right, then his left): face so the set's aim points at
// the enemy, then (1) in the hide pose his head is out of sight, behind something right next to
// it, (2) the way from there to the stepped-out head is clear (he can lean there), and (3) from
// the stepped-out head he sees the enemy's eyes and chest.
bool CNPC_GermanSoldier::CornerFitAt( const Vector &vecPos, CBaseEntity *pEnemy, CornerFit_t &fit, const Vector *pvecEye )
{
	fit.bValid = false;
	if ( !pEnemy )
		return false;
	CornerCalibrate();
	Vector vecEye = pvecEye ? *pvecEye : pEnemy->EyePosition();
	Vector vecToFoe = vecEye - vecPos;
	float flFoeYaw = RAD2DEG( atan2f( vecToFoe.y, vecToFoe.x ) );
	CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
	int iWeapon = m_bCoverFireSemi ? 0 : 1;
	for ( int st = 0; st < 2; st++ )
	for ( int sd = 0; sd < 2; sd++ )
	{
		const CornerSet_t &set = m_CornerSets[iWeapon][st][sd];
		if ( !set.bValid )
			continue;
		float flYaw = AngleNormalize( flFoeYaw - set.flAimYaw );		// so the set's centre aim points at him
		Vector vecHide = vecPos + RotateLocal( set.vecHideEye, flYaw );
		Vector vecAim = vecPos + RotateLocal( set.vecAimEye, flYaw );
		trace_t tr;
		UTIL_TraceLine( vecEye, vecHide, COVER_MASK, &filter, &tr );
		if ( tr.fraction >= 1.0f || ( vecHide - tr.endpos ).Length2D() > m_flCoverMaxGap )
			continue;		// seen hiding, or hidden only by something far off
		UTIL_TraceLine( vecEye, vecHide - Vector( 0, 0, 20 ), COVER_MASK, &filter, &tr );
		if ( tr.fraction >= 1.0f )
			continue;		// his chest shows
		UTIL_TraceLine( vecHide, vecAim, COVER_MASK, &filter, &tr );
		if ( tr.fraction < 1.0f )
			continue;		// no room to lean out there
		UTIL_TraceLine( vecAim, vecEye, COVER_MASK, &filter, &tr );
		if ( tr.fraction < 1.0f )
			continue;
		UTIL_TraceLine( vecAim, vecEye - Vector( 0, 0, 20 ), COVER_MASK, &filter, &tr );
		if ( tr.fraction < 1.0f )
			continue;
		fit.bValid = true;
		fit.iWeapon = iWeapon;
		fit.iStance = st;
		fit.iSide = sd;
		fit.flYaw = flYaw;
		fit.vecHideEye = vecHide;
		fit.vecAimEye = vecAim;
		return true;
	}
	return false;
}

// At his corner: aim the blend at the enemy (corner_yaw), within what the three aims span.
void CNPC_GermanSoldier::UpdateCornerAim( void )
{
	const CornerSet_t *p = CornerSet();
	if ( !p || m_poseCornerYaw < 0 || !GetEnemy() )
		return;
	float flOff = CornerAimOffset( *p, GetAbsAngles().y, GetAbsOrigin(), GetEnemyLKP() );
	float flPose = 0.0f;
	if ( fabsf( p->flOffHi - p->flOffLo ) > 1.0f )
		flPose = clamp( RemapVal( flOff, p->flOffLo, p->flOffHi, -1.0f, 1.0f ), -1.0f, 1.0f );
	SetPoseParameter( m_poseCornerYaw, flPose );
}

//=============================================================================
// R2 - the cover he occupies
//=============================================================================
bool CNPC_GermanSoldier::OccupyCover( void )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( !pEnemy || m_actHide == ACT_INVALID )
		return false;
	int nType = LowCoverAgainst( pEnemy );		// the same answer the brain got
	bool bSameSpot = m_Cover.bValid && ( m_Cover.vecPos - GetAbsOrigin() ).Length2D() < 24.0f;
	if ( nType == COVER_CORNER )
		return true;		// already at his corner (same set, same facing)
	CornerFit_t fit;
	fit.bValid = false;
	if ( nType <= COVER_FULL )
	{
		// Not low cover: a corner, if that's what he ran here for and it still works from where he stopped.
		if ( !m_PendingCorner.bValid || ( m_vecCoverSpot - GetAbsOrigin() ).Length2D() > 32.0f ||
			 !CornerFitAt( GetAbsOrigin(), pEnemy, fit ) )
			return false;
		nType = COVER_CORNER;
	}
	m_Cover.bValid = true;
	m_Cover.vecPos = GetAbsOrigin();
	m_Cover.nType = nType;
	m_Cover.bCorner = nType == COVER_CORNER;
	m_Cover.corner = fit;
	m_PendingCorner.bValid = false;
	m_flCoverCheckTime = 0.0f;		// the cache was for "not at a corner"
	// The exact point the cover test fired from (one owner for the heights).
	if ( m_Cover.bCorner )
		m_Cover.vecExposedEye = fit.vecAimEye;
	else
		m_Cover.vecExposedEye = m_Cover.vecPos + Vector( 0, 0, nType == COVER_CROUCH ? COVER_CROUCH_EYE : COVER_STAND_EYE );
	if ( !bSameSpot )
		m_Cover.bCouldntSee = false;
	m_Cover.flExposedSince = 0.0f;
	m_vecCoverSpot = m_Cover.vecPos;		// claimed: squadmates pick other spots
	m_bHasCoverSpot = true;
	return true;
}

// From the point he'd fire from - not from wherever his eyes are this think (hidden they're
// below the wall). If he couldn't see you from there and you haven't moved 16 units since,
// he still can't (CoD4) - no popping up just to look at the same empty spot.
bool CNPC_GermanSoldier::CanSeeEnemyFromExposed( void )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( !pEnemy || !m_Cover.bValid )
		return false;
	Vector vecTarget = pEnemy->EyePosition();
	if ( m_Cover.bCouldntSee && ( vecTarget - m_Cover.vecCouldntSeeAt ).Length() < 16.0f )
		return false;
	bool bSee = ( vecTarget - m_Cover.vecExposedEye ).Length() <= GetSenses()->GetDistLook();
	const CornerSet_t *pSet = CornerSet();
	if ( bSee && pSet )
	{
		// a corner aims only across what its three blended aims span (+10 degrees)
		float flOff = CornerAimOffset( *pSet, m_Cover.corner.flYaw, m_Cover.vecPos, vecTarget );
		bSee = flOff >= MIN( pSet->flOffLo, pSet->flOffHi ) - 10.0f && flOff <= MAX( pSet->flOffLo, pSet->flOffHi ) + 10.0f;
	}
	if ( bSee )
	{
		CTraceFilterSkipTwoEntities filter( this, pEnemy, COLLISION_GROUP_NONE );
		trace_t tr;
		UTIL_TraceLine( m_Cover.vecExposedEye, vecTarget, COVER_MASK, &filter, &tr );
		bSee = tr.fraction >= 1.0f;
	}
	m_Cover.bCouldntSee = !bSee;
	if ( !bSee )
		m_Cover.vecCouldntSeeAt = vecTarget;
	return bSee;
}

// Flanked = the enemy can see where he hides, for 0.5 s straight (hysteresis: a glance
// around the wall's end doesn't send him running).
bool CNPC_GermanSoldier::IsFlankedAtCover( void )
{
	if ( !GetEnemy() || !m_Cover.bValid )
		return false;
	if ( LowCoverAgainst( GetEnemy() ) != COVER_EXPOSED )
	{
		m_Cover.flExposedSince = 0.0f;
		return false;
	}
	if ( m_Cover.flExposedSince == 0.0f )
		m_Cover.flExposedSince = gpGlobals->curtime;
	return gpGlobals->curtime - m_Cover.flExposedSince >= 0.5f;
}

//=============================================================================
// R3 - the cover loop (after CoD4's cover_behavior.gsc)
//
//   HIDE    down behind the wall. Decides only when its time is up:
//             pinned by fire -> stay down (reload if not full)
//             clip empty     -> RELOAD
//             can see you from his firing point -> RISE, then SHOOT (needs a squad firing slot)
//             nothing to shoot, clip not full  -> RELOAD
//             look cooldown over (4-15 s)      -> RISE, then LOOK
//             nothing seen for cover_hold_time -> leave the loop (bored: the brain bounds him forward)
//             otherwise wait 0.3-0.4 s
//   RISE    the duck-up transition plays; no shots until it's done
//   SHOOT   up firing until: clip empty, rounds cracking past, lost sight 0.6 s, or
//           cover_expose_max - then DUCK. First shot 0.3-0.6 s after he's up (you see him aim).
//   LOOK    up, aimed at your last spot 1-2.5 s; spots you -> SHOOT
//   DUCK    back down (transition), then HIDE for cover_hide_min..max (longer when wounded)
//   RELOAD  the hidden CoD2 reload
//
// Every state takes time. He leaves the loop only when flanked (hide spot seen 0.5 s) or bored.
//=============================================================================
void CNPC_GermanSoldier::SetCoverState( CoverState_t eState, const char *pszWhy )
{
	// Watchdog: 6 state changes inside 1.5 s means states that don't take time.
	m_flCoverStateTimes[m_iCoverStateFlip] = gpGlobals->curtime;
	m_iCoverStateFlip = ( m_iCoverStateFlip + 1 ) % 6;
	float flSpan = gpGlobals->curtime - m_flCoverStateTimes[m_iCoverStateFlip];
	if ( flSpan < 1.5f )
		Invariant( INV_COVER_CHURN, "6 cover-loop states in %.2f s (now %s -> %s: %s)", flSpan,
				   s_pszCoverState[m_eCoverState], s_pszCoverState[eState], pszWhy );
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: cover %s -> %s (%s)\n", gpGlobals->curtime, entindex(), s_pszCoverState[m_eCoverState], s_pszCoverState[eState], pszWhy );

	float now = gpGlobals->curtime;
	m_eCoverState = eState;
	m_flCoverStateStart = now;
	switch ( eState )
	{
	case CS_HIDE:
		CoverPose( ACT_IDLE );		// hidden stance -> the CoD2 hide pose
		break;
	case CS_RISE:
		CoverPose( m_Cover.nType == COVER_STAND ? ACT_IDLE_ANGRY : ACT_RANGE_AIM_LOW );	// up stance -> CoD2 firing pose (transition first)
		m_flCoverStateUntil = now + 1.5f;	// transition timeout
		break;
	case CS_SHOOT:
		{
			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			int nShots = 1;
			if ( pWeapon )
			{
				nShots = pWeapon->UsesClipsForAmmo1() ? MAX( 1, pWeapon->Clip1() ) : 30;		// CoD: fire until empty...
				if ( m_bCoverFireSemi == false )
					nShots = MIN( nShots, MAX( 1, pWeapon->GetRandomBurst() ) * random->RandomInt( 1, 2 ) );	// ...an SMG gives a burst or two
				m_flShotInterval = pWeapon->GetFireRate();
			}
			if ( IsWounded() )
				nShots = MAX( 1, (int)( nShots * m_flWoundedBurstScale + 0.5f ) );
			m_nShotsLeft = nShots;
			m_flCoverStateUntil = now + random->RandomFloat( m_flCoverExposeMax * 0.5f, m_flCoverExposeMax );	// ...or until he's been up long enough
			m_flNextShotTime = MAX( now + random->RandomFloat( 0.3f, 0.6f ), pWeapon ? (float)pWeapon->m_flNextPrimaryAttack : 0.0f );
			m_flCoverLostSight = 0.0f;
		}
		break;
	case CS_LOOK:
		m_flCoverStateUntil = now + random->RandomFloat( 1.0f, 2.5f );
		m_flCoverNextLook = now + random->RandomFloat( m_flCoverLookMin, m_flCoverLookMax );
		break;
	case CS_DUCK:
		m_bSuppressing = false;
		VacateStrategySlot();
		CoverPose( ACT_IDLE );
		m_flCoverHiddenSince = now;
		break;
	case CS_RELOAD:
		{
			ChatterReloading();
			ResetIdealActivity( ACT_RELOAD_LOW );	// hidden stance -> the CoD2 hidden reload / the corner reload (a fresh one each time)
			Activity act = m_Cover.bCorner && CornerAct( CN_RELOAD ) != ACT_INVALID ? CornerAct( CN_RELOAD ) :
						   m_actCoverReload != ACT_INVALID ? m_actCoverReload : ACT_RELOAD_LOW;
			int iSeq = SelectWeightedSequence( act );
			m_flReloadDoneTime = now + ( iSeq != ACTIVITY_NOT_AVAILABLE ? SequenceDuration( iSeq ) : 2.0f ) * 0.85f;
			m_flGestureReloadDone = 0.0f;
		}
		break;
	}
}

void CNPC_GermanSoldier::CoverDecideWhileHidden( void )
{
	float now = gpGlobals->curtime;
	CBaseCombatWeapon *pWeapon = GetActiveWeapon();
	int nClip = ( pWeapon && pWeapon->UsesClipsForAmmo1() ) ? pWeapon->Clip1() : 1;
	int nMaxClip = ( pWeapon && pWeapon->UsesClipsForAmmo1() ) ? pWeapon->GetMaxClip1() : 1;
	bool bPinned = now - m_flLastSuppressed < m_flUnderFireHide && now - m_flCoverHiddenSince < 10.0f;	// 10 s at most

	if ( bPinned )
	{
		if ( nClip < nMaxClip )
			SetCoverState( CS_RELOAD, "pinned down - reloads while he can't come up" );
		else
		{
			m_flCoverStateUntil = now + random->RandomFloat( 0.4f, 0.8f );
			SetCoverState( CS_HIDE, "pinned down - rounds cracking past" );
		}
		return;
	}
	if ( nClip <= 0 )
	{
		SetCoverState( CS_RELOAD, "clip empty" );
		return;
	}
	// The squad wants covering fire: up, and rounds onto his position even if he can't see him.
	if ( IsSquadSuppressor() && !CanSeeEnemyFromExposed() && GetEnemy() && CanSuppressEnemy( GetEnemyLKP() ) )
	{
		m_bRiseToShoot = true;
		m_bSuppressing = true;
		SetCoverState( CS_RISE, "covering fire on your position for the squad" );
		return;
	}
	if ( CanSeeEnemyFromExposed() )
	{
		if ( !ClaimFiringSlot() )
		{
			m_flCoverStateUntil = now + random->RandomFloat( 0.5f, 1.2f );
			SetCoverState( CS_HIDE, "can see you from the wall - waiting for a squad firing slot" );
			return;
		}
		m_bRiseToShoot = true;
		SetCoverState( CS_RISE, "can see you from the wall - coming up to fire" );
		return;
	}
	if ( nClip < nMaxClip )
	{
		SetCoverState( CS_RELOAD, "nothing to shoot - topping up" );
		return;
	}
	if ( now >= m_flCoverNextLook )
	{
		m_bRiseToShoot = false;
		SetCoverState( CS_RISE, "coming up to look for you" );
		return;
	}
	if ( now >= m_flCoverBoredAt )
	{
		if ( npc_german_debug.GetBool() )
			Msg( "npc_german_soldier %d: cover - bored, nothing to do here for %.0f s\n", entindex(), m_flCoverHoldTime );
		m_eCoverExit = CEXIT_BORED;
		TaskComplete();
		return;
	}
	m_flCoverStateUntil = now + random->RandomFloat( 0.3f, 0.4f );
	SetCoverState( CS_HIDE, "waiting" );
}

void CNPC_GermanSoldier::RunCoverLoop( void )
{
	CBaseEntity *pEnemy = GetEnemy();
	if ( !pEnemy )
	{
		TaskComplete();
		return;
	}
	float now = gpGlobals->curtime;
	if ( m_Cover.bCorner )
	{
		// At a corner he faces the way the corner set needs (measured), and the blend aims.
		GetMotor()->SetIdealYawAndUpdate( m_Cover.corner.flYaw, AI_KEEP_YAW_SPEED );
		UpdateCornerAim();
		float flOff = fabsf( AngleDiff( GetAbsAngles().y, m_Cover.corner.flYaw ) );
		if ( flOff < 25.0f )
			m_flCornerYawBad = 0.0f;
		else if ( m_flCornerYawBad == 0.0f )
			m_flCornerYawBad = now;
		else if ( now - m_flCornerYawBad > 1.5f )
			Invariant( INV_CORNER_YAW, "at his corner but facing %.0f deg off it for %.1f s", flOff, now - m_flCornerYawBad );
	}
	else
		GetMotor()->SetIdealYawToTargetAndUpdate( GetEnemyLKP(), AI_KEEP_YAW_SPEED );	// face his last known spot

	// A squad order (not mid-reload or mid-transition): hand him back to the brain to carry it out.
	if ( m_eSquadOrder != ORDER_NONE && now >= m_flSquadOrderAt && m_eCoverState != CS_RELOAD && !IsInStanceTransition() )
	{
		m_bSuppressing = false;
		m_eCoverExit = CEXIT_ORDER;
		VacateStrategySlot();
		TaskComplete();
		return;
	}

	if ( IsFlankedAtCover() )
	{
		if ( npc_german_debug.GetBool() )
			Msg( "npc_german_soldier %d: cover - flanked (his hide spot has been in your view 0.5 s)\n", entindex() );
		m_eCoverExit = CEXIT_FLANKED;
		m_bHasCoverSpot = false;
		VacateStrategySlot();
		TaskComplete();
		return;
	}

	// The stance each state wants. If the stance owner says "not yet" (min hold), the state waits.
	bool bUp = m_eCoverState == CS_RISE || m_eCoverState == CS_SHOOT || m_eCoverState == CS_LOOK;
	Stance_t eWant = m_Cover.bCorner ? ( bUp ? STANCE_CORNER_AIM : STANCE_CORNER_HIDE ) :
					 bUp ? ( m_Cover.nType == COVER_STAND ? STANCE_MID_UP : STANCE_LOW_UP ) : STANCE_HIDE;
	if ( m_eStance != eWant )
	{
		if ( !SetStance( eWant, s_pszCoverState[m_eCoverState] ) )
			return;
		m_flCoverStateStart = now;		// the state's clock starts once he's actually in it
		if ( m_eCoverState == CS_RISE )
			m_flCoverStateUntil = now + 1.5f;
	}

	switch ( m_eCoverState )
	{
	case CS_HIDE:
		if ( now >= m_flCoverStateUntil )
			CoverDecideWhileHidden();
		break;

	case CS_RISE:
		if ( now - m_flCoverStateStart < 0.25f || ( IsInStanceTransition() && now < m_flCoverStateUntil ) )
			break;
		if ( m_bRiseToShoot )
			SetCoverState( CS_SHOOT, "up" );
		else
			SetCoverState( CS_LOOK, "up" );
		break;

	case CS_SHOOT:
		{
			bool bSee = CanSeeEnemyFromExposed();
			bool bCoverDone = m_bSuppressing && !IsSquadSuppressor();	// the squad's done with his covering fire
			if ( bCoverDone )
				m_bSuppressing = false;
			if ( bSee )
			{
				m_flCoverLastSaw = now;
				m_flCoverBoredAt = now + m_flCoverHoldTime;
				m_flCoverLostSight = 0.0f;
			}
			else if ( m_flCoverLostSight == 0.0f )
				m_flCoverLostSight = now;

			CBaseCombatWeapon *pWeapon = GetActiveWeapon();
			const char *pszDuck = NULL;
			if ( pWeapon && pWeapon->UsesClipsForAmmo1() && pWeapon->Clip1() <= 0 )
				pszDuck = "clip empty - down to reload";
			else if ( m_flLastSuppressed > m_flCoverStateStart + 0.3f )
				pszDuck = "rounds cracking past - ducks";
			else if ( bCoverDone && !bSee )
				pszDuck = "covering fire done";
			else if ( !m_bSuppressing && m_flCoverLostSight > 0.0f && now - m_flCoverLostSight > 0.6f )
				pszDuck = "lost sight of you";
			else if ( m_nShotsLeft <= 0 || now >= m_flCoverStateUntil )
				pszDuck = "done firing";
			if ( pszDuck )
			{
				float flHide = random->RandomFloat( m_flCoverHideMin, m_flCoverHideMax );
				if ( IsWounded() )
					flHide *= m_flWoundedHideScale;
				m_flCoverStateUntil = now + flHide;		// DUCK carries it into HIDE
				SetCoverState( CS_DUCK, pszDuck );
				break;
			}
			if ( bSee || m_bSuppressing )
				TryFireShot();
		}
		break;

	case CS_LOOK:
		if ( CanSeeEnemyFromExposed() && ClaimFiringSlot() )
		{
			m_flCoverLastSaw = now;
			m_flCoverBoredAt = now + m_flCoverHoldTime;
			SetCoverState( CS_SHOOT, "spotted you while looking" );
			break;
		}
		if ( now >= m_flCoverStateUntil )
		{
			m_flCoverStateUntil = now + random->RandomFloat( 0.3f, 0.4f );
			SetCoverState( CS_DUCK, "nothing there" );
		}
		break;

	case CS_DUCK:
		{
			float flHideUntil = m_flCoverStateUntil;
			if ( now - m_flCoverStateStart < 0.3f || ( IsInStanceTransition() && now - m_flCoverStateStart < 1.5f ) )
				break;
			SetCoverState( CS_HIDE, "down" );
			m_flCoverStateUntil = MAX( flHideUntil, now + 0.3f );
		}
		break;

	case CS_RELOAD:
		if ( now >= m_flReloadDoneTime )
		{
			FinishReload();
			m_flGestureReloadDone = -1.0f;
			m_flCoverStateUntil = now + random->RandomFloat( 0.2f, 0.4f );
			SetCoverState( CS_HIDE, "reloaded" );
		}
		break;
	}
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
// How many squadmates are already at, or heading for, a spot within 96 units of this one.
// (Searches score against it: a squad spreads out instead of piling onto one wall.)
int CNPC_GermanSoldier::SquadCrowding( const Vector &vecPos )
{
	if ( !m_pSquad )
		return 0;
	int n = 0;
	AISquadIter_t iter;
	for ( CAI_BaseNPC *pMember = m_pSquad->GetFirstMember( &iter ); pMember; pMember = m_pSquad->GetNextMember( &iter ) )
	{
		if ( pMember == this || !pMember->IsAlive() || !FClassnameIs( pMember, "npc_german_soldier" ) )
			continue;
		CNPC_GermanSoldier *pG = static_cast<CNPC_GermanSoldier *>( pMember );
		Vector vecTheirs = pG->m_bHasCoverSpot ? pG->m_vecCoverSpot : pG->GetAbsOrigin();
		if ( ( vecTheirs - vecPos ).Length2D() < 96.0f )
			n++;
	}
	return n;
}

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
// npc_german_debug 2: every node a search looked at, coloured by why it was or wasn't used,
// for 4 s - tells "the map has no nodes there" apart from "the nodes there aren't cover".
//   grey = out of reach (too high/low, too far, too close to you, taken, recently failed)
//   dark grey = wrong place for this order (not closer / not further back / not on the flank side)
//   red = you'd see him there (or only something far off hides him)
//   orange = out of your sight completely, but he can't shoot from it (inside a house, behind a corner)
//   green = usable low cover   cyan = usable corner (hide beside it, step out to fire)   blue = the one he picked
static void DrawSearchNode( const Vector &vecPos, int r, int g, int b )
{
	if ( npc_german_debug.GetInt() >= 2 )
		NDebugOverlay::Box( vecPos, Vector( -6, -6, 0 ), Vector( 6, 6, 12 ), r, g, b, 160, 4.0f );
}

bool CNPC_GermanSoldier::FindCoverSpot( CBaseEntity *pEnemy, int nMode, const Vector *pvecThreatEye )
{
	if ( !pEnemy || !g_pBigAINet )
		return false;
	bool bAdvance = nMode == SEARCH_ADVANCE;
	bool bFlank = nMode == SEARCH_FLANK_LEFT || nMode == SEARCH_FLANK_RIGHT;
	bool bRetreat = nMode == SEARCH_RETREAT;
	bool bOrdinary = nMode == SEARCH_COVER;
	bool bAssault = nMode == SEARCH_ASSAULT;
	float flRadius = bFlank || bAssault ? m_flFlankRadius : m_flCoverSearchRadius;
	// One owner for "where the threat is" in this search: his eyes now, or where he was last seen.
	Vector vecThreatEye = pvecThreatEye ? *pvecThreatEye : pEnemy->EyePosition();
	Vector vecMe = GetAbsOrigin(), vecFoe = pvecThreatEye ? *pvecThreatEye - Vector( 0, 0, 64 ) : pEnemy->GetAbsOrigin();
	float flMyFoe = ( vecMe - vecFoe ).Length2D();
	// flank geometry: the squad faces the enemy along -m_vecFlankFrom; its left is that turned 90 degrees
	Vector vecFacing = -m_vecFlankFrom;
	Vector vecLeft( -vecFacing.y, vecFacing.x, 0.0f );

	struct Cand_t { Vector pos; float cover; float foe; float score; CornerFit_t fit; };
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
		if ( flDist > flRadius )
			continue;
		if ( fabsf( vecPos.z - vecMe.z ) > 160.0f || flDist < 24.0f )
		{
			DrawSearchNode( vecPos, 120, 120, 120 );
			continue;
		}
		float flFoe = ( vecPos - vecFoe ).Length2D();
		if ( flFoe < 192.0f || IsSpotBad( vecPos ) || IsCoverSpotTaken( vecPos ) )
		{
			DrawSearchNode( vecPos, 120, 120, 120 );
			continue;
		}
		if ( ( bAdvance && flFoe > flMyFoe - 128.0f ) ||		// bounding forward: only cover that actually gains ground
			 ( bRetreat && flFoe < flMyFoe + 192.0f ) ||		// falling back: only cover that gives ground
			 ( bAssault && ( flFoe > 450.0f || flFoe > flMyFoe - 64.0f ) ) )	// going after him: next to where he was, and closer
		{
			DrawSearchNode( vecPos, 50, 50, 50 );
			continue;
		}
		float flFlankDeg = 0.0f;
		if ( bFlank )
		{
			// out to the side of the squad's line on him: 50+ degrees off it, on the ordered side,
			// not too close to him and not so far it's out of the fight
			Vector vecOff = vecPos - vecFoe;
			vecOff.z = 0.0f;
			if ( flFoe < 256.0f || flFoe > 1000.0f )
			{
				DrawSearchNode( vecPos, 50, 50, 50 );
				continue;
			}
			vecOff /= flFoe;
			float flCos = vecOff.x * m_vecFlankFrom.x + vecOff.y * m_vecFlankFrom.y;
			bool bLeft = vecOff.x * vecLeft.x + vecOff.y * vecLeft.y > 0.0f;
			if ( flCos > 0.64f || bLeft != ( nMode == SEARCH_FLANK_LEFT ) )
			{
				DrawSearchNode( vecPos, 50, 50, 50 );
				continue;
			}
			flFlankDeg = RAD2DEG( acosf( clamp( flCos, -1.0f, 1.0f ) ) );
		}
		float flCloser = flMyFoe - flFoe;	// how much ground toward the enemy this spot takes
		if ( bOrdinary && flCloser > 256.0f * m_flSquadAggression )
		{
			DrawSearchNode( vecPos, 50, 50, 50 );
			continue;	// taking cover is not attacking: ground is only taken by bounding / flanking
		}
		nTested++;
		int nCover = CoverAt( vecPos, pEnemy, NULL, &vecThreatEye );
		Cand_t c;
		c.fit.bValid = false;
		if ( nCover == COVER_FULL && CornerFitAt( vecPos, pEnemy, c.fit, &vecThreatEye ) )
		{
			nCover = COVER_CORNER;		// hidden here, and a corner set can step out and shoot from it
			DrawSearchNode( vecPos, 0, 220, 255 );
		}
		else if ( nCover <= COVER_FULL )
		{
			if ( nCover == COVER_FULL )
				DrawSearchNode( vecPos, 255, 140, 0 );
			else
				DrawSearchNode( vecPos, 255, 0, 0 );
			continue;
		}
		else
			DrawSearchNode( vecPos, 0, 255, 0 );
		c.pos = vecPos;
		c.cover = ( nCover == COVER_CROUCH ? 100.0f : nCover == COVER_CORNER ? ( c.fit.iStance == 0 ? 90.0f : 80.0f ) : 80.0f )
				- ( flFoe < 400.0f ? ( 400.0f - flFoe ) * 0.1f : 0.0f );	// not right in his face
		c.foe = flFoe;
		if ( bOrdinary && flCloser > 0.0f )
			c.cover -= flCloser * 0.15f;		// prefer cover that doesn't close on him
		if ( bFlank )
			c.cover += flFlankDeg * 0.5f;		// the further round his side, the better
		if ( bRetreat )
			c.cover += ( flFoe - flMyFoe ) * 0.05f;
		if ( bAssault )
			c.cover += ( 450.0f - flFoe ) * 0.08f;		// the closer to where he was, the better
		c.cover -= 25.0f * SquadCrowding( vecPos );		// spread out: not onto a squadmate's wall
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
	float flBest = -FLT_MAX, flBestLen = 0.0f, flBestExposed = 0.0f;
	Vector vecBest = vec3_origin;
	CornerFit_t fitBest;
	fitBest.bValid = false;
	int nCorners = 0;
	for ( int k = 0; k < nCands; k++ )
		nCorners += cands[k].fit.bValid ? 1 : 0;
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
		if ( flLen > flRadius * 1.5f || ( bAdvance && flLen > m_flBoundMax ) )
		{
			DeleteAll( pRoute );
			continue;	// reachable, but only the long way round (a bound is a short dash)
		}
		// Killzone / TLOU: the run itself matters - every unit of it in your line of fire costs.
		float flExposed = RouteExposure( vecMe, pRoute, pEnemy, &vecThreatEye );
		DeleteAll( pRoute );
		float flScore = c.cover - flLen * 0.08f - flExposed * ( bFlank || bAssault ? 0.25f : 0.1f );	// a flank / an assault must stay out of his sight
		if ( flScore > flBest )
		{
			flBest = flScore;
			vecBest = c.pos;
			fitBest = c.fit;
			flBestLen = flLen;
			flBestExposed = flExposed;
		}
	}
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d: %s - %d spots tested, %d gave cover (%d corners), %d had no route: %s%s (run %.0f, %.0f of it in your line of fire)\n",
			 gpGlobals->curtime, entindex(), bAdvance ? "advance search (closer cover)" : bFlank ? ( nMode == SEARCH_FLANK_LEFT ? "flank search (left)" : "flank search (right)" ) :
			 bRetreat ? "fall-back search" : bAssault ? "assault search (next to where you were)" : pvecThreatEye ? "watch search (your last position)" : "cover search", nTested, nCands, nCorners, nNoRoute,
			 flBest > -FLT_MAX ? "going" : "nothing usable", flBest > -FLT_MAX && fitBest.bValid ? " to a corner" : "", flBestLen, flBestExposed );
	if ( flBest == -FLT_MAX )
		return false;
	if ( npc_german_debug.GetInt() >= 2 )
		NDebugOverlay::Box( vecBest, Vector( -8, -8, 0 ), Vector( 8, 8, 24 ), 0, 80, 255, 200, 4.0f );
	m_vecCoverSpot = vecBest;
	m_PendingCorner = fitBest;
	m_vecCoverSearchEye = vecThreatEye;
	m_bHasCoverSpot = true;
	m_flLastSearchRun = flBestLen;
	const char *pszLandmark = GermanChatter_LandmarkAt( vecBest, vecThreatEye, this, pEnemy );
	Q_strncpy( m_szCoverLandmark, pszLandmark ? pszLandmark : "", sizeof( m_szCoverLandmark ) );
	return true;
}

// Rounds hitting near him (or into him): keep his head down for a while; if he's up in
// low cover, duck right now.
void CNPC_GermanSoldier::NoteUnderFire( void )
{
	m_flUnderFireUntil = gpGlobals->curtime + m_flUnderFireHide;
	m_flLastSuppressed = gpGlobals->curtime;
}

void CNPC_GermanSoldier::UpdateCoverAwareness( void )
{
	if ( !IsAlive() )
		return;
	// Near misses: bullet impacts from someone he's hostile to, close to him.
	if ( HasCondition( COND_HEAR_BULLET_IMPACT ) )
	{
		CSound *pSound = GetBestSound( SOUND_BULLET_IMPACT );
		CBaseEntity *pOwner = pSound ? pSound->m_hOwner.Get() : NULL;
		if ( pSound && pOwner && pOwner != this && IRelationType( pOwner ) == D_HT &&
			 ( pSound->GetSoundOrigin() - WorldSpaceCenter() ).Length() < 96.0f )
			NoteUnderFire();
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
	Q_snprintf( sz, sizeof( sz ), "anim %s%s", GetSequence() >= 0 ? GetSequenceName( GetSequence() ) : "?",
				IsCrouching() ? " (crouched)" : "" );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	if ( GetEnemy() )
	{
		static const char *s_Cover[] = { "EXPOSED", "full cover", "low wall (crouch fire)", "chest-high (stand fire)", "corner" };
		m_flCoverCheckTime = 0.0f;		// fresh check so the traces get drawn
		Q_snprintf( sz, sizeof( sz ), "cover: %s%s%s%s", s_Cover[ LowCoverAgainst( GetEnemy() ) + 1 ],
					gpGlobals->curtime < m_flUnderFireUntil ? "  UNDER FIRE" : "", IsWounded() ? "  WOUNDED" : "",
					m_iMySquadSlot != SQUAD_SLOT_NONE ? "  [firing slot]" : "" );
		NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	}
	Q_snprintf( sz, sizeof( sz ), "stance: %s (%s)%s%s", s_pszStance[m_eStance], m_pszStanceWhy ? m_pszStanceWhy : "?",
				IsCurSchedule( SCHED_GERMAN_COVER, false ) || IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) ? "   cover loop: " :
				IsCurSchedule( SCHED_GERMAN_OPEN, false ) ? "   open loop: " : "",
				IsCurSchedule( SCHED_GERMAN_COVER, false ) || IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) ? s_pszCoverState[m_eCoverState] :
				IsCurSchedule( SCHED_GERMAN_OPEN, false ) ? s_pszOpenState[m_eOpenState] : "" );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	if ( m_Cover.bValid && m_Cover.bCorner )
	{
		// his corner: where his head hides (green), where it comes out to fire (yellow), his facing
		NDebugOverlay::Box( m_Cover.corner.vecHideEye, Vector( -3, -3, -3 ), Vector( 3, 3, 3 ), 0, 255, 0, 160, 0.12f );
		NDebugOverlay::Box( m_Cover.corner.vecAimEye, Vector( -3, -3, -3 ), Vector( 3, 3, 3 ), 255, 255, 0, 160, 0.12f );
		Vector vecFwd;
		AngleVectors( QAngle( 0, m_Cover.corner.flYaw, 0 ), &vecFwd );
		NDebugOverlay::Line( m_Cover.vecPos + Vector( 0, 0, 4 ), m_Cover.vecPos + Vector( 0, 0, 4 ) + vecFwd * 32.0f, 0, 220, 255, true, 0.12f );
		const CornerSet_t *pSet = CornerSet();
		if ( pSet )
		{
			// where the measurement says his gun points when he's out (red): it should run along the barrel
			Vector vecAim;
			AngleVectors( QAngle( 0, m_Cover.corner.flYaw + pSet->flAimYaw, 0 ), &vecAim );
			NDebugOverlay::Line( m_Cover.corner.vecAimEye, m_Cover.corner.vecAimEye + vecAim * 64.0f, 255, 0, 0, true, 0.12f );
		}
		Q_snprintf( sz, sizeof( sz ), "corner: %s, %s%s", m_Cover.corner.iSide == 0 ? "on his right" : "on his left",
					m_Cover.corner.iStance == 0 ? "standing" : "crouched", m_poseCornerYaw >= 0 ? "" : "  (no corner_yaw pose param!)" );
		NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
	}
	Q_snprintf( sz, sizeof( sz ), "health %d/%d", GetHealth(), GetMaxHealth() );
	NDebugOverlay::EntityText( entindex(), line++, sz, 0.12f );
}

// A transition animation has finished: pick the real animation again (the translation
// sees we're no longer in a hide/aim pose and returns the target directly).
void CNPC_GermanSoldier::UpdateCoverTransition( void )
{
	if ( IsInStanceTransition() && gpGlobals->curtime >= m_flStanceTransitionEnd )
	{
		m_actStanceTransition = ACT_INVALID;
		if ( !IsMoving() )
			ResetActivity();	// the transition is over: the stance's own pose
	}
	// Guard: a cover transition still playing well after the stance owner ended it (0.5 s grace -
	// the switch to the next pose takes a think or two). Measured from when it SHOULD have ended,
	// so a long move (a 1.3 s chest-high rise, a corner step-out) is never mistaken for a stuck one.
	if ( GetModelPtr() && GetSequence() >= 0 && IsCoverTransition( GetSequenceActivity( GetSequence() ) ) &&
		 !IsInStanceTransition() && gpGlobals->curtime - MAX( m_flStanceTransitionEnd, m_flSeqStarted ) > 0.5f )
	{
		Invariant( INV_ANIM_STUCK, "transition %s still playing %.1f s after it should have ended (stance %s) - resetting",
				   GetSequenceName( GetSequence() ), gpGlobals->curtime - MAX( m_flStanceTransitionEnd, m_flSeqStarted ), s_pszStance[m_eStance] );
		ResetActivity();
		m_flSeqStarted = gpGlobals->curtime;
	}
}

// One owner for the playback rate: every think, from what is actually playing. The CoD2 firing
// pose over cover holds still between shots (each shot replays its recoil); everything else
// plays at normal speed. (Death animations set their own speed in TASK_DIE, after this.)
void CNPC_GermanSoldier::UpdatePlaybackRate( void )
{
	if ( m_bDeathAnim || !GetModelPtr() || GetSequence() < 0 )
		return;
	Activity actSeq = GetSequenceActivity( GetSequence() );
	bool bCoverFire = actSeq != ACT_INVALID && ( actSeq == m_actCoverLowFire || actSeq == m_actCoverMidFire || actSeq == CornerFireAct() );
	SetPlaybackRate( bCoverFire && gpGlobals->curtime >= m_flCoverRecoilUntil ? 0.0f : 1.0f );
}

Activity CNPC_GermanSoldier::NPC_TranslateActivity( Activity eNewActivity )
{
	// The cover stances pick the CoD2 cover poses: hidden (and the hidden reload), up over a low
	// wall, up over a chest-high one. A pure function of the stance (one owner) - it never
	// looks at the world, so the pose can't flicker with what he senses.
	bool bStatic = !IsMoving();
	bool bIsReload = eNewActivity == ACT_RELOAD || eNewActivity == ACT_RELOAD_LOW;
	bool bIdleOrAim = eNewActivity == ACT_IDLE || eNewActivity == ACT_IDLE_ANGRY || eNewActivity == ACT_IDLE_AIM_AGITATED ||
					  eNewActivity == ACT_COVER_LOW || eNewActivity == ACT_COVER || eNewActivity == ACT_COVER_MED ||
					  eNewActivity == ACT_CROUCHIDLE || eNewActivity == ACT_RANGE_AIM_LOW ||
					  eNewActivity == ACT_RANGE_ATTACK1 || eNewActivity == ACT_RANGE_ATTACK1_LOW;
	if ( bStatic && IsInStanceTransition() && ( bIdleOrAim || bIsReload ) )
		return m_actStanceTransition;		// ducking / rising: the transition, until the stance owner says it's over
	if ( bStatic && m_eStance == STANCE_HIDE && m_actHide != ACT_INVALID )
	{
		if ( ( eNewActivity == ACT_RELOAD || eNewActivity == ACT_RELOAD_LOW ) && m_actCoverReload != ACT_INVALID )
			eNewActivity = m_actCoverReload;
		else if ( bIdleOrAim )
			eNewActivity = m_actHide;
	}
	else if ( bStatic && m_eStance == STANCE_LOW_UP && m_actCoverLowFire != ACT_INVALID && bIdleOrAim )
		eNewActivity = m_actCoverLowFire;
	else if ( bStatic && m_eStance == STANCE_MID_UP && m_actCoverMidFire != ACT_INVALID && bIdleOrAim )
		eNewActivity = m_actCoverMidFire;
	else if ( bStatic && m_eStance == STANCE_CORNER_HIDE && CornerAct( CN_ALERT ) != ACT_INVALID )
	{
		if ( bIsReload && CornerAct( CN_RELOAD ) != ACT_INVALID )
			return CornerAct( CN_RELOAD );
		if ( bIdleOrAim )
			return CornerAct( CN_ALERT );
	}
	else if ( bStatic && m_eStance == STANCE_CORNER_AIM && CornerFireAct() != ACT_INVALID && bIdleOrAim )
		return CornerFireAct();		// held on its first frame (the aim) between shots; each shot plays it

	Activity eResult = TranslateToDoD( eNewActivity );

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
	BaseClass::OnChangeActivity( eNewActivity );	// stands him for ACT_RUN / ACT_RUN_AIM / ACT_WALK
	if ( ( eNewActivity == ACT_RUN || eNewActivity == ACT_RUN_AIM || eNewActivity == ACT_WALK ) && m_eStance != STANCE_STAND )
	{
		m_eStance = STANCE_STAND;
		m_flStanceSince = gpGlobals->curtime;
		m_pszStanceWhy = "moving (engine stands movers)";
	}
	if ( IsMoving() )
		m_actStanceTransition = ACT_INVALID;
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

	Activity actNow = GetSequenceActivity( GetSequence() );
	if ( actNow != ACT_INVALID && ( actNow == m_actCoverLowFire || actNow == m_actCoverMidFire || actNow == CornerFireAct() ) )
	{
		// The CoD2 firing pose has its own recoil: replay it for this shot.
		if ( m_bCoverFireSemi )
		{
			SetCycle( 0.0f );
			m_flCoverRecoilUntil = gpGlobals->curtime + SequenceDuration( GetSequence() );
		}
		else
			m_flCoverRecoilUntil = gpGlobals->curtime + 0.15f;	// automatic: the loop plays while he's firing
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
	if ( pTask && ( pTask->iTask == TASK_RELOAD || pTask->iTask == TASK_GERMAN_OPEN_LOOP || pTask->iTask == TASK_GERMAN_COVER_LOOP ) )
		return;	// these handle their own reload timing

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

// ai_disable stops PrescheduleThink (where the overlay is drawn) but not the think itself:
// keep the overlay up while paused, so a paused fight can be looked at.
void CNPC_GermanSoldier::NPCThink( void )
{
	BaseClass::NPCThink();
	if ( ( CAI_BaseNPC::m_nDebugBits & bits_debugDisableAI ) && npc_german_debug.GetInt() >= 2 && IsAlive() )
		DrawDebugOverlay();
}

void CNPC_GermanSoldier::PrescheduleThink( void )
{
	BaseClass::PrescheduleThink();
	UpdateMovingReload();
	UpdatePainRecovery();
	SyncStance();
	WatchdogPose();
	if ( IsAlive() && GetState() == NPC_STATE_COMBAT )
	{
		ChatterThreat();
		SquadTick();
	}
	m_Chatter.Update();
	UpdateCoverTransition();
	UpdateCoverAwareness();
	if ( npc_german_debug.GetInt() >= 2 )
		DrawDebugOverlay();
	UpdatePlaybackRate();
	UpdateTestPainAll();
	if ( m_bTestCrouch && m_eStance != STANCE_CROUCH )
		SetStance( STANCE_CROUCH, "npc_german_test_crouch", true );
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
			SetStance( STANCE_STAND, "running to cover", true );
			ChatterOrderCoverMove();
			m_flProgressCheck = gpGlobals->curtime + 1.5f;
			m_flProgressDist = ( m_vecCoverSpot - GetAbsOrigin() ).Length2D();
			TaskComplete();
		}
		break;

	case TASK_GERMAN_WATCH:
		{
			// Watching his last spot: up over his cover if he's at one, else as he is.
			if ( IsAtCoverSpot() && m_Cover.bValid && ( m_Cover.vecPos - GetAbsOrigin() ).Length2D() < 24.0f )
				SetStance( m_Cover.bCorner ? STANCE_CORNER_AIM : m_Cover.nType == COVER_STAND ? STANCE_MID_UP : STANCE_LOW_UP, "watching over his cover" );
			SetIdealActivity( ACT_IDLE_ANGRY );
			m_flWatchUntil = gpGlobals->curtime + random->RandomFloat( 1.0f, 2.0f );	// takes time; re-decides after (no instant interrupts: no dithering)
		}
		break;

	case TASK_GERMAN_OPEN_LOOP:
		{
			VacateStrategySlot();
			if ( !GetEnemy() )
			{
				TaskComplete();
				break;
			}
			// Stance once: crouched far off or with rounds about, standing up close. Never stands
			// a crouched man back up (that's the bob).
			float flDist = ( GetEnemy()->GetAbsOrigin() - GetAbsOrigin() ).Length2D();
			bool bLow = flDist > 600.0f || gpGlobals->curtime - m_flLastSuppressed < m_flUnderFireHide || m_eStance == STANCE_CROUCH;
			SetStance( bLow ? STANCE_CROUCH : STANCE_STAND, bLow ? "fighting in the open (low)" : "fighting in the open" );
			m_flOpenNextCoverCheck = gpGlobals->curtime + 3.0f;
			m_flOpenLastSaw = gpGlobals->curtime;	// boredom counts from here: every entry is a full cover_hold_time (no zero-time re-entries)
			m_flOpenStateUntil = gpGlobals->curtime + random->RandomFloat( 0.1f, 0.5f );
			SetOpenState( IsClipEmpty() ? OS_RELOAD : OS_AIM, "no cover he can reach" );
		}
		break;

	case TASK_GERMAN_COVER_LOOP:
		VacateStrategySlot();
		if ( !OccupyCover() )
		{
			if ( IsCurSchedule( SCHED_GERMAN_MOVE_TO_COVER, false ) && m_bHasCoverSpot && GetEnemy() )
			{
				// Arrived, but it doesn't cover him now. Blame the spot only if it was wrong all
				// along (it doesn't cover him even from where the enemy was when it was chosen);
				// if the enemy has simply moved, the spot is fine - he just needs another one now.
				CornerFit_t fitThen;
				bool bSpotWasRight = CoverAt( GetAbsOrigin(), GetEnemy(), NULL, &m_vecCoverSearchEye ) > COVER_FULL ||
									 ( m_PendingCorner.bValid && CornerFitAt( GetAbsOrigin(), GetEnemy(), fitThen, &m_vecCoverSearchEye ) );
				if ( npc_german_debug.GetBool() )
					Msg( "npc_german_soldier %d: reached his spot but it doesn't cover him - %s\n", entindex(),
						 bSpotWasRight ? "you moved since he picked it; looking again" : "the spot was wrong, skipping it" );
				if ( !bSpotWasRight )
					MarkSpotBad( m_vecCoverSpot );
			}
			m_bHasCoverSpot = false;
			m_flNextCoverSearch = 0.0f;
			TaskFail( "not in low cover" );
			break;
		}
		m_eCoverExit = CEXIT_NONE;
		m_flCoverBoredAt = gpGlobals->curtime + m_flCoverHoldTime;
		m_flCoverNextLook = gpGlobals->curtime + random->RandomFloat( 0.5f, 2.0f );
		m_flCoverHiddenSince = gpGlobals->curtime;
		m_flCoverStateUntil = gpGlobals->curtime + random->RandomFloat( 0.05f, 0.8f );	// desync the squad (CoD)
		SetCoverState( CS_HIDE, m_Cover.bCorner ? ( m_Cover.corner.iSide == 0 ? "took a corner (on his right)" : "took a corner (on his left)" ) :
								m_Cover.nType == COVER_STAND ? "took chest-high cover" : "took low cover" );
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
		if ( IsCornerStance( m_eStance ) && m_Cover.bCorner )
		{
			GetMotor()->SetIdealYawAndUpdate( m_Cover.corner.flYaw, AI_KEEP_YAW_SPEED );	// a corner faces its own way
			UpdateCornerAim();
		}
		else if ( GetEnemy() )
			GetMotor()->SetIdealYawToTargetAndUpdate( GetEnemyLKP(), AI_KEEP_YAW_SPEED );
		if ( gpGlobals->curtime >= m_flWatchUntil )
			TaskComplete();
		break;

	case TASK_GERMAN_COVER_LOOP:
		RunCoverLoop();
		break;

	case TASK_GERMAN_OPEN_LOOP:
		RunOpenLoop();
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

	// At a corner: the corner set's own death
	if ( IsCornerStance( m_eStance ) && !IsMoving() && CornerAct( CN_DEATH ) != ACT_INVALID )
		return CornerAct( CN_DEATH );

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
	m_Chatter.OnDeath();
	CNPC_GermanSoldier *pWitness = NearestSquadmate( 800.0f, true );
	if ( pWitness )
		pWitness->m_Chatter.Request( CHAT_CASUALTY, NULL, NULL, random->RandomFloat( 0.4f, 1.0f ) );
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
		SetStance( STANCE_CROUCH, "a pain that ends on one knee", true );
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
	WatchdogNoteSchedule();
	m_iLastShortSched = SelectScheduleInner();
	return m_iLastShortSched;
}

int CNPC_GermanSoldier::SelectScheduleInner( void )
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
		Msg( "[%.2f] npc_german_soldier %d: decide %s - %s\n", gpGlobals->curtime, entindex(), GetSchedulingSymbols()->ScheduleIdToSymbol( GetGlobalScheduleId( iSched ) ), pszWhy );
	m_flDecisionTimes[m_iDecision] = gpGlobals->curtime;
	m_iDecision = ( m_iDecision + 1 ) % 5;
	float flSpan = gpGlobals->curtime - m_flDecisionTimes[m_iDecision];
	if ( flSpan < 1.0f )
		Invariant( INV_DECISION_CHURN, "5 decisions in %.2f s (now %s - %s)", flSpan,
				   GetSchedulingSymbols()->ScheduleIdToSymbol( GetGlobalScheduleId( iSched ) ), pszWhy );
	m_iLastDecision = iSched;
	m_pszLastWhy = pszWhy;
	return iSched;
}

// A failed schedule (no route, stuck) never falls back to the engine's stand-around SCHED_FAIL
// in a fight: he gets low and keeps facing you while the brain re-decides next think.
int CNPC_GermanSoldier::SelectFailSchedule( int failedSchedule, int failedTask, AI_TaskFailureCode_t taskFailCode )
{
	WatchdogNoteSchedule();
	if ( IsAlive() && GetState() == NPC_STATE_COMBAT && GetEnemy() )
	{
		SetStance( STANCE_CROUCH, "last plan failed" );
		return m_iLastShortSched = Decide( SCHED_GERMAN_HOLD_COVER, "last plan failed - crouch and watch" );
	}
	return m_iLastShortSched = BaseClass::SelectFailSchedule( failedSchedule, failedTask, taskFailCode );
}

//-----------------------------------------------------------------------------
// The combat brain (R4-lite). A short priority list; it only chooses WHERE he fights:
//   at low cover  -> the cover loop owns him (hide / rise / shoot / look / reload) until it
//                    hands him back: flanked, or bored (then he may bound forward)
//   not in cover  -> run to the best reachable cover (route-checked, never toward you
//                    except by bounding)
//   no cover anywhere reachable -> fight in the open
// Every movement is ours (the engine's take-cover / line-of-fire moves don't understand
// low walls). The brain runs when a schedule ends or is interrupted - not every think.
//-----------------------------------------------------------------------------
int CNPC_GermanSoldier::SelectCombatSchedule( void )
{
	float now = gpGlobals->curtime;
	VacateStrategySlot();		// every decision re-claims its squad slot
	m_eMoveKind = MOVE_COVER;

	int nCover = LowCoverAgainst( GetEnemy() );
	bool bInCover = nCover > COVER_FULL && m_actHide != ACT_INVALID;
	CoverExit_t eExit = m_eCoverExit;
	m_eCoverExit = CEXIT_NONE;
	bool bFlanked = eExit == CEXIT_FLANKED;

	// 1. A grenade or anything dangerous: get away from it.
	if ( HasCondition( COND_HEAR_DANGER ) )
	{
		m_Chatter.Request( CHAT_INCOMING_GRENADE );
		return Decide( SCHED_TAKE_COVER_FROM_BEST_SOUND, "danger heard" );
	}

	// 2. A squad order: find his own spot for it; if there's none, say so (the squad asks someone else).
	if ( m_eSquadOrder != ORDER_NONE && now >= m_flSquadOrderAt )
	{
		SquadOrder_t eOrder = m_eSquadOrder;
		m_eSquadOrder = ORDER_NONE;
		static const int s_nMode[NUM_ORDERS] = { SEARCH_COVER, SEARCH_ADVANCE, SEARCH_FLANK_LEFT, SEARCH_FLANK_RIGHT, SEARCH_RETREAT, SEARCH_ASSAULT, SEARCH_COVER };
		static const MoveKind_t s_eKind[NUM_ORDERS] = { MOVE_COVER, MOVE_BOUND, MOVE_FLANK_LEFT, MOVE_FLANK_RIGHT, MOVE_FALLBACK, MOVE_ASSAULT, MOVE_OVERWATCH };
		// going after him / watching for him: against where he was last seen, not where he is
		Vector vecLastEye = GetEnemyLKP() + Vector( 0, 0, 64 );
		bool bByLKP = eOrder == ORDER_ASSAULT || eOrder == ORDER_OVERWATCH;
		if ( FindCoverSpot( GetEnemy(), s_nMode[eOrder], bByLKP ? &vecLastEye : NULL ) )
		{
			m_eMoveKind = s_eKind[eOrder];
			if ( eOrder == ORDER_FLANK_LEFT || eOrder == ORDER_FLANK_RIGHT )
			{
				GermanSquadState_t *pSq = GetGermanSquad( m_SquadName );
				if ( pSq && pSq->hFlanker.Get() == this )	// deadline from the actual run: twice a jog, plus 6 s
					pSq->flFlankDeadline = now + clamp( m_flLastSearchRun / 150.0f * 2.0f + 6.0f, 10.0f, 40.0f );
			}
			static const char *s_pszWhy[NUM_ORDERS] = { "", "squad: bound forward", "squad: flank left", "squad: flank right", "squad: fall back",
														"squad: go after him where he was last seen", "squad: watch where he was last seen" };
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, s_pszWhy[eOrder] );
		}
		SquadOrderFailed( eOrder );
	}

	// 3. In low cover (and not just flanked out of it): the cover loop. A man on his own who left it
	//    bored bounds forward by himself (in a squad, the coordinator decides who bounds).
	if ( bInCover && !bFlanked )
	{
		if ( eExit == CEXIT_BORED && !m_pSquad && now >= m_flNextAdvance && ClaimBoundSlot() )
		{
			m_flNextAdvance = now + 4.0f;
			if ( FindCoverSpot( GetEnemy(), SEARCH_ADVANCE ) )
			{
				m_eMoveKind = MOVE_BOUND;
				return Decide( SCHED_GERMAN_MOVE_TO_COVER, "bored at his cover - bounding forward" );
			}
			VacateStrategySlot();
		}
		return Decide( SCHED_GERMAN_COVER, eExit == CEXIT_BORED ? "bored, nowhere better - back to his cover" : "in low cover - cover loop" );
	}

	// 3. The open loop spotted cover (already scored and claimed): go.
	if ( eExit == CEXIT_FOUND_COVER && m_bHasCoverSpot )
		return Decide( SCHED_GERMAN_MOVE_TO_COVER, "spotted cover from the open - running to it" );

	// 4. Flanked, or in the open: run to the best reachable low cover.
	if ( bFlanked || now >= m_flNextCoverSearch )
	{
		m_flNextCoverSearch = now + 3.0f;
		if ( FindCoverSpot( GetEnemy() ) )
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, bFlanked ? "flanked - moving to new cover" : "in the open - running to cover" );
	}

	// 5. Out in the open and nothing to shoot for a while: bound forward if there's closer cover.
	if ( eExit == CEXIT_BORED && !m_pSquad && now >= m_flNextAdvance && ClaimBoundSlot() )
	{
		m_flNextAdvance = now + 4.0f;
		if ( FindCoverSpot( GetEnemy(), SEARCH_ADVANCE ) )
		{
			m_eMoveKind = MOVE_BOUND;
			return Decide( SCHED_GERMAN_MOVE_TO_COVER, "lost sight a while - bounding forward" );
		}
		VacateStrategySlot();
	}

	// 6. No cover he can reach: the open-ground loop.
	return Decide( SCHED_GERMAN_OPEN, eExit == CEXIT_BORED ? "nothing closer - keeps fighting in the open" : "no cover he can reach - fight in the open" );
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
	SetStance( m_bTestCrouch ? STANCE_CROUCH : STANCE_STAND, "npc_german_test_crouch", true );
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

	DECLARE_TASK( TASK_GERMAN_COVER_LOOP )
	DECLARE_TASK( TASK_GERMAN_PATH_TO_COVER )
	DECLARE_TASK( TASK_GERMAN_WATCH )
	DECLARE_TASK( TASK_GERMAN_OPEN_LOOP )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_FIRE1 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_FIRE2 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_FIRE3 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_FIRE4 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_FIRE5 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_BOUND1 )
	DECLARE_SQUADSLOT( SQUAD_SLOT_GERMAN_BOUND2 )
	DECLARE_CONDITION( COND_GERMAN_FLANKED )

	//=========================================================
	// SCHED_GERMAN_COVER: at low cover, the cover loop owns him. It ends on its own when he's
	// flanked or bored; these interrupts are the only other way out.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_COVER,

		"	Tasks"
		"		TASK_STOP_MOVING			0"
		"		TASK_GERMAN_COVER_LOOP		0"
		""
		"	Interrupts"
		"		COND_NEW_ENEMY"
		"		COND_ENEMY_DEAD"
		"		COND_HEAR_DANGER"
	)

	//=========================================================
	// SCHED_GERMAN_MOVE_TO_COVER: run to the chosen low cover (firing on the move), then the
	// cover loop takes him over right there.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_MOVE_TO_COVER,

		"	Tasks"
		"		TASK_GERMAN_PATH_TO_COVER	0"
		"		TASK_WAIT_FOR_MOVEMENT		0"
		"		TASK_GERMAN_COVER_LOOP		0"
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
		"		COND_LIGHT_DAMAGE"
		"		COND_HEAVY_DAMAGE"
		"		COND_HEAR_DANGER"
	)

	//=========================================================
	// SCHED_GERMAN_OPEN: no cover he can reach - the open-ground loop owns him. It ends on its
	// own when cover turns up or he's had nothing to shoot for cover_hold_time.
	//=========================================================
	DEFINE_SCHEDULE
	(
		SCHED_GERMAN_OPEN,

		"	Tasks"
		"		TASK_STOP_MOVING			0"
		"		TASK_GERMAN_OPEN_LOOP		0"
		""
		"	Interrupts"
		"		COND_NEW_ENEMY"
		"		COND_ENEMY_DEAD"
		"		COND_HEAR_DANGER"
	)

AI_END_CUSTOM_NPC()
