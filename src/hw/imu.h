#pragma once

#include <stdint.h>

namespace imu {

// BHI260AP on the board's 1.8 V rail. power::begin() raises BOARD_1V8_EN
// before this is called; without that rail the chip is silent on the bus.
bool begin();
bool present();

// Drain the sensor FIFO and refresh the cached accel sample. Cheap when the
// FIFO is empty; call from the slow UI path.
void poll();

// Most recent acceleration in m/s^2, or false until the first sample lands.
bool acceleration(float *ax, float *ay, float *az);

// Coarse attitude from the accel vector, for the diagnostics page.
// "face up" / "face down" / "on side" / "--" while unknown.
const char *orientation();

}  // namespace imu
