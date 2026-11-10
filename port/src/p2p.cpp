// WWE SmackDown vs. Raw 2011 - peer-to-peer online matches (see p2p.h).
//
// The session message decoding (XGI B0006-B001C buffers, search results in
// the caller's memory) follows PR #3, "Add groundwork for P2P online
// sessions", by gitSothib - its session directory is a protocol between the
// games here instead of a web service.
#include "p2p.h"
#include "leaderboards.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/online_hooks.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "arena_mods.h"
#include "cpu_difficulty.h"
#include "frame_rate.h"
#include "online_files.h"
#include "online_net.h"
#include "port_map.h"
#include "net_sim.h"
#include "net_stats.h"
#include "online_overlay.h"
#include "online_rules.h"
#include "match_types.h"
#include "script_input.h"

REXCVAR_DEFINE_BOOL(p2p_enabled, true, "Online", "Peer-to-peer online matches (with online_enabled)");
REXCVAR_DEFINE_UINT32(p2p_port, 36000, "Online",
                      "This game's port base for online matches (UDP; a second game on the same PC: 36100)");
REXCVAR_DEFINE_STRING(p2p_address, "", "Online",
                      "The LAN address this game gives peers (tests: a second game on this PC, e.g. 127.0.0.2)");
REXCVAR_DEFINE_STRING(p2p_peers, "", "Online",
                      "Addresses searched besides the LAN, comma-separated (host or host:port)");
REXCVAR_DEFINE_BOOL(p2p_relay, true, "Online",
                    "Matches through the online server's relay when players can't connect directly");
REXCVAR_DEFINE_BOOL(p2p_ipv6, true, "Online",
                    "Also connect over IPv6 where both players have it: no router in the way, only its firewall (off: IPv4 only)");
REXCVAR_DEFINE_STRING(p2p_stun, "stun.l.google.com:19302", "Online",
                      "The STUN server that tells this game its public address (off: none)");
REXCVAR_DEFINE_BOOL(p2p_force_relay, false, "Online", "Tests: every peer through the relay, never directly");
REXCVAR_DEFINE_BOOL(p2p_match_report, true, "Online",
                    "After each online match, tell the server how it connected (direct / relay, the router's kind, "
                    "the ping) - no addresses or names; the server keeps counts per day");
REXCVAR_DEFINE_BOOL(p2p_lockstep, true, "Online",
                    "Matches step the world once a frame on both games (as on the console); off: tests only");
REXCVAR_DEFINE_BOOL(p2p_send_every_frame, true, "Online",
                    "Online matches send each frame's input at once and keep the input delay to the measured ping "
                    "(lower lag on long links); off: the game's own send interval and delay");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kBasePort = 36000;            // (port bases are kBasePort + n * 100)
constexpr int kInstances = 4;                    // (searched on the LAN and this PC)
constexpr uint32_t kFakeNet = 0x0A400000;        // 10.64.0.0/16: peers as the game sees them
constexpr uint32_t kMagic = 0x53565250;          // "SVRP"
constexpr uint32_t kContextGameType = 0x0000800A;  // X_CONTEXT_GAME_TYPE (ranked / standard)
constexpr uint32_t kContextGameMode = 0x0000800B;  // X_CONTEXT_GAME_MODE
// The port's session properties (0x1000F00x, XUSER type 1 ints the host's session carries to joiners):
// 0x1000F001 PORT RULES (here), 0x1000F002 the WEAPONS page's set (weapons_page.cpp), 0x1000F003 mod weapons
// (Arena Porting, planned), 0x1000F004 the match network (here), 0x1000F006 the CPU tab's settings (cpu_difficulty.cpp), 0x1000F005 the netcode (here; the Online Rollback
// Netcode chat's modes read P2PNetcode).
constexpr uint32_t kPortRules = 0x1000F001;        // the port's match rules (match_types: mode << 16 | set)
constexpr uint32_t kNetMode = 0x1000F004;          // the host's match network (NetMode)
constexpr uint32_t kNetcode = 0x1000F005;          // the host's netcode (0 lockstep, 1 rollback, 2 lockstep_vclock)
// The game's own "downloadable content" property: the join check (online.cpp, sub_824CDE90) refuses a session
// whose value isn't 113 - a constant of the game's session settings, the same in every copy of the port.
constexpr uint32_t kDlcProperty = 0x1000000B, kDlcValue = 113;
constexpr uint8_t kVersion = 1;
enum : uint8_t {
  kSearch = 1, kSessions = 2, kQosRequest = 3, kQosReply = 4, kHello = 5, kData = 6,
  kFileRequest = 7, kFilePart = 8,  // (files peers fetch from each other: P2PFetch)
  kModsRefused = 9,                 // (a searcher's mods differ: its set, for the host's notice)
  kMods = 14,                       // (a lobby's superstar mods: online_mods.cpp)
  kChat = 15,                       // (lobby chat, whole: online_chat.cpp)
  kModXfer = 16,                    // (mod files, one peer to another, direct links only: mod_transfer.cpp)
  kCandidates = 17                  // (this game's addresses, to a player it's connecting to: Probe)
};
// Round trips to the peers a match exchanges game packets with (net_sim.h);
// game packets with the last few repeated (a peer that pings takes them).
enum : uint8_t { kPing = 11, kPong = 12, kDataV2 = 13 };

rex::memory::Memory* g_memory = nullptr;

// -- byte helpers --------------------------------------------------------------------

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v)); }

uint8_t* Guest(uint32_t address) { return g_memory->TranslateVirtual<uint8_t*>(address); }

// An address for the log (players post their logs): the game's own peer
// addresses (10.64.0.0/16) and this PC (127.x) in full, a LAN address as
// its network only (192.168.x.x), an internet address not at all.
std::string Ip(uint32_t ip) {
  if ((ip >> 24) == 0xF0) return "IPv6";  // (kV6Net)
  const auto part = [ip](int k) { return std::to_string((ip >> (24 - 8 * k)) & 255); };
  if ((ip & 0xFFFF0000) == kFakeNet || (ip >> 24) == 127) return part(0) + "." + part(1) + "." + part(2) + "." + part(3);
  const bool lan = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
  return lan ? part(0) + "." + part(1) + ".x.x" : "x.x.x.x";
}

// A message: appended fields, read back in order.
struct Writer {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }
  void u32(uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)), u32(uint32_t(v)); }
  void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool need(size_t k) { return ok = ok && at + k <= n; }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  uint16_t u16() { uint16_t v = need(2) ? Be16(p + at) : 0; at += ok ? 2 : 0; return v; }
  uint32_t u32() { uint32_t v = need(4) ? Be32(p + at) : 0; at += ok ? 4 : 0; return v; }
  uint64_t u64() { uint64_t v = need(8) ? Be64(p + at) : 0; at += ok ? 8 : 0; return v; }
  void bytes(uint8_t* out, size_t k) {
    if (need(k)) std::memcpy(out, p + at, k), at += k;
  }
};



// -- sockets -------------------------------------------------------------------------

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool Reset() { return WSAGetLastError() == WSAECONNRESET; }  // (an ICMP "port unreachable" from an earlier send)
#else
using Socket = int;
const Socket kBadSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool Reset() { return errno == ECONNREFUSED; }
#endif

Socket g_socket = kBadSocket;  // the P2P socket (this game's port base): everything but LAN game traffic

sockaddr_in Sin(uint32_t ip, uint16_t port) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(ip);
  to.sin_port = htons(port);
  return to;
}

// IPv6: a far end's address is known everywhere by a number in 240.0.0.0/8 (reserved in IPv4: never a real address),
// which this table turns back into the address when a packet goes out - peers, candidates and routes stay as they are.
constexpr uint32_t kV6Net = 0xF0000000;
using V6 = std::array<uint8_t, 16>;
std::mutex g_v6_mutex;
std::vector<V6> g_v6_table;
Socket g_socket6 = kBadSocket;
std::atomic<uint32_t> g_my_v6{0};  // (this game's address, as such a number; 0: none)
bool g_v6_test = false;            // (test aid: an address of SVR2011_TEST_V6's, any kind)
bool g_v6_real = false;            // (test aid: through the real socket, also behind the NAT box)
bool IsV6(uint32_t ip) { return (ip >> 24) == (kV6Net >> 24); }
uint32_t V6Number(const uint8_t* address) {
  V6 a;
  std::memcpy(a.data(), address, 16);
  std::lock_guard lock(g_v6_mutex);
  const auto it = std::find(g_v6_table.begin(), g_v6_table.end(), a);
  if (it != g_v6_table.end()) return kV6Net | uint32_t(it - g_v6_table.begin() + 1);
  if (g_v6_table.size() >= 4096) return 0;
  g_v6_table.push_back(a);
  return kV6Net | uint32_t(g_v6_table.size());
}
bool V6Of(uint32_t number, V6* out) {
  std::lock_guard lock(g_v6_mutex);
  const uint32_t k = number & 0xFFFFFF;
  if (!k || k > g_v6_table.size()) return false;
  *out = g_v6_table[k - 1];
  return true;
}
// One place on the internet: global unicast, not a tunnel's (Teredo, 6to4) nor the documentation's.
bool GlobalV6(const uint8_t* a) {
  if ((a[0] & 0xE0) != 0x20) return false;
  if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0 && a[3] == 0) return false;
  if (a[0] == 0x20 && a[1] == 0x02) return false;
  return !(a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0D && a[3] == 0xB8);
}
constexpr uint32_t kBoxV6 = 0xFFFFFFFE;  // (the NAT box: an IPv6 packet's wrapping starts so)

// Test aid: SVR2011_TEST_NAT_BOX=<ip:port> - this game behind a simulated router (tools/dev/nat_box.py): everything
// on the P2P socket goes to the box, wrapped with the real far end's address (6 bytes), and comes back the same way.
uint32_t g_box_ip = 0;
uint16_t g_box_port = 0;

void SendOn(Socket s, uint32_t ip, uint16_t port, const uint8_t* data, size_t size) {
  if (s == g_socket && IsV6(ip)) {
    V6 a;
    if (!V6Of(ip, &a)) return;
    if (g_box_ip && !g_v6_real) {
      std::vector<uint8_t> wrapped(22 + size);
      Put32(wrapped.data(), kBoxV6), Put16(wrapped.data() + 4, port);
      std::memcpy(wrapped.data() + 6, a.data(), 16);
      std::memcpy(wrapped.data() + 22, data, size);
      sockaddr_in box = Sin(g_box_ip, g_box_port);
      sendto(s, reinterpret_cast<const char*>(wrapped.data()), int(wrapped.size()), 0, reinterpret_cast<sockaddr*>(&box), sizeof(box));
      return;
    }
    if (g_socket6 == kBadSocket) return;
    sockaddr_in6 to = {};
    to.sin6_family = AF_INET6;
    to.sin6_port = htons(port);
    std::memcpy(&to.sin6_addr, a.data(), 16);
    sendto(g_socket6, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    return;
  }
  if (g_box_ip && s == g_socket) {
    std::vector<uint8_t> wrapped(6 + size);
    Put32(wrapped.data(), ip), Put16(wrapped.data() + 4, port);
    std::memcpy(wrapped.data() + 6, data, size);
    sockaddr_in box = Sin(g_box_ip, g_box_port);
    sendto(s, reinterpret_cast<const char*>(wrapped.data()), int(wrapped.size()), 0, reinterpret_cast<sockaddr*>(&box), sizeof(box));
    return;
  }
  sockaddr_in to = Sin(ip, port);
  sendto(s, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}
void SendTo(uint32_t ip, uint16_t port, const std::vector<uint8_t>& data) {
  SendOn(g_socket, ip, port, data.data(), data.size());
}

// host or host:port -> address (0: unknown)
std::pair<uint32_t, uint16_t> Resolve(std::string item, uint16_t port) {
  item.erase(0, item.find_first_not_of(" \t"));
  item.erase(item.find_last_not_of(" \t") + 1);
  if (const size_t colon = item.rfind(':'); colon != std::string::npos) {
    port = uint16_t(std::atoi(item.c_str() + colon + 1));
    item.resize(colon);
  }
  if (item.empty()) return {0, 0};
  addrinfo hints = {}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  uint32_t ip = 0;
  if (getaddrinfo(item.c_str(), nullptr, &hints, &found) == 0 && found) {
    ip = ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
  }
  return {ip, port};
}

// This PC's address on its network (the interface the default route uses).
uint32_t LanIp() {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadSocket) return 0x7F000001;
  sockaddr_in to = Sin(0x08080808, 53);  // (no packet is sent)
  uint32_t ip = 0x7F000001;
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in me = {};
    socklen_t len = sizeof(me);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr) ip = ntohl(me.sin_addr.s_addr);
  }
  CloseSocket(s);
  return ip;
}

// -- this game's address and its peers ----------------------------------------------

using Mac = std::array<uint8_t, 6>;
using XnAddr = std::array<uint8_t, 36>;  // in_addr ina, inaOnline, be16 port, abEnet[6], abOnline[20]

struct Me {
  uint32_t lan = 0x7F000001;
  std::atomic<uint32_t> online{0};       // the public address (STUN, or as the relay sees it)
  std::atomic<uint16_t> online_port{0};  // (the P2P socket's, outside the NAT)
  uint16_t base = kBasePort;
  Mac mac{};
  uint64_t xuid = 0;
} g_me;
std::atomic<uint16_t> g_mapped_port{0};  // (the router forwards it: port_map.h; STUN's port doesn't replace it)
std::atomic<uint32_t> g_mapped_ip{0};    // (the outside address that router claims; 0: it didn't say)
std::atomic<uint32_t> g_stun_ip{0};      // (the outside address STUN saw)
std::atomic<uint32_t> g_edge_ip{0};      // (the one the relay over UDP sees)
// The P2P socket's port outside the NAT as two far ends see it: STUN's (Google)
// and the UDP relay's (the VPS). Different: a symmetric NAT - a new port per
// destination, so a peer's punch to the port the relay saw misses.
std::atomic<uint16_t> g_stun_port{0}, g_edge_port{0};
std::atomic<int64_t> g_stun_ms{0}, g_edge_ms{0};  // (when each came, NowMs: a port seen long ago proves nothing)

// A router's mapping is the way in only if that router is the one on the internet: behind another router or a
// carrier's NAT it claims a private outside address (or one STUN doesn't see), and the port it gave means nothing
// out there. When it doesn't say, the port STUN saw tells.
bool PublicIp(uint32_t ip) {
  const uint8_t a = uint8_t(ip >> 24), b = uint8_t(ip >> 16);
  return !(a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128) ||
           (a == 169 && b == 254));
}
bool MappedUsable() {
  const uint16_t port = g_mapped_port;
  if (!port) return false;
  const uint32_t claimed = g_mapped_ip, seen = g_stun_ip;
  if (claimed) return PublicIp(claimed) && (!seen || seen == claimed);
  const uint16_t stun = g_stun_port;
  return !stun || stun == port;
}

// The match report (p2p_match_report): this match's paths, a second at a time.
struct MatchReport {
  uint32_t seconds = 0, direct = 0, relay = 0, lan = 0;  // (peer-seconds per path)
  uint32_t ipv6 = 0;                                     // (of direct's: over IPv6)
  double rtt_sum = 0;
  uint32_t rtt_n = 0;
  uint8_t network = 0;  // (the match network it played on: NetMode)
} g_report;

// A game port's real port: + 20000 (the game's own, UDP 1001, is below 1024,
// which Android and Linux don't let apps bind) + the port base's offset (two
// games on one PC differ).
constexpr uint16_t kPortShift = 20000;
uint16_t Offset(uint16_t base) { return uint16_t(base - kBasePort + kPortShift); }

// abOnline: the player's xuid, then the P2P socket's public port.
XnAddr MyXnAddr() {
  XnAddr a{};
  const uint32_t online = g_me.online;
  Put32(a.data(), g_me.lan);
  Put32(a.data() + 4, online ? online : g_me.lan);
  Put16(a.data() + 8, g_me.base);
  std::memcpy(a.data() + 10, g_me.mac.data(), 6);
  Put64(a.data() + 16, g_me.xuid);
  Put16(a.data() + 24, g_me.online_port);
  return a;
}

// The online server's match relay (port/server/relay.py): two long requests
// (a pipe), so it works wherever the server's https does.
std::mutex g_pipe_mutex;
std::shared_ptr<net::Pipe> g_pipe;
std::atomic<bool> g_relay_up{false};  // (it answered)
std::atomic<int64_t> g_online_at{0};  // last session activity (steady clock, ms): the relay is wanted

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}
// The match network (the server's match_network setting, P2PSetServerNetwork): auto - direct, the
// relay when direct fails; relay - every peer through the relay; p2p - match traffic direct only (the
// relay still finds sessions and carries hellos: a server that struggles only passes those). Latched
// per match session - the host's mode travels in kNetMode and joiners follow it - so a running match
// never switches; outside a match, auto.
enum NetMode : uint8_t { kNetAuto = 0, kNetRelay = 1, kNetP2P = 2 };
std::atomic<uint8_t> g_server_net{kNetAuto};  // (the server's answer; auto when it can't be reached)
std::atomic<uint8_t> g_match_net{kNetAuto};   // (this match session's)
const char* NetName(uint8_t m) { return m == kNetRelay ? "relay" : m == kNetP2P ? "p2p" : "auto"; }
bool ForceRelay() { return REXCVAR_GET(p2p_force_relay) || g_match_net == kNetRelay; }
bool NoRelayData() { return g_match_net == kNetP2P && !REXCVAR_GET(p2p_force_relay); }  // (p2p)
// The netcode (the server's netcode setting, P2PSetServerNetcode): latched the same way - the host's in kNetcode,
// joiners follow it, a host without it (an older game) or a value this game doesn't know: lockstep; outside a match
// session, lockstep.
constexpr uint8_t kNetcodeMax = 2;
std::atomic<uint8_t> g_server_netcode{0};  // (the server's answer; lockstep when it can't be reached)
std::atomic<uint8_t> g_match_netcode{0};   // (this match session's)
const char* NetcodeName(uint8_t m) { return m == 1 ? "rollback" : m == 2 ? "lockstep_vclock" : "lockstep"; }

// The addresses a player may be reached at, and which of them answered: every one is tried at once (Probe), and the
// fastest that answers carries the game (Nominate).
enum CandidateKind : uint8_t { kCandLan = 1, kCandMapped = 2, kCandStun = 3, kCandEdge = 4, kCandHeard = 5, kCandBase = 6, kCandV6 = 7 };
const char* CandidateName(uint8_t k) {
  return k == kCandLan ? "LAN" : k == kCandMapped ? "router mapping" : k == kCandStun ? "STUN" : k == kCandEdge ? "relay's view"
       : k == kCandHeard ? "heard from" : k == kCandV6 ? "IPv6" : "port base";
}
struct Candidate {
  uint32_t ip = 0;
  uint16_t port = 0;
  uint8_t kind = 0;
  Clock::time_point ok{};  // (a ping to it came back from it)
  float rtt = 0;           // (ms, smoothed)
};

