#pragma once

#include <stdint.h>

namespace touch {

// Press and release of the first finger. Movement in between is not reported:
// the panel needs ~0.7 s per refresh, so live drag-tracking is pointless. The
// UI compares the down and up positions instead and repaints once.
struct Event {
    enum class Kind : uint8_t { Down, Up };
    Kind kind;
    int16_t x;
    int16_t y;
};

// CST328 capacitive controller. Returns false if the panel does not answer.
bool begin();
bool present();

// Non-blocking, at most one event per call. Coordinates are in display space
// (0..239 x, 0..319 y) matching the e-paper's rotation 0.
bool poll(Event *out);

}  // namespace touch
