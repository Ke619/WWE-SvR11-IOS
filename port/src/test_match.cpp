// WWE SmackDown vs. Raw 2011 - test aid: a match with the wrestlers and arena
// a test asks for, whatever was picked on the select screen.
//
//   SVR2011_TEST_MATCH="people=JOHN CENA,RANDY ORTON arena=11 cpu=all"
//
// attires=2,1: each person's attire (1-4, in person order; default 1). min=2: only a match with 2 or more
// picks (a Road to WrestleMania story's match, not its walk); an empty name keeps that person. exact=1: only the
// people listed (a later slot that the route's presses picked is emptied - match replay tests). ports=0,-1: each
// person's controller - a player's (0-3) or -1 the CPU (match replays: tools/replay_match.ps1).
// people: roster names (as the log's "match:" lines print them), ids, or
// CAW<N> (the Created Superstar in save slot N - enough for the entrances
// and the ring announcer; the match itself crashed once: the select screen's
// own CAW set-up isn't done), comma-separated, in person order; arena: the arena id (the "match:" line's
// "arena N"). The rule stays SVR2011_TEST_RULE's (match_types.cpp). With
// "route match" (script_input.h) a test goes from the start to that match.
//
// As the match's people are built from the picks (sub_828BC5E8(match) ->
// sub_828BBEF0: 2116-byte slots at match+432, +8 superstar id * 100 + attire,
// +54 the id, +5 the team, +4 the kind, -8 the controller - see
// match_types.cpp), the slots get the asked-for superstars; slots beyond the
// picks become CPU wrestlers of their own team. The arena: the match
// screen's choice (below).

#include <cctype>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "big_roster.h"
#include "generated/default/svr2011_init.h"
#include "arena_mods.h"
#include "fast_start.h"
#include "p2p.h"
#include "rtwm_streak.h"
#include "script_input.h"
#include "superstar_mods.h"
#include "universe_mods.h"
#include "../modmaker/svrfmt/bytes.h"
#include "util.h"

namespace {

constexpr uint32_t kSlots = 432, kSlotSize = 2116, kMaxPeople = 6;
constexpr uint32_t kLive = 0x82E3DE00, kArena = 64;
constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260, kOwnId = 32, kName = 34;
constexpr uint32_t kNoPick = 51200;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
using svrfmt::Rd32, svrfmt::Wr32;
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

struct TestMatch {
  bool set = false;
  std::vector<std::string> people;  // names or ids
  std::vector<int> attires;          // attires=<n>,... (1-4) per person, in person order (default 1)
  int arena = -1;
  bool all_cpu = false;  // cpu=all: every person a CPU (the picks too)
  int min_people = 0;    // min=N: only a match whose picks are N or more (min=2: a story's match, not its walk)
  bool exact = false;
  std::vector<int> ports;  // ports=0,-1: per person, the player's controller (0-3) playing him, or -1 the CPU    // exact=1: the people listed only - a later slot the route's presses picked is emptied
};

const TestMatch& Config() {
  static const TestMatch config = [] {
    TestMatch m;
    const char* v = std::getenv("SVR2011_TEST_MATCH");
    if (!v || !*v) return m;
    m.set = true;
    // "key=value" pairs; a value runs to the next " key=" (names have spaces)
    std::string s = v;
    size_t at = 0;
    while (at < s.size()) {
      const size_t eq = s.find('=', at);
      if (eq == std::string::npos) break;
      std::string key = s.substr(at, eq - at);
      while (!key.empty() && key.front() == ' ') key.erase(0, 1);
      size_t end = s.size();
      for (size_t next = s.find('=', eq + 1); next != std::string::npos; next = s.find('=', next + 1)) {
        const size_t space = s.rfind(' ', next);
        if (space != std::string::npos && space > eq) {
          end = space;
          break;
        }
      }
      const std::string value = s.substr(eq + 1, end - eq - 1);
      if (key == "people") {
        std::stringstream list(value);
        std::string name;
        while (std::getline(list, name, ',') && m.people.size() < kMaxPeople) {
          while (!name.empty() && name.front() == ' ') name.erase(0, 1);
          while (!name.empty() && name.back() == ' ') name.pop_back();
          m.people.push_back(name);  // (an empty one keeps that person)
        }
      } else if (key == "attires") {
        std::stringstream list(value);
        std::string n;
        while (std::getline(list, n, ',')) m.attires.push_back(std::clamp(std::atoi(n.c_str()), 1, 4));
      } else if (key == "arena") {
        m.arena = std::atoi(value.c_str());
      } else if (key == "cpu") {
        m.all_cpu = value == "all";
      } else if (key == "ports") {
        std::istringstream list(value);
        std::string n;
        while (std::getline(list, n, ',')) m.ports.push_back(std::clamp(std::atoi(n.c_str()), -1, 3));
      } else if (key == "exact") {
        m.exact = value == "1";
      } else if (key == "min") {
        m.min_people = std::atoi(value.c_str());
      } else {
        REXLOG_WARN("test match: unknown setting '{}'", key);
      }
      at = end;
    }
    return m;
  }();
  return config;
}

const uint8_t* Record(const uint8_t* base, uint32_t id) {
  if (id >= 1000) return nullptr;
  const uint32_t index = svr2011::RosterIndex(base, id);
  if (index >= 1000) return nullptr;
  const uint8_t* rec = base + svr2011::RosterEntry(kRecords, kRecordSize, index);
  return Rd16(rec + kOwnId) == id ? rec : nullptr;
}

using svr2011::Upper;

// A name or id -> the superstar id (0: none).
uint32_t FindSuperstar(const uint8_t* base, const std::string& want) {
  if (!want.empty() && std::isdigit(static_cast<unsigned char>(want[0]))) {
    const uint32_t id = uint32_t(std::atoi(want.c_str()));
    return Record(base, id) ? id : 0;
  }
  // (a selectable record first: the disc keeps hidden ones with a superstar mod's name - KHALI, 166)
  constexpr uint32_t kSelectable = 221;
  const std::string w = Upper(want);
  uint32_t hidden = 0;
  for (uint32_t id = 1; id < 1000; ++id)
    if (const uint8_t* rec = Record(base, id); rec && Upper(reinterpret_cast<const char*>(rec + kName)) == w) {
      if (rec[kSelectable] == 1) return id;
      if (!hidden) hidden = id;
    }
  return hidden;
}

// Not the title screen's demo match: only a match chosen in the menus (the
// last entry chosen is not the title screen's, group 0).
bool Applies() {
  if (!Config().set) return false;
  uint32_t group = 0, row = 0;
  return svr2011::ScriptMenuSelects(&group, &row) > 0 && group != 0;
}

}  // namespace

