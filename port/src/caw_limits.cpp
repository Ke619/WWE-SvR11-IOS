// WWE SmackDown vs. Raw 2011 - CREATE A SUPERSTAR without its limits
// (caw_no_limits, on by default; OPTIONS -> CAW LIMITS: STOCK gives the
// game's own back):
//
// - No REMAINING POINTS budget. Every part / layer costs a size class
//   (sub_828DC468: a class from the layer's sub-record, table 0x8206E0B8
//   {0,1,2,4,6,8,12,16}) against a budget of 48 (sub_828DC280); over it the
//   pickers grey items out ("... Remaining Points are maxed out", strings
//   6730-6735: sub_828D76F8's bits 0x4 / 0x8 / 0x10) and a saved costume
//   isn't applied (sub_828E1F10, bit 0x2). With the cost 0 nothing is ever
//   over (the meter reads 48 of 48).
// - Any number of parts of one kind: a category's maximum (sub_828EFE70(cat,
//   0), table 0x8206EFD8: 1-3 for clothing) becomes the block's room, so a
//   second top, jacket or pair of tights goes on as a new layer instead of
//   replacing the first. Kept as the game has them: the base slots (2, 4-10:
//   body, head, face, hair - one fixed slot each), the categories already at
//   64, the second maximum (k 1), and the real array limits (56 layers per
//   attire block + 8 base, 64 in the editor: "layers are maxed out").
//
// Nothing in the .cas or the online lobby block changes size: a CAW made so
// still fits both; only its layers are more / dearer than the game allowed.
//
// The budget and the counts were also what kept a CAW inside the editor's
// part heap (heap 42) and its draw lists: opening a CAW whose ring block held
// 31 hoodies or 16+ extra T-shirts crashed (21 Oct tests). Below: a full
// heap 42 falls back to system memory (any CAW, any setting), and no-limits
// stops at kMaxLayers layers a block (50 T-shirts still crashed the draw).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#if defined(__ANDROID__)
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "caw_limits.h"

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(caw_no_limits, true, "Game",
                    "CREATE A SUPERSTAR without the REMAINING POINTS budget and with any number of parts of a kind "
                    "(OPTIONS -> CAW LIMITS)");

namespace {

bool NoLimits() {
  // (test aid: SVR2011_TEST_CAW_LIMITS=stock / off - the game's own rules whatever the setting)
  static const bool stock = [] {
    const char* v = std::getenv("SVR2011_TEST_CAW_LIMITS");
    return v && (*v == 's' || *v == '0' || *v == 'o');
  }();
  return !stock && REXCVAR_GET(caw_no_limits);
}

}  // namespace

