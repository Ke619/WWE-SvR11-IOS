// SvR2011 Mod Maker: the window, the shell (title bar, rail, status line,
// log, problems), projects, settings and the game data every page shares.
// Pages: arena_page.cpp (arenas, backstage, VS screen), star_page.cpp,
// signs_page.cpp, backgrounds_page.cpp, media_page.cpp, moves_page.cpp, assets_page.cpp,
// anims_page.cpp, icons_page.cpp, help_page.cpp; the 3D editor is editor.cpp.
#include "app.h"
#include "move_check.h"
#include "mod_check.h"

#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "char_preview.h"
#include "disc_list.h"
#include "audio_out.h"
#include <future>
#include <restartmanager.h>
#include "simple_port.h"
#include "simple_steps.h"
#include "model_materials.h"
#include "model_materials.h"
#include "hair_sim.h"
#include "editor.h"
#include "full_preview.h"
#include "tool_run.h"
#include "json.h"
#include "session_log.h"
extern "C" {
#include "../launcher/move_warning.h"
#include "../launcher/movie_maker.h"
}
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "svrfmt/disc.h"
#include "rig_map.h"
#include "svrfmt/char_fbx.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/png.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mm {

using namespace svrfmt;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
HWND g_wnd = nullptr;
std::wstring g_game;
float g_scale = 1;
bool g_behind = false;

void ShellOpen(const std::wstring& what) {
  if (what.empty()) return;
  if (g_behind) {
    Log("(test: would open " + Utf8(what) + ")");
    return;
  }
  ShellExecuteW(nullptr, L"open", what.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
PageId g_page = PageId::kArena;
std::wstring g_project_file;
PageId g_project_page = PageId::kArena;
Settings g_settings;

namespace {

IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void CreateTarget() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
}

bool CreateDevice(HWND w) {
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = w;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  // (test aid SVR2011_MM_D3D_DEBUG=1: the D3D11 debug layer, its messages logged - DrainD3DMessages)
  const UINT flags = _wgetenv(L"SVR2011_MM_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx)) &&
      FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx)))
    return false;
  CreateTarget();
  return true;
}

// The debug layer's messages (SVR2011_MM_D3D_DEBUG), each kind logged once, 200 at most.
void DrainD3DMessages() {
  static ID3D11InfoQueue* q = nullptr;
  static bool tried = false;
  static std::set<int> seen;
  if (!tried && _wgetenv(L"SVR2011_MM_D3D_DEBUG")) {
    tried = true;
    if (g_dev) g_dev->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&q));
    Log(q ? "d3d11: debug layer on" : "d3d11: no debug layer (Graphics Tools not installed?)");
  }
  if (!q) return;
  for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n && seen.size() < 200; ++i) {
    SIZE_T len = 0;
    q->GetMessage(i, nullptr, &len);
    std::vector<char> b(len);
    auto* m = reinterpret_cast<D3D11_MESSAGE*>(b.data());
    if (FAILED(q->GetMessage(i, m, &len)) || !seen.insert(int(m->ID)).second) continue;
    Log("d3d11 debug " + std::to_string(int(m->Severity)) + ": " + std::string(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0));
  }
  q->ClearStoredMessages();
}

// ---------------------------------------------------------------- log / work

std::mutex g_log_mutex;
std::vector<std::string> g_log;
std::string g_status, g_progress;
std::atomic<bool> g_busy{false};
std::thread g_worker;
std::mutex g_ui_mutex;
std::vector<std::function<void()>> g_ui_queue;
std::map<PageId, int> g_touches;  // edits a page reported with Touch()

}  // namespace

ID3D11ShaderResourceView* MakeTexture(const Image& img) {
  if (img.rgba.empty() || !g_dev) return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(img.w);
  td.Height = UINT(img.h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd = {img.rgba.data(), UINT(img.w * 4), 0};
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) {
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
  }
  return srv;
}

void Picture::Draw(float w, float h) {
  if (Empty()) {
    ImGui::Dummy(ImVec2(w, h));
    return;
  }
  const float k = std::min(w / image.w, h / image.h);
  const ImVec2 size(image.w * k, image.h * k);
  const ImVec2 at = ImGui::GetCursorPos();
  ImGui::SetCursorPos(ImVec2(at.x + (w - size.x) / 2, at.y + (h - size.y) / 2));
  ImGui::Image(Id(), size);
  ImGui::SetCursorPos(ImVec2(at.x, at.y + h));
  ImGui::Dummy(ImVec2(w, 0));
}

// ---------------------------------------------------------------- strings

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return w;
}

std::string PathStr(const fs::path& p) { return Utf8(p.wstring()); }

#ifndef PORT_VERSION
#define PORT_VERSION "dev"
#endif
std::string MadeWith() { return std::string("made_with=Mod Maker ") + PORT_VERSION + "\n"; }
std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }
std::string Upper(std::string s) { for (char& c : s) c = char(std::toupper(uint8_t(c))); return s; }

std::string IdFrom(const std::string& name, const char* fallback) {
  std::string id;
  for (char c : name) {
    if (std::isalnum(uint8_t(c))) id.push_back(char(std::tolower(uint8_t(c))));
    else if (!id.empty() && id.back() != '_') id.push_back('_');
  }
  while (!id.empty() && id.back() == '_') id.pop_back();
  return id.empty() ? fallback : id;
}

std::string FileName(const std::wstring& path) { return Utf8(fs::path(path).filename().wstring()); }

std::string Human(size_t bytes) {
  char b[32];
  if (bytes >= 1048576) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
  else if (bytes >= 1024) std::snprintf(b, sizeof b, "%.0f KB", bytes / 1024.0);
  else std::snprintf(b, sizeof b, "%zu bytes", bytes);
  return b;
}

std::string Value(const std::string& text, const char* key) {
  const std::string k = std::string("\n") + key + "=";
  const std::string t = "\n" + text;
  const size_t at = t.find(k);
  if (at == std::string::npos) return {};
  size_t end = t.find_first_of("\r\n", at + k.size());
  if (end == std::string::npos) end = t.size();
  return t.substr(at + k.size(), end - at - k.size());
}

std::vector<std::string> Values(const std::string& text, const char* key) {
  std::vector<std::string> out;
  const std::string k = std::string("\n") + key + "=";
  const std::string t = "\n" + text;
  for (size_t at = t.find(k); at != std::string::npos; at = t.find(k, at + 1)) {
    size_t end = t.find_first_of("\r\n", at + k.size());
    if (end == std::string::npos) end = t.size();
    out.push_back(t.substr(at + k.size(), end - at - k.size()));
  }
  return out;
}

// ---------------------------------------------------------------- dialogs

const COMDLG_FILTERSPEC kPictureFilter[1] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp, *.dds)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds"}};
const COMDLG_FILTERSPEC kSoundFilter[1] = {{L"Sounds (*.mp3, *.m4a, *.wav, *.flac, *.wma, *.ogg)", L"*.mp3;*.m4a;*.aac;*.wav;*.flac;*.wma;*.ogg"}};
const COMDLG_FILTERSPEC kBinkFilter[1] = {{L"Movies for the game (*.mp4, *.bik)", L"*.mp4;*.bik"}};
const COMDLG_FILTERSPEC kPacFilter[1] = {{L"Character model pac (*.pac)", L"*.pac"}};
const COMDLG_FILTERSPEC kModFilter[1] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
const COMDLG_FILTERSPEC kProjectFilter[1] = {{L"Mod Maker project (*.svrproj)", L"*.svrproj"}};
const COMDLG_FILTERSPEC kOpenFilter[1] = {{L"Projects and mods (*.svrproj, *.svrmod)", L"*.svrproj;*.svrmod"}};

std::wstring PickFile(bool save, const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec,
                      const wchar_t* def_ext, const wchar_t* def_name) {
  IFileDialog* dlg = nullptr;
  std::wstring out;
  if (g_behind) {  // (a hidden test can't answer a dialog: it would hang)
    Log("(test: a file dialog was skipped: " + Utf8(title) + ")");
    return out;
  }
  if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return out;
  dlg->SetTitle(title);
  if (nspec) dlg->SetFileTypes(nspec, spec);
  if (def_ext) dlg->SetDefaultExtension(def_ext);
  if (def_name) dlg->SetFileName(def_name);
  IShellItem* item = nullptr;
  PWSTR path = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)) &&
      SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    out = path;
    CoTaskMemFree(path);
  }
  if (item) item->Release();
  dlg->Release();
  return out;
}

std::vector<std::wstring> PickFiles(const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec) {
  std::vector<std::wstring> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT);
  dlg->SetTitle(title);
  dlg->SetFileTypes(nspec, spec);
  IShellItemArray* items = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResults(&items))) {
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
      IShellItem* item = nullptr;
      PWSTR path = nullptr;
      if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out.push_back(path);
        CoTaskMemFree(path);
      }
      if (item) item->Release();
    }
    items->Release();
  }
  dlg->Release();
  return out;
}

std::wstring PickFolder(const wchar_t* title) {
  IFileOpenDialog* dlg = nullptr;
  std::wstring out;
  if (g_behind) {  // (a hidden test can't answer a dialog: it would hang)
    Log("(test: a folder dialog was skipped: " + Utf8(title) + ")");
    return out;
  }
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opt = 0;
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS);
  dlg->SetTitle(title);
  IShellItem* item = nullptr;
  PWSTR path = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)) &&
      SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    out = path;
    CoTaskMemFree(path);
  }
  if (item) item->Release();
  dlg->Release();
  return out;
}

bool LoadPicture(const std::wstring& file, Image& out) {
  if (Lower(FileName(file)).ends_with(".dds")) {
    Bytes d;
    if (ReadFile(Utf8(file), d) && DdsDecode(d, out)) return true;
  } else if (LoadImageFile(Utf8(file), out)) {
    return true;
  }
  Log("That picture could not be read: " + Utf8(file));
  return false;
}

// ---------------------------------------------------------------- log / work

void Log(const std::string& s) {
  session::Line(s);  // (the session log file, session_log.h)
  std::lock_guard lock(g_log_mutex);
  g_log.push_back(s);
  if (g_log.size() > 2000) g_log.erase(g_log.begin(), g_log.begin() + 500);
}

void WriteLogTo(const std::wstring& file) {
  if (FILE* f = _wfopen(file.c_str(), L"wb")) {
    std::lock_guard lock(g_log_mutex);
    for (const auto& line : g_log) std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
  }
}

void Status(const std::string& s) {
  Log(s);
  std::lock_guard lock(g_log_mutex);
  g_status = s;
}

void Progress(const std::string& what) {
  std::lock_guard lock(g_log_mutex);
  g_progress = what;
}
std::string ProgressNow() {
  std::lock_guard lock(g_log_mutex);
  return g_progress;
}

void RunInBackground(std::function<void()> fn) {
  if (g_busy) {
    Status("Still working on the last job: wait a moment.");
    return;
  }
  if (g_worker.joinable()) g_worker.join();
  g_busy = true;
  g_worker = std::thread([fn] {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ms >= 50) Log("perf: background job " + std::to_string(ms) + " ms");
    {
      std::lock_guard lock(g_log_mutex);
      g_progress.clear();
    }
    g_busy = false;
  });
}

bool Busy() { return g_busy; }

void WhenIdle(std::function<void()> fn) {
  if (!Busy()) fn();
  else OnUiThread([fn] { WhenIdle(fn); });
}

void OnUiThread(std::function<void()> fn) {
  std::lock_guard lock(g_ui_mutex);
  g_ui_queue.push_back(std::move(fn));
}

void Touch() { ++g_touches[g_page == PageId::kEditor ? PageId::kArena : g_page]; }

// ---------------------------------------------------------------- problems

int ErrorCount(const std::vector<Problem>& p) {
  int n = 0;
  for (const auto& x : p) n += x.error;
  return n;
}

// ---- installing a mod's files

std::string WhoHolds(const fs::path& dir) {
  std::vector<std::wstring> files;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec) && files.size() < 256) files.push_back(e.path().wstring());
  if (files.empty()) return "";
  DWORD session = 0;
  WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
  if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return "";
  std::vector<LPCWSTR> names;
  for (const auto& f : files) names.push_back(f.c_str());
  std::string who;
  if (RmRegisterResources(session, UINT(names.size()), names.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
    UINT need = 0, have = 16;
    DWORD reason = 0;
    std::vector<RM_PROCESS_INFO> info(have);
    if (RmGetList(session, &need, &have, info.data(), &reason) == ERROR_SUCCESS)
      for (UINT i = 0; i < have; ++i) {
        const std::string n = Utf8(info[i].strAppName);
        if (who.find(n) == std::string::npos) who += (who.empty() ? "" : ", ") + n;
      }
  }
  RmEndSession(session);
  return who;
}

namespace {
// A Windows error on `path`, in plain words.
std::string Explain(DWORD err, const fs::path& path, const fs::path& dir) {
  const std::string p = PathStr(path);
  const bool onedrive = Lower(PathStr(dir)).find("onedrive") != std::string::npos;
  switch (err) {
    case ERROR_ACCESS_DENIED:
      return "Windows refused to let the Mod Maker write into " + PathStr(dir) + " (access denied). The folder may be read-only, "
             "blocked by Windows Security's \"Controlled folder access\" or an antivirus" + (onedrive ? ", or held by OneDrive (the game is in a OneDrive folder)" : "") +
             ". Allow the Mod Maker there, or move the game to a plain folder such as C:\\Games\\SvR2011.";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: {
      const std::string who = WhoHolds(dir);
      return FileName(path.wstring()) + " is in use" + (who.empty() ? " by another program" : " by " + who) + ": close " +
             (who.empty() ? "it" : "that") + " and install again.";
    }
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_PATH_NOT_FOUND:
      return p.size() > 240 ? "The path is too long for Windows (" + std::to_string(p.size()) + " characters): move the game to a shorter folder, such as C:\\Games\\SvR2011."
                            : "Windows couldn't make the folder " + PathStr(path.parent_path()) + ".";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return "The drive is full: free some space and install again.";
    case ERROR_WRITE_PROTECT:
      return "The drive is write-protected.";
    default: {
      wchar_t* msg = nullptr;
      FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                     reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
      std::string m = msg ? Utf8(msg) : "error " + std::to_string(err);
      if (msg) LocalFree(msg);
      while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == '.')) m.pop_back();
      return "Windows couldn't write " + p + ": " + m + ".";
    }
  }
}

bool WriteOne(const fs::path& path, const Bytes& data, DWORD& err) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return err = GetLastError(), false;
  DWORD done = 0;
  const BOOL ok = data.empty() || ::WriteFile(h, data.data(), DWORD(data.size()), &done, nullptr);
  err = ok ? 0 : GetLastError();
  CloseHandle(h);
  return ok && done == data.size();
}

// The Mod Maker's own open files (a song playing, the Full preview's video), let go on the UI thread.
void ReleaseOwnFiles() {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> f = done->get_future();
  OnUiThread([done] {
    audio_out::Unload();
    if (full_preview::IsOpen()) full_preview::Close();
    done->set_value();
  });
  f.wait_for(std::chrono::seconds(3));
}
}  // namespace

bool InstallModFiles(const fs::path& dir, const std::vector<ZipEntry>& files, std::string* why) {
  {  // (a mod with custom moves - a move pack, or a superstar mod's moves\ pack: asked first, as the launcher does;
     // a hidden test can't answer: only logged - launcher/move_warning.h)
    bool moves = false;
    std::string name;
    for (const auto& f : files) {
      moves |= f.name == "pack.txt" || f.name == "moves/pack.txt";
      if (f.name == "manifest.txt") name = Value(std::string(f.data.begin(), f.data.end()), "name");
    }
    if (name.empty()) name = PathStr(dir.filename());
    if (moves) {
      const fs::path game = dir.parent_path().parent_path().parent_path();  // (<game>\Mods\<type>\<folder>)
      Log("install: " + name + " has custom moves (experimental, most likely broken)");
      if (!move_warning_ask(g_wnd, game.c_str(), Wide(name).c_str(), g_behind)) {
        if (why) *why = "it has custom moves (you chose not to install it).";
        Log("install: " + name + " not installed (custom moves: No)");
        return false;
      }
    }
  }
  std::error_code ec;
  const fs::path stage = dir.parent_path() / (L"." + dir.filename().wstring() + L".new");
  const fs::path old = dir.parent_path() / (L"." + dir.filename().wstring() + L".old");
  fs::remove_all(stage, ec);
  fs::remove_all(old, ec);
  for (const auto& f : files) {
    if (f.name.empty() || f.name.back() == '/') continue;
    DWORD err = 0;
    if (!WriteOne(stage / fs::u8path(f.name), f.data, err)) {
      if (why) *why = Explain(err, stage / fs::u8path(f.name), dir);
      Log("install: writing " + f.name + " failed (Windows error " + std::to_string(err) + ")");
      fs::remove_all(stage, ec);
      return false;
    }
  }
  // the old one out of the way, the new one in (a folder held open fails here: the Mod Maker's own let go, once more)
  for (int attempt = 0; fs::exists(dir, ec); ++attempt) {
    if (MoveFileExW(dir.c_str(), old.c_str(), 0)) break;
    const DWORD err = GetLastError();
    const std::string who = WhoHolds(dir);
    Log("install: moving the old folder failed (Windows error " + std::to_string(err) + ")" + (who.empty() ? "" : ", held by " + who));
    const bool mine = who.find("Mod Maker") != std::string::npos || who.find("SvR2011 Mod Maker") != std::string::npos;
    if (attempt == 0 && (mine || who.empty())) {
      ReleaseOwnFiles();
      Sleep(300);
      continue;
    }
    if (why)
      *why = !who.empty() ? "The old copy of the mod (" + PathStr(dir) + ") is in use by " + who + ": close " + (mine ? "its preview" : "that program") + " and install again."
                          : Explain(err, dir, dir);
    fs::remove_all(stage, ec);
    return false;
  }
  if (!MoveFileExW(stage.c_str(), dir.c_str(), 0)) {
    const DWORD err = GetLastError();
    MoveFileExW(old.c_str(), dir.c_str(), 0);  // (the old one back)
    if (why) *why = Explain(err, dir, dir);
    fs::remove_all(stage, ec);
    return false;
  }
  fs::remove_all(old, ec);
  {  // (the session log, for a report: what was installed, its theme / music / movie files)
    std::string keys;
    for (const auto& f : files)
      if (f.name == "manifest.txt") {
        const std::string t(f.data.begin(), f.data.end());
        for (const char* k : {"type", "name", "base", "song", "music", "movie", "theme_gain", "call_gain", "entrance", "victory", "cae", "moves", "pack"})
          if (const std::string v = Value(t, k); !v.empty()) keys += std::string(" ") + k + "=" + v;
      }
    Log("install: " + PathStr(dir.parent_path().filename()) + "/" + PathStr(dir.filename()) + " (" + std::to_string(files.size()) +
        " files):" + keys);
  }
  return true;
}

int g_fixes_done = 0;
int g_test_install_frames = -1;  // --install-star: the Superstar page's Install, once the page settled (test aid)
std::string g_test_fix;  // --run-fix "<text>[|<n>]": fix n (0: the recommended) of the page's first problem with <text>
int g_test_fix_frames = 0;

bool TestFixOn(const std::vector<Problem>& problems) {
  if (g_test_fix.empty()) return false;
  const size_t bar = g_test_fix.find('|');
  const std::string want = g_test_fix.substr(0, bar);
  const int n = bar == std::string::npos ? 0 : std::atoi(g_test_fix.c_str() + bar + 1);
  for (const auto& p : problems)
    Log("test: problem: " + p.text + (p.fixes.empty() ? "" : "  [" + std::to_string(p.fixes.size()) + " fixes: " + p.fixes[0].label + "]"));
  bool done = false;
  for (const auto& p : problems)
    if (p.text.find(want) != std::string::npos && n < int(p.fixes.size())) {
      Log("test: running fix \"" + p.fixes[size_t(n)].label + "\" of: " + p.text);
      RunFix(p.fixes[size_t(n)]);
      done = true;
      break;
    }
  if (!done) Log("test: no problem with \"" + want + "\" and fix " + std::to_string(n));
  g_test_fix.clear();
  return true;
}
int FixesDone() { return g_fixes_done; }
void FixChanged() { ++g_fixes_done; }

// The mod folders threads_parts.py found no colour masks in, kept in <game>\Mods\.convert\threads_no_masks.txt (one
// path a line) so the doctor offers making them at once next time.
std::set<std::wstring> g_threads_no_masks;
bool g_threads_no_masks_read = false;
fs::path NoMasksFile() { return fs::path(g_game) / L"Mods" / L".convert" / L"threads_no_masks.txt"; }
void ReadNoMasks() {
  if (g_threads_no_masks_read || g_game.empty()) return;
  g_threads_no_masks_read = true;
  Bytes t;
  if (!ReadFile(PathStr(NoMasksFile()), t)) return;
  const std::string s(t.begin(), t.end());
  for (size_t at = 0; at < s.size();) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    if (e > at) g_threads_no_masks.insert(Wide(s.substr(at, e - at)));
    at = e + 1;
  }
}
void WriteNoMasks() {
  std::string t;
  for (const auto& d : g_threads_no_masks) t += Utf8(d) + "\n";
  std::error_code ec;
  fs::create_directories(NoMasksFile().parent_path(), ec);
  WriteFile(PathStr(NoMasksFile()), Bytes(t.begin(), t.end()));
}
bool ThreadsNoMasks(const fs::path& dir) {
  ReadNoMasks();
  std::error_code ec;
  return g_threads_no_masks.count(fs::absolute(dir).wstring()) > 0 && !fs::exists(dir / "threads.txt", ec);
}
void MakeThreadsParts(const fs::path& dir, std::function<void(int, const std::string&)> done, bool make_masks) {
  const std::wstring d = fs::absolute(dir).wstring();
  std::vector<std::wstring> args = {d, L"--json", L"--game", g_game};
  if (make_masks) args.push_back(L"--make-masks");
  const bool started = RunToolJson(L"threads_parts.py", args, [d, done](int code, const std::string& out) {
    Json j;
    const std::string msg = ParseJson(out, j) ? j["message"].Str() : std::string();
    ReadNoMasks();
    if (code == 3 && g_threads_no_masks.insert(d).second) WriteNoMasks();
    if (code == 0 && g_threads_no_masks.erase(d)) WriteNoMasks();
    for (const auto& m : j["masks"].items)  // (masks painted: which model file, attire, channels)
      Log("threads parts: masks made: " + m["pac"].Str() + " attire " + std::to_string(m["attire"].Int()) +
          (m["info"].Str().empty() ? "" : ": " + m["info"].Str()));
    Log("threads parts: " + Utf8(d) + ": exit " + std::to_string(code) + (msg.empty() ? "" : ": " + msg));
    if (done) done(code, msg);
  });
  if (!started && done) done(-1, "the Mod Maker tools (Python) aren't ready, or another tool is running");
}

void RunFix(const FixAction& f) {
  if (f.run) f.run();
  ++g_fixes_done;
  Status("Done: " + (f.done.empty() ? f.label : f.done));
}

