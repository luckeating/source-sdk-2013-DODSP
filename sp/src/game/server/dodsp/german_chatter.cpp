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
// how long a request stays worth saying while he waits for his turn (s)
static const float s_flValid[NUM_GROUPS]     = {  1.0f, 3.0f, 1.0f, 2.0f, 2.0f };

struct ChatDef_t
{
	const char	*pszCategory;		// sound category (ORDER_COVER appends _<landmark>)
	ChatGroup_t	eGroup;
	float		flPriority;
};

static const ChatDef_t s_ChatDefs[NUM_CHAT_EVENTS] =
{
	{ "threat_infantry_generic",	GROUP_THREAT,	0.5f },	// CHAT_THREAT
	{ "threat_infantry_exposed",	GROUP_THREAT,	0.5f },	// CHAT_THREAT_EXPOSED
	{ "order_attack_infantry",		GROUP_ORDER,	0.6f },	// CHAT_ATTACK
	{ "order_cover",				GROUP_ORDER,	0.9f },	// CHAT_ORDER_COVER
	{ "order_move_forward",			GROUP_ORDER,	0.9f },	// CHAT_ORDER_FORWARD
	{ "order_move_flank",			GROUP_ORDER,	0.9f },	// CHAT_ORDER_FLANK
	{ "order_move_flankleft",		GROUP_ORDER,	0.9f },	// CHAT_ORDER_FLANK_LEFT
	{ "order_move_flankright",		GROUP_ORDER,	0.9f },	// CHAT_ORDER_FLANK_RIGHT
	{ "order_action_suppress",		GROUP_ORDER,	0.5f },	// CHAT_ORDER_SUPPRESS
	{ "inform_reloading_generic",	GROUP_INFORM,	0.2f },	// CHAT_RELOADING
	{ "inform_reloading_coverme",	GROUP_INFORM,	0.3f },	// CHAT_RELOADING_COVERME
	{ "inform_incoming_grenade",	GROUP_INFORM,	0.95f },// CHAT_INCOMING_GRENADE
	{ "reaction_casualty_generic",	GROUP_REACTION,	0.5f },	// CHAT_CASUALTY
	{ "response_ack_yes",			GROUP_RESPONSE,	0.9f },	// CHAT_ACK_YES
	{ "response_ack_covering",		GROUP_RESPONSE,	0.9f },	// CHAT_ACK_COVERING
};

static const int s_iVoices[] = { 0, 2, 3 };

// Landmark names the voice set has lines for (order_cover_<name>)
static const char *s_pszLandmarks[] =
{
	"antiairgun", "barn", "barrels", "boxcar", "brencarrier", "bridge", "brokenwall", "bunker", "car", "church",
	"crashedplane", "crates", "ditch", "embankment", "factory", "flatbed", "fountain", "gully", "halftrack", "haycart",
	"house", "kubelwagon", "largepipe", "rocks", "rubble", "sandbags", "silo", "statue", "streetcar", "tires",
	"tractor", "train", "tree", "trench", "truck", "wall", "well", "woodpile",
};

//-----------------------------------------------------------------------------
// Tuning (from scripts/npc_german_soldier.txt, set by the soldier)
//-----------------------------------------------------------------------------
static bool		s_bEnabled = true;
static char		s_cCrySpeaker[3] = { 'n', 'm', 'o' };	// pain/death speaker for chatter voices 0, 2, 3 (matched by pitch rank)
static char		s_szNation[8] = "us";
static float	s_flFrequency = 1.0f;

