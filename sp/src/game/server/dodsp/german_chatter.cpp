//====== DOD-SP =====================================================================
//
// German battle chatter (stage A3a). See german_chatter.h.
//
//==================================================================================
#include "cbase.h"
#include "ai_basenpc.h"
#include "german_chatter.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "engine/IStaticPropMgr.h"
#include "engine/ICollideable.h"
#include "triggers.h"
#include "filesystem.h"

extern ISoundEmitterSystemBase *soundemitterbase;

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ConVar npc_german_debug;

#define CHATTER_SCRIPT	"scripts/game_sounds_german_chatter.txt"

//-----------------------------------------------------------------------------
// The kinds of line, and the rules for each (CoD4 battlechatter.gsc values)
//-----------------------------------------------------------------------------
enum ChatGroup_t { GROUP_THREAT, GROUP_ORDER, GROUP_INFORM, GROUP_REACTION, GROUP_RESPONSE, NUM_GROUPS };

// minimum waits (s): the same man / anyone in his squad
static const float s_flSelfWait[NUM_GROUPS]  = { 12.0f, 8.0f, 6.0f, 1.0f, 1.0f };
static const float s_flSquadWait[NUM_GROUPS] = {  8.0f, 6.0f, 8.0f, 1.0f, 1.0f };
// how long a request stays worth saying while he waits for his turn (s). C1: threats 1 -> 2.5 and
// informs 1 -> 2: a squadmate's 1.5-2 s line used to outlast them, so they were never said.
static const float s_flValid[NUM_GROUPS]     = {  2.5f, 3.0f, 2.0f, 2.0f, 2.0f };

struct ChatDef_t
{
	const char	*pszCategory;		// sound category (ORDER_COVER appends _<landmark>)
	ChatGroup_t	eGroup;
	float		flPriority;
	const char	*pszMeaning;		// what the line means, in English (the chatter log)
};

// One row per ChatEvent_t, in its order (the assert below catches a missing row)
static const ChatDef_t s_ChatDefs[] =
{
	{ "threat_infantry_generic",	GROUP_THREAT,	0.5f, "enemy infantry spotted!" },	// CHAT_THREAT
	{ "threat_infantry_exposed",	GROUP_THREAT,	0.5f, "enemy out in the open!" },	// CHAT_THREAT_EXPOSED
	{ "order_attack_infantry",		GROUP_ORDER,	0.6f, "attack!" },	// CHAT_ATTACK
	{ "order_cover",				GROUP_ORDER,	0.9f, "take cover" },	// CHAT_ORDER_COVER
	{ "order_move_forward",			GROUP_ORDER,	0.9f, "forward!" },	// CHAT_ORDER_FORWARD
	{ "order_move_flank",			GROUP_ORDER,	0.9f, "flank them!" },	// CHAT_ORDER_FLANK
	{ "order_move_flankleft",		GROUP_ORDER,	0.9f, "flank left!" },	// CHAT_ORDER_FLANK_LEFT
	{ "order_move_flankright",		GROUP_ORDER,	0.9f, "flank right!" },	// CHAT_ORDER_FLANK_RIGHT
	{ "order_action_suppress",		GROUP_ORDER,	0.5f, "keep his head down!" },	// CHAT_ORDER_SUPPRESS
	{ "order_move_backward",		GROUP_ORDER,	0.9f, "fall back!" },	// CHAT_ORDER_BACK
	{ "inform_reloading_generic",	GROUP_INFORM,	0.2f, "reloading!" },	// CHAT_RELOADING
	{ "inform_reloading_coverme",	GROUP_INFORM,	0.3f, "reloading - cover me!" },	// CHAT_RELOADING_COVERME
	{ "inform_incoming_grenade",	GROUP_INFORM,	0.95f, "grenade!" },// CHAT_INCOMING_GRENADE
	{ "reaction_casualty_generic",	GROUP_REACTION,	0.5f, "man down!" },	// CHAT_CASUALTY
	{ "response_ack_yes",			GROUP_RESPONSE,	0.9f, "yes, sir!" },	// CHAT_ACK_YES
	{ "response_ack_covering",		GROUP_RESPONSE,	0.9f, "covering you!" },	// CHAT_ACK_COVERING
};
COMPILE_TIME_ASSERT( ARRAYSIZE( s_ChatDefs ) == NUM_CHAT_EVENTS );

// Lines a voice may say that aren't s_ChatDefs rows (precached; checked at load and by the CI lint)
static const char *s_pszExtraCategories[] = { "response_ack_norank_yes", "order_cover_generic" };

static const char *s_pszDropNames[] =
{
	"off", "self-wait", "squad-wait", "kind-wait", "outranked", "replaced", "stale", "no-line", "cut",
};
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszDropNames ) == NUM_CHAT_DROPS );

// C1: all four voices of the set. Voice 1 (1014 lines, files named GE_4_) was left out before only
// because the old generator dropped it.
static const int s_iVoices[] = { 0, 1, 2, 3 };
COMPILE_TIME_ASSERT( ARRAYSIZE( s_iVoices ) == GERMAN_CHATTER_VOICES );

// Landmark names the voice set has lines for (order_cover_<name>)
static const char *s_pszLandmarks[] =
{
	"antiairgun", "barn", "barrels", "boxcar", "brencarrier", "bridge", "brokenwall", "bunker", "car", "church",
	"crashedplane", "crates", "ditch", "embankment", "factory", "flatbed", "fountain", "gully", "halftrack", "haycart",
	"house", "kubelwagon", "largepipe", "rocks", "rubble", "sandbags", "silo", "statue", "streetcar", "tires",
	"tractor", "train", "tree", "trench", "truck", "wall", "well", "woodpile",
};

//-----------------------------------------------------------------------------
// C2 sentence parts. Every literal line kind here is checked against the generated sound script by
// devtools/dodsp_lint_client.py (every voice, every enemy nation).
//-----------------------------------------------------------------------------
#define CHAT_PART_GAP	0.12f		// silence between two parts (the clips are trimmed tight, measured)

// Surnames the voice set says (name_<x>): one per soldier, unique in his squad
static const char *s_pszNames[] =
{
	"name_adler", "name_albrecht", "name_bader", "name_bauer", "name_baumann", "name_becker", "name_bergmann",
	"name_beyer", "name_brandt", "name_brauer", "name_braun", "name_broemel", "name_bruekner", "name_faerber",
	"name_fiedler", "name_fischer", "name_fleischer", "name_gaertner", "name_groenemeier", "name_gross", "name_hahn",
	"name_hoffmann", "name_hohmann", "name_huber", "name_jaeger", "name_kahn", "name_kaltenbach", "name_kaufmann",
	"name_kessler", "name_koch", "name_koehler", "name_koenig", "name_kraemer", "name_krieger", "name_krueger",
	"name_kuemmel", "name_kunze", "name_lange", "name_lehmann", "name_meier", "name_meyer", "name_mueller",
	"name_neuberger", "name_neumann", "name_pfeffer", "name_pfeiffer", "name_reichmann", "name_reimann",
	"name_reiniger", "name_reiter", "name_richter", "name_sauer", "name_schaefer", "name_schlueter", "name_schmidt",
	"name_schneider", "name_schrader", "name_schreiber", "name_schroeder", "name_schuhmann", "name_schultz",
	"name_schumann", "name_schwarz", "name_steinhauer", "name_vogel", "name_wagner", "name_weber", "name_wolf",
	"name_zimmermann",
};
#define CHAT_MAX_NAMES	96
COMPILE_TIME_ASSERT( ARRAYSIZE( s_pszNames ) <= CHAT_MAX_NAMES );

// Where in a building: [floor][opening]. NULL = the set has no such line (the floor's generic one is used).
// The set's floors are American: firstfloor = ground floor.
enum { LOCKIND_GENERIC, LOCKIND_WINDOW, LOCKIND_DOORWAY, LOCKIND_BALCONY, NUM_LOCKINDS };
enum { FLOOR_BASEMENT, FLOOR_GROUND, FLOOR_SECOND, FLOOR_THIRD, FLOOR_TOP, NUM_FLOORS };
static const char *s_pszLocations[NUM_FLOORS][NUM_LOCKINDS] =
{
	{ "location_basement_generic",		"location_basement_window",		NULL,							NULL },
	{ "location_firstfloor_generic",	"location_firstfloor_window",	"location_firstfloor_doorway",	NULL },
	{ "location_secondfloor_generic",	"location_secondfloor_window",	"location_secondfloor_doorway",	"location_secondfloor_balcony" },
	{ "location_thirdfloor_generic",	"location_thirdfloor_window",	NULL,							"location_thirdfloor_balcony" },
	{ "location_topfloor_generic",		"location_topfloor_window",		NULL,							"location_topfloor_balcony" },
};
static const char *s_pszFloorWords[NUM_FLOORS] = { "basement", "ground floor", "second floor", "third floor", "top floor" };
static const char *s_pszKindWords[NUM_LOCKINDS] = { "", ", window", ", doorway", ", balcony" };
static const char *s_pszRoof = "location_roof_generic";

// Compass (Hammer: +Y north, +X east), from east counter-clockwise in 45 degree steps
static const char *s_pszCompass[8] =
{
	"direction_relative_east", "direction_relative_northeast", "direction_relative_north", "direction_relative_northwest",
	"direction_relative_west", "direction_relative_southwest", "direction_relative_south", "direction_relative_southeast",
};
static const char *s_pszCompassWords[8] = { "east", "northeast", "north", "northwest", "west", "southwest", "south", "southeast" };

// The orders at the end of a full threat call
static const char *s_pszAttackWindow = "order_attack_window";

static bool SoundExists( const char *pszName );
static bool EnemyLocation( int iVoice, CBaseEntity *pSpeaker, CBaseEntity *pEnemy, ChatPart_t &part, bool *pbWindow );

