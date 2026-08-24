#pragma once

#include <stdint.h>

// Small persistent slice of user state, kept in NVS.
//
// Only choices the user made deliberately and would be irritated to make again
// after a reboot: the range they were on, whether the basemap was up, which
// feed they picked. Not the pan offset or the selection -- those are
// moment-to-moment, and persisting them would restore a view of traffic that
// has long since flown away.
struct Settings {
    uint8_t rangeIndex;
    uint8_t provider;  // AdsbProvider, as a byte so the struct stays POD
    bool mapEnabled;
    bool centreOnGnss;
    bool keypadBacklight;
};

namespace settings {

// Loads the saved set, falling back to the config.h defaults when nothing has
// been stored yet or when the stored blob does not match this build's layout.
// Call before anything reads a setting.
void begin();

// The live values. Mutate through here, then call markDirty().
Settings &get();

// Note that something changed. The write itself is deferred -- see poll().
void markDirty();

// Flushes a dirty set once it has stopped changing. NVS wears with every
// write and a held-down zoom key can step the range a dozen times in a
// second, so the write waits for the user to settle and is skipped entirely
// when the bytes match what is already stored.
void poll();

// One line for the diagnostics page.
const char *status();

}  // namespace settings
