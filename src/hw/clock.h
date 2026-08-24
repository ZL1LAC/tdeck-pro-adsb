#pragma once

#include <stdint.h>

// Wall-clock time. Named "wallclock" rather than "clock" so nothing collides
// with libc's clock().
//
// The ESP32 system clock is the single store; this module only seeds it. Two
// sources feed it, whichever turns up first: SNTP over the Wi-Fi the feed needs
// anyway, and the GNSS date/time TinyGPSPlus is already parsing for free. That
// covers both the indoors-no-fix and the off-grid cases.
namespace wallclock {

// Applies CLOCK_TZ. Call before anything reads local time.
void begin();

// Starts SNTP once the link is up and seeds from GNSS while the clock is
// unset. Call from the main loop; never blocks.
void poll();

// True once the system clock has been set by either source.
bool valid();

// Which source set it. Meaningless while !valid().
bool fromNtp();

// Local time, honouring CLOCK_TZ including DST. False if the clock is unset.
bool localHm(uint8_t *hour, uint8_t *minute);
bool localHms(uint8_t *hour, uint8_t *minute, uint8_t *second);

}  // namespace wallclock
