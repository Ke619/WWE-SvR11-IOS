// WWE SmackDown vs. Raw 2011 - Gauntlet Eliminator: six superstars, each on
// his own; two start, the other four come in one at a time on a timer; a pin
// or a submission eliminates (with the referee: the gauntlet's own falls);
// the last one left wins (HANDICAP -> GAUNTLET ELIMINATOR, match_types.cpp:
// the gauntlet rule 0x52 set up for six, player 1 and the first opponent
// picked, the four entrants random; GAUNTLET ELIMINATOR: EXTREME RULES adds
// weapons lying about and no disqualifications).
//
// The gauntlet (slobber_knocker.cpp has its fields): a waiting entrant
// (state +446 1, role +2624 3) is walked to the ring by the gauntlet's entry
// task once the match time passes its entry time (info +40, s16 seconds; 0
// never); the falls judge sub_82245618 brings in, for a character that lost,
// a waiting one of its team, and ends the match when that team has none
// left - two sides. Here everyone has his own team, so before the judge
// (GauntletEliminatorBeforeJudge, from its hook) a fall that doesn't decide
// the match is the port's: the loser is out (state 4: he leaves, as a beaten
// gauntlet entrant does) and his fall taken back (as Three Stages of Hell's),
// and the judge goes on. The last fall - one left in the ring, nobody
// waiting - is left to the judge: it ends the match.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "match_hud.h"
#include "match_types.h"
#include "util.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C, kGauntlet = 0x52;
constexpr uint32_t kState = 446, kLost = 447, kBeatenBy = 448, kTeam = 1800, kRole = 2624, kPerson = 2660;
constexpr int kFirstEntry = 60, kEntryGap = 60;  // (s: the entrants come in at 1:00, 2:00, 3:00, 4:00)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

using svr2011::NowMs;

std::atomic<int> g_left{0};            // superstars still in it (in the ring or waiting)
std::atomic<int> g_next_entry{-1};     // s of match time: the next entrant's, -1 none
std::atomic<int> g_match_secs{0};
std::atomic<int64_t> g_seen{0};        // ms: the match's last update
bool g_timed = false;                  // the entry times are set (this match)
uint32_t g_last_frames = 0;

bool Running(uint8_t* base) { return svr2011::GauntletEliminatorMatch() && base[kLive] == kGauntlet; }

uint32_t Call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3) {
  const auto saved = ctx;
  ctx.r3.u64 = r3;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// A character's info record (+40: its entry time), 0 if none.
uint32_t InfoOf(PPCContext& ctx, uint8_t* base, uint32_t c) {
  return Call(ctx, base, sub_82573EC8, Call(ctx, base, sub_8257C178, Rd32(base + c + kPerson)));
}

bool Waiting(uint8_t* base, uint32_t c) { return base[c + kState] == 1 && Rd32(base + c + kRole) == 3 && !base[c + kLost]; }
bool InRing(uint8_t* base, uint32_t c) { return base[c + kState] == 0 && Rd32(base + c + kTeam) < 30; }

std::string Characters(PPCContext& ctx, uint8_t* base) {
  std::string all;
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4)) {
      const uint32_t info = InfoOf(ctx, base, c);
      all += fmt::format(" {}:state {} lost {} by {} team {} role {} in {} entry {}", i, base[c + kState], base[c + kLost],
                         base[c + kBeatenBy], Rd32(base + c + kTeam), Rd32(base + c + kRole), Rd32(base + c + 2348),
                         info ? int16_t(Rd16(base + info + 40)) : -1);
    }
  return all;
}

// The waiting entrant with the earliest entry time (or 0).
uint32_t NextWaiter(PPCContext& ctx, uint8_t* base, int* entry) {
  uint32_t best = 0;
  int best_at = 0x7FFF;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || !Waiting(base, c)) continue;
    const uint32_t info = InfoOf(ctx, base, c);
    const int at = info ? int16_t(Rd16(base + info + 40)) : 0x7FFF;
    if (at > 0 && at < best_at) best = c, best_at = at;
  }
  if (entry) *entry = best ? best_at : -1;
  return best;
}

void SetEntry(PPCContext& ctx, uint8_t* base, uint32_t c, int secs) {
  if (const uint32_t info = InfoOf(ctx, base, c)) base[info + 40] = uint8_t(secs >> 8), base[info + 41] = uint8_t(secs);
}

// The count over the match (match_hud.h: only while the match runs).
class Panel final : public rex::ui::ImGuiDialog {
 public:
  explicit Panel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (NowMs() - g_seen.load() > 500 || !svr2011::MatchHudVisible()) return;
    std::string right = std::to_string(g_left.load()) + " LEFT";
    if (const int next = g_next_entry.load(); next > 0) {
      const int in = std::max(0, next - g_match_secs.load());
      right += fmt::format("    NEXT ENTRANT {}:{:02}", in / 60, in % 60);
    }
    svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, "GAUNTLET ELIMINATOR", right);
  }
};

}  // namespace

