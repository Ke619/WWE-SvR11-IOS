// WWE SmackDown vs. Raw 2011 - the port's connection to the Community
// Creations server (see online_net.h).

#include "online_net.h"
#include "fps_overlay.h"
#include "online_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include "util.h"

REXCVAR_DEFINE_STRING(online_token, "", "Online",
                      "The player's Community Creations session (the launcher signs in and writes it)");

namespace svr2011::net {

namespace {

TransferListener g_transfer = nullptr;  // (SetTransferListener)
LocalAnswer g_local = nullptr;          // (SetLocalAnswer)
std::atomic<int> g_profile{0};          // (SignedInProfile)

// -- sockets ----------------------------------------------------------------------

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool StartSockets() {
  static const bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
}
void SetTimeout(Socket s, int seconds) {
  DWORD ms = DWORD(seconds) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}
#else
using Socket = int;
const Socket kBadSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool StartSockets() { return true; }
void SetTimeout(Socket s, int seconds) {
  timeval tv = {seconds, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

bool SendAll(Socket s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = send(s, data.data() + sent, int(std::min<size_t>(data.size() - sent, 1 << 20)), 0);
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

using svr2011::Lower;

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// -- MD5 (the presence login's proof) -------------------------------------------

std::string Md5Hex(const std::string& message) {
  static const uint32_t k[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  std::string m = message;
  const uint64_t bits = uint64_t(message.size()) * 8;
  m += char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  for (int i = 0; i < 8; ++i) m += char((bits >> (8 * i)) & 0xFF);
  uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const uint8_t*>(m.data() + off + i * 4);
      w[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    uint32_t a = h0, b = h1, c = h2, d = h3;
    for (int i = 0; i < 64; ++i) {
      uint32_t f, g;
      if (i < 16) f = (b & c) | (~b & d), g = i;
      else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (b | ~d), g = (7 * i) % 16;
      const uint32_t t = d;
      d = c;
      c = b;
      const uint32_t x = a + f + k[i] + w[g];
      b = b + ((x << r[i]) | (x >> (32 - r[i])));
      a = t;
    }
    h0 += a, h1 += b, h2 += c, h3 += d;
  }
  char out[33];
  const uint32_t hs[4] = {h0, h1, h2, h3};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) std::snprintf(out + i * 8 + j * 2, 3, "%02x", (hs[i] >> (8 * j)) & 0xFF);
  }
  return std::string(out, 32);
}

// -- http(s) ------------------------------------------------------------------------

struct Url {
  bool https = false;
  std::string host;
  int port = 80;
  std::string path = "/";
};

bool ParseUrl(const std::string& url, Url* out) {
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    out->https = true, out->port = 443, rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    out->https = false, out->port = 80, rest = url.substr(7);
  } else {
    return false;
  }
  const size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash);
  out->path = slash == std::string::npos ? "/" : rest.substr(slash);
  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    out->port = std::atoi(hostport.c_str() + colon + 1);
    hostport.resize(colon);
  }
  out->host = hostport;
  return !out->host.empty() && out->port > 0;
}

void ParseHeaders(const std::string& block, Response* r) {
  size_t pos = block.find("\r\n");
  while (pos != std::string::npos && pos + 2 < block.size()) {
    const size_t end = block.find("\r\n", pos + 2);
    const std::string line = block.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) r->headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    pos = end;
  }
}

#if defined(_WIN32)

std::wstring Wide(const std::string& s) {
  std::wstring w(s.size() + 1, L'\0');
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
  w.resize(n > 0 ? n - 1 : 0);
  return w;
}

