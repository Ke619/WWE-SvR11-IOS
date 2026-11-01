// WWE SmackDown vs. Raw 2011 - the port's menu entries: CONTROLS, GRAPHICS,
// LANGUAGE and BACKGROUNDS in MY WWE -> OPTIONS, ACHIEVEMENTS and JUKEBOX in
// MY WWE and EXIT in the main menu.
//
// tools/patch_menu.py (and game_files.cpp) add the entries to the menu table
// (menu.pac MFLO/0000) with their own label string ids. Here:
//  - the string lookup returns their labels (the game's string tables have no
//    such strings),
//  - choosing an entry opens the port's page instead of the game's screen
//    (graphics_page.h, achievements_page.h) or closes the game.

#include "frame_rate.h"
#include "menu_hooks.h"
#include "thread_cpu.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#endif

#include <cstdio>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "achievements_page.h"
#include "arena_mods.h"
#include "graphics_page.h"
#include "jukebox.h"
#include "backgrounds_page.h"
#include "weapons_page.h"
#include "discord_presence.h"
#include "online.h"
#include "caw_limits.h"
#include "match_record.h"
#include "match_types.h"
#include "mystery_opponent.h"
#include "superstar_mods.h"
#include "touch_controls.h"
#include "native/native_renderer.h"
#if SVR2011_DEBUG_SUITE
#include "debug_suite/debug_suite.h"
#endif

