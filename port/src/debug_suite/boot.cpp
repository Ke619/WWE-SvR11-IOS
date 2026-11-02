// WWE SmackDown vs. Raw 2011 - the debug suite's direct boot (debug_suite.h: dbg_boot).
//
// dbg_boot=match: from launch straight into the match dbg_match asks for (dbg_boot=<page>: a menu page, below). The intros and the practice ring are
// skipped (fast_start.cpp's START pulses, on the Start Screen too), and as soon as the main menu is up the match
// set-up is filled and committed as the exhibition flow's sub_82736020 does after its select screens, and the
// next scene (1, the exhibition match) asked for - the game tears its menus down and loads the match itself
// (sub_82440328 -> the front end's fade -> menu+1864 -> sub_82452DE0 -> sub_82575250 -> sub_82197C40).
//
// The match set-up (*0x82EDEB48): sub_82594990 mode (0 exhibition), sub_828BD278 rule (all 30 slots cleared,
// the profile's match options), sub_828BD1F0 arena, per person sub_828BB290 (id, pad or -1 for a CPU, attire
// 0-3), sub_828B9D70 kind, sub_828B9D80 team, sub_828B9E30 / sub_828B9E40; sub_828BB418 the title on the line;
// game state *0x82E3DDFC +16 = 1; sub_828BC5E8 the commit to the live record; sub_828C3EA8; sub_82574050(live
// +288).
//
// Tried and dropped: not building the practice ring (sub_8258A870, from the Start Screen's vtable[15] once
// the saves are loaded) - it runs before the Start Screen shows, and without it the game stays on NOW LOADING.
//
// Not done (the select screens' work): the port's match-type extras hung on sub_827374A0 (match_types.cpp:
// runtime rules, MYSTERY OPPONENT ...), CAW people's fighter entries, the exhibition flow object (after the match
// its menus may need it - a dbg run quits before).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "arena_mods.h"
#include "debug_suite/debug_suite.h"
#include "script_input.h"
#include "../../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_STRING(dbg_boot, "", "Debug",
                      "SVRDBG: start straight in: match (dbg_match's), a page (caw, threads, cae, options, "
                      "match_creator, universe ...), options:<ROW> or menu:<A>B>C>");
REXCVAR_DEFINE_STRING(dbg_match, "", "Debug",
                      "SVRDBG: the match for dbg_boot=match: people=EDGE,KANE attires=1,1 arena=1 rule=0 cpu=all");
REXCVAR_DEFINE_BOOL(dbg_entrances, false, "Debug", "SVRDBG: a booted match plays its entrances");
REXCVAR_DECLARE(bool, skip_intros);
REXCVAR_DECLARE(bool, skip_training);

namespace svr2011::dbg {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSetup = 0x82EDEB48, kMenu = 0x82E3DB68, kGame = 0x82E3DDFC, kLive = 0x82E3DE00;
constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260, kOwnId = 32, kName = 34;
constexpr uint32_t kSelectable = 221;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
using svrfmt::Rd32, svrfmt::Wr32;

struct Match {
  std::vector<std::string> people;
  std::vector<int> attires;  // 1-4
  int arena = 1, rule = 0;
  bool all_cpu = false;
};

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// "people=EDGE,KANE attires=1,1 arena=1 rule=0 cpu=all" (a value runs to the next " key=": names have spaces;
// rule in hex as SVR2011_TEST_RULE)
Match Parse(const std::string& s) {
  Match m;
  size_t at = 0;
  while (at < s.size()) {
    const size_t eq = s.find('=', at);
    if (eq == std::string::npos) break;
    std::string key = s.substr(at, eq - at);
    while (!key.empty() && key.front() == ' ') key.erase(0, 1);
    size_t end = s.size();
    for (size_t next = s.find('=', eq + 1); next != std::string::npos; next = s.find('=', next + 1))
      if (const size_t space = s.rfind(' ', next); space != std::string::npos && space > eq) {
        end = space;
        break;
      }
    const std::string value = s.substr(eq + 1, end - eq - 1);
    std::stringstream list(value);
    std::string item;
    if (key == "people") {
      while (std::getline(list, item, ',') && m.people.size() < 6) {
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) m.people.push_back(item);
      }
    } else if (key == "attires") {
      while (std::getline(list, item, ',')) m.attires.push_back(std::clamp(std::atoi(item.c_str()), 1, 4));
    } else if (key == "arena") {
      m.arena = std::atoi(value.c_str());
    } else if (key == "rule") {
      m.rule = int(std::strtol(value.c_str(), nullptr, 16));
    } else if (key == "cpu") {
      m.all_cpu = value == "all";
    } else {
      REXLOG_WARN("SVRDBG: boot: unknown dbg_match setting '{}'", key);
    }
    at = end;
  }
  return m;
}

