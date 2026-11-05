// WWE SmackDown vs. Raw 2011 - failed vertex / index buffer allocations.
//
// sub_82914DD0(size, align, flags) gets physical memory for the game's D3D
// buffers (through sub_82158C70) and returns 0 when there is no room. The
// character model loader (sub_826E73F8, per mesh: vertex count x a stride
// from the mesh's format flags, then sub_82915D98 locks it) doesn't check,
// and the null crashes the match load (lr 826E7494) - seen with a converted
// SvR 2009 model (Umaga: body + dreadlocks) that loads part by part.
//
// Every failed allocation is logged once per caller with its size, so a
// model that doesn't fit shows what it asked for.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "arena_mods.h"
#include "caw_limits.h"
#include "frame_rate.h"
#include "native/native_renderer.h"
#include "fps_overlay.h"
#include "mem_trace.h"
#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {
std::mutex g_mutex;
std::set<uint32_t> g_logged;
std::set<uint32_t> g_x128;  // allocator-128 blocks from the fallback memory (FallbackBlock), freed by XMemFree
uint64_t g_x128_bytes = 0, g_x128_count = 0;
std::atomic<uint32_t> g_x128_n{0};  // (their count: XMemFree's hook asks without the lock while there are none)
uint64_t g_failed = 0;
rex::memory::Memory* g_memory = nullptr;

// how full the physical heaps are: free pages x page size per heap
void LogHeaps() {
  if (!g_memory) return;
  auto* phys = g_memory->GetPhysicalHeap();
  std::string s;
  for (uint32_t ps : {4096u, 65536u, 16u << 20}) {
    auto* h = g_memory->LookupHeapByType(true, ps);
    if (!h) continue;
    char b[96];
    std::snprintf(b, sizeof b, " %uK pages: %u of %u free;", ps >> 10, h->GetUnreservedPageCount(), h->GetTotalPageCount());
    s += b;
  }
  if (phys) {
    char b[96];
    std::snprintf(b, sizeof b, " physical: %u of %u 4K pages free", phys->GetUnreservedPageCount(), phys->GetTotalPageCount());
    s += b;
  }
  REXLOG_WARN("[svr2011] model memory: heaps{}", s);
}
}  // namespace