struct Peer {
  XnAddr xnaddr{};
  uint32_t fake = 0;        // what the game knows it as
  uint32_t route = 0;       // where its packets go: the address it was last heard from, or its own
  uint16_t route_port = 0;  // (and port: outside a NAT, not its port base)
  uint16_t base = kBasePort;
  bool relayed = false;     // its packets go through the relay
  bool tunnel = false;      // game traffic goes through the P2P socket (not on this network)
  Clock::time_point direct{};          // last heard from directly
  Clock::time_point punched{};         // last direct try while relayed
  uint32_t seen_ip = 0;                // its address as the relay sees it
  uint16_t seen_port = 0;
  bool data_v2 = false;                // it pings: it takes kDataV2
  Clock::time_point direct_ok{};       // (v2) a direct ping last came back directly
  Clock::time_point direct_pong{};     // (v2) the same, without the "fresh start": direct is proven only by this
  std::vector<Candidate> candidates;   // its addresses: below
  Clock::time_point probing_from{};    // when both started trying them (none: not connecting to it)
  Clock::time_point probed{};          // the last round of tries
  Clock::time_point candidates_sent{}; // this game's list last sent to it
  uint32_t logged_ip = 0;              // (the direct path last logged)
  uint16_t logged_port = 0;
  float rtt_direct = 0, rtt_relay = 0;  // (v2) each path's round trip, smoothed (ms; 0 not measured)
  Clock::time_point relay_ok{};        // (v2) a relayed ping last came back
  std::map<std::pair<uint16_t, uint16_t>, uint32_t> delivered;  // (v2: last sequence passed on, by to / from port)
  Clock::time_point game_traffic{};    // game packets last passed either way (a session together)
  Clock::time_point pinged{};          // last pinged (the ping loop skips peers of earlier sessions)
};

Candidate* FindCandidate(Peer& p, uint32_t ip, uint16_t port) {
  for (Candidate& c : p.candidates)
    if (c.ip == ip && c.port == port) return &c;
  return nullptr;
}
void AddCandidate(Peer& p, uint32_t ip, uint16_t port, uint8_t kind) {
  if (!ip || !port) return;
  if (Candidate* known = FindCandidate(p, ip, port)) {
    if (known->kind == kCandHeard) known->kind = kind;  // (now it's known what that address is)
    return;
  }
  if (p.candidates.size() >= 16) {  // (an address it had long ago makes way)
    const auto dead = std::find_if(p.candidates.begin(), p.candidates.end(), [](const Candidate& c) { return c.ok == Clock::time_point{}; });
    if (dead == p.candidates.end()) return;
    p.candidates.erase(dead);
  }
  p.candidates.push_back({ip, port, kind});
}
// The path that carries the game: the fastest address answering now - the LAN's first on anything like a tie (the same
// house: not out to the router and back). The one in use stays unless another is clearly faster (10 ms) or it stopped
// answering. Caller holds g_mutex.
bool RouteProven(Peer& p, Clock::time_point now) {
  const Candidate* c = FindCandidate(p, p.route, p.route_port);
  return c && now - c->ok < std::chrono::seconds(3);
}
void Nominate(Peer& p, Clock::time_point now) {
  const auto cost = [](const Candidate& c) { return c.rtt - (c.kind == kCandLan ? 20.0f : 0.0f); };
  const Candidate* best = nullptr;
  for (const Candidate& c : p.candidates)
    if (now - c.ok < std::chrono::seconds(3) && (!best || cost(c) < cost(*best))) best = &c;
  if (!best) return;
  if (!RouteProven(p, now) || cost(*best) + 10 < cost(*FindCandidate(p, p.route, p.route_port)))
    p.route = best->ip, p.route_port = best->port;
  if (p.route != p.logged_ip || p.route_port != p.logged_port) {
    p.logged_ip = p.route, p.logged_port = p.route_port;
    const Candidate* c = FindCandidate(p, p.route, p.route_port);
    REXLOG_INFO("p2p: peer {} direct path: {} {}:{} ({:.0f} ms)", Ip(p.fake), CandidateName(c->kind), Ip(p.route), p.route_port, c->rtt);
  }
}

std::mutex g_mutex;
std::map<Mac, Peer> g_peers;
std::map<uint32_t, Mac> g_fake;  // fake address -> peer
std::set<uint16_t> g_bound;      // the game's own ports (as it knows them)

Mac MacOf(const uint8_t* xnaddr) {
  Mac m;
  std::memcpy(m.data(), xnaddr + 10, 6);
  return m;
}
// (a peer as the other modules know it: its MAC in a number)
uint64_t PeerId(const Mac& m) {
  uint64_t v = 0;
  for (uint8_t b : m) v = v << 8 | b;
  return v;
}

// The way to a peer not heard from yet: this PC, its LAN address (same network) or its public one.
uint32_t RouteTo(const uint8_t* xnaddr) {
  const uint32_t lan = Be32(xnaddr), online = Be32(xnaddr + 4);
  if (lan == g_me.lan) return lan;  // (a second game on this PC: the game binds its own address, not loopback)
  const uint32_t mine = g_me.online;
  if (!online || online == lan || online == (mine ? mine : g_me.lan)) return lan;
  return online;
}

// How a packet got here.
struct Via {
  uint32_t ip = 0;  // its sender as seen (relayed: as the relay saw it); 0: not heard from
  uint16_t port = 0;
  bool relayed = false;
};

// The peer at xnaddr (added if new), and how it was just heard from.
Peer& Learn(const uint8_t* xnaddr, Via via = {}) {
  const Mac mac = MacOf(xnaddr);
  auto [it, added] = g_peers.try_emplace(mac);
  Peer& p = it->second;
  std::memcpy(p.xnaddr.data(), xnaddr, 36);
  p.base = Be16(xnaddr + 8);
  if (added) {
    uint32_t fake = kFakeNet | (uint32_t(mac[4]) << 8 | mac[5]);
    while (g_fake.count(fake) || (fake & 0xFF) == 0 || (fake & 0xFF) == 255) fake = kFakeNet | ((fake + 1) & 0xFFFF);
    p.fake = fake;
    g_fake[fake] = mac;
    p.route = RouteTo(xnaddr), p.route_port = p.base;
    // (outside its router: the port it reported, not its base - most routers change it)
    if (const uint16_t port = Be16(xnaddr + 24); port && p.route == Be32(xnaddr + 4)) p.route_port = port;
    // A player not on this PC: its game packets go through the tunnel (this
    // game's P2P port, the one its router lets through or the relay reaches)
    // from the start, and through the relay until something comes directly.
    // Players the game itself introduces (a third joining a rumble: the others
    // only know its address from the host) were sent to its game port behind
    // its router, and dropped, until it happened to answer: joins above two
    // players timed out.
    if (Be32(xnaddr) != g_me.lan) {
      p.tunnel = true;
      if (g_relay_up && p.route != Be32(xnaddr) && !NoRelayData()) p.relayed = true;  // (another network)
    }
  }
  const auto now = Clock::now();
  // A peer that pings (this version): its path is set by PingLoop from round
  // trips that came back directly (both ways confirmed), not by which way the
  // last packet came - a direct path working one way only had a peer send its
  // game packets into it for 10 s and more, then flip back and forth.
  if (p.data_v2 && !ForceRelay()) {
    if (via.relayed) p.seen_ip = via.ip, p.seen_port = via.port;
    if (!via.relayed && via.ip) {
      const uint32_t from = via.ip == 0x7F000001 ? Be32(xnaddr) : via.ip;
      AddCandidate(p, from, via.port, kCandHeard);
      if (!RouteProven(p, now)) p.route = from, p.route_port = via.port;  // (else: Nominate; a NAT may move it: then this)
      p.direct = now;
      if (from != Be32(xnaddr) && from != g_me.lan && !p.tunnel) {
        p.tunnel = true;  // (through a NAT: its game ports are not reachable, only this one)
        REXLOG_INFO("p2p: peer {} is outside this network ({}:{})", Ip(p.fake), Ip(from), via.port);
      }
    }
    if (NetForceTunnel()) p.tunnel = true;
    return p;
  }
  if (via.relayed) {
    p.seen_ip = via.ip, p.seen_port = via.port;
    // (direct while that works: the relay only when nothing came directly lately)
    if (!p.relayed && !NoRelayData() && now - p.direct > std::chrono::seconds(10)) {
      p.relayed = p.tunnel = true;
      REXLOG_INFO("p2p: peer {} through the relay (seen at {}:{})", Ip(p.fake), Ip(via.ip), via.port);
    }
  } else if (via.ip) {
    // (heard from: its address as this PC sees it; loopback: this PC, by its own address)
    const uint32_t from = via.ip == 0x7F000001 ? Be32(xnaddr) : via.ip;
    p.route = from, p.route_port = via.port, p.direct = now;
    if (from != Be32(xnaddr) && from != g_me.lan && !p.tunnel) {
      p.tunnel = true;  // (through a NAT: its game ports are not reachable, only this one)
      REXLOG_INFO("p2p: peer {} is outside this network ({}:{})", Ip(p.fake), Ip(from), via.port);
    }
    if (p.relayed) {
      p.relayed = false;
      REXLOG_INFO("p2p: peer {} direct at {}:{}", Ip(p.fake), Ip(from), via.port);
    }
  }
  if (ForceRelay()) p.relayed = p.tunnel = true;
  if (NetForceTunnel()) p.tunnel = true;  // (tests: the delay applies on this PC too)
  if (added) {
    REXLOG_INFO("p2p: peer {:02X}{:02X}{:02X}{:02X}{:02X}{:02X} at {}:{}{} is {}", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5], Ip(p.route), p.route_port, p.relayed ? " (relay)" : "", Ip(p.fake));
  }
  return p;
}

// Where a peer's packets go (taken under g_mutex, used without it).
struct Path {
  Mac mac{};
  bool relayed = false;
  uint32_t ip = 0;
  uint16_t port = 0;
};
Path PathOf(const Mac& mac, const Peer& p) { return {mac, p.relayed, p.route, p.route_port ? p.route_port : p.base}; }

// "SVRR" relay packets (port/server/relay.py), each in a frame: a 16-bit length, then the packet.
constexpr uint32_t kRelayMagic = 0x53565252;
enum : uint8_t { kRegister = 1, kWelcome = 2, kForward = 3, kFrom = 4 };

void ToPipe(uint8_t type, const uint8_t* id, const uint8_t* data, size_t size) {
  std::shared_ptr<net::Pipe> pipe;
  {
    std::lock_guard lock(g_pipe_mutex);
    pipe = g_pipe;
  }
  if (!pipe || size > 60000) return;
  std::vector<uint8_t> out(2 + 12 + size);
  Put16(out.data(), uint16_t(12 + size));
  Put32(out.data() + 2, kRelayMagic), out[6] = 1, out[7] = type;
  std::memcpy(out.data() + 8, id, 6);
  if (size) std::memcpy(out.data() + 14, data, size);
  pipe->Send(out.data(), out.size());
}

// The relay over UDP (the server's --relay-udp-port): a ticket from https,
// registered from the P2P socket every 5 s; packets go that way while it
// answers (welcomed in the last 15 s), else through the https pipe. Over TCP
// one lost segment held back every later packet.
std::atomic<uint32_t> g_udp_relay_ip{0};
std::atomic<uint16_t> g_udp_relay_port{0};
std::atomic<int64_t> g_udp_welcome_ms{0};

bool UdpRelayUp() { return g_udp_relay_ip && NowMs() - g_udp_welcome_ms.load() < 15000; }

void ToRelay(const Mac& to, const uint8_t* data, size_t size) {
  if (UdpRelayUp()) {
    std::vector<uint8_t> out(12 + size);
    Put32(out.data(), kRelayMagic), out[4] = 1, out[5] = kForward;
    std::memcpy(out.data() + 6, to.data(), 6);
    if (size) std::memcpy(out.data() + 12, data, size);
    SendOn(g_socket, g_udp_relay_ip, g_udp_relay_port, out.data(), out.size());
    return;
  }
  if (g_relay_up) ToPipe(kForward, to.data(), data, size);
}

void Send(const Path& path, const uint8_t* data, size_t size) {
  if (path.relayed) {
    ToRelay(path.mac, data, size);
  } else {
    SendOn(g_socket, path.ip, path.port, data, size);
  }
}
void Send(const Path& path, const std::vector<uint8_t>& data) { Send(path, data.data(), data.size()); }

// -- tunnelled game traffic -------------------------------------------------------------
//
// A peer outside this network is reached only through its P2P socket (the one
// port its NAT, port forward or the relay knows). Its game ports get a proxy
// socket each on 127.0.0.1 here: the game sends to the proxy, the proxy's
// packets go to the peer in kData messages, and the peer's come back out of
// the proxy to the game's own socket - from the address the game knows the
// peer's port as.

// kDataV2 (peers that ping: this version's): each tunnel packet repeats the
// last few small game packets of its stream (to / from port) with their
// sequence numbers, and the far side passes each on once, in order. In
// online matches the game's lockstep waits for every input: one lost packet
// stood both games still for a round trip plus the game's resend (400-900 ms
// at 300 ms with 2% loss); now the next packet, 16 ms later, brings it.
constexpr size_t kRepeat = 3;          // earlier packets repeated in each
constexpr size_t kRepeatMaxSize = 96;  // (only small ones: inputs, not transfers)

struct DataStream {
  uint32_t next = 1;
  std::deque<std::pair<uint32_t, std::vector<uint8_t>>> recent;
};

struct Proxy {
  Socket s = kBadSocket;
  Mac mac{};
  uint16_t remote = 0;  // the peer's game port (as its game knows it)
  uint16_t local = 0;   // the proxy's port
  std::map<uint16_t, DataStream> streams;  // (v2: by this game's port; the proxy's thread only)
  std::atomic<int64_t> used_ms{0};  // last packet through it, either way (FreeIdleProxies)
  std::atomic<bool> dead{false};    // freed: its thread deletes it
};
std::map<std::pair<Mac, uint16_t>, Proxy*> g_proxies;
std::map<uint16_t, Proxy*> g_proxy_ports;

std::vector<uint8_t> DataHeader(uint16_t to, uint16_t from) {
  Writer w;
  w.u32(kMagic), w.u8(kVersion), w.u8(kData);
  w.bytes(g_me.mac.data(), 6);
  w.u16(to), w.u16(from);
  return w.b;
}

// A v2 packet: the header, the newest sequence number, how many packets, then
// each (newest first) as a 16-bit length and its bytes.
std::vector<uint8_t> DataV2(DataStream& st, uint16_t to, uint16_t from, const uint8_t* data, size_t size) {
  const uint32_t seq = st.next++;
  Writer w;
  w.u32(kMagic), w.u8(kVersion), w.u8(kDataV2);
  w.bytes(g_me.mac.data(), 6);
  w.u16(to), w.u16(from);
  w.u32(seq);
  const size_t repeat = size <= kRepeatMaxSize ? std::min(st.recent.size(), kRepeat) : 0;
  w.u8(uint8_t(1 + repeat));
  w.u16(uint16_t(size)), w.bytes(data, size);
  for (size_t k = 0; k < repeat; ++k) {
    const auto& [s, bytes] = st.recent[st.recent.size() - 1 - k];
    if (s != seq - 1 - k) break;  // (only an unbroken run back from this one)
    w.u16(uint16_t(bytes.size())), w.bytes(bytes.data(), bytes.size());
  }
  // (the count written is an upper bound; the reader stops at the end)
  if (size <= kRepeatMaxSize) {
    st.recent.emplace_back(seq, std::vector<uint8_t>(data, data + size));
    while (st.recent.size() > kRepeat) st.recent.pop_front();
  } else {
    st.recent.clear();  // (a big one breaks the run: the next starts over)
  }
  return w.b;
}

