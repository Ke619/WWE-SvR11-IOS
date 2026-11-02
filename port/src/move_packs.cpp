// Move packs (move_packs.h).
//
// A move is its motions plus a few tables (scratchpad research
// svr10/re_moveport.md):
// - motions: YMKs banks, keyed (id, x = phase/variant, y = track: 0 the
//   attacker, 1 the victim, others props/cameras), in m.pac (MOT/BM.., MOT/RR..
//   and MVMT/.. menu banks) and mpsp.pac MOTP/GAME (the banks a match loads
//   for every superstar). A bank: "YMKs", a 256-byte event-size table at 0x10,
//   u32 count at 0x110, then count x {u8 y, u8 x, u16 id, u32 offset, u32
//   frames, u32 runtime} sorted by key (id<<16 | x<<8 | y) and the motion data
//   in directory order (an entry's size is the next offset minus its own).
// - misc.pac MOVS/WAZE: 160-byte move records (id at +0x90); the first 16
//   bytes are the category bits that make a move selectable in a slot.
// - misc.pac WAZA/DATA children 0 (EXH: 36-byte per-key move data), 1 (per-key
//   sound events: 16-byte records + an event pool) and 2 (MBD: 8-byte records),
//   each a PACH of groups sorted by (u16 id, u8, u8); BATS/INIT and BATH/INIT
//   hold the same three as children 10, 11 and 12.
//
// pack.txt (written by tools/movepack.py), one item a line:
//   motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file> [<20 bytes hex>]
//     (YMBs banks - the ADPCM ones: taunts, submissions, finishers - also take
//     the motion's 20-byte header: type, segments, interval, 0, 4 floats)
//   waze <id> <16 bytes hex>
//   wazename <id> <name>   (the move record's name, e.g. a record 2011 keeps
//     without a motion, given one)
//   wazecopy <id> <from id>   (a new move on a free record: the whole record
//     of another move - category bits, parameters - but its own id; applied
//     before waze / wazename lines)
//   exh <group> <36 bytes hex>
//   evt <group> <16 bytes hex> <events hex>
//   mbd <group> <8 bytes hex>
//   pacentry <pac under pac\, e.g. evt\Nyujyo5.pac> <GROUP> <name> <file>
//     (a whole entry of an EPK8 or EPAC pac, unpacked: e.g. an entrance's script
//     and motions, EVPE/EVTE <number>, or its pyro, prtcls.pac PCE6/LPE6 <0NNN>
//     (4-character names in EPAC); added, or replacing the game's. The pac is
//     copied to the overlay with the entries in - never written in place)
//   propfallback <new prop id> <game prop id>
//     (an entrance prop the pack adds to evt\evtobj.pac SOBJ, e.g. a ported bike:
//     when that copy can't be made, the pack's entrance scripts use the game's
//     prop instead and the other packs still play)
// Keys the game already has are left as they are.
//
// The merged pacs go to <game>/Mods/PacOverlay with copies of pac/plist360.h
// and plist360_4x3.h naming them ("mods\pacoverlay\m.pac"). The game reads
// its pac list from there (arena_mods.cpp: sub_826B9280) and, since the
// pre-built directory plist360.arc only knows the original files, mounts the
// pacs by reading each one's own table (superstar_mods.cpp: sub_825953B0
// instead of sub_82595428). Unchanged entries are streamed from the original
// files; changed ones are re-compressed (real BPE).
#include "move_packs.h"
#include "no_mods.h"
#include "superstar_mods.h"
#include "startup_pump.h"
#include "text_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xam/content_manager.h>

#include "../modmaker/svrfmt/pac.h"

namespace {

namespace fs = std::filesystem;
using svrfmt::Bytes;

constexpr const char* kFormat = "movepacks 14";  // (14: record names cut on a UTF-8 character, bad bytes as '?' - a broken name froze the text draw; 13: pack motions going on to a motion nobody has get the game's own follow-up - every install merged again once; 12: new pacentry names keep the others' places, propfallback; 11: BATS / BATH INIT get the records themselves, not WAZA's tables - a DLC's differ; 2: banks whose sentinel points past the data; 3: WAZE category counts; 4: YMBs banks, record names; 6: WAZA records in id order; 7: of two packs' copies of a motion the first read is used; 8: pacentry lines; 9: YMBs motions appended after the stock ones; 10: pacentry into EPAC pacs (prtcls.pac pyro))
std::string g_folder;  // PacListFolder
constexpr const char* kPropPac = "evt\\evtobj.pac";  // (entrance props: SOBJ <id>)

struct Motion {
  uint16_t id = 0;
  uint8_t x = 0, y = 0;
  uint32_t frames = 0;
  Bytes data;
  Bytes hdr;  // (YMBs: the 20-byte motion header)
  std::string pack;  // (the pack it came from: its folder, or the superstar mod's)
  bool motp = false;  // (a packed copy for a MOTP bank: its frames' event blocks are laid out differently)
};
struct Record {
  uint32_t group = 0;
  Bytes rec, events;
};
struct Packs {
  // pac -> "TYPE/NAME" -> child path -> motions
  std::map<std::string, std::map<std::string, std::map<std::vector<uint32_t>, std::vector<Motion>>>> motions;
  std::map<uint16_t, Bytes> waze;
  std::map<uint16_t, std::string> names;
  std::map<uint16_t, uint16_t> copies;  // record id -> the id its record is copied from
  std::vector<Record> exh, evt, mbd;
  // EPK8 pac (under pac\, lower case, e.g. "evt\nyujyo5.pac") -> "GROUP/name" -> unpacked data
  // (pacentry lines: whole entries added to / replacing the pac's, e.g. an entrance's EVPE/EVTE)
  std::map<std::string, std::map<std::string, Bytes>> entries;
  // every pacentry line read (also without the data): for the entrances the packs add (ModEntrances)
  struct EntryLine {
    std::string pac, group, name;
    fs::path dir, file;
  };
  std::vector<EntryLine> entry_lines;
  std::map<uint32_t, uint32_t> prop_fallback;  // (propfallback lines: an added evtobj prop -> the game's)
  size_t count = 0;
};

uint32_t Le32(const uint8_t* p) { return svrfmt::Le32(p); }
uint16_t Le16(const uint8_t* p) { return svrfmt::Le16(p); }
void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }

Bytes Hex(const std::string& s) {
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

bool ReadAll(const fs::path& p, Bytes& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  out.resize(size_t(f.tellg()));
  f.seekg(0);
  return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

// ---- reading the packs

// The motion ids the enabled packs bring as YMBs (streamed) motions - a
// motion line with a 20-byte header (YmbsInsert) - for the game's "streamed
// motion" flag (sub_82154F00, below). Set as the packs are read (each start,
// merged or not), read in matches.
std::atomic<bool> g_ymbs_motion[0x8000] = {};
// The moves whose own body motion (y 0 / 1) a pack lists but doesn't have the
// file of (an install missing files - a player's mod logged 306 "missing
// motions/..."): such a move "plays" as nothing, and a CPU whose move-set
// held them only reversed. They don't count as usable (MoveUsable), so a
// superstar mod's slots naming them keep the base's moves (superstar_mods.cpp).
// Game moves too: a pack lists a body motion for a move because the game has
// none it plays (Sabu's Enzuigiri 3260 has a 2011 record, the motions are
// 2010's) - at worst a slot keeps the base's move.
std::mutex g_motionless_mutex;
std::set<uint16_t> g_motionless;

// data false: the lists and the stamp only (no motion files read - enough to
// see that the overlay is up to date: thousands of small files on a hard
// drive with a cold cache took 35 s). A missing motion file counts in both
// ways, so the stamps match.
void ReadPack(const fs::path& dir, Packs& p, std::string& stamp, bool data = true) {
  std::ifstream f(dir / "pack.txt");
  std::string line;
  size_t n = 0;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "motion") {
      std::string pac, path, file;
      uint32_t id = 0, x = 0, y = 0, frames = 0;
      std::string hdr;
      in >> pac >> path >> id >> x >> y >> frames >> file >> hdr;
      std::vector<std::string> parts;
      for (size_t a = 0; a <= path.size();) {
        size_t e = path.find('/', a);
        if (e == std::string::npos) e = path.size();
        parts.push_back(path.substr(a, e - a));
        a = e + 1;
      }
      if (parts.size() < 2 || file.empty()) continue;
      std::vector<uint32_t> kids;
      for (size_t k = 2; k < parts.size(); ++k) kids.push_back(uint32_t(std::strtoul(parts[k].c_str(), nullptr, 16)));
      Motion m;
      m.id = uint16_t(id), m.x = uint8_t(x), m.y = uint8_t(y), m.frames = frames;
      m.hdr = Hex(hdr);
      if (m.hdr.size() == 20) g_ymbs_motion[id & 0x7FFF] = true;
      m.pack = (dir.filename() == "moves" ? dir.parent_path() : dir).filename().string();
      m.motp = parts[0] == "MOTP";  // (by its bank: the SvR 2008 ports name their packed copies *_p.ymk, not motp_*)
      // (the motion file in the stamp - size and time, no read: a pack whose
      // motion files changed but not its pack.txt was kept merged as it was)
      {
        std::error_code ec;
        const uintmax_t size = fs::file_size(dir / fs::u8path(file), ec);
        if (ec && y <= 1) {
          std::lock_guard lock(g_motionless_mutex);
          g_motionless.insert(uint16_t(id));
        }
        stamp += file + " " + std::to_string(ec ? 0 : size) + " " +
                 std::to_string(static_cast<long long>(fs::last_write_time(dir / fs::u8path(file), ec).time_since_epoch().count())) + "\n";
      }
      if (data) {
        svr2011::KeepWindowAlive();
        if (!ReadAll(dir / fs::u8path(file), m.data)) {
          REXLOG_WARN("[svr2011] move packs: {}: missing {}", dir.filename().string(), file);
          ++n;
          continue;
        }
      }
      std::string key = parts[0];
      key.resize(4, ' ');
      std::string name = parts[1];
      name.resize(4, ' ');
      p.motions[pac][key + "/" + name][kids].push_back(std::move(m));
    } else if (kind == "waze") {
      uint32_t id = 0;
      std::string hex;
      in >> id >> hex;
      if (const Bytes b = Hex(hex); b.size() == 16) p.waze[uint16_t(id)] = b;
    } else if (kind == "wazecopy") {
      uint32_t id = 0, from = 0;
      in >> id >> from;
      if (id && from && id != from) p.copies[uint16_t(id)] = uint16_t(from);
    } else if (kind == "wazename") {
      uint32_t id = 0;
      std::string name;
      in >> id;
      std::getline(in, name);
      name.erase(0, name.find_first_not_of(' '));
      while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
      if (id && !name.empty()) p.names[uint16_t(id)] = svr2011::CleanUtf8(name, 63);
    } else if (kind == "exh" || kind == "evt" || kind == "mbd") {
      Record r;
      std::string rec, ev;
      in >> r.group >> rec >> ev;
      r.rec = Hex(rec), r.events = Hex(ev);
      const size_t want = kind == "exh" ? 36 : kind == "evt" ? 16 : 8;
      if (r.rec.size() != want) continue;
      (kind == "exh" ? p.exh : kind == "evt" ? p.evt : p.mbd).push_back(std::move(r));
    } else if (kind == "pacentry") {
      std::string pac, group, name, file;
      in >> pac >> group >> name >> file;
      if (group.size() == 3) group += ' ';  // ("EMD": the model group "EMD ", attire mods)
      if (group.size() != 4 || name.empty() || name.size() > 8 || file.empty()) continue;
      for (auto& c : pac) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
      if (pac.find("..") != std::string::npos || pac.size() < 5 || pac.compare(pac.size() - 4, 4, ".pac")) continue;
      Bytes bytes;
      std::error_code ec;
      const uintmax_t size = data ? 0 : fs::file_size(dir / fs::u8path(file), ec);
      if (data ? !ReadAll(dir / fs::u8path(file), bytes) : bool(ec)) {
        REXLOG_WARN("[svr2011] move packs: {}: missing {}", dir.filename().string(), file);
        continue;
      }
      // (the file in the stamp: a changed entry rebuilds the overlay)
      stamp += file + " " + std::to_string(data ? uintmax_t(bytes.size()) : size) + " " +
               std::to_string(static_cast<long long>(fs::last_write_time(dir / fs::u8path(file), ec).time_since_epoch().count())) + "\n";
      p.entries[pac].emplace(group + "/" + name, std::move(bytes));  // (the first pack's wins)
      p.entry_lines.push_back({pac, group, name, dir, dir / fs::u8path(file)});
    } else if (kind == "propfallback") {
      uint32_t id = 0, game_id = 0;
      in >> id >> game_id;
      if (!id || !game_id || id == game_id) continue;
      p.prop_fallback.emplace(id, game_id);
    } else {
      continue;
    }
    ++n;
  }
  p.count += n;
  stamp += dir.string() + " " + std::to_string(n) + "\n";
}

// ---- YMKs banks

uint32_t Key(uint16_t id, uint8_t x, uint8_t y) { return uint32_t(id) << 16 | uint32_t(x) << 8 | y; }

// A motion's end chain: its last event op 49 names the motion that follows it (the victim's down motion, the
// attacker's get-up ...; tools/svr10_moves.py end_chain). A 2010 / 2008 move ported as is can name a motion 2011
// doesn't have (869 in 5786 / 5768 / 5697, 170 in Sabu's 5790): the character is left in no motion - lying in a
// fallback pose until a state reset stands him up ("the mod falls to the floor, then teleports standing", 23 Oct).
// The ids the game's banks and the packs have (built before a merge, KnownMotions); empty: not checked.
std::vector<bool> g_known_motions;

// The frame stream (re_moveport.md): bytes below 0x60 skip frames, 0x60 + u16 size or 0x61.. a record of that size
// minus 0x60. A frame's event block leads its record: BM 0x80 | length, MOTP a byte below 0x40 = length - 1.
// Offset of the last op 49's u16 (0: none); T: the bank's event-size table (0x10..0x110).
size_t EndChainAt(const Bytes& d, const uint8_t* T, bool motp) {
  size_t p = 0, at = 0;
  while (p < d.size()) {
    const uint8_t b = d[p];
    if (b < 0x60) {
      ++p;
      continue;
    }
    size_t sz;
    if (b == 0x60) {
      if (p + 3 > d.size()) return 0;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = size_t(b - 0x60), p += 1;
    }
    if (p + sz > d.size()) return 0;  // (not a frame stream as expected: left as it is)
    if (sz) {
      size_t q = 0, end = 0;
      if (!motp && (d[p] & 0x80)) q = p + 1, end = p + 1 + (d[p] & 0x7F);
      else if (motp && d[p] < 0x40) q = p + 1, end = p + 2 + d[p];
      end = std::min(end, d.size());
      while (q && q < end) {
        const uint8_t op = d[q], len = T[op];
        if (!len || q + len > end) break;
        if (op == 49 && len >= 3) at = q + 1;
        q += len;
      }
    }
    p += sz;
  }
  return at;
}