// The match's block heap (sub_821DB908 at each match start: up to 2 regions of 6.5 MB by XMemAlloc, allocator 0,
// caller 821DB9B4 -> sub_8215A380 in 19EC0000-1A540000 and 1A540000-1ABC0000, each exactly its size, protect 4; the
// second only when the first is full - big models: six 1,489-page superstars' blocks, 286 KB each at 82258AC4,
// filled the first) needs its windows free (and a page more each: the 4 KB pages' 0x1000 offset, LowAbove below).
// Another allocator shares them (1 MB / 64 KB / 48 KB ... blocks, protect 404) and model / arena loads push into
// them: six 1,047-page superstars in a 327k-vertex arena left 73 MB of physical memory free and no 6.5 MB there. So
// each window is held (a reserve in the physical heap) once the game's heaps are made (a reserve before that, at
// start-up, broke their set-up: its own physical blocks found no room, the boot a crash in sub_8269AD08) and the
// window is free, handed over for the block heap's own request only and taken back when XMemFree frees that block;
// the other allocator's blocks go elsewhere at once (the "anywhere" path below, as with a full window before). Off:
// test aid SVR2011_TEST_BLOCKHEAP_RESERVE_OFF=1.
namespace {
constexpr uint32_t kBlockSize = 0x680000;
constexpr uint32_t kBlockLo[2] = {0x19EC0000, 0x1A540000};  // (the windows: lo .. lo + kBlockSize)
constexpr uint32_t kPageOffset = 0x1000;  // (see LowAbove below: each reserve is at its window + this)
std::mutex g_block_mutex;
bool g_block_held[2] = {};      // (the reserves in place)
uint32_t g_block_ptr[2] = {};   // (the block heap's blocks now, from XMemAlloc: given back to the reserves when freed)
std::atomic<bool> g_blocks_all{false};  // (every window held or in the block heap's use: nothing to try)
thread_local bool t_block_request = false;  // (inside the block heap's XMemAlloc)
using svrfmt::Rd32;

bool BlockReserveOn() {
  static const bool on = std::getenv("SVR2011_TEST_BLOCKHEAP_RESERVE_OFF") == nullptr;
  return on;
}

// (the window [lo, hi] is one of the block heap's: its index, else -1)
int BlockWindow(uint32_t lo, uint32_t hi) {
  for (int w = 0; w < 2; ++w)
    if (lo == kBlockLo[w] && hi == kBlockLo[w] + kBlockSize) return w;
  return -1;
}

// The SDK's 4 KB-page physical heap (E0000000) is the physical memory + 0x1000 (MmAllocatePhysicalMemoryEx: a window
// [lo, hi] gives E0000000 + lo - 0x1000 at the lowest), and the game's check after the allocation (sub_82158818:
// address & 0x1FFFFFFF >= the window's start, else freed and 0) sees that page below the start: a block at its
// window's very bottom always failed - the only place for one exactly the window's size, as the block heap's (27 Oct:
// F9EBF000, XMemAlloc 0, the crash). So such a request's window starts a page higher (this), and a widened one too.
uint32_t LowAbove(uint32_t lo, uint32_t prot) { return (prot & 0xA0000000u) == 0 && lo ? lo + kPageOffset : lo; }

// (g_block_mutex held) every free window held; true when they all are
bool HoldBlockWindows(const char* when) {
  if (!g_memory) return false;
  auto* phys = g_memory->GetPhysicalHeap();
  bool all = true;
  for (int w = 0; w < 2; ++w) {
    if (g_block_held[w] || g_block_ptr[w]) continue;
    g_block_held[w] = phys && phys->AllocFixed(kBlockLo[w] + kPageOffset, kBlockSize, 0x1000,
                                               rex::memory::kMemoryAllocationReserve,
                                               rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
    all = all && g_block_held[w];
    static int held = 0, missed = 0;
    if (g_block_held[w] ? held++ < 8 : (missed++ < 3 || missed % 1000 == 0))
      REXLOG_INFO("[svr2011] model memory: block heap window {:08X}-{:08X} {} ({})", kBlockLo[w], kBlockLo[w] + kBlockSize,
                  g_block_held[w] ? "held" : "not held yet - something is in it", when);
  }
  return all;
}
}  // namespace

namespace svr2011 {
void InstallModelMem(rex::memory::Memory* memory) { g_memory = memory; }
}  // namespace svr2011

// The load watch: physical memory with no room anywhere for a block (above) is often survived - the caller has a
// fallback, or does without - but a load can also wait on it for ever: a silent loading screen (27 Oct, six
// 1,489-page superstars in an arena past the mesh cap). So after such a failure, 90 s of nothing but a loading
// screen (a handful of draws a frame) ends the game with a plain "out of game memory" message, the window and the
// physical memory left in the log, and a crash report - not an endless loading screen. A scene drawn in between
// (more draws: entrances, the match) disarms it, and only a match's load counts (NoteMatchLoading: the menus draw a
// handful too). Test aids: SVR2011_TEST_OOM_WATCH_S=<seconds> (90; Android 180), SVR2011_TEST_OOM_ARM=1.
namespace {
std::mutex g_oom_mutex;
std::string g_oom_what;  // (the first failure since the last scene: window, size, physical pages free)
std::atomic<bool> g_oom_armed{false};
std::atomic<bool> g_match_loading{false};  // (NoteMatchLoading)
void StartLoadWatch(uint32_t lo, uint32_t hi, uint32_t size);

void NoteOutOfRoom(uint32_t lo, uint32_t hi, uint32_t size) {
  if (g_oom_armed.exchange(true)) return;
  StartLoadWatch(lo, hi, size);
}

void StartLoadWatch(uint32_t lo, uint32_t hi, uint32_t size) {
  auto* phys = g_memory ? g_memory->GetPhysicalHeap() : nullptr;
  {
    std::lock_guard lock(g_oom_mutex);
    g_oom_what = fmt::format("{} bytes in the window {:08X}-{:08X}; physical memory {} of {} 4K pages free", size, lo, hi,
                             phys ? phys->GetUnreservedPageCount() : 0, phys ? phys->GetTotalPageCount() : 0);
  }
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      rex::thread::set_current_thread_name("model memory");
      static const int limit_s = [] {
        const char* v = std::getenv("SVR2011_TEST_OOM_WATCH_S");
#if defined(__ANDROID__)
        constexpr int kDefault = 180;  // (a phone's loads are slower)
#else
        constexpr int kDefault = 90;
#endif
        return v && std::atoi(v) > 0 ? std::atoi(v) : kDefault;
      }();
      int quiet_s = 0;
      uint64_t frames = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_oom_armed.load()) {
          quiet_s = 0;
          continue;
        }
        // (only frames this renderer counted: one that doesn't count them never trips it)
        const uint64_t now = svr2011::native::FramesShown();
        if (now == frames) continue;
        frames = now;
        // (a frame with a scene: disarmed - the load got through, or it's the match / menus)
        if (svr2011::native::LastFrameDraws() > 5) {
          g_oom_armed = false, quiet_s = 0;
          continue;
        }
        if (!g_match_loading.load()) {  // (the menus draw a handful too: only a match's load counts)
          quiet_s = 0;
          continue;
        }
        if (++quiet_s < limit_s) continue;
        std::string what;
        {
          std::lock_guard lock(g_oom_mutex);
          what = g_oom_what;
        }
        REXLOG_ERROR("[svr2011] out of game memory: a load has waited {} s on a loading screen since physical memory had "
                     "no room for {} - the mods / arena in this match are too much for the game's 512 MB; ending the "
                     "game (crash report)", quiet_s, what);
        svr2011::ShowNotice("Out of game memory: this match's superstars and arena are too big to load together. "
                            "The game will close - pick lighter mods or another arena.", 10.0f);
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::abort();  // (the crash reporter writes the report, the log carries the line above)
      }
    }).detach();
  });
}
}  // namespace

