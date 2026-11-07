// WWE SmackDown vs. Raw 2011 - Road to WrestleMania's Streak story played as a superstar mod (rtwm_streak.cpp).
#pragma once

#include <cstdint>

namespace svr2011 {

// The main menu's last choice was Road to WrestleMania (menu_hooks.cpp).
bool InRoadToWrestleMania();
// A list block was built (sort_filters.cpp): `on` - of Create-a-Superstar slots (the Streak's), its first CAW what the
// story gets for a mod; off - a roster list (picks there aren't the Streak's).
void RtwmCasList(uint32_t first_caw, bool on);
// A match's people are built (test_match.cpp's hook): in Road to WrestleMania, the CAW the mod stands in for becomes
// the mod.
void RtwmStreakPeople(uint8_t* base, uint32_t match);
// Each world frame, before the game's update (frame_rate.cpp): test aids for Road to WrestleMania's scenes -
// SVR2011_TEST_POS_LOG=1 (fighter 0's place every second) and SVR2011_TEST_RTWM_WARP=x,z[,y] (fighter 0 - the player
// in a backstage walk - put there 5 seconds into a scene with the player alone: a walk's objective reached
// without walking, for hidden tests).
void RtwmBeforeFrame(uint8_t* base);
// Around a select panel's update: with a mod under the cursor in the Streak's list, the player's pick (the CAW slot
// the story gets) reads as the mod while the panel looks - true if swapped; End puts the CAW slot back.
bool RtwmShowModBegin(uint8_t* base);
void RtwmShowModEnd(uint8_t* base, bool swapped);

}  // namespace svr2011
