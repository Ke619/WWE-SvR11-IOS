// Bring-up measurements for the Switch: the real clocks in the game log.
#include <switch.h>

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "frame_rate.h"

extern "C" volatile u64 g_nx_read_ticks, g_nx_read_bytes, g_nx_read_calls;

namespace {

u32 Rate(ClkrstSession* s) {
  u32 hz = 0;
  return R_SUCCEEDED(clkrstGetClockRate(s, &hz)) ? hz / 1000000 : 0;
}

void Run() {
  std::this_thread::sleep_for(std::chrono::seconds(15));
  ClkrstSession cpu{}, gpu{}, emc{};
  const bool clocks = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
  auto log_clocks = [&] {
    if (clocks) {
      REXLOG_INFO("[nx] clocks: CPU {} MHz, GPU {} MHz, memory {} MHz, {}", Rate(&cpu), Rate(&gpu), Rate(&emc),
                  appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    } else {
      REXLOG_INFO("[nx] clocks: can't be read");
    }
  };
  log_clocks();
  u64 ticks = 0, bytes = 0, calls = 0;
  for (int i = 0;; i++) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (i % 6 == 5) log_clocks();
    const u64 t = g_nx_read_ticks, b = g_nx_read_bytes, c = g_nx_read_calls;
    if (c != calls) {
      REXLOG_INFO("[nx] reads in 5 s: {} calls, {} MB, {} ms in the reads", c - calls, (b - bytes) >> 20,
                  (t - ticks) / 19200);
    }
    ticks = t, bytes = b, calls = c;
  }
}

}  // namespace

extern "C" void NxAbStart(void) { std::thread(Run).detach(); }
