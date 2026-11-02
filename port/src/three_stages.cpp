// WWE SmackDown vs. Raw 2011 - THREE STAGES OF HELL: the first side to win
// two falls, each fall under another stipulation. In every mode
// (match_types.cpp: EXTREME RULES -> THREE STAGES OF HELL, or its row in
// 6-MAN / HANDICAP), on the mode's normal match: one on one 0x00, tag 0x03,
// triple threat 0x0D, fatal-4-way 0x0E, 6-man tag 0x08, one on two tornado
// 0x0F.
//
// Its submenu has six sets of three stages (ThreeStagesSetup's set):
//   0 NORMAL / FALLS COUNT ANYWHERE / LAST MAN STANDING (below);
//   1 NORMAL / FIRST BLOOD / FINISHERS;
//   2 NORMAL / LAST MAN STANDING / SUBMISSION;
//   3 TABLE / TLC / LADDER - played as the mode's TLC match (0x48-0x4B: the
//     prize hangs, tables and ladders are there from the bell);
//   4 HELL IN A CELL: FIRST BLOOD / LAST MAN STANDING / SUBMISSION - played as
//     the mode's cell match (0x31-0x34: the cell stands from the bell);
//   5 TABLE / LADDER / TABLE - played as the mode's TLC match too (one prize
//     fall, between two table falls).
// Sets 3 and 5 are the switch mod three_stages_tables_ladders.
// What loads at the bell (a cell, a cage, the prize, the table and ladder
// models) can't change mid-match: a set needing one is played in that rule
// from the start and only what wins a fall changes (live option bytes:
// +1 pinfall, +7 give up, +18 table, +19 ladder, +24 TLC, +26 last man
// standing, +40 first blood). FINISHERS is the port's: a pin or submission
// counts only within 10 s (kFinisherWindow) after a finisher or signature -
// the game's move cinematic, when it holds the world (frame_rate.cpp's
// paused round: ThreeStagesCinematic); any other fall is taken back. (No
// "finisher" byte showed in the people's records at those moments; who did
// the move isn't checked.)
//
// One match, not three: the falls / match-end judge (sub_82245618, hooked in
// slobber_knocker.cpp, which calls ThreeStagesBeforeJudge first) sees a fall
// as a character's lost byte (+447) - in tag the legal man pinned; here a
// fall that doesn't win the match is counted for the side that beat him (his
// +448 "beaten by": that person's team, +1800; with two sides, the other
// one) and taken back (lost 0, the judge goes on), and the live rules
// (0x82E3DE00, the 64-byte option record the match runs with) switch to the
// next stage. A side is a team: a wrestler in triple threat / fatal-4-way, a
// team in tag / 6-man / handicap. Health and damage carry over.
//   fall 1: normal (the rule's own);
//   fall 2: FALLS COUNT ANYWHERE (as rule 0x2B: +1 pinfall 2 = anywhere, +17
//           1; no count-out +5, no rope break +3);
//   fall 3: LAST MAN STANDING (as rule 0x3B: +26 1, no pinfall +1, no give
//           up +7, no count-out). In tag and triple threat the engine plays
//           it with any number of people (one down for 10: his side loses;
//           the match ends from the count, not the falls judge). In 6-man tag
//           it never ended (8 minutes: partners and tags break the count), so
//           there fall 3 is NO DISQUALIFICATION (Extreme Rules' option bytes,
//           without +56: no rope breaks, no DQ, no ring-out; pin or give up
//           in the ring).
// The score is shown over the match.
//
// The prize (sets 3 and 5, in the TLC match): a fall for it is the climb
// controller's lost code 6 (sub_8223D7E8, as the climber's grab motion
// plays). One that doesn't win the match, or one in a stage that isn't for
// the prize (a CPU climbs in a table stage too), is taken back; kPrizeWait
// later (the prize held up a moment) the climber falls off his ladder - the
// game's own fall when a ladder goes over (sub_82289118: the ladder let go,
// sub_82396E80(char, 28159, 6, 0, 0.0)) - since the prize-held pose 28113
// only ends with the match; the prize leaves his hands (+1828, the held
// weapon) and hangs again: the manager's (0x82E3BC30) re-hang, sub_8223BA48
// - the game's own at a match's (re)start.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include <rex/ppc.h>

