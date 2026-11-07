// WWE SmackDown vs. Raw 2011 - the CPU's difficulty, part by part (cpu_difficulty.h, docs/CPU_DIFFICULTY.md).
//
// The game keeps its DIFFICULTY (0 EASY - 3 LEGEND) at +10342 of its game block (0x82E3DE00) and reads it in a
// few places. The ones adjusted here:
//  - the CPU's reversal chance: each CPU's reversal roll (sub_82835200) takes the action when a 0-99 roll is under
//    PROB x the move's value / 100; PROB comes from the misc.pac text file (PROB_EASY.. 30 / 55 / 80 / 100) when the
//    roll's object is made (sub_82835150 -> its +8). Set here after that: any 0-200.
//  - damage (sub_821F32C8: a level's scale 0.75 / 1 / 1 / 1.2 on a move's damage fields), the pin kick-out window
//    (sub_8221DD88: 15 / 13 / 11 / 9) and the momentum / stamina rates (sub_821F4D90, sub_821F4FA0, sub_821F5FC0):
//    each reads the byte while it runs - for the call, the byte holds the level chosen here, then its own again.
// Online the host's values count for everyone (session property 0x1000F006), so every game makes the same rolls.
#include "cpu_difficulty.h"

#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "graphics_page.h"
#include "p2p.h"

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_INT32(cpu_reversal_chance, -1, "Gameplay",
                     "The CPU's reversal chance, % (-1 AUTO: the difficulty's - EASY 30, NORMAL 55, HARD 80, LEGEND 100)");
REXCVAR_DEFINE_INT32(cpu_reversal_rates, -1, "Gameplay",
                     "The game's A.I. REVERSAL BALANCING sliders for every kind, 0-10 (x 10; -1 AUTO: its own)");
REXCVAR_DEFINE_INT32(cpu_damage_level, -1, "Gameplay",
                     "Damage as on this difficulty: 0 EASY - 3 LEGEND (-1 AUTO: the match's difficulty)");
REXCVAR_DEFINE_INT32(cpu_pin_level, -1, "Gameplay",
                     "The pin kick-out window as on this difficulty: 0 EASY - 3 LEGEND (-1 AUTO)");
REXCVAR_DEFINE_INT32(cpu_momentum_level, -1, "Gameplay",
                     "Momentum and stamina rates as on this difficulty: 0 EASY - 3 LEGEND (-1 AUTO)");