bool FixButtons(const Problem& p, bool accent_first) {
  bool pressed = false;
  for (size_t k = 0; k < p.fixes.size(); ++k) {
    ImGui::PushID(int(k));
    const bool accent = k == 0 && accent_first;
    if (accent) PushAccent();
    if (ImGui::SmallButton(p.fixes[k].label.c_str())) {
      const FixAction f = p.fixes[k];  // (the list is rebuilt: keep a copy)
      RunFix(f);
      pressed = true;
    }
    if (accent) PopAccent();
    ImGui::PopID();
    if (k + 1 < p.fixes.size()) ImGui::SameLine();
  }
  return pressed;
}

void DrawProblems(const std::vector<Problem>& problems) {
  for (size_t i = 0; i < problems.size(); ++i) {
    const Problem& p = problems[i];
    ImGui::PushID(int(i));
    ImGui::PushStyleColor(ImGuiCol_Text, p.error ? kBad : kWarn);
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s%s%s", p.text.c_str(), p.fix.empty() ? "" : "  -  ", p.fix.c_str());
    ImGui::PopStyleColor();
    if (!p.fixes.empty()) {
      ImGui::Indent();
      if (FixButtons(p)) {
        ImGui::Unindent();
        ImGui::PopID();
        break;  // (the fix changed the list)
      }
      ImGui::Unindent();
    }
    ImGui::PopID();
  }
}

int ModButtons(const std::vector<Problem>& problems, bool with_test, float width) {
  static int pending = 0;  // (a press waiting for the warnings to be confirmed)
  const int errors = ErrorCount(problems);
  const bool warnings = int(problems.size()) > errors;
  const float w = (width > 0 ? width : 200) * g_scale;
  int pressed = 0;
  ImGui::BeginDisabled(errors > 0 || Busy());
  PushAccent();
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(w, 0))) pressed = 1;
  PopAccent();
  ImGui::SameLine();
  if (ImGui::Button("Install into game", ImVec2(w, 0))) pressed = 2;
  if (with_test) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kGood);
    if (ImGui::Button("Test in game", ImVec2(w, 0))) pressed = 3;
    ImGui::PopStyleColor();
  }
  ImGui::EndDisabled();
  if (errors > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tip = "Fix the problems first:";
    for (const auto& p : problems)
      if (p.error) tip += "\n- " + p.text;
    ImGui::SetTooltip("%s", tip.c_str());
  }
  if (pressed && warnings) {
    pending = pressed;
    ImGui::OpenPopup("Warnings");
    pressed = 0;
  }
  if (ImGui::BeginPopupModal("Warnings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("The mod will work, but:");
    DrawProblems(problems);
    ImGui::Separator();
    const char* go = pending == 1 ? "Save anyway" : pending == 2 ? "Install anyway" : "Test anyway";
    if (ImGui::Button(go, ImVec2(150 * g_scale, 0))) {
      pressed = pending;
      pending = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Go back", ImVec2(150 * g_scale, 0))) {
      pending = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  return pressed;
}

// ---------------------------------------------------------------- game data

ArenaInfo g_arenas[20] = {
    {"arena_SD", 0, "SmackDown"},          {"arena_RAW", 1, "RAW"},
    {"arena_WM26", 11, "WrestleMania XXVI"}, {"arena_slam", 4, "SummerSlam"},
    {"arena_SS", 17, "Superstars"},     {"arena_HinC", 6, "Hell in a Cell"},
    {"arena_Judg", 13, "Judgment Day"},     {"arena_BASH", 15, "The Bash"},
    {"arena_NoC", 14, "Night of Champions"}, {"arena_BP", 5, "Breaking Point"},
    {"arena_RR", 9, "Royal Rumble"},        {"arena_BR", 18, "Bragging Rights"},
    {"arena_Series", 7, "Survivor Series"}, {"arena_TLC", 8, "TLC"},
    {"arena_ttt", 3, "Tribute to Troops"}, {"arena_EC", 10, "Elimination Chamber"},
    {"arena_back", 12, "Backlash"},         {"arena_EXT", 16, "Extreme Rules"},
    {"arena_ECW", 2, "ECW"},                {"arena_drui", 19, "Undertaker"},
};

std::string ArenaPath(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "bg%02d.pac", g_arenas[i].number);
  return PathStr(fs::path(g_game) / L"pac" / L"bg" / b);
}

int ArenaTileOf(int number) {
  for (int i = 0; i < 20; ++i)
    if (g_arenas[i].number == number) return i;
  return -1;
}

namespace {

bool LoadBanners() {
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac"), d) || !EpacRead(d, e)) return false;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != "MASI") continue;
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(pe.data), texs)) continue;
        for (const auto& t : texs)
          for (auto& a : g_arenas)
            if (t.name == a.banner && a.tile.Empty()) {
              Image img;
              if (DdsDecode(t.data, img)) a.tile.Set(std::move(img));
            }
      }
    }
  return true;
}

std::vector<StarInfo> g_stars;
std::map<int, std::array<int, 7>> g_ratings;
std::map<int, int> g_scales;  // record +28 (4096 = 1.0)
bool g_stars_loaded = false;
std::map<int, Picture> g_renders;

// Names and ratings from chEtc.pac CHAR/DAT (260-byte records after a 4-byte
// header: ratings at +0, the full name at +34); the playable ones are those
// with a render in DLC_HD.pac.
void LoadStars() {
  g_stars_loaded = true;
  if (g_game.empty()) return;
  Bytes d;
  Epac e;
  std::map<int, std::string> names;
  if (ReadFile(PathStr(fs::path(g_game) / L"pac" / L"ch" / L"chEtc.pac"), d) && EpacRead(d, e))
    for (const auto& g : e.groups)
      for (const auto& en : g.entries)
        if (g.type == "CHAR" && en.name.rfind("DAT", 0) == 0) {
          std::vector<PachEntry> pe;
          if (!PachRead(en.data, pe)) continue;
          for (const auto& x : pe) {
            const Bytes r = Unpack(x.data);
            if (r.size() < 4 + 66) continue;
            const char* nm = reinterpret_cast<const char*>(&r[4 + 34]);
            names[int(x.id)] = std::string(nm, strnlen(nm, 32));
            std::array<int, 7> v{};
            for (int j = 0; j < 7; ++j) v[j] = r[4 + j];
            g_ratings[int(x.id)] = v;
            g_scales[int(x.id)] = r[4 + 28] | (r[4 + 29] << 8);
          }
        }
  Bytes h;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"DLC_HD.pac"), h) || !EpacRead(h, e)) return;
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries) {
        const int id = std::atoi(en.name.c_str());
        if (id < 100 || id > 321) continue;  // (attires 1000+, DLC ids)
        g_stars.push_back({id, names.count(id) ? names[id] : "Superstar " + std::to_string(id)});
      }
  std::sort(g_stars.begin(), g_stars.end(), [](const StarInfo& a, const StarInfo& b) { return a.name < b.name; });
}

}  // namespace

const std::vector<StarInfo>& Stars() {
  if (!g_stars_loaded) LoadStars();
  return g_stars;
}

const StarInfo* StarById(int id) {
  for (const auto& s : Stars())
    if (s.id == id) return &s;
  return nullptr;
}

int StarRating(int id, int k) {
  Stars();
  const auto it = g_ratings.find(id);
  return it == g_ratings.end() ? 74 : std::clamp(it->second[k], 1, 99);
}

float StarScale(int id) {
  Stars();
  const auto it = g_scales.find(id);
  return it == g_scales.end() || it->second < 2048 || it->second > 8192 ? 1.0f : float(it->second) / 4096.0f;
}

Picture* StarRender(int id) {
  auto it = g_renders.find(id);
  if (it != g_renders.end()) return &it->second;
  Picture& p = g_renders[id];
  Bytes h;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"DLC_HD.pac"), h) || !EpacRead(h, e)) return &p;
  char key[8];
  std::snprintf(key, sizeof key, "%04d", id);
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries)
        if (en.name == key) {
          Image img;
          if (DdsDecode(Unpack(en.data), img)) p.Set(std::move(img));  // (BPE-packed DDS)
        }
  return &p;
}

bool ReadPacIndex(const fs::path& file, PacIndex& out) {
  out = PacIndex();
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  Bytes idx(0x4000);
  const bool ok = std::fread(idx.data(), 1, idx.size(), f) == idx.size();
  std::fclose(f);
  return ok && ParsePacIndex(idx, out);
}

bool ParsePacIndex(const Bytes& idx, PacIndex& out) {
  out = PacIndex();
  std::vector<svrfmt::EpacRow> rows;
  if (!svrfmt::EpacTable(idx, 0, rows)) return false;
  out.epk8 = !std::memcmp(idx.data(), "EPK8", 4);
  for (auto& r : rows) {
    PacEntryInfo e;
    e.group = std::move(r.group);
    e.name = std::move(r.name);
    e.offset = r.offset;
    e.size = r.size;
    out.entries.push_back(std::move(e));
  }
  return true;
}

bool ReadPacEntry(const fs::path& file, const PacEntryInfo& e, Bytes& out) {
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  out.resize(e.size);
  const bool ok = _fseeki64(f, int64_t(e.offset), SEEK_SET) == 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool ReadPacEntry(const fs::path& file, const char* name, Bytes& out, const char* group) {
  PacIndex idx;
  if (!ReadPacIndex(file, idx)) return false;
  for (const auto& e : idx.entries)
    if (e.name == name && (!group || e.group == group)) return ReadPacEntry(file, e, out);
  return false;
}

namespace {
std::map<int, std::map<int, std::string>> g_strings;  // table -> id -> text
bool g_strings_loaded = false;

// string.pac: EPAC group SDB, entry "64LL" = every table as PACH ids 0..13 (LL 00 = English;
// IT's is misnamed, so the entry with 14 tables is taken when 6400 is missing). A table:
// {u32 0, u32 count} then 16-byte records {str_off, str_len, id, 0} sorted by id, UTF-8 text.
void LoadStrings() {
  g_strings_loaded = true;
  if (g_game.empty()) return;
  PacIndex idx;
  const fs::path file = fs::path(g_game) / L"pac" / L"string.pac";
  if (!ReadPacIndex(file, idx)) return;
  const PacEntryInfo* pick = nullptr;
  for (const auto& e : idx.entries)
    if (e.name == "6400") pick = &e;
  Bytes d;
  if (!pick || !ReadPacEntry(file, *pick, d)) return;
  std::vector<PachEntry> tables;
  if (!PachRead(d, tables)) return;
  for (const auto& t : tables) {
    const Bytes& b = t.data;  // (not BPE)
    if (b.size() < 8) continue;
    const uint32_t n = Le32(&b[4]);
    auto& m = g_strings[int(t.id)];
    for (uint32_t k = 0; k < n && 8 + 16 * size_t(k + 1) <= b.size(); ++k) {
      const uint8_t* r = &b[8 + 16 * k];
      const uint32_t off = Le32(r), len = Le32(r + 4), id = Le32(r + 8);
      if (off >= b.size()) continue;
      const char* s = reinterpret_cast<const char*>(&b[off]);
      m[int(id)] = std::string(s, strnlen(s, std::min<size_t>(len ? len : 1, b.size() - off)));
    }
  }
}
}  // namespace

const std::string& GameString(int id) {
  static const std::string none;
  if (!g_strings_loaded) LoadStrings();
  std::vector<int> order;
  if (id >= 50000) order = {1};
  else if (id >= 45000) order = {2};
  else if (id >= 40000) order = {0};
  else { for (int t = 3; t <= 13; ++t) order.push_back(t); order.push_back(0); }
  for (int t : order) {
    const auto tb = g_strings.find(t);
    if (tb == g_strings.end()) continue;
    const auto it = tb->second.find(id);
    if (it != tb->second.end()) return it->second;
  }
  return none;
}

namespace {
std::map<int, std::string> g_move_names;
bool g_move_names_loaded = false;
}  // namespace

const std::map<int, std::string>& MoveNames() {
  if (g_move_names_loaded) return g_move_names;
  g_move_names_loaded = true;
  if (g_game.empty()) return g_move_names;
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"misc.pac"), d) || !EpacRead(d, e)) return g_move_names;
  for (const auto& g : e.groups) {
    if (g.type != "MOVS") continue;
    for (const auto& en : g.entries) {
      if (en.name.rfind("WAZE", 0) != 0) continue;
      const Bytes raw = Unpack(en.data);
      // the records start 0x10 before "* test motion *" (the first record's name)
      const char* probe = "* test motion *";
      size_t at = std::string::npos;
      for (size_t i = 0; i + 16 < raw.size(); ++i)
        if (!std::memcmp(&raw[i], probe, 15)) { at = i - 0x10; break; }
      if (at == std::string::npos || raw.size() < 8) continue;
      const uint32_t count = Le32(&raw[4]);
      for (uint32_t k = 0; k < count && at + 160 * (k + 1) <= raw.size(); ++k) {
        const uint8_t* r = &raw[at + 160 * k];
        const int id = Le16(r + 0x90);
        const char* nm = reinterpret_cast<const char*>(r + 0x10);
        const std::string name(nm, strnlen(nm, 64));
        if (id && !name.empty() && !g_move_names.count(id)) g_move_names[id] = name;
      }
    }
  }
  return g_move_names;
}

namespace {
std::map<int, MoveInfo> g_move_infos;
std::map<int, std::vector<int>> g_profiles;  // star id -> 224 move ids
std::map<int, int> g_profile_music;           // star id -> its theme's music id (ProfileMusic)
struct SlotRule {
  uint8_t bits[16] = {};
  int situation = -1;  // (-1: any)
};
std::vector<SlotRule> g_slot_rules;
std::vector<std::vector<int>> g_slot_moves;
bool g_moveset_loaded = false;

void LoadMoveset() {
  if (g_moveset_loaded) return;
  g_moveset_loaded = true;
  if (g_game.empty()) return;
  Bytes d;
  Epac e;
  if (ReadFile(PathStr(fs::path(g_game) / L"pac" / L"misc.pac"), d) && EpacRead(d, e))
    for (const auto& g : e.groups)
      for (const auto& en : g.entries) {
        if (g.type != "MOVS" || en.name.rfind("WAZE", 0) != 0) continue;
        const Bytes raw = Unpack(en.data);
        size_t at = std::string::npos;
        for (size_t i = 0x10; i + 16 < raw.size(); ++i)
          if (!std::memcmp(&raw[i], "* test motion *", 15)) { at = i - 0x10; break; }
        if (at == std::string::npos) continue;
        for (size_t k = 0; at + 160 * (k + 1) <= raw.size(); ++k) {
          const uint8_t* r = &raw[at + 160 * k];
          MoveInfo m;
          m.id = Le16(r + 0x90);
          const char* nm = reinterpret_cast<const char*>(r + 0x10);
          m.name.assign(nm, strnlen(nm, 64));
          if (!m.id || m.name.empty() || g_move_infos.count(m.id)) continue;
          std::memcpy(m.bits, r, 16);
          m.situation = r[0x73];
          g_move_infos[m.id] = std::move(m);
        }
      }
  d.clear();
  e = Epac();
  std::map<int, std::string> names;
  if (ReadFile(PathStr(fs::path(g_game) / L"pac" / L"ch" / L"chEtc.pac"), d) && EpacRead(d, e))
    for (const auto& g : e.groups)
      for (const auto& en : g.entries) {
        if (g.type != "CHAR") continue;
        std::vector<PachEntry> pe;
        if (en.name.rfind("PRO", 0) == 0 && PachRead(Unpack(en.data), pe))
          for (const auto& x : pe) {
            const Bytes r = Unpack(x.data);
            if (r.size() < 2 * kMoveSlots) continue;
            std::vector<int> v(kMoveSlots);
            for (int s = 0; s < kMoveSlots; ++s) v[size_t(s)] = Le16(&r[size_t(s) * 2]);
            g_profiles[int(x.id)] = std::move(v);
            if (r.size() >= 0x1C0 + 0x12) g_profile_music[int(x.id)] = Le16(&r[0x1C0 + 0x10]);
          }
        if (en.name.rfind("DAT", 0) == 0 && PachRead(Unpack(en.data), pe))
          for (const auto& x : pe) {
            const Bytes r = Unpack(x.data);
            if (r.size() >= 4 + 70) names[int(x.id)] = std::string(reinterpret_cast<const char*>(&r[4 + 34]), strnlen(reinterpret_cast<const char*>(&r[4 + 34]), 32));
          }
      }
  // the real roster (named, not the motion tests)
  std::vector<const std::vector<int>*> real;
  for (const auto& [id, v] : g_profiles) {
    const auto n = names.find(id);
    if (id < 100 || id >= 9000 || n == names.end() || n->second.empty() || n->second == "0" || Lower(n->second).rfind("motion", 0) == 0) continue;
    real.push_back(&v);
  }
  g_slot_rules.assign(kMoveSlots, SlotRule());
  g_slot_moves.assign(kMoveSlots, {});
  for (int s = 0; s < kMoveSlots; ++s) {
    int count[128] = {}, n = 0;
    std::map<int, int> sits;
    for (const auto* v : real) {
      const auto it = g_move_infos.find((*v)[size_t(s)]);
      if (it == g_move_infos.end()) continue;
      ++n;
      for (int b = 0; b < 128; ++b)
        if (it->second.bits[b / 8] >> (b % 8) & 1) ++count[b];
      ++sits[it->second.situation];
    }
    SlotRule& rule = g_slot_rules[size_t(s)];
    for (int b = 0; b < 128 && n; ++b)
      if (count[b] * 10 >= n * 9 && b != 121 && b != 124 && b != 127) rule.bits[b / 8] |= uint8_t(1 << (b % 8));
    for (const auto& [sit, c] : sits)
      if ((sit == 13 || sit == 14) && c * 10 >= n * 9) rule.situation = sit;  // (opponent seated on the top turnbuckle)
  }
  for (const auto& [id, m] : g_move_infos)
    for (int s = 0; s < kMoveSlots; ++s)
      if (MoveFitsSlot(s, id)) g_slot_moves[size_t(s)].push_back(id);
  for (auto& v : g_slot_moves)
    std::sort(v.begin(), v.end(), [](int a, int b) { return g_move_infos[a].name < g_move_infos[b].name; });
  Log("moveset: " + std::to_string(g_move_infos.size()) + " moves, " + std::to_string(real.size()) + " roster profiles");
}
}  // namespace

namespace {
std::vector<EntranceInfo> g_entrances, g_entrances_next;
std::atomic<int> g_entrances_state{0};  // 0 not read, 1 reading, 2 read, 3 ready

std::string EvpName(const Bytes& stored) {
  const Bytes b = Unpack(stored);
  if (b.size() < 0x42) return "";
  std::string n(reinterpret_cast<const char*>(&b[0x22]), strnlen(reinterpret_cast<const char*>(&b[0x22]), 32));
  while (!n.empty() && n.back() == ' ') n.pop_back();
  return n;
}
}  // namespace

std::vector<EntranceInfo> PackEntrances(const fs::path& pack) {
  std::vector<EntranceInfo> out;
  Bytes t;
  if (!ReadFile(PathStr(pack / L"pack.txt"), t)) return out;
  const std::string s(t.begin(), t.end());
  for (size_t at = 0; at < s.size();) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    std::string line = s.substr(at, e - at);
    at = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    char pac[260], group[16], name[64], file[260];
    if (std::sscanf(line.c_str(), "pacentry %259s %15s %63s %259s", pac, group, name, file) != 4) continue;
    if (std::strncmp(group, "EVP", 3) || !std::isdigit(uint8_t(name[0]))) continue;
    EntranceInfo i;
    i.number = std::atoi(name);
    Bytes f;
    if (ReadFile(PathStr(pack / Wide(file)), f)) i.name = EvpName(f);
    i.from = PathStr(pack.filename());
    out.push_back(std::move(i));
  }
  return out;
}

void ReloadEntrances() {
  int expect = g_entrances_state.load();
  if (expect == 1 || !g_entrances_state.compare_exchange_strong(expect, 1)) return;
  std::thread([game = g_game] {
    std::vector<EntranceInfo> out;
    std::set<int> seen;
    std::error_code ec;
    std::vector<fs::path> pacs;
    for (const auto& e : fs::directory_iterator(fs::path(game) / L"pac" / L"evt", ec))
      if (Lower(PathStr(e.path().filename())).rfind("nyujyo", 0) == 0) pacs.push_back(e.path());
    std::sort(pacs.begin(), pacs.end());
    for (const auto& p : pacs) {
      PacIndex idx;
      if (!ReadPacIndex(p, idx)) continue;
      for (const auto& e : idx.entries) {
        if (e.group.rfind("EVP", 0) != 0 || e.name.empty() || !std::isdigit(uint8_t(e.name[0]))) continue;
        const int n = std::atoi(e.name.c_str());
        if (n <= 0 || n >= 1000 || !seen.insert(n).second) continue;
        Bytes b;
        EntranceInfo i;
        i.number = n;
        if (ReadPacEntry(p, e, b)) i.name = EvpName(b);
        out.push_back(std::move(i));
      }
    }
    for (const wchar_t* where : {L"Moves", L"Superstars"})
      for (const auto& e : fs::directory_iterator(fs::path(game) / L"Mods" / where, ec)) {
        if (!e.is_directory(ec) || fs::exists(e.path() / L"disabled", ec)) continue;
        const fs::path pack = std::wstring(where) == L"Moves" ? e.path() : e.path() / L"moves";
        for (auto& i : PackEntrances(pack))
          if (!IsVictoryNumber(i.number) && seen.insert(i.number).second) out.push_back(std::move(i));
      }
    std::sort(out.begin(), out.end(), [](const EntranceInfo& a, const EntranceInfo& b) { return a.number < b.number; });
    g_entrances_next = std::move(out);
    g_entrances_state = 2;
  }).detach();
}

bool EntrancesReady() {
  if (g_entrances_state == 0) ReloadEntrances();
  if (g_entrances_state == 2) {
    g_entrances = std::move(g_entrances_next);
    g_entrances_next.clear();
    g_entrances_state = 3;
    Log("entrances: " + std::to_string(g_entrances.size()) + " installed");
  }
  return g_entrances_state == 3;
}

const std::vector<EntranceInfo>& Entrances() {
  static const std::vector<EntranceInfo> kNone;
  return EntrancesReady() ? g_entrances : kNone;
}

const std::vector<EntranceInfo>& Victories() {
  static std::vector<EntranceInfo> v;
  static bool done = false;
  if (done) return v;
  done = true;
  std::set<int> seen;
  std::error_code ec;
  std::vector<fs::path> pacs;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"pac" / L"evt", ec))
    if (Lower(PathStr(e.path().filename())).rfind("winning", 0) == 0) pacs.push_back(e.path());
  std::sort(pacs.begin(), pacs.end());
  for (const auto& p : pacs) {
    PacIndex idx;
    if (!ReadPacIndex(p, idx)) continue;
    for (const auto& e : idx.entries) {
      if (e.group.rfind("EVP", 0) != 0 || e.name.empty() || !std::isdigit(uint8_t(e.name[0]))) continue;
      const int n = std::atoi(e.name.c_str());
      if (n < 6000 || n > 7107 || !seen.insert(n).second) continue;  // (singles: tag, trio, manager scenes are 7150+)
      Bytes b;
      EntranceInfo i;
      i.number = n;
      if (ReadPacEntry(p, e, b)) i.name = EvpName(b);
      v.push_back(std::move(i));
    }
  }
  for (const wchar_t* where : {L"Moves", L"Superstars"})
    for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / where, ec)) {
      if (!e.is_directory(ec) || fs::exists(e.path() / L"disabled", ec)) continue;
      for (auto& i : PackEntrances(std::wstring(where) == L"Moves" ? e.path() : e.path() / L"moves"))
        if (IsVictoryNumber(i.number) && seen.insert(i.number).second) v.push_back(std::move(i));
    }
  std::sort(v.begin(), v.end(), [](const EntranceInfo& a, const EntranceInfo& b) { return a.number < b.number; });
  return v;
}

