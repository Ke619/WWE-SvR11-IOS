// WWE SmackDown vs. Raw 2011 - Slobber Knocker: player 1 against an endless
// line of opponents, one at a time, until pinned (HANDICAP -> SLOBBER
// KNOCKER, match_types.cpp: the gauntlet rule 0x52, player 1 and the first
// opponent picked, the 2 waiting opponents random).
//
// The gauntlet (research: docs/MATCH_TYPES_RESEARCH.md):
// - characters (0x82E3CC50, u32[9]: people 0-5, then the referee and the
//   commentators): +446 state (0 in the match, 1 waiting, 4 beaten / out),
//   +447 how it lost (0 still in), +448 who beat it, +1800 team, +2624 role
//   (3 waiting entrant), +80 its superstar (sub_828B7028 -> id);
// - the falls / match-end judge sub_82245618: for a character that lost
//   (+447) it brings in a waiting one of its team (+446 = 0) - the match ends
//   only when that team has none left.
// Here a beaten opponent's slot is loaded with a new random superstar who
// then waits, so the opponents' team always has someone to come in; the
// match ends when player 1 loses. The number beaten is shown over the match.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "match_hud.h"
#include "match_types.h"
#include "../modmaker/svrfmt/bytes.h"
#include "util.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kGauntlet = 0x52;
constexpr uint32_t kState = 446, kLost = 447, kBeatenBy = 448, kTeam = 1800, kRole = 2624;
constexpr uint32_t kOpponents = 1;  // (their team; player 1's is 0)
constexpr uint32_t kBusy = 476;  // (0 once a beaten character has left - the Royal Rumble's test before it unloads one)

using svrfmt::Rd32, svrfmt::Wr32;
uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

using svr2011::NowMs;

std::atomic<int> g_beaten{0};    // opponents beaten this match
std::atomic<int64_t> g_seen{0};  // ms: the match's last update (the count is shown)
std::set<uint32_t> g_counted;    // characters counted (their fall)
uint32_t g_frames = 0;

bool Running(uint8_t* base) { return svr2011::SlobberKnockerMatch() && base[kLive] == kGauntlet; }

std::string Characters(uint8_t* base) {
  std::string all;
  const uint32_t infos = Rd32(base + 0x82E3DDFC);
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4)) {
      const uint32_t person = Rd32(base + c + 2660);
      const uint32_t info = infos && person < 33 ? infos + 128 + person * 52 : 0;
      all += fmt::format(" {}:{:08X} state {} lost {} by {} team {} role {} area {} +476 {:08X} +2348 {} person {} entry {}",
                         i, c, base[c + kState], base[c + kLost], base[c + kBeatenBy], Rd32(base + c + kTeam),
                         Rd32(base + c + kRole), base[c + 444], Rd32(base + c + kBusy), Rd32(base + c + 2348), person,
                         info ? int16_t(Rd16(base + info + 40)) : -1);
    }
  return all;
}

// Falls of player 1's opponents, counted once each.
void CountFalls(uint8_t* base) {
  const uint32_t player = Rd32(base + kChars);
  if (!player) return;
  const uint32_t team = Rd32(base + player + kTeam);
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || !base[c + kLost] || Rd32(base + c + kTeam) == team || g_counted.count(c)) continue;
    g_counted.insert(c);
    ++g_beaten;
    REXLOG_INFO("slobber knocker: opponent {} beaten - {} so far", i, g_beaten.load());
  }
}

bool WaiterReady(uint8_t* base, uint32_t team) {
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (c && base[c + kState] == 1 && !base[c + kLost] && Rd32(base + c + kTeam) == team &&
        Rd32(base + c + kRole) == 3)
      return true;
  }
  return false;
}

// -- The next opponent ---------------------------------------------------
//
// A beaten opponent, once gone (state 4, the judge done with it - lost back
// to 0 - and +476 0: it has left), waits again for its turn like a gauntlet
// entrant: state 1, role 3, not walked in yet (+2348 0), an entry time (info
// +40, s16: the gauntlet's entry task sub_82328AA0 walks a waiting entrant
// to the ring once the match time has passed it; 0 never). So the opponents'
// team always has someone to come in, and the line goes round the 4
// opponents (the one picked and 3 random).
// Not done: a new superstar each time. Reloading a beaten opponent's slot
// mid-match (the Royal Rumble's unload sub_8217CCD8 + the run-in task's
// load: sub_822F5AD8, sub_8217CB48 / CB68, sub_8224AB90) works most times but
// the characters' job (thread 22, sub_82252610 -> sub_82258CC0, a component
// of the character) can still hold the old character - a crash; the game's
// swap task (sub_825BDF60) is for empty person slots only (the draw crashed).
// A safe point to free a character would be needed (the Royal Rumble's
// eliminated ones: sub_8223FD78).
constexpr uint32_t kOutFrames = 60;  // (gone 1 s before it waits again)

