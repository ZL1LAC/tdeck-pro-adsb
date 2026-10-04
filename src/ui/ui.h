#pragma once

#include <stdint.h>

#include "core/tracker.h"
#include "hw/touch.h"
#include "net/adsb_source.h"

namespace ui {

enum class View : uint8_t { Radar, List, Detail, Status, Settings };

// Everything the views need that lives outside the UI. main.cpp refreshes this
// once per loop; the UI never reaches into the drivers itself.
struct Context {
    const Tracker *tracker = nullptr;

    // Our own position: what ranges and bearings are measured from. The plot
    // may be panned away from it -- see panOffsetNm().
    double ownLat = 0.0;
    double ownLon = 0.0;
    bool ownFromGnss = false;

    bool gnssEnabled = false;
    bool gnssFix = false;
    uint32_t gnssSatellites = 0;
    uint32_t gnssBaud = 0;

    bool wifiConnected = false;
    adsb::Activity networkActivity;
    int wifiRssi = 0;
    const char *wifiSsid = "";
    const char *ipAddress = "";

    bool batteryValid = false;
    uint8_t batteryPercent = 0;
    uint16_t batteryMilliVolts = 0;

    // True while face-down idle has parked the panel / slowed the feed.
    bool idle = false;

    // Local time, already resolved by hw/clock. Invalid until either SNTP or
    // the GNSS has set the system clock.
    bool clockValid = false;
    uint8_t clockHour = 0;
    uint8_t clockMinute = 0;

    bool keypadPresent = false;
    bool touchPresent = false;

    uint32_t lastFetchAgeMs = 0;  // UINT32_MAX if we have never fetched
    adsb::FetchStats lastFetch;
};

void begin();

void setContext(const Context &ctx);

// Input. Both are safe to call with nothing pending.
void handleKey(char key);
void handleTouch(const touch::Event &event);

// Repaints the panel if the scene changed. Returns true if the e-paper was
// actually driven (which takes ~0.7 s and blocks).
bool tick();

// True once when the user has asked for an immediate feed refresh ('r').
bool consumeRefreshRequest();

// Whether the user wants their own position taken from the GNSS fix rather
// than from the configured home position.
bool centreOnGnss();

// How far the plot has been panned away from our own position, in nautical
// miles east and north. Zero unless the user has dragged or arrow-panned.
// main.cpp uses this to aim the feed query at what is actually on screen.
void panOffsetNm(float *eastNm, float *northNm);

// Current query radius, so main.cpp can keep the feed request in step with
// what is actually being displayed.
uint16_t rangeNm();

// ICAO hex of the selected target, or empty. main.cpp passes this to
// Tracker::finishUpdate so a filter change cannot drop what you have open.
const char *selectedHex();

// True once when the traffic filter changed and the live store should be
// compacted before the next poll.
bool consumeFilterChange();

// True once when the poll pace changed and main should retarget gNextPollMs.
bool consumePollPaceChange();

}  // namespace ui
