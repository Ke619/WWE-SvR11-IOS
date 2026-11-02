// WWE SmackDown vs. Raw 2011 - Road to WrestleMania's Streak story ("vs Undertaker") played as a superstar mod.
//
// The story is for Created Superstars only: its select is a list menu of Create-a-Superstar slots (sort_filters.cpp
// keeps the game's lists there), and the story keeps the player as that CAW slot throughout - its saves, events and
// cutscenes. Here, in Road to WrestleMania, the MODS category of that list shows the superstar mods; picking one
// hands the story a CAW (the list's first) so it starts, and the new story remembers the mod (UserData/
// rtwm_streak.txt: the mod's folder and the CAW slot the story got - kept for CONTINUE). In its matches, the person
// who is that CAW becomes the mod (the match's people, test_match.cpp's hook: RtwmStreakPeople). Undertaker stays
// himself. The scenes with the player alone (the backstage walks) are the mod's too (29 Oct): the walk's load spun on
// '/EMD/<id>00' - a CAW's pick ends in kind 0, which no model pac has (the swap now writes kind 2) - and then the
// walk found no player character by the CAW's id (sub_825344E0's hook, below). Picking a CAW plays the story as it is.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cae_mod_motions.h"
#include "generated/default/svr2011_init.h"
#include "rtwm_streak.h"
#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

namespace svr2011 {
bool InRoadToWrestleMania();  // (menu_hooks.cpp: the main menu's last choice)
}

namespace {

using svrfmt::Rd32, svrfmt::RdF, svrfmt::Wr32, svrfmt::WrF;
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
uint32_t g_solo_frames = 0;
// The swap in force: the story's player is the CAW (its person id, the save slot) but his character carries the
// mod's id - sub_825344E0 below finds him by either (0 / 0: none)
uint32_t g_swap_caw = 0, g_swap_id = 0, g_swap_person = 0;
// The loader's entry that built the mod's character (its first word the person's pick), and whether the scene set up
// last has the player alone (the walks): sub_82195BA0's hook below
uint32_t g_player_entry = 0;
// The story's own id for its player in the walk (its pick's: 104, Jericho's suit, for a mod - sub_82556000's hook):
// the walk finds its player by it too - the character, now the mod's, answers to it as well (sub_825344E0's hook)
uint32_t g_story_id = 0;
bool g_solo_scene = false;  // (world frames since the last scene with the player alone was set up; 0: none)

std::filesystem::path StateFile() {
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "UserData";
  if (const char* v = std::getenv("SVR2011_USER_DATA"); v && *v) dir = v;
  return dir / "rtwm_streak.txt";
}

// The choice: the mod (its folder) and the CAW slot the story was given (-1: none - the story as it is).
struct Choice {
  std::string folder;
  int caw = -1;
};
Choice& State() {
  static Choice c = [] {
    Choice r;
    std::ifstream in(StateFile());
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("mod=", 0) == 0) r.folder = line.substr(4);
      if (line.rfind("caw=", 0) == 0) r.caw = std::atoi(line.c_str() + 4);
    }
    if (!r.folder.empty() && r.caw >= 0) REXLOG_INFO("[svr2011] rtwm streak: plays as the mod {} (CAW slot {})", r.folder, r.caw);
    return r;
  }();
  return c;
}
void Save(const Choice& c) {
  std::error_code ec;
  std::filesystem::create_directories(StateFile().parent_path(), ec);
  std::ofstream out(StateFile(), std::ios::trunc);
  out << "# Road to WrestleMania, the Streak story played as a superstar mod (the game writes this)\n";
  if (!c.folder.empty()) out << "mod=" << c.folder << "\ncaw=" << c.caw << "\n";
}

// The mod's id now (a mod keeps its id, slots.txt; 0: not installed / off).
uint32_t ModNow(const std::string& folder) {
  if (folder.empty()) return 0;
  for (uint32_t id : svr2011::SuperstarModIds(false))
    if (svr2011::SuperstarModFolder(id) == folder) return id;
  return 0;
}

uint32_t g_first_caw = 0;  // (the Streak list's first CAW slot: what the story gets for a mod)
bool g_cas_list = false;    // (the list shown is the Streak's - Create-a-Superstar slots)
// The select's last pick, for the new story (taken at its first people): the mod's folder, or "" for a CAW. Picks
// only browse until then - the select's own pick of its first entry as it opens too.
bool g_pending = false;
std::string g_pending_folder;
// The mod the cursor rests on (0: none) and where its pick is kept - the player's record +120, which holds the CAW
// slot for the story; the select's panel is shown the mod (sub_82466830 below).
uint32_t g_pick_mod = 0, g_pick_word = 0;

}  // namespace

