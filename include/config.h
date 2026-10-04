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
// Your own receiver, or one of three free key-less aggregators. All of them
// serve the same readsb-shaped JSON; they differ only in URL layout and in the
// name of the aircraft array.
//
//   LOCAL           ADSB_LOCAL_URL      your Pi -- see secrets.h
//   ADSB_LOL        api.adsb.lol        no published rate limit, no "desc" field
//   ADSB_FI         opendata.adsb.fi    includes "desc" (type description)
//   AIRPLANES_LIVE  api.airplanes.live  includes "desc", asks for <= 1 req/sec
//
// LOCAL is worth having wherever it reaches. Positions are typically under a
// second old rather than several, the response is a few kilobytes rather than
// a few hundred, there is no TLS handshake and no rate limit, and it keeps
// working with the internet down. What you give up is coverage beyond your own
// antenna's horizon, and -- unless readsb runs with a --db-file -- the
// registration and type designator, which aircraft.json does not carry.
enum class AdsbProvider : uint8_t { ADSB_LOL, ADSB_FI, AIRPLANES_LIVE, LOCAL };

// Which feed the firmware starts on, and which aggregator the 'p' key swaps
// to. The choice is made at runtime rather than compiled in, so both
// transports have to be linked: that costs about 120 KB of flash in mbedtls
// which a LOCAL-only build does not pay. Worth it for being able to walk out
// of Wi-Fi range and still see traffic. A toggle is kept in NVS, so the feed
// you were last on is the one you come back up on.
#define ADSB_PROVIDER_DEFAULT AdsbProvider::LOCAL
#define ADSB_PROVIDER_REMOTE  AdsbProvider::AIRPLANES_LIVE

// Sent so feed operators can identify (and contact) misbehaving clients.
#define ADSB_USER_AGENT "tdeckpro-adsb/0.1 (+https://github.com/)"

// Query radius in nautical miles. The public endpoints cap this at 250 and
// filter server-side. A local aircraft.json has no radius parameter -- it
// carries everything the receiver hears -- so the same figure is applied as a
// client-side filter instead, keeping distant traffic out of the MAX_AIRCRAFT
// slots that nearby traffic needs.
#define ADSB_QUERY_RADIUS_NM 60

// How often to hit the feed, and the hard floor a manual refresh cannot beat.
//
// Against an aggregator the limit is politeness: these are volunteer-run, and
// the e-paper needs ~0.7 s per refresh, so polling faster than ~10 s buys
// nothing but battery drain and rate-limit trouble. Against your own Pi there
// is nobody to be polite to, so the limit becomes the panel itself -- 5 s sits
// just above EPD_MIN_REFRESH_INTERVAL_MS, which is as fast as the glass can
// show a change anyway. The timeout drops with it: a LAN round trip that has
// not answered in 3 s is not going to.
// Your own receiver: the limit is the panel, not politeness. Measured at 35 ms
// per fetch on the LAN, so a 1 s poll costs about 3% of one core and readsb
// only recomputes its own state at 1 Hz anyway -- asking faster than that
// returns the same numbers twice.
#define ADSB_LOCAL_POLL_INTERVAL_MS   1000UL
#define ADSB_LOCAL_MIN_INTERVAL_MS     500UL
#define ADSB_LOCAL_HTTP_TIMEOUT_MS    3000UL

// A public aggregator: volunteer-run, so be polite.
#define ADSB_REMOTE_POLL_INTERVAL_MS 15000UL
#define ADSB_REMOTE_MIN_INTERVAL_MS   5000UL
#define ADSB_REMOTE_HTTP_TIMEOUT_MS  12000UL

// Drop an aircraft from the local store once we have not seen it for this long.
#define AIRCRAFT_STALE_MS 90000UL
#define MAX_AIRCRAFT 96

// Per-aircraft position trail samples kept in RAM. Drawn only for the selected
// target so the radar stays readable on the e-paper display.
#define AIRCRAFT_TRAIL_POINTS 12
#define AIRCRAFT_TRAIL_MIN_NM 0.2f

// ========================================================== Home position ===
// HOME_LATITUDE / HOME_LONGITUDE are in secrets.h alongside the Wi-Fi
// credentials -- an address is worth keeping out of a public repository too.
// They are used until the GNSS gets a fix, and whenever GNSS centring is off.

// The receiver is off. It is the largest continuous draw on the board, and on
// a unit that lives at a fixed address it spends its life confirming a
// position config.h already knows -- so the rail is held down from
// power::begin() and gnss::begin() returns without touching the UART.
//
// Everything below still works if you set this true: the duty cycle, the
// acquire/sleep state machine, the 'g' key. Two things change when it is
// false. The plot centre is always HOME_LATITUDE / HOME_LONGITUDE, and the
// clock loses its off-grid source -- it is then SNTP or the retained RTC only,
// so a board with no Wi-Fi and no recent reboot shows --:--.
#define GNSS_ENABLED false
#define GNSS_CENTRE_BY_DEFAULT true

// GNSS duty cycle. The module is the largest continuous draw on the board and
// the position it reports barely changes, so it is not held powered for its
// own sake: when nothing is reading a position -- the plot is centred on the
// configured home and the clock is already set -- the rail is simply switched
// off. When something is, the module runs only long enough to produce a fix.
//
// GNSS_FIX_HOLD_MS is how long a fix stays usable after the module powers
// down, and must comfortably exceed one sleep plus one acquisition, or a slow
// acquisition would let it expire and bounce the plot back to the configured
// home before the replacement lands.
#define GNSS_SLEEP_MS       120000UL  // between successful fixes
#define GNSS_ACQUIRE_MAX_MS  90000UL  // give up on an acquisition
#define GNSS_RETRY_MS       300000UL  // back off after giving up
#define GNSS_FIX_HOLD_MS    900000UL  // a fix stays usable this long

