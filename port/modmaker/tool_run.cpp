// Python converters run from the Mod Maker (tool_run.h).
#include "tool_run.h"

#include "app.h"
#include "disc_list.h"
#include "session_log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

extern "C" {
#include "../launcher/setup_tab.h"
}

namespace mm {

namespace {

std::wstring g_tools, g_python, g_ffmpeg, g_svrmod, g_vgm;
bool g_checked = false;
std::string g_missing;
bool g_missing_checked = false;
std::mutex g_hint_mutex;
std::string g_hint;  // what the last converter's output said was missing ("numpy", "svrmod.exe" ...)

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(buf).parent_path().wstring();
}

// A Microsoft Store app alias (the 0-byte python.exe that opens the Store) - a real Store Python isn't one.
bool IsAlias(const std::wstring& p) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  return !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.nFileSizeHigh == 0 && a.nFileSizeLow == 0);
}

// The launcher's First time setup puts Python (numpy, Pillow) and ffmpeg in <game>\Tools.
std::vector<fs::path> ToolsHomes() {
  std::vector<fs::path> out = {fs::path(ExeDir()) / L"Tools"};
  if (!g_game.empty()) out.push_back(fs::path(g_game) / L"Tools");
  return out;
}

// The first program on PATH with that name ("" = none).
std::wstring OnPath(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  if (SearchPathW(nullptr, name, L".exe", DWORD(std::size(buf)), buf, nullptr)) return buf;
  return L"";
}

// "" when `python` imports numpy and PIL, else the missing ones ("numpy, Pillow"); "?" when it didn't run.
std::string ProbeModules(const std::wstring& python) {
  const std::wstring probe = Quote(python) + L" -c \"import importlib.util as u; "
                             L"print('missing:' + ','.join(n for m, n in (('numpy', 'numpy'), ('PIL', 'Pillow')) if u.find_spec(m) is None))\"";
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return "?";
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr, si.hStdError = wr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = probe;
  std::string out;
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(wr);
    char buf[512];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
  } else {
    CloseHandle(wr);
  }
  CloseHandle(rd);
  const size_t at = out.find("missing:");
  if (at == std::string::npos) return "?";
  std::string m = out.substr(at + 8);
  while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ')) m.pop_back();
  for (auto& c : m)
    if (c == ',') c = '/';
  std::string pretty;
  for (size_t i = 0; i < m.size(); ++i) pretty += m[i] == '/' ? std::string(", ") : std::string(1, m[i]);
  return pretty;
}

