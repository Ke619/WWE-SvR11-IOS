// SvR2011 Mod Maker: the session log and "Report a problem" (session_log.h).

#include "session_log.h"

#include <share.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <csignal>
#include <exception>
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "app.h"
#include "mod_check.h"
#include "tool_run.h"
#include "../launcher/report_send.h"

#ifndef PORT_VERSION
#define PORT_VERSION "dev"
#endif

namespace mm::session {

namespace {

constexpr size_t kKeepLogs = 10;          // session logs kept in the folder
constexpr size_t kReportLogs = 5;         // this one and the 4 before go in a report
constexpr size_t kTextCap = 8u << 20;     // a log in a report: its last 8 MB
constexpr size_t kSmallCap = 2u << 20;    // anything else: its last 2 MB

std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_tried = false;            // (Open once: a folder that can't be written isn't retried each frame)
fs::path g_path;                     // this session's log
std::vector<std::string> g_pending;  // lines before the file is open
const auto g_start = std::chrono::steady_clock::now();
PageId g_last_page = PageId::kCount;

std::string Stamp(const char* format) {
  char b[64];
  const std::time_t t = std::time(nullptr);
  std::tm tm = {};
  localtime_s(&tm, &t);
  std::strftime(b, sizeof b, format, &tm);
  return b;
}

std::string TimePrefix() {
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  char b[48];
  std::snprintf(b, sizeof b, "[%s +%.1fs] ", Stamp("%H:%M:%S").c_str(), s);
  return b;
}

fs::path SettingsDir() {  // (as app.cpp's, with its test override)
  if (const wchar_t* own = _wgetenv(L"SVR2011_MM_SETTINGS"); own && *own) return fs::path(own);
  wchar_t* p = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / L"SvR2011 Mod Maker";
  if (p) CoTaskMemFree(p);
  return dir;
}

fs::path LogDir() { return g_game.empty() ? SettingsDir() / L"logs" : fs::path(g_game) / L"logs"; }

// The Mod Maker's logs in a folder, newest first.
std::vector<fs::path> Logs(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::wstring n = e.path().filename().wstring();
    if (e.is_regular_file(ec) && n.rfind(L"modmaker_", 0) == 0 && e.path().extension() == L".log") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
  return out;  // (names are modmaker_<date>-<time>: by name = by age)
}

std::string WindowsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v = {sizeof v};
  if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
  return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
}

std::string GpuName() {
  if (!g_dev) return "?";
  std::string name = "?";
  IDXGIDevice* d = nullptr;
  if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&d)))) {
    IDXGIAdapter* a = nullptr;
    if (SUCCEEDED(d->GetAdapter(&a))) {
      DXGI_ADAPTER_DESC desc = {};
      if (SUCCEEDED(a->GetDesc(&desc))) name = Utf8(desc.Description);
      a->Release();
    }
    d->Release();
  }
  return name;
}

// The version, the system and the settings: the log's first lines and the report's report.txt.
std::string SystemText() {
  std::string t = "Mod Maker " PORT_VERSION "\n";
  t += "Windows " + WindowsVersion() + "\n";
  SYSTEM_INFO si = {};
  GetNativeSystemInfo(&si);
  MEMORYSTATUSEX m = {sizeof m};
  GlobalMemoryStatusEx(&m);
  t += "CPU threads " + std::to_string(si.dwNumberOfProcessors) + ", memory " + Human(size_t(m.ullTotalPhys)) +
       " (" + Human(size_t(m.ullAvailPhys)) + " free), GPU " + GpuName() + "\n";
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  t += "program " + Utf8(exe) + "\n";
  t += "command line " + Utf8(GetCommandLineW()) + "\n";
  char scale[32];
  std::snprintf(scale, sizeof scale, "%.2f", g_scale);
  t += "game folder " + (g_game.empty() ? std::string("(none)") : Utf8(g_game)) + "\n";
  t += "settings: text size " + std::to_string(g_settings.font) + " px, UI scale " + scale + ", log " +
       (g_settings.log_open ? "open" : "closed") + ", Bink 2 tool " +
       (g_settings.bink2_tool.empty() ? std::string("(look for one)") : Utf8(g_settings.bink2_tool)) + "\n";
  for (const auto& d : g_settings.my_discs) t += "my game discs: " + d.key + " (" + d.id + ") " + Utf8(d.path) + "\n";
  if (g_settings.my_discs.empty()) t += "my game discs: (none set)\n";
  for (const auto& r : g_settings.recent) t += "settings: recent project " + Utf8(r) + "\n";
  return t;
}

// the last lines, for a crash file (kept apart from the file: a crash can't wait for g_mutex)
constexpr size_t kTail = 40;
std::string g_tail[kTail];
std::atomic<size_t> g_tail_n{0};

void WriteLine(const std::string& line) {  // (g_mutex held)
  g_tail[g_tail_n.fetch_add(1) % kTail] = line;
  if (!g_file) {
    if (g_pending.size() < 20000) g_pending.push_back(line);
    return;
  }
  std::fputs(line.c_str(), g_file);
  std::fputc('\n', g_file);
  std::fflush(g_file);
}

// ---------------------------------------------------------------- crashes

std::atomic<bool> g_crashed{false};

