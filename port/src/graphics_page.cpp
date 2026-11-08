// WWE SmackDown vs. Raw 2011 - the GRAPHICS page (see graphics_page.h).
//
// Drawn over the game's OPTIONS panel in the same style (the panel, its
// header, one bar per setting, the selected one red with < > around the
// value), and every setting applies at once (CROWD and GRAPHICS API: at the
// next start). Three tabs: DISPLAY, GRAPHICS and ADV. GRAPHICS. The same page
// is also MY WWE -> OPTIONS -> CONTROLS, which rebinds the keyboard and the mouse
// (the SDK's keyboard driver reads its keybind_* settings every poll, so a change
// applies at once too) and each player's controller (pad_map.h), and LANGUAGE.

#include "frame_rate.h"
#include "free_cam.h"
#include "graphics_page.h"
#include "platform.h"
#include "cpu_difficulty.h"
#include "crowd_off.h"
#include "display_modes.h"
#include "arena_lights.h"
#include "port_map.h"
#include "backgrounds_page.h"
#include "report_page.h"
#include "weapons_page.h"
#include "mystery_opponent.h"
#include "vs_styles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "achievements_page.h"
#include "fps_overlay.h"
#include "jukebox.h"
#include "native/native_renderer.h"
#include "online_overlay.h"
#include "pad_icons.h"
#include "pad_map.h"
#include "pad_types.h"
#include "touch_controls.h"

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

struct Resolution {
  int w, h;
  std::string label;
};
// Window sizes (the render scale follows the window: native_renderer.cpp):
// the ones the screen reports to Windows (display_modes.h - 5K, 8K and
// ultrawide sizes where the screen has them), else this fixed list.
constexpr int kFixedResolutions[][2] = {{854, 480}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
std::vector<Resolution> g_resolutions;
// GRAPHICS -> RENDER RESOLUTION: native_max_scale (AUTO: follow the window,
// up to 4x; the phone's screen is the window), and below the Xbox 360's 720p
// native_render_scale (360P / 480P: the scene smaller, scaled up); between
// the 720p steps native_scene_height (900P / 1080P: 2x, scaled down to it).
struct Scale {
  int value;
  double res;  // (native_render_scale)
  int height;  // (native_scene_height; 0: off)
  const char* label;
};
// (AUTO: up to 8x - 4320 lines, an 8K screen; saves from before it say 4,
// which the renderer reads as AUTO too. 2880P / 4320P: 4x / 6x exactly.)
constexpr Scale kScales[] = {{8, 1.0, 0, "AUTO"},       {1, 0.5, 0, "360P"},     {1, 2.0 / 3.0, 0, "480P"},
                             {1, 1.0, 0, "720P (XBOX 360)"}, {2, 1.0, 900, "900P"}, {2, 1.0, 1080, "1080P"},
                             {2, 1.0, 0, "1440P"},       {3, 1.0, 0, "2160P"},  {4, 1.0, 2880, "2880P"},
                             {6, 1.0, 4320, "4320P (8K)"}};
constexpr int kNumScales = int(std::size(kScales));
// GRAPHICS -> TEXTURES: native_texture_quality (MEDIUM / LOW: big textures
// from the game's own half / quarter size mipmaps - native/textures.h).
constexpr const char* kTextureQualities[] = {"high", "medium", "low"};
constexpr const char* kTextureQualityLabels[] = {"HIGH", "MEDIUM", "LOW"};
constexpr int kNumTextureQualities = 3;

// The game's languages (its text; the voices are English): Xbox 360 language
// ids (user_language, read when the game starts).
struct Language {
  uint32_t id;
  const char* label;
};
constexpr Language kLanguages[] = {
    {1, "ENGLISH"}, {4, "FRAN\xC3\x87" "AIS"}, {3, "DEUTSCH"}, {5, "ESPA\xC3\x91OL"}, {6, "ITALIANO"}};
constexpr int kNumLanguages = int(std::size(kLanguages));

// The rows, on two tabs (LB / RB).
enum Row {
  kResolution, kDisplay, kVsync, kFpsCounter, kRenderer,         // DISPLAY
  kRenderScale, kAntiAliasing, kEffects, kCutsceneFps,           // QUALITY
  kTouch, kLanguage,                                             // DISPLAY (last)
  kWide, kPrepare, kDof, kMotionBlur, kSoft, kReplays,                      // QUALITY (last)
  kFrameRate, kFullSpeed,                                        // DISPLAY
  kCpuPriority,                                                  // DISPLAY (PC, last)
  kTextureQuality,                                               // GRAPHICS
  kPreset, kCrowd, kShadows,                                     // ADV. GRAPHICS
  kFastStart,                                                    // DISPLAY (last)
  kPing,                                                         // DISPLAY
  kRouterPort,                                                   // DISPLAY
  kArenaBrightness,                                              // GRAPHICS
  kOverlayCorner,                                                // DISPLAY (after PERFORMANCE)
  kUpscaling,                                                    // GRAPHICS (after ANTI-ALIASING)
  kCawLimits,                                                    // DISPLAY (last: caw_limits.cpp)
  kMatchCamera,                                                  // DISPLAY (free_cam.h)
  kMatFix,                                                       // GRAPHICS (after ARENA BRIGHTNESS)
  kVsStyle,                                                      // GRAPHICS (after RING MAT: vs_styles.h)
  kCpuPreset, kCpuReversal, kCpuRates, kCpuDamage, kCpuPin, kCpuMomentum,  // CPU (cpu_difficulty.h)
};
// FRAME RATE (frame_rate.h): the choices.
constexpr int kFrameRates[] = {30, 60};

// ADV. GRAPHICS -> CROWD DENSITY.
constexpr const char* kLevelNames[] = {"LOW", "MED", "HIGH"};
constexpr const char* kCrowdValues[] = {"off", "medium", "on"};

// ADV. GRAPHICS -> PRESET: sets these rows, all in one go. The page shows the one that matches them, else CUSTOM.
// Which device: only the presets that fit it are offered.
enum PresetDevice { kPresetAndroid, kPresetSwitch, kPresetDeck, kPresetPc };
struct Preset {
  const char* name;
  PresetDevice device;
  int crowd;           // (0 LOW, 1 MED, 2 HIGH)
  bool shadows;
  bool effects;        // (EFFECT DETAIL HIGH)
  int textures;        // (kTextureQualities)
  int scale;           // (kScales)
  int aa;              // (1 off, 2-4)
  int frame_rate;      // (kFrameRates: 0 for 30, 1 for 60)
};
constexpr int kScale360 = 1, kScale720 = 3, kScale1080 = 5, kScale1440 = 6;
constexpr Preset kPresets[] = {
    {"MALI", kPresetAndroid, 0, false, false, 2, kScale360, 1, 0},
    {"SNAPDRAGON", kPresetAndroid, 2, true, false, 0, kScale720, 1, 1},
    {"SWITCH", kPresetSwitch, 2, true, false, 2, kScale720, 1, 0},
    {"STEAM DECK", kPresetDeck, 2, true, false, 0, kScale720, 1, 1},
    {"LOW END PC", kPresetPc, 1, true, false, 1, kScale720, 1, 1},
    {"MEDIUM END PC", kPresetPc, 2, true, false, 0, kScale1080, 1, 1},
    {"HIGH END PC", kPresetPc, 2, true, true, 0, kScale1440, 2, 1},
};
bool PresetFits(const Preset& p) {
#if defined(__SWITCH__)
  return p.device == kPresetSwitch;
#elif defined(__ANDROID__)
  return p.device == kPresetAndroid;
#else
  return p.device == kPresetPc || (p.device == kPresetDeck && svr2011::IsSteamDeck());
#endif
}
constexpr int kNumFrameRates = 2;
enum Tab { kDisplayTab, kGraphicsTab, kAdvancedTab, kCpuTab, kTabs };
const char* kTabNames[kTabs] = {"DISPLAY", "GRAPHICS", "ADV. GRAPHICS", "CPU"};
#if defined(__ANDROID__)
// (the phone: the window is the screen - no window size or mode)
// (no RENDERER row: the phone draws natively on Vulkan, its only graphics API)
const std::vector<Row> kTabRowsAll[kTabs] = {{kFrameRate, kFullSpeed, kVsync, kFpsCounter, kOverlayCorner, kMatchCamera, kPing, kRouterPort, kTouch, kReplays, kFastStart, kCawLimits},
#else
const std::vector<Row> kTabRowsAll[kTabs] = {{kResolution, kDisplay, kFrameRate, kFullSpeed, kVsync, kFpsCounter, kOverlayCorner, kMatchCamera, kPing, kRouterPort, kRenderer, kTouch,
                                              kReplays, kCpuPriority, kFastStart, kCawLimits},
#endif
                                             {kRenderScale, kAntiAliasing, kUpscaling, kTextureQuality, kArenaBrightness, kMatFix, kVsStyle, kWide, kCutsceneFps},
                                             {kPreset, kCrowd, kShadows, kEffects, kDof, kMotionBlur, kSoft, kPrepare},
                                             {kCpuPreset, kCpuReversal, kCpuRates, kCpuDamage, kCpuPin, kCpuMomentum}};
// SHADOWS: only with the renderer's native_shadows setting.
bool HasShadows() {
  static const bool has = !rex::cvar::GetFlagByName("native_shadows").empty();
  return has;
}
const std::vector<Row>& TabRows(int tab) {
  static const std::array<std::vector<Row>, kTabs> rows = [] {
    std::array<std::vector<Row>, kTabs> r;
    for (int t = 0; t < kTabs; ++t)
      for (Row id : kTabRowsAll[t])
        if ((id != kShadows || HasShadows()) && (id != kVsStyle || !svr2011::VsStyles().empty()))
          r[t].push_back(id);
    return r;
  }();
  return rows[tab];
}

// CONTROLS: each controller input, what it does in a match, its keyboard
// keys (the SDK keyboard driver's setting: a comma-separated list, each key
// with optional Shift+ / Ctrl+ / Alt+) and the driver's default.
struct Binding {
  const char* cvar;
  const char* input;
  const char* action;
  const char* keys;  // (default)
};
constexpr Binding kBindings[] = {
    {"keybind_a", "A", "ACTION", "Semicolon,Space"},
    {"keybind_b", "B", "IRISH WHIP / PIN", "Quote,Backspace"},
    {"keybind_x", "X", "STRIKE", "L"},
    {"keybind_y", "Y", "FINISHER / SIGNATURE", "P"},
    {"keybind_left_shoulder", "LB", "RUN", "1"},
    {"keybind_right_shoulder", "RB", "TURN", "3"},
    {"keybind_left_trigger", "LT", "DRAG", "Q,I"},
    {"keybind_right_trigger", "RT", "REVERSE", "E,O"},
    {"keybind_lstick_up", "LEFT STICK UP", "MOVE", "W"},
    {"keybind_lstick_down", "LEFT STICK DOWN", "MOVE", "S"},
    {"keybind_lstick_left", "LEFT STICK LEFT", "MOVE", "A"},
    {"keybind_lstick_right", "LEFT STICK RIGHT", "MOVE", "D"},
    {"keybind_lstick_press", "L3", "TARGET", "F"},
    {"keybind_rstick_up", "RIGHT STICK UP", "GRAPPLE", "Up"},
    {"keybind_rstick_down", "RIGHT STICK DOWN", "GRAPPLE", "Down"},
    {"keybind_rstick_left", "RIGHT STICK LEFT", "GRAPPLE", "Left"},
    {"keybind_rstick_right", "RIGHT STICK RIGHT", "GRAPPLE", "Right"},
    {"keybind_rstick_press", "R3", "SUBMIT", "K"},
    {"keybind_dpad_up", "D-PAD UP", "TAUNT", "Shift+Up"},
    {"keybind_dpad_down", "D-PAD DOWN", "TAUNT", "Shift+Down"},
    {"keybind_dpad_left", "D-PAD LEFT", "TAUNT", "Shift+Left"},
    {"keybind_dpad_right", "D-PAD RIGHT", "TAUNT", "Shift+Right"},
    {"keybind_start", "START", "PAUSE", "X,Return"},
    {"keybind_back", "BACK", "BACK", "Z,Tab"},
};
constexpr int kNumBindings = int(std::size(kBindings));
// The tabs (LB / RB), each with a RESET TO DEFAULTS last:
// KEYBOARD: the inputs' keys.
// MOUSE: MOUSE LOOK (the right stick), MOUSE SPEED, the inputs' mouse buttons (in the same keybind_* settings).
// CONTROLLER: PLAYER (whose layout), VIBRATION (the vibration setting: pad_types.cpp), the buttons (pad_map.h).
enum ControlTab { kKeysTab, kMouseTab, kPadTab, kControlTabs };
const char* kControlTabNames[kControlTabs] = {"KEYBOARD", "MOUSE", "CONTROLLER"};
#if defined(__ANDROID__)
constexpr int kFirstControlTab = kPadTab;  // (the phone: its on-screen controller has its own editor)
#else
constexpr int kFirstControlTab = kKeysTab;
#endif
constexpr int kMouseLookRow = 0, kMouseSpeedRow = 1;
constexpr int kPadPlayerRow = 0, kVibrationRow = 1;
int FirstInputRow(int tab) { return tab == kKeysTab ? 0 : 2; }
int InputRows(int tab) { return tab == kPadTab ? kPadInputs : kNumBindings; }
int ControlRows(int tab) { return FirstInputRow(tab) + InputRows(tab) + 1; }
constexpr int kControlsShown = 9;              // (rows on screen; the list scrolls)

// A key list as its names; a mouse button is a key of the list.
std::vector<std::string> SplitKeys(const std::string& keys) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= keys.size()) {
    const size_t comma = std::min(keys.find(',', at), keys.size());
    std::string k = keys.substr(at, comma - at);
    while (!k.empty() && k.back() == ' ') k.pop_back();
    while (!k.empty() && k.front() == ' ') k.erase(0, 1);
    if (!k.empty()) out.push_back(k);
    at = comma + 1;
  }
  return out;
}
std::string JoinKeys(const std::vector<std::string>& keys) {
  std::string out;
  for (const std::string& k : keys) out += (out.empty() ? "" : ",") + k;
  return out;
}
bool IsMouseKey(const std::string& k) { return k == "LMB" || k == "RMB" || k == "MMB"; }
// The keyboard's or the mouse's part of a list.
std::vector<std::string> DeviceKeys(const std::string& keys, bool mouse) {
  std::vector<std::string> out;
  for (const std::string& k : SplitKeys(keys))
    if (IsMouseKey(k) == mouse) out.push_back(k);
  return out;
}
// The inputs the game's menus need: they always keep a keyboard key, so a keyboard player can always reach this page
// and leave a menu. (The page's own Enter, Esc and arrows, and a controller's buttons in menus, can't be changed.)
bool NeedsKey(int binding) {
  const std::string c = kBindings[binding].cvar;
  return c == "keybind_a" || c == "keybind_b" || c == "keybind_start" || c == "keybind_lstick_up" ||
         c == "keybind_lstick_down" || c == "keybind_lstick_left" || c == "keybind_lstick_right";
}
// A physical input's name on a player's controller.
const char* PadInputLabel(int input, bool playstation) {
  static const char* const kPs[kPadInputs] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "L2", "R2",
                                              "L3", "R3", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
                                              "START", "SELECT"};
  return playstation ? kPs[input] : PadInput(input).label;
}

