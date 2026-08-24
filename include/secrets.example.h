// Your Wi-Fi networks and your home position.
//
//     cp include/secrets.example.h include/secrets.h
//
// then fill in the values below. include/secrets.h is listed in .gitignore, so
// a password and an address stay on your machine rather than going out with
// the first commit. Everything else is tunable in config.h.
#pragma once

#include <stddef.h>

struct WifiCredential {
    const char *ssid;
    const char *pass;  // nullptr or "" for an open network
};

// Tried in order; the first one that associates wins. List as many as you like.
static const WifiCredential kWifiNetworks[] = {
    {"YOUR_SSID", "YOUR_PASSWORD"},
    // {"phone-hotspot", "hotspotpass"},
};
static const size_t kWifiNetworkCount =
    sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);

// Where you are until the GNSS gets a fix -- and permanently, if GNSS_ENABLED
// is false in config.h. This one is Auckland; replace it with yours.
#define HOME_LATITUDE  -36.848460
#define HOME_LONGITUDE 174.763332
