// USER MOVIES - see user_movies.h.
//
// The game names entrance movies by number (movies\titantron\<id>.bik,
// sub_828B52D8(id) builds the path). The CREATE AN ENTRANCE movie list is
// category 0 of the list tables sub_8287DFD0 turns into menu rows (records
// {value = movie id, name string, unlock}); 254 is HIGHLIGHT REEL, 999 NONE.
// The game uses ids up to 518 and 901-999; user movies get 700-899, kept in
// Custom Movies\ids.txt ("<id><TAB><file>") so a saved entrance keeps its
// movie when other movies are added or removed.
#include "user_movies.h"

#include "hd_movies.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "ascii_fold.h"  // (launcher/)
#include "cae_mod_motions.h"

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr int kFirstId = 700, kLastId = 899;
constexpr int kNoneId = 999;
constexpr uint32_t kSubListRow = 254;  // the movie list row that opens the sub-list
constexpr uint32_t kMovieCategory = 0;
constexpr uint32_t kListItemSize = 24;  // {value, flags, unlock, text[12]}

rex::memory::Memory* g_memory = nullptr;
std::filesystem::path g_folder;
std::mutex g_mutex;
std::map<int, std::string> g_ids;             // id -> file name (ASCII)
std::set<std::string> g_pending;              // files being written (ReserveUserMovie)
std::map<std::string, uint32_t> g_labels;     // label -> guest string

using svrfmt::Rd32, svrfmt::Wr32;
int16_t Rd16(uint8_t* base, uint32_t a) { return int16_t(base[a] << 8 | base[a + 1]); }

// A path for the log (UTF-8).
std::string Utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

bool Ascii(const std::wstring& s) {
  return std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= 32 && c < 127; });
}

// The same bytes (a copy).
bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::error_code ec;
  if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec) return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  std::vector<char> x(65536), y(65536);
  while (fa && fb) {
    fa.read(x.data(), x.size()), fb.read(y.data(), y.size());
    if (fa.gcount() != fb.gcount() || std::memcmp(x.data(), y.data(), size_t(fa.gcount()))) return false;
  }
  return true;
}

