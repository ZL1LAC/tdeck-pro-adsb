#include "imu.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include <SensorBHI260AP.hpp>
#include <bosch/BoschSensorDataHelper.hpp>
#include <BoschFirmware.h>

#include "board_pins.h"

namespace imu {
namespace {

#ifndef BOSCH_APP30_SHUTTLE_BHI260_FW
#error "Build with -DBOSCH_APP30_SHUTTLE_BHI260_FW so BoschFirmware.h picks an image."
#endif

SensorBHI260AP gBhy;
SensorXYZ gAccel(SensorBHI260AP::ACCEL_PASSTHROUGH, gBhy);

bool gPresent = false;
bool gHaveSample = false;
float gAx = 0.0f;
float gAy = 0.0f;
float gAz = 0.0f;
const char *gOrientation = "--";

void updateOrientation() {
    const float ax = gAx;
    const float ay = gAy;
    const float az = gAz;
    const float aax = fabsf(ax);
    const float aay = fabsf(ay);
    const float aaz = fabsf(az);

    // Ignore soft/noisy samples: face-down idle keys off this string, so a
    // half-g wobble must not flip the label. ~0.7 g keeps us near resting.
    constexpr float kMinG = 7.0f;
    if (aax < kMinG && aay < kMinG && aaz < kMinG) return;

    // Gravity dominates a resting reading. Pick the largest axis and map it
    // to a pocket-friendly label. Z sign is still provisional -- confirm on
    // the diagnostics page before relying on face-down idle.
    if (aaz >= aax && aaz >= aay) {
        gOrientation = (az >= 0.0f) ? "face up" : "face down";
    } else {
        gOrientation = "on side";
    }
}

}  // namespace

bool begin() {
    gPresent = false;
    gHaveSample = false;
    gOrientation = "--";

    // Give the 1.8 V rail a moment after power::begin() raised it. The hub
    // boots from ROM and is not instantly ready on the bus.
    delay(20);

    gBhy.setPins(BOARD_IMU_RST);
    // RAM firmware (bosch_firmware_type == 0): rewritten every boot.
    gBhy.setFirmware(bosch_firmware_image, bosch_firmware_size, false, false);
    gBhy.setBootFromFlash(false);

    if (!gBhy.begin(Wire, BHI260AP_SLAVE_ADDRESS_L, BOARD_I2C_SDA, BOARD_I2C_SCL)) {
        log_w("BHI260AP init failed: %s", gBhy.getError());
        return false;
    }

    // A few hertz is plenty for the diagnostics page; the e-paper cannot show
    // more, and a 100 Hz FIFO would just burn I2C bandwidth.
    constexpr float kRateHz = 5.0f;
    constexpr uint32_t kLatencyMs = 0;
    if (!gAccel.enable(kRateHz, kLatencyMs)) {
        log_w("BHI260AP accel enable failed");
        return false;
    }

    gPresent = true;
    log_i("BHI260AP IMU ready");
    return true;
}

bool present() { return gPresent; }

void poll() {
    if (!gPresent) return;

    gBhy.update();

    if (!gAccel.hasUpdated()) return;

    gAx = gAccel.getX();
    gAy = gAccel.getY();
    gAz = gAccel.getZ();
    gHaveSample = true;
    updateOrientation();
}

bool acceleration(float *ax, float *ay, float *az) {
    if (!gHaveSample) return false;
    if (ax) *ax = gAx;
    if (ay) *ay = gAy;
    if (az) *az = gAz;
    return true;
}

const char *orientation() { return gOrientation; }

}  // namespace imu
