#include "keypad.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace keypad {
namespace {

constexpr uint8_t kRows = 4;
constexpr uint8_t kCols = 10;

// The TCA8418 FIFO reports a key index with bit 7 set for a press and clear
// for a release. The index is always row-major over ten columns whatever the
// matrix is configured as, so a 4x10 board spans keys 1..40 -- presses
// 129..168, releases 1..40. The ceiling used to be hard-coded at 163, which
// silently discarded the last five keys of the matrix as "not a press".
constexpr int kPressMin = 129;
constexpr int kPressMax = kPressMin + kRows * kCols - 1;
constexpr int kReleaseMin = 1;
constexpr int kReleaseMax = kRows * kCols;

const char kKeymap[kRows][kCols] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '0'},
    {'2', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', 'E'},
    {' ', ' ', ' ', ' ', ' ', '-', '*', 'S', '0', 'U'},
};

// The matrix is read over the same I2C bus as the touch panel and the PMU;
// checking it every loop iteration was pure bus traffic for no benefit.
constexpr uint32_t kPollIntervalMs = 25;

LastKey gLastKey = {0, -1, -1, 0};

Adafruit_TCA8418 gDevice;
bool gPresent = false;
uint32_t gLastPollMs = 0;

}  // namespace

bool begin() {
    if (!gDevice.begin(ADDR_KEYPAD_TCA8418, &Wire)) {
        log_e("TCA8418 keypad not found at 0x%02X", ADDR_KEYPAD_TCA8418);
        gPresent = false;
        return false;
    }
    gDevice.matrix(kRows, kCols);
    gDevice.flush();
    gPresent = true;
    return true;
}

bool present() { return gPresent; }

const LastKey &lastKey() { return gLastKey; }

char poll() {
    if (!gPresent) return 0;

    const uint32_t now = millis();
    if (now - gLastPollMs < kPollIntervalMs) return 0;
    gLastPollMs = now;

    if (gDevice.available() == 0) return 0;

    const int event = gDevice.getEvent();
    if (event < kPressMin || event > kPressMax) {
        // Key-up (or a GPI event we did not ask for): consume and ignore.
        (void)kReleaseMin;
        (void)kReleaseMax;
        gLastKey = {0, -1, -1, static_cast<int16_t>(event)};
        log_d("keypad: ignored raw=%d", event);
        return 0;
    }

    const int key = event - kPressMin;
    const int row = key / kCols;
    const int col = (kCols - 1) - (key % kCols);
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        log_d("keypad: out of range raw=%d -> (%d,%d)", event, row, col);
        return 0;
    }

    const char c = kKeymap[row][col];
    gLastKey = {c, static_cast<int8_t>(row), static_cast<int8_t>(col),
                static_cast<int16_t>(event)};
    log_d("keypad: '%c' (%d,%d) raw=%d", c, row, col, event);
    return c;
}

}  // namespace keypad
