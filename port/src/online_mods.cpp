// WWE SmackDown vs. Raw 2011 - superstar and arena mods in online lobbies
// (see online_mods.h).
//
// The lobby (L = **0x82EDE924, online_cpu.cpp) keeps 280-byte slot records
// (+136 the superstar, +172 the panel's u16 copy) and 15044-byte superstar
// blocks (+4.. the 15040 the game sends; +32 there the superstar, u16). The
// host takes a pick (sub_828AE250: sub_828A5F58 says whether it can be had)
// and a block (sub_828AE378 -> sub_828A6078), then sends the slot's record
// (sub_828ADDF8 on the others) and block (sub_828AE4C8) to everyone: the
// host's lobby is in the host's numbering, a client's in its own - each
// arrival is translated from its sender's.
//
// Each game's mods go to the lobby's games every 2 s (p2p.h P2PSendMods):
//   u8 1, u64 session, u64 xuid, u8 host, name, arena key + u8 tile (the
//   host's custom arena; "" none), u16 superstars {u16 id, key, label},
//   u16 arenas {key, label}  (strings: u8 length + bytes), then (games of later versions) move packs, the host's
//   agreed packs + an echo, backstage mods, weapon mods {key, label} and the host's agreed mod weapons {key, u8 n}
// (a table over 1000 bytes goes in parts: u8 3, ... - SendTable) and the host tells a refused picker why: u8 2,
// u64 session, u64 to, text.
// Test aids: SVR2011_TEST_LOBBY_LOG logs the tables and translations;
// SVR2011_TEST_MOD_HIDE=<folder>[,<folder>] leaves mods out of this game's
// table (as if not installed).

#include "online_mods.h"
#include "no_mods.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "arena_mods.h"
#include "fps_overlay.h"
#include "frame_rate.h"
#include "match_types.h"
#include "move_packs.h"
#include "online_rules.h"
#include "online_files.h"
#include "p2p.h"
#include "superstar_mods.h"
#include "weapon_mods.h"
#include "weapons_page.h"
#include "../modmaker/svrfmt/bytes.h"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kLobbyPtr = 0x82EDE924;  // -> -> L
constexpr uint32_t kGss = 0x82EDE910;       // -> the network library (sub_8288EC28: a net player's online id)
constexpr uint32_t kSlots = 204, kSlotSize = 280;
constexpr uint32_t kXuids = 0x2CF20;        // L + kXuids + 8 * slot: the slot's player
constexpr uint32_t kNone = 512;             // (no superstar)

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32, svrfmt::Wr16;

bool LogOn() {
  static const bool on = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  return on;
}

uint64_t MyXuid() {
  static const uint64_t x = std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16);
  return x;
}

// -- this game's mods -----------------------------------------------------------------

struct Mine {
  std::map<uint32_t, std::string> key_of;   // superstar id -> key
  std::map<std::string, uint32_t> id_of;    // key -> superstar id
  std::map<std::string, std::string> label;  // key -> "Sabu 1.3" (superstars and arenas)
  std::map<std::string, std::string> arena_folder;  // arena key -> folder
  std::map<std::string, std::pair<std::string, std::string>> pack_of;  // move pack key -> (kind, folder)
  std::set<std::string> backstage;  // backstage mod keys ("bs:" + its key)
  std::map<std::string, int> weapon_id;  // weapon mod key -> its weapon id here (weapon_mods.h)
};

std::set<std::string> Hidden() {
  std::set<std::string> out;
  if (const char* v = std::getenv("SVR2011_TEST_MOD_HIDE"); v && *v) {
    std::stringstream ss(v);
    std::string f;
    while (std::getline(ss, f, ',')) out.insert(f);
  }
  return out;
}

const Mine& MyMods() {
  if (svr2011::NoMods()) {
    static const Mine none;
    return none;
  }  // (Start Without Mods: no_mods.h)
  static const Mine mine = [] {
    Mine m;
    const auto hidden = Hidden();
    for (uint32_t id : svr2011::SuperstarModIds()) {
      const std::string folder = svr2011::SuperstarModFolder(id);
      std::string key, label;
      if (folder.empty() || hidden.count(folder) || !svr2011::ModKey("Superstars", folder, &key, &label)) continue;
      m.key_of[id] = key, m.id_of[key] = id, m.label[key] = label;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(rex::filesystem::GetExecutableFolder() / "Mods" / "Arenas", ec)) {
      const std::string folder = e.path().filename().string();
      std::string key, label;
      if (!e.is_directory() || hidden.count(folder) || !svr2011::ModKey("Arenas", folder, &key, &label)) continue;
      m.label[key] = label, m.arena_folder[key] = folder;
    }
    // (move packs: Mods/Moves/<pack>, a superstar mod's moves/ - by its mod's key; "pack:" keys)
    for (const auto& [kind, folder] : svr2011::MovePacks()) {
      std::string key, label;
      if (hidden.count(folder) || !svr2011::ModKey(kind, folder, &key, &label)) continue;
      m.pack_of["pack:" + key] = {kind, folder}, m.label["pack:" + key] = label;
    }
    for (const auto& e : fs::directory_iterator(rex::filesystem::GetExecutableFolder() / "Mods" / "Backstage", ec)) {
      if (svr2011::BackstageModsOff()) break;  // (no_mods.h: none offered)
      const std::string folder = e.path().filename().string();
      std::string key, label;
      if (!e.is_directory() || hidden.count(folder) || !svr2011::ModKey("Backstage", folder, &key, &label)) continue;
      m.backstage.insert("bs:" + key), m.label["bs:" + key] = label;
    }
    if (svr2011::OnlineModsAllowed())  // (weapon mods: by their own keys - not in the gameplay set, online_files.h)
      for (const auto& w : svr2011::WeaponMods())
        if (w.extreme && !w.key.empty() && !hidden.count(w.folder)) m.weapon_id[w.key] = w.id, m.label[w.key] = w.name;
    REXLOG_INFO("online mods: {} superstar and {} arena mods, {} move packs, {} weapon mods for online lobbies",
                m.key_of.size(), m.arena_folder.size(), m.pack_of.size(), m.weapon_id.size());
    return m;
  }();
  return mine;
}

