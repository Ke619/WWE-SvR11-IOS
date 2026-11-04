// Superstar mods in WWE Universe (universe_mods.h; user: "increase the limit - people WILL have DLC and WILL have
// full CAW slots filled", so mods don't borrow created / DLC places).
//
// Universe keeps 150 superstars in the save (P = *(0x82E407B4) + 0x364B4): 24-byte entries at P+16 (0-49 created,
// 50-69 DLC, 70-149 a disc list), a rivalry byte per pair at P+3616 (strict upper triangle: i < j,
// i * (299 - i) / 2 + j - 1), match cards from P+14942 (ids). The accessors are few:
// - sub_825B7660(id) -> place (-1 none); its 11 callers all do *(obj + 4) + 16 + place * 24 inline and test -1 only
//   (nothing stores, bounds or reuses the place);
// - sub_825AD850(id, id) -> rivalry index (-1 none); its 7 callers do base (P + 3616) + index, lbzx / stbx.
// A mod gets a side entry (24 B) and a side row of rivalry bytes: columns 0-149 the places, 150 + j the j-th mod
// (a mod-mod pair lives in the lower mod's row). Its place is a "virtual place" k with P + 16 + 24k (32-bit) = its
// side entry; its pairs return the offset from P + 3616 to the side byte. Every caller works unchanged.
// The table is guest memory (SystemHeapAlloc - the phone's build too) and lives in
// Saves\.universe\mods.bin (a dot folder, as .logos: a file of its own in Saves is taken for a save and gets a
// header), keyed by mod folder: read after a save is loaded, written when the game writes
// SaveData.dat (its time changes), cleared on Universe's reset. A save without the file: cleared.
#include "universe_mods.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "frame_rate.h"
#include "superstar_mods.h"

namespace svr2011 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kSavePtr = 0x82E407B4, kUniverse = 0x364B4, kEntries = 16, kRivalry = 3616, kEntrySize = 24;
// (326: the mod slots, superstar_mods.cpp - 137 pool ids + big_roster's 323-511)
constexpr uint32_t kPlaces = 150, kMax = 326, kRow = kPlaces + kMax;

rex::memory::Memory* g_memory = nullptr;
fs::path g_file;            // Saves\.universe\mods.bin
fs::path g_save;            // Saves\SaveData.dat
uint32_t g_entries = 0;     // guest: kMax x 24 B (+ 8: placed so P + 16 and an entry are 8 apart)
uint32_t g_shift = 0;       // the entries' start in that block (0-7)
uint32_t g_rows = 0;        // guest: kMax x kRow rivalry bytes
std::vector<uint32_t> g_ids;  // the mods, in their side order (SuperstarModIds)
fs::file_time_type g_seen{};  // SaveData.dat's time last read or written
bool g_loaded = false;        // a save was loaded this run (or there was none at start: a new one may come)
bool g_fresh = false;         // Fresh() has looked at the mods' entries since the last clear / read

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

uint32_t P(uint8_t* base) {
  const uint32_t save = Rd32(base + kSavePtr);
  return save ? save + kUniverse : 0;
}

int Index(uint32_t id) {
  for (size_t i = 0; i < g_ids.size(); ++i)
    if (g_ids[i] == id) return int(i);
  return -1;
}

// The entries' start: (start - (P + 16)) a multiple of 8, so 24k can reach it (moved if P moves).
uint32_t Entries(uint8_t* base, uint32_t p) {
  const uint32_t want = (p + kEntries - g_entries) & 7;
  if (want != g_shift) {
    uint8_t* b = base + g_entries;
    std::memmove(b + want, b + g_shift, kMax * kEntrySize);
    g_shift = want;
  }
  return g_entries + g_shift;
}

void Clear() {
  g_fresh = false;
  if (!g_memory || !g_entries) return;
  std::memset(g_memory->TranslateVirtual<uint8_t*>(g_entries), 0, kMax * kEntrySize + 8);
  std::memset(g_memory->TranslateVirtual<uint8_t*>(g_rows), 0, kMax * kRow);
}

