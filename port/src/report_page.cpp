// WWE SmackDown vs. Raw 2011 - REPORT A PROBLEM inside the game (see report_page.h).

#include "report_page.h"

#include <chrono>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "generated/default/svr2011_init.h"

#include "frame_rate.h"
#include "match_diag.h"
#include "match_record.h"
#include "native/native_renderer.h"

#include "../launcher/report_urls.h"

#if defined(_WIN32)
extern "C" {
#include "report_flow.h"
}
#elif defined(__SWITCH__)
#include "../switch/nx_report.h"
#endif

#ifndef SVR2011_PORT_VERSION
#define SVR2011_PORT_VERSION "dev"
#endif

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// The launcher's questions (launcher/report_flow.c report_kinds / report_happens; the app's ReportForm the same).
constexpr const char* kKindText[] = {"The game crashed (closed by itself)", "The game froze or hung",
                                  "Graphics look wrong", "Sound or music", "Controller or keyboard",
                                  "Online", "Mods", "Saves or created content", "The launcher",
                                  "Something else"};
constexpr const char* kHappensText[] = {"Every time", "Sometimes", "It happened once"};
constexpr const char* kNeedWhat = "Please say what happened first.";  // (report_flow_check's words)

// 2.2.1: the Discord name (the contact) is needed - the server's rule (reports.discord_name, report_send.c
// report_discord_check): 2-32 characters, lowercase letters, digits, "_" and ".", no two dots in a row. Capitals are
// made lower case. "" with the name in *out, else why.
std::string DiscordWhy(const std::string& text, std::string* out) {
  std::string s;
  for (char c : text) s += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  if (s.empty()) return "Add your Discord name - we answer there.";
  if (s.find('@') != std::string::npos) return "Write your Discord name without the @.";
  if (s.find('/') != std::string::npos || s.find(':') != std::string::npos) return "Write your Discord name, not a link.";
  bool ok = s.size() >= 2 && s.size() <= 32 && s.find("..") == std::string::npos;
  for (char c : s) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.');
  if (!ok) return "That isn't a Discord name: 2-32 characters - lowercase letters, digits, _ and . (no spaces).";
  *out = s;
  return {};
}
std::string SavedDiscordName();  // (the last one sent from a form here - below, per platform)

rex::ui::ImGuiDrawer* g_imgui = nullptr;
rex::ui::ImmediateDrawer* g_drawer = nullptr;
rex::ui::Window* g_window = nullptr;
rex::input::InputSystem* g_input = nullptr;
rex::memory::Memory* g_memory = nullptr;
fs::path g_game_dir;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false}, g_open{false}, g_wait_release{false};
std::atomic<bool> g_from_pause{false};

// A test run (svrtest's scripted pad, the SVR2011_TEST_* aids): reports only to a server on this PC.
bool TestRun() {
  static const bool test = std::getenv("SVR2011_INPUT_FILE") || std::getenv("SVR2011_TEST_MATCH") ||
                           std::getenv("SVR2011_TEST_REPORT");
  return test;
}
// Why a test run may not send to `server` ("" when it may).
std::string Refused(const std::string& server) {
  const char* allow = std::getenv("SVR2011_ALLOW_LIVE");
  if (!TestRun() || (allow && std::string(allow) == "1")) return {};
  std::string host = server;
  if (const size_t p = host.find("://"); p != std::string::npos) host = host.substr(p + 3);
  host = host.substr(0, host.find('/'));
  if (!host.empty() && host[0] == '[') host = host.substr(1, host.find(']') - 1);
  else host = host.substr(0, host.find(':'));
  if (host == "127.0.0.1" || host == "localhost" || host == "::1") return {};
  REXLOG_WARN("[svr2011] report: test run - a report to {} refused (not this PC; SVR2011_ALLOW_LIVE=1 lets it through)",
              server);
  return "Test run: a report to a server not on this PC was refused (SVR2011_ALLOW_LIVE=1 lets it through).";
}

// ---------------------------------------------------------------------------
// The screenshot: a PNG (stored deflate - the server takes 5 MB a picture: the frame halved past 1280 wide).

uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

bool WritePng(const fs::path& file, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(w * 3 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (const uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size() || raw.empty();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (raw.empty()) break;
  }
  const uint32_t adler = b << 16 | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
    uint8_t len[4], crc[4];
    Wr32(len, uint32_t(d.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), d.begin(), d.end());
    Wr32(crc, Crc(body.data(), body.size()));
    out.write(reinterpret_cast<const char*>(len), 4);
    out.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
    out.write(reinterpret_cast<const char*>(crc), 4);
  };
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr(13, 0);
  Wr32(ihdr.data(), w), Wr32(ihdr.data() + 4, h);
  ihdr[8] = 8, ihdr[9] = 2;  // (8-bit RGB)
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return bool(out);
}

struct Shot {
  std::vector<uint8_t> rgb;  // (at most 1280 wide)
  uint32_t w = 0, h = 0;
};
Shot Smaller(std::vector<uint8_t>&& rgb, uint32_t w, uint32_t h) {
  const uint32_t f = std::max(1u, (w + 1279) / 1280);
  Shot s;
  if (f == 1) {
    s.rgb = std::move(rgb), s.w = w, s.h = h;
    return s;
  }
  s.w = w / f, s.h = h / f;
  s.rgb.resize(size_t(s.w) * s.h * 3);
  for (uint32_t y = 0; y < s.h; ++y)
    for (uint32_t x = 0; x < s.w; ++x)
      for (int c = 0; c < 3; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < f; ++dy)
          for (uint32_t dx = 0; dx < f; ++dx) sum += rgb[(size_t(y * f + dy) * w + x * f + dx) * 3 + size_t(c)];
        s.rgb[(size_t(y) * s.w + x) * 3 + size_t(c)] = uint8_t(sum / (f * f));
      }
  return s;
}

// ---------------------------------------------------------------------------
// The send (a worker thread): Windows through launcher/report_flow.c, Android through the app's GameReport.java.

struct Answers {
  int kind = 0, happens = 0;
  std::string what, before, contact;
};
struct Send {
  Answers answers;
  std::string server, match_dir, picture;
  bool with_mods = true, with_save = true, pictures_later = false;
};
struct Sent {
  bool done = false, ok = false;
  std::string id, why;
  int seen_before = 0;
  bool account = false;  // (refused for the sign-in or the Discord name: the reason says what to do)
};
std::mutex g_sent_mutex;
Sent g_sent;
std::atomic<uint64_t> g_progress_sent{0}, g_progress_total{0};

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
  std::wstring w(s.size() + 1, L'\0');
  w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()))));
  if (!w.empty()) w.pop_back();
  return w;
}
std::string Narrow(const wchar_t* w) {
  char b[1024] = {};
  WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
  return b;
}
void Progress(void*, uint64_t sent, uint64_t total) { g_progress_sent = sent, g_progress_total = total; }

