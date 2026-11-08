// Bring-up profiler for the Switch: every few seconds each thread's CPU share and
// where it is (pc, lr, return addresses on its stack), in logs/nx_threads.txt.
// Offsets are from the program's start: tools name them from svr2011.elf.
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

#define SVR_THREADS_FILE "/SvR2011/logs/nx_threads.txt"

extern "C" void _start(void);
extern "C" bool NxStartLoading(void);
extern "C" volatile Handle g_nx_reader;

namespace {

constexpr int kMax = 192;
constexpr int kWords = 10;

struct Entry {
  Handle handle;
  u64 last_ticks;
};
Entry g_threads[kMax];
volatile int g_count = 0;
Mutex g_lock;

struct Snapshot {
  Handle handle;
  u64 ticks, id, pc, lr, sp;
  u64 words[kWords];
  int nwords;
  bool ok;
};
Snapshot g_snap[kMax];

void Register(Handle h) {
  mutexLock(&g_lock);
  if (g_count < kMax) {
    g_threads[g_count].handle = h;
    g_threads[g_count].last_ticks = 0;
    g_count = g_count + 1;
  }
  mutexUnlock(&g_lock);
}

// Where the busy threads spend their time, in the game: 50 samples a second of the six
// busiest threads for 20 s, then a pause, in logs/nx_samples.txt (nx_samples.py names them).
// A thread is paused only for the moment its registers and stack are read; nothing that
// could need a lock it holds runs meanwhile.
constexpr int kTop = 6;
constexpr int kSampleWords = 8;
struct Sample {
  u32 tid;
  u32 pc, lr;
  u32 words[kSampleWords];
};
constexpr int kMaxSamples = 6100;
Sample g_samples[kMaxSamples];
volatile Handle g_top[kTop];
volatile u32 g_top_id[kTop];
volatile bool g_in_game = false;
volatile u64 g_main_id = 0;

void Sampler(void*) {
  const u64 base = reinterpret_cast<u64>(&_start);
  // (Each sample pauses a busy thread, about 300 a second: only when this file exists.)
  FILE* wanted = fopen("/SvR2011/nx_profile_samples.txt", "r");
  if (!wanted) return;
  fclose(wanted);
  FILE* f = fopen("/SvR2011/logs/nx_samples.txt", "w");
  if (!f) return;
  // Window 0 is the start-up (40 s, the main thread first), window 1 the Start Screen's
  // loading (from START), then two in the game.
  for (int window = 0; window < 4;) {
    if (window == 1) {
      svcSleepThread(100000000ll);
      if (!NxStartLoading()) continue;
    } else if (window > 1) {
      svcSleepThread(1000000000ll);
      if (!g_in_game) continue;
    }
    int n = 0;
    const u64 start = armGetSystemTick();
    const u64 length = (window == 0 ? 40ull : 20ull) * 19200000ull;
    while (n < kMaxSamples - kTop && armGetSystemTick() - start < length) {
      for (int i = 0; i < kTop && n < kMaxSamples; i++) {
        // (The start-up and the Start Screen's loading: the sixth slot is whoever read a file last.)
        const bool reader_slot = i == kTop - 1 && window < 2 && g_nx_reader != 0;
        const Handle h = reader_slot ? g_nx_reader : g_top[i];
        if (!h) continue;
        svcSleepThread(window == 0 ? 6600000 : 3300000);
        if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
        static ThreadContext ctx;
        if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h))) {
          Sample& o = g_samples[n];
          o = Sample{};
          o.tid = reader_slot ? 1 : g_top_id[i];
          o.pc = static_cast<u32>(ctx.pc.x - base);
          o.lr = static_cast<u32>(ctx.lr - base);
          MemoryInfo info{};
          u32 page = 0;
          if (R_SUCCEEDED(svcQueryMemory(&info, &page, ctx.sp)) && (info.perm & Perm_R)) {
            const u64* sp = reinterpret_cast<const u64*>(ctx.sp);
            const u64 end = info.addr + info.size;
            int got = 0;
            for (int w = 0; w < 512 && reinterpret_cast<u64>(sp + w + 1) <= end && got < kSampleWords; w++) {
              if (sp[w] > base + 0x1000 && sp[w] < base + (1ull << 27)) o.words[got++] = static_cast<u32>(sp[w] - base);
            }
          }
          n++;
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
      }
    }
    fprintf(f, "== window %d at %.0f s, %d samples\n", window, double(armGetSystemTick()) / 19200000.0, n);
    for (int i = 0; i < n; i++) {
      const Sample& o = g_samples[i];
      fprintf(f, "%u %x %x", o.tid, o.pc, o.lr);
      for (int w = 0; w < kSampleWords && o.words[w]; w++) fprintf(f, " %x", o.words[w]);
      fputc('\n', f);
    }
    fflush(f);
    window++;
    if (window > 2) svcSleepThread(30000000000ll);
  }
  fclose(f);
}

