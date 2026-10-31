// Ring Kit, game side (arenas branch). See ring_rules.h.
//
// The ring is defined once, in a block of constants the ring code and the
// wrestlers' moves share (0x82D9D700): mat height -12 (+0x0C), edge 31.3
// (+0x08), size 64 (+0x04), rope spacing 4.2 (+0x1C), the four corner posts
// at (+-31.7, -12, +-31.7) (+0x80). Start-up code (0x82D35DD0-0x82D36100)
// derives globals from it; the bottom rope's height is one of them
// (0x82E354E4 = -12 - 3.4). Rope k (0 = bottom) is at that height minus
// k * 4.2. Units: 1 = 10 cm, y points down.
#include "ring_rules.h"
#include "caw_limits.h"
#include "fps_overlay.h"

#include <algorithm>
#include <thread>
#include <cstdint>
#include <set>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;

constexpr uint32_t kRingConstants = 0x82D9D700;
constexpr uint32_t kRopeGap = 0x82D9D71C;   // 4.2
constexpr uint32_t kRopeBase = 0x82E354E4;  // -15.4 (= -12 - 3.4)
constexpr float kGameGap = 4.2f, kGameBase = 3.4f, kMatY = -12.0f;

struct Rules {
  bool ropes[3] = {true, true, true};
  float rope_base = kGameBase, rope_gap = kGameGap;
};
Rules g_rules;
std::atomic<bool> g_no_ropes{false};  // every rope left out: rope gameplay off

bool NoRopes(const Rules& r) { return !r.ropes[0] && !r.ropes[1] && !r.ropes[2]; }

float RdF(const uint8_t* p) {
  const uint32_t v = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

Rules Parse(const std::string& text) {
  Rules r;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    std::istringstream vs(v);
    if (k == "ring.ropes") {
      for (bool& b : r.ropes) { int x = 1; vs >> x; b = x != 0; }
    } else if (k == "ring.rope_base") {
      r.rope_base = std::strtof(v.c_str(), nullptr);
    } else if (k == "ring.rope_gap") {
      r.rope_gap = std::strtof(v.c_str(), nullptr);
    }
  }
  // keep the top rope under the posts (25.1 above the floor, 13.1 above the mat)
  if (r.rope_base < 1.0f || r.rope_base > 8.0f) r.rope_base = kGameBase;
  if (r.rope_gap < 1.5f || r.rope_gap > 6.0f || r.rope_base + 2 * r.rope_gap > 12.5f) r.rope_gap = kGameGap;
  return r;
}

void Apply(const Rules& r) {
  if (!g_memory) return;
  uint8_t* base = g_memory->virtual_membase();
  WrF(base + kRopeGap, r.rope_gap);
  WrF(base + kRopeBase, kMatY - r.rope_base);
}

// Test aid (SVR2011_TEST_STATE_LOG=<offsets>, e.g. "212,216"): a fighter's
// words at those offsets and its position (+288), whenever one changes.
std::vector<uint32_t> g_log_fields;
bool g_log_scan = false;  // SVR2011_TEST_STATE_LOG=scan