// Every motion id in a pac's YMKs / YMBs banks (all its entries, every level of their trees).
void CollectMotionIds(const Bytes& blob, std::vector<bool>& ids, int depth = 0) {
  const Bytes raw = svrfmt::Unpack(blob);
  if (raw.size() >= 0x114 && (!std::memcmp(raw.data(), "YMKs", 4) || !std::memcmp(raw.data(), "YMBs", 4))) {
    const uint32_t n = Le32(&raw[0x110]);
    for (uint32_t i = 0; i < n && 0x114 + 16 * size_t(i) + 4 <= raw.size(); ++i) ids[Le16(&raw[0x114 + 16 * i + 2])] = true;
    return;
  }
  std::vector<svrfmt::PachEntry> kids;
  if (depth < 6 && svrfmt::PachRead(raw, kids))
    for (const auto& k : kids) CollectMotionIds(k.data, ids, depth + 1);
}

bool YmbsInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added);

// Inserts the motions (keys the bank lacks) and lays the data out in
// directory order. False if the bank can't take them (not YMKs, unsorted).
bool BankInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added) {
  if (raw.size() >= 0x114 && !std::memcmp(raw.data(), "YMBs", 4)) return YmbsInsert(raw, add, added);
  if (raw.size() < 0x114 || std::memcmp(raw.data(), "YMKs", 4)) return false;
  const uint32_t n = Le32(&raw[0x110]);
  const size_t base = 0x114 + size_t(n) * 16;
  if (base > raw.size() || n == 0) return false;
  struct E {
    uint8_t y, x;
    uint16_t id;
    int64_t off;  // -1: new
    uint32_t frames, rt;
    const uint8_t* data;
    size_t size;
  };
  std::vector<E> ents(n);
  std::vector<uint32_t> offs;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = &raw[0x114 + 16 * i];
    ents[i] = {d[0], d[1], Le16(d + 2), Le32(d + 4), Le32(d + 8), Le32(d + 12), nullptr, 0};
    offs.push_back(uint32_t(ents[i].off));
  }
  std::sort(offs.begin(), offs.end());
  offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
  for (auto& e : ents) {
    const auto it = std::upper_bound(offs.begin(), offs.end(), uint32_t(e.off));
    // (a bank's closing sentinel, id 32767, can point past the data -
    // MVMT/SPE2: it has none)
    const size_t data = raw.size() - base;
    const size_t end = std::min<size_t>(it == offs.end() ? data : *it, data);
    const size_t off = std::min<size_t>(size_t(e.off), data);
    e.data = raw.data() + base + off;
    e.size = end > off ? end - off : 0;
  }
  for (uint32_t i = 0; i + 1 < n; ++i)
    if (ents[i].off > ents[i + 1].off) return false;
  std::map<uint32_t, int> rts;
  for (const auto& e : ents) ++rts[e.rt];
  const uint32_t rt = std::max_element(rts.begin(), rts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  std::vector<uint32_t> keys;
  std::set<uint32_t> present;
  for (const auto& e : ents) keys.push_back(Key(e.id, e.x, e.y)), present.insert(keys.back());
  std::vector<const Motion*> sorted;
  for (const auto& m : add) sorted.push_back(&m);
  std::stable_sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
  added = 0;
  std::vector<std::unique_ptr<Bytes>> fixed;  // (copies whose end chain was cleared: ents point into them)
  for (const Motion* m : sorted) {
    const uint32_t k = Key(m->id, m->x, m->y);
    if (present.count(k)) continue;
    // (above the bank's sentinel: that motion is left out - before, the whole bank was, every pack's motions in it
    // with it: one ported move on record 17013 in MOT/BM00/0, whose sentinel is 16000, took the bank's other
    // packs' moves away too)
    if (m->id >= ents.back().id) {
      REXLOG_WARN("[svr2011] move packs: {}: motion {} x {} y {} - id past this bank's last ({}), left out", m->pack,
                  m->id, m->x, m->y, ents.back().id);
      continue;
    }
    size_t pos = 0;
    while (pos < keys.size() && keys[pos] <= k) ++pos;
    if (pos && keys[pos - 1] > k) return false;
    const uint8_t* data = m->data.data();
    if (!g_known_motions.empty()) {
      if (const size_t at = EndChainAt(m->data, &raw[0x10], m->motp)) {
        const uint16_t next = Le16(&m->data[at]);
        if (next && next < 20000 && m->y < 50 && !g_known_motions[next]) {  // (y 50+ / higher ids: not read as this layout)
          fixed.push_back(std::make_unique<Bytes>(m->data));
          PutLe16(fixed.back()->data() + at, 0);
          data = fixed.back()->data();
          REXLOG_WARN("[svr2011] move packs: {}: motion {} x {} y {} goes on to motion {}, which neither the game nor "
                      "a pack has - cleared (the game's own follow-up instead of none)", m->pack, m->id, m->x, m->y, next);
        }
      }
    }
    keys.insert(keys.begin() + long(pos), k);
    ents.insert(ents.begin() + long(pos), E{m->y, m->x, m->id, -1, m->frames, rt, data, m->data.size()});
    present.insert(k);
    ++added;
  }
  if (!added) return true;
  Bytes body, dir;
  int64_t last_old = -2;
  uint32_t last_new = 0;
  for (const auto& e : ents) {
    uint32_t off;
    if (e.off >= 0 && e.off == last_old) {
      off = last_new;  // (neighbours sharing one block keep sharing it)
    } else {
      off = uint32_t(body.size());
      body.insert(body.end(), e.data, e.data + e.size);
    }
    last_old = e.off, last_new = off;
    uint8_t d[16];
    d[0] = e.y, d[1] = e.x;
    PutLe16(d + 2, e.id);
    svrfmt::PutLe32(d + 4, off);
    svrfmt::PutLe32(d + 8, e.frames);
    svrfmt::PutLe32(d + 12, e.rt);
    dir.insert(dir.end(), d, d + 16);
  }
  Bytes out(raw.begin(), raw.begin() + 0x110);
  svrfmt::AppLe32(out, uint32_t(ents.size()));
  svrfmt::App(out, dir);
  svrfmt::App(out, body);
  raw = std::move(out);
  return true;
}

// A YMBs bank (re_charmodel.md): the same directory, then a database - u32,
// a 20-byte header per motion (same order), the data - with the directory
// offsets counted from the database. New motions bring their header.
bool YmbsInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added) {
  const uint32_t n = Le32(&raw[0x110]);
  const size_t db = 0x114 + size_t(n) * 16;
  if (n == 0 || db + 4 + 20 * size_t(n) > raw.size()) return false;
  struct E {
    uint8_t y, x;
    uint16_t id;
    int64_t off;  // (from the database; -1: new)
    uint32_t frames, rt;
    const uint8_t* hdr;
    const uint8_t* data;
    size_t size;
  };
  const size_t data_size = raw.size() - db;
  std::vector<E> ents(n);
  std::vector<uint32_t> offs;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = &raw[0x114 + 16 * i];
    ents[i] = {d[0], d[1], Le16(d + 2), Le32(d + 4), Le32(d + 8), Le32(d + 12), &raw[db + 4 + 20 * i], nullptr, 0};
    offs.push_back(uint32_t(ents[i].off));
  }
  std::sort(offs.begin(), offs.end());
  offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
  for (auto& e : ents) {
    const auto it = std::upper_bound(offs.begin(), offs.end(), uint32_t(e.off));
    const size_t end = std::min<size_t>(it == offs.end() ? data_size : *it, data_size);
    const size_t off = std::min<size_t>(size_t(e.off), data_size);
    e.data = raw.data() + db + off;
    e.size = end > off ? end - off : 0;
  }
  std::map<uint32_t, int> rts;
  for (const auto& e : ents) ++rts[e.rt];
  const uint32_t rt = std::max_element(rts.begin(), rts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  std::vector<uint32_t> keys;
  std::set<uint32_t> present;
  for (const auto& e : ents) keys.push_back(Key(e.id, e.x, e.y)), present.insert(keys.back());
  for (size_t i = 0; i + 1 < keys.size(); ++i)
    if (keys[i] > keys[i + 1]) return false;
  std::vector<const Motion*> sorted;
  for (const auto& m : add) sorted.push_back(&m);
  std::stable_sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
  added = 0;
  for (const Motion* m : sorted) {
    const uint32_t k = Key(m->id, m->x, m->y);
    if (present.count(k) || m->hdr.size() != 20) continue;
    // (above the bank's sentinel: that motion is left out - before, the whole bank was, every pack's motions in it
    // with it: one ported move on record 17013 in MOT/BM00/0, whose sentinel is 16000, took the bank's other
    // packs' moves away too)
    if (m->id >= ents.back().id) {
      REXLOG_WARN("[svr2011] move packs: {}: motion {} x {} y {} - id past this bank's last ({}), left out", m->pack,
                  m->id, m->x, m->y, ents.back().id);
      continue;
    }
    // Appended after the stock entries (before the end mark), never between
    // them: the game keeps YMBs motions by their place in the bank, so one put
    // in front shifted every stock motion after it - any superstar's Camel
    // Clutch 2 (21450) then read another motion's data and crashed once
    // Sabu's ported camel clutch (5962) went in ahead of it.
    size_t pos = keys.size();
    while (pos > 0 && ents[pos - 1].id == ents.back().id) --pos;
    keys.insert(keys.begin() + long(pos), k);
    ents.insert(ents.begin() + long(pos), E{m->y, m->x, m->id, -1, m->frames, rt, m->hdr.data(), m->data.data(), m->data.size()});
    present.insert(k);
    ++added;
  }
  if (!added) return true;
  const size_t start = 4 + 20 * ents.size();  // (the data's offset in the new database)
  Bytes body, dir, hdrs;
  int64_t last_old = -2;
  uint32_t last_new = 0;
  for (const auto& e : ents) {
    uint32_t off;
    if (e.off >= 0 && e.off == last_old) {
      off = last_new;  // (neighbours sharing one block keep sharing it)
    } else {
      off = uint32_t(start + body.size());
      body.insert(body.end(), e.data, e.data + e.size);
    }
    last_old = e.off, last_new = off;
    uint8_t d[16];
    d[0] = e.y, d[1] = e.x;
    PutLe16(d + 2, e.id);
    svrfmt::PutLe32(d + 4, off);
    svrfmt::PutLe32(d + 8, e.frames);
    svrfmt::PutLe32(d + 12, e.rt);
    dir.insert(dir.end(), d, d + 16);
    hdrs.insert(hdrs.end(), e.hdr, e.hdr + 20);
  }
  Bytes out(raw.begin(), raw.begin() + 0x110);
  svrfmt::AppLe32(out, uint32_t(ents.size()));
  svrfmt::App(out, dir);
  // (the database's word: the header size << 16 | the motion count - stock
  // 0x140138 for 312 - the new count, not the old one)
  svrfmt::AppLe32(out, (Le32(&raw[db]) & 0xFFFF0000u) | uint32_t(ents.size() & 0xFFFF));
  svrfmt::App(out, hdrs);
  svrfmt::App(out, body);
  raw = std::move(out);
  return true;
}

// ---- PACH trees

Bytes Repack(const Bytes& raw, bool was_bpe) { return was_bpe ? svrfmt::BpeEncode(raw) : raw; }

// Applies `edit` to the descendant of `blob` at child ids `path` (each level
// re-packed as it was). False if a level is missing.
template <typename F>
bool EditTree(Bytes& blob, const std::vector<uint32_t>& path, size_t depth, F&& edit) {
  const bool bpe = svrfmt::IsBpe(blob);
  Bytes raw = svrfmt::Unpack(blob);
  if (depth == path.size()) {
    if (!edit(raw)) return false;
    blob = Repack(raw, bpe);
    return true;
  }
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(raw, kids)) return false;
  bool done = false;
  for (auto& k : kids)
    if (k.id == path[depth]) done = EditTree(k.data, path, depth + 1, edit);
  if (!done) return false;
  blob = Repack(svrfmt::PachWrite(kids), bpe);
  return true;
}

// ---- record tables (misc.pac)

uint32_t RecKey(const uint8_t* r) { return uint32_t(Le16(r)) << 16 | uint32_t(r[2]) << 8 | r[3]; }

// Inserts records (header of `head` bytes, `size`-byte records, count u32 at
// +4) in move id order (after the id's own records; appended only if the
// table isn't in id order); keys (id, x, b3) present stay. The game finds a
// record by a binary search on the u16 id alone (sub_826A4960): its tables
// are sorted by id, not always by the whole key (EXH group 16 has 845/0/1
// before 845/0/0), and records appended there after the last id - ported
// strikes - left them and every later stock id unfound: no hit reaction or
// impact sound (2.0.4 report, the SvR2010 superstars' strikes).
void InsertRecords(Bytes& t, size_t head, size_t size, std::vector<Bytes> add, Bytes* pool = nullptr,
                   std::vector<Bytes>* events = nullptr) {
  const uint32_t n = Le32(&t[4]);
  std::vector<Bytes> recs;
  for (uint32_t i = 0; i < n; ++i) recs.emplace_back(t.begin() + long(head + i * size), t.begin() + long(head + (i + 1) * size));
  Bytes tail(t.begin() + long(head + n * size), t.end());
  bool sorted = true;
  for (size_t i = 0; i + 1 < recs.size(); ++i) sorted &= Le16(recs[i].data()) <= Le16(recs[i + 1].data());
  std::set<uint32_t> have;
  for (const auto& r : recs) have.insert(RecKey(r.data()));
  for (size_t a = 0; a < add.size(); ++a) {
    Bytes r = add[a];
    if (have.count(RecKey(r.data()))) continue;
    if (pool && events) {  // (an event record: its events appended to the pool)
      PutLe16(&r[8], uint16_t(tail.size() / 4));
      r[10] = uint8_t((*events)[a].size() / 4);
      svrfmt::App(tail, (*events)[a]);
    }
    have.insert(RecKey(r.data()));
    if (!sorted) {
      recs.push_back(r);
      continue;
    }
    size_t pos = 0;
    while (pos < recs.size() && Le16(recs[pos].data()) <= Le16(r.data())) ++pos;
    recs.insert(recs.begin() + long(pos), r);
  }
  Bytes out(t.begin(), t.begin() + long(head));
  svrfmt::PutLe32(&out[4], uint32_t(recs.size()));
  for (const auto& r : recs) svrfmt::App(out, r);
  svrfmt::App(out, tail);
  t = std::move(out);
}

