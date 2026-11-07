// WWE SmackDown vs. Raw 2011 - a faster start (see fast_start.h).
//
// The boot: the legal screens and movies\logo.bik (THQ, Yuke's), then the
// Start Screen ("Press START button": a press-START screen, sub_8258B5F8,
// vtable 82030010), then after its START the profile loads and the game
// shows the practice ring (the training facility's demo match, arena 29)
// with "Press START button for Main Menu"; its START opens the main menu.
// Each of those steps takes START, so the skips press it for the player:
// pulses of START on player 1's pad (XamInputGetState: frame_rate.cpp's
// hook) -
// - skip_intros: from launch until the Start Screen shows (then nothing:
//   the player's own START there picks their pad - pad_types.cpp);
// - skip_training: from the Start Screen's START until the main menu is up
//   (the main-menu object shows group 1: menu_hooks.cpp's ScriptMenuState).
// Each has a time limit, and once the Start Screen shows a held START is
// kept from it for a moment.

#include "fast_start.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "rtwm_streak.h"
#include "script_input.h"
#if SVR2011_DEBUG_SUITE
#include "debug_suite/debug_suite.h"
#endif

REXCVAR_DEFINE_BOOL(skip_intros, false, "Gameplay", "Skip the logos and intro movie at launch: straight to the Start Screen");
REXCVAR_DEFINE_BOOL(skip_training, false, "Gameplay",
                    "Skip the practice ring after the Start Screen: straight to the main menu");

namespace {

using Clock = std::chrono::steady_clock;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

constexpr uint32_t kStartScreenVtable = 0x82030010;
constexpr uint16_t kStart = 0x0010;  // (XInput's button bits: the game's pad state +4)

enum Phase { kBoot, kStartScreen, kTraining, kDone };
std::atomic<int> g_phase{kBoot};
Clock::time_point g_phase_since = Clock::now();
int g_hold_off = 0;  // ms START is kept from the Start Screen
bool g_back = false;  // (kTraining: back from Road to WrestleMania, not the boot's skip)
constexpr uint32_t kLiveArena = 0x82E3DE00 + 64;  // (the match's arena - test_match.cpp)
constexpr uint32_t kPracticeRing = 29;
bool g_logged[3] = {};

void Enter(Phase p) {
  g_phase = p;
  g_phase_since = Clock::now();
}

}  // namespace

namespace svr2011 {

void FastStartScreenFrame(uint8_t* base, uint32_t screen) {
  if (!screen || Rd32(base + screen) != kStartScreenVtable || g_phase.load() != kBoot) return;
  if (REXCVAR_GET(skip_intros)) {
    REXLOG_INFO("fast start: the Start Screen (intros skipped)");
    g_hold_off = 700;  // (ms: a START still held from the skip isn't the player's)
  }
  Enter(kStartScreen);
}

void FastStartPressStart(uint8_t* base, uint32_t screen, uint32_t pad) {
  (void)pad;
  if (!screen || Rd32(base + screen) != kStartScreenVtable || g_phase.load() != kStartScreen) return;
  if (REXCVAR_GET(skip_training)) {
    REXLOG_INFO("fast start: skipping the practice ring");
    Enter(kTraining);
  } else {
    Enter(kDone);
  }
}

void FastStartInput(uint8_t* base, uint32_t state) {
  if (!state) return;
  const int phase = g_phase.load();
  if (phase == kDone) return;
  uint8_t* buttons = base + state + 4;
  const uint16_t now = uint16_t(buttons[0] << 8 | buttons[1]);
  if (phase == kStartScreen) {
    if (Clock::now() - g_phase_since < std::chrono::milliseconds(g_hold_off)) {
      if (now & kStart) buttons[0] = uint8_t((now & ~kStart) >> 8), buttons[1] = uint8_t(now & ~kStart);
      return;
    }
#if SVR2011_DEBUG_SUITE
    if (!svr2011::dbg::BootWanted())  // (the debug suite's direct boot: its START too)
#endif
      return;
  }
  if (phase == kTraining) {
    uint32_t group = 0, row = 0;
    const bool menu = ScriptMenuState(&group, &row);
    if (menu && group == 1) {
      REXLOG_INFO("fast start: the main menu");
      g_back = false;
      Enter(kDone);
      return;
    }
    // (back from Road to WrestleMania: another menu open - a pause menu, START taken by
    // something else - or 30 s gone: stop, START isn't the practice ring's)
    if (g_back && ((menu && group != 0) || Clock::now() - g_phase_since > std::chrono::seconds(30))) {
      REXLOG_WARN("fast start: no main menu after leaving Road to WrestleMania (menu group {:X}) - START left alone",
                  menu ? group : 0);
      g_back = false;
      Enter(kDone);
      return;
    }
  }
  const bool on = phase == kBoot          ? REXCVAR_GET(skip_intros)
                  : phase == kStartScreen ? true
                                          : REXCVAR_GET(skip_training) || g_back;
  const auto limit = std::chrono::seconds(phase == kBoot ? 90 : 120);
  if (!on || Clock::now() - g_phase_since > limit) {
    if (on && !g_logged[phase]) {
      REXLOG_WARN("fast start: gave up skipping ({} s)", int(limit.count()));
      g_logged[phase] = true;
    }
    return;
  }
  // START for 60 ms, then 60 ms without (the pads are read many times a
  // frame); the packet number moves on so the game sees new input.
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_phase_since).count();
  if ((ms / 60) % 2 == 0) {
    const uint16_t b = uint16_t(now | kStart);
    buttons[0] = uint8_t(b >> 8), buttons[1] = uint8_t(b);
    Wr32(base + state, Rd32(base + state) + 1);
  }
}

void FastStartPeopleCommitted(uint8_t* base) {
  // (after the commit: the practice ring's arena is set by then; a story's own commit has its arena)
  if (g_phase.load() != kDone || !InRoadToWrestleMania() || Rd32(base + kLiveArena) != kPracticeRing) return;
  REXLOG_INFO("fast start: back from Road to WrestleMania to the practice ring - START pressed for the main menu");
  g_back = true;
  Enter(kTraining);
}

}  // namespace svr2011