namespace svr2011 {
void NoteMatchLoading(bool loading) {
  g_match_loading = loading;
  // Test aid SVR2011_TEST_OOM_ARM=1: armed at every match load, as if physical memory had run out (the message path)
  static const bool arm = std::getenv("SVR2011_TEST_OOM_ARM") != nullptr;
  if (loading && arm && !g_oom_armed.exchange(true)) StartLoadWatch(0, 0, 0);
}
}  // namespace svr2011

REX_EXTERN(__imp__sub_82914DD0);
REX_HOOK_RAW(sub_82914DD0) {
  const uint32_t size = ctx.r3.u32, align = ctx.r4.u32, flags = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82914DD0(ctx, base);
  // test aid SVR2011_TEST_BUFFER_LOG: every model-loader vertex buffer (caller 826E7468) and the running total
  static const bool log_all = std::getenv("SVR2011_TEST_BUFFER_LOG") != nullptr;
  if (log_all && caller == 0x826E7468) {
    static uint64_t total = 0;
    total += size;
    REXLOG_INFO("[svr2011] model memory: vertex buffer {} bytes ({}), total {}", size, ctx.r3.u32 ? "ok" : "FAILED", total);
  }
  if (ctx.r3.u32 == 0 && size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_failed;
    if (g_logged.insert(caller).second)
      REXLOG_WARN("[svr2011] model memory: a {} byte buffer (align {}, flags {:x}) has no room (caller {:08X}, {} failed so far)",
                  size, align, flags, caller, g_failed);
    if (g_failed <= 3) LogHeaps();
  }
}

// The game's XMemAlloc (sub_82158C70(size, attributes): heaps by the allocator id in
// attributes byte 1, table at 0x82DDBC18): failures logged with the attributes, once each.
REX_EXTERN(__imp__sub_82158C70);
REX_HOOK_RAW(sub_82158C70) {
  const uint32_t size = ctx.r3.u32, attr = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const bool block = caller == 0x821DB9B4 && size == kBlockSize && BlockReserveOn();
  t_block_request = block;
  __imp__sub_82158C70(ctx, base);
  t_block_request = false;
  if (block && ctx.r3.u32) {
    std::lock_guard<std::mutex> lock(g_block_mutex);
    for (int w = 0; w < 2; ++w)
      if ((ctx.r3.u32 & 0x1FFFFFFF) == kBlockLo[w]) g_block_ptr[w] = ctx.r3.u32;
  }
  // Big models / arenas: allocator 128 (the model loader's vertex / index buffers and textures - a wrestler's 768 KB
  // window, the stage's) full - the block from the fallback memory; XMemFree's hook (below) gives it back. Every
  // allocator-128 block is freed by XMemFree (SVR2011_TEST_MEMTRACE, 23 Oct: 21,782 of 21,782, two call sites).
  if (ctx.r3.u32 == 0 && size && ((attr >> 16) & 0xFF) == 128 && svr2011::BigModelMemory())
    if (const uint32_t p = svr2011::FallbackBlock(size)) {
      ctx.r3.u64 = p;
      std::lock_guard<std::mutex> lock(g_mutex);
      g_x128.insert(p);
      g_x128_n = uint32_t(g_x128.size());
      g_x128_bytes += size;
      if (++g_x128_count <= 3 || g_x128_count % 200 == 0)
        REXLOG_INFO("[svr2011] model memory: allocator 128 full - {} bytes at {:08X} from the fallback memory (caller {:08X};"
                    " {} blocks, {:.1f} MB held there)", size, p, caller, g_x128.size(), g_x128_bytes / 1048576.0);
    }
  if (svr2011::MemTraceOn() && ((attr >> 16) & 0xFF) == 128) svr2011::MemTraceXAlloc(ctx.r3.u32, size, attr, caller);
  if (ctx.r3.u32 == 0 && size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static std::set<uint64_t> seen;
    if (seen.insert(uint64_t(attr) << 32 | caller).second)
      REXLOG_WARN("[svr2011] model memory: XMemAlloc {} bytes, attributes {:08X} (allocator {}) failed (caller {:08X})", size,
                  attr, (attr >> 16) & 0xFF, caller);
  }
}