namespace svr2011 {
namespace {

constexpr uint32_t kGame = 0x82E3DE00;     // the game block (sub_825740C8)
constexpr uint32_t kDifficulty = 10342;    // its DIFFICULTY byte
// (not 0x1000F005, the netcode's since 2.1.6: a host's session had the netcode there, which reads as no tuning, so the
// CPU tab did nothing online - report b7e708)
constexpr uint32_t kProperty = 0x1000F006;  // the host's CpuTuning (Pack)

std::atomic<uint32_t> g_local{0xFFFFFFFFu};  // (packed: Pack)

// (the property: reversal chance in the top byte (0xFF AUTO), then 4 bits each - rates, damage, pin, momentum
// (0xF AUTO) - and 0xC5 in the low byte: a game whose host sends nothing gets AUTO)
uint32_t Pack(const CpuTuning& t) {
  auto n = [](int v, int hi) { return v < 0 ? 0xFu : uint32_t(std::clamp(v, 0, hi)); };
  const uint32_t r = t.reversal < 0 ? 0xFFu : uint32_t(std::clamp(t.reversal, 0, 200));
  return r << 24 | n(t.rates, 10) << 20 | n(t.damage, 3) << 16 | n(t.pin, 3) << 12 | n(t.momentum, 3) << 8 | 0xC5u;
}
CpuTuning Unpack(uint32_t v) {
  if ((v & 0xFF) != 0xC5) return {};
  auto n = [](uint32_t b) { return b == 0xF ? -1 : int(b); };
  return {v >> 24 == 0xFF ? -1 : int(v >> 24), n((v >> 20) & 15), n((v >> 16) & 15), n((v >> 12) & 15), n((v >> 8) & 15)};
}

void Publish() { P2PSetHostProperty(kProperty, g_local.load()); }

void Load() {
  static const bool done = [] {
    CpuTuning t;
    t.reversal = rex::cvar::Query<int32_t>("cpu_reversal_chance");
    t.rates = rex::cvar::Query<int32_t>("cpu_reversal_rates");
    t.damage = rex::cvar::Query<int32_t>("cpu_damage_level");
    t.pin = rex::cvar::Query<int32_t>("cpu_pin_level");
    t.momentum = rex::cvar::Query<int32_t>("cpu_momentum_level");
    g_local = Pack(t);
    Publish();
    if (t != CpuTuning{})
      REXLOG_INFO("[svr2011] CPU difficulty: reversal chance {}, rates {}, damage level {}, pin level {}, momentum level {} "
                  "(-1 AUTO)", t.reversal, t.rates, t.damage, t.pin, t.momentum);
    return true;
  }();
  (void)done;
}

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// A call with the DIFFICULTY byte at `level` (a level 0-3; else as it is).
template <typename F>
void WithLevel(uint8_t* base, int level, F call) {
  if (level < 0 || level > 3) return call();
  uint8_t& b = base[kGame + kDifficulty];
  const uint8_t was = b;
  b = uint8_t(level);
  call();
  b = was;
}

// Test aid: SVR2011_TEST_CPU_LOG=1 - each CPU's reversal rolls (taken / made) and its PROB, every 10 s of match.
struct RollLog {
  uint32_t object = 0, prob = 0, made = 0, taken = 0;
};
RollLog g_rolls[8];
std::atomic<uint32_t> g_hits{0}, g_damage{0};  // (the damage calls and their fields' total, this match)

// (the test log's line, every 10 s of match; a new match starts the counts again)
void LogTick(uint8_t* base) {
  static uint32_t last = 0;
  const uint32_t frames = Rd32(base + 0x82E3CD0C);  // (the match's frames)
  if (frames < last) {
    for (auto& g : g_rolls) g = RollLog{};
    g_hits = 0, g_damage = 0;
    last = 0;
    return;
  }
  if (frames < last + 600) return;
  last = frames;
  std::string line;
  for (const auto& g : g_rolls)
    if (g.object) line += fmt::format(" [{:08X} prob {} taken {}/{}]", g.object, g.prob, g.taken, g.made);
  REXLOG_INFO("[svr2011] CPU reversals at frame {} (difficulty {}):{} | damage {} in {} hits", frames,
              base[kGame + kDifficulty], line, g_damage.load(), g_hits.load());
}

// Test aid: SVR2011_TEST_DIFFICULTY=<0-3> - the match's DIFFICULTY (set before each CPU's AI is made).
void TestDifficulty(uint8_t* base) {
  static const int level = [] {
    const char* v = std::getenv("SVR2011_TEST_DIFFICULTY");
    return v && *v ? std::atoi(v) : -1;
  }();
  if (level >= 0 && level <= 3) base[kGame + kDifficulty] = uint8_t(level);
}

}  // namespace

CpuTuning LocalCpuTuning() {
  Load();
  return Unpack(g_local.load());
}

CpuTuning EffectiveCpuTuning() {
  Load();
  if (P2PMatchLobby().in) {
    const auto host = P2PSessionProperty(kProperty);
    return host ? Unpack(*host) : CpuTuning{};  // (a host without it - an older game: AUTO)
  }
  return Unpack(g_local.load());
}

void SetCpuTuning(const CpuTuning& t) {
  Load();
  g_local = Pack(t);
  rex::cvar::SetFlagByName("cpu_reversal_chance", std::to_string(t.reversal));
  rex::cvar::SetFlagByName("cpu_reversal_rates", std::to_string(t.rates));
  rex::cvar::SetFlagByName("cpu_damage_level", std::to_string(t.damage));
  rex::cvar::SetFlagByName("cpu_pin_level", std::to_string(t.pin));
  rex::cvar::SetFlagByName("cpu_momentum_level", std::to_string(t.momentum));
  SaveConfigSetting("cpu_reversal_chance", std::to_string(t.reversal));
  SaveConfigSetting("cpu_reversal_rates", std::to_string(t.rates));
  SaveConfigSetting("cpu_damage_level", std::to_string(t.damage));
  SaveConfigSetting("cpu_pin_level", std::to_string(t.pin));
  SaveConfigSetting("cpu_momentum_level", std::to_string(t.momentum));
  Publish();
  REXLOG_INFO("[svr2011] CPU difficulty set: reversal chance {}, rates {}, damage level {}, pin level {}, momentum level {}",
              t.reversal, t.rates, t.damage, t.pin, t.momentum);
}

}  // namespace svr2011

