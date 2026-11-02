// WWE SmackDown vs. Raw 2011 - managers playable: the character select's "?"
// tile opens a list of the managers and valets (as the DIVAS, CAW and DLC
// tiles open theirs) instead of the random roulette.
//
// Managers are complete characters (they wrestle a full match); only the
// select screen kept them out (roster record +221 "selectable" = 0). Each
// player's cursor (the select screen's player object) has 8 character lists
// at +8116 (2580 bytes each: count, 322 ids, 322 enable flags); the game uses
// only list 6 (cursor+5228 is always 6) and copies it to all 8. List 7 here
// holds the managers and cursor+5228 = 7 means "the managers list is open":
// - filled after every copy / rebuild of the lists (sub_8245DE00,
//   sub_82448A90);
// - A on the "?" tile (kind 4, sub_824621F8) opens it the way the DIVAS / CAW
//   / DLC tiles open theirs (list mode cursor+28916 = 1, so B and the generic
//   "a list is open" paths work), without the roulette;
// - browsing (sub_8245F1C0): every entry is valid, the highlight stays on the
//   "?" tile (2, 2);
// - B (sub_82462310) and coming back from attire select (sub_8245F0C8) keep
//   or end the managers mode.
// Picking one goes through the game's own checks (sub_82734CA0).
// Modded characters: add their ids to kManagers.

#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(managers_tile, true, "Gameplay",
                    "Character select: the \"?\" tile opens the managers (Paul Bearer, The Hurricane, ...) "
                    "instead of a random pick");

namespace {

// Stephanie McMahon, Theodore Long, Paul Bearer, Tiffany, The Hurricane
// (Hornswoggle, 195, is left out: his moves and animations are broken).
constexpr uint32_t kManagers[] = {147, 186, 251, 296, 274};

// Cursor (player object) fields.
constexpr uint32_t kScreen = 272, kPlayer = 284, kCol = 88, kRow = 72;
constexpr uint32_t kListIndex = 5228, kListMode = 28916;
constexpr uint32_t kLists = 8116, kListSize = 2580, kManagerList = 7, kMasterList = 6;
constexpr uint32_t kTileKindRandom = 4;

using svrfmt::Rd32, svrfmt::Wr32;

// Fills the list at `list` (count, ids, enable flags) with the managers.
void FillManagers(uint8_t* base, uint32_t list) {
  uint32_t n = 0;
  for (uint32_t id : kManagers) {
    Wr32(base, list + 4 + n * 4, id);
    Wr32(base, list + 4 + 1288 + n * 4, 1);
    ++n;
  }
  for (uint32_t id : svr2011::SuperstarModIds()) {  // (arenas branch: superstar mods, superstar_mods.cpp)
    Wr32(base, list + 4 + n * 4, id);
    Wr32(base, list + 4 + 1288 + n * 4, 1);
    ++n;
  }
  Wr32(base, list, n);
}

bool ManagersOpen(uint8_t* base, uint32_t cursor) { return Rd32(base, cursor + kListIndex) == kManagerList; }

// Problem reports ("the select screen got stuck"): a line when a character select sets its players up - once per
// screen, again after 15 s (the same screen object comes back for the next match).
void NoteSelectOpen(uint32_t screen) {
  static uint32_t last = 0;
  static auto at = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (screen == last && now - at < std::chrono::seconds(15)) return;
  last = screen, at = now;
  REXLOG_INFO("[svr2011] character select: open (screen {:08X})", screen);
}

}  // namespace

// The cursor's lists copied from the screen's (cursor, source).
REX_EXTERN(__imp__sub_8245DE00);
REX_HOOK_RAW(sub_8245DE00) {
  const uint32_t cursor = ctx.r3.u32;
  NoteSelectOpen(Rd32(base, cursor + kScreen));
  __imp__sub_8245DE00(ctx, base);
  if (REXCVAR_GET(managers_tile)) FillManagers(base, cursor + kLists + kManagerList * kListSize);
}

// The cursor's lists rebuilt (screen, out = cursor+8116, player).
REX_EXTERN(__imp__sub_82448A90);
REX_HOOK_RAW(sub_82448A90) {
  const uint32_t out = ctx.r4.u32;
  NoteSelectOpen(ctx.r3.u32);
  __imp__sub_82448A90(ctx, base);
  if (REXCVAR_GET(managers_tile) && out) FillManagers(base, out + kManagerList * kListSize);
}

