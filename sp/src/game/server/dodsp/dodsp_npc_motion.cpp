//========= DOD-SP ============================================================
// dodsp_npc_motion.cpp - THE way code (not the AI's own walking) moves an NPC.
//
// INVARIANT: an NPC's position and its pose reach the screen equally late.
//
// The client draws an NPC's pose (cycle, pose parameters, layers) cl_interp late: 0.1 s rounded up to ticks + 1 tick
// = 8 ticks at the defaults (c_baseentity.cpp GetInterpolationAmount; "animated every tick" is false for NPCs). It
// draws its position one tick late (every movetype but STEP is "simulated every tick"). Valve squares that for
// WALKING NPCs on the server: the AI moves him at its think and the networked origin then glides from the last
// think's spot to this one over the next think interval (baseentity.cpp ComputeStepSimulationNetwork) - one think
// (7 ticks) on the server + one tick on the client = the pose's 8. Their comment: "allows the client-side
// interpolation to interpolate animation and simulation data at the same time".
// Anything that moves an NPC OUTSIDE its think breaks that: the engine sees "external code moved the entity", drops
// the glide and networks the raw origin - drawn ~0.1 s ahead of the pose. Our ride get-outs and bail-outs did
// exactly that (MOVETYPE_NONE, origin set every tick): 24-38 units between where his body was drawn and where it
// should be, his feet drawn under the ground before landing (dodsp_tools/motion_sim.py measures it for every
// code-driven animation we have: get-outs, bail-out, the CoD2 traversals, a parachute touchdown).
//
// This class does what the glide does, for code-driven moves: the NETWORKED origin and angles are where he really
// was one delay ago (DelayTicks: the client's own pose window minus its one tick), read from a ring of real
// positions - sampled every tick while code moves him, and at every think otherwise (so the walk before a move and
// after it reads exactly like the engine's glide, and hand-offs are seamless). The real origin stays where he is
// (collision, AI, traces). The animation runs on the engine's normal clock (rate x the model's fps, advanced at
// thinks); the class stamps that clock where a motion starts, where it switches sequence and where it ends
// (StudioFrameAdvance there), so the client's samples sit on those corners. Pose parameters and layers the AI sets
// at its thinks (aim, head) stay smooth - they never leave the engine's window.
// Measured (motion_sim.py, worst bone vs the true motion): stock 24-38 units; this <= 1.7 on every case; the
// walk -> move -> walk hand-off at a run draws 1.7 units per frame throughout (the "animated every tick" clock of
// V3c jumped 13 units at the start, and stepped think-set pose parameters 4.7 degrees at a time).
// The hook: CBaseEntity::UseStepSimulationNetworkOrigin / ...Angles ask DoDSP_NetworkOrigin / DoDSP_NetworkAngles
// first (baseentity.h/.cpp, lint check_npc_motion keeps them there).
//
// V3f - INVARIANT: while a motion is active its owner is simulated EVERY tick. The engine runs PhysicsSimulate for a
// MOVETYPE_NONE entity only on ticks one of its thinks is due (WillSimulateGamePhysics is false for NONE ->
// EFL_NO_GAME_PHYSICS_SIMULATION -> entitylist.cpp CSimThinkManager lists him for his next think tick only). So the
// "every tick" moves of V3b, V3c and V3e all really ran at the soldier's thinks, 0.1-0.2 s apart, while the client -
// told he is "simulated every tick" - drew his position with a one-tick window: it held him, then snapped (owner's
// video 2026-11-20: standing at the deck edge ~0.4 s, then on the ground in one frame). motion_sim.py's 'shipped' clock
// reproduces it (20-37 units). KeepSimulating schedules an empty think context (DODSP_MOTION_THINK) for the next tick
// for as long as the motion is active (the move, and the delay draining after it); a skipped tick prints an INVARIANT.
//=============================================================================
#include "cbase.h"
#include "baseanimating.h"
#include "player.h"
#include "dodsp_npc_motion.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define DODSP_MOTION_THINK	"DoDSPMotionTick"

