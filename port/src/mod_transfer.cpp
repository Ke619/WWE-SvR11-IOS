// WWE SmackDown vs. Raw 2011 - online: a host's gameplay mods to a joiner who lacks them (mod_transfer.h).
//
// Messages (p2p.cpp kModXfer, to one peer, direct links only; little-endian):
//   1 set?   {u16 page}                         -> 2 set   {u32 gen, u16 page, u16 pages, text}
//   3 list?  {u32 gen, u16 mod, u16 page}       -> 4 list  {u32 gen, u16 mod, u16 page, u16 pages,
//                                                           entries {u16 len, path, u32 size, u32 crc}}
//   5 data?  {u32 gen, u16 mod, u16 file, u32 chunk, u16 count}
//                                               -> 6 data  {u32 gen, u16 mod, u16 file, u32 chunk, bytes}
//                                                  (one message a chunk, kChunk bytes but the file's last)
//   7 error  {u8 code, text}
// gen = the CRC of the host's set text: a set that changed (a mod added or removed) makes old indices
// unknown (error 3), and the joiner starts over.
// A set line: kind \t id \t version \t crc \t name \t bytes \t files.
//
// The host answers only while it hosts a session, at most online_mod_upload_peers joiners at once, its data
// paced to online_mod_upload_kbs in all (a sender thread: the requests wait their turn, never the network
// thread). The joiner keeps a window of requested ranges: it grows while ranges come back whole and halves
// when one is late, so a slow or busy link isn't flooded.
#include "mod_transfer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include "../modmaker/svrfmt/mod_travel.h"
#include "online_files.h"
#include "online_mods.h"
#include "p2p.h"
#include "util.h"

REXCVAR_DEFINE_BOOL(online_mods_load, false, "Online",
                    "This run also loads the mods fetched from a session's host (<user data>\\OnlineMods; set by the "
                    "game's RESTART AND REJOIN, never saved)");
REXCVAR_DEFINE_BOOL(online_mod_upload, true, "Online",
                    "Hosting: send this game's gameplay mods to joiners who lack them (direct links only)");
REXCVAR_DEFINE_INT32(online_mod_upload_kbs, 4096, "Online",
                     "Hosting: the most KB a second this game sends of its mods to joiners who lack them (in all)");
REXCVAR_DEFINE_INT32(online_mod_upload_peers, 2, "Online",
                     "Hosting: how many joiners may fetch this game's mods at once (the others wait)");
REXCVAR_DEFINE_INT32(online_mod_max_mb, 300, "Online", "Joining: the largest mod (MB) fetched from a host");
REXCVAR_DEFINE_INT32(online_mods_max_mb, 4096, "Online",
                     "Joining: the most MB of mods fetched from one host for a session");

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace svr2011 {
namespace {

constexpr uint32_t kChunk = 1200;        // (a message stays under a 1500-byte MTU with the p2p header)
constexpr uint32_t kRangeChunks = 32;    // one request: 32 chunks (38 KB)
constexpr int kWindowStart = 7, kWindowMin = 2, kWindowMax = 54;  // ranges in flight (256 KB .. 2 MB)
constexpr size_t kMaxPayload = 1350;
enum : uint8_t { kSetReq = 1, kSet = 2, kListReq = 3, kList = 4, kDataReq = 5, kData = 6, kError = 7 };
enum : uint8_t { kErrBusy = 1, kErrNotHosting = 2, kErrUnknown = 3, kErrUnsendable = 4, kErrRead = 5, kErrOff = 6 };

// -- bytes ------------------------------------------------------------------------------

struct Out {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
  void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
  void str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
};
struct In {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool need(size_t k) { return ok = ok && at + k <= n; }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  uint16_t u16() { const uint16_t lo = u8(); return uint16_t(lo | u8() << 8); }
  uint32_t u32() { const uint32_t lo = u16(); return lo | uint32_t(u16()) << 16; }
  std::string str(size_t k) {
    if (!need(k)) return {};
    std::string s(reinterpret_cast<const char*>(p + at), k);
    at += k;
    return s;
  }
  std::string rest() { return str(n - at); }
};

std::vector<std::string> Split(const std::string& s, char by) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == by) out.push_back(cur), cur.clear();
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

using svr2011::Lower;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

}  // namespace

// -- what may travel -----------------------------------------------------------------------

bool ModFileAllowed(const std::string& rel, std::string* why) { return svrfmt::ModFileAllowed(rel, why); }

