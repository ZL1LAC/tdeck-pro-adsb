#pragma once

#include <stdint.h>

namespace als {

// LTR-553ALS ambient light sensor on the shared I2C bus. Lux drives auto
// keypad backlight and is shown on diagnostics. Proximity is left powered
// down -- nothing wants it, and the IR LED is a small but continuous draw.
bool begin();
bool present();

// Non-blocking. Refreshes the cached lux at a few hertz; safe to call from
// the slow UI path. Returns false if the sensor never answered.
bool poll();

// Most recent lux reading, or false if we have never had a valid sample.
bool lux(float *out);

}  // namespace als