std::optional<Response> RequestImpl(const std::string& method, const Url& url, const Headers& headers,
                                    const std::string& body, const std::string& /*full_url*/) {
  static HINTERNET session = [] {
    HINTERNET s = WinHttpOpen(L"SvR2011-PC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    // (sending: a whole body at once - an entrance movie is up to 40 MB)
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 300000, 120000);
    return s;
  }();
  if (!session) return std::nullopt;
  HINTERNET connect = WinHttpConnect(session, Wide(url.host).c_str(), INTERNET_PORT(url.port), 0);
  if (!connect) return std::nullopt;
  std::optional<Response> result;
  HINTERNET req = WinHttpOpenRequest(connect, Wide(method).c_str(), Wide(url.path).c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     url.https ? WINHTTP_FLAG_SECURE : 0);
  if (req) {
    std::string lines;
    for (const auto& [k, v] : headers) lines += k + ": " + v + "\r\n";
    const std::wstring wlines = Wide(lines);
    if (WinHttpSendRequest(req, wlines.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wlines.c_str(), DWORD(-1L),
                           body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                           DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
      Response r;
      DWORD status = 0, size = sizeof(status);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      r.status = int(status);
      DWORD length = 0;
      WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                          &length, WINHTTP_NO_HEADER_INDEX);
      if (length) {
        std::wstring raw(length / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &length, WINHTTP_NO_HEADER_INDEX)) {
          ParseHeaders(std::string(raw.begin(), raw.end()), &r);
        }
      }
      bool ok = true;
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
          ok = false;
          break;
        }
        if (!avail) break;
        const size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, r.body.data() + at, avail, &got)) {
          ok = false;
          break;
        }
        r.body.resize(at + got);
        if (r.body.size() > (32u << 20)) {
          ok = false;
          break;
        }
      }
      if (ok) result = std::move(r);
    }
    WinHttpCloseHandle(req);
  }
  WinHttpCloseHandle(connect);
  return result;
}

#else

// A whole HTTP answer (status line, headers, body).
std::optional<Response> ParseRaw(const std::string& raw) {
  const size_t end = raw.find("\r\n\r\n");
  if (end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) return std::nullopt;
  Response r;
  r.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
  ParseHeaders(raw.substr(0, end), &r);
  r.body = raw.substr(end + 4);
  return r;
}

// Plain http over sockets.
std::optional<Response> SocketRequest(const std::string& method, const Url& url, const Headers& headers,
                                      const std::string& body) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 || !found) {
    return std::nullopt;
  }
  Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBadSocket) {
    freeaddrinfo(found);
    return std::nullopt;
  }
  SetTimeout(s, 30);
  const bool connected = connect(s, found->ai_addr, int(found->ai_addrlen)) == 0;
  freeaddrinfo(found);
  std::string raw;
  if (connected) {
    std::string request = method + " " + url.path + " HTTP/1.0\r\nHost: " + url.host + ":" +
                          std::to_string(url.port) + "\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n";
    for (const auto& [k, v] : headers) request += k + ": " + v + "\r\n";
    request += "\r\n" + body;
    if (SendAll(s, request)) {
      char buf[65536];
      int n;
      while ((n = int(recv(s, buf, sizeof(buf), 0))) > 0 && raw.size() < (32u << 20)) raw.append(buf, size_t(n));
    }
  }
  CloseSocket(s);
  return ParseRaw(raw);
}

#if defined(__ANDROID__)