// -- the other games' ---------------------------------------------------------------------

struct Theirs {
  std::string name;
  bool host = false;
  std::map<uint32_t, std::string> key_of;  // their superstar id -> key
  std::map<std::string, uint32_t> id_of;
  std::map<std::string, std::string> label;
  std::set<std::string> arenas;            // arena keys
  std::string arena;                       // (host) the session's custom arena key, "" none
  int tile = -1;
  std::set<std::string> packs;             // its move pack keys
  std::vector<std::string> agreed;         // (the host) the packs every player has, for the match
  std::set<std::string> backstage;         // its backstage mod keys
  bool backstage_known = false;
  std::set<std::string> weapons;           // its weapon mod keys
  std::vector<std::pair<std::string, int>> wpicks;  // (the host) its mod weapons every player has: {key, count}
  bool weapons_known = false;
  uint64_t echo = 0;                       // (a client) the host's agreed list it has (its hash)
  bool packs_known = false;                // (it sent its packs: a game of this version)
  Clock::time_point heard;
  bool told = false;                       // (its CHAT line said)
};

std::mutex g_mutex;
uint64_t g_session = 0;
std::map<uint64_t, Theirs> g_theirs;  // by online id
std::vector<std::string> g_chat;      // CHAT lines to add (the game's thread)
std::string g_arena_lacking;          // (a client) the session's arena this game lacks: its label
uint64_t g_host = 0;                  // the session's host
bool g_retranslate = false;           // (a client: the host's table just came - the records before it in its numbering)
// The move packs this lobby's match plays: every player has them (the host decides, everyone echoes - the
// match starts only when all have the same list). A match's move from another pack plays its category's
// default on every game (OnlinePackMoveAllowed). Kept while a match runs.
std::vector<std::string> g_agreed;
uint64_t g_agreed_hash = 0;
std::string g_off_said;               // (the CHAT line about packs off, once a change)
// The host's mod weapons for this lobby's Extreme Rules matches that every player has ({key, count}, sorted): the
// host decides, everyone echoes them with the packs (Echo) - the WEAPONS page's choice online (weapons_page.h).
std::vector<std::pair<std::string, int>> g_wpicks;
// (the host) A player's echo not matching since (its xuid -> when first seen so); past 30 s this lobby's match plays
// with move packs and mod weapons off (g_packs_off: the list every game can take - none), the CHAT says whose.
std::map<uint64_t, Clock::time_point> g_echo_since;
bool g_packs_off = false;
// When the lobby screen's frame last ran (OnlineModsFrame; Clock ticks): the move packs list is decided and taken
// only while the lobby shows - kept from the match's load to the lobby's return. (Not !InMatch(): only a menu
// choice turns that off, and an online match goes back to the lobby without one.)
std::atomic<Clock::rep> g_lobby_frame{0};
bool LobbyShows() {
  return Clock::now().time_since_epoch() - Clock::duration(g_lobby_frame.load()) < std::chrono::milliseconds(1500);
}

uint64_t HashKeys(const std::vector<std::string>& keys) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const auto& k : keys) {
    for (unsigned char c : k) h = (h ^ c) * 0x100000001B3ull;
    h = (h ^ 0xFF) * 0x100000001B3ull;
  }
  return h | 1;  // (never 0: "none yet")
}

// The moves the agreed packs give (a match's lookup, rebuilt outside matches).
std::shared_ptr<const std::vector<bool>> g_allowed_moves;
std::atomic<bool> g_restricted{false};  // (an online match session: moves off agreed packs play defaults)

void SetAgreed(std::vector<std::string> keys) {  // (under g_mutex)
  std::sort(keys.begin(), keys.end());
  if (keys == g_agreed && g_agreed_hash) return;
  g_agreed = std::move(keys);
  g_agreed_hash = HashKeys(g_agreed);
  auto allowed = std::make_shared<std::vector<bool>>(65536, false);
  for (const auto& k : g_agreed) {
    const auto it = MyMods().pack_of.find(k);
    if (it == MyMods().pack_of.end()) continue;
    for (const uint16_t id : svr2011::MovePackMoves(it->second.first, it->second.second)) (*allowed)[id] = true;
  }
  std::atomic_store(&g_allowed_moves, std::shared_ptr<const std::vector<bool>>(std::move(allowed)));
  // (the CHAT: this game's packs off for the match)
  std::vector<std::string> off;
  for (const auto& [key, pack] : MyMods().pack_of)
    if (!std::binary_search(g_agreed.begin(), g_agreed.end(), key)) off.push_back(MyMods().label.at(key));
  std::string line;
  if (!off.empty()) {
    line = g_packs_off ? "Move packs off for this match (a player's game didn't take the list): "
                       : "Move packs off for this match (not every player has them): ";
    for (size_t i = 0; i < off.size(); ++i) {
      if (line.size() + off[i].size() > 90) {
        line += " +" + std::to_string(off.size() - i);
        break;
      }
      line += (i ? ", " : "") + off[i];
    }
  }
  if (line != g_off_said) {
    if (!line.empty() && !g_packs_off) g_chat.push_back(line);  // (packs off: the CHAT already says whose game)
    g_off_said = line;
    if (LogOn() || !line.empty()) REXLOG_INFO("online mods: agreed move packs {} - {}", g_agreed.size(), line.empty() ? "all of this game's" : line);
  }
}

