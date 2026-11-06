// Bring-up measurements for the Switch: the real clocks, and in a match a fixed series
// of live settings changes, 15 s each. The perf lines in the game log show what each
// one does to the GPU wait; "[nx] ab:" lines mark the changes.
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
  struct Step {
    const char* name;
    const char* value;
  };
  static const Step kSteps[] = {
      {"native_shadows", "false"},  {"depth_of_field", "false"}, {"motion_blur", "false"},
      {"native_shadows", "true"},   {"depth_of_field", "true"},  {"motion_blur", "true"},
  };
  bool done = false;
  while (!done) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    log_clocks();
    if (!svr2011::InMatch()) {
      continue;
    }
    REXLOG_INFO("[nx] ab: in a match; baseline for 25 s");
    std::this_thread::sleep_for(std::chrono::seconds(25));
    log_clocks();
    for (const Step& s : kSteps) {
      const bool ok = rex::cvar::SetFlagByName(s.name, s.value);
      REXLOG_INFO("[nx] ab: {} = {} ({})", s.name, s.value, ok ? "set" : "NOT SET");
      std::this_thread::sleep_for(std::chrono::seconds(15));
    }
    REXLOG_INFO("[nx] ab: series done");
    done = true;
  }
}

}  // namespace

extern "C" void NxAbStart(void) { std::thread(Run).detach(); }