// The match's arena: the match screen's choice (its object, *0x82EDEB48:
// +244, read by sub_8247B6C8(screen); sub_82773F00 copies it to the live
// match record's +64 as the match loads). It is set as soon as the screen
// exists - as if picked in SELECT ARENA - so everything the game works out
// from it (a cage match's cage, ...) fits. Done from the live record's
// getter (sub_825740C8), which runs all the time. (Putting it straight into
// the live record left parts of the load with the arena they had read
// first: no cage, and a freeze on a phone.)
namespace {
constexpr uint32_t kMatchScreen = 0x82EDEB48, kScreenArena = 244;

void SetScreenArena(uint8_t* base, uint32_t screen) {
  // (a slot that plays as another arena - arena_mods.h ArenaNumberFor - as the match would; with
  // SVR2011_TEST_ARENA_AS_TILE=1 the tile's own number, as SELECT ARENA leaves it - the game's own
  // path then makes it the arena it plays as)
  static const bool as_tile = std::getenv("SVR2011_TEST_ARENA_AS_TILE") != nullptr;
  const int arena = as_tile ? Config().arena : svr2011::ArenaNumberFor(Config().arena);
  if (arena < 0 || !screen || !Applies() || Rd32(base + screen + kScreenArena) == uint32_t(arena)) return;
  // (as the tile: once made the arena it plays as, left so - as after a real pick)
  if (as_tile && Rd32(base + screen + kScreenArena) == uint32_t(svr2011::ArenaNumberFor(arena))) return;
  static uint32_t logged = 0;
  if (logged++ < 2)
    REXLOG_INFO("test match: arena {} (the match screen had {})", arena, Rd32(base + screen + kScreenArena));
  Wr32(base + screen + kScreenArena, uint32_t(arena));
}
}  // namespace

REX_EXTERN(__imp__sub_825740C8);
REX_HOOK_RAW(sub_825740C8) {
  __imp__sub_825740C8(ctx, base);
  if (Config().arena >= 0) {
    const uint32_t r3 = ctx.r3.u32;
    SetScreenArena(base, Rd32(base + kMatchScreen));
    ctx.r3.u64 = r3;
  }
}

REX_EXTERN(__imp__sub_8247B6C8);
REX_HOOK_RAW(sub_8247B6C8) {
  svr2011::ScreenArenaRead(base, ctx.r3.u32);
  SetScreenArena(base, ctx.r3.u32);
  __imp__sub_8247B6C8(ctx, base);
}