//-----------------------------------------------------------------------------
// Tuning (from scripts/npc_german_soldier.txt, set by the soldier)
//-----------------------------------------------------------------------------
static bool		s_bEnabled = true;
// pain/death speaker (m/n/o) for chatter voices 0..3, matched by pitch (measured 2026-11-18: voices
// 0 < 3 < 2 < 1, cries n < o < m): only three cry speakers, so the two highest voices share m
static char		s_cCrySpeaker[GERMAN_CHATTER_VOICES] = { 'n', 'm', 'm', 'o' };
static char		s_szNation[8] = "us";
static float	s_flFrequency = 1.0f;

void CGermanChatter::SetTuning( bool bEnabled, const char *pszNation, float flFrequency, const char *pszCrySpeakers )
{
	// "n m m o": the pain/death speaker (m/n/o) for chatter voices 0..3. Three letters is the
	// pre-C1 form (voices 0, 2, 3 - voice 1 keeps its default).
	if ( pszCrySpeakers )
	{
		char c[GERMAN_CHATTER_VOICES];
		int k = 0;
		for ( const char *p = pszCrySpeakers; *p && k < GERMAN_CHATTER_VOICES; p++ )
			if ( *p == 'm' || *p == 'n' || *p == 'o' )
				c[k++] = *p;
		if ( k == GERMAN_CHATTER_VOICES )
		{
			for ( int i = 0; i < k; i++ )
				s_cCrySpeaker[i] = c[i];
		}
		else if ( k == 3 )
		{
			s_cCrySpeaker[0] = c[0];
			s_cCrySpeaker[2] = c[1];
			s_cCrySpeaker[3] = c[2];
		}
	}
	s_bEnabled = bEnabled;
	Q_strncpy( s_szNation, ( pszNation && pszNation[0] ) ? pszNation : "us", sizeof( s_szNation ) );
	s_flFrequency = clamp( flFrequency, 0.1f, 10.0f );
}

//-----------------------------------------------------------------------------
// Squad state: one speaker at a time, the squad's minimum waits, the voice rotation.
// Keyed by squad name; cleared when a new map starts (game time goes backwards).
//-----------------------------------------------------------------------------
struct SquadChat_t
{
	float	flSpeakingUntil;
	float	flNextGroup[NUM_GROUPS];
	float	flNextCasualty;
	float	flNextReload;
	int		nVoices;
	bool	abNameUsed[CHAT_MAX_NAMES];
	Vector	vecLastFull;		// where the enemy was at the last full threat call (C2)
	float	flLastFull;
};
static CUtlDict<SquadChat_t, int>	s_Squads;
static float						s_flSquadsTime = 0.0f;

// What happened to every request since the game started (npc_german_chatter). Never reset: a reset
// on "game time went backwards" also fired on loading a save, so a fight before the load vanished
// from the counts (C1 test 2026-11-18: soldiers heard talking, report said 0). The log has the detail.
static int		s_nSaid[NUM_CHAT_EVENTS];
static int		s_nDropped[NUM_CHAT_EVENTS][NUM_CHAT_DROPS];
// What the map load found (LevelPrecache)
static int		s_nEntries = 0, s_nWaves = 0, s_nWavesMissing = 0, s_nKindsMissing = 0;
static char		s_szFirstMissing[MAX_PATH] = "";

//-----------------------------------------------------------------------------
// The chatter log: DOD-SP\dodsp_chatter_log.txt, a new file every game start. One line per line
// said (who, to whom, what it means) and per line not said (why). The owner pastes it back.
//-----------------------------------------------------------------------------
static FileHandle_t	s_hLog = FILESYSTEM_INVALID_HANDLE;
static bool			s_bLogTried = false;
static char			s_szLogMap[64] = "";

static void ChatLog( const char *pszFormat, ... )
{
	if ( s_hLog == FILESYSTEM_INVALID_HANDLE )
	{
		if ( s_bLogTried )
			return;
		s_bLogTried = true;
		s_hLog = filesystem->Open( "dodsp_chatter_log.txt", "w", "MOD" );
		if ( s_hLog == FILESYSTEM_INVALID_HANDLE )
		{
			Warning( "German chatter: can't write DOD-SP\\dodsp_chatter_log.txt\n" );
			return;
		}
		filesystem->FPrintf( s_hLog, "DOD-SP German chatter log (a new file every game start). [game time] soldier (voice, squad)\n" );
	}
	const char *pszMap = STRING( gpGlobals->mapname );
	if ( !pszMap )
		pszMap = "";
	if ( Q_strcmp( pszMap, s_szLogMap ) )
	{
		Q_strncpy( s_szLogMap, pszMap, sizeof( s_szLogMap ) );
		filesystem->FPrintf( s_hLog, "\n=== map %s ===\n", pszMap );
	}
	char sz[512];
	va_list ap;
	va_start( ap, pszFormat );
	Q_vsnprintf( sz, sizeof( sz ), pszFormat, ap );
	va_end( ap );
	filesystem->FPrintf( s_hLog, "[%7.2f] %s\n", gpGlobals->curtime, sz );
	filesystem->Flush( s_hLog );
}

// A new map or a loaded save: game time went backwards. The squads' waits start over (their times
// belong to the old clock); the counters don't (see above).
static void CheckNewMap( void )
{
	if ( gpGlobals->curtime < s_flSquadsTime - 1.0f )
	{
		s_Squads.RemoveAll();
		if ( s_hLog != FILESYSTEM_INVALID_HANDLE )
			ChatLog( "--- game time went back from %.2f (map restarted or a save loaded) ---", s_flSquadsTime );
	}
	s_flSquadsTime = gpGlobals->curtime;
}

static SquadChat_t &GetSquadChat( const char *pszSquad )
{
	CheckNewMap();
	int i = s_Squads.Find( pszSquad );
	if ( i == s_Squads.InvalidIndex() )
	{
		SquadChat_t sq;
		memset( &sq, 0, sizeof( sq ) );
		sq.nVoices = abs( (int)HashString( pszSquad ) ) % GERMAN_CHATTER_VOICES;	// squads start their rotation on different voices
		i = s_Squads.Insert( pszSquad, sq );
	}
	return s_Squads[i];
}

//-----------------------------------------------------------------------------
// Registry, so an order can make its target answer
//-----------------------------------------------------------------------------
static CUtlVector<CGermanChatter *> s_AllChatter;

CGermanChatter *CGermanChatter::ForEntity( CBaseEntity *pEntity )
{
	if ( !pEntity )
		return NULL;
	for ( int i = 0; i < s_AllChatter.Count(); i++ )
		if ( s_AllChatter[i]->m_hOwner.Get() == pEntity )
			return s_AllChatter[i];
	return NULL;
}

CGermanChatter::CGermanChatter()
{
	m_szSquad[0] = 0;
	m_iVoice = 0;
	for ( int i = 0; i < 8; i++ )
		m_flNextGroup[i] = 0.0f;
	m_flSpeakingUntil = 0.0f;
	m_flNextPain = 0.0f;
	memset( &m_Pending, 0, sizeof( m_Pending ) );
	m_Sentence.bActive = false;
	m_Sentence.nParts = m_Sentence.iNext = 0;
	m_iName = -1;
	s_AllChatter.AddToTail( this );
}

CGermanChatter::~CGermanChatter()
{
	s_AllChatter.FindAndRemove( this );
}

void CGermanChatter::Init( CAI_BaseNPC *pOwner, const char *pszSquad )
{
	m_hOwner = pOwner;
	if ( pszSquad && pszSquad[0] )
		Q_strncpy( m_szSquad, pszSquad, sizeof( m_szSquad ) );
	else
		Q_snprintf( m_szSquad, sizeof( m_szSquad ), "#solo%d", pOwner ? pOwner->entindex() : 0 );
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	m_iVoice = s_iVoices[ ( sq.nVoices++ ) % GERMAN_CHATTER_VOICES ];
	// a surname nobody else in his squad has (a new squad starts at a random one)
	if ( m_iName < 0 || sq.abNameUsed[m_iName] )
	{
		int nNames = ARRAYSIZE( s_pszNames );
		int iStart = random->RandomInt( 0, nNames - 1 );
		m_iName = iStart;
		for ( int k = 0; k < nNames; k++ )
		{
			int i = ( iStart + k ) % nNames;
			if ( !sq.abNameUsed[i] )
			{
				m_iName = i;
				break;
			}
		}
	}
	sq.abNameUsed[m_iName] = true;
}

const char *CGermanChatter::GetName( void ) const
{
	static char s_sz[4][32];
	static int s_i = 0;
	if ( m_iName < 0 || m_iName >= (int)ARRAYSIZE( s_pszNames ) )
		return "";
	char *psz = s_sz[ ( s_i++ ) & 3 ];
	Q_strncpy( psz, s_pszNames[m_iName] + 5, 32 );		// past "name_"
	psz[0] = (char)toupper( (unsigned char)psz[0] );
	return psz;
}

bool CGermanChatter::IsSpeaking( void ) const
{
	return m_Sentence.bActive || gpGlobals->curtime < m_flSpeakingUntil;
}

void CGermanChatter::OnDeath( void )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( pOwner && IsSpeaking() )
		pOwner->StopSound( pOwner->entindex(), CHAN_VOICE, "" );
	if ( m_Pending.bActive )
		Drop( m_Pending.eEvent, CHATDROP_CUT );
	m_Pending.bActive = false;
	if ( m_Sentence.bActive )
		EndSentence( "killed" );
	// let the squad talk again straight away
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	if ( sq.flSpeakingUntil > gpGlobals->curtime && m_flSpeakingUntil >= sq.flSpeakingUntil )
		sq.flSpeakingUntil = gpGlobals->curtime;
	m_flSpeakingUntil = 0.0f;
}

static int VoiceSlot( int iVoice )
{
	for ( int i = 0; i < GERMAN_CHATTER_VOICES; i++ )
		if ( s_iVoices[i] == iVoice )
			return i;
	return 0;
}

