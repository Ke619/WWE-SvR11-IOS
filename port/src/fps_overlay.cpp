// WWE SmackDown vs. Raw 2011 - on-screen FPS counter (see fps_overlay.h).

#include "fps_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "frame_rate.h"
#include "frame_stats.h"
#include "free_cam.h"
#include "native/native_renderer.h"
#include "net_sim.h"
#include "perf_stats.h"
#include "touch_controls.h"

REXCVAR_DEFINE_BOOL(show_fps, true, "UI", "Show the frame rate counter (toggle in game: F2)");
REXCVAR_DEFINE_INT32(perf_overlay, -1, "UI",
                     "The performance overlay: 0 off, 1 the frame rate, 2 everything (RAM, VRAM, CPU, the renderer's "
                     "costs); -1 follows show_fps. F2 cycles it");
REXCVAR_DEFINE_INT32(perf_overlay_corner, 0, "UI",
                     "Where the performance overlay sits: 0 top left, 1 top right, 2 bottom left, 3 bottom right");
REXCVAR_DEFINE_BOOL(show_ping, true, "UI", "Online: show the round trip to the other players (top right)");

namespace svr2011 {

namespace {
FpsOverlay* g_overlay = nullptr;     // lives for the whole run
std::atomic<int> g_level{0};         // 0 off, 1 the frame rate, 2 everything
std::atomic<int> g_corner{0};
std::mutex g_notice_mutex;
std::string g_notice;
std::chrono::steady_clock::time_point g_notice_until;
}

static void DrawNotice(ImGuiIO& io) {
  std::string text;
  {
    std::lock_guard lock(g_notice_mutex);
    if (g_notice.empty()) return;
    if (std::chrono::steady_clock::now() > g_notice_until) {
      g_notice.clear();
      return;
    }
    text = g_notice;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.85f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##svr2011_notice", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.4f);
    // (a long notice wraps instead of running off the screen: "Game files missing: ..." on a phone lost its end)
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.88f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
}

// "Preparing graphics": the native renderer building the known pipelines
// ahead, at the bottom right while in the menus (hidden in matches, where it
// waits).
static void DrawPipelineProgress(ImGuiIO& io) {
#ifndef SVR2011_D3D_TRACE
  uint32_t done = 0, total = 0;
  bool paused = false;
  if (!native::PreparingPipelines(&done, &total, &paused) || paused || !total) return;
  const float pad = 16.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, io.DisplaySize.y - pad), ImGuiCond_Always,
                          ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##svr2011_pipelines", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted("Preparing graphics");
    char label[32];
    std::snprintf(label, sizeof(label), "%u / %u", done, total);
    ImGui::ProgressBar(float(done) / float(total), ImVec2(240.0f, 0.0f), label);
  }
  ImGui::End();
#else
  (void)io;
#endif
}

// The native renderer couldn't start or stopped: the game has no other way to
// draw, so the reason, plainly, in the middle of the (black) screen.
static void DrawNativeFailure(ImGuiIO& io) {
#ifndef SVR2011_D3D_TRACE
  const std::string reason = native::FailureReason();
  if (reason.empty()) return;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.7f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.85f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
  // (missing files: not the device - a driver or another graphics API won't help)
  const bool files_missing = reason.rfind("its shaders are missing", 0) == 0;
  if (ImGui::Begin("##svr2011_native_failed", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextWrapped(files_missing ? "Some of the game's files are missing" : "The game's graphics can't run on this device");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("Reason: %s.", reason.c_str());
    ImGui::Spacing();
    if (files_missing) {
#if defined(__ANDROID__)
      ImGui::TextWrapped("Reinstall the game with the launcher (its native_shaders folder), and please send a problem "
                         "report (launcher: Play tab, Report a problem).");
#else
      ImGui::TextWrapped("Extract the release zip again into a plain folder such as C:\\Games\\SvR2011 (not Pictures, "
                         "Documents, Desktop or OneDrive), check that its native_shaders folder has shaders.dxil.pak, "
                         "and start the game from the launcher there. Your antivirus may also have removed the file.");
#endif
    } else {
#if defined(__ANDROID__)
    ImGui::TextWrapped("Try another graphics driver (launcher: Settings, Graphics driver), and please send a problem "
                       "report (launcher: Play tab, Report a problem).");
#else
    ImGui::TextWrapped("Update your graphics driver, or try another graphics API - DIRECT3D 11 for older GPUs (MY WWE > "
                       "Options > Graphics, or the launcher's Settings) - and please send a problem report (the "
                       "launcher's Report a problem).");
#endif
    }
  }
  ImGui::End();
#else
  (void)io;
#endif
}

// Online: the worst round trip to the other players (net_sim.h), as signal
// bars and milliseconds at the top right, with the path it took (direct, IPv6,
// LAN, relay) - only while peers answer pings.
static void DrawPing(ImGuiIO& io) {
  if (!REXCVAR_GET(show_ping)) return;
  const auto links = NetLinks();
  if (links.empty()) return;
  float worst = 0;
  const char* how = "";
  for (const auto& l : links) {
    if (l.rtt_ms >= worst) worst = l.rtt_ms, how = l.how;
  }
  const int bars = worst < 80 ? 4 : worst < 150 ? 3 : worst < 250 ? 2 : 1;
  const ImVec4 colour = bars >= 3 ? ImVec4(0.45f, 1.0f, 0.45f, 1.0f)
                        : bars == 2 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                    : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 6.0f, 6.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.45f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##svr2011_ping", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.2f);
    // (the bars: four, rising, as many lit as the link is good)
    const float h = ImGui::GetTextLineHeight(), w = h * 0.22f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int k = 0; k < 4; ++k) {
      const float top = at.y + h * (1.0f - 0.25f * float(k + 1));
      const ImVec2 a(at.x + k * (w + 2.0f), top), b(a.x + w, at.y + h);
      draw->AddRectFilled(a, b, k < bars ? ImGui::GetColorU32(colour) : IM_COL32(110, 110, 110, 160));
    }
    ImGui::Dummy(ImVec2(4 * (w + 2.0f), h));
    ImGui::SameLine();
    ImGui::TextColored(colour, "%.0f ms (%s)", worst, how);
    // (outside a match - the lobby - or with more than one peer: every
    // player and their own ping, so players can see who is far)
    if (links.size() > 1 || !svr2011::InMatch()) {
      ImGui::SetWindowFontScale(1.0f);
      for (const auto& l : links) {
        const ImVec4 c = l.rtt_ms < 150 ? ImVec4(0.45f, 1.0f, 0.45f, 1.0f)
                         : l.rtt_ms < 250 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                          : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        const std::string who = l.name.empty() ? "Player" : l.name;
        ImGui::TextColored(c, "%s  %.0f ms (%s)", who.c_str(), l.rtt_ms, l.how);
      }
    }
  }
  ImGui::End();
}

