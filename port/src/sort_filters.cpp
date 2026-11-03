// WWE SmackDown vs. Raw 2011 - list menus' sort categories (Superstar
// Threads and the like: X opens "Sort Category"): NPC and MODS, and
// characters re-tagged into a category.
//
// The categories (label string 0x122 + category, sub_824611B8): 0
// CHAMPIONSHIP, 1 SMACKDOWN, 2 RAW, 3 ECW, 4 CREATE A SUPERSTAR, 5 OVERALL,
// 6 DEFAULT, 7 RANDOM - each a 2580-byte list (count, 322 ids, 322 flags)
// at +8116 + category * 2580 of the screen's object (category at +88; an
// empty list is greyed out). sub_82734970(?, lists) builds them from the
// master list 6. There is room for 8 lists only, so two that are always
// empty in this game take the new ones:
// - 3 ECW (no ECW brand in 2011) -> NPC: the managers and valets
//   (managers.cpp's playable ones) and characters tagged NPC;
// - 5 OVERALL (the roster by rating - DEFAULT still lists everyone) -> MODS:
//   the superstar mods (superstar_mods.h) and characters tagged MODS. (Not
//   7 RANDOM, though its list is empty: picked, its view is the roster
//   shuffled, not list 7.)
// Tags: UserData/sort_tags.txt, lines "<id> = SMACKDOWN | RAW | NPC | MODS"
// (# comments): the character moves into that category (out of the other
// three). The labels "NPC" / "MODS" are guest strings in place of the
// game's.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "big_roster.h"
#include "generated/default/svr2011_init.h"
#include "rtwm_streak.h"
#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

using svrfmt::Rd32, svrfmt::Wr32;

constexpr uint32_t kListSize = 2580, kMaxEntries = 322, kFlags = 4 + 1288;
constexpr uint32_t kSmackDown = 1, kRaw = 2, kNpc = 3, kMods = 5;
// The playable managers and valets (managers.cpp's list): Stephanie
// McMahon, Theodore Long, Paul Bearer, Tiffany, The Hurricane.
constexpr uint32_t kNpcs[] = {147, 186, 251, 296, 274};

// UserData/sort_tags.txt: id -> category (read once).
const std::map<uint32_t, uint32_t>& Tags() {
  static const std::map<uint32_t, uint32_t> tags = [] {
    std::map<uint32_t, uint32_t> t;
    std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "UserData";
    if (const char* v = std::getenv("SVR2011_USER_DATA"); v && *v) dir = v;
    std::ifstream in(dir / "sort_tags.txt");
    std::string line;
    while (std::getline(in, line)) {
      if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string name = line.substr(eq + 1);
      name.erase(std::remove_if(name.begin(), name.end(), ::isspace), name.end());
      std::transform(name.begin(), name.end(), name.begin(), ::toupper);
      const uint32_t id = uint32_t(std::strtoul(line.c_str(), nullptr, 10));
      const uint32_t cat = name == "SMACKDOWN" ? kSmackDown : name == "RAW" ? kRaw : name == "NPC" ? kNpc
                           : name == "MODS"    ? kMods      : 0;
      if (id && cat) t[id] = cat;
    }
    if (!t.empty()) REXLOG_INFO("[svr2011] sort: {} characters re-tagged (sort_tags.txt)", t.size());
    return t;
  }();
  return tags;
}

struct List {
  uint8_t* base;
  uint32_t at;
  uint32_t Count() const { return Rd32(base + at); }
  uint32_t Id(uint32_t i) const { return Rd32(base + at + 4 + i * 4); }
  uint32_t Flag(uint32_t i) const { return Rd32(base + at + kFlags + i * 4); }
  bool Has(uint32_t id) const {
    for (uint32_t i = 0; i < Count(); ++i)
      if ((Id(i) & 0x7FFFFFFF) == id) return true;
    return false;
  }
  void Add(uint32_t id, uint32_t flag) {
    const uint32_t n = Count();
    if (n >= kMaxEntries || Has(id)) return;
    Wr32(base + at + 4 + n * 4, id);
    Wr32(base + at + kFlags + n * 4, flag);
    Wr32(base + at, n + 1);
  }
  void Remove(uint32_t id) {
    uint32_t n = Count(), out = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t v = Id(i), f = Flag(i);
      if ((v & 0x7FFFFFFF) == id) continue;
      Wr32(base + at + 4 + out * 4, v);
      Wr32(base + at + kFlags + out * 4, f);
      ++out;
    }
    Wr32(base + at, out);
  }
};

// The MODS list (5): the mods and characters tagged MODS.
void FillMods(uint8_t* base, uint32_t lists) {
  List mods{base, lists + kMods * kListSize};
  const List master{base, lists + 6 * kListSize};
  auto flag_of = [&](uint32_t id) {
    for (uint32_t i = 0; i < master.Count(); ++i)
      if ((master.Id(i) & 0x7FFFFFFF) == id) return master.Flag(i);
    return 1u;
  };
  Wr32(base + mods.at, 0);
  for (uint32_t id : svr2011::SuperstarModIds()) mods.Add(id, flag_of(id));
  for (const auto& [id, to] : Tags())
    if (to == kMods) mods.Add(id, flag_of(id));
}

// The lists of a list block (8 categories) after the game built them.
void Retag(uint8_t* base, uint32_t lists) {
  List cat[8];
  for (uint32_t i = 0; i < 8; ++i) cat[i] = List{base, lists + i * kListSize};
  const List& master = cat[6];
  auto flag_of = [&](uint32_t id) {  // (the master list's flag, or 1: on)
    for (uint32_t i = 0; i < master.Count(); ++i)
      if ((master.Id(i) & 0x7FFFFFFF) == id) return master.Flag(i);
    return 1u;
  };
  Wr32(base + cat[kNpc].at, 0);
  for (uint32_t id : kNpcs) cat[kNpc].Add(id, 1);
  for (const auto& [id, to] : Tags()) {
    for (uint32_t c : {kSmackDown, kRaw, kNpc}) cat[c].Remove(id);
    if (to != kMods) cat[to].Add(id, flag_of(id));
  }
  FillMods(base, lists);
}