uint32_t g_out_frames[6] = {};

uint32_t Call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3) {
  const auto saved = ctx;
  ctx.r3.u64 = r3;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

void RecycleStep(PPCContext& ctx, uint8_t* base) {
  const uint32_t player = Rd32(base + kChars);
  if (!player || Rd32(base + kLive + 288)) return;  // (+288: the match is decided)
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    const bool out = c && base[c + kState] == 4 && !base[c + kLost] && !Rd32(base + c + kBusy) &&
                     Rd32(base + c + kTeam) != Rd32(base + player + kTeam);
    g_out_frames[i] = out ? g_out_frames[i] + 1 : 0;
    if (!out || g_out_frames[i] < kOutFrames) continue;
    g_out_frames[i] = 0;
    base[c + kState] = 1;
    Wr32(base + c + kRole, 3);
    Wr32(base + c + 2348, 0);
    if (const uint32_t info = Call(ctx, base, sub_82573EC8, Call(ctx, base, sub_8257C178, Rd32(base + c + 2660))))
      base[info + 40] = 0, base[info + 41] = 1;
    g_counted.erase(c);
    REXLOG_INFO("slobber knocker: opponent {} waits to come in again", i);
  }
}

// The count, over the match (match_hud.h: only while the match runs).
class Count final : public rex::ui::ImGuiDialog {
 public:
  explicit Count(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (NowMs() - g_seen.load() > 500 || !svr2011::MatchHudVisible()) return;
    svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, "SLOBBER KNOCKER",
                          "BEATEN: " + std::to_string(g_beaten.load()));
  }
};

}  // namespace

namespace svr2011 {

void InstallSlobberKnockerOverlay(rex::ui::ImGuiDrawer* drawer) { new Count(drawer); }

void SlobberKnockerStart() {
  g_beaten = 0;
  g_counted.clear();
  g_frames = 0;
  for (uint32_t& f : g_out_frames) f = 0;
}

void SlobberKnockerUpdate(PPCContext& ctx, uint8_t* base) {
  ++g_frames;
  if (Rd32(base + kChars) && int32_t(Rd32(base + 0x82E3CD0C)) > 0) g_seen = NowMs();  // (a match running)
  // Test aids (with the count shown: g_frames counts from the select screen):
  // SVR2011_TEST_SK_LOG=1 - the characters once a second;
  // SVR2011_TEST_SK_BEAT=<s> - every <s> seconds the opponent in the match
  // loses (as if pinned by player 1).
  static const bool log = std::getenv("SVR2011_TEST_SK_LOG") != nullptr;
  static const int beat = [] { const char* v = std::getenv("SVR2011_TEST_SK_BEAT"); return v ? std::atoi(v) : 0; }();
  if (log && g_frames % 60 == 0) REXLOG_INFO("slobber knocker: characters{}", Characters(base));
  // SVR2011_TEST_SK_LOSE=<s> - after <s> seconds player 1 loses.
  static const int lose = [] { const char* v = std::getenv("SVR2011_TEST_SK_LOSE"); return v ? std::atoi(v) : 0; }();
  if (lose > 0 && g_frames == uint32_t(lose * 60))
    if (const uint32_t player = Rd32(base + kChars)) {
      base[player + kLost] = 1;
      REXLOG_INFO("slobber knocker: test - player 1 loses");
    }
  if (beat > 0 && g_frames % uint32_t(beat * 60) == 0) {
    const uint32_t player = Rd32(base + kChars);
    for (uint32_t i = 1; player && i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      if (!c || base[c + kState] != 0 || base[c + kLost] || base[c + 444] != 0 ||
          Rd32(base + c + kTeam) == Rd32(base + player + kTeam))
        continue;
      base[c + kLost] = 1;
      base[c + kBeatenBy] = 0;
      REXLOG_INFO("slobber knocker: test - opponent {} loses", i);
      break;
    }
  }
}

}  // namespace svr2011

