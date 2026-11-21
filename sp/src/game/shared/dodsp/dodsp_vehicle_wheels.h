//========= DOD-SP ============================================================
// dodsp_vehicle_wheels.h - a vehicle model's wheel facts, read from the model itself (shared: client + server).
//
// ONE owner: the model. dodsp_tools/vehicle_preview/vehicle_rig.py puts the port's mesh on CoD2's own running-gear
// bones and weights and measures every fact from the mesh into the QC as $keyvalues "dodsp_wheels":
//     "dodsp_wheels" { "wheelbase" "122.1"  "body_pivot" "-26.1"  "body_bone" "body"
//                      "susp_left_front" "up 6.4 down 6.4"   "wheel_left_01" "radius 17.5 steer 1"  ... }
// susp_*: a suspension unit (CoD2's tag_wheel_<front|middle|back>_<side>, on the ground under its wheels). The client
// puts it on the ground under it every frame within its travel; its road wheels, its share of the track belt (vertex
// weights) and the top run where that rests on them go with it. wheel_*: spins by its own radius, the steered ones
// turn with the server's steering angle. The server turns its steering into the bicycle model's wheelbase.
// A model without the block keeps V1's rule: every bone with "wheel" in its name spins, radius = its height above the
// vehicle's floor, nothing steers or rides on suspension.
//=============================================================================
#ifndef DODSP_VEHICLE_WHEELS_H
#define DODSP_VEHICLE_WHEELS_H
#pragma once

#include "utlvector.h"

struct model_t;

#define DODSP_WHEELS_KEY	"dodsp_wheels"

struct DoDSPWheel_t
{
	char	szBone[64];
	float	flRadius;			// units, about the axle
	bool	bSteer;				// turns with the steering
};

struct DoDSPSusp_t
{
	char	szBone[64];
	float	flUp;				// units it may rise (the room between what moves with it and what doesn't, measured)
	float	flDown;				// units it may drop below its rest
};

struct DoDSPWheels_t
{
	DoDSPWheels_t() : flWheelbase( 0.0f ), flBodyPivot( 0.0f ), bBodyPivot( false ) { szBodyBone[0] = 0; }
	float						flWheelbase;	// front (steered) axle to the middle of the ground wheels behind it
	float						flBodyPivot;	// where the sprung body tips: model forward (x) from the origin
	bool						bBodyPivot;		// the model gives one (the body rides on springs, the wheels stay down)
	char						szBodyBone[64];	// the sprung body: this bone and its children tip; the rest (root
												// chassis with the tread belt, the wheels) is the unsprung running gear
	CUtlVector<DoDSPWheel_t>	wheels;
	CUtlVector<DoDSPSusp_t>		susp;
};

// false: the model has no "dodsp_wheels" block. A malformed entry prints an INVARIANT (once per model) and is skipped.
bool DoDSP_ReadWheels( const model_t *pModel, DoDSPWheels_t &out );

#endif // DODSP_VEHICLE_WHEELS_H