Sent SendNow(const Send& s) {
  Sent out;
  const std::string refused = Refused(s.server);
  if (!refused.empty()) {
    out.why = refused;
    return out;
  }
  static ReportAnswers a;  // (big: one send at a time)
  std::memset(&a, 0, sizeof a);
  auto put = [](char* d, size_t n, const std::string& v) { std::snprintf(d, n, "%s", v.c_str()); };
  put(a.problem, sizeof a.problem, kKindText[s.answers.kind]);
  put(a.happens, sizeof a.happens, kHappensText[s.answers.happens]);
  put(a.what, sizeof a.what, s.answers.what);
  put(a.before, sizeof a.before, s.answers.before);
  put(a.contact, sizeof a.contact, s.answers.contact);
  const std::wstring game = g_game_dir.wstring(), version = Wide(SVR2011_PORT_VERSION), server = Wide(s.server),
                     match = Wide(s.match_dir), picture = Wide(s.picture);
  ReportFlow f = {};
  f.game_dir = game.c_str();
  f.version = version.c_str();
  f.app = "game";
  f.answers = &a;
  f.server = server.c_str();
  f.match_dir = match.empty() ? nullptr : match.c_str();
  f.with_mods = s.with_mods;
  f.with_save = s.with_save;
  if (!picture.empty()) f.images[0] = picture.c_str(), f.n_images = 1;
  f.pictures_later = s.pictures_later;
  f.progress = Progress;
  // (2.2.1: the launcher's saved sign-in, as leaderboards.cpp and p2p.cpp use it)
  static std::string token;
  token = rex::cvar::Query<std::string>("online_token");
  f.token = token.c_str();
  static ReportResult r;
  out.ok = report_flow_send(&f, &r) != 0;
  out.id = r.id;
  out.seen_before = r.seen_before;
  out.why = r.why[0] ? Narrow(r.why) : "no answer from the server.";
  out.account = r.code[0] != 0;  // (the server's account / Discord refusal: its words say what to do)
  return out;
}
std::string SavedDiscordName() {
  WCHAR w[80];
  report_discord_load(g_game_dir.wstring().c_str(), w, 80);
  return Narrow(w);
}
#elif defined(__ANDROID__)
// The app's class (from the activity's class loader: FindClass on a native thread sees only the system's).
jclass GameReportClass(JNIEnv* env) {
  static jclass cls = nullptr;
  static std::once_flag once;
  std::call_once(once, [env] {
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return;
    jclass activity_class = env->GetObjectClass(activity);
    jobject loader = env->CallObjectMethod(
        activity, env->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jstring name = env->NewStringUTF("io.github.kaikoclanworth1.svr2011.GameReport");
    jobject found = env->CallObjectMethod(
        loader, env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      found = nullptr;
    }
    if (found) cls = static_cast<jclass>(env->NewGlobalRef(found));
    for (jobject o : {static_cast<jobject>(activity_class), loader, static_cast<jobject>(loader_class),
                      static_cast<jobject>(name), found})
      if (o) env->DeleteLocalRef(o);
    env->DeleteLocalRef(activity);
  });
  return cls;
}

Sent SendNow(const Send& s) {
  Sent out;
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jclass cls = env ? GameReportClass(env) : nullptr;
  jmethodID m = cls ? env->GetStaticMethodID(cls, "send",
                                             "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;"
                                             "ZZLjava/lang/String;Ljava/lang/String;ZZ)Ljava/lang/String;")
                    : nullptr;
  if (!m) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
    out.why = "The app can't send reports (GameReport missing).";
    return out;
  }
  const std::string fields[5] = {kKindText[s.answers.kind], kHappensText[s.answers.happens], s.answers.what, s.answers.before,
                                 s.answers.contact};
  jobjectArray arr = env->NewObjectArray(5, env->FindClass("java/lang/String"), nullptr);
  for (int i = 0; i < 5; ++i) {
    jstring v = env->NewStringUTF(fields[i].c_str());
    env->SetObjectArrayElement(arr, i, v);
    env->DeleteLocalRef(v);
  }
  jstring server = env->NewStringUTF(s.server.c_str()), game = env->NewStringUTF(g_game_dir.string().c_str()),
          version = env->NewStringUTF(SVR2011_PORT_VERSION), match = env->NewStringUTF(s.match_dir.c_str()),
          picture = env->NewStringUTF(s.picture.c_str());
  auto res = static_cast<jstring>(env->CallStaticObjectMethod(cls, m, server, game, version, arr, jboolean(s.with_save),
                                                              jboolean(s.with_mods), match, picture,
                                                              jboolean(s.pictures_later), jboolean(TestRun())));
  if (env->ExceptionCheck()) env->ExceptionClear(), res = nullptr;
  std::string r;
  if (res) {
    const char* c = env->GetStringUTFChars(res, nullptr);
    r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(res, c);
    env->DeleteLocalRef(res);
  }
  for (jobject o : {static_cast<jobject>(arr), static_cast<jobject>(server), static_cast<jobject>(game),
                    static_cast<jobject>(version), static_cast<jobject>(match), static_cast<jobject>(picture)})
    env->DeleteLocalRef(o);
  // ("ok\t<id>\t<seen before>" / "fail\t<why>")
  const size_t t1 = r.find('\t'), t2 = t1 == std::string::npos ? t1 : r.find('\t', t1 + 1);
  if (r.rfind("ok\t", 0) == 0) {
    out.ok = true;
    out.id = r.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
    if (t2 != std::string::npos) out.seen_before = std::atoi(r.c_str() + t2 + 1);
  } else {
    out.why = t1 == std::string::npos ? "no answer from the server." : r.substr(t1 + 1);
    out.account = r.rfind("account\t", 0) == 0;  // ("account\t<why>": the server's sign-in / Discord refusal)
  }
  return out;
}
// The Discord name the app remembers (GameReport.discordName(): its settings, as its own report form keeps it).
std::string SavedDiscordName() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jclass cls = env ? GameReportClass(env) : nullptr;
  jmethodID m = cls ? env->GetStaticMethodID(cls, "discordName", "()Ljava/lang/String;") : nullptr;
  if (!m) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
    return {};
  }
  auto res = static_cast<jstring>(env->CallStaticObjectMethod(cls, m));
  if (env->ExceptionCheck()) env->ExceptionClear(), res = nullptr;
  std::string name;
  if (res) {
    const char* c = env->GetStringUTFChars(res, nullptr);
    name = c ? c : "";
    if (c) env->ReleaseStringUTFChars(res, c);
    env->DeleteLocalRef(res);
  }
  return name;
}
#elif defined(__SWITCH__)
void SwitchProgress(uint64_t sent, uint64_t total) { g_progress_sent = sent, g_progress_total = total; }

