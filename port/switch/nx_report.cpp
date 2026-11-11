// See nx_report.h. The form the server takes is in launcher/report_send.h.

#include "nx_report.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace nx_report {
namespace {

constexpr size_t kMaxZip = 24u << 20;    // (the server takes 25 MB)
constexpr size_t kMaxMatch = 5u << 20;   // (its limit for a match)
constexpr size_t kLogTail = 6u << 20;    // the end of each log part
constexpr int kTimeoutSeconds = 30;

// The roots of the public server's certificates (Let's Encrypt).
const char kRoots[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

bool ReadFile(const std::string& path, std::string& out, size_t tail = 0) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  struct stat st = {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  size_t size = size_t(st.st_size);
  if (tail && size > tail) {
    lseek(fd, off_t(size - tail), SEEK_SET);
    size = tail;
  }
  out.resize(size);
  size_t at = 0;
  while (at < size) {
    const ssize_t n = read(fd, out.data() + at, std::min<size_t>(size - at, 1u << 20));
    if (n <= 0) break;
    at += size_t(n);
  }
  out.resize(at);
  close(fd);
  return true;
}

std::vector<std::string> List(const std::string& dir, bool folders) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st = {};
    if (stat((dir + "/" + name).c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode) == folders) out.push_back(name);
  }
  closedir(d);
  return out;
}

// ---- a zip file, written as it goes -----------------------------------------------------------------------------

class Zip {
 public:
  explicit Zip(const std::string& path) { fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); }
  ~Zip() {
    if (fd_ >= 0) close(fd_);
  }
  bool ok() const { return fd_ >= 0 && !bad_; }
  size_t size() const { return at_; }

  // False when it would pass `limit`; the zip stays good without this file.
  bool Add(const std::string& name, const std::string& data, size_t limit) {
    if (!ok()) return false;
    std::vector<uint8_t> packed(compressBound(uLong(data.size())) + 64);
    z_stream z = {};
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = uInt(data.size());
    z.next_out = packed.data();
    z.avail_out = uInt(packed.size());
    const int r = deflate(&z, Z_FINISH);
    const size_t n = z.total_out;
    deflateEnd(&z);
    if (r != Z_STREAM_END) return false;
    if (at_ + n + name.size() + 30 > limit) return false;

    File f;
    f.name = name;
    f.crc = uint32_t(crc32(crc32(0, nullptr, 0), reinterpret_cast<const Bytef*>(data.data()), uInt(data.size())));
    f.packed = uint32_t(n);
    f.size = uint32_t(data.size());
    f.at = uint32_t(at_);
    Stamp(f);
    std::string head;
    P32(head, 0x04034B50), P16(head, 20), P16(head, 0x0800), P16(head, 8), P16(head, f.time), P16(head, f.date);
    P32(head, f.crc), P32(head, f.packed), P32(head, f.size), P16(head, uint16_t(name.size())), P16(head, 0);
    head += name;
    Write(head.data(), head.size());
    Write(packed.data(), n);
    files_.push_back(std::move(f));
    return ok();
  }

  bool Finish() {
    if (!ok()) return false;
    const uint32_t start = uint32_t(at_);
    std::string dir;
    for (const File& f : files_) {
      P32(dir, 0x02014B50), P16(dir, 20), P16(dir, 20), P16(dir, 0x0800), P16(dir, 8), P16(dir, f.time), P16(dir, f.date);
      P32(dir, f.crc), P32(dir, f.packed), P32(dir, f.size), P16(dir, uint16_t(f.name.size()));
      P16(dir, 0), P16(dir, 0), P16(dir, 0), P16(dir, 0), P32(dir, 0), P32(dir, f.at);
      dir += f.name;
    }
    std::string end;
    P32(end, 0x06054B50), P16(end, 0), P16(end, 0), P16(end, uint16_t(files_.size())), P16(end, uint16_t(files_.size()));
    P32(end, uint32_t(dir.size())), P32(end, start), P16(end, 0);
    Write(dir.data(), dir.size());
    Write(end.data(), end.size());
    const bool good = ok();
    close(fd_);
    fd_ = -1;
    return good;
  }

 private:
  struct File {
    std::string name;
    uint32_t crc = 0, packed = 0, size = 0, at = 0;
    uint16_t time = 0, date = 0x21;
  };
  static void P16(std::string& s, uint16_t v) { s += char(v), s += char(v >> 8); }
  static void P32(std::string& s, uint32_t v) { P16(s, uint16_t(v)), P16(s, uint16_t(v >> 16)); }
  static void Stamp(File& f) {
    const time_t now = time(nullptr);
    if (const tm* t = localtime(&now); t && t->tm_year >= 80) {
      f.time = uint16_t(t->tm_hour << 11 | t->tm_min << 5 | t->tm_sec / 2);
      f.date = uint16_t((t->tm_year - 80) << 9 | (t->tm_mon + 1) << 5 | t->tm_mday);
    }
  }
  void Write(const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n && !bad_) {
      const ssize_t w = write(fd_, c, std::min<size_t>(n, 1u << 20));
      if (w <= 0) {
        bad_ = true;
        return;
      }
      c += w, n -= size_t(w), at_ += size_t(w);
    }
  }
  int fd_ = -1;
  bool bad_ = false;
  size_t at_ = 0;
  std::vector<File> files_;
};