std::string ModuleAt(DWORD64 addr) {  // "<module>+0x<offset>"
  HMODULE m = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &m) && m) {
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\');
    char b[MAX_PATH + 32];
    std::snprintf(b, sizeof b, "%s+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(addr - DWORD64(m)));
    return b;
  }
  char b[32];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(addr));
  return b;
}

std::string StackText(CONTEXT* from) {  // up to 40 frames, module+offset each
  CONTEXT c;
  if (from) c = *from;
  else RtlCaptureContext(&c);
  STACKFRAME64 f = {};
  f.AddrPC.Offset = c.Rip, f.AddrPC.Mode = AddrModeFlat;
  f.AddrFrame.Offset = c.Rbp, f.AddrFrame.Mode = AddrModeFlat;
  f.AddrStack.Offset = c.Rsp, f.AddrStack.Mode = AddrModeFlat;
  const HANDLE proc = GetCurrentProcess();
  static bool sym = SymInitialize(proc, nullptr, TRUE) != FALSE;
  (void)sym;
  std::string out;
  for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &c, nullptr,
                                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
    if (!f.AddrPC.Offset) break;
    out += "  " + ModuleAt(f.AddrPC.Offset) + "\n";
  }
  return out;
}

void WriteCrash(const std::string& what, CONTEXT* ctx) {
  if (g_crashed.exchange(true)) return;  // (one report: a crash while writing this one stays quiet)
  std::string t = "==== SvR2011 Mod Maker crash ====\nMod Maker " PORT_VERSION "\nwhen " + Stamp("%Y-%m-%d %H:%M:%S") +
                  "\nwhat " + what + "\npage " + PageName(g_page) + "\nthread " +
                  std::to_string(GetCurrentThreadId()) + "\nstack:\n" + StackText(ctx) + "last log lines:\n";
  const size_t n = g_tail_n.load();
  for (size_t i = n > kTail ? n - kTail : 0; i < n; ++i) t += "  " + g_tail[i % kTail] + "\n";
  std::error_code ec;
  const fs::path dir = g_path.empty() ? LogDir() : g_path.parent_path();
  fs::create_directories(dir, ec);
  if (FILE* f = _wfopen((dir / Wide("modmaker_crash_" + Stamp("%Y%m%d-%H%M%S") + ".txt")).c_str(), L"wb")) {
    std::fputs(t.c_str(), f);
    std::fclose(f);
  }
  if (g_mutex.try_lock()) {  // (the session log: its CRASH line - not when the crash came while the log was held)
    if (g_file) std::fprintf(g_file, "%sCRASH: %s (modmaker_crash_*.txt in the logs folder)\n", TimePrefix().c_str(), what.c_str()), std::fflush(g_file);
    g_mutex.unlock();
  }
}

LONG WINAPI OnException(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD* r = e->ExceptionRecord;
  char b[256];
  std::snprintf(b, sizeof b, "exception 0x%08lx at %s", r->ExceptionCode, ModuleAt(DWORD64(r->ExceptionAddress)).c_str());
  std::string what = b;
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    std::snprintf(b, sizeof b, " (%s 0x%llx)", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "running" : "reading",
                  static_cast<unsigned long long>(r->ExceptionInformation[1]));
    what += b;
  }
  if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) what += " (stack overflow)";
  WriteCrash(what, e->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashHandler() {
  ULONG guarantee = 64 * 1024;  // (room for the handler on the UI thread when its stack overflows)
  SetThreadStackGuarantee(&guarantee);
  SetUnhandledExceptionFilter(OnException);
  std::set_terminate([] {
    std::string what = "std::terminate";
    try {
      if (auto p = std::current_exception()) std::rethrow_exception(p);
    } catch (const std::exception& x) {
      what += std::string(" (") + x.what() + ")";
    } catch (...) {
    }
    WriteCrash(what, nullptr);
    std::abort();
  });
  std::signal(SIGABRT, [](int) { WriteCrash("abort", nullptr); });
  _set_purecall_handler([] { WriteCrash("pure virtual call", nullptr); std::abort(); });
  _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    WriteCrash("bad CRT parameter", nullptr);
    std::abort();
  });
}

void PresentResult(long hr) {
  static bool told = false;
  if (SUCCEEDED(hr) || told) return;
  told = true;
  char b[160];
  const long why = g_dev ? long(g_dev->GetDeviceRemovedReason()) : 0;
  std::snprintf(b, sizeof b, "GPU: Present failed 0x%08lx, device removed reason 0x%08lx (a driver reset or crash: the 3D views stop)",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(why));
  Line(b);
}

