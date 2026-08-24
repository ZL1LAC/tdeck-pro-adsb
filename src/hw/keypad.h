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
bool begin();
bool present();

// Non-blocking. Returns the character of the key pressed since the last call,
// or 0 if none. Only key-down events are reported; auto-repeat is not enabled.
char poll();

}  // namespace keypad