// V3f: a think due next tick = the engine simulates him next tick (MOVETYPE_NONE alone would wait for his AI think)
static void KeepSimulating( CBaseAnimating *pOwner )
{
	pOwner->SetContextThink( &CBaseEntity::SUB_DoNothing, gpGlobals->curtime + TICK_INTERVAL, DODSP_MOTION_THINK );
}

void CDoDSPNpcMotion::Reset( void )
{
	m_nCount = m_nHead = 0;
	m_bActive = m_bMoving = false;
	m_nEndTick = 0;
	m_nLastParentedTick = -1000;
	m_nLastSimTick = -1;
	m_bGapWarned = false;
	m_vecNet.Init();
	m_angNet.Init();
	m_pszWhat = "";
}

// One owner: the client's pose window for an NPC, in ticks, minus the one tick its position is drawn late. Read
// from the local player's own settings (all user info), the way the client computes it (cdll_bounded_cvars.cpp
// GetClientInterpAmount, c_baseentity.cpp AdjustInterpolationAmount).
static int s_nBaseWindow = 8, s_nNpcWindow = 8;		// DelayTicks' last reading: any entity's window / an NPC's

int CDoDSPNpcMotion::WindowTicks( bool bNPC )
{
	DelayTicks();			// (re)reads the player's settings at most once a second
	return bNPC ? s_nNpcWindow : s_nBaseWindow;
}

int CDoDSPNpcMotion::DelayTicks( void )
{
	static float s_flChecked = -1.0f;
	static int s_nTicks = 7, s_nPrinted = -1;
	if ( s_flChecked >= 0.0f && gpGlobals->curtime >= s_flChecked && gpGlobals->curtime < s_flChecked + 1.0f )
		return s_nTicks;
	s_flChecked = gpGlobals->curtime;
	float flInterp = 0.1f, flRatio = 2.0f, flRate = 20.0f, flNPCs = 0.0f;
	CBasePlayer *pPlayer = UTIL_GetLocalPlayer();
	if ( pPlayer )
	{
		const char *psz;
		if ( ( psz = engine->GetClientConVarValue( pPlayer->entindex(), "cl_interp" ) ) != NULL && psz[0] ) flInterp = Q_atof( psz );
		if ( ( psz = engine->GetClientConVarValue( pPlayer->entindex(), "cl_interp_ratio" ) ) != NULL && psz[0] ) flRatio = Q_atof( psz );
		if ( ( psz = engine->GetClientConVarValue( pPlayer->entindex(), "cl_updaterate" ) ) != NULL && psz[0] ) flRate = Q_atof( psz );
		if ( ( psz = engine->GetClientConVarValue( pPlayer->entindex(), "cl_interp_npcs" ) ) != NULL && psz[0] ) flNPCs = Q_atof( psz );
	}
	float flBase = MAX( flInterp, flRate > 0.0f ? flRatio / flRate : 0.0f );
	int nWindow = TIME_TO_TICKS( flBase ) + 1;
	s_nBaseWindow = nWindow;
	if ( flNPCs > 0.0f )
		nWindow = MAX( nWindow, TIME_TO_TICKS( flNPCs ) + 1 );
	s_nNpcWindow = nWindow;
	s_nTicks = clamp( nWindow - 1, 1, DODSP_MOTION_SAMPLES / 2 );
	if ( s_nTicks != s_nPrinted )
	{
		s_nPrinted = s_nTicks;
		Msg( "DOD-SP NPC motion: the client draws NPC poses %d ticks late (interp %.3f s) - code-driven moves are networked %d ticks late to match%s\n",
			 nWindow, flBase, s_nTicks, s_nTicks == 7 ? "" : " (walking NPCs glide over their 7-tick think, so a hand-off is off by the difference)" );
	}
	return s_nTicks;
}

