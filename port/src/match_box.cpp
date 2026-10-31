// A match's black box (match_box.h, docs/LOG_DIAGNOSTICS.md). The world update's own thread writes it (after the
// update's jobs: MatchTypesUpdate), so what it notes is the same frame for everyone; readers (the watchdog thread's
// match end and stuck checks, the crash reporter) take the lock - the crash reporter only tries it. Characters: the
// match's list at 0x82E3CC50 (ten pointers; +212 the motion, +24 its frame, +288 / +292 / +296 the root position,
// +1156 the person: 0-5 wrestlers, 6 the referee, 7 / 8 the announcers). Weapons: 72 object slots at 0x82DE0220
// (+80 the model number, +44 the holder: a person, 0xFF none).
#include "match_box.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "arena_mods.h"
#include "match_diag.h"
#include "match_record.h"
#include "move_packs.h"
#include "p2p.h"

REXCVAR_DECLARE(int32_t, frame_rate);
REXCVAR_DECLARE(int32_t, cpu_reversal_chance);
REXCVAR_DECLARE(int32_t, cpu_reversal_rates);
REXCVAR_DECLARE(int32_t, cpu_damage_level);
REXCVAR_DECLARE(int32_t, cpu_pin_level);
REXCVAR_DECLARE(int32_t, cpu_momentum_level);

