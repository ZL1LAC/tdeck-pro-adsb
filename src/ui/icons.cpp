#include "icons.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config.h"
#include "display.h"

namespace icons {
namespace {

const char kMagic[8] = {'T', 'D', 'E', 'C', 'K', 'I', 'C', 'O'};
constexpr uint16_t kVersion = 1;
constexpr uint8_t kHeaderLen = 32;

// Mirrors tools/build_icons.py.
constexpr uint8_t kDesignatorLen = 4;
constexpr uint8_t kMapEntryLen = kDesignatorLen + 2;
constexpr uint8_t kShapeEntryLen = 8;

constexpr uint16_t BLACK = 0x0000;

// The whole file, so the offsets in the header are usable as written.
uint8_t *gBuf = nullptr;
uint16_t gShapeCount = 0;
uint16_t gMapCount = 0;
uint32_t gShapeOff = 0;
uint32_t gMapOff = 0;
uint32_t gBitmapOff = 0;
char gStatus[32] = "not started";

inline uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// A designator as the table stores it: upper case, NUL-padded to four bytes.
// The build tool sorts on exactly this, so the comparison here and the
// ordering there cannot disagree.
void padKey(const char *type, uint8_t *out) {
    memset(out, 0, kDesignatorLen);
    for (uint8_t i = 0; i < kDesignatorLen && type[i]; ++i) {
        const char c = type[i];
        out[i] = (c >= 'a' && c <= 'z') ? static_cast<uint8_t>(c - 'a' + 'A')
                                        : static_cast<uint8_t>(c);
    }
}

// Index into the shape table, or -1.
int32_t findShape(const char *type) {
    if (!available() || !type || !type[0]) return -1;

    uint8_t want[kDesignatorLen];
    padKey(type, want);

    uint32_t lo = 0, hi = gMapCount;  // half-open
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *entry = gBuf + gMapOff + static_cast<size_t>(mid) * kMapEntryLen;
        const int cmp = memcmp(want, entry, kDesignatorLen);
        if (cmp == 0) {
            const uint16_t idx = rd16(entry + kDesignatorLen);
            return idx < gShapeCount ? static_cast<int32_t>(idx) : -1;
        }
        if (cmp > 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

const uint8_t *shapeEntry(int32_t index) {
    return gBuf + gShapeOff + static_cast<size_t>(index) * kShapeEntryLen;
}

}  // namespace

bool begin() {
    if (!SPIFFS.begin(false)) {
        snprintf(gStatus, sizeof(gStatus), "no SPIFFS");
        return false;
    }

    File f = SPIFFS.open(ICON_FILE, "r");
    if (!f) {
        snprintf(gStatus, sizeof(gStatus), "no %s", ICON_FILE);
        return false;
    }

    uint8_t header[kHeaderLen];
    if (f.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
        snprintf(gStatus, sizeof(gStatus), "header truncated");
        f.close();
        return false;
    }

    const uint16_t version = rd16(header + 8);
    const uint16_t shapeCount = rd16(header + 10);
    const uint16_t mapCount = rd16(header + 12);
    const uint32_t shapeOff = rd32(header + 16);
    const uint32_t mapOff = rd32(header + 20);
    const uint32_t bitmapOff = rd32(header + 24);

    if (memcmp(header, kMagic, sizeof(kMagic)) != 0 || version != kVersion) {
        snprintf(gStatus, sizeof(gStatus), "format mismatch");
        f.close();
        return false;
    }

    const size_t size = static_cast<size_t>(f.size());
    if (shapeCount == 0 || mapCount == 0 || bitmapOff > size ||
        shapeOff + static_cast<size_t>(shapeCount) * kShapeEntryLen > size ||
        mapOff + static_cast<size_t>(mapCount) * kMapEntryLen > size) {
        snprintf(gStatus, sizeof(gStatus), "truncated");
        f.close();
        return false;
    }

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

    gShapeCount = shapeCount;
    gMapCount = mapCount;
    gShapeOff = shapeOff;
    gMapOff = mapOff;
    gBitmapOff = bitmapOff;

    snprintf(gStatus, sizeof(gStatus), "%u types %uk",
             static_cast<unsigned>(gMapCount),
             static_cast<unsigned>(size / 1024));
    log_i("icons: %u shapes, %u designators, %u KB in PSRAM",
          static_cast<unsigned>(gShapeCount), static_cast<unsigned>(gMapCount),
          static_cast<unsigned>(size / 1024));
    return true;
}

bool available() { return gBuf != nullptr && gShapeCount > 0; }

bool size(const char *type, int16_t *w, int16_t *h) {
    const int32_t idx = findShape(type);
    if (idx < 0) return false;
    const uint8_t *e = shapeEntry(idx);
    if (w) *w = static_cast<int16_t>(rd16(e));
    if (h) *h = static_cast<int16_t>(rd16(e + 2));
    return true;
}

bool draw(const char *type, int16_t x, int16_t y) {
    const int32_t idx = findShape(type);
    if (idx < 0) return false;

    const uint8_t *e = shapeEntry(idx);
    const int16_t w = static_cast<int16_t>(rd16(e));
    const int16_t h = static_cast<int16_t>(rd16(e + 2));
    const uint8_t *bits = gBuf + gBitmapOff + rd32(e + 4);

    // Rows are byte-aligned and MSB-first, which is the layout drawBitmap()
    // reads. Clear bits are left alone rather than painted white, so the
    // silhouette drops onto whatever is behind it.
    display::gfx().drawBitmap(x, y, bits, w, h, BLACK);
    return true;
}

const char *status() { return gStatus; }

}  // namespace icons