namespace {

void Open() {
  std::lock_guard lock(g_mutex);
  if (g_file || g_tried) return;
  g_tried = true;
  const fs::path dir = LogDir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  // (old sessions: the newest kKeepLogs - 1 stay, with this one kKeepLogs)
  const auto old = Logs(dir);
  for (size_t i = kKeepLogs - 1; i < old.size(); ++i) fs::remove(old[i], ec);
  g_path = dir / Wide("modmaker_" + Stamp("%Y%m%d-%H%M%S") + ".log");
  g_file = _wfsopen(g_path.c_str(), L"wb", _SH_DENYWR);
  if (!g_file) return;
  std::string head = "==== SvR2011 Mod Maker session log ====\n" + SystemText();
  std::fputs(head.c_str(), g_file);
  for (const auto& l : g_pending) std::fputs((l + "\n").c_str(), g_file);
  g_pending.clear();
  std::fflush(g_file);
}

// ---------------------------------------------------------------- privacy

// Internet addresses (x.x.x.x; loopback kept, lines with "ersion" kept: version numbers), the Windows user folder
// (%USERPROFILE%) and secrets (a value given to a password / token / key: "password=..." keeps "password=").
std::string Redact(std::string text) {
  // the user folder, either slash, any case
  wchar_t* prof = nullptr;
  std::string user;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) user = Utf8(prof);
  if (prof) CoTaskMemFree(prof);
  if (user.size() > 3) {
    for (int slash = 0; slash < 2; ++slash) {
      std::string u = user;
      if (slash) std::replace(u.begin(), u.end(), '\\', '/');
      const std::string lu = Lower(u);
      std::string lt = Lower(text);
      for (size_t at = lt.find(lu); at != std::string::npos; at = lt.find(lu, at + 13)) {
        text.replace(at, u.size(), "%USERPROFILE%");
        lt.replace(at, u.size(), "%userprofile%");
      }
    }
  }
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    const std::string low = Lower(line);
    // (a value given to a password / token / key: "password=...", "token: ...", "\"api_key\": ...")
    for (const char* secret : {"password", "passwd", "token", "secret", "api_key", "apikey"})
      if (const size_t at = low.find(secret); at != std::string::npos) {
        size_t p = at + std::strlen(secret);
        while (p < line.size() && p < at + std::strlen(secret) + 4 && (line[p] == ' ' || line[p] == '"' || line[p] == '\''))
          ++p;
        if (p < line.size() && (line[p] == '=' || line[p] == ':')) {
          line = line.substr(0, p + 1) + " (removed)";
          break;
        }
      }
    if (low.find("ersion") == std::string::npos) {
      std::string r;
      for (size_t k = 0; k < line.size();) {
        auto digit = [&](size_t p) { return p < line.size() && line[p] >= '0' && line[p] <= '9'; };
        if (digit(k) && (k == 0 || (!digit(k - 1) && line[k - 1] != '.'))) {
          size_t p = k;
          int parts = 0, first = -1;
          bool ok = true;
          for (; parts < 4 && ok; ++parts) {
            const size_t s = p;
            int v = 0;
            while (digit(p) && p - s < 3) v = v * 10 + (line[p++] - '0');
            if (p == s || v > 255) ok = false;
            if (parts == 0) first = v;
            if (ok && parts < 3) {
              if (p < line.size() && line[p] == '.') ++p;
              else ok = false;
            }
          }
          if (ok && !digit(p) && !(p + 1 < line.size() && line[p] == '.' && digit(p + 1))) {
            r += first == 127 ? line.substr(k, p - k) : "x.x.x.x";
            k = p;
            continue;
          }
        }
        r += line[k++];
      }
      line = r;
    }
    out += line;
    if (e < text.size()) out += '\n';
    i = e + 1;
  }
  return out;
}

// A text file's last `cap` bytes (shared: the session log is still being written); "" if it can't be read.
std::string ReadTail(const fs::path& file, size_t cap) {
  FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
  if (!f) return "";
  _fseeki64(f, 0, SEEK_END);
  const long long n = _ftelli64(f);
  const long long c = static_cast<long long>(cap), from = n > c ? n - c : 0;
  _fseeki64(f, from, SEEK_SET);
  std::string s(size_t(n - from), '\0');
  s.resize(std::fread(s.data(), 1, s.size(), f));
  std::fclose(f);
  if (from) s = "(... the first " + Human(size_t(from)) + " left out)\n" + s;
  return s;
}

bool IsText(const fs::path& p) {
  const std::string x = Lower(p.extension().string());
  return x == ".txt" || x == ".json" || x == ".log" || x == ".ini" || x == ".toml" || x == ".csv";
}

// The newest file under `dir` whose name matches (recursive).
fs::path Newest(const fs::path& dir, const std::function<bool(const fs::path&)>& match) {
  fs::path best;
  fs::file_time_type when{};
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec) || !match(it->path())) continue;
    const auto t = it->last_write_time(ec);
    if (best.empty() || t > when) best = it->path(), when = t;
  }
  return best;
}

// ---------------------------------------------------------------- the report

const char* const kKinds[] = {"The Mod Maker crashed (closed by itself)", "The Mod Maker froze or hung",
                              "A converter or a port failed", "A mod I made doesn't work in the game",
                              "Something looks or sounds wrong", "Installing or testing in the game",
                              "Something else"};
const char* const kAgain[] = {"Every time", "Sometimes", "It happened once"};

std::function<std::string()> g_state;
bool g_open = false;      // the window
int g_kind = 0, g_again = 0;
char g_what[4096] = "", g_before[2048] = "";
std::wstring g_made;      // the last report's zip ("" = none yet)
std::string g_description;
std::string g_test;       // --test-report
bool g_test_pending = false;
int g_test_frames = 0;
// Send report (launcher/report_send.h: the online server's /api/reports): the contact, the pictures, the answer.
char g_contact[64] = "";
std::vector<std::wstring> g_pics;   // up to REPORT_MAX_IMAGES
std::string g_pic_why;              // why the last picture wasn't added
std::atomic<bool> g_sending{false};
bool g_sent_done = false;           // a send finished (g_sent: its answer)
ReportResult g_sent{};
std::string g_send_file;            // --test-report-send <answers file>
bool g_send_test = false;

