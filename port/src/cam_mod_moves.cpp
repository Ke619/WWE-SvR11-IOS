// CREATE A MOVE-SET: the move packs' own moves as a MOD group, and a moveset naming a move whose
// pack is gone. docs/CAM_MOD_MOVES.md.
//
// The game's move table (*(0x82E3C214)): +60 the MOVS/WAZE records (160 bytes, big-endian after
// the load: +0 / +8 category bits, +0x10 the name, +0x78 the unlock key, +0x90 the move id), +72 +
// cat * 4 the category lists (u16 record indices, counts at +584 + cat * 2, built from the bits that
// move_packs.cpp's PatchWaze gives the packs' moves: svr2011::PackMove), +840 + list * 4 the
// 26 lists CREATE A MOVE-SET's tabs show (counts at +944 + list * 2), filled by sub_82378A68 in
// record order. A moveset keeps a u16 move id per slot (profile + 2 * slot, CAWs and superstars alike).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "cam_mod_moves.h"
#include "crowd_fight.h"
#include "match_diag.h"
#include "move_packs.h"
#include "online_mods.h"
#include "p2p.h"
#include "text_guard.h"

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr uint32_t kMoveTable = 0x82E3C214;
constexpr uint32_t kRecordSize = 160;

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr16;

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;
std::map<uint16_t, uint32_t> g_labels;  // move id -> guest "MOD: <name>" (lives for the whole run)

bool IsModRecord(uint8_t* base, uint32_t records, uint16_t index) {
  return svr2011::PackMove(Rd16(base, records + index * kRecordSize + 0x90));
}

constexpr uint8_t kSlotCategory[224] = {  // (sub_8237A430: 128 = not a CAM slot)
    128, 128, 128, 103, 107, 104, 106, 105, 108,   0,   0,   0,   0, 128, 128,   2,
      2,   3,   3,   4,   4,   5,   5, 128, 128, 128, 128, 128, 128, 128, 128,   6,
     11,   7,  12,   8,   8,   9,  10,  13,  13, 128,  13, 128,  13,  13,  14,  14,
     15,  16,  16,  96,  97, 128, 128, 128, 128, 128, 128, 128, 128,  26,  26,  26,
     26,  28,  30,  30,  30,  30,  20,  32,  32,  32,  32,  19,  35,  35,  35,  35,
     17,  33,  33,  33,  33,  18,  34,  34,  34,  34, 128, 128, 128, 128,  27,  27,
     27,  27, 128,  29, 128, 128,  38, 128,  36, 117, 128, 128,  40, 128,  36, 119,
    128, 128,  39, 128, 128, 118, 128, 128,  41, 128, 128,  43, 128, 128,  42,  67,
    128, 128, 128,  50, 128,  50,  46, 128, 128, 128,  48, 128, 128, 128, 128,  51,
    128,  51,  47,  49, 128,  54,  52,  53, 128, 128, 128,  62,  60,  61,  64,  63,
     58, 128,  59, 128,  55, 128,  56,  57,  88,  88,  87,  87,  70,  72,  74,  76,
    128,  73,  75,  84,  83,  77,  78,  79,  80,  90,  90,  89, 128,  92, 128,  91,
     91,  95,  95,  93,  94,  98, 128, 101, 101, 101, 101, 102, 102, 102, 102, 100,
    100,  82,  81,  68,  69,  21,  22, 128,  23,  24,  25, 116, 115, 115, 115, 128};

// A created finisher (16001-19999, 30000-30999): its own records, not in the category lists.
bool CreatedFinisher(uint16_t id) { return (id >= 16001 && id <= 19999) || (id >= 30000 && id <= 30999); }

}  // namespace

namespace svr2011 {
void InstallCamModMoves(rex::memory::Memory* memory) { g_memory = memory; }
}  // namespace svr2011

// The game tells kinds of move apart by their id: in a submission = the motion in 21000-21499 (sub_821B57F8 for an
// id, sub_821B58F8 for a fighter's current one), a standing submission 21000-21099 (sub_821B2540), a front grapple
// 4500-5549 / 6510-6609 / 21000-21099 / ... (sub_821B9E80). A pack's move copied onto another record (its wazecopy:
// the free records are elsewhere - every 21000s record is a stock submission) answered as nothing of the kind: a
// ported submission ran no hold - its victim stood up when x 0 ended (2K15's Kimura Lock, report 20261104-093125).
// These tests now ask about the move it was copied from.
namespace {
uint32_t AsCopied(uint32_t id) {
  const uint16_t from = id < 65536 ? svr2011::PackCopySource(uint16_t(id)) : 0;
  return from ? from : id;
}
}  // namespace

