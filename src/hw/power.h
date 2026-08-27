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
//
// Counted rather than set, because there are now two things that want it and
// they overlap: the fetch runs on its own task and a repaint runs on the loop
// task, and whichever finishes first must not drop the clock out from under
// the other. The frequency rises on the first claim and falls on the last
// release. Boot counts as a claim -- setup() runs at 240 MHz -- so main()
// releases one at the end of it.
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
