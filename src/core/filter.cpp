#include "filter.h"

#include <stdint.h>

#include "config.h"
#include "settings.h"

namespace filter {
namespace {

constexpr int32_t kFl100Ft = 10000;
constexpr int16_t kUpperAtsFt = 19500;

TrafficFilter current() {
    return static_cast<TrafficFilter>(settings::get().trafficFilter);
}

}  // namespace

bool accept(const Aircraft &a) {
    if (a.emergency) return true;

    switch (current()) {
        case TrafficFilter::All:
            return true;
        case TrafficFilter::Airborne:
            return !a.onGround();
        case TrafficFilter::Low:
            if (a.onGround()) return false;
            if (!a.altitudeKnown()) return true;
            return a.altitudeFt < kFl100Ft;
        case TrafficFilter::High:
            if (!a.altitudeKnown()) return false;
            return a.altitudeFt >= kFl100Ft;
    }
    return true;
}

const char *name() {
    switch (current()) {
        case TrafficFilter::Airborne: return "air";
        case TrafficFilter::Low: return "low";
        case TrafficFilter::High: return "high";
        case TrafficFilter::All:
        default: return "all";
    }
}

TrafficFilter cycle() {
    Settings &s = settings::get();
    s.trafficFilter = static_cast<uint8_t>(
        (s.trafficFilter + 1) % (static_cast<uint8_t>(TrafficFilter::High) + 1));
    settings::markDirty();
    return current();
}

int16_t airspaceMaxFloorFt() {
    switch (current()) {
        case TrafficFilter::Low: return kFl100Ft;
        case TrafficFilter::All:
        case TrafficFilter::Airborne:
            return kUpperAtsFt;
        case TrafficFilter::High:
            return INT16_MAX;
    }
    return kUpperAtsFt;
}

int16_t airspaceMinCeilingFt() {
    return current() == TrafficFilter::High ? static_cast<int16_t>(kFl100Ft)
                                            : static_cast<int16_t>(0);
}

bool airspaceVisible(int16_t floorFt, int16_t ceilFt) {
    const int16_t maxFloor = airspaceMaxFloorFt();
    if (maxFloor != INT16_MAX && floorFt != INT16_MIN && floorFt >= maxFloor) {
        return false;
    }
    const int16_t minCeil = airspaceMinCeilingFt();
    if (minCeil > 0 && ceilFt != INT16_MAX && ceilFt != INT16_MIN &&
        ceilFt < minCeil) {
        return false;
    }
    return true;
}

}  // namespace filter
