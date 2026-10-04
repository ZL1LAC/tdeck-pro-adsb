#include "tracker.h"

#include <math.h>
#include <string.h>

#include "filter.h"
#include "geo.h"

namespace {
// Copy src into dst only when src carries information, so a snapshot that
// omits a field does not wipe what we already learned about the target.
void mergeString(char *dst, size_t dstLen, const char *src) {
    if (!src || !src[0]) return;
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}

void appendTrailPoint(Aircraft &a, double lat, double lon) {
    if (a.trailCount > 0) {
        const uint8_t last =
            static_cast<uint8_t>((a.trailNext + AIRCRAFT_TRAIL_POINTS - 1) %
                                 AIRCRAFT_TRAIL_POINTS);
        const Aircraft::TrailPoint &p = a.trail[last];
        if (geo::distanceNm(p.lat, p.lon, lat, lon) < AIRCRAFT_TRAIL_MIN_NM) {
            return;
        }
    }

    a.trail[a.trailNext].lat = lat;
    a.trail[a.trailNext].lon = lon;
    a.trailNext = static_cast<uint8_t>((a.trailNext + 1) % AIRCRAFT_TRAIL_POINTS);
    if (a.trailCount < AIRCRAFT_TRAIL_POINTS) ++a.trailCount;
}
}  // namespace

void Tracker::clear() {
    count_ = 0;
    positionCount_ = 0;
}

void Tracker::setCentre(double lat, double lon) {
    centreLat_ = lat;
    centreLon_ = lon;
    haveCentre_ = true;
}

int Tracker::findSlot(const char *hex) const {
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(items_[i].hex, hex) == 0) return static_cast<int>(i);
    }
    return -1;
}

// The store is full and this is not a target we already hold. A snapshot
// arrives in whatever order the feed serialises it, so keeping the first
// MAX_AIRCRAFT of it keeps an arbitrary subset -- and the one thing a radar
// must not do is discard the traffic nearest you in favour of something at the
// far edge of the query. So the slot is given away only to a nearer target.
//
// Entries with no position are parked at 1e9 nm, so anything that reported one
// outbids them. That is the right order: a target that cannot be plotted is
// the cheapest one to lose.
int Tracker::evictFor(const Aircraft &incoming) const {
    if (!haveCentre_ || !incoming.hasPosition || count_ == 0) return -1;

    const float want = static_cast<float>(
        geo::distanceNm(centreLat_, centreLon_, incoming.lat, incoming.lon));

    size_t worst = 0;
    float worstNm = items_[0].distanceNm;
    for (size_t i = 1; i < count_; ++i) {
        if (items_[i].distanceNm > worstNm) {
            worstNm = items_[i].distanceNm;
            worst = i;
        }
    }
    if (worstNm <= want) return -1;
    return static_cast<int>(worst);
}

bool Tracker::upsert(const Aircraft &incoming, uint32_t nowMs) {
    if (!incoming.hex[0]) return true;  // nothing usable, but not an overflow

    int slot = findSlot(incoming.hex);
    if (slot < 0) {
        if (count_ >= MAX_AIRCRAFT) {
            slot = evictFor(incoming);
            if (slot < 0) return false;
        } else {
            slot = static_cast<int>(count_++);
        }
        items_[slot] = incoming;
        items_[slot].lastUpdateMs = nowMs;
        items_[slot].trailCount = 0;
        items_[slot].trailNext = 0;
        if (incoming.hasPosition) {
            appendTrailPoint(items_[slot], incoming.lat, incoming.lon);
        }
        // finishUpdate() fills this in for everything at the end of the poll,
        // but the rest of this same snapshot has to be able to outbid the
        // entry before then -- and the zero Aircraft{} leaves here would read
        // as "directly overhead", making a new arrival unevictable.
        items_[slot].distanceNm =
            (haveCentre_ && incoming.hasPosition)
                ? static_cast<float>(geo::distanceNm(centreLat_, centreLon_,
                                                     incoming.lat, incoming.lon))
                : 1.0e9f;
        return true;
    }

    Aircraft &dst = items_[slot];
    mergeString(dst.flight, sizeof(dst.flight), incoming.flight);
    mergeString(dst.reg, sizeof(dst.reg), incoming.reg);
    mergeString(dst.type, sizeof(dst.type), incoming.type);
    mergeString(dst.squawk, sizeof(dst.squawk), incoming.squawk);

    if (incoming.hasPosition) {
        appendTrailPoint(dst, incoming.lat, incoming.lon);
        dst.lat = incoming.lat;
        dst.lon = incoming.lon;
        dst.hasPosition = true;
        dst.seenPosSec = incoming.seenPosSec;
    }
    if (incoming.altitudeFt != Aircraft::kAltitudeUnknown) {
        dst.altitudeFt = incoming.altitudeFt;
    }
    if (incoming.hasTrack) {
        dst.trackDeg = incoming.trackDeg;
        dst.hasTrack = true;
    }
    if (incoming.groundSpeedKt > 0.0f) dst.groundSpeedKt = incoming.groundSpeedKt;
    dst.verticalRateFpm = incoming.verticalRateFpm;
    dst.emergency = incoming.emergency;
    dst.lastUpdateMs = nowMs;
    return true;
}

