#include "tracker.h"

#include <string.h>

#include "geo.h"

namespace {
// Copy src into dst only when src carries information, so a snapshot that
// omits a field does not wipe what we already learned about the target.
void mergeString(char *dst, size_t dstLen, const char *src) {
    if (!src || !src[0]) return;
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}
}  // namespace

void Tracker::clear() {
    count_ = 0;
    positionCount_ = 0;
}

int Tracker::findSlot(const char *hex) const {
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(items_[i].hex, hex) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool Tracker::upsert(const Aircraft &incoming, uint32_t nowMs) {
    if (!incoming.hex[0]) return true;  // nothing usable, but not an overflow

    int slot = findSlot(incoming.hex);
    if (slot < 0) {
        if (count_ >= MAX_AIRCRAFT) return false;
        slot = static_cast<int>(count_++);
        items_[slot] = incoming;
        items_[slot].lastUpdateMs = nowMs;
        return true;
    }

    Aircraft &dst = items_[slot];
    mergeString(dst.flight, sizeof(dst.flight), incoming.flight);
    mergeString(dst.reg, sizeof(dst.reg), incoming.reg);
    mergeString(dst.type, sizeof(dst.type), incoming.type);
    mergeString(dst.squawk, sizeof(dst.squawk), incoming.squawk);

    if (incoming.hasPosition) {
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

void Tracker::finishUpdate(double centreLat, double centreLon, uint32_t nowMs) {
    // Age out stale targets by compacting the array in place.
    size_t write = 0;
    for (size_t read = 0; read < count_; ++read) {
        if (nowMs - items_[read].lastUpdateMs > AIRCRAFT_STALE_MS) continue;
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

uint32_t Tracker::sceneHash() const {
    // FNV-1a over the fields the renderer actually shows. Positions are
    // quantised to ~0.001 deg (about 60 m) and altitude to 100 ft so that
    // GPS jitter alone does not trigger an e-paper refresh.
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 16777619u;
        }
    };

    mix(static_cast<uint32_t>(count_));
    for (size_t i = 0; i < count_; ++i) {
        const Aircraft &a = items_[i];
        for (const char *p = a.hex; *p; ++p) {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
        for (const char *p = a.label(); *p; ++p) {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.lat * 1000.0)));
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.lon * 1000.0)));
        mix(static_cast<uint32_t>(a.altitudeKnown() ? a.altitudeFt / 100 : -1));
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.trackDeg / 5.0f)));
        mix(static_cast<uint32_t>(static_cast<int32_t>(a.groundSpeedKt / 5.0f)));
    }
    return h;
}