// Rescans the folder: ids for new .bik files, drops files that are gone.
void Refresh() {
  std::map<int, std::string> ids;
  {
    std::ifstream in(g_folder / "ids.txt");
    std::string line;
    while (std::getline(in, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      const int id = std::atoi(line.substr(0, tab).c_str());
      if (id >= kFirstId && id <= kLastId) ids[id] = line.substr(tab + 1);
    }
  }
  std::vector<std::string> files;
  std::error_code ec;
  // (an .mp4 - the launcher's movie maker, movie_shrink.py: an HD movie under "<name>.bik", its
  // stub - or a real .bik of that name, the fallback where it doesn't decode; hd_movies.h)
  for (const auto& e : std::filesystem::directory_iterator(g_folder, ec)) {
    std::wstring ext = e.path().extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".mp4" || !e.is_regular_file(ec) || !Ascii(e.path().filename().wstring())) continue;
    const std::string bik = e.path().stem().string() + ".bik";
    static std::set<std::string> no_decoder;  // (probed once per run)
    if (no_decoder.count(bik)) continue;
    // (a mod's own .mp4 under that name - prepared first, superstar / media mods - plays, not this one: the
    // launcher's Shrink movies made "<name>.mp4" here from the game's copy of the mod's .bik - its fallback, or its
    // HD stub: a black movie - and this scan took the mod's tron over for good)
    if (const auto mp4 = svr2011::HdMovieMp4(bik); !mp4.empty() && !std::filesystem::equivalent(mp4.parent_path(), g_folder, ec)) {
      static std::set<std::string> told;
      if (told.insert(bik).second)
        REXLOG_INFO("user movies: {} left out - a mod's own movie plays under that name ({})", e.path().filename().string(),
                    mp4.string());
      continue;
    }
    if (!svr2011::PrepareHdMovie(g_folder, bik, e.path())) no_decoder.insert(bik);
  }
  for (const auto& e : std::filesystem::directory_iterator(g_folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const auto p = e.path();
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".bik") continue;
    std::wstring name = p.filename().wstring();
    if (!Ascii(name)) {
      // (a name with accents - an older launcher, the phone's, or the player's
      // own: renamed to plain letters, "Peña.bik" -> "Pena.bik"; the game's
      // paths and menus are 8-bit)
      std::wstring stem;
      for (wchar_t c : p.stem().wstring()) {
        const wchar_t f = ascii_fold_char(c);
        if (f >= 32 && f < 127) stem += f;
      }
      const bool usable = stem.find_first_not_of(L" ._-") != std::wstring::npos;
      const std::wstring plain = stem + L".bik";
      const auto to = g_folder / plain;
      // (an older copy of a superstar mod's movie under its accented name, the plain-letters one there now: dropped)
      if (usable && std::filesystem::exists(to, ec) && SameFile(to, p) && std::filesystem::remove(p, ec)) {
        REXLOG_INFO("user movies: removed {} (the same as {})", Utf8(p.filename()), to.filename().string());
        continue;
      }
      ec.clear();
      if (!usable || std::filesystem::exists(to, ec) || (std::filesystem::rename(p, to, ec), ec)) {
        REXLOG_WARN("user movies: skipping {} (use plain letters in the name)", Utf8(p.filename()));
        ec.clear();
        continue;
      }
      REXLOG_INFO("user movies: renamed {} to {}", Utf8(p.filename()), to.filename().string());
      name = plain;
    }
    files.push_back(std::filesystem::path(name).string());
  }
  for (auto it = ids.begin(); it != ids.end();) {
    const bool there = std::find(files.begin(), files.end(), it->second) != files.end() ||
                       g_pending.count(it->second) ||
                       std::filesystem::exists(g_folder / (it->second + ".part"), ec);  // (being written)
    it = there ? std::next(it) : ids.erase(it);
  }
  std::sort(files.begin(), files.end());
  bool changed = false;
  for (const auto& f : files) {
    const bool known = std::any_of(ids.begin(), ids.end(), [&](const auto& kv) { return kv.second == f; });
    if (known) continue;
    for (int id = kFirstId; id <= kLastId; ++id) {
      if (!ids.count(id)) {
        ids[id] = f;
        changed = true;
        break;
      }
    }
  }
  if (changed || ids.size() != g_ids.size()) {
    std::ofstream out(g_folder / "ids.txt", std::ios::trunc);
    for (const auto& [id, f] : ids) out << id << '\t' << f << '\n';
  }
  g_ids = std::move(ids);
}

// A guest string that lives for the whole run (menu rows keep the pointer).
uint32_t Text(const std::string& label) {
  if (auto it = g_labels.find(label); it != g_labels.end()) return it->second;
  const uint32_t s = g_memory->SystemHeapAlloc(uint32_t(label.size() + 1));
  if (!s) return 0;
  std::memcpy(g_memory->TranslateVirtual<char*>(s), label.c_str(), label.size() + 1);
  g_labels[label] = s;
  return s;
}

// The menu label of a movie: its file name without .bik, upper case.
uint32_t Label(const std::string& file) {
  std::string label = std::filesystem::path(file).stem().string();
  std::transform(label.begin(), label.end(), label.begin(), ::toupper);
  if (label.size() > 40) label.resize(40);
  return Text(label);
}

}  // namespace