// ================================================================== Map =====
// Vector basemap: coastline, airports and (optionally) airspace, drawn under
// the radar plot. Built offline and flashed separately from the firmware, or
// copied onto the SD card as /map.bin (preferred when present):
//
//     python tools/build_map.py          # writes data/map.bin
//     pio run -t uploadfs                # flashes it into SPIFFS
//
// Missing data is not an error -- the radar simply draws without it. Toggled
// at runtime with the 'm' key. Additional region files can live in /maps on
// the card and are picked from the settings page.
#define MAP_ENABLED_BY_DEFAULT true
#define MAP_FILE "/map.bin"

// ICAO address -> registration and type, also optional, from SD or SPIFFS:
//
//     python tools/build_db.py           # writes data/aircraftdb.bin
//     pio run -t uploadfs
//
// Only needed because a local receiver's aircraft.json carries neither field.
// Absent, those columns stay empty exactly as they did before. A worldwide
// table is too large for SPIFFS and PSRAM; put it on the SD card and the
// firmware seeks it on demand (see aircraftdb::details).
#define AIRCRAFT_DB_FILE "/aircraftdb.bin"

// Plan-view silhouettes for the detail page, keyed by type designator:
//
//     python tools/build_icons.py        # writes data/icons.bin
//     pio run -t uploadfs                # or copy onto the SD card
//
// Also optional. Absent, the detail page simply lays out without one.
#define ICON_FILE "/icons.bin"

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
// Altitude is always feet. Choose horizontal units (boot default; the
// settings page can change this at runtime):
//   0 = nautical miles, 1 = statute miles, 2 = kilometres
#define UNITS_DISTANCE 0
// Speed: 0 = knots, 1 = mph, 2 = km/h
#define UNITS_SPEED 0

// =============================================================== Display ====
// Range rings, in nautical miles, cycled with the 'z' / 'x' keys.
static const uint16_t kRangeStepsNm[] = {5, 10, 20, 40, 60, 100, 150, 250};
static const size_t kRangeStepCount = sizeof(kRangeStepsNm) / sizeof(kRangeStepsNm[0]);
#define RANGE_DEFAULT_INDEX 3   // 40 nm

// Floor on how often the panel may be driven, and the backstop against a
// scene that changes faster than the glass can show it. User input bypasses
// it, so this is never a floor on responding to a keypress.
//
// A partial refresh takes 651 ms, so 1.5 Hz is the physical ceiling and this
// is what stands between the firmware and running the panel at it. The number
// matters more than it looks: e-paper wears with every refresh, and holding
// 1.5 Hz around the clock would be roughly 86,000 refreshes a day.
//
// 2 s is chosen to sit just below where the content rate takes over. Now that
// positions are fingerprinted in plot pixels rather than degrees, a repaint
// only happens when something actually moves a pixel -- at the default 40 nm
// range that is 0.36 nm, which a 450 kt aircraft covers in about 3 s, so this
// floor does not even bind. Zoom in to 5 nm and a pixel is 0.045 nm, crossed
// in a third of a second, and the floor is what holds the panel to a sane
// rate. The refresh rate therefore scales itself with the zoom level.
#define EPD_MIN_REFRESH_INTERVAL_MS 2000UL

// A full (flashing) refresh clears accumulated ghosting. Every Nth partial is
// promoted to one; also forced on demand with the 'f' key. Raised from 20 with
// the faster refresh rate -- every 20th was a flash every 30 s once partials
// could come every 2 s, which reads as a fault rather than as maintenance.
// Tune by eye: too high and ghosting builds, too low and the flashing annoys.
#define EPD_FULL_REFRESH_EVERY 60
#define EPD_FULL_REFRESH_MAX_AGE_MS 600000UL   // 10 minutes

// Keyboard backlight at boot, and the fallback when nothing has been saved:
//   0 = off, 1 = on, 2 = auto (ALS, with the hysteresis below).
#define KEYPAD_BACKLIGHT_DEFAULT 0

// Auto mode: LED on at or below the first, off at or above the second.
// Between them the last state is kept so a room around 40 lx does not flicker.
#define KEYPAD_BACKLIGHT_AUTO_ON_LX  20.0f
#define KEYPAD_BACKLIGHT_AUTO_OFF_LX 80.0f

// Park the panel (and slow the feed) when the IMU reports face-down for
// FACE_DOWN_IDLE_MS. The panel is hibernated, not merely left undrawn, and
// the keypad LED is forced off. Saves wear and a chunk of the poll power
// budget while the unit is in a pocket. Toggle from Settings; wake on any
// key/touch or when the attitude leaves face-down.
// Off by default: IMU Z polarity on this board is still a best guess, and a
// wrong sign hibernates the panel a couple of seconds after boot -- which
// reads as a freeze. Turn it on from Settings once Diagnostics shows the
// expected "face up" / "face down" labels.
#define FACE_DOWN_IDLE_DEFAULT false
#define FACE_DOWN_IDLE_MS 5000UL
#define FACE_DOWN_IDLE_POLL_MS 60000UL

// =========================================================== Diagnostics ===
// Seconds between heap lines on the serial log; 0 keeps it quiet.
//
// The number worth watching is not free bytes but the largest contiguous
// block, and the gap between the two is fragmentation. It matters here because
// the feed allocates and frees a variable-size JSON arena in PSRAM roughly
// once a second against a local receiver -- a pattern that can leave a heap
// with megabytes free and no room for the next parse, days into a run, which
// is exactly when nobody is watching.
#define DIAG_HEAP_LOG_INTERVAL_MS 60000UL