namespace {

// -- the host -------------------------------------------------------------------------------

struct HostFile {
  std::string rel;
  fs::path path;
  uint32_t size = 0, crc = 0;
};
struct HostMod {
  std::string kind, id, version, crc, name;
  fs::path dir;
  uint64_t bytes = 0;
  uint32_t count = 0;
  bool listed = false, listing = false;  // (files + their CRCs read: on a thread, at the first list request)
  std::string unsendable;
  std::vector<HostFile> files;
};
struct HostRequest {
  uint64_t peer;
  uint32_t gen, chunk;
  uint16_t mod, file, count;
};

std::mutex g_host_mutex;
std::condition_variable g_host_cv;
uint32_t g_gen = 0;
std::string g_set_text;
std::vector<HostMod> g_host_mods;
std::deque<HostRequest> g_queue;
std::map<uint64_t, int64_t> g_peer_seen;  // joiner -> its last request (ms)
std::atomic<uint64_t> g_sent_bytes{0};

void SendTo(uint64_t peer, const Out& o) { P2PSendModXfer(peer, o.b); }

void SendError(uint64_t peer, uint8_t code, const std::string& text) {
  Out o;
  o.u8(kError), o.u8(code), o.str(text.substr(0, 300));
  SendTo(peer, o);
}

using svrfmt::CosmeticFile;  // (the files outside the mod's key: mod_travel.h)

// The host's set now (by its text: rebuilt when it changes; a mod that can't travel says why, so the joiner
// is told before it asks). Caller holds g_host_mutex.
void RefreshSet() {
  const std::string& set = GameplayModSet();
  const uint32_t gen = Crc32(0, set.data(), set.size());
  if (gen == g_gen && !g_host_mods.empty()) return;
  g_gen = gen;
  g_host_mods.clear();
  g_set_text.clear();
  const fs::path mods = rex::filesystem::GetExecutableFolder() / "Mods";
  for (const std::string& line : Split(set, '\n')) {
    const auto f = Split(line, '\t');
    if (f.size() < 5 || f[0].rfind("test", 0) == 0) continue;
    HostMod m{f[0], f[1], f[2], f[3], f[4]};
    m.dir = mods / m.kind / m.id;
    std::error_code ec;
    if (!fs::is_directory(m.dir, ec)) continue;  // (the test aid's made-up lines)
    for (const auto& e : fs::recursive_directory_iterator(m.dir, ec)) {
      if (!e.is_regular_file()) continue;
      const std::string rel = fs::relative(e.path(), m.dir, ec).generic_string();
      std::string why;
      if (!ModFileAllowed(rel, &why)) {
        if (!CosmeticFile(e.path()) && m.unsendable.empty()) m.unsendable = rel + " (" + why + ")";
        continue;
      }
      m.bytes += e.file_size(ec);
      ++m.count;
    }
    g_set_text += m.kind + "\t" + m.id + "\t" + m.version + "\t" + m.crc + "\t" + m.name + "\t" +
                  std::to_string(m.bytes) + "\t" + std::to_string(m.count) + "\t" + m.unsendable + "\n";
    g_host_mods.push_back(std::move(m));
  }
}

// A mod's files with their CRCs (read once, on a thread: a big mod takes a second - never the network
// thread's). A file that changes gameplay but can't travel makes the mod unsendable (the joiner's copy would
// never match the host's key). Works on a copy, stored if the set is still the same.
void ListMod(uint32_t gen, size_t index, HostMod m) {
  rex::thread::set_current_thread_name("mod transfer list");
  std::error_code ec;
  std::vector<fs::path> all;
  for (const auto& e : fs::recursive_directory_iterator(m.dir, ec))
    if (e.is_regular_file()) all.push_back(e.path());
  std::sort(all.begin(), all.end());
  for (const fs::path& p : all) {
    const std::string rel = fs::relative(p, m.dir, ec).generic_string();
    std::string why;
    if (!ModFileAllowed(rel, &why)) {
      REXLOG_INFO("mod transfer: {}\\{}: {} isn't sent - {}", m.kind, m.id, rel, why);
      if (!CosmeticFile(p) && m.unsendable.empty()) m.unsendable = rel + " (" + why + ")";
      continue;
    }
    HostFile f{rel, p};
    const uint64_t size = fs::file_size(p, ec);
    if (size > 0xFFFFFFF0ull) { m.unsendable = rel + " (larger than 4 GB)"; continue; }
    f.size = uint32_t(size);
    if (!FileCrc32(p, &f.crc)) { m.unsendable = rel + " (unreadable)"; continue; }
    m.files.push_back(std::move(f));
  }
  std::lock_guard lock(g_host_mutex);
  if (gen != g_gen || index >= g_host_mods.size()) return;
  m.listed = true, m.listing = false;
  REXLOG_INFO("mod transfer: {}\\{} listed: {} files{}", m.kind, m.id, m.files.size(),
              m.unsendable.empty() ? "" : " - can't be sent: " + m.unsendable);
  g_host_mods[index] = std::move(m);
}

// One request: may this joiner be served (hosting, and within the joiners allowed at once)?
bool Admit(uint64_t peer) {
  uint64_t session = 0;
  if (!P2PHosting(&session)) {
    SendError(peer, kErrNotHosting, "This game doesn't host a session now.");
    return false;
  }
  if (!REXCVAR_GET(online_mod_upload)) {
    SendError(peer, kErrOff, "The host doesn't send its mods (its online_mod_upload setting is off).");
    return false;
  }
  const int64_t now = NowMs();
  for (auto it = g_peer_seen.begin(); it != g_peer_seen.end();)
    it = now - it->second > 5000 ? g_peer_seen.erase(it) : std::next(it);
  if (!g_peer_seen.count(peer) && int(g_peer_seen.size()) >= std::max(1, REXCVAR_GET(online_mod_upload_peers))) {
    SendError(peer, kErrBusy, "The host is sending its mods to other players - waiting for a turn.");
    return false;
  }
  g_peer_seen[peer] = now;
  return true;
}

void OnHostMessage(uint64_t peer, In& in, uint8_t op) {
  std::unique_lock lock(g_host_mutex);
  if (!Admit(peer)) return;
  RefreshSet();
  if (op == kSetReq) {
    const uint16_t page = in.u16();
    const size_t per = kMaxPayload - 16, pages = std::max<size_t>(1, (g_set_text.size() + per - 1) / per);
    if (page >= pages) return;
    Out o;
    o.u8(kSet), o.u32(g_gen), o.u16(page), o.u16(uint16_t(pages));
    o.str(g_set_text.substr(page * per, per));
    lock.unlock();
    return SendTo(peer, o);
  }
  const uint32_t gen = in.u32();
  const uint16_t mod = in.u16();
  if (!in.ok) return;
  if (gen != g_gen || mod >= g_host_mods.size()) {
    lock.unlock();
    return SendError(peer, kErrUnknown, "The host's mods changed - asking again.");
  }
  HostMod& m = g_host_mods[mod];
  if (!m.listed) {  // (the joiner asks again: it is listed by then)
    if (!m.listing) {
      m.listing = true;
      std::thread(ListMod, g_gen, size_t(mod), m).detach();
    }
    return;
  }
  if (!m.unsendable.empty()) {
    const std::string text = m.name + " can't be sent: " + m.unsendable;
    lock.unlock();
    return SendError(peer, kErrUnsendable, text);
  }
  if (op == kListReq) {
    const uint16_t page = in.u16();
    // (pages of whole entries)
    std::vector<std::pair<size_t, size_t>> pages;  // first entry, count
    size_t at = 0;
    while (at < m.files.size() || pages.empty()) {
      size_t bytes = 16, k = at;
      while (k < m.files.size() && bytes + 10 + m.files[k].rel.size() <= kMaxPayload) bytes += 10 + m.files[k++].rel.size();
      pages.emplace_back(at, k - at);
      if (k == at) break;
      at = k;
    }
    if (page >= pages.size()) return;
    Out o;
    o.u8(kList), o.u32(g_gen), o.u16(mod), o.u16(page), o.u16(uint16_t(pages.size()));
    for (size_t i = pages[page].first; i < pages[page].first + pages[page].second; ++i) {
      o.u16(uint16_t(m.files[i].rel.size())), o.str(m.files[i].rel), o.u32(m.files[i].size), o.u32(m.files[i].crc);
    }
    lock.unlock();
    return SendTo(peer, o);
  }
  if (op == kDataReq) {
    HostRequest r{peer, gen, 0, mod, in.u16(), 0};
    r.chunk = in.u32(), r.count = uint16_t(std::min<uint32_t>(in.u16(), kRangeChunks * 2));
    if (!in.ok || r.file >= m.files.size() || !r.count) return;
    // (a joiner's queue stays short: a request it repeated replaces the older one)
    size_t mine = 0;
    for (auto it = g_queue.begin(); it != g_queue.end();) {
      if (it->peer == peer && it->mod == r.mod && it->file == r.file && it->chunk == r.chunk) {
        it = g_queue.erase(it);
        continue;
      }
      mine += it->peer == peer;
      ++it;
    }
    if (mine >= 128) return;
    g_queue.push_back(r);
    g_host_cv.notify_one();
  }
}

// The sender: the requests in turn, paced to online_mod_upload_kbs (a token bucket of 1/8 s).
void SenderLoop() {
  std::FILE* open = nullptr;
  fs::path open_path;
  double tokens = 0;
  auto last = Clock::now();
  std::vector<char> buf(kChunk);
  for (;;) {
    HostRequest r;
    fs::path path;
    uint32_t size = 0;
    {
      std::unique_lock lock(g_host_mutex);
      g_host_cv.wait_for(lock, std::chrono::seconds(2), [] { return !g_queue.empty(); });
      if (g_queue.empty()) {
        if (open) std::fclose(open), open = nullptr, open_path.clear();
        continue;
      }
      r = g_queue.front();
      g_queue.pop_front();
      if (r.gen != g_gen || r.mod >= g_host_mods.size() || r.file >= g_host_mods[r.mod].files.size()) continue;
      path = g_host_mods[r.mod].files[r.file].path;
      size = g_host_mods[r.mod].files[r.file].size;
    }
    if (path != open_path) {
      if (open) std::fclose(open);
      open = std::fopen(path.string().c_str(), "rb");
      open_path = open ? path : fs::path();
      if (!open) {
        SendError(r.peer, kErrRead, "The host couldn't read one of the mod's files.");
        continue;
      }
    }
    for (uint32_t c = r.chunk; c < r.chunk + r.count; ++c) {
      const uint64_t at = uint64_t(c) * kChunk;
      if (at >= size && !(size == 0 && c == 0)) break;
      const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, size - at));
      // (wait for the bytes the rate allows)
      const double rate = std::max(64, REXCVAR_GET(online_mod_upload_kbs)) * 1024.0;
      for (;;) {
        const auto now = Clock::now();
        tokens = std::min(rate / 8, tokens + rate * std::chrono::duration<double>(now - last).count());
        last = now;
        if (tokens >= n) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      tokens -= n;
      if (n && (std::fseek(open, long(at), SEEK_SET) != 0 || std::fread(buf.data(), 1, n, open) != n)) {
        SendError(r.peer, kErrRead, "The host couldn't read one of the mod's files.");
        break;
      }
      Out o;
      o.u8(kData), o.u32(r.gen), o.u16(r.mod), o.u16(r.file), o.u32(c);
      o.b.insert(o.b.end(), buf.begin(), buf.begin() + n);
      if (!P2PSendModXfer(r.peer, o.b)) break;  // (the link went relayed or the peer left)
      g_sent_bytes += n;
    }
  }
}

