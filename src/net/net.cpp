#include "net.h"

#include <WiFi.h>
#include <string.h>

#include "config.h"

namespace net {
namespace {

State gState = State::Idle;
size_t gCandidate = 0;
uint32_t gAttemptStartedMs = 0;
uint32_t gRetryAtMs = 0;
uint32_t gBackoffMs = 2000;

constexpr uint32_t kBackoffMaxMs = 60000;

void startAttempt(uint32_t now) {
    if (kWifiNetworkCount == 0) {
        // Nothing to associate with. Park with a retry far in the future
        // rather than returning straight back here on every poll() for the
        // life of the run -- and say so, because the alternative symptom is a
        // radio that silently never comes up.
        log_e("wifi: secrets.h configures no networks");
        gState = State::Failed;
        gRetryAtMs = now + kBackoffMaxMs;
        return;
    }
    const WifiCredential &cred = kWifiNetworks[gCandidate % kWifiNetworkCount];
    log_i("wifi: connecting to \"%s\"", cred.ssid);
    WiFi.disconnect(true, true);
    WiFi.begin(cred.ssid, (cred.pass && cred.pass[0]) ? cred.pass : nullptr);
    gAttemptStartedMs = now;
    gState = State::Connecting;
}

}  // namespace

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);  // modem sleep: the poll interval is measured in seconds
    startAttempt(millis());
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
                if (gCandidate % kWifiNetworkCount == 0) {
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