// What a converter's output line says is missing ("" = nothing): Python's ModuleNotFoundError, the tools' ToolMissing.
std::string HintOf(const std::string& line) {
  if (const size_t at = line.find("No module named '"); at != std::string::npos) {
    const size_t e = line.find('\'', at + 17);
    std::string mod = line.substr(at + 17, e == std::string::npos ? std::string::npos : e - at - 17);
    if (const size_t dot = mod.find('.'); dot != std::string::npos) mod.resize(dot);
    // (one of the Mod Maker Tools' own modules - hctp2jboy, hmot... - is no package to install: the converter's own
    // error says what's wrong; a player was asked "Missing: hctp2jboy - install now?" again and again)
    if (!g_tools.empty() && !mod.empty()) {
      std::error_code ec;
      const fs::path tools(g_tools), file = Wide(mod + ".py");
      if (fs::exists(tools / file, ec)) return "";
      for (fs::recursive_directory_iterator it(tools, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() == file) return "";
    }
    return mod == "PIL" ? "Pillow" : mod;
  }
  if (line.find("ToolMissing") != std::string::npos || (line.find("svrmod.exe") != std::string::npos && line.find("not found") != std::string::npos))
    return line.find("vgmstream") != std::string::npos ? "vgmstream" : "svrmod.exe";
  return "";
}
void NoteHint(const std::string& line) {
  const std::string h = HintOf(line);
  if (h.empty()) return;
  std::lock_guard lock(g_hint_mutex);
  if (g_hint.find(h) == std::string::npos) g_hint += (g_hint.empty() ? "" : ", ") + h;
}
std::string TakeHint() {
  std::lock_guard lock(g_hint_mutex);
  std::string h;
  std::swap(h, g_hint);
  return h;
}

void Check() {
  if (g_checked) return;
  g_checked = true;
  g_python.clear(), g_ffmpeg.clear(), g_svrmod.clear(), g_vgm.clear(), g_tools.clear();
  g_missing.clear(), g_missing_checked = false;
  const std::wstring exe = ExeDir();
  std::error_code ec;
  for (const fs::path cand : {fs::path(exe) / L"Mod Maker Tools", fs::path(exe) / L"tools",
                              fs::path(exe) / L".." / L".." / L".." / L"tools",  // (out/build/<preset> -> port/tools)
                              fs::path(exe) / L".." / L"tools"})
    if (fs::exists(cand / L"svr10_superstar.py", ec)) {
      g_tools = fs::weakly_canonical(cand, ec).wstring();
      break;
    }
  // Python: the launcher setup's (<game>\Tools\python), then python / python3 on PATH (not the Store's
  // 0-byte alias), then the py launcher
  for (const auto& home : ToolsHomes())
    if (g_python.empty() && fs::exists(home / L"python" / L"python.exe", ec)) g_python = (home / L"python" / L"python.exe").wstring();
  if (g_python.empty()) {  // (the player's own: only one that has numpy and Pillow - else the first, to say what's missing)
    std::vector<std::wstring> cands;
    for (const wchar_t* name : {L"python.exe", L"python3.exe"})
      if (const std::wstring p = OnPath(name); !p.empty() && !IsAlias(p)) cands.push_back(p);
    wchar_t no_py[8];  // (test: SVR2011_TEST_NO_PY=1 - as on a PC whose only Python is one without numpy)
    if (const std::wstring p = OnPath(L"py.exe"); !p.empty() && !GetEnvironmentVariableW(L"SVR2011_TEST_NO_PY", no_py, 8)) cands.push_back(p);
    for (const auto& c : cands) {
      const std::string m = ProbeModules(c);
      Log("tools: Python " + Utf8(c) + (m.empty() ? ": numpy and Pillow found" : m == "?" ? ": didn't run" : ": missing " + m));
      if (m.empty()) {
        g_python = c, g_missing_checked = true;
        break;
      }
      if (g_python.empty()) g_python = c, g_missing = m == "?" ? "numpy, Pillow" : m, g_missing_checked = true;
    }
  }
  // ffmpeg: the setup's, then PATH, then C:\ffmpeg\bin; the setup's goes first on the scripts' PATH too
  // (their shutil.which('ffmpeg') finds it: children inherit this process's environment)
  for (const auto& home : ToolsHomes())
    if (g_ffmpeg.empty() && fs::exists(home / L"ffmpeg" / L"ffmpeg.exe", ec)) {
      g_ffmpeg = (home / L"ffmpeg" / L"ffmpeg.exe").wstring();
      wchar_t path[32767];
      const DWORD n = GetEnvironmentVariableW(L"PATH", path, DWORD(std::size(path)));
      const std::wstring dir = (home / L"ffmpeg").wstring();
      if (n < std::size(path) && std::wstring(path, n).find(dir) == std::wstring::npos)
        SetEnvironmentVariableW(L"PATH", (dir + L";" + std::wstring(path, n)).c_str());
    }
  if (g_ffmpeg.empty()) g_ffmpeg = OnPath(L"ffmpeg.exe");
  if (g_ffmpeg.empty() && fs::exists(L"C:\\ffmpeg\\bin\\ffmpeg.exe", ec)) g_ffmpeg = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
  // svrmod.exe and vgmstream: the Mod Maker Tools folder's (a development build: out\modmaker, third_party)
  if (!g_tools.empty()) {
    const fs::path t = g_tools;
    for (const fs::path c : {t / L"svrmod.exe", t / L".." / L"out" / L"modmaker" / L"svrmod.exe"})
      if (g_svrmod.empty() && fs::exists(c, ec)) g_svrmod = fs::weakly_canonical(c, ec).wstring();
    for (const fs::path c : {t / L"vgmstream" / L"vgmstream-cli.exe", t / L".." / L"third_party" / L"vgmstream" / L"vgmstream-cli.exe"})
      if (g_vgm.empty() && fs::exists(c, ec)) g_vgm = fs::weakly_canonical(c, ec).wstring();
  }
  // (every converter inherits them: svrfmt.py / wwe13_entrance.find_svrmod / wwise.find_vgmstream read these first)
  SetEnvironmentVariableW(L"SVR2011_SVRMOD", g_svrmod.empty() ? nullptr : g_svrmod.c_str());
  SetEnvironmentVariableW(L"SVR2011_VGMSTREAM", g_vgm.empty() ? nullptr : g_vgm.c_str());
  Log("tools: scripts " + (g_tools.empty() ? std::string("not found") : Utf8(g_tools)) + ", Python " +
      (g_python.empty() ? std::string("not found") : Utf8(g_python)) + ", ffmpeg " +
      (g_ffmpeg.empty() ? std::string("not found") : Utf8(g_ffmpeg)) + ", svrmod " +
      (g_svrmod.empty() ? std::string("not found") : Utf8(g_svrmod)) + ", vgmstream " +
      (g_vgm.empty() ? std::string("not found") : Utf8(g_vgm)));
}

// Runs a command with no window, reading its output into the log; returns the exit code.
// The session log's line for a finished run: its exit code and time.
void Finished(int code, std::chrono::steady_clock::time_point start) {
  char b[96];
  std::snprintf(b, sizeof b, "  (exit code %d, %.1f s)", code,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  session::Line(b);
}

int RunCapture(const std::wstring& cmdline, const std::wstring& cwd) {
  const auto start = std::chrono::steady_clock::now();
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = cmdline;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    Log("Could not start: " + Utf8(cmdline));
    return -1;
  }
  std::string line;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
    for (DWORD i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) Log("  " + line), Progress(line.substr(0, 120)), NoteHint(line);
        line.clear();
      } else {
        line += buf[i];
      }
    }
  }
  if (!line.empty()) Log("  " + line);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  Finished(int(code), start);
  return int(code);
}

