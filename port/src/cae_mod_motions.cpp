// MODS rows in CREATE AN ENTRANCE - see cae_mod_motions.h and docs/CAE_MOD_MOTIONS.md.
//
// The game's lists are data (pac\DLC.pac ENEX/LSD: categories of {value, string id, unlock}),
// turned into menu rows by sub_8287DFD0(list, r4, table, category, base); a list is
// {count, capacity, items}, items 24 bytes {value, flags, unlock, text (a guest char*) at +12},
// sub_828829D8(list, value, text, 0, 0, unlock) appends one. The motion lists:
//   - the one-page editor's MOTION: category 2 (singles), 3 / 4 (tag / trio teams), and its
//     CHAMPION (16) / MULTI-TITLE (17) entrances: values = entrance numbers;
//   - the scene editor's MOTION for scenes 0-4 (segments 0/5/10/15/20): categories 15, 7, 8, 9, 10
//     (table 0x8206B708): values = CAE part numbers 10000 + 1000k + n.
// A saved entrance keeps only the numbers (S+16, or part +14 at S+24+34p), so a mod's rows need no
// ids of their own: the pac numbers themselves. The game finds a script by number in any pac
// (sub_82751C60), and skips a part whose script is missing (that scene doesn't play).
#include "cae_mod_motions.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "move_packs.h"
#include "superstar_mods.h"

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr uint32_t kItemSize = 24;
constexpr uint32_t kHeaderValue = 0xFFFFFFFE;  // (the disabled header row: a value no list has)
constexpr uint32_t kPartCategories[5] = {15, 7, 8, 9, 10};

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;
std::map<std::string, uint32_t> g_text;  // label -> guest string (lives for the whole run)
std::set<uint32_t> g_motion_lists;       // menu lists filled with a motion category
std::set<uint32_t> g_light_lists;        // menu lists filled with the lighting category (14)
uint32_t g_picked = 0;                   // the superstar last picked in a select screen (CaeNotePick)
thread_local bool t_loading = false;     // the full editor loading a stored entrance (CaeLoadingEntrance)
thread_local bool t_scene_default = false;  // (its last scene motion was the mod's own default: its lighting is too)

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32;

uint32_t Text(const std::string& label) {  // (g_mutex held)
  if (auto it = g_text.find(label); it != g_text.end()) return it->second;
  const uint32_t s = g_memory->SystemHeapAlloc(uint32_t(label.size() + 1));
  if (!s) return 0;
  std::memcpy(g_memory->TranslateVirtual<char*>(s), label.c_str(), label.size() + 1);
  g_text[label] = s;
  return s;
}

int IndexOf(uint8_t* base, uint32_t list, uint32_t value) {
  const uint32_t n = Rd32(base, list), items = Rd32(base, list + 8);
  for (uint32_t i = 0; i < n && items; ++i)
    if (Rd32(base, items + i * kItemSize) == value) return int(i);
  return -1;
}

// sub_828829D8(list, value, text, 0, 0, unlock -1) -> the new row's index
int AddRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value, uint32_t text, bool disabled) {
  const auto saved = ctx;
  ctx.r3.u64 = list;
  ctx.r4.u64 = value;
  ctx.r5.u64 = text;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = uint64_t(int64_t(-1));
  sub_828829D8(ctx, base);
  ctx = saved;
  const uint32_t n = Rd32(base, list);
  if (!n || Rd32(base, Rd32(base, list + 8) + (n - 1) * kItemSize) != value) return -1;
  if (disabled) Wr32(base, Rd32(base, list + 8) + (n - 1) * kItemSize + 4, 1);  // (flags 1: greyed, not picked)
  return int(n - 1);
}

// The kind of motion list a category is: the CAE part k (0-4), or -2 singles, -3 teams; -1 none.
int ListKind(uint32_t category) {
  for (int k = 0; k < 5; ++k)
    if (category == kPartCategories[k]) return k;
  if (category == 2 || category == 16 || category == 17) return -2;
  if (category == 3 || category == 4) return -3;
  return -1;
}

bool TeamNumber(int n) { return n >= 700 && n < 800; }  // (2011's tag / trio entrances: 700-799)

}  // namespace

