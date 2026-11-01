// Superstar Threads for superstar mods (docs/SUPERSTAR_MODS.md, "Superstar Threads").
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

struct ThreadsMod {
  uint32_t slot = 0, base = 0;
  std::string folder;            // Mods\Superstars\<folder>
  std::filesystem::path ch_pac;  // its model pac
};

// At start, before the overlay device is registered: Mods\SuperstarOverlay\threads.pac with the game's
// EDIT/SSTL as EDIT/SSTM plus a record for each mod whose model is its base's (the base's record under the
// mod's id) or that has a threads.txt (its parts). False: none (no file, no alias).
bool BuildThreadsOverlay(rex::memory::Memory* memory, const std::filesystem::path& game,
                         const std::filesystem::path& overlay, const std::vector<ThreadsMod>& mods);

// The saves folder (its .threads subfolder holds the mods' slots); before InstallSuperstarMods.
void SetThreadsSaves(const std::filesystem::path& saves);

// The installed DLC packages' folder (<user data>\0000000000000000\<title>\00000002): a package's
// pac\edit\edit_dlcNN.pac has its own EDIT/SSTL (the game's list plus its superstars); before InstallSuperstarMods.
void SetThreadsDlc(const std::filesystem::path& dlc_root);

// The resolver's lookup name for "/EDIT/SSTL" (a guest string "/EDIT/SSTM"), or 0.
uint32_t ThreadsAlias(const char* name);

// Test aid SVR2011_TEST_THREADS_SLOTS=1 (when Threads opens its list): each superstar id whose profile has a
// named slot, with its profile index and the slots' names.
void LogThreadsSlots(const uint8_t* base);

// A mod's profile just set up (1056 bytes, guest memory): its three Threads slots (the last 324 bytes) are
// the ones saved for its folder (Saves\.threads\<folder>.bin), else empty - never its base's.
void ThreadsSlots(uint8_t* profile, const std::string& folder);

}  // namespace svr2011
