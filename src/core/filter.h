#pragma once

#include "aircraft.h"
#include "settings.h"

// Which traffic belongs on the plot and in the list, and which airspace
// outlines are worth drawing under it. Driven by Settings.trafficFilter.
namespace filter {

bool accept(const Aircraft &a);

const char *name();

// All -> Airborne -> Low -> High -> All. Returns the new value.
TrafficFilter cycle();

// Airspace whose floor sits above this (feet) is dropped. INT16_MAX means
// "no floor cutoff". Unknown floors (INT16_MIN) are never dropped this way.
int16_t airspaceMaxFloorFt();

// Airspace whose ceiling sits below this (feet) is dropped. 0 means "no
// ceiling cutoff". Unknown/unlimited ceilings are never dropped this way.
int16_t airspaceMinCeilingFt();

bool airspaceVisible(int16_t floorFt, int16_t ceilFt);

}  // namespace filter