// A mod's entry the game never set up (all zero: the Universe was made before the mod) gets what a superstar on the
// disc list starts with: its first group of ratings at 500 / 600 / 600 (the values every unused place holds; at 0 the
// cards never book the mod). Where that group sits in the 24 bytes is read off the game's own entries (+0 or +4);
// nothing is written while they aren't set up yet.
void Fresh(uint8_t* base, uint32_t p, uint8_t* ent) {
  if (g_fresh) return;
  int at0 = 0, at4 = 0;
  for (uint32_t k = 70; k < kPlaces; ++k) {
    const uint8_t* e = base + p + kEntries + k * kEntrySize;
    const uint32_t v0 = uint32_t(e[0]) << 8 | e[1], v4 = uint32_t(e[4]) << 8 | e[5];
    at0 += v0 >= 100 && v0 < 2000, at4 += v4 >= 100 && v4 < 2000;
  }
  if (std::max(at0, at4) < 10) return;
  g_fresh = true;
  const uint32_t off = at4 > at0 ? 4 : 0;
  for (size_t i = 0; i < g_ids.size(); ++i) {
    uint8_t* e = ent + i * kEntrySize;
    if (std::any_of(e, e + kEntrySize, [](uint8_t c) { return c != 0; })) continue;
    const uint16_t v[4] = {500, 600, 600, 0};
    for (int j = 0; j < 4; ++j) e[off + 2 * j] = uint8_t(v[j] >> 8), e[off + 2 * j + 1] = uint8_t(v[j]);
    REXLOG_INFO("[svr2011] universe mods: {} starts with the disc list's ratings (500 / 600 / 600 at +{})",
                SuperstarModFolder(g_ids[i]), off);
  }
}

fs::file_time_type SaveTime() {
  std::error_code ec;
  const auto t = fs::last_write_time(g_save, ec);
  return ec ? fs::file_time_type{} : t;
}

// ---- the file: "SVRUNIV1", u32 n, per mod: u8 name length + folder, 24 entry bytes, 150 place bytes, then the
// mod-mod bytes: n x n (row i, column j; i < j used)
void Write(uint8_t* base) {
  if (g_file.empty() || !g_entries) return;
  const uint32_t p = P(base);
  if (!p) return;
  const uint8_t* ent = base + Entries(base, p);
  const uint8_t* rows = base + g_rows;
  std::string out = "SVRUNIV1";
  const uint32_t n = uint32_t(g_ids.size());
  out.append(reinterpret_cast<const char*>(&n), 4);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string f = SuperstarModFolder(g_ids[i]).substr(0, 255);
    out += char(f.size());
    out += f;
    out.append(reinterpret_cast<const char*>(ent + i * kEntrySize), kEntrySize);
    out.append(reinterpret_cast<const char*>(rows + i * kRow), kPlaces);
  }
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = 0; j < n; ++j) out += char(rows[i * kRow + kPlaces + j]);
  const fs::path tmp = g_file.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o.write(out.data(), std::streamsize(out.size()));
    if (!o) {
      REXLOG_WARN("[svr2011] universe mods: {} couldn't be written", tmp.string());
      return;
    }
  }
  std::error_code ec;
  fs::rename(tmp, g_file, ec);
  if (ec) REXLOG_WARN("[svr2011] universe mods: {} couldn't be replaced ({})", g_file.string(), ec.message());
  else REXLOG_INFO("[svr2011] universe mods: {} mods' Universe data saved", n);
}

void Read(uint8_t* base) {
  Clear();
  std::ifstream in(g_file, std::ios::binary);
  if (!in) {
    REXLOG_INFO("[svr2011] universe mods: no {} - the mods start fresh in this Universe", g_file.filename().string());
    return;
  }
  std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t at = 8;
  uint32_t n = 0;
  if (t.size() < 12 || t.compare(0, 8, "SVRUNIV1") != 0) {
    REXLOG_WARN("[svr2011] universe mods: {} isn't one", g_file.string());
    return;
  }
  std::memcpy(&n, t.data() + at, 4);
  at += 4;
  const uint32_t p = P(base);
  uint8_t* ent = base + (p ? Entries(base, p) : g_entries + g_shift);
  uint8_t* rows = base + g_rows;
  std::vector<int> to(n, -1);  // the file's mod i -> its side index now (by folder)
  std::map<std::string, int> by;
  for (size_t i = 0; i < g_ids.size(); ++i) by[SuperstarModFolder(g_ids[i])] = int(i);
  int kept = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (at >= t.size()) {
    REXLOG_WARN("[svr2011] universe mods: {} is cut short", g_file.string());
    return;
  }
    const size_t len = uint8_t(t[at++]);
    if (at + len + kEntrySize + kPlaces > t.size()) {
    REXLOG_WARN("[svr2011] universe mods: {} is cut short", g_file.string());
    return;
  }
    const std::string f = t.substr(at, len);
    at += len;
    const auto it = by.find(f);
    if (it != by.end()) {
      to[i] = it->second, ++kept;
      std::memcpy(ent + it->second * kEntrySize, t.data() + at, kEntrySize);
      std::memcpy(rows + it->second * kRow, t.data() + at + kEntrySize, kPlaces);
    }
    at += kEntrySize + kPlaces;
  }
  if (at + size_t(n) * n <= t.size())
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < n; ++j) {
        const uint8_t v = uint8_t(t[at + i * n + j]);
        if (!v || to[i] < 0 || to[j] < 0 || to[i] == to[j]) continue;
        const int lo = std::min(to[i], to[j]), hi = std::max(to[i], to[j]);
        rows[lo * kRow + kPlaces + hi] = v;
      }
  REXLOG_INFO("[svr2011] universe mods: {} of {} mods' Universe data read ({} mods on now)", kept, n, g_ids.size());
}

}  // namespace

