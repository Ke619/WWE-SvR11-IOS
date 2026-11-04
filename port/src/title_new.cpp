// WWE SmackDown vs. Raw 2011 - new titles (title_new.h).

#include "title_new.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>

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

namespace {

constexpr int kStockTitles = 14;   // the game's table: 0x82EDEB50, 14 rows x 16 B
constexpr int kMaxNew = 32;        // (title indices travel as signed bytes elsewhere: well under 127)
constexpr uint32_t kNoHolder = 0x200;
constexpr uint32_t kTable = 0x82EDEB50;

struct NewTitle {
  std::string folder, name, champion;
  bool tag = false;
  uint32_t holder[2] = {kNoHolder, kNoHolder};
};
std::vector<NewTitle> g_new;

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

void InstallNewTitles() {
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

// A holder, read from the save: (title, slot).
REX_HOOK_RAW(sub_8243B0A8) {
  if (const NewTitle* t = Row(ctx.r3.s32)) {
    ctx.r3.u64 = t->holder[ctx.r4.u32 & 1];
    return;
  }
  __imp__sub_8243B0A8(ctx, base);
}

namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

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
      svr2011::NewTitlesForSave();
    }
    const uint32_t h = call(sub_828BD548, kStockTitles, 0, 1);
    const uint32_t h2 = call(sub_8243B0A8, kStockTitles, 0);
    REXLOG_INFO("[svr2011] new titles test: title {} holder {} / {} tag {} in play {}", kStockTitles, h, h2, call(sub_828BD508, kStockTitles),
                call(sub_828C22E0, kStockTitles));
  }
  ctx = saved;
}

}  // namespace
