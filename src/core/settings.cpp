#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "core/geo.h"
#include "hw/touch.h"

namespace settings {
namespace {

Preferences gPrefs;
Settings gValues;
Settings gStored;   // what NVS actually holds, so a no-op change costs nothing
bool gOpen = false;
bool gDirty = false;
bool gLoaded = false;
uint32_t gDirtyAtMs = 0;
uint32_t gWrites = 0;

char gWifiSsid[kWifiSsidMax + 1] = {0};
char gWifiPass[kWifiPassMax + 1] = {0};
char gStoredWifiSsid[kWifiSsidMax + 1] = {0};
char gStoredWifiPass[kWifiPassMax + 1] = {0};

char gMapPath[kMapPathMax + 1] = {0};
char gStoredMapPath[kMapPathMax + 1] = {0};
bool gLogEnabled = false;
bool gStoredLog = false;

char gLocalUrl[kLocalUrlMax + 1] = {0};
char gStoredLocalUrl[kLocalUrlMax + 1] = {0};
bool gFaceDownIdle = FACE_DOWN_IDLE_DEFAULT;
bool gStoredFaceDownIdle = FACE_DOWN_IDLE_DEFAULT;

// Long enough that a burst of keypresses -- a held zoom, a walk through the
// range steps -- settles into one write rather than a dozen.
constexpr uint32_t kSettleMs = 5000;

constexpr char kNamespace[] = "adsb";
// v1 was the original 5-field blob. v2 adds units, filter, sort, poll, touch
// axes and a captured home. The old key is still read so a flash does not
// throw away the range and feed the user was on.
constexpr char kKeyV1[] = "v1";
constexpr char kKey[] = "v2";
constexpr char kWifiSsidKey[] = "ssid";
constexpr char kWifiPassKey[] = "pass";
constexpr char kMapPathKey[] = "map";
constexpr char kLogKey[] = "log";
constexpr char kLocalUrlKey[] = "lurl";
constexpr char kIdleKey[] = "idle2";  // was "idle"; bump so a face-down default that
                                      // hibernated upright boards does not stick

struct SettingsV1 {
    uint8_t rangeIndex;
    uint8_t provider;
    bool mapEnabled;
    bool centreOnGnss;
    bool keypadBacklight;
};

char gStatus[32] = "not loaded";

void applyDefaults(Settings *s) {
    *s = Settings{};
    s->rangeIndex = RANGE_DEFAULT_INDEX;
    s->provider = static_cast<uint8_t>(ADSB_PROVIDER_DEFAULT);
    s->mapEnabled = MAP_ENABLED_BY_DEFAULT;
    s->centreOnGnss = GNSS_CENTRE_BY_DEFAULT;
    s->keypadBacklight = KEYPAD_BACKLIGHT_DEFAULT;
    s->distanceUnits = static_cast<uint8_t>(UNITS_DISTANCE);
    s->speedUnits = static_cast<uint8_t>(UNITS_SPEED);
    s->trafficFilter = static_cast<uint8_t>(TrafficFilter::All);
    s->listSort = static_cast<uint8_t>(ListSort::Range);
    s->pollPace = static_cast<uint8_t>(PollPace::Fast);
    s->swapXY = false;
    s->mirrorX = false;
    s->mirrorY = false;
    s->homeOverride = false;
    s->homeLatE7 = static_cast<int32_t>(lround(HOME_LATITUDE * 1e7));
    s->homeLonE7 = static_cast<int32_t>(lround(HOME_LONGITUDE * 1e7));
}

bool plausibleV1(const SettingsV1 &s) {
    if (s.rangeIndex >= kRangeStepCount) return false;
    if (s.provider > static_cast<uint8_t>(AdsbProvider::LOCAL)) return false;
    return true;
}

bool plausible(const Settings &s) {
    if (s.rangeIndex >= kRangeStepCount) return false;
    if (s.provider > static_cast<uint8_t>(AdsbProvider::LOCAL)) return false;
    if (s.distanceUnits > 2) return false;
    if (s.speedUnits > 2) return false;
    if (s.trafficFilter > static_cast<uint8_t>(TrafficFilter::High)) return false;
    if (s.listSort > static_cast<uint8_t>(ListSort::Callsign)) return false;
    if (s.pollPace > static_cast<uint8_t>(PollPace::Slow)) return false;
    if (s.keypadBacklight > static_cast<uint8_t>(BacklightMode::Auto)) return false;
    if (s.homeLatE7 < -900000000 || s.homeLatE7 > 900000000) return false;
    if (s.homeLonE7 < -1800000000 || s.homeLonE7 > 1800000000) return false;
    return true;
}

void copyV1(const SettingsV1 &in, Settings *out) {
    applyDefaults(out);
    out->rangeIndex = in.rangeIndex;
    out->provider = in.provider;
    out->mapEnabled = in.mapEnabled;
    out->centreOnGnss = in.centreOnGnss;
    out->keypadBacklight = in.keypadBacklight ? static_cast<uint8_t>(BacklightMode::On)
                                              : static_cast<uint8_t>(BacklightMode::Off);
}

void loadWifi() {
    gWifiSsid[0] = 0;
    gWifiPass[0] = 0;
    if (!gOpen) return;
    gPrefs.getString(kWifiSsidKey, gWifiSsid, sizeof(gWifiSsid));
    gPrefs.getString(kWifiPassKey, gWifiPass, sizeof(gWifiPass));
    strncpy(gStoredWifiSsid, gWifiSsid, sizeof(gStoredWifiSsid) - 1);
    strncpy(gStoredWifiPass, gWifiPass, sizeof(gStoredWifiPass) - 1);
}

void loadMapPath() {
    gMapPath[0] = 0;
    if (!gOpen) return;
    gPrefs.getString(kMapPathKey, gMapPath, sizeof(gMapPath));
    strncpy(gStoredMapPath, gMapPath, sizeof(gStoredMapPath) - 1);
}

void loadLog() {
    gLogEnabled = false;
    if (!gOpen) return;
    gLogEnabled = gPrefs.getBool(kLogKey, false);
    gStoredLog = gLogEnabled;
}

void loadLocalUrl() {
    gLocalUrl[0] = 0;
    if (!gOpen) return;
    gPrefs.getString(kLocalUrlKey, gLocalUrl, sizeof(gLocalUrl));
    strncpy(gStoredLocalUrl, gLocalUrl, sizeof(gStoredLocalUrl) - 1);
}

void loadIdle() {
    gFaceDownIdle = FACE_DOWN_IDLE_DEFAULT;
    if (!gOpen) return;
    gFaceDownIdle = gPrefs.getBool(kIdleKey, FACE_DOWN_IDLE_DEFAULT);
    gStoredFaceDownIdle = gFaceDownIdle;
}

void saveWifiIfChanged() {
    if (!gOpen) return;
    if (strcmp(gWifiSsid, gStoredWifiSsid) == 0 &&
        strcmp(gWifiPass, gStoredWifiPass) == 0) {
        return;
    }
    if (gWifiSsid[0]) {
        gPrefs.putString(kWifiSsidKey, gWifiSsid);
        gPrefs.putString(kWifiPassKey, gWifiPass);
    } else {
        gPrefs.remove(kWifiSsidKey);
        gPrefs.remove(kWifiPassKey);
    }
    strncpy(gStoredWifiSsid, gWifiSsid, sizeof(gStoredWifiSsid) - 1);
    strncpy(gStoredWifiPass, gWifiPass, sizeof(gStoredWifiPass) - 1);
    ++gWrites;
}

void saveMapPathIfChanged() {
    if (!gOpen) return;
    if (strcmp(gMapPath, gStoredMapPath) == 0) return;
    if (gMapPath[0]) {
        gPrefs.putString(kMapPathKey, gMapPath);
    } else {
        gPrefs.remove(kMapPathKey);
    }
    strncpy(gStoredMapPath, gMapPath, sizeof(gStoredMapPath) - 1);
    ++gWrites;
}

void saveLogIfChanged() {
    if (!gOpen) return;
    if (gLogEnabled == gStoredLog) return;
    gPrefs.putBool(kLogKey, gLogEnabled);
    gStoredLog = gLogEnabled;
    ++gWrites;
}

void saveLocalUrlIfChanged() {
    if (!gOpen) return;
    if (strcmp(gLocalUrl, gStoredLocalUrl) == 0) return;
    if (gLocalUrl[0]) {
        gPrefs.putString(kLocalUrlKey, gLocalUrl);
    } else {
        gPrefs.remove(kLocalUrlKey);
    }
    strncpy(gStoredLocalUrl, gLocalUrl, sizeof(gStoredLocalUrl) - 1);
    ++gWrites;
}

void saveIdleIfChanged() {
    if (!gOpen) return;
    if (gFaceDownIdle == gStoredFaceDownIdle) return;
    gPrefs.putBool(kIdleKey, gFaceDownIdle);
    gStoredFaceDownIdle = gFaceDownIdle;
    ++gWrites;
}

}  // namespace

void applyLive() {
    geo::setDisplayUnits(gValues.distanceUnits, gValues.speedUnits);
    touch::setAxisFlags(gValues.swapXY, gValues.mirrorX, gValues.mirrorY);
}

void begin() {
    applyDefaults(&gValues);

    if (!gPrefs.begin(kNamespace, false)) {
        snprintf(gStatus, sizeof(gStatus), "NVS unavailable");
        log_w("settings: NVS would not open; using defaults");
        gStored = gValues;
        applyLive();
        return;
    }
    gOpen = true;

    Settings loaded{};
    const size_t got = gPrefs.getBytes(kKey, &loaded, sizeof(loaded));
    if (got == sizeof(loaded) && plausible(loaded)) {
        gValues = loaded;
        gLoaded = true;
        snprintf(gStatus, sizeof(gStatus), "loaded");
    } else {
        SettingsV1 v1{};
        const size_t gotV1 = gPrefs.getBytes(kKeyV1, &v1, sizeof(v1));
        if (gotV1 == sizeof(v1) && plausibleV1(v1)) {
            copyV1(v1, &gValues);
            gLoaded = true;
            gDirty = true;
            gDirtyAtMs = millis();
            snprintf(gStatus, sizeof(gStatus), "migrated v1");
            log_i("settings: migrated v1 blob to v2");
        } else if (got == 0 && gotV1 == 0) {
            snprintf(gStatus, sizeof(gStatus), "defaults (first run)");
        } else {
            snprintf(gStatus, sizeof(gStatus), "defaults (stale blob)");
            log_w("settings: stored blob was %u bytes or implausible; using defaults",
                  static_cast<unsigned>(got));
        }
    }
    gStored = gValues;
    loadWifi();
    loadMapPath();
    loadLog();
    loadLocalUrl();
    loadIdle();
    applyLive();
}

Settings &get() { return gValues; }

void markDirty() {
    gDirty = true;
    gDirtyAtMs = millis();
}

void poll() {
    if (!gDirty) return;
    if (static_cast<int32_t>(millis() - (gDirtyAtMs + kSettleMs)) < 0) return;
    gDirty = false;

    saveWifiIfChanged();
    saveMapPathIfChanged();
    saveLogIfChanged();
    saveLocalUrlIfChanged();
    saveIdleIfChanged();

    // Toggling a setting and toggling it straight back should cost no flash.
    if (memcmp(&gValues, &gStored, sizeof(gValues)) == 0) return;
    if (!gOpen) return;

    const size_t put = gPrefs.putBytes(kKey, &gValues, sizeof(gValues));
    if (put != sizeof(gValues)) {
        log_w("settings: write returned %u", static_cast<unsigned>(put));
        return;
    }
    gStored = gValues;
    ++gWrites;
    snprintf(gStatus, sizeof(gStatus), "%s, %u write%s",
             gLoaded ? "loaded" : "defaults", static_cast<unsigned>(gWrites),
             gWrites == 1 ? "" : "s");
    log_i("settings: saved");
}

const char *status() { return gStatus; }

bool wifiOverride() { return gWifiSsid[0] != '\0'; }

const char *wifiSsid() { return gWifiSsid; }

const char *wifiPass() { return gWifiPass; }

void setWifiOverride(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) {
        clearWifiOverride();
        return;
    }
    strncpy(gWifiSsid, ssid, kWifiSsidMax);
    gWifiSsid[kWifiSsidMax] = '\0';
    if (pass) {
        strncpy(gWifiPass, pass, kWifiPassMax);
        gWifiPass[kWifiPassMax] = '\0';
    } else {
        gWifiPass[0] = '\0';
    }
    saveWifiIfChanged();
    snprintf(gStatus, sizeof(gStatus), "%s, %u write%s",
             gLoaded ? "loaded" : "defaults", static_cast<unsigned>(gWrites),
             gWrites == 1 ? "" : "s");
}