namespace svr2011 {

void RtwmCasList(uint32_t first_caw, bool on) {
  g_cas_list = on;
  if (on) g_first_caw = first_caw;
}

void RtwmStreakPeople(uint8_t* base, uint32_t match) {
  if (!InRoadToWrestleMania()) return;
  Choice& c = State();
  if (g_pending) {  // (a new story: the select's last pick)
    g_pending = false;
    g_pick_mod = 0;  // (the select is gone: its panel no longer shown the mod)
    if (g_pending_folder != c.folder || (!c.folder.empty() && c.caw != int(g_first_caw))) {
      c.folder = g_pending_folder, c.caw = c.folder.empty() ? -1 : int(g_first_caw);
      Save(c);
      if (c.folder.empty()) REXLOG_INFO("[svr2011] rtwm streak: a new story with a CAW - played as it is");
      else REXLOG_INFO("[svr2011] rtwm streak: a new story - CAW slot {} plays as the mod {} in its matches", c.caw, c.folder);
    }
  }
  if (c.caw < 0 || c.folder.empty()) return;
  const uint32_t id = ModNow(c.folder);
  if (!id) {
    static bool said = false;
    if (!said) {
      REXLOG_WARN("[svr2011] rtwm streak: the mod {} isn't installed (or is off) - the CAW plays", c.folder);
      said = true;
    }
    return;
  }
  constexpr uint32_t kSlots = 432, kSlotSize = 2116, kNoPick = 51200;
  uint32_t people = 0;
  for (uint32_t i = 0; i < 6; ++i) people += Rd32(base + match + kSlots + i * kSlotSize + 8) != kNoPick;
  g_solo_scene = people < 2;
  if (people < 2) {  // (the player alone: a backstage walk - the mod plays it too)
    g_solo_frames = 1;  // (SVR2011_TEST_RTWM_WARP counts from here)
    static uint32_t said = 0;
    if (said++ < 3) REXLOG_INFO("[svr2011] rtwm streak: a scene with the player alone - the mod {} plays it", c.folder);
  }
  for (uint32_t i = 0; i < 6; ++i) {
    uint8_t* slot = base + match + kSlots + i * kSlotSize;
    const uint32_t v = Rd32(slot + 8);
    if (v == kNoPick || v / 100 != uint32_t(c.caw)) continue;
    // (the attire digit kept - the mod's attire 1 for the CAW's first - and the model kind always 2, a real
    // model's EMD "<id>x2": a CAW's pick ends in 0 in the solo scenes - the walk asked for '/EMD/00011100', which
    // no pac has, and the loader spun on its 0 bytes; in matches it ends in 2 already)
    static const bool kind0 = std::getenv("SVR2011_TEST_RTWM_KIND0") != nullptr;  // (test: the old digits - the 0-byte guard)
    Wr32(slot + 8, kind0 ? id * 100 + v % 100 : id * 100 + (v % 100) / 10 * 10 + 2);
    Wr16(slot + 54, id);
    g_swap_caw = uint32_t(c.caw), g_swap_id = id, g_swap_person = i;
    REXLOG_INFO("[svr2011] rtwm streak: person {} - the CAW (slot {}) plays as the mod {} (id {})", i, c.caw, c.folder, id);
  }
}

void RtwmBeforeFrame(uint8_t* base) {
  static const bool pos_log = std::getenv("SVR2011_TEST_POS_LOG") != nullptr;
  static const char* warp = std::getenv("SVR2011_TEST_RTWM_WARP");
  if ((!pos_log && !warp) || !InRoadToWrestleMania()) return;
  constexpr uint32_t kChars = 0x82E3CC50;  // (the match's characters: hctp_room.cpp)
  const uint32_t f = Rd32(base + kChars);
  if (!f) return;
  static uint32_t frame = 0;
  if (pos_log && ++frame % 60 == 0) {
    constexpr uint32_t kCharCount = 0x82E3CD0C;
    std::string s;
    for (uint32_t i = 0; i < 8; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4))
        s += fmt::format(" [{} {:08X} {:.1f} {:.1f} {:.1f}]", i, c, RdF(base + c + 288), RdF(base + c + 292), RdF(base + c + 296));
    REXLOG_INFO("[svr2011] rtwm: characters {}:{}", Rd32(base + kCharCount), s);
  }
  // (SVR2011_TEST_RTWM_WARP_ANY=1: the warps start with any Road to WrestleMania scene's first frames, not only a
  // mod-played one - a stock story's backstage walk)
  static const bool any = std::getenv("SVR2011_TEST_RTWM_WARP_ANY") != nullptr;
  if (warp && any && !g_solo_frames && Rd32(base + 0x82E3CD0C) < 10) g_solo_frames = 1;
  if (!warp || !g_solo_frames) return;
  ++g_solo_frames;
  // (5 s in, then each next place of "x,z[,y];x,z..." 6 s later; each held for 10 frames, as hctp_room's doors)
  constexpr uint32_t kAt = 300, kEvery = 360, kHeld = 10;
  if (g_solo_frames < kAt || (g_solo_frames - kAt) % kEvery >= kHeld) return;
  const uint32_t k = (g_solo_frames - kAt) / kEvery;
  const char* q = warp;
  for (uint32_t i = 0; i < k && q; ++i) q = std::strchr(q, ';'), q = q ? q + 1 : nullptr;
  if (!q) return;
  float x = 0, z = 0, y = RdF(base + f + 292);
  if (std::sscanf(q, "%f,%f,%f", &x, &z, &y) < 2) return;
  // The place is kept in more copies than the character's +288: its +0xF90 / +0xFD0, the root bones' matrices and
  // the walk's own controller (elsewhere in the heaps) - with +288 alone the player was back at the old place as
  // soon as he moved. A test aid, so plainly: every copy of the old place (x and z 8 bytes apart) in the game heaps,
  // found at the warp, is written. WARNING: test-only - the count grows each warp (14 -> 23 -> 42 in one walk: other
  // data matching by chance gets written too). Week 1's objective marker sits at about (-1, 340) (the entrance
  // stage); warping there or walking in from (-12, 330) did not start the next scene (23 Oct) - the walk asks none
  // of the event conditions (SVR2011_TEST_EVENT_LOG).
  static std::vector<uint32_t> copies;
  if ((g_solo_frames - kAt) % kEvery == 0) {
    const float ox = RdF(base + f + 288), oz = RdF(base + f + 296);
    copies.clear();
    for (uint32_t a = 0xA0000000; a < 0xB0000000; a += 4)  // (the game heaps)
      if (std::fabs(RdF(base + a) - ox) < 0.01f && std::fabs(RdF(base + a + 8) - oz) < 0.01f) copies.push_back(a);
    REXLOG_INFO("[svr2011] rtwm: test - fighter 0 put at {:.1f} {:.1f} {:.1f} (was {:.1f} {:.1f}; {} copies)", x, y, z, ox, oz,
                copies.size());
  }
  for (uint32_t a : copies) WrF(base + a, x), WrF(base + a + 4, y), WrF(base + a + 8, z);
}

}  // namespace svr2011

