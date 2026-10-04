#include "ui.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "basemap.h"
#include "board_pins.h"
#include "config.h"
#include "core/filter.h"
#include "core/geo.h"
#include "core/aircraftdb.h"
#include "core/settings.h"
#include "display.h"
#include "icons.h"
#include "hw/als.h"
#include "hw/clock.h"
#include "hw/gnss.h"
#include "hw/imu.h"
#include "hw/keypad.h"
#include "hw/power.h"
#include "hw/sdcard.h"
#include "net/net.h"

namespace ui {
namespace {

// GxEPD2's colour constants, kept local so this file only needs Adafruit_GFX.
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;

// ------------------------------------------------------------- geometry ----
constexpr int16_t SCREEN_W = EPD_WIDTH;
constexpr int16_t SCREEN_H = EPD_HEIGHT;

constexpr int16_t STATUS_H = 32;
constexpr int16_t FOOTER_H = 28;
constexpr int16_t CONTENT_Y = STATUS_H;
constexpr int16_t FOOTER_Y = SCREEN_H - FOOTER_H;
constexpr int16_t CONTENT_H = FOOTER_Y - CONTENT_Y;

// The scope is a complete instrument face: header, plot, target readout, nav.
constexpr int16_t SCOPE_Y = STATUS_H;
constexpr int16_t SCOPE_H = 200;
constexpr int16_t READOUT_Y = SCOPE_Y + SCOPE_H;
constexpr int16_t READOUT_H = 60;
constexpr int16_t NAV_Y = READOUT_Y + READOUT_H;

constexpr int16_t CHAR_W = 6;   // built-in GFX font, size 1
constexpr int16_t CHAR_H = 8;
constexpr int16_t ROW_H = 40;   // readable two-line traffic cards
constexpr int16_t STATUS_ROW_H = 16;
constexpr int16_t SETTINGS_TOP = CONTENT_Y + 28;
constexpr int16_t SETTINGS_ROW_H = 36;
constexpr int SETTINGS_VISIBLE = (FOOTER_Y - SETTINGS_TOP) / SETTINGS_ROW_H;
constexpr int16_t SETTINGS_BOTTOM = SETTINGS_TOP + SETTINGS_VISIBLE * SETTINGS_ROW_H;

constexpr int16_t RADAR_CX = SCREEN_W / 2;
constexpr int16_t RADAR_CY = SCOPE_Y + SCOPE_H / 2;
constexpr int16_t RADAR_R = 88;

// Labelling every target turns the plot into mush. Nearest few plus whatever
// is selected is what you actually want to read.
constexpr int kMaxLabels = 5;
constexpr int kTapRadiusPx = 18;

// On-screen zoom / recentre buttons, clear of the outer range ring.
constexpr int16_t BTN = 28;
constexpr int16_t BTN_Y = SCOPE_Y + SCOPE_H - BTN - 5;
constexpr int16_t BTN_MINUS_X = 4;
constexpr int16_t BTN_PLUS_X = SCREEN_W - 4 - BTN;
constexpr int16_t BTN_HOME_X = 4;
constexpr int16_t BTN_HOME_Y = CONTENT_Y + 15;
constexpr int16_t BTN_FOLLOW_X = SCREEN_W - 4 - BTN;
constexpr int16_t BTN_FOLLOW_Y = CONTENT_Y + 15;

// A drag shorter than this is a tap, not a pan.
constexpr int kDragThresholdPx = 10;
// Keeps the plot from being flung somewhere the feed will never cover.
constexpr float kMaxPanNm = 200.0f;

// --------------------------------------------------------------- state -----
Context gCtx;
View gView = View::Radar;
View gPreviousView = View::Radar;

char gSelectedHex[8] = {0};
size_t gRangeIndex = RANGE_DEFAULT_INDEX;  // overwritten by begin() from NVS

// Plot centre as an offset from our own position, in nautical miles.
float gPanEastNm = 0.0f;
float gPanNorthNm = 0.0f;
// When set, each tick rewrites the pan so the selected aircraft stays at the
// plot centre. Not saved: it only means something while that target is live.
bool gFollowAircraft = false;

// Where the current touch started, so a release can tell a tap from a drag.
int16_t gTouchDownX = 0;
int16_t gTouchDownY = 0;
int gListTop = 0;
uint8_t gListOrder[MAX_AIRCRAFT];
int gListCount = 0;
bool gListFollowSelection = true;

int gSettingsTop = 0;
int gSettingsRow = 0;
int gSettingsRows = 0;
uint8_t gSettingsCategory = 0;

bool gFilterChanged = false;
bool gPollPaceChanged = false;

enum class EditField : uint8_t { None, Ssid, Pass, LocalUrl };
EditField gEditField = EditField::None;
char gEditBuf[settings::kLocalUrlMax + 1] = {0};
bool gEditShift = false;
bool gEditSym = false;

enum class SetRow : uint8_t {
    Distance = 0,
    Speed,
    Traffic,
    Sort,
    Poll,
    Feed,
    LocalUrl,
    Map,
    MapFile,
    Gnss,
    Backlight,
    Idle,
    SwapXY,
    MirrorX,
    MirrorY,
    Home,
    WifiSsid,
    WifiPass,
    TrackLog,
    System,
    Count
};

uint8_t settingCategory(SetRow row) {
    switch (row) {
        case SetRow::Distance:
        case SetRow::Speed:
        case SetRow::Traffic:
        case SetRow::Sort:
        case SetRow::Poll:
        case SetRow::TrackLog: return 0;  // traffic
        case SetRow::Map:
        case SetRow::MapFile: return 1;  // display
        case SetRow::Feed:
        case SetRow::LocalUrl:
        case SetRow::Gnss:
        case SetRow::Home:
        case SetRow::WifiSsid:
        case SetRow::WifiPass: return 2;  // connection
        default: return 3;  // device
    }
}

int firstSettingInCategory(uint8_t category) {
    for (int i = 0; i < static_cast<int>(SetRow::Count); ++i) {
        if (settingCategory(static_cast<SetRow>(i)) == category) return i;
    }
    return 0;
}

int settingAtCategoryIndex(uint8_t category, int wanted) {
    int found = 0;
    for (int i = 0; i < static_cast<int>(SetRow::Count); ++i) {
        if (settingCategory(static_cast<SetRow>(i)) != category) continue;
        if (found++ == wanted) return i;
    }
    return firstSettingInCategory(category);
}

// The diagnostics page has more rows than the panel has lines, so it scrolls.
// Kept as a row index rather than pixels so a row can never be half drawn.
// gStatusRows is what the last draw counted, which is what the scroll clamps
// against: several rows are conditional, so the total is not a constant
// anybody can write down here.
int gStatusTop = 0;
int gStatusRows = 0;

bool gRefreshRequested = false;
bool gCentreOnGnss = GNSS_CENTRE_BY_DEFAULT;
bool gMapEnabled = MAP_ENABLED_BY_DEFAULT;

void setView(View v);

// Mirrors the handful of UI choices worth surviving a reboot into the
// persisted set. The write itself is deferred and coalesced by
// settings::poll(), so calling this on every keypress is cheap.
void persist() {
    Settings &s = settings::get();
    s.rangeIndex = static_cast<uint8_t>(gRangeIndex);
    s.mapEnabled = gMapEnabled;
    s.centreOnGnss = gCentreOnGnss;
    s.provider = static_cast<uint8_t>(adsb::provider());
    settings::markDirty();
}

// ------------------------------------------------------------- helpers -----
Adafruit_GFX &g() { return display::gfx(); }

void textAt(int16_t x, int16_t y, const char *s, uint16_t colour = BLACK,
            uint8_t size = 1) {
    Adafruit_GFX &d = g();
    d.setFont(nullptr);
    d.setTextSize(size);
    d.setTextColor(colour);
    d.setCursor(x, y);
    d.print(s);
}

// Bound fixed-width text to its allocated area; editors retain the tail.
size_t textBounded(int16_t x, int16_t y, int16_t width, const char *s,
                   uint16_t colour = BLACK, uint8_t size = 1, bool tail = false) {
    char shown[SCREEN_W / CHAR_W + 1];
    const size_t capacity = width > 0 ? width / (CHAR_W * size) : 0;
    size_t count = strlen(s);
    if (count > capacity) {
        if (tail) s += count - capacity;
        count = capacity;
    }
    if (count >= sizeof(shown)) count = sizeof(shown) - 1;
    memcpy(shown, s, count);
    shown[count] = '\0';
    textAt(x, y, shown, colour, size);
    return count;
}

void textRight(int16_t xRight, int16_t y, const char *s,
               uint16_t colour = BLACK) {
    textAt(static_cast<int16_t>(xRight - strlen(s) * CHAR_W), y, s, colour);
}

void labelAt(int16_t x, int16_t y, const char *s, uint16_t colour = BLACK) {
    char upper[24];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof(upper); ++i) {
        upper[i] = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
    }
    upper[i] = '\0';
    textAt(x, y, upper, colour);
}

int selectionIndex() {
    if (!gCtx.tracker || !gSelectedHex[0]) return -1;
    return gCtx.tracker->indexOfHex(gSelectedHex);
}

const Aircraft *selected() {
    const int idx = selectionIndex();
    if (idx < 0) return nullptr;
    return &gCtx.tracker->at(static_cast<size_t>(idx));
}

void selectIndex(int idx) {
    if (!gCtx.tracker || idx < 0 ||
        idx >= static_cast<int>(gCtx.tracker->count())) {
        gSelectedHex[0] = '\0';
        gFollowAircraft = false;
        return;
    }
    strncpy(gSelectedHex, gCtx.tracker->at(static_cast<size_t>(idx)).hex,
            sizeof(gSelectedHex) - 1);
    gSelectedHex[sizeof(gSelectedHex) - 1] = '\0';
}

void clearSelection() {
    if (!gSelectedHex[0] && !gFollowAircraft) return;
    gSelectedHex[0] = '\0';
    gFollowAircraft = false;
    display::invalidate();
}

bool listBefore(uint8_t ia, uint8_t ib) {
    const Aircraft &a = gCtx.tracker->at(ia);
    const Aircraft &b = gCtx.tracker->at(ib);
    switch (static_cast<ListSort>(settings::get().listSort)) {
        case ListSort::Alt: {
            auto key = [](const Aircraft &x) -> int32_t {
                if (!x.altitudeKnown()) return INT32_MIN / 2;
                if (x.onGround()) return INT32_MIN / 2 + 1;
                return x.altitudeFt;
            };
            return key(a) > key(b);
        }
        case ListSort::Speed:
            return a.groundSpeedKt > b.groundSpeedKt;
        case ListSort::Callsign:
            return strcasecmp(a.label(), b.label()) < 0;
        case ListSort::Range:
        default:
            return a.distanceNm < b.distanceNm;
    }
}

void rebuildListOrder() {
    gListCount = gCtx.tracker ? static_cast<int>(gCtx.tracker->count()) : 0;
    for (int i = 0; i < gListCount; ++i) {
        gListOrder[i] = static_cast<uint8_t>(i);
    }
    if (gListCount < 2) return;
    if (static_cast<ListSort>(settings::get().listSort) == ListSort::Range) {
        return;  // tracker is already nearest-first
    }
    for (int i = 1; i < gListCount; ++i) {
        const uint8_t key = gListOrder[i];
        int j = i;
        while (j > 0 && listBefore(key, gListOrder[j - 1])) {
            gListOrder[j] = gListOrder[j - 1];
            --j;
        }
        gListOrder[j] = key;
    }
}

int visualIndexOfStore(int storeIdx) {
    for (int i = 0; i < gListCount; ++i) {
        if (gListOrder[i] == storeIdx) return i;
    }
    return storeIdx;
}

void moveSelection(int delta) {
    if (!gCtx.tracker || gCtx.tracker->count() == 0) return;
    gListFollowSelection = true;
    rebuildListOrder();
    const int n = gListCount;
    int vis = visualIndexOfStore(selectionIndex());
    if (vis < 0 || selectionIndex() < 0) {
        vis = (delta >= 0) ? 0 : n - 1;
    } else {
        vis += delta;
        if (vis < 0) vis = n - 1;
        if (vis >= n) vis = 0;
    }
    selectIndex(gListOrder[vis]);
}

uint16_t currentRangeNm() { return kRangeStepsNm[gRangeIndex]; }

// Signal strength as 0-4 bars.
//
// Raw dBm is far noisier than a 240x320 e-paper can usefully show: WiFi.RSSI()
// wanders several dB between reads, so displaying the number meant the frame
// changed constantly and the panel repainted about 1.4 times a second. The
// exact figure lives on the diagnostics page instead. Bar changes are given
// 3 dB of hysteresis so a signal parked on a threshold cannot flap either.
int wifiBars(int rssi) {
    static const int kThresholds[4] = {-85, -75, -65, -55};
    static int last = -1;

    int bars = 0;
    for (int i = 0; i < 4; ++i) {
        if (rssi >= kThresholds[i]) bars = i + 1;
    }
    if (last >= 0 && bars != last) {
        const int boundary = kThresholds[bars > last ? last : bars];
        int delta = rssi - boundary;
        if (delta < 0) delta = -delta;
        if (delta < 3) bars = last;
    }
    last = bars;
    return bars;
}

// GNSS fix quality as 0 (no fix), 1 (marginal) or 2 (good).
//
// Same reasoning as wifiBars(): the raw satellite count was on the status bar
// and in the scene hash, and it moves every second under an open sky, so the
// panel repainted at the 4 s floor rather than once per feed poll. Two
// satellites of hysteresis keep a constellation sitting on the threshold from
// flapping. The exact count is on the diagnostics page.
int gnssQuality(bool fix, uint32_t satellites) {
    static int last = 0;
    if (!fix) {
        last = 0;
        return 0;
    }
    int quality = satellites >= 6 ? 2 : 1;
    if (last != 0 && quality != last) {
        const uint32_t delta = satellites > 6 ? satellites - 6 : 6 - satellites;
        if (delta < 2) quality = last;
    }
    last = quality;
    return quality;
}

// Feed staleness, at a resolution that coarsens as it grows.
//
// A literal per-second read-out would have to enter the scene hash at
// per-second resolution, which would drive the panel at
// EPD_MIN_REFRESH_INTERVAL_MS for ever -- the same trap raw RSSI fell into
// above. Instead a healthy feed, polled every ADSB_POLL_INTERVAL_MS, never
// leaves the first bucket and so costs nothing, while a dying one climbs in
// steps you can actually act on.
//
// 0 = never fetched, 1 = fresh, 2 = 30-59 s, 3..62 = whole minutes, 63 = an
// hour or more.
uint8_t feedAgeBucket() {
    const uint32_t age = gCtx.lastFetchAgeMs;
    if (age == UINT32_MAX) return 0;
    if (age < 30000) return 1;
    if (age < 60000) return 2;
    const uint32_t minutes = age / 60000;
    if (minutes >= 60) return 63;
    return static_cast<uint8_t>(2 + minutes);
}

// Shares feedAgeBucket() with the scene hash so the two cannot drift apart and
// leave a stale figure frozen on the glass.
void formatFeedAge(char *out, size_t len) {
    const uint8_t bucket = feedAgeBucket();
    if (bucket == 0) {
        snprintf(out, len, "--");
    } else if (bucket == 1) {
        snprintf(out, len, "OK");
    } else if (bucket == 2) {
        snprintf(out, len, "30s");
    } else if (bucket == 63) {
        snprintf(out, len, "old");
    } else {
        snprintf(out, len, "%um", static_cast<unsigned>(bucket - 2));
    }
}

float pixelsPerNm() { return static_cast<float>(RADAR_R) / currentRangeNm(); }

bool isPanned() { return gPanEastNm != 0.0f || gPanNorthNm != 0.0f; }

void clampPan() {
    const float d = sqrtf(gPanEastNm * gPanEastNm + gPanNorthNm * gPanNorthNm);
    if (d > kMaxPanNm) {
        gPanEastNm *= kMaxPanNm / d;
        gPanNorthNm *= kMaxPanNm / d;
    }
}

// Centre of what is actually plotted: our own position, moved by however far
// the user has panned. main.cpp aims the feed query at the same point.
void plotCentre(double *lat, double *lon) {
    geo::offsetNm(gCtx.ownLat, gCtx.ownLon, gPanEastNm, gPanNorthNm, lat, lon);
}

// One keypress moves a quarter of the visible width, so panning scales with
// however far you are zoomed out.
float panStepNm() { return currentRangeNm() * 0.25f; }

// Panning, zooming and recentring all keep the same view; only what is
// plotted inside it moves. Every repaint already drives the whole 240x320
// window, so a partial is enough -- and at 651 ms against 1016 ms it is the
// difference between a scope you can walk across and one you wait on. Held
// keys still hit the every-20th-partial promotion, so ghosting is cleared
// during a burst rather than on every press of it.
void panByNm(float eastNm, float northNm) {
    // A hand pan means the user wants the plot, not the aircraft.
    gFollowAircraft = false;
    gPanEastNm += eastNm;
    gPanNorthNm += northNm;
    clampPan();
    display::invalidate();
}

void zoomIn() {
    if (gRangeIndex == 0) return;
    --gRangeIndex;
    persist();
    display::invalidate();
}

void zoomOut() {
    if (gRangeIndex + 1 >= kRangeStepCount) return;
    ++gRangeIndex;
    persist();
    display::invalidate();
}

void recentre() {
    const bool wasFollowing = gFollowAircraft;
    gFollowAircraft = false;
    if (!isPanned()) {
        if (wasFollowing) display::invalidate();
        return;
    }
    gPanEastNm = 0.0f;
    gPanNorthNm = 0.0f;
    display::invalidate();
}

const geo::Projector &ownProjector();

// Park the plot centre on the selected aircraft. The next tick keeps it
// there; this only snaps immediately so the key feels like it landed.
void updateFollow() {
    if (!gFollowAircraft) return;
    const Aircraft *a = selected();
    if (!a) {
        gFollowAircraft = false;
        return;
    }
    if (!a->hasPosition) return;
    float east = 0.0f, north = 0.0f;
    ownProjector().project(a->lat, a->lon, &east, &north);
    gPanEastNm = east;
    gPanNorthNm = north;
    clampPan();
}

void toggleFollow() {
    if (gFollowAircraft) {
        gFollowAircraft = false;
        display::invalidate();
        return;
    }
    const Aircraft *a = selected();
    if (!a || !a->hasPosition) return;
    gFollowAircraft = true;
    updateFollow();
    display::invalidate();
}

// Formats an altitude the way a controller would say it: flight levels above
// the transition altitude, feet below it.
void formatAltitude(const Aircraft &a, char *out, size_t len) {
    if (a.onGround()) {
        snprintf(out, len, "GND");
    } else if (!a.altitudeKnown()) {
        snprintf(out, len, "----");
    } else if (a.altitudeFt >= 18000) {
        snprintf(out, len, "FL%03d", static_cast<int>(a.altitudeFt / 100));
    } else {
        snprintf(out, len, "%d", static_cast<int>(a.altitudeFt));
    }
}

char verticalTrendChar(const Aircraft &a) {
    if (a.verticalRateFpm > 200) return '^';
    if (a.verticalRateFpm < -200) return 'v';
    return ' ';
}

// One projection origin per own-position change rather than one per call.
// geo::projectNm() takes a cos() in double every time it is asked, and the
// radar asks it once per target to plot, once per target to fingerprint, and
// once per target on every tap -- all about the same centre. Tracker::
// sceneHash() hoists the same way, which is what keeps the pixel the hash
// quantises to and the pixel the renderer draws on the same one.
const geo::Projector &ownProjector() {
    static double lat = 91.0, lon = 181.0;  // impossible: forces a first build
    static geo::Projector projector(0.0, 0.0);
    if (lat != gCtx.ownLat || lon != gCtx.ownLon) {
        lat = gCtx.ownLat;
        lon = gCtx.ownLon;
        projector = geo::Projector(lat, lon);
    }
    return projector;
}

// Screen position of a target on the radar. Returns false when it falls
// outside the plotted range.
bool radarPosition(const Aircraft &a, int16_t *sx, int16_t *sy) {
    if (!a.hasPosition) return false;
    float east = 0.0f, north = 0.0f;
    ownProjector().project(a.lat, a.lon, &east, &north);
    east -= gPanEastNm;
    north -= gPanNorthNm;

    const float scale = pixelsPerNm();
    const float px = east * scale;
    const float py = -north * scale;
    if (px * px + py * py > static_cast<float>(RADAR_R) * RADAR_R) return false;

    *sx = static_cast<int16_t>(lroundf(RADAR_CX + px));
    *sy = static_cast<int16_t>(lroundf(RADAR_CY + py));
    return true;
}

bool radarLatLonPosition(double lat, double lon, int16_t *sx, int16_t *sy) {
    float east = 0.0f, north = 0.0f;
    ownProjector().project(lat, lon, &east, &north);
    east -= gPanEastNm;
    north -= gPanNorthNm;

    const float scale = pixelsPerNm();
    const float px = east * scale;
    const float py = -north * scale;
    if (px * px + py * py > static_cast<float>(RADAR_R) * RADAR_R) return false;

    *sx = static_cast<int16_t>(lroundf(RADAR_CX + px));
    *sy = static_cast<int16_t>(lroundf(RADAR_CY + py));
    return true;
}

// ---------------------------------------------------------- status bar -----
// A 13x8 cell: an 11x7 body, a 2x3 terminal nub, and a fill bar scaled to
// charge. Left hollow when the gauge is silent, which reads as "unknown"
// rather than as "flat".
void drawBatteryGlyph(int16_t x, int16_t y, bool valid, uint8_t pct) {
    Adafruit_GFX &d = g();
    d.drawRect(x, y, 11, 7, BLACK);
    d.fillRect(static_cast<int16_t>(x + 11), static_cast<int16_t>(y + 2), 2, 3, BLACK);
    if (!valid) return;
    if (pct > 100) pct = 100;
    const int16_t w = static_cast<int16_t>((pct * 9 + 50) / 100);
    if (w > 0) {
        d.fillRect(static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 1), w, 5,
                   BLACK);
    }
}

