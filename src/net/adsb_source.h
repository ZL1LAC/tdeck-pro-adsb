#pragma once

#include <stdint.h>

#include "config.h"
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
    uint16_t filtered = 0;   // dropped as beyond the query radius (local feed)
    uint32_t durationMs = 0;
    uint32_t bytes = 0;
};

// Which feed is in use. Runtime rather than compile-time so the 'p' key can
// swap between your own receiver and a public aggregator without a reflash --
// the local one is fresher and free, the aggregator sees past your antenna's
// horizon and keeps working away from home. Resets to ADSB_PROVIDER_DEFAULT on
// reboot; there is no NVS yet.
AdsbProvider provider();
void setProvider(AdsbProvider p);

// Swap between the local receiver and ADSB_PROVIDER_REMOTE.
void toggleProvider();

// True when the current feed is our own receiver: plain HTTP rather than TLS,
// and a client-side radius filter because it serves everything it hears.
bool isLocal();

// Poll cadence for the current feed, from the ADSB_LOCAL_* / ADSB_REMOTE_*
// settings in config.h.
uint32_t pollIntervalMs();
uint32_t minIntervalMs();

// Human-readable name of the current provider, for the status bar.
const char *providerName();

// Query the feed for traffic within `radiusNm` of (lat, lon) and merge it
// into `tracker`. The public endpoints cap the radius at 250 nm and filter
// server-side; a local receiver serves everything it hears, so there the
// radius is applied here instead.
//
// Blocking: expect 0.3-3 s depending on link quality. Only merges -- the
// caller must call `tracker.finishUpdate()` afterwards with its OWN position,
// which is not necessarily this query centre once the plot has been panned.
FetchStats fetch(Tracker &tracker, double lat, double lon, uint16_t radiusNm);

// millis() of the last successful fetch, or 0.
uint32_t lastSuccessMs();
const FetchStats &lastStats();

}  // namespace adsb
