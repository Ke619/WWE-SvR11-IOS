// What online needs to trust - see online_files.h.
#include "online_files.h"

#include "../modmaker/svrfmt/mod_travel.h"
#include "mod_transfer.h"
#include "no_mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include "match_types.h"
#include "online_net.h"

#ifndef SVR2011_PORT_VERSION
#define SVR2011_PORT_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace {

// -- The launcher's list --------------------------------------------------

#define VERIFY_MENU_TABLE 1  // (as launcher/verify.c)
constexpr uint64_t kMenuTableAt = 0x25F000, kMenuTableSize = 0x6800;
struct GameFile {
  const wchar_t* path;
  uint64_t size;
  uint32_t crc, crc_ported;
  int flags;
};
const GameFile kFiles[] = {
#include "../launcher/game_files_crc.inc"
};

// Files a game folder may lack (as the launcher): the disc's system update and dashboard art.
bool Optional(const std::wstring& p) { return p.rfind(L"$SystemUpdate\\", 0) == 0 || p == L"nxeart"; }

// -- CRC-32 (slicing by 8) ------------------------------------------------

uint32_t g_crc[8][256];
void CrcInit() {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i)
    for (int t = 1; t < 8; ++t) g_crc[t][i] = g_crc[0][g_crc[t - 1][i] & 0xFF] ^ (g_crc[t - 1][i] >> 8);
}
uint32_t CrcUpdate(uint32_t c, const uint8_t* p, size_t n) {
  c = ~c;
  while (n >= 8) {
    uint32_t a, b;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
    a ^= c;
    c = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^ g_crc[4][a >> 24] ^
        g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^ g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    p += 8, n -= 8;
  }
  while (n--) c = g_crc[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}
std::once_flag g_crc_once;

// A file's CRC-32 (menu.pac: its menu table left out); false if unreadable.
bool FileCrc(const fs::path& p, bool skip_menu_table, uint32_t* out) {
  std::call_once(g_crc_once, CrcInit);
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> buf(1 << 20);
  uint32_t c = 0;
  uint64_t at = 0;
  for (size_t got; (got = std::fread(buf.data(), 1, buf.size(), f)) > 0; at += got) {
    if (!skip_menu_table || at + got <= kMenuTableAt || at >= kMenuTableAt + kMenuTableSize) {
      c = CrcUpdate(c, buf.data(), got);
      continue;
    }
    if (at < kMenuTableAt) c = CrcUpdate(c, buf.data(), size_t(kMenuTableAt - at));
    if (at + got > kMenuTableAt + kMenuTableSize) {
      const size_t from = size_t(kMenuTableAt + kMenuTableSize - at);
      c = CrcUpdate(c, buf.data() + from, got - from);
    }
  }
  std::fclose(f);
  *out = c;
  return true;
}

uint64_t MTime(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  return ec ? 0 : uint64_t(t.time_since_epoch().count());
}

// -- The check ------------------------------------------------------------

std::atomic<svr2011::GameFiles> g_state{svr2011::GameFiles::kChecking};
std::mutex g_bad_mutex;
std::string g_first_bad;

struct Cached {
  uint64_t size = 0, mtime = 0;
  uint32_t crc = 0;
};

void Check(fs::path game, fs::path user_data) {
  const fs::path cache_file = user_data / "game_files_check.txt";
  std::map<std::string, Cached> cache;
  if (std::ifstream in(cache_file); in) {
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream s(line);
      std::string path;
      Cached c;
      if (std::getline(s, path, '\t') && (s >> c.size >> c.mtime >> std::hex >> c.crc)) cache[path] = c;
    }
  }
  std::map<std::string, Cached> now;
  const auto t0 = std::chrono::steady_clock::now();
  int read = 0, bad = 0;
  std::string first_bad;
  for (const GameFile& g : kFiles) {
    std::wstring wpath = g.path;
    std::string rel(wpath.begin(), wpath.end());  // (the list is ASCII)
    std::replace(rel.begin(), rel.end(), '\\', '/');
    const fs::path p = game / fs::path(rel);
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) {
      if (!Optional(wpath) && !bad++) first_bad = rel + " (missing)";
      continue;
    }
    Cached c{size, MTime(p), 0};
    if (auto it = cache.find(rel); it != cache.end() && it->second.size == c.size && it->second.mtime == c.mtime) {
      c.crc = it->second.crc;
    } else if (size == g.size) {
      if (!FileCrc(p, g.flags & VERIFY_MENU_TABLE, &c.crc)) c.crc = ~g.crc;
      ++read;
    }
    now[rel] = c;
    if (size != g.size || (c.crc != g.crc && (!g.crc_ported || c.crc != g.crc_ported)))
      if (!bad++) first_bad = rel;
  }
  if (std::ofstream out(cache_file, std::ios::trunc); out)
    for (const auto& [path, c] : now) out << path << '\t' << c.size << ' ' << c.mtime << ' ' << std::hex << c.crc << std::dec << '\n';
  {
    std::lock_guard lock(g_bad_mutex);
    g_first_bad = first_bad;
  }
  g_state = bad ? svr2011::GameFiles::kModified : svr2011::GameFiles::kClean;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (bad)
    REXLOG_WARN("online: game files modified ({} differ, first {}) - online is off ({:.1f} s, {} files read)", bad,
                first_bad, s, read);
  else
    REXLOG_INFO("online: game files clean ({} files, {} read, {:.1f} s)", std::size(kFiles), read, s);
}

