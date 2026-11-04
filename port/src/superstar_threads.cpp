// Superstar Threads for superstar mods.
//
// Threads lists the superstars of edit\edit.pac EDIT/SSTL: one PACH child, a u16 count, then 484-byte
// records {u32 id (little-endian), 48 u32 part-name string ids (4 attires x 12, -1 = none), per part an
// area / view / kind byte, default RGB}. Part p of an attire paints channel (p-1)%4 of that attire's
// Mask1/2/3 texture in the model. A superstar's three slots are the last 324 bytes of his 1056-byte
// profile (+0x2DC, +0x348, +0x3B4: 108 bytes each, +0 = -1 when empty), saved with it in SaveData.dat.
//
// threads.pac in Mods\SuperstarOverlay holds the game's SSTL plus a record for each listed mod as
// EDIT/SSTM, and the game's lookup of /EDIT/SSTL is pointed at it. The game's SSTL is the installed DLCs' where
// there are any (a DLC's pac\edit\edit_dlcNN.pac has the whole list plus its superstars - Vol. 3: 50-58): built
// from the disc's alone, the DLC superstars were gone from Threads once a mod was listed (2.1.6 report). A mod gets
// - the base's record under its own id when its model is its base's (the attire-1 models, byte for byte:
//   an attire swap, a renamed copy);
// - else one from its threads.txt (tools/threads_parts.py names its masks' channels with stock part
//   names; the bytes come from the stock parts of the same name);
// - else none: not listed.
//
// A mod's slots are kept by its folder (Saves\.threads\<folder>.bin), not by the id it holds this run: put
// into its profile as it is set up (never the base's slots), written when the game saves.
#include "big_roster.h"
#include "superstar_threads.h"
#include "superstar_mods.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ppc.h>

#include "../modmaker/svrfmt/pac.h"

#include "generated/default/svr2011_init.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kIdToIndex = 0x82DB3610, kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kSlotsAt = 0x2DC, kSlotSize = 108, kSlots = 3, kSlotsSize = kSlotSize * kSlots;
// a record: u32 id, 48 part names (4 attires x 12; part p of an attire paints channel (p-1)%4 of its
// Mask1/2/3), then per part its area (camera), view and kind bytes, then its default RGB
constexpr size_t kRecord = 484, kParts = 48, kAreaAt = 196, kViewAt = 244, kKindAt = 292, kColourAt = 340;

uint32_t g_alias_slash = 0, g_alias_bare = 0;  // guest "/EDIT/SSTM", "EDIT/SSTM"
fs::path g_dir;                                // Saves\.threads
fs::path g_dlc_root;                           // the installed DLC packages
std::mutex g_mutex;
std::vector<svr2011::ThreadsMod> g_mods;              // the listed mods
std::map<std::string, std::vector<uint8_t>> g_saved;  // folder -> its 324 bytes as last saved / loaded
std::string g_listing;                                 // the listed mods: record source, parts per attire, model

std::vector<uint8_t> EmptySlots() {
  std::vector<uint8_t> s(kSlotsSize, 0);
  for (uint32_t k = 0; k < kSlots; ++k) std::memset(s.data() + k * kSlotSize, 0xFF, 4);
  return s;
}
bool IsEmpty(const uint8_t* s) {
  for (uint32_t k = 0; k < kSlots; ++k)
    if (std::memcmp(s + k * kSlotSize, "\xFF\xFF\xFF\xFF", 4)) return false;
  return true;
}

// A pac's attire-1 models: the EMD entries named "<6-digit id>0<kind>", by kind.
std::map<char, svrfmt::Bytes> Attire1Models(const fs::path& pac) {
  std::map<char, svrfmt::Bytes> out;
  svrfmt::Bytes raw;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile(pac.string(), raw) || !svrfmt::EpacRead(raw, e)) return out;
  for (const auto& g : e.groups) {
    if (g.type.rfind("EMD", 0)) continue;
    for (const auto& x : g.entries)
      if (x.name.size() == 8 && x.name[6] == '0') out[x.name[7]] = x.data;
  }
  return out;
}

