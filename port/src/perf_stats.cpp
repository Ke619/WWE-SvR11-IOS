// WWE SmackDown vs. Raw 2011 - performance numbers (see perf_stats.h).

#include "perf_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "frame_rate.h"
#include "frame_stats.h"
#if !defined(SVR2011_D3D_TRACE)
#include "native/gpu.h"
#include "native/native_renderer.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_INT32(perf_log_seconds, 0, "UI",
                     "Every N seconds a perf: log line with the overlay's numbers (RAM, guest heaps, VRAM, CPU, the "
                     "renderer's frame costs), and a warning when RAM or VRAM keeps growing from match to match; 0 off");

namespace svr2011 {

namespace {

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;
PerfSample g_last;
std::chrono::steady_clock::time_point g_last_at{};
bool g_have = false;

// CPU time per thread, between samples.
struct Cpu {
  double process_s = 0;
  std::chrono::steady_clock::time_point at{};
  std::map<uint32_t, double> thread_s;
  std::map<uint32_t, std::string> names;
} g_cpu;

#if defined(_WIN32)
double FileTimeSeconds(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7; }

void Memory(PerfSample& s) {
  PROCESS_MEMORY_COUNTERS_EX c = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c))
    s.ram_ws_mb = c.WorkingSetSize / 1048576.0, s.ram_private_mb = c.PrivateUsage / 1048576.0;
}

std::string ThreadName(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn get = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  PWSTR w = nullptr;
  std::string out;
  if (get && SUCCEEDED(get(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) out.push_back(*p < 128 ? char(*p) : '?');
    LocalFree(w);
  }
  return out;
}

// thread id -> CPU seconds (user + kernel), names for new ones.
std::map<uint32_t, double> ThreadTimes(std::map<uint32_t, std::string>& names) {
  std::map<uint32_t, double> out;
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te = {sizeof te};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) out[te.th32ThreadID] = FileTimeSeconds(k) + FileTimeSeconds(u);
    if (!names.count(te.th32ThreadID)) names[te.th32ThreadID] = ThreadName(h);
    CloseHandle(h);
  }
  CloseHandle(snap);
  return out;
}

double ProcessSeconds() {
  FILETIME c, e, k, u;
  return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? FileTimeSeconds(k) + FileTimeSeconds(u) : 0;
}

int Cores() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return int(si.dwNumberOfProcessors);
}
#else
void Memory(PerfSample& s) {
  if (FILE* f = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      unsigned long kb = 0;
      if (std::sscanf(line, "VmRSS: %lu kB", &kb) == 1) s.ram_ws_mb = kb / 1024.0;
      if (std::sscanf(line, "RssAnon: %lu kB", &kb) == 1) s.ram_private_mb = kb / 1024.0;
    }
    std::fclose(f);
  }
}

// /proc/<...>/stat: the name in parentheses, then utime and stime (fields 14 and 15) in clock ticks.
bool StatTimes(const std::string& path, double& seconds, std::string* name) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char buf[1024] = {};
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  const char* open = std::strchr(buf, '('), *close = std::strrchr(buf, ')');
  if (!open || !close) return false;
  if (name) name->assign(open + 1, close);
  unsigned long ut = 0, st = 0;
  // after ") ": state, ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime
  if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu", &ut, &st) != 2) return false;
  seconds = double(ut + st) / double(sysconf(_SC_CLK_TCK));
  return true;
}

std::map<uint32_t, double> ThreadTimes(std::map<uint32_t, std::string>& names) {
  std::map<uint32_t, double> out;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      const uint32_t tid = uint32_t(std::atoi(e->d_name));
      if (!tid) continue;
      double s = 0;
      std::string name;
      if (StatTimes("/proc/self/task/" + std::string(e->d_name) + "/stat", s, &name)) {
        out[tid] = s;
        if (!names.count(tid)) names[tid] = name;
      }
    }
    closedir(d);
  }
  return out;
}

double ProcessSeconds() {
  double s = 0;
  StatTimes("/proc/self/stat", s, nullptr);
  return s;
}

int Cores() { return std::max(1, int(sysconf(_SC_NPROCESSORS_ONLN))); }
#endif

