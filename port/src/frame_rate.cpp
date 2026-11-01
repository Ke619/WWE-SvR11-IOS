// The game's frame rate - see frame_rate.h.
//
// The timing block at 0x82EDDBE8 (written by sub_826E1AE8 SetFrameRate):
//   +8  int   fps                  +28 float 1/fps (Havok's step)
//   +32 float fps                  +36 float 1000/fps     +40 int 1000/fps
//   +60 float 60/fps (the step in 60 Hz frames, what most code scales by)
//   +64 float fps/60
// It only knows 25 / 30 / 50 / 60, and much of the game steps by whole
// frames or tells 30 from 60 only - so it stays at the game's own 60 here,
// and the frame rate is made by how often the world update runs.
//
// The game's loop (logic thread, sub_821595D8): wait for the render thread,
// then the world update sub_8269D768 - task list upkeep (sub_8269D3F0) and
// the task manager sub_8269C728(manager):
//   update pass: the systems' slot 1 (input among them), then group by group
//     (the scheduler +68's slot 4 allows the group) every live task's slot 1;
//   draw pass:   the systems' slot 2, the render kick (sub_826A6018 /
//     sub_826A6080), list upkeep, the systems' slot 3.
// Here the update pass runs once per 60 Hz tick of real time, the draw pass
// once a frame. A frame always gets at least one update (the menus are drawn
// in it), so the game draws at most 60 frames a second; the frame clock (the
// guest vblank, guest_vblank_hz) runs at the chosen rate up to 60. At 30 fps,
// or when the PC can't make 60, a frame runs two (up to four) updates; the
// extra ones repeat the controller reading and leave the characters' job to
// the frame's last update (below). Online lockstep: one update a frame.
#include "frame_rate.h"
#include "match_box.h"
#include "match_record.h"
#include "match_diag.h"
#include "free_cam.h"
#include "thread_cpu.h"
#include "fast_start.h"
#include "pad_icons.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/ui/presenter.h>

#include "arena_mods.h"
#include "caw_limits.h"
#include "generated/default/svr2011_init.h"
#include "jukebox.h"
#include "match_types.h"
#include "rtwm_streak.h"
#include "native/native_renderer.h"
#include "online_overlay.h"
#include "online_rules.h"
#include "players.h"
#include "android_perf.h"
#include "mem_trace.h"
#include "superstar_mods.h"
#if SVR2011_DEBUG_SUITE
#include "debug_suite/debug_suite.h"
#endif

REXCVAR_DEFINE_INT32(frame_rate, 60, "GPU", "Frames a second: 30 or 60");
REXCVAR_DEFINE_BOOL(full_speed, true, "GPU",
                    "Keep the game at full speed when frames are slower than 60 (extra world updates); off: one "
                    "update a frame, as the console (slower when the device can't make 60)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTiming = 0x82EDDBE8;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;  // the match's frame count (its clock: / fps)
constexpr uint32_t kChars = 0x82E3CC50;        // the wrestlers

uint8_t* g_base = nullptr;            // the guest's memory
rex::memory::Memory* g_memory = nullptr;  // (the thread dump: readable pages)
std::atomic<bool> g_scene_thirty{false};  // an entrance at the original's 30 fps
std::atomic<bool> g_lockstep{false};      // online: one world update per frame at 60
std::atomic<bool> g_in_match{false};      // 30 fps applies in matches only
std::atomic<void (*)()> g_match_start{nullptr};  // armed match start (ArmMatchStart)

void Wr32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Wr32(p, v);
}

// The frame clock: the frames the game draws a second. 30 fps (in a match,
// or an entrance at the original's 30) with the native renderer: the game
// still runs at 60 Hz, one world update a frame, and every other frame isn't
// shown or drawn (native::SetHalfFrames) - two updates a frame made the
// characters' job step two frames at a time, so game checks waiting for one
// animation frame (a pin's count, a move's hit) could miss it. In progress:
// only with the test aid SVR2011_TEST_HALF_30=1 for now (else 30 fps as
// before, two updates a frame).
void SetFrameClock() {
  const int fps = g_lockstep ? 60 : g_scene_thirty ? 30 : svr2011::FrameRateNow();
  static const bool half_30 = std::getenv("SVR2011_TEST_HALF_30") != nullptr;
  const bool half = fps == 30 && half_30 && svr2011::native::Enabled();
  svr2011::native::SetHalfFrames(half);
  rex::cvar::SetFlagByName("guest_vblank_hz", std::to_string(half ? 60 : fps));
  static int last = -1;
  if (int(half) * 1000 + fps != last) {
    last = int(half) * 1000 + fps;
    REXLOG_INFO("frame rate: the game draws {}{}", fps, half ? " (60 Hz, every other frame shown)" : "");
  }
}

void StartDeveloperAids(uint8_t* base);

}  // namespace

namespace svr2011 {

int TargetFrameRate() {
  return REXCVAR_GET(frame_rate) == 30 ? 30 : 60;  // (older settings' 120 / 144 / 240: 60)
}

int FrameRateNow() {
  // (test aid: SVR2011_TEST_IN_MATCH=1 - the chosen rate everywhere, e.g. the training ring)
  static const bool always = std::getenv("SVR2011_TEST_IN_MATCH") != nullptr;
  if (!REXCVAR_GET(full_speed)) return 60;  // (30 needs two updates a frame)
  return g_in_match || always ? TargetFrameRate() : 60;
}

void SetFrameRateInMatch(bool on) {
  if (!on) g_match_start = nullptr;
  if (g_in_match.exchange(on) != on) SetFrameClock();
}

bool InMatch() { return g_in_match.load(); }

void ArmMatchStart(void (*on_start)()) { g_match_start = on_start; }

void InstallFrameRate(rex::memory::Memory* memory) {
  if (!memory) return;
  g_memory = memory;
  g_base = memory->virtual_membase();
  SetFrameClock();
  REXLOG_INFO("frame rate: {} fps (the game draws {})", TargetFrameRate(), FrameRateNow());
  StartDeveloperAids(g_base);
}

// The timing block for `fps` frames a second.
void WriteTiming(uint8_t* base, int fps) {
  uint8_t* t = base + kTiming;
  Wr32(t + 8, uint32_t(fps));
  WrF(t + 28, 1.0f / float(fps));
  WrF(t + 32, float(fps));
  WrF(t + 36, 1000.0f / float(fps));
  Wr32(t + 40, uint32_t(1000 / fps));
  WrF(t + 60, 60.0f / float(fps));
  WrF(t + 64, float(fps) / 60.0f);
}

void ApplyFrameRate(uint8_t* base) { WriteTiming(base, 60); }

bool Lockstep() { return g_lockstep.load(); }

void SetLockstep(bool on) {
  if (g_lockstep.exchange(on) == on) return;
  SetFrameClock();
  REXLOG_INFO("frame rate: online lockstep {}", on ? "on (one world update a frame, 60 Hz)" : "off");
}

void SetSceneThirtyFps(bool on) {
  if (g_scene_thirty.exchange(on) != on) SetFrameClock();
}

// GRAPHICS -> FRAME RATE: the chosen rate, now.
void SetTargetFrameRate(int fps) {
  rex::cvar::SetFlagByName("frame_rate", std::to_string(fps));
  SetFrameClock();
  REXLOG_INFO("frame rate: {} fps (GRAPHICS; the game draws {})", TargetFrameRate(), FrameRateNow());
}

}  // namespace svr2011

// -- The world at 60 Hz ------------------------------------------------------

namespace {

Clock::time_point g_world_last;
double g_world_acc = 0;  // 60 Hz ticks of real time not yet run
int g_world_ticks = 1;   // update passes this frame
int g_world_passes = 1;  // (the passes run: 0 while the free camera holds the match)

constexpr int kMaxTicks = 2;  // (below 30 fps the game slows down rather than racing: 3-4 updates a frame - a
                               // window in the background, tabbing out - crashed a worker thread, sub_8281F5F0)

uint32_t G32(uint8_t* base, uint32_t a) { return Rd32(base + a); }

}  // namespace

int svr2011::WorldPassesThisFrame() { return g_world_passes; }

bool svr2011::GuestReadable(uint32_t a) {
  if (!g_memory) return false;
  auto* heap = g_memory->LookupHeap(a);
  return heap && heap->QueryRangeAccess(a, a + 3) != rex::memory::PageAccess::kNoAccess;
}