void CGermanChatter::SetTuning( bool bEnabled, const char *pszNation, float flFrequency, const char *pszCrySpeakers )
{
	// "n m o": the pain/death speaker (m/n/o) for chatter voices 0, 2 and 3
	if ( pszCrySpeakers )
	{
		int k = 0;
		for ( const char *c = pszCrySpeakers; *c && k < 3; c++ )
			if ( *c == 'm' || *c == 'n' || *c == 'o' )
				s_cCrySpeaker[k++] = *c;
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
};
static CUtlDict<SquadChat_t, int>	s_Squads;
static float						s_flSquadsTime = 0.0f;

static SquadChat_t &GetSquadChat( const char *pszSquad )
{
	if ( gpGlobals->curtime < s_flSquadsTime - 1.0f )
		s_Squads.RemoveAll();		// new map
	s_flSquadsTime = gpGlobals->curtime;
	int i = s_Squads.Find( pszSquad );
	if ( i == s_Squads.InvalidIndex() )
	{
		SquadChat_t sq;
		memset( &sq, 0, sizeof( sq ) );
		sq.nVoices = abs( (int)HashString( pszSquad ) ) % 3;	// squads start their rotation on different voices
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
	m_iVoice = s_iVoices[ ( sq.nVoices++ ) % 3 ];
}

bool CGermanChatter::IsSpeaking( void ) const
{
	return gpGlobals->curtime < m_flSpeakingUntil;
}

void CGermanChatter::OnDeath( void )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( pOwner && IsSpeaking() )
		pOwner->StopSound( pOwner->entindex(), CHAN_VOICE, "" );
	m_Pending.bActive = false;
	// let the squad talk again straight away
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	if ( sq.flSpeakingUntil > gpGlobals->curtime && m_flSpeakingUntil >= sq.flSpeakingUntil )
		sq.flSpeakingUntil = gpGlobals->curtime;
	m_flSpeakingUntil = 0.0f;
}

static int VoiceSlot( int iVoice )
{
	for ( int i = 0; i < 3; i++ )
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
	m_Pending.bActive = false;		// a man who's just been hit doesn't finish his sentence
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

void CGermanChatter::Request( ChatEvent_t eEvent, const char *pszModifier, CBaseEntity *pTarget, float flDelay )
{
	CBaseEntity *pOwner = m_hOwner.Get();
	if ( !s_bEnabled || !pOwner || !pOwner->IsAlive() || eEvent < 0 || eEvent >= NUM_CHAT_EVENTS )
		return;
	const ChatDef_t &def = s_ChatDefs[eEvent];
	float now = gpGlobals->curtime;
	SquadChat_t &sq = GetSquadChat( m_szSquad );

	// Said too recently - by him, by his squad, or (casualty, reloading) this exact kind of line.
	if ( now < m_flNextGroup[def.eGroup] || now < sq.flNextGroup[def.eGroup] )
		return;
	if ( eEvent == CHAT_CASUALTY && now < sq.flNextCasualty )
		return;
	if ( ( eEvent == CHAT_RELOADING || eEvent == CHAT_RELOADING_COVERME ) && now < sq.flNextReload )
		return;
	// Something more important is already waiting to be said.
	if ( m_Pending.bActive && now < m_Pending.flExpire && m_Pending.flPriority > def.flPriority )
		return;

	m_Pending.bActive = true;
	m_Pending.eEvent = eEvent;
	Q_strncpy( m_Pending.szModifier, pszModifier ? pszModifier : "", sizeof( m_Pending.szModifier ) );
	m_Pending.hTarget = pTarget;
	m_Pending.flNotBefore = now + flDelay;
	m_Pending.flExpire = now + flDelay + s_flValid[def.eGroup];
	m_Pending.flPriority = def.flPriority;
}

bool CGermanChatter::BuildSoundName( const Pending_t &p, char *pszOut, int nOut )
{
	const ChatDef_t &def = s_ChatDefs[p.eEvent];
	char szCategory[64];
	if ( p.eEvent == CHAT_ORDER_COVER )
		Q_snprintf( szCategory, sizeof( szCategory ), "order_cover_%s", p.szModifier[0] ? p.szModifier : "generic" );
	else if ( p.eEvent == CHAT_ACK_YES && random->RandomInt( 0, 1 ) )
		Q_strncpy( szCategory, "response_ack_norank_yes", sizeof( szCategory ) );
	else
		Q_strncpy( szCategory, def.pszCategory, sizeof( szCategory ) );

	Q_snprintf( pszOut, nOut, "GermanChatter.v%d.%s.%s", m_iVoice, szCategory, s_szNation );
	if ( soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( pszOut ) ) )
		return true;
	if ( p.eEvent == CHAT_ORDER_COVER )
	{
		// no line for that landmark in this voice: the generic "take cover"
		Q_snprintf( pszOut, nOut, "GermanChatter.v%d.order_cover_generic.%s", m_iVoice, s_szNation );
		return soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( pszOut ) );
	}
	return false;
}