// A mod's threads.txt (tools/threads_parts.py) -> its record (id left 0): lines "<attire>.<part>=<name id>,<rrggbb>"
// with a part name stock records use (its bytes from them). False with why: no file, or nothing usable in it.
bool PartsRecord(const fs::path& file, const std::map<uint32_t, uint32_t>& codes, svrfmt::Bytes& rec, std::string& why) {
  FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) {
    why = "its model isn't its base's and it has no threads.txt (Mod Maker: make Threads parts)";
    return false;
  }
  rec.assign(kRecord, 0);
  std::memset(rec.data() + 4, 0xFF, kParts * 4);
  std::memset(rec.data() + kColourAt, 0xFF, kParts * 3);
  int parts = 0, bad = 0;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    if (char* c = std::strchr(line, '#')) *c = 0;
    unsigned attire = 0, part = 0, name = 0, rgb = 0;
    char end = 0;
    if (std::sscanf(line, " format = %u", &name) == 1) {
      if (name != 1) bad = 1000;  // (a newer layout: don't guess)
      continue;
    }
    const int got = std::sscanf(line, " %u.%u = %u , %6x %c", &attire, &part, &name, &rgb, &end);
    if (got <= 0) continue;
    const auto code = codes.find(name);
    if (got != 4 || attire < 1 || attire > 4 || part < 1 || part > 12 || code == codes.end()) {
      ++bad;
      continue;
    }
    const size_t k = (attire - 1) * 12 + (part - 1);
    for (int i = 0; i < 4; ++i) rec[4 + k * 4 + i] = uint8_t(name >> (8 * i));
    rec[kAreaAt + k] = uint8_t(code->second >> 16), rec[kViewAt + k] = uint8_t(code->second >> 8);
    rec[kKindAt + k] = uint8_t(code->second);
    rec[kColourAt + k * 3] = uint8_t(rgb >> 16), rec[kColourAt + k * 3 + 1] = uint8_t(rgb >> 8);
    rec[kColourAt + k * 3 + 2] = uint8_t(rgb);
    ++parts;
  }
  std::fclose(f);
  if (bad >= 1000) {
    why = "its threads.txt is a newer format than this game reads";
    return false;
  }
  if (!parts) {
    why = bad ? "its threads.txt has no usable line (" + std::to_string(bad) + " bad)" : "its threads.txt lists no part";
    return false;
  }
  if (bad) REXLOG_WARN("[svr2011] superstar threads: {}: {} line(s) skipped (not attire.part=name id,rrggbb with a game part name)",
                       file.string(), bad);
  return true;
}

uint32_t RecordId(const svrfmt::Bytes& list, size_t o) {
  return uint32_t(list[o] | list[o + 1] << 8 | list[o + 2] << 16 | uint32_t(list[o + 3]) << 24);
}

// A DLC edit pac's EDIT/SSTL list (u16 count, records); false: none.
bool ReadSstl(const fs::path& pac, svrfmt::Bytes& list) {
  svrfmt::Bytes raw;
  svrfmt::Epac edit;
  if (!svrfmt::ReadFile(pac.string(), raw) || !svrfmt::EpacRead(raw, edit)) return false;
  for (const auto& g : edit.groups)
    for (const auto& x : g.entries) {
      std::vector<svrfmt::PachEntry> kids;
      if (g.type != "EDIT" || x.name != "SSTL" || !svrfmt::PachRead(svrfmt::Unpack(x.data), kids) || kids.empty())
        continue;
      list = svrfmt::Unpack(kids[0].data);
      const uint32_t count = list.size() >= 2 ? uint32_t(list[0] | list[1] << 8) : 0;
      return count && list.size() == 2 + count * kRecord;
    }
  return false;
}

// The installed DLCs' superstars added to the disc's list (`list`): each package's pac\edit\*.pac SSTL (fewest
// records first), the records whose id the list lacks appended. Their files, for the log.
std::string MergeDlcLists(svrfmt::Bytes& list) {
  std::vector<std::pair<fs::path, svrfmt::Bytes>> lists;
  std::error_code ec;
  if (!g_dlc_root.empty() && fs::is_directory(g_dlc_root, ec))
    for (const auto& pkg : fs::directory_iterator(g_dlc_root, ec)) {
      std::error_code e2;
      for (const auto& f : fs::directory_iterator(pkg.path() / "pac" / "edit", e2)) {
        svrfmt::Bytes l;
        if (f.is_regular_file(e2) && f.path().extension() == ".pac" && ReadSstl(f.path(), l))
          lists.emplace_back(f.path(), std::move(l));
      }
    }
  std::stable_sort(lists.begin(), lists.end(), [](const auto& a, const auto& b) { return a.second.size() < b.second.size(); });
  std::string names;
  // (the list keeps its records in their places - the game's own first, as the disc has them - and gets each DLC
  // superstar it lacks after them: the DLCs' lists put theirs first, and with the disc's superstars moved down by
  // them no Threads edit showed on anyone in the select screen, entrances or matches - 2.1.7 report a3e367)
  std::set<uint32_t> has;
  for (size_t o = 2; o < list.size(); o += kRecord) has.insert(RecordId(list, o));
  for (const auto& [file, l] : lists) {
    for (size_t o = 2; o < l.size(); o += kRecord)
      if (has.insert(RecordId(l, o)).second) list.insert(list.end(), l.begin() + long(o), l.begin() + long(o + kRecord));
    const uint32_t n = uint32_t((list.size() - 2) / kRecord);
    list[0] = uint8_t(n), list[1] = uint8_t(n >> 8);
    names += (names.empty() ? "" : ", ") + file.filename().string() + " (" + std::to_string((l.size() - 2) / kRecord) + ")";
  }
  return names;
}