const char *viewTitle() {
    switch (gView) {
        case View::Radar: return "RADAR";
        case View::List: return "FLIGHTS";
        case View::Detail: return "AIRCRAFT";
        case View::Status: return "SYSTEM";
        case View::Settings: return "SETUP";
    }
    return "TRAFFIC";
}

// Each arrow has an inverse background while transferring; idle arrows remain
// visible so the pair reads as a network indicator even between polls.
void drawActivityArrow(int16_t x, bool up, bool active) {
    Adafruit_GFX &d = g();
    if (active) d.fillRect(x, 5, 8, 14, BLACK);
    const uint16_t ink = active ? WHITE : BLACK;
    const int16_t cx = x + 3;
    const int16_t tip = up ? 8 : 15;
    const int16_t wing = up ? 11 : 12;
    d.drawFastVLine(cx, 8, 8, ink);
    d.drawLine(cx - 2, wing, cx, tip, ink);
    d.drawLine(cx + 2, wing, cx, tip, ink);
}

void drawStatusBar() {
    Adafruit_GFX &d = g();
    char buf[24];
    textBounded(6, 3, 174, viewTitle(), BLACK, 2);

    if (gView == View::Radar || gView == View::List) {
        snprintf(buf, sizeof(buf), "R %u%s",
                 static_cast<unsigned>(geo::displayDistance(currentRangeNm())),
                 geo::distanceUnitLabel());
        textAt(6, 22, buf);
    } else {
        textAt(6, 22, gCtx.wifiConnected ? "WI-FI CONNECTED" : "WI-FI OFFLINE");
    }

    drawActivityArrow(166, true, gCtx.networkActivity.upload);
    drawActivityArrow(175, false, gCtx.networkActivity.download);
    formatFeedAge(buf, sizeof(buf));
    if (gCtx.idle) snprintf(buf, sizeof(buf), "IDLE");
    char health[24];
    snprintf(health, sizeof(health), "%s %.8s", gCtx.wifiConnected ? "FEED" : "OFFLINE", buf);
    textRight(SCREEN_W - 6, 22, health);
    drawBatteryGlyph(190, 8, gCtx.batteryValid, gCtx.batteryPercent);
    if (gCtx.batteryValid) snprintf(buf, sizeof(buf), "%u%%", gCtx.batteryPercent);
    else snprintf(buf, sizeof(buf), "--%%");
    textRight(SCREEN_W - 6, 8, buf);
    d.drawFastHLine(0, STATUS_H - 1, SCREEN_W, BLACK);
}