void ProxyLoop(Proxy* proxy) {
  rex::thread::set_current_thread_name("p2p proxy");
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    const int n = int(recvfrom(proxy->s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) {
      if (proxy->dead) {  // (FreeIdleProxies freed it; nothing else holds it any more)
#if !defined(_WIN32)
        CloseSocket(proxy->s);
#endif
        delete proxy;
        return;
      }
      if (!Reset()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    proxy->used_ms = NowMs();
    // (from the game's own socket: its port as the game knows it)
    const uint16_t from_port = uint16_t(ntohs(from.sin_port) - Offset(g_me.base));
    Path path;
    uint32_t fake = 0;
    bool v2 = false;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_peers.find(proxy->mac);
      if (it == g_peers.end()) continue;
      path = PathOf(proxy->mac, it->second);
      fake = it->second.fake;
      v2 = it->second.data_v2;
      it->second.game_traffic = Clock::now();
    }
    std::vector<uint8_t> packet;
    if (v2) {
      packet = DataV2(proxy->streams[from_port], proxy->remote, from_port, buf.data(), size_t(n));
    } else {
      packet = DataHeader(proxy->remote, from_port);
      packet.insert(packet.end(), buf.begin(), buf.begin() + n);
    }
    NetPacket(true, fake, size_t(n), path.relayed);
    NetSimSend([path, packet = std::move(packet)] { Send(path, packet); });  // (net_sim.h: a test's delay)
  }
}

// The proxy for the peer's port `remote` (under g_mutex).
Proxy* ProxyFor(const Mac& mac, uint16_t remote) {
  if (auto it = g_proxies.find({mac, remote}); it != g_proxies.end()) return it->second;
  // (one per peer port this game ever played with - they live as long as the
  // game; at 64 a busy evening of Royal Rumbles ran out: a new peer's packets
  // were dropped, it froze and "left" - or every one was kicked, the host)
  if (g_proxies.size() >= 512) {
    static bool said = false;
    if (!std::exchange(said, true)) REXLOG_ERROR("p2p: 512 peer proxies - new players' packets are dropped");
    return nullptr;
  }
  auto* proxy = new Proxy{socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), mac, remote, 0};
  sockaddr_in at = Sin(0x7F000001, 0);
  socklen_t len = sizeof(at);
  if (proxy->s == kBadSocket || bind(proxy->s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 ||
      getsockname(proxy->s, reinterpret_cast<sockaddr*>(&at), &len) != 0) {
    REXLOG_WARN("p2p: no proxy socket for a peer");
    if (proxy->s != kBadSocket) CloseSocket(proxy->s);
    delete proxy;
    return nullptr;
  }
  proxy->local = ntohs(at.sin_port);
  proxy->used_ms = NowMs();
  g_proxies[{mac, remote}] = proxy;
  g_proxy_ports[proxy->local] = proxy;
  std::thread(ProxyLoop, proxy).detach();  // (proxies live as long as the game)
  return proxy;
}

// Proxies with no packet either way for 2 minutes (a session long over): their
// socket closed, their thread ends. They piled up for good before - at the cap
// a new player's packets were dropped. One wanted again is made again.
void FreeIdleProxies() {
  constexpr int64_t kIdleMs = 120000;
  const int64_t now = NowMs();
  std::lock_guard lock(g_mutex);
  for (auto it = g_proxies.begin(); it != g_proxies.end();) {
    Proxy* proxy = it->second;
    if (now - proxy->used_ms.load() < kIdleMs) {
      ++it;
      continue;
    }
    g_proxy_ports.erase(proxy->local);
    it = g_proxies.erase(it);
    proxy->dead = true;
#if defined(_WIN32)
    CloseSocket(proxy->s);  // (its recvfrom returns: the thread deletes it)
#else
    shutdown(proxy->s, SHUT_RDWR);  // (wakes its recvfrom; the thread closes it - no fd reused under it)
#endif
  }
}

// -- sessions ----------------------------------------------------------------------------

struct Property {
  uint32_t id = 0;
  uint8_t type = 0;
  std::vector<uint8_t> value;  // (guest byte order)
};

struct Session {
  uint64_t id = 0, nonce = 0;
  XnAddr host{};
  uint32_t flags = 0, public_slots = 0, private_slots = 0, filled_public = 0, filled_private = 0;
  std::map<uint32_t, uint32_t> contexts;
  std::vector<Property> properties;
};

struct Local {
  Session session;
  bool host = false;
  bool started = false;  // (XSessionStart .. XSessionEnd: its match runs - not offered to searches)
  std::set<uint64_t> members_public, members_private;
};

std::map<uint32_t, uint32_t> g_contexts;      // the player's (B0006)
std::map<uint32_t, Property> g_properties;    // (B0007)
std::map<uint32_t, Local> g_sessions;         // by session object
uint64_t g_host_left_session = 0;               // (the session whose host left in a match: told once)
std::atomic<int64_t> g_host_left_ms{0};
std::map<uint64_t, std::vector<uint8_t>> g_qos;  // QoS data by session id (XNetQosListen)
std::map<uint64_t, Session> g_found;          // the last search's sessions
// An invite accepted (online_overlay.cpp): its session comes first in searches,
// its private slots open to this game (as the console's invites let in).
uint64_t g_invited = 0;
std::atomic<P2PModHandler> g_mod_handler{nullptr};
std::atomic<P2PModXferHandler> g_mod_xfer_handler{nullptr};
std::atomic<P2PChatHandler> g_chat_handler{nullptr};
Clock::time_point g_invited_at{};

// A match is on while a peer-network session is (XSESSION_CREATE_USES_PEER_NETWORK;
// not the ONLINE menu's presence session): then the world steps exactly once a
// frame on both peers, as on the console, or locally simulated things (the
// referee...) drift apart (under g_mutex).
void UpdateLockstep() {
  static bool on = false;
  bool match = false;
  for (const auto& [obj, local] : g_sessions) match |= (local.session.flags & 0x20) != 0;
  match &= REXCVAR_GET(p2p_lockstep);
  if (match == on) return;
  on = match;
  svr2011::SetLockstep(on);
  REXLOG_INFO("p2p: lockstep {}", on ? "on (a match session)" : "off");
}

void WriteSession(Writer& w, const Session& s) {
  w.u64(s.id);
  w.bytes(s.host.data(), 36);
  w.u32(s.flags), w.u32(s.public_slots), w.u32(s.private_slots), w.u32(s.filled_public), w.u32(s.filled_private);
  w.u16(uint16_t(s.contexts.size()));
  for (const auto& [id, v] : s.contexts) w.u32(id), w.u32(v);
  w.u16(uint16_t(s.properties.size()));
  for (const auto& p : s.properties) {
    w.u32(p.id), w.u8(p.type), w.u16(uint16_t(p.value.size()));
    w.bytes(p.value.data(), p.value.size());
  }
}

// A match session (flags 0x20) always carries kDlcProperty = 113: a host's game without it (its settings or
// properties lost it - 2.1.8 report 38981a advertised 0) made every joiner's game refuse it with "you must have the
// same downloadable content" (the port's real differences - version, mods - have their own checks). For the host's
// session and for each found one (older hosts). Logged when changed.
// Test aids: SVR2011_TEST_DLC_PROPERTY=<n> - this game hosts with that value instead (an older host);
// SVR2011_TEST_DLC_KEEP=1 - this game leaves found sessions' value as it is (the game before the fix).
void CarryDlcProperty(Session& s, const char* who, bool host) {
  if (!(s.flags & 0x20)) return;
  if (!host && std::getenv("SVR2011_TEST_DLC_KEEP")) return;
  if (const char* t = std::getenv("SVR2011_TEST_DLC_PROPERTY"); host && t && *t) {
    s.properties.erase(std::remove_if(s.properties.begin(), s.properties.end(),
                                      [](const Property& q) { return q.id == kDlcProperty; }),
                       s.properties.end());
    const uint32_t v = uint32_t(std::strtoul(t, nullptr, 10));
    s.properties.push_back(Property{kDlcProperty, 1, {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}});
    REXLOG_INFO("p2p: test - session {:016X} hosted with DLC property {}", s.id, v);
    return;
  }
  auto p = std::find_if(s.properties.begin(), s.properties.end(), [](const Property& q) { return q.id == kDlcProperty; });
  const bool had = p != s.properties.end();
  const uint32_t value = had && p->value.size() >= 4 ? Be32(p->value.data()) : 0;
  if (had && p->value.size() == 4 && value == kDlcValue) return;
  REXLOG_INFO("p2p: session {:016X}'s DLC property was {} - {} carries {}", s.id,
              had ? fmt::format("{}", value) : std::string("missing"), who, kDlcValue);
  if (had) s.properties.erase(p);
  s.properties.push_back(Property{kDlcProperty, 1, {0, 0, 0, uint8_t(kDlcValue)}});
}

bool ReadSession(Reader& r, Session& s) {
  s.id = r.u64();
  r.bytes(s.host.data(), 36);
  s.flags = r.u32(), s.public_slots = r.u32(), s.private_slots = r.u32(), s.filled_public = r.u32(),
  s.filled_private = r.u32();
  const uint16_t nc = r.u16();
  for (uint16_t i = 0; i < nc && r.ok && i < 64; ++i) {
    const uint32_t id = r.u32();
    s.contexts[id] = r.u32();
  }
  const uint16_t np = r.u16();
  for (uint16_t i = 0; i < np && r.ok && i < 64; ++i) {
    Property p;
    p.id = r.u32(), p.type = r.u8();
    const uint16_t len = r.u16();
    if (len > 4096 || !r.need(len)) return false;
    p.value.assign(r.p + r.at, r.p + r.at + len);
    r.at += len;
    s.properties.push_back(std::move(p));
  }
  if (r.ok) CarryDlcProperty(s, "this game", false);
  return r.ok && s.id && s.public_slots + s.private_slots <= 32;
}

// The gameplay mods (online_files.h): a host's search reply carries its set
// after its sessions ("MODS", the hash, the set's text; older games stop
// reading before it). A searcher plays only with the same set: other hosts'
// sessions are left out, and the game's own message says why (the search's
// "No game sessions were found", an invite's join failure: P2PHiddenReason,
// online.cpp's strings). The host only logs it.
constexpr uint32_t kModsTag = 0x4D4F4453;  // "MODS"
constexpr size_t kModsMax = 6000;           // (the set's text, at most)
// After the version, the DLC packages with game files (online_files.h DlcSet - with mods off online too: the
// DLC is the game's own): "DLCS", the set's text. A reply without it (a game from before) isn't refused for it.
constexpr uint32_t kDlcTag = 0x444C4353;  // "DLCS"
constexpr size_t kDlcMax = 1000;

void WriteMods(Writer& w) {
  const std::string& set = svr2011::GameplayModSet();
  const size_t n = std::min(set.size(), kModsMax);
  w.u32(kModsTag), w.u64(svr2011::GameplayModSetHash()), w.u16(uint16_t(n));
  w.bytes(reinterpret_cast<const uint8_t*>(set.data()), n);
  const std::string& v = svr2011::PortVersion();  // (then the version)
  w.u8(uint8_t(std::min<size_t>(v.size(), 32)));
  w.bytes(reinterpret_cast<const uint8_t*>(v.data()), std::min<size_t>(v.size(), 32));
  const std::string& dlc = svr2011::DlcSet();
  const size_t dn = std::min(dlc.size(), kDlcMax);
  w.u32(kDlcTag), w.u16(uint16_t(dn));
  w.bytes(reinterpret_cast<const uint8_t*>(dlc.data()), dn);
}

// Why this game can't play the other side's (its version, then its mods;
// "" when they match). A game from before the check has neither: an older version.
// (the last Mismatch's DLC difference, short, for the search's notice: "DLC VOL03 MISSING" / "THEY LACK DLC VOL02";
// "" when it wasn't the DLC - set and read on the searching thread)
thread_local std::string t_dlc_short;

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string Mismatch(Reader& r) {
  t_dlc_short.clear();
  if (!r.ok || r.at + 14 > r.n || Be32(r.p + r.at) != kModsTag) return svr2011::VersionDifference("");
  r.u32(), r.u64();
  const uint16_t n = r.u16();
  if (!r.need(n)) return svr2011::VersionDifference("");
  std::string set(reinterpret_cast<const char*>(r.p + r.at), n);
  r.at += n;
  std::string version;
  if (r.need(1)) {
    const uint8_t vn = r.u8();
    if (r.need(vn)) version.assign(reinterpret_cast<const char*>(r.p + r.at), vn), r.at += vn;
  }
  if (std::string why = svr2011::VersionDifference(version); !why.empty()) return why;
  if (r.at + 6 <= r.n && Be32(r.p + r.at) == kDlcTag) {
    r.u32();
    const uint16_t dn = r.u16();
    if (r.need(dn)) {
      const std::string dlc(reinterpret_cast<const char*>(r.p + r.at), dn);
      r.at += dn;
      if (std::string why = svr2011::DlcDifference(svr2011::DlcSet(), dlc); !why.empty()) {
        std::vector<std::string> here, there;
        svr2011::DlcMissing(svr2011::DlcSet(), dlc, &here, &there);
        t_dlc_short = !here.empty() ? Upper(here.front()) + (here.size() > 1 ? " AND MORE" : "") + " MISSING"
                                    : "THEY LACK " + Upper(there.front());
        return why;
      }
    }
  }
  // (superstar and arena mods are checked in the lobby - per pick, per
  // session: online_mods.h - the others here)
  return svr2011::ModSetDifference(svr2011::GameplayModSet(), set, false);
}

void TellModsRefused(const uint8_t* host_xnaddr);  // (below: tells a host its sessions were left out)

// Why the last search / invite left a host out (P2PHiddenReason).
std::mutex g_hidden_mutex;
std::string g_hidden_why;
std::set<uint64_t> g_hidden_ids;  // (a host answers once per path: counted once)
std::chrono::steady_clock::time_point g_hidden_at{};

void Hidden(const std::string& why, const std::vector<uint64_t>& sessions) {
  std::lock_guard lock(g_hidden_mutex);
  if (g_hidden_ids.empty() || g_hidden_why.empty()) g_hidden_why = why;
  g_hidden_ids.insert(sessions.begin(), sessions.end());
  g_hidden_at = std::chrono::steady_clock::now();
}

// -- the discovery protocol ------------------------------------------------------------

std::mutex g_wait_mutex;
std::condition_variable g_wait;
std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_replies;  // by request nonce

std::vector<uint8_t> Header(uint8_t type, uint32_t nonce) {
  Writer w;
  w.u32(kMagic), w.u8(kVersion), w.u8(type), w.u32(nonce);
  w.bytes(MyXnAddr().data(), 36);
  return w.b;
}

uint32_t NewNonce() {
  static std::atomic<uint32_t> next{uint32_t(std::random_device{}())};
  return ++next;
}

// A far-off peer with no direct round trip yet: its addresses, every second (from PingLoop, and whenever it's heard
// through the relay), until one answers - both routers letting the other in: direct from then on. Not only while
// relayed: on match network p2p nothing comes through the relay after the join, and a first try that was lost was
// the last.
void Punch(const Mac& mac) {
  std::vector<std::pair<uint32_t, uint16_t>> to;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(mac);
    if (it == g_peers.end() || !it->second.tunnel || ForceRelay()) return;
    Peer& p = it->second;
    if (p.probing_from != Clock::time_point{}) return;  // (Probe's)
    const auto now = Clock::now();
    if (now - (p.data_v2 ? p.direct_pong : p.direct) < std::chrono::seconds(3)) return;  // (direct works)
    if (now - p.punched < std::chrono::seconds(1)) return;
    p.punched = now;
    const auto add = [&to](uint32_t ip, uint16_t port) {
      if (ip && port && std::find(to.begin(), to.end(), std::make_pair(ip, port)) == to.end()) to.emplace_back(ip, port);
    };
    const uint32_t online = Be32(p.xnaddr.data() + 4), lan = Be32(p.xnaddr.data());
    add(p.seen_ip, p.seen_port);
    add(online, Be16(p.xnaddr.data() + 24));
    add(online, p.base);
    // (the same public address: the same network, or strangers behind one carrier's NAT - its LAN address or, if the
    // router turns packets back in, its public one; both tried)
    if (online == g_me.online) add(lan, p.base);
  }
  const auto hello = Header(kHello, NewNonce() | 1);
  for (const auto& [ip, port] : to) SendTo(ip, port, hello);
}

// -- connecting to a player: every address at once, from both sides ---------------------
//
// A player this game is connecting to (joining its session, or it joining this one's, or one the game plays with) gets
// this game's addresses through the relay (kCandidates: LAN, the router's mapping, what STUN and the relay over UDP
// saw), and sends its own back. The list reaching a game is the "start": both then ping every address - fast at first,
// so the two sides' first packets, each opening its own router for the other's, overlap whatever the relay's delay -
// until one answers; the fastest answering carries the game (Nominate). Pings keep the chosen path's mappings open.
// Only for players in a session with this game: a list from anyone else is ignored, so no stranger on the relay can
// have this game send packets at an address it picks.
// IPv6 is one more address on the list: no NAT, but most routers' firewalls only let in what answers something sent
// out - the same both-sides-at-once start gets through them. It goes only to the players in the session (a temporary
// address where the system has one: it picks that for a new connection).
void BeginProbing(Peer& p, Clock::time_point now) {
  if (p.probing_from == Clock::time_point{}) p.probing_from = now, p.probed = {};
}

std::vector<Candidate> MyCandidates() {
  std::vector<Candidate> out;
  const auto add = [&out](uint32_t ip, uint16_t port, uint8_t kind) {
    if (ip && port && std::none_of(out.begin(), out.end(), [&](const Candidate& c) { return c.ip == ip && c.port == port; }))
      out.push_back({ip, port, kind});
  };
  add(g_me.lan, g_me.base, kCandLan);
  if (MappedUsable()) add(g_me.online, g_mapped_port, kCandMapped);
  const int64_t now = NowMs();
  if (now - g_stun_ms < 90000) add(g_stun_ip, g_stun_port, kCandStun);
  if (now - g_edge_ms < 30000) add(g_edge_ip, g_edge_port, kCandEdge);
  add(g_my_v6, g_me.base, kCandV6);
  return out;
}

void SendCandidates(const Path& path) {
  const std::vector<Candidate> mine = MyCandidates();
  Writer w;
  w.b = Header(kCandidates, NewNonce());
  w.u8(uint8_t(mine.size()));
  for (const Candidate& c : mine) {  // (kind, family, address, port)
    w.u8(c.kind), w.u8(IsV6(c.ip) ? 6 : 4);
    if (V6 a; IsV6(c.ip) && V6Of(c.ip, &a)) w.bytes(a.data(), 16);
    else w.u32(c.ip);
    w.u16(c.port);
  }
  ToRelay(path.mac, w.b.data(), w.b.size());
  if (!path.relayed) SendOn(g_socket, path.ip, path.port, w.b.data(), w.b.size());  // (no relay: a LAN, p2p_peers)
}

// A list that came (through Learn's lock already released).
void OnCandidates(const Mac& mac, Reader& r) {
  Path back;
  bool answer = false;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_peers.find(mac);
    if (it == g_peers.end() || ForceRelay()) return;
    Peer& p = it->second;
    const bool in_session = std::any_of(g_sessions.begin(), g_sessions.end(), [](const auto& l) { return (l.second.session.flags & 0x20) != 0; });
    if (!in_session && p.probing_from == Clock::time_point{}) return;  // (a stranger's)
    const uint8_t n = r.u8();
    for (uint8_t k = 0; k < n && k < 12 && r.ok; ++k) {
      const uint8_t kind = r.u8(), family = r.u8();
      if (family == 6) {
        V6 a;
        r.bytes(a.data(), 16);
        const uint16_t port = r.u16();
        // (only where this game has IPv6 itself)
        if (r.ok && g_my_v6 && (g_v6_test || GlobalV6(a.data()))) AddCandidate(p, V6Number(a.data()), port, kind);
        continue;
      }
      if (family != 4) break;  // (a kind this version doesn't know: the rest unread)
      const uint32_t ip = r.u32();
      const uint16_t port = r.u16();
      const uint8_t top = uint8_t(ip >> 24);
      // (nothing that isn't one place: broadcast, multicast, loopback - a test's NAT box has its "public" addresses there)
      if (!r.ok || top >= 224 || top == 0 || (top == 127 && !g_box_ip)) continue;
      AddCandidate(p, ip, port, kind);
    }
    const auto now = Clock::now();
    BeginProbing(p, now);
    if (now - p.candidates_sent >= std::chrono::seconds(2)) {
      p.candidates_sent = now;
      answer = true;
      back = PathOf(mac, p);
    }
  }
  if (answer) SendCandidates(back);
}

// A ping: this game's time, then its player's name (for the peers' lobby ping list; older games stop reading before it).
std::vector<uint8_t> PingPacket() {
  Writer w;
  w.b = Header(kPing, NewNonce());
  w.u64(NowUs());
  const std::string name = rex::cvar::Query<std::string>("online_name").substr(0, 32);
  w.u8(uint8_t(name.size())), w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  return w.b;
}