void CGermanChatter::PlayPain( void )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	float now = gpGlobals->curtime;
	if ( !s_bEnabled || !pOwner || !pOwner->IsAlive() || now < m_flNextPain )
		return;
	m_flNextPain = now + random->RandomFloat( 0.8f, 1.6f );
	if ( random->RandomFloat( 0.0f, 1.0f ) < 0.15f )
		return;		// sometimes he just takes it
	if ( IsSpeaking() )
		pOwner->StopSound( pOwner->entindex(), CHAN_VOICE, "" );
	if ( m_Pending.bActive )
		Drop( m_Pending.eEvent, CHATDROP_CUT );
	m_Pending.bActive = false;		// a man who's just been hit doesn't finish his sentence
	if ( m_Sentence.bActive )
		EndSentence( "hit" );
	char szSound[64];
	Q_snprintf( szSound, sizeof( szSound ), "GermanVoice.%c.pain", s_cCrySpeaker[ VoiceSlot( m_iVoice ) ] );
	float flDuration = 0.0f;
	pOwner->EmitSound( szSound, 0.0f, &flDuration );
	m_flSpeakingUntil = now + MAX( flDuration, 0.5f );
}

void CGermanChatter::PlayDeath( bool bHeadshot )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !s_bEnabled || !pOwner )
		return;
	if ( bHeadshot && random->RandomFloat( 0.0f, 1.0f ) < 0.7f )
		return;		// dead before he could make a sound
	char szSound[64];
	Q_snprintf( szSound, sizeof( szSound ), "GermanVoice.%c.death", s_cCrySpeaker[ VoiceSlot( m_iVoice ) ] );
	pOwner->EmitSound( szSound );
}

// Every request that isn't said ends here, with its reason (C1: before, they vanished without a trace).
void CGermanChatter::Drop( ChatEvent_t eEvent, ChatDrop_t eWhy, float flDetail )
{
	CheckNewMap();
	if ( eEvent < 0 || eEvent >= NUM_CHAT_EVENTS || eWhy < 0 || eWhy >= NUM_CHAT_DROPS )
		return;
	s_nDropped[eEvent][eWhy]++;
	{
		CBaseEntity *pOwner = m_hOwner.Get();
		char szDetail[32] = "";
		if ( flDetail > 0.0f )
			Q_snprintf( szDetail, sizeof( szDetail ), " (%.1f s)", flDetail );
		ChatLog( "soldier %d %s (voice %d, %s) NOT SAID %s \"%s\" - %s%s", pOwner ? pOwner->entindex() : 0, GetName(), m_iVoice, m_szSquad,
				 s_ChatDefs[eEvent].pszCategory, s_ChatDefs[eEvent].pszMeaning, s_pszDropNames[eWhy], szDetail );
	}
	if ( npc_german_debug.GetBool() )
	{
		CBaseEntity *pOwner = m_hOwner.Get();
		Msg( "[%.2f] npc_german_soldier %d (voice %d): chatter %s not said - %s", gpGlobals->curtime,
			 pOwner ? pOwner->entindex() : 0, m_iVoice, s_ChatDefs[eEvent].pszCategory, s_pszDropNames[eWhy] );
		if ( flDetail > 0.0f )
			Msg( " (%.1f s)", flDetail );
		Msg( "\n" );
	}
}

bool CGermanChatter::CanSay( ChatEvent_t eEvent ) const
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !s_bEnabled || !pOwner || !pOwner->IsAlive() || eEvent < 0 || eEvent >= NUM_CHAT_EVENTS )
		return false;
	const ChatDef_t &def = s_ChatDefs[eEvent];
	float now = gpGlobals->curtime;
	if ( now < m_flNextGroup[def.eGroup] || m_Sentence.bActive )
		return false;
	return !( m_Pending.bActive && now < m_Pending.flExpire && m_Pending.flPriority > def.flPriority );
}

void CGermanChatter::Request( ChatEvent_t eEvent, const char *pszModifier, CBaseEntity *pTarget, float flDelay, const char *pszNote )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !pOwner || !pOwner->IsAlive() || eEvent < 0 || eEvent >= NUM_CHAT_EVENTS )
		return;
	if ( !s_bEnabled )
	{
		Drop( eEvent, CHATDROP_OFF );
		return;
	}
	const ChatDef_t &def = s_ChatDefs[eEvent];
	float now = gpGlobals->curtime;
	SquadChat_t &sq = GetSquadChat( m_szSquad );

	// Said too recently - by him, by his squad, or (casualty, reloading) this exact kind of line.
	if ( now < m_flNextGroup[def.eGroup] )
	{
		Drop( eEvent, CHATDROP_SELF_WAIT, m_flNextGroup[def.eGroup] - now );
		return;
	}
	if ( now < sq.flNextGroup[def.eGroup] )
	{
		Drop( eEvent, CHATDROP_SQUAD_WAIT, sq.flNextGroup[def.eGroup] - now );
		return;
	}
	if ( eEvent == CHAT_CASUALTY && now < sq.flNextCasualty )
	{
		Drop( eEvent, CHATDROP_KIND_WAIT, sq.flNextCasualty - now );
		return;
	}
	if ( ( eEvent == CHAT_RELOADING || eEvent == CHAT_RELOADING_COVERME ) && now < sq.flNextReload )
	{
		Drop( eEvent, CHATDROP_KIND_WAIT, sq.flNextReload - now );
		return;
	}
	// Something more important is already waiting to be said.
	bool bWaiting = m_Pending.bActive && now < m_Pending.flExpire;
	if ( bWaiting && m_Pending.flPriority > def.flPriority )
	{
		Drop( eEvent, CHATDROP_OUTRANKED );
		return;
	}
	if ( bWaiting )
		Drop( m_Pending.eEvent, CHATDROP_REPLACED );

	m_Pending.bActive = true;
	m_Pending.eEvent = eEvent;
	Q_strncpy( m_Pending.szModifier, pszModifier ? pszModifier : "", sizeof( m_Pending.szModifier ) );
	Q_strncpy( m_Pending.szNote, pszNote ? pszNote : "", sizeof( m_Pending.szNote ) );
	m_Pending.hTarget = pTarget;
	m_Pending.flNotBefore = now + flDelay;
	m_Pending.flExpire = now + flDelay + s_flValid[def.eGroup];
	m_Pending.flPriority = def.flPriority;
}

void CGermanChatter::Update( void )
{
	CBaseAnimating *pOwner = dynamic_cast<CBaseAnimating *>( m_hOwner.Get() );
	float now = gpGlobals->curtime;

	// A sentence under way: the next part when the last one has finished
	if ( m_Sentence.bActive )
	{
		if ( !pOwner || !pOwner->IsAlive() )
			EndSentence( "killed" );
		else if ( now >= m_Sentence.flNextAt )
		{
			if ( !PlaySentencePart() )
				EndSentence( "the man it was for, or the enemy, is gone" );
			else if ( m_Sentence.iNext >= m_Sentence.nParts )
				EndSentence( NULL );
		}
		return;
	}

	if ( !m_Pending.bActive )
		return;
	if ( !pOwner || !pOwner->IsAlive() || now > m_Pending.flExpire )
	{
		m_Pending.bActive = false;	// stale: the moment passed
		Drop( m_Pending.eEvent, ( pOwner && pOwner->IsAlive() ) ? CHATDROP_STALE : CHATDROP_CUT, now - m_Pending.flNotBefore );
		return;
	}
	if ( now < m_Pending.flNotBefore )
		return;
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	if ( now < sq.flSpeakingUntil )
		return;		// someone in his squad is talking: wait his turn

	Pending_t p = m_Pending;
	m_Pending.bActive = false;
	bool bNamed = false;
	int nParts = BuildSentence( p, m_Sentence.parts, bNamed );
	if ( nParts <= 0 )
	{
		Drop( p.eEvent, CHATDROP_NO_LINE );
		return;
	}
	m_Sentence.bActive = true;
	m_Sentence.eEvent = p.eEvent;
	m_Sentence.nParts = nParts;
	m_Sentence.iNext = 0;
	m_Sentence.flNextAt = now;
	m_Sentence.flLastDuration = 0.0f;
	m_Sentence.bNamed = bNamed;
	m_Sentence.hTarget = p.hTarget;
	CAI_BaseNPC *pNPC = pOwner->MyNPCPointer();
	m_Sentence.hEnemy = pNPC ? pNPC->GetEnemy() : NULL;
	Q_strncpy( m_Sentence.szNote, p.szNote, sizeof( m_Sentence.szNote ) );

	s_nSaid[p.eEvent]++;
	const ChatDef_t &def = s_ChatDefs[p.eEvent];
	m_flNextGroup[def.eGroup] = now + s_flSelfWait[def.eGroup] / s_flFrequency;
	sq.flNextGroup[def.eGroup] = now + s_flSquadWait[def.eGroup] / s_flFrequency;
	if ( p.eEvent == CHAT_CASUALTY )
		sq.flNextCasualty = now + 20.0f / s_flFrequency;
	if ( p.eEvent == CHAT_RELOADING || p.eEvent == CHAT_RELOADING_COVERME )
		sq.flNextReload = now + 20.0f / s_flFrequency;

	if ( !PlaySentencePart() )
		EndSentence( "the man it was for, or the enemy, is gone" );
	else if ( m_Sentence.iNext >= m_Sentence.nParts )
		EndSentence( NULL );
}

