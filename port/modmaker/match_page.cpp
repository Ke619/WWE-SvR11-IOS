// Match types page: match type mods (docs/MATCH_TYPE_MODS.md). Either the
// switch for one of the port's own match types (type=matchtype, builtin=), or
// a custom match type made from one of the game's 119 rule records with a
// menu row, people, slots, arena, option rules and weapons - the game side of
// custom types is being built (Limit Breaking); the Mod Maker writes the
// format now. The game's rule / option records come from misc.pac (RUL /
// OPT), their names from string.pac.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "mod_check.h"
#include "svrfmt/pac.h"

namespace mm {
namespace match_page {

using namespace svrfmt;

namespace {

// ---- the port's own match types (tools/make_matchtype_mods.py)
struct Builtin {
  const char* key;
  const char* name;
  const char* about;
};
const Builtin kBuiltins[] = {
    {"falls_count_anywhere", "Falls Count Anywhere", "ONE ON ONE, TWO ON TWO, TRIPLE THREAT and FATAL-4-WAY: pins and submissions count anywhere."},
    {"championship_scramble", "Championship Scramble", "FATAL-4-WAY: five superstars, one more each minute, interim champions, the title holder at the bell wins."},
    {"royal_rumble_15_25", "15- and 25-Man Royal Rumble", "ROYAL RUMBLE: 15-MAN and 25-MAN beside 10, 20 and 30."},
    {"lumberjack", "Lumberjack", "6-MAN: LUMBERJACK, with lumberjacks who attack whoever lands on the floor."},
    // (free_roaming_backstage: removed from the game for 2.1.6 - src/match_types.cpp MatchTypeSwitches)
    {"backstage_more_people", "Backstage for 3, 4 and 6", "BACKSTAGE brawls in TRIPLE THREAT, FATAL-4-WAY and 6-MAN."},
    {"weapons_everywhere", "Weapons Everywhere", "EXTREME RULES: weapons already lying in and around the ring."},
    {"slobber_knocker", "Slobber Knocker", "One wrestler against an endless line of opponents."},
    {"three_stages_of_hell", "Three Stages of Hell", "ONE ON ONE -> EXTREME RULES: a normal fall, then Falls Count Anywhere, then Last Man Standing."},
    {"elimination", "Elimination", "TRIPLE THREAT and FATAL-4-WAY: a pin or give up eliminates; the last one left wins."},
    {"mystery_opponent", "Mystery Opponent", "ONE ON ONE -> NORMAL MATCH: the CPU picks your opponent in secret."},
    {"hiac_tables_ladders", "Hell in a Cell: Tables & Ladders", "HELL IN A CELL: tables, ladders and chairs lying inside the cell."},
    {"hiac_roof_start", "Hell in a Cell: Start on the Roof", "HELL IN A CELL: the match starts on top of the cell."},
    {"three_stages_tables_ladders", "Three Stages of Hell: Tables & Ladders", "THREE STAGES OF HELL (one on one, two on two, triple threat, fatal-4-way): TABLE / LADDER / TABLE and TABLE / TLC / LADDER, in the TLC match."},
    {"gauntlet_eliminator", "Gauntlet Eliminator", "HANDICAP: six superstars, two start, one more each minute; a pin or submission eliminates."},
    {"cage_weapons", "Steel Cage: Weapons Inside", "STEEL CAGE: chairs and other weapons lying in the ring."},
    {"inferno_weapons", "Inferno: Weapons Inside", "INFERNO MATCH: chairs and other weapons lying in the ring."},
    {"online_first_blood", "Online First Blood", "ONLINE Player Match: FIRST BLOOD under EXTREME RULES MATCHES (one on one, 3-man)."},
};

// ---- the game's rule and option records
struct Rule {
  int id = 0;
  int participants = 0;
  uint8_t slots[6][3] = {};  // index, team, kind
  int mode = 0;              // +36: 1 normal, 4 special (forced arena)
  int arena = 0;             // +40
  std::string category, family, label, full_name, internal;
  uint8_t opt[64] = {};
};
std::vector<Rule> g_rules;
bool g_rules_loaded = false;

struct OptRule {
  const char* key;
  const char* name;
  int byte;
  bool flag;  // 0/1 (else a number)
};
const OptRule kOpt[] = {
    {"pinfall", "Pinfall", 1, true},          {"ko", "K.O.", 2, true},                   {"rope_break", "Rope break", 3, true},
    {"dq_off", "DQ off", 4, true},            {"count_out", "Count-out", 5, true},       {"over_the_top", "Over the top rope", 6, true},
    {"give_up", "Give up", 7, true},          {"minutes", "Time limit (min)", 8, false}, {"cage", "Cage", 9, false},
    {"iron_man_falls", "Iron Man falls", 12, false}, {"hell_in_a_cell", "Hell in a Cell", 20, true}, {"timed", "Timed", 21, true},
    {"last_man_standing", "Last Man Standing", 26, true}, {"ring_out", "Ring out", 27, true}, {"interference", "Interference", 32, true},
    {"outside", "Outside allowed", 33, true}, {"tornado", "Tornado", 36, true},          {"elimination", "Elimination", 37, true},
    {"entrance", "Entrances", 38, true},      {"first_blood", "First Blood", 40, true},  {"chamber", "Chamber", 41, true},
    {"escape_door", "Escape door", 44, true}, {"extreme_rules", "Extreme Rules", 56, true}, {"tag", "Tag", 58, true},
    {"inferno", "Inferno", 59, true},
};
const char* const kMenus[] = {"ONE ON ONE", "TWO ON TWO", "TRIPLE THREAT", "FATAL-4-WAY", "6-MAN", "HANDICAP", "ROYAL RUMBLE", "BACKSTAGE"};
const char* const kKinds[] = {"wrestler", "special referee", "manager", "gauntlet waiter"};

void LoadRules() {
  g_rules_loaded = true;
  g_rules.clear();
  Bytes misc;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"misc.pac"), misc)) return;
  auto find = [&](const char* magic) -> size_t {
    for (size_t i = 0; i + 4 <= misc.size(); ++i)
      if (!std::memcmp(&misc[i], magic, 4)) return i;
    return std::string::npos;
  };
  const size_t rul = find("RUL\0"), opt = find("OPT\0");
  if (rul == std::string::npos) return;
  const uint32_t n = Le32(&misc[rul + 4]);
  for (uint32_t i = 0; i < n && rul + 8 + 100 * (i + 1) <= misc.size(); ++i) {
    const uint8_t* r = &misc[rul + 8 + 100 * i];
    Rule x;
    x.id = int(i);
    x.participants = Le16(r);
    for (int k = 0; k < 6; ++k) std::memcpy(x.slots[k], r + 2 + 3 * k, 3);
    x.mode = int(Le32(r + 36));
    x.arena = int(Le32(r + 40));
    x.category = GameString(int(Le32(r + 28)));
    x.family = GameString(int(Le32(r + 44)));
    x.label = GameString(int(Le32(r + 48)));
    x.full_name = GameString(int(Le32(r + 56)));
    x.internal = GameString(int(Le32(r + 60)));
    if (opt != std::string::npos && opt + 8 + 64 * (i + 1) <= misc.size()) std::memcpy(x.opt, &misc[opt + 8 + 64 * i], 64);
    g_rules.push_back(std::move(x));
  }
}

