// WWE SmackDown vs. Raw 2011 - faster loading screens (see fast_loads.h).
//
// Where the time goes (2 Nov, frame shots every second against the game's task list): from the Start Screen's
// START to the practice ring, 12 s of Loading saved game, Load Successful, Checking Created Superstar save data and
// NOW LOADING - the files behind them read in well under a second; the game counts frames there. Four updates a
// frame cut title -> main menu from 22.4 s to 8.3 s.
//
// The signal: those screens are all the Start Screen's own - its task (vtable 82030008) stays, and the task list
// doesn't change, until the practice ring's scene comes - so from its START taken (pad_types.cpp's hook of its
// press-START frame, sub_8258B5F8, which stops once START is taken) until that task stops running.
//
// The game's frame is its task manager (*0x82EC5F94, sub_8269BE90): per phase (+44: 16-byte entries, +0 the
// first task, +8 the end) a list of tasks, each run (vtable[1]) while its +8 is set; +56 a task per phase.
// Test aid SVR2011_TEST_TASK_LOG=1: the running tasks (phase:vtable) logged whenever they change - how the
// screens were told apart.

#include "fast_loads.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fast_loads, false, "Gameplay",
                    "Faster loading screens: the game runs faster while one is up (their animations play faster)");

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

constexpr uint32_t kManager = 0x82EC5F94;
constexpr uint32_t kStartScreenTask = 0x82030008;  // (the Start Screen's task vtable)
constexpr int kTicks = 4;                           // updates a frame on a loading screen

std::atomic<bool> g_start_taken{false};  // the Start Screen's START taken (until its task stops running)

// The running tasks: (phase << 32) | vtable, sorted (+1 on a phase's own task).
std::vector<uint64_t> RunningTasks(uint8_t* base) {
  std::vector<uint64_t> out;
  const uint32_t mgr = Rd32(base + kManager);
  if (!mgr) return out;
  const uint32_t first = Rd32(base + mgr + 44), end = Rd32(base + mgr + 52);
  const uint32_t phases = end > first ? std::min<uint32_t>((end - first) / 16, 32) : 0;
  const uint32_t solo = Rd32(base + mgr + 56);
  for (uint32_t i = 0; i < phases; ++i) {
    if (solo)
      if (const uint32_t t = Rd32(base + solo + i * 4)) out.push_back(uint64_t(i) << 32 | Rd32(base + t) | 1);
    const uint32_t e = first + i * 16;
    uint32_t p = Rd32(base + e), pe = Rd32(base + e + 8);
    for (int n = 0; p < pe && n < 256; p += 4, ++n) {
      const uint32_t t = Rd32(base + p);
      if (!t || t == 0xFFFFFFFFu || !Rd32(base + t + 8)) continue;
      out.push_back(uint64_t(i) << 32 | Rd32(base + t));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool TaskRunning(uint8_t* base, uint32_t vtable) {
  for (uint64_t t : RunningTasks(base))
    if (uint32_t(t) == vtable) return true;
  return false;
}

void LogTasks(uint8_t* base) {
  static std::vector<uint64_t> last;
  std::vector<uint64_t> now = RunningTasks(base);
  if (now == last) return;
  std::string added, removed;
  for (uint64_t t : now)
    if (!std::binary_search(last.begin(), last.end(), t)) added += fmt::format(" +{}:{:08X}", t >> 32, uint32_t(t));
  for (uint64_t t : last)
    if (!std::binary_search(now.begin(), now.end(), t)) removed += fmt::format(" -{}:{:08X}", t >> 32, uint32_t(t));
  REXLOG_INFO("fast loads: tasks{}{} ({} running)", added, removed, now.size());
  last = std::move(now);
}

}  // namespace

namespace svr2011 {

void FastLoadsStartScreen(bool start_taken) {
  if (start_taken && !g_start_taken.exchange(true) && REXCVAR_GET(fast_loads))
    REXLOG_INFO("fast loads: the Start Screen's loading - {} updates a frame", kTicks);
}

int FastLoadsTicks(uint8_t* base) {
  static const bool task_log = std::getenv("SVR2011_TEST_TASK_LOG") != nullptr;
  if (task_log) LogTasks(base);
  if (g_start_taken.load() && !TaskRunning(base, kStartScreenTask)) {  // (the Start Screen gone: its loading is over)
    g_start_taken = false;
    if (REXCVAR_GET(fast_loads)) REXLOG_INFO("fast loads: the Start Screen's loading over - the usual speed");
  }
  if (!REXCVAR_GET(fast_loads)) return 0;
  return g_start_taken.load() ? kTicks : 0;
}

}  // namespace svr2011