namespace {

constexpr uint32_t kGraphicsLabelId = 0xAFC0;  // keep in sync with tools/patch_menu.py
constexpr uint32_t kExitLabelId = 0xAFC1;
constexpr uint32_t kAchievementsLabelId = 0xAFC2;
constexpr uint32_t kAchievementsTextId = 0xAFC3;  // its description line
constexpr uint32_t kLanguageLabelId = 0xAFC4;
constexpr uint32_t kJukeboxLabelId = 0xAFC5;
constexpr uint32_t kJukeboxTextId = 0xAFC6;
constexpr uint32_t kControlsLabelId = 0xAFC7;
constexpr uint32_t kBackgroundsLabelId = 0x0FA0B200;  // (not 0xAFC8: the game's own, the help bar's A)
constexpr uint32_t kWeaponsLabelId = 0x0FA0B201;
// Character select: the name beside the "?" tile ("Random", looked up by the
// panel code in sub_82465728 for tile kind 4) - that tile opens the managers
// (src/managers.cpp), so it reads "Mods" there (the superstar mods are in its list).
constexpr uint32_t kRandomTileId = 0x0116;
constexpr uint32_t kRandomTileCaller = 0x82465970;
constexpr uint32_t kMainMenuGroup = 0x01;
constexpr uint32_t kMyWweGroup = 0x05;
constexpr uint32_t kAchievementsRow = 4;       // MY WWE: after OPTIONS
constexpr uint32_t kJukeboxRow = 5;            // MY WWE: after ACHIEVEMENTS
constexpr uint32_t kOptionsGroup = 0x11;       // MY WWE -> OPTIONS
constexpr uint32_t kControlsRow = 5;           // its 6th entry (after CHEAT CODES)
constexpr uint32_t kGraphicsRow = 6;           // after CONTROLS
constexpr uint32_t kLanguageRow = 7;           // after GRAPHICS
constexpr uint32_t kBackgroundsRow = 8;        // after LANGUAGE
constexpr uint32_t kWeaponsRow = 9;            // after BACKGROUNDS
constexpr uint32_t kExitRow = 7;               // main menu: after SHOP (row 6)
// Main-menu object fields (sub_82447210 and friends).
constexpr uint32_t kMenuGroup = 380;   // current group (< 1000) or screen id
constexpr uint32_t kMenuCursor = 360;  // cursor row within the group

uint32_t g_label = 0;       // guest address of "GRAPHICS"
uint32_t g_exit_label = 0;  // and of "EXIT"
uint32_t g_ach_label = 0;   // "ACHIEVEMENTS" and its description
uint32_t g_ach_text = 0;
uint32_t g_jukebox_label = 0;  // "JUKEBOX" and its description
uint32_t g_jukebox_text = 0;
uint32_t g_language_label = 0;  // "LANGUAGE"
uint32_t g_controls_label = 0;  // "CONTROLS"
uint32_t g_backgrounds_label = 0;  // "BACKGROUNDS"
uint32_t g_weapons_label = 0;  // "WEAPONS"
uint32_t g_extra_label = 0;     // "Mods" (the "?" tile opens the managers: src/managers.cpp)

// CREATE AN ENTRANCE's motion presets (DIVA / SUPERSTAR / LEGEND nn: entrance scripts 480-563 left from older
// games) by whose entrance they are - English only, the other languages keep the game's text. The names are the
// scripts' own (EVP +0x22, pac\evt) and the DLC.pac ENEX/LSD rows {entrance, string id}; the ones whose script
// name doesn't say who (Diva 03, General Powerfull, Brian Kendrick next to The Brian Kendrick, General Entrance)
// keep the game's label. (Player request, 20 Oct.)
struct PresetName {
  uint32_t id;
  const char* name;
};
constexpr PresetName kPresetNames[] = {
    {0x2009, "MARIA"}, {0x200A, "TORRIE WILSON"}, {0x200B, "ASHLEY"}, {0x200C, "VICTORIA"}, {0x200D, "CANDICE"},
    {0x200E, "JILLIAN"}, {0x200F, "TRISH STRATUS"}, {0x2010, "VAMPIRE DIVA"}, {0x2011, "GYMNAST DIVA"},
    {0x2017, "KURT ANGLE"}, {0x2018, "BOOKER T"}, {0x2019, "KING BOOKER"}, {0x201A, "SABU"}, {0x201B, "SANDMAN"},
    {0x201D, "KENNY DYKSTRA"}, {0x201E, "BOBBY LASHLEY"}, {0x201F, "GREGORY HELMS"}, {0x2020, "KEVIN THORN"},
    {0x2021, "BALLS MAHONEY"}, {0x2022, "LANCE CADE"}, {0x2023, "TREVOR MURDOCH"}, {0x2024, "BIG DADDY V"},
    {0x2025, "SNITSKY"}, {0x2027, "CHUCK PALUMBO"}, {0x2028, "TAZZ"}, {0x2029, "HARDCORE HOLLY"},
    {0x202B, "CHRIS JERICHO (CLASSIC)"}, {0x202C, "RANDY ORTON (CLASSIC)"}, {0x202D, "SHANE MCMAHON"},
    {0x202E, "TOMMY DREAMER"}, {0x202F, "CURT HAWKINS"}, {0x2030, "CHARLIE HAAS"}, {0x2031, "SUPER CRAZY"},
    {0x2032, "BATISTA (CLASSIC)"}, {0x2033, "THE BOOGEYMAN"}, {0x2034, "JIMMY WANG YANG"}, {0x2035, "JEFF HARDY"},
    {0x2036, "JBL"}, {0x2037, "UMAGA"}, {0x2038, "THE BRIAN KENDRICK"}, {0x2039, "THE GREAT KHALI"},
    {0x203A, "JESSE"}, {0x203B, "BOXER"}, {0x203C, "VIP"}, {0x203D, "LADIES MAN"}, {0x203E, "CLUMSY SUPERSTAR"}, {0x207C, "ZOMBIE"},
    {0x2053, "HULK HOGAN"}, {0x2054, "MILLION DOLLAR MAN"}, {0x2055, "MR. PERFECT"}, {0x2056, "DUSTY RHODES"},
    {0x2057, "JUNKYARD DOG"}, {0x2058, "BOB ORTON JR."}, {0x2059, "EDDIE GUERRERO"}, {0x205A, "MICK FOLEY"},
    {0x205B, "RIC FLAIR"}, {0x205C, "VADER"}, {0x205D, "DOINK THE CLOWN"},
};
std::map<uint32_t, uint32_t> g_preset_text;  // string id -> guest string (English only)

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// For the log: the menus' entries by (group, row) -> their label string id,
// from the menu table the game reads (MFLO/0000: 0x74-byte records, a
// group's records consecutive, hidden ones - +0x41 bit 0 - not shown): the
// port's, with its rows added (match_types.cpp, ScriptMenuTable), else the
// installed menu.pac's; and the game's string table (the first lookup's
// manager).
uint32_t g_string_manager = 0;
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_menu_labels;
bool g_menu_labels_read = false;
std::vector<uint8_t> g_menu_table;          // (the port's table, as the game got it)
std::atomic<bool> g_menu_texts_stale{false};  // (a new table after the texts were read)

// For scripted tests (script_input.h): the main-menu object, the entries'
// label texts by (group, row) and a count of the entries chosen.
uint8_t* g_menu = nullptr;
std::mutex g_menu_mutex;
std::map<std::pair<uint32_t, uint32_t>, std::string> g_menu_texts;
std::atomic<uint32_t> g_menu_selects{0};
std::atomic<uint32_t> g_menu_selected{0};  // (group << 16 | row of the last one)
std::atomic<uint32_t> g_main_row{~0u};      // (the main menu's - group 1 - last choice: 2 Road to WrestleMania)

void ReadMenuLabels() {
  g_menu_labels_read = true;
  g_menu_labels.clear();
  std::vector<uint8_t> d;
  size_t kMflo = 0;
  {
    std::lock_guard lock(g_menu_mutex);
    d = g_menu_table;
  }
  if (d.empty()) {
    const auto file = rex::filesystem::GetExecutableFolder() / "pac" / "menu" / "menu.pac";
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    kMflo = 0x25F000;
    if (d.size() < kMflo + 0x6800) return;
  }
  const size_t kFirst = kMflo + 0x28, kRec = 0x74;
  if (d.size() < kFirst) return;
  const uint32_t total = Be32(&d[kMflo + 4]);
  std::map<uint32_t, uint32_t> rows;  // group -> next row
  for (uint32_t i = 0; i < total && kFirst + (i + 1) * kRec <= d.size(); ++i) {
    const uint8_t* r = &d[kFirst + i * kRec];
    if (r[0x41] & 1) continue;  // (hidden)
    const uint32_t group = Be32(r + 0x18);
    g_menu_labels[{group, rows[group]++}] = Be32(r);
  }
}

// A label's text (printable characters only: icons such as MY WWE's logo
// are control codes), through the string lookup below.
std::string LabelText(PPCContext& ctx, uint8_t* base, uint32_t id) {
  std::string label;
  if (!g_string_manager) return label;
  const auto saved = ctx;
  ctx.r3.u64 = g_string_manager;
  ctx.r4.u64 = id;
  sub_82153EF8(ctx, base);
  if (ctx.r3.u32) {
    for (const char* t = reinterpret_cast<const char*>(base + ctx.r3.u32); *t && label.size() < 60; ++t)
      if (static_cast<unsigned char>(*t) >= 0x20) label += *t;
    while (!label.empty() && label.back() == ' ') label.pop_back();
    while (!label.empty() && label.front() == ' ') label.erase(0, 1);
  }
  ctx = saved;
  return label;
}

// The scripts' view of the menus, once (the first time the menu is used).
void ReadMenuTexts(PPCContext& ctx, uint8_t* base) {
  if (!g_menu_labels_read || g_menu_texts_stale.exchange(false)) ReadMenuLabels();
  std::map<std::pair<uint32_t, uint32_t>, std::string> texts;
  for (const auto& [key, id] : g_menu_labels) texts[key] = LabelText(ctx, base, id);
  if (std::getenv("SVR2011_INPUT_FILE")) {  // (scripted tests: the names a "menu" step can use)
    std::map<uint32_t, std::string> groups;
    for (const auto& [key, text] : texts) groups[key.first] += (groups[key.first].empty() ? "" : " | ") + text;
    for (const auto& [group, list] : groups) REXLOG_INFO("script input: menu group {:X}: {}", group, list);
  }
  std::lock_guard lock(g_menu_mutex);
  g_menu_texts = std::move(texts);
}

}  // namespace