// The running converter's job object (it and every process it starts): CancelTool ends them all.
std::mutex g_job_mutex;
std::string g_notices;  // (g_job_mutex: the running tool's `notice <text>` lines)
HANDLE g_job = nullptr;
std::atomic<bool> g_cancelled{false};

// stdout collected, stderr logged on a thread of its own (progress lines to the bar).
int RunCaptureJson(const std::wstring& cmdline, const std::wstring& cwd, std::string& out) {
  const auto start = std::chrono::steady_clock::now();
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE ord = nullptr, owr = nullptr, erd = nullptr, ewr = nullptr;
  if (!CreatePipe(&ord, &owr, &sa, 0)) return -1;
  if (!CreatePipe(&erd, &ewr, &sa, 0)) {
    CloseHandle(ord), CloseHandle(owr);
    return -1;
  }
  SetHandleInformation(ord, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(erd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = owr;
  si.hStdError = ewr;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = cmdline;
  g_cancelled = false;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr,
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(owr);
  CloseHandle(ewr);
  if (!ok) {
    CloseHandle(ord), CloseHandle(erd);
    Log("Could not start: " + Utf8(cmdline));
    return -1;
  }
  {  // (in a job before it runs: what it starts is in it too)
    std::lock_guard lock(g_job_mutex);
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li), AssignProcessToJobObject(g_job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  std::thread err([erd] {
    std::string line;
    char buf[4096];
    DWORD n = 0;
    auto flush = [&] {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("progress ", 0) == 0) {
        const size_t sp = line.find(' ', 9);
        Progress(sp == std::string::npos ? line.substr(9) : line.substr(9, sp - 9) + "%  " + line.substr(sp + 1, 110));
      } else if (line.rfind("notice ", 0) == 0) {  // (for the player: on screen when the job ends, RunToolJson)
        Log("  " + line.substr(7));
        std::lock_guard lock(g_job_mutex);
        g_notices += (g_notices.empty() ? "" : "  ") + line.substr(7);
      } else if (!line.empty()) {
        Log("  " + line), NoteHint(line);
      }
      line.clear();
    };
    while (ReadFile(erd, buf, sizeof buf, &n, nullptr) && n)
      for (DWORD i = 0; i < n; ++i)
        if (buf[i] == '\n') flush();
        else line += buf[i];
    flush();
    CloseHandle(erd);
  });
  char buf[8192];
  DWORD n = 0;
  while (ReadFile(ord, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
  CloseHandle(ord);
  err.join();
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  {
    std::lock_guard lock(g_job_mutex);
    if (g_job) CloseHandle(g_job), g_job = nullptr;
  }
  if (g_cancelled) session::Line("  (cancelled)");
  session::Raw("  its answer (stdout):", out.size() > (4u << 20) ? out.substr(out.size() - (4u << 20)) : out);
  Finished(int(code), start);
  return int(code);
}

}  // namespace

int RunWait(const std::wstring& cmdline, std::string& out) {
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr, si.hStdError = wr, si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = cmdline;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                 nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    Log("Could not start: " + Utf8(cmdline));
    return -1;
  }
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
  return int(code);
}

std::string ShrinkMovie(const std::wstring& in, const std::wstring& out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::wstring ff = FfmpegExe(), part = out + L".part.mp4";
  fs::create_directories(fs::path(out).parent_path(), ec);
  fs::remove(part, ec);
  const fs::path script = ToolsDir().empty() ? fs::path() : fs::path(ToolsDir()) / L"movie_shrink.py";
  const bool use_script = !script.empty() && fs::exists(script, ec) && !PythonExe().empty();
  std::wstring cmd;
  if (use_script) {
    cmd = Quote(PythonExe()) + L" -X utf8 " + Quote(script.wstring()) + L" " + Quote(in) + L" " + Quote(part);
    if (!ff.empty()) cmd += L" --ffmpeg " + Quote(ff);
  } else {
    if (ff.empty()) return "ffmpeg is missing (Mod Maker tools: Install what's missing)";
    if (FILE* f = _wfopen(in.c_str(), L"rb")) {  // (Bink 2: the ffmpeg builds don't read it)
      char head[4] = {};
      const size_t got = std::fread(head, 1, 4, f);
      std::fclose(f);
      if (got == 4 && head[0] == 'K' && head[1] == 'B' && head[2] == '2') return "a Bink 2 movie, which ffmpeg can't read";
    }
    cmd = Quote(ff) + L" -hide_banner -loglevel error -nostdin -y -i " + Quote(in) +
          L" -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 20 -pix_fmt yuv420p -c:a aac -movflags +faststart " + Quote(part);
  }
  Log("movie: " + Utf8(fs::path(in).filename().wstring()) + " -> mp4 (" + (use_script ? "movie_shrink.py" : "ffmpeg") + ")");
  std::string text;
  const int code = RunWait(cmd, text);
  if (code == 0 && fs::exists(part, ec) && fs::file_size(part, ec) > 0) {
    fs::remove(out, ec);
    fs::rename(part, out, ec);
    if (!ec) return {};
  }
  fs::remove(part, ec);
  // why: the script's JSON "error", else the last line it printed
  std::string why;
  if (const size_t e = text.rfind("\"error\""); e != std::string::npos) {
    const size_t q1 = text.find('"', text.find(':', e) + 1), q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
    if (q2 != std::string::npos) why = text.substr(q1 + 1, q2 - q1 - 1);
  }
  if (why.empty()) {
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    why = t.substr(t.find_last_of('\n') == std::string::npos ? 0 : t.find_last_of('\n') + 1);
  }
  Log("movie: the mp4 wasn't made (exit " + std::to_string(code) + "): " + text.substr(text.size() > 2000 ? text.size() - 2000 : 0));
  return why.empty() ? "the conversion stopped (exit " + std::to_string(code) + ")" : why;
}

void CancelTool() {
  std::lock_guard lock(g_job_mutex);
  if (!g_job) return;
  g_cancelled = true;
  TerminateJobObject(g_job, 1);
  Log("The running converter was cancelled.");
}
bool ToolCancelled() { return g_cancelled; }

std::wstring ToolsDir() { Check(); return g_tools; }
std::wstring PythonExe() { Check(); return g_python; }
std::wstring FfmpegExe() { Check(); return g_ffmpeg; }
std::wstring SvrmodExe() { Check(); return g_svrmod; }
std::wstring VgmstreamExe() { Check(); return g_vgm; }
void RecheckTools() { g_checked = false, g_missing_checked = false; }

std::string PythonMissing() {
  Check();
  if (g_missing_checked) return g_missing.empty() ? "" : g_missing + " (Mod Maker tools: Install what's missing)";
  g_missing_checked = true;
  if (g_python.empty()) {
    g_missing = "Python 3 with numpy and Pillow";
    return g_missing + " (Mod Maker tools: Install what's missing)";
  }
  const std::string m = ProbeModules(g_python);
  g_missing = m == "?" ? "numpy, Pillow" : m;
  return g_missing.empty() ? "" : g_missing + " (Mod Maker tools: Install what's missing)";
}

std::wstring Quote(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring q = L"\"";
  for (wchar_t c : arg) q += c == L'"' ? L"\\\"" : std::wstring(1, c);
  return q + L"\"";
}

bool RunCommand(const std::wstring& cmdline, std::function<void(int)> done, const std::wstring& cwd) {
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return false;
  }
  Log("> " + Utf8(cmdline));
  RunInBackground([cmdline, cwd, done] {
    Progress("Running ...");
    const int code = RunCapture(cmdline, cwd);
    OnUiThread([done, code] { if (done) done(code); });
  });
  return true;
}

namespace {
struct MovesAsk {
  std::function<void()> go, cancel;
  int frames = 0;
};
std::unique_ptr<MovesAsk> g_moves_ask;  // the conversion waiting for the warning's answer
bool g_moves_ok = false;                // accepted this session
bool g_moves_dont_show = false;         // the dialog's tick
std::string MovesTestAnswer() {
  const char* v = std::getenv("SVR2011_TEST_MOVES_WARNING");
  return v ? v : "";
}
}  // namespace

bool ConvertsMoves(const std::wstring& script, const std::vector<std::wstring>& args) {
  if (args.empty() || args[0] != L"convert") return false;
  if (script == L"moves_port.py") return true;
  if (script == L"superstar_port.py") return std::find(args.begin(), args.end(), L"--no-moves") == args.end();
  return false;
}

void MovesWarning() {
  if (!g_moves_ask) return;
  const char* kTitle = "Move converting is experimental###moveswarning";
  if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560 * g_scale, 0), ImGuiCond_Always);
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1), "Move converting is HIGHLY EXPERIMENTAL.");
  ImGui::Spacing();
  ImGui::Bullet();
  ImGui::TextWrapped("There WILL be bugs: moves may freeze, look wrong or not work at all.");
  ImGui::Bullet();
  ImGui::TextWrapped("By continuing you agree that your mod may break in a future update, and that you may have to "
                     "remake it from scratch.");
  ImGui::Spacing();
  ImGui::TextDisabled("Cancel and nothing is converted.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::Checkbox("Don't show this again", &g_moves_dont_show);
  ImGui::Spacing();
  const std::string test = g_behind ? MovesTestAnswer() : "";
  ++g_moves_ask->frames;
  PushAccent();
  if (test == "accept_dont_show" && g_moves_ask->frames == 20) g_moves_dont_show = true;  // (test: the tick, then accept)
  const bool go = ImGui::Button("I understand - convert", ImVec2(220 * g_scale, 0)) ||
                  ((test == "accept" || test == "accept_dont_show") && g_moves_ask->frames > 30);
  PopAccent();
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel", ImVec2(120 * g_scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                      (test == "cancel" && g_moves_ask->frames > 30);
  if (go || cancel) {
    auto ask = std::move(g_moves_ask);
    ImGui::CloseCurrentPopup();
    if (go) {
      g_moves_ok = true;
      if (g_moves_dont_show) g_settings.moves_warning_off = true, SaveSettings();
      Log("moves warning: accepted" + std::string(g_moves_dont_show ? " (don't show again)" : ""));
      WhenIdle(ask->go);
    } else {
      Log("moves warning: cancelled - nothing converted");
      Status("Not converted: move converting cancelled.");
      ask->cancel();
    }
  }
  ImGui::EndPopup();
}

