//=============================================================================//
//
// DOD-SP: Day of Defeat weapons for NPCs (stage 2b).
//
// weapon_k98 and weapon_mp40 use DoD:S world models and sounds. Everything an
// NPC needs to tune lives in the weapon's own script, in an "npc" block next to
// the normal WeaponData keys, e.g. scripts/weapon_k98.txt:
//
//     "npc"
//     {
//         "damage"            "110"   // per bullet, against NPCs (DoD's own value)
//         "damage_player"     "40"    // per bullet, against the player (balance knob)
//         "fire_rate"         "1.6"   // seconds between shots (bolt cycle)
//         "burst_min"         "1"
//         "burst_max"         "1"
//         "rest_min"          "0.4"   // pause between bursts
//         "rest_max"          "1.0"
//         "spread"            "1.6"   // degrees, before NPC skill (proficiency) is applied
//         "muzzleflash"       "shotgun" // smg1, pistol, shotgun, combine or none
//         "tracer_freq"       "0"     // 0 = no tracers, 1 = every shot, 3 = every third
//         "bolt_sound_delay"  "0.55"  // > 0 plays the "special1" sound this long after each shot
//     }
//
// The script is re-read whenever a weapon is precached (map load), so edits
// only need a map restart.
//
//=============================================================================//

#include "cbase.h"
#include "npcevent.h"
#include "basehlcombatweapon.h"
#include "basecombatcharacter.h"
#include "ai_basenpc.h"
#include "soundent.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "te_effect_dispatch.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"

#define DODSP_WEAPON_SOUNDS		"scripts/game_sounds_dodsp_weapons.txt"

extern ISoundEmitterSystemBase *soundemitterbase;

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// DoD's weapon sounds live in DoD's game_sounds_weapons.txt, which HL2's file of
// the same name hides, so we load our own copy of the entries we use. Sound
// overrides are cleared when a level shuts down - including "restart" and
// "reload" of the same map - so load them at the start of EVERY level, before
// any entity precaches.
//-----------------------------------------------------------------------------
class CDODSPWeaponSoundLoader : public CAutoGameSystem
{
public:
	CDODSPWeaponSoundLoader() : CAutoGameSystem( "DODSPWeaponSoundLoader" ) {}

	virtual void LevelInitPreEntity()
	{
		if ( filesystem->FileExists( DODSP_WEAPON_SOUNDS, "GAME" ) )
			soundemitterbase->AddSoundOverrides( DODSP_WEAPON_SOUNDS );
		else
			Warning( "DOD-SP: %s is missing - DoD weapons will be silent\n", DODSP_WEAPON_SOUNDS );
	}
};
static CDODSPWeaponSoundLoader g_DODSPWeaponSoundLoader;

extern acttable_t *GetSMG1Acttable();
extern int GetSMG1ActtableCount();
extern acttable_t *GetAR2Acttable();
extern int GetAR2ActtableCount();

//-----------------------------------------------------------------------------
// Shared base for the DoD guns (no network class of its own)
//-----------------------------------------------------------------------------
class CWeaponDODSPGun : public CBaseHLCombatWeapon
{
	DECLARE_CLASS( CWeaponDODSPGun, CBaseHLCombatWeapon );
	DECLARE_DATADESC();

public:
	CWeaponDODSPGun();

	void		Precache( void );

	int			CapabilitiesGet( void ) { return bits_CAP_WEAPON_RANGE_ATTACK1; }

	const Vector &GetBulletSpread( void );
	int			GetMinBurst( void ) { return m_nBurstMin; }
	int			GetMaxBurst( void ) { return m_nBurstMax; }
	float		GetFireRate( void ) { return m_flFireRate; }
	float		GetMinRestTime( void ) { return m_flRestMin; }
	float		GetMaxRestTime( void ) { return m_flRestMax; }

	void		Operator_HandleAnimEvent( animevent_t *pEvent, CBaseCombatCharacter *pOperator );
	void		Operator_ForceNPCFire( CBaseCombatCharacter *pOperator, bool bSecondary );
	void		FireNPCPrimaryAttack( CBaseCombatCharacter *pOperator, const Vector &vecShootOrigin, const Vector &vecShootDir );

	// HL2 NPCs (rebels, Combine) that pick these up use the HL2 animation sets.
	acttable_t	*ActivityList( void ) { return UsesRifleAnims() ? GetAR2Acttable() : GetSMG1Acttable(); }
	int			ActivityListCount( void ) { return UsesRifleAnims() ? GetAR2ActtableCount() : GetSMG1ActtableCount(); }
	acttable_t	*GetBackupActivityList( void ) { return ActivityList(); }
	int			GetBackupActivityListCount( void ) { return ActivityListCount(); }