// A log name: svr2011_<game>.log or svr2011_<game>.<part>.log. The higher part is the older.
bool LogName(const std::string& name, long& game, long& part) {
  if (name.rfind("svr2011_", 0) != 0 || name.size() < 13 || name.compare(name.size() - 4, 4, ".log") != 0) return false;
  const char* p = name.c_str() + 8;
  if (*p < '0' || *p > '9') return false;
  char* q = nullptr;
  game = std::strtol(p, &q, 10);
  part = 0;
  if (*q == '.' && q[1] >= '0' && q[1] <= '9') part = std::strtol(q + 1, &q, 10);
  return !std::strcmp(q, ".log");
}

// The newest game's parts and the newest part of the two games before it.
void AddLogs(Zip& zip, const std::string& game_dir) {
  const std::string dir = game_dir + "/logs";
  std::map<long, std::vector<std::pair<long, std::string>>> games;
  for (const std::string& name : List(dir, false)) {
    long game = 0, part = 0;
    if (LogName(name, game, part)) games[game].push_back({part, name});
  }
  int nth = 0;
  for (auto g = games.rbegin(); g != games.rend() && nth < 3; ++g, ++nth) {
    std::sort(g->second.begin(), g->second.end());
    for (const auto& [part, name] : g->second) {
      std::string text;
      if (!ReadFile(dir + "/" + name, text, kLogTail)) continue;
      RedactIps(text);
      if (!zip.Add("logs/" + name, text, kMaxZip)) return;
      if (nth) break;
    }
  }
}

void AddFile(Zip& zip, const std::string& path, const std::string& name, size_t tail = 0, bool redact = false) {
  std::string text;
  if (!ReadFile(path, text, tail) || text.empty()) return;
  if (redact) RedactIps(text);
  zip.Add(name, text, kMaxZip);
}

// The settings, without the account's token and password.
void AddSettings(Zip& zip, const std::string& game_dir) {
  std::string text, out;
  if (!ReadFile(game_dir + "/svr2011.toml", text)) return;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size() - 1;
    std::string line = text.substr(at, end - at + 1);
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos) {
      if (line.compare(first, 12, "online_token") == 0) line = "online_token = \"(removed)\"\n";
      else if (line.compare(first, 15, "online_password") == 0) line = "online_password = \"(removed)\"\n";
    }
    out += line;
    at = end + 1;
  }
  RedactIps(out);
  zip.Add("svr2011.toml", out, kMaxZip);
}

void AddSave(Zip& zip, const std::string& game_dir) {
  const std::string saves = game_dir + "/Saves";
  AddFile(zip, saves + "/SaveData.dat", "save/SaveData.dat");
  AddFile(zip, saves + "/.info/SaveData.dat.header", "save/.info/SaveData.dat.header");
  for (const std::string& name : List(saves + "/.universe", false))
    AddFile(zip, saves + "/.universe/" + name, "save/.universe/" + name);
}

std::string Firmware() {
  const u32 v = hosversionGet();
  char s[64];
  std::snprintf(s, sizeof s, "Horizon %u.%u.%u%s", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v),
                hosversionIsAtmosphere() ? " Atmosphere" : "");
  return s;
}