void CDoDSPNpcMotion::Add( CBaseAnimating *pOwner )
{
	int nTick = gpGlobals->tickcount;
	int iLast = ( m_nHead + DODSP_MOTION_SAMPLES - 1 ) % DODSP_MOTION_SAMPLES;
	if ( m_nCount && m_Samples[iLast].nTick >= nTick )
	{
		m_Samples[iLast].vecOrigin = pOwner->GetAbsOrigin();		// same tick: the latest word on it
		m_Samples[iLast].angAngles = pOwner->GetAbsAngles();
		return;
	}
	Sample_t &s = m_Samples[m_nHead];
	s.nTick = nTick;
	s.vecOrigin = pOwner->GetAbsOrigin();
	s.angAngles = pOwner->GetAbsAngles();
	m_nHead = ( m_nHead + 1 ) % DODSP_MOTION_SAMPLES;
	if ( m_nCount < DODSP_MOTION_SAMPLES )
		m_nCount++;
}

// what is networked this tick: the real position DelayTicks ago, linear between the samples around it
void CDoDSPNpcMotion::Update( CBaseAnimating *pOwner )
{
	if ( !m_nCount )
		Add( pOwner );
	float flAt = (float)( gpGlobals->tickcount - DelayTicks() );
	int iOld = ( m_nHead + DODSP_MOTION_SAMPLES - m_nCount ) % DODSP_MOTION_SAMPLES;
	const Sample_t *pPrev = &m_Samples[iOld], *pNext = pPrev;
	for ( int k = 0; k < m_nCount; k++ )
	{
		const Sample_t *p = &m_Samples[( iOld + k ) % DODSP_MOTION_SAMPLES];
		if ( p->nTick <= flAt )
			pPrev = pNext = p;
		else
		{
			pNext = p;
			break;
		}
	}
	float t = ( pNext->nTick > pPrev->nTick ) ? clamp( ( flAt - pPrev->nTick ) / (float)( pNext->nTick - pPrev->nTick ), 0.0f, 1.0f ) : 1.0f;
	if ( pNext->nTick <= flAt )
		t = 1.0f;
	m_vecNet = pPrev->vecOrigin + ( pNext->vecOrigin - pPrev->vecOrigin ) * t;
	for ( int i = 0; i < 3; i++ )
		m_angNet[i] = pPrev->angAngles[i] + AngleDiff( pNext->angAngles[i], pPrev->angAngles[i] ) * t;
	// the send proxies only run for changed state, and the client stamps the origin with the simulation time
	pOwner->NetworkStateChanged();
	pOwner->SetSimulationTime( gpGlobals->curtime );
}

void CDoDSPNpcMotion::Begin( CBaseAnimating *pOwner, const char *pszWhat )
{
	// fresh from a parent (a seat), a spawn or a long stand: nothing before this moment means anything on screen
	// (the walk's think samples are kept only when they lead right up to now - a move started from a run)
	int nNewest = m_nCount ? m_Samples[( m_nHead + DODSP_MOTION_SAMPLES - 1 ) % DODSP_MOTION_SAMPLES].nTick : -1000000;
	if ( !m_nCount || gpGlobals->tickcount - m_nLastParentedTick <= DelayTicks() + 1 || gpGlobals->tickcount - nNewest > 2 * DelayTicks() + 2 )
		m_nCount = m_nHead = 0;
	m_bActive = m_bMoving = true;
	m_pszWhat = pszWhat ? pszWhat : "";
	m_nLastSimTick = gpGlobals->tickcount;
	m_bGapWarned = false;
	KeepSimulating( pOwner );
	pOwner->StudioFrameAdvance();		// stamp: the motion's sequence (set next by the caller) runs from now
	Add( pOwner );
	Update( pOwner );
}