	void		BoltSoundThink( void );

	// MG42 (A4b): bipod down = deployed (the w_..bd model, tight spread); up = carried (hip fire, wide)
	virtual void SetDeployed( bool bDeployed ) {}
	virtual bool IsDeployed( void ) const { return false; }

protected:
	virtual bool UsesRifleAnims( void ) { return false; }
	void		LoadNPCTuning( void );

	float		m_flDamage;
	int			m_iPlayerDamage;
	int			m_nMuzzleFlashType;
	float		m_flFireRate;
	int			m_nBurstMin;
	int			m_nBurstMax;
	float		m_flRestMin;
	float		m_flRestMax;
	float		m_flSpreadDegrees;
	int			m_nTracerFreq;
	float		m_flBoltSoundDelay;
	float		m_flHipSpreadScale;		// MG: spread x this when not deployed
};

BEGIN_DATADESC( CWeaponDODSPGun )
	DEFINE_THINKFUNC( BoltSoundThink ),
END_DATADESC()

CWeaponDODSPGun::CWeaponDODSPGun()
{
	m_flDamage = 0.0f;		// 0 = use the ammo type's damage
	m_iPlayerDamage = 0;	// 0 = same as m_flDamage
	m_nMuzzleFlashType = MUZZLEFLASH_SMG1;
	m_flFireRate = 0.1f;
	m_nBurstMin = 1;
	m_nBurstMax = 1;
	m_flRestMin = 0.3f;
	m_flRestMax = 0.6f;
	m_flSpreadDegrees = 3.0f;
	m_nTracerFreq = 2;
	m_flBoltSoundDelay = 0.0f;
	m_flHipSpreadScale = 1.0f;
	m_fMinRange1 = 0;
	m_fMaxRange1 = 32768;	// whole map: a K98 is effective to ~500 m, longer than any Source map. sight_distance and spread limit him
}

void CWeaponDODSPGun::Precache( void )
{
	BaseClass::Precache();
	LoadNPCTuning();
}

void CWeaponDODSPGun::LoadNPCTuning( void )
{
	char szFile[MAX_PATH];
	Q_snprintf( szFile, sizeof( szFile ), "scripts/%s.txt", GetClassname() );

	KeyValues *pFile = new KeyValues( "WeaponData" );
	if ( pFile->LoadFromFile( filesystem, szFile, "GAME" ) )
	{
		KeyValues *pNPC = pFile->FindKey( "npc" );
		if ( pNPC )
		{
			m_flDamage			= pNPC->GetFloat( "damage", m_flDamage );
			m_iPlayerDamage		= pNPC->GetInt( "damage_player", m_iPlayerDamage );

			const char *pszFlash = pNPC->GetString( "muzzleflash", "smg1" );
			if ( !Q_stricmp( pszFlash, "none" ) )			m_nMuzzleFlashType = -1;
			else if ( !Q_stricmp( pszFlash, "pistol" ) )	m_nMuzzleFlashType = MUZZLEFLASH_PISTOL;
			else if ( !Q_stricmp( pszFlash, "shotgun" ) )	m_nMuzzleFlashType = MUZZLEFLASH_SHOTGUN;
			else if ( !Q_stricmp( pszFlash, "combine" ) )	m_nMuzzleFlashType = MUZZLEFLASH_COMBINE;
			else											m_nMuzzleFlashType = MUZZLEFLASH_SMG1;
			m_flFireRate		= pNPC->GetFloat( "fire_rate", m_flFireRate );
			m_nBurstMin			= MAX( 1, pNPC->GetInt( "burst_min", m_nBurstMin ) );
			m_nBurstMax			= MAX( m_nBurstMin, pNPC->GetInt( "burst_max", m_nBurstMax ) );
			m_flRestMin			= pNPC->GetFloat( "rest_min", m_flRestMin );
			m_flRestMax			= MAX( m_flRestMin, pNPC->GetFloat( "rest_max", m_flRestMax ) );
			m_flSpreadDegrees	= pNPC->GetFloat( "spread", m_flSpreadDegrees );
			m_nTracerFreq		= pNPC->GetInt( "tracer_freq", m_nTracerFreq );
			m_flBoltSoundDelay	= pNPC->GetFloat( "bolt_sound_delay", m_flBoltSoundDelay );
			m_flHipSpreadScale	= MAX( 1.0f, pNPC->GetFloat( "hip_spread_scale", m_flHipSpreadScale ) );
		}
		else
		{
			Warning( "%s: no \"npc\" block in %s, using defaults\n", GetClassname(), szFile );
		}
	}
	else
	{
		Warning( "%s: could not read %s\n", GetClassname(), szFile );
	}
	pFile->deleteThis();
}

