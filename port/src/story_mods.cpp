// WWE SmackDown vs. Raw 2011 - superstar mods in Story Designer's stories.
//
// Story Designer offers the mods in its superstar lists (a scene's cast, a match's people: its sort category MODS,
// sort_filters.cpp, and its other views) and a story names them by id. A story is a file of its own,
// Saves\<NN>ScriptShow.sas (NN = its save slot 00-09, 7,130,260 bytes; SaveData.dat doesn't hold it), laid out as the
// game holds it in memory (sub_82628FE0 walks it for "contains downloadable content"):
// - 500 moments of 4056 bytes from +0x4E98; before each: +-8 its type (0 match, 1 scene), +-6 u16 0xFFFF when the
//   slot is empty, +-4 u32 a byte sum of the moment;
// - a scene's cast: 6 x 8 bytes from +16, a u16 id first (sub_82628D90); a match's people: 30 x 12 bytes from +4 and
//   2 more from +368, a u16 id first (sub_82628CA0); 512 = nobody;
// - the 15 Created Superstars it carries after that, from +0x280000;
// - the last 4 bytes a u32 byte sum of the file.
// A mod's id is only its id while that mod is installed - one removed leaves its id to the next new mod, and with
// big_roster a mod past the pool ids gets the next free one. So each story's mod fields are noted after the game
// writes it - Saves\.universe\story_mods.txt, "<file> <field offset> <mod folder> <id written> <stand-in>" a line -
// and at the next start a field whose mod has another id now gets that id, and one whose mod isn't installed its
// stand-in (the mod's base superstar); a mod that comes back gets its field back. Both sums follow each change, as
// the game's own edit does (a cast change moved the moment's and the file's sum by the same amount). The story file
// keeps plain ids: a game without this (2.1.9) reads it as ever.

#include "story_mods.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kStorySize = 7130260;
constexpr uint32_t kMoments = 0x4E98, kMomentSize = 4056, kMomentCount = 500;
constexpr uint32_t kNobody = 512;

uint16_t Rd16(const svrfmt::Bytes& d, size_t o) { return uint16_t(d[o] << 8 | d[o + 1]); }
uint32_t Rd32(const svrfmt::Bytes& d, size_t o) { return uint32_t(d[o]) << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]; }
void Wr32(svrfmt::Bytes& d, size_t o, uint32_t v) {
  d[o] = uint8_t(v >> 24), d[o + 1] = uint8_t(v >> 16), d[o + 2] = uint8_t(v >> 8), d[o + 3] = uint8_t(v);
}

// Every superstar id field of the story's moments: (field offset, its moment's sum offset).
std::vector<std::pair<uint32_t, uint32_t>> IdFields(const svrfmt::Bytes& d) {
  std::vector<std::pair<uint32_t, uint32_t>> f;
  for (uint32_t i = 0; i < kMomentCount; ++i) {
    const uint32_t p = kMoments + i * kMomentSize;
    if (p + kMomentSize > d.size()) break;
    if (Rd16(d, p - 6) == 0xFFFF) continue;
    const uint8_t type = d[p - 8];
    if (type == 1)
      for (uint32_t k = 0; k < 6; ++k) f.push_back({p + 16 + k * 8, p - 4});
    else if (type == 0) {
      for (uint32_t k = 0; k < 30; ++k) f.push_back({p + 4 + k * 12, p - 4});
      for (uint32_t k = 0; k < 2; ++k) f.push_back({p + 368 + k * 12, p - 4});
    }
  }
  return f;
}

struct Note {
  uint32_t off;
  std::string folder;
  uint16_t id, stub;
};
using Notes = std::map<std::string, std::vector<Note>>;  // story file name -> its mod fields

fs::path NotesFile(const fs::path& saves) { return saves / ".universe" / "story_mods.txt"; }

Notes ReadNotes(const fs::path& saves) {
  Notes n;
  if (FILE* t = std::fopen(NotesFile(saves).string().c_str(), "rb")) {
    char line[700];
    while (std::fgets(line, sizeof line, t)) {
      char file[64] = {}, folder[512] = {};
      unsigned off = 0, id = 0, stub = 0;
      if (std::sscanf(line, "%63[^\t]\t%x\t%511[^\t]\t%u\t%u", file, &off, folder, &id, &stub) == 5)
        n[file].push_back({off, folder, uint16_t(id), uint16_t(stub)});
    }
    std::fclose(t);
  }
  return n;
}

void WriteNotes(const fs::path& saves, const Notes& n) {
  std::error_code ec;
  fs::create_directories(saves / ".universe", ec);
  const fs::path to = NotesFile(saves), part = fs::path(to).concat(".tmp");
  if (FILE* t = std::fopen(part.string().c_str(), "wb")) {
    for (const auto& [file, notes] : n)
      for (const Note& x : notes) std::fprintf(t, "%s\t%X\t%s\t%u\t%u\n", file.c_str(), x.off, x.folder.c_str(), x.id, x.stub);
    std::fclose(t);
    fs::rename(part, to, ec);
  }
}

bool IsStory(const fs::directory_entry& e) {
  const std::string n = e.path().filename().string();
  return n.size() == 16 && n.compare(2, 14, "ScriptShow.sas") == 0 && std::isdigit(uint8_t(n[0])) &&
         std::isdigit(uint8_t(n[1]));
}