void InstallUniverseMods(rex::memory::Memory* memory, const fs::path& saves) {
  g_memory = memory;
  g_save = saves / "SaveData.dat";
  g_file = saves / ".universe" / "mods.bin";
  {  // (2.1.2 test builds kept it as Saves\SaveData.universe_mods.bin - moved, and the save header the saves layer
     // made for it removed)
    std::error_code ec;
    fs::create_directories(g_file.parent_path(), ec);
    const fs::path old = saves / "SaveData.universe_mods.bin";
    if (fs::is_regular_file(old, ec)) {
      if (!fs::exists(g_file, ec)) fs::rename(old, g_file, ec);
      if (!ec) fs::remove(old, ec);
      for (const char* x : {".header", ".png", ".file"}) fs::remove(saves / ".info" / ("SaveData.universe_mods.bin" + std::string(x)), ec);
      REXLOG_INFO("[svr2011] universe mods: side table moved to {}", g_file.string());
    }
  }
  g_ids = SuperstarModIds(false);  // (not the host's fetched ones)
  if (g_ids.size() > kMax) {
    REXLOG_WARN("[svr2011] universe mods: {} mods - the first {} take part in Universe", g_ids.size(), kMax);
    g_ids.resize(kMax);
  }
  g_entries = memory->SystemHeapAlloc(kMax * kEntrySize + 8);
  g_rows = memory->SystemHeapAlloc(kMax * kRow);
  g_shift = 0;
  Clear();
  g_seen = SaveTime();  // (the save there at start: only a save the game writes later is the side table's cue)
  g_loaded = g_seen == fs::file_time_type{};  // (no save yet: the first one written is a new Universe)
  REXLOG_INFO("[svr2011] universe mods: {} mods have a Universe place (side table {:08X} / {:08X})", g_ids.size(),
              g_entries, g_rows);
}

bool UniverseModIn(uint32_t id) { return g_entries && Index(id) >= 0; }

int64_t UniverseModPlace(uint8_t* base, uint32_t id) {
  const int i = Index(id);
  const uint32_t p = P(base);
  if (i < 0 || !p || !g_entries) return -1;
  Fresh(base, p, base + Entries(base, p));
  const uint32_t d = Entries(base, p) + uint32_t(i) * kEntrySize - (p + kEntries);  // (a multiple of 8)
  const uint32_t k = ((d >> 3) * 0x0AAAAAABu) & 0x1FFFFFFFu;  // (3k = d / 8 mod 2^29: 0x0AAAAAAB = 1/3 mod 2^29)
  if (uint32_t(k * kEntrySize) != d) {
    REXLOG_WARN("[svr2011] universe mods: no place for id {} (d {:08X})", id, d);
    return -1;
  }
  return int64_t(k);
}

int64_t UniverseModPair(uint8_t* base, uint32_t table, uint32_t a, int64_t place_a, uint32_t b, int64_t place_b,
                        bool working_copy) {
  const uint32_t p = P(base);
  if (!p || !g_rows || a == b || (table != p + kRivalry && !working_copy)) return -1;
  if (table != p + kRivalry) {  // (a working copy: its mod pairs are the side bytes themselves - see the header)
    static bool said = false;
    if (!said) {
      said = true;
      REXLOG_INFO("[svr2011] universe mods: rivalry working copy at {:08X} - its mod pairs use the side table", table);
    }
  }
  const int ia = Index(a), ib = Index(b);
  int row = -1;
  uint32_t col = 0;
  if (ia >= 0 && ib >= 0) {
    row = std::min(ia, ib), col = kPlaces + uint32_t(std::max(ia, ib));
  } else if (ia >= 0) {
    if (place_b < 0 || place_b >= kPlaces) return -1;
    row = ia, col = uint32_t(place_b);
  } else if (ib >= 0) {
    if (place_a < 0 || place_a >= kPlaces) return -1;
    row = ib, col = uint32_t(place_a);
  } else {
    return -1;
  }
  const uint32_t off = g_rows + uint32_t(row) * kRow + col - table;
  return off == 0xFFFFFFFFu ? -1 : int64_t(int32_t(off));
}

