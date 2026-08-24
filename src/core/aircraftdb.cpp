#include "aircraftdb.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config.h"

namespace aircraftdb {
namespace {

const char kMagic[8] = {'T', 'D', 'E', 'C', 'K', 'R', 'E', 'G'};
constexpr uint16_t kVersion = 2;
constexpr uint8_t kHeaderLen = 32;

// Mirrors tools/build_db.py. Kept as constants rather than taken from the
// header so a mismatched build fails the check in begin() instead of quietly
// reading at the wrong stride.
constexpr uint8_t kRecordLen = 24;
constexpr uint8_t kRegLen = 8;
constexpr uint8_t kTypeLen = 4;
constexpr uint16_t kNoString = 0xFFFF;

// The whole file, header included, so the offsets in the header can be used as
// written rather than rebased.
uint8_t *gBuf = nullptr;
uint32_t gCount = 0;
uint32_t gStrCount = 0;
uint32_t gStrtabOff = 0;
uint32_t gPoolOff = 0;
uint32_t gSize = 0;
char gStatus[32] = "not started";

inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline const uint8_t *recordAt(uint32_t index) {
    return gBuf + kHeaderLen + static_cast<size_t>(index) * kRecordLen;
}

// Records are sorted by this, and it is what the search compares.
inline uint32_t icaoAt(uint32_t index) {
    const uint8_t *p = recordAt(index);
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

// Never returns null: an absent or out-of-range string reads as empty, so
// callers can print it without checking.
const char *stringAt(uint16_t index) {
    if (index == kNoString || index >= gStrCount) return "";
    const uint32_t off = rd32(gBuf + gStrtabOff + static_cast<size_t>(index) * 4);
    if (gPoolOff + off >= gSize) return "";  // truncated file
    return reinterpret_cast<const char *>(gBuf + gPoolOff + off);
}

// Parses exactly six hex digits. Returns false for anything else -- notably
// the '~' prefix readsb puts on non-ICAO addresses (TIS-B and ADS-R targets),
// which are in no registry and must not be looked up.
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
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    size_t n = 0;
    while (n < srcLen && src[n] != '\0') ++n;
    if (n > dstLen - 1) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// -1 on a miss.
int32_t find(const char *hex) {
    if (!available()) return -1;
    uint32_t want = 0;
    if (!parseIcao(hex, &want)) return -1;

    uint32_t lo = 0, hi = gCount;  // half-open
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uint32_t key = icaoAt(mid);
        if (key == want) return static_cast<int32_t>(mid);
        if (key < want) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

void clear(Details *out) {
    if (!out) return;
    out->reg[0] = '\0';
    out->type[0] = '\0';
    out->desc = "";
    out->op = "";
    out->year = 0;
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

    const uint16_t version = rd16(header + 8);
    const uint16_t recordLen = rd16(header + 10);
    const uint32_t count = rd32(header + 12);
    const uint8_t regLen = header[16];
    const uint8_t typeLen = header[17];
    const uint32_t strCount = rd32(header + 20);
    const uint32_t strtabOff = rd32(header + 24);
    const uint32_t poolOff = rd32(header + 28);

    if (memcmp(header, kMagic, sizeof(kMagic)) != 0 || version != kVersion ||
        recordLen != kRecordLen || regLen != kRegLen || typeLen != kTypeLen) {
        snprintf(gStatus, sizeof(gStatus), "format mismatch");
        log_w("aircraftdb: %s is not a v%u table this build can read",
              AIRCRAFT_DB_FILE, static_cast<unsigned>(kVersion));
        f.close();
        return false;
    }

    const size_t size = static_cast<size_t>(f.size());
    const size_t need = kHeaderLen + static_cast<size_t>(count) * kRecordLen;
    if (count == 0 || need > size || strtabOff > size || poolOff > size ||
        strtabOff + static_cast<size_t>(strCount) * 4 > size) {
        snprintf(gStatus, sizeof(gStatus), "truncated");
        f.close();
        return false;
    }

    // PSRAM: 8 MB of it, and the point of loading up front is that a lookup
    // then costs a binary search rather than a file seek. It never grows or
    // moves, so this allocation lives for the run -- which is also what lets
    // details() hand out pointers into the string pool instead of copying.
    gBuf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!gBuf) gBuf = static_cast<uint8_t *>(malloc(size));
    if (!gBuf) {
        snprintf(gStatus, sizeof(gStatus), "no room (%uk)",
                 static_cast<unsigned>(size / 1024));
        f.close();
        return false;
    }

    f.seek(0);
    if (f.read(gBuf, size) != static_cast<int>(size)) {
        snprintf(gStatus, sizeof(gStatus), "read failed");
        free(gBuf);
        gBuf = nullptr;
        f.close();
        return false;
    }
    f.close();

    gCount = count;
    gStrCount = strCount;
    gStrtabOff = strtabOff;
    gPoolOff = poolOff;
    gSize = static_cast<uint32_t>(size);

    snprintf(gStatus, sizeof(gStatus), "%u rec %uk",
             static_cast<unsigned>(gCount), static_cast<unsigned>(size / 1024));
    log_i("aircraftdb: %u records, %u strings, %u KB in PSRAM",
          static_cast<unsigned>(gCount), static_cast<unsigned>(gStrCount),
          static_cast<unsigned>(size / 1024));
    return true;
}

bool available() { return gBuf != nullptr && gCount > 0; }

bool details(const char *hex, Details *out) {
    clear(out);
    const int32_t idx = find(hex);
    if (idx < 0 || !out) return false;

    const uint8_t *p = recordAt(static_cast<uint32_t>(idx));
    copyPadded(out->reg, sizeof(out->reg), p + 3, kRegLen);
    copyPadded(out->type, sizeof(out->type), p + 3 + kRegLen, kTypeLen);
    out->desc = stringAt(rd16(p + 3 + kRegLen + kTypeLen));
    out->op = stringAt(rd16(p + 3 + kRegLen + kTypeLen + 2));
    out->year = rd16(p + 3 + kRegLen + kTypeLen + 4);
    return true;
}

bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen) {
    if (reg && regLen) reg[0] = '\0';
    if (type && typeLen) type[0] = '\0';

    const int32_t idx = find(hex);
    if (idx < 0) return false;

    const uint8_t *p = recordAt(static_cast<uint32_t>(idx));
    copyPadded(reg, regLen, p + 3, kRegLen);
    copyPadded(type, typeLen, p + 3 + kRegLen, kTypeLen);
    return true;
}

const char *status() { return gStatus; }

}  // namespace aircraftdb