void CDoDSPNpcMotion::Move( CBaseAnimating *pOwner, const Vector &vecPos )
{
	if ( !m_bMoving )
	{
		static float s_flWarned = -1.0f;
		if ( s_flWarned != gpGlobals->curtime )
			Warning( "INVARIANT DOD-SP NPC motion: %s %d moved by code outside a motion (Begin first) - its position would be drawn ahead of its pose\n",
					 pOwner->GetClassname(), pOwner->entindex() );
		s_flWarned = gpGlobals->curtime;
	}
	if ( pOwner->GetMoveType() != MOVETYPE_NONE || pOwner->GetMoveParent() )
	{
		static float s_flWarned2 = -1.0f;
		if ( s_flWarned2 != gpGlobals->curtime )
			Warning( "INVARIANT DOD-SP NPC motion: %s %d (%s) moved by code with movetype %d%s - the engine would move him too\n", pOwner->GetClassname(),
					 pOwner->entindex(), m_pszWhat, (int)pOwner->GetMoveType(), pOwner->GetMoveParent() ? " and a parent" : "" );
		s_flWarned2 = gpGlobals->curtime;
	}
	pOwner->SetAbsOrigin( vecPos );
	Add( pOwner );
	Update( pOwner );
}

void CDoDSPNpcMotion::SwitchSequence( CBaseAnimating *pOwner )
{
	pOwner->StudioFrameAdvance();		// stamp: the old sequence's last sample sits on the switch
}

void CDoDSPNpcMotion::End( CBaseAnimating *pOwner )
{
	if ( !m_bMoving )
		return;
	pOwner->StudioFrameAdvance();		// stamp: the motion's last frame sits on its end
	Add( pOwner );
	m_bMoving = false;
	m_nEndTick = gpGlobals->tickcount;
}

void CDoDSPNpcMotion::PostSimulate( CBaseAnimating *pOwner )
{
	if ( pOwner->GetMoveParent() )
		m_nLastParentedTick = gpGlobals->tickcount;
	bool bThought = pOwner->GetLastThinkTick() == gpGlobals->tickcount;
	// the AI's own moves happen at its thinks: those are the samples (between them the glide - or this - interpolates)
	if ( bThought && !m_bMoving && !pOwner->GetMoveParent() )
		Add( pOwner );
	if ( !m_bActive )
		return;
	if ( pOwner->GetMoveParent() )
	{
		Warning( "INVARIANT DOD-SP NPC motion: %s %d (%s) was parented while its delayed position was still networked - dropped\n",
				 pOwner->GetClassname(), pOwner->entindex(), m_pszWhat );
		m_bActive = m_bMoving = false;
		return;
	}
	// V3f watchdog: every tick of the motion is simulated (KeepSimulating); a gap = steps on screen
	int nGap = gpGlobals->tickcount - m_nLastSimTick;
	if ( nGap > 1 && !m_bGapWarned )
	{
		m_bGapWarned = true;
		Warning( "INVARIANT DOD-SP NPC motion: %s %d (%s) was simulated %d ticks apart - the engine skipped him (a MOVETYPE_NONE "
				 "entity is simulated only when a think is due), so his position reaches the screen in steps\n",
				 pOwner->GetClassname(), pOwner->entindex(), m_pszWhat, nGap );
	}
	m_nLastSimTick = gpGlobals->tickcount;
	int nSinceEnd = gpGlobals->tickcount - m_nEndTick, nDelay = DelayTicks();
	// hand back to the engine's glide on a think once the delay has drained: the glide then starts from the very
	// spot this line shows (both read the last two think positions), so nothing jumps
	if ( !m_bMoving && nSinceEnd >= nDelay && ( ( bThought && pOwner->GetMoveType() == MOVETYPE_STEP ) || nSinceEnd >= 3 * nDelay ) )
	{
		m_bActive = false;
		return;
	}
	Update( pOwner );
	KeepSimulating( pOwner );
}
