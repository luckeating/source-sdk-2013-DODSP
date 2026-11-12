//====== DOD-SP =====================================================================
//
// German battle chatter (stage A3a).
//
// One owner for what German soldiers say: who talks, when, and who answers. The rules follow
// Call of Duty's battlechatter.gsc: one speaker per squad at a time, minimum waits per kind of
// line (per man and per squad), priorities when two things want saying, and an order is
// answered by the man it was given to ("Hinter die Mauer!" - "Jawohl!").
//
// Sounds: scripts/game_sounds_german_chatter.txt (generated from the de/<voice>/ wave set),
// loaded by the code - no manifest edit needed. Entries are
//     GermanChatter.v<voice>.<category>.<enemy nation>
// Voices are handed out round-robin inside a squad (0, 2, 3, 0, ...) so a squad never answers
// itself in the same voice.
//
//==================================================================================
#ifndef GERMAN_CHATTER_H
#define GERMAN_CHATTER_H
#ifdef _WIN32
#pragma once
#endif

class CAI_BaseNPC;

enum ChatEvent_t
{
	CHAT_THREAT,				// spotted the enemy
	CHAT_THREAT_EXPOSED,		// spotted the enemy out in the open
	CHAT_ATTACK,				// "Angriff!" - attack the infantry
	CHAT_ORDER_COVER,			// to a squadmate running to cover: "behind the <landmark>!" (modifier = landmark)
	CHAT_ORDER_FORWARD,			// to a squadmate bounding forward
	CHAT_ORDER_FLANK,			// A3b
	CHAT_ORDER_FLANK_LEFT,
	CHAT_ORDER_FLANK_RIGHT,
	CHAT_ORDER_SUPPRESS,		// A3b
	CHAT_RELOADING,
	CHAT_RELOADING_COVERME,		// reloading, with a squadmate close by to cover him (target = that squadmate)
	CHAT_INCOMING_GRENADE,
	CHAT_CASUALTY,				// a squadmate went down
	CHAT_ACK_YES,				// answering an order
	CHAT_ACK_COVERING,			// answering "cover me"
	NUM_CHAT_EVENTS
};

class CGermanChatter
{
public:
	CGermanChatter();
	~CGermanChatter();

	void		Init( CAI_BaseNPC *pOwner, const char *pszSquad );
	// Ask to say something. Dropped at once if this kind of line was said too recently (by him
	// or his squad); otherwise kept until it can be said (one speaker per squad) or goes stale.
	// pTarget: who it's said to (orders: he answers). flDelay: not before this many seconds.
	void		Request( ChatEvent_t eEvent, const char *pszModifier = NULL, CBaseEntity *pTarget = NULL, float flDelay = 0.0f );
	void		Update( void );			// every think: say the pending line when it's his turn
	void		OnDeath( void );
	// His own voice when hit / killed (same man as his callouts): cuts off whatever he was saying.
	void		PlayPain( void );					// not every hit, never back to back
	void		PlayDeath( bool bHeadshot );		// a head shot is usually silent
	int			GetVoice( void ) const { return m_iVoice; }
	bool		IsSpeaking( void ) const;

	static void	SetTuning( bool bEnabled, const char *pszNation, float flFrequency, const char *pszCrySpeakers = NULL );
	static void	LevelPrecache( void );	// loads the sound script and precaches every line a soldier can say
	static CGermanChatter *ForEntity( CBaseEntity *pEntity );

private:
	struct Pending_t
	{
		bool		bActive;
		ChatEvent_t	eEvent;
		char		szModifier[32];
		EHANDLE		hTarget;
		float		flNotBefore;
		float		flExpire;
		float		flPriority;
	};

	bool		BuildSoundName( const Pending_t &p, char *pszOut, int nOut );

	EHANDLE		m_hOwner;
	char		m_szSquad[64];
	int			m_iVoice;
	float		m_flNextGroup[8];		// his own minimum waits per kind of line
	float		m_flSpeakingUntil;
	float		m_flNextPain;
	Pending_t	m_Pending;
};

// Which landmark (barrels, wall, kubelwagon...) a soldier at vecSpot hides behind, facing
// vecThreat: a trigger_german_landmark volume if one covers the spot, otherwise the name of
// the prop model in the way. NULL = nothing recognised (the generic line is used).
const char *GermanChatter_LandmarkAt( const Vector &vecSpot, const Vector &vecThreat, CBaseEntity *pIgnore1, CBaseEntity *pIgnore2 );

#endif // GERMAN_CHATTER_H