// A guest copy of `text` (allocated once).
uint32_t GuestString(const char* text) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return 0;
  const uint32_t n = uint32_t(std::strlen(text)) + 1;
  const uint32_t g = memory->SystemHeapAlloc(n);
  if (g) std::memcpy(memory->virtual_membase() + g, text, n);
  return g;
}

}  // namespace

// Test aid: SVR2011_TEST_ROSTER_LIST=<file> - writes "<id> <name> <+209>
// <selectable> <gender> <DLC>" (tab separated) for every roster id (once, as the lists are
// first built) - for the launcher's tag editor.
void DumpRoster(uint8_t* base) {
  static bool done = false;
  const char* path = std::getenv("SVR2011_TEST_ROSTER_LIST");
  if (done || !path || !*path) return;
  done = true;
  constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260;
  std::ofstream out(path);
  out << "# id\tname\t+209\tselectable\tgender(1 diva)\tdlc\n";
  int n = 0;
  for (uint32_t id = 1; id < 1000; ++id) {
    const uint32_t index = uint32_t(base[kIdToIndex + id * 2]) << 8 | base[kIdToIndex + id * 2 + 1];
    if (index >= 1000) continue;
    const uint8_t* rec = base + svr2011::RosterEntry(kRecords, kRecordSize, index);
    if ((uint32_t(rec[32]) << 8 | rec[33]) != id) continue;
    const std::string name(reinterpret_cast<const char*>(rec + 34), strnlen(reinterpret_cast<const char*>(rec + 34), 32));
    if (name.empty()) continue;
    out << id << '\t' << name << '\t' << int(rec[209]) << '\t' << int(rec[221]) << '\t' << int(rec[208]) << '\t'
        << int(rec[257]) << '\n';
    ++n;
  }
  REXLOG_INFO("[svr2011] sort: roster list ({} ids) written to {}", n, path);
}

// A list block is built: sub_82734970(?, lists).
REX_EXTERN(__imp__sub_82734970);
REX_HOOK_RAW(sub_82734970) {
  const uint32_t lists = ctx.r4.u32;
  __imp__sub_82734970(ctx, base);
  // (not a list of Create-a-Superstar slots: Road to WrestleMania's Streak story lists only those - its master holds
  // slot numbers 0, 1, 2 ..., not roster ids - and NPC / MODS put roster ids in it: they showed, and picking one did
  // nothing (slot 111...). A roster list has no id 0; its CAS entries have the top bit. Every id a slot number (under
  // the 50 CAS slots) and id 0 there - or, in Road to WrestleMania, any slots: a save whose one CAW is in slot 2
  // listed only {2}, was taken for a roster list and its MODS showed the CAW alone, 28 Oct.)
  constexpr uint32_t kCasSlots = 50;
  bool slots = false;
  if (lists) {
    const List master{base, lists + 6 * kListSize};
    bool all_slots = master.Count() > 0, zero = false;
    for (uint32_t i = 0; i < master.Count(); ++i) all_slots &= master.Id(i) < kCasSlots, zero |= master.Id(i) == 0;
    slots = all_slots && (zero || svr2011::InRoadToWrestleMania());
  }
  if (lists && !slots) Retag(base, lists);
  if (lists && !slots) svr2011::RtwmCasList(0, false);
  // (Road to WrestleMania's Streak list: the superstar mods in MODS - rtwm_streak.cpp plays the story as one)
  if (lists && slots && svr2011::InRoadToWrestleMania() && !svr2011::SuperstarModIds().empty()) {
    const List master{base, lists + 6 * kListSize};
    svr2011::RtwmCasList(master.Count() ? master.Id(0) : 0, true);
    FillMods(base, lists);
  }
  DumpRoster(base);
  if (lists && std::getenv("SVR2011_TEST_LIST_LOG")) {  // (test aid: each category's count and first ids)
    for (uint32_t c = 0; c < 8; ++c) {
      const List l{base, lists + c * kListSize};
      std::string ids;
      for (uint32_t i = 0; i < l.Count() && i < 12; ++i) ids += fmt::format(" {:X}/{}", l.Id(i), l.Flag(i));
      REXLOG_INFO("[svr2011] sort: list {:08X} cat {}: {} -{}", lists, c, l.Count(), ids);
    }
  }
}

// A category is shown: sub_824611B8(screen, player) - its label (the text
// object's +2008) for the new two.
REX_EXTERN(__imp__sub_824611B8);
REX_HOOK_RAW(sub_824611B8) {
  const uint32_t obj = ctx.r3.u32, player = ctx.r4.u32;
  __imp__sub_824611B8(ctx, base);
  const uint32_t cat = Rd32(base + obj + 88);
  if (cat != kNpc && cat != kMods) return;
  static const uint32_t npc = GuestString("NPC"), mods = GuestString("MODS");
  // (the text object, as the function finds it)
  const uint32_t entries = Rd32(base + Rd32(base + obj + 276) + 12);
  const uint32_t p = Rd32(base + entries + player * 12 + 4);
  const uint32_t t = p ? Rd32(base + p + 7872) : 0;
  const uint32_t text = t ? Rd32(base + t + 112) : 0;
  if (text && (cat == kNpc ? npc : mods)) Wr32(base + text + 2008, cat == kNpc ? npc : mods);
}
