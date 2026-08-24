#pragma once

#include <Adafruit_GFX.h>
#include <stdint.h>

namespace display {

// Brings up SPI and the GDEQ031T10 panel, and clears it once.
void begin();

// The GFX surface the views draw into. Only valid inside a draw callback.
Adafruit_GFX &gfx();

// Renders one frame.
//
// `sceneHash` is a fingerprint of everything `draw` will put on screen. If it
// matches the frame already showing, nothing happens and the call returns
// false -- an e-paper refresh costs ~0.7 s and a visible flash, so repainting
// an identical frame is worse than useless.
//
// Every EPD_FULL_REFRESH_EVERY frames (or after EPD_FULL_REFRESH_MAX_AGE_MS)
// the update is promoted to a full refresh to clear accumulated ghosting.
bool render(uint32_t sceneHash, void (*draw)());

// Force the next render() to do a full refresh and to repaint even if the
// scene is unchanged. Use after a view change or on the user's request.
void invalidate(bool forceFullRefresh = false);

// Put the panel into deep sleep. It keeps showing the last image.
void hibernate();

}  // namespace display