namespace {
void P32(uint8_t* base, uint32_t a, uint32_t v) { Wr32(base + a, v); }

// What the world update is running: a watchdog logs it when the update is
// stuck (a wait that a frame with two updates would never see end).
std::atomic<uint32_t> g_dbg_obj{0}, g_dbg_fn{0}, g_dbg_pass{0};
std::atomic<rex::system::KernelState*> g_kernel{nullptr};

// Every guest thread: id, name, where it is (lr) and its call stack (the
// back chain from r1: saved lr at each frame's -8) - for a stuck update. Only
// reads inside the thread's own stack (its PCR: +0x70 base, high; +0x74 end,
// low), and stops at the first link that leaves it or doesn't go up.

void DumpGuestThreads(uint8_t* base) {
  auto* kernel = g_kernel.load();
  if (!kernel || !g_memory) return;
  // (a stack's range can hold pages never mapped - its untouched end: each
  // word is read only from a readable page)
  const auto readable = [](uint32_t a) {
    auto* heap = g_memory->LookupHeap(a);
    return heap && heap->QueryRangeAccess(a, a + 3) != rex::memory::PageAccess::kNoAccess;
  };
  for (auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!t || !t->is_guest_thread() || !t->thread_state() || !t->pcr_ptr()) continue;
    const PPCContext* c = t->thread_state()->context();
    const uint32_t hi = Rd32(base + t->pcr_ptr() + 0x70), lo = Rd32(base + t->pcr_ptr() + 0x74);
    const auto in_stack = [&](uint32_t a) { return lo < hi && a >= lo && a + 4 <= hi && readable(a); };
    std::string chain = fmt::format("{:08X}", uint32_t(c->lr));
    uint32_t sp = c->r1.u32;
    for (int i = 0; i < 16 && in_stack(sp); ++i) {
      const uint32_t prev = Rd32(base + sp);
      if (prev <= sp || !in_stack(prev) || !in_stack(prev - 8)) break;
      chain += fmt::format(" {:08X}", Rd32(base + prev - 8));
      sp = prev;
    }
    // (r3: in a wait - RtlEnterCriticalSection, a wait for an object - what it waits on)
    REXLOG_WARN("frame rate: thread {:08X} '{}' at {} (r3 {:08X})", t->thread_id(), t->name(), chain, c->r3.u32);
  }
  // The game's heap locks held (a critical section per heap, 28 bytes each from 0x82EC55B0: +0x10 lock count,
  // +0x14 recursion, +0x18 the owner's KTHREAD): a stuck update waiting on one (2.1.7 reports 228dce / 9ce92b: the
  // world update freeing in sub_8269B2D0 -> sub_8269AAD8, RtlEnterCriticalSection, after a match) shows its owner.
  // A busy one with no owner too (lock count not -1: 2.1.8 report 73e349 - the world update held heap 1 and waited
  // on a lock no thread was listed as holding).
  std::string held;
  for (uint32_t h = 0; h < 64; ++h) {
    const uint32_t cs = 0x82EC55B0 + h * 28, owner = Rd32(base + cs + 0x18);
    int32_t waiting = 0;  // (the lock count is the host's own int, updated atomically - not big-endian)
    std::memcpy(&waiting, base + cs + 0x10, 4);
    if ((!owner && waiting == -1) || base[cs] != 1) continue;  // (free; or not a critical section's event header)
    uint32_t id = 0;
    for (auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>())
      if (t && owner && t->guest_object() == owner) id = t->thread_id();
    held += fmt::format(" [heap {} at {:08X}: thread {:08X} (KTHREAD {:08X}), lock count {}, recursion {}]", h, cs, id,
                        owner, waiting, int32_t(Rd32(base + cs + 0x14)));
  }
  REXLOG_WARN("frame rate: heap locks held:{}", held.empty() ? std::string(" none") : held);
}
std::atomic<int64_t> g_dbg_since{0};

// obj->vtable[slot](obj, r4, r5)
void Virt(PPCContext& ctx, uint8_t* base, uint32_t obj, int slot, uint32_t r4 = 0, uint32_t r5 = 0) {
  g_dbg_obj = obj;
  g_dbg_fn = G32(base, G32(base, obj) + slot * 4);
  g_dbg_since = Clock::now().time_since_epoch().count();
  ctx.r3.u64 = obj;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.ctr.u64 = G32(base, G32(base, obj) + slot * 4);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// The manager's systems (list +0..+8, then +68): their slot `slot`.
void Systems(PPCContext& ctx, uint8_t* base, uint32_t m, int slot, bool scheduler_first) {
  // (as the original: list ends read once before each loop, +68 read when used)
  if (scheduler_first)
    if (const uint32_t o = G32(base, m + 68)) Virt(ctx, base, o, slot);
  for (uint32_t p = G32(base, m + 0), end = G32(base, m + 8); p != end; p += 4) {
    const uint32_t o = G32(base, p);
    if (o != 0xFFFFFFFF) Virt(ctx, base, o, slot);
  }
  if (!scheduler_first)
    if (const uint32_t o = G32(base, m + 68)) Virt(ctx, base, o, slot);
}

// Drops the removed (-1) entries of the list at `l` (begin +0, end +8, dirty +12).
void Compact(uint8_t* base, uint32_t l) {
  if (G32(base, l + 12) == 0) return;
  uint32_t out = G32(base, l);
  for (uint32_t p = G32(base, l); p != G32(base, l + 8); p += 4)
    if (const uint32_t v = G32(base, p); v != 0xFFFFFFFF) P32(base, out, v), out += 4;
  P32(base, l + 8, out);
  P32(base, l + 12, 0);
}

// Research aid (with SVR2011_TEST_LOST_TRACE): a watched guest word (the
// pin judge's count, set by its hook) - each task or system call that
// changes it is logged.
bool g_extra_update = false;  // (see below: an extra update of this frame)

// The characters' job's hand-offs, the last 512 (all threads), dumped with a
// stuck world update - for the "stuck in pass 20" freeze (Android, 4+ people
// at 2 updates a frame). Kinds: F frame (a ticks), R/r round run/skipped (a
// job +92, b +104), P/p paused round run/skipped, E/e its second half, J job
// run (a ticks), W/w job wait in/out (a idx, b flag), O owed round before
// the draw (a 1 round, 0 paused).
struct JobEvent {
  uint32_t frame;
  char kind;
  uint8_t pass, extra, thread;
  uint32_t a, b;
};
JobEvent g_job_events[512];
std::atomic<uint32_t> g_job_event_next{0};
std::atomic<uint32_t> g_job_frame{0};
std::atomic<int> g_dbg_pass_now{0};
void JobEv(char kind, uint32_t a = 0, uint32_t b = 0, uint8_t thread = 0) {
  const uint32_t i = g_job_event_next.fetch_add(1) % 512;
  g_job_events[i] = {g_job_frame.load(), kind, uint8_t(g_dbg_pass_now.load()), uint8_t(g_extra_update), thread, a, b};
}
void DumpJobEvents() {
  const uint32_t end = g_job_event_next.load();
  std::string line;
  for (uint32_t i = end > 160 ? end - 160 : 0; i < end; ++i) {
    const JobEvent& e = g_job_events[i % 512];
    line += fmt::format(" {}{}{}:{}/{:X}/{:X}", e.frame % 1000, e.kind, e.extra ? "x" : "", e.pass, e.a, e.b);
    if (line.size() > 900) {
      REXLOG_WARN("frame rate: job events{}", line);
      line.clear();
    }
  }
  if (!line.empty()) REXLOG_WARN("frame rate: job events{}", line);
}
std::atomic<uint32_t> g_watch{0};
uint32_t g_watch_last = 0;
void WatchCheck(uint8_t* base, const char* where, uint32_t obj, uint32_t fn) {
  const uint32_t a = g_watch.load();
  if (!a) return;
  const uint32_t v = Rd32(base + a);
  if (v == g_watch_last) return;
  REXLOG_INFO("lost trace: watch {:08X} {} -> {} in {} (object {:08X} function {:08X}), frame {}{}", a, int32_t(g_watch_last),
              int32_t(v), where, obj, fn, Rd32(base + kMatchFrames), g_extra_update ? " (extra update)" : "");
  g_watch_last = v;
}

void UpdatePass(PPCContext& ctx, uint8_t* base, uint32_t m) {
  WatchCheck(base, "before the update", 0, 0);
  Systems(ctx, base, m, 1, false);
  WatchCheck(base, "the systems' slot 1", 0, 0);
  P32(base, m + 96, 1);
  const uint32_t groups = (G32(base, m + 40) - G32(base, m + 32)) >> 4;
  for (uint32_t g = 0; g < groups; ++g) {
    if (const uint32_t scheduler = G32(base, m + 68)) {
      Virt(ctx, base, scheduler, 4, g, G32(base, m + 80));
      if (ctx.r3.u32 == 0) continue;
    }
    const uint32_t list = G32(base, m + 32) + g * 16;
    for (uint32_t p = G32(base, list), end = G32(base, list + 8); p != end; p += 4) {
      const uint32_t t = G32(base, p);
      if (t != 0xFFFFFFFF && G32(base, t + 8) != 0) {
        Virt(ctx, base, t, 1);
        WatchCheck(base, "a task", t, g_dbg_fn.load());
      }
    }
  }
  ctx.r3.u64 = m;
  sub_8269C648(ctx, base);  // (the groups' lists)
  P32(base, m + 96, 0);
}

void DrawPass(PPCContext& ctx, uint8_t* base, uint32_t m) {
  Systems(ctx, base, m, 2, false);
  P32(base, m + 100, 1);
  constexpr uint32_t kRenderQueue = 0x82ED6030;
  ctx.r3.u64 = G32(base, kRenderQueue);
  sub_826A6018(ctx, base);
  ctx.r3.u64 = G32(base, kRenderQueue);
  sub_826A6080(ctx, base);
  P32(base, m + 100, 0);
  ctx.r3.u64 = m;
  sub_8269C6B8(ctx, base);
  Compact(base, m + 16);
  Systems(ctx, base, m, 3, true);
}

// The controllers are read in the update pass (a system's slot 1 ->
// sub_826CA070 -> XamInputGetState through sub_82905058), and some menus look
// at the presses after the update. With two updates in a frame a press would
// be new in the first and held in the second - lost to those menus - so all
// but the last update of a frame (an extra update) see the previous reading
// again (no change): a press arrives in the frame's last update.

std::atomic<uint64_t> g_lat_drawn{0};  // (latency test aid: frames drawn)
// Render-thread commands the logic thread queued this frame (see
// sub_8269B2D0 below).
std::vector<uint32_t> g_frame_commands;
std::atomic<std::thread::id> g_logic_thread{};  // (the thread running the world update)
// The characters' job's round (sub_8216F4C8, below) or its paused round
// (sub_8216ED38 + sub_8216E458) an extra update left, for the frame's last
// update - or, when that one reached neither, for before the draw.
enum class JobOwed { kNone, kRound, kPaused };
JobOwed g_job_owed = JobOwed::kNone;
uint32_t g_job_paused_obj = 0;   // (sub_8216ED38's r3)
bool g_job_paused_e458 = false;  // (sub_8216E458 followed it)
struct InputReading {
  bool valid = false;
  uint32_t result = 0;
  uint8_t state[16] = {};  // XINPUT_STATE
};
InputReading g_input[8];

// Test aid: SVR2011_TEST_MATCH_TIME=<s> - 20 s into a match its time jumps to
// <s> (a timed match then ends on its own).
void TestMatchTime(uint8_t* base) {
  static const double test_time = [] {
    const char* v = std::getenv("SVR2011_TEST_MATCH_TIME");
    return v ? std::atof(v) : 0.0;
  }();
  if (test_time <= 0) return;
  static uint32_t last = 0;
  static bool jumped = false;
  const uint32_t frames = G32(base, kMatchFrames);
  if (frames < last) jumped = false;  // (a new match)
  last = frames;
  if (!jumped && frames > 20 * 60 && frames < test_time * 60) {
    jumped = true;
    P32(base, kMatchFrames, uint32_t(test_time * 60));
    REXLOG_INFO("frame rate: test - match time set to {} s", test_time);
  }
}

}  // namespace