std::string JsonText(const std::string& v) {
  std::string out = "\"";
  for (const char c : v) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) {
          char u[8];
          std::snprintf(u, sizeof u, "\\u%04x", unsigned(uint8_t(c)));
          out += u;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

// A value from the server's small answer ("" when it isn't there).
std::string JsonField(const std::string& json, const char* key) {
  const std::string want = std::string("\"") + key + "\"";
  size_t p = json.find(want);
  if (p == std::string::npos) return {};
  p = json.find(':', p + want.size());
  if (p == std::string::npos) return {};
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) ++p;
  if (p >= json.size()) return {};
  std::string out;
  if (json[p] != '"') {
    while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ' ') out += json[p++];
    return out;
  }
  for (++p; p < json.size() && json[p] != '"'; ++p) {
    if (json[p] == '\\' && p + 1 < json.size()) {
      const char c = json[++p];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
    } else {
      out += json[p];
    }
  }
  return out;
}

// ---- the connection -----------------------------------------------------------------------------------------------

struct Url {
  bool https = false;
  std::string host, path;
  int port = 80;
};

bool ParseUrl(std::string url, Url& u) {
  if (url.rfind("https://", 0) == 0) {
    u.https = true, u.port = 443;
    url = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    url = url.substr(7);
  } else if (url.find("://") != std::string::npos) {
    return false;
  }
  const size_t slash = url.find('/');
  u.path = slash == std::string::npos ? "" : url.substr(slash);
  while (!u.path.empty() && u.path.back() == '/') u.path.pop_back();
  std::string host = url.substr(0, slash);
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    u.port = std::atoi(host.c_str() + colon + 1);
    host.resize(colon);
  }
  u.host = host;
  return !host.empty() && u.port > 0 && u.port < 65536;
}

int RandomBytes(void*, unsigned char* out, size_t n) {
  randomGet(out, n);
  return 0;
}

int BioSend(void* ctx, const unsigned char* p, size_t n) {
  const ssize_t r = send(*static_cast<int*>(ctx), p, n, 0);
  return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : int(r);
}
int BioRecv(void* ctx, unsigned char* p, size_t n) {
  const ssize_t r = recv(*static_cast<int*>(ctx), p, n, 0);
  return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : int(r);
}

class Connection {
 public:
  ~Connection() {
    if (tls_on_) mbedtls_ssl_close_notify(&tls_->ssl);
    if (fd_ >= 0) close(fd_);
    if (tls_) {
      mbedtls_ssl_free(&tls_->ssl);
      mbedtls_ssl_config_free(&tls_->conf);
      mbedtls_x509_crt_free(&tls_->roots);
      mbedtls_ctr_drbg_free(&tls_->drbg);
    }
  }