// A player's pick in a select list: sub_82732938(state, player, id). In Road to WrestleMania's CAW list (the
// Streak story) a mod is remembered and the story gets a CAW; a CAW picked plays the story as it is.
REX_EXTERN(__imp__sub_82732938);
REX_HOOK_RAW(sub_82732938) {
  const uint32_t id = ctx.r5.u32;
  static const bool log = std::getenv("SVR2011_TEST_PICK_LOG") != nullptr;
  if (log) REXLOG_INFO("[svr2011] pick: player {} id {:X} (rtwm {})", ctx.r4.u32, id, svr2011::InRoadToWrestleMania());
  svr2011::CaeNotePick(id);  // (CREATE AN ENTRANCE: the picked mod's own MOD: row - cae_mod_motions.cpp)
  g_pick_mod = 0;
  if (svr2011::InRoadToWrestleMania() && g_cas_list) {
    g_pending = true;
    g_pending_folder = svr2011::IsSuperstarMod(id) ? svr2011::SuperstarModFolder(id) : std::string();
    if (!g_pending_folder.empty()) {
      REXLOG_INFO("[svr2011] rtwm streak: {} picked - the story gets CAW slot {}, its matches the mod", g_pending_folder,
                  g_first_caw);
      // (the mod's id stored: the panel's picture is filled from it as the cursor moves; the CAW slot replaces it at
      // the end of the cursor's update - RtwmShowModEnd - before the story copies it)
      // (the record as the function finds it: state + its table [+13520] * 1352 + player * 204)
      g_pick_mod = id;
      g_pick_word = ctx.r3.u32 + Rd32(base + ctx.r3.u32 + 13520) * 1352 + ctx.r4.u32 * 204 + 120;
    }
  }
  __imp__sub_82732938(ctx, base);
}