const uint8_t* Record(const uint8_t* base, uint32_t id) {
  if (id >= 1000) return nullptr;
  const uint32_t index = Rd16(base + kIdToIndex + id * 2);
  if (index >= 1000) return nullptr;
  const uint8_t* rec = base + kRecords + index * kRecordSize;
  return Rd16(rec + kOwnId) == id ? rec : nullptr;
}

// A roster name (as the "match:" lines print it) or id -> the superstar id (0: none). As test_match.cpp.
uint32_t FindSuperstar(const uint8_t* base, const std::string& want) {
  // (CAW<N>: the Created Superstar in save slot N - character id N, below 50)
  if (want.size() > 3 && Upper(want.substr(0, 3)) == "CAW" && std::isdigit(static_cast<unsigned char>(want[3]))) {
    const uint32_t slot = uint32_t(std::atoi(want.c_str() + 3));
    return slot < 50 ? slot : ~0u;
  }
  if (!want.empty() && std::isdigit(static_cast<unsigned char>(want[0]))) {
    const uint32_t id = uint32_t(std::atoi(want.c_str()));
    return Record(base, id) ? id : 0;
  }
  const std::string w = Upper(want);
  uint32_t hidden = 0;
  for (uint32_t id = 1; id < 1000; ++id)
    if (const uint8_t* rec = Record(base, id); rec && Upper(reinterpret_cast<const char*>(rec + kName)) == w) {
      if (rec[kSelectable] == 1) return id;
      if (!hidden) hidden = id;
    }
  return hidden;
}

// A guest call: r3.. = args.
template <typename F>
void Call(PPCContext& ctx, uint8_t* base, F fn, std::initializer_list<int64_t> args) {
  PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (const int64_t a : args) r[i++]->s64 = a;
  fn(ctx, base);
}

