// Superstar mods in WWE Universe (universe_mods.cpp): Universe keeps 150 superstars in the save; a switched-on mod
// gets an entry and its rivalries in a side table instead (guest memory, saved next to the save as
// Saves\.universe\mods.bin), so the save itself stays as older builds read it.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallUniverseMods(rex::memory::Memory* memory, const std::filesystem::path& saves);
// sub_825B7660 for a mod: a "virtual place" k whose entry math (P + 16 + 24k, 32-bit) lands on the mod's side entry;
// -1 when the mod has none (the side table full, or not set up).
int64_t UniverseModPlace(uint8_t* base, uint32_t id);
// sub_825AD850 with a mod in the pair (place_a / place_b: the other id's own place, -1 none): the offset from the
// rivalry table the caller reads (`table`) to the pair's side byte, -1 for none. One byte per pair, whichever order.
// Universe's own table (P + 3616) has the mods' bytes. `working_copy`: `table` is the copy sub_825C3D70 makes on the
// stack (11325 bytes copied in at its start, back to P + 3616 at its end - the weekly relationship pass, whose
// sub_825C2BA0 / sub_825AD950 adds and team checks work on it): its mod pairs answer with the side bytes too, read
// and written there at once (the copy is always written back, so the outcome is the same). Before 2.1.5 they
// answered -1 ("no pair") there: a mod team's members looked like non-allies and the team was dissolved
// (player report 21 Oct). Any other table answers -1.
int64_t UniverseModPair(uint8_t* base, uint32_t table, uint32_t a, int64_t place_a, uint32_t b, int64_t place_b,
                        bool working_copy = false);
// Whether a mod has a Universe entry.
bool UniverseModIn(uint32_t id);
// After Universe's reset (sub_825B6C60) and after a save is loaded (sub_8257A218): the side table cleared / read
// from its file (a save without one: cleared).
void UniverseModsReset();
void UniverseModsLoaded(uint8_t* base);
// Once a frame: the game wrote SaveData.dat -> the side table written beside it (tmp + rename).
void UniverseModsTick(uint8_t* base);
// Universe's teams and stables (player report 21 Oct: they disband after a match): 70 x 270 B at save + 0xF5C8 -
// +0 u16 members, +2 five u16 superstar ids (0x200 none), +0x50 the name. About twice a second: a team made,
// changed or gone is logged with its members and what was going on (a match running or not), and the whole list
// when a save comes in.
void UniverseTeamsTick(uint8_t* base);
// Test aid SVR2011_TEST_TEAM_ENTRANCE=<team>,...: that team (0-69) for a superstar who is one of its members, else
// -1 - always -1 outside a test game (the exe in runs\svrtest\). The test match makes those slots the team.
int TestTeamOf(uint8_t* base, uint32_t id);
// ... and that team's record in the save's team table (a guest address; 0 outside a test game)
uint32_t TestTeamRecord(uint8_t* base);

}  // namespace svr2011
