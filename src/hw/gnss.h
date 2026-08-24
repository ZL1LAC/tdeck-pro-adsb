#pragma once

#include <stdint.h>

namespace gnss {

// Powers up the MIA-M10Q and probes for its UART baud rate (the module ships
// at 38400 but some units are found at 9600). No-op when GNSS_ENABLED is false.
void begin();

// Feed the NMEA parser. Call often; it never blocks.
void poll();

bool hasFix();
double latitude();
double longitude();
uint32_t satellites();

// UTC from the last NMEA sentence. False unless both the date and the time are
// valid and recent. TinyGPSPlus parses these already, so reading them is free.
bool utcNow(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour,
            uint8_t *minute, uint8_t *second);

// Milliseconds since the last valid position sentence, or UINT32_MAX if we
// have never had a fix.
uint32_t fixAgeMs();

// Detected UART speed, or 0 while still searching.
uint32_t baud();

}  // namespace gnss