void ProbeLoop() {
  rex::thread::set_current_thread_name("p2p probe");
  using namespace std::chrono_literals;
  for (;;) {
    std::this_thread::sleep_for(50ms);
    const auto now = Clock::now();
    std::vector<std::pair<uint32_t, uint16_t>> to;
    {
      std::lock_guard lock(g_mutex);
      for (auto& [mac, p] : g_peers) {
        if (p.probing_from == Clock::time_point{} || !p.tunnel || ForceRelay()) continue;
        if (now - std::max(p.probing_from, p.game_traffic) > 30s) {  // (not playing with it any more)
          p.probing_from = {};
          continue;
        }
        const auto age = now - p.probing_from;
        // (direct works; the first seconds go on: a faster address may answer too)
        if (now - p.direct_pong < 3s && age >= 3s) continue;
        if (now - p.probed < (age < 5s ? 100ms : age < 10s ? 250ms : 1000ms)) continue;
        p.probed = now;
        const uint32_t online = Be32(p.xnaddr.data() + 4), lan = Be32(p.xnaddr.data());
        AddCandidate(p, p.seen_ip, p.seen_port, kCandEdge);
        AddCandidate(p, online, Be16(p.xnaddr.data() + 24), kCandStun);
        AddCandidate(p, online, p.base, kCandBase);
        AddCandidate(p, lan, p.base, kCandLan);
        for (const Candidate& c : p.candidates) to.emplace_back(c.ip, c.port);
      }
    }
    if (to.empty()) continue;
    for (const auto& [ip, port] : to) {
      Path path;
      path.ip = ip, path.port = port;
      NetSimSend([path, b = PingPacket()] { Send(path, b); });
    }
  }
}

// A peer's game packet: to this game's port `to`, from its port `from`.
void OnData(const uint8_t* data, size_t size, Via via) {
  if (size < 16) return;
  Mac mac;
  std::memcpy(mac.data(), data + 6, 6);
  const uint16_t to = Be16(data + 12), from = Be16(data + 14);
  Socket s;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(mac);
    if (it == g_peers.end()) return;
    Peer& p = it->second;
    if (!via.relayed && via.ip) {  // (a NAT may move it: follow)
      if (via.ip != p.route || via.port != p.route_port) {
        const XnAddr xnaddr = p.xnaddr;
        Learn(xnaddr.data(), via);
      }
      p.direct = Clock::now();
    }
    NetPacket(false, p.fake, size - 16, via.relayed);
    p.game_traffic = Clock::now();
    Proxy* proxy = ProxyFor(mac, from);
    if (!proxy) return;
    proxy->used_ms = NowMs();
    s = proxy->s;
  }
  SendOn(s, 0x7F000001, uint16_t(to + Offset(g_me.base)), data + 16, size - 16);
  if (via.relayed) Punch(mac);
}

// A v2 game packet (DataV2): the ones not passed on yet, oldest first.
void OnDataV2(const uint8_t* data, size_t size, Via via) {
  if (size < 21) return;
  Mac mac;
  std::memcpy(mac.data(), data + 6, 6);
  const uint16_t to = Be16(data + 12), from = Be16(data + 14);
  const uint32_t seq = Be32(data + 16);
  const uint8_t count = data[20];
  // the packets, newest first: (sequence, bytes)
  std::vector<std::pair<uint32_t, std::pair<const uint8_t*, size_t>>> got;
  for (size_t at = 21, k = 0; k < count && at + 2 <= size; ++k) {
    const size_t n = Be16(data + at);
    if (at + 2 + n > size) break;
    got.push_back({seq - uint32_t(k), {data + at + 2, n}});
    at += 2 + n;
  }
  if (got.empty()) return;
  Socket s;
  uint32_t fake = 0;
  std::vector<std::pair<const uint8_t*, size_t>> deliver;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(mac);
    if (it == g_peers.end()) return;
    Peer& p = it->second;
    if (!via.relayed && via.ip) {
      if (via.ip != p.route || via.port != p.route_port) {
        const XnAddr xnaddr = p.xnaddr;
        Learn(xnaddr.data(), via);
      }
      p.direct = Clock::now();
    }
    fake = p.fake;
    p.game_traffic = Clock::now();
    uint32_t& last = p.delivered[{to, from}];
    // (a peer that started over - a new match, a restarted game - begins at 1 again: far behind, or a stream's
    // very first packets behind a short old one - RESTART AND REJOIN's new copy, whose join packets were dropped
    // as already seen while the old copy's stream had fewer than 64)
    const uint32_t was = last;
    // (a stream's very start again: the peer's game restarted - its old proxy goes, so this game's game sees a new
    // address: through the old one its new join was taken for the old connection and never answered)
    const bool restarted = seq <= kRepeat + 1 && seq < last;
    if (seq + 64 < last || restarted) last = 0;
    for (auto e = got.rbegin(); e != got.rend(); ++e) {
      if (e->first > last) deliver.push_back(e->second), last = e->first;
    }
    if (restarted)
      if (auto old = g_proxies.find({mac, from}); old != g_proxies.end()) {
        Proxy* dead = old->second;
        g_proxy_ports.erase(dead->local);
        g_proxies.erase(old);
        dead->dead = true;
#if defined(_WIN32)
        CloseSocket(dead->s);  // (its recvfrom returns: its thread deletes it)
#else
        shutdown(dead->s, SHUT_RDWR);
#endif
        REXLOG_INFO("p2p: a peer's game started over (port {}): a new proxy for it", from);
      }
    Proxy* proxy = ProxyFor(mac, from);
    // Test aid: SVR2011_TEST_P2P_DATA_LOG=<n> - the first n game packets in of which nothing got through: their
    // stream, sequence, the last one passed on before
    static int data_log = [] { const char* v = std::getenv("SVR2011_TEST_P2P_DATA_LOG"); return v ? std::atoi(v) : 0; }();
    if (data_log > 0 && (deliver.empty() || !proxy)) {
      --data_log;
      REXLOG_INFO("p2p: data in {}->{} seq {} ({} in it), last was {}, {} passed on{}", from, to, seq, got.size(), was,
                  deliver.size(), proxy ? "" : " - no proxy");
    }
    if (!proxy) return;
    proxy->used_ms = NowMs();
    s = proxy->s;
  }
  for (const auto& [bytes, n] : deliver) {
    NetPacket(false, fake, n, via.relayed);
    SendOn(s, 0x7F000001, uint16_t(to + Offset(g_me.base)), bytes, n);
  }
  if (via.relayed) Punch(mac);
}

// The router's kind, from the ports the two far ends saw (g_stun_port /
// g_edge_port) - and the step between them for a symmetric one.
std::pair<const char*, int> NatKind() {
  if (MappedUsable()) return {"mapped", 0};  // (UPnP / NAT-PMP opened the port: port_map.h)
  if (g_me.online && g_me.online == g_me.lan) return {"open", 0};
  const uint16_t stun = g_stun_port, edge = g_edge_port;
  // (both seen lately, the mapping kept open between them: an old STUN answer from before the router dropped the
  // mapping differs on any router that picks a new port, and read as "symmetric")
  const int64_t now = NowMs();
  if (!stun || !edge || now - g_stun_ms > 90000 || now - g_edge_ms > 30000) return {"unknown", 0};
  if (stun == edge) return {"cone", 0};
  return {"symmetric", int(edge) - int(stun)};
}

// Once, after a match: how it connected, to the server's counts (no address,
// no name: the path, the router's kind, the average ping, the version).
// Silent when it fails.
void SendMatchReport(const MatchReport& r) {
  if (!REXCVAR_GET(p2p_match_report)) return;
  const uint32_t peer_secs = r.direct + r.relay + r.lan;
  if (!peer_secs) return;
  const char* path = r.relay * 2 >= peer_secs ? "relay" : r.lan * 2 > peer_secs ? "lan" : "direct";
  const auto [nat, step] = NatKind();
  const int rtt = r.rtt_n ? int(r.rtt_sum / r.rtt_n + 0.5) : 0;
  std::string version;
  for (const char c : PortVersion())
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') version.push_back(c);
  // (IPv6: a field of its own beside path "direct": a server before 2.2.0 ignores it and counts a direct match, where
  // a path value it doesn't know would be refused)
  const bool ipv6 = !std::strcmp(path, "direct") && r.ipv6 * 2 > r.direct;
  const std::string body = fmt::format(R"({{"version":"{}","path":"{}",{}"nat":"{}","step":{},"rtt":{},"network":"{}"}})",
                                       version.substr(0, 24), path, ipv6 ? "\"ipv6\":true," : "", nat, step, rtt,
                                       NetName(r.network));
  REXLOG_INFO("p2p: match report {} ({} s; peer-seconds direct {} (IPv6 {}), relay {}, LAN {})", body, r.seconds, r.direct,
              r.ipv6, r.relay, r.lan);
  std::thread([body] {
    net::ServerRequest("POST", "/api/netreport", body, {{"Content-Type", "application/json"}});
  }).detach();
}

// Once a second, a ping to each peer this game exchanges game packets with (it
// has a proxy): their round trips for the log and the ping indicator (net_sim.h).
constexpr auto kPingAfterGame = std::chrono::seconds(15);  // (pings this long after the last game packet)

// The relay warning (online_overlay.cpp: P2PRelayPath): this match session's way to the far-off players,
// decided in its lobby - 8 s of game traffic with them, never during the match. Relayed because direct
// failed (no direct round trip), or by the server / the test switch; not when the relay was only faster.
// Relayed only after 5 s without a direct round trip, direct again as soon as one comes back (the warning: once a
// session). Caller holds g_mutex.
std::atomic<uint8_t> g_relay_path{0};  // (P2PRelayPathInfo::kind)
std::atomic<uint64_t> g_relay_session{0};

void RelayCheck(const std::set<Mac>& gamers, bool in_match) {
  static uint64_t session = 0;
  static Clock::time_point since{}, failing{};
  uint64_t sid = 0;
  for (const auto& [obj, local] : g_sessions)
    if (local.session.flags & 0x20) sid = local.session.id;
  if (sid != session) session = sid, since = {}, failing = {}, g_relay_path = 0, g_relay_session = sid;
  if (!sid || in_match) return;
  const auto now = Clock::now();
  int remote = 0;
  bool failed = false;
  for (const Mac& mac : gamers) {
    const auto it = g_peers.find(mac);
    if (it == g_peers.end()) continue;
    const Peer& p = it->second;
    if (!p.tunnel || now - p.game_traffic > std::chrono::seconds(5)) continue;
    ++remote;
    // (no direct round trip: on the relay - or, on match network p2p, where the relay never carries it, nowhere)
    const bool direct_ok = p.data_v2 && now - p.direct_pong < std::chrono::seconds(3);
    if (!direct_ok && (p.relayed || NoRelayData())) failed = true;
  }
  if (!remote) {
    since = {};
    return;
  }
  if (since == Clock::time_point{}) since = now;
  if (!failed) failing = {};
  else if (failing == Clock::time_point{}) failing = now;
  if (now - since < std::chrono::seconds(8)) return;
  const uint8_t fail_kind = NoRelayData() ? 4 : 2;  // (p2p: no relay to fall back on)
  const uint8_t path =
      ForceRelay() ? 3 : failed && now - failing >= std::chrono::seconds(5) ? fail_kind : failed ? g_relay_path.load() : 1;
  if (!path) return;
  if (g_relay_path.exchange(path) != path)
    REXLOG_INFO("p2p: session {:016X} {} ({} far-off player{}, this router: {})", sid,
                path == 4   ? "can't connect (direct failed; this server's matches don't go through the relay)"
                : path == 3 ? "plays through the relay (set so)"
                : path == 2 ? "plays through the relay (direct failed)"
                            : "plays direct",
                remote, remote == 1 ? "" : "s", NatKind().first);
}

void PingLoop() {
  rex::thread::set_current_thread_name("p2p ping");
  for (int tick = 1;; ++tick) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (tick % 10 == 0) FreeIdleProxies();
    std::vector<Path> to, send_list;
    std::vector<Mac> punch;
    const bool in_match = NetStatsNow().active;
    {
      std::lock_guard lock(g_mutex);
      std::set<Mac> gamers;
      for (const auto& [key, proxy] : g_proxies) gamers.insert(proxy->mac);
      RelayCheck(gamers, in_match);
      if (in_match) {  // (the match report: each peer's path this second)
        ++g_report.seconds;
        g_report.network = g_match_net;
        for (const Mac& mac : gamers) {
          const auto it = g_peers.find(mac);
          if (it == g_peers.end()) continue;
          const Peer& p = it->second;
          ++(!p.tunnel ? g_report.lan : p.relayed ? g_report.relay : g_report.direct);
          if (p.tunnel && !p.relayed && IsV6(p.route)) ++g_report.ipv6;
          if (const float rtt = p.relayed ? p.rtt_relay : p.rtt_direct; rtt > 0) g_report.rtt_sum += rtt, ++g_report.rtt_n;
        }
      }
      const auto now = Clock::now();
      for (const Mac& mac : gamers) {
        auto it = g_peers.find(mac);
        if (it == g_peers.end()) continue;
        Peer& p = it->second;
        // (only the players this game plays with now: a peer of an earlier
        // session - its proxy lives on - was pinged for good and stayed in the
        // players' ping list after leaving, and after a restart of either game)
        if (now - p.game_traffic > kPingAfterGame) continue;
        if (now - p.pinged > std::chrono::seconds(3)) p.direct_ok = p.relay_ok = now;  // (pinged again: a fresh start)
        p.pinged = now;
        // (match network p2p: no match data through the relay, either way - a peer put on it before this session's
        // mode latched (in the ONLINE menu, the mode auto) stayed there: the joiner's game packets went into the relay
        // while the host answered only directly, and nothing came back - report 01a0ae)
        if (NoRelayData() && p.relayed && !ForceRelay()) {
          p.relayed = false;
          REXLOG_INFO("p2p: peer {} off the relay (this server's matches don't go through it)", Ip(p.fake));
        }
        Path direct = PathOf(mac, p);
        direct.relayed = false;
        // A pinging peer's path, checked every second: direct whenever a direct ping came back in the last 3 s, the
        // relay only when none did. (Never for being faster: it adds its own detour, and a direct path that works is
        // what the players want. Not the "fresh start" either: a new lobby's first seconds went direct with nothing
        // proven, into the void for a pair whose direct fails.)
        if (p.data_v2 && !ForceRelay()) {
          const bool ok = now - p.direct_pong < std::chrono::seconds(3);
          const bool relay = !ok && g_relay_up && p.tunnel && !NoRelayData();
          const char* why = relay ? "no direct round trip for 3 s" : ok ? "a direct round trip" : "no relay";
          if (relay != p.relayed) {
            p.relayed = relay;
            REXLOG_INFO("p2p: peer {} {} ({}; round trips direct {:.0f} ms, relay {:.0f} ms)", Ip(p.fake),
                        relay ? "through the relay" : "direct", why, p.rtt_direct, p.rtt_relay);
          }
        }
        if (p.tunnel) punch.push_back(mac);
        // (a player the game plays with: connecting to it, its list again while nothing direct answers)
        if (p.tunnel && !ForceRelay() && now - p.direct_pong >= std::chrono::seconds(3) &&
            now - p.candidates_sent >= std::chrono::seconds(5)) {
          BeginProbing(p, now);
          p.candidates_sent = now;
          send_list.push_back(direct);
        }
        // (pings both ways for a tunnel peer: the direct one tells whether direct works)
        if (!ForceRelay()) to.push_back(direct);
        if (p.tunnel && g_relay_up && !NoRelayData()) {
          Path relayed = direct;
          relayed.relayed = true;
          to.push_back(relayed);
        }
      }
    }
    for (const Mac& mac : punch) Punch(mac);
    for (const Path& path : send_list) SendCandidates(path);
    for (const Path& path : to) {
      NetSimSend([path, b = PingPacket()] { Send(path, b); });
    }
    // The session's host gone in a match (its game closed or cut off): the game
    // ends the session for everyone after its drop timeout, saying only "a
    // connection error" - this says why (a notice now; the game's "An error
    // occurred during the match" reworded: online.cpp, P2PHostLeft).
    if (in_match) {
      std::lock_guard lock(g_mutex);
      const auto now = Clock::now();
      for (const auto& [object, local] : g_sessions) {
        if (local.host) continue;
        const auto it = g_peers.find(MacOf(local.session.host.data()));
        if (it == g_peers.end()) continue;
        const Peer& h = it->second;
        const auto heard = std::max(h.direct_ok, h.relay_ok);
        if (heard == Clock::time_point{} || now - heard < std::chrono::seconds(4) ||
            g_host_left_session == local.session.id)
          continue;
        g_host_left_session = local.session.id;
        g_host_left_ms = NowMs();
        REXLOG_WARN("p2p: the session's host ({}) stopped answering in the match - it left", Ip(h.fake));
        OnlineOverlayNotice("THE HOST LEFT", "THE MATCH IS OVER - BACK TO THE ONLINE MENU", 12);
      }
    }
    if (!in_match && g_report.seconds) {
      if (g_report.seconds >= 30) SendMatchReport(g_report);  // (a match, not a lobby's blip)
      g_report = {};
    }
    // (test: SVR2011_TEST_MATCH_REPORT=<s> - the report after that many seconds of the match, as if it ended)
    static const uint32_t test_after = [] { const char* v = std::getenv("SVR2011_TEST_MATCH_REPORT"); return v ? uint32_t(std::atoi(v)) : 0u; }();
    if (test_after && in_match && g_report.seconds == test_after) SendMatchReport(g_report);
  }
}

// -- files peers fetch from each other (P2PFetch: online_cas.cpp) -------------------
//
// A request names a file (kind + key) and a byte range; the peers that have it
// answer with parts of up to kChunk bytes (total 0xFFFFFFFF: "I haven't"). The
// asker re-requests what didn't arrive from the peer that answered.

constexpr uint32_t kChunk = 1024, kNone = 0xFFFFFFFF;
P2PFileSource g_file_source = nullptr;

struct FileFetch {
  uint8_t kind = 0;
  std::string key;
  std::string data;
  std::vector<bool> have;
  uint32_t total = 0, got = 0;
  int refusals = 0;
  bool known = false;
  Path source;
};
std::map<uint32_t, FileFetch*> g_fetches;  // by request nonce (under g_wait_mutex)

