#include "power.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "config.h"

namespace power {
namespace {

bool gKeypadBacklight = false;
bool gBoosted = true;  // setup() runs at the boot frequency

// One claim held from boot, released at the end of setup().
int gBoostClaims = 1;
portMUX_TYPE gBoostMux = portMUX_INITIALIZER_UNLOCKED;

constexpr uint32_t kIdleCpuMhz = 80;
constexpr uint32_t kBoostCpuMhz = 240;

bool i2cPresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool readReg8(uint8_t addr, uint8_t reg, uint8_t *out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(addr), 1) != 1) return false;
    *out = Wire.read();
    return true;
}

bool writeReg8(uint8_t addr, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

// BQ27220 registers are 16-bit little-endian.
bool readReg16(uint8_t addr, uint8_t reg, uint16_t *out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(addr), 2) != 2) return false;
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    *out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    return true;
}

void configureCharger() {
    if (!i2cPresent(ADDR_PMU_BQ25896)) {
        log_w("BQ25896 not responding; leaving charger at defaults");
        return;
    }
    // REG07[5:4] is the I2C watchdog. Left at its 40 s default the charger
    // silently reverts to register defaults, which on this board shows up as
    // the charge current dropping. Disable it.
    uint8_t reg07 = 0;
    if (readReg8(ADDR_PMU_BQ25896, 0x07, &reg07)) {
        writeReg8(ADDR_PMU_BQ25896, 0x07, static_cast<uint8_t>(reg07 & ~0x30));
    }
}

}  // namespace

void begin() {
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, 400000);

    pinMode(BOARD_KEYPAD_LED, OUTPUT);
    setKeypadBacklight(KEYPAD_BACKLIGHT_DEFAULT);

    // The SX1262 and the A7682E modem are unused by this firmware. Holding
    // their rails off saves roughly 20 mA and keeps them off the shared SPI
    // bus while the e-paper is being written.
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, LOW);

    // SX1262 chip select must be parked high even with the rail down, or it
    // can load the bus the e-paper shares.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);

    pinMode(BOARD_GPS_EN, OUTPUT);
    digitalWrite(BOARD_GPS_EN, GNSS_ENABLED ? HIGH : LOW);

    configureCharger();
}

bool readBattery(uint8_t *percent, uint16_t *milliVolts) {
    uint16_t soc = 0;
    uint16_t mv = 0;
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x2C, &soc)) return false;
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x08, &mv)) return false;
    if (soc > 100) soc = 100;
    if (percent) *percent = static_cast<uint8_t>(soc);
    if (milliVolts) *milliVolts = mv;
    return true;
}

void setKeypadBacklight(bool on) {
    gKeypadBacklight = on;
    digitalWrite(BOARD_KEYPAD_LED, on ? HIGH : LOW);
}

bool keypadBacklight() { return gKeypadBacklight; }

namespace {

void applyBoost(bool on) {
    if (on == gBoosted) return;
    // setCpuFrequencyMhz() is a no-op when the frequency already matches and
    // notifies the peripheral drivers itself, so this is safe to call often.
    if (!setCpuFrequencyMhz(on ? kBoostCpuMhz : kIdleCpuMhz)) return;
    gBoosted = on;
}

}  // namespace

void acquireBoost() {
    bool first = false;
    portENTER_CRITICAL(&gBoostMux);
    first = (gBoostClaims++ == 0);
    portEXIT_CRITICAL(&gBoostMux);
    // Outside the critical section: setCpuFrequencyMhz() walks the peripheral
    // drivers and is far too heavy to hold a spinlock across both cores for.
    if (first) applyBoost(true);
}

void releaseBoost() {
    bool last = false;
    portENTER_CRITICAL(&gBoostMux);
    if (gBoostClaims > 0) last = (--gBoostClaims == 0);
    portEXIT_CRITICAL(&gBoostMux);
    if (last) applyBoost(false);
}

bool boosted() { return gBoosted; }

}  // namespace power