// The Discord name sent last (UserData/report_discord.txt, the PC launcher's file).
std::string SavedDiscordName() {
  std::ifstream in(g_game_dir / "UserData" / "report_discord.txt");
  std::string name;
  std::getline(in, name);
  while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
  return name;
}
void SaveDiscordName(const std::string& name) {
  std::error_code ec;
  fs::create_directories(g_game_dir / "UserData", ec);
  std::ofstream(g_game_dir / "UserData" / "report_discord.txt", std::ios::binary | std::ios::trunc) << name << "\n";
}

Sent SendNow(const Send& s) {
  Sent out;
  std::string server = s.server;
#if defined(SVR_NX_REPORTS_LIVE)
  if (server.empty()) server = "https://sho-ti.me/svr";
#else
  // A test build sends only where svr2011.toml says, and never to the public server.
  if (server.empty() || server.find("sho-ti.me") != std::string::npos) {
    out.why = "Test build: set online_server in svr2011.toml to a private server.";
    return out;
  }
#endif
  nx_report::Input in;
  const std::u8string dir = g_game_dir.u8string();
  in.game_dir.assign(dir.begin(), dir.end());
  in.server = server;
  in.version = SVR2011_PORT_VERSION;
  in.problem = kKindText[s.answers.kind];
  in.happens = kHappensText[s.answers.happens];
  in.what = s.answers.what;
  in.before = s.answers.before;
  in.contact = s.answers.contact;  // (the Discord name, checked by the page)
  in.token = rex::cvar::Query<std::string>("online_token");
  in.match_dir = s.match_dir;
  in.picture = s.picture;
  in.with_save = s.with_save;
  in.pictures_later = s.pictures_later;
  in.progress = SwitchProgress;
  const nx_report::Output r = nx_report::Send(in);
  out.ok = r.ok;
  out.id = r.id;
  out.seen_before = r.seen_before;
  out.why = r.why.empty() ? "no answer from the server." : r.why;
  out.account = r.account;
  if (r.ok) SaveDiscordName(s.answers.contact);
  return out;
}
#else
Sent SendNow(const Send&) {
  Sent out;
  out.why = "Reports can't be sent from this build.";
  return out;
}
std::string SavedDiscordName() { return {}; }
#endif

void StartSend(Send s) {
  {
    std::lock_guard lock(g_sent_mutex);
    g_sent = {};
  }
  g_progress_sent = 0, g_progress_total = 0;
  std::thread([s = std::move(s)] {
    REXLOG_INFO("[svr2011] report: sending to {} ({}; match {}, mods {}, save {}, picture {})", s.server,
                kKindText[s.answers.kind], s.match_dir.empty() ? "none" : s.match_dir, s.with_mods, s.with_save,
                s.pictures_later ? "later" : s.picture.empty() ? "none" : "screenshot");
    Sent r = SendNow(s);
    if (r.ok) REXLOG_INFO("[svr2011] report: sent - id {} (seen before {})", r.id, r.seen_before);
    else REXLOG_WARN("[svr2011] report: not sent - {}", r.why);
    std::error_code ec;
    if (!s.picture.empty()) fs::remove(fs::u8path(s.picture), ec);
    r.done = true;
    std::lock_guard lock(g_sent_mutex);
    g_sent = std::move(r);
  }).detach();
}

// ---------------------------------------------------------------------------
// The page.

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text, float wrap = 0) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text, nullptr, wrap);
}
ImVec2 TextSize(ImFont* font, float size, const char* text, float wrap = 0) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, wrap, text);
}
float Hint(ImDrawList* dl, float x, float cy, float s, const char* button, ImU32 colour, const char* label) {
  const float r = 13 * s;
  dl->AddCircleFilled(ImVec2(x + r, cy), r, colour, 24);
  const float gs = 16 * s;
  const ImVec2 gz = TextSize(g_menu_font, gs, button);
  Text(dl, g_menu_font, gs, ImVec2(x + r - gz.x * 0.5f, cy - gz.y * 0.5f), IM_COL32(255, 255, 255, 255), button);
  const float ls = 18 * s;
  const ImVec2 lz = TextSize(g_menu_font, ls, label);
  Text(dl, g_menu_font, ls, ImVec2(x + r * 2 + 8 * s, cy - lz.y * 0.5f), IM_COL32(230, 230, 235, 255), label);
  return x + r * 2 + 8 * s + lz.x + 26 * s;
}

// Touches while the page is open (a phone): taps for the next frame, every touch the page's (not the on-screen
// pad's).
std::mutex g_tap_mutex;
std::vector<ImVec2> g_taps;  // (fractions of the window)
std::map<uint32_t, ImVec2> g_downs;
class PageTouch final : public rex::ui::WindowInputListener {
 public:
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    using A = rex::ui::TouchEvent::Action;
    if (!g_open || !g_window) return;
    const float w = float(g_window->GetActualPhysicalWidth()), h = float(g_window->GetActualPhysicalHeight());
    if (w <= 0 || h <= 0) return;
    const ImVec2 at(e.x() / w, e.y() / h);
    std::lock_guard lock(g_tap_mutex);
    if (e.action() == A::kDown) {
      g_downs[e.pointer_id()] = at;
    } else if (e.action() == A::kUp) {
      auto it = g_downs.find(e.pointer_id());
      if (it != g_downs.end() && std::abs(it->second.x - at.x) < 0.04f && std::abs(it->second.y - at.y) < 0.05f)
        g_taps.push_back(at);
      g_downs.erase(e.pointer_id());
    } else if (e.action() == A::kCancel) {
      g_downs.erase(e.pointer_id());
    }
    e.set_handled(true);
  }
};
PageTouch g_touch;

// Test aid: SVR2011_TEST_REPORT_TYPE=<file> - each line written there is typed into the text being edited (as the
// keyboard's characters: the test game's window never has the keyboard's focus), then the line goes.
void TestTyping(ImGuiIO& io) {
  static const std::string file = [] {
    const char* v = std::getenv("SVR2011_TEST_REPORT_TYPE");
    return std::string(v ? v : "");
  }();
  if (file.empty()) return;
  std::ifstream in(file);
  std::string line;
  if (!std::getline(in, line)) return;
  std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::ofstream(file, std::ios::trunc) << rest;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  io.AddInputCharactersUTF8(line.c_str());
}

enum Row { kKind, kHappens, kWhat, kBefore, kContact, kPicture, kMatch, kMods, kSave, kSend, kRows };
enum Picture { kScreenshot, kLater, kNone };
const char* const kPictureText[] = {"SCREENSHOT OF THIS SCREEN", "ADD PICTURES LATER IN THE LAUNCHER", "NONE"};