// -- The newest version ---------------------------------------------------

constexpr const char* kReleases = "https://api.github.com/repos/KaikoClanworth1/wwe-svr2011-pc/releases?per_page=10";
constexpr int64_t kVersionCacheSeconds = 6 * 3600;
enum class Version { kChecking, kLatest, kOlder };
std::atomic<Version> g_version{Version::kChecking};
std::mutex g_latest_mutex;
std::string g_latest;  // the newest release's version (when known)

// a newer than b (dotted numbers, as the launcher's version_newer).
bool VersionNewer(const std::string& a, const std::string& b) {
  const char *x = a.c_str(), *y = b.c_str();
  while (*x || *y) {
    char *xe, *ye;
    const long p = std::strtol(x, &xe, 10), q = std::strtol(y, &ye, 10);
    if (p != q) return p > q;
    x = xe, y = ye;
    if (*x == '.') ++x;
    if (*y == '.') ++y;
    if ((*x && !std::isdigit(static_cast<unsigned char>(*x))) || (*y && !std::isdigit(static_cast<unsigned char>(*y))))
      break;
  }
  return false;
}

void CheckVersion(fs::path user_data) {
  rex::thread::set_current_thread_name("version check");
  const std::string mine = svr2011::PortVersion();
  if (mine.empty() || !std::isdigit(static_cast<unsigned char>(mine[0]))) {  // (a developer build: no check)
    g_version = Version::kLatest;
    return;
  }
  const fs::path cache = user_data / "online_version.txt";
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
  std::string latest;
  if (std::ifstream in(cache); in) {
    int64_t at = 0;
    std::string v;
    if ((in >> at >> v) && now - at >= 0 && now - at < kVersionCacheSeconds) latest = v;
  }
  if (latest.empty()) {
    auto r = svr2011::net::Request("GET", kReleases,
                                   {{"Accept", "application/vnd.github+json"}, {"User-Agent", "SvR2011-PC-Game"}});
    // (the first release's tag: newest first, drafts not listed)
    if (r && r->status == 200)
      if (size_t at = r->body.find("\"tag_name\""); at != std::string::npos)
        if (size_t q1 = r->body.find('"', r->body.find(':', at) + 1); q1 != std::string::npos)
          if (size_t q2 = r->body.find('"', q1 + 1); q2 != std::string::npos) {
            latest = r->body.substr(q1 + 1, q2 - q1 - 1);
            if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
          }
    if (latest.empty()) {
      REXLOG_WARN("online: the newest version couldn't be asked (GitHub {}) - online stays on",
                  r ? std::to_string(r->status) : std::string("out of reach"));
      g_version = Version::kLatest;
      return;
    }
    if (std::ofstream out(cache, std::ios::trunc); out) out << now << ' ' << latest << '\n';
  }
  {
    std::lock_guard lock(g_latest_mutex);
    g_latest = latest;
  }
  const bool older = VersionNewer(latest, mine);
  g_version = older ? Version::kOlder : Version::kLatest;
  if (older)
    REXLOG_WARN("online: version {} - the newest is {}: online is off until the update", mine, latest);
  else
    REXLOG_INFO("online: version {} (the newest: {})", mine, latest);
}

