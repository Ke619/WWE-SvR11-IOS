// More exhibition match types - see match_types.h.
//
// Game side (docs/MATCH_TYPES_RESEARCH.md): a match is a rule record (misc.pac
// RUL, 100 bytes, ids 0x00-0x76); a menu row (menu.pac MFLO/0000) names one by
// its id (+0x5C) and becomes group 2000 + id, which sub_824402A0(screen)
// turns back into the id when the match is set up.
//
// Menu rows are added as the game reads the menu table: sub_82BAA6E8(menus,
// table, size) copies each shown 0x74-byte record of MFLO/0000 into its own
// structures (sized by the table's counts, header +4 total and +8 shown) and
// the caller frees the table afterwards. The hook hands it a copy with the
// new rows inserted after their anchors (a group's rows are consecutive, in
// display order; flag 2 at +0x3C marks the group's last row; a submenu finds
// its rows by their parent node, +0x38). No file size limit applies.
#include "big_roster.h"
#include "match_diag.h"
#include "match_types.h"
#include "frame_rate.h"
#include "mystery_opponent.h"
#include "no_mods.h"
#include "pad_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <random>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "arena_mods.h"
#include "match_hud.h"
#include "online_chat.h"
#include "online_rules.h"
#include "p2p.h"
#include "script_input.h"
#include "weapons_page.h"
#include "generated/default/svr2011_init.h"
#include "free_cam.h"
#include "crowd_fight.h"
#include "match_box.h"
#include "match_record.h"
#include "thread_cpu.h"
#include "util.h"
#include "../modmaker/svrfmt/bytes.h"

namespace svr2011 {
std::atomic<uint32_t> g_match_rule{~0u}, g_match_arena{~0u};  // (the last match placed: LastMatch)
void LastMatch(uint32_t* rule, uint32_t* arena) { *rule = g_match_rule.load(), *arena = g_match_arena.load(); }
std::atomic<uint32_t> g_match_placements{0};
uint32_t MatchPlacements() { return g_match_placements.load(); }
// (the whole backstage 0x19 / 0x1A, the 1 on 1 rooms 0x1B.. and the 2 on 2 rooms 0x70-0x76: arena 78)
bool BackstageRule(uint32_t rule) { return (rule >= 0x19 && rule <= 0x21) || (rule >= 0x70 && rule <= 0x76); }
}  // namespace svr2011

namespace {

constexpr uint32_t kRec = 0x74, kFirst = 0x28;
constexpr uint32_t kScreenMatch = 0x7D0;
constexpr uint32_t kNoText = 0x05F5E0FF;

rex::memory::Memory* g_memory = nullptr;

using svrfmt::Rd32, svrfmt::RdF, svrfmt::Wr32, svrfmt::WrF;
// A row to add: after the row labelled `anchor` in menu group `group`, a copy
// of it with this label, description and rule (its match type mod: `mod`).
struct Row {
  uint32_t anchor, group;
  uint32_t label, description, rule;
  const char* mod;
};

// PLAY -> ... (groups: 06 ONE ON ONE, 07 TWO ON TWO, 08 TRIPLE THREAT,
// 09 FATAL-4-WAY, 0A 6-MAN, 0C ROYAL RUMBLE). All the game's own rules and
// names: Falls Count Anywhere (the labels of the rows cut from the menu),
// 15- and 25-man Royal Rumble, Lumberjack.
const Row kRows[] = {
    {0xA053, 0x06, 0xA054, kNoText, 0x2B, "falls_count_anywhere"},  // ONE ON ONE: after FIRST BLOOD
    {0xA061, 0x07, 0xA062, kNoText, 0x2E, "falls_count_anywhere"},  // TWO ON TWO: after MIXED TAG
    {0xA06A, 0x08, 0xA06B, kNoText, 0x2F, "falls_count_anywhere"},  // TRIPLE THREAT: after NORMAL
    {0xA072, 0x09, 0xA073, kNoText, 0x30, "falls_count_anywhere"},  // FATAL-4-WAY: after NORMAL
    // FATAL-4-WAY: CHAMPIONSHIP SCRAMBLE (the game's own rule 0x26 and label
    // A038, cut from PLAY - its row there only opens submenus): 5 people, 2
    // start and one more comes in each minute; a fall makes an interim
    // champion and the match goes on; at the bell (5:00) the last one wins.
    {0xA072, 0x09, 0xA038, kNoText, 0x26, "championship_scramble"},
    {0xA08C, 0x0C, 0x5313, kNoText, 0x15, "royal_rumble_15_25"},  // ROYAL RUMBLE: 15-MAN after 10-MAN
    {0xA08D, 0x0C, 0x5314, kNoText, 0x17, "royal_rumble_15_25"},  // 25-MAN after 20-MAN
    {0xA07F, 0x0A, 0x0052, kNoText, 0x55, "lumberjack"},          // 6-MAN: LUMBERJACK after ARMAGEDDON
    // BACKSTAGE (1 on 1, 2 on 2): FREE-ROAMING BACKSTAGE after PARKING LOT -
    // the whole backstage (rules without a named area, see below).
    {0xA0DD, 0x12, 0x9C90, kNoText, 0x19, "free_roaming_backstage"},
    {0xA0DD, 0x14, 0x9C90, kNoText, 0x1A, "free_roaming_backstage"},
};
constexpr uint32_t kFreeRoamLabel = 0x9C90;
constexpr uint32_t kWholeBackstage = 0x19, kWholeBackstage2 = 0x1A;  // 1 on 1, 2 on 2
// The match being played is a free-roaming backstage one (set at match set-up).
bool g_free_roam = false;

// BACKSTAGE submenus (like ONE ON ONE's and TWO ON TWO's) for TRIPLE THREAT,
// FATAL-4-WAY and 6-MAN: the 7 areas of TWO ON TWO -> BACKSTAGE (menu group
// 0x14, rules 0x70-0x76: one per area), played with 3, 4 or 6 people (the
// rule record is reshaped for the match, see below).
struct Backstage {
  uint32_t anchor, group;   // after this row
  uint32_t people;
  uint32_t like;            // a rule with these people and teams
  uint32_t menu_group = 0;  // the areas' group, given when the rows are added
};
Backstage g_backstage[] = {
    {0xA070, 0x08, 3, 0x0D},  // TRIPLE THREAT: after TLC (like NORMAL)
    {0xA079, 0x09, 4, 0x0E},  // FATAL-4-WAY: after TLC (like NORMAL)
    {0xA09C, 0x0A, 6, 0x23},  // 6-MAN: after LADDER (like BATTLE ROYAL)
};
constexpr uint32_t kBackstageSubmenu = 0xA05D, kBackstageSubmenuGroup = 0x07;  // its row
constexpr uint32_t kBackstageAreasGroup = 0x14;                               // its areas
constexpr uint32_t kBackstageFirstRule = 0x70, kBackstageLastRule = 0x76;

// Backstage mods with their own row (arena_mods.h BackstageRows: e.g. the SvR
// 2008 parking lot): in BACKSTAGE (1 on 1, group 0x12), after the row of
// their room's rule (0x1B + room), a copy labelled with the mod's row= text
// (string ids kOwnRowLabel + i, see MatchTypeString). The row's place in its
// group is remembered: picking it plays that room's rule with the mod's bg78.
constexpr uint32_t kBackstage1v1Group = 0x12, kBackstage1v1FirstRule = 0x1B;
constexpr uint32_t kOwnRowLabel = 0x0FA0B000;
int g_own_row_index[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int g_pending_own_row = -1;
std::vector<uint32_t> g_own_row_text;  // guest strings

// WEAPONS EVERYWHERE: after each EXTREME RULES row (rules 0x4D-0x50, no
// disqualification; the Extreme Rules weapons are loaded), a copy labelled
// kWeaponsLabel. Its match is that Extreme Rules match with weapons already
// lying in and around the ring at the bell (see "Weapons everywhere" below).
constexpr uint32_t kWeaponsLabel = 0x0FA0B100, kWeaponsText = 0x0FA0B101;  // (label, description)
// MY WWE -> OPTIONS: its WEAPONS row (game_files.cpp) and REPORT A PROBLEM after it (menu_hooks.cpp gives its text).
constexpr uint32_t kOptionsGroup = 0x11, kOptionsWeaponsLabel = 0x0FA0B201, kReportLabel = 0x0FA0B202;
#if defined(__SWITCH__)
constexpr uint32_t kExitLabel = 0xAFC1, kMainGroup = 0x01;  // (the main menu's EXIT: game_files.cpp)
#endif
constexpr uint32_t kExtremeFirst = 0x4D, kExtremeLast = 0x50;
constexpr uint32_t kFullGroup = 0x06;  // ONE ON ONE: 14 rows, the most a list shows
struct WeaponsRow {
  uint32_t group;
  uint32_t index;  // its place in the group
};
std::vector<WeaponsRow> g_weapons_rows;
// HELL IN A CELL: START ON THE ROOF - a third row of each cell submenu (match
// type mod hiac_roof_start): everyone is put on the cell's roof at the bell
// (MatchTypesUpdate). The roof is the area +444 10, the height y -60.5 (y is
// down) of the real position vector *(char +304) - the shove-off, the
// break-through and climbing down work from there as after a climb; no pins
// count up there (the game's own rule), so the fight comes down to finish.
constexpr uint32_t kRoofLabel = 0x0FA0B120, kRoofText = 0x0FA0B121;
std::vector<WeaponsRow> g_roof_rows;
bool g_pending_roof = false;
// CPUs left alone on the cell's roof climb down: a CPU wrestler (its match
// slot's controller, slot -8, 1, and controller port +1152 4 or more) alone up there (area +444 10, no other
// wrestler 0-5 on it) for 25 s of match frames takes the game's own
// way down - the climb-down a player starts with the action button pushed
// towards an edge (sub_8230A760: area 10, the edge check sub_821BE8F8, then
// sub_82397228(char, 1672)) - from his free state (the post-hook of its
// handler sub_82292D50). The CPUs' AI never picks it on purpose (a 4-CPU
// fatal-4-way stayed up 4 min). Only match state decides (frames, places,
// slots): the same on both machines online. Humans are never touched. Test
// aids: SVR2011_TEST_ROOF_DOWN=0 off; SVR2011_TEST_ROOF_DOWN_SECS=<s>.
constexpr uint32_t kRoofArea = 10, kClimbDown = 1672;
uint32_t g_roof_alone_since[6] = {};  // match frame + 1 since that person is alone on the roof (0: not)
// The CPU stall (PiPPiN's report, 30 Oct: a CPU Jeff Hardy rolled out and stood 50 s on the floor until counted out;
// memory svr2011-ring-stall): now and then a CPU's AI goes silent - the other one carries on - and stays so. A CPU
// wrestler (slot controller 1, port +1152 4+) on the floor outside (+444 area 1) left in one motion for
// 8 s while the referee counts (his motion 191 seen lately - only rules with a count-out play it) climbs back in:
// the game's own climb from the apron (kClimbIn). Only match state decides (motions, places, frames, slots): the same
// on every machine online. Humans are never touched. Two CPUs left in a lock-up for 10 s go on with it (kLockUpOn,
// MatchTypesUpdate).
// Test aid: SVR2011_TEST_STALL_SECS=<s> - after <s> s (both nets), and without waiting for a count.
constexpr uint32_t kRefCounting = 191, kClimbIn = 7014, kLockUp = 21100, kLockUpOn = 21201;
uint32_t g_count_seen = 0;               // match frame + 1 the referee last counted (motion 191)
uint32_t g_still_motion[6] = {}, g_still_since[6] = {};  // per person: the motion and match frame + 1 it began
bool g_roof = false;  // the match set up starts on the cell's roof
bool g_pending_weapons = false;
bool g_weapons = false;  // the match set up is a WEAPONS EVERYWHERE one
uint32_t g_weapons_text[2] = {};

// SLOBBER KNOCKER: after HANDICAP -> GAUNTLET (group 0x0B, rule 0x52), a
// copy: player 1 against an endless line of opponents, one at a time, until
// pinned (see "Slobber Knocker" below).
constexpr uint32_t kSlobberLabel = 0x0FA0B102, kSlobberText = 0x0FA0B103;
constexpr uint32_t kGauntletRow = 0xA081, kHandicapGroup = 0x0B, kGauntlet = 0x52;
constexpr int kSlobberPeople = 5;  // player 1, the opponent in the match, 3 waiting
int g_slobber_row = -1;  // its place in the group
bool g_pending_slobber = false;
bool g_slobber = false;  // the match set up is a Slobber Knocker
uint32_t g_slobber_text[2] = {};
// GAUNTLET ELIMINATOR (gauntlet_eliminator.cpp): HANDICAP rows after SLOBBER
// KNOCKER - the gauntlet 0x52 for six, each on his own: player 1 and the
// first opponent picked (the 1 on 1 select screen), four random entrants who
// come in on a timer; a pin or submission eliminates. Its EXTREME RULES row
// adds weapons lying about (6-MAN's set: the Extreme Rules option crashed
// six-person matches) and no disqualifications.
constexpr uint32_t kGeLabel = 0x0FA0B10C, kGeText = 0x0FA0B10D, kGeErLabel = 0x0FA0B10E, kGeErText = 0x0FA0B10F;
constexpr int kGePeople = 6;
int g_ge_row = -1, g_ge_er_row = -1;  // their places in the group
bool g_pending_ge = false;
bool g_ge = false;  // the match set up is a Gauntlet Eliminator
// ONLINE: the session's port rules play another rule than the session's own
// (GAUNTLET ELIMINATOR: 6-MAN 0x08 -> the gauntlet 0x52). The lobby's set-up
// (sub_827374A0) sees it, but the match's live rules are built again from
// the session's rule at its start (sub_828C48E8): there it's swapped too.
uint32_t g_online_from = ~0u, g_online_to = ~0u;
uint32_t g_ge_text[4] = {};

// ELIMINATION: after NORMAL in TRIPLE THREAT (rule 0x0D) and FATAL-4-WAY
// (0x0E), a copy whose match has the ELIMINATION rule on (as MATCH CREATOR ->
// RULES -> ELIMINATION: live option +37 = 1): a pin or give up eliminates
// that wrestler - he leaves - and the match goes on until one is left. The
// engine does it (a triple threat: one eliminated, he walked out, the match
// went on, the last fall decided it, highlights as usual). The game itself
// offers ELIMINATION only for these rules' normal, falls count anywhere and
// extreme rules versions (MRPD), not with a cage, cell, ladder, table, TLC or
// backstage - left so.
constexpr uint32_t kElimLabel = 0x0FA0B104, kElimText = 0x0FA0B105;
// THREE STAGES OF HELL (three_stages.cpp): in each EXTREME RULES submenu
// after WEAPONS EVERYWHERE (its match: the mode's normal one, kModes), and a
// row in 6-MAN and HANDICAP.
constexpr uint32_t kStagesLabel = 0x0FA0B106, kStagesText = 0x0FA0B107;
// Its submenu: one row per set of stages (three_stages.cpp kSets), labels
// kStagesSetLabel + 2 * set (+1 its description: 0x0FA0B114-0x0FA0B11F).
// Sets 3 (TABLE / TLC / LADDER), 5 (TABLE / LADDER / TABLE) and 4 (HELL IN A
// CELL) only where the mode has those matches: one on one, two on two,
// triple threat, fatal-4-way; 3 and 5 are the switch mod
// three_stages_tables_ladders.
constexpr uint32_t kStagesSetLabel = 0x0FA0B114;
struct StagesRow {
  uint32_t group, index;
  int set;
};
// A structure's match with weapons inside (WEAPONS EVERYWHERE's way, its own
// placed list): its row becomes a submenu of the match and the weapons one.
// - HELL IN A CELL: TABLES & LADDERS (one on one, two on two, triple threat,
//   fatal-4-way: 0x31-0x34) - tables, ladders and chairs inside the cell
//   (kPlacedCell). Not the 6-man cell (0x35): the Extreme Rules option that
//   loads the table and ladder models crashed 6-man matches.
// - STEEL CAGE: WEAPONS INSIDE (0x3C-0x3F) and INFERNO MATCH: WEAPONS INSIDE
//   (0x51) - in the ring (kPlacedRing): the cage stands at the ring's edge,
//   the fire around it. The models every match loads (chairs, hand weapons):
//   with the Extreme Rules option (+56: tables, ladders) the cage crashed at
//   its start (sub_824186E8 with no event data, as 6-man).
constexpr uint32_t kCellFirst = 0x31, kCellLast = 0x34;
struct StructureWeapons {
  uint32_t first, last;
  const char* mod;       // (its match type mod)
  uint32_t label, text;  // (B108/B109: mystery_opponent.h)
  const char* name;
  const char* about;
};
constexpr StructureWeapons kStructures[] = {
    {kCellFirst, kCellLast, "hiac_tables_ladders", 0x0FA0B10A, 0x0FA0B10B, "TABLES & LADDERS",
     "Hell in a Cell with tables, ladders and chairs lying inside the cell - no disqualifications."},
    {0x3C, 0x3F, "cage_weapons", 0x0FA0B110, 0x0FA0B111, "WEAPONS INSIDE",
     "A Steel Cage match with chairs and other weapons lying in the ring - no disqualifications."},
    {0x51, 0x51, "inferno_weapons", 0x0FA0B112, 0x0FA0B113, "WEAPONS INSIDE",
     "An Inferno match with chairs and other weapons lying in the ring - no disqualifications."},
};
uint32_t g_structure_text[std::size(kStructures)][2] = {};
const StructureWeapons* StructureOf(uint32_t rule) {
  for (const StructureWeapons& s : kStructures)
    if (rule >= s.first && rule <= s.last) return &s;
  return nullptr;
}
std::vector<StagesRow> g_stages_rows;
bool g_pending_stages = false;
int g_pending_set = 0;
uint32_t g_stages_set_text[2 * svr2011::kThreeStagesSets] = {};
// A set's match in a mode (its normal one): the TLC one for sets 3 and 5, the
// cell one for set 4.
uint32_t SetRule(uint32_t normal, int set) {
  constexpr uint32_t kNormals[] = {0x00, 0x03, 0x0D, 0x0E}, kTlc[] = {0x48, 0x49, 0x4A, 0x4B}, kCell[] = {0x31, 0x32, 0x33, 0x34};
  for (int i = 0; i < 4; ++i)
    if (normal == kNormals[i]) return set == 3 || set == 5 ? kTlc[i] : set == 4 ? kCell[i] : normal;
  return normal;
}
// The sets a mode's submenu offers, in row order: the three normal ones, then
// (one on one, two on two, triple threat, fatal-4-way) the table / ladder
// ones (switch mod three_stages_tables_ladders).
// The sets with a FIRST BLOOD fall (1 NORMAL / FIRST BLOOD / FINISHERS, 4 HELL IN A CELL: FIRST BLOOD / LMS /
// SUBMISSION) aren't offered: a fall won by first blood never reaches the falls judge - the game's own
// blood win starts and the match froze (report 2026-10-16). Back when that fall is handled.
std::vector<int> SetsFor(uint32_t normal) {
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("match types: THREE STAGES OF HELL sets with a FIRST BLOOD fall left out (a first blood win froze "
                "the match)");
  }
  std::vector<int> sets{0, 2};
  if (SetRule(normal, 3) == normal) return sets;
  if (svr2011::MatchTypeSwitchOn("three_stages_tables_ladders")) sets.push_back(5), sets.push_back(3);
  return sets;
}

// The modes with an EXTREME RULES row (group, its normal match: THREE STAGES
// OF HELL's first fall). Their EXTREME RULES becomes a submenu of EXTREME
// RULES, WEAPONS EVERYWHERE and THREE STAGES OF HELL (ONE ON ONE, TWO ON TWO
// and FATAL-4-WAY are full lists: 14 rows).
struct ExtremeMode {
  uint32_t group, normal;
};
constexpr ExtremeMode kExtremeModes[] = {{0x06, 0x00}, {0x07, 0x03}, {0x08, 0x0D}, {0x09, 0x0E}};
// 6-MAN and HANDICAP have no EXTREME RULES: WEAPONS EVERYWHERE and THREE
// STAGES OF HELL are rows after the anchor (the last before MATCH CREATOR),
// both on the mode's normal match - 6-MAN TAG (0x08) and ONE ON TWO TORNADO
// (0x0F); Weapons Everywhere gets the Extreme Rules option bytes (below).
struct PlainMode {
  uint32_t anchor, group, normal;
};
constexpr PlainMode kPlainModes[] = {{0xA09C, 0x0A, 0x08}, {0xA086, 0x0B, 0x0F}};
// A WEAPONS EVERYWHERE match's rule: Extreme Rules, or 6-MAN's / HANDICAP's normal one.
bool WeaponsRule(uint32_t rule) {
  if (rule >= kExtremeFirst && rule <= kExtremeLast) return true;
  if (StructureOf(rule)) return true;
  if (rule == 0x52) return true;  // (GAUNTLET ELIMINATOR: EXTREME RULES)
  for (const PlainMode& m : kPlainModes)
    if (rule == m.normal) return true;
  return false;
}
bool StagesRule(uint32_t rule) {
  for (const ExtremeMode& m : kExtremeModes)
    if (rule == m.normal || rule == SetRule(m.normal, 3) || rule == SetRule(m.normal, 4)) return true;
  for (const PlainMode& m : kPlainModes)
    if (rule == m.normal) return true;
  return false;
}
// MYSTERY OPPONENT: ONE ON ONE -> NORMAL MATCH becomes a submenu of NORMAL
// MATCH and MYSTERY OPPONENT (mystery_opponent.cpp: a normal one on one).
WeaponsRow g_mystery_row = {~0u, ~0u};
bool g_pending_mystery = false;
uint32_t g_stages_text[2] = {};
constexpr uint32_t kTripleThreat = 0x0D, kFatal4Way = 0x0E;
std::vector<WeaponsRow> g_elim_rows;  // (group, place in the group)
bool g_pending_elimination = false;
bool g_elimination = false;  // the match set up is an ELIMINATION one
uint32_t g_elim_text[2] = {};

// Developer aid: SVR2011_TEST_RULE=<hex id> plays that rule record wherever
// the game would play ONE ON ONE -> NORMAL (id 0), to try a rule in game.
int g_test_rule = -1;
// Test aid: SVR2011_TEST_MODE=<mode> - ONE ON ONE NORMAL (route match) plays
// a mode only a runtime row gives: three_stages, elimination_tt,
// elimination_f4w, weapons, slobber, lumberjack (as if its row was picked).
enum class TestMode { kNone, kThreeStages, kElimTT, kElimF4W, kWeapons, kSlobber, kLumberjack, kGauntletElim, kGauntletElimEr, kMystery, kRoof };
TestMode g_test_mode = TestMode::kNone;

// Node ids (+0x1C): the game finds a node by binary search over the table
// (sub_82BA9B48) - B goes back through the row's parent (sub_8243FC18) - so
// they must not go down along the table. A submenu row added in the middle
// of a group takes the node id of the record before it (or of the one it
// replaces): the two then share it, which B doesn't mind (it wants the
// parent's group, the same for both); its own rows, at the table's end, get
// new ids above all others. (A new top id in the middle left B doing nothing
// in the submenu after backing out of character select.)
// The menu table with the rows added (empty if it isn't the main menus).
std::vector<uint8_t> WithRows(const uint8_t* table, uint32_t size) {
  if (size < kFirst || Rd32(table) != 1) return {};
  g_weapons_rows.clear();
  g_roof_rows.clear();
  g_stages_rows.clear();
  g_elim_rows.clear();
  const uint32_t total = Rd32(table + 4), shown = Rd32(table + 8);
  if (total == 0 || total > 0x1000 || kFirst + total * kRec > size) return {};
  // Test aid: SVR2011_TEST_MENU_DUMP=<hex group> - that group's records and
  // any record of rule 0x26 (CHAMPIONSHIP SCRAMBLE), logged as read.
  if (const char* v = std::getenv("SVR2011_TEST_MENU_DUMP")) {
    const uint32_t want = uint32_t(std::strtoul(v, nullptr, 16));
    for (uint32_t i = 0; i < total; ++i) {
      const uint8_t* rec = table + kFirst + i * kRec;
      if (Rd32(rec + 0x18) == want || (Rd32(rec + 0x5C) == 0x26 && Rd32(rec + 0x54) == kScreenMatch))
        REXLOG_INFO("menu dump: #{} label {:X} text {:X} group {:X} node {:X} parent {:X} +3C {:X} +40 {:X} screen {:X} rule {:X} (shown {})",
                    i, Rd32(rec), Rd32(rec + 4), Rd32(rec + 0x18), Rd32(rec + 0x1C), Rd32(rec + 0x38), Rd32(rec + 0x3C),
                    Rd32(rec + 0x40), Rd32(rec + 0x54), Rd32(rec + 0x5C), shown);
    }
  }
  // The rows the BACKSTAGE submenus copy, and free node / group ids.
  const uint8_t* submenu = nullptr;
  std::vector<const uint8_t*> areas;
  uint32_t node = 0, group = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t* rec = table + kFirst + i * kRec;
    node = std::max(node, Rd32(rec + 0x1C) + 1);
    group = std::max(group, Rd32(rec + 0x18) + 1);
    if (Rd32(rec) == kBackstageSubmenu && Rd32(rec + 0x18) == kBackstageSubmenuGroup) submenu = rec;
    if (Rd32(rec + 0x18) == kBackstageAreasGroup && Rd32(rec + 0x54) == kScreenMatch) areas.push_back(rec);
  }
  std::vector<uint8_t> out(table, table + kFirst);
  std::vector<uint8_t> tail;  // the submenus' areas: new groups, at the end
  uint32_t added = 0;
  // (match type mods switched off: their rows aren't added)
  const bool elimination = svr2011::MatchTypeSwitchOn("elimination"), slobber = svr2011::MatchTypeSwitchOn("slobber_knocker");
  const bool mystery = svr2011::MatchTypeSwitchOn("mystery_opponent"), weapons = svr2011::MatchTypeSwitchOn("weapons_everywhere");
  const bool stages = svr2011::MatchTypeSwitchOn("three_stages_of_hell");
  bool structure_on[std::size(kStructures)];
  for (size_t s = 0; s < std::size(kStructures); ++s) structure_on[s] = svr2011::MatchTypeSwitchOn(kStructures[s].mod);
  const bool gauntlet_elim = svr2011::MatchTypeSwitchOn("gauntlet_eliminator");
  const bool roof_start = svr2011::MatchTypeSwitchOn("hiac_roof_start");
  const bool more_backstage = svr2011::MatchTypeSwitchOn("backstage_more_people");
  const bool free_roam_on = svr2011::MatchTypeSwitchOn("free_roaming_backstage");
  // THREE STAGES OF HELL's sets: rows of a submenu (its node `parent`, depth
  // word +0x14 `depth`), in group `in_group`, nodes from `first_node`, each
  // a copy of a match row of the mode (`leaf_like`: the table's first row of
  // that rule) playing the set's match.
  struct StageSets {
    uint32_t parent = 0, depth = 0, normal = 0;
  } stage_sets;
  const auto AddStageSets = [&](const StageSets& s, uint32_t in_group, uint32_t first_node) {
    const uint8_t* like = nullptr;
    for (uint32_t j = 0; j < total && !like; ++j) {
      const uint8_t* r = table + kFirst + j * kRec;
      if (Rd32(r + 0x54) == kScreenMatch && Rd32(r + 0x5C) == s.normal) like = r;
    }
    if (!like) return;
    const std::vector<int> offered = SetsFor(s.normal);
    const int sets = int(offered.size());
    for (int row = 0; row < sets; ++row) {
      const int k = offered[size_t(row)];
      std::vector<uint8_t> leaf(like, like + kRec);
      Wr32(leaf.data() + 0x00, kStagesSetLabel + uint32_t(2 * k));
      Wr32(leaf.data() + 0x04, kStagesSetLabel + uint32_t(2 * k + 1));
      Wr32(leaf.data() + 0x14, s.depth);
      Wr32(leaf.data() + 0x18, in_group);
      Wr32(leaf.data() + 0x1C, first_node + uint32_t(row));
      Wr32(leaf.data() + 0x38, s.parent);
      Wr32(leaf.data() + 0x40, 0);
      Wr32(leaf.data() + 0x3C, row + 1 == sets ? Rd32(like + 0x3C) | 2u : Rd32(like + 0x3C) & ~2u);
      Wr32(leaf.data() + 0x5C, SetRule(s.normal, k));
      g_stages_rows.push_back({in_group, uint32_t(row), k});
      tail.insert(tail.end(), leaf.begin(), leaf.end());
      ++added;
    }
    REXLOG_INFO("match types: THREE STAGES OF HELL sets (rule {:02X}): {} rows, group {:02X}", s.normal, sets, in_group);
  };
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t* rec = table + kFirst + i * kRec;
#if defined(__SWITCH__)
    // No EXIT on the main menu: a Switch game is closed from the HOME menu. (SHOP before it becomes the group's last.)
    if (Rd32(rec) == kExitLabel && Rd32(rec + 0x18) == kMainGroup) {
      uint8_t* prev = out.data() + out.size() - kRec;
      Wr32(prev + 0x3C, Rd32(prev + 0x3C) | (Rd32(rec + 0x3C) & 2u));
      --added;
      continue;
    }
#endif
    out.insert(out.end(), rec, rec + kRec);
    // MY WWE -> OPTIONS -> REPORT A PROBLEM (report_page.h) after WEAPONS, the group's last: a copy of it (its node
    // id too, as menu.pac's port rows - game_files.cpp; menu.pac has no room left for one more record).
    if (Rd32(rec) == kOptionsWeaponsLabel && Rd32(rec + 0x18) == kOptionsGroup && (Rd32(rec + 0x3C) & 2)) {
      uint8_t* prev = out.data() + out.size() - kRec;
      std::vector<uint8_t> copy(prev, prev + kRec);
      Wr32(copy.data() + 0x00, kReportLabel);
      Wr32(copy.data() + 0x40, 0);  // no NEW badge, not hidden
      Wr32(prev + 0x3C, Rd32(prev + 0x3C) & ~2u);
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      continue;
    }
    if (Rd32(rec + 0x54) != kScreenMatch) continue;
    for (const Row& row : kRows) {
      if (Rd32(rec) != row.anchor || Rd32(rec + 0x18) != row.group || !svr2011::MatchTypeSwitchOn(row.mod)) continue;
      uint8_t* prev = out.data() + out.size() - kRec;
      std::vector<uint8_t> copy(prev, prev + kRec);
      Wr32(copy.data() + 0x00, row.label);
      Wr32(copy.data() + 0x64, row.description);
      Wr32(copy.data() + 0x5C, row.rule);
      Wr32(copy.data() + 0x40, 0);  // no NEW badge, not hidden
      // The group's last row stays last.
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, flags);
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
    }
    if (const uint32_t rule = Rd32(rec + 0x5C);
        elimination && (rule == kTripleThreat || rule == kFatal4Way) && Rd32(rec + 0x54) == kScreenMatch) {
      const uint32_t in_group = Rd32(rec + 0x18);
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kElimLabel);
      Wr32(copy.data() + 0x04, kElimText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      uint32_t index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == in_group) ++index;
      g_elim_rows.push_back({in_group, index});
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: ELIMINATION (rule {:02X}) at group {:02X} row {}", rule, in_group, index);
    }
    if (slobber && Rd32(rec) == kGauntletRow && Rd32(rec + 0x18) == kHandicapGroup) {
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kSlobberLabel);
      Wr32(copy.data() + 0x04, kSlobberText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      int index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == kHandicapGroup) ++index;
      g_slobber_row = index;
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: SLOBBER KNOCKER at group {:02X} row {}", kHandicapGroup, index);
    }
    if (gauntlet_elim && Rd32(rec) == kGauntletRow && Rd32(rec + 0x18) == kHandicapGroup) {
      for (int k = 0; k < 2; ++k) {  // (GAUNTLET ELIMINATOR, its EXTREME RULES one)
        std::vector<uint8_t> copy(rec, rec + kRec);
        Wr32(copy.data() + 0x00, k ? kGeErLabel : kGeLabel);
        Wr32(copy.data() + 0x04, k ? kGeErText : kGeText);
        Wr32(copy.data() + 0x40, 0);
        uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
        const uint32_t flags = Rd32(prev + 0x3C);
        Wr32(prev + 0x3C, flags & ~2u);
        Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
        int index = 0;
        for (size_t at = kFirst; at < out.size(); at += kRec)
          if (Rd32(out.data() + at + 0x18) == kHandicapGroup) ++index;
        (k ? g_ge_er_row : g_ge_row) = index;
        out.insert(out.end(), copy.begin(), copy.end());
        ++added;
      }
      REXLOG_INFO("match types: GAUNTLET ELIMINATOR at group {:02X} rows {} and {}", kHandicapGroup, g_ge_row, g_ge_er_row);
    }
    if (mystery && Rd32(rec + 0x5C) == 0x00 && Rd32(rec + 0x18) == kFullGroup && submenu) {
      // NORMAL MATCH becomes a submenu (its own node) of NORMAL MATCH and
      // MYSTERY OPPONENT (a new group), as EXTREME RULES below.
      out.resize(out.size() - kRec);
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x00, Rd32(rec + 0x00));
      Wr32(row.data() + 0x04, Rd32(rec + 0x04));
      Wr32(row.data() + 0x18, kFullGroup);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(rec + 0x3C) & 2u));
      Wr32(row.data() + 0x40, Rd32(rec + 0x40));
      out.insert(out.end(), row.begin(), row.end());
      for (uint32_t k = 0; k < 2; ++k) {  // (NORMAL MATCH, MYSTERY OPPONENT)
        std::vector<uint8_t> leaf(rec, rec + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x14, Rd32(rec + 0x14) + 0x100);
        Wr32(leaf.data() + 0x40, 0);
        Wr32(leaf.data() + 0x3C, k == 1 ? Rd32(rec + 0x3C) | 2u : Rd32(rec + 0x3C) & ~2u);
        if (k == 1) {
          Wr32(leaf.data() + 0x00, svr2011::kMysteryLabel);
          Wr32(leaf.data() + 0x04, svr2011::kMysteryText);
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      g_mystery_row = {group, 1};
      REXLOG_INFO("match types: NORMAL MATCH submenu with MYSTERY OPPONENT, group {:02X}", group);
      node += 2;
      ++group;
    }
    if (const uint32_t rule = Rd32(rec + 0x5C), st = StructureOf(rule) ? uint32_t(StructureOf(rule) - kStructures) : ~0u;
        st != ~0u && (structure_on[st] || (st == 0 && roof_start)) && submenu) {
      // (the cell: its weapons row and / or START ON THE ROOF)
      const bool with_weapons = structure_on[st], with_roof = st == 0 && roof_start;
      // The row becomes a submenu (a new node in its place) of the match and
      // its weapons one (a new group).
      out.resize(out.size() - kRec);
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x00, Rd32(rec + 0x00));
      Wr32(row.data() + 0x04, Rd32(rec + 0x04));  // (description)
      Wr32(row.data() + 0x18, Rd32(rec + 0x18));
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));  // (its node: the cell row's own)
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(rec + 0x3C) & 2u));
      Wr32(row.data() + 0x40, Rd32(rec + 0x40));
      out.insert(out.end(), row.begin(), row.end());
      // (the match, the weapons one, the roof one)
      const uint32_t leaves = 1 + (with_weapons ? 1 : 0) + (with_roof ? 1 : 0);
      for (uint32_t k = 0; k < leaves; ++k) {
        const bool is_weapons = k == 1 && with_weapons, is_roof = k == leaves - 1 && with_roof;
        std::vector<uint8_t> leaf(rec, rec + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x14, Rd32(rec + 0x14) + 0x100);  // (depth +0x16: one down)
        Wr32(leaf.data() + 0x40, 0);
        Wr32(leaf.data() + 0x3C, k == leaves - 1 ? Rd32(rec + 0x3C) | 2u : Rd32(rec + 0x3C) & ~2u);
        if (is_weapons) {
          Wr32(leaf.data() + 0x00, kStructures[st].label);
          Wr32(leaf.data() + 0x04, kStructures[st].text);
          g_weapons_rows.push_back({group, k});
        }
        if (is_roof) {
          Wr32(leaf.data() + 0x00, kRoofLabel);
          Wr32(leaf.data() + 0x04, kRoofText);
          g_roof_rows.push_back({group, k});
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      REXLOG_INFO("match types: rule {:02X} submenu with {}{}, group {:02X}", rule, with_weapons ? kStructures[st].name : "",
                  with_roof ? " + START ON THE ROOF" : "", group);
      node += leaves;
      ++group;
      continue;
    }
    const ExtremeMode* mode = nullptr;
    for (const ExtremeMode& m : kExtremeModes)
      if (Rd32(rec + 0x18) == m.group) mode = &m;
    if (const uint32_t rule = Rd32(rec + 0x5C);
        (weapons || stages) && rule >= kExtremeFirst && rule <= kExtremeLast && mode && submenu) {
      // EXTREME RULES becomes a submenu (a new node in its place) of EXTREME
      // RULES, WEAPONS EVERYWHERE and THREE STAGES OF HELL (a new group).
      out.resize(out.size() - kRec);
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x00, Rd32(rec + 0x00));
      Wr32(row.data() + 0x04, Rd32(rec + 0x04));  // (description)
      Wr32(row.data() + 0x18, mode->group);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));  // (its node: EXTREME RULES' own - node ids, above)
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(rec + 0x3C) & 2u));
      Wr32(row.data() + 0x40, Rd32(rec + 0x40));
      out.insert(out.end(), row.begin(), row.end());
      // (EXTREME RULES, then WEAPONS EVERYWHERE and THREE STAGES OF HELL - those on)
      std::vector<uint32_t> kinds = {0};
      if (weapons) kinds.push_back(1);
      if (stages) kinds.push_back(2);
      for (uint32_t k = 0; k < kinds.size(); ++k) {
        std::vector<uint8_t> leaf(rec, rec + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x14, Rd32(rec + 0x14) + 0x100);  // (depth +0x16: one down)
        Wr32(leaf.data() + 0x40, 0);
        Wr32(leaf.data() + 0x3C, k + 1 == kinds.size() ? Rd32(rec + 0x3C) | 2u : Rd32(rec + 0x3C) & ~2u);
        if (kinds[k] == 1) {
          Wr32(leaf.data() + 0x00, kWeaponsLabel);
          Wr32(leaf.data() + 0x04, kWeaponsText);
          g_weapons_rows.push_back({group, k});
        } else if (kinds[k] == 2) {  // (a submenu of the sets: a new node, its rows a new group)
          std::vector<uint8_t> row(submenu, submenu + kRec);
          Wr32(row.data() + 0x00, kStagesLabel);
          Wr32(row.data() + 0x04, kStagesText);
          Wr32(row.data() + 0x14, Rd32(submenu + 0x14) + 0x100);  // (one down from EXTREME RULES)
          Wr32(row.data() + 0x18, group);
          Wr32(row.data() + 0x1C, node + k);
          Wr32(row.data() + 0x38, Rd32(rec + 0x1C));
          Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(leaf.data() + 0x3C) & 2u));
          Wr32(row.data() + 0x40, 0);
          leaf = row;
          stage_sets = {node + k, Rd32(rec + 0x14) + 0x200, mode->normal};
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      if (stage_sets.parent) AddStageSets(stage_sets, group + 1, node + uint32_t(kinds.size()));
      REXLOG_INFO("match types: EXTREME RULES (rule {:02X}) submenu with WEAPONS EVERYWHERE, group {:02X}", rule, group);
      node += uint32_t(kinds.size());
      ++group;
      if (stage_sets.parent) node += uint32_t(SetsFor(stage_sets.normal).size()), ++group, stage_sets = {};
    } else if (const uint32_t rule = Rd32(rec + 0x5C);
               weapons && rule >= kExtremeFirst && rule <= kExtremeLast && !mode) {
      const uint32_t in_group = Rd32(rec + 0x18);
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kWeaponsLabel);
      Wr32(copy.data() + 0x04, kWeaponsText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      uint32_t index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == in_group) ++index;
      g_weapons_rows.push_back({in_group, index});
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: WEAPONS EVERYWHERE (rule {:02X}) at group {:02X} row {}", rule, in_group, index);
    }
    for (const PlainMode& m : kPlainModes) {
      if (Rd32(rec) != m.anchor || Rd32(rec + 0x18) != m.group) continue;
      for (int k = 0; k < 2; ++k) {  // (WEAPONS EVERYWHERE, THREE STAGES OF HELL)
        if (!(k == 0 ? weapons : stages)) continue;
        std::vector<uint8_t> copy(rec, rec + kRec);
        Wr32(copy.data() + 0x00, k == 0 ? kWeaponsLabel : kStagesLabel);
        Wr32(copy.data() + 0x04, k == 0 ? kWeaponsText : kStagesText);
        Wr32(copy.data() + 0x5C, m.normal);
        Wr32(copy.data() + 0x40, 0);
        if (k == 1 && submenu) {  // (THREE STAGES OF HELL: a submenu of the sets, its rows a new group)
          // Its node: the one of the record before it (node ids, above). A new top id here, in the middle of the
          // table, left B doing nothing in the sets (player report, 2.1.5: "Gauntlet > Three Stages of Hell, you
          // can't go back any more" - HANDICAP's and 6-MAN's).
          const uint32_t shared = Rd32(out.data() + out.size() - kRec + 0x1C);
          std::vector<uint8_t> row(submenu, submenu + kRec);
          Wr32(row.data() + 0x00, kStagesLabel);
          Wr32(row.data() + 0x04, kStagesText);
          Wr32(row.data() + 0x18, m.group);
          Wr32(row.data() + 0x1C, shared);
          Wr32(row.data() + 0x38, Rd32(rec + 0x38));
          Wr32(row.data() + 0x40, 0);
          copy = row;
          AddStageSets({shared, Rd32(rec + 0x14) + 0x100, m.normal}, group, node);
          node += uint32_t(SetsFor(m.normal).size());
          ++group;
        }
        uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
        const uint32_t flags = Rd32(prev + 0x3C);
        Wr32(prev + 0x3C, flags & ~2u);
        Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
        uint32_t index = 0;
        for (size_t at = kFirst; at < out.size(); at += kRec)
          if (Rd32(out.data() + at + 0x18) == m.group) ++index;
        if (k == 0) g_weapons_rows.push_back({m.group, index});
        out.insert(out.end(), copy.begin(), copy.end());
        ++added;
        REXLOG_INFO("match types: {} (rule {:02X}) at group {:02X} row {}",
                    k == 0 ? "WEAPONS EVERYWHERE" : "THREE STAGES OF HELL", m.normal, m.group, index);
      }
    }
    if (Rd32(rec + 0x18) == kBackstage1v1Group) {
      // (after the room's row and the rows added after it)
      const auto& own = svr2011::BackstageRows();
      for (size_t k = 0; k < own.size() && k < 8; ++k) {
        if (Rd32(rec + 0x5C) != kBackstage1v1FirstRule + uint32_t(own[k].area)) continue;
        std::vector<uint8_t> copy(rec, rec + kRec);
        Wr32(copy.data() + 0x00, kOwnRowLabel + uint32_t(k));
        Wr32(copy.data() + 0x64, kNoText);
        Wr32(copy.data() + 0x40, 0);
        // (the group's last row stays last)
        uint8_t* prev = out.data() + out.size() - kRec;
        const uint32_t flags = Rd32(prev + 0x3C);
        Wr32(prev + 0x3C, flags & ~2u);
        Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
        uint32_t index = 0;  // the row's place in its group
        for (size_t at = kFirst; at < out.size(); at += kRec)
          if (Rd32(out.data() + at + 0x18) == kBackstage1v1Group) ++index;
        g_own_row_index[k] = int(index);
        out.insert(out.end(), copy.begin(), copy.end());
        ++added;
        REXLOG_INFO("match types: backstage row '{}' at BACKSTAGE row {}", own[k].label, index);
      }
    }
    for (Backstage& b : g_backstage) {
      if (!more_backstage || !submenu || areas.size() != 7) break;
      if (Rd32(rec) != b.anchor || Rd32(rec + 0x18) != b.group) continue;
      // The submenu row: in this group, a new node.
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x18, b.group);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));  // (its node: the anchor's - node ids, above)
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, Rd32(row.data() + 0x3C) & ~2u);
      Wr32(row.data() + 0x40, 0);
      out.insert(out.end(), row.begin(), row.end());
      ++added;
      // Its areas: a new group under that node.
      b.menu_group = group;
      const uint32_t count = uint32_t(areas.size()) + (free_roam_on ? 1 : 0);
      for (uint32_t k = 0; k < count; ++k) {
        // The 7 areas, then FREE-ROAMING BACKSTAGE (the group's last).
        const bool free_roam = k == areas.size();
        const bool last = k + 1 == count;
        std::vector<uint8_t> leaf(areas[free_roam ? k - 1 : k], areas[free_roam ? k - 1 : k] + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x40, 0);
        const uint32_t flags = Rd32(leaf.data() + 0x3C);
        Wr32(leaf.data() + 0x3C, last ? flags | 2u : flags & ~2u);
        if (free_roam) {
          Wr32(leaf.data() + 0x00, kFreeRoamLabel);
          Wr32(leaf.data() + 0x5C, kWholeBackstage2);
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      node += count;
      ++group;
    }
  }
  if (!added) return {};
  out.insert(out.end(), tail.begin(), tail.end());
  out.insert(out.end(), table + kFirst + total * kRec, table + size);
  Wr32(out.data() + 4, total + added);
  Wr32(out.data() + 8, shown + added);
  REXLOG_INFO("match types: {} rows added to the match menus", added);
  return out;
}

