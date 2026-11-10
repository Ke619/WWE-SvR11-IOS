// WWE SmackDown vs. Raw 2011 - title belts a bit bigger (title_belt_scale).
//
// The belts looked small on the superstars. They are models of their own:
// entrance / celebration / cut-scene belts in pac\evt\evtobj.pac (SOBJ/<id>,
// the JBOY named by its id: Stretched 2002-2010, 2050, 2305, 2308, 2512;
// Folded 2012-2020, 2051, 2306, 2309, 2513; Rolled 2022-2030, 2052, 2307,
// 2310, 2514, 2422, 2548; variants 3002-3513, 4002-4513, 5002-5024) and the
// belt one picks up in a match (bgEtc.pac STG/WPON/000e: the Folded ones,
// "2015_wwe_HC_2" too). No field scales a model, so each belt's model is
// scaled as the game builds it: sub_826B6BA8(src, type, size, offset, align)
// copies a JBOY into a fresh allocation and turns its offsets into pointers
// (one copy per instance, data still big-endian); then, before anything else
// reads it, every vertex position (28-byte vertices: position at +0), node
// translation (+16) and bounding sphere (mesh +0xA4, node +64) is multiplied
// - a uniform scale about the model's origin, the point the game places
// (hand, shoulder, waist).
//
// Except the Rolled ones (2022-2030, 2052, 2307, 2310, 2514, 2422, 2548): those
// are the belts worn ROUND THE WAIST (a Created Superstar's entrance, report
// f00a1a: the CAW's 2024, an ECW champion's 2307). Scaled about the waist,
// the loop was 20% wider than the body and stood off it, so they stay at x1.00.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_DOUBLE(title_belt_scale, 1.2, "Gameplay",
                      "Size of the title belts (1.0: as in the original game)");

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void ScaleF(uint8_t* p, float s) {
  uint32_t u = Rd32(p);
  float f;
  std::memcpy(&f, &u, 4);
  f *= s;
  std::memcpy(&u, &f, 4);
  p[0] = uint8_t(u >> 24), p[1] = uint8_t(u >> 16), p[2] = uint8_t(u >> 8), p[3] = uint8_t(u);
}

bool InRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

// A belt's object name: its id ("2014"), maybe with a suffix ("2015_wwe_HC_2").
bool IsBeltName(const char* n) {
  for (int i = 0; i < 4; ++i)
    if (n[i] < '0' || n[i] > '9') return false;
  if (n[4] != 0 && n[4] != '_') return false;
  const int id = std::atoi(std::string(n, 4).c_str());
  static const int kSingles[] = {2050, 2305, 2308, 2512, 2051, 2306, 2309, 2513,
                                 2052, 2307, 2310, 2514, 2422, 2548};
  for (int s : kSingles)
    if (id == s) return true;
  return InRange(id, 2002, 2010) || InRange(id, 2012, 2020) || InRange(id, 2022, 2030) ||
         InRange(id, 3002, 3513) || InRange(id, 4002, 4513) || InRange(id, 5002, 5024);
}

// A Rolled belt: worn round the waist (kept at its own size, above).
bool IsWaistBelt(const char* n) {
  const int id = std::atoi(std::string(n, 4).c_str());
  static const int kSingles[] = {2052, 2307, 2310, 2514, 2422, 2548};
  for (int s : kSingles)
    if (id == s) return true;
  return InRange(id, 2022, 2030);
}

}  // namespace

REX_EXTERN(__imp__sub_826B6BA8);
REX_HOOK_RAW(sub_826B6BA8) {
  const uint32_t src = ctx.r3.u32, off_ptr = ctx.r6.u32;
  const uint32_t chunk = src + (off_ptr ? Rd32(base + off_ptr) : 0);
  const bool jboy = src && !std::memcmp(base + chunk, "JBOY", 4);
  const uint32_t len = jboy ? Rd32(base + chunk + 4) : 0;
  __imp__sub_826B6BA8(ctx, base);
  const uint32_t res = ctx.r3.u32;
  if (!jboy || !res || len < 0x40) return;
  static const float s = float(REXCVAR_GET(title_belt_scale));
  if (s <= 0.0f || s == 1.0f) return;
  // a pointer of the built model (absolute; an offset if the fix-up left one)
  auto ptr = [&](uint32_t v) -> uint32_t {
    if (v >= res && v < res + len) return v;
    if (v && v < len) return res + v;
    return 0;
  };
  uint8_t* m = base + res;
  const uint32_t obj = ptr(Rd32(m + 0x28));
  if (!obj) return;
  char name[17] = {};
  std::memcpy(name, base + obj, 16);
  if (!IsBeltName(name)) return;
  if (IsWaistBelt(name)) {
    static std::mutex mu;
    static std::set<std::string> told;  // (one line per model)
    std::lock_guard<std::mutex> lock(mu);
    if (told.insert(std::string(name, 4)).second)
      REXLOG_INFO("[svr2011] title belts: {} worn on the waist - kept at x1.00", name);
    return;
  }
  const uint32_t nmesh = Rd32(m + 0x10), meshes = ptr(Rd32(m + 0x14));
  const uint32_t nnode = Rd32(m + 0x18), nodes = ptr(Rd32(m + 0x20));
  if (nmesh > 256 || nnode > 256 || (nmesh && !meshes) || (nnode && !nodes)) return;
  uint32_t verts = 0;
  for (uint32_t i = 0; i < nmesh; ++i) {
    uint8_t* d = base + meshes + 0xB4 * i;
    const uint32_t vc = Rd32(d);
    const uint32_t cell = ptr(Rd32(d + 0x68));
    const uint32_t vd = cell ? ptr(Rd32(base + cell)) : 0;
    if (vd && vc < 65536 && vd + 28ull * vc <= uint64_t(res) + len) {
      for (uint32_t k = 0; k < vc; ++k)
        for (int c = 0; c < 3; ++c) ScaleF(base + vd + 28 * k + 4 * c, s);
      verts += vc;
    }
    for (int c = 0; c < 4; ++c) ScaleF(d + 0xA4 + 4 * c, s);
  }
  for (uint32_t i = 0; i < nnode; ++i) {
    uint8_t* n = base + nodes + 80 * i;
    for (int c = 0; c < 3; ++c) ScaleF(n + 16 + 4 * c, s);
    for (int c = 0; c < 4; ++c) ScaleF(n + 64 + 4 * c, s);
  }
  static int logged = 0;
  if (logged++ < 24)
    REXLOG_INFO("[svr2011] title belts: {} x{:.2f} ({} meshes, {} vertices, {} nodes)", name, s, nmesh, verts, nnode);
}