bool RunToolJson(const std::wstring& script, const std::vector<std::wstring>& args_in,
                 std::function<void(int, const std::string&)> done, const std::wstring& cwd) {
  if (!ConvertersReady()) return false;
  // (the source game's DLC folder, My game discs: the porters read its files beside the disc's)
  std::vector<std::wstring> args = args_in;
  // (not twice: an accepted move warning comes back here with these args)
  if ((script == L"superstar_port.py" || script == L"moves_port.py" || script == L"entrance_port.py" || script == L"arena_port.py") &&
      std::find(args_in.begin(), args_in.end(), L"--dlc") == args_in.end())
    for (size_t i = 0; i + 1 < args_in.size(); ++i)
      if (args_in[i] == L"--game") {
        if (const std::wstring dlc = disc_list::DlcFor(Utf8(args_in[i + 1])); !dlc.empty())
          args.push_back(L"--dlc"), args.push_back(dlc);
        break;
      }
  // moves: the experimental warning first (once a session; a hidden test only when it asks for it)
  if (ConvertsMoves(script, args) && !g_moves_ok && !g_settings.moves_warning_off && (!g_behind || !MovesTestAnswer().empty())) {
    if (g_moves_ask) {
      Status("Answer the move warning first.");
      return false;
    }
    g_moves_ask = std::make_unique<MovesAsk>();
    g_moves_ask->go = [script, args, done, cwd] { RunToolJson(script, args, done, cwd); };
    g_moves_ask->cancel = [done] {
      if (done) done(2, "{\"ok\": false, \"error\": \"Not converted: you cancelled the move conversion.\"}");
    };
    Log("moves warning: shown before " + Utf8(script) + " convert");
    return true;
  }
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == L"--svrmod" && args[i + 1].empty()) {  // (SvrmodArgs: a WWE '12 - 2K17 port without svrmod.exe)
      OpenToolsPanel("Missing: svrmod.exe (the Mod Maker's pac tool, needed for WWE '12 - 2K17 discs).");
      return false;
    }
  if (g_tools.empty() || !fs::exists(fs::path(g_tools) / script)) {
    Status("The converter " + Utf8(script) + " isn't in the Mod Maker Tools folder.");
    return false;
  }
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return false;
  }
  std::wstring cmd = Quote(g_python) + L" -X utf8 " + Quote((fs::path(g_tools) / script).wstring());
  for (const auto& a : args) cmd += L" " + Quote(a);
  std::wstring work = cwd;  // (as RunTool: what a script drops lands in Mods\.convert)
  if (work.empty()) {
    const fs::path w = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(w, ec);
    work = w.wstring();
  }
  Log("> " + Utf8(cmd));
  TakeHint();
  RunInBackground([cmd, cwd = work, done] {
    Progress("Running ...");
    std::string out;
    const int code = RunCaptureJson(cmd, cwd, out);
    NoteHint(out);  // (a JSON error naming what's missing)
    std::string hint = TakeHint();
    if (code == 0) hint.clear();
    std::string notices;
    {
      std::lock_guard lock(g_job_mutex);
      notices.swap(g_notices);
    }
    OnUiThread([done, code, out, hint, notices] {
      if (!hint.empty()) OpenToolsPanel("Missing: " + hint + " - install now?");
      if (done) done(code, out);
      if (!notices.empty()) Status(notices);  // (after done: its own status would cover it)
    });
  });
  return true;
}

