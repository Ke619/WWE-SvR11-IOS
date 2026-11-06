// Guest routines done natively on the Switch, where every cycle counts.
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc.h>

// The game's memcpy (8.5 KB of recompiled AltiVec, 6% of the game thread's busy time). It
// copies forwards and returns the destination, which stays in r3.
REX_HOOK_RAW(sub_8215A000) {
  if (ctx.r5.u32) std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