// The keys a binding can take: ImGui's key -> the driver's name for it.
struct KeyName {
  ImGuiKey key;
  const char* name;
};
const std::vector<KeyName>& KeyNames() {
  static const std::vector<KeyName> names = [] {
    std::vector<KeyName> v;
    static const char* const kLetters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                             "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    for (int i = 0; i < 26; ++i) v.push_back({ImGuiKey(ImGuiKey_A + i), kLetters[i]});
    static const char* const kDigits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    for (int i = 0; i < 10; ++i) v.push_back({ImGuiKey(ImGuiKey_0 + i), kDigits[i]});
    static const char* const kF[12] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    for (int i = 0; i < 12; ++i) v.push_back({ImGuiKey(ImGuiKey_F1 + i), kF[i]});
    static const char* const kPad[10] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
                                         "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"};
    for (int i = 0; i < 10; ++i) v.push_back({ImGuiKey(ImGuiKey_Keypad0 + i), kPad[i]});
    const KeyName rest[] = {
        {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_LeftArrow, "Left"},
        {ImGuiKey_RightArrow, "Right"}, {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Return"},
        {ImGuiKey_KeypadEnter, "Return"}, {ImGuiKey_Tab, "Tab"}, {ImGuiKey_Backspace, "Backspace"},
        {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
        {ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Apostrophe, "Quote"},
        {ImGuiKey_Semicolon, "Semicolon"}, {ImGuiKey_Comma, "Comma"}, {ImGuiKey_Period, "Period"},
        {ImGuiKey_Slash, "Slash"}, {ImGuiKey_Backslash, "Backslash"}, {ImGuiKey_LeftBracket, "LBracket"},
        {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_Minus, "Minus"}, {ImGuiKey_Equal, "Plus"},
        {ImGuiKey_GraveAccent, "Backtick"}, {ImGuiKey_KeypadAdd, "NumpadPlus"},
        {ImGuiKey_KeypadSubtract, "NumpadMinus"}, {ImGuiKey_KeypadMultiply, "NumpadStar"},
        {ImGuiKey_KeypadDivide, "NumpadSlash"}, {ImGuiKey_CapsLock, "CapsLock"},
    };
    v.insert(v.end(), std::begin(rest), std::end(rest));
    return v;
  }();
  return names;
}

// "Semicolon,Space" -> "SEMICOLON, SPACE"; none -> "NONE".
std::string ShowKeys(const std::string& keys) {
  if (keys.empty()) return "NONE";
  std::string out;
  for (char c : keys) {
    if (c == ',') out += ", ";
    else out += char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}
// MY WWE -> OPTIONS -> LANGUAGE: the same page with only this row.
const std::vector<Row> kLanguageRows = {kLanguage};
// The page's three uses (MY WWE -> OPTIONS -> GRAPHICS / CONTROLS / LANGUAGE).
enum Mode { kGraphicsMode, kControlsMode, kLanguageMode };

// DISPLAY MODE: a window, a borderless window covering the screen, or the
// screen itself (exclusive fullscreen: fullscreen_exclusive in the SDK).
enum DisplayMode { kWindowed, kBorderless, kExclusive, kDisplayModes };
const char* kDisplayModeNames[kDisplayModes] = {"WINDOWED", "BORDERLESS", "FULL SCREEN"};

std::filesystem::path g_config_path;
rex::ui::Window* g_window = nullptr;
rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<int> g_requested_mode{kGraphicsMode};  // (opened as GRAPHICS, CONTROLS or LANGUAGE)
std::atomic<bool> g_open{false};
// After closing, the game keeps seeing an idle pad until every button is
// released, so the button that closed the page does not reach the menu.
std::atomic<bool> g_wait_release{false};

// Sets `key = value` among the settings file's top-level keys (before any
// [section]), keeping everything else (the launcher's format).
void SaveSetting(const std::string& key, const std::string& value) {
  std::vector<std::string> lines;
  {
    std::ifstream in(g_config_path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string entry = key + " = " + value;
  size_t insert_at = lines.size();
  bool done = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    const size_t first = l.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (l[first] == '[') {
      insert_at = std::min(insert_at, i);
      break;
    }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || l[first] == '#') continue;
    std::string k = l.substr(first, eq - first);
    k.erase(k.find_last_not_of(" \t") + 1);
    if (k == key) {
      lines[i] = entry;
      done = true;
      break;
    }
  }
  if (!done) lines.insert(lines.begin() + std::ptrdiff_t(insert_at), entry);
  const std::filesystem::path tmp = g_config_path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
    if (!out) {
      REXLOG_WARN("GRAPHICS: could not save {}", g_config_path.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, g_config_path, ec);
  if (ec) REXLOG_WARN("GRAPHICS: could not save {}: {}", g_config_path.string(), ec.message());
}

// The window's display mode now (fullscreen_exclusive: the SDK's window).
int CurrentDisplayMode() {
  if (!g_window || !g_window->IsFullscreen()) return kWindowed;
  return rex::cvar::Query<bool>("fullscreen_exclusive") ? kExclusive : kBorderless;
}

// Switches the window to `mode` and saves it (fullscreen, fullscreen_exclusive).
void SetDisplayMode(int mode) {
  const bool exclusive = mode == kExclusive;
  const bool changed_kind = exclusive != rex::cvar::Query<bool>("fullscreen_exclusive");
  rex::cvar::SetFlagByName("fullscreen_exclusive", exclusive ? "true" : "false");
  if (g_window) {
    // (borderless <-> exclusive: leave and re-enter full screen in the new kind)
    if (changed_kind && g_window->IsFullscreen() && mode != kWindowed) g_window->SetFullscreen(false);
    g_window->SetFullscreen(mode != kWindowed);
  }
  SaveSetting("fullscreen", mode != kWindowed ? "true" : "false");
  SaveSetting("fullscreen_exclusive", exclusive ? "true" : "false");
}

#if defined(_WIN32)
// The game's top-level window (this process's, titled like the game).
HWND GameWindow() {
  struct Find {
    DWORD pid;
    HWND found;
  } find{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND h, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t title[128];
        if (pid == f->pid && IsWindowVisible(h) && GetWindowTextW(h, title, 128) &&
            wcsstr(title, L"SmackDown")) {
          f->found = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&find));
  return find.found;
}

// Gives the (windowed) game window a client area of w x h, centred on its
// monitor.
void ResizeWindow(int w, int h) {
  if (!g_window) return;
  g_window->app_context().CallInUIThread([w, h] {
    HWND hwnd = GameWindow();
    if (!hwnd || (g_window && g_window->IsFullscreen())) return;
    RECT rc = {0, 0, w, h};
    const DWORD style = DWORD(GetWindowLongW(hwnd, GWL_STYLE));
    const DWORD ex = DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    const int x = mi.rcWork.left + std::max(0, int(mi.rcWork.right - mi.rcWork.left - ww) / 2);
    const int y = mi.rcWork.top + std::max(0, int(mi.rcWork.bottom - mi.rcWork.top - wh) / 2);
    SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
  });
}
#else
// (Android: the window is the screen.)
void ResizeWindow(int, int) {}
#endif

class GraphicsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit GraphicsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Load();
  void Change(int row, int dir);
  void Hide();  // (not ImGuiDialog::Close, which deletes the page)
  uint16_t PadButtons();
  // CONTROLS: navigate / rebind (true when it used the frame's input).
  void ControlsInput(uint16_t pressed, uint16_t act, bool& back);
  void SetBinding(int i, const std::string& keys);
  // A key (or mouse button) for an input: it leaves any other input, unless that would leave one the menus need
  // without a key. `add`: next to the input's keys, else in place of the keyboard's / the mouse's part of them.
  void AssignKey(int binding, const std::string& name, bool add);
  void SetPadLayout(uint32_t player, uint64_t layout);
  void SetNotice(const std::string& text);
  void ResetControlTab();
  void StartCapture(int input, bool add);

  int tab_ = kDisplayTab;
  int mode_ = kGraphicsMode;  // (Mode)
  int row_ = 0;  // (in the tab)
  int resolution_ = 0;
  int scale_ = 0;  // (kScales)
  int textures_ = 0;  // (kTextureQualities)
  int brightness_ = 100;  // ARENA BRIGHTNESS, percent (arena_lights.h)
  bool touch_ = false;  // the on-screen controller
  bool wide_ = true;    // matches as wide as the screen
  bool dof_ = true, blur_ = true, soft_ = false, replays_ = true;  // depth of field, motion blur (post_effects.cpp)
  bool caw_no_limits_ = true;  // CAW LIMITS: OFF (the port's) / STOCK (caw_limits.cpp)
  int fast_start_ = 0;  // FAST START (fast_start.h): bit 0 skip_intros, bit 1 skip_training
  bool prepare_ = true;  // the known pipelines built ahead in the menus
  int crowd_ = 2, crowd_at_start_ = 2;  // arena_crowd: 0 LOW, 1 MED, 2 HIGH (read when the game starts)
  bool shadows_ = true;  // native_shadows
  const Preset* CurrentPreset() const;
  void ApplyPreset(const Preset& p);
  bool mat_fix_ = true;  // native_mat_fix
  int vs_style_ = 0;      // vs_style: 0 2011's, k the k-th installed style (vs_styles.h)
  int language_ = 0;    // kLanguages index (saved; applies at the next start)
  int language_at_start_ = 0;
  int aa_ = 1;  // anti-aliasing level: 1 off, 2-4 supersampling per side
  // UPSCALING: 0 off, 1-4 FSR 1, 5-8 FSR 2 (experimental: Direct3D 12 / Vulkan), 9-12 DLSS (experimental: NVIDIA
  // RTX, Direct3D 12) - native_upscale_quality 0-3 (ultra quality .. performance)
  int upscale_ = 0;
  bool ping_ = true;
  bool router_port_ = true;
  svr2011::CpuTuning cpu_;  // (the CPU tab)
  int perf_ = 1, corner_ = 0;  // (PERFORMANCE: 0 off, 1 the frame rate, 2 everything; its corner)
  bool vsync_ = true, native_ = true, full_speed_ = true;
  bool high_priority_ = false;  // (process_priority 1: above normal, perf_hooks.cpp)
  int display_ = kWindowed;  // (DisplayMode)
  bool effects_ = true, fps60_ = true;
  int frame_rate_ = 1;  // (kFrameRates)
  bool native_at_start_ = false;
  // The graphics API (gpu_backend): 0 any (Direct3D 12, else Vulkan, else
  // Direct3D 11), 1 vulkan, 2 d3d11 - picked when the game starts, so a change
  // takes effect at the next start.
  int api_ = 0, api_at_start_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  // CONTROLS: the row, the first row shown, and a key being waited for
  // (capture_: the binding; capture_add_: added to its keys, else replaces).
  int control_row_ = 0, control_top_ = 0;
  int capture_ = -1;
  int capture_mod_ = -1;  // (a Shift / Ctrl / Alt held while waiting: kMods index)
  int control_tab_ = kFirstControlTab;  // (ControlTab)
  int pad_player_ = 0;                  // (the CONTROLLER tab's player)
  bool capture_armed_ = false;          // (a controller button: everything was released first)
  std::string notice_;                  // (what the last change did, under the list)
  Clock::time_point notice_until_{};
  bool capture_add_ = false;
  Clock::time_point capture_until_{};
  Clock::time_point repeat_at_{};
  Clock::time_point opened_at_{};
};

void GraphicsPage::Load() {
  const int w = rex::cvar::Query<int32_t>("window_width");
  const int h = rex::cvar::Query<int32_t>("window_height");
  {
    std::vector<svr2011::DisplaySize> sizes;
#if defined(_WIN32)
    sizes = svr2011::DisplaySizes(GameWindow());
#endif
    const bool fixed = sizes.empty();
    if (fixed)
      for (const auto& f : kFixedResolutions) sizes.push_back({f[0], f[1]});
    // (the saved size, when the list doesn't have it: kept as a choice)
    if (w >= 640 && h >= 360 &&
        std::none_of(sizes.begin(), sizes.end(), [&](const svr2011::DisplaySize& d) { return d.w == w && d.h == h; }))
      sizes.push_back({w, h});
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
      return int64_t(a.w) * a.h != int64_t(b.w) * b.h ? int64_t(a.w) * a.h < int64_t(b.w) * b.h : a.w < b.w;
    });
    g_resolutions.clear();
    for (const auto& d : sizes) g_resolutions.push_back({d.w, d.h, std::to_string(d.w) + " X " + std::to_string(d.h)});
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::string list;
      for (const auto& r : g_resolutions) list += (list.empty() ? "" : ", ") + r.label;
      REXLOG_INFO("[svr2011] graphics: window sizes {}: {}", fixed ? "(fixed list)" : "(the screen's)", list);
    }
  }
  resolution_ = 0;
  for (int i = 0; i < int(g_resolutions.size()); ++i) {
    if (g_resolutions[i].w == 1920 && g_resolutions[i].h == 1080) resolution_ = i;  // (the default)
  }
  for (int i = 0; i < int(g_resolutions.size()); ++i) {
    if (g_resolutions[i].w == w && g_resolutions[i].h == h) resolution_ = i;
  }
  {
    const int32_t level = rex::cvar::Query<int32_t>("native_aa");
    aa_ = level > 0 ? std::clamp<int32_t>(level, 1, 4) : (rex::cvar::Query<bool>("native_2x_msaa") ? 2 : 1);
  }
  {
    const std::string kind = rex::cvar::GetFlagByName("native_upscale");
    const int quality = std::clamp<int32_t>(rex::cvar::Query<int32_t>("native_upscale_quality"), 0, 3);
    upscale_ = kind == "fsr1" ? quality + 1 : kind == "fsr2" ? quality + 5 : kind == "dlss" ? quality + 9 : 0;
  }
  {
    const int32_t v = rex::cvar::Query<int32_t>("native_max_scale");
    const double res = rex::cvar::Query<double>("native_render_scale");
    const int32_t height = rex::cvar::Query<int32_t>("native_scene_height");
    scale_ = 0;
    if (height > 0) {  // (the nearest of 900P / 1080P)
      for (int i = 0; i < kNumScales; ++i)
        if (kScales[i].height > 0 &&
            (kScales[scale_].height == 0 || std::abs(kScales[i].height - height) < std::abs(kScales[scale_].height - height)))
          scale_ = i;
    } else if (res > 0.0 && res < 0.99) {  // (below 720p: the nearer of 360P / 480P)
      for (int i = 0; i < kNumScales; ++i)
        if (kScales[i].res < 0.99 && std::abs(kScales[i].res - res) < std::abs(kScales[scale_].res - res)) scale_ = i;
    } else {
      for (int i = 0; i < kNumScales; ++i)
        if (kScales[i].value == v && kScales[i].res >= 0.99 && kScales[i].height == 0) scale_ = i;
    }
  }
  {
    const std::string q = rex::cvar::Query<std::string>("native_texture_quality");
    textures_ = 0;
    for (int i = 0; i < kNumTextureQualities; ++i)
      if (q == kTextureQualities[i]) textures_ = i;
  }
  effects_ = rex::cvar::Query<bool>("native_scale_effects");
  brightness_ = ArenaBrightness();
  fps60_ = rex::cvar::Query<bool>("unlock_30fps");
  native_at_start_ = native::CanSwitch();
  {
    const std::string api = rex::cvar::Query<std::string>("gpu_backend");
    api_at_start_ = api == "vulkan" ? 1 : api == "d3d11" ? 2 : 0;
  }
  api_ = api_at_start_;
  native_ = true;  // (the game draws only natively)
  perf_ = PerfOverlayLevel();
  corner_ = PerfOverlayCorner();
  ping_ = rex::cvar::Query<bool>("show_ping");
  router_port_ = rex::cvar::Query<bool>("p2p_port_mapping");
  cpu_ = svr2011::LocalCpuTuning();
  touch_ = rex::cvar::Query<bool>("touch_controls");
  wide_ = rex::cvar::Query<bool>("native_widescreen");
  dof_ = rex::cvar::Query<bool>("depth_of_field");
  blur_ = rex::cvar::Query<bool>("motion_blur");
  soft_ = rex::cvar::Query<bool>("soft_filter");
  replays_ = rex::cvar::Query<bool>("replays");
  caw_no_limits_ = rex::cvar::Query<bool>("caw_no_limits");
  fast_start_ = (rex::cvar::Query<bool>("skip_intros") ? 1 : 0) | (rex::cvar::Query<bool>("skip_training") ? 2 : 0);
  prepare_ = rex::cvar::Query<bool>("native_prepare_pipelines");
  {
    const std::string c = rex::cvar::Query<std::string>("arena_crowd");
    crowd_ = c == "on"       ? 2
             : c == "medium" ? 1
             : c == "off"    ? 0
                             : svr2011::crowd::Level();  // (auto: what this run chose at the start)
    static const bool at_start = crowd_;
    crowd_at_start_ = at_start;
  }
  shadows_ = !HasShadows() || rex::cvar::Query<bool>("native_shadows");
  mat_fix_ = rex::cvar::Query<bool>("native_mat_fix");
  {
    const std::string v = rex::cvar::Query<std::string>("vs_style");
    const auto& styles = svr2011::VsStyles();
    vs_style_ = 0;
    for (size_t i = 0; i < styles.size(); ++i)
      if (styles[i].first == v) vs_style_ = int(i) + 1;
  }
  {
    const uint32_t id = rex::cvar::Query<uint32_t>("user_language");
    language_ = 0;
    for (int i = 0; i < kNumLanguages; ++i)
      if (kLanguages[i].id == id) language_ = i;
    static const int at_start = language_;
    language_at_start_ = at_start;
  }
  vsync_ = rex::cvar::Query<bool>("vsync");
  full_speed_ = rex::cvar::Query<bool>("full_speed");
  high_priority_ = rex::cvar::Query<int32_t>("process_priority") >= 1;
  display_ = CurrentDisplayMode();
  frame_rate_ = 1;
  for (int i = 0; i < kNumFrameRates; ++i)
    if (kFrameRates[i] == TargetFrameRate()) frame_rate_ = i;
}