// One part of the sentence, after checking it still makes sense. Keeps the squad's one-speaker lock
// past the gap to the next part, so nobody talks over him mid-sentence.
bool CGermanChatter::PlaySentencePart( void )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !pOwner || m_Sentence.iNext >= m_Sentence.nParts )
		return false;
	const ChatPart_t &part = m_Sentence.parts[m_Sentence.iNext];
	CBaseEntity *pTarget = m_Sentence.hTarget.Get();
	CBaseEntity *pEnemy = m_Sentence.hEnemy.Get();
	if ( part.eCheck == PARTCHECK_TARGET && ( !pTarget || !pTarget->IsAlive() ) )
		return false;
	if ( part.eCheck == PARTCHECK_ENEMY && ( !pEnemy || !pEnemy->IsAlive() ) )
		return false;
	char szSound[128];
	Q_snprintf( szSound, sizeof( szSound ), "GermanChatter.v%d.%s.%s", m_iVoice, part.szCategory, s_szNation );
	float flDuration = 0.0f;
	pOwner->EmitSound( szSound, 0.0f, &flDuration );
	if ( flDuration <= 0.0f )
		flDuration = 1.0f;
	float now = gpGlobals->curtime;
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	m_flSpeakingUntil = sq.flSpeakingUntil = now + flDuration + 0.2f;
	m_Sentence.flLastDuration = flDuration;
	m_Sentence.flNextAt = now + flDuration + CHAT_PART_GAP;
	m_Sentence.iNext++;
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d (voice %d) says: %s\n", now, pOwner->entindex(), m_iVoice, szSound );
	return true;
}

// The sentence is over - said in full (pszCut NULL) or cut short. One log line with every part said,
// and the man it was said to answers (only a sentence said in full).
void CGermanChatter::EndSentence( const char *pszCut )
{
	if ( !m_Sentence.bActive )
		return;
	m_Sentence.bActive = false;
	CBaseEntity *pOwner = m_hOwner.Get();
	CBaseEntity *pTarget = m_Sentence.hTarget.Get();
	ChatEvent_t eEvent = m_Sentence.eEvent;

	char szKinds[256] = "", szMeaning[320] = "";
	for ( int i = 0; i < m_Sentence.iNext; i++ )
	{
		if ( i )
		{
			Q_strncat( szKinds, " + ", sizeof( szKinds ) );
			Q_strncat( szMeaning, " ", sizeof( szMeaning ) );
		}
		Q_strncat( szKinds, m_Sentence.parts[i].szCategory, sizeof( szKinds ) );
		Q_strncat( szMeaning, m_Sentence.parts[i].szMeaning, sizeof( szMeaning ) );
	}
	if ( eEvent == CHAT_ORDER_COVER && m_Sentence.szNote[0] )
	{
		Q_strncat( szMeaning, " (no landmark recognised: ", sizeof( szMeaning ) );
		Q_strncat( szMeaning, m_Sentence.szNote, sizeof( szMeaning ) );
		Q_strncat( szMeaning, ")", sizeof( szMeaning ) );
	}
	char szTo[64] = "";
	if ( pTarget )
	{
		CGermanChatter *pTo = ForEntity( pTarget );
		Q_snprintf( szTo, sizeof( szTo ), " -> to %s %d %s", pTarget->IsPlayer() ? "the player" : "soldier", pTarget->entindex(),
					pTo ? pTo->GetName() : "" );
	}
	char szCut[96] = "";
	if ( pszCut )
		Q_snprintf( szCut, sizeof( szCut ), " [cut after %d of %d parts: %s]", m_Sentence.iNext, m_Sentence.nParts, pszCut );
	ChatLog( "soldier %d %s (voice %d, %s) SAID %s \"%s\"%s%s", pOwner ? pOwner->entindex() : 0, GetName(), m_iVoice, m_szSquad,
			 szKinds, szMeaning, szTo, szCut );
	if ( pszCut || !pOwner )
		return;

	// The man an order (or a named call) was given to answers it.
	CGermanChatter *pAnswer = ForEntity( pTarget );
	if ( !pAnswer || pAnswer == this )
		return;
	float flDelay = m_Sentence.flLastDuration + 0.3f;
	if ( eEvent == CHAT_ORDER_COVER || eEvent == CHAT_ORDER_FORWARD || eEvent == CHAT_ORDER_FLANK ||
		 eEvent == CHAT_ORDER_FLANK_LEFT || eEvent == CHAT_ORDER_FLANK_RIGHT || eEvent == CHAT_ORDER_SUPPRESS ||
		 eEvent == CHAT_ORDER_BACK || ( m_Sentence.bNamed && ( eEvent == CHAT_THREAT || eEvent == CHAT_THREAT_EXPOSED ) ) )
		pAnswer->Request( CHAT_ACK_YES, NULL, pOwner, flDelay );
	else if ( eEvent == CHAT_RELOADING_COVERME )
		pAnswer->Request( CHAT_ACK_COVERING, NULL, pOwner, flDelay );
}

//-----------------------------------------------------------------------------
// Sounds: the script is added for this map, then every line a soldier can say is precached
// (only the configured enemy nation's variants).
//-----------------------------------------------------------------------------
static bool SoundExists( const char *pszName )
{
	return soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( pszName ) );
}

// Once per map, right after the script loads: does every line kind exist for every voice, and is
// every wave file the script names on disk? One console line either way.
// INVARIANT: every line the code can ask for has a sound entry in every voice, and every wave exists.
static void CheckChatterSounds( void )
{
	char szName[128];
	s_nKindsMissing = 0;
	const char *pszFirstKind = NULL;
	for ( int v = 0; v < GERMAN_CHATTER_VOICES; v++ )
	{
		for ( int e = 0; e < NUM_CHAT_EVENTS + (int)ARRAYSIZE( s_pszExtraCategories ); e++ )
		{
			const char *pszCategory = e < NUM_CHAT_EVENTS ? s_ChatDefs[e].pszCategory : s_pszExtraCategories[e - NUM_CHAT_EVENTS];
			if ( e == CHAT_ORDER_COVER )
				continue;		// order_cover_<landmark>: the generic one is in the extras, landmarks fall back to it
			Q_snprintf( szName, sizeof( szName ), "GermanChatter.v%d.%s.%s", s_iVoices[v], pszCategory, s_szNation );
			if ( !SoundExists( szName ) )
			{
				if ( !s_nKindsMissing )
					pszFirstKind = pszCategory;
				s_nKindsMissing++;
			}
		}
	}

	// The waves: GermanChatter entries for this nation (each wave once) and the cries
	s_nEntries = s_nWaves = s_nWavesMissing = 0;
	s_szFirstMissing[0] = 0;
	for ( int i = soundemitterbase->First(); i != soundemitterbase->InvalidIndex(); i = soundemitterbase->Next( i ) )
	{
		const char *pszName = soundemitterbase->GetSoundName( i );
		if ( !pszName )
			continue;
		bool bLine = !Q_strnicmp( pszName, "GermanChatter.", 14 );
		if ( !bLine && Q_strnicmp( pszName, "GermanVoice.", 12 ) )
			continue;
		s_nEntries++;
		if ( bLine )
		{
			const char *pszDot = Q_strrchr( pszName, '.' );
			if ( !pszDot || Q_stricmp( pszDot + 1, s_szNation ) )
				continue;
		}
		CSoundParametersInternal *pParams = soundemitterbase->InternalGetParametersForSound( i );
		if ( !pParams )
			continue;
		const SoundFile *pFiles = pParams->GetSoundNames();
		for ( int w = 0; w < pParams->NumSoundNames(); w++ )
		{
			CUtlSymbol sym = pFiles[w].symbol;
			const char *pszWave = soundemitterbase->GetWaveName( sym );
			if ( !pszWave || !pszWave[0] )
				continue;
			s_nWaves++;
			char szPath[MAX_PATH];
			Q_snprintf( szPath, sizeof( szPath ), "sound/%s", pszWave );
			if ( !filesystem->FileExists( szPath, "GAME" ) )
			{
				if ( !s_nWavesMissing )
					Q_strncpy( s_szFirstMissing, szPath, sizeof( s_szFirstMissing ) );
				s_nWavesMissing++;
			}
		}
	}

	if ( s_nWavesMissing )
		Warning( "INVARIANT German chatter: %d of %d wave files missing (first: %s) - copy the voice set's Battlechatter folder into DOD-SP\\sound\\dodsp\\\n",
				 s_nWavesMissing, s_nWaves, s_szFirstMissing );
	if ( s_nKindsMissing )
		Warning( "INVARIANT German chatter: %d line kinds have no sound entry (first: %s, enemy nation '%s') - %s is old or the nation is wrong\n",
				 s_nKindsMissing, pszFirstKind ? pszFirstKind : "?", s_szNation, CHATTER_SCRIPT );
	if ( !s_nWavesMissing && !s_nKindsMissing )
		Msg( "DOD-SP German chatter: %d voices, %d lines for enemy nation '%s', all wave files found\n", GERMAN_CHATTER_VOICES, s_nWaves, s_szNation );
}

