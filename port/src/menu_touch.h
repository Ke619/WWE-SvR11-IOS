// WWE SmackDown vs. Raw 2011 - tapping the game's own menus directly.
#pragma once

#include <cstdint>

namespace svr2011 {

// How the caller presses a pad button for a moment (XInput button bits): each platform's touch input has its own.
using MenuTouchPress = void (*)(uint16_t buttons);

// A tap at (x, y), fractions of the game's 16:9 picture (0..1; not of the screen, which may have bars). Any thread.
// True: it was on a row of the current menu - the cursor is there and A pressed. False: not handled, nothing changed.
bool MenuTouchTap(float x, float y, MenuTouchPress press);

}  // namespace svr2011

#include <rex/ppc.h>

namespace svr2011 {

// (pad_icons' hook on a character select panel; the guest's thread)
void MenuTouchPanel(PPCContext& ctx, uint8_t* base, uint32_t cursor);

}  // namespace svr2011
