#pragma once

#include <stddef.h>
#include <stdint.h>

// SD slot on the shared SPI bus, CS = BOARD_SD_CS.
//
// Held inactive from power::begin() so a card that is not mounted cannot
// steal the bus from the e-paper. begin() after display::begin() (SPI is
// already up). Safe to call with no card: mounted() stays false and every
// helper no-ops.

namespace sdcard {

constexpr int kMaxMaps = 12;
constexpr int kMapNameMax = 24;

struct MapList {
    uint8_t count = 0;
    char name[kMaxMaps][kMapNameMax];  // basename, e.g. "nz.bin"
};

void begin();
bool mounted();
const char *status();

uint64_t sizeBytes();
uint64_t usedBytes();

const MapList &maps();
void scanMaps();

// Cycle the saved map path through: default -> each /maps/*.bin -> default.
// `dir` is +1 or -1. Returns the path that should now be opened (empty
// string = default search).
const char *cycleMapPath(int dir = 1);

// wifi.txt / home.txt / local.txt on the card. Does not log passwords.
void applyConfigFiles();

// Append one CSV line to the RAM buffer; poll() writes it out.
void logTraffic(const char *line);
void poll();  // flush if the buffer has settled
void flushLog();

// 1-bit PBM of the current framebuffer. False if no card or the buffer
// could not be copied. requestScreenshot() dumps on the next poll(), so
// the write never shares the bus with a panel refresh.
void requestScreenshot();
bool saveScreenshot();
const char *lastShot();
uint16_t shotCount();

}  // namespace sdcard
