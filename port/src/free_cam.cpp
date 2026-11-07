// Free camera / photo mode: see free_cam.h.
#include "free_cam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "fps_overlay.h"
#include "generated/default/svr2011_init.h"
#include "frame_rate.h"
#include "graphics_page.h"
#include "keyboard_typing.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_INT32(match_camera, 0, "UI",
                     "The match camera: 0 NORMAL, 1 FAR, 2 CLOSE, 3 HIGH, 4 LOW, 5 RAMP (ring matches; BACK + R3 / F4 cycle it)");

namespace svr2011 {
namespace {

using Clock = std::chrono::steady_clock;

using svrfmt::Rd32, svrfmt::RdF, svrfmt::WrF;

struct V3 {
  float x = 0, y = 0, z = 0;
  float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Unit(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 1};
}

// The scene's view: sub_82227150(view, eye, direction, ...) builds it, a 4 x 4
// matrix, into 0x82E3CC00 (match_types.cpp's hook calls FreeCamSceneView after
// it; every in-match camera's frame update ends there). Row vectors:
// p_view = p_world * V, the view looking down +z; the world's up is -Y.
constexpr uint32_t kSceneView = 0x82E3CC00;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;  // (frame_rate.cpp: the match's clock)

enum class Mode { kOff, kFlying, kFixed };

std::mutex g_mutex;
Mode g_mode = Mode::kOff;
bool g_capture = false;              // (F6 from off: take the game's view at the next draw)
bool g_paused = false;               // F8
std::atomic<bool> g_hide_ui{false};  // F7
// The match's clock stood still a moment (not by F8): the game's own pause menu (START), a replay - its UI shows
// and its keys are its own (FreeCamFrame). 2.1.8 reports 43e5cc / 176c9a: an F7 pressed before (in a menu, or a
// match ago) hid every match's pause menu, and a flying camera kept the arrows / W A S D the menu moves with -
// "the pause screen doesn't work", "couldn't exit the match".
std::atomic<bool> g_clock_still{false};
// The free camera, in the space of the game's view when it was taken (V0):
// its eye and its axes (right, up, forward) there - so its view is
// V0 * T(-eye) * [axes]. Up and the yaw: the taken view's up (level, as the
// game's cameras are).
float g_v0[4][4] = {};
V3 g_eye;
V3 g_axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
V3 g_wup{0, -1, 0};            // (the world's up there: -Y is up in this game)
float g_right_sign = 1;        // (right = this x (up x forward): the taken view's x)
// The zoom ([ ], the triggers): the view's x and y scaled (as a narrower or
// wider field of view would; the projection is the game's).
float g_zoom = 1;
float g_speed = 100;           // (units a second: a tenth of the taken view's distance to what it looked at)
Clock::time_point g_last_tick;

// Held keys (the window's key events) and script-held keys (test aid); the pads.
bool g_held[256] = {};
std::atomic<bool> g_keyboard_used{false};  // (a key pressed in the game's window this session: the hints are for it)
Clock::time_point g_script_until[256] = {};
float g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0, g_lt = 0, g_rt = 0;
bool g_lb = false, g_rb = false;

bool Held(uint8_t vk) { return g_held[vk] || Clock::now() < g_script_until[vk]; }

void ReadView(const uint8_t* p, float m[4][4]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m[i][j] = RdF(p + 16 * i + 4 * j);
}

void Fly(float dt) {
  float speed = g_speed;
  if (Held(0x10) || Held(0xA0) || Held(0xA1)) speed *= 4.0f;
  if (Held(0x11) || Held(0xA2) || Held(0xA3)) speed *= 0.2f;
  const float mx = (Held('D') ? 1.0f : 0) - (Held('A') ? 1.0f : 0) + g_lx;
  const float mz = (Held('W') ? 1.0f : 0) - (Held('S') ? 1.0f : 0) + g_ly;
  const float my = (Held('E') ? 1.0f : 0) - (Held('Q') ? 1.0f : 0) + (g_rb ? 1.0f : 0) - (g_lb ? 1.0f : 0);
  const float yaw = (Held(0x27) ? 1.0f : 0) - (Held(0x25) ? 1.0f : 0) + g_rx;
  const float pitch = (Held(0x26) ? 1.0f : 0) - (Held(0x28) ? 1.0f : 0) + g_ry;
  const float zoom = (Held(0xDD) ? 1.0f : 0) - (Held(0xDB) ? 1.0f : 0) + g_rt - g_lt;  // (] / RT closer, [ / LT wider)
  g_zoom = std::clamp(g_zoom * std::exp(zoom * 0.8f * dt), 0.7f, 6.0f);
  g_eye = g_eye + g_axes[0] * (mx * speed * dt) + g_axes[2] * (mz * speed * dt) + g_wup * (my * speed * dt);
  // (turned about the world's up and tilted against the horizon: never rolled)
  const float look = 1.2f * dt / g_zoom;  // (slower zoomed in)
  float e = std::asin(std::clamp(Dot(g_axes[2], g_wup), -1.0f, 1.0f));
  V3 h = Unit(g_axes[2] + g_wup * -Dot(g_axes[2], g_wup));
  if (yaw != 0) h = Unit(h * std::cos(yaw * look) + g_axes[0] * std::sin(yaw * look));
  e = std::clamp(e + pitch * look, -1.45f, 1.45f);
  g_axes[2] = Unit(h * std::cos(e) + g_wup * std::sin(e));
  g_axes[0] = Unit(Cross(g_wup, g_axes[2])) * g_right_sign;
  g_axes[1] = Unit(Cross(g_axes[2], g_axes[0]));
}

// The free view into a render camera: V0 * T(-eye) * [axes].
void WriteView(uint8_t* p) {
  float out[4][4];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out[i][j] = g_v0[i][0] * g_axes[j].x + g_v0[i][1] * g_axes[j].y + g_v0[i][2] * g_axes[j].z;
  const V3 t{g_v0[3][0] - g_eye.x, g_v0[3][1] - g_eye.y, g_v0[3][2] - g_eye.z};
  for (int j = 0; j < 3; ++j) out[3][j] = Dot(t, g_axes[j]);
  for (int i = 0; i < 4; ++i) out[i][0] *= g_zoom, out[i][1] *= g_zoom;  // (the zoom)
  for (int i = 0; i < 3; ++i) out[i][3] = g_v0[i][3];
  out[3][3] = g_v0[3][3];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) WrF(p + 16 * i + 4 * j, out[i][j]);
}

void Notice(const char* text) { ShowNotice(text, 3.0f); }

// MATCH CAMERA (free_cam.h): the preset, and the eye / direction put back after the view is built.
std::atomic<int> g_preset{-1};  // (-1: from the setting at first use)
// (RAMP: the fighters' sphere the match camera frames - centre, radius; radius < 0 none yet)
std::atomic<float> g_sx{0}, g_sy{0}, g_sz{0}, g_sr{-1};
thread_local bool g_follow = false;  // (inside the follow camera's update: sub_822285F0's hook)
struct Saved {
  bool active = false;
  uint32_t eye = 0, dir = 0;
  float e[3] = {}, d[3] = {};
} g_saved;
constexpr uint32_t kLive = 0x82E3DE00;  // (+64 the match's arena)
constexpr uint32_t kFollowBuild = 0x82228894;  // (sub_822285F0's call of sub_82227150)
constexpr uint32_t kBackstageArena = 78;

void Hotkey(uint8_t vk) {
  std::lock_guard lock(g_mutex);
  switch (vk) {
    case 0x73:  // F4: the next MATCH CAMERA
      CycleMatchCamera();
      return;
    case 0x75:  // F6
      if (g_mode == Mode::kOff) {
        g_capture = true;
        Notice("Free camera: W A S D, Q E, arrows, [ ] (or a controller) - F6 keeps it there, F5 back");
      } else if (g_mode == Mode::kFlying) {
        g_mode = Mode::kFixed;
        Notice("Free camera kept here - F6 moves it again, F5 back to the game's camera");
      } else {
        g_mode = Mode::kFlying;
        g_last_tick = Clock::now();
        Notice("Free camera: flying");
      }
      REXLOG_INFO("[svr2011] free camera: F6 ({})",
                  g_capture ? "taking the game's view" : g_mode == Mode::kFixed ? "kept" : "flying");
      break;
    case 0x74:  // F5
      if (g_mode != Mode::kOff || g_capture) {
        g_mode = Mode::kOff, g_capture = false, g_paused = false;
        Notice("The game's camera");
        REXLOG_INFO("[svr2011] free camera: off");
      }
      break;
    case 0x76:  // F7 (in a match only: it is for pictures - and cleared when the match ends)
      if (!InMatch()) break;
      g_hide_ui = !g_hide_ui;
      Notice(g_hide_ui ? "UI hidden - F7 shows it (START shows the pause menu)" : "UI shown");
      REXLOG_INFO("[svr2011] free camera: UI {}", g_hide_ui ? "hidden" : "shown");
      break;
    case 0x77:  // F8
      if (g_mode == Mode::kOff) break;
      if (Lockstep()) {
        Notice("No pause in an online match");
        break;
      }
      g_paused = !g_paused;
      REXLOG_INFO("[svr2011] free camera: match {}", g_paused ? "paused" : "running");
      break;
    default:
      break;
  }
}

bool FlyKey(uint32_t vk) {
  switch (vk) {
    case 'W': case 'A': case 'S': case 'D': case 'Q': case 'E':
    case 0x25: case 0x26: case 0x27: case 0x28:
    case 0x10: case 0x11: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xDB: case 0xDD:
      return true;
    default:
      return false;
  }
}

// Ahead of the keyboard's controller emulation and the binds (F7 is the
// SDK's achievements overlay otherwise). Window keys only: the focus.
class FreeCamListener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint32_t vk = uint32_t(e.virtual_key());
    if (vk >= 256 || GameKeyboardOpen()) return;
    g_keyboard_used = true;
    if (vk >= 0x73 && vk <= 0x77) {
      if (!e.is_ctrl_pressed() && !e.is_alt_pressed()) {
        Hotkey(uint8_t(vk));
        e.set_handled(true);
      }
      return;
    }
    std::lock_guard lock(g_mutex);
    g_held[vk] = true;
    if (g_mode == Mode::kFlying && FlyKey(vk)) e.set_handled(true);  // (the players stand still)
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const uint32_t vk = uint32_t(e.virtual_key());
    if (vk >= 256) return;
    std::lock_guard lock(g_mutex);
    g_held[vk] = false;
    if (g_mode == Mode::kFlying && FlyKey(vk)) e.set_handled(true);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {  // (no key-ups come then)
    std::lock_guard lock(g_mutex);
    std::fill(std::begin(g_held), std::end(g_held), false);
  }
};