#include "frame_rate.h"
#include "generated/default/svr2011_init.h"
#include "match_hud.h"
#include "match_types.h"
#include "util.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kMatchOver = 288;
constexpr uint32_t kLost = 447, kBeatenBy = 448, kTeam = 1800, kPerson = 1156;
constexpr int kMaxSides = 6, kTeams = 16;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

using svr2011::NowMs;

enum Kind { kNormal, kFca, kLms, kNoDq, kFirstBlood, kFinishers, kSubmission, kTable, kTlc, kLadder, kTablesLadders };
constexpr const char* kKindName[] = {"NORMAL", "FALLS COUNT ANYWHERE", "LAST MAN STANDING", "NO DISQUALIFICATION",
                                     "FIRST BLOOD", "FINISHERS", "SUBMISSION", "TABLE", "TLC", "LADDER",
                                     "TABLES & LADDERS"};
constexpr Kind kSets[svr2011::kThreeStagesSets][3] = {
    {kNormal, kFca, kLms},
    {kNormal, kFirstBlood, kFinishers},
    {kNormal, kLms, kSubmission},
    {kTable, kTlc, kLadder},
    {kFirstBlood, kLms, kSubmission},
    {kTable, kLadder, kTable},
};
constexpr uint32_t kSixManTag = 0x08;  // (LAST MAN STANDING: NO DISQUALIFICATION, see above)
constexpr int64_t kFinisherWindow = 10000;  // ms
std::atomic<int> g_set{0};
Kind KindOf(int stage) {
  Kind k = kSets[std::clamp(g_set.load(), 0, svr2011::kThreeStagesSets - 1)][std::clamp(stage, 0, 2)];
  return k;
}
std::atomic<bool> g_no_dq{false};      // this match's LAST MAN STANDING is that one
const char* StageName(int stage) {
  const Kind k = KindOf(stage);
  return k == kLms && g_no_dq.load() ? "NO DISQUALIFICATION" : kKindName[k];
}

std::atomic<bool> g_on{false};
std::atomic<int> g_stage{0};               // 0-2
std::atomic<int> g_sides{0};               // sides in the match now (their teams, in person order)
std::atomic<uint32_t> g_side_team[kMaxSides] = {};
std::atomic<int> g_falls[kTeams] = {};     // falls won by each team
std::atomic<int64_t> g_seen{0};            // ms: the match's last update (the score is shown)
std::atomic<int64_t> g_stage_at{0};        // ms: the last stage change (its banner)
// The live rules of fall 1 (saved before fall 2 changes them): a REMATCH
// doesn't set the match up again - its fall 1 gets them back.
uint8_t g_live0[64];
bool g_live0_saved = false;

bool g_falls_any() {
  for (const auto& f : g_falls)
    if (f.load()) return true;
  return false;
}

// The sides: the people's teams now, in person order (teams are set as the
// match starts - kept from earlier, a handicap match showed three sides).
void FindSides(uint8_t* base, const uint32_t (&chars)[6]) {
  uint32_t teams[kMaxSides];
  int n = 0;
  // (the match's own people - live +34 - not a Universe run-in waiting outside: a 1v1 showed three sides)
  const uint32_t people = std::clamp<uint32_t>(base[kLive + 34], 2, 6);
  for (uint32_t i = 0; i < people; ++i) {
    const uint32_t c = chars[i];
    if (!c) continue;
    const uint32_t team = Rd32(base + c + kTeam);
    bool seen = team >= uint32_t(kTeams);
    for (int s = 0; s < n; ++s) seen = seen || teams[s] == team;
    if (!seen && n < kMaxSides) teams[n++] = team;
  }
  for (int s = 0; s < n; ++s) g_side_team[s] = teams[s];
  g_sides = n;
}
uint32_t TeamOf(uint8_t* base, uint32_t c) { return std::min<uint32_t>(Rd32(base + c + kTeam), kTeams - 1); }