std::string Dec(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%u", v);
  return b;
}

}  // namespace

namespace svr2011 {

void SetThreadsSaves(const fs::path& saves) { g_dir = saves / ".threads"; }
void SetThreadsDlc(const fs::path& dlc_root) { g_dlc_root = dlc_root; }

bool BuildThreadsOverlay(rex::memory::Memory* memory, const fs::path& game, const fs::path& overlay,
                         const std::vector<ThreadsMod>& mods) {
  std::error_code ec;
  const fs::path out = overlay / "threads.pac";
  fs::remove(out, ec);
  svrfmt::Bytes raw;
  svrfmt::Epac edit;
  if (mods.empty() || !svrfmt::ReadFile((game / "pac" / "edit" / "edit.pac").string(), raw) ||
      !svrfmt::EpacRead(raw, edit))
    return false;
  const svrfmt::EpacEntry* sstl = nullptr;
  svrfmt::EpacGroup group;
  for (const auto& g : edit.groups)
    if (g.type == "EDIT") {
      group = g;
      for (const auto& x : g.entries)
        if (x.name == "SSTL") sstl = &x;
    }
  std::vector<svrfmt::PachEntry> kids;
  if (!sstl || !svrfmt::PachRead(svrfmt::Unpack(sstl->data), kids) || kids.empty()) {
    REXLOG_WARN("[svr2011] superstar threads: edit.pac has no EDIT/SSTL as expected - mods not listed");
    return false;
  }
  svrfmt::Bytes list = svrfmt::Unpack(kids[0].data);
  uint32_t count = list.size() >= 2 ? uint32_t(list[0] | list[1] << 8) : 0;
  if (!count || list.size() != 2 + count * kRecord) {
    REXLOG_WARN("[svr2011] superstar threads: SSTL is {} bytes for {} records - mods not listed", list.size(), count);
    return false;
  }
  if (const std::string dlc = MergeDlcLists(list); !dlc.empty()) {
    count = uint32_t(list[0] | list[1] << 8);
    REXLOG_INFO("[svr2011] superstar threads: the DLCs' lists ({}) - {} superstars before the mods", dlc, count);
  }
  std::map<uint32_t, size_t> at;  // id -> record offset
  std::map<uint32_t, std::map<uint32_t, int>> uses;  // part name -> {its area/view/kind bytes -> how many parts}
  for (uint32_t i = 0; i < count; ++i) {
    const size_t o = 2 + i * kRecord;
    at[RecordId(list, o)] = o;
    for (uint32_t k = 0; k < kParts; ++k) {
      const size_t n = o + 4 + k * 4;
      const uint32_t name = uint32_t(list[n] | list[n + 1] << 8 | list[n + 2] << 16 | uint32_t(list[n + 3]) << 24);
      if (name != 0xFFFFFFFFu)
        ++uses[name][uint32_t(list[o + kAreaAt + k]) << 16 | list[o + kViewAt + k] << 8 | list[o + kKindAt + k]];
    }
  }
  std::map<uint32_t, uint32_t> codes;  // part name -> the bytes stock parts of that name use most
  for (const auto& [name, m] : uses)
    codes[name] = std::max_element(m.begin(), m.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  std::vector<ThreadsMod> listed;
  std::string names, summary;
  for (const auto& m : mods) {
    if (at.count(m.slot)) {
      REXLOG_INFO("[svr2011] superstar threads: {} not listed - its id {} has the game's own Threads record", m.folder, m.slot);
      continue;
    }
    const auto b = at.find(m.base);  // (end: the base isn't in Threads - the mod can still bring threads.txt)
    char nm[16];
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.base);
    const auto base_models = Attire1Models(game / "pac" / "ch" / nm);
    const auto own = Attire1Models(m.ch_pac);
    svrfmt::Bytes rec;
    std::string why, from;
    if (b != at.end() && !base_models.empty() && own == base_models) {
      rec.assign(list.begin() + long(b->second), list.begin() + long(b->second + kRecord));
      from = "the record of " + Dec(m.base);
    } else if (!PartsRecord(m.ch_pac.parent_path() / "threads.txt", codes, rec, why)) {
      REXLOG_INFO("[svr2011] superstar threads: {} not listed - {}", m.folder, why);
      continue;
    } else {
      from = "threads.txt";
    }
    for (int k = 0; k < 4; ++k) rec[k] = uint8_t(m.slot >> (8 * k));
    std::string per;  // (parts per attire: the mask channels the record paints)
    for (int a = 0; a < 4; ++a) {
      int n = 0;
      for (int p = 0; p < 12; ++p) n += std::memcmp(&rec[4 + (a * 12 + p) * 4], "\xFF\xFF\xFF\xFF", 4) != 0;
      per += (a ? "/" : "") + Dec(uint32_t(n));
    }
    list.insert(list.end(), rec.begin(), rec.end());
    listed.push_back(m);
    names += (names.empty() ? "" : ", ") + m.folder + " (id " + Dec(m.slot) + ")";
    summary += (summary.empty() ? "" : "; ") + m.folder + " id " + Dec(m.slot) + ": " + from + ", parts " + per +
               " (attires 1-4), model " + m.ch_pac.filename().string();
  }
  if (listed.empty()) return false;
  const uint32_t n = count + uint32_t(listed.size());
  list[0] = uint8_t(n), list[1] = uint8_t(n >> 8);
  kids[0].data = list;
  svrfmt::EpacEntry entry;
  entry.name = "SSTM";
  entry.data = svrfmt::PachWrite(kids);
  group.entries = {entry};
  edit.groups = {group};
  if (!svrfmt::WriteFile(out.string(), svrfmt::EpacWrite(edit))) {
    REXLOG_WARN("[svr2011] superstar threads: cannot write {}", out.string());
    return false;
  }
  uint8_t* mem = memory->TranslateVirtual<uint8_t*>(0);
  g_alias_slash = memory->SystemHeapAlloc(16);
  g_alias_bare = memory->SystemHeapAlloc(16);
  std::memcpy(mem + g_alias_slash, "/EDIT/SSTM", 11);
  std::memcpy(mem + g_alias_bare, "EDIT/SSTM", 10);
  {
    std::lock_guard lock(g_mutex);
    g_mods = listed;
    g_listing = summary;
  }
  REXLOG_INFO("[svr2011] superstar threads: {} mod(s) listed: {}", listed.size(), names);
  return true;
}

uint32_t ThreadsAlias(const char* name) {
  if (!name) return 0;
  const bool slash = !std::strcmp(name, "/EDIT/SSTL");
  if (!slash && std::strcmp(name, "EDIT/SSTL")) return 0;
  {  // (Threads opens its list: what it gets, once a few seconds - problem reports)
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(10)) {
      last = now;
      std::lock_guard lock(g_mutex);
      REXLOG_INFO("[svr2011] superstar threads: list opened - {}", g_listing.empty() ? "the game's own (no mod listed)" : g_listing);
    }
  }
  if (!g_alias_slash) return 0;
  return slash ? g_alias_slash : g_alias_bare;
}