REX_EXTERN(__imp__sub_828BC5E8);
REX_HOOK_RAW(sub_828BC5E8) {
  if (ctx.r3.u32) svr2011::RtwmStreakPeople(base, ctx.r3.u32);  // (the Streak story played as a mod)
  svr2011::SuperstarModHandovers(base);  // (a slot's old mod's saved profile written back since: this mod's)
  const TestMatch& m = Config();
  int picked_count = 0;
  if (ctx.r3.u32)
    for (uint32_t i = 0; i < kMaxPeople; ++i) picked_count += Rd32(base + ctx.r3.u32 + kSlots + i * kSlotSize + 8) != kNoPick;
  if (Applies() && ctx.r3.u32 && picked_count >= m.min_people) {
    const uint32_t match = ctx.r3.u32;
    std::string done;
    for (uint32_t i = 0; i < m.people.size(); ++i) {
      // (CAW<N>: the Created Superstar in save slot N, character id N - not
      // in the roster's record table)
      const std::string& who = m.people[i];
      if (who.empty()) continue;
      const bool caw = who.size() > 3 && Upper(who.substr(0, 3)) == "CAW" && std::isdigit(uint8_t(who[3]));
      const uint32_t caw_id = caw ? uint32_t(std::atoi(who.c_str() + 3)) : 0;
      if (caw && caw_id >= 50) {
        REXLOG_WARN("test match: no Created Superstar slot {}", caw_id);
        continue;
      }
      const uint32_t id = caw ? caw_id : FindSuperstar(base, who);
      if (!id && !caw) {
        REXLOG_WARN("test match: no superstar '{}'", m.people[i]);
        continue;
      }
      uint8_t* slot = base + match + kSlots + i * kSlotSize;
      const bool picked = Rd32(slot + 8) != kNoPick;
      // (attire: the first, as the select screen gives, unless attires= names another)
      const int attire = i < m.attires.size() ? m.attires[i] : 1;
      Wr32(slot + 8, id * 100 + uint32_t(attire - 1) * 10 + 2);
      Wr16(slot + 54, id);
      // (the slot's title - +37, the one the ring announcer names - is the new
      // person's, from the save's title holders as sub_825EA208 asks; the
      // menu's pick's otherwise: a non-champion announced as "the WWE
      // Champion")
      auto held = [&](uint32_t kind) {
        PPCContext c = ctx;
        c.r3.u64 = id, c.r4.u64 = kind;
        sub_828BDEF8(c, base);
        return c.r3.u32 & 0xFF;
      };
      uint32_t title = held(1);
      if (title == 255) title = held(2);
      slot[37] = uint8_t(title);
      if (title == 255) slot[39] = 255, slot[38] &= uint8_t(~1);
      if (!picked) {
        slot[5] = uint8_t(i);  // (a team of its own)
        slot[4] = 0;           // (kind: a wrestler)
        slot[-8] = 1;          // (controller: the CPU)
      }
      // (test aid SVR2011_TEST_TEAM_ENTRANCE, test games only: a member of that team is in it as the select screen's
      // team pick makes him - the slot's +40, the match slot record's +48 (its +16 key is this +8), -1 none - so the
      // match plays the team's entrance; picked one by one, each walked out with his own)
      if (const int team = svr2011::TestTeamOf(base, id); team >= 0) {
        slot[40] = uint8_t(team);
        // (and the game block's +9968, the slot of the team whose entrance and theme play together - -1 none; read
        // by sub_82322DA0 before sub_823373A8 asks the team record: the team's first member's slot)
        uint8_t& lead = base[0x82E3DE00u + 9968];
        const int was = int8_t(lead);
        if (was < 0 || was >= int(kMaxPeople)) lead = uint8_t(i);
        REXLOG_INFO("test match: person {} (superstar {}) as team {} (team slot {} -> {})", i, id, team, was, int8_t(lead));
        // (and the match screen's copy of that team's record - sub_828B9520: *(0x82EDEB48) + 0xFA6C + team * 270, the
        // save's layout: +20 tag / +26 trio entrance, read by the entrance getter sub_82756538 - which the select
        // screen fills when a team is picked: a test match's was empty, and the team's members asked no entrance)
        if (const uint32_t ms = Rd32(base + 0x82EDEB48), rec = svr2011::TestTeamRecord(base); ms && rec) {
          uint8_t* t = base + ms + 0xFA6C + uint32_t(team) * 270;
          std::memcpy(t, base + rec, 270);
          REXLOG_INFO("test match: match team {} copy: count {} members {} {} entrances {} / {} name '{}'", team,
                      Rd16(t), Rd16(t + 2), Rd16(t + 4), Rd16(t + 20), Rd16(t + 26),
                      std::string(reinterpret_cast<const char*>(t + 0x50), strnlen(reinterpret_cast<const char*>(t + 0x50), 32)));
        }
      }
      // (a CPU: slot -8 1 and slot -4's low bits 6 - the character's +1152, the pad that plays him; the select
      // screen's CPU picks have 6. With -8 alone person 0 kept pad 0: an idle player in "CPU vs CPU" tests)
      const auto cpu = [&] { slot[-8] = 1, slot[-4] = uint8_t((slot[-4] & ~7) | 6); };
      if (m.all_cpu) cpu();  // (cpu=all: the picks too)
      if (i < m.ports.size()) {  // (ports=: the controller - the CPU, else slot -4's low bits the player)
        if (m.ports[i] < 0) cpu();
        else slot[-8] = 0, slot[-4] = uint8_t((slot[-4] & ~7) | m.ports[i]);
      }
      // Online (a LAN test, tools/dev/svrtest_lan.ps1): a player's slot carries the lobby's whole person record
      // (+56 its flag, the record at +92 - sub_828B90F8) and two 112-byte entries (+2040 / +2044 their flags -
      // sub_828B91A0), which the commit's people builder copies instead of building the superstar at +8. Off:
      // the superstar asked for is built from the game's data, alike on both games.
      if (svr2011::P2POnlineMode()) Wr32(slot + 56, 0), Wr32(slot + 2040, 0), Wr32(slot + 2044, 0);
      done += (done.empty() ? "" : ", ") +
              (caw ? "CAW " + std::to_string(id) : std::string(reinterpret_cast<const char*>(Record(base, id) + kName)));
    }
    if (m.exact)  // (match replays: a phantom third wrestler made two runs of one setup differ)
      for (uint32_t i = uint32_t(m.people.size()); i < 6; ++i)
        if (uint8_t* slot = base + match + kSlots + i * kSlotSize; Rd32(slot + 8) != kNoPick) {
          REXLOG_INFO("test match: slot {} emptied (exact=1)", i);
          Wr32(slot + 8, kNoPick);
        }
    REXLOG_INFO("test match: people {}; arena {}", done.empty() ? "as picked" : done,
                Rd32(base + kLive + kArena));
    // (a Universe card's people as the match's again: its select screen re-picks every slot - big roster's card test)
    svr2011::BigRosterTestCard(base);
    // Test aid: SVR2011_TEST_DAMAGE=<0-255> - the six DAMAGE BALANCING sliders (game block +10394..+10399, damage x
    // (slider x 0.01 + 0.5); 255 about 3x) for this match: pins come in minutes, not at a match's end (pin tests).
    if (const char* v = std::getenv("SVR2011_TEST_DAMAGE")) {
      const int d = std::clamp(std::atoi(v), 0, 255);
      for (uint32_t i = 0; i < 6; ++i) base[0x82E3DE00u + 10394 + i] = uint8_t(d);
      REXLOG_INFO("test match: damage sliders {} (SVR2011_TEST_DAMAGE)", d);
    }
    // Test aid: SVR2011_TEST_MOMENTUM=<0-255> - the two momentum sliders (+10406 / +10407): finishers come in
    // seconds, so a CPU plays its own finisher (finisher tests without scripted input)
    if (const char* v = std::getenv("SVR2011_TEST_MOMENTUM")) {
      const int m = std::clamp(std::atoi(v), 0, 255);
      base[0x82E3DE00u + 10406] = base[0x82E3DE00u + 10407] = uint8_t(m);
      REXLOG_INFO("test match: momentum sliders {} (SVR2011_TEST_MOMENTUM)", m);
    }
  }
  // (problem reports: the picks - who and which attire; a Created Superstar is its save slot, ids below 50)
  if (const uint32_t match = ctx.r3.u32) {
    std::string picks;
    for (uint32_t i = 0; i < kMaxPeople; ++i) {
      const uint32_t v = Rd32(base + match + kSlots + i * kSlotSize + 8);
      if (v == kNoPick || v / 100 >= 512) continue;
      picks += fmt::format("{}person {} {} {} attire {}", picks.empty() ? "" : ", ", i, v / 100 < 50 ? "CAW slot" : "id",
                           v / 100, v % 100 / 10 + 1);
    }
    static std::string last;
    if (!picks.empty() && picks != last) REXLOG_INFO("[svr2011] match picks: {}", picks);
    last = picks;
    // (the loading picture: a superstar mod's in the match - person 0's, else the next person's in slot order)
    std::string load;
    for (uint32_t i = 0; i < kMaxPeople && load.empty(); ++i) {
      const uint32_t v = Rd32(base + match + kSlots + i * kSlotSize + 8);
      if (v != kNoPick && v / 100 < 512) load = svr2011::SuperstarModLoading(v / 100);
    }
    svr2011::SetSuperstarLoading(load);
  }
  __imp__sub_828BC5E8(ctx, base);
  svr2011::FastStartPeopleCommitted(base);  // (back from Road to WrestleMania: on to the main menu)
}