// -- the joiner -------------------------------------------------------------------------------

struct Incoming {
  uint8_t op;
  std::vector<uint8_t> data;
};
std::mutex g_dl_mutex;
std::condition_variable g_dl_cv;
std::deque<Incoming> g_inbox;
ModDownloadState g_state;
std::atomic<bool> g_running{false}, g_cancel{false};
uint64_t g_host = 0;
std::thread g_thread;

int TestLoss() {
  static const int v = [] { const char* e = std::getenv("SVR2011_TEST_XFER_LOSS"); return e ? std::atoi(e) : 0; }();
  return v;
}

// (never a relative path: no user data folder known = nothing fetched, nothing cleared)
fs::path Root() { return UserDataFolder().empty() ? fs::path() : UserDataFolder() / "OnlineMods"; }

void OnJoinerMessage(uint64_t peer, uint8_t op, const uint8_t* data, size_t n) {
  if (!g_running || peer != g_host) return;
  if (op == kData && TestLoss() > 0 && std::rand() % 100 < TestLoss()) return;  // (test: a lossy link)
  std::lock_guard lock(g_dl_mutex);
  if (g_inbox.size() > 20000) return;
  g_inbox.push_back({op, std::vector<uint8_t>(data, data + n)});
  g_dl_cv.notify_one();
}