void CGermanChatter::Update( void )
{
	if ( !m_Pending.bActive )
		return;
	CBaseAnimating *pOwner = dynamic_cast<CBaseAnimating *>( m_hOwner.Get() );
	float now = gpGlobals->curtime;
	if ( !pOwner || !pOwner->IsAlive() || now > m_Pending.flExpire )
	{
		m_Pending.bActive = false;	// stale: the moment passed
		return;
	}
	if ( now < m_Pending.flNotBefore )
		return;
	SquadChat_t &sq = GetSquadChat( m_szSquad );
	if ( now < sq.flSpeakingUntil )
		return;		// someone in his squad is talking: wait his turn

	Pending_t p = m_Pending;
	m_Pending.bActive = false;
	char szSound[128];
	if ( !BuildSoundName( p, szSound, sizeof( szSound ) ) )
	{
		if ( npc_german_debug.GetBool() )
			Msg( "[%.2f] npc_german_soldier %d: chatter - no sound %s (is %s loaded?)\n", now, pOwner->entindex(), szSound, CHATTER_SCRIPT );
		return;
	}

	float flDuration = 0.0f;
	pOwner->EmitSound( szSound, 0.0f, &flDuration );
	if ( flDuration <= 0.0f )
		flDuration = 1.5f;
	const ChatDef_t &def = s_ChatDefs[p.eEvent];
	m_flSpeakingUntil = sq.flSpeakingUntil = now + flDuration + 0.2f;
	m_flNextGroup[def.eGroup] = now + s_flSelfWait[def.eGroup] / s_flFrequency;
	sq.flNextGroup[def.eGroup] = now + s_flSquadWait[def.eGroup] / s_flFrequency;
	if ( p.eEvent == CHAT_CASUALTY )
		sq.flNextCasualty = now + 20.0f / s_flFrequency;
	if ( p.eEvent == CHAT_RELOADING || p.eEvent == CHAT_RELOADING_COVERME )
		sq.flNextReload = now + 20.0f / s_flFrequency;

	CBaseEntity *pTarget = p.hTarget.Get();
	if ( npc_german_debug.GetBool() )
		Msg( "[%.2f] npc_german_soldier %d (voice %d) says: %s%s%s\n", now, pOwner->entindex(), m_iVoice, szSound,
			 pTarget ? " -> to " : "", pTarget ? pTarget->GetDebugName() : "" );

	// The man an order was given to answers it.
	CGermanChatter *pAnswer = ForEntity( pTarget );
	if ( pAnswer && pAnswer != this )
	{
		if ( p.eEvent == CHAT_ORDER_COVER || p.eEvent == CHAT_ORDER_FORWARD || p.eEvent == CHAT_ORDER_FLANK ||
			 p.eEvent == CHAT_ORDER_FLANK_LEFT || p.eEvent == CHAT_ORDER_FLANK_RIGHT || p.eEvent == CHAT_ORDER_SUPPRESS )
			pAnswer->Request( CHAT_ACK_YES, NULL, pOwner, flDuration + 0.3f );
		else if ( p.eEvent == CHAT_RELOADING_COVERME )
			pAnswer->Request( CHAT_ACK_COVERING, NULL, pOwner, flDuration + 0.3f );
	}
}