// The game's XMemFree (sub_82158940(ptr, attributes)): test aid SVR2011_TEST_MEMTRACE (mem_trace.h).
REX_EXTERN(__imp__sub_82158940);
REX_HOOK_RAW(sub_82158940) {
  if (svr2011::MemTraceOn()) svr2011::MemTraceXFree(ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  if (const uint32_t ptr = ctx.r3.u32; ptr && g_x128_n.load(std::memory_order_relaxed)) {  // (a fallback block: back there)
    bool mine = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      mine = g_x128.erase(ptr) != 0;
      g_x128_n = uint32_t(g_x128.size());
      if (mine && g_x128.empty())
        REXLOG_INFO("[svr2011] model memory: every allocator-128 fallback block given back ({} given so far)", g_x128_count);
    }
    if (mine && svr2011::CawHeapFree(ptr)) return;
  }
  const uint32_t ptr = ctx.r3.u32;
  __imp__sub_82158940(ctx, base);
  if (ptr && BlockReserveOn()) {  // (the block heap's block freed: its window held again)
    std::lock_guard<std::mutex> lock(g_block_mutex);
    for (int w = 0; w < 2; ++w)
      if (ptr == g_block_ptr[w]) {
        g_block_ptr[w] = 0;
        g_blocks_all = HoldBlockWindows("block heap freed");
        if (auto* phys = g_memory ? g_memory->GetPhysicalHeap() : nullptr; phys && w == 0)  // (a match's end)
          REXLOG_INFO("[svr2011] model memory: block heap freed - physical {} of {} 4K pages free, fallback {:.1f} MB; "
                      "render queue peak {}%", phys->GetUnreservedPageCount(),
                      phys->GetTotalPageCount(), svr2011::FallbackBytesInUse() / 1048576.0, svr2011::RenderQueuePeakTake());
      }
  }
}