std::string RuleTitle(const Rule& r) {
  char b[160];
  std::snprintf(b, sizeof b, "%02X  %s%s%s", r.id, r.full_name.empty() ? r.internal.c_str() : r.full_name.c_str(),
                r.family.empty() ? "" : "  -  ", r.family.c_str());
  return b;
}

// ---- the project
struct Project {
  int kind = 0;  // 0 switch for a port match type, 1 custom
  int builtin = 0;
  char name[64] = "", author[64] = "", version[16] = "1.0", about[200] = "";
  // custom
  int base = 0x2B;
  int menu = 0;
  char submenu[32] = "";
  char label[32] = "";
  int people = 2;
  bool ffa = false;
  bool own_slots = false;
  int slots[6][3] = {{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}, {4, 4, 0}, {5, 5, 0}};
  int arena = -1;        // -1 the base's, 0 the player's pick, else bgNN
  bool opt_set[32] = {};
  int opt_val[32] = {};
  bool opt_lock[32] = {};
  int weapons = -1;      // rule id, -1 none
};
Project g_p;
int g_view_rule = -1;
int g_test_tab = -1;  // --match-tab <tab>[,<rule>]

const Rule* RuleById(int id) {
  for (const auto& r : g_rules)
    if (r.id == id) return &r;
  return nullptr;
}

std::string Id() { return IdFrom(g_p.kind == 0 ? kBuiltins[g_p.builtin].key : g_p.name, "match_type"); }