void CGermanChatter::LevelPrecache( void )
{
	// The sound system drops every override script at level end (ClearSoundOverrides), so
	// whether the lines are loaded is asked of the sound system itself, every time - never
	// remembered here. (A remembered "done for this map" broke on reloading the same map.)
	char szTest[128];
	Q_snprintf( szTest, sizeof( szTest ), "GermanChatter.v0.response_ack_yes.%s", s_szNation );
	if ( !SoundExists( szTest ) )
	{
		soundemitterbase->AddSoundOverrides( CHATTER_SCRIPT, true );
		if ( !SoundExists( szTest ) )
		{
			s_nEntries = s_nWaves = s_nWavesMissing = 0;
			static float s_flNextWarn = 0.0f;
			if ( Plat_FloatTime() >= s_flNextWarn )
			{
				s_flNextWarn = Plat_FloatTime() + 30.0f;
				Warning( "German chatter: %s didn't load (no %s) - soldiers will be silent\n", CHATTER_SCRIPT, szTest );
			}
			return;
		}
		CheckChatterSounds();		// once per map: the script was just loaded
		// C2: sentences can use any line kind of the enemy nation (names, floors, landmarks, compass):
		// precache them all, once per map (the override script is dropped at level end, so this runs
		// again on the next map)
		int nAll = 0;
		for ( int i = soundemitterbase->First(); i != soundemitterbase->InvalidIndex(); i = soundemitterbase->Next( i ) )
		{
			const char *pszName = soundemitterbase->GetSoundName( i );
			if ( !pszName || Q_strnicmp( pszName, "GermanChatter.", 14 ) )
				continue;
			const char *pszDot = Q_strrchr( pszName, '.' );
			if ( pszDot && !Q_stricmp( pszDot + 1, s_szNation ) )
			{
				CBaseEntity::PrecacheScriptSound( pszName );
				nAll++;
			}
		}
		if ( npc_german_debug.GetBool() )
			Msg( "npc_german_soldier: battle chatter - %d line kinds precached for sentences\n", nAll );
	}

	int nPrecached = 0;
	char szName[128];
	for ( int v = 0; v < GERMAN_CHATTER_VOICES; v++ )
	{
		for ( int e = 0; e < NUM_CHAT_EVENTS + (int)ARRAYSIZE( s_pszExtraCategories ); e++ )
		{
			if ( e == CHAT_ORDER_COVER )
			{
				for ( int k = 0; k < (int)ARRAYSIZE( s_pszLandmarks ); k++ )
				{
					Q_snprintf( szName, sizeof( szName ), "GermanChatter.v%d.order_cover_%s.%s", s_iVoices[v], s_pszLandmarks[k], s_szNation );
					if ( SoundExists( szName ) )
					{
						CBaseEntity::PrecacheScriptSound( szName );
						nPrecached++;
					}
				}
				continue;
			}
			const char *pszCategory = e < NUM_CHAT_EVENTS ? s_ChatDefs[e].pszCategory : s_pszExtraCategories[e - NUM_CHAT_EVENTS];
			Q_snprintf( szName, sizeof( szName ), "GermanChatter.v%d.%s.%s", s_iVoices[v], pszCategory, s_szNation );
			if ( SoundExists( szName ) )
			{
				CBaseEntity::PrecacheScriptSound( szName );
				nPrecached++;
			}
		}
	}
	for ( int i = 0; i < 3; i++ )
	{
		for ( int kind = 0; kind < 2; kind++ )
		{
			Q_snprintf( szName, sizeof( szName ), "GermanVoice.%c.%s", "mno"[i], kind ? "death" : "pain" );
			if ( SoundExists( szName ) )
			{
				CBaseEntity::PrecacheScriptSound( szName );
				nPrecached++;
			}
		}
	}
	if ( nPrecached == 0 )
		Warning( "npc_german_soldier: no battle chatter - %s missing or empty\n", CHATTER_SCRIPT );
	else if ( npc_german_debug.GetBool() )
		Msg( "npc_german_soldier: battle chatter - %d sound entries precached (enemy nation '%s')\n", nPrecached, s_szNation );
}

//-----------------------------------------------------------------------------
// npc_german_chatter: what the soldiers said this map, and why the rest wasn't said
//-----------------------------------------------------------------------------
void CGermanChatter::Report( void )
{
	CheckNewMap();
	int nSoldiers = 0, nSolo = 0, nVoice[GERMAN_CHATTER_VOICES] = { 0 };
	for ( int i = 0; i < s_AllChatter.Count(); i++ )
	{
		CBaseEntity *pOwner = s_AllChatter[i]->m_hOwner.Get();
		if ( !pOwner || !pOwner->IsAlive() )
			continue;
		nSoldiers++;
		if ( s_AllChatter[i]->m_szSquad[0] == '#' )
			nSolo++;
		nVoice[ VoiceSlot( s_AllChatter[i]->m_iVoice ) ]++;
	}
	int nSaid = 0, nDrop[NUM_CHAT_DROPS] = { 0 };
	for ( int e = 0; e < NUM_CHAT_EVENTS; e++ )
	{
		nSaid += s_nSaid[e];
		for ( int d = 0; d < NUM_CHAT_DROPS; d++ )
			nDrop[d] += s_nDropped[e][d];
	}
	char szLoad[160];
	char szTest[128];
	Q_snprintf( szTest, sizeof( szTest ), "GermanChatter.v0.response_ack_yes.%s", s_szNation );
	if ( !SoundExists( szTest ) )
		Q_snprintf( szLoad, sizeof( szLoad ), "SCRIPT NOT LOADED (%s)", CHATTER_SCRIPT );
	else
		Q_snprintf( szLoad, sizeof( szLoad ), "%d entries, %d/%d waves found", s_nEntries, s_nWaves - s_nWavesMissing, s_nWaves );
	char szDrops[256] = "";
	for ( int d = 0; d < NUM_CHAT_DROPS; d++ )
	{
		if ( !nDrop[d] )
			continue;
		char sz[32];
		Q_snprintf( sz, sizeof( sz ), "%s%s %d", szDrops[0] ? ", " : "", s_pszDropNames[d], nDrop[d] );
		Q_strncat( szDrops, sz, sizeof( szDrops ) );
	}
	Msg( "German chatter: %s, enemy '%s', %s | soldiers %d (voices %d/%d/%d/%d, %d without a squad) | since game start: said %d | not said: %s | every line: DOD-SP\\dodsp_chatter_log.txt\n",
		 s_bEnabled ? "on" : "OFF", s_szNation, szLoad, nSoldiers, nVoice[0], nVoice[1], nVoice[2], nVoice[3], nSolo, nSaid,
		 szDrops[0] ? szDrops : "none" );
	for ( int e = 0; e < NUM_CHAT_EVENTS; e++ )
	{
		int nAny = s_nSaid[e];
		for ( int d = 0; d < NUM_CHAT_DROPS; d++ )
			nAny += s_nDropped[e][d];
		if ( !nAny )
			continue;
		char szLine[256];
		Q_snprintf( szLine, sizeof( szLine ), "  %-26s said %d", s_ChatDefs[e].pszCategory, s_nSaid[e] );
		for ( int d = 0; d < NUM_CHAT_DROPS; d++ )
		{
			if ( !s_nDropped[e][d] )
				continue;
			char sz[32];
			Q_snprintf( sz, sizeof( sz ), ", %s %d", s_pszDropNames[d], s_nDropped[e][d] );
			Q_strncat( szLine, sz, sizeof( szLine ) );
		}
		Msg( "%s\n", szLine );
	}
}

CON_COMMAND( npc_german_chatter, "German battle chatter this map: what was said, what wasn't and why." )
{
	CGermanChatter::Report();
}

//-----------------------------------------------------------------------------
// trigger_german_landmark: a brush volume naming what's there, for the cover callouts
// ("Hinter die Fässer!"). Wins over anything the prop names suggest.
//-----------------------------------------------------------------------------
class CGermanLandmark : public CBaseEntity
{
public:
	DECLARE_CLASS( CGermanLandmark, CBaseEntity );
	DECLARE_DATADESC();

	void Spawn( void )
	{
		SetSolid( SOLID_NONE );
		SetMoveType( MOVETYPE_NONE );
		SetModel( STRING( GetModelName() ) );	// the brush gives the bounds
		AddEffects( EF_NODRAW );
	}
	string_t m_iszLandmark;
};

LINK_ENTITY_TO_CLASS( trigger_german_landmark, CGermanLandmark );

BEGIN_DATADESC( CGermanLandmark )
	DEFINE_KEYFIELD( m_iszLandmark, FIELD_STRING, "landmark" ),
END_DATADESC()

//-----------------------------------------------------------------------------
// Prop model -> landmark. Matched on whole WORDS of the model's file name, not on pieces of
// text: "lights_streetlight01" is not a tree, "trainstation_bench" is not a train. A file name
// is split at _ - . and each word loses its numbering ("woodbarrel001" -> "woodbarrel",
// "riverbed02c" -> "riverbed"). The first rule (in this order) that any word matches decides.
// Tested against every prop in dod_argentan (dodsp_tools/landmark_report.py).
//-----------------------------------------------------------------------------
enum
{
	LM_WORD   = 1,		// the word itself, or its plural ("crate", "crates", "boxes")
	LM_PREFIX = 2,		// a word starting with it ("sandbags_line1" "kubelwagen")
	LM_SUFFIX = 4,		// a word ending with it ("woodbarrel" "farmhouse" "concretewall")
};

struct LandmarkRule_t
{
	const char	*pszWord;
	const char	*pszLandmark;	// "" = known dressing, never named as cover (the generic line)
	int			fMatch;
};