REX_EXTERN(__imp__sub_821B57F8);
REX_HOOK_RAW(sub_821B57F8) {
  ctx.r3.u64 = AsCopied(ctx.r3.u32);
  __imp__sub_821B57F8(ctx, base);
}

REX_EXTERN(__imp__sub_821B2540);
REX_HOOK_RAW(sub_821B2540) {
  ctx.r3.u64 = AsCopied(ctx.r3.u32);
  __imp__sub_821B2540(ctx, base);
}

REX_EXTERN(__imp__sub_821B9E80);
REX_HOOK_RAW(sub_821B9E80) {
  ctx.r3.u64 = AsCopied(ctx.r3.u32);
  __imp__sub_821B9E80(ctx, base);
}

REX_HOOK_RAW(sub_821B58F8) {   // (the fighter's motion +212 in 21000-21499 - the game's own, its id as copied)
  const int32_t m = int32_t(AsCopied(Rd32(base, ctx.r3.u32 + 212)));
  ctx.r3.u64 = (m >= 21000 && m < 21500) ? 1 : 0;
}

// CREATE A MOVE-SET's list rows (sub_82BA1AC8(page, tab, first row)): each of the 13 rows' text element
// (*(*(page + 620) + 8) + ((row + *(page + 1112)) << 8)) keeps a pointer to its record's name at +216. A pack
// move's row points to "MOD: <name>" - unless the record's own name has it (move_packs.cpp names pack moves so
// everywhere since 29 Oct, the match HUD too).
REX_EXTERN(__imp__sub_82BA1AC8);
REX_HOOK_RAW(sub_82BA1AC8) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_82BA1AC8(ctx, base);
  const uint32_t table = Rd32(base, kMoveTable);
  if (!g_memory || !page || !table) return;
  const uint32_t records = Rd32(base, table + 60), count = Rd32(base, table + 56);
  const uint32_t items = Rd32(base, page + 620);
  if (!records || !items || !Rd32(base, items + 8)) return;
  for (uint32_t row = 0; row < 13; ++row) {
    const uint32_t el = Rd32(base, items + 8) + ((row + Rd32(base, page + 1112)) << 8);
    const uint32_t name = Rd32(base, el + 216);
    if (name < records + 16 || (name - 16 - records) % kRecordSize || (name - 16 - records) / kRecordSize >= count) continue;
    const uint32_t rec = name - 16;
    const uint16_t id = Rd16(base, rec + 0x90);
    if (!svr2011::PackMove(id)) continue;
    // (the record's own name says it already - PatchWaze names pack moves "MOD: <name>": no "MOD: MOD: ")
    if (!std::strncmp(reinterpret_cast<const char*>(base + name), "MOD: ", 5)) continue;
    uint32_t label = 0;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_labels.find(id); it != g_labels.end()) {
        label = it->second;
      } else {
        const std::string text = svr2011::CleanUtf8(
            "MOD: " + std::string(reinterpret_cast<const char*>(base + name), strnlen(reinterpret_cast<const char*>(base + name), 63)), 68);
        label = g_memory->SystemHeapAlloc(uint32_t(text.size() + 1));
        if (label) std::memcpy(g_memory->TranslateVirtual<char*>(label), text.c_str(), text.size() + 1);
        g_labels[id] = label;
      }
    }
    if (label) base[el + 216] = uint8_t(label >> 24), base[el + 217] = uint8_t(label >> 16), base[el + 218] = uint8_t(label >> 8), base[el + 219] = uint8_t(label);
  }
}

// CREATE A MOVE-SET's tab lists filled (sub_82378A68): the packs' moves ("MOD: " names) go to the end of
// each list, in their order - a MOD group under the game's moves (list 25's first row, NONE, stays).
REX_EXTERN(__imp__sub_82378A68);
REX_HOOK_RAW(sub_82378A68) {
  __imp__sub_82378A68(ctx, base);
  const uint32_t table = Rd32(base, kMoveTable);
  if (!table) return;
  const uint32_t records = Rd32(base, table + 60);
  if (!records) return;
  int moved = 0;
  for (uint32_t l = 0; l < 26; ++l) {
    const uint32_t list = Rd32(base, table + 840 + l * 4);
    const uint16_t n = Rd16(base, table + 944 + l * 2);
    if (!list || n < 2) continue;
    std::vector<uint16_t> keep, mods;
    for (uint16_t i = 0; i < n; ++i) {
      const uint16_t idx = Rd16(base, list + 2 * i);
      (IsModRecord(base, records, idx) ? mods : keep).push_back(idx);
    }
    if (mods.empty()) continue;
    uint16_t i = 0;
    for (const uint16_t v : keep) Wr16(base, list + 2 * i++, v);
    for (const uint16_t v : mods) Wr16(base, list + 2 * i++, v);
    moved += int(mods.size());
  }
  static int logged = 0;
  if (moved && logged++ < 4) REXLOG_INFO("[svr2011] cam mods: {} MOD rows grouped at the end of the move lists", moved);
}

