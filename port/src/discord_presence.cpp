// WWE SmackDown vs. Raw 2011 - Discord Rich Presence (see discord_presence.h).

#include "discord_presence.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(discord_presence, true, "UI", "Show what you're playing in Discord (Rich Presence)");
REXCVAR_DEFINE_STRING(discord_app_id, "1554845537088577716", "UI",
                      "The Discord application (its name and icon) Rich Presence shows");

namespace svr2011 {

namespace {

std::atomic<int> g_scene{int(DiscordScene::kMenus)};
std::atomic<int> g_scene_version{0};

#if defined(_WIN32)

class Pipe {
 public:
  ~Pipe() { Close(); }
  bool Open() {
    for (int i = 0; i < 10; ++i) {
      const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
      h_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h_ != INVALID_HANDLE_VALUE) return true;
    }
    return false;
  }
  void Close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  // A frame: opcode, length (little-endian), JSON.
  bool Send(uint32_t op, const std::string& json) {
    std::string frame(8 + json.size(), '\0');
    const uint32_t len = uint32_t(json.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &len, 4);
    std::memcpy(frame.data() + 8, json.data(), json.size());
    DWORD written = 0;
    return WriteFile(h_, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
  }
  // The reply (its JSON; empty on failure).
  std::string Receive() {
    uint32_t header[2];
    DWORD got = 0;
    if (!ReadFile(h_, header, 8, &got, nullptr) || got != 8 || header[1] > (1u << 20)) return {};
    std::string json(header[1], '\0');
    DWORD total = 0;
    while (total < header[1]) {
      if (!ReadFile(h_, json.data() + total, header[1] - total, &got, nullptr) || !got) return {};
      total += got;
    }
    return json;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

std::string Activity(DiscordScene scene, int64_t start) {
  const char* details = scene == DiscordScene::kMatch       ? "In a match"
                        : scene == DiscordScene::kEntrances ? "Watching the entrances"
                                                            : "In the menus";
  return std::string("{\"details\":\"") + details + "\",\"timestamps\":{\"start\":" + std::to_string(start) +
         "},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"WWE SmackDown vs. Raw 2011 (PC port)\"}}";
}

void Run(std::string app_id) {
  rex::thread::set_current_thread_name("discord");
  const int64_t start =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const DWORD pid = GetCurrentProcessId();
  int nonce = 0;
  bool logged_missing = false;
  for (;;) {
    Pipe pipe;
    if (!pipe.Open()) {  // (Discord not running: try again now and then)
      if (!logged_missing) REXLOG_INFO("discord: not running (Rich Presence waits for it)");
      logged_missing = true;
      std::this_thread::sleep_for(std::chrono::seconds(20));
      continue;
    }
    if (!pipe.Send(0, "{\"v\":1,\"client_id\":\"" + app_id + "\"}") ||
        pipe.Receive().find("\"READY\"") == std::string::npos) {
      REXLOG_WARN("discord: the handshake failed (is discord_app_id right?)");
      std::this_thread::sleep_for(std::chrono::seconds(60));
      continue;
    }
    REXLOG_INFO("discord: connected, Rich Presence on");
    logged_missing = false;
    int shown = -1;
    for (;;) {
      const int version = g_scene_version.load();
      if (version != shown) {
        const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                                ",\"activity\":" + Activity(DiscordScene(g_scene.load()), start) +
                                "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
        if (!pipe.Send(1, cmd) || pipe.Receive().empty()) break;  // (Discord closed: reconnect)
        shown = version;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    REXLOG_INFO("discord: disconnected");
  }
}

#endif  // _WIN32

}  // namespace

void StartDiscordPresence() {
#if defined(_WIN32)
  if (!REXCVAR_GET(discord_presence)) return;
  // (automated tests don't show on the player's Discord unless asked)
  if (std::getenv("SVR2011_INPUT_FILE") && !std::getenv("SVR2011_DISCORD_TEST")) return;
  const std::string app_id = REXCVAR_GET(discord_app_id);
  if (app_id.empty()) {
    REXLOG_INFO("discord: no discord_app_id set - Rich Presence off");
    return;
  }
  std::thread(Run, app_id).detach();
#endif
}

void SetDiscordScene(DiscordScene scene) {
  if (g_scene.exchange(int(scene)) != int(scene)) ++g_scene_version;
}

}  // namespace svr2011