// A Created Superstar person: what the select screen's pick commit (sub_82462650) does for one (character id
// below 50, sub_828B6CB0) - the fighter entry (sub_8257C280(*0x82EC1A68, person): 44 bytes, reset by
// sub_823BF228; +4 person, +5 0 (2 a manager), +28 1 and +32 the attire (chosen), +12 the person record
// (sub_828B5E28 + sub_828B5A18 on the roster *0x82EDEA88), +0 the attire code (sub_828B8190), +8
// sub_825743A0(person)) and its CAS data: a 0x2DC70-byte buffer from the CAS heap (sub_8269A178; the current one
// sub_8269A478, set with sub_8269AC60), sub_828CDBC8(buf + 576), 348 bytes at +0x2DB14 cleared, then built from
// the CAW record (sub_828CE8D0(buf, sub_828B6950(id))); +16 = buf.
void SetUpCaw(PPCContext& ctx, uint8_t* base, uint32_t person, uint32_t id, uint32_t attire) {
  constexpr uint32_t kFighters = 0x82EC1A68, kRoster = 0x82EDEA88, kCasSize = 0x2DC70;
  Call(ctx, base, sub_8257C280, {Rd32(base + kFighters), person});
  const uint32_t e = ctx.r3.u32;
  if (!e) return;
  Call(ctx, base, sub_822AF098, {e});
  Call(ctx, base, sub_823BF228, {e});
  base[e + 5] = 0, base[e + 4] = uint8_t(person);
  Wr32(base + e + 28, 1), base[e + 32] = uint8_t(attire);
  Call(ctx, base, sub_828B5E28, {Rd32(base + kRoster), person, id, 0});
  Call(ctx, base, sub_828B5A18, {Rd32(base + kRoster), person});
  Wr32(base + e + 12, ctx.r3.u32);
  Call(ctx, base, sub_828B8190, {id, attire, 1});
  Wr32(base + e + 0, ctx.r3.u32);
  if (const uint32_t old = Rd32(base + e + 16)) Call(ctx, base, sub_8269B2D0, {old, 1});
  Call(ctx, base, sub_8269A178, {});
  const uint32_t cas_heap = ctx.r3.u32;
  Call(ctx, base, sub_8269A478, {});
  const uint32_t heap = ctx.r3.u32;
  Call(ctx, base, sub_8269AC60, {cas_heap});
  Call(ctx, base, sub_82150000, {kCasSize});
  const uint32_t buf = ctx.r3.u32;
  if (buf) {
    Call(ctx, base, sub_828CDBC8, {buf + 576});
    Call(ctx, base, sub_82953D58, {buf + 0x2DB14, 348});
    // (built from the CAW record: the select screen's per-person copy, sub_82739920, reads select-screen
    // state a boot doesn't have - a null read)
    Call(ctx, base, sub_828B6950, {id});
    Call(ctx, base, sub_828CE8D0, {buf, ctx.r3.u32});
  }
  Wr32(base + e + 16, buf);
  Call(ctx, base, sub_8269AC60, {heap});
  Call(ctx, base, sub_825743A0, {person});
  Wr32(base + e + 8, ctx.r3.u32);
  REXLOG_INFO("SVRDBG: boot: person {} is Created Superstar {} (CAS data {})", person, id, buf ? "set" : "MISSING");
}

bool g_done = false;
Clock::time_point g_menu_since{};
std::atomic<bool> g_booted{false}, g_entrances{false};
Clock::time_point g_entrances_since{};

void StartMatch(PPCContext& ctx, uint8_t* base) {
  const Match m = Parse(REXCVAR_GET(dbg_match));
  const uint32_t setup = Rd32(base + kSetup), menu = Rd32(base + kMenu);
  if (!setup || !menu) return;
  std::vector<uint32_t> ids;
  std::string line;
  for (const std::string& who : m.people) {
    const uint32_t id = FindSuperstar(base, who);
    if (id == ~0u || (!id && Upper(who) != "CAW0")) {
      REXLOG_ERROR("SVRDBG: boot: no superstar '{}' - not booted", who);
      g_done = true;
      return;
    }
    ids.push_back(id);
    line += fmt::format("{}{} ({})", line.empty() ? "" : ", ", who, id);
  }
  if (ids.size() < 2) {
    REXLOG_ERROR("SVRDBG: boot: dbg_match needs people=<two or more> - not booted");
    g_done = true;
    return;
  }
  const int arena = svr2011::ArenaNumberFor(m.arena);
  const PPCContext saved = ctx;
  Call(ctx, base, sub_82594990, {setup, 0});        // mode: exhibition
  Call(ctx, base, sub_828BD278, {setup, m.rule, 0});  // rule (slots cleared, the profile's options)
  Call(ctx, base, sub_828BD1F0, {setup, arena < 0 ? 300 : arena, 0});
  for (uint32_t i = 0; i < ids.size(); ++i) {
    const int attire = (i < m.attires.size() ? m.attires[i] : 1) - 1;
    const int pad = i == 0 && !m.all_cpu ? 0 : -1;
    Call(ctx, base, sub_828BB290, {setup, i, ids[i], pad, attire, 2, 1, 1});
    Call(ctx, base, sub_828B9D70, {setup, i, 0});  // a wrestler
    Call(ctx, base, sub_828B9D80, {setup, i, i});  // a team of its own
    Call(ctx, base, sub_828B9E30, {setup, i, 0});
    Call(ctx, base, sub_828B9E40, {setup, i, -1});
    if (ids[i] < 50) SetUpCaw(ctx, base, i, ids[i], uint32_t(attire));
  }
  Call(ctx, base, sub_828BB418, {setup});
  if (const uint32_t game = Rd32(base + kGame)) Wr32(base + game + 16, 1);
  Call(ctx, base, sub_828BC5E8, {setup});  // the commit
  Call(ctx, base, sub_828C3EA8, {});
  Call(ctx, base, sub_82574050, {kLive + 288});
  Call(ctx, base, sub_82440328, {menu, 1});  // the next scene: the exhibition match
  ctx = saved;
  g_done = true;
  g_booted = true;
  REXLOG_INFO("SVRDBG: boot: match - {}; rule {:02X}, arena {}{}", line, m.rule, m.arena, m.all_cpu ? ", all CPU" : "");
}