const Preset* GraphicsPage::CurrentPreset() const {
  for (const Preset& p : kPresets) {
    if (PresetFits(p) && crowd_ == p.crowd && shadows_ == p.shadows && effects_ == p.effects && textures_ == p.textures &&
        scale_ == p.scale && aa_ == p.aa && frame_rate_ == p.frame_rate && upscale_ == 0)
      return &p;
  }
  return nullptr;
}

// Each row through Change, as if the player had moved it: the same saving and the same cvars.
void GraphicsPage::ApplyPreset(const Preset& p) {
  const auto to = [this](Row row, const auto& value, const auto& want) {
    for (int i = 0; i < 32 && value() != want; ++i) Change(row, 1);
  };
  to(kUpscaling, [&] { return upscale_; }, 0);
  to(kAntiAliasing, [&] { return aa_; }, p.aa);
  to(kRenderScale, [&] { return scale_; }, p.scale);
  to(kTextureQuality, [&] { return textures_; }, p.textures);
  to(kEffects, [&] { return effects_; }, p.effects);
  to(kFrameRate, [&] { return frame_rate_; }, p.frame_rate);
  to(kCrowd, [&] { return crowd_; }, p.crowd);
  if (HasShadows()) to(kShadows, [&] { return shadows_; }, p.shadows);
}