// The scene's view as the game built it (eye, direction: its arguments).
void OnSceneView(uint8_t* base, uint32_t eye, uint32_t dir) {
  std::lock_guard lock(g_mutex);
  uint8_t* view = base + kSceneView;
  static const bool probe = std::getenv("SVR2011_TEST_FREECAM_PROBE") != nullptr;
  static uint32_t calls = 0;
  if (probe && calls++ % 300 == 0) {
    std::string s;
    for (uint32_t k = 0; k < 16; ++k) {
      char c[24];
      std::snprintf(c, sizeof c, "%s%.3f", k % 4 ? " " : k ? " | " : "", RdF(view + 4 * k));
      s += c;
    }
    std::string after;
    for (uint32_t k = 16; k < 64; ++k) {
      char c[24];
      std::snprintf(c, sizeof c, " %.3f", RdF(view + 4 * k));
      after += c;
    }
    REXLOG_INFO("[svr2011] free camera probe: after the view{}", after);
    REXLOG_INFO("[svr2011] free camera probe: view [{}] eye ({:.2f}, {:.2f}, {:.2f}) direction ({:.3f}, {:.3f}, {:.3f})",
                s, eye ? RdF(base + eye) : 0.f, eye ? RdF(base + eye + 4) : 0.f, eye ? RdF(base + eye + 8) : 0.f,
                dir ? RdF(base + dir) : 0.f, dir ? RdF(base + dir + 4) : 0.f, dir ? RdF(base + dir + 8) : 0.f);
  }
  if (g_capture) {
    g_capture = false;
    ReadView(view, g_v0);
    g_eye = {};
    g_axes[0] = {1, 0, 0}, g_axes[1] = {0, 1, 0}, g_axes[2] = {0, 0, 1};
    g_wup = Unit(V3{-g_v0[1][0], -g_v0[1][1], -g_v0[1][2]});
    g_zoom = 1;
    g_right_sign = Cross(g_wup, V3{0, 0, 1}).x >= 0 ? 1.0f : -1.0f;
    // (a quarter of the way to the world's origin - the ring - a second)
    const float dist = std::sqrt(g_v0[3][0] * g_v0[3][0] + g_v0[3][1] * g_v0[3][1] + g_v0[3][2] * g_v0[3][2]);
    g_speed = std::clamp(dist * 0.25f, 2.0f, 500.0f);
    g_last_tick = Clock::now();
    g_mode = Mode::kFlying;
    REXLOG_INFO("[svr2011] free camera: on - the scene's view, {:.1f} from the origin, speed {:.1f}", dist, g_speed);
  }
  if (g_mode == Mode::kOff) return;
  const auto now = Clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - g_last_tick).count(), 0.0f, 0.1f);
  g_last_tick = now;
  if (g_mode == Mode::kFlying) Fly(dt);
  WriteView(view);
}

}  // namespace