void LogFighter(uint8_t* base, uint32_t f) {
  constexpr uint32_t kChars = 0x82E3CC50;
  int i = 0;
  while (i < 6 && Rd32(base + kChars + i * 4) != f) ++i;
  if (i == 6) return;
  if (g_log_scan) {
    // every small-int word of the first 4 KB (not counters) for fighter 0
    static uint32_t prev[1024];
    static bool primed = false;
    if (i != 0) return;
    std::string changed;
    for (uint32_t k = 0; k < 1024; ++k) {
      const uint32_t v = Rd32(base + f + 4 * k);
      const uint32_t o = prev[k];
      prev[k] = v;
      if (!primed || v == o || v > 4096 || o > 4096 || v == o + 1) continue;
      changed += fmt::format(" +{}: {}->{}", 4 * k, o, v);
    }
    primed = true;
    static int calls = 0;
    if (++calls % 600 == 1)
      REXLOG_INFO("[svr2011] ring scan: call {} fighter {:08X} +212 {} +0 {:08X}", calls, f, Rd32(base + f + 212),
                  Rd32(base + f));
    if (!changed.empty())
      REXLOG_INFO("[svr2011] ring scan: at ({:.1f} {:.1f} {:.1f}){}", RdF(base + f + 288), RdF(base + f + 292),
                  RdF(base + f + 296), changed);
    return;
  }
  static std::vector<uint32_t> last[6];
  auto& l = last[i];
  l.resize(g_log_fields.size());
  std::string changed;
  for (size_t k = 0; k < g_log_fields.size(); ++k) {
    const uint32_t v = Rd32(base + f + g_log_fields[k]);
    if (v == l[k]) continue;
    changed += fmt::format(" +{}: {} -> {}", g_log_fields[k], l[k], v);
    l[k] = v;
  }
  if (!changed.empty())
    REXLOG_INFO("[svr2011] ring state: {} at ({:.1f} {:.1f} {:.1f}){}", i, RdF(base + f + 288), RdF(base + f + 292),
                RdF(base + f + 296), changed);
}

}  // namespace

// The rope draw (once a frame in a match).
// Test aid (SVR2011_TEST_HEAP_LOG=1): the game's heaps (table at
// *0x82EC5F48, 36 bytes each: start, end) once, and every failed allocation
// (sub_8269A8D8(size, align) for the thread's heap, sub_8269A478).
bool g_heap_log = false;
int g_heap_watch = -1;  // (SVR2011_TEST_HEAP_LOG=<heap 2-53>: every allocation in that heap too)

// A failed allocation the hooks below had no fallback for: logged once per heap and caller (every one with the
// test aid) - a player's crash report then names it (27 Oct: an Extreme Rules weapon load wrote to such a 0 at
// lr 8215A008, now and then). The file cache's heap (16) fails on purpose while it frees old files: only with the aid.
namespace {
// The game's heaps (table at *0x82EC5F48, 36 bytes each: start, end).
void LogHeapTable(const uint8_t* base) {
  const uint32_t table = Rd32(base + 0x82EC5F48);
  if (!table) return;
  for (int i = 0; i < 54; ++i) {
    const uint32_t a = Rd32(base + table + i * 36), b = Rd32(base + table + i * 36 + 4);
    if (a || b) REXLOG_INFO("[svr2011] heap {}: {:08X}-{:08X} ({:.1f} MB)", i, a, b, (b - a) / 1048576.0);
  }
}

// A heap the game can't do without when it's full: heap 12 (4.5 MB) failed three 4-byte allocations
// in sub_82173708 at a match load and the game wrote to the 0 it got (2.1.7 report c9e6c5: a raw access violation
// at lr 821738C4, after many arena previews). Such a failure now ends the game here, before the caller uses the 0:
// the heaps and the failure in the log, the "out of game memory" notice, then a crash report (abort). Other
// threads that fail meanwhile wait for the end. (Any size: a forced failure of a 1.4 MB block there, lr 821EA528,
// was written to as well.)
constexpr uint32_t kExhaustedHeaps[] = {12};
void HeapExhausted(const uint8_t* base, uint32_t heap, uint32_t size, uint32_t lr) {
  static std::atomic<bool> ending{false};
  if (ending.exchange(true))
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  LogHeapTable(base);
  REXLOG_ERROR("[svr2011] out of game memory: heap {} has no room for {} bytes (lr {:08X}) - the game can't "
               "go on; ending it (crash report)", heap, size, lr);
  rex::FlushLogging();
  svr2011::ShowNotice("Out of game memory: the game ran out of room for its menus and match data. "
                      "The game will close - use fewer mods / custom arenas in one session.", 10.0f);
  std::this_thread::sleep_for(std::chrono::seconds(10));
  std::abort();  // (the crash reporter writes the report, the log carries the line above)
}

void NoteFailed(const uint8_t* base, uint32_t heap, uint32_t size, uint32_t lr) {
  static std::mutex m;
  static std::set<uint64_t> seen;
  if (heap == 16 && !g_heap_log) return;
  bool told = true;
  {
    std::lock_guard lock(m);
    if (!g_heap_log && !seen.insert(uint64_t(heap) << 32 | lr).second) told = false;
  }
  if (told) REXLOG_WARN("[svr2011] heap {}: allocation of {} bytes FAILED (lr {:08X})", heap, size, lr);
  if (std::find(std::begin(kExhaustedHeaps), std::end(kExhaustedHeaps), heap) != std::end(kExhaustedHeaps))
    HeapExhausted(base, heap, size, lr);
}

// Test aid SVR2011_TEST_HEAP_FAIL=<heap>@<seconds>: that heap's allocations fail from that many seconds after start.
bool ForcedFail(uint32_t heap) {
  static const auto cfg = [] {
    std::pair<int, int> c{-1, 0};
    if (const char* v = std::getenv("SVR2011_TEST_HEAP_FAIL"); v && *v) {
      c.first = std::atoi(v);
      if (const char* at = std::strchr(v, '@')) c.second = std::atoi(at + 1);
    }
    return c;
  }();
  static const auto t0 = std::chrono::steady_clock::now();
  return cfg.first >= 0 && int(heap) == cfg.first && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(cfg.second);
}
}  // namespace

