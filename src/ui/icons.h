#pragma once

#include <stdint.h>

// Plan-view aircraft silhouettes, keyed by ICAO type designator, from a table
// built by tools/build_icons.py and loaded from SD or SPIFFS.
//
// The build tool does all the geometry: the source artwork is SVG, and its
// curves are flattened and scan-converted to 1-bit bitmaps offline, so the
// firmware only ever indexes a table and calls drawBitmap(). Nothing here
// parses a path or fills a polygon.
//
// Missing data is not an error: available() stays false, draw() does nothing
// and reports it, and the detail page simply lays out without a picture.
namespace icons {

// Loads the table into PSRAM. Safe to call when no icon table is present.
bool begin();

bool available();

// Pixel size of the silhouette for a type designator, false if there is none.
// Sizes vary -- an airliner is tall and narrow, a helicopter nearly square --
// so the caller has to ask before it can lay out around one.
bool size(const char *type, int16_t *w, int16_t *h);

// Draws the silhouette with its top-left at (x, y). Returns false if the type
// has no shape, having drawn nothing.
bool draw(const char *type, int16_t x, int16_t y);

// One line for the diagnostics page, e.g. "2640 types 87k".
const char *status();

}  // namespace icons