void Tracker::finishUpdate(double centreLat, double centreLon, uint32_t nowMs,
                           const char *keepHex) {
    centreLat_ = centreLat;
    centreLon_ = centreLon;
    haveCentre_ = true;

    // Age out stale targets, and drop ones the current filter does not want,
    // by compacting the array in place. The selected aircraft is kept even
    // when it would now fail, so cycling the filter cannot yank the target
    // you were looking at out from under you.
    size_t write = 0;
    for (size_t read = 0; read < count_; ++read) {
        if (nowMs - items_[read].lastUpdateMs > AIRCRAFT_STALE_MS) continue;
        const bool keepSelected =
            keepHex && keepHex[0] && strcmp(items_[read].hex, keepHex) == 0;
        if (!keepSelected && !filter::accept(items_[read])) continue;
        if (write != read) items_[write] = items_[read];
        ++write;
    }
    count_ = write;

    positionCount_ = 0;
    for (size_t i = 0; i < count_; ++i) {
        Aircraft &a = items_[i];
        if (a.hasPosition) {
            a.distanceNm = static_cast<float>(
                geo::distanceNm(centreLat, centreLon, a.lat, a.lon));
            a.bearingDeg = static_cast<float>(
                geo::bearingDeg(centreLat, centreLon, a.lat, a.lon));
            ++positionCount_;
        } else {
            // Sorts to the end without needing a separate comparator branch.
            a.distanceNm = 1.0e9f;
            a.bearingDeg = 0.0f;
        }
    }

    sortByDistance();
}

void Tracker::sortByDistance() {
    // Insertion sort: the list is small and nearly sorted between polls.
    for (size_t i = 1; i < count_; ++i) {
        Aircraft key = items_[i];
        size_t j = i;
        while (j > 0 && items_[j - 1].distanceNm > key.distanceNm) {
            items_[j] = items_[j - 1];
            --j;
        }
        items_[j] = key;
    }
}

int Tracker::indexOfHex(const char *hex) const {
    if (!hex || !hex[0]) return -1;
    return findSlot(hex);
}

uint32_t Tracker::sceneHash(double centreLat, double centreLon,
                            float pixelsPerNm) const {
    // Hoisted out of the loop: geo::projectNm() takes a cos() in double for
    // every call it is given, and this used to ask for one per aircraft, twice
    // a second, for a centre that changes at walking pace. Same reason the
    // basemap projects through one of these rather than several thousand.
    const geo::Projector projector(centreLat, centreLon);

    // FNV-1a per aircraft, summed rather than chained so the result does not
    // depend on the order the array happens to be sorted into.
    uint32_t acc = 0;
    for (size_t i = 0; i < count_; ++i) {
        const Aircraft &a = items_[i];

        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t v) {
            for (int b = 0; b < 4; ++b) {
                h ^= (v >> (b * 8)) & 0xFF;
                h *= 16777619u;
            }
        };
        auto mixStr = [&h](const char *p) {
            for (; *p; ++p) {
                h ^= static_cast<uint8_t>(*p);
                h *= 16777619u;
            }
        };

        mixStr(a.hex);
        mixStr(a.label());

        if (a.hasPosition) {
            if (pixelsPerNm > 0.0f) {
                // Exactly the projection the renderer uses, rounded to the
                // pixel it would land on.
                float east = 0.0f, north = 0.0f;
                projector.project(a.lat, a.lon, &east, &north);
                mix(static_cast<uint32_t>(lroundf(east * pixelsPerNm)));
                mix(static_cast<uint32_t>(lroundf(north * pixelsPerNm)));
            } else {
                // The list prints tenths of a mile and whole degrees.
                mix(static_cast<uint32_t>(
                    static_cast<int32_t>(a.distanceNm * 10.0f)));
                mix(static_cast<uint32_t>(
                    static_cast<int32_t>(a.bearingDeg)));
            }
        }

        mix(static_cast<uint32_t>(a.altitudeKnown() ? a.altitudeFt / 100 : -1));
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.trackDeg / 5.0f)));
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.groundSpeedKt / 5.0f)));

        acc += h;
    }

    // Folded in last so that losing one target and gaining another whose
    // hashes happen to sum the same still counts as a change.
    uint32_t out = 2166136261u;
    for (int b = 0; b < 4; ++b) {
        out ^= (acc >> (b * 8)) & 0xFF;
        out *= 16777619u;
        out ^= (static_cast<uint32_t>(count_) >> (b * 8)) & 0xFF;
        out *= 16777619u;
    }
    return out;
}