// One WAZA/DATA child (a PACH of groups) with the pack's records added.
bool EditGroups(Bytes& child, const std::vector<Record>& recs, int kind) {
  std::map<uint32_t, std::vector<const Record*>> by;
  for (const auto& r : recs) by[r.group].push_back(&r);
  if (by.empty()) return true;
  const bool bpe = svrfmt::IsBpe(child);
  std::vector<svrfmt::PachEntry> groups;
  if (!svrfmt::PachRead(svrfmt::Unpack(child), groups)) return false;
  for (auto& g : groups) {
    auto it = by.find(g.id);
    if (it == by.end()) continue;
    const bool gb = svrfmt::IsBpe(g.data);
    Bytes t = svrfmt::Unpack(g.data);
    std::vector<Bytes> add, events;
    for (const Record* r : it->second) add.push_back(r->rec), events.push_back(r->events);
    if (kind == 0 && t.size() >= 8 && !std::memcmp(t.data(), "EXH", 4)) InsertRecords(t, 8, 36, add);
    else if (kind == 1 && t.size() >= 8 && Le32(t.data()) == 16) InsertRecords(t, 8, 16, add, &t, &events);
    else if (kind == 2 && t.size() >= 8 && !std::memcmp(t.data(), "MBD", 4)) InsertRecords(t, 8, 8, add);
    else continue;
    g.data = Repack(t, gb);
  }
  child = Repack(svrfmt::PachWrite(groups), bpe);
  return true;
}

// ---- EPAC streaming

struct TocEntry {
  std::string type, name;
  uint32_t sector, size256;
};

bool ReadToc(std::ifstream& f, Bytes& head, std::vector<TocEntry>& toc) {
  head.resize(0x4000);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(head.data()), 0x4000) || std::memcmp(head.data(), "EPAC", 4)) return false;
  for (size_t p = 0x800; p + 12 <= 0x4000 && Le32(&head[p]);) {
    const std::string type(reinterpret_cast<char*>(&head[p]), 4);
    const uint32_t cnt = Le32(&head[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12)
      toc.push_back({type, std::string(reinterpret_cast<char*>(&head[p]), 4), Le32(&head[p + 4]), Le32(&head[p + 8])});
  }
  return true;
}

// <dst>.part, written whole, in place of dst: a game starting meanwhile never reads half a pac, and an
// overlay entry that is a hard link to a game file is replaced, never written through.
fs::path Part(const fs::path& dst) {
  fs::path p = dst;
  p += ".part";
  return p;
}
bool Commit(const fs::path& part, const fs::path& dst) {
  std::error_code ec;
  fs::rename(part, dst, ec);
  if (ec) {
    REXLOG_WARN("[svr2011] move packs: cannot replace {} ({})", dst.string(), ec.message());
    fs::remove(part, ec);
    return false;
  }
  return true;
}

// `data` as `dst` through its .part, only when written whole (a full disk: the old file - or none - stays, never a
// short one).
bool WriteWhole(const fs::path& dst, const Bytes& data) {
  const fs::path part = Part(dst);
  bool ok;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close();
    ok = bool(out);
  }
  if (!ok) {
    std::error_code ec;
    fs::remove(part, ec);
    REXLOG_WARN("[svr2011] move packs: cannot write {}", dst.string());
    return false;
  }
  return Commit(part, dst);
}

// Writes `src` to `dst` with the entries in `changed` ("TYPE/NAME") replaced,
// laid out as svrfmt::EpacWrite does.
bool WritePacTo(const fs::path& src, const fs::path& dst, const std::map<std::string, Bytes>& changed);
bool WritePac(const fs::path& src, const fs::path& dst, const std::map<std::string, Bytes>& changed) {
  const fs::path part = Part(dst);
  if (!WritePacTo(src, part, changed)) {
    std::error_code ec;
    fs::remove(part, ec);
    return false;
  }
  return Commit(part, dst);
}
bool WritePacTo(const fs::path& src, const fs::path& dst, const std::map<std::string, Bytes>& changed) {
  std::ifstream in(src, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!in || !ReadToc(in, head, toc)) return false;
  in.seekg(0, std::ios::end);
  const uint64_t src_size = uint64_t(in.tellg());
  uint64_t src_data_end = 0x4000 + uint64_t(Le32(&head[8]));
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  Bytes zeros(0x800, 0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);  // (the table is rewritten at the end)
  uint64_t pos = 0;  // within the data
  Bytes newtoc;
  std::string group;
  size_t group_at = 0;
  std::vector<char> buf(1 << 20);
  for (size_t i = 0; i < toc.size(); ++i) {
    const auto& e = toc[i];
    if (e.type != group) {
      group = e.type;
      group_at = newtoc.size();
      svrfmt::AppName(newtoc, e.type, 4);
      svrfmt::AppLe32(newtoc, 0);
      svrfmt::AppLe32(newtoc, 0);
    }
    svrfmt::PutLe32(&newtoc[group_at + 4], Le32(&newtoc[group_at + 4]) + 3);
    if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
    svrfmt::AppName(newtoc, e.name, 4);
    svrfmt::AppLe32(newtoc, uint32_t(pos / 0x800));
    const auto it = changed.find(e.type + "/" + e.name);
    uint64_t size;
    if (it != changed.end()) {
      out.write(reinterpret_cast<const char*>(it->second.data()), std::streamsize(it->second.size()));
      size = it->second.size();
    } else {
      size = uint64_t(e.size256) * 0x100;
      in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
      for (uint64_t left = size; left;) {
        svr2011::KeepWindowAlive();  // (a big pac or bank takes seconds: the window keeps answering)
        const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        if (!in.read(buf.data(), std::streamsize(n))) return false;
        out.write(buf.data(), std::streamsize(n));
        left -= n;
      }
    }
    svrfmt::AppLe32(newtoc, uint32_t((size + 0xFF) / 0x100));
    pos += size;
    if (pos % 0x100) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x100 - pos % 0x100)), pos += 0x100 - pos % 0x100;
  }
  if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
  // the packer's footer after the data, as the original has it
  if (src_data_end < src_size) {
    in.seekg(std::streamoff(src_data_end));
    std::vector<char> tail(size_t(src_size - src_data_end));
    if (in.read(tail.data(), std::streamsize(tail.size()))) out.write(tail.data(), std::streamsize(tail.size()));
  } else {
    out.write(reinterpret_cast<char*>(zeros.data()), 0x800);
  }
  newtoc.resize(0x3800, 0);
  std::memcpy(&head[0x800], newtoc.data(), 0x3800);
  svrfmt::PutLe32(&head[8], uint32_t(pos));
  out.seekp(0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);
  out.close();
  return bool(out);
}

// ---- EPK8 (evt\Nyujyo*.pac): pacentry entries added
// Table at 0x800-0x4000: groups {4CC, u16 count * 4, u16 1, u32 0} each followed
// by 16-byte entries {char name[8] (space padded), u32 sector (0x800 units from
// 0x4000), u32 size / 0x100}; header +4 = the table's size, +8 = the data's.
// Names are numbers kept in numeric order, "65535" (an end mark) last. The
// added entries are BPE-packed as the game's are.

// A pac list name ("evt\nyujyo5.pac") as a path on this system (Android: '/').
fs::path PacPath(std::string pac) {
  std::replace(pac.begin(), pac.end(), '\\', '/');
  return fs::u8path(pac);
}

uint64_t Epk8Order(const std::string& name) {
  const std::string n = name.substr(0, name.find(' '));
  if (n == "65535") return ~0ull;
  return n.empty() || n.find_first_not_of("0123456789") != std::string::npos ? ~0ull - 1 : std::stoull(n);
}

bool BuildEntryPac(const fs::path& game, const fs::path& overlay, const std::string& pac,
                   const std::map<std::string, Bytes>& add) {
  const fs::path src = game / "pac" / PacPath(pac), dst = overlay / PacPath(pac);
  std::ifstream in(src, std::ios::binary);
  Bytes head(0x4000);
  if (!in || !in.read(reinterpret_cast<char*>(head.data()), 0x4000) ||
      (std::memcmp(head.data(), "EPK8", 4) && std::memcmp(head.data(), "EPAC", 4))) {
    REXLOG_WARN("[svr2011] move packs: {} is not an EPK8 / EPAC pac", pac);
    return false;
  }
  // EPAC (prtcls.pac: an entrance's pyro PCE6/LPE6 <0NNN>): groups {4CC, u32 count * 3, u32 0},
  // 12-byte entries {char name[4], u32 sector, u32 size / 0x100}; added entries are packed only
  // when the group's own are (prtcls.pac's are plain PACH)
  const bool epac = !std::memcmp(head.data(), "EPAC", 4);
  const size_t name_len = epac ? 4 : 8, ent_len = name_len + 8;
  struct E {
    std::string name;
    uint32_t sector = 0, size256 = 0;
    const Bytes* data = nullptr;  // (an added entry, packed)
  };
  struct G {
    std::string type;
    Bytes extra;
    std::vector<E> ents;
  };
  std::vector<G> groups;
  for (size_t q = 0x800; q + 12 <= 0x4000 && Le32(&head[q]);) {
    G g;
    g.type.assign(reinterpret_cast<char*>(&head[q]), 4);
    const uint32_t n = epac ? Le32(&head[q + 4]) / 3 : Le16(&head[q + 4]) / 4;
    g.extra.assign(head.begin() + long(q + (epac ? 8 : 6)), head.begin() + long(q + 12));
    q += 12;
    for (uint32_t i = 0; i < n && q + ent_len <= 0x4000; ++i, q += ent_len)
      g.ents.push_back({std::string(reinterpret_cast<char*>(&head[q]), name_len), Le32(&head[q + name_len]),
                        Le32(&head[q + name_len + 4])});
    groups.push_back(std::move(g));
  }
  std::map<std::string, Bytes> packed;
  for (const auto& [key, raw] : add) {
    const std::string type = key.substr(0, 4), name = (key.substr(5) + "        ").substr(0, name_len);
    auto g = std::find_if(groups.begin(), groups.end(), [&](const G& x) { return x.type == type; });
    if (g == groups.end()) {
      REXLOG_WARN("[svr2011] move packs: {} has no group {}", pac, type);
      continue;
    }
    bool bpe = !epac;
    auto e = std::find_if(g->ents.begin(), g->ents.end(), [&](const E& x) { return x.name == name; });
    // (stored as the group's own entries are: evt's EPK8 entries are BPE-packed, a
    // model pac's EMD entries plain PACHs - BPE-packed, the game couldn't load one. The
    // entry it replaces, else the first numbered one: evtobj.pac's LIST is packed, its
    // props plain PACHs, read in place - a packed prop crashed the entrance)
    auto like = e != g->ents.end() ? e : std::find_if(g->ents.begin(), g->ents.end(), [](const E& x) {
      return Epk8Order(x.name) < ~0ull - 1;
    });
    if (like == g->ents.end()) like = g->ents.begin();
    if (like != g->ents.end()) {
      char magic[4] = {};
      in.seekg(std::streamoff(0x4000 + uint64_t(like->sector) * 0x800));
      bpe = in.read(magic, 4) && !std::memcmp(magic, "BPE ", 4);
      in.clear();
    }
    const Bytes& data = packed[key] = bpe ? svrfmt::BpeEncode(raw) : raw;
    if (e != g->ents.end()) {
      e->data = &data;
      continue;
    }
    // a new name goes in among the numbers, before the first greater one (else after the last): the
    // other entries keep their places (evtobj.pac: LIST first, F000.. after the numbers; EPK8: "65535" last)
    const uint64_t order = Epk8Order(name);
    auto at = g->ents.end();
    for (auto it = g->ents.begin(); it != g->ents.end(); ++it) {
      const uint64_t o = Epk8Order(it->name);
      if (o >= ~0ull - 1 && o != ~0ull) continue;  // (a name that isn't a number)
      if (o > order) {
        at = it;
        break;
      }
      at = std::next(it);
    }
    g->ents.insert(at, {name, 0, 0, &data});
  }
  in.seekg(0, std::ios::end);
  const uint64_t src_size = uint64_t(in.tellg()), src_end = 0x4000 + uint64_t(Le32(&head[8]));
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  const fs::path part = Part(dst);
  std::ofstream out(part, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);  // (the table is rewritten at the end)
  Bytes table, zeros(0x800, 0);
  std::vector<char> buf(1 << 20);
  uint64_t pos = 0;
  for (const auto& g : groups) {
    svrfmt::AppName(table, g.type, 4);
    if (epac) svrfmt::AppLe32(table, uint32_t(g.ents.size() * 3));
    else table.push_back(uint8_t(g.ents.size() * 4)), table.push_back(uint8_t(g.ents.size() * 4 >> 8));
    table.insert(table.end(), g.extra.begin(), g.extra.end());
    for (const auto& e : g.ents) {
      if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
      table.insert(table.end(), e.name.begin(), e.name.end());
      svrfmt::AppLe32(table, uint32_t(pos / 0x800));
      uint64_t size;
      if (e.data) {
        out.write(reinterpret_cast<const char*>(e.data->data()), std::streamsize(e.data->size()));
        size = e.data->size();
      } else {
        size = uint64_t(e.size256) * 0x100;
        in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
        for (uint64_t left = size; left;) {
          svr2011::KeepWindowAlive();  // (a big pac or bank takes seconds: the window keeps answering)
          const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
          if (!in.read(buf.data(), std::streamsize(n))) return false;
          out.write(buf.data(), std::streamsize(n));
          left -= n;
        }
      }
      svrfmt::AppLe32(table, uint32_t((size + 0xFF) / 0x100));
      pos += size;
      if (pos % 0x100) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x100 - pos % 0x100)), pos += 0x100 - pos % 0x100;
    }
  }
  if (table.size() > 0x3800) {
    REXLOG_WARN("[svr2011] move packs: {}: too many entries for the table", pac);
    return false;
  }
  if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
  if (src_end < src_size) {  // (the packer's footer, as the original has it)
    in.seekg(std::streamoff(src_end));
    std::vector<char> tail(size_t(src_size - src_end));
    if (in.read(tail.data(), std::streamsize(tail.size()))) out.write(tail.data(), std::streamsize(tail.size()));
  } else {
    out.write(reinterpret_cast<char*>(zeros.data()), 0x800);
  }
  const uint32_t table_size = uint32_t(table.size());
  table.resize(0x3800, 0);
  std::memcpy(&head[0x800], table.data(), 0x3800);
  svrfmt::PutLe32(&head[4], table_size);
  svrfmt::PutLe32(&head[8], uint32_t(pos));
  out.seekp(0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);
  out.close();
  if (!out) {
    fs::remove(part, ec);
    return false;
  }
  if (!Commit(part, dst)) return false;
  REXLOG_INFO("[svr2011] move packs: {}: {} entries added or replaced", pac, add.size());
  return true;
}