void drawFooterFrame() {
    g().drawFastHLine(0, FOOTER_Y, SCREEN_W, BLACK);
}

void drawFooterLines(const char *line1, const char *line2) {
    drawFooterFrame();
    if (line1) textAt(4, FOOTER_Y + 8, line1);
    if (line2) textAt(4, FOOTER_Y + 15, line2);
}

void drawNavBar(bool secondary = false) {
    Adafruit_GFX &d = g();
    drawFooterFrame();
    if (secondary) {
        d.drawRect(4, NAV_Y + 3, 64, FOOTER_H - 6, BLACK);
        textAt(14, NAV_Y + 10, "< BACK");
        textRight(SCREEN_W - 6, NAV_Y + 10, gView == View::Detail ? "J/K NEXT TARGET" : "W/S SCROLL");
        return;
    }
    constexpr int16_t kTabW = SCREEN_W / 3;
    const char *labels[3] = {"SCOPE", "LIST", "SETUP"};
    const View views[3] = {View::Radar, View::List, View::Settings};
    for (int i = 0; i < 3; ++i) {
        const int16_t x = static_cast<int16_t>(i * kTabW);
        if (views[i] == gView) d.fillRect(x + 3, NAV_Y + 3, kTabW - 6, FOOTER_H - 6, BLACK);
        textAt(x + (kTabW - strlen(labels[i]) * CHAR_W) / 2, NAV_Y + 10,
               labels[i], views[i] == gView ? WHITE : BLACK);
    }
}

void drawTargetReadout() {
    g().drawFastHLine(0, READOUT_Y, SCREEN_W, BLACK);
    constexpr int16_t kTextWidth = SCREEN_W - 10;
    char line[64];
    const Aircraft *a = selected();
    if (!a) {
        const unsigned count = gCtx.tracker
                                   ? static_cast<unsigned>(gCtx.tracker->count())
                                   : 0;
        snprintf(line, sizeof(line), "%u AIRCRAFT", count);
        textBounded(6, READOUT_Y + 6, kTextWidth, line, BLACK, 2);
        snprintf(line, sizeof(line), "%s / %s", filter::name(), adsb::providerName());
        textBounded(6, READOUT_Y + 29, kTextWidth, line);
        textBounded(6, READOUT_Y + 45, kTextWidth,
                    !gCtx.wifiConnected ? "WAITING FOR WI-FI" : "TAP AIRCRAFT / J-K TO SELECT");
        return;
    }

    const int16_t callsignWidth = kTextWidth - 54;
    textBounded(6, READOUT_Y + 5, callsignWidth, a->label(), BLACK, 2);
    textRight(SCREEN_W - 6, READOUT_Y + 9, a->emergency ? "ALERT" : (gFollowAircraft ? "FOLLOW" : "DETAIL >"));

    char alt[12];
    formatAltitude(*a, alt, sizeof(alt));
    snprintf(line, sizeof(line), "ALT %s%c  SPD %3.0f%s", alt, verticalTrendChar(*a),
             geo::displaySpeed(a->groundSpeedKt), geo::speedUnitLabel());
    textBounded(6, READOUT_Y + 29, kTextWidth, line);
    if (a->hasPosition) {
        snprintf(line, sizeof(line), "%s  %4.1f%s  %03.0f %s",
                 a->type[0] ? a->type : "----", geo::displayDistance(a->distanceNm),
                 geo::distanceUnitLabel(), a->bearingDeg,
                 geo::compassPoint(a->bearingDeg));
    } else {
        snprintf(line, sizeof(line), "%s  NO POSITION",
                 a->type[0] ? a->type : "----");
    }
    textBounded(6, READOUT_Y + 45, kTextWidth, line);
}

// --------------------------------------------------------- radar view ------
void drawAircraftMarker(int16_t x, int16_t y, const Aircraft &a, bool isSelected) {
    Adafruit_GFX &d = g();

    if (a.hasTrack) {
        const float rad = a.trackDeg * static_cast<float>(M_PI) / 180.0f;
        const float ux = sinf(rad);   // along-track, screen coords
        const float uy = -cosf(rad);
        const float rx = -uy;         // starboard
        const float ry = ux;

        const float tailX = x - 3.0f * ux;
        const float tailY = y - 3.0f * uy;
        d.fillTriangle(static_cast<int16_t>(lroundf(x + 5.0f * ux)),
                       static_cast<int16_t>(lroundf(y + 5.0f * uy)),
                       static_cast<int16_t>(lroundf(tailX + 3.0f * rx)),
                       static_cast<int16_t>(lroundf(tailY + 3.0f * ry)),
                       static_cast<int16_t>(lroundf(tailX - 3.0f * rx)),
                       static_cast<int16_t>(lroundf(tailY - 3.0f * ry)), BLACK);
        // Short leader line in the direction of travel, length scaled by speed
        // so fast traffic reads as fast at a glance.
        const float lead = 6.0f + a.groundSpeedKt / 60.0f;
        d.drawLine(static_cast<int16_t>(lroundf(x + 5.0f * ux)),
                   static_cast<int16_t>(lroundf(y + 5.0f * uy)),
                   static_cast<int16_t>(lroundf(x + (5.0f + lead) * ux)),
                   static_cast<int16_t>(lroundf(y + (5.0f + lead) * uy)), BLACK);
    } else {
        d.fillCircle(x, y, 3, BLACK);
    }

    if (a.emergency) {
        d.drawRect(x - 7, y - 7, 15, 15, BLACK);
    }
    if (isSelected) {
        d.drawCircle(x, y, 9, BLACK);
        d.drawCircle(x, y, 10, BLACK);
    }
}

void drawAircraftTrail(const Aircraft &a) {
    if (a.trailCount < 2) return;

    Adafruit_GFX &d = g();
    bool havePrev = false;
    int16_t prevX = 0, prevY = 0;
    const uint8_t first =
        (a.trailCount == AIRCRAFT_TRAIL_POINTS) ? a.trailNext : 0;

    for (uint8_t i = 0; i < a.trailCount; ++i) {
        const uint8_t slot =
            static_cast<uint8_t>((first + i) % AIRCRAFT_TRAIL_POINTS);
        int16_t x = 0, y = 0;
        const bool visible =
            radarLatLonPosition(a.trail[slot].lat, a.trail[slot].lon, &x, &y);
        if (visible) {
            d.fillCircle(x, y, 1, BLACK);
            if (havePrev) d.drawLine(prevX, prevY, x, y, BLACK);
        }
        havePrev = visible;
        prevX = x;
        prevY = y;
    }
}

// Zoom out / zoom in, plus a recentre button that only appears once the plot
// has been dragged away from our own position.
void drawRadarButtons() {
    Adafruit_GFX &d = g();

    d.drawRect(BTN_MINUS_X, BTN_Y, BTN, BTN, BLACK);
    d.drawFastHLine(BTN_MINUS_X + 5, BTN_Y + BTN / 2, BTN - 10, BLACK);

    d.drawRect(BTN_PLUS_X, BTN_Y, BTN, BTN, BLACK);
    d.drawFastHLine(BTN_PLUS_X + 5, BTN_Y + BTN / 2, BTN - 10, BLACK);
    d.drawFastVLine(BTN_PLUS_X + BTN / 2, BTN_Y + 5, BTN - 10, BLACK);

    if (isPanned()) {
        d.drawRect(BTN_HOME_X, BTN_HOME_Y, BTN, BTN, BLACK);
        const int16_t cx = BTN_HOME_X + BTN / 2;
        const int16_t cy = BTN_HOME_Y + BTN / 2;
        d.drawFastHLine(cx - 6, cy, 13, BLACK);
        d.drawFastVLine(cx, cy - 6, 13, BLACK);
        d.fillCircle(cx, cy, 2, BLACK);
    }

    // Lock-on: shown once a positioned target is selected, filled while the
    // plot is chasing it. Top-right, opposite the recentre crosshair.
    const Aircraft *followed = selected();
    if (gFollowAircraft || (followed && followed->hasPosition)) {
        const uint16_t ink = gFollowAircraft ? WHITE : BLACK;
        if (gFollowAircraft) {
            d.fillRect(BTN_FOLLOW_X, BTN_FOLLOW_Y, BTN, BTN, BLACK);
        } else {
            d.drawRect(BTN_FOLLOW_X, BTN_FOLLOW_Y, BTN, BTN, BLACK);
        }
        const int16_t cx = BTN_FOLLOW_X + BTN / 2;
        const int16_t cy = BTN_FOLLOW_Y + BTN / 2;
        d.drawCircle(cx, cy, 5, ink);
        d.drawFastHLine(cx - 3, cy, 7, ink);
        d.drawFastVLine(cx, cy - 3, 7, ink);
    }
}

bool inButton(int16_t x, int16_t y, int16_t bx, int16_t by) {
    // Generous by 4 px on every side: 20 px squares are small for a fingertip.
    return x >= bx - 4 && x < bx + BTN + 4 && y >= by - 4 && y < by + BTN + 4;
}

