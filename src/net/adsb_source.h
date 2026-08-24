#pragma once

#include <stdint.h>

#include "core/tracker.h"

namespace adsb {

enum class Result : uint8_t {
    Ok,
    NotConnected,
    RateLimited,   // called again before ADSB_POLL_MIN_INTERVAL_MS elapsed
    HttpError,
    ParseError,
};

struct FetchStats {
    Result result = Result::NotConnected;
    int httpStatus = 0;
    uint16_t received = 0;   // aircraft objects in the response
    uint16_t stored = 0;     // how many made it into the tracker
    uint32_t durationMs = 0;
    uint32_t bytes = 0;
};

// Human-readable name of the configured provider, for the status bar.
const char *providerName();

// Query the feed for traffic within `radiusNm` of (lat, lon) and merge it
// into `tracker`. The public endpoints cap the radius at 250 nm.
//
// Blocking: expect 0.3-3 s depending on link quality. Only merges -- the
// caller must call `tracker.finishUpdate()` afterwards with its OWN position,
// which is not necessarily this query centre once the plot has been panned.
FetchStats fetch(Tracker &tracker, double lat, double lon, uint16_t radiusNm);

// millis() of the last successful fetch, or 0.
uint32_t lastSuccessMs();
const FetchStats &lastStats();

}  // namespace adsb