void FpsOverlay::OnDraw(ImGuiIO& io) {
  DrawNativeFailure(io);
  DrawPipelineProgress(io);
  if (UiHidden()) return;  // (free camera F7: free_cam.h)
  DrawNotice(io);
  DrawPing(io);
  const int level = g_level.load(std::memory_order_relaxed);
  if (!level) return;
  const FrameTiming t = GetFrameTiming();
  // A corner of the window (perf_overlay_corner): out of the game's own HUD in the middle.
  const int corner = g_corner.load(std::memory_order_relaxed);
  const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
  const float pad = 6.0f;
  // (top right: under the online ping when it shows)
  const float ping_h = right && !bottom && REXCVAR_GET(show_ping) && !NetLinks().empty() ? 34.0f : 0.0f;
  // (the touch controls' buttons in that corner - MENU, EDIT: beside them, not under them; the window's height
  // from the last frame)
  static float last_h = 0;
  const float y0 = bottom ? io.DisplaySize.y - pad - last_h : pad + ping_h;
  const float side = pad + TouchControlsSideInset(right, y0, y0 + last_h);
  ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - side : side, bottom ? io.DisplaySize.y - pad : pad + ping_h),
                          ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
  ImGui::SetNextWindowBgAlpha(level >= 2 ? 0.6f : 0.45f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
  // (the window's size: ImGui's font is small on a 1440p / 4K window - 720p is 1x)
  const float k = std::max(1.0f, io.DisplaySize.y / 720.0f);
  if (ImGui::Begin("##svr2011_fps", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.5f * k);
    // Green at the frame rate the game draws (frame_rate.h), yellow from 3/4 of it.
    const double cap = FrameRateNow();
    const ImVec4 colour = t.fps >= cap * 0.97   ? ImVec4(0.45f, 1.0f, 0.45f, 1.0f)
                          : t.fps >= cap * 0.75 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                                : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(colour, "FPS %.0f", t.fps);
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms  1%% low %.0f", t.frame_ms, t.low_1pct_fps);
#ifndef SVR2011_D3D_TRACE  // the census build has no native renderer
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s", native::RendererLabel().c_str());
#endif
#ifndef SVR2011_D3D_TRACE
    if (level >= 2) {  // (the game's own GPU memory: the renderer's textures and render targets - the system's
                       // numbers, RAM / CPU / the driver's budget, go to the perf: log lines, perf_stats.h)
      const native::FrameCosts c = native::LastFrameCosts();
      ImGui::SetWindowFontScale(1.15f * k);
      const ImVec4 dim(0.85f, 0.85f, 0.85f, 1.0f);
      ImGui::TextColored(dim, "Game VRAM %.0f MB", c.texture_mb + c.targets_mb);
      ImGui::TextColored(dim, "Textures %.0f MB (%u)   render targets %.0f MB", c.texture_mb, c.textures, c.targets_mb);
    }
#endif
    last_h = ImGui::GetWindowHeight();
  }
  ImGui::End();
}

void InstallFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  const int level = REXCVAR_GET(perf_overlay);
  g_level = level >= 0 ? std::min(level, 2) : REXCVAR_GET(show_fps) ? 1 : 0;
  g_corner = std::clamp(REXCVAR_GET(perf_overlay_corner), 0, 3);
  g_overlay = new FpsOverlay(drawer);
  rex::ui::RegisterBind("bind_fps_counter", "F2", "Performance overlay: off, the frame rate, everything",
                        [] { SetPerfOverlayLevel((PerfOverlayLevel() + 1) % 3); });
}

void SetFpsCounterVisible(bool visible) { g_level = visible ? std::max(1, g_level.load()) : 0; }

bool FpsCounterVisible() { return g_level.load() > 0; }

void SetPerfOverlayLevel(int level) { g_level = std::clamp(level, 0, 2); }
int PerfOverlayLevel() { return g_level.load(); }
void SetPerfOverlayCorner(int corner) { g_corner = std::clamp(corner, 0, 3); }
int PerfOverlayCorner() { return g_corner.load(); }

void ShowNotice(const std::string& text, float seconds) {
  std::lock_guard lock(g_notice_mutex);
  // (logged when it comes up - not again while the same text is still showing: hidden tests and players' report logs
  // see what the overlay said; frame shots don't hold the overlay)
  if (text != g_notice || std::chrono::steady_clock::now() > g_notice_until)
    REXLOG_INFO("[svr2011] notice on screen: {}", text);
  g_notice = text;
  g_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
}

}  // namespace svr2011