void InstallFreeCam(rex::ui::Window* window) {
  if (!window) return;
  static FreeCamListener listener;
  window->app_context().CallInUIThread([window] {
    window->AddInputListener(&listener, 1100);
    window->AddListener(&listener);
  });
}

void FreeCamSceneView(uint8_t* base, uint32_t view, uint32_t eye, uint32_t dir) {
  if (view == kSceneView) OnSceneView(base, eye, dir);
}

void FreeCamFrame(uint8_t* base) {
  static bool hinted = false;
  std::lock_guard lock(g_mutex);
  // The keys, briefly, once a session: in a match, for someone playing on the keyboard (a pad or touch player
  // never presses one, so never sees it; not on Android). While the camera flies, its keys stay up (not with
  // the UI hidden: F7 is for pictures).
#if !defined(__ANDROID__)
  if (!hinted && g_keyboard_used.load() && InMatch() && Rd32(base + kMatchFrames) > 180) {
    hinted = true;
    ShowNotice("Free camera: F6   back: F5   hide UI: F7", 4.0f);
  }
#else
  (void)hinted;
#endif
  // Out of the match (its end, a quit): the free camera off and the UI shown - nothing carries into the next one.
  static bool was_in = false;
  const bool in = InMatch();
  if (was_in && !in && (g_mode != Mode::kOff || g_capture || g_paused || g_hide_ui.load())) {
    g_mode = Mode::kOff, g_capture = false, g_paused = false, g_hide_ui = false;
    REXLOG_INFO("[svr2011] free camera: the match is over - off, UI shown");
  }
  was_in = in;
  // The match's clock standing still for half a second, not by F8 (and past its first frames: the entrances
  // count from 0): the game's pause menu or a replay - its UI shown (UiHidden), a flying camera kept where it is
  // so the keys work the menu again (F6 flies it again).
  static uint32_t last = 0;
  static int still = 0;
  const uint32_t clock = in ? Rd32(base + kMatchFrames) : 0;
  still = in && clock > 60 && clock == last && !g_paused ? still + 1 : 0;
  last = clock;
  g_clock_still = still >= 30;
  if (g_clock_still.load() && g_mode == Mode::kFlying) {
    g_mode = Mode::kFixed;
    Notice("Free camera kept here - the menu has the keys (F6 flies again, F5 back)");
    REXLOG_INFO("[svr2011] free camera: kept - the match's clock stopped (a menu)");
  }
#if !defined(__ANDROID__)
  if (g_mode == Mode::kFlying && !g_hide_ui.load())
    ShowNotice("Free camera: W A S D  Q E  arrows  [ ]   F6 keep   F5 back   F7 hide UI", 0.5f);
#endif
  // (F8 holds the camera too: without world updates nothing builds the view - written here it isn't drawn.
  // Frame the shot, then F8.)
}