// The select panel reads the player's pick (the record's +120) each frame. With a mod under the cursor the record
// holds the CAW slot the story gets, so the panel showed the CAW (DEFAULT / RAW / 78, 28 Oct): its readers are shown
// the mod while they run, the CAW slot put back after - the cursor's update (sub_82465038, mystery_opponent.cpp's
// hook), the "MODDED" badge's sub_82466830 (superstar_mods.cpp), sub_82465E88 and the text's sub_824678C8 (found with
// SVR2011_WATCH_ADDR + _READ). The story's own copy of the pick is taken outside them: with the mod's id left in +120
// the story began as the mod; with these it begins as the CAW slot.
namespace svr2011 {
bool RtwmShowModBegin(uint8_t* base) {
  const bool swap = g_pick_mod && g_pick_word && g_cas_list && InRoadToWrestleMania() &&
                    Rd32(base + g_pick_word) == g_first_caw;
  if (swap) Wr32(base + g_pick_word, g_pick_mod);
  return swap;
}
void RtwmShowModEnd(uint8_t* base, bool swapped) {
  if (swapped || (g_pick_mod && g_pick_word && g_cas_list && Rd32(base + g_pick_word) == g_pick_mod))
    Wr32(base + g_pick_word, g_first_caw);
}
}  // namespace svr2011
namespace {
template <void (*F)(PPCContext&, uint8_t*)>
void ShownTheMod(PPCContext& ctx, uint8_t* base) {
  const bool swapped = svr2011::RtwmShowModBegin(base);
  F(ctx, base);
  svr2011::RtwmShowModEnd(base, swapped);
}
}  // namespace
REX_EXTERN(__imp__sub_82466830);
REX_HOOK_RAW(sub_82466830) { ShownTheMod<__imp__sub_82466830>(ctx, base); }
REX_EXTERN(__imp__sub_82465E88);
REX_HOOK_RAW(sub_82465E88) { ShownTheMod<__imp__sub_82465E88>(ctx, base); }
// (name, brand and overall: sub_824678C8 and under it sub_82465728 / sub_824613E0 - with the others alone the panel
// had the mod's picture and badge over DEFAULT / 78)
REX_EXTERN(__imp__sub_824678C8);
REX_HOOK_RAW(sub_824678C8) { ShownTheMod<__imp__sub_824678C8>(ctx, base); }

// The story finds its player's character by his person id (sub_825344E0(id): the characters table, each one's
// person +80 -> sub_828B7028). That id is the CAW's; the character playing him is the mod's: without this the walk
// found no player (sub_82535018 -> 0) and crashed reading its +304 (sub_825484A0, 29 Oct).
REX_EXTERN(__imp__sub_825344E0);
REX_HOOK_RAW(sub_825344E0) {
  const uint32_t want = ctx.r3.u32;
  __imp__sub_825344E0(ctx, base);
  if (ctx.r3.u32 || !g_swap_id || !svr2011::InRoadToWrestleMania()) return;
  const uint32_t c = g_swap_person < 9 ? Rd32(base + 0x82E3CC50 + g_swap_person * 4) : 0;
  if (!c) return;
  const auto saved = ctx;
  ctx.r3.u64 = Rd32(base + c + 80);
  sub_828B7028(ctx, base);
  const uint32_t is = ctx.r3.u32;
  ctx = saved;
  if (want != g_swap_caw && !(g_solo_scene && g_story_id && want == g_story_id)) {
    // (another person's id finds nobody: the game's own answer stands - logged)
    static uint32_t said = 0;
    if (said++ < 5)
      REXLOG_INFO("[svr2011] rtwm streak: no character for person id {} (the player's {} reads {})", int32_t(want),
                  g_swap_caw, is);
    return;
  }
  // (the swapped person's character - characters-table entry = person - whatever his id reads: the mod's id, its
  // base's (the mod's record is a copy of the base's), or another later on - JBL's walk read neither some seconds
  // in and crashed as before, 29 Oct)
  ctx.r3.u64 = c;
  static uint32_t said = 0;
  if (said < 4 && (said == 0 || (is != g_swap_id && is != svr2011::SuperstarModBase(g_swap_id)))) {
    ++said;
    REXLOG_INFO("[svr2011] rtwm streak: the story's player (CAW {}) is the mod's character (his id reads {})", want, is);
  }
}

