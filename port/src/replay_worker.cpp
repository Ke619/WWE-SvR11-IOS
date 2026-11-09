// WWE SmackDown vs. Raw 2011 - the replay worker never loses a command.
//
// The replay system's worker thread (sub_82167A28, its object in r31) waits
// on a work event (*(obj+56), sub_8215A8C0), takes ONE command from its queue
// (head +172, count +176, up to 2), runs it, sets the command's result slot
// and goes back to waiting. The submit (sub_82167458) queues a command and
// sets the event - but an event is a flag, not a counter: two submits before
// the worker wakes leave one wake for two commands, and the second waits
// forever. On the 360 (and on PC) the worker wakes before a second submit; on
// a phone (and a Steam Deck) it can be slower, and a won match never ended -
// the match end waits for that command's result (a Steel Cage escape, ...).
// So the worker's wait returns at once while its queue still holds commands,
// as a counting semaphore would.

#include "caller_nv.h"
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

namespace {
constexpr uint32_t kWorkerWaitReturn = 0x82167A7C;  // (after the worker's bl sub_8215A8C0)
constexpr uint32_t kQueueCount = 176;               // worker object: commands queued

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}  // namespace

// sub_8215A8C0(handle, timeout): wait for an object (0 = signaled).
REX_EXTERN(__imp__sub_8215A8C0);
REX_HOOK_RAW(sub_8215A8C0) {
  if (uint32_t(ctx.lr) == kWorkerWaitReturn && SVR_CALLER_NV(ctx, r31) &&
      int32_t(Rd32(base + SVR_CALLER_NV(ctx, r31) + kQueueCount)) > 0) {
    static uint32_t logged = 0;
    if (logged++ < 8) {
      REXLOG_INFO("[svr2011] replay worker: {} command(s) still queued - running them without waiting",
                  Rd32(base + SVR_CALLER_NV(ctx, r31) + kQueueCount));
    }
    ctx.r3.u64 = 0;  // (signaled)
    return;
  }
  __imp__sub_8215A8C0(ctx, base);
}