  bool Open(const Url& u, const std::string& game_dir, std::string& why) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    char port[16];
    std::snprintf(port, sizeof port, "%d", u.port);
    if (getaddrinfo(u.host.c_str(), port, &hints, &found) != 0 || !found) {
      why = "Can't find the server. Is the console connected to the internet?";
      return false;
    }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ >= 0) {
      timeval t = {kTimeoutSeconds, 0};
      setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
      setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    }
    const bool connected = fd_ >= 0 && connect(fd_, found->ai_addr, found->ai_addrlen) == 0;
    freeaddrinfo(found);
    if (!connected) {
      why = "Can't reach the server. Is the console connected to the internet?";
      return false;
    }
    return !u.https || StartTls(u, game_dir, why);
  }

  bool Write(const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
      const int w = tls_on_ ? mbedtls_ssl_write(&tls_->ssl, reinterpret_cast<const unsigned char*>(c), n)
                            : int(send(fd_, c, n, 0));
      if (w <= 0) return false;
      c += w, n -= size_t(w);
    }
    return true;
  }

  // Up to the end of the answer (the server closes: Connection: close).
  void ReadAll(std::string& out) {
    char buf[4096];
    while (out.size() < (1u << 20)) {
      const int r = tls_on_ ? mbedtls_ssl_read(&tls_->ssl, reinterpret_cast<unsigned char*>(buf), sizeof buf)
                            : int(recv(fd_, buf, sizeof buf, 0));
      if (r <= 0) break;
      out.append(buf, size_t(r));
    }
  }

 private:
  struct Tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_ctr_drbg_context drbg;
  };

  bool StartTls(const Url& u, const std::string& game_dir, std::string& why) {
    tls_ = std::make_unique<Tls>();
    mbedtls_ssl_init(&tls_->ssl);
    mbedtls_ssl_config_init(&tls_->conf);
    mbedtls_x509_crt_init(&tls_->roots);
    mbedtls_ctr_drbg_init(&tls_->drbg);
    bool good = mbedtls_ctr_drbg_seed(&tls_->drbg, RandomBytes, nullptr, nullptr, 0) == 0 &&
                mbedtls_x509_crt_parse(&tls_->roots, reinterpret_cast<const unsigned char*>(kRoots), sizeof kRoots) == 0;
#if !defined(SVR_NX_REPORTS_LIVE)
    // A test build: a private server's own certificate, from the SD card. Never read by a release build.
    std::string test_ca;
    if (good && ReadFile(game_dir + "/nx_test_ca.pem", test_ca)) {
      test_ca.push_back('\0');
      good = mbedtls_x509_crt_parse(&tls_->roots, reinterpret_cast<const unsigned char*>(test_ca.data()), test_ca.size()) == 0;
    }
#else
    (void)game_dir;
#endif
    good = good &&
           mbedtls_ssl_config_defaults(&tls_->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (!good) {
      why = "Secure connection: the setup failed.";
      return false;
    }
    mbedtls_ssl_conf_authmode(&tls_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls_->conf, &tls_->roots, nullptr);
    mbedtls_ssl_conf_rng(&tls_->conf, mbedtls_ctr_drbg_random, &tls_->drbg);
    good = mbedtls_ssl_setup(&tls_->ssl, &tls_->conf) == 0 && mbedtls_ssl_set_hostname(&tls_->ssl, u.host.c_str()) == 0;
    if (!good) {
      why = "Secure connection: the setup failed.";
      return false;
    }
    mbedtls_ssl_set_bio(&tls_->ssl, &fd_, BioSend, BioRecv, nullptr);
    const int r = mbedtls_ssl_handshake(&tls_->ssl);
    if (r != 0) {
      const uint32_t flags = mbedtls_ssl_get_verify_result(&tls_->ssl);
      char s[160];
      if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)))
        std::snprintf(s, sizeof s, "The server's certificate looks out of date. Is the console's clock right?");
      else
        std::snprintf(s, sizeof s, "Secure connection failed (error -0x%04X, certificate flags 0x%X).", unsigned(-r), unsigned(flags));
      why = s;
      return false;
    }
    tls_on_ = true;
    return true;
  }

  int fd_ = -1;
  std::unique_ptr<Tls> tls_;
  bool tls_on_ = false;
};

// ---- the request ----------------------------------------------------------------------------------------------------

struct Part {
  std::string head;
  std::string file;  // "" : head only
  size_t file_size = 0;
};

Part FilePart(const std::string& boundary, const char* name, const std::string& filename, const char* type, const std::string& path,
              size_t max) {
  Part p;
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || size_t(st.st_size) > max) return p;
  p.file = path;
  p.file_size = size_t(st.st_size);
  p.head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename +
           "\"\r\nContent-Type: " + type + "\r\n\r\n";
  return p;
}

bool ZipFolder(const std::string& dir, const std::string& prefix, Zip& zip, size_t limit) {
  for (const std::string& name : List(dir, false)) {
    std::string data;
    if (!ReadFile(dir + "/" + name, data, limit) || !zip.Add(prefix + name, data, limit)) return false;
  }
  for (const std::string& name : List(dir, true))
    if (!ZipFolder(dir + "/" + name, prefix + name + "/", zip, limit)) return false;
  return true;
}

}  // namespace

void RedactIps(std::string& text) {
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  const size_t n = text.size();
  char* d = text.data();
  size_t i = 0, o = 0, line = 0;
  int skip = -1;
  while (i < n) {
    if (skip < 0) {
      skip = 0;
      for (size_t e = line; e < n && d[e] != '\n'; ++e)
        if (e + 5 < n && !std::memcmp(d + e, "ersion", 6)) skip = 1;
    }
    if (!skip && digit(d[i]) && (i == 0 || (!digit(d[i - 1]) && d[i - 1] != '.'))) {
      size_t p = i;
      bool ok = true;
      unsigned v[4] = {};
      for (int part = 0; part < 4 && ok; ++part) {
        const size_t s = p;
        while (p < n && digit(d[p]) && p - s < 3) v[part] = v[part] * 10 + unsigned(d[p++] - '0');
        if (p == s || v[part] > 255) ok = false;
        else if (part < 3) {
          if (p < n && d[p] == '.') ++p;
          else ok = false;
        }
      }
      if (ok && (p >= n || (!digit(d[p]) && !(d[p] == '.' && p + 1 < n && digit(d[p + 1]))))) {
        // (Reads ahead of the write position, so the text only gets shorter.)
        if (v[0] != 127 && p - i >= 7) {
          std::memcpy(d + o, "x.x.x.x", 7);
          o += 7;
        } else {
          std::memmove(d + o, d + i, p - i);
          o += p - i;
        }
        i = p;
        continue;
      }
    }
    if (d[i] == '\n') {
      line = i + 1;
      skip = -1;
    }
    d[o++] = d[i++];
  }
  text.resize(o);
}

