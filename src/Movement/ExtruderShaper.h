/*
 * PressureAdvanceShaper.h
 *
 *  Created on: 14 May 2021
 *      Author: David
 */

#ifndef SRC_MOVEMENT_EXTRUDERSHAPER_H_
#define SRC_MOVEMENT_EXTRUDERSHAPER_H_

#include <RepRapFirmware.h>

// This class implements MoveSegment generation for extruders with pressure advance.
// It also tracks extrusion that has been commanded but not implemented because less than one full step has been accumulated.
// Currently it only supports linear pressure advance.
// It also stores the maximum extrusion feedrate, which limits the extruder speed during forward extrusion only
// (retracts and load/unload moves are not affected). This can be configured via M203.1.
class ExtruderShaper
{
public:
	ExtruderShaper()
		: k(0.0), maxExtrusionSpeed(0.0)
	{ }

	// Temporary functions until we support more sophisticated pressure advance
	float GetKclocks() const noexcept { return k; }								// get pressure advance in step clocks
	float GetKseconds() const noexcept { return k * (1.0/(float)StepClockRate); }
	void SetKseconds(float val) noexcept { k = val * StepClockRate; }			// set pressure advance in seconds

	// Maximum extrusion feedrate (only applied during forward extrusion, not retracts/load/unload)
	float GetMaxExtrusionSpeed() const noexcept { return maxExtrusionSpeed; }	// get max extrusion speed in mm per step clock (0 = unlimited)
	void SetMaxExtrusionSpeed(float speed) noexcept { maxExtrusionSpeed = speed; }	// set max extrusion speed in mm per step clock

private:
	float k;								// the pressure advance constant in step clocks
	float maxExtrusionSpeed;				// the maximum extrusion feedrate in mm per step clock, 0 means unlimited
};

#endif /* SRC_MOVEMENT_EXTRUDERSHAPER_H_ */