namespace svr2011 {

bool ScriptMenuState(uint32_t* group, uint32_t* row) {
  if (!g_menu) return false;
  *group = Be32(g_menu + kMenuGroup);
  *row = Be32(g_menu + kMenuCursor);
  return true;
}

int ScriptMenuRow(uint32_t group, const std::string& label, int* rows) {
  auto norm = [](std::string s) {
    std::string out;
    for (char c : s) {
      if (static_cast<unsigned char>(c) >= 0x80) continue;  // (icons, such as MY WWE's logo)
      if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
      out += char(std::toupper(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  };
  const std::string want = norm(label);
  std::lock_guard lock(g_menu_mutex);
  int found = -1, n = 0;
  for (auto it = g_menu_texts.lower_bound({group, 0}); it != g_menu_texts.end() && it->first.first == group; ++it, ++n)
    if (found < 0 && norm(it->second) == want) found = int(it->first.second);
  if (rows) *rows = n;
  return found;
}

std::string ScriptMenuTexts(uint32_t group) {
  std::lock_guard lock(g_menu_mutex);
  std::string s;
  for (auto it = g_menu_texts.lower_bound({group, 0}); it != g_menu_texts.end() && it->first.first == group; ++it)
    s += (s.empty() ? "" : ", ") + it->second;
  return s;
}

void ScriptMenuTable(const uint8_t* table, size_t size) {
  {
    std::lock_guard lock(g_menu_mutex);
    g_menu_table.assign(table, table + size);
  }
  g_menu_texts_stale = true;  // (read again at the menu's next use: the rows' places moved)
}

// (by its place, not its label: the label is in the player's language)
bool InRoadToWrestleMania() { return g_main_row.load() == 2; }

uint32_t ScriptMenuSelects(uint32_t* group, uint32_t* row) {
  const uint32_t last = g_menu_selected.load();
  *group = last >> 16;
  *row = last & 0xFFFF;
  return g_menu_selects.load();
}

}  // namespace svr2011

namespace svr2011 {

void InstallMenuHooks(rex::memory::Memory* memory) {
  // The labels in the game's language (user_language: the Xbox 360 language
  // ids; the game's own text follows it).
  struct Labels {
    uint32_t language;
    const char* graphics;
    const char* exit;
    const char* achievements;
    const char* achievements_text;
    const char* language_label;
    const char* jukebox;
    const char* jukebox_text;
    const char* controls;
    const char* backgrounds;
    const char* weapons;
  };
  static const Labels kLabels[] = {
      {1, "GRAPHICS", "EXIT", "ACHIEVEMENTS",
       "View your achievements: what you've unlocked and how to earn the rest.", "LANGUAGE", "JUKEBOX",
       "Choose which songs play in the menus.", "CONTROLS", "BACKGROUNDS", "WEAPONS"},
      {4, "GRAPHISMES", "QUITTER", "SUCC\xC3\x88S",
       "Consultez vos succ\xC3\xA8s : ceux d\xC3\xA9" "bloqu\xC3\xA9s et comment obtenir les autres.", "LANGUE", "JUKEBOX",
       "Choisissez les musiques jou\xC3\xA9" "es dans les menus.", "COMMANDES", "ARRI\xC3\x88RE-PLANS", "ARMES"},
      {3, "GRAFIK", "BEENDEN", "ERFOLGE",
       "Deine Erfolge: was du freigeschaltet hast und wie du den Rest bekommst.", "SPRACHE", "JUKEBOX",
       "W\xC3\xA4hle, welche Songs in den Men\xC3\xBC" "s laufen.", "STEUERUNG", "HINTERGR\xC3\x9C" "NDE", "WAFFEN"},
      {5, "GR\xC3\x81" "FICOS", "SALIR", "LOGROS",
       "Mira tus logros: los que has desbloqueado y c\xC3\xB3mo conseguir el resto.", "IDIOMA", "JUKEBOX",
       "Elige qu\xC3\xA9 canciones suenan en los men\xC3\xBA" "s.", "CONTROLES", "FONDOS", "ARMAS"},
      {6, "GRAFICA", "ESCI", "OBIETTIVI",
       "I tuoi obiettivi: quelli sbloccati e come ottenere gli altri.", "LINGUA", "JUKEBOX",
       "Scegli quali brani suonano nei men\xC3\xB9.", "COMANDI", "SFONDI", "ARMI"},
  };
  const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
  const Labels* l = &kLabels[0];
  for (const Labels& k : kLabels)
    if (k.language == language) l = &k;
  if (l == &kLabels[0])  // (English - also the fallback for other ids: CREATE AN ENTRANCE's presets by name)
    for (const PresetName& pn : kPresetNames) {
      const uint32_t n = uint32_t(std::strlen(pn.name) + 1);
      if (const uint32_t t = memory->SystemHeapAlloc(n)) {
        std::memcpy(memory->TranslateVirtual<char*>(t), pn.name, n);
        g_preset_text[pn.id] = t;
      }
    }
  const size_t n_graphics = std::strlen(l->graphics) + 1, n_exit = std::strlen(l->exit) + 1,
               n_ach = std::strlen(l->achievements) + 1, n_text = std::strlen(l->achievements_text) + 1,
               n_language = std::strlen(l->language_label) + 1, n_jukebox = std::strlen(l->jukebox) + 1,
               n_jukebox_text = std::strlen(l->jukebox_text) + 1, n_controls = std::strlen(l->controls) + 1,
               n_backgrounds = std::strlen(l->backgrounds) + 1, n_weapons = std::strlen(l->weapons) + 1;
  g_label = memory->SystemHeapAlloc(
      uint32_t(n_graphics + n_exit + n_ach + n_text + n_language + 6 + n_jukebox + n_jukebox_text + n_controls + n_backgrounds + n_weapons + 16));
  if (!g_label) {
    REXLOG_WARN("menu: no guest memory for the GRAPHICS label");
    return;
  }
  std::memcpy(memory->TranslateVirtual<char*>(g_label), l->graphics, n_graphics);
  g_exit_label = g_label + uint32_t(n_graphics);
  std::memcpy(memory->TranslateVirtual<char*>(g_exit_label), l->exit, n_exit);
  g_ach_label = g_exit_label + uint32_t(n_exit);
  std::memcpy(memory->TranslateVirtual<char*>(g_ach_label), l->achievements, n_ach);
  g_ach_text = g_ach_label + uint32_t(n_ach);
  std::memcpy(memory->TranslateVirtual<char*>(g_ach_text), l->achievements_text, n_text);
  g_language_label = g_ach_text + uint32_t(n_text);
  std::memcpy(memory->TranslateVirtual<char*>(g_language_label), l->language_label, n_language);
  g_extra_label = g_language_label + uint32_t(n_language);
  std::memcpy(memory->TranslateVirtual<char*>(g_extra_label), "Mods", 5);  // (the same in all five; shown in capitals - the tile holds the superstar mods)
  g_jukebox_label = g_extra_label + 6;
  std::memcpy(memory->TranslateVirtual<char*>(g_jukebox_label), l->jukebox, n_jukebox);
  g_jukebox_text = g_jukebox_label + uint32_t(n_jukebox);
  std::memcpy(memory->TranslateVirtual<char*>(g_jukebox_text), l->jukebox_text, n_jukebox_text);
  g_controls_label = g_jukebox_text + uint32_t(n_jukebox_text);
  std::memcpy(memory->TranslateVirtual<char*>(g_controls_label), l->controls, n_controls);
  g_backgrounds_label = g_controls_label + uint32_t(n_controls);
  std::memcpy(memory->TranslateVirtual<char*>(g_backgrounds_label), l->backgrounds, n_backgrounds);
  g_weapons_label = g_backgrounds_label + uint32_t(n_backgrounds);
  std::memcpy(memory->TranslateVirtual<char*>(g_weapons_label), l->weapons, n_weapons);
}

std::string GameString(PPCContext& ctx, uint8_t* base, uint32_t id) { return LabelText(ctx, base, id); }

}  // namespace svr2011

// String lookup: sub_82153EF8(manager, id) -> (r3) the string.
REX_EXTERN(__imp__sub_82153EF8);
REX_HOOK_RAW(sub_82153EF8) {
  if (!g_string_manager) g_string_manager = ctx.r3.u32;  // (for the log's menu names)
  if (const uint32_t s = svr2011::OnlineString(ctx.r4.u32)) {  // (online.h)
    ctx.r3.u64 = s;
    return;
  }
  if (const uint32_t s = svr2011::ArenaSelectString(uint32_t(ctx.lr))) {  // (arena_mods.h)
    ctx.r3.u64 = s;
    return;
  }
  if (const uint32_t s = svr2011::SuperstarModString(ctx.r4.u32)) {  // (arenas branch: superstar_mods.h)
    ctx.r3.u64 = s;
    return;
  }
  if (const uint32_t s = svr2011::MatchTypeString(ctx.r4.u32)) {  // (match_types.h: added rows)
    ctx.r3.u64 = s;
    return;
  }
  if (const auto it = g_preset_text.find(ctx.r4.u32); it != g_preset_text.end()) {
    ctx.r3.u64 = it->second;
    return;
  }
  if (g_label && ctx.r4.u32 == kGraphicsLabelId) {
    ctx.r3.u64 = g_label;
    return;
  }
  if (g_exit_label && ctx.r4.u32 == kExitLabelId) {
    ctx.r3.u64 = g_exit_label;
    return;
  }
  if (g_ach_label && ctx.r4.u32 == kAchievementsLabelId) {
    ctx.r3.u64 = g_ach_label;
    return;
  }
  if (g_ach_text && ctx.r4.u32 == kAchievementsTextId) {
    ctx.r3.u64 = g_ach_text;
    return;
  }
  if (g_jukebox_label && ctx.r4.u32 == kJukeboxLabelId) {
    ctx.r3.u64 = g_jukebox_label;
    return;
  }
  if (g_jukebox_text && ctx.r4.u32 == kJukeboxTextId) {
    ctx.r3.u64 = g_jukebox_text;
    return;
  }
  if (g_controls_label && ctx.r4.u32 == kControlsLabelId) {
    ctx.r3.u64 = g_controls_label;
    return;
  }
  if (g_backgrounds_label && ctx.r4.u32 == kBackgroundsLabelId) {
    ctx.r3.u64 = g_backgrounds_label;
    return;
  }
  if (g_weapons_label && ctx.r4.u32 == kWeaponsLabelId) {
    ctx.r3.u64 = g_weapons_label;
    return;
  }
  if (g_language_label && ctx.r4.u32 == kLanguageLabelId) {
    ctx.r3.u64 = g_language_label;
    return;
  }
  if (g_extra_label && ctx.r4.u32 == kRandomTileId && uint32_t(ctx.lr) == kRandomTileCaller &&
      rex::cvar::Query<bool>("managers_tile")) {
    ctx.r3.u64 = g_extra_label;
    return;
  }
  // Debug: SVR2011_DUMP_STRINGS=<file> writes all the loaded strings (ids
  // 0-0xFFFF) once the menu tables are loaded (first lookup of PLAY).
  static bool dumped = false;
  if (!dumped && ctx.r4.u32 == 0xA029) {
    dumped = true;
    char* path = nullptr;
    size_t n = 0;
    if (_dupenv_s(&path, &n, "SVR2011_DUMP_STRINGS") == 0 && path) {
      if (FILE* f = std::fopen(path, "wb")) {
        const auto saved = ctx;
        for (uint32_t id = 0; id < 0x10000; ++id) {
          ctx.r3.u64 = saved.r3.u64;
          ctx.r4.u64 = id;
          __imp__sub_82153EF8(ctx, base);
          const uint32_t s = ctx.r3.u32;
          if (s) std::fprintf(f, "%04X\t%.120s\n", id, reinterpret_cast<const char*>(base + s));
        }
        std::fclose(f);
        ctx = saved;
      }
      free(path);
    }
  }
  // Debug: SVR2011_LOG_STRINGS=<text> - lookups returning a string with that
  // text, with the caller (to find the code behind a label).
  static const std::string log_text = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "SVR2011_LOG_STRINGS") == 0 && v) {
      s = v;
      free(v);
    }
    return s;
  }();
  if (!log_text.empty()) {
    const uint32_t manager = ctx.r3.u32, id = ctx.r4.u32, caller = uint32_t(ctx.lr);
    __imp__sub_82153EF8(ctx, base);
    if (ctx.r3.u32) {
      const char* s = reinterpret_cast<const char*>(base + ctx.r3.u32);
      if (std::strstr(s, log_text.c_str()))
        REXLOG_INFO("[svr2011] string {:04X} (manager {:08X}, caller {:08X}): {:.80}", id, manager, caller, s);
    }
    return;
  }
  __imp__sub_82153EF8(ctx, base);
}