//-----------------------------------------------------------------------------
// Sounds: the script is added for this map, then every line a soldier can say is precached
// (only the configured enemy nation's variants).
//-----------------------------------------------------------------------------
void CGermanChatter::LevelPrecache( void )
{
	static string_t s_iszPrecachedMap = NULL_STRING;
	static float s_flPrecachedAt = -1.0f;
	if ( s_iszPrecachedMap == gpGlobals->mapname && gpGlobals->curtime >= s_flPrecachedAt )
		return;
	s_iszPrecachedMap = gpGlobals->mapname;
	s_flPrecachedAt = gpGlobals->curtime;

	char szTest[128];
	Q_snprintf( szTest, sizeof( szTest ), "GermanChatter.v0.response_ack_yes.%s", s_szNation );
	if ( !soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( szTest ) ) )
		soundemitterbase->AddSoundOverrides( CHATTER_SCRIPT, true );

	int nPrecached = 0;
	char szName[128];
	for ( int v = 0; v < 3; v++ )
	{
		for ( int e = 0; e < NUM_CHAT_EVENTS; e++ )
		{
			if ( e == CHAT_ORDER_COVER )
			{
				for ( int k = -1; k < (int)ARRAYSIZE( s_pszLandmarks ); k++ )
				{
					Q_snprintf( szName, sizeof( szName ), "GermanChatter.v%d.order_cover_%s.%s", s_iVoices[v], k < 0 ? "generic" : s_pszLandmarks[k], s_szNation );
					if ( soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( szName ) ) )
					{
						CBaseEntity::PrecacheScriptSound( szName );
						nPrecached++;
					}
				}
				continue;
			}
			for ( int pass = 0; pass < 2; pass++ )
			{
				if ( pass == 1 && e != CHAT_ACK_YES )
					break;
				Q_snprintf( szName, sizeof( szName ), "GermanChatter.v%d.%s.%s", s_iVoices[v],
							pass == 1 ? "response_ack_norank_yes" : s_ChatDefs[e].pszCategory, s_szNation );
				if ( soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( szName ) ) )
				{
					CBaseEntity::PrecacheScriptSound( szName );
					nPrecached++;
				}
			}
		}
	}
	for ( int i = 0; i < 3; i++ )
	{
		for ( int kind = 0; kind < 2; kind++ )
		{
			Q_snprintf( szName, sizeof( szName ), "GermanVoice.%c.%s", "mno"[i], kind ? "death" : "pain" );
			if ( soundemitterbase->IsValidIndex( soundemitterbase->GetSoundIndex( szName ) ) )
			{
				CBaseEntity::PrecacheScriptSound( szName );
				nPrecached++;
			}
		}
	}
	if ( nPrecached == 0 )
		Warning( "npc_german_soldier: no battle chatter - %s missing or empty (wave files go in sound/dodsp/chatter/de/<voice>/)\n", CHATTER_SCRIPT );
	else if ( npc_german_debug.GetBool() )
		Msg( "npc_german_soldier: battle chatter - %d sound entries precached (enemy nation '%s')\n", nPrecached, s_szNation );
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