static const LandmarkRule_t s_LandmarkRules[] =
{
	// dressing whose name would otherwise match something below
	{ "phonepole", "", LM_PREFIX },		{ "pole", "", LM_WORD },		{ "crane", "", LM_WORD },
	{ "gunbarrel", "", LM_PREFIX },		{ "chimney", "", LM_PREFIX },	{ "roof", "", LM_PREFIX },
	{ "gutter", "", LM_PREFIX },		{ "light", "", LM_PREFIX },		{ "lamp", "", LM_PREFIX },
	{ "sign", "", LM_PREFIX },

	// building fronts (C2: the second chatter log's storefront3 had no rule)
	{ "storefront", "house", LM_PREFIX },		{ "facade", "house", LM_PREFIX },	{ "shopfront", "house", LM_PREFIX },

	// vehicles (specific before general: "flatbed" before "train", "kubel" before "car")
	{ "boxcar", "boxcar", LM_PREFIX },			{ "flatbed", "flatbed", LM_PREFIX },
	{ "streetcar", "streetcar", LM_PREFIX },	{ "tram", "streetcar", LM_WORD },
	{ "brencarrier", "brencarrier", LM_PREFIX },{ "carrier", "brencarrier", LM_WORD },
	{ "halftrack", "halftrack", LM_PREFIX },	{ "sdkfz", "halftrack", LM_PREFIX },
	{ "kubel", "kubelwagon", LM_PREFIX },		{ "schwimm", "kubelwagon", LM_PREFIX },
	{ "jeep", "car", LM_PREFIX },				{ "222", "car", LM_WORD },		// Sd.Kfz. 222 armoured car
	{ "truck", "truck", LM_PREFIX | LM_SUFFIX },{ "opel", "truck", LM_PREFIX },	{ "lorry", "truck", LM_PREFIX },
	{ "tractor", "tractor", LM_PREFIX },
	{ "haycart", "haycart", LM_PREFIX },		{ "cart", "haycart", LM_WORD },	{ "wagon", "haycart", LM_WORD | LM_SUFFIX },
	{ "crashedplane", "crashedplane", LM_PREFIX }, { "plane", "crashedplane", LM_WORD | LM_SUFFIX },
	{ "flak", "antiairgun", LM_PREFIX },		{ "aagun", "antiairgun", LM_PREFIX },
	{ "locomotive", "train", LM_PREFIX },		{ "train", "train", LM_WORD },

	// things soldiers hide behind
	{ "barrel", "barrels", LM_PREFIX | LM_SUFFIX },	{ "drum", "barrels", LM_WORD | LM_SUFFIX },
	{ "crate", "crates", LM_PREFIX | LM_SUFFIX },	{ "box", "crates", LM_WORD },
	{ "sandbag", "sandbags", LM_PREFIX | LM_SUFFIX },
	{ "tire", "tires", LM_PREFIX | LM_SUFFIX },	{ "tyre", "tires", LM_PREFIX | LM_SUFFIX },
	{ "woodpile", "woodpile", LM_PREFIX },		{ "logpile", "woodpile", LM_PREFIX },
	{ "firewood", "woodpile", LM_PREFIX },		{ "log", "woodpile", LM_WORD },
	{ "pipe", "largepipe", LM_WORD | LM_SUFFIX },
	{ "fountain", "fountain", LM_PREFIX | LM_SUFFIX }, { "statue", "statue", LM_PREFIX },
	{ "well", "well", LM_WORD },
	{ "tree", "tree", LM_PREFIX | LM_SUFFIX },	{ "stump", "tree", LM_WORD },
	{ "rock", "rocks", LM_PREFIX | LM_SUFFIX },	{ "boulder", "rocks", LM_PREFIX },
	{ "brokenwall", "brokenwall", LM_PREFIX },	{ "ruin", "brokenwall", LM_PREFIX },
	{ "rubble", "rubble", LM_PREFIX | LM_SUFFIX },	{ "debris", "rubble", LM_PREFIX },
	{ "destroyed", "rubble", LM_WORD },
	{ "wall", "wall", LM_WORD | LM_SUFFIX },
	{ "bunker", "bunker", LM_PREFIX | LM_SUFFIX },	{ "pillbox", "bunker", LM_PREFIX },
	{ "silo", "silo", LM_PREFIX },
	{ "church", "church", LM_PREFIX | LM_SUFFIX },
	{ "barn", "barn", LM_WORD },				{ "stable", "barn", LM_PREFIX },
	{ "house", "house", LM_WORD | LM_SUFFIX },
	{ "factory", "factory", LM_PREFIX },		{ "bridge", "bridge", LM_WORD },
	{ "trench", "trench", LM_PREFIX },			{ "ditch", "ditch", LM_PREFIX },
	{ "embankment", "embankment", LM_PREFIX },	{ "gully", "gully", LM_PREFIX },
	{ "car", "car", LM_WORD },
};

static bool LandmarkWordMatches( const char *pszWord, int nLen, const LandmarkRule_t &rule )
{
	int nRule = Q_strlen( rule.pszWord );
	if ( nRule > nLen )
		return false;
	if ( rule.fMatch & LM_WORD )
	{
		if ( nLen == nRule && !Q_strnicmp( pszWord, rule.pszWord, nRule ) )
			return true;
		if ( !Q_strnicmp( pszWord, rule.pszWord, nRule ) &&
			 ( ( nLen == nRule + 1 && pszWord[nRule] == 's' ) ||
			   ( nLen == nRule + 2 && pszWord[nRule] == 'e' && pszWord[nRule + 1] == 's' ) ) )
			return true;
	}
	if ( ( rule.fMatch & LM_PREFIX ) && !Q_strnicmp( pszWord, rule.pszWord, nRule ) )
		return true;
	if ( rule.fMatch & LM_SUFFIX )
	{
		if ( !Q_strnicmp( pszWord + nLen - nRule, rule.pszWord, nRule ) )
			return true;
		if ( nLen > nRule && pszWord[nLen - 1] == 's' && !Q_strnicmp( pszWord + nLen - 1 - nRule, rule.pszWord, nRule ) )
			return true;
	}
	return false;
}

// bFound: a rule decided (even if it decided "dressing", which returns NULL)
static const char *LandmarkFromModel( const char *pszName, bool *pbFound = NULL )
{
	if ( pbFound )
		*pbFound = false;
	if ( !pszName || !pszName[0] || pszName[0] == '*' || Q_stristr( pszName, "skybox" ) )
		return NULL;
	const char *pszFile = Q_strrchr( pszName, '/' );		// the model's own name, not its folder
	pszFile = pszFile ? pszFile + 1 : pszName;

	// split into words
	char szWords[8][32];
	int nWords = 0;
	for ( const char *p = pszFile; *p && *p != '.' && nWords < 8; )
	{
		while ( *p && *p != '.' && !V_isalnum( *p ) )
			p++;
		int n = 0;
		while ( V_isalnum( *p ) )
		{
			if ( n < 31 )
				szWords[nWords][n++] = (char)tolower( (unsigned char)*p );
			p++;
		}
		if ( !n )
			break;
		szWords[nWords][n] = 0;
		// drop the numbering: "001a" "02c" "3" (all-digit words like "222" stay)
		int m = n;
		if ( m > 1 && V_isalpha( szWords[nWords][m - 1] ) && V_isdigit( szWords[nWords][m - 2] ) )
			m--;
		while ( m > 0 && V_isdigit( szWords[nWords][m - 1] ) )
			m--;
		if ( m > 0 )
			szWords[nWords][m] = 0;
		nWords++;
	}

	for ( int r = 0; r < (int)ARRAYSIZE( s_LandmarkRules ); r++ )
	{
		for ( int w = 0; w < nWords; w++ )
		{
			if ( LandmarkWordMatches( szWords[w], Q_strlen( szWords[w] ), s_LandmarkRules[r] ) )
			{
				if ( pbFound )
					*pbFound = true;
				return s_LandmarkRules[r].pszLandmark[0] ? s_LandmarkRules[r].pszLandmark : NULL;
			}
		}
	}
	return NULL;
}

// Brushwork has no model name - only its material, and that only says something useful for a
// few things ("nature/argentan_blendrubbledirt", "dod_sandbags01").
static const char *LandmarkFromMaterial( const char *pszMaterial )
{
	static const char *s_pszMaterialWords[][2] =
	{
		{ "rubble", "rubble" }, { "debris", "rubble" }, { "sandbag", "sandbags" }, { "crate", "crates" },
	};
	if ( !pszMaterial || !pszMaterial[0] )
		return NULL;
	for ( int i = 0; i < (int)ARRAYSIZE( s_pszMaterialWords ); i++ )
		if ( Q_stristr( pszMaterial, s_pszMaterialWords[i][0] ) )
			return s_pszMaterialWords[i][1];
	return NULL;
}

const char *GermanChatter_LandmarkAt( const Vector &vecSpot, const Vector &vecThreat, CBaseEntity *pIgnore1, CBaseEntity *pIgnore2,
									  char *pszWhat, int nWhat )
{
	char szScratch[4];
	if ( !pszWhat || nWhat <= 0 )
	{
		pszWhat = szScratch;
		nWhat = sizeof( szScratch );
	}
	pszWhat[0] = 0;
	// 1. A mapper's volume.
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "trigger_german_landmark" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "trigger_german_landmark" ) )
	{
		CGermanLandmark *pLandmark = static_cast<CGermanLandmark *>( pEnt );
		if ( pLandmark->m_iszLandmark != NULL_STRING && pEnt->CollisionProp()->IsPointInBounds( vecSpot + Vector( 0, 0, 8 ) ) )
		{
			Q_snprintf( pszWhat, nWhat, "trigger_german_landmark" );
			return STRING( pLandmark->m_iszLandmark );
		}
	}

	// 2. What's in the way toward the threat, at hiding height.
	Vector vecFrom = vecSpot + Vector( 0, 0, 24 );
	Vector vecDir = vecThreat - vecFrom;
	vecDir.z = 0.0f;
	if ( VectorNormalize( vecDir ) < 1.0f )
	{
		Q_snprintf( pszWhat, nWhat, "the enemy is right above or below him" );
		return NULL;
	}
	CTraceFilterSkipTwoEntities filter( pIgnore1, pIgnore2, COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( vecFrom, vecFrom + vecDir * 160.0f, MASK_SOLID, &filter, &tr );
	if ( tr.fraction >= 1.0f )
	{
		Q_snprintf( pszWhat, nWhat, "nothing solid within 160 units toward the enemy at hip height" );
		return NULL;
	}

	bool bBrush = true;
	if ( tr.m_pEnt && !tr.m_pEnt->IsWorld() )
	{
		const char *pszModel = STRING( tr.m_pEnt->GetModelName() );
		if ( pszModel && pszModel[0] && pszModel[0] != '*' )
		{
			// a prop entity: its model decides
			const char *psz = LandmarkFromModel( pszModel );
			Q_snprintf( pszWhat, nWhat, "%s %s%s", tr.m_pEnt->GetClassname(), pszModel, psz ? "" : " - no name rule matches" );
			return psz;
		}
		// a brush entity (func_brush, func_breakable...): treat as brushwork below
	}
	else
	{
		// a static prop there? Its model decides, even when that's "nothing known" (a hedge is
		// not a low wall).
		CUtlVector<ICollideable *> props;
		Vector vecPad( 12, 12, 12 );
		staticpropmgr->GetAllStaticPropsInAABB( tr.endpos - vecPad, tr.endpos + vecPad, &props );
		bool bAnyProp = false;
		for ( int i = 0; i < props.Count(); i++ )
		{
			const model_t *pModel = props[i] ? props[i]->GetCollisionModel() : NULL;
			const char *pszModel = pModel ? modelinfo->GetModelName( pModel ) : NULL;
			if ( !pszModel || Q_stristr( pszModel, "skybox" ) )
				continue;
			if ( !bAnyProp )
				Q_snprintf( pszWhat, nWhat, "static prop %s - no name rule matches", pszModel );
			bAnyProp = true;
			bool bFound;
			const char *psz = LandmarkFromModel( pszModel, &bFound );
			if ( psz )
				return psz;
		}
		if ( bAnyProp )
			bBrush = false;
	}
	if ( !bBrush )
		return NULL;

	// 3. Brushwork / displacement.
	const char *pszMaterial = LandmarkFromMaterial( tr.surface.name );
	Q_snprintf( pszWhat, nWhat, "brushwork %s", tr.surface.name ? tr.surface.name : "?" );
	if ( tr.IsDispSurface() && tr.plane.normal.z > 0.2f )
		return pszMaterial ? pszMaterial : "embankment";	// ground rising toward the enemy

	// Low enough to shoot over (clear at standing chest height, a little past it) = a wall.
	// Taller brushwork is usually a building: unnamed unless a trigger_german_landmark says.
	trace_t trHigh;
	Vector vecHigh = vecSpot + Vector( 0, 0, 64 );
	float flDist = ( tr.endpos - vecFrom ).Length2D();
	UTIL_TraceLine( vecHigh, vecHigh + vecDir * ( flDist + 48.0f ), MASK_SOLID, &filter, &trHigh );
	if ( trHigh.fraction >= 1.0f )
		return pszMaterial ? pszMaterial : "wall";
	if ( !pszMaterial )
		Q_snprintf( pszWhat, nWhat, "tall brushwork %s - a building or a wall too high to shoot over", tr.surface.name ? tr.surface.name : "?" );
	return pszMaterial;
}

