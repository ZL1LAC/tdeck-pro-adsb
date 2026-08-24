#pragma once

#include <stdint.h>

namespace gnss {

// Powers up the MIA-M10Q and probes for its UART baud rate (the module ships
// at 38400 but some units are found at 9600). No-op when GNSS_ENABLED is false.
void begin();

// Feed the NMEA parser and run the power policy. Call often; it never blocks.
void poll();

// Whether anything is actually using the position.
//
// The module is the largest continuous draw on the board and the position it
// reports barely changes, so it is not held powered for its own sake. When
// nothing needs a position -- the plot is centred on the configured home and
// the clock is already set -- the rail is simply switched off. When something
// does, the module is cycled: acquire a fix, power down, wake again later.
void setNeeded(bool needed);

// True while a *usable* position is known, which deliberately outlives the
// module being powered: a fix is held for kFixHoldMs across the sleep between
// acquisitions. Without that the plot centre would fall back to the configured
// home every time the rail went down and jump back on the next fix.
bool hasFix();

double latitude();
double longitude();
uint32_t satellites();

// UTC from the last NMEA sentence. False unless both the date and the time are
// valid and recent, so this stops answering once the module powers down --
// which is correct, since it is only ever used to seed the system clock.
bool utcNow(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour,
            uint8_t *minute, uint8_t *second);

// Milliseconds since the last valid position sentence, or UINT32_MAX if we
// have never had a fix.
uint32_t fixAgeMs();

// Detected UART speed, or 0 while still searching.
uint32_t baud();

// -- power policy, for the diagnostics page ---------------------------------

// Whether the rail is up right now.
bool powered();

// Time to first fix of the most recent acquisition, or 0 if none has completed.
// Worth watching: it decides whether duty cycling is winning. A module that
// keeps its ephemeris across the sleep re-fixes in a second or two, one that
// cold starts every time takes half a minute and saves far less.
uint32_t lastTtffMs();

// Share of wall-clock time the rail has been up, as a percentage.
uint8_t dutyPercent();

// Milliseconds until the next acquisition, or 0 when not sleeping.
uint32_t sleepRemainingMs();

}  // namespace gnss
