#include "power.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "config.h"
#include "als.h"

namespace power {
namespace {

bool gKeypadBacklight = false;
bool gBoosted = true;  // clock stays at the 240 MHz boot frequency

// Last gauge report, kept so the system page can show why the status-bar
// percent and the chip disagree.
uint8_t gGaugePercent = 255;
uint16_t gDesignMah = 0;

// 305070 cell fitted to the T-Deck Pro. The gauge's own design-capacity
// register is trusted only when it has been programmed for this pack.
constexpr uint16_t kPackMah = 1400;
constexpr uint16_t kPackMahMin = 1200;
constexpr uint16_t kPackMahMax = 1600;

// One claim held from boot, released at the end of setup().
int gBoostClaims = 1;
portMUX_TYPE gBoostMux = portMUX_INITIALIZER_UNLOCKED;

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

bool gaugeCommand(uint16_t command) {
    Wire.beginTransmission(ADDR_GAUGE_BQ27220);
    Wire.write(0x00);
    Wire.write(static_cast<uint8_t>(command));
    Wire.write(static_cast<uint8_t>(command >> 8));
    const bool ok = Wire.endTransmission() == 0;
    delay(10);
    return ok;
}

bool waitGaugeConfig(bool enabled) {
    const uint32_t started = millis();
    do {
        uint16_t status = 0;
        if (readReg16(ADDR_GAUGE_BQ27220, 0x3A, &status) &&
            ((status & 0x0400) != 0) == enabled) return true;
        delay(50);
    } while (millis() - started < 3000);
    return false;
}

// Data-memory addresses are little-endian; their I2 capacity values are
// big-endian. Commit the four-byte address/data payload with checksum/length.
bool writeGaugeCapacity(uint16_t address, uint16_t capacity) {
    const uint8_t bytes[] = {static_cast<uint8_t>(address),
                             static_cast<uint8_t>(address >> 8),
                             static_cast<uint8_t>(capacity >> 8),
                             static_cast<uint8_t>(capacity)};
    Wire.beginTransmission(ADDR_GAUGE_BQ27220);
    Wire.write(0x3E);
    Wire.write(bytes, sizeof(bytes));
    if (Wire.endTransmission() != 0) return false;
    delay(10);
    const uint8_t sum = static_cast<uint8_t>(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
    Wire.beginTransmission(ADDR_GAUGE_BQ27220);
    Wire.write(0x60);
    Wire.write(static_cast<uint8_t>(0xFF - sum));
    Wire.write(6);
    if (Wire.endTransmission() != 0) return false;
    delay(10);
    return true;
}

void configureGauge() {
    uint16_t design = 0, full = 0, status = 0;
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x3C, &design) ||
        !readReg16(ADDR_GAUGE_BQ27220, 0x12, &full) ||
        !readReg16(ADDR_GAUGE_BQ27220, 0x3A, &status)) {
        log_w("battery: gauge unavailable during configuration");
        return;
    }
    // Preserve learned full-charge capacity once the pack is configured.
    if (design == kPackMah) {
        log_i("battery: gauge configured, design %u mAh, full %u mAh", design, full);
        return;
    }
    const bool wasSealed = ((status >> 1) & 3) == 3;
    bool ok = true;
    if (wasSealed) {
        // LilyGO/SensorLib key, then TI ROM default if still sealed.
        ok = gaugeCommand(0x0414) && gaugeCommand(0x3672);
        ok = ok && readReg16(ADDR_GAUGE_BQ27220, 0x3A, &status);
        if (ok && ((status >> 1) & 3) == 3)
            ok = gaugeCommand(0x8000) && gaugeCommand(0x8000);
    }
    ok = ok && readReg16(ADDR_GAUGE_BQ27220, 0x3A, &status);
    if (ok && ((status >> 1) & 3) != 1)
        ok = gaugeCommand(0xFFFF) && gaugeCommand(0xFFFF);
    ok = ok && readReg16(ADDR_GAUGE_BQ27220, 0x3A, &status) &&
         ((status >> 1) & 3) == 1;
    ok = ok && gaugeCommand(0x0090) && waitGaugeConfig(true);
    if (ok) {
        ok = writeGaugeCapacity(0x929D, kPackMah) &&
             writeGaugeCapacity(0x929F, kPackMah);
    }
    // Always attempt to leave update mode, including after a failed write.
    const bool exited = gaugeCommand(0x0091) && waitGaugeConfig(false);
    if (wasSealed) gaugeCommand(0x0030);
    delay(100);
    ok = ok && exited && readReg16(ADDR_GAUGE_BQ27220, 0x3C, &design) &&
         readReg16(ADDR_GAUGE_BQ27220, 0x12, &full) &&
         design == kPackMah && full == kPackMah;
    if (ok) {
        log_i("battery: configured and verified design %u mAh, full %u mAh", design, full);
    } else {
        log_w("battery: capacity configuration failed (design %u, full %u mAh)", design, full);
    }
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