std::vector<uint8_t> FileRequest(uint32_t nonce, uint8_t kind, const std::string& key, uint32_t offset,
                                 uint32_t length) {
  Writer w;
  w.b = Header(kFileRequest, nonce);
  w.u8(kind), w.u8(uint8_t(key.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  w.u32(offset), w.u32(length);
  return w.b;
}

// A peer asks for a file this game has: the parts of the range (or "I haven't").
void ServeFile(Reader& r, uint32_t nonce, const Path& back) {
  const uint8_t kind = r.u8(), len = r.u8();
  if (!r.need(len)) return;
  const std::string key(reinterpret_cast<const char*>(r.p + r.at), len);
  r.at += len;
  const uint32_t offset = r.u32(), length = r.u32();
  if (!r.ok || !g_file_source) return;
  // (the last few files asked for, so a transfer reads each once)
  static std::mutex cache_mutex;
  static std::vector<std::pair<std::string, std::shared_ptr<std::optional<std::string>>>> cache;
  std::shared_ptr<std::optional<std::string>> file;
  {
    std::lock_guard lock(cache_mutex);
    const std::string id = std::to_string(kind) + ":" + key;
    for (auto& [k, v] : cache) {
      if (k == id) file = v;
    }
    if (!file) {
      file = std::make_shared<std::optional<std::string>>(g_file_source(kind, key));
      cache.emplace_back(id, file);
      if (cache.size() > 8) cache.erase(cache.begin());
    }
  }
  auto part = [&](uint32_t total, uint32_t at, const char* bytes, uint16_t n) {
    Writer w;
    w.b = Header(kFilePart, nonce);
    w.u8(kind), w.u8(uint8_t(key.size()));
    w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    w.u32(total), w.u32(at), w.u16(n);
    w.bytes(reinterpret_cast<const uint8_t*>(bytes), n);
    Send(back, w.b);
  };
  if (!*file) return part(kNone, 0, nullptr, 0);
  const std::string& bytes = **file;
  const uint32_t total = uint32_t(bytes.size());
  const uint32_t end = uint32_t(std::min<uint64_t>(total, uint64_t(offset) + std::min<uint32_t>(length, 1u << 20)));
  if (total == 0) return part(0, 0, nullptr, 0);
  for (uint32_t at = offset; at < end; at += kChunk) {
    part(total, at, bytes.data() + at, uint16_t(std::min<uint32_t>(kChunk, end - at)));
  }
}

void OnFilePart(Reader& r, uint32_t nonce, const Path& back) {
  r.u8();
  const uint8_t len = r.u8();
  if (!r.need(len)) return;
  r.at += len;
  const uint32_t total = r.u32(), at = r.u32();
  const uint16_t n = r.u16();
  if (!r.ok || !r.need(n)) return;
  std::lock_guard lock(g_wait_mutex);
  auto it = g_fetches.find(nonce);
  if (it == g_fetches.end()) return;
  FileFetch& f = *it->second;
  if (total == kNone) {
    ++f.refusals;
  } else if (!f.known) {
    if (total > (64u << 20)) return;
    f.known = true, f.total = total, f.source = back;
    f.data.assign(total, '\0');
    f.have.assign((total + kChunk - 1) / kChunk, false);
  }
  if (total != kNone && f.known && total == f.total && at < total && at % kChunk == 0 && !f.have[at / kChunk]) {
    std::memcpy(f.data.data() + at, r.p + r.at, std::min<uint32_t>(n, total - at));
    f.have[at / kChunk] = true;
    ++f.got;
  }
  g_wait.notify_all();
}

void OnPacket(const uint8_t* data, size_t size, Via via) {
  Reader r{data, size};
  if (r.u32() != kMagic || r.u8() != kVersion) return;
  const uint8_t type = r.u8();
  if (type == kData) return OnData(data, size, via);
  if (type == kDataV2) return OnDataV2(data, size, via);
  const uint32_t nonce = r.u32();
  XnAddr sender{};
  r.bytes(sender.data(), 36);
  const Mac mac = MacOf(sender.data());
  if (!r.ok || mac == g_me.mac) return;  // (our own broadcast)
  if (ForceRelay() && !via.relayed) return;
  Path back;
  {
    std::lock_guard lock(g_mutex);
    back = PathOf(mac, Learn(sender.data(), via));
  }
  // (replies go back the way the request came)
  back.relayed = via.relayed;
  if (!via.relayed) back.ip = via.ip, back.port = via.port;
  if (via.relayed) Punch(mac);
  switch (type) {
    case kSearch: {  // -> the sessions this game hosts
      Writer w;
      w.b = Header(kSessions, nonce);
      std::vector<Session> mine;
      {
        std::lock_guard lock(g_mutex);
        for (const auto& [obj, local] : g_sessions) {
          // (not one whose match runs: a lockstep match can't be joined - it listed, the join then failed)
          if (local.host && !local.started) mine.push_back(local.session);
        }
      }
      if (mine.empty() && via.relayed) break;  // (a search through the relay reaches every game)
      w.u16(uint16_t(mine.size()));
      for (const auto& s : mine) WriteSession(w, s);
      WriteMods(w);
      Send(back, w.b);
      break;
    }
    case kModsRefused: {  // a searcher left this game's sessions out: its version or mods differ
      const std::string why = Mismatch(r);
      if (!why.empty()) REXLOG_INFO("p2p: a player couldn't join - {}", why);
      break;
    }
    case kQosRequest: {
      const uint64_t id = r.u64();
      Writer w;
      w.b = Header(kQosReply, nonce);
      std::vector<uint8_t> qos;
      {
        std::lock_guard lock(g_mutex);
        if (auto it = g_qos.find(id); it != g_qos.end()) qos = it->second;
      }
      w.u64(id), w.u16(uint16_t(qos.size()));
      w.bytes(qos.data(), qos.size());
      Send(back, w.b);
      break;
    }
    case kHello:
      if (nonce & 1) Send(back, Header(kHello, nonce + 1));  // (the first of a pair: answered)
      break;
    case kCandidates:
      OnCandidates(mac, r);
      break;
    case kPing: {  // -> its time back (the way game packets go, with a test's delay)
      {
        std::lock_guard lock(g_mutex);  // (it pings: it's this version, kDataV2 from now on)
        if (auto it = g_peers.find(mac); it != g_peers.end() && !it->second.data_v2) {
          it->second.data_v2 = !NetDataV1();
          REXLOG_INFO("p2p: peer {} takes repeated game packets{}", Ip(it->second.fake),
                      NetDataV1() ? " (off: SVR2011_TEST_NET_V1)" : "");
        }
      }
      Writer w;
      w.b = Header(kPong, nonce);
      w.u64(r.u64());
      if (r.ok) NetSimSend([back, b = std::move(w.b)] { Send(back, b); });
      if (const size_t n = r.u8(); r.ok && n && r.need(n)) {  // (its player's name)
        const std::string name(reinterpret_cast<const char*>(data + r.at), n);
        uint32_t fake = 0;
        {
          std::lock_guard lock(g_mutex);
          if (auto it = g_peers.find(mac); it != g_peers.end()) fake = it->second.fake;
        }
        if (fake) NetPeerName(fake, name);
      }
      break;
    }
    case kPong: {
      const uint64_t sent_us = r.u64();
      if (!r.ok) break;
      uint32_t fake = 0;
      bool current = true;  // (the round trip of the path game packets take now)
      const char* how = via.relayed ? "relay" : "direct";
      {
        std::lock_guard lock(g_mutex);
        if (auto it = g_peers.find(mac); it != g_peers.end()) {
          Peer& p = it->second;
          fake = p.fake;
          // (each path's round trip, smoothed)
          const float ms = float(double(NowUs() - sent_us) / 1000.0);
          const auto smooth = [ms](float& rtt) { rtt = rtt > 0 ? rtt * 0.7f + ms * 0.3f : ms; };
          const auto now = Clock::now();
          if (via.relayed) {
            smooth(p.rtt_relay);
            p.relay_ok = now;
            current = p.relayed;
            how = "relay";
          } else {
            // (the address that answered: the fastest of those answering carries the game)
            const uint32_t from = via.ip == 0x7F000001 ? Be32(p.xnaddr.data()) : via.ip;
            p.direct_ok = p.direct_pong = now;
            AddCandidate(p, from, via.port, kCandHeard);
            bool on_lan = !p.tunnel;
            if (Candidate* c = FindCandidate(p, from, via.port)) smooth(c->rtt), c->ok = now, on_lan |= c->kind == kCandLan;
            how = IsV6(from) ? "IPv6" : on_lan ? "LAN" : "direct";
            if (p.tunnel) Nominate(p, now);
            const bool on_route = from == p.route && via.port == p.route_port;
            if (on_route) smooth(p.rtt_direct);
            current = !p.relayed && on_route;
          }
        }
      }
      if (current) NetRtt(fake, double(NowUs() - sent_us) / 1000.0, via.relayed, how);
      break;
    }
    case kMods: {  // (online_mods.cpp: the rest is its own)
      P2PModHandler handler = g_mod_handler.load();
      if (handler && r.ok && r.at <= r.n) handler(r.p + r.at, r.n - r.at);
      break;
    }
    case kModXfer: {  // (mod_transfer.cpp: the rest is its own; never through the relay)
      P2PModXferHandler handler = g_mod_xfer_handler.load();
      if (handler && !via.relayed && r.ok && r.at <= r.n) handler(PeerId(mac), r.p + r.at, r.n - r.at);
      break;
    }
    case kChat: {  // (online_chat.cpp: the rest is its own)
      P2PChatHandler handler = g_chat_handler.load();
      if (handler && r.ok && r.at <= r.n) handler(r.p + r.at, r.n - r.at);
      break;
    }
    case kFileRequest:
      ServeFile(r, nonce, back);
      break;
    case kFilePart:
      OnFilePart(r, nonce, back);
      break;
    case kSessions:
    case kQosReply: {
      std::lock_guard lock(g_wait_mutex);
      if (auto it = g_replies.find(nonce); it != g_replies.end()) {
        it->second.emplace_back(data + r.at, data + size);
        g_wait.notify_all();
      }
      break;
    }
  }
}

// STUN (RFC 5389) binding requests and answers: this socket's public address.
constexpr uint32_t kStunCookie = 0x2112A442;

bool OnStun(const uint8_t* data, size_t size) {
  if (size < 20 || Be16(data) != 0x0101 || Be32(data + 4) != kStunCookie) return false;
  for (size_t at = 20; at + 4 <= size;) {
    const uint16_t type = Be16(data + at), len = Be16(data + at + 2);
    if (at + 4 + len > size) break;
    if ((type == 0x0020 || type == 0x0001) && len >= 8 && data[at + 5] == 1) {  // (XOR-)MAPPED-ADDRESS, IPv4
      uint16_t port = Be16(data + at + 6);
      uint32_t ip = Be32(data + at + 8);
      if (type == 0x0020) port ^= uint16_t(kStunCookie >> 16), ip ^= kStunCookie;
      g_stun_port = port;  // (before a router mapping replaces it: the NAT's own choice)
      g_stun_ip = ip;
      g_stun_ms = NowMs();
      if (MappedUsable()) port = g_mapped_port;  // (forwarded by the router: that one)
      if (g_me.online != ip || g_me.online_port != port) {
        REXLOG_INFO("p2p: public address {}:{} (STUN)", Ip(ip), port);
      }
      g_me.online = ip, g_me.online_port = port;
      break;
    }
    at += 4 + ((len + 3) & ~3u);
  }
  return true;
}

void OnRelay(const uint8_t* data, size_t size) {
  if (size < 12 || data[4] != 1) return;
  const uint32_t ip = Be32(data + 6);
  const uint16_t port = Be16(data + 10);
  if (data[5] == kWelcome) {
    if (!g_relay_up.exchange(true)) REXLOG_INFO("p2p: relay up (this game seen from {})", Ip(ip));
    if (!g_me.online && ip) g_me.online = ip;  // (no STUN answer)
  } else if (data[5] == kFrom) {
    OnPacket(data + 12, size - 12, {ip, port, true});
  }
}

void Receive() {
  rex::thread::set_current_thread_name("p2p receive");
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    int n = int(recvfrom(g_socket, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) {
      if (!Reset()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    uint32_t ip = ntohl(from.sin_addr.s_addr);
    uint16_t port = ntohs(from.sin_port);
    if (g_box_ip) {  // (test aid: only what the simulated router lets in, from its real sender)
      if (ip != g_box_ip || port != g_box_port || n < 6) continue;
      const int wrap = n >= 22 && Be32(buf.data()) == kBoxV6 ? 22 : 6;
      if (wrap == 22) ip = V6Number(buf.data() + 6);
      else ip = Be32(buf.data());
      port = Be16(buf.data() + 4);
      n -= wrap;
      std::memmove(buf.data(), buf.data() + wrap, size_t(n));
    }
    // (the relay over UDP: its frames without the length)
    if (n >= 6 && Be32(buf.data()) == kRelayMagic && ip == g_udp_relay_ip && port == g_udp_relay_port) {
      if (buf[5] == kWelcome && g_udp_welcome_ms.exchange(NowMs()) == 0) {
        REXLOG_INFO("p2p: relay over UDP up");
      }
      if (buf[5] == kWelcome && n >= 12) {  // (this socket's address as the VPS sees it)
        g_edge_ip = Be32(buf.data() + 6), g_edge_port = Be16(buf.data() + 10), g_edge_ms = NowMs();
      }
      OnRelay(buf.data(), size_t(n));
      continue;
    }
    // Test aid: SVR2011_TEST_NET_NO_DIRECT=1 - nothing comes in directly (a router that blocks it): the relay
    // only, as for a player whose direct connection fails (the relay warning's tests).
    static const bool no_direct = [] {
      const char* v = std::getenv("SVR2011_TEST_NET_NO_DIRECT");
      return v && v[0] == '1';
    }();
    if (no_direct) continue;
    if (!OnStun(buf.data(), size_t(n))) {
      OnPacket(buf.data(), size_t(n), {ip, port, false});
    }
  }
}

// The IPv6 socket's: as the other's, with their sender's address as its number.
void Receive6() {
  rex::thread::set_current_thread_name("p2p receive 6");
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in6 from = {};
    socklen_t len = sizeof(from);
    const int n = int(recvfrom(g_socket6, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) {
      if (!Reset()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    static const bool no_direct = [] {
      const char* v = std::getenv("SVR2011_TEST_NET_NO_DIRECT");
      return v && v[0] == '1';
    }();
    if (no_direct) continue;
    if (const uint32_t ip = V6Number(reinterpret_cast<const uint8_t*>(&from.sin6_addr)))
      OnPacket(buf.data(), size_t(n), {ip, ntohs(from.sin6_port), false});
  }
}

// This game's IPv6 address: the one the system would send from (asked by a connect on a UDP socket: nothing is sent);
// a temporary one where it has them. None behind a tunnel or with no IPv6 on the internet. Test aid
// SVR2011_TEST_V6=<address>: that one.
void FindMyV6() {
  if (!REXCVAR_GET(p2p_ipv6)) return;
  uint32_t mine = 0;
  if (const char* t = std::getenv("SVR2011_TEST_V6"); t && *t) {
    in6_addr a{};
    if (inet_pton(AF_INET6, t, &a) == 1) mine = V6Number(reinterpret_cast<const uint8_t*>(&a));
  } else if (g_socket6 != kBadSocket) {
    Socket s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s != kBadSocket) {
      sockaddr_in6 to = {};
      to.sin6_family = AF_INET6;
      to.sin6_port = htons(53);
      inet_pton(AF_INET6, "2001:4860:4860::8888", &to.sin6_addr);
      sockaddr_in6 me = {};
      socklen_t len = sizeof(me);
      if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
          getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && GlobalV6(reinterpret_cast<const uint8_t*>(&me.sin6_addr)))
        mine = V6Number(reinterpret_cast<const uint8_t*>(&me.sin6_addr));
      CloseSocket(s);
    }
  }
  if (g_my_v6.exchange(mine) != mine) REXLOG_INFO("p2p: IPv6 {}", mine ? "on" : "off (no address on the internet)");
}

// The relay's frames as they come (the pipe's thread).
void OnPipeData(const uint8_t* data, size_t size) {
  static std::vector<uint8_t> buf;  // (one pipe at a time)
  if (!data) {
    buf.clear();
    if (g_relay_up.exchange(false)) REXLOG_INFO("p2p: relay down");
    return;
  }
  buf.insert(buf.end(), data, data + size);
  size_t at = 0;
  while (buf.size() - at >= 2 && buf.size() - at >= 2u + Be16(buf.data() + at)) {
    const uint16_t len = Be16(buf.data() + at);
    if (len >= 6 && Be32(buf.data() + at + 2) == kRelayMagic) OnRelay(buf.data() + at + 2, len);
    at += 2u + len;
  }
  buf.erase(buf.begin(), buf.begin() + ptrdiff_t(at));
}

// A JSON string.
std::string JsonQuote(const std::string& v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    if (uint8_t(c) >= 0x20) out += c;
  }
  return out + "\"";
}

// The sessions this game hosts, for the server's live view (the dashboard):
// every 20 s while it hosts - players and their paths, the match picked, lobby
// or match - and "ended" when the session closes.
void ReportLive(bool signed_in) {
  static Clock::time_point last{};
  static std::set<uint64_t> reported;
  const auto now = Clock::now();
  if (!signed_in || now - last < std::chrono::seconds(20)) return;
  last = now;
  std::vector<std::string> bodies;
  std::set<uint64_t> hosting;
  {
    std::lock_guard lock(g_mutex);
    const bool in_match = NetStatsNow().active;
    std::map<uint32_t, std::string> names;
    for (const auto& l : NetLinks()) names[l.peer] = l.name;
    for (const auto& [object, local] : g_sessions) {
      // (a match session: not the ONLINE menus' presence session, flags 707 with one slot)
      if (!local.host || local.session.public_slots + local.session.private_slots < 2) continue;
      hosting.insert(local.session.id);
      std::string players = "{\"name\": " + JsonQuote(rex::cvar::Query<std::string>("online_name")) + ", \"path\": \"host\"}";
      for (const auto& [mac, p] : g_peers) {
        if (now - p.game_traffic > std::chrono::seconds(15)) continue;  // (this session's players)
        const auto n = names.find(p.fake);
        // (its relay id too: a peer with no name here yet - an older game's, or one whose first ping hasn't
        // come - is named on the dashboard from the relay's account for it)
        players += ", {\"name\": " + JsonQuote(n != names.end() && !n->second.empty() ? n->second : "?") +
                   ", \"path\": \"" + (p.relayed ? "relay" : p.tunnel ? "direct" : "lan") + "\", \"id\": \"" +
                   fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]) + "\"}";
      }
      uint32_t rule = OnlineCreateSessionRule(), arena = ~0u, played = ~0u;
      LastMatch(&played, &arena);
      if (in_match && played != ~0u) rule = played;
      // (a custom arena plays on a stock arena's number: its own name too - the manifest's name=, else its folder)
      std::string arena_mod;
      if (int tile = -1; in_match && !(arena_mod = ArenaModInUse(&tile)).empty()) {
        std::istringstream manifest(ArenaModManifest());
        for (std::string line; std::getline(manifest, line);)
          if (line.rfind("name=", 0) == 0) {
            arena_mod = line.substr(5);
            while (!arena_mod.empty() && (arena_mod.back() == '\r' || arena_mod.back() == ' ')) arena_mod.pop_back();
            break;
          }
      }
      // (Ranked or Player Match: the session's X_CONTEXT_GAME_TYPE as it was made, 0 = ranked; "" not known)
      const auto game_type = local.session.contexts.find(kContextGameType);
      const char* match_kind =
          game_type == local.session.contexts.end() ? "" : game_type->second == 0 ? "ranked" : "player";
      bodies.push_back(fmt::format(
          "{{\"session\": \"{:X}\", \"match_kind\": \"{}\", \"state\": \"{}\", \"rule\": \"{}\", \"arena\": \"{}\", \"arena_mod\": {}, "
          "\"slots\": {}, \"version\": {}, \"network\": \"{}\", \"players\": [{}]}}",
          local.session.id, match_kind, in_match ? "match" : "lobby", rule == ~0u ? "" : fmt::format("rule {:02X}", rule),
          in_match && arena != ~0u ? fmt::format("arena {}", arena) : "", JsonQuote(arena_mod),
          local.session.public_slots + local.session.private_slots, JsonQuote(PortVersion()), NetName(g_match_net),
          players));
    }
  }
  for (const uint64_t id : reported)
    if (!hosting.count(id)) bodies.push_back(fmt::format("{{\"session\": \"{:X}\", \"ended\": true}}", id));
  reported = std::move(hosting);
  for (const std::string& b : bodies)
    net::ServerRequest("POST", "/api/live", b, {{"Content-Type", "application/json"}});
}

// STUN now and then; the relay's pipe (signed in) while the player is in
// online mode (P2POnlineMode: the ONLINE menus, a session, or either in the
// last 30 s) - registered every 15 s (which also keeps its NAT's mapping
// open). Each pipe is two of the server's connections: games outside online
// play don't hold any.
void Keepalive() {
  rex::thread::set_current_thread_name("p2p keepalive");
  using namespace std::chrono_literals;
  const bool relay = REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty();
  const std::string stun = REXCVAR_GET(p2p_stun);
  char id[13];
  std::snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", g_me.mac[0], g_me.mac[1], g_me.mac[2], g_me.mac[3],
                g_me.mac[4], g_me.mac[5]);
  Clock::time_point tried{}, registered{}, stunned{}, stun_sent{}, stun_resolved{}, ticketed{}, udp_registered{};
  std::pair<uint32_t, uint16_t> stun_to{0, 0};
  int stun_tries = 0;
  int64_t stun_sent_ms = 0;
  bool was_wanted = false;
  Clock::time_point v6_checked{};
  std::vector<uint8_t> ticket;  // (the relay over UDP)
  for (;;) {
    const auto now = Clock::now();
    {
      std::lock_guard lock(g_mutex);
      if (!g_sessions.empty()) g_online_at = NowMs();
    }
    const bool wanted = relay && P2POnlineMode();
    std::shared_ptr<net::Pipe> pipe;
    {
      std::lock_guard lock(g_pipe_mutex);
      pipe = g_pipe;
    }
    const bool open = pipe && pipe->Open();
    if (wanted && !open && (tried == Clock::time_point{} || now - tried >= 30s)) {  // (again every 30 s while down)
      tried = now;
      auto opened = net::OpenPipe(std::string("/api/relay/") + id, OnPipeData);
      std::lock_guard lock(g_pipe_mutex);
      g_pipe = std::move(opened);
      registered = {};
    } else if (!wanted && pipe) {
      pipe->Close();
      std::lock_guard lock(g_pipe_mutex);
      g_pipe.reset();
      tried = {};
      REXLOG_INFO("p2p: relay closed (not online)");
    }
    // STUN: on entering online and every 25 s while online (a router drops a quiet socket's mapping within a minute
    // or two and may pick another port: the port peers are given must be this one), every 5 minutes otherwise; one
    // that isn't answered again after 3 s, twice.
    if (!stun.empty() && stun != "off") {
      const bool cycle = stunned == Clock::time_point{} || now - stunned >= (wanted ? 25s : 300s) || (wanted && !was_wanted);
      const bool again = !cycle && stun_tries < 3 && g_stun_ms.load() < stun_sent_ms && now - stun_sent >= 3s;
      if (cycle) stunned = now, stun_tries = 0;
      if (cycle || again) {
        ++stun_tries;
        stun_sent = now;
        stun_sent_ms = NowMs();
        if (!stun_to.first || now - stun_resolved >= 300s) stun_to = Resolve(stun, 3478), stun_resolved = now;
        if (stun_to.first) {
          uint8_t req[20] = {0, 1, 0, 0};
          Put32(req + 4, kStunCookie);
          for (int i = 8; i < 20; ++i) req[i] = uint8_t(NewNonce());
          SendOn(g_socket, stun_to.first, stun_to.second, req, sizeof(req));
        }
      }
    }
    // (its address: a temporary one changes now and then)
    if (wanted && (!was_wanted || now - v6_checked >= 60s)) v6_checked = now, FindMyV6();
    was_wanted = wanted;
    if (wanted && (registered == Clock::time_point{} || now - registered >= 15s)) {
      registered = now;
      ToPipe(kRegister, g_me.mac.data(), nullptr, 0);
    }
    // The relay over UDP: a ticket (again after 30 minutes; a server without
    // UDP answers 404: asked again in 10), then registered every 5 s.
    if (wanted && (ticketed == Clock::time_point{} || now - ticketed >= (ticket.empty() ? 600s : 1800s))) {
      ticketed = now;
      ticket.clear();
      if (auto r = net::ServerRequest("GET", std::string("/api/relay/") + id + "/udp"); r && r->status == 200) {
        const std::string& b = r->body;
        const size_t p = b.find("\"port\""), t = b.find("\"ticket\"");
        if (p != std::string::npos && t != std::string::npos) {
          const int udp_port = std::atoi(b.c_str() + b.find(':', p) + 1);
          const size_t q = b.find('"', b.find(':', t) + 1);
          const std::string hex = q == std::string::npos ? "" : b.substr(q + 1, 32);
          std::string host = rex::cvar::Query<std::string>("online_server");
          if (const size_t s = host.find("://"); s != std::string::npos) host.erase(0, s + 3);
          host = host.substr(0, host.find('/'));
          host = host.substr(0, host.find(':'));
          const auto [ip, port] = Resolve(host, uint16_t(udp_port));
          if (ip && port && hex.size() == 32) {
            for (size_t k = 0; k < 16; ++k) ticket.push_back(uint8_t(std::stoul(hex.substr(2 * k, 2), nullptr, 16)));
            g_udp_relay_ip = ip, g_udp_relay_port = port;
            REXLOG_INFO("p2p: relay over UDP at port {}", port);
          }
        }
      }
    }
    if (wanted && ticket.size() == 16 && g_udp_relay_ip && (udp_registered == Clock::time_point{} || now - udp_registered >= 5s)) {
      udp_registered = now;
      uint8_t reg[6 + 6 + 16];
      Put32(reg, kRelayMagic), reg[4] = 1, reg[5] = kRegister;
      std::memcpy(reg + 6, g_me.mac.data(), 6);
      std::memcpy(reg + 12, ticket.data(), 16);
      SendOn(g_socket, g_udp_relay_ip, g_udp_relay_port, reg, sizeof(reg));
    }
    if (!wanted && g_udp_welcome_ms) g_udp_welcome_ms = 0, ticket.clear(), ticketed = {};
    ReportLive(relay);
    std::this_thread::sleep_for(1s);
  }
}