void clearWifiOverride() {
    gWifiSsid[0] = '\0';
    gWifiPass[0] = '\0';
    saveWifiIfChanged();
}

void adoptWifiOverride(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    strncpy(gWifiSsid, ssid, kWifiSsidMax);
    gWifiSsid[kWifiSsidMax] = '\0';
    if (pass) {
        strncpy(gWifiPass, pass, kWifiPassMax);
        gWifiPass[kWifiPassMax] = '\0';
    } else {
        gWifiPass[0] = '\0';
    }
    // Pretend this is what NVS already holds, so poll() will not write a
    // travel card's network into flash.
    strncpy(gStoredWifiSsid, gWifiSsid, sizeof(gStoredWifiSsid) - 1);
    strncpy(gStoredWifiPass, gWifiPass, sizeof(gStoredWifiPass) - 1);
}

const char *mapPath() { return gMapPath; }

void setMapPath(const char *path) {
    if (!path) path = "";
    strncpy(gMapPath, path, kMapPathMax);
    gMapPath[kMapPathMax] = '\0';
    saveMapPathIfChanged();
}

const char *mapLabel() {
    if (!gMapPath[0]) return "auto";
    const char *slash = strrchr(gMapPath, '/');
    return slash ? slash + 1 : gMapPath;
}