REX_EXTERN(__imp__sub_8269A8D8);
REX_HOOK_RAW(sub_8269A8D8) {
  const uint32_t size = ctx.r3.u32;
  __imp__sub_8269A8D8(ctx, base);
  if (static const bool aid = std::getenv("SVR2011_TEST_HEAP_FAIL") != nullptr; aid && ctx.r3.u32) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (ForcedFail(heap)) ctx.r3.u64 = 0;
  }
  if (ctx.r3.u32 == 0 && size) {  // (the CAW editor's part heap full: caw_limits.h)
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (heap == 42)
      if (const uint32_t p = svr2011::CawHeapFallback(size)) {
        ctx.r3.u64 = p;
        return;
      }
    // (the stage's heap full - an arena with more 3D detail than the game's own: caw_limits.h kStageHeap; the
    // match's weapons' heap - OPTIONS -> WEAPONS picks: kWeaponHeap)
    if ((heap == svr2011::kStageHeap || heap == svr2011::kWeaponHeap) && svr2011::BigModelMemory())
      if (const uint32_t p = svr2011::FallbackBlock(size, true)) {
        ctx.r3.u64 = p;
        static int told = 0;
        if (told++ < 5 || told % 200 == 0)
          REXLOG_INFO("[svr2011] model memory: the {} heap ({}) full - {} bytes from the fallback memory ({} so)",
                      heap == svr2011::kStageHeap ? "stage's" : "weapons'", heap, size, told);
        return;
      }
  }
  if (ctx.r3.u32 == 0 && size) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    NoteFailed(base, heap, size, uint32_t(ctx.lr));
  }
  if (!g_heap_log) return;
  static bool listed = false;
  if (!listed && Rd32(base + 0x82EC5F48)) {
    listed = true;
    LogHeapTable(base);
  }
}

