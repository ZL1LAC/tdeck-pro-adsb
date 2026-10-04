#pragma once

#include <stdint.h>

namespace power {

// Brings up the shared I2C bus, parks the radios we do not use, and quiets the
// charger's I2C watchdog, and configures the gauge for the 1400 mAh pack.
// Call once, before any other hw:: module.
void begin();

// Battery state. percent is what the status bar should show. The BQ27220
// defaults to a 3000 mAh pack; begin() sets design and initial full-charge
// capacity to 1400 mAh in RAM, reapplying after gauge power loss. Once set,
// learned full-charge capacity is preserved on subsequent MCU boots.
// If configuration failed or SOC is implausible, use a voltage estimate.
// Returns false if the gauge did not answer.
bool readBattery(uint8_t *percent, uint16_t *milliVolts);

// What the gauge itself last reported. 255 / 0 until the first successful read.
// gaugePercent() is 255 when that read did not include a plausible SOC.
uint8_t gaugePercent();
uint16_t designMilliAmpHours();

void setKeypadBacklight(bool on);
bool keypadBacklight();

// Apply a BacklightMode. Auto reads als::lux() with the hysteresis in
// config.h; forceOff wins (face-down idle) without changing the saved mode.
void refreshKeypadBacklight(uint8_t mode, bool forceOff = false);

// CPU clock policy.
//
// The clock stays at the 240 MHz boot frequency. It used to drop to 80 MHz
// whenever nothing was fetching or repainting, which is almost every UI tick
// once the device is just sitting there. That PLL switch, with Wi-Fi up,
// wedges the Wi-Fi task after a while; loop() then blocks inside
// WiFi.status() and the panel never moves again. The claim counter is kept
// so a later build can gate something cheaper than the CPU clock on it.
void acquireBoost();
void releaseBoost();
bool boosted();

// Scoped form, for a claim that starts and ends in one place. A claim handed
// between loop passes -- the fetch -- has to use the calls above instead.
class Boost {
   public:
    Boost() { acquireBoost(); }
    ~Boost() { releaseBoost(); }
    Boost(const Boost &) = delete;
    Boost &operator=(const Boost &) = delete;
};

}  // namespace power
