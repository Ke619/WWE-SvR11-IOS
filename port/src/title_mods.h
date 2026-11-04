// WWE SmackDown vs. Raw 2011 - title mods: a championship's name, belt and pictures replaced
// (Mods\Titles\<folder>, docs/TITLE_MODS.md).
//
// A title mod's manifest.txt: type=title, title=<key> (IC, US, WWE, WHC, WWE_TAG, WORLD_TAG, ECW, DIVAS,
// WOMENS, HARDCORE, CRUISER, MILLION, WCW), name=<the title's new name>. Its parts:
//  - the name: the game's string lookup (menu_hooks.cpp) asks TitleModString for every string; the ids that
//    name a title (kTitleStrings: found by text in the English string.pac, the same ids in every language)
//    get the mod's name - a whole name in the case the game wrote it, a phrase ("... CHAMPIONSHIP
//    OPPORTUNITY") with the title's own name in it swapped, in whatever language that name is;
//  - textures\: files named as in texture packs (<name>_<hash16>.dds / .png) - the belts' textures and the
//    menu pictures (menuHD.pac CMSM belt_<title> ...): texture_packs.cpp reads TitleModTextureFolders;
//  - pack.txt: pacentry lines (move_packs.cpp) - whole belt models, pac\evt\evtobj.pac SOBJ/<id>.
// Two mods on one title: the first by folder name wins (the Mod Maker's doctor warns). Cosmetic: not in an
// online lobby's mod set. Off with the launcher's switch (a "disabled" file) or Start Without Mods.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <rex/ppc.h>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// At start (before the game's strings are asked for): reads Mods\Titles.
void InstallTitleMods(rex::memory::Memory* memory);
// The string lookup (sub_82153EF8: r3 manager, r4 id): a title mod's text for that id, or 0 (the game's).
uint32_t TitleModString(PPCContext& ctx, uint8_t* base);
// The enabled title mods' textures\ folders, in the order they win (texture_packs.cpp).
std::vector<std::filesystem::path> TitleModTextureFolders();

}  // namespace svr2011