void FreeCamPad(uint32_t user, uint8_t* s) {
  if (InMatch() && user < 8) {
    static bool r3_was[8] = {};
    const uint16_t b = uint16_t(s[4] << 8 | s[5]);
    const bool r3 = (b & 0x0020) && (b & 0x0080);  // (BACK + R3)
    if (r3 && !r3_was[user]) CycleMatchCamera();
    r3_was[user] = r3;
    if (b & 0x0020) s[5] = uint8_t(s[5] & ~0x80);  // (R3 with BACK held: the camera's, not SUBMIT)
  }
  std::lock_guard lock(g_mutex);
  if (user == 0) g_lx = g_ly = g_rx = g_ry = g_lt = g_rt = 0, g_lb = g_rb = false;
  if (g_mode != Mode::kFlying) return;
  const auto axis = [](const uint8_t* p) {
    const float v = float(int16_t(uint16_t(p[0] << 8 | p[1]))) / 32767.0f;
    return std::fabs(v) < 0.25f ? 0.0f : std::clamp((v - std::copysign(0.25f, v)) / 0.75f, -1.0f, 1.0f);
  };
  const uint16_t buttons = uint16_t(s[4] << 8 | s[5]);
  g_lx += axis(s + 8), g_ly += axis(s + 10), g_rx += axis(s + 12), g_ry += axis(s + 14);
  g_lt += s[6] / 255.0f, g_rt += s[7] / 255.0f;
  g_lb = g_lb || (buttons & 0x0100), g_rb = g_rb || (buttons & 0x0200);
  // (the game: nothing pressed, the sticks centred - but START and BACK: the pause menu opens while the camera
  // flies, and then it stops flying - FreeCamFrame; 2.1.8 report 176c9a "couldn't exit the match")
  std::memset(s + 4, 0, 12);
  s[5] = uint8_t(buttons & 0x30);
}

