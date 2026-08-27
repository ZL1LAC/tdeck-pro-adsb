#pragma once

#include <stdint.h>

#include "config.h"
#include "core/tracker.h"

namespace adsb {

enum class Result : uint8_t {
    Ok,
    NotConnected,
    HttpError,
    ParseError,
};

struct FetchStats {
    Result result = Result::NotConnected;
    int httpStatus = 0;
    uint16_t received = 0;   // aircraft objects in the response
    uint16_t stored = 0;     // how many made it into the tracker
    uint16_t filtered = 0;   // dropped as beyond the query radius (local feed)
    uint16_t dropped = 0;    // refused: store full, and no nearer than its contents
    uint32_t durationMs = 0;
    uint32_t bytes = 0;
};

// Which feed is in use. Runtime rather than compile-time so the 'p' key can
// swap between your own receiver and a public aggregator without a reflash --
// the local one is fresher and free, the aggregator sees past your antenna's
// horizon and keeps working away from home.
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

// ------------------------------------------------------------ the fetch ----
//
// A poll is a socket, a TLS handshake and a couple of hundred kilobytes of
// JSON: 1.3-1.5 s against an aggregator. Run on the loop task that was 1.5 s
// in which nothing read the keyboard, the touch panel or the GNSS UART -- the
// three things main.cpp's fast path exists to keep up with. So it runs on a
// task of its own instead, and the loop task is left holding nothing but a
// request and, later, a result.
//
// The handoff is a baton, not a shared structure. The task decodes into a
// staging store of its own and never touches the caller's tracker; the merge
// happens in collect(), back on the loop task. Nothing needs a mutex, and the
// tracker stays as single-threaded as it has always been.

// Start the task. Call once, after net::begin().
void begin();

// Ask for a snapshot around (lat, lon). Returns false -- having done nothing --
// if a fetch is already in flight, the rate limiter refused, or the link is
// down. Never blocks.
bool request(double lat, double lon, uint16_t radiusNm);

// A fetch is in flight.
bool busy();

// A fetch has finished and is waiting to be collected. Cheap enough for the
// fast path, which uses it to cut a UI tick short rather than let a result sit
// until the next one.
bool ready();

// Merges a finished fetch into `tracker` and reports how it went. False when
// there was nothing waiting. True at most once per fetch.
//
// The caller must still call tracker.finishUpdate() afterwards with its OWN
// position, which is not necessarily the query centre once the plot has been
// panned.
bool collect(Tracker &tracker, FetchStats *stats);

// millis() of the last successful fetch, or 0.
uint32_t lastSuccessMs();
const FetchStats &lastStats();

// Smallest the fetch task's stack has ever been, in bytes. On the diagnostics
// page because a TLS handshake is the deepest thing this firmware does and the
// figure is otherwise invisible until it overflows.
uint32_t taskHeadroomBytes();

}  // namespace adsb