// -- Backstage with 3, 4 or 6 people --------------------------------------
//
// The area of a backstage match comes from its rule id (0x70-0x76: one per
// area, 2 on 2). For a row of the new submenus, that rule's record takes
// the shape of a match with that many people while the match is set up and
// played (setting up any other match puts it back): of the 100-byte record
// +0..+27 (people, their start places / teams, select screen, team layout),
// +44 (match family) and +52, +64 (its first rule flag); +34 (the people
// count) and +35 (free for all) of the 64-byte option record. (Only +35 was
// copied before: a 6-man backstage match had 4 people - the areas' 2 on 2
// count.)
constexpr uint32_t kRules = 0x82EDE954;  // -> 119 x 100 bytes, then 119 x 64
constexpr uint32_t kRuleSize = 100, kRule2 = 11900, kRule2Size = 64;
constexpr uint32_t kRule2People = 34, kRule2FreeForAll = 35;
struct Span {
  uint32_t at, size;
};
constexpr Span kShape[] = {{0, 28}, {44, 4}, {52, 4}, {64, 1}};
// Lumberjack (0x55, story mode only): 2 wrestlers picked (the 1 on 1 select
// screen of rule 00: +24 team layout, +36, +44, +52, +64) and 4 lumberjacks
// - people 2-5, team 3, as manager slots (kind 2: the select screen leaves
// them out) filled with random superstars as the match loads (below).
constexpr uint32_t kLumberjack = 0x55, kLumberjackLike = 0x00, kSixMan = 0x0A;
// At most 4: the people builder (sub_828BBEF0) takes exactly 6 people from
// the match's slots (the referee is person 6, the commentators 7-8) - 6-8
// lumberjacks would need it and every per-person table behind it redone.
constexpr uint32_t kMaxLumberjacks = 4;
// How many lumberjacks (test aid: SVR2011_TEST_LJ_COUNT=<n>, 1-4).
uint32_t LumberjackCount() {
  static const uint32_t n = [] {
    const char* v = std::getenv("SVR2011_TEST_LJ_COUNT");
    return v ? std::clamp<uint32_t>(uint32_t(std::atoi(v)), 1, kMaxLumberjacks) : 4u;
  }();
  return n;
}
constexpr Span kSelect[] = {{24, 4}, {36, 4}, {44, 4}, {52, 4}, {64, 1}};

// The match row picked last: the rule record to reshape like which, and how
// (pending_like ~0: none).
uint32_t g_pending_rule_lo = 0, g_pending_rule_hi = 0, g_pending_like = ~0u;
bool g_pending_select_only = false;
bool g_lumberjacks_chosen = false;  // (people 2-5 of this Lumberjack match filled)
struct Saved {
  uint32_t rule = 0;
  uint8_t rec[kRuleSize];
  uint8_t rec2[kRule2Size];
};
Saved g_saved;  // the record reshaped now (rule 0: none)

void RestoreRule(uint8_t* base) {
  if (!g_saved.rule) return;
  if (const uint32_t rules = Rd32(base + kRules)) {
    std::memcpy(base + rules + g_saved.rule * kRuleSize, g_saved.rec, kRuleSize);
    std::memcpy(base + rules + kRule2 + g_saved.rule * kRule2Size, g_saved.rec2, kRule2Size);
  }
  g_saved.rule = 0;
}

// The whole backstage (rules 0x19 1 on 1, 0x1A 2 on 2: no named area) is a
// story mode rule: in an exhibition its CPU stands still, whatever its
// records (its area, from the rule id, has none for the AI). So it is played
// as the parking lot (0x1B 1 on 1, 0x70 2 on 2: the CPU fights), starting
// there, with g_free_roam: every room shown, a fight box around the whole
// backstage, the backstage camera.
constexpr uint32_t kStoryContext = 0x82E3C1F4;

void ShapeRule(uint8_t* base, uint32_t rule, uint32_t like, bool select_only) {
  const uint32_t rules = Rd32(base + kRules);
  if (!rules) return;
  uint8_t* rec = base + rules + rule * kRuleSize;
  const uint8_t* from = base + rules + like * kRuleSize;
  uint8_t* rec2 = base + rules + kRule2 + rule * kRule2Size;
  if (g_saved.rule != rule) {  // (the whole backstage's are already saved)
    g_saved.rule = rule;
    std::memcpy(g_saved.rec, rec, kRuleSize);
    std::memcpy(g_saved.rec2, rec2, kRule2Size);
  }
  if (select_only) {
    for (const Span& span : kSelect) std::memcpy(rec + span.at, from + span.at, span.size);
    // Lumberjack: 2 wrestlers and 4 lumberjacks (the record has 3 wrestlers,
    // teams 0, 1, 2, and 3 lumberjacks): people 2-5 team 3, manager slots.
    if (rule == kLumberjack) {
      for (int i = 2; i < 6; ++i) rec[2 + i * 3 + 1] = 3, rec[2 + i * 3 + 2] = 2;
    }
    // Slobber Knocker: the 2 waiting opponents (people 2-3, kind 3) as manager
    // slots while the select screen runs (it would have them picked); kind 3
    // again as the people are built (sub_828BBEF0 below).
    if (rule == kGauntlet && g_ge) {  // (six: people 2-5 the entrants, each his own team)
      rec[0] = 0, rec[1] = uint8_t(kGePeople);  // (a big-endian u16)
      rec2[34] = uint8_t(kGePeople);
      // (kind 2 - manager slots - while the select screen runs, 3 as the people
      // are built; online there's no select screen and the people builder
      // isn't run: 3 at once)
      const uint8_t kind = g_online_to == kGauntlet ? 3 : 2;
      for (int i = 2; i < kGePeople; ++i) rec[2 + i * 3] = uint8_t(i), rec[2 + i * 3 + 1] = uint8_t(i), rec[2 + i * 3 + 2] = kind;
    }
    if (rule == kGauntlet && g_slobber && g_online_to == kGauntlet) {
      // ONLINE (co-op): six - the 2 players a team (0), the lobby's 4 CPUs the
      // other (1); the kinds are the match slots' (sub_828BB290 below)
      rec[0] = 0, rec[1] = 6;
      rec2[34] = 6;
      for (int i = 0; i < 6; ++i) rec[2 + i * 3] = uint8_t(i), rec[2 + i * 3 + 1] = i < 2 ? 0 : 1, rec[2 + i * 3 + 2] = i < 3 ? 0 : 3;
    } else if (rule == kGauntlet && g_slobber) {
      // (and a third: person 4 - the gauntlet has 4 people, the 5-person one
      // 0x58 shows 5 work)
      const uint8_t* five = base + rules + 0x58 * kRuleSize;  // (the 5-person gauntlet's count)
      rec[0] = five[0], rec[1] = five[1];
      rec2[34] = base[rules + kRule2 + 0x58 * kRule2Size + 34];
      rec[2 + 4 * 3] = 4, rec[2 + 4 * 3 + 1] = 1;
      for (int i = 2; i < kSlobberPeople; ++i) rec[2 + i * 3 + 2] = 2;
    }
  } else {
    for (const Span& span : kShape) std::memcpy(rec + span.at, from + span.at, span.size);
    for (uint32_t at : {kRule2People, kRule2FreeForAll}) rec2[at] = base[rules + kRule2 + like * kRule2Size + at];
  }
  REXLOG_INFO("match types: rule {:02X} set up like rule {:02X}{}", rule, like, select_only ? " (select screen)" : "");
}

}  // namespace

namespace svr2011 {

const std::map<std::string, bool>& MatchTypeSwitches() {
  static const std::map<std::string, bool> on = [] {
    std::map<std::string, bool> m;
    const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "MatchTypes";
    std::error_code ec;
    for (const char* k : {"falls_count_anywhere", "championship_scramble", "royal_rumble_15_25", "lumberjack",
                          "free_roaming_backstage", "backstage_more_people", "weapons_everywhere", "slobber_knocker",
                          "three_stages_of_hell", "elimination", "mystery_opponent", "hiac_tables_ladders", "hiac_roof_start",
                          "three_stages_tables_ladders",
                          "gauntlet_eliminator", "cage_weapons", "inferno_weapons", "online_first_blood"}) {
      // (Start Without Mods: none - they count as mods for online, online_files.h ActiveMatchMods)
      m[k] = !NoMods() && !std::filesystem::exists(dir / k / "disabled", ec);
      // (FREE-ROAMING BACKSTAGE is removed - the user's call for 2.1.6: its rows never added, its rules played as
      // a named room at set-up, sub_827374A0 below)
      const bool removed = std::string(k) == "free_roaming_backstage";
      if (removed) m[k] = false;
      if (!m[k])
        REXLOG_INFO("match types: {} off ({})", k, removed ? "removed" : NoMods() ? "started without mods" : "mod switched off");
    }
    return m;
  }();
  return on;
}

bool MatchTypeSwitchOn(const char* id) {
  const auto& on = MatchTypeSwitches();
  const auto it = on.find(id);
  return it == on.end() || it->second;
}

bool MatchTypeOn(const char* id) { return MatchTypeSwitchOn(id) || svr2011::P2PMatchLobby().in; }

void InstallMatchTypes(rex::memory::Memory* memory) {
  g_memory = memory;
  if (const char* v = std::getenv("SVR2011_TEST_MODE"); v && *v) {
    const std::string m = v;
    struct Mode {
      const char* name;
      TestMode mode;
      int rule;
    };
    constexpr Mode kModes[] = {{"three_stages", TestMode::kThreeStages, 0x00},
                               {"elimination_tt", TestMode::kElimTT, int(kTripleThreat)},
                               {"elimination_f4w", TestMode::kElimF4W, int(kFatal4Way)},
                               {"weapons", TestMode::kWeapons, int(kExtremeFirst)},
                               {"cell_weapons", TestMode::kWeapons, int(kCellFirst)},
                               {"cage_weapons", TestMode::kWeapons, 0x3C},
                               {"inferno_weapons", TestMode::kWeapons, 0x51},
                               {"slobber", TestMode::kSlobber, int(kGauntlet)},
                               {"gauntlet_eliminator", TestMode::kGauntletElim, int(kGauntlet)},
                               {"gauntlet_eliminator_er", TestMode::kGauntletElimEr, int(kGauntlet)},
                               {"lumberjack", TestMode::kLumberjack, int(kLumberjack)},
                               {"mystery", TestMode::kMystery, 0},
                               {"roof", TestMode::kRoof, int(kCellFirst)}};
    for (const Mode& k : kModes)
      if (m == k.name) {
        g_test_mode = k.mode;
        if (k.rule) g_test_rule = k.rule;
        REXLOG_INFO("match types: ONE ON ONE NORMAL plays {} (SVR2011_TEST_MODE, rule {:02X})", k.name, k.rule);
      }
    if (g_test_mode == TestMode::kNone) REXLOG_WARN("match types: unknown SVR2011_TEST_MODE '{}'", m);
  }
  if (const char* v = std::getenv("SVR2011_TEST_RULE"); v && *v) {
    g_test_rule = int(std::strtol(v, nullptr, 16));
    REXLOG_INFO("match types: ONE ON ONE NORMAL plays rule {:02X} (SVR2011_TEST_RULE)", g_test_rule);
  }
}

}  // namespace svr2011

namespace {
// The CPU stall net (g_count_seen above): from the free state of character c.
void CpuStallNet(const PPCContext& saved, uint8_t* base, uint32_t c) {
  static const int test_secs = [] { const char* v = std::getenv("SVR2011_TEST_STALL_SECS"); return v ? std::atoi(v) : 0; }();
  const uint32_t stall_frames = uint32_t((test_secs > 0 ? test_secs : 8) * 60);
  const uint32_t person = Rd32(base + c + 1156), frames = Rd32(base + 0x82E3CD0C), motion = Rd32(base + c + 212);
  if (person >= 6) return;
  // (a new motion, a new match, or frames when he wasn't free - knocked down, in a move: the free state comes back
  // in the same standing motion, and a stand from before the knock-down counted - Cena climbed in 2 s after getting up)
  static uint32_t seen[6] = {};
  const bool gap = !seen[person] || frames > seen[person] + 2 || frames < seen[person];
  seen[person] = frames;  // (every frame while he stands: measured, 30 Oct)
  if (gap || g_still_motion[person] != motion || !g_still_since[person] || frames + 1 < g_still_since[person]) {
    g_still_motion[person] = motion, g_still_since[person] = frames + 1;
    return;
  }
  if (frames + 1 - g_still_since[person] < stall_frames) return;
  const uint32_t match = Rd32(base + 0x82EDEB48);
  if (!match || base[match + 432 + person * 2116 - 8] != 1 || Rd32(base + c + 1152) < 4) return;  // (a CPU only)
  const auto rd_f = [&](uint32_t at) { const uint32_t u = Rd32(base + at); float f; std::memcpy(&f, &u, 4); return f; };
  const float x = rd_f(c + 288), y = rd_f(c + 292), z = rd_f(c + 296);
  if (base[c + 444] != 1 || y < -4.f) return;  // (on the floor outside)
  const bool counting = g_count_seen && frames + 1 - g_count_seen < 120;
  if (!counting && test_secs <= 0) return;
  if (Rd32(base + c + 1828) != ~0u) return;  // (holding something)
  // The game's own way back in (7014 -> 7010 -> 7015/7016: it takes him to the apron and over it - from z 45.7 he
  // was on the mat at 22.8), facing the ring from the side he is on. (The action-button handler sub_8230A760 with
  // the stick held that way started nothing.) Not from far off - the ramp, the crowd.
  constexpr float kFar = 50.f;
  const bool along_x = std::fabs(x) > std::fabs(z);  // (he is off the ring's +-x side)
  if (std::max(std::fabs(x), std::fabs(z)) > kFar) return;
  constexpr float kHalfPi = 1.5707963f;
  const float yaw = along_x ? (x < 0 ? -kHalfPi : kHalfPi) : (z < 0 ? 2.f * kHalfPi : 0.f);
  uint32_t yaw_bits;
  std::memcpy(&yaw_bits, &yaw, 4);
  Wr32(base + c + 276, yaw_bits);
  PPCContext k = saved;
  k.r3.u64 = c, k.r4.u64 = kClimbIn;
  sub_82397228(k, base);
  const uint32_t after = Rd32(base + c + 212);
  static int logged = 0;
  if (logged++ < 40)
    REXLOG_INFO("cpu stall: person {} idle {} s outside (motion {}, x {:.0f} z {:.0f}, count {}) -> back in: motion {}",
                person, (frames + 1 - g_still_since[person]) / 60, motion, x, z, counting ? "on" : "off (test)", after);
  g_still_since[person] = frames + 1;  // (again after another stall_frames if it didn't take)
}
}  // namespace

// A wrestler's free state (standing, nothing on): sub_82292D50(state), the
// character *(state +280). After it: a CPU alone on the roof long enough
// climbs down (above, g_roof_alone_since) - as if it pressed the action button
// towards the nearest edge.
// Test aid: SVR2011_TEST_FOLLOW_LOG=1 - a motion event's jump to a paired motion (ops 49 / 50: sub_821D4B50, r4 = the
// event, r3 + 36 the fighter): the event's bytes and where they are, the fighter's motion and frame.
REX_EXTERN(__imp__sub_821D4B50);
REX_HOOK_RAW(sub_821D4B50) {
  static const bool follow_log = std::getenv("SVR2011_TEST_FOLLOW_LOG") != nullptr;
  if (follow_log) {
    const uint32_t ev = ctx.r4.u32, ch = Rd32(base + ctx.r3.u32 + 36);
    std::string bytes;
    for (int i = -24; i < 12; ++i) bytes += (i == 0 ? " [" : i == 4 ? " ] " : " ") + fmt::format("{:02X}", base[ev + i]);
    REXLOG_INFO("follow: event at {:08X}:{} - fighter {:08X} person {} motion {} frame bits {:08X}", ev, bytes, ch,
                ch ? Rd32(base + ch + 1156) : 0, ch ? Rd32(base + ch + 212) : 0, ch ? Rd32(base + ch + 24) : 0);
  }
  __imp__sub_821D4B50(ctx, base);
}