// Main menu: go into the selected entry - sub_82447210(menu, by_player);
// by_player (r4) is 1 when the player pressed A.
REX_EXTERN(__imp__sub_82447210);
REX_HOOK_RAW(sub_82447210) {
  if (!g_menu || g_menu_texts_stale.load()) ReadMenuTexts(ctx, base);
  g_menu = base + ctx.r3.u32;
  if (ctx.r4.u32 == 1) {
    const uint8_t* menu = base + ctx.r3.u32;
    const uint32_t group = Be32(menu + kMenuGroup), cursor = Be32(menu + kMenuCursor);
    g_menu_selected = group << 16 | (cursor & 0xFFFF);
    if (group == 1) g_main_row = cursor;
    ++g_menu_selects;
    // The entry's label, for the log ("menu select: OPTIONS (group 5 row 3)").
    std::string label;
    if (!g_menu_labels_read) ReadMenuLabels();
    if (auto it = g_menu_labels.find({group, cursor}); it != g_menu_labels.end())
      label = LabelText(ctx, base, it->second);
    REXLOG_INFO("[svr2011] menu select: {}{}group {:X} row {}{}", label, label.empty() ? "" : " (", group, cursor,
                label.empty() ? "" : ")");
    svr2011::LoadWatchMenuSelect(label.c_str());
    svr2011::TouchGameInMatch(false);  // (the touch controller's MENU layout)
    svr2011::SetFrameRateInMatch(false);  // (menus at 60 fps)
    svr2011::native::SetMatchScene(false);  // (wide screens: menus at 16:9)
    svr2011::SetDiscordScene(svr2011::DiscordScene::kMenus);
    if (group == kOptionsGroup && cursor == kGraphicsRow) {
      svr2011::OpenGraphicsPage();
      return;
    }
    if (group == kOptionsGroup && cursor == kControlsRow) {
      svr2011::OpenControlsPage();
      return;
    }
    if (group == kOptionsGroup && cursor == kLanguageRow) {
      svr2011::OpenLanguagePage();
      return;
    }
    if (group == kOptionsGroup && cursor == kBackgroundsRow) {
      svr2011::OpenBackgroundsPage();
      return;
    }
    if (group == kOptionsGroup && cursor == kWeaponsRow) {
      svr2011::OpenWeaponsPage();
      return;
    }
    if (group == kMyWweGroup && cursor == kAchievementsRow) {
      svr2011::OpenAchievementsPage();
      return;
    }
    if (group == kMyWweGroup && cursor == kJukeboxRow) {
      svr2011::OpenJukeboxPage();
      return;
    }
    if (group == kMainMenuGroup && cursor == kExitRow) {
      REXLOG_INFO("[svr2011] EXIT chosen - closing");
      svr2011::RequestExit();
      return;
    }
  }
  __imp__sub_82447210(ctx, base);
}