namespace svr2011 {

std::filesystem::path UserMoviesFolder() { return g_folder; }

std::filesystem::path UserMovieFile(int id) {
  std::lock_guard lock(g_mutex);
  auto it = g_ids.find(id);
  return it == g_ids.end() ? std::filesystem::path() : g_folder / it->second;
}

int ReserveUserMovie(const std::string& file) {
  std::lock_guard lock(g_mutex);
  if (g_folder.empty()) return 0;
  g_pending.insert(file);
  Refresh();
  for (const auto& [id, f] : g_ids) {
    if (f == file) return id;
  }
  for (int id = kFirstId; id <= kLastId; ++id) {
    if (!g_ids.count(id)) {
      g_ids[id] = file;
      std::ofstream out(g_folder / "ids.txt", std::ios::trunc);
      for (const auto& [i, f] : g_ids) out << i << '\t' << f << '\n';
      return id;
    }
  }
  return 0;
}

void FinishUserMovie(const std::string& file) {
  std::lock_guard lock(g_mutex);
  g_pending.erase(file);
  Refresh();
}

void InstallUserMovies(rex::memory::Memory* memory, const std::filesystem::path& folder) {
  g_memory = memory;
  g_folder = folder;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::lock_guard lock(g_mutex);
  Refresh();
  REXLOG_INFO("user movies: {} in {}", g_ids.size(), Utf8(folder));
}

}  // namespace svr2011

// CREATE AN ENTRANCE -> FINALIZE -> MOVIE. Its first row (value 254,
// HIGHLIGHT REEL) opens a sub-list: the list right after the movie list,
// which sub_8287CFB0 fills with the 20 highlight reel slots; picking one
// keeps 254 in the movie list and the pick in the sub-list (like MUSIC ->
// USER PLAYLIST). That row becomes USER MOVIES and its sub-list holds the
// user movies, then the saved highlight reels. The entrance then plays the
// picked id (sub_828B52D8 below).

// The movie list (sub_8287DFD0(list, r4, table, category, base)): the 254
// row is named USER MOVIES.
REX_EXTERN(__imp__sub_8287DFD0);
REX_HOOK_RAW(sub_8287DFD0) {
  const uint32_t list = ctx.r3.u32, table = ctx.r5.u32, category = ctx.r6.u32;
  bool movies = false;
  if (category == kMovieCategory && g_memory) {
    const uint32_t items = Rd32(base, table + (category + 30) * 4);
    movies = items && Rd16(base, table + (category + 96) * 2) > 1 && Rd16(base, items + 6) == kNoneId;
  }
  __imp__sub_8287DFD0(ctx, base);
  {  // (the other filler of this hook: the MODS rows of the entrance motion lists)
    const auto r3 = ctx.r3;
    svr2011::CaeModRows(ctx, base, list, category);
    ctx.r3 = r3;
  }
  if (!movies) return;
  const uint32_t count = Rd32(base, list), rows_at = Rd32(base, list + 8);
  // Tools: SVR2011_DUMP_MOVIE_NAMES=<file> writes "<id><TAB><name>" for the
  // game's movies (the launcher's list of superstars' movies).
  char* dump = nullptr;
  size_t dump_len = 0;
  if (_dupenv_s(&dump, &dump_len, "SVR2011_DUMP_MOVIE_NAMES") == 0 && dump) {
    if (FILE* f = std::fopen(dump, "w")) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t row = rows_at + i * kListItemSize, s = Rd32(base, row + 12);
        if (s) std::fprintf(f, "%d\t%s\n", int(Rd32(base, row)), reinterpret_cast<const char*>(base + s));
      }
      std::fclose(f);
    }
    free(dump);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t row = rows_at + i * kListItemSize;
    if (Rd32(base, row) != kSubListRow) continue;
    std::lock_guard lock(g_mutex);
    if (const uint32_t s = Text("USER MOVIES")) Wr32(base, row + 12, s);  // {value, flags, unlock, text}
    break;
  }
}