void OnMessage(uint64_t peer, const uint8_t* data, size_t n) {
  if (!n) return;
  In in{data, n};
  const uint8_t op = in.u8();
  if (op == kSetReq || op == kListReq || op == kDataReq) return OnHostMessage(peer, in, op);
  OnJoinerMessage(peer, op, data + 1, n - 1);
}

void SetState(ModDownloadState::Phase phase, const std::string& error = {}) {
  std::lock_guard lock(g_dl_mutex);
  g_state.phase = phase;
  if (!error.empty()) g_state.error = error;
}

// Waits for an answer of this kind (or an error); nullopt on time-out / cancel. Caller doesn't hold the lock.
std::optional<Incoming> Await(uint8_t op, int ms, std::string* error, uint8_t* code = nullptr) {
  const auto until = Clock::now() + std::chrono::milliseconds(ms);
  std::unique_lock lock(g_dl_mutex);
  while (!g_cancel) {
    for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
      if (it->op == op || it->op == kError) {
        Incoming got = std::move(*it);
        g_inbox.erase(it);
        if (got.op == kError) {
          In in{got.data.data(), got.data.size()};
          const uint8_t c = in.u8();
          if (code) *code = c;
          if (error) *error = in.rest();
          return std::nullopt;
        }
        return got;
      }
    }
    if (g_dl_cv.wait_until(lock, until) == std::cv_status::timeout) break;
  }
  return std::nullopt;
}

bool Send(const Out& o) { return P2PSendModXfer(g_host, o.b); }

struct Listed {
  std::string rel;
  uint32_t size, crc;
};

// The set (all pages), with its gen; empty on failure.
std::vector<ModOffer> FetchSet(uint32_t* gen, std::string* error) {
  std::string text;
  uint16_t pages = 1;
  for (uint16_t page = 0; page < pages; ++page) {
    std::optional<Incoming> got;
    const auto give_up = Clock::now() + std::chrono::minutes(3);
    for (int tries = 0; tries < 4 && !got && !g_cancel; ++tries) {
      Out o;
      o.u8(kSetReq), o.u16(page);
      if (!Send(o)) {
        if (error) *error = "There's no direct link to the host: mods only come straight from the host, never through the relay.";
        return {};
      }
      uint8_t code = 0;
      error->clear();
      got = Await(kSet, 1500, error, &code);
      if (!got && code == kErrBusy && Clock::now() < give_up) {  // (other joiners first: wait for a turn)
        SetState(ModDownloadState::kAsking, *error);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        tries = -1;
        continue;
      }
      if (!got && !error->empty()) return {};
    }
    if (!got) {
      if (error && error->empty()) *error = "The host didn't answer.";
      return {};
    }
    In in{got->data.data(), got->data.size()};
    *gen = in.u32();
    if (in.u16() != page) return {};
    pages = in.u16();
    text += in.rest();
  }
  std::vector<ModOffer> out;
  for (const std::string& line : Split(text, '\n')) {
    const auto f = Split(line, '\t');
    if (f.size() < 7) continue;
    ModOffer m{f[0], f[1], f[2], f[3], f[4]};
    m.bytes = std::strtoull(f[5].c_str(), nullptr, 10);
    m.files = uint32_t(std::strtoul(f[6].c_str(), nullptr, 10));
    if (f.size() >= 8) m.unsendable = f[7];
    out.push_back(std::move(m));
  }
  return out;
}

fs::path ModDir(const ModOffer& m) { return Root() / m.kind / (m.id + "@" + m.crc); }

bool SafeName(const std::string& s) {
  return !s.empty() && s.size() < 100 && s.find_first_of("/\\:*?\"<>|") == std::string::npos && s != "." &&
         s != ".." && ModFileAllowed(s + "/x.txt");
}