std::string Manifest() {
  std::string m = "type=matchtype\nid=" + Id() + "\nname=" + (g_p.kind == 0 && !g_p.name[0] ? kBuiltins[g_p.builtin].name : g_p.name) +
                  "\nauthor=" + g_p.author + "\nversion=" + g_p.version + "\n" + MadeWith();
  const std::string about = g_p.about[0] ? g_p.about : g_p.kind == 0 ? kBuiltins[g_p.builtin].about : "";
  if (!about.empty()) m += "about=" + about + "\n";
  if (g_p.kind == 0) {
    m += std::string("builtin=") + kBuiltins[g_p.builtin].key + "\n";
    return m;
  }
  char b[128];
  std::snprintf(b, sizeof b, "base=0x%02X\n", g_p.base);
  m += b;
  m += std::string("menu=") + kMenus[g_p.menu] + "\n";
  if (g_p.submenu[0]) m += std::string("submenu=") + g_p.submenu + "\n";
  if (g_p.label[0]) m += std::string("label=") + g_p.label + "\n";
  m += "people=" + std::to_string(g_p.people) + "\nffa=" + (g_p.ffa ? "1" : "0") + "\n";
  if (g_p.own_slots) {
    m += "slots=";
    for (int k = 0; k < g_p.people && k < 6; ++k)
      m += (k ? "," : "") + std::to_string(g_p.slots[k][0]) + ":" + std::to_string(g_p.slots[k][1]) + ":" + std::to_string(g_p.slots[k][2]);
    m += "\n";
  }
  if (g_p.arena >= 0) m += "arena=" + std::to_string(g_p.arena) + "\n";
  for (size_t i = 0; i < std::size(kOpt); ++i)
    if (g_p.opt_set[i]) m += std::string("opt.") + kOpt[i].key + "=" + std::to_string(g_p.opt_val[i]) + (g_p.opt_lock[i] ? "!" : "") + "\n";
  if (g_p.weapons >= 0) std::snprintf(b, sizeof b, "weapons=0x%02X\n", g_p.weapons), m += b;
  return m;
}

void Problems(std::vector<Problem>& p) {
  if (g_p.kind == 1) {
    if (!g_p.name[0]) p.push_back(Error("The match type has no name."));
    if (!RuleById(g_p.base)) p.push_back(Error("The base match isn't one of the game's rules."));
    if (g_p.people < 2 || g_p.people > 6) p.push_back(Error("A match needs 2 to 6 wrestlers.").With("Make it 2", [] { g_p.people = 2; Touch(); }));
    if (g_p.own_slots) {
      bool ok = true;
      for (int k = 0; k < g_p.people; ++k) ok &= g_p.slots[k][0] >= 0 && g_p.slots[k][0] < 6 && g_p.slots[k][1] >= 0 && g_p.slots[k][1] < 6 && g_p.slots[k][2] >= 0 && g_p.slots[k][2] < 4;
      if (!ok) p.push_back(Error("A slot's index / team must be 0-5 and its kind 0-3."));
    }
    if (g_p.menu == 0 && !g_p.submenu[0]) p.push_back(Error("The ONE ON ONE menu is full (it shows 14 choices at most): your match needs a menu of its own inside it."));
    p.push_back(Warning("Brand-new match types don't work in the game yet (switching the port's own match types on and off does): the game skips this one for now."));
    p.push_back(Warning("Online, this match only works when both players have it installed."));
  }
  if (!g_game.empty()) {
    std::error_code ec;
    if (fs::exists(fs::path(g_game) / L"Mods" / L"MatchTypes" / fs::u8path(Id()) / L"manifest.txt", ec))
      p.push_back(Warning("A match type with this name is installed already: installing replaces it."));
  }
}

bool Build(std::vector<ZipEntry>& files) {
  const std::string m = Manifest();
  files.push_back({"manifest.txt", Bytes(m.begin(), m.end())});
  return true;
}

void SaveMod() {
  std::vector<ZipEntry> files;
  Build(files);
  const std::wstring f = PickFile(true, L"Save the match type mod", kModFilter, 1, L"svrmod", (L"matchtype_" + Wide(Id()) + L".svrmod").c_str());
  if (f.empty()) return;
  Status(WriteFile(Utf8(f), ZipWrite(files)) ? "Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +)." : "The mod could not be written.");
}

void Install() {
  std::vector<ZipEntry> files;
  Build(files);
  const fs::path dir = fs::path(g_game) / L"Mods" / L"MatchTypes" / fs::u8path(Id());
  std::string why;  // (the folder replaced as a whole: no "disabled" file - installing switches it on)
  if (!InstallModFiles(dir, files, &why)) { Status("Not installed: " + why); return; }
  Status("Installed: the match type is in the game's menus when it next starts (" + PathStr(dir) + ")." + InstallReport("MatchTypes", dir));
}

