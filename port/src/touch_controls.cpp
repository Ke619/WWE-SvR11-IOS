// WWE SmackDown vs. Raw 2011 - on-screen touch controller (see touch_controls.h).

#include "touch_controls.h"
#include "touch_icons.h"
#include "pad_types.h"
#include "online_overlay.h"
#include "keyboard_typing.h"
#include "menu_touch.h"
#include "frame_rate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(touch_controls, true, "UI", "On-screen touch controller");
#else
REXCVAR_DEFINE_BOOL(touch_controls, false, "UI", "On-screen touch controller");
#endif
REXCVAR_DEFINE_BOOL(menu_touch, true, "UI", "Tap the game's menus directly");
REXCVAR_DEFINE_BOOL(menu_touch_mouse, true, "UI",
                    "The mouse on the game's menus: a left click acts as a tap (lists, OPTIONS, the select grid, the VS "
                    "screen); the cursor shows while the mouse moves");

REXCVAR_DEFINE_BOOL(touch_auto_layout, true, "UI",
                    "Touch controller: MATCH layout in matches, MENU layout in menus");

namespace svr2011 {

using namespace rex::input;  // X_INPUT_* types and button bits
using rex::X_RESULT;
using rex::X_STATUS;

namespace {

using Clock = std::chrono::steady_clock;

// Triggers, as action bits above the 16 button bits.
constexpr uint32_t kLT = 0x10000;
constexpr uint32_t kRT = 0x20000;

// What a button can press (the editor cycles through these).
struct Action {
  uint32_t mask;
  const char* label;
};
constexpr Action kActions[] = {
    {X_INPUT_GAMEPAD_A, "A"},
    {X_INPUT_GAMEPAD_B, "B"},
    {X_INPUT_GAMEPAD_X, "X"},
    {X_INPUT_GAMEPAD_Y, "Y"},
    {X_INPUT_GAMEPAD_LEFT_SHOULDER, "LB"},
    {X_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
    {kLT, "LT"},
    {kRT, "RT"},
    {X_INPUT_GAMEPAD_LEFT_THUMB, "LS"},
    {X_INPUT_GAMEPAD_RIGHT_THUMB, "RS"},
    {X_INPUT_GAMEPAD_START, "START"},
    {X_INPUT_GAMEPAD_BACK, "BACK"},
    {X_INPUT_GAMEPAD_DPAD_UP, "UP"},
    {X_INPUT_GAMEPAD_DPAD_DOWN, "DOWN"},
    {X_INPUT_GAMEPAD_DPAD_LEFT, "LEFT"},
    {X_INPUT_GAMEPAD_DPAD_RIGHT, "RIGHT"},
    // One-touch moves (the game's two-button inputs).
    {X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_Y, "RB+Y"},
    {X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_Y, "LB+Y"},
    {X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_RIGHT_THUMB, "CAM"},  // (the next MATCH CAMERA: free_cam.h)
};

// The controls sheet (the MATCH layout's "?"): the game's match controls
// (Prima's official guide and the 360 move lists).
const char* const kHelpLeft[] = {
    "L STICK", "Move. With X: directional strike; with A: in / out of the ring",
    "R STICK", "Grapple: flick for a hold, flick again for the move",
    "R3",      "Submission (from a hold, or on a downed opponent)",
    "X",       "Strike. Tap for a combo, hold for a strong strike",
    "A",       "Action: ring in / out, pick up weapons, under the ring",
    "B",       "Irish whip. On a downed opponent: pin",
    "Y",       "Signature / finisher when stored (L STICK + Y: the 2nd)",
    nullptr,
};
const char* const kHelpRight[] = {
    "RT",       "Reversal: press as the move comes",
    "LB",       "Hold to run. Toward a corner: climb it",
    "LT",       "Hold to drag the opponent",
    "RB",       "Turn a groggy opponent around",
    "RB + Y",   "Move Thief (75% momentum). Hold at full: Fired Up",
    "D-PAD",    "Taunts",
    "PINS",     "Kick out / escape: mash or hold as the game shows",
    nullptr,
};
const char* const* const kHelp[2] = {kHelpLeft, kHelpRight};

enum class Kind : uint8_t {
  kButton,  // round (face buttons) or a pill (the rest: `wide`)
  kStick,   // action 0: left stick, 1: right stick
  kDpad,
  kLayout,  // switches MENU / MATCH
  kEdit,    // opens the editor
  kHelp,    // the controls sheet
  kKeyboard,  // the phone's keyboard, for typing into the game's (shown only then)
  kOnline,    // the ONLINE overlay: friends, invites (online_overlay.h; online play on)
};

struct Control {
  std::string id;  // (in the layout file)
  Kind kind = Kind::kButton;
  uint32_t action = 0;
  bool wide = false;
  // Centre: anchor * window width + dx * height, y * height (so a layout
  // keeps its corners on any aspect ratio); size: diameter / height, in
  // units of the window height.
  float anchor = 0, dx = 0, y = 0, size = 0.15f;
  bool visible = true;
  std::string caption;  // under the label (one-touch moves: what it does)
  std::string icon;     // a PNG in UserData/touch_icons drawn instead of the button (touch_icons.h); empty: none
  // State.
  bool pressed = false;
  float sx = 0, sy = 0;  // stick / D-pad deflection, -1..1 (y down)
};

// The sticks and the D-pad show their own state, so only the other controls take an icon.
bool CanHaveIcon(const Control& c) { return c.kind != Kind::kStick && c.kind != Kind::kDpad; }

enum { kMenu = 0, kMatch = 1, kLayouts = 2 };
const char* const kLayoutNames[kLayouts] = {"MENU", "MATCH"};

// ---------------------------------------------------------------------------
// state (UI thread, except the pad state the game reads: g_mutex)

std::mutex g_mutex;
std::vector<Control> g_layouts[kLayouts];
std::atomic<int> g_mode{kMenu};
float g_opacity = 0.5f;
bool g_auto = true;
X_INPUT_GAMEPAD g_pad{};  // what the controls press now (g_mutex)
// A button tapped and let go quicker than the game and the port's pages poll
// still counts: each press lasts at least kMinPress (g_mutex).
constexpr auto kMinPress = std::chrono::milliseconds(80);
std::chrono::steady_clock::time_point g_pressed_at[16], g_hold_until[16];
uint32_t g_packet = 0;

struct Pointer {
  int control = -1;  // in the current layout
};
std::unordered_map<uint32_t, Pointer> g_pointers;

rex::ui::Window* g_window = nullptr;
std::filesystem::path g_file;
ImFont* g_font = nullptr;
float g_w = 0, g_h = 0;  // the window, at the last draw
// The controls' unit: the height of the window's 16:9 area (the window
// height on wide screens; less on near-square ones such as a foldable's
// inner screen, where the height would make them huge).
float g_u = 0;
Clock::time_point g_last_touch{};
// Real controllers connected (ControllerWatch), and when one last connected
// (steady-clock ticks): a controller hides the on-screen one at once.
std::atomic<int> g_controllers{0};
std::atomic<int64_t> g_controller_since{0};

std::atomic<bool> g_editor{false};
std::atomic<bool> g_help{false};
int g_edit_layout = kMenu;
int g_selected = -1;
uint32_t g_drag_pointer = UINT32_MAX;
float g_drag_ox = 0, g_drag_oy = 0;  // finger - centre, at the grab

// The editor's toolbar buttons at the last draw.
struct UiButton {
  ImVec2 min, max;
  int id;
};
std::vector<UiButton> g_ui;

// ---------------------------------------------------------------------------
// layouts

Control Button(const char* id, uint32_t action, float anchor, float dx, float y, float size,
               bool wide = false, const char* caption = "") {
  Control c;
  c.id = id;
  c.action = action;
  c.anchor = anchor, c.dx = dx, c.y = y, c.size = size;
  c.wide = wide;
  c.caption = caption;
  return c;
}

Control Special(const char* id, Kind kind, uint32_t action, float anchor, float dx, float y,
                float size) {
  Control c = Button(id, action, anchor, dx, y, size);
  c.kind = kind;
  return c;
}

std::vector<Control> DefaultLayout(int layout) {
  constexpr float L = 0, R = 1, C = 0.5f;
  std::vector<Control> v;
  // Both: the layout switch, the editor, START and BACK.
  v.push_back(Special("layout", Kind::kLayout, 0, L, 0.10f, 0.06f, 0.09f));
  v.push_back(Special("edit", Kind::kEdit, 0, R, -0.06f, 0.06f, 0.09f));
  v.push_back(Button("back", X_INPUT_GAMEPAD_BACK, C, -0.12f, 0.94f, 0.08f, true));
  v.push_back(Button("start", X_INPUT_GAMEPAD_START, C, 0.12f, 0.94f, 0.08f, true));
  if (layout == kMenu) {
    v.push_back(Special("dpad", Kind::kDpad, 0, L, 0.24f, 0.64f, 0.36f));
    v.push_back(Button("a", X_INPUT_GAMEPAD_A, R, -0.21f, 0.80f, 0.16f));
    v.push_back(Button("b", X_INPUT_GAMEPAD_B, R, -0.07f, 0.64f, 0.16f));
    v.push_back(Button("x", X_INPUT_GAMEPAD_X, R, -0.35f, 0.64f, 0.16f));
    v.push_back(Button("y", X_INPUT_GAMEPAD_Y, R, -0.21f, 0.48f, 0.16f));
    v.push_back(Button("lb", X_INPUT_GAMEPAD_LEFT_SHOULDER, L, 0.16f, 0.30f, 0.10f, true));
    v.push_back(Button("lt", kLT, L, 0.16f, 0.17f, 0.10f, true));
    v.push_back(Button("rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER, R, -0.18f, 0.30f, 0.10f, true));
    v.push_back(Button("rt", kRT, R, -0.18f, 0.17f, 0.10f, true));
    v[5].caption = "SELECT";  // (A)
    v[6].caption = "BACK";    // (B)
    v.push_back(Special("keyboard", Kind::kKeyboard, 0, L, 0.30f, 0.06f, 0.09f));  // (beside MENU)
    v.back().wide = true;
    v.push_back(Special("online", Kind::kOnline, 0, R, -0.20f, 0.06f, 0.09f));  // (beside EDIT)
    v.back().wide = true;
    return v;
  }
  v.push_back(Special("help", Kind::kHelp, 0, L, 0.22f, 0.06f, 0.09f));
  v.push_back(Special("lstick", Kind::kStick, 0, L, 0.22f, 0.70f, 0.34f));
  v.push_back(Special("dpad", Kind::kDpad, 0, L, 0.12f, 0.36f, 0.20f));
  v.push_back(Special("rstick", Kind::kStick, 1, R, -0.47f, 0.82f, 0.22f));
  v.push_back(Button("a", X_INPUT_GAMEPAD_A, R, -0.21f, 0.80f, 0.15f));
  v.push_back(Button("b", X_INPUT_GAMEPAD_B, R, -0.08f, 0.66f, 0.15f));
  v.push_back(Button("x", X_INPUT_GAMEPAD_X, R, -0.34f, 0.66f, 0.15f));
  v.push_back(Button("y", X_INPUT_GAMEPAD_Y, R, -0.21f, 0.52f, 0.15f));
  v.push_back(Button("lb", X_INPUT_GAMEPAD_LEFT_SHOULDER, L, 0.33f, 0.30f, 0.09f, true));
  v.push_back(Button("lt", kLT, L, 0.33f, 0.18f, 0.09f, true));
  v.push_back(Button("rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER, R, -0.14f, 0.36f, 0.09f, true));
  v.push_back(Button("rt", kRT, R, -0.14f, 0.21f, 0.12f, true));
  // One-touch moves and the rest of the pad.
  v.push_back(Button("r3", X_INPUT_GAMEPAD_RIGHT_THUMB, R, -0.47f, 0.60f, 0.08f, true, "SUBMIT"));
  v.push_back(Button("rby", X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_Y, R, -0.37f, 0.20f, 0.08f,
                     true, "FIRED UP"));
  Control l3 = Button("l3", X_INPUT_GAMEPAD_LEFT_THUMB, L, 0.44f, 0.46f, 0.08f, true, "TARGET");
  l3.visible = false;  // (manual targeting only)
  v.push_back(l3);
  // BACK + R3: the next MATCH CAMERA (free_cam.h), beside EDIT
  v.push_back(Button("cam", X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_RIGHT_THUMB, R, -0.18f, 0.06f, 0.09f, true,
                     "CAMERA"));
  // What the buttons do in a match.
  for (Control& c : v) {
    if (!c.caption.empty() || c.kind != Kind::kButton) continue;
    switch (c.action) {
      case X_INPUT_GAMEPAD_A: c.caption = "ACTION"; break;
      case X_INPUT_GAMEPAD_B: c.caption = "WHIP / PIN"; break;
      case X_INPUT_GAMEPAD_X: c.caption = "STRIKE"; break;
      case X_INPUT_GAMEPAD_Y: c.caption = "FINISHER"; break;
      case X_INPUT_GAMEPAD_LEFT_SHOULDER: c.caption = "RUN"; break;
      case X_INPUT_GAMEPAD_RIGHT_SHOULDER: c.caption = "TURN"; break;
      case kLT: c.caption = "DRAG"; break;
      case kRT: c.caption = "REVERSE"; break;
      default: break;
    }
  }
  return v;
}

void ResetLayouts() {
  for (int i = 0; i < kLayouts; ++i) g_layouts[i] = DefaultLayout(i);
  g_opacity = 0.5f;
}

void Save() {
  if (g_file.empty()) return;
  std::ostringstream s;
  s << "# WWE SmackDown vs. Raw 2011 - on-screen controller layout (the game's editor writes it)\n";
  s << "opacity " << g_opacity << "\n";
  s << "auto " << (g_auto ? 1 : 0) << "\n";
  for (int l = 0; l < kLayouts; ++l) {
    for (const Control& c : g_layouts[l]) {
      char line[256];
      std::snprintf(line, sizeof(line), "control %d %s %.3f %.4f %.4f %.4f %d %X\n", l, c.id.c_str(),
                    c.anchor, c.dx, c.y, c.size, c.visible ? 1 : 0, c.action);
      s << line;
      // (a name may hold spaces: it is the rest of the line)
      if (!c.icon.empty()) s << "icon " << l << ' ' << c.id << ' ' << c.icon << '\n';
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(g_file.parent_path(), ec);
  const auto tmp = std::filesystem::path(g_file).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << s.str();
  }
  std::filesystem::rename(tmp, g_file, ec);
  if (ec) REXLOG_WARN("touch controls: could not save {}: {}", g_file.string(), ec.message());
}

void Load() {
  ResetLayouts();
  g_auto = REXCVAR_GET(touch_auto_layout);
  std::ifstream f(g_file);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream in(line);
    std::string key;
    in >> key;
    if (key == "opacity") {
      in >> g_opacity;
      g_opacity = std::clamp(g_opacity, 0.1f, 1.0f);
    } else if (key == "auto") {
      int a = 1;
      in >> a;
      g_auto = a != 0;
    } else if (key == "control") {
      int l = -1, vis = 1;
      std::string id;
      float anchor, dx, y, size;
      std::string action;
      if (!(in >> l >> id >> anchor >> dx >> y >> size >> vis >> action) || l < 0 || l >= kLayouts) continue;
      for (Control& c : g_layouts[l]) {
        if (c.id != id) continue;
        c.anchor = std::clamp(anchor, 0.0f, 1.0f);
        c.dx = dx;
        c.y = std::clamp(y, 0.0f, 1.0f);
        c.size = std::clamp(size, 0.04f, 0.6f);
        c.visible = vis != 0;
        if (c.kind == Kind::kButton) c.action = uint32_t(std::strtoul(action.c_str(), nullptr, 16));
      }
    } else if (key == "icon") {
      // icon <layout> <control id> <file name in UserData/touch_icons>
      int l = -1;
      std::string id, name;
      if (!(in >> l >> id) || l < 0 || l >= kLayouts) continue;
      std::getline(in >> std::ws, name);
      while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
      if (!TouchIconNameValid(name)) continue;  // (a layout file may come from anyone)
      for (Control& c : g_layouts[l]) {
        if (c.id == id && CanHaveIcon(c)) c.icon = name;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// geometry

ImVec2 Centre(const Control& c) { return ImVec2(c.anchor * g_w + c.dx * g_u, c.y * g_h); }

// Half extents (a pill is wider than tall).
ImVec2 Half(const Control& c) {
  const float r = c.size * g_u * 0.5f;
  return c.wide ? ImVec2(r * 1.7f, r * 0.8f) : ImVec2(r, r);
}

// Whether (x, y) touches the control (a little larger than drawn).
bool Hit(const Control& c, float x, float y, float slack = 1.15f) {
  const ImVec2 p = Centre(c), h = Half(c);
  const float dx = (x - p.x) / (h.x * slack), dy = (y - p.y) / (h.y * slack);
  return c.wide ? std::abs(dx) <= 1 && std::abs(dy) <= 1 : dx * dx + dy * dy <= 1;
}

std::vector<Control>& Current() { return g_layouts[g_mode.load()]; }

const char* ActionLabel(uint32_t action) {
  for (const Action& a : kActions)
    if (a.mask == action) return a.label;
  return "?";
}

// ---------------------------------------------------------------------------
// the pad

void UpdatePad() {
  X_INPUT_GAMEPAD pad{};
  uint32_t bits = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;
  for (const Control& c : Current()) {
    switch (c.kind) {
      case Kind::kButton:
        if (c.pressed) bits |= c.action;
        break;
      case Kind::kStick:
        (c.action ? rx : lx) = c.sx;
        (c.action ? ry : ly) = -c.sy;
        break;
      case Kind::kDpad: {
        const float len = std::sqrt(c.sx * c.sx + c.sy * c.sy);
        if (len < 0.25f) break;
        const float ux = c.sx / len, uy = c.sy / len;  // (0.38: 8 directions)
        if (ux > 0.38f) bits |= X_INPUT_GAMEPAD_DPAD_RIGHT;
        if (ux < -0.38f) bits |= X_INPUT_GAMEPAD_DPAD_LEFT;
        if (uy > 0.38f) bits |= X_INPUT_GAMEPAD_DPAD_DOWN;
        if (uy < -0.38f) bits |= X_INPUT_GAMEPAD_DPAD_UP;
        break;
      }
      default:
        break;
    }
  }
  pad.buttons = uint16_t(bits & 0xFFFF);
  pad.left_trigger = (bits & kLT) ? 255 : 0;
  pad.right_trigger = (bits & kRT) ? 255 : 0;
  auto axis = [](float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
  pad.thumb_lx = axis(lx);
  pad.thumb_ly = axis(ly);
  pad.thumb_rx = axis(rx);
  pad.thumb_ry = axis(ry);
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (int b = 0; b < 16; ++b) {
    const bool was = uint16_t(g_pad.buttons) >> b & 1, is = uint16_t(pad.buttons) >> b & 1;
    if (is && !was) g_pressed_at[b] = now;
    if (was && !is) g_hold_until[b] = g_pressed_at[b] + kMinPress;
  }
  g_pad = pad;
}

void ReleaseAll() {
  for (auto& layout : g_layouts)
    for (Control& c : layout) c.pressed = false, c.sx = c.sy = 0;
  g_pointers.clear();
  UpdatePad();
}

void SetMode(int mode) {
  if (g_mode.load() == mode) return;
  ReleaseAll();
  g_mode = mode;
}

// Where a finger on a control points it (sticks, D-pad).
void Aim(Control& c, float x, float y) {
  const ImVec2 p = Centre(c);
  const float r = c.size * g_u * 0.5f * (c.kind == Kind::kStick ? 0.75f : 1.0f);
  float dx = (x - p.x) / r, dy = (y - p.y) / r;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len > 1) dx /= len, dy /= len;
  c.sx = dx, c.sy = dy;
}

// The control under a finger (the nearest one it touches).
// The KEYBOARD button only while the game's keyboard is up (and in the editor).
// ONLINE only with online play on.
bool Shown(const Control& c) {
  if (c.kind == Kind::kOnline) return g_editor || rex::cvar::Query<bool>("online_enabled");
  return c.kind != Kind::kKeyboard || g_editor || svr2011::GameKeyboardOpen();
}

int Find(float x, float y) {
  auto& layout = Current();
  int best = -1;
  float best_d = 1e30f;
  for (int i = 0; i < int(layout.size()); ++i) {
    const Control& c = layout[i];
    if (!c.visible || !Shown(c) || !Hit(c, x, y)) continue;
    const ImVec2 p = Centre(c);
    const float d = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y);
    if (d < best_d) best_d = d, best = i;
  }
  return best;
}

bool Visible() {
  if (!REXCVAR_GET(touch_controls)) return false;
  // With a controller connected (any backend: XInput, SDL), hidden - only a
  // touch after it connected brings it back, for 15 s.
  if (g_controllers.load() > 0 || SDL_HasGamepad()) {
    const Clock::time_point since{Clock::duration(g_controller_since.load())};
    return g_last_touch > since && Clock::now() - g_last_touch < std::chrono::seconds(15);
  }
  return true;
}

// ---------------------------------------------------------------------------
// editor

enum UiId {
  kUiLayoutMenu, kUiLayoutMatch, kUiSmaller, kUiBigger, kUiShow, kUiPrevAction, kUiNextAction,
  kUiFainter, kUiStronger, kUiAuto, kUiReset, kUiDone, kUiPrevIcon, kUiNextIcon,
};

// The PNGs in UserData/touch_icons, listed when the editor opens.
std::vector<std::string> g_icons;

// An icon's name as the toolbar shows it.
std::string IconLabel(const std::string& icon) {
  if (icon.empty()) return "NONE";
  std::string s = icon.substr(0, icon.size() - 4);  // (".png")
  if (s.size() > 14) s = s.substr(0, 12) + "..";
  return s;
}

void OpenEditor() {
  ReleaseAll();
  g_edit_layout = g_mode.load();
  g_selected = -1;
  g_drag_pointer = UINT32_MAX;
  TouchIconsRetry();  // (a picture may have been added since)
  g_icons = TouchIconList();
  g_editor = true;
}

void CloseEditor() {
  g_editor = false;
  g_drag_pointer = UINT32_MAX;
  Save();
  ReleaseAll();
}

void EditorAction(int id) {
  auto& layout = g_layouts[g_edit_layout];
  Control* c = g_selected >= 0 && g_selected < int(layout.size()) ? &layout[g_selected] : nullptr;
  switch (id) {
    case kUiLayoutMenu:
    case kUiLayoutMatch:
      g_edit_layout = id == kUiLayoutMenu ? kMenu : kMatch;
      g_mode = g_edit_layout;  // (shown underneath while editing)
      g_selected = -1;
      break;
    case kUiSmaller:
    case kUiBigger:
      if (c) c->size = std::clamp(c->size * (id == kUiBigger ? 1.12f : 1 / 1.12f), 0.04f, 0.6f);
      break;
    case kUiShow:
      if (c && c->kind != Kind::kEdit) c->visible = !c->visible;  // (the editor stays reachable)
      break;
    case kUiPrevAction:
    case kUiNextAction:
      if (c && c->kind == Kind::kButton) {
        const int n = int(std::size(kActions));
        int k = 0;
        while (k < n && kActions[k].mask != c->action) ++k;
        k = (k + (id == kUiNextAction ? 1 : n - 1)) % n;
        c->action = kActions[k].mask;
        c->caption.clear();
      }
      break;
    case kUiPrevIcon:
    case kUiNextIcon:
      if (c && CanHaveIcon(*c)) {
        // NONE (the drawn button), then each PNG in the folder.
        const int n = int(g_icons.size()) + 1;
        int k = 0;
        for (int i = 0; i < int(g_icons.size()); ++i)
          if (g_icons[i] == c->icon) k = i + 1;
        k = (k + (id == kUiNextIcon ? 1 : n - 1)) % n;
        c->icon = k == 0 ? std::string() : g_icons[k - 1];
      }
      break;
    case kUiFainter:
    case kUiStronger:
      g_opacity = std::clamp(g_opacity + (id == kUiStronger ? 0.1f : -0.1f), 0.1f, 1.0f);
      break;
    case kUiAuto:
      g_auto = !g_auto;
      break;
    case kUiReset:
      g_layouts[g_edit_layout] = DefaultLayout(g_edit_layout);
      g_selected = -1;
      break;
    case kUiDone:
      CloseEditor();
      break;
  }
}

void EditorDown(uint32_t pointer, float x, float y) {
  for (const UiButton& b : g_ui) {
    if (x >= b.min.x && x <= b.max.x && y >= b.min.y && y <= b.max.y) {
      EditorAction(b.id);
      return;
    }
  }
  // A control (hidden ones too): select it and drag it.
  auto& layout = g_layouts[g_edit_layout];
  int best = -1;
  float best_d = 1e30f;
  for (int i = 0; i < int(layout.size()); ++i) {
    if (!Hit(layout[i], x, y)) continue;
    const ImVec2 p = Centre(layout[i]);
    const float d = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y);
    if (d < best_d) best_d = d, best = i;
  }
  g_selected = best;
  if (best >= 0) {
    const ImVec2 p = Centre(layout[best]);
    g_drag_pointer = pointer;
    g_drag_ox = x - p.x, g_drag_oy = y - p.y;
  }
}

void EditorMove(uint32_t pointer, float x, float y) {
  if (pointer != g_drag_pointer || g_selected < 0 || g_h <= 0) return;
  Control& c = g_layouts[g_edit_layout][g_selected];
  const float cx = std::clamp(x - g_drag_ox, 0.0f, g_w), cy = std::clamp(y - g_drag_oy, 0.0f, g_h);
  // Anchored to the side it is on (so it stays there on other screens).
  c.anchor = cx < g_w / 3 ? 0.0f : cx > g_w * 2 / 3 ? 1.0f : 0.5f;
  c.dx = (cx - c.anchor * g_w) / g_u;
  c.y = cy / g_h;
}

// ---------------------------------------------------------------------------
// fingers (UI thread)

// A tap on the game's own menus (menu_touch.h): a finger that came down off the controls and lifted about where it
// was. The events go on to the others meanwhile, as they did.
constexpr uint32_t kMousePointer = 0xFFFFFFF0u;
constexpr uint32_t kNoTap = UINT32_MAX;
uint32_t g_tap_pointer = kNoTap;
float g_tap_x = 0, g_tap_y = 0;
Clock::time_point g_tap_at;

void TapDown(uint32_t pointer, float x, float y) {
  if (!REXCVAR_GET(menu_touch) || (pointer == kMousePointer && !REXCVAR_GET(menu_touch_mouse))) return;
  // (a match with mnk_mode on: the mouse is the player's camera and moves there, not a finger)
  if (pointer == kMousePointer && svr2011::InMatch() && rex::cvar::Query<bool>("mnk_mode")) return;
  g_tap_pointer = pointer, g_tap_x = x, g_tap_y = y, g_tap_at = Clock::now();
}

void TapMove(uint32_t pointer, float x, float y) {
  if (pointer != g_tap_pointer) return;
  if (std::fabs(x - g_tap_x) > g_w * 0.03f || std::fabs(y - g_tap_y) > g_h * 0.05f) g_tap_pointer = kNoTap;
}

void TapPress(uint16_t buttons) {
  std::lock_guard lock(g_mutex);
  const auto until = std::chrono::steady_clock::now() + kMinPress;
  for (int b = 0; b < 16; ++b)
    if (buttons >> b & 1) g_hold_until[b] = until;
}

void TapUp(uint32_t pointer) {
  if (pointer != g_tap_pointer) return;
  g_tap_pointer = kNoTap;
  if (Clock::now() - g_tap_at > std::chrono::milliseconds(700) || g_w <= 0 || g_h <= 0) return;
  // Menus are 16:9 with bars on other shapes of screen.
  const float pw = std::min(g_w, g_h * 16 / 9), ph = pw * 9 / 16;
  MenuTouchTap((g_tap_x - (g_w - pw) / 2) / pw, (g_tap_y - (g_h - ph) / 2) / ph, TapPress);
}

bool Down(uint32_t pointer, float x, float y) {
  g_last_touch = Clock::now();
  static int logged = 0;
  if (logged < 6) {
    ++logged;
    REXLOG_INFO("touch controls: down at {:.0f},{:.0f} (overlay {:.0f}x{:.0f}) -> control {}", x, y, g_w, g_h,
                Visible() && !g_editor && !g_help ? Find(x, y) : -2);
  }
  if (g_editor) {
    EditorDown(pointer, x, y);
    return true;
  }
  if (g_help) {
    g_help = false;
    return true;
  }
  const int i = Visible() ? Find(x, y) : -1;
  if (i < 0) {
    TapDown(pointer, x, y);
    return false;
  }
  Control& c = Current()[i];
  switch (c.kind) {
    case Kind::kLayout:
      SetMode(g_mode.load() == kMenu ? kMatch : kMenu);
      return true;
    case Kind::kEdit:
      OpenEditor();
      return true;
    case Kind::kHelp:
      ReleaseAll();
      g_help = true;
      return true;
    case Kind::kKeyboard:
      svr2011::ToggleSystemKeyboard();
      return true;
    case Kind::kOnline:
      svr2011::ToggleOnlineOverlay();
      return true;
    case Kind::kStick:
    case Kind::kDpad:
      Aim(c, x, y);
      break;
    case Kind::kButton:
      c.pressed = true;
      break;
  }
  g_pointers[pointer] = Pointer{i};
  UpdatePad();
  return true;
}

bool Move(uint32_t pointer, float x, float y) {
  if (g_editor) {
    EditorMove(pointer, x, y);
    return true;
  }
  TapMove(pointer, x, y);
  auto it = g_pointers.find(pointer);
  if (it == g_pointers.end()) return false;
  auto& layout = Current();
  Control& c = layout[it->second.control];
  if (c.kind == Kind::kButton) {
    // Sliding across the buttons presses the one under the finger.
    const int i = Find(x, y);
    if (i >= 0 && i != it->second.control && layout[i].kind == Kind::kButton) {
      bool held = false;  // (by another finger)
      for (auto& [p, ptr] : g_pointers)
        if (p != pointer && ptr.control == it->second.control) held = true;
      c.pressed = held;
      layout[i].pressed = true;
      it->second.control = i;
    }
  } else {
    Aim(c, x, y);
  }
  UpdatePad();
  return true;
}

bool Up(uint32_t pointer) {
  if (g_editor) {
    if (pointer == g_drag_pointer) g_drag_pointer = UINT32_MAX;
    return true;
  }
  TapUp(pointer);
  auto it = g_pointers.find(pointer);
  if (it == g_pointers.end()) return false;
  const int i = it->second.control;
  g_pointers.erase(it);
  bool held = false;
  for (auto& [p, ptr] : g_pointers)
    if (ptr.control == i) held = true;
  if (!held) {
    Control& c = Current()[i];
    c.pressed = false;
    c.sx = c.sy = 0;
  }
  UpdatePad();
  return true;
}

// Window events are in physical pixels; the overlay (ImGui) works in its
// display units - fewer on high-density screens (a phone: 2.6 pixels each).
ImVec2 ToOverlay(float x, float y) {
  if (!g_window || g_w <= 0 || g_h <= 0) return ImVec2(x, y);
  const float pw = float(g_window->GetActualPhysicalWidth()), ph = float(g_window->GetActualPhysicalHeight());
  return ImVec2(pw > 0 ? x * g_w / pw : x, ph > 0 ? y * g_h / ph : y);
}

class TouchListener final : public rex::ui::WindowInputListener {
 public:
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    using A = rex::ui::TouchEvent::Action;
    bool handled = false;
    const ImVec2 p = ToOverlay(e.x(), e.y());
    switch (e.action()) {
      case A::kDown:
        handled = Down(e.pointer_id(), p.x, p.y);
        break;
      case A::kMove:
        handled = Move(e.pointer_id(), p.x, p.y);
        break;
      case A::kUp:
      case A::kCancel:
        handled = Up(e.pointer_id());
        break;
    }
    if (handled) e.set_handled(true);
  }
  // The mouse as one finger (touch-screen PCs report touches as touches; this
  // is for trying the layout with a mouse).
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    const ImVec2 p = ToOverlay(float(e.x()), float(e.y()));
    if (e.button() == rex::ui::MouseEvent::Button::kLeft && Down(kMousePointer, p.x, p.y)) e.set_handled(true);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    const ImVec2 p = ToOverlay(float(e.x()), float(e.y()));
    if (Move(kMousePointer, p.x, p.y)) e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kLeft && Up(kMousePointer)) e.set_handled(true);
  }
};

// ---------------------------------------------------------------------------
// drawing

ImU32 Colour(float r, float g, float b, float a) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }

void Text(ImDrawList* dl, ImVec2 centre, float size, ImU32 colour, const char* text) {
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text);
  dl->AddText(font, size, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f), colour, text);
}

// Text that fits a width (shrunk if needed).
void TextFit(ImDrawList* dl, ImVec2 centre, float size, float width, ImU32 colour, const char* text) {
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  const float w = font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
  if (w > width && w > 0) size *= width / w;
  Text(dl, centre, size, colour, text);
}

// A control drawn as the player's own picture, fitted into the space of the
// button it replaces (a round one or a pill) and dimmed a little while pressed.
void DrawIcon(ImDrawList* dl, const Control& c, const TouchIcon& icon, float alpha) {
  const ImVec2 p = Centre(c), h = Half(c);
  const float r = c.size * g_u * 0.5f;
  const float press = c.pressed ? 0.92f : 1.0f;
  const float fit = std::min(h.x * 2 * press / icon.width, h.y * 2 * press / icon.height);
  const float hw = icon.width * fit * 0.5f, hh = icon.height * fit * 0.5f;
  const float k = c.pressed ? 0.7f : 1.0f;
  dl->AddImage(reinterpret_cast<ImTextureID>(icon.texture), ImVec2(p.x - hw, p.y - hh), ImVec2(p.x + hw, p.y + hh),
               ImVec2(0, 0), ImVec2(1, 1), Colour(k, k, k, alpha));
  if (!c.caption.empty()) {
    TextFit(dl, ImVec2(p.x, p.y + h.y + r * 0.22f), r * 0.42f, r * 2.4f, Colour(1, 1, 1, 0.85f * alpha),
            c.caption.c_str());
  }
}

void FaceColour(uint32_t action, float* r, float* g, float* b) {
  *r = *g = *b = 0.92f;
  if (action == X_INPUT_GAMEPAD_A) *r = 0.35f, *g = 0.80f, *b = 0.25f;
  if (action == X_INPUT_GAMEPAD_B) *r = 0.92f, *g = 0.25f, *b = 0.22f;
  if (action == X_INPUT_GAMEPAD_X) *r = 0.25f, *g = 0.50f, *b = 0.95f;
  if (action == X_INPUT_GAMEPAD_Y) *r = 0.98f, *g = 0.78f, *b = 0.15f;
}

void DrawControl(ImDrawList* dl, const Control& c, float alpha, bool selected) {
  const ImVec2 p = Centre(c), h = Half(c);
  const float r = c.size * g_u * 0.5f;
  const ImU32 fill = Colour(0.05f, 0.05f, 0.07f, (c.pressed ? 0.75f : 0.40f) * alpha);
  const ImU32 line = Colour(1, 1, 1, (c.pressed ? 0.95f : 0.55f) * alpha);
  const float thick = std::max(2.0f, g_u * 0.004f);
  switch (c.kind) {
    case Kind::kStick: {
      dl->AddCircleFilled(p, r, fill, 48);
      dl->AddCircle(p, r, line, 48, thick);
      const float k = r * 0.75f;
      const ImVec2 knob(p.x + c.sx * k, p.y + c.sy * k);
      dl->AddCircleFilled(knob, r * 0.42f, Colour(0.85f, 0.85f, 0.9f, 0.55f * alpha + (c.pressed ? 0.2f : 0)), 32);
      Text(dl, knob, r * 0.28f, Colour(0.1f, 0.1f, 0.12f, alpha), c.action ? "R" : "L");
      if (g_mode.load() == kMatch) {
        TextFit(dl, ImVec2(p.x, p.y - r - r * 0.16f), r * 0.24f, r * 2, Colour(1, 1, 1, 0.8f * alpha),
                c.action ? "GRAPPLE" : "MOVE");
      }
      break;
    }
    case Kind::kDpad: {
      const float a = r * 0.34f;  // arm half-width
      const ImU32 arm = Colour(0.05f, 0.05f, 0.07f, 0.45f * alpha);
      dl->AddRectFilled(ImVec2(p.x - a, p.y - r), ImVec2(p.x + a, p.y + r), arm, a * 0.3f);
      dl->AddRectFilled(ImVec2(p.x - r, p.y - a), ImVec2(p.x + r, p.y + a), arm, a * 0.3f);
      dl->AddRect(ImVec2(p.x - a, p.y - r), ImVec2(p.x + a, p.y + r), line, a * 0.3f, 0, thick);
      dl->AddRect(ImVec2(p.x - r, p.y - a), ImVec2(p.x + r, p.y + a), line, a * 0.3f, 0, thick);
      // Arrows, lit when pressed.
      const float len = std::sqrt(c.sx * c.sx + c.sy * c.sy);
      const float ux = len > 0.25f ? c.sx / len : 0, uy = len > 0.25f ? c.sy / len : 0;
      const struct {
        float dx, dy;
        bool on;
      } arrows[] = {{0, -1, uy < -0.38f}, {0, 1, uy > 0.38f}, {-1, 0, ux < -0.38f}, {1, 0, ux > 0.38f}};
      for (const auto& ar : arrows) {
        const ImVec2 tip(p.x + ar.dx * r * 0.85f, p.y + ar.dy * r * 0.85f);
        const ImVec2 base(p.x + ar.dx * r * 0.55f, p.y + ar.dy * r * 0.55f);
        const ImVec2 side(-ar.dy * a * 0.6f, ar.dx * a * 0.6f);
        dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y),
                              ImVec2(base.x - side.x, base.y - side.y),
                              ar.on ? Colour(1, 1, 1, 0.95f * alpha) : Colour(1, 1, 1, 0.45f * alpha));
      }
      break;
    }
    default: {
      // The player's own picture, if the control has one that loads.
      TouchIcon icon;
      if (!c.icon.empty() && CanHaveIcon(c) && TouchIconGet(c.icon, &icon)) {
        DrawIcon(dl, c, icon, alpha);
        break;
      }
      const char* label = c.kind == Kind::kLayout ? kLayoutNames[g_mode.load()]
                          : c.kind == Kind::kEdit ? "EDIT"
                          : c.kind == Kind::kHelp ? "?"
                          : c.kind == Kind::kKeyboard ? "KEYBOARD"
                          : c.kind == Kind::kOnline   ? "ONLINE"
                                                  : ActionLabel(c.action);
      if (c.wide) {
        dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), fill, h.y);
        dl->AddRect(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), line, h.y, 0, thick);
        if (!c.caption.empty()) {
          TextFit(dl, ImVec2(p.x, p.y - h.y * 0.22f), h.y * 0.95f, h.x * 1.8f, Colour(1, 1, 1, alpha), label);
          TextFit(dl, ImVec2(p.x, p.y + h.y * 0.50f), h.y * 0.58f, h.x * 1.8f, Colour(1, 1, 1, 0.8f * alpha),
                  c.caption.c_str());
        } else {
          TextFit(dl, p, h.y * 1.05f, h.x * 1.7f, Colour(1, 1, 1, alpha), label);
        }
      } else {
        float cr, cg, cb;
        FaceColour(c.kind == Kind::kButton ? c.action : 0, &cr, &cg, &cb);
        dl->AddCircleFilled(p, r, fill, 40);
        dl->AddCircle(p, r, Colour(cr, cg, cb, (c.pressed ? 1.0f : 0.7f) * alpha), 40, thick * 1.5f);
        TextFit(dl, p, r * (std::strlen(label) > 2 ? 0.7f : 1.1f), r * 1.7f, Colour(cr, cg, cb, alpha), label);
        if (!c.caption.empty()) {
          TextFit(dl, ImVec2(p.x, p.y + r * 1.22f), r * 0.42f, r * 2.4f, Colour(1, 1, 1, 0.85f * alpha),
                  c.caption.c_str());
        }
      }
      break;
    }
  }
  if (selected) {
    dl->AddRect(ImVec2(p.x - h.x - 6, p.y - h.y - 6), ImVec2(p.x + h.x + 6, p.y + h.y + 6),
                Colour(1.0f, 0.8f, 0.1f, 1), 6, 0, thick * 1.5f);
  }
}

