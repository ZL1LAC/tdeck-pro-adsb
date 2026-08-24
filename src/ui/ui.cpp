#include "ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "basemap.h"
#include "board_pins.h"
#include "config.h"
#include "core/geo.h"
#include "core/aircraftdb.h"
#include "core/settings.h"
#include "display.h"
#include "icons.h"
#include "hw/clock.h"
#include "hw/gnss.h"
#include "hw/keypad.h"
#include "hw/power.h"

namespace ui {
namespace {

// GxEPD2's colour constants, kept local so this file only needs Adafruit_GFX.
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;

// ------------------------------------------------------------- geometry ----
constexpr int16_t SCREEN_W = EPD_WIDTH;
constexpr int16_t SCREEN_H = EPD_HEIGHT;

constexpr int16_t STATUS_H = 14;
constexpr int16_t FOOTER_H = 21;
constexpr int16_t CONTENT_Y = STATUS_H + 1;
constexpr int16_t FOOTER_Y = SCREEN_H - FOOTER_H;
constexpr int16_t CONTENT_H = FOOTER_Y - 1 - CONTENT_Y;

constexpr int16_t CHAR_W = 6;   // built-in GFX font, size 1
constexpr int16_t CHAR_H = 8;
constexpr int16_t ROW_H = 11;   // list rows

// --------------------------------------------- status bar layout -----------
// The bar is packed to the pixel: four zones separated by 1 px rules. The two
// right-hand fields are anchored to the right edge rather than to a fixed x,
// so a 3- vs 4-character battery percentage cannot shift anything else. Worst
// case ("RDR 463km", "96ac", "100%") lands exactly on SCREEN_W - 2.
constexpr int16_t BAR_TEXT_Y = 3;
constexpr int16_t BAR_DIV_1 = 59;     // context | feed
constexpr int16_t BAR_DIV_2 = 113;    // feed | radios
constexpr int16_t BAR_DIV_3 = 161;    // radios | power
constexpr int16_t BAR_CTX_X = 2;      // "RDR 40nm" / "DETAIL" / "DIAG"
constexpr int16_t BAR_COUNT_X = 63;   // "12ac"
constexpr int16_t BAR_AGE_X = 91;     // "OK" / "30s" / "2m"
constexpr int16_t BAR_WIFI_X = 117;   // 4 bars, 2 px wide on a 3 px pitch
constexpr int16_t BAR_GNSS_X = 133;   // "G8*"
constexpr int16_t BAR_CLOCK_X = 165;  // "23:45"
constexpr int16_t BAR_BATT_X = 199;   // battery glyph, 13 px wide

constexpr int16_t RADAR_CX = SCREEN_W / 2;
constexpr int16_t RADAR_CY = CONTENT_Y + CONTENT_H / 2;
constexpr int16_t RADAR_R = 110;

// Labelling every target turns the plot into mush. Nearest few plus whatever
// is selected is what you actually want to read.
constexpr int kMaxLabels = 10;
constexpr int kTapRadiusPx = 18;

// On-screen zoom / recentre buttons, clear of the outer range ring.
constexpr int16_t BTN = 20;
constexpr int16_t BTN_Y = FOOTER_Y - 5 - BTN;
constexpr int16_t BTN_MINUS_X = 4;
constexpr int16_t BTN_PLUS_X = SCREEN_W - 4 - BTN;
constexpr int16_t BTN_HOME_X = 4;
constexpr int16_t BTN_HOME_Y = CONTENT_Y + 15;

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

// Where the current touch started, so a release can tell a tap from a drag.
int16_t gTouchDownX = 0;
int16_t gTouchDownY = 0;
int gListTop = 0;
bool gRefreshRequested = false;
bool gCentreOnGnss = GNSS_CENTRE_BY_DEFAULT;
bool gMapEnabled = MAP_ENABLED_BY_DEFAULT;

// Mirrors the handful of UI choices worth surviving a reboot into the
// persisted set. The write itself is deferred and coalesced by
// settings::poll(), so calling this on every keypress is cheap.
void persist() {
    Settings &s = settings::get();
    s.rangeIndex = static_cast<uint8_t>(gRangeIndex);
    s.mapEnabled = gMapEnabled;
    s.centreOnGnss = gCentreOnGnss;
    s.keypadBacklight = power::keypadBacklight();
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

void textRight(int16_t xRight, int16_t y, const char *s,
               uint16_t colour = BLACK) {
    textAt(static_cast<int16_t>(xRight - strlen(s) * CHAR_W), y, s, colour);
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
        return;
    }
    strncpy(gSelectedHex, gCtx.tracker->at(static_cast<size_t>(idx)).hex,
            sizeof(gSelectedHex) - 1);
    gSelectedHex[sizeof(gSelectedHex) - 1] = '\0';
}

void moveSelection(int delta) {
    if (!gCtx.tracker || gCtx.tracker->count() == 0) return;
    const int n = static_cast<int>(gCtx.tracker->count());
    int idx = selectionIndex();
    if (idx < 0) {
        idx = (delta >= 0) ? 0 : n - 1;
    } else {
        idx += delta;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
    }
    selectIndex(idx);
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
    if (!isPanned()) return;
    gPanEastNm = 0.0f;
    gPanNorthNm = 0.0f;
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

// Screen position of a target on the radar. Returns false when it falls
// outside the plotted range.
bool radarPosition(const Aircraft &a, int16_t *sx, int16_t *sy) {
    if (!a.hasPosition) return false;
    float east = 0.0f, north = 0.0f;
    geo::projectNm(gCtx.ownLat, gCtx.ownLon, a.lat, a.lon, &east, &north);
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
void drawVDivider(int16_t x) { g().drawFastVLine(x, 2, 10, BLACK); }

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

void drawStatusBar() {
    Adafruit_GFX &d = g();
    char buf[24];

    // -- context. Which view you are on, plus the plotted range wherever that
    // means something: the list used to give you no way to tell what radius
    // you were looking at.
    switch (gView) {
        case View::Radar:
        case View::List:
            snprintf(buf, sizeof(buf), "%s %u%s", gView == View::Radar ? "RDR" : "LST",
                     static_cast<unsigned>(geo::displayDistance(currentRangeNm())),
                     geo::distanceUnitLabel());
            break;
        case View::Detail:
            snprintf(buf, sizeof(buf), "DETAIL");
            break;
        case View::Status:
            snprintf(buf, sizeof(buf), "DIAG");
            break;
    }
    textAt(BAR_CTX_X, BAR_TEXT_Y, buf);
    drawVDivider(BAR_DIV_1);

    // -- feed. How much traffic we are holding, and how healthy the poll loop
    // is.
    const size_t total = gCtx.tracker ? gCtx.tracker->count() : 0;
    snprintf(buf, sizeof(buf), "%uac", static_cast<unsigned>(total));
    textAt(BAR_COUNT_X, BAR_TEXT_Y, buf);

    formatFeedAge(buf, sizeof(buf));
    textAt(BAR_AGE_X, BAR_TEXT_Y, buf);
    drawVDivider(BAR_DIV_2);

    // -- radios.
    if (gCtx.wifiConnected) {
        const int bars = wifiBars(gCtx.wifiRssi);
        for (int i = 0; i < 4; ++i) {
            const int16_t h = static_cast<int16_t>(2 + 2 * i);
            const int16_t bx = static_cast<int16_t>(BAR_WIFI_X + i * 3);
            if (i < bars) {
                d.fillRect(bx, static_cast<int16_t>(11 - h), 2, h, BLACK);
            } else {
                d.drawFastHLine(bx, 10, 2, BLACK);  // empty bar: base tick only
            }
        }
    } else {
        textAt(BAR_WIFI_X, BAR_TEXT_Y, "--");
    }

    if (!gCtx.gnssEnabled) {
        snprintf(buf, sizeof(buf), "G--");
    } else if (gCtx.gnssFix) {
        const int quality = gnssQuality(true, gCtx.gnssSatellites);
        snprintf(buf, sizeof(buf), "G%s%s", quality >= 2 ? "ok" : "lo",
                 gCentreOnGnss ? "*" : "");
    } else {
        snprintf(buf, sizeof(buf), "G..");
    }
    textAt(BAR_GNSS_X, BAR_TEXT_Y, buf);
    drawVDivider(BAR_DIV_3);

    // -- time and power.
    if (gCtx.clockValid) {
        snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(gCtx.clockHour),
                 static_cast<unsigned>(gCtx.clockMinute));
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    textAt(BAR_CLOCK_X, BAR_TEXT_Y, buf);

    drawBatteryGlyph(BAR_BATT_X, BAR_TEXT_Y, gCtx.batteryValid, gCtx.batteryPercent);
    if (gCtx.batteryValid) {
        snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(gCtx.batteryPercent));
    } else {
        snprintf(buf, sizeof(buf), "--%%");
    }
    textRight(SCREEN_W - 2, BAR_TEXT_Y, buf);

    d.drawFastHLine(0, STATUS_H, SCREEN_W, BLACK);
}

void drawFooterFrame() {
    g().drawFastHLine(0, FOOTER_Y - 1, SCREEN_W, BLACK);
}

void drawFooterLines(const char *line1, const char *line2) {
    drawFooterFrame();
    if (line1) textAt(2, FOOTER_Y + 3, line1);
    if (line2) textAt(2, FOOTER_Y + 12, line2);
}

// Footer showing the current selection, or the key legend when nothing is
// selected.
void drawSelectionFooter(const char *hints) {
    const Aircraft *a = selected();
    if (!a) {
        drawFooterLines(hints, nullptr);
        return;
    }

    char alt[12];
    formatAltitude(*a, alt, sizeof(alt));

    char line1[48];
    snprintf(line1, sizeof(line1), "%-8s %-6s%c %3.0f%s", a->label(), alt,
             verticalTrendChar(*a), geo::displaySpeed(a->groundSpeedKt),
             geo::speedUnitLabel());

    char line2[48];
    if (a->hasPosition) {
        snprintf(line2, sizeof(line2), "%-4s %5.1f%s %03.0f %s", a->type,
                 geo::displayDistance(a->distanceNm), geo::distanceUnitLabel(),
                 a->bearingDeg, geo::compassPoint(a->bearingDeg));
    } else {
        snprintf(line2, sizeof(line2), "%-4s  no position", a->type);
    }
    drawFooterLines(line1, line2);
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

    // Range rings at a third, two thirds and full range.
    for (int i = 1; i <= 3; ++i) {
        const int16_t r = static_cast<int16_t>(RADAR_R * i / 3);
        d.drawCircle(RADAR_CX, RADAR_CY, r, BLACK);
        snprintf(buf, sizeof(buf), "%.0f", geo::displayDistance(
                                               currentRangeNm() * i / 3.0f));
        textAt(static_cast<int16_t>(RADAR_CX + 3),
               static_cast<int16_t>(RADAR_CY - r - 1), buf);
    }

    // Cardinal ticks.
    d.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_R - 4, 8, BLACK);
    d.drawFastVLine(RADAR_CX, RADAR_CY + RADAR_R - 4, 8, BLACK);
    d.drawFastHLine(RADAR_CX - RADAR_R - 4, RADAR_CY, 8, BLACK);
    d.drawFastHLine(RADAR_CX + RADAR_R - 4, RADAR_CY, 8, BLACK);
    textAt(RADAR_CX - 2, RADAR_CY - RADAR_R - 13, "N");
    textAt(RADAR_CX - 2, RADAR_CY + RADAR_R + 6, "S");
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

    if (!gCtx.tracker) return;

    const int selIdx = selectionIndex();
    int labelled = 0;
    int plotted = 0;

    for (size_t i = 0; i < gCtx.tracker->count(); ++i) {
        const Aircraft &a = gCtx.tracker->at(i);
        int16_t x = 0, y = 0;
        if (!radarPosition(a, &x, &y)) continue;
        ++plotted;

        const bool isSelected = (static_cast<int>(i) == selIdx);
        drawAircraftMarker(x, y, a, isSelected);

        if (labelled < kMaxLabels || isSelected) {
            char alt[12];
            formatAltitude(a, alt, sizeof(alt));

            // Nudge labels back inside the panel so long callsigns stay legible.
            int16_t lx = x + 7;
            const int16_t widest = static_cast<int16_t>(
                strlen(a.label()) > strlen(alt) ? strlen(a.label()) : strlen(alt));
            if (lx + widest * CHAR_W > SCREEN_W - 2) {
                lx = static_cast<int16_t>(x - 7 - widest * CHAR_W);
            }
            if (lx < 1) lx = 1;

            textAt(lx, static_cast<int16_t>(y - 8), a.label());
            textAt(lx, static_cast<int16_t>(y + 1), alt);
            ++labelled;
        }
    }

    // The range now reads out in the status bar, so all this corner has left
    // to say is how much traffic falls outside the plot.
    const int hidden = static_cast<int>(gCtx.tracker->count()) - plotted;
    if (hidden > 0) {
        snprintf(buf, sizeof(buf), "+%d out", hidden);
        textRight(SCREEN_W - 2, CONTENT_Y + 2, buf);
    }

    drawRadarButtons();

    // Naming the feed in the hint line does double duty: it says which source
    // the plot came from, and it says which key changes it. Dragging is the
    // one thing on this screen nobody needs telling about.
    char hints[48];
    snprintf(hints, sizeof(hints), "p:%s  t:list  z/x:zoom  m:map",
             adsb::providerName());
    drawSelectionFooter(isPanned() ? "drag:pan  c:recentre  z/x:zoom" : hints);
}

// ---------------------------------------------------------- list view ------
void drawList() {
    Adafruit_GFX &d = g();
    char buf[64];

    d.fillRect(0, CONTENT_Y, SCREEN_W, ROW_H, BLACK);
    textAt(2, CONTENT_Y + 2, "CALLSIGN  ALT    SPD  DIST  BRG", WHITE);

    if (!gCtx.tracker || gCtx.tracker->count() == 0) {
        textAt(2, CONTENT_Y + ROW_H + 8,
               gCtx.wifiConnected ? "No traffic in range." : "Waiting for Wi-Fi...");
        drawFooterLines("t:radar  r:refresh  i:diag", nullptr);
        return;
    }

    const int rows = (CONTENT_H - ROW_H) / ROW_H;
    const int total = static_cast<int>(gCtx.tracker->count());
    const int selIdx = selectionIndex();

    // Keep the selection on screen.
    if (selIdx >= 0) {
        if (selIdx < gListTop) gListTop = selIdx;
        if (selIdx >= gListTop + rows) gListTop = selIdx - rows + 1;
    }
    if (gListTop > total - rows) gListTop = total - rows;
    if (gListTop < 0) gListTop = 0;

    for (int r = 0; r < rows; ++r) {
        const int idx = gListTop + r;
        if (idx >= total) break;
        const Aircraft &a = gCtx.tracker->at(static_cast<size_t>(idx));
        const int16_t y = static_cast<int16_t>(CONTENT_Y + ROW_H + r * ROW_H);
        const bool isSelected = (idx == selIdx);

        if (isSelected) d.fillRect(0, y, SCREEN_W, ROW_H, BLACK);

        char alt[12];
        formatAltitude(a, alt, sizeof(alt));

        if (a.hasPosition) {
            snprintf(buf, sizeof(buf), "%-8s %-6s%c %3.0f %5.1f %03.0f", a.label(),
                     alt, verticalTrendChar(a), geo::displaySpeed(a.groundSpeedKt),
                     geo::displayDistance(a.distanceNm), a.bearingDeg);
        } else {
            snprintf(buf, sizeof(buf), "%-8s %-6s%c %3.0f   --    --", a.label(),
                     alt, verticalTrendChar(a), geo::displaySpeed(a.groundSpeedKt));
        }
        textAt(2, static_cast<int16_t>(y + 2), buf, isSelected ? WHITE : BLACK);
    }

    if (total > rows) {
        // Scroll position indicator down the right edge.
        const int16_t trackTop = CONTENT_Y + ROW_H;
        const int16_t trackH = static_cast<int16_t>(rows * ROW_H);
        const int16_t barH = static_cast<int16_t>(
            trackH * rows / total > 4 ? trackH * rows / total : 4);
        const int16_t barY = static_cast<int16_t>(
            trackTop + (trackH - barH) * gListTop / (total - rows));
        d.fillRect(SCREEN_W - 3, barY, 3, barH, BLACK);
    }

    drawSelectionFooter("t:radar  w/s:move  E:info  r:refresh");
}

// -------------------------------------------------------- detail view ------
void drawDetail() {
    const Aircraft *a = selected();
    if (!a) {
        textAt(4, CONTENT_Y + 8, "Nothing selected.");
        drawFooterLines("U:back", nullptr);
        return;
    }

    char buf[64];
    int16_t y = CONTENT_Y + 6;

    textAt(4, y, a->label(), BLACK, 2);
    y += 20;

    // What the aircraft actually is, in words. The feed never carries this --
    // an aggregator sends a four-letter type designator at best and a local
    // receiver sends nothing at all -- so it comes from the flashed table.
    // Looked up here rather than kept on every Aircraft: the detail page shows
    // one target, where the tracker holds MAX_AIRCRAFT of them.
    aircraftdb::Details db;
    const bool known = aircraftdb::details(a->hex, &db);

    if (known && db.desc[0]) {
        // 40 columns at this size; descriptions run to 47, so wrap once on a
        // space rather than truncating mid-word.
        const int kCols = SCREEN_W / CHAR_W - 1;
        const int len = static_cast<int>(strlen(db.desc));
        if (len <= kCols) {
            textAt(4, y, db.desc);
            y += 10;
        } else {
            int split = kCols;
            while (split > 0 && db.desc[split] != ' ') --split;
            if (split == 0) split = kCols;  // one very long word
            char line[48];
            const size_t n = static_cast<size_t>(split) < sizeof(line)
                                 ? static_cast<size_t>(split)
                                 : sizeof(line) - 1;
            memcpy(line, db.desc, n);
            line[n] = ' ';
            textAt(4, y, line);
            y += 10;
            textAt(4, y, db.desc + split + (db.desc[split] == ' ' ? 1 : 0));
            y += 10;
        }
    }

    // Who operates it, on its own full-width line rather than as a row: these
    // run to 47 characters where every row value fits in nine, and giving it a
    // column would have squeezed everything else or collided with the
    // silhouette on the right.
    if (known && db.op[0]) {
        snprintf(buf, sizeof(buf), "%.39s", db.op);
        textAt(4, y, buf);
        y += 10;
    }

    g().drawFastHLine(4, y, SCREEN_W - 8, BLACK);
    y += 6;

    // Plan-view silhouette, right-aligned against the rows below. Drawn from
    // the feed's type designator where it has one and the database's
    // otherwise, so it still appears for a local receiver that sends neither.
    const char *iconType = a->type[0] ? a->type : (known ? db.type : "");
    int16_t iconW = 0, iconH = 0;
    if (iconType[0] && icons::size(iconType, &iconW, &iconH)) {
        icons::draw(iconType, static_cast<int16_t>(SCREEN_W - 4 - iconW),
                    static_cast<int16_t>(y + 2));
    }

    auto row = [&](const char *label, const char *value) {
        textAt(4, y, label);
        textAt(4 + 11 * CHAR_W, y, value);
        y += 12;
    };

    row("ICAO", a->hex);
    // The feed's value wins where it has one; the table fills the gap it
    // leaves. Neither is second-guessed against the other.
    row("Reg", a->reg[0] ? a->reg : (known && db.reg[0] ? db.reg : "--"));
    row("Type", a->type[0] ? a->type : (known && db.type[0] ? db.type : "--"));

    if (known && db.year != 0) {
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(db.year));
        row("Built", buf);
    }