bool FreeCamPaused() { return g_paused; }

void MatchCameraSphere(float x, float y, float z, float radius) { g_sx = x, g_sy = y, g_sz = z, g_sr = radius; }

int MatchCamera() {
  int p = g_preset.load();
  if (p < 0) g_preset = p = std::clamp(REXCVAR_GET(match_camera), 0, kMatchCameras - 1);
  return p;
}

const char* MatchCameraName(int preset) {
  static const char* const kNames[kMatchCameras] = {"NORMAL", "FAR", "CLOSE", "HIGH", "LOW", "RAMP"};
  return kNames[std::clamp(preset, 0, kMatchCameras - 1)];
}

void SetMatchCamera(int preset, bool save) {
  preset = (preset % kMatchCameras + kMatchCameras) % kMatchCameras;
  g_preset = preset;
  rex::cvar::SetFlagByName("match_camera", std::to_string(preset));
  if (save) SaveConfigSetting("match_camera", std::to_string(preset));
}

void CycleMatchCamera() {
  const int p = (MatchCamera() + 1) % kMatchCameras;
  SetMatchCamera(p, true);
  REXLOG_INFO("[svr2011] match camera: {}", MatchCameraName(p));
  if (InMatch()) ShowNotice(std::string("CAMERA: ") + MatchCameraName(p) + "   (BACK + R3 / F4)", 2.0f);
}