// What a game echoes of the host's lists: the packs' hash, and the mod weapons' folded in (none: the packs' alone,
// as games without weapon mods echo); under g_mutex.
uint64_t Echo() {
  if (g_wpicks.empty() || !g_agreed_hash) return g_agreed_hash;
  uint64_t h = 0xCBF29CE484222325ull;
  for (const auto& [k, n] : g_wpicks) {
    for (unsigned char c : k) h = (h ^ c) * 0x100000001B3ull;
    h = (h ^ uint64_t(n + 1)) * 0x100000001B3ull;
  }
  return (g_agreed_hash ^ h) | 1;
}

// Strings in messages.
void PutStr(std::vector<uint8_t>& o, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 255);
  o.push_back(uint8_t(n));
  o.insert(o.end(), s.begin(), s.begin() + n);
}
void Put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v >> 8)), o.push_back(uint8_t(v)); }
void Put64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 7; i >= 0; --i) o.push_back(uint8_t(v >> (8 * i)));
}
struct In {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool need(size_t k) { return ok = ok && at + k <= n; }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  uint16_t u16() {
    if (!need(2)) return 0;
    const uint16_t v = Rd16(p + at);
    at += 2;
    return v;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[at + i];
    at += 8;
    return v;
  }
  std::string str() {
    const uint8_t k = u8();
    if (!need(k)) return "";
    std::string s(reinterpret_cast<const char*>(p + at), k);
    at += k;
    return s;
  }
};

// The custom arena this host's session plays (key, tile; "" none).
std::string HostArena(int* tile) {
  int t = -1;
  const std::string folder = svr2011::ArenaModInUse(&t);
  if (folder.empty()) return "";
  for (const auto& [key, f] : MyMods().arena_folder)
    if (f == folder) {
      if (tile) *tile = t;
      return key;
    }
  return "";
}

void SendTable(const svr2011::P2PLobby& lobby) {
  const Mine& m = MyMods();
  std::vector<uint8_t> o;
  o.push_back(1);
  Put64(o, lobby.session), Put64(o, MyXuid());
  o.push_back(lobby.host ? 1 : 0);
  PutStr(o, rex::cvar::Query<std::string>("online_name"));
  int tile = -1;
  PutStr(o, lobby.host ? HostArena(&tile) : std::string());
  o.push_back(uint8_t(tile < 0 ? 0xFF : tile));
  Put16(o, uint16_t(m.key_of.size()));
  for (const auto& [id, key] : m.key_of) Put16(o, uint16_t(id)), PutStr(o, key), PutStr(o, m.label.at(key));
  Put16(o, uint16_t(m.arena_folder.size()));
  for (const auto& [key, f] : m.arena_folder) PutStr(o, key), PutStr(o, m.label.at(key));
  Put16(o, uint16_t(m.pack_of.size()));
  for (const auto& [key, pack] : m.pack_of) PutStr(o, key), PutStr(o, m.label.at(key));
  {
    std::lock_guard lock(g_mutex);
    if (lobby.host) {
      Put16(o, uint16_t(g_agreed.size()));
      for (const auto& k : g_agreed) PutStr(o, k);
    } else {
      Put16(o, 0);
    }
    Put64(o, Echo());
  }
  Put16(o, uint16_t(m.backstage.size()));
  for (const auto& key : m.backstage) PutStr(o, key), PutStr(o, m.label.at(key));
  if (!m.weapon_id.empty()) {  // (weapon mods: only games that have some - the others lack every mod weapon)
    Put16(o, uint16_t(m.weapon_id.size()));
    for (const auto& [key, id] : m.weapon_id) PutStr(o, key), PutStr(o, m.label.at(key));
    std::lock_guard lock(g_mutex);
    Put16(o, uint16_t(lobby.host ? g_wpicks.size() : 0));
    if (lobby.host)
      for (const auto& [key, n] : g_wpicks) PutStr(o, key), o.push_back(uint8_t(n));
  }
  // (test aid: SVR2011_TEST_TABLE_PAD=<n> - n made-up superstar mods more in the table, a big mod list)
  static const int pad = [] {
    const char* v = std::getenv("SVR2011_TEST_TABLE_PAD");
    return v ? std::clamp(std::atoi(v), 0, 2000) : 0;
  }();
  if (pad) {
    const size_t at = 1 + 8 + 8 + 1 + 1 + o[18] + 1 + o[19 + o[18]] + 1;  // (the superstars' count)
    const uint16_t n = uint16_t(o[at] << 8 | o[at + 1]);
    std::vector<uint8_t> extra;
    for (int i = 0; i < pad; ++i) {
      Put16(extra, uint16_t(0xF000 + i));
      PutStr(extra, "pad_" + std::to_string(i) + "\t1.0\t00000000");
      PutStr(extra, "Test Pad Superstar " + std::to_string(i) + " 1.0");
    }
    o[at] = uint8_t((n + pad) >> 8), o[at + 1] = uint8_t(n + pad);
    o.insert(o.begin() + at + 2, extra.begin(), extra.end());
  }
  // A big table (a long mod list) goes in parts under 1200 bytes with the p2p header - one UDP packet over
  // 1400 is split by IP and some routers drop the pieces: u8 3, u64 session, u64 xuid, u64 the table's hash,
  // u8 part, u8 parts, its bytes (games before this ignore kind 3; the version check keeps them apart).
  constexpr size_t kPart = 1000;
  if (o.size() <= kPart) {
    svr2011::P2PSendMods(o);
    return;
  }
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t c : o) h = (h ^ c) * 0x100000001B3ull;
  const size_t parts = (o.size() - 1 + kPart - 1) / kPart;  // (the kind byte stays out)
  if (parts > 255) return;
  static uint64_t said = 0;
  if (said != h && LogOn()) REXLOG_INFO("online mods: this game's table {} bytes, sent in {} parts", o.size(), parts);
  said = h;
  // (test aid: SVR2011_TEST_TABLE_DROP=<part> - that part left out of the first send: the next one fills it in)
  static int drop = [] {
    const char* v = std::getenv("SVR2011_TEST_TABLE_DROP");
    return v ? std::atoi(v) : -1;
  }();
  for (size_t i = 0; i < parts; ++i) {
    if (int(i) == drop) {
      REXLOG_INFO("online mods: test - part {} of {} left out", i, parts);
      drop = -1;
      continue;
    }
    std::vector<uint8_t> c;
    c.push_back(3);
    Put64(c, lobby.session), Put64(c, MyXuid()), Put64(c, h);
    c.push_back(uint8_t(i)), c.push_back(uint8_t(parts));
    const size_t from = 1 + i * kPart, to = std::min(o.size(), from + kPart);
    c.insert(c.end(), o.begin() + from, o.begin() + to);
    svr2011::P2PSendMods(c);
  }
}