const std::map<int, MoveInfo>& MoveInfos() {
  LoadMoveset();
  return g_move_infos;
}

int ProfileMusic(int star_id) {
  LoadMoveset();
  const auto it = g_profile_music.find(star_id);
  return it == g_profile_music.end() ? 0 : it->second;
}

std::vector<int> ProfileMoves(int star_id) {
  LoadMoveset();
  const auto it = g_profiles.find(star_id);
  return it == g_profiles.end() ? std::vector<int>() : it->second;
}

bool MoveFitsSlot(int slot, int move_id) {
  LoadMoveset();
  if (slot < 0 || slot >= kMoveSlots || g_slot_rules.empty()) return false;
  const auto it = g_move_infos.find(move_id);
  if (it == g_move_infos.end()) return false;
  const SlotRule& r = g_slot_rules[size_t(slot)];
  for (int i = 0; i < 16; ++i)
    if ((it->second.bits[i] & r.bits[i]) != r.bits[i]) return false;
  return r.situation < 0 || it->second.situation == r.situation;
}

const std::vector<int>& MovesForSlot(int slot) {
  static const std::vector<int> kNone;
  LoadMoveset();
  return slot >= 0 && slot < int(g_slot_moves.size()) ? g_slot_moves[size_t(slot)] : kNone;
}

const std::string& MoveName(int id) {
  static const std::string none;
  const auto& m = MoveNames();
  const auto it = m.find(id);
  return it == m.end() ? none : it->second;
}

// A game running from this game folder (another copy's test game doesn't count).
bool GameRunning() {
  if (g_game.empty()) return false;
  static std::wstring last_game;
  static bool last_result = false;
  static double last_time = -10;
  const double now = ImGui::GetTime();
  if (last_game == g_game && now - last_time < 1.0) return last_result;  // (asked every frame)
  last_game = g_game, last_time = now;
  last_result = false;
  const std::wstring want = Lower(PathStr(fs::path(g_game) / L"svr2011.exe")) == "" ? L"" : (fs::path(g_game) / L"svr2011.exe").wstring();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe = {sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !last_result; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, L"svr2011.exe")) continue;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
      wchar_t path[MAX_PATH * 2];
      DWORD n = DWORD(std::size(path));
      if (QueryFullProcessImageNameW(h, 0, path, &n) && !_wcsicmp(path, want.c_str())) last_result = true;
      CloseHandle(h);
    }
  }
  CloseHandle(snap);
  return last_result;
}

void StartGame(const std::wstring& extra_env) {
  const std::wstring exe = (fs::path(g_game) / L"svr2011.exe").wstring();
  // "NAME=value;NAME=value": set for the child only
  std::vector<std::wstring> set;
  for (size_t at = 0; at < extra_env.size();) {
    size_t end = extra_env.find(L';', at);
    if (end == std::wstring::npos) end = extra_env.size();
    const std::wstring kv = extra_env.substr(at, end - at);
    const size_t eq = kv.find(L'=');
    if (eq != std::wstring::npos) {
      SetEnvironmentVariableW(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
      set.push_back(kv.substr(0, eq));
    }
    at = end + 1;
  }
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + exe + L"\"";
  if (g_behind) {  // a test: the game behind everything, muted, its log where the test reads it
    SetEnvironmentVariableW(L"SVR2011_WINDOW_BEHIND", L"1");
    set.push_back(L"SVR2011_WINDOW_BEHIND");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    cmd += L" --monitor=2 --audio_mute=true --fullscreen=false --log_file=\"" + std::wstring(tmp) + L"svr2011_modmaker_play.log\"";
  }
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, g_behind ? BELOW_NORMAL_PRIORITY_CLASS : 0, nullptr,
                     g_game.c_str(), &si, &pi)) {
    Log("game started: pid " + std::to_string(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Status("The game is starting.");
  } else {
    Status("Could not start " + Utf8(exe) + ".");
  }
  for (const auto& n : set) SetEnvironmentVariableW(n.c_str(), nullptr);
}

// ---------------------------------------------------------------- widgets

bool FileRow(const char* label, std::wstring& file, const char* none, const COMDLG_FILTERSPEC* spec, UINT nspec,
             const char* tip, float width) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::Button(label, ImVec2(width * g_scale, 0))) {
    const std::wstring p = PickFile(false, Wide(label).c_str(), spec, nspec);
    if (!p.empty()) file = p, changed = true;
  }
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  ImGui::SameLine();
  if (file.empty()) ImGui::TextDisabled("%s", none);
  else ImGui::TextUnformatted(FileName(file).c_str());
  if (!file.empty()) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Utf8(file).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) file.clear(), changed = true;
  }
  ImGui::PopID();
  return changed;
}

bool TextField(const char* label, char* buf, size_t size, const char* hint, float width) {
  ImGui::SetNextItemWidth(width * g_scale);
  return hint ? ImGui::InputTextWithHint(label, hint, buf, size) : ImGui::InputText(label, buf, size);
}

void Hint(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(420 * g_scale);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void Heading(const char* title, const char* about) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.35f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (about) ImGui::TextDisabled("%s", about);
  ImGui::Spacing();
}

void PushAccent() {
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.25f, 1));
}
void PopAccent() { ImGui::PopStyleColor(2); }

Image FitFigure(const Image& src, int size) {
  // the figure: alpha > 32, when the picture has a transparent background
  int l = src.w, t = src.h, r = -1, b = -1;
  bool cut = false;
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) {
      if (src.rgba[(size_t(y) * src.w + x) * 4 + 3] <= 32) { cut = true; continue; }
      l = std::min(l, x), r = std::max(r, x), t = std::min(t, y), b = std::max(b, y);
    }
  if (!cut || r < 0) l = 0, t = 0, r = src.w - 1, b = src.h - 1;
  const int cw = r - l + 1, ch = b - t + 1;
  Image crop;
  crop.w = cw, crop.h = ch;
  crop.rgba.resize(size_t(cw) * ch * 4);
  for (int y = 0; y < ch; ++y)
    std::memcpy(&crop.rgba[size_t(y) * cw * 4], &src.rgba[(size_t(t + y) * src.w + l) * 4], size_t(cw) * 4);
  // a cut-out fills the height (no wider than 96%: then the width decides and it stands on the bottom);
  // a full picture fits whole, top aligned
  const float k = cut ? std::min(float(size) / ch, size * 0.96f / cw) : std::min(float(size) / cw, float(size) / ch);
  const int w = std::clamp(int(cw * k + 0.5f), 1, size), h = std::clamp(int(ch * k + 0.5f), 1, size);
  const Image fit = Resize(crop, w, h);
  Image out;
  out.w = out.h = size;
  out.rgba.assign(size_t(size) * size * 4, 0);
  const int x0 = (size - w) / 2, y0 = cut ? size - h : 0;
  for (int y = 0; y < h; ++y)
    std::memcpy(&out.rgba[(size_t(y0 + y) * size + x0) * 4], &fit.rgba[size_t(y) * w * 4], size_t(w) * 4);
  return out;
}

Image FitLoading(const Image& src) {
  constexpr int W = 1024, H = 512;
  if (!src.w || !src.h) return Image();
  if (src.w == W && src.h == H) return src;
  const float aspect = float(src.w) / float(src.h);
  if (aspect > 1.8f && aspect < 2.2f) return Resize(src, W, H);  // (wide: it fills the screen)
  // the back: the picture covering the whole screen, blurred (down to 32 x 16 and up again) and darkened
  const float cover = std::max(float(W) / src.w, float(H) / src.h);
  const int cw = std::max(1, int(src.w * cover + 0.5f)), chh = std::max(1, int(src.h * cover + 0.5f));
  const Image covered = Resize(src, cw, chh);
  Image crop;
  crop.w = W, crop.h = H;
  crop.rgba.resize(size_t(W) * H * 4);
  for (int y = 0; y < H; ++y)
    std::memcpy(&crop.rgba[size_t(y) * W * 4], &covered.rgba[(size_t(y + (chh - H) / 2) * cw + (cw - W) / 2) * 4], size_t(W) * 4);
  Image out = Resize(Resize(Resize(crop, 128, 64), 32, 16), W, H);
  for (size_t i = 0; i < out.rgba.size(); i += 4) {
    for (int c = 0; c < 3; ++c) out.rgba[i + c] = uint8_t(out.rgba[i + c] * 0.55f);
    out.rgba[i + 3] = 255;
  }
  // the picture itself, whole: on the right half when it is square or tall (as the converters place a render), else centred
  const float fit = std::min(float(W) / src.w, float(H) / src.h);
  const int fw = std::max(1, int(src.w * fit + 0.5f)), fh = std::max(1, int(src.h * fit + 0.5f));
  const Image front = Resize(src, fw, fh);
  const int x0 = fw <= W / 2 + 64 ? std::min(W - fw, W * 3 / 4 - fw / 2) : (W - fw) / 2, y0 = (H - fh) / 2;
  for (int y = 0; y < fh; ++y)
    for (int x = 0; x < fw; ++x) {
      const uint8_t* s = &front.rgba[(size_t(y) * fw + x) * 4];
      uint8_t* d = &out.rgba[(size_t(y0 + y) * W + x0 + x) * 4];
      const float a = s[3] / 255.0f;
      for (int c = 0; c < 3; ++c) d[c] = uint8_t(s[c] * a + d[c] * (1 - a) + 0.5f);
    }
  return out;
}

Image GameLook(const Image& src) {
  // the figure's luminance mean / spread and saturation, moved half way to the stock renders' medians
  double sum = 0, sum2 = 0, sat = 0, n = 0;
  for (size_t i = 0; i + 3 < src.rgba.size(); i += 4) {
    if (src.rgba[i + 3] < 128) continue;
    const double r = src.rgba[i] / 255.0, g = src.rgba[i + 1] / 255.0, b = src.rgba[i + 2] / 255.0;
    const double lum = 0.299 * r + 0.587 * g + 0.114 * b, mx = std::max({r, g, b}), mn = std::min({r, g, b});
    sum += lum, sum2 += lum * lum, sat += mx > 1e-3 ? (mx - mn) / mx : 0, n += 1;
  }
  if (n < 100) return src;
  const double mean = sum / n, sd = std::sqrt(std::max(1e-6, sum2 / n - mean * mean)), s = sat / n;
  const double want_mean = (mean + 0.386) / 2, want_sd = (sd + 0.214) / 2, want_s = (s + 0.412) / 2;
  const double gain = std::clamp(want_sd / sd, 0.5, 2.0), sgain = s > 1e-3 ? std::clamp(want_s / s, 0.5, 2.0) : 1.0;
  Image out = src;
  for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
    const double r = src.rgba[i] / 255.0, g = src.rgba[i + 1] / 255.0, b = src.rgba[i + 2] / 255.0;
    const double lum = 0.299 * r + 0.587 * g + 0.114 * b, nl = (lum - mean) * gain + want_mean;
    for (int c = 0; c < 3; ++c) {
      const double v = nl + ((c == 0 ? r : c == 1 ? g : b) - lum) * sgain;
      out.rgba[i + c] = uint8_t(std::clamp(v, 0.0, 1.0) * 255 + 0.5);
    }
  }
  return out;
}

std::map<std::wstring, std::vector<ModelPages>> g_gear_budget;  // (ModelBudget's kind-4 entries)
const std::vector<ModelPages>& ModelBudget(const std::wstring& pac) {
  struct Cached {
    fs::file_time_type time;
    uintmax_t size = 0;
    std::vector<ModelPages> pages, gear;
  };
  static std::map<std::wstring, Cached> cache;
  static const std::vector<ModelPages> none;
  std::error_code ec;
  const auto t = fs::last_write_time(pac, ec);
  if (ec) return none;
  const uintmax_t size = fs::file_size(pac, ec);
  auto it = cache.find(pac);
  if (it != cache.end() && it->second.time == t && it->second.size == size) return it->second.pages;
  Cached c{t, size, {}};
  Bytes d;
  Epac e;
  if (ReadFile(Utf8(pac), d) && EpacRead(d, e))
    for (const auto& g : e.groups) {
      if (g.type.rfind("EMD", 0) != 0) continue;
      for (const auto& en : g.entries) {
        if (en.name.empty() || (en.name.back() != '2' && en.name.back() != '4')) continue;  // (kind 2: the wrestler's model; 4: entrance gear)
        std::vector<PachEntry> kids;
        if (!PachRead(Unpack(en.data), kids)) continue;
        int pages = 0, blood = 0;
        for (const auto& k : kids) {
          const Bytes raw = Unpack(k.data);
          Model m;
          if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
          int mp = 0;
          for (const auto& s : m.meshes) {
            const uint32_t blocks = s.raw.size() >= 0x60 ? Be32(&s.raw[0x5C]) : 1;
            size_t idx = 0;
            for (const auto& st : s.strips) idx += st.indices.size();
            mp += int((s.verts.size() * (std::max(1u, blocks) == 1 ? 32 : 44) + 4095) / 4096 + (idx * 2 + 4095) / 4096);
          }
          pages += mp;
          if (k.id >= 0x7530 && k.id <= 0x7532) blood += mp;
        }
        (en.name.back() == '4' ? c.gear : c.pages).push_back({en.name, pages, blood});
      }
    }
  std::string line = "model budget: " + Utf8(fs::path(pac).filename().wstring());
  for (const auto& p : c.pages) line += "  " + p.entry + " " + std::to_string(p.pages);
  Log(line);
  g_gear_budget[pac] = c.gear;
  return (cache[pac] = std::move(c)).pages;
}

const std::vector<ModelPages>& GearBudget(const std::wstring& pac) {
  ModelBudget(pac);  // (fills both)
  static const std::vector<ModelPages> none;
  const auto it = g_gear_budget.find(pac);
  return it == g_gear_budget.end() ? none : it->second;
}

void ExportModelFbx(const std::wstring& pac, int attire, const std::string& name) {
  const std::wstring dir = PickFolder(L"A folder for the FBX (the textures go in a textures folder beside it)");
  if (dir.empty()) return;
  const std::string id = IdFrom(name.empty() ? "model" : name, "model") + (attire > 1 ? "_attire" + std::to_string(attire) : "");
  RunInBackground([pac, attire, dir, id] {
    Progress("Writing the FBX ...");
    Bytes b;
    svrfmt::CharExportReport rep;
    const bool ok = ReadFile(Utf8(pac), b) && svrfmt::ExportCharacterFbx(b, attire, Utf8(dir), id, rep);
    for (const auto& w : rep.warnings) Log("  " + w);
    Status(ok ? "Exported " + id + ".fbx: " + std::to_string(rep.bones) + " bones, " + std::to_string(rep.meshes) + " meshes, " +
                    std::to_string(rep.vertices) + " vertices, " + std::to_string(rep.textures) + " textures (" + Utf8(dir) + ")."
              : "The FBX could not be written: " + (rep.warnings.empty() ? std::string("see the log") : rep.warnings.front()));
  });
}

std::vector<int> ModelAttires(const std::wstring& pac) {
  std::vector<int> v;
  for (const auto& p : ModelBudget(pac))
    if (p.entry.size() >= 8 && std::isdigit(uint8_t(p.entry[6]))) v.push_back(p.entry[6] - '0' + 1);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void ModelCheckProblems(const std::wstring& pac, const std::string& who, std::vector<Problem>& out) {
  struct Cached {
    fs::file_time_type time;
    uintmax_t size = 0;
    svrfmt::CharModelIssues issues;
  };
  static std::map<std::wstring, Cached> cache;
  std::error_code ec;
  const auto t = fs::last_write_time(pac, ec);
  const uintmax_t size = fs::file_size(pac, ec);
  if (ec) return;
  auto it = cache.find(pac);
  if (it == cache.end() || it->second.time != t || it->second.size != size) {
    Cached c{t, size, {}};
    Bytes b;
    if (ReadFile(Utf8(pac), b)) c.issues = svrfmt::CheckCharacterModel(b, 1);
    it = cache.insert_or_assign(pac, std::move(c)).first;
  }
  const auto& is = it->second.issues;
  for (const auto& c : is.attire_clash)
    out.push_back(Error(who + "'s model pac names two outfits the same (" + c + "): the second one never shows, and a match "
                        "asked for it hangs on NOW LOADING.",
                        "Port the superstar again with this Mod Maker (WWE 2K16 / 2K17 ports made before 2.1.2 have this)."));
  for (const auto& h : is.hair)
    if (h.find("room for") != std::string::npos)  // (proven: Asuka 2K17 crashed / hung at the match load)
      out.push_back(Error(who + "'s models are too big for moving hair - " + h + ": the game crashes or hangs when the match loads.",
                          "Port the superstar again with this Mod Maker (it leaves the blood overlays out then), or remove the "
                          "blood overlays (entries 0x7530 - 0x7532) in the pac editor."));
    else
      out.push_back(Warning(who + "'s hair settings are damaged - " + h + " (a converter defect of 14 Oct).",
                            "Port the superstar again with this Mod Maker: it writes each hair chain once."));
  for (const auto& m : is.nan_meshes)
    out.push_back(Error(who + "'s model is damaged (part " + m + " has broken points): the game would crash when it loads it.",
                        "Export the model again from your 3D program, or make it again from the FBX."));
  for (int e = 0; e < 2; ++e)
    if (is.eye_off[e] > 0.15) {
      char d[16];
      std::snprintf(d, sizeof d, "%.1f", is.eye_off[e] * 10);
      out.push_back(Warning(who + "'s " + (e ? "right" : "left") + " eye sits " + d + " cm away from where the game moves it: "
                            "it pops out of the face when they blink.", "Making the model from the FBX puts the eyes right."));
    }
}

void BudgetProblems(const std::wstring& pac, const std::string& who, bool first_only, std::vector<Problem>& out,
                    std::function<void(const std::wstring&)> use) {
  const ModelPages* worst = nullptr;
  for (const auto& p : ModelBudget(pac)) {
    if (!worst || p.pages > worst->pages) worst = &p;
    if (first_only) break;
  }
  if (worst && worst->pages > kModelPagesOld && worst->pages <= kModelPagesWarn)
    out.push_back(Warning(who + "'s model takes " + std::to_string(worst->pages) + " memory pages: version " + kNeedsVersion +
                       " or newer plays it (older games had " + std::to_string(kModelPagesOld) + " and leave it out).", ""));
  // (27 Oct: six 1,489-page superstars in one 6-man - 60 fps at 1080p, about 40 at 4x supersampling with 2x AA)
  if (worst && worst->pages > kModelPagesHeavy)
    out.push_back(Warning(who + "'s model is very detailed (" + std::to_string(worst->pages) + " memory pages). Fine on its "
                          "own; six models this detailed in one match play at 60 fps at 1080p, about 40 fps at the "
                          "highest render scale with anti-aliasing.").Note());
  if (!worst || worst->pages <= kModelPagesWarn) return;
  const int pct = worst->pages * 100 / kModelPages;
  // a lighter copy: the model without its blood, used in its place
  auto lighter = [pac, use] {
    Bytes in, out;
    std::string err;
    if (!ReadFile(Utf8(pac), in) || !StripBlood(in, out, &err)) {
      Status("A lighter copy couldn't be made: " + (err.empty() ? std::string("the model didn't read") : err) + ".");
      return;
    }
    const fs::path dir = fs::path(g_game) / L"Mods" / L".convert" / L"lighter";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path to = dir / (fs::path(pac).stem().wstring() + L"_noblood.pac");
    if (!WriteFile(PathStr(to), out)) {
      Status("A lighter copy couldn't be written into " + PathStr(dir) + ".");
      return;
    }
    use(to.wstring());
  };
  // (as the game: over the limit it drops the blood overlays, then leaves out a model still over)
  if (worst->pages - worst->blood > kModelPages) {
    out.push_back(Error(who + "'s model is too big for the game's memory (" + std::to_string(pct) + "% of what a wrestler gets): "
                        "the game would leave " + who + " out of matches.",
                        "Use a model with fewer polygons, or one made from an FBX (the builder keeps to the limit)."));
  } else if (worst->pages > kModelPages) {
    Problem p = Warning(who + "'s model is a little too big for the game's memory: to fit, the game shows no blood on " + who +
                        " in matches.");
    if (use) p = std::move(p).With("Make a lighter copy without the blood", lighter, "a lighter copy of the model (without blood) is used");
    out.push_back(std::move(p));
  } else {
    out.push_back(Warning(who + "'s model is close to the game's memory limit (" + std::to_string(pct) + "%): fine, but it can't get any bigger.").Note());
  }
}

void MakeRenders(const Image& src, Image& render, Image& bust, Image& icon) {
  // framed the game's way on 512 x 512 (FitFigure); the bust (SSFC): that at
  // 3/4 size, its top 256 rows around the figure's middle; the face icon:
  // the bust's top middle at 64 x 64
  Image big = FitFigure(src, 512);
  const Image three = Resize(big, 384, 384);
  double sx = 0, sa = 0;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 384; ++x) {
      const double a = three.rgba[(size_t(y) * 384 + x) * 4 + 3];
      sx += a * x, sa += a;
    }
  const int cx = sa > 0 ? int(sx / sa) : 192;
  const int left = std::clamp(cx - 128, 0, 384 - 256);
  bust.w = bust.h = 256;
  bust.rgba.resize(size_t(256) * 256 * 4);
  for (int y = 0; y < 256; ++y)
    std::memcpy(&bust.rgba[size_t(y) * 256 * 4], &three.rgba[(size_t(y) * 384 + left) * 4], 256 * 4);
  Image head;
  head.w = head.h = 128;
  head.rgba.resize(size_t(128) * 128 * 4);
  for (int y = 0; y < 128; ++y)
    std::memcpy(&head.rgba[size_t(y) * 128 * 4], &bust.rgba[(size_t(y) * 256 + 64) * 4], 128 * 4);
  icon = Resize(head, 64, 64);
  render = std::move(big);
}

void MakeBink(const std::wstring& video, const std::wstring& out, int fit, int seconds, std::function<void(std::wstring)> done) {
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return;
  }
  RunInBackground([video, out, fit, seconds, done] {
    Progress("Making the Bink movie (" + FileName(video) + ") ...");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MovieJob job = {};
    wcsncpy_s(job.video, video.c_str(), _TRUNCATE);
    wcsncpy_s(job.out, out.c_str(), _TRUNCATE);
    job.fit = fit;
    job.max_seconds = seconds;
    const bool ok = movie_make(&job) != 0;
    CoUninitialize();
    const std::wstring err = job.err;
    const int frames = job.frames;
    // then an .mp4 of it (the size mods carry; the .bik stays beside it as the game's fallback)
    std::wstring mp4;
    std::string why;
    if (ok) {
      Progress("Making the mp4 (" + FileName(out) + ") ...");
      mp4 = fs::path(out).replace_extension(L".mp4").wstring();
      why = ShrinkMovie(out, mp4);
      if (!why.empty()) mp4.clear();
    }
    OnUiThread([ok, out, mp4, why, err, frames, done] {
      std::error_code ec;
      if (ok && !mp4.empty())
        Status("Movie made: " + FileName(mp4) + " (" + std::to_string(frames) + " frames, " + SizeText(fs::file_size(mp4, ec)) + "; as a Bink it's " +
               SizeText(fs::file_size(out, ec)) + ").");
      else if (ok) Status("Bink movie made: " + FileName(out) + " (" + std::to_string(frames) + " frames); no mp4: " + why + ".");
      else Status("The movie could not be made: " + Utf8(err));
      if (done) done(!ok ? L"" : !mp4.empty() ? mp4 : out);
    });
  });
}

