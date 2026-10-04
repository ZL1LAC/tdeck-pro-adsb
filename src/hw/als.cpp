#include "als.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "board_pins.h"

namespace als {
namespace {

// Lite-On LTR-553ALS-WA register map (datasheet §8).
constexpr uint8_t kRegAlsContr = 0x80;
constexpr uint8_t kRegAlsMeasRate = 0x85;
constexpr uint8_t kRegPartId = 0x86;
constexpr uint8_t kRegAlsDataCh1 = 0x88;  // CH1 then CH0, four bytes

constexpr uint8_t kPartIdExpected = 0x92;

// ALS_CONTR: active mode, gain 1X (widest range: 1..64k lux).
constexpr uint8_t kAlsContrActive1x = 0x01;

// ALS_MEAS_RATE: integration 100 ms, repeat 500 ms.
constexpr uint8_t kAlsMeasRate = 0x03;

// Gain multiplier and integration-time factor for the datasheet lux formula
// (Appendix A / ESPHome's LTR-ALS port). ALS_INT is integration_ms / 100.
constexpr float kAlsGain = 1.0f;
constexpr float kAlsInt = 1.0f;  // 100 ms

constexpr uint32_t kPollIntervalMs = 500;

bool gPresent = false;
bool gHaveLux = false;
float gLux = 0.0f;
uint32_t gLastPollMs = 0;

bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(ADDR_ALS_LTR553);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(ADDR_ALS_LTR553);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(ADDR_ALS_LTR553),
                         static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
    return true;
}

float countsToLux(uint16_t ch0, uint16_t ch1) {
    const uint32_t sum = static_cast<uint32_t>(ch0) + ch1;
    if (sum == 0) return 0.0f;

    const float ratio = static_cast<float>(ch1) / static_cast<float>(sum);
    float lux = 0.0f;
    if (ratio < 0.45f) {
        lux = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        lux = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        lux = 0.5926f * ch0 + 0.1185f * ch1;
    } else {
        return 0.0f;
    }
    lux /= (kAlsGain * kAlsInt);
    if (lux < 0.0f) lux = 0.0f;
    return lux;
}

}  // namespace

bool begin() {
    gPresent = false;
    gHaveLux = false;
    gLux = 0.0f;

    uint8_t part = 0;
    if (!readRegs(kRegPartId, &part, 1) || part != kPartIdExpected) {
        log_w("LTR553 ALS not found at 0x%02X (part=0x%02X)", ADDR_ALS_LTR553,
              part);
        return false;
    }

    if (!writeReg(kRegAlsMeasRate, kAlsMeasRate)) return false;
    if (!writeReg(kRegAlsContr, kAlsContrActive1x)) return false;

    gPresent = true;
    gLastPollMs = 0;
    log_i("LTR553 ALS ready");
    return true;
}

bool present() { return gPresent; }

bool poll() {
    if (!gPresent) return false;

    const uint32_t now = millis();
    if (gLastPollMs != 0 &&
        static_cast<int32_t>(now - gLastPollMs) <
            static_cast<int32_t>(kPollIntervalMs)) {
        return gHaveLux;
    }
    gLastPollMs = now;

    uint8_t raw[4];
    if (!readRegs(kRegAlsDataCh1, raw, sizeof(raw))) {
        gHaveLux = false;
        return false;
    }

    const uint16_t ch1 =
        static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8);
    const uint16_t ch0 =
        static_cast<uint16_t>(raw[2]) | (static_cast<uint16_t>(raw[3]) << 8);

    gLux = countsToLux(ch0, ch1);
    gHaveLux = true;
    return true;
}

bool lux(float *out) {
    if (!gHaveLux || !out) return false;
    *out = gLux;
    return true;
}

}  // namespace als