void CpuUse(PerfSample& s) {
  const auto now = std::chrono::steady_clock::now();
  const double proc = ProcessSeconds();
  auto threads = ThreadTimes(g_cpu.names);
  s.cores = Cores();
  if (g_cpu.at.time_since_epoch().count()) {
    const double wall = std::chrono::duration<double>(now - g_cpu.at).count();
    if (wall > 0) {
      s.cpu_pct = 100.0 * (proc - g_cpu.process_s) / (wall * s.cores);
      std::vector<PerfSample::Thread> v;
      for (const auto& [tid, t] : threads) {
        const auto it = g_cpu.thread_s.find(tid);
        if (it == g_cpu.thread_s.end()) continue;
        std::string n = g_cpu.names[tid];
        if (const size_t p = n.find(" ("); p != std::string::npos) n.resize(p);  // ("XThread18DC (F800005C)": its name)
        v.push_back({n.empty() ? "thread " + std::to_string(tid) : n, 100.0 * (t - it->second) / wall});
      }
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.pct > b.pct; });
      if (v.size() > 3) v.resize(3);
      s.busiest = std::move(v);
    }
  }
  g_cpu.process_s = proc, g_cpu.at = now, g_cpu.thread_s = std::move(threads);
  for (auto it = g_cpu.names.begin(); it != g_cpu.names.end();)  // (names of threads gone)
    it = g_cpu.thread_s.count(it->first) ? std::next(it) : g_cpu.names.erase(it);
}

void GuestMemory(PerfSample& s) {
  if (!g_memory) return;
  auto mb = [](const rex::memory::BaseHeap* h) { return h ? double(h->reserved_page_count()) * h->page_size() / 1048576.0 : 0.0; };
  const rex::memory::BaseHeap* phys = g_memory->GetPhysicalHeap();
  s.guest_physical_mb = mb(phys);
  s.guest_physical_total_mb = phys ? double(phys->heap_size()) / 1048576.0 : 0;
  s.guest_virtual_mb = mb(g_memory->LookupHeap(0x00010000)) + mb(g_memory->LookupHeap(0x40000000));
}

PerfSample Take() {
  PerfSample s;
  Memory(s);
  GuestMemory(s);
  CpuUse(s);
#if !defined(SVR2011_D3D_TRACE)
  uint64_t used = 0, budget = 0;
  if (native::backend::VideoMemory(&used, &budget)) s.vram = true, s.vram_used_mb = used / 1048576.0, s.vram_budget_mb = budget / 1048576.0;
  const native::FrameCosts c = native::LastFrameCosts();
  s.draw_ms = c.draw_ms, s.gpu_wait_ms = c.gpu_wait_ms, s.submit_ms = c.submit_ms, s.draws = c.draws, s.texture_mb = c.texture_mb;
  s.targets_mb = c.targets_mb;
#endif
  return s;
}

// perf_log_seconds: the log line, and the RAM / VRAM after each match (InMatch ends) - growing at every one of 3
// matches in a row by more than a little each: a warning.
void LogThread(int every) {
  rex::thread::set_current_thread_name("perf log");
  std::vector<std::pair<double, double>> ends;  // (private MB, VRAM MB) at each match's end
  bool was_in = false, warned = false;
  int tick = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const bool in = InMatch();
    if (++tick % every == 0) REXLOG_INFO("[svr2011] perf: {}", PerfLine(SamplePerf()));
    if (was_in && !in) {
      const PerfSample s = SamplePerf();
      ends.push_back({s.ram_private_mb, s.vram_used_mb});
      REXLOG_INFO("[svr2011] perf: match {} over: private {:.0f} MB, VRAM {:.0f} MB", ends.size(), s.ram_private_mb, s.vram_used_mb);
      if (ends.size() >= 4 && !warned) {
        const size_t n = ends.size();
        bool ram = true, vram = true;
        for (size_t k = n - 3; k < n; ++k) {
          ram &= ends[k].first > ends[k - 1].first + 8;     // (more than 8 MB a match)
          vram &= ends[k].second > ends[k - 1].second + 8;
        }
        if (ram || vram) {
          warned = true;
          REXLOG_WARN("[svr2011] perf: {} grew after each of the last 3 matches ({:.0f} -> {:.0f} MB): a leak?",
                      ram ? "RAM (private bytes)" : "VRAM", ram ? ends[n - 4].first : ends[n - 4].second,
                      ram ? ends[n - 1].first : ends[n - 1].second);
        }
      }
    }
    was_in = in;
  }
}