// dbg_boot=<page>: the menu path to a page, chosen with the game's own calls (as its input handler
// sub_82448260 makes them for a cursor move and A - no pad input), the row found as the menu_hooks' "menu
// <LABEL>" steps find it; one entry each time the menu has settled on a new group. A path is labels joined by '>' (menu:CREATE MODES>CREATE A SUPERSTAR>NEW), or a
// short name below; options:<ROW> is MY WWE > OPTIONS > <ROW>.
struct Page {
  const char* name;
  const char* path;
};
constexpr Page kPages[] = {
    {"caw", "CREATE MODES>CREATE A SUPERSTAR>NEW"},
    {"create", "CREATE MODES>CREATE A SUPERSTAR>NEW"},
    {"caw_edit", "CREATE MODES>CREATE A SUPERSTAR>EDIT"},
    {"threads", "CREATE MODES>CREATE A SUPERSTAR>SUPERSTAR THREADS"},
    {"paint", "CREATE MODES>CREATE A SUPERSTAR>PAINT TOOL"},
    {"cae", "CREATE MODES>CREATE AN ENTRANCE"},
    {"finisher", "CREATE MODES>CREATE A FINISHER"},
    {"moveset", "CREATE MODES>CREATE A MOVE-SET"},
    {"story", "CREATE MODES>STORY DESIGNER"},
    {"reel", "CREATE MODES>HIGHLIGHT REEL"},
    {"options", "MY>OPTIONS"},
    {"match_creator", "MY>OPTIONS>MATCH CREATOR"},
    {"jukebox", "MY>JUKEBOX"},
    {"achievements", "MY>ACHIEVEMENTS"},
    {"superstar_management", "MY>SUPERSTAR MANAGEMENT"},
    {"universe", "UNIVERSE"},
    {"rtwm", "ROAD TO WRESTLEMANIA"},
    {"online", "ONLINE"},
    {"shop", "SHOP"},
};

std::vector<std::string> g_path;  // the labels still to choose
std::string g_page;
uint32_t g_left_group = ~0u;      // the group the last choice was made in (the next waits for another)
Clock::time_point g_group_since{};
uint32_t g_group_seen = ~0u;

bool MenuPath(const std::string& want, std::vector<std::string>* out) {
  std::string path;
  if (want.rfind("menu:", 0) == 0) path = want.substr(5);
  else if (want.rfind("options:", 0) == 0) path = "MY>OPTIONS>" + want.substr(8);
  else
    for (const Page& p : kPages)
      if (want == p.name) path = p.path;
  if (path.empty()) return false;
  std::stringstream list(path);
  std::string item;
  while (std::getline(list, item, '>')) out->push_back(item);
  return !out->empty();
}