void drawRadar() {
    Adafruit_GFX &d = g();
    char buf[24];

    // Basemap first: everything else has to read on top of it.
    if (gMapEnabled && basemap::available()) {
        double mapLat = 0.0, mapLon = 0.0;
        plotCentre(&mapLat, &mapLon);
        basemap::draw(mapLat, mapLon, pixelsPerNm(), RADAR_CX, RADAR_CY, RADAR_R,
                      currentRangeNm());
    }

    // Aircraft traffic displays favour an uncluttered half/full range scale.
    for (int i = 1; i <= 2; ++i) {
        const int16_t r = static_cast<int16_t>(RADAR_R * i / 2);
        d.drawCircle(RADAR_CX, RADAR_CY, r, BLACK);
        snprintf(buf, sizeof(buf), "%.0f", geo::displayDistance(
                                               currentRangeNm() * i / 2.0f));
        textAt(static_cast<int16_t>(RADAR_CX + 3),
               static_cast<int16_t>(RADAR_CY - r - 1), buf);
    }

    // Cardinal ticks.
    d.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_R - 4, 8, BLACK);
    d.drawFastVLine(RADAR_CX, RADAR_CY + RADAR_R - 4, 8, BLACK);
    d.drawFastHLine(RADAR_CX - RADAR_R - 4, RADAR_CY, 8, BLACK);
    d.drawFastHLine(RADAR_CX + RADAR_R - 4, RADAR_CY, 8, BLACK);
    textAt(RADAR_CX - 2, SCOPE_Y + 2, "N");
    textAt(RADAR_CX - 2, RADAR_CY + RADAR_R + 2, "S");
    textAt(RADAR_CX + RADAR_R + 6, RADAR_CY - 3, "E");
    textAt(RADAR_CX - RADAR_R - 12, RADAR_CY - 3, "W");

    // Own position. Once panned this is no longer the middle of the plot, so
    // draw it where it actually falls -- and only if it is still on screen.
    const float scale = pixelsPerNm();
    const int16_t ox = static_cast<int16_t>(lroundf(RADAR_CX - gPanEastNm * scale));
    const int16_t oy = static_cast<int16_t>(lroundf(RADAR_CY + gPanNorthNm * scale));
    if (ox >= 0 && ox < SCREEN_W && oy >= CONTENT_Y && oy < FOOTER_Y) {
        d.drawLine(ox - 4, oy, ox + 4, oy, BLACK);
        d.drawLine(ox, oy - 4, ox, oy + 4, BLACK);
        d.fillCircle(ox, oy, 2, BLACK);
        if (isPanned()) d.drawCircle(ox, oy, 6, BLACK);
    }

    if (!gCtx.tracker) {
        drawRadarButtons();
        drawTargetReadout();
        drawNavBar();
        return;
    }

    const int selIdx = selectionIndex();
    int plotted = 0;

    if (selIdx >= 0 && selIdx < static_cast<int>(gCtx.tracker->count())) {
        drawAircraftTrail(gCtx.tracker->at(static_cast<size_t>(selIdx)));
    }

    // Draw symbols first so the selected label can claim space before the
    // remaining labels are placed.
    for (size_t i = 0; i < gCtx.tracker->count(); ++i) {
        const Aircraft &a = gCtx.tracker->at(i);
        int16_t x = 0, y = 0;
        if (!radarPosition(a, &x, &y)) continue;
        ++plotted;
        drawAircraftMarker(x, y, a, static_cast<int>(i) == selIdx);
    }

    struct LabelBox { int16_t x1, y1, x2, y2; };
    LabelBox boxes[kMaxLabels + 1];
    int labelled = 0;
    auto placeLabel = [&](int idx, bool selectedLabel) {
        if (idx < 0 || idx >= static_cast<int>(gCtx.tracker->count())) return;
        const int labelLimit = kMaxLabels + (selIdx >= 0 ? 1 : 0);
        if (!selectedLabel && labelled >= labelLimit) return;
        const Aircraft &a = gCtx.tracker->at(static_cast<size_t>(idx));
        int16_t x = 0, y = 0;
        if (!radarPosition(a, &x, &y)) return;
        char alt[12];
        formatAltitude(a, alt, sizeof(alt));
        const int16_t widest = static_cast<int16_t>(
            strlen(a.label()) > strlen(alt) ? strlen(a.label()) : strlen(alt));
        int16_t lx = static_cast<int16_t>(x + 7);
        if (lx + widest * CHAR_W > SCREEN_W - 2)
            lx = static_cast<int16_t>(x - 7 - widest * CHAR_W);
        if (lx < 1) lx = 1;
        int16_t ly = static_cast<int16_t>(y - 8);
        if (ly < SCOPE_Y) ly = SCOPE_Y;
        if (ly + 17 >= READOUT_Y) ly = READOUT_Y - 18;
        LabelBox candidate{lx, ly, static_cast<int16_t>(lx + widest * CHAR_W),
                           static_cast<int16_t>(ly + 17)};
        if (!selectedLabel) {
            for (int b = 0; b < labelled; ++b) {
                if (candidate.x1 <= boxes[b].x2 && candidate.x2 >= boxes[b].x1 &&
                    candidate.y1 <= boxes[b].y2 && candidate.y2 >= boxes[b].y1)
                    return;
            }
        }
        boxes[labelled++] = candidate;
        textAt(lx, ly, a.label());
        textAt(lx, static_cast<int16_t>(ly + 9), alt);
    };
    placeLabel(selIdx, true);
    for (size_t i = 0; i < gCtx.tracker->count(); ++i) {
        if (static_cast<int>(i) != selIdx) placeLabel(static_cast<int>(i), false);
    }

    // The range now reads out in the status bar, so all this corner has left
    // to say is what the plot is not showing you -- and there are two quite
    // different reasons for that. One is traffic past the outer ring, which
    // zooming out would bring in. The other never reported a position at all,
    // which no amount of zooming will fix. Counting them together made a feed
    // full of positionless targets read as a sky full of traffic just off the
    // edge of the screen.
    const int total = static_cast<int>(gCtx.tracker->count());
    const int noPosition =
        total - static_cast<int>(gCtx.tracker->positionCount());
    const int beyondRange = total - plotted - noPosition;
    // Its own buffer rather than the shared one: both counts are bounded
    // by MAX_AIRCRAFT, but the compiler cannot know that and sizes the
    // worst case at two ten-digit integers.
    char corner[40] = {0};
    if (beyondRange > 0 && noPosition > 0) {
        snprintf(corner, sizeof(corner), "+%d out +%d nopos", beyondRange,
                 noPosition);
    } else if (beyondRange > 0) {
        snprintf(corner, sizeof(corner), "+%d out", beyondRange);
    } else if (noPosition > 0) {
        snprintf(corner, sizeof(corner), "+%d nopos", noPosition);
    }
    if (corner[0]) textRight(SCREEN_W - 2, CONTENT_Y + 2, corner);

    drawRadarButtons();

    drawTargetReadout();
    drawNavBar();
}

// ---------------------------------------------------------- list view ------
void drawList() {
    Adafruit_GFX &d = g();
    char buf[64];

    if (!gCtx.tracker || gCtx.tracker->count() == 0) {
        const char *msg = !gCtx.wifiConnected ? "Waiting for Wi-Fi..."
                          : (gCtx.lastFetch.received > 0 ? "All traffic filtered."
                                                         : "No traffic in range.");
        textAt(8, CONTENT_Y + 64, "NO AIRCRAFT", BLACK, 2);
        textBounded(8, CONTENT_Y + 92, SCREEN_W - 16, msg);
        textAt(8, CONTENT_Y + 112, "R refresh / O settings");
        drawNavBar();
        return;
    }

    rebuildListOrder();
    const int rows = CONTENT_H / ROW_H;
    const int total = gListCount;
    const int selStore = selectionIndex();
    const int selVis = visualIndexOfStore(selStore);

    if (gListFollowSelection && selVis >= 0 && selStore >= 0) {
        if (selVis < gListTop) gListTop = selVis;
        if (selVis >= gListTop + rows) gListTop = selVis - rows + 1;
    }
    if (gListTop > total - rows) gListTop = total - rows;
    if (gListTop < 0) gListTop = 0;

    for (int r = 0; r < rows; ++r) {
        const int vis = gListTop + r;
        if (vis >= total) break;
        const int idx = gListOrder[vis];
        const Aircraft &a = gCtx.tracker->at(static_cast<size_t>(idx));
        const int16_t y = static_cast<int16_t>(CONTENT_Y + r * ROW_H);
        const bool isSelected = (idx == selStore);

        if (isSelected) d.fillRect(3, y + 2, SCREEN_W - 9, ROW_H - 4, BLACK);

        char alt[12];
        formatAltitude(a, alt, sizeof(alt));

        const uint16_t ink = isSelected ? WHITE : BLACK;
        textBounded(8, y + 6, 132, a.label(), ink, 2);
        snprintf(buf, sizeof(buf), "%s%c", alt, verticalTrendChar(a));
        textRight(SCREEN_W - 10, y + 10, buf, ink);
        if (a.hasPosition) {
            snprintf(buf, sizeof(buf), "%-5s  %3.0f%s   %4.1f%s   %03.0f",
                     a.type[0] ? a.type : "----", geo::displaySpeed(a.groundSpeedKt),
                     geo::speedUnitLabel(), geo::displayDistance(a.distanceNm),
                     geo::distanceUnitLabel(), a.bearingDeg);
        } else {
            snprintf(buf, sizeof(buf), "%-5s  %3.0f%s   NO POSITION",
                     a.type[0] ? a.type : "----", geo::displaySpeed(a.groundSpeedKt),
                     geo::speedUnitLabel());
        }
        textBounded(8, y + 27, SCREEN_W - 18, buf, ink);
        if (!isSelected) d.drawFastHLine(8, y + ROW_H - 1, SCREEN_W - 18, BLACK);
    }

    if (total > rows) {
        const int16_t trackTop = CONTENT_Y;
        const int16_t trackH = static_cast<int16_t>(rows * ROW_H);
        const int16_t barH = static_cast<int16_t>(
            trackH * rows / total > 4 ? trackH * rows / total : 4);
        const int16_t barY = static_cast<int16_t>(
            trackTop + (trackH - barH) * gListTop / (total - rows));
        d.fillRect(SCREEN_W - 3, barY, 3, barH, BLACK);
    }

    drawNavBar();
}

// -------------------------------------------------------- detail view ------
void drawDetail() {
    const Aircraft *a = selected();
    if (!a) {
        textAt(8, CONTENT_Y + 64, "NO SELECTION", BLACK, 2);
        textAt(8, CONTENT_Y + 92, "Choose an aircraft on radar or list.");
        drawNavBar(true);
        return;
    }
    Adafruit_GFX &d = g();
    char buf[64];
    aircraftdb::Details db;
    const bool known = aircraftdb::details(a->hex, &db);
    const char *type = a->type[0] ? a->type : (known && db.type[0] ? db.type : "----");
    const char *reg = a->reg[0] ? a->reg : (known && db.reg[0] ? db.reg : "--");
    textBounded(8, CONTENT_Y + 7, SCREEN_W - 16, a->label(), BLACK, 2);
    snprintf(buf, sizeof(buf), "%s  /  %s  /  %s", reg, type, a->hex);
    textBounded(8, CONTENT_Y + 29, SCREEN_W - 16, buf);
    textBounded(8, CONTENT_Y + 44, SCREEN_W - 16,
                known && db.op[0] ? db.op : "Operator not reported");

    const int16_t metricY = CONTENT_Y + 62;
    d.drawFastHLine(8, metricY, SCREEN_W - 16, BLACK);
    d.drawFastVLine(SCREEN_W / 2, metricY + 5, 43, BLACK);
    textAt(8, metricY + 8, "ALTITUDE");
    textAt(SCREEN_W / 2 + 8, metricY + 8, "GROUND SPEED");
    char alt[12];
    formatAltitude(*a, alt, sizeof(alt));
    textBounded(8, metricY + 24, SCREEN_W / 2 - 16, alt, BLACK, 2);
    snprintf(buf, sizeof(buf), "%.0f", geo::displaySpeed(a->groundSpeedKt));
    const size_t speedChars = textBounded(SCREEN_W / 2 + 8, metricY + 24,
                                         SCREEN_W / 2 - 40, buf, BLACK, 2);
    textAt(SCREEN_W / 2 + 8 + speedChars * CHAR_W * 2, metricY + 31,
           geo::speedUnitLabel());
    d.drawFastHLine(8, metricY + 51, SCREEN_W - 16, BLACK);

    int16_t y = metricY + 61;
    // Keep the original type-specific silhouette beside the flight data.
    // Feed type takes priority; the database supplies it for local feeds.
    int16_t iconW = 0, iconH = 0;
    const bool hasIcon = icons::size(type, &iconW, &iconH) &&
                         iconW <= 88 && iconH <= 104;
    if (hasIcon) {
        icons::draw(type, SCREEN_W - 8 - iconW, y);
    }
    const int16_t valueX = hasIcon ? 56 : 86;
    const int16_t valueWidth = hasIcon ? SCREEN_W - 16 - iconW - valueX
                                      : SCREEN_W - 94;
    auto row = [&](const char *label, const char *value) {
        labelAt(8, y, label);
        textBounded(valueX, y, valueWidth, value);
        y += 16;
    };
    if (a->hasPosition) {
        snprintf(buf, sizeof(buf), "%.1f %s",
                 geo::displayDistance(a->distanceNm), geo::distanceUnitLabel());
    } else {
        snprintf(buf, sizeof(buf), "not reported");
    }
    row("Range", buf);
    if (a->hasTrack) snprintf(buf, sizeof(buf), "%03.0f %s", a->trackDeg, geo::compassPoint(a->trackDeg));
    else snprintf(buf, sizeof(buf), "--");
    row("Track", buf);
    snprintf(buf, sizeof(buf), "%+d fpm", static_cast<int>(a->verticalRateFpm));
    row("Climb", buf);
    row("Squawk", a->squawk[0] ? a->squawk : "--");
    if (a->hasPosition) {
        snprintf(buf, sizeof(buf), "%03.0f %s", a->bearingDeg, geo::compassPoint(a->bearingDeg));
        row("Brg", buf);
        snprintf(buf, sizeof(buf), "%.0f s ago", a->seenPosSec);
    } else {
        row("Brg", "--");
        snprintf(buf, sizeof(buf), "--");
    }
    row("Age", buf);
    if (a->hasPosition) {
        snprintf(buf, sizeof(buf), "POS %.4f, %.4f", a->lat, a->lon);
        textBounded(8, FOOTER_Y - 32, SCREEN_W - 16, buf);
    }
    // Reserve one bottom strip for aircraft identity or an emergency banner.
    if (a->emergency) {
        d.fillRect(4, FOOTER_Y - 23, SCREEN_W - 8, 19, BLACK);
        textAt(8, FOOTER_Y - 17, "EMERGENCY / SPECIAL SQUAWK", WHITE);
    } else {
        snprintf(buf, sizeof(buf), "%s%s%u", known && db.desc[0] ? db.desc : type,
                 known && db.year ? " / " : "", known && db.year ? db.year : 0);
        // Do not append a synthetic year when the database has none.
        if (!(known && db.year)) snprintf(buf, sizeof(buf), "%s", known && db.desc[0] ? db.desc : type);
        textBounded(8, FOOTER_Y - 17, SCREEN_W - 16, buf);
    }
    drawNavBar(true);
}