REX_EXTERN(__imp__sub_82292D50);
REX_HOOK_RAW(sub_82292D50) {
  const uint32_t c = Rd32(base + ctx.r3.u32 + 280);
  const uint32_t motion = c ? Rd32(base + c + 212) : 0;
  const PPCContext saved = ctx;
  __imp__sub_82292D50(ctx, base);
  // Once a match, who plays each wrestler: its character's port +1152 is its player (0-3; a CPU's too - the
  // game plays a person whose player has no controller), so: that player's controller, or the CPU (problem
  // reports - "the CPU only reverses": a controller nobody holds played him).
  if (c) {
    static uint32_t told_match = 0, told = 0;
    const uint32_t m = Rd32(base + 0x82EDEB48), person = Rd32(base + c + 1156), port = Rd32(base + c + 1152);
    if (m != told_match) told_match = m, told = 0;
    if (m && person < 6 && !(told >> person & 1)) {
      told |= 1u << person;
      static const char* const kType[] = {"none", "Xbox", "PlayStation", "keyboard"};
      const svr2011::PadType t = port < svr2011::kMaxPlayers ? svr2011::PlayerPadType(port) : svr2011::PadType::kNone;
      REXLOG_INFO("[svr2011] match: person {} - {}", person,
                  t != svr2011::PadType::kNone ? fmt::format("player {}'s controller ({})", port + 1, kType[int(t)])
                                               : std::string("the CPU"));
    }
  }
  if (c) CpuStallNet(saved, base, c);
  static const bool off = [] { const char* v = std::getenv("SVR2011_TEST_ROOF_DOWN"); return v && *v == '0'; }();
  static const uint32_t alone_frames = [] {
    const char* v = std::getenv("SVR2011_TEST_ROOF_DOWN_SECS");
    return uint32_t((v ? std::max(1, std::atoi(v)) : 25) * 60);
  }();
  if (off || !c || base[c + 444] != kRoofArea || Rd32(base + c + 212) != motion) return;  // (on the roof, nothing started)
  const uint32_t person = Rd32(base + c + 1156), frames = Rd32(base + 0x82E3CD0C);
  // (test aid: SVR2011_TEST_ROOF_DOWN_LOG=1 - once a second, a free wrestler on the roof and what the check sees)
  if (static const bool log = std::getenv("SVR2011_TEST_ROOF_DOWN_LOG") != nullptr; log && frames && frames % 60 == 0) {
    const uint32_t m = Rd32(base + 0x82EDEB48);
    REXLOG_INFO("match types: roof - person {} alone {} frames, slot ctl {}, port {}, held {:X}, x {:.0f} z {:.0f}", person,
                person < 6 && g_roof_alone_since[person] ? frames + 1 - g_roof_alone_since[person] : 0,
                m && person < 6 ? base[m + 432 + person * 2116 - 8] : 255, Rd32(base + c + 1152), Rd32(base + c + 1828),
                [&] { const uint32_t u = Rd32(base + c + 288); float f; std::memcpy(&f, &u, 4); return f; }(),
                [&] { const uint32_t u = Rd32(base + c + 296); float f; std::memcpy(&f, &u, 4); return f; }());
  }
  if (person >= 6 || !g_roof_alone_since[person] || frames + 1 - g_roof_alone_since[person] < alone_frames) return;
  if (Rd32(base + c + 1828) != ~0u) return;  // (holding something: the edge check goes by it)
  const uint32_t match = Rd32(base + 0x82EDEB48);
  // (a CPU only: its slot's controller 1 and its character's controller port +1152 not a pad's, 0-3 - a
  // test match's cpu=all marks person 0's slot too while a pad still plays him)
  if (!match || base[match + 432 + person * 2116 - 8] != 1 || Rd32(base + c + 1152) < 4) return;
  // The nearest edge: the way the wrestler is farthest out (octants of +2212:
  // 0 -z, 2 -x, 4 +z, 6 +x) - the edge check wants more than 20 out that way.
  const auto rd_f = [&](uint32_t at) { const uint32_t u = Rd32(base + at); float f; std::memcpy(&f, &u, 4); return f; };
  const float x = rd_f(c + 288), z = rd_f(c + 296);
  const uint32_t octant = std::fabs(x) > std::fabs(z) ? (x < 0 ? 2u : 6u) : (z < 0 ? 0u : 4u);
  if (std::max(std::fabs(x), std::fabs(z)) <= 20.0f) return;  // (near the middle: later, as he moves)
  Wr32(base + c + 2212, octant);
  PPCContext k = saved;
  k.r3.u64 = c;
  sub_821BE8F8(k, base);
  if (!k.r3.u32) return;
  k = saved;
  k.r3.u64 = c, k.r4.u64 = kClimbDown;
  sub_82397228(k, base);
  REXLOG_INFO("match types: CPU person {} alone on the roof {} s - climbs down (x {:.0f} z {:.0f}, octant {}, motion {})",
              person, (frames + 1 - g_roof_alone_since[person]) / 60, x, z, octant, Rd32(base + c + 212));
  g_roof_alone_since[person] = 0;
}

// The menu table is read: sub_82BAA6E8(menus, table, size).
REX_EXTERN(__imp__sub_82BAA6E8);
REX_HOOK_RAW(sub_82BAA6E8) {
  const uint32_t table = ctx.r4.u32, size = ctx.r5.u32;
  std::vector<uint8_t> rows;
  if (g_memory && table) rows = WithRows(base + table, size);
  svr2011::LoadWatchDone("its menus read");
  if (rows.empty()) {
    __imp__sub_82BAA6E8(ctx, base);
    return;
  }
  const uint32_t copy = g_memory->SystemHeapAlloc(uint32_t(rows.size()));
  if (!copy) {
    __imp__sub_82BAA6E8(ctx, base);
    return;
  }
  std::memcpy(base + copy, rows.data(), rows.size());
  svr2011::ScriptMenuTable(rows.data(), rows.size());  // (scripted tests' "menu" steps: the rows as shown)
  ctx.r4.u64 = copy;
  ctx.r5.u64 = uint32_t(rows.size());
  __imp__sub_82BAA6E8(ctx, base);
  g_memory->SystemHeapFree(copy);
}

// The rule of the menu row picked: sub_824402A0(screen) -> id (119 = none).
REX_EXTERN(__imp__sub_824402A0);
REX_HOOK_RAW(sub_824402A0) {
  __imp__sub_824402A0(ctx, base);
  if (g_test_rule >= 0 && ctx.r3.u32 == 0) ctx.r3.u64 = uint32_t(g_test_rule);
}

// A menu row's match: sub_8243FCC8(?, group, row) -> 2000 + rule for a match
// row. Remembers whether it was a row of the backstage submenus.
REX_EXTERN(__imp__sub_8243FCC8);
REX_HOOK_RAW(sub_8243FCC8) {
  const uint32_t group = ctx.r4.u32, ctx_row = ctx.r5.u32 & 0xFFFF;
  __imp__sub_8243FCC8(ctx, base);
  const int32_t result = ctx.r3.s32;
  if (result < 2000 || result >= 2119) return;
  g_pending_like = ~0u;
  g_pending_own_row = -1;
  g_pending_slobber = group == kHandicapGroup && g_slobber_row >= 0 && ctx_row == uint32_t(g_slobber_row);
  g_pending_ge = group == kHandicapGroup && ((g_ge_row >= 0 && ctx_row == uint32_t(g_ge_row)) ||
                                             (g_ge_er_row >= 0 && ctx_row == uint32_t(g_ge_er_row)));
  const bool ge_er = g_pending_ge && g_ge_er_row >= 0 && ctx_row == uint32_t(g_ge_er_row);
  if (g_pending_slobber || g_pending_ge) {  // (the 1 on 1 select screen: player 1 and the first opponent)
    g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
    g_pending_like = kLumberjackLike;
    g_pending_select_only = true;
  }
  g_pending_stages = false;
  for (const StagesRow& w : g_stages_rows)
    if (group == w.group && ctx_row == w.index) g_pending_stages = true, g_pending_set = w.set;
  g_pending_mystery = group == g_mystery_row.group && ctx_row == g_mystery_row.index;
  g_pending_elimination = false;
  for (const WeaponsRow& w : g_elim_rows)
    if (group == w.group && ctx_row == w.index) g_pending_elimination = true;
  g_pending_weapons = ge_er;  // (GAUNTLET ELIMINATOR: EXTREME RULES)
  for (const WeaponsRow& w : g_weapons_rows)
    if (group == w.group && ctx_row == w.index) g_pending_weapons = true;
  g_pending_roof = false;
  for (const WeaponsRow& w : g_roof_rows)
    if (group == w.group && ctx_row == w.index) g_pending_roof = true;
  for (int k = 0; k < 8; ++k)
    if (group == kBackstage1v1Group && g_own_row_index[k] >= 0 && ctx_row == uint32_t(g_own_row_index[k]))
      g_pending_own_row = k;
  if (std::getenv("SVR2011_TEST_ROW_LOG"))
    REXLOG_INFO("match types: row picked: group {:02X} row {} -> rule {:02X} (own row {})", group, ctx_row,
                result - 2000, g_pending_own_row);
  for (const Backstage& b : g_backstage) {
    if (b.menu_group && group == b.menu_group) {
      g_pending_rule_lo = kBackstageFirstRule;
      g_pending_rule_hi = kBackstageLastRule;
      g_pending_like = b.like;
      g_pending_select_only = false;
    }
  }
  if (group == kSixMan && uint32_t(result - 2000) == kLumberjack) {
    g_pending_rule_lo = g_pending_rule_hi = kLumberjack;
    g_pending_like = kLumberjackLike;
    g_pending_select_only = true;
  }
  // (test aid SVR2011_TEST_MODE: ONE ON ONE NORMAL as the mode's row)
  if (g_test_mode != TestMode::kNone && result == 2000) {
    switch (g_test_mode) {
      case TestMode::kThreeStages: {  // (SVR2011_TEST_3S_SET=<0-5>: the set; its match is SVR2011_TEST_RULE's)
        g_pending_stages = true;
        const char* s = std::getenv("SVR2011_TEST_3S_SET");
        g_pending_set = s ? std::atoi(s) : 0;
        break;
      }
      case TestMode::kElimTT:
      case TestMode::kElimF4W: g_pending_elimination = true; break;
      case TestMode::kWeapons: g_pending_weapons = true; break;
      case TestMode::kMystery: g_pending_mystery = true; break;
      case TestMode::kRoof: g_pending_roof = true; break;
      case TestMode::kGauntletElim:
      case TestMode::kGauntletElimEr:
        g_pending_ge = true;
        g_pending_weapons = g_test_mode == TestMode::kGauntletElimEr;
        g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
        g_pending_like = kLumberjackLike;
        g_pending_select_only = true;
        break;
      case TestMode::kSlobber:
        g_pending_slobber = true;
        g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
        g_pending_like = kLumberjackLike;
        g_pending_select_only = true;
        break;
      case TestMode::kLumberjack:
        g_pending_rule_lo = g_pending_rule_hi = kLumberjack;
        g_pending_like = kLumberjackLike;
        g_pending_select_only = true;
        break;
      default: break;
    }
  }
}

// A match is set up: sub_827374A0(match, rule, ...).
REX_EXTERN(__imp__sub_827374A0);
REX_HOOK_RAW(sub_827374A0) {
  uint32_t rule = ctx.r4.u32;
  // ONLINE: the session's PORT RULES (online_rules.h) - the same modes as the
  // offline rows, on both sides, before the match is set up.
  const svr2011::OnlineRulesSetup online = svr2011::OnlineRulesFor(rule);
  svr2011::MysteryOpponentOnline(online.on && online.mystery && !Rd32(base + kStoryContext));
  g_online_from = g_online_to = ~0u;
  if (online.on && !Rd32(base + kStoryContext) && online.rule != rule) g_online_from = rule, g_online_to = online.rule;
  if (online.on && !Rd32(base + kStoryContext)) {
    rule = online.rule;
    ctx.r4.u64 = rule;
    g_pending_weapons = online.weapons;
    g_pending_stages = online.stages;
    if (online.roof) g_pending_roof = true;
    if (online.slobber) {  // (as the SLOBBER KNOCKER row, for six: the players a team against the CPUs)
      g_pending_slobber = true;
      g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
      g_pending_like = kLumberjackLike;
      g_pending_select_only = true;
    }
    if (online.ge) {  // (as the GAUNTLET ELIMINATOR row: the gauntlet reshaped for six, each his own team)
      g_pending_ge = true;
      g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
      g_pending_like = kLumberjackLike;
      g_pending_select_only = true;
    }
    g_pending_set = online.set;
  }
  // Test aid: SVR2011_TEST_SETUP_TRACE=1 - each match set-up with its caller
  // and the story context (a guest stack walk read unmapped pages: left out).
  if (std::getenv("SVR2011_TEST_SETUP_TRACE"))
    REXLOG_INFO("match types: set-up of rule {:02X} (match {:08X}) from {:08X}, story context {:08X}", rule, ctx.r3.u32,
                uint32_t(ctx.lr), Rd32(base + kStoryContext));
  // Research aid: SVR2011_TEST_RULE_DUMP=1 - once, the rule records (first 28
  // bytes, +36..+67) and option records (64 bytes) of some rules.
  if (static bool dumped = false; !dumped && std::getenv("SVR2011_TEST_RULE_DUMP"))
    if (const uint32_t rules = Rd32(base + kRules)) {
      dumped = true;
      for (uint32_t r : {0x00u, 0x03u, 0x04u, 0x07u, 0x08u, 0x0Bu, 0x0Du, 0x0Eu, 0x0Fu, 0x10u, 0x11u, 0x12u, 0x13u,
                         0x23u, 0x27u, 0x28u, 0x29u, 0x2Au, 0x31u, 0x35u, 0x3Bu, 0x4Cu, 0x4Du, 0x4Eu, 0x4Fu, 0x50u}) {
        std::string rec, opt;
        for (uint32_t k = 0; k < kRuleSize; ++k) rec += fmt::format("{:02X}", base[rules + r * kRuleSize + k]);
        for (uint32_t k = 0; k < kRule2Size; ++k) opt += fmt::format("{:02X}", base[rules + kRule2 + r * kRule2Size + k]);
        REXLOG_INFO("rule dump {:02X}: rec {} opt {}", r, rec, opt);
      }
    }
  svr2011::SetMatchLoading(true);  // (arena_mods: a custom arena's loading pictures)
  RestoreRule(base);
  g_lumberjacks_chosen = false;
  // FREE-ROAMING BACKSTAGE is removed (its rows: MatchTypeSwitches): a whole-backstage rule reaching here anyway - a
  // saved match, Universe, a CPU set-up - plays the first named room instead (PARKING LOT: 0x1B, 2 on 2: 0x70).
  // Road to WrestleMania's own uses (its story context) stay the game's.
  if ((rule == kWholeBackstage || rule == kWholeBackstage2) && !Rd32(base + kStoryContext)) {
    const uint32_t room = rule == kWholeBackstage ? kBackstage1v1FirstRule : 0x70u;
    REXLOG_INFO("match types: rule {:02X} (free-roaming backstage, removed) played as rule {:02X}", rule, room);
    rule = room;
    ctx.r4.u64 = rule;
  }
  g_free_roam = (rule == kWholeBackstage || rule == kWholeBackstage2) && !Rd32(base + kStoryContext);
  // A backstage mod's own row: its bg78 for this match (any other: the usual)
  {
    const auto& own = svr2011::BackstageRows();
    int k = g_pending_own_row;
    // (test aid SVR2011_TEST_BACKSTAGE_ROW=<row name>: that mod's row, with SVR2011_TEST_RULE its area's rule -
    // a match started without the menus, e.g. svrtest lan)
    if (const char* v = std::getenv("SVR2011_TEST_BACKSTAGE_ROW"); k < 0 && v && *v)
      for (size_t i = 0; i < own.size(); ++i)
        if (own[i].label == v) k = int(i);
    const bool use = k >= 0 && size_t(k) < own.size() && !Rd32(base + kStoryContext) &&
                     rule == kBackstage1v1FirstRule + uint32_t(own[size_t(k)].area);
    if (use) REXLOG_INFO("match types: backstage area '{}'", own[size_t(k)].label);
    svr2011::UseBackstageRow(use ? k : -1);
    g_pending_own_row = -1;
  }
  g_slobber = g_pending_slobber && rule == kGauntlet && !Rd32(base + kStoryContext);
  g_pending_slobber = false;
  g_roof = g_pending_roof && rule >= kCellFirst && rule <= kCellLast && !Rd32(base + kStoryContext);
  g_pending_roof = false;
  if (g_roof) REXLOG_INFO("match types: HELL IN A CELL - START ON THE ROOF (rule {:02X})", rule);
  g_ge = g_pending_ge && rule == kGauntlet && !Rd32(base + kStoryContext);
  g_pending_ge = false;
  if (g_ge) {
    REXLOG_INFO("match types: GAUNTLET ELIMINATOR");
    svr2011::GauntletEliminatorStart();
  }
  if (g_slobber) {
    REXLOG_INFO("match types: SLOBBER KNOCKER");
    svr2011::SlobberKnockerStart();
  }
  svr2011::ThreeStagesSetup(g_pending_stages && StagesRule(rule) && !Rd32(base + kStoryContext), g_pending_set);
  g_pending_stages = false;
  svr2011::MysteryOpponentSetup(base, g_pending_mystery && rule == 0x00 && !Rd32(base + kStoryContext));
  g_pending_mystery = false;
  g_elimination = g_pending_elimination && (rule == kTripleThreat || rule == kFatal4Way) && !Rd32(base + kStoryContext);
  g_pending_elimination = false;
  if (g_elimination) REXLOG_INFO("match types: ELIMINATION (rule {:02X})", rule);
  g_weapons = g_pending_weapons && WeaponsRule(rule) && !Rd32(base + kStoryContext);
  g_pending_weapons = false;
  if (g_weapons) REXLOG_INFO("match types: WEAPONS EVERYWHERE (rule {:02X})", rule);
  const uint32_t played = !g_free_roam ? rule : rule == kWholeBackstage ? 0x1Bu : 0x70u;
  if (g_free_roam) REXLOG_INFO("match types: free-roaming backstage, played as rule {:02X}", played);
  if (g_pending_like != ~0u && ((rule >= g_pending_rule_lo && rule <= g_pending_rule_hi) ||
                         (!g_pending_select_only && rule == kWholeBackstage2)))
    ShapeRule(base, played, g_pending_like, g_pending_select_only);
  // 6-MAN WEAPONS EVERYWHERE: a free-for-all - all six in the ring, each for
  // himself. Its rule is 6-MAN's normal one, the 6-man tag 0x08 (two of each
  // team on the apron). B11 set only the record's +8 (2, "everyone legal"),
  // but the game's own tornado 6-man (0x0B) has +8 0 too - its tornado bytes
  // are in the option record - and the 2.1.5 report still saw a tag match. So
  // the record takes the shape of the 6-man battle royal (0x23: six teams,
  // the free-for-all option), as 6-MAN's backstage rows do (ShapeRule: the
  // slots and teams, the match family, the option record's people count and
  // free-for-all byte); back after the match (RestoreRule).
  constexpr uint32_t kSixTag = 0x08, kSixFreeForAll = 0x23;
  if (g_weapons && rule == kSixTag) {
    ShapeRule(base, rule, kSixFreeForAll, false);
    REXLOG_INFO("match types: WEAPONS EVERYWHERE - 6-man as a free-for-all (shaped like rule {:02X})", kSixFreeForAll);
  }
  ctx.r4.u64 = played;
  __imp__sub_827374A0(ctx, base);
}

// -- Lumberjack outside Road to WrestleMania ------------------------------
//
// The story mode's match script (sub_822C3D08 -> sub_822BB158, story mode
// only) marks the lumberjacks (team 3) for the AI: character +2572 -> +168 =
// 1. Done here for every Lumberjack match, from the game's per-character
// "is a competitor" check (sub_82225D68(character)), which every match runs.
namespace {

void MarkLumberjacks(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kCharCount = 0x82E3CD0C;
  if (base[kLive] != kLumberjack || int32_t(Rd32(base + kCharCount)) <= 0) return;
  const auto saved = ctx;
  for (uint32_t i = 0; i < 2 + kMaxLumberjacks; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    const uint32_t ai = Rd32(base + ch + 2572);
    if (!ai || Rd32(base + ai + 168) == 1) continue;
    ctx.r3.u64 = Rd32(base + ch + 1156);
    sub_8257C190(ctx, base);
    sub_82573EC8(ctx, base);
    const uint32_t info = ctx.r3.u32;
    if (info && base[info + 13] == 3) {
      Wr32(base + ai + 168, 1);
      REXLOG_INFO("match types: lumberjack {} stays ringside (character {:08X})", i, ch);
    }
  }
  ctx = saved;
}

}  // namespace

// A per-character check (sub_825EEA80(?, character)) that, in a Lumberjack
// match, says no for the lumberjacks - people 3 and up in the story match;
// here from person 2 (the third person is a lumberjack too).
REX_EXTERN(__imp__sub_825EEA80);
REX_HOOK_RAW(sub_825EEA80) {
  const uint32_t ch = ctx.r4.u32;
  __imp__sub_825EEA80(ctx, base);
  if (base[0x82E3DE00] == kLumberjack && ch && Rd32(base + ch + 1156) >= 2) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82225D68);
REX_HOOK_RAW(sub_82225D68) {
  MarkLumberjacks(ctx, base);
  __imp__sub_82225D68(ctx, base);
}

// The lumberjacks: as the match's people are built (sub_828BBEF0(match):
// its picks, 2116-byte slots at match+432 - +8 superstar id * 100 + attire
// (51200: none), +54 the id, +5 the team, +4 the kind, -8 the controller
// (1: CPU) - become people), slots 2-5 of a Lumberjack match (manager slots:
// the select screen leaves them empty) get 4 random superstars - selectable,
// not DLC, not the two picked, of the picked wrestlers' gender (a diva match
// gets divas) - as CPU managers of team 3 (ringside).
namespace {

constexpr uint32_t kSlots = 432, kSlotSize = 2116;
uint32_t g_lumberjack_ids[kMaxLumberjacks] = {};

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

// The lumberjacks' slot kind: 2, managers - they stay at ringside (as
// wrestlers, kind 0, they get in and brawl). (test aid:
// SVR2011_TEST_LJ_KIND=<n>)
uint8_t LumberjackKind() {
  static const int kind = [] { const char* v = std::getenv("SVR2011_TEST_LJ_KIND"); return v ? std::atoi(v) : 2; }();
  return uint8_t(kind);
}

// (each time the people are built - the choice is kept unless it clashes with
// a pick)
void FillRandomSlots(uint8_t* base, uint32_t match, uint32_t count, uint8_t team, uint8_t kind, const char* what);

void FillLumberjackSlots(uint8_t* base, uint32_t match) {
  FillRandomSlots(base, match, LumberjackCount(), 3, LumberjackKind(), "lumberjacks");
}

// People 2.. of the match's slots: `count` random superstars (not the picks,
// their gender), CPU, on `team` with slot kind `kind`.
void FillRandomSlots(uint8_t* base, uint32_t match, uint32_t count, uint8_t team, uint8_t kind, const char* what) {
  constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260;
  constexpr uint32_t kOwnId = 32, kName = 34, kGender = 208, kSelectable = 221, kSamePerson = 228, kDlc = 257;
  uint8_t* slot[2 + kMaxLumberjacks];
  for (uint32_t i = 0; i < 2 + count; ++i) slot[i] = base + match + kSlots + i * kSlotSize;
  const uint32_t picked[2] = {Rd32(slot[0] + 8) / 100, Rd32(slot[1] + 8) / 100};  // (+8: what the people are built from)
  auto record = [&](uint32_t id) -> const uint8_t* {
    if (id >= 1000) return nullptr;
    const uint32_t index = svr2011::RosterIndex(base, id);
    if (index >= 1000) return nullptr;
    const uint8_t* rec = base + svr2011::RosterEntry(kRecords, kRecordSize, index);
    return Rd16(rec + kOwnId) == id ? rec : nullptr;
  };
  const uint8_t* first = record(picked[0]);
  const uint32_t gender = first ? first[kGender] : 0;
  // (one of the picks, under another id: the same name or the same person, +228)
  auto is_picked = [&](uint32_t id) {
    const uint8_t* rec = record(id);
    if (!rec) return false;
    for (uint32_t p : picked) {
      const uint8_t* pr = record(p);
      if (id == p || (pr && (std::strncmp(reinterpret_cast<const char*>(rec + kName), reinterpret_cast<const char*>(pr + kName), 32) == 0 ||
                             (Rd16(rec + kSamePerson) && Rd16(rec + kSamePerson) == Rd16(pr + kSamePerson)) ||
                             Rd16(rec + kSamePerson) == p || Rd16(pr + kSamePerson) == id)))
        return true;
    }
    return false;
  };
  bool keep = g_lumberjacks_chosen;
  for (uint32_t j = 0; j < count; ++j) {
    const uint32_t id = g_lumberjack_ids[j];
    const uint8_t* rec = record(id);
    if (!rec || is_picked(id) || rec[kGender] != gender) keep = false;
  }
  std::string names;
  if (!keep) {
    std::vector<uint32_t> pool;
    for (uint32_t id = 1; id < 1000; ++id) {
      const uint8_t* rec = record(id);
      if (rec && rec[kSelectable] == 1 && !rec[kDlc] && rec[kGender] == gender && !is_picked(id))
        pool.push_back(id);
    }
    if (pool.size() < count) {
      REXLOG_WARN("match types: {} - only {} superstars to choose from", what, pool.size());
      return;
    }
    static std::mt19937 rng{std::random_device{}()};
    std::shuffle(pool.begin(), pool.end(), rng);
    uint32_t chosen = 0;
    std::vector<std::string> seen;  // (distinct names: one superstar under two ids once)
    for (uint32_t id : pool) {
      if (chosen == count) break;
      const std::string name(reinterpret_cast<const char*>(record(id) + kName), strnlen(reinterpret_cast<const char*>(record(id) + kName), 32));
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      seen.push_back(name);
      g_lumberjack_ids[chosen++] = id;
      names += fmt::format("{}{}", names.empty() ? "" : ", ", name);
    }
    if (chosen < count) return;
    g_lumberjacks_chosen = true;
  }
  for (uint32_t i = 2; i < 2 + count; ++i) {
    const uint32_t id = g_lumberjack_ids[i - 2], now = Rd32(slot[i] + 8);
    if (now != 51200 && now != id * 100 + 2) continue;  // (not an empty slot: someone's pick)
    Wr32(slot[i] + 8, id * 100 + 2);  // (attire: the first, as the select screen gives)
    Wr16(slot[i] + 54, id);
    slot[i][5] = team;
    slot[i][4] = kind;
    slot[i][-8] = 1;     // (controller: the CPU)
  }
  if (!names.empty()) REXLOG_INFO("match types: {}: {}", what, names);
}

}  // namespace

REX_EXTERN(__imp__sub_828BBEF0);
REX_HOOK_RAW(sub_828BBEF0) {
  if (std::getenv("SVR2011_TEST_PEOPLE")) {  // (test aid: the match's slots 0-5, 64 bytes each)
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t slot = ctx.r3.u32 + 432 + i * 2116;
      std::string hex;
      for (uint32_t at = 0; at < 64; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : " ", base[slot - 8 + at]);
      REXLOG_INFO("[svr2011] people: match {:08X} slot {} (from -8):{}", ctx.r3.u32, i, hex);
    }
  }
  if (base[0x82E3DE00] == kLumberjack && !Rd32(base + kStoryContext) && ctx.r3.u32) {
    FillLumberjackSlots(base, ctx.r3.u32);
  }
  if (g_slobber && g_online_to != kGauntlet && base[0x82E3DE00] == kGauntlet && ctx.r3.u32) {  // (the 2 waiting opponents; online: the lobby's)
    if (const uint32_t rules = Rd32(base + kRules))
      for (int i = 2; i < kSlobberPeople; ++i) base[rules + kGauntlet * kRuleSize + 2 + i * 3 + 2] = 3;
    FillRandomSlots(base, ctx.r3.u32, kSlobberPeople - 2, 1, 3, "slobber knocker opponents");
    for (uint32_t i = kSlobberPeople; i < 6; ++i) Wr32(base + ctx.r3.u32 + kSlots + i * kSlotSize + 8, 51200);
  }
  if (g_ge && base[0x82E3DE00] == kGauntlet && ctx.r3.u32) {  // (the 4 entrants: random, each his own team)
    if (const uint32_t rules = Rd32(base + kRules))
      for (int i = 2; i < kGePeople; ++i) base[rules + kGauntlet * kRuleSize + 2 + i * 3 + 2] = 3;
    FillRandomSlots(base, ctx.r3.u32, kGePeople - 2, 2, 3, "gauntlet eliminator entrants");
    for (uint32_t i = 2; i < uint32_t(kGePeople); ++i) base[ctx.r3.u32 + kSlots + i * kSlotSize + 5] = uint8_t(i);
  }
  svr2011::MysteryOpponentFill(base, ctx.r3.u32);  // (mystery_opponent.h)
  __imp__sub_828BBEF0(ctx, base);
}