// sub_8215A380(size, min, max, align, protect) -> MmAllocatePhysicalMemoryEx: an
// allocator heap growing inside its physical address window. Failures logged.
//
// A window that's full is tried again anywhere in physical memory: the match's block heap (sub_821DB908 at the
// match start, after the entrances: 2 x 6.5 MB, the first in 19EC0000-1A540000 - exactly its size) found a page
// of its window taken - two big 2K14 ports in a mod arena (2.1.3 / 2.1.4.1 reports, every time: Eddie Guerrero
// and Undertaker Biker in "Smackdown 13") - and the game cleared the null it got (memset at lr 821DBA14), a
// crash 2.5 s after the entrances. The heap's users take its base as given (sub_821DB908 lays its blocks from
// the address returned), so another place works - above the window's start: the caller (sub_82158818) frees a
// block below it and fails (27 Oct: the block heap placed at 138A8000 still crashed). No room there is still 0
// (logged with the heaps).
REX_EXTERN(__imp__sub_8215A380);
REX_HOOK_RAW(sub_8215A380) {
  const uint32_t size = ctx.r3.u32, lo = ctx.r4.u32, hi = ctx.r5.u32, align = ctx.r6.u32, prot = ctx.r7.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const PPCContext in = ctx;
  // Test aid: SVR2011_TEST_BLOCKHEAP_SQUAT=1 - 64 KB of the block heap's window taken first (once), as another
  // allocation spilling into it would: the window is then too full for it.
  if (static const bool squat = std::getenv("SVR2011_TEST_BLOCKHEAP_SQUAT") != nullptr;
      squat && size == 0x680000 && lo == 0x19EC0000) {
    static bool done = false;
    if (!done) {
      done = true;
      ctx.r3.u64 = 0x10000;
      __imp__sub_8215A380(ctx, base);
      REXLOG_WARN("[svr2011] model memory: test - 64 KB of {:08X}-{:08X} taken first (at {:08X})", lo, hi, ctx.r3.u32);
      ctx = in;
    }
  }
  // The block heap's windows held (above): the block heap's own requests get them, any other goes elsewhere at once.
  if (BlockReserveOn() && !g_blocks_all.load(std::memory_order_relaxed) && !t_block_request && Rd32(base, 0x82EC5F48) &&
      Rd32(base, Rd32(base, 0x82EC5F48) + 53 * 36)) {  // (the heap table made - heap 53, the last, set up: hold them)
    std::lock_guard<std::mutex> lock(g_block_mutex);
    g_blocks_all = HoldBlockWindows("the game's heaps made");
  }
  const int bw = BlockReserveOn() ? BlockWindow(lo, hi) : -1;
  bool block_window = false;
  if (bw >= 0) {
    std::lock_guard<std::mutex> lock(g_block_mutex);
    if (t_block_request && size == kBlockSize) {
      if (g_block_held[bw] && g_memory->GetPhysicalHeap()->Release(kBlockLo[bw] + kPageOffset)) g_block_held[bw] = false;
      block_window = true;
    } else if (g_block_held[bw]) {
      static uint32_t moved = 0;
      if (++moved <= 3 || moved % 500 == 0)
        REXLOG_INFO("[svr2011] model memory: {} bytes for a block heap window placed elsewhere ({} so far)", size, moved);
      ctx.r4.u64 = LowAbove(lo, prot), ctx.r5.u64 = 0x1FFFFFFF;  // (above the window: the reserves themselves skipped)
    }
  }
  if (size && hi > lo && size >= hi - lo && ctx.r4.u32 == lo && ctx.r5.u32 == hi)  // (exactly its window's size)
    ctx.r4.u64 = LowAbove(lo, prot), ctx.r5.u64 = hi + (LowAbove(lo, prot) - lo);
  __imp__sub_8215A380(ctx, base);
  if (block_window && ctx.r3.u32 == 0) {
    std::lock_guard<std::mutex> lock(g_block_mutex);
    g_blocks_all = HoldBlockWindows("block heap request failed");
  }
  static const bool log_all = std::getenv("SVR2011_TEST_BUFFER_LOG") != nullptr;
  if (ctx.r3.u32 == 0 || log_all)
    REXLOG_WARN("[svr2011] model memory: physical {} bytes in {:08X}-{:08X} (align {:X}, protect {:X}) -> {:08X} (caller {:08X})",
                size, lo, hi, align, prot, ctx.r3.u32, caller);
  if (ctx.r3.u32 == 0 && size && (lo != 0 || hi < 0x1FFFFFFF)) {
    LogHeaps();
    ctx = in;
    ctx.r4.u64 = LowAbove(lo, prot), ctx.r5.u64 = 0x1FFFFFFF;  // (anywhere above the window's start: LowAbove)
    __imp__sub_8215A380(ctx, base);
    REXLOG_WARN("[svr2011] model memory: ... its window full: {}", ctx.r3.u32 ? fmt::format("placed at {:08X} instead", ctx.r3.u32)
                                                                          : std::string("no room anywhere either"));
    if (!ctx.r3.u32) NoteOutOfRoom(lo, hi, size);
  }
}

// -- The file loader's cache ---------------------------------------------------
//
// The game's file loader thread (g821593A8, sub_826A1D38) reads each file into a buffer from its cache
// (*0x82ED5FE8: sub_826A1C70(size, heap) -> sub_826A1C08 -> sub_826BA928(cache, size, 2048); other loader steps
// call sub_826BA928 too). When the cache's heap (cache+76: heap 16, 20 MB) has no room, sub_826BA928 frees the
// oldest idle file (state 11, sub_826BA590) and tries again - forever: with nothing idle left it spins at 100%
// and the game waits on the load screen (2.1.4.1 report 21 Oct: "hangs after the entrances, every match"; hang
// note lr 826BA5BC / 826B9FE0, stack 826BA9A8 826A1C50 826A1CE0 826A1F1C). After 3 s with nothing idle the
// file and what holds the cache are logged (each entry: name +50, size +44 x 256, state +100, buffer +96; the
// list from cache+4, nodes {next, prev, entry}) and the buffer comes from the fallback memory instead
// (caw_limits.h FallbackBlock: the emulator's physical system heap; the cache frees its buffers with
// sub_8269B2D0, whose hook hands such a block back), so the match loads instead of hanging.
// Test aids: SVR2011_TEST_CACHE_LOG=1 - every request of 1 MB or more; SVR2011_TEST_CACHE_FULL=<bytes> - the
// cache finds no room for a request of at least that size (the hang, on purpose).
namespace svr2011 {
// (match_types.cpp: the cell's extra rule motions' read, '/MOT/RUWM' while pending)
extern const char* const kExtraRuleMotionsRead;
bool ExtraRuleMotionsPending();
void ExtraRuleMotionsRead();
}  // namespace svr2011

