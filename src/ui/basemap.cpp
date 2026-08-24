#include "basemap.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <SPIFFS.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "core/geo.h"
#include "display.h"

namespace basemap {
namespace {

constexpr uint16_t BLACK = 0x0000;

const char kMagic[8] = {'T', 'D', 'E', 'C', 'K', 'M', 'A', 'P'};
constexpr uint16_t kVersion = 1;
constexpr uint8_t kMaxLevels = 4;
constexpr uint8_t kHeaderLen = 32;
constexpr uint8_t kDirEntryLen = 12;
constexpr uint8_t kFeatureHeaderLen = 20;

enum Kind : uint8_t { kCoast = 0, kAirspace = 1, kRunway = 2, kAirport = 3 };

// Stroke patterns, one bit per pixel stepped along the line, LSB first. The
// coastline is solid because it is what the eye anchors on; airspace is dashed
// so its long boundaries cannot compete with traffic for attention.
constexpr uint16_t kSolid = 0xFFFF;
constexpr uint16_t kDashed = 0x0F0F;

struct LevelDir {
    uint32_t offset;
    uint32_t length;
    uint16_t count;
    uint16_t maxRangeNm;
};

bool gReady = false;
LevelDir gLevels[kMaxLevels];
uint16_t gLevelCount = 0;

int gLoaded = -1;  // level currently in gBuf, or -1
uint8_t *gBuf = nullptr;
uint32_t gCapacity = 0;  // bytes allocated
uint32_t gDataLen = 0;   // bytes of gBuf that are valid
char gStatus[32] = "not started";

// The blob is laid out so every int32 is 4-byte aligned, but reading through
// memcpy costs nothing measurable next to a 0.7 s panel refresh and removes
// any chance of an alignment trap on Xtensa.
inline int32_t rd32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

int levelFor(uint16_t rangeNm) {
    for (uint16_t i = 0; i < gLevelCount; ++i) {
        if (rangeNm <= gLevels[i].maxRangeNm) return static_cast<int>(i);
    }
    return static_cast<int>(gLevelCount) - 1;
}

bool loadLevel(int idx) {
    if (idx == gLoaded) return true;

    File f = SPIFFS.open(MAP_FILE, "r");
    if (!f) return false;

    const uint32_t len = gLevels[idx].length;
    if (len > gCapacity) {
        free(gBuf);
        // Prefer PSRAM: the largest level is tens of kilobytes and internal
        // SRAM is better spent on the framebuffer and the TLS stack.
        gBuf = static_cast<uint8_t *>(ps_malloc(len));
        if (gBuf == nullptr) gBuf = static_cast<uint8_t *>(malloc(len));
        if (gBuf == nullptr) {
            gCapacity = 0;
            gLoaded = -1;
            f.close();
            snprintf(gStatus, sizeof(gStatus), "L%d oom %uk", idx,
                     static_cast<unsigned>(len / 1024));
            return false;
        }
        gCapacity = len;
    }

    f.seek(gLevels[idx].offset);
    const size_t got = f.read(gBuf, len);
    f.close();
    if (got != len) {
        gLoaded = -1;
        snprintf(gStatus, sizeof(gStatus), "L%d short read", idx);
        return false;
    }

    gLoaded = idx;
    gDataLen = len;
    snprintf(gStatus, sizeof(gStatus), "L%d %uf %uk", idx,
             static_cast<unsigned>(gLevels[idx].count),
             static_cast<unsigned>(len / 1024));
    return true;
}

// Bresenham with a rotating stroke pattern. Adafruit_GFX only draws solid
// lines, and a dashed basemap is the cheapest way to keep it visually behind
// the traffic on a display with no greys to fall back on.
void styledLine(Adafruit_GFX &d, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                uint16_t pattern) {
    if (pattern == kSolid) {
        d.drawLine(x0, y0, x1, y1, BLACK);
        return;
    }

    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    uint8_t phase = 0;

    for (;;) {
        if (pattern & (1u << (phase & 15))) d.drawPixel(x0, y0, BLACK);
        ++phase;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 = static_cast<int16_t>(x0 + sx);
        }
        if (e2 <= dx) {
            err += dx;
            y0 = static_cast<int16_t>(y0 + sy);
        }
    }
}

// Trims a segment to the plot circle, so the basemap stops at the outer range
// ring instead of spilling across the chrome. Returns false when the segment
// misses the circle entirely.
bool clipToCircle(float *x0, float *y0, float *x1, float *y1, float cx, float cy,
                  float r) {
    const float dx = *x1 - *x0;
    const float dy = *y1 - *y0;
    const float fx = *x0 - cx;
    const float fy = *y0 - cy;

    const float a = dx * dx + dy * dy;
    const float b = 2.0f * (fx * dx + fy * dy);
    const float c = fx * fx + fy * fy - r * r;

    if (a < 1e-6f) return c <= 0.0f;  // degenerate segment: a single point

    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return false;
    disc = sqrtf(disc);

    float t0 = (-b - disc) / (2.0f * a);
    float t1 = (-b + disc) / (2.0f * a);
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 > 1.0f) t1 = 1.0f;
    if (t0 > t1) return false;

