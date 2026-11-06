// WWE SmackDown vs. Raw 2011 - instant replays off (the replays setting).
//
// The game has no option for them ("POST MATCH REPLAY" is an unused string).
// Two kinds, both skipped the way the game skips them itself:
// - Finisher replays: a finisher (move ids 3500-3649) creates a task
//   (sub_8230C140); each frame sub_8230C2D0 runs it: state 0 waits ~70
//   frames, then it takes the camera and the HUD and replays the move from a
//   few angles. Off, the task deletes itself while still waiting (state 0) -
//   what the game does when a finisher is interrupted (0x8230C33C) - before
//   it has touched anything.
// - The match-end highlights: the match flow's replay step (ctor
//   sub_82413698) picks up to 5 clips and plays them before the celebration.
//   With no clips the ctor sets its "done" flag (+788) and the step goes
//   straight on to the celebration and results; off, that flag is set.
//
// Stuck highlights: the step's update (sub_82413280) waits in state 1 until
// the replay recorder hands it the next clip (sub_82413170 ->
// sub_8217B550 -> sub_8219F9F0, which only succeeds while the recorder's
// state, +0, is 1 = idle). If the recorder never gets back to idle (seen on
// a Steam Deck: the match was won and never ended), state 1 retried forever
// with the match still on screen. After 8 s in state 1 (or 30 s in state 2,
// the wait before a clip; no scene is made yet in either) the step is done -
// the game's own "no highlights" path - and the recorder's state is logged.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "match_box.h"

#include "generated/default/svr2011_init.h"
#include "thread_cpu.h"
#include "../modmaker/svrfmt/bytes.h"
#include "util.h"

REXCVAR_DEFINE_BOOL(replays, true, "Gameplay", "Instant replays (after finishers and at the end of a match)");

namespace {
using svrfmt::Rd32, svrfmt::Wr32;
}  // namespace

REX_EXTERN(__imp__sub_8230C2D0);
REX_EXTERN(__imp__sub_8230C248);
REX_HOOK_RAW(sub_8230C2D0) {
  if (!REXCVAR_GET(replays) && Rd32(base + ctx.r3.u32 + 80) == 0) {
    ctx.r4.u64 = 1;  // (delete)
    __imp__sub_8230C248(ctx, base);
    return;
  }
  const uint32_t task = ctx.r3.u32;
  const uint32_t before = Rd32(base + task + 80);
  __imp__sub_8230C2D0(ctx, base);
  if (before == 0 && Rd32(base + task + 80) != 0) REXLOG_INFO("[svr2011] finisher replay: plays");
}

REX_EXTERN(__imp__sub_82413698);
REX_HOOK_RAW(sub_82413698) {
  const uint32_t step = ctx.r3.u32;
  __imp__sub_82413698(ctx, base);
  REXLOG_INFO("[svr2011] match-end highlights: {} clips", Rd32(base + step + 780));
  if (!REXCVAR_GET(replays)) Wr32(base + step + 788, 1);  // (no highlights: done)
}

namespace {
constexpr int64_t kStuckMs = 8000;       // state 1: no clip from the recorder
constexpr int64_t kStuckWaitMs = 30000;  // state 2: the wait before a clip
uint32_t g_step = 0, g_state = 0;  // the highlight step and its state last frame
int64_t g_since = 0;               // when it got there
uint32_t g_recorder_state = 0;     // the recorder's state at the last refused clip
uint32_t g_recorder = 0;           // and the recorder
using svr2011::NowMs;
}  // namespace