// -------------------------------------------------------- status view ------
void drawStatus() {
    char buf[64];
    const int16_t heading = CONTENT_Y + 6;

    textAt(8, heading, "DIAGNOSTICS");
    textRight(SCREEN_W - 8, heading, "W/S SCROLL");

    // The heading stays put and the rows scroll under it.
    constexpr int16_t kRowH = STATUS_ROW_H;
    const int16_t top = CONTENT_Y + 28;
    const int visible = (FOOTER_Y - top) / kRowH;

    // Every row is offered; only the ones inside the window are drawn. Doing
    // it this way rather than skipping the work means the count at the end is
    // the true total, conditional rows included, which is what the scroll
    // needs to know how far down it may go.
    int index = 0;
    auto row = [&](const char *label, const char *value) {
        const int i = index++;
        if (i < gStatusTop || i >= gStatusTop + visible) return;
        const int16_t y = static_cast<int16_t>(top + (i - gStatusTop) * kRowH);
        labelAt(8, y + 3, label);
        textBounded(80, y + 3, SCREEN_W - 88, value);
    };

    row("Firmware", "adsb 0.1");
    row("Feed", adsb::providerName());

    const adsb::FetchStats &f = gCtx.lastFetch;
    switch (f.result) {
        case adsb::Result::Ok: {
            // A local receiver serves everything it hears, so the count it
            // sent and the count we kept are different numbers worth seeing --
            // as is the count the store had no room for, which is the only
            // warning that MAX_AIRCRAFT, rather than the antenna, is now the
            // limit on what you can see.
            // snprintf() reports what it would have written, so the offset
            // is clamped before it is used as one: an unclamped overrun would
            // turn sizeof(buf) - n into a very large size_t.
            size_t n = static_cast<size_t>(snprintf(buf, sizeof(buf), "ok %u ac",
                                                    static_cast<unsigned>(f.stored)));
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            if (f.filtered > 0) {
                n += static_cast<size_t>(
                    snprintf(buf + n, sizeof(buf) - n, " -%u far",
                             static_cast<unsigned>(f.filtered)));
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            }
            if (f.dropped > 0) {
                snprintf(buf + n, sizeof(buf) - n, " -%u full",
                         static_cast<unsigned>(f.dropped));
            }
            break;
        }
        case adsb::Result::HttpError:
            snprintf(buf, sizeof(buf), "HTTP %d", f.httpStatus);
            break;
        case adsb::Result::ParseError:
            snprintf(buf, sizeof(buf), "parse error");
            break;
        case adsb::Result::NotConnected:
            snprintf(buf, sizeof(buf), "no network");
            break;
    }
    row("Last poll", buf);

    if (gCtx.lastFetchAgeMs == UINT32_MAX) {
        snprintf(buf, sizeof(buf), "never");
    } else {
        snprintf(buf, sizeof(buf), "%lu s ago",
                 static_cast<unsigned long>(gCtx.lastFetchAgeMs / 1000));
    }
    row("Poll age", buf);

    snprintf(buf, sizeof(buf), "%lu ms", static_cast<unsigned long>(f.durationMs));
    row("Poll time", buf);

    // The fetch runs on its own task now, and a TLS handshake is the deepest
    // thing this firmware does. An overflow there would look like a reboot
    // with no other explanation, so the headroom is worth a line.
    const uint32_t headroom = adsb::taskHeadroomBytes();
    if (headroom == 0) {
        snprintf(buf, sizeof(buf), "NOT RUNNING");
    } else if (adsb::busy()) {
        snprintf(buf, sizeof(buf), "fetching");
    } else {
        snprintf(buf, sizeof(buf), "idle, %u B free",
                 static_cast<unsigned>(headroom));
    }
    row("Feed task", buf);

    row("SSID", gCtx.wifiConnected ? gCtx.wifiSsid : "not connected");
    row("IP", gCtx.wifiConnected ? gCtx.ipAddress : "--");

    snprintf(buf, sizeof(buf), "%d dBm", gCtx.wifiRssi);
    row("RSSI", buf);

    if (!gCtx.gnssEnabled) {
        snprintf(buf, sizeof(buf), "disabled");
    } else if (gCtx.gnssFix) {
        snprintf(buf, sizeof(buf), "fix, %u sat",
                 static_cast<unsigned>(gCtx.gnssSatellites));
    } else {
        snprintf(buf, sizeof(buf), "searching");
    }
    row("GNSS", buf);

    snprintf(buf, sizeof(buf), "%lu baud", static_cast<unsigned long>(gCtx.gnssBaud));
    row("GNSS link", buf);

    row("Map", gMapEnabled ? basemap::status() : "off");
    row("SD", sdcard::status());
    row("Shot", sdcard::lastShot());

    uint8_t ch = 0, cm = 0, cs = 0;
    if (wallclock::localHms(&ch, &cm, &cs)) {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s", static_cast<unsigned>(ch),
                 static_cast<unsigned>(cm), static_cast<unsigned>(cs),
                 wallclock::fromNtp() ? "NTP" : "GNSS");
    } else {
        snprintf(buf, sizeof(buf), "not set");
    }
    row("Clock", buf);

    snprintf(buf, sizeof(buf), "%.4f", gCtx.ownLat);
    row("Own lat", buf);
    snprintf(buf, sizeof(buf), "%.4f", gCtx.ownLon);
    row("Own lon", buf);
    row("Position", gCtx.ownFromGnss ? "GNSS"
                    : (settings::get().homeOverride ? "saved home" : "config"));

    if (isPanned()) {
        snprintf(buf, sizeof(buf), "%+.0fE %+.0fN nm", gPanEastNm, gPanNorthNm);
    } else {
        snprintf(buf, sizeof(buf), "centred");
    }
    row("Pan", buf);

    snprintf(buf, sizeof(buf), "%.0f %s",
             geo::displayDistance(currentRangeNm()), geo::distanceUnitLabel());
    row("Range", buf);

    if (gCtx.batteryValid) {
        snprintf(buf, sizeof(buf), "%u%%  %u mV",
                 static_cast<unsigned>(gCtx.batteryPercent),
                 static_cast<unsigned>(gCtx.batteryMilliVolts));
    } else {
        snprintf(buf, sizeof(buf), "gauge silent");
    }
    row("Battery", buf);

    // The chip's own percentage, which is wrong until its design capacity
    // matches the 1400 mAh cell. The row above is what the status bar shows.
    if (power::gaugePercent() > 100) {
        snprintf(buf, sizeof(buf), "no reading");
    } else {
        snprintf(buf, sizeof(buf), "%u%% of %u mAh",
                 static_cast<unsigned>(power::gaugePercent()),
                 static_cast<unsigned>(power::designMilliAmpHours()));
    }
    row("Gauge", buf);

    // Ambient light also drives auto keypad backlight; the number lives here.
    // IMU attitude drives face-down idle, and the vector lives here. Both are
    // polled from main.cpp; the values are whatever the last sample was.
    float lux = 0.0f;
    if (!als::present()) {
        snprintf(buf, sizeof(buf), "silent");
    } else if (!als::lux(&lux)) {
        snprintf(buf, sizeof(buf), "warming");
    } else if (lux < 10.0f) {
        snprintf(buf, sizeof(buf), "%.1f lx", lux);
    } else {
        snprintf(buf, sizeof(buf), "%.0f lx", lux);
    }
    row("ALS", buf);

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (!imu::present()) {
        snprintf(buf, sizeof(buf), "silent");
    } else if (!imu::acceleration(&ax, &ay, &az)) {
        snprintf(buf, sizeof(buf), "warming");
    } else {
        snprintf(buf, sizeof(buf), "%+.1f %+.1f %+.1f", ax, ay, az);
    }
    row("IMU a", buf);
    if (!imu::present()) {
        row("IMU", "silent");
    } else if (gCtx.idle) {
        snprintf(buf, sizeof(buf), "%s IDLE", imu::orientation());
        row("IMU", buf);
    } else {
        row("IMU", imu::orientation());
    }

    if (settings::localUrlOverride()) {
        const char *u = settings::localUrl();
        if (strlen(u) > 28) {
            snprintf(buf, sizeof(buf), "...%s", u + strlen(u) - 25);
        } else {
            snprintf(buf, sizeof(buf), "%s", u);
        }
    } else {
        snprintf(buf, sizeof(buf), "compiled");
    }
    row("Local URL", buf);

    if (gCtx.tracker) {
        snprintf(buf, sizeof(buf), "%u of %u",
                 static_cast<unsigned>(gCtx.tracker->positionCount()),
                 static_cast<unsigned>(gCtx.tracker->count()));
        row("Plottable", buf);
    }

    snprintf(buf, sizeof(buf), "%s / %s", gCtx.keypadPresent ? "kbd" : "NO kbd",
             gCtx.touchPresent ? "touch" : "NO touch");
    row("Input", buf);

    // What the last key actually reported. The keymap in keypad.cpp came from
    // LilyGO's factory example and may not match the board in your hands, so
    // this is how you find out what to change it to: press a key here and read
    // off the character it produced and where the controller says it was.
    const keypad::LastKey &lk = keypad::lastKey();
    if (lk.raw == 0) {
        snprintf(buf, sizeof(buf), "press one");
    } else if (lk.row < 0) {
        snprintf(buf, sizeof(buf), "raw %d dropped", static_cast<int>(lk.raw));
    } else {
        snprintf(buf, sizeof(buf), "'%c' r%d c%d raw%d", lk.c,
                 static_cast<int>(lk.row), static_cast<int>(lk.col),
                 static_cast<int>(lk.raw));
    }
    row("Last key", buf);

    const touch::LastTap &tap = touch::lastTap();
    if (tap.x < 0) {
        snprintf(buf, sizeof(buf), "tap one");
    } else {
        snprintf(buf, sizeof(buf), "%d, %d", static_cast<int>(tap.x),
                 static_cast<int>(tap.y));
    }
    row("Last tap", buf);

    // Whether duty cycling is actually winning. A module that keeps its
    // ephemeris across the sleep re-fixes in a second or two; one that cold
    // starts every time takes half a minute and saves far less, and the only
    // way to tell the two apart is to watch the numbers.
    if (gCtx.gnssEnabled) {
        if (gnss::powered()) {
            snprintf(buf, sizeof(buf), "on  %u%% duty",
                     static_cast<unsigned>(gnss::dutyPercent()));
        } else if (gnss::sleepRemainingMs() > 0) {
            snprintf(buf, sizeof(buf), "%us  %u%% duty",
                     static_cast<unsigned>(gnss::sleepRemainingMs() / 1000),
                     static_cast<unsigned>(gnss::dutyPercent()));
        } else {
            snprintf(buf, sizeof(buf), "off, unused");
        }
        row("GNSS pwr", buf);

        if (gnss::lastTtffMs() > 0) {
            snprintf(buf, sizeof(buf), "%.1f s",
                     gnss::lastTtffMs() / 1000.0f);
            row("Last TTFF", buf);
        }
    }

    row("AC db", aircraftdb::status());
    row("Icons", icons::status());
    row("Settings", settings::status());
    row("Track log", settings::logEnabled() ? "on" : "off");

    snprintf(buf, sizeof(buf), "%u k / %u k",
             static_cast<unsigned>(ESP.getFreeHeap() / 1024),
             static_cast<unsigned>(ESP.getFreePsram() / 1024));
    row("Free RAM", buf);

    // Largest contiguous block of each heap. Free bytes alone cannot tell you
    // the arena is about to fail to allocate; this and the line above drifting
    // apart is what fragmentation looks like from the outside.
    snprintf(buf, sizeof(buf), "%u k / %u k",
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    row("Max block", buf);

    gStatusRows = index;

    // Same scroll indicator the list view uses, so the two pages read the same
    // way -- and so there is something on screen saying the page continues.
    if (index > visible) {
        const int16_t trackH = static_cast<int16_t>(visible * kRowH);
        int16_t barH = static_cast<int16_t>(trackH * visible / index);
        if (barH < 4) barH = 4;
        const int16_t barY = static_cast<int16_t>(
            top + (trackH - barH) * gStatusTop / (index - visible));
        g().fillRect(SCREEN_W - 3, barY, 3, barH, BLACK);
    }

    drawNavBar(true);
}