namespace svr2011 {

void InstallGauntletEliminatorOverlay(rex::ui::ImGuiDrawer* drawer) { new Panel(drawer); }

void GauntletEliminatorStart() {
  g_timed = false;
  g_last_frames = 0;
  g_left = 6;
  g_next_entry = -1;
}

// Each world update while it runs: the entry times once the four wait, the HUD.
void GauntletEliminatorUpdate(PPCContext& ctx, uint8_t* base) {
  if (!Running(base)) return;
  const uint32_t frames = Rd32(base + kMatchFrames);
  if (frames < g_last_frames) g_timed = false;  // (a REMATCH: the clock starts again)
  g_last_frames = frames;
  if (!Rd32(base + kChars) || int32_t(frames) <= 0) return;
  g_seen = NowMs();
  const int secs = int(frames / 60);
  g_match_secs = secs;
  if (!g_timed) {  // (the four entrants: 1:00, 2:00, 3:00, 4:00 - in slot order)
    int k = 0;
    for (uint32_t i = 0; i < 6; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4); c && Waiting(base, c)) SetEntry(ctx, base, c, kFirstEntry + kEntryGap * k++);
    g_timed = k > 0;
    if (g_timed) REXLOG_INFO("gauntlet eliminator: {} entrants wait;{}", k, Characters(ctx, base));
  }
  int left = 0, in_ring = 0;
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4)) {
      if (InRing(base, c) && !base[c + kLost]) ++left, ++in_ring;
      else if (Waiting(base, c)) ++left;
    }
  g_left = left;
  int next = -1;
  const uint32_t waiter = NextWaiter(ctx, base, &next);
  // (one alone in the ring and someone waiting: the next comes in now)
  if (in_ring == 1 && waiter && next > secs + 3 && !Rd32(base + kLive + 288)) {
    SetEntry(ctx, base, waiter, secs + 3);
    next = secs + 3;
    REXLOG_INFO("gauntlet eliminator: one left in the ring - the next entrant comes in now");
  }
  // (an entrant's time has come: in, as the judge brings one in - state 0;
  // the entry task walks him to the ring)
  if (waiter && next <= secs && !Rd32(base + kLive + 288)) {
    SetEntry(ctx, base, waiter, 1);
    base[waiter + kState] = 0;
    REXLOG_INFO("gauntlet eliminator: an entrant comes in at {} s", secs);
    NextWaiter(ctx, base, &next);
  }
  g_next_entry = next;
  // Test aid: SVR2011_TEST_GE_BEAT=<s> - every <s> s one in the ring loses
  // (as if pinned by another in the ring).
  static const int beat = [] { const char* v = std::getenv("SVR2011_TEST_GE_BEAT"); return v ? std::atoi(v) : 0; }();
  if (beat > 0 && secs > 0 && frames % uint32_t(beat * 60) == 0 && !Rd32(base + kLive + 288)) {
    uint32_t loser = ~0u, winner = ~0u;
    for (uint32_t i = 0; i < 6; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4); c && InRing(base, c) && !base[c + kLost] && base[c + 444] == 0) {
        if (loser == ~0u) loser = i;
        else if (winner == ~0u) winner = i;
      }
    if (loser != ~0u && winner != ~0u) {
      const uint32_t c = Rd32(base + kChars + loser * 4);
      base[c + kLost] = 1, base[c + kBeatenBy] = uint8_t(winner);
      REXLOG_INFO("gauntlet eliminator: test - person {} loses to person {}", loser, winner);
    }
  }
  static const bool log = std::getenv("SVR2011_TEST_GE_LOG") != nullptr;  // (test aid: the characters each 5 s)
  if (log && frames % 300 == 0) REXLOG_INFO("gauntlet eliminator: {} s{}", secs, Characters(ctx, base));
}

// Before the falls judge: a fall that doesn't decide the match eliminates.
void GauntletEliminatorBeforeJudge(PPCContext& ctx, uint8_t* base) {
  if (!Running(base) || Rd32(base + kLive + 288)) return;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || !base[c + kLost] || !InRing(base, c)) continue;
    int others = 0, waiting = 0;
    for (uint32_t k = 0; k < 6; ++k)
      if (const uint32_t o = Rd32(base + kChars + k * 4); o && o != c) {
        if (InRing(base, o) && !base[o + kLost]) ++others;
        else if (Waiting(base, o)) ++waiting;
      }
    if (others + waiting < 2) continue;  // (the last fall: the judge's)
    REXLOG_INFO("gauntlet eliminator: person {} eliminated (lost {}, by {}) - {} in the ring, {} waiting", i,
                base[c + kLost], base[c + kBeatenBy], others, waiting);
    base[c + kState] = 4;  // (out: he leaves)
    base[c + kLost] = 0;
  }
}

}  // namespace svr2011