    const float ox = *x0, oy = *y0;
    *x0 = ox + dx * t0;
    *y0 = oy + dy * t0;
    *x1 = ox + dx * t1;
    *y1 = oy + dy * t1;
    return true;
}

}  // namespace

bool begin() {
    gReady = false;

    if (!SPIFFS.begin(false)) {
        snprintf(gStatus, sizeof(gStatus), "no filesystem");
        log_w("basemap: SPIFFS mount failed");
        return false;
    }

    File f = SPIFFS.open(MAP_FILE, "r");
    if (!f) {
        snprintf(gStatus, sizeof(gStatus), "no %s", MAP_FILE);
        log_w("basemap: %s missing; run 'pio run -t uploadfs'", MAP_FILE);
        return false;
    }

    uint8_t hdr[kHeaderLen];
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr) ||
        memcmp(hdr, kMagic, sizeof(kMagic)) != 0) {
        snprintf(gStatus, sizeof(gStatus), "bad header");
        f.close();
        return false;
    }

    const uint16_t version = rd16(hdr + 8);
    const uint16_t levels = rd16(hdr + 10);
    if (version != kVersion || levels == 0 || levels > kMaxLevels) {
        snprintf(gStatus, sizeof(gStatus), "v%u/%u levels", version, levels);
        f.close();
        return false;
    }

    uint8_t dir[kDirEntryLen * kMaxLevels];
    if (f.read(dir, kDirEntryLen * levels) != kDirEntryLen * levels) {
        snprintf(gStatus, sizeof(gStatus), "short directory");
        f.close();
        return false;
    }
    f.close();

    for (uint16_t i = 0; i < levels; ++i) {
        const uint8_t *e = dir + kDirEntryLen * i;
        gLevels[i].offset = static_cast<uint32_t>(rd32(e));
        gLevels[i].length = static_cast<uint32_t>(rd32(e + 4));
        gLevels[i].count = rd16(e + 8);
        gLevels[i].maxRangeNm = rd16(e + 10);
    }
    gLevelCount = levels;
    gReady = true;

    snprintf(gStatus, sizeof(gStatus), "%u levels", levels);
    log_i("basemap: %u levels from %s", levels, MAP_FILE);
    return true;
}

bool available() { return gReady; }

const char *status() { return gStatus; }

