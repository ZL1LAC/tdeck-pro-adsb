#pragma once

#include <stdint.h>

namespace power {

// Brings up the shared I2C bus, parks the radios we do not use, and quiets the
// charger's I2C watchdog. Call once, before any other hw:: module.
void begin();

// Battery state from the BQ27220 fuel gauge. Returns false if the gauge did
// not answer -- callers should then hide the battery indicator rather than
// show a wrong number.
bool readBattery(uint8_t *percent, uint16_t *milliVolts);

void setKeypadBacklight(bool on);
bool keypadBacklight();

// CPU clock policy.
//
// The prebuilt Arduino libraries are compiled without CONFIG_PM_ENABLE, so
// there is no dynamic frequency scaling and no tickless idle to lean on: the
// core runs flat out at 240 MHz whatever the firmware is doing. Almost all of
// that is spent waiting -- on a 15 s poll timer, on a panel that needs 0.7 s
// to repaint, on a finger that is not there. Two things genuinely are
// CPU-bound (the TLS fetch and drawing a frame), so those get boosted and
// everything else runs at 80 MHz.
//
// 80 MHz is the floor that keeps the PLL-derived APB clock at 80 MHz, so the
// UART, I2C and SPI peripherals need no re-tuning across the change.
void setBoost(bool on);
bool boosted();

// Scoped form: boosts on construction, drops back on the way out.
class Boost {
   public:
    Boost() : was_(boosted()) { setBoost(true); }
    ~Boost() { setBoost(was_); }
    Boost(const Boost &) = delete;
    Boost &operator=(const Boost &) = delete;

   private:
    bool was_;
};

}  // namespace power