// The recorder hands out a clip: sub_8219F9F0(recorder, clip), 0 = not now.
REX_EXTERN(__imp__sub_8219F9F0);
REX_HOOK_RAW(sub_8219F9F0) {
  const uint32_t recorder = ctx.r3.u32;
  // Test aid: SVR2011_TEST_HIGHLIGHT_STALL=1 - the recorder never hands out a clip.
  static const bool stall = std::getenv("SVR2011_TEST_HIGHLIGHT_STALL") != nullptr;
  if (stall) {
    g_recorder = recorder;
    g_recorder_state = Rd32(base + recorder);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8219F9F0(ctx, base);
  if (ctx.r3.u32 == 0) g_recorder = recorder, g_recorder_state = Rd32(base + recorder);
}

REX_EXTERN(__imp__sub_82413280);
REX_HOOK_RAW(sub_82413280) {
  const uint32_t step = ctx.r3.u32;
  const uint32_t state = Rd32(base + step + 776);
  const int64_t now = NowMs();
  if (step != g_step || state != g_state) {
    if (step == g_step || state != 0)
      REXLOG_INFO("[svr2011] match-end highlights: state {} (clip {} of {})", state, Rd32(base + step + 784),
                  Rd32(base + step + 780));
    g_step = step, g_state = state, g_since = now;
  } else if (((state == 1 && now - g_since > kStuckMs) || (state == 2 && now - g_since > kStuckWaitMs)) &&
             Rd32(base + step + 788) != 1) {
    REXLOG_WARN(
        "[svr2011] match-end highlights stuck in state {} for {} s (clip {} of {}; recorder {:08X} state {}, now {}, "
        "clips {}, stream {:08X}) - skipped",
        state, (now - g_since) / 1000, Rd32(base + step + 784), Rd32(base + step + 780), g_recorder, g_recorder_state,
        g_recorder ? Rd32(base + g_recorder) : 0, g_recorder ? Rd32(base + g_recorder + 8) : 0,
        g_recorder ? Rd32(base + g_recorder + 20) : 0);
    Wr32(base + step + 788, 1);  // (done: on to the celebration and results)
  }
  __imp__sub_82413280(ctx, base);
}

// The match's end (logged, for reports of a won match that never ended):
// sub_82322DA0, each frame, steps the end - state (+16) 0: waits while the
// object of sub_821650F8 is busy (its +156 -> +1504 not 0 or 5); 1: the result;
// 2: waits for a fade (sub_8217A8D8(+8)), then the highlights step
// (sub_823EDC78 -> sub_82413698, logged above); 3-5: on to the results. Each
// change is logged, and a state still waiting after 10 s logs what it waits on.
REX_EXTERN(__imp__sub_82322DA0);
REX_HOOK_RAW(sub_82322DA0) {
  const uint32_t task = ctx.r3.u32;
  const auto saved = ctx;
  __imp__sub_82322DA0(ctx, base);
  using Clock = std::chrono::steady_clock;
  static uint32_t last_task = 0, last_state = ~0u;
  static Clock::time_point since;
  static bool told = false;
  const uint32_t state = Rd32(base + task + 16);
  if (task != last_task || state != last_state) {
    REXLOG_INFO("[svr2011] match end: step {} (task {:08X}, +20 {})", state, task, Rd32(base + task + 20));
    if (task != last_task) svr2011::MatchBoxMatchEnded();  // (a new end task - its first step 0, or 1 after a win: the box)
    if (state == 0) svr2011::LoadWatchStart();  // (a mode leaving the practice ring: thread_cpu.h)
    last_task = task, last_state = state, since = Clock::now(), told = false;
    return;
  }
  if (told || state == 3 || Clock::now() - since < std::chrono::seconds(8)) return;  // (3: the highlights play)
  told = true;
  // Step 0's object: sub_821650F8(**0x82E35468) (as the step does); its part
  // at +156 runs a handshake with the frame's helper thread (sub_826E1868):
  // +1504 the update's side (0/5 = done), +1508 the helper's, +1496 a buffer.
  uint32_t obj = 0, part = 0;
  if (const uint32_t p = Rd32(base + 0x82E35468)) {
    PPCContext c = saved;
    c.r3.u64 = Rd32(base + p);
    sub_821650F8(c, base);
    obj = c.r3.u32;
    if (obj) part = Rd32(base + obj + 156);
  }
  std::string queue;  // (the helper's callbacks, sub_826E0F78: both buffers)
  if (const uint32_t q = Rd32(base + 0x82F6D96C)) {
    queue = fmt::format(" write {} read {}:", Rd32(base + q + 40), Rd32(base + q + 44));
    for (uint32_t i = 0; i < 64; ++i) {
      const uint32_t e = q + 168 + i * 16;
      if (Rd32(base + e) || Rd32(base + e + 4))
        queue += fmt::format(" [{}.{} {:08X} {:08X} {:08X} {}]", i / 32, i % 32, Rd32(base + e), Rd32(base + e + 4),
                             Rd32(base + e + 8), Rd32(base + e + 12));
    }
  }
  // The replay worker (*0x82DE0368, interface +16 = the part's +1512): its
  // "done" (sub_82167768) wants both results (+48, +52); +152/+160 its queue.
  std::string worker;
  if (const uint32_t w = part ? Rd32(base + part + 1512) : 0)
    worker = fmt::format(", replay worker {:08X} (+32 {} +48 {:08X} +52 {:08X} +152 {} +160 {})", w, Rd32(base + w + 32),
                         Rd32(base + w + 48), Rd32(base + w + 52), Rd32(base + w + 152), Rd32(base + w + 160));
  REXLOG_WARN("[svr2011] match end: step {} waiting 8 s - +20 {}, object {:08X} (+56 {} +88 {} +112 {}), part {:08X} "
              "(+1504 {} +1508 {} +1496 {}), fade {} (global {:08X}); queue{}",
              state, Rd32(base + task + 20), obj, obj ? Rd32(base + obj + 56) : 0, obj ? Rd32(base + obj + 88) : 0,
              obj ? Rd32(base + obj + 112) : 0, part, part ? Rd32(base + part + 1504) : 0,
              part ? Rd32(base + part + 1508) : 0, part ? Rd32(base + part + 1496) : 0, Rd32(base + task + 8),
              Rd32(base + 0x82DEA30C), queue + worker);
  // Step 0 waits for the part to finish with the replay worker (+1504 4 ->
  // 5, or 6 -> 0, once the worker's "done"); on Android (and the Steam Deck: the
  // recorder above) the worker can stay unfinished after a win - the match
  // never ended. On, as the game would: 5 or 0 (the end then goes on; the
  // highlights' own fallback above covers a recorder that stays busy).
  const uint32_t side = part ? Rd32(base + part + 1504) : 0;
  if (state == 0 && (side == 4 || side == 6)) {  // (6: the same wait, then 0)
    Wr32(base + part + 1504, side == 4 ? 5 : 0);
    REXLOG_WARN("[svr2011] match end: the replay worker never finished - went on without it");
  }
}

// Test aid: SVR2011_TEST_REPLAY_WORKER_STALL=1 - the replay worker's "done"
// (sub_82167768) never true, as on the Fold 7 after a Steel Cage escape.
REX_EXTERN(__imp__sub_82167768);
REX_HOOK_RAW(sub_82167768) {
  static const bool stall = std::getenv("SVR2011_TEST_REPLAY_WORKER_STALL") != nullptr;
  __imp__sub_82167768(ctx, base);
  if (stall) ctx.r3.u64 = 0;
}

// A replayed object's history (vtable 0x82006090: two block-heap arrays by handle at +132 / +140, made by
// sub_8225DA88 / filled while the match records): sub_8225DC88 looks a time up in it with no check. A handle whose
// block is gone (+0 null) was a crash in the match-end highlights - "the game closed after I win", every time (2.1.6
// report, Triple Threat, guest read 0x162 in clip 4 of 5). Now that object isn't stepped (-1: not found, as its
// caller sub_8225DD58 expects) and it's logged with its handles, for the reports.
// Test aid: SVR2011_TEST_REPLAY_GONE=<n> - the n-th look-up of the run is treated as gone (the guard's path).
// For that log line: the last give-backs of replayed objects' histories (sub_82261D28: both handles freed at a clip
// change) and the last block-heap compactions (sub_821DC110), with the caller, the thread and the match clock - kept
// in a small ring (no log of their own) and printed when the guard above finds a history gone, so a report shows what
// emptied it.
namespace {
struct HistoryEvent {
  const char* what = nullptr;  // "given back" / "compaction"
  uint32_t obj = 0, h1 = 0, h2 = 0, caller = 0, thread = 0, clock = 0;
};
constexpr int kHistoryEvents = 16;
std::mutex g_history_mutex;
HistoryEvent g_history[kHistoryEvents];
int g_history_next = 0;
void NoteHistoryEvent(uint8_t* base, const char* what, uint32_t obj, uint32_t h1, uint32_t h2, uint32_t caller) {
  const uint32_t thread = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
  std::lock_guard lock(g_history_mutex);
  g_history[g_history_next] = {what, obj, h1, h2, caller, thread, Rd32(base + 0x82E3CD0C)};
  g_history_next = (g_history_next + 1) % kHistoryEvents;
}
std::string HistoryEvents() {
  std::lock_guard lock(g_history_mutex);
  std::string s;
  for (int k = 0; k < kHistoryEvents; ++k) {
    const HistoryEvent& e = g_history[(g_history_next + k) % kHistoryEvents];
    if (!e.what) continue;
    s += fmt::format(" | {} obj {:08X} handles {:08X} {:08X} by {:08X} thread {:04X} clock {}", e.what, e.obj, e.h1, e.h2,
                     e.caller, e.thread, e.clock);
  }
  return s.empty() ? std::string(" (none)") : s;
}
}  // namespace

REX_EXTERN(__imp__sub_82261D28);
REX_HOOK_RAW(sub_82261D28) {
  const uint32_t obj = ctx.r3.u32;
  if (obj) NoteHistoryEvent(base, "given back", obj, Rd32(base + obj + 132), Rd32(base + obj + 140), uint32_t(ctx.lr));
  __imp__sub_82261D28(ctx, base);
}

REX_EXTERN(__imp__sub_821DC110);
REX_HOOK_RAW(sub_821DC110) {
  NoteHistoryEvent(base, "compaction", 0, 0, 0, uint32_t(ctx.lr));
  __imp__sub_821DC110(ctx, base);
}

// A replay track's step (sub_82252898, for each of the 140 in sub_821A0A48, from the recorder's queued callback on its
// own thread) reads its block at +64 unchecked when +72 is set - but a clip change frees that block (sub_822523A8:
// +64 0, +72 left as it was), which can happen just before the callback runs: a read of guest 0 at a clip change
// (report 684e56, Android, match-end highlights). Nothing to step then.
// Test aids: SVR2011_TEST_TRACK_GONE=<n> - the n-th step of a track with a block finds it gone, once;
// SVR2011_TEST_TRACK_GUARD_OFF=1 - no check (with the other: the crash).
REX_EXTERN(__imp__sub_82252898);
REX_HOOK_RAW(sub_82252898) {
  const uint32_t track = ctx.r3.u32;
  auto at = [&](uint32_t a) { return Rd32(base + a + REX_PHYS_HOST_OFFSET(a)); };
  static const int test_gone = [] { const char* v = std::getenv("SVR2011_TEST_TRACK_GONE"); return v ? std::atoi(v) : 0; }();
  static const bool guard_off = std::getenv("SVR2011_TEST_TRACK_GUARD_OFF") != nullptr;
  static std::atomic<int> steps{0};
  const bool used = track && at(track + 36) && at(track + 72);
  uint32_t kept = 0;
  if (test_gone > 0 && used && at(track + 64) && steps.fetch_add(1) + 1 == test_gone) {
    kept = at(track + 64);
    Wr32(base + track + 64 + REX_PHYS_HOST_OFFSET(track + 64), 0);
    REXLOG_WARN("[svr2011] replay: test - track {:08X} loses its block (SVR2011_TEST_TRACK_GONE)", track);
  }
  if (used && !guard_off) {
    const uint32_t block = at(track + 64);
    if (!block || !at(block)) {
      static std::atomic<int> skipped{0};
      const int n = skipped.fetch_add(1) + 1;
      if (n <= 3 || n % 500 == 0) REXLOG_WARN("[svr2011] replay: a track's block is gone (track {:08X}) - not stepped ({} times)", track, n);
      if (kept) Wr32(base + track + 64 + REX_PHYS_HOST_OFFSET(track + 64), kept);
      return;
    }
  }
  __imp__sub_82252898(ctx, base);
  if (kept) Wr32(base + track + 64 + REX_PHYS_HOST_OFFSET(track + 64), kept);
}

REX_EXTERN(__imp__sub_8225DC88);
REX_HOOK_RAW(sub_8225DC88) {
  const uint32_t obj = ctx.r3.u32;
  // (guest 0xE0000000 and up - the block heap's physical pages - sit 4 KB further on the host on Windows only, as the
  // game's own loads)
  auto at = [&](uint32_t a) { return Rd32(base + a + REX_PHYS_HOST_OFFSET(a)); };
  const uint32_t h1 = obj ? at(obj + 132) : 0, h2 = obj ? at(obj + 140) : 0;
  static const int test_gone = [] { const char* v = std::getenv("SVR2011_TEST_REPLAY_GONE"); return v ? std::atoi(v) : 0; }();
  static int lookups = 0;
  const bool test = test_gone > 0 && ++lookups == test_gone;
  if (test || !h1 || !h2 || !at(h1) || !at(h2)) {
    static int skipped = 0;
    if (++skipped <= 5 || skipped % 500 == 0) {
      REXLOG_WARN("[svr2011] replay: an object's history is gone (object {:08X}, handles {:08X} {:08X} -> {:08X} {:08X}) - "
                  "not stepped ({} times){}", obj, h1, h2, h1 ? at(h1) : 0, h2 ? at(h2) : 0, skipped,
                  test ? " - test (SVR2011_TEST_REPLAY_GONE)" : "");
      if (skipped <= 2) REXLOG_WARN("[svr2011] replay: the last histories given back / compactions, oldest first:{}", HistoryEvents());
    }
    ctx.r3.u64 = 0xFFFFFFFFFFFFFFFFull;
    return;
  }
  __imp__sub_8225DC88(ctx, base);
}