    row("Squawk", a->squawk[0] ? a->squawk : "--");

    char alt[12];
    formatAltitude(*a, alt, sizeof(alt));
    snprintf(buf, sizeof(buf), "%s", alt);
    row("Altitude", buf);

    snprintf(buf, sizeof(buf), "%+d fpm", static_cast<int>(a->verticalRateFpm));
    row("V/S", buf);

    snprintf(buf, sizeof(buf), "%.0f %s", geo::displaySpeed(a->groundSpeedKt),
             geo::speedUnitLabel());
    row("Speed", buf);

    if (a->hasTrack) {
        snprintf(buf, sizeof(buf), "%03.0f %s", a->trackDeg,
                 geo::compassPoint(a->trackDeg));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    row("Track", buf);

    if (a->hasPosition) {
        snprintf(buf, sizeof(buf), "%.1f %s", geo::displayDistance(a->distanceNm),
                 geo::distanceUnitLabel());
        row("Range", buf);

        snprintf(buf, sizeof(buf), "%03.0f %s", a->bearingDeg,
                 geo::compassPoint(a->bearingDeg));
        row("Bearing", buf);

        snprintf(buf, sizeof(buf), "%.4f", a->lat);
        row("Lat", buf);
        snprintf(buf, sizeof(buf), "%.4f", a->lon);
        row("Lon", buf);

        snprintf(buf, sizeof(buf), "%.0f s ago", a->seenPosSec);
        row("Pos age", buf);
    } else {
        row("Position", "not reported");
    }

    if (a->emergency) {
        y += 4;
        g().fillRect(4, y, SCREEN_W - 8, 12, BLACK);
        textAt(8, y + 2, "EMERGENCY / SPECIAL SQUAWK", WHITE);
    }

    drawFooterLines("U:back  w/s:next target", nullptr);
}

// -------------------------------------------------------- status view ------
void drawStatus() {
    char buf[64];
    int16_t y = CONTENT_Y + 6;

    textAt(4, y, "Diagnostics", BLACK, 2);
    y += 22;

    auto row = [&](const char *label, const char *value) {
        textAt(4, y, label);
        textAt(4 + 11 * CHAR_W, y, value);
        y += 12;
    };

    row("Firmware", "adsb 0.1");
    row("Feed", adsb::providerName());

    const adsb::FetchStats &f = gCtx.lastFetch;
    switch (f.result) {
        case adsb::Result::Ok:
            // A local receiver serves everything it hears, so the count it
            // sent and the count we kept are different numbers worth seeing.
            if (f.filtered > 0) {
                snprintf(buf, sizeof(buf), "ok %u ac -%u far",
                         static_cast<unsigned>(f.stored),
                         static_cast<unsigned>(f.filtered));
            } else {
                snprintf(buf, sizeof(buf), "ok %u ac",
                         static_cast<unsigned>(f.stored));
            }
            break;
        case adsb::Result::HttpError:
            snprintf(buf, sizeof(buf), "HTTP %d", f.httpStatus);
            break;
        case adsb::Result::ParseError:
            snprintf(buf, sizeof(buf), "parse error");
            break;
        case adsb::Result::NotConnected:
            snprintf(buf, sizeof(buf), "no network");
            break;
        case adsb::Result::RateLimited:
            snprintf(buf, sizeof(buf), "throttled");
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
    row("Position", gCtx.ownFromGnss ? "GNSS" : "config");

    if (isPanned()) {
        snprintf(buf, sizeof(buf), "%+.0fE %+.0fN nm", gPanEastNm, gPanNorthNm);
    } else {
        snprintf(buf, sizeof(buf), "centred");
    }
    row("Pan", buf);

    snprintf(buf, sizeof(buf), "%u nm", static_cast<unsigned>(currentRangeNm()));
    row("Range", buf);

    if (gCtx.batteryValid) {
        snprintf(buf, sizeof(buf), "%u%%  %u mV",
                 static_cast<unsigned>(gCtx.batteryPercent),
                 static_cast<unsigned>(gCtx.batteryMilliVolts));
    } else {
        snprintf(buf, sizeof(buf), "gauge silent");
    }
    row("Battery", buf);

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

    snprintf(buf, sizeof(buf), "%u k / %u k",
             static_cast<unsigned>(ESP.getFreeHeap() / 1024),
             static_cast<unsigned>(ESP.getFreePsram() / 1024));
    row("Free RAM", buf);

    drawFooterLines("U:back  g:GNSS centre  l:backlight", nullptr);
}

void drawCurrentView() {
    drawStatusBar();
    switch (gView) {
        case View::Radar: drawRadar(); break;
        case View::List: drawList(); break;
        case View::Detail: drawDetail(); break;
        case View::Status: drawStatus(); break;
    }
}

void setView(View v) {
    if (v == gView) return;
    gPreviousView = gView;
    gView = v;
    // A view change replaces the whole screen, so clear the ghosting with it.
    display::invalidate(true);
}

void goBack() {
    if (gView == View::Detail || gView == View::Status) {
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

uint16_t rangeNm() { return currentRangeNm(); }

void handleKey(char key) {
    if (key == 0) return;

    switch (key) {
        case 't':
            setView(gView == View::Radar ? View::List : View::Radar);
            break;

        // WASD pans the plot on the radar. In the list and detail views there
        // is nothing to pan, so w/s fall back to moving the selection -- which
        // is what those keys mean everywhere else.
        case 'w':
            if (gView == View::Radar) {
                panByNm(0.0f, panStepNm());
            } else {
                moveSelection(-1);
                display::invalidate();
            }
            break;
        case 's':
            if (gView == View::Radar) {
                panByNm(0.0f, -panStepNm());
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

        case 'k':
            moveSelection(-1);
            display::invalidate();
            break;
        case 'j':
            moveSelection(1);
            display::invalidate();
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
            // one would be meaningless.
            gPanEastNm = 0.0f;
            gPanNorthNm = 0.0f;
            persist();
            display::invalidate();
            break;

        case 'l':
            power::setKeypadBacklight(!power::keypadBacklight());
            persist();
            break;

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

        default:
            break;
    }
}

// A press and release in roughly the same place.
void handleTap(int16_t x, int16_t y) {
    // On the detail and diagnostics pages both strips of chrome mean "back".
    // Swapping radar for list is meaningless from a page that is neither, and
    // the footer legend on both of them literally reads "U:back" -- so tapping
    // it has to do that. It used to ask for the detail view, which from the
    // detail view is a no-op, leaving the one strip of screen that says "back"
    // as the one strip that ignored you.
    const bool subPage = (gView == View::Detail || gView == View::Status);

    if (y < STATUS_H) {
        if (subPage) goBack();
        else setView(gView == View::Radar ? View::List : View::Radar);
        return;
    }

    if (y >= FOOTER_Y) {
        if (subPage) goBack();
        else if (selected()) setView(View::Detail);
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
        }
        return;
    }

    if (gView == View::List && gCtx.tracker) {
        const int row = (y - CONTENT_Y - ROW_H) / ROW_H;
        if (row < 0) return;  // header
        const int idx = gListTop + row;
        if (idx >= static_cast<int>(gCtx.tracker->count())) return;
        if (idx == selectionIndex()) {
            setView(View::Detail);
        } else {
            selectIndex(idx);
            display::invalidate();
        }
        return;
    }

    if (gView == View::Detail || gView == View::Status) goBack();
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
    if (gView == View::Radar && gTouchDownY >= STATUS_H && gTouchDownY < FOOTER_Y) {
        const float scale = pixelsPerNm();
        panByNm(-dx / scale, dy / scale);
    }
}

bool tick() {
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
    mix(static_cast<uint32_t>(gListTop));
    mix(static_cast<uint32_t>(gMapEnabled));
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
    mix(static_cast<uint32_t>(gCtx.wifiConnected ? wifiBars(gCtx.wifiRssi) : -1));
    mix(static_cast<uint32_t>(gCtx.gnssFix));
    mix(static_cast<uint32_t>(gnssQuality(gCtx.gnssFix, gCtx.gnssSatellites)));
    mix(feedAgeBucket());
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
    }

    return display::render(h, drawCurrentView);
}

}  // namespace ui
