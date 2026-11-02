// The port's match HUD - see match_hud.h.
#include "match_hud.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstring>

#include <imgui.h>

#include "frame_rate.h"
#include "util.h"

namespace {

constexpr uint32_t kMatchFrames = 0x82E3CD0C;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

using svr2011::NowMs;

std::atomic<uint32_t> g_frames{0};
std::atomic<int64_t> g_ticked_at{0};  // ms: the clock last went up
ImFont* g_menu = nullptr;
ImFont* g_title = nullptr;

// The game's colours: its menu buttons (black, white rim, the red of RAW),
// gold for titles.
constexpr ImU32 kPanel = IM_COL32(8, 8, 10, 225), kRim = IM_COL32(235, 235, 235, 255),
                kRed = IM_COL32(200, 22, 26, 255), kGold = IM_COL32(242, 196, 64, 255),
                kWhite = IM_COL32(255, 255, 255, 255), kShadow = IM_COL32(0, 0, 0, 200);

ImU32 Alpha(ImU32 c, float a) {
  const uint32_t al = uint32_t(float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

// The game's 16:9 area in the display (pillar / letter boxes left out) and its scale (720 lines = 1).
struct Area {
  float x, y, w, h, s;
};
Area GameArea(float dw, float dh) {
  const float w = std::min(dw, dh * 16.0f / 9.0f), h = w * 9.0f / 16.0f;
  return {(dw - w) * 0.5f, (dh - h) * 0.5f, w, h, h / 720.0f};
}

ImVec2 Size(ImFont* f, float size, const std::string& t) {
  return (f ? f : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str());
}
void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 at, ImU32 c, const std::string& t, float a = 1.0f) {
  if (!f) f = ImGui::GetFont();
  const float sh = std::max(1.0f, size * 0.06f);
  dl->AddText(f, size, ImVec2(at.x + sh, at.y + sh), Alpha(kShadow, a), t.c_str());
  dl->AddText(f, size, at, Alpha(c, a), t.c_str());
}

// A slanted panel (a parallelogram leaning right, as the game's buttons):
// the box a..b, `lean` the slant's width; a red edge on its left.
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float lean, float s, float alpha) {
  const ImVec2 p[4] = {ImVec2(a.x + lean, a.y), ImVec2(b.x + lean, a.y), ImVec2(b.x, b.y), ImVec2(a.x, b.y)};
  dl->AddConvexPolyFilled(p, 4, Alpha(kPanel, alpha));
  const float red = 7.0f * s;  // (the red edge)
  const ImVec2 r[4] = {p[0], ImVec2(p[0].x + red, a.y), ImVec2(p[3].x + red, b.y), p[3]};
  dl->AddConvexPolyFilled(r, 4, Alpha(kRed, alpha));
  dl->AddPolyline(p, 4, Alpha(kRim, alpha), ImDrawFlags_Closed, 2.0f * s);
}

}  // namespace