// Sends `request` (`send`) and collects the answers for `wait`.
std::vector<std::vector<uint8_t>> Ask(uint32_t nonce, const std::function<void()>& send,
                                      std::chrono::milliseconds wait, size_t enough = SIZE_MAX) {
  {
    std::lock_guard lock(g_wait_mutex);
    g_replies[nonce];
  }
  send();
  std::unique_lock lock(g_wait_mutex);
  g_wait.wait_for(lock, wait, [&] { return g_replies[nonce].size() >= enough; });
  auto replies = std::move(g_replies[nonce]);
  g_replies.erase(nonce);
  return replies;
}

// The places a search asks: the LAN, this PC, p2p_peers (ports: the bases)
// and everyone on the relay.
std::vector<std::pair<uint32_t, uint16_t>> SearchTargets() {
  std::vector<std::pair<uint32_t, uint16_t>> targets;
  if (ForceRelay()) return targets;
  for (int i = 0; i < kInstances; ++i) {
    const uint16_t port = uint16_t(kBasePort + 100 * i);
    targets.emplace_back(0xFFFFFFFF, port);
    targets.emplace_back(0x7F000001, port);
  }
  std::stringstream list(REXCVAR_GET(p2p_peers));
  for (std::string item; std::getline(list, item, ',');) {
    if (const auto target = Resolve(item, kBasePort); target.first) targets.push_back(target);
  }
  return targets;
}