// The loader builds each character from an entry whose first word is the person's pick (sub_82195BA0(character,
// entry, ...): it writes the character's +80). In a walk the player's entry is built twice: first with the pick as the
// swap left it (the mod's), then again with 10412 - Chris Jericho's attire 2, his suit - written there in between:
// the walk's backstage clothes for the player, which the game has for its own superstars and CAWs only; the mod's id
// got Jericho's (29 Oct: JBL and Festus walked as Jericho). In a scene with the player alone, the entry that built
// the mod is given his pick again before it is built (attire 1, model kind 2: what every mod's pac has).
// That 10412 comes from the story's own person table: sub_82556000 copies a person's pick (sub_82573EC8(index) +16)
// into the loader's entry, before the loader thread reads the model it names (Jericho's /EMD/00010412 was read, then
// built). So the entry is put right as soon as it is filled; the build below checks again.
static void PlayerEntryAsMod(uint8_t* base, const char* where) {
  if (!g_player_entry || !g_swap_id || !g_solo_scene || !svr2011::InRoadToWrestleMania()) return;
  const uint32_t pick = Rd32(base + g_player_entry);
  if (pick / 100 == g_swap_id) return;
  Wr32(base + g_player_entry, g_swap_id * 100 + 2);
  static uint32_t said = 0;
  if (said++ < 4)
    REXLOG_INFO("[svr2011] rtwm streak: the walk's player loads as the mod (id {}), not as pick {} ({})", g_swap_id, pick,
                where);
}

// (sub_82556000(r3): person index *(r3 + 16); the record sub_82573EC8(index), its pick at +16, is copied to the loader's
// entry and to a second copy the model load reads. For the call the record holds the mod's pick - both copies the
// mod's - and gets its own back after: the story's data, and its saves, stay the CAW story's)
REX_EXTERN(__imp__sub_82556000);
REX_HOOK_RAW(sub_82556000) {
  uint32_t rec = 0, was = 0;
  if (g_swap_id && g_solo_scene && svr2011::InRoadToWrestleMania() && ctx.r3.u32) {
    const uint32_t index = Rd32(base + ctx.r3.u32 + 16);
    if (index == g_swap_person) {
      const auto saved = ctx;
      ctx.r3.u64 = index;
      sub_82573EC8(ctx, base);
      rec = ctx.r3.u32;
      ctx = saved;
      if (rec) {
        was = Rd32(base + rec + 16);
        if (was == 51200 || was / 100 == g_swap_id) rec = 0;  // (no pick, or the mod already)
        else Wr32(base + rec + 16, g_swap_id * 100 + 2);
      }
    }
  }
  __imp__sub_82556000(ctx, base);
  if (rec) {
    Wr32(base + rec + 16, was);
    g_story_id = was / 100;
    static uint32_t said = 0;
    if (said++ < 4)
      REXLOG_INFO("[svr2011] rtwm streak: the walk's player loads as the mod (id {}), not as the story's pick {}", g_swap_id,
                  was);
  }
  PlayerEntryAsMod(base, "copied from the story");
}

// (research, SVR2011_TEST_RTWM_BUILD_LOG=1: each build's entry and the characters' +80 after)
REX_EXTERN(__imp__sub_82195BA0);
REX_HOOK_RAW(sub_82195BA0) {
  static const bool log = std::getenv("SVR2011_TEST_RTWM_BUILD_LOG") != nullptr;
  const uint32_t r3 = ctx.r3.u32, entry = ctx.r4.u32;
  if (entry && g_swap_id && svr2011::InRoadToWrestleMania()) {
    const uint32_t pick = Rd32(base + entry);
    if (pick / 100 == g_swap_id) g_player_entry = entry;
    else if (entry == g_player_entry) PlayerEntryAsMod(base, "at the build");
  }
  __imp__sub_82195BA0(ctx, base);
  if (!log) return;
  std::string chars;
  for (uint32_t i = 0; i < 3; ++i)
    if (const uint32_t c = Rd32(base + 0x82E3CC50 + i * 4)) chars += fmt::format(" {}:{:08X}+80={}", i, c, Rd32(base + c + 80));
  REXLOG_INFO("[svr2011] rtwm build: sub_82195BA0({:08X}, entry {:08X} pick {}) ->{}", r3, entry,
              entry ? Rd32(base + entry) : 0, chars);
}