// A layer's cost in REMAINING POINTS: sub_828DC468(layer sub-record) -> points.
REX_EXTERN(__imp__sub_828DC468);
REX_HOOK_RAW(sub_828DC468) {
  if (!NoLimits()) {
    __imp__sub_828DC468(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// A category's maximum count: sub_828EFE70(category, k) -> the table's word k
// (0, 1: the two maxima; 2, 3: sort keys).
REX_EXTERN(__imp__sub_828EFE70);
REX_HOOK_RAW(sub_828EFE70) {
  const uint32_t cat = ctx.r3.u32, k = ctx.r4.u32;
  __imp__sub_828EFE70(ctx, base);
  if (k != 0 || !NoLimits()) return;
  const bool base_slot = cat == 2 || (cat >= 4 && cat <= 10);
  const uint32_t stock = ctx.r3.u32;
  if (base_slot || stock == 0 || stock >= 64) return;
  ctx.r3.u64 = 64;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("[svr2011] caw limits: any number of parts of a kind (category {} max {} -> 64)", cat, stock);
  }
}

// The editor's free layer slot: sub_828D6388(editor) -> the first free one of
// 8..63 (0-7: the base parts), 64 when none ("... layers are maxed out").
// Without the limits a CAW could fill all 56: the editor drew 40 layers of
// T-shirts (with the heap fallback below) but crashed at 50 - another of its
// lists ran out (a null draw object in sub_82915D98, from sub_828C5E78).
// So no-limits stops at kMaxLayers layers per attire block, under that.
constexpr uint32_t kMaxLayers = 36;
REX_EXTERN(__imp__sub_828D6388);
REX_HOOK_RAW(sub_828D6388) {
  __imp__sub_828D6388(ctx, base);
  if (NoLimits() && ctx.r3.u32 < 64 && ctx.r3.u32 >= 8 + kMaxLayers) ctx.r3.u64 = 64;
}

// -- The editor's part heap ----------------------------------------------------
//
// CREATE A SUPERSTAR loads every layer of the CAW being edited into heap 42
// (45 MB, A52FEE00-A7FFEE00 - its render object's heap, sub_828D8DC8 ->
// sub_828C76A8(.., 42, ..); matches and the select screen use each fighter's
// own heap). A heavy CAW (about 30 hoodies, or 16 more T-shirts, in one
// attire) filled it: the part loader's buffer (sub_828E3718 ->
// sub_8269A8D8) came back 0 and the decoder (sub_826AEF10) wrote to it - a
// crash on opening the CAW, whatever the limits setting. A failed heap-42
// allocation now takes the block from the emulator's physical system heap
// (4 KB aligned, up to kFallbackMax in all) and the game's free of it
// (sub_8269B2D0) gives it back there.
namespace {
rex::memory::Memory* g_memory = nullptr;
constexpr uint32_t kFallbackMax = 192u << 20;
std::mutex g_fallback_mutex;
std::unordered_map<uint32_t, uint32_t> g_fallback;  // guest pointer -> size
uint32_t g_fallback_bytes = 0;
uint32_t g_fallback_logged = 0;  // (the last 16 MB step logged: "fallback in use N MB / cap")
std::atomic<uint32_t> g_fallback_blocks{0};  // (every game free asks: no lock while there are none)
}  // namespace

namespace svr2011 {

void InstallCawLimits(rex::memory::Memory* memory) {
  g_memory = memory;
#if defined(__ANDROID__)
  const uint64_t ram = uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGE_SIZE));
  REXLOG_INFO("[svr2011] big models: {} pages a wrestler ({} MB of device memory{})", ModelPageLimit(), ram >> 20,
              std::getenv("SVR2011_TEST_RAM_MB") ? ", SVR2011_TEST_RAM_MB set" : "");
#else
  REXLOG_INFO("[svr2011] big models: {} pages a wrestler{}", ModelPageLimit(),
              std::getenv("SVR2011_TEST_RAM_MB") ? " (SVR2011_TEST_RAM_MB set)" : "");
#endif
}

uint32_t CawHeapFallback(uint32_t size) {
  static const bool off = std::getenv("SVR2011_TEST_CAW_HEAP_FALLBACK_OFF") != nullptr;  // (test aid: the game's way)
  if (!g_memory || !size || off) return 0;
  std::lock_guard lock(g_fallback_mutex);
  if (g_fallback_bytes + size > kFallbackMax) {
    REXLOG_WARN("[svr2011] caw limits: the editor's part heap is full and the fallback too ({} MB) - {} bytes not given",
                g_fallback_bytes >> 20, size);
    return 0;
  }
  const uint32_t ptr = g_memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!ptr) {
    REXLOG_WARN("[svr2011] caw limits: the editor's part heap is full, no fallback memory for {} bytes", size);
    return 0;
  }
  g_fallback[ptr] = size;
  g_fallback_bytes += size;
  g_fallback_blocks = uint32_t(g_fallback.size());
  static int told = 0;
  if (told++ < 3 || told % 100 == 0)
    REXLOG_INFO("[svr2011] caw limits: the editor's part heap (42) is full - {} bytes at {:08X} from the system heap "
                "({:.1f} MB so, {} blocks)",
                size, ptr, g_fallback_bytes / 1048576.0, g_fallback.size());
  return ptr;
}

