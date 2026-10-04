#include "display.h"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

// GxEPD2 keeps the 1-bit page buffer private. page_height == HEIGHT, so that
// buffer is the whole 240x320 frame, which is what a PBM dump needs. The
// define is scoped to this include; nothing else in the firmware uses it.
#define private public
#include <GxEPD2_BW.h>
#undef private

#include "board_pins.h"
#include "config.h"
#include "hw/spibus.h"

namespace display {
namespace {

// page_height == HEIGHT gives a single full-screen page: 240 * 320 / 8 =
// 9600 bytes, which is cheap enough to keep resident and means each view can
// draw in one pass instead of being re-entered per band.
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> gEpd(
    GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST,
                          BOARD_EPD_BUSY));

uint32_t gLastHash = 0;
bool gHaveLastHash = false;
uint16_t gPartialsSinceFull = 0;
uint32_t gLastFullRefreshMs = 0;
uint32_t gLastRenderMs = 0;
bool gForceRepaint = true;
bool gForceFull = true;

}  // namespace

void begin() {
    spibus::begin();
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    // Match LilyGO's factory clock: 2 MHz is enough for the UC8253 and keeps
    // the shared bus well clear of the SD card's 10 MHz setting.
    gEpd.epd2.selectSPI(SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));
    gEpd.init(115200, true, 2, false);
    // BUSY is active-low and the panel stops driving it once powered down.
    // A floating read looks "busy", and every later refresh then sits in
    // GxEPD2's 10 s timeout. Pull the line up so idle reads as idle.
    pinMode(BOARD_EPD_BUSY, INPUT_PULLUP);
    gEpd.setRotation(0);  // portrait, 240 x 320, keyboard at the bottom
    gEpd.setTextColor(GxEPD_BLACK);
    gEpd.setTextWrap(false);
    gEpd.setFullWindow();
    gEpd.firstPage();
    do {
        gEpd.fillScreen(GxEPD_WHITE);
    } while (gEpd.nextPage());
    gLastFullRefreshMs = millis();
}

Adafruit_GFX &gfx() { return gEpd; }

void invalidate(bool forceFullRefresh) {
    gForceRepaint = true;
    if (forceFullRefresh) gForceFull = true;
}

bool render(uint32_t sceneHash, void (*draw)()) {
    const uint32_t now = millis();

    if (!gForceRepaint) {
        if (gHaveLastHash && sceneHash == gLastHash) return false;
        // The scene really did change, but not long enough ago to be worth a
        // 0.7 s flash. Leave it pending; the next call will pick it up.
        if (gLastRenderMs != 0 && now - gLastRenderMs < EPD_MIN_REFRESH_INTERVAL_MS) {
            return false;
        }
    }
    const bool full = gForceFull ||
                      gPartialsSinceFull >= EPD_FULL_REFRESH_EVERY ||
                      (now - gLastFullRefreshMs) > EPD_FULL_REFRESH_MAX_AGE_MS;

    if (full) {
        gEpd.setFullWindow();
    } else {
        gEpd.setPartialWindow(0, 0, gEpd.width(), gEpd.height());
    }

    spibus::Lock lock;

    gEpd.firstPage();
    do {
        gEpd.fillScreen(GxEPD_WHITE);
        draw();
    } while (gEpd.nextPage());

    // GxEPD2 only powers the panel down on its full-refresh path; after a
    // partial it leaves the UC8253 booster running. Nineteen of every twenty
    // refreshes here are partial, so without this the panel is powered
    // continuously between updates for no benefit. The previous-image RAM is
    // already restored by nextPage()'s writeImageAgain(), and powerOff() (as
    // opposed to hibernate()) keeps it, so the next partial still has its
    // baseline.
    gEpd.powerOff();

    if (full) {
        gPartialsSinceFull = 0;
        gLastFullRefreshMs = now;
    } else {
        ++gPartialsSinceFull;
    }
    gLastHash = sceneHash;
    gHaveLastHash = true;
    gLastRenderMs = now;
    gForceRepaint = false;
    gForceFull = false;
    return true;
}

void hibernate() {
    spibus::Lock lock;
    gEpd.hibernate();
    // Previous-image RAM on the UC8253 does not survive deep sleep, so the
    // next update cannot be a partial.
    gForceRepaint = true;
    gForceFull = true;
    gHaveLastHash = false;
}

size_t frameBytes() {
    return static_cast<size_t>(EPD_WIDTH / 8) * static_cast<size_t>(EPD_HEIGHT);
}

bool copyFrame(uint8_t *dst, size_t len) {
    if (!dst || len < frameBytes()) return false;
    memcpy(dst, gEpd._buffer, frameBytes());
    return true;
}

}  // namespace display