// -- The gameplay mod set -------------------------------------------------

std::string ManifestValue(const fs::path& manifest, const char* key) {
  std::ifstream in(manifest);
  std::string line;
  const std::string want = std::string(key) + "=";
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(want, 0) == 0) return line.substr(want.size());
  }
  return "";
}

// Pictures, sounds and movies: cosmetic (left out of a mod's CRC). Dot files too: the Mod Maker's project settings
// (.mm_project.txt) and local markers (.applied) aren't the mod.
// (the files outside a mod's key: one rule with what may travel online - svrfmt/mod_travel.h)
bool Cosmetic(const fs::path& p) { return svrfmt::CosmeticFile(p); }

// A mod folder's CRC for the set: its files but the cosmetic ones in a stable order, each one's path and CRC
// folded (the same for a downloaded copy: mod_transfer.cpp checks it against the host's). The manifest by the lines
// that are the mod's, not this PC's (mod_travel.h): the launcher's level stamp made every bundled mod differ on every
// PC (report b7e708).
uint32_t FolderCrc(const fs::path& dir) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& f : fs::recursive_directory_iterator(dir, ec))
    if (f.is_regular_file() && !Cosmetic(f.path())) files.push_back(f.path());
  std::sort(files.begin(), files.end());
  uint32_t crc = 0;
  for (const fs::path& f : files) {
    uint32_t c = 0;
    const std::string rel = fs::relative(f, dir, ec).generic_string();
    if (rel == "manifest.txt") {
      std::ifstream in(f, std::ios::binary);
      if (!in) continue;
      const std::string text = svrfmt::ManifestKeyText(std::string(std::istreambuf_iterator<char>(in), {}));
      std::call_once(g_crc_once, CrcInit);
      c = CrcUpdate(0, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    } else if (!FileCrc(f, false, &c)) {
      continue;
    }
    crc = CrcUpdate(crc, reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
    crc = CrcUpdate(crc, reinterpret_cast<const uint8_t*>(&c), 4);
  }
  return crc;
}

std::string BuildModSet() {
  if (svr2011::NoMods()) return {};  // (Start Without Mods: no_mods.h)
  std::vector<std::string> lines;
  const fs::path mods = rex::filesystem::GetExecutableFolder() / "Mods";
  std::error_code ec;
  for (const char* kind : {"Superstars", "Moves", "Arenas", "Backstage"}) {
    if (svr2011::BackstageModsOff() && std::string(kind) == "Backstage") continue;  // (no_mods.h: not loaded)
    // (a run that loads the host's fetched mods: those too - <folder>@<crc>, its key the host's; only the kinds
    // that load from there: superstar_mods.cpp)
    std::vector<std::pair<fs::directory_entry, bool>> entries;  // (fetched: its folder is <id>@<crc>)
    std::set<std::string> own;
    for (const auto& e : fs::directory_iterator(mods / kind, ec))
      entries.push_back({e, false}), own.insert(e.path().filename().string());
    if (svr2011::OnlineModsLoad() && !std::strcmp(kind, "Superstars"))
      for (const auto& e : fs::directory_iterator(svr2011::OnlineModsRoot() / kind, ec)) entries.push_back({e, true});
    for (const auto& [e, fetched] : entries) {
      if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
      std::string id = e.path().filename().string();
      if (fetched) {
        id = id.substr(0, id.rfind('@'));
        if (own.count(id)) continue;  // (this game's own of that name plays: superstar_mods.cpp)
      }
      const uint32_t crc = FolderCrc(e.path());
      const fs::path manifest = e.path() / "manifest.txt";
      std::string name = ManifestValue(manifest, "name"), version = ManifestValue(manifest, "version");
      if (name.empty()) name = id;
      if (version.empty()) version = "-";
      char hex[16];
      std::snprintf(hex, sizeof(hex), "%08X", crc);
      lines.push_back(std::string(kind) + "\t" + id + "\t" + version + "\t" + hex + "\t" + name);
    }
  }
  // (The match type switches - Mods\MatchTypes - aren't in the set: they
  // only add or remove the port's types from the menus; online the host
  // picks the match (PORT RULES) and in a match session every type is on
  // for both games: match_types.h MatchTypeOn.)
  // Test aid: SVR2011_TEST_MOD_SET_EXTRA="<kind>|<id>|<version>|<name>" - one
  // more mod in this game's set (two test games share one Mods folder).
  if (const char* v = std::getenv("SVR2011_TEST_MOD_SET_EXTRA"); v && *v) {
    std::string l = v;
    std::replace(l.begin(), l.end(), '|', '\t');
    const size_t at = l.find('\t', l.find('\t', l.find('\t') + 1) + 1);  // (after the version: a made-up CRC)
    if (at != std::string::npos) lines.push_back(l.substr(0, at) + "\t00000000" + l.substr(at));
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const std::string& l : lines) out += l + "\n";
  return out;
}

struct Line {
  std::string kind, id, version, crc, name;
};
std::map<std::string, Line> ParseSet(const std::string& set) {
  std::map<std::string, Line> out;
  std::istringstream s(set);
  std::string l;
  while (std::getline(s, l)) {
    Line x;
    std::istringstream f(l);
    if (std::getline(f, x.kind, '\t') && std::getline(f, x.id, '\t') && std::getline(f, x.version, '\t') &&
        std::getline(f, x.crc, '\t') && std::getline(f, x.name) &&
        x.kind != "MatchTypes")  // (2.1.1 and older sent their match type switches off: left out)
      out[x.kind + "/" + x.id] = x;
  }
  return out;
}

std::string Describe(const Line& l) {
  return l.version == "-" ? l.name : l.name + " " + l.version;
}

}  // namespace

