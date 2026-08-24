// User-tunable settings. Edit this file before your first flash.
#pragma once

#include <stdint.h>

// =============================================================== Wi-Fi ======
// kWifiNetworks lives in secrets.h, which is gitignored, so a password does
// not go out with the first commit. Copy secrets.example.h over to it.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "No include/secrets.h -- copy include/secrets.example.h to include/secrets.h and fill in your Wi-Fi details."
#endif

#define WIFI_HOSTNAME        "tdeckpro-adsb"
#define WIFI_CONNECT_TIMEOUT_MS 15000UL

// ========================================================== Data source =====
// Three free, key-less feeds. All return readsb-shaped JSON; they differ only
// in URL layout and in the name of the aircraft array.
//
//   ADSB_LOL        api.adsb.lol        no published rate limit, no "desc" field
//   ADSB_FI         opendata.adsb.fi    includes "desc" (type description)
//   AIRPLANES_LIVE  api.airplanes.live  includes "desc", asks for <= 1 req/sec
enum class AdsbProvider : uint8_t { ADSB_LOL, ADSB_FI, AIRPLANES_LIVE };

#define ADSB_PROVIDER AdsbProvider::ADSB_FI

// Sent so feed operators can identify (and contact) misbehaving clients.
#define ADSB_USER_AGENT "tdeckpro-adsb/0.1 (+https://github.com/)"

// Query radius in nautical miles. The public endpoints cap this at 250.
#define ADSB_QUERY_RADIUS_NM 60

// How often to hit the API. The e-paper needs ~0.7 s per refresh, so polling
// faster than ~10 s buys nothing but battery drain and rate-limit trouble.
#define ADSB_POLL_INTERVAL_MS 15000UL
#define ADSB_POLL_MIN_INTERVAL_MS 5000UL   // hard floor, also applies to manual refresh
#define ADSB_HTTP_TIMEOUT_MS 12000UL

// Drop an aircraft from the local store once we have not seen it for this long.
#define AIRCRAFT_STALE_MS 90000UL
#define MAX_AIRCRAFT 96

// ========================================================== Home position ===
// HOME_LATITUDE / HOME_LONGITUDE are in secrets.h alongside the Wi-Fi
// credentials -- an address is worth keeping out of a public repository too.
// They are used until the GNSS gets a fix, and whenever GNSS centring is off.

// Set false to save ~30 mA if you always want the fixed home position.
#define GNSS_ENABLED true
#define GNSS_CENTRE_BY_DEFAULT true

// ================================================================== Map =====
// Vector basemap: coastline, airports and (optionally) airspace, drawn under
// the radar plot. Built offline and flashed separately from the firmware:
//
//     python tools/build_map.py          # writes data/map.bin
//     pio run -t uploadfs                # flashes it into SPIFFS
//
// Missing data is not an error -- the radar simply draws without it. Toggled
// at runtime with the 'm' key.
#define MAP_ENABLED_BY_DEFAULT true
#define MAP_FILE "/map.bin"

// Airport idents are only drawn at or below this range, where there is room
// for them without burying the traffic.
#define MAP_LABEL_RANGE_NM 40

// ================================================================ Clock =====
// The status-bar clock is set from SNTP when Wi-Fi is up, and from the GNSS
// date/time otherwise -- whichever arrives first wins, so it works indoors and
// off-grid.
//
// POSIX TZ string, including DST rules. The default matches the HOME_LATITUDE /
// HOME_LONGITUDE above. Other zones: https://github.com/nayarsystems/posix_tz_db
#define CLOCK_TZ "NZST-12NZDT,M9.5.0,M4.1.0/3"
#define CLOCK_NTP_SERVER_1 "pool.ntp.org"
#define CLOCK_NTP_SERVER_2 "time.nist.gov"

// ================================================================ Units =====
// Altitude is always feet. Choose horizontal units:
//   0 = nautical miles, 1 = statute miles, 2 = kilometres
#define UNITS_DISTANCE 0
// Speed: 0 = knots, 1 = mph, 2 = km/h
#define UNITS_SPEED 0

// =============================================================== Display ====
// Range rings, in nautical miles, cycled with the 'a' / 'd' keys.
static const uint16_t kRangeStepsNm[] = {5, 10, 20, 40, 60, 100, 150, 250};
static const size_t kRangeStepCount = sizeof(kRangeStepsNm) / sizeof(kRangeStepsNm[0]);
#define RANGE_DEFAULT_INDEX 3   // 40 nm

// A full (flashing) refresh clears accumulated ghosting. Every Nth partial
// update is promoted to a full one; also forced on demand with the 'f' key.
// Floor on how often the panel may be driven. Content can change faster than
// this (RSSI, poll age, aircraft moving); repainting every time would burn
// ~1.4 refreshes/second for changes nobody can see. User input bypasses it.
#define EPD_MIN_REFRESH_INTERVAL_MS 4000UL

#define EPD_FULL_REFRESH_EVERY 20
#define EPD_FULL_REFRESH_MAX_AGE_MS 600000UL   // 10 minutes

// Keyboard backlight on at boot.
#define KEYPAD_BACKLIGHT_DEFAULT false