// ONLINE: the match's slots are filled from the lobby (sub_828BB290(match),
// not the offline people builder above): GAUNTLET ELIMINATOR's entrants
// (people 2-5) are waiting ones and everyone is his own team, as offline.
// The person's kind is the slot's +0 word (the rule record's kind, copied
// by the set-up sub_828BD278 from the match's rule at +8 - online the
// session's 6-man, kinds 0): 3 = a waiting entrant (role 3, state 1 when the
// character is made). (+4 is the slot's no-pad / CPU flag.)
// Test aid (a LAN test, tools/dev/svrtest_lan.ps1): online, SVR2011_TEST_RULE plays where the session's ONE ON
// ONE NORMAL would - the online set-up gives the rule to sub_828BD278(match, rule, options) itself (not
// sub_827374A0) - alike on both games (with SVR2011_TEST_BACKSTAGE_ROW, a backstage mod's row for its area).
REX_EXTERN(__imp__sub_828BD278);
REX_HOOK_RAW(sub_828BD278) {
  if (g_test_rule >= 0 && ctx.r4.u32 == 0 && svr2011::P2POnlineMode() && !Rd32(base + kStoryContext)) {
    ctx.r4.u64 = uint32_t(g_test_rule);
    static int told = 0;
    if (told++ < 4) REXLOG_INFO("match types: online ONE ON ONE NORMAL plays rule {:02X} (SVR2011_TEST_RULE)", g_test_rule);
    // (and SVR2011_TEST_BACKSTAGE_ROW's row when the rule is its area's: online, the menus' own-row
    // choice in sub_827374A0's hook isn't made)
    if (const char* v = std::getenv("SVR2011_TEST_BACKSTAGE_ROW"); v && *v) {
      const auto& own = svr2011::BackstageRows();
      for (size_t i = 0; i < own.size(); ++i)
        if (own[i].label == v && uint32_t(g_test_rule) == kBackstage1v1FirstRule + uint32_t(own[i].area)) {
          svr2011::UseBackstageRow(int(i));
          REXLOG_INFO("match types: online backstage area '{}' (SVR2011_TEST_BACKSTAGE_ROW)", own[i].label);
        }
    }
  }
  __imp__sub_828BD278(ctx, base);
}

REX_EXTERN(__imp__sub_828BB290);
REX_HOOK_RAW(sub_828BB290) {
  const uint32_t match = ctx.r3.u32;
  __imp__sub_828BB290(ctx, base);
  if (g_ge && g_online_to == kGauntlet && match) {
    for (uint32_t i = 0; i < uint32_t(kGePeople); ++i) {
      uint8_t* slot = base + match + kSlots + i * kSlotSize;
      slot[5] = uint8_t(i);
      if (i >= 2) slot[0] = 0, slot[1] = 0, slot[2] = 0, slot[3] = 3;
    }
    REXLOG_INFO("match types: online GAUNTLET ELIMINATOR - people 2-5 waiting entrants, each his own team");
  }
  // SLOBBER KNOCKER (co-op): the players (0, 1) team 0; the CPUs team 1 -
  // person 2 in the ring, 3-5 waiting (kind 3) to come in in turn
  if (g_slobber && g_online_to == kGauntlet && match) {
    for (uint32_t i = 0; i < 6; ++i) {
      uint8_t* slot = base + match + kSlots + i * kSlotSize;
      slot[5] = i < 2 ? 0 : 1;
      slot[0] = 0, slot[1] = 0, slot[2] = 0, slot[3] = i >= 3 ? 3 : 0;
    }
    REXLOG_INFO("match types: online SLOBBER KNOCKER - the players a team against the CPUs, 3 waiting");
  }
}

// -- The whole backstage ---------------------------------------------------
//
// The free-roam backstage stage (arena 78) has every room loaded; a task
// ('CFRS', sub_825310B8 every frame) hides them all but the corridor, the
// interview set and the one room at a position - the walking player's in
// Road to WrestleMania, the match camera's otherwise - so a fight elsewhere
// looks into black rooms. For a match in no named area (rule 0x19 / 0x1A:
// the whole backstage), every room is shown after the update:
// sub_8252FFF0(task, room 0-9, show), less the objects the game always hides.
REX_EXTERN(__imp__sub_825310B8);
REX_HOOK_RAW(sub_825310B8) {
  const uint32_t task = ctx.r3.u32 - 32;
  __imp__sub_825310B8(ctx, base);
  svr2011::HctpWorldVisibility(ctx, base);
  constexpr uint32_t kLive = 0x82E3DE00, kStory = 0x82E3C1F4;
  if (Rd32(base + kLive + 64) != 78 || !g_free_roam || Rd32(base + kStory)) return;
  const auto saved = ctx;
  for (uint32_t room : {0u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
    ctx.r3.u64 = task;
    ctx.r4.u64 = room;
    ctx.r5.u64 = 1;
    sub_8252FFF0(ctx, base);
  }
  // The objects the update always hides stay hidden: sub_8252FF40(lo, hi, 0).
  for (uint32_t id : {6u, 25u, 43u, 72u, 85u, 172u, 184u, 197u, 198u}) {
    ctx.r3.u64 = id;
    ctx.r4.u64 = id;
    ctx.r5.u64 = 0;
    sub_8252FF40(ctx, base);
  }
  ctx = saved;
}

// -- The backstage camera --------------------------------------------------
//
// In-match cameras are made by sub_82313D98: the backstage camera (follows
// the fighters, turns with them) when sub_821852E0() says the match is
// backstage, the ring camera otherwise. sub_821852E0 leaves out the whole
// backstage (rule 0x19), which then gets the ring camera: far, high and
// fixed - it looks down onto the room boxes. While the cameras are made, the
// whole backstage counts as backstage.
namespace {

constexpr uint32_t kLiveSettings = 0x82E3DE00;
bool g_making_cameras = false;

bool WholeBackstage(uint8_t* base) { return Rd32(base + kLiveSettings + 64) == 78 && g_free_roam; }

// (test aids: a fighter put at x, y, z - his base +288 and the body's three copies (+0, +704, +768, hctp_room) -
// in area `area` (+444: 0 the ring, 1 outside); a paired start keeps such places)
void PutFighter(uint8_t* base, uint32_t f, float x, float y, float z, uint8_t area = 0) {
  WrF(base + f + 288, x), WrF(base + f + 292, y), WrF(base + f + 296, z);
  if (const uint32_t body = Rd32(base + f + 304))
    for (uint32_t at : {0u, 704u, 768u}) WrF(base + body + at, x), WrF(base + body + at + 4, y), WrF(base + body + at + 8, z);
  base[f + 444] = area;
}

}  // namespace

REX_EXTERN(__imp__sub_82313D98);
REX_HOOK_RAW(sub_82313D98) {
  g_making_cameras = true;
  __imp__sub_82313D98(ctx, base);
  g_making_cameras = false;
}

REX_EXTERN(__imp__sub_821852E0);
REX_HOOK_RAW(sub_821852E0) {
  if (WholeBackstage(base) && !Rd32(base + 0x82E3C1F4)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_821852E0(ctx, base);
}

// The backstage camera's distance: sub_822F26B8(camera) sets camera+932 =
// the framed radius x 3.7. A little closer in the free-roaming backstage:
// x 3.2 (x 2.4 was "really zoomed in"); a backstage mod's area: at most its
// camera= distance.
REX_EXTERN(__imp__sub_822F26B8);
REX_HOOK_RAW(sub_822F26B8) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_822F26B8(ctx, base);
  // A backstage mod's own area may cap it (manifest camera=): a small area
  // ringed by objects (the 2008 parking lot's cars) - farther out the
  // camera would stand among them.
  if (svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const float cap = svr2011::BackstageRows()[size_t(svr2011::g_active_row)].camera;
    if (cap > 0 && RdF(base + camera + 932) > cap) WrF(base + camera + 932, cap);
  }
  if (!WholeBackstage(base)) return;
  constexpr float kCloser = 3.2f / 3.7f;
  WrF(base + camera + 932, RdF(base + camera + 932) * kCloser);
}

// A backstage mod's own area with camera_height=: the match camera's eye is
// at least that high. The backstage camera's frame update (sub_822F2A38, the
// camera in r3) puts the eye at +304..+312 (the framed centre + its rotation
// x the distance; for rule 1B always on the +z side, looking toward -z), the
// look direction at +288..+296, and builds the view from them
// (sub_82227150(0x82E3CC00, cam+304, cam+288, ...)); the target (the framed
// centre) is on its stack at +128/+132/+136. There the eye is raised (-Y is
// up) and the direction re-aimed at the target: in the 2008 parking lot the
// camera, a radius x 3.7 out on the +z side, otherwise stood behind the
// vehicles on that side. (Clamping the eye inside the area, or putting it on
// the area's centre side, gave extreme close-ups.)
uint32_t g_camera_frame = 0;
// After the fighters jump (SnapMatchCamera: through an HCTP door into another room, 1000s away), the
// eye follows them at once: for kSnapFrames views it is put at the framed target + the eye's offset
// from it before the jump (x / z), also in its kept copy (+96 / +104), so the camera's own easing
// carries on from there. (Moving the eye once didn't hold: it was rebuilt and panned across.)
constexpr int kSnapFrames = 30;
int g_camera_snap = 0;
float g_camera_offset[2] = {};
bool g_camera_offset_set = false;
REX_EXTERN(__imp__sub_822F2A38);
REX_HOOK_RAW(sub_822F2A38) {
  g_camera_frame = ctx.r3.u32;
  __imp__sub_822F2A38(ctx, base);
  g_camera_frame = 0;
}
REX_EXTERN(__imp__sub_82227150);
REX_HOOK_RAW(sub_82227150) {
  // Test aid: SVR2011_TEST_VIEW="<eye x,y,z>,<target x,y,z>" - every scene view from there (y up is negative), to
  // look at one place of an arena in a match (the announce tables).
  static const std::vector<float> test_view = [] {
    std::vector<float> v;
    if (const char* s = std::getenv("SVR2011_TEST_VIEW"))
      for (const char* p = s; *p;) {
        v.push_back(float(std::atof(p)));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
      }
    return v;
  }();
  // (SVR2011_TEST_VIEW_PAN=<units a frame>: eye and target moved along x with the match's clock - a camera pan the
  // same every run, for FSR 2's motion vectors)
  static const float test_pan = std::getenv("SVR2011_TEST_VIEW_PAN") ? float(std::atof(std::getenv("SVR2011_TEST_VIEW_PAN"))) : 0.0f;
  if (test_view.size() == 6 && ctx.r3.u32 == 0x82E3CC00 && Rd32(base + 0x82E3CD0C) > 0) {
    const float dx = test_view[3] - test_view[0], dy = test_view[4] - test_view[1], dz = test_view[5] - test_view[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float pan = test_pan * float(Rd32(base + 0x82E3CD0C) % 600);
    for (int k = 0; k < 3 && len > 1e-3f; ++k) {
      WrF(base + ctx.r4.u32 + 4 * uint32_t(k), test_view[size_t(k)] + (k == 0 ? pan : 0.0f));
      WrF(base + ctx.r5.u32 + 4 * uint32_t(k), (k == 0 ? dx : k == 1 ? dy : dz) / len);
    }
  }
  const uint32_t cam = g_camera_frame;
  if (cam && ctx.r3.u32 == 0x82E3CC00 && ctx.r4.u32 == cam + 304 && ctx.r5.u32 == cam + 288) {
    const uint32_t sp = ctx.r1.u32;  // (still sub_822F2A38's frame: the framed target at +128 / +136)
    const float tx = RdF(base + sp + 128), tz = RdF(base + sp + 136);
    if (g_camera_snap > 0 && g_camera_offset_set) {
      --g_camera_snap;
      for (uint32_t at : {96u, 304u}) {
        WrF(base + cam + at, tx + g_camera_offset[0]);
        WrF(base + cam + at + 8, tz + g_camera_offset[1]);
      }
    } else {
      g_camera_offset[0] = RdF(base + cam + 304) - tx, g_camera_offset[1] = RdF(base + cam + 312) - tz;
      g_camera_offset_set = true;
    }
  }
  if (cam && ctx.r3.u32 == 0x82E3CC00 && ctx.r4.u32 == cam + 304 && ctx.r5.u32 == cam + 288 &&
      svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
    const float height = row.camera_height;
    float ey = RdF(base + cam + 308);
    if (height > 0 && ey > -height) {
      const uint32_t sp = ctx.r1.u32;  // (still sub_822F2A38's frame)
      const float tx = RdF(base + sp + 128), ty = RdF(base + sp + 132), tz = RdF(base + sp + 136);
      const float ex = RdF(base + cam + 304), ez = RdF(base + cam + 312);
      ey = -height;
      WrF(base + cam + 308, ey);
      const float dx = tx - ex, dy = ty - ey, dz = tz - ez;
      const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (len > 1e-3f) {
        WrF(base + cam + 288, dx / len);
        WrF(base + cam + 292, dy / len);
        WrF(base + cam + 296, dz / len);
      }
    }
  }
  const uint32_t view = ctx.r3.u32, eye = ctx.r4.u32, dir = ctx.r5.u32;
  svr2011::MatchCameraBefore(base, uint32_t(ctx.lr), view, eye, dir);  // (MATCH CAMERA: free_cam.h)
  __imp__sub_82227150(ctx, base);
  svr2011::MatchCameraAfter(base);
  svr2011::FreeCamSceneView(base, view, eye, dir);  // (the free camera's, F6: free_cam.h)
}

// What the match camera frames: sub_82227690(camera) -> a sphere (centre
// +16/+20/+24, radius +32) around every competitor. Backstage with more than
// two people, it is centred on their average position instead (radius: the
// farthest of them from it). In an HCTP room's row (hctp=), fighters at
// different heights (one up on the cage top, -Y up) are framed head to feet: the centre
// between the lowest feet and the highest head, the radius at least half that.
REX_EXTERN(__imp__sub_82227690);
REX_HOOK_RAW(sub_82227690) {
  __imp__sub_82227690(ctx, base);
  const uint32_t sphere = ctx.r3.u32;
  if (sphere)  // (MATCH CAMERA's RAMP: free_cam.h)
    svr2011::MatchCameraSphere(RdF(base + sphere + 16), RdF(base + sphere + 20), RdF(base + sphere + 24),
                               RdF(base + sphere + 32));
  if (!sphere || Rd32(base + kLiveSettings + 64) != 78) return;
  constexpr uint32_t kChars = 0x82E3CC50;
  float xs[9], ys[9], zs[9];
  int n = 0;
  const auto saved = ctx;
  for (uint32_t i = 0; i < 9; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    ctx.r3.u64 = ch;
    sub_82225D68(ctx, base);
    const bool competitor = ctx.r3.u32 != 0;
    const uint32_t pos = Rd32(base + ch + 304);
    if (!competitor || !pos) continue;
    xs[n] = RdF(base + pos);
    ys[n] = RdF(base + pos + 4);
    zs[n] = RdF(base + pos + 8);
    ++n;
  }
  ctx = saved;
  ctx.r3.u64 = sphere;
  if (n == 0) return;
  // (a wrestler stands about 19 tall)
  float top = ys[0], feet = ys[0];
  for (int i = 1; i < n; ++i) top = std::min(top, ys[i]), feet = std::max(feet, ys[i]);
  // (an HCTP room's row only: 2011's own areas keep the game's framing)
  const bool hctp_row = svr2011::g_active_row >= 0 &&
                        size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
                        svr2011::BackstageRows()[size_t(svr2011::g_active_row)].hctp > 0;
  const auto frame_heights = [&] {
    if (!hctp_row || feet - top <= 8.0f) return;
    const float head = top - 19.0f;
    WrF(base + sphere + 20, (head + feet) / 2);
    if (RdF(base + sphere + 32) < (feet - head) / 2 + 4.0f) WrF(base + sphere + 32, (feet - head) / 2 + 4.0f);
  };
  if (n <= 2) {
    frame_heights();
    return;
  }
  // Around the fighters near the player (1P: the first competitor): their
  // average; anyone farther away (another fight elsewhere) is left out, so
  // the camera stays close to the player's fight.
  const float px = xs[0], pz = zs[0];
  constexpr float kNear = 220.0f, kMaxRadius = 90.0f;
  float mx = 0, mz = 0;
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    if (std::hypot(xs[i] - px, zs[i] - pz) > kNear) continue;
    mx += xs[i];
    mz += zs[i];
    ++kept;
  }
  mx /= float(kept);
  mz /= float(kept);
  float r = 0;
  for (int i = 0; i < n; ++i)
    if (std::hypot(xs[i] - px, zs[i] - pz) <= kNear) r = std::max(r, std::hypot(xs[i] - mx, zs[i] - mz));
  r = std::min(r + 12.0f, kMaxRadius);
  WrF(base + sphere + 16, mx);
  WrF(base + sphere + 24, mz);
  WrF(base + sphere + 32, r);
  frame_heights();
}

// -- The whole backstage's fight box --------------------------------------
//
// A backstage match (sub_821852E0() true) keeps its fighters in a box set up
// by sub_8224EF28(box) for its area: centre box+32/+36/+40, half sizes +64
// inner x, +68 inner z, +72 outer x, +76 outer z (also copied to
// 0x82D9E9D4..E0, which the per-body clamp sub_821E5408 and the AI read).
// The whole backstage has no area there - the box would stay a point - so it
// gets a box around all of it: the walls of the whole backstage (bg78 wall
// mesh 0, x -530..510, z -550..470) keep the fighters in, as in story mode.
uint32_t g_fight_box = 0;  // (the last box set up: SetFightBox moves it)
REX_EXTERN(__imp__sub_8224EF28);
REX_HOOK_RAW(sub_8224EF28) {
  const uint32_t box = ctx.r3.u32;
  __imp__sub_8224EF28(ctx, base);
  if (box) g_fight_box = box;
  if (box && std::getenv("SVR2011_TEST_BOX_LOG"))  // (test aid: the area's own box)
    REXLOG_INFO("match types: area box centre {:.0f} {:.0f} {:.0f}, half {:.0f} {:.0f} / {:.0f} {:.0f}",
                RdF(base + box + 32), RdF(base + box + 36), RdF(base + box + 40), RdF(base + box + 64),
                RdF(base + box + 68), RdF(base + box + 72), RdF(base + box + 76));
  // A backstage mod's own area with its own box (manifest box=): centred on
  // it, the area's half sizes inside, a little more outside.
  if (box && svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
    if (row.has_box) {
      WrF(base + box + 32, row.box[0]);
      WrF(base + box + 40, row.box[1]);
      const float half[4] = {row.box[2], row.box[3], row.box[2] + 5.0f, row.box[3] + 5.0f};
      for (uint32_t i = 0; i < 4; ++i) {
        WrF(base + box + 64 + i * 4, half[i]);
        WrF(base + 0x82D9E9D4 + i * 4, half[i]);
      }
      REXLOG_INFO("match types: '{}' fight box centre {:.0f} {:.0f}, half {:.0f} {:.0f}", row.label, row.box[0],
                  row.box[1], row.box[2], row.box[3]);
      return;
    }
  }
  if (!box || !WholeBackstage(base) || Rd32(base + 0x82E3C1F4)) return;
  WrF(base + box + 32, -10.0f);
  WrF(base + box + 36, 0.0f);
  WrF(base + box + 40, -40.0f);
  WrF(base + box + 44, 1.0f);
  float kHalf[4] = {520.0f, 510.0f, 525.0f, 515.0f};
  if (const char* v = std::getenv("SVR2011_TEST_BOX")) {  // (test aid: half sizes)
    const float h = float(std::atof(v));
    for (float& f : kHalf) f = h;
  }
  constexpr uint32_t kGlobals = 0x82D9E9D4;
  for (uint32_t i = 0; i < 4; ++i) {
    WrF(base + box + 64 + i * 4, kHalf[i]);
    WrF(base + kGlobals + i * 4, kHalf[i]);
  }
  REXLOG_INFO("match types: the whole backstage's fight box covers all of it");
}

// -- Lumberjack start places ----------------------------------------------
//
// People start where sub_822AD738(placer, rule, ?) puts them: the placer's
// table (+4) has an 88-byte entry per rule id - +0 the id, then per person
// 12 bytes (+0 kind, 1 wrestler / 2 manager; int16 x, y, z, facing at +4..;
// y -120 in the ring, 0 on the floor; tenths). The story Lumberjack's entry
// (0x55) has 3 wrestlers in the ring and its lumberjacks on the floor around
// it: (50,0,500), (-500,0,0), (0,0,-500). Here person 2 is a lumberjack too:
// on the fourth side, (500, 0, 0), facing the ring.
// The match in the log (once per placement - the match's start): rule, arena,
// each person: name (from the person's roster record), person number, team.
void LogMatch(PPCContext& ctx, uint8_t* base, uint32_t rule) {
  constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kRoster = 0x82EDEA88;
  const auto saved = ctx;
  std::string people, setup;  // (setup: person : superstar id - online_chat.h OnlineMatchSetup)
  svr2011::ClearMatchPeople();  // (a frozen match's report: match_diag.h)
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    const uint32_t id = Rd32(base + ch + 1156);
    std::string name;
    uint32_t superstar = 0xFFFF;  // (the record's +0x20: the superstar id)
    if (const uint32_t roster = Rd32(base + kRoster)) {
      ctx.r3.u64 = roster;
      ctx.r4.u64 = id;
      sub_828B5A18(ctx, base);
      if (const uint32_t rec = ctx.r3.u32) {  // (the record's name: its text at +0x22; +0x20 is a number)
        if (std::getenv("SVR2011_TEST_ROSTER_DUMP")) {  // (test aid: the record, 260 bytes)
          std::string hex;
          for (uint32_t at = 0; at < 260; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : fmt::format(" +{:03X}:", at), base[rec + at]);
          REXLOG_INFO("[svr2011] match: test - roster record {} at {:08X}:{}", id, rec, hex);
        }
        for (uint32_t at = 0x22; at < 0x22 + 64 && base[rec + at] >= 0x20 && base[rec + at] < 0x7F; ++at)
          name += char(base[rec + at]);
        superstar = uint32_t(base[rec + 0x20]) << 8 | base[rec + 0x21];
      }
    }
    ctx.r3.u64 = id;
    sub_8257C190(ctx, base);
    sub_82573EC8(ctx, base);
    const uint32_t info = ctx.r3.u32;
    svr2011::NoteMatchPerson(i, superstar, name.empty() ? "?" : name);
    setup += fmt::format(" {}:{}", id, superstar);
    people += fmt::format("{}{} (person {}, team {})", people.empty() ? "" : ", ", name.empty() ? "?" : name, id,
                          info ? base[info + 13] : 255);
  }
  ctx = saved;
  std::string line = fmt::format("rule {:02X}, arena {} - {}", rule, Rd32(base + kLive + 64), people);
  svr2011::g_match_rule = rule, svr2011::g_match_arena = Rd32(base + kLive + 64);
  static std::string last;  // (placed three times as a match loads: logged once)
  if (line == last) return;
  last = line;
  REXLOG_INFO("[svr2011] match: {}", line);
  svr2011::OnlineMatchSetup(fmt::format("rule {:02X}, arena {},{}", rule, Rd32(base + kLive + 64), setup));
}

REX_EXTERN(__imp__sub_822AD738);
REX_HOOK_RAW(sub_822AD738) {
  const uint32_t placer = ctx.r3.u32;
  svr2011::SetMatchLoading(false);  // (its loading screen is up)
  svr2011::LoadWatchDone("a match placed");
  svr2011::g_match_placements.fetch_add(1);
  LogMatch(ctx, base, ctx.r4.u32);
  // Research aid (Hell in a Cell roof): SVR2011_TEST_CELL_START=<y tenths> -
  // in a cell match people 0 and 1 start at that height (x -150 / +150, z 0).
  if (static const char* cy = std::getenv("SVR2011_TEST_CELL_START");
      cy && placer && ctx.r4.u32 >= 0x31 && ctx.r4.u32 <= 0x35) {
    uint8_t* entry = base + Rd32(base + placer + 4) + ctx.r4.u32 * 88 + 4;
    uint8_t saved[24];
    std::memcpy(saved, entry, 24);
    const int16_t y = int16_t(std::atoi(cy));
    for (int i = 0; i < 2; ++i) {
      uint8_t* p = entry + i * 12;
      const int16_t v[4] = {int16_t(i ? 150 : -150), y, 0, int16_t(i ? 2700 : 900)};
      for (int k = 0; k < 4; ++k) p[4 + k * 2] = uint8_t(uint16_t(v[k]) >> 8), p[5 + k * 2] = uint8_t(v[k]);
    }
    REXLOG_INFO("match types: cell test - the two start at height {} (tenths); were {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", y,
                saved[4], saved[5], saved[6], saved[7], saved[8], saved[9], saved[10], saved[11]);
    __imp__sub_822AD738(ctx, base);
    std::memcpy(entry, saved, 24);
    return;
  }
  // A backstage mod's own area with start=: its fighters' places (x, z,
  // facing; tenths, y 0) for the match, the room's table back after.
  if (placer && !Rd32(base + kStoryContext) && svr2011::g_active_row >= 0 &&
      size_t(svr2011::g_active_row) < svr2011::BackstageRows().size()) {
    const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
    const uint32_t rule = ctx.r4.u32;
    static int checks = 0;
    if (checks++ < 6)
      REXLOG_INFO("match types: start places? rule {:02X} area {} starts {}", rule, row.area, row.starts.size());
    if (!row.starts.empty() && rule == kBackstage1v1FirstRule + uint32_t(row.area)) {
      uint8_t* people = base + Rd32(base + placer + 4) + rule * 88 + 4;
      const size_t n = std::min<size_t>(row.starts.size(), 7);
      uint8_t saved[7 * 12];
      std::memcpy(saved, people, n * 12);
      for (size_t i = 0; i < n; ++i) {
        uint8_t* p = people + i * 12;
        const auto put = [&](int at, float v) {
          const int16_t t = int16_t(std::lround(v * 10.0f));
          p[at] = uint8_t(uint16_t(t) >> 8), p[at + 1] = uint8_t(t & 0xFF);
        };
        put(4, row.starts[i].x), put(6, row.starts[i].y), put(8, row.starts[i].z), put(10, row.starts[i].facing);
      }
      static int logged = -1;
      if (logged != svr2011::g_active_row) {
        logged = svr2011::g_active_row;
        REXLOG_INFO("match types: '{}' starts at ({:.0f}, {:.0f})...", row.label, row.starts[0].x, row.starts[0].z);
      }
      __imp__sub_822AD738(ctx, base);
      std::memcpy(people, saved, n * 12);
      return;
    }
  }
  if (ctx.r4.u32 != kLumberjack || !placer || Rd32(base + kStoryContext)) {
    __imp__sub_822AD738(ctx, base);
    return;
  }
  uint8_t* person2 = base + Rd32(base + placer + 4) + kLumberjack * 88 + 4 + 2 * 12;
  uint8_t saved[12];
  std::memcpy(saved, person2, 12);
  const uint8_t ringside[12] = {0x01, 0, 0, 0, 0x01, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x03, 0x84};  // 500, 0, 0, 900
  std::memcpy(person2, ringside, 12);
  __imp__sub_822AD738(ctx, base);
  std::memcpy(person2, saved, 12);
}

