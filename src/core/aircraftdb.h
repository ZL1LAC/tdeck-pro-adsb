#pragma once

#include <stddef.h>
#include <stdint.h>

// ICAO address -> what the aircraft actually is, from a table built by
// tools/build_db.py.
//
// A local receiver's aircraft.json carries none of this: readsb only serves it
// when started with a --db-file, and holding that database in readsb costs a
// Pi around 50 MB of RAM. Carrying the slice we can actually hear on the
// device costs the Pi nothing and works with no network at all -- the mapping
// is fixed, so there is nothing to keep fresh but a reflash.
//
// The file is opened from the SD card if present, otherwise SPIFFS. A regional
// table (~1 MB) is loaded into PSRAM and binary-searched. A worldwide table
// (~30 MB / 615k records) will not fit; that path keeps the file open and
// seeks. details() still works; the per-poll lookup() is skipped in seek mode
// so a 20-probe search is not paid for every target on every fetch.
//
// Missing data is not an error: available() stays false, every lookup misses,
// and the fields stay empty exactly as they did before.
namespace aircraftdb {

struct Details {
    char reg[10];
    char type[6];
    char desc[48];  // MAX_TEXT 47 in build_db.py, plus NUL
    char op[48];
    uint16_t year;  // 0 when unknown
};

// Mounts the filesystem if needed and loads or opens the table. Safe to call
// when no database is present.
bool begin();

bool available();

// True when the whole table is in PSRAM. The fetch task only calls lookup()
// in that case; a seek-backed table would stall the shared SPI bus.
bool ramResident();

// Everything known about one 6-digit ICAO hex address. Returns false on a
// miss, having cleared `out` to empty strings and a null year.
bool details(const char *hex, Details *out);

// The two fields the list and radar need, for the per-poll path where the rest
// would be copied and thrown away. `reg` and `type` are always
// NUL-terminated, and left empty on a miss.
bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen);

// One line for the diagnostics page, e.g. "22807 rec 801k" or "sd 615k seek".
const char *status();

}  // namespace aircraftdb