// Who in the lobby lacks a key (the others' names, with what they have of
// it instead: "EntUp (has Tazz 0.9)"); under g_mutex.
std::vector<std::string> Lacking(const std::string& key, const std::vector<uint64_t>& members, bool arena) {
  std::vector<std::string> out;
  const std::string folder = key.substr(0, key.find('\t'));
  for (uint64_t x : members) {
    if (x == MyXuid()) continue;
    const auto it = g_theirs.find(x);
    if (it == g_theirs.end()) {
      out.push_back("a player who just joined");
      continue;
    }
    const Theirs& t = it->second;
    const bool has = arena ? t.arenas.count(key) != 0 : t.id_of.count(key) != 0;
    if (has) continue;
    std::string other;  // (another version of it?)
    for (const auto& [k, l] : t.label)
      if (k.substr(0, k.find('\t')) == folder) other = l;
    out.push_back(t.name + (other.empty() ? "" : " (has " + other + ")"));
  }
  return out;
}

std::string Join(const std::vector<std::string>& v, const char* sep = ", ") {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
  return s;
}

// A table came: kept, and its CHAT line (what it lacks of this game's mods
// and what this game lacks of its) once.
void OnTable(In& r) {
  const uint64_t session = r.u64(), xuid = r.u64();
  Theirs t;
  t.host = r.u8() != 0;
  t.name = r.str();
  t.arena = r.str();
  const uint8_t tile = r.u8();
  t.tile = tile == 0xFF ? -1 : tile;
  const uint16_t n = r.u16();
  for (uint16_t i = 0; i < n && r.ok; ++i) {
    const uint16_t id = r.u16();
    const std::string key = r.str(), label = r.str();
    t.key_of[id] = key, t.id_of[key] = id, t.label[key] = label;
  }
  const uint16_t na = r.u16();
  for (uint16_t i = 0; i < na && r.ok; ++i) {
    const std::string key = r.str(), label = r.str();
    t.arenas.insert(key), t.label[key] = label;
  }
  if (r.ok && r.at < r.n) {  // (move packs: games of this version)
    const uint16_t np = r.u16();
    for (uint16_t i = 0; i < np && r.ok; ++i) {
      const std::string key = r.str(), label = r.str();
      t.packs.insert(key), t.label[key] = label;
    }
    const uint16_t ng = r.u16();
    for (uint16_t i = 0; i < ng && r.ok; ++i) t.agreed.push_back(r.str());
    t.echo = r.u64();
    t.packs_known = r.ok;
  }
  if (r.ok && r.at < r.n) {  // (backstage mods)
    const uint16_t nb = r.u16();
    for (uint16_t i = 0; i < nb && r.ok; ++i) {
      const std::string key = r.str(), label = r.str();
      t.backstage.insert(key), t.label[key] = label;
    }
    t.backstage_known = r.ok;
  }
  if (r.ok && r.at < r.n) {  // (weapon mods, the host's agreed mod weapons)
    const uint16_t nw = r.u16();
    for (uint16_t i = 0; i < nw && r.ok; ++i) {
      const std::string key = r.str(), label = r.str();
      t.weapons.insert(key), t.label[key] = label;
    }
    const uint16_t np = r.u16();
    for (uint16_t i = 0; i < np && r.ok; ++i) {
      std::string key = r.str();
      const int n = r.u8();
      t.wpicks.emplace_back(std::move(key), n);
    }
    t.weapons_known = r.ok;
  }
  if (!r.ok || !xuid || xuid == MyXuid()) return;
  if (t.name.empty()) t.name = "A player";
  std::lock_guard lock(g_mutex);
  if (session != g_session) return;  // (another session's: other games on this network)
  t.heard = Clock::now();
  auto& slot = g_theirs[xuid];
  const bool first = slot.heard == Clock::time_point{};
  t.told = slot.told;
  const bool changed = slot.key_of != t.key_of || slot.arenas != t.arenas || slot.arena != t.arena;
  slot = std::move(t);
  if (slot.host && g_host != xuid) g_host = xuid, g_retranslate = true;
  if (first && LogOn())
    REXLOG_INFO("online mods: {}'s mods: {} superstar, {} arena{}", slot.name, slot.key_of.size(), slot.arenas.size(),
                slot.arena.empty() ? "" : " (the session's arena: " + slot.label[slot.arena] + ")");
  if (slot.told && !changed) return;
  slot.told = true;
  // (what it lacks of this game's, what this game lacks of its: one line each)
  const Mine& m = MyMods();
  std::vector<std::string> they_lack, i_lack;
  for (const auto& [key, id] : m.id_of)
    if (!slot.id_of.count(key)) they_lack.push_back(m.label.at(key));
  for (const auto& [key, id] : slot.id_of)
    if (!m.id_of.count(key)) i_lack.push_back(slot.label[key]);
  auto line = [](const std::string& head, const std::vector<std::string>& v) {
    std::string s = head;
    size_t k = 0;
    for (; k < v.size(); ++k) {
      const std::string add = (k ? ", " : "") + v[k];
      if (s.size() + add.size() > 80) break;
      s += add;
    }
    if (k < v.size()) s += " +" + std::to_string(v.size() - k);
    return s;
  };
  if (!they_lack.empty()) g_chat.push_back(line(slot.name + " lacks: ", they_lack));
  if (!i_lack.empty()) g_chat.push_back(line("You lack (" + slot.name + "'s): ", i_lack));
}