// The on-screen keyboard (the controller's): four rows of ten, then SHIFT SPACE DELETE DONE.
constexpr const char* kKeys[] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
constexpr int kKeyRows = 5, kKeyCols = 10;

class ReportPage final : public rex::ui::ImGuiDialog {
 public:
  explicit ReportPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t PadButtons();
  void Open();
  void Close();
  void Activate(int row, int dir);
  void SendIt();
  std::string& Field(int row) { return row == kWhat ? a_.what : row == kBefore ? a_.before : a_.contact; }
  bool DrawEditor(ImGuiIO& io, ImDrawList* dl, float s, const std::function<ImVec2(float, float)>& P, uint16_t pressed,
                  uint16_t act, const std::vector<ImVec2>& taps);
  void KeyPress(int r, int c);

  Answers a_;
  int sel_ = 0, picture_ = kScreenshot;
  bool with_match_ = false, with_mods_ = true, with_save_ = true;
  std::string match_dir_;
  bool from_pause_ = false;
  // the screenshot (from before the menu) and its preview
  bool shot_wanted_ = false;
  Shot shot_;
  std::unique_ptr<rex::ui::ImmediateTexture> preview_;
  // the text being typed (-1: none) and the on-screen keyboard's key
  int editing_ = -1;
  bool focus_edit_ = false, shift_ = false, key_done_ = false;
  int key_r_ = 0, key_c_ = 0;
  char buf_[6000] = {};
  ImGuiKey release_ = ImGuiKey_None;
  Clock::time_point keyboard_until_{};
  // send state
  enum class State { kForm, kSending, kSent } state_ = State::kForm;
  std::string status_;
  bool status_bad_ = false;
  Sent sent_;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{};
};