// ---------------------------------------------------------------------------
// 60 fps where the game drops to 30 (entrances, some cutscenes).
//
// The game's frame rate is one setting: sub_826E1AE8(fps) - SetFrameRate - or
// sub_826E1C88 (halve it, what entrances use) -
// stores it (a global at 0x82EDDBF0: 60, or 30 / 25 in entrances) with the
// timing constants that go with it, and ~100 places read it (a frame step of
// 2 at 30 fps, the D3D present interval through sub_826D8A90 ->
// SetPresentInterval sub_8291D1D0, ...). It always stays at the game's own
// 60 (frame_rate.h: the world runs in 60 Hz steps); these scenes are drawn at
// 30 frames a second only with the setting off. (Only forcing the present
// interval to 1 made entrances play ~1.7x too fast.)
REXCVAR_DEFINE_BOOL(unlock_30fps, true, "GPU",
                    "Run the game's 30 fps scenes (entrances, cutscenes) at 60 fps");

namespace {
void ArmWatch(uint8_t* base);
}

// Entrances halve the rate instead: sub_826E1C88 (60 -> 30, 50 -> 25, with
// the matching constants; sub_826E1D28 restores it). Never done: the game's
// world stays at its 60 Hz steps (frame_rate.h); with the setting off the
// entrance is drawn at 30 frames a second instead, as the original.
REX_EXTERN(__imp__sub_826E1C88);
REX_HOOK_RAW(sub_826E1C88) {
  svr2011::TouchGameInMatch(true);  // entrances: a match (the touch controller's MATCH layout)
  svr2011::SetFrameRateInMatch(true);  // (the chosen frame rate)
  svr2011::native::SetMatchScene(true);  // (wide screens: full width)
  svr2011::SetDiscordScene(svr2011::DiscordScene::kEntrances);
  svr2011::ArmMatchStart(nullptr);  // (switched here already)
#if SVR2011_DEBUG_SUITE
  svr2011::dbg::OnEntrances(true);
#endif
  if (!REXCVAR_GET(unlock_30fps)) svr2011::SetSceneThirtyFps(true);
}

