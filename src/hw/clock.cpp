#include "clock.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "hw/gnss.h"
#include "net/net.h"

namespace wallclock {
namespace {

// The ESP32 boots at epoch 0, so any timestamp comfortably in the past proves
// somebody has set the clock. 2024-01-01T00:00:00Z.
constexpr time_t kEpochSanity = 1704067200;

bool gNtpStarted = false;
bool gSeeded = false;

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). Used
// instead of timegm() so we never depend on which newlib extensions the
// toolchain happens to expose.
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);          // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// GNSS gives UTC, so this is a straight civil-to-epoch conversion with no
// timezone involved.
time_t utcToEpoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                  uint8_t minute, uint8_t second) {
    const int32_t days = daysFromCivil(year, month, day);
    return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

bool nowLocal(struct tm *out) {
    const time_t now = time(nullptr);
    if (now < kEpochSanity) return false;
    localtime_r(&now, out);
    return true;
}

}  // namespace

void begin() {
    setenv("TZ", CLOCK_TZ, 1);
    tzset();
}

void poll() {
    // SNTP runs asynchronously in the background once started, and keeps
    // disciplining the clock afterwards -- so start it even if GNSS got there
    // first, and only ever start it once.
    if (!gNtpStarted && net::connected()) {
        configTzTime(CLOCK_TZ, CLOCK_NTP_SERVER_1, CLOCK_NTP_SERVER_2);
        gNtpStarted = true;
    }

    const bool set = time(nullptr) >= kEpochSanity;
    if (set && !gSeeded) {
        // The GNSS path below latches gSeeded itself, so reaching here means
        // either SNTP has landed or the RTC survived a warm reset with the
        // time still in it.
        gSeeded = true;
        log_i("clock set from %s", gNtpStarted ? "NTP" : "retained RTC");
        return;
    }
    if (set) return;

    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!gnss::utcNow(&year, &month, &day, &hour, &minute, &second)) return;

    struct timeval tv = {};
    tv.tv_sec = utcToEpoch(year, month, day, hour, minute, second);
    settimeofday(&tv, nullptr);
    gSeeded = true;
    log_i("clock set from GNSS (%04u-%02u-%02u %02u:%02u:%02uZ)",
          static_cast<unsigned>(year), static_cast<unsigned>(month),
          static_cast<unsigned>(day), static_cast<unsigned>(hour),
          static_cast<unsigned>(minute), static_cast<unsigned>(second));
}

bool valid() { return time(nullptr) >= kEpochSanity; }

// Asked of SNTP rather than remembered, so that a clock first seeded from the
// GNSS reports honestly once SNTP has taken over disciplining it.
bool fromNtp() {
    return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

bool localHm(uint8_t *hour, uint8_t *minute) {
    struct tm t = {};
    if (!nowLocal(&t)) return false;
    *hour = static_cast<uint8_t>(t.tm_hour);
    *minute = static_cast<uint8_t>(t.tm_min);
    return true;
}

bool localHms(uint8_t *hour, uint8_t *minute, uint8_t *second) {
    struct tm t = {};
    if (!nowLocal(&t)) return false;
    *hour = static_cast<uint8_t>(t.tm_hour);
    *minute = static_cast<uint8_t>(t.tm_min);
    *second = static_cast<uint8_t>(t.tm_sec);
    return true;
}

}  // namespace wallclock
