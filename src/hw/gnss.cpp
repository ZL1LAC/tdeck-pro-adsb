#include "gnss.h"

#include <Arduino.h>
#include <TinyGPS++.h>

#include "board_pins.h"
#include "config.h"

namespace gnss {
namespace {

TinyGPSPlus gParser;
HardwareSerial &gSerial = Serial2;

bool gEnabled = false;
uint32_t gBaud = 0;
uint32_t gLastFixMs = 0;
bool gEverFixed = false;

// Candidates in the order they are worth trying on this module.
const uint32_t kBaudCandidates[] = {38400, 9600, 115200, 57600};
constexpr uint32_t kProbeWindowMs = 400;

// Listen briefly for anything that looks like NMEA.
bool probe(uint32_t baud) {
    gSerial.begin(baud, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    const uint32_t deadline = millis() + kProbeWindowMs;
    int sentenceStarts = 0;
    while (millis() < deadline) {
        while (gSerial.available()) {
            if (gSerial.read() == '$' && ++sentenceStarts >= 2) return true;
        }
        delay(1);
    }
    gSerial.end();
    return false;
}

}  // namespace

void begin() {
    if (!GNSS_ENABLED) return;

    // Power-cycle the module so a warm reboot starts from a known state.
    pinMode(BOARD_GPS_EN, OUTPUT);
    digitalWrite(BOARD_GPS_EN, LOW);
    delay(50);
    digitalWrite(BOARD_GPS_EN, HIGH);
    delay(200);

    for (uint32_t candidate : kBaudCandidates) {
        if (probe(candidate)) {
            gBaud = candidate;
            gEnabled = true;
            log_i("GNSS talking at %u baud", candidate);
            return;
        }
    }

    // Nothing recognisable. Leave the port open at the most likely rate so a
    // module that simply needed longer to boot still gets picked up.
    gSerial.begin(kBaudCandidates[0], SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    gBaud = 0;
    gEnabled = true;
    log_w("GNSS did not answer during probe; defaulting to 38400 baud");
}

void poll() {
    if (!gEnabled) return;
    while (gSerial.available()) {
        gParser.encode(static_cast<char>(gSerial.read()));
    }
    if (gParser.location.isValid() && gParser.location.isUpdated()) {
        gLastFixMs = millis();
        gEverFixed = true;
        if (gBaud == 0) gBaud = kBaudCandidates[0];
    }
}

bool hasFix() {
    return gEverFixed && gParser.location.isValid() &&
           gParser.location.age() < 10000;
}

double latitude() { return gParser.location.lat(); }
double longitude() { return gParser.location.lng(); }
uint32_t satellites() {
    return gParser.satellites.isValid() ? gParser.satellites.value() : 0;
}

bool utcNow(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour,
            uint8_t *minute, uint8_t *second) {
    if (!gEnabled) return false;
    if (!gParser.date.isValid() || !gParser.time.isValid()) return false;
    // Same freshness idea as hasFix(), but tighter: a stale time sentence is
    // worse than none, since we would seed the system clock from it.
    if (gParser.date.age() > 2000 || gParser.time.age() > 2000) return false;
    // A module with no almanac yet reports 2000-01-01 with the date flagged
    // valid; refuse anything before the firmware could plausibly exist.
    if (gParser.date.year() < 2024) return false;

    *year = static_cast<uint16_t>(gParser.date.year());
    *month = static_cast<uint8_t>(gParser.date.month());
    *day = static_cast<uint8_t>(gParser.date.day());
    *hour = static_cast<uint8_t>(gParser.time.hour());
    *minute = static_cast<uint8_t>(gParser.time.minute());
    *second = static_cast<uint8_t>(gParser.time.second());
    return true;
}

uint32_t fixAgeMs() {
    if (!gEverFixed) return UINT32_MAX;
    return millis() - gLastFixMs;
}

uint32_t baud() { return gBaud; }

}  // namespace gnss