// Test aid: SVR2011_TEST_PEOPLE=1 - logs sub_828B5E28 (a person record
// built: people, index, ...) with its caller, for the Lumberjack picks work.
REX_EXTERN(__imp__sub_828B5E28);
REX_HOOK_RAW(sub_828B5E28) {
  static const bool on = std::getenv("SVR2011_TEST_PEOPLE") != nullptr;
  if (on)
    REXLOG_INFO("[svr2011] people: sub_828B5E28(r3 {:08X} r4 {} r5 {:08X} r6 {:08X}) from {:08X}", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_828B5E28(ctx, base);
}

// The per-fighter controls (sub_82216C58(control), once a frame each: +32
// the fighter, +36 its input object), remembered per fighter for the
// lumberjacks' controller.
namespace {
struct Control {
  uint32_t fighter = 0, input = 0;
};
Control g_controls[8];
// Attacks the controller asks for: a lumberjack fighter and his wrestler
// (started from the control hook, which has a context).
struct Attack {
  uint32_t fighter = 0, wrestler = 0, at_frame = 0;
};
Attack g_attacks[6];
// Their attacks: paired grapples (sub_82191AA8(fighter, opponent, motion)
// puts both into the same motion, so only paired ones work - a strike, e.g.
// 3128, would make the wrestler throw it too). Motion ids, not misc.pac move
// ids (those froze both). Seen in CPU fights: 120 a front grapple, 8030 and
// 10901 throws that leave him down (6801 -> 50, 449). 1000 is only lying down.
constexpr uint32_t kAttackMotions[] = {120, 8030, 10901};
}  // namespace

REX_EXTERN(__imp__sub_82216C58);
REX_HOOK_RAW(sub_82216C58) {
  const uint32_t control = ctx.r3.u32, fighter = Rd32(base + control + 32), input = Rd32(base + control + 36);
  for (Control& c : g_controls)
    if (c.fighter == fighter || !c.fighter) {
      c.fighter = fighter, c.input = input;
      break;
    }
  __imp__sub_82216C58(ctx, base);
  // Research aid: SVR2011_TEST_AI_INPUT=1 - once a second, people 0 and 1: the control, its input object (+36) and the
  // input's first 64 bytes, and the motion (the CPU stall: does a standing CPU's input still change?)
  if (static const bool ai_input = std::getenv("SVR2011_TEST_AI_INPUT") != nullptr; ai_input && fighter && input) {
    const uint32_t who = Rd32(base + fighter + 1156), frames = Rd32(base + 0x82E3CD0C);
    if (who < 2 && frames % 60 == 0) {
      std::string hex;
      for (uint32_t i = 0; i < 64; ++i) hex += fmt::format("{:02X}", base[input + i]);
      for (uint32_t k : {16u, 20u})  // (the gPAD's buffers: the pressed state)
        if (const uint32_t b = Rd32(base + input + k); b && b < 0xF0000000u) {
          hex += k == 16u ? " | " : " / ";
          for (uint32_t i = 0; i < 48; ++i) hex += fmt::format("{:02X}", base[b + i]);
        }
      REXLOG_INFO("ai input: person {} frame {} control {:08X} input {:08X} motion {} {}", who, frames, control, input,
                  Rd32(base + fighter + 212), hex);
    }
  }
  // A fighter on the floor outside (+444 area 1, base y near 0) never stands under the ring, yet a paired move started
  // by the apron could have its pair's places redone into the ring's footprint (a ported Double Arm DDT 2 from the
  // floor by the apron: the victim's base at (13.4, 8.4), the attacker's slid to 15 from it), and when the move ended
  // the game played the ring entry from there - the victim floated at ring height and climbed in (item 10, 29 Oct).
  // Such a base goes back to the fighter's last one on the floor outside: the move plays where it started.
  // (not backstage - arena 78, the HCTP rooms too: no ring there, the floor round the middle is the room's)
  if (fighter && fighter < 0xF0000000u && Rd32(base + kLiveSettings + 64) != 78) {
    struct Floor {
      uint32_t fighter;
      float x, z;
    };
    static Floor floor_at[16] = {};
    // (empty again each match - the match frame going back: fighters' addresses are reused, and online every peer
    // must start the same whatever it played before; the rest is the synced match state alone)
    static uint32_t last_frame = 0;
    if (const uint32_t frame = Rd32(base + 0x82E3CD0C); frame < last_frame) {
      for (Floor& s : floor_at) s = {};
      last_frame = frame;
    } else {
      last_frame = frame;
    }
    constexpr float kRopes = 28.8f;  // (the ring's footprint: inside the ropes)
    const float x = RdF(base + fighter + 288), y = RdF(base + fighter + 292), z = RdF(base + fighter + 296);
    Floor* f = nullptr;
    for (Floor& s : floor_at)
      if (s.fighter == fighter || (!f && !s.fighter)) f = &s;
    if (f && f->fighter != fighter) *f = {fighter, 0.f, 0.f};
    const bool outside = base[fighter + 444] == 1 && y > -4.f;
    auto clear = [&](float px, float pz) { return std::fabs(px) >= kRopes || std::fabs(pz) >= kRopes; };
    const uint32_t body = Rd32(base + fighter + 304);
    if (f && outside && clear(x, z)) {
      f->x = x, f->z = z;
    } else if (f && outside) {
      // (the move's first frame: the base is redone already, the body still where he stood - Cena thrown out by a
      // test the same frame had no floor place yet, and his slid partner was put back again and again)
      if (body && RdF(base + body + 4) > -4.f && clear(RdF(base + body), RdF(base + body + 8)) && f->x == 0.f && f->z == 0.f)
        f->x = RdF(base + body), f->z = RdF(base + body + 8);
      if (f->x != 0.f || f->z != 0.f) {
        static int logged = 0;
        if (logged++ < 24)
          REXLOG_INFO("match types: fighter {:08X} on the floor outside with his base under the ring ({:.1f}, {:.1f}): "
                      "back to ({:.1f}, {:.1f})", fighter, x, z, f->x, f->z);
        WrF(base + fighter + 288, f->x), WrF(base + fighter + 296, f->z);
      }
    }
  }
  // Test aid: SVR2011_TEST_MOTION_LOG=1 - each fighter's motion (+212) when
  // it changes, with the person, the match frame, where he is (+288) and his
  // area (+444: 0 the ring, 1 outside).
  // SVR2011_TEST_HW_WATCH_MOTION=<person>,<motion> - once that person is in that motion, the hardware write watch
  // (frame_rate.cpp) on his motion word: who starts the motion after it (a ported move's 5873 -> 23231, 7017d6).
  static const std::pair<int, int> watch_motion = [] {
    int p = -1, m = 0;
    if (const char* v = std::getenv("SVR2011_TEST_HW_WATCH_MOTION")) std::sscanf(v, "%d,%d", &p, &m);
    return std::make_pair(p, m);
  }();
  if (watch_motion.first >= 0 && fighter && int(Rd32(base + fighter + 1156)) == watch_motion.first &&
      int(Rd32(base + fighter + 212)) == watch_motion.second)
    svr2011::HwWatchOn(base, fighter + 212);
  static const bool motion_log = std::getenv("SVR2011_TEST_MOTION_LOG") != nullptr;
  if (motion_log && fighter) {
    static uint32_t last_motion[16] = {};
    const uint32_t person = std::min<uint32_t>(Rd32(base + fighter + 1156), 15), m = Rd32(base + fighter + 212);
    if (m != last_motion[person]) {
      last_motion[person] = m;
      REXLOG_INFO("motion: person {} -> {} (frame {}, state {}) at ({:.1f}, {:.1f}, {:.1f}) area {}", person, m,
                  Rd32(base + 0x82E3CD0C), base[fighter + 446], RdF(base + fighter + 288), RdF(base + fighter + 292),
                  RdF(base + fighter + 296), base[fighter + 444]);
    }
  }
  // Test aid: SVR2011_TEST_POS_LOG=<frames> - for that many frames after person 0's or 1's motion changes, every 4th:
  // the motion's base (+288, where the paired start put it), the body's place (body +0) and the facing (+276) of
  // both - where a ported dive or corner move starts against the game's own
  static const int pos_frames = [] { const char* v = std::getenv("SVR2011_TEST_POS_LOG"); return v ? std::atoi(v) : 0; }();
  if (pos_frames > 0 && fighter) {
    static uint32_t last_m[2] = {}, since[2] = {};
    const uint32_t person = Rd32(base + fighter + 1156);
    if (person < 2) {
      const uint32_t m = Rd32(base + fighter + 212);
      if (m != last_m[person]) last_m[person] = m, since[person] = 0;
      if (since[person] <= uint32_t(pos_frames) && since[person] % 4 == 0) {
        const uint32_t body = Rd32(base + fighter + 304);
        REXLOG_INFO("pos: person {} motion {} +{} base ({:.1f}, {:.1f}, {:.1f}) body ({:.1f}, {:.1f}, {:.1f}) yaw {:.2f}", person, m,
                    since[person], RdF(base + fighter + 288), RdF(base + fighter + 292), RdF(base + fighter + 296),
                    body ? RdF(base + body) : 0.0f, body ? RdF(base + body + 4) : 0.0f, body ? RdF(base + body + 8) : 0.0f,
                    RdF(base + fighter + 276));
      }
      ++since[person];
    }
  }
  // Test aid: SVR2011_TEST_LOSE=<person>,<s> - at <s> seconds of the match clock the person loses (to person 0).
  static const std::pair<int, int> lose = [] {
    const char* v = std::getenv("SVR2011_TEST_LOSE");
    int p = -1, s = 0;
    if (v && std::sscanf(v, "%d,%d", &p, &s) != 2) p = -1;
    return std::make_pair(p, s);
  }();
  if (lose.first >= 0 && fighter && int(Rd32(base + fighter + 1156)) == lose.first &&
      Rd32(base + 0x82E3CD0C) == uint32_t(lose.second * 60) && !base[fighter + 447]) {
    base[fighter + 447] = 1, base[fighter + 448] = 0;
    REXLOG_INFO("match types: test - person {} loses", lose.first);
  }
  for (Attack& a : g_attacks)
    if (a.fighter == fighter && a.wrestler) {
      // (test aid: SVR2011_TEST_LJ_MOTION=<id>[,<id>...] - each attack the next)
      static const std::vector<uint32_t> motions = [] {
        std::vector<uint32_t> m;
        if (const char* v = std::getenv("SVR2011_TEST_LJ_MOTION"))
          for (const char* p = v; *p;) {
            char* end = nullptr;
            const unsigned long id = std::strtoul(p, &end, 10);
            if (end == p) break;
            m.push_back(uint32_t(id));
            for (p = end; *p == ',' || *p == ' ';) ++p;
          }
        if (m.empty()) m.assign(std::begin(kAttackMotions), std::end(kAttackMotions));
        return m;
      }();
      static size_t next = 0;
      const uint32_t motion = motions[next++ % motions.size()];
      const auto saved = ctx;
      const uint32_t before = Rd32(base + fighter + 212), wbefore = Rd32(base + a.wrestler + 212);
      ctx.r3.u64 = fighter;
      ctx.r4.u64 = a.wrestler;
      ctx.r5.u64 = motion;
      sub_82191AA8(ctx, base);
      REXLOG_INFO("match types: lumberjack attack - motion {} (lumberjack {} -> {}, wrestler {} -> {})", motion, before,
                  Rd32(base + fighter + 212), wbefore, Rd32(base + a.wrestler + 212));
      ctx = saved;
      a.wrestler = 0;
    }
  // Test aid: SVR2011_TEST_COND_LIST_PROBE=<person>,<id> - once, 5 s into a match: the event list condition <id>
  // (sub_82852950) asked about that person's character, as a Road to WrestleMania script asks it (the object's +0 =
  // the character: here the match's character table cell). A standing wrestler handles no one (+20 empty), as MVP
  // just in at John Cena's week 2 brawl: id 38 read through it and crashed (reports 960221 / f08745); with the
  // guard it answers false. SVR2011_TEST_COND_LIST_GUARD_OFF=1 runs it unguarded (the crash, for a before / after).
  if (static const char* probe = std::getenv("SVR2011_TEST_COND_LIST_PROBE"); probe) {
    static bool done = false;
    uint32_t who = 0, cid = 0;
    if (!done && std::sscanf(probe, "%u,%u", &who, &cid) == 2 && who < 6 && Rd32(base + 0x82E3CD0C) > 300) {
      const uint32_t cell = 0x82E3CC50 + who * 4, ch = Rd32(base + cell);
      if (ch && ch == fighter) {
        done = true;
        const auto saved = ctx;
        ctx.r3.u64 = cell;
        ctx.r4.u64 = cid;
        REXLOG_INFO("[svr2011] test: event list condition {} for person {} (character {:08X}, its +20 {:08X})", cid, who, ch,
                    Rd32(base + ch + 20));
        sub_82852950(ctx, base);
        REXLOG_INFO("[svr2011] test: event list condition {} for person {} answered {}", cid, who, ctx.r3.u32);
        ctx = saved;
      }
    }
  }
  // Test aid: SVR2011_TEST_COND_PROBE=<person>,<id> - once, while that person's character's +4 is empty (a match's
  // start): the event condition <id> (sub_82856B00) asked about him, as a CPU's check does - ids 30 / 31 / 97 hand
  // the empty +4 on and crashed at clock 13 (3 Nov); with the guard they answer false. SVR2011_TEST_COND_GUARD_OFF=1
  // runs it unguarded (the crash, for a before / after).
  if (static const char* probe = std::getenv("SVR2011_TEST_COND_PROBE"); probe) {
    static bool done = false;
    uint32_t who = 0, cid = 0;
    if (!done && std::sscanf(probe, "%u,%u", &who, &cid) == 2 && who < 6) {
      const uint32_t cell = 0x82E3CC50 + who * 4, ch = Rd32(base + cell);
      if (ch && ch == fighter && !Rd32(base + ch + 4)) {
        done = true;
        const auto saved = ctx;
        ctx.r3.u64 = cell;
        ctx.r4.u64 = cid;
        REXLOG_INFO("[svr2011] test: event condition {} for person {} (character {:08X}, its +4 empty, frame {})", cid, who,
                    ch, Rd32(base + 0x82E3CD0C));
        sub_82856B00(ctx, base);
        REXLOG_INFO("[svr2011] test: event condition {} for person {} answered {}", cid, who, ctx.r3.u32);
        ctx = saved;
      }
    }
  }
  // Test aid: SVR2011_TEST_FORCE_MOVE=<attacker person>,<target person>,<every s>,<motion>[,<motion>...]
  // - in a match, every <s> seconds the attacker starts the next motion (a
  // paired move, sub_82191AA8) on the target, wherever they are: for seeing
  // ported moves without waiting for the AI to pick them.
  static const std::vector<uint32_t> force = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_FORCE_MOVE"))
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(p, &end, 10);
        if (end == p) break;
        v.push_back(uint32_t(n));
        for (p = end; *p == ',' || *p == ' ';) ++p;
      }
    return v.size() >= 4 ? v : std::vector<uint32_t>{};
  }();
  if (!force.empty() && force[0] < 6 && force[1] < 6) {
    constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
    const uint32_t att = Rd32(base + kChars + force[0] * 4), tgt = Rd32(base + kChars + force[1] * 4);
    const uint32_t frames = Rd32(base + kMatchFrames), every = std::max<uint32_t>(force[2], 1) * 60;
    static uint32_t last = 0, next = 0;
    if (att == fighter && tgt && frames > 300 && frames >= last + every) {
      last = frames;
      const uint32_t motion = force[3 + next++ % (force.size() - 3)];
      // SVR2011_TEST_FORCE_PLACE=<x>,<z>,<yaw>[,<y>[,<target x>,<target z>[,<target y>[,<target area>]]]] - the
      // attacker is first put there in the ring (y -12 the mat; yaw in radians, +z at 0): corner, rope and dive
      // moves start where they belong; with a target place the target too (an apron move: the target on the apron,
      // area 1 - the paired start then sets both from the move).
      static const std::vector<float> place = [] {
        std::vector<float> v;
        if (const char* e = std::getenv("SVR2011_TEST_FORCE_PLACE"))
          for (const char* p = e; *p;) {
            char* end = nullptr;
            const float f = std::strtof(p, &end);
            if (end == p) break;
            v.push_back(f);
            for (p = end; *p == ',' || *p == ' ';) ++p;
          }
        return v;
      }();
      if (place.size() >= 3) {
        const float x = place[0], z = place[1], yaw = place[2], y = place.size() > 3 ? place[3] : -12.0f;
        PutFighter(base, att, x, y, z, std::fabs(x) > 28.8f || std::fabs(z) > 28.8f ? 1 : 0);  // (past the ropes: outside)
        WrF(base + att + 276, yaw);
        if (place.size() >= 6)
          PutFighter(base, tgt, place[4], place.size() > 6 ? place[6] : -12.0f, place[5], place.size() > 7 ? uint8_t(place[7]) : 0);
      }
      const auto saved = ctx;
      // SVR2011_TEST_FORCE_PARTNER=<person>[,<place>[,<setup>]] - a tag team move: that person (the attacker's partner)
      // plays the move's third track (y 2), as the game's own double teams do after the paired start (sub_822E35E8:
      // the corner and standing ones both end sub_82396E80(partner, move, 0, 2)). <place>: 0 the attacker's base and
      // facing (default), 1 the target's, 2 where he is. <setup>: the game's own calls before its paired start - 1 the
      // corner double team's sub_821F0F90(attacker, partner), 2 the standing one's sub_82191420(target) +
      // sub_821EFA98(target, attacker), 0 none (the pair froze in the move, with or without the partner, 9 Nov).
      static const std::array<int, 3> partner = [] {
        std::array<int, 3> v{-1, 0, 0};
        if (const char* e = std::getenv("SVR2011_TEST_FORCE_PARTNER")) std::sscanf(e, "%d,%d,%d", &v[0], &v[1], &v[2]);
        return v;
      }();
      const uint32_t par = partner[0] >= 0 && partner[0] < 6 ? Rd32(base + kChars + uint32_t(partner[0]) * 4) : 0;
      const bool with_partner = par && par != att && par != tgt;
      if (with_partner && partner[2] == 1) {
        ctx.r3.u64 = att, ctx.r4.u64 = par;
        sub_821F0F90(ctx, base);
      } else if (with_partner && partner[2] == 2) {
        ctx.r3.u64 = tgt;
        sub_82191420(ctx, base);
        ctx.r3.u64 = tgt, ctx.r4.u64 = att;
        sub_821EFA98(ctx, base);
      }
      ctx.r3.u64 = att;
      ctx.r4.u64 = tgt;
      ctx.r5.u64 = motion;
      sub_82191AA8(ctx, base);
      // (and again after the start: the paired start keeps or redoes the places by the move - a stock corner move
      // kept them, a ported one got its pair put back near the target - and the facing came out as the start left
      // it (0.17 for a written -0.79: the target then landed outside the ring); written after it, the motion plays
      // from them)
      if (place.size() >= 3) {
        const float x = place[0], z = place[1], y = place.size() > 3 ? place[3] : -12.0f;
        WrF(base + att + 288, x), WrF(base + att + 292, y), WrF(base + att + 296, z), WrF(base + att + 276, place[2]);
        if (place.size() >= 6)
          WrF(base + tgt + 288, place[4]), WrF(base + tgt + 292, place.size() > 6 ? place[6] : -12.0f), WrF(base + tgt + 296, place[5]);
      }
      REXLOG_INFO("match types: test - person {} starts motion {} on person {} ({} -> state {})", force[0], motion,
                  force[1], frames, Rd32(base + att + 212));
      if (with_partner) {
        const uint32_t from = partner[1] == 1 ? tgt : partner[1] == 0 ? att : 0;
        if (from)
          WrF(base + par + 288, RdF(base + from + 288)), WrF(base + par + 292, RdF(base + from + 292)),
              WrF(base + par + 296, RdF(base + from + 296)), WrF(base + par + 276, RdF(base + from + 276));
        ctx.r3.u64 = par, ctx.r4.u64 = motion, ctx.r5.u64 = 0, ctx.r6.u64 = 2;
        sub_82396E80(ctx, base);
        REXLOG_INFO("match types: test - person {} plays motion {} as the partner (place {}, set-up {}, state {})",
                    partner[0], motion, partner[1], partner[2], Rd32(base + par + 212));
      }
      ctx = saved;
    }
  }
  // Test aid: SVR2011_TEST_FORCE_DIVE=<every s>,<set-up motion>,<dive motion>[,<dive motion>...] - a dive from the
  // top rope onto a lying person 1: every <s> seconds person 0 takes person 1 down with the set-up move (a paired
  // move that leaves him lying, e.g. 5000 - person 0 put in the middle of the ring first, so it stays in the ring);
  // 10 s later (the set-up over: placed during a move, both went back to its track) he is put on the top turnbuckle
  // - (28.8, -21.9, -28.8), where a CPU's climb ends - with person 1 lying 20 units in front of him, and the dive
  // starts through the paired start sub_82191AA8 (it keeps written places - the one-person setter sub_82396E80 didn't,
  // with or without sub_8217C2B8), after which person 1 gets his own lying motion back (a dive has no track of his).
  // A start with no target crashed: sub_8218F550 links the two people (+96 of each = the other) and wrote to 0x60.
  // Stock dives start on the turnbuckle; ported ones (their tracks start at the root) rise where he stood, with
  // person 1 put beside him - enough to see where the dive lands.
  static const std::vector<uint32_t> dive = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_FORCE_DIVE"))
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(p, &end, 10);
        if (end == p) break;
        v.push_back(uint32_t(n));
        for (p = end; *p == ',' || *p == ' ';) ++p;
      }
    return v.size() >= 3 ? v : std::vector<uint32_t>{};
  }();
  if (!dive.empty()) {
    constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
    const uint32_t att = Rd32(base + kChars), tgt = Rd32(base + kChars + 4);
    const uint32_t frames = Rd32(base + kMatchFrames), every = std::max<uint32_t>(dive[0], 10) * 60;
    static uint32_t phase_at = 0, next = 0;
    static int phase = 0;  // (0: the set-up next, 1: the dive next)
    if (att == fighter && tgt && frames > 300) {
      if (frames < phase_at) phase_at = 0, phase = 0;  // (a new match)
      const auto saved = ctx;
      auto put = [&](uint32_t f, float x, float y, float z) { PutFighter(base, f, x, y, z); };
      if (phase == 0 && frames >= phase_at + every) {
        phase_at = frames, phase = 1;
        put(att, 0.0f, -12.0f, 0.0f);
        ctx.r3.u64 = att, ctx.r4.u64 = tgt, ctx.r5.u64 = dive[1];
        sub_82191AA8(ctx, base);
        REXLOG_INFO("match types: test dive - set-up {} on person 1 ({})", dive[1], frames);
      } else if (phase == 1 && frames >= phase_at + 600) {
        phase = 0;
        const uint32_t motion = dive[2 + next++ % (dive.size() - 2)];
        const float ax = 28.8f, ay = -21.9f, az = -28.8f;
        const float vx = ax - 14.1f, vz = az + 14.1f;  // (20 units toward the ring's middle)
        // (facing him: through the one-person setter a turned yaw faced in; through the paired start - below - the
        // turned yaw dove out of the ring)
        const float yaw = std::atan2(vx - ax, vz - az);
        // Places written before the one-person setter alone (sub_82396E80(fighter, motion, x, y)) didn't hold, with or
        // without sub_8217C2B8; through the paired start they do (FORCE_PLACE). So the dive goes through the paired
        // start - its links (+96) both valid here - and person 1 then gets his own lying motion back at once (a dive
        // has no track of his: in it he'd wait for good).
        const uint32_t lying = Rd32(base + tgt + 212), lx = Rd32(base + tgt + 216), ly = Rd32(base + tgt + 220);
        put(att, ax, ay, az);
        WrF(base + att + 276, yaw);
        put(tgt, vx, -12.0f, vz);
        ctx.r3.u64 = att, ctx.r4.u64 = tgt, ctx.r5.u64 = motion;
        sub_82191AA8(ctx, base);
        // (nothing written after the start: the place or the facing written again made the stock Frog Splash dive out
        // of the ring, 28 Oct - the start's own are right for a dive)
        ctx.r3.u64 = tgt, ctx.r4.u64 = lying, ctx.r5.u64 = lx, ctx.r6.u64 = ly;
        sub_82396E80(ctx, base);
        REXLOG_INFO("match types: test dive - person 0 dives with {} from the top rope onto person 1 ({} -> state {})",
                    motion, frames, Rd32(base + att + 212));
      }
      ctx = saved;
    }
  }
}

// -- The lumberjacks' controller ---------------------------------------------
//
// The lumberjacks are managers (kind 2): they keep to their posts on the
// floor and their AI never fights (neither their role +2624 nor the AI's mark
// +168 changes that during a match). So the port makes them act: each world
// update, a wrestler on the floor (y about 0, outside the ring's +-30 - in
// the ring y is -12) for half a second gets the nearest lumberjack within
// reach, who grapples him (kAttackMotions in turn, started from the fighter
// control hook above) - and again while he stays out: one attack on a
// wrestler every kWrestlerGap, each lumberjack resting kLumberjackGap between
// his. (Each lumberjack once per trip out, two at most, left a wrestler who
// stayed on the floor alone after two - "they just watch".) Their hits would
// disqualify them (lost 7, the match over): LumberjackBeforeJudge takes that back.
namespace {

constexpr float kRingHalf = 30.0f, kReach = 26.0f;
constexpr uint32_t kWrestlerGap = 180, kLumberjackGap = 360;  // (updates: 3 s, 6 s)

struct Pos {
  float x, y, z;
};
Pos PosOf(const uint8_t* base, uint32_t ch) { return {RdF(base + ch + 288), RdF(base + ch + 292), RdF(base + ch + 296)}; }
bool InRing(const Pos& p) { return p.y < -6.0f && std::fabs(p.x) < kRingHalf && std::fabs(p.z) < kRingHalf; }
bool OnFloor(const Pos& p) { return p.y > -6.0f && (std::fabs(p.x) > kRingHalf || std::fabs(p.z) > kRingHalf); }

void LumberjackController(uint8_t* base) {
  constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
  static uint32_t last_frames = 0;
  static int floor_time[6] = {};        // (updates a wrestler has been on the floor (+) / in the ring (-))
  static uint32_t next_on[6] = {};      // (a wrestler: the next attack on him not before)
  static uint32_t busy_until[6] = {};   // (a lumberjack: resting until)
  const uint32_t frames = Rd32(base + kMatchFrames);
  const bool running = frames != last_frames && frames > 60;
  if (running && last_frames <= 60) {  // (once a match: its live rules - byte 5 the count out, 0x80 none)
    std::string live;
    for (uint32_t i = 0; i < 16; ++i) live += fmt::format(" {:02X}", base[0x82E3DE00 + i]);
    REXLOG_INFO("match types: Lumberjack live rules{}", live);
  }
  last_frames = frames;
  if (!running) {
    for (int& f : floor_time) f = 0;
    for (uint32_t& n : next_on) n = 0;
    for (uint32_t& b : busy_until) b = 0;
    for (Attack& a : g_attacks) a = Attack{};
    return;
  }
  uint32_t ch[6];
  Pos pos[6];
  for (uint32_t i = 0; i < 6; ++i) {
    ch[i] = Rd32(base + kChars + i * 4);
    if (ch[i]) pos[i] = PosOf(base, ch[i]);
  }
  // The lumberjacks: people 2-5 (+1156), whatever their character slot.
  auto is_lumberjack = [&](uint32_t i) { return ch[i] && Rd32(base + ch[i] + 1156) >= 2; };
  // Test aid: SVR2011_TEST_LJ_FORCE=<s> - every <s> seconds the next
  // lumberjack attacks person 0 wherever they are (for trying attack moves).
  static const int force = [] { const char* v = std::getenv("SVR2011_TEST_LJ_FORCE"); return v ? std::atoi(v) : 0; }();
  if (force > 0 && frames % uint32_t(force * 60) == 0 && ch[0]) {
    static uint32_t who = 2;
    for (uint32_t k = 0; k < 6; ++k, who = 2 + (who - 1) % 4)
      if (is_lumberjack(who)) {
        g_attacks[who] = Attack{ch[who], ch[0], frames};
        REXLOG_INFO("match types: test - lumberjack {} attacks person 0", who);
        who = 2 + (who - 1) % 4;
        break;
      }
  }
  for (uint32_t w = 0; w < 6; ++w) {
    if (!ch[w] || is_lumberjack(w)) continue;
    if (OnFloor(pos[w])) floor_time[w] = std::max(floor_time[w], 0) + 1;
    else if (InRing(pos[w])) floor_time[w] = std::min(floor_time[w], 0) - 1;
    if (floor_time[w] < 30 || frames < next_on[w]) continue;
    // (the nearest rested lumberjack within reach)
    int best = -1;
    float best_d = kReach;
    for (uint32_t i = 0; i < 6; ++i)
      if (is_lumberjack(i) && frames >= busy_until[i] && !InRing(pos[i])) {
        const float d = std::hypot(pos[i].x - pos[w].x, pos[i].z - pos[w].z);
        if (d < best_d) best = int(i), best_d = d;
      }
    if (best < 0) continue;
    busy_until[best] = frames + kLumberjackGap;
    next_on[w] = frames + kWrestlerGap;
    g_attacks[best] = Attack{ch[best], ch[w], frames};
    REXLOG_INFO("match types: lumberjack {} goes for person {}", Rd32(base + ch[best] + 1156), Rd32(base + ch[w] + 1156));
  }
}

}  // namespace

namespace svr2011 {

bool SlobberKnockerMatch() { return g_slobber; }
bool GauntletEliminatorMatch() { return g_ge; }

// A lumberjack (a manager, team 3) who hits a wrestler gets the lost byte
// (+447) 7 - disqualified for interference - and the judge then ends the
// match ("WINS BY WAY OF DQ"). Their attacks are the point here: taken back.
void LumberjackBeforeJudge(uint8_t* base) {
  if (base[0x82E3DE00] != kLumberjack || Rd32(base + kStoryContext) || !MatchTypeOn("lumberjack")) return;
  constexpr uint32_t kChars = 0x82E3CC50;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || Rd32(base + c + 1156) < 2 || !base[c + 447]) continue;
    static int count = 0;
    if (++count <= 20)
      REXLOG_INFO("match types: lumberjack {} disqualified (lost {}) - taken back", Rd32(base + c + 1156), base[c + 447]);
    base[c + 447] = 0;
    base[c + 448] = 0;
  }
}

