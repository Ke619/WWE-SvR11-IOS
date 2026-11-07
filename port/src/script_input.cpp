// WWE SmackDown vs. Raw 2011 - scripted controller (see script_input.h).

#include "script_input.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <mutex>
#include <vector>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

#include "keyboard_typing.h"
#include "pad_types.h"
#include "free_cam.h"
#include "story_designer.h"
#include "touch_controls.h"

namespace svr2011 {

namespace {

constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x53435254);  // 'SCRT'
// Test aid: "connect" - a second, idle pad connects now (a controller nobody holds, joining late).
constexpr auto kExtraDevice = static_cast<rex::input::DeviceId>(0x53435255);
std::atomic<bool> g_extra_pad{false};
constexpr auto kPollInterval = std::chrono::milliseconds(100);

uint16_t ParseButtons(const std::string& spec) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kNames[] = {
      {"UP", X_INPUT_GAMEPAD_DPAD_UP},       {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},   {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"START", X_INPUT_GAMEPAD_START},      {"BACK", X_INPUT_GAMEPAD_BACK},
      {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"A", X_INPUT_GAMEPAD_A},              {"B", X_INPUT_GAMEPAD_B},
      {"X", X_INPUT_GAMEPAD_X},              {"Y", X_INPUT_GAMEPAD_Y},
  };
  uint16_t bits = 0;
  std::stringstream ss(spec);
  std::string name;
  while (std::getline(ss, name, '+')) {
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    bool found = false;
    for (const auto& n : kNames) {
      if (name == n.name) {
        bits |= n.bit;
        found = true;
      }
    }
    if (!found) {
      REXLOG_WARN("script input: unknown button '{}'", name);
    }
  }
  return bits;
}

// Named routes ("route <name>"): the steps to a place in the game, from the
// start. Menu names are the English ones (the test config's language).
struct Route {
  const char* name;
  const char* steps;
};
constexpr Route kRoutes[] = {
    {"main", "title"},
    {"exhibition", "route main\nmenu PLAY\nmenu ONE ON ONE"},
    {"normal", "route exhibition\nmenu NORMAL MATCH"},
    {"cage", "route exhibition\nmenu STEEL CAGE"},
    {"match_creator", "route exhibition\nmenu MATCH CREATOR"},
    {"online", "route main\nmenu ONLINE"},
    {"options", "route main\nmenu MY\nmenu OPTIONS"},
    {"jukebox", "route main\nmenu MY\nmenu JUKEBOX"},
    // (through the select screen and the match screen to the match loading:
    // with SVR2011_TEST_MATCH / SVR2011_TEST_RULE, the match a test asks for)
    {"match", "route normal\npressuntil A match: rule"},
    // (the same, but the presses stop as the match loads: its entrances play - arena checks of ramps and screens)
    {"match_entrances", "route normal\npressuntil A arena mods: a match loads"},
};

// "until": the text a step waits for in the log, and whether it has come.
std::mutex g_until_mutex;
std::string g_until_text;
std::atomic<bool> g_until_hit{false};

class UntilSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string_view text(msg.payload.data(), msg.payload.size());
    if (text.find("script input:") != std::string_view::npos) return;  // (not the step's own line)
    std::lock_guard lock(g_until_mutex);
    if (!g_until_text.empty() && text.find(g_until_text) != std::string_view::npos) g_until_hit = true;
  }
  void flush_() override {}
};

}  // namespace

// The "until" steps' sink: added once at setup (svr2011_app.cpp, before any
// game thread). rex::AddSink pushes onto each logger's sinks without a lock,
// and spdlog walks them unlocked while it logs: added at the first poll (a
// few seconds in), it raced the game files check thread's log line - a test
// game crashed in spdlog's flush_ 4 s after start.
void InstallScriptLogSink() {
  static bool done = false;
  if (done) return;
  done = true;
  rex::AddSink(std::make_shared<UntilSink>());
}

ScriptInputDriver::ScriptInputDriver(std::filesystem::path command_file)
    : InputDriver(nullptr, 0), file_(std::move(command_file)) {
  // (a copy the game restarted itself into - online_overlay.cpp: the commands
  // already there were the last copy's)
  if (std::getenv("SVR2011_RELAUNCHED")) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file_, ec);
    if (!ec) consumed_ = size_t(size);
  }
}

X_STATUS ScriptInputDriver::Setup() {
  REXLOG_INFO("script input: reading commands from {}", file_.string());
  return X_STATUS_SUCCESS;
}

void ScriptInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Script";
  // Test aid: SVR2011_TEST_SCRIPT_AS_KEYBOARD=1 - the scripted controller is listed as the keyboard (its name,
  // no controller number), the players' order as with a keyboard player (pad_types.cpp)
  static const bool as_keyboard = std::getenv("SVR2011_TEST_SCRIPT_AS_KEYBOARD") != nullptr;
  if (as_keyboard) info.name = "Keyboard and Mouse", info.synthetic = true;
  out.push_back(info);
  if (g_extra_pad.load()) {
    rex::input::DeviceInfo extra;
    extra.id = kExtraDevice;
    extra.name = "Test Pad";
    out.push_back(extra);
  }
}

void ScriptInputDriver::ParseLine(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  if (!(in >> verb) || verb[0] == '#') {
    return;
  }
  std::transform(verb.begin(), verb.end(), verb.begin(), ::tolower);
  Step step;
  if (verb == "press" || verb == "hold") {
    std::string buttons;
    in >> buttons;
    step.buttons = ParseButtons(buttons);
    step.ms = 120;
    in >> step.ms;
    queue_.push_back(step);
    queue_.push_back(Step{.ms = 120});  // release, so repeated presses register
  } else if (verb == "stick") {
    std::string side;
    int x = 0, y = 0;
    in >> side >> x >> y >> step.ms;
    auto clamp = [](int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); };
    (side == "R" || side == "r" ? step.rx : step.lx) = clamp(x);
    (side == "R" || side == "r" ? step.ry : step.ly) = clamp(y);
    // Optional: buttons held meanwhile ("-" none), the right trigger and the left
    // trigger ("stick L 0 32000 2000 LB 255 0").
    std::string buttons;
    if (in >> buttons && buttons != "-") step.buttons = ParseButtons(buttons);
    int rt = 0;
    if (in >> rt) step.right_trigger = static_cast<uint8_t>(std::clamp(rt, 0, 255));
    int lt = 0;  // and the left trigger
    if (in >> lt) step.left_trigger = static_cast<uint8_t>(std::clamp(lt, 0, 255));
    queue_.push_back(step);
  } else if (verb == "trigger") {
    std::string side;
    int v = 0;
    in >> side >> v >> step.ms;
    (side == "R" || side == "r" ? step.right_trigger : step.left_trigger) =
        static_cast<uint8_t>(std::clamp(v, 0, 255));
    queue_.push_back(step);
  } else if (verb == "connect") {  // (test aid: an idle pad connects - applied as the line is read)
    g_extra_pad = true;
    REXLOG_INFO("script input: connect - an idle Test Pad connects");
  } else if (verb == "padtype") {  // (test aid: pad_types.h - applied as the line is read)
    int player = 0;
    std::string type;
    in >> player >> type;
    if (player >= 1) svr2011::SetTestPadType(uint32_t(player - 1), type);
  } else if (verb == "wait") {
    in >> step.ms;
    queue_.push_back(step);
  } else if (verb == "sdcopy" || verb == "sdpaste") {  // (story_designer.h: Ctrl+C / Ctrl+V on its lists)
    step.sd = verb == "sdcopy" ? 1 : 2;
    step.ms = 100;
    queue_.push_back(step);
  } else if (verb == "paste" || verb == "copy") {  // (keyboard_typing.h test aids)
    if (verb == "paste") std::getline(in >> std::ws, step.text);
    step.paste = verb == "paste";
    step.copy = verb == "copy";
    step.ms = 100;
    queue_.push_back(step);
  } else if (verb == "type") {
    std::getline(in >> std::ws, step.text);
    step.ms = 100;
    queue_.push_back(step);
  } else if (verb == "touch" || verb == "drag") {
    step.touch = true;
    in >> step.x0 >> step.y0;
    step.x1 = step.x0, step.y1 = step.y0;
    if (verb == "drag") in >> step.x1 >> step.y1;
    step.ms = 150;
    in >> step.ms;
    queue_.push_back(step);
    queue_.push_back(Step{.ms = 120});  // (lifted, then a pause)
  } else if (verb == "title") {
    step.kind = Step::kTitle;
    queue_.push_back(step);
  } else if (verb == "hotkey") {  // (hotkey <key> [ms]: the free camera's keys, at once - free_cam.h)
    std::string name;
    int ms = 100;
    in >> name >> ms;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    if (!svr2011::FreeCamScriptKey(name.c_str(), ms)) REXLOG_WARN("script input: unknown hotkey '{}'", line);
    else REXLOG_INFO("script input: {}", line);
    return;
  } else if (verb == "where") {  // (logs the open menu group and row: the overnight menu explorer)
    step.kind = Step::kWhere;
    queue_.push_back(step);
  } else if (verb == "pressuntil") {
    std::string buttons;
    in >> buttons;
    step.buttons = ParseButtons(buttons);
    std::getline(in >> std::ws, step.arg);
    if (!step.buttons || step.arg.empty()) {
      REXLOG_WARN("script input: '{}' needs buttons and a text", line);
      return;
    }
    step.kind = Step::kPressUntil;
    queue_.push_back(step);
  } else if (verb == "menu" || verb == "until") {
    std::getline(in >> std::ws, step.arg);
    if (step.arg.empty()) {
      REXLOG_WARN("script input: '{}' needs a name", line);
      return;
    }
    step.kind = verb == "menu" ? Step::kMenu : Step::kUntil;
    queue_.push_back(step);
  } else if (verb == "route") {
    std::string name;
    in >> name;
    const Route* route = nullptr;
    for (const Route& r : kRoutes)
      if (name == r.name) route = &r;
    if (!route) {
      std::string names;
      for (const Route& r : kRoutes) names += std::string(names.empty() ? "" : ", ") + r.name;
      REXLOG_WARN("script input: no route '{}' (routes: {})", name, names);
      return;
    }
    REXLOG_INFO("script input: {}", line);
    std::istringstream steps(route->steps);
    std::string sub;
    while (std::getline(steps, sub)) ParseLine(sub);
    return;
  } else if (verb == "set") {
    std::string name, value;
    in >> name >> value;
    rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("script input: {}", line);
    return;
  } else if (verb == "key") {
    static const struct {
      const char* name;
      uint16_t vk;
    } kKeys[] = {{"BACK", 0x08}, {"ENTER", 0x0D}, {"ESC", 0x1B}, {"LEFT", 0x25},
                 {"UP", 0x26},   {"RIGHT", 0x27}, {"DOWN", 0x28}};
    std::string name;
    in >> name;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    for (const auto& k : kKeys) {
      if (name == k.name) step.key = k.vk;
    }
    if (!step.key) {
      REXLOG_WARN("script input: unknown key '{}'", line);
      return;
    }
    step.ms = 100;
    queue_.push_back(step);
  } else {
    REXLOG_WARN("script input: unknown command '{}'", line);
    return;
  }
  REXLOG_INFO("script input: {}", line);
}