std::string SizeText(uint64_t bytes) {
  char b[32];
  if (bytes >= 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
  else std::snprintf(b, sizeof b, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
  return b;
}

namespace {
std::mutex g_shrink_mutex;  // (one shrink at a time: the slot's background one and a save's wait for the same file)
fs::path Mp4Path(const std::wstring& bik) {
  std::error_code ec;
  const auto size = fs::file_size(bik, ec);
  const auto when = fs::last_write_time(bik, ec).time_since_epoch().count();
  uint32_t h = 2166136261u;
  for (wchar_t c : Lower(Utf8(bik)) + "|" + std::to_string(size) + "|" + std::to_string(when)) h = (h ^ uint32_t(uint8_t(c))) * 16777619u;
  char hex[16];
  std::snprintf(hex, sizeof hex, "%08x", h);
  return fs::path(g_game) / L"Mods" / L".convert" / L"movies" / (fs::path(bik).stem().wstring() + L"_" + Wide(hex) + L".mp4");
}
}  // namespace

std::wstring CachedMp4(const std::wstring& bik) {
  std::error_code ec;
  const fs::path p = Mp4Path(bik);
  return fs::exists(p, ec) ? p.wstring() : std::wstring();
}

std::wstring ShrunkMovie(const std::wstring& bik, std::string* why) {
  std::lock_guard lock(g_shrink_mutex);
  if (std::wstring have = CachedMp4(bik); !have.empty()) return have;
  const fs::path p = Mp4Path(bik);
  const std::string err = ShrinkMovie(bik, p.wstring());
  if (why) *why = err;
  return err.empty() ? p.wstring() : std::wstring();
}

std::string ModMovieFiles(const std::wstring& src, const std::string& stem, std::vector<ZipEntry>& files, std::string* why) {
  std::error_code ec;
  const std::string ext = Lower(Utf8(fs::path(src).extension().wstring()));
  auto is_bik = [](const std::wstring& f) {
    if (FILE* fp = _wfopen(f.c_str(), L"rb")) {
      char h[3] = {};
      const bool b = std::fread(h, 1, 3, fp) == 3 && !std::memcmp(h, "BIK", 3);
      std::fclose(fp);
      return b;
    }
    return false;
  };
  std::wstring bik, mp4;
  if (ext == ".mp4") {
    mp4 = src;
    const std::wstring beside = fs::path(src).replace_extension(L".bik").wstring();  // (a movie made here: its Bink beside it)
    if (fs::exists(beside, ec) && is_bik(beside)) bik = beside;
  } else {
    if (is_bik(src)) bik = src;
    Progress("Making the entrance movie an mp4 (" + FileName(src) + ") ...");
    std::string err;
    mp4 = ShrunkMovie(src, &err);
    if (mp4.empty()) Log("movie: kept as it is (" + err + ")");
  }
  Bytes d;
  if (!mp4.empty() && ReadFile(Utf8(mp4), d)) {
    files.push_back({stem + ".mp4", std::move(d)});
    Log("movie: " + stem + ".mp4 " + SizeText(files.back().data.size()) + (bik.empty() ? "" : ", the Bink " + SizeText(fs::file_size(bik, ec))) +
        (g_settings.keep_bik && !bik.empty() ? " (kept beside it)" : ""));
    if (g_settings.keep_bik && !bik.empty() && ReadFile(Utf8(bik), d)) files.push_back({stem + ".bik", std::move(d)});
    return stem + ".mp4";
  }
  if (!bik.empty() && ReadFile(Utf8(bik), d)) {  // (no mp4 could be made: the Bink as before)
    files.push_back({stem + ".bik", std::move(d)});
    return stem + ".bik";
  }
  if (why) *why = ext == ".bk2" || ext == ".bik2" ? "a Bink 2 movie that couldn't be converted" : "not a movie the game can play (a .bik or an .mp4)";
  return {};
}

std::wstring ProjectMovie(const std::wstring& movie) {
  if (movie.empty() || Lower(Utf8(fs::path(movie).extension().wstring())) == ".mp4") return movie;
  const std::wstring mp4 = CachedMp4(movie);
  return mp4.empty() ? movie : mp4;
}

void MovieSizeRow(const std::wstring& movie) {
  if (movie.empty()) return;
  static std::atomic<bool> running{false};
  static std::wstring failed_for;
  static std::string failed_why;
  std::error_code ec;
  const uint64_t size = fs::file_size(movie, ec);
  if (ec) return;
  const std::string ext = Lower(Utf8(fs::path(movie).extension().wstring()));
  ImGui::Indent();
  if (ext == ".mp4") {
    const std::wstring beside = fs::path(movie).replace_extension(L".bik").wstring();
    if (fs::exists(beside, ec))
      ImGui::TextDisabled("%s as an mp4 (as a Bink: %s)", SizeText(size).c_str(), SizeText(fs::file_size(beside, ec)).c_str());
    else
      ImGui::TextDisabled("%s (an mp4)", SizeText(size).c_str());
  } else {
    const std::wstring mp4 = CachedMp4(movie);
    if (!mp4.empty()) {
      ImGui::TextDisabled("%s as a Bink -> %s as an mp4 (the mod carries the mp4)", SizeText(size).c_str(), SizeText(fs::file_size(mp4, ec)).c_str());
    } else if (failed_for == movie) {
      ImGui::TextDisabled("%s as a Bink; no mp4: %s (the mod carries the Bink)", SizeText(size).c_str(), failed_why.c_str());
    } else {
      ImGui::TextDisabled("%s as a Bink -> making the mp4 ...", SizeText(size).c_str());
      if (!running.exchange(true))
        std::thread([movie] {
          std::string why;
          const std::wstring made = ShrunkMovie(movie, &why);
          OnUiThread([movie, made, why] {
            if (made.empty()) failed_for = movie, failed_why = why;
            running = false;
          });
        }).detach();
    }
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Keep the Bink too", &g_settings.keep_bik)) SaveSettings();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Mods carry the movie as an mp4 (much smaller). Ticked, they carry the Bink (.bik) beside it too: the game plays\n"
                      "the Bink where Windows can't play mp4 (Steam Deck / Proton, Windows N). Bigger mods; for advanced users.");
  ImGui::Unindent();
}

int ArenaPacNumber(const std::string& name) {  // (the last "bg" followed by exactly two digits)
  const std::string l = Lower(name);
  int found = -1;
  for (size_t at = 0; at + 4 <= l.size(); ++at)
    if (l.compare(at, 2, "bg") == 0 && std::isdigit(uint8_t(l[at + 2])) && std::isdigit(uint8_t(l[at + 3])) &&
        (at + 4 == l.size() || !std::isdigit(uint8_t(l[at + 4]))))
      found = (l[at + 2] - '0') * 10 + (l[at + 3] - '0');
  return found;
}

void ViewFooter() {
  ImGui::PushID("viewfooter");
  // (on the same line while a checkbox labelled `label` still fits there, else on the next)
  auto next_fits = [](const char* label) {
    const float need = ImGui::GetStyle().ItemSpacing.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + need <= right) ImGui::SameLine();
  };
  if (ImGui::SmallButton("Reset view")) char_preview::ResetCamera();
  ImGui::SameLine();
  const bool has = char_preview::HasBlood();
  ImGui::BeginDisabled(!has);
  bool blood = char_preview::Blood() && has;
  if (ImGui::Checkbox("Blood", &blood)) char_preview::SetBlood(blood);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(has ? "The blood a superstar gets in a match, on the face and body." : "This model has no blood layers.");
  next_fits("Physics");
  bool physics = char_preview::Physics();
  if (ImGui::Checkbox("Physics", &physics)) char_preview::SetPhysics(physics);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hair and body jiggle as the game moves them (the model's own settings).");
  next_fits("Ring");
  bool ring = char_preview::Ring();
  if (ImGui::Checkbox("Ring", &ring)) char_preview::SetRing(ring, Utf8((fs::path(g_game) / L"pac" / L"bg" / L"bg29.pac").wstring()));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("The practice arena's ring round the superstar (posts, ropes, turnbuckles, aprons), its canvas under their feet.");
  if (char_preview::HasGear() || char_preview::Gear()) {
    next_fits("Entrance gear");
    ImGui::BeginDisabled(!char_preview::HasGear());
    bool gear = char_preview::Gear() && char_preview::HasGear();
    if (ImGui::Checkbox("Entrance gear", &gear)) char_preview::SetGear(gear);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(char_preview::HasGear() ? "What they wear for the entrance (a coat, hat or shirt)." : "This attire has no entrance gear.");
  }
  if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectMax().x + ImGui::GetCursorScreenPos().x > ImGui::CalcTextSize("Damage").x + 130 * g_scale)
    ImGui::SameLine();  // (else the Damage slider on its own line)
  ImGui::BeginDisabled(!blood);
  int stage = char_preview::BloodStage();
  static const char* kLevels[] = {"light", "medium", "heavy"};
  ImGui::TextUnformatted("Damage");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x, 70 * g_scale, 110 * g_scale));
  if (ImGui::SliderInt("##damage", &stage, 0, 2, kLevels[std::clamp(stage, 0, 2)], ImGuiSliderFlags_NoInput)) char_preview::SetBloodStage(stage);
  ImGui::EndDisabled();
  ImGui::SameLine();  // (the hint on this line when it fits what's left of it)
  if (const char* hint = "Drag to turn, scroll to zoom"; ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(hint).x + 8 * g_scale)
    ImGui::TextDisabled("%s", hint);
  else
    ImGui::NewLine();
  ImGui::PopID();
}