// ONLINE: FIRST BLOOD in Player Match. The online MATCH TYPE screen's
// categories (the image's data at 0x82028A60: per online type a record {id,
// list, count}, its list {rule, 0, 0} entries, 12 bytes) leave First Blood
// out; the one on one EXTREME RULES type (3) and the triple threat one (0xB)
// get it (0x29 / 0x2A, before EXTREME RULES, the last), in port memory. The
// session carries the rule (property 0x1000000C): the joiner plays it as any.
namespace {
void OnlineFirstBlood(uint8_t* base) {
  static bool done = false;
  if (done || !g_memory || !svr2011::MatchTypeSwitchOn("online_first_blood")) return;  // (the host's offer: its own switch)
  done = true;
  struct Add {
    uint32_t record, rule;
  };
  for (const Add& a : {Add{0x82028A70, 0x29}, Add{0x82028AD0, 0x2A}}) {
    const uint32_t list = Rd32(base + a.record + 4), n = Rd32(base + a.record + 8);
    if (!list || n == 0 || n > 16) continue;
    bool has = false;
    for (uint32_t i = 0; i < n; ++i) has = has || Rd32(base + list + i * 12) == a.rule;
    if (has) continue;
    const uint32_t copy = g_memory->SystemHeapAlloc((n + 1) * 12);
    if (!copy) continue;
    std::memcpy(base + copy, base + list, (n - 1) * 12);   // (all but the last: EXTREME RULES)
    Wr32(base + copy + (n - 1) * 12, a.rule);
    Wr32(base + copy + (n - 1) * 12 + 4, 0);
    Wr32(base + copy + (n - 1) * 12 + 8, 0);
    std::memcpy(base + copy + n * 12, base + list + (n - 1) * 12, 12);
    // (the image is read-only: writable for the record's two words)
    auto* heap = g_memory->LookupHeap(a.record);
    uint32_t old = 0;
    if (!heap || !heap->QueryProtect(a.record, &old) ||
        !heap->Protect(a.record + 4, 8, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
      REXLOG_WARN("match types: online - the type record {:08X} can't be made writable", a.record);
      continue;
    }
    Wr32(base + a.record + 4, copy);
    Wr32(base + a.record + 8, n + 1);
    heap->Protect(a.record + 4, 8, old);
    REXLOG_INFO("match types: online - FIRST BLOOD (rule {:02X}) in the online type {:X}", a.rule, Rd32(base + a.record));
  }
}
}  // namespace

void MatchTypesUpdate(PPCContext& ctx, uint8_t* base) {
  OnlineFirstBlood(base);
  constexpr uint32_t kChars = 0x82E3CC50;
  MatchHudTick(base);  // (match_hud.h: the HUD shows while the match's clock runs)
  svr2011::CrowdHandoffUpdate(ctx, base);  // (a fan hands a weapon over the barricade: crowd_fight.h)
  svr2011::MatchBoxUpdate(ctx, base);      // (the match's black box: match_box.h)
  svr2011::MatchRecordUpdate(ctx, base);   // (match recordings: match_record.h)
  // (the CPU stall net: the referee - person 6 - counting someone out, motion 191)
  if (const uint32_t ref = Rd32(base + kChars + 6 * 4); ref && Rd32(base + ref + 212) == kRefCounting)
    g_count_seen = Rd32(base + 0x82E3CD0C) + 1;
  // ... and two CPUs left in a lock-up (21100) for 10 s - Cena / Orton on arena 17 stayed in one 4 min: the game's
  // own way on from it, the lock-up's next part 21201 (sub_82173280 sets up 21100 with 21201 / 21202), started as a
  // pair by the paired start sub_82191AA8(a, b, 21201): 1050, then 21201 together, then both free and fighting.
  // (Tried: both back to standing one by one - a crash, the pair link half made; 21201 / 21202 one on each - a crash;
  // a pair grapple 5000 - frozen; 10005 - a 10 s loop.) Match state only: alike on every machine.
  {
    static uint32_t motion_of[6] = {}, since[6] = {};
    const uint32_t frames = Rd32(base + 0x82E3CD0C), match = Rd32(base + 0x82EDEB48);
    static const int test_secs = [] { const char* v = std::getenv("SVR2011_TEST_STALL_SECS"); return v ? std::atoi(v) : 0; }();
    const uint32_t hold_frames = uint32_t((test_secs > 0 ? test_secs : 10) * 60);
    uint32_t ch[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
      ch[i] = Rd32(base + kChars + i * 4);
      const uint32_t m = ch[i] ? Rd32(base + ch[i] + 212) : 0;
      if (m != motion_of[i] || !since[i] || frames + 1 < since[i]) motion_of[i] = m, since[i] = frames + 1;
    }
    const auto cpu = [&](uint32_t i) { return match && base[match + 432 + i * 2116 - 8] == 1 && Rd32(base + ch[i] + 1152) >= 4; };
    for (uint32_t i = 0; i < 6; ++i)
      for (uint32_t j = i + 1; j < 6; ++j)
        if (ch[i] && ch[j] && motion_of[i] == kLockUp && motion_of[j] == kLockUp && frames + 1 - since[i] >= hold_frames &&
            frames + 1 - since[j] >= hold_frames && cpu(i) && cpu(j)) {
          PPCContext k = ctx;
          k.r3.u64 = ch[i], k.r4.u64 = ch[j], k.r5.u64 = kLockUpOn;
          sub_82191AA8(k, base);
          static int logged = 0;
          if (logged++ < 40)
            REXLOG_INFO("cpu stall: persons {} and {} in a lock-up {} s -> on with it: {} {} / {} {}", i, j,
                        (frames + 1 - since[i]) / 60, i, Rd32(base + ch[i] + 212), j, Rd32(base + ch[j] + 212));
          since[i] = since[j] = frames + 1;
        }
  }
  // Research aid (stuck wrestlers, B11/B12): SVR2011_TEST_STUCK_CHARS=1 - each
  // second, people 0-5 whose place and motion (+212) haven't changed for 10 s
  // while the match clock runs are logged (once each time), with the motion.
  if (static const bool stuck_log = std::getenv("SVR2011_TEST_STUCK_CHARS") != nullptr; stuck_log) {
    struct Seen { uint32_t x = 0, z = 0, motion = 0, since = 0; bool told = false; };
    static Seen seen[6];
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    static uint32_t last = 0;
    if (frames < last) for (Seen& s : seen) s = Seen{};
    // (SVR2011_TEST_PUT_P1=<x>,<z>[,<frame>]: at that match frame (240) person 0 is put there on the floor, once a match)
    static bool put = false;
    if (frames < last) put = false;
    static const char* put_at = std::getenv("SVR2011_TEST_PUT_P1");
    static const uint32_t put_frame = [] {
      float x, z;
      int f = 240;
      if (put_at) std::sscanf(put_at, "%f,%f,%d", &x, &z, &f);
      return uint32_t(f);
    }();
    if (const char* at = put_at; at && !put && frames >= put_frame)
      if (const uint32_t c = Rd32(base + kChars)) {
        put = true;
        float x = 0, z = 0;
        std::sscanf(at, "%f,%f", &x, &z);
        WrF(base + c + 288, x), WrF(base + c + 292, 0.0f), WrF(base + c + 296, z);
        REXLOG_INFO("match types: person 0 put at ({}, {})", x, z);
      }
    if (frames / 60 != last / 60)
      for (uint32_t i = 0; i < 6; ++i) {
        const uint32_t c = Rd32(base + kChars + i * 4);
        if (!c) continue;
        Seen& s = seen[i];
        const uint32_t x = Rd32(base + c + 288), z = Rd32(base + c + 296), m = Rd32(base + c + 212);
        if (x != s.x || z != s.z || m != s.motion) s = Seen{x, z, m, frames};
        else if (!s.told && frames - s.since >= 600) {
          s.told = true;
          const uint32_t target = svr2011::AiTargetOf(c);
          REXLOG_INFO("match types: person {} stuck since match frame {} at ({:.1f}, {:.1f}), motion {}, state {} lost {}; "
                      "AI target {:08X}{}", i, s.since, RdF(base + c + 288), RdF(base + c + 296), m, base[c + 446], base[c + 447],
                      target, target ? fmt::format(" (state {} lost {} at {:.1f}, {:.1f}, motion {})", base[target + 446],
                                                   base[target + 447], RdF(base + target + 288), RdF(base + target + 296),
                                                   Rd32(base + target + 212)) : std::string());
        }
      }
    last = frames;
  }
  // START ON THE ROOF: from the bell for 1.5 s everyone in it (people 0-3,
  // not the referee or the commentators) is held on the roof - a corner
  // spot each, off the breakable middle panels (|x| < 13): the real
  // position vector, its copy and the area.
  if (g_roof && base[0x82E3DE00] >= kCellFirst && base[0x82E3DE00] <= kCellLast) {
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    static uint32_t placed_frames = ~0u;
    if (frames < 90) {
      constexpr float kSpot[4][2] = {{-30.0f, 30.0f}, {30.0f, -30.0f}, {30.0f, 30.0f}, {-30.0f, -30.0f}};
      constexpr float kRoofY = -60.5f;
      // (test aid: SVR2011_TEST_ROOF_SKIP=<person> - that one starts in the ring, e.g. the idle player 0)
      static const int skip = [] { const char* v = std::getenv("SVR2011_TEST_ROOF_SKIP"); return v ? std::atoi(v) : -1; }();
      for (uint32_t i = 0, k = 0; i < 6 && k < 4; ++i) {
        const uint32_t c = Rd32(base + kChars + i * 4);
        if (!c || Rd32(base + c + 1800) >= 30 || int(i) == skip) continue;
        const float x = kSpot[k][0], z = kSpot[k][1];
        ++k;
        if (const uint32_t v = Rd32(base + c + 304)) WrF(base + v, x), WrF(base + v + 4, kRoofY), WrF(base + v + 8, z);
        WrF(base + c + 288, x), WrF(base + c + 292, kRoofY), WrF(base + c + 296, z);
        base[c + 444] = 10;
      }
      if (placed_frames == ~0u) REXLOG_INFO("match types: START ON THE ROOF - everyone on the roof (frame {})", frames);
      placed_frames = frames;
    } else {
      placed_frames = ~0u;
    }
  }
  // CPUs alone on the roof: since when (the climb-down: sub_82292D50's hook)
  {
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    uint32_t on_roof[6] = {}, n = 0;
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      on_roof[i] = c && base[c + 444] == kRoofArea;
      n += on_roof[i];
    }
    for (uint32_t i = 0; i < 6; ++i) {
      if (on_roof[i] && n == 1) {
        if (!g_roof_alone_since[i] || frames + 1 < g_roof_alone_since[i]) g_roof_alone_since[i] = frames + 1;
      } else {
        g_roof_alone_since[i] = 0;
      }
    }
  }
  // Research aid (Hell in a Cell roof): SVR2011_TEST_CELL=1 - each second of a
  // match the first two people's position, area bytes (+441, +444) and motion;
  // SVR2011_TEST_CELL_ROOF=<y> - from match frame 180 to 240 both are put at
  // that height (y is down: the roof -60.5), x -30 / +30, z 30 - the real
  // position vector (*(+304)) and its copy (+288), area +444 10 (the roof).
  if (static const bool cell_log = std::getenv("SVR2011_TEST_CELL") != nullptr; cell_log) {
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    static uint32_t last = 0;
    static bool moved = false;
    if (frames < last) moved = false;
    if (static const char* roof = std::getenv("SVR2011_TEST_CELL_ROOF"); roof && frames >= 180 && frames < 240) {
      const float y = float(std::atof(roof));
      for (uint32_t i = 0; i < 2; ++i)
        if (const uint32_t c = Rd32(base + kChars + i * 4)) {
          const float x = i ? 30.0f : -30.0f, z = 30.0f;
          if (const uint32_t v = Rd32(base + c + 304)) WrF(base + v, x), WrF(base + v + 4, y), WrF(base + v + 8, z);
          WrF(base + c + 288, x), WrF(base + c + 292, y), WrF(base + c + 296, z);
          base[c + 444] = 10;
          if (!moved) REXLOG_INFO("match types: cell test - person {} put on the roof ({}, {}, {})", i, x, y, z);
        }
      moved = true;
    }
    if (frames / 60 != last / 60 || frames < last) {
      std::string s;
      for (uint32_t i = 0; i < 2; ++i)
        if (const uint32_t c = Rd32(base + kChars + i * 4))
          s += fmt::format(" {}:({:.1f},{:.1f},{:.1f}) a{} b{} m{} f{:X}", i, RdF(base + c + 288), RdF(base + c + 292),
                           RdF(base + c + 296), base[c + 441], base[c + 444], Rd32(base + c + 212) & 0x7FFF,
                           Rd32(base + c + 516));
      REXLOG_INFO("match types: cell test frame {} rule {:02X}{}", frames, base[0x82E3DE00], s);
    }
    last = frames;
  }
  // Research aid: SVR2011_TEST_ITEM_DUMP=<file> - every 30 updates of a match,
  // the object table (0x82DE0220, 72 slots: people and weapons) appended:
  // u32 match frames, then per slot u32 object and its first 0xC00 bytes.
  if (static const char* dump = std::getenv("SVR2011_TEST_ITEM_DUMP"); dump && Rd32(base + 0x82E3CD0C) > 0) {
    static uint32_t n = 0;
    if (++n % 30 == 0)
      if (FILE* f = std::fopen(dump, "ab")) {
        const uint32_t frames = Rd32(base + 0x82E3CD0C);
        std::fwrite(&frames, 4, 1, f);
        for (uint32_t i = 0; i < 72; ++i) {
          const uint32_t o = Rd32(base + 0x82DE0220 + i * 4);
          std::fwrite(&o, 4, 1, f);
          if (o) std::fwrite(base + o, 1, 0xC00, f);
        }
        std::fclose(f);
      }
  }
  // Research aid: SVR2011_TEST_CHAR_DUMP=<file> - every 6 updates of a match,
  // the first two people (0x82E3CC50): u32 match frames, then per person u32
  // its address and its first 0x3000 bytes.
  if (static const char* dump = std::getenv("SVR2011_TEST_CHAR_DUMP"); dump && Rd32(base + 0x82E3CD0C) > 0) {
    static uint32_t n = 0;
    if (++n % 6 == 0)
      if (FILE* f = std::fopen(dump, "ab")) {
        const uint32_t frames = Rd32(base + 0x82E3CD0C);
        std::fwrite(&frames, 4, 1, f);
        for (uint32_t i = 0; i < 2; ++i) {
          const uint32_t c = Rd32(base + kChars + i * 4);
          std::fwrite(&c, 4, 1, f);
          if (c) std::fwrite(base + c, 1, 0x3000, f);
        }
        std::fclose(f);
      }
  }
  // Test aid: SVR2011_TEST_ITEM_LOG=1 - each weapon picked up (its state,
  // +44, from none to taken): its slot, model, where it lay, and whether it
  // came from under the ring (hidden at 0,-12,0 until then).
  if (static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr; item_log) {
    // (kind per slot: 0 unknown, 1 lying at the bell (placed), 2 hidden (under the ring), 3 out from under it)
    static uint32_t holder[72], kind[72], last_frames = 0;
    static float last_pos[72][3];
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    if (frames < last_frames)
      for (uint32_t i = 0; i < 72; ++i) holder[i] = 0xFF, kind[i] = 0;
    last_frames = frames;
    for (uint32_t i = 0; i < 72; ++i) {
      const uint32_t o = Rd32(base + 0x82DE0220 + i * 4);
      if (!o) continue;
      const uint32_t h = Rd32(base + o + 44) & 0xFF;
      const uint32_t model = Rd32(base + o + 80);
      if (model >= 1000) continue;  // (not a weapon)
      const float x = RdF(base + o + 288), y = RdF(base + o + 292), z = RdF(base + o + 296);
      const bool hidden_now = x == 0.f && z == 0.f && y < -11.f;
      if (kind[i] == 0 && frames >= 120 && frames < 600 && h == 0xFF) kind[i] = hidden_now ? 2 : 1;
      if (kind[i] == 2 && !hidden_now && last_pos[i][1] < -11.f && last_pos[i][0] == 0.f) {
        std::string who;  // (out from under the ring: what the people are doing)
        for (uint32_t c = 0; c < 6; ++c)
          if (const uint32_t p = Rd32(base + kChars + c * 4))
            who += fmt::format(" [{}: motion {} at ({:.0f},{:.0f})]", Rd32(base + p + 1156), Rd32(base + p + 212),
                               RdF(base + p + 288), RdF(base + p + 296));
        REXLOG_INFO("item: slot {} (model {}) out from under the ring at ({:.0f},{:.0f},{:.0f}), frame {};{}", i, model,
                    x, y, z, frames, who);
      }
      if (h != 0xFF && holder[i] == 0xFF && kind[i] != 0) {
        static const char* kKinds[] = {"?", "placed", "from under the ring", "again"};
        uint32_t who = 0xFF;  // (the holder: the person whose held object +1828 is this one's id +1152; +44 is its state)
        for (uint32_t c = 0; c < 9 && who == 0xFF; ++c)
          if (const uint32_t p = Rd32(base + kChars + c * 4); p && Rd32(base + p + 1828) == Rd32(base + o + 1152)) who = c;
        REXLOG_INFO("item: person {} picks up slot {} (model {}) {} at ({:.0f},{:.0f},{:.0f}), frame {}", who, i, model,
                    kKinds[kind[i]], last_pos[i][0], last_pos[i][1], last_pos[i][2], frames);
        if (kind[i] == 2) kind[i] = 3;
      }
      holder[i] = h;
      if (h == 0xFF)
        for (int k = 0; k < 3; ++k) last_pos[i][k] = RdF(base + o + 288 + k * 4);
    }
  }
  // Research aid: SVR2011_TEST_RULE_MOTIONS=1 - once a match (frame 600), the first slot of each motion bank list
  // (*0x82E3CC40, *0x82E3C108 - searched in slot order) that has the ladders' and tables' motions and two of the cell's
  // own (report 8a07a5: the cell's are found first, Extreme Rules' RUWM 6 / 7 after them).
  if (static const bool motions_log = !svr2011::Env("SVR2011_TEST_RULE_MOTIONS").empty(); motions_log) {
    static uint32_t told_at = ~0u;
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    if (frames >= 600 && frames < 660 && told_at != 600) {
      told_at = 600;
      const auto at = [&](uint32_t a) { return base + a + REX_PHYS_HOST_OFFSET(a); };
      constexpr uint32_t kKeys[][2] = {{15943, 6}, {15084, 6}, {15010, 6}, {15943, 11}, {15090, 11}, {15010, 11}, {1670, 0}, {4200, 0}};
      std::string s;
      for (const uint32_t list_at : {0x82E3CC40u, 0x82E3C108u}) {
        const uint32_t list = Rd32(base + list_at);
        s += fmt::format(" | list {:08X}:", list_at);
        for (const auto& [id, x] : kKeys) {
          int slot = -1;
          for (uint32_t k = 0; list && k < 68 && slot < 0; ++k) {
            uint32_t b = Rd32(at(list + 4 * k));
            if (!b) continue;
            if (Rd32(at(b)) != 0x734B4D59 && Rd32(at(b)) != 0x73424D59) b -= 16;
            const uint32_t n = Rd32(at(b + 272));
            for (uint32_t i = 0; n < 65536 && i < n && slot < 0; ++i)
              if (Rd32(at(b + 276 + i * 16)) == (id << 16 | x << 8)) slot = int(k);
          }
          s += fmt::format(" {}x{} slot {}", id, x, slot);
        }
      }
      REXLOG_INFO("match types: rule motions (SVR2011_TEST_RULE_MOTIONS){}", s);
    }
    if (frames < 600) told_at = ~0u;
  }
  // (with SVR2011_TEST_ITEM_LOG) the stock under the ring (*0x82E3C06C: +0
  // count, +8 entries: weapon model ids, -1 taken) each time it changes, and how long each
  // search under the ring (motions 15480 / 15481) lasts.
  if (static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr; item_log) {
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    if (const uint32_t list = Rd32(base + 0x82E3C06C); list && frames > 0) {
      const int32_t n = int32_t(Rd32(base + list));
      const uint32_t entries = Rd32(base + list + 8);
      std::string s;
      for (int32_t i = 0; i < n && i < 32 && entries; ++i) {
        s += fmt::format(" {}", int32_t(Rd32(base + entries + uint32_t(i) * 4)));  // (weapon model ids; -1 taken)
      }
      static std::string last;
      if (s != last) {
        last = s;
        REXLOG_INFO("item: under the ring ({}):{} frame {}", n, s, frames);
      }
    }
    // (each minute: in how many of its seconds someone held a weapon)
    static uint32_t held_s = 0, sampled_s = 0, last_sample = 0;
    if (frames > 0 && frames / 60 != last_sample) {
      last_sample = frames / 60;
      bool held = false;
      for (uint32_t i = 0; i < 72 && !held; ++i)
        if (const uint32_t o = Rd32(base + 0x82DE0220 + i * 4); o && Rd32(base + o + 80) < 1000)
          held = (Rd32(base + o + 44) & 0xFF) != 0xFF;
      held_s += held, ++sampled_s;
      if (sampled_s == 60) {
        REXLOG_INFO("item: a weapon in hand {} of the last 60 s, frame {}", held_s, frames);
        held_s = sampled_s = 0;
      }
    }
    static uint32_t search_from[6] = {};
    for (uint32_t c = 0; c < 6; ++c) {
      const uint32_t p = Rd32(base + kChars + c * 4);
      const uint32_t m = p ? Rd32(base + p + 212) : 0;
      const bool searching = m == 15480 || m == 15481;
      if (searching && !search_from[c]) search_from[c] = frames ? frames : 1;
      if (!searching && search_from[c]) {
        REXLOG_INFO("item: person {} searched under the ring {:.1f} s", c, (frames - search_from[c]) / 60.0);
        search_from[c] = 0;
      }
    }
  }
  if (g_slobber && base[0x82E3DE00] == kGauntlet) SlobberKnockerUpdate(ctx, base);
  if (g_ge && base[0x82E3DE00] == kGauntlet) svr2011::GauntletEliminatorUpdate(ctx, base);
  if (ThreeStagesMatch()) ThreeStagesUpdate(base);
  if (base[0x82E3DE00] != kLumberjack || Rd32(base + kStoryContext) || !MatchTypeOn("lumberjack")) return;
  LumberjackController(base);
  static const bool probe = std::getenv("SVR2011_TEST_PEOPLE") != nullptr;
  static uint32_t frames = 0;
  if (probe && ++frames % 60 == 0) {
    std::string all;
    for (uint32_t i = 0; i < 6; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4)) {
        all += fmt::format(" {}:{:08X} ({:.0f},{:.0f},{:.0f}) area {} role {} +2348 {} +464 {} +476 {:08X}", i, c,
                           RdF(base + c + 288), RdF(base + c + 292), RdF(base + c + 296), base[c + 444],
                           Rd32(base + c + 2624), Rd32(base + c + 2348), base[c + 464], Rd32(base + c + 476));
      }
    REXLOG_INFO("[svr2011] people: roles{}", all);
    if (const uint32_t c2 = Rd32(base + kChars + 8)) {  // (character 2's position object +304 and body +112)
      std::string f;
      const uint32_t po = Rd32(base + c2 + 304), body = Rd32(base + c2 + 112);
      for (uint32_t at = 0; po && at < 96; at += 4) f += fmt::format(" {:.1f}", RdF(base + po + at));
      f += " | body";
      for (uint32_t at = 32; body && at < 96; at += 4) f += fmt::format(" {:.1f}", RdF(base + body + at));
      REXLOG_INFO("[svr2011] people: 2 at ({:.1f},{:.1f},{:.1f}) pos object {:08X}:{}", RdF(base + c2 + 288),
                  RdF(base + c2 + 292), RdF(base + c2 + 296), po, f);
    }
    const uint32_t c0 = Rd32(base + kChars);
    for (const Control& c : g_controls)
      if (c.fighter == c0 && c.input) {
        std::string hex;
        for (uint32_t at = 0; at < 128; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : " ", base[c.input + at]);
        REXLOG_INFO("[svr2011] people: input of 0 at {:08X}:{}", c.input, hex);
      }
  }
}

}  // namespace svr2011

// -- Weapons everywhere ------------------------------------------------------
//
// The weapons lying around at the bell come from a table (bgEtc.pac
// STG/WPON 45010, kept at *(0x82E3BE98) once loaded): per rule id a list of
// 28-byte records (f32 position x/y/z, f32 rotation x/y/z in degrees, s16
// motion, u8 place: 0 ring / 1 floor, u8 model), plus a shared ringside set
// (announce tables, steps). sub_8227B938 makes the weapons as the match loads
// and sub_8227D130 puts them in place at the start. Extreme Rules (0x4D-0x50)
// has no records but loads the Extreme Rules weapons' models (live option
// +56: table 5, ladder 11, trash can 63, guitar 89, crutch 60, ... and the
// chair 4 of every match). For a WEAPONS EVERYWHERE match its list is ours
// while those two run; any other match gets the game's back.
namespace {

constexpr uint32_t kWeaponTable = 0x82E3BE98;
struct Placed {
  uint8_t model, place;
  float x, y, z, yaw;
};
// In the ring y = -12, on the floor 0 (a table stands at y -6.7 by the
// apron); the ring is about +-30 (its apron to about 34), the announce
// tables at z -76. On the camera's side (+z) the barrier is close: weapons
// at z 54-55 lay against it, and a wrestler picking one up there froze for
// the rest of the match (B11, a player's video) - they lie at z 45.
constexpr Placed kPlaced[] = {
    {4, 0, 10.f, -12.f, 12.f, 30.f},      // chairs in the ring
    {4, 0, -12.f, -12.f, -8.f, 200.f},
    {5, 1, 0.f, -6.7f, 45.f, 0.f},        // tables by the apron
    {5, 1, -52.f, 0.f, 0.f, 90.f},
    {11, 1, 52.f, 0.f, 0.f, 90.f},        // a ladder
    {63, 1, 40.f, 0.f, 45.f, 0.f},        // trash can
    {89, 1, -40.f, 0.f, 45.f, 30.f},      // guitar
    {4, 1, 45.f, 0.f, -45.f, 60.f},       // a chair on the floor
};
// 6-MAN: no Extreme Rules weapons - the extreme rules option (+56, which
// loads their models) crashed every 6-man match at its start (sub_824186E8
// with no event data; skipped, a resource read later): six people and that
// pack don't fit (no 6-person rule of the game has it). Its weapons are the
// models every match loads (a normal match's stock under the ring: chairs
// 4, hand weapons 102, 72, 28; 33 / 34 are the steel steps' halves - left out).
constexpr Placed kPlacedSix[] = {
    {4, 0, 10.f, -12.f, 12.f, 30.f},      // chairs in the ring
    {4, 0, -12.f, -12.f, -8.f, 200.f},
    {102, 1, 0.f, 0.f, 45.f, 0.f},        // by the apron
    {72, 1, -52.f, 0.f, 0.f, 90.f},
    {28, 1, 52.f, 0.f, 0.f, 90.f},
    {4, 1, 40.f, 0.f, 45.f, 0.f},         // chairs on the floor
    {4, 1, -40.f, 0.f, 45.f, 30.f},
    {4, 1, 45.f, 0.f, -45.f, 60.f},
};
// HELL IN A CELL: inside its walls (about +-55; the ring +-30, its apron to
// about 34): tables and ladders on the floor around the ring, chairs.
constexpr Placed kPlacedCell[] = {
    {4, 0, 10.f, -12.f, 12.f, 30.f},      // chairs in the ring
    {4, 0, -12.f, -12.f, -8.f, 200.f},
    {5, 1, 0.f, 0.f, 42.f, 0.f},          // tables
    {5, 1, -42.f, 0.f, -12.f, 90.f},
    {11, 1, 40.f, 0.f, 10.f, 90.f},       // ladders
    {11, 1, -10.f, 0.f, -40.f, 0.f},
    {4, 1, 40.f, 0.f, -38.f, 60.f},       // a chair on the floor
};
constexpr uint32_t kSixManRule = 0x08;  // (6-MAN's WEAPONS EVERYWHERE rule)
// STEEL CAGE / INFERNO: in the ring (y -12, inside the ropes at about +-28).
constexpr Placed kPlacedRing[] = {
    {4, 0, -15.f, -12.f, 12.f, 30.f},     // chairs
    {4, 0, 14.f, -12.f, -14.f, 200.f},
    {4, 0, 0.f, -12.f, -19.f, 90.f},
    {102, 0, 0.f, -12.f, 19.f, 0.f},      // hand weapons (a normal match's stock under the ring)
    {72, 0, 17.f, -12.f, 13.f, 0.f},
    {28, 0, -17.f, -12.f, -12.f, 30.f},
};
uint32_t g_weapon_records = 0, g_weapon_records_six = 0, g_weapon_records_cell = 0, g_weapon_records_ring = 0;  // guest: our records
struct Swapped {
  uint32_t entry = 0, count = 0, records = 0;  // the game's, while ours are in
  uint32_t head = 0, head_rule = 0;            // Extreme Rules' head lent to a rule without a list
} g_swapped;

// The rule's {count, records} in the table (0 if none).
uint32_t WeaponEntry(uint8_t* base, uint32_t rule) {
  const uint32_t table = Rd32(base + kWeaponTable);
  if (!table) return 0;
  const int16_t n = int16_t(uint16_t(base[table + 4] << 8 | base[table + 5]));
  const uint32_t heads = Rd32(base + table + 8), entries = Rd32(base + table + 12);
  for (int i = 0; i < n && heads && entries; ++i) {
    const uint32_t h = heads + uint32_t(i) * 8;
    if (uint32_t(base[h] << 8 | base[h + 1]) == rule) return entries + uint32_t(i) * 8;
  }
  return 0;
}

void RestoreWeapons(uint8_t* base) {
  if (!g_swapped.entry) return;
  Wr32(base + g_swapped.entry, g_swapped.count);
  Wr32(base + g_swapped.entry + 4, g_swapped.records);
  if (g_swapped.head) base[g_swapped.head] = uint8_t(g_swapped.head_rule >> 8), base[g_swapped.head + 1] = uint8_t(g_swapped.head_rule);
  g_swapped = {};
}

// The head (rule id, 16 bits) of the rule's list in the table (0 if none).
uint32_t WeaponHead(uint8_t* base, uint32_t rule) {
  const uint32_t table = Rd32(base + kWeaponTable);
  if (!table) return 0;
  const int16_t n = int16_t(uint16_t(base[table + 4] << 8 | base[table + 5]));
  const uint32_t heads = Rd32(base + table + 8);
  for (int i = 0; i < n && heads; ++i)
    if (const uint32_t h = heads + uint32_t(i) * 8; uint32_t(base[h] << 8 | base[h + 1]) == rule) return h;
  return 0;
}

// Our list in (a WEAPONS EVERYWHERE match), or the game's back.
uint32_t g_row_records = 0, g_row_records_row = ~0u, g_row_records_n = 0;  // guest: a backstage row's list

// A backstage mod's own area with weapons=: its list for the area's rule.
bool ApplyRowWeapons(uint8_t* base, uint32_t rule) {
  if (svr2011::g_active_row < 0 || size_t(svr2011::g_active_row) >= svr2011::BackstageRows().size() || !g_memory ||
      Rd32(base + kStoryContext))
    return false;
  const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
  if ((row.weapons.empty() && !row.no_weapons) || rule != kBackstage1v1FirstRule + uint32_t(row.area)) return false;
  const uint32_t entry = WeaponEntry(base, rule);
  if (!entry) return false;
  const uint32_t n = uint32_t(std::min<size_t>(row.weapons.size(), 32));
  if (g_row_records_row != uint32_t(svr2011::g_active_row)) {
    if (!g_row_records) g_row_records = g_memory->SystemHeapAlloc(32 * 28);
    if (!g_row_records) return false;
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = base + g_row_records + i * 28;
      const auto& w = row.weapons[i];
      WrF(r, w.x), WrF(r + 4, w.y), WrF(r + 8, w.z);
      WrF(r + 12, 0.f), WrF(r + 16, w.yaw), WrF(r + 20, 0.f);
      r[24] = uint8_t(15010 >> 8), r[25] = uint8_t(15010 & 0xFF);  // (motion: lying)
      r[26] = 1;                                                    // (on the floor)
      r[27] = uint8_t(w.model);
    }
    g_row_records_row = uint32_t(svr2011::g_active_row);
    g_row_records_n = n;
    REXLOG_INFO("match types: '{}' brings {} weapons", row.label, n);
  }
  g_swapped = {entry, Rd32(base + entry), Rd32(base + entry + 4)};
  Wr32(base + entry, g_row_records_n);
  Wr32(base + entry + 4, g_row_records);
  return true;
}

// Test aid: SVR2011_TEST_PLACED_MODELS=<model>,... - the weapons WEAPONS
// EVERYWHERE places, in its order (to see weapons: OPTIONS -> WEAPONS ones).
int TestPlacedModel(uint32_t i, int model) {
  static const std::vector<int> models = [] {
    std::vector<int> out;
    if (const char* v = std::getenv("SVR2011_TEST_PLACED_MODELS"))
      for (const char* p = v; *p;) {
        out.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
      }
    return out;
  }();
  return i < models.size() && models[i] > 0 ? models[i] : model;
}

void ApplyWeapons(uint8_t* base) {
  RestoreWeapons(base);
  const uint32_t rule = base[0x82E3DE00];
  if (ApplyRowWeapons(base, rule)) return;
  if (!g_weapons || !WeaponsRule(rule) || !g_memory) return;
  uint32_t entry = WeaponEntry(base, rule);
  uint32_t lent = 0;
  if (!entry)  // (6-MAN / HANDICAP: no list of their own - Extreme Rules' head says this rule meanwhile)
    if ((lent = WeaponHead(base, kExtremeFirst))) {
      base[lent] = uint8_t(rule >> 8), base[lent + 1] = uint8_t(rule);
      entry = WeaponEntry(base, rule);
    }
  if (!entry) {
    REXLOG_WARN("match types: no weapon list for rule {:02X}", rule);
    return;
  }
  const bool six = rule == kSixManRule || rule == kGauntlet, cell = rule >= kCellFirst && rule <= kCellLast;
  const bool ring = StructureOf(rule) && !cell;  // (cage, inferno)
  const Placed* list = six ? kPlacedSix : cell ? kPlacedCell : ring ? kPlacedRing : kPlaced;
  const uint32_t n = six ? uint32_t(std::size(kPlacedSix)) : cell ? uint32_t(std::size(kPlacedCell))
                     : ring ? uint32_t(std::size(kPlacedRing)) : uint32_t(std::size(kPlaced));
  uint32_t& records = six ? g_weapon_records_six : cell ? g_weapon_records_cell : ring ? g_weapon_records_ring : g_weapon_records;
  if (!records) {
    records = g_memory->SystemHeapAlloc(n * 28);
    if (!records) return;
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = base + records + i * 28;
      Placed w = list[i];
      // (test aid: SVR2011_TEST_WE_IN_RING=1 - all of them in the ring, in a row: to see them)
      if (std::getenv("SVR2011_TEST_WE_IN_RING")) w.place = 0, w.x = -24.f + 7.f * float(i), w.y = -12.f, w.z = 0.f;
      w.model = uint8_t(TestPlacedModel(i, w.model));
      WrF(r, w.x), WrF(r + 4, w.y), WrF(r + 8, w.z);
      WrF(r + 12, 0.f), WrF(r + 16, w.yaw), WrF(r + 20, 0.f);
      r[24] = uint8_t(15010 >> 8), r[25] = uint8_t(15010 & 0xFF);  // (motion: lying)
      r[26] = w.place;
      r[27] = w.model;
    }
  }
  for (uint32_t i = 0; i < n; ++i)  // (the chosen weapons lie about in place of the game's: OPTIONS -> WEAPONS)
    base[records + i * 28 + 27] = uint8_t(svr2011::ExtremeWeaponsPlacedModel(int(i), TestPlacedModel(i, list[i].model)));
  g_swapped = {entry, Rd32(base + entry), Rd32(base + entry + 4), lent, lent ? kExtremeFirst : 0};
  Wr32(base + entry, n);
  Wr32(base + entry + 4, records);
}

}  // namespace