    // REG06[7:2] is the charge-voltage limit: 3840 mV + n*16 mV. Left at the
    // chip default the pack stops short of 4.20 V and a full battery never
    // reads as full. 4208 mV is the step LilyGO uses for this cell. Bits 1:0
    // (precharge threshold, recharge offset) are left alone.
    uint8_t reg06 = 0;
    if (readReg8(ADDR_PMU_BQ25896, 0x06, &reg06)) {
        constexpr uint8_t kVreg = (4208 - 3840) / 16;
        const uint8_t next = static_cast<uint8_t>((reg06 & 0x03) | (kVreg << 2));
        if (next != reg06) writeReg8(ADDR_PMU_BQ25896, 0x06, next);
    }
}

// Resting single-cell LiPo open-circuit voltage. Used when the gauge's
// design capacity is not this pack, because its SOC is then scaled to the
// wrong cell. Under load the voltage sags and the percent dips with it;
// that is still closer than a 3000 mAh scale on a 1400 mAh cell.
uint8_t percentFromMilliVolts(uint16_t mv) {
    struct Step {
        uint16_t mv;
        uint8_t pct;
    };
    constexpr Step kCurve[] = {
        {3400, 0},  {3600, 10}, {3680, 20}, {3720, 30}, {3760, 40}, {3800, 50},
        {3860, 60}, {3920, 70}, {3980, 80}, {4060, 90}, {4180, 100},
    };
    if (mv <= kCurve[0].mv) return 0;
    constexpr int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (mv >= kCurve[n - 1].mv) return 100;
    for (int i = 1; i < n; ++i) {
        if (mv > kCurve[i].mv) continue;
        const uint16_t span = kCurve[i].mv - kCurve[i - 1].mv;
        const uint16_t into = mv - kCurve[i - 1].mv;
        const uint8_t dp = kCurve[i].pct - kCurve[i - 1].pct;
        return static_cast<uint8_t>(kCurve[i - 1].pct + (into * dp) / span);
    }
    return 100;
}

}  // namespace

void begin() {
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, 400000);
    // Without a timeout a stuck I2C slave (touch/IMU after a glitch) parks
    // the Arduino loop forever and the panel looks frozen on its last frame.
    Wire.setTimeOut(50);

    pinMode(BOARD_KEYPAD_LED, OUTPUT);
    setKeypadBacklight(false);

    // The SX1262 and the A7682E modem are unused by this firmware. Holding
    // their rails off saves roughly 20 mA and keeps the LoRa transceiver off
    // the shared SPI bus while the e-paper is being written. The IMU's 1.8 V
    // rail is raised further down -- it is used.
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, LOW);

    // Every CS on the shared SPI bus must sit high before SPI.begin(). LilyGO
    // documents this explicitly; a low or floating line lets that device fight
    // the e-paper for MISO.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT);
    digitalWrite(BOARD_EPD_CS, HIGH);

    pinMode(BOARD_GPS_EN, OUTPUT);
    digitalWrite(BOARD_GPS_EN, GNSS_ENABLED ? HIGH : LOW);

    // BHI260AP sits on its own 1.8 V rail. Raise it here so imu::begin() can
    // talk to the chip; without this the address simply never ACKs. ALS lives
    // on the always-on 3.3 V rail and needs no help.
    pinMode(BOARD_1V8_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);

    configureCharger();
    configureGauge();
}

