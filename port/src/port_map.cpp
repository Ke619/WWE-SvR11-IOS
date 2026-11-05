// WWE SmackDown vs. Raw 2011 - router port mapping, see port_map.h.
//
// A peer behind a strict NAT can't be reached directly unless its router
// forwards the port: then matches go through the relay (a detour - UK to Asia
// through the relay's country). Most home routers take a mapping request:
// - NAT-PMP (RFC 6886: Apple, many others): UDP to the gateway, port 5351;
// - UPnP IGD: the router found with SSDP (M-SEARCH to 239.255.255.250:1900),
//   its description (HTTP) names the WANIPConnection / WANPPPConnection
//   service, whose control URL takes SOAP AddPortMapping.
// Only the game's own UDP port is mapped, outside = inside, with a 1-hour
// lease renewed every 30 minutes and removed when the game exits (a lease
// rather than a permanent mapping, so nothing stays on the router if the game
// is killed; a router that takes only permanent mappings is left alone).
// Test aids: SVR2011_TEST_NATPMP=<ip:port> (the gateway's NAT-PMP),
// SVR2011_TEST_UPNP=<description URL> (no SSDP search), SVR2011_TEST_NO_NATPMP=1.
#include "port_map.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(p2p_port_mapping, true, "Online",
                    "Ask the router to forward this game's online port (UPnP / NAT-PMP; a 1-hour lease, removed at "
                    "exit) so far-off players connect directly; off: the router is never touched");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kBad = INVALID_SOCKET;