// A CPU's AI made (it reads the DIFFICULTY): the test aid's level first.
REX_EXTERN(__imp__sub_82814B08);
REX_HOOK_RAW(sub_82814B08) {
  svr2011::TestDifficulty(base);
  __imp__sub_82814B08(ctx, base);
}
REX_EXTERN(__imp__sub_82812230);
REX_HOOK_RAW(sub_82812230) {
  svr2011::TestDifficulty(base);
  __imp__sub_82812230(ctx, base);
}

// The reversal roll's object, made for each CPU's AI (its +8: PROB, %): the chance chosen here instead.
REX_EXTERN(__imp__sub_82835150);
REX_HOOK_RAW(sub_82835150) {
  __imp__sub_82835150(ctx, base);
  const svr2011::CpuTuning t = svr2011::EffectiveCpuTuning();
  if (t.reversal >= 0 && ctx.r3.u32) svr2011::Wr32(base + ctx.r3.u32 + 8, uint32_t(t.reversal));
  static const bool log = std::getenv("SVR2011_TEST_CPU_LOG") != nullptr;
  if (log && ctx.r3.u32)
    REXLOG_INFO("[svr2011] CPU reversal roll {:08X} made: difficulty {}, chance {}", ctx.r3.u32,
                base[svr2011::kGame + svr2011::kDifficulty], svr2011::Rd32(base + ctx.r3.u32 + 8));
}

// The reversal roll (r3 the object, r4 the move's value 0-255): logged for the tests.
REX_EXTERN(__imp__sub_82835200);
REX_HOOK_RAW(sub_82835200) {
  static const bool log = std::getenv("SVR2011_TEST_CPU_LOG") != nullptr;
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82835200(ctx, base);
  if (!log) return;
  using svr2011::g_rolls;
  int slot = -1;
  for (int i = 0; i < 8 && slot < 0; ++i)
    if (g_rolls[i].object == object) slot = i;
  for (int i = 0; i < 8 && slot < 0; ++i)
    if (!g_rolls[i].object) g_rolls[i].object = object, slot = i;
  if (slot < 0) return;
  auto& r = g_rolls[slot];
  r.prob = svr2011::Rd32(base + object + 8);
  ++r.made;
  if (ctx.r3.u32) ++r.taken;
  svr2011::LogTick(base);
}

// The reversal charge of each kind (sub_82815738 ... sub_82815990: the game's slider x the charge at +off / 50):
// with the rates set, that slider's value is the one chosen here.
namespace {
template <uint32_t kOffset>
void Rates(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const uint32_t self = ctx.r3.u32;
  original(ctx, base);
  const int rates = svr2011::EffectiveCpuTuning().rates;
  if (rates >= 0 && self) ctx.r3.u64 = uint64_t(int64_t(int32_t(svr2011::Rd32(base + self + kOffset))) * rates * 10 / 50);
}
}  // namespace
REX_EXTERN(__imp__sub_82815738);
REX_HOOK_RAW(sub_82815738) { Rates<24>(ctx, base, __imp__sub_82815738); }
REX_EXTERN(__imp__sub_828157B0);
REX_HOOK_RAW(sub_828157B0) { Rates<28>(ctx, base, __imp__sub_828157B0); }
REX_EXTERN(__imp__sub_82815828);
REX_HOOK_RAW(sub_82815828) { Rates<32>(ctx, base, __imp__sub_82815828); }
REX_EXTERN(__imp__sub_828158A0);
REX_HOOK_RAW(sub_828158A0) { Rates<36>(ctx, base, __imp__sub_828158A0); }
REX_EXTERN(__imp__sub_82815918);
REX_HOOK_RAW(sub_82815918) { Rates<44>(ctx, base, __imp__sub_82815918); }
REX_EXTERN(__imp__sub_82815990);
REX_HOOK_RAW(sub_82815990) { Rates<48>(ctx, base, __imp__sub_82815990); }