// The world update, once a frame of the game's loop: how many 60 Hz ticks
// this frame runs (at least one).
REX_EXTERN(__imp__sub_8269D768);
REX_HOOK_RAW(sub_8269D768) {
  svr2011::PerfHintThread();  // (android_perf.h: the world update's thread in the phone's hint session)
  if (!g_kernel) g_kernel = REX_KERNEL_STATE();  // (for the stuck-update thread dump)
  // Test aid: SVR2011_TEST_GUEST_FAULT=<seconds> - that long after the first world update, one write to guest
  // address 0 (unmapped: a fault no handler takes) - proves guest_faults.cpp: one log line with the place, then a
  // crash report and the game closes (Android: no endless retry).
  static const int fault_secs = [] { const char* v = std::getenv("SVR2011_TEST_GUEST_FAULT"); return v ? std::atoi(v) : 0; }();
  if (fault_secs > 0) {
    static const auto first = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - first >= std::chrono::seconds(fault_secs)) {
      REXLOG_WARN("frame rate: SVR2011_TEST_GUEST_FAULT - writing guest address 0 now");
      rex::FlushLogging();
      *reinterpret_cast<volatile uint32_t*>(base) = 0x0DEFACED;
    }
  }
  // Test aid: SVR2011_TEST_HEAP_LOCK_SHOW=<heap> (with SVR2011_TEST_DUMP_AT=<s>): from 2 s before the dump to 2 s
  // after it, that heap's lock names this thread (the world update's) as its owner - the lock itself left free, so a
  // real taker just writes its own - and the dump's "heap locks held" line shows it.
  if (static const char* v = std::getenv("SVR2011_TEST_HEAP_LOCK_SHOW"); v && *v) {
    static const int dump_at = [] { const char* d = std::getenv("SVR2011_TEST_DUMP_AT"); return d ? std::atoi(d) : 0; }();
    static const auto t0 = Clock::now();
    static int state = 0;  // (0 before, 1 shown, 2 done)
    const uint32_t cs = 0x82EC55B0 + uint32_t(std::atoi(v)) * 28;
    const auto at = std::chrono::duration<double>(Clock::now() - t0).count();
    if (state == 0 && dump_at > 2 && at > dump_at - 2) {
      state = 1;
      const uint32_t me = rex::system::XThread::GetCurrentThread()->guest_object();
      for (int k = 0; k < 4; ++k) base[cs + 0x18 + k] = uint8_t(me >> (24 - 8 * k));
      REXLOG_WARN("frame rate: test - heap {}'s lock shown as owned by this thread (KTHREAD {:08X})", std::atoi(v), me);
    } else if (state == 1 && at > dump_at + 2) {
      state = 2;
      for (int k = 0; k < 4; ++k) base[cs + 0x18 + k] = 0;
    }
  }
  // Test aid: SVR2011_TEST_SLOW_MS=<ms> - a slow PC (each frame that much longer).
  static const int slow_ms = [] { const char* v = std::getenv("SVR2011_TEST_SLOW_MS"); return v ? std::atoi(v) : 0; }();
  // (SVR2011_TEST_SLOW_IN_MATCH=1: only while a match runs - the menus at speed for the scripted route)
  // (=2: only while the match's clock runs - the highlights and replays at speed)
  static const char* slow_in_match = std::getenv("SVR2011_TEST_SLOW_IN_MATCH");
  static uint32_t slow_clock = 0;
  const bool clock_runs = G32(base, kMatchFrames) != slow_clock;
  slow_clock = G32(base, kMatchFrames);
  if (slow_ms > 0 && (!slow_in_match || (*slow_in_match == '2' ? clock_runs : g_in_match.load())))
    std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
  const auto now = Clock::now();
  if (g_world_last == Clock::time_point{}) g_world_last = now - std::chrono::microseconds(16667);
  g_world_acc += std::chrono::duration<double>(now - g_world_last).count() * 60.0;
  g_world_last = now;
  g_world_ticks = std::clamp(int(g_world_acc), 1, kMaxTicks);
  g_world_acc = std::clamp(g_world_acc - g_world_ticks, -0.5, 1.0);
  if (g_lockstep || !REXCVAR_GET(full_speed)) g_world_ticks = 1, g_world_acc = 0;
#if SVR2011_DEBUG_SUITE
  if (const int n = svr2011::dbg::SpeedTicks(); n && !g_lockstep) g_world_ticks = n, g_world_acc = 0;  // (dbg_speed)
#endif
  if (const int n = svr2011::MatchReplayPasses(); n > 0) g_world_ticks = n, g_world_acc = 0;  // (a replay: as recorded)
  // 30 fps at 60 Hz (SetHalfFrames): while frames take two updates (a PC that
  // can't keep 60 Hz) every frame is shown - as 30 fps did before - and every
  // other one again once it keeps up (a running average of the updates).
  {
    static double avg_ticks = 1.0;
    static bool suspended = false;
    avg_ticks = avg_ticks * 0.95 + g_world_ticks * 0.05;
    const bool want = suspended ? avg_ticks > 1.1 : avg_ticks > 1.3;
    if (want != suspended) {
      suspended = want;
      svr2011::native::SetHalfSuspended(want);
      static int count = 0;
      if (svr2011::native::HalfFrames() && (++count <= 10 || count % 100 == 0))
        REXLOG_INFO("frame rate: 30 fps at 60 Hz - {} ({:.2f} updates a frame)",
                    want ? "every frame shown (the world can't keep 60 Hz)" : "every other frame shown again", avg_ticks);
    }
  }
  svr2011::HctpBeforeFrame(base);  // (match_types.h: a door's placements held)
  svr2011::RtwmBeforeFrame(base);  // (rtwm_streak.h: test aids)
  __imp__sub_8269D768(ctx, base);
#if SVR2011_DEBUG_SUITE
  svr2011::dbg::BootFrame(ctx, base);  // (dbg_boot: debug_suite.h)
  svr2011::dbg::ClockFrame();          // (dbg_state_clock)
#endif
  TestMatchTime(base);
  svr2011::MatchTypesUpdate(ctx, base);  // (match_types.h: the lumberjacks, Slobber Knocker)
  svr2011::HctpRoomUpdate(ctx, base);    // (match_types.h: an HCTP backstage room's gimmicks)
  svr2011::JukeboxUpdate(ctx, base);     // (jukebox.h: a song stopped, skipped or previewed)
  // An armed match start (ArmMatchStart): once the match's frame count has
  // gone up 30 updates running.
  static uint32_t last_frames = 0;
  static int counting = 0;
  const uint32_t frames = G32(base, kMatchFrames);
  counting = frames > last_frames ? counting + 1 : 0;
  last_frames = frames;
  if (counting >= 30)
    if (auto start = g_match_start.exchange(nullptr)) start();

}

