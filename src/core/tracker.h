#pragma once

#include <stddef.h>

#include "aircraft.h"

// Fixed-capacity store of the aircraft we currently know about.
//
// The feed gives us a full snapshot every poll, but targets drop in and out of
// coverage between polls. Rather than replacing the set wholesale we merge each
// snapshot in and age entries out after AIRCRAFT_STALE_MS, so a target that
// blinks for one poll does not flicker off the screen.
class Tracker {
  public:
    void clear();

    // Merge one aircraft from a feed snapshot. Matching is by ICAO hex.
    // Returns false only if the store is full and the target is unknown.
    bool upsert(const Aircraft &incoming, uint32_t nowMs);

    // Drop entries older than AIRCRAFT_STALE_MS, recompute range/bearing
    // against the given centre, and sort nearest-first.
    void finishUpdate(double centreLat, double centreLon, uint32_t nowMs);

    size_t count() const { return count_; }
    const Aircraft &at(size_t i) const { return items_[i]; }

    // Index into the sorted array, or -1.
    int indexOfHex(const char *hex) const;

    // How many of the tracked aircraft actually reported a position. Feed
    // entries without one still carry callsign and altitude, so they show in
    // the list but cannot be plotted.
    size_t positionCount() const { return positionCount_; }

    // Cheap fingerprint of everything the UI draws, so we can skip an e-paper
    // refresh when nothing has actually moved.
    uint32_t sceneHash() const;

  private:
    Aircraft items_[MAX_AIRCRAFT];
    size_t count_ = 0;
    size_t positionCount_ = 0;

    int findSlot(const char *hex) const;
    void sortByDistance();
};