void Monitor(void*) {
  FILE* f = fopen(SVR_THREADS_FILE, "w");
  if (!f) return;
  const u64 base = reinterpret_cast<u64>(&_start);
  u64 before = armGetSystemTick();
  Handle self = threadGetCurHandle();
  // (Pausing threads to see where they are costs frames: only when this file exists.)
  FILE* flag = fopen("/SvR2011/nx_profile_stacks.txt", "r");
  const bool pause = flag != nullptr;  // g_pause
  if (flag) fclose(flag);
  for (int round = 0; round < 120; round++) {
    svcSleepThread(round < 4 ? 5000000000ll : 15000000000ll);
    const u64 now = armGetSystemTick();
    const double span = double(now - before);
    before = now;
    mutexLock(&g_lock);
    const int n = g_count;
    for (int i = 0; i < n; i++) {
      Snapshot& s = g_snap[i];
      s.handle = g_threads[i].handle;
      s.ok = false;
      s.nwords = 0;
      s.id = 0;
      u64 ticks = 0;
      if (s.handle == self || R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, s.handle, UINT64_MAX))) continue;
      s.ticks = ticks - g_threads[i].last_ticks;
      g_threads[i].last_ticks = ticks;
      svcGetThreadId(&s.id, s.handle);
      if (!pause) continue;
      if (double(s.ticks) * 100.0 / span < 2.0 && (round % 4) != 3) continue;
      if (R_FAILED(svcSetThreadActivity(s.handle, ThreadActivity_Paused))) continue;
      static ThreadContext ctx;
      if (R_SUCCEEDED(svcGetThreadContext3(&ctx, s.handle))) {
        s.pc = ctx.pc.x;
        s.lr = ctx.lr;
        s.sp = ctx.sp;
        s.ok = true;
        MemoryInfo info{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, ctx.sp)) && (info.perm & Perm_R)) {
          const u64* sp = reinterpret_cast<const u64*>(ctx.sp);
          const u64 end = info.addr + info.size;
          for (int w = 0; w < 2048 && reinterpret_cast<u64>(sp + w + 1) <= end && s.nwords < kWords; w++) {
            if (sp[w] > base + 0x1000 && sp[w] < base + (1ull << 27)) s.words[s.nwords++] = sp[w] - base;
          }
        }
      }
      svcSetThreadActivity(s.handle, ThreadActivity_Runnable);
    }
    mutexUnlock(&g_lock);
    {
      // The busiest threads, for the sampler.
      double total = 0;
      int best[kTop];
      for (int t = 0; t < kTop; t++) best[t] = -1;
      for (int i = 0; i < n; i++) {
        const Snapshot& c = g_snap[i];
        if (!c.id) continue;
        total += double(c.ticks) * 100.0 / span;
        for (int t = 0; t < kTop; t++) {
          if (best[t] < 0 || c.ticks > g_snap[best[t]].ticks) {
            for (int m = kTop - 1; m > t; m--) best[m] = best[m - 1];
            best[t] = i;
            break;
          }
        }
      }
      for (int t = 0; t < kTop; t++) {
        g_top[t] = best[t] >= 0 ? g_snap[best[t]].handle : 0;
        g_top_id[t] = best[t] >= 0 ? static_cast<u32>(g_snap[best[t]].id) : 0;
      }
      g_in_game = total > 150.0;
      // In the game the three busiest threads get a core each. The rest prefer the fourth
      // core at priority 63, the only one where threads there share time, and may run on any
      // core that is idle: the three take their core back at once. Kept to the fourth, five
      // helpers filled it and a woken one could wait 10 ms slices while the frame waited for it.
      static u64 pinned[3];
      u64 cores = 0;
      svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
      int top[3] = {-1, -1, -1};
      for (int t = 0, k = 0; t < kTop && k < 3; t++) {
        if (best[t] >= 0 && g_snap[best[t]].id != g_main_id) top[k++] = best[t];
      }
      bool changed = false;
      for (int k = 0; k < 3; k++) {
        const u64 id = top[k] >= 0 ? g_snap[top[k]].id : 0;
        if (id != pinned[k]) pinned[k] = id, changed = true;
      }
      static int placed_threads = 0;
      if (n != placed_threads) changed = true;  // (a thread started since)
      if (g_in_game && changed && (cores & 8)) {
        placed_threads = n;
        for (int i = 0; i < n; i++) {
          const Snapshot& c = g_snap[i];
          if (!c.id || c.handle == self) continue;
          if (c.id == g_main_id) {  // (it presents: its own priority, away from the three)
            svcSetThreadCoreMask(c.handle, 3, 0x8);
            continue;
          }
          const int core = i == top[0] ? 0 : i == top[1] ? 1 : i == top[2] ? 2 : 3;
          if (core < 3) {
            svcSetThreadCoreMask(c.handle, core, 1 << core);
            svcSetThreadPriority(c.handle, 0x3B);
          } else {
            svcSetThreadCoreMask(c.handle, 3, 0xF);
            svcSetThreadPriority(c.handle, 0x3F);
          }
        }
        fprintf(f, "== placed: core mask %llx, %llu %llu %llu on cores 0-2\n", (unsigned long long)cores,
                (unsigned long long)pinned[0], (unsigned long long)pinned[1], (unsigned long long)pinned[2]);
      }
    }
    u64 used = 0;
    fprintf(f, "== round %d, %.1f s, %d threads\n", round, span / 19200000.0, n);
    for (int i = 0; i < n; i++) {
      const Snapshot& s = g_snap[i];
      if (!s.id) continue;
      fprintf(f, "t%llu %5.1f%%", (unsigned long long)s.id, double(s.ticks) * 100.0 / span);
      if (s.ok) {
        fprintf(f, " pc +%llx lr +%llx :", (unsigned long long)(s.pc - base), (unsigned long long)(s.lr - base));
        for (int w = 0; w < s.nwords; w++) fprintf(f, " %llx", (unsigned long long)s.words[w]);
      }
      fprintf(f, "\n");
    }
    fflush(f);
    fsync(fileno(f));
    (void)used;
  }
  fclose(f);
}

}  // namespace

extern "C" {

Result __real_threadStart(Thread* t);
Result __wrap_threadStart(Thread* t) {
  Result rc = __real_threadStart(t);
  if (R_SUCCEEDED(rc)) Register(t->handle);
  return rc;
}

void NxProfileStart(void) {
  mutexInit(&g_lock);
  Register(envGetMainThreadHandle());
  g_top[0] = envGetMainThreadHandle();
  svcGetThreadId(const_cast<u64*>(&g_main_id), envGetMainThreadHandle());
  g_top_id[0] = static_cast<u32>(g_main_id);
  u64 cores = 0;
  svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  const int own_core = (cores & 8) ? 3 : 2;
  static Thread t;
  if (R_SUCCEEDED(threadCreate(&t, Monitor, nullptr, nullptr, 0x8000, 0x2C, own_core))) {
    __real_threadStart(&t);
  }
  static Thread sampler;
  if (R_SUCCEEDED(threadCreate(&sampler, Sampler, nullptr, nullptr, 0x8000, 0x2C, own_core))) {
    __real_threadStart(&sampler);
  }
}

}  // extern "C"
