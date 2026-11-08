#include "crowd_off.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include "modmaker/svrfmt/jboy.h"
#include "modmaker/svrfmt/pac.h"

REXCVAR_DEFINE_STRING(arena_crowd, "auto", "GPU",
                      "The arena crowd: on, medium (about half the people), off (no people in the seats: weak phones), "
                      "auto (off in Mali mode)");

namespace svr2011::crowd {
namespace fs = std::filesystem;
using svrfmt::Bytes;

namespace {

// The crowd's people: nested packs 0x4E20 and 0x4E2A of the stage index.
bool IsCrowd(uint32_t id) { return id == 0x4E20 || id == 0x4E2A; }

// A model's meshes down to one vertex and no triangles (as the Mod Maker's
// empty arena - the game still finds the model).
bool Empty(svrfmt::Model& m) {
  bool any = false;
  for (auto& s : m.meshes) {
    if (s.verts.size() <= 1) continue;
    s.verts.resize(1);
    s.uvs.resize(1);
    for (auto& w : s.weights) w.resize(1);
    for (auto& st : s.strips) st.indices.clear();
    const auto& p = s.verts[0].pos;
    s.sphere = {p[0], p[1], p[2], 0.0f};
    any = true;
  }
  return any;
}

// The arena file without its crowd (false: not an arena / nothing to do).
bool Strip(const Bytes& in, Bytes& out, int& emptied, bool half) {
  svrfmt::Epac epac;
  if (!svrfmt::EpacRead(in, epac)) return false;
  for (auto& g : epac.groups)
    for (auto& entry : g.entries) {
      if (!svrfmt::IsPach(entry.data)) continue;
      std::vector<svrfmt::PachEntry> entries;
      if (!svrfmt::PachRead(entry.data, entries)) return false;
      for (auto& e : entries) {
        if (!IsCrowd(e.id)) continue;
        const bool packed = svrfmt::IsBpe(e.data);
        std::vector<svrfmt::PachEntry> people;
        if (!svrfmt::PachRead(svrfmt::Unpack(e.data), people)) continue;
        int index = 0;
        for (auto& person : people) {
          if (half && index++ % 2 == 0) continue;  // (medium: every other person stays)
          const Bytes raw = svrfmt::Unpack(person.data);
          svrfmt::Model m;
          if (!svrfmt::IsJboy(raw) || !svrfmt::JboyRead(raw, m) || !Empty(m)) continue;
          const Bytes w = svrfmt::JboyWrite(m);
          person.data = svrfmt::IsBpe(person.data) ? svrfmt::BpeEncode(w) : w;
          ++emptied;
        }
        const Bytes pach = svrfmt::PachWrite(people);
        e.data = packed ? svrfmt::BpeEncode(pach) : pach;
      }
      if (!emptied) return false;
      entry.data = svrfmt::PachWrite(entries);
      out = svrfmt::EpacWrite(epac);
      return true;  // (one stage index an arena)
    }
  return false;
}

// "<size> <time>" of a file: a copy is redone when its original changes.
std::string Stamp(const fs::path& p) {
  std::error_code e1, e2;
  const auto size = fs::file_size(p, e1);
  const auto time = fs::last_write_time(p, e2).time_since_epoch().count();
  return e1 || e2 ? "" : std::to_string(uint64_t(size)) + " " + std::to_string(int64_t(time));
}

std::string ReadText(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

struct Job {
  fs::path source, copy;
  std::function<void()> ready;
};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_jobs;
std::vector<fs::path> g_queued;  // (copies being made)
bool g_worker = false;

void Make(const Job& job) {
  const auto t0 = std::chrono::steady_clock::now();
  std::ifstream f(job.source, std::ios::binary);
  const Bytes in((std::istreambuf_iterator<char>(f)), {});
  Bytes out;
  int emptied = 0;
  std::error_code ec;
  fs::create_directories(job.copy.parent_path(), ec);
  const fs::path stamp = job.copy.string() + ".stamp";
  const bool half = Level() == 1;
  if (!Strip(in, out, emptied, half) || out.size() > in.size()) {
    // (no crowd, or the copy would not fit the game's arena memory: the original)
    REXLOG_INFO("[svr2011] crowd: {} kept as is ({})", job.source.filename().string(),
                emptied ? "the copy came out bigger" : "no crowd in it");
    std::ofstream(stamp, std::ios::binary) << Stamp(job.source) << " original";
    return;
  }
  const fs::path tmp = job.copy.string() + ".tmp";
  std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  fs::rename(tmp, job.copy, ec);
  if (ec) return;
  std::ofstream(stamp, std::ios::binary) << Stamp(job.source);
  REXLOG_INFO("[svr2011] crowd: {} {} ({} people emptied, {} -> {} KB, {:.1f} s)",
              job.source.filename().string(), half ? "with half its crowd" : "without its crowd", emptied, in.size() / 1024, out.size() / 1024,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  if (job.ready) job.ready();
}

void Worker() {
  rex::thread::set_current_thread_name("crowd off");
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_jobs.empty(); });
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    Make(job);
    std::lock_guard lock(g_mutex);
    g_queued.erase(std::remove(g_queued.begin(), g_queued.end(), job.copy), g_queued.end());
  }
}

}  // namespace

