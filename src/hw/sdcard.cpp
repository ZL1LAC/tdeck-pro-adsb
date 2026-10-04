#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "board_pins.h"
#include "core/settings.h"
#include "hw/clock.h"
#include "hw/spibus.h"
#include "ui/display.h"

namespace sdcard {
namespace {

constexpr uint32_t kSpiHz = 10000000;
constexpr uint32_t kLogSettleMs = 2000;
constexpr size_t kLogBufLen = 1024;

bool gMounted = false;
char gStatus[32] = "not started";
MapList gMaps;
uint64_t gSize = 0;
uint64_t gUsed = 0;

char gLogBuf[kLogBufLen];
size_t gLogLen = 0;
uint32_t gLogDirtyAtMs = 0;
uint32_t gLogBytes = 0;

bool gShotPending = false;
char gLastShot[24] = "";
uint16_t gShotCount = 0;

void trim(char *s) {
    if (!s) return;
    char *a = s;
    while (*a && isspace(static_cast<unsigned char>(*a))) ++a;
    char *b = a + strlen(a);
    while (b > a && isspace(static_cast<unsigned char>(b[-1]))) --b;
    *b = '\0';
    if (a != s) memmove(s, a, static_cast<size_t>(b - a) + 1);
}

const char *basenameOf(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool endsWithBin(const char *name) {
    const size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".bin") == 0;
}

bool ensureDir(const char *path) {
    if (SD.exists(path)) return true;
    return SD.mkdir(path);
}

void parseWifiText(char *text) {
    char ssid[settings::kWifiSsidMax + 1] = {0};
    char pass[settings::kWifiPassMax + 1] = {0};
    int plain = 0;
    char *save = nullptr;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(nullptr, "\n", &save)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strncasecmp(line, "ssid=", 5) == 0) {
            strncpy(ssid, line + 5, sizeof(ssid) - 1);
            trim(ssid);
        } else if (strncasecmp(line, "pass=", 5) == 0 ||
                   strncasecmp(line, "password=", 9) == 0) {
            const char *v = strchr(line, '=');
            if (v) {
                strncpy(pass, v + 1, sizeof(pass) - 1);
                trim(pass);
            }
        } else if (plain == 0) {
            strncpy(ssid, line, sizeof(ssid) - 1);
            ++plain;
        } else if (plain == 1) {
            strncpy(pass, line, sizeof(pass) - 1);
            ++plain;
        }
    }
    if (!ssid[0]) return;
    settings::adoptWifiOverride(ssid, pass);
    log_i("sdcard: wifi.txt applied, ssid=%s", ssid);
}

void parseHomeText(char *text) {
    double lat = 0, lon = 0;
    bool haveLat = false, haveLon = false;
    char *save = nullptr;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(nullptr, "\n", &save)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strncasecmp(line, "lat=", 4) == 0) {
            lat = strtod(line + 4, nullptr);
            haveLat = true;
        } else if (strncasecmp(line, "lon=", 4) == 0 ||
                   strncasecmp(line, "lng=", 4) == 0) {
            lon = strtod(strchr(line, '=') + 1, nullptr);
            haveLon = true;
        } else {
            char *comma = strchr(line, ',');
            if (comma) {
                *comma = '\0';
                lat = strtod(line, nullptr);
                lon = strtod(comma + 1, nullptr);
                haveLat = haveLon = true;
            } else if (!haveLat) {
                lat = strtod(line, nullptr);
                haveLat = true;
            } else if (!haveLon) {
                lon = strtod(line, nullptr);
                haveLon = true;
            }
        }
    }
    if (!haveLat || !haveLon) return;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        log_w("sdcard: home.txt coordinates out of range");
        return;
    }
    settings::adoptHome(lat, lon);
    log_i("sdcard: home.txt applied (%.4f, %.4f)", lat, lon);
}

