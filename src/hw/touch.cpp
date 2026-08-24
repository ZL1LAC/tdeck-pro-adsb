#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace touch {
namespace {

// Register protocol taken from the Hynitron vendor driver LilyGO ships as
// examples/factory/hyn_cst3xx.c. The CST328 answers a 16-bit big-endian
// register address.
constexpr uint16_t kRegTouchData = 0xD000;  // 7-byte first-finger report
constexpr uint16_t kRegNormalMode = 0xD109;
constexpr uint8_t kReportTail = 0xAB;

// Set these if your panel reports mirrored or transposed coordinates.
constexpr bool kSwapXY = false;
constexpr bool kMirrorX = false;
constexpr bool kMirrorY = false;

// Only used while a contact is live -- when idle we wait for the INT line.
constexpr uint32_t kPollIntervalMs = 20;
// A single dropped report should not read as a finger lift, so require the
// contact to be gone for this long before calling it a release.
constexpr uint32_t kReleaseHoldoffMs = 60;

bool gPresent = false;
bool gTouching = false;
volatile bool gIrqPending = false;
uint32_t gLastPollMs = 0;
uint32_t gLastFingerMs = 0;
int16_t gLastX = 0;
int16_t gLastY = 0;

// The CST328 pulls INT low when a report is ready. Reading it over I2C
// regardless was throwing periodic timeouts (Wire error 263) -- the controller
// does not answer while idle.
void IRAM_ATTR onTouchIrq() { gIrqPending = true; }

bool writeRegister(uint16_t reg) {
    Wire.beginTransmission(ADDR_TOUCH_CST328);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    return Wire.endTransmission(true) == 0;
}

bool readRegister(uint16_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(ADDR_TOUCH_CST328);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(ADDR_TOUCH_CST328),
                         static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

// The controller latches its report until we acknowledge it.
void releaseReport() {
    Wire.beginTransmission(ADDR_TOUCH_CST328);
    Wire.write(static_cast<uint8_t>(kRegTouchData >> 8));
    Wire.write(static_cast<uint8_t>(kRegTouchData & 0xFF));
    Wire.write(kReportTail);
    Wire.endTransmission(true);
}

void clampToScreen(int16_t *x, int16_t *y) {
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= EPD_WIDTH) *x = EPD_WIDTH - 1;
    if (*y >= EPD_HEIGHT) *y = EPD_HEIGHT - 1;
}

// Reads the first contact. Returns false when no finger is down.
bool readContact(int16_t *x, int16_t *y) {
    uint8_t buf[7];
    if (!readRegister(kRegTouchData, buf, sizeof(buf))) return false;

    // A valid report ends in 0xAB and never begins with it.
    const bool valid = (buf[6] == kReportTail && buf[0] != kReportTail);
    const uint8_t status = buf[5];
    releaseReport();

    if (!valid) return false;
    if (status & 0x80) return false;        // capacitive key, not the screen
    if ((status & 0x7F) == 0) return false; // no contacts

    int16_t px = static_cast<int16_t>((static_cast<uint16_t>(buf[1]) << 4) |
                                      ((buf[3] >> 4) & 0x0F));
    int16_t py = static_cast<int16_t>((static_cast<uint16_t>(buf[2]) << 4) |
                                      (buf[3] & 0x0F));

    if (kSwapXY) {
        const int16_t t = px;
        px = py;
        py = t;
    }
    if (kMirrorX) px = (EPD_WIDTH - 1) - px;
    if (kMirrorY) py = (EPD_HEIGHT - 1) - py;

    clampToScreen(&px, &py);
    *x = px;
    *y = py;
    return true;
}

}  // namespace

bool begin() {
    pinMode(BOARD_TOUCH_INT, INPUT_PULLUP);
    pinMode(BOARD_TOUCH_RST, OUTPUT);
    digitalWrite(BOARD_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(BOARD_TOUCH_RST, HIGH);
    delay(60);

    Wire.beginTransmission(ADDR_TOUCH_CST328);
    if (Wire.endTransmission() != 0) {
        log_e("CST328 touch not found at 0x%02X", ADDR_TOUCH_CST328);
        gPresent = false;
        return false;
    }

    gPresent = writeRegister(kRegNormalMode);
    if (gPresent) {
        attachInterrupt(digitalPinToInterrupt(BOARD_TOUCH_INT), onTouchIrq,
                        FALLING);
    }
    return gPresent;
}

bool present() { return gPresent; }

bool poll(Event *out) {
    if (!gPresent) return false;

    const uint32_t now = millis();

    // Idle: nothing to do until the controller says so. While a contact is
    // live, keep reading on a timer too -- a stationary finger may stop
    // producing edges, and we would otherwise call it a lift.
    const bool due = gTouching && (now - gLastPollMs) >= kPollIntervalMs;
    if (!gIrqPending && !due) return false;
    gIrqPending = false;
    gLastPollMs = now;

    int16_t x = 0, y = 0;
    if (readContact(&x, &y)) {
        gLastX = x;
        gLastY = y;
        gLastFingerMs = now;
        if (gTouching) return false;  // still the same press, position tracked
        gTouching = true;
        if (out) {
            out->kind = Event::Kind::Down;
            out->x = x;
            out->y = y;
        }
        log_d("touch down: %d,%d", x, y);
        return true;
    }

    if (gTouching && (now - gLastFingerMs) >= kReleaseHoldoffMs) {
        gTouching = false;
        if (out) {
            out->kind = Event::Kind::Up;
            out->x = gLastX;
            out->y = gLastY;
        }
        log_d("touch up: %d,%d", gLastX, gLastY);
        return true;
    }

    return false;
}

}  // namespace touch