bool BinkFromVideoButton(const char* label, std::wstring& movie, const std::string& stem) {
  ImGui::PushID(label);
  bool pressed = false;
  if (ImGui::Button(label, ImVec2(220 * g_scale, 0))) {
    const COMDLG_FILTERSPEC spec[] = {{L"Videos and pictures", L"*.mp4;*.mov;*.m4v;*.avi;*.wmv;*.mkv;*.webm;*.bk2;*.bik2;*.png;*.jpg;*.jpeg;*.bmp"}};
    const std::wstring f = PickFile(false, L"A video (or a picture) for the entrance movie", spec, 1);
    if (!f.empty()) {
      const fs::path work = fs::path(g_game) / L"Mods" / L".convert";
      std::error_code ec;
      fs::create_directories(work, ec);
      const std::wstring out = (work / (Wide(IdFrom(stem, "movie")) + L"_" + fs::path(f).stem().wstring() + L".bik")).wstring();
      std::wstring* target = &movie;
      const std::string ext = Lower(fs::path(f).extension().string());
      if (ext == ".bk2" || ext == ".bik2") {  // Bink 2 (WWE 2K15 and later): through the player's RAD tool to .mp4 first
        const std::wstring mp4 = (work / (fs::path(f).stem().wstring() + L"_bink2.mp4")).wstring();
        std::vector<std::wstring> args = {f, mp4};
        if (!g_settings.bink2_tool.empty()) args.push_back(L"--tool"), args.push_back(g_settings.bink2_tool);
        RunToolJson(L"bink2_convert.py", args, [mp4, out, target](int code, const std::string& text) {
          const size_t e = text.find("\"error\"");
          if (code != 0 || !fs::exists(mp4)) {
            Status("The Bink 2 video didn't convert" + (e == std::string::npos ? std::string(" (see the log).") : ": see the log.") +
                   " Bink 2 needs RAD Video Tools (free, radgametools.com) - set it on Superstar > From another game.");
            Log("bink2: " + text);
            return;
          }
          WhenIdle([mp4, out, target] { MakeBink(mp4, out, 0, 0, [target](std::wstring made) { if (!made.empty()) *target = made, Touch(); }); });
        });
      } else {
        MakeBink(f, out, 0, 0, [target](std::wstring made) { if (!made.empty()) *target = made, Touch(); });
      }
      pressed = true;
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Any video or picture -> a 320 x 320 Bink laid out like the game's titantron movies (the "
                      "launcher's Movies tab does the same, with a preview and the bottom strip).");
  ImGui::PopID();
  return pressed;
}

// ---------------------------------------------------------------- dropped files

namespace {
std::vector<std::wstring> g_drop_files;  // the last drop (WM_DROPFILES / the ui script), its point, its age in frames
ImVec2 g_drop_at;
int g_drop_frames = 0;
}  // namespace

bool DroppedOnLastItem(std::vector<std::wstring>& files) {
  if (g_drop_files.empty()) return false;
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  if (g_drop_at.x < a.x || g_drop_at.y < a.y || g_drop_at.x >= b.x || g_drop_at.y >= b.y) return false;
  files = std::move(g_drop_files);
  g_drop_files.clear();
  return true;
}

void SetDrop(std::vector<std::wstring> files, ImVec2 at) {
  g_drop_files = std::move(files);
  g_drop_at = at;
  g_drop_frames = 0;
}

// ---------------------------------------------------------------- settings

namespace {

fs::path SettingsDir() {
  wchar_t* p = nullptr;
  fs::path dir;
  if (const wchar_t* own = _wgetenv(L"SVR2011_MM_SETTINGS"); own && *own) {  // (test runs: a settings folder of their own)
    std::error_code ec;
    fs::create_directories(own, ec);
    return fs::path(own);
  }
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / L"SvR2011 Mod Maker";
  if (p) CoTaskMemFree(p);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

void LoadSettings() {
  Bytes d;
  if (!ReadFile(PathStr(SettingsDir() / L"settings.txt"), d)) return;
  const std::string t(d.begin(), d.end());
  g_settings.game = Wide(Value(t, "game"));
  for (const auto& r : Values(t, "recent")) g_settings.recent.push_back(Wide(r));
  g_settings.win_x = std::atoi(Value(t, "win_x").c_str());
  g_settings.win_y = std::atoi(Value(t, "win_y").c_str());
  g_settings.win_w = std::atoi(Value(t, "win_w").c_str());
  g_settings.win_h = std::atoi(Value(t, "win_h").c_str());
  g_settings.maximized = Value(t, "maximized") == "1";
  g_settings.log_open = Value(t, "log_open") == "1";
  if (!Value(t, "font").empty()) g_settings.font = std::clamp(std::atoi(Value(t, "font").c_str()), 12, 28);
  for (const auto& f : Values(t, "disc_folder")) g_settings.disc_folders.push_back(Wide(f));
  g_settings.bink2_tool = Wide(Value(t, "bink2_tool"));
  for (const auto& d : Values(t, "my_disc")) {  // <game key>|<disc id>|<path>
    const size_t a = d.find('|'), b = a == std::string::npos ? a : d.find('|', a + 1);
    if (b == std::string::npos) continue;
    g_settings.my_discs.push_back({d.substr(0, a), d.substr(a + 1, b - a - 1), Wide(d.substr(b + 1))});
  }
  g_settings.discs_seeded = Value(t, "discs_seeded") == "1";
  g_settings.keep_bik = Value(t, "keep_bik") == "1";
  g_settings.moves_warning_off = Value(t, "moves_warning_off") == "1";
}

}  // namespace

void SaveSettings() {
  std::string t = "game=" + Utf8(g_settings.game) + "\n";
  for (const auto& r : g_settings.recent) t += "recent=" + Utf8(r) + "\n";
  t += "win_x=" + std::to_string(g_settings.win_x) + "\nwin_y=" + std::to_string(g_settings.win_y) +
       "\nwin_w=" + std::to_string(g_settings.win_w) + "\nwin_h=" + std::to_string(g_settings.win_h) +
       "\nmaximized=" + (g_settings.maximized ? "1" : "0") + "\nlog_open=" + (g_settings.log_open ? "1" : "0") +
       "\nfont=" + std::to_string(g_settings.font) + "\n";
  for (const auto& f : g_settings.disc_folders) t += "disc_folder=" + Utf8(f) + "\n";
  if (!g_settings.bink2_tool.empty()) t += "bink2_tool=" + Utf8(g_settings.bink2_tool) + "\n";
  for (const auto& d : g_settings.my_discs) t += "my_disc=" + d.key + "|" + d.id + "|" + Utf8(d.path) + "\n";
  if (g_settings.discs_seeded) t += "discs_seeded=1\n";
  if (g_settings.keep_bik) t += "keep_bik=1\n";
  if (g_settings.moves_warning_off) t += "moves_warning_off=1\n";
  WriteFile(PathStr(SettingsDir() / L"settings.txt"), Bytes(t.begin(), t.end()));
}

// ---- disc images

namespace {
std::vector<DiscRef> g_discs;
std::atomic<int> g_discs_state{0};  // 0 not read, 1 the first search runs, 3 ready
}  // namespace

std::string DiscGameName(const std::string& id) {
  static const std::map<std::string, const char*> kNames = {
      {"545107E0", "SmackDown vs. Raw 2007 (Xbox 360)"}, {"5451080B", "SmackDown vs. Raw 2008 (Xbox 360)"},
      {"54510826", "SmackDown vs. Raw 2009 (Xbox 360)"}, {"54510844", "SmackDown vs. Raw 2010 (Xbox 360)"},
      {"54510838", "Legends of WrestleMania (Xbox 360)"}, {"54510890", "WWE '12 (Xbox 360)"},
      {"545108B4", "WWE '13 (Xbox 360)"}, {"545408B2", "WWE 2K14 (Xbox 360)"}, {"545408B6", "WWE 2K15 (Xbox 360)"},
      {"315A07D2", "WWE 2K16 (Xbox 360)"}, {"545408BA", "WWE 2K17 (Xbox 360)"},
      {"SLES_504.77", "SmackDown! Just Bring It (PS2)"}, {"SLES_512.83", "SmackDown! Shut Your Mouth (PS2)"},
      {"SLES_520.36", "SmackDown! Here Comes the Pain (PS2)"}, {"SLES_527.81", "SmackDown! vs. Raw (PS2)"},
      {"SLES_536.76", "SmackDown! vs. Raw 2006 (PS2)"}, {"SLES_544.89", "SmackDown vs. Raw 2007 (PS2)"},
      {"SLES_548.79", "SmackDown vs. Raw 2008 (PS2)"}, {"SLES_552.51", "SmackDown vs. Raw 2009 (PS2)"},
      {"SLES_555.45", "SmackDown vs. Raw 2010 (PS2)"}, {"SLES_556.35", "SmackDown vs. Raw 2011 (PS2)"},
      {"SLUS_203.16", "SmackDown! Just Bring It (PS2)"}, {"SLUS_202.69", "SmackDown! Just Bring It (PS2)"},
      {"SLUS_201.22", "SmackDown! Just Bring It (PS2)"}, {"SLUS_204.83", "SmackDown! Shut Your Mouth (PS2)"},
      {"SLUS_205.12", "SmackDown! Shut Your Mouth (PS2)"}, {"SLUS_207.87", "SmackDown! Here Comes the Pain (PS2)"},
      {"SLUS_210.60", "SmackDown! vs. Raw (PS2)"}, {"SLUS_209.36", "SmackDown! vs. Raw (PS2)"},
      {"SLUS_212.86", "SmackDown! vs. Raw 2006 (PS2)"}, {"SLUS_211.34", "SmackDown! vs. Raw 2006 (PS2)"},
      {"SLUS_214.27", "SmackDown vs. Raw 2007 (PS2)"}, {"SLUS_216.45", "SmackDown vs. Raw 2008 (PS2)"},
      {"SLUS_218.10", "SmackDown vs. Raw 2009 (PS2)"}, {"SLUS_219.01", "SmackDown vs. Raw 2010 (PS2)"},
      {"SLUS_219.39", "SmackDown vs. Raw 2011 (PS2)"}, {"SLUS_203.85", "Crush Hour (PS2)"},
  };
  const auto it = kNames.find(id);
  return it == kNames.end() ? std::string() : it->second;
}

std::vector<std::wstring> DiscFolders() {
  if (!g_settings.disc_folders.empty()) return g_settings.disc_folders;
  return {L"D:\\Roms\\Xbox 360\\WWE", L"D:\\Roms\\Playstation 2\\WWE Games"};
}

// Discs() is My game discs (disc_list.h): each set game whose image / folder is there, in its order.
void RescanDiscs() {
  std::vector<DiscRef> out;
  std::error_code ec;
  for (const auto& d : g_settings.my_discs) {
    const disc_list::Game* g = disc_list::GameByKey(d.key);
    if (!g || d.path.empty() || !fs::exists(d.path, ec)) continue;
    DiscRef r;
    r.path = d.path;
    r.id = d.id.empty() ? disc_list::IdFor(d.key) : d.id;
    r.xbox = g->xbox;
    r.game = disc_list::FullName(*g) + (fs::is_directory(d.path, ec) ? " - folder" : "");
    out.push_back(std::move(r));
  }
  g_discs = std::move(out);
  g_discs_state = 3;
}

bool DiscsReady() {
  if (g_discs_state == 0) {
    RescanDiscs();
    if (!g_settings.discs_seeded) {  // the first time: the old disc folders searched to fill My game discs
      g_settings.discs_seeded = true;
      SaveSettings();
      std::vector<std::wstring> folders;
      std::error_code ec;
      for (const auto& f : DiscFolders())
        if (fs::is_directory(f, ec)) folders.push_back(f);
      if (g_settings.my_discs.empty() && !folders.empty()) {
        Log("discs: My game discs is empty - searching the disc folders once");
        disc_list::FindInFolders(folders);
        g_discs_state = 1;
      }
    }
  }
  if (g_discs_state == 1 && !disc_list::Scanning()) RescanDiscs();
  return g_discs_state == 3;
}

const std::vector<DiscRef>& Discs() {
  static const std::vector<DiscRef> kNone;
  return DiscsReady() ? g_discs : kNone;
}

void AddRecent(const std::wstring& file) {
  auto& r = g_settings.recent;
  r.erase(std::remove(r.begin(), r.end(), file), r.end());
  r.insert(r.begin(), file);
  if (r.size() > 8) r.resize(8);
  SaveSettings();
}

// ---------------------------------------------------------------- pages

namespace {

struct PageDesc {
  PageId id;
  const char* name;
  const char* section;  // rail heading ("" = continues the last)
  PageHooks* hooks;     // nullptr: not a mod type
  void (*draw)();
};

void DrawEditor() {
  editor::Draw();
}

// The "From another game" tabs: the Port page's kinds and the extracted-files tabs (Other games) for one thing
// The cards over a "From another game" tab: one file, or a folder of the game's files, instead of a disc. `file`: what
// one file is for these kinds ("" when they need more than one).
void SourceCards(const char* file) {
  const std::wstring src = port_page::CurrentSource();
  std::error_code ec;
  const bool folder = !src.empty() && fs::is_directory(src, ec), lone = !src.empty() && Lower(Utf8(fs::path(src).extension().wstring())) == ".pac";
  ImGui::TextDisabled("Where from:");
  const float w = simple::CardWidth(), h = 70 * g_scale;
  if (file[0]) {
    if (simple::Card("src_file", "One file", file, lone, w, h)) {
      static const COMDLG_FILTERSPEC kPac[1] = {{L"A game file (*.pac)", L"*.pac"}};
      const std::wstring f = PickFile(false, L"One file from another game", kPac, 1);
      if (!f.empty()) port_page::SetSource(f);
    }
    ImGui::SameLine(0, 8 * g_scale);
  }
  if (simple::Card("src_folder", "A folder of game files", "the game's files you copied off a disc", folder, w, h)) {
    const std::wstring f = PickFolder(L"A folder with the game's files (the one holding its pac folder)");
    if (!f.empty()) port_page::SetSource(f);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Or one of your game discs:");
}

void FromAnotherGame(const char* what, unsigned kinds, unsigned other, const char* file = "") {
  Heading("From another game", what);
  SourceCards(file);
  port_page::DrawKinds(kinds);
  if (port_page::CurrentSource().empty()) {  // (a new player: where the discs come from)
    ImGui::TextDisabled("Pick one of your games above, or a file of one. Your discs are set once in My game discs.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open My game discs")) GoTo(PageId::kDiscs);
  }
  if (!other) return;
  ImGui::Spacing();
  ImGui::SeparatorText("From another game's extracted files");
  other_page::DrawTabs(other);
}

// Every page (its project hooks, its draw). The sidebar shows the groups (kGroups) and their tabs.
const PageDesc kPages[] = {
    {PageId::kArena, "Arena", "Make", &arena_page::hooks, nullptr},
    {PageId::kEditor, "3D editor", "", nullptr, DrawEditor},
    {PageId::kBackstage, "Backstage", "", &backstage_page::hooks, nullptr},
    {PageId::kStar, "Superstar", "", &star_page::hooks, nullptr},
    {PageId::kAttire, "Attires", "", &attire_page::hooks, nullptr},
    {PageId::kThreads, "Threads parts", "", nullptr, star_page::ThreadsDraw},
    {PageId::kMoves, "Move pack", "", &moves_page::hooks, nullptr},
    {PageId::kSigns, "Crowd signs", "", &signs_page::hooks, nullptr},
    {PageId::kBackgrounds, "Menu backgrounds", "", &backgrounds_page::hooks, nullptr},
    {PageId::kMedia, "Media", "", &media_page::hooks, nullptr},
    {PageId::kMatch, "Match types", "", &match_page::hooks, nullptr},
    {PageId::kPort, "Port from a disc", "", nullptr, port_page::Draw},
    {PageId::kDiscs, "My game discs", "", nullptr, disc_list::Draw},
    {PageId::kPortEntrance, "Port an entrance", "", nullptr, [] { simple_port::Draw(simple_port::kEntrance); }},
    {PageId::kPortMoves, "Port moves", "", nullptr, [] { simple_port::Draw(simple_port::kMoves); }},
    {PageId::kPortTaunts, "Port taunts", "", nullptr, [] { simple_port::Draw(simple_port::kTaunts); }},
    {PageId::kOther, "Other games", "", nullptr, other_page::Draw},
    {PageId::kCaw, "CAW pictures", "Saves", nullptr, caw_page::Draw},
    {PageId::kAssets, "Game assets", "Look", nullptr, assets_page::Draw},
    {PageId::kAnims, "Animations", "", nullptr, anims_page::Draw},
    {PageId::kInstalled, "Installed mods", "", nullptr, installed_page::Draw},
    {PageId::kIcons, "Icons & renders", "", nullptr, icons_page::Draw},
    {PageId::kHelp, "Manual", "Help", nullptr, help_page::Draw},
    {PageId::kStarPort, "Superstar from another game", "", nullptr,
     [] {
       FromAnotherGame("A superstar or attires from your other WWE games (SvR 2007 to 2K17, the PS2 games): pick the disc "
                       "or one character file; the superstar opens on the Superstar tab to finish.",
                       port_page::kSuperstars | port_page::kAttires,
                       other_page::kStarFile | other_page::kW13Star | other_page::kSvr10Star,
                       "a character pac (chNNN.pac) or a PS2 game's CH.PAC");
     }},
    {PageId::kArenaPort, "Arena from another game", "", nullptr,
     [] {
       FromAnotherGame("An arena, a backstage room or the VS screens from your other WWE games: pick the disc or one "
                       "arena file; an arena opens on the Arena tab to finish.",
                       port_page::kArenas | port_page::kBackstageKind | port_page::kVsScreens,
                       other_page::kW13Arena | other_page::kSvr08,
                       "an arena pac (pac\\bg\\bgNN.pac)");
     }},
    {PageId::kMovesPort, "Moves from another game", "", nullptr,
     [] {
       FromAnotherGame("A superstar's moves or an entrance from another game, with every option (the Moves, Taunts and "
                       "Entrance tabs are the quick way).",
                       port_page::kMovesKind | port_page::kEntrances, other_page::kSvr10Moves,
                       "an entrance pac (Nyujyo0.pac, Entrance01.pac ...)");
     }},
    {PageId::kBelts, "Belts & weapons", "", nullptr,
     [] {
       FromAnotherGame("Title belts' and weapons' looks from another game, as a texture pack.",
                       port_page::kBelts | port_page::kWeapons, 0);
     }},
};

// The sidebar: one entry per group; a group with more than one tab shows them in a row over the page.
struct GroupTab {
  const char* label;
  PageId page;
  int media = -1;  // the Media page's part (media_page::SetTab)
};
struct Group {
  const char* name;
  const char* section;  // "" = continues the last
  std::vector<GroupTab> tabs;
};
const Group kGroups[] = {
    {"Superstar", "Make", {{"Superstar", PageId::kStar}, {"Attires", PageId::kAttire}, {"Threads parts", PageId::kThreads},
                           {"From another game", PageId::kStarPort}}},
    {"Arena", "", {{"Arena", PageId::kArena}, {"Backstage", PageId::kBackstage}, {"3D editor", PageId::kEditor},
                   {"From another game", PageId::kArenaPort}}},
    {"Moves & entrances", "", {{"Moves", PageId::kPortMoves}, {"Taunts", PageId::kPortTaunts}, {"Entrance", PageId::kPortEntrance},
                               {"Move pack", PageId::kMoves}, {"From another game", PageId::kMovesPort}}},
    {"Media", "", {{"Crowd signs", PageId::kSigns}, {"Menu backgrounds", PageId::kBackgrounds},
                   {"Titantron videos", PageId::kMedia, 0}, {"Menus & renders", PageId::kMedia, 1}, {"Audio", PageId::kMedia, 2},
                   {"Belts & weapons", PageId::kBelts}}},
    {"Match types", "", {{"Match types", PageId::kMatch}}},
    {"My game discs", "Porting", {{"My game discs", PageId::kDiscs}}},
    {"CAW pictures", "Saves", {{"CAW pictures", PageId::kCaw}}},
    {"Game assets", "Look", {{"Game assets", PageId::kAssets}}},
    {"Animations", "", {{"Animations", PageId::kAnims}}},
    {"Installed mods", "", {{"Installed mods", PageId::kInstalled}}},
    {"Icons & renders", "", {{"Icons & renders", PageId::kIcons}}},
    {"Manual", "Help", {{"Manual", PageId::kHelp}}},
};
constexpr int kGroupCount = int(sizeof kGroups / sizeof kGroups[0]);
int g_group_tab[kGroupCount] = {};  // each group's last tab (its sidebar entry goes back to it)

bool TabOn(const GroupTab& t) { return g_page == t.page && (t.media < 0 || media_page::Tab() == t.media); }

int GroupOf(PageId p) {
  for (int g = 0; g < kGroupCount; ++g)
    for (const auto& t : kGroups[g].tabs)
      if (t.page == p) return g;
  return -1;
}

void ShowTab(const GroupTab& t) {
  g_page = t.page;
  if (t.media >= 0) media_page::SetTab(t.media);
}

// The old Port from a disc and Other games pages live on the "From another game" tab of the thing they make.
void ResolvePage() {
  if (g_page == PageId::kPort) {
    const unsigned bit = 1u << port_page::Kind();
    g_page = bit & (port_page::kSuperstars | port_page::kAttires)    ? PageId::kStarPort
             : bit & (port_page::kMovesKind | port_page::kEntrances) ? PageId::kMovesPort
             : bit & (port_page::kBelts | port_page::kWeapons)       ? PageId::kBelts
                                                                     : PageId::kArenaPort;
  } else if (g_page == PageId::kOther) {
    g_page = PageId::kArenaPort;  // (what goes there from other pages: a WWE '12 - 2K17 arena file to convert)
  }
  const int g = GroupOf(g_page);
  if (g >= 0)
    for (int i = 0; i < int(kGroups[g].tabs.size()); ++i)
      if (TabOn(kGroups[g].tabs[size_t(i)])) g_group_tab[g] = i;
}

// The tab row over the page of a group with more than one tab.
void TabRow() {
  const int g = GroupOf(g_page);
  if (g < 0 || kGroups[g].tabs.size() < 2) return;
  for (size_t i = 0; i < kGroups[g].tabs.size(); ++i) {
    const GroupTab& t = kGroups[g].tabs[i];
    if (i) ImGui::SameLine(0, 4 * g_scale);
    const bool on = TabOn(t);
    const bool editor_off = t.page == PageId::kEditor && !editor::HasArena();
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::BeginDisabled(editor_off);
    if (ImGui::Button(t.label)) ShowTab(t);
    ImGui::EndDisabled();
    if (editor_off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Open an arena or a backstage area in 3D first (the Arena or Backstage tab).");
    if (on) ImGui::PopStyleColor();
  }
  ImGui::Separator();
}

const PageDesc& Desc(PageId p) {
  for (const auto& d : kPages)
    if (d.id == p) return d;
  return kPages[0];
}

PageHooks* Hooks(PageId p) { return Desc(p).hooks; }

// Each mod page is its own project: its state when last saved / opened, its file.
std::map<PageId, std::string> g_saved_state;
std::map<PageId, int> g_saved_touches;
std::map<PageId, std::wstring> g_project_files;

std::string CurrentState(PageId page) {
  PageHooks* h = Hooks(page);
  std::string s = h && h->state ? h->state() : std::string();
  if (h && h->write) {  // (and every key the project saves: a change to any of them is unsaved - materials too)
    ProjectOut out;
    out.keys_only = true;
    h->write(out);
    s += "\n--\n" + out.text;
  }
  if (page == PageId::kArena || page == PageId::kBackstage) s += "|" + std::to_string(editor::EditCount());
  return s;
}

void MarkSaved(PageId page) {
  g_saved_state[page] = CurrentState(page);
  g_saved_touches[page] = g_touches[page];
}

bool g_opened = false;  // a project or mod opened since the last reset

void MarkAllSaved() {
  for (const auto& d : kPages)
    if (d.hooks) MarkSaved(d.id);
}

// Nothing made yet: nothing opened, nothing changed (an opened mod or project counts as made)
bool ProjectUntouched() { return g_project_file.empty() && !g_opened && !ProjectDirty(); }

// The shown page decides the project (the 3D editor belongs to the arena /
// backstage page whose arena it holds).
void FollowPage() {
  PageId p = g_page;
  if (p == PageId::kEditor) p = Value(CurrentState(PageId::kBackstage), "backstage") != "-1" ? PageId::kBackstage : PageId::kArena;
  if (Hooks(p)) {
    g_project_page = p;
    g_project_file = g_project_files[p];
  }
}

}  // namespace

const char* PageName(PageId p) {
  const char* n = Desc(p).name;
  while (*n == ' ') ++n;
  return n;
}

void GoTo(PageId p) { g_page = p; }

bool PageByName(const std::wstring& name, PageId& page) {
  // (the old pages' names keep working: port / other open the "From another game" tab of the shown kind)
  static const struct {
    const wchar_t* name;
    PageId page;
    int media;
  } kNames[] = {
      {L"arena", PageId::kArena, -1}, {L"editor", PageId::kEditor, -1}, {L"backstage", PageId::kBackstage, -1},
      {L"superstar", PageId::kStar, -1}, {L"moves", PageId::kMoves, -1}, {L"move-pack", PageId::kMoves, -1},
      {L"signs", PageId::kSigns, -1}, {L"backgrounds", PageId::kBackgrounds, -1}, {L"media", PageId::kMedia, 0},
      {L"media-videos", PageId::kMedia, 0}, {L"media-renders", PageId::kMedia, 1}, {L"media-audio", PageId::kMedia, 2},
      {L"match", PageId::kMatch, -1}, {L"other", PageId::kOther, -1}, {L"caw", PageId::kCaw, -1},
      {L"assets", PageId::kAssets, -1}, {L"anims", PageId::kAnims, -1}, {L"icons", PageId::kIcons, -1},
      {L"help", PageId::kHelp, -1}, {L"port", PageId::kPort, -1}, {L"attires", PageId::kAttire, -1},
      {L"installed", PageId::kInstalled, -1}, {L"discs", PageId::kDiscs, -1},
      {L"port-entrance", PageId::kPortEntrance, -1}, {L"entrance", PageId::kPortEntrance, -1},
      {L"port-moves", PageId::kPortMoves, -1}, {L"moves-entrances", PageId::kPortMoves, -1},
      {L"port-taunts", PageId::kPortTaunts, -1}, {L"taunts", PageId::kPortTaunts, -1},
      {L"superstar-port", PageId::kStarPort, -1}, {L"arena-port", PageId::kArenaPort, -1},
      {L"moves-port", PageId::kMovesPort, -1}, {L"belts", PageId::kBelts, -1}, {L"threads", PageId::kThreads, -1}};
  for (const auto& n : kNames)
    if (name == n.name) {
      page = n.page;
      if (n.media >= 0) media_page::SetTab(n.media);
      return true;
    }
  return false;
}

bool ProjectDirty() {
  return g_saved_state.count(g_project_page) && (CurrentState(g_project_page) != g_saved_state[g_project_page] ||
                                                 g_touches[g_project_page] != g_saved_touches[g_project_page]);
}

void ProjectReset() {
  for (const auto& d : kPages)
    if (d.hooks && d.hooks->reset) d.hooks->reset();
  g_project_files.clear();
  g_project_file.clear();
  g_opened = false;
  MarkAllSaved();
}

// A new project of one type: that page emptied (the others keep their work).
void ProjectNew(PageId type) {
  if (PageHooks* h = Hooks(type); h && h->reset) h->reset();
  if (type == PageId::kArena || type == PageId::kBackstage) {
    if (PageHooks* o = Hooks(type == PageId::kArena ? PageId::kBackstage : PageId::kArena); o && o->reset) o->reset();
  }
  g_project_files[type].clear();
  g_opened = false;
  g_page = type;
  FollowPage();
  MarkSaved(type);
  Status(std::string("New ") + PageName(type) + " project.");
}

// ---- ProjectOut / ProjectIn

std::string ProjectOut::File(const std::string& key, const std::wstring& path, const std::string& as) {
  if (path.empty() || keys_only) return {};
  Bytes d;
  if (!ReadFile(Utf8(path), d)) {
    Log("Could not read " + Utf8(path) + " (left out of the project).");
    return {};
  }
  std::string name = as + Utf8(fs::path(path).extension().wstring());
  files.push_back({name, std::move(d)});
  text += key + "=" + name + "\n";
  text += key + ".from=" + Utf8(path) + "\n";
  return name;
}

void ProjectOut::Png(const std::string& key, const Image& img, const std::string& as) {
  if (img.rgba.empty() || keys_only) return;
  // (PNG through WIC needs a file: the project keeps pictures as RGBA DDS instead - lossless, quick)
  files.push_back({as + ".dds", DdsEncode(img, DxtFormat::kArgb, false)});
  text += key + "=" + as + ".dds\n";
}

const ZipEntry* ProjectIn::Find(const std::string& name) const {
  for (const auto& e : files)
    if (e.name == name) return &e;
  return nullptr;
}

std::wstring ProjectIn::Extract(const std::string& name) const {
  const ZipEntry* e = Find(name);
  if (!e) return {};
  wchar_t* p = nullptr;
  fs::path cache;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
    cache = fs::path(p) / L"SvR2011 Mod Maker" / L"projects" / dir.stem();
  if (p) CoTaskMemFree(p);
  std::error_code ec;
  fs::create_directories(cache, ec);
  const fs::path out = cache / fs::u8path(name);
  fs::create_directories(out.parent_path(), ec);  // (moves/motions/...: sub folders - without them a mod's pack was lost on open)
  if (!WriteFile(PathStr(out), e->data)) return {};
  return out.wstring();
}

bool ProjectIn::Png(const char* key, Image& out) const {
  const std::string n = Get(key);
  const ZipEntry* e = n.empty() ? nullptr : Find(n);
  return e && DdsDecode(e->data, out);
}

bool ProjectSave(bool ask_name) {
  PageHooks* h = Hooks(g_project_page);
  if (!h || !h->write) {
    Status("This page has nothing to save as a project.");
    return false;
  }
  std::wstring f = g_project_file;
  if (ask_name || f.empty()) {
    const std::string stem = IdFrom(h->state ? Value(h->state(), "name") : "", PageName(g_project_page));
    f = PickFile(true, L"Save the project", kProjectFilter, 1, L"svrproj", (Wide(stem) + L".svrproj").c_str());
    if (f.empty()) return false;
  }
  if (Busy()) {
    Status("Still working on the last job: save in a moment.");
    return false;
  }
  ProjectOut out;
  out.text = std::string("svrproj=1\ntype=") + h->type + "\n";
  h->write(out);
  out.files.insert(out.files.begin(), ZipEntry{"project.txt", Bytes(out.text.begin(), out.text.end())});
  g_project_file = f;
  g_project_files[g_project_page] = f;
  AddRecent(f);
  const std::string path = Utf8(f);
  if (out.deferred.empty()) {
    if (!WriteFile(path, ZipWrite(out.files))) {
      Status("The project could not be written: " + path);
      return false;
    }
    MarkSaved(g_project_page);
    Status("Project saved: " + path);
    return true;
  }
  // the slow part in the background; the page counts as saved once the file is written
  // (the state it holds is the one gathered now)
  auto shared = std::make_shared<ProjectOut>(std::move(out));
  const PageId page = g_project_page;
  const std::string state = CurrentState(page);
  const int touches = g_touches[page];
  RunInBackground([shared, path, page, state, touches] {
    Progress("Saving the project ...");
    for (auto& fn : shared->deferred) fn(shared->files);
    const bool ok = WriteFile(path, ZipWrite(shared->files));
    OnUiThread([ok, page, state, touches, path] {
      if (ok) g_saved_state[page] = state, g_saved_touches[page] = touches;
      Status(ok ? "Project saved: " + path : "The project could not be written: " + path);
    });
  });
  return true;
}

bool ProjectOpenIn(ProjectIn& in, bool mod, const std::wstring& file);

bool ProjectOpen(const std::wstring& file) {
  Bytes zip;
  ProjectIn in;
  in.dir = fs::path(file);
  in.dir = in.dir.parent_path() / in.dir.stem();
  if (!ReadFile(Utf8(file), zip) || !ZipRead(zip, in.files)) {
    Status("That is not a project or a mod the Mod Maker can open: " + Utf8(file));
    return false;
  }
  return ProjectOpenIn(in, Lower(FileName(file)).ends_with(".svrmod"), file);
}

// Test aid --ui-script <txt>: ImGui events played into the (hidden) window at given times, inside the app - never
// real input. One event a line, "<seconds> <what>": "move x y", "click x y" (left down + up, a frame apart),
// "rclick x y", "wheel n", "type <text>", "key <Escape|Enter|Tab|Backspace|Down|Up|A-Z>". x / y in the window's pixels
// (as in a --shot picture). Lines starting with # are skipped.
void UiScript(const std::wstring& file) {
  struct Ev {
    double t;
    std::string what, arg;
  };
  static std::vector<Ev> evs;
  static bool loaded = false;
  static size_t next = 0;
  static int up_frame = 0;  // (a click's button-up, the frame after its down)
  ImGuiIO& io = ImGui::GetIO();
  if (!loaded) {
    loaded = true;
    Bytes b;
    if (ReadFile(Utf8(file), b)) {
      std::string text(b.begin(), b.end()), line;
      for (size_t at = 0; at <= text.size(); ++at) {
        if (at < text.size() && text[at] != '\n') {
          if (text[at] != '\r') line += text[at];
          continue;
        }
        if (!line.empty() && line[0] != '#') {
          char what[32] = {};
          double t = 0;
          int used = 0;
          if (std::sscanf(line.c_str(), "%lf %31s %n", &t, what, &used) >= 2) evs.push_back({t, what, line.substr(size_t(used))});
        }
        line.clear();
      }
    }
    Log("ui script: " + std::to_string(evs.size()) + " events");
  }
  if (up_frame && --up_frame == 0) io.AddMouseButtonEvent(0, false);
  while (next < evs.size() && ImGui::GetTime() >= evs[next].t && !up_frame) {
    const Ev& e = evs[next++];
    float x = 0, y = 0;
    std::sscanf(e.arg.c_str(), "%f %f", &x, &y);
    if (e.what == "move") io.AddMousePosEvent(x, y);
    else if (e.what == "click" || e.what == "rclick") {
      io.AddMousePosEvent(x, y);
      io.AddMouseButtonEvent(e.what == "click" ? 0 : 1, true);
      if (e.what == "click") up_frame = 2;
      else io.AddMouseButtonEvent(1, false);
    } else if (e.what == "wheel") io.AddMouseWheelEvent(0, x);
    else if (e.what == "drop") {  // drop <x> <y> <file>: as if dropped from Explorer
      int used = 0;
      std::sscanf(e.arg.c_str(), "%f %f %n", &x, &y, &used);
      io.AddMousePosEvent(x, y);
      SetDrop({Wide(e.arg.substr(size_t(used)))}, ImVec2(x, y));
    }
    else if (e.what == "type") io.AddInputCharactersUTF8(e.arg.c_str());
    else if (e.what == "key") {
      const ImGuiKey k = e.arg.size() == 1 && e.arg[0] >= 'A' && e.arg[0] <= 'Z' ? ImGuiKey(ImGuiKey_A + (e.arg[0] - 'A'))
                         : e.arg == "Escape" ? ImGuiKey_Escape : e.arg == "Enter" ? ImGuiKey_Enter : e.arg == "Tab" ? ImGuiKey_Tab
                         : e.arg == "Backspace" ? ImGuiKey_Backspace : e.arg == "Down" ? ImGuiKey_DownArrow : ImGuiKey_UpArrow;
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    }
    Log("ui script: " + e.what + " " + e.arg);
  }
}

bool ProjectOpenFolder(const std::wstring& dir) {
  ProjectIn in;
  in.dir = fs::path(dir);  // (Extract writes into the project cache under LocalAppData, never here)
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    Bytes b;
    if (!ReadFile(PathStr(e.path()), b)) continue;
    in.files.push_back({fs::relative(e.path(), dir, ec).generic_string(), std::move(b)});
  }
  if (in.files.empty()) {
    Status("Nothing to open in " + Utf8(dir) + ".");
    return false;
  }
  return ProjectOpenIn(in, true, (fs::path(dir) / L"manifest.txt").wstring());
}

namespace {
// An absolute path inside a value (X:\..., X:/..., \\server\...) cut to its file name; the rest kept.
std::string StripPaths(const std::string& v) {
  std::string out;
  for (size_t i = 0; i < v.size();) {
    const bool drive = i + 2 < v.size() && std::isalpha(uint8_t(v[i])) && v[i + 1] == ':' && (v[i + 2] == '\\' || v[i + 2] == '/') &&
                       (i == 0 || !std::isalnum(uint8_t(v[i - 1])));
    const bool unc = i + 1 < v.size() && v[i] == '\\' && v[i + 1] == '\\';
    if (!drive && !unc) {
      out += v[i++];
      continue;
    }
    size_t e = i;  // (a path ends at the value's own separators)
    while (e < v.size() && v[e] != '\t' && v[e] != '\x1d' && v[e] != '\x1f' && v[e] != '|' && v[e] != ',' && v[e] != '\n') ++e;
    const std::string p = v.substr(i, e - i);
    const size_t slash = p.find_last_of("\\/");
    out += slash == std::string::npos ? p : p.substr(slash + 1);
    i = e;
  }
  return out;
}
}  // namespace

std::string ModProjectText(PageId page) {
  PageHooks* h = Hooks(page);
  if (!h || !h->write) return {};
  ProjectOut out;
  out.keys_only = true;
  h->write(out);
  std::string t = "# The Mod Maker's project settings for this mod (materials, FBX steps ...): opening the mod brings "
                  "them back. Paths are file names only.\n";
  for (size_t at = 0; at < out.text.size();) {
    size_t e = out.text.find('\n', at);
    if (e == std::string::npos) e = out.text.size();
    const std::string line = out.text.substr(at, e - at);
    at = e + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    if (key.size() > 5 && key.compare(key.size() - 5, 5, ".from") == 0) continue;  // (the author's own folders)
    t += key + "=" + StripPaths(line.substr(eq + 1)) + "\n";
  }
  return t;
}

void AddModProject(std::vector<ZipEntry>& files, const std::string& text) {
  if (text.empty()) return;
  for (auto& f : files)
    if (f.name == ".mm_project.txt") {
      f.data.assign(text.begin(), text.end());
      return;
    }
  files.push_back({".mm_project.txt", Bytes(text.begin(), text.end())});
}

bool ProjectOpenIn(ProjectIn& in, bool mod, const std::wstring& file) {
  const ZipEntry* txt = in.Find(mod ? "manifest.txt" : "project.txt");
  if (!txt) {
    Status(mod ? "That mod has no manifest.txt." : "That project has no project.txt.");
    return false;
  }
  in.text.assign(txt->data.begin(), txt->data.end());
  if (mod)  // (the mod's own project settings: the keys its manifest doesn't have - an older mod has none)
    if (const ZipEntry* pt = in.Find(".mm_project.txt")) {
      const std::string p(pt->data.begin(), pt->data.end());
      if (!in.text.empty() && in.text.back() != '\n') in.text += '\n';
      for (size_t at = 0; at < p.size();) {
        size_t e = p.find('\n', at);
        if (e == std::string::npos) e = p.size();
        const std::string line = p.substr(at, e - at);
        at = e + 1;
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        if (Value(in.text, line.substr(0, eq).c_str()).empty()) in.text += line + "\n";
      }
    }
  const std::string type = Value(in.text, "type");
  const PageDesc* page = nullptr;
  for (const auto& d : kPages)
    if (d.hooks && d.hooks->type && type == d.hooks->type) page = &d;
  if (!page) {
    Status("The Mod Maker doesn't know the mod type \"" + type + "\".");
    return false;
  }
  ProjectNew(page->id);
  const auto& fn = mod ? page->hooks->read_mod : page->hooks->read;
  if (!fn || !fn(in)) {
    Status("Could not open " + Utf8(file) + ".");
    return false;
  }
  g_project_files[page->id] = mod ? L"" : file;
  FollowPage();
  AddRecent(file);
  MarkSaved(page->id);
  g_opened = true;  // (made: its problems count even before a change)
  Status("Opened " + Utf8(file) + (mod ? " (a mod: save it as a project to keep working on it)." : "."));
  return true;
}

// ---------------------------------------------------------------- the shell

namespace {

void Style() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = 0;
  s.FrameRounding = 4;
  s.ChildRounding = 6;
  s.PopupRounding = 6;
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ScrollbarSize = 12;
  s.WindowBorderSize = 0;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0.110f, 0.114f, 0.133f, 1);
  c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.15f, 1);
  c[ImGuiCol_Button] = ImVec4(0.165f, 0.176f, 0.208f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.28f, 0.33f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_Header] = ImVec4(0.78f, 0.06f, 0.18f, 0.6f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.78f, 0.06f, 0.18f, 0.8f);
  c[ImGuiCol_FrameBg] = ImVec4(0.165f, 0.176f, 0.208f, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1);
  c[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.18f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(0.5f, 0.08f, 0.16f, 1);
  c[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(1, 0.3f, 0.4f, 1);
  c[ImGuiCol_SliderGrab] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_SliderGrabActive] = ImVec4(1, 0.2f, 0.35f, 1);
  c[ImGuiCol_Separator] = ImVec4(0.25f, 0.26f, 0.3f, 1);
}

bool g_show_problems = false;
std::vector<Problem> g_problems;  // the project page's, this frame

void PickGameFolder() {
  const std::wstring d = PickFolder(L"The installed game folder (it has the pac folder)");
  if (d.empty()) return;
  if (!fs::exists(fs::path(d) / L"pac" / L"bg")) {
    Status("That folder has no pac\\bg: it isn't the game folder.");
    return;
  }
  g_game = d;
  g_settings.game = d;
  SaveSettings();
  for (auto& a : g_arenas) a.tile.Clear();
  LoadBanners();
  g_stars_loaded = false;
  g_stars.clear();
  g_renders.clear();
  Status("Game folder: " + Utf8(d));
}

void NewMenu() {
  if (ImGui::BeginPopup("new_menu")) {
    ImGui::TextDisabled("A new mod:");
    for (const auto& d : kPages)
      if (d.hooks && ImGui::MenuItem(PageName(d.id))) ProjectNew(d.id);
    ImGui::EndPopup();
  }
}

void TitleBar() {
  ImGui::BeginChild("title", ImVec2(0, 44 * g_scale), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPosY(6 * g_scale);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
  ImGui::TextUnformatted("SvR2011 Mod Maker");
  ImGui::PopFont();
  ImGui::SameLine(0, 24 * g_scale);
  const bool dirty = ProjectDirty();
  if (ImGui::Button("New")) ImGui::OpenPopup("new_menu");
  NewMenu();
  ImGui::SameLine();
  if (ImGui::Button("Open...")) {
    const std::wstring f = PickFile(false, L"Open a project or a mod", kOpenFilter, 1);
    if (!f.empty()) ProjectOpen(f);
  }
  if (!g_settings.recent.empty()) {
    ImGui::SameLine(0, 2);
    if (ImGui::ArrowButton("recent", ImGuiDir_Down)) ImGui::OpenPopup("recent_menu");
    if (ImGui::BeginPopup("recent_menu")) {
      ImGui::TextDisabled("Recent:");
      for (const auto& r : std::vector<std::wstring>(g_settings.recent))
        if (ImGui::MenuItem(Utf8(r).c_str())) ProjectOpen(r);
      ImGui::EndPopup();
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!Hooks(g_project_page));
  if (ImGui::Button(dirty ? "Save *" : "Save")) ProjectSave(false);
  ImGui::SameLine();
  if (ImGui::Button("Save as...")) ProjectSave(true);
  ImGui::EndDisabled();
  ImGui::SameLine(0, 24 * g_scale);
  {
    PageHooks* h = Hooks(g_project_page);
    const std::string name = h && h->state ? Value(h->state(), "name") : "";
    ImGui::TextDisabled("%s project:", PageName(g_project_page));
    ImGui::SameLine();
    ImGui::TextUnformatted(g_project_file.empty() ? (name.empty() ? "(unsaved)" : name.c_str())
                                                  : FileName(g_project_file).c_str());
    if (!g_project_file.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Utf8(g_project_file).c_str());
  }
  // right: the text size and the game folder
  const std::string game = g_game.empty() ? "No game folder" : Utf8(fs::path(g_game).filename().wstring());
  const float w = ImGui::CalcTextSize(game.c_str()).x + 40 * g_scale;
  const float rw = ImGui::CalcTextSize("Report a problem...").x + 24 * g_scale;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w - rw - 60 * g_scale));
  if (ImGui::Button("Report a problem...")) {
    PageHooks* h = Hooks(g_project_page);
    session::OpenReport([h] { return h && h->state ? h->state() : std::string(); });
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Something went wrong? Make a report (a zip with what the Mod Maker did) to post.");
  ImGui::SameLine();
  if (ImGui::Button("Aa")) ImGui::OpenPopup("text_size");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Text size (applies the next time the Mod Maker starts).");
  if (ImGui::BeginPopup("text_size")) {
    const int sizes[] = {14, 15, 17, 19, 22, 26};
    for (int sz : sizes) {
      char label[32];
      std::snprintf(label, sizeof label, "%d px%s", sz, sz == 17 ? "  (normal)" : "");
      if (ImGui::MenuItem(label, nullptr, g_settings.font == sz)) {
        g_settings.font = sz;
        SaveSettings();
        Status("Text size saved: it applies the next time the Mod Maker starts.");
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (g_game.empty()) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  if (ImGui::Button(game.c_str())) PickGameFolder();
  if (g_game.empty()) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", g_game.empty() ? "Choose the folder where the game is installed."
                                            : ("The game folder the arenas, superstars and renders are read from:\n" +
                                               Utf8(g_game) + "\nClick to change it.")
                                                  .c_str());
  ImGui::EndChild();
}

void Rail(float height) {
  ImGui::BeginChild("rail", ImVec2(170 * g_scale, height), true);
  for (int g = 0; g < kGroupCount; ++g) {
    const Group& d = kGroups[g];
    if (d.section[0]) {
      if (g) ImGui::Spacing();
      ImGui::TextDisabled("%s", Upper(d.section).c_str());
    }
    const bool on = GroupOf(g_page) == g;
    bool project_type = false;  // (a dot: the current project's type is in this group)
    for (const auto& t : d.tabs) project_type |= Hooks(t.page) && t.page == g_project_page;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    if (ImGui::Button(d.name, ImVec2(-1, 0))) {
      const GroupTab& t = d.tabs[size_t(g_group_tab[g])];
      ShowTab(t.page == PageId::kEditor && !editor::HasArena() ? d.tabs[0] : t);
    }
    if (project_type && !on) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(b.x - 10 * g_scale, (a.y + b.y) / 2), 3 * g_scale,
                                                  IM_COL32(230, 25, 50, 255));
    }
    if (on) ImGui::PopStyleColor();
  }
  ImGui::EndChild();
}

void StatusBar() {
  ImGui::BeginChild("status", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
  std::string status, progress;
  {
    std::lock_guard lock(g_log_mutex);
    status = g_status, progress = g_progress;
  }
  if (Busy()) {
    const char spin[] = "|/-\\";
    ImGui::TextColored(kWarn, "%c  %s", spin[int(ImGui::GetTime() * 8) & 3], progress.empty() ? "Working..." : progress.c_str());
  } else {
    ImGui::TextUnformatted(status.c_str());
  }
  // right: problems and the log
  // (a fresh, untouched project isn't counted: "no arena yet" is no problem before you start)
  const bool untouched = ProjectUntouched();
  if (untouched) g_problems.clear();
  const int errors = ErrorCount(g_problems), warnings = int(g_problems.size()) - errors;
  char pb[128];
  if (g_problems.empty()) std::snprintf(pb, sizeof pb, untouched ? "Nothing made yet" : "No problems");
  else std::snprintf(pb, sizeof pb, "%s%s%d error%s, %d warning%s", g_page != g_project_page ? PageName(g_project_page) : "",
                     g_page != g_project_page ? " project: " : "", errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
  const float w = ImGui::CalcTextSize(pb).x + ImGui::CalcTextSize("Log").x + 60 * g_scale;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w));
  if (!g_problems.empty()) ImGui::PushStyleColor(ImGuiCol_Text, errors ? kBad : kWarn);
  if (ImGui::SmallButton(pb)) g_show_problems = !g_show_problems, g_settings.log_open = false;
  if (!g_problems.empty()) ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::SmallButton(g_settings.log_open ? "Log v" : "Log ^")) {
    g_settings.log_open = !g_settings.log_open;
    g_show_problems = false;
    SaveSettings();
  }
  ImGui::EndChild();
}

void LogPanel(float height) {
  ImGui::BeginChild("log", ImVec2(0, height), true);
  if (g_show_problems) {
    ImGui::TextDisabled("Problems on the %s page:", PageName(g_project_page));
    if (g_problems.empty()) ImGui::TextUnformatted("None.");
    DrawProblems(g_problems);
  } else {
    std::lock_guard lock(g_log_mutex);
    for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}

void Draw() {
  {  // work handed to the UI thread
    std::vector<std::function<void()>> q;
    {
      std::lock_guard lock(g_ui_mutex);
      q.swap(g_ui_queue);
    }
    for (auto& fn : q) fn();
  }
  arena_page::Tick();
  ResolvePage();
  FollowPage();
  g_problems.clear();
  if (PageHooks* h = Hooks(g_project_page); h && h->problems) h->problems(g_problems);
  if (!g_test_fix.empty() && g_page != PageId::kInstalled && ++g_test_fix_frames > 90 && !Busy()) TestFixOn(g_problems);
  if (g_test_install_frames >= 0 && g_test_fix.empty() && ++g_test_install_frames > 120 && !Busy())
    g_test_install_frames = -1, Log("test: install"), star_page::TestInstall();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("Mod Maker", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
  TitleBar();
  const float status_h = 26 * g_scale;
  const float log_h = (g_settings.log_open || g_show_problems) ? 140 * g_scale : 0;
  const float body_h = -(status_h + log_h + (log_h ? ImGui::GetStyle().ItemSpacing.y : 0));
  Rail(body_h);
  ImGui::SameLine();
  const PageDesc& d = Desc(g_page);
  ImGui::BeginChild("page", ImVec2(0, body_h), d.id != PageId::kEditor,
                    d.id == PageId::kEditor ? ImGuiWindowFlags_None : ImGuiWindowFlags_None);
  TabRow();
  if (d.draw) d.draw();
  else if (d.hooks && d.hooks->draw) d.hooks->draw();
  ImGui::EndChild();
  if (log_h) LogPanel(log_h);
  StatusBar();
  ImGui::End();
  if (!g_drop_files.empty() && ++g_drop_frames > 2) g_drop_files.clear();  // (dropped where nothing takes files)
  full_preview::Draw();  // (its own window, over the page)
  ToolsPanel();          // (the Mod Maker tools: found / missing, Install what's missing)
  MovesWarning();        // (before a move conversion: the experimental warning to accept)
  session::Tick();       // (the session log file, page changes, the Report a problem window)
  // Ctrl+S saves the project
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && Hooks(g_project_page)) ProjectSave(false);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
    const std::wstring f = PickFile(false, L"Open a project or a mod", kOpenFilter, 1);
    if (!f.empty()) ProjectOpen(f);
  }
}

bool g_quit_asked = false;

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(w, m, wp, lp)) return true;
  switch (m) {
    case WM_SIZE:
      if (g_dev && wp != SIZE_MINIMIZED) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        CreateTarget();
      }
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      std::vector<std::wstring> files;
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT k = 0; k < n; ++k) {
        wchar_t f[MAX_PATH * 4];
        if (DragQueryFileW(drop, k, f, UINT(std::size(f)))) files.push_back(f);
      }
      POINT pt = {};
      DragQueryPoint(drop, &pt);
      DragFinish(drop);
      SetDrop(std::move(files), ImVec2(float(pt.x), float(pt.y)));
      return 0;
    }
    case WM_CLOSE:
      if (ProjectDirty() && !g_quit_asked && IsWindowVisible(w)) {
        const int r = MessageBoxW(w, L"Save the project before closing?", L"SvR2011 Mod Maker",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !ProjectSave(false)) return 0;
      }
      DestroyWindow(w);
      return 0;
    case WM_DESTROY: {
      WINDOWPLACEMENT pl = {sizeof pl};
      if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_NOACTIVATE) && GetWindowPlacement(w, &pl)) {
        g_settings.win_x = pl.rcNormalPosition.left;
        g_settings.win_y = pl.rcNormalPosition.top;
        g_settings.win_w = pl.rcNormalPosition.right - pl.rcNormalPosition.left;
        g_settings.win_h = pl.rcNormalPosition.bottom - pl.rcNormalPosition.top;
        g_settings.maximized = pl.showCmd == SW_SHOWMAXIMIZED;
        SaveSettings();
      }
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(w, m, wp, lp);
}

}  // namespace
}  // namespace mm