const char *distanceName(uint8_t u) {
    if (u == 1) return "miles";
    if (u == 2) return "km";
    return "nm";
}

const char *speedName(uint8_t u) {
    if (u == 1) return "mph";
    if (u == 2) return "kph";
    return "knots";
}

const char *sortName(uint8_t u) {
    switch (static_cast<ListSort>(u)) {
        case ListSort::Alt: return "altitude";
        case ListSort::Speed: return "speed";
        case ListSort::Callsign: return "callsign";
        case ListSort::Range:
        default: return "range";
    }
}

const char *paceName(uint8_t u) {
    switch (static_cast<PollPace>(u)) {
        case PollPace::Normal: return "normal";
        case PollPace::Slow: return "slow";
        case PollPace::Fast:
        default: return "fast";
    }
}

const char *filterLongName(uint8_t u) {
    switch (static_cast<TrafficFilter>(u)) {
        case TrafficFilter::Airborne: return "airborne";
        case TrafficFilter::Low: return "below FL100";
        case TrafficFilter::High: return "FL100+";
        case TrafficFilter::All:
        default: return "all";
    }
}

void maskPassword(char *out, size_t len, const char *pass) {
    const size_t n = strlen(pass);
    if (n == 0) {
        snprintf(out, len, "(empty)");
        return;
    }
    size_t i = 0;
    for (; i < n && i + 1 < len; ++i) out[i] = '*';
    out[i] = '\0';
}

char symbolFor(char c) {
    switch (c) {
        case 'q': return '1';
        case 'w': return '2';
        case 'e': return '3';
        case 'r': return '4';
        case 't': return '5';
        case 'y': return '6';
        case 'u': return '7';
        case 'i': return '8';
        case 'o': return '9';
        case 'p': return '0';
        case 'a': return '!';
        case 's': return '@';
        case 'd': return '#';
        case 'f': return '$';
        case 'g': return '%';
        case 'h': return '^';
        case 'j': return '&';
        case 'k': return '*';
        case 'l': return '(';
        case 'z': return '-';
        case 'x': return '_';
        case 'c': return '=';
        case 'v': return '+';
        case 'b': return '[';
        case 'n': return ']';
        case 'm': return '/';
        case '-': return '?';
        case '0': return ')';
        default: return c;
    }
}

char mapEditChar(char c) {
    if (c == keypad::kShift || c == keypad::kSymbol || c == keypad::kEnter ||
        c == keypad::kBackspace || c == keypad::kAlt || c == keypad::kMic) {
        return 0;
    }
    if (gEditSym) {
        // Local feed URLs need '.' and ':' more than brackets.
        if (gEditField == EditField::LocalUrl) {
            if (c == 'v') return '.';
            if (c == 'b') return ':';
        }
        return symbolFor(c);
    }
    if (gEditShift && c >= 'a' && c <= 'z') return static_cast<char>(toupper(c));
    return c;
}

void cancelEdit() {
    gEditField = EditField::None;
    gEditBuf[0] = '\0';
    gEditShift = false;
    gEditSym = false;
}

void startEdit(EditField field) {
    gEditField = field;
    gEditShift = false;
    gEditSym = false;
    if (field == EditField::Ssid) {
        strncpy(gEditBuf, settings::wifiOverride() ? settings::wifiSsid()
                                                   : gCtx.wifiSsid,
                settings::kWifiSsidMax);
        gEditBuf[settings::kWifiSsidMax] = '\0';
    } else if (field == EditField::Pass) {
        strncpy(gEditBuf, settings::wifiPass(), settings::kWifiPassMax);
        gEditBuf[settings::kWifiPassMax] = '\0';
    } else if (field == EditField::LocalUrl) {
        const char *url = settings::localUrlOverride() ? settings::localUrl()
                                                       : ADSB_LOCAL_URL;
        strncpy(gEditBuf, url, settings::kLocalUrlMax);
        gEditBuf[settings::kLocalUrlMax] = '\0';
    }
}

void commitEdit() {
    if (gEditField == EditField::Ssid) {
        if (!gEditBuf[0]) {
            settings::clearWifiOverride();
        } else {
            settings::setWifiOverride(gEditBuf, settings::wifiPass());
        }
        net::reconnect();
    } else if (gEditField == EditField::Pass) {
        const char *ssid = settings::wifiOverride() ? settings::wifiSsid()
                                                    : gCtx.wifiSsid;
        if (ssid && ssid[0]) {
            settings::setWifiOverride(ssid, gEditBuf);
            net::reconnect();
        }
    } else if (gEditField == EditField::LocalUrl) {
        if (!gEditBuf[0]) {
            settings::clearLocalUrl();
        } else {
            settings::setLocalUrl(gEditBuf);
        }
        gRefreshRequested = true;
    }
    cancelEdit();
    display::invalidate();
}

void noteFilterChanged() {
    gFilterChanged = true;
    display::invalidate();
}

void cycleByte(uint8_t *v, uint8_t maxInclusive, int dir) {
    const int next = static_cast<int>(*v) + dir;
    if (next < 0) *v = maxInclusive;
    else if (next > maxInclusive) *v = 0;
    else *v = static_cast<uint8_t>(next);
}

void applySettingDelta(int dir) {
    Settings &s = settings::get();
    switch (static_cast<SetRow>(gSettingsRow)) {
        case SetRow::Distance:
            cycleByte(&s.distanceUnits, 2, dir);
            settings::applyLive();
            break;
        case SetRow::Speed:
            cycleByte(&s.speedUnits, 2, dir);
            settings::applyLive();
            break;
        case SetRow::Traffic:
            cycleByte(&s.trafficFilter, static_cast<uint8_t>(TrafficFilter::High),
                      dir);
            noteFilterChanged();
            break;
        case SetRow::Sort:
            cycleByte(&s.listSort, static_cast<uint8_t>(ListSort::Callsign), dir);
            gListFollowSelection = true;
            break;
        case SetRow::Poll:
            cycleByte(&s.pollPace, static_cast<uint8_t>(PollPace::Slow), dir);
            gPollPaceChanged = true;
            break;
        case SetRow::Feed:
            adsb::toggleProvider();
            gRefreshRequested = true;
            break;
        case SetRow::LocalUrl:
            if (dir == 0) startEdit(EditField::LocalUrl);
            return;
        case SetRow::Map:
            gMapEnabled = !gMapEnabled;
            s.mapEnabled = gMapEnabled;
            break;
        case SetRow::MapFile:
            sdcard::cycleMapPath(dir == 0 ? 1 : dir);
            basemap::reload();
            break;
        case SetRow::Gnss:
            gCentreOnGnss = !gCentreOnGnss;
            s.centreOnGnss = gCentreOnGnss;
            gPanEastNm = 0.0f;
            gPanNorthNm = 0.0f;
            break;
        case SetRow::Backlight:
            cycleByte(&s.keypadBacklight, static_cast<uint8_t>(BacklightMode::Auto),
                      dir);
            power::refreshKeypadBacklight(s.keypadBacklight);
            break;
        case SetRow::Idle:
            settings::setFaceDownIdle(!settings::faceDownIdle());
            break;
        case SetRow::SwapXY:
            s.swapXY = !s.swapXY;
            settings::applyLive();
            break;
        case SetRow::MirrorX:
            s.mirrorX = !s.mirrorX;
            settings::applyLive();
            break;
        case SetRow::MirrorY:
            s.mirrorY = !s.mirrorY;
            settings::applyLive();
            break;
        case SetRow::Home:
            if (dir == 0) {
                s.homeLatE7 = static_cast<int32_t>(lround(gCtx.ownLat * 1e7));
                s.homeLonE7 = static_cast<int32_t>(lround(gCtx.ownLon * 1e7));
                s.homeOverride = true;
            }
            break;
        case SetRow::WifiSsid:
            if (dir == 0) startEdit(EditField::Ssid);
            return;
        case SetRow::WifiPass:
            if (dir == 0) startEdit(EditField::Pass);
            return;
        case SetRow::TrackLog:
            settings::setLogEnabled(!settings::logEnabled());
            break;
        case SetRow::System:
            if (dir == 0) setView(View::Status);
            return;
        default:
            break;
    }
    settings::markDirty();
    persist();
    display::invalidate();
}

void scrollSettings(int delta) {
    const int visible = SETTINGS_VISIBLE;
    int maxTop = gSettingsRows - visible;
    if (maxTop < 0) maxTop = 0;
    int next = gSettingsTop + delta;
    if (next < 0) next = 0;
    if (next > maxTop) next = maxTop;
    if (next == gSettingsTop) return;
    gSettingsTop = next;
    display::invalidate();
}

void moveSettingsRow(int delta) {
    int local = 0;
    for (int i = 0; i < static_cast<int>(SetRow::Count); ++i) {
        if (settingCategory(static_cast<SetRow>(i)) != gSettingsCategory) continue;
        if (i == gSettingsRow) break;
        ++local;
    }
    local += delta;
    if (local < 0) local = gSettingsRows - 1;
    if (local >= gSettingsRows) local = 0;
    gSettingsRow = settingAtCategoryIndex(gSettingsCategory, local);
    const int visible = SETTINGS_VISIBLE;
    if (local < gSettingsTop) gSettingsTop = local;
    if (local >= gSettingsTop + visible) {
        gSettingsTop = local - visible + 1;
    }
    display::invalidate();
}