// What the report needs from the UI thread, taken before the worker starts.
struct Snapshot {
  std::string description, system, project, tools;
  std::wstring game, project_file;
  bool test = false;
};

std::string ModDoctor(const std::wstring& game) {
  if (game.empty()) return "(no game folder)\n";
  std::string text;
  auto put = [&](const std::string& who, const std::vector<Problem>& p) {
    if (p.empty()) text += who + " | ok\n";
    for (const auto& x : p) text += who + (x.error ? " | MUST FIX | " : " | check | ") + x.text + (x.fix.empty() ? "" : " | " + x.fix) + "\n";
  };
  put("(set)", CheckInstalledSet());
  std::error_code ec;
  for (const char* kind : {"Superstars", "Attires", "Arenas", "Moves", "Backstage", "Signs", "Media", "Backgrounds", "MatchTypes"})
    for (const auto& e : fs::directory_iterator(fs::path(game) / L"Mods" / kind, ec))
      if (e.is_directory(ec) && e.path().filename().wstring()[0] != L'.')
        put(std::string(kind) + "/" + Utf8(e.path().filename().wstring()), CheckModFolder(kind, e.path()));
  return text;
}

// The game's side of a problem (a mod that crashes or plays wrong in the game - players aren't asked for files): its
// newest logs (logs\svr2011_*.log) and crash notes (UserData\crashes\crash_*.txt), every installed mod's small text files
// (manifest.txt, its moves list, moves\pack.txt, .applied), the Music folder's file names and the mod theme owners
// (Music\.mod_themes).
void GameFiles(const fs::path& game, const std::function<void(std::string, const std::string&)>& add) {
  std::error_code ec;
  auto newest = [&](const fs::path& dir, const std::wstring& prefix, const std::wstring& ext, size_t n) {
    std::vector<std::pair<fs::file_time_type, fs::path>> v;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::wstring f = e.path().filename().wstring();
      if (e.is_regular_file(ec) && f.rfind(prefix, 0) == 0 && Lower(e.path().extension().string()) == Utf8(ext))
        v.push_back({e.last_write_time(ec), e.path()});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (v.size() > n) v.resize(n);
    return v;
  };
  for (const auto& [t, p] : newest(game / L"logs", L"svr2011", L".log", 3)) add("game/logs/" + PathStr(p.filename()), ReadTail(p, kTextCap));
  for (const auto& [t, p] : newest(game / L"logs", L"matchbox_", L".txt", 3))  // (the last matches' black boxes)
    add("game/logs/" + PathStr(p.filename()), ReadTail(p, kSmallCap));
  for (const auto& [t, p] : newest(game / L"UserData" / L"crashes", L"crash_", L".txt", 3))
    add("game/crashes/" + PathStr(p.filename()), ReadTail(p, kSmallCap));
  for (const char* kind : {"Superstars", "Attires", "Arenas", "Moves", "Backstage", "Signs", "Media", "Backgrounds", "MatchTypes"})
    for (const auto& e : fs::directory_iterator(game / L"Mods" / kind, ec)) {
      if (!e.is_directory(ec) || e.path().filename().wstring()[0] == L'.') continue;
      const std::string at = std::string("game/mods/") + kind + "/" + Utf8(e.path().filename().wstring()) + "/";
      Bytes man;
      std::string moves = "moves.txt";
      if (svrfmt::ReadFile(PathStr(e.path() / L"manifest.txt"), man)) {
        const std::string t(man.begin(), man.end());
        add(at + "manifest.txt", t);
        if (const std::string m = Value(t, "moves"); !m.empty()) moves = m;
      }
      for (const fs::path f : {fs::u8path(moves), fs::path(L"moves") / L"pack.txt", fs::path(L".applied")})
        if (fs::is_regular_file(e.path() / f, ec) && fs::file_size(e.path() / f, ec) <= kSmallCap)
          add(at + PathStr(f.generic_wstring()), ReadTail(e.path() / f, kSmallCap));
    }
  std::string music;
  for (const auto& e : fs::recursive_directory_iterator(game / L"Music", ec))
    if (e.is_regular_file(ec)) music += PathStr(fs::relative(e.path(), game / L"Music", ec)) + "  " + std::to_string(e.file_size(ec)) + "\n";
  add("game/music.txt", music.empty() ? std::string("(no Music folder, or empty)\n") : music);
  if (fs::is_regular_file(game / L"Music" / L".mod_themes", ec)) add("game/music_mod_themes.txt", ReadTail(game / L"Music" / L".mod_themes", kSmallCap));
}