// https through Android's own TLS: NetBridge.request (android/java/.../NetBridge.java).
// The relay's threads are native, so the class comes from the activity's class
// loader (FindClass there sees only the system's classes).
jclass NetBridgeClass(JNIEnv* env) {
  static jclass cls = nullptr;
  static std::once_flag once;
  std::call_once(once, [env] {
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return;
    jclass activity_class = env->GetObjectClass(activity);
    jobject loader = env->CallObjectMethod(
        activity, env->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jstring name = env->NewStringUTF("io.github.kaikoclanworth1.svr2011.NetBridge");
    jobject found = env->CallObjectMethod(
        loader, env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      found = nullptr;
    }
    if (found) cls = static_cast<jclass>(env->NewGlobalRef(found));
    for (jobject o : {static_cast<jobject>(activity_class), loader, static_cast<jobject>(loader_class),
                      static_cast<jobject>(name), found}) {
      if (o) env->DeleteLocalRef(o);
    }
    env->DeleteLocalRef(activity);
    if (!cls) REXLOG_ERROR("online: NetBridge is missing from the app");
  });
  return cls;
}

std::optional<Response> BridgeRequest(const std::string& method, const std::string& url, const Headers& headers,
                                      const std::string& body) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env) return std::nullopt;
  jclass cls = NetBridgeClass(env);
  if (!cls) return std::nullopt;
  static jmethodID request =
      env->GetStaticMethodID(cls, "request", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[B)[B");
  if (!request) {
    env->ExceptionClear();
    return std::nullopt;
  }
  if (env->PushLocalFrame(16 + jint(headers.size()) * 2) != 0) return std::nullopt;
  jobjectArray jheaders =
      env->NewObjectArray(jsize(headers.size() * 2), env->FindClass("java/lang/String"), nullptr);
  for (size_t i = 0; i < headers.size(); ++i) {
    env->SetObjectArrayElement(jheaders, jsize(i * 2), env->NewStringUTF(headers[i].first.c_str()));
    env->SetObjectArrayElement(jheaders, jsize(i * 2 + 1), env->NewStringUTF(headers[i].second.c_str()));
  }
  jbyteArray jbody = env->NewByteArray(jsize(body.size()));
  env->SetByteArrayRegion(jbody, 0, jsize(body.size()), reinterpret_cast<const jbyte*>(body.data()));
  auto answer = static_cast<jbyteArray>(env->CallStaticObjectMethod(
      cls, request, env->NewStringUTF(method.c_str()), env->NewStringUTF(url.c_str()), jheaders, jbody));
  std::string raw;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  } else if (answer) {
    raw.resize(size_t(env->GetArrayLength(answer)));
    env->GetByteArrayRegion(answer, 0, jsize(raw.size()), reinterpret_cast<jbyte*>(raw.data()));
  }
  env->PopLocalFrame(nullptr);
  return ParseRaw(raw);
}

#endif

std::optional<Response> RequestImpl(const std::string& method, const Url& url, const Headers& headers,
                                    const std::string& body, const std::string& full_url) {
  if (url.https) {
#if defined(__ANDROID__)
    return BridgeRequest(method, full_url, headers, body);
#else
    static std::once_flag once;
    std::call_once(once, [] { REXLOG_WARN("online: https servers aren't supported on this platform yet"); });
    return std::nullopt;
#endif
  }
  return SocketRequest(method, url, headers, body);
}

#endif

std::string ServerBase() {
  std::string base = rex::cvar::Query<std::string>("online_server");
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.find("://") == std::string::npos) base = "http://" + base;  // (a bare host:port)
  return base;
}

// -- the relay: the game's http ------------------------------------------------------

