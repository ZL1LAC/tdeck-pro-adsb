// T-Deck Pro ADS-B tracker.
//
// The board cannot receive 1090 MHz itself -- the SX1262 is sub-GHz only and
// the ESP32-S3 could not decode a 2 MSPS stream anyway -- so this firmware is
// a client of a public ADS-B aggregator over Wi-Fi. It polls traffic around
// the current position, keeps a local aged view of it, and plots it on the
// e-paper as a north-up radar scope with a sortable list behind it.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "board_pins.h"
#include "config.h"
#include "core/aircraftdb.h"
#include "core/aircraft.h"
#include "core/assets.h"
#include "core/geo.h"
#include "core/settings.h"
#include "core/tracker.h"
#include "hw/clock.h"
#include "hw/gnss.h"
#include "hw/keypad.h"
#include "hw/als.h"
#include "hw/imu.h"
#include "hw/power.h"
#include "hw/sdcard.h"
#include "hw/touch.h"
#include "net/adsb_source.h"
#include "net/net.h"
#include "ui/basemap.h"
#include "ui/display.h"
#include "ui/icons.h"
#include "ui/ui.h"

namespace {

Tracker gTracker;

uint32_t gNextPollMs = 0;
uint32_t gNextAgeOutMs = 0;
uint16_t gLastRadiusNm = 0;
double gLastQueryLat = 0.0;
double gLastQueryLon = 0.0;
bool gHaveQueried = false;
uint32_t gNextBatteryMs = 0;
uint32_t gNextHeapLogMs = 0;
bool gBatteryValid = false;
uint8_t gBatteryPercent = 0;
uint16_t gBatteryMilliVolts = 0;

constexpr uint32_t kBatteryIntervalMs = 30000;
constexpr uint32_t kAgeOutIntervalMs = 5000;

// How often the scene is reassembled and offered to the panel when nothing
// has happened. The clock on the status bar has minute resolution, the feed
// age buckets at 30 s, and display::render() will not drive the glass more
// often than EPD_MIN_REFRESH_INTERVAL_MS anyway -- so once a second is
// already far finer than anything on screen can show. Input and a completed
// poll both jump the queue, so this is not a latency floor for the user.
// Halved from 1000 once the local feed made a poll cost 35 ms: the poll is
// scheduled off this tick, so a 1 s poll interval sampled once a second
// aliases into a 2 s one. Reassembling the context is the expensive half and
// twice a second is still nothing next to the hundred times a second it used
// to run at.
constexpr uint32_t kUiIntervalMs = 500;

uint32_t gNextUiMs = 0;

String gIpText;
char gSsidText[33] = "";
IPAddress gCachedIp;

// Face-down idle: debounce the IMU orientation, then hibernate the panel and
// stretch the feed poll. Any key/touch or leaving face-down wakes immediately.
bool gFaceDownLatched = false;
uint32_t gFaceDownSinceMs = 0;

void wakeFromIdle() {
    if (!gFaceDownLatched) return;
    gFaceDownLatched = false;
    gFaceDownSinceMs = 0;
    display::invalidate(true);
    power::refreshKeypadBacklight(settings::get().keypadBacklight);
    gNextPollMs = millis();
    log_i("idle: woke");
}

void updateFaceDownIdle() {
    if (!settings::faceDownIdle() || !imu::present()) {
        wakeFromIdle();
        return;
    }
    const bool down = strcmp(imu::orientation(), "face down") == 0;
    const uint32_t now = millis();
    if (!down) {
        gFaceDownSinceMs = 0;
        wakeFromIdle();
        return;
    }
    if (gFaceDownSinceMs == 0) gFaceDownSinceMs = now;
    if (!gFaceDownLatched &&
        static_cast<int32_t>(now - gFaceDownSinceMs) >=
            static_cast<int32_t>(FACE_DOWN_IDLE_MS)) {
        gFaceDownLatched = true;
        display::hibernate();
        log_i("idle: face down, hibernating panel");
    }
}

// WiFi.localIP() is a field read off the netif; toString() and WiFi.SSID()
// are the parts that allocate a String and call into esp_wifi. Comparing the
// address first means the expensive half runs on the connect edge and on a
// DHCP change, and never again.
void refreshNetworkText(bool connected) {
    const IPAddress ip = connected ? WiFi.localIP() : IPAddress();
    if (ip == gCachedIp) return;
    gCachedIp = ip;

    if (!connected) {
        gIpText = "";
        gSsidText[0] = '\0';
        return;
    }
    gIpText = ip.toString();
    strncpy(gSsidText, net::ssid(), sizeof(gSsidText) - 1);
    gSsidText[sizeof(gSsidText) - 1] = '\0';
}

void drawSplash() {
    Adafruit_GFX &d = display::gfx();
    d.setFont(nullptr);
    d.setTextColor(0x0000);
    d.setTextSize(2);
    d.setCursor(18, 120);
    d.print("ADS-B");
    d.setCursor(18, 142);
    d.print("Tracker");
    d.setTextSize(1);
    d.setCursor(18, 176);
    d.print("T-Deck Pro v1.0");
    d.setCursor(18, 188);
    d.print(adsb::providerName());
    d.setCursor(18, 208);
    d.print("Bringing up radios...");
    d.drawRect(10, 100, 220, 130, 0x0000);
}

// Our own position: the GNSS fix when we have one and the user wants it,
// otherwise the home position from config.h. Ranges and bearings are all
// measured from here, even when the plot has been panned elsewhere.
void ownPosition(double *lat, double *lon, bool *fromGnss) {
    if (ui::centreOnGnss() && gnss::hasFix()) {
        *lat = gnss::latitude();
        *lon = gnss::longitude();
        *fromGnss = true;
    } else {
        *lat = settings::homeLatitude();
        *lon = settings::homeLongitude();
        *fromGnss = false;
    }
}

// Centre of what is actually on screen: our own position plus however far the
// user has dragged the plot away from it.
void viewCentre(double ownLat, double ownLon, double *viewLat, double *viewLon,
                float *panDistNm) {
    float east = 0.0f, north = 0.0f;
    ui::panOffsetNm(&east, &north);

    geo::offsetNm(ownLat, ownLon, east, north, viewLat, viewLon);
    *panDistNm = sqrtf(east * east + north * north);
}

// The query follows what is on screen: the visible range, plus however far we
// have panned, so the circle around the view centre still reaches back to our
// own position and the list does not lose nearby traffic while panned.
uint16_t queryRadiusNm(float panDistNm) {
    uint32_t r = ui::rangeNm() + static_cast<uint32_t>(panDistNm + 0.5f);
    if (r < ADSB_QUERY_RADIUS_NM) r = ADSB_QUERY_RADIUS_NM;
    return static_cast<uint16_t>(r > 250 ? 250 : r);
}

// Hands the query to the fetch task and returns immediately. Nothing about
// the outcome is known here; collectPoll() picks it up on a later pass, which
// is the entire point of the arrangement -- the socket, the TLS handshake and
// the JSON parse no longer sit between a keypress and the panel noticing it.
void startPoll(double lat, double lon, uint16_t radiusNm) {
    // Recorded up front so a refused call still counts as "we have asked about
    // this area", instead of retriggering on every loop.
    gLastRadiusNm = radiusNm;
    gLastQueryLat = lat;
    gLastQueryLon = lon;
    gHaveQueried = true;

    if (!adsb::request(lat, lon, radiusNm)) {
        // Throttled, or the link went away between the check and here.
        gNextPollMs = millis() + adsb::minIntervalMs();
        return;
    }

    // Scheduled from the request rather than from the result, now that the
    // two are no longer the same instant. ADSB_*_POLL_INTERVAL_MS means how
    // often we hit the feed; measuring it from the answer instead makes it
    // that plus however long the answer took, plus however much of a 651 ms
    // repaint it landed in the middle of. On hardware that stretched a 1 s
    // local poll to 2.3 s -- the panel setting the feed's cadence, which is
    // exactly backwards.
    gNextPollMs = millis() + adsb::pollIntervalMs();

    // Held until the result is collected. That window is exactly the fetch,
    // and the fetch is the only genuinely CPU-bound work outside a repaint.
    power::acquireBoost();
}

// True when a fetch landed on this pass, which is what triggers the recompute
// below. Scheduling is startPoll()'s job, apart from the backoff.
bool collectPoll() {
    adsb::FetchStats stats;
    if (!adsb::collect(gTracker, &stats)) return false;
    power::releaseBoost();

    // Back off a little on failure so a dead feed does not hammer the link.
    if (stats.result != adsb::Result::Ok) {
        gNextPollMs = millis() + adsb::pollIntervalMs() * 2;
    }
    return true;
}

void refreshBattery() {
    const uint32_t now = millis();
    // Wrap-safe, like every other deadline here. Compared directly it was the
    // one that was not: at the 49.7-day millis() rollover `now` restarts near
    // zero while this deadline is still near the top of the range, so the
    // gauge would never be read again until the next reboot.
    if (static_cast<int32_t>(now - gNextBatteryMs) < 0) return;
    gNextBatteryMs = now + kBatteryIntervalMs;
    gBatteryValid = power::readBattery(&gBatteryPercent, &gBatteryMilliVolts);
}

// Free bytes and the largest contiguous block, for both heaps. The two figures
// diverging is fragmentation, which is the failure a device that runs for
// weeks actually suffers -- and the feed hands the PSRAM allocator a
// variable-size JSON arena to allocate and free about once a second, so it is
// the pattern most likely to cause it here.
void logHeap() {
    if (DIAG_HEAP_LOG_INTERVAL_MS == 0) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - gNextHeapLogMs) < 0) return;
    gNextHeapLogMs = now + DIAG_HEAP_LOG_INTERVAL_MS;

    log_i("heap: int %uk free / %uk min / %uk block | psram %uk free / %uk min "
          "/ %uk block | feed task %u B",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
          static_cast<unsigned>(
              heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
          static_cast<unsigned>(
              heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
          static_cast<unsigned>(
              heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
          static_cast<unsigned>(
              heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
          static_cast<unsigned>(adsb::taskHeadroomBytes()));
}

void formatLogTime(char *dst, size_t dstLen) {
    time_t now = time(nullptr);
    struct tm tm;
    if (wallclock::valid() && now > 1600000000 && localtime_r(&now, &tm)) {
        snprintf(dst, dstLen, "%04u-%02u-%02uT%02u:%02u:%02u",
                 static_cast<unsigned>(tm.tm_year + 1900) % 10000u,
                 static_cast<unsigned>(tm.tm_mon + 1) % 100u,
                 static_cast<unsigned>(tm.tm_mday) % 100u,
                 static_cast<unsigned>(tm.tm_hour) % 100u,
                 static_cast<unsigned>(tm.tm_min) % 100u,
                 static_cast<unsigned>(tm.tm_sec) % 100u);
        return;
    }
    snprintf(dst, dstLen, "m%lu", static_cast<unsigned long>(millis()));
}

void logTrafficSnapshot() {
    if (!settings::logEnabled() || !sdcard::mounted()) return;
    char when[24];
    formatLogTime(when, sizeof(when));
    for (size_t i = 0; i < gTracker.count(); ++i) {
        const Aircraft &a = gTracker.at(i);
        if (!a.hasPosition) continue;
        char alt[12] = "";
        if (a.altitudeKnown()) {
            snprintf(alt, sizeof(alt), "%d", static_cast<int>(a.altitudeFt));
        } else if (a.onGround()) {
            snprintf(alt, sizeof(alt), "0");
        }
        char line[160];
        snprintf(line, sizeof(line), "%s,%s,%s,%s,%.5f,%.5f,%s,%.0f,%.0f", when,
                 a.hex, a.flight, a.reg, a.lat, a.lon, alt, a.groundSpeedKt,
                 a.hasTrack ? a.trackDeg : 0.0f);
        sdcard::logTraffic(line);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("T-Deck Pro ADS-B tracker starting");

    // Before power::begin() and ui::begin() and the first poll, all of which
    // read saved choices. Backlight mode is applied after power::begin().
    settings::begin();
    adsb::setProvider(static_cast<AdsbProvider>(settings::get().provider));

    power::begin();
    power::refreshKeypadBacklight(settings::get().keypadBacklight);
    display::begin();
    display::render(0, drawSplash);

    const bool keypadOk = keypad::begin();
    const bool touchOk = touch::begin();
    const bool alsOk = als::begin();
    const bool imuOk = imu::begin();
    Serial.printf("keypad %s, touch %s, als %s, imu %s\n",
                  keypadOk ? "ok" : "MISSING", touchOk ? "ok" : "MISSING",
                  alsOk ? "ok" : "MISSING", imuOk ? "ok" : "MISSING");
    als::poll();
    power::refreshKeypadBacklight(settings::get().keypadBacklight);

    // After display::begin() so SPI is already up, and before net::begin()
    // so a wifi.txt on the card can win the association.
    sdcard::begin();
    sdcard::applyConfigFiles();

    gnss::begin();
    net::begin();
    adsb::begin();
    wallclock::begin();
    assets::begin();
    basemap::begin();
    aircraftdb::begin();
    icons::begin();
    ui::begin();

    refreshBattery();
    gNextPollMs = millis();  // poll as soon as the link comes up

    // Boot holds one boost claim. The clock stays at 240 MHz either way; this
    // just drops the counter so a later claim/release pair stays balanced.
    power::releaseBoost();

    // A blocked loop() used to sit forever: Arduino only watches the idle
    // task, and this firmware runs on the core whose idle task is exempt.
    // Subscribing loop() means a deadlock reboots in about five seconds
    // instead of leaving the last frame up until the battery dies.
    enableLoopWDT();
}

void loop() {
    // -- fast path. Everything here has to keep up with the hardware: the
    // GNSS UART ring is two kilobytes, which is half a second of NMEA at 38400
    // baud, and a keypress the user cannot feel land is a keypress they press
    // twice. Nothing below blocks for longer than a panel refresh any more --
    // the feed fetch, which used to hold this loop for a second and a half at
    // a time, now runs on a task of its own and hands back a result.
    net::poll();
    gnss::poll();

    bool input = false;
    char key = 0;
    while ((key = keypad::poll()) != 0) {
        wakeFromIdle();
        ui::handleKey(key);
        input = true;
    }

    touch::Event tap{};
    if (touch::poll(&tap)) {
        wakeFromIdle();
        ui::handleTouch(tap);
        input = true;
    }

    // -- slow path. Reassembling the context calls into esp_wifi and esp_netif
    // and hashing the scene walks all MAX_AIRCRAFT slots; at the old hundred
    // passes a second that was the firmware's largest single expense, and
    // none of it could change fast enough to matter. Input, and the poll it
    // may have triggered, still run it immediately.
    const uint32_t now = millis();
    // A landed fetch jumps the queue the same way input does: it is the one
    // thing that changes the picture, and letting it wait out the rest of a
    // tick would hand back the latency the task just saved.
    if (!input && !adsb::ready() && static_cast<int32_t>(now - gNextUiMs) < 0) {
        delay(10);
        return;
    }
    gNextUiMs = now + kUiIntervalMs;

    power::Boost boost;

    wallclock::poll();
    refreshBattery();
    settings::poll();
    als::poll();
    imu::poll();
    updateFaceDownIdle();
    power::refreshKeypadBacklight(settings::get().keypadBacklight, gFaceDownLatched);
    logHeap();

    // While face-down idle, skip the panel and stretch the feed. The panel is
    // hibernated (deep sleep, last image held) rather than merely undrawn.
    // Ageing still runs so a pocketed unit does not keep a frozen plot of
    // stale traffic for the moment it wakes.
    if (gFaceDownLatched && !input) {
        if (static_cast<int32_t>(now - gNextAgeOutMs) >= 0) {
            gNextAgeOutMs = now + kAgeOutIntervalMs;
            double lat = 0.0, lon = 0.0;
            bool fromGnss = false;
            ownPosition(&lat, &lon, &fromGnss);
            gTracker.finishUpdate(lat, lon, millis(), ui::selectedHex());
        }
        collectPoll();  // drain a landed fetch so the baton is not stuck
        if (net::connected() && !adsb::busy() &&
            static_cast<int32_t>(now - gNextPollMs) >= 0) {
            // Push the deadline out rather than fetching a view we will not
            // show. A wake resets gNextPollMs.
            gNextPollMs = now + FACE_DOWN_IDLE_POLL_MS;
        }
        sdcard::poll();
        delay(10);
        return;
    }

    // The only consumers of a GNSS position are the plot centre and, until it
    // is set, the clock. When neither wants one the module is powered down
    // rather than duty cycled, which is the whole saving rather than most of
    // it. Evaluated here rather than in the fast path because it cannot change
    // faster than a keypress.
    gnss::setNeeded(ui::centreOnGnss() || !wallclock::valid());

    double lat = 0.0, lon = 0.0;
    bool fromGnss = false;
    ownPosition(&lat, &lon, &fromGnss);

    double viewLat = lat, viewLon = lon;
    float panDistNm = 0.0f;
    viewCentre(lat, lon, &viewLat, &viewLon, &panDistNm);

    const uint16_t radius = queryRadiusNm(panDistNm);
    const bool manual = ui::consumeRefreshRequest();
    const bool rangeGrew = radius > gLastRadiusNm;
    // Panned far enough that the last query no longer covers the screen.
    const bool moved =
        gHaveQueried && geo::distanceNm(gLastQueryLat, gLastQueryLon, viewLat,
                                        viewLon) > ui::rangeNm() * 0.25;

    const bool connected = net::connected();
    refreshNetworkText(connected);

    // Collected before the next one is started, so a feed that answers inside
    // one tick can start its successor on the same pass rather than the next.
    const bool fetched = collectPoll();

    if (ui::consumePollPaceChange()) {
        // Retarget from now so a switch to Slow is felt immediately rather
        // than after whatever interval the previous pace had already queued.
        gNextPollMs = now;
    }

    if (connected && !adsb::busy() &&
        (manual || rangeGrew || moved ||
         static_cast<int32_t>(now - gNextPollMs) >= 0)) {
        startPoll(viewLat, viewLon, radius);
    }

    // Ranges and bearings are always measured from our own position, never the
    // view centre. Recomputed after every fetch, and periodically regardless
    // so stale targets retire even when the link is down -- otherwise a
    // dropped Wi-Fi connection leaves a frozen plot that still looks live.
    const bool filterChanged = ui::consumeFilterChange();
    if (fetched || filterChanged || static_cast<int32_t>(now - gNextAgeOutMs) >= 0) {
        gNextAgeOutMs = now + kAgeOutIntervalMs;
        gTracker.finishUpdate(lat, lon, millis(), ui::selectedHex());
        if (fetched) logTrafficSnapshot();
    }

    ui::Context ctx;
    ctx.tracker = &gTracker;
    ctx.ownLat = lat;
    ctx.ownLon = lon;
    ctx.ownFromGnss = fromGnss;
    ctx.gnssEnabled = GNSS_ENABLED;
    ctx.gnssFix = gnss::hasFix();
    ctx.gnssSatellites = gnss::satellites();
    ctx.gnssBaud = gnss::baud();
    ctx.wifiConnected = connected;
    ctx.networkActivity = adsb::activity();
    if (!connected) ctx.networkActivity = {};
    ctx.wifiRssi = connected ? net::rssi() : 0;
    ctx.wifiSsid = gSsidText;
    ctx.ipAddress = gIpText.c_str();

    ctx.clockValid = wallclock::localHm(&ctx.clockHour, &ctx.clockMinute);

    ctx.batteryValid = gBatteryValid;
    ctx.batteryPercent = gBatteryPercent;
    ctx.batteryMilliVolts = gBatteryMilliVolts;
    ctx.idle = gFaceDownLatched;
    ctx.keypadPresent = keypad::present();
    ctx.touchPresent = touch::present();
    ctx.lastFetchAgeMs =
        adsb::lastSuccessMs() == 0 ? UINT32_MAX : millis() - adsb::lastSuccessMs();
    ctx.lastFetch = adsb::lastStats();

    ui::setContext(ctx);
    ui::tick();
    sdcard::poll();

    delay(10);
}