// Aim at something and see what a soldier crouched where you stand would call it.
CON_COMMAND( npc_german_landmark, "What a German soldier at your position would call the cover in front of you." )
{
	CBasePlayer *pPlayer = UTIL_GetCommandClient();
	if ( !pPlayer )
		return;
	Vector vecForward;
	pPlayer->EyeVectors( &vecForward );
	Vector vecSpot = pPlayer->GetAbsOrigin();
	const char *psz = GermanChatter_LandmarkAt( vecSpot, vecSpot + vecForward * 1000.0f, pPlayer, NULL );
	Msg( "landmark: %s\n", psz ? psz : "(generic)" );
}

//=============================================================================
// C2 - sentences: where the enemy is, and what a soldier says in one breath
//=============================================================================
static bool LineExists( int iVoice, const char *pszCategory )
{
	char sz[128];
	Q_snprintf( sz, sizeof( sz ), "GermanChatter.v%d.%s.%s", iVoice, pszCategory, s_szNation );
	return SoundExists( sz );
}

static void SetPart( ChatPart_t &part, const char *pszCategory, const char *pszMeaning, int eCheck )
{
	Q_strncpy( part.szCategory, pszCategory, sizeof( part.szCategory ) );
	Q_strncpy( part.szMeaning, pszMeaning, sizeof( part.szMeaning ) );
	part.eCheck = eCheck;
}

// Under a roof: solid (not sky) within 400 units straight up
static bool UnderRoof( const Vector &vecFrom, CBaseEntity *pIgnore )
{
	trace_t tr;
	UTIL_TraceLine( vecFrom, vecFrom + Vector( 0, 0, 400 ), MASK_SOLID_BRUSHONLY, pIgnore, COLLISION_GROUP_NONE, &tr );
	return tr.fraction < 1.0f && !( tr.surface.flags & SURF_SKY );
}

// The voice set's word for one of our landmark names (landmark_behind_/landmark_near_ spell a few differently)
static const char *LandmarkSetWord( const char *pszLandmark )
{
	if ( !Q_stricmp( pszLandmark, "tree" ) )
		return "trees";
	if ( !Q_stricmp( pszLandmark, "kubelwagon" ) )
		return "kubelwagen";
	return pszLandmark;
}

// The nearest recognised thing within 200 units of vecPos: a mapper's volume, a static prop, a prop entity
static const char *LandmarkNear( const Vector &vecPos, CBaseEntity *pIgnore )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "trigger_german_landmark" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "trigger_german_landmark" ) )
	{
		CGermanLandmark *pLandmark = static_cast<CGermanLandmark *>( pEnt );
		if ( pLandmark->m_iszLandmark != NULL_STRING && pEnt->CollisionProp()->IsPointInBounds( vecPos + Vector( 0, 0, 8 ) ) )
			return STRING( pLandmark->m_iszLandmark );
	}
	const float flRange = 200.0f;
	const char *pszBest = NULL;
	float flBest = flRange * flRange;
	CUtlVector<ICollideable *> props;
	staticpropmgr->GetAllStaticPropsInAABB( vecPos - Vector( flRange, flRange, 64 ), vecPos + Vector( flRange, flRange, 128 ), &props );
	for ( int i = 0; i < props.Count(); i++ )
	{
		const model_t *pModel = props[i] ? props[i]->GetCollisionModel() : NULL;
		const char *pszModel = pModel ? modelinfo->GetModelName( pModel ) : NULL;
		const char *psz = pszModel ? LandmarkFromModel( pszModel ) : NULL;
		float flDist = psz ? ( props[i]->GetCollisionOrigin() - vecPos ).Length2DSqr() : FLT_MAX;
		if ( psz && flDist < flBest )
		{
			flBest = flDist;
			pszBest = psz;
		}
	}
	CBaseEntity *apList[64];
	int nEnts = UTIL_EntitiesInSphere( apList, ARRAYSIZE( apList ), vecPos, flRange, 0 );
	for ( int i = 0; i < nEnts; i++ )
	{
		CBaseEntity *pEnt = apList[i];
		if ( !pEnt || pEnt == pIgnore || pEnt->IsPlayer() || pEnt->MyNPCPointer() )
			continue;
		const char *pszModel = STRING( pEnt->GetModelName() );
		if ( !pszModel || !pszModel[0] || pszModel[0] == '*' )
			continue;
		const char *psz = LandmarkFromModel( pszModel );
		float flDist = psz ? ( pEnt->WorldSpaceCenter() - vecPos ).Length2DSqr() : FLT_MAX;
		if ( psz && flDist < flBest )
		{
			flBest = flDist;
			pszBest = psz;
		}
	}
	return pszBest;
}