void RuleView(const Rule& r) {
  ImGui::Text("Rule %02X: %s", r.id, r.full_name.c_str());
  ImGui::TextDisabled("category %s | menu family %s | row label %s | internal %s", r.category.c_str(), r.family.c_str(), r.label.c_str(), r.internal.c_str());
  std::string slots;
  for (int k = 0; k < r.participants && k < 6; ++k) {
    char b[48];
    std::snprintf(b, sizeof b, "%s%d: team %d, %s", k ? "  |  " : "", r.slots[k][0], r.slots[k][1], r.slots[k][2] < 4 ? kKinds[r.slots[k][2]] : "?");
    slots += b;
  }
  ImGui::Text("%d participants:  %s", r.participants, slots.c_str());
  ImGui::Text("Arena: %s (mode %d)   People (OPT): %d   Free-for-all: %d", r.arena ? ("bg" + std::to_string(r.arena)).c_str() : "the player's pick", r.mode, r.opt[34] & 0x7F, r.opt[35] & 0x7F);
  std::string rules;
  for (const auto& o : kOpt) {
    const uint8_t v = r.opt[o.byte];
    if (!(v & 0x7F) && !(v & 0x80)) continue;
    rules += std::string(o.name) + "=" + std::to_string(v & 0x7F) + (v & 0x80 ? "(locked) " : " ");
  }
  ImGui::TextWrapped("Option rules: %s", rules.empty() ? "all default" : rules.c_str());
}

