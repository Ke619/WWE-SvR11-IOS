// Superstar mods (arenas branch): new playable characters from
// <game>/Mods/Superstars/<id>/, in the free DLC slots 59-69. See
// superstar_mods.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::filesystem {
class VirtualFileSystem;
}

namespace svr2011 {

void InstallSuperstarMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* vfs);

// Attire mods that replace an attire a stock superstar's own pac has: a move
// pack each (Mods/SuperstarOverlay/attirepacks/<folder>: "pacentry
// ch\chNNN.pac EMD <entry> <file>"), merged by InstallMovePacks into an overlay
// copy of the game's pac. Called by InstallMovePacks before it lists the packs. dlc_root: the installed DLC
// packages (their pac\ch\ch<id>_dlcNN.pac: the DLC superstars' attires and DLC attires of disc superstars count
// as the superstar's own; an attire mod replacing one of those is mounted as a guest pac, LoadAttireMods).
void PrepareAttirePacks(const std::filesystem::path& game, const std::filesystem::path& dlc_root);
// Before the Custom Movies folder is mounted (its device lists the files
// once): the mods' entrance movies are copied into it.
void CopySuperstarMovies(const std::filesystem::path& movies);
// The installed mods' character ids (the select screen's EXTRA list); with_online false: without the ones
// fetched from a session's host for this run (WWE Universe, Road to WrestleMania, Mystery Opponent).
std::vector<uint32_t> SuperstarModIds(bool with_online = true);
// A mod fetched from a session's host for this run (OnlineMods): never kept in saves (superstar_threads.cpp).
bool IsOnlineTempSuperstar(uint32_t id);
// The saves folder (its SaveData.dat: what a fetched mod's id is put back to in every save built).
void SetSuperstarModsSaves(const std::filesystem::path& saves);
// The save being built (superstar_threads.cpp's sub_827235D8 hook): saving true puts back the fetched mods' ids as
// the save had them, false (after the build) the mods again.
void OnlineTempForSave(uint8_t* base, bool saving);
// Per-mod save files (per_mod_saves): around the save writer - the mods' bytes to Saves\.mods\<folder>.dat and
// their slots' stubs into the save (saving), then back.
void PerModSavesForSave(uint8_t* base, bool saving);
// After a save is loaded: the save's teams / titles / Universe cards that name a mod get its id back (refs.bin).
void PerModSavesLoaded(uint8_t* base);
bool IsSuperstarMod(uint32_t id);
// The installed mods' entrance numbers (manifest entrance= and attire<N>_entrance=, without the fetched ones): the
// CREATE AN ENTRANCE rows "MOD: <name>" (cae_mod_motions.cpp). base: the manifest's base superstar - a mod on a game
// entrance keeps its base's scene parts (10000 + 1000k + base: Jeff Hardy's are Matt Hardy's 10112...).
struct SuperstarModEntrance {
  uint32_t id;
  std::string name;
  std::vector<int> numbers;
  uint32_t base = 0;
};
std::vector<SuperstarModEntrance> SuperstarModEntrances();
// A mod's folder in Mods/Superstars ("" for no mod).
std::string SuperstarModFolder(uint32_t id);
// A mod's loading picture file (manifest loading=, else its load.dds; "" for none).
std::string SuperstarModLoading(uint32_t id);
// Before a match's people are built (sub_828BC5E8): a slot that changed hands at a save load and holds the old
// mod's saved profile again gets this mod's moves, entrance and movie back.
void SuperstarModHandovers(uint8_t* base);
// Once a frame: a mod's profile that is its slot's placeholder again (a
// save's copy) gets the mod's back.
void CheckModProfiles(uint8_t* base);
// A mod's base character id (its manifest's base=), else 0.
uint32_t SuperstarModBase(uint32_t id);
// The id of the installed mod in folder (Mods/Superstars/<folder>; not a fetched one), else 0.
uint32_t SuperstarModIdOf(const std::string& folder);
// The saves folder SetSuperstarModsSaves was given ("" before).
std::filesystem::path SuperstarModsSavesDir();
// The game's text for string id (menu_hooks.cpp's lookup): a mod's attire
// names, else 0.
uint32_t SuperstarModString(uint32_t id);
// Another pac in Mods/SuperstarOverlay to mount with the mods' (media mods);
// before InstallSuperstarMods.
void AddOverlayMount(const std::string& file);

}  // namespace svr2011