namespace {
constexpr uint32_t kLoaderCache = 0x82ED5FE8;

std::string GuestName(const uint8_t* base, uint32_t a) {
  std::string s;
  for (uint32_t i = 0; a && i < 72; ++i) {
    const char c = char(base[a + i]);
    if (!c) break;
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

struct CacheAlloc {
  bool active = false, stuck = false, given = false;
  uint32_t size = 0, tries = 0;
  std::chrono::steady_clock::time_point t0;
  std::string name;  // (the file it is for: noted when it is found stuck)
};
thread_local CacheAlloc t_alloc;
thread_local uint32_t t_request = 0;  // (the loader's request in sub_826A1C70: its file name at +50)

std::string RequestName(const uint8_t* base) {
  const std::string s = GuestName(base, t_request ? t_request + 50 : 0);
  return s.empty() ? std::string("(a loader step without a file name)") : "'" + s + "'";
}

void LogLoaderCache(const uint8_t* base, uint32_t cache, const CacheAlloc& r, double waited) {
  const uint32_t heap = Rd32(base, cache + 76);
  const uint32_t table = Rd32(base, 0x82EC5F48);
  uint32_t lo = 0, hi = 0;
  if (table && heap < 54) lo = Rd32(base, table + heap * 36), hi = Rd32(base, table + heap * 36 + 4);
  REXLOG_WARN("[svr2011] file cache: the loader has waited {:.0f} s ({} tries) for {} bytes for {} - cache heap {} "
              "{:08X}-{:08X} ({:.1f} MB) has no room and nothing idle to free",
              waited, r.tries, r.size, RequestName(base), heap, lo, hi, (hi - lo) / 1048576.0);
  uint64_t held = 0;
  uint32_t n = 0, shown = 0, node = Rd32(base, cache + 4);
  for (; node && n < 400; ++n) {
    const uint32_t e = Rd32(base, node + 8);
    if (!e) break;
    const uint32_t size = Rd32(base, e + 44) << 8, state = Rd32(base, e + 100), buf = Rd32(base, e + 96);
    if (buf) held += size;
    if ((state || buf) && shown++ < 40)  // (the pool's unused entries left out)
      REXLOG_WARN("[svr2011] file cache:   '{}' {} bytes, state {}{}", GuestName(base, e + 50), size, state,
                  buf ? fmt::format(", buffer {:08X}", buf) : std::string());
    const uint32_t next = Rd32(base, node);
    if (next == node) break;
    node = next;
  }
  REXLOG_WARN("[svr2011] file cache: {} entries ({} in use), {:.1f} MB in their buffers", n, shown, held / 1048576.0);
  LogHeaps();
}
}  // namespace

REX_EXTERN(__imp__sub_826A1C70);
REX_HOOK_RAW(sub_826A1C70) {
  const bool loader = uint32_t(ctx.lr) == 0x826A1F1C;  // (sub_826A1D38's file read: r31 = the request)
  if (loader) t_request = ctx.r31.u32;
  // (SVR2011_TEST_EMD_LOG=1: each model read - the request's first 50 bytes and its size: which pac a scene's
  // character model is looked up in - research for the Streak walk's mod, rtwm_streak.cpp)
  if (static const bool emd_log = std::getenv("SVR2011_TEST_EMD_LOG") != nullptr; emd_log && loader) {
    const std::string name = GuestName(base, t_request + 50);
    if (name.rfind("/EMD/", 0) == 0) {
      std::string hex;
      for (uint32_t k = 0; k < 50; ++k) hex += fmt::format("{}{:02X}", k % 4 ? "" : " ", base[t_request + k]);
      REXLOG_INFO("[svr2011] file cache: read {} - {} bytes, request {:08X}:{} (caller {:08X})", name,
                  uint64_t(Rd32(base, t_request + 44)) << 8, t_request, hex, uint32_t(ctx.lr));
    }
  }
  // A redirected arena's stage entry is read at the served file's size, not the mounted directory's (the shipped
  // file's): request +44 = size / 0x100, which this buffer and the read take (arena_mods.h ServedStageUnits).
  if (loader) {
    const std::string name = GuestName(base, t_request + 50);
    if (const uint32_t units = svr2011::ServedStageUnits(name.c_str()); units && units != Rd32(base, t_request + 44)) {
      REXLOG_INFO("[svr2011] arena mods: {} read at {} bytes (the directory says {})", name, uint64_t(units) << 8,
                  uint64_t(Rd32(base, t_request + 44)) << 8);
      uint8_t* p = base + t_request + 44;
      p[0] = uint8_t(units >> 24), p[1] = uint8_t(units >> 16), p[2] = uint8_t(units >> 8), p[3] = uint8_t(units);
      ctx.r3.u64 = uint64_t(units) << 8;  // (the buffer's size: the caller read it from +44 already)
    }
  }
  __imp__sub_826A1C70(ctx, base);
  if (loader) t_request = 0;
}

// (sub_826A1D38's second buffer for a file, sub_826BAB40(cache, request): its unpacked copy)
REX_EXTERN(__imp__sub_826BAB40);
REX_HOOK_RAW(sub_826BAB40) {
  const bool loader = uint32_t(ctx.lr) == 0x826A2268;
  if (loader) t_request = ctx.r4.u32;
  __imp__sub_826BAB40(ctx, base);
  if (loader) t_request = 0;
}

REX_EXTERN(__imp__sub_826BA928);
REX_HOOK_RAW(sub_826BA928) {
  const bool loader = ctx.r3.u32 == Rd32(base, kLoaderCache);
  if (loader) t_alloc = CacheAlloc{true, false, false, ctx.r4.u32, 0, std::chrono::steady_clock::now()};
  const uint32_t size = ctx.r4.u32;
  const std::string name = loader ? RequestName(base) : std::string();
  __imp__sub_826BA928(ctx, base);
  if (!loader) return;
  static const bool log_all = std::getenv("SVR2011_TEST_CACHE_LOG") != nullptr;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_alloc.t0).count();
  if (t_alloc.stuck || s >= 1.0 || (log_all && size >= (1u << 20)))
    REXLOG_INFO("[svr2011] file cache: {} {} bytes -> {:08X} after {:.1f} s ({} tries)", name, size, ctx.r3.u32, s,
                t_alloc.tries);
  t_alloc.active = false;
  if (name == svr2011::kExtraRuleMotionsRead) svr2011::ExtraRuleMotionsRead();
}

// (inside sub_826BA928's retry loop: one call per failed try; r3 after it = the idle files it could still free)
REX_EXTERN(__imp__sub_826B9FB8);
REX_HOOK_RAW(sub_826B9FB8) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_826B9FB8(ctx, base);
  CacheAlloc& r = t_alloc;
  if (!r.active) return;
  ++r.tries;
  if (r.stuck || ctx.r3.u32 != 0) return;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.t0).count();
  // (the cell's extra rule motions - match_types.cpp: Extreme Rules' RUWM read after the cell's own, when heap 16
  // already holds the arena, the stage and the weapons' models - from the fallback memory at once, no 3 s wait)
  const bool at_once = svr2011::ExtraRuleMotionsPending() && RequestName(base) == svr2011::kExtraRuleMotionsRead;
  if (s < 3.0 && !at_once) return;
  r.stuck = true;
  r.name = RequestName(base);
  if (at_once)
    REXLOG_INFO("[svr2011] file cache: no room for {} ({} bytes) - from the fallback memory", r.name, r.size);
  else
    LogLoaderCache(base, cache, r, s);
}