// The command file is read on a thread of its own: an open can block for
// hundreds of ms (antivirus...), and GetState runs on the game thread - the
// tests measured those as 370-830 ms frame stalls that players never have.
// The reader hands over whole lines; PollFile parses them.
namespace {
std::mutex g_file_mutex;
std::vector<std::string> g_file_lines;  // read, not yet parsed
}  // namespace

void ScriptInputDriver::PollFile() {
  static std::once_flag started;
  std::call_once(started, [this] {
    std::thread([file = file_, start = consumed_] {
      size_t consumed = start;
      for (;;) {
        std::this_thread::sleep_for(kPollInterval);
        std::ifstream f(file, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const size_t size = static_cast<size_t>(f.tellg());
        if (size < consumed) consumed = 0;  // file was truncated: start over
        if (size == consumed) continue;
        f.seekg(static_cast<std::streamoff>(consumed));
        std::string text(size - consumed, '\0');
        f.read(text.data(), static_cast<std::streamsize>(text.size()));
        // Only whole lines; a partly written last line waits for the next poll.
        const size_t end = text.rfind('\n');
        if (end == std::string::npos) continue;
        consumed += end + 1;
        std::istringstream lines(text.substr(0, end));
        std::lock_guard lock(g_file_mutex);
        for (std::string line; std::getline(lines, line);) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          g_file_lines.push_back(std::move(line));
        }
      }
    }).detach();
  });
  std::vector<std::string> lines;
  {
    std::lock_guard lock(g_file_mutex);
    lines.swap(g_file_lines);
  }
  for (const std::string& line : lines) ParseLine(line);
}

