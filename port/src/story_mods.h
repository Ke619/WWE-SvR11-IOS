// Story Designer stories that name a superstar mod (story_mods.cpp).
#pragma once

namespace svr2011 {

// Once a frame (superstar_mods.cpp's CheckModProfiles): at the first call each story whose mods moved or are gone is
// put right; after that a story the game wrote gets its mods noted (Saves\.universe\story_mods.txt).
void StoryModsTick();

}  // namespace svr2011
