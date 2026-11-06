// Clocks on the Switch, and bring-up measurements in the game log.
//
// While loading: CPU 1785 MHz, from the start to the Start Screen and during the loads
// after it. Not the system's boost mode: it takes the GPU down to 76.8 MHz, and the game's
// loading screens count frames.
// At 60 fps (GRAPHICS): CPU 1785 MHz, GPU 768 MHz, memory 1600 MHz, in handheld too; at
// 30 fps the console's normal clocks. Cvar switch_boost_60 turns that off.
#include <switch.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "frame_rate.h"

REXCVAR_DEFINE_BOOL(switch_boost_60, true, "GPU", "Switch: higher clocks when the frame rate is set to 60");
REXCVAR_DECLARE(int32_t, frame_rate);

extern "C" volatile u64 g_nx_read_ticks, g_nx_read_bytes, g_nx_read_calls;

namespace {

std::atomic<bool> g_boot{true};        // from the start to the Start Screen
std::atomic<bool> g_start_load{false};  // the Start Screen's loading
std::atomic<u64> g_match_load_at{0};    // a match's load began (ticks; 0: none)

u32 Rate(ClkrstSession* s) {
  u32 hz = 0;
  return R_SUCCEEDED(clkrstGetClockRate(s, &hz)) ? hz / 1000000 : 0;
}

void Want(ClkrstSession* s, u32 mhz) {
  if (Rate(s) != mhz) clkrstSetClockRate(s, mhz * 1000000);
}

constexpr u64 kTicksPerSecond = 19200000;

void Run() {
  ClkrstSession cpu{}, gpu{}, emc{};
  const bool clocks = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
  bool boosting = false, high = false;
  u64 bytes = 0, ticks = 0, calls = 0, last_bytes = 0;
  const u64 start = armGetSystemTick();
  const u64 boot_cap = start + 45 * kTicksPerSecond;
  for (u32 step = 1;; step++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const u64 now = armGetSystemTick();
    // Once a second: is a match's load still reading?
    if (step % 4 == 0) {
      const u64 b = g_nx_read_bytes;
      const u64 began = g_match_load_at.load();
      if (began && (now - began > 45 * kTicksPerSecond ||
                    (now - began > 3 * kTicksPerSecond && b - last_bytes < 1024 * 1024))) {
        g_match_load_at = 0;
      }
      last_bytes = b;
    }
    if (g_boot.load() && now > boot_cap) g_boot = false;
    const bool loading = g_boot.load() || g_start_load.load() || g_match_load_at.load() != 0;
    if (loading != boosting) {
      boosting = loading;
      REXLOG_INFO("[nx] loading boost {}", loading ? "on" : "off");
    }
    // Once a second: the clocks for the frame rate chosen, the CPU up while loading.
    if (clocks && step % 4 == 0) {
      const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
      const bool focus = appletGetFocusState() == AppletFocusState_InFocus;
      const bool want = REXCVAR_GET(switch_boost_60) && REXCVAR_GET(frame_rate) >= 60 && focus;
      if (want) {
        Want(&cpu, 1785);
        Want(&gpu, 768);
        Want(&emc, 1600);
      } else {
        Want(&cpu, loading && focus ? 1785 : 1020);
        if (high) {
          Want(&gpu, docked ? 768 : 307);
          Want(&emc, docked ? 1600 : 1331);
        }
      }
      if (want != high) REXLOG_INFO("[nx] 60 fps clocks {}", want ? "on" : "off");
      high = want;
    }
    if (step % 20 == 0) {
      const u64 t = g_nx_read_ticks, b = g_nx_read_bytes, c = g_nx_read_calls;
      if (c != calls) {
        REXLOG_INFO("[nx] reads in 5 s: {} calls, {} MB, {} ms in the reads", c - calls, (b - bytes) >> 20,
                    (t - ticks) / 19200);
      }
      ticks = t, bytes = b, calls = c;
    }
    if (step % 120 == 60) {
      if (clocks) {
        REXLOG_INFO("[nx] clocks: CPU {} MHz, GPU {} MHz, memory {} MHz, {}", Rate(&cpu), Rate(&gpu), Rate(&emc),
                    appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
      } else {
        REXLOG_INFO("[nx] clocks: can't be read");
      }
    }
  }
}

}  // namespace

extern "C" void NxAbStart(void) { std::thread(Run).detach(); }
extern "C" void NxBootDone(void) { g_boot = false; }
extern "C" void NxLoadBoost(bool on) { g_start_load = on; }
extern "C" void NxMatchLoad(void) { g_match_load_at = armGetSystemTick(); }