void parseLocalText(char *text) {
    char url[settings::kLocalUrlMax + 1] = {0};
    char *save = nullptr;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(nullptr, "\n", &save)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strncasecmp(line, "url=", 4) == 0) {
            strncpy(url, line + 4, sizeof(url) - 1);
            trim(url);
        } else if (!url[0]) {
            strncpy(url, line, sizeof(url) - 1);
            trim(url);
        }
    }
    if (!url[0]) return;
    if (strncasecmp(url, "http://", 7) != 0 &&
        strncasecmp(url, "https://", 8) != 0) {
        log_w("sdcard: local.txt URL must start with http:// or https://");
        return;
    }
    settings::adoptLocalUrl(url);
    log_i("sdcard: local.txt applied");
}

bool readSmallFile(const char *path, char *dst, size_t dstLen) {
    spibus::Lock lock;
    if (!SD.exists(path)) return false;
    fs::File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t n = f.read(reinterpret_cast<uint8_t *>(dst), dstLen - 1);
    f.close();
    dst[n] = '\0';
    return n > 0;
}

void formatShotName(char *dst, size_t dstLen) {
    time_t now = time(nullptr);
    struct tm tm;
    if (wallclock::valid() && now > 1600000000 && localtime_r(&now, &tm)) {
        snprintf(dst, dstLen, "/shots/%04u%02u%02u-%02u%02u%02u.pbm",
                 static_cast<unsigned>(tm.tm_year + 1900) % 10000u,
                 static_cast<unsigned>(tm.tm_mon + 1) % 100u,
                 static_cast<unsigned>(tm.tm_mday) % 100u,
                 static_cast<unsigned>(tm.tm_hour) % 100u,
                 static_cast<unsigned>(tm.tm_min) % 100u,
                 static_cast<unsigned>(tm.tm_sec) % 100u);
        return;
    }
    snprintf(dst, dstLen, "/shots/%lu.pbm", static_cast<unsigned long>(millis()));
}

}  // namespace

void begin() {
    gMounted = false;
    gMaps.count = 0;
    gSize = 0;
    gUsed = 0;

    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);

    spibus::Lock lock;
    if (!SD.begin(BOARD_SD_CS, SPI, kSpiHz)) {
        snprintf(gStatus, sizeof(gStatus), "no card");
        log_i("sdcard: no card, or mount failed");
        return;
    }

    const uint8_t type = SD.cardType();
    if (type == CARD_NONE) {
        snprintf(gStatus, sizeof(gStatus), "no card");
        SD.end();
        digitalWrite(BOARD_SD_CS, HIGH);
        return;
    }

    gMounted = true;
    gSize = SD.cardSize();
    gUsed = SD.usedBytes();
    const unsigned mb = static_cast<unsigned>(gSize / (1024ull * 1024ull));
    snprintf(gStatus, sizeof(gStatus), "ok %u MB", mb);
    log_i("sdcard: mounted, %u MB", mb);

    scanMaps();
}

bool mounted() { return gMounted; }

const char *status() { return gStatus; }

uint64_t sizeBytes() { return gSize; }
uint64_t usedBytes() { return gUsed; }

const MapList &maps() { return gMaps; }

void scanMaps() {
    gMaps.count = 0;
    if (!gMounted) return;
    spibus::Lock lock;
    fs::File dir = SD.open("/maps");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }
    for (;;) {
        fs::File entry = dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }
        const char *base = basenameOf(entry.name());
        if (!endsWithBin(base)) {
            entry.close();
            continue;
        }
        if (gMaps.count >= kMaxMaps) {
            entry.close();
            break;
        }
        strncpy(gMaps.name[gMaps.count], base, kMapNameMax - 1);
        gMaps.name[gMaps.count][kMapNameMax - 1] = '\0';
        ++gMaps.count;
        entry.close();
    }
    dir.close();

    for (uint8_t i = 1; i < gMaps.count; ++i) {
        char tmp[kMapNameMax];
        strncpy(tmp, gMaps.name[i], kMapNameMax);
        uint8_t j = i;
        while (j > 0 && strcasecmp(gMaps.name[j - 1], tmp) > 0) {
            strncpy(gMaps.name[j], gMaps.name[j - 1], kMapNameMax);
            --j;
        }
        strncpy(gMaps.name[j], tmp, kMapNameMax);
    }
}