// An entrance script (EVP?: "0FOP", big-endian; object count +0x40 low half, table at +0x48 + 8, 32-byte
// objects {u32 idx, u32 model, u32 type x2, ...}): its props (type 00000100 0) on an added evtobj id
// moved to the game's (propfallback). -> how many.
int FallBackProps(Bytes& d, const std::map<uint32_t, uint32_t>& fallback) {
  const auto be32 = [&](size_t o) { return uint32_t(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
  if (d.size() < 0x4C || std::memcmp(d.data(), "0FOP", 4)) return 0;
  const uint32_t n = be32(0x40) & 0xFFFF;
  const size_t t = size_t(be32(0x48)) + 8;
  int moved = 0;
  for (uint32_t i = 0; i < n && t + 32 * (i + 1) <= d.size(); ++i) {
    const size_t q = t + 32 * i;
    const auto f = fallback.find(be32(q + 4));
    if (f == fallback.end() || be32(q + 8) != 0x100 || be32(q + 12) != 0) continue;
    for (int k = 0; k < 4; ++k) d[q + 4 + k] = uint8_t(f->second >> (24 - 8 * k));
    ++moved;
  }
  return moved;
}

bool ReadEntry(const fs::path& pac, const std::string& type_name, Bytes& out) {
  std::ifstream in(pac, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!in || !ReadToc(in, head, toc)) return false;
  for (const auto& e : toc)
    if (e.type + "/" + e.name == type_name) {
      out.resize(size_t(e.size256) * 0x100);
      in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
      return bool(in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
    }
  return false;
}

// Two packs with the same motion key: the first read is used (packs are read
// in path order - Mods\Moves\<pack> before Mods\Superstars\<mod>\moves, then
// A-Z), the other is ignored. Copies that are the same (one ported move in
// several superstar mods) are normal; different ones are logged, once per
// pair of packs, with the move ids: an old exported move pack can hide a
// mod's fixed motion that way.
void WarnOverrides(const Packs& p) {
  std::map<std::pair<std::string, std::string>, std::set<uint16_t>> diff;  // (used, ignored) -> move ids
  for (const auto& [pac, entries] : p.motions)
    for (const auto& [type_name, banks] : entries)
      for (const auto& [path, motions] : banks) {
        std::map<uint32_t, const Motion*> first;
        for (const Motion& m : motions) {
          const auto [it, fresh] = first.emplace(Key(m.id, m.x, m.y), &m);
          if (!fresh && it->second->pack != m.pack && it->second->data != m.data)
            diff[{it->second->pack, m.pack}].insert(m.id);
        }
      }
  for (const auto& [packs, ids] : diff) {
    std::string list;
    for (uint16_t id : ids) list += (list.empty() ? "" : " ") + std::to_string(id);
    REXLOG_WARN("[svr2011] move packs: \"{}\" and \"{}\" carry different motions for moves {}: \"{}\"'s are used",
                packs.first, packs.second, list, packs.first);
  }
}

// ---- building

using MotionEntries = std::map<std::string, std::map<std::vector<uint32_t>, std::vector<Motion>>>;

// src with the packs' motions in its banks, to dst. dlc: a DLC's copy of the pac (only some of its entries:
// the others aren't looked for). *changed_entries: the entries it has of those the packs add.
bool BuildMotionFile(const fs::path& src, const fs::path& dst, const std::string& pac, const MotionEntries& entries,
                     bool dlc = false, size_t* changed_entries = nullptr) {
  std::map<std::string, Bytes> changed;
  size_t total = 0;
  for (const auto& [type_name, banks] : entries) {
    Bytes blob;
    if (!ReadEntry(src, type_name, blob)) {
      if (!dlc) REXLOG_WARN("[svr2011] move packs: {} has no {}", pac, type_name);
      continue;
    }
    for (const auto& [path, motions] : banks) {
      svr2011::KeepWindowAlive();  // (a big pac or bank takes seconds: the window keeps answering)
      size_t added = 0;
      if (!EditTree(blob, path, 0, [&](Bytes& raw) { return BankInsert(raw, motions, added); })) {
        REXLOG_WARN("[svr2011] move packs: {} {}: bank not changed", pac, type_name);
        continue;
      }
      total += added;
    }
    changed[type_name] = std::move(blob);
  }
  if (changed_entries) *changed_entries = changed.size();
  if (dlc && changed.empty()) return true;  // (none of the packs' banks in this DLC pac)
  // (mpsp.pac MOTP/GAME, 16.4 MB stock, is read whole into the file loader's cache - 20 MB, heap 16 - at the first
  // match: grown past it by packs' motions the match load hung there in 2.1.4.1, 21 Oct report; model_mem.cpp
  // gives it fallback memory now. The size in the log says so.)
  for (const auto& [type_name, blob] : changed)
    if (blob.size() >= (16u << 20))
      REXLOG_INFO("[svr2011] move packs: {} {} is {:.1f} MB{}", pac, type_name, blob.size() / 1048576.0,
                  type_name == "MOTP/GAME" && blob.size() >= (19u << 20)
                      ? " - more than the game's 20 MB file cache can take beside the match's other files (it loads "
                        "from the fallback memory)"
                      : "");
  if (!WritePac(src, dst, changed)) return false;
  REXLOG_INFO("[svr2011] move packs: {}: {} motions added", pac, total);
  return true;
}

bool BuildMotionPac(const fs::path& game, const fs::path& overlay, const std::string& pac, const MotionEntries& entries) {
  return BuildMotionFile(game / "pac" / pac, overlay / pac, pac, entries);
}

// MOVS/WAZE as in the file (little-endian: u32 255, u32 record count, 128
// u16 category counts, then 160-byte records from +272 - the first "* test
// motion *" - each with its category bits, u64s at +0 / +8, and its move id
// at +0x90): the packs' moves get their 16 bytes of bits, and the category
// counts are recounted. Those counts size the game's move lists (sub_8237BA68
// allocates by them, then writes every member): stock counts with new bits
// overran the lists and lost the ported moves at their ends. Returns the
// records set (-1: not that layout); *fixed: the counts changed.
int PatchWaze(uint8_t* w, size_t size, const std::map<uint16_t, Bytes>& waze, int* fixed,
              const std::map<uint16_t, std::string>& names = {}, const std::map<uint16_t, uint16_t>& copies = {}) {
  static const char kMark[] = "* test motion *";
  if (size < 272 + 160 || std::memcmp(w + 272 + 16, kMark, sizeof kMark - 1)) return -1;
  const uint32_t records = Le32(w + 4);
  if (size_t(records) * 160 + 272 > size) return -1;
  int set = 0;
  if (!copies.empty()) {  // (whole records of other moves, the id kept)
    std::map<uint16_t, uint8_t*> by_id;
    for (uint32_t k = 0; k < records; ++k) {
      uint8_t* r = w + 272 + size_t(k) * 160;
      if (!std::all_of(r, r + 160, [](uint8_t b) { return b == 0; })) by_id.emplace(Le16(r + 0x90), r);
    }
    for (const auto& [id, from] : copies) {
      const auto d = by_id.find(id), f = by_id.find(from);
      if (d == by_id.end() || f == by_id.end()) continue;
      uint8_t rec[160];
      std::memcpy(rec, f->second, 160);
      std::memcpy(rec + 0x90, d->second + 0x90, 2);
      if (std::memcmp(d->second, rec, 160)) std::memcpy(d->second, rec, 160), ++set;
    }
  }
  for (uint32_t k = 0; k < records; ++k) {
    uint8_t* r = w + 272 + size_t(k) * 160;
    if (std::all_of(r, r + 160, [](uint8_t b) { return b == 0; })) continue;
    const uint16_t id = Le16(r + 0x90);
    if (const auto nm = names.find(id); nm != names.end()) {  // (the name: 64 bytes at +0x10, "MOD: " first - below)
      uint8_t buf[64] = {};
      // (cut on a character boundary, bad bytes as '?': a broken UTF-8 name froze the game's text draw - text_guard.cpp)
      const std::string want =
          svr2011::CleanUtf8(nm->second.rfind("MOD: ", 0) == 0 ? nm->second : "MOD: " + nm->second, 63);
      std::memcpy(buf, want.data(), want.size());
      if (std::memcmp(r + 0x10, buf, 64)) std::memcpy(r + 0x10, buf, 64), ++set;
    }
    const auto f = waze.find(id);
    if (f != waze.end() && std::memcmp(r, f->second.data(), 16)) std::memcpy(r, f->second.data(), 16), ++set;
    // (a pack's move is named "MOD: <name>" wherever the game lists it - as CREATE AN ENTRANCE's MOD: rows; once:
    // the overlay's records come through here again)
    if (names.count(id) || copies.count(id) || f != waze.end()) {
      static const char kMod[] = "MOD: ";
      char* nm = reinterpret_cast<char*>(r + 0x10);
      if (nm[0] && std::strncmp(nm, kMod, 5)) {
        char buf[64] = {};
        const std::string text = svr2011::CleanUtf8(kMod + std::string(nm, strnlen(nm, 64)), 63);
        std::memcpy(buf, text.data(), text.size());
        std::memcpy(r + 0x10, buf, 64), ++set;
      }
    }
  }
  uint32_t count[128] = {};
  for (uint32_t k = 0; k < records; ++k)
    for (int c = 0; c < 128; ++c)
      if (w[272 + size_t(k) * 160 + (c >> 3)] >> (c & 7) & 1) ++count[c];
  *fixed = 0;
  for (int c = 0; c < 128; ++c) {
    const uint16_t n = uint16_t(std::min<uint32_t>(count[c], 0xFFFF));
    *fixed += Le16(w + 8 + 2 * c) != n;
    PutLe16(w + 8 + 2 * c, n);
  }
  return set;
}

std::map<uint16_t, Bytes> g_waze;  // the packs' moves' category bits (PatchWaze at each MOVS/WAZE load)
// Each pack's moves ((kind "Moves" / "Superstars", the mod's folder) -> ids): online lobbies agree which
// packs every player has; a match's move from another pack plays its category's default (MovePacks).
std::map<std::pair<std::string, std::string>, std::set<uint16_t>> g_pack_ids;
std::mutex g_pack_ids_mutex;
std::map<uint16_t, std::string> g_names;  // and their record names
std::map<uint16_t, uint16_t> g_copies;     // and records copied from other moves

bool BuildMiscFile(const fs::path& src, const fs::path& dst, const Packs& p) {
  std::map<std::string, Bytes> changed;
  if (!p.waze.empty() || !p.names.empty() || !p.copies.empty()) {
    Bytes blob;
    if (!ReadEntry(src, "MOVS/WAZE", blob)) return false;
    const bool bpe = svrfmt::IsBpe(blob);
    Bytes w = svrfmt::Unpack(blob);
    int fixed = 0;
    const int set = PatchWaze(w.data(), w.size(), p.waze, &fixed, p.names, p.copies);
    if (set < 0) return false;
    changed["MOVS/WAZE"] = Repack(w, bpe);
    REXLOG_INFO("[svr2011] move packs: {} move records made selectable, {} category counts updated", set, fixed);
  }
  if (!p.exh.empty() || !p.evt.empty() || !p.mbd.empty()) {
    Bytes waza;
    if (!ReadEntry(src, "WAZA/DATA", waza)) return false;
    Bytes c[3];
    {
      const bool bpe = svrfmt::IsBpe(waza);
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(waza), kids)) return false;
      for (auto& k : kids)
        if (k.id < 3) {
          if (!EditGroups(k.data, k.id == 0 ? p.exh : k.id == 1 ? p.evt : p.mbd, int(k.id))) return false;
          c[k.id] = k.data;
        }
      changed["WAZA/DATA"] = Repack(svrfmt::PachWrite(kids), bpe);
    }
    // BATS / BATH INIT children 10-12: the same three tables - in the game's misc.pac byte for byte WAZA's,
    // in a DLC's not (its WAZA has the DLC's moves, 51000 / 270488 bytes, its BATS 39284 / 205980): copying
    // WAZA's over them (2.1.3) changed the stock moves' data in matches - wrestlers fell and got up at once
    // (players' reports 2026-10-19). Each gets the packs' records itself; one that can't stays as it is.
    for (const char* nm : {"BATS/INIT", "BATH/INIT"}) {
      Bytes blob;
      if (!ReadEntry(src, nm, blob)) continue;
      const bool bpe = svrfmt::IsBpe(blob);
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(blob), kids)) continue;
      for (auto& k : kids) {
        if (k.id < 10 || k.id > 12 || c[k.id - 10].empty()) continue;
        Bytes own = k.data;
        if (EditGroups(own, k.id == 10 ? p.exh : k.id == 11 ? p.evt : p.mbd, int(k.id - 10))) k.data = std::move(own);
        else REXLOG_WARN("[svr2011] move packs: {} child {}: the packs' records don't fit - kept as it is", nm, k.id);
      }
      changed[nm] = Repack(svrfmt::PachWrite(kids), bpe);
    }
  }
  return WritePac(src, dst, changed);
}

bool BuildMisc(const fs::path& game, const fs::path& overlay, const Packs& p) {
  return BuildMiscFile(game / "pac" / "misc.pac", overlay / "misc.pac", p);
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(uint8_t(ch)));
  return s;
}

// ---- DLC copies of the merged pacs
// An installed DLC package (dlc.cpp: <user data>\0000000000000000\<title>\00000002\<package>\) can bring its
// own copy of a pac the packs change - SvR 2011's Vol. 2 has pac\mpsp_dlc02.pac (the whole MOTP/GAME set with
// its moves) and m_dlc02.pac (some MOT banks), Vol. 1 and 2 misc.pac (the move records, MOVS/WAZE, WAZA) - and
// in a match the game reads those instead of the overlay's: no motion from a pack was found (moves stood still
// or snapped), and a CPU whose moves are the packs' own had no record of them - nothing to attack with, it only
// reversed (player report 2026-10-15). So the packs also go into copies of those,
// Mods\PacOverlay\dlc\<package>\<name>, and the game's open of dlcN:\pac\<name> is linked to the copy. The
// package's files are only ever read (test folders link their user data to the player's: a build writing into
// the package changed the player's DLC). Players without the DLC: nothing.
struct DlcCopy {
  fs::path file;    // the package's pac (read only)
  std::string pac;  // the game's pac it stands for ("m.pac", "mpsp.pac", "misc.pac")
  fs::path copy;    // the overlay's copy with the packs
};

std::vector<DlcCopy> DlcCopies(const fs::path& dlc_root, const fs::path& overlay, const std::set<std::string>& pacs) {
  std::vector<DlcCopy> out;
  std::error_code ec;
  if (dlc_root.empty() || !fs::is_directory(dlc_root, ec)) return out;
  for (const auto& pkg : fs::directory_iterator(dlc_root, ec)) {
    const fs::path dir = pkg.path() / "pac";
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
      if (!f.is_regular_file()) continue;
      const std::string name = Lower(f.path().filename().string());
      // (misc.pac too: a copy once crashed the pac loading - read at the package catalog's old places,
      // PatchDlcCatalogs since)
      for (const std::string p : {"m.pac", "mpsp.pac", "misc.pac"}) {
        if (!pacs.count(p)) continue;
        const std::string stem = p.substr(0, p.size() - 4);
        // (the same name, or <stem>_dlcNN.pac)
        if (name == p ||
            (name.rfind(stem + "_dlc", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".pac") == 0))
          out.push_back({f.path(), p, overlay / "dlc" / pkg.path().filename() / f.path().filename()});
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const DlcCopy& a, const DlcCopy& b) { return a.file < b.file; });
  return out;
}

