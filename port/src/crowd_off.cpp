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
                      "The arena crowd: on, off (no people in the seats: weak phones), auto (off in Mali mode)");

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
bool Strip(const Bytes& in, Bytes& out, int& emptied) {
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
        for (auto& person : people) {
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
  if (!Strip(in, out, emptied) || out.size() > in.size()) {
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
  REXLOG_INFO("[svr2011] crowd: {} without its crowd ({} people, {} -> {} KB, {:.1f} s)",
              job.source.filename().string(), emptied, in.size() / 1024, out.size() / 1024,
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

bool Off() {
  static const bool off = [] {
    const std::string v = REXCVAR_GET(arena_crowd);
    if (v == "off") return true;
    if (v == "on") return false;
    return rex::cvar::GetFlagByName("mali_alpha") == "true";
  }();
  return off;
}

fs::path Serve(const fs::path& source, const fs::path& cache_dir, std::function<void()> ready) {
  if (!Off()) return source;
  // (one copy per original: a custom arena's file is named after its folder too)
  std::string key = source.parent_path().filename().string() + "_" + source.filename().string();
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