// A story the game wrote: its mod fields noted (a field noted before that holds its stand-in still - a mod not
// installed this run - stays noted).
std::vector<Note> NoteStory(const svrfmt::Bytes& d, const std::vector<Note>& before) {
  std::vector<Note> notes;
  for (const auto& [off, sum] : IdFields(d)) {
    const uint32_t id = Rd16(d, off);
    if (id && id < kNobody && svr2011::IsSuperstarMod(id) && !svr2011::IsOnlineTempSuperstar(id)) {
      const uint32_t b = svr2011::SuperstarModBase(id);
      const uint16_t stub = uint16_t(b && b < 322 && !svr2011::IsSuperstarMod(b) ? b : id);
      notes.push_back({off, svr2011::SuperstarModFolder(id), uint16_t(id), stub});
      continue;
    }
    for (const Note& x : before)
      if (x.off == off && x.id == id && x.id == x.stub && !svr2011::SuperstarModIdOf(x.folder)) notes.push_back(x);
  }
  return notes;
}

// At the start: each noted field still holding what was written gets its mod's id this run, else the stand-in.
// Returns the fields changed (the file rewritten, both sums following).
int PutRight(const fs::path& file, svrfmt::Bytes& d, std::vector<Note>& notes) {
  std::map<uint32_t, uint32_t> sum_of;
  for (const auto& [off, sum] : IdFields(d)) sum_of[off] = sum;
  int changed = 0, stale = 0;
  int64_t file_delta = 0;
  for (Note& x : notes) {
    const auto s = sum_of.find(x.off);
    if (s == sum_of.end() || Rd16(d, x.off) != x.id) {
      ++stale;  // (the story was written without this note - another copy of it)
      continue;
    }
    const uint32_t now = svr2011::SuperstarModIdOf(x.folder);
    const uint16_t want = uint16_t(now ? now : x.stub);
    if (want == x.id) continue;
    const int delta = (want >> 8) + (want & 0xFF) - (x.id >> 8) - (x.id & 0xFF);
    d[x.off] = uint8_t(want >> 8), d[x.off + 1] = uint8_t(want);
    Wr32(d, s->second, Rd32(d, s->second) + uint32_t(delta));
    file_delta += delta;
    REXLOG_INFO("[svr2011] story mods: {} +{:X}: {} {} -> {}{}", file.filename().string(), x.off, x.folder, x.id, want,
                now ? "" : " (not installed: its stand-in)");
    x.id = want;
    ++changed;
  }
  if (stale) REXLOG_INFO("[svr2011] story mods: {}: {} noted field(s) no longer as written - left", file.filename().string(), stale);
  if (!changed) return 0;
  Wr32(d, d.size() - 4, Rd32(d, d.size() - 4) + uint32_t(int32_t(file_delta)));
  std::error_code ec;
  const fs::path keep = file.parent_path() / ".universe" / (file.filename().string() + ".before");
  fs::copy_file(file, keep, fs::copy_options::overwrite_existing, ec);  // (the story as it was, once per change)
  const fs::path part = fs::path(file).concat(".tmp");
  if (FILE* o = std::fopen(part.string().c_str(), "wb")) {
    const bool ok = std::fwrite(d.data(), 1, d.size(), o) == d.size();
    std::fclose(o);
    if (ok) fs::rename(part, file, ec);
    if (ok && !ec) return changed;
  }
  REXLOG_WARN("[svr2011] story mods: {} could not be written", file.string());
  return -1;
}

std::map<std::string, fs::file_time_type> g_seen;  // story file -> its write time last noted / put right
bool g_started = false;
uint32_t g_frames = 0;

}  // namespace

namespace svr2011 {

void StoryModsTick() {
  if (g_started && ++g_frames % 60) return;
  const fs::path saves = SuperstarModsSavesDir();
  std::error_code ec;
  if (saves.empty() || !fs::is_directory(saves, ec)) return;
  Notes notes = ReadNotes(saves);
  bool changed = false;
  for (const auto& e : fs::directory_iterator(saves, ec)) {
    if (!e.is_regular_file(ec) || !IsStory(e)) continue;
    const std::string name = e.path().filename().string();
    const auto t = fs::last_write_time(e.path(), ec);
    if (ec || (g_started && g_seen.count(name) && g_seen[name] == t)) continue;
    svrfmt::Bytes d;
    if (!svrfmt::ReadFile(e.path().string(), d) || d.size() != kStorySize) continue;
    auto& n = notes[name];
    if (!g_started) {  // (the start: what moved or went, put right)
      const int c = PutRight(e.path(), d, n);
      if (c > 0) changed = true;
      g_seen[name] = fs::last_write_time(e.path(), ec);
      continue;
    }
    const std::vector<Note> now = NoteStory(d, n);  // (the game wrote it: its mod fields)
    g_seen[name] = t;
    if (now.size() == n.size() && std::equal(now.begin(), now.end(), n.begin(), [](const Note& a, const Note& b) {
          return a.off == b.off && a.folder == b.folder && a.id == b.id && a.stub == b.stub;
        }))
      continue;
    n = now;
    changed = true;
    REXLOG_INFO("[svr2011] story mods: {} written - {} field(s) name a mod", name, n.size());
  }
  if (!g_started) {
    g_started = true;
    // (stories already there that this run never saw noted: noted now, so a mod that moves later is followed)
    for (const auto& e : fs::directory_iterator(saves, ec)) {
      if (!e.is_regular_file(ec) || !IsStory(e)) continue;
      const std::string name = e.path().filename().string();
      if (!notes[name].empty()) continue;
      svrfmt::Bytes d;
      if (!svrfmt::ReadFile(e.path().string(), d) || d.size() != kStorySize) continue;
      notes[name] = NoteStory(d, {});
      if (!notes[name].empty()) {
        changed = true;
        REXLOG_INFO("[svr2011] story mods: {} - {} field(s) name a mod", name, notes[name].size());
      }
    }
  }
  for (auto it = notes.begin(); it != notes.end();) it = it->second.empty() ? notes.erase(it) : std::next(it);
  if (changed) WriteNotes(saves, notes);
}

}  // namespace svr2011