// The live rules for a stage: from the match's own (fall 1's, saved), what
// wins a fall set as the stage's kind.
void ApplyStage(uint8_t* base, int stage) {
  uint8_t* live = base + kLive;
  if (!g_live0_saved) std::memcpy(g_live0, live, sizeof(g_live0)), g_live0_saved = true;
  std::memcpy(live, g_live0, sizeof(g_live0));
  if (stage > 0) g_stage_at = NowMs();
  // (no win condition but the stage's: pin, give up, table, ladder, TLC, last man standing, first blood)
  const auto only = [&](int pin, int give_up) {
    live[1] = uint8_t(pin), live[7] = uint8_t(give_up);
    live[17] = 0, live[18] = 0, live[19] = 0, live[24] = 0, live[26] = 0, live[40] = 0;
  };
  // (no count-outs in any stage: a count-out ends the match outside the falls
  // judge - a normal fall 1 ended so, the stages never seen)
  live[5] = 0;
  Kind kind = KindOf(stage);
  if (kind == kLms && live[0] == kSixManTag) kind = kNoDq, g_no_dq = true;
  switch (kind) {
    case kNormal: break;  // (the rule's own)
    case kFca:
      only(2, live[7]);
      live[17] = 1, live[3] = 0, live[5] = 0, live[27] = 0;
      break;
    case kLms:
      only(0, 0);
      live[26] = 1, live[3] = 0, live[5] = 0, live[27] = 0;
      break;
    case kNoDq:
      only(1, live[7] ? live[7] : 1);  // (pinfall in the ring)
      live[3] = 0, live[10] = 0, live[27] = 0;  // (+27 DISQUALIFICATION off: 0 - live bytes never hold the lock bit)
      break;
    case kFirstBlood:
      only(0, 0);
      live[40] = 1;
      break;
    case kFinishers:  // (a pin or give up; the port takes back one not after a finisher)
      only(1, 1);
      break;
    case kSubmission:
      only(0, 1);
      break;
    case kTable:
      only(0, 0);
      live[18] = 1;
      break;
    case kTlc:
      only(0, 0);
      live[24] = 1;
      break;
    case kLadder:
      only(0, 0);
      live[19] = 1;
      break;
    case kTablesLadders:  // (pin or give up, no DQ)
      only(1, 1);
      live[3] = 0, live[4] = 1, live[10] = 0;
      break;
  }
  REXLOG_INFO("three stages: fall {} - {}", stage + 1, StageName(stage));
}

// The prize: a fall for it taken back, the climber off his ladder and the
// prize back up kPrizeWait later.
constexpr uint8_t kPrizeLost = 6;
constexpr uint32_t kPrizeManager = 0x82E3BC30, kMotion = 212, kHeld = 1828, kObjects = 0x82DE0220;
constexpr uint32_t kLadderObject = 11, kLadderFall = 28159;
constexpr int64_t kPrizeWait = 1500;  // ms
std::atomic<int64_t> g_prize_since{0};  // ms: a prize fall taken back (0: none)
bool PrizeStage(int stage) {
  const Kind k = KindOf(stage);
  return k == kLadder || k == kTlc;
}
bool GrabMotion(uint32_t m) { return (m >= 1641 && m <= 1643) || (m >= 28110 && m <= 28115); }

// FINISHERS: the last move cinematic (ms).
std::atomic<int64_t> g_cinematic_at{0};