// The sub-list: sub_8287CFB0(reels, list) adds the highlight reel slots
// (1000 + i, empty ones disabled) and returns how many reels are saved (the
// 254 row is disabled without any). Here: the user movies first, then the
// saved reels (empty slots dropped); the count includes the movies.
REX_EXTERN(__imp__sub_8287CFB0);
REX_HOOK_RAW(sub_8287CFB0) {
  const uint32_t list = ctx.r4.u32;
  __imp__sub_8287CFB0(ctx, base);
  if (!g_memory) return;
  const uint32_t reels = ctx.r3.u32;
  std::vector<std::pair<int, uint32_t>> rows;
  {
    std::lock_guard lock(g_mutex);
    Refresh();
    for (const auto& [id, file] : g_ids) {
      if (const uint32_t s = Label(file)) rows.emplace_back(id, s);
    }
  }
  const auto saved = ctx;
  for (const auto& [id, label] : rows) {  // sub_828829D8(list, value, text, 0, 0, unlock)
    ctx.r3.u64 = list;
    ctx.r4.u64 = uint32_t(id);
    ctx.r5.u64 = label;
    ctx.r6.u64 = 0;
    ctx.r7.u64 = 0;
    ctx.r8.u64 = uint64_t(int64_t(-1));
    sub_828829D8(ctx, base);
  }
  ctx = saved;
  // {count, capacity, rows}: [movies][saved reels].
  const uint32_t count = Rd32(base, list), rows_at = Rd32(base, list + 8);
  const size_t n = std::min<size_t>(rows.size(), count);
  std::vector<uint8_t> out;
  const uint8_t* all = base + rows_at;
  out.insert(out.end(), all + (count - n) * kListItemSize, all + count * kListItemSize);
  // (test aid: SVR2011_TEST_FAKE_REELS=1 keeps the empty reel slots, shown as saved - a full reel list)
  static const bool fake_reels = std::getenv("SVR2011_TEST_FAKE_REELS") != nullptr;
  for (uint32_t i = 0; i < count - n; ++i) {
    const uint8_t* row = all + i * kListItemSize;
    if (fake_reels) {
      out.insert(out.end(), row, row + kListItemSize);
      std::memset(&out[out.size() - kListItemSize + 4], 0, 4);
    } else if (Rd32(base, rows_at + i * kListItemSize + 4) == 0) {
      out.insert(out.end(), row, row + kListItemSize);
    }
  }
  std::memcpy(base + rows_at, out.data(), out.size());
  Wr32(base, list, uint32_t(out.size() / kListItemSize));
  ctx.r3.u64 = reels + uint32_t(n);
}

// The sub-list's page: sub_8287FD08(menu) fills the rows of the page shown
// (view = menu+380: page +276, rows a page +32; the widget menu+16) with
// sub_8272C228(widget, page, row, item) and stops at the list's end - the
// highlight reels were always 2 full pages, so the rows after it kept the
// page shown before ("38 39 40" under 47 movies). Here: those rows hidden
// (sub_8272C228 with no item hides a row).
REX_EXTERN(__imp__sub_8287FD08);
REX_HOOK_RAW(sub_8287FD08) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_8287FD08(ctx, base);
  const uint32_t view = Rd32(base, menu + 380);
  if (!view) return;
  auto call = ctx;
  call.r3.u64 = view;
  sub_8287BE70(call, base);  // (the row count, as sub_8287FD08 reads it)
  const int32_t count = call.r3.s32;
  const int32_t per_page = int32_t(Rd32(base, view + 32)), page = int32_t(Rd32(base, view + 276));
  if (per_page <= 0 || per_page > 64 || page < 0) return;
  for (int32_t row = std::max(0, count - page * per_page); row < per_page; ++row) {
    call.r3.u64 = menu + 16;
    call.r4.u64 = uint32_t(page);
    call.r5.u64 = uint32_t(row);
    call.r6.u64 = 0;
    sub_8272C228(call, base);
  }
}