ScriptInputDriver::Step ScriptInputDriver::CurrentStep() {
  std::lock_guard lock(mutex_);
  if (!started_) {  // (the first poll; the "until" sink was added at setup: InstallScriptLogSink)
    started_ = true;
    if (const char* route = std::getenv("SVR2011_ROUTE"); route && *route && !std::getenv("SVR2011_RELAUNCHED"))
      ParseLine(std::string("route ") + route);
  }
  PollFile();
  const auto now = Clock::now();
  for (;;) {
    if (running_ && current_.kind != Step::kPlain) {
      Step out;
      if (RunSmart(now, out)) return out;
      running_ = false;  // (done: the next step)
      current_ = Step{};
      if (queue_.empty()) REXLOG_INFO("script input: all steps done");  // (for the test tools)
    }
    if (running_ && now < step_end_) return current_;
    constexpr uint32_t kFinger = 7;
    if (running_ && current_.touch) TouchInject(kFinger, 2, current_.x1, current_.y1);
    running_ = !queue_.empty();
    if (!running_) {
      current_ = Step{};
      return current_;
    }
    current_ = queue_.front();
    queue_.pop_front();
    step_end_ = now + std::chrono::milliseconds(current_.ms);
    if (current_.kind != Step::kPlain) {
      smart_ = Smart{};
      smart_.start = smart_.changed = now;
      if (current_.kind == Step::kUntil || current_.kind == Step::kPressUntil) {
        std::lock_guard until(g_until_mutex);
        g_until_text = current_.arg;
        g_until_hit = false;
      }
      continue;
    }
    if (current_.sd) StoryListKey(current_.sd == 2);
    if (current_.paste) PasteForTest(current_.text);
    else if (current_.copy) CopyForTest();
    else if (!current_.text.empty()) TypeText(current_.text);
    if (current_.key) TypeKey(current_.key);
    if (current_.touch) {
      TouchInject(kFinger, 0, current_.x0, current_.y0);
      if (current_.x1 != current_.x0 || current_.y1 != current_.y0) {
        TouchInject(kFinger, 1, current_.x1, current_.y1);
      }
    }
    return current_;
  }
}

// A tap: held 120 ms, then 150 ms released.
void ScriptInputDriver::Press(uint16_t buttons, Clock::time_point now) {
  smart_.holding = buttons;
  smart_.press_end = now + std::chrono::milliseconds(120);
  smart_.release_end = smart_.press_end + std::chrono::milliseconds(150);
}

