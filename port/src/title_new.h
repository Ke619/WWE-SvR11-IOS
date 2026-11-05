// WWE SmackDown vs. Raw 2011 - new titles: championships beyond the game's own (Mods\Titles\<folder>, manifest
// type=title, new=1, name=, champion=, kind=tag; docs/TITLE_MODS.md).
//
// The game's titles are a table of 14 rows (0x82EDEB50) and holders in the save (14 x 2 u16). A new title takes an
// index from 32 up, in folder order, for this run only: nothing keeps the index. The title module's functions
// (title_new.cpp) answer for those indices, and the holders live beside the save, by the mod's folder, in
// Saves\titles.txt - never in the save itself, so a removed mod takes only its holders with it. With no new
// title installed each hook is the game's own function.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// At start: reads Mods\Titles for new=1 mods.
void InstallNewTitles(rex::memory::Memory* memory);
int NewTitleCount();
// After the game loads a save (superstar_mods.cpp).
void NewTitlesLoaded();
// The holders' file written.
void NewTitlesForSave();
// Each frame: the holders written when the save file has changed (the game builds its save
// before it asks, so a save turned down at its prompt doesn't count).
void NewTitlesTick(uint8_t* base);
// Test aid SVR2011_TEST_NEW_TITLES (after a save load).
void NewTitlesTestCheck(PPCContext& ctx, uint8_t* base);

}  // namespace svr2011