// The falls / match-end judge: sub_82245618(...) -> 1 when the match is over.
REX_EXTERN(__imp__sub_82245618);
REX_HOOK_RAW(sub_82245618) {
  svr2011::ThreeStagesBeforeJudge(ctx, base);  // (three_stages.cpp: a fall that doesn't end the match)
  svr2011::LumberjackBeforeJudge(base);   // (match_types.cpp)
  svr2011::GauntletEliminatorBeforeJudge(ctx, base);  // (gauntlet_eliminator.cpp: a fall that eliminates)
  // Test aid: SVR2011_TEST_FALL_LOG=1 - every match: the characters each time
  // one's state / lost byte changes, and the judge's result.
  static const bool fall_log = std::getenv("SVR2011_TEST_FALL_LOG") != nullptr;
  if (fall_log) {
    static std::string last;
    std::string now;
    for (uint32_t i = 0; i < 6; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4)) now += fmt::format("{}{}", base[c + kState], base[c + kLost]);
    if (now != last) {
      last = now;
      std::string opt;
      for (uint32_t i = 0; i < 64; ++i) opt += fmt::format("{:02X}", base[kLive + i]);
      REXLOG_INFO("falls: rule {:02X} live+288 {} live {};{}", base[kLive], Rd32(base + kLive + 288), opt,
                  Characters(base));
    }
  }
  const bool running = Running(base);
  if (running) CountFalls(base);
  if (running) RecycleStep(ctx, base);
  // An opponent lost with no one waiting to come in (the beaten ones not gone
  // yet): one of those waits now - else the gauntlet would end.
  if (running) {
    const uint32_t player = Rd32(base + kChars);
    for (uint32_t i = 1; player && i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      if (!c || !base[c + kLost] || Rd32(base + c + kTeam) == Rd32(base + player + kTeam) ||
          WaiterReady(base, Rd32(base + c + kTeam)))
        continue;
      for (uint32_t k = 1; k < 6; ++k) {
        const uint32_t w = Rd32(base + kChars + k * 4);
        if (!w || w == c || base[w + kState] != 4 || base[w + kLost] || Rd32(base + w + kTeam) != Rd32(base + c + kTeam))
          continue;
        base[w + kState] = 1;
        Wr32(base + w + kRole, 3);
        Wr32(base + w + 2348, 0);
        if (const uint32_t info = Call(ctx, base, sub_82573EC8, Call(ctx, base, sub_8257C178, Rd32(base + w + 2660))))
          base[info + 40] = 0, base[info + 41] = 1;
        g_counted.erase(w);
        REXLOG_INFO("slobber knocker: opponent {} waits to come in again (early)", k);
        break;
      }
      break;
    }
  }
  __imp__sub_82245618(ctx, base);
  if (fall_log && ctx.r3.u32 == 1) {
    static uint32_t ended = 0;
    if (Rd32(base + kLive + 288) != ended)
      REXLOG_INFO("falls: the judge ends the match (live+288 {}, winner {}, loser {})", Rd32(base + kLive + 288),
                  base[kLive + 9968], base[kLive + 9969]);
    ended = Rd32(base + kLive + 288);
  }
  if (running && ctx.r3.u32 == 1)
    REXLOG_INFO("slobber knocker: the match is over - {} beaten;{}", g_beaten.load(), Characters(base));
}