REX_EXTERN(__imp__sub_8269BA30);
REX_HOOK_RAW(sub_8269BA30) {
  const uint32_t units = ctx.r3.u32;  // 16-byte units
  __imp__sub_8269BA30(ctx, base);
  if (static const bool aid = std::getenv("SVR2011_TEST_HEAP_FAIL") != nullptr; aid && ctx.r3.u32) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (ForcedFail(heap)) ctx.r3.u64 = 0;
  }
  if (ctx.r3.u32 == 0 && units) {  // (the CAW editor's part heap full: caw_limits.h)
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (heap == 42)
      if (const uint32_t p = svr2011::CawHeapFallback(units * 16)) {
        ctx.r3.u64 = p;
        return;
      }
    // (the match's weapons' heap full - OPTIONS -> WEAPONS picks: caw_limits.h kWeaponHeap; the stage's heap too:
    // an arena about 5x the game's own 3D detail (654k vertices) asked it for 322,752 bytes this way, lr 8215140C -
    // 0, the decoder then wrote to it from the arena load, sub_821878A0; and heap 53 - the game's 25 MB rest, where a
    // character file is read whole (sub_826A1420, lr 826A14EC): a 547k-vertex superstar's 27 MB ch.pac didn't fit)
    // (and the wrestlers' model heaps 17-20: the 547k-vertex superstar's model asked one for 28.7 MB this way - GPU
    // data, so the physical fallback, as model_mem.cpp's model-heap one)
    const bool model_heap = heap >= 17 && heap <= 20;
    if ((heap == svr2011::kWeaponHeap || heap == svr2011::kStageHeap || heap == 53 || model_heap) && svr2011::BigModelMemory())
      if (const uint32_t p = svr2011::FallbackBlock(units * 16, !model_heap)) {
        ctx.r3.u64 = p;
        static int told = 0;
        if (told++ < 5 || told % 200 == 0)
          REXLOG_INFO("[svr2011] model memory: the {} heap ({}) full - {} bytes from the fallback memory ({} so)",
                      heap == svr2011::kStageHeap ? "stage's" : heap == 53 ? "file" : model_heap ? "model" : "weapons'", heap,
                      units * 16, told);
        return;
      }
  }
  if (g_heap_log && ctx.r3.u32 && (units * 16 >= 256 * 1024 || g_heap_watch >= 0)) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (units * 16 >= 256 * 1024 || int(heap) == g_heap_watch)
      REXLOG_INFO("[svr2011] heap {}: {} bytes at {:08X} (lr {:08X})", heap, units * 16, ctx.r3.u32, uint32_t(ctx.lr));
  }
  if (ctx.r3.u32 == 0 && units) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    NoteFailed(base, heap, units * 16, uint32_t(ctx.lr));
  }
}

REX_EXTERN(__imp__sub_8219A508);
REX_HOOK_RAW(sub_8219A508) {
  Apply(g_rules);  // (start-up code sets the rope height after install)
  __imp__sub_8219A508(ctx, base);
}

// -- No ropes: rope gameplay off ---------------------------------------------
//
// (static analysis: docs/ARENA_MOD_MAKER_PLAN.md "Rope gameplay")
// A running fighter reaching the rope line: sub_823091A0(fighter, quarter)
// starts the rebound (motion 60); a whipped one: sub_82309398 (62, 64 ...);
// a second path checks sub_823054F0 first. With no ropes the runner brakes
// instead: the whip bytes cleared and the game's run brake (motion 53, as
// releasing run does). Rope breaks: every pin / submission / hold asks
// sub_821BD388(fighter) "touching the ropes"; with no ropes it answers no.
namespace {

void Brake(PPCContext& ctx, uint8_t* base, const char* why) {
  const uint32_t f = ctx.r3.u32;
  REXLOG_INFO("[svr2011] ring: no ropes, {} {:08X} brakes (motion {})", why, f, Rd32(base + f + 212));
  base[f + 1286] = 0;
  base[f + 1287] = 0;
  base[f + 1290] = 0;
  ctx.r3.u64 = f;
  ctx.r4.u64 = 53;
  sub_82397228(ctx, base);
  ctx.r3.u64 = 1;
}

}  // namespace

REX_EXTERN(__imp__sub_823091A0);
REX_HOOK_RAW(sub_823091A0) {
  if (g_no_ropes) {
    Brake(ctx, base, "runner");
    return;
  }
  __imp__sub_823091A0(ctx, base);
}

REX_EXTERN(__imp__sub_82309398);
REX_HOOK_RAW(sub_82309398) {
  if (g_no_ropes) {
    Brake(ctx, base, "whipped");
    return;
  }
  __imp__sub_82309398(ctx, base);
}

REX_EXTERN(__imp__sub_823054F0);
REX_HOOK_RAW(sub_823054F0) {
  if (g_no_ropes) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823054F0(ctx, base);
}

