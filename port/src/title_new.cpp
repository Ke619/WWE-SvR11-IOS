// WWE SmackDown vs. Raw 2011 - new titles (title_new.h).

#include "title_new.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "generated/default/svr2011_init.h"
#include "no_mods.h"
#include "superstar_mods.h"
#include "text_guard.h"

namespace fs = std::filesystem;

REX_EXTERN(__imp__sub_828C22E0);
REX_EXTERN(__imp__sub_828BD488);
REX_EXTERN(__imp__sub_828BD4A8);
REX_EXTERN(__imp__sub_828BD508);
REX_EXTERN(__imp__sub_828BD530);
REX_EXTERN(__imp__sub_828BD548);
REX_EXTERN(__imp__sub_828BD5B8);
REX_EXTERN(__imp__sub_828BD6C8);
REX_EXTERN(__imp__sub_828BDB38);
REX_EXTERN(__imp__sub_8243B0A8);
REX_EXTERN(__imp__sub_82588698);
REX_EXTERN(__imp__sub_8244C9B0);
REX_EXTERN(__imp__sub_82732D68);
REX_EXTERN(__imp__sub_8244CF50);
REX_EXTERN(__imp__sub_82735910);

namespace {

constexpr int kStockTitles = 14;   // the game's table: 0x82EDEB50, 14 rows x 16 B
constexpr int kMaxNew = 32;        // (title indices travel as signed bytes elsewhere: well under 127)
constexpr uint32_t kNoHolder = 0x200;
constexpr uint32_t kTable = 0x82EDEB50;

struct NewTitle {
  std::string folder, name, champion;
  bool tag = false;
  uint32_t holder[2] = {kNoHolder, kNoHolder};
  uint32_t guest_name = 0, guest_short = 0;  // (in capitals, as the game's lists write titles)
};
std::vector<NewTitle> g_new;
rex::memory::Memory* g_memory = nullptr;
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string Value(const std::string& text, const char* key) {
  const std::string k = std::string(key) + "=";
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.rfind(k, 0) == 0) return l.substr(k.size());
  }
  return {};
}

NewTitle* Row(int64_t index) {
  const int64_t k = index - kStockTitles;
  return k >= 0 && k < int64_t(g_new.size()) ? &g_new[size_t(k)] : nullptr;
}

fs::path HoldersFile() {
  const fs::path dir = svr2011::SuperstarModsSavesDir();
  return dir.empty() ? fs::path() : dir / "titles.txt";
}

// A holder as the file has it: a superstar mod by its folder (its id can change), else the id.
std::string HolderText(uint32_t id) {
  if (id == kNoHolder) return "-";
  const std::string mod = svr2011::SuperstarModFolder(id);
  return mod.empty() ? std::to_string(id) : "m:" + mod;
}

uint32_t HolderOf(const std::string& s) {
  if (s.empty() || s == "-") return kNoHolder;
  if (s.rfind("m:", 0) == 0) {  // (the mod gone or off: the title stands empty)
    const uint32_t id = svr2011::SuperstarModIdOf(s.substr(2));
    return id ? id : kNoHolder;
  }
  return uint32_t(std::strtoul(s.c_str(), nullptr, 10));
}

void TestCheck(PPCContext& ctx, uint8_t* base);

}  // namespace

