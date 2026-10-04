#pragma once

// Recursive mutex around the shared SPI bus (e-paper, SD, parked LoRa).
//
// Taking the lock also raises every CS line, matching LilyGO's factory
// protocol: an inactive device left selected on this bus can corrupt the
// e-paper transfer and leave render() waiting on BUSY forever. Recursive so
// a draw callback that loads a map level can nest under the renderer.

namespace spibus {

void begin();

class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

}  // namespace spibus
