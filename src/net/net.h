#pragma once

#include <stdint.h>

namespace net {

enum class State : uint8_t { Idle, Connecting, Connected, Failed };

void begin();

// Drives connection and reconnection. Call from the main loop; never blocks
// for more than a few milliseconds.
void poll();

State state();
bool connected();
const char *ssid();
int rssi();

}  // namespace net