namespace svr2011 {

int NewTitleCount() { return int(g_new.size()); }

void InstallNewTitles(rex::memory::Memory* memory) {
  g_memory = memory;
  if (NoMods()) return;
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "Titles";
  std::error_code ec;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec) && e.path().filename().string()[0] != '.' && !fs::exists(e.path() / "disabled", ec) &&
        fs::exists(e.path() / "manifest.txt", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());  // (by folder name: a title's index is only for this run)
  for (const fs::path& f : folders) {
    std::ifstream in(f / "manifest.txt", std::ios::binary);
    const std::string man((std::istreambuf_iterator<char>(in)), {});
    if (Value(man, "type") != "title" || Value(man, "new") != "1") continue;
    if (int(g_new.size()) >= kMaxNew) {
      REXLOG_WARN("[svr2011] new titles: {} left out ({} is the limit)", f.filename().string(), kMaxNew);
      continue;
    }
    NewTitle t;
    t.folder = f.filename().string();
    t.name = CleanUtf8(Value(man, "name"), 120);
    t.champion = CleanUtf8(Value(man, "champion"), 120);
    t.tag = Value(man, "kind") == "tag";
    if (t.name.empty()) {
      REXLOG_WARN("[svr2011] new titles: {} has no name - left out", t.folder);
      continue;
    }
    REXLOG_INFO("[svr2011] new titles: {} = title {} \"{}\"{}", t.folder, kStockTitles + int(g_new.size()), t.name, t.tag ? " (tag)" : "");
    const auto guest = [&](const std::string& text) {
      std::string u = text;
      for (char& c : u) c = char(std::toupper(static_cast<unsigned char>(c)));
      const uint32_t at = memory ? memory->SystemHeapAlloc(uint32_t(u.size() + 1)) : 0;
      if (at) std::memcpy(memory->TranslateVirtual<char*>(at), u.c_str(), u.size() + 1);
      return at;
    };
    if (t.champion.empty()) {  // (as a replaced title's: the name less its "ship", else the name and "Champion")
      std::string up = t.name;
      for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
      t.champion = up.size() > 4 && up.compare(up.size() - 4, 4, "SHIP") == 0 ? t.name.substr(0, t.name.size() - 4) : t.name + " Champion";
    }
    // (the entrance's banner: the game adds " CHAMPION" to this one: short=, else the holder's form less that word)
    std::string short_name = CleanUtf8(Value(man, "short"), 120);
    if (short_name.empty()) {
      short_name = t.champion;
      std::string up = short_name;
      for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
      const std::string word = "CHAMPION";
      if (up.size() >= word.size() && up.compare(up.size() - word.size(), word.size(), word) == 0)
        short_name.resize(short_name.size() - word.size());
      while (!short_name.empty() && short_name.back() == ' ') short_name.pop_back();
      if (short_name.empty()) short_name = t.name;
    }
    t.guest_name = guest(t.name), t.guest_short = guest(short_name);
    g_new.push_back(std::move(t));
  }
}

void NewTitlesLoaded() {
  for (NewTitle& t : g_new) t.holder[0] = t.holder[1] = kNoHolder;
  const fs::path file = HoldersFile();
  if (g_new.empty() || file.empty()) return;
  std::ifstream in(file, std::ios::binary);
  int n = 0;
  for (std::string l; std::getline(in, l);) {
    while (!l.empty() && l.back() == '\r') l.pop_back();
    std::istringstream row(l);
    std::string folder, a, b;
    if (!std::getline(row, folder, '\t') || !std::getline(row, a, '\t') || !std::getline(row, b, '\t')) continue;
    for (NewTitle& t : g_new)
      if (t.folder == folder) t.holder[0] = HolderOf(a), t.holder[1] = HolderOf(b), ++n;
  }
  REXLOG_INFO("[svr2011] new titles: {} of {} have saved holders", n, g_new.size());
}

void NewTitlesForSave() {
  const fs::path file = HoldersFile();
  if (g_new.empty() || file.empty()) return;
  // (rows of titles not installed now stay: the mod switched off and on again keeps its holders)
  std::vector<std::string> keep;
  {
    std::ifstream in(file, std::ios::binary);
    for (std::string l; std::getline(in, l);) {
      while (!l.empty() && l.back() == '\r') l.pop_back();
      const std::string folder = l.substr(0, l.find('\t'));
      if (!folder.empty() && std::none_of(g_new.begin(), g_new.end(), [&](const NewTitle& t) { return t.folder == folder; }))
        keep.push_back(l);
    }
  }
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".new";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    for (const std::string& l : keep) out << l << '\n';
    for (const NewTitle& t : g_new) out << t.folder << '\t' << HolderText(t.holder[0]) << '\t' << HolderText(t.holder[1]) << '\n';
  }
  fs::rename(tmp, file, ec);
  if (ec) REXLOG_WARN("[svr2011] new titles: the holders could not be saved ({})", ec.message());
}