// The task manager: the update pass per tick, the draw pass once (see top).
REX_EXTERN(__imp__sub_8269C728);
REX_HOOK_RAW(sub_8269C728) {
  g_logic_thread = std::this_thread::get_id();
  svr2011::NoteThreadRole("logic");  // (thread_cpu.h)
  const auto saved = ctx;
  const uint32_t m = ctx.r3.u32;
  constexpr uint32_t kOwner = 0x82EC5F94;
  const uint32_t owner = G32(base, kOwner);
  ctx.r3.u64 = owner + 108;
  REX_CALL_INDIRECT_FUNC(0x82D4753Cu);  // (RtlEnterCriticalSection)
  P32(base, owner + 136, 1);
  g_job_owed = JobOwed::kNone;
  g_frame_commands.clear();  // (last frame's ran at the barrier)
  g_job_frame.fetch_add(1);
  JobEv('F', uint32_t(g_world_ticks));
  // (free camera F8: the match held - no update passes, the frame drawn as it was; free_cam.h)
  const int passes = svr2011::FreeCamPaused() && !g_lockstep && svr2011::InMatch() ? 0 : g_world_ticks;
  g_world_passes = passes;
  for (int i = 0; i < passes; ++i) {
    g_dbg_pass_now = 10 + i;
    g_dbg_pass = 10 + i;
    g_extra_update = i + 1 < passes;
    UpdatePass(ctx, base, m);
  }
  g_extra_update = false;
  svr2011::FreeCamFrame(base);  // (free_cam.h)
  if (g_job_owed != JobOwed::kNone) {  // (only an extra update reached the job this frame)
    const auto before = ctx;
    const bool round = g_job_owed == JobOwed::kRound;
    g_dbg_pass_now = 19;
    JobEv('O', round ? 1 : 0);
    if (round) {
      sub_8216F4C8(ctx, base);
    } else {
      ctx.r3.u64 = g_job_paused_obj;
      sub_8216ED38(ctx, base);
      if (g_job_paused_e458) {
        ctx.r3.u64 = g_job_paused_obj;
        sub_8216E458(ctx, base);
      }
    }
    g_job_owed = JobOwed::kNone;
    ctx = before;
    static int late[2] = {};
    const int n = ++late[round];
    if (n == 1 || n % 100 == 0)
      REXLOG_INFO("frame rate: the characters' job {} ran before the draw ({} times)", round ? "round" : "paused round", n);
  }
  // Test aid: SVR2011_TEST_STUCK=1 - one update 5 s long, 60 s into the run
  // (the stuck-update log and its thread dump).
  static const bool test_stuck = std::getenv("SVR2011_TEST_STUCK") != nullptr;
  static const auto started = Clock::now();
  static bool stuck_done = false;
  if (test_stuck && !stuck_done && Clock::now() - started > std::chrono::seconds(60)) {
    stuck_done = true;
    g_dbg_pass = 20;
    g_dbg_since = Clock::now().time_since_epoch().count();
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  g_dbg_pass = 20;
  g_dbg_pass_now = 20;
  DrawPass(ctx, base, m);
  ++g_lat_drawn;
  g_dbg_pass = 0;
  g_dbg_since = Clock::now().time_since_epoch().count();
  static std::once_flag watchdog;
  // Test aid: SVR2011_TEST_HITCH_MS=<ms> - an update or draw pass running
  // longer than that: every guest thread's back chain (once per pass; what a
  // stutter waits on).
  static std::once_flag hitch;
  std::call_once(hitch, [] {
    const char* v = std::getenv("SVR2011_TEST_HITCH_MS");
    const int ms = v ? std::atoi(v) : 0;
    if (ms <= 0) return;
    std::thread([ms] {
      int64_t logged_for = -1;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t s = g_dbg_since.load();
        const auto since = Clock::time_point(Clock::duration(s));
        if (g_dbg_pass != 0 && s != logged_for && g_base && Clock::now() - since > std::chrono::milliseconds(ms)) {
          logged_for = s;
          REXLOG_WARN("frame rate: test - pass {} over {} ms - object {:08X} function {:08X}", g_dbg_pass.load(), ms,
                      g_dbg_obj.load(), g_dbg_fn.load());
          DumpGuestThreads(g_base);
        }
      }
    }).detach();
  });
  std::call_once(watchdog, [] {
    std::thread([] {
      // (test aid: SVR2011_TEST_DUMP_AT=<s> - every guest thread's back chain once, <s> s after the start)
      static const int dump_at = [] { const char* v = std::getenv("SVR2011_TEST_DUMP_AT"); return v ? std::atoi(v) : 0; }();
      const auto started = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        svr2011::MatchDiagTick(g_base, g_in_match.load());  // (wrestlers who stopped: match_diag.h)
        static bool dumped = false;
        if (dump_at > 0 && !dumped && g_base && Clock::now() - started > std::chrono::seconds(dump_at)) {
          dumped = true;
          REXLOG_INFO("frame rate: test - thread dump");
          DumpGuestThreads(g_base);
        }
        const auto since = Clock::time_point(Clock::duration(g_dbg_since.load()));
        if (g_dbg_pass != 0 && Clock::now() - since > std::chrono::seconds(3)) {
          std::string jobs;
          if (uint8_t* b = g_base) {
            const uint32_t q = Rd32(b + 0x82ED6030);
            const uint32_t n = q ? Rd32(b + q + 8) : 0;
            for (uint32_t i = 0; i < n && i < 16; ++i) {
              const uint32_t w = Rd32(b + Rd32(b + q + 4) + i * 4);
              if (w)
                jobs += fmt::format(" [{}: {:08X} sync {} fn {:08X} n {} +24 {:08X} +28 {:08X} +32 {:08X} +44 {:08X} +48 {:08X}]",
                                    i, w, Rd32(b + w + 20), Rd32(b + w + 52), Rd32(b + w + 16), Rd32(b + w + 24),
                                    Rd32(b + w + 28), Rd32(b + w + 32), Rd32(b + w + 44), Rd32(b + w + 48));
            }
          }
          REXLOG_WARN("frame rate: world update stuck {} s in pass {} (1x: update x+1, 20: draw) - object {:08X} "
                      "function {:08X}; jobs{}",
                      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - since).count(),
                      g_dbg_pass.load(), g_dbg_obj.load(), g_dbg_fn.load(), jobs);
          static int64_t dumped_for = -1;  // (once per stuck update)
          if (dumped_for != g_dbg_since.load() && g_base) {
            dumped_for = g_dbg_since.load();
            DumpGuestThreads(g_base);
            DumpJobEvents();
            if (g_in_match.load()) svr2011::LogMatchState(g_base, "world update stuck"), svr2011::MatchBoxLog("world update stuck");
          }
        }
      }
    }).detach();
  });
  const uint32_t owner_now = G32(base, kOwner);
  ctx.r3.u64 = owner_now + 108;
  REX_CALL_INDIRECT_FUNC(0x82D4754Cu);  // (RtlLeaveCriticalSection)
  P32(base, owner_now + 136, 0);
  ctx.r1 = saved.r1;
  ctx.lr = saved.lr;
}

// The characters' job ('CHPH': the wrestlers' poses and the physics world, on
// its own thread). sub_8216F4C8, from the update's group gate, waits for the
// job the last update started, takes its results and starts the next one;
// the job (sub_82171940 a run) steps by the timing block's 1/60. The job and
// the frame's draw depend on each other (double-buffered poses), so it runs
// once a frame, as the game expects, in the frame's last update: extra updates
// leave it, and the job does the work of every tick since it was last started
// (sub_82171940 that many times on the job thread). The update's gate runs
// either the round or, while the game's world is held (a finisher's
// cinematic, a match's end), the paused round (sub_8216ED38, sub_8216E458) -
// which starts the job too: both count as the frame's one start. Only when
// the frame's last update reached neither does what an extra update left run
// before the draw (which waits for the job). (2.0.1-2.0.2 ran a left round
// before the draw even after the last update's paused round - two starts: a
// draw frozen on job 826DFD40, a match end that never came. Running the round
// in an extra update itself freezes that update - sub_8243C568 waits for the
// draw.)
namespace {
std::atomic<int> g_job_ticks{1};  // ticks the next / running job stands for
int g_job_pending = 0;            // ticks since the job was last started
}  // namespace

REX_EXTERN(__imp__sub_8216F4C8);
REX_HOOK_RAW(sub_8216F4C8) {
  ++g_job_pending;
  // Research aid: SVR2011_TEST_JOB_EVERY=1 - the round in every update (the
  // deadlock it gives, with the stuck-update thread dump).
  static const bool every = std::getenv("SVR2011_TEST_JOB_EVERY") != nullptr;
  {
    const uint32_t j = G32(base, 0x82DE9C88);
    JobEv(g_extra_update && !every ? 'r' : 'R', j ? G32(base, j + 92) : 0xFFFF, j ? G32(base, j + 104) : 0xFFFF);
  }
  if (g_extra_update && !every) {  // (an extra update of this frame)
    g_job_owed = JobOwed::kRound;
    return;
  }
  g_job_owed = JobOwed::kNone;
  // (the running job must be done before its tick count changes: waited for
  // as the game does just after - the event stays set for it)
  constexpr uint32_t kJob = 0x82DE9C88;
  const auto saved = ctx;
  sub_8216E750(ctx, base);
  const uint32_t job = G32(base, kJob);
  if (ctx.r3.u32 == 0 && job && G32(base, job + 104) == 0 && G32(base, job + 92) != 0) {
    ctx.r3.u64 = G32(base, job + 276);
    ctx.r4.u64 = uint64_t(-1);
    sub_8215A8C0(ctx, base);
  }
  ctx = saved;
  g_job_ticks = std::clamp(g_job_pending, 1, kMaxTicks);
  g_job_pending = 0;
  __imp__sub_8216F4C8(ctx, base);
}

// The paused round (see above). Test aid SVR2011_TEST_JOB_LAST=1: 2.0.2's way
// (paused rounds in every update; a left round still runs before the draw).
REX_EXTERN(__imp__sub_8216ED38);
REX_HOOK_RAW(sub_8216ED38) {
  static const bool old_way = std::getenv("SVR2011_TEST_JOB_LAST") != nullptr;
  JobEv(g_extra_update && !old_way ? 'p' : 'P', ctx.r3.u32);
  if (g_extra_update && !old_way) {
    g_job_owed = JobOwed::kPaused, g_job_paused_obj = ctx.r3.u32, g_job_paused_e458 = false;
    return;
  }
  if (!old_way) g_job_owed = JobOwed::kNone;
  svr2011::ThreeStagesCinematic();  // (the world held: a finisher's / signature's cinematic - match_types.h)
#if SVR2011_DEBUG_SUITE
  svr2011::dbg::OnWorldHeld();  // (dbg_burst)
#endif
  // Research aid: SVR2011_TEST_PAUSE_LOG=1 - when the world starts being
  // held (a move's cinematic, a match's end), with the match frame.
  if (static const bool pause_log = std::getenv("SVR2011_TEST_PAUSE_LOG") != nullptr; pause_log) {
    static uint32_t last_frame = 0;
    const uint32_t f = g_job_frame.load();
    if (f > last_frame + 2) REXLOG_INFO("frame rate: test - the world is held from match frame {}", Rd32(base + kMatchFrames));
    last_frame = f;
  }
  __imp__sub_8216ED38(ctx, base);
}

REX_EXTERN(__imp__sub_8216E458);
REX_HOOK_RAW(sub_8216E458) {
  static const bool old_way = std::getenv("SVR2011_TEST_JOB_LAST") != nullptr;
  JobEv(g_extra_update && !old_way ? 'e' : 'E', ctx.r3.u32);
  if (g_extra_update && !old_way) {
    if (g_job_owed == JobOwed::kPaused) g_job_paused_e458 = true;
    return;
  }
  __imp__sub_8216E458(ctx, base);
}

REX_EXTERN(__imp__sub_82171940);
REX_HOOK_RAW(sub_82171940) {
  const auto saved = ctx;
  const int ticks = g_job_ticks.load();
  JobEv('J', uint32_t(ticks), 0, 1);
  for (int i = 0; i < ticks; ++i) {
    ctx = saved;
    __imp__sub_82171940(ctx, base);
  }
}