void LogThreadsSlots(const uint8_t* base) {
  static const bool on = std::getenv("SVR2011_TEST_THREADS_SLOTS") != nullptr;
  static auto last = std::chrono::steady_clock::time_point{};
  if (!on || std::chrono::steady_clock::now() - last < std::chrono::seconds(10)) return;
  last = std::chrono::steady_clock::now();
  std::string s;
  for (uint32_t id = 0; id < 512; ++id) {
    const uint32_t si = svr2011::RosterIndex(base, id);
    if (si >= 512) continue;
    std::string names;
    for (uint32_t k = 0; k < kSlots; ++k) {
      const char* n = reinterpret_cast<const char*>(base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kSlotsAt + k * kSlotSize + 0x28);
      if (n[0] >= 32 && n[0] < 127) names += (names.empty() ? "" : "|") + std::string(n, strnlen(n, 16));
    }
    if (!names.empty()) s += " [id " + Dec(id) + " profile " + Dec(si) + ": " + names + "]";
  }
  REXLOG_INFO("[svr2011] superstar threads: test - named slots:{}", s);
}

void ThreadsSlots(uint8_t* profile, const std::string& folder) {
  if (!profile || g_dir.empty()) return;
  std::lock_guard lock(g_mutex);
  auto it = g_saved.find(folder);
  if (it == g_saved.end()) {
    std::vector<uint8_t> s;
    if (FILE* f = std::fopen((g_dir / (folder + ".bin")).string().c_str(), "rb")) {
      s.resize(kSlotsSize);
      if (std::fread(s.data(), 1, kSlotsSize, f) != kSlotsSize) s.clear();
      std::fclose(f);
    }
    it = g_saved.emplace(folder, s.empty() ? EmptySlots() : s).first;
  }
  std::memcpy(profile + kSlotsAt, it->second.data(), kSlotsSize);
}

}  // namespace svr2011

