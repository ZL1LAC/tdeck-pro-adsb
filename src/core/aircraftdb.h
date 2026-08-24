#pragma once

#include <stddef.h>
#include <stdint.h>

// ICAO address -> what the aircraft actually is, from a table flashed into
// SPIFFS by tools/build_db.py.
//
// A local receiver's aircraft.json carries none of this: readsb only serves it
// when started with a --db-file, and holding that database in readsb costs a
// Pi around 50 MB of RAM. Carrying the slice we can actually hear on the
// device costs the Pi nothing and works with no network at all -- the mapping
// is fixed, so there is nothing to keep fresh but a reflash.
//
// Missing data is not an error: available() stays false, every lookup misses,
// and the fields stay empty exactly as they did before.
namespace aircraftdb {

struct Details {
    // Fixed-width in the record, so these are copied out.
    char reg[10];
    char type[6];

    // Pointers into the pooled strings in PSRAM, which live for the run and
    // are never null -- an absent field reads as "". Thousands of aircraft
    // share one copy of "CESSNA 172 Skyhawk", which is what makes carrying
    // descriptions affordable at all.
    const char *desc;
    const char *op;

    uint16_t year;  // 0 when unknown
};

// Mounts SPIFFS if needed and loads the table into PSRAM. Safe to call when no
// database is flashed.
bool begin();

bool available();

// Everything known about one 6-digit ICAO hex address. Returns false on a
// miss, having cleared `out` to empty strings and a null year.
bool details(const char *hex, Details *out);

// The two fields the list and radar need, for the per-poll path where the rest
// would be copied and thrown away. `reg` and `type` are always
// NUL-terminated, and left empty on a miss.
bool lookup(const char *hex, char *reg, size_t regLen, char *type,
            size_t typeLen);

// One line for the diagnostics page, e.g. "22807 rec 801k".
const char *status();

}  // namespace aircraftdb