struct ToolItem {
  std::string text;
  int id;
  bool active;
};

// The editor's buttons: rows centred across the middle of the screen (the
// edges and corners are where the controls are).
void DrawToolbar(ImDrawList* dl, const std::vector<ToolItem>& items) {
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  const float h = std::max(40.0f, g_h * 0.075f), gap = h * 0.2f, size = h * 0.5f;
  const float max_w = std::max(g_w * 0.56f, h * 8);
  // Rows that fit the width.
  std::vector<std::vector<std::pair<const ToolItem*, float>>> rows(1);
  float row_w = 0;
  for (const ToolItem& it : items) {
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0, it.text.c_str()).x + h * 0.6f;
    if (!rows.back().empty() && row_w + gap + w > max_w) rows.emplace_back(), row_w = 0;
    row_w += (rows.back().empty() ? 0 : gap) + w;
    rows.back().push_back({&it, w});
  }
  float y = g_h * 0.42f - (rows.size() * (h + gap)) * 0.5f;
  for (const auto& row : rows) {
    float total = 0;
    for (const auto& [it, w] : row) total += w + gap;
    float x = (g_w - (total - gap)) * 0.5f;
    for (const auto& [it, w] : row) {
      const ImVec2 mn(x, y), mx(x + w, y + h);
      dl->AddRectFilled(mn, mx, it->active ? Colour(0.85f, 0.15f, 0.12f, 0.95f) : Colour(0.12f, 0.12f, 0.15f, 0.92f),
                        h * 0.2f);
      dl->AddRect(mn, mx, Colour(1, 1, 1, 0.6f), h * 0.2f, 0, 2);
      Text(dl, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), size, Colour(1, 1, 1, 1), it->text.c_str());
      g_ui.push_back({mn, mx, it->id});
      x += w + gap;
    }
    y += h + gap;
  }
}