// Where the enemy is, as the speaker sees him - one part, the first of these that fits:
//  1. a building: in it (under a roof) seen from outside or from another level -> its floor and the opening
//     (a window if there's wall below him toward us, a doorway if not); high up in the open -> a balcony
//     (a wall right behind him) or the roof
//  2. behind something we have a word for ("behind the barrels") - the cover between him and us
//  3. near something we have a word for, within 200 units
//  4. the compass
// *pbWindow: at a window (the full call then ends "fire at the window!").
static bool EnemyLocation( int iVoice, CBaseEntity *pSpeaker, CBaseEntity *pEnemy, ChatPart_t &part, bool *pbWindow )
{
	*pbWindow = false;
	Vector vecFeet = pEnemy->GetAbsOrigin();
	Vector vecEye = pEnemy->EyePosition();
	Vector vecSpk = pSpeaker->EyePosition();
	float flDZ = vecFeet.z - pSpeaker->GetAbsOrigin().z;
	Vector vecToSpk = vecSpk - vecEye;
	vecToSpk.z = 0.0f;
	if ( VectorNormalize( vecToSpk ) < 1.0f )
		vecToSpk = Vector( 1, 0, 0 );

	// 1. a building
	bool bEnemyRoof = UnderRoof( vecEye, pEnemy );
	bool bSpeakerRoof = UnderRoof( vecSpk, pSpeaker );
	trace_t trGround;
	UTIL_TraceLine( vecFeet + Vector( 0, 0, 8 ), vecFeet - Vector( 0, 0, 32 ), MASK_SOLID_BRUSHONLY, pEnemy, COLLISION_GROUP_NONE, &trGround );
	bool bOnTerrain = trGround.fraction < 1.0f && trGround.IsDispSurface();
	if ( ( bEnemyRoof && ( !bSpeakerRoof || fabs( flDZ ) > 96.0f ) ) || ( !bEnemyRoof && !bOnTerrain && flDZ > 96.0f ) )
	{
		int iFloor = flDZ < -64.0f ? FLOOR_BASEMENT : flDZ < 80.0f ? FLOOR_GROUND : flDZ < 210.0f ? FLOOR_SECOND : flDZ < 340.0f ? FLOOR_THIRD : FLOOR_TOP;
		int iKind = LOCKIND_GENERIC;
		const char *pszCategory = NULL;
		trace_t tr;
		if ( !bEnemyRoof )
		{
			UTIL_TraceLine( vecEye, vecEye - vecToSpk * 72.0f, MASK_SOLID_BRUSHONLY, pEnemy, COLLISION_GROUP_NONE, &tr );
			if ( tr.fraction < 1.0f && s_pszLocations[iFloor][LOCKIND_BALCONY] )
			{
				iKind = LOCKIND_BALCONY;
				pszCategory = s_pszLocations[iFloor][iKind];
			}
			else if ( LineExists( iVoice, s_pszRoof ) )
			{
				SetPart( part, s_pszRoof, "on the roof!", PARTCHECK_ENEMY );
				return true;
			}
		}
		else
		{
			Vector vecKnee = vecFeet + Vector( 0, 0, 16 );
			UTIL_TraceLine( vecKnee, vecKnee + vecToSpk * 64.0f, MASK_SOLID_BRUSHONLY, pEnemy, COLLISION_GROUP_NONE, &tr );
			iKind = tr.fraction < 1.0f ? LOCKIND_WINDOW : LOCKIND_DOORWAY;
			pszCategory = s_pszLocations[iFloor][iKind];
		}
		if ( !pszCategory || !LineExists( iVoice, pszCategory ) )
		{
			iKind = LOCKIND_GENERIC;
			pszCategory = s_pszLocations[iFloor][LOCKIND_GENERIC];
		}
		if ( pszCategory && LineExists( iVoice, pszCategory ) )
		{
			char szMeaning[64];
			Q_snprintf( szMeaning, sizeof( szMeaning ), "%s%s!", s_pszFloorWords[iFloor], s_pszKindWords[iKind] );
			SetPart( part, pszCategory, szMeaning, PARTCHECK_ENEMY );
			*pbWindow = ( iKind == LOCKIND_WINDOW );
			return true;
		}
	}

	// 2. behind something, 3. near something
	char szCategory[64], szMeaning[64];
	const char *pszBehind = GermanChatter_LandmarkAt( vecFeet, vecSpk, pEnemy, pSpeaker );
	if ( pszBehind )
	{
		Q_snprintf( szCategory, sizeof( szCategory ), "landmark_behind_%s", LandmarkSetWord( pszBehind ) );
		if ( LineExists( iVoice, szCategory ) )
		{
			Q_snprintf( szMeaning, sizeof( szMeaning ), "behind the %s!", pszBehind );
			SetPart( part, szCategory, szMeaning, PARTCHECK_ENEMY );
			return true;
		}
	}
	const char *pszNear = LandmarkNear( vecFeet, pEnemy );
	if ( pszNear )
	{
		Q_snprintf( szCategory, sizeof( szCategory ), "landmark_near_%s", LandmarkSetWord( pszNear ) );
		if ( LineExists( iVoice, szCategory ) )
		{
			Q_snprintf( szMeaning, sizeof( szMeaning ), "near the %s!", pszNear );
			SetPart( part, szCategory, szMeaning, PARTCHECK_ENEMY );
			return true;
		}
	}

	// 4. the compass
	Vector vecDir = vecFeet - pSpeaker->GetAbsOrigin();
	float flYaw = RAD2DEG( atan2f( vecDir.y, vecDir.x ) );
	int iSector = ( (int)floorf( ( flYaw + 22.5f ) / 45.0f ) % 8 + 8 ) % 8;
	if ( LineExists( iVoice, s_pszCompass[iSector] ) )
	{
		Q_snprintf( szMeaning, sizeof( szMeaning ), "to the %s!", s_pszCompassWords[iSector] );
		SetPart( part, s_pszCompass[iSector], szMeaning, PARTCHECK_ENEMY );
		return true;
	}
	return false;
}

int CGermanChatter::BuildSentence( const Pending_t &p, ChatPart_t *pParts, bool &bNamed )
{
	bNamed = false;
	int n = 0;
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !pOwner )
		return 0;
	CBaseEntity *pTarget = p.hTarget.Get();
	CGermanChatter *pTo = ( pTarget && pTarget != pOwner ) ? ForEntity( pTarget ) : NULL;
	bool bToNamed = pTo && pTo->m_iName >= 0 && pTo->m_iName < (int)ARRAYSIZE( s_pszNames ) && LineExists( m_iVoice, s_pszNames[pTo->m_iName] );
	const ChatDef_t &def = s_ChatDefs[p.eEvent];
	char szMeaning[64];

	switch ( p.eEvent )
	{
	case CHAT_THREAT:
	case CHAT_THREAT_EXPOSED:
	{
		// THREAT + LOCATION; the full call (NAME + THREAT + LOCATION + ORDER) on first contact or when the
		// enemy is somewhere new (300 units from the last full call, or 20 s since)
		CAI_BaseNPC *pNPC = pOwner->MyNPCPointer();
		CBaseEntity *pEnemy = pNPC ? pNPC->GetEnemy() : NULL;
		ChatPart_t loc;
		memset( &loc, 0, sizeof( loc ) );
		bool bWindow = false;
		bool bLoc = pEnemy && EnemyLocation( m_iVoice, pOwner, pEnemy, loc, &bWindow );
		SquadChat_t &sq = GetSquadChat( m_szSquad );
		float now = gpGlobals->curtime;
		bool bFresh = pEnemy && ( sq.flLastFull <= 0.0f || now - sq.flLastFull > 20.0f || ( pEnemy->GetAbsOrigin() - sq.vecLastFull ).Length() > 300.0f );
		bool bFull = bFresh && bLoc && bToNamed;
		if ( bFresh && bLoc )
		{
			sq.vecLastFull = pEnemy->GetAbsOrigin();
			sq.flLastFull = now;
		}
		if ( bFull )
		{
			Q_snprintf( szMeaning, sizeof( szMeaning ), "%s!", pTo->GetName() );
			SetPart( pParts[n++], s_pszNames[pTo->m_iName], szMeaning, PARTCHECK_TARGET );
			bNamed = true;
		}
		if ( !LineExists( m_iVoice, def.pszCategory ) )
			return 0;
		SetPart( pParts[n++], def.pszCategory, def.pszMeaning, PARTCHECK_NONE );
		if ( bLoc )
			pParts[n++] = loc;
		if ( bFull )
		{
			const char *pszOrder = bWindow && LineExists( m_iVoice, s_pszAttackWindow ) ? s_pszAttackWindow : "order_attack_infantry";
			SetPart( pParts[n++], pszOrder, bWindow ? "fire at the window!" : "attack!", PARTCHECK_TARGET );
		}
		return n;
	}

	case CHAT_ORDER_COVER:
	case CHAT_ORDER_FORWARD:
	case CHAT_ORDER_FLANK:
	case CHAT_ORDER_FLANK_LEFT:
	case CHAT_ORDER_FLANK_RIGHT:
	case CHAT_ORDER_SUPPRESS:
	case CHAT_ORDER_BACK:
	{
		// NAME + ORDER (three times in four: a squad doesn't name every order)
		char szCategory[64];
		if ( p.eEvent == CHAT_ORDER_COVER )
		{
			Q_snprintf( szCategory, sizeof( szCategory ), "order_cover_%s", p.szModifier[0] ? p.szModifier : "generic" );
			if ( p.szModifier[0] && LineExists( m_iVoice, szCategory ) )
				Q_snprintf( szMeaning, sizeof( szMeaning ), "take cover - behind the %s!", p.szModifier );
			else
			{
				Q_strncpy( szCategory, "order_cover_generic", sizeof( szCategory ) );
				Q_strncpy( szMeaning, "take cover!", sizeof( szMeaning ) );
			}
		}
		else
		{
			Q_strncpy( szCategory, def.pszCategory, sizeof( szCategory ) );
			Q_strncpy( szMeaning, def.pszMeaning, sizeof( szMeaning ) );
		}
		if ( !LineExists( m_iVoice, szCategory ) )
			return 0;
		if ( bToNamed && random->RandomFloat( 0.0f, 1.0f ) < 0.75f )
		{
			char szName[32];
			Q_snprintf( szName, sizeof( szName ), "%s!", pTo->GetName() );
			SetPart( pParts[n++], s_pszNames[pTo->m_iName], szName, PARTCHECK_TARGET );
			bNamed = true;
		}
		SetPart( pParts[n++], szCategory, szMeaning, bNamed ? PARTCHECK_TARGET : PARTCHECK_NONE );
		return n;
	}

	default:
	{
		const char *pszCategory = def.pszCategory;
		if ( p.eEvent == CHAT_ACK_YES && random->RandomInt( 0, 1 ) && LineExists( m_iVoice, "response_ack_norank_yes" ) )
			pszCategory = "response_ack_norank_yes";
		if ( !LineExists( m_iVoice, pszCategory ) )
			return 0;
		SetPart( pParts[n++], pszCategory, def.pszMeaning, PARTCHECK_NONE );
		break;
	}
	}
	return n;
}

// Stand somewhere and hear (well, read) how the nearest German soldier would say where you are.
CON_COMMAND( npc_german_callout, "Where the nearest German soldier would say you are (the location part of his threat call)." )
{
	CBasePlayer *pPlayer = UTIL_GetCommandClient();
	if ( !pPlayer )
		return;
	CBaseEntity *pBest = NULL;
	float flBest = FLT_MAX;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "npc_german_soldier" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "npc_german_soldier" ) )
	{
		float flDist = ( pEnt->GetAbsOrigin() - pPlayer->GetAbsOrigin() ).LengthSqr();
		if ( pEnt->IsAlive() && flDist < flBest )
		{
			flBest = flDist;
			pBest = pEnt;
		}
	}
	CGermanChatter *pChatter = CGermanChatter::ForEntity( pBest );
	if ( !pChatter )
	{
		Msg( "npc_german_callout: no live German soldier on the map\n" );
		return;
	}
	ChatPart_t part;
	memset( &part, 0, sizeof( part ) );
	bool bWindow = false;
	if ( EnemyLocation( pChatter->GetVoice(), pBest, pPlayer, part, &bWindow ) )
		Msg( "npc_german_callout: soldier %d %s, %.0f units away, would say: %s \"%s\"%s\n", pBest->entindex(), pChatter->GetName(),
			 sqrtf( flBest ), part.szCategory, part.szMeaning, bWindow ? " (then: fire at the window!)" : "" );
	else
		Msg( "npc_german_callout: soldier %d %s has no words for where you are\n", pBest->entindex(), pChatter->GetName() );
}