bool RunTool(const std::wstring& script, const std::vector<std::wstring>& args, std::function<void(int)> done,
             const std::wstring& cwd) {
  if (!ConvertersReady()) return false;
  if (g_tools.empty()) {
    Status("The converter scripts (Mod Maker Tools folder next to the Mod Maker) were not found.");
    return false;
  }
  std::wstring cmd = Quote(g_python) + L" " + Quote((fs::path(g_tools) / script).wstring());
  for (const auto& a : args) cmd += L" " + Quote(a);
  // the tools' working folder: whatever a script drops (surveys, caches) lands here, not in the tools folder
  std::wstring work = cwd;
  if (work.empty()) {
    const fs::path w = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(w, ec);
    work = w.wstring();
  }
  TakeHint();
  return RunCommand(cmd, [done](int code) {
    const std::string hint = TakeHint();
    if (code != 0 && !hint.empty()) OpenToolsPanel("Missing: " + hint + " - install now?");
    if (done) done(code);
  }, work);
}

// ---- the tools panel

namespace {
bool g_panel_open = false;
std::string g_panel_why;
std::atomic<bool> g_installing{false};
std::atomic<int> g_permille{0};
std::mutex g_panel_mutex;
std::string g_install_text, g_install_result;
volatile LONG g_cancel = 0;
bool g_ffmpeg_too = true;
std::wstring g_test_from;  // (--tools-install-from: copy, don't download)
bool g_test_press = false;

void SetupProgress(int permille, const WCHAR* text) {
  if (permille >= 0) g_permille = permille;
  if (text) {
    std::lock_guard lock(g_panel_mutex);
    g_install_text = Utf8(text);
  }
}

bool IsToolsPython(const std::wstring& p) {
  for (const auto& home : ToolsHomes())
    if (!p.empty() && fs::path(p).parent_path() == home / L"python") return true;
  return false;
}

// What "Install what's missing" fetches now (the launcher's Setup items).
unsigned InstallItems() {
  unsigned items = 0;
  if (!PythonMissing().empty()) items |= 1u << SETUP_PYTHON;
  if (FfmpegExe().empty() && g_ffmpeg_too) items |= 1u << SETUP_FFMPEG;
  return items;
}

void StartInstall() {
  if (g_installing || g_game.empty()) return;
  const unsigned items = InstallItems();
  if (!items) return;
  g_installing = true, g_permille = 0, g_cancel = 0;
  {
    std::lock_guard lock(g_panel_mutex);
    g_install_text = "Starting...", g_install_result.clear();
  }
  Log("tools: installing" + std::string(items & (1u << SETUP_PYTHON) ? " Python + numpy + Pillow" : "") +
      (items & (1u << SETUP_FFMPEG) ? " ffmpeg" : "") + " into " + Utf8((fs::path(g_game) / L"Tools").wstring()));
  std::thread([items, game = g_game, from = g_test_from] {
    WCHAR err[600] = L"";
    int ok = 1;
    if (!from.empty()) {  // (test: the same flow, the files copied from an earlier install)
      std::error_code ec;
      for (const auto& [bit, sub] : {std::make_pair(unsigned(SETUP_PYTHON), L"python"), std::make_pair(unsigned(SETUP_FFMPEG), L"ffmpeg")})
        if (items & (1u << bit)) {
          SetupProgress(bit == SETUP_PYTHON ? 300 : 700, (std::wstring(L"Copying ") + sub + L" (test)").c_str());
          fs::create_directories(fs::path(game) / L"Tools", ec);
          fs::copy(fs::path(from) / L"Tools" / sub, fs::path(game) / L"Tools" / sub,
                   fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
          if (ec) ok = 0, swprintf_s(err, L"copy failed: %hs", ec.message().c_str());
        }
    } else {
      ok = setup_install(game.c_str(), items, SetupProgress, &g_cancel, err, std::size(err));
    }
    const std::string result = ok ? "Installed. The converters use them now." : "Not installed: " + Utf8(err);
    OnUiThread([result, ok] {
      RecheckTools();
      PythonMissing();
      {
        std::lock_guard lock(g_panel_mutex);
        g_install_result = result;
      }
      Log("tools: " + result);
      Status(result);
      if (ok) g_panel_why.clear();
      g_installing = false;
    });
  }).detach();
}

void Row(bool ok, bool optional, const char* name, const std::string& detail) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  if (ok) ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.45f, 1), "found");
  else if (optional) ImGui::TextDisabled("optional");
  else ImGui::TextColored(kWarn, "MISSING");
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(name);
  ImGui::TableNextColumn();
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(detail.c_str());
  ImGui::PopTextWrapPos();
}