// An entrance's movie -> the CREATE AN ENTRANCE menus (B16). Two loaders put
// a stored entrance into a menu's picks: sub_8287E960(slots, e) (the full
// editor; slots = screen+4288, e = the entrance in the profile +0x1C4 layout,
// the movie id at e+14 = profile +0x1C0 +0x12) and sub_82875320(page) (the
// one-page editor of an existing superstar's entrance: MOTION, CHAMPION and
// MULTI-TITLE ENTRANCE, MOVIE, MUSIC). Both find a highlight reel
// (1000-1019) in the sub-list by value and any other id in the movie list
// (sub_828823E0); a user movie (700-899) isn't there, so they picked row 0
// (USER MOVIES) and the sub-list's row 0 - the first user movie, which
// FINALIZE / SAVE then wrote (the sub-list row's value when the movie row is
// 254): editing an entrance with a user movie (a superstar mod's own, or one
// the player picked) turned its movie into the first one in the list. Here:
// USER MOVIES and the movie's own row in the sub-list (filled before the
// lookup, by sub_8287CFB0); NONE if its file is gone. An empty sub-list (the
// reel storage not ready: sub_8287CFB0 not called) leaves the game's picks.
namespace {

int IndexOf(uint8_t* base, uint32_t list, uint32_t value) {  // (as sub_828823E0)
  if (!list) return -1;
  const uint32_t n = Rd32(base, list), rows = Rd32(base, list + 8);
  for (uint32_t i = 0; i < n && rows; ++i)
    if (Rd32(base, rows + i * kListItemSize) == value) return int(i);
  return -1;
}

// The rows to select for user movie `id`: {movie list row, sub-list row}, or
// false to leave the game's picks.
bool UserMoviePicks(uint8_t* base, uint32_t movies, uint32_t sub, int id, uint32_t* row, uint32_t* pick) {
  const int r = IndexOf(base, movies, kSubListRow), p = IndexOf(base, sub, uint32_t(id));
  if (r >= 0 && p >= 0) {
    *row = uint32_t(r), *pick = uint32_t(p);
    REXLOG_INFO("user movies: entrance movie {} -> USER MOVIES row {}", id, p);
    return true;
  }
  if (!sub || Rd32(base, sub) == 0) return false;
  const int none = IndexOf(base, movies, kNoneId);
  if (none < 0) return false;
  *row = uint32_t(none), *pick = 0;
  REXLOG_INFO("user movies: entrance movie {} has no file - NONE", id);
  return true;
}

// sub_82875320's own lookup of the movie id (its copy of the entrance is on
// its stack): the user movie it missed, and the list it looked in.
thread_local bool t_in_page = false;
thread_local int t_page_movie = -1;

}  // namespace

REX_EXTERN(__imp__sub_8287E960);
REX_HOOK_RAW(sub_8287E960) {
  const uint32_t slots = ctx.r3.u32, e = ctx.r4.u32;
  svr2011::CaeLoadingEntrance(true);  // (the edited mod's own scene parts: "MOD: <name>" - cae_mod_motions.cpp)
  __imp__sub_8287E960(ctx, base);
  svr2011::CaeLoadingEntrance(false);
  if (!g_memory || !slots || !e) return;
  const int id = uint16_t(Rd16(base, e + 14));
  if (id < kFirstId || id > kLastId) return;
  uint32_t row = 0, pick = 0;
  if (UserMoviePicks(base, Rd32(base, slots + 500), Rd32(base, slots + 508), id, &row, &pick)) {
    Wr32(base, slots + 496, row);  // {index, list} picks: the movie row, the sub-list row
    Wr32(base, slots + 504, pick);
  }
}

// sub_82875320(page): the movie list at page+2264, the sub-list at page+2292;
// the picks at page+152 (movie row) / +156 (sub-list row), then copied with
// the others (+136..+156 -> +160..+180, the "unchanged" baseline).
REX_EXTERN(__imp__sub_828823E0);
REX_HOOK_RAW(sub_828823E0) {
  const uint32_t list = ctx.r3.u32, value = ctx.r4.u32;
  __imp__sub_828823E0(ctx, base);
  if (t_in_page && ctx.r3.s32 < 0 && int(value) >= kFirstId && int(value) <= kLastId) t_page_movie = int(value);
  // (an entrance motion list missing the stored number - a removed mod's: a MISSING MOD row keeps it)
  if (ctx.r3.s32 < 0)
    if (const int row = svr2011::CaeMissingRow(ctx, base, list, value); row >= 0) ctx.r3.u64 = uint32_t(row);
  // (a number several rows have: the edited mod's own row)
  if (ctx.r3.s32 >= 0)
    if (const int row = svr2011::CaePreferredRow(ctx, base, list, value); row >= 0) ctx.r3.u64 = uint32_t(row);
}
REX_EXTERN(__imp__sub_82875320);
REX_HOOK_RAW(sub_82875320) {
  const uint32_t page = ctx.r3.u32;
  t_in_page = true;
  t_page_movie = -1;
  __imp__sub_82875320(ctx, base);
  t_in_page = false;
  if (!g_memory || !page || t_page_movie < 0) return;
  uint32_t row = 0, pick = 0;
  if (UserMoviePicks(base, page + 2264, page + 2292, t_page_movie, &row, &pick)) {
    Wr32(base, page + 152, row);
    Wr32(base, page + 156, pick);
    Wr32(base, page + 176, row);
    Wr32(base, page + 180, pick);
  }
}

