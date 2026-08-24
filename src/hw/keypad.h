#pragma once

#include <stdint.h>

namespace keypad {

// Special characters produced by the T-Deck Pro matrix. The letter/digit keys
// return their obvious ASCII character; these five positions are modifier or
// action keys and the vendor keymap gives them stand-in characters.
//
// The mapping below matches LilyGO's factory firmware. If a key on your unit
// reports something unexpected, watch the serial log -- every press is printed
// as `keypad: '<c>' (r,c)` -- and adjust here.
constexpr char kEnter = 'E';      // return key
constexpr char kBackspace = 'U';  // delete key
constexpr char kShift = 'S';
constexpr char kSymbol = '$';
constexpr char kAlt = '2';
constexpr char kMic = '*';
constexpr char kSpace = ' ';

// Returns false if the TCA8418 did not answer on the I2C bus, in which case
// poll() will always report "no key" and the UI falls back to touch only.
// The most recent key event, shown on the diagnostics page.
//
// This keymap is transcribed from LilyGO's factory example and the board it
// was transcribed for may not be the board you have, so the firmware has to be
// able to tell you what a key actually reported rather than leaving you to
// guess. `row` and `col` are -1 when the decode rejected the event outright.
struct LastKey {
    char c;
    int8_t row;
    int8_t col;
    int16_t raw;
};
const LastKey &lastKey();

bool begin();
bool present();

// Non-blocking. Returns the character of the key pressed since the last call,
// or 0 if none. Only key-down events are reported; auto-repeat is not enabled.
char poll();

}  // namespace keypad