void HandleHttp(Socket s) {
  SetTimeout(s, 60);
  std::string data;
  char buf[65536];
  size_t head_end;
  while ((head_end = data.find("\r\n\r\n")) == std::string::npos) {
    const int n = int(recv(s, buf, sizeof(buf), 0));
    if (n <= 0 || data.size() > 65536) {
      CloseSocket(s);
      return;
    }
    data.append(buf, size_t(n));
  }
  const std::string head = data.substr(0, head_end);
  std::string body = data.substr(head_end + 4);
  const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
  const std::string method = head.substr(0, sp1), target = head.substr(sp1 + 1, sp2 - sp1 - 1);
  Response request_headers;
  ParseHeaders(head, &request_headers);
  const size_t length = size_t(std::strtoull(request_headers.headers["content-length"].c_str(), nullptr, 10));
  while (body.size() < length && body.size() < (16u << 20)) {
    const int n = int(recv(s, buf, sizeof(buf), 0));
    if (n <= 0) break;
    body.append(buf, size_t(n));
  }
  if (g_local) {
    if (auto local = g_local(method, target, request_headers.headers, body)) {
      REXLOG_INFO("online: {} {} -> answered here ({} bytes)", method, target, local->size());
      SendAll(s, *local);
      CloseSocket(s);
      return;
    }
  }
  Headers forward;
  for (const char* name : {"content-type", "soapaction"}) {
    auto it = request_headers.headers.find(name);
    if (it != request_headers.headers.end()) forward.emplace_back(name, it->second);
  }
  // A Community Creations record from this build: its upload passed the "uses mods" check at the
  // confirm step (online.h CcUploadModsRefused) - the server refuses unmarked ones (cc_unmarked).
  // (the game asks for /SakeStorageServer/StorageServer.asmx - any case)
  if (Lower(target).find("/sakestorageserver/") != std::string::npos && body.find("CreateRecord") != std::string::npos &&
      body.find("UserContent") != std::string::npos)
    forward.emplace_back("x-svr-modfree", "1");
  {  // (where the player is, for the presence poll: Community Creations' requests)
    const std::string path = Lower(target);
    if (path.find("/sakefileserver/upload.aspx") != std::string::npos ||
        (path.find("/sakestorageserver/") != std::string::npos && body.find("CreateRecord") != std::string::npos))
      NoteOnlineActivity(OnlineActivity::kCcUpload);
    else if (path.find("/sakefileserver/download.aspx") != std::string::npos)
      NoteOnlineActivity(OnlineActivity::kCcDownload);
    else if (path.find("/sakestorageserver/") != std::string::npos && body.find("Newsfeed") == std::string::npos)
      NoteOnlineActivity(OnlineActivity::kCcBrowse);  // (not the NEWS panel's read at every visit to ONLINE)
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto r = ServerRequest(method, "/game" + target, body, forward);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::string reply;
  if (!r) {
    REXLOG_WARN("online: {} {} - the server didn't answer", method, target);
    reply = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  } else {
    REXLOG_INFO("online: {} {} -> {} ({} bytes, {} ms)", method, target, r->status, r->body.size(), ms);
    reply = "HTTP/1.1 " + std::to_string(r->status) + (r->status == 200 ? " OK" : " Error") + "\r\n";
    for (const char* name : {"Content-Type", "Sake-File-Result", "Sake-File-Id"}) {
      auto it = r->headers.find(Lower(name));
      if (it != r->headers.end()) reply += std::string(name) + ": " + it->second + "\r\n";
    }
    reply += "Content-Length: " + std::to_string(r->body.size()) + "\r\nConnection: close\r\n\r\n";
    reply += r->body;
  }
  SendAll(s, reply);
  CloseSocket(s);
  if (r && r->status == 200 && g_transfer) {
    const std::string path = Lower(target);
    if (path.find("/sakefileserver/upload.aspx") != std::string::npos && r->headers["sake-file-result"] == "0") {
      const int fileid = std::atoi(r->headers["sake-file-id"].c_str());
      if (fileid > 0) g_transfer(true, fileid, request_headers.headers["content-type"], body);
    } else if (path.find("/sakefileserver/download.aspx") != std::string::npos &&
               r->headers["sake-file-result"] == "0") {
      const size_t at = path.find("fileid=");
      const int fileid = at == std::string::npos ? 0 : std::atoi(path.c_str() + at + 7);
      if (fileid > 0) g_transfer(false, fileid, {}, r->body);
    }
  }
}

// -- the relay: the presence login -------------------------------------------------

std::string GpValue(const std::string& msg, const std::string& key) {
  const std::string k = "\\" + key + "\\";
  const size_t at = msg.find(k);
  if (at == std::string::npos) return {};
  const size_t start = at + k.size(), end = msg.find('\\', start);
  return msg.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The JSON field `key` of a flat object (the server's answers).
std::string JsonField(const std::string& json, const std::string& key) {
  const std::string k = "\"" + key + "\":";
  size_t at = json.find(k);
  if (at == std::string::npos) return {};
  at += k.size();
  while (at < json.size() && json[at] == ' ') ++at;
  if (at < json.size() && json[at] == '"') {
    const size_t end = json.find('"', at + 1);
    return json.substr(at + 1, end - at - 1);
  }
  const size_t end = json.find_first_of(",}", at);
  return Trim(json.substr(at, end - at));
}

void HandleGp(Socket s) {
  SetTimeout(s, 180);
  std::mt19937 rng(std::random_device{}());
  std::string challenge;
  for (int i = 0; i < 10; ++i) challenge += char('A' + rng() % 26);
  SendAll(s, "\\lc\\1\\challenge\\" + challenge + "\\id\\1\\final\\");
  std::string data;
  char buf[4096];
  for (;;) {
    const int n = int(recv(s, buf, sizeof(buf), 0));
    if (n <= 0) break;
    data.append(buf, size_t(n));
    size_t end;
    while ((end = data.find("\\final\\")) != std::string::npos) {
      const std::string msg = data.substr(0, end);
      data.erase(0, end + 7);
      const std::string id = GpValue(msg, "id").empty() ? "1" : GpValue(msg, "id");
      if (msg.rfind("\\login\\", 0) == 0) {
        // (client=2: the server's reason in this release's words - its re-activation code, an email to confirm)
        auto session = ServerRequest("GET", "/api/session?client=2");
        if (!session || session->status != 200 || JsonField(session->body, "ok") != "true") {
          const std::string why = !session ? "the server didn't answer" : JsonField(session->body, "error");
          REXLOG_WARN("online: presence login refused: {}", why.empty() ? "not signed in" : why);
          // (an account the server keeps off online: why, on screen - the launcher's Online tab does the steps)
          if (session && session->status == 403 && !why.empty())
            svr2011::ShowNotice("Online: " + why + " (the launcher's Online tab)", 12.0f);
          SendAll(s, "\\error\\\\err\\260\\fatal\\\\errmsg\\Sign in to Community Creations in the launcher.\\id\\" +
                         id + "\\final\\");
          continue;
        }
        const std::string user = GpValue(msg, "user");
        // the game's password is its XUID in decimal: the part before the '@'
        const std::string password = user.substr(0, user.find('@'));
        const std::string ph = Md5Hex(password);
        const std::string proof =
            Md5Hex(ph + std::string(48, ' ') + user + challenge + GpValue(msg, "challenge") + ph);
        const std::string pid = JsonField(session->body, "profileid");
        SendAll(s, "\\lc\\2\\sesskey\\" + std::to_string(rng() % 2000000000 + 1) + "\\proof\\" + proof +
                       "\\userid\\" + pid + "\\profileid\\" + pid + "\\uniquenick\\\\lt\\" +
                       JsonField(session->body, "ticket") + "\\id\\" + id + "\\final\\");
        REXLOG_INFO("online: signed in as {} (profile {})", JsonField(session->body, "name"), pid);
        g_profile = std::atoi(pid.c_str());
      } else if (msg.rfind("\\getprofile\\", 0) == 0) {
        const std::string pid = GpValue(msg, "profileid");
        std::string sig;
        for (int i = 0; i < 32; ++i) sig += "0123456789abcdef"[rng() % 16];
        SendAll(s, "\\pi\\\\profileid\\" + pid + "\\nick\\" + pid + "\\userid\\" + pid + "\\uniquenick\\" + pid +
                       "\\sig\\" + sig + "\\id\\" + id + "\\final\\");
      }
      // (\ka\, \status\, \logout\ ...: nothing to answer)
    }
  }
  CloseSocket(s);
}

// A listener on 127.0.0.1 (a free port); each connection on its own thread.
uint16_t Listen(void (*handler)(Socket)) {
  Socket l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == kBadSocket) return 0;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socklen_t len = sizeof(addr);
  if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(l, 16) != 0 ||
      getsockname(l, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    CloseSocket(l);
    return 0;
  }
  std::thread([l, handler] {
    for (;;) {
      Socket c = accept(l, nullptr, nullptr);
      if (c == kBadSocket) continue;
      std::thread(handler, c).detach();
    }
  }).detach();
  return ntohs(addr.sin_port);
}

// -- pipes: two long requests (the match relay) ----------------------------------------
//
// A half reads (the GET's body as it arrives) or writes (the POST's chunked
// body); Close() only asks: each half stops on its own thread.

struct Half {
  virtual ~Half() = default;
  virtual int Read(uint8_t* /*buf*/, size_t /*size*/) { return -1; }  // bytes, 0: nothing yet, -1: over
  virtual bool Write(const uint8_t* /*data*/, size_t /*size*/) { return false; }
  virtual void Finish() {}  // (the upload: its last chunk)
};

#if defined(_WIN32)

HINTERNET PipeSession() {
  static HINTERNET session = [] {
    HINTERNET s = WinHttpOpen(L"SvR2011-PC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    // (the server sends a keepalive every 5 s: 30 s of nothing is a dead stream)
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 30000, 30000);
    return s;
  }();
  return session;
}

struct WinHalf : Half {
  HINTERNET connect = nullptr, req = nullptr;
  bool upload = false;
  ~WinHalf() override {
    if (req) WinHttpCloseHandle(req);
    if (connect) WinHttpCloseHandle(connect);
  }
  // Starts the request: the GET up to its answer's headers (its status), the POST up to its body.
  int Start(const Url& url, bool post, const Headers& headers) {
    upload = post;
    if (!PipeSession()) return 0;
    connect = WinHttpConnect(PipeSession(), Wide(url.host).c_str(), INTERNET_PORT(url.port), 0);
    if (!connect) return 0;
    req = WinHttpOpenRequest(connect, post ? L"POST" : L"GET", Wide(url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return 0;
    std::string lines;
    for (const auto& [k, v] : headers) lines += k + ": " + v + "\r\n";
    if (post) lines += "Transfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n";
    const std::wstring wlines = Wide(lines);
    if (!WinHttpSendRequest(req, wlines.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0,
                            post ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : 0, 0)) {
      return 0;
    }
    if (post) return 200;
    if (!WinHttpReceiveResponse(req, nullptr)) return 0;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return int(status);
  }
  int Read(uint8_t* buf, size_t size) override {
    DWORD avail = 0, got = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || !avail) return -1;  // (blocks until some comes)
    if (!WinHttpReadData(req, buf, std::min<DWORD>(avail, DWORD(size)), &got) || !got) return -1;
    return int(got);
  }
  bool Write(const uint8_t* data, size_t size) override {
    char head[16];
    std::snprintf(head, sizeof(head), "%zx\r\n", size);
    std::string chunk = head;
    chunk.append(reinterpret_cast<const char*>(data), size);
    chunk += "\r\n";
    DWORD wrote = 0;
    return WinHttpWriteData(req, chunk.data(), DWORD(chunk.size()), &wrote) && wrote == chunk.size();
  }
  void Finish() override {
    DWORD wrote = 0;
    if (upload && WinHttpWriteData(req, "0\r\n\r\n", 5, &wrote)) WinHttpReceiveResponse(req, nullptr);
  }
};

std::unique_ptr<Half> StartHalf(const Url& url, const std::string& /*full_url*/, bool post, const Headers& headers,
                                int* status) {
  auto half = std::make_unique<WinHalf>();
  *status = half->Start(url, post, headers);
  return half;
}

#elif defined(__ANDROID__)

// NetBridge.pipe* (android/java/.../NetBridge.java): Android's own http(s).
struct BridgeHalf : Half {
  jobject pipe = nullptr;  // (a global reference)
  static JNIEnv* Env() { return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); }
  static jmethodID Method(JNIEnv* env, const char* name, const char* sig) {
    jclass cls = NetBridgeClass(env);
    jmethodID m = cls ? env->GetStaticMethodID(cls, name, sig) : nullptr;
    if (!m) env->ExceptionClear();
    return m;
  }
  ~BridgeHalf() override {
    JNIEnv* env = Env();
    if (!env || !pipe) return;
    if (jmethodID close = Method(env, "pipeClose", "(Ljava/lang/Object;)V")) {
      env->CallStaticVoidMethod(NetBridgeClass(env), close, pipe);
      env->ExceptionClear();
    }
    env->DeleteGlobalRef(pipe);
  }
  int Start(const std::string& url, bool post, const Headers& headers) {
    JNIEnv* env = Env();
    if (!env) return 0;
    jmethodID open = Method(env, "pipeOpen", "(Ljava/lang/String;Z[Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID status = Method(env, "pipeStatus", "(Ljava/lang/Object;)I");
    if (!open || !status || env->PushLocalFrame(16 + jint(headers.size()) * 2) != 0) return 0;
    jobjectArray jheaders =
        env->NewObjectArray(jsize(headers.size() * 2), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < headers.size(); ++i) {
      env->SetObjectArrayElement(jheaders, jsize(i * 2), env->NewStringUTF(headers[i].first.c_str()));
      env->SetObjectArrayElement(jheaders, jsize(i * 2 + 1), env->NewStringUTF(headers[i].second.c_str()));
    }
    jobject p = env->CallStaticObjectMethod(NetBridgeClass(env), open, env->NewStringUTF(url.c_str()),
                                            jboolean(post), jheaders);
    int code = 0;
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    } else if (p) {
      pipe = env->NewGlobalRef(p);
      code = env->CallStaticIntMethod(NetBridgeClass(env), status, pipe);
      env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
    return code;
  }
  int Read(uint8_t* buf, size_t size) override {
    JNIEnv* env = Env();
    jmethodID read = env ? Method(env, "pipeRead", "(Ljava/lang/Object;[B)I") : nullptr;
    if (!read || !pipe) return -1;
    jbyteArray jbuf = env->NewByteArray(jsize(size));
    const int n = env->CallStaticIntMethod(NetBridgeClass(env), read, pipe, jbuf);
    const bool failed = env->ExceptionCheck();
    env->ExceptionClear();
    if (!failed && n > 0) env->GetByteArrayRegion(jbuf, 0, n, reinterpret_cast<jbyte*>(buf));
    env->DeleteLocalRef(jbuf);
    return failed ? -1 : n;
  }
  bool Write(const uint8_t* data, size_t size) override {
    JNIEnv* env = Env();
    jmethodID write = env ? Method(env, "pipeWrite", "(Ljava/lang/Object;[B)Z") : nullptr;
    if (!write || !pipe) return false;
    jbyteArray jdata = env->NewByteArray(jsize(size));
    env->SetByteArrayRegion(jdata, 0, jsize(size), reinterpret_cast<const jbyte*>(data));
    const bool ok = env->CallStaticBooleanMethod(NetBridgeClass(env), write, pipe, jdata);
    const bool failed = env->ExceptionCheck();
    env->ExceptionClear();
    env->DeleteLocalRef(jdata);
    return ok && !failed;
  }
};

std::unique_ptr<Half> StartHalf(const Url& /*url*/, const std::string& full_url, bool post, const Headers& headers,
                                int* status) {
  auto half = std::make_unique<BridgeHalf>();
  *status = half->Start(full_url, post, headers);
  return half;
}

#else

// Plain http over a socket (no TLS here: https servers need Windows or Android).
struct SocketHalf : Half {
  Socket s = kBadSocket;
  std::string early;  // (body bytes read with the headers)
  ~SocketHalf() override {
    if (s != kBadSocket) CloseSocket(s);
  }
  int Start(const Url& url, bool post, const Headers& headers) {
    if (url.https) return 0;
    addrinfo hints = {}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 || !found) return 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const bool connected = s != kBadSocket && connect(s, found->ai_addr, int(found->ai_addrlen)) == 0;
    freeaddrinfo(found);
    if (!connected) return 0;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    SetTimeout(s, 30);
    // (the download: HTTP/1.0, so its body comes plain until the server closes it)
    std::string request = (post ? "POST " : "GET ") + url.path + (post ? " HTTP/1.1" : " HTTP/1.0") +
                          "\r\nHost: " + url.host + ":" + std::to_string(url.port) + "\r\n";
    if (post) request += "Transfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n";
    for (const auto& [k, v] : headers) request += k + ": " + v + "\r\n";
    if (!SendAll(s, request + "\r\n")) return 0;
    if (post) return 200;
    std::string head;
    char buf[4096];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536) {
      const int n = int(recv(s, buf, sizeof(buf), 0));
      if (n <= 0) return 0;
      head.append(buf, size_t(n));
    }
    const size_t end = head.find("\r\n\r\n");
    early = head.substr(end + 4);
    return head.compare(0, 5, "HTTP/") == 0 ? std::atoi(head.c_str() + head.find(' ') + 1) : 0;
  }
  int Read(uint8_t* buf, size_t size) override {
    if (!early.empty()) {
      const size_t n = std::min(size, early.size());
      std::memcpy(buf, early.data(), n);
      early.erase(0, n);
      return int(n);
    }
    const int n = int(recv(s, reinterpret_cast<char*>(buf), int(size), 0));
    return n > 0 ? n : -1;
  }
  bool Write(const uint8_t* data, size_t size) override {
    char head[16];
    std::snprintf(head, sizeof(head), "%zx\r\n", size);
    return SendAll(s, head + std::string(reinterpret_cast<const char*>(data), size) + "\r\n");
  }
  void Finish() override { SendAll(s, "0\r\n\r\n"); }
};

std::unique_ptr<Half> StartHalf(const Url& url, const std::string& /*full_url*/, bool post, const Headers& headers,
                                int* status) {
  auto half = std::make_unique<SocketHalf>();
  *status = half->Start(url, post, headers);
  return half;
}

#endif

class PipeImpl : public Pipe {
 public:
  bool Send(const uint8_t* data, size_t size) override {
    std::lock_guard lock(mutex_);
    if (!open_ || !up_) return false;
    if (!up_->Write(data, size)) {
      REXLOG_WARN("online: the stream to the server broke");
      Over();
      return false;
    }
    return true;
  }
  bool Open() const override { return open_; }
  void Close() override {
    std::lock_guard lock(mutex_);
    Over();
  }

  std::function<void(const uint8_t*, size_t)> on_data_;
  std::unique_ptr<Half> down_, up_;
  std::atomic<bool> open_{true};
  std::mutex mutex_;

  void Over() {  // (under mutex_)
    if (!open_.exchange(false)) return;
    if (up_) up_->Finish();
  }
  void ReadLoop() {
    std::vector<uint8_t> buf(16384);
    for (;;) {
      const int n = down_->Read(buf.data(), buf.size());
      if (n < 0 || !open_) break;
      if (n > 0) on_data_(buf.data(), size_t(n));
    }
    {
      std::lock_guard lock(mutex_);
      Over();
    }
    on_data_(nullptr, 0);
  }
};

}  // namespace