void DrawEditor(ImDrawList* dl) {
  g_ui.clear();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_w, g_h), Colour(0, 0, 0, 0.45f));
  auto& layout = g_layouts[g_edit_layout];
  for (int i = 0; i < int(layout.size()); ++i) {
    DrawControl(dl, layout[i], layout[i].visible ? 1.0f : 0.3f, i == g_selected);
  }
  std::vector<ToolItem> items;
  items.push_back({"MENU LAYOUT", kUiLayoutMenu, g_edit_layout == kMenu});
  items.push_back({"MATCH LAYOUT", kUiLayoutMatch, g_edit_layout == kMatch});
  const Control* c = g_selected >= 0 ? &layout[g_selected] : nullptr;
  if (c) {
    items.push_back({"SIZE -", kUiSmaller, false});
    items.push_back({"SIZE +", kUiBigger, false});
    if (c->kind != Kind::kEdit) items.push_back({c->visible ? "HIDE" : "SHOW", kUiShow, false});
    if (c->kind == Kind::kButton) {
      items.push_back({"<", kUiPrevAction, false});
      items.push_back({std::string("PRESSES ") + ActionLabel(c->action), kUiNextAction, false});
      items.push_back({">", kUiNextAction, false});
    }
    if (CanHaveIcon(*c)) {
      items.push_back({"<", kUiPrevIcon, false});
      items.push_back({"ICON " + IconLabel(c->icon), kUiNextIcon, false});
      items.push_back({">", kUiNextIcon, false});
    }
  }
  items.push_back({"OPACITY -", kUiFainter, false});
  items.push_back({"OPACITY " + std::to_string(int(std::lround(g_opacity * 100))) + "% +", kUiStronger, false});
  items.push_back({g_auto ? "AUTO SWITCH: ON" : "AUTO SWITCH: OFF", kUiAuto, false});
  items.push_back({"RESET LAYOUT", kUiReset, false});
  items.push_back({"DONE", kUiDone, true});
  DrawToolbar(dl, items);
  const float h = std::max(40.0f, g_h * 0.075f);
  Text(dl, ImVec2(g_w * 0.5f, g_h * 0.97f), h * 0.42f, Colour(1, 1, 1, 0.85f),
       c ? (g_icons.empty() && CanHaveIcon(*c)
                ? "Drag to move. To give it an icon, put PNG pictures in UserData/touch_icons and reopen EDIT."
                : "Drag to move. The buttons above size it, hide it, change what it presses or its icon.")
         : "Tap a control to change it, or drag it to move it.");
}