// The score at the top and, at a stage change, a banner (match_hud.h: only
// while the match runs - not in the menus, select, entrances, pauses,
// replays or the end screens).
class Score final : public rex::ui::ImGuiDialog {
 public:
  explicit Score(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_on.load() || !svr2011::MatchHudVisible()) return;
    std::string score = "0 - 0";
    if (const int n = g_sides.load(); n >= 2) {
      score.clear();
      for (int s = 0; s < n; ++s)
        score += (s ? " - " : "") + std::to_string(g_falls[std::min<uint32_t>(g_side_team[s].load(), kTeams - 1)].load());
    }
    const int stage = g_stage.load();
    svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, "THREE STAGES OF HELL",
                          score + "    FALL " + std::to_string(stage + 1) + ": " + StageName(stage));
    // A stage change: its banner for 5 s from when the match is seen again
    // (a fall stops the match's clock a while - the game's own moment).
    const int64_t now = NowMs();
    if (stage != banner_stage_) banner_stage_ = stage, banner_from_ = stage > 0 ? now : 0;
    if (banner_from_ && now - banner_from_ < 5000)
      svr2011::DrawHudBanner(io.DisplaySize.x, io.DisplaySize.y, "FALL " + std::to_string(stage + 1), StageName(stage),
                             float(now - banner_from_) / 5000.0f);
  }

 private:
  int banner_stage_ = 0;
  int64_t banner_from_ = 0;
};

}  // namespace

namespace svr2011 {

void InstallThreeStagesOverlay(rex::ui::ImGuiDrawer* drawer) { new Score(drawer); }

void ThreeStagesSetup(bool on, int set) {
  g_on = on;
  g_set = std::clamp(set, 0, kThreeStagesSets - 1);
  g_cinematic_at = 0;
  g_live0_saved = false;
  g_stage_at = 0;
  g_stage = 0;
  g_no_dq = false;
  g_sides = 0;
  g_prize_since = 0;
  for (auto& f : g_falls) f = 0;
  if (on) REXLOG_INFO("three stages: on - {} / {} / {}", kKindName[kSets[g_set][0]], kKindName[kSets[g_set][1]],
                      kKindName[kSets[g_set][2]]);
}

void ThreeStagesCinematic() {
  if (g_on.load()) g_cinematic_at = NowMs();
}

bool ThreeStagesMatch() { return g_on.load(); }

// Each world update: the score stays shown (the judge isn't asked during a
// count). Test aid: SVR2011_TEST_3S_LOG=1 - the characters once a second.
void ThreeStagesUpdate(uint8_t* base) {
  // A new match (its clock starts again: a REMATCH, which doesn't set the
  // match up again) starts at fall 1, 0 - 0, with fall 1's rules.
  static uint32_t last_clock = 0;
  const uint32_t clock = Rd32(base + 0x82E3CD0C);
  if (clock < last_clock && (g_stage.load() > 0 || g_falls_any())) {
    if (g_live0_saved && base[kLive] == g_live0[0]) ApplyStage(base, 0);
    g_stage = 0;
    g_no_dq = false;
    for (auto& f : g_falls) f = 0;
    g_stage_at = 0;
    REXLOG_INFO("three stages: a new match (a rematch) - fall 1, 0 - 0");
  }
  last_clock = clock;
  // A match starts (the port's "in a match", frame_rate.h - not the menus
  // after the title's demo match, whose clock is left over): fall 1, 0 - 0,
  // the match's own rules saved at its first update.
  static bool was_in = false;
  const bool in = svr2011::InMatch();
  if (in && !was_in) {
    g_live0_saved = false;
    g_stage = 0;
    g_no_dq = false;
    for (auto& f : g_falls) f = 0;
    g_stage_at = 0;
  }
  was_in = in;
  if (!in || !Rd32(base + kChars) || int32_t(clock) <= 0) return;  // (a match running)
  if (!g_live0_saved) {
    // (test aid: SVR2011_TEST_3S_STAGE=<0-2> - the match starts at that stage)
    static const int first = [] { const char* v = std::getenv("SVR2011_TEST_3S_STAGE"); return v ? std::clamp(std::atoi(v), 0, 2) : 0; }();
    ApplyStage(base, 0);
    if (first > 0) g_stage = first, ApplyStage(base, first);
  }
  g_seen = NowMs();
  static const bool log = std::getenv("SVR2011_TEST_3S_LOG") != nullptr;
  static uint32_t frames = 0;
  if (!log || ++frames % 60) return;
  std::string all;
  for (uint32_t i = 0; i < 9; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4))
      all += fmt::format(" {}:{:08X} state {} lost {} by {} team {} role {} area {} person {} motion {} held {:X}", i, c,
                         base[c + 446], base[c + kLost], base[c + kBeatenBy], Rd32(base + c + 1800), Rd32(base + c + 2624),
                         base[c + 444], Rd32(base + c + 2660), Rd32(base + c + kMotion), Rd32(base + c + kHeld));
  std::string opt;
  for (int i = 0; i < 40; ++i) opt += fmt::format("{:02X}", base[kLive + i]);
  REXLOG_INFO("three stages: live {} characters{}", opt, all);
}