// A match starts: sub_823EEB98(match flow) - once per match, as it loads,
// with or without entrances (not for the training ring). The match states
// switch from it too (entrances may be off; sub_826E1C88 then never runs) -
// but some menus call it as well (ONLINE), so only once the match is running
// (frame_rate.h ArmMatchStart: its frame count counting).
REX_EXTERN(__imp__sub_823EEB98);
REX_HOOK_RAW(sub_823EEB98) {
  svr2011::ArmMatchStart([] {
    REXLOG_INFO("[svr2011] match starts");
    svr2011::NoteMatchLoading(false);  // (model_mem.cpp's load watch)
    svr2011::TouchGameInMatch(true);  // (the touch controller's MATCH layout)
    svr2011::native::SetMatchScene(true);  // (wide screens: full width)
    svr2011::SetDiscordScene(svr2011::DiscordScene::kMatch);
    svr2011::SetFrameRateInMatch(true);  // (the chosen frame rate)
    svr2011::MysteryOpponentReveal("the match runs");  // (mystery_opponent.h)
  });
  svr2011::MatchRecordMatchLoads();  // (the match's rand() seed: match_record.h)
  svr2011::NoteMatchLoading(true);    // (model_mem.cpp's load watch)
  svr2011::ArenaPlaysAsAtLoad(base);  // (a custom arena on another arena's tile: its arena now - arena_mods.h)
  __imp__sub_823EEB98(ctx, base);
}