// One file in ranges (resuming a .part); false: failed (error set) or cancelled.
bool FetchFile(uint32_t gen, uint16_t mod, uint16_t index, const Listed& f, const fs::path& dest, std::string* error) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  uint32_t have_crc = 0;
  if (fs::exists(dest, ec) && fs::file_size(dest, ec) == f.size && FileCrc32(dest, &have_crc) && have_crc == f.crc)
    return true;  // (fetched already)
  const fs::path part = fs::path(dest).concat(".part"), map_path = fs::path(dest).concat(".partmap");
  const uint32_t chunks = std::max<uint32_t>(1, (f.size + kChunk - 1) / kChunk);
  std::vector<uint8_t> have((chunks + 7) / 8, 0);
  if (fs::exists(part, ec) && fs::file_size(part, ec) == f.size) {
    std::ifstream m(map_path, std::ios::binary);
    std::vector<uint8_t> saved((std::istreambuf_iterator<char>(m)), {});
    if (saved.size() == have.size()) have = saved;
  } else {
    std::ofstream(part, std::ios::binary | std::ios::trunc).close();
    fs::resize_file(part, f.size, ec);
  }
  std::FILE* out = std::fopen(part.string().c_str(), "r+b");
  if (!out) {
    *error = "Couldn't write the mod's files (" + dest.filename().string() + ").";
    return false;
  }
  auto has = [&](uint32_t c) { return (have[c >> 3] >> (c & 7) & 1) != 0; };
  uint32_t left = 0;
  for (uint32_t c = 0; c < chunks; ++c) left += !has(c);
  {
    std::lock_guard lock(g_dl_mutex);
    g_state.done += std::min<uint64_t>(f.size, uint64_t(chunks - left) * kChunk);  // (resumed: what was there)
  }
  std::map<uint32_t, Clock::time_point> pending;  // range start -> asked
  uint32_t next = 0;
  int window = kWindowStart;
  double rtt_ms = 200;
  auto last_cut = Clock::now(), last_progress = Clock::now(), last_map = Clock::now(), rate_at = Clock::now();
  uint64_t rate_bytes = 0;
  bool ok = true;
  auto ask = [&](uint32_t start) {
    uint32_t first = start, end = std::min(chunks, start + kRangeChunks);
    while (first < end && has(first)) ++first;
    while (end > first && has(end - 1)) --end;
    if (first >= end) return false;
    Out o;
    o.u8(kDataReq), o.u32(gen), o.u16(mod), o.u16(index), o.u32(first), o.u16(uint16_t(end - first));
    if (!Send(o)) return false;
    pending[start] = Clock::now();
    return true;
  };
  auto range_done = [&](uint32_t start) {
    for (uint32_t c = start; c < std::min(chunks, start + kRangeChunks); ++c)
      if (!has(c)) return false;
    return true;
  };
  while (left && ok) {
    if (g_cancel) { ok = false; break; }
    // (fill the window)
    while (int(pending.size()) < window && next < chunks) {
      if (!range_done(next) && !ask(next)) {
        if (!P2PPeerDirect(g_host)) {
          *error = "The link to the host is no longer direct: mods never come through the relay.";
          ok = false;
        }
        break;
      }
      next += kRangeChunks;
    }
    if (!ok) break;
    std::deque<Incoming> got;
    {
      std::unique_lock lock(g_dl_mutex);
      g_dl_cv.wait_for(lock, std::chrono::milliseconds(20), [] { return !g_inbox.empty(); });
      got.swap(g_inbox);
    }
    const auto now = Clock::now();
    for (Incoming& m : got) {
      In in{m.data.data(), m.data.size()};
      if (m.op == kError) {
        in.u8();
        *error = in.rest();
        ok = false;
        break;
      }
      if (m.op != kData) continue;
      const uint32_t g = in.u32();
      const uint16_t md = in.u16(), fi = in.u16();
      const uint32_t c = in.u32();
      if (!in.ok || g != gen || md != mod || fi != index || c >= chunks || has(c)) continue;
      const uint32_t want = uint32_t(std::min<uint64_t>(kChunk, f.size - uint64_t(c) * kChunk));
      if (in.n - in.at != want) continue;
      if (want && (std::fseek(out, long(uint64_t(c) * kChunk), SEEK_SET) != 0 ||
                   std::fwrite(in.p + in.at, 1, want, out) != want)) {
        *error = "Couldn't write the mod's files (disk full?).";
        ok = false;
        break;
      }
      have[c >> 3] |= uint8_t(1 << (c & 7));
      --left;
      rate_bytes += want;
      last_progress = now;
      {
        std::lock_guard lock(g_dl_mutex);
        g_state.done += want;
      }
      const uint32_t start = c / kRangeChunks * kRangeChunks;
      if (auto it = pending.find(start); it != pending.end() && range_done(start)) {
        rtt_ms = rtt_ms * 0.8 + 0.2 * std::chrono::duration<double, std::milli>(now - it->second).count();
        pending.erase(it);
        window = std::min(kWindowMax, window + 1);
      }
    }
    if (!ok) break;
    // (late ranges: asked again, the window halved - at most once a round trip)
    const auto rto = std::chrono::milliseconds(int(std::max(400.0, rtt_ms * 4)));
    bool late = false;
    for (auto& [start, at] : pending) {
      if (now - at < rto) continue;
      late = true;
      if (range_done(start)) continue;
      ask(start);
    }
    for (auto it = pending.begin(); it != pending.end();) it = range_done(it->first) ? pending.erase(it) : std::next(it);
    if (late && now - last_cut > rto) window = std::max(kWindowMin, window / 2), last_cut = now;
    if (now - last_progress > std::chrono::seconds(20)) {
      *error = "The host stopped sending (20 s without data).";
      ok = false;
    }
    if (now - rate_at > std::chrono::milliseconds(500)) {
      const double secs = std::chrono::duration<double>(now - rate_at).count();
      std::lock_guard lock(g_dl_mutex);
      g_state.rate = g_state.rate ? g_state.rate * 0.7 + 0.3 * rate_bytes / secs : rate_bytes / secs;
      rate_bytes = 0, rate_at = now;
    }
    if (now - last_map > std::chrono::seconds(2)) {  // (resume point)
      std::fflush(out);
      std::ofstream(map_path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(have.data()),
                                                                       std::streamsize(have.size()));
      last_map = now;
    }
  }
  std::fclose(out);
  if (!ok) {
    std::ofstream(map_path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(have.data()),
                                                                     std::streamsize(have.size()));
    return false;
  }
  uint32_t crc = 0;
  if (!FileCrc32(part, &crc) || crc != f.crc) {
    fs::remove(part, ec), fs::remove(map_path, ec);
    *error = "A file of the mod didn't arrive right (" + f.rel + ": its CRC differs).";
    return false;
  }
  fs::remove(dest, ec);
  fs::rename(part, dest, ec);
  fs::remove(map_path, ec);
  if (ec) {
    *error = "Couldn't put the mod's files in place.";
    return false;
  }
  return true;
}