uint16_t ReportPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE st = {};
  if (!g_input || !g_input->HeldState(0, &st)) return 0;
  uint16_t b = st.gamepad.buttons;
  constexpr int kDead = 16000;
  if (st.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (st.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (st.gamepad.thumb_lx > kDead) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (st.gamepad.thumb_lx < -kDead) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  return b;
}

// The newest recorded match: this run's (the one being played), else the newest in logs\matches.
std::string NewestMatch() {
  std::string dir = svr2011::MatchRecordFolder();
  if (!dir.empty()) return dir;
  std::error_code ec;
  fs::path best;
  for (const auto& e : fs::directory_iterator(g_game_dir / "logs" / "matches", ec))
    if (e.is_directory(ec) && (best.empty() || e.path().filename() > best.filename())) best = e.path();
  const std::u8string u = best.u8string();
  return std::string(u.begin(), u.end());
}

void ReportPage::Open() {
  from_pause_ = g_from_pause.load();
  a_ = {};
  a_.contact = SavedDiscordName();  // (2.2.1: needed - the one sent last, ready)
  sel_ = 0;
  picture_ = kScreenshot;
  with_mods_ = with_save_ = true;
#if defined(__SWITCH__)
  with_mods_ = false;  // (the console's report doesn't send mods)
#endif
  match_dir_ = NewestMatch();
  with_match_ = from_pause_ && !match_dir_.empty();  // (the match being played; from the menus the player picks)
  editing_ = -1;
  state_ = State::kForm;
  status_.clear();
  status_bad_ = false;
  preview_.reset();
  shot_ = {};
  // (from the pause menu: the frame kept at START, before the pause - else the next frame: the menu as it was)
  std::vector<uint8_t> rgb;
  uint32_t w = 0, h = 0;
  shot_wanted_ = !(from_pause_ && svr2011::native::TakeFrameCapture(rgb, w, h));
  if (shot_wanted_) svr2011::native::RequestFrameCapture();
  else shot_ = Smaller(std::move(rgb), w, h);
  wait_release_ = true;  // (the A that opened the page is still down)
  opened_at_ = Clock::now();
  g_open = true;
  REXLOG_INFO("[svr2011] report: page open ({}; match {})", from_pause_ ? "pause menu" : "MY WWE -> OPTIONS",
              match_dir_.empty() ? "none" : match_dir_);
}

void ReportPage::Close() {
  REXLOG_INFO("[svr2011] report: page closed{}", state_ == State::kSent && sent_.ok ? " (sent)" : "");
  preview_.reset();
  shot_ = {};
  editing_ = -1;
  g_wait_release = true;
  g_open = false;
}

void ReportPage::Activate(int row, int dir) {
  const int step = dir < 0 ? -1 : 1;
  switch (row) {
    case kKind: a_.kind = (a_.kind + int(std::size(kKindText)) + step) % int(std::size(kKindText)); break;
    case kHappens: a_.happens = (a_.happens + int(std::size(kHappensText)) + step) % int(std::size(kHappensText)); break;
    case kPicture: picture_ = (picture_ + 3 + step) % 3; break;
    case kMatch: if (!match_dir_.empty()) with_match_ = !with_match_; break;
#if !defined(__SWITCH__)
    case kMods: if (with_match_) with_mods_ = !with_mods_; break;
#endif
    case kSave: with_save_ = !with_save_; break;
    case kWhat:
    case kBefore:
    case kContact:
      if (dir != 0) break;  // (A / tap: type)
      editing_ = row;
      std::snprintf(buf_, sizeof buf_, "%s", Field(row).c_str());
      focus_edit_ = true;
      key_r_ = key_c_ = 0;
      break;
    case kSend:
      if (dir == 0) SendIt();
      break;
    default: break;
  }
}

void ReportPage::SendIt() {
  // (the launcher's rule: "what happened" said)
  const char* p = a_.what.c_str();
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') ++p;
  if (!*p) {
    status_ = kNeedWhat, status_bad_ = true;
    sel_ = kWhat;
    return;
  }
  // (the server's rule too: no web addresses)
  const bool url_what = report_text_has_url(a_.what.c_str()), url_before = report_text_has_url(a_.before.c_str());
  if (url_what || url_before) {
    status_ = REPORT_NO_URLS, status_bad_ = true;
    sel_ = url_what ? kWhat : kBefore;
    return;
  }
  if (std::string name, why = DiscordWhy(a_.contact, &name); !why.empty()) {
    status_ = why, status_bad_ = true;
    sel_ = kContact;
    return;
  } else {
    a_.contact = name;
  }
  Send s;
  s.answers = a_;
  s.server = rex::cvar::Query<std::string>("online_server");
  s.match_dir = with_match_ ? match_dir_ : "";
  s.with_mods = with_match_ && with_mods_;
  s.with_save = with_save_;
  s.pictures_later = picture_ == kLater;
  if (picture_ == kScreenshot && !shot_.rgb.empty()) {
    std::error_code ec;
    const fs::path dir = g_game_dir / "Reports" / ".upload";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "screenshot.png";
    if (WritePng(file, shot_.rgb, shot_.w, shot_.h)) {
      const std::u8string u = file.u8string();
      s.picture.assign(u.begin(), u.end());
    }
  }
  state_ = State::kSending;
  status_ = "Sending...", status_bad_ = false;
  StartSend(std::move(s));
}

void ReportPage::KeyPress(int r, int c) {
  ImGuiIO& io = ImGui::GetIO();
  if (r < 4) {
    char ch = kKeys[r][c];
    if (shift_ && ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
    if (shift_ && ch == '\'') ch = '_';  // (Discord names have _)
    io.AddInputCharacter(uint32_t(uint8_t(ch)));
    return;
  }
  // SHIFT (cols 0-2), SPACE (3-5), DELETE (6-7), DONE (8-9)
  if (c <= 2) shift_ = !shift_;
  else if (c <= 5) io.AddInputCharacter(' ');
  else if (c <= 7) io.AddKeyEvent(ImGuiKey_Backspace, true), release_ = ImGuiKey_Backspace;
  else key_done_ = true;
}

// The text box and the on-screen keyboard; true when the typing is done.
bool ReportPage::DrawEditor(ImGuiIO& io, ImDrawList* dl, float s, const std::function<ImVec2(float, float)>& P,
                            uint16_t pressed, uint16_t act, const std::vector<ImVec2>& taps) {
  using namespace rex::input;
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255);
  const bool multi = editing_ != kContact;
  const char* title = editing_ == kWhat ? "WHAT HAPPENED?" : editing_ == kBefore ? "WHAT DID YOU DO JUST BEFORE?"
                                                                                  : "CONTACT (OPTIONAL)";
  dl->AddRectFilled(P(100, 96), P(1180, 640), IM_COL32(10, 10, 14, 250), 8 * s);
  dl->AddRect(P(100, 96), P(1180, 640), IM_COL32(235, 235, 240, 255), 8 * s, 0, 2 * s);
  Text(dl, g_menu_font, 24 * s, P(130, 110), kWhite, title);
  Text(dl, g_menu_font, 16 * s, P(130, 142), kGrey,
       editing_ == kContact ? "A Discord name or an e-mail, if we may ask you about it." :
       "Type with the keyboard, or the keys below with the controller.");
  // The text box (ImGui's: the keyboard, the phone's keyboard; the keys below feed it as typed characters).
  const ImVec2 ta = P(130, 170), tb = P(1150, multi ? 350 : 214);
  ImGui::SetNextWindowPos(ta);
  ImGui::SetNextWindowSize(ImVec2(tb.x - ta.x, tb.y - ta.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(34, 34, 40, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
  ImGui::Begin("##report_edit", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PushFont(g_menu_font, 20 * s);
  if (focus_edit_) ImGui::SetKeyboardFocusHere(), focus_edit_ = false;
  const size_t max = editing_ == kContact ? 120 : sizeof buf_ - 1;
  bool enter = false;
  if (multi) {
    ImGui::InputTextMultiline("##text", buf_, max + 1, ImVec2(tb.x - ta.x, tb.y - ta.y));
  } else {
    ImGui::SetNextItemWidth(tb.x - ta.x);
    enter = ImGui::InputText("##text", buf_, max + 1, ImGuiInputTextFlags_EnterReturnsTrue);
  }
  if (!ImGui::IsItemActive() && !ImGui::IsItemDeactivated()) focus_edit_ = true;  // (keep the keyboard's focus)
  else if (ImGui::IsItemActive()) TestTyping(io);
  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);

  // The keys.
  const float kx = 190, ky = multi ? 372 : 236, kw = 90, kh = 44;
  bool done = enter || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false);
  if (act & X_INPUT_GAMEPAD_DPAD_UP) key_r_ = (key_r_ + kKeyRows - 1) % kKeyRows;
  if (act & X_INPUT_GAMEPAD_DPAD_DOWN) key_r_ = (key_r_ + 1) % kKeyRows;
  if (act & X_INPUT_GAMEPAD_DPAD_LEFT) key_c_ = (key_c_ + kKeyCols - 1) % kKeyCols;
  if (act & X_INPUT_GAMEPAD_DPAD_RIGHT) key_c_ = (key_c_ + 1) % kKeyCols;
  if (pressed & X_INPUT_GAMEPAD_A) KeyPress(key_r_, key_c_);
  if (pressed & X_INPUT_GAMEPAD_X) io.AddKeyEvent(ImGuiKey_Backspace, true), release_ = ImGuiKey_Backspace;
  if (pressed & X_INPUT_GAMEPAD_Y) io.AddInputCharacter(' ');
  if (pressed & (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER)) shift_ = !shift_;
  if (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START)) done = true;
  for (const ImVec2& t : taps)
    for (int r = 0; r < kKeyRows; ++r)
      for (int c = 0; c < kKeyCols; ++c) {
        const ImVec2 a = P(kx + c * kw, ky + r * kh), b = P(kx + (c + 1) * kw - 4, ky + (r + 1) * kh - 4);
        if (t.x >= a.x && t.x < b.x && t.y >= a.y && t.y < b.y) key_r_ = r, key_c_ = c, KeyPress(r, c);
      }
  for (int r = 0; r < kKeyRows; ++r) {
    for (int c = 0; c < kKeyCols; ++c) {
      if (r == 4 && c != 0 && c != 3 && c != 6 && c != 8) continue;  // (the wide keys: drawn once)
      const int span = r < 4 ? 1 : c == 0 || c == 3 ? 3 : 2;
      const bool sel = key_r_ == r && (r < 4 ? key_c_ == c : key_c_ >= c && key_c_ < c + span);
      const ImVec2 a = P(kx + c * kw, ky + r * kh), b = P(kx + (c + span) * kw - 4, ky + (r + 1) * kh - 4);
      if (sel) dl->AddRectFilled(a, b, IM_COL32(150, 18, 20, 255), 4 * s);
      else dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 255), 4 * s);
      char k[16];
      if (r < 4) {
        char ch = kKeys[r][c];
        if (shift_ && ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        if (shift_ && ch == '\'') ch = '_';
        k[0] = ch, k[1] = 0;
      } else {
        std::snprintf(k, sizeof k, "%s", c == 0 ? (shift_ ? "SHIFT ON" : "SHIFT") : c == 3 ? "SPACE" : c == 6 ? "DELETE" : "DONE");
      }
      const ImVec2 z = TextSize(g_menu_font, 20 * s, k);
      Text(dl, g_menu_font, 20 * s, ImVec2((a.x + b.x - z.x) * 0.5f, (a.y + b.y - z.y) * 0.5f), kWhite, k);
    }
  }
  {
    const float cy = P(0, 614).y;
    float x = P(130, 0).x;
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255), "TYPE");
    x = Hint(dl, x, cy, s, "X", IM_COL32(40, 90, 200, 255), "DELETE");
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255), "SPACE");
    x = Hint(dl, x, cy, s, "LB", IM_COL32(90, 90, 100, 255), "SHIFT");
    Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "DONE");
  }
  if (key_done_) done = true, key_done_ = false;
  return done;
}

void ReportPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  if (release_ != ImGuiKey_None) io.AddKeyEvent(release_, false), release_ = ImGuiKey_None;
  if (g_open_requested.exchange(false) && !g_open) Open();  // (two pad reads can ask before it's open)
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    std::lock_guard lock(g_tap_mutex);
    g_taps.clear();
    return;
  }
  // The screenshot (asked for at the open), its preview.
  if (shot_wanted_) {
    std::vector<uint8_t> rgb;
    uint32_t w = 0, h = 0;
    if (svr2011::native::TakeFrameCapture(rgb, w, h)) shot_ = Smaller(std::move(rgb), w, h), shot_wanted_ = false;
  }
  if (!preview_ && !shot_.rgb.empty() && g_drawer) {
    const uint32_t pw = 320, ph = std::max(1u, shot_.h * pw / std::max(1u, shot_.w));
    std::vector<uint8_t> rgba(size_t(pw) * ph * 4);
    for (uint32_t y = 0; y < ph; ++y)
      for (uint32_t x = 0; x < pw; ++x) {
        const uint8_t* p = &shot_.rgb[(size_t(y * shot_.h / ph) * shot_.w + x * shot_.w / pw) * 3];
        uint8_t* d = &rgba[(size_t(y) * pw + x) * 4];
        d[0] = p[0], d[1] = p[1], d[2] = p[2], d[3] = 255;
      }
    preview_ = g_drawer->CreateTexture(pw, ph, rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
  }
  if (state_ == State::kSending) {
    std::lock_guard lock(g_sent_mutex);
    if (g_sent.done) {
      sent_ = g_sent;
      state_ = sent_.ok ? State::kSent : State::kForm;
      status_ = sent_.ok ? "" : "Not sent: " + sent_.why + (sent_.account ? "" : " Check you're online, then try again.");
      status_bad_ = !sent_.ok;
    }
  }

  // Input: the controller (edges, held directions repeat), the keyboard, taps.
  uint16_t buttons = PadButtons();
  const auto now = Clock::now();
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k)  // (typing: the keyboard's keys reach the
    if (ImGui::IsKeyDown(ImGuiKey(k))) keyboard_until_ = now + std::chrono::milliseconds(300);  //  pad too - not here)
  if (editing_ >= 0 && now < keyboard_until_) buttons = 0;
  if (wait_release_) {
    if (buttons == 0 && now - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat =
      X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
  uint16_t act = pressed;
  if (pressed & kRepeat) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kRepeat) && now >= repeat_at_) {
    act |= buttons & kRepeat;
    repeat_at_ = now + std::chrono::milliseconds(110);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  std::vector<ImVec2> taps;
  {
    std::lock_guard lock(g_tap_mutex);
    for (const ImVec2& t : g_taps) taps.push_back(ImVec2(t.x * io.DisplaySize.x, t.y * io.DisplaySize.y));
    g_taps.clear();
  }
  if (!wait_release_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
    taps.push_back(io.MousePos);

  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  const std::function<ImVec2(float, float)> P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  auto inside = [](ImVec2 t, ImVec2 a, ImVec2 b) { return t.x >= a.x && t.x < b.x && t.y >= a.y && t.y < b.y; };
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGold = IM_COL32(255, 200, 60, 255),
              kRed = IM_COL32(255, 110, 100, 255), kGreen = IM_COL32(90, 200, 90, 255);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 235), 10 * s);
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7)
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = from_pause_ ? "PAUSE" : "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const ImVec2 psz = TextSize(g_menu_font, 22 * s, "REPORT A PROBLEM");
    Text(dl, g_menu_font, 22 * s, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "REPORT A PROBLEM");
  }

  bool back = false;
  if (editing_ >= 0) {
    if (DrawEditor(io, dl, s, P, pressed, act, taps)) {
      std::string text = buf_;
      if (editing_ == kContact) text = text.substr(0, 120);
      Field(editing_) = text;
      REXLOG_INFO("[svr2011] report: {} typed ({} bytes)",
                  editing_ == kWhat ? "what happened" : editing_ == kBefore ? "just before" : "contact", text.size());
      editing_ = -1;
      wait_release_ = true;  // (the B / START that ended it)
      opened_at_ = now;
    }
    return;
  }

  if (state_ == State::kSent) {  // (the answer)
    const float wrap = P(1180, 0).x - P(100, 0).x;
    Text(dl, g_menu_font, 30 * s, P(100, 120), kGreen, "Your report reached us.");
    std::string line = "Report id: " + sent_.id;
    Text(dl, g_menu_font, 24 * s, P(100, 180), kWhite, line.c_str());
    if (sent_.seen_before > 0) {
      line = "This problem was already reported " + std::to_string(sent_.seen_before) +
             (sent_.seen_before == 1 ? " time" : " times") + " - yours adds to it, thank you.";
      Text(dl, g_menu_font, 18 * s, P(100, 230), kGrey, line.c_str(), wrap);
    }
#if defined(__ANDROID__)
    const char* where = "It's in the app's My reports: you'll see there when it's fixed or if we need more.";
#elif defined(__SWITCH__)
    const std::string where_text = "Your report number is " + sent_.id + ". Keep it if you want to ask about this report.";
    const char* where = where_text.c_str();
#else
    const char* where = "It's in the launcher's Reports tab: you'll see there when it's fixed or if we need more.";
#endif
    Text(dl, g_menu_font, 18 * s, P(100, 280), kGrey, where, wrap);
#if !defined(__SWITCH__)
    if (picture_ == kLater) Text(dl, g_menu_font, 18 * s, P(100, 320), kGold, "Add your pictures to it there.", wrap);
#endif
    Hint(dl, P(84, 0).x, P(0, 660).y, s, "B", IM_COL32(200, 40, 40, 255), "CLOSE");
    if ((pressed & (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START)) || !taps.empty() ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
      Close();
    return;
  }

  // The form.
  const bool sending = state_ == State::kSending;
  auto choice = [](int r) { return r <= kHappens || (r >= kPicture && r <= kSave); };
  if (!sending) {
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) sel_ = (sel_ + kRows - 1) % kRows;
    if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % kRows;
    if ((act & X_INPUT_GAMEPAD_DPAD_LEFT) || key(ImGuiKey_LeftArrow)) Activate(sel_, -1);
    if ((act & X_INPUT_GAMEPAD_DPAD_RIGHT) || key(ImGuiKey_RightArrow)) Activate(sel_, +1);
    if ((pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
      Activate(sel_, choice(sel_) ? +1 : 0);
    back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_BACK)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  }
  const float row_h = 46, gap = 6, top = 98;
  const std::string what1 = a_.what.substr(0, a_.what.find('\n')), before1 = a_.before.substr(0, a_.before.find('\n'));
  auto cut = [](std::string t) { return t.size() > 34 ? t.substr(0, 32) + "..." : t; };
  char pct[48];
  const uint64_t total = g_progress_total.load();
  std::snprintf(pct, sizeof pct, total ? "SENDING %d%%" : "SENDING...", total ? int(g_progress_sent * 100 / total) : 0);
  static const char* const kLabels[] = {"PROBLEM", "HOW OFTEN", "WHAT HAPPENED", "JUST BEFORE", "CONTACT",
                                        "PICTURE", "MATCH",     "MATCH'S MODS",  "MAIN SAVE",   ""};
  for (int r = 0; r < kRows; ++r) {
    const float y = top + r * (row_h + gap);
    const ImVec2 a = P(80, y), b = P(744, y + row_h);
    if (!sending)
      for (const ImVec2& t : taps)
        if (inside(t, a, b)) sel_ = r, Activate(r, choice(r) ? +1 : 0);
    const bool sel = r == sel_;
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    std::string value;
    ImU32 vc = kWhite;
    switch (r) {
      case kKind: value = kKindText[a_.kind]; break;
      case kHappens: value = kHappensText[a_.happens]; break;
      case kWhat:
        value = a_.what.empty() ? "(needed - A to type)" : cut(what1);
        if (a_.what.empty()) vc = kGold;
        break;
      case kBefore:
        value = a_.before.empty() ? "(A to type)" : cut(before1);
        if (a_.before.empty()) vc = kGrey;
        break;
      case kContact:
        value = a_.contact.empty() ? "(needed - A to type)" : cut(a_.contact);
        if (a_.contact.empty()) vc = kGold;
        break;
      case kPicture:
        value = picture_ == kScreenshot ? (shot_.rgb.empty() ? "SCREENSHOT (none yet)" : "SCREENSHOT")
                : picture_ == kLater   ? "LATER IN THE LAUNCHER"
                                       : "NONE";
        break;
      case kMatch:
        value = match_dir_.empty() ? "NONE RECORDED" : with_match_ ? (from_pause_ ? "THIS MATCH" : "THE LAST MATCH") : "NO";
        if (match_dir_.empty()) vc = kDim;
        break;
      case kMods:
        value = !with_match_ ? "-" : with_mods_ ? "YES" : "NO";
        if (!with_match_) vc = kDim;
        break;
      case kSave: value = with_save_ ? "YES" : "NO"; break;
      case kSend: value = sending ? pct : status_bad_ && !sent_.why.empty() ? "TRY AGAIN" : "SEND"; break;
      default: break;
    }
    const float ts = 20 * s;
    if (r == kSend) {
      const ImVec2 z = TextSize(g_menu_font, 22 * s, value.c_str());
      Text(dl, g_menu_font, 22 * s, ImVec2((a.x + b.x - z.x) * 0.5f, (a.y + b.y - z.y) * 0.5f), kWhite, value.c_str());
      continue;
    }
    const float ty = (a.y + b.y) * 0.5f - TextSize(g_menu_font, ts, "A").y * 0.5f;
    Text(dl, g_menu_font, ts, ImVec2(P(96, 0).x, ty), sel ? kWhite : kGrey, kLabels[r]);
    const ImVec2 vz = TextSize(g_menu_font, ts, value.c_str());
    Text(dl, g_menu_font, ts, ImVec2(P(728, 0).x - vz.x, ty), vc, value.c_str());
  }
  // The selected row's help, the screenshot, the status.
  const ImVec2 da = P(766, top), db = P(1200, top + kRows * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  static const char* const kHelp[] = {
      "What kind of problem was it?",
      "How often does it happen?",
      "Needed: what happened - what you saw, in which mode or match, with whom.",
      "What did you do just before it happened?",
      "Optional: a Discord name or an e-mail, if we may ask you about it.",
      "A picture of the game from before this menu opened - or none now, and your own pictures added later in the "
      "launcher's Reports tab.",
      "The match's recording (its setup and the controllers' input) - so we can play it again.",
      "The mods the match used (only those the server doesn't have yet).",
      "Your main save (not created superstars, logos or replays).",
      "Sends the report with the game's logs."};
  const float wrap = db.x - da.x - 40 * s;
  Text(dl, g_menu_font, 18 * s, P(786, top + 16), kWhite, kHelp[sel_], wrap);
  if (preview_ && picture_ == kScreenshot) {
    const float pw = 400, ph = pw * float(shot_.h) / float(std::max(1u, shot_.w));
    const ImVec2 ia = P(783, top + 140), ib = P(783 + pw, top + 140 + ph);
    dl->AddImage(reinterpret_cast<ImTextureID>(preview_.get()), ia, ib);
    dl->AddRect(ia, ib, IM_COL32(200, 200, 208, 255), 0, 0, 1.0f * s);
  }
  if (!status_.empty())
    Text(dl, g_menu_font, 18 * s, P(786, top + 400), status_bad_ ? kRed : kGold, status_.c_str(), wrap);
  {
    const float cy = P(0, 660).y;
    float x = P(84, 0).x;
    const float x_b = x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255),
             sel_ == kSend ? "SEND" : sel_ >= kWhat && sel_ <= kContact ? "TYPE" : "CHANGE");
    Hint(dl, x, cy, s, "<>", IM_COL32(90, 90, 100, 255), "CHANGE");
    for (const ImVec2& t : taps)
      if (!sending && t.y > cy - 16 * s && t.y < cy + 16 * s && t.x >= x_b && t.x < x_b + 110 * s) back = true;
  }
  if (back) Close();
}