// XamInputGetState(user, state) for the game: sub_82905058.
REX_EXTERN(__imp__sub_82905058);
REX_HOOK_RAW(sub_82905058) {
  ctx.r3.u64 = ctx.r3.u32 + svr2011::PadUserOffset();  // (pads 5-8: players.h)
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  InputReading* r = user < 8 ? &g_input[user] : nullptr;
  if (g_extra_update && r && r->valid && out) {
    std::memcpy(base + out, r->state, sizeof(r->state));
    ctx.r3.u64 = r->result;
    return;
  }
  __imp__sub_82905058(ctx, base);
  if (out && ctx.r3.u32 == 0) svr2011::FreeCamPad(user, base + out);  // (flying: the game gets no input - free_cam.h)
  if (user == 0 && out && ctx.r3.u32 == 0) {
    svr2011::FastStartInput(base, out);  // (the start skips: fast_start.h)
#if SVR2011_DEBUG_SUITE
    svr2011::dbg::BootInput(base, out);  // (dbg_boot: a booted match's entrances skipped)
#endif
    // (the online overlay's BACK + RB never reaches the game: online_overlay.h)
    const uint16_t b = svr2011::OnlineOverlayPad(uint16_t(base[out + 4] << 8 | base[out + 5]));
    base[out + 4] = uint8_t(b >> 8), base[out + 5] = uint8_t(b);
    svr2011::OnlineRulesPad(b);  // (LB / RB: the PORT RULES row - online_rules.h)
  }
  if (out && ctx.r3.u32 == 0) svr2011::PadIconsInput(user, base + out);  // (who pressed last: pad_icons.h)
  if (out) {  // (a replay: the recording's pad this frame - match_record.h)
    uint32_t result = ctx.r3.u32;
    if (svr2011::MatchReplayPad(user, result, base + out)) ctx.r3.u64 = result;
  }
  if (r && out) {
    r->valid = true;
    r->result = ctx.r3.u32;
    std::memcpy(r->state, base + out, sizeof(r->state));
    svr2011::ArenaSelectPad(user, ctx.r3.u32 == 0 ? uint16_t((base[out + 4] << 8 | base[out + 5]) & ~0xC00 |
                                                             (base[out + 6] > 0x40 ? 0x400 : 0) |
                                                             (base[out + 7] > 0x40 ? 0x800 : 0))
                                                : 0);  // (arena_mods.h: LB/RB, LT/RT as 0x400/0x800)
    if (user == 0) svr2011::CheckModProfiles(base);  // (superstar_mods.h)
    svr2011::MatchRecordPad(user, ctx.r3.u32, base + out);  // (the match's inputs.bin: match_record.h)
  }
}

// A CPU's controller word (the AI's virtual pad): sub_82811B98(Y) hands the
// word at *(*(Y + 20) + 12) (high half the stick, low half the buttons) to
// its controller (Y + 16, through sub_827F4238 -> sub_8221D480: +72 the
// pressed edges = new & ~previous) and clears it, each update. With two
// updates a frame the characters' job sees only the last one's state, so a
// press the AI made in an extra update was an edge there and a held button
// (or nothing) in the last: the CPU often didn't start a run - it walked, or
// stood, where at 60 it ran (a player's report, 2.0.6, at 30 fps). Now, as
// with the players' pads (the XamInputGetState repeat above): an extra update
// gets the word the last update delivered again, and its own buttons wait
// for the frame's last update, delivered with that one's.
// (Test aids: SVR2011_TEST_CPU_PAD_OLD=1 the old way; SVR2011_TEST_CPU_PAD_LOG=1
// counts the words with buttons in extra / last updates, every 10 s.)
namespace {
struct CpuPad {
  uint32_t vpad = 0;       // (guest address of the word: one per CPU)
  uint32_t frame = 0;      // (g_job_frame when last seen)
  uint32_t last = 0;       // the word delivered in the last update of a frame
  uint16_t buttons = 0;    // the extra updates' buttons, for the last update
  uint16_t stick = 0;      // the extra updates' latest stick
  bool has_stick = false;
};
CpuPad g_cpu_pads[12];
}  // namespace
REX_EXTERN(__imp__sub_82811B98);
REX_HOOK_RAW(sub_82811B98) {
  static const bool old_way = std::getenv("SVR2011_TEST_CPU_PAD_OLD") != nullptr;
  static const bool log = std::getenv("SVR2011_TEST_CPU_PAD_LOG") != nullptr;
  const uint32_t y = ctx.r3.u32, links = y ? Rd32(base + y + 20) : 0, vpad = links ? Rd32(base + links + 12) : 0;
  if (old_way || !vpad || std::this_thread::get_id() != g_logic_thread.load()) {
    __imp__sub_82811B98(ctx, base);
    return;
  }
  const uint32_t frame = g_job_frame.load(), w = Rd32(base + vpad);
  CpuPad* e = nullptr;
  for (CpuPad& c : g_cpu_pads)
    if (c.vpad == vpad) e = &c;
  if (!e) {  // (a new one: the oldest's place)
    e = &g_cpu_pads[0];
    for (CpuPad& c : g_cpu_pads)
      if (c.frame < e->frame) e = &c;
    *e = CpuPad{vpad};
  }
  if (frame - e->frame > 2) e->last = 0, e->buttons = 0, e->has_stick = false;  // (not seen for a while: a new match)
  e->frame = frame;
  if (log && (w & 0xFFFF)) {
    static uint32_t counts[2];
    static int64_t at = 0;
    ++counts[g_extra_update];
    if (const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        now - at > 10000) {
      at = now;
      REXLOG_INFO("frame rate: CPU pad words with buttons - {} in extra updates, {} in last updates", counts[1], counts[0]);
    }
  }
  if (g_extra_update) {
    e->buttons |= uint16_t(w);
    if (w >> 16) e->stick = uint16_t(w >> 16), e->has_stick = true;
    Wr32(base + vpad, e->last);  // (the controller sees last frame's word again: no new edge)
  } else {
    const uint32_t stick = (w >> 16) ? (w >> 16) : e->has_stick ? e->stick : 0;
    const uint32_t word = stick << 16 | uint16_t(w | e->buttons);
    Wr32(base + vpad, word);
    e->last = word;
    e->buttons = 0, e->has_stick = false;
  }
  __imp__sub_82811B98(ctx, base);
}

// -- Developer aids ----------------------------------------------------------

namespace {

void StartDeveloperAids(uint8_t* base) {
  // SVR2011_FPS_PROBE=1 logs, every second, the match frames (60 a second at
  // the game's speed), the frames shown and wrestler 1's position.
  if (const char* v = std::getenv("SVR2011_FPS_PROBE"); v && (*v == '1' || *v == '2')) {
    std::thread([base, v] {
      uint32_t last = Rd32(base + kMatchFrames);
      uint64_t last_presents = rex::ui::HostPresentCount(), last_new = rex::ui::HostNewGuestFramePresentCount();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint32_t frames = Rd32(base + kMatchFrames);
        const uint32_t ch = Rd32(base + kChars), ch2 = Rd32(base + kChars + 4);
        const float x = ch ? RdF(base + ch + 288) : 0, z = ch ? RdF(base + ch + 296) : 0;
        const float x2 = ch2 ? RdF(base + ch2 + 288) : 0, z2 = ch2 ? RdF(base + ch2 + 296) : 0;
        const uint64_t presents = rex::ui::HostPresentCount(), shown = rex::ui::HostNewGuestFramePresentCount();
        REXLOG_INFO("fps probe: {} game frames/s, {} shown ({} presents), wrestler 1 at ({:.1f}, {:.1f}), 2 at "
                    "({:.1f}, {:.1f})",
                    frames - last, shown - last_new, presents - last_presents, x, z, x2, z2);
        if (*v == '2') {  // (SVR2011_FPS_PROBE=2: all six, x y z)
          std::string all;
          for (uint32_t i = 0; i < 6; ++i)
            if (const uint32_t c = Rd32(base + kChars + i * 4))
              all += fmt::format(" {}:({:.0f},{:.0f},{:.0f} ai {})", i, RdF(base + c + 288), RdF(base + c + 292),
                                 RdF(base + c + 296), Rd32(base + c + 2572) ? Rd32(base + Rd32(base + c + 2572) + 168) : 99);
          REXLOG_INFO("fps probe: all{}", all);
        }
        last = frames;
        last_presents = presents;
        last_new = shown;
      }
    }).detach();
  }
  // SVR2011_FPS_DUMP=<file> appends, every 100 ms, the real time, the match
  // frames and the first 64 KB of wrestlers 1 and 2 (tools/rate_diff.py
  // compares two runs).
  if (const char* path = std::getenv("SVR2011_FPS_DUMP"); path && *path) {
    std::thread([base, file = std::string(path)] {
      constexpr uint32_t kSize = 0x10000;
      const auto t0 = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t a = Rd32(base + kChars), b = Rd32(base + kChars + 4);
        if (!a || !b) continue;
        if (FILE* f = std::fopen(file.c_str(), "ab")) {
          const double t = std::chrono::duration<double>(Clock::now() - t0).count();
          const uint32_t frames = Rd32(base + kMatchFrames);
          std::fwrite(&t, 8, 1, f);
          std::fwrite(&frames, 4, 1, f);
          std::fwrite(&a, 4, 1, f);
          std::fwrite(&b, 4, 1, f);
          std::fwrite(base + a, 1, kSize, f);
          std::fwrite(base + b, 1, kSize, f);
          std::fclose(f);
        }
      }
    }).detach();
  }
}

}  // namespace


// A frame's callbacks for another thread (sub_826E0F78 queues them,
// sub_826E1260 runs them once a frame): with two updates in a frame each one
// is queued twice, and the second update may let go of what the first one's
// entry points at. sub_8217A718 (queued by sub_8217BC90 with the object at
// *0x82DEA30C) takes the object's part at +156 - crashed on a PC at 30 fps
// (2.0.2) when it was already gone: without it there is nothing to do.
REX_EXTERN(__imp__sub_8217A718);
REX_HOOK_RAW(sub_8217A718) {
  if (ctx.r3.u32 == 0 || Rd32(base + ctx.r3.u32 + 156) == 0) {
    static int skipped = 0;
    if (++skipped == 1 || skipped % 100 == 0)
      REXLOG_INFO("frame rate: a queued callback's object was gone - skipped ({} times)", skipped);
    return;
  }
  __imp__sub_8217A718(ctx, base);
}

