// A hook that needs its caller's non-volatile register (r14-r31).
//
// On the Switch these registers are local variables of the recompiled functions
// (non_volatile_as_local, switch/svr2011_codegen.toml), so they aren't in ctx. A mid-function
// hook on the call in that file hands the value over. Elsewhere it is simply in ctx.
#pragma once

#include <cstdint>

#ifdef __SWITCH__
namespace svr2011 {
extern thread_local uint32_t t_caller_nv;
}
#define SVR_CALLER_NV(ctx, reg) (svr2011::t_caller_nv)
#else
#define SVR_CALLER_NV(ctx, reg) ((ctx).reg.u32)
#endif
