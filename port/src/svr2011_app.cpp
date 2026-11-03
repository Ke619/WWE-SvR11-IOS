// WWE SmackDown vs. Raw 2011 - app setup (paths and settings defaults).

#include "generated/default/svr2011_init.h"

#include "svr2011_app.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include <imgui.h>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

#include "achievements_page.h"
#include "jukebox.h"
#include "backgrounds_page.h"
#include "report_page.h"
#include "weapons_page.h"
#include "weapon_mods.h"
#include "caw_logos.h"
#include "paint_pages.h"
#include "crowd_fight.h"
#include "match_types.h"
#include "online_rules.h"
#include "port_map.h"
#include "mystery_opponent.h"
#include "arena_mods.h"
#include "story_designer.h"
#include "move_packs.h"
#include "ring_rules.h"
#include "caw_limits.h"
#include "superstar_mods.h"
#include "caw_check.h"
#include "superstar_threads.h"
#include "crowd_signs.h"
#include "hd_movies.h"
#include "media_mods.h"
#include "vs_styles.h"
#include "frame_rate.h"
#include "guest_faults.h"
#include "perf_hooks.h"
#include "crash_report.h"
#include "match_box.h"
#include "dlc.h"
#include "fps_overlay.h"
#include "perf_stats.h"
#if SVR2011_DEBUG_SUITE
#include "debug_suite/debug_suite.h"
#endif
#include "game_files.h"
#include "graphics_page.h"
#include "menu_hooks.h"
#include "music.h"
#include "saves.h"
#include "user_movies.h"
#include "cae_mod_motions.h"
#include "cam_mod_moves.h"
#include "text_guard.h"
#include "native/native_renderer.h"
#include "frame_stats.h"
#include "keyboard_typing.h"
#include "free_cam.h"
#include "gpu_probe.h"
#if SVR2011_DLSS
#include "native/dlss.h"
#endif
#include "platform.h"
#include "script_input.h"
#include "touch_controls.h"
#include "pad_types.h"
#include "pad_icons.h"
#include "playtime.h"
#include <rex/input/flags.h>
#include "discord_presence.h"
#include "online.h"
#include "entrance_media.h"
#include "online_cas.h"
#include "match_hud.h"
#include "online_files.h"
#include "online_overlay.h"
#include "startup_pump.h"
#include "leaderboards.h"
#include "online_mods.h"
#include "online_chat.h"
#include "mod_transfer.h"
#include "p2p.h"
#include "universe_mods.h"
#include "universe_welcome.h"
#include "util.h"
#if defined(_WIN32)
#include "xaudio2_audio.h"
#endif

#ifndef SVR2011_D3D_TRACE
namespace svr2011 {
void InstallModelMem(rex::memory::Memory* memory);  // (model_mem.cpp: failed buffer allocations logged)
}
#endif