void DrawHelp(ImDrawList* dl) {
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_w, g_h), Colour(0, 0, 0, 0.85f));
  const float line = g_h * 0.085f;
  const float size = std::min(g_h * 0.036f, g_w * 0.017f);
  Text(dl, ImVec2(g_w * 0.5f, g_h * 0.07f), size * 1.5f, Colour(1, 0.8f, 0.2f, 1), "MATCH CONTROLS");
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  for (int k = 0; k < 2; ++k) {
    const float x0 = g_w * (k ? 0.51f : 0.03f), key_w = g_w * 0.09f, text_w = g_w * 0.36f;
    float y = g_h * 0.17f;
    for (const char* const* s = kHelp[k]; *s; s += 2) {
      dl->AddText(font, size, ImVec2(x0, y), Colour(1, 0.8f, 0.2f, 1), s[0]);
      // (wrapped to its column)
      dl->AddText(font, size, ImVec2(x0 + key_w, y), Colour(1, 1, 1, 1), s[1], nullptr, text_w);
      y += line;
    }
  }
  Text(dl, ImVec2(g_w * 0.5f, g_h * 0.95f), size, Colour(1, 1, 1, 0.8f),
       "Tap to close.  Tip: the gear (EDIT) button moves, resizes and remaps every control.");
}

class TouchOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    // On top of the port's pages (GRAPHICS, JUKEBOX, ACHIEVEMENTS...: they draw
    // on the foreground list too, in the drawer's dialog order, and are added
    // after this one): once a frame the overlay moves to the end of the dialog
    // list, and the drawer's loop reaches it again last. (Taps on its buttons
    // never reach the pages: its input listener comes first and takes them.)
    if (const int frame = ImGui::GetFrameCount(); frame != raised_frame_) {
      raised_frame_ = frame;
      imgui_drawer()->RemoveDialog(this);
      imgui_drawer()->AddDialog(this);
      return;
    }
    g_w = io.DisplaySize.x;
    g_h = io.DisplaySize.y;
    if (g_h <= 0) return;
    g_u = std::min(g_h, g_w * 9.0f / 16.0f);
    if (g_editor) {
      DrawEditor(ImGui::GetForegroundDrawList());
      return;
    }
    if (g_help) {
      DrawHelp(ImGui::GetForegroundDrawList());
      return;
    }
    if (!Visible()) {
      if (!g_pointers.empty()) ReleaseAll();
      return;
    }
    svr2011::UpdateSystemKeyboard();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const Control& c : Current()) {
      if (c.visible && Shown(c)) DrawControl(dl, c, g_opacity, false);
    }
  }

 private:
  int raised_frame_ = -1;
};