namespace svr2011 {

void InstallCaeModMotions(rex::memory::Memory* memory) { g_memory = memory; }

void CaeModRows(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t category, int scene) {
  const int kind = scene >= 0 ? scene : ListKind(category);
  if (!g_memory || !list) return;
  static const bool list_log = std::getenv("SVR2011_TEST_CAE_LISTS") != nullptr;  // (test aid: every list filled)
  if (list_log) {
    std::string v;
    const uint32_t n = Rd32(base, list), items = Rd32(base, list + 8);
    for (uint32_t i = 0; i < n && i < 6 && items; ++i) v += " " + std::to_string(Rd32(base, items + i * kItemSize));
    REXLOG_INFO("[svr2011] cae lists: category {} ({:08X}): {} rows -{}", category, list, n, v);
  }
  {
    std::lock_guard lock(g_mutex);  // (list objects are reused: a list filled with another category isn't one)
    if (kind == -1) g_motion_lists.erase(list);
    else g_motion_lists.insert(list);
    if (scene < 0 && category == 14) g_light_lists.insert(list);
    else g_light_lists.erase(list);
  }
  if (kind == -1) return;
  std::vector<std::pair<uint32_t, std::string>> rows;
  for (const auto& e : ModEntrances()) {
    uint32_t value;
    if (kind >= 0) {
      if (!(e.parts >> kind & 1)) continue;
      value = uint32_t(10000 + 1000 * kind + e.number);
    } else {
      if ((kind == -3) != TeamNumber(e.number)) continue;
      value = uint32_t(e.number);
    }
    if (IndexOf(base, list, value) >= 0) continue;  // (a mod replacing a listed entrance: its own row)
    rows.emplace_back(value, "MOD: " + e.label);
  }
  // Every superstar mod's own entrance (manifest entrance= / attire<N>_entrance=), named after it, even when its
  // number is one the list has (most mods use a game entrance: Jeff Hardy's is 535, listed as Matt Hardy's - only the
  // mods carrying entrance scripts of their own had a row, 29 Oct) or one another mod uses too.
  for (const auto& m : svr2011::SuperstarModEntrances())
    for (int n : m.numbers) {
      uint32_t value;
      if (kind >= 0) {  // (a scene part: when there is one - the game's, or a pack's)
        value = uint32_t(10000 + 1000 * kind + n);
        if (IndexOf(base, list, value) < 0 && !EntranceScriptExists(int(value))) continue;
      } else {
        if ((kind == -3) != TeamNumber(n)) continue;
        value = uint32_t(n);
      }
      const std::string label = "MOD: " + m.name;
      if (std::find(rows.begin(), rows.end(), std::make_pair(value, label)) == rows.end()) rows.emplace_back(value, label);
    }
  if (rows.empty()) return;
  // (a group at the end of the list, each row "MOD: <label>": a disabled header row isn't drawn - flags 1
  // hides a row - so the prefix is the group's name)
  std::vector<std::pair<uint32_t, uint32_t>> texts;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [v, l] : rows) texts.emplace_back(v, Text(l));
  }
  for (const auto& [v, t] : texts)
    if (t) AddRow(ctx, base, list, v, t, false);
  static int logged = 0;
  if (logged++ < 24)
    REXLOG_INFO("[svr2011] cae mods: list category {} ({:08X}): {} mod rows, {} rows in all", category, list, texts.size(),
                Rd32(base, list));
}

void CaeNotePick(uint32_t id) { g_picked = id; }

void CaeLoadingEntrance(bool on) { t_loading = on, t_scene_default = false; }

int CaePreferredRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value) {
  // (a number several rows have - a game entrance and the mods using it: the superstar being edited keeps his own row,
  // Jeff Hardy "MOD: Jeff Hardy", not the first row with 535, Matt Hardy's)
  static const bool look_log = std::getenv("SVR2011_TEST_CAE_LOOKUP") != nullptr;  // (test aid: each lookup)
  if (look_log && value >= 100 && value < 0xFFFF) {
    bool listed;
    {
      std::lock_guard lock(g_mutex);
      listed = g_motion_lists.count(list) != 0;
    }
    REXLOG_INFO("[svr2011] cae lookup: list {:08X} ({} rows, motion list {}) value {} picked {:X}", list, Rd32(base, list),
                listed, value, g_picked);
  }
  if (!g_memory || !list || !g_picked) return -1;
  bool motion, light;
  {
    std::lock_guard lock(g_mutex);
    motion = g_motion_lists.count(list) != 0, light = g_light_lists.count(list) != 0;
  }
  // The full editor loading the edited mod's entrance (ADVANCED's scenes): a scene still on the mod's own parts - his
  // entrance numbers, or his base's parts that a mod on a game entrance keeps (Jeff Hardy: Matt Hardy's 10112...) -
  // and that scene's lighting read the game rows' names (MATT HARDY) until a template was picked: they get a
  // "MOD: <name>" row of that value (added when the list has none). Parts the player saved differently keep theirs.
  if (t_loading && (light || (motion && value >= 10000 && value < 15000))) {
    for (const auto& m : SuperstarModEntrances()) {
      if (m.id != g_picked) continue;
      const int n = int(value % 1000);
      if (motion) t_scene_default = n == int(m.base) || std::find(m.numbers.begin(), m.numbers.end(), n) != m.numbers.end();
      else if (!std::exchange(t_scene_default, false)) return -1;
      if (motion && !t_scene_default) return -1;
      uint32_t text;
      {
        std::lock_guard lock(g_mutex);
        text = Text("MOD: " + m.name);
      }
      if (!text) return -1;
      const uint32_t count = Rd32(base, list), items = Rd32(base, list + 8);
      for (uint32_t i = 0; i < count && items; ++i)
        if (Rd32(base, items + i * kItemSize) == value && Rd32(base, items + i * kItemSize + 12) == text) return int(i);
      return AddRow(ctx, base, list, value, text, false);
    }
    return -1;
  }
  if (!motion) return -1;
  const uint32_t n = value >= 10000 ? value % 1000 : value;
  for (const auto& m : SuperstarModEntrances()) {
    if (m.id != g_picked || std::find(m.numbers.begin(), m.numbers.end(), int(n)) == m.numbers.end()) continue;
    uint32_t text = 0;
    {
      std::lock_guard lock(g_mutex);
      const auto it = g_text.find("MOD: " + m.name);
      if (it == g_text.end()) return -1;
      text = it->second;
    }
    const uint32_t count = Rd32(base, list), items = Rd32(base, list + 8);
    for (uint32_t i = 0; i < count && items; ++i)
      if (Rd32(base, items + i * kItemSize) == value && Rd32(base, items + i * kItemSize + 12) == text) return int(i);
  }
  return -1;
}

int CaeMissingRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value) {
  if (!g_memory || !list || value == 0 || value >= 0xFFFF || value == kHeaderValue) return -1;
  // Any stored number the list doesn't have would become row 0 (Alicia Fox) and be saved so: it keeps
  // a row of its own - a mod's (its label), a script the list leaves out ("ENTRANCE n"), or one whose
  // mod is gone ("MISSING MOD (n)": the match then falls back, EntranceFallback).
  std::string label;
  bool missing = false;
  {
    std::lock_guard lock(g_mutex);
    if (!g_motion_lists.count(list)) return -1;
  }
  const uint32_t own_n = value >= 10000 ? value % 1000 : value;
  for (const auto& e : ModEntrances())
    if (uint32_t(e.number) == own_n) label = "MOD: " + e.label;
  if (label.empty()) {
    missing = own_n >= 100 && !EntranceScriptExists(int(value));  // (below 100: DLC entrances, in DLC pacs)
    label = missing ? "MISSING MOD (" + std::to_string(value) + ")" : "ENTRANCE " + std::to_string(value);
  }
  uint32_t text = 0;
  {
    std::lock_guard lock(g_mutex);
    text = Text(label);
  }
  if (!text) return -1;
  const int row = AddRow(ctx, base, list, value, text, false);
  static int logged = 0;
  if (logged++ < 32)
    REXLOG_INFO("[svr2011] cae mods: stored entrance motion {} not in the list {:08X} ({} rows): row {} \"{}\"{}", value,
                list, Rd32(base, list), row, label, missing ? " (no script: mod removed?)" : "");
  return row;
}