// sub_828061E8(pair): on a worker thread, a pair of characters (+0, +20)
// gets two values from each one's +2572 (+656, +660) - or, when the game
// says so, both are zeroed and nothing read. In an ELIMINATION match
// (fatal-4-way, Fold) the pair's +20 was NULL once one had been eliminated:
// a read of guest 0xA0C over and over, then a hang. A pair missing a
// character takes the zeroing path.
// Called from the update below (sub_8280CFD0) after its own pick of the people the CPU looks at - +8, +12 and +20,
// each read through right after (their +444, +446 ...). In a ROYAL RUMBLE the pick could find nobody once the
// opponent was thrown out (all three NULL: an access violation at the +444 read - 2 of 3 heavy 15-man rumbles on
// PC, 3 Nov, main too): the empty ones become the character himself and stay so - more of the CPU's code reads +20
// after the update in the same frame (sub_82811D58 -> sub_827F6C20, a read of guest 0x538 when it was put back to
// NULL) - until the next update's guard below finds someone in the ring for +20.
namespace {
thread_local uint32_t t_update_pair = 0;  // (inside sub_8280CFD0 for this pair)
thread_local bool t_stand_in = false;    // (+8 / +12 / +20 given the character this update: logged)
constexpr uint32_t kLooksAt[] = {8, 12, 20};
}  // namespace
REX_EXTERN(__imp__sub_828061E8);
REX_HOOK_RAW(sub_828061E8) {
  const uint32_t pair = ctx.r3.u32;
  // Test aid SVR2011_TEST_PAIR_NULL=<s>: once, that many seconds into a match, the update's pick comes out empty
  // (+8, +12, +20 NULL) as in the rumble - with SVR2011_TEST_PAIR_OLD=1 the old way, which crashes
  static const int null_at = [] { const char* v = std::getenv("SVR2011_TEST_PAIR_NULL"); return v ? std::atoi(v) : -1; }();
  if (static bool done = false; null_at >= 0 && !done && pair && pair == t_update_pair && Rd32(base + pair + 20) &&
                                int32_t(Rd32(base + 0x82E3CD0C)) > null_at * 60) {
    done = true;
    for (const uint32_t off : kLooksAt) Wr32(base + pair + off, 0);
    REXLOG_INFO("match types: test - the update's pick of {:08X} emptied", Rd32(base + pair));
  }
  if (pair && (!Rd32(base + pair) || !Rd32(base + pair + 20))) {
    Wr32(base + pair + 656, 0);
    Wr32(base + pair + 660, 0);
    static int count = 0;
    if (++count <= 10 || count % 1000 == 0)
      REXLOG_INFO("match types: a character pair missing one ({:08X} / {:08X}) - skipped ({} times)",
                  Rd32(base + pair), Rd32(base + pair + 20), count);
    if (const uint32_t self = Rd32(base + pair); self && pair == t_update_pair && !std::getenv("SVR2011_TEST_PAIR_OLD"))
      for (int i = 0; i < 3; ++i)
        if (!Rd32(base + pair + kLooksAt[i])) Wr32(base + pair + kLooksAt[i], self), t_stand_in = true;
    return;
  }
  __imp__sub_828061E8(ctx, base);
}

