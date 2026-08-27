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

// Candidates in the order they are worth trying on this module.
const uint32_t kBaudCandidates[] = {38400, 9600, 115200, 57600};
constexpr uint32_t kProbeWindowMs = 400;

// The UART ring holds 256 bytes unless told otherwise, which at 38400 baud is
// 67 ms of NMEA -- a tenth of one panel refresh, and a twentieth of one
// aggregator poll, both of which block the loop that drains it. Dropping
// sentences does not cost a fix, since the module holds that itself, but it
// does delay us noticing one, and noticing is what the duty cycle is timing
// against. Two kilobytes buys half a second of slack for one page of DRAM.
constexpr size_t kRxBufferBytes = 2048;

// setRxBufferSize() is only honoured while the driver is uninstalled, so it
// has to be re-applied ahead of every begin() rather than set once.
void openSerial(uint32_t baud) {
    gSerial.setRxBufferSize(kRxBufferBytes);
    gSerial.begin(baud, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
}

// -- power policy -----------------------------------------------------------
//
// The plot centre is the only thing that needs a position, and it is a
// handheld sitting on a desk most of the time -- a fix from two minutes ago is
// as good as a fix from now. So the module runs just long enough to produce
// one and is then switched off at the rail.
//
// kFixHoldMs has to comfortably exceed one full sleep-plus-acquire cycle, or a
// slow acquisition would let the held fix expire and bounce the plot back to
// the configured home before the new one lands.
constexpr uint32_t kSleepMs = GNSS_SLEEP_MS;
constexpr uint32_t kMaxAcquireMs = GNSS_ACQUIRE_MAX_MS;
constexpr uint32_t kRetryMs = GNSS_RETRY_MS;
constexpr uint32_t kFixHoldMs = GNSS_FIX_HOLD_MS;

enum class State : uint8_t { Off, Acquiring, Sleeping };

State gState = State::Off;
bool gNeeded = true;
uint32_t gAcquireStartMs = 0;
uint32_t gWakeAtMs = 0;
uint32_t gPoweredSinceMs = 0;
uint32_t gPoweredTotalMs = 0;
uint32_t gLastTtffMs = 0;

// The last known fix, cached so it survives the module being powered down.
bool gEverFixed = false;
double gLat = 0.0;
double gLon = 0.0;
uint32_t gLastFixMs = 0;
uint32_t gSatellites = 0;

// Listen briefly for anything that looks like NMEA.
bool probe(uint32_t baud) {
    openSerial(baud);
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

void powerOn() {
    digitalWrite(BOARD_GPS_EN, HIGH);
    // The baud rate was settled during begin(), so there is nothing to probe:
    // just reopen the port. The module needs a moment before it says anything,
    // which costs nothing here because poll() is called continuously.
    openSerial(gBaud ? gBaud : kBaudCandidates[0]);
    gState = State::Acquiring;
    gAcquireStartMs = millis();
    gPoweredSinceMs = gAcquireStartMs;
}

void powerOff(State next, uint32_t wakeAtMs) {
    if (gState == State::Acquiring) {
        gPoweredTotalMs += millis() - gPoweredSinceMs;
        gSerial.end();
        digitalWrite(BOARD_GPS_EN, LOW);
    }
    gState = next;
    gWakeAtMs = wakeAtMs;
}

// Drain the UART into the parser and latch anything usable that comes out.
bool drain() {
    while (gSerial.available()) {
        gParser.encode(static_cast<char>(gSerial.read()));
    }
    if (!gParser.location.isValid() || !gParser.location.isUpdated()) {
        return false;
    }
    gLat = gParser.location.lat();
    gLon = gParser.location.lng();
    gLastFixMs = millis();
    gEverFixed = true;
    if (gParser.satellites.isValid()) gSatellites = gParser.satellites.value();
    if (gBaud == 0) gBaud = kBaudCandidates[0];
    return true;
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

    gEnabled = true;
    gState = State::Acquiring;
    gAcquireStartMs = millis();
    gPoweredSinceMs = gAcquireStartMs;

    for (uint32_t candidate : kBaudCandidates) {
        if (probe(candidate)) {
            gBaud = candidate;
            log_i("GNSS talking at %u baud", candidate);
            return;
        }
    }

    // Nothing recognisable. Leave the port open at the most likely rate so a
    // module that simply needed longer to boot still gets picked up.
    openSerial(kBaudCandidates[0]);
    gBaud = 0;
    log_w("GNSS did not answer during probe; defaulting to 38400 baud");
}

void setNeeded(bool needed) { gNeeded = needed; }

void poll() {
    if (!gEnabled) return;
    const uint32_t now = millis();

    // Nothing is reading the position: the cheapest fix is the one we do not
    // go looking for.
    if (!gNeeded) {
        if (gState != State::Off) {
            powerOff(State::Off, 0);
            log_i("GNSS: powered down, nothing needs a position");
        }
        return;
    }

    switch (gState) {
        case State::Off:
            powerOn();
            break;

        case State::Sleeping:
            if (static_cast<int32_t>(now - gWakeAtMs) >= 0) powerOn();
            break;

        case State::Acquiring:
            if (drain()) {
                gLastTtffMs = now - gAcquireStartMs;
                powerOff(State::Sleeping, now + kSleepMs);
                log_i("GNSS: fix in %u ms, sleeping %u s",
                      static_cast<unsigned>(gLastTtffMs),
                      static_cast<unsigned>(kSleepMs / 1000));
            } else if (now - gAcquireStartMs > kMaxAcquireMs) {
                powerOff(State::Sleeping, now + kRetryMs);
                log_w("GNSS: no fix in %u s, backing off %u s",
                      static_cast<unsigned>(kMaxAcquireMs / 1000),
                      static_cast<unsigned>(kRetryMs / 1000));
            }
            break;
    }
}

// Deliberately independent of the module being powered -- see the header.
bool hasFix() {
    return gEverFixed && (millis() - gLastFixMs) < kFixHoldMs;
}

double latitude() { return gLat; }
double longitude() { return gLon; }
uint32_t satellites() { return gSatellites; }

bool utcNow(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour,
            uint8_t *minute, uint8_t *second) {
    if (!gEnabled) return false;
    if (!gParser.date.isValid() || !gParser.time.isValid()) return false;
    // Same freshness idea as hasFix(), but tighter and not held across a power
    // down: a stale time sentence is worse than none, since we would seed the
    // system clock from it.
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

bool powered() { return gState == State::Acquiring; }

uint32_t lastTtffMs() { return gLastTtffMs; }

uint8_t dutyPercent() {
    const uint32_t now = millis();
    if (now == 0) return 0;
    uint32_t on = gPoweredTotalMs;
    if (gState == State::Acquiring) on += now - gPoweredSinceMs;
    const uint32_t pct = static_cast<uint32_t>((on * 100ULL) / now);
    return static_cast<uint8_t>(pct > 100 ? 100 : pct);
}

uint32_t sleepRemainingMs() {
    if (gState != State::Sleeping) return 0;
    const int32_t left = static_cast<int32_t>(gWakeAtMs - millis());
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

}  // namespace gnss