REX_EXTERN(__imp__sub_821BD388);
REX_HOOK_RAW(sub_821BD388) {
  if (g_no_ropes) {
    static int n = 0;
    if (n++ % 600 == 0) REXLOG_INFO("[svr2011] ring: no ropes, no rope break ({} asks)", n);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821BD388(ctx, base);
}

namespace {

// Test aid (SVR2011_TEST_RUN=1): every 6 s, fighter 0 standing (motion 0 or
// 20) starts running the way it faces (sub_8220C808: motion 50), so a
// test sees rope rebounds (or brakes) without knowing the run button.
bool g_test_run = false;

void TestRun(PPCContext& ctx, uint8_t* base, uint32_t f) {
  if (Rd32(base + 0x82E3CC50) != f) return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(6)) return;
  const uint32_t m = Rd32(base + f + 212);
  if (m != 0 && m != 20 && m != 20500) return;  // (20500: standing backstage)
  last = now;
  const auto saved = ctx;
  ctx.r3.u64 = f;
  sub_8220C808(ctx, base);
  ctx = saved;
  REXLOG_INFO("[svr2011] ring: test run from ({:.1f} {:.1f})", RdF(base + f + 288), RdF(base + f + 296));
}

}  // namespace

// A fighter's control, once a frame each (test aids above).
// (sub_822EE2E0(control): fighter at +32; runs the 116 control handlers at
// 0x82008790 until one takes the frame - the run handler is entry 7)
REX_EXTERN(__imp__sub_822EE2E0);
REX_HOOK_RAW(sub_822EE2E0) {
  if (!g_log_fields.empty() || g_test_run) {
    const uint32_t f = Rd32(base + ctx.r3.u32 + 32);
    if (!g_log_fields.empty()) LogFighter(base, f);
    if (g_test_run) TestRun(ctx, base, f);
  }
  __imp__sub_822EE2E0(ctx, base);
}

namespace svr2011 {

void InstallRingRules(rex::memory::Memory* memory) {
  g_memory = memory;
  g_test_run = std::getenv("SVR2011_TEST_RUN") != nullptr;
  g_heap_log = std::getenv("SVR2011_TEST_HEAP_LOG") != nullptr;
  if (const char* v = std::getenv("SVR2011_TEST_HEAP_LOG"); v && std::atoi(v) > 1) g_heap_watch = std::atoi(v);  // (1: on)
  if (const char* v = std::getenv("SVR2011_TEST_STATE_LOG"); v && *v) {
    g_log_scan = std::string(v) == "scan";
    if (g_log_scan) g_log_fields.push_back(0);
    for (const char* p = v; *p;) {
      char* end = nullptr;
      const unsigned long off = std::strtoul(p, &end, 0);
      if (end == p) { ++p; continue; }
      g_log_fields.push_back(uint32_t(off));
      p = end;
    }
    REXLOG_INFO("[svr2011] ring: state log {} ({} fields)", g_log_scan ? "scan" : "fields", g_log_fields.size());
  }
  // the ring constants are in read-only data
  if (auto* heap = memory->LookupHeap(kRingConstants))
    heap->Protect(kRingConstants & ~0xFFFu, 0x1000,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  // Test aid: SVR2011_TEST_RING=<manifest lines, ';' for new lines>
  if (const char* v = std::getenv("SVR2011_TEST_RING"); v && *v) {
    std::string s = v;
    for (char& c : s)
      if (c == ';') c = '\n';
    SetRingRules(s);
  }
}

void SetRingRules(const std::string& manifest) {
  std::lock_guard lock(g_mutex);
  g_rules = Parse(manifest);
  g_no_ropes = NoRopes(g_rules);
  Apply(g_rules);
  REXLOG_INFO("[svr2011] ring: ropes {}{}{}, bottom rope {:.1f} above the mat, gap {:.1f}", int(g_rules.ropes[0]),
              int(g_rules.ropes[1]), int(g_rules.ropes[2]), g_rules.rope_base, g_rules.rope_gap);
}

}  // namespace svr2011