// Makes the zip (a worker thread); its path, or "" (logged).
std::wstring BuildReport(const Snapshot& s) {
  std::vector<ZipEntry> z;
  auto add = [&](std::string name, const std::string& text) {
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::string r = Redact(text);
    z.push_back({name, Bytes(r.begin(), r.end())});
  };
  add("report.txt", s.description + "\n\nSvR 2011 Mod Maker problem report, made " + Stamp("%Y-%m-%d %H:%M:%S") + "\n" +
                        s.system +
                        "contents: the newest Mod Maker session logs (logs/), the open project's text files "
                        "(project/), the converters' newest report and log (convert/), the tools check (tools.txt), "
                        "the mod doctor on the installed mods (mod_doctor.txt), the game's newest logs (game/logs/) and "
                        "crash notes (game/crashes/), every installed mod's manifest, moves list and pack list "
                        "(game/mods/), the Music folder's file names (game/music.txt); internet addresses, the Windows user "
                        "folder's name and values given to passwords, tokens and keys blanked\n");
  // the session logs: this one and the ones before
  {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fflush(g_file);
  }
  const fs::path log_dir = s.game.empty() ? SettingsDir() / L"logs" : fs::path(s.game) / L"logs";
  std::vector<fs::path> logs = Logs(log_dir);
  if (log_dir != SettingsDir() / L"logs")  // (a session before the game folder was set)
    for (const auto& p : Logs(SettingsDir() / L"logs")) logs.push_back(p);
  for (size_t i = 0; i < logs.size() && i < kReportLogs; ++i)
    add("logs/" + PathStr(logs[i].filename()), ReadTail(logs[i], kTextCap));
  {  // the newest two crash files (InstallCrashHandler)
    std::vector<fs::path> crashes;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(log_dir, ec))
      if (e.path().filename().wstring().rfind(L"modmaker_crash_", 0) == 0) crashes.push_back(e.path());
    std::sort(crashes.begin(), crashes.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    for (size_t i = 0; i < crashes.size() && i < 2; ++i) add("logs/" + PathStr(crashes[i].filename()), ReadTail(crashes[i], kSmallCap));
  }
  // the open project: its state, and the text files in its .svrproj / mod folder
  add("project/state.txt", s.project);
  if (!s.project_file.empty()) {
    const fs::path pf = s.project_file;
    std::error_code ec;
    if (fs::is_directory(pf, ec)) {
      for (const auto& e : fs::recursive_directory_iterator(pf, ec))
        if (e.is_regular_file(ec) && IsText(e.path()) && e.file_size(ec) <= kSmallCap)
          add("project/" + PathStr(fs::relative(e.path(), pf, ec)), ReadTail(e.path(), kSmallCap));
    } else {
      Bytes d;
      std::vector<ZipEntry> inside;
      if (fs::file_size(pf, ec) <= (512u << 20) && svrfmt::ReadFile(PathStr(pf), d) && svrfmt::ZipRead(d, inside))
        for (const auto& e : inside)
          if (IsText(fs::path(e.name)) && e.data.size() <= kSmallCap) add("project/" + e.name, std::string(e.data.begin(), e.data.end()));
    }
  }
  // the converters' work folder: the newest report.json and log
  if (!s.game.empty()) {
    const fs::path work = fs::path(s.game) / L"Mods" / L".convert";
    const fs::path rep = Newest(work, [](const fs::path& p) { return Lower(PathStr(p.filename())) == "report.json"; });
    const fs::path log = Newest(work, [](const fs::path& p) { return Lower(p.extension().string()) == ".log"; });
    std::error_code ec;
    if (!rep.empty()) add("convert/" + PathStr(fs::relative(rep, work, ec)), ReadTail(rep, kSmallCap));
    if (!log.empty()) add("convert/" + PathStr(fs::relative(log, work, ec)), ReadTail(log, kSmallCap));
    if (rep.empty() && log.empty()) add("convert/none.txt", "No report.json or log in Mods\\.convert.\n");
  }
  add("tools.txt", s.tools);
  add("mod_doctor.txt", ModDoctor(s.game));
  if (!s.game.empty()) GameFiles(fs::path(s.game), add);
  // the zip, where the launcher puts its reports
  const fs::path dir = (s.game.empty() ? SettingsDir() : fs::path(s.game)) / L"Reports";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path zip = dir / Wide("SvR2011-ModMaker-report-" + Stamp("%Y%m%d-%H%M%S") + ".zip");
  if (!svrfmt::WriteFile(PathStr(zip), ZipWrite(z))) {
    Status("The report could not be written to " + PathStr(dir) + ".");
    return L"";
  }
  Line("report: " + PathStr(zip) + " (" + std::to_string(z.size()) + " files)");
  return zip.wstring();
}

std::atomic<bool> g_gathering{false};  // a report being made (its worker runs)

// The UI thread's part, then the worker (the mod doctor reads every installed mod: a big Mods folder takes a while).
void SendReport(const std::wstring& zip, bool test);
void StartReport(const std::string& description, bool test, bool send = false) {
  if (g_gathering) return;
  Snapshot s;
  s.description = description;
  s.system = SystemText();
  s.game = g_game;
  s.project_file = g_project_file;
  s.project = "page " + std::string(PageName(g_project_page)) + "\nfile " +
              (g_project_file.empty() ? std::string("(unsaved)") : Utf8(g_project_file)) + "\n\n" + (g_state ? g_state() : "");
  s.tools = ToolsReport();
  s.test = test;
  // (the game data the mod doctor shares with the pages, loaded here first: the pages read it while the worker runs)
  if (!g_game.empty()) MoveInfos(), EntrancesReady();
  Line("report: making a report");
  g_gathering = true;
  std::thread([s, send] {
    const std::wstring zip = BuildReport(s);
    OnUiThread([zip, test = s.test, send] {
      g_gathering = false;
      g_made = zip;
      if (send && !zip.empty()) {
        SendReport(zip, test);
        return;
      }
      if (test) {
        Log("report test: " + Utf8(zip));
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      }
    });
  }).detach();
}