void SwitchTab() {
  ImGui::TextWrapped("The port adds these match types to the game; each has a switch mod the player turns on or off "
                     "in the launcher's Mods tab. Saving one here makes that switch (the bundled ones come with the "
                     "release).");
  ImGui::Spacing();
  ImGui::SetNextItemWidth(360 * g_scale);
  if (ImGui::BeginCombo("Match type", kBuiltins[g_p.builtin].name)) {
    for (int i = 0; i < int(std::size(kBuiltins)); ++i)
      if (ImGui::Selectable(kBuiltins[i].name, g_p.builtin == i)) g_p.builtin = i;
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("%s", kBuiltins[g_p.builtin].about);
  TextField("Author (optional)", g_p.author, sizeof g_p.author);
  TextField("Version (optional)", g_p.version, sizeof g_p.version, "1.0");
}

void CustomTab() {
  ImGui::TextWrapped("A match type of your own: one of the game's matches as the base, under a new row in a menu "
                     "list, with its own people, slots, arena, option rules and weapons.");
  ImGui::TextColored(kWarn, "The game doesn't play custom match types yet: this page saves them in the format a later update will read.");
  ImGui::Spacing();
  TextField("Name", g_p.name, sizeof g_p.name, "the match type's name");
  TextField("About (optional)", g_p.about, sizeof g_p.about, "one line for the launcher", 420);
  TextField("Author (optional)", g_p.author, sizeof g_p.author);
  TextField("Version (optional)", g_p.version, sizeof g_p.version, "1.0");
  ImGui::Separator();
  const Rule* base = RuleById(g_p.base);
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("Base match", base ? RuleTitle(*base).c_str() : "?")) {
    for (const auto& r : g_rules)
      if (ImGui::Selectable(RuleTitle(r).c_str(), g_p.base == r.id)) {
        g_p.base = r.id;
        g_p.people = std::clamp(int(r.opt[34] & 0x7F), 2, 6);
        g_p.ffa = (r.opt[35] & 0x7F) != 0;
        for (int k = 0; k < 6; ++k) for (int j = 0; j < 3; ++j) g_p.slots[k][j] = r.slots[k][j];
      }
    ImGui::EndCombo();
  }
  Hint("The rule record the match starts from: its family (cage, ladder, backstage ...), flags and slots. The game's "
       "code knows families by rule id, so a cage match stays a cage.");
  if (base) RuleView(*base);
  ImGui::Separator();
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::Combo("Menu list", &g_p.menu, kMenus, int(std::size(kMenus)));
  ImGui::SameLine();
  TextField(g_p.menu == 0 ? "Submenu (needed in ONE ON ONE)" : "Submenu (optional)", g_p.submenu, sizeof g_p.submenu, "e.g. EXTREME RULES", 220);
  TextField("Row text (optional)", g_p.label, sizeof g_p.label, "the name");
  ImGui::SetNextItemWidth(120 * g_scale);
  ImGui::SliderInt("People", &g_p.people, 2, 6);
  ImGui::SameLine();
  ImGui::Checkbox("Free-for-all", &g_p.ffa);
  ImGui::Checkbox("Own slots", &g_p.own_slots);
  Hint("Who the people are: each slot's index, team and kind (wrestler, special referee, manager, gauntlet waiter).");
  if (g_p.own_slots)
    for (int k = 0; k < g_p.people; ++k) {
      ImGui::PushID(k);
      ImGui::SetNextItemWidth(80 * g_scale);
      ImGui::InputInt("##i", &g_p.slots[k][0], 0);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80 * g_scale);
      ImGui::InputInt("team", &g_p.slots[k][1], 0);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * g_scale);
      ImGui::Combo("kind", &g_p.slots[k][2], kKinds, 4);
      ImGui::PopID();
    }
  {
    std::string cur = g_p.arena < 0 ? "the base's" : g_p.arena == 0 ? "the player's pick" : g_p.arena == 78 ? "backstage (bg78)" : "";
    for (int i = 0; i < 20 && cur.empty(); ++i)
      if (g_arenas[i].number == g_p.arena) cur = g_arenas[i].name;
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::BeginCombo("Arena", cur.c_str())) {
      if (ImGui::Selectable("the base's", g_p.arena < 0)) g_p.arena = -1;
      if (ImGui::Selectable("the player's pick", g_p.arena == 0)) g_p.arena = 0;
      if (ImGui::Selectable("backstage (bg78)", g_p.arena == 78)) g_p.arena = 78;
      for (int i = 0; i < 20; ++i)
        if (ImGui::Selectable(g_arenas[i].name, g_p.arena == g_arenas[i].number)) g_p.arena = g_arenas[i].number;
      ImGui::EndCombo();
    }
  }
  if (ImGui::CollapsingHeader("Option rules")) {
    ImGui::TextDisabled("Tick a rule to set it; Lock keeps the player from changing it. Unticked rules stay the base's.");
    for (size_t i = 0; i < std::size(kOpt); ++i) {
      ImGui::PushID(int(i));
      ImGui::Checkbox("##set", &g_p.opt_set[i]);
      ImGui::SameLine();
      ImGui::BeginDisabled(!g_p.opt_set[i]);
      ImGui::SetNextItemWidth(160 * g_scale);
      if (kOpt[i].flag) {
        bool on = g_p.opt_val[i] != 0;
        if (ImGui::Checkbox(kOpt[i].name, &on)) g_p.opt_val[i] = on;
      } else {
        ImGui::InputInt(kOpt[i].name, &g_p.opt_val[i], 0);
        g_p.opt_val[i] = std::clamp(g_p.opt_val[i], 0, 127);
      }
      ImGui::SameLine(360 * g_scale);
      ImGui::Checkbox("Lock", &g_p.opt_lock[i]);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  {
    const Rule* w = g_p.weapons >= 0 ? RuleById(g_p.weapons) : nullptr;
    ImGui::SetNextItemWidth(420 * g_scale);
    if (ImGui::BeginCombo("Placed weapons like", w ? RuleTitle(*w).c_str() : "none (the base's)")) {
      if (ImGui::Selectable("none (the base's)", g_p.weapons < 0)) g_p.weapons = -1;
      for (const auto& r : g_rules)
        if (ImGui::Selectable(RuleTitle(r).c_str(), g_p.weapons == r.id)) g_p.weapons = r.id;
      ImGui::EndCombo();
    }
    Hint("Which match's weapons lie in and around the ring at the start (Extreme Rules 4D, TLC 48 ...).");
  }
  if (ImGui::CollapsingHeader("manifest.txt it writes")) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.55f, 1));
    ImGui::TextUnformatted(Manifest().c_str());
    ImGui::PopStyleColor();
  }
}

