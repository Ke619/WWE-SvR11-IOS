// CREATE AN ENTRANCE: the entrances mods add (move packs' pacentry lines: converted entrances and
// superstar mods' own) as a MODS group in the motion lists, in both editors, and what happens when
// a saved entrance names one whose mod is gone. docs/CAE_MOD_MOTIONS.md.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallCaeModMotions(rex::memory::Memory* memory);

// After the game fills menu list `list` with list category `category` (sub_8287DFD0, hooked in
// user_movies.cpp): one "MOD: <name>" row per mod entrance in the motion lists - each pack entrance, and each
// superstar mod's own. scene 0-4: a SELECT TEMPLATE list of that scene (sub_8287E0F0, its own categories).
void CaeModRows(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t category, int scene = -1);

// After a list lookup by value (sub_828823E0(list, value) -> row or -1) missed: a motion list
// gets a "MISSING MOD" row holding `value` when no script `value` exists (its mod removed), so the
// editor keeps the number instead of row 0. -> the row, or -1.
int CaeMissingRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value);

// A select screen's pick (sub_82732938's hook, rtwm_streak.cpp): the superstar whose entrance is edited next.
void CaeNotePick(uint32_t id);
// After a lookup found `value`: when the superstar being edited is a mod with that entrance number, its own
// "MOD: <name>" row (the number is also a game entrance's or another mod's). While the full editor loads his
// entrance (CaeLoadingEntrance), a scene part still his own - or his base's - and that scene's lighting get one
// too (added if missing). -> the row, or -1.
int CaePreferredRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value);
// Around the full editor's load of a stored entrance (sub_8287E960's hook, user_movies.cpp).
void CaeLoadingEntrance(bool on);

// An entrance number the match is about to play (sub_82756538 / 718 / 768 for match slot `idx`):
// a number without a script (its mod removed) -> the superstar's own entrance or part, else the
// game's default part; anything else unchanged. part: 0-4, -1 for the champion / multi-title ones.
uint32_t EntranceFallback(uint8_t* base, uint32_t mgr, uint32_t idx, int part, uint32_t n);

}  // namespace svr2011