// (the file loader's cache, model_mem.cpp: a file that never fits in its heap - the same fallback memory, its
// blocks freed by the same hook)
// (logged at each 16 MB step up and at every refusal, so a report shows how near the cap a match came)
uint32_t FallbackBlock(uint32_t size, bool cpu, uint32_t align) {
  if (!g_memory || !size) return 0;
  std::lock_guard lock(g_fallback_mutex);
  if (g_fallback_bytes + size > kFallbackMax) {
    static int told = 0;
    if (told++ < 10 || told % 1000 == 0)
      REXLOG_WARN("[svr2011] fallback in use {:.1f} MB / cap {} MB - {} bytes refused (the game's own failure follows)",
                  g_fallback_bytes / 1048576.0, kFallbackMax >> 20, size);
    return 0;
  }
  const uint32_t ptr = g_memory->SystemHeapAlloc(size, align ? align : cpu ? 0x10 : 0x1000,
                                                 cpu ? rex::memory::kSystemHeapVirtual : rex::memory::kSystemHeapPhysical);
  if (!ptr) {
    REXLOG_WARN("[svr2011] fallback in use {:.1f} MB / cap {} MB - the system heap has no {} bytes",
                g_fallback_bytes / 1048576.0, kFallbackMax >> 20, size);
    return 0;
  }
  g_fallback[ptr] = size;
  g_fallback_bytes += size;
  g_fallback_blocks = uint32_t(g_fallback.size());
  if (const uint32_t step = g_fallback_bytes >> 24; step > g_fallback_logged) {
    g_fallback_logged = step;
    REXLOG_INFO("[svr2011] fallback in use {:.1f} MB / cap {} MB ({} blocks)", g_fallback_bytes / 1048576.0,
                kFallbackMax >> 20, g_fallback.size());
  }
  return ptr;
}

bool BigModelMemory() {
  static const bool on = std::getenv("SVR2011_TEST_BIGMEM_OFF") == nullptr;
  return on;
}

// (Android: a device with under 6 GB of memory keeps 1024 pages - the big models' fallback memory is host memory too,
// and six of them in a match on a small phone is more than it holds; logged once)
uint32_t ModelPageLimit() {
  static const uint32_t pages = [] {
    if (const char* v = std::getenv("SVR2011_TEST_PAGE_LIMIT"); v && *v) return uint32_t(std::strtoul(v, nullptr, 10));
    if (!BigModelMemory()) return 192u;
    uint64_t ram = 0;  // (0: the rule doesn't apply - a PC)
#if defined(__ANDROID__)
    ram = uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGE_SIZE));
#endif
    // (test aid SVR2011_TEST_RAM_MB=<MB>: the memory as if it were that - the small-device rule, on any device or a PC)
    if (const char* v = std::getenv("SVR2011_TEST_RAM_MB"); v && *v) ram = uint64_t(std::strtoull(v, nullptr, 10)) << 20;
    return ram && ram < (uint64_t(6) << 30) ? 1024u : 1600u;
  }();
  return pages;
}

uint32_t FallbackBytesInUse() {
  std::lock_guard lock(g_fallback_mutex);
  return g_fallback_bytes;
}

bool CawHeapFree(uint32_t ptr) {
  if (!ptr || !g_fallback_blocks.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_fallback_mutex);
  const auto it = g_fallback.find(ptr);
  if (it == g_fallback.end()) return false;
  g_fallback_bytes -= it->second;
  g_fallback.erase(it);
  g_fallback_blocks = uint32_t(g_fallback.size());
  if ((g_fallback_bytes >> 24) + 1 < g_fallback_logged)  // (16 MB down: the next climb logged again)
    g_fallback_logged = (g_fallback_bytes >> 24) + 1;
  g_memory->SystemHeapFree(ptr);
  if (g_fallback.empty()) {  // (a game that takes fallback memory gives it all back: logged a few times)
    static int told = 0;
    if (told++ < 5) REXLOG_INFO("[svr2011] fallback in use 0 MB / cap {} MB - every block given back", kFallbackMax >> 20);
  }
  return true;
}

}  // namespace svr2011