// ---------------------------------------------------------------------------
// The pause menu's row (sub_824271E8 makes the pause screen, vtable 0x820183E0; its rows: a 20-byte entry each at
// +40 - +16 the label, +0 / +4 its state - +44 how many (+36's +28 the cursor's), +48 the cursor, +16 the screen's state (1 opening, 2 up,
// 5 a row chosen). The rows come from a table of 8 (0x820182F8, the 8th left out in most matches); a row's index is what
// sub_824068A8(screens, kind) opens: 0 RETURN TO GAME, 1 RESTART MATCH, 2 HIGHLIGHT REEL, 3 VISUAL PRESENTATION,
// 4 CONTROL SETTINGS, 5 MATCH INFORMATION, 6 QUIT MATCH, 7 the 8th).

constexpr uint32_t kPauseVtable = 0x820183E0;
constexpr uint32_t kQuitLabel = 0x094B;
constexpr uint32_t kReportLabel = 0x0FA0B202;  // (menu_hooks.cpp gives its text)
constexpr uint32_t kMaxRows = 16;
std::atomic<uint32_t> g_pause{0}, g_pause_items{0}, g_pause_row{0};
std::atomic<uint32_t> g_pause_any{0};  // (also online, where it gets no REPORT A PROBLEM: PauseMenuUp)
uint16_t g_last_pad = 0;