void MenuFrame(PPCContext& ctx, uint8_t* base) {
  uint32_t group = 0, row = 0;
  if (!ScriptMenuState(&group, &row)) return;
  if (group != g_group_seen) {
    if (g_left_group != ~0u) REXLOG_INFO("SVRDBG: boot: menu group {:X}", group);
    g_group_seen = group, g_group_since = Clock::now();
  }
  // (a group of the menu - not a screen id - other than the last choice's, settled a moment)
  if (group >= 1000 || group == g_left_group || Clock::now() - g_group_since < std::chrono::milliseconds(1000)) return;
  if (g_left_group == ~0u && group != 1) return;  // (the first choice in the main menu - not the title's group 0)
  const uint32_t menu = Rd32(base + kMenu);
  if (!menu) return;
  int rows = 0;
  const std::string& label = g_path.front();
  const int at = ScriptMenuRow(group, label, &rows);
  if (at < 0) {
    REXLOG_ERROR("SVRDBG: boot: no '{}' in menu group {:X} ({}) - stopped", label, group, ScriptMenuTexts(group));
    g_done = true;
    return;
  }
  // As the menu's own input (sub_82448260): a cursor move - +360 the row, +384 what it leads to
  // (sub_82445640), the redraws - then A: sub_82447210(menu, 1) and the two calls after it.
  const PPCContext saved = ctx;
  Wr32(base + menu + 360, uint32_t(at));
  Call(ctx, base, sub_82445640, {menu, group < 1000 ? int64_t(group) : -1, at});
  Wr32(base + menu + 384, ctx.r3.u32);
  for (auto fn : {sub_82446B90, sub_82446E30, sub_82447168, sub_824463F8, sub_82445D80}) Call(ctx, base, fn, {menu});
  Wr32(base + menu + 1796, 0);  // (who pressed A: player 1 - its checks, sub_824418B8 / sub_82441A80, read it)
  Call(ctx, base, sub_82447210, {menu, 1});
  Call(ctx, base, sub_82446E30, {menu});
  Call(ctx, base, sub_82447168, {menu});
  ctx = saved;
  REXLOG_INFO("SVRDBG: boot: chose {} (group {:X} row {})", label, group, at);
  g_left_group = group;
  g_path.erase(g_path.begin());
  if (g_path.empty()) {
    g_done = true;
    REXLOG_INFO("SVRDBG: boot: page {} open", g_page);
  }
}

}  // namespace

bool BootWanted() { return !g_done && !REXCVAR_GET(dbg_boot).empty(); }

void InstallBoot() {
  const std::string want = REXCVAR_GET(dbg_boot);
  if (want.empty()) return;
  if (want != "match") {
    if (!MenuPath(want, &g_path)) {
      std::string names;
      for (const Page& p : kPages) names += std::string(names.empty() ? "" : ", ") + p.name;
      REXLOG_ERROR("SVRDBG: boot: dbg_boot={} isn't known (match, {}, options:<ROW>, menu:<A>B>C>)", want, names);
      g_done = true;
      return;
    }
    g_page = want;
  }
  REXCVAR_SET(skip_intros, true);
  REXCVAR_SET(skip_training, true);
  REXLOG_INFO("SVRDBG: boot: {} ({})", want, want == "match" ? REXCVAR_GET(dbg_match) : want);
}

void BootFrame(PPCContext& ctx, uint8_t* base) {
  if (!BootWanted()) return;
  if (!g_path.empty()) return MenuFrame(ctx, base);
  uint32_t group = 0, row = 0;
  if (!ScriptMenuState(&group, &row) || group != 1) {
    g_menu_since = {};
    return;
  }
  // (the main menu up and settled a moment)
  if (g_menu_since == Clock::time_point{}) g_menu_since = Clock::now();
  if (Clock::now() - g_menu_since < std::chrono::milliseconds(300)) return;
  StartMatch(ctx, base);
}

void OnEntrances(bool on) {
  if (on && !g_entrances.load()) g_entrances_since = Clock::now();
  g_entrances = on;
}

void BootInput(uint8_t* base, uint32_t state) {
  if (!state || !g_booted.load() || !g_entrances.load() || REXCVAR_GET(dbg_entrances)) return;
  // A for 60 ms, then 60 ms without, the packet number moving on (as fast_start.cpp's START)
  constexpr uint16_t kA = 0x1000;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_entrances_since).count();
  if ((ms / 60) % 2) return;
  uint8_t* buttons = base + state + 4;
  const uint16_t b = uint16_t((buttons[0] << 8 | buttons[1]) | kA);
  buttons[0] = uint8_t(b >> 8), buttons[1] = uint8_t(b);
  Wr32(base + state, Rd32(base + state) + 1);
}

}  // namespace svr2011::dbg