void RulesTab() {
  ImGui::TextWrapped("The game's %zu rule records (misc.pac RUL) with their option records: what every match is "
                     "made of. Pick one to see it.", g_rules.size());
  ImGui::BeginChild("rules", ImVec2(420 * g_scale, 0), true);
  for (const auto& r : g_rules)
    if (ImGui::Selectable(RuleTitle(r).c_str(), g_view_rule == r.id)) g_view_rule = r.id;
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("rule", ImVec2(0, 0), true);
  if (const Rule* r = RuleById(g_view_rule)) {
    RuleView(*r);
    if (ImGui::Button("Use as the base of a custom match type")) {
      g_p.kind = 1;
      g_p.base = r->id;
      g_p.people = std::clamp(int(r->opt[34] & 0x7F), 2, 6);
      g_p.ffa = (r->opt[35] & 0x7F) != 0;
      for (int k = 0; k < 6; ++k) for (int j = 0; j < 3; ++j) g_p.slots[k][j] = r->slots[k][j];
    }
  } else {
    ImGui::TextDisabled("Pick a rule.");
  }
  ImGui::EndChild();
}

void Draw() {
  if (!g_rules_loaded && !g_game.empty()) LoadRules();
  Heading("Match types", "A switch for one of the port's match types, or a match type of your own from the game's "
                         "rules - a menu row, people, slots, arena, option rules and weapons.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("mt", ImVec2(0, -48 * g_scale), false);
  if (ImGui::BeginTabBar("mt_tabs")) {
    auto fl = [&](int t) { return g_test_tab == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabItem("Port match type (switch)", nullptr, fl(0))) { g_p.kind = 0; SwitchTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Custom match type", nullptr, fl(1))) { g_p.kind = 1; CustomTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("The game's matches", nullptr, fl(2))) { RulesTab(); ImGui::EndTabItem(); }
    g_test_tab = -1;
    ImGui::EndTabBar();
  }
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SaveMod(); break;
    case 2: Install(); break;
  }
}

std::string StateText() { return Manifest(); }
void Reset() { g_p = Project(); }
void WriteProject(ProjectOut& out) { out.text += Manifest(); }

bool Read(const ProjectIn& in, bool) {
  g_p = Project();
  std::snprintf(g_p.name, sizeof g_p.name, "%s", in.Get("name").c_str());
  std::snprintf(g_p.author, sizeof g_p.author, "%s", in.Get("author").c_str());
  std::snprintf(g_p.version, sizeof g_p.version, "%s", in.Get("version").c_str());
  std::snprintf(g_p.about, sizeof g_p.about, "%s", in.Get("about").c_str());
  if (!g_p.version[0]) std::snprintf(g_p.version, sizeof g_p.version, "1.0");
  const std::string builtin = in.Get("builtin");
  if (!builtin.empty()) {
    g_p.kind = 0;
    for (int i = 0; i < int(std::size(kBuiltins)); ++i)
      if (builtin == kBuiltins[i].key) g_p.builtin = i;
    return true;
  }
  g_p.kind = 1;
  g_p.base = int(std::strtol(in.Get("base").c_str(), nullptr, 0));
  for (int i = 0; i < int(std::size(kMenus)); ++i)
    if (in.Get("menu") == kMenus[i]) g_p.menu = i;
  std::snprintf(g_p.submenu, sizeof g_p.submenu, "%s", in.Get("submenu").c_str());
  std::snprintf(g_p.label, sizeof g_p.label, "%s", in.Get("label").c_str());
  g_p.people = std::clamp(in.GetInt("people", 2), 2, 6);
  g_p.ffa = in.GetInt("ffa", 0) != 0;
  const std::string slots = in.Get("slots");
  if (!slots.empty()) {
    g_p.own_slots = true;
    int k = 0;
    for (size_t at = 0; at < slots.size() && k < 6; ++k) {
      std::sscanf(slots.c_str() + at, "%d:%d:%d", &g_p.slots[k][0], &g_p.slots[k][1], &g_p.slots[k][2]);
      const size_t c = slots.find(',', at);
      at = c == std::string::npos ? slots.size() : c + 1;
    }
  }
  g_p.arena = in.Get("arena").empty() ? -1 : in.GetInt("arena", 0);
  for (size_t i = 0; i < std::size(kOpt); ++i) {
    const std::string v = in.Get((std::string("opt.") + kOpt[i].key).c_str());
    if (v.empty()) continue;
    g_p.opt_set[i] = true;
    g_p.opt_val[i] = std::atoi(v.c_str());
    g_p.opt_lock[i] = v.back() == '!';
  }
  g_p.weapons = in.Get("weapons").empty() ? -1 : int(std::strtol(in.Get("weapons").c_str(), nullptr, 0));
  return true;
}

}  // namespace

void TestTab(int tab, int rule) {
  g_test_tab = tab;
  if (rule >= 0) g_view_rule = rule, g_p.base = rule;
}

PageHooks hooks = {
    "matchtype", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace match_page
}  // namespace mm