std::string Where(const std::wstring& p) { return Utf8(p); }

// The Bink 2 tool (RAD Video Tools), as the launcher's Setup looks for it.
std::wstring Bink2() {
  wchar_t buf[MAX_PATH * 2];
  if (SearchPathW(nullptr, L"radvideo64.exe", nullptr, DWORD(std::size(buf)), buf, nullptr)) return buf;
  std::error_code ec;
  for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"LOCALAPPDATA"}) {
    wchar_t b[MAX_PATH];
    if (!GetEnvironmentVariableW(env, b, MAX_PATH)) continue;
    for (const wchar_t* sub : {L"RADVideo", L"RAD Video Tools", L"RAD Game Tools\\RADVideo"})
      if (fs::exists(fs::path(b) / sub / L"radvideo64.exe", ec)) return (fs::path(b) / sub / L"radvideo64.exe").wstring();
  }
  return L"";
}
}  // namespace

std::string ToolsReport() {
  RecheckTools();
  const std::wstring py = PythonExe(), bink = Bink2();
  const std::string missing = PythonMissing().empty() ? "" : g_missing;
  auto row = [](bool ok, bool optional, const char* name, const std::string& detail) {
    return std::string(ok ? "found    " : optional ? "optional " : "MISSING  ") + name + ": " + detail + "\n";
  };
  return row(!py.empty(), false, "Python 3", py.empty() ? "not found" : (IsToolsPython(py) ? "the Mod Maker's own: " : "your own: ") + Utf8(py)) +
         row(!py.empty() && missing.empty(), false, "numpy + Pillow", missing.empty() ? (py.empty() ? "-" : "found") : "missing: " + missing) +
         row(!ToolsDir().empty(), false, "Converter scripts", ToolsDir().empty() ? "not found" : Utf8(ToolsDir())) +
         row(!SvrmodExe().empty(), false, "svrmod.exe", SvrmodExe().empty() ? "not found" : Utf8(SvrmodExe())) +
         row(!VgmstreamExe().empty(), false, "vgmstream", VgmstreamExe().empty() ? "not found" : Utf8(VgmstreamExe())) +
         row(!FfmpegExe().empty(), true, "ffmpeg", FfmpegExe().empty() ? "not found" : Utf8(FfmpegExe())) +
         row(!bink.empty(), true, "Bink 2 tool", bink.empty() ? "not found" : Utf8(bink)) +
         "Bink 2 tool chosen: " + (g_settings.bink2_tool.empty() ? std::string("(none)") : Utf8(g_settings.bink2_tool)) + "\n";
}

