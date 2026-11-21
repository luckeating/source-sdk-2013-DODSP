//========= DOD-SP ============================================================
// dodsp_vehicle_wheels.h - a vehicle model's wheel facts, read from the model itself (shared: client + server).
//
// ONE owner: the model. dodsp_tools/vehicle_preview/halftrack_rig.py measures every wheel from the mesh (its radius
// about the axle, whether it steers, the wheelbase) and writes them into the QC as $keyvalues "dodsp_wheels":
//     "dodsp_wheels" { "wheelbase" "122.1"  "wheel_left_01" "radius 17.5 steer 1"  "wheel_left_02" "radius 12.6" ... }
// The client spins each wheel by its own radius and turns the steered ones; the server turns the steering angle it
// networks into the bicycle model's wheelbase. A model without the block (the tanks today) keeps the old rule: every
// bone with "wheel" in its name spins, radius = its height above the vehicle's floor, nothing steers.
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

struct DoDSPWheels_t
{
	DoDSPWheels_t() : flWheelbase( 0.0f ) {}
	float						flWheelbase;	// front (steered) axle to the middle of the ground wheels behind it
	CUtlVector<DoDSPWheel_t>	wheels;
};

// false: the model has no "dodsp_wheels" block. A malformed entry prints an INVARIANT (once per model) and is skipped.
bool DoDSP_ReadWheels( const model_t *pModel, DoDSPWheels_t &out );

#endif // DODSP_VEHICLE_WHEELS_H
