// WWE SmackDown vs. Raw 2011 - tapping the game's own menus (first: finding where things are drawn).
//
// The menus are guest layouts: sub_823D1C48(layout, node, ...) draws a layout, sub_823CE528(layout, node, ..) a group
// node (it adds the node's offset and multiplies its scale into the layout's running transform, layout +144 / +160),
// and sub_823D4008(layout, element, ..) each 256-byte element. An element's descriptor is at +192 (+0 id, +4 type: 1
// text - sub_823D1E98, 2 picture); +196 non-zero: not drawn. Its place, as the text draw works it out:
//   x = e[+160] + L[+144] + e[+4] * L[+160],  y = e[+164] + L[+148] + e[+8] * L[+164]
// Test aid SVR2011_TEST_MENU_TOUCH_LOG=1: each new element drawn, once.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "native/native_renderer.h"
#include "script_input.h"

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// (a guest C string, for the log: printable only)
std::string GuestText(const uint8_t* base, uint32_t address) {
  std::string out;
  if (address < 0x10000) return out;
  for (uint32_t k = 0; k < 48; ++k) {
    const uint8_t c = base[address + k];
    if (!c) break;
    out += c >= 0x20 && c < 0x7F ? char(c) : '.';
  }
  return out;
}

const bool g_log = [] {
  const char* v = std::getenv("SVR2011_TEST_MENU_TOUCH_LOG");
  return v && *v == '1';
}();

void LogElement(const uint8_t* base, uint32_t layout, uint32_t element) {
  const uint8_t* L = base + layout;
  const uint8_t* e = base + element;
  const uint32_t desc = Rd32(e + 192);
  if (desc < 0x10000 || Rd32(e + 196)) return;
  const uint32_t id = Rd32(base + desc), type = Rd32(base + desc + 4);
  if (type != 1 && type != 2) return;
  const float x = RdF(e + 160) + RdF(L + 144) + RdF(e + 4) * RdF(L + 160);
  const float y = RdF(e + 164) + RdF(L + 148) + RdF(e + 8) * RdF(L + 164);
  static std::mutex mutex;
  static std::set<std::tuple<uint32_t, uint32_t, int, int>> seen;
  {
    std::lock_guard lock(mutex);
    if (seen.size() > 20000) return;
    if (!seen.insert({layout, element, int(x), int(y)}).second) return;
  }
  uint32_t group = 0, row = 0;
  svr2011::ScriptMenuState(&group, &row);
  REXLOG_INFO("[svr2011] menu touch: L {:08X} e {:08X} id {} type {} at {:.1f},{:.1f} scale {:.3f},{:.3f} e4 {:.1f},{:.1f} size {:.1f},{:.1f} / {:.1f},{:.1f} menu {:X}/{} \"{}\"",
              layout, element, id, type, x, y, RdF(L + 160), RdF(L + 164), RdF(e + 4), RdF(e + 8), RdF(e + 176), RdF(e + 180),
              RdF(e + 12), RdF(e + 16), group, row, type == 1 ? GuestText(base, Rd32(e + 216)) : "");
}

// Test aid SVR2011_TEST_MENU_TOUCH_SHOT=<path prefix>: a frame (PPM) 3 s after each menu group change - a hidden test
// game has no window to capture. Called on the draw thread.
void ShotAid() {
  static const char* prefix = std::getenv("SVR2011_TEST_MENU_TOUCH_SHOT");
  if (!prefix) return;
  static uint32_t shown = ~0u, pending = ~0u;
  static bool asked = false;
  static std::chrono::steady_clock::time_point since;
  uint32_t group = 0, row = 0;
  svr2011::ScriptMenuState(&group, &row);
  const auto now = std::chrono::steady_clock::now();
  if (group != pending) pending = group, since = now, asked = false;
  if (pending == shown) return;
  if (!asked && now - since > std::chrono::seconds(3)) {
    svr2011::native::RequestFrameCapture();
    asked = true;
  }
  std::vector<uint8_t> rgb;
  uint32_t w = 0, h = 0;
  if (asked && svr2011::native::TakeFrameCapture(rgb, w, h)) {
    shown = pending;
    const std::string file = std::string(prefix) + "_" + std::to_string(pending) + ".ppm";
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
      REXLOG_INFO("[svr2011] menu touch: frame {}x{} -> {}", w, h, file);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_823D4008);
REX_HOOK_RAW(sub_823D4008) {
  if (g_log) LogElement(base, ctx.r3.u32, ctx.r4.u32);
  ShotAid();
  __imp__sub_823D4008(ctx, base);
}