namespace svr2011 {

void MatchHudTick(uint8_t* base) {
  const uint32_t f = Rd32(base + kMatchFrames);
  if (f != g_frames.exchange(f) && f > 0) g_ticked_at = NowMs();
}

bool MatchHudVisible() { return InMatch() && g_frames.load() > 0 && NowMs() - g_ticked_at.load() < 300; }

void SetMatchHudFonts(ImFont* menu, ImFont* title) {
  g_menu = menu;
  g_title = title ? title : menu;
}

void DrawHudPanel(float dw, float dh, const std::string& left, const std::string& right) {
  const Area g = GameArea(dw, dh);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float size = 22.0f * g.s, gap = 18.0f * g.s;
  const ImVec2 lt = Size(g_title, size, left), rt = Size(g_title, size, right);
  const float pad_x = 26.0f * g.s, pad_y = 7.0f * g.s, lean = 12.0f * g.s;
  const float w = lt.x + gap + rt.x + pad_x * 2, h = std::max(lt.y, rt.y) + pad_y * 2;
  const ImVec2 a(g.x + (g.w - w) * 0.5f, g.y + g.h * 0.03f), b(a.x + w, a.y + h);
  Panel(dl, a, b, lean, g.s, 1.0f);
  Text(dl, g_title, size, ImVec2(a.x + pad_x + lean * 0.5f, a.y + pad_y), kGold, left);
  Text(dl, g_title, size, ImVec2(a.x + pad_x + lean * 0.5f + lt.x + gap, a.y + pad_y), kWhite, right);
}

void DrawHudBanner(float dw, float dh, const std::string& big, const std::string& sub, float t) {
  const Area g = GameArea(dw, dh);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // (slides in over the first 12%, holds, fades over the last 20%)
  const float in = std::min(1.0f, t / 0.12f), alpha = t > 0.8f ? (1.0f - t) / 0.2f : 1.0f;
  const float bs = 54.0f * g.s, ss = 26.0f * g.s;
  const ImVec2 bt = Size(g_title, bs, big), st = Size(g_title, ss, sub);
  const float w = std::max(bt.x, st.x) + 90.0f * g.s, h = bt.y + st.y + 30.0f * g.s, lean = 22.0f * g.s;
  const float slide = (1.0f - in) * (1.0f - in) * g.w * 0.6f;
  const ImVec2 a(g.x + (g.w - w) * 0.5f - slide, g.y + g.h * 0.36f), b(a.x + w, a.y + h);
  Panel(dl, a, b, lean, g.s, alpha * 0.95f);
  const float cx = (a.x + b.x + lean) * 0.5f;
  Text(dl, g_title, bs, ImVec2(cx - bt.x * 0.5f, a.y + 8.0f * g.s), kWhite, big, alpha);
  Text(dl, g_title, ss, ImVec2(cx - st.x * 0.5f, a.y + 14.0f * g.s + bt.y), kGold, sub, alpha);
}

HudRowHits DrawHudRow(float dw, float dh, float y, const std::string& label, const std::string& value,
                      const std::string& note, const std::string& legend) {
  const Area g = GameArea(dw, dh);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float size = 22.0f * g.s, small = 17.0f * g.s, lean = 12.0f * g.s;
  // (as wide as the game's CREATE SESSION box: x 0.16 .. 0.86 of the area)
  const ImVec2 a(g.x + g.w * 0.16f, g.y + g.h * y), b(g.x + g.w * 0.86f, a.y + size + 14.0f * g.s);
  Panel(dl, a, b, lean, g.s, 1.0f);
  const float ty = a.y + 7.0f * g.s;
  Text(dl, g_title, size, ImVec2(a.x + 26.0f * g.s, ty), kGold, label);
  // the value between arrows, centred in the right half
  const float vx0 = a.x + (b.x - a.x) * 0.42f, vx1 = b.x - 18.0f * g.s;
  const ImVec2 vs = Size(g_title, size, value);
  const float cx = (vx0 + vx1) * 0.5f;
  Text(dl, g_title, size, ImVec2(cx - vs.x * 0.5f, ty), kWhite, value);
  const float arrow = size * 0.42f, my = ty + size * 0.5f;
  const auto tri = [&](float x, bool right) {
    const ImVec2 p0(right ? x - arrow : x + arrow, my - arrow), p1(right ? x - arrow : x + arrow, my + arrow), p2(x, my);
    dl->AddTriangleFilled(p0, p1, p2, kWhite);
  };
  tri(vx0, false);
  tri(vx1, true);
  HudRowHits hits;
  const float pad = 10.0f * g.s;
  hits.left[0] = vx0 - pad, hits.left[1] = a.y, hits.left[2] = vx0 + arrow + pad * 2, hits.left[3] = b.y;
  hits.right[0] = vx1 - arrow - pad * 2, hits.right[1] = a.y, hits.right[2] = vx1 + pad, hits.right[3] = b.y;
  float ny = b.y + 6.0f * g.s;
  if (!note.empty()) {
    const ImVec2 ns = Size(g_menu, small, note);
    Text(dl, g_menu, small, ImVec2(g.x + (g.w - ns.x) * 0.5f, ny), kWhite, note);
    ny += ns.y + 4.0f * g.s;
  }
  if (!legend.empty()) {
    const ImVec2 ls = Size(g_menu, small, legend);
    Text(dl, g_menu, small, ImVec2(g.x + (g.w - ls.x) * 0.5f, ny), kGold, legend);
  }
  return hits;
}

}  // namespace svr2011