// Builds before this one merged into the package itself, its own file kept as <name>.orig: that goes back.
void RestoreDlcOrig(const fs::path& dlc_root) {
  std::error_code ec;
  if (dlc_root.empty() || !fs::is_directory(dlc_root, ec)) return;
  for (const auto& pkg : fs::directory_iterator(dlc_root, ec))
    for (const auto& f : fs::directory_iterator(pkg.path() / "pac", ec)) {
      const fs::path p = f.path();
      if (p.extension() != ".orig") continue;
      fs::path to = p;
      to.replace_extension();
      std::error_code e2;
      fs::rename(p, to, e2);  // (replaces the merged one)
      if (!e2) REXLOG_INFO("[svr2011] move packs: DLC {} back as it came (an older build had merged into it)",
                           to.filename().string());
      else REXLOG_WARN("[svr2011] move packs: DLC {}: cannot put {} back ({})", to.string(), p.filename().string(), e2.message());
    }
}

// The copy with the packs; false when it isn't served (none of the packs' banks in it, or it failed: the
// package's own file plays).
bool MergeDlcCopy(const DlcCopy& c, const Packs& p) {
  std::error_code ec;
  fs::remove(c.copy, ec);
  // (test aid: SVR2011_TEST_DLC_SKIP=<text>[,<text>] - DLC files whose name holds one aren't merged)
  if (const char* v = std::getenv("SVR2011_TEST_DLC_SKIP"); v && *v) {
    const std::string name = Lower(c.file.filename().string());
    for (const char* q = v; *q;) {
      const char* e = std::strchr(q, ',');
      const std::string t = Lower(e ? std::string(q, e) : std::string(q));
      if (!t.empty() && name.find(t) != std::string::npos) {
        REXLOG_INFO("[svr2011] move packs: DLC {} not merged (test)", c.file.filename().string());
        return false;
      }
      if (!e) break;
      q = e + 1;
    }
  }
  fs::create_directories(c.copy.parent_path(), ec);
  bool ok = true;
  size_t entries = 0;
  if (c.pac == "misc.pac") {
    ok = BuildMiscFile(c.file, c.copy, p);
    entries = 1;
  } else {
    for (const auto& [pac, motions] : p.motions)  // (as pack.txt names it: "m.pac", "mpsp.pac")
      if (Lower(pac) == c.pac) {
        ok = BuildMotionFile(c.file, c.copy, c.file.filename().string(), motions, true, &entries);
        break;
      }
  }
  if (!ok || entries == 0) {
    if (!ok) REXLOG_WARN("[svr2011] move packs: DLC {}: merging failed - the package's own plays", c.file.string());
    fs::remove(c.copy, ec);
    return false;
  }
  REXLOG_INFO("[svr2011] move packs: DLC {} has the packs too ({}, copy in the overlay)", c.file.filename().string(), c.pac);
  return true;
}

// A package's info\catalog_hd.arc / catalog_sd.arc is a pre-built directory of its pacs: each entry's name,
// big-endian sector and size (in 0x100s) as the pac's own table has them - the game reads the pacs by it,
// not by their tables. A merged copy's entries sit elsewhere (every pack moved them further: a plain match
// crashed at load, CREATE A MOVE-SET's diving list with one pack), so copies of the catalogs with the copy's
// places go beside it. An entry is found by its name with its old place (12 bytes, once in the catalog); a
// copy whose entries aren't all found so is dropped (the package's own pac plays).
std::vector<fs::path> DlcCatalogs(const fs::path& package) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(package / "info", ec)) {
    const std::string name = Lower(f.path().filename().string());
    if (f.is_regular_file() && name.size() > 4 && name.compare(name.size() - 4, 4, ".arc") == 0) out.push_back(f.path());
  }
  return out;
}

bool PatchCatalog(Bytes& arc, const fs::path& orig_pac, const fs::path& copy_pac) {
  std::ifstream a(orig_pac, std::ios::binary), b(copy_pac, std::ios::binary);
  Bytes ha, hb;
  std::vector<TocEntry> ta, tb;
  if (!a || !b || !ReadToc(a, ha, ta) || !ReadToc(b, hb, tb) || ta.size() != tb.size()) return false;
  auto be = [](uint8_t* d, uint32_t v) { d[0] = uint8_t(v >> 24), d[1] = uint8_t(v >> 16), d[2] = uint8_t(v >> 8), d[3] = uint8_t(v); };
  std::vector<std::pair<size_t, const TocEntry*>> at;
  for (size_t i = 0; i < ta.size(); ++i) {
    if (ta[i].type != tb[i].type || ta[i].name != tb[i].name || ta[i].name.size() != 4) return false;
    uint8_t want[12];
    std::memcpy(want, ta[i].name.data(), 4);
    be(want + 4, ta[i].sector), be(want + 8, ta[i].size256);
    const auto first = std::search(arc.begin(), arc.end(), want, want + 12);
    if (first == arc.end() || std::search(first + 1, arc.end(), want, want + 12) != arc.end()) return false;
    at.push_back({size_t(first - arc.begin()), &tb[i]});
  }
  for (const auto& [p, e] : at) be(&arc[p + 4], e->sector), be(&arc[p + 8], e->size256);
  return true;
}

// ---- The overlay's own files checked (2.1.7 report 4a90f4: CREATE A MOVE-SET loaded for ever - a read that
// started past a served file's end never completed). The stamp names what the overlay was made from; these say
// whether the files there are still what the game will read: an overlay pac or DLC copy whose table points past
// its end (cut short: a copy, a full disk, a crash), or a DLC catalog copy that doesn't name a package file the game
// reads with it at its place (a copy gone, a package file changed, an older build's leftovers).

// "" when every entry of `pac` (an EPAC) lies within the file, else why not. Not an EPAC: "" (not checked).
std::string TocFits(const fs::path& pac) {
  std::ifstream f(pac, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!f || !ReadToc(f, head, toc)) return "";
  std::error_code ec;
  const uint64_t size = fs::file_size(pac, ec);
  for (const auto& e : toc) {
    const uint64_t end = 0x4000 + uint64_t(e.sector) * 0x800 + uint64_t(e.size256) * 0x100;
    if (end > size)
      return fmt::format("{} is {} bytes, its entry {}/{} ends at {}", pac.filename().string(), size, e.type, e.name, end);
  }
  return "";
}

// Whether `arc` (a DLC catalog) names every entry of `pac` at its place ({name, sector, size}, big-endian - as
// PatchCatalog finds them).
bool CatalogNames(const Bytes& arc, const fs::path& pac) {
  std::ifstream f(pac, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!f || !ReadToc(f, head, toc) || toc.empty()) return false;
  for (const auto& e : toc) {
    if (e.name.size() != 4) return false;
    uint8_t want[12];
    std::memcpy(want, e.name.data(), 4);
    for (int k = 0; k < 4; ++k) want[4 + k] = uint8_t(e.sector >> (24 - 8 * k)), want[8 + k] = uint8_t(e.size256 >> (24 - 8 * k));
    if (std::search(arc.begin(), arc.end(), want, want + 12) == arc.end()) return false;
  }
  return true;
}

// "" when the served catalogs of `package` (copies in `list`) name, for every package pac its own catalog names, the
// file the game will read in its place (the served copy, else the package's own) - else why not.
std::string PackageMismatch(const fs::path& package, const std::vector<const DlcCopy*>& list) {
  std::error_code ec;
  std::map<fs::path, fs::path> read;  // package pac -> the file the game reads
  for (const auto& f : fs::directory_iterator(package / "pac", ec))
    if (f.is_regular_file(ec) && Lower(f.path().extension().string()) == ".pac") read[f.path()] = f.path();
  for (const DlcCopy* c : list)
    if (fs::exists(c->copy, ec)) read[c->file] = c->copy;
  const fs::path info = list.front()->copy.parent_path() / "info";
  for (const fs::path& cat : DlcCatalogs(package)) {
    Bytes own, served;
    if (!ReadAll(cat, own)) continue;
    if (!ReadAll(info / cat.filename(), served)) return fmt::format("no {} beside its copies", cat.filename().string());
    for (const auto& [file, used] : read) {
      if (!CatalogNames(own, file)) continue;  // (not one this catalog places)
      if (!CatalogNames(served, used))
        return fmt::format("{} doesn't place {} as the game reads it", cat.filename().string(), used.filename().string());
    }
  }
  return "";
}

// The first thing wrong with the overlay's files ("" when none): its pacs, the DLC copies, the served catalogs.
// "" when the overlay's copy of a game pac (an EPAC) still holds every entry of the game's own - a merge only adds or
// replaces entries - else the first it lacks. 2.1.8 report b67115: an overlay misc.pac without BATL/SAVE or MOVS/WAZE
// passed as up to date (its table fit its size): the start-up read of /BATL/SAVE found it in no pac and crashed.
std::string KeepsEntries(const fs::path& own, const fs::path& copy) {
  std::ifstream a(own, std::ios::binary), b(copy, std::ios::binary);
  Bytes ha, hb;
  std::vector<TocEntry> ta, tb;
  if (!a || !ReadToc(a, ha, ta)) return "";  // (the game's own isn't an EPAC: not checked)
  if (!b || !ReadToc(b, hb, tb)) return fmt::format("{} isn't a pac", copy.filename().string());
  std::set<std::pair<std::string, std::string>> has;
  for (const auto& e : tb) has.insert({e.type, e.name});
  for (const auto& e : ta)
    if (!has.count({e.type, e.name})) return fmt::format("{} lacks the game's {}/{}", copy.filename().string(), e.type, e.name);
  return "";
}

std::string OverlayDamage(const fs::path& game, const fs::path& overlay, const std::set<std::string>& pacs,
                          const std::vector<DlcCopy>& dlc) {
  std::error_code ec;
  for (const auto& p : pacs) {
    if (std::string why = TocFits(overlay / PacPath(p)); !why.empty()) return why;
    if (std::string why = KeepsEntries(game / "pac" / PacPath(p), overlay / PacPath(p)); !why.empty()) return why;
  }
  std::map<fs::path, std::vector<const DlcCopy*>> by_package;
  for (const auto& c : dlc) {
    if (!fs::exists(c.copy, ec)) continue;
    if (std::string why = TocFits(c.copy); !why.empty()) return "DLC " + why;
    if (std::string why = KeepsEntries(c.file, c.copy); !why.empty()) return "DLC " + why;
    by_package[c.file.parent_path().parent_path()].push_back(&c);
  }
  for (const auto& [package, list] : by_package)
    if (std::string why = PackageMismatch(package, list); !why.empty())
      return "DLC " + package.filename().string() + ": " + why;
  return "";
}

// The catalogs of the packages with copies, into the overlay beside them; copies that can't be named in
// every catalog removed.
void PatchDlcCatalogs(std::vector<DlcCopy>& copies) {
  std::error_code ec;
  std::map<fs::path, std::vector<DlcCopy*>> by_package;
  for (auto& c : copies)
    if (fs::exists(c.copy, ec)) by_package[c.file.parent_path().parent_path()].push_back(&c);
  for (auto& [package, list] : by_package) {
    const fs::path out_dir = list.front()->copy.parent_path() / "info";
    for (const fs::path& cat : DlcCatalogs(package)) {
      Bytes arc;
      if (!ReadAll(cat, arc)) continue;
      for (DlcCopy* c : list) {
        if (!fs::exists(c->copy, ec)) continue;
        Bytes test = arc;
        if (!PatchCatalog(test, c->file, c->copy)) {
          REXLOG_WARN("[svr2011] move packs: DLC {}: its entries aren't all in {} - the package's own plays",
                      c->file.filename().string(), cat.filename().string());
          fs::remove(c->copy, ec);
          continue;
        }
        arc = std::move(test);
      }
      fs::create_directories(out_dir, ec);
      const fs::path dst = out_dir / cat.filename();
      if (WriteWhole(dst, arc)) REXLOG_INFO("[svr2011] move packs: DLC {} with the copies' places", cat.filename().string());
    }
  }
  // (a catalog written before a later one dropped a copy names that copy's places: written again without it)
  for (auto& [package, list] : by_package) {
    bool dropped = false;
    for (DlcCopy* c : list) dropped |= !fs::exists(c->copy, ec);
    if (!dropped) continue;
    const fs::path out_dir = list.front()->copy.parent_path() / "info";
    for (const fs::path& cat : DlcCatalogs(package)) {
      Bytes arc;
      if (!ReadAll(cat, arc)) continue;
      for (DlcCopy* c : list)
        if (fs::exists(c->copy, ec)) PatchCatalog(arc, c->file, c->copy);
      WriteWhole(out_dir / cat.filename(), arc);
    }
  }
}

// The overlay's copies by package folder name (lower case): (path under the package's root, guest target).
// When the game mounts a package (dlc0:, dlc1: ... in the order it finds them) its root gets links to them -
// <root>:\pac\<name>, <root>:\info\<catalog> - and loses them when it unmounts it (SetContentMountHook).
std::mutex g_dlc_mutex;
std::map<std::string, std::vector<std::pair<std::string, std::string>>> g_dlc_links;
rex::filesystem::VirtualFileSystem* g_dlc_vfs = nullptr;

void OnContentMount(const std::string& root, const fs::path& package, bool mounted) {
  std::lock_guard lock(g_dlc_mutex);
  if (!g_dlc_vfs) return;
  const auto it = g_dlc_links.find(Lower(package.filename().string()));
  if (it == g_dlc_links.end()) return;
  for (const auto& [rel, target] : it->second) {
    const std::string guest = root + ":\\" + rel;
    if (mounted) g_dlc_vfs->RegisterSymbolicLink(guest, target);
    else g_dlc_vfs->UnregisterSymbolicLink(guest);
  }
  if (mounted)
    REXLOG_INFO("[svr2011] move packs: DLC {} mounted as {}: {} file(s) from the overlay", package.filename().string(), root,
                it->second.size());
}

void LinkDlcCopies(rex::filesystem::VirtualFileSystem* vfs, const fs::path& overlay, const std::vector<DlcCopy>& copies) {
  std::error_code ec;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> links;
  std::set<fs::path> catalogs;
  auto add = [&](const std::string& pkg, const std::string& rel, const fs::path& copy) {
    std::string r = fs::relative(copy, overlay, ec).string();
    std::replace(r.begin(), r.end(), '/', '\\');
    links[pkg].push_back({rel, "\\Device\\Harddisk0\\Partition1\\Mods\\PacOverlay\\" + r});
  };
  int n = 0;
  for (const auto& c : copies) {
    if (!fs::exists(c.copy, ec)) continue;
    const fs::path package = c.file.parent_path().parent_path();
    const std::string pkg = Lower(package.filename().string());
    add(pkg, "pac\\" + c.file.filename().string(), c.copy);
    ++n;
    for (const fs::path& cat : DlcCatalogs(package)) {
      const fs::path mine = c.copy.parent_path() / "info" / cat.filename();
      if (fs::exists(mine, ec) && catalogs.insert(mine).second) add(pkg, "info\\" + cat.filename().string(), mine);
    }
  }
  {
    std::lock_guard lock(g_dlc_mutex);
    g_dlc_links = std::move(links);
    g_dlc_vfs = vfs;
  }
  rex::system::xam::SetContentMountHook(&OnContentMount);
  if (n) REXLOG_INFO("[svr2011] move packs: {} DLC pac(s) and {} catalog(s) served from the overlay", n, catalogs.size());
}