bool PauseShown(uint8_t* base, uint32_t* cursor) {
  const uint32_t o = g_pause.load();
  const uint32_t state = o ? Rd32(base + o + 16) : 0;  // (1 opening, 2 up, 5 a row chosen)
  if (!o || Rd32(base + o) != kPauseVtable || Rd32(base + o + 40) != g_pause_items.load() || (state != 1 && state != 2))
    return false;
  if (cursor) *cursor = Rd32(base + o + 48);
  return true;
}

}  // namespace

REX_EXTERN(__imp__sub_824271E8);
REX_HOOK_RAW(sub_824271E8) {
  const uint32_t o = ctx.r3.u32;
  __imp__sub_824271E8(ctx, base);
  if (!o || Rd32(base + o) != kPauseVtable || !g_memory) return;
  g_pause_any = o;
  if (svr2011::Lockstep()) return;  // (online: the match doesn't pause - no REPORT A PROBLEM there)
  const uint32_t items = Rd32(base + o + 40), count = Rd32(base + o + 44);
  if (!items || count == 0 || count + 1 > kMaxRows) return;
  uint32_t quit = count;
  for (uint32_t i = 0; i < count; ++i)
    if (Rd32(base + items + i * 20 + 16) == kQuitLabel) quit = i;
  if (quit == count) return;
  static uint32_t ours = 0;  // (the rows with ours: the game's array stays as it is, freed by the game)
  if (!ours) ours = g_memory->SystemHeapAlloc(kMaxRows * 20);
  if (!ours) return;
  uint8_t* d = base + ours;
  std::memcpy(d, base + items, quit * 20);
  std::memcpy(d + quit * 20, base + items + quit * 20, 20);  // (QUIT MATCH's state: always on)
  Wr32(d + quit * 20 + 16, kReportLabel);
  std::memcpy(d + (quit + 1) * 20, base + items + quit * 20, (count - quit) * 20);
  Wr32(base + o + 40, ours);
  Wr32(base + o + 44, count + 1);
  // (the cursor's own row count: +36's +28 - the constructor's 7 - else the cursor wraps before QUIT MATCH)
  const uint32_t cursor = Rd32(base + o + 36);
  const uint32_t moves = cursor ? Rd32(base + cursor + 28) : 0;
  if (moves == count) Wr32(base + cursor + 28, count + 1);
  g_pause = o, g_pause_items = ours, g_pause_row = quit;
  REXLOG_INFO("[svr2011] report: pause menu - REPORT A PROBLEM at row {} of {} (cursor rows {} -> {})", quit, count + 1,
              moves, cursor ? Rd32(base + cursor + 28) : 0);
}

// The pause menu's choice: the rows after ours open what they did before.
REX_EXTERN(__imp__sub_824068A8);
REX_HOOK_RAW(sub_824068A8) {
  const uint32_t screen = Rd32(base + ctx.r3.u32 + 112), kind = ctx.r4.u32;
  if (screen && screen == g_pause.load() && Rd32(base + screen) == kPauseVtable &&
      Rd32(base + screen + 40) == g_pause_items.load()) {
    const uint32_t row = g_pause_row.load();
    if (kind == row) {  // (A never reaches it - ReportPagePad; a resume, to be safe)
      REXLOG_WARN("[svr2011] report: the pause menu chose REPORT A PROBLEM itself - back to the match");
      ctx.r4.u64 = 0;
    } else if (kind > row) {
      ctx.r4.u64 = kind - 1;
    }
    REXLOG_INFO("[svr2011] report: pause menu row {} chosen - opens the game's {}", kind, ctx.r4.u32);
  }
  __imp__sub_824068A8(ctx, base);
}

namespace svr2011 {

bool PauseMenuUp(uint8_t* base) {
  const uint32_t o = g_pause_any.load();
  if (!o || !base || Rd32(base + o) != kPauseVtable) return false;
  const uint32_t state = Rd32(base + o + 16);
  static const bool log = std::getenv("SVR2011_TEST_PAD_MAP_LOG") != nullptr;
  static uint32_t logged = ~0u;
  if (log && state != logged) {
    logged = state;
    REXLOG_INFO("[svr2011] pause screen {:08X} state {}", o, state);
  }
  // (a row chosen: 3, then 5 - for a page the game opens from it and, for good, once back in the match; 0 on the way
  // back from that page. Until the match clock runs again it's still the pause. A page like CONTROL SETTINGS keeps the
  // clock running, so it counts as play: its buttons and their icons both follow the layout.)
  using Clock = std::chrono::steady_clock;
  static uint32_t clock_was = 0;
  static Clock::time_point moved_at{};
  const uint32_t clock = MatchClock(base);
  if (clock != clock_was) clock_was = clock, moved_at = Clock::now();
  if (state == 1 || state == 2) return true;
  return Clock::now() - moved_at > std::chrono::milliseconds(150);
}

void ReportPagePad(uint8_t* base, uint8_t* state) {
  if (!base || !state) return;
  const uint16_t b = uint16_t(state[4] << 8 | state[5]);
  const uint16_t edge = uint16_t(b & ~g_last_pad);
  g_last_pad = b;
  uint32_t cursor = 0;
  const bool shown = PauseShown(base, &cursor);
  // START in a match, the pause not up yet: the frame before it, for the screenshot.
  if ((edge & 0x0010) && InMatch() && !shown && !Lockstep()) native::RequestFrameCapture(true);
  if (!shown || cursor != g_pause_row.load()) return;
  if ((b & 0x1000) && !g_open && !g_open_requested) OpenReportPage(true);
  if (g_open || g_open_requested) {  // (the game never sees that A)
    const uint16_t kept = uint16_t(b & ~0x1000);
    state[4] = uint8_t(kept >> 8), state[5] = uint8_t(kept);
  }
}

void InstallReportPage(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::ui::Window* window,
                       rex::input::InputSystem* input, rex::memory::Memory* memory,
                       const std::filesystem::path& user_data) {
  g_imgui = drawer;
  g_drawer = immediate;
  g_window = window;
  g_input = input;
  g_memory = memory;
  g_game_dir = user_data.parent_path();
  if (window) window->app_context().CallInUIThread([window] { window->AddInputListener(&g_touch, 951); });
  new ReportPage(drawer);  // lives for the whole run
}

void SetReportPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenReportPage(bool from_pause) {
  g_from_pause = from_pause;
  g_open_requested = true;
}

bool ReportPageHoldsInput() { return g_open.load() || g_open_requested.load() || g_wait_release.load(); }

}  // namespace svr2011