void drawSettings() {
    char buf[48];
    const Settings &s = settings::get();
    const int16_t heading = CONTENT_Y + 6;
    const char *tabs[4] = {"TRAFFIC", "DISPLAY", "CONNECT", "DEVICE"};
    constexpr int16_t kTabW = SCREEN_W / 4;
    for (int i = 0; i < 4; ++i) {
        const int16_t x = static_cast<int16_t>(i * kTabW);
        if (i == gSettingsCategory) g().fillRect(x, heading - 4, kTabW, 18, BLACK);
        if (i) g().drawFastVLine(x, heading - 4, 18, BLACK);
        textAt(x + 5, heading + 1, tabs[i], i == gSettingsCategory ? WHITE : BLACK);
    }

    constexpr int16_t kRowH = SETTINGS_ROW_H;
    constexpr int16_t top = SETTINGS_TOP;
    constexpr int visible = SETTINGS_VISIBLE;

    int index = 0;
    int categoryIndex = 0;
    auto row = [&](const char *label, const char *value, bool editing) {
        const int i = index++;
        if (settingCategory(static_cast<SetRow>(i)) != gSettingsCategory) return;
        const int ci = categoryIndex++;
        if (ci < gSettingsTop || ci >= gSettingsTop + visible) return;
        const int16_t y = static_cast<int16_t>(top + (ci - gSettingsTop) * kRowH);
        const bool sel = (i == gSettingsRow);
        if (sel) g().fillRect(3, y + 2, SCREEN_W - 9, kRowH - 4, BLACK);
        const uint16_t colour = sel ? WHITE : BLACK;
        labelAt(8, y + 6, label, colour);
        // Leave room for the scrollbar and a cursor after the final character.
        const int16_t valueWidth = SCREEN_W - 20 - (editing ? CHAR_W : 0);
        const size_t shown = textBounded(8, y + 22, valueWidth,
                                         editing ? gEditBuf : value, colour, 1,
                                         editing);
        if (sel && editing) {
            const int16_t cx = static_cast<int16_t>(8 + shown * CHAR_W);
            g().drawFastVLine(cx, y + 21, CHAR_H + 2, colour);
        }
        if (!sel) g().drawFastHLine(4, y + kRowH - 1, SCREEN_W - 12, BLACK);
    };

    row("Distance", distanceName(s.distanceUnits), false);
    row("Speed", speedName(s.speedUnits), false);
    row("Traffic", filterLongName(s.trafficFilter), false);
    row("Sort", sortName(s.listSort), false);
    row("Poll", paceName(s.pollPace), false);
    row("Feed", adsb::providerName(), false);

    if (gEditField == EditField::LocalUrl) {
        row("Local URL", gEditBuf, true);
    } else if (settings::localUrlOverride()) {
        row("Local URL", settings::localUrl(), false);
    } else {
        row("Local URL", "compiled", false);
    }

    row("Map", gMapEnabled ? "on" : "off", false);
    row("Map file", settings::mapLabel(), false);
    row("GNSS ctr", gCentreOnGnss ? "on" : "off", false);
    switch (s.keypadBacklight) {
        case static_cast<uint8_t>(BacklightMode::On):
            row("Backlight", "on", false);
            break;
        case static_cast<uint8_t>(BacklightMode::Auto):
            row("Backlight", "auto", false);
            break;
        default:
            row("Backlight", "off", false);
            break;
    }
    row("Face idle", settings::faceDownIdle() ? "on" : "off", false);
    row("Swap XY", s.swapXY ? "yes" : "no", false);
    row("Mirror X", s.mirrorX ? "yes" : "no", false);
    row("Mirror Y", s.mirrorY ? "yes" : "no", false);

    if (s.homeOverride) {
        snprintf(buf, sizeof(buf), "%.4f,%.4f", s.homeLatE7 * 1e-7,
                 s.homeLonE7 * 1e-7);
    } else {
        snprintf(buf, sizeof(buf), "compiled");
    }
    row("Home", buf, false);

    if (gEditField == EditField::Ssid) {
        row("Wi-Fi", gEditBuf, true);
    } else if (settings::wifiOverride()) {
        row("Wi-Fi", settings::wifiSsid(), false);
    } else {
        row("Wi-Fi", "compiled", false);
    }

    if (gEditField == EditField::Pass) {
        row("Password", gEditBuf, true);
    } else {
        maskPassword(buf, sizeof(buf), settings::wifiPass());
        row("Password", settings::wifiOverride() ? buf : "compiled", false);
    }

    row("Track log", settings::logEnabled() ? "on" : "off", false);
    row("System", "open", false);

    gSettingsRows = categoryIndex;

    if (categoryIndex > visible) {
        const int16_t trackH = static_cast<int16_t>(visible * kRowH);
        int16_t barH = static_cast<int16_t>(trackH * visible / categoryIndex);
        if (barH < 4) barH = 4;
        const int16_t barY = static_cast<int16_t>(
            top + (trackH - barH) * gSettingsTop / (categoryIndex - visible));
        g().fillRect(SCREEN_W - 3, barY, 3, barH, BLACK);
    }

    if (gEditField != EditField::None) {
        drawFooterLines("E:save  U:cancel  S:shift  $:sym", nullptr);
    } else {
        drawNavBar();
    }
}

void activateSetting() {
    const SetRow row = static_cast<SetRow>(gSettingsRow);
    if (row == SetRow::Home || row == SetRow::WifiSsid || row == SetRow::WifiPass ||
        row == SetRow::LocalUrl || row == SetRow::System) {
        applySettingDelta(0);
    } else {
        applySettingDelta(1);
    }
}

void drawCurrentView() {
    drawStatusBar();
    switch (gView) {
        case View::Radar: drawRadar(); break;
        case View::List: drawList(); break;
        case View::Detail: drawDetail(); break;
        case View::Status: drawStatus(); break;
        case View::Settings: drawSettings(); break;
    }
}

// Clamped against what the last draw actually counted, so a page that grows
// a row stays reachable without anybody updating a constant.
void scrollStatus(int delta) {
    const int visible = (FOOTER_Y - (CONTENT_Y + 28)) / STATUS_ROW_H;
    int maxTop = gStatusRows - visible;
    if (maxTop < 0) maxTop = 0;

    int next = gStatusTop + delta;
    if (next < 0) next = 0;
    if (next > maxTop) next = maxTop;
    if (next == gStatusTop) return;  // already at the end: do not spend a refresh

    gStatusTop = next;
    display::invalidate();
}

void setView(View v) {
    if (v == gView) return;
    cancelEdit();
    gPreviousView = gView;
    gView = v;
    // Always arrive at the top of the diagnostics rather than wherever it was
    // left, which from the outside looks like a page with its head cut off.
    if (v == View::Status) gStatusTop = 0;
    if (v == View::Settings) gSettingsTop = 0;
    // A view change replaces the whole screen, so clear the ghosting with it.
    display::invalidate(true);
}

void goBack() {
    if (gEditField != EditField::None) {
        cancelEdit();
        display::invalidate();
        return;
    }
    if (gView == View::Detail || gView == View::Status || gView == View::Settings) {
        setView(gPreviousView == gView ? View::Radar : gPreviousView);
    } else {
        gSelectedHex[0] = '\0';
        display::invalidate();
    }
}

}  // namespace

void begin() {
    // settings::begin() has already run, so this is the saved set or the
    // config.h defaults -- the UI cannot tell which, and does not need to.
    const Settings &s = settings::get();
    gRangeIndex = s.rangeIndex < kRangeStepCount ? s.rangeIndex : RANGE_DEFAULT_INDEX;
    gMapEnabled = s.mapEnabled;
    gCentreOnGnss = s.centreOnGnss;

    gView = View::Radar;
    gPreviousView = View::Radar;
    display::invalidate(true);
}

void setContext(const Context &ctx) { gCtx = ctx; }

bool consumeRefreshRequest() {
    const bool r = gRefreshRequested;
    gRefreshRequested = false;
    return r;
}

bool centreOnGnss() { return gCentreOnGnss; }

void panOffsetNm(float *eastNm, float *northNm) {
    if (eastNm) *eastNm = gPanEastNm;
    if (northNm) *northNm = gPanNorthNm;
}

void scrollList(int delta) {
    rebuildListOrder();
    const int rows = CONTENT_H / ROW_H;
    int maxTop = gListCount - rows;
    if (maxTop < 0) maxTop = 0;
    int next = gListTop + delta;
    if (next < 0) next = 0;
    if (next > maxTop) next = maxTop;
    if (next == gListTop) return;
    gListTop = next;
    gListFollowSelection = false;
    display::invalidate();
}

void handleEditKey(char key) {
    if (key == keypad::kEnter) {
        commitEdit();
        return;
    }
    if (key == keypad::kBackspace) {
        const size_t n = strlen(gEditBuf);
        if (n == 0) {
            cancelEdit();
            display::invalidate();
            return;
        }
        gEditBuf[n - 1] = '\0';
        display::invalidate();
        return;
    }
    if (key == keypad::kShift) {
        gEditShift = !gEditShift;
        display::invalidate();
        return;
    }
    if (key == keypad::kSymbol) {
        gEditSym = !gEditSym;
        display::invalidate();
        return;
    }
    const char mapped = mapEditChar(key);
    if (!mapped) return;
    const size_t cap =
        (gEditField == EditField::Ssid)      ? settings::kWifiSsidMax
        : (gEditField == EditField::LocalUrl) ? settings::kLocalUrlMax
                                              : settings::kWifiPassMax;
    const size_t n = strlen(gEditBuf);
    if (n >= cap) return;
    gEditBuf[n] = mapped;
    gEditBuf[n + 1] = '\0';
    if (gEditShift) gEditShift = false;  // sticky for one character
    display::invalidate();
}

bool handleSettingsKey(char key) {
    if (gEditField != EditField::None) {
        handleEditKey(key);
        return true;
    }
    switch (key) {
        case '1':
        case '2':
        case '3':
        case '4':
            gSettingsCategory = static_cast<uint8_t>(key - '1');
            gSettingsTop = 0;
            gSettingsRow = firstSettingInCategory(gSettingsCategory);
            display::invalidate();
            return true;
        case 'w':
        case 'k':
            moveSettingsRow(-1);
            return true;
        case 's':
        case 'j':
            moveSettingsRow(1);
            return true;
        case 'a':
            applySettingDelta(-1);
            return true;
        case 'd':
            applySettingDelta(1);
            return true;
        case keypad::kEnter:
            activateSetting();
            return true;
        case 'c':
            if (static_cast<SetRow>(gSettingsRow) == SetRow::Home) {
                Settings &s = settings::get();
                s.homeOverride = false;
                settings::markDirty();
                display::invalidate();
            } else if (static_cast<SetRow>(gSettingsRow) == SetRow::MapFile) {
                settings::setMapPath("");
                basemap::reload();
                display::invalidate();
            } else if (static_cast<SetRow>(gSettingsRow) == SetRow::LocalUrl) {
                settings::clearLocalUrl();
                gRefreshRequested = true;
                display::invalidate();
            } else if (static_cast<SetRow>(gSettingsRow) == SetRow::WifiSsid ||
                       static_cast<SetRow>(gSettingsRow) == SetRow::WifiPass) {
                settings::clearWifiOverride();
                net::reconnect();
                display::invalidate();
            }
            return true;
        default:
            return false;
    }
}

uint16_t rangeNm() { return currentRangeNm(); }

const char *selectedHex() { return gSelectedHex; }

bool consumeFilterChange() {
    const bool r = gFilterChanged;
    gFilterChanged = false;
    return r;
}

bool consumePollPaceChange() {
    const bool r = gPollPaceChanged;
    gPollPaceChanged = false;
    return r;
}

void handleKey(char key) {
    if (key == 0) return;

    if (gView == View::Settings && handleSettingsKey(key)) return;

    switch (key) {
        case 't':
            setView(gView == View::Radar ? View::List : View::Radar);
            break;

        case 'o':
            setView(gView == View::Settings ? gPreviousView : View::Settings);
            break;

        case 'h':
            if (gView == View::Radar || gView == View::List ||
                gView == View::Settings) {
                filter::cycle();
                noteFilterChanged();
            }
            break;

        // WASD pans the plot on the radar. In the list and detail views there
        // is nothing to pan, so w/s fall back to moving the selection -- which
        // is what those keys mean everywhere else.
        case 'w':
            if (gView == View::Radar) {
                panByNm(0.0f, panStepNm());
            } else if (gView == View::Status) {
                scrollStatus(-1);
            } else {
                moveSelection(-1);
                display::invalidate();
            }
            break;
        case 's':
            if (gView == View::Radar) {
                panByNm(0.0f, -panStepNm());
            } else if (gView == View::Status) {
                scrollStatus(1);
            } else {
                moveSelection(1);
                display::invalidate();
            }
            break;
        case 'a':
            if (gView == View::Radar) panByNm(-panStepNm(), 0.0f);
            break;
        case 'd':
            if (gView == View::Radar) panByNm(panStepNm(), 0.0f);
            break;

        case 'x':
            zoomIn();
            break;
        case 'z':
            zoomOut();
            break;

        case 'c':
            recentre();
            break;

        case 'b':
            toggleFollow();
            break;

        case 'k':
            if (gView == View::Status) {
                scrollStatus(-1);
            } else {
                moveSelection(-1);
                display::invalidate();
            }
            break;
        case 'j':
            if (gView == View::Status) {
                scrollStatus(1);
            } else {
                moveSelection(1);
                display::invalidate();
            }
            break;

        case keypad::kEnter:
            if (gView == View::Detail) {
                goBack();
            } else if (selected()) {
                setView(View::Detail);
            }
            break;

        case keypad::kBackspace:
            goBack();
            break;

        case 'r':
            gRefreshRequested = true;
            break;

        case 'f':
            display::invalidate(true);
            break;

        case 'g':
            gCentreOnGnss = !gCentreOnGnss;
            // Our own position is about to jump; a pan measured from the old
            // one would be meaningless, and so would a follow offset.
            gFollowAircraft = false;
            gPanEastNm = 0.0f;
            gPanNorthNm = 0.0f;
            persist();
            display::invalidate();
            break;

        case 'l': {
            Settings &s = settings::get();
            cycleByte(&s.keypadBacklight, static_cast<uint8_t>(BacklightMode::Auto),
                      1);
            power::refreshKeypadBacklight(s.keypadBacklight);
            persist();
            if (gView == View::Settings) display::invalidate();
            break;
        }

        case 'm':
            gMapEnabled = !gMapEnabled;
            persist();
            display::invalidate();
            break;

        // Swap between your own receiver and the aggregator. The old feed's
        // targets are left to age out rather than cleared: they merge by ICAO
        // hex, so switching to the aggregator fills in the registration and
        // type a local aircraft.json does not carry, and switching back keeps
        // them while positions go back to being a fraction of a second old.
        case 'p':
            adsb::toggleProvider();
            persist();
            gRefreshRequested = true;
            display::invalidate();
            break;

        case 'i':
            setView(gView == View::Status ? gPreviousView : View::Status);
            break;

        case 'v':
            sdcard::requestScreenshot();
            break;

        default:
            break;
    }
}

