#pragma once

#include <stdint.h>

// Vector basemap drawn under the radar plot.
//
// Named "basemap" rather than "map" so nothing collides with Arduino's global
// map() or with std::map.
//
// The data is built offline by tools/build_map.py and flashed into the SPIFFS
// partition as /map.bin: simplified polylines at three detail levels, chosen by
// the plotted range. Vector rather than raster tiles because the panel is
// 1-bit -- dithered imagery turns to mud under the aircraft markers -- and
// because every frame redraws from scratch, so per-frame decode cost would be
// paid over and over.
//
// Drawing costs no extra e-paper refreshes: the map is a pure function of the
// plot centre and the range, both of which already feed the scene hash.
namespace basemap {

// Mounts SPIFFS and reads the file header. Safe to call when no map is
// flashed; available() then stays false and draw() does nothing.
bool begin();

bool available();

// One line for the diagnostics page, e.g. "L1 131f 13.6k" or "no /map.bin".
const char *status();

// Draws the basemap for a view centred on (centreLat, centreLon), clipped to
// the circle at (cx, cy) of the given radius. `rangeNm` picks the detail level.
void draw(double centreLat, double centreLon, float pixelsPerNm, int16_t cx,
          int16_t cy, int16_t radius, uint16_t rangeNm);

}  // namespace basemap
