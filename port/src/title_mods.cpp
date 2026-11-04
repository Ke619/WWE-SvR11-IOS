// WWE SmackDown vs. Raw 2011 - title mods (title_mods.h).

#include "title_mods.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "generated/default/svr2011_init.h"
#include "no_mods.h"
#include "text_guard.h"
#include "title_new.h"

namespace fs = std::filesystem;

REX_EXTERN(__imp__sub_82153EF8);

namespace {

// The string ids that name each title (English string.pac groups XX00, by text: whole = the name alone,
// phrase = the name inside other text). The first whole id gives the title's name in the language in use. Then
// the holder's form, "<title> Champion" (Universe rankings): its whole ids and phrases, and its English text
// (the stock name swapped in a phrase when no whole id gives the language's own).
struct TitleStrings {
  const char* key;
  std::vector<uint32_t> whole, phrase;
  std::vector<uint32_t> champ_whole, champ_phrase;
  const char* champion;
};
const TitleStrings kTitleStrings[] = {
    {"IC", {0x17A, 0x39C, 0x307B, 0x6A1B, 0x6A26, 0x9E14}, {0x6A2F}, {0x6ABC}, {}, "Intercontinental Champion"},
    {"US", {0x17B, 0x39D, 0x307C, 0x6A18, 0x6A23, 0x9E15}, {0x4016, 0x6A2C}, {0x6AB9}, {}, "United States Champion"},
    {"WWE", {0x173, 0x398, 0x6A17, 0x6A22, 0x6A36}, {0x3FD8, 0x400F, 0x420B, 0x6A2B}, {0x6AB8}, {}, "WWE Champion"},
    {"WHC", {0x174, 0x399, 0x3079, 0x6A1A, 0x6A25, 0x9E11}, {0x6A2E}, {0x6ABB}, {}, "World Heavyweight Champion"},
    {"WWE_TAG", {0x176, 0x177}, {0x39A, 0x39B, 0x69C8, 0x6A1D, 0xC5AE, 0xC7A6, 0xC7A7, 0xC7C6, 0xC90C}, {},
     {0x6ABE, 0xC846}, "WWE Tag Team Champion"},
    {"WORLD_TAG", {0x178, 0x179, 0x9E13}, {}, {}, {}, "World Tag Team Champion"},
    {"ECW", {0x175, 0x3A0, 0x6A1E, 0x6A32, 0x9E1A}, {0xC908}, {}, {}, "ECW Champion"},
    {"DIVAS", {0x309, 0x39F, 0x3082, 0x6A1C, 0x6A27, 0x6A3B, 0xA023}, {0x6A30}, {0x6ABD}, {}, "Divas Champion"},
    {"WOMENS", {0x17D, 0x39E, 0x307E, 0x6A19, 0x6A24, 0x6A38, 0x9E17}, {0x6A2D}, {0x6ABA}, {}, "Women's Champion"},
    {"HARDCORE", {0x308, 0x3A1, 0x307F, 0x6A20, 0x6A34, 0x9E19}, {0xC90B, 0xCB69}, {}, {}, "Hardcore Champion"},
    {"CRUISER", {0x3A3, 0x307A, 0x6A21, 0x6A35, 0x9E16}, {0xC90A, 0xCB69, 0xCF19}, {}, {}, "Cruiserweight Champion"},
    {"MILLION", {0x3A4, 0x9E2D}, {0xCB69}, {}, {}, "Million Dollar Champion"},
    {"WCW", {0x193, 0x3A2, 0x3080, 0x6A1F, 0x6A33, 0x9E1C}, {0xC909}, {}, {}, "WCW Champion"},
};

struct TitleMod {
  fs::path folder;
  const TitleStrings* title = nullptr;
  std::string name;      // UTF-8, as the game draws it
  std::string champion;  // the holder's form (champion=, else the name less its "ship")
};

rex::memory::Memory* g_memory = nullptr;
std::vector<TitleMod> g_mods;                          // (one per title: the first by folder name)
std::map<uint32_t, std::vector<const TitleMod*>> g_by_id;  // string id -> the mods naming it
std::mutex g_mutex;
std::map<std::string, uint32_t> g_guest;  // text -> its guest copy (made once)

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool AllUpper(const std::string& s) {
  bool letter = false;
  for (unsigned char c : s) {
    if (c >= 'a' && c <= 'z') return false;
    letter |= c >= 'A' && c <= 'Z';
  }
  return letter;
}

// The game's own text for a string id (the lookup itself), "" when none.
std::string GameText(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t id) {
  const auto saved = ctx;
  ctx.r3.u64 = manager;
  ctx.r4.u64 = id;
  __imp__sub_82153EF8(ctx, base);
  std::string s = ctx.r3.u32 ? std::string(reinterpret_cast<const char*>(base + ctx.r3.u32)) : std::string();
  ctx = saved;
  return s;
}

// `text` with the title's name (`stock`, any case) swapped for `name` - in capitals where the game wrote it so.
std::string Swap(const std::string& text, const std::string& stock, const std::string& name) {
  if (stock.empty()) return text;
  const std::string up = Upper(text), key = Upper(stock);
  std::string out;
  size_t at = 0;
  for (size_t f; (f = up.find(key, at)) != std::string::npos; at = f + key.size())
    out += text.substr(at, f - at) + (AllUpper(text.substr(f, key.size())) ? Upper(name) : name);
  return out + text.substr(at);
}

uint32_t Guest(const std::string& text) {
  std::lock_guard lock(g_mutex);
  if (const auto it = g_guest.find(text); it != g_guest.end()) return it->second;
  const uint32_t n = uint32_t(text.size() + 1);
  const uint32_t at = g_memory ? g_memory->SystemHeapAlloc(n) : 0;
  if (at) std::memcpy(g_memory->TranslateVirtual<char*>(at), text.c_str(), n);
  g_guest[text] = at;
  return at;
}

std::string Value(const std::string& text, const char* key) {
  const std::string k = std::string(key) + "=";
  size_t at = 0;
  for (size_t nl; at < text.size(); at = nl + 1) {
    nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    std::string l = text.substr(at, nl - at);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.rfind(k, 0) == 0) return l.substr(k.size());
  }
  return {};
}

}  // namespace