void GraphicsPage::Change(int row, int dir) {
  switch (row) {
    case kRenderScale: {
      scale_ = (scale_ + dir + kNumScales) % kNumScales;
      char res[16];
      std::snprintf(res, sizeof(res), "%.4f", kScales[scale_].res);
      rex::cvar::SetFlagByName("native_max_scale", std::to_string(kScales[scale_].value));
      rex::cvar::SetFlagByName("native_render_scale", res);
      SaveSetting("native_max_scale", std::to_string(kScales[scale_].value));
      SaveSetting("native_render_scale", res);
      rex::cvar::SetFlagByName("native_scene_height", std::to_string(kScales[scale_].height));
      SaveSetting("native_scene_height", std::to_string(kScales[scale_].height));
      break;
    }
    case kCrowd:  // (the arenas' crowd-less copies are chosen when the game starts: crowd_off.h)
      crowd_ = (crowd_ + dir + 3) % 3;
      rex::cvar::SetFlagByName("arena_crowd", kCrowdValues[crowd_]);
      SaveSetting("arena_crowd", std::string("\"") + kCrowdValues[crowd_] + "\"");
      break;
    case kPreset: {
      const int n = int(std::size(kPresets));
      int at = -1;  // (CUSTOM: the first one next)
      for (int i = 0; i < n; ++i)
        if (&kPresets[i] == CurrentPreset()) at = i;
      if (at < 0 && dir < 0) at = 0;
      for (int step = 0; step < n; ++step) {
        at = (at + dir + n) % n;
        if (PresetFits(kPresets[at])) break;
      }
      if (PresetFits(kPresets[at])) ApplyPreset(kPresets[at]);
      break;
    }
    case kShadows:
      shadows_ = !shadows_;
      rex::cvar::SetFlagByName("native_shadows", shadows_ ? "true" : "false");
      SaveSetting("native_shadows", shadows_ ? "true" : "false");
      break;
    case kMatFix:
      mat_fix_ = !mat_fix_;
      rex::cvar::SetFlagByName("native_mat_fix", mat_fix_ ? "true" : "false");
      SaveSetting("native_mat_fix", mat_fix_ ? "true" : "false");
      break;
    case kVsStyle: {
      const auto& styles = svr2011::VsStyles();
      const int n = int(styles.size()) + 1;
      vs_style_ = (vs_style_ + (dir ? dir : 1) + n) % n;
      const std::string v = vs_style_ ? styles[size_t(vs_style_ - 1)].first : "";
      rex::cvar::SetFlagByName("vs_style", v);
      SaveSetting("vs_style", "\"" + v + "\"");
      break;
    }
    case kArenaBrightness:
      brightness_ += 10 * (dir ? dir : 1);  // (50-200%, round as the other rows)
      if (brightness_ > 200) brightness_ = 50;
      if (brightness_ < 50) brightness_ = 200;
      SetArenaBrightness(brightness_);
      SaveSetting("arena_brightness", std::to_string(brightness_));
      break;
    case kTextureQuality:
      textures_ = (textures_ + dir + kNumTextureQualities) % kNumTextureQualities;
      rex::cvar::SetFlagByName("native_texture_quality", kTextureQualities[textures_]);
      SaveSetting("native_texture_quality", std::string("\"") + kTextureQualities[textures_] + "\"");
      break;
    case kEffects:
      effects_ = !effects_;
      rex::cvar::SetFlagByName("native_scale_effects", effects_ ? "true" : "false");
      SaveSetting("native_scale_effects", effects_ ? "true" : "false");
      break;
    case kCutsceneFps:
      fps60_ = !fps60_;
      rex::cvar::SetFlagByName("unlock_30fps", fps60_ ? "true" : "false");
      SaveSetting("unlock_30fps", fps60_ ? "true" : "false");
      break;
    case kResolution: {
      const int n = std::max(1, int(g_resolutions.size()));
      resolution_ = (resolution_ + dir + n) % n;
      if (g_resolutions.empty()) break;
      const Resolution& r = g_resolutions[resolution_];
      ResizeWindow(r.w, r.h);
      SaveSetting("window_width", std::to_string(r.w));
      SaveSetting("window_height", std::to_string(r.h));
      // (the emulated renderer's scale, fixed at start)
      SaveSetting("resolution_scale", std::to_string(std::clamp((r.h + 719) / 720, 1, 3)));
      break;
    }
    case kAntiAliasing:
      aa_ = (aa_ - 1 + dir + 4) % 4 + 1;  // OFF, 2X, 3X, 4X
      rex::cvar::SetFlagByName("native_aa", std::to_string(aa_));
      rex::cvar::SetFlagByName("native_2x_msaa", aa_ >= 2 ? "true" : "false");
      SaveSetting("native_aa", std::to_string(aa_));
      SaveSetting("native_2x_msaa", aa_ >= 2 ? "true" : "false");
      break;
    case kUpscaling: {
      // OFF, FSR 1 ULTRA QUALITY .. PERFORMANCE, then FSR 2 and DLSS where they can run (the log says why not)
      std::vector<int> choices;
      for (int i = 0; i <= 4; ++i) choices.push_back(i);
      if (native::UpscalerAvailable("fsr2"))
        for (int i = 5; i <= 8; ++i) choices.push_back(i);
      if (native::UpscalerAvailable("dlss"))
        for (int i = 9; i <= 12; ++i) choices.push_back(i);
      int at = 0;
      for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i] <= upscale_) at = int(i);
      const int count = int(choices.size());
      upscale_ = choices[size_t((at + dir + count) % count)];
      const char* kind = upscale_ == 0 ? "off" : upscale_ <= 4 ? "fsr1" : upscale_ <= 8 ? "fsr2" : "dlss";
      rex::cvar::SetFlagByName("native_upscale", kind);
      // (a string: quoted - a bare word made the whole file unreadable, every setting back at its default)
      SaveSetting("native_upscale", std::string("\"") + kind + "\"");
      if (upscale_) {
        const std::string quality = std::to_string((upscale_ - 1) % 4);
        rex::cvar::SetFlagByName("native_upscale_quality", quality);
        SaveSetting("native_upscale_quality", quality);
      }
      break;
    }
    case kLanguage:
      language_ = (language_ + dir + kNumLanguages) % kNumLanguages;
      SaveSetting("user_language", std::to_string(kLanguages[language_].id));
      break;
    case kDof:
      dof_ = !dof_;
      rex::cvar::SetFlagByName("depth_of_field", dof_ ? "true" : "false");
      SaveSetting("depth_of_field", dof_ ? "true" : "false");
      break;
    case kFastStart:  // (OFF, INTROS, PRACTICE RING, BOTH)
      fast_start_ = (fast_start_ + dir + 4) % 4;
      rex::cvar::SetFlagByName("skip_intros", fast_start_ & 1 ? "true" : "false");
      rex::cvar::SetFlagByName("skip_training", fast_start_ & 2 ? "true" : "false");
      SaveSetting("skip_intros", fast_start_ & 1 ? "true" : "false");
      SaveSetting("skip_training", fast_start_ & 2 ? "true" : "false");
      break;
    case kCawLimits:
      caw_no_limits_ = !caw_no_limits_;
      rex::cvar::SetFlagByName("caw_no_limits", caw_no_limits_ ? "true" : "false");
      SaveSetting("caw_no_limits", caw_no_limits_ ? "true" : "false");
      break;
    case kReplays:
      replays_ = !replays_;
      rex::cvar::SetFlagByName("replays", replays_ ? "true" : "false");
      SaveSetting("replays", replays_ ? "true" : "false");
      break;
    case kSoft:
      soft_ = !soft_;
      rex::cvar::SetFlagByName("soft_filter", soft_ ? "true" : "false");
      SaveSetting("soft_filter", soft_ ? "true" : "false");
      break;
    case kMotionBlur:
      blur_ = !blur_;
      rex::cvar::SetFlagByName("motion_blur", blur_ ? "true" : "false");
      SaveSetting("motion_blur", blur_ ? "true" : "false");
      break;
    case kWide:
      wide_ = !wide_;
      rex::cvar::SetFlagByName("native_widescreen", wide_ ? "true" : "false");
      SaveSetting("native_widescreen", wide_ ? "true" : "false");
      break;
    case kPrepare:
      prepare_ = !prepare_;
      rex::cvar::SetFlagByName("native_prepare_pipelines", prepare_ ? "true" : "false");
      SaveSetting("native_prepare_pipelines", prepare_ ? "true" : "false");
      break;
    case kTouch:
      touch_ = !touch_;
      rex::cvar::SetFlagByName("touch_controls", touch_ ? "true" : "false");
      SaveSetting("touch_controls", touch_ ? "true" : "false");
      break;
    case kPing:
      ping_ = !ping_;
      rex::cvar::SetFlagByName("show_ping", ping_ ? "true" : "false");
      SaveSetting("show_ping", ping_ ? "true" : "false");
      break;
    case kCpuPreset:  // (AUTO <-> LEGEND+; a hand-made set goes to LEGEND+ first)
      cpu_ = cpu_ == svr2011::kLegendPlus ? svr2011::CpuTuning{} : svr2011::kLegendPlus;
      svr2011::SetCpuTuning(cpu_);
      break;
    case kCpuReversal:  // (AUTO, 0-200 % in 10s)
      cpu_.reversal = cpu_.reversal < 0 ? (dir < 0 ? 200 : 0) : cpu_.reversal + 10 * (dir ? dir : 1);
      if (cpu_.reversal > 200 || cpu_.reversal < 0) cpu_.reversal = -1;
      svr2011::SetCpuTuning(cpu_);
      break;
    case kCpuRates:  // (AUTO, 0-10)
      cpu_.rates = cpu_.rates < 0 ? (dir < 0 ? 10 : 0) : cpu_.rates + (dir ? dir : 1);
      if (cpu_.rates > 10 || cpu_.rates < 0) cpu_.rates = -1;
      svr2011::SetCpuTuning(cpu_);
      break;
    case kCpuDamage:
    case kCpuPin:
    case kCpuMomentum: {  // (AUTO, EASY - LEGEND)
      int& v = row == kCpuDamage ? cpu_.damage : row == kCpuPin ? cpu_.pin : cpu_.momentum;
      v = (v + 1 + (dir ? dir : 1) + 5) % 5 - 1;
      svr2011::SetCpuTuning(cpu_);
      break;
    }
    case kRouterPort:
      router_port_ = !router_port_;
      rex::cvar::SetFlagByName("p2p_port_mapping", router_port_ ? "true" : "false");
      SaveSetting("p2p_port_mapping", router_port_ ? "true" : "false");
      if (!router_port_) svr2011::StopPortMapping();  // (off: given back now; on: from the next start)
      break;
    case kFpsCounter:
      perf_ = (perf_ + dir + 3) % 3;
      SetPerfOverlayLevel(perf_);
      SaveSetting("perf_overlay", std::to_string(perf_));
      SaveSetting("show_fps", perf_ ? "true" : "false");  // (older launchers read it)
      break;
    case kMatchCamera:
      SetMatchCamera(MatchCamera() + dir, true);
      break;
    case kOverlayCorner:
      corner_ = (corner_ + dir + 4) % 4;
      SetPerfOverlayCorner(corner_);
      SaveSetting("perf_overlay_corner", std::to_string(corner_));
      break;
    case kFrameRate:
      frame_rate_ = (frame_rate_ + dir + kNumFrameRates) % kNumFrameRates;
      SetTargetFrameRate(kFrameRates[frame_rate_]);
      SaveSetting("frame_rate", std::to_string(kFrameRates[frame_rate_]));
      break;
    case kCpuPriority:  // (applies at once; perf_hooks.cpp sets it at start)
      high_priority_ = !high_priority_;
      rex::cvar::SetFlagByName("process_priority", high_priority_ ? "1" : "0");
      SaveSetting("process_priority", high_priority_ ? "1" : "0");
#if defined(_WIN32)
      SetPriorityClass(GetCurrentProcess(), high_priority_ ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif
      break;
    case kFullSpeed:
      full_speed_ = !full_speed_;
      rex::cvar::SetFlagByName("full_speed", full_speed_ ? "true" : "false");
      SaveSetting("full_speed", full_speed_ ? "true" : "false");
      SetTargetFrameRate(TargetFrameRate());  // (the frame clock: 30 needs it on)
      break;
    case kVsync:
      vsync_ = !vsync_;
      rex::cvar::SetFlagByName("vsync", vsync_ ? "true" : "false");
      SaveSetting("vsync", vsync_ ? "true" : "false");
      break;
    case kDisplay:
      display_ = (display_ + dir + kDisplayModes) % kDisplayModes;
      SetDisplayMode(display_);
      if (display_ == kWindowed && resolution_ < int(g_resolutions.size())) {
        const Resolution& r = g_resolutions[resolution_];
        ResizeWindow(r.w, r.h);
      }
      break;
    case kRenderer: {
      // The native renderer's graphics API: auto (D3D12, else Vulkan, else
      // D3D11), Vulkan or D3D11 (no emulated renderer any more).
      static const char* const kApis[3] = {"\"any\"", "\"vulkan\"", "\"d3d11\""};
      api_ = (api_ + dir + 3) % 3;
      SaveSetting("native_renderer", "\"main\"");
      SaveSetting("gpu_backend", kApis[api_]);
      break;
    }
    default:
      break;
  }
}