// HELL IN A CELL: TABLES & LADDERS - the ladders' and tables' motions. A match's rule motions are one group of m.pac
// (MOT/RU..): sub_82325D18 picks it from the live rules - the cell (+20) is RUHI, before Extreme Rules' RUWM (+19, +24,
// +56) - and sub_82325E78 has a loader (sub_82325C58) read its children whose bit is in a flags word: bit 6 (+19 / +24 /
// +56) the ladders' x 6 motions, bit 7 (+18 / +24 / +51 / +56) the tables' x 11. Each child read takes the bank lists'
// next free slot, 40-67 (sub_823261B8). RUHI has no children 6 / 7: a ladder or table picked up in the cell started
// 15943 / 15090 x 6 / 11 with nothing to play, and the wrestler stood in it for the rest of the match (report
// 8a07a5, every time). So a second loader: RUWM, those children only, started after the cell's own - its banks in the
// slots after the cell's (the lists are searched in slot order: the cell's motions come first; RUWM 6 / 7 share no key
// with RUHI anyway). A stock cell has none of those bits; Extreme Rules gets RUWM itself.
namespace {
char g_rule_motions[16] = {};  // (the rule motions' group sub_82325D18 picked last)
std::atomic<bool> g_extra_read{false};  // (our RUWM read not served yet: model_mem.cpp takes no room as no wait)
}  // namespace
namespace svr2011 {
extern const char* const kExtraRuleMotionsRead = "'/MOT/RUWM'";  // (as the file cache's log names a request)
bool ExtraRuleMotionsPending() { return g_extra_read.load(std::memory_order_relaxed); }
void ExtraRuleMotionsRead() { g_extra_read = false; }
}  // namespace svr2011

REX_EXTERN(__imp__sub_82325D18);
REX_HOOK_RAW(sub_82325D18) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82325D18(ctx, base);
  if (out) std::snprintf(g_rule_motions, sizeof g_rule_motions, "%.15s", reinterpret_cast<const char*>(base + out));
}

REX_EXTERN(__imp__sub_82325E78);
REX_HOOK_RAW(sub_82325E78) {
  const uint32_t loading = ctx.r3.u32 ? Rd32(base + ctx.r3.u32 + 272) : 1;  // (already loading: the game's call does nothing)
  g_rule_motions[0] = 0;
  __imp__sub_82325E78(ctx, base);
  constexpr uint32_t kLiveRules = 0x82E3DE00;
  const uint8_t* live = base + kLiveRules;
  const uint32_t bits = (live[19] || live[24] || live[56] ? 1u << 6 : 0) | (live[18] || live[24] || live[51] || live[56] ? 1u << 7 : 0);
  if (loading || std::strcmp(g_rule_motions, "/MOT/RUHI") || !bits || !g_memory) return;
  static uint32_t path = 0;  // (guest: "/MOT/RUWM")
  if (!path && (path = g_memory->SystemHeapAlloc(16))) std::memcpy(base + path, "/MOT/RUWM", 10);
  if (!path) return;
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  // (as sub_82325E78 makes its own: the object from the heap sub_8269A178 names, the one before put back after)
  sub_8269A178(ctx, base);
  const uint32_t object_heap = ctx.r3.u32;
  sub_8269A478(ctx, base);
  const uint32_t was = ctx.r3.u32;
  ctx.r3.u64 = object_heap;
  sub_8269AC60(ctx, base);
  sub_825743A8(ctx, base);  // (the heap the banks are read into, as the game's loader has it)
  const uint32_t heap = ctx.r3.u32;
  ctx.r3.u64 = 64;
  sub_82150000(ctx, base);  // (the loader object)
  if (const uint32_t loader = ctx.r3.u32) {
    ctx.r3.u64 = loader, ctx.r4.u64 = path, ctx.r5.u64 = bits, ctx.r6.u64 = heap;
    g_extra_read = true;
    sub_82325C58(ctx, base);
    REXLOG_INFO("match types: HELL IN A CELL with weapons - Extreme Rules' ladder / table motions (MOT/RUWM bits {:X}) "
                "loaded after the cell's", bits);
  }
  ctx.r3.u64 = was;
  sub_8269AC60(ctx, base);
  ctx = saved;
}

// The crowd area's weapons (crowd_fight.h): with the crowd area open, the rule's placed list gets these too -
// on the floor behind the barricade (SvR 2008's crowd area: barricade z -85, outer wall z -112), fixed places (every
// game of an online match places the same: lockstep). Models every Extreme Rules match makes: chairs, and the trash can
// and guitar of its nine (OPTIONS -> WEAPONS picks: the chosen ones in their place, as WEAPONS EVERYWHERE's).
constexpr Placed kPlacedCrowd[] = {
    {4, 1, -30.f, 0.f, -97.f, 20.f},   // a chair
    {63, 1, 0.f, 0.f, -98.f, 0.f},     // a trash can
    {89, 1, 28.f, 0.f, -96.f, 80.f},   // a guitar
    {4, 1, 60.f, 0.f, -100.f, 140.f},  // a chair by the stands
};
struct {
  uint32_t entry = 0, count = 0, records = 0;
} g_crowd_swapped;
uint32_t g_crowd_records = 0;

void RestoreCrowdWeapons(uint8_t* base) {
  if (!g_crowd_swapped.entry) return;
  Wr32(base + g_crowd_swapped.entry, g_crowd_swapped.count);
  Wr32(base + g_crowd_swapped.entry + 4, g_crowd_swapped.records);
  g_crowd_swapped = {};
}

void AddCrowdWeapons(uint8_t* base) {
  RestoreCrowdWeapons(base);
  // (not with WEAPONS EVERYWHERE's list in: on top of it a quit crashed in the placing, sub_821C1F20)
  if (!g_memory || g_swapped.entry || !svr2011::CrowdAreaFor(base)) return;
  const uint32_t rule = base[0x82E3DE00];
  const uint32_t entry = WeaponEntry(base, rule);
  if (!entry) return;
  const uint32_t n = Rd32(base + entry), records = Rd32(base + entry + 4);
  constexpr uint32_t k = uint32_t(std::size(kPlacedCrowd)), kMax = 64;
  if ((n && !records) || n > kMax) return;  // (Extreme Rules' own list is empty: ours alone)
  if (!g_crowd_records) g_crowd_records = g_memory->SystemHeapAlloc((kMax + k) * 28);
  if (!g_crowd_records) return;
  if (n) std::memcpy(base + g_crowd_records, base + records, size_t(n) * 28);
  for (uint32_t i = 0; i < k; ++i) {
    uint8_t* r = base + g_crowd_records + (n + i) * 28;
    const Placed& w = kPlacedCrowd[i];
    WrF(r, w.x), WrF(r + 4, w.y), WrF(r + 8, w.z);
    WrF(r + 12, 0.f), WrF(r + 16, w.yaw), WrF(r + 20, 0.f);
    r[24] = uint8_t(15010 >> 8), r[25] = uint8_t(15010 & 0xFF);  // (motion: lying)
    r[26] = w.place;
    r[27] = uint8_t(svr2011::ExtremeWeaponsPlacedModel(int(n + i), w.model));
  }
  g_crowd_swapped = {entry, n, records};
  Wr32(base + entry, n + k);
  Wr32(base + entry + 4, g_crowd_records);
  REXLOG_INFO("match types: the crowd area's weapons - {} more on the floor behind the barricade (rule {:02X})", k, rule);
}

// The match's weapons are made (as it loads): sub_8227B938.
REX_EXTERN(__imp__sub_8227B938);
REX_HOOK_RAW(sub_8227B938) {
  ApplyWeapons(base);
  AddCrowdWeapons(base);
  if (g_swapped.entry) REXLOG_INFO("match types: WEAPONS EVERYWHERE - the weapons are made");
  svr2011::ExtremeWeaponsMaking(base, true, ctx);  // (the Extreme Rules weapons as chosen: weapons_page.h)
  __imp__sub_8227B938(ctx, base);
  svr2011::ExtremeWeaponsMaking(base, false, ctx);
  RestoreCrowdWeapons(base);
  RestoreWeapons(base);
}

// ... and put in place at the start: sub_8227D130(weapons).
REX_EXTERN(__imp__sub_8227D130);
REX_HOOK_RAW(sub_8227D130) {
  ApplyWeapons(base);
  AddCrowdWeapons(base);
  __imp__sub_8227D130(ctx, base);
  svr2011::ExtremeWeaponsPlaced(base, g_memory);  // (the stock under the ring: the chosen weapons - weapons_page.h)
  RestoreCrowdWeapons(base);
  RestoreWeapons(base);
}

// The CPU and the weapons lying about. Its behaviour script asks conditions
// by id (sub_828578E8(this, id), *this the AI: +0 its person, +400 its
// position): 157 "a usable weapon lies somewhere" (sub_8280DFB0 any) leads to
// its "go get a weapon" actions (300 any, 301-304 / 329 a kind: sub_82837C30
// finds the nearest), 158 "one can come from under the ring" (the stock list
// at *0x82E3C06C not empty, fewer than 4 out) to the search under it (actions
// 140 / 141, sub_82828490: to the apron, motions 15480 / 15481) - and it
// mostly searched, some of it without asking 158: in WEAPONS EVERYWHERE the
// CPU walked past the placed weapons to the apron. Now, while a usable weapon
// lies within kNearWeapon, 158 is false and an action 140 / 141 or 260 (the
// search itself, sub_82828968) being made (the action factories
// sub_82812838(ai, id), sub_828136D0) is a 300 instead: the CPU goes for that
// weapon; under the ring only when nothing usable is near.
// (test aid: SVR2011_TEST_WE_AI_OFF=1 - as the game)
namespace {
constexpr float kNearWeapon = 90.f;  // (the ring is about +-30, the placed ones at most ~75 from its middle)
// After a CPU's weapon action (one made, or redirected), its next weapon
// choices are the game's own for a while: without that, every dropped weapon
// had another within reach - weapon after weapon, rarely wrestling.
constexpr uint32_t kWeaponCooldown = 25 * 60;  // (updates)
struct WeaponTurn {
  uint32_t ai = 0, at = 0;
};
WeaponTurn g_weapon_turns[8];
bool WeaponCooling(uint8_t* base, uint32_t ai) {
  const uint32_t frames = Rd32(base + 0x82E3CD0C);
  for (const WeaponTurn& w : g_weapon_turns)
    if (w.ai == ai && frames >= w.at && frames - w.at < kWeaponCooldown) return true;
  return false;
}
void WeaponUsed(uint8_t* base, uint32_t ai) {
  const uint32_t frames = Rd32(base + 0x82E3CD0C);
  WeaponTurn* slot = nullptr;  // (this CPU's, else a free one, else the oldest)
  for (WeaponTurn& w : g_weapon_turns)
    if (w.ai == ai) slot = &w;
  for (WeaponTurn& w : g_weapon_turns)
    if (!slot && !w.ai) slot = &w;
  for (WeaponTurn& w : g_weapon_turns)
    if (!slot || (slot->ai != ai && slot->ai && w.at < slot->at)) slot = &w;
  *slot = {ai, frames};
}

// The nearest usable weapon lying within kNearWeapon of the AI (its slot), or -1.
int32_t NearWeapon(PPCContext& ctx, uint8_t* base, uint32_t ai) {
  static const bool ai_off = std::getenv("SVR2011_TEST_WE_AI_OFF") != nullptr;
  if (!g_weapons || ai_off || !ai || !WeaponsRule(base[0x82E3DE00])) return -1;
  static const bool no_cooldown = std::getenv("SVR2011_TEST_WE_NO_COOLDOWN") != nullptr;  // (test aid: 2.0.5's way)
  if (!no_cooldown && WeaponCooling(base, ai)) return -1;
  const uint32_t person = Rd32(base + ai);
  if (!person) return -1;
  PPCContext c = ctx;
  c.r3.u64 = ai, c.r4.u64 = uint64_t(-1), c.r5.u64 = uint64_t(-1);
  sub_8280DFB0(c, base);
  const int32_t slot = int32_t(c.r3.u32);
  const uint32_t item = slot >= 0 && slot < 72 ? Rd32(base + 0x82DE0220 + uint32_t(slot) * 4) : 0;
  if (!item) return -1;
  const float dx = RdF(base + item + 288) - RdF(base + ai + 400), dz = RdF(base + item + 296) - RdF(base + ai + 408);
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d >= kNearWeapon) return -1;
  static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr;
  static uint32_t last_logged = 0;
  const uint32_t frames = Rd32(base + 0x82E3CD0C);
  if (item_log && frames - last_logged > 60) {
    last_logged = frames;
    REXLOG_INFO("item: person {} - not under the ring: slot {} (model {}) lies {:.0f} away, frame {}",
                Rd32(base + person + 1156), slot, Rd32(base + item + 80), d, frames);
  }
  return slot;
}
}  // namespace

bool CondCharacterMissing1xx(uint8_t* base, uint32_t object, uint32_t id);  // (below, with the other tables)
static void EventAnswer(uint32_t id, PPCContext& ctx);

REX_EXTERN(__imp__sub_828578E8);
REX_HOOK_RAW(sub_828578E8) {
  const uint32_t cond = ctx.r4.u32;
  // (the event scripts' conditions 100-199: a missing character - see CharacterMissing below)
  if (CondCharacterMissing1xx(base, ctx.r3.u32, cond)) {
    ctx.r3.u64 = 0;
    return;
  }
  // (a CPU in its weapon cooldown: "a usable weapon lies somewhere" (157) is no - it wrestles)
  static const bool no_cooldown = std::getenv("SVR2011_TEST_WE_NO_COOLDOWN") != nullptr;
  if (ctx.r4.u32 == 157 && ctx.r3.u32 && g_weapons && !no_cooldown && WeaponsRule(base[0x82E3DE00]) &&
      WeaponCooling(base, Rd32(base + ctx.r3.u32))) {
    ctx.r3.u64 = 0;
    return;
  }
  if (ctx.r4.u32 == 158 && ctx.r3.u32 && NearWeapon(ctx, base, Rd32(base + ctx.r3.u32)) >= 0) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828578E8(ctx, base);
  EventAnswer(cond, ctx);
}

// A dive's hit / miss branch (x 30 / 40) in sub_82191BA0: the target gets motion 1333, sub_821EF9A8(diver, target),
// then the target's partner (+96, the pair's link: sub_8218F550 sets each one's to the other) has its +1372 counted
// up - with no check. A target whose link was empty by then crashed the match (2.1.7 report 20261031-022513-dca504:
// online Extreme Rules, 35 min in, Sheamus' diving fist drop 3571 - read of guest 0x55C, lr 8219287C). The target
// gets the diver as its partner there, as the dive's start links them; logged. Match state only: alike online.
REX_EXTERN(__imp__sub_821EF9A8);
REX_HOOK_RAW(sub_821EF9A8) {
  const uint32_t diver = ctx.r3.u32, target = ctx.r4.u32, lr = uint32_t(ctx.lr);
  // (test aids: SVR2011_TEST_DIVE_UNLINK=1 - the target's link emptied here first, the report's state;
  // SVR2011_TEST_DIVE_GUARD_OFF=1 - no link put back: the report's crash)
  static const bool unlink = std::getenv("SVR2011_TEST_DIVE_UNLINK") != nullptr;
  static const bool guard_off = std::getenv("SVR2011_TEST_DIVE_GUARD_OFF") != nullptr;
  if (unlink && lr == 0x8219287C && target) Wr32(base + target + 96, 0);
  __imp__sub_821EF9A8(ctx, base);
  if (guard_off || lr != 0x8219287C || !diver || !target || Rd32(base + target + 96)) return;
  Wr32(base + target + 96, diver);
  static int logged = 0;
  if (logged++ < 8)
    REXLOG_WARN("match types: a dive's branch found its target (person {}) unlinked - linked to the diver (person {}), "
                "match frame {}", Rd32(base + target + 1156), Rd32(base + diver + 1156), Rd32(base + 0x82E3CD0C));
}

// Research aid: SVR2011_TEST_AI_LOG=1 - each CPU action made (both factories): person, action id, match frame; the
// same action again and again folded into one line with its count (a CPU that stands still: what it keeps trying).
namespace {
void AiActionLog(uint8_t* base, uint32_t ai, uint32_t id, int factory) {
  static const bool on = std::getenv("SVR2011_TEST_AI_LOG") != nullptr;
  if (!on || !ai) return;
  const uint32_t person = Rd32(base + ai), who = person && person < 0xF0000000u ? Rd32(base + person + 1156) : 99;
  static std::mutex mu;
  if (who >= 8) {  // (an AI whose +0 is no person 0-7: once each - the second CPU's actions went missing here)
    static std::set<uint32_t> seen;
    std::lock_guard lock(mu);
    if (seen.insert(ai).second)
      REXLOG_INFO("ai: AI {:08X} with +0 {:08X} (person {}) makes action {} (factory {})", ai, person, who, id, factory);
    return;
  }
  static uint32_t last_id[8] = {}, repeats[8] = {}, since[8] = {};
  std::lock_guard lock(mu);
  const uint32_t frames = Rd32(base + 0x82E3CD0C);
  if (last_id[who] == id) {
    ++repeats[who];
    return;
  }
  if (repeats[who])
    REXLOG_INFO("ai: person {} action {} x{} more (frames {}-{})", who, last_id[who], repeats[who], since[who], frames);
  last_id[who] = id, repeats[who] = 0, since[who] = frames;
  REXLOG_INFO("ai: person {} action {} (factory {}) frame {}", who, id, factory, frames);
}

// Research aid: SVR2011_TEST_AI_PROBE=1 - where an AI keeps the action a factory made (the CPU stall: its "current
// action" field). Each made action (AI, object, id) is looked for on the next calls: the
// offsets it turns up at in the AI (0x2000 bytes) or the person its +0 points to (0x1000) are logged, a few each.
void AiProbe(uint8_t* base, uint32_t ai, uint32_t obj, uint32_t id) {
  static const bool on = std::getenv("SVR2011_TEST_AI_PROBE") != nullptr;
  if (!on) return;
  struct Made {
    uint32_t ai, obj, id, tries;
  };
  static std::mutex mu;
  static Made pend[16] = {};
  static std::map<uint32_t, int> told;  // offset -> lines logged
  std::lock_guard lock(mu);
  for (Made& m : pend) {
    if (!m.ai) continue;
    const uint32_t p = Rd32(base + m.ai), who = p && p < 0xF0000000u ? Rd32(base + p + 1156) : 99;
    for (int in_person = 0; in_person < 2; ++in_person) {
      const uint32_t at = in_person ? p : m.ai, size = in_person ? 0x1000 : 0x2000;
      if (!at || at >= 0xF0000000u) continue;
      for (uint32_t off = 0; off < size; off += 4)
        if (Rd32(base + at + off) == m.obj && told[off + (in_person ? 0x100000u : 0u)]++ < 6)
          REXLOG_INFO("ai probe: AI {:08X} (+0 {:08X}, person {}) - action {:08X} (id {}) at {} +{:X}, frame {}", m.ai,
                      p, who, m.obj, m.id, in_person ? "the person" : "the AI", off, Rd32(base + 0x82E3CD0C));
    }
    if (++m.tries > 4) m = {};
  }
  if (ai && obj)
    for (Made& m : pend)
      if (!m.ai) {
        m = {ai, obj, id, 0};
        break;
      }
}
}  // namespace

REX_EXTERN(__imp__sub_82812838);
REX_HOOK_RAW(sub_82812838) {
  const uint32_t id = ctx.r4.u32;
  AiActionLog(base, ctx.r3.u32, id, 1);
  if ((id == 140 || id == 141 || id == 260) && NearWeapon(ctx, base, ctx.r3.u32) >= 0) ctx.r4.u64 = 300;
  // (a weapon action made: this CPU's cooldown starts - WEAPONS EVERYWHERE)
  if (g_weapons && ctx.r3.u32 && (id == 140 || id == 141 || id == 260 || (id >= 300 && id <= 304)))
    WeaponUsed(base, ctx.r3.u32);
  // (with SVR2011_TEST_ITEM_LOG: each weapon action made - asked id, made id)
  static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr;
  if (item_log && (id == 140 || id == 141 || id == 260 || (id >= 300 && id <= 304))) {
    const uint32_t ai = ctx.r3.u32, person = ai ? Rd32(base + ai) : 0;
    REXLOG_INFO("item: person {} action {} -> {} frame {}", person ? Rd32(base + person + 1156) : 99, id, ctx.r4.u32,
                Rd32(base + 0x82E3CD0C));
  }
  const uint32_t ai_in = ctx.r3.u32, id_in = ctx.r4.u32;
  __imp__sub_82812838(ctx, base);
  AiProbe(base, ai_in, ctx.r3.u32, id_in);
}

// ... and its sibling for actions 146-346: 260, the search itself (sub_82828968).
REX_EXTERN(__imp__sub_828136D0);
REX_HOOK_RAW(sub_828136D0) {
  AiActionLog(base, ctx.r3.u32, ctx.r4.u32, 2);
  if (static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr; item_log && ctx.r4.u32 == 260) {
    const uint32_t person = ctx.r3.u32 ? Rd32(base + ctx.r3.u32) : 0;
    REXLOG_INFO("item: person {} action 260 (second factory) frame {}", person ? Rd32(base + person + 1156) : 99,
                Rd32(base + 0x82E3CD0C));
  }
  if (ctx.r4.u32 == 260 && NearWeapon(ctx, base, ctx.r3.u32) >= 0) {
    ctx.r4.u64 = 300;
    sub_82812838(ctx, base);  // (300: the first factory's)
    return;
  }
  const uint32_t ai_in = ctx.r3.u32, id_in = ctx.r4.u32;
  __imp__sub_828136D0(ctx, base);
  AiProbe(base, ai_in, ctx.r3.u32, id_in);
}

// Research aid (with SVR2011_TEST_ITEM_LOG): the CPU's "go get a weapon"
// action starts (sub_82837C30(action): +84 the kind wanted - 301 one in a
// hand, 302 a ladder, 303 a table, 304 a chair, 329 model 1, else any; +52
// the person; +80 the weapon found, -1 none).
REX_EXTERN(__imp__sub_82837C30);
REX_HOOK_RAW(sub_82837C30) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_82837C30(ctx, base);
  static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr;
  if (item_log) {
    const uint32_t ai = Rd32(base + action + 52), person = ai ? Rd32(base + ai) : 0;  // (the AI: +0 its person)
    REXLOG_INFO("item: person {} goes for a weapon - kind {}, found slot {}, frame {} (area {})",
                person ? Rd32(base + person + 1156) : 99, Rd32(base + action + 84), int32_t(Rd32(base + action + 80)),
                Rd32(base + 0x82E3CD0C), ai ? Rd32(base + ai + 208) : 99);
  }
}

// The weapon models a match loads: sub_82301148(list) fills the wanted-model
// list {s32 count; s32 ids[72]} with a hard-coded set per area (parking: 65,
// 63, 17); 45010 (weapons=) only places them. A backstage mod row's weapon
// models and load_weapons= join the list (sub_823010F0(list, id) appends, no
// duplicates) - weapons the room makes later (sub_821D2070) need theirs loaded.
REX_EXTERN(__imp__sub_82301148);
REX_HOOK_RAW(sub_82301148) {
  const uint32_t list = ctx.r3.u32;
  __imp__sub_82301148(ctx, base);
  // (the Extreme Rules option on: its weapons as chosen on MY WWE -> OPTIONS -> WEAPONS - weapons_page.h)
  if (list && base[0x82E3DE00 + 56]) svr2011::ChooseExtremeWeapons(base, list);
  // Test aid: SVR2011_TEST_EXTRA_WEAPONS=<model>,... - more entries in the load list (how much room the weapons'
  // memory has: with SVR2011_TEST_HEAP_LOG the failing heap); <model>=<model> swaps an entry instead (a weapon
  // mod in a stock weapon's place, the list as long as it was).
  if (const char* v = std::getenv("SVR2011_TEST_EXTRA_WEAPONS"); v && *v && list && base[0x82E3DE00 + 56]) {
    for (const char* p = v; *p;) {
      const uint32_t n = Rd32(base + list);
      const uint32_t a = uint32_t(std::atoi(p));
      while (*p && *p != ',' && *p != '=') ++p;
      if (*p == '=') {
        const uint32_t b = uint32_t(std::atoi(++p));
        for (uint32_t i = 0; i < n && i < 72; ++i)
          if (Rd32(base + list + 4 + 4 * i) == a) {
            Wr32(base + list + 4 + 4 * i, b);
            break;
          }
        while (*p && *p != ',') ++p;
      } else if (n < 72) {
        Wr32(base + list + 4 + 4 * n, a);
        Wr32(base + list, n + 1);
      }
      if (*p) ++p;
    }
    REXLOG_INFO("match types: test - the weapon load list has {} entries", Rd32(base + list));
  }
  // (for reports: each load's list - a longer one than the game's crashed loads)
  if (list) REXLOG_INFO("match types: the weapon load list has {} models (rule {:02X})", Rd32(base + list), base[0x82E3DE00]);
  const int r = svr2011::g_active_row;
  if (!list || r < 0 || size_t(r) >= svr2011::BackstageRows().size() || Rd32(base + kStoryContext)) return;
  const auto& row = svr2011::BackstageRows()[size_t(r)];
  std::vector<int> models = row.load_weapons;
  for (const auto& w : row.weapons) models.push_back(w.model);
  if (models.empty()) return;
  const auto saved = ctx;
  for (int m : models) {
    if (int32_t(Rd32(base + list)) >= 47) break;  // (48 loaded-model slots)
    ctx.r3.u64 = list;
    ctx.r4.u64 = uint32_t(m);
    sub_823010F0(ctx, base);
  }
  ctx = saved;
  REXLOG_INFO("match types: '{}' loads weapon models ({} wanted now)", row.label, int32_t(Rd32(base + list)));
}

namespace svr2011 {

uint32_t FighterInput(uint32_t fighter) {
  for (const Control& c : g_controls)
    if (c.fighter == fighter) return c.input;
  return 0;
}

// Menu text of the rows added here (menu_hooks.cpp's string lookup), else 0.
uint32_t MatchTypeString(uint32_t id) {
  if (const uint32_t t = MysteryOpponentString(id, g_memory)) return t;
  if ((id == kRoofLabel || id == kRoofText) && g_memory) {
    static uint32_t text[2] = {};
    const uint32_t k = id == kRoofText;
    const char* s = k ? "The match starts on top of the cell. No pins count up there: shove your opponent off, break "
                        "through the roof or climb down to finish it."
                      : "START ON THE ROOF";
    if (!text[k]) {
      const uint32_t n = uint32_t(std::strlen(s) + 1);
      text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(text[k]), s, n);
    }
    return text[k];
  }
  for (size_t s = 0; s < std::size(kStructures) && g_memory; ++s) {
    const StructureWeapons& st = kStructures[s];
    if (id != st.label && id != st.text) continue;
    const uint32_t k = id == st.text;
    const char* text = k ? st.about : st.name;
    if (!g_structure_text[s][k]) {
      const uint32_t n = uint32_t(std::strlen(text) + 1);
      g_structure_text[s][k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_structure_text[s][k]), text, n);
    }
    return g_structure_text[s][k];
  }
  if (id >= kStagesSetLabel && id < kStagesSetLabel + 2 * svr2011::kThreeStagesSets && g_memory) {
    static const char* const kText[] = {
        "NORMAL / FCA / LMS",
        "A normal fall, then Falls Count Anywhere, then Last Man Standing.",
        "NORMAL / BLOOD / FINISHER",
        "A normal fall, then First Blood, then Finishers: a pin or submission counts only right after a finisher.",
        "NORMAL / LMS / SUBMISSION",
        "A normal fall, then Last Man Standing, then Submission.",
        "TABLE / TLC / LADDER",
        "Through a table, then a TLC fall for the prize, then a Ladder fall for it.",
        "CELL: BLOOD / LMS / SUB",
        "Inside Hell in a Cell: First Blood, then Last Man Standing, then Submission.",
        "TABLE / LADDER / TABLE",
        "Through a table, then a Ladder fall for the prize, then through a table again."};
    const uint32_t k = id - kStagesSetLabel;
    if (!g_stages_set_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_stages_set_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_stages_set_text[k]), kText[k], n);
    }
    return g_stages_set_text[k];
  }
  if ((id == kStagesLabel || id == kStagesText) && g_memory) {
    static const char* const kText[] = {
        "THREE STAGES OF HELL",
        "The first to win two falls, each under another stipulation - the damage carries over. Pick the three."};
    const uint32_t k = id - kStagesLabel;
    if (!g_stages_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_stages_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_stages_text[k]), kText[k], n);
    }
    return g_stages_text[k];
  }
  if ((id == kElimLabel || id == kElimText) && g_memory) {
    static const char* const kText[] = {
        "ELIMINATION",
        "A pin or a give up eliminates that Superstar, and the match goes on until only one is left."};
    const uint32_t k = id - kElimLabel;
    if (!g_elim_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_elim_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_elim_text[k]), kText[k], n);
    }
    return g_elim_text[k];
  }
  if (id >= kGeLabel && id <= kGeErText && g_memory) {
    static const char* const kText[] = {
        "GAUNTLET ELIMINATOR",
        "Six superstars, each on his own: two start, the other four come in one a minute. A pin or a submission "
        "eliminates - the last one left wins.",
        "G. ELIMINATOR: EXTREME RULES",
        "The Gauntlet Eliminator with weapons lying about and no disqualifications."};
    const uint32_t k = id - kGeLabel;
    if (!g_ge_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_ge_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_ge_text[k]), kText[k], n);
    }
    return g_ge_text[k];
  }
  if ((id == kSlobberLabel || id == kSlobberText) && g_memory) {
    static const char* const kText[] = {
        "SLOBBER KNOCKER",
        "One on one against an endless line of opponents. Beat one and the next comes in - how many can "
        "you beat before you're pinned?"};
    const uint32_t k = id - kSlobberLabel;
    if (!g_slobber_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_slobber_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_slobber_text[k]), kText[k], n);
    }
    return g_slobber_text[k];
  }
  if ((id == kWeaponsLabel || id == kWeaponsText) && g_memory) {
    static const char* const kText[] = {
        "WEAPONS EVERYWHERE",
        "Extreme Rules with chairs, tables, a ladder and more already waiting in and around the ring at "
        "the bell."};
    const uint32_t k = id - kWeaponsLabel;
    if (!g_weapons_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_weapons_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_weapons_text[k]), kText[k], n);
    }
    return g_weapons_text[k];
  }
  if (id < kOwnRowLabel || id >= kOwnRowLabel + 8 || !g_memory) return 0;
  const auto& own = BackstageRows();
  const size_t k = id - kOwnRowLabel;
  if (k >= own.size()) return 0;
  if (g_own_row_text.size() < own.size()) g_own_row_text.resize(own.size(), 0);
  if (!g_own_row_text[k]) {
    std::string t = own[k].label;
    for (char& c : t) c = char(std::toupper(static_cast<unsigned char>(c)));
    const uint32_t at = g_memory->SystemHeapAlloc(uint32_t(t.size() + 1));
    std::memcpy(g_memory->TranslateVirtual<char*>(at), t.c_str(), t.size() + 1);
    g_own_row_text[k] = at;
  }
  return g_own_row_text[k];
}

}  // namespace svr2011