std::vector<Session> Search() {
  g_online_at = NowMs();
  {
    std::lock_guard lock(g_hidden_mutex);  // (this search's own: P2PHiddenReason)
    g_hidden_why.clear(), g_hidden_ids.clear();
  }
  if (REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty()) {
    for (int i = 0; i < 30 && !g_relay_up; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const uint32_t nonce = NewNonce();
  const auto request = Header(kSearch, nonce);
  const auto targets = SearchTargets();
  auto replies = Ask(nonce, [&] {
    for (const auto& [ip, port] : targets) SendTo(ip, port, request);
    if (g_relay_up) ToRelay(Mac{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, request.data(), request.size());
  }, std::chrono::milliseconds(g_relay_up ? 1200 : 700));
  std::map<uint64_t, Session> found;
  size_t dlc_hidden = 0;
  std::map<std::string, size_t> dlc_why;  // (short reason -> sessions)
  for (const auto& reply : replies) {
    Reader r{reply.data(), reply.size()};
    const uint16_t count = r.u16();
    std::vector<Session> got;
    for (uint16_t i = 0; i < count && r.ok && i < 25; ++i) {
      Session s;
      if (!ReadSession(r, s)) break;
      got.push_back(std::move(s));
    }
    if (got.empty()) continue;
    // (the host's version and mods: others - its sessions left out, both told)
    if (const std::string why = Mismatch(r); !why.empty()) {
      REXLOG_INFO("p2p: {} session(s) left out - {}", got.size(), why);
      if (!t_dlc_short.empty()) dlc_hidden += got.size(), dlc_why[t_dlc_short] += got.size();
      std::vector<uint64_t> ids;
      for (const auto& s : got) ids.push_back(s.id);
      Hidden(why, ids);
      TellModsRefused(got.front().host.data());
      continue;
    }
    for (auto& s : got) found[s.id] = std::move(s);
  }
  // (some sessions shown, others hidden for their DLC: the game's own box only comes when none show - say it here;
  // the search repeats every few seconds: once a minute for the same reason)
  if (dlc_hidden && !found.empty()) {
    static std::string told;
    static std::chrono::steady_clock::time_point told_at{};
    std::string top;
    size_t most = 0;
    for (const auto& [k, n] : dlc_why)
      if (n > most) most = n, top = k;
    const auto now = std::chrono::steady_clock::now();
    if (top != told || now - told_at > std::chrono::minutes(1)) {
      told = top, told_at = now;
      REXLOG_INFO("p2p: search: {} session(s) hidden for their DLC ({})", dlc_hidden, top);
      svr2011::OnlineOverlayNotice(std::to_string(dlc_hidden) + (dlc_hidden == 1 ? " SESSION" : " SESSIONS") +
                                       " HIDDEN: " + top,
                                   top.rfind("THEY LACK", 0) == 0 ? "THEY CAN'T PLAY YOUR DLC - THE OTHERS SHOW BELOW"
                                                                  : "INSTALL IT FROM THE DLC TAB TO JOIN THEM", 8);
    }
  }
  std::vector<Session> sessions;
  std::lock_guard lock(g_mutex);
  for (auto& [id, s] : found) {
    Learn(s.host.data());
    g_found[id] = s;
    sessions.push_back(std::move(s));
  }
  return sessions;
}

// Tells a host this game is coming (so its packets are recognised).
void Hello(const uint8_t* host_xnaddr) {
  Path path;
  bool list = false;
  {
    std::lock_guard lock(g_mutex);
    Peer& p = Learn(host_xnaddr);
    path = PathOf(MacOf(host_xnaddr), p);
    if (p.tunnel && !ForceRelay()) {
      const auto now = Clock::now();
      BeginProbing(p, now);
      p.candidates_sent = now;
      list = true;
    }
  }
  const uint32_t nonce = NewNonce() | 1;
  const auto hello = Header(kHello, nonce);
  // (both ways: directly, which may be dropped by its router, and through the
  // relay, so it learns of this game and answers either way)
  Path direct = path;
  direct.relayed = false;
  for (int i = 0; i < 3; ++i) Send(direct, hello);
  if (g_relay_up && !ForceRelay()) ToRelay(path.mac, hello.data(), hello.size());
  else if (path.relayed) Send(path, hello);
  if (list) SendCandidates(path);
}

// Tells a host its sessions were left out (the mods differ): this game's set.
void TellModsRefused(const uint8_t* host_xnaddr) {
  Path path;
  {
    std::lock_guard lock(g_mutex);
    path = PathOf(MacOf(host_xnaddr), Learn(host_xnaddr));
  }
  Writer w;
  w.b = Header(kModsRefused, NewNonce());
  WriteMods(w);
  Send(path, w.b);
}

// -- XNet ------------------------------------------------------------------------------

uint32_t TitleXnAddr(uint8_t* out) {
  const XnAddr a = MyXnAddr();
  std::memcpy(out, a.data(), 36);
  // XNET_GET_XNADDR_STATIC | GATEWAY | DNS | ONLINE
  return 0x04 | 0x20 | 0x40 | 0x80;
}

uint32_t XnAddrToInAddr(const uint8_t* xnaddr) {
  if (MacOf(xnaddr) == g_me.mac) return 0x7F000001;
  bool known;
  uint32_t fake;
  {
    std::lock_guard lock(g_mutex);
    known = g_peers.count(MacOf(xnaddr)) != 0;
    fake = Learn(xnaddr).fake;
  }
  if (!known) Hello(xnaddr);
  return fake;
}

bool InAddrToXnAddr(uint32_t ina, uint8_t* out) {
  std::lock_guard lock(g_mutex);
  auto it = g_fake.find(ina);
  if (it == g_fake.end()) return false;
  std::memcpy(out, g_peers[it->second].xnaddr.data(), 36);
  return true;
}

// (the first few of each way: SVR2011_P2P_TRACE=1 logs them all)
void Trace(const char* way, uint32_t a, uint16_t ap, uint32_t b, uint16_t bp) {
  static const bool all = std::getenv("SVR2011_P2P_TRACE") != nullptr;
  static std::atomic<int> count{0};
  if (all || ++count <= 40) REXLOG_INFO("p2p: {} {}:{} -> {}:{}", way, Ip(a), ap, Ip(b), bp);
}

void ToHost(uint32_t& addr, uint16_t& port) {
  if ((addr & 0xFFFF0000) != kFakeNet) return;
  std::lock_guard lock(g_mutex);
  auto it = g_fake.find(addr);
  if (it == g_fake.end()) {
    Trace("send to unknown", addr, port, addr, port);
    return;
  }
  const Peer& p = g_peers[it->second];
  const uint32_t a = addr;
  const uint16_t ap = port;
  if (p.tunnel) {
    if (Proxy* proxy = ProxyFor(it->second, port)) addr = 0x7F000001, port = proxy->local;
    Trace(p.relayed ? "send (tunnel, relay)" : "send (tunnel)", a, ap, Be32(p.xnaddr.data()), ap);
    return;
  }
  addr = p.route;
  port = uint16_t(port + Offset(p.base));
  Trace("send", a, ap, addr, port);
}

void ToGuest(uint32_t& addr, uint16_t& port) {
  std::lock_guard lock(g_mutex);
  if (addr == 0x7F000001) {
    if (auto it = g_proxy_ports.find(port); it != g_proxy_ports.end()) {  // (a tunnelled peer)
      const Proxy* proxy = it->second;
      addr = g_peers[proxy->mac].fake;
      port = proxy->remote;
      return;
    }
  }
  for (const auto& [mac, p] : g_peers) {
    if (p.route != addr || p.tunnel) continue;
    const uint16_t guest = uint16_t(port - Offset(p.base));
    // (two games on one PC: the one whose port base makes it one of the game's ports)
    if (g_bound.empty() || g_bound.count(guest)) {
      Trace("recv", addr, port, p.fake, guest);
      addr = p.fake;
      port = guest;
      return;
    }
  }
  if (port != 53 && addr != 0x7F000001) Trace("recv from unknown", addr, port, addr, port);
}

uint16_t BindPort(uint16_t port) {
  std::lock_guard lock(g_mutex);
  g_bound.insert(port);
  return uint16_t(port + Offset(g_me.base));
}

uint16_t GuestPort(uint16_t port) {
  std::lock_guard lock(g_mutex);
  const uint16_t guest = uint16_t(port - Offset(g_me.base));
  return g_bound.count(guest) ? guest : port;
}

// The QoS data a match session's host listens with is the game's session attribute block (154 bytes: a 10-byte
// head, then word 0 and attribute k at word k + 1 - the session list's details and the join check read it), so
// the DLC value (attribute 5, kDlcProperty's) is in it too, at byte 34: 113 there as well, like CarryDlcProperty.
// (SVR2011_TEST_DLC_PROPERTY: an older host's value here too.)
constexpr size_t kQosBlockSize = 154, kQosDlcAt = 10 + 6 * 4;
void CarryDlcQos(std::vector<uint8_t>& data, uint64_t id) {
  if (data.size() != kQosBlockSize || std::getenv("SVR2011_TEST_DLC_KEEP")) return;
  const uint32_t value = Be32(data.data() + kQosDlcAt);
  if (value == kDlcValue) return;
  REXLOG_INFO("p2p: session {:016X}'s QoS DLC value was {} - this game carries {}", id, value, kDlcValue);
  Put32(data.data() + kQosDlcAt, kDlcValue);
}

uint32_t QosListen(const uint8_t* xnkid, const uint8_t* data, uint32_t size, uint32_t flags) {
  const uint64_t id = Be64(xnkid);
  std::lock_guard lock(g_mutex);
  if (flags & 0x10) {  // XNET_QOS_LISTEN_RELEASE
    g_qos.erase(id);
  } else if ((flags & 0x04) && data) {  // SET_DATA
    g_qos[id].assign(data, data + std::min<uint32_t>(size, 1024));
    if (const char* t = std::getenv("SVR2011_TEST_DLC_PROPERTY"); t && *t && size == kQosBlockSize)
      Put32(g_qos[id].data() + kQosDlcAt, uint32_t(std::strtoul(t, nullptr, 10)));  // (CarryDlcProperty's test aid)
  } else {
    g_qos.try_emplace(id);
  }
  return 0;
}

std::vector<rex::kernel::xam::OnlineHooks::QosResult> QosLookup(
    const std::vector<rex::kernel::xam::OnlineHooks::QosTarget>& targets) {
  std::vector<rex::kernel::xam::OnlineHooks::QosResult> results(targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    Path path;
    {
      std::lock_guard lock(g_mutex);
      path = PathOf(MacOf(targets[i].xnaddr.data()), Learn(targets[i].xnaddr.data()));
    }
    const uint32_t nonce = NewNonce();
    auto request = Header(kQosRequest, nonce);
    request.insert(request.end(), targets[i].xnkid.begin(), targets[i].xnkid.end());
    const auto t0 = Clock::now();
    auto replies = Ask(nonce, [&] { Send(path, request); }, std::chrono::milliseconds(path.relayed ? 1500 : 800), 1);
    if (replies.empty()) continue;
    Reader r{replies[0].data(), replies[0].size()};
    r.u64();
    const uint16_t len = r.u16();
    auto& out = results[i];
    out.contacted = true;
    out.rtt_ms = uint16_t(std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
    if (len && r.need(len)) out.data.assign(r.p + r.at, r.p + r.at + len);
    CarryDlcQos(out.data, Be64(targets[i].xnkid.data()));
  }
  return results;
}

// -- XGI session messages (app 0xFB) -------------------------------------------------

constexpr uint32_t kOk = 0, kInsufficientBuffer = 122;

Property ReadProperty(uint32_t address) {
  const uint8_t* b = Guest(address);
  Property p{Be32(b), b[8], {}};
  if (p.type == 4 || p.type == 6) {  // (string, binary)
    const uint32_t size = std::min<uint32_t>(Be32(b + 16), 4096), ptr = Be32(b + 20);
    if (size && ptr) p.value.assign(Guest(ptr), Guest(ptr) + size);
  } else {
    const size_t size = (p.type == 2 || p.type == 3 || p.type == 7) ? 8 : 4;
    p.value.assign(b + 16, b + 16 + size);
  }
  return p;
}

// XSESSION_INFO: XNKID, the host's XNADDR, its key (none here).
void WriteInfo(uint8_t* out, const Session& s) {
  std::memset(out, 0, 60);
  Put64(out, s.id);
  std::memcpy(out + 8, s.host.data(), 36);
}

// XSESSION_SEARCHRESULT_HEADER + results (92 bytes each) + their contexts and
// properties, in the caller's buffer.
uint32_t WriteResults(uint32_t address, uint32_t capacity, const std::vector<Session>& sessions) {
  size_t need = 8 + sessions.size() * 92;
  for (const auto& s : sessions) {
    need += s.contexts.size() * 8 + s.properties.size() * 24;
    for (const auto& p : s.properties) {
      if (p.type == 4 || p.type == 6) need += (p.value.size() + 3) & ~size_t(3);
    }
  }
  if (capacity < need) return kInsufficientBuffer;
  uint8_t* b = Guest(address);
  std::memset(b, 0, need);
  Put32(b, uint32_t(sessions.size()));
  Put32(b + 4, sessions.empty() ? 0 : address + 8);
  size_t tail = 8 + sessions.size() * 92;
  for (size_t i = 0; i < sessions.size(); ++i) {
    const Session& s = sessions[i];
    uint8_t* row = b + 8 + i * 92;
    WriteInfo(row, s);
    Put32(row + 60, s.public_slots - std::min(s.filled_public, s.public_slots));
    Put32(row + 64, s.private_slots - std::min(s.filled_private, s.private_slots));
    Put32(row + 68, s.filled_public), Put32(row + 72, s.filled_private);
    Put32(row + 76, uint32_t(s.properties.size())), Put32(row + 80, uint32_t(s.contexts.size()));
    if (!s.contexts.empty()) Put32(row + 88, uint32_t(address + tail));
    for (const auto& [id, value] : s.contexts) Put32(b + tail, id), Put32(b + tail + 4, value), tail += 8;
    if (!s.properties.empty()) Put32(row + 84, uint32_t(address + tail));
    size_t record = tail;
    tail += s.properties.size() * 24;
    for (const auto& p : s.properties) {
      Put32(b + record, p.id);
      b[record + 8] = p.type;
      if (p.type == 4 || p.type == 6) {
        Put32(b + record + 16, uint32_t(p.value.size()));
        Put32(b + record + 20, p.value.empty() ? 0 : uint32_t(address + tail));
        std::copy(p.value.begin(), p.value.end(), b + tail);
        tail += (p.value.size() + 3) & ~size_t(3);
      } else {
        std::copy(p.value.begin(), p.value.end(), b + record + 16);
      }
      record += 24;
    }
  }
  return kOk;
}

uint64_t NewSessionId() {
  std::random_device rd;
  return (uint64_t(rd()) << 32 | rd()) & 0x00FFFFFFFFFFFFFFull | 0xAE00000000000000ull;
}

std::optional<uint32_t> Xgi(uint32_t message, uint32_t buffer, uint32_t length) {
  if (!buffer) return std::nullopt;
  if (message >= 0xB0010 && message <= 0xB001C) g_online_at = NowMs();  // (session messages: playing online)
  const uint8_t* b = Guest(buffer);
  auto u32 = [&](size_t at) { return Be32(b + at); };
  if (message != 0xB0006 && message != 0xB0007) {
    static std::mutex seen_mutex;
    static std::map<uint32_t, int> seen;
    std::lock_guard lock(seen_mutex);
    if (++seen[message] <= 20) {
      REXLOG_INFO("p2p: XGI {:05X} ({} bytes) {:08X} {:08X} {:08X} {:08X}", message, length, u32(0), u32(4),
                  length >= 12 ? u32(8) : 0, length >= 16 ? u32(12) : 0);
    }
  }
  switch (message) {
    case 0xB0006: {  // XUserSetContext: kept for the sessions this game hosts
      std::lock_guard lock(g_mutex);
      if (auto c = g_contexts.find(u32(16)); c == g_contexts.end() || c->second != u32(20))
        REXLOG_INFO("p2p: context {:08X} = {}", u32(16), u32(20));
      g_contexts[u32(16)] = u32(20);
      return std::nullopt;
    }
    case 0xB0007: {  // XUserSetProperty
      Property p{u32(16), uint8_t(u32(16) >> 28), {}};
      const uint32_t size = std::min<uint32_t>(u32(20), 4096), ptr = u32(24);
      if (size && ptr) p.value.assign(Guest(ptr), Guest(ptr) + size);
      std::lock_guard lock(g_mutex);
      if (auto o = g_properties.find(p.id); o == g_properties.end() || o->second.value != p.value) {
        std::string hex;
        for (size_t i = 0; i < std::min<size_t>(p.value.size(), 16); ++i) hex += fmt::format("{:02X}", p.value[i]);
        REXLOG_INFO("p2p: property {:08X} = {}{}", p.id, hex, p.value.size() > 16 ? "..." : "");
      }
      g_properties[p.id] = std::move(p);
      return std::nullopt;
    }
    case 0xB0010: {  // XSessionCreate
      const uint32_t object = u32(0), flags = u32(4), info = u32(20), nonce = u32(24);
      Local local;
      local.host = (flags & 1) != 0;
      Session& s = local.session;
      s.flags = flags, s.public_slots = u32(8), s.private_slots = u32(12);
      if (local.host) {
        (void)LocalCpuTuning();  // (its first read publishes the CPU tab's settings: before the properties are copied)
        s.id = NewSessionId();
        s.nonce = NewSessionId();
        s.host = MyXnAddr();
        std::lock_guard lock(g_mutex);
        s.contexts = g_contexts;
        for (const auto& [id, p] : g_properties) s.properties.push_back(p);
        // (Ranked: the game's own rules only)
        if (auto t = s.contexts.find(kContextGameType); t != s.contexts.end() && t->second == 0)
          s.properties.erase(std::remove_if(s.properties.begin(), s.properties.end(),
                                            [](const Property& p) { return p.id == kPortRules; }),
                             s.properties.end());
        for (const auto& p : s.properties)
          if (p.id == kPortRules && p.value.size() >= 4)
            REXLOG_INFO("p2p: port rules {:08X} (host)", Be32(p.value.data()));
        if (flags & 0x20) {  // (a match's session: the server's match network, for the joiners too)
          uint8_t mode = g_server_net;
          const bool relay = REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty() && g_relay_up;
          if (mode == kNetRelay && !relay) {
            REXLOG_INFO("p2p: match network relay wanted, but this game has no relay (signed out, p2p_relay off or "
                        "the relay not answering): auto");
            mode = kNetAuto;
          }
          s.properties.erase(std::remove_if(s.properties.begin(), s.properties.end(),
                                            [](const Property& p) { return p.id == kNetMode; }),
                             s.properties.end());
          s.properties.push_back(Property{kNetMode, 1, {0, 0, 0, mode}});
          g_match_net = mode;
          REXLOG_INFO("p2p: match network {} (host; the server's {})", NetName(mode), NetName(g_server_net));
          const uint8_t netcode = g_server_netcode;
          s.properties.erase(std::remove_if(s.properties.begin(), s.properties.end(),
                                            [](const Property& p) { return p.id == kNetcode; }),
                             s.properties.end());
          s.properties.push_back(Property{kNetcode, 1, {0, 0, 0, netcode}});
          g_match_netcode = netcode;
          REXLOG_INFO("p2p: netcode {} (host; the server's)", NetcodeName(netcode));
        }
        CarryDlcProperty(s, "the host", true);
        // X_PROPERTY_GAMER_HOSTNAME (LIVE filled it in: the host's name, UTF-16)
        Property name{0x40008109, 4, {}};
        for (char c : rex::cvar::Query<std::string>("online_name")) name.value.push_back(0), name.value.push_back(uint8_t(c));
        name.value.push_back(0), name.value.push_back(0);
        s.properties.push_back(std::move(name));
      } else if (info) {  // (joining: the session the game found)
        s.id = Be64(Guest(info));
        {
          std::lock_guard found_lock(g_mutex);
          uint8_t mode = kNetAuto, netcode = 0;  // (a host without them - an older game: auto, lockstep)
          if (auto f = g_found.find(s.id); f != g_found.end())
            for (const auto& p : f->second.properties) {
              if (p.id == kPortRules && p.value.size() >= 4)
                REXLOG_INFO("p2p: port rules {:08X} (joiner)", Be32(p.value.data()));
              if (p.id == kNetMode && p.value.size() >= 4 && p.value[3] <= kNetP2P) mode = p.value[3];
              if (p.id == kNetcode && p.value.size() >= 4 && p.value[3] <= kNetcodeMax) netcode = p.value[3];
            }
          if (flags & 0x20) {
            g_match_net = mode;
            g_match_netcode = netcode;
            REXLOG_INFO("p2p: match network {}, netcode {} (joiner; the host's)", NetName(mode), NetcodeName(netcode));
          }
        }
        std::memcpy(s.host.data(), Guest(info) + 8, 36);
        Hello(s.host.data());
      }
      if (info && local.host) WriteInfo(Guest(info), s);
      if (nonce) Put64(Guest(nonce), s.nonce);
      REXLOG_INFO("p2p: session {:016X} {} (flags {:X}, {} + {} slots, game type {}, mode {})", s.id,
                  local.host ? "hosted" : "joined", flags, s.public_slots, s.private_slots,
                  s.contexts.count(kContextGameType) ? int(s.contexts.at(kContextGameType)) : -1,
                  s.contexts.count(kContextGameMode) ? int(s.contexts.at(kContextGameMode)) : -1);
      std::lock_guard lock(g_mutex);
      g_sessions[object] = std::move(local);
      UpdateLockstep();
      return kOk;
    }
    case 0xB0011: {  // XSessionDelete
      std::lock_guard lock(g_mutex);
      if (auto it = g_sessions.find(u32(0)); it != g_sessions.end()) {
        g_qos.erase(it->second.session.id);
        REXLOG_INFO("p2p: session {:016X} deleted", it->second.session.id);
        g_sessions.erase(it);
        if (std::none_of(g_sessions.begin(), g_sessions.end(), [](const auto& l) { return (l.second.session.flags & 0x20) != 0; }))
          g_match_net = kNetAuto, g_match_netcode = 0;
        UpdateLockstep();
        return kOk;
      }
      return std::nullopt;
    }
    case 0xB0012:    // XSessionJoinLocal / Remote
    case 0xB0013: {  // XSessionLeaveLocal / Remote
      std::lock_guard lock(g_mutex);
      auto it = g_sessions.find(u32(0));
      if (it == g_sessions.end()) return std::nullopt;
      Local& local = it->second;
      const uint32_t count = std::min<uint32_t>(u32(4), 16), xuids = u32(8), indices = u32(12),
                     privates = message == 0xB0012 ? u32(16) : 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t who = xuids ? Be64(Guest(xuids) + 8 * i) : (g_me.xuid ^ (indices ? Be32(Guest(indices) + 4 * i) : 0));
        const bool priv = privates && Be32(Guest(privates) + 4 * i);
        REXLOG_INFO("p2p: session {:016X} {} {} {:016X}{}", local.session.id, message == 0xB0012 ? "join" : "leave",
                    xuids ? "remote" : "local", who, priv ? " (private)" : "");
        if (message == 0xB0012) {
          (priv ? local.members_private : local.members_public).insert(who);
        } else {
          local.members_public.erase(who), local.members_private.erase(who);
        }
      }
      local.session.filled_public = uint32_t(local.members_public.size());
      local.session.filled_private = uint32_t(local.members_private.size());
      return kOk;
    }
    case 0xB0014:    // XSessionStart
    case 0xB0015: {  // XSessionEnd
      std::lock_guard lock(g_mutex);
      auto it = g_sessions.find(u32(0));
      if (it == g_sessions.end()) return std::nullopt;
      it->second.started = message == 0xB0014;
      return kOk;
    }
    case 0xB0018: {  // XSessionModify
      std::lock_guard lock(g_mutex);
      auto it = g_sessions.find(u32(0));
      if (it == g_sessions.end()) return std::nullopt;
      it->second.session.flags = u32(4);
      it->second.session.public_slots = u32(8);
      it->second.session.private_slots = u32(12);
      UpdateLockstep();
      return kOk;
    }
    case 0xB0016:    // XSessionSearch
    case 0xB001C: {  // XSessionSearchEx
      const uint32_t size = u32(24), results = u32(28);
      if (!results) return std::nullopt;
      // The query's contexts (XUSER_CONTEXT {id, value} x n at u32(20), the
      // count in u32(12)'s low half): a Ranked search finds only ranked
      // sessions and a Player Match search only player ones - the session's
      // game type / mode contexts must be the same (as LIVE's search did).
      std::map<uint32_t, uint32_t> want;
      {
        const uint32_t contexts = u32(20), n = std::min<uint32_t>(u32(12) & 0xFFFF, 16);
        for (uint32_t i = 0; contexts && i < n; ++i) {
          const uint32_t id = Be32(Guest(contexts) + 8 * i), value = Be32(Guest(contexts) + 8 * i + 4);
          if (id == kContextGameType || id == kContextGameMode) want[id] = value;
        }
        static int logged = 0;
        if (logged++ < 10)
          REXLOG_INFO("p2p: search (procedure {}, words {:08X} {:08X} {:08X}, {} contexts{}{})", u32(0), u32(12), u32(16),
                      u32(20), n,
                      want.count(kContextGameType) ? fmt::format(", game type {}", want[kContextGameType]) : "",
                      want.count(kContextGameMode) ? fmt::format(", mode {}", want[kContextGameMode]) : "");
      }
      auto same_kind = [&](const Session& s) {
        for (const auto& [id, value] : want)
          if (auto it = s.contexts.find(id); it != s.contexts.end() && it->second != value) return false;
        return true;
      };
      NoteOnlineActivity(OnlineActivity::kSearch);  // (the presence poll's place: matchmaking)
      auto sessions = Search();
      const size_t before = sessions.size();
      sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const Session& s) { return !same_kind(s); }),
                     sessions.end());
      if (sessions.size() != before)
        REXLOG_INFO("p2p: search: {} session(s) of the other kind (ranked / player) left out", before - sessions.size());
      std::vector<Session> open;
      uint64_t invited = 0;
      {
        std::lock_guard lock(g_mutex);
        if (g_invited && Clock::now() - g_invited_at < std::chrono::minutes(15)) invited = g_invited;
      }
      for (const auto& s : sessions) {
        if (s.id == invited) {
          Session mine = s;  // (its private slots are this game's to take)
          mine.public_slots += mine.private_slots, mine.filled_public += mine.filled_private;
          mine.private_slots = mine.filled_private = 0;
          if (mine.filled_public < mine.public_slots) open.insert(open.begin(), mine);
          REXLOG_INFO("p2p: search: the invited session {:016X}{}", s.id, mine.filled_public < mine.public_slots ? "" : " is full");
        } else if (s.filled_public < s.public_slots) {
          open.push_back(s);
        }
      }
      // Best first (QUICK MATCH takes the game's best of the list): the host this game reached fastest
      // (its last round trip; never pinged: after those), then the one with more players waiting.
      {
        std::lock_guard lock(g_mutex);
        auto rtt = [&](const Session& s) {
          const auto it = g_peers.find(MacOf(s.host.data()));
          if (it == g_peers.end()) return 100000.0f;
          const float r = it->second.relayed ? it->second.rtt_relay : it->second.rtt_direct;
          return r > 0 ? r : 100000.0f;
        };
        const auto from = open.begin() + (invited && !open.empty() && open.front().id == invited ? 1 : 0);
        std::stable_sort(from, open.end(), [&](const Session& a, const Session& b) {
          const float ra = rtt(a), rb = rtt(b);
          if (ra != rb) return ra < rb;
          return a.filled_public > b.filled_public;
        });
      }
      // (an invite's search lists that session alone: the game sorts its list
      // by connection, not by this order - the first row was another session;
      // once listed, the invite is done with and searches are as before)
      if (invited && !open.empty() && open.front().id == invited) {
        open.resize(1);
        std::lock_guard lock(g_mutex);
        g_invited = 0;
      }
      if (open.size() > std::max<uint32_t>(1, u32(8))) open.resize(std::max<uint32_t>(1, u32(8)));
      REXLOG_INFO("p2p: search found {} session(s)", open.size());
      return WriteResults(results, size, open);
    }
    default:
      return LeaderboardsXgi(message, buffer, length);  // (the stats: leaderboards.h)
  }
}

}  // namespace