std::shared_ptr<Pipe> OpenPipe(const std::string& path, std::function<void(const uint8_t*, size_t)> on_data) {
  const std::string url = ServerBase() + path;
  Url u;
  if (!ParseUrl(url, &u)) return nullptr;
  Headers headers;
  const std::string token = rex::cvar::Query<std::string>("online_token");
  if (!token.empty()) headers.emplace_back("Authorization", "Bearer " + token);
  auto pipe = std::make_shared<PipeImpl>();
  pipe->on_data_ = std::move(on_data);
  int status = 0;
  pipe->down_ = StartHalf(u, url, false, headers, &status);  // (first: the server takes uploads for open downloads)
  if (status != 200) {
    REXLOG_WARN("online: {} {}", path, status ? "refused (" + std::to_string(status) + ")" : "didn't answer");
    return nullptr;
  }
  pipe->up_ = StartHalf(u, url, true, headers, &status);
  if (status != 200) return nullptr;
  std::thread([pipe] { pipe->ReadLoop(); }).detach();
  return pipe;
}

namespace {

}  // namespace

std::optional<Response> Request(const std::string& method, const std::string& url, const Headers& headers,
                                const std::string& body) {
  Url u;
  if (!ParseUrl(url, &u)) return std::nullopt;
  return RequestImpl(method, u, headers, body, url);
}

