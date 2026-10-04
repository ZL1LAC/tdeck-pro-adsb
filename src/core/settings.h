#pragma once

#include <stddef.h>
#include <stdint.h>

// Small persistent slice of user state, kept in NVS.
//
// Only choices the user made deliberately and would be irritated to make again
// after a reboot: the range they were on, whether the basemap was up, which
// feed they picked, units, filters, the home they captured. Not the pan offset
// or the selection -- those are moment-to-moment, and persisting them would
// restore a view of traffic that has long since flown away.
//
// Wi-Fi SSID/password are stored as separate NVS strings, not in this blob.

enum class TrafficFilter : uint8_t { All = 0, Airborne = 1, Low = 2, High = 3 };
enum class ListSort : uint8_t { Range = 0, Alt = 1, Speed = 2, Callsign = 3 };
enum class PollPace : uint8_t { Fast = 0, Normal = 1, Slow = 2 };
enum class BacklightMode : uint8_t { Off = 0, On = 1, Auto = 2 };

struct Settings {
    uint8_t rangeIndex;
    uint8_t provider;  // AdsbProvider, as a byte so the struct stays POD
    bool mapEnabled;
    bool centreOnGnss;
    uint8_t keypadBacklight;  // BacklightMode. Same byte as the old bool.
    uint8_t distanceUnits;  // 0 = nm, 1 = mi, 2 = km
    uint8_t speedUnits;     // 0 = kt, 1 = mph, 2 = kph
    uint8_t trafficFilter;  // TrafficFilter
    uint8_t listSort;       // ListSort
    uint8_t pollPace;       // PollPace
    bool swapXY;
    bool mirrorX;
    bool mirrorY;
    bool homeOverride;
    int32_t homeLatE7;
    int32_t homeLonE7;
};

namespace settings {

constexpr size_t kWifiSsidMax = 32;
constexpr size_t kWifiPassMax = 63;
constexpr size_t kLocalUrlMax = 95;

// Loads the saved set, falling back to the config.h defaults when nothing has
// been stored yet or when the stored blob does not match this build's layout.
// A v1 blob is migrated field-by-field so range/feed/map/backlight survive.
// Call before anything reads a setting.
void begin();

// The live values. Mutate through here, then call markDirty().
Settings &get();

// Push units and touch-axis flags into the modules that consume them.
// begin() does this; call again after those fields change.
void applyLive();

// Note that something changed. The write itself is deferred -- see poll().
void markDirty();

// Flushes a dirty set once it has stopped changing. NVS wears with every
// write and a held-down zoom key can step the range a dozen times in a
// second, so the write waits for the user to settle and is skipped entirely
// when the bytes match what is already stored.
void poll();

// One line for the diagnostics page.
const char *status();

// Optional override network, tried before the compiled secrets.h list.
// Empty SSID means "no override". Password is never logged.
bool wifiOverride();
const char *wifiSsid();
const char *wifiPass();
void setWifiOverride(const char *ssid, const char *pass);
void clearWifiOverride();

// RAM-only: a wifi.txt on the SD card. Does not write NVS, so removing the
// card restores the saved network on the next boot.
void adoptWifiOverride(const char *ssid, const char *pass);

// Optional local feed URL override. Empty means secrets.h ADSB_LOCAL_URL.
bool localUrlOverride();
const char *localUrl();
void setLocalUrl(const char *url);
void clearLocalUrl();
void adoptLocalUrl(const char *url);  // RAM-only, from /local.txt

// Empty path means "search /map.bin on SD, then SPIFFS". A non-empty path is
// opened as written (typically /maps/something.bin on the card).
constexpr size_t kMapPathMax = 47;
const char *mapPath();
void setMapPath(const char *path);
const char *mapLabel();  // "auto" or the basename, for the settings row

bool logEnabled();
void setLogEnabled(bool on);

// Park the panel when the IMU reports face-down.
bool faceDownIdle();
void setFaceDownIdle(bool on);

// Home used when GNSS centring is off. Override wins over secrets.h.
double homeLatitude();
double homeLongitude();

// RAM-only: a home.txt on the SD card. Same persistence rule as wifi.txt.
void adoptHome(double lat, double lon);

}  // namespace settings