// The replay recorder's callbacks: sub_8217BC90 (the replay object at
// *0x82DEA30C, each update) queues two (sub_826E0F78(queue, fn, fn2, obj,
// kind)): 0x8217AF30/0x8217AF38 (sub_8217AC50 records a frame or steps the
// playback, sub_8217AB58 the playback's side) and 0x827ACA10/0x8217A718 (the
// handshake with the update's side, sub_821DA3F0). With two updates in a
// frame both were queued twice and run back to back - the recorder and the
// playback lost step with the handshake: at 30 fps the replays showed the
// match's first moments, nearly still, and the highlights had one clip (a
// player's report, 2.0.4). Queued once a frame, in its last update, as at 60
// (the recording then holds a frame per frame drawn).
// Played back so, a recording made at two updates a frame (a slow PC, or 30
// fps) ran twice as fast once the replay itself kept 60 (two players'
// reports, 2.1.3). The recorder runs the same way for both (+92, its mode,
// stays 1), so: while the match's clock runs, it records - the updates each
// recorded frame stood for are counted; while it stands (the highlights, a
// replay), the playback steps once per that many updates. At 60 nothing
// changes (one update a frame, a step each).
static bool g_replay_step = true;  // (this frame's step: its two callbacks queued)
REX_EXTERN(__imp__sub_826E0F78);
REX_HOOK_RAW(sub_826E0F78) {
  const uint32_t obj = ctx.r6.u32;
  if ((ctx.r5.u32 == 0x8217AF38 || ctx.r5.u32 == 0x8217A718) && obj && obj == Rd32(base + 0x82DEA30C) &&
      std::this_thread::get_id() == g_logic_thread.load()) {
    if (g_extra_update) {
      static int skipped = 0;
      if (++skipped == 1 || skipped % 1000 == 0)
        REXLOG_INFO("frame rate: the replay recorder's callbacks queued once a frame ({} skipped)", skipped);
      return;
    }
    if (ctx.r5.u32 == 0x8217AF38) {  // (the first of the two, once a frame)
      static double rec_updates = 0, rec_frames = 0;  // (the recording: updates, frames)
      static double play_acc = 0;
      static int play_frames = 0, play_steps = 0;  // (for the log)
      static uint32_t last_clock = 0;
      static int phase = -1;  // 1 recording, 0 playing
      const uint32_t clock = G32(base, kMatchFrames);
      if (clock < last_clock) rec_updates = rec_frames = 0;  // (a new match)
      const int now = clock != last_clock ? 1 : 0;
      last_clock = clock;
      const double per = std::clamp(rec_frames > 0 ? rec_updates / rec_frames : 1.0, 1.0, double(kMaxTicks));
      if (now != phase) {
        const uint32_t part = Rd32(base + obj + 156);
        REXLOG_INFO("frame rate: replay recorder {} ({:.0f} frames recorded, {:.2f} updates each; {} steps in {} "
                    "frames played; mode {} part {})",
                    now ? "recording" : "playing", rec_frames, per, play_steps, play_frames, Rd32(base + obj + 92),
                    part ? Rd32(base + part + 1504) : 0);
#if SVR2011_DEBUG_SUITE
        if (now && rec_frames == 0) svr2011::dbg::OnMatchStarted();  // (a new match's first recording: the cues' zero)
#endif
        phase = now;
        play_acc = 0;
        play_frames = play_steps = 0;
      }
      if (now) {
        rec_updates += g_world_ticks;
        rec_frames += 1;
        g_replay_step = true;
      } else {
        play_acc += g_world_ticks;
        g_replay_step = play_acc >= per - 0.01;
        if (g_replay_step) play_acc -= per, ++play_steps;
        if (++play_frames % 600 == 0)
          REXLOG_INFO("frame rate: replay recorder playing - {} steps in {} frames", play_steps, play_frames);
      }
    }
    if (!g_replay_step) return;  // (playing: this frame shows the last one again)
  }
  __imp__sub_826E0F78(ctx, base);
}

// Its sibling sub_8217AB58 (the same object, +96 its state 1-5) also reads
// +156's part (+1504, or +1496 at states 2-3) unchecked - crashed so in a
// match at 60 fps (2.0.5 tests, guest read of 0x5E0); its other paths skip a
// missing part, so without it there is nothing to do here either.
REX_EXTERN(__imp__sub_8217AB58);
REX_HOOK_RAW(sub_8217AB58) {
  if (ctx.r3.u32 == 0 || Rd32(base + ctx.r3.u32 + 156) == 0) {
    static int skipped = 0;
    if (++skipped == 1 || skipped % 100 == 0)
      REXLOG_INFO("frame rate: a queued callback's object was gone - skipped ({} times, AB58)", skipped);
    return;
  }
  __imp__sub_8217AB58(ctx, base);
}

// A pure virtual call (sub_828F3F38, the runtime's _purecall: error R6025,
// the game quits): with two updates in a frame the second may destroy an
// object the first queued for the render thread (render command 24,
// sub_826DE6B0: obj->vtable[6] there), its vtable then the base class's - a
// PC at 30 fps crashed so (2.0.2). The call on the destroyed object is
// skipped instead (the caller's lr and object logged).
REX_EXTERN(__imp__sub_828F3F38);
REX_HOOK_RAW(sub_828F3F38) {
  static int count = 0;
  if (++count <= 20)
    REXLOG_WARN("frame rate: a pure virtual call skipped (caller {:08X}, object {:08X}) - {} so far", uint32_t(ctx.lr),
                ctx.r3.u32, count);
  ctx.r3.u64 = 0;
}

// The job system's "wait for job idx" (sub_8216A450(jobs, idx): if job
// idx's pending flag (+(idx+14)*4) is set, wait on its event (+(idx+5)*4)
// for ever, then clear the flag). In an extra update a task may wait so for
// the characters' job, which only the frame's last update starts: a world
// update stuck for ever (a player tabbing out of fullscreen - long frames,
// 3-4 updates each - froze in pass 12, function 82225508, c27d191). There
// the wait is at most 50 ms: a running job ends well within it, as before;
// one not started, the flag stays for the frame's last update or the draw.
REX_EXTERN(__imp__sub_8216A450);
//
// The lost request (the "stuck in pass 20" freeze: Fold, PC rumble): a job
// is asked for by sub_8216A3A8(jobs, character): its event (+(idx+5)*4)
// reset, the wait flag (+(idx+14)*4) 1, the request flag (+(idx+23)*4) 0
// and the worker's wake event (+16) set. The worker (sub_8216E098) runs each
// slot whose request flag is 0, sets its event - then the request flag 1.
// Between those two steps the waiter can wake, and the slot be asked for
// again: the worker's late "1" then wipes that new request - nobody runs it
// and the next wait is for ever (more likely with two updates a frame). So a
// wait here is in 200 ms slices: no event yet but the request flag 1 (the
// worker thinks it done) - it's asked for again (flag 0, wake the worker).
REX_HOOK_RAW(sub_8216A450) {
  if (!g_extra_update || std::this_thread::get_id() != g_logic_thread.load()) {
    const uint32_t jobs = ctx.r3.u32, idx = ctx.r4.u32;
    const bool logic = std::this_thread::get_id() == g_logic_thread.load();
    const uint32_t flag = jobs + (idx + 14) * 4, request = jobs + (idx + 23) * 4;
    JobEv('W', idx, Rd32(base + flag), logic ? 0 : 2);
    if (Rd32(base + flag) != 0) {
      const auto saved = ctx;
      for (;;) {
        ctx.r3.u64 = Rd32(base + jobs + (idx + 5) * 4);
        ctx.r4.u64 = 200;  // (ms)
        sub_8215A8C0(ctx, base);
        if (ctx.r3.u32 == 0) break;  // (signalled: done)
        if (Rd32(base + request) != 0) {  // (the worker took no request: it was lost)
          Wr32(base + request, 0);
          ctx.r3.u64 = Rd32(base + jobs + 16);
          sub_8215ADE0(ctx, base);
          static int lost = 0;
          if (++lost <= 20 || lost % 100 == 0)
            REXLOG_WARN("frame rate: job {} was asked for but lost - asked again ({} times)", idx, lost);
          JobEv('L', idx, 0, logic ? 0 : 2);
        }
      }
      ctx = saved;
      Wr32(base + flag, 0);
    }
    JobEv('w', idx, Rd32(base + flag), logic ? 0 : 2);
    return;
  }
  JobEv('W', ctx.r4.u32, Rd32(base + ctx.r3.u32 + (ctx.r4.u32 + 14) * 4));
  const uint32_t jobs = ctx.r3.u32, idx = ctx.r4.u32;
  const uint32_t flag = jobs + (idx + 14) * 4;
  if (Rd32(base + flag) == 0) return;
  const auto saved = ctx;
  ctx.r3.u64 = Rd32(base + jobs + (idx + 5) * 4);
  ctx.r4.u64 = 50;  // (ms)
  sub_8215A8C0(ctx, base);
  const uint32_t status = ctx.r3.u32;
  ctx = saved;
  if (status == 0) {  // (signalled: done)
    base[flag] = base[flag + 1] = base[flag + 2] = base[flag + 3] = 0;
    return;
  }
  static int count = 0;
  if (++count == 1 || count % 1000 == 0)
    REXLOG_INFO("frame rate: an extra update didn't wait for job {} (not done yet; {} times)", idx, count);
}

// -- Latency test aid (SVR2011_TEST_LATENCY=1) -------------------------------
//
// Frames drawn (the draw pass) against frames the native renderer published
// (PublishFrame, in order: one per game swap) = frames in flight from the
// game's draw to the renderer's hand-over; then the time from a publish to
// the presenter's next new game frame (a 1 ms poll). Once a second.
namespace {
std::atomic<uint64_t> g_lat_published{0}, g_lat_swapped{0};
std::atomic<int64_t> g_lat_publish_at{0};
}  // namespace

namespace svr2011 {
void LatencyOnSwap() { ++g_lat_swapped; }
void LatencyOnPublish() {
  static const bool on = std::getenv("SVR2011_TEST_LATENCY") != nullptr;
  if (!on) return;
  ++g_lat_published;
  g_lat_publish_at = Clock::now().time_since_epoch().count();
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      uint64_t seen = rex::ui::HostNewGuestFramePresentCount();
      double sum_ms = 0, max_ms = 0;
      int n = 0;
      int64_t min_inflight = INT64_MAX, max_inflight = INT64_MIN;
      auto last_log = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t shown = rex::ui::HostNewGuestFramePresentCount();
        const int64_t inflight = int64_t(g_lat_drawn.load()) - int64_t(g_lat_published.load());
        min_inflight = std::min(min_inflight, inflight), max_inflight = std::max(max_inflight, inflight);
        if (shown != seen) {
          seen = shown;
          const auto at = Clock::time_point(Clock::duration(g_lat_publish_at.load()));
          const double ms = std::chrono::duration<double, std::milli>(Clock::now() - at).count();
          sum_ms += ms, max_ms = std::max(max_ms, ms), ++n;
        }
        if (Clock::now() - last_log >= std::chrono::seconds(1)) {
          last_log = Clock::now();
          REXLOG_INFO("frame rate: latency - drawn {} swapped {} published {}: drawn-published {}..{}, swapped-published "
                      "{}; publish -> shown avg {:.1f} ms, max {:.1f} ms ({} new frames shown)",
                      g_lat_drawn.load(), g_lat_swapped.load(), g_lat_published.load(), min_inflight, max_inflight,
                      int64_t(g_lat_swapped.load()) - int64_t(g_lat_published.load()), n ? sum_ms / n : 0.0, max_ms, n);
          sum_ms = max_ms = 0, n = 0, min_inflight = INT64_MAX, max_inflight = INT64_MIN;
        }
      }
    }).detach();
  });
}
}  // namespace svr2011