bool readBattery(uint8_t *percent, uint16_t *milliVolts) {
    uint16_t soc = 0;
    uint16_t mv = 0;
    uint16_t design = 0;
    // 0x08 voltage (mV), 0x2C state of charge (%), 0x3C design capacity (mAh).
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x08, &mv)) return false;
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x2C, &soc)) return false;
    if (!readReg16(ADDR_GAUGE_BQ27220, 0x3C, &design)) return false;

    gDesignMah = design;
    gGaugePercent = soc <= 100 ? static_cast<uint8_t>(soc) : 255;
    static bool loggedReading = false;
    if (!loggedReading) {
        loggedReading = true;
        log_i("battery: gauge SOC %u%%, voltage %u mV, design %u mAh", soc, mv, design);
    }

    const bool packMatches = design >= kPackMahMin && design <= kPackMahMax;
    // 0% while the cell is still above 3.55 V is the unprogrammed end-of-
    // discharge threshold, not an empty battery.
    const bool gaugeLie = soc == 0 && mv >= 3550;
    uint8_t shown;
    if (packMatches && soc <= 100 && !gaugeLie) {
        shown = static_cast<uint8_t>(soc);
    } else {
        shown = percentFromMilliVolts(mv);
        // Once per programmed capacity, not every 30 s poll: the voltage
        // estimate jitters a percent under load and would flood the log.
        static uint16_t loggedDesign = 0xFFFF;
        if (design != loggedDesign) {
            loggedDesign = design;
            log_i("battery: gauge %u%% of %u mAh (pack is %u), showing %u%% from %u mV",
                  static_cast<unsigned>(soc > 100 ? 255 : soc),
                  static_cast<unsigned>(design), static_cast<unsigned>(kPackMah),
                  static_cast<unsigned>(shown), static_cast<unsigned>(mv));
        }
    }

    if (percent) *percent = shown;
    if (milliVolts) *milliVolts = mv;
    return true;
}

uint8_t gaugePercent() { return gGaugePercent; }

uint16_t designMilliAmpHours() { return gDesignMah; }

void setKeypadBacklight(bool on) {
    gKeypadBacklight = on;
    digitalWrite(BOARD_KEYPAD_LED, on ? HIGH : LOW);
}

bool keypadBacklight() { return gKeypadBacklight; }

void refreshKeypadBacklight(uint8_t mode, bool forceOff) {
    // 0 = off, 1 = on, anything else = auto. Numbers rather than the
    // BacklightMode enum so this file does not depend on settings.h.
    if (forceOff || mode == 0) {
        setKeypadBacklight(false);
        return;
    }
    if (mode == 1) {
        setKeypadBacklight(true);
        return;
    }

    float lux = 0.0f;
    if (!als::lux(&lux)) {
        setKeypadBacklight(false);
        return;
    }
    if (lux <= KEYPAD_BACKLIGHT_AUTO_ON_LX) {
        setKeypadBacklight(true);
    } else if (lux >= KEYPAD_BACKLIGHT_AUTO_OFF_LX) {
        setKeypadBacklight(false);
    }
}

namespace {

void applyBoost(bool on) {
    // Stay at the 240 MHz boot clock. Dropping to 80 MHz between UI ticks
    // reconfigures the PLL while Wi-Fi is associated, and after the radio
    // has been idle that switch wedges the Wi-Fi task. The next
    // WiFi.status() from loop() then never returns, which is a panel that
    // sits on its last frame until the battery dies.
    (void)on;
    gBoosted = true;
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