// -- Fewer locked rules ------------------------------------------------------
//
// Each rule's 64-byte option record (*(0x82EDE954) + 11900 + id * 64, copied
// from misc.pac by sub_828B5078) has a byte per option; bit 7 locks it: the
// rules screen greys the row (sub_82470BC0) and the match uses the record's
// value, not the player's (sub_828C48E8). Unlocked here, for the matches
// they can't break: K.O. (+2), ROPE BREAK (+3) and GIVE UP (+7) - extra ways
// to win or a rope break, which every one on one / tag / multi-person match
// has - and DQ (+4) where it is locked on (tag, handicap: No DQ can be
// chosen). Left locked: Royal Rumbles (eliminations over the top only),
// Elimination Chamber, backstage and story rules, Lumberjack (its rules are
// the match), count-outs in matches of more than 2 (the count logic assumes
// two), over the top rope (needs the battle royal rule swap), entrances.
namespace {

bool KeepLocked(uint32_t rule) {
  return (rule >= 0x14 && rule <= 0x18) || rule == 0x56 ||  // Royal Rumble
         rule == 0x27 || rule == 0x28 ||                    // Elimination Chamber
         (rule >= 0x19 && rule <= 0x21) || (rule >= 0x61 && rule <= 0x76) ||  // backstage, in-ring brawls
         rule == 0x54 || rule == 0x55 || rule == 0x57 || rule == 0x59 || rule == 0x5A || rule == 0x5C ||
         rule == 0x5D;  // story rules, Lumberjack
}

}  // namespace

REX_EXTERN(__imp__sub_828B5078);
REX_HOOK_RAW(sub_828B5078) {
  const uint32_t rules = ctx.r3.u32;
  __imp__sub_828B5078(ctx, base);
  constexpr uint32_t kCount = 119, kKo = 2, kRopeBreak = 3, kDq = 4, kGiveUp = 7;
  int unlocked = 0;
  for (uint32_t rule = 0; rule < kCount; ++rule) {
    if (KeepLocked(rule)) continue;
    uint8_t* opt = base + rules + kRule2 + rule * kRule2Size;
    for (uint32_t at : {kKo, kRopeBreak, kGiveUp})
      if (opt[at] & 0x80) opt[at] &= 0x7F, ++unlocked;
    if (opt[kDq] == 0x80) opt[kDq] = 0x00, ++unlocked;  // (locked on: No DQ can be chosen)
  }
  REXLOG_INFO("match types: {} locked match rules unlocked", unlocked);
}

// -- MATCH CREATOR: everything allowed in every match ------------------------
//
// The MATCH CREATOR's 3 pages (ENVIRONMENT, WIN CONDITION, RULES) grey rows
// from misc.pac /MRME/MRPD, loaded by sub_82490558(loader): 10 tables at
// loader +48, +52, +64, +68, +72, +76, +80, +84, +88, +92 (chunks 1-10).
// - Per match family (sub_82490918(rule): 68 families, table 0x8201E4F8 -
//   12 bytes each: a u32 rule list, its count) and row: the defaults (1, 3,
//   7) and what is allowed (2 ENVIRONMENT, 8 rows: ring STANDARD / CAGE /
//   HELL IN A CELL / CHAMBER / INFERNO, entrance, replay, momentum; 4 WIN
//   CONDITION, 11 rows: pin and give up, 2 out of 3, ironman, over the top,
//   K.O., last man standing, finisher, first blood, flaming table, climb out,
//   escape; 8 RULES, 6 rows: DQ, rope break, ring out, elimination, falls
//   count anywhere, time limit): 0x09 free, 0x00 greyed, 0x02-0x04 fixed.
// - Combinations: 5 (ring structure x win rows), 6 (a win row's value x the
//   others: 0x0A itself), 9 and 10 (the same for the rules).
// Every greyed (0x00) cell is allowed here - fixed ones stay - but for the
// families kept as they are (KeepFamily: Royal Rumble, Elimination Chamber,
// backstage, in-ring brawls). A row freed here was an option locked off in
// the rule's 64-byte record (bit 7, value 0): those are opened while a
// match's live rules are built (sub_828C48E8 below), so the choice counts.
namespace {

bool KeepFamily(uint32_t f) {
  return f == 12 || f == 26 || f == 55 || f == 60 || f == 48 || f == 49 || f == 61 || (f >= 62 && f <= 67) ||
         f == 1;  // (1: INFERNO 1 on 1 - with LAST MAN STANDING it crashed)
}

// What stays greyed (the sweep, tools/mc_sweep.ps1 - docs/MATCH_TYPES_RESEARCH.md):
// - ladder / TLC families in a cage, cell or inferno ring (the CPU stood idle),
//   and their win conditions as they are (fatal-4-way ladder with LAST MAN
//   STANDING hung in the characters' job);
bool LadderFamily(uint32_t f) { return f == 4 || f == 5 || f == 18 || f == 19 || f == 30 || f == 31 || f == 39 || f == 40 || f == 50; }
// - tag and team families with OVER THE TOP ROPE (crashed at the start);
bool TeamFamily(uint32_t f) { return (f >= 14 && f <= 27) || (f >= 45 && f <= 47) || f == 52; }
// - the INFERNO ring but where it already was and in 1 on 1 / triple threat /
//   fatal-4-way normal (elsewhere the CPU stood idle or, in a tag match,
//   crashed); the CHAMBER ring where it already was (not tested).
bool KeepEnvironment(uint32_t f, uint32_t c) {
  if (c == 3) return true;
  if (c == 4) return !(f == 0 || f == 28 || f == 36);
  return (c == 1 || c == 2) && LadderFamily(f);
}

// A rule's MATCH CREATOR family (sub_82490918; -1: none).
int FamilyOf(const uint8_t* base, uint32_t rule) {
  constexpr uint32_t kFamilyTable = 0x8201E4F8;
  for (uint32_t f = 0; f < 68; ++f) {
    const uint32_t list = Rd32(base + kFamilyTable + f * 12), n = Rd32(base + kFamilyTable + f * 12 + 4);
    for (uint32_t k = 0; list && k < n && k < 16; ++k)
      if (Rd32(base + list + k * 4) == rule) return int(f);
  }
  return -1;
}

// The option record bytes the MATCH CREATOR's rows set (sub_828C48E8, from
// the saved record: +0 ring, +4 pin, +5/+6 falls / ironman, +7 over the top,
// +8 K.O., +9 last man standing, +10 finisher, +11 first blood, +12 flaming
// table, +13 climb out, +14 escape, +15 ring out, +16 rope break, +17 DQ /
// count out, +18 elimination, +19 falls count anywhere, +20 time limit).
constexpr uint8_t kMcBytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20, 21, 22, 23, 25, 26,
                                27, 28, 29, 32, 37, 38, 39, 40, 41, 44, 46, 59, 63};

}  // namespace

REX_EXTERN(__imp__sub_82490558);
REX_HOOK_RAW(sub_82490558) {
  const uint32_t loader = ctx.r3.u32;
  __imp__sub_82490558(ctx, base);
  constexpr uint32_t kFamilies = 68;
  constexpr uint8_t kFree = 0x09;
  // (test aid: SVR2011_TEST_MC_OLD=1 - the tables as the game has them)
  static const bool old = std::getenv("SVR2011_TEST_MC_OLD") != nullptr;
  if (old) return;
  int n = 0;
  // (keep(row, column): a cell left as it is)
  auto free_cells = [&](uint32_t table, uint32_t width, uint32_t rows, bool families, auto keep) {
    if (!table) return;
    for (uint32_t row = 0; row < rows; ++row) {
      if (families && KeepFamily(row)) continue;
      uint8_t* cells = base + table + row * width;
      for (uint32_t c = 0; c < width; ++c)
        if (cells[c] == 0 && !keep(row, c)) cells[c] = kFree, ++n;
    }
  };
  auto none = [](uint32_t, uint32_t) { return false; };
  free_cells(Rd32(base + loader + 52), 8, kFamilies, true, KeepEnvironment);  // ENVIRONMENT
  // WIN CONDITION: FLAMING TABLE (8) stays a table match's own; OVER THE TOP
  // (3) not in team matches.
  free_cells(Rd32(base + loader + 68), 11, kFamilies, true,
             [](uint32_t f, uint32_t c) { return c == 8 || (c == 3 && TeamFamily(f)) || LadderFamily(f); });
  free_cells(Rd32(base + loader + 84), 6, kFamilies, true, none);  // RULES
  // Ring x win: the ring decides FLAMING TABLE, CLIMB OUT and ESCAPE (8-10;
  // the last two a cage's own) and OVER THE TOP (3: not out of a cage, cell
  // or chamber) - left as they are.
  free_cells(Rd32(base + loader + 72), 11, 5, false,
             [](uint32_t ring, uint32_t c) { return c >= 8 || (c == 3 && ring >= 1 && ring <= 3); });
  free_cells(Rd32(base + loader + 76), 11, 24, false, none);  // win x win
  free_cells(Rd32(base + loader + 88), 6, 29, false, none);   // ... x rules
  free_cells(Rd32(base + loader + 92), 6, 19, false, none);   // rules x rules
  static bool logged = false;
  if (!logged) {
    REXLOG_INFO("match types: MATCH CREATOR - {} greyed cells allowed (everything in every match)", n);
    logged = true;
  }
}

// The live rules of a match are built: sub_828C48E8(rule, live, saved) -
// from the rule's option record and the player's MATCH CREATOR record for
// it (saved: 28 bytes, +18 ELIMINATION). For an ELIMINATION row the saved
// record has ELIMINATION on while the live rules are built - the game then
// sets up its elimination as MATCH CREATOR's own does (only the live +37 byte
// was not enough: the first fall ended the match) - and is put back after,
// so the player's MATCH CREATOR settings stay as they were.
REX_EXTERN(__imp__sub_828C48E8);
REX_HOOK_RAW(sub_828C48E8) {
  constexpr uint32_t kSavedElimination = 18;
  if (g_online_from != ~0u && ctx.r3.u32 == g_online_from) {
    REXLOG_INFO("match types: online - the live rules of rule {:02X} built as {:02X} (port rules)", g_online_from, g_online_to);
    ctx.r3.u64 = g_online_to;
    // GAUNTLET ELIMINATOR: each his own team (the lobby's are the 6-man's 3 on 3)
    if (g_online_to == kGauntlet && g_ge)
      if (const uint32_t match = Rd32(base + 0x82EDEB48))
        for (uint32_t i = 0; i < uint32_t(kGePeople); ++i) base[match + kSlots + i * kSlotSize + 5] = uint8_t(i);
  }
  const uint32_t rule = ctx.r3.u32, live = ctx.r4.u32, saved = ctx.r5.u32;
  const bool elim = g_elimination && (rule == kTripleThreat || rule == kFatal4Way) && saved;
  const bool extreme_bytes = g_weapons && WeaponsRule(rule) && (rule < kExtremeFirst || rule > kExtremeLast);
  const uint8_t was = elim ? base[saved + kSavedElimination] : 0;
  if (elim) base[saved + kSavedElimination] = 1;
  // Test aid: SVR2011_TEST_MC="<saved byte>=<value>,..." - the MATCH CREATOR
  // record of every match set so (e.g. "9=1" last man standing, "0=1" a cage).
  static const std::vector<std::pair<uint32_t, uint8_t>> test_mc = [] {
    std::vector<std::pair<uint32_t, uint8_t>> v;
    if (const char* e = std::getenv("SVR2011_TEST_MC"))
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long k = std::strtoul(p, &end, 10);
        if (end == p || *end != '=') break;
        const unsigned long val = std::strtoul(end + 1, &end, 10);
        v.push_back({uint32_t(k), uint8_t(val)});
        for (p = end; *p == ',' || *p == ' ';) ++p;
      }
    return v;
  }();
  uint8_t mc_was[28] = {};
  if (saved && !test_mc.empty()) {
    std::memcpy(mc_was, base + saved, 28);
    std::string set;
    for (const auto& [k, val] : test_mc)
      if (k < 28) base[saved + k] = val, set += fmt::format(" +{}={}", k, val);
    REXLOG_INFO("match types: test - MATCH CREATOR record for rule {:02X}:{}", rule, set);
  }
  // MATCH CREATOR, everything allowed: the options this rule has locked off
  // (bit 7, value 0) are open while its live rules are built.
  static const bool old = std::getenv("SVR2011_TEST_MC_OLD") != nullptr;
  const int family = old || Rd32(base + kStoryContext) ? -1 : FamilyOf(base, rule);
  uint8_t* opt = family >= 0 && !KeepFamily(uint32_t(family)) && Rd32(base + kRules)
                     ? base + Rd32(base + kRules) + kRule2 + rule * kRule2Size
                     : nullptr;
  uint8_t opened[sizeof(kMcBytes)] = {};
  if (opt)
    for (size_t k = 0; k < sizeof(kMcBytes); ++k)
      if (opt[kMcBytes[k]] == 0x80) opt[kMcBytes[k]] = 0x00, opened[k] = 1;
  __imp__sub_828C48E8(ctx, base);
  if (opt)
    for (size_t k = 0; k < sizeof(kMcBytes); ++k)
      if (opened[k]) opt[kMcBytes[k]] = 0x80;
  if (saved && !test_mc.empty()) std::memcpy(base + saved, mc_was, 28);
  if (elim) base[saved + kSavedElimination] = was;
  // WEAPONS EVERYWHERE in 6-MAN / HANDICAP: the bytes Extreme Rules sets in
  // every mode's option record (Extreme Rules 0x4D-0x50 against the normal
  // rules): +3 rope breaks off, +10 0, +27 DISQUALIFICATION off (the record's locked 0x80 is 0 live), +56 extreme
  // rules (its weapons' models are loaded). (Not +32 0x80 and +47 1, set only
  // by the 2 on 2 and fatal-4-way ones: a 6-man match crashed in its match
  // screen, sub_824186E8.)
  if (extreme_bytes && live) {
    uint8_t* l = base + live;
    l[3] = 0, l[10] = 0, l[27] = 0;  // (+27 DISQUALIFICATION: 0 off - the game has masked the lock bit 0x80 off by now)
    const bool ring = StructureOf(rule) && (rule < kCellFirst || rule > kCellLast);  // (cage, inferno)
    if (rule != kSixManRule && rule != kGauntlet && !ring) l[56] = 1;  // (6-MAN / the six-person gauntlet / cage / inferno: not - see kPlacedSix, kPlacedRing)
    if (rule == kGauntlet || ring) l[4] = 1;                            // (RING OUT: no exiting)
    if (rule == kSixManRule) l[5] = 0;                                   // (6-MAN: no count-outs; out of the ring is fine, as in Extreme Rules)
    REXLOG_INFO("match types: WEAPONS EVERYWHERE - rule {:02X} with the Extreme Rules options", rule);
  }
  svr2011::MysteryOpponentLive(base, live);  // (entrances on: mystery_opponent.h)
  // Test aid (match replays, docs/MATCH_RECORDINGS.md): SVR2011_TEST_ENTRANCES=0|1 - the live rules' ENTRANCES
  // (+38) for this match: off, the people start in their in-ring places at the bell.
  if (const char* v = std::getenv("SVR2011_TEST_ENTRANCES"); v && live) {
    base[live + 38] = uint8_t(*v == '1');
    REXLOG_INFO("match types: test - entrances {} (live rules {:08X})", *v == '1' ? "on" : "off", live);
  }
}

// Research aid (online match types): SVR2011_TEST_ONLINE_TRACE=1 - the
// match families (sub_82490918 rule -> family, sub_82490980 rule -> its
// flag) and the online lists' getters (sub_82470A60 count, sub_82470AA0
// list, sub_82470AE0 label) logged with their callers (the first 40 calls of
// each caller).
namespace {
bool OnlineTrace() {
  static const bool on = std::getenv("SVR2011_TEST_ONLINE_TRACE") != nullptr;
  return on;
}
void TraceCall(const char* fn, uint32_t lr, uint32_t a, uint32_t b, uint32_t result) {
  static std::map<std::pair<std::string, uint32_t>, int> seen;
  if (++seen[{fn, lr}] > 40) return;
  REXLOG_INFO("online trace: {}({:X}, {:X}) = {:X} from {:08X}", fn, a, b, result, lr);
}
}  // namespace

#define SVR2011_ONLINE_TRACE(fn)                                              \
  REX_EXTERN(__imp__##fn);                                                    \
  REX_HOOK_RAW(fn) {                                                          \
    if (!OnlineTrace()) return __imp__##fn(ctx, base);                        \
    const uint32_t lr = uint32_t(ctx.lr), a = ctx.r3.u32, b = ctx.r4.u32;     \
    __imp__##fn(ctx, base);                                                   \
    TraceCall(#fn, lr, a, b, ctx.r3.u32);                                     \
  }
SVR2011_ONLINE_TRACE(sub_82490918)
SVR2011_ONLINE_TRACE(sub_82490980)
SVR2011_ONLINE_TRACE(sub_82470A60)
SVR2011_ONLINE_TRACE(sub_82470AA0)
SVR2011_ONLINE_TRACE(sub_82470AE0)
SVR2011_ONLINE_TRACE(sub_828B4A48)

// Test aids for the event scripts' conditions (both tables below): SVR2011_TEST_EVENT_LOG=1 - each id's first
// answer of each value, with its caller; SVR2011_TEST_EVENT_TRUE=<id>[,<id>...] - those ids answer 1 (an
// objective reached without walking there: Road to WrestleMania's backstage walks).
static void EventAnswer(uint32_t id, PPCContext& ctx) {
  static const bool log = std::getenv("SVR2011_TEST_EVENT_LOG") != nullptr;
  static const std::set<uint32_t> forced = [] {
    std::set<uint32_t> s;
    if (const char* v = std::getenv("SVR2011_TEST_EVENT_TRUE"))
      for (const char* q = v; *q;) {
        s.insert(uint32_t(std::strtoul(q, nullptr, 10)));
        while (*q && *q != ',') ++q;
        if (*q == ',') ++q;
      }
    return s;
  }();
  if (forced.count(id)) {
    static std::set<uint32_t> told;
    if (told.insert(id).second) REXLOG_INFO("[svr2011] event condition {}: test - answered 1 (was {})", id, ctx.r3.u32);
    ctx.r3.u64 = 1;
  }
  if (!log) return;
  static std::mutex mu;
  static std::set<uint64_t> seen;
  std::lock_guard lock(mu);
  if (seen.insert(uint64_t(id) << 32 | ctx.r3.u32).second)
    REXLOG_INFO("[svr2011] event condition {} = {} (from {:08X})", id, ctx.r3.u32, uint32_t(ctx.lr));
}

// A condition of the game's event scripts (sub_82856B00(object, id), 99 of
// them): id 32 reads the object's character's +20 (another character - the
// one it's handling) and that one's +212 / +220 without checking it's there
// (id 33, its neighbour, checks +12 first). With none, the console read near
// address 0 and the condition came out false; here it crashed - Road to
// WrestleMania, Jericho's week 12, the backstage fight with Triple H, right
// after the vending machine. With no character there the condition is false,
// as on the console.
REX_EXTERN(__imp__sub_82856B00);
REX_HOOK_RAW(sub_82856B00) {
  // (the 75 ids whose case starts with the object's character - none there: false too; id 32 also its
  // +20, the one it handles. The others read plain values off the character, or check before going
  // further; the 24 ids that don't read the character are left alone - all 99 cases read through.)
  constexpr uint64_t kReadsChar[2] = {0xC01FADCFFFFFFFFEull, 0x00000007803FFFFFull};  // (bit = id)
  const uint32_t id = ctx.r4.u32;
  if (ctx.r3.u32 && id < 128 && (kReadsChar[id / 64] >> (id % 64) & 1)) {
    const uint32_t ch = Rd32(base + ctx.r3.u32);
    // (ids 30, 31 and 97 hand the character's +4 to sub_82804EC8 / sub_82804FD0 / sub_82809A38, which read its +1768
    // at once: a CPU's check at a match's start - clock 13, the people still on the stage with entrances off - found
    // +4 empty, an access violation in sub_82804EC8, 2 of 10 test matches on 3 Nov)
    static const bool plus4_off = std::getenv("SVR2011_TEST_COND_GUARD_OFF") != nullptr;  // (test: the crash)
    if (!ch || (id == 32 && !Rd32(base + ch + 20)) ||
        (!plus4_off && (id == 30 || id == 31 || id == 97) && !Rd32(base + ch + 4))) {
      static int logged = 0;
      if (logged++ < 3)
        REXLOG_INFO("[svr2011] event condition {} with no character to check - false (as on the console)", id);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82856B00(ctx, base);
  EventAnswer(id, ctx);
}

// ...and its conditions 400-483 (sub_82858B08(object, id), which sub_82859388 hands the 400s): the same kind
// of unchecked reads. Road to WrestleMania, Jericho's week 12 (2.1.4.1, Android, every time, right after the
// cutscene): with id 32 answered false, the script asked id 460 - the character's +4 handed to sub_821B4C70
// (its +212 == 284) - and +4 was empty there: a read of 0xD4, the world update stuck. The ids whose case reads
// the character's +4 (or hands it on) or its +20 - the one it handles - with nothing there answer false, as the
// console's read near address 0 did; id 422 checks +4 itself (a missing character still answers false).
REX_EXTERN(__imp__sub_82858B08);
REX_HOOK_RAW(sub_82858B08) {
  static constexpr uint16_t kPlus4[] = {424, 434, 435, 442, 448, 450, 451, 452, 457, 458, 459, 460, 461, 462, 464, 465};
  static constexpr uint16_t kPlus20[] = {453, 454, 455, 456, 478};
  const uint32_t id = ctx.r4.u32;
  auto listed = [&](const auto& ids) {
    for (const uint16_t v : ids)
      if (v == id) return true;
    return false;
  };
  if (ctx.r3.u32 && id >= 400 && id < 484) {
    const bool plus4 = listed(kPlus4), plus20 = listed(kPlus20);
    if (plus4 || plus20 || id == 422) {
      const uint32_t ch = Rd32(base + ctx.r3.u32);
      if (!ch || (plus4 && !Rd32(base + ch + 4)) || (plus20 && !Rd32(base + ch + 20))) {
        static int logged = 0;
        if (logged++ < 3)
          REXLOG_INFO("[svr2011] event condition {} with no character to check - false (as on the console)", id);
        ctx.r3.u64 = 0;
        return;
      }
    }
  }
  __imp__sub_82858B08(ctx, base);
  EventAnswer(id, ctx);
}

// ...and its conditions 100-199 (sub_828578E8) and 200-299 (sub_82853E38), the same kind again. Road to WrestleMania,
// John Cena's week 2 (2.1.5, PC crash lr 82859424 / ctr 82857DEC, Android a freeze - two reports, 24 Oct): the
// backstage brawl with Ted and Cody, when MVP opens the door - id 119 reads the object's character's +20 (the one it
// handles) and that one's +1828; MVP, just in, handles no one: a read of 0x724. 174 hands +20 on (sub_821B59A8) and
// 284 reads its +446 the same way. With nothing there these answer false, as the console's read near address 0 did;
// so do the ids whose case starts with the object's character when there is none (kReads*: every case that loads
// it, found by reading the cases - scratchpad tool cond_scan.py).
namespace {
bool CharacterMissing(uint8_t* base, uint32_t object, uint32_t id, uint32_t first, const uint64_t (&reads)[2],
                      std::initializer_list<uint32_t> plus20) {
  const uint32_t k = id - first;
  if (!object || k >= 100 || !(reads[k / 64] >> (k % 64) & 1)) return false;
  const uint32_t ch = Rd32(base + object);
  if (!ch) return true;
  for (const uint32_t v : plus20)
    if (v == id && !Rd32(base + ch + 20)) return true;
  return false;
}
}  // namespace

// (sub_828578E8's hook is above, with the weapon cooldown answers)
bool CondCharacterMissing1xx(uint8_t* base, uint32_t object, uint32_t id) {
  constexpr uint64_t kReads1xx[2] = {0xFBFFFFFAEF7CBFFFull, 0x0000000FFF3FFEBFull};  // (bit = id - 100)
  if (!CharacterMissing(base, object, id, 100, kReads1xx, {119, 174})) return false;
  static int logged = 0;
  if (logged++ < 3) REXLOG_INFO("[svr2011] event condition {} with no character to check - false (as on the console)", id);
  return true;
}

REX_EXTERN(__imp__sub_82853E38);
REX_HOOK_RAW(sub_82853E38) {
  constexpr uint64_t kReads2xx[2] = {0xEF1DFF7FFFFFFEFEull, 0x0000000FDFFFFFFFull};  // (bit = id - 200)
  const uint32_t id = ctx.r4.u32;
  if (CharacterMissing(base, ctx.r3.u32, id, 200, kReads2xx, {284})) {
    static int logged = 0;
    if (logged++ < 3) REXLOG_INFO("[svr2011] event condition {} with no character to check - false (as on the console)", id);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82853E38(ctx, base);
  EventAnswer(id, ctx);
}

// ...and the other condition table, sub_82852950(object, id 1-225), that sub_82853718 runs over a byte list of ids.
// Road to WrestleMania, John Cena's week 2 again (2.1.8, PC, every time - reports 20261101-123956-960221 and
// 20261101-130325-f08745: crash lr 82853780 / ctr 82852FBC): MVP joins the backstage brawl and id 38 reads the
// object's character's +20 (the one he handles) and that one's +212 - MVP, just in, handles no one. The ids below
// load the object's character with no check (scratchpad tool cond_scan.py); 33 reads its +4, 35-38 and 64 its +20,
// 68 / 70 / 71 its +0. With nothing there they answer false, as the console's read near address 0 did. (Its own ids:
// not EventAnswer's, which are the other tables'.)
REX_EXTERN(__imp__sub_82852950);
REX_HOOK_RAW(sub_82852950) {
  static constexpr uint8_t kReads[] = {3,   5,   9,   20,  21,  22,  23,  24,  25,  26,  31,  32,  33,  34,  35,  36,  37,
                                       38,  39,  41,  42,  43,  44,  45,  50,  51,  52,  53,  54,  55,  56,  57,  61,  62,
                                       63,  64,  66,  67,  68,  70,  71,  80,  81,  82,  83,  90,  91,  100, 101, 102, 103,
                                       104, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
                                       126, 127, 128, 129, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210};
  const uint32_t object = ctx.r3.u32, id = ctx.r4.u32;
  static const bool guard_off = std::getenv("SVR2011_TEST_COND_LIST_GUARD_OFF") != nullptr;  // (test: the crash)
  if (object && !guard_off && std::find(std::begin(kReads), std::end(kReads), id) != std::end(kReads)) {
    const uint32_t ch = Rd32(base + object);
    const uint32_t at = id == 33 ? 4 : (id >= 35 && id <= 38) || id == 64 ? 20 : id == 68 || id == 70 || id == 71 ? 0 : 1;
    if (!ch || (at != 1 && !Rd32(base + ch + at))) {
      static int logged = 0;
      if (logged++ < 3)
        REXLOG_INFO("[svr2011] event list condition {} with no character to check - false (as on the console)", id);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82852950(ctx, base);
  // (test aids, as EventAnswer's for this table's ids: SVR2011_TEST_EVENT_LOG=1 logs each id's first answer of each
  // value; SVR2011_TEST_EVENT_LIST_TRUE=<id>[,<id>...] answers 1 - a backstage walk's objective without walking)
  static const bool log = std::getenv("SVR2011_TEST_EVENT_LOG") != nullptr;
  static const std::set<uint32_t> forced = [] {
    std::set<uint32_t> s;
    if (const char* v = std::getenv("SVR2011_TEST_EVENT_LIST_TRUE"))
      for (const char* q = v; *q;) {
        s.insert(uint32_t(std::strtoul(q, nullptr, 10)));
        while (*q && *q != ',') ++q;
        if (*q == ',') ++q;
      }
    return s;
  }();
  if (forced.count(id)) ctx.r3.u64 = 1;
  if (log) {
    static std::mutex mu;
    static std::set<uint64_t> seen;
    std::lock_guard lock(mu);
    if (seen.insert(uint64_t(id) << 32 | ctx.r3.u32).second)
      REXLOG_INFO("[svr2011] event list condition {} = {}{}", id, ctx.r3.u32, forced.count(id) ? " (test: forced)" : "");
  }
}

namespace svr2011 {

void SetFightBox(uint8_t* base, float cx, float cz, float hx, float hz) {
  if (!g_fight_box) return;
  WrF(base + g_fight_box + 32, cx);
  WrF(base + g_fight_box + 40, cz);
  const float half[4] = {hx, hz, hx + 5.0f, hz + 5.0f};
  for (uint32_t i = 0; i < 4; ++i) {
    WrF(base + g_fight_box + 64 + i * 4, half[i]);
    WrF(base + 0x82D9E9D4 + i * 4, half[i]);
  }
}

void SnapMatchCamera() { g_camera_snap = kSnapFrames; }

}  // namespace svr2011