// One merge at a time across games sharing the folder (Mods\PacOverlay\merge.lock, a folder: made or not, in
// one step; its owner.txt names the game's process). A lock whose game is gone - closed from Task Manager while
// merging, a crash - is taken over at once: it used to hold every start on a black screen for up to 2 minutes
// (report 2026-10-22 09:42: "another game is merging - waiting", closed, start after start). A lock with no
// owner.txt (a game before this change, or one stopped between the two steps) counts as left after 15 s.
bool ProcessRunning(uint32_t pid);  // (end of the file: the OS headers)
uint32_t OwnPid();

struct MergeLock {
  fs::path dir;
  bool held = false;
  explicit MergeLock(const fs::path& overlay) : dir(overlay / "merge.lock") {
    std::error_code ec;
    fs::create_directories(overlay, ec);
    const auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < 1200; ++t) {  // (up to 2 minutes while its game runs)
      if (fs::create_directory(dir, ec)) {
        held = true;
        std::ofstream(dir / "owner.txt", std::ios::binary) << OwnPid() << "\n";
        if (t)
          REXLOG_INFO("[svr2011] move packs: merge lock free after {} ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        return;
      }
      uint32_t owner = 0;
      {
        std::ifstream in(dir / "owner.txt", std::ios::binary);
        if (!(in >> owner)) owner = 0;
      }
      const auto age = fs::file_time_type::clock::now() - fs::last_write_time(dir, ec);
      const bool old = !ec && age > std::chrono::minutes(10);
      const bool gone = owner ? (owner == OwnPid() || !ProcessRunning(owner)) : (!ec && age > std::chrono::seconds(15));
      if (old || gone) {
        REXLOG_WARN("[svr2011] move packs: merge lock left by {} ({} s old) - taken over",
                    owner ? "game process " + std::to_string(owner) + ", no longer running" : std::string("a game that stopped"),
                    ec ? int64_t(-1) : int64_t(std::chrono::duration_cast<std::chrono::seconds>(age).count()));
        fs::remove_all(dir, ec);
        continue;
      }
      if (t == 0)
        REXLOG_INFO("[svr2011] move packs: another game is merging ({}) - waiting",
                    owner ? "process " + std::to_string(owner) : std::string("no owner yet"));
      svr2011::KeepWindowAlive();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REXLOG_WARN("[svr2011] move packs: {} stayed - merging anyway", dir.string());
  }
  ~MergeLock() {
    std::error_code ec;
    if (held) fs::remove_all(dir, ec);
  }
};

// pac/plist360(_4x3).h with the merged pacs named in the overlay.
bool WriteList(const fs::path& game, const fs::path& overlay, const std::string& name, const std::set<std::string>& pacs,
               const std::vector<std::string>& extra = {}) {
  std::ifstream in(game / "pac" / name, std::ios::binary);
  if (!in) return true;  // (the 4:3 list may not exist)
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    const std::string l = Lower(line);
    for (const auto& p : pacs)
      if (l == "pac\\" + p) line = "mods\\pacoverlay\\" + p;
    out << line << (cr ? "\r\n" : "\n");
  }
  for (const auto& e : extra) out << e << "\r\n";
  std::ofstream o(overlay / name, std::ios::binary | std::ios::trunc);
  o << out.str();
  return bool(o);
}

// The game folder's file device indexed the folder at start-up, before the
// overlay was made: the game opens the pacs through directory handles, which
// only walk that index (no lookup on the disk), so files made now would not
// be found - an endless loading screen - or would keep a size from before.
// Resolving each through the file system adds it to the index; update()
// refreshes a size it already had.
void Register(rex::filesystem::VirtualFileSystem* vfs, const fs::path& overlay) {
  if (!vfs) return;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(overlay, ec)) {  // (entry pacs sit in evt\)
    if (!e.is_regular_file()) continue;
    std::string rel = fs::relative(e.path(), overlay, ec).string();
    std::replace(rel.begin(), rel.end(), '/', '\\');
    const std::string path = "\\Device\\Harddisk0\\Partition1\\Mods\\PacOverlay\\" + rel;
    if (auto* entry = vfs->ResolvePath(path)) entry->update();
    else REXLOG_WARN("[svr2011] move packs: the game can't see {}", path);
  }
}

// pack.txt by its content (FNV-1a): a same-size edit keeps its size, and a
// copied pack can keep an old time.
std::string ContentStamp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  uint64_t h = 1469598103934665603ull;
  char c;
  while (in.get(c)) h = (h ^ uint8_t(c)) * 1099511628211ull;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
  return p.filename().string() + " " + hex + "\n";
}

std::string FileStamp(const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
  return p.filename().string() + " " + std::to_string(static_cast<unsigned long long>(size)) + " " + std::to_string(static_cast<long long>(time)) + "\n";
}

}  // namespace

