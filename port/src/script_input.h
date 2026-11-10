// WWE SmackDown vs. Raw 2011 - scripted controller for automated testing.
//
// With SVR2011_INPUT_FILE=<path> set, the game's only controller is a virtual
// pad driven by commands appended to that file (one per line), so a test can
// play the game while it runs in the background:
//
//   press <buttons> [ms]        tap buttons (default 120 ms), then release
//   hold <buttons> <ms>         same, explicit duration
//   stick <L|R> <x> <y> <ms>    hold a stick at x,y (-32768..32767)
//   trigger <L|R> <0-255> <ms>  hold a trigger
//   wait <ms>                   idle
//   type <text>                 type on the PC keyboard (keyboard_typing.h)
//   key <BACK|ENTER|ESC|LEFT|RIGHT|UP|DOWN>   press a PC keyboard key
//   bindkey <F1-F12|...>        a window key through the key binds (rex/ui/keybinds.h), e.g. F12 REPORT A PROBLEM
//   set <cvar> <value>          sets a setting at once (not queued; tests of live changes)
//   connect                     an idle "Test Pad" connects now (not queued; late-controller tests;
//                               SVR2011_TEST_SCRIPT_AS_KEYBOARD=1 lists the script as the keyboard)
//   touch <x> <y> <ms>          a finger on the on-screen controller at x, y
//                               (fractions of the window) for ms
//   click <x> <y> [ms]          the mouse's left button at x, y (fractions of the window): the menus by mouse
//   drag <x0> <y0> <x1> <y1> <ms>   a finger down at x0, y0, moved to x1, y1
//
// Steps that wait for the game instead of a fixed time (so a slow phone and a
// fast PC run the same script):
//   where                       logs the open menu's group and row
//                               ("script input: where: group G row R", or
//                               "where: no menu"): the menu explorer
//   title                       START until the main menu is up (skips the
//                               intro movies and the title screen)
//   menu <LABEL>                in the open menu, move to the entry named
//                               LABEL (as the log's "menu select" lines and
//                               "menu group" list name them) and choose it
//   until <text>                wait for a log line containing text
//   pressuntil <buttons> <text> press the buttons every 1.5 s until a log line
//                               contains text ("pressuntil A match: rule":
//                               through the select screens to the match)
//   route <name>                a named route (kRoutes in script_input.cpp),
//                               e.g. "route cage": the Steel Cage character
//                               select; "route match": into the match
//                               SVR2011_TEST_MATCH asks for (test_match.cpp).
//                               SVR2011_ROUTE=<name> runs one at start.
// Each gives up after a while (180 s; test aid SVR2011_TEST_SCRIPT_WAIT=<s>) - a warning in the log - and the
// script goes on.
//
// <buttons> is one or more of A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT
// joined with '+'. Lines starting with '#' are ignored.

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/input/input_driver.h>

namespace svr2011 {

// The menus, for the steps above (menu_hooks.cpp): the main-menu object's
// group and cursor row (false before the menu is first used); the row of the
// entry named label in that group (-1: none) and the group's row count; the
// group's names; how many entries have been chosen (and the last one).
bool ScriptMenuState(uint32_t* group, uint32_t* row);
void ScriptMenuSetRow(uint32_t row);  // (menu_touch: the cursor, directly)
int ScriptMenuRow(uint32_t group, const std::string& label, int* rows);
std::string ScriptMenuTexts(uint32_t group);
uint32_t ScriptMenuSelects(uint32_t* group, uint32_t* row);
// The menu table the game reads when the port changed it (match_types.cpp:
// its rows added) - the steps' row places come from it, not menu.pac's.
void ScriptMenuTable(const uint8_t* table, size_t size);

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;  // X_INPUT_* types and button bits

// The "until" steps' log sink - once, at setup, before any game thread
// (svr2011_app.cpp; adding a sink later races the threads' logging).
void InstallScriptLogSink();

// Test aids: the scripted pad's left stick held at x, y whatever the script says (on false: the script's again).
void ScriptStickOverride(bool on, int16_t x, int16_t y);

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(std::filesystem::path command_file);

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  using Clock = std::chrono::steady_clock;

  struct Step {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int ms = 0;
    std::string text;  // typed when the step starts
    bool paste = false, copy = false;
    int sd = 0;  // ("sdcopy" 1 / "sdpaste" 2: story_designer.h)  // ("paste <text>" / "copy": keyboard_typing.h test aids)
    uint16_t key = 0;  // PC keyboard key pressed when the step starts
    std::string hotkey;  // a key bind's key (hotkey: rex::ui::ProcessKeyEvent)
    bool touch = false;  // a finger (touch_controls.h) from x0, y0 to x1, y1
    bool mouse = false;  // ... the mouse's left button instead (the menus by mouse: "click")
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    enum Kind : uint8_t { kPlain, kTitle, kMenu, kUntil, kPressUntil, kWhere } kind = kPlain;  // (a step that waits for the game)
    std::string arg;  // its label / text
  };

  // A waiting step's progress.
  struct Smart {
    Clock::time_point start{}, next{}, press_end{}, release_end{}, changed{}, pressed_at{}, a_deadline{};
    uint16_t holding = 0;
    uint32_t group = ~0u, row = ~0u, row_at_press = ~0u, selects = 0;
    int presses = 0, stuck = 0, a_tries = 0;
    bool horizontal = false, a_pressed = false;
  };

  void PollFile();
  void ParseLine(const std::string& line);
  Step CurrentStep();
  bool RunSmart(Clock::time_point now, Step& out);  // false once the step is done
  void Press(uint16_t buttons, Clock::time_point now);

  std::filesystem::path file_;  // (read on a thread of its own: PollFile)
  size_t consumed_ = 0;          // where that reading starts (a relaunched copy: past the last copy's)
  std::mutex mutex_;
  std::deque<Step> queue_;
  bool running_ = false;
  bool started_ = false;
  Step current_{};
  Smart smart_{};
  Clock::time_point step_end_{};
  uint32_t packet_ = 0;
};

}  // namespace svr2011
