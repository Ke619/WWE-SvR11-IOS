// CAW parts from mods (see caw_parts.h, docs/CAW_PARTS.md).
//
// Create A Superstar's part lists are edit\edit.pac EDIT/MENU: per category (child id = the category, the same
// number as pac\edit\edNN.pac and casThumb.pac's CAST/"%04X") a 12-byte header (u16 total, men's, women's) and
// 60-byte records (+12 u16 model -> /EDNN/M%03X, +14 u16 texture -> /EDNN/T%03X, +20 colour slots, +23 gender,
// +32 u16 item - records sharing one are its variants, +36 1 on an item's first record). The editor resolves every
// name through sub_826B87B0, so a mod part needs no change to the game's pacs:
// - its model / texture entries go into the overlay pac Mods/SuperstarOverlay/cawparts.pac under names the game's
//   pac hasn't (M<next>, T<next>) in a group of its own, PNNN: the resolver's "/EDNN/M<next>" and "EDNN/M<next>" are
//   pointed there (a bare name is only looked for in the first archive with that group - the game's edNN.pac);
// - the category's MENU child gets its records appended and its thumbnail set (casThumb, one DDS per record) its
//   thumbnails: the whole MENU as EDIQ/MENU and each changed set as CASQ/"%04X" in the same pac, and the
//   resolver's "/EDIT/MENU" and "/CAST/%04X" are pointed at them (CawPartsAlias).
// A saved CAW keeps a part as its record index, so records are only ever appended: Mods/SuperstarOverlay/
// cawparts.index keeps each mod part's record / model / texture numbers across starts. A removed mod's record
// stays (a variant of the category's first stock item of that gender, hidden in the list): a CAW that wore the
// part shows that stock part instead (the same with Start Without Mods: every mod record falls back).
//
// A mod (Mods/CawParts/<id>/manifest.txt, type=cawpart; a "disabled" file turns it off):
//   part=<category> <m|f|mf> <stem>     one per part: <stem>.m.pach (the model entry: PACH of JBOY 20xx/21xx/22xx),
//                                       <stem>.t.pach (the textures: PACH of 10xx/11xx TGAs), <stem>.thumb.dds
//                                       (256 x 128 DXT1), <stem>.rec (its 60-byte MENU record: colour bands, slots)
// (tools/caw_part.py makes them from an OBJ / FBX.)
#include "caw_parts.h"
#include "no_mods.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

#include "../modmaker/svrfmt/pac.h"
#include "generated/default/svr2011_init.h"
#include "superstar_mods.h"

namespace fs = std::filesystem;

namespace {

std::map<uint64_t, uint32_t> g_row_source;  // (category << 32 | a mod part's texture number -> the texture its .rec names)

constexpr size_t kHead = 12, kRec = 60;

struct Part {          // a mod part (one gender: an "mf" part is two)
  std::string key;     // "<mod>/<stem>/<m|f>" - its line in cawparts.index
  std::string mod, stem;
  int category = 0, gender = 1;
  fs::path folder;
};
struct Slot {          // cawparts.index: what a key has, for good
  int category = 0, record = 0, model = 0, texture = 0, gender = 1;
};

rex::memory::Memory* g_memory = nullptr;
uint32_t g_menu_slash = 0, g_menu_bare = 0;
std::map<int, std::pair<uint32_t, uint32_t>> g_cast;  // category -> guest "/CASQ/00nn", "CASQ/00nn"
std::map<std::string, std::pair<uint32_t, uint32_t>> g_part_alias;  // "ED18/M054" -> guest "/P018/M054", "P018/M054"
std::map<int, int> g_first;                          // category -> its first mod record
std::vector<std::string> g_listing;

fs::path Game() { return rex::filesystem::GetExecutableFolder(); }

uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }

uint32_t Guest(const std::string& s) {
  const uint32_t at = g_memory->SystemHeapAlloc(uint32_t(s.size() + 1));
  std::memcpy(g_memory->TranslateVirtual<uint8_t*>(at), s.c_str(), s.size() + 1);
  return at;
}