namespace svr2011 {

const std::string& PacListFolder() { return g_folder; }

// The backstage mods with their own row and gimmicks (Mods/Backstage/<id>:
// manifest row= and gimmick=<entry>, gimmick.pac = a small pac holding only
// GMGB/<entry>; arena_mods.cpp) by folder name - the first one of each entry
// (arena_mods.cpp leaves a later mod with the same entry without gimmicks).
// Each is mounted as one more pac (bsgimmick.pac, bsgimmick2.pac ...): its
// line goes at the end of the overlay pac list (pacs mount in list order and a
// lookup takes the first match, so the game's own entries stay as they are).
// (File-level links aren't followed by the game's opens, only folder links: a
// gm.pac served in place of pac\gm.pac never was.)
constexpr size_t kMaxGimmickPacs = 8;
std::string GimmickPacName(size_t i) { return i ? "bsgimmick" + std::to_string(i + 1) + ".pac" : "bsgimmick.pac"; }
std::vector<fs::path> BackstageGimmickPacs(const fs::path& game) {
  if (svr2011::NoMods() || svr2011::BackstageModsOff()) return {};  // (Start Without Mods / backstage mods off: no_mods.h)
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(game / "Mods" / "Backstage", ec))
    if (e.is_directory() && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "gimmick.pac", ec) &&
        fs::exists(e.path() / "arena.pac", ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  std::vector<fs::path> out;
  std::set<std::string> entries;
  for (const auto& d : dirs) {
    std::ifstream m(d / "manifest.txt", std::ios::binary);
    std::string line, gimmick;
    bool row = false;
    while (std::getline(m, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      row |= line.rfind("row=", 0) == 0;
      if (line.rfind("gimmick=", 0) == 0) gimmick = line.substr(8);
    }
    if (row && gimmick.size() == 4 && out.size() < kMaxGimmickPacs && entries.insert(gimmick).second)
      out.push_back(d / "gimmick.pac");
  }
  return out;
}

void LoadMoveTables(const fs::path& game);  // (below: the move tables for a match's fallback, cam_mod_moves.cpp)

// ---- The entrances the packs add (CREATE AN ENTRANCE's MODS rows, cae_mod_motions.cpp):
// pacentry lines EVP? <n> (n < 10000) are entrances, EVP? <10000 + 1000k + n> their CAE parts.
// The label: the superstar mod's name (manifest name=) for a mod's own moves/, else the script's
// own name (EVP +0x22) and the game it came from (the pack.txt's first line, as the converters
// write it: "# <game> entrance <n> as 2011 <m>").
namespace {
std::mutex g_ent_mutex;
std::vector<ModEntrance> g_mod_entrances;
std::set<int> g_mod_scripts;  // every EVP number the packs add (entrances and parts)

std::string ManifestName(const fs::path& mod) {
  std::ifstream in(mod / "manifest.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("name=", 0) == 0) return line.substr(5);
  }
  return mod.filename().string();
}

std::string ScriptName(const fs::path& evp) {  // (EVP +0x22: 30 bytes, NUL padded; '_' as space)
  std::ifstream in(evp, std::ios::binary);
  char buf[0x40] = {};
  if (!in.read(buf, sizeof(buf))) return "";
  std::string s(buf + 0x22, strnlen(buf + 0x22, 30));
  std::replace(s.begin(), s.end(), '_', ' ');
  return s;
}

std::string PackGame(const fs::path& dir) {  // ("# WWE '13 entrance 246 as 2011 497" -> "WWE '13")
  std::ifstream in(dir / "pack.txt");
  std::string line;
  if (!std::getline(in, line) || line.rfind("# ", 0) != 0) return "";
  const size_t e = line.find(" entrance ");
  return e == std::string::npos ? "" : line.substr(2, e - 2);
}
}  // namespace

void SetModEntrances(const std::vector<Packs::EntryLine>& lines) {
  std::map<int, ModEntrance> ents;
  std::set<int> scripts;
  std::map<fs::path, int> per_dir;  // (entrances per pack: a mod with several gets the script name too)
  for (const auto& l : lines) {
    if (l.group.rfind("EVP", 0) != 0 || l.name.empty() || l.name.find_first_not_of("0123456789") != std::string::npos)
      continue;
    const int n = std::atoi(l.name.c_str());
    scripts.insert(n);
    if (n < 6000 && !ents.count(n)) per_dir[l.dir]++;
  }
  for (const auto& l : lines) {
    if (l.group.rfind("EVP", 0) != 0 || l.name.empty() || l.name.find_first_not_of("0123456789") != std::string::npos)
      continue;
    const int n = std::atoi(l.name.c_str());
    if (n >= 6000 || ents.count(n)) continue;  // (6000-9999: victory scenes, docs/VICTORY_SCENES.md; 10000+ parts)
    ModEntrance e;
    e.number = n;
    const std::string script = ScriptName(l.file);
    const bool mod_moves = l.dir.filename() == "moves" && l.dir.parent_path().parent_path().filename() == "Superstars";
    std::error_code ec;
    if (mod_moves || fs::exists(l.dir / "manifest.txt", ec)) {  // (a pack with a manifest: its name=, as a mod's - the
      // Mod Maker's Simple entrance porting names it after the superstar)
      e.label = ManifestName(mod_moves ? l.dir.parent_path() : l.dir);
      if (per_dir[l.dir] > 1 && !script.empty()) e.label += " - " + script;
    } else {
      const std::string game = PackGame(l.dir);
      e.label = script.empty() ? l.dir.filename().string() : script;
      if (!game.empty()) e.label += " (" + game + ")";
    }
    if (e.label.size() > 40) e.label.resize(40);
    for (int k = 0; k < 5; ++k)
      if (scripts.count(10000 + 1000 * k + n)) e.parts |= uint8_t(1u << k);
    ents[n] = e;
  }
  std::lock_guard lock(g_ent_mutex);
  g_mod_entrances.clear();
  for (auto& [n, e] : ents) g_mod_entrances.push_back(std::move(e));
  g_mod_scripts = std::move(scripts);
  if (!g_mod_entrances.empty()) REXLOG_INFO("[svr2011] move packs: {} mod entrances", g_mod_entrances.size());
}

std::vector<ModEntrance> ModEntrances() {
  std::lock_guard lock(g_ent_mutex);
  return g_mod_entrances;
}

bool EntranceScriptExists(int n) {
  // the game's own: the EVP groups of pac\evt\*.pac (EPK8 tables: groups {4CC, u16 count * 4, u16, u32},
  // 16-byte entries {name[8], sector, size}), read once
  static const std::set<int> stock = [] {
    std::set<int> s;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(rex::filesystem::GetExecutableFolder() / "pac" / "evt", ec)) {
      std::ifstream in(e.path(), std::ios::binary);
      Bytes head(0x4000);
      if (!in.read(reinterpret_cast<char*>(head.data()), 0x4000) || std::memcmp(head.data(), "EPK8", 4)) continue;
      for (size_t q = 0x800; q + 12 <= 0x4000 && Le32(&head[q]);) {
        const bool evp = !std::memcmp(&head[q], "EVP", 3);
        const uint32_t cnt = Le16(&head[q + 4]) / 4;
        q += 12;
        for (uint32_t i = 0; i < cnt && q + 16 <= 0x4000; ++i, q += 16) {
          const std::string nm(reinterpret_cast<char*>(&head[q]), 8);
          if (evp && std::isdigit(static_cast<unsigned char>(nm[0]))) s.insert(std::atoi(nm.c_str()));
        }
      }
    }
    REXLOG_INFO("[svr2011] move packs: {} stock entrance scripts", s.size());
    return s;
  }();
  if (stock.count(n)) return true;
  std::lock_guard lock(g_ent_mutex);
  return g_mod_scripts.count(n) != 0;
}

void InstallMovePacks(rex::filesystem::VirtualFileSystem* vfs, const fs::path& dlc_root) {
  if (const char* v = std::getenv("SVR2011_TEST_PLIST"); v && *v) {  // (test aid: a folder made by hand)
    g_folder = v;
    return;
  }
  const fs::path game = rex::filesystem::GetExecutableFolder();
  if (svr2011::NoMods()) {  // (Start Without Mods: the overlay left as it is, the stock tables - no_mods.h)
    REXLOG_INFO("[svr2011] move packs: started without mods - none merged, the overlay kept");
    LoadMoveTables(game);
    return;
  }
  const fs::path overlay = game / "Mods" / "PacOverlay";
  std::error_code ec;
  // the packs: superstar mods' moves/ folders and Mods/Moves/<pack>
  std::vector<fs::path> dirs;
  // (attire mods replacing an attire the game's pac has: generated packs, a pacentry line each)
  PrepareAttirePacks(game);
  for (const fs::path& root : {game / "Mods" / "Superstars", game / "Mods" / "Moves",
                               game / "Mods" / "SuperstarOverlay" / "attirepacks"})
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
      const fs::path d = root.filename() == "Superstars" ? e.path() / "moves" : e.path();
      if (fs::exists(d / "pack.txt", ec)) dirs.push_back(d);
    }
  std::sort(dirs.begin(), dirs.end());
  RestoreDlcOrig(dlc_root);  // (an older build's merge into the package undone)
  const std::vector<fs::path> gms = BackstageGimmickPacs(game);
  const auto remove_gimmick_pacs = [&] {
    for (size_t i = 0; i < kMaxGimmickPacs; ++i) fs::remove(overlay / GimmickPacName(i), ec);
  };
  if (dirs.empty() && gms.empty()) {
    for (const char* f : {"m.pac", "misc.pac", "mpsp.pac", "gm.pac", "plist360.h", "plist360_4x3.h", "stamp.txt"})
      fs::remove(overlay / f, ec);  // (nothing to serve: free the space)
    remove_gimmick_pacs();
    fs::remove_all(overlay / "evt", ec);
    fs::remove_all(overlay / "dlc", ec);
    LoadMoveTables(game);  // (a removed pack's moves in movesets: the match fallback still needs the tables)
    return;
  }
  Packs packs;
  std::string stamp;
  // (data false: the stamp and lists only; the motion files are read when merging)
  auto read_packs = [&](bool data) {
    packs = Packs{};
    stamp = std::string(kFormat) + "\n";
    std::map<std::pair<std::string, std::string>, std::set<uint16_t>> pack_ids;
    for (const auto& d : dirs) {
      std::set<uint16_t> before;
      for (const auto& [id, b] : packs.waze) before.insert(id);
      for (const auto& [id, n] : packs.names) before.insert(id);
      for (const auto& [id, c] : packs.copies) before.insert(id);
      ReadPack(d, packs, stamp, data);
      stamp += ContentStamp(d / "pack.txt");
      // (its moves: the ids it adds - a superstar mod's moves/ folder, or Mods/Moves/<pack>)
      const bool star = d.filename() == "moves";
      const std::string kind = star ? "Superstars" : d.parent_path().filename().string();
      if (kind != "Superstars" && kind != "Moves") continue;
      auto& ids = pack_ids[{kind, (star ? d.parent_path() : d).filename().string()}];
      for (const auto& [id, b] : packs.waze) if (!before.count(id)) ids.insert(id);
      for (const auto& [id, n] : packs.names) if (!before.count(id)) ids.insert(id);
      for (const auto& [id, c] : packs.copies) if (!before.count(id)) ids.insert(id);
    }
    {
      std::lock_guard lock(g_pack_ids_mutex);
      g_pack_ids = std::move(pack_ids);
    }
    // (the game's own files too - in both reads, or the saved stamp never
    // matches the next start's and every start merges again)
    for (const char* f : {"m.pac", "misc.pac", "mpsp.pac", "plist360.h", "plist360_4x3.h"})
      stamp += FileStamp(game / "pac" / f);
    for (const auto& [pac, e] : packs.entries) stamp += FileStamp(game / "pac" / PacPath(pac));
    for (const auto& gm : gms) stamp += "gimmick " + gm.string() + " " + FileStamp(gm);
  };
  read_packs(false);
  SetModEntrances(packs.entry_lines);
  WarnOverrides(packs);
  std::set<std::string> pacs;
  for (const auto& [pac, e] : packs.motions) pacs.insert(Lower(pac));
  for (const auto& [pac, e] : packs.entries) pacs.insert(pac);  // ("evt\nyujyo5.pac": WriteList maps pac\evt\... to it)
  if (!packs.waze.empty() || !packs.names.empty() || !packs.copies.empty() || !packs.exh.empty() || !packs.evt.empty() ||
      !packs.mbd.empty())
    pacs.insert("misc.pac");
  g_waze = packs.waze;
  g_names = packs.names;
  g_copies = packs.copies;
  // (the DLC packages' pacs the packs go into: copies in the overlay - a package installed again, or another,
  // merges again)
  std::vector<DlcCopy> dlc = DlcCopies(dlc_root, overlay, pacs);
  auto dlc_stamp = [&] {
    std::string s;
    std::set<fs::path> packages;
    for (const auto& c : dlc) {
      s += "dlc " + c.file.parent_path().parent_path().filename().string() + "/" + FileStamp(c.file);
      packages.insert(c.file.parent_path().parent_path());
    }
    for (const auto& p : packages)
      for (const fs::path& cat : DlcCatalogs(p)) s += "dlc " + p.filename().string() + "/" + FileStamp(cat);
    return s;
  };
  // (a served copy needs its package's catalogs beside it: the game would read it at the old places)
  auto dlc_complete = [&] {
    for (const auto& c : dlc)
      if (fs::exists(c.copy, ec))
        for (const fs::path& cat : DlcCatalogs(c.file.parent_path().parent_path()))
          if (!fs::exists(c.copy.parent_path() / "info" / cat.filename(), ec)) return false;
    return true;
  };
  const MergeLock lock(overlay);  // (another game merging the same folder: its result, or ours after it)
  {
    std::ifstream old(overlay / "stamp.txt", std::ios::binary);
    std::string was((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
    bool have = was == stamp + dlc_stamp();
    for (const auto& p : pacs) have &= fs::exists(overlay / PacPath(p), ec);
    for (size_t i = 0; i < gms.size(); ++i) have &= fs::exists(overlay / GimmickPacName(i), ec);
    have &= dlc_complete();
    if (have) {
      if (const std::string why = OverlayDamage(game, overlay, pacs, dlc); !why.empty()) {
        REXLOG_WARN("[svr2011] move packs: the overlay's files don't match ({}) - merging again", why);
        have = false;
      }
    }
    if (have) {
      LinkDlcCopies(vfs, overlay, dlc);
      Register(vfs, overlay);
      g_folder = "Mods\\PacOverlay";
      REXLOG_INFO("[svr2011] move packs: {} packs ({} items), overlay up to date", dirs.size(), packs.count);
      LoadMoveTables(game);
      return;
    }
  }
  REXLOG_INFO("[svr2011] move packs: merging {} packs ({} items) into {} ...", dirs.size(), packs.count, overlay.string());
  {
    const std::string checked = stamp;
    read_packs(true);  // (now the motion files too)
    if (stamp != checked) REXLOG_WARN("[svr2011] move packs: a pack changed while it was read");
  }
  fs::create_directories(overlay, ec);
  fs::remove(overlay / "stamp.txt", ec);
  bool ok = true;
  {  // (the motions the game and the packs have: a pack motion going on to none of them is cleared, BankInsert)
    g_known_motions.assign(0x10000, false);
    for (const char* name : {"m.pac", "mpsp.pac"}) {
      std::ifstream in(game / "pac" / name, std::ios::binary);
      Bytes head, blob;
      std::vector<TocEntry> toc;
      if (!in || !ReadToc(in, head, toc)) continue;
      for (const auto& e : toc) {
        svr2011::KeepWindowAlive();
        blob.resize(size_t(e.size256) * 0x100);
        in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
        if (in.read(reinterpret_cast<char*>(blob.data()), std::streamsize(blob.size()))) CollectMotionIds(blob, g_known_motions);
        else in.clear();
      }
    }
    size_t n = 0;
    for (bool b : g_known_motions) n += b;
    for (const auto& [pac, entries] : packs.motions)
      for (const auto& [type_name, banks] : entries)
        for (const auto& [path, motions] : banks)
          for (const auto& m : motions) g_known_motions[m.id] = true;
    if (n < 1000) g_known_motions.clear();  // (the game's motions not read: nothing judged)
    REXLOG_INFO("[svr2011] move packs: {} motion ids in the game's m.pac / mpsp.pac", n);
  }
  for (const auto& [pac, entries] : packs.motions) {
    svr2011::KeepWindowAlive();
    ok = ok && BuildMotionPac(game, overlay, Lower(pac), entries);
  }
  svr2011::KeepWindowAlive();
  if (ok && pacs.count("misc.pac")) ok = BuildMisc(game, overlay, packs);
  fs::remove_all(overlay / "evt", ec);  // (entry pacs: copies of the game's with the packs' entries)
  // the entrance props first: a copy of evtobj.pac that can't be made (its table full, the disk) leaves the
  // packs' props out, and their entrance scripts use the game's props named by propfallback lines
  if (const auto obj = packs.entries.find(kPropPac); ok && obj != packs.entries.end()) {
    svr2011::KeepWindowAlive();
    // (test aid SVR2011_TEST_NO_PROP_COPY: as if the copy couldn't be made)
    if (std::getenv("SVR2011_TEST_NO_PROP_COPY") || !BuildEntryPac(game, overlay, kPropPac, obj->second)) {
      if (packs.prop_fallback.empty()) {
        ok = false;
      } else {
        int moved = 0;
        for (auto& [pac, entries] : packs.entries)
          for (auto& [key, data] : entries)
            if (!key.compare(0, 3, "EVP")) moved += FallBackProps(data, packs.prop_fallback);
        REXLOG_WARN("[svr2011] move packs: {} could not be copied with the packs' {} prop(s) - {} entrance prop(s) "
                    "use the game's instead (propfallback)", kPropPac, obj->second.size(), moved);
        fs::remove(overlay / PacPath(kPropPac), ec);
        pacs.erase(kPropPac);
        packs.entries.erase(obj);
      }
    }
  }
  for (const auto& [pac, entries] : packs.entries) {
    svr2011::KeepWindowAlive();
    if (pac == kPropPac) continue;
    ok = ok && BuildEntryPac(game, overlay, pac, entries);
  }
  fs::remove(overlay / "gm.pac", ec);  // (an older build served a whole gm.pac)
  remove_gimmick_pacs();
  std::vector<std::string> extra;
  for (size_t i = 0; ok && i < gms.size(); ++i) {
    const fs::path to = overlay / GimmickPacName(i);
    if (fs::exists(to, ec)) continue;  // (left in place - a running game holds it: not linked over)
    fs::create_hard_link(gms[i], to, ec);  // (a hard link to the mod's file, else a copy)
    if (ec) {
      ec.clear();
      // (never over an entry left in place: an old link would be written through)
      fs::copy_file(gms[i], to, fs::copy_options::none, ec);
    }
    ok = !ec;
    extra.push_back(std::string("mods\\pacoverlay\\") + GimmickPacName(i));
    REXLOG_INFO("[svr2011] move packs: backstage gimmicks from {}", gms[i].string());
  }
  ok = ok && WriteList(game, overlay, "plist360.h", pacs, extra) && WriteList(game, overlay, "plist360_4x3.h", pacs, extra);
  if (!ok) {
    REXLOG_WARN("[svr2011] move packs: merging failed - the game plays without them");
    LoadMoveTables(game);
    return;
  }
  // the DLC packages' copies (from the packages' files, never written)
  fs::remove_all(overlay / "dlc", ec);
  for (const auto& c : dlc) {
    svr2011::KeepWindowAlive();
    MergeDlcCopy(c, packs);  // (a failure: the package's own plays, logged)
  }
  PatchDlcCatalogs(dlc);
  for (const auto& c : dlc)  // (a catalog that couldn't be written: that copy isn't served)
    if (fs::exists(c.copy, ec))
      for (const fs::path& cat : DlcCatalogs(c.file.parent_path().parent_path()))
        if (!fs::exists(c.copy.parent_path() / "info" / cat.filename(), ec)) {
          REXLOG_WARN("[svr2011] move packs: DLC {}: no {} beside it - the package's own plays", c.file.filename().string(),
                      cat.filename().string());
          fs::remove(c.copy, ec);
          break;
        }
  // (a merged pac that lost one of the game's entries - never wanted: a merge only adds or replaces - is served as the
  // game's own instead: without its packs, but the game finds every file it reads)
  for (const auto& p : pacs)
    if (const std::string why = KeepsEntries(game / "pac" / PacPath(p), overlay / PacPath(p)); !why.empty()) {
      REXLOG_ERROR("[svr2011] move packs: {} after the merge - the game's own {} is served instead (its packs left out)",
                   why, p);
      fs::remove(overlay / PacPath(p), ec);  // (a link to the game's file is only unlinked, never written through)
      fs::copy_file(game / "pac" / PacPath(p), overlay / PacPath(p), ec);
    }
  {  // (a package whose served files still don't match its catalogs: its own files play)
    std::map<fs::path, std::vector<const DlcCopy*>> by_package;
    for (const auto& c : dlc)
      if (fs::exists(c.copy, ec)) by_package[c.file.parent_path().parent_path()].push_back(&c);
    for (const auto& [package, list] : by_package) {
      std::string why = PackageMismatch(package, list);
      for (const DlcCopy* c : list)
        if (why.empty()) why = TocFits(c->copy);
      if (why.empty()) continue;
      REXLOG_WARN("[svr2011] move packs: DLC {}: {} - the package's own files play", package.filename().string(), why);
      for (const DlcCopy* c : list) fs::remove(c->copy, ec);
    }
  }
  {
    const std::string all = stamp + dlc_stamp();
    WriteWhole(overlay / "stamp.txt", Bytes(all.begin(), all.end()));
  }
  LinkDlcCopies(vfs, overlay, dlc);
  Register(vfs, overlay);
  g_folder = "Mods\\PacOverlay";
  REXLOG_INFO("[svr2011] move packs: merged");
  LoadMoveTables(game);
}

}  // namespace svr2011

// MOVS/WAZE loaded (sub_8237ED68): sub_8237E708(data) swaps its header,
// then its records are swapped and the category lists built. The game can
// load another file's than the overlay's misc.pac (with the DLCs installed,
// one without the packs' moves - Jeff Hardy's ported finisher wasn't in
// CREATE A MOVESET's list): each is patched here, as the file still is.
namespace {
// The moves a moveset can name (MoveUsable): a record with category bits, as
// the last MOVS/WAZE loaded (after the packs' patch).
std::mutex g_usable_mutex;
std::vector<bool> g_usable;  // (empty: not loaded yet)
std::vector<uint16_t> g_defaults;  // category -> its fallback move (CategoryDefaultMove)
std::vector<bool> g_record, g_bits;  // a record with the id at all / with category bits (MoveMissing)
std::vector<bool> g_past_pack;       // ids a pack has given (Mods\PacOverlay\pack_moves.txt, kept across starts)
std::vector<bool> g_stock_ok;        // the stock game plays the id without packs (a record with bits, or EXH data)

// A pack's move copied (wazecopy) onto a record of 16000 and up outside its template's thousand: the game never
// starts a move there (docs/MOVE_PORTING.md 2.4) - the pair stood frozen in its first frame. Bundled Mankind 2.0.3's
// Double Axe Handle 8 on 17093 (template 11003) froze matches (report 20261101-073525-b564c9; 2.0.4 moved it).
bool NeverStarts(uint16_t id, uint16_t from) { return id >= 16000 && id / 1000 != from / 1000; }

// MoveUsable's list and CategoryDefaultMove's table from MOVS/WAZE records as in the file (the packs' patch
// applied): at start-up from misc.pac (a match never loads MOVS/WAZE itself - CREATE A MOVE-SET does) and
// again at each load (sub_8237E708).
void SetMoveTables(const uint8_t* w) {
  std::vector<bool> usable(65536, false), record(65536, false);
  for (uint32_t k = 0, n = Le32(w + 4); k < n; ++k) {
    const uint8_t* r = w + 272 + size_t(k) * 160;
    record[Le16(r + 0x90)] = true;
    if (std::any_of(r, r + 16, [](uint8_t b) { return b != 0; })) usable[Le16(r + 0x90)] = true;
  }
  std::vector<bool> bits = usable;
  // (a move pack's own moves count whatever the records say: Sabu's Arabian
  // Facebuster 6861 has 2011's motions and the pack's bits but no record of
  // its own in the file - it plays)
  for (const auto& [id, b] : g_waze) usable[id] = true;
  for (const auto& [id, from] : g_copies) usable[id] = true;
  for (const auto& [id, n] : g_names) usable[id] = true;
  // (but not a copy on a record the game never starts - NeverStarts: movesets keep the base's move, a saved edit
  // the mod's own)
  for (const auto& [id, from] : g_copies)
    if (NeverStarts(id, from)) usable[id] = false;
  // per category, the move a slot falls back to in a match when its own has no record (cam_mod_moves.cpp):
  // the first game move (not a pack's) of that category the game always shows (unlock 512 or 1-3), else
  // its first game move
  std::vector<uint16_t> defaults(128, 0), any(128, 0);
  for (uint32_t k = 0, n = Le32(w + 4); k < n; ++k) {
    const uint8_t* r = w + 272 + size_t(k) * 160;
    const uint16_t id = Le16(r + 0x90), unlock = Le16(r + 0x78);
    if (!id || g_waze.count(id) || g_copies.count(id) || g_names.count(id)) continue;
    const bool shown = unlock == 512 || (unlock >= 1 && unlock <= 3);
    for (int c = 0; c < 128; ++c) {
      if (!(r[c >> 3] >> (c & 7) & 1)) continue;
      if (!any[c]) any[c] = id;
      if (shown && !defaults[c]) defaults[c] = id;
    }
  }
  for (int c = 0; c < 128; ++c)
    if (!defaults[c]) defaults[c] = any[c];
  std::lock_guard lock(g_usable_mutex);
  g_usable = std::move(usable);
  g_defaults = std::move(defaults);
  g_record = std::move(record);
  g_bits = std::move(bits);
}
}  // namespace

namespace svr2011 {
void LoadMoveTables(const fs::path& game) {
  const fs::path overlay = game / "Mods" / "PacOverlay" / "misc.pac";
  std::error_code ec;
  const fs::path src = !svr2011::NoMods() && fs::exists(overlay, ec) ? overlay : game / "pac" / "misc.pac";
  Bytes blob;
  if (!ReadEntry(src, "MOVS/WAZE", blob)) {
    REXLOG_WARN("[svr2011] move packs: no MOVS/WAZE in {} - no match fallback for removed moves", src.string());
    return;
  }
  Bytes w = svrfmt::Unpack(blob);
  if (w.size() < 272 + 160 || std::memcmp(w.data() + 272 + 16, "* test motion *", 15)) return;
  int fixed = 0;
  PatchWaze(w.data(), w.size(), g_waze, &fixed, g_names, g_copies);  // (the overlay's is patched already: no change)
  SetMoveTables(w.data());
  // the ids packs have given so far (this start's added): a removed pack's move keeps a cut record without
  // bits, which stock movesets' moves can have too - only a past pack's id there is "missing"
  std::vector<bool> past(65536, false);
  const fs::path list = game / "Mods" / "PacOverlay" / "pack_moves.txt";
  {
    std::ifstream in(list);
    for (int id; in >> id;)
      if (id > 0 && id < 65536) past[id] = true;
  }
  bool added = false;
  auto mark = [&](uint16_t id) {
    if (!past[id]) past[id] = true, added = true;
  };
  for (const auto& [id, b] : g_waze) mark(id);
  for (const auto& [id, nm] : g_names) mark(id);
  for (const auto& [id, from] : g_copies) mark(id);
  if (added) {
    fs::create_directories(list.parent_path(), ec);
    std::ofstream out(list, std::ios::trunc);
    for (int id = 1; id < 65536; ++id)
      if (past[id]) out << id << '\n';
  }
  // what the stock game can play without packs: its own misc.pac's records with bits and EXH data
  std::vector<bool> stock_ok(65536, false);
  Bytes sw, sd;
  if (ReadEntry(game / "pac" / "misc.pac", "MOVS/WAZE", sw)) {
    const Bytes w0 = svrfmt::Unpack(sw);
    if (w0.size() >= 272)
      for (uint32_t k = 0, n = Le32(w0.data() + 4); k < n && 272 + size_t(k + 1) * 160 <= w0.size(); ++k) {
        const uint8_t* r = w0.data() + 272 + size_t(k) * 160;
        if (std::any_of(r, r + 16, [](uint8_t b) { return b != 0; })) stock_ok[Le16(r + 0x90)] = true;
      }
  }
  if (ReadEntry(game / "pac" / "misc.pac", "WAZA/DATA", sd)) {
    std::vector<svrfmt::PachEntry> kids, groups;
    if (svrfmt::PachRead(svrfmt::Unpack(sd), kids) && !kids.empty() && svrfmt::PachRead(svrfmt::Unpack(kids[0].data), groups))
      for (const auto& g : groups) {
        const Bytes t = svrfmt::Unpack(g.data);
        if (t.size() < 8 || std::memcmp(t.data(), "EXH", 3)) continue;
        for (size_t o = 8; o + 36 <= t.size(); o += 36) stock_ok[Le16(t.data() + o)] = true;
      }
  }
  {
    std::lock_guard lock(g_usable_mutex);
    g_past_pack = std::move(past);
    g_stock_ok = std::move(stock_ok);
  }
  {
    std::string never;
    for (const auto& [id, from] : g_copies)
      if (NeverStarts(id, from)) never += fmt::format(" {} (from {})", id, from);
    if (!never.empty())
      REXLOG_WARN("[svr2011] move packs: moves on records the game never starts (16000 and up outside the template's "
                  "thousand - not usable, movesets keep other moves; port them again):{}", never);
  }
  {
    std::string gone;
    std::set<uint16_t> ids;
    {
      std::lock_guard lock(g_motionless_mutex);  // (a copy: MoveUsable takes the lock too)
      ids = g_motionless;
    }
    for (uint16_t id : ids)
      if (svr2011::MoveUsable(id) == 0) gone += " " + std::to_string(id);
    if (!gone.empty())
      REXLOG_WARN("[svr2011] move packs: moves without their motion files (not usable - movesets keep other moves):{}", gone);
  }
  REXLOG_INFO("[svr2011] move packs: move tables from {} ({} records)", src.filename().string(), Le32(w.data() + 4));
}
}  // namespace svr2011

namespace svr2011 {
// (g_waze / g_names / g_copies are set once at start-up, before the game runs)
bool PackMove(uint16_t id) { return g_waze.count(id) || g_names.count(id) || g_copies.count(id); }

uint16_t CategoryDefaultMove(int category) {
  std::lock_guard lock(g_usable_mutex);
  return category >= 0 && size_t(category) < g_defaults.size() ? g_defaults[category] : 0;
}

std::vector<std::pair<std::string, std::string>> MovePacks() {
  std::lock_guard lock(g_pack_ids_mutex);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [pack, ids] : g_pack_ids)
    if (!ids.empty()) out.push_back(pack);
  return out;
}

std::vector<uint16_t> MovePackMoves(const std::string& kind, const std::string& folder) {
  std::lock_guard lock(g_pack_ids_mutex);
  const auto it = g_pack_ids.find({kind, folder});
  return it == g_pack_ids.end() ? std::vector<uint16_t>{} : std::vector<uint16_t>(it->second.begin(), it->second.end());
}

bool StockPlays(uint16_t id) {
  std::lock_guard lock(g_usable_mutex);
  return g_stock_ok.empty() || g_stock_ok[id];  // (tables not loaded: nothing to judge by)
}

bool MoveMissing(uint16_t id) {
  // only a move a pack gave once, no pack gives now and the stock game can't play: stock movesets name moves
  // without records, bits or EXH data that play fine, so nothing wider can be trusted
  if (!id || PackMove(id)) return false;
  std::lock_guard lock(g_usable_mutex);
  if (g_past_pack.empty() || !g_past_pack[id]) return false;
  return g_stock_ok.empty() || !g_stock_ok[id];
}

int MoveUsable(uint16_t id) {
  {
    std::lock_guard lock(g_usable_mutex);
    if (g_usable.empty()) return -1;
    if (!g_usable[id]) return 0;
  }
  std::lock_guard lock(g_motionless_mutex);
  return g_motionless.count(id) ? 0 : 1;
}
}  // namespace svr2011