void Run(std::vector<ModOffer> wanted) {
  rex::thread::set_current_thread_name("mod transfer");
  std::string error;
  uint32_t gen = 0;
  const auto t0 = Clock::now();
  auto fail = [&](const std::string& why) {
    REXLOG_WARN("mod transfer: download failed: {}", why);
    SetState(ModDownloadState::kFailed, why);
    g_running = false;
  };
  SetState(ModDownloadState::kAsking);
  const std::vector<ModOffer> set = FetchSet(&gen, &error);
  if (set.empty()) return fail(error.empty() ? "The host didn't answer." : error);
  // (each wanted mod by its place in the host's set, the sizes as the host says)
  std::vector<std::pair<uint16_t, ModOffer>> plan;
  uint64_t total = 0;
  const uint64_t max_mod = uint64_t(std::max(1, REXCVAR_GET(online_mod_max_mb))) << 20;
  // (1024 was 2.1.6's default, which every launcher wrote into the settings: read as the new default)
  const int mods_mb = REXCVAR_GET(online_mods_max_mb) == 1024 ? 4096 : REXCVAR_GET(online_mods_max_mb);
  const uint64_t max_all = uint64_t(std::max(1, mods_mb)) << 20;
  for (const ModOffer& w : wanted) {
    auto it = std::find_if(set.begin(), set.end(), [&](const ModOffer& s) {
      return s.kind == w.kind && s.id == w.id && s.crc == w.crc;
    });
    if (it == set.end()) return fail("The host no longer has " + w.name + ".");
    if (!SafeName(it->kind) || !SafeName(it->id) || !SafeName(it->crc)) return fail("A mod's name can't be a folder name.");
    if (it->bytes > max_mod)
      return fail(it->name + " is " + std::to_string(it->bytes >> 20) + " MB: more than the " +
                  std::to_string(max_mod >> 20) + " MB a mod may be (online_mod_max_mb).");
    total += it->bytes;
    plan.emplace_back(uint16_t(it - set.begin()), *it);
  }
  if (total > max_all)
    return fail("These mods are " + std::to_string(total >> 20) + " MB: more than the " + std::to_string(max_all >> 20) +
                " MB fetched for a session (online_mods_max_mb).");
  std::error_code ec;
  if (Root().empty() || !Root().is_absolute()) return fail("No user data folder to put the mods in.");
  fs::create_directories(Root(), ec);
  const auto space = fs::space(Root(), ec);
  if (!ec && space.available < total + (64ull << 20)) return fail("There isn't enough free disk space for these mods.");
  {
    std::lock_guard lock(g_dl_mutex);
    g_state.total = total, g_state.done = 0, g_state.mods = int(plan.size()), g_state.mod = 0;
  }
  SetState(ModDownloadState::kFetching);
  REXLOG_INFO("mod transfer: fetching {} mod(s), {:.1f} MB, from the host", plan.size(), total / 1048576.0);
  for (size_t k = 0; k < plan.size(); ++k) {
    const auto& [index, m] = plan[k];
    {
      std::lock_guard lock(g_dl_mutex);
      g_state.mod = int(k + 1);
    }
    // (its file list, every page; every path checked here again)
    std::vector<Listed> files;
    uint16_t pages = 1;
    for (uint16_t page = 0; page < pages; ++page) {
      std::optional<Incoming> got;
      for (int tries = 0; tries < 15 && !got && !g_cancel; ++tries) {  // (the host lists a big mod first)
        Out o;
        o.u8(kListReq), o.u32(gen), o.u16(index), o.u16(page);
        if (!Send(o)) {  // (a link still settling: a moment, again - through the relay for good: the reason)
          if (tries >= 10) return fail("There's no direct link to the host: mods never come through the relay.");
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
        error.clear();
        got = Await(kList, 2000, &error);
        if (!got && !error.empty()) return fail(error);
      }
      if (!got) return fail(g_cancel ? "Cancelled." : "The host didn't send " + m.name + "'s file list.");
      In in{got->data.data(), got->data.size()};
      if (in.u32() != gen || in.u16() != index || in.u16() != page) return fail("The host's answer didn't match.");
      pages = in.u16();
      while (in.ok && in.at < in.n) {
        const uint16_t len = in.u16();
        Listed f{in.str(len), 0, 0};
        f.size = in.u32(), f.crc = in.u32();
        if (in.ok) files.push_back(std::move(f));
      }
    }
    uint64_t sum = 0;
    for (const Listed& f : files) {
      std::string why;
      if (!ModFileAllowed(f.rel, &why)) return fail(m.name + ": the host sent a file that isn't allowed (" + f.rel + ": " + why + ").");
      sum += f.size;
    }
    if (sum != m.bytes || files.size() != m.files) return fail(m.name + ": the host's file list doesn't add up.");
    const fs::path dir = ModDir(m);
    const fs::path canon = fs::weakly_canonical(dir, ec);
    for (uint16_t i = 0; i < files.size(); ++i) {
      const fs::path dest = dir / fs::u8path(files[i].rel);
      // (and the place it lands is inside the mod's folder, whatever the path says)
      const std::string d = fs::weakly_canonical(dest, ec).string(), c = canon.string();
      if (d.size() <= c.size() || d.compare(0, c.size(), c) != 0) return fail(m.name + ": a file would land outside its folder.");
      error.clear();
      if (!FetchFile(gen, index, i, files[i], dest, &error)) return fail(g_cancel ? "Cancelled." : error);
    }
    // (the whole mod against the host's key: its CRC as the mod set has it)
    char hex[16];
    std::snprintf(hex, sizeof hex, "%08X", ModFolderCrc(dir));
    if (m.crc != hex) {
      fs::remove_all(dir, ec);
      return fail(m.name + " arrived but doesn't match the host's copy (" + hex + " vs " + m.crc + ").");
    }
    REXLOG_INFO("mod transfer: {} {} ({} files, {:.1f} MB) fetched and checked", m.kind, m.id, files.size(),
                m.bytes / 1048576.0);
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  REXLOG_INFO("mod transfer: done - {:.1f} MB in {:.1f} s ({:.2f} MB/s)", total / 1048576.0, secs,
              secs > 0 ? total / 1048576.0 / secs : 0.0);
  SetState(ModDownloadState::kDone);
  g_running = false;
}

std::mutex g_offer_mutex;
std::optional<ModLobbyOffer> g_offer;  // (the newest, for the overlay)
std::vector<ModOffer> g_last_wanted;   // (the last download's mods: the test aid's resume)

// The lobby watch: once per session this game joined (3 s in: the direct link and the lobby's mod tables are up),
// the host's gameplay mods this game lacks -> an offer for the overlay; a download goes when the session does.
// Test aids: SVR2011_TEST_MOD_DOWNLOAD=1 - progress in the log every 2 s (=all: every one of the host's mods is
// offered); SVR2011_TEST_MOD_ANSWER=yes|no answers the offer itself; SVR2011_TEST_XFER_STOP_MB=<n>: the first
// download is cancelled after n MB and started again 3 s later (it must resume from its .part files).
void LobbyLoop() {
  const char* mode = std::getenv("SVR2011_TEST_MOD_DOWNLOAD");
  const bool test = mode != nullptr, all = mode && std::string(mode) == "all";
  const char* stop_env = std::getenv("SVR2011_TEST_XFER_STOP_MB");
  uint64_t stop_at = stop_env ? uint64_t(std::atoi(stop_env)) << 20 : 0;
  uint64_t tried = 0, seen = 0;
  int64_t seen_at = 0, last_log = 0, retry_at = 0;
  int asks = 0;
  for (;; std::this_thread::sleep_for(std::chrono::milliseconds(500))) {
    const ModDownloadState s = ModDownload();
    if (test && g_running && NowMs() - last_log > 2000) {
      last_log = NowMs();
      REXLOG_INFO("mod transfer: mod {}/{}, {:.1f} of {:.1f} MB, {:.2f} MB/s", s.mod, s.mods, s.done / 1048576.0,
                  s.total / 1048576.0, s.rate / 1048576.0);
    }
    if (stop_at && g_running && s.done >= stop_at) {
      REXLOG_INFO("mod transfer: test: cancelled at {:.1f} MB - resuming in 3 s", s.done / 1048576.0);
      stop_at = 0;
      CancelModDownload();
      while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::this_thread::sleep_for(std::chrono::seconds(3));
      StartModDownload(g_last_wanted);
      continue;
    }
    uint64_t session = 0;
    const uint64_t host = P2PSessionHost(&session);
    if (!host && g_running && s.phase == ModDownloadState::kFetching) {  // (the session went: so does its download)
      REXLOG_INFO("mod transfer: the session ended - the download stops");
      CancelModDownload();
    }
    if (!host || session == tried || g_running) continue;
    if (session != seen) seen = session, seen_at = NowMs(), asks = 0, retry_at = 0;
    if (NowMs() - seen_at < 3000 || NowMs() < retry_at) continue;
    tried = session;
    ModLobbyOffer offer;
    offer.session = session;
    if (!P2PPeerDirect(host)) {
      offer.lacking = OnlineModsLackedFromHost();
      REXLOG_INFO("mod transfer: the host's link goes through the relay - no mods can come ({} lacked)",
                  offer.lacking.size());
      if (offer.lacking.empty()) continue;
      offer.relayed = true;
    } else {
      g_host = host;
      g_running = true;  // (AskHostMods takes the host's answers only while running)
      std::string why;
      const auto offers = AskHostMods(4000, &why);
      g_running = false;
      const bool settling = why == "The host didn't answer." || why.find("direct link") != std::string::npos;
      if (offers.empty() && settling) {  // (the link still settling: again in 5 s, for a minute)
        if (++asks < 12) tried = 0, retry_at = NowMs() + 5000;
        REXLOG_INFO("mod transfer: the host's mods: {}{}", why, asks < 12 ? " - asking again in 5 s" : " - given up");
        continue;
      }
      if (offers.empty() && !why.empty()) {  // (the host won't send: its reason, if this game lacks anything)
        REXLOG_INFO("mod transfer: the host's mods: {}", why);
        for (const std::string& l : OnlineModsLackedFromHost()) offer.unsendable.push_back(l + ": " + why);
        if (offer.unsendable.empty()) continue;
      }
      std::map<std::string, std::pair<std::string, std::string>> mine;  // (kind, id -> version, CRC)
      for (const std::string& line : Split(GameplayModSet(), '\n')) {
        const auto f = Split(line, '\t');
        if (f.size() >= 4) mine[f[0] + "\t" + f[1]] = {f[2], f[3]};
      }
      for (const ModOffer& o : offers) {
        const auto have = mine.find(o.kind + "\t" + o.id);
        if (!all && have != mine.end() && have->second.second == o.crc) continue;
        if (!all && have != mine.end()) {
          const std::string name = o.name.empty() ? o.id : o.name;
          offer.differ.push_back(have->second.first == o.version || o.version == "-" || have->second.first == "-"
                                     ? name + " (your copy isn't the same as the host's)"
                                     : name + ": you have " + have->second.first + ", the host has " + o.version);
          REXLOG_INFO("mod transfer: {} {}: this game has {} ({}), the host {} ({})", o.kind, o.id, have->second.first,
                      have->second.second, o.version, o.crc);
          continue;
        }
        if (!o.unsendable.empty()) offer.unsendable.push_back((o.name.empty() ? o.id : o.name) + ": " + o.unsendable);
        else offer.mods.push_back(o), offer.bytes += o.bytes;
      }
      REXLOG_INFO("mod transfer: the host has {} mod(s), {} this game lacks ({:.1f} MB), {} it can't send, {} it has "
                  "another copy of", offers.size(), offer.mods.size(), offer.bytes / 1048576.0, offer.unsendable.size(),
                  offer.differ.size());
      if (offer.mods.empty() && offer.unsendable.empty() && offer.differ.empty()) continue;
    }
    if (const char* answer = std::getenv("SVR2011_TEST_MOD_ANSWER")) {
      REXLOG_INFO("mod transfer: test: the offer answered {}", answer);
      if (std::string(answer) == "yes" && !offer.relayed) StartModDownload(offer.mods);
      continue;
    }
    std::lock_guard lock(g_offer_mutex);
    g_offer = std::move(offer);
  }
}

}  // namespace

std::vector<ModOffer> AskHostMods(int timeout_ms, std::string* why) {
  if (!g_host) g_host = P2PSessionHost();
  if (!g_host) return {};
  uint32_t gen = 0;
  std::string error;
  const bool was = g_running.exchange(true);
  auto set = FetchSet(&gen, &error);
  if (!was) g_running = false;
  (void)timeout_ms;
  if (why) *why = set.empty() ? error : std::string();
  return set;
}

bool StartModDownload(const std::vector<ModOffer>& mods) {
  if (g_running.exchange(true)) return false;
  const uint64_t host = P2PSessionHost();
  if (!host || !P2PPeerDirect(host)) {
    g_running = false;
    SetState(ModDownloadState::kFailed, host ? "There's no direct link to the host: mods only come straight from "
                                               "the host, never through the relay."
                                             : "Not in a session.");
    return false;
  }
  g_host = host;
  g_cancel = false;
  {
    std::lock_guard lock(g_dl_mutex);
    g_inbox.clear();
    g_state = {};
  }
  if (g_thread.joinable()) g_thread.join();
  g_last_wanted = mods;
  g_thread = std::thread(Run, mods);
  return true;
}

void CancelModDownload() { g_cancel = true; }

ModDownloadState ModDownload() {
  std::lock_guard lock(g_dl_mutex);
  return g_state;
}

fs::path FetchedModFolder(const ModOffer& mod) {
  const fs::path dir = ModDir(mod);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return {};
  for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
    const std::string x = e.path().extension().string();
    if (x == ".part" || x == ".partmap") return {};
  }
  return dir;
}