std::string Hex4(int n) {
  char b[8];
  std::snprintf(b, sizeof b, "%04X", n);
  return b;
}

std::vector<Part> ReadMods() {
  std::vector<Part> out;
  std::error_code ec;
  const fs::path root = Game() / "Mods" / "CawParts";
  if (svr2011::NoMods() || !fs::is_directory(root, ec)) return out;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(root, ec))
    if (e.is_directory(ec) && e.path().filename().string()[0] != '.' && !fs::exists(e.path() / "disabled", ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    std::ifstream in(d / "manifest.txt", std::ios::binary);
    std::string line, type, id = d.filename().string();
    std::vector<std::string> parts;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("type=", 0) == 0) type = line.substr(5);
      if (line.rfind("id=", 0) == 0 && line.size() > 3) id = line.substr(3);
      if (line.rfind("part=", 0) == 0) parts.push_back(line.substr(5));
    }
    std::transform(type.begin(), type.end(), type.begin(), ::tolower);
    if (type != "cawpart") continue;
    for (const auto& p : parts) {
      std::istringstream s(p);
      int cat = 0;
      std::string g, stem;
      if (!(s >> cat >> g >> stem) || cat <= 0 || cat > 255) {
        REXLOG_WARN("[svr2011] CAW parts: {}: part line \"{}\" not understood", id, p);
        continue;
      }
      for (char c : g) {
        if (c != 'm' && c != 'f') continue;
        Part x;
        x.mod = id, x.stem = stem, x.category = cat, x.gender = c == 'm' ? 1 : 2, x.folder = d;
        x.key = id + "/" + stem + "/" + c;
        bool ok = true;
        for (const char* ext : {".m.pach", ".t.pach", ".thumb.dds", ".rec"})
          if (!fs::is_regular_file(d / (stem + ext), ec)) {
            REXLOG_WARN("[svr2011] CAW parts: {}: {}{} missing - part left out", id, stem, ext);
            ok = false;
          }
        if (ok) out.push_back(x);
      }
    }
  }
  return out;
}

std::map<std::string, Slot> ReadIndex(const fs::path& file) {
  std::map<std::string, Slot> m;
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream s(line);
    std::string key;
    Slot x;
    if (std::getline(s, key, '\t') && s >> x.category >> x.record >> x.model >> x.texture >> x.gender) m[key] = x;
  }
  return m;
}

bool WriteIndex(const fs::path& file, const std::map<std::string, Slot>& m) {
  std::ofstream o(file, std::ios::binary | std::ios::trunc);
  o << "# CAW parts: each mod part's record / model / texture numbers, kept for good (saved CAWs keep the record)\n";
  for (const auto& [k, x] : m) o << k << '\t' << x.category << ' ' << x.record << ' ' << x.model << ' ' << x.texture << ' ' << x.gender << '\n';
  return bool(o);
}

// The number of M / T entries of a category's edNN.pac (the next index for each: the names are hex, from 0).
bool PacCounts(int category, int& models, int& textures) {
  char n[16];
  std::snprintf(n, sizeof n, "ed%02d.pac", category);
  svrfmt::Bytes d;
  std::vector<svrfmt::EpacRow> rows;
  std::ifstream f(Game() / "pac" / "edit" / n, std::ios::binary);
  if (!f) return false;
  d.resize(0x4000);
  f.read(reinterpret_cast<char*>(d.data()), 0x4000);
  f.seekg(0, std::ios::end);
  if (!svrfmt::EpacTable(d, uint64_t(f.tellg()), rows)) return false;
  models = textures = 0;
  for (const auto& r : rows) {
    if (!r.name.empty() && r.name[0] == 'M') ++models;
    if (!r.name.empty() && r.name[0] == 'T') ++textures;
  }
  return models > 0;
}

}  // namespace