const Vector &CWeaponDODSPGun::GetBulletSpread( void )
{
	static Vector cone;
	float flDeg = m_flSpreadDegrees * ( IsDeployed() ? 1.0f : m_flHipSpreadScale );	// hip_spread_scale: 1 unless the script says (the MG42)
	float flSin = sinf( DEG2RAD( flDeg * 0.5f ) );
	cone.Init( flSin, flSin, flSin );
	return cone;
}

void CWeaponDODSPGun::FireNPCPrimaryAttack( CBaseCombatCharacter *pOperator, const Vector &vecShootOrigin, const Vector &vecShootDir )
{
	CSoundEnt::InsertSound( SOUND_COMBAT | SOUND_CONTEXT_GUNFIRE, pOperator->GetAbsOrigin(), SOUNDENT_VOLUME_MACHINEGUN, 0.2,
		pOperator, SOUNDENT_CHANNEL_WEAPON, pOperator->GetEnemy() );

	WeaponSound( SINGLE_NPC );

	// vecShootDir already carries the spread (GetActualShootTrajectory applies
	// GetBulletSpread scaled by the NPC's proficiency), like the HL2 guns.
	FireBulletsInfo_t info( 1, vecShootOrigin, vecShootDir, VECTOR_CONE_PRECALCULATED, MAX_TRACE_LENGTH, m_iPrimaryAmmoType );
	info.m_iTracerFreq = m_nTracerFreq;
	info.m_flDamage = m_flDamage;
	info.m_iPlayerDamage = m_iPlayerDamage;
	info.m_pAttacker = pOperator;
	pOperator->FireBullets( info );

	pOperator->DoMuzzleFlash();		// dynamic light

	// DoD animations have no muzzle-flash events, so send the flash sprite ourselves
	// from the world model's "muzzle_flash" attachment.
	if ( m_nMuzzleFlashType >= 0 )
	{
		int iAttachment = LookupAttachment( "muzzle_flash" );
		if ( iAttachment <= 0 )
			iAttachment = LookupAttachment( "muzzle" );
		if ( iAttachment <= 0 )
			iAttachment = LookupAttachment( "0" );		// DoD's MG world models name the muzzle "0" (shell eject "1")

		CEffectData data;
		data.m_nEntIndex = entindex();
		data.m_nAttachmentIndex = MAX( iAttachment, 0 );
		data.m_fFlags = m_nMuzzleFlashType;
		data.m_flScale = 1.0f;
		DispatchEffect( "MuzzleFlash", data );
	}
	m_iClip1 = MAX( 0, m_iClip1 - 1 );

	// Bolt / cycle pacing: the German soldier waits for this before the next shot.
	m_flNextPrimaryAttack = gpGlobals->curtime + m_flFireRate;

	if ( m_flBoltSoundDelay > 0.0f )
	{
		SetContextThink( &CWeaponDODSPGun::BoltSoundThink, gpGlobals->curtime + m_flBoltSoundDelay, "BoltSound" );
	}
}

void CWeaponDODSPGun::BoltSoundThink( void )
{
	if ( GetOwner() )
		WeaponSound( SPECIAL1 );
}

void CWeaponDODSPGun::Operator_HandleAnimEvent( animevent_t *pEvent, CBaseCombatCharacter *pOperator )
{
	switch ( pEvent->event )
	{
	// Any HL2 NPC gun-fire event fires these guns, so both HL2 NPCs and the
	// German soldier (which sends one from code) can use them.
	case EVENT_WEAPON_SMG1:
	case EVENT_WEAPON_AR1:
	case EVENT_WEAPON_AR2:
	case EVENT_WEAPON_PISTOL_FIRE:
	case EVENT_WEAPON_SNIPER_RIFLE_FIRE:
	case EVENT_WEAPON_SMG1_BURST1:
	case EVENT_WEAPON_SMG1_BURSTN:
		{
			Vector vecShootOrigin;
			QAngle angDiscard;
			if ( !pEvent->options || !pEvent->options[0] || !pOperator->GetAttachment( pEvent->options, vecShootOrigin, angDiscard ) )
				vecShootOrigin = pOperator->Weapon_ShootPosition();

			CAI_BaseNPC *pNPC = pOperator->MyNPCPointer();
			if ( !pNPC )
				return;

			Vector vecShootDir = pNPC->GetActualShootTrajectory( vecShootOrigin );
			FireNPCPrimaryAttack( pOperator, vecShootOrigin, vecShootDir );
		}
		break;

	default:
		BaseClass::Operator_HandleAnimEvent( pEvent, pOperator );
		break;
	}
}

