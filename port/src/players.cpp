// WWE SmackDown vs. Raw 2011 - more local players (the PS3 version takes 6;
// the Xbox 360 one polls 4 pads).
//
// Input: sub_826CA070(input) polls the pads - its pad list at +4 (list+12
// the count: 8 pad objects exist; list+8 the pad objects), a 36-byte state
// per pad at +8 - each through XamInputGetState (sub_82905058); both its
// loops (and sub_826C9EA0's) stop at 4. Pads 5-8: the reader runs again
// over them - the list's pad array and the states moved on by 4, the user
// index too (PadUserOffset, in frame_rate.cpp's XamInputGetState hook).
// The SDK hands out 8 users (kMaxGuestUsers). Setting more_pads (on).

#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "players.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(more_pads, true, "Input", "Pads 5-8 too (the game reads 4; it has 8 pad objects)");

namespace {
using svrfmt::Rd32, svrfmt::Wr32;
uint32_t g_user_offset = 0;
}  // namespace

namespace svr2011 {
uint32_t PadUserOffset() { return g_user_offset; }
}  // namespace svr2011

// The pads are read: sub_826CA070(input).
REX_EXTERN(__imp__sub_826CA070);
REX_HOOK_RAW(sub_826CA070) {
  const auto saved = ctx;
  __imp__sub_826CA070(ctx, base);
  const bool pads8 = REXCVAR_GET(more_pads);
  const uint32_t input = saved.r3.u32, list = Rd32(base + input + 4);
  if (!pads8 || !list || Rd32(base + list + 12) <= 4) return;
  // Pads 5-8: the same reader over the list's pads 4-7 and their states.
  const uint32_t pads = Rd32(base + list + 8), states = Rd32(base + input + 8);
  Wr32(base + list + 8, pads + 4 * 4);
  Wr32(base + input + 8, states + 4 * 36);
  Wr32(base + list + 12, Rd32(base + list + 12) - 4);
  g_user_offset = 4;
  const auto after = ctx;
  ctx = saved;
  __imp__sub_826CA070(ctx, base);
  ctx = after;
  g_user_offset = 0;
  Wr32(base + list + 12, Rd32(base + list + 12) + 4);
  Wr32(base + input + 8, states);
  Wr32(base + list + 8, pads);
}