bool OnlineModsLoad() { return REXCVAR_GET(online_mods_load) && !Root().empty() && Root().is_absolute(); }
fs::path OnlineModsRoot() { return Root(); }

void ClearFetchedMods() {
  if (g_running || Root().empty() || !Root().is_absolute()) return;
  if (OnlineModsLoad()) return;  // (this run plays with them: they go at its end / the next start)
  std::error_code ec;
  if (fs::exists(Root(), ec)) {
    const auto n = fs::remove_all(Root(), ec);
    REXLOG_INFO("mod transfer: OnlineMods cleared ({} entries){}", n, ec ? " - some in use, next start" : "");
  }
}

void InstallModTransfer() {
  // (what an earlier run fetched - it crashed, or ended in a session - goes now; test: kept for the resume test)
  if (!std::getenv("SVR2011_TEST_XFER_KEEP")) ClearFetchedMods();
  SetP2PModXferHandler(OnMessage);
  std::thread(SenderLoop).detach();
  std::thread(LobbyLoop).detach();
}

bool TakeModLobbyOffer(ModLobbyOffer* out) {
  std::lock_guard lock(g_offer_mutex);
  if (!g_offer) return false;
  *out = std::move(*g_offer);
  g_offer.reset();
  return true;
}

void AnswerModLobbyOffer(const ModLobbyOffer& offer, bool download) {
  REXLOG_INFO("mod transfer: the player answered {} to {} mod(s), {:.1f} MB", download ? "YES" : "NO",
              offer.mods.size(), offer.bytes / 1048576.0);
  if (download && !offer.relayed && P2PSessionHost() != 0) StartModDownload(offer.mods);
}

}  // namespace svr2011