void CWeaponDODSPGun::Operator_ForceNPCFire( CBaseCombatCharacter *pOperator, bool bSecondary )
{
	m_iClip1++;	// forced fire doesn't cost ammo

	Vector vecShootOrigin = pOperator->Weapon_ShootPosition();
	Vector vecShootDir;
	CAI_BaseNPC *pNPC = pOperator->MyNPCPointer();
	if ( pNPC && pNPC->GetEnemy() )
	{
		vecShootDir = pNPC->GetActualShootTrajectory( vecShootOrigin );
	}
	else
	{
		QAngle angShoot;
		GetAttachment( LookupAttachment( "muzzle" ), vecShootOrigin, angShoot );
		AngleVectors( angShoot, &vecShootDir );
	}

	FireNPCPrimaryAttack( pOperator, vecShootOrigin, vecShootDir );
}

//-----------------------------------------------------------------------------
// Kar98k - bolt-action rifle
//-----------------------------------------------------------------------------
class CWeaponK98 : public CWeaponDODSPGun
{
	DECLARE_CLASS( CWeaponK98, CWeaponDODSPGun );
public:
	DECLARE_SERVERCLASS();
protected:
	bool UsesRifleAnims( void ) { return true; }
};

IMPLEMENT_SERVERCLASS_ST( CWeaponK98, DT_WeaponK98 )
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS( weapon_k98, CWeaponK98 );
PRECACHE_WEAPON_REGISTER( weapon_k98 );

//-----------------------------------------------------------------------------
// MP40 - submachine gun
//-----------------------------------------------------------------------------
class CWeaponMP40 : public CWeaponDODSPGun
{
	DECLARE_CLASS( CWeaponMP40, CWeaponDODSPGun );
public:
	DECLARE_SERVERCLASS();
};

IMPLEMENT_SERVERCLASS_ST( CWeaponMP40, DT_WeaponMP40 )
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS( weapon_mp40, CWeaponMP40 );
PRECACHE_WEAPON_REGISTER( weapon_mp40 );

//-----------------------------------------------------------------------------
// MG42 - the squad's machine gun (A4b). Carried with the bipod up (w_mg42bu); the gunner
// deploys it on a wall or prone and it swaps to the bipod-down model (w_mg42bd). Both put the
// gun on ValveBiped.weapon_bone (the right hand), like the MP40.
//-----------------------------------------------------------------------------
#define MG42_MODEL_UP		"models/weapons/w_mg42bu.mdl"
#define MG42_MODEL_DOWN		"models/weapons/w_mg42bd.mdl"

class CWeaponMG42 : public CWeaponDODSPGun
{
	DECLARE_CLASS( CWeaponMG42, CWeaponDODSPGun );
public:
	DECLARE_SERVERCLASS();
	DECLARE_DATADESC();

	CWeaponMG42() { m_bDeployed = false; }

	void Precache( void )
	{
		BaseClass::Precache();
		PrecacheModel( MG42_MODEL_UP );
		PrecacheModel( MG42_MODEL_DOWN );
		PrecacheScriptSound( "DODSP_MG42.Deploy" );
	}
	void SetDeployed( bool bDeployed )
	{
		if ( bDeployed == m_bDeployed )
			return;
		m_bDeployed = bDeployed;
		SetModel( bDeployed ? MG42_MODEL_DOWN : MG42_MODEL_UP );
		EmitSound( "DODSP_MG42.Deploy" );
	}
	bool IsDeployed( void ) const { return m_bDeployed; }
	void Drop( const Vector &vecVelocity )
	{
		SetDeployed( false );		// a dropped MG lies there with its bipod up
		BaseClass::Drop( vecVelocity );
	}
protected:
	bool UsesRifleAnims( void ) { return true; }
	bool m_bDeployed;
};

BEGIN_DATADESC( CWeaponMG42 )
	DEFINE_FIELD( m_bDeployed, FIELD_BOOLEAN ),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST( CWeaponMG42, DT_WeaponMG42 )
END_SEND_TABLE()

LINK_ENTITY_TO_CLASS( weapon_mg42, CWeaponMG42 );
PRECACHE_WEAPON_REGISTER( weapon_mg42 );

// For the German soldier: deploy / stow a DoD gun that has a bipod (no-op for the others).
void DoDSP_SetWeaponDeployed( CBaseCombatWeapon *pWeapon, bool bDeployed )
{
	CWeaponDODSPGun *pGun = dynamic_cast<CWeaponDODSPGun *>( pWeapon );
	if ( pGun )
		pGun->SetDeployed( bDeployed );
}