// The same object's update, sub_8280CFD0(pair): +0 a character, +20 the one
// it's set on (a CPU's opponent). With +20 NULL (an elimination left it so;
// fatal-4-way ELIMINATION) it reads past NULL further on (guest 0x1BC, the
// character's +444, PC repro): +20 becomes another character still in the
// match (state 0, another team) before it runs; with none, it's skipped.
namespace {
std::mutex g_targets_mutex;
std::map<uint32_t, uint32_t> g_targets;  // character -> its AI pair's +20, last update
}  // namespace
REX_EXTERN(__imp__sub_8280CFD0);
REX_HOOK_RAW(sub_8280CFD0) {
  const uint32_t pair = ctx.r3.u32;
  // (not at a match's start - there +20 is empty until the game sets it - and
  // only for a competitor: not the referee or the commentators, teams 30+)
  const uint32_t self = pair ? Rd32(base + pair) : 0;
  // (GAUNTLET ELIMINATOR: one alone in the ring while the next entrant walks
  // down - the game's own pick of an opponent found none and read past NULL
  // in the same place: until someone is at ringside, the update is skipped)
  const auto at_ringside = [&](uint32_t c) {
    uint32_t x, z;
    std::memcpy(&x, base + c + 288, 4), std::memcpy(&z, base + c + 296, 4);
    x = __builtin_bswap32(x), z = __builtin_bswap32(z);
    float fx, fz;
    std::memcpy(&fx, &x, 4), std::memcpy(&fz, &z, 4);
    return std::fabs(fx) < 58.0f && std::fabs(fz) < 58.0f;  // (the ramp starts at about z 60)
  };
  // Test aid SVR2011_TEST_NO_OTHER=<from s>,<to s>: in that part of a match nobody passes valid_other below (a rumble
  // where nobody else is in the ring and up)
  static const std::pair<int, int> no_other = [] {
    int a = -1, b = -1;
    if (const char* v = std::getenv("SVR2011_TEST_NO_OTHER")) std::sscanf(v, "%d,%d", &a, &b);
    return std::make_pair(a, b);
  }();
  const int32_t now_frame = int32_t(Rd32(base + 0x82E3CD0C));
  const auto valid_other = [&] {
    if (no_other.first >= 0 && now_frame >= no_other.first * 60 && now_frame < no_other.second * 60) return 0u;
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      if (c && c != self && base[c + kState] == 0 && !base[c + kLost] && Rd32(base + c + kTeam) < 30 &&
          Rd32(base + c + kTeam) != Rd32(base + self + kTeam) && at_ringside(c))
        return c;
    }
    return 0u;
  };
  if (self && svr2011::GauntletEliminatorMatch() && Rd32(base + self + kTeam) < 30 && !valid_other()) {
    static int alone = 0;
    if (++alone <= 10 || alone % 1000 == 0)
      REXLOG_INFO("match types: a character with no opponent in the ring ({:08X}) - skipped ({} times)", self, alone);
    return;
  }
  // (B13, Android: a Road to WrestleMania gauntlet froze 1-2 moves after the
  // next entrant came in - reads of guest 0x1BC over and over, as here; the
  // story match's clock may start again with each entrant, and the guard only
  // ran after match frame 300. Now also a pair that had an opponent earlier
  // in this match - the start, where the game fills +20 itself, stays as it
  // was. SVR2011_TEST_PAIR_OLD=1 the old way.)
  static const bool old_way = std::getenv("SVR2011_TEST_PAIR_OLD") != nullptr;
  bool had = false;
  {
    static std::mutex m;
    static std::set<uint32_t> pairs;  // (pairs that have had an opponent this match)
    static uint32_t first = 0;        // (person 0: a new one, a new match)
    static uint32_t placed = 0;       // (a match placed since: a new match too - person 0 is often made at the same
                                      // place: a second backstage brawl kept the quit one's pairs, the guard found
                                      // nobody "at ringside" there and skipped the AI's set-up, which then read a
                                      // null target: crash, 21 Oct report. Not the clock: a gauntlet's restarts.)
    std::lock_guard lock(m);
    const uint32_t now_placed = svr2011::MatchPlacements();
    if (const uint32_t c = Rd32(base + kChars); c != first || now_placed != placed) first = c, placed = now_placed, pairs.clear();
    if (pair && Rd32(base + pair + 20)) pairs.insert(pair);
    had = pair && pairs.count(pair) && !old_way;
  }
  // (+20 the character himself: the stand-in above - someone else as soon as there's one)
  if (const uint32_t on = Rd32(base + pair + 20); self && (!on || on == self) && Rd32(base + self + kTeam) < 30 &&
      (had || int32_t(Rd32(base + 0x82E3CD0C)) > 300)) {
    uint32_t other = valid_other();
    // (nobody in the ring and up - a ROYAL RUMBLE: skipping every update left a CPU standing for the last 12 minutes,
    // 3 Nov: the nearest one in the match (state 0: not waiting to come in, 1, nor thrown out, 4) on another team,
    // wherever he is - on the floor, on the ramp; nobody at all - the
    // character himself, the update runs and the game's own pick inside it may find someone. SVR2011_TEST_IDLE_OLD=1:
    // skipped as before)
    static const bool idle_old = std::getenv("SVR2011_TEST_IDLE_OLD") != nullptr;
    const char* how = "now";
    if (!other && !idle_old) {
      const auto RdF = [](const uint8_t* p) {
        uint32_t u;
        std::memcpy(&u, p, 4);
        u = __builtin_bswap32(u);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
      };
      float best = 1e30f;
      for (uint32_t i = 0; i < 6; ++i) {
        const uint32_t c = Rd32(base + kChars + i * 4);
        if (!c || c == self || base[c + kState] != 0 || base[c + kLost] || Rd32(base + c + kTeam) >= 30 ||
            Rd32(base + c + kTeam) == Rd32(base + self + kTeam))
          continue;
        const float dx = RdF(base + c + 288) - RdF(base + self + 288), dz = RdF(base + c + 296) - RdF(base + self + 296);
        if (dx * dx + dz * dz < best) best = dx * dx + dz * dz, other = c, how = "the nearest";
      }
      if (!other) other = self, how = "himself";
    }
    static int count = 0;
    if (++count <= 10 || count % 1000 == 0)
      REXLOG_INFO("match types: a character's opponent was gone ({:08X}) - {} ({} times)", self,
                  other ? fmt::format("{} {:08X}", how, other) : std::string("skipped"), count);
    if (!other) return;
    Wr32(base + pair + 20, other);
  }
  t_update_pair = pair, t_stand_in = false;
  __imp__sub_8280CFD0(ctx, base);
  t_update_pair = 0;
  if (t_stand_in) {
    static int count = 0;
    if (++count <= 10 || count % 1000 == 0)
      REXLOG_INFO("match types: a CPU's update found nobody to look at ({:08X}) - itself until there's someone ({} times)",
                  self, count);
    t_stand_in = false;
  }
  if (self) {  // (research aid: the stuck-wrestler log's AI target - match_types.h AiTargetOf)
    std::lock_guard lock(g_targets_mutex);
    g_targets[self] = Rd32(base + pair + 20);
  }
}

namespace svr2011 {
uint32_t AiTargetOf(uint32_t character) {
  std::lock_guard lock(g_targets_mutex);
  const auto it = g_targets.find(character);
  return it == g_targets.end() ? 0 : it->second;
}
}  // namespace svr2011
