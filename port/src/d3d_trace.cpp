// WWE SmackDown vs. Raw 2011 - D3D library call census (see d3d_trace.h).

#include "d3d_trace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <windows.h>

#include "frame_stats.h"
#include "util.h"

namespace svr2011::d3d_trace {

namespace {

FILE* fopen_u(const char* path, const char* mode) {
  FILE* f = nullptr;
  return fopen_s(&f, path, mode) == 0 ? f : nullptr;
}

constexpr int kMaxFunctions = 4096;
// Functions whose arguments are checked for shader containers: the shader
// creation functions found by an earlier census that checked every function
// (too slow to leave on: VirtualQuery per argument per call). More can be
// added with SVR2011_D3D_TRACE_CHECK=<hex>,<hex>,...
// 0x82921360 = CreatePixelShader, 0x82921548 = CreateVertexShader.
constexpr uint32_t kCheckedFunctions[] = {0x82921360, 0x82921548};
bool g_check[kMaxFunctions];

std::atomic<uint64_t> g_calls[kMaxFunctions];
std::atomic<uint64_t> g_total[kMaxFunctions];
std::atomic<uint32_t> g_caller[kMaxFunctions];
std::atomic<uint32_t> g_containers[kMaxFunctions];
// Argument ranges of r3..r8 over the first kArgSamples calls of each function.
constexpr uint64_t kArgSamples = 200000;
constexpr int kArgs = 6;
std::atomic<uint32_t> g_arg_min[kMaxFunctions][kArgs];
std::atomic<uint32_t> g_arg_max[kMaxFunctions][kArgs];
std::atomic<uint32_t> g_arg_first[kMaxFunctions][kArgs];

void TrackArg(uint32_t index, int k, uint32_t v, bool first) {
  if (first) {
    g_arg_min[index][k] = v;
    g_arg_max[index][k] = v;
    g_arg_first[index][k] = v;
    return;
  }
  uint32_t cur = g_arg_min[index][k].load(std::memory_order_relaxed);
  while (v < cur && !g_arg_min[index][k].compare_exchange_weak(cur, v)) {}
  cur = g_arg_max[index][k].load(std::memory_order_relaxed);
  while (v > cur && !g_arg_max[index][k].compare_exchange_weak(cur, v)) {}
}

std::filesystem::path g_dir;
std::mutex g_xsc_mutex;
std::unordered_set<uint64_t> g_xsc_seen;

using svr2011::Env;

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const auto end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<const uint8_t*>(p) + n <= end;
}

uint64_t Hash(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// If guest address `addr` holds a shader container, save it (once).
void CheckContainer(uint32_t index, int reg, uint32_t addr, uint8_t* base) {
  if (addr < 0x40000000u || (addr & 3)) return;
  const uint8_t* p = base + addr;
  if (!Readable(p, 0x24)) return;
  const uint32_t flags = LoadBE32(p);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t size = LoadBE32(p + 4) + LoadBE32(p + 8);
  if (size <= 0x24 || size > 0x100000 || LoadBE32(p + 0x1C) || LoadBE32(p + 0x20)) return;
  if (!Readable(p, size)) return;
  g_containers[index].fetch_add(1, std::memory_order_relaxed);
  const uint64_t h = Hash(p, size);
  std::lock_guard lock(g_xsc_mutex);
  if (!g_xsc_seen.insert(h).second) return;
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s.xsc", static_cast<unsigned long long>(h),
                (flags & 1) ? "vs" : "ps");
  if (FILE* f = fopen_u((g_dir / "xsc" / name).string().c_str(), "wb")) {
    std::fwrite(p, 1, size, f);
    std::fclose(f);
  }
  if (FILE* f = fopen_u((g_dir / "xsc.csv").string().c_str(), "a")) {
    std::fprintf(f, "%s,%08X,r%d,%08X,%u\n", name, kFunctions[index], reg, addr, size);
    std::fclose(f);
  }
}

// Object dumps: for (function, argument register), the bytes the argument
// points at, for the first kMaxObjects distinct pointers -> objects.csv.
// Defaults below; more with SVR2011_D3D_TRACE_DUMP=<hex>:r<N>:<bytes>,...
struct DumpSpec {
  uint32_t function;
  int reg;
  uint32_t bytes;
};
constexpr DumpSpec kDefaultDumps[] = {
    {0x82917EC8, 5, 64},   // SetTexture: texture object
    {0x8291DD70, 5, 48},   // SetStreamSource: vertex buffer object
    {0x8291DE90, 4, 48},   // SetIndices: index buffer object
    {0x82920F78, 4, 256},  // SetVertexDeclaration: declaration object
    {0x82921B58, 3, 20480},  // DrawIndexedVertices: the device object itself
};
constexpr size_t kMaxObjects = 400;
struct DumpState {
  int reg[4] = {};
  uint32_t bytes[4] = {};
  int count = 0;
  std::unordered_set<uint64_t> seen;  // (reg << 32) | pointer
  std::atomic<bool> full{false};       // kMaxObjects reached: stop looking
};
DumpState* g_dump[kMaxFunctions];
std::mutex g_dump_mutex;

void DumpObject(uint32_t index, const PPCContext& ctx, uint8_t* base) {
  DumpState& d = *g_dump[index];
  const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  for (int k = 0; k < d.count; ++k) {
    const uint32_t addr = regs[d.reg[k] - 3];
    if (addr < 0x40000000u || (addr & 3)) continue;
    std::lock_guard lock(g_dump_mutex);
    if (d.seen.size() >= kMaxObjects) {
      d.full = true;
      continue;
    }
    if (!d.seen.insert((uint64_t(d.reg[k]) << 32) | addr).second) continue;
    const uint8_t* p = base + addr;
    if (!Readable(p, d.bytes[k])) continue;
    if (FILE* f = fopen_u((g_dir / "objects.csv").string().c_str(), "a")) {
      std::fprintf(f, "%08X,r%d,%08X,", kFunctions[index], d.reg[k], addr);
      for (int r = 0; r < 8; ++r) std::fprintf(f, "%08X%c", regs[r], r == 7 ? ',' : ' ');
      for (uint32_t i = 0; i < d.bytes[k]; ++i) std::fprintf(f, "%02X", p[i]);
      std::fputc('\n', f);
      std::fclose(f);
    }
  }
}

void AddDump(uint32_t function, int reg, uint32_t bytes) {
  for (uint32_t i = 0; i < kFunctionCount && i < kMaxFunctions; ++i) {
    if (kFunctions[i] != function) continue;
    if (!g_dump[i]) g_dump[i] = new DumpState();
    DumpState& d = *g_dump[i];
    if (d.count < 4) {
      d.reg[d.count] = reg;
      d.bytes[d.count] = bytes;
      ++d.count;
    }
  }
}

// Ordered call log: every D3D call (except the GPU spin-wait) during
// kLogFrames frames starting at presented frame SVR2011_D3D_TRACE_FRAMELOG
// -> framelog.csv (frame, thread, function, r4..r8).
struct LogEntry {
  uint32_t frame, tid, function, args[5];
};
constexpr size_t kLogCapacity = 1 << 20;
constexpr uint64_t kLogFrames = 2;
constexpr uint32_t kSpinWait = 0x82919F18;
LogEntry* g_log = nullptr;
std::atomic<size_t> g_log_next{0};
uint64_t g_log_start = 0;
std::atomic<bool> g_log_written{false};

void WriteFrameLog() {
  if (g_log_written.exchange(true)) return;
  const size_t n = std::min(g_log_next.load(), kLogCapacity);
  if (FILE* f = fopen_u((g_dir / "framelog.csv").string().c_str(), "w")) {
    std::fprintf(f, "frame,tid,function,r4,r5,r6,r7,r8\n");
    for (size_t i = 0; i < n; ++i) {
      const LogEntry& e = g_log[i];
      std::fprintf(f, "%u,%u,%08X,%08X,%08X,%08X,%08X,%08X\n", e.frame, e.tid, e.function,
                   e.args[0], e.args[1], e.args[2], e.args[3], e.args[4]);
    }
    std::fclose(f);
  }
}

void LogCall(uint32_t index, const PPCContext& ctx) {
  const uint64_t frame = GetFrameStats().frame_count;
  if (frame < g_log_start || kFunctions[index] == kSpinWait) return;
  if (frame >= g_log_start + kLogFrames) {
    WriteFrameLog();
    return;
  }
  const size_t i = g_log_next.fetch_add(1, std::memory_order_relaxed);
  if (i >= kLogCapacity) return;
  g_log[i] = LogEntry{static_cast<uint32_t>(frame), GetCurrentThreadId(), kFunctions[index],
                      {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32}};
}

// Every 5 s: rewrite calls.csv with the last window's counts.
void Reporter() {
  const std::string path = (g_dir / "calls.csv").string();
  uint64_t last_frames = GetFrameStats().frame_count;
  int window = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const uint64_t frames = GetFrameStats().frame_count;
    const double f = static_cast<double>(frames - last_frames);
    last_frames = frames;
    ++window;
    FILE* out = fopen_u((path + ".tmp").c_str(), "w");
    if (!out) continue;
    std::fprintf(out, "# window %d: %.0f frames\n", window, f);
    std::fprintf(out,
                 "address,calls_window,per_frame,calls_total,caller,containers,"
                 "r3_min,r3_max,r4_min,r4_max,r5_min,r5_max,r6_min,r6_max,r7_min,r7_max,"
                 "r8_min,r8_max\n");
    for (uint32_t i = 0; i < kFunctionCount && i < kMaxFunctions; ++i) {
      const uint64_t n = g_calls[i].exchange(0, std::memory_order_relaxed);
      std::fprintf(out, "%08X,%llu,%.2f,%llu,%08X,%u", kFunctions[i],
                   static_cast<unsigned long long>(n), f > 0 ? n / f : 0.0,
                   static_cast<unsigned long long>(g_total[i].load()),
                   g_caller[i].load(std::memory_order_relaxed), g_containers[i].load());
      for (int k = 0; k < kArgs; ++k) {
        std::fprintf(out, ",%08X,%08X", g_arg_min[i][k].load(), g_arg_max[i][k].load());
      }
      std::fputc('\n', out);
    }
    std::fclose(out);
    std::remove(path.c_str());
    std::rename((path + ".tmp").c_str(), path.c_str());
  }
}

}  // namespace

