#pragma once

#include <FS.h>

// Open a generated blob (map, aircraft db, icons). SD wins over SPIFFS so a
// card can carry a region without a reflash. Missing data is not an error:
// the caller gets an invalid File and reports it the same way as before.

namespace assets {

void begin();  // mounts SPIFFS; SD is sdcard::begin()'s job

// Empty path: try settings map path, then /map.bin on SD, then SPIFFS.
fs::File openMap();
fs::File openDb();
fs::File openIcons();

// Named path on SD, then SPIFFS, then fail.
fs::File openRead(const char *path);

const char *mapSource();  // "sd", "spiffs", or "none"
const char *dbSource();
const char *iconSource();

}  // namespace assets
