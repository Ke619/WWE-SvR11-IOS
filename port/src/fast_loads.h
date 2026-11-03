// WWE SmackDown vs. Raw 2011 - faster loading screens (fast_loads.cpp): cvar fast_loads (off by default) - while
// a static loading or info screen is up, the world runs several updates a frame, so the game's frame-counted waits
// there end sooner (their animations play faster). Now: the Start Screen after its START (Loading saved game, Load
// Successful, Checking Created Superstar save data, NOW LOADING - 12 s at the usual speed). Never in menus,
// matches, entrances, cutscenes, online or lockstep (frame_rate.cpp asks only outside a match and lockstep).
// The PC launcher's Play tab and the Android app's settings set it.

#pragma once

#include <cstdint>

namespace svr2011 {

// Once a frame, before the update passes (frame_rate.cpp's task manager hook): the updates this frame should
// take (0: the usual).
int FastLoadsTicks(uint8_t* base);

// The Start Screen's frame (pad_types.cpp's sub_8258B5F8 hook); start_taken: its START was taken this frame.
void FastLoadsStartScreen(bool start_taken);

}  // namespace svr2011
