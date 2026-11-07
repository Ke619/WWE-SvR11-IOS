#include "android_perf.h"

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(android_perf_hints, true, "GPU",
                    "Android 13+: tell the phone each frame how long the game's threads worked (ADPF performance "
                    "hints), so it raises the CPU's clocks before a frame runs late. At the next start");

namespace svr2011 {
namespace {

// (libandroid's APerformanceHint_*: Android 13 / API 33, setThreads 14 / 34; the app runs from Android 10, so
// they're looked up rather than linked)
struct Api {
  void* (*get_manager)() = nullptr;
  void* (*create)(void*, const int32_t*, size_t, int64_t) = nullptr;
  int (*report)(void*, int64_t) = nullptr;
  int (*update_target)(void*, int64_t) = nullptr;
  int (*set_threads)(void*, const int32_t*, size_t) = nullptr;
  void (*close)(void*) = nullptr;
};

std::mutex g_mutex;
Api g_api;
bool g_loaded = false, g_usable = false;
void* g_manager = nullptr;
void* g_session = nullptr;
std::vector<int32_t> g_tids;
int64_t g_target = 0;

bool Load() {
  if (g_loaded) return g_usable;
  g_loaded = true;
  if (!REXCVAR_GET(android_perf_hints)) {
    REXLOG_INFO("android perf: performance hints off (android_perf_hints)");
    return false;
  }
  void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  g_api.get_manager = reinterpret_cast<void* (*)()>(dlsym(lib, "APerformanceHint_getManager"));
  g_api.create = reinterpret_cast<void* (*)(void*, const int32_t*, size_t, int64_t)>(
      dlsym(lib, "APerformanceHint_createSession"));
  g_api.report = reinterpret_cast<int (*)(void*, int64_t)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
  g_api.update_target =
      reinterpret_cast<int (*)(void*, int64_t)>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
  g_api.set_threads = reinterpret_cast<int (*)(void*, const int32_t*, size_t)>(dlsym(lib, "APerformanceHint_setThreads"));
  g_api.close = reinterpret_cast<void (*)(void*)>(dlsym(lib, "APerformanceHint_closeSession"));
  g_manager = g_api.get_manager ? g_api.get_manager() : nullptr;
  g_usable = g_manager && g_api.create && g_api.report && g_api.update_target && g_api.close;
  REXLOG_INFO("android perf: performance hints {}", g_usable ? "available" : "not available (Android 12 or older)");
  return g_usable;
}

// (g_mutex held) The session for g_tids: the threads changed in place (Android 14), else a new session.
void Rebuild() {
  if (g_tids.empty() || !g_target) return;
  if (g_session && g_api.set_threads && g_api.set_threads(g_session, g_tids.data(), g_tids.size()) == 0) return;
  if (g_session) g_api.close(g_session);
  g_session = g_api.create(g_manager, g_tids.data(), g_tids.size(), g_target);
  REXLOG_INFO("android perf: hint session {} for {} thread(s), {:.1f} ms a frame", g_session ? "made" : "refused",
              g_tids.size(), double(g_target) / 1e6);
}

int64_t ThreadCpuNs() {
  timespec t = {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

}  // namespace

void PerfHintThread() {
  static thread_local bool joined = false;
  if (joined) return;
  joined = true;
  std::lock_guard lock(g_mutex);
  if (!Load()) return;
  const int32_t tid = int32_t(syscall(SYS_gettid));
  if (std::find(g_tids.begin(), g_tids.end(), tid) != g_tids.end()) return;
  g_tids.push_back(tid);
  Rebuild();
}

void PerfHintFrame(int fps) {
  static thread_local int64_t last = ThreadCpuNs();
  const int64_t now = ThreadCpuNs();
  const int64_t work = now - last;
  last = now;
  PerfHintThread();
  std::lock_guard lock(g_mutex);
  if (!g_usable) return;
  const int64_t target = 1000000000 / std::max(fps, 1);
  if (target != g_target) {
    g_target = target;
    if (g_session) g_api.update_target(g_session, target);
    else Rebuild();
  }
  if (g_session && work > 0) g_api.report(g_session, work);
}

}  // namespace svr2011

#endif
