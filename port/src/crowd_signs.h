// Crowd signs (arenas branch): new signs for the crowd to hold. See
// crowd_signs.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// A character's own signs (up to 4 DDS files): sign ids first + 1..4 (a superstar mod's: 10000 + its key * 10,
// superstar_mods.cpp). Call before InstallCrowdSigns.
// A superstar mod's own crowd signs are ModSignBase(its key, 400-999) + 1..4.
uint32_t ModSignBase(uint32_t key);

void AddCharacterSigns(uint32_t first, const std::vector<std::filesystem::path>& dds);

// Reads the sign packs (<game>/Mods/Signs/<pack>/*.dds) and builds the match
// sign bank. After InstallSuperstarMods.
void InstallCrowdSigns(rex::memory::Memory* memory);

}  // namespace svr2011