namespace svr2011 {

namespace {
fs::path g_game_root, g_user_data;
}  // namespace

void SetGameFilesPaths(const fs::path& game_root, const fs::path& user_data) {
  g_game_root = game_root;
  g_user_data = user_data;
}

const fs::path& UserDataFolder() { return g_user_data; }

uint32_t ModFolderCrc(const fs::path& dir) { return FolderCrc(dir); }

uint32_t Crc32(uint32_t crc, const void* data, size_t n) {
  std::call_once(g_crc_once, CrcInit);
  return CrcUpdate(crc, static_cast<const uint8_t*>(data), n);
}

bool FileCrc32(const fs::path& file, uint32_t* out) { return FileCrc(file, false, out); }

std::vector<MissingGameFile> MissingGameFiles() {
  std::vector<MissingGameFile> out;
  const char* test = std::getenv("SVR2011_TEST_MISSING_GAME_FILE");
  std::error_code ec;
  for (const GameFile& g : kFiles) {
    const std::wstring p = g.path;
    if (Optional(p)) continue;
    std::string u;
    for (wchar_t c : p) u += char(c);  // (the list's names are ASCII)
    // (slashes, as the online check: on Android a backslash is part of a name - every file in a subfolder counted as
    // missing there, "360 of the disc's files missing (265 ...)" in every Android 2.1.9 log)
    std::string rel = u;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (fs::exists(g_game_root / fs::path(rel), ec) && !(test && u == test)) continue;
    const bool essential = p == L"default.xex" || p.rfind(L"plist360", 0) == 0 || p.rfind(L"pac\\", 0) == 0;
    out.push_back({u, essential});
  }
  return out;
}

void SupportedXex(uint64_t* size, uint32_t* crc) {
  *size = 0, *crc = 0;
  for (const GameFile& g : kFiles)
    if (std::wstring(g.path) == L"default.xex") *size = g.size, *crc = g.crc;
}

int DefaultXexSupported(uint64_t* size, uint32_t* crc) {
  uint64_t want_size;
  uint32_t want_crc;
  std::error_code ec;
  const fs::path xex = g_game_root / "default.xex";
  *size = 0, *crc = 0;
  SupportedXex(&want_size, &want_crc);
  if (!want_size || !fs::exists(xex, ec)) return 1;
  *size = uint64_t(fs::file_size(xex, ec));
  if (ec) return -1;
  if (const char* t = std::getenv("SVR2011_TEST_OTHER_XEX"); t && *t == '1') return 0;
  if (*size != want_size) return 0;   // (a different size: no need to read it)
  if (!FileCrc32(xex, crc)) return -1;
  return *crc == want_crc ? 1 : 0;
}

void StartGameFilesCheck() {
  static std::once_flag once;
  std::call_once(once, [game_root = g_game_root, user_data = g_user_data] {
    // Test aid: SVR2011_TEST_FILES_ROOT=<folder> - that folder checked instead
    // (a copy with an edited file, the game still running from its own).
    fs::path root = game_root;
    if (const char* v = std::getenv("SVR2011_TEST_FILES_ROOT"); v && *v) root = v;
    std::thread([root, user_data] {
      rex::thread::set_current_thread_name("file check");
      // (background priority, low disk priority too: a first check - every
      // file hashed - mustn't compete with the game's own loading)
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#elif defined(__linux__) || defined(__ANDROID__)
      setpriority(PRIO_PROCESS, 0, 19);  // (this thread: Linux nice values are per thread)
#endif
      Check(root, user_data);
    }).detach();
    std::thread([user_data] { CheckVersion(user_data); }).detach();
  });
}

GameFiles GameFilesState(std::string* first_bad) {
  const GameFiles s = g_state.load();
  if (first_bad) {
    std::lock_guard lock(g_bad_mutex);
    *first_bad = g_first_bad;
  }
  return s;
}

std::string GameFilesOnlineMessage() {
  const GameFiles files = GameFilesState();
  if (files == GameFiles::kModified)
    return "Your game files are modified - online is off.\nUse Verify game files in the launcher to repair them.";
  if (g_version.load() == Version::kOlder) {
    std::lock_guard lock(g_latest_mutex);
    return "Unable to play online due to a difference in game versions.\nUpdate to v" + g_latest +
           " (the launcher's Check for updates, or GitHub) to play online.";
  }
  if (files == GameFiles::kChecking || g_version.load() == Version::kChecking)
    return "Checking your game files.\nOnline will be ready in a moment - please try again.";
  return ModsOnlineMessage();
}

const std::string& PortVersion() {
  static const std::string v = [] {
    const char* t = std::getenv("SVR2011_TEST_VERSION");
    return std::string(t && *t ? t : SVR2011_PORT_VERSION);
  }();
  return v;
}

std::string VersionDifference(const std::string& theirs) {
  if (theirs == PortVersion()) return "";
  if (!theirs.empty() && VersionNewer(theirs, PortVersion()))
    return "They're on v" + theirs + ", you're on v" + PortVersion() + ": update to v" + theirs +
           " (launcher or GitHub) to play them.";
  return "They're on " + (theirs.empty() ? std::string("an older version") : "v" + theirs) + ", you're on v" +
         PortVersion() + ": they need to update.";
}

std::atomic<bool> g_mods_online{true};  // (the server's switch: SetOnlineModsAllowed)
std::atomic<int> g_mods_server{0};       // (0: not asked yet, 1: it answered, 2: it didn't - OnlineServerUnanswered)

void SetOnlineModsAllowed(bool on) {
  g_mods_server = 1;
  if (g_mods_online.exchange(on) != on)
    REXLOG_WARN("online: mods in online sessions {} by the server", on ? "allowed again" : "switched off");
}

void OnlineServerUnanswered() { g_mods_server = 2; }

std::mutex g_block_mutex;
std::string g_account_block;  // (the server's reason this account can't play online; "" none)

void SetOnlineAccountBlock(const std::string& why) {
  std::lock_guard lock(g_block_mutex);
  if (why != g_account_block && !why.empty()) REXLOG_WARN("online: the server keeps this account off online: {}", why);
  g_account_block = why;
}

const std::vector<std::string>& ActiveMatchMods() {
  static const std::vector<std::string> mods = [] {
    std::vector<std::string> out;
    if (NoMods()) return out;  // (Start Without Mods: none loaded - no_mods.h)
    std::error_code ec;
    const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods";
    for (const char* kind : {"Superstars", "Moves", "Arenas", "Backstage", "Weapons", "Attires", "Media"}) {
      if (BackstageModsOff() && std::string(kind) == "Backstage") continue;  // (no_mods.h: not loaded)
      for (const auto& e : fs::directory_iterator(dir / kind, ec))
        if (e.is_directory() && !fs::exists(e.path() / "disabled", ec))
          out.push_back(std::string(kind) + "/" + e.path().filename().string());
    }
    for (const auto& [id, on] : MatchTypeSwitches())  // (the restored match types: on unless "disabled")
      if (on) out.push_back("MatchTypes/" + id);
    // (the online mod tests' extra mod, SVR2011_TEST_MOD_SET_EXTRA "<kind>|<id>|...": BuildModSet)
    if (const char* v = std::getenv("SVR2011_TEST_MOD_SET_EXTRA"); v && *v) {
      std::string l = v;
      const size_t a = l.find('|'), b = a == std::string::npos ? a : l.find('|', a + 1);
      if (b != std::string::npos) out.push_back(l.substr(0, a) + "/" + l.substr(a + 1, b - a - 1));
    }
    std::sort(out.begin(), out.end());
    return out;
  }();
  return mods;
}

std::string ModsOnlineMessage() {
  {  // (the account first: the server's own words, in lines the dialog shows - about 56 characters)
    std::lock_guard lock(g_block_mutex);
    if (!g_account_block.empty()) {
      std::string out = "Online is off for your account.\n", line;
      std::istringstream words(g_account_block + " Use the launcher's Online tab.");
      for (std::string w; words >> w;) {
        if (!line.empty() && line.size() + 1 + w.size() > 56) out += line + "\n", line.clear();
        line += (line.empty() ? "" : " ") + w;
      }
      return out + line;
    }
  }
  // (big_roster's ids 323-511 play online as any other mod's: the lobby's key translation, a mod an "added"
  // superstar at any id and its match slot filled - superstar_mods.cpp; part 5, proven in LAN matches)
  const auto& mods = ActiveMatchMods();
  const int server = g_mods_server.load();
  const bool blocked = !mods.empty() && !(server == 1 && g_mods_online.load());
  static std::atomic<int> said{-1};  // (the log: once per change)
  const int now = blocked ? server : -2;
  if (said.exchange(now) != now && blocked) {
    std::string list;
    for (size_t i = 0; i < mods.size() && i < 6; ++i) list += (i ? ", " : "") + mods[i];
    if (mods.size() > 6) list += ", ... (" + std::to_string(mods.size()) + " in all)";
    REXLOG_WARN("online: matches off - {} mod(s) on ({}) and the server's mods_online {}", mods.size(), list,
                server == 1 ? "is off" : server == 2 ? "unknown (no answer: as off)" : "not known yet");
  }
  if (!blocked) return "";
  if (server == 0)
    return "Checking the online server.\nOnline will be ready in a moment - please try again.";
  // (the launcher's button / the app's tab: their own English names in every language)
  struct Text {
    uint32_t language;
    const char* text;
  };
#ifdef __ANDROID__  // (the phone has no Start Without Mods: the SvR 2011 app's Mods tab switches them off)
  static const Text kTexts[] = {
      {1, "You have mods enabled.\nPlease turn your mods off in the Mods tab of the SvR 2011 app, then restart the "
          "game to play online matches."},
      {4, "Vous avez des mods activ\xC3\xA9s.\nD\xC3\xA9sactivez vos mods dans l'onglet Mods de l'app SvR 2011, "
          "puis relancez le jeu pour jouer des matchs en ligne."},
      {3, "Du hast Mods aktiviert.\nSchalte deine Mods im Tab Mods der SvR 2011 App aus und starte das Spiel neu, um "
          "Online-Matches zu spielen."},
      {5, "Tienes mods activados.\nDesactiva tus mods en la pesta\xC3\xB1" "a Mods de la app SvR 2011 y vuelve a "
          "iniciar el juego para jugar partidas en l\xC3\xADnea."},
      {6, "Hai delle mod attive.\nDisattiva le mod nella scheda Mods dell'app SvR 2011, poi riavvia il gioco per "
          "giocare partite online."},
  };
#else
  static const Text kTexts[] = {
      {1, "You have mods enabled.\nPlease relaunch the game with Start Without Mods in the launcher to play online "
          "matches."},
      {4, "Vous avez des mods activ\xC3\xA9s.\nRelancez le jeu avec Start Without Mods dans le lanceur pour jouer "
          "des matchs en ligne."},
      {3, "Du hast Mods aktiviert.\nStarte das Spiel im Launcher mit Start Without Mods neu, um Online-Matches zu "
          "spielen."},
      {5, "Tienes mods activados.\nVuelve a iniciar el juego con Start Without Mods en el launcher para jugar "
          "partidas en l\xC3\xADnea."},
      {6, "Hai delle mod attive.\nRiavvia il gioco con Start Without Mods nel launcher per giocare partite online."},
  };
#endif
  const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
  for (const Text& t : kTexts)
    if (t.language == language) return t.text;
  return kTexts[0].text;
}

bool OnlineModsAllowed() { return g_mods_online.load(); }

const std::string& GameplayModSet() {
  static const std::string none;
  if (!OnlineModsAllowed()) return none;  // (mods off online: no set to compare, none to share)
  static const std::string set = [] {
    std::string s = BuildModSet();
    REXLOG_INFO("online: gameplay mods for online: {}", s.empty() ? std::string("none") : "\n" + s);
    return s;
  }();
  return set;
}

bool ModKey(const std::string& kind, const std::string& folder, std::string* key, std::string* label) {
  if (!OnlineModsAllowed()) return false;  // (mods off online: no mod can be picked)
  static const auto set = ParseSet(GameplayModSet());
  const auto it = set.find(kind + "/" + folder);
  if (it == set.end()) return false;
  if (key) *key = it->second.id + "	" + it->second.version + "	" + it->second.crc;
  if (label) *label = Describe(it->second);
  return true;
}

uint64_t GameplayModSetHash() {
  const std::string& s = GameplayModSet();
  if (s.empty()) return 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char ch : s) h = (h ^ ch) * 0x100000001B3ull;
  return h;
}