void OnCall(uint32_t index, const PPCContext& ctx, uint8_t* base) {
  static const bool enabled = [] {
    const std::string dir = Env("SVR2011_D3D_TRACE_DIR");
    if (dir.empty()) return false;
    g_dir = dir;
    std::filesystem::create_directories(g_dir / "xsc");
    std::string extra = Env("SVR2011_D3D_TRACE_CHECK");
    for (uint32_t i = 0; i < kFunctionCount && i < kMaxFunctions; ++i) {
      for (uint32_t a : kCheckedFunctions) g_check[i] |= kFunctions[i] == a;
      char hex[16];
      std::snprintf(hex, sizeof(hex), "%08X", kFunctions[i]);
      g_check[i] |= !extra.empty() && extra.find(hex) != std::string::npos;
    }
    for (const DumpSpec& s : kDefaultDumps) AddDump(s.function, s.reg, s.bytes);
    if (std::string start = Env("SVR2011_D3D_TRACE_FRAMELOG"); !start.empty()) {
      g_log_start = std::strtoull(start.c_str(), nullptr, 10);
      g_log = new LogEntry[kLogCapacity];
    }
    // SVR2011_D3D_TRACE_DUMP=82917EC8:r5:64,...
    std::string dumps = Env("SVR2011_D3D_TRACE_DUMP");
    for (size_t pos = 0; pos < dumps.size();) {
      size_t end = dumps.find(',', pos);
      if (end == std::string::npos) end = dumps.size();
      unsigned fn = 0, reg = 0, bytes = 0;
      if (std::sscanf(dumps.substr(pos, end - pos).c_str(), "%x:r%u:%u", &fn, &reg, &bytes) == 3 &&
          reg >= 3 && reg <= 10 && bytes > 0 && bytes <= 4096)
        AddDump(fn, static_cast<int>(reg), bytes);
      pos = end + 1;
    }
    std::thread(Reporter).detach();
    return true;
  }();
  if (!enabled || index >= kMaxFunctions) return;
  g_calls[index].fetch_add(1, std::memory_order_relaxed);
  const uint64_t total = g_total[index].fetch_add(1, std::memory_order_relaxed);
  g_caller[index].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  if (total < kArgSamples) {
    const uint32_t args[kArgs] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                  ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    for (int k = 0; k < kArgs; ++k) TrackArg(index, k, args[k], total == 0);
  }
  (void)total;
  if (g_check[index]) {
    CheckContainer(index, 3, ctx.r3.u32, base);
    CheckContainer(index, 4, ctx.r4.u32, base);
    CheckContainer(index, 5, ctx.r5.u32, base);
    CheckContainer(index, 6, ctx.r6.u32, base);
  }
  // SVR2011_D3D_TRACE_DEVICE_FRAME=<frame>: snapshot the device object (from
  // the first DrawIndexedVertices at or after that frame) -> device_<frame>.bin
  static const uint64_t device_frame = [] {
    std::string v = Env("SVR2011_D3D_TRACE_DEVICE_FRAME");
    return v.empty() ? ~0ull : std::strtoull(v.c_str(), nullptr, 10);
  }();
  static std::atomic<bool> device_dumped{false};
  if (kFunctions[index] == 0x82921B58 && !device_dumped.load(std::memory_order_relaxed) &&
      GetFrameStats().frame_count >= device_frame && !device_dumped.exchange(true)) {
    const uint8_t* p = base + ctx.r3.u32;
    char name[64];
    std::snprintf(name, sizeof(name), "device_%llu.bin",
                  static_cast<unsigned long long>(GetFrameStats().frame_count));
    if (Readable(p, 0x5000)) {
      if (FILE* f = fopen_u((g_dir / name).string().c_str(), "wb")) {
        std::fwrite(p, 1, 0x5000, f);
        std::fclose(f);
      }
    }
  }
  if (g_log && !g_log_written.load(std::memory_order_relaxed)) {
    LogCall(index, ctx);
  }
  if (g_dump[index] && !g_dump[index]->full.load(std::memory_order_relaxed)) {
    DumpObject(index, ctx, base);
  }
}

}  // namespace svr2011::d3d_trace