std::string Describe() {
  return std::string("SvR 2011 Mod Maker ") + PORT_VERSION + "\nProblem: " + kKinds[g_kind] + "\nHappens: " + kAgain[g_again] +
         "\n\nWhat happened:\n" + g_what + "\n\nJust before:\n" + (g_before[0] ? g_before : "(not said)") + "\n";
}

// A server address on this PC (127.0.0.1 / localhost / [::1], any port): the only kind a test may send to.
bool LocalServer(const char* url) {
  if (!url) return false;
  std::string s = url;
  if (const size_t k = s.find("://"); k != std::string::npos) s = s.substr(k + 3);
  for (const char* h : {"127.0.0.1", "localhost", "[::1]"})
    if (!s.compare(0, std::strlen(h), h) && (s.size() == std::strlen(h) || s[std::strlen(h)] == ':' || s[std::strlen(h)] == '/'))
      return true;
  return false;
}

// The online server the game uses (<game>\svr2011.toml online_server), else the default one.
std::wstring ReportServer() {
  if (const char* e = std::getenv("SVR2011_REPORT_SERVER")) return Wide(e);  // (tests: a private server)
  std::wstring server = L"https://sho-ti.me/svr";
  if (FILE* f = g_game.empty() ? nullptr : _wfsopen((fs::path(g_game) / L"svr2011.toml").c_str(), L"rb", _SH_DENYNO)) {
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
      const char* p = line;
      while (*p == ' ') ++p;
      if (std::strncmp(p, "online_server", 13)) continue;
      const char* q = std::strchr(p, '"');
      const char* e = q ? std::strchr(q + 1, '"') : nullptr;
      if (q && e && e > q + 1) server = Wide(std::string(q + 1, e));
    }
    std::fclose(f);
  }
  return server;
}

// Sends the made zip with the answers and pictures (a worker); the window shows the answer (g_sent).
void SendReport(const std::wstring& zip, bool test) {
  g_sending = true;
  g_sent_done = false;
  const std::wstring server = ReportServer();
  const std::string problem = kKinds[g_kind], happens = kAgain[g_again], what = g_what, before = g_before,
                    contact = g_contact;
  const std::vector<std::wstring> pics = g_pics;
  Line("report: sending to " + Utf8(server) + " (" + std::to_string(pics.size()) + " picture(s))");
  std::thread([=] {
    // (the hint for the server's duplicate check: the newest session log's last error line)
    std::string sig;
    for (const auto& p : Logs(fs::path(g_game.empty() ? SettingsDir() : fs::path(g_game)) / L"logs")) {
      const std::string text = ReadTail(p, 1u << 20);
      size_t at = text.rfind("error");
      if (at == std::string::npos) at = text.rfind("failed");
      if (at != std::string::npos) {
        size_t b = text.rfind('\n', at), e = text.find('\n', at);
        sig = text.substr(b == std::string::npos ? 0 : b + 1, (e == std::string::npos ? text.size() : e) - (b == std::string::npos ? 0 : b + 1));
      }
      break;
    }
    char* json = report_json("modmaker", PORT_VERSION, problem.c_str(), happens.c_str(), what.c_str(), before.c_str(),
                             contact.c_str(), sig.c_str());
    ReportUpload u{};
    u.json = json;
    u.zip = zip.c_str();
    for (size_t i = 0; i < pics.size() && i < REPORT_MAX_IMAGES; ++i) u.images[i] = pics[i].c_str();
    u.n_images = int(std::min<size_t>(pics.size(), REPORT_MAX_IMAGES));
    ReportResult r{};
    report_send(server.c_str(), &u, &r);
    std::free(json);
    OnUiThread([r, test] {
      g_sent = r;
      g_sent_done = true;
      g_sending = false;
      if (r.id[0]) Line(std::string("report: sent, id ") + r.id + ", seen before " + std::to_string(r.seen_before));
      else Line("report: not sent - " + Utf8(std::wstring(r.why)) + " (HTTP " + std::to_string(r.status) + ")");
      if (test) {
        Log(r.id[0] ? std::string("report test: sent ") + r.id + " seen_before " + std::to_string(r.seen_before)
                    : "report test: not sent - " + Utf8(std::wstring(r.why)));
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      }
    });
  }).detach();
}

void AddPicture(const std::wstring& path) {
  if (g_pics.size() >= REPORT_MAX_IMAGES) return;
  for (const auto& p : g_pics)
    if (!_wcsicmp(p.c_str(), path.c_str())) return;
  WCHAR why[300];
  if (!report_image_ok(path.c_str(), why, 300)) {
    g_pic_why = Utf8(why);
    return;
  }
  g_pic_why.clear();
  g_pics.push_back(path);
}