void UniverseModsReset() {
  Clear();
  REXLOG_INFO("[svr2011] universe mods: Universe reset - the mods' side table cleared");
}

void UniverseModsLoaded(uint8_t* base) {
  if (!g_memory || !g_entries) return;
  Read(base);
  g_seen = SaveTime();
  g_loaded = true;
}

namespace {
constexpr uint32_t kTeams = 0xF5C8, kTeamSize = 270, kTeamCount = 70, kTeamName = 0x50, kNoMember = 0x200;

struct Team {
  std::vector<uint32_t> members;
  std::string name;
  bool operator==(const Team& o) const { return members == o.members && name == o.name; }
};

Team ReadTeam(const uint8_t* t) {
  Team out;
  const uint32_t n = std::min<uint32_t>(uint32_t(t[0]) << 8 | t[1], 5);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t id = uint32_t(t[2 + 2 * i]) << 8 | t[3 + 2 * i];
    if (id != kNoMember) out.members.push_back(id);
  }
  for (uint32_t i = kTeamName; i < kTeamName + 48 && t[i] >= 0x20 && t[i] < 0x7F; ++i) out.name += char(t[i]);
  return out;
}

std::string Members(const Team& t) {
  std::string out;
  for (const uint32_t id : t.members) {
    const std::string mod = SuperstarModFolder(id);
    out += (out.empty() ? "" : ", ") + std::to_string(id) + (mod.empty() ? "" : " (mod " + mod + ")");
  }
  return out.empty() ? "none" : out;
}
}  // namespace

void UniverseTeamsTick(uint8_t* base) {
  static uint32_t frames = 0, save_seen = 0, grace = 0;  // (grace: ticks after a list - a save still coming in)
  static std::vector<Team> seen;
  if (++frames % 30) return;
  const uint32_t save = Rd32(base + kSavePtr);
  if (!save) return;
  std::vector<Team> now;
  for (uint32_t k = 0; k < kTeamCount; ++k) now.push_back(ReadTeam(base + save + kTeams + k * kTeamSize));
  const auto in_use = [](const Team& t) { return !t.members.empty() || !t.name.empty(); };
  int changed = 0;
  for (uint32_t k = 0; k < kTeamCount && !seen.empty(); ++k) changed += !(now[k] == seen[k]);
  if (save != save_seen || seen.empty() || changed > 3 || (changed && grace)) {  // (a save came in: the list)
    grace = 60;  // (about 30 s: a save's teams come in over several seconds)
    std::string list;
    int n = 0;
    for (uint32_t k = 0; k < kTeamCount; ++k)
      if (in_use(now[k])) ++n, list += fmt::format("{}{} \"{}\" [{}]", list.empty() ? "" : "; ", k + 1, now[k].name, Members(now[k]));
    REXLOG_INFO("[svr2011] universe teams: {} in this save{}{}", n, n ? ": " : "", list);
  } else {
    if (grace) --grace;
    const char* when = InMatch() ? "during a match" : "outside a match";
    for (uint32_t k = 0; k < kTeamCount; ++k) {
      if (now[k] == seen[k]) continue;
      const Team& a = seen[k];
      const Team& b = now[k];
      if (!in_use(a))
        REXLOG_INFO("[svr2011] universe teams: team {} \"{}\" made - {} ({})", k + 1, b.name, Members(b), when);
      else if (!in_use(b) || b.members.size() < 2)
        REXLOG_INFO("[svr2011] universe teams: team {} \"{}\" dissolved - it had {}, now {} ({})", k + 1, a.name,
                    Members(a), Members(b), when);
      else
        REXLOG_INFO("[svr2011] universe teams: team {} \"{}\" changed - {} -> \"{}\" {} ({})", k + 1, a.name,
                    Members(a), b.name, Members(b), when);
    }
  }
  seen = std::move(now);
  save_seen = save;
}

void UniverseModsTick(uint8_t* base) {
  static uint32_t frames = 0;
  if (!g_entries || g_ids.empty() || !g_loaded || ++frames % 60) return;  // (about once a second; never before a load:
  // the file would lose what it holds)
  const auto t = SaveTime();
  if (t == fs::file_time_type{} || t == g_seen) return;
  g_seen = t;
  Write(base);
}

}  // namespace svr2011