// A move by category and id (sub_8237D4C8(table, cat, id) -> its record, NULL when the category list
// hasn't it): saving a moveset (sub_8237E9E0) reads the tag-team finisher's record +115 without a check
// (returning to 0x8237EBF4) - a removed pack's move there crashed the save. It gets record 0 ("* test
// motion *", no category: the code's "none").
REX_EXTERN(__imp__sub_8237D4C8);
REX_HOOK_RAW(sub_8237D4C8) {
  const uint32_t lr = uint32_t(ctx.lr), id = ctx.r5.u32, table = ctx.r3.u32;
  __imp__sub_8237D4C8(ctx, base);
  if (ctx.r3.u32 || lr != 0x8237EBF4 || !table) return;
  ctx.r3.u64 = Rd32(base, table + 60);
  static int logged = 0;
  if (logged++ < 8) REXLOG_WARN("[svr2011] cam mods: tag-team finisher {} has no record (pack removed?) - saved as it is", id & 0xFFFF);
}

// A match's move for a slot: sub_8218E580(fighter, slot) the raw id, sub_8218E600(fighter, slot) the id with
// the game's own substitutions (88 callers: every attack asks it). A slot naming a move with no usable record
// (its pack removed, or a profile from another PC) would play a motion that isn't there: the match gets the
// first move of the slot's category instead. The moveset itself is untouched, so the move comes back with
// its pack. (The profile copy sub_82193620 isn't on an exhibition match's path: the getters are.)
namespace {
// Test aid: SVR2011_TEST_MISSING_MOVE=<slot>:<id> (or all:<id>) makes the match see that id (no record) in
// that slot (every slot).
const std::pair<int, int>& TestMissing() {
  static const std::pair<int, int> test = [] {
    const char* v = std::getenv("SVR2011_TEST_MISSING_MOVE");
    int s = -1, id = 0;
    if (v && std::sscanf(v, "all:%d", &id) == 1) return std::make_pair(999, id);
    if (v && std::sscanf(v, "%d:%d", &s, &id) == 2 && s >= 0 && s < 224) return std::make_pair(s, id);
    return std::make_pair(-1, 0);
  }();
  return test;
}

uint32_t MatchMove(uint8_t* base, uint32_t slot, uint32_t got) {
  if (slot >= 224) return got;
  static bool once = false;
  if (!once) {
    once = true;
    REXLOG_INFO("[svr2011] cam mods: match move getter active (slot {} move {})", slot, got & 0x7FFF);
  }
  // Test aid: SVR2011_TEST_MOVE_LOG=1 logs the first 2000 distinct (slot, move) pairs the match asks for (menus ask
  // too: 200 ran out before a match).
  static const bool trace = [] { const char* v = std::getenv("SVR2011_TEST_MOVE_LOG"); return v && *v == '1'; }();
  if (trace) {
    static std::mutex tmu;
    static std::map<uint32_t, bool> seen;
    std::lock_guard lock(tmu);
    if (seen.size() < 2000 && !seen[slot << 16 | (got & 0xFFFF)]) {
      seen[slot << 16 | (got & 0xFFFF)] = true;
      REXLOG_INFO("[svr2011] cam mods: match asks slot {} (category {}) -> move {}", slot, kSlotCategory[slot], got & 0x7FFF);
    }
  }
  // Test aid: SVR2011_TEST_SLOT_MOVE=<slot>[,<slot>...]:<id> - every person's move in those slots is that one, as if
  // in the moveset (a pack's move started the game's own way - a CPU's finisher with SVR2011_TEST_MOMENTUM)
  static const std::pair<std::vector<int>, int> slot_move = [] {
    std::pair<std::vector<int>, int> r{{}, 0};
    const char* v = std::getenv("SVR2011_TEST_SLOT_MOVE");
    const char* colon = v ? std::strchr(v, ':') : nullptr;
    if (!colon) return r;
    r.second = std::atoi(colon + 1);
    for (const char* p = v; p < colon; ++p)
      if (p == v || p[-1] == ',') r.first.push_back(std::atoi(p));
    return r;
  }();
  if (std::find(slot_move.first.begin(), slot_move.first.end(), int(slot)) != slot_move.first.end())
    return (got & 0x8000) | uint32_t(slot_move.second);
  const auto& test = TestMissing();
  bool planted = false;
  if (test.first == int(slot) || (test.first == 999 && kSlotCategory[slot] < 128))
    got = (got & 0x8000) | uint32_t(test.second), planted = true;
  // Test aid: SVR2011_TEST_HELD_MOVE=<slot>:<id> (or all:<id>): the match sees that id there, judged as a moveset's
  // move would be (a save holding a move with no motion - NoMotionMove)
  static const std::pair<int, int> held = [] {
    const char* v = std::getenv("SVR2011_TEST_HELD_MOVE");
    int s = -1, id = 0;
    if (v && std::sscanf(v, "all:%d", &id) == 1) return std::make_pair(999, id);
    if (v && std::sscanf(v, "%d:%d", &s, &id) == 2 && s >= 0 && s < 224) return std::make_pair(s, id);
    return std::make_pair(-1, 0);
  }();
  if (!planted && (held.first == int(slot) || (held.first == 999 && kSlotCategory[slot] < 128)))
    got = (got & 0x8000) | uint32_t(held.second);
  const uint32_t cat = kSlotCategory[slot];
  const uint16_t id = uint16_t(got & 0x7FFF);
  // (no move table in a match - 0x82E3C214 is a menu's: what the game loaded is asked, move_packs.cpp)
  // (online: a move from a pack not every player has - its category's default on every game: online_mods.h)
  // (a networked match - P2PMatchLobby().in, the same on every peer: the rules every peer judges the same only -
  // NoMotionMove and the missing / left-out pack motions depend on each player's own files, and lockstep peers must
  // pick the same move. Asked last: it takes p2p's lock. Test aid SVR2011_TEST_ONLINE_MOVES=1: judged as networked)
  static const bool test_online = [] { const char* v = std::getenv("SVR2011_TEST_ONLINE_MOVES"); return v && *v == '1'; }();
  const auto networked = [] { return test_online || svr2011::P2PMatchLobby().in; };
  if (cat >= 128 || !id || CreatedFinisher(id) ||
      !(planted || svr2011::MoveMissing(id, false) || (svr2011::MoveMissing(id, true) && !networked()) ||
        !svr2011::OnlinePackMoveAllowed(id)))
    return got;
  const uint16_t def = svr2011::CategoryDefaultMove(int(cat));
  if (!def) {
    static int nolog = 0;
    if (nolog++ < 8) REXLOG_WARN("[svr2011] cam mods: slot {} move {} has no record and category {} has no move to use", slot, id, cat);
    return got;
  }
  static std::mutex mu;
  static std::map<uint32_t, bool> logged;
  {
    std::lock_guard lock(mu);
    if (logged.size() < 64 && !logged[slot << 16 | id]) {
      logged[slot << 16 | id] = true;
      REXLOG_WARN("[svr2011] cam mods: slot {} move {} has no record (pack removed?) - {} in this match", slot, id, def);
    }
  }
  return (got & 0x8000) | def;
}
}  // namespace

REX_EXTERN(__imp__sub_8218E600);
REX_HOOK_RAW(sub_8218E600) {
  const uint32_t fighter = ctx.r3.u32, slot = ctx.r4.u32;
  __imp__sub_8218E600(ctx, base);
  ctx.r3.u64 = MatchMove(base, slot, ctx.r3.u32);
  svr2011::NoteMove(fighter, slot, ctx.r3.u32);  // (a frozen match's report: match_diag.h)
  svr2011::CrowdMoveAsked(fighter, slot);         // (a taunt by the barricade: crowd_fight.h)
}
REX_EXTERN(__imp__sub_8218E580);
REX_HOOK_RAW(sub_8218E580) {
  const uint32_t fighter = ctx.r3.u32, slot = ctx.r4.u32;
  __imp__sub_8218E580(ctx, base);
  ctx.r3.u64 = MatchMove(base, slot, ctx.r3.u32);
  svr2011::NoteMove(fighter, slot, ctx.r3.u32);
  svr2011::CrowdMoveAsked(fighter, slot);
}
