// WWE SmackDown vs. Raw 2011 - title matches outside Universe (docs/TITLE_MODS.md).
//
// The select screen already has them: state 10 asks "TITLE MATCH?" when the picked superstar holds a title, but its check
// (sub_8245DD48) only says yes for titles outside Universe's seven (sub_828BDA98).

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "generated/default/svr2011_init.h"
#include "p2p.h"

REX_EXTERN(__imp__sub_8245DD48);
REX_EXTERN(__imp__sub_82463090);
REX_EXTERN(__imp__sub_8243D680);

namespace {

constexpr uint32_t kMatchSetup = 0x82EDE630;  // (the select screen's setup; sub_827323B8 = a slot's record)

bool Trace() {
  static const bool on = std::getenv("SVR2011_TEST_EXH_TRACE") != nullptr;
  return on;
}

uint32_t Be32(const uint8_t* base, uint32_t at) {
  return uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 | uint32_t(base[at + 2]) << 8 | base[at + 3];
}

}  // namespace

// Any title the picked superstar holds can be asked about, not only those outside Universe's seven.
REX_HOOK_RAW(sub_8245DD48) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8245DD48(ctx, base);
  if (ctx.r3.u32 != 0 || svr2011::P2POnlineMode()) return;
  const uint32_t r = ctx.r3.u32;
  const auto saved = ctx;
  ctx.r3.u64 = Be32(base, kMatchSetup);
  sub_827323D8(ctx, base);
  const bool scramble = ctx.r3.u32 && Be32(base, ctx.r3.u32) == 38;  // (the title is the match itself)
  ctx = saved;
  if (scramble) return;
  ctx.r3.u64 = Be32(base, self + 272);
  sub_824499A0(ctx, base);
  const bool fixed = ctx.r3.u32 != 0;  // (Universe's own people)
  uint32_t count = 0;
  if (!fixed) {
    ctx.r3.u64 = Be32(base, kMatchSetup), ctx.r4.u64 = Be32(base, self + 284);
    sub_827323B8(ctx, base);
    count = ctx.r3.u32 ? Be32(base, ctx.r3.u32 + 124) : 0;
  }
  ctx = saved;
  ctx.r3.u64 = r;
  if (!fixed && count > 0) ctx.r3.u64 = 1;
  if (Trace()) REXLOG_INFO("[svr2011] exhibition titles: check fixed {} count {} -> {}", fixed, count, ctx.r3.u32);
}

REX_HOOK_RAW(sub_82463090) {
  // SVR2011_TEST_EXH_HOLDER=<title>,<id>[,<id>][;...]: who holds the title when the select screen first opens.
  static bool holder_set = false;
  if (const char* h = !holder_set ? std::getenv("SVR2011_TEST_EXH_HOLDER") : nullptr) {
    holder_set = true;
    for (; h && *h; h = std::strchr(h, ';') ? std::strchr(h, ';') + 1 : nullptr) {
      uint32_t title = 0, x = 512, y = 512;
      if (std::sscanf(h, "%u,%u,%u", &title, &x, &y) < 2) continue;
      const auto saved = ctx;
      ctx.r3.u64 = title, ctx.r4.u64 = x, ctx.r5.u64 = 0;
      sub_828BD5B8(ctx, base);
      ctx.r3.u64 = title, ctx.r4.u64 = y, ctx.r5.u64 = 1;
      sub_828BD5B8(ctx, base);
      ctx = saved;
    }
  }
  if (Trace()) {
    const uint32_t self = ctx.r3.u32, state = ctx.r4.u32;
    const auto saved = ctx;
    ctx.r3.u64 = Be32(base, kMatchSetup), ctx.r4.u64 = Be32(base, self + 284);
    uint32_t rec = 0;
    if (ctx.r3.u32) {
      sub_827323B8(ctx, base);
      rec = ctx.r3.u32;
    }
    ctx = saved;
    if (rec)
      REXLOG_INFO("[svr2011] exhibition titles: state {} slot {} person {} +96 {} +108 {} +124 {} titles {} {} stake {} {}", state,
                  Be32(base, self + 284), Be32(base, rec), Be32(base, rec + 96), Be32(base, rec + 108), Be32(base, rec + 124),
                  Be32(base, rec + 128), Be32(base, rec + 132), Be32(base, rec + 36), Be32(base, rec + 40));
    else
      REXLOG_INFO("[svr2011] exhibition titles: state {}", state);
  }
  // After the attire menu the game goes on to state 13 without asking (only a superstar with one attire reaches the check).
  static uint32_t last = 0;
  if (ctx.r4.u32 == 13 && last == 6 && !svr2011::P2POnlineMode()) {
    const auto saved = ctx;
    sub_8245DD48(ctx, base);
    const bool ask = ctx.r3.u32 == 1;
    ctx = saved;
    if (ask) {
      ctx.r4.u64 = 9;
      if (Trace()) REXLOG_INFO("[svr2011] exhibition titles: asked");
    }
  }
  last = ctx.r4.u32;
  __imp__sub_82463090(ctx, base);
}

// End of a match: the game's route for a staked title.
REX_HOOK_RAW(sub_8243D680) {
  const auto holder = [](PPCContext& c, uint8_t* b, uint32_t t) {
    const auto keep = c;
    c.r3.u64 = t, c.r4.u64 = 0, c.r5.u64 = 1;
    sub_828BD548(c, b);
    const uint32_t r = c.r3.u32;
    c = keep;
    return r;
  };
  const uint32_t self = ctx.r3.u32;
  const uint32_t h0 = Trace() ? holder(ctx, base, 0) : 0, h4 = Trace() ? holder(ctx, base, 4) : 0;
  const auto second = [](PPCContext& c, uint8_t* b, uint32_t t) {
    const auto keep = c;
    c.r3.u64 = t, c.r4.u64 = 1, c.r5.u64 = 1;
    sub_828BD548(c, b);
    const uint32_t r = c.r3.u32;
    c = keep;
    return r;
  };
  __imp__sub_8243D680(ctx, base);
  if (Trace())
    REXLOG_INFO("[svr2011] exhibition titles: end of match title {} result {} holder 0: {} -> {} holder 4: {} -> {} tag 2: {} {} tag 3: {} {}", Be32(base, self + 112),
                Be32(base, self + 120), h0, holder(ctx, base, 0), h4, holder(ctx, base, 4), holder(ctx, base, 2), second(ctx, base, 2),
                holder(ctx, base, 3), second(ctx, base, 3));
}