// A press and release in roughly the same place.
void handleTap(int16_t x, int16_t y) {
    if (y < STATUS_H) {
        return;
    }

    if (y >= FOOTER_Y) {
        if (gView == View::Detail || gView == View::Status) {
            if (x < 72) goBack();
            return;
        }
        if (x < SCREEN_W / 3) setView(View::Radar);
        else if (x < 2 * SCREEN_W / 3) setView(View::List);
        else setView(View::Settings);
        return;
    }

    if (gView == View::Radar && y >= READOUT_Y) {
        if (selected()) setView(View::Detail);
        return;
    }

    if (gView == View::Settings) {
        if (y < SETTINGS_TOP) {
            uint8_t category = static_cast<uint8_t>(x / (SCREEN_W / 4));
            if (category > 3) category = 3;
            if (category != gSettingsCategory) {
                gSettingsCategory = category;
                gSettingsTop = 0;
                gSettingsRow = firstSettingInCategory(category);
                display::invalidate();
            }
            return;
        }
        if (y >= SETTINGS_BOTTOM) return;
        const int row = (y - SETTINGS_TOP) / SETTINGS_ROW_H + gSettingsTop;
        if (row >= 0 && row < gSettingsRows) {
            gSettingsRow = settingAtCategoryIndex(gSettingsCategory, row);
            activateSetting();
        }
        return;
    }

    if (gView == View::Radar) {
        if (inButton(x, y, BTN_MINUS_X, BTN_Y)) {
            zoomOut();
            return;
        }
        if (inButton(x, y, BTN_PLUS_X, BTN_Y)) {
            zoomIn();
            return;
        }
        if (isPanned() && inButton(x, y, BTN_HOME_X, BTN_HOME_Y)) {
            recentre();
            return;
        }
        if (inButton(x, y, BTN_FOLLOW_X, BTN_FOLLOW_Y)) {
            toggleFollow();
            return;
        }
    }

    if (gView == View::Radar && gCtx.tracker) {
        int best = -1;
        int bestDistSq = kTapRadiusPx * kTapRadiusPx;
        for (size_t i = 0; i < gCtx.tracker->count(); ++i) {
            int16_t ax = 0, ay = 0;
            if (!radarPosition(gCtx.tracker->at(i), &ax, &ay)) continue;
            const int dx = ax - x;
            const int dy = ay - y;
            const int distSq = dx * dx + dy * dy;
            if (distSq <= bestDistSq) {
                bestDistSq = distSq;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) {
            selectIndex(best);
            display::invalidate();
        } else if (gSelectedHex[0]) {
            clearSelection();
        }
        return;
    }

    if (gView == View::List && gCtx.tracker) {
        rebuildListOrder();
        const int row = (y - CONTENT_Y) / ROW_H;
        if (row < 0) return;
        const int vis = gListTop + row;
        if (vis < 0 || vis >= gListCount) return;
        const int idx = gListOrder[vis];
        gListFollowSelection = true;
        if (idx == selectionIndex()) {
            setView(View::Detail);
        } else {
            selectIndex(idx);
            display::invalidate();
        }
        return;
    }

}

void handleTouch(const touch::Event &event) {
    if (event.kind == touch::Event::Kind::Down) {
        gTouchDownX = event.x;
        gTouchDownY = event.y;
        return;
    }

    int dx = event.x - gTouchDownX;
    int dy = event.y - gTouchDownY;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;

    if (adx < kDragThresholdPx && ady < kDragThresholdPx) {
        handleTap(gTouchDownX, gTouchDownY);
        return;
    }

    // Drag the map: the content follows your finger, so the plot centre moves
    // the opposite way. Only the radar has anything to pan.
    if (gView == View::Radar && gTouchDownY >= SCOPE_Y &&
        gTouchDownY < READOUT_Y) {
        const float scale = pixelsPerNm();
        panByNm(-dx / scale, dy / scale);
        return;
    }

    // The diagnostics page is taller than the panel. A tap on it still means
    // back -- that is what its footer says -- but a drag is already told apart
    // from a tap above, so it can scroll, content following the finger the
    // same way the radar does.
    if (gView == View::Status && gTouchDownY >= STATUS_H && gTouchDownY < FOOTER_Y) {
        scrollStatus(-dy / STATUS_ROW_H);
        return;
    }
    if (gView == View::Settings && gTouchDownY >= SETTINGS_TOP && gTouchDownY < SETTINGS_BOTTOM) {
        scrollSettings(-dy / SETTINGS_ROW_H);
        return;
    }
    if (gView == View::List && gTouchDownY >= STATUS_H && gTouchDownY < FOOTER_Y) {
        scrollList(-dy / ROW_H);
    }
}

bool tick() {
    // Before the fingerprint, so a moving target shifts the plot centre and
    // the hash in the same pass.
    updateFollow();

    // Fold the chrome into the fingerprint so a battery or Wi-Fi change also
    // repaints, but quantise the noisy inputs.
    // The radar asks whether anything moved a pixel; the list and detail views
    // print numbers and are fingerprinted at the resolution they print at.
    const float hashPxPerNm = (gView == View::Radar) ? pixelsPerNm() : 0.0f;
    double plotLat = gCtx.ownLat, plotLon = gCtx.ownLon;
    if (gView == View::Radar) plotCentre(&plotLat, &plotLon);

    uint32_t h = gCtx.tracker
                     ? gCtx.tracker->sceneHash(plotLat, plotLon, hashPxPerNm)
                     : 0u;
    auto mix = [&h](uint32_t v) {
        h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    };
    mix(static_cast<uint32_t>(gView));
    mix(static_cast<uint32_t>(gRangeIndex));
    mix(static_cast<uint32_t>(static_cast<int32_t>(gPanEastNm * 10.0f)));
    mix(static_cast<uint32_t>(static_cast<int32_t>(gPanNorthNm * 10.0f)));
    mix(static_cast<uint32_t>(gFollowAircraft));
    mix(static_cast<uint32_t>(gListTop));
    mix(static_cast<uint32_t>(gStatusTop));
    mix(static_cast<uint32_t>(gSettingsTop));
    mix(static_cast<uint32_t>(gSettingsRow));
    mix(static_cast<uint32_t>(gSettingsCategory));
    mix(static_cast<uint32_t>(gEditField));
    mix(static_cast<uint32_t>(gMapEnabled));
    mix(settings::get().distanceUnits);
    mix(settings::get().speedUnits);
    mix(settings::get().trafficFilter);
    mix(settings::get().listSort);
    mix(settings::get().pollPace);
    mix(settings::get().swapXY);
    mix(settings::get().mirrorX);
    mix(settings::get().mirrorY);
    mix(settings::get().homeOverride);
    mix(static_cast<uint32_t>(settings::get().homeLatE7));
    mix(static_cast<uint32_t>(settings::get().homeLonE7));
    mix(static_cast<uint32_t>(settings::wifiOverride()));
    mix(static_cast<uint32_t>(settings::logEnabled()));
    mix(static_cast<uint32_t>(settings::faceDownIdle()));
    mix(settings::get().keypadBacklight);
    for (const char *p = settings::mapPath(); *p; ++p) {
        mix(static_cast<uint8_t>(*p));
    }
    for (const char *p = settings::localUrl(); *p; ++p) {
        mix(static_cast<uint8_t>(*p));
    }
    // The basemap and our own crosshair move with our position even when no
    // aircraft do, so it has to be in the fingerprint -- at the same pixel
    // resolution as everything else on the radar, which is what finally
    // silences GNSS jitter rather than merely slowing it down. One nautical
    // mile is a minute of latitude, so degrees * pxPerNm * 60 is pixels.
    if (hashPxPerNm > 0.0f) {
        const double perDeg = static_cast<double>(hashPxPerNm) * 60.0;
        mix(static_cast<uint32_t>(lround(gCtx.ownLat * perDeg)));
        mix(static_cast<uint32_t>(lround(gCtx.ownLon * perDeg)));
    } else {
        mix(static_cast<uint32_t>(static_cast<int32_t>(gCtx.ownLat * 1000.0)));
        mix(static_cast<uint32_t>(static_cast<int32_t>(gCtx.ownLon * 1000.0)));
    }
    mix(gCtx.batteryValid ? gCtx.batteryPercent : 0xFFu);
    mix(static_cast<uint32_t>(gCtx.wifiConnected));
    mix(static_cast<uint32_t>(gCtx.networkActivity.upload));
    mix(static_cast<uint32_t>(gCtx.networkActivity.download));
    mix(static_cast<uint32_t>(gCtx.wifiConnected ? wifiBars(gCtx.wifiRssi) : -1));
    mix(static_cast<uint32_t>(gCtx.gnssFix));
    mix(static_cast<uint32_t>(gnssQuality(gCtx.gnssFix, gCtx.gnssSatellites)));
    mix(feedAgeBucket());
    mix(static_cast<uint32_t>(gCtx.idle));
    // Minute resolution: this alone repaints the panel once a minute, which is
    // the accepted price of a clock on the glass.
    mix(gCtx.clockValid
            ? static_cast<uint32_t>(gCtx.clockHour) * 60u + gCtx.clockMinute
            : 0xFFFFu);
    for (const char *p = gSelectedHex; *p; ++p) mix(static_cast<uint8_t>(*p));
    // Only on the diagnostics page, where the last key is on screen: elsewhere
    // this would repaint the panel for a keypress that changed nothing.
    if (gView == View::Status) {
        mix(static_cast<uint32_t>(keypad::lastKey().raw));
        mix(static_cast<uint32_t>(static_cast<uint16_t>(touch::lastTap().x)));
        mix(static_cast<uint32_t>(static_cast<uint16_t>(touch::lastTap().y)));
        mix(static_cast<uint32_t>(sdcard::shotCount()));
        // Quantised so GNSS-grade IMU noise does not thrash the panel: lux to
        // the nearest 10 lx, accel to 0.2 m/s^2. Orientation is a string that
        // only flips on a real attitude change.
        float lux = 0.0f;
        if (als::lux(&lux)) {
            mix(static_cast<uint32_t>(lroundf(lux / 10.0f)));
        } else {
            mix(0xFFFEu);
        }
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        if (imu::acceleration(&ax, &ay, &az)) {
            mix(static_cast<uint32_t>(lroundf(ax * 5.0f)));
            mix(static_cast<uint32_t>(lroundf(ay * 5.0f)));
            mix(static_cast<uint32_t>(lroundf(az * 5.0f)));
        } else {
            mix(0xFFFEu);
        }
        for (const char *p = imu::orientation(); *p; ++p) {
            mix(static_cast<uint8_t>(*p));
        }
    }
    if (gView == View::Settings && gEditField != EditField::None) {
        for (const char *p = gEditBuf; *p; ++p) mix(static_cast<uint8_t>(*p));
        mix(static_cast<uint32_t>(gEditShift));
        mix(static_cast<uint32_t>(gEditSym));
    }

    return display::render(h, drawCurrentView);
}

}  // namespace ui