void OnNote(In& r) {
  const uint64_t session = r.u64(), to = r.u64();
  const std::string text = r.str();
  {
    std::lock_guard lock(g_mutex);
    if (!r.ok || session != g_session || to != MyXuid()) return;
  }
  svr2011::ShowNotice(text, 8.0f);
}

// A big table's parts (SendTable), per sender: kept across its 2 s resends while the table stays the same (a
// lost part comes again), read as one once all came.
struct Parts {
  uint64_t hash = 0;
  std::vector<std::vector<uint8_t>> part;
  size_t got = 0;
};
std::mutex g_parts_mutex;
std::map<uint64_t, Parts> g_parts;

void OnPart(In& r) {
  const uint64_t session = r.u64(), xuid = r.u64(), hash = r.u64();
  const uint8_t i = r.u8(), n = r.u8();
  if (!r.ok || !n || i >= n || !xuid) return;
  std::vector<uint8_t> whole;
  {
    std::lock_guard lock(g_parts_mutex);
    {
      std::lock_guard lock2(g_mutex);
      if (session != g_session) return;
    }
    Parts& p = g_parts[xuid];
    if (p.hash != hash || p.part.size() != n) p = Parts{hash, std::vector<std::vector<uint8_t>>(n), 0};
    if (!p.part[i].empty() || r.at >= r.n) return;
    p.part[i].assign(r.p + r.at, r.p + r.n);
    if (++p.got < n) return;
    for (const auto& b : p.part) whole.insert(whole.end(), b.begin(), b.end());
    p = Parts{};  // (the next resend: read again)
  }
  In t{whole.data(), whole.size()};
  OnTable(t);
}

void OnMessage(const uint8_t* data, size_t size) {
  In r{data, size};
  const uint8_t kind = r.u8();
  if (kind == 1) OnTable(r);
  else if (kind == 2) OnNote(r);
  else if (kind == 3) OnPart(r);
}

// Every 2 s: this game's table to the lobby; the others' kept while they're
// in it; a client plays the host's custom arena if it has it.
void Loop() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const svr2011::P2PLobby lobby = svr2011::P2PMatchLobby();
    std::string arena, arena_label;
    int tile = -1;
    bool apply = false;
    {
      std::lock_guard lock(g_mutex);
      if (!lobby.in || lobby.session != g_session) {
        if (g_session && LogOn()) REXLOG_INFO("online mods: lobby {:016X} over", g_session);
        g_theirs.clear(), g_chat.clear(), g_arena_lacking.clear(), g_host = 0;
        g_agreed.clear(), g_agreed_hash = 0, g_off_said.clear(), g_wpicks.clear();
        g_echo_since.clear(), g_packs_off = false;
        g_session = lobby.in ? lobby.session : 0;
        apply = true;  // (back to no arena: below)
      }
      for (auto it = g_theirs.begin(); it != g_theirs.end();)
        it = std::find(lobby.members.begin(), lobby.members.end(), it->first) == lobby.members.end() ? g_theirs.erase(it)
                                                                                                      : std::next(it);
      g_restricted = lobby.in;
      const bool shows = lobby.in && LobbyShows();
      static bool showed = false;
      if (lobby.in && shows != showed && LogOn())
        REXLOG_INFO("online mods: the lobby screen {} (in a match: {}) - the move packs list {}",
                    shows ? "shows" : "is gone", svr2011::InMatch(), shows ? "follows the lobby" : "kept for the match");
      showed = shows;
      if (shows) {
        if (lobby.host) {  // (the packs every player in the lobby has)
          std::vector<std::string> keys;
          for (const auto& [key, pack] : MyMods().pack_of) {
            bool all = true;
            for (const uint64_t x : lobby.members) {
              if (x == MyXuid()) continue;
              const auto t = g_theirs.find(x);
              all = all && t != g_theirs.end() && t->second.packs.count(key);
            }
            if (all) keys.push_back(key);
          }
          if (g_packs_off) keys.clear();  // (a player's game didn't take the list: none for this match)
          SetAgreed(std::move(keys));
          // (this game's mod weapons that every player has)
          std::vector<std::pair<std::string, int>> picks;
          for (const auto& [key, n] : svr2011::HostModWeaponPicks()) {
            bool all = MyMods().weapon_id.count(key) != 0;
            for (const uint64_t x : lobby.members) {
              if (x == MyXuid()) continue;
              const auto t = g_theirs.find(x);
              all = all && t != g_theirs.end() && t->second.weapons.count(key);
            }
            if (all) picks.emplace_back(key, n);
          }
          std::sort(picks.begin(), picks.end());
          if (g_packs_off) picks.clear();
          if (picks != g_wpicks && LogOn()) REXLOG_INFO("online mods: agreed mod weapons {}", picks.size());
          g_wpicks = std::move(picks);
        } else if (auto h = g_theirs.find(g_host); h != g_theirs.end() && h->second.packs_known &&
                   !std::getenv("SVR2011_TEST_NO_ECHO")) {  // (test aid: a joiner that never takes the host's list)
          SetAgreed(h->second.agreed);
          g_wpicks = h->second.weapons_known ? h->second.wpicks : std::vector<std::pair<std::string, int>>{};
        }
      }
      if (lobby.in && !lobby.host) {
        if (auto h = g_theirs.find(g_host); h != g_theirs.end()) {
          arena = h->second.arena, tile = h->second.tile, apply = true;
          if (!arena.empty()) arena_label = h->second.label[arena];
        }
      }
    }
    if (apply && !lobby.host) {
      // (the host's custom arena here too, or none)
      const auto f = MyMods().arena_folder.find(arena);
      std::string lacking;
      if (arena.empty()) svr2011::UseArenaMod("", -1);
      else if (f != MyMods().arena_folder.end()) svr2011::UseArenaMod(f->second, tile);
      else lacking = arena_label;
      std::lock_guard lock(g_mutex);
      if (lacking != g_arena_lacking && !lacking.empty())
        g_chat.push_back("You lack this session's arena: " + lacking);
      g_arena_lacking = lacking;
    }
    if (lobby.in && lobby.members.size() >= 2) SendTable(lobby);
  }
}