namespace svr2011 {
namespace {

constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C, kLive = 0x82E3DE00;
constexpr uint32_t kObjects = 0x82DE0220, kObjectSlots = 72;
constexpr uint32_t kMotion = 212, kMotionFrame = 24, kPerson = 1156;
constexpr int kPeople = 10, kMotions = 32, kEvents = 64;
constexpr uint32_t kStanding = 20500;
bool Lying(uint32_t m) { return m >= 430 && m <= 448; }  // (match_diag.cpp: 473-478, selling, stand up straight in stock too)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

struct MotionNote {
  uint32_t clock = 0, motion = 0, slot = 0, move = 0xFFFF;
  float frame = 0;
};
struct Person {
  uint32_t ch = 0, person = 0xFF;
  std::array<MotionNote, kMotions> ring{};
  uint32_t n = 0;  // (notes made; the ring holds the last kMotions)
  float x = 0, y = 0, z = 0;
  uint32_t motion = 0;
  const MotionNote& at(uint32_t k) const { return ring[k % kMotions]; }
};
struct Event {
  uint32_t clock = 0;
  std::string text;
};

std::mutex g_box;
uint8_t* g_base = nullptr;
bool g_on = false;           // (a match is being noted)
uint32_t g_last_clock = 0;
std::array<Person, kPeople> g_people;
std::array<Event, kEvents> g_events;
uint32_t g_event_n = 0;
uint32_t g_rule = 0xFFFFFFFF;
std::array<uint32_t, kObjectSlots> g_object{}, g_holder{};
std::map<uint16_t, std::string> g_pack_of;  // move id -> the pack (its folder), this match
std::array<uint8_t, kPeople> g_told_teleport{}, g_told_nan{}, g_told_missing{};
uint32_t g_told_refloop = 0, g_logged = 0;
std::vector<std::string> g_logged_why;
std::array<std::atomic<uint32_t>, 16> g_missed{};  // motion ids a bank list missed (look-ups: any thread)
std::array<std::atomic<uint32_t>, 65536 / 32> g_found{};  // motion ids some bank list had, this match (a bit each)
bool g_test_crash = false;  // (SVR2011_TEST_BOX=crash: a fault once the update is done)
std::atomic<uint32_t> g_test_missing{0};  // (SVR2011_TEST_BOX=missing: this id counts as never found)
bool Found(uint32_t id) {
  return id != g_test_missing.load(std::memory_order_relaxed) &&
         (g_found[(id & 0xFFFF) / 32].load(std::memory_order_relaxed) >> (id % 32) & 1);
}
bool g_restart = false;
bool g_referee = false;  // (a referee was seen this match: not the menu's attract match)  // (the match clock went back to 0: the bell after the entrances - the box starts again)
uint32_t g_match_number = 0;

void AddEvent(uint32_t clock, std::string text) {
  g_events[g_event_n % kEvents] = {clock, std::move(text)};
  ++g_event_n;
}

std::string MoveText(uint32_t move, uint32_t slot) {
  if (move == 0xFFFF) return "-";
  const auto it = g_pack_of.find(uint16_t(move));
  return fmt::format("{} slot {:03X} {}", move, slot * 2, it != g_pack_of.end() ? "pack " + it->second : std::string("game"));
}

const char* State(uint32_t m) { return Lying(m) ? "lying" : m == kStanding ? "standing" : "-"; }

void StartMatch(uint8_t* base) {
  g_people = {}, g_events = {}, g_event_n = 0, g_rule = 0xFFFFFFFF;
  g_object.fill(0), g_holder.fill(0xFF);
  g_told_teleport.fill(0), g_told_nan.fill(0), g_told_missing.fill(0), g_told_refloop = 0, g_logged = 0, g_logged_why.clear();
  for (auto& m : g_missed) m = 0;
  for (auto& f : g_found) f.store(0, std::memory_order_relaxed);
  g_test_missing = 0;
  g_pack_of.clear();
  g_referee = false;
  for (const auto& [kind, folder] : MovePacks())  // (once a match: which pack each pack move comes from)
    for (const uint16_t id : MovePackMoves(kind, folder)) g_pack_of.emplace(id, folder);
  ++g_match_number;
  (void)base;
}

// A detector fired: the box into the log (compact) - outside the lock.
std::vector<std::string> g_pending;

void Detect(const std::string& why) { g_pending.push_back(why); }

// Test aid: SVR2011_TEST_BOX=<what>[,<match clock>] - once a match (clock 600, from the bell), a detector's case made up for
// person 0 (teleport: lying 440 then standing; nan: a NaN position; missing: its motion missed by every bank;
// refloop: the referee's count step repeating) or a crash (crash: a fault on the world update's thread).
void TestBox(uint32_t clock, bool referee, Person*& p0, bool& nan_test) {
  static const char* v = std::getenv("SVR2011_TEST_BOX");
  static size_t done = 0;
  static bool bell = false;  // (the clock counted from the bell: it also runs through the entrances, then resets)
  if (!v) return;
  if (clock < 60) done = 0;
  if (clock <= 2) bell = referee;
  if (!bell) return;
  char list[64] = {};
  int at = 600;
  std::sscanf(v, "%63[a-z+],%d", list, &at);
  std::vector<std::string> cases;
  for (const char* q = list; *q;) {
    const char* e = std::strchr(q, '+');
    cases.emplace_back(q, e ? size_t(e - q) : std::strlen(q));
    q = e ? e + 1 : q + std::strlen(q);
  }
  if (done >= cases.size() || clock < uint32_t(at) + uint32_t(done) * 120 || !p0 || !p0->ch) return;
  const std::string w = cases[done++];
  AddEvent(clock, "test: " + w);
  if (w == "teleport") {
    p0->ring[p0->n % kMotions] = {clock, 440, 0, 0xFFFF, 0}, ++p0->n;
    p0->ring[p0->n % kMotions] = {clock + 1, kStanding, 0, 0xFFFF, 0}, ++p0->n;
    Detect(fmt::format("person {} stood up from lying 440 with no get-up (test)", p0->person));
  } else if (w == "nan") {
    nan_test = true;
  } else if (w == "missing") {
    MatchBoxMotionLooked(p0->motion | 0x10000, false);  // (test: missed, and never found)
  } else if (w == "refloop") {  // (his motion 23004 four times now: the detector below sees it)
    for (int k = 0; k < 4; ++k) AddEvent(clock, "referee motion 23004"), AddEvent(clock, "referee motion 20500");
    AddEvent(clock, "referee motion 23004");
  } else if (w == "crash") {
    g_test_crash = true;  // (after the update lets the box go: the crash report reads it)
  }
}

}  // namespace

void MatchBoxMotionLooked(uint32_t id, bool found) {
  const bool test = id & 0x10000;  // (SVR2011_TEST_BOX=missing: counts as never found)
  id &= 0xFFFF;
  if (!id) return;
  if (found) {
    auto& w = g_found[id / 32];
    if (!(w.load(std::memory_order_relaxed) >> (id % 32) & 1)) w.fetch_or(1u << (id % 32), std::memory_order_relaxed);
    return;
  }
  if (test) g_test_missing = id;
  for (auto& m : g_missed) {
    uint32_t was = 0;
    if (m.compare_exchange_strong(was, id) || was == id) return;
  }
}

void MatchBoxUpdate(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (!base) return;
  const uint32_t clock = Rd32(base + kMatchFrames);
  const uint32_t c0 = Rd32(base + kChars);
  {
    std::lock_guard lock(g_box);
    g_base = base;
    if (!c0 || c0 < 0x40000000u) {  // (no match running)
      g_last_clock = clock;
      return;
    }
    if (!clock) {  // (the clock before the bell, or reset at it: the box starts with the clock's run)
      g_restart = g_on;
      g_last_clock = 0;
      return;
    }
    if (!g_on || g_restart || clock < g_last_clock) StartMatch(base), g_on = true, g_restart = false;
    g_last_clock = clock;
    // The rule (changes: a match type's own steps - THREE STAGES OF HELL's falls ...)
    if (const uint32_t rule = base[kLive]; rule != g_rule) {
      AddEvent(clock, fmt::format("rule {:02X}{}", rule, g_rule == 0xFFFFFFFF ? "" : fmt::format(" (was {:02X})", g_rule)));
      g_rule = rule;
    }
    Person* p0 = nullptr;
    bool nan_test = false, referee = false;
    for (int i = 0; i < kPeople; ++i) {
      Person& p = g_people[size_t(i)];
      const uint32_t ch = Rd32(base + kChars + uint32_t(i) * 4);
      if (ch < 0x40000000u || (ch & 3)) {
        p.ch = 0;
        continue;
      }
      if (p.ch != ch) p = {}, p.ch = ch;
      p.person = Rd32(base + ch + kPerson);
      if (p.person == 6) g_referee = referee = true;
      if (p.person == 0) p0 = &p;
      p.x = RdF(base + ch + 288), p.y = RdF(base + ch + 292), p.z = RdF(base + ch + 296);
      const uint32_t motion = Rd32(base + ch + kMotion);
      const uint32_t prev = p.n ? p.at(p.n - 1).motion : ~0u;
      p.motion = motion;
      if (motion == prev) continue;
      uint32_t slot = 0, move = 0xFFFF;
      LastMoveOf(ch, slot, move);
      p.ring[p.n % kMotions] = {clock, motion, slot, move, RdF(base + ch + kMotionFrame)};
      ++p.n;
      if (p.person == 6) AddEvent(clock, fmt::format("referee motion {}", motion));
      // lying -> standing with no get-up between (the "falls, then pops up standing" reports)
      if (p.person < 6 && motion == kStanding && prev != ~0u && Lying(prev) && g_told_teleport[size_t(i)] < 2) {
        ++g_told_teleport[size_t(i)];
        Detect(fmt::format("person {} stood up from lying {} with no get-up", p.person, prev));
      }
    }
    TestBox(clock, referee, p0, nan_test);
    // A NaN or far-away position (a wrestler thrown out of the world)
    for (int i = 0; i < kPeople; ++i) {
      Person& p = g_people[size_t(i)];
      if (!p.ch || g_told_nan[size_t(i)]) continue;
      const bool bad = !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > 2000.f ||
                       std::fabs(p.z) > 2000.f || std::fabs(p.y) > 1000.f || (nan_test && &p == p0);
      if (bad) g_told_nan[size_t(i)] = 1, Detect(fmt::format("person {} at a NaN / far position ({}, {}, {}){}", p.person, p.x,
                                                             p.y, p.z, nan_test && &p == p0 ? " (test)" : ""));
    }
    // A person in a motion no bank has (a look-up of it missed)
    for (auto& m : g_missed) {
      const uint32_t id = m.load(std::memory_order_relaxed);
      if (!id || Found(id)) continue;  // (another bank list had it: not missing)
      for (int i = 0; i < kPeople; ++i) {
        Person& p = g_people[size_t(i)];
        if (p.ch && p.motion == id && g_told_missing[size_t(i)] < 1) {
          g_told_missing[size_t(i)] = 1;
          AddEvent(clock, fmt::format("motion {} missed by every bank (person {})", id, p.person));
          Detect(fmt::format("person {} is in motion {}, which no bank has", p.person, id));
        }
      }
    }
    // The referee's motions repeating a step (the "count goes 1, 2, 2, 2 ..." reports): one motion (not standing) four
    // times within 6 s - his idle 191 / 192 come 4-9 s apart, a count's steps under a second
    if (g_told_refloop < 2) {
      std::vector<std::pair<uint32_t, uint32_t>> ref;  // (motion, clock), newest first
      for (uint32_t k = g_event_n; k-- > 0 && g_event_n - k <= kEvents && ref.size() < 16;) {
        const Event& e = g_events[k % kEvents];
        if (e.text.rfind("referee motion ", 0) == 0) ref.emplace_back(uint32_t(std::atoi(e.text.c_str() + 15)), e.clock);
      }
      if (!ref.empty() && ref[0].first != kStanding && ref[0].second == clock) {
        int same = 0;
        for (const auto& [m, c] : ref)
          if (m == ref[0].first && clock - c <= 360) ++same;
        if (same >= 4) {
          ++g_told_refloop;
          Detect(fmt::format("the referee repeats motion {} {} times within 6 s", ref[0].first, same));
        }
      }
    }
    // Weapons, every 10 updates: made, picked up, dropped, gone (broken / removed). The holder is the person whose
    // held object (+1828) is the weapon's id (+1152); the weapon's +44 is only its state (8 taken up, 12 in hand, 2 ...),
    // which the box once printed as the person (a 1v1 online box: "picked up by person 8 / 12").
    const auto Holder = [&](uint32_t o) -> uint32_t {
      const uint32_t id = Rd32(base + o + 1152);
      if (id == ~0u) return 0xFF;  // (-1: a person's "nothing held")
      for (uint32_t p = 0; p < 9; ++p)
        if (const uint32_t c = Rd32(base + kChars + p * 4); c && Rd32(base + c + 1828) == id) return p;
      return 0xFF;
    };
    if (clock % 10 == 0)
      for (uint32_t i = 0; i < kObjectSlots; ++i) {
        const uint32_t o = Rd32(base + kObjects + i * 4);
        const uint32_t model = o ? Rd32(base + o + 80) : 0;
        if (o && model >= 1000) continue;  // (not a weapon)
        if (o != g_object[i]) {
          if (g_object[i] && !o) AddEvent(clock, fmt::format("weapon slot {} gone", i));
          if (o && clock > 60) AddEvent(clock, fmt::format("weapon slot {} made (model {})", i, model));
          g_object[i] = o, g_holder[i] = o ? Holder(o) : 0xFF;
          continue;
        }
        if (!o) continue;
        const uint32_t h = Holder(o);
        if (h != g_holder[i])
          AddEvent(clock, h == 0xFF ? fmt::format("weapon slot {} (model {}) dropped by person {}", i, model, g_holder[i])
                                    : fmt::format("weapon slot {} (model {}) picked up by person {}", i, model, h));
        g_holder[i] = h;
      }
  }
  std::vector<std::string> pending;
  pending.swap(g_pending);
  for (const auto& why : pending) MatchBoxLog(why.c_str());
  if (g_test_crash) {
    REXLOG_WARN("[svr2011] box: test - a fault on the world update's thread");
    rex::FlushLogging();
    *static_cast<volatile int*>(nullptr) = 1;
  }
}

std::string MatchBoxText(const char* why, bool full) {
  // (names and mods: the match line's, match_diag.cpp - before the box's lock)
  const auto people = g_base ? MatchPeople(g_base) : std::vector<MatchPerson>{};
  std::lock_guard lock(g_box);
  if (!g_on) return {};
  uint8_t* base = g_base;
  std::string s = fmt::format("match box ({}), match {}, clock {}\n", why, g_match_number, g_last_clock);
  const std::string manifest = ArenaModManifest();
  std::string arena_mod;
  if (const size_t at = manifest.find("name="); at != std::string::npos)
    arena_mod = manifest.substr(at + 5, manifest.find('\n', at) - at - 5);
  s += fmt::format("  rule {:02X}, arena {}{}, frame rate {}, {}, CPU tab: reversal chance {} rates {} damage {} pin {} momentum {}"
                   " (-1: the game's)\n",
                   base ? base[kLive] : 0, base ? Rd32(base + kLive + 64) : 0, arena_mod.empty() ? "" : " - mod " + arena_mod,
                   REXCVAR_GET(frame_rate), P2POnlineMode() ? "online" : "offline", REXCVAR_GET(cpu_reversal_chance),
                   REXCVAR_GET(cpu_reversal_rates), REXCVAR_GET(cpu_damage_level), REXCVAR_GET(cpu_pin_level),
                   REXCVAR_GET(cpu_momentum_level));
  const uint32_t show = full ? kMotions : 8;
  for (const Person& p : g_people) {
    if (!p.ch) continue;
    std::string name = "?", mod;
    for (const auto& m : people)
      if (m.person == p.person) name = m.name, mod = m.mod;
    s += fmt::format("  person {} {}{}: motion {} ({}), at ({:.0f}, {:.0f}, {:.0f})\n", p.person, name,
                     mod.empty() ? "" : " - mod " + mod, p.motion, State(p.motion), p.x, p.y, p.z);
    std::string seq;
    std::vector<std::string> moves;
    for (uint32_t k = p.n > show ? p.n - show : 0; k < p.n; ++k) {
      const MotionNote& m = p.at(k);
      seq += fmt::format("{}{}@{}", seq.empty() ? "" : " ", m.motion, m.clock);
      const std::string mv = m.move ? MoveText(m.move, m.slot) : std::string("-");
      if (mv == "-") continue;
      moves.erase(std::remove(moves.begin(), moves.end(), mv), moves.end());
      moves.push_back(mv);
    }
    s += "    motions (motion@clock): " + seq + "\n";
    if (moves.size() > 8) moves.erase(moves.begin(), moves.end() - 8);
    std::string ml;
    for (const auto& m : moves) ml += (ml.empty() ? "" : " | ") + m;
    s += "    moves: " + (ml.empty() ? std::string("-") : ml) + "\n";
  }
  const uint32_t ev = full ? kEvents : 12;
  std::string evs;
  for (uint32_t k = g_event_n > ev ? g_event_n - ev : 0; k < g_event_n; ++k)
    evs += fmt::format("    {} {}\n", g_events[k % kEvents].clock, g_events[k % kEvents].text);
  s += "  events (clock text):\n" + (evs.empty() ? std::string("    -\n") : evs);
  return s;
}

void MatchBoxLog(const char* why) {
  {
    std::lock_guard lock(g_box);
    if (!g_on || g_logged >= 3) return;
    const std::string key = why;
    if (std::find(g_logged_why.begin(), g_logged_why.end(), key) != g_logged_why.end()) return;
    g_logged_why.push_back(key);
    ++g_logged;
  }
  const std::string text = MatchBoxText(why, false);
  MatchRecordBox(text);  // (the match's folder: box.txt - match_record.h)
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    REXLOG_WARN("[svr2011] box: {}", text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos) break;
    at = end + 1;
  }
}