const std::string& DlcSet() {
  static const std::string set = [] {
    std::vector<std::string> lines;
    const fs::path title = UserDataFolder() / "0000000000000000" / "5451085D";
    std::error_code ec;
    for (const auto& pkg : fs::directory_iterator(title / "00000002", ec)) {
      if (!pkg.is_directory(ec)) continue;
      // (its game files: names and sizes, the pac folder's - movies, sounds and the catalog don't play)
      std::vector<std::pair<std::string, uint64_t>> files;
      for (const auto& f : fs::recursive_directory_iterator(pkg.path() / "pac", ec)) {
        if (!f.is_regular_file(ec)) continue;
        std::string rel = fs::relative(f.path(), pkg.path(), ec).generic_string();
        std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (rel.rfind("pac/string", 0) == 0) continue;  // (PatchOnlineStrings: the wording)
        files.push_back({rel, uint64_t(f.file_size(ec))});
      }
      if (files.empty()) continue;  // (a catalog only: Fan Axxess)
      std::sort(files.begin(), files.end());
      uint64_t h = 0xCBF29CE484222325ull;
      for (const auto& [name, size] : files) {
        for (unsigned char ch : name) h = (h ^ ch) * 0x100000001B3ull;
        for (int i = 0; i < 8; ++i) h = (h ^ uint8_t(size >> (8 * i))) * 0x100000001B3ull;
      }
      // (its name: the content header's, UTF-16BE at 8 - "WWE SmackDown vs. Raw 2011 - Vol02" -> "DLC Vol02")
      std::string name;
      {
        std::ifstream in(title / "Headers" / "00000002" / (pkg.path().filename().string() + ".header"), std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t i = 8; i + 1 < d.size() && i < 8 + 256; i += 2) {
          const uint16_t c = uint16_t(d[i] << 8 | d[i + 1]);
          if (!c) break;
          name += c >= 0x20 && c < 0x7F ? char(c) : c == 0x2013 || c == 0x2014 ? '-' : '?';
        }
      }
      for (const char* game : {"WWE SmackDown vs. Raw 2011", "WWE SmackDown vs Raw 2011"})
        if (name.rfind(game, 0) == 0) name = name.substr(std::strlen(game));
      while (!name.empty() && (name.front() == ' ' || name.front() == '-' || name.front() == ':')) name.erase(0, 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      if (name.empty()) name = pkg.path().filename().string().substr(0, 12);
      if (name.rfind("DLC", 0) != 0) name = "DLC " + name;
      char hex[24];
      std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(h));
      lines.push_back(std::string("DLC\t") + hex + "\t-\t" + hex + "\t" + name);
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    REXLOG_INFO("online: DLC packages with game files: {}", out.empty() ? std::string("none") : "\n" + out);
    return out;
  }();
  // (test aid: SVR2011_TEST_DLC_SET=none | "<fingerprint>|<name>,..." - this game's set instead: two test games
  // share one user data's DLC)
  if (const char* v = std::getenv("SVR2011_TEST_DLC_SET"); v && *v) {
    static const std::string test = [v] {
      std::vector<std::string> lines;
      std::stringstream ss(std::string(v) == "none" ? std::string() : std::string(v));
      for (std::string item; std::getline(ss, item, ',');) {
        const size_t bar = item.find('|');
        if (bar == std::string::npos) continue;
        lines.push_back("DLC\t" + item.substr(0, bar) + "\t-\t" + item.substr(0, bar) + "\t" + item.substr(bar + 1));
      }
      std::sort(lines.begin(), lines.end());
      std::string out;
      for (const std::string& l : lines) out += l + "\n";
      REXLOG_INFO("online: DLC packages (test aid SVR2011_TEST_DLC_SET): {}", out.empty() ? std::string("none") : "\n" + out);
      return out;
    }();
    return test;
  }
  return set;
}

std::string DlcDifference(const std::string& mine, const std::string& theirs) {
  const auto a = ParseSet(mine), b = ParseSet(theirs);
  std::vector<std::string> parts;
  for (const auto& [key, l] : b)
    if (!a.count(key)) parts.push_back("they have " + l.name + ", you don't");
  for (const auto& [key, l] : a)
    if (!b.count(key)) parts.push_back("you have " + l.name + ", they don't");
  if (parts.empty()) return "";
  std::string out = "Your downloadable content doesn't match: ";
  for (size_t i = 0; i < parts.size() && i < 2; ++i) out += (i ? "; " : "") + parts[i];
  if (parts.size() > 2) out += "; and " + std::to_string(parts.size() - 2) + " more";
  return out + ".";
}

std::string ModSetDifference(const std::string& mine, const std::string& theirs, bool superstars) {
  auto a = ParseSet(mine), b = ParseSet(theirs);
  if (!superstars) {  // (checked in the lobby: online_mods.h)
    for (auto* m : {&a, &b})
      for (auto it = m->begin(); it != m->end();)
        it = it->second.kind == "Superstars" || it->second.kind == "Arenas" || it->second.kind == "Moves" ||
                     it->second.kind == "Backstage"
                 ? m->erase(it) : std::next(it);
  }
  std::vector<std::string> parts;
  for (const auto& [key, l] : b)
    if (!a.count(key)) parts.push_back("they have " + Describe(l) + ", you don't");
  for (const auto& [key, l] : a)
    if (!b.count(key)) parts.push_back("you have " + Describe(l) + ", they don't");
  for (const auto& [key, l] : a)
    if (auto it = b.find(key); it != b.end() && (it->second.version != l.version || it->second.crc != l.crc))
      parts.push_back(it->second.version != l.version
                          ? l.name + ": yours is " + l.version + ", theirs " + it->second.version
                          : l.name + ": your files differ from theirs");
  if (parts.empty()) return "";
  std::string out = "Your mods don't match: ";
  // (the game's message box holds about three lines: two named, then a count)
  for (size_t i = 0; i < parts.size() && i < 2; ++i) out += (i ? "; " : "") + parts[i];
  if (parts.size() > 2) out += "; and " + std::to_string(parts.size() - 2) + " more";
  return out + ".";
}

}  // namespace svr2011
