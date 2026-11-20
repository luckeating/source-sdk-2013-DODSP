//====== DOD-SP =====================================================================
//
// Vehicle armour and shells (stage V4) - the numbers and the maths, one owner each. The vehicle code
// (dodsp_vehicle.cpp) decides what a hit DOES (parts, crew, fires); this file decides whether it gets in.
//
// Real-world data, owner's call 2026-11-20 ("fully real"): armour in mm per plate (hull / turret x front /
// side / rear / top / bottom) with each plate's lean, shells with their real penetration by range, their
// real explosive charge. All of it lives in scripts/dodsp_vehicles.txt with the sources next to it.
//
//   - Penetration tables are against a FLAT (vertical) plate, mm at metres. Sources quoted at 30 degrees
//     are converted in the script's comments (x 1.155 = 1/cos 30). DoDSP_ShellPen interpolates by range.
//   - A plate's effective thickness = mm / cos(angle between the shell's path and the plate's normal),
//     with the plate leaned back by its slope. Past VEHICLE_RICOCHET_ANGLE a shell skids off unless it
//     overmatches the plate (calibre >= 3x the plate).
//   - High explosive: the blast's size comes from the charge (cube-root law), anchored on the 75 mm M48
//     (0.68 kg TNT: kills ~10 m, wounds ~25 m in the open), times dodsp_vehicle_he_scale (owner 2026-11-20:
//     half the real size, 0.5). Against armour the REAL size counts (a plate doesn't care about balance).
//   - Bullets: rifle/MG ball ~6 mm at close range, pistol/SMG ~2 mm (DoDSP_BulletPen). Enough for open tops
//     and the thinnest plates, nothing else.
//
// Units: DODSP_UNITS_PER_METRE (40: a DoD player is 72 units ~ 1.8 m). One owner.
//
//==================================================================================
#ifndef DODSP_VEHICLE_ARMOUR_H
#define DODSP_VEHICLE_ARMOUR_H
#ifdef _WIN32
#pragma once
#endif

class KeyValues;

#define DODSP_UNITS_PER_METRE		40.0f
#define VEHICLE_RICOCHET_ANGLE		70.0f		// degrees off the plate's normal past which a shell skids off
#define DODSP_PEN_POINTS			6

enum ArmourPart_t { APART_HULL, APART_TURRET, NUM_APARTS };
enum ArmourFace_t { AFACE_FRONT, AFACE_SIDE, AFACE_REAR, AFACE_TOP, AFACE_BOTTOM, NUM_AFACES };
extern const char *g_pszArmourPart[NUM_APARTS];
extern const char *g_pszArmourFace[NUM_AFACES];

#define DODSP_MAX_CREW				6
enum CrewRole_t { CREW_COMMANDER, CREW_GUNNER, CREW_LOADER, CREW_DRIVER, CREW_RADIO, NUM_CREW_ROLES };
extern const char *g_pszCrewRole[NUM_CREW_ROLES];

struct ArmourPlate_t
{
	float	flMM;			// thickness
	float	flSlope;		// degrees the plate leans back from upright (0 = vertical; top/bottom: unused)
};

struct DoDSPArmour_t
{
	bool			bValid;
	ArmourPlate_t	plates[NUM_APARTS][NUM_AFACES];
	bool			bOpenTop;			// the crew compartment is open (halftrack, carrier, Sd.Kfz. 222's screened turret)
	float			flStructure;		// how much behind-armour effect it takes before it's knocked out (game value)
	int				nCrew;
	int				eCrew[DODSP_MAX_CREW];	// CrewRole_t per seat
	float			flAmmoFire;			// chance an ammunition hit sets it burning (Sherman dry stowage 0.6-0.8 - real)
	float			flFuelFire;			// chance an engine hit sets it burning
	int				iEngineAt;			// AFACE_REAR / AFACE_FRONT / AFACE_TOP (middle): where the engine is
};

struct DoDSPShell_t
{
	bool	bValid;
	float	flCalibre;					// mm (overmatch, and how big the hole is)
	int		nPen;
	float	flPenRange[DODSP_PEN_POINTS];	// metres
	float	flPenMM[DODSP_PEN_POINTS];		// flat plate
	float	flEffect;					// what gets inside when it penetrates (game value, vs DoDSPArmour_t::flStructure)
	float	flFiller;					// kg of explosive (HE shells; small for APHE/APC, 0 for solid shot)
};

// Load. Returns false (and says why in pszErr) when a block is missing or wrong; the caller prints the INVARIANT.
bool	DoDSP_ParseArmour( KeyValues *pKV, DoDSPArmour_t &out, char *pszErr, int nErr );
bool	DoDSP_ParseShell( KeyValues *pKV, DoDSPShell_t &out, bool bHE, char *pszErr, int nErr );

// Penetration of a shell at a distance (game units), flat plate.
float	DoDSP_ShellPen( const DoDSPShell_t &shell, float flDistUnits );
// High explosive: what a charge pierces at contact (fragments + blast), and the blast's radii (game units).
float	DoDSP_HEPen( float flFillerKg );
void	DoDSP_HERadiiReal( float flFillerKg, float *pflKill, float *pflWound );		// against armour: real size
void	DoDSP_HERadiiPeople( float flFillerKg, float *pflKill, float *pflWound );	// against people: x dodsp_vehicle_he_scale
// A bullet's penetration (ammo type from GetAmmoDef; the damage picks rifle vs pistol for ammo we don't know).
float	DoDSP_BulletPen( int iAmmoType, float flDamage, float flDistUnits );
// Effective thickness of a plate for a path; *pflAngle = degrees off the normal. Normal is the plate's outward normal.
float	DoDSP_EffectiveArmour( const ArmourPlate_t &plate, const Vector &vecPath, const Vector &vecNormal, float *pflAngle );

// Ray against a box (local space). On a hit: *pt = distance along vecDir (unit), *pAxis / *pSign = the face
// entered (axis 0..2, sign +-1). A ray starting inside reports the face it would leave by as the face (t 0).
bool	DoDSP_RayBox( const Vector &vecOrigin, const Vector &vecDir, const Vector &vecMins, const Vector &vecMaxs, float *pt, int *pAxis, int *pSign );

// A blast going off (one owner for shell explosions): the fireball, the sound, the shake, and the damage to
// people (radius from the charge, x dodsp_vehicle_he_scale). While it deals damage, g_DoDSPBlast says what it
// is, so a vehicle in the blast can judge it by its real charge.
struct DoDSPBlast_t { bool bActive; float flFiller; Vector vecCentre; };
extern DoDSPBlast_t g_DoDSPBlast;
void	DoDSP_ShellBlast( const Vector &vecCentre, float flFillerKg, CBaseEntity *pAttacker, CBaseEntity *pIgnore, const char *pszBigSound );

#endif // DODSP_VEHICLE_ARMOUR_H