REX_EXTERN(__imp__sub_8237E708);
REX_HOOK_RAW(sub_8237E708) {
  const uint32_t data = ctx.r3.u32;
  static const bool off = [] { const char* v = std::getenv("SVR2011_TEST_WAZE_LOAD"); return v && *v == '0'; }();
  if (data && (!g_waze.empty() || !g_names.empty() || !g_copies.empty()) && !off) {
    uint8_t* w = base + data;
    const size_t size = 272 + size_t(Le32(w + 4)) * 160;
    int fixed = 0;
    const int set = Le32(w + 4) < 20000 ? PatchWaze(w, size, g_waze, &fixed, g_names, g_copies) : -1;
    static int logged = 0;
    if (logged++ < 4)
      REXLOG_INFO("[svr2011] move packs: MOVS/WAZE loaded ({} records): {} moves set, {} category counts updated",
                  Le32(w + 4), set, fixed);
  }
  if (data && Le32(base + data + 4) < 20000 && !std::memcmp(base + data + 272 + 16, "* test motion *", 15))
    SetMoveTables(base + data);  // (records still as in the file: id at +0x90, bits +0..+15)
  __imp__sub_8237E708(ctx, base);
}

// A moveset's move by slot (r3 slot, r4 moveset; sub_8237D4C8 looks it up in
// its category's list): NULL when the move isn't in that list, and these
// callers read the record without a check (+115 / +116: CREATE A MOVESET's
// signature / finisher slots crashed, read of 0x74, on a mod whose profile
// was its slot's placeholder - move 15271, which doesn't exist). They
// get record 0 instead ("* test motion *": its bytes there are 0, as the code
// takes for none, and it is in no category). Other callers check for NULL.
REX_EXTERN(__imp__sub_82379158);
REX_HOOK_RAW(sub_82379158) {
  const uint32_t lr = uint32_t(ctx.lr), slot = ctx.r3.u32, moveset = ctx.r4.u32;
  __imp__sub_82379158(ctx, base);
  if (ctx.r3.u32) return;
  switch (lr) {
    case 0x82B9E1DC: case 0x82BA69E4: case 0x82B9C2E4: case 0x823797FC: case 0x82BA6424: break;
    default: return;
  }
  auto rd = [&](uint32_t a) { const uint8_t* p = base + a; return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
  const uint32_t table = rd(0x82E3C214);
  const uint32_t first = table ? rd(table + 60) : 0;
  if (!first) return;
  ctx.r3.u64 = first;
  static int logged = 0;
  if (logged++ < 8) {
    const uint32_t id = moveset ? (uint32_t(base[moveset + (slot + 2) * 2]) << 8 | base[moveset + (slot + 2) * 2 + 1]) : 0;
    REXLOG_WARN("[svr2011] moveset: slot {} move {} not in its list (caller {:08X}) - none shown", slot, id, lr);
  }
}

// "Is this motion streamed (YMBs)": sub_82154F00(id) -> its byte in the game's
// motion flag table (*0x82DDBBE4, bound *0x82DDBBE0, loaded from a data file:
// set for the stock YMBs ids, 9000-9779). The motion code goes by it - 0 sends
// a YMBs motion down the keyframe path (sub_82309798 -> 82394360, sub_823B8F30):
// a pack's new YMBs taunt (3894, even with stock 9602's own data) held one
// pose its whole length. A pack's YMBs motion ids answer 1.
REX_EXTERN(__imp__sub_82154F00);
REX_HOOK_RAW(sub_82154F00) {
  const uint32_t id = ctx.r3.u32 & 0x7FFF;
  __imp__sub_82154F00(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !g_ymbs_motion[id].load(std::memory_order_relaxed)) return;
  ctx.r3.u64 = 1;
  static std::atomic<bool> told[0x8000] = {};
  if (!told[id].exchange(true)) REXLOG_INFO("[svr2011] move packs: motion {} streamed (YMBs, a pack's)", id);
}

// A motion's face event (op 68: [68, n, frames]; sub_823C3F98 -> sub_823B3130 -> here with r4 = n - 1) looks
// n - 1 up in the game's table at 0x82DA1540 - 18 entries (0-17; what follows is other data) - and reads the
// face's s16 at that entry. A ported motion with n past 18 read far outside it: the match thread faulted and
// both wrestlers stayed in the move (report 001255: The Brian Kendrick and Matt Striker in motion 7430, entry
// 126 = 0x41000000). Such an event is skipped (the face keeps its expression) and logged once a motion.
constexpr uint32_t kFaceEntries = 18;
thread_local uint32_t t_face_char = 0;  // (the character whose op 68 is running: sub_823C3F98's r4, for the log)
REX_EXTERN(__imp__sub_823C3F98);
REX_HOOK_RAW(sub_823C3F98) {
  t_face_char = ctx.r4.u32;
  __imp__sub_823C3F98(ctx, base);
  t_face_char = 0;
}
REX_EXTERN(__imp__sub_823B6EE0);
REX_HOOK_RAW(sub_823B6EE0) {
  // Test aids: SVR2011_TEST_FACE_INDEX=<n> - every face event (op 68) asks for entry n, as a broken ported
  // motion's did; SVR2011_TEST_FACE_GUARD=0 - no guard (the report's fault).
  static const int test_index = std::getenv("SVR2011_TEST_FACE_INDEX") ? std::atoi(std::getenv("SVR2011_TEST_FACE_INDEX")) : -1;
  static const bool guard = [] { const char* v = std::getenv("SVR2011_TEST_FACE_GUARD"); return !v || *v != '0'; }();
  if (test_index >= 0 && t_face_char) ctx.r4.u64 = uint32_t(test_index);
  // (n = 0, r4 -1: the game's own case, left alone)
  if (!guard || ctx.r4.u32 < kFaceEntries || ctx.r4.u32 == 0xFFFFFFFFu) {
    __imp__sub_823B6EE0(ctx, base);
    return;
  }
  const auto rd = [&](uint32_t a) {
    const uint8_t* p = base + a;
    return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
  };
  uint32_t person = 10, motion = 0;
  for (uint32_t i = 0; i < 10 && t_face_char; ++i)
    if (rd(0x82E3CC50 + i * 4) == t_face_char) person = i, motion = rd(t_face_char + 212);
  static std::mutex m;
  static std::set<uint64_t> told;
  std::lock_guard lock(m);
  if (told.size() < 64 && told.insert(uint64_t(motion) << 32 | ctx.r4.u32).second)
    REXLOG_WARN("[svr2011] motion event: face {} past the game's {} (person {}, motion {}) - skipped", ctx.r4.u32,
                kFaceEntries, person, motion);
}

// The merge lock's owner check (MergeLock above).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

uint32_t OwnPid() {
#if defined(_WIN32)
  return uint32_t(GetCurrentProcessId());
#else
  return uint32_t(getpid());
#endif
}

bool ProcessRunning(uint32_t pid) {
#if defined(_WIN32)
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;  // (there, but not ours to look at: still running)
  DWORD code = 0;
  const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return running;
#else
  return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

}  // namespace