TouchListener g_listener;

// The game going out of sight (folding the phone, home, a call, the
// notification shade) can take a finger without its up or cancel - its
// control would stay held and the player act on his own when the game is
// back (B18): everything held is let go then.
class FocusListener final : public rex::ui::WindowListener {
 public:
  void OnLostFocus(rex::ui::UISetupEvent&) override { LetGo("focus lost"); }
  void OnMinimized(rex::ui::UIEvent&) override { LetGo("minimized"); }

 private:
  static void LetGo(const char* why) {
    if (g_pointers.empty()) return;
    REXLOG_INFO("touch controls: {} with {} finger(s) down - let go", why, g_pointers.size());
    ReleaseAll();
  }
};

FocusListener g_focus_listener;

class TouchDriver final : public InputDriver {
 public:
  TouchDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    // (always: switched on in the GRAPHICS page, it works at once; idle, it
    // is an idle pad merged into player 1's)
    DeviceInfo info;
    info.id = kDevice;
    info.name = "Touch";
    info.synthetic = true;  // (merged into player 1's controller)
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      std::lock_guard lock(g_mutex);
      out->packet_number = ++g_packet;
      if (!TouchControlsHoldInput()) {
        out->gamepad = g_pad;
        const auto now = std::chrono::steady_clock::now();
        uint16_t held = uint16_t(out->gamepad.buttons);
        for (int b = 0; b < 16; ++b)
          if (now < g_hold_until[b]) held |= uint16_t(1u << b);
        out->gamepad.buttons = held;
      }
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out->gamepad.buttons = 0xFFFF;
      out->gamepad.left_trigger = 0xFF;
      out->gamepad.right_trigger = 0xFF;
      out->gamepad.thumb_lx = int16_t(0x7FFF);
      out->gamepad.thumb_ly = int16_t(0x7FFF);
      out->gamepad.thumb_rx = int16_t(0x7FFF);
      out->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<DeviceId>(0x54554348);  // 'TUCH'
};