void MatchBoxMatchEnded() {
  {
    std::lock_guard lock(g_box);  // (the menu's attract match - no referee - isn't written)
    REXLOG_INFO("[svr2011] box: the match's end (match {}, {}{})", g_match_number, g_on ? "noted" : "not noted",
                g_referee ? "" : ", no referee: not written");
    if (!g_on || !g_referee) return;
  }
  const std::string text = MatchBoxText("the match's end", true);
  {
    std::lock_guard lock(g_box);
    g_on = false;
  }
  MatchRecordEnded(text);  // (the match's folder: box.txt, match.json complete, done.txt - match_record.h)
  if (text.empty()) return;
  namespace fs = std::filesystem;
  const std::string log = rex::cvar::Query<std::string>("log_file");
  std::error_code ec;
  // (the log's folder: log_file's, else the SDK's own sequential logs in <the current folder>\logs - on Android
  // <the game folder>/logs, the current one being "/")
#if defined(__ANDROID__)
  const fs::path dir = log.empty() ? rex::filesystem::GetExecutableFolder() / "logs" : fs::u8path(log).parent_path();
#else
  const fs::path dir = log.empty() ? fs::current_path(ec) / "logs" : fs::u8path(log).parent_path();
#endif
  // logs\matchbox_<n>.txt: the newest 3 kept (1 newest)
  for (int k = 3; k > 1; --k) fs::rename(dir / fmt::format("matchbox_{}.txt", k - 1), dir / fmt::format("matchbox_{}.txt", k), ec);
  if (FILE* f = std::fopen((dir / "matchbox_1.txt").string().c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    REXLOG_INFO("[svr2011] box: the match's box in {} ({} bytes)", (dir / "matchbox_1.txt").string(), text.size());
  }
}

void MatchBoxCrashText(FILE* f) {
  MatchRecordCrashText(f);  // (the match's folder, marked unfinished: match_record.h)
  if (!f || !g_box.try_lock()) return;  // (a crash inside the box's own work: no box)
  const bool on = g_on;
  g_box.unlock();
  if (!on) return;
  const std::string text = MatchBoxText("a crash", true);
  std::fputs(text.c_str(), f);
}

}  // namespace svr2011