namespace svr2011 {

void InstallCawParts(rex::memory::Memory* memory) {
  g_memory = memory;
  std::error_code ec;
  const fs::path overlay = Game() / "Mods" / "SuperstarOverlay";
  const fs::path index_file = overlay / "cawparts.index", out = overlay / "cawparts.pac";
  const std::vector<Part> parts = ReadMods();
  std::map<std::string, Slot> index = ReadIndex(index_file);
  fs::remove(out, ec);
  if (parts.empty() && index.empty()) return;  // (no mod part, now or ever)

  // the game's lists
  svrfmt::Bytes raw;
  svrfmt::Epac edit, thumbs;
  std::vector<svrfmt::PachEntry> menu, cats, casts;
  const svrfmt::EpacEntry* menu_entry = nullptr;
  if (!svrfmt::ReadFile((Game() / "pac" / "edit" / "edit.pac").string(), raw) || !svrfmt::EpacRead(raw, edit)) {
    REXLOG_WARN("[svr2011] CAW parts: pac/edit/edit.pac unreadable - mod parts left out");
    return;
  }
  for (const auto& g : edit.groups)
    for (const auto& e : g.entries)
      if (g.type == "EDIT" && e.name == "MENU") menu_entry = &e;
  if (!menu_entry || !svrfmt::PachRead(svrfmt::Unpack(menu_entry->data), menu) || menu.empty() ||
      !svrfmt::PachRead(svrfmt::Unpack(menu[0].data), cats)) {
    REXLOG_WARN("[svr2011] CAW parts: edit.pac has no EDIT/MENU as expected - mod parts left out");
    return;
  }
  if (!svrfmt::ReadFile((Game() / "pac" / "edit" / "casThumb.pac").string(), raw) || !svrfmt::EpacRead(raw, thumbs) ||
      thumbs.groups.empty()) {
    REXLOG_WARN("[svr2011] CAW parts: pac/edit/casThumb.pac unreadable - mod parts left out");
    return;
  }
  auto cat_of = [&](int category) -> svrfmt::PachEntry* {
    for (auto& c : cats)
      if (int(c.id) == category) return &c;
    return nullptr;
  };

  // numbers for parts that have none yet: after the game's and every number ever given (append-only)
  std::set<int> touched;
  for (const auto& [k, x] : index) touched.insert(x.category);
  for (const auto& p : parts) touched.insert(p.category);
  std::map<int, int> stock_records, next_record, next_model, next_texture;
  for (int c : touched) {
    svrfmt::PachEntry* e = cat_of(c);
    int models = 0, textures = 0;
    if (!e || !PacCounts(c, models, textures)) {
      REXLOG_WARN("[svr2011] CAW parts: category {} unknown - its mod parts left out", c);
      continue;
    }
    const svrfmt::Bytes k = svrfmt::Unpack(e->data);
    stock_records[c] = k.size() >= kHead ? Rd16(k.data()) : 0;
    next_record[c] = stock_records[c], next_model[c] = models, next_texture[c] = textures;
  }
  for (const auto& [k, x] : index) {
    next_record[x.category] = std::max(next_record[x.category], x.record + 1);
    next_model[x.category] = std::max(next_model[x.category], x.model + 1);
    next_texture[x.category] = std::max(next_texture[x.category], x.texture + 1);
  }
  bool index_changed = false;
  std::map<std::string, const Part*> present;
  for (const auto& p : parts) {
    if (!stock_records.count(p.category)) continue;
    present[p.key] = &p;
    if (index.count(p.key)) continue;
    Slot x;
    x.category = p.category, x.gender = p.gender;
    x.record = next_record[p.category]++, x.model = next_model[p.category]++, x.texture = next_texture[p.category]++;
    index[p.key] = x;
    index_changed = true;
  }

  // the categories: records in record order (a removed mod's: a hidden variant of a stock item), thumbnails
  svrfmt::Epac pac;
  pac.header = edit.header;
  std::map<int, svrfmt::EpacGroup> parts_groups;  // category -> "EDNN" with the present parts' entries
  svrfmt::EpacGroup cast_group;
  cast_group.type = "CASQ";
  svrfmt::EpacGroup* cast_src = &thumbs.groups[0];
  g_listing.clear();
  for (int c : touched) {
    if (!stock_records.count(c)) continue;
    std::vector<std::pair<int, std::string>> order;  // record -> key
    for (const auto& [k, x] : index)
      if (x.category == c) order.push_back({x.record, k});
    std::sort(order.begin(), order.end());
    svrfmt::PachEntry* e = cat_of(c);
    svrfmt::Bytes k = svrfmt::Unpack(e->data);
    const int stock = stock_records[c];
    if (k.size() != kHead + size_t(stock) * kRec) {
      REXLOG_WARN("[svr2011] CAW parts: category {}'s list is {} bytes for {} records - left as it is", c, k.size(), stock);
      continue;
    }
    // the category's thumbnails
    std::vector<svrfmt::PachEntry> th;
    const std::string tname = Hex4(c);
    for (const auto& te : cast_src->entries)
      if (te.name == tname) svrfmt::PachRead(svrfmt::Unpack(te.data), th);
    uint32_t item = 0;  // (the last item number: a new part is an item of its own after it)
    for (int r = 0; r < stock; ++r) item = std::max<uint32_t>(item, Rd16(&k[kHead + size_t(r) * kRec + 32]));
    int first_stock[3] = {-1, -1, -1};  // gender -> the first stock record
    for (int r = 0; r < stock; ++r) {
      const int gnd = k[kHead + size_t(r) * kRec + 23];
      if (gnd >= 1 && gnd <= 2 && first_stock[gnd] < 0) first_stock[gnd] = r;
    }
    char ed[8], pgroup[8];
    std::snprintf(ed, sizeof ed, "ED%02d", c);
    std::snprintf(pgroup, sizeof pgroup, "P%03d", c);
    svrfmt::EpacGroup& pg = parts_groups[c];
    pg.type = pgroup;
    int men = 0, women = 0, expected = stock;
    for (const auto& [rec_no, key] : order) {
      const Slot& x = index[key];
      if (rec_no != expected) {  // (a gap: an index edited by hand) - filled so the records stay in place
        REXLOG_WARN("[svr2011] CAW parts: category {}: record {} expected, the index says {} - stopping there", c, expected, rec_no);
        break;
      }
      ++expected;
      svrfmt::Bytes rec(kRec, 0);
      const auto it = present.find(key);
      const int src = first_stock[x.gender] >= 0 ? first_stock[x.gender] : 0;
      bool ok = false;
      if (it != present.end()) {
        const Part& p = *it->second;
        svrfmt::Bytes r, m, t, dds;
        if (svrfmt::ReadFile((p.folder / (p.stem + ".rec")).string(), r) && r.size() == kRec &&
            svrfmt::ReadFile((p.folder / (p.stem + ".m.pach")).string(), m) && svrfmt::IsPach(m) &&
            svrfmt::ReadFile((p.folder / (p.stem + ".t.pach")).string(), t) && svrfmt::IsPach(t) &&
            svrfmt::ReadFile((p.folder / (p.stem + ".thumb.dds")).string(), dds) && dds.size() > 128) {
          rec = r;
          g_row_source[uint64_t(c) << 32 | uint32_t(x.texture)] = uint32_t(r[14]) | uint32_t(r[15]) << 8;
          Wr16(&rec[12], uint32_t(x.model));
          Wr16(&rec[14], uint32_t(x.texture));
          rec[23] = uint8_t(x.gender);
          Wr16(&rec[32], ++item);
          rec[36] = 1;
          char mn[8], tn[8];
          std::snprintf(mn, sizeof mn, "M%03X", x.model);
          std::snprintf(tn, sizeof tn, "T%03X", x.texture);
          pg.entries.push_back({mn, m});
          pg.entries.push_back({tn, t});
          for (const char* n : {mn, tn}) g_part_alias[std::string(ed) + "/" + n] = {Guest(std::string("/") + pgroup + "/" + n),
                                                                                    Guest(std::string(pgroup) + "/" + n)};
          th.push_back({uint32_t(rec_no), dds});
          ok = true;
          g_listing.push_back(p.mod + ": category " + std::to_string(c) + " " + (x.gender == 1 ? "men" : "women") +
                              " record " + std::to_string(rec_no) + " (" + mn + ")");
        } else {
          REXLOG_WARN("[svr2011] CAW parts: {}: {} unreadable - its record falls back to a stock part", p.mod, p.stem);
        }
      }
      if (!ok) {  // a removed (or broken) mod's record: the stock item's variant, not listed
        rec.assign(k.begin() + long(kHead + size_t(src) * kRec), k.begin() + long(kHead + size_t(src + 1) * kRec));
        rec[36] = 0;
        svrfmt::Bytes stock_thumb;
        for (const auto& te : th)
          if (int(te.id) == src) stock_thumb = te.data;
        th.push_back({uint32_t(rec_no), stock_thumb});
        if (it == present.end())
          g_listing.push_back(key.substr(0, key.find('/')) + ": category " + std::to_string(c) + " record " +
                              std::to_string(rec_no) + " - mod not here, falls back to stock record " + std::to_string(src));
      }
      rec[23] == 1 ? ++men : ++women;
      k.insert(k.end(), rec.begin(), rec.end());
    }
    const int added = expected - stock;
    if (!added) continue;
    Wr16(&k[0], uint32_t(stock + added));
    Wr16(&k[2], uint32_t(Rd16(&k[2]) + men));
    Wr16(&k[4], uint32_t(Rd16(&k[4]) + women));
    e->data = k;
    cast_group.entries.push_back({tname, svrfmt::PachWrite(th)});
    g_first[c] = stock;
  }
  if (g_first.empty()) return;
  menu[0].data = svrfmt::PachWrite(cats);
  svrfmt::EpacGroup menu_group;
  menu_group.type = "EDIQ";
  menu_group.entries.push_back({"MENU", svrfmt::PachWrite(menu)});
  pac.groups.push_back(menu_group);
  pac.groups.push_back(cast_group);
  for (auto& [c, g] : parts_groups)
    if (!g.entries.empty()) pac.groups.push_back(g);
  svrfmt::Bytes o = svrfmt::EpacWrite(pac);
  uint32_t toc = 0;  // (header +4: the directory's size)
  for (const auto& g : pac.groups) toc += 12 + 12 * uint32_t(g.entries.size());
  o[4] = uint8_t(toc), o[5] = uint8_t(toc >> 8), o[6] = uint8_t(toc >> 16), o[7] = uint8_t(toc >> 24);
  fs::create_directories(overlay, ec);
  if ((index_changed && !WriteIndex(index_file, index)) || !svrfmt::WriteFile(out.string(), o)) {
    REXLOG_WARN("[svr2011] CAW parts: can't write {} - mod parts left out", overlay.string());
    g_first.clear();
    return;
  }
  g_menu_slash = Guest("/EDIQ/MENU");
  g_menu_bare = Guest("EDIQ/MENU");
  for (const auto& e : cast_group.entries) {
    const int c = std::stoi(e.name, nullptr, 16);
    g_cast[c] = {Guest("/CASQ/" + e.name), Guest("CASQ/" + e.name)};
  }
  AddOverlayMount("cawparts.pac");
  for (const auto& l : g_listing) REXLOG_INFO("[svr2011] CAW parts: {}", l);
  REXLOG_INFO("[svr2011] CAW parts: {} categor{} changed, {} mod part(s)", g_first.size(), g_first.size() == 1 ? "y" : "ies",
              parts.size());
}

uint32_t CawPartsAlias(const char* name) {
  if (!name || (!g_menu_slash && g_cast.empty())) return 0;
  const bool slash = *name == '/';
  const char* n = slash ? name + 1 : name;
  if (!std::strcmp(n, "EDIT/MENU")) return slash ? g_menu_slash : g_menu_bare;
  static const bool no_part_alias = std::getenv("SVR2011_TEST_CAWPARTS_NOALIAS") != nullptr;  // (experiment)
  if (!no_part_alias && (n[0] == 'E' || n[0] == 'P') && std::strlen(n) == 9) {
    // (the bare name - asked for a part past the game's own count, as for a DLC one, also as our own name past its
    // "/" - isn't found in an overlay archive: it gets the full name)
    const auto it = g_part_alias.find(n);
    if (it != g_part_alias.end()) return it->second.first;
    if (!slash && n[0] == 'P')
      for (const auto& [k, v] : g_part_alias)
        if (!std::strcmp(n, g_memory->TranslateVirtual<const char*>(v.second))) return v.first;
  }
  if (!std::strncmp(n, "CAST/", 5) && std::strlen(n) == 9) {
    const auto it = g_cast.find(int(std::strtol(n + 5, nullptr, 16)));
    if (it != g_cast.end()) return slash ? it->second.first : it->second.second;
  }
  return 0;
}

uint32_t CawPartsMissing(const char* name) {
  if (!name || !g_memory || std::strncmp(name, "/ED", 3) || std::strlen(name) != 10 || name[5] != '/' ||
      (name[6] != 'M' && name[6] != 'T') || !std::isdigit(uint8_t(name[3])) || !std::isdigit(uint8_t(name[4])) ||
      !std::strcmp(name + 7, "000"))
    return 0;
  static std::mutex m;
  static std::map<std::string, uint32_t> names;
  std::string first(name);
  first.replace(7, 3, "000");
  std::lock_guard lock(m);
  auto it = names.find(first);
  if (it == names.end()) {
    it = names.emplace(first, Guest(first)).first;
    REXLOG_WARN("[svr2011] CAW parts: {} is in no pac (a removed mod's part?): {} instead", name, first);
  }
  return it->second;
}

int CawPartsFirstRecord(int category) {
  const auto it = g_first.find(category);
  return it == g_first.end() ? -1 : it->second;
}

std::vector<std::string> CawPartsListing() { return g_listing; }

}  // namespace svr2011

