// Bring-up measurements for the Switch: the real clocks in the game log.
#include <switch.h>

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "frame_rate.h"

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
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    log_clocks();
  }
}

}  // namespace

extern "C" void NxAbStart(void) { std::thread(Run).detach(); }
