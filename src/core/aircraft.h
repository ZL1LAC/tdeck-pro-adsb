#pragma once

#include <stdint.h>

#include "config.h"

// One tracked target. Kept POD and compact: MAX_AIRCRAFT of these live in a
// flat array so we never fragment the heap on a long-running device.
struct Aircraft {
    char hex[8];      // ICAO 24-bit address, lower case hex. Primary key.
    char flight[10];  // Callsign, space-trimmed. May be empty.
    char reg[10];     // Registration, when the feed supplies it.
    char type[6];     // ICAO type designator, e.g. "A20N".

    double lat;
    double lon;
    bool hasPosition;

    int32_t altitudeFt;  // kAltitudeUnknown, or kAltitudeGround for "ground".
    int16_t verticalRateFpm;
    float groundSpeedKt;
    float trackDeg;
    bool hasTrack;

    char squawk[6];
    bool emergency;

    // Derived from the current centre position, recomputed on every poll.
    float distanceNm;
    float bearingDeg;

    uint32_t lastUpdateMs;  // millis() of the last feed update mentioning it.
    float seenPosSec;       // Feed-reported age of the position report.

    struct TrailPoint {
        double lat;
        double lon;
    };
    TrailPoint trail[AIRCRAFT_TRAIL_POINTS];
    uint8_t trailCount;
    uint8_t trailNext;

    static constexpr int32_t kAltitudeUnknown = INT32_MIN;
    static constexpr int32_t kAltitudeGround = INT32_MIN + 1;

    bool valid() const { return hex[0] != '\0'; }
    bool onGround() const { return altitudeFt == kAltitudeGround; }
    bool altitudeKnown() const {
        return altitudeFt != kAltitudeUnknown && altitudeFt != kAltitudeGround;
    }
    // Best available label for the target, never empty.
    const char *label() const {
        if (flight[0]) return flight;
        if (reg[0]) return reg;
        return hex;
    }
};