// The game's render-thread commands: sub_826DFFA0(queue, ?) hands out a
// command record (+0 type); type 24 (sub_826DE6B0, "set texture") calls its
// object's (+20) vtable[6] on the render thread, which runs the queue at the
// frame barrier. With two world updates in a frame the second may free that
// object first: the render thread then calls into freed memory - a pure
// virtual call (c36b4fd skips those) or, the memory reused, garbage (a weak
// PC crashed so, 2ac47b9). So the logic thread's commands of the frame are
// noted, and a free (operator delete: sub_8269B2D0(ptr, ?)) of a block
// holding a pending type-24 command's object turns that command into type 0
// (nothing). The free itself is made as always.
namespace {
uint32_t g_queue = 0;  // (the queue: its buffers and size read by sub_826DFFA0's hook)
std::atomic<uint32_t> g_queue_peak{0};  // (the fullest thread part since RenderQueuePeakTake, %)
}  // namespace
REX_EXTERN(__imp__sub_826DFFA0);
REX_HOOK_RAW(sub_826DFFA0) {
  const uint32_t queue = ctx.r3.u32, desc = ctx.r4.u32;
  __imp__sub_826DFFA0(ctx, base);
  if (!ctx.r3.u32) {  // (a thread's part of the buffer full: the caller writes to the 0 - logged)
    static std::atomic<uint32_t> full{0};
    if (const uint32_t n = ++full; n <= 5)
      REXLOG_WARN("frame rate: render command queue {:08X} full ({} units a buffer, a command of {} units)", queue,
                  Rd32(base + queue + 20), desc ? Rd32(base + desc + 8) + 1 : 0);
  }
  // (how full the buffer gets in a match: its record 0 - +0 buffer, +4 units used, +8 units - every 64th command;
  // in menus and loads the records are laid out otherwise)
  static std::atomic<uint32_t> calls{0};
  if (queue == g_queue && ((++calls & 63) == 0 || !ctx.r3.u32) && svr2011::InMatch()) {
    const uint32_t rec = Rd32(base + queue + 76);
    const uint32_t used = rec ? Rd32(base + rec + 4) : 0, cap = rec ? Rd32(base + rec + 8) : 0;
    if (cap) {
      const uint32_t pct = uint32_t(std::min<uint64_t>(uint64_t(used) * 100 / cap, 100));
      for (uint32_t was = g_queue_peak.load(); pct > was && !g_queue_peak.compare_exchange_weak(was, pct);) {
      }
      static std::atomic<uint32_t> step{0};  // (a new 10% step of the run: logged)
      if (uint32_t was = step.load(); pct / 10 > was && step.compare_exchange_strong(was, pct / 10))
        REXLOG_INFO("frame rate: render command queue {}% full in a match ({} of {} units)", pct, used, cap);
    }
  }
  if (ctx.r3.u32 && std::this_thread::get_id() == g_logic_thread.load() && g_frame_commands.size() < 100000)
    g_frame_commands.push_back(ctx.r3.u32);
}

uint32_t svr2011::RenderQueuePeakTake() { return g_queue_peak.exchange(0); }

// The queue is made once (sub_826DF610, from sub_82578B80): r8 = its 16-byte units per buffer (393,216 on PC - 6 MB
// a buffer; two buffers, +40 / +44). In a match record 0 (+76) is the whole buffer, at times the buffer is split in
// 30,720-unit thread parts; a command takes one or more units. An arena with too many meshes ran a part full in a 6-man
// (27 Oct: bg16 x5, 1,770 meshes; the caller wrote to the 0 it got - sub_826DE130 / sub_826DE0A8): the Mod Maker's and
// the converters' mesh cap (svrfmt/arena.h kMaxArenaMeshesNew) keeps arenas under it. (A 3x buffer didn't help: the
// parts stay 30,720 units.) Logged: its size, how full it gets in a match (sub_826DFFA0's hook).
REX_EXTERN(__imp__sub_826DF610);
REX_HOOK_RAW(sub_826DF610) {
  const uint32_t q = ctx.r3.u32;
  __imp__sub_826DF610(ctx, base);
  g_queue = q;
  REXLOG_INFO("frame rate: render command queue {:08X}: {} units per buffer", q, Rd32(base + q + 20));
}

// And where the render thread runs them (sub_826E02A8(?, command)): a
// type-24 command whose object is gone some way the notes above miss (a
// player's crash during loading in 2026-10-05, then in most scripted test
// routes: "call to invalid function at 0x542F5745" or an access violation at
// sub_826E02A8+0x15da reading the object) is dropped there - when the
// object's address isn't a heap's, its vtable isn't in the game's image, or its
// vtable[6] isn't a game function. Logged; a live object is never skipped.
namespace {
// A guest address in the heaps or the image (where a game object can be).
bool GuestObjectAddress(uint32_t a) {
  return (a >= 0x40000000u && a < 0x60000000u) || (a >= 0x70000000u && a < 0x90000000u) || a >= 0xA0000000u;
}
}  // namespace

REX_EXTERN(__imp__sub_826E02A8);
REX_HOOK_RAW(sub_826E02A8) {
  const uint32_t cmd = ctx.r4.u32;
  if (cmd && Rd32(base + cmd) == 24) {
    static PPCFunc* const invalid = rex::runtime::ResolveIndirectFunction(0);
    const uint32_t obj = Rd32(base + cmd + 20);
    const uint32_t vt = obj && GuestObjectAddress(obj) ? Rd32(base + obj) : 0;
    const bool image = vt >= 0x82000000u && vt < 0x83000000u;
    const uint32_t fn = image ? Rd32(base + vt + 24) : 0;
    if (!image || rex::runtime::ResolveIndirectFunction(fn) == invalid) {
      static int count = 0;
      if (++count <= 20 || count % 1000 == 0)
        REXLOG_WARN("frame rate: a render command's object was gone (object {:08X}, vtable {:08X}) - skipped ({} times)",
                    obj, vt, count);
      return;
    }
  }
  __imp__sub_826E02A8(ctx, base);
}

REX_EXTERN(__imp__sub_8269B2D0);
REX_HOOK_RAW(sub_8269B2D0) {
  const uint32_t ptr = ctx.r3.u32;
  if (svr2011::MemTraceOn()) svr2011::MemTraceDelete(ptr, uint32_t(ctx.lr));  // (test aid: mem_trace.h)
  if (svr2011::CawHeapFree(ptr)) return;  // (a CAW editor part from the fallback memory: caw_limits.h)
  if (ptr && !g_frame_commands.empty() && std::this_thread::get_id() == g_logic_thread.load()) {
    constexpr uint32_t kWithin = 0x4000;  // (the object: at or just after the block's start)
    for (const uint32_t cmd : g_frame_commands) {
      if (Rd32(base + cmd) != 24) continue;
      const uint32_t obj = Rd32(base + cmd + 20);
      if (obj >= ptr && obj - ptr < kWithin) {
        Wr32(base + cmd, 0);
        static int count = 0;
        if (++count <= 20 || count % 1000 == 0)
          REXLOG_INFO("frame rate: a queued render command's object was freed - command dropped ({} times)", count);
      }
    }
  }
  __imp__sub_8269B2D0(ctx, base);
}

// -- Research aid (30 fps pins): SVR2011_TEST_LOST_TRACE=1 -------------------
// The functions that write a character's lost byte (+447), each wrapped:
// when one sets a character's lost byte, it is logged with its caller and
// the match frame.
namespace {
bool LostTraceOn() {
  static const bool on = std::getenv("SVR2011_TEST_LOST_TRACE") != nullptr;
  return on;
}
struct LostSnap {
  uint8_t lost[6] = {};
};
LostSnap SnapLost(uint8_t* base) {
  LostSnap s;
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4)) s.lost[i] = base[c + 447];
  return s;
}
void CheckLost(uint8_t* base, const LostSnap& before, const char* fn, uint32_t lr) {
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4))
      if (base[c + 447] != before.lost[i])
        REXLOG_INFO("lost trace: {} (from {:08X}) set person {} lost {} -> {} (by {}), frame {}{}", fn, lr, i,
                    before.lost[i], base[c + 447], base[c + 448], Rd32(base + kMatchFrames),
                    g_extra_update ? " (extra update)" : "");
}
}  // namespace