using namespace mm;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  session::InstallCrashHandler();  // (a crash leaves a file and a log line: session_log.h)
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LoadSettings();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  // --test-repack <folder>,<report.txt>: every EPAC / EPK8 under the folder read and written
  // back in memory (the pac tool's saves must be byte-identical when nothing changed); no window
  for (int i = 1; i + 1 < argc; ++i)
    if (!wcscmp(argv[i], L"--test-repack")) {
      const std::wstring a = argv[i + 1];
      const size_t comma = a.find(L',');
      const fs::path folder = a.substr(0, comma), report = comma == std::wstring::npos ? L"repack.txt" : a.substr(comma + 1);
      int files = 0, same = 0;
      std::string out;
      std::error_code ec;
      for (const auto& e : fs::recursive_directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".pac") continue;
        Bytes d;
        svrfmt::Epac pac;
        if (!ReadFile(PathStr(e.path()), d) || !svrfmt::EpacRead(d, pac)) continue;
        ++files;
        if (svrfmt::EpacWrite(pac) == d) ++same;
        else out += "differs: " + PathStr(e.path()) + "\n";
      }
      out += "repack: " + std::to_string(same) + " / " + std::to_string(files) + " identical\n";
      WriteFile(PathStr(report), Bytes(out.begin(), out.end()));
      return same == files ? 0 : 1;
    }
  // --test-lufs <song>,<report.txt>: the theme volume meter on one file; no window
  for (int i = 1; i + 1 < argc; ++i)
    if (!wcscmp(argv[i], L"--test-lufs")) {
      const std::wstring a = argv[i + 1];
      const size_t comma = a.find(L',');
      double l = 0;
      std::string err;
      const bool ok = MeasureLufs(a.substr(0, comma), &l, &err);
      char b[128];
      std::snprintf(b, sizeof b, "%s %.2f LUFS %s\n", ok ? "ok" : "failed", l, err.c_str());
      WriteFile(Utf8(comma == std::wstring::npos ? L"lufs.txt" : a.substr(comma + 1)), Bytes(b, b + std::strlen(b)));
      return ok ? 0 : 1;
    }
  // test aids: --editor <arena tile 0-19> opens the editor on it, --open <mod> opens a mod,
  // --editor-view <0-2> a camera preset, --select <name> an object; --page <n>; the
  // superstar / signs / media pages' --star*, --sign, --media-* and --test-*-save
  int start_editor = -1, start_view = -1, start_new = -1, start_page = -1, start_backstage = -1, test_lib = -1;
  std::wstring start_mod, start_select, test_save, project, test_project, test_roundtrip, moves_pack, page_name;
  std::string test_obj_add, test_obj_export;  // (--test-add-object / --test-export-object: editor::TestObjects)
  int test_obj_remove = -1;
  bool behind = false;  // --behind: tests - the window is never shown at all (frames still run)
  std::wstring perf_tour;  // --perf-tour <report>: every page in turn, its first-frame and slowest times
  std::vector<std::wstring> run_tool_args;
  std::wstring make_bink_in, make_bink_out, moves_save;
  std::wstring shot;    // --shot <png> [--shot-after <s>]: the frame saved from the back buffer, then quit
  std::wstring ui_script;  // --ui-script <txt>: ImGui input played in, no real input (see UiScript)
  bool ring_view = false;
  double shot_after = 6;
  std::vector<std::pair<double, std::wstring>> page_at;  // --page-at <page>@<s>[,...]: page switches at those times (tests)
  int star_id = 0, star_call = -1;
  float star_height = 0, star_call_gain = 0;
  std::string star_moveset, star_import_moveset;
  int star_entrance = 0;
  int star_victory = 0;
  std::wstring star_model, star_song, star_movie, star_picture, star_voice, star_save, sign_save, media_save, media_video;
  std::string star_name;
  std::vector<std::wstring> sign_files, media_pictures, background_files;
  std::wstring background_save;
  int media_arena = -1;
  for (int i = 1; i < argc; ++i) {
    const bool more = i + 1 < argc;
    if (!wcscmp(argv[i], L"--game") && more) g_game = argv[++i];
    else if (!wcscmp(argv[i], L"--editor") && more) start_editor = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--editor-view") && more) start_view = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--open") && more) start_mod = argv[++i];
    else if (!wcscmp(argv[i], L"--vs-test") && more) arena_page::SetVsTest(Utf8(argv[++i]));
    else if (!wcscmp(argv[i], L"--project") && more) project = argv[++i];
    else if (!wcscmp(argv[i], L"--behind")) behind = true, g_behind = true;
    else if (!wcscmp(argv[i], L"--test-report") && more) session::TestReport(Utf8(argv[++i]));
    else if (!wcscmp(argv[i], L"--test-report-send") && more) session::TestReportSend(Utf8(argv[++i]));  // <answers file>  // <what happened>: Report a problem, no window, then quit
    else if (!wcscmp(argv[i], L"--watch-strip") && more) anims_page::TestStrip(argv[++i]);
    else if (!wcscmp(argv[i], L"--move-check") && more) move_check::Test(argv[++i]), g_page = PageId::kMoves;
    else if (!wcscmp(argv[i], L"--watch") && more) {  // <star id>,<dummy id>,<move id>: a match move watched here
      int a[3] = {0, 0, 0};
      swscanf_s(argv[++i], L"%d,%d,%d", &a[0], &a[1], &a[2]);
      anims_page::TestWatch(a[0], a[1], a[2]);
      g_page = PageId::kAnims;
    }
    else if (!wcscmp(argv[i], L"--test-play") && more) {  // <star id>,<dummy id>,<move id>: Play in game
      int a[3] = {0, 0, 0};
      swscanf_s(argv[++i], L"%d,%d,%d", &a[0], &a[1], &a[2]);
      anims_page::TestPlay(a[0], a[1], a[2]);
      g_page = PageId::kAnims;
    }
    else if (!wcscmp(argv[i], L"--shot") && more) shot = argv[++i];
    else if (!wcscmp(argv[i], L"--shot-after") && more) shot_after = _wtof(argv[++i]);
    else if (!wcscmp(argv[i], L"--ui-script") && more) ui_script = argv[++i];
    else if (!wcscmp(argv[i], L"--page-at") && more) {
      const std::wstring v = argv[++i];
      for (size_t at = 0; at < v.size();) {
        size_t c = v.find(L',', at);
        if (c == std::wstring::npos) c = v.size();
        const std::wstring item = v.substr(at, c - at);
        if (const size_t a = item.find(L'@'); a != std::wstring::npos) page_at.push_back({_wtof(item.c_str() + a + 1), item.substr(0, a)});
        at = c + 1;
      }
    }
    else if (!wcscmp(argv[i], L"--test-project") && more) test_project = argv[++i];
    else if (!wcscmp(argv[i], L"--test-roundtrip") && more) test_roundtrip = argv[++i];
    else if (!wcscmp(argv[i], L"--moves-pack") && more) moves_pack = argv[++i];
    else if (!wcscmp(argv[i], L"--test-moves-save") && more) moves_save = argv[++i];
    else if (!wcscmp(argv[i], L"--caw-announcer") && more) caw_page::TestAnnouncer(Utf8(argv[++i]));  // <CLIP> (- clears), with --caw <index>
    else if (!wcscmp(argv[i], L"--caw") && more) {  // <index>[,<attire>,<picture>[,save]]
      std::wstring a = argv[++i];
      std::vector<std::wstring> parts;
      for (size_t at = 0; at <= a.size();) {
        size_t e = a.find(L',', at);
        if (e == std::wstring::npos) e = a.size();
        parts.push_back(a.substr(at, e - at));
        at = e + 1;
      }
      caw_page::TestStart(_wtoi(parts[0].c_str()), parts.size() > 1 ? _wtoi(parts[1].c_str()) : 0,
                          parts.size() > 2 ? parts[2] : L"", parts.size() > 3 && parts[3] == L"save");
      g_page = PageId::kCaw;
    }
    else if (!wcscmp(argv[i], L"--convert-w13") && more) {  // <wwe13 pac>,<host tile>
      const std::wstring a = argv[++i];
      const size_t c = a.rfind(L',');
      other_page::TestConvertW13(a.substr(0, c), c == std::wstring::npos ? 1 : _wtoi(a.c_str() + c + 1));
    }
    else if (!wcscmp(argv[i], L"--make-bink") && more) {  // <video or picture>,<out.bik>
      const std::wstring a = argv[++i];
      const size_t c = a.rfind(L',');
      if (c != std::wstring::npos) make_bink_in = a.substr(0, c), make_bink_out = a.substr(c + 1);
    }
    else if (!wcscmp(argv[i], L"--run-tool") && more) {  // <script>,<arg>,<arg>...: the Python tool, output in the log
      std::vector<std::wstring> parts;
      const std::wstring a = argv[++i];
      for (size_t at = 0; at <= a.size();) {
        size_t e = a.find(L',', at);
        if (e == std::wstring::npos) e = a.size();
        parts.push_back(a.substr(at, e - at));
        at = e + 1;
      }
      run_tool_args = parts;
      g_page = PageId::kOther;
    }
    else if (!wcscmp(argv[i], L"--match-tab") && more) {  // <tab>[,<rule>]
      int a[2] = {0, -1};
      swscanf_s(argv[++i], L"%d,%d", &a[0], &a[1]);
      match_page::TestTab(a[0], a[1]);
      g_page = PageId::kMatch;
    }
    else if (!wcscmp(argv[i], L"--assets") && more) assets_page::TestOpen(Utf8(argv[++i])), g_page = PageId::kAssets;
    else if (!wcscmp(argv[i], L"--assets-source") && more) assets_page::TestSource(argv[++i]), g_page = PageId::kAssets;
    else if (!wcscmp(argv[i], L"--test-pac-edit") && more) pac_editor::TestEdit(argv[++i]), g_page = PageId::kAssets;
    else if (!wcscmp(argv[i], L"--perf-tour") && more) perf_tour = argv[++i];
    else if (!wcscmp(argv[i], L"--port-fullpreview")) port_page::TestFullPreview();  // with --port-test on an entrance: its result in the Full preview
    else if (!wcscmp(argv[i], L"--port-test") && more) port_page::TestStart(Utf8(argv[++i])), g_page = PageId::kPort;
    else if (!wcscmp(argv[i], L"--port-clear")) port_page::TestClear(), g_page = PageId::kPort;
    else if (!wcscmp(argv[i], L"--port-trim")) port_page::TestTrim(), g_page = PageId::kPort;
    else if (!wcscmp(argv[i], L"--port-as-attire") && more) port_page::TestAsAttire(Utf8(argv[++i]));  // with --port-test on a superstar
    else if (!wcscmp(argv[i], L"--icons") && more) icons_page::TestTab(_wtoi(argv[++i])), g_page = PageId::kIcons;
    else if (!wcscmp(argv[i], L"--fbx-map") && more) {  // <fbx>,<out file>: its skeleton, the rig found and the mapping
      const std::wstring v = argv[++i];
      const size_t c = v.find(L',');
      std::vector<svrfmt::FbxBone> fb;
      std::string err, out;
      if (c != std::wstring::npos && svrfmt::ReadFbxSkeleton(Utf8(v.substr(0, c)), fb, err)) {
        std::vector<RigBone> rb;
        for (const auto& b : fb) rb.push_back({b.name, b.parent});
        const auto map = MapRig(rb);
        out = "rig: " + DetectRig(rb) + "\n";
        int mapped = 0, skinned = 0, skinned_mapped = 0;
        for (size_t k = 0; k < fb.size(); ++k) {
          mapped += !map[k].empty(), skinned += fb[k].skinned, skinned_mapped += fb[k].skinned && !map[k].empty();
          out += "  " + fb[k].name + (fb[k].skinned ? " *" : "") + " -> " + (map[k].empty() ? "(folds into its parent)" : map[k]) + "\n";
        }
        out += std::to_string(fb.size()) + " bones, " + std::to_string(mapped) + " mapped; " + std::to_string(skinned) +
               " with weights, " + std::to_string(skinned_mapped) + " of them mapped\n";
      } else {
        out = "FAILED: " + err + "\n";
      }
      if (c != std::wstring::npos) WriteFile(Utf8(v.substr(c + 1)), Bytes(out.begin(), out.end()));
      return 0;
    }
    else if (!wcscmp(argv[i], L"--export-fbx") && more) {  // <pac>,<attire>,<out dir>,<name>: Export to FBX, then quit
      std::vector<std::wstring> p;
      const std::wstring v = argv[++i];
      for (size_t at = 0; at <= v.size();) {
        size_t c = v.find(L',', at);
        if (c == std::wstring::npos) c = v.size();
        p.push_back(v.substr(at, c - at)), at = c + 1;
      }
      Bytes b;
      svrfmt::CharExportReport rep;
      const bool ok = p.size() == 4 && ReadFile(Utf8(p[0]), b) && svrfmt::ExportCharacterFbx(b, _wtoi(p[1].c_str()), Utf8(p[2]), Utf8(p[3]), rep);
      std::string t = std::string(ok ? "ok" : "failed") + ": " + std::to_string(rep.bones) + " bones\n";
      for (const auto& w : rep.warnings) t += w + "\n";
      if (p.size() == 4) WriteFile(Utf8(p[2]) + "/export.txt", Bytes(t.begin(), t.end()));
      return ok ? 0 : 1;
    }
    else if (!wcscmp(argv[i], L"--fbx-build-input") && more) {  // <fbx>,<template pac>,<out dir>: the automatic mapping, build.json + report.txt
      const std::wstring v = argv[++i];
      const size_t a = v.find(L','), b = v.find(L',', a + 1);
      if (a == std::wstring::npos || b == std::wstring::npos) return 1;
      const std::string fbx = Utf8(v.substr(0, a)), tpl = Utf8(v.substr(a + 1, b - a - 1));
      std::string dir = Utf8(v.substr(b + 1));
      // (test: "<dir>|<mesh>=spec:1.5&tint:1,0.3,0.3&cut:1&both:1&flip:1&alpha:flat&normal:<png>&shine:<png>;..." - the builder's per-mesh material)
      svrfmt::FbxImportOptions mopts;
      if (const wchar_t* r = _wgetenv(L"SVR2011_TEST_FBX_REDUCE")) mopts.max_vertices = _wtoi(r), mopts.keep_face = !wcsstr(r, L"all");  // ("<n>[all]")
      if (const wchar_t* h = _wgetenv(L"SVR2011_TEST_FBX_HAIR")) mopts.hair = _wtoi(h);  // (-1 auto, 0, 1)
      if (const size_t bar = dir.find('|'); bar != std::string::npos) {
        const std::string spec = dir.substr(bar + 1);
        dir.resize(bar);
        for (size_t at = 0; at < spec.size();) {
          size_t e = spec.find(';', at);
          if (e == std::string::npos) e = spec.size();
          const std::string item = spec.substr(at, e - at);
          at = e + 1;
          const size_t eq = item.find('=');
          if (eq == std::string::npos) continue;
          svrfmt::FbxMaterial& f = mopts.material[item.substr(0, eq)];
          std::string rest = item.substr(eq + 1);
          for (size_t p = 0; p <= rest.size();) {
            size_t q = rest.find('&', p);
            if (q == std::string::npos) q = rest.size();
            const std::string kv = rest.substr(p, q - p);
            p = q + 1;
            const size_t c = kv.find(':');
            if (c == std::string::npos) continue;
            const std::string k = kv.substr(0, c), val = kv.substr(c + 1);
            if (k == "spec") f.specular = float(std::atof(val.c_str()));
            if (k == "tint") f.has_tint = std::sscanf(val.c_str(), "%f,%f,%f", &f.tint[0], &f.tint[1], &f.tint[2]) == 3;
            if (k == "cut") f.cutout = std::atoi(val.c_str());
            if (k == "both") f.double_sided = std::atoi(val.c_str());
            if (k == "flip") f.flip = std::atoi(val.c_str());
            if (k == "shine") f.shine_mask = val;
            if (k == "alpha") f.specular_alpha = val;
            if (k == "normal") f.normal = val;
          }
        }
      }
      std::vector<svrfmt::FbxBone> fb;
      std::string err, out;
      Bytes pac;
      svrfmt::FbxImportReport rep;
      bool ok = false;
      if (svrfmt::ReadFbxSkeleton(fbx, fb, err) && ReadFile(tpl, pac)) {
        std::vector<RigBone> rb;
        for (const auto& k : fb) rb.push_back({k.name, k.parent});
        ok = svrfmt::FbxToBuildInput(fbx, MapRig(rb), pac, tpl, dir, rep, mopts);
      }
      char line[256];
      std::snprintf(line, sizeof line, "%s: %d meshes, %d vertices, %d triangles, %d textures (sheet %d), %d bones aimed (worst %.3f deg), %d 2011 bones, scale %.4f, height %.1f cm\n",
                    ok ? "OK" : "FAILED", rep.meshes, rep.vertices, rep.triangles, rep.textures, rep.atlas, rep.aimed, rep.aim_error_deg, rep.bones_used, rep.scale, rep.height_cm);
      if (rep.reduced_from)
        out += "  reduced from " + std::to_string(rep.reduced_from) + " vertices (" + std::to_string(rep.cross_merges) +
               " merges across bones)\n";
      for (const auto& w : rep.reduced_lines) out += "  reduced: " + w + "\n";
      out = out + line + err + (rep.aim_worst.empty() ? "" : "  worst aim: " + rep.aim_worst + "\n");
      for (const auto& w : rep.warnings) out += "  warning: " + w + "\n";
      for (const auto& w : rep.skipped) out += "  skipped: " + w + "\n";
      for (const auto& w : rep.mesh_lines) out += "  mesh: " + w + "\n";
      std::error_code ec;
      fs::create_directories(fs::u8path(dir), ec);
      WriteFile(dir + "/report.txt", Bytes(out.begin(), out.end()));
      return ok ? 0 : 1;
    }
    else if (!wcscmp(argv[i], L"--model-check") && more) {  // <pac>[;<pac>...],<out>: the D12 model checks, a line per pac
      const std::wstring v = argv[++i];
      const size_t c = v.rfind(L',');
      if (c == std::wstring::npos) return 1;
      std::string out;
      std::wstring list = v.substr(0, c);
      for (size_t a = 0; a <= list.size();) {
        size_t b = list.find(L';', a);
        if (b == std::wstring::npos) b = list.size();
        const std::wstring pac = list.substr(a, b - a);
        a = b + 1;
        Bytes d;
        const auto is = ReadFile(Utf8(pac), d) ? svrfmt::CheckCharacterModel(d, 1) : svrfmt::CharModelIssues();
        char line[256];
        std::snprintf(line, sizeof line, "%s: read %d, nan %d, eyes %.3f %.3f, hair %d, attire clash %d\n", FileName(pac).c_str(), int(is.read), int(is.nan_meshes.size()), is.eye_off[0], is.eye_off[1], int(is.hair.size()), int(is.attire_clash.size()));
        out += line;
      }
      WriteFile(Utf8(v.substr(c + 1)), Bytes(out.begin(), out.end()));
      return 0;
    }
    else if (!wcscmp(argv[i], L"--check-mods") && more) {  // <out>: every installed mod checked (mod_check), then quit (--game first)
      const std::wstring out = argv[++i];
      std::string text;
      auto put = [&](const std::string& who, const std::vector<Problem>& p) {
        for (const auto& x : p) text += who + (x.error ? " | MUST FIX | " : " | check | ") + x.text + (x.fix.empty() ? "" : " | " + x.fix) + "\n";
      };
      put("(set)", CheckInstalledSet());
      std::error_code ec;
      for (const char* kind : {"Superstars", "Attires", "Arenas", "Moves", "Backstage", "Signs", "Media", "Backgrounds", "MatchTypes"})
        for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / kind, ec))
          if (e.is_directory(ec) && e.path().filename().wstring()[0] != L'.') put(std::string(kind) + "/" + Utf8(e.path().filename().wstring()), CheckModFolder(kind, e.path()));
      WriteFile(Utf8(out), Bytes(text.begin(), text.end()));
      return 0;
    }
    else if (!wcscmp(argv[i], L"--hair-ref") && more) {  // <dump>,<out>[,<floor y>]: the hair solver against the game's numbers, then quit
      std::wstring v = argv[++i];
      std::vector<std::wstring> p;
      for (size_t at = 0; at <= v.size();) {
        size_t c = v.find(L',', at);
        if (c == std::wstring::npos) c = v.size();
        p.push_back(v.substr(at, c - at)), at = c + 1;
      }
      Bytes d;
      std::string rep = p.size() >= 2 && ReadFile(Utf8(p[0]), d)
                            ? hair::ReplayDump(std::string(d.begin(), d.end()), p.size() > 2 ? float(_wtof(p[2].c_str())) : -12.0f)
                            : "can't read the dump\n";
      if (p.size() >= 2) WriteFile(Utf8(p[1]), Bytes(rep.begin(), rep.end()));
      return 0;
    }
    else if (!wcscmp(argv[i], L"--check-svrmod") && more) {  // <file>,<out>: a .svrmod checked, then quit
      const std::wstring v = argv[++i];
      const size_t c = v.rfind(L',');
      std::string text;
      for (const auto& x : CheckSvrmod(v.substr(0, c))) text += std::string(x.error ? "MUST FIX | " : "check | ") + x.text + "\n";
      if (text.empty()) text = "clean\n";
      if (c != std::wstring::npos) WriteFile(Utf8(v.substr(c + 1)), Bytes(text.begin(), text.end()));
      return 0;
    }
    else if (!wcscmp(argv[i], L"--rig-selftest")) {  // the rig mapper against each known rig's standard names
      const auto fails = RigSelfTest();
      std::string out = "rig self test: " + std::to_string(fails.size()) + " failed\n";
      for (const auto& f : fails) out += "  " + f + "\n";
      if (++i < argc) WriteFile(Utf8(argv[i]), Bytes(out.begin(), out.end()));
      return fails.empty() ? 0 : 1;
    }
    else if (!wcscmp(argv[i], L"--export-char-fbx") && more) {  // <pac>,<attire>,<out dir>: export, read back, log, quit
      const std::wstring v = argv[++i];
      const size_t a = v.find(L','), b = v.find(L',', a + 1);
      if (a != std::wstring::npos && b != std::wstring::npos) {
        Bytes pac;
        svrfmt::CharExportReport rep;
        const std::wstring out = v.substr(b + 1);
        const int attire = _wtoi(v.substr(a + 1, b - a - 1).c_str());
        const bool ok = ReadFile(Utf8(v.substr(0, a)), pac) && svrfmt::ExportCharacterFbx(pac, attire, Utf8(out), "test", rep);
        Log(std::string("test: export ") + (ok ? "ok" : "FAILED") + ": " + std::to_string(rep.bones) + " bones, " + std::to_string(rep.meshes) +
            " meshes, " + std::to_string(rep.vertices) + " vertices, " + std::to_string(rep.triangles) + " triangles, " +
            std::to_string(rep.textures) + " textures");
        for (const auto& w : rep.warnings) Log("  " + w);
        if (ok)
          for (const auto& l : svrfmt::CheckCharacterFbx(pac, attire, Utf8(out) + "\\test.fbx")) Log("test: read back: " + l);
        WriteLogTo(out + L"\\test.log");
        return 0;
      }
    }
    else if (!wcscmp(argv[i], L"--installed") && more) installed_page::TestSelect(Utf8(argv[++i])), g_page = PageId::kInstalled;
    else if (!wcscmp(argv[i], L"--full-preview") && more) {  // <entrance>,<star>[,<key>[,<camera 0/1>]]: the Full preview
      int e = -1, s = -1, cam = 1;
      float key = -1;
      std::swscanf(argv[++i], L"%d,%d,%f,%d", &e, &s, &key, &cam);
      full_preview::TestOpen(e, s, key, cam != 0);
    }
    else if (!wcscmp(argv[i], L"--full-preview-song") && more) full_preview::TestSong(argv[++i]);
    else if (!wcscmp(argv[i], L"--full-preview-model") && more) full_preview::TestModel(argv[++i]);
    else if (!wcscmp(argv[i], L"--full-preview-movie") && more) full_preview::TestMovie(argv[++i]);
    else if (!wcscmp(argv[i], L"--full-preview-dump") && more) full_preview::TestDump(argv[++i]);
    else if (!wcscmp(argv[i], L"--pose-dump-motion") && more) char_preview::g_test_pose_dump = argv[++i];
    else if (!wcscmp(argv[i], L"--full-preview-cameras") && more) full_preview::TestCameras(argv[++i]);
    else if (!wcscmp(argv[i], L"--full-preview-view") && more) full_preview::TestView(argv[++i]);
    else if (!wcscmp(argv[i], L"--moves-put") && more) moves_page::TestPut(argv[++i]), g_page = PageId::kMoves;
    else if (!wcscmp(argv[i], L"--pac-attires") && more) attire_page::TestPacAttires(argv[++i]), g_page = PageId::kAttire;
    else if (!wcscmp(argv[i], L"--pose-compare") && more) anims_page::TestCompare(argv[++i]), g_page = PageId::kAnims;
    else if (!wcscmp(argv[i], L"--view-attire") && more) char_preview::SetAttire(_wtoi(argv[++i]));  // the 3D view's attire
    else if (!wcscmp(argv[i], L"--anims") && more) {  // <star id>,<bank index>,<motion id>[,<dummy id>[,<key: paused there>]]
      int a[5] = {0, -1, -1, 0, -1};
      swscanf_s(argv[++i], L"%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4]);
      anims_page::TestStart(a[0], a[1], a[2], a[3]);
      anims_page::TestKey(a[4]);
      g_page = PageId::kAnims;
    }
    else if (!wcscmp(argv[i], L"--page-name") && more) page_name = argv[++i];  // arena editor backstage superstar moves signs media assets anims icons help
    else if (!wcscmp(argv[i], L"--select") && more) start_select = argv[++i];
    else if (!wcscmp(argv[i], L"--test-picture") && more) editor::TestPicture(Utf8(argv[++i]));
    else if (!wcscmp(argv[i], L"--test-add-object") && more) test_obj_add = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-export-object") && more) test_obj_export = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-remove-part") && more) test_obj_remove = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-edit-save") && more) test_save = argv[++i];
    else if (!wcscmp(argv[i], L"--new-arena") && more) start_new = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-lib") && more) test_lib = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--page") && more) start_page = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--backstage") && more) start_backstage = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star") && more) star_id = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-song") && more) star_song = argv[++i];
    else if (!wcscmp(argv[i], L"--star-movie") && more) star_movie = argv[++i];
    else if (!wcscmp(argv[i], L"--star-picture") && more) star_picture = argv[++i];
    else if (!wcscmp(argv[i], L"--star-full") && more) star_page::TestFull(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-load") && more) star_page::TestLoad(argv[++i]);  // the loading picture (fitted to 1024 x 512)
    else if (!wcscmp(argv[i], L"--star-hometown") && more) star_page::TestHometown(Utf8(argv[++i]));
    else if (!wcscmp(argv[i], L"--star-pose") && more) star_page::TestPose(_wtoi(argv[++i]));
    else if (!wcscmp(argv[i], L"--star-fbx-make")) star_page::TestFbxMake();
    else if (!wcscmp(argv[i], L"--fbx-report") && more) star_page::TestFbxReport(argv[++i]);  // with --star-fbx: the import report, quit  // with --star-fbx: Make the model, log, quit
    else if (!wcscmp(argv[i], L"--simple-take") && more) {  // <svrmod>,<0|1>: a converted superstar's look (0) or all of it (1) into the project
      const std::wstring v = argv[++i];
      const size_t c = v.rfind(L',');
      star_page::TestSimpleTake(v.substr(0, c), c != std::wstring::npos && v.substr(c + 1) == L"1");
    }
    else if (!wcscmp(argv[i], L"--simple-ring") && more) star_page::TestSimpleRing(_wtoi(argv[++i]));  // an entrance's walk to the ring in Simple's preview
    else if (!wcscmp(argv[i], L"--simple-capture") && more) star_page::TestSimpleCapture(argv[++i]);  // the 3D view as the select picture -> <png>, quit
    else if (!wcscmp(argv[i], L"--simple-look") && more) star_page::TestSimpleLook(_wtoi(argv[++i]));  // Simple's Look step on way <n>
    else if (!wcscmp(argv[i], L"--attire-pick") && more) simple_port::AttireTest(Utf8(argv[++i]));  // open | <n,n,...>: the attire picker
    else if (!wcscmp(argv[i], L"--look-tab") && more) star_page::TestLookTab(_wtoi(argv[++i]));  // Simple's Look step: 0 Model, 1 Materials
    else if (!wcscmp(argv[i], L"--fbx-step") && more) star_page::TestFbxStep(_wtoi(argv[++i]) - 1);  // the FBX import's step <1-5>
    else if (!wcscmp(argv[i], L"--simple") && more) {  // the Superstar page in Simple mode at <step>[,from]: that step's "From another game"
      const std::wstring a = argv[++i];
      star_page::TestSimple(_wtoi(a.c_str()));
      if (a.find(L",from") != std::wstring::npos) star_page::TestSimpleFrom(_wtoi(a.c_str()));
    }
    else if (!wcscmp(argv[i], L"--simple-chain") && more) {  // "<kind>:<game>:<item>;...;save=<file>" (simple_star.cpp)
      star_page::TestSimple(0);
      star_page::TestSimpleChain(Utf8(argv[++i]));
      g_page = PageId::kStar;
    }
    else if (!wcscmp(argv[i], L"--simple-arena") && more) arena_page::TestSimple(_wtoi(argv[++i])), g_page = PageId::kArena;  // Simple arena at <step>
    else if (!wcscmp(argv[i], L"--simple-arena-start") && more) arena_page::TestSimpleStart(Utf8(argv[++i])), g_page = PageId::kArena;  // tile:<n> | empty:<n> | disc:<key>[:<bgNN>[:<host>]]
    else if (!wcscmp(argv[i], L"--discs-find") && more) disc_list::TestFind(argv[++i]), g_page = PageId::kDiscs;  // <folder>[,<report>]: Find my discs
    else if (!wcscmp(argv[i], L"--simple-port") && more) {  // <entrance|moves|taunts|import>,<game key>[,<item>[,<step>[,add]]]
      const std::string spec = Utf8(argv[++i]);
      simple_port::TestStart(spec);
      if (spec.rfind("import", 0) == 0) star_page::TestSimple(1), star_page::TestSimpleLook(1), g_page = PageId::kStar;  // (Simple superstar's Look step)
      else g_page = spec.rfind("moves", 0) == 0 ? PageId::kPortMoves : spec.rfind("taunts", 0) == 0 ? PageId::kPortTaunts : PageId::kPortEntrance;
    }
    else if (!wcscmp(argv[i], L"--star-fbx") && more) {  // <fbx>[,<fbx_map= text>]: the FBX import's mapping table
      const std::wstring v = argv[++i];
      const size_t c = v.find(L',');
      star_page::TestFbx(v.substr(0, c), c == std::wstring::npos ? std::string() : Utf8(v.substr(c + 1)));
    }
    else if (!wcscmp(argv[i], L"--star-call") && more) star_call = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-height") && more) star_height = float(_wtof(argv[++i]));
    else if (!wcscmp(argv[i], L"--star-call-gain") && more) star_call_gain = float(_wtof(argv[++i]));
    else if (!wcscmp(argv[i], L"--star-moveset") && more) star_moveset = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--threads-edit") && more) star_page::ThreadsTest(Utf8(argv[++i]));  // <attire>[,make|name][,save]
    else if (!wcscmp(argv[i], L"--star-cae") && more) star_page::TestCae(Utf8(argv[++i]));  // a created entrance (Mods\.cae id)
    else if (!wcscmp(argv[i], L"--star-import-moveset") && more) star_import_moveset = Utf8(argv[++i]);  // star:<id> | mod:<folder>
    else if (!wcscmp(argv[i], L"--star-entrance") && more) star_entrance = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-victory") && more) star_victory = _wtoi(argv[++i]);  // (victory=, the section open)
    else if (!wcscmp(argv[i], L"--star-victory-port") && more) star_page::TestVictoryPort(Utf8(argv[++i]));  // (<game>,<n>: converted)
    else if (!wcscmp(argv[i], L"--run-fix") && more) g_test_fix = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--install-star")) g_test_install_frames = 0;  // "<text>[|<n>]" (see g_test_fix)
    else if (!wcscmp(argv[i], L"--show-problems")) g_show_problems = true;
    else if (!wcscmp(argv[i], L"--blood")) char_preview::SetBlood(true);
    else if (!wcscmp(argv[i], L"--gear")) char_preview::SetGear(true);  // entrance gear shown in the 3D views
    else if (!wcscmp(argv[i], L"--ring")) ring_view = true;
    else if (!wcscmp(argv[i], L"--no-physics")) char_preview::SetPhysics(false);  // hair and jiggle off (test aid)  // the basic ring in the 3D views (once the game folder is known)
    else if (!wcscmp(argv[i], L"--star-gear-fbx") && more) star_page::TestGearFbx(Utf8(argv[++i]));  // <attire>:<fbx>: fit the FBX gear, log, quit
    else if (!wcscmp(argv[i], L"--star-gear-from") && more) star_page::TestGearFrom(Utf8(argv[++i]));  // <attire>:<donor id>: fit the donor's entrance gear, log, quit
    else if (!wcscmp(argv[i], L"--star-gear") && more) star_page::TestGear(Utf8(argv[++i]));  // <attire>:<off|from<k>>[,...]
    else if (!wcscmp(argv[i], L"--hits")) char_preview::SetHits(true);
    else if (!wcscmp(argv[i], L"--attire-keep-entrance")) attire_page::TestKeepEntrance();  // (--attire from a superstar mod: its entrance too)
    else if (!wcscmp(argv[i], L"--attire-from") && more) {  // <n>[,<name>]: --attire takes the source's attire n (and names it)
      const std::string v = Utf8(argv[++i]);
      const size_t c = v.find(',');
      attire_page::TestFrom(std::atoi(v.c_str()), c == std::string::npos ? "" : v.substr(c + 1));
    }
    else if (!wcscmp(argv[i], L"--attire") && more) {  // <star id>,<slot 0=new>,<model pac | superstar mod folder / .svrmod / .svrproj>[,<save .svrmod>]
      const std::wstring a = argv[++i];
      const size_t c1 = a.find(L','), c2 = a.find(L',', c1 + 1), c3 = a.find(L',', c2 + 1);
      attire_page::TestStart(_wtoi(a.c_str()), a.substr(c2 + 1, c3 == std::wstring::npos ? std::wstring::npos : c3 - c2 - 1),
                             _wtoi(a.c_str() + c1 + 1), c3 == std::wstring::npos ? L"" : a.substr(c3 + 1));
      g_page = PageId::kAttire;
    }
    else if (!wcscmp(argv[i], L"--star-watch") && more) star_page::TestWatchSlot(int(std::wcstol(argv[++i], nullptr, 0)));
    else if (!wcscmp(argv[i], L"--star-voice") && more) star_voice = argv[++i];
    else if (!wcscmp(argv[i], L"--star-model") && more) star_model = argv[++i];
    else if (!wcscmp(argv[i], L"--star-mat") && more) star_page::TestMaterials(Utf8(argv[++i]), false);
    else if (!wcscmp(argv[i], L"--open-materials")) star_page::TestMaterials("", true);
    else if (!wcscmp(argv[i], L"--mat-select") && more) MaterialTestSelect(Utf8(argv[++i])), star_page::TestMaterials("", true);
    else if (!wcscmp(argv[i], L"--tools-panel")) TestToolsPanel(false, L"");
    else if (!wcscmp(argv[i], L"--tools-install")) TestToolsPanel(true, L"");
    else if (!wcscmp(argv[i], L"--tools-install-from") && more) TestToolsPanel(false, argv[++i]);
    else if (!wcscmp(argv[i], L"--alpha-fixture") && more) {  // <pac>,<out pac>,<id:mesh>;<id:mesh>...: the cut-out fixture (mm::AlphaFixture), then quit
      const std::wstring v = argv[++i];
      const size_t a = v.find(L','), b = v.find(L',', a + 1);
      if (a == std::wstring::npos || b == std::wstring::npos) return 1;
      std::vector<std::pair<uint32_t, int>> ms;
      for (const std::string item : {Utf8(v.substr(b + 1))}) {
        size_t at = 0;
        while (at < item.size()) {
          size_t e = item.find(';', at);
          if (e == std::string::npos) e = item.size();
          unsigned id = 0;
          int mesh = 0;
          if (std::sscanf(item.substr(at, e - at).c_str(), "%u:%d", &id, &mesh) == 2) ms.push_back({id, mesh});
          at = e + 1;
        }
      }
      svrfmt::Bytes pac;
      std::string err;
      const bool ok = svrfmt::ReadFile(Utf8(v.substr(0, a)), pac) && mm::AlphaFixture(pac, ms, &err) &&
                      svrfmt::WriteFile(Utf8(v.substr(a + 1, b - a - 1)), pac);
      if (!ok) WriteFile(Utf8(v.substr(a + 1, b - a - 1)) + ".err", Bytes(err.begin(), err.end()));
      return ok ? 0 : 1;
    }
    else if (!wcscmp(argv[i], L"--mat-dump") && more) {  // <pac>,<out.txt>: every mesh's shader, flags and parameters, then quit
      const std::wstring a = argv[++i];
      const size_t c = a.find(L',');
      svrfmt::Bytes pac;
      if (c != std::wstring::npos && svrfmt::ReadFile(Utf8(a.substr(0, c)), pac)) {
        const std::string t = mm::DumpMaterials(pac);
        if (FILE* f = _wfopen(a.substr(c + 1).c_str(), L"wb")) std::fwrite(t.data(), 1, t.size(), f), std::fclose(f);
      }
      return 0;
    }
    else if (!wcscmp(argv[i], L"--star-name") && more) star_name = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--sign") && more) sign_files.push_back(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-sign-save") && more) sign_save = argv[++i];
    else if (!wcscmp(argv[i], L"--background") && more) background_files.push_back(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-background-save") && more) background_save = argv[++i];
    else if (!wcscmp(argv[i], L"--media-arena") && more) media_arena = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--media-video") && more) media_video = argv[++i];
    else if (!wcscmp(argv[i], L"--media-picture") && more) media_pictures.push_back(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-media-save") && more) media_save = argv[++i];
    else if (!wcscmp(argv[i], L"--test-star-save") && more) star_save = argv[++i];
    else if (argv[i][0] != L'-') project = argv[i];  // a project or mod file (double-clicked)
  }
  // The old numbering of --page (0 arenas, 1 editor, 2 VS screen, 3 superstars, 4 signs, 5-7 media).
  const PageId page_of[] = {PageId::kArena, PageId::kEditor, PageId::kArena, PageId::kStar,
                            PageId::kSigns, PageId::kMedia,  PageId::kMedia, PageId::kMedia};
  if (g_game.empty()) g_game = g_settings.game;
  if (!g_game.empty() && !fs::exists(fs::path(g_game) / L"pac" / L"bg")) g_game.clear();

  WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                    nullptr, nullptr, L"SvR2011ModMaker", nullptr};
  RegisterClassExW(&wc);
  const UINT dpi = GetDpiForSystem();
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = MulDiv(1360, dpi, 96), h = MulDiv(860, dpi, 96);
  if (g_settings.win_w > 400 && g_settings.win_h > 300) {
    x = g_settings.win_x, y = g_settings.win_y, w = g_settings.win_w, h = g_settings.win_h;
    // (still on a screen?)
    const RECT r = {x, y, x + w, y + h};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
  }
  if (behind) x = 0, y = 0, w = MulDiv(1280, dpi, 96), h = MulDiv(800, dpi, 96);
  if (const wchar_t* size = _wgetenv(L"SVR2011_MM_SHOT_SIZE"); behind && size && *size) {  // (test shots: "<w>x<h>")
    int sw = 0, sh = 0;
    if (swscanf_s(size, L"%dx%d", &sw, &sh) == 2 && sw >= 640 && sh >= 480) w = MulDiv(sw, dpi, 96), h = MulDiv(sh, dpi, 96);
  }
  g_wnd = CreateWindowExW(behind ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, wc.lpszClassName, L"SvR2011 Mod Maker",
                          WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
  DragAcceptFiles(g_wnd, TRUE);  // (pictures dropped on the pages' drop targets)
  if (!CreateDevice(g_wnd)) {
    if (!behind) MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"SvR2011 Mod Maker", MB_ICONERROR);
    return 1;
  }
  if (behind) {
    // never shown: nothing on any screen, nothing on the taskbar (the swap chain still renders)
  } else {
    ShowWindow(g_wnd, g_settings.maximized && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_wnd);
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  {
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    const std::string seg = Utf8(std::wstring(fonts) + L"\\Fonts\\segoeui.ttf");
    if (fs::exists(seg)) io.Fonts->AddFontFromFileTTF(seg.c_str(), g_settings.font * dpi / 96.0f);
  }
  g_scale = g_settings.font * dpi / 96.0f / 17.0f;
  Style();
  ImGui::GetStyle().ScaleAllSizes(g_scale);
  ImGui_ImplWin32_Init(g_wnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);
  if (g_game.empty()) {
    Status("Choose the folder where the game is installed (it has the pac folder).");
    g_game = PickFolder(L"The installed game folder (with pac\\bg)");
    if (!g_game.empty() && !fs::exists(fs::path(g_game) / L"pac" / L"bg")) g_game.clear();
    if (!g_game.empty()) g_settings.game = g_game, SaveSettings();
  }
  if (!g_game.empty() && LoadBanners()) Status("Game folder: " + Utf8(g_game));
  else if (!g_game.empty()) Status("The game folder's menu files could not be read: " + Utf8(g_game));
  else Status("No game folder: nothing can be read. Click the game folder button at the top right.");
  StartupToolsCheck();  // (Python, numpy, Pillow, the scripts, svrmod.exe: the panel opens when one is missing)
  editor::Hooks hooks;
  hooks.log = [](const std::string& s) { Log(s); };
  hooks.test_in_game = [] { arena_page::TestInGame(); };
  hooks.pick_picture = [](const char* title) { return Utf8(PickFile(false, Wide(title).c_str(), kPictureFilter, 1)); };
  if (!g_game.empty())
    for (int i = 0; i < 20; ++i) hooks.library.push_back({g_arenas[i].name, ArenaPath(i)});
  editor::Init(g_dev, g_ctx, hooks);
  char_preview::Init(g_dev, g_ctx);
  ProjectReset();

  // the test aids
  star_page::TestStart(star_id, star_model, star_name, star_picture, star_save);
  if (ring_view) char_preview::SetRing(true, Utf8((fs::path(g_game) / L"pac" / L"bg" / L"bg29.pac").wstring()));
  if (star_call >= 0 || !star_song.empty() || !star_movie.empty() || !star_voice.empty())
    star_page::TestFiles(star_song, star_movie, star_voice, star_call);
  if (star_height > 0) star_page::TestHeight(star_height);
  if (star_call_gain != 0) star_page::TestCallGain(star_call_gain);
  if (!star_moveset.empty()) star_page::TestMoveset(star_moveset);
  if (!star_import_moveset.empty()) star_page::TestImportMoveset(star_import_moveset);
  if (star_entrance) star_page::TestEntrance(star_entrance);
  if (star_victory) star_page::TestVictory(star_victory);
  signs_page::TestStart(sign_files, sign_save);
  backgrounds_page::TestStart(background_files, background_save);
  media_page::TestStart(media_arena, media_video, media_pictures, media_save);
  arena_page::SetTestSave(test_save, test_lib);
  if (!test_obj_add.empty() || !test_obj_export.empty() || test_obj_remove >= 0)
    editor::TestObjects(test_obj_add, test_obj_export, test_obj_remove);
  if (start_new >= 0 && start_new < 20) arena_page::StartNew(start_new);
  if (start_editor >= 0 && start_editor < 20) arena_page::OpenEditor(start_editor);
  editor::TestStart(start_view, Utf8(start_select));  // (before the arena is set)
  if (start_page >= 0 && start_page < 8 && start_mod.empty()) g_page = page_of[start_page];
  if (start_page >= 5 && start_page < 8) media_page::SetTab(start_page - 5);
  if (start_backstage >= 0 && start_backstage < 7) arena_page::StartBackstage(start_backstage);
  if (!start_mod.empty()) {
    arena_page::OpenModFile(start_mod);
    if (start_page >= 0 && start_page < 8) g_page = page_of[start_page];
  }
  if (!moves_pack.empty()) moves_page::TestOpen(moves_pack), g_page = PageId::kMoves;
  if (!moves_save.empty()) {
    moves_page::TestSave(moves_save);
    WriteLogTo(moves_save + L".log");
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  }
  if (!make_bink_in.empty()) MakeBink(make_bink_in, make_bink_out, 0, 3, [](std::wstring out) { Status(out.empty() ? "test: bink failed" : "test: bink made"); });
  if (!run_tool_args.empty()) {
    const std::wstring script = run_tool_args[0];
    run_tool_args.erase(run_tool_args.begin());
    RunTool(script, run_tool_args, [](int code) { Status("test: tool exit code " + std::to_string(code)); });
  }
  if (!page_name.empty()) PageByName(page_name, g_page);
  if (!project.empty()) ProjectOpen(project);
  // test aid: --test-project <file.svrproj>: the shown page saved as a project, everything
  // reset, the project opened again; its state text goes to <file>.txt, the log to <file>.log
  bool test_project_pending = !test_project.empty();
  // test aid: --test-roundtrip <in.txt>: a project's keys (type= first) read into their page, then saved as a project
  // and opened again, and through a mod's .mm_project.txt (a bare manifest + the file) opened again; each state
  // against the first, the lines that differ listed in <in>.out.txt
  bool test_roundtrip_pending = !test_roundtrip.empty();

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (IsIconic(g_wnd)) Sleep(30);  // (minimized: slow down; frames still run for the test aids)
    if (behind && IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);  // (stays unseen)
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (!ui_script.empty()) UiScript(ui_script);
    ImGui::NewFrame();
    // timing (D17): startup to the first frame, a page's first frame, frames over 100 ms
    static PageId timed_page = PageId::kCount;
    static double tour_slowest = 0;
    static int frames_on_page = 0;
    const PageId page_now = g_page;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    Draw();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    if (ImGui::GetFrameCount() == 1) {
      FILETIME created, x1, x2, x3, now;
      GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
      GetSystemTimeAsFileTime(&now);
      const auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      Log("perf: started in " + std::to_string((ticks(now) - ticks(created)) / 10000) + " ms (first frame)");
    }
    if (page_now != timed_page) {
      if (!perf_tour.empty() && timed_page != PageId::kCount)
        Log("perf: " + std::string(PageName(timed_page)) + " slowest frame " + std::to_string(int(tour_slowest)) + " ms");
      timed_page = page_now, frames_on_page = 0, tour_slowest = 0;
      Log("perf: " + std::string(PageName(page_now)) + " first frame " + std::to_string(int(ms)) + " ms");
    } else {
      tour_slowest = std::max(tour_slowest, ms);
      if (ms > 100) Log("perf: slow frame on " + std::string(PageName(page_now)) + ": " + std::to_string(int(ms)) + " ms");
    }
    if (!perf_tour.empty() && ++frames_on_page >= 90 && !Busy()) {  // the next page (the 3D editor needs an arena: skipped)
      int next = int(g_page) + 1;
      if (next == int(PageId::kEditor)) ++next;
      if (next >= int(PageId::kCount)) {
        WriteLogTo(perf_tour);
        perf_tour.clear();
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      } else {
        g_page = PageId(next);
      }
    }
    ImGui::Render();
    if (test_roundtrip_pending && !Busy() && ImGui::GetFrameCount() > 3) {
      test_roundtrip_pending = false;
      std::string report;
      auto diff = [&](const std::string& what, const std::string& a, const std::string& b) {
        auto lines = [](const std::string& s) {
          std::vector<std::string> v;
          for (size_t at = 0; at < s.size();) {
            size_t e = s.find('\n', at);
            if (e == std::string::npos) e = s.size();
            v.push_back(s.substr(at, e - at));
            at = e + 1;
          }
          return v;
        };
        const auto la = lines(a), lb = lines(b);
        int n = 0;
        for (const auto& l : la)
          if (std::find(lb.begin(), lb.end(), l) == lb.end()) report += what + " lost:  " + l + "\n", ++n;
        for (const auto& l : lb)
          if (std::find(la.begin(), la.end(), l) == la.end()) report += what + " new:   " + l + "\n", ++n;
        report += what + (n ? ": " + std::to_string(n) + " line(s) differ\n" : ": same\n");
      };
      Bytes t;
      ProjectIn in;
      if (ReadFile(Utf8(test_roundtrip), t)) in.files.push_back({"project.txt", t});
      const bool read = !in.files.empty() && ProjectOpenIn(in, false, test_roundtrip);
      const PageId page = g_project_page;
      const std::string first = CurrentState(page);
      // as a project
      const std::wstring proj = test_roundtrip + L".svrproj";
      g_project_file = proj;
      g_project_files[page] = proj;
      const bool saved = read && ProjectSave(false);
      ProjectReset();
      const bool opened = saved && ProjectOpen(proj);
      diff("project", first, CurrentState(page));
      // as a mod: the page's .mm_project.txt with a bare manifest (its own keys: type and name)
      ProjectReset();
      ProjectOpen(proj);
      const std::string text = ModProjectText(page);
      PageHooks* h = Hooks(page);
      const std::string man = std::string("type=") + (h && h->type ? h->type : "") + "\nname=" + Value(first, "name") + "\n";
      ProjectIn mod;
      mod.files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
      AddModProject(mod.files, text);
      ProjectReset();
      const bool mod_opened = ProjectOpenIn(mod, true, test_roundtrip + L".svrmod");
      diff("mod", first, CurrentState(page));
      report = std::string("read=") + (read ? "1" : "0") + " saved=" + (saved ? "1" : "0") + " opened=" + (opened ? "1" : "0") +
               " mod_opened=" + (mod_opened ? "1" : "0") + "\n--- .mm_project.txt\n" + text + "--- differences\n" + report +
               "--- first state\n" + first + "\n";
      WriteFile(Utf8(test_roundtrip + L".out.txt"), Bytes(report.begin(), report.end()));
      WriteLogTo(test_roundtrip + L".log");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    if (test_project_pending && !Busy() && ImGui::GetFrameCount() > 3) {
      test_project_pending = false;
      FollowPage();
      const std::string before = CurrentState(g_project_page);
      g_project_file = test_project;
      g_project_files[g_project_page] = test_project;
      const bool saved = ProjectSave(false);
      ProjectReset();
      const bool opened = saved && ProjectOpen(test_project);
      const std::string after = CurrentState(g_project_page);
      const std::string report = std::string("saved=") + (saved ? "1" : "0") + "\nopened=" + (opened ? "1" : "0") +
                                 "\nsame=" + (before == after ? "1" : "0") + "\n--- before\n" + before +
                                 "\n--- after\n" + after + "\n";
      WriteFile(Utf8(test_project + L".txt"), Bytes(report.begin(), report.end()));
      WriteLogTo(test_project + L".log");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    const float clear[4] = {0.086f, 0.090f, 0.106f, 1};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    while (!page_at.empty() && ImGui::GetTime() >= page_at.front().first) {  // (test aid --page-at)
      if (PageByName(page_at.front().second, g_page)) Log("test: page " + Utf8(page_at.front().second));
      page_at.erase(page_at.begin());
    }
    if (!shot.empty() && ImGui::GetTime() >= shot_after && !Busy() && !disc_list::Scanning()) {  // the frame to a PNG, then quit
      ID3D11Texture2D* back = nullptr;
      g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
      D3D11_TEXTURE2D_DESC td;
      back->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
      ID3D11Texture2D* staging = nullptr;
      if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &staging))) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
          Image img;
          img.w = int(td.Width), img.h = int(td.Height);
          img.rgba.resize(size_t(img.w) * img.h * 4);
          for (int yy = 0; yy < img.h; ++yy) {
            std::memcpy(&img.rgba[size_t(yy) * img.w * 4], static_cast<const uint8_t*>(ms.pData) + size_t(yy) * ms.RowPitch, size_t(img.w) * 4);
            for (int xx = 0; xx < img.w; ++xx) img.rgba[(size_t(yy) * img.w + xx) * 4 + 3] = 255;
          }
          g_ctx->Unmap(staging, 0);
          SavePng(Utf8(shot), img);
        }
        staging->Release();
      }
      back->Release();
      WriteLogTo(shot + L".log");
      shot.clear();
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    DrainD3DMessages();
    session::PresentResult(g_swap->Present(1, 0));
  }
  if (g_worker.joinable()) g_worker.join();
  editor::Shutdown();
  char_preview::Shutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