void NewTitlesTestCheck(PPCContext& ctx, uint8_t* base) {
  if (std::getenv("SVR2011_TEST_NEW_TITLES")) TestCheck(ctx, base);
}

}  // namespace svr2011

// The title module's functions, each: the game's own for titles 0-13 (and with no new title installed).

// Is the title in play (unlocked ...).
REX_HOOK_RAW(sub_828C22E0) {
  if (Row(ctx.r3.s32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828C22E0(ctx, base);
}

// A title's display order (the table's +13).
REX_HOOK_RAW(sub_828BD488) {
  if (Row(ctx.r3.s32)) return;  // (r3: its index is its order, after the game's)
  __imp__sub_828BD488(ctx, base);
}

// The title at a display order.
REX_HOOK_RAW(sub_828BD4A8) {
  if (Row(ctx.r3.s32)) return;
  __imp__sub_828BD4A8(ctx, base);
}

// Is it a tag title (the table's +12 == 2).
REX_HOOK_RAW(sub_828BD508) {
  if (const NewTitle* t = Row(ctx.r3.s32)) {
    ctx.r3.u64 = t->tag ? 1 : 0;
    return;
  }
  __imp__sub_828BD508(ctx, base);
}

// The table's +8.
REX_HOOK_RAW(sub_828BD530) {
  if (Row(ctx.r3.s32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828BD530(ctx, base);
}

// A holder: (title, slot 0/1, check in play).
REX_HOOK_RAW(sub_828BD548) {
  if (const NewTitle* t = Row(ctx.r3.s32)) {
    ctx.r3.u64 = t->holder[ctx.r4.u32 & 1];
    return;
  }
  __imp__sub_828BD548(ctx, base);
}

// A holder set: (title, id, slot).
REX_HOOK_RAW(sub_828BD5B8) {
  if (NewTitle* t = Row(ctx.r3.s32)) {
    t->holder[ctx.r5.u32 & 1] = ctx.r4.u32 & 0xFFFF;
    REXLOG_INFO("[svr2011] new titles: {} holder {} is now {}", t->folder, ctx.r5.u32 & 1, ctx.r4.u32 & 0xFFFF);
    return;
  }
  __imp__sub_828BD5B8(ctx, base);
}

// A title's holders back to its defaults.
REX_HOOK_RAW(sub_828BD6C8) {
  if (NewTitle* t = Row(ctx.r3.s32)) {
    t->holder[0] = t->holder[1] = kNoHolder;
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828BD6C8(ctx, base);
}

// The titles a superstar holds: (id, out u32[], 0 singles / 1 tag / else both, check in play) -> count. The callers'
// lists have room for the game's 14.
REX_HOOK_RAW(sub_828BDB38) {
  if (g_new.empty()) {
    __imp__sub_828BDB38(ctx, base);
    return;
  }
  const uint32_t id = ctx.r3.u32, out = ctx.r4.u32, which = ctx.r5.u32;
  __imp__sub_828BDB38(ctx, base);
  uint32_t n = ctx.r3.u32;
  for (size_t k = 0; k < g_new.size() && n < uint32_t(kStockTitles); ++k) {
    const NewTitle& t = g_new[k];
    if (t.tag ? which == 0 : which == 1) continue;
    if (t.holder[0] != id && !(t.tag && t.holder[1] == id)) continue;
    const uint32_t v = uint32_t(kStockTitles + k);
    uint8_t* p = base + out + n * 4;
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    ++n;
  }
  ctx.r3.u64 = n;
}

// A title's name: (title, kind 0 the name / 1 the short name, for " CHAMPION" after it) -> the string.
REX_HOOK_RAW(sub_82588698) {
  if (const NewTitle* t = Row(ctx.r3.s32)) {
    static uint32_t said = 0;
    if (said != uint32_t(ctx.lr)) REXLOG_INFO("[svr2011] new titles: name of {} asked by {:08X}", t->folder, uint32_t(ctx.lr));
    said = uint32_t(ctx.lr);
    ctx.r3.u64 = ctx.r4.u32 == 1 ? t->guest_short : t->guest_name;
    return;
  }
  __imp__sub_82588698(ctx, base);
}

// Championship Scramble's title list: 11 rows on screen (widgets 256 B apart, from *(*(*0x82EDE628 + 56) + 8) + 0x53300:
// name W - 2600, holder's name W + 216), the title of each at +400 of the list, its cursor at -200 and row count at
// -196. With new titles the rows scroll: past the last row (or before the first) the texts and titles move by one and
// the cursor goes back. The game's own 11 rows are the top, as it filled them.
namespace {
struct ListRow {
  uint32_t title = 255, name = 0, holder = 0;
  uint8_t name_on = 0, holder_on = 0;
  uint32_t fade_a = 0, fade_b = 0;  // (the row's floats: a title not in play is dimmed)
};
struct TitleList {
  uint32_t self = 0, widgets = 0;
  std::vector<ListRow> rows;
  int top = 0, last_cursor = 0;
} g_list;
constexpr int kListRows = 11;
// The championship art of each of the game's rows: the widget it shows (sub_82452A48, each frame), the picture is the
// widget's +0x4C, its alpha +0xD4. Scrolled, each row's widget gets the picture of the title now on it; a new title has
// none yet and shows no art.
constexpr uint32_t kArtWidget[kListRows] = {1343, 1342, 1344, 1345, 1347, 1350, 1349, 1346, 1348, 1351, 1352};
uint32_t g_art_picture[kListRows] = {};
bool g_art_known = false;

uint32_t Rd(const uint8_t* base, uint32_t a) { return Be32(base + a); }
void Wr(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

void ShowFrom(uint8_t* base, int top) {
  g_list.top = top;
  for (int i = 0; i < kListRows; ++i) {
    const uint32_t w = g_list.widgets + 0x53300 + uint32_t(i) * 256;
    const ListRow& r = g_list.rows[size_t(top + i)];
    Wr(base, w - 2600, r.name);
    Wr(base, w + 216, r.holder);
    base[w - 2669] = r.name_on;
    base[w + 147] = r.holder_on;
    Wr(base, w - 2604, r.fade_a), Wr(base, w + 212, r.fade_b);
    Wr(base, g_list.self + 400 + uint32_t(i) * 4, r.title);
    const uint32_t art = g_list.widgets + kArtWidget[i] * 256;
    const int stock = top + i;
    if (stock < kListRows) {
      Wr(base, art + 0x4C, g_art_picture[stock]);
      Wr(base, art + 0xD4, 0x3F800000);
    } else {
      Wr(base, art + 0xD4, 0);
    }
  }
  REXLOG_INFO("[svr2011] new titles: title list from row {} of {}", top, g_list.rows.size());
}
}  // namespace

REX_HOOK_RAW(sub_8244C9B0) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8244C9B0(ctx, base);
  if (g_new.empty()) return;
  g_list = TitleList();
  g_list.self = self;
  g_list.widgets = Rd(base, Rd(base, Rd(base, 0x82EDE628) + 56) + 8);
  for (int i = 0; i < kListRows; ++i) {
    const uint32_t w = g_list.widgets + 0x53300 + uint32_t(i) * 256;
    ListRow r;
    r.title = Rd(base, self + 400 + uint32_t(i) * 4);
    r.name = Rd(base, w - 2600), r.holder = Rd(base, w + 216);
    r.name_on = base[w - 2669], r.holder_on = base[w + 147];
    r.fade_a = Rd(base, w - 2604), r.fade_b = Rd(base, w + 212);
    g_list.rows.push_back(r);
  }
  const auto saved = ctx;
  for (size_t k = 0; k < g_new.size(); ++k) {
    const NewTitle& t = g_new[k];
    ListRow r = g_list.rows[0];  // (an in-play row's look: rows 0 and 1 always are)
    r.title = uint32_t(kStockTitles + k);
    r.name = t.guest_name, r.name_on = 5;
    bool shown = t.holder[0] != kNoHolder && t.holder[0] < 322;
    if (shown) {  // (as the game's rows: a holder it can show)
      ctx.r3.u64 = t.holder[0];
      sub_828C1288(ctx, base);
      shown = ctx.r3.u32 == 1;
    }
    if (shown) {
      ctx.r3.u64 = t.holder[0], ctx.r4.u64 = 0;
      sub_825884D8(ctx, base);
      r.holder = ctx.r3.u32, r.holder_on = 5;
    } else {
      ctx.r3.u64 = Rd(base, 0x82E3CD48), ctx.r4.u64 = 281;  // ("NONE")
      sub_82153EF8(ctx, base);
      r.holder = ctx.r3.u32, r.holder_on = 0;
    }
    g_list.rows.push_back(r);
  }
  ctx = saved;
  g_list.last_cursor = 0;
  if (!g_art_known) {  // (the first time: the layout's own; later ones may be as a scrolled list left them)
    g_art_known = true;
    for (int i = 0; i < kListRows; ++i) g_art_picture[i] = Rd(base, g_list.widgets + kArtWidget[i] * 256 + 0x4C);
  }
  ShowFrom(base, 0);
  REXLOG_INFO("[svr2011] new titles: title list has {} rows", g_list.rows.size());
}

namespace svr2011 {
// Each frame: the save file changed: the holders beside it.
void NewTitlesTick(uint8_t* base) {
  if (!g_new.empty()) {
    static uint32_t frames = 0;
    static fs::file_time_type seen{};
    if (++frames % 30 == 0) {
      std::error_code ec;
      const fs::path dir = SuperstarModsSavesDir();
      const fs::file_time_type now = dir.empty() ? fs::file_time_type{} : fs::last_write_time(dir / "SaveData.dat", ec);
      if (!ec && now != fs::file_time_type{}) {
        if (seen != fs::file_time_type{} && now != seen) {
          NewTitlesForSave();
          REXLOG_INFO("[svr2011] new titles: the save written - the holders beside it");
        }
        seen = now;
      }
    }
  }
}
}  // namespace svr2011

// The list's cursor moved (the game calls it with the new row, then sets the holder's picture from the row's title): past
// an end the rows scroll first.
REX_HOOK_RAW(sub_8244CF50) {
  const uint32_t list = ctx.r3.u32 + 228;
  if (!g_new.empty() && list == g_list.self && !g_list.rows.empty()) {
    const int cursor = int(Rd(base, list - 200)), bottom = kListRows - 1, most = int(g_list.rows.size()) - kListRows;
    const int last = g_list.last_cursor;
    int to = cursor;
    if (last == bottom && cursor == 0) {  // (down past the last row)
      if (g_list.top < most) ShowFrom(base, g_list.top + 1), to = bottom;
      else ShowFrom(base, 0);
    } else if (last == 0 && cursor == bottom) {  // (up past the first)
      if (g_list.top > 0) ShowFrom(base, g_list.top - 1), to = 0;
      else ShowFrom(base, most);
    }
    if (to != cursor) Wr(base, list - 200, uint32_t(to));
    g_list.last_cursor = to;
  }
  __imp__sub_8244CF50(ctx, base);
}

// Is a superstar's title pick allowed on the select screen: (select, person) -> 0 / 1. The game's marks the titles in
// use in a stack list of 14; with a new title in the picks, the same rules here.
REX_HOOK_RAW(sub_82732D68) {
  const uint32_t self = ctx.r3.u32, person = ctx.r4.u32;
  if (g_new.empty()) {
    __imp__sub_82732D68(ctx, base);
    return;
  }
  const uint32_t side = Rd(base, self + 13520) * 1352 + self;
  const uint32_t n = Rd(base, side + 88);
  const auto row = [&](uint32_t p) { return side + p * 204 + 120; };
  bool any_new = false;
  for (uint32_t p = 0; p < n && p < 32; ++p) {
    const uint32_t t = Rd(base, row(p) + 40);
    if (t != 255 && t >= uint32_t(kStockTitles)) any_new = true;
  }
  const uint32_t mine = Rd(base, row(person) + 40), team = Rd(base, row(person) + 12);
  if (!any_new || mine == 255) {
    __imp__sub_82732D68(ctx, base);
    return;
  }
  const auto saved = ctx;
  const auto is_tag = [&](uint32_t t) {
    ctx.r3.u64 = t;
    sub_828BD508(ctx, base);
    return ctx.r3.u32 != 0;
  };
  const auto holder = [&](uint32_t t, uint32_t slot) {
    ctx.r3.u64 = t, ctx.r4.u64 = slot, ctx.r5.u64 = 1;
    sub_828BD548(ctx, base);
    return ctx.r3.u32;
  };
  const bool tag = is_tag(mine);
  std::vector<uint32_t> used = {mine};
  uint32_t ok = 1;
  for (uint32_t p = 0; p < n && p < 32 && ok; ++p) {
    if (p == person) continue;
    const uint32_t r = row(p), id = Rd(base, r);
    if (Rd(base, r + 80)) {
      ctx.r3.u64 = id, ctx.r4.u64 = mine, ctx.r5.u64 = Rd(base, self + 14340);
      sub_828BD9D0(ctx, base);
      if (!ctx.r3.u32) {
        ok = 0;
        break;
      }
      if (tag && Rd(base, r + 4) == 1 && Rd(base, r + 12) == team && Rd(base, r + 40) == 255 && holder(mine, 0) != id && holder(mine, 1) != id) {
        ok = 0;
        break;
      }
    }
    if (!Rd(base, r + 120)) continue;
    const uint32_t t = Rd(base, r + 40);
    if (t == 255) continue;
    if (std::find(used.begin(), used.end(), t) == used.end()) used.push_back(t);
    if (tag ? (t == mine && Rd(base, r + 12) != team) : t == mine) ok = 0;
  }
  ctx = saved;
  ctx.r3.u64 = ok && used.size() <= 2 ? 1 : 0;
}

// The match's title is set: (select, title, 1).
REX_HOOK_RAW(sub_82735910) {
  if (const NewTitle* t = Row(ctx.r4.s32)) REXLOG_INFO("[svr2011] new titles: the match is for {}", t->folder);
  __imp__sub_82735910(ctx, base);
}

// A holder, read from the save: (title, slot).
REX_HOOK_RAW(sub_8243B0A8) {
  if (const NewTitle* t = Row(ctx.r3.s32)) {
    ctx.r3.u64 = t->holder[ctx.r4.u32 & 1];
    return;
  }
  __imp__sub_8243B0A8(ctx, base);
}

namespace {


// Test aid SVR2011_TEST_NEW_TITLES: the game's table in the log; =<id>: that superstar given the first new title
// through the game's own accessor, read back through the others.
void TestCheck(PPCContext& ctx, uint8_t* base) {
  const auto saved = ctx;
  const auto call = [&](void (*fn)(PPCContext&, uint8_t*), uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    ctx.r3.u64 = a, ctx.r4.u64 = b, ctx.r5.u64 = c;
    fn(ctx, base);
    return ctx.r3.u32;
  };
  for (int i = 0; i < kStockTitles; ++i) {
    const uint8_t* r = base + kTable + i * 16;
    REXLOG_INFO("[svr2011] new titles test: table {}: a {} b {} c {} kind {} order {} | holders {} {} in play {}", i, Be32(r), Be32(r + 4), Be32(r + 8),
                r[12], int(int8_t(r[13])), call(sub_828BD548, uint32_t(i), 0, 0), call(sub_828BD548, uint32_t(i), 1, 0), call(sub_828C22E0, uint32_t(i)));
  }
  if (!g_new.empty()) {
    const char* v = std::getenv("SVR2011_TEST_NEW_TITLES");
    const uint32_t id = uint32_t(std::strtoul(v, nullptr, 10));
    if (id) {
      ctx.r3.u64 = kStockTitles, ctx.r4.u64 = id, ctx.r5.u64 = 0;
      sub_828BD5B8(ctx, base);
    }
    const uint32_t h = call(sub_828BD548, kStockTitles, 0, 1);
    const uint32_t h2 = call(sub_8243B0A8, kStockTitles, 0);
    REXLOG_INFO("[svr2011] new titles test: title {} holder {} / {} tag {} in play {}", kStockTitles, h, h2, call(sub_828BD508, kStockTitles),
                call(sub_828C22E0, kStockTitles));
  }
  ctx = saved;
}

}  // namespace
