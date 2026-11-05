// WWE SmackDown vs. Raw 2011 - online test aids and packet counts (net_sim.h).
#include "net_sim.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include <rex/logging.h>

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

struct SimConfig {
  bool on = false;
  int delay_ms = 0, jitter_ms = 0;
  double loss = 0;  // (0-1)
};

const SimConfig& Config() {
  static const SimConfig c = [] {
    SimConfig s;
    const char* v = std::getenv("SVR2011_TEST_NET_DELAY");
    if (!v || !*v) return s;
    s.on = true;
    s.delay_ms = std::max(0, std::atoi(v));
    if (const char* a = std::strchr(v, ',')) {
      s.jitter_ms = std::max(0, std::atoi(a + 1));
      if (const char* b = std::strchr(a + 1, ',')) s.loss = std::clamp(std::atof(b + 1) / 100.0, 0.0, 1.0);
    }
    REXLOG_INFO("net sim: game packets out {} ms + up to {} ms jitter, {:.1f}% lost", s.delay_ms, s.jitter_ms,
                s.loss * 100);
    return s;
  }();
  return c;
}

// The link: packets due at their times, sent in order by one thread.
std::mutex g_sim_mutex;
std::condition_variable g_sim_wake;
std::multimap<Clock::time_point, std::function<void()>> g_queue;
Clock::time_point g_last_due{};

void SimThread() {
  std::unique_lock lock(g_sim_mutex);
  for (;;) {
    if (g_queue.empty()) {
      g_sim_wake.wait(lock);
      continue;
    }
    const auto due = g_queue.begin()->first;
    if (Clock::now() < due) {
      g_sim_wake.wait_until(lock, due);
      continue;
    }
    auto send = std::move(g_queue.begin()->second);
    g_queue.erase(g_queue.begin());
    lock.unlock();
    send();
    lock.lock();
  }
}

// -- packet counts ----------------------------------------------------------------
struct PeerCounts {
  uint64_t out = 0, in = 0, out_bytes = 0, in_bytes = 0, relayed_in = 0, relayed_out = 0;
  double max_gap_ms = 0;
  Clock::time_point last_in{};
};
std::mutex g_count_mutex;
std::map<uint32_t, PeerCounts> g_counts;
std::atomic<bool> g_reporter{false};

std::string Ip(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
         "." + std::to_string(ip & 255);
}

void Reporter() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::map<uint32_t, PeerCounts> got;
    {
      std::lock_guard lock(g_count_mutex);
      for (auto& [peer, c] : g_counts) {
        if (!c.in && !c.out) continue;
        got[peer] = c;
        const auto last = c.last_in;
        c = {};
        c.last_in = last;
      }
    }
    for (const auto& [peer, c] : got) {
      REXLOG_INFO("net: peer {} out {} pk/s ({} B/s{}), in {} pk/s ({} B/s{}), longest gap in {:.0f} ms", Ip(peer),
                  c.out / 5, c.out_bytes / 5, c.relayed_out ? ", " + std::to_string(c.relayed_out) + " relayed" : "",
                  c.in / 5, c.in_bytes / 5, c.relayed_in ? ", " + std::to_string(c.relayed_in) + " relayed" : "",
                  c.max_gap_ms);
    }
  }
}

bool CountsOn() {
  static const bool on = [] {
    const char* v = std::getenv("SVR2011_NET_STATS");
    return v && v[0] == '2';
  }();
  return on;
}

}  // namespace

bool NetSimActive() { return Config().on; }

void NetSimSend(std::function<void()> send) {
  const SimConfig& c = Config();
  if (!c.on) return send();
  static std::mt19937 rng{std::random_device{}()};
  static std::once_flag started;
  std::call_once(started, [] { std::thread(SimThread).detach(); });
  std::lock_guard lock(g_sim_mutex);
  if (c.loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < c.loss) return;
  const int jitter = c.jitter_ms ? std::uniform_int_distribution<int>(0, c.jitter_ms)(rng) : 0;
  // (in order, as most internet paths keep them)
  const auto due = std::max(g_last_due, Clock::now() + std::chrono::milliseconds(c.delay_ms + jitter));
  g_last_due = due;
  g_queue.emplace(due, std::move(send));
  g_sim_wake.notify_one();
}

bool NetForceTunnel() {
  static const bool on = [] {
    const char* v = std::getenv("SVR2011_TEST_NET_TUNNEL");
    return v && v[0] == '1';
  }();
  return on;
}

namespace {
struct Rtt {
  float last = 0;
  std::vector<std::pair<Clock::time_point, float>> recent;  // (the last 5 s)
  bool relayed = false;
  const char* how = "";
  std::string name;
};
std::mutex g_rtt_mutex;
std::map<uint32_t, Rtt> g_rtts;
}  // namespace

void NetRtt(uint32_t peer, double ms, bool relayed, const char* how) {
  const auto now = Clock::now();
  std::lock_guard lock(g_rtt_mutex);
  Rtt& r = g_rtts[peer];
  if (std::string_view(r.how) != how) REXLOG_INFO("net: peer {} pings over {} ({:.0f} ms)", Ip(peer), how, ms);
  r.last = float(ms), r.relayed = relayed, r.how = how;
  r.recent.emplace_back(now, float(ms));
  std::erase_if(r.recent, [&](const auto& e) { return now - e.first > std::chrono::seconds(5); });
}

void NetPeerName(uint32_t peer, const std::string& name) {
  std::lock_guard lock(g_rtt_mutex);
  g_rtts[peer].name = name;
}

std::vector<NetLink> NetLinks() {
  const auto now = Clock::now();
  std::vector<NetLink> links;
  std::lock_guard lock(g_rtt_mutex);
  for (auto& [peer, r] : g_rtts) {
    std::erase_if(r.recent, [&](const auto& e) { return now - e.first > std::chrono::seconds(5); });
    if (r.recent.empty()) continue;
    NetLink l;
    l.peer = peer, l.rtt_ms = r.last, l.relayed = r.relayed, l.how = r.how, l.name = r.name;
    for (const auto& e : r.recent) l.worst_ms = std::max(l.worst_ms, e.second);
    links.push_back(l);
  }
  return links;
}

bool NetDataV1() {
  static const bool on = [] {
    const char* v = std::getenv("SVR2011_TEST_NET_V1");
    return v && v[0] == '1';
  }();
  return on;
}

void NetPacket(bool out, uint32_t peer, size_t size, bool relayed) {
  if (!CountsOn()) return;
  if (!g_reporter.exchange(true)) std::thread(Reporter).detach();
  std::lock_guard lock(g_count_mutex);
  PeerCounts& c = g_counts[peer];
  if (out) {
    ++c.out, c.out_bytes += size, c.relayed_out += relayed;
  } else {
    const auto now = Clock::now();
    if (c.last_in != Clock::time_point{})
      c.max_gap_ms = std::max(c.max_gap_ms, std::chrono::duration<double, std::milli>(now - c.last_in).count());
    c.last_in = now;
    ++c.in, c.in_bytes += size, c.relayed_in += relayed;
  }
}

}  // namespace svr2011
