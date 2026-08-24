// T-Deck Pro ADS-B tracker.
//
// The board cannot receive 1090 MHz itself -- the SX1262 is sub-GHz only and
// the ESP32-S3 could not decode a 2 MSPS stream anyway -- so this firmware is
// a client of a public ADS-B aggregator over Wi-Fi. It polls traffic around
// the current position, keeps a local aged view of it, and plots it on the
// e-paper as a north-up radar scope with a sortable list behind it.

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>

#include "board_pins.h"
#include "config.h"
#include "core/aircraftdb.h"
#include "core/geo.h"
#include "core/settings.h"
#include "core/tracker.h"
#include "hw/clock.h"
#include "hw/gnss.h"
#include "hw/keypad.h"
#include "hw/power.h"
#include "hw/touch.h"
#include "net/adsb_source.h"
#include "net/net.h"
#include "ui/basemap.h"
#include "ui/display.h"
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
        *lat = HOME_LATITUDE;
        *lon = HOME_LONGITUDE;
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

void pollFeed(double lat, double lon, uint16_t radiusNm) {
    // Recorded up front so a rate-limited call still counts as "we have asked
    // about this area", instead of retriggering on every loop.
    gLastRadiusNm = radiusNm;
    gLastQueryLat = lat;
    gLastQueryLon = lon;
    gHaveQueried = true;

    // The handshake and the JSON parse are the only genuinely CPU-bound work
    // in the poll, and they are over in a second or two.
    power::Boost boost;

    const adsb::FetchStats stats = adsb::fetch(gTracker, lat, lon, radiusNm);
    if (stats.result == adsb::Result::RateLimited) {
        gNextPollMs = millis() + adsb::minIntervalMs();
        return;
    }
    // Back off a little on failure so a dead feed does not hammer the link.
    gNextPollMs = millis() + (stats.result == adsb::Result::Ok
                                  ? adsb::pollIntervalMs()
                                  : adsb::pollIntervalMs() * 2);
}

void refreshBattery() {
    const uint32_t now = millis();
    if (now < gNextBatteryMs) return;
    gNextBatteryMs = now + kBatteryIntervalMs;
    gBatteryValid = power::readBattery(&gBatteryPercent, &gBatteryMilliVolts);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("T-Deck Pro ADS-B tracker starting");

    // Before power::begin(), which sets the keyboard backlight, and before
    // ui::begin() and the first poll, both of which read saved choices.
    settings::begin();
    adsb::setProvider(static_cast<AdsbProvider>(settings::get().provider));

    power::begin();
    power::setKeypadBacklight(settings::get().keypadBacklight);
    display::begin();
    display::render(0, drawSplash);

    const bool keypadOk = keypad::begin();
    const bool touchOk = touch::begin();
    Serial.printf("keypad %s, touch %s\n", keypadOk ? "ok" : "MISSING",
                  touchOk ? "ok" : "MISSING");

    gnss::begin();
    net::begin();
    wallclock::begin();
    basemap::begin();
    aircraftdb::begin();
    ui::begin();

    refreshBattery();
    gNextPollMs = millis();  // poll as soon as the link comes up

    // Everything above ran at the 240 MHz boot frequency. From here on the
    // clock is raised only around a feed poll and a repaint.
    power::setBoost(false);
}

void loop() {
    // -- fast path. Everything here has to keep up with the hardware: the
    // GNSS UART ring is 256 bytes, which is 67 ms of NMEA at 38400 baud, and
    // a keypress the user cannot feel land is a keypress they press twice.
    net::poll();
    gnss::poll();

    bool input = false;
    char key = 0;
    while ((key = keypad::poll()) != 0) {
        ui::handleKey(key);
        input = true;
    }

    touch::Event tap{};
    if (touch::poll(&tap)) {
        ui::handleTouch(tap);
        input = true;
    }

    // -- slow path. Reassembling the context calls into esp_wifi and esp_netif
    // and hashing the scene walks all MAX_AIRCRAFT slots; at the old hundred
    // passes a second that was the firmware's largest single expense, and
    // none of it could change fast enough to matter. Input, and the poll it
    // may have triggered, still run it immediately.
    const uint32_t now = millis();
    if (!input && static_cast<int32_t>(now - gNextUiMs) < 0) {
        delay(10);
        return;
    }
    gNextUiMs = now + kUiIntervalMs;

    power::Boost boost;

    wallclock::poll();
    refreshBattery();
    settings::poll();

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

    bool polled = false;
    if (connected && (manual || rangeGrew || moved ||
                      static_cast<int32_t>(now - gNextPollMs) >= 0)) {
        pollFeed(viewLat, viewLon, radius);
        polled = true;
    }

    // Ranges and bearings are always measured from our own position, never the
    // view centre. Recomputed after every poll, and periodically regardless so
    // stale targets retire even when the link is down -- otherwise a dropped
    // Wi-Fi connection leaves a frozen plot that still looks live.
    if (polled || static_cast<int32_t>(now - gNextAgeOutMs) >= 0) {
        gNextAgeOutMs = now + kAgeOutIntervalMs;
        gTracker.finishUpdate(lat, lon, millis());
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
    ctx.wifiRssi = connected ? net::rssi() : 0;
    ctx.wifiSsid = gSsidText;
    ctx.ipAddress = gIpText.c_str();

    ctx.clockValid = wallclock::localHm(&ctx.clockHour, &ctx.clockMinute);

    ctx.batteryValid = gBatteryValid;
    ctx.batteryPercent = gBatteryPercent;
    ctx.batteryMilliVolts = gBatteryMilliVolts;
    ctx.keypadPresent = keypad::present();
    ctx.touchPresent = touch::present();
    ctx.lastFetchAgeMs =
        adsb::lastSuccessMs() == 0 ? UINT32_MAX : millis() - adsb::lastSuccessMs();
    ctx.lastFetch = adsb::lastStats();

    ui::setContext(ctx);
    ui::tick();

    delay(10);
}