#define SVR2011_LOST_TRACE(fn)                                \
  REX_EXTERN(__imp__##fn);                                    \
  REX_HOOK_RAW(fn) {                                          \
    if (!LostTraceOn()) return __imp__##fn(ctx, base);        \
    const LostSnap before = SnapLost(base);                   \
    const uint32_t lr = uint32_t(ctx.lr);                     \
    __imp__##fn(ctx, base);                                   \
    CheckLost(base, before, #fn, lr);                         \
  }
SVR2011_LOST_TRACE(sub_82244E88)
SVR2011_LOST_TRACE(sub_82245B60)
SVR2011_LOST_TRACE(sub_822473B0)
// The pin count (all frame rates). sub_822446E0(judge) judges a pin each
// update (judge +112: the pinning character's index; its +516 is 2 while the
// pin is on): at judge +100 = 3 the pin is won. +100 itself is set from the
// referee's count motion (190) by the characters' task (sub_821965D0) - and
// with two updates in a frame (30 fps, a slow PC or phone) after a frame of
// one, the step that should make a count 2 made it 0 instead: the referee
// went on counting to the end of the motion, the pin wasn't given and he
// counted again (4 of 9 pins at 30 fps; never at 60) - and once that happens
// the count stays 0. The referee's count is the same every time: 1, 2 and 3
// come 78, 140 and 210 frames into his count motion. So here, once a count
// has dropped to 0 with the referee still in the count motion and the pin
// still on, the count follows that timing. A real kick-out ends the pin
// (+516), changes the wrestlers' pin motions (the pinner's +212 and his
// opponent's, +96) or the referee's: then it's left alone.
namespace {
struct PinGuard {
  uint32_t judge = 0;
  int32_t count = 0;    // (the highest count seen in this count)
  uint32_t start = 0;   // (match frame the referee's count motion began)
  bool broken = false;  // (the count dropped to 0: the timing rules)
  uint32_t motions = 0; // (the pinner's and his opponent's motions at the start)
  int32_t voiced = 0;   // (the count the judge last called out: its +104 after the judge ran)
};
int32_t CountAt(uint32_t frames) { return frames >= 210 ? 3 : frames >= 140 ? 2 : frames >= 78 ? 1 : 0; }
PinGuard g_pin;

// Problem reports: each step of a pin's count as the judge leaves it (+100, its voice is played as it changes:
// sub_82165AD8, 0x20000 + count - 30863 / 30866) - count, match frame, the judge's update tick (+96), the
// referee's count motion frame, who is pinned, an extra update (two in a frame), the guard's say. (200 lines
// a session.)
struct PinLog {
  uint32_t judge = 0;
  int32_t count = -1;
  uint32_t start = 0;  // (match frame the count began: its first step)
};
PinLog g_pin_log;
int PersonOf(uint8_t* base, uint32_t ch) {  // (the character's place in the match's list)
  for (int i = 0; i < 10; ++i)
    if (Rd32(base + kChars + uint32_t(i) * 4) == ch) return i;
  return -1;
}
void PinCountLog(uint8_t* base, uint32_t judge, uint32_t pinner, bool pin_on, uint32_t frame) {
  static int lines = 0;
  const int32_t count = int32_t(Rd32(base + judge + 100));
  if (g_pin_log.judge != judge) {
    if (count <= 0) return;  // (a judge with no count going: nothing to tell)
    g_pin_log = PinLog{judge, 0, frame};
  }
  if (count == g_pin_log.count) return;
  if (g_pin_log.count <= 0 && count > 0) g_pin_log.start = frame;
  const uint32_t pinned = pinner ? Rd32(base + pinner + 96) : 0;
  if (lines < 200 && ++lines)
    REXLOG_INFO("pin count: {} -> {} (frame {}, {} frames into the count, judge tick {}, pin {}{}{}){}",
                g_pin_log.count, count, frame, frame - g_pin_log.start, int32_t(Rd32(base + judge + 96)),
                pin_on ? "on" : "off", g_pin.broken ? ", by the guard's timing" : "",
                g_extra_update ? ", an extra update" : "",
                pinned ? fmt::format(" - person {} pinning person {}", PersonOf(base, pinner), PersonOf(base, pinned))
                       : std::string());
  g_pin_log.count = count;
  if (count <= 0) g_pin_log = PinLog{};  // (the count over: kicked out, broken, or the pin won)
}

uint32_t RefereeOf(uint8_t* base) {
  for (uint32_t i = 0; i < 9; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4))
      if (Rd32(base + c + 2624) == 1) return c;
  return 0;
}
}  // namespace

REX_EXTERN(__imp__sub_822446E0);
REX_HOOK_RAW(sub_822446E0) {
  const uint32_t judge = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const uint32_t pinner = Rd32(base + kChars + Rd32(base + judge + 112) * 4);
  const uint32_t ref = RefereeOf(base);
  const uint32_t frame = Rd32(base + kMatchFrames);
  const bool pin_on = pinner && Rd32(base + pinner + 516) == 2;
  const bool counting = ref && (Rd32(base + ref + 212) & 0x7FFF) == 190;
  // Test aid: SVR2011_TEST_PIN_DROP_3=1 - once a session, a count of 2 drops to 0 in an extra update when the 3 is
  // due (as the lost step does), to try the guard's repair at the 3.
  if (static const bool drop3 = std::getenv("SVR2011_TEST_PIN_DROP_3") != nullptr; drop3 && g_extra_update && pin_on &&
      counting && g_pin.judge == judge && g_pin.count == 2 && frame - g_pin.start >= 205 &&
      Rd32(base + judge + 100) == 2) {
    static bool dropped = false;
    if (!dropped) {
      dropped = true;
      Wr32(base + judge + 100, 0);
      REXLOG_INFO("frame rate: test - the pin count of 2 dropped to 0 (SVR2011_TEST_PIN_DROP_3)");
    }
  }
  const int32_t count = int32_t(Rd32(base + judge + 100));
  const uint32_t pinned = pinner ? Rd32(base + pinner + 96) : 0;
  const uint32_t motions = pinner && pinned ? (Rd32(base + pinner + 212) << 16) ^ Rd32(base + pinned + 212) : 0;
  if (!pin_on || !counting || g_pin.judge != judge || motions != g_pin.motions) {
    g_pin = PinGuard{pin_on && counting ? judge : 0, pin_on && counting ? count : 0, frame, false, motions};
  } else if (count > g_pin.count) {
    g_pin.count = count;
  } else if (count == 0 && g_pin.count > 0) {
    // Only the lost step is put right: a count of 1 dropping when the 2 is due (140 frames in), or of 2 when
    // the 3 is due (210) - the same lost step one slap later, seen in an extra update (the only place it
    // happens: never at 60 fps). Left alone, the referee stayed at 2 and the pin was never given, or he
    // counted again and called "two" at his third slap (report 2026-10-21). A count dropping at any other
    // moment is the pinned man kicking out - not always with a motion change in the same update: taken as
    // broken, nobody kicked out at 30 fps (report 2026-10-17); a kick-out at the 3 still counts in a
    // frame's only (or last) update.
    const uint32_t into = frame - g_pin.start;
    const bool lost_2 = g_pin.count == 1 && into >= 128 && into <= 160;
    const bool lost_3 = g_pin.count == 2 && into >= 198 && into <= 230 && g_extra_update;
    if (!g_pin.broken && !lost_2 && !lost_3) {
      g_pin = PinGuard{};  // (a kick-out: the game's own count)
    } else {
      if (!g_pin.broken) {
        g_pin.broken = true;
        static int fixed = 0;
        if (++fixed <= 20 || fixed % 100 == 0)
          REXLOG_INFO("frame rate: a pin count of {} dropped to 0 {} frames into the count - it goes on by its timing ({} times)",
                      g_pin.count, into, fixed);
      }
      g_pin.count = std::max(g_pin.count, CountAt(into));
      Wr32(base + judge + 100, uint32_t(g_pin.count));
      // The lost step also takes the count the judge last called out (+104) back to 0, every update: with only
      // +100 put back the judge saw a new count each update and said "two" again and again until the 3 - the
      // loud repeating 2-count (report 2026-10-24: up to ~70 times at a lost 2). So the count already called
      // out goes back too: the judge speaks only when the count goes up.
      if (g_pin.voiced > 0) Wr32(base + judge + 104, uint32_t(std::min(g_pin.voiced, g_pin.count)));
    }
  }
  if (!LostTraceOn()) {
    __imp__sub_822446E0(ctx, base);
    if (g_pin.judge == judge) g_pin.voiced = int32_t(Rd32(base + judge + 104));
    PinCountLog(base, judge, pinner, pin_on, frame);
    return;
  }
  const LostSnap before = SnapLost(base);
  if (!g_watch.load()) g_watch = judge + 100, g_watch_last = Rd32(base + judge + 100);
  if (pin_on)
    REXLOG_INFO("lost trace: judge +96 {} +100 {} +104 {} +116 {} char {} motion {} ref motion {} frame {}{}",
                int32_t(Rd32(base + judge + 96)), int32_t(Rd32(base + judge + 100)), int32_t(Rd32(base + judge + 104)),
                Rd32(base + judge + 116), Rd32(base + judge + 112), Rd32(base + pinner + 212),
                ref ? Rd32(base + ref + 212) : 0, frame, g_extra_update ? " (extra update)" : "");
  __imp__sub_822446E0(ctx, base);
  if (g_pin.judge == judge) g_pin.voiced = int32_t(Rd32(base + judge + 104));
  CheckLost(base, before, "sub_822446E0", lr);
  PinCountLog(base, judge, pinner, pin_on, frame);
}
// Problem reports: each count voice the pin judge plays (sub_82165AD8 called from sub_822446E0, its id
// 0x20000 + count - 30863 / 30866) - a repeated "two" shows as repeated lines (200 a session).
REX_EXTERN(__imp__sub_82165AD8);
REX_HOOK_RAW(sub_82165AD8) {
  static int lines = 0;
  if (const uint32_t lr = uint32_t(ctx.lr); lr >= 0x822446E0 && lr < 0x82244C30 && lines < 200 && ++lines)
    REXLOG_INFO("pin voice {:X} (count {} or {} by its bank), frame {}{}", ctx.r4.u32,
                int32_t(ctx.r4.u32) - 0x20000 + 30863, int32_t(ctx.r4.u32) - 0x20000 + 30866,
                Rd32(base + kMatchFrames), g_extra_update ? " (extra update)" : "");
  __imp__sub_82165AD8(ctx, base);
}
SVR2011_LOST_TRACE(sub_82245DF8)
SVR2011_LOST_TRACE(sub_82243660)
SVR2011_LOST_TRACE(sub_822EB378)
// The referee's pin count object (vtable 0x820057C4): slot 6, sub_82242C98,
// adds one (+32) and at 8 tells the pin is won.
REX_EXTERN(__imp__sub_82242C98);
REX_HOOK_RAW(sub_82242C98) {
  if (LostTraceOn())
    REXLOG_INFO("lost trace: count {} -> {} (from {:08X}), frame {}{}", Rd32(base + ctx.r3.u32 + 32),
                Rd32(base + ctx.r3.u32 + 32) + 1, uint32_t(ctx.lr), Rd32(base + kMatchFrames),
                g_extra_update ? " (extra update)" : "");
  __imp__sub_82242C98(ctx, base);
}
