#include "net.h"

#include <WiFi.h>
#include <string.h>

#include "config.h"
#include "core/settings.h"

namespace net {
namespace {

State gState = State::Idle;
size_t gCandidate = 0;
uint32_t gAttemptStartedMs = 0;
uint32_t gRetryAtMs = 0;
uint32_t gBackoffMs = 2000;

constexpr uint32_t kBackoffMaxMs = 60000;

size_t networkCount() {
    return kWifiNetworkCount + (settings::wifiOverride() ? 1 : 0);
}

void credentialsAt(size_t index, const char **ssid, const char **pass) {
    if (settings::wifiOverride()) {
        if (index == 0 || kWifiNetworkCount == 0) {
            *ssid = settings::wifiSsid();
            *pass = settings::wifiPass();
            return;
        }
        --index;
    }
    const WifiCredential &cred = kWifiNetworks[index % kWifiNetworkCount];
    *ssid = cred.ssid;
    *pass = cred.pass;
}

void startAttempt(uint32_t now) {
    const size_t n = networkCount();
    if (n == 0) {
        // Nothing to associate with. Park with a retry far in the future
        // rather than returning straight back here on every poll() for the
        // life of the run -- and say so, because the alternative symptom is a
        // radio that silently never comes up.
        log_e("wifi: no networks configured");
        gState = State::Failed;
        gRetryAtMs = now + kBackoffMaxMs;
        return;
    }
    const char *ssid = nullptr;
    const char *pass = nullptr;
    credentialsAt(gCandidate % n, &ssid, &pass);
    log_i("wifi: connecting to \"%s\"", ssid ? ssid : "");
    // wifioff/eraseap both stall for seconds and, with a wedged stack, never
    // come back. Drop the association and try the next network.
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, (pass && pass[0]) ? pass : nullptr);
    gAttemptStartedMs = now;
    gState = State::Connecting;
}

}  // namespace

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.setAutoReconnect(true);
    // Modem sleep is what "locks up after sitting" looks like on this chip:
    // the radio dozes between polls, and a later wake never returns to the
    // Wi-Fi task. The loop then blocks on the first status/RSSI call.
    WiFi.setSleep(false);
    startAttempt(millis());
}

void reconnect() {
    gCandidate = 0;
    gBackoffMs = 2000;
    WiFi.disconnect(true, true);
    gState = State::Failed;
    gRetryAtMs = millis();
    log_i("wifi: reconnect requested");
}

void poll() {
    const uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (gState != State::Connected) {
            log_i("wifi: connected to %s, ip %s, rssi %d",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
            gState = State::Connected;
            gBackoffMs = 2000;
        }
        return;
    }

    switch (gState) {
        case State::Connecting:
            if (now - gAttemptStartedMs > WIFI_CONNECT_TIMEOUT_MS) {
                log_w("wifi: attempt timed out");
                ++gCandidate;
                // Back off only after every configured network has had a go.
                if (gCandidate % networkCount() == 0) {
                    gState = State::Failed;
                    gRetryAtMs = now + gBackoffMs;
                    gBackoffMs = (gBackoffMs * 2 > kBackoffMaxMs) ? kBackoffMaxMs
                                                                  : gBackoffMs * 2;
                } else {
                    startAttempt(now);
                }
            }
            break;

        case State::Connected:  // dropped
            log_w("wifi: link lost");
            gState = State::Failed;
            gRetryAtMs = now + 1000;
            break;

        case State::Idle:
        case State::Failed:
            if (static_cast<int32_t>(now - gRetryAtMs) >= 0) startAttempt(now);
            break;
    }
}

State state() { return gState; }
bool connected() { return WiFi.status() == WL_CONNECTED; }
const char *ssid() {
    // WiFi.SSID() returns a temporary String; copy it so callers get a pointer
    // that stays valid after this returns.
    static char buf[33];
    strncpy(buf, WiFi.SSID().c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}
int rssi() { return WiFi.RSSI(); }

}  // namespace net