void MatchCameraBefore(uint8_t* base, uint32_t lr, uint32_t view, uint32_t eye, uint32_t dir) {
  g_saved.active = false;
  static const bool lr_log = std::getenv("SVR2011_TEST_CAM_LR") != nullptr;  // (test aid: who builds the view)
  if (lr_log && view == kSceneView && InMatch()) {
    static uint32_t seen[32] = {};
    static int n = 0;
    bool found = false;
    for (int i = 0; i < n; ++i) found = found || seen[i] == lr;
    if (!found && n < 32) {
      seen[n++] = lr;
      REXLOG_INFO("[svr2011] match camera: the view built from {:08X}", lr);
    }
  }
  const int p = MatchCamera();
  if (!p || view != kSceneView || !eye || !dir || lr != kFollowBuild || !g_follow || !InMatch()) return;
  if (Rd32(base + kLive + 64) == kBackstageArena) return;
  {
    std::lock_guard lock(g_mutex);
    if (g_mode != Mode::kOff || g_capture) return;  // (the free camera's)
  }
  const V3 e{RdF(base + eye), RdF(base + eye + 4), RdF(base + eye + 8)};
  const V3 d = Unit(V3{RdF(base + dir), RdF(base + dir + 4), RdF(base + dir + 8)});
  // What it looks at: the ring's centre along its direction (at least 30 ahead).
  const float dist = std::max(30.0f, -Dot(e, d));
  const V3 target = e + d * dist;
  V3 ne = e;
  switch (p) {
    case 1: ne = target + d * (-dist * 1.45f); break;   // FAR
    case 2: ne = target + d * (-dist * 0.65f); break;   // CLOSE
    case 3: ne.y = e.y - dist * 0.45f; break;           // HIGH (-Y is up)
    case 4: ne.y = std::min(e.y + dist * 0.25f, -14.0f); break;  // LOW (kept above the ring: the mat is at -12)
  }
  V3 nd = Unit(target + ne * -1.0f);
  if (p == 5 && g_sr.load() >= 0) {
    // RAMP: the Royal Rumble's camera (sub_82331980) - its direction locked, looking from the hard-camera side
    // toward the ramp and stage (+z), a little down; its eye follows the fighters (their sphere's centre) from
    // a distance that keeps them all in the shot. Any arena: the ring's at the origin in every one.
    const V3 c{g_sx.load(), g_sy.load(), g_sz.load()};
    nd = Unit(V3{0.0f, 0.3f, 0.95f});
    const float back = std::clamp(g_sr.load() * 2.5f + 80.0f, 95.0f, 180.0f);  // (the Rumble's: about 95 away)
    ne = c + nd * -back;
  }
  static int logged_preset = -1;
  if (p != logged_preset) {
    logged_preset = p;
    REXLOG_INFO("[svr2011] match camera {}: eye ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}), {:.1f} from what it looks at",
                MatchCameraName(p), e.x, e.y, e.z, ne.x, ne.y, ne.z, dist);
  }
  g_saved = {true, eye, dir, {e.x, e.y, e.z}, {RdF(base + dir), RdF(base + dir + 4), RdF(base + dir + 8)}};
  WrF(base + eye, ne.x), WrF(base + eye + 4, ne.y), WrF(base + eye + 8, ne.z);
  WrF(base + dir, nd.x), WrF(base + dir + 4, nd.y), WrF(base + dir + 8, nd.z);
}

void MatchCameraAfter(uint8_t* base) {
  if (!g_saved.active) return;
  g_saved.active = false;  // (the camera's own eye and direction back: its smoothing goes on from them)
  for (int i = 0; i < 3; ++i) WrF(base + g_saved.eye + 4 * i, g_saved.e[i]), WrF(base + g_saved.dir + 4 * i, g_saved.d[i]);
}

bool UiHidden() { return g_hide_ui.load() && !g_clock_still.load(); }

bool FreeCamScriptKey(const char* name, int ms) {
  static const struct {
    const char* name;
    uint8_t vk;
  } kKeys[] = {{"F4", 0x73},   {"F5", 0x74},   {"F6", 0x75},   {"F7", 0x76},   {"F8", 0x77},   {"W", 'W'},      {"A", 'A'},
               {"S", 'S'},     {"D", 'D'},     {"Q", 'Q'},     {"E", 'E'},     {"UP", 0x26},    {"DOWN", 0x28},
               {"LEFT", 0x25}, {"RIGHT", 0x27}, {"SHIFT", 0x10}, {"CTRL", 0x11}, {"[", 0xDB},    {"]", 0xDD}};
  for (const auto& k : kKeys)
    if (std::strcmp(name, k.name) == 0) {
      g_keyboard_used = true;  // (a key, as from the window)
      if (k.vk >= 0x73 && k.vk <= 0x77) {
        Hotkey(k.vk);
      } else {
        std::lock_guard lock(g_mutex);
        g_script_until[k.vk] = Clock::now() + std::chrono::milliseconds(std::max(ms, 1));
      }
      return true;
    }
  return false;
}

}  // namespace svr2011




// The match camera's frame (free_cam.h): sub_822285F0(camera), the in-match cameras' view update (one class,
// vtable 0x82005160), called from 0x82398B10 for the follow camera while the camera mode (0x82E3CCF8) is 1 -
// during play; the entrances are mode 6, and the big-move cuts come through sub_822280B0 (from 0x82228C2C).
REX_EXTERN(__imp__sub_822285F0);
REX_HOOK_RAW(sub_822285F0) {
  svr2011::g_follow = uint32_t(ctx.lr) == 0x82398B10 && svr2011::Rd32(base + 0x82E3CCF8) == 1;
  __imp__sub_822285F0(ctx, base);
  svr2011::g_follow = false;
}