void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// The prize back up: out of the climber's hands, the climber off his ladder
// (the ladder's own fall), the manager's re-hang.
void PrizeRehang(PPCContext& ctx, uint8_t* base, const uint32_t (&chars)[6]) {
  const uint32_t mgr = Rd32(base + kPrizeManager);
  if (!mgr) return;
  const uint32_t prize = Rd32(base + mgr + 52), id = prize ? Rd32(base + prize + 1152) : ~0u;
  for (uint32_t c : chars) {
    if (!c) continue;
    const uint32_t motion = Rd32(base + c + kMotion);
    if (prize && Rd32(base + c + kHeld) == id) {
      Wr32(base + c + kHeld, ~0u);  // (-1: nothing held)
      REXLOG_INFO("three stages: person {} lets go of the prize (motion {})", Rd32(base + c + kPerson), motion);
    }
    if (!GrabMotion(motion)) continue;
    // (his ladder: one he is on lets him go - as sub_82289118 does)
    for (uint32_t s = 0; s < 72; ++s)
      if (const uint32_t o = Rd32(base + kObjects + s * 4); o && Rd32(base + o + 80) == kLadderObject && Rd32(base + o + 96) == c)
        Wr32(base + o + 44, ~0u), Wr32(base + o + 96, 0);
    if (Rd32(base + c + kHeld) != ~0u) Wr32(base + c + kHeld, ~0u);
    PPCContext f = ctx;
    f.r3.u64 = c, f.r4.u64 = kLadderFall, f.r5.u64 = 6, f.r6.u64 = 0, f.f1.f64 = 0.0;
    sub_82396E80(f, base);
    REXLOG_INFO("three stages: person {} falls off the ladder (motion {} -> {})", Rd32(base + c + kPerson), motion,
                Rd32(base + c + kMotion));
  }
  PPCContext c = ctx;
  c.r3.u64 = mgr;
  sub_8223BA48(c, base);
  REXLOG_INFO("three stages: the prize hangs again");
}

