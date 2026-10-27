// WWE SmackDown vs. Raw 2011 - CAW parts from mods (docs/CAW_PARTS.md): new items in Create A Superstar's part
// lists (glasses first), from Mods/CawParts/<id>/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Builds Mods/SuperstarOverlay/cawparts.pac (the parts, the categories' MENU lists and thumbnails with the mods'
// records appended) and has it mounted with the superstar mods' files. Before InstallSuperstarMods.
void InstallCawParts(rex::memory::Memory* memory);
// The file name resolver's name (sub_826B87B0): "/EDIT/MENU" and a changed category's "/CAST/00nn" asked for by
// the overlay's names; 0 = as it is.
uint32_t CawPartsAlias(const char* name);
// A record of a category that a mod part has (or had: a removed mod's record falls back to a stock part).
// category -> the first such record index; -1: none.
int CawPartsFirstRecord(int category);
// The mod parts listed this run: "<mod id>: <category> <record>..." (problem reports, tests).
std::vector<std::string> CawPartsListing();

}  // namespace svr2011
