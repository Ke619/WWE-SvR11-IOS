// WWE SmackDown vs. Raw 2011 - how long the game has been played (the
// launcher's Play tab shows it).
//
// <game>/playtime.txt holds the total ("seconds=N", "sessions=N"); while the
// game runs, the time since the last write is added to it every 30 s, so a
// crash loses at most that. Written to a .tmp and renamed over the old file
// (never left half-written). Test runs (a scripted controller) don't count.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace svr2011 {
namespace {

struct Totals {
  uint64_t seconds = 0, sessions = 0;
  bool ok = false;  // (the file read, with its total)
};

Totals Read(const std::filesystem::path& file) {
  Totals t;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const uint64_t v = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
    if (key == "seconds") t.seconds = v, t.ok = true;
    if (key == "sessions") t.sessions = v;
  }
  return t;
}

bool Write(const std::filesystem::path& file, const Totals& t) {
  const auto tmp = std::filesystem::path(file).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# WWE SmackDown vs. Raw 2011 PC port - time played (the launcher shows it)\n"
        << "seconds=" << t.seconds << "\nsessions=" << t.sessions << "\n";
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  return !ec;
}

}  // namespace

void InstallPlaytime() {
  if (std::getenv("SVR2011_INPUT_FILE")) return;  // (a test)
  const auto file = rex::filesystem::GetExecutableFolder() / "playtime.txt";
  std::thread([file] {
    rex::thread::set_current_thread_name("playtime");
    using Clock = std::chrono::steady_clock;
    Totals t = Read(file);
    ++t.sessions;
    Write(file, t);
    REXLOG_INFO("[svr2011] playtime: {} h {} min before this session", t.seconds / 3600, t.seconds / 60 % 60);
    auto last = Clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      const auto now = Clock::now();
      const uint64_t add = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(now - last).count());
      // (re-read: another copy of the game in the same folder adds its own)
      Totals cur = Read(file);
      std::error_code ec;
      if (!cur.ok && std::filesystem::exists(file, ec)) continue;  // (not readable now: never written over with 0)
      cur.seconds += add;
      if (cur.sessions == 0) cur.sessions = t.sessions;
      if (Write(file, cur)) last += std::chrono::seconds(add);  // (the part second carries on)
    }
  }).detach();
}

}  // namespace svr2011