// The SDK's device assignment (SlotAssignment), counting the real controllers
// (not the keyboard, the touch pad or the stand-in: those are synthetic).
class ControllerWatch final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
    static const bool script_is_pad = [] {  // (tests: the scripted controller counts)
      const char* v = std::getenv("SVR2011_SCRIPT_IS_PAD");
      return v && *v == '1';
    }();
    int n = 0;
    std::string names;
    for (const DeviceInfo& d : devices) {
      if (d.synthetic || (d.name == "Script" && !script_is_pad)) continue;
      ++n;
      names += (names.empty() ? "" : ", ") + d.name;
    }
    const int before = g_controllers.exchange(n);
    if (n > before) g_controller_since = Clock::now().time_since_epoch().count();
    if (n != before) {
      REXLOG_INFO("touch controls: {} controller(s){}{}", n, n ? ": " : "", names);
    }
    slots_.OnDevicesChanged(devices);
  }
  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
    slots_.DevicesForUser(user_index, out);
  }

 private:
  PlayerSlots slots_;  // (pad_types.h: the keyboard is a player of its own)
};

}  // namespace

std::unique_ptr<InputDriver> CreateTouchDriver() { return std::make_unique<TouchDriver>(); }

std::unique_ptr<DeviceAssignment> CreateControllerWatch() { return std::make_unique<ControllerWatch>(); }