bool logEnabled() { return gLogEnabled; }

void setLogEnabled(bool on) {
    gLogEnabled = on;
    saveLogIfChanged();
}

bool localUrlOverride() { return gLocalUrl[0] != '\0'; }

const char *localUrl() { return gLocalUrl; }

void setLocalUrl(const char *url) {
    if (!url || !url[0]) {
        clearLocalUrl();
        return;
    }
    strncpy(gLocalUrl, url, kLocalUrlMax);
    gLocalUrl[kLocalUrlMax] = '\0';
    saveLocalUrlIfChanged();
}

void clearLocalUrl() {
    gLocalUrl[0] = '\0';
    saveLocalUrlIfChanged();
}

void adoptLocalUrl(const char *url) {
    if (!url || !url[0]) return;
    strncpy(gLocalUrl, url, kLocalUrlMax);
    gLocalUrl[kLocalUrlMax] = '\0';
    strncpy(gStoredLocalUrl, gLocalUrl, sizeof(gStoredLocalUrl) - 1);
}

bool faceDownIdle() { return gFaceDownIdle; }

void setFaceDownIdle(bool on) {
    gFaceDownIdle = on;
    saveIdleIfChanged();
}

double homeLatitude() {
    if (gValues.homeOverride) return gValues.homeLatE7 * 1e-7;
    return HOME_LATITUDE;
}

double homeLongitude() {
    if (gValues.homeOverride) return gValues.homeLonE7 * 1e-7;
    return HOME_LONGITUDE;
}

void adoptHome(double lat, double lon) {
    gValues.homeOverride = true;
    gValues.homeLatE7 = static_cast<int32_t>(lround(lat * 1e7));
    gValues.homeLonE7 = static_cast<int32_t>(lround(lon * 1e7));
    gStored.homeOverride = gValues.homeOverride;
    gStored.homeLatE7 = gValues.homeLatE7;
    gStored.homeLonE7 = gValues.homeLonE7;
}

}  // namespace settings