// A light memory line every 30 s, perf_log_seconds or not (2.1.9 report 94e7bb: a 4 GB phone's game closed by Android
// after 10-20 minutes, no crash - and nothing in the log to tell growth from a full phone): the process's RAM, on
// Linux / Android its proportional share (PSS, what the low-memory killer weighs) and swap, and what the system has
// left. Its own thread: smaps_rollup takes a while on a big address space.
std::string MemoryLine() {
  PerfSample s;
  Memory(s);
  char b[256];
  std::snprintf(b, sizeof b, "RAM %.0f MB (private %.0f)", s.ram_ws_mb, s.ram_private_mb);
  std::string out = b;
#if defined(_WIN32)
  MEMORYSTATUSEX m = {sizeof m};
  if (GlobalMemoryStatusEx(&m))
    std::snprintf(b, sizeof b, "; system: %.0f MB available of %.0f MB", m.ullAvailPhys / 1048576.0,
                  m.ullTotalPhys / 1048576.0),
        out += b;
#else
  auto kb = [](const char* file, const char* key) {
    long v = -1;
    if (FILE* f = std::fopen(file, "r")) {
      char line[256];
      const size_t n = std::strlen(key);
      while (std::fgets(line, sizeof line, f))
        if (!std::strncmp(line, key, n)) {
          v = std::atol(line + n);
          break;
        }
      std::fclose(f);
    }
    return v;
  };
  if (const long pss = kb("/proc/self/smaps_rollup", "Pss:"); pss >= 0)
    std::snprintf(b, sizeof b, ", PSS %.0f MB", pss / 1024.0), out += b;
  if (const long swap = kb("/proc/self/status", "VmSwap:"); swap >= 0)
    std::snprintf(b, sizeof b, ", swap %.0f MB", swap / 1024.0), out += b;
  const long avail = kb("/proc/meminfo", "MemAvailable:"), total = kb("/proc/meminfo", "MemTotal:");
  if (avail >= 0 && total > 0)
    std::snprintf(b, sizeof b, "; system: %.0f MB available of %.0f MB", avail / 1024.0, total / 1024.0), out += b;
#endif
  return out;
}

void MemoryLogThread() {
  rex::thread::set_current_thread_name("memory log");
  for (;;) {
    REXLOG_INFO("[svr2011] memory: {}", MemoryLine());
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}

}  // namespace

PerfSample SamplePerf() {
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (!g_have || now - g_last_at > std::chrono::milliseconds(500)) g_last = Take(), g_last_at = now, g_have = true;
  return g_last;
}

std::string PerfLine(const PerfSample& s) {
  const FrameTiming t = GetFrameTiming();
  char b[512];
  std::snprintf(b, sizeof b,
                "%.0f fps (%.1f ms, 1%% low %.0f) | RAM %.0f MB (private %.0f) | guest %.0f / %.0f MB physical, %.0f MB "
                "virtual | VRAM %s | CPU %.0f%% of %d cores",
                t.fps, t.frame_ms, t.low_1pct_fps, s.ram_ws_mb, s.ram_private_mb, s.guest_physical_mb,
                s.guest_physical_total_mb, s.guest_virtual_mb,
                s.vram ? (std::to_string(int(s.vram_used_mb)) + " / " + std::to_string(int(s.vram_budget_mb)) + " MB").c_str() : "n/a",
                s.cpu_pct, s.cores);
  std::string out = b;
  for (const auto& th : s.busiest) {
    std::snprintf(b, sizeof b, ", %s %.0f%%", th.name.c_str(), th.pct);
    out += b;
  }
  std::snprintf(b, sizeof b, " | game VRAM %.0f MB (textures %.0f, render targets %.0f) | renderer: draw %.2f ms, GPU wait "
                "%.2f ms, submit %.2f ms, %.0f draws",
                s.texture_mb + s.targets_mb, s.texture_mb, s.targets_mb, s.draw_ms, s.gpu_wait_ms, s.submit_ms, s.draws);
  return out + b;
}

void InstallPerfStats(rex::memory::Memory* memory) {
  g_memory = memory;
  std::thread(MemoryLogThread).detach();
  const int every = REXCVAR_GET(perf_log_seconds);
  if (every > 0) {
    REXLOG_INFO("[svr2011] perf: a line every {} s (perf_log_seconds), and a warning when RAM / VRAM keeps growing", every);
    std::thread(LogThread, every).detach();
  }
}

}  // namespace svr2011