namespace svr2011 {
// (sub_8269BBA0's hook, superstar_mods.cpp: the cache's allocation from inside sub_826BA928's retry loop)
bool LoaderCacheForcedFull(uint32_t size) {
  static const uint32_t full = [] {
    const char* e = std::getenv("SVR2011_TEST_CACHE_FULL");
    return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return full && size >= full;
}

uint32_t LoaderCacheFallback(uint32_t size) {
  CacheAlloc& r = t_alloc;
  if (!r.active || !r.stuck) return 0;
  // (a request of 0 bytes: a file no mounted pac has - its directory entry not found, size 0 - e.g. a character's
  // model under a name no pac carries. It never fits nothing, so it spun forever: a small zeroed block instead,
  // the read empty - the load goes on, and the log names the missing file)
  const uint32_t p = FallbackBlock(std::max(size, 2048u));
  if (!size && !r.given)
    REXLOG_ERROR("[svr2011] file cache: {} is in no pac (0 bytes) - read as an empty file so the load goes on",
                 r.name);
  if (!r.given) {
    r.given = true;
    REXLOG_WARN("[svr2011] file cache: {} bytes {}", size,
                p ? fmt::format("from the fallback memory instead, at {:08X}", p) : std::string("- no fallback memory either"));
  }
  return p;
}
}  // namespace svr2011

// -- A match's motion files ----------------------------------------------------
//
// A match's motions live in one 36 MB buffer of heap 42 (sub_821636D8: table +344 the buffer, +336 its size, +340 what
// is used; the table's two sets: table + 352 * set, the one loading = *(table + 704) ^ 1). Each file of the group -
// MOT/BM00, BM01, BM02, BM03 ... - is read so that it ENDS at the buffer's top (sub_82162960, sub_82162890: dest = buffer
// + size - file), and sub_821646E8 trims its banks down to the used end (sub_82163808: only the motions this match can
// play). Nothing compares the two: packs grew MOT/BM01 from 17 MB to 35 MB and its read landed on the banks BM00 had
// already left there - slots 3 to 7 of the motion list junk (report d10f64: every motion of a strike bank gone, the
// wrestlers stand in a motion no bank has; 89cfee: the game's search crashed on them). Such a file is read into the
// fallback memory instead, and given back when its banks have been copied (sub_826A2D78: the read's answer).
// Test aid SVR2011_TEST_BANK_FILE_OFF=1: the game's place (the report's junk).
namespace {
thread_local uint32_t t_motion_table = 0;  // (sub_82162960 / sub_82162890 running: the table)
constexpr uint32_t kMotionFileMargin = 0x1000;
struct MotionFileBlock {
  uint32_t reader, block;
};
std::mutex g_motion_file_mutex;
std::vector<MotionFileBlock> g_motion_file_blocks;
}  // namespace

namespace svr2011 {
// (online.cpp's sub_826A2FA8 hook, before the game's: r3 the reader, r4 the name, r5 where it goes)
void MotionFileRead(PPCContext& ctx, uint8_t* base) {
  const uint32_t table = t_motion_table;
  if (!table || !ctx.r5.u32) return;
  static const bool off = std::getenv("SVR2011_TEST_BANK_FILE_OFF") != nullptr;
  if (off) return;
  const uint32_t set = table + 352 * (Rd32(base, table + 704) ^ 1), buffer = Rd32(base, set + 344);
  const uint32_t size = Rd32(base, set + 336), used = Rd32(base, set + 340), dest = ctx.r5.u32;
  if (dest >= buffer + used + kMotionFileMargin) return;
  const uint32_t bytes = buffer + size - dest;  // (the game's place: its size, rounded up to 2 KB)
  // (the virtual system heap: only the CPU reads it, and the physical pool may have no 37 MB in one piece by now)
  const uint32_t block = FallbackBlock(bytes, true, 0x1000);
  const std::string name = GuestName(base, ctx.r4.u32);
  if (!block) {
    REXLOG_ERROR("[svr2011] motion file {} ({} bytes): no fallback memory - it overwrites the motions before it", name, bytes);
    return;
  }
  {
    std::lock_guard lock(g_motion_file_mutex);
    g_motion_file_blocks.push_back({ctx.r3.u32, block});
  }
  static int told = 0;
  if (told++ < 8)
    REXLOG_WARN("[svr2011] motion file {} ({:.1f} MB) doesn't fit above the {:.1f} MB of motions before it in the {:.0f} MB "
                "buffer - read into the fallback memory, at {:08X}", name, bytes / 1048576.0, used / 1048576.0,
                size / 1048576.0, block);
  ctx.r5.u64 = block;
}
}  // namespace svr2011

REX_EXTERN(__imp__sub_82162960);
REX_HOOK_RAW(sub_82162960) {
  t_motion_table = ctx.r4.u32;
  __imp__sub_82162960(ctx, base);
  t_motion_table = 0;
}

REX_EXTERN(__imp__sub_82162890);
REX_HOOK_RAW(sub_82162890) {
  t_motion_table = ctx.r4.u32;
  __imp__sub_82162890(ctx, base);
  t_motion_table = 0;
}

// sub_826A2D78(.., .., reader): a read has come - its answer (for a motion file, sub_821646E8) copies what it keeps
REX_EXTERN(__imp__sub_826A2D78);
REX_HOOK_RAW(sub_826A2D78) {
  const uint32_t reader = ctx.r5.u32;
  __imp__sub_826A2D78(ctx, base);
  std::lock_guard lock(g_motion_file_mutex);
  for (size_t i = 0; i < g_motion_file_blocks.size(); ++i)
    if (g_motion_file_blocks[i].reader == reader) {
      svr2011::CawHeapFree(g_motion_file_blocks[i].block);
      g_motion_file_blocks.erase(g_motion_file_blocks.begin() + long(i));
      break;
    }
}