void draw(double centreLat, double centreLon, float pixelsPerNm, int16_t cx,
          int16_t cy, int16_t radius, uint16_t rangeNm) {
    if (!gReady || pixelsPerNm <= 0.0f) return;
    if (!loadLevel(levelFor(rangeNm))) return;

    Adafruit_GFX &d = display::gfx();

    // Visible window in degrees, with margin so a feature whose vertices all
    // fall outside is still drawn when the edge between them crosses the plot.
    const double visibleNm = radius / pixelsPerNm;
    const double dLat = visibleNm / 60.0 * 1.15;
    const double cosLat = cos(centreLat * M_PI / 180.0);
    const double dLon = dLat / (fabs(cosLat) > 1e-6 ? fabs(cosLat) : 1e-6);

    const int32_t vMinLat = static_cast<int32_t>((centreLat - dLat) * 1e7);
    const int32_t vMaxLat = static_cast<int32_t>((centreLat + dLat) * 1e7);
    const int32_t vMinLon = static_cast<int32_t>((centreLon - dLon) * 1e7);
    const int32_t vMaxLon = static_cast<int32_t>((centreLon + dLon) * 1e7);

    const float fcx = cx, fcy = cy, fr = radius;
    const bool labels = rangeNm <= MAP_LABEL_RANGE_NM;

    // Built once per frame rather than per vertex: this is what used to make
    // the map the most expensive thing on screen.
    const geo::Projector projector(centreLat, centreLon);

    const uint8_t *p = gBuf;
    const uint8_t *const end = gBuf + gDataLen;

    while (p + kFeatureHeaderLen <= end) {
        const int32_t bMinLat = rd32(p);
        const int32_t bMinLon = rd32(p + 4);
        const int32_t bMaxLat = rd32(p + 8);
        const int32_t bMaxLon = rd32(p + 12);
        const uint16_t npts = rd16(p + 16);
        const uint8_t kind = p[18];
        const uint8_t nameLen = p[19];

        const uint8_t *coords = p + kFeatureHeaderLen;
        const uint8_t *name = coords + 8u * npts;
        const uint8_t *next = name + nameLen + ((4u - (nameLen & 3u)) & 3u);
        if (next > end || npts == 0) break;  // truncated or corrupt: stop

        if (bMaxLat < vMinLat || bMinLat > vMaxLat || bMaxLon < vMinLon ||
            bMinLon > vMaxLon) {
            p = next;
            continue;
        }

        auto project = [&](const uint8_t *c, float *sx, float *sy) {
            float e = 0.0f, n = 0.0f;
            projector.project(rd32(c), rd32(c + 4), &e, &n);
            *sx = fcx + e * pixelsPerNm;
            *sy = fcy - n * pixelsPerNm;
        };

        if (kind == kAirport) {
            float x = 0.0f, y = 0.0f;
            project(coords, &x, &y);
            const float ox = x - fcx, oy = y - fcy;
            if (ox * ox + oy * oy <= fr * fr) {
                const int16_t ix = static_cast<int16_t>(lroundf(x));
                const int16_t iy = static_cast<int16_t>(lroundf(y));
                d.drawCircle(ix, iy, 2, BLACK);
                if (labels && nameLen > 0) {
                    char text[8];
                    const uint8_t n = nameLen < sizeof(text) - 1
                                          ? nameLen
                                          : sizeof(text) - 1;
                    memcpy(text, name, n);
                    text[n] = '\0';
                    d.setFont(nullptr);
                    d.setTextSize(1);
                    d.setTextColor(BLACK);
                    d.setCursor(static_cast<int16_t>(ix + 4),
                                static_cast<int16_t>(iy - 3));
                    d.print(text);
                }
            }
            p = next;
            continue;
        }

        const uint16_t pattern = (kind == kAirspace) ? kDashed : kSolid;
        float px = 0.0f, py = 0.0f;
        project(coords, &px, &py);
        for (uint16_t i = 1; i < npts; ++i) {
            float qx = 0.0f, qy = 0.0f;
            project(coords + 8u * i, &qx, &qy);

            // Clip before rounding: an unclipped vertex can be far off panel,
            // and int16 would wrap.
            float ax = px, ay = py, bx = qx, by = qy;
            if (clipToCircle(&ax, &ay, &bx, &by, fcx, fcy, fr)) {
                styledLine(d, static_cast<int16_t>(lroundf(ax)),
                           static_cast<int16_t>(lroundf(ay)),
                           static_cast<int16_t>(lroundf(bx)),
                           static_cast<int16_t>(lroundf(by)), pattern);
            }
            px = qx;
            py = qy;
        }
        p = next;
    }
}

}  // namespace basemap
