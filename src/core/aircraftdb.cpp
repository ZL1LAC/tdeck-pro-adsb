#include "aircraftdb.h"

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config.h"
#include "core/assets.h"
#include "hw/spibus.h"

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

// Regional tables fit comfortably; a worldwide dump does not, and allocating
// 30 MB just to watch malloc fail would fragment PSRAM before the JSON arena
// had a chance.
constexpr size_t kMaxRamLoad = 3 * 1024 * 1024;

constexpr int kCacheSlots = 48;

uint8_t *gBuf = nullptr;
fs::File gFile;
bool gDisk = false;
uint32_t gCount = 0;
uint32_t gStrCount = 0;
uint32_t gStrtabOff = 0;
uint32_t gPoolOff = 0;
uint32_t gSize = 0;
char gStatus[32] = "not started";

struct CacheSlot {
    int32_t index = -1;
    uint8_t rec[kRecordLen];
};
CacheSlot gCache[kCacheSlots];
uint8_t gCacheClock = 0;

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

inline const uint8_t *recordAtRam(uint32_t index) {
    return gBuf + kHeaderLen + static_cast<size_t>(index) * kRecordLen;
}

void cacheReset() {
    for (int i = 0; i < kCacheSlots; ++i) gCache[i].index = -1;
    gCacheClock = 0;
}

bool fillRecord(uint32_t index, uint8_t *dst) {
    if (index >= gCount || !dst) return false;
    if (gBuf) {
        memcpy(dst, recordAtRam(index), kRecordLen);
        return true;
    }
    for (int i = 0; i < kCacheSlots; ++i) {
        if (gCache[i].index == static_cast<int32_t>(index)) {
            memcpy(dst, gCache[i].rec, kRecordLen);
            return true;
        }
    }
    spibus::Lock lock;
    if (!gFile) return false;
    const uint32_t off = kHeaderLen + index * kRecordLen;
    if (!gFile.seek(off)) return false;
    if (gFile.read(dst, kRecordLen) != static_cast<int>(kRecordLen)) return false;
    gCache[gCacheClock].index = static_cast<int32_t>(index);
    memcpy(gCache[gCacheClock].rec, dst, kRecordLen);
    gCacheClock = static_cast<uint8_t>((gCacheClock + 1) % kCacheSlots);
    return true;
}

inline uint32_t icaoAt(uint32_t index) {
    uint8_t rec[kRecordLen];
    if (!fillRecord(index, rec)) return 0xFFFFFFFFu;
    return (static_cast<uint32_t>(rec[0]) << 16) |
           (static_cast<uint32_t>(rec[1]) << 8) | rec[2];
}

void loadString(uint16_t index, char *dst, size_t dstLen) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    if (index == kNoString || index >= gStrCount) return;

    if (gBuf) {
        const uint32_t off =
            rd32(gBuf + gStrtabOff + static_cast<size_t>(index) * 4);
        if (gPoolOff + off >= gSize) return;
        strncpy(dst, reinterpret_cast<const char *>(gBuf + gPoolOff + off),
                dstLen - 1);
        dst[dstLen - 1] = '\0';
        return;
    }

    spibus::Lock lock;
    if (!gFile) return;
    if (!gFile.seek(gStrtabOff + static_cast<size_t>(index) * 4)) return;
    uint8_t offb[4];
    if (gFile.read(offb, 4) != 4) return;
    const uint32_t off = rd32(offb);
    if (gPoolOff + off >= gSize) return;
    if (!gFile.seek(gPoolOff + off)) return;
    size_t n = 0;
    while (n + 1 < dstLen) {
        uint8_t c = 0;
        if (gFile.read(&c, 1) != 1) break;
        if (c == 0) break;
        dst[n++] = static_cast<char>(c);
    }
    dst[n] = '\0';
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

void copyPadded(char *dst, size_t dstLen, const uint8_t *src, size_t srcLen) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    size_t n = 0;
    while (n < srcLen && src[n] != '\0') ++n;
    if (n > dstLen - 1) n = dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int32_t find(const char *hex) {
    if (!available()) return -1;
    uint32_t want = 0;
    if (!parseIcao(hex, &want)) return -1;

    uint32_t lo = 0, hi = gCount;
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
    out->desc[0] = '\0';
    out->op[0] = '\0';
    out->year = 0;
}

}  // namespace