const char *cycleMapPath(int dir) {
    if (dir == 0) dir = 1;
    const int step = dir > 0 ? 1 : -1;
    scanMaps();
    const int n = 1 + static_cast<int>(gMaps.count);
    const char *cur = settings::mapPath();
    int idx = 0;
    if (cur && cur[0]) {
        const char *want = basenameOf(cur);
        for (uint8_t i = 0; i < gMaps.count; ++i) {
            if (strcasecmp(gMaps.name[i], want) == 0) {
                idx = 1 + static_cast<int>(i);
                break;
            }
        }
    }
    idx = (idx + step) % n;
    if (idx < 0) idx += n;
    if (idx == 0) {
        settings::setMapPath("");
    } else {
        char path[settings::kMapPathMax + 1];
        snprintf(path, sizeof(path), "/maps/%s", gMaps.name[idx - 1]);
        settings::setMapPath(path);
    }
    return settings::mapPath();
}

void applyConfigFiles() {
    if (!gMounted) return;
    char buf[256];
    if (readSmallFile("/wifi.txt", buf, sizeof(buf))) parseWifiText(buf);
    if (readSmallFile("/home.txt", buf, sizeof(buf))) parseHomeText(buf);
    if (readSmallFile("/local.txt", buf, sizeof(buf))) parseLocalText(buf);
}

void logTraffic(const char *line) {
    if (!gMounted || !line || !line[0]) return;
    const size_t n = strlen(line);
    if (n + 1 >= kLogBufLen) return;
    if (gLogLen + n + 1 > kLogBufLen) flushLog();
    memcpy(gLogBuf + gLogLen, line, n);
    gLogLen += n;
    if (gLogLen == 0 || gLogBuf[gLogLen - 1] != '\n') {
        gLogBuf[gLogLen++] = '\n';
    }
    gLogDirtyAtMs = millis();
}

void flushLog() {
    if (!gMounted || gLogLen == 0) return;
    spibus::Lock lock;
    if (!ensureDir("/logs")) {
        log_w("sdcard: could not create /logs");
        gLogLen = 0;
        return;
    }
    fs::File f = SD.open("/logs/adsb.csv", FILE_APPEND);
    if (!f) {
        log_w("sdcard: could not open /logs/adsb.csv");
        gLogLen = 0;
        return;
    }
    if (f.size() == 0) {
        f.print("time,hex,flight,reg,lat,lon,alt_ft,gs_kt,track\n");
    }
    f.write(reinterpret_cast<const uint8_t *>(gLogBuf), gLogLen);
    gLogBytes += static_cast<uint32_t>(gLogLen);
    f.close();
    gLogLen = 0;
    gUsed = SD.usedBytes();
}

void poll() {
    if (gShotPending) {
        gShotPending = false;
        saveScreenshot();
    }
    if (gLogLen == 0) return;
    if (static_cast<int32_t>(millis() - (gLogDirtyAtMs + kLogSettleMs)) < 0) {
        return;
    }
    flushLog();
}

bool saveScreenshot() {
    if (!gMounted) return false;
    const size_t bytes = display::frameBytes();
    uint8_t *buf = static_cast<uint8_t *>(malloc(bytes));
    if (!buf) return false;
    if (!display::copyFrame(buf, bytes)) {
        free(buf);
        return false;
    }
    for (size_t i = 0; i < bytes; ++i) buf[i] = static_cast<uint8_t>(~buf[i]);

    char path[40];
    formatShotName(path, sizeof(path));

    bool ok = false;
    {
        spibus::Lock lock;
        if (!ensureDir("/shots")) {
            free(buf);
            return false;
        }
        fs::File f = SD.open(path, FILE_WRITE);
        if (f) {
            f.print("P4\n");
            f.printf("%d %d\n", EPD_WIDTH, EPD_HEIGHT);
            f.write(buf, bytes);
            f.close();
            ok = true;
            strncpy(gLastShot, basenameOf(path), sizeof(gLastShot) - 1);
            gLastShot[sizeof(gLastShot) - 1] = '\0';
            ++gShotCount;
            gUsed = SD.usedBytes();
            log_i("sdcard: wrote %s", path);
        }
    }
    free(buf);
    return ok;
}

void requestScreenshot() { gShotPending = true; }

const char *lastShot() { return gLastShot[0] ? gLastShot : "none"; }

uint16_t shotCount() { return gShotCount; }

}  // namespace sdcard