// The entrances' end: sub_826E1D28 restores the frame rate - the match starts.
REX_EXTERN(__imp__sub_826E1D28);
REX_HOOK_RAW(sub_826E1D28) {
  REXLOG_INFO("[svr2011] entrances over");
#if SVR2011_DEBUG_SUITE
  svr2011::dbg::OnEntrances(false);
#endif
  svr2011::MysteryOpponentReveal("the entrances' end");  // (mystery_opponent.h)
  svr2011::SetDiscordScene(svr2011::DiscordScene::kMatch);
  __imp__sub_826E1D28(ctx, base);
  svr2011::SetSceneThirtyFps(false);
  svr2011::ApplyFrameRate(base);  // (frame_rate.h)
}

REX_EXTERN(__imp__sub_826E1AE8);
REX_HOOK_RAW(sub_826E1AE8) {
  REXLOG_INFO("[svr2011] SetFrameRate({}){}", ctx.r3.u32, ctx.r3.u32 != 60 ? " - kept at 60" : "");
  __imp__sub_826E1AE8(ctx, base);
  svr2011::ApplyFrameRate(base);  // the game's 60 again (frame_rate.h)
  ArmWatch(base);  // debug (SVR2011_WATCH_ADDR)
}

// ---------------------------------------------------------------------------
#if !defined(_WIN32)
namespace {
void ArmWatch(uint8_t*) {}  // (the watch below is Windows only)
}  // namespace
#else
// Debug: SVR2011_WATCH_ADDR=<guest hex address> logs the host stack of every
// write to that guest word (armed once the game has set its frame rate; the
// page is write-protected, other writes to it are single-stepped through).
namespace {
// The guest's physical memory is visible at 0xA0000000, 0xC0000000 and
// 0xE0000000 (separate host mappings): watch the word in all three.
constexpr int kViews = 3;
uintptr_t g_watch_host[kViews] = {};
uintptr_t g_watch_page[kViews] = {};
thread_local int g_watch_stepping = -1;
DWORD g_watch_protect = PAGE_READONLY;  // PAGE_NOACCESS with SVR2011_WATCH_READ (reads too)

void ProtectAll(DWORD protect) {
  DWORD old;
  for (uintptr_t page : g_watch_page)
    if (page) VirtualProtect(reinterpret_cast<void*>(page), 0x1000, protect, &old);
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 &&
      (ep->ExceptionRecord->ExceptionInformation[0] == 1 || g_watch_protect == PAGE_NOACCESS)) {
    const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    const uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    int v = -1;
    for (int i = 0; i < kViews; i++)
      if (g_watch_page[i] && (addr & ~uintptr_t(0xFFF)) == g_watch_page[i]) v = i;
    if (v < 0) return EXCEPTION_CONTINUE_SEARCH;
    if (addr >= g_watch_host[v] && addr < g_watch_host[v] + 4) {
      void* frames[16];
      const USHORT n = CaptureStackBackTrace(0, 16, frames, nullptr);
      HANDLE process = GetCurrentProcess();
      static bool sym = SymInitialize(process, nullptr, TRUE);
      (void)sym;
      std::string stack;
      for (USHORT i = 0; i < n; ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* info = reinterpret_cast<SYMBOL_INFO*>(buf);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(process, DWORD64(frames[i]), &disp, info)) {
          stack += std::string(" < ") + info->Name;
        }
      }
      REXLOG_INFO("[svr2011] watch: {} (view {}) +{}{}", write ? "write" : "read", v, uint32_t(addr - g_watch_host[v]), stack);
    }
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_watch_page[v]), 0x1000, PAGE_READWRITE, &old);
    ep->ContextRecord->EFlags |= 0x100;  // single-step the write, then re-arm
    g_watch_stepping = v;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && g_watch_stepping >= 0) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_watch_page[g_watch_stepping]), 0x1000,
                   g_watch_protect, &old);
    g_watch_stepping = -1;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void ArmWatch(uint8_t* base) {
  static bool armed = false;
  if (armed) return;
  armed = true;
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "SVR2011_WATCH_ADDR") != 0 || !v) return;
  const uint32_t guest = uint32_t(std::strtoul(v, nullptr, 16));
  free(v);
  const uint32_t phys = guest & 0x1FFFFFFF;
  const uint32_t views[kViews] = {0xA0000000u | phys, 0xC0000000u | phys,
                                  (0xE0000000u | phys) + 0x1000};  // E view is offset
  for (int i = 0; i < kViews; i++) {
    g_watch_host[i] = reinterpret_cast<uintptr_t>(base) + views[i];
    g_watch_page[i] = g_watch_host[i] & ~uintptr_t(0xFFF);
  }
  if (_dupenv_s(&v, &n, "SVR2011_WATCH_READ") == 0 && v) {
    g_watch_protect = PAGE_NOACCESS;
    free(v);
  }
  AddVectoredExceptionHandler(1, WatchHandler);
  ProtectAll(g_watch_protect);
  REXLOG_INFO("[svr2011] watch: armed on {:08X} (all views)", guest);
  // The guest heap resets the pages' protection when it (re)commits them:
  // re-arm now and then.
  CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        for (;;) {
          Sleep(50);
          ProtectAll(g_watch_protect);
        }
      },
      nullptr, 0, nullptr);
}
}  // namespace
#endif  // _WIN32