// -- translation -----------------------------------------------------------------------

// A superstar id in `from`'s numbering -> this game's (kNone: a mod this game
// hasn't); under g_mutex.
uint32_t ToMine(uint64_t from, uint32_t id) {
  if (!from || from == MyXuid()) return id;
  const auto t = g_theirs.find(from);
  if (t == g_theirs.end()) return id;
  const auto k = t->second.key_of.find(id);
  if (k == t->second.key_of.end()) return id;  // (a stock superstar: the same everywhere)
  const auto m = MyMods().id_of.find(k->second);
  const uint32_t mine = m == MyMods().id_of.end() ? kNone : m->second;
  if (LogOn() && mine != id)
    REXLOG_INFO("online mods: {}'s superstar {} is {} here ({})", t->second.name, id, mine, t->second.label[k->second]);
  return mine;
}

// A net player's online id (the network library's own table).
uint64_t XuidOf(PPCContext& ctx, uint8_t* base, uint32_t net) {
  PPCContext c = ctx;
  c.r3.u64 = Rd32(base + kGss), c.r4.u64 = net;
  if (!c.r3.u32) return 0;
  sub_8288EC28(c, base);
  return c.r3.u64;
}

uint32_t Lobby(uint8_t* base) {
  const uint32_t p = Rd32(base + kLobbyPtr);
  return p ? Rd32(base + p) : 0;
}

constexpr uint32_t kBlocks = 3568, kBlockSize = 15044;
constexpr uint32_t kChat = 0x82EDE92C;  // -> the lobby's CHAT lines

// The block's superstar fields (+32 the id, u16).
void TranslateBlock(uint8_t* block, uint64_t from) {
  std::lock_guard lock(g_mutex);
  const uint32_t id = Rd16(block + 32);
  const uint32_t mine = ToMine(from, id);
  if (LogOn() && mine != id) {  // (where else the block has the id: u16 / u32, big-endian)
    std::string at;
    for (uint32_t o = 0; o + 4 <= 15040; o += 2) {
      if (Rd16(block + o) == id) at += " " + std::to_string(o);
      else if (o % 4 == 0 && Rd32(block + o) == id) at += " " + std::to_string(o) + "w";
    }
    REXLOG_INFO("online mods: block with superstar {} - the id at{}", id, at);
  }
  if (mine == id) return;
  // (the id is at +32, +218 and +228 - seen in a block for each mod; u16)
  for (uint32_t o : {32u, 218u, 228u})
    if (Rd16(block + o) == id) Wr16(block + o, uint16_t(mine == kNone ? 0 : mine));
}

thread_local bool t_refuse = false;
thread_local uint64_t t_block_from = 0;
uint32_t g_chat_text = 0;  // (guest: a CHAT line)
rex::memory::Memory* g_memory = nullptr;

}  // namespace