void ThreeStagesBeforeJudge(PPCContext& ctx, uint8_t* base) {
  if (!g_on.load() || !svr2011::InMatch() || !g_live0_saved) return;
  uint32_t chars[6] = {};
  int people = 0;
  for (uint32_t i = 0; i < 6; ++i)
    if ((chars[i] = Rd32(base + kChars + i * 4))) ++people;
  if (people < 2) return;
  g_seen = NowMs();
  FindSides(base, chars);  // (in person order: the score's order)
  if (Rd32(base + kLive + kMatchOver)) return;
  // A prize fall taken back: the climb controller may give it again as the
  // grab plays - taken back again; kPrizeWait later the climber falls off
  // and the prize hangs again.
  if (const int64_t since = g_prize_since.load()) {
    for (uint32_t c : chars)
      if (c && base[c + kLost] == kPrizeLost) base[c + kLost] = 0, base[c + kBeatenBy] = 0;
    if (NowMs() - since < kPrizeWait) return;
    g_prize_since = 0;
    PrizeRehang(ctx, base, chars);
    return;
  }
  // Test aid: SVR2011_TEST_3S_FALL=<s> - every <s> seconds a person of the
  // second side (then the first, in turn) loses a fall - falls 1 and 2 only.
  static const int every = [] { const char* v = std::getenv("SVR2011_TEST_3S_FALL"); return v ? std::atoi(v) : 0; }();
  static int64_t next = 0;
  if (every > 0 && g_stage.load() < 2 && g_sides.load() >= 2) {  // (falls 1 and 2: the third as it comes)
    const int64_t now = NowMs();
    if (!next) next = now + every * 1000;
    if (now >= next) {
      next = now + every * 1000;
      const int loser_side = (g_stage.load() % 2) ? 0 : 1, winner_side = 1 - loser_side;
      uint32_t loser = 0, winner = 0;
      for (uint32_t c : chars) {
        if (!c) continue;
        const uint32_t team = TeamOf(base, c);
        if (team == g_side_team[loser_side].load() && !loser) loser = c;
        if (team == g_side_team[winner_side].load() && !winner) winner = c;
      }
      if (loser && winner && !base[loser + kLost]) {
        base[loser + kLost] = 1;
        base[loser + kBeatenBy] = uint8_t(Rd32(base + winner + kPerson));
        REXLOG_INFO("three stages: test - side {} loses a fall", loser_side);
      }
    }
  }
  for (uint32_t c : chars) {
    if (!c || !base[c + kLost]) continue;
    const int loser = int(TeamOf(base, c));
    // The team that beat him: the team of the person +448 names, else (two sides) the other one.
    int winner = -1;
    const uint32_t by = base[c + kBeatenBy];
    for (uint32_t o : chars)
      if (o && o != c && Rd32(base + o + kPerson) == by && int(TeamOf(base, o)) != loser) winner = int(TeamOf(base, o));
    if (winner < 0 && g_sides.load() == 2)
      winner = int(g_side_team[0].load() == uint32_t(loser) ? g_side_team[1].load() : g_side_team[0].load());
    REXLOG_INFO("three stages: person {} (team {}) lost (code {}, by {}) - fall to team {}", Rd32(base + c + kPerson),
                loser, base[c + kLost], by, winner);
    const bool prize = base[c + kLost] == kPrizeLost;
    // The prize taken in a stage not for it (a table one): no fall - it hangs again
    if (prize && !PrizeStage(g_stage.load())) {
      REXLOG_INFO("three stages: the prize taken in a {} fall - doesn't count, it hangs again", StageName(g_stage.load()));
      base[c + kLost] = 0;
      base[c + kBeatenBy] = 0;
      g_prize_since = NowMs();
      return;
    }
    // FINISHERS: not within kFinisherWindow of a finisher / signature - taken back, not counted
    if (KindOf(g_stage.load()) == kFinishers && NowMs() - g_cinematic_at.load() > kFinisherWindow) {
      REXLOG_INFO("three stages: no finisher before it - the fall doesn't count");
      base[c + kLost] = 0;
      base[c + kBeatenBy] = 0;
      return;
    }
    if (winner < 0 || winner >= kTeams) return;  // (unknown: the judge decides, as a normal match)
    const int won = ++g_falls[winner];
    std::string score;
    for (int s = 0; s < g_sides.load(); ++s)
      score += fmt::format("{}{}", s ? " - " : "", g_falls[std::min<uint32_t>(g_side_team[s].load(), kTeams - 1)].load());
    REXLOG_INFO("three stages: fall {} to team {} - {}", g_stage.load() + 1, winner, score);
    if (won >= 2) return;  // (the judge ends the match)
    base[c + kLost] = 0;   // (taken back: the match goes on)
    base[c + kBeatenBy] = 0;
    if (prize) g_prize_since = NowMs();  // (the prize back up when the climber is down)
    g_stage = std::min(g_stage.load() + 1, 2);
    ApplyStage(base, g_stage.load());
    return;
  }
}

}  // namespace svr2011