void OpenToolsPanel(const std::string& why) {
  g_panel_open = true;
  if (!why.empty()) g_panel_why = why, Status(why);
}

bool ConvertersReady() {
  const std::string m = PythonMissing();
  if (!m.empty()) {
    OpenToolsPanel("Missing: " + g_missing + " - install now?");
    return false;
  }
  if (ToolsDir().empty()) {
    OpenToolsPanel("Missing: the converter scripts (the Mod Maker Tools folder next to the Mod Maker).");
    return false;
  }
  return true;
}

void StartupToolsCheck() {
  if (g_game.empty()) return;
  RecheckTools();
  const bool need = !PythonMissing().empty() || ToolsDir().empty() || SvrmodExe().empty();
  if (need && !g_behind) OpenToolsPanel("Some tools the converters (the From another game tabs) need are missing.");
}

void TestToolsPanel(bool install, const std::wstring& from) {
  g_panel_open = true;
  if (!from.empty()) g_test_from = from;
  if (install) g_test_press = true;
}

void ToolsPanel() {
  if (!g_panel_open) return;
  if (g_test_press && !g_installing && ImGui::GetFrameCount() > 30) g_test_press = false, StartInstall();
  ImGui::SetNextWindowSize(ImVec2(760 * g_scale, 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Mod Maker tools", &g_panel_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  if (!g_panel_why.empty()) ImGui::TextColored(kWarn, "%s", g_panel_why.c_str());
  ImGui::TextWrapped("What bringing things over from your other WWE games needs (the From another game tabs, Moves & entrances and the Simple makers), found or missing:");
  const std::wstring py = PythonExe();
  const std::string missing = PythonMissing().empty() ? "" : g_missing;
  const bool py_found = !py.empty();
  const bool mods_ok = py_found && missing.empty();
  if (ImGui::BeginTable("tools", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    Row(py_found, false, "Python 3",
        !py_found ? "not found" : IsToolsPython(py) ? "the Mod Maker's own: " + Where(py) : "your own: " + Where(py));
    Row(mods_ok, false, "numpy + Pillow",
        mods_ok ? "found" : !py_found ? "come with the Mod Maker's Python" : "missing in that Python: " + missing);
    Row(!ToolsDir().empty(), false, "Converter scripts",
        ToolsDir().empty() ? "not found: the \"Mod Maker Tools\" folder next to the Mod Maker - unpack the release zip again" : Where(ToolsDir()));
    Row(!SvrmodExe().empty(), false, "svrmod.exe",
        SvrmodExe().empty() ? "not found: needed to read WWE '12 - 2K17 discs; it comes in the release zip's Mod Maker Tools folder"
                            : Where(SvrmodExe()));
    Row(!VgmstreamExe().empty(), false, "vgmstream",
        VgmstreamExe().empty() ? "not found: theme songs from WWE '12 - 2K17 (without it the superstars come without them); it comes "
                                 "in the release zip's Mod Maker Tools\\vgmstream"
                               : Where(VgmstreamExe()));
    Row(!FfmpegExe().empty(), true, "ffmpeg", FfmpegExe().empty() ? "SvR 2010 theme songs and the entrance preview's music" : Where(FfmpegExe()));
    const std::wstring bink = Bink2();
    Row(!bink.empty(), true, "Bink 2 tool", bink.empty() ? "only for .bk2 videos: RAD Video Tools, your own download from RAD's site" : Where(bink));
    ImGui::EndTable();
  }
  ImGui::Spacing();
  const bool ships = ToolsDir().empty() || SvrmodExe().empty() || VgmstreamExe().empty();
  if (ships)
    ImGui::TextWrapped("The scripts, svrmod.exe and vgmstream come with the Mod Maker: unpack the latest release zip over the "
                       "game folder (keep \"Mod Maker Tools\" next to the Mod Maker).");
  const unsigned items = InstallItems();
  if (g_installing) {
    std::string text;
    {
      std::lock_guard lock(g_panel_mutex);
      text = g_install_text;
    }
    ImGui::ProgressBar(g_permille / 1000.0f, ImVec2(-1, 0), text.c_str());
    if (ImGui::Button("Cancel")) InterlockedExchange(&g_cancel, 1);
  } else {
    if (FfmpegExe().empty()) ImGui::Checkbox("Also ffmpeg (optional)", &g_ffmpeg_too);
    if (items) {
      WCHAR list[1024];
      unsigned long long total = 0;
      setup_describe(items, list, std::size(list), &total);
      ImGui::PushTextWrapPos(0);  // (the game folder can be long: wrapped, not cut at the window's edge)
      ImGui::TextDisabled("Downloads (only from the official sites, checked against pinned SHA-256s; into %s\\Tools, no admin "
                          "rights):", Utf8(g_game).c_str());
      ImGui::TextUnformatted(Utf8(list).c_str());
      ImGui::PopTextWrapPos();
      char label[96];
      std::snprintf(label, sizeof label, "Install what's missing (%.0f MB)", double(total) / 1e6);
      ImGui::BeginDisabled(g_game.empty());
      if (ImGui::Button(label)) StartInstall();
      ImGui::EndDisabled();
      ImGui::SameLine();
    } else if (mods_ok) {
      ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.45f, 1), "Python, numpy and Pillow are ready.");
      ImGui::SameLine();
    }
    if (ImGui::Button("Check again")) RecheckTools(), PythonMissing();
    std::string result;
    {
      std::lock_guard lock(g_panel_mutex);
      result = g_install_result;
    }
    if (!result.empty()) ImGui::TextWrapped("%s", result.c_str());
  }
  ImGui::End();
}

}  // namespace mm