void PickPictures() {
  static WCHAR files[8192];
  OPENFILENAMEW of{};
  files[0] = 0;
  of.lStructSize = sizeof of;
  of.hwndOwner = g_wnd;
  of.lpstrFilter = L"Pictures (PNG, JPEG, WebP)\0*.png;*.jpg;*.jpeg;*.webp\0";
  of.lpstrFile = files;
  of.nMaxFile = 8192;
  of.lpstrTitle = L"Pictures for the report";
  of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
  if (!GetOpenFileNameW(&of)) return;
  if (files[wcslen(files) + 1] == 0) {
    AddPicture(files);
  } else {
    for (const WCHAR* p = files + wcslen(files) + 1; *p; p += wcslen(p) + 1) AddPicture(std::wstring(files) + L"\\" + p);
  }
}

// "Paste picture": the clipboard's bitmap saved as a PNG under <game>\Reports (WIC), then added.
void PastePicture() {
  using Microsoft::WRL::ComPtr;
  if (!OpenClipboard(g_wnd)) return;
  HBITMAP bmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
  if (!bmp) {
    CloseClipboard();
    g_pic_why = "The clipboard has no picture (copy a screenshot first: Print Screen, or Win+Shift+S).";
    return;
  }
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> src;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &src));
  CloseClipboard();
  const fs::path dir = (g_game.empty() ? SettingsDir() : fs::path(g_game)) / L"Reports";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path out = dir / Wide("pasted-" + Stamp("%Y%m%d-%H%M%S") + "-" + std::to_string(g_pics.size()) + ".png");
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  ok = ok && SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) &&
       SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
       SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
       SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->WriteSource(src.Get(), nullptr)) &&
       SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
  if (!ok) {
    g_pic_why = "The picture couldn't be saved.";
    return;
  }
  stream.Reset();
  AddPicture(out.wstring());
}

