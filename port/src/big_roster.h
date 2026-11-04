// Big roster (cvar big_roster, experimental, off by default): the game's tables with one entry per
// record index (242) grow past index 241 - see big_roster.cpp and docs/UNLIMITED_MOD_SLOTS.md.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

constexpr uint32_t kStockIndices = 242, kMaxIndices = 512, kMaxIds = 512;

void InstallBigRoster(rex::memory::Memory* memory);
bool BigRosterOn();
// The guest address of entry index of the table at start (stride bytes an entry): past index 241 the table's
// extension (the same one the game's code is sent to).
uint32_t RosterEntry(uint32_t start, uint32_t stride, uint32_t index);
// Test aid SVR2011_TEST_HIGH_INDEX=<n> (big_roster on): the first n of these ids (the mods) get record indices from
// 242 up, their old index left unused - the extensions without new ids. Before the game reads its tables.
void BigRosterTestMove(uint8_t* base, const uint32_t* ids, int count);
// Test aid SVR2011_TEST_UNIVERSE_CARD=<id>:<id>,... (after a save load): the Universe cards' people with the first
// id get the second (a moved mod on a card without simulating weeks).
void BigRosterTestCard(uint8_t* base);

}  // namespace svr2011