// prop model name pieces -> landmark (specific before general: "boxcar" before "box", "haycart" before "car")
static const char *s_pszKeywords[][2] =
{
	{ "boxcar", "boxcar" }, { "flatbed", "flatbed" }, { "streetcar", "streetcar" }, { "tram", "streetcar" },
	{ "brencarrier", "brencarrier" }, { "bren_carrier", "brencarrier" }, { "carrier", "brencarrier" },
	{ "halftrack", "halftrack" }, { "half_track", "halftrack" }, { "sdkfz", "halftrack" },
	{ "kubel", "kubelwagon" }, { "schwimm", "kubelwagon" }, { "jeep", "car" },
	{ "truck", "truck" }, { "opel", "truck" }, { "lorry", "truck" },
	{ "tractor", "tractor" }, { "haycart", "haycart" }, { "hay", "haycart" }, { "cart", "haycart" }, { "wagon", "haycart" },
	{ "plane", "crashedplane" }, { "wreck", "crashedplane" }, { "flak", "antiairgun" }, { "aagun", "antiairgun" },
	{ "locomotive", "train" }, { "train", "train" },
	{ "barrel", "barrels" }, { "drum", "barrels" }, { "crate", "crates" }, { "box", "crates" },
	{ "sandbag", "sandbags" }, { "tire", "tires" }, { "tyre", "tires" },
	{ "woodpile", "woodpile" }, { "logpile", "woodpile" }, { "firewood", "woodpile" }, { "log", "woodpile" },
	{ "pipe", "largepipe" }, { "fountain", "fountain" }, { "statue", "statue" }, { "well", "well" },
	{ "tree", "tree" }, { "stump", "tree" }, { "rock", "rocks" }, { "boulder", "rocks" },
	{ "brokenwall", "brokenwall" }, { "wall_broken", "brokenwall" }, { "ruin", "brokenwall" },
	{ "rubble", "rubble" }, { "debris", "rubble" }, { "wall", "wall" },
	{ "bunker", "bunker" }, { "silo", "silo" }, { "church", "church" }, { "barn", "barn" }, { "house", "house" },
	{ "factory", "factory" }, { "bridge", "bridge" }, { "trench", "trench" }, { "ditch", "ditch" },
	{ "car", "car" },
};

static const char *LandmarkFromName( const char *pszName )
{
	if ( !pszName || !pszName[0] )
		return NULL;
	const char *pszFile = Q_strrchr( pszName, '/' );		// the model's own name, not its folder
	pszFile = pszFile ? pszFile + 1 : pszName;
	for ( int i = 0; i < (int)ARRAYSIZE( s_pszKeywords ); i++ )
		if ( Q_stristr( pszFile, s_pszKeywords[i][0] ) )
			return s_pszKeywords[i][1];
	return NULL;
}

const char *GermanChatter_LandmarkAt( const Vector &vecSpot, const Vector &vecThreat, CBaseEntity *pIgnore1, CBaseEntity *pIgnore2 )
{
	// 1. A mapper's volume.
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "trigger_german_landmark" ); pEnt;
		  pEnt = gEntList.FindEntityByClassname( pEnt, "trigger_german_landmark" ) )
	{
		CGermanLandmark *pLandmark = static_cast<CGermanLandmark *>( pEnt );
		if ( pLandmark->m_iszLandmark != NULL_STRING && pEnt->CollisionProp()->IsPointInBounds( vecSpot + Vector( 0, 0, 8 ) ) )
			return STRING( pLandmark->m_iszLandmark );
	}

	// 2. What's in the way toward the threat, at hiding height.
	Vector vecFrom = vecSpot + Vector( 0, 0, 24 );
	Vector vecDir = vecThreat - vecFrom;
	vecDir.z = 0.0f;
	if ( VectorNormalize( vecDir ) < 1.0f )
		return NULL;
	CTraceFilterSkipTwoEntities filter( pIgnore1, pIgnore2, COLLISION_GROUP_NONE );
	trace_t tr;
	UTIL_TraceLine( vecFrom, vecFrom + vecDir * 160.0f, MASK_SOLID, &filter, &tr );
	if ( tr.fraction >= 1.0f )
		return NULL;
	if ( tr.m_pEnt && !tr.m_pEnt->IsWorld() )
	{
		const char *psz = LandmarkFromName( STRING( tr.m_pEnt->GetModelName() ) );
		return psz ? psz : LandmarkFromName( tr.m_pEnt->GetClassname() );
	}
	// world: a static prop there?
	CUtlVector<ICollideable *> props;
	Vector vecPad( 12, 12, 12 );
	staticpropmgr->GetAllStaticPropsInAABB( tr.endpos - vecPad, tr.endpos + vecPad, &props );
	for ( int i = 0; i < props.Count(); i++ )
	{
		const model_t *pModel = props[i] ? props[i]->GetCollisionModel() : NULL;
		const char *psz = pModel ? LandmarkFromName( modelinfo->GetModelName( pModel ) ) : NULL;
		if ( psz )
			return psz;
	}
	return NULL;
}