// A on a tile (cursor): the "?" tile opens the managers list.
REX_EXTERN(__imp__sub_824621F8);
REX_HOOK_RAW(sub_824621F8) {
  const uint32_t cursor = ctx.r3.u32;
  REXLOG_INFO("[svr2011] character select: player {} A on the tile at column {} row {}{}", Rd32(base, cursor + kPlayer),
              Rd32(base, cursor + kCol), Rd32(base, cursor + kRow), Rd32(base, cursor + kListMode) ? " (a list is open)" : "");
  // (Match Creator sets the lists up without the two calls above: fill them here)
  if (REXCVAR_GET(managers_tile) && Rd32(base, cursor + kListMode) == 0 &&
      Rd32(base, cursor + kLists + kManagerList * kListSize) == 0)
    FillManagers(base, cursor + kLists + kManagerList * kListSize);
  if (!REXCVAR_GET(managers_tile) || Rd32(base, cursor + kListMode) != 0 ||
      Rd32(base, cursor + kLists + kManagerList * kListSize) == 0) {
    __imp__sub_824621F8(ctx, base);
    return;
  }
  const auto saved = ctx;
  const uint32_t screen = Rd32(base, cursor + kScreen);
  ctx.r3.u64 = screen;
  sub_82449A58(ctx, base);
  const bool grid = ctx.r3.u32 == 1;
  ctx.r3.u64 = screen;
  ctx.r4.u64 = Rd32(base, cursor + kCol);
  ctx.r5.u64 = Rd32(base, cursor + kRow);
  sub_8244C710(ctx, base);
  const bool ours = grid && ctx.r3.u32 == kTileKindRandom;
  if (!grid && ctx.r3.u32 == kTileKindRandom)
    REXLOG_INFO("[svr2011] managers: \"?\" tile left to the game (screen {:08X} +42240 = {})", screen,
                Rd32(base, screen + 42240));
  if (!ours) {
    ctx = saved;
    __imp__sub_824621F8(ctx, base);
    return;
  }
  // (as the DIVAS / CAW / DLC tiles: sub_824621F8's own sequence)
  Wr32(base, cursor + kListIndex, kManagerList);
  Wr32(base, cursor + kListMode, 1);
  ctx.r3.u64 = screen;
  ctx.r4.u64 = Rd32(base, cursor + kPlayer);
  sub_8244FF78(ctx, base);
  ctx.r3.u64 = cursor;
  sub_8245E2C8(ctx, base);
  const uint32_t index = ctx.r3.u32;
  ctx.r3.u64 = cursor;
  ctx.r4.u64 = index;
  sub_8245FFA0(ctx, base);
  ctx.r3.u64 = cursor;
  sub_8245F530(ctx, base);
  ctx.r3.u64 = cursor + 44;
  const uint32_t vtable = Rd32(base, cursor + 44);
  ctx.ctr.u64 = Rd32(base, vtable + 80);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  ctx.r3.u64 = cursor;
  ctx.r4.u64 = 0;
  sub_82462118(ctx, base);  // (the panel shows the first manager)
  REXLOG_INFO("[svr2011] managers list opened (player {}, cursor {:08X})", Rd32(base, cursor + kPlayer), cursor);
  ctx = saved;
  ctx.r3.u64 = 0;
}

// The list entry under the cursor (cursor): in the managers list every entry
// is valid, the highlight stays on the "?" tile.
REX_EXTERN(__imp__sub_8245F1C0);
REX_HOOK_RAW(sub_8245F1C0) {
  const uint32_t cursor = ctx.r3.u32;
  const uint32_t mode = Rd32(base, cursor + kListMode);
  if (!ManagersOpen(base, cursor) || mode < 1 || mode > 3) {
    __imp__sub_8245F1C0(ctx, base);
    return;
  }
  const uint32_t count = Rd32(base, cursor + kLists + kManagerList * kListSize);
  if (count == 0) {
    ctx.r3.u64 = 3;
    return;
  }
  int32_t index = int32_t(Rd32(base, cursor + kRow));
  index = ((index % int32_t(count)) + int32_t(count)) % int32_t(count);
  Wr32(base, cursor + kRow, uint32_t(index));
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 128;  // (a frame for the out values)
  const uint32_t col = ctx.r1.u32 + 80, row = ctx.r1.u32 + 84;
  Wr32(base, col, 2);
  Wr32(base, row, 2);
  ctx.r3.u64 = Rd32(base, cursor + kScreen);
  ctx.r4.u64 = Rd32(base, cursor + kPlayer);
  ctx.r5.u64 = col;
  ctx.r6.u64 = row;
  sub_82450090(ctx, base);
  ctx = saved;
  ctx.r3.u64 = 1;
}

// B in a list (cursor): leaving the managers list goes back to the grid.
REX_EXTERN(__imp__sub_82462310);
REX_HOOK_RAW(sub_82462310) {
  const uint32_t cursor = ctx.r3.u32;
  const uint32_t mode = Rd32(base, cursor + kListMode);
  if (ManagersOpen(base, cursor) && mode >= 1 && mode <= 3) {
    Wr32(base, cursor + kListIndex, kMasterList);
    Wr32(base, cursor + kRow, 0);
  }
  __imp__sub_82462310(ctx, base);
}

// The list mode worked out again from the picked character (cursor): with the
// managers list open it stays a list.
REX_EXTERN(__imp__sub_8245F0C8);
REX_HOOK_RAW(sub_8245F0C8) {
  const uint32_t cursor = ctx.r3.u32;
  __imp__sub_8245F0C8(ctx, base);
  if (ManagersOpen(base, cursor)) Wr32(base, cursor + kListMode, 1);
}