REXCVAR_DEFINE_STRING(saves_folder, "", "Storage",
                      "Where the saves live: empty for the game folder's Saves, or a folder (absolute, or "
                      "relative to the game folder) - e.g. to keep two installs' saves apart or shared")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(audio_backend, "xaudio2", "Audio",
                      "Audio output: xaudio2 (default) or sdl")
    .allowed({"xaudio2", "sdl"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// The utility drive's folder. HostPathDevice always reports 64 MB free, and
// at startup the game turns off match highlights (the MATCH HIGHLIGHTS
// button after a match, greyed) unless cache: has room for its 150 MB
// replay file (Spectacular.rps). This reports the host disk's free space,
// up to 1 GB.
class UtilityDriveDevice : public rex::filesystem::HostPathDevice {
 public:
  UtilityDriveDevice(const std::filesystem::path& dir)
      : HostPathDevice("\\UTILITY", dir, false) {
    std::error_code ec;
    const auto space = std::filesystem::space(dir, ec);
    const uint64_t free = ec ? kCap : std::min<uint64_t>(space.available, kCap);
    units_ = uint32_t(free / bytes_per_sector());
  }
  uint32_t total_allocation_units() const override { return uint32_t(kCap / bytes_per_sector()); }
  uint32_t available_allocation_units() const override { return units_; }

 private:
  static constexpr uint64_t kCap = 1ull << 30;
  uint32_t units_ = 0;
};

// Written when svr2011.toml is missing: the settings a fresh install starts
// with. The launcher's Settings tab rewrites this file.
constexpr char kDefaultConfig[] =
    "# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)\n"
    "gpu_plugin = \"xenos\"\n"
    "native_renderer = \"main\"\n"  // (the only renderer: no emulated one)
    "input_backend = \"xinput\"\n"
    "resolution = \"720p\"\n"
    "resolution_scale = 1\n"
    "fullscreen = false\n"
    "vsync = true\n";

std::filesystem::path g_config_path;  // svr2011.toml (the GRAPHICS page saves there)

// Value of an environment variable, or "" when unset.
using svr2011::Env;

// A settings file's "basic strings" broken over lines joined again: a
// `key = "...` line with no closing quote takes the next non-blank lines
// (the blank ones between them dropped) until its quote closes.
std::string JoinBrokenTomlStrings(const std::string& text) {
  const auto open_string = [](const std::string& line) {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line[p] == '#' || line[p] == '[') return false;
    const size_t eq = line.find('=', p);
    if (eq == std::string::npos) return false;
    p = line.find_first_not_of(" \t", eq + 1);
    if (p == std::string::npos || line[p] != '"' || line.compare(p, 3, "\"\"\"") == 0) return false;
    for (++p; p < line.size(); ++p) {
      if (line[p] == '\\') ++p;
      else if (line[p] == '"') return false;
    }
    return true;
  };
  std::string out, pending;
  bool joining = false;
  for (size_t pos = 0; pos <= text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    const bool last = nl == text.size();
    pos = nl + 1;
    if (joining) {
      if (line.empty()) {
        if (last) break;
        continue;
      }
      pending += line;
    } else {
      pending = line;
    }
    joining = open_string(pending) && !last;
    if (joining) continue;
    out += pending;
    if (!last) out += cr ? "\r\n" : "\n";
  }
  if (joining) out += pending;
  return out;
}

// A settings file's top-level `key = word` lines with the word not quoted
// (true / false / inf / nan and numbers aside) quoted: 2.1.5's GRAPHICS ->
// UPSCALING wrote native_upscale = off, not TOML - the file didn't parse and
// every setting was back at its default at each start (VIBRATION off came
// back on). As the Android launcher's GameSettings.Repaired.
std::string QuoteBareTomlWords(const std::string& text, std::string* keys) {
  const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  std::string out;
  bool section = false;
  for (size_t pos = 0; pos <= text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    const bool last = nl == text.size();
    pos = nl + 1;
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    const size_t a = line.find_first_not_of(" \t");
    const size_t eq = line.find('=');
    if (a != std::string::npos && line[a] == '[') section = true;
    if (!section && a != std::string::npos && line[a] != '#' && eq != std::string::npos && eq > a) {
      std::string key = line.substr(a, eq - a), value = line.substr(eq + 1);
      key.erase(key.find_last_not_of(" \t") + 1);
      value.erase(0, value.find_first_not_of(" \t"));
      value.erase(value.find_last_not_of(" \t") + 1);
      const bool key_ok = !key.empty() && std::all_of(key.begin(), key.end(), word);
      const bool bare = !value.empty() && (std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_') &&
                        std::all_of(value.begin(), value.end(),
                                    [&](char c) { return word(c) || c == '.' || c == '-'; });
      if (key_ok && bare && value != "true" && value != "false" && value != "inf" && value != "nan") {
        line = key + " = \"" + value + "\"";
        if (keys) *keys += (keys->empty() ? "" : ", ") + key;
      }
    }
    out += line;
    if (!last) out += cr ? "\r\n" : "\n";
  }
  return out;
}

// A settings file's top-level lines TOML can't read - no `=` (a file cut off
// mid-line: "render_sc") or nothing after it - turned into comments: one such
// line made the whole file unreadable, every setting back at its default at
// each start (release test S5, 27 Oct). Lines inside a value over several lines
// (an array or a """ string) are left alone.
std::string SetAsideBrokenTomlLines(const std::string& text, std::string* lines) {
  std::string out;
  bool section = false, array = false, triple = false;
  for (size_t pos = 0; pos <= text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    const bool last = nl == text.size();
    pos = nl + 1;
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    const size_t a = line.find_first_not_of(" \t");
    const size_t eq = line.find('=');
    size_t quotes = 0;  // (""" marks on the line)
    for (size_t q = line.find("\"\"\""); q != std::string::npos; q = line.find("\"\"\"", q + 3)) ++quotes;
    if (triple || array) {
      if (triple && quotes % 2) triple = false;
      if (array && line.find(']') != std::string::npos) array = false;
    } else if (a != std::string::npos && line[a] == '[') {
      section = true;
    } else if (!section && a != std::string::npos && line[a] != '#') {
      std::string value = eq == std::string::npos ? std::string() : line.substr(eq + 1);
      value.erase(0, value.find_first_not_of(" \t"));
      if (eq == std::string::npos || eq == a || value.empty() || value[0] == '#') {
        if (lines) *lines += (lines->empty() ? "" : ", ") + line.substr(a);
        line = "# (unreadable, set aside by the game) " + line.substr(a);
      } else if (quotes % 2) {
        triple = true;
      } else if (value[0] == '[' && value.find(']') == std::string::npos) {
        array = true;
      }
    }
    out += line;
    if (!last) out += cr ? "\r\n" : "\n";
  }
  return out;
}

// What the settings repairs above did: OnConfigurePaths runs before the log
// file is open, so their lines are written at OnPreSetup.
std::vector<std::string> g_settings_notes;

// The runtime's user data (paths.user_data_root) and the saves folder.
std::filesystem::path g_user_data;
std::filesystem::path g_saves;

// The disc's files the game folder lacks (online_files.h MissingGameFiles), at start: logged; one the game can't
// start without stops it here with a message (Windows) - it crashed 0.3 s in otherwise, a read of guest 0x28 with
// pac\_startup.pac and plist360.h missing (2.1.7 report ea8202). Not for another game's data (--game_data_root:
// the Anthology's). (Test aid SVR2011_TEST_NO_DIALOGS=1: no box.)
bool g_disc_data_beside = false;  // (the game data is the folder beside the program)
void CheckGameFilesThere() {
  if (!g_disc_data_beside) return;
  const auto missing = svr2011::MissingGameFiles();
  if (missing.empty()) return;
  std::string list;
  int essential = 0;
  for (const auto& m : missing) {
    essential += m.essential;
    if (list.size() < 600) list += (list.empty() ? "" : ", ") + m.path;
  }
  if (!essential) {
    REXLOG_WARN("game files: {} of the disc's files missing from the game folder: {}", missing.size(), list);
    return;
  }
  REXLOG_ERROR("game files: {} of the disc's files missing from the game folder ({} the game can't start without): {}"
               " - not started (reinstall from the Xbox 360 disc image: launcher Install, then Verify game files)",
               missing.size(), essential, list);
  rex::FlushLogging();
#if defined(_WIN32)
  if (!std::getenv("SVR2011_TEST_NO_DIALOGS")) {
    std::wstring text =
        L"The game files in this folder don't match the Xbox 360 disc - some are missing, so the game can't start:\n\n";
    int shown = 0;
    for (const auto& m : missing) {
      if (!m.essential || ++shown > 8) continue;
      text += L"    " + std::wstring(m.path.begin(), m.path.end()) + L"\n";
    }
    if (essential > 8) text += L"    ... and " + std::to_wstring(essential - 8) + L" more\n";
    text += L"\nInstall the game again from your Xbox 360 disc image (ISO) with the launcher's Install tab, "
            L"then use Verify game files.";
    MessageBoxW(nullptr, text.c_str(), L"SvR 2011 - game files missing", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  }
  std::_Exit(3);
#endif
}

// A Mac through Wine / CrossOver (Wine's ntdll says the host is Darwin): its Vulkan is MoltenVK, which lacks what the
// game's Vulkan path needs - the game aborted in winevulkan a moment in, a black window (2.1.7 report f331f8, Apple
// M1). Logged, and a question (Windows): play anyway, or quit. Test aid SVR2011_TEST_MAC_WINE=1 (as if on one).
void CheckMacWine() {
#if defined(_WIN32)
  bool mac = std::getenv("SVR2011_TEST_MAC_WINE") != nullptr;
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    if (auto host = reinterpret_cast<void (*)(const char**, const char**)>(GetProcAddress(ntdll, "wine_get_host_version"))) {
      const char *sysname = nullptr, *release = nullptr;
      host(&sysname, &release);
      REXLOG_INFO("platform: Wine on {} {}", sysname ? sysname : "?", release ? release : "");
      mac |= sysname && !std::strcmp(sysname, "Darwin");
    }
  if (!mac) return;
  REXLOG_WARN("platform: a Mac through Wine / CrossOver - not supported (its Vulkan driver, MoltenVK, lacks what the "
              "game needs: a black window or a crash at start are likely)");
  rex::FlushLogging();
  if (std::getenv("SVR2011_TEST_NO_DIALOGS")) return;
  const int answer = MessageBoxW(
      nullptr,
      L"This looks like a Mac running the game through Wine or CrossOver. That isn't supported: the Vulkan driver "
      L"there (MoltenVK) lacks what the game needs, so it usually shows a black window or closes at start.\n\n"
      L"Try anyway?",
      L"SvR 2011 - Mac through Wine isn't supported", MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
  if (answer != IDYES) {
    REXLOG_INFO("platform: the player chose not to try");
    rex::FlushLogging();
    std::_Exit(4);
  }
  REXLOG_INFO("platform: the player chose to try anyway");
#endif
}

// default.xex from another release of the game (online_files.h DefaultXexSupported): the port's code is the
// supported xex's, recompiled, so another's ran a moment and quit without a word ("No function registered at
// 8216B140": the Japanese disc, 2.1.8 report 6fd821). Stopped here with a message (Windows) instead.
void CheckSupportedXex() {
  if (!g_disc_data_beside) return;
  uint64_t size, want_size;
  uint32_t crc, want_crc;
  const int ok = svr2011::DefaultXexSupported(&size, &crc);
  if (ok != 0) {
    if (ok < 0) REXLOG_WARN("game files: default.xex couldn't be read to check its release");
    return;
  }
  svr2011::SupportedXex(&want_size, &want_crc);
  REXLOG_ERROR("game files: default.xex is from another release of the game ({} bytes{}; the port's is {} bytes, CRC "
               "{:08X} - the US / Europe disc) - not started",
               size, crc ? fmt::format(", CRC {:08X}", crc) : std::string(), want_size, want_crc);
  rex::FlushLogging();
#if defined(_WIN32)
  if (!std::getenv("SVR2011_TEST_NO_DIALOGS")) {
    const std::wstring text =
        L"This game folder's default.xex is from a different release of WWE SmackDown vs. Raw 2011 than the one "
        L"this port was made from - for example the Japanese disc.\n\n"
        L"The port only runs the US / Europe Xbox 360 release (title 5451085D). Install the game from that disc "
        L"image (ISO) with the launcher's Setup -> The game, then use Verify game files.";
    MessageBoxW(nullptr, text.c_str(), L"SvR 2011 - unsupported game release", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  }
  std::_Exit(3);
#endif
}

}  // namespace

void Svr2011App::OnConfigurePaths(rex::PathConfig& paths) {
  // 1 ms timer resolution. Without it Windows rounds every Sleep up to the
  // 15.6 ms default tick, and the runtime sleeps while the emulated GPU waits
  // on the game's fences (and for the game's own Sleep calls) - that alone
  // held the training ring to ~39 fps.
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif

  // The game asks the music player (XMP) for its playback controller twice a
  // frame from its simulation thread; the runtime's anti-spin delay for that
  // call (10 ms each, meant for titles that poll it in a tight loop) held the
  // training ring to ~39 fps. Not a user setting, so it is forced here.
  rex::cvar::SetFlagByName("xmp_throttle", "false");

  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();

  // The disc files live beside the program unless --game_data_root says
  // otherwise.
  if (paths.game_data_root.empty()) {
    paths.game_data_root = exe_dir;
    g_disc_data_beside = true;
  }
  paths.user_data_root = exe_dir / "UserData";
  // Tests: SVR2011_USER_DATA=<folder> keeps their saves away from the player's.
  if (std::string dir = Env("SVR2011_USER_DATA"); !dir.empty()) {
    paths.user_data_root = dir;
  }
  g_user_data = paths.user_data_root;
  // Saves: <game>\Saves (tests: beside their user data).
  g_saves = (Env("SVR2011_USER_DATA").empty() ? exe_dir : g_user_data) / "Saves";
  paths.cache_root = exe_dir / "UserData" / "cache";
  svr2011::InstallCrashReporter(exe_dir / "UserData" / "crashes");
  svr2011::SetCrashTextHook(&svr2011::MatchBoxCrashText);  // (the match's black box in the report: match_box.h)
  // Online: where the game files checked against the launcher's list are (online_files.h; the check starts with online).
  svr2011::SetGameFilesPaths(paths.game_data_root, g_user_data);

  // Tests: SVR2011_CONFIG=<file> keeps settings changes away from the player's.
  if (std::string config = Env("SVR2011_CONFIG"); !config.empty()) {
    paths.config_path = config;
  }
  g_config_path = paths.config_path;
  // The settings a fresh install starts with, for this platform.
  std::string config = kDefaultConfig;
#if defined(__ANDROID__)
  // The phone: Vulkan, SDL controllers, the whole screen, and the Xbox 360's
  // 720p render cost (at the screen's 1080p it rendered 3x, 30-45 fps; the
  // GRAPHICS page raises it).
  config.replace(config.find("input_backend = \"xinput\""), 24, "input_backend = \"sdl\"");
  config += "gpu_backend = \"vulkan\"\n";
  config += "native_max_scale = 1\n";
  config += "native_2x_msaa = false\n";
  config.replace(config.find("fullscreen = false"), 18, "fullscreen = true");
#else
  // The PC keyboard plays as player 1's controller too (the SDK's keyboard
  // driver: keybind_* in the settings).
  config += "mnk_mode = true\n";
#endif
  if (svr2011::IsSteamDeck()) {  // its screen is the window
    config.replace(config.find("fullscreen = false"), 18, "fullscreen = true");
  }
  if (!std::filesystem::exists(paths.config_path)) {
    std::ofstream(paths.config_path) << config;
  } else {
    // A settings file from before, or one written by a launcher with only
    // the keys it changed (the Android app saves its settings on Play, before
    // the game has ever run: no gpu_plugin, so nothing could draw - a black
    // screen): the defaults it lacks are added (top-level keys only).
    std::ifstream in(paths.config_path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    // A string broken over lines: launchers up to 2.1.2 wrote a value longer
    // than 1023 bytes (MY MUSIC's song list) back in pieces, a blank line more
    // at every save. The file no longer parsed - every setting ignored, no
    // gpu_plugin: a black screen at start (a 2.1.2 report).
    if (std::string fixed = JoinBrokenTomlStrings(text); fixed != text) {
      g_settings_notes.push_back(
          fmt::format("settings: {} had a value broken over lines - joined again", paths.config_path.string()));
      text = std::move(fixed);
      std::ofstream(paths.config_path, std::ios::binary | std::ios::trunc) << text;
    }
    if (std::string keys, fixed = QuoteBareTomlWords(text, &keys); fixed != text) {
      g_settings_notes.push_back(fmt::format("settings: {} had words without quotes ({}) - quoted, so the file reads again",
                                             paths.config_path.string(), keys));
      text = std::move(fixed);
      std::ofstream(paths.config_path, std::ios::binary | std::ios::trunc) << text;
    }
    if (std::string lines, fixed = SetAsideBrokenTomlLines(text, &lines); fixed != text) {
      g_settings_notes.push_back(fmt::format("settings: {} had lines that can't be read ({}) - set aside as comments, "
                                             "the other settings kept", paths.config_path.string(), lines));
      text = std::move(fixed);
      std::ofstream(paths.config_path, std::ios::binary | std::ios::trunc) << text;
    }
    auto key_of = [](const std::string& line) {
      const size_t a = line.find_first_not_of(" \t");
      const size_t eq = line.find('=');
      if (a == std::string::npos || eq == std::string::npos || line[a] == '#' || line[a] == '[') return std::string();
      size_t e = eq;
      while (e > a && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
      return line.substr(a, e - a);
    };
    std::vector<std::string> have;
    size_t section = std::string::npos;  // where the first [section] starts
    for (size_t pos = 0; pos < text.size();) {
      const size_t nl = text.find('\n', pos);
      const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      const size_t a = line.find_first_not_of(" \t");
      if (a != std::string::npos && line[a] == '[') {
        section = pos;
        break;
      }
      if (std::string k = key_of(line); !k.empty()) have.push_back(k);
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    std::string missing;
    for (size_t pos = 0; pos < config.size();) {
      const size_t nl = config.find('\n', pos);
      const std::string line = config.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      const std::string k = key_of(line);
      if (!k.empty() && std::find(have.begin(), have.end(), k) == have.end()) missing += line + "\n";
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (!missing.empty()) {
      REXLOG_INFO("settings: added the defaults {} lacked:\n{}", paths.config_path.string(), missing);
      if (section == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += missing;
      } else {
        text.insert(section, missing);  // (top-level keys go before the first [section])
      }
      std::ofstream(paths.config_path, std::ios::binary | std::ios::trunc) << text;
    }
  }
  svr2011::PrepareOnlineSettings(paths.config_path, exe_dir);  // (online.h)
}

void Svr2011App::OnPreSetup(rex::RuntimeConfig& config) {
  // Test games (tools/dev/svrtest.ps1 and the other test scripts): SVR2011_AUDIO_MUTE=1 mutes every sound path
  // (they all read audio_mute) even when the command line was dropped - a flag given twice is a parse error that
  // loses every --option, --audio_mute=true with them (27 Oct: a test game was heard).
  if (const char* m = std::getenv("SVR2011_AUDIO_MUTE"); m && *m == '1') {
    rex::cvar::SetFlagByName("audio_mute", "true");
    REXLOG_INFO("settings: audio muted (SVR2011_AUDIO_MUTE)");
  }
  for (const auto& note : g_settings_notes) REXLOG_WARN("{}", note);  // (OnConfigurePaths' repairs)
  g_settings_notes.clear();
  CheckGameFilesThere();  // (a game folder without the disc's essential files: a message, not a crash)
  CheckSupportedXex();     // (another release's default.xex: a message, not a silent exit)
  CheckMacWine();          // (a Mac through Wine: told it isn't supported, may still try)
  // A settings file that couldn't be read (every setting ignored) still draws.
  if (config.gpu_plugin.empty()) {
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    config.gpu_plugin = "xenos";
    REXLOG_WARN("settings: no gpu_plugin (could {} be read?) - using xenos", g_config_path.string());
  }
#if defined(__ANDROID__)
  // The phone draws natively on Vulkan only: a settings file with another
  // value ("any", seen in a player's report: "no backend for the emulator's
  // graphics API" and nothing drew) gets Vulkan.
  if (const std::string api = rex::cvar::Query<std::string>("gpu_backend"); api != "vulkan") {
    rex::cvar::SetFlagByName("gpu_backend", "vulkan");
    REXLOG_WARN("graphics: gpu_backend \"{}\" - the phone draws on Vulkan", api);
  }
#endif
#if defined(_WIN32)
  // "any": Direct3D 12 when it can run the native renderer (Windows 10 2004+,
  // binding tier 2, Shader Model 6), else Vulkan 1.1, else Direct3D 11 (older
  // GPUs / drivers) - gpu_probe.h.
  if (rex::cvar::Query<std::string>("gpu_backend") == "any") {
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    bool cached = false;
    const std::string api = svr2011::PickGraphicsApi(g_user_data / "cache" / "graphics_api.txt", &why, &cached);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (api != "d3d12") {
      rex::cvar::SetFlagByName("gpu_backend", api);
      REXLOG_WARN("graphics: {} - using {} ({:.0f} ms to check{})", why, api == "vulkan" ? "Vulkan" : "Direct3D 11", ms,
                  cached ? ", known for this GPU and driver" : "");
    } else {
      REXLOG_INFO("graphics: Direct3D 12 ({:.0f} ms to check{})", ms, cached ? ", known for this GPU and driver" : "");
    }
  }
  // DLSS on Vulkan: NGX's extensions on the SDK's Vulkan device - asked before it's made, when the game has DLSS
  // (nvngx_dlss.dll beside it), so GRAPHICS can offer it. Only drivers that list them enable them (NVIDIA's
  // VK_NVX_*; buffer device address only with VK_NVX_binary_import): AMD / Intel devices stay as they were.
#if SVR2011_DLSS
  {
    std::error_code ec;
    if (rex::cvar::Query<std::string>("gpu_backend") == "vulkan" &&
        std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "nvngx_dlss.dll", ec))
      svr2011::dlss::RequestVulkanExtensions();
  }
#endif
#endif
  // Automated tests: the only controller is one driven by a command file.
  if (std::string file = Env("SVR2011_INPUT_FILE"); !file.empty()) {
    svr2011::InstallScriptLogSink();  // (now: logging is up, no game thread yet)
    config.input_factory = [file](bool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = std::make_unique<rex::input::InputSystem>(nullptr);
      input->AddDriver(std::make_unique<svr2011::ScriptInputDriver>(file));
      input->AddDriver(svr2011::CreateTouchDriver());  // (tests tap it: "touch")
      input->SetDeviceAssignment(svr2011::CreateControllerWatch());
      return input;
    };
  } else {
    // The SDK's controllers plus the on-screen touch controller
    // (touch_controls.h), merged into player 1's.
    config.input_factory = [](bool tool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = rex::input::CreateDefaultInputSystem(tool);
      if (!tool) {
#if defined(_WIN32)
        // (pad_types.h: PlayStation controllers next to the XInput ones)
        if (REXCVAR_GET(input_backend) == "xinput") {
          auto ps = svr2011::CreatePlayStationDriver();
          using rex::X_STATUS;
          if (ps->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(ps));
        }
#endif
        input->AddDriver(svr2011::CreateTouchDriver());
        input->SetDeviceAssignment(svr2011::CreateControllerWatch());  // (hides it with a controller)
      }
      return input;
    };
  }

#if !defined(_WIN32)
  return;  // SDL audio (the SDK default): XAudio2 is Windows only
#endif
  if (REXCVAR_GET(audio_backend) == "sdl") {
    return;  // the SDK default
  }
  // XAudio2, falling back to SDL if it cannot start (no audio device, ...).
  config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
      -> std::unique_ptr<rex::system::IAudioSystem> {
#if defined(_WIN32)
    if (svr2011::XAudio2AudioSystem::IsAvailable()) {
      return svr2011::XAudio2AudioSystem::Create(dispatcher);
    }
#endif
    REXLOG_WARN("XAudio2 unavailable, using SDL audio");
    return rex::audio::sdl::SDLAudioSystem::Create(dispatcher);
  };
}

void Svr2011App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  SetGuestFrameStats(svr2011::GetFrameStats);  // F3 overlay
  svr2011::InstallFpsOverlay(drawer);          // F2 counter
  if (window()) {
    window()->SetTitle("WWE SmackDown vs. Raw 2011");
  }
}

// The GRAPHICS page's fonts (bold, like the game's menus); ImGui's own
// stays the default for the overlays.
void Svr2011App::OnConfigureFonts(ImFontAtlas* atlas) {
  const char* kBold = "C:\\Windows\\Fonts\\segoeuib.ttf";
  const char* kBoldItalic = "C:\\Windows\\Fonts\\segoeuiz.ttf";
  ImFont* menu = std::filesystem::exists(kBold) ? atlas->AddFontFromFileTTF(kBold, 30.0f) : nullptr;
  ImFont* title =
      std::filesystem::exists(kBoldItalic) ? atlas->AddFontFromFileTTF(kBoldItalic, 40.0f) : nullptr;
  svr2011::SetGraphicsPageFonts(menu, title ? title : menu);
  svr2011::SetOnlineOverlayFonts(menu, title ? title : menu);
  // The touch controller's labels: the menu font, or the phone's own.
  ImFont* touch = menu;
  for (const char* f : {"/system/fonts/Roboto-Bold.ttf", "/system/fonts/Roboto-Regular.ttf"}) {
    if (!touch && std::filesystem::exists(f)) touch = atlas->AddFontFromFileTTF(f, 48.0f);
  }
  svr2011::SetTouchControlsFont(touch);
  svr2011::SetAchievementsPageFonts(menu, title ? title : menu);
  svr2011::SetJukeboxPageFonts(menu, title ? title : menu);
  svr2011::SetBackgroundsPageFonts(menu, title ? title : menu);
  svr2011::SetWeaponsPageFonts(menu, title ? title : menu);
  svr2011::SetReportPageFonts(menu, title ? title : menu);
  svr2011::SetMatchHudFonts(menu, title ? title : menu);
}

void Svr2011App::OnPostLoadXexImage() {
  // The log's first port lines: the build and the settings a problem report
  // needs (SVR2011_PORT_VERSION / _BUILD_COMMIT: CMakeLists.txt).
  {
#ifndef SVR2011_PORT_VERSION
#define SVR2011_PORT_VERSION "dev"
#endif
#ifndef SVR2011_BUILD_COMMIT
#define SVR2011_BUILD_COMMIT "unknown"
#endif
    REXLOG_INFO("==== SvR 2011 PC port {} (build {}) ====", SVR2011_PORT_VERSION, SVR2011_BUILD_COMMIT);
    REXLOG_INFO("game folder: {}", rex::filesystem::GetExecutableFolder().string());
#if defined(_WIN32)
    // (src/utf8.manifest: Windows before 10 version 1903 ignores the UTF-8
    // code page, and a game folder with other letters than A-Z can't be
    // opened everywhere - unread settings, a black screen at the start)
    if (GetACP() != CP_UTF8) {
      const std::u16string folder = rex::filesystem::GetExecutableFolder().u16string();
      if (std::any_of(folder.begin(), folder.end(), [](char16_t c) { return c > 0x7F; }))
        REXLOG_WARN("game folder: its name has letters other than A-Z, which this Windows (before 10 version "
                    "1903) can't open everywhere - if the game starts to a black screen, move it to a folder "
                    "with a plain name (such as C:\\Games\\SvR2011)");
    }
#endif
    static const char* const kGroups[][2] = {
        {"display", "fullscreen fullscreen_exclusive window_width window_height vsync show_fps perf_overlay perf_overlay_corner perf_log_seconds"},
        {"renderer", "native_renderer gpu_backend native_max_scale native_render_scale native_scene_height native_aa native_2x_msaa native_scale_effects "
                     "native_texture_quality native_mat_fix native_texture_packs native_dump_textures native_widescreen native_prepare_pipelines "
                     "frame_rate full_speed unlock_30fps "
                     "process_priority"},
        {"effects", "depth_of_field motion_blur soft_filter"},
        {"gameplay", "replays managers_tile mixed_gender_matches caw_logos_anywhere unlock_everything user_language"},
        {"input", "input_backend mnk_mode touch_controls touch_auto_layout vibration device_vibration"},
        {"audio", "audio_backend audio_mute"},
        {"online", "online_enabled online_server"},
        {"storage", "saves_folder"},
#if defined(__ANDROID__)
        {"android", "gpu_driver gpu_driver_env"},
#endif
    };
    for (const auto& group : kGroups) {
      std::string line;
      std::string names = group[1];
      size_t pos = 0;
      while (pos < names.size()) {
        size_t end = names.find(' ', pos);
        if (end == std::string::npos) end = names.size();
        const std::string name = names.substr(pos, end - pos);
        std::string value = rex::cvar::GetFlagByName(name);
        if (value.empty()) value = "\"\"";
        line += (line.empty() ? "" : ", ") + name + " " + value;
        pos = end + 1;
      }
      REXLOG_INFO("settings ({}): {}", group[0], line);
    }
  }
  // Test aid: SVR2011_TEST_CRASH=1 - a bad memory access 20 s after start, =stack a stack overflow
  // (checks the crash report and its lines in the log).
  if (const std::string crash = Env("SVR2011_TEST_CRASH"); !crash.empty()) {
    std::thread([crash] {
      std::this_thread::sleep_for(std::chrono::seconds(20));
      REXLOG_WARN("SVR2011_TEST_CRASH: crashing on purpose ({})", crash == "stack" ? "stack overflow" : "bad access");
      if (crash == "stack") {
        struct Deep {
          static int Go(volatile int n) {
            volatile char pad[4096];
            pad[0] = char(n);
            return Go(n + 1) + pad[0];
          }
        };
        Deep::Go(0);
      }
      *static_cast<volatile int*>(nullptr) = 1;
    }).detach();
  }
#if defined(_WIN32)
  // Test aid: SVR2011_TEST_LOAD_DLL=<dll> - loads it as an overlay (Discord, RTSS...) gets loaded into the game,
  // and says whether its start-up (DllMain) worked: with 2.1.0's 14.29 VC++ DLLs next to the game, one built
  // for the newer runtime failed there (0xc0000142 when it comes in at the process start).
  if (auto dll = Env("SVR2011_TEST_LOAD_DLL"); !dll.empty()) {
    HMODULE m = LoadLibraryA(dll.c_str());
    DWORD e = m ? 0 : GetLastError();
    char where[MAX_PATH] = "";
    if (HMODULE rt = GetModuleHandleA("msvcp140.dll")) GetModuleFileNameA(rt, where, MAX_PATH);
    if (m)
      REXLOG_INFO("SVR2011_TEST_LOAD_DLL: {} loaded (msvcp140 from {})", dll, where);
    else
      REXLOG_WARN("SVR2011_TEST_LOAD_DLL: {} failed, error {} (msvcp140 from {})", dll, e, where);
  }
#endif
  // Entrances, finishers and other post-processed scenes sample the frame's
  // resolved colour texture through fetch constants whose type field is 0
  // ("invalid"); the Xbox 360 samples them as textures, the emulator's default
  // treats them as unbound (black) - a black screen with only the 2D overlay.
  // (Set here: the flag lives in the GPU plugin, loaded after OnConfigurePaths;
  // it is read at every draw.)
  if (!rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true")) {
    REXLOG_WARN("could not enable gpu_allow_invalid_fetch_constants");
  }
#ifndef SVR2011_D3D_TRACE  // the census build has no native renderer
  svr2011::native::Attach(runtime()->memory());
  if (rex::ui::Window* w = window()) {
    svr2011::native::SetWindowSizeSource([w] {
      return std::pair<uint32_t, uint32_t>(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
    });
  }
#endif

  // The utility drive (cache:\): the game keeps created content there -
  // EditEx.cac (Create modes: superstars, paint tool, ...) and Spectacular.rps
  // (highlight reels); without it those are never written. The SDK leaves
  // cache: unmounted; mount it on a folder next to the saves. (Registered
  // before the game's own XMountUtilityDrive link, which then doesn't replace it.)
  // SVR2011_NO_UTILITY_DRIVE=1 leaves it unmounted (for comparing).
  if (Env("SVR2011_NO_UTILITY_DRIVE").empty()) {
    const std::filesystem::path dir = g_user_data / "UtilityDrive";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto device = std::make_unique<UtilityDriveDevice>(dir);
    auto* fs = runtime()->file_system();
    if (device->Initialize() && fs->RegisterDevice(std::move(device))) {
      fs->RegisterSymbolicLink("cache:", "\\UTILITY");
    } else {
      REXLOG_WARN("could not mount the utility drive (cache:) at {}", dir.string());
    }
  }

  // The port's text and menu changes to the installed disc files (once).
  svr2011::PatchGameFiles(rex::filesystem::GetExecutableFolder());

  // The saves folder chosen in the settings (saves_folder; the launcher's
  // Saves tab sets it). Read here: the settings are loaded after
  // OnConfigurePaths. (It wins over the tests' default beside their user
  // data too: their settings files are their own.)
  if (!REXCVAR_GET(saves_folder).empty()) {
    std::filesystem::path dir = std::filesystem::u8path(REXCVAR_GET(saves_folder));
    if (dir.is_relative()) dir = rex::filesystem::GetExecutableFolder() / dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (std::filesystem::is_directory(dir, ec)) {
      g_saves = dir.lexically_normal();
    } else {
      REXLOG_WARN("saves folder {} can't be used ({}); using {}", dir.string(), ec.message(), g_saves.string());
    }
  }
  REXLOG_INFO("saves: {}", g_saves.string());
  svr2011::CheckCawSaves(g_saves);  // (problem reports: the Created Superstar saves, caw_check.h)

  // Saves as plain files in the Saves folder (saves.h).
  svr2011::UseFlatSaves(runtime()->kernel_state(), g_user_data, g_saves);

  // DLC packages the launcher put in "DLC" (dlc.h).
  {
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    // (the user data the content goes to: <game>\UserData, or a test's)
    svr2011::InstallDlc(runtime()->kernel_state(), exe_dir / "DLC", g_user_data);
  }

  // USER PLAYLIST entrance music from the Music folder (music.h); tests:
  // SVR2011_MUSIC=<folder>.
  {
    std::filesystem::path music = rex::filesystem::GetExecutableFolder() / "Music";
    if (std::string dir = Env("SVR2011_MUSIC"); !dir.empty()) music = dir;
    svr2011::InstallUserMusic(music);
  }

  // USER MOVIES entrance movies from the Custom Movies folder, mounted as
  // umovie: (user_movies.h); tests: SVR2011_MOVIES=<folder>.
  {
    std::filesystem::path movies = rex::filesystem::GetExecutableFolder() / "Custom Movies";
    if (std::string dir = Env("SVR2011_MOVIES"); !dir.empty()) movies = dir;
    svr2011::InstallCaeModMotions(runtime()->memory());  // (CREATE AN ENTRANCE's MODS rows)
    svr2011::InstallCamModMoves(runtime()->memory());    // (CREATE A MOVE-SET's MOD rows)
    svr2011::InstallTextGuard(runtime()->memory());      // (no byte of a name stalls the game's text walks)
    std::error_code ec;
    std::filesystem::create_directories(movies, ec);
    // (HD movies' stubs, hd_movies.h: the folder made before the mount - the
    // device knows the folders it found then)
    std::filesystem::create_directories(movies / ".hd", ec);
    svr2011::CopySuperstarMovies(movies);  // (and the HD movies' stubs)
    if (std::string t = Env("SVR2011_TEST_MOVIE"); t.size() > 4 && t.substr(t.size() - 4) == ".mp4")
      svr2011::PrepareHdMovie(movies, "hdtest.bik", t);  // (tests: user_movies.cpp)  // (arenas branch: superstar_mods.h)
    svr2011::CopyMediaMovies(movies);      // (arenas branch: media_mods.h)
    // (before the mount too: its scan writes the folder's own .mp4s' stubs and
    // renames accented names - files made after it don't open until the next
    // start: a new USER MOVIES .mp4 stayed black)
    svr2011::InstallUserMovies(runtime()->memory(), movies);
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\USERMOVIES", movies, true);
    auto* fs = runtime()->file_system();
    if (device->Initialize() && fs->RegisterDevice(std::move(device))) {
      fs->RegisterSymbolicLink("umovie:", "\\USERMOVIES");
    } else {
      REXLOG_WARN("could not mount the Custom Movies folder {}", movies.string());
    }
  }

  REXLOG_INFO("platform: {}", svr2011::PlatformDescription());

  // The mods below can take a while on a cold hard drive: the window keeps
  // answering Windows meanwhile (startup_pump.h).
  svr2011::StartupThread() = std::this_thread::get_id();

  // MY WWE -> OPTIONS -> GRAPHICS (menu_hooks.cpp, graphics_page.h).
  svr2011::InstallMenuHooks(runtime()->memory());
  svr2011::InstallPerfStats(runtime()->memory());  // (perf_stats.h: the overlay's numbers, perf_log_seconds)
#if SVR2011_DEBUG_SUITE
  svr2011::dbg::Install(runtime()->memory());  // (debug_suite.h: the dbg build only)
#endif
  svr2011::InstallPadIcons(runtime()->memory());  // (pad_icons.h)
  svr2011::InstallPlaytime();                     // (playtime.h)
  svr2011::InstallCawLogos(runtime()->memory(), g_saves);
  // PC keyboard typing in the game's on-screen keyboard (keyboard_typing.h).
  svr2011::InstallKeyboardTyping(window());
  svr2011::InstallFreeCam(window());  // (F5-F8: free_cam.h)
  // 10 pages of Paint Tool logos (paint_pages.h).
  svr2011::InstallPaintPages(runtime()->memory(),
                             static_cast<rex::input::InputSystem*>(runtime()->input_system()), g_saves);
  // More exhibition match types (match_types.h).
  svr2011::InstallMatchTypes(runtime()->memory());
  // Arena mods: custom arenas in place of a host arena (arena_mods.h).
  svr2011::KeepWindowAlive();
  {
    // (the installed DLC packages - dlc.cpp - whose own m / mpsp pacs get copies with the packs in the overlay)
    char title[16];
    std::snprintf(title, sizeof(title), "%08X", runtime()->kernel_state()->title_id());
    svr2011::InstallMovePacks(runtime()->file_system(),  // (arenas branch: before the game mounts its pacs)
                              g_user_data / "0000000000000000" / title / "00000002");
  }
  // Weapon mods: new weapons in the game's weapon tables (weapon_mods.h) - before the arena mods: backstage rows name
  // them in load_weapons= / weapons=.
  svr2011::SetWeaponModsMemory(runtime()->memory());
  svr2011::LoadWeaponMods();
  svr2011::KeepWindowAlive();
  svr2011::InstallArenaMods(runtime()->memory(), runtime()->file_system());
  svr2011::KeepWindowAlive();
  // Ring Kit: rope heights and rules from a custom arena's manifest (ring_rules.h).
  svr2011::InstallRingRules(runtime()->memory());
  svr2011::InstallCawLimits(runtime()->memory());  // (the CAW editor's part heap fallback: caw_limits.h)
  svr2011::InstallJukebox(runtime()->memory());  // MY WWE -> JUKEBOX (jukebox.h)
  // Superstar mods: new characters in the free DLC slots (superstar_mods.h).
  svr2011::InstallMediaMods(runtime()->memory());  // (arenas branch: before the superstar mods mount its pac)
  svr2011::InstallVsStyles(runtime()->memory());   // (the same: its overlay pac)
  svr2011::KeepWindowAlive();
#ifndef SVR2011_D3D_TRACE
  svr2011::InstallModelMem(runtime()->memory());  // (model_mem.cpp)
#endif
  svr2011::SetThreadsSaves(g_saves);  // (the mods' Superstar Threads slots, kept by folder)
  {
    char title[16];
    std::snprintf(title, sizeof(title), "%08X", runtime()->kernel_state()->title_id());
    svr2011::SetThreadsDlc(g_user_data / "0000000000000000" / title / "00000002");  // (the DLCs' own Threads lists)
  }
  svr2011::SetSuperstarModsSaves(g_saves);  // (a fetched mod's id as the save file has it)
  svr2011::InstallSuperstarMods(runtime()->memory(), runtime()->file_system());
  svr2011::InstallUniverseWelcome(g_saves, g_user_data);  // (the Universe welcome kept as seen once accepted)
  svr2011::InstallUniverseMods(runtime()->memory(), g_saves);  // (the mods' WWE Universe side table: after the mods)
  svr2011::KeepWindowAlive();
  svr2011::InstallCrowdSigns(runtime()->memory());  // (after the superstar mods: their signs)
  svr2011::KeepWindowAlive();
  // The frame rate: 30 / 60 / 120 / 144 / 240 at the game's speed (frame_rate.h).
  // (after the runtime's memory fault handler: faults it doesn't take are placed in the log, never retried forever)
  svr2011::InstallGuestFaultLog();
  svr2011::InstallFrameRate(runtime()->memory());
  svr2011::InstallPerfSettings();  // (perf_hooks.h)
  if (imgui_drawer()) {
    // MY WWE -> ACHIEVEMENTS (achievements_page.h).
    svr2011::InstallAchievementsPage(
        imgui_drawer(), immediate_drawer(), runtime(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
    svr2011::InstallJukeboxPage(imgui_drawer(), static_cast<rex::input::InputSystem*>(runtime()->input_system()));
    svr2011::InstallBackgroundsPage(imgui_drawer(), static_cast<rex::input::InputSystem*>(runtime()->input_system()));
    svr2011::InstallWeaponsPage(imgui_drawer(), static_cast<rex::input::InputSystem*>(runtime()->input_system()));
    svr2011::InstallReportPage(imgui_drawer(), immediate_drawer(), window(),
                               static_cast<rex::input::InputSystem*>(runtime()->input_system()), runtime()->memory(),
                               g_user_data);  // (report_page.h)
    svr2011::InstallGraphicsPage(
        imgui_drawer(), window(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()), g_config_path);
    // The on-screen controller (touch_controls.h).
    svr2011::InstallTouchControls(imgui_drawer(), window(), g_user_data, immediate_drawer());
    svr2011::InstallPaintPagesOverlay(imgui_drawer());
    svr2011::InstallSlobberKnockerOverlay(imgui_drawer());  // (match_types.h)
    svr2011::InstallHctpOverlay(imgui_drawer());            // (match_types.h: HCTP door prompts)
    svr2011::InstallCrowdOverlay(imgui_drawer());           // (crowd_fight.h: the climb into the crowd)
    svr2011::InstallGauntletEliminatorOverlay(imgui_drawer());
    svr2011::InstallOnlineRulesOverlay(imgui_drawer());  // (online_rules.h)
    svr2011::InstallThreeStagesOverlay(imgui_drawer());
    svr2011::InstallMysteryOpponentOverlay(imgui_drawer());  // (the hidden opponent's "?")
    // The ONLINE overlay: friends, invites (online_overlay.h).
    svr2011::InstallOnlineOverlay(imgui_drawer(), window(),
                                  static_cast<rex::input::InputSystem*>(runtime()->input_system()),
                                  runtime()->kernel_state(), g_user_data);
    svr2011::InstallArenaModsOverlay(imgui_drawer());
    svr2011::InstallStoryDesignerOverlay(imgui_drawer());
  }
  {
    svr2011::StartDiscordPresence();  // (discord_presence.h)
  }
  svr2011::InstallOnline(runtime()->memory(), g_saves);  // Community Creations (online.h)
  // ... its Superstars' entrance songs and movies (entrance_media.h)
  svr2011::InstallEntranceMedia(runtime()->memory());
  // Online matches, peer to peer (p2p.h)
  svr2011::InstallP2P(runtime()->memory());
  // ... and its Created Superstars' Paint Tool data, peer to peer (online_cas.h)
  svr2011::InstallOnlineCas(g_saves);
  // ... and the online leaderboards (leaderboards.h)
  svr2011::InstallLeaderboards(runtime()->memory(), runtime()->kernel_state());
  svr2011::InstallOnlineMods(runtime()->memory());  // (superstar / arena mods in online lobbies)
  svr2011::InstallModTransfer();  // (the host's mods to joiners who lack them, P2P: mod_transfer.h)
  svr2011::InstallOnlineChat();  // (lobby chat longer than the game's)

  // Developer aid: SVR2011_DUMP_IMAGE=<file> writes the loaded (decrypted,
  // decompressed) executable image for analysis tools (port/tools/).
  if (std::string dump = Env("SVR2011_DUMP_IMAGE"); !dump.empty()) {
    const uint8_t* image = runtime()->virtual_membase() + REX_IMAGE_BASE;
    std::ofstream(dump, std::ios::binary)
        .write(reinterpret_cast<const char*>(image), REX_IMAGE_SIZE);
  }
}

// The window closing (the app hard-exits after: no atexit) - the router's
// port mapping removed first.
bool Svr2011App::OnWindowCloseRequested() {
  svr2011::StopPortMapping();
  svr2011::OnlineOverlaySayGoodbye();  // (shown offline at once)
  return true;
}