void GraphicsPage::SetBinding(int i, const std::string& keys) {
  rex::cvar::SetFlagByName(kBindings[i].cvar, keys);
  SaveSetting(kBindings[i].cvar, "\"" + keys + "\"");
  REXLOG_INFO("[svr2011] controls: {} = \"{}\"", kBindings[i].cvar, keys);
}

void GraphicsPage::SetNotice(const std::string& text) {
  notice_ = text;
  notice_until_ = Clock::now() + std::chrono::seconds(6);
  REXLOG_INFO("[svr2011] controls: {}", text);
}

void GraphicsPage::AssignKey(int binding, const std::string& name, bool add) {
  const bool mouse = IsMouseKey(name);
  std::vector<int> others;
  for (int i = 0; i < kNumBindings; ++i) {
    if (i == binding) continue;
    const std::vector<std::string> keys = SplitKeys(rex::cvar::Query<std::string>(kBindings[i].cvar));
    if (std::find(keys.begin(), keys.end(), name) == keys.end()) continue;
    if (!mouse && NeedsKey(i) && DeviceKeys(rex::cvar::Query<std::string>(kBindings[i].cvar), false).size() == 1) {
      SetNotice(ShowKeys(name) + " IS THE ONLY KEY FOR " + kBindings[i].input + " - NOT CHANGED");
      return;
    }
    others.push_back(i);
  }
  for (int i : others) {
    std::vector<std::string> keys = SplitKeys(rex::cvar::Query<std::string>(kBindings[i].cvar));
    keys.erase(std::remove(keys.begin(), keys.end(), name), keys.end());
    SetBinding(i, JoinKeys(keys));
    SetNotice(ShowKeys(name) + " MOVED FROM " + kBindings[i].input + " TO " + kBindings[binding].input);
  }
  std::vector<std::string> keys;
  for (const std::string& k : SplitKeys(rex::cvar::Query<std::string>(kBindings[binding].cvar)))
    if (add || IsMouseKey(k) != mouse) keys.push_back(k);
  if (std::find(keys.begin(), keys.end(), name) == keys.end()) keys.push_back(name);
  SetBinding(binding, JoinKeys(keys));
}

void GraphicsPage::SetPadLayout(uint32_t player, uint64_t layout) {
  SetPadMapLayout(player, layout);
  const std::string setting = "pad_map_p" + std::to_string(player + 1), text = PadMapText(layout);
  rex::cvar::SetFlagByName(setting, text);
  SaveSetting(setting, "\"" + text + "\"");
  REXLOG_INFO("[svr2011] controls: {} = \"{}\"", setting, text);
}

void GraphicsPage::ResetControlTab() {
  if (control_tab_ == kPadTab) {
    SetPadLayout(uint32_t(pad_player_), kPadMapStandard);
    SetNotice("PLAYER " + std::to_string(pad_player_ + 1) + ": THE STANDARD BUTTONS");
    return;
  }
  const bool mouse = control_tab_ == kMouseTab;
  for (int i = 0; i < kNumBindings; ++i) {
    const std::string now = rex::cvar::Query<std::string>(kBindings[i].cvar);
    // (the tab's own part goes back; the other tab's stays)
    std::vector<std::string> keys = mouse ? DeviceKeys(now, false) : SplitKeys(kBindings[i].keys);
    if (!mouse)
      for (const std::string& k : DeviceKeys(now, true)) keys.push_back(k);
    SetBinding(i, JoinKeys(keys));
  }
  if (mouse) {
    rex::cvar::SetFlagByName("mnk_mouse", "false");
    SaveSetting("mnk_mouse", "false");
    rex::cvar::SetFlagByName("mnk_sensitivity", "1.0");
    SaveSetting("mnk_sensitivity", "1.0");
  }
  SetNotice(mouse ? "THE MOUSE: NO BUTTONS, NO MOUSE LOOK" : "THE KEYBOARD: THE DEFAULT KEYS");
}

void GraphicsPage::StartCapture(int input, bool add) {
  // (from the next frame on: the key that started it is not the new key)
  capture_ = input;
  capture_mod_ = -1;
  capture_add_ = add;
  capture_armed_ = false;
  (void)TakePadIconsMouseClick();
  capture_until_ = Clock::now() + std::chrono::seconds(8);
}

void GraphicsPage::ControlsInput(uint16_t pressed, uint16_t act, bool& back) {
  using namespace rex::input;
  auto key = [](ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); };
  // LB / RB, Tab: the tab.
  if ((pressed & (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER)) || key(ImGuiKey_Tab, false)) {
    const int tabs = kControlTabs - kFirstControlTab;
    const int step = (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) ? tabs - 1 : 1;
    control_tab_ = kFirstControlTab + (control_tab_ - kFirstControlTab + step) % tabs;
    control_row_ = control_top_ = 0;
    return;
  }
  const int rows = ControlRows(control_tab_);
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow, true)) control_row_ = (control_row_ + rows - 1) % rows;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow, true)) control_row_ = (control_row_ + 1) % rows;
  const bool change = (pressed & X_INPUT_GAMEPAD_A) || key(ImGuiKey_Enter, false);
  const bool add = (pressed & X_INPUT_GAMEPAD_Y) || key(ImGuiKey_Insert, false);
  const bool clear = (pressed & X_INPUT_GAMEPAD_X) || key(ImGuiKey_Delete, false);
  const bool reset = (pressed & X_INPUT_GAMEPAD_BACK) || key(ImGuiKey_R, false);
  const bool left = (act & X_INPUT_GAMEPAD_DPAD_LEFT) || key(ImGuiKey_LeftArrow, false);
  const bool right = (act & X_INPUT_GAMEPAD_DPAD_RIGHT) || key(ImGuiKey_RightArrow, false);
  const int first = FirstInputRow(control_tab_);
  if (control_row_ < first) {  // (the tab's settings)
    if (control_tab_ == kPadTab && control_row_ == kPadPlayerRow) {
      if (change || right) pad_player_ = (pad_player_ + 1) % kPadMapPlayers;
      if (left) pad_player_ = (pad_player_ + kPadMapPlayers - 1) % kPadMapPlayers;
    } else if (control_tab_ == kPadTab) {  // VIBRATION: A, left / right toggle it
      if (change || left || right) {
        const bool on = !rex::cvar::Query<bool>("vibration");
        rex::cvar::SetFlagByName("vibration", on ? "true" : "false");
        SaveSetting("vibration", on ? "true" : "false");
        REXLOG_INFO("[svr2011] controls: vibration {}", on ? "on" : "off");
      }
    } else if (control_row_ == kMouseLookRow) {
      if (change || left || right) {
        const bool on = !rex::cvar::Query<bool>("mnk_mouse");
        rex::cvar::SetFlagByName("mnk_mouse", on ? "true" : "false");
        SaveSetting("mnk_mouse", on ? "true" : "false");
        REXLOG_INFO("[svr2011] controls: mouse look {}", on ? "on" : "off");
      }
    } else if (left || right) {  // MOUSE SPEED
      const double speed = std::clamp(rex::cvar::Query<double>("mnk_sensitivity") + (right ? 0.25 : -0.25), 0.25, 4.0);
      const std::string text = std::to_string(speed);
      rex::cvar::SetFlagByName("mnk_sensitivity", text);
      SaveSetting("mnk_sensitivity", text);
    }
    return;
  }
  if (control_row_ == rows - 1) {  // RESET TO DEFAULTS
    if (change) ResetControlTab();
    return;
  }
  const int input = control_row_ - first;
  if (control_tab_ == kPadTab) {
    const PadType type = PlayerPadType(uint32_t(pad_player_));
    if (change) {
      if (type == PadType::kXbox || type == PadType::kPlayStation) {
        StartCapture(input, false);
        back = false;
      } else {
        SetNotice("PLAYER " + std::to_string(pad_player_ + 1) + " HAS NO CONTROLLER NOW");
      }
    } else if (reset) {  // (this input back on its own button; BACK doesn't close the page here)
      uint64_t layout = PadMapLayout(uint32_t(pad_player_));
      if (PadMapAssign(&layout, input, input) >= 0) SetPadLayout(uint32_t(pad_player_), layout);
      back = false;
    }
    return;
  }
  const bool mouse = control_tab_ == kMouseTab;
  const std::string now = rex::cvar::Query<std::string>(kBindings[input].cvar);
  if (change || add) {
    StartCapture(input, add && !change);
    back = false;
  } else if (clear) {
    if (!mouse && NeedsKey(input)) {
      SetNotice(std::string(kBindings[input].input) + " MUST KEEP A KEY: THE MENUS NEED IT");
    } else {
      SetBinding(input, JoinKeys(DeviceKeys(now, !mouse)));
    }
  } else if (reset) {  // (this input's default keys, its mouse buttons as they are; the mouse: none)
    back = false;
    if (mouse) {
      SetBinding(input, JoinKeys(DeviceKeys(now, false)));
    } else {
      bool first_key = true;
      for (const std::string& k : SplitKeys(kBindings[input].keys)) {
        AssignKey(input, k, !first_key);
        first_key = false;
      }
    }
  }
}

void GraphicsPage::Hide() {
  g_wait_release = true;
  g_open = false;
}

uint16_t GraphicsPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  const int lx = s.gamepad.thumb_lx, ly = s.gamepad.thumb_ly;
  constexpr int kDead = 16000;
  if (ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (lx < -kDead) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (lx > kDead) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  return b;
}

// Text in `font` at `size` pixels.
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// Test aid: SVR2011_TEST_PAGE_KEYS=<file> - each line written there ("G",
// "Shift+H", "Escape") is pressed on the page's keyboard (as ImGui key
// events: the test game's window never has the keyboard's focus).
void TestPageKeys(ImGuiIO& io) {
  static const std::string file = [] {
    const char* v = std::getenv("SVR2011_TEST_PAGE_KEYS");
    return std::string(v ? v : "");
  }();
  if (file.empty()) return;
  static ImGuiKey down = ImGuiKey_None;
  static bool shift = false;
  static Clock::time_point next{};
  if (down != ImGuiKey_None) {  // (released the frame after)
    io.AddKeyEvent(down, false);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    down = ImGuiKey_None;
    return;
  }
  if (Clock::now() < next) return;
  next = Clock::now() + std::chrono::milliseconds(200);
  std::ifstream in(file);
  std::string line;
  if (!std::getline(in, line)) return;
  std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::ofstream(file, std::ios::trunc) << rest;
  while (!line.empty() && (line.back() == 13 || line.back() == 32)) line.pop_back();
  if (IsMouseKey(line)) {
    static std::string click;  // (lasts: the page takes the name's address)
    click = line;
    PadIconsMouseClick(click.c_str());
    return;
  }
  shift = line.rfind("Shift+", 0) == 0;
  if (shift) line = line.substr(6);
  ImGuiKey key = line == "Escape" ? ImGuiKey_Escape : line == "Delete" ? ImGuiKey_Delete : ImGuiKey_None;
  for (const KeyName& k : KeyNames())
    if (line == k.name) key = k.key;
  if (key == ImGuiKey_None) return;
  if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
  io.AddKeyEvent(key, true);
  down = key;
}

void GraphicsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  TestPageKeys(io);
  if (g_open_requested.exchange(false)) {
    Load();
    tab_ = kDisplayTab;
    row_ = 0;
    if (const char* v = std::getenv("SVR2011_TEST_GFX_ROW")) row_ = std::atoi(v);  // (test aid: the cursor's row)
    mode_ = g_requested_mode.exchange(kGraphicsMode);
    control_row_ = control_top_ = 0;
    control_tab_ = kFirstControlTab;
    capture_ = -1;
    // The A that opened the page is still down: wait for everything to be
    // released, judged only on pad states the game polled after opening.
    wait_release_ = true;
    opened_at_ = Clock::now();
    g_open = true;
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }
  display_ = CurrentDisplayMode();  // (F11)

  // Input: controller (edges, with auto-repeat for held directions) and keys.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) {
      wait_release_ = false;
    }
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kDirs = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                             X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
  uint16_t act = pressed;
  const auto now = Clock::now();
  if (pressed & kDirs) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kDirs) && now >= repeat_at_) {
    act |= buttons & kDirs;
    repeat_at_ = now + std::chrono::milliseconds(130);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int move = 0, dir = 0;
  bool back = false;
  {
  const std::vector<Row>& rows = mode_ == kLanguageMode ? kLanguageRows : TabRows(tab_);
  const int n = std::max(1, int(rows.size()));
  if (capture_ >= 0) {
    // CONTROLS, waiting for a key: the keyboard is the key's (the pad - which
    // the keyboard drives too - is ignored until everything is released).
    prev_buttons_ = buttons;
    const auto cancel = [&] {
      capture_ = -1;
      capture_mod_ = -1;
      wait_release_ = true;
      opened_at_ = now;
    };
    // Shift / Ctrl / Alt pressed and let go with no other key: that key alone
    // (the keyboard driver takes a bare modifier name as a key of its own).
    struct Mod {
      ImGuiKey left, right;
      const char* name;
    };
    static const Mod kMods[] = {{ImGuiKey_LeftShift, ImGuiKey_RightShift, "Shift"},
                                {ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, "Ctrl"},
                                {ImGuiKey_LeftAlt, ImGuiKey_RightAlt, "Alt"}};
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || now > capture_until_) {
      cancel();
    } else if (control_tab_ == kPadTab) {
      // A button on that player's controller, once everything is released (the button that started this).
      X_INPUT_STATE st = {};
      uint32_t held = 0;
      if (g_input && g_input->HeldState(uint32_t(pad_player_), &st)) {
        for (int i = 0; i < kPadInputs; ++i)
          if (PadInput(i).bit & uint16_t(st.gamepad.buttons)) held |= 1u << i;
        if (uint8_t(st.gamepad.left_trigger) > 0x40) held |= 1u << 6;
        if (uint8_t(st.gamepad.right_trigger) > 0x40) held |= 1u << 7;
      }
      if (!capture_armed_) {
        capture_armed_ = held == 0;
      } else if (held) {
        int physical = 0;
        while (!(held >> physical & 1)) ++physical;
        uint64_t layout = PadMapLayout(uint32_t(pad_player_));
        const int other = PadMapAssign(&layout, capture_, physical);
        if (other >= 0) {
          SetPadLayout(uint32_t(pad_player_), layout);
          const bool ps = PlayerPadType(uint32_t(pad_player_)) == PadType::kPlayStation;
          SetNotice(std::string(PadInputLabel(capture_, ps)) + " AND " + PadInputLabel(other, ps) + " SWAPPED");
        }
        cancel();
      }
    } else if (control_tab_ == kMouseTab) {
      if (const char* click = TakePadIconsMouseClick()) {
        AssignKey(capture_, click, capture_add_);
        cancel();
      }
    } else {
      for (const Mod& m : kMods)
        if (ImGui::IsKeyPressed(m.left, false) || ImGui::IsKeyPressed(m.right, false)) capture_mod_ = &m - kMods;
      const char* bare = nullptr;
      if (capture_mod_ >= 0 && !ImGui::IsKeyDown(kMods[capture_mod_].left) && !ImGui::IsKeyDown(kMods[capture_mod_].right))
        bare = kMods[capture_mod_].name;
      if (bare) {
        AssignKey(capture_, bare, capture_add_);
        cancel();
      } else {
        for (const KeyName& k : KeyNames()) {
          if (!ImGui::IsKeyPressed(k.key, false)) continue;
          std::string name;
          if (io.KeyCtrl) name += "Ctrl+";
          if (io.KeyAlt) name += "Alt+";
          if (io.KeyShift) name += "Shift+";
          name += k.name;
          AssignKey(capture_, name, capture_add_);
          cancel();
          break;
        }
      }
    }
  } else {
    if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
    if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
    if ((act & X_INPUT_GAMEPAD_DPAD_LEFT) || key(ImGuiKey_LeftArrow)) dir = -1;
    if ((act & X_INPUT_GAMEPAD_DPAD_RIGHT) || key(ImGuiKey_RightArrow)) dir = 1;
    if ((pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) dir = 1;
    if ((pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      back = true;
    }
    if (mode_ == kGraphicsMode &&
        ((pressed & (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER)) ||
         ImGui::IsKeyPressed(ImGuiKey_Tab, false))) {
      const int step = (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) ? kTabs - 1 : 1;
      tab_ = (tab_ + step) % kTabs;
      row_ = 0;
    } else if (mode_ == kControlsMode) {
      ControlsInput(pressed, act, back);
    } else {
      if (move) row_ = (row_ + move + n) % n;
      if (dir) Change(rows[row_], dir);
    }
  }
  }
  const std::vector<Row>& rows = mode_ == kLanguageMode ? kLanguageRows : TabRows(tab_);
  const int n = int(rows.size());

  // Layout in the game's 1280 x 720 frame (letterboxed into the window), over
  // the OPTIONS panel: the panel 144..1134 x 78..542, its header 78..112.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // Panel: dark glass with a thin white rim, as the game's.
  dl->AddRectFilled(P(144, 78), P(1134, 542), IM_COL32(8, 8, 12, 255), 10 * s);
  dl->AddRectFilledMultiColor(P(146, 80), P(1132, 300), IM_COL32(40, 40, 48, 120),
                              IM_COL32(40, 40, 48, 120), IM_COL32(8, 8, 12, 0),
                              IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(144, 78), P(1134, 542), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white tab with the title, the page name.
  dl->AddRectFilled(P(150, 84), P(1128, 118), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 160; x < 1120; x += 7) {
    for (float y = 90; y < 114; y += 7) {
      dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
    }
  }
  dl->AddQuadFilled(P(248, 84), P(690, 84), P(672, 118), P(236, 118), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(248, 84), P(690, 84), P(672, 118), P(236, 118), IM_COL32(235, 235, 240, 255),
              2 * s);
  {
    const char* title = "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(462, 0).x - sz.x * 0.5f, P(0, 101).y - sz.y * 0.5f),
         IM_COL32(255, 255, 255, 255), title);
    // GRAPHICS: its tabs (LB / RB), the chosen one white on red; CONTROLS and
    // LANGUAGE: their name alone.
    const bool alone = mode_ == kLanguageMode || (mode_ == kControlsMode && kFirstControlTab == kPadTab);
    const float ps = 18 * s;
    float x = P(708, 0).x;
    const float cy = P(0, 101).y;
    const bool controls = mode_ == kControlsMode;
    for (int i = 0; i < (alone ? 1 : controls ? int(kControlTabs) : int(kTabs)); ++i) {
      const char* name = mode_ == kLanguageMode ? "LANGUAGE" : controls ? (alone ? "CONTROLS" : kControlTabNames[i]) : kTabNames[i];
      const bool chosen = alone || i == (controls ? control_tab_ : tab_);
      const ImVec2 z = TextSize(g_menu_font, ps, name);
      const ImVec2 a(x - 10 * s, cy - 13 * s), b(x + z.x + 10 * s, cy + 13 * s);
      if (chosen) {
        dl->AddRectFilled(a, b, IM_COL32(150, 18, 20, 255), 3 * s);
        dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.2f * s);
      }
      Text(dl, g_menu_font, ps, ImVec2(x, cy - z.y * 0.5f),
           chosen ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 158, 255), name);
      x += z.x + 24 * s;
    }
    const float hs = 15 * s;
    const char* hint = alone ? "" : "LB / RB";
    const ImVec2 hz = TextSize(g_menu_font, hs, hint);
    Text(dl, g_menu_font, hs, ImVec2(P(1112, 0).x - hz.x, cy - hz.y * 0.5f), IM_COL32(150, 150, 158, 255),
         hint);
  }

  // CONTROLS: the tab's settings, the inputs, RESET TO DEFAULTS; a scrolling list.
  if (mode_ == kControlsMode) {
    const float row_h = 30.0f, gap = 8.0f, top = 132.0f;
    const float fs = 21 * s;
    const int control_rows = ControlRows(control_tab_), first_input = FirstInputRow(control_tab_);
    const int reset_row = control_rows - 1;
    const PadType pad_type = PlayerPadType(uint32_t(pad_player_));
    const bool ps = pad_type == PadType::kPlayStation;
    const uint64_t layout = PadMapLayout(uint32_t(pad_player_));
    control_top_ = std::clamp(control_top_, std::max(0, control_row_ - kControlsShown + 1),
                              std::min(control_row_, control_rows - kControlsShown));
    for (int k = 0; k < kControlsShown; ++k) {
      const int i = control_top_ + k;
      if (i >= control_rows) break;
      const float y = top + k * (row_h + gap);
      const bool sel = i == control_row_;
      const ImVec2 a = P(166, y), b = P(1112, y + row_h);
      if (sel) {
        dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255),
                                    IM_COL32(95, 8, 10, 255), IM_COL32(150, 18, 20, 255));
        dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
      } else {
        dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
      }
      const float ty = P(0, y + row_h * 0.5f).y - TextSize(g_menu_font, fs, "A").y * 0.5f;
      if (i == reset_row) {
        const char* reset = "RESET TO DEFAULTS";
        const ImVec2 z = TextSize(g_menu_font, fs, reset);
        Text(dl, g_menu_font, fs, ImVec2(P(639, 0).x - z.x * 0.5f, ty), IM_COL32(255, 255, 255, 255), reset);
        continue;
      }
      std::string input, action, v;
      ImU32 colour = IM_COL32(255, 255, 255, 255);
      if (i < first_input) {
        if (control_tab_ == kPadTab && i == kPadPlayerRow) {
          input = "PLAYER", action = "WHOSE BUTTONS";
          v = std::to_string(pad_player_ + 1) + (pad_type == PadType::kXbox        ? " - XBOX"
                                                 : ps                              ? " - PLAYSTATION"
                                                 : pad_type == PadType::kKeyboard  ? " - KEYBOARD"
                                                                                   : " - NO CONTROLLER");
        } else if (control_tab_ == kPadTab) {
          input = "VIBRATION", action = "CONTROLLER RUMBLE", v = rex::cvar::Query<bool>("vibration") ? "ON" : "OFF";
        } else if (i == kMouseLookRow) {
          input = "MOUSE LOOK", action = "THE MOUSE MOVES THE RIGHT STICK", v = rex::cvar::Query<bool>("mnk_mouse") ? "ON" : "OFF";
        } else {
          char speed[16];
          std::snprintf(speed, sizeof(speed), "%.2f", rex::cvar::Query<double>("mnk_sensitivity"));
          input = "MOUSE SPEED", action = "MOUSE LOOK", v = speed;
        }
      } else if (control_tab_ == kPadTab) {
        const int in = i - first_input;
        input = PadInputLabel(in, ps), action = PadInput(in).action;
        v = capture_ == in ? "PRESS A BUTTON" : PadInputLabel(PadMapPhysical(layout, in), ps);
        if (capture_ != in && PadMapPhysical(layout, in) != in) colour = IM_COL32(255, 200, 60, 255);
      } else {
        const Binding& bd = kBindings[i - first_input];
        const bool mouse = control_tab_ == kMouseTab;
        input = bd.input, action = bd.action;
        const bool waiting = capture_ == i - first_input;
        v = waiting ? (mouse ? "CLICK A MOUSE BUTTON" : capture_add_ ? "PRESS A KEY TO ADD" : "PRESS A KEY")
                    : ShowKeys(JoinKeys(DeviceKeys(rex::cvar::Query<std::string>(bd.cvar), mouse)));
      }
      if (capture_ >= 0 && i >= first_input && capture_ == i - first_input) colour = IM_COL32(255, 200, 60, 255);
      Text(dl, g_menu_font, fs, ImVec2(P(186, 0).x, ty), IM_COL32(255, 255, 255, 255), input.c_str());
      Text(dl, g_menu_font, fs * 0.85f, ImVec2(P(470, 0).x, ty + fs * 0.08f), IM_COL32(180, 180, 188, 255), action.c_str());
      const ImVec2 vs = TextSize(g_menu_font, fs, v.c_str());
      Text(dl, g_menu_font, fs, ImVec2(P(1092, 0).x - vs.x, ty), colour, v.c_str());
    }
    // (more above / below)
    const ImU32 arrow = IM_COL32(200, 200, 205, 255);
    if (control_top_ > 0)
      dl->AddTriangleFilled(P(639, 120), P(629, 128), P(649, 128), arrow);
    if (control_top_ + kControlsShown < control_rows) {
      const float yb = top + kControlsShown * (row_h + gap) - gap + 4;
      dl->AddTriangleFilled(P(639, yb + 8), P(629, yb), P(649, yb), arrow);
    }
    const bool keys_off = control_tab_ != kPadTab && !rex::cvar::Query<bool>("mnk_mode");
    const char* help =
        capture_ >= 0
            ? (control_tab_ == kPadTab    ? "Press the button on that player's controller: it swaps with the one using it."
               : control_tab_ == kMouseTab ? "Click a mouse button. ESC: cancel."
                                           : "Press the key (Shift / Ctrl / Alt + a key: a combination; tapped alone: that key). ESC: cancel.")
        : control_row_ == reset_row
            ? (control_tab_ == kPadTab ? "A: this player's standard buttons." : control_tab_ == kMouseTab ? "A: no mouse buttons, no mouse look." : "A: the default keys for every input.")
        : control_row_ < first_input
            ? (control_tab_ == kPadTab && control_row_ == kPadPlayerRow ? "LEFT / RIGHT: the player. Their layout applies in matches; menus keep the real buttons."
               : control_tab_ == kMouseTab && control_row_ == kMouseSpeedRow ? "LEFT / RIGHT: how fast the mouse moves the right stick."
               : control_tab_ == kMouseTab ? "A / LEFT / RIGHT: the mouse moves the right stick, and the window holds the cursor."
                                           : "A / LEFT / RIGHT: controller vibration on or off.")
        : control_tab_ == kPadTab
            ? "A: change (the button is swapped)   BACK: its own button"
        : control_tab_ == kMouseTab
            ? "A: change   Y: add a button   X: clear   BACK: none"
            : "A: change   Y: add a key   X: clear   BACK: reset   (keyboard: Enter, Insert, Delete, R, Tab)";
    const float hs = 18 * s;
    const ImVec2 sz = TextSize(g_menu_font, hs, help);
    Text(dl, g_menu_font, hs, ImVec2(P(639, 0).x - sz.x * 0.5f, P(0, 508).y - sz.y * 0.5f),
         IM_COL32(200, 200, 205, 255), help);
    const std::string line = now < notice_until_ ? notice_ : keys_off ? std::string("THE KEYBOARD AND MOUSE ARE OFF IN THE LAUNCHER'S SETTINGS") : std::string();
    if (!line.empty()) {
      const ImVec2 nz = TextSize(g_menu_font, hs, line.c_str());
      Text(dl, g_menu_font, hs, ImVec2(P(639, 0).x - nz.x * 0.5f, P(0, 486).y - nz.y * 0.5f), IM_COL32(255, 200, 60, 255), line.c_str());
    }
    if (back) Hide();
    return;
  }

  // Rows.
  const bool restart_renderer = api_ != api_at_start_;
  const bool restart_crowd = crowd_ != crowd_at_start_;
  auto value = [&](Row id) -> const char* {
    switch (id) {
      case kResolution:
        return display_ != kWindowed || resolution_ >= int(g_resolutions.size()) ? "FULL SCREEN"
                                                                                 : g_resolutions[resolution_].label.c_str();
      case kDisplay: return kDisplayModeNames[display_];
      case kVsync: return vsync_ ? "ON" : "OFF";
      case kFrameRate: {
        static char fps[16];
        std::snprintf(fps, sizeof(fps), "%d FPS", kFrameRates[frame_rate_]);
        return fps;
      }
      case kFpsCounter: return perf_ == 2 ? "FULL" : perf_ == 1 ? "FPS" : "OFF";
      case kMatchCamera: return MatchCameraName(MatchCamera());
      case kOverlayCorner: {
        static const char* const kCorners[] = {"TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT"};
        return kCorners[corner_ & 3];
      }
      case kPing: return ping_ ? "ON" : "OFF";
      case kRouterPort: return router_port_ ? "ON" : "OFF";
      case kCpuPreset:
        return cpu_ == svr2011::CpuTuning{} ? "AUTO" : cpu_ == svr2011::kLegendPlus ? "LEGEND+" : "CUSTOM";
      case kCpuReversal: {
        static char b[16];
        if (cpu_.reversal < 0) return "AUTO";
        std::snprintf(b, sizeof b, "%d%%", cpu_.reversal);
        return b;
      }
      case kCpuRates: {
        static char b[16];
        if (cpu_.rates < 0) return "AUTO";
        std::snprintf(b, sizeof b, "%d", cpu_.rates * 10);
        return b;
      }
      case kCpuDamage:
      case kCpuPin:
      case kCpuMomentum: {
        static const char* const kLevels[] = {"EASY", "NORMAL", "HARD", "LEGEND"};
        const int v = id == kCpuDamage ? cpu_.damage : id == kCpuPin ? cpu_.pin : cpu_.momentum;
        return v < 0 || v > 3 ? "AUTO" : kLevels[v];
      }
      case kFullSpeed: return full_speed_ ? "ON" : "OFF (AS THE XBOX 360)";
      case kTouch: return touch_ ? "ON" : "OFF";
      case kWide: return wide_ ? "FULL WIDTH" : "16:9";
      case kDof: return dof_ ? "ON" : "OFF";
      case kMotionBlur: return blur_ ? "ON" : "OFF";
      case kSoft: return soft_ ? "ON (XBOX 360)" : "OFF";
      case kReplays: return replays_ ? "ON" : "OFF";
      case kCawLimits: return caw_no_limits_ ? "OFF" : "STOCK";
      case kFastStart: {
        static const char* const kNames[4] = {"OFF", "SKIP INTROS", "SKIP PRACTICE RING", "SKIP BOTH"};
        return kNames[fast_start_ & 3];
      }
      case kCpuPriority: return high_priority_ ? "HIGH" : "NORMAL";
      case kPrepare: return prepare_ ? "ON" : "OFF";
      case kLanguage: return kLanguages[language_].label;
      case kRenderer: return api_ == 1 ? "VULKAN" : api_ == 2 ? "DIRECT3D 11" : "AUTO";
      case kRenderScale: return kScales[scale_].label;
      case kTextureQuality: return kTextureQualityLabels[textures_];
      case kMatFix: return mat_fix_ ? "SMOOTH" : "ORIGINAL";
      case kVsStyle: {
        static std::string up;
        const auto& styles = svr2011::VsStyles();
        up = vs_style_ && vs_style_ <= int(styles.size()) ? styles[size_t(vs_style_ - 1)].second : "SvR 2011";
        for (char& c : up) c = char(std::toupper(uint8_t(c)));
        return up.c_str();
      }
      case kArenaBrightness: {
        static char b[16];
        std::snprintf(b, sizeof b, "%d%%", brightness_);
        return b;
      }
      case kAntiAliasing: return aa_ == 1 ? "OFF" : aa_ == 2 ? "2X" : aa_ == 3 ? "3X" : "4X";
      case kUpscaling: {
        static const char* const kNames[] = {"OFF",
                                             "FSR 1 ULTRA QUALITY",
                                             "FSR 1 QUALITY",
                                             "FSR 1 BALANCED",
                                             "FSR 1 PERFORMANCE",
                                             "FSR 2 ULTRA QUALITY (EXPERIMENTAL)",
                                             "FSR 2 QUALITY (EXPERIMENTAL)",
                                             "FSR 2 BALANCED (EXPERIMENTAL)",
                                             "FSR 2 PERFORMANCE (EXPERIMENTAL)",
                                             "DLSS ULTRA QUALITY (EXPERIMENTAL)",
                                             "DLSS QUALITY (EXPERIMENTAL)",
                                             "DLSS BALANCED (EXPERIMENTAL)",
                                             "DLSS PERFORMANCE (EXPERIMENTAL)"};
        return kNames[std::clamp(upscale_, 0, 12)];
      }
      case kPreset: {
        const Preset* p = CurrentPreset();
        return p ? p->name : "CUSTOM";
      }
      case kCrowd: return kLevelNames[crowd_];
      case kShadows: return shadows_ ? "ON" : "OFF";
      case kEffects: return effects_ ? "HIGH" : "NORMAL";
      case kCutsceneFps: return fps60_ ? "60 FPS" : "30 FPS (ORIGINAL)";
    }
    return "";
  };
  auto label = [](Row id) -> const char* {
    switch (id) {
      case kResolution: return "RESOLUTION";
      case kDisplay: return "DISPLAY MODE";
      case kVsync: return "VSYNC";
      case kFrameRate: return "FRAME RATE";
      case kFpsCounter: return "PERFORMANCE";
      case kOverlayCorner: return "PERFORMANCE CORNER";
      case kMatchCamera: return "MATCH CAMERA";
      case kPing: return "ONLINE PING";
      case kRouterPort: return "ROUTER PORT (UPNP)";
      case kCpuPreset: return "PRESET";
      case kCpuReversal: return "CPU REVERSAL CHANCE";
      case kCpuRates: return "CPU REVERSAL RATES";
      case kCpuDamage: return "DAMAGE";
      case kCpuPin: return "PIN KICK-OUT";
      case kCpuMomentum: return "MOMENTUM";
      case kFullSpeed: return "FULL SPEED";
      case kTouch: return "TOUCH CONTROLS";
      case kWide: return "WIDE SCREENS";
      case kDof: return "DEPTH OF FIELD";
      case kMotionBlur: return "MOTION BLUR";
      case kSoft: return "SOFT FILTER";
      case kReplays: return "REPLAYS";
      case kFastStart: return "FAST START";
      case kCawLimits: return "CAW LIMITS";
      case kCpuPriority: return "CPU PRIORITY";
      case kPrepare: return "PREPARE GRAPHICS";
      case kLanguage: return "LANGUAGE";
      case kRenderer: return "GRAPHICS API";
      case kRenderScale: return "RENDER RESOLUTION";
      case kTextureQuality: return "TEXTURES";
      case kArenaBrightness: return "ARENA BRIGHTNESS";
      case kMatFix: return "RING MAT";
      case kVsStyle: return "VS SCREEN STYLE";
      case kAntiAliasing: return "ANTI-ALIASING";
      case kUpscaling: return "UPSCALING";
      case kEffects: return "EFFECT DETAIL";
      case kPreset: return "PRESET";
      case kCrowd: return "CROWD DENSITY";
      case kShadows: return "SHADOWS";
      case kCutsceneFps: return "ENTRANCE FRAME RATE";
    }
    return "";
  };
  auto help_for = [this](Row id) -> const char* {
    switch (id) {
      case kResolution: return "The window's size. The game renders at the scale that fills it.";
      case kDisplay:
        return display_ == kExclusive ? "Full screen: the game takes the display (F11 switches to a window)."
               : display_ == kBorderless ? "Borderless: a window covering the screen (F11 switches)."
                                         : "Play in a window (F11 switches to full screen).";
      case kVsync: return "Waits for the monitor's refresh: no tearing.";
      case kFrameRate: return "Frames a second in matches, at most (menus: 60). The game always runs at its normal speed.";
      case kFpsCounter: return "FPS: the frame rate. FULL: also the game's video memory (its textures and render targets). F2 switches it.";
      case kOverlayCorner: return "Which corner of the screen the performance numbers sit in.";
      case kMatchCamera:
        return "The camera in ring matches: NORMAL, FAR, CLOSE, HIGH, LOW or RAMP (the Royal Rumble's view). In a "
               "match BACK + R3 (F4) changes it.";
      case kRouterPort: return "Online: your router opens the game's port while you play (UPnP), so far-off players connect directly. OFF: never touched.";
      case kCpuPreset:
        return "AUTO: the CPU as the DIFFICULTY you pick in the game. LEGEND+: harder than LEGEND - every row below "
               "at its hard end. Online the host's settings count.";
      case kCpuReversal:
        return "How often the CPU takes a chance to reverse. AUTO: the difficulty's (EASY 30%, NORMAL 55%, HARD 80%, "
               "LEGEND 100%). Over 100%: more than LEGEND.";
      case kCpuRates:
        return "The game's A.I. REVERSAL BALANCING sliders for every kind of move (its default 50, most 100). AUTO: "
               "your own settings there.";
      case kCpuDamage:
        return "Damage as on that difficulty (EASY less, LEGEND more). AUTO: the match's difficulty.";
      case kCpuPin:
        return "The pin kick-out as on that difficulty (LEGEND: the shortest window). AUTO: the match's difficulty.";
      case kCpuMomentum:
        return "Momentum and stamina rates as on that difficulty. AUTO: the match's difficulty.";
      case kPing: return "Online matches: the delay to the other players at the top right (bars: green under 150 ms, yellow under 250, red above).";
      case kFullSpeed:
        return full_speed_ ? "The game keeps its normal speed when the frame rate drops (and at 30 FPS)."
                           : "One game step a frame, as the Xbox 360: the game slows down below 60 FPS (30 FPS off).";
      case kTouch: return "The on-screen controller. Its EDIT button moves, resizes and remaps it.";
      case kWide: return "Screens wider than 16:9: matches fill the width (menus stay 16:9).";
      case kDof: return "Wide shots blur what is out of focus. OFF: the whole ring stays sharp.";
      case kMotionBlur: return "Blur and trails on fast moves and replays.";
      case kSoft: return "The game's 720p smoothing filter: blurs wide shots at higher resolutions.";
      case kReplays: return "Instant replays after finishers and the highlights at the end of a match.";
      case kCawLimits:
        return caw_no_limits_ ? "CREATE A SUPERSTAR: no points budget, stack any parts (they may clip)."
                              : "CREATE A SUPERSTAR with the game's own points budget and part counts.";
      case kFastStart:
        return "At launch: no logo movies (straight to the Start Screen) and / or no practice ring (straight to the "
               "main menu). Next start.";
      case kCpuPriority: return "Helps on busy or weak PCs: the game gets the CPU before other programs.";
      case kPrepare: return "Builds the graphics ahead in the menus, so matches don't stutter (native).";
      case kLanguage:
        return language_ == language_at_start_
                   ? "The game's text (the commentary stays English)."
                   : "The game's text: changes the next time the game starts.";
      case kRenderer: return "The graphics API the game draws with. AUTO: Direct3D 12, else Vulkan, else Direct3D 11 (older GPUs).";
      case kRenderScale:
        return "The game's picture size. AUTO fills the screen; lower is faster (360P / 480P: softer, for weak "
               "devices; 1080P: sharp on a 1080p screen, lighter than AUTO's 1440P). ANTI-ALIASING draws it bigger.";
      case kCrowd:
        return crowd_ == crowd_at_start_ ? "The people in the seats. LOW: empty seats. MED: about half. HIGH: everyone."
                                         : "Takes effect the next time the game starts.";
      case kShadows: return "The wrestlers', ring's and arena's shadows. OFF: faster.";
      case kPreset:
        return "Sets resolution, upscaling, anti-aliasing, textures, effect detail, crowd, shadows and frame rate "
               "together. Changing one of them afterwards shows CUSTOM. A new crowd level applies at the next start.";
      case kAntiAliasing:
        return upscale_ ? "Off while UPSCALING is on: FSR / DLSS draw the picture smaller and smooth its edges "
                          "themselves. Set UPSCALING to OFF to use this."
                        : "Smoother edges: the picture is drawn 2, 3 or 4 times as tall and wide as your screen and "
                          "shrunk to it - slower. At most 4x the Xbox 360's 720p, so on a 1080p screen 3X and 4X look "
                          "the same. Works with every RENDER RESOLUTION.";
      case kUpscaling:
        return upscale_ >= 9
                   ? "NVIDIA DLSS 4 (experimental, RTX GPUs): matches drawn smaller and rebuilt by NVIDIA's AI model "
                     "over several frames; fast moves can leave faint trails. Menus use FSR 1."
               : upscale_ >= 5
                   ? "AMD FSR 2 (experimental): matches drawn smaller and rebuilt over several frames - sharper "
                     "than FSR 1, smooth edges; fast moves can leave faint trails. Menus use FSR 1."
                   : "AMD FSR 1: the game drawn smaller (77%, 67%, 59% or 50% of the screen's height) and scaled up "
                     "sharp - faster on weak PCs and phones. Replaces RENDER SCALE while on.";
      case kVsStyle:
        return "The match set-up screen's look: SvR 2011's, or an older game's made by the Mod Maker from your "
               "disc (From another game). From the next start of the game.";
      case kMatFix:
        return "SMOOTH: the game's ring mats without the rows of light patches in their art. ORIGINAL: as on the "
               "Xbox 360. From the next arena that loads.";
      case kArenaBrightness:
        return "The arena's lights - on the wrestlers, the ring and the hall. 100%: the game's own. From the next "
               "arena that loads.";
      case kTextureQuality:
        return "MEDIUM / LOW: big textures at half / a quarter of their size - less memory, faster loading. "
               "Menus and text stay sharp.";
      case kEffects:
        return "HIGH: shadows, reflections and glow at the render resolution. NORMAL: at the Xbox 360's size, faster.";
      case kCutsceneFps: return "Entrances and cutscenes at 60 fps, or 30 as on the Xbox 360 (half the work).";
    }
    return "";
  };
  row_ = std::clamp(row_, 0, n - 1);
  // At most kShown rows on screen (the help line below stays clear on any tab): more scroll with the cursor,
  // arrows above / below saying so.
  constexpr int kShown = 9;
  static int first = 0;
  if (n <= kShown) first = 0;
  first = std::clamp(first, std::max(0, row_ - kShown + 1), std::min(row_, std::max(0, n - kShown)));
  const int shown = std::min(n, kShown);
  // (9 rows: a little tighter, so the help line below stays clear)
  const float row_h = shown > 7 ? 30.0f : 34.0f, gap = shown > 7 ? 7.0f : 13.0f;
  const float top = 310 - (float(shown) * (row_h + gap) - gap) * 0.5f;
  const float fs = 22 * s;
  {
    const float ah = 7 * s, cx = P(639, 0).x;
    if (first > 0) {
      const float ay = P(0, top - 12).y;
      dl->AddTriangleFilled(ImVec2(cx, ay - ah), ImVec2(cx - ah * 1.4f, ay + ah * 0.6f),
                            ImVec2(cx + ah * 1.4f, ay + ah * 0.6f), IM_COL32(255, 255, 255, 220));
    }
    if (first + shown < n) {
      const float ay = P(0, top + shown * (row_h + gap) - gap + 12).y;
      dl->AddTriangleFilled(ImVec2(cx, ay + ah), ImVec2(cx - ah * 1.4f, ay - ah * 0.6f),
                            ImVec2(cx + ah * 1.4f, ay - ah * 0.6f), IM_COL32(255, 255, 255, 220));
    }
  }
  for (int i = first; i < first + shown; ++i) {
    const Row id = rows[i];
    const float y = top + (i - first) * (row_h + gap);
    const bool sel = i == row_;
    const ImVec2 a = P(166, y), b = P(1112, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(95, 8, 10, 255), IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    const float ty = P(0, y + row_h * 0.5f).y - TextSize(g_menu_font, fs, "A").y * 0.5f;
    Text(dl, g_menu_font, fs, ImVec2(P(186, 0).x, ty), IM_COL32(255, 255, 255, 255), label(id));
    const char* v = value(id);
    const ImVec2 vs = TextSize(g_menu_font, fs, v);
    const float vx = P(930, 0).x - vs.x * 0.5f;
    const ImU32 vc = ((id == kRenderer && restart_renderer) || (id == kCrowd && restart_crowd))
                         ? IM_COL32(255, 200, 60, 255)
                         : IM_COL32(255, 255, 255, 255);
    Text(dl, g_menu_font, fs, ImVec2(vx, ty), vc, v);
    if (sel) {
      const float cy = P(0, y + row_h * 0.5f).y, h = 8 * s;
      const float lx = P(780, 0).x, rx = P(1080, 0).x;
      dl->AddTriangleFilled(ImVec2(lx - h, cy), ImVec2(lx + h * 0.6f, cy - h),
                            ImVec2(lx + h * 0.6f, cy + h), IM_COL32(255, 255, 255, 255));
      dl->AddTriangleFilled(ImVec2(rx + h, cy), ImVec2(rx - h * 0.6f, cy - h),
                            ImVec2(rx - h * 0.6f, cy + h), IM_COL32(255, 255, 255, 255));
    }
  }
  // Description of the selected setting, as the game's panels have.
  {
    const Row id = rows[row_];
    std::string help = help_for(id);
    if (id == kRenderer && restart_renderer)
      help = "Takes effect the next time the game starts.";
    const float hs = 18 * s;
    const ImVec2 sz = TextSize(g_menu_font, hs, help.c_str());
    Text(dl, g_menu_font, hs, ImVec2(P(639, 0).x - sz.x * 0.5f, P(0, 508).y - sz.y * 0.5f),
         IM_COL32(200, 200, 205, 255), help.c_str());
  }

  if (back) Hide();
}

}  // namespace

void InstallGraphicsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                         rex::input::InputSystem* input,
                         const std::filesystem::path& config_path) {
  g_window = window;
  g_input = input;
  g_config_path = config_path;
#if !defined(__ANDROID__)
  // (a settings file with no key on an input the menus need - an older version's CLEAR: its default keys, or a keyboard
  // player could never reach this page again)
  for (int i = 0; i < kNumBindings; ++i) {
    if (!NeedsKey(i)) continue;
    const std::string now = rex::cvar::Query<std::string>(kBindings[i].cvar);
    if (!DeviceKeys(now, false).empty()) continue;
    std::vector<std::string> keys = SplitKeys(kBindings[i].keys);
    for (const std::string& k : DeviceKeys(now, true)) keys.push_back(k);
    const std::string text = JoinKeys(keys);
    rex::cvar::SetFlagByName(kBindings[i].cvar, text);
    SaveSetting(kBindings[i].cvar, "\"" + text + "\"");
    REXLOG_WARN("[svr2011] controls: {} had no key - back to \"{}\"", kBindings[i].cvar, text);
  }
#endif
  new GraphicsPage(drawer);  // lives for the whole run
  // F11: full screen (the kind chosen on the page) <-> window.
  rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle full screen", [] {
    if (!g_window) return;
    SetDisplayMode(g_window->IsFullscreen() ? kWindowed
                   : rex::cvar::Query<bool>("fullscreen_exclusive") ? kExclusive : kBorderless);
  });
  if (input) {
    // (one hold for the port's pages: this one, ACHIEVEMENTS, JUKEBOX and BACKGROUNDS)
    input->SetGuestInputHold([] { return GuestPagesHoldInput(); });
  }
}

bool GuestPagesHoldInput() {
  return g_open.load() || g_wait_release.load() || AchievementsPageHoldsInput() || JukeboxPageHoldsInput() ||
         BackgroundsPageHoldsInput() || WeaponsPageHoldsInput() || TouchControlsHoldInput() || OnlineOverlayHoldsInput() ||
         MysteryOpponentHoldsInput() || ReportPageHoldsInput();
}

void SetGraphicsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenGraphicsPage() {
  g_requested_mode = kGraphicsMode;
  g_open_requested = true;
}

void OpenControlsPage() {
  g_requested_mode = kControlsMode;
  g_open_requested = true;
}

void SaveConfigSetting(const std::string& key, const std::string& value) { SaveSetting(key, value); }

void OpenLanguagePage() {
  g_requested_mode = kLanguageMode;
  g_open_requested = true;
}

void RequestExit() {
  rex::ui::Window* w = g_window;
  if (!w) return;
  w->app_context().CallInUIThread([w] { w->RequestClose(); });
}

}  // namespace svr2011