void InstallTouchControls(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                          const std::filesystem::path& user_data, rex::ui::ImmediateDrawer* immediate) {
  g_window = window;
  g_file = user_data / "touch_layout.txt";
  TouchIconsInit(immediate, user_data);
  Load();
  if (drawer) new TouchOverlay(drawer);  // lives for the whole run
  if (window) {
    window->app_context().CallInUIThread([window] {
      window->AddInputListener(&g_listener, 900);
      window->AddListener(&g_focus_listener);
      // (the menus by mouse: its cursor shows while it moves and hides again - mnk_mode's capture hides it and puts
      // this back after; menu_touch_mouse off: as before, always shown)
      if (REXCVAR_GET(menu_touch) && REXCVAR_GET(menu_touch_mouse))
        window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    });
  }
  REXLOG_INFO("touch controls: {} ({})", REXCVAR_GET(touch_controls) ? "on" : "off", g_file.string());
}

void SetTouchControlsFont(ImFont* font) { g_font = font; }

float TouchControlsSideInset(bool right, float top, float bottom) {
  if (!Visible() || g_editor.load() || g_help.load() || g_w <= 0) return 0;
  float inset = 0;
  for (const Control& c : Current()) {
    if (!c.visible || !Shown(c)) continue;
    const ImVec2 p = Centre(c), h = Half(c);
    if (p.y + h.y < top || p.y - h.y > bottom) continue;           // (not in the band)
    if (right ? p.x < g_w * 0.5f : p.x > g_w * 0.5f) continue;  // (the other half)
    inset = std::max(inset, right ? g_w - (p.x - h.x) : p.x + h.x);
  }
  return inset > 0 ? inset + 4.0f : 0.0f;
}

bool TouchControlsHoldInput() { return g_editor.load() || g_help.load(); }

void TouchGameInMatch(bool in_match) {
  if (!g_auto || !g_window) return;
  const int mode = in_match ? kMatch : kMenu;
  if (g_mode.load() == mode) return;
  g_window->app_context().CallInUIThread([mode] {
    if (!g_editor) SetMode(mode);
  });
}

void TouchInject(uint32_t pointer, int action, float x, float y) {
  if (!g_window) return;
  g_window->app_context().CallInUIThread([=] {
    const float px = x * g_w, py = y * g_h;
    if (action == 0) Down(pointer, px, py);
    else if (action == 1) Move(pointer, px, py);
    else Up(pointer);
  });
}

void TouchControlsHeld(uint16_t* buttons, uint8_t* left_trigger, uint8_t* right_trigger) {
  std::lock_guard lock(g_mutex);
  uint16_t held = uint16_t(g_pad.buttons);
  const auto now = std::chrono::steady_clock::now();
  for (int b = 0; b < 16; ++b)
    if (now < g_hold_until[b]) held |= uint16_t(1u << b);
  *buttons = held, *left_trigger = uint8_t(g_pad.left_trigger), *right_trigger = uint8_t(g_pad.right_trigger);
}

}  // namespace svr2011
