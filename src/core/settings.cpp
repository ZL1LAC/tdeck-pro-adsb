#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"

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

// Long enough that a burst of keypresses -- a held zoom, a walk through the
// range steps -- settles into one write rather than a dozen.
constexpr uint32_t kSettleMs = 5000;

constexpr char kNamespace[] = "adsb";
// Bump when the Settings layout changes: an old blob then simply fails to
// match and the defaults are used, which is the correct migration for
// preferences nobody would miss.
constexpr char kKey[] = "v1";

char gStatus[32] = "not loaded";

void applyDefaults(Settings *s) {
    *s = Settings{};
    s->rangeIndex = RANGE_DEFAULT_INDEX;
    s->provider = static_cast<uint8_t>(ADSB_PROVIDER_DEFAULT);
    s->mapEnabled = MAP_ENABLED_BY_DEFAULT;
    s->centreOnGnss = GNSS_CENTRE_BY_DEFAULT;
    s->keypadBacklight = KEYPAD_BACKLIGHT_DEFAULT;
}

// A blob that survived a firmware change could hold anything; anything the UI
// would index with has to be checked before it is trusted.
bool plausible(const Settings &s) {
    if (s.rangeIndex >= kRangeStepCount) return false;
    if (s.provider > static_cast<uint8_t>(AdsbProvider::LOCAL)) return false;
    return true;
}

}  // namespace

void begin() {
    applyDefaults(&gValues);

    if (!gPrefs.begin(kNamespace, false)) {
        snprintf(gStatus, sizeof(gStatus), "NVS unavailable");
        log_w("settings: NVS would not open; using defaults");
        gStored = gValues;
        return;
    }
    gOpen = true;

    Settings loaded{};
    const size_t got = gPrefs.getBytes(kKey, &loaded, sizeof(loaded));
    if (got == sizeof(loaded) && plausible(loaded)) {
        gValues = loaded;
        gLoaded = true;
        snprintf(gStatus, sizeof(gStatus), "loaded");
    } else if (got == 0) {
        snprintf(gStatus, sizeof(gStatus), "defaults (first run)");
    } else {
        snprintf(gStatus, sizeof(gStatus), "defaults (stale blob)");
        log_w("settings: stored blob was %u bytes or implausible; using defaults",
              static_cast<unsigned>(got));
    }
    gStored = gValues;
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

}  // namespace settings