uint32_t EntranceFallback(uint8_t* base, uint32_t mgr, uint32_t idx, int part, uint32_t n) {
  if (!n || n >= 0xFFFF || idx >= 16) return n;
  const uint32_t own_n = n >= 10000 ? n % 1000 : n;
  if (own_n < 100 || EntranceScriptExists(int(n))) return n;  // (DLC 50-57 live in DLC pacs)
  const uint32_t S = mgr + 36 + idx * 280;
  const uint32_t own = Rd16(base, S + 4);  // (the profile's own entrance: the default copy, E+0x08)
  std::vector<uint32_t> cands;
  if (n >= 10000) {
    const uint32_t k = (n - 10000) / 1000;
    cands = {10000 + 1000 * k + own, 10507 + 1000 * k, 10540 + 1000 * k};  // (own, then ENEX/BIN's defaults)
  } else {
    cands = {own, Rd16(base, S + 16)};
  }
  for (const uint32_t c : cands) {
    if (c && c != n && EntranceScriptExists(int(c))) {
      static int logged = 0;
      if (logged++ < 32)
        REXLOG_INFO("[svr2011] cae mods: entrance {} (slot {}, part {}) has no script (mod removed?): {} instead", n, idx,
                    part, c);
      return c;
    }
  }
  return n;
}

}  // namespace svr2011

// ADVANCED CREATION's SELECT TEMPLATE lists (one per scene, after a NONE row) are filled by
// sub_8287E0F0(list, r4, table, scene 0-4) - sub_8287DFD0's twin that leaves out the locked rows, from another table
// (its categories are the scenes): they get the MOD rows too.
// A SELECT TEMPLATE pick looks its template record up by the row's value (sub_82883A68(table, scene, value) -> a
// 16-byte record: +4 the motion, then the rest of the scene - pyro, lighting... - for the editor to copy in). A mod's
// number has none, and the null record crashed the game (MOD: MANKIND, 29 Oct): it gets a copy of the scene's first
// record (the game's first template) with its own motion.
REX_EXTERN(__imp__sub_82883A68);
REX_HOOK_RAW(sub_82883A68) {
  const auto in = ctx;
  __imp__sub_82883A68(ctx, base);
  const uint32_t value = in.r5.u32;
  if (ctx.r3.u32 || value < 10000 || value >= 15000 || !g_memory) return;
  const int n = int(value % 1000);
  bool mod = false;
  for (const auto& e : svr2011::ModEntrances()) mod |= e.number == n;
  for (const auto& m : svr2011::SuperstarModEntrances())
    mod |= std::find(m.numbers.begin(), m.numbers.end(), n) != m.numbers.end();
  if (!mod) return;
  ctx = in;
  ctx.r5.u64 = 0;  // (sub_828839D8(table, scene, index): the scene's first template)
  sub_828839D8(ctx, base);
  const uint32_t first = ctx.r3.u32;
  ctx = in;
  if (!first) return;
  static std::map<uint32_t, uint32_t> made;  // (value -> the guest copy: made once)
  uint32_t& rec = made[value];
  if (!rec && (rec = g_memory->SystemHeapAlloc(16)) != 0) {
    std::memcpy(base + rec, base + first, 16);
    base[rec + 4] = uint8_t(value >> 8), base[rec + 5] = uint8_t(value);
    REXLOG_INFO("[svr2011] cae mods: template {} is a mod's - a record made from the scene's first", value);
  }
  ctx.r3.u64 = rec;
}

REX_EXTERN(__imp__sub_8287E0F0);
REX_HOOK_RAW(sub_8287E0F0) {
  const uint32_t list = ctx.r3.u32, scene = ctx.r6.u32;
  __imp__sub_8287E0F0(ctx, base);
  const auto r3 = ctx.r3;
  if (scene < 5) svr2011::CaeModRows(ctx, base, list, scene, int(scene));
  ctx.r3 = r3;
}
