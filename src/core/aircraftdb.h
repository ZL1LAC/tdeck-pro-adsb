#pragma once

#include <stddef.h>
#include <stdint.h>

// ICAO address -> registration and type designator, from a table flashed into
// SPIFFS by tools/build_db.py.
//
// A local receiver's aircraft.json carries neither field: readsb only serves
// them when started with a --db-file, and holding that database in readsb
// costs a Pi around 50 MB of RAM. Carrying the slice we can actually hear on
// the device costs the Pi nothing and works with no network at all -- the
// mapping is fixed, so there is nothing to keep fresh but a reflash.
//
// Missing data is not an error: available() stays false, lookup() always
// misses, and the fields simply stay empty as they did before.
namespace aircraftdb {

// Mounts SPIFFS if needed and loads the table into PSRAM. Safe to call when no
// database is flashed.
bool begin();

bool available();

// Looks up a 6-digit lower- or upper-case ICAO hex string. `reg` and `type`
// are always NUL-terminated, and left empty on a miss. Returns true only when
// something was found.
bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen);

// One line for the diagnostics page, e.g. "4898 rec 77k" or "no /aircraftdb.bin".
const char *status();

}  // namespace aircraftdb
