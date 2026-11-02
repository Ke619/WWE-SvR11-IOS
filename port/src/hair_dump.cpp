// Test aid (SVR2011_TEST_HAIR_DUMP=<file>): the superstar hair solver's state ("gBaH", 2011's
// "Bane" spring hair - docs scratchpad re_bane.md) around its per-frame steps, for checking a
// reimplementation against the game. Only the character in slot 0. One text line per chain and
// step:
//   <frame> <step> <chain> <count> then per node: pos.xyz vel.xyz prev.xyz pinned
//   steps: A0 before step A (8216D618), A1 after it, B1 after step B (8216C6F0), C1 after step C
//   (8216A9E8); first "P <chain> <count> <16 rest lengths> lencorr visc grav spring damp radius bend
//   movecorr A"; after A1 also "S <frame> <n> x y z r ..." (the collision spheres) and
//   "D <frame> <chain> dir.xyz" (H+24128+16i, the head -Z the step-B kick compares against).
// The chain records: C = H + 768 + 1408 i (count +0, nodes +16 + 80 j: pos +0, vel +16, prev +48,
// pinned +64); chain count H+752; H+64 the character; spheres H+23312 (count H+24112).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

const char* const g_path = std::getenv("SVR2011_TEST_HAIR_DUMP");
std::mutex g_mutex;
FILE* g_file = nullptr;
uint32_t g_frame = 0;
constexpr uint32_t kChars = 0x82E3CC50;

using svrfmt::Rd32, svrfmt::RdF;

bool Wanted(uint8_t* base, uint32_t h) {
  if (!g_path || !h) return false;
  return Rd32(base + h + 64) != 0 && Rd32(base + h + 64) == Rd32(base + kChars);
}

void Dump(uint8_t* base, uint32_t h, const char* step) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) g_file = std::fopen(g_path, "w");
  if (!g_file) return;
  const uint32_t nchain = Rd32(base + h + 752);
  for (uint32_t i = 0; i < nchain && i < 16; ++i) {
    const uint32_t c = h + 768 + 1408 * i;
    const uint32_t count = Rd32(base + c);
    std::fprintf(g_file, "%u %s %u %u", g_frame, step, i, count);
    for (uint32_t j = 0; j < count && j < 16; ++j) {
      const uint32_t n = c + 16 + 80 * j;
      for (uint32_t at : {0u, 4u, 8u, 16u, 20u, 24u, 48u, 52u, 56u}) std::fprintf(g_file, " %.5f", RdF(base + n + at));
      std::fprintf(g_file, " %u", Rd32(base + n + 64));
    }
    std::fputc('\n', g_file);
  }
  if (g_frame == 0 && std::strcmp(step, "A0") == 0)
    for (uint32_t i = 0; i < nchain && i < 16; ++i) {
      // (once: the chain's rest lengths C+1296 + 4j and its parameters C+1360..1392)
      const uint32_t c = h + 768 + 1408 * i;
      std::fprintf(g_file, "P %u %u", i, Rd32(base + c));
      for (uint32_t j = 0; j < 16; ++j) std::fprintf(g_file, " %.5f", RdF(base + c + 1296 + 4 * j));
      for (uint32_t at = 1360; at < 1392; at += 4) std::fprintf(g_file, " %.5f", RdF(base + c + at));
      std::fprintf(g_file, " %u\n", Rd32(base + c + 1392));
    }
  if (std::strcmp(step, "A1") == 0) {
    const uint32_t ns = Rd32(base + h + 24112);
    std::fprintf(g_file, "S %u %u", g_frame, ns);
    for (uint32_t s = 0; s < ns && s < 70; ++s)
      for (uint32_t k = 0; k < 4; ++k) std::fprintf(g_file, " %.5f", RdF(base + h + 23312 + 16 * s + 4 * k));
    std::fputc('\n', g_file);
    for (uint32_t i = 0; i < nchain && i < 16; ++i)
      std::fprintf(g_file, "D %u %u %.5f %.5f %.5f\n", g_frame, i, RdF(base + h + 24128 + 16 * i),
                   RdF(base + h + 24132 + 16 * i), RdF(base + h + 24136 + 16 * i));
  }
  if (std::strcmp(step, "C1") == 0) {
    ++g_frame;
    if (g_frame % 60 == 0) std::fflush(g_file);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_8216D618);
REX_HOOK_RAW(sub_8216D618) {
  const uint32_t h = ctx.r3.u32;
  const bool want = Wanted(base, h);
  if (want) Dump(base, h, "A0");
  __imp__sub_8216D618(ctx, base);
  if (want) Dump(base, h, "A1");
}

REX_EXTERN(__imp__sub_8216C6F0);
REX_HOOK_RAW(sub_8216C6F0) {
  const uint32_t h = ctx.r3.u32;
  __imp__sub_8216C6F0(ctx, base);
  if (Wanted(base, h)) Dump(base, h, "B1");
}

REX_EXTERN(__imp__sub_8216A9E8);
REX_HOOK_RAW(sub_8216A9E8) {
  const uint32_t h = ctx.r3.u32;
  __imp__sub_8216A9E8(ctx, base);
  if (Wanted(base, h)) Dump(base, h, "C1");
}
