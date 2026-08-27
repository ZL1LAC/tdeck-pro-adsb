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
    //
    // Returns false only when the store is full, the target is unknown, and it
    // is no nearer than the farthest one already held -- see evictFor().
    bool upsert(const Aircraft &incoming, uint32_t nowMs);

    // Drop entries older than AIRCRAFT_STALE_MS, recompute range/bearing
    // against the given centre, and sort nearest-first.
    void finishUpdate(double centreLat, double centreLon, uint32_t nowMs);

    // Where upsert() should measure from when it has to choose which of two
    // targets to keep. finishUpdate() sets this as a side effect; a store that
    // is filled before its first finishUpdate() -- the feed's staging copy --
    // has to be told up front.
    void setCentre(double lat, double lon);

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
    //
    // `pixelsPerNm` above zero quantises positions to plot pixels about
    // (centreLat, centreLon) -- the question the radar actually asks is "has
    // anything moved a pixel", and at 250 nm one pixel is 2.3 nm. Quantising
    // in degrees instead, as this used to, meant a target could churn seventy
    // buckets without moving anywhere the panel could show, which cost a
    // 651 ms refresh every time. Pass 0 for the list and detail views, where
    // the readouts are numeric and get the resolution they are printed at.
    //
    // Aircraft are combined commutatively, so two targets swapping places in
    // the distance sort -- which GNSS jitter alone can do when they are
    // near-equidistant -- is not by itself a reason to repaint.
    uint32_t sceneHash(double centreLat, double centreLon,
                       float pixelsPerNm) const;

  private:
    Aircraft items_[MAX_AIRCRAFT];
    size_t count_ = 0;
    size_t positionCount_ = 0;

    // Where ranges were last measured from, remembered so upsert() can work
    // out which of two targets is the nearer one without being told again.
    double centreLat_ = 0.0;
    double centreLon_ = 0.0;
    bool haveCentre_ = false;

    int findSlot(const char *hex) const;
    // Which slot a target should take when the store is full, or -1 to refuse.
    int evictFor(const Aircraft &incoming) const;
    void sortByDistance();
};