void SetP2PFileSource(P2PFileSource source) { g_file_source = source; }

P2PRelayPathInfo P2PRelayPath() {
  P2PRelayPathInfo info;
  if (NetStatsNow().active) return info;  // (never in a match)
  info.kind = P2PRelayPathInfo::Kind(g_relay_path.load());
  info.session = g_relay_session.load();
  info.forced_by_server = g_match_net == kNetRelay;
  const std::string nat = NatKind().first;
  info.side = nat == "mapped" || nat == "open" ? 2 : nat == "symmetric" ? 1 : 0;
  info.port = g_me.base;
  info.carrier_nat = g_mapped_ip && !PublicIp(g_mapped_ip);  // (port_map: "behind another one")
  return info;
}

bool P2PSessionInfo(uint8_t* out, uint32_t* slots) {
  std::lock_guard lock(g_mutex);
  const Local* best = nullptr;
  for (const auto& [obj, local] : g_sessions) {
    if (!(local.session.flags & 0x20)) continue;  // (a match's, not the ONLINE menu's presence session)
    if (!best || (local.host && !best->host)) best = &local;
  }
  if (!best) return false;
  WriteInfo(out, best->session);
  if (slots) *slots = best->session.public_slots + best->session.private_slots;
  return true;
}

int P2PSessionRole() {
  std::lock_guard lock(g_mutex);
  int role = 0;
  for (const auto& [obj, local] : g_sessions)
    if (local.session.flags & 0x20) role = local.host ? 1 : role ? role : 2;  // (a match's: the host's wins)
  return role;
}

void P2PSetHostProperty(uint32_t id, uint32_t value) {
  Property p{id, uint8_t(id >> 28), {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)}};
  std::lock_guard lock(g_mutex);
  g_properties[id] = std::move(p);
}

std::optional<uint32_t> P2PSessionProperty(uint32_t id) {
  std::lock_guard lock(g_mutex);
  for (const auto& [obj, local] : g_sessions) {
    if (!(local.session.flags & 0x20)) continue;  // (a match's, not the ONLINE menu's presence session)
    // (a joiner's session has the host's properties from the search)
    const auto found = g_found.find(local.session.id);
    const auto& props = local.host || found == g_found.end() ? local.session.properties : found->second.properties;
    for (const auto& p : props)
      if (p.id == id && p.value.size() >= 4)
        return uint32_t(p.value[0]) << 24 | uint32_t(p.value[1]) << 16 | uint32_t(p.value[2]) << 8 | p.value[3];
  }
  return std::nullopt;
}

void SetPortRules(uint32_t value) {
  if (!value) {
    std::lock_guard lock(g_mutex);
    g_properties.erase(kPortRules);
    return;
  }
  REXLOG_INFO("p2p: port rules {:08X} set (host)", value);
  P2PSetHostProperty(kPortRules, value);
}

std::optional<uint32_t> PortRules() { return P2PSessionProperty(kPortRules); }

void P2PSetServerNetwork(const std::string& mode) {
  const uint8_t m = mode == "relay" ? kNetRelay : mode == "p2p" ? kNetP2P : kNetAuto;
  if (g_server_net.exchange(m) != m) REXLOG_INFO("p2p: the server's match network is {} (new sessions)", NetName(m));
}

void P2PSetServerNetcode(const std::string& mode) {
  const uint8_t m = mode == "rollback" ? 1 : mode == "lockstep_vclock" ? 2 : 0;  // (unknown: lockstep)
  if (g_server_netcode.exchange(m) != m) REXLOG_INFO("p2p: the server's netcode is {} (new sessions)", NetcodeName(m));
}

int P2PNetcode() { return g_match_netcode.load(); }

P2PLobby P2PMatchLobby() {
  P2PLobby out;
  std::lock_guard lock(g_mutex);
  const Local* best = nullptr;
  for (const auto& [obj, local] : g_sessions) {
    if (!(local.session.flags & 0x20)) continue;  // (a match's, not the ONLINE menu's presence session)
    if (!best || (local.host && !best->host)) best = &local;
  }
  if (!best) return out;
  out.in = true, out.host = best->host, out.session = best->session.id;
  for (uint64_t x : best->members_public) out.members.push_back(x);
  for (uint64_t x : best->members_private) out.members.push_back(x);
  return out;
}

void SetP2PModHandler(P2PModHandler handler) { g_mod_handler = handler; }

void SetP2PChatHandler(P2PChatHandler handler) { g_chat_handler = handler; }

void P2PSendChat(const std::vector<uint8_t>& payload) {
  if (g_socket == kBadSocket || payload.size() > 2000) return;
  std::vector<Path> peers;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [mac, p] : g_peers) peers.push_back(PathOf(mac, p));
  }
  Writer w;
  w.b = Header(kChat, NewNonce());
  w.bytes(payload.data(), payload.size());
  for (const auto& p : peers) Send(p, w.b);
}

// -- mod files (mod_transfer.cpp): to one peer, over a direct link only -----------------

void SetP2PModXferHandler(P2PModXferHandler handler) { g_mod_xfer_handler = handler; }

// Direct: a peer on this network, or one outside it whose direct path works now (a direct ping came back in the
// last 10 s, or it was heard from directly) and that isn't sent through the relay. Never in the forced-relay
// test mode.
bool DirectLocked(const Peer& p) {
  if (ForceRelay() || p.relayed) return false;
  if (!p.tunnel) return true;
  const auto now = Clock::now();
  return now - p.direct_ok < std::chrono::seconds(10) || now - p.direct < std::chrono::seconds(10);
}

bool P2PPeerDirect(uint64_t peer) {
  std::lock_guard lock(g_mutex);
  for (const auto& [mac, p] : g_peers)
    if (PeerId(mac) == peer) return DirectLocked(p);
  return false;
}

bool P2PSendModXfer(uint64_t peer, const std::vector<uint8_t>& payload) {
  if (g_socket == kBadSocket || payload.size() > 1400) return false;
  Path path{};
  {
    std::lock_guard lock(g_mutex);
    bool found = false;
    for (const auto& [mac, p] : g_peers)
      if (PeerId(mac) == peer) {
        if (!DirectLocked(p)) return false;
        path = PathOf(mac, p), found = true;
      }
    if (!found) return false;
  }
  Writer w;
  w.b = Header(kModXfer, NewNonce());
  w.bytes(payload.data(), payload.size());
  Send(path, w.b);
  return true;
}

uint64_t P2PSessionHost(uint64_t* session) {
  std::lock_guard lock(g_mutex);
  for (const auto& [object, local] : g_sessions) {
    if (local.host || local.session.public_slots + local.session.private_slots < 2) continue;
    if (session) *session = local.session.id;
    return PeerId(MacOf(local.session.host.data()));
  }
  return 0;
}

bool P2PHosting(uint64_t* session) {
  std::lock_guard lock(g_mutex);
  for (const auto& [object, local] : g_sessions)
    if (local.host && local.session.public_slots + local.session.private_slots >= 2) {
      if (session) *session = local.session.id;
      return true;
    }
  return false;
}

void P2PSendMods(const std::vector<uint8_t>& payload) {
  if (g_socket == kBadSocket || payload.size() > 50000) return;
  std::vector<Path> peers;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [mac, p] : g_peers) peers.push_back(PathOf(mac, p));
  }
  Writer w;
  w.b = Header(kMods, NewNonce());
  w.bytes(payload.data(), payload.size());
  for (const auto& p : peers) Send(p, w.b);
}

std::optional<bool> P2PRanked() {
  std::lock_guard lock(g_mutex);
  const auto t = g_contexts.find(kContextGameType);
  if (t == g_contexts.end()) return std::nullopt;
  return t->second == 0;
}

bool P2POnlineMode() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_sessions.empty()) return true;
  }
  // (the ONLINE menus: the main-menu object's group 0x5EC - the game's presence
  // session there isn't held all the time)
  uint32_t group = 0, row = 0;
  if (ScriptMenuState(&group, &row) && group == 0x5EC) return true;
  const int64_t at = g_online_at;
  return at && NowMs() - at < 30000;  // (between screens the sessions come and go)
}

bool P2PHostLeft() {
  const int64_t at = g_host_left_ms.load();
  return at && NowMs() - at < 60000;
}

std::string P2PHiddenReason(int* sessions) {
  std::lock_guard lock(g_hidden_mutex);
  if (sessions) *sessions = int(g_hidden_ids.size());
  if (g_hidden_ids.empty() || std::chrono::steady_clock::now() - g_hidden_at > std::chrono::minutes(2)) return "";
  return g_hidden_why;
}

void P2PExpectInvite(const uint8_t* info) {
  {
    std::lock_guard lock(g_mutex);
    g_invited = Be64(info);
    g_invited_at = Clock::now();
  }
  REXLOG_INFO("p2p: invited to session {:016X}: first in searches", Be64(info));
}

bool P2PReachHost(const uint8_t* info) {
  {
    std::lock_guard lock(g_hidden_mutex);
    g_hidden_why.clear(), g_hidden_ids.clear();
  }
  const uint8_t* host = info + 8;
  const Mac mac = MacOf(host);
  Path path;
  {
    std::lock_guard lock(g_mutex);
    path = PathOf(mac, Learn(host));
  }
  g_online_at = NowMs();  // (the relay is wanted now)
  if (REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty()) {
    for (int i = 0; i < 30 && !g_relay_up; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const uint32_t nonce = NewNonce();
  const auto request = Header(kSearch, nonce);
  auto replies = Ask(nonce, [&] {
    if (!path.relayed && !ForceRelay()) Send(path, request);
    ToRelay(mac, request.data(), request.size());
  }, std::chrono::milliseconds(2500), 1);
  bool found = false;
  const uint64_t id = Be64(info);
  bool refused = false;
  for (const auto& reply : replies) {
    Reader r{reply.data(), reply.size()};
    const uint16_t count = r.u16();
    std::vector<Session> got;
    for (uint16_t i = 0; i < count && r.ok && i < 25; ++i) {
      Session s;
      if (!ReadSession(r, s)) break;
      got.push_back(std::move(s));
    }
    // (an invite too: only with the same mods)
    if (const std::string why = Mismatch(r); !why.empty()) {
      Hidden(why, {id});
      if (!got.empty()) TellModsRefused(got.front().host.data());
      refused = true;
      continue;
    }
    std::lock_guard lock(g_mutex);
    for (auto& s : got) {
      found |= s.id == id;
      g_found[s.id] = std::move(s);
    }
  }
  if (refused && !found) {
    REXLOG_INFO("p2p: invite: the host's mods differ - not joining");
    return false;
  }
  REXLOG_INFO("p2p: invite: host {} {}", Ip(path.ip), found ? "has the session" : replies.empty() ? "didn't answer" : "no longer has the session");
  return found;
}

std::optional<std::string> P2PFetch(uint8_t kind, const std::string& key, std::chrono::milliseconds timeout) {
  if (g_socket == kBadSocket || key.size() > 200) return std::nullopt;
  FileFetch fetch;
  fetch.kind = kind, fetch.key = key;
  const uint32_t nonce = NewNonce();
  std::vector<Path> peers;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [mac, p] : g_peers) peers.push_back(PathOf(mac, p));
  }
  if (peers.empty()) return std::nullopt;
  {
    std::lock_guard lock(g_wait_mutex);
    g_fetches[nonce] = &fetch;
  }
  const auto ask_all = FileRequest(nonce, kind, key, 0, kNone);
  for (const auto& p : peers) Send(p, ask_all);
  const auto deadline = Clock::now() + timeout;
  auto last = Clock::now();
  uint32_t last_got = 0;
  std::optional<std::string> result;
  std::unique_lock lock(g_wait_mutex);
  while (Clock::now() < deadline) {
    g_wait.wait_for(lock, std::chrono::milliseconds(200));
    if (fetch.known && fetch.got == fetch.have.size()) {
      result = std::move(fetch.data);
      break;
    }
    if (!fetch.known && fetch.refusals >= int(peers.size())) break;  // (nobody has it)
    if (fetch.got != last_got) last = Clock::now(), last_got = fetch.got;
    if (Clock::now() - last < std::chrono::milliseconds(800)) continue;
    last = Clock::now();
    // (what didn't come: asked again, a range at a time)
    std::vector<std::vector<uint8_t>> asks;
    if (!fetch.known) {
      asks.push_back(ask_all);
    } else {
      for (size_t i = 0; i < fetch.have.size() && asks.size() < 32;) {
        if (fetch.have[i]) { ++i; continue; }
        size_t j = i;
        while (j < fetch.have.size() && !fetch.have[j] && j - i < 64) ++j;
        asks.push_back(FileRequest(nonce, kind, key, uint32_t(i * kChunk), uint32_t((j - i) * kChunk)));
        i = j;
      }
    }
    const Path source = fetch.source;
    const bool known = fetch.known;
    lock.unlock();
    for (const auto& a : asks) {
      if (known) {
        Send(source, a);
      } else {
        for (const auto& p : peers) Send(p, a);
      }
    }
    lock.lock();
  }
  g_fetches.erase(nonce);
  lock.unlock();
  REXLOG_INFO("p2p: file {}:{} {}", kind, key,
              result ? fmt::format("fetched ({} bytes)", result->size()) : std::string("not fetched"));
  return result;
}

void InstallP2P(rex::memory::Memory* memory) {
  if (!rex::cvar::Query<bool>("online_enabled") || !REXCVAR_GET(p2p_enabled)) return;
  g_memory = memory;
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  g_me.base = uint16_t(REXCVAR_GET(p2p_port));
  g_me.lan = LanIp();
  if (const std::string a = REXCVAR_GET(p2p_address); !a.empty()) {
    in_addr ina{};
    if (inet_pton(AF_INET, a.c_str(), &ina) == 1) g_me.lan = ntohl(ina.s_addr);
  }
  // The P2P port: the setting's, else the next base other games search
  // (+100, up to 3 more). With none, P2P was off and CREATE SESSION ended in
  // "You have been disconnected" (a 2.1.3 report: UDP 36000 held by another
  // program or a second game). The game's own ports follow the base (Offset).
  const uint16_t wanted = g_me.base;
  for (int k = 0; k < 4; ++k) {
    const uint16_t base = uint16_t(wanted + 100 * k);
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) break;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in at = {};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons(base);
    if (bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0) {
      g_socket = s;
      g_me.base = base;
      break;
    }
    CloseSocket(s);
    REXLOG_WARN("p2p: UDP port {} is in use (another program, or a second game)", base);
  }
  if (g_socket == kBadSocket) {
    REXLOG_ERROR("p2p: no free UDP port from {} on - online sessions can't be made (set p2p_port)", wanted);
    return;
  }
  if (g_me.base != wanted) REXLOG_WARN("p2p: using UDP port {} instead of {}", g_me.base, wanted);
  // the player's online id (and the port base: two games on one PC differ)
  const std::string id = rex::cvar::Query<std::string>("online_xuid") + ":" + std::to_string(g_me.base);
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : id) h = (h ^ c) * 1099511628211ull;
  g_me.mac = {0x02, uint8_t(h >> 32), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h)};
  g_me.xuid = std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16);
  if (const char* box = std::getenv("SVR2011_TEST_NAT_BOX"); box && *box) {
    const auto at = Resolve(box, 0);
    g_box_ip = at.first, g_box_port = at.second;
    REXLOG_INFO("p2p: test: behind the NAT box at {}:{}", Ip(g_box_ip), g_box_port);
  }
  // The IPv6 socket, on the same port (nothing behind the test's NAT box, unless it asks for the real one).
  g_v6_real = std::getenv("SVR2011_TEST_V6_REAL") != nullptr;
  g_v6_test = std::getenv("SVR2011_TEST_V6") != nullptr;
  if (REXCVAR_GET(p2p_ipv6) && (!g_box_ip || g_v6_real)) {
    Socket s6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s6 != kBadSocket) {
      int only = 1;
      setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only));
      sockaddr_in6 at = {};
      at.sin6_family = AF_INET6;
      at.sin6_port = htons(g_me.base);
      if (bind(s6, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0) g_socket6 = s6;
      else CloseSocket(s6);
    }
  }
  std::thread(Receive).detach();
  if (g_socket6 != kBadSocket) std::thread(Receive6).detach();
  std::thread(Keepalive).detach();
  std::thread(PingLoop).detach();
  std::thread(ProbeLoop).detach();
  // The router asked to forward the P2P port (port_map.h; not for a test's
  // second game on this PC, which names its own address - unless it names a test router too)
  if ((REXCVAR_GET(p2p_address).empty() && g_me.lan >> 24 != 127) || std::getenv("SVR2011_TEST_NATPMP"))
    StartPortMapping(g_me.base, g_me.lan, [](uint32_t ip, uint16_t port) {
      g_mapped_ip = ip;
      g_mapped_port = port;
      if (MappedUsable()) {
        if (ip) g_me.online = ip;
        g_me.online_port = port;
      } else {
        REXLOG_INFO("p2p: router port mapping not used - that router is behind another one (it says its outside "
                    "address is {}; players reach this game through the one in front)", Ip(ip));
      }
    });
  rex::kernel::xam::OnlineHooks hooks;
  hooks.title_xnaddr = TitleXnAddr;
  hooks.xnaddr_to_inaddr = XnAddrToInAddr;
  hooks.inaddr_to_xnaddr = InAddrToXnAddr;
  hooks.qos_listen = QosListen;
  hooks.qos_lookup = QosLookup;
  hooks.to_host = ToHost;
  hooks.to_guest = ToGuest;
  hooks.bind_port = BindPort;
  hooks.guest_port = GuestPort;
  hooks.xgi = Xgi;
  rex::kernel::xam::SetOnlineHooks(std::move(hooks));
  REXLOG_INFO("p2p: on, {} port {}{} (id {:02X}{:02X}{:02X}{:02X}{:02X}{:02X})", Ip(g_me.lan), g_me.base, ForceRelay() ? ", relay only" : "", g_me.mac[0],
              g_me.mac[1], g_me.mac[2], g_me.mac[3], g_me.mac[4], g_me.mac[5]);
}

}  // namespace svr2011