// Damage, the pin kick-out window, momentum / stamina: as on the chosen level.
REX_EXTERN(__imp__sub_821F32C8);
REX_HOOK_RAW(sub_821F32C8) {
  const uint32_t move = ctx.r4.u32;  // (the attack: its +4 holds four 5-bit damage fields)
  svr2011::WithLevel(base, svr2011::EffectiveCpuTuning().damage, [&] { __imp__sub_821F32C8(ctx, base); });
  static const bool log = std::getenv("SVR2011_TEST_CPU_LOG") != nullptr;
  if (log && move) {
    const uint32_t w = svr2011::Rd32(base + move + 4);
    svr2011::g_damage += (w >> 27) + ((w >> 22) & 31) + ((w >> 17) & 31) + ((w >> 12) & 31);
    ++svr2011::g_hits;
    svr2011::LogTick(base);
  }
}
REX_EXTERN(__imp__sub_8221DD88);
REX_HOOK_RAW(sub_8221DD88) {
  svr2011::WithLevel(base, svr2011::EffectiveCpuTuning().pin, [&] { __imp__sub_8221DD88(ctx, base); });
}
REX_EXTERN(__imp__sub_821F4D90);
REX_HOOK_RAW(sub_821F4D90) {
  svr2011::WithLevel(base, svr2011::EffectiveCpuTuning().momentum, [&] { __imp__sub_821F4D90(ctx, base); });
}
REX_EXTERN(__imp__sub_821F4FA0);
REX_HOOK_RAW(sub_821F4FA0) {
  svr2011::WithLevel(base, svr2011::EffectiveCpuTuning().momentum, [&] { __imp__sub_821F4FA0(ctx, base); });
}
REX_EXTERN(__imp__sub_821F5FC0);
REX_HOOK_RAW(sub_821F5FC0) {
  svr2011::WithLevel(base, svr2011::EffectiveCpuTuning().momentum, [&] { __imp__sub_821F5FC0(ctx, base); });
}

// The grid object's worker and its rebuild. A worker thread (sub_827EB1E8 -> sub_827F1998) steps sub_8281B168, which
// stamps circles into the object's cell grid (+32) and reads +32 afresh for every cell (sub_8281C540). The main thread,
// loading the next match, rebuilds the object (sub_8281E398: sub_8281BC50 frees the grid and nulls +32, then a new one):
// the worker read through the null (SIGSEGV at guest 0xE9, 0x279: cell * 10 + 3; report d4c066, a Fold run on main, 1 ms
// after "a match loads"). The rebuild now waits for the worker's step, and a step that finds a rebuild running is skipped.
// Also the free at a match load (sub_8281BC50 directly). Test aids: SVR2011_TEST_GRID_SLOW_MS=<ms>, SVR2011_TEST_GRID_GUARD_OFF=1.
namespace {
std::recursive_mutex g_grid_mutex;
bool GridGuardOff() {
  static const bool off = std::getenv("SVR2011_TEST_GRID_GUARD_OFF") != nullptr;
  return off;
}
}  // namespace

REX_EXTERN(__imp__sub_8281B168);
REX_HOOK_RAW(sub_8281B168) {
  if (GridGuardOff()) {
    __imp__sub_8281B168(ctx, base);
    return;
  }
  std::unique_lock lock(g_grid_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    static std::atomic<int> skipped{0};
    if (++skipped == 1) REXLOG_INFO("[svr2011] grid: a worker step skipped - the grid is being rebuilt");
    return;
  }
  __imp__sub_8281B168(ctx, base);
}

REX_EXTERN(__imp__sub_8281E398);
REX_HOOK_RAW(sub_8281E398) {
  if (GridGuardOff()) {
    __imp__sub_8281E398(ctx, base);
    return;
  }
  std::lock_guard lock(g_grid_mutex);
  __imp__sub_8281E398(ctx, base);
}

REX_EXTERN(__imp__sub_8281BC50);
REX_HOOK_RAW(sub_8281BC50) {
  if (GridGuardOff()) {
    __imp__sub_8281BC50(ctx, base);
    return;
  }
  std::lock_guard lock(g_grid_mutex);
  __imp__sub_8281BC50(ctx, base);
}

// (the test aid: each circle starts that much later, so a free lands inside the walk)
REX_EXTERN(__imp__sub_8281C540);
REX_HOOK_RAW(sub_8281C540) {
  static const int slow = [] { const char* v = std::getenv("SVR2011_TEST_GRID_SLOW_MS"); return v ? std::atoi(v) : 0; }();
  static int calls = 0;  // (every 16th: the world still runs)
  if (slow > 0 && ++calls % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow));
  __imp__sub_8281C540(ctx, base);
}