bool begin() {
    cacheReset();
    gBuf = nullptr;
    gDisk = false;
    gCount = 0;
    if (gFile) gFile.close();

    fs::File f = assets::openDb();
    if (!f) {
        snprintf(gStatus, sizeof(gStatus), "no %s", AIRCRAFT_DB_FILE);
        return false;
    }

    uint8_t header[kHeaderLen];
    size_t size = 0;
    {
        spibus::Lock lock;
        if (f.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
            snprintf(gStatus, sizeof(gStatus), "header truncated");
            f.close();
            return false;
        }
        size = static_cast<size_t>(f.size());
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
        log_w("aircraftdb: table is not a v%u table this build can read",
              static_cast<unsigned>(kVersion));
        f.close();
        return false;
    }

    const size_t need = kHeaderLen + static_cast<size_t>(count) * kRecordLen;
    if (count == 0 || need > size || strtabOff > size || poolOff > size ||
        strtabOff + static_cast<size_t>(strCount) * 4 > size) {
        snprintf(gStatus, sizeof(gStatus), "truncated");
        f.close();
        return false;
    }

    gStrCount = strCount;
    gStrtabOff = strtabOff;
    gPoolOff = poolOff;
    gSize = static_cast<uint32_t>(size);

    const bool tryRam = size <= kMaxRamLoad;
    if (tryRam) {
        gBuf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        if (!gBuf) gBuf = static_cast<uint8_t *>(malloc(size));
    }

    if (gBuf) {
        spibus::Lock lock;
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
        snprintf(gStatus, sizeof(gStatus), "%s %u rec %uk", assets::dbSource(),
                 static_cast<unsigned>(gCount),
                 static_cast<unsigned>(size / 1024));
        log_i("aircraftdb: %u records, %u strings, %u KB in PSRAM (%s)",
              static_cast<unsigned>(gCount), static_cast<unsigned>(gStrCount),
              static_cast<unsigned>(size / 1024), assets::dbSource());
        return true;
    }

    gFile = f;
    gDisk = true;
    gCount = count;
    snprintf(gStatus, sizeof(gStatus), "%s %uk seek", assets::dbSource(),
             static_cast<unsigned>(gCount / 1000));
    log_i("aircraftdb: %u records on %s, seek mode (%u KB)",
          static_cast<unsigned>(gCount), assets::dbSource(),
          static_cast<unsigned>(size / 1024));
    return true;
}

bool available() { return gCount > 0 && (gBuf != nullptr || gDisk); }

bool ramResident() { return gBuf != nullptr && gCount > 0; }

bool details(const char *hex, Details *out) {
    clear(out);
    const int32_t idx = find(hex);
    if (idx < 0 || !out) return false;

    uint8_t rec[kRecordLen];
    if (!fillRecord(static_cast<uint32_t>(idx), rec)) return false;
    copyPadded(out->reg, sizeof(out->reg), rec + 3, kRegLen);
    copyPadded(out->type, sizeof(out->type), rec + 3 + kRegLen, kTypeLen);
    loadString(rd16(rec + 3 + kRegLen + kTypeLen), out->desc, sizeof(out->desc));
    loadString(rd16(rec + 3 + kRegLen + kTypeLen + 2), out->op, sizeof(out->op));
    out->year = rd16(rec + 3 + kRegLen + kTypeLen + 4);
    return true;
}

bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen) {
    if (reg && regLen) reg[0] = '\0';
    if (type && typeLen) type[0] = '\0';

    const int32_t idx = find(hex);
    if (idx < 0) return false;

    uint8_t rec[kRecordLen];
    if (!fillRecord(static_cast<uint32_t>(idx), rec)) return false;
    copyPadded(reg, regLen, rec + 3, kRegLen);
    copyPadded(type, typeLen, rec + 3 + kRegLen, kTypeLen);
    return true;
}

const char *status() { return gStatus; }

}  // namespace aircraftdb
