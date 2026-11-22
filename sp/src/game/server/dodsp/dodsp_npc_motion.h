//========= DOD-SP ============================================================
// dodsp_npc_motion.h - THE way code (not the AI's own walking) moves an NPC: ride get-outs, bail-outs, and later
// vaults, climbs, jump-downs, parachutes. See dodsp_npc_motion.cpp for why.
//=============================================================================
#ifndef DODSP_NPC_MOTION_H
#define DODSP_NPC_MOTION_H
#pragma once

class CBaseAnimating;

#define DODSP_MOTION_SAMPLES	64		// ring of positions: > the longest delay (cl_interp 0.25 s) in ticks + thinks

class CDoDSPNpcMotion
{
public:
	CDoDSPNpcMotion() { Reset(); }

	// a scripted motion starts: the owner is unparented and MOVETYPE_NONE; stamps the animation clock
	void			Begin( CBaseAnimating *pOwner, const char *pszWhat );
	// every tick of the motion INSTEAD of SetAbsOrigin (moves him, samples the delay line, networks it)
	void			Move( CBaseAnimating *pOwner, const Vector &vecPos );
	// the owner switches sequence inside the motion (call right before setting the new sequence)
	void			SwitchSequence( CBaseAnimating *pOwner );
	// the motion is over (he's back on his own movement): the delay drains, then the engine's glide takes over
	void			End( CBaseAnimating *pOwner );
	// every tick from the owner's PhysicsSimulate, AFTER the base class ran (a think may have moved him); while the
	// motion is active it keeps a think due next tick, so the engine really does simulate him every tick (V3f)
	void			PostSimulate( CBaseAnimating *pOwner );
	// the hooks CBaseEntity's origin/angle send proxies call (DoDSP_NetworkOrigin / DoDSP_NetworkAngles)
	const Vector	*NetworkOrigin( void ) const { return m_bActive ? &m_vecNet : NULL; }
	const QAngle	*NetworkAngles( void ) const { return m_bActive ? &m_angNet : NULL; }
	bool			Active( void ) const { return m_bActive; }
	bool			Moving( void ) const { return m_bActive && m_bMoving; }
	void			Reset( void );			// save loaded / removed / teleported: nothing delayed any more

	static int		DelayTicks( void );		// one owner: how many ticks late the client draws an NPC's pose, minus one
	static int		WindowTicks( bool bNPC );	// V2b2: the client's draw window in ticks - an NPC's (cl_interp_npcs counts), or any other entity's

private:
	struct Sample_t { int nTick; Vector vecOrigin; QAngle angAngles; };
	void			Add( CBaseAnimating *pOwner );
	void			Update( CBaseAnimating *pOwner );
	Sample_t		m_Samples[DODSP_MOTION_SAMPLES];
	int				m_nCount, m_nHead;
	bool			m_bActive, m_bMoving;
	int				m_nEndTick;
	int				m_nLastParentedTick;
	int				m_nLastSimTick;			// V3f: the last tick it was simulated (the watchdog: no tick may be skipped)
	bool			m_bGapWarned;
	Vector			m_vecNet;
	QAngle			m_angNet;
	const char		*m_pszWhat;
};

#endif // DODSP_NPC_MOTION_H