void Window() {
  if (g_open) ImGui::OpenPopup("Report a problem"), g_open = false;
  ImGui::SetNextWindowSize(ImVec2(620 * g_scale, 0), ImGuiCond_Always);  // (the height fits each page)
  if (!ImGui::BeginPopupModal("Report a problem", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
  if (g_sending) {
    const char spin[] = "|/-\\";
    ImGui::Text("%c  Sending the report ...", spin[int(ImGui::GetTime() * 8) & 3]);
  } else if (g_gathering) {
    const char spin[] = "|/-\\";
    ImGui::Text("%c  Gathering the report ...", spin[int(ImGui::GetTime() * 8) & 3]);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("The logs, your project, the converters' last report, the tools and the mod doctor on every "
                        "installed mod - a big Mods folder takes a little while.");
    ImGui::PopTextWrapPos();
  } else if (g_made.empty()) {
    ImGui::TextWrapped("Tell us what went wrong. The report also has the Mod Maker's own record of what it did "
                       "(pages, projects, converters and their output) - no passwords, no internet addresses, no "
                       "disc images or game files.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##kind", &g_kind, kKinds, IM_ARRAYSIZE(kKinds));
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##again", &g_again, kAgain, IM_ARRAYSIZE(kAgain));
    ImGui::TextUnformatted("What happened?");
    ImGui::InputTextMultiline("##what", g_what, sizeof g_what, ImVec2(-1, 110 * g_scale));
    ImGui::TextUnformatted("What were you doing just before? (optional)");
    ImGui::InputTextMultiline("##before", g_before, sizeof g_before, ImVec2(-1, 70 * g_scale));
    ImGui::TextUnformatted("Your Discord name (optional - so we can ask you):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##contact", g_contact, sizeof g_contact);
    ImGui::TextUnformatted("Pictures (up to 5): screenshots of the problem help a lot.");
    for (size_t i = 0; i < g_pics.size(); ++i) {
      ImGui::PushID(int(i));
      if (ImGui::SmallButton("Remove")) {
        g_pics.erase(g_pics.begin() + long(i));
        ImGui::PopID();
        break;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(Utf8(fs::path(g_pics[i]).filename().wstring()).c_str());
      ImGui::PopID();
    }
    ImGui::BeginDisabled(g_pics.size() >= REPORT_MAX_IMAGES);
    if (ImGui::Button("Add pictures...")) PickPictures();
    ImGui::SameLine();
    if (ImGui::Button("Paste picture")) PastePicture();
    ImGui::EndDisabled();
    if (!g_pic_why.empty()) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "%s", g_pic_why.c_str());
    {
      ReportUpload u{};
      u.zip = L"report.zip";
      u.n_images = int(g_pics.size());
      WCHAR line[300];
      report_send_summary(&u, line, 300);
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", Utf8(line).c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    const bool said = g_what[0] != 0;
    ImGui::BeginDisabled(!said);
    if (ImGui::Button("Send report")) {
      g_description = Describe();
      g_sent_done = false;
      StartReport(g_description, false, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save zip")) {
      g_description = Describe();
      g_sent_done = false;
      StartReport(g_description, false);
    }
    ImGui::EndDisabled();
    if (!said && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Say what happened first.");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  } else if (g_sent_done && g_sent.id[0]) {
    ImGui::TextWrapped("Your report reached us. Report id:  %s", g_sent.id);
    if (g_sent.seen_before > 0)
      ImGui::TextWrapped("This problem was already reported %d time%s - yours adds to it, thank you.", g_sent.seen_before,
                         g_sent.seen_before == 1 ? "" : "s");
    ImGui::TextWrapped("If you post about it on our Discord (bug-svr11 forum), give this id. A copy is kept here:");
    ImGui::TextWrapped("%s", Utf8(g_made).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Copy the id")) ImGui::SetClipboardText(g_sent.id);
    ImGui::SameLine();
    if (ImGui::Button("Open the folder")) ShellOpen(fs::path(g_made).parent_path().wstring());
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      g_made.clear(), g_what[0] = g_before[0] = 0, g_pics.clear(), g_sent_done = false;
      ImGui::CloseCurrentPopup();
    }
  } else {
    if (g_sent_done)
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "Not sent: %s", Utf8(std::wstring(g_sent.why)).c_str());
    ImGui::TextWrapped("The report is ready:");
    ImGui::TextWrapped("%s", Utf8(g_made).c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Post it in a new thread in the Discord's bug-svr11 forum: attach the zip and paste the "
                       "description (Copy the description).");
    ImGui::Spacing();
    if (ImGui::Button("Open the folder")) ShellOpen(fs::path(g_made).parent_path().wstring());
    ImGui::SameLine();
    if (ImGui::Button("Copy the description")) ImGui::SetClipboardText(g_description.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open Discord")) ShellOpen(L"https://discord.com/invite/QhGSuDKHwk");
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      g_made.clear(), g_what[0] = g_before[0] = 0, g_pics.clear(), g_sent_done = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

}  // namespace

void Line(const std::string& s) {
  std::lock_guard lock(g_mutex);
  WriteLine(TimePrefix() + s);
}

void Raw(const std::string& label, const std::string& text) {
  std::lock_guard lock(g_mutex);
  WriteLine(TimePrefix() + label);
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    WriteLine("    " + text.substr(i, e - i));
    i = e + 1;
  }
}

void Tick() {
  // (the file opens on the first frame: the game folder is settled by then; a game folder chosen later keeps it)
  if (!g_file && !g_tried) Open();
  if (static int frames = 0; ++frames == 30) {  // test aid SVR2011_MM_TEST_CRASH=av|throw: a crash, for the crash file
    const char* t = std::getenv("SVR2011_MM_TEST_CRASH");
    if (t && !std::strcmp(t, "av")) Line("test crash: av"), *static_cast<volatile int*>(nullptr) = 1;
    if (t && !std::strcmp(t, "throw")) Line("test crash: throw"), std::thread([] { throw std::runtime_error("test throw"); }).join();
  }
  if (g_page != g_last_page) {
    g_last_page = g_page;
    Line(std::string("page: ") + PageName(g_page));
  }
  if (g_test_pending && ++g_test_frames > 30) {
    g_test_pending = false;
    if (g_send_test && !std::getenv("SVR2011_REPORT_SERVER")) {
      // (a test never goes to the live server by default: SVR2011_REPORT_SERVER names the test server - 26 Oct a
      // launcher test report reached the live one)
      Log("report test: not sent - SVR2011_REPORT_SERVER isn't set (give the test server)");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    } else if (g_send_test && !LocalServer(std::getenv("SVR2011_REPORT_SERVER")) &&
               !(std::getenv("SVR2011_ALLOW_LIVE") && !std::strcmp(std::getenv("SVR2011_ALLOW_LIVE"), "1"))) {
      // (and only a server on this PC: a test never writes to the live one - SVR2011_ALLOW_LIVE=1 lets it through)
      Log(std::string("report test: not sent - SVR2011_REPORT_SERVER ") + std::getenv("SVR2011_REPORT_SERVER") +
          " is not a server on this PC (SVR2011_ALLOW_LIVE=1 lets it through)");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    } else if (g_send_test) {  // (--test-report-send: the answers file - what= before= contact= problem=<n> image=<path>)
      if (FILE* f = _wfsopen(Wide(g_send_file).c_str(), L"rb", _SH_DENYNO)) {
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
          std::string l = line;
          if (l.rfind("\xEF\xBB\xBF", 0) == 0) l.erase(0, 3);  // (a BOM)
          while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
          const size_t eq = l.find('=');
          if (eq == std::string::npos) continue;
          const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
          if (k == "what") std::strncpy(g_what, v.c_str(), sizeof g_what - 1);
          else if (k == "before") std::strncpy(g_before, v.c_str(), sizeof g_before - 1);
          else if (k == "contact") std::strncpy(g_contact, v.c_str(), sizeof g_contact - 1);
          else if (k == "problem") g_kind = std::clamp(std::atoi(v.c_str()), 0, int(IM_ARRAYSIZE(kKinds)) - 1);
          else if (k == "image") AddPicture(Wide(v));
        }
        std::fclose(f);
      }
      if (!g_pic_why.empty()) Log("report test: picture refused - " + g_pic_why);
      StartReport(Describe(), true, true);  // (sends, logs the answer and closes)
    } else {
      std::strncpy(g_what, g_test.c_str(), sizeof g_what - 1);
      StartReport(Describe(), true);  // (logs the zip and closes the Mod Maker when it's done)
    }
  }
  Window();
}

void OpenReport(std::function<std::string()> project_state) {
  g_state = std::move(project_state);
  g_made.clear();
  g_open = true;
  Line("report: the report window opened");
}

void TestReport(const std::string& what) {
  g_test = what;
  g_test_pending = true;
}

void TestReportSend(const std::string& answers_file) {
  g_send_file = answers_file;
  g_send_test = true;
  g_test_pending = true;
}

}  // namespace mm::session