namespace svr2011 {

void InstallOnlineMods(rex::memory::Memory* memory) {
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  g_memory = memory;
  g_chat_text = memory->SystemHeapAlloc(128);
  SetP2PModHandler(OnMessage);
  std::thread(Loop).detach();
}

void OnlineModsPick(PPCContext& ctx, uint8_t* base, uint32_t msg) {
  t_refuse = false;
  if (!msg) return;
  const uint32_t data = Rd32(base + msg + 8);
  if (!data) return;
  const uint64_t from = XuidOf(ctx, base, Rd32(base + msg));
  const uint32_t id = Rd32(base + data);
  std::lock_guard lock(g_mutex);
  const uint32_t mine = ToMine(from, id);
  if (mine != id && mine != kNone) Wr32(base + data, mine);
  // (a mod: everyone in the lobby must have it)
  std::string key, name = "this game";
  if (mine == kNone) {
    if (auto t = g_theirs.find(from); t != g_theirs.end()) key = t->second.key_of[id], name = t->second.name;
  } else if (auto k = MyMods().key_of.find(mine); k != MyMods().key_of.end()) {
    key = k->second;
  }
  if (key.empty()) return;
  std::vector<std::string> lacking;
  if (mine == kNone) lacking.push_back(rex::cvar::Query<std::string>("online_name") + " (the host)");
  const auto rest = Lacking(key, P2PMatchLobby().members, false);
  lacking.insert(lacking.end(), rest.begin(), rest.end());
  // (the picker itself doesn't count)
  if (auto t = g_theirs.find(from); t != g_theirs.end())
    lacking.erase(std::remove_if(lacking.begin(), lacking.end(),
                                 [&](const std::string& s) { return s.rfind(t->second.name, 0) == 0; }),
                  lacking.end());
  if (lacking.empty()) return;
  t_refuse = true;
  std::string label = MyMods().label.count(key) ? MyMods().label.at(key) : key.substr(0, key.find('\t'));
  if (auto t = g_theirs.find(from); t != g_theirs.end() && t->second.label.count(key)) label = t->second.label[key];
  const std::string text = "The user/s in the lobby do not have the mods installed you do.\n" + label + ": " +
                           Join(lacking) + (lacking.size() == 1 ? " lacks it." : " lack it.");
  REXLOG_INFO("online mods: the pick of {} by {} refused - {}", label, name, Join(lacking));
  if (!from || from == MyXuid()) {
    ShowNotice(text, 8.0f);
  } else {
    std::vector<uint8_t> o;
    o.push_back(2);
    Put64(o, g_session), Put64(o, from);
    PutStr(o, text.substr(0, 255));
    P2PSendMods(o);
  }
}

void OnlineModsPickDone() { t_refuse = false; }

void OnlineModsBlockFrom(PPCContext& ctx, uint8_t* base, uint32_t msg) {
  t_block_from = msg ? XuidOf(ctx, base, Rd32(base + msg)) : 0;
}

void OnlineModsBlockStore(uint8_t* base, uint32_t data) {
  if (data && t_block_from) TranslateBlock(base + data, t_block_from);
  t_block_from = 0;
}

void OnlineModsFrame(PPCContext& ctx, uint8_t* base) {
  const uint32_t L = Lobby(base);
  if (!L) return;
  g_lobby_frame = Clock::now().time_since_epoch().count();  // (LobbyShows)
  // The lines waiting go into the lobby's CHAT (its 44-line ring at
  // *0x82EDE92C: sub_828A7B38(chat, text)).
  {
    std::vector<std::string> lines;
    const uint32_t chat = Rd32(base + kChat);
    {
      std::lock_guard lock(g_mutex);
      if (chat && g_chat_text && !g_chat.empty()) lines.swap(g_chat);
    }
    for (const std::string& l : lines) {
      char* out = g_memory->TranslateVirtual<char*>(g_chat_text);
      const size_t n = std::min<size_t>(l.size(), 91);
      std::memcpy(out, l.data(), n);
      out[n] = 0;
      PPCContext c = ctx;
      c.r3.u64 = chat, c.r4.u64 = g_chat_text;
      sub_828A7B38(c, base);
      if (LogOn()) REXLOG_INFO("online mods: CHAT: {}", l);
    }
  }
  // (a client: the records and blocks that came before the host's table)
  uint64_t host;
  {
    std::lock_guard lock(g_mutex);
    if (!g_retranslate) return;
    g_retranslate = false;
    host = g_host;
  }
  const uint32_t slots = std::min<uint32_t>(Rd32(base + L + 28), 12);
  for (uint32_t j = 0; j < slots; ++j) {
    uint8_t* rec = base + L + kSlots + j * kSlotSize;
    if (Rd32(rec) < 5) continue;
    {
      std::lock_guard lock(g_mutex);
      const uint32_t id = Rd32(rec + 136), mine = ToMine(host, id);
      if (mine != id) Wr32(rec + 136, mine), Wr16(rec + 172, uint16_t(mine == kNone ? 0 : mine));
    }
    TranslateBlock(base + L + kBlocks + j * kBlockSize, host);
  }
  if (LogOn()) REXLOG_INFO("online mods: the lobby's records read again in this game's numbering");
}

std::string OnlineModsHostName() {
  std::lock_guard lock(g_mutex);
  const auto it = g_theirs.find(g_host);
  return it != g_theirs.end() ? it->second.name : std::string();
}

std::vector<std::string> OnlineModsLackedFromHost() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> out;
  const auto it = g_theirs.find(g_host);
  if (it != g_theirs.end()) {
    const Mine& m = MyMods();
    for (const auto& [key, id] : it->second.id_of)
      if (!m.id_of.count(key)) out.push_back(it->second.label.count(key) ? it->second.label.at(key) : key);
  }
  if (!g_arena_lacking.empty()) out.push_back(g_arena_lacking);
  return out;
}