bool ScriptInputDriver::RunSmart(Clock::time_point now, Step& out) {
  using std::chrono::milliseconds;
  using std::chrono::seconds;
  Smart& m = smart_;
  if (now < m.press_end) {
    out.buttons = m.holding;
    return true;
  }
  if (now < m.release_end) return true;
  const double waited = std::chrono::duration<double>(now - m.start).count();
  uint32_t group = 0, row = 0;
  const bool menu = ScriptMenuState(&group, &row);

  if (current_.kind == Step::kWhere) {
    if (menu)
      REXLOG_INFO("script input: where: group {:X} row {}", group, row);
    else
      REXLOG_INFO("script input: where: no menu");
    return false;
  }

  if (current_.kind == Step::kUntil || current_.kind == Step::kPressUntil) {
    // (test aid SVR2011_TEST_SCRIPT_WAIT=<s>: a longer give-up - whole matches played to their end, repeated)
    static const double give_up = [] {
      const char* e = std::getenv("SVR2011_TEST_SCRIPT_WAIT");
      return e && std::atof(e) > 0 ? std::atof(e) : 180.0;
    }();
    const bool hit = g_until_hit;
    if (hit || waited > give_up) {
      if (hit)
        REXLOG_INFO("script input: until \"{}\": seen after {:.1f} s", current_.arg, waited);
      else
        REXLOG_WARN("script input: until \"{}\": not seen in {:.0f} s - going on", current_.arg, give_up);
      std::lock_guard until(g_until_mutex);
      g_until_text.clear();
      return false;
    }
    if (current_.kind == Step::kPressUntil && now >= m.next) {  // (a press every 1.5 s meanwhile)
      Press(current_.buttons, now);
      m.next = now + milliseconds(1500);
    }
    return true;
  }

  if (current_.kind == Step::kTitle) {
    if (menu && group == 1) {
      REXLOG_INFO("script input: title: main menu after {:.1f} s", waited);
      return false;
    }
    if (waited > 240) {
      REXLOG_WARN("script input: title: no main menu in 240 s - going on");
      return false;
    }
    if (now >= m.next) {
      Press(X_INPUT_GAMEPAD_START, now);
      m.next = now + seconds(2);
    }
    return true;
  }

  // menu <LABEL>
  if (!menu) {
    if (waited > 90) {
      REXLOG_WARN("script input: menu {}: no menu in 90 s - going on", current_.arg);
      return false;
    }
    return true;
  }
  if (m.a_pressed) {
    uint32_t chosen_group = 0, chosen_row = 0;
    if (ScriptMenuSelects(&chosen_group, &chosen_row) != m.selects) {
      if (chosen_group == m.group && chosen_row == m.row_at_press)
        REXLOG_INFO("script input: menu {}: chosen (group {:X} row {}, {:.1f} s)", current_.arg, chosen_group,
                    chosen_row, waited);
      else
        REXLOG_WARN("script input: menu {}: the game took group {:X} row {} instead", current_.arg, chosen_group,
                    chosen_row);
      return false;
    }
    if (now < m.a_deadline) return true;
    if (++m.a_tries >= 3) {
      REXLOG_WARN("script input: menu {}: A not taken - going on", current_.arg);
      return false;
    }
    m.a_pressed = false;  // (again)
  }
  int rows = 0;
  const int want = ScriptMenuRow(group, current_.arg, &rows);
  if (group != m.group || row != m.row) {
    m.group = group, m.row = row;
    m.changed = now;
  }
  if (want < 0) {
    if (waited > 90) {
      REXLOG_WARN("script input: menu {}: not in the open menu (group {:X}: {}) - going on", current_.arg, group,
                  ScriptMenuTexts(group));
      return false;
    }
    return true;
  }
  if (now - m.changed < milliseconds(400) || now - m.pressed_at < milliseconds(800)) return true;  // (settling)
  if (row == uint32_t(want)) {
    uint32_t g = 0, r = 0;
    m.selects = ScriptMenuSelects(&g, &r);
    m.row_at_press = uint32_t(want);
    m.a_pressed = true;
    m.a_deadline = now + seconds(3);
    m.pressed_at = now;
    Press(X_INPUT_GAMEPAD_A, now);
    return true;
  }
  if (m.presses > rows * 3 + 10) {
    REXLOG_WARN("script input: menu {}: the cursor never got to row {} (at {}) - going on", current_.arg, want, row);
    return false;
  }
  if (m.presses > 0 && row == m.row_at_press && ++m.stuck >= 2) {
    m.horizontal = !m.horizontal;  // (a menu across the screen)
    m.stuck = 0;
  }
  const int n = std::max(rows, 1);
  const int down = ((want - int(row)) % n + n) % n;
  const bool forward = down <= n - down;
  Press(m.horizontal ? (forward ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT)
                     : (forward ? X_INPUT_GAMEPAD_DPAD_DOWN : X_INPUT_GAMEPAD_DPAD_UP),
        now);
  m.row_at_press = row;
  m.pressed_at = now;
  ++m.presses;
  return true;
}

// (test aids steering the left stick from the game's side: ScriptStickOverride)
static std::atomic<bool> g_stick_on{false};
static std::atomic<int32_t> g_stick_xy{0};
void ScriptStickOverride(bool on, int16_t x, int16_t y) {
  g_stick_xy = int32_t(uint32_t(uint16_t(x)) << 16 | uint16_t(y));
  g_stick_on = on;
}

X_RESULT ScriptInputDriver::GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) {
  if (id == kExtraDevice && g_extra_pad.load()) {  // (the idle Test Pad: nothing pressed)
    if (out_state) std::memset(out_state, 0, sizeof(*out_state));
    return X_ERROR_SUCCESS;
  }
  if (id != kDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const Step s = CurrentStep();
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = s.buttons;
    out_state->gamepad.left_trigger = s.left_trigger;
    out_state->gamepad.right_trigger = s.right_trigger;
    out_state->gamepad.thumb_lx = s.lx;
    out_state->gamepad.thumb_ly = s.ly;
    if (g_stick_on.load()) {
      const uint32_t o = uint32_t(g_stick_xy.load());
      out_state->gamepad.thumb_lx = int16_t(o >> 16), out_state->gamepad.thumb_ly = int16_t(o & 0xFFFF);
    }
    out_state->gamepad.thumb_rx = s.rx;
    out_state->gamepad.thumb_ry = s.ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (id != kDevice && !(id == kExtraDevice && g_extra_pad.load())) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) {
  return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                               X_INPUT_KEYSTROKE*) {
  return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace svr2011