// The save written (sub_827235D8 builds it from the records and profiles): each listed mod's slots, as its
// profile holds them now, into Saves\.threads\<folder>.bin (removed when all three are empty).
REX_EXTERN(__imp__sub_827235D8);
REX_HOOK_RAW(sub_827235D8) {
  {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    for (const auto& m : g_mods) {
      const uint32_t si = svr2011::RosterIndex(base, m.slot);
      if (si >= 512) continue;
      const uint8_t* s = base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kSlotsAt;
      std::vector<uint8_t> now(s, s + kSlotsSize);
      auto& was = g_saved[m.folder];
      if (was == now) continue;
      const fs::path f = g_dir / (m.folder + ".bin");
      if (IsEmpty(s)) {
        fs::remove(f, ec);
      } else {
        fs::create_directories(g_dir, ec);
        const fs::path part = fs::path(f).concat(".part");
        if (FILE* o = std::fopen(part.string().c_str(), "wb")) {
          const bool ok = std::fwrite(now.data(), 1, now.size(), o) == now.size();
          std::fclose(o);
          if (ok) fs::rename(part, f, ec);
        }
      }
      was = now;
      REXLOG_INFO("[svr2011] superstar threads: {} - slots saved ({})", m.folder, IsEmpty(s) ? "all empty" : "edited");
    }
  }
  svr2011::OnlineTempForSave(base, true);  // (the fetched mods of this run: not in the save)
  svr2011::PerModSavesForSave(base, true);  // (per_mod_saves: the mods' data in their own files)
  __imp__sub_827235D8(ctx, base);
  // Test aid: SVR2011_TEST_SAVE_DUMP=<folder> - the records, profiles and the built save (guest memory) as files
  if (const char* dir = std::getenv("SVR2011_TEST_SAVE_DUMP")) {
    auto dump = [&](const char* name, uint32_t at, uint32_t size) {
      if (FILE* f = std::fopen((std::string(dir) + "\\" + name).c_str(), "wb")) {
        std::fwrite(base + at, 1, size, f);
        std::fclose(f);
      }
    };
    dump("records.bin", 0x82E407C0, 512 * 260);
    dump("profiles.bin", kProfiles, 512 * kProfileSize);
    dump("idtoindex.bin", kIdToIndex, 1024);
    const uint32_t save = uint32_t(base[0x82E407B4] << 24 | base[0x82E407B5] << 16 | base[0x82E407B6] << 8 | base[0x82E407B7]);
    if (save) dump("save.bin", save, 0x80000);
    REXLOG_INFO("[svr2011] superstar threads: test - records, profiles and the save dumped to {}", dir);
  }
  svr2011::PerModSavesForSave(base, false);
  svr2011::OnlineTempForSave(base, false);
}

// The edit screens' light (sub_8279EDE8 opens a colour screen: sub_82788310 -> sub_82787A20) is set on the
// preview object's light (+116) or, when it has none, on the scene's at *0x82E3CC50 (+272): written without the
// null check its siblings sub_82787990 / sub_82787AC0 make. With no scene light yet (2.1.4.1 Android report: a
// Threads attire edit) the write to 0x110 faulted over and over and the game froze: skipped as the siblings do.
REX_EXTERN(__imp__sub_82787A20);
REX_HOOK_RAW(sub_82787A20) {
  const auto rd32 = [&](uint32_t a) {
    return uint32_t(base[a] << 24 | base[a + 1] << 16 | base[a + 2] << 8 | base[a + 3]);
  };
  static const bool test_none = std::getenv("SVR2011_TEST_NO_SCENE_LIGHT") != nullptr;  // (test aid: as if none)
  if (ctx.r3.u32 && !rd32(ctx.r3.u32 + 116) && (test_none || !rd32(0x82E3CC50))) {
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_WARN("[svr2011] edit screen: no preview or scene light (object {:08X}, caller {:08X}) - light left as it is",
                  ctx.r3.u32, uint32_t(ctx.lr));
    }
    return;
  }
  __imp__sub_82787A20(ctx, base);
}