std::optional<Response> ServerRequest(const std::string& method, const std::string& path, const std::string& body,
                                      Headers headers) {
  const std::string token = rex::cvar::Query<std::string>("online_token");
  if (!token.empty()) headers.emplace_back("Authorization", "Bearer " + token);
  auto r = Request(method, ServerBase() + path, headers, body);
  // (the server's word for the player - a Community Creations name its filter refused: on screen)
  if (r)
    if (auto it = r->headers.find("x-svr-notice"); it != r->headers.end() && !it->second.empty())
      svr2011::ShowNotice(it->second, 8.0f);
  return r;
}

void SetTransferListener(TransferListener listener) { g_transfer = listener; }

void SetLocalAnswer(LocalAnswer answer) { g_local = answer; }

int SignedInProfile() { return g_profile; }

bool StartRelay() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    if (!StartSockets()) return;
    const uint16_t http = Listen(HandleHttp), gp = Listen(HandleGp);
    if (!http || !gp) {
      REXLOG_ERROR("online: the relay couldn't listen on this PC");
      return;
    }
    rex::cvar::SetFlagByName("online_relay_http", std::to_string(http));
    rex::cvar::SetFlagByName("online_relay_gp", std::to_string(gp));
    REXLOG_INFO("online: relay on 127.0.0.1 ports {} (http) and {} (presence) -> {}", http, gp, ServerBase());
    ok = true;
  });
  return ok;
}

}  // namespace svr2011::net