// The path of entrance movie `id`: sub_828B52D8(id) -> (r3) the path.
REX_EXTERN(__imp__sub_828B52D8);
REX_HOOK_RAW(sub_828B52D8) {
  const int id = ctx.r3.s32;
  __imp__sub_828B52D8(ctx, base);
  // Tests: SVR2011_TEST_MOVIE=<file in the movies folder> plays it for every
  // entrance (e.g. a test pattern, to see what the titantron shows); a full
  // path to an .mp4: that, as an HD movie (hd_movies.h; "hdtest.bik").
  static const std::string test_movie = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "SVR2011_TEST_MOVIE") == 0 && v) {
      s = v;
      free(v);
    }
    if (s.size() > 4 && s.substr(s.size() - 4) == ".mp4") {
      s = svr2011::RegisterHdMovie("hdtest.bik", std::filesystem::path(s)) ? svr2011::HdMovieFile("hdtest.bik") : "";
    }
    return s;
  }();
  if (!test_movie.empty() && g_memory) {
    const std::string path = "umovie:\\" + test_movie;
    std::memcpy(base + ctx.r3.u32, path.c_str(), path.size() + 1);
    return;
  }
  if (id < kFirstId || id > kLastId || !g_memory) return;
  std::string file;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_ids.find(id); it != g_ids.end()) file = it->second;
  }
  // (an HD movie - an .mp4 the platform decodes: its stub, hd_movies.h)
  const std::string path = file.empty() ? "GAME:\\movies\\titantron\\999.bik" : "umovie:\\" + svr2011::HdMovieFile(file);
  REXLOG_INFO("user movies: entrance movie {} plays {}", id, path);
  if (path.size() < 250) std::memcpy(base + ctx.r3.u32, path.c_str(), path.size() + 1);
}

// The layout of entrance movie `id`: sub_828B5250(id) reads a byte table of
// the game's movies (ids 0-999); the arena shows the movie's top on the big
// screen and its bottom strip on the stage when it is 1 (sub_8275FD20), else
// the whole frame on the big screen and the arena's own movie below. User
// movies aren't in the table (0): they get the superstar movies' layout.
REX_EXTERN(__imp__sub_828B5250);
REX_HOOK_RAW(sub_828B5250) {
  const int id = ctx.r3.s32;
  __imp__sub_828B5250(ctx, base);
  static bool logged = false;
  if (!logged && g_memory) {  // (once: the values the game's movies have)
    logged = true;
    std::map<uint32_t, int> counts;
    const uint32_t table = Rd32(base, 0x82EDE960);
    if (table) {
      for (int i = 0; i < 1000; ++i) counts[base[table + i]]++;
      std::string s;
      for (const auto& [v, n] : counts) s += fmt::format(" {}x{}", v, n);
      REXLOG_INFO("user movies: movie layout table values:{} (id {} -> {})", s, id, ctx.r3.u32);
    }
  }
  if (id >= kFirstId && id <= kLastId) ctx.r3.u64 = 1;
  // Tests: SVR2011_TEST_LAYOUT=<value> for every movie.
  static const int test_layout = [] {
    char* v = nullptr;
    size_t n = 0;
    int r = -1;
    if (_dupenv_s(&v, &n, "SVR2011_TEST_LAYOUT") == 0 && v) {
      r = std::atoi(v);
      free(v);
    }
    return r;
  }();
  if (test_layout >= 0) ctx.r3.u64 = uint32_t(test_layout);
}
