#include "aircraftdb.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config.h"

namespace aircraftdb {
namespace {

const char kMagic[8] = {'T', 'D', 'E', 'C', 'K', 'R', 'E', 'G'};
constexpr uint16_t kVersion = 1;
constexpr uint8_t kHeaderLen = 32;

// Mirrors tools/build_db.py. Kept as constants rather than read from the
// header so a mismatched build fails the check below instead of quietly
// reading at the wrong stride.
constexpr uint8_t kRecordLen = 16;
constexpr uint8_t kRegLen = 8;
constexpr uint8_t kTypeLen = 4;

uint8_t *gBuf = nullptr;   // the record array only; the header is not kept
uint32_t gCount = 0;
char gStatus[32] = "not started";

// Records are sorted by this, and it is what the search compares.
inline uint32_t icaoAt(uint32_t index) {
    const uint8_t *p = gBuf + static_cast<size_t>(index) * kRecordLen;
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

// Parses exactly six hex digits. Returns false for anything else -- notably
// the '~' prefix readsb puts on non-ICAO addresses (TIS-B and ADS-R targets),
// which are not in any registry and must not be looked up.
bool parseIcao(const char *hex, uint32_t *out) {
    if (!hex) return false;
    uint32_t v = 0;
    int i = 0;
    for (; i < 6; ++i) {
        const char c = hex[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (hex[i] != '\0') return false;
    *out = v;
    return true;
}

// Fixed-width and NUL-padded rather than NUL-terminated, so a field that fills
// its column has no terminator of its own to copy.
void copyPadded(char *dst, size_t dstLen, const uint8_t *src, size_t srcLen) {
    dst[0] = '\0';
    if (dstLen == 0) return;
    size_t n = 0;
    while (n < srcLen && src[n] != '\0') ++n;
    if (n > dstLen - 1) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

}  // namespace

bool begin() {
    // basemap::begin() usually got here first; mounting twice is harmless and
    // this must not depend on the order the two are brought up in.
    if (!SPIFFS.begin(false)) {
        snprintf(gStatus, sizeof(gStatus), "no SPIFFS");
        return false;
    }

    File f = SPIFFS.open(AIRCRAFT_DB_FILE, "r");
    if (!f) {
        snprintf(gStatus, sizeof(gStatus), "no %s", AIRCRAFT_DB_FILE);
        return false;
    }

    uint8_t header[kHeaderLen];
    if (f.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
        snprintf(gStatus, sizeof(gStatus), "header truncated");
        f.close();
        return false;
    }

    uint16_t version = 0, recordLen = 0;
    uint32_t count = 0;
    memcpy(&version, header + 8, sizeof(version));
    memcpy(&recordLen, header + 10, sizeof(recordLen));
    memcpy(&count, header + 12, sizeof(count));
    const uint8_t regLen = header[16];
    const uint8_t typeLen = header[17];

    if (memcmp(header, kMagic, sizeof(kMagic)) != 0 || version != kVersion ||
        recordLen != kRecordLen || regLen != kRegLen || typeLen != kTypeLen) {
        snprintf(gStatus, sizeof(gStatus), "format mismatch");
        log_w("aircraftdb: %s is not a v%u table this build can read",
              AIRCRAFT_DB_FILE, static_cast<unsigned>(kVersion));
        f.close();
        return false;
    }

    const size_t bytes = static_cast<size_t>(count) * kRecordLen;
    if (count == 0 || bytes + kHeaderLen > static_cast<size_t>(f.size())) {
        snprintf(gStatus, sizeof(gStatus), "truncated");
        f.close();
        return false;
    }

    // PSRAM: 8 MB of it, and the whole point of loading the table up front is
    // that a lookup then costs a binary search rather than a file seek. It
    // never grows or moves, so this allocation lives for the run.
    gBuf = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!gBuf) gBuf = static_cast<uint8_t *>(malloc(bytes));
    if (!gBuf) {
        snprintf(gStatus, sizeof(gStatus), "no room (%uk)",
                 static_cast<unsigned>(bytes / 1024));
        f.close();
        return false;
    }

    if (f.read(gBuf, bytes) != static_cast<int>(bytes)) {
        snprintf(gStatus, sizeof(gStatus), "read failed");
        free(gBuf);
        gBuf = nullptr;
        f.close();
        return false;
    }
    f.close();

    gCount = count;
    snprintf(gStatus, sizeof(gStatus), "%u rec %uk",
             static_cast<unsigned>(gCount), static_cast<unsigned>(bytes / 1024));
    log_i("aircraftdb: %u records, %u KB in PSRAM",
          static_cast<unsigned>(gCount), static_cast<unsigned>(bytes / 1024));
    return true;
}

bool available() { return gBuf != nullptr && gCount > 0; }

bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen) {
    if (reg && regLen) reg[0] = '\0';
    if (type && typeLen) type[0] = '\0';
    if (!available()) return false;

    uint32_t want = 0;
    if (!parseIcao(hex, &want)) return false;

    uint32_t lo = 0, hi = gCount;  // half-open
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uint32_t key = icaoAt(mid);
        if (key == want) {
            const uint8_t *p = gBuf + static_cast<size_t>(mid) * kRecordLen;
            if (reg) copyPadded(reg, regLen, p + 3, kRegLen);
            if (type) copyPadded(type, typeLen, p + 3 + kRegLen, kTypeLen);
            return true;
        }
        if (key < want) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

const char *status() { return gStatus; }

}  // namespace aircraftdb