// A part's settings (default colours, its layer...) are a second table, one per category: 28-byte rows by the record's
// texture number, sub_828DDC98 / sub_828DDCC8, built when the editor loads, as long as the stock textures. A mod part's
// number is past its end, and the next category's row made it draw wrong or not at all. It gets a row of its own, a copy
// of the row of the texture its .rec names at +14 (its template).
REX_EXTERN(__imp__sub_828DDC98);
REX_EXTERN(__imp__sub_828DDCC8);

namespace {

uint32_t CawRow(uint8_t* base, uint32_t self, uint32_t cat, uint32_t index) {
  const uint32_t at = self + (cat + 139) * 8;
  const uint32_t count = uint32_t(base[at]) << 8 | base[at + 1];
  const uint32_t table = uint32_t(base[at + 4]) << 24 | uint32_t(base[at + 5]) << 16 | uint32_t(base[at + 6]) << 8 | base[at + 7];
  if (!table || index < count) return 0;
  static std::map<uint64_t, uint32_t> rows;
  const uint64_t key = uint64_t(cat) << 32 | index;
  static std::mutex m;
  std::lock_guard lock(m);
  auto it = rows.find(key);
  if (it != rows.end()) return it->second;
  const auto src = g_row_source.find(key);
  const uint32_t from = src != g_row_source.end() && src->second < count ? src->second : 0;
  const uint32_t row = g_memory ? g_memory->SystemHeapAlloc(28) : 0;
  if (!row) return 0;
  std::memcpy(base + row, base + table + from * 28, 28);
  rows[key] = row;
  REXLOG_INFO("[svr2011] CAW parts: category {} texture {}: a row of its own, from texture {}", cat, index, from);
  return row;
}

}  // namespace

REX_HOOK_RAW(sub_828DDC98) {
  const uint32_t self = ctx.r3.u32, index = ctx.r4.u32;
  __imp__sub_828DDC98(ctx, base);
  const uint32_t cat = uint32_t(base[self + 1640]) << 24 | uint32_t(base[self + 1641]) << 16 | uint32_t(base[self + 1642]) << 8 | base[self + 1643];
  if (const uint32_t row = CawRow(base, self, cat, index)) ctx.r3.u64 = row;
}

REX_HOOK_RAW(sub_828DDCC8) {
  const uint32_t self = ctx.r3.u32, cat = ctx.r4.u32, index = ctx.r5.u32;
  __imp__sub_828DDCC8(ctx, base);
  if (const uint32_t row = CawRow(base, self, cat, index)) ctx.r3.u64 = row;
}