bool OnlineModsMayStart(uint8_t* base, uint32_t L) {
  if (!L) return true;
  const P2PLobby lobby = P2PMatchLobby();
  if (!lobby.in || lobby.members.size() < 2) return true;
  std::string why, detail;  // (detail: the log's only)
  {
    std::lock_guard lock(g_mutex);
    if (!g_arena_lacking.empty()) why = "You don't have this session's arena: " + g_arena_lacking;
    if (why.empty() && lobby.host) {
      int tile = -1;
      if (const std::string arena = HostArena(&tile); !arena.empty()) {
        const auto lacking = Lacking(arena, lobby.members, true);
        if (!lacking.empty()) why = Join(lacking) + " lacks this session's arena: " + MyMods().label.at(arena);
      }
    }
    if (why.empty() && lobby.host) {  // (every game plays the same move packs: they echo the host's list)
      for (const uint64_t x : lobby.members) {
        if (x == MyXuid()) continue;
        const auto t = g_theirs.find(x);
        if (t == g_theirs.end()) {
          why = "a player who just joined is still sending its mods";
          break;
        }
        // (nothing to agree - no packs, no mod weapons for this match: a game that never took a list (echo 0) plays
        // every pack move as its default too, the same as an empty list - Start Without Mods on both, 2.1.5 report;
        // either side's "none" counts: 0 (no list taken / worked out yet) or the empty list's hash)
        static const uint64_t kEmpty = HashKeys({});
        const bool none = g_agreed.empty() && g_wpicks.empty();
        const bool same =
            t->second.echo == Echo() || (none && (t->second.echo == 0 || t->second.echo == kEmpty));
        if (same) {
          g_echo_since.erase(x);
          continue;
        }
        if (t->second.packs_known) {
          auto& since = g_echo_since.emplace(x, Clock::now()).first->second;
          if (!g_packs_off && Clock::now() - since > std::chrono::seconds(30)) {
            g_packs_off = true;  // (the next frame's list is none: SetAgreed above)
            g_chat.push_back("Move packs off for this match: " + t->second.name + "'s game didn't take the list");
            REXLOG_WARN("online mods: {}'s game hasn't taken the move packs list in 30 s (echo {:016X}, this host's "
                        "{:016X}) - move packs and mod weapons off for this match", t->second.name, t->second.echo, Echo());
          }
          why = t->second.name + (g_packs_off ? "'s game doesn't answer the move packs list (it can leave and join again)"
                                              : " is still taking the move packs list");
          detail = fmt::format(" (its echo {:016X}, this host's {:016X}; packs {} / {}, weapons {} / {})", t->second.echo,
                               Echo(), t->second.packs.size(), g_agreed.size(), t->second.wpicks.size(), g_wpicks.size());
          break;
        }
      }
    }
    // (a backstage match plays its rooms with each game's backstage mods: they must be the same)
    if (why.empty() && lobby.host && BackstageRule(OnlineCreateSessionRule())) {
      for (const uint64_t x : lobby.members) {
        if (x == MyXuid()) continue;
        const auto t = g_theirs.find(x);
        if (t == g_theirs.end() || !t->second.backstage_known || t->second.backstage == MyMods().backstage) continue;
        std::vector<std::string> diff;
        for (const auto& k : MyMods().backstage)
          if (!t->second.backstage.count(k)) diff.push_back(MyMods().label.at(k));
        for (const auto& k : t->second.backstage)
          if (!MyMods().backstage.count(k)) diff.push_back(t->second.label[k]);
        why = "your backstage mods differ from " + t->second.name + "'s (" + Join(diff) +
              "): pick a match outside the backstage";
        break;
      }
    }
    const uint32_t slots = std::min<uint32_t>(Rd32(base + L + 28), 12);
    for (uint32_t j = 0; j < slots && why.empty(); ++j) {
      const uint8_t* rec = base + L + kSlots + j * kSlotSize;
      if (Rd32(rec) < 8) continue;
      const uint32_t id = Rd32(rec + 136);
      if (id == kNone) {  // (a mod this game lacks: shown as none here)
        continue;
      }
      const auto k = MyMods().key_of.find(id);
      if (k == MyMods().key_of.end()) continue;
      const auto lacking = Lacking(k->second, lobby.members, false);
      if (!lacking.empty()) why = Join(lacking) + " lacks " + MyMods().label.at(k->second) + ": pick another superstar";
    }
  }
  if (why.empty()) return true;
  static std::string told;
  static Clock::time_point told_at{};
  if (why != told || Clock::now() - told_at > std::chrono::seconds(20)) {
    ShowNotice("The match can't start: " + why, 6.0f);
    REXLOG_INFO("online mods: the match waits - {}{}", why, detail);
    told = why, told_at = Clock::now();
  }
  return false;
}

std::vector<std::pair<int, int>> OnlineModWeapons() {
  std::vector<std::pair<int, int>> out;
  std::lock_guard lock(g_mutex);
  for (const auto& [key, n] : g_wpicks)
    if (const auto it = MyMods().weapon_id.find(key); it != MyMods().weapon_id.end()) out.emplace_back(it->second, n);
  return out;
}

bool OnlinePackMoveAllowed(uint16_t id) {
  if (!g_restricted.load() || StockPlays(id)) return true;
  const auto allowed = std::atomic_load(&g_allowed_moves);
  return allowed && (*allowed)[id];
}

bool OnlineModEveryoneHas(uint32_t id) {
  const auto k = MyMods().key_of.find(id);
  if (k == MyMods().key_of.end()) return !IsSuperstarMod(id);  // (stock, or a mod left out of the table)
  const P2PLobby lobby = P2PMatchLobby();
  std::lock_guard lock(g_mutex);
  return Lacking(k->second, lobby.members, false).empty();
}

}  // namespace svr2011

// The host's "can this superstar be had" (sub_828A5F58(L, id), the pick
// handler's only): no, for a mod someone in the lobby lacks.
REX_EXTERN(__imp__sub_828A5F58);
REX_HOOK_RAW(sub_828A5F58) {
  if (t_refuse) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828A5F58(ctx, base);
}

// A client: a slot's record from the host (sub_828ADDF8(L, msg, slot): msg +8
// the record, +12 its size): its superstar in this game's numbering.
REX_EXTERN(__imp__sub_828ADDF8);
REX_HOOK_RAW(sub_828ADDF8) {
  const uint32_t msg = ctx.r4.u32;
  if (msg && Rd32(base + msg + 12) == kSlotSize) {
    if (const uint32_t rec = Rd32(base + msg + 8)) {
      std::lock_guard lock(g_mutex);
      const uint32_t id = Rd32(base + rec + 136);
      const uint32_t mine = ToMine(g_host, id);
      if (mine != id) {
        Wr32(base + rec + 136, mine);
        Wr16(base + rec + 172, uint16_t(mine == kNone ? 0 : mine));
      }
    }
  }
  __imp__sub_828ADDF8(ctx, base);
}

// A client: a slot's superstar block from the host (sub_828AE4C8(L, msg): msg
// +8 -> u32 slot, the block).
REX_EXTERN(__imp__sub_828AE4C8);
REX_HOOK_RAW(sub_828AE4C8) {
  const uint32_t msg = ctx.r4.u32;
  if (msg) {
    if (const uint32_t data = Rd32(base + msg + 8)) {
      uint64_t host;
      {
        std::lock_guard lock(g_mutex);
        host = g_host;
      }
      if (host) TranslateBlock(base + data + 4, host);
    }
  }
  __imp__sub_828AE4C8(ctx, base);
}