bool AddReceipt(const Input& in, const Output& out) {
  const std::string dir = in.game_dir + "/UserData", path = dir + "/reports.json", tmp = path + ".tmp";
  mkdir(dir.c_str(), 0755);
  std::string old;
  ReadFile(path, old);
  const size_t end = old.rfind(']');
  const bool some = end != std::string::npos && old.find('{') != std::string::npos;
  std::string text = some ? old.substr(0, old.find_last_not_of(" \r\n\t", end - 1) + 1) + ",\n" : "[\n";
  char date[32] = "";
  const time_t now = time(nullptr);
  if (const tm* t = localtime(&now)) std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", t);
  std::string what = in.what.substr(0, 160);
  while (!what.empty() && (uint8_t(what.back()) & 0xC0) == 0x80) what.pop_back();  // (a whole character)
  if (!what.empty() && uint8_t(what.back()) >= 0xC0) what.pop_back();
  text += "{\"id\":" + JsonText(out.id) + ",\"secret\":" + JsonText(out.secret) + ",\"date\":" + JsonText(date) +
          ",\"app\":\"game\",\"problem\":" + JsonText(in.problem) + ",\"what\":" + JsonText(what);
  if (in.pictures_later) text += ",\"pictures_later\":1";
  text += "}\n]\n";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  std::fclose(f);
  if (!wrote) {
    std::remove(tmp.c_str());
    return false;
  }
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

Output Send(const Input& in) {
  Output out;
  Url url;
  if (in.token.empty()) {
    out.account = true;
    out.why = "Sign in first: the PC launcher's Online tab, then Setup -> Switch again to bring the sign-in here.";
    return out;
  }
  if (!ParseUrl(in.server, url)) {
    out.why = "The server's address isn't right: " + in.server;
    return out;
  }
  const std::string reports = in.game_dir + "/Reports", upload = reports + "/.upload";
  mkdir(reports.c_str(), 0755);
  mkdir(upload.c_str(), 0755);
  const std::string zip_path = upload + "/report.zip", match_path = upload + "/match.zip";

  {
    Zip zip(zip_path);
    std::string text = "SvR 2011 port " + in.version + " (Switch, " + Firmware() + ")\r\nProblem: " + in.problem + "\r\nHappens: " +
                       in.happens + "\r\n\r\nWhat happened:\r\n" + in.what + "\r\n\r\nJust before:\r\n" +
                       (in.before.empty() ? "(not said)" : in.before) + "\r\n";
    zip.Add("report.txt", text, kMaxZip);
    AddLogs(zip, in.game_dir);
    AddFile(zip, in.game_dir + "/logs/nx_crash.txt", "crashes/nx_crash.txt", 256u << 10, true);
    AddFile(zip, in.game_dir + "/logs/nx_install_check.txt", "logs/nx_install_check.txt", 64u << 10, true);
    AddSettings(zip, in.game_dir);
    if (in.with_save) AddSave(zip, in.game_dir);
    if (!zip.Finish()) {
      out.why = "The report could not be written. Is the SD card full?";
      return out;
    }
  }

  std::string match_name;
  bool with_match = false;
  if (!in.match_dir.empty()) {
    Zip zip(match_path);
    with_match = ZipFolder(in.match_dir, "", zip, kMaxMatch) && zip.Finish();
    if (with_match) {
      const size_t slash = in.match_dir.find_last_of('/');
      match_name = slash == std::string::npos ? in.match_dir : in.match_dir.substr(slash + 1);
    }
  }

  std::string json = "{\"format\":1,\"app\":\"game\",\"version\":" + JsonText(in.version) + ",\"platform\":\"switch\",\"os\":" +
                     JsonText(Firmware()) + ",\"problem\":" + JsonText(in.problem) + ",\"happens\":" + JsonText(in.happens) +
                     ",\"what\":" + JsonText(in.what) + ",\"before\":" + JsonText(in.before) + ",\"contact\":" + JsonText(in.contact);
  if (with_match) json += ",\"match\":" + JsonText(match_name);
  json += "}";

  u64 bits[2] = {};
  randomGet(bits, sizeof bits);
  char bound[64];
  std::snprintf(bound, sizeof bound, "----SvR2011%016llx%016llx", (unsigned long long)bits[0], (unsigned long long)bits[1]);
  const std::string boundary = bound;

  std::vector<Part> parts;
  Part report;
  report.head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"report\"\r\nContent-Type: application/json\r\n\r\n" + json;
  parts.push_back(report);
  if (Part p = FilePart(boundary, "zip", "report.zip", "application/zip", zip_path, kMaxZip + (1u << 20)); !p.file.empty())
    parts.push_back(p);
  if (!in.picture.empty())
    if (Part p = FilePart(boundary, "image0", "image0.png", "image/png", in.picture, 8u << 20); !p.file.empty()) parts.push_back(p);
  if (with_match)
    if (Part p = FilePart(boundary, "match", "match.zip", "application/zip", match_path, kMaxMatch); !p.file.empty()) parts.push_back(p);

  const std::string tail = "--" + boundary + "--\r\n";
  uint64_t total = tail.size();
  for (const Part& p : parts) total += p.head.size() + p.file_size + 2;

  auto cleanup = [&] {
    unlink(zip_path.c_str());
    unlink(match_path.c_str());
  };

  Connection c;
  if (!c.Open(url, in.game_dir, out.why)) {
    cleanup();
    return out;
  }
  const std::string host = (url.https ? url.port == 443 : url.port == 80) ? url.host : url.host + ":" + std::to_string(url.port);
  const std::string head = "POST " + url.path + "/api/reports HTTP/1.1\r\nHost: " + host +
                           "\r\nUser-Agent: SvR2011 Report (Switch)\r\nAccept: */*\r\nConnection: close\r\nAuthorization: Bearer " + in.token + "\r\n"
                           "Content-Type: multipart/form-data; boundary=" + boundary +
                           "\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n";
  bool sent = c.Write(head.data(), head.size());
  uint64_t done = 0;
  auto tick = [&](size_t n) {
    done += n;
    if (in.progress) in.progress(done, total);
  };
  std::vector<char> chunk(64u << 10);
  for (const Part& p : parts) {
    if (!sent) break;
    sent = c.Write(p.head.data(), p.head.size());
    tick(p.head.size());
    if (sent && !p.file.empty()) {
      const int fd = open(p.file.c_str(), O_RDONLY);
      sent = fd >= 0;
      while (sent) {
        const ssize_t r = read(fd, chunk.data(), chunk.size());
        if (r <= 0) break;
        sent = c.Write(chunk.data(), size_t(r));
        tick(size_t(r));
      }
      if (fd >= 0) close(fd);
    }
    sent = sent && c.Write("\r\n", 2);
    tick(2);
  }
  sent = sent && c.Write(tail.data(), tail.size());
  tick(tail.size());

  // (An early refusal arrives even when the write stopped.)
  std::string answer;
  c.ReadAll(answer);
  cleanup();

  const size_t split = answer.find("\r\n\r\n");
  int status = 0;
  if (answer.rfind("HTTP/", 0) == 0) {
    const size_t sp = answer.find(' ');
    if (sp != std::string::npos) status = std::atoi(answer.c_str() + sp + 1);
  }
  const std::string body = split == std::string::npos ? std::string() : answer.substr(split + 4);
  if (status == 200 || status == 201) {
    out.id = JsonField(body, "id");
    out.secret = JsonField(body, "secret");
    out.seen_before = std::atoi(JsonField(body, "seen_before").c_str());
    out.ok = !out.id.empty();
    if (out.ok) {
      if (!out.secret.empty()) AddReceipt(in, out);
      return out;
    }
  }
  out.account = !JsonField(body, "code").empty();  // (sign_in_needed, discord_needed, update_needed, account_banned)
  const std::string error = JsonField(body, "error");
  if (!error.empty()) out.why = error;
  else if (status) out.why = "The server answered " + std::to_string(status) + ".";
  else out.why = sent ? "No answer from the server." : "The connection to the server was lost.";
  return out;
}

}  // namespace nx_report