void Close(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kBad = -1;
void Close(Socket s) { close(s); }
#endif

constexpr uint32_t kLease = 3600;  // (seconds; renewed at half)

std::string Ip(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

sockaddr_in Sin(uint32_t ip, uint16_t port) {
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(ip);
  a.sin_port = htons(port);
  return a;
}

void Timeout(Socket s, int ms) {
#if defined(_WIN32)
  DWORD t = DWORD(ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
  timeval t{ms / 1000, (ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

uint32_t Resolve(const std::string& host) {
  addrinfo hints = {}, *found = nullptr;
  hints.ai_family = AF_INET;
  uint32_t ip = 0;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) == 0 && found) {
    ip = ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
  }
  return ip;
}

std::pair<uint32_t, uint16_t> HostPort(const std::string& s, uint16_t default_port) {
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos) return {Resolve(s), default_port};
  return {Resolve(s.substr(0, colon)), uint16_t(std::atoi(s.c_str() + colon + 1))};
}

// -- NAT-PMP ---------------------------------------------------------------------------

// The default gateway (where NAT-PMP asks); 0 not known.
std::pair<uint32_t, uint16_t> NatPmpGateway(uint32_t lan_ip) {
  if (const char* t = std::getenv("SVR2011_TEST_NATPMP")) return HostPort(t, 5351);
  if (std::getenv("SVR2011_TEST_NO_NATPMP") || std::getenv("SVR2011_TEST_UPNP")) return {0, 0};
#if defined(_WIN32)
  MIB_IPFORWARDROW row = {};
  if (GetBestRoute(htonl(0x08080808), 0, &row) == NO_ERROR && row.dwForwardNextHop)
    return {ntohl(row.dwForwardNextHop), 5351};
#endif
  // (elsewhere: the usual x.y.z.1 of a home network)
  return {lan_ip ? (lan_ip & 0xFFFFFF00u) | 1 : 0, 5351};
}

// One request, a reply of at least `want` bytes (retried 4 times, 250 ms
// doubling, as RFC 6886 asks); empty: none.
std::vector<uint8_t> NatPmpAsk(uint32_t gw, uint16_t port, const std::vector<uint8_t>& req, size_t want) {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBad) return {};
  const sockaddr_in to = Sin(gw, port);
  std::vector<uint8_t> buf(64);
  int wait = 250;
  for (int attempt = 0; attempt < 4; ++attempt, wait *= 2) {
    sendto(s, reinterpret_cast<const char*>(req.data()), int(req.size()), 0, reinterpret_cast<const sockaddr*>(&to),
           sizeof(to));
    Timeout(s, wait);
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    const int n = int(recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n >= int(want) && buf[1] == uint8_t(req[1] + 128)) {
      Close(s);
      buf.resize(size_t(n));
      return buf;
    }
  }
  Close(s);
  return {};
}

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Map (lease > 0) or remove (0) UDP `port`; the external port on success.
std::optional<uint16_t> NatPmpMap(uint32_t gw, uint16_t gw_port, uint16_t port, uint32_t lease) {
  const std::vector<uint8_t> req = {0, 1, 0, 0, uint8_t(port >> 8), uint8_t(port), uint8_t(lease ? port >> 8 : 0),
                                    uint8_t(lease ? port : 0), uint8_t(lease >> 24), uint8_t(lease >> 16),
                                    uint8_t(lease >> 8), uint8_t(lease)};
  const auto r = NatPmpAsk(gw, gw_port, req, 16);
  if (r.empty() || Be16(r.data() + 2) != 0) return std::nullopt;
  return Be16(r.data() + 10);
}

uint32_t NatPmpExternal(uint32_t gw, uint16_t gw_port) {
  const auto r = NatPmpAsk(gw, gw_port, {0, 0}, 12);
  return r.empty() || Be16(r.data() + 2) != 0 ? 0 : Be32(r.data() + 8);
}

// -- UPnP IGD --------------------------------------------------------------------------

struct Url {
  uint32_t ip = 0;
  uint16_t port = 80;
  std::string host, path = "/";
};

std::optional<Url> ParseUrl(const std::string& u) {
  if (u.rfind("http://", 0) != 0) return std::nullopt;
  Url url;
  const size_t start = 7, slash = u.find('/', start);
  const std::string hostport = u.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
  url.path = slash == std::string::npos ? "/" : u.substr(slash);
  const size_t colon = hostport.rfind(':');
  url.host = hostport;
  if (colon != std::string::npos) url.port = uint16_t(std::atoi(hostport.c_str() + colon + 1));
  url.ip = Resolve(colon == std::string::npos ? hostport : hostport.substr(0, colon));
  if (!url.ip) return std::nullopt;
  return url;
}

// A plain HTTP/1.1 request on the LAN (3 s timeouts): the status and body.
std::optional<std::pair<int, std::string>> Http(const Url& url, const std::string& method, const std::string& headers,
                                                const std::string& body) {
  Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) return std::nullopt;
  Timeout(s, 3000);
  const sockaddr_in to = Sin(url.ip, url.port);
  if (connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) {
    Close(s);
    return std::nullopt;
  }
  const std::string req = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                          "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
                          headers + "\r\n" + body;
  send(s, req.data(), int(req.size()), 0);
  std::string got;
  char buf[4096];
  for (int n; (n = int(recv(s, buf, sizeof(buf), 0))) > 0 && got.size() < 256 * 1024;) got.append(buf, size_t(n));
  Close(s);
  const size_t sp = got.find(' '), split = got.find("\r\n\r\n");
  if (got.rfind("HTTP/", 0) != 0 || sp == std::string::npos || split == std::string::npos) return std::nullopt;
  return std::make_pair(std::atoi(got.c_str() + sp + 1), got.substr(split + 4));
}

std::string Between(const std::string& s, const std::string& open, const std::string& close, size_t from = 0) {
  const size_t a = s.find(open, from);
  if (a == std::string::npos) return {};
  const size_t b = s.find(close, a + open.size());
  return b == std::string::npos ? std::string() : s.substr(a + open.size(), b - a - open.size());
}

// SSDP: the routers' description URLs (a 2.5 s search). From the adapter the game plays on (with a VPN's or a virtual
// machine's adapter on the PC, the system may pick that one and no router hears it), three times (it's UDP), asking
// for the device and for its WAN services (some routers only answer one of them).
std::vector<std::string> SsdpSearch(uint32_t lan_ip) {
  if (const char* t = std::getenv("SVR2011_TEST_UPNP")) return {t};
  std::vector<std::string> found;
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBad) return found;
  if (lan_ip) {
    in_addr from = {};
    from.s_addr = htonl(lan_ip);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&from), sizeof(from));
  }
  const sockaddr_in to = Sin(0xEFFFFFFA, 1900);  // 239.255.255.250
  const auto ask = [&] {
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                           "urn:schemas-upnp-org:service:WANIPConnection:1",
                           "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
      const std::string req = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                           "MX: 2\r\nST: ") + st + "\r\n\r\n";
      sendto(s, req.data(), int(req.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
  };
  ask();
  int asked = 1;
  const auto start = Clock::now();
  const auto until = start + 2500ms;
  char buf[2048];
  while (Clock::now() < until) {
    if (asked < 3 && Clock::now() - start >= asked * 500ms) ask(), ++asked;
    Timeout(s, 250);
    const int n = int(recv(s, buf, sizeof(buf) - 1, 0));
    if (n <= 0) continue;
    std::string reply(buf, size_t(n)), lower = reply;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lower.find("\nlocation:");
    if (at == std::string::npos) continue;
    size_t a = at + 10;
    while (a < reply.size() && reply[a] == ' ') ++a;
    const size_t b = reply.find_first_of("\r\n", a);
    const std::string loc = reply.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (std::find(found.begin(), found.end(), loc) == found.end()) found.push_back(loc);
  }
  Close(s);
  return found;
}

