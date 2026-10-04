#include "assets.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <string.h>

#include "config.h"
#include "core/settings.h"
#include "hw/sdcard.h"
#include "hw/spibus.h"

namespace assets {
namespace {

const char *gMapSrc = "none";
const char *gDbSrc = "none";
const char *gIconSrc = "none";

fs::File trySd(const char *path) {
    if (!path || !path[0] || !sdcard::mounted()) return fs::File();
    spibus::Lock lock;
    if (!SD.exists(path)) return fs::File();
    return SD.open(path, FILE_READ);
}

const char *sourceOf(const char *path, const fs::File &f) {
    if (!f) return "none";
    if (sdcard::mounted()) {
        spibus::Lock lock;
        if (SD.exists(path)) return "sd";
    }
    return "spiffs";
}

}  // namespace

void begin() {
    if (!SPIFFS.begin(false)) {
        log_w("assets: SPIFFS mount failed");
    }
}

fs::File openRead(const char *path) {
    fs::File f = trySd(path);
    if (f) return f;
    if (!path || !path[0]) return fs::File();
    return SPIFFS.open(path, "r");
}

fs::File openMap() {
    const char *pref = settings::mapPath();
    if (pref && pref[0]) {
        fs::File f = openRead(pref);
        gMapSrc = sourceOf(pref, f);
        if (f) return f;
        log_w("assets: map %s missing, falling back", pref);
    }
    fs::File sd = trySd(MAP_FILE);
    if (sd) {
        gMapSrc = "sd";
        return sd;
    }
    fs::File sp = SPIFFS.open(MAP_FILE, "r");
    gMapSrc = sp ? "spiffs" : "none";
    return sp;
}

fs::File openDb() {
    fs::File f = openRead(AIRCRAFT_DB_FILE);
    gDbSrc = sourceOf(AIRCRAFT_DB_FILE, f);
    return f;
}

fs::File openIcons() {
    fs::File f = openRead(ICON_FILE);
    gIconSrc = sourceOf(ICON_FILE, f);
    return f;
}

const char *mapSource() { return gMapSrc; }
const char *dbSource() { return gDbSrc; }
const char *iconSource() { return gIconSrc; }

}  // namespace assets
