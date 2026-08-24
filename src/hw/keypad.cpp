#include "keypad.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace keypad {
namespace {

constexpr uint8_t kRows = 4;
constexpr uint8_t kCols = 10;

// The TCA8418 FIFO encodes key-down as 129..163 and key-up as 1..35, both
// offset from a row-major key index. Column order is reversed on this board.
constexpr int kPressMin = 129;
constexpr int kPressMax = 163;
constexpr int kReleaseMin = 1;
constexpr int kReleaseMax = 35;

const char kKeymap[kRows][kCols] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '0'},
    {'2', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', 'E'},
    {' ', ' ', ' ', ' ', ' ', '-', '*', 'S', '0', 'U'},
};

// The matrix is read over the same I2C bus as the touch panel and the PMU;
// checking it every loop iteration was pure bus traffic for no benefit.
constexpr uint32_t kPollIntervalMs = 25;

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
        return 0;
    }

    const int key = event - kPressMin;
    const int row = key / kCols;
    const int col = (kCols - 1) - (key % kCols);
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) return 0;

    const char c = kKeymap[row][col];
    log_d("keypad: '%c' (%d,%d)", c, row, col);
    return c;
}

}  // namespace keypad