// Mali, known before the GPU is opened: auto's crowd is chosen at the start (the arenas come through their copies
// from the first file the game opens), and the GPU report's own check (backend_vulkan.cpp: vendor 0x13B5 turns
// mali_alpha on) comes seconds later - so a phone started without the launcher's Mali mode (test configs, a home
// screen shortcut, settings reset) kept the whole crowd while the page said LOW. On Android the Mali driver's
// GLES library names it; vulkan_simulate_mali (a PC test of the Mali paths) counts as the GPU report counts it.
bool MaliAtStart(const char** why) {
  if (rex::cvar::GetFlagByName("mali_alpha") == "true") return *why = "mali_alpha", true;
  if (rex::cvar::GetFlagByName("vulkan_simulate_mali") == "true") return *why = "vulkan_simulate_mali", true;
#if defined(__ANDROID__)
  for (const char* lib : {"/vendor/lib64/egl/libGLES_mali.so", "/vendor/lib/egl/libGLES_mali.so",
                          "/system/vendor/lib64/egl/libGLES_mali.so", "/system/lib64/egl/libGLES_mali.so"}) {
    std::error_code e;
    if (fs::exists(lib, e)) return *why = "a Mali GPU driver", true;
  }
#endif
  return false;
}

int Level() {
  static const int level = [] {
    const std::string v = REXCVAR_GET(arena_crowd);
    if (v == "off") return 0;
    if (v == "medium") return 1;
    if (v == "on") return 2;
    const char* why = "";
    const bool mali = MaliAtStart(&why);
    REXLOG_INFO("[svr2011] crowd: auto - {}", mali ? std::string("off (") + why + ")" : std::string("everyone"));
    return mali ? 0 : 2;
  }();
  return level;
}

bool UsesCopies() { return Level() != 2; }

fs::path Serve(const fs::path& source, const fs::path& cache_dir, std::function<void()> ready) {
  if (!UsesCopies()) return source;
  // (one copy per original: a custom arena's file is named after its folder too)
  std::string key = std::string(Level() == 1 ? "half_" : "") + source.parent_path().filename().string() + "_" +
                    source.filename().string();
  const fs::path copy = cache_dir / key;
  const std::string want = Stamp(source);
  const std::string have = ReadText(copy.string() + ".stamp");
  std::error_code ec;
  if (!want.empty() && have == want && fs::exists(copy, ec)) return copy;
  if (!want.empty() && have == want + " original") return source;  // (nothing to strip)
  std::lock_guard lock(g_mutex);
  if (std::find(g_queued.begin(), g_queued.end(), copy) == g_queued.end()) {
    g_queued.push_back(copy);
    g_jobs.push_back({source, copy, std::move(ready)});
    if (!g_worker) {
      g_worker = true;
      std::thread(Worker).detach();
    }
    g_wake.notify_one();
  }
  return source;
}

}  // namespace svr2011::crowd