namespace svr2011 {

namespace {
std::once_flag g_load_once;
// Mods\Titles read once - by the start or the texture packs, whichever asks first.
void Load() {
  if (NoMods()) return;  // (Start Without Mods: no_mods.h)
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "Titles";
  std::error_code ec;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec) && e.path().filename().string()[0] != '.' && !fs::exists(e.path() / "disabled", ec) &&
        fs::exists(e.path() / "manifest.txt", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  for (const fs::path& f : folders) {
    std::ifstream in(f / "manifest.txt", std::ios::binary);
    const std::string man((std::istreambuf_iterator<char>(in)), {});
    if (Value(man, "new") == "1") continue;  // (a new title, not a replaced one: title_new.cpp)
    const std::string key = Upper(Value(man, "title"));
    const TitleStrings* t = nullptr;
    for (const auto& k : kTitleStrings)
      if (key == k.key) t = &k;
    if (Value(man, "type") != "title" || !t) {
      REXLOG_WARN("[svr2011] title mods: {}: not a title mod (type=title, title=<one of the 12 keys>)", f.filename().string());
      continue;
    }
    if (std::any_of(g_mods.begin(), g_mods.end(), [&](const TitleMod& m) { return m.title == t; })) {
      REXLOG_WARN("[svr2011] title mods: {}: {} is replaced by an earlier mod already - left out", f.filename().string(), key);
      continue;
    }
    TitleMod m;
    m.folder = f, m.title = t, m.name = CleanUtf8(Value(man, "name"), 120);
    m.champion = CleanUtf8(Value(man, "champion"), 120);
    if (m.champion.empty() && !m.name.empty()) {  // ("SvR PC Championship" -> "SvR PC Champion")
      const std::string up = Upper(m.name);
      m.champion = up.size() > 4 && up.compare(up.size() - 4, 4, "SHIP") == 0 ? m.name.substr(0, m.name.size() - 4)
                                                                             : m.name + " Champion";
    }
    g_mods.push_back(m);
  }
  for (const TitleMod& m : g_mods) {
    if (m.name.empty()) continue;  // (a belt or pictures alone: the game's name kept)
    for (uint32_t id : m.title->whole) g_by_id[id].push_back(&m);
    for (uint32_t id : m.title->phrase) g_by_id[id].push_back(&m);
    for (uint32_t id : m.title->champ_whole) g_by_id[id].push_back(&m);
    for (uint32_t id : m.title->champ_phrase) g_by_id[id].push_back(&m);
  }
  for (const TitleMod& m : g_mods)
    REXLOG_INFO("[svr2011] title mods: {} replaces {}{}", m.folder.filename().string(), m.title->key,
                m.name.empty() ? std::string(" (belt / pictures only)") : " as \"" + m.name + "\"");
}
}  // namespace

void InstallTitleMods(rex::memory::Memory* memory) {
  g_memory = memory;
  std::call_once(g_load_once, Load);
  InstallNewTitles(memory);
}

uint32_t TitleModString(PPCContext& ctx, uint8_t* base) {
  const auto it = g_by_id.find(ctx.r4.u32);
  if (it == g_by_id.end()) return 0;
  const uint32_t manager = ctx.r3.u32, id = ctx.r4.u32;
  std::string text = GameText(ctx, base, manager, id);
  if (text.empty()) return 0;
  const auto has = [&](const std::vector<uint32_t>& v) { return std::find(v.begin(), v.end(), id) != v.end(); };
  for (const TitleMod* m : it->second) {
    const TitleStrings& t = *m->title;
    if (has(t.whole)) {
      text = AllUpper(text) ? Upper(m->name) : m->name;  // (the name alone: as the game cased it)
    } else if (has(t.phrase)) {
      // (a phrase: the title's name in the language in use - its first whole string - swapped)
      text = Swap(text, GameText(ctx, base, manager, t.whole.front()), m->name);
    } else if (has(t.champ_whole)) {
      text = AllUpper(text) ? Upper(m->champion) : m->champion;  // ("INTERCONTINENTAL CHAMPION": the holder)
    } else {
      // (the holder in a phrase: the language's own form when it has one, else the English)
      const std::string stock = t.champ_whole.empty() ? t.champion : GameText(ctx, base, manager, t.champ_whole.front());
      text = Swap(text, stock, m->champion);
    }
  }
  return Guest(text);
}

std::vector<fs::path> TitleModTextureFolders() {
  std::call_once(g_load_once, Load);
  std::vector<fs::path> out;
  std::error_code ec;
  for (const TitleMod& m : g_mods)
    if (fs::is_directory(m.folder / "textures", ec)) out.push_back(m.folder / "textures");
  return out;
}

}  // namespace svr2011