struct Igd {
  Url control;
  std::string service;  // (its service type, for SOAPAction)
};

// A description's WAN connection service: its control URL.
std::optional<Igd> FindIgd(const std::string& location) {
  const auto loc = ParseUrl(location);
  if (!loc) return std::nullopt;
  const auto desc = Http(*loc, "GET", "", "");
  if (!desc || desc->first != 200) return std::nullopt;
  const std::string& x = desc->second;
  for (const char* type : {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                           "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
    const size_t at = x.find(std::string("<serviceType>") + type + "</serviceType>");
    if (at == std::string::npos) continue;
    std::string control = Between(x, "<controlURL>", "</controlURL>", at);
    if (control.empty()) continue;
    if (control.rfind("http://", 0) != 0) {  // (relative: to URLBase, else the description's host)
      std::string base = Between(x, "<URLBase>", "</URLBase>");
      if (base.empty()) base = "http://" + loc->host;
      while (!base.empty() && base.back() == '/') base.pop_back();
      control = base + (control.front() == '/' ? "" : "/") + control;
    }
    if (auto url = ParseUrl(control)) return Igd{*url, type};
  }
  return std::nullopt;
}

std::optional<std::pair<int, std::string>> Soap(const Igd& igd, const std::string& action, const std::string& args) {
  const std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                           action + " xmlns:u=\"" + igd.service + "\">" + args + "</u:" + action +
                           "></s:Body></s:Envelope>\r\n";
  const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + igd.service + "#" +
                              action + "\"\r\n";
  return Http(igd.control, "POST", headers, body);
}

std::string MappingArgs(uint16_t port) {
  return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
         "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
}

// AddPortMapping with a lease; the UPnP error code on failure (-1: no answer).
int UpnpMap(const Igd& igd, uint16_t port, uint32_t lan_ip) {
  const auto r = Soap(igd, "AddPortMapping",
                      MappingArgs(port) + "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort><NewInternalClient>" +
                          Ip(lan_ip) + "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>SvR2011 online"
                          "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(kLease) + "</NewLeaseDuration>");
  if (!r) return -1;
  if (r->first == 200) return 0;
  const std::string code = Between(r->second, "<errorCode>", "</errorCode>");
  return code.empty() ? r->first : std::atoi(code.c_str());
}

uint32_t UpnpExternal(const Igd& igd) {
  const auto r = Soap(igd, "GetExternalIPAddress", "");
  if (!r || r->first != 200) return 0;
  const std::string ip = Between(r->second, "<NewExternalIPAddress>", "</NewExternalIPAddress>");
  in_addr a = {};
  return !ip.empty() && inet_pton(AF_INET, ip.c_str(), &a) == 1 ? ntohl(a.s_addr) : 0;
}

// -- the mapping held ---------------------------------------------------------------

std::mutex g_mutex;
enum class Kind { kNone, kNatPmp, kUpnp } g_kind = Kind::kNone;
uint32_t g_gw = 0;
uint16_t g_gw_port = 0, g_port = 0;
std::optional<Igd> g_igd;

// At exit: the mapping removed (a killed game leaves it to its lease).
void Remove() {
  std::lock_guard lock(g_mutex);
  if (g_kind == Kind::kNatPmp) {
    NatPmpMap(g_gw, g_gw_port, g_port, 0);
  } else if (g_kind == Kind::kUpnp && g_igd) {
    Soap(*g_igd, "DeletePortMapping", MappingArgs(g_port));
  }
  if (g_kind != Kind::kNone) REXLOG_INFO("p2p: router port mapping removed (UDP {})", g_port);
  g_kind = Kind::kNone;
}

// One attempt: NAT-PMP, then UPnP. True if held.
bool MapOnce(uint16_t port, uint32_t lan_ip, uint32_t* external) {
  const auto [gw, gw_port] = NatPmpGateway(lan_ip);
  if (gw) {
    if (const auto ext = NatPmpMap(gw, gw_port, port, kLease)) {
      if (*ext != port) {  // (outside != inside: the peers would be told the wrong port - give it back)
        NatPmpMap(gw, gw_port, port, 0);
        REXLOG_INFO("p2p: router port mapping: NAT-PMP gave port {} for {} - not used", *ext, port);
      } else {
        std::lock_guard lock(g_mutex);
        g_kind = Kind::kNatPmp, g_gw = gw, g_gw_port = gw_port, g_port = port;
        *external = NatPmpExternal(gw, gw_port);
        return true;
      }
    }
  }
  for (const std::string& location : SsdpSearch(lan_ip)) {
    const auto igd = FindIgd(location);
    if (!igd) continue;
    const int error = UpnpMap(*igd, port, lan_ip);
    if (error == 0) {
      std::lock_guard lock(g_mutex);
      g_kind = Kind::kUpnp, g_igd = igd, g_port = port;
      *external = UpnpExternal(*igd);
      return true;
    }
    // (725: OnlyPermanentLeasesSupported - a permanent mapping would outlive a
    // killed game, so the router is left alone)
    REXLOG_INFO("p2p: router port mapping: UPnP {} refused it (error {}){}", location, error,
                error == 725 ? " - takes only permanent mappings, not used" : "");
  }
  return false;
}

}  // namespace

void StopPortMapping() { Remove(); }

void StartPortMapping(uint16_t port, uint32_t lan_ip, std::function<void(uint32_t, uint16_t)> mapped) {
  if (!REXCVAR_GET(p2p_port_mapping) || !port) return;
  std::thread([port, lan_ip, mapped = std::move(mapped)] {
    uint32_t external = 0;
    if (!MapOnce(port, lan_ip, &external)) {
      REXLOG_INFO("p2p: router port mapping: none (no UPnP / NAT-PMP router answered, or it refused)");
      return;
    }
    {
      std::lock_guard lock(g_mutex);
      REXLOG_INFO("p2p: router port mapping UDP {} -> {}:{} ({}{})", port, Ip(lan_ip), port,
                  g_kind == Kind::kNatPmp ? "NAT-PMP" : "UPnP",
                  external ? fmt::format(", outside address {}", Ip(external)) : std::string());
    }
    std::atexit(Remove);
    if (mapped) mapped(external, port);
    for (;;) {  // (renewed at half the lease)
      std::this_thread::sleep_for(std::chrono::seconds(kLease / 2));
      uint32_t again = 0;
      if (!MapOnce(port, lan_ip, &again)) REXLOG_WARN("p2p: router port mapping: renewal refused");
    }
  }).detach();
}

}  // namespace svr2011
