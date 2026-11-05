// WWE SmackDown vs. Raw 2011 - online services (see online.h).

#include "online.h"

#include "generated/default/svr2011_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>


#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "caw_logos.h"
#include "entrance_media.h"
#include "move_packs.h"
#include "online_files.h"
#include "online_net.h"
#include "online_overlay.h"
#include "online_chat.h"
#include "p2p.h"
#include "paint_pages.h"
#include "../modmaker/svrfmt/bytes.h"

namespace svr2011 {

namespace {

// The AuthService URL (the game's own SSL engine; the server speaks http).
constexpr uint32_t kAuthUrl = 0x82087480;
constexpr char kAuthUrlHttps[] = "https://%s.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx";
constexpr char kAuthUrlHttp[] = "http://%s.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx";

// The RSA public key (hex modulus; exponent 010001) login certificates are
// checked against: GameSpy's, replaced by the one gamespy_server.py signs with.
constexpr uint32_t kCertKeyModulus = 0x82087370;
constexpr char kGameSpyModulus[] =
    "BF05D63E93751AD4A59A4A7389CF0BE8A22CCDEEA1E7F12C062D6E194472EFDA5184CCECEB4FBADF5EB1D7ABFE911814"
    "53972AA971F624AF9BA8F0F82E2869FB7D44BDE8D56EE50977898F3FEE75869622C4981F07506248BD3D092E8EA05C12"
    "B2FA37881176084C8F8B8756C4722CDC57D2AD28ACD3AD85934FB48D6B2D2027";
constexpr char kServerModulus[] =
    "EFD6AB4EF900EFFDD0634EE8B5C5E7355B06831E51DF9380E5D73B1516E63D7BE3F6A3323AC4D402CAEAE7250885D7DC"
    "C755F16B77DC7C1B2D14ED9D8CC926DE48F3675C63499910398B12D27B04FF858CB1F928F5CE0A6F2C4841AD3EA0DA13"
    "AEAA68893865509FEDB7F559F52A94DC0B26D145918CF8FEA1F84DF47506EA63";
static_assert(sizeof(kGameSpyModulus) == sizeof(kServerModulus));

// Replaces the string at address, if it is the expected one.
bool Patch(rex::memory::Memory* memory, uint32_t address, const char* expected, const char* text,
           size_t size) {
  char* p = memory->TranslateVirtual<char*>(address);
  if (std::memcmp(p, expected, std::strlen(expected) + 1) != 0) {
    REXLOG_WARN("online: unexpected data at {:08X} (not patched)", address);
    return false;
  }
  // (the image is read-only: writable for the copy)
  auto* heap = memory->LookupHeap(address);
  uint32_t old = 0;
  if (!heap || !heap->QueryProtect(address, &old) ||
      !heap->Protect(address, uint32_t(size), rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_WARN("online: {:08X} can't be made writable (not patched)", address);
    return false;
  }
  std::memcpy(p, text, size);
  heap->Protect(address, uint32_t(size), old);
  return true;
}

}  // namespace

void PrepareOnlineSettings(const std::filesystem::path& config, const std::filesystem::path& exe_dir) {
  std::vector<std::string> lines;
  {
    std::ifstream in(config);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  auto key_of = [](const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == '[') return std::string();
    std::string key = line.substr(0, eq);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    return key;
  };
  // (top-level keys go before the first [section])
  auto set = [&](const std::string& key, const std::string& line) {
    size_t end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      if (!lines[i].empty() && lines[i][0] == '[') {
        end = i;
        break;
      }
      if (key_of(lines[i]) == key) {
        lines[i] = line;
        return;
      }
    }
    while (end > 0 && lines[end - 1].empty()) --end;  // (before the blank lines)
    lines.insert(lines.begin() + end, line);
  };
  bool changed = false;
  const std::filesystem::path brought = exe_dir / "online.toml";
  if (std::filesystem::exists(brought)) {
    std::ifstream in(brought);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string key = key_of(line);
      if (key == "online_enabled" || key == "online_name" || key == "online_server" || key == "online_token" ||
          key == "online_xuid") {  // (the PC's account comes along)
        set(key, line);
        changed = true;
      }
    }
    in.close();
    std::error_code ec;
    std::filesystem::remove(brought, ec);
    REXLOG_INFO("online: settings from {}", brought.string());
  }
  bool has_xuid = false;
  for (const auto& line : lines) has_xuid |= key_of(line) == "online_xuid";
  if (!has_xuid) {
    std::random_device rd;
    const uint64_t id = ((uint64_t(rd()) << 32) | rd()) & 0x0000FFFFFFFFFFFFull;
    char text[64];
    std::snprintf(text, sizeof(text), "online_xuid = \"%016llX\"", 0x0009000000000000ull | id);
    set("online_xuid", text);
    changed = true;
  }
  if (changed) {
    std::ofstream out(config, std::ios::trunc);
    for (const auto& line : lines) out << line << '\n';
  }
}

namespace {
uint32_t g_empty_string = 0;  // guest "" (removed labels)
// guest: why online is off (online_files.h: modified game files, an older
// version, still checking) - filled when the game asks for the text
constexpr uint32_t kOffTextSize = 256;
uint32_t g_online_off_text = 0;
rex::memory::Memory* g_online_memory = nullptr;
// guest: the game's "no game sessions" / join failure message with why hosts
// were left out (p2p.h P2PHiddenReason: another version or other mods)
uint32_t g_hidden_text = 0;
// guest: the CPU slots' help bar labels (online_cpu.cpp)
constexpr uint32_t kAddCpuId = 0xEF10, kRemoveCpuId = 0xEF11, kWaitingId = 0xEF12;  // (16-bit: the bar holds u16 ids; blank in the game's table)
constexpr uint32_t kPlayerInfoId = 0xB0A7;  // PLAYER INFO: the lobby's own Y label - its slot's icon is Y's glyph
constexpr uint32_t kYIconGuess = 0xAFCA;    // (before one was seen; 0xAFCB is X's)
uint32_t g_cpu_labels[3] = {};
uint32_t g_scratch = 0;  // (OnlineScratch)
// guest: CC's "uses mods" refusal (CcUploadModsRefused, below)
constexpr uint32_t kModsRefusedId = 0xEF13;  // (16-bit, blank in the game's table, as 0xEF10-0xEF12)
uint32_t g_mods_text = 0;
std::string g_mods_reason;
// (string ids: from SVR2011_DUMP_STRINGS)
constexpr uint32_t kNoSessionsId = 0xC6D4;  // "No game sessions were found. Search again?"
constexpr uint32_t kJoinFailedId = 0xC67F;  // "Failed to find a session. Please try again." (an invite)

// Text into a guest buffer (kOffTextSize).
uint32_t GuestText(uint32_t buffer, const std::string& text) {
  if (!buffer || !g_online_memory) return 0;
  char* out = g_online_memory->TranslateVirtual<char*>(buffer);
  const size_t n = std::min<size_t>(text.size(), kOffTextSize - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
  return buffer;
}

// -- The Superstars' extra logos (Saves\.logos) and the server ----------------

std::filesystem::path g_saves;  // the saves folder (Saves\.logos: caw_logos.cpp)

uint64_t Fnv64(const std::string& data) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : data) h = (h ^ c) * 1099511628211ull;
  return h;
}

constexpr size_t kLogoSize = 1024 + 65536;  // a stored logo: palette + pixels (caw_logos.cpp)

// The server asks for the logos (Saves\.logos) of Superstars that were
// uploaded but whose logos it doesn't have: the ones this player has go up.
// (The game's own upload carries only the Superstar file.)
void LogoUploads() {
  rex::thread::set_current_thread_name("logo uploads");
  for (;;) {
    auto wanted = net::ServerRequest("GET", "/api/logos/wanted");
    if (wanted && wanted->status == 200) {
      std::istringstream lines(wanted->body);
      for (std::string hash; std::getline(lines, hash);) {
        if (hash.size() != 16) continue;
        std::ifstream in(g_saves / ".logos" / (hash + ".bin"), std::ios::binary);
        if (!in) continue;
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() != kLogoSize) continue;
        auto r = net::ServerRequest("POST", "/api/logo/" + hash, data, {{"Content-Type", "application/octet-stream"}});
        REXLOG_INFO("online: logo {} sent to the server ({})", hash,
                    r && r->headers["svr2011-result"] == "0" ? "stored" : "refused");
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}

// A logo a Superstar shows but this PC doesn't have (one downloaded from
// Community Creations): from the server, checked against its hash, into the
// store.
bool FetchLogo(uint64_t hash, const std::filesystem::path& file) {
  char name[17];
  std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(hash));
  auto r = net::ServerRequest("GET", std::string("/api/logo/") + name);
  if (!r || r->status != 200) return false;
  const std::string* data = &r->body;
  if (data->size() != kLogoSize || Fnv64(*data) != hash) return false;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data->data(), std::streamsize(data->size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, file, ec);
  return !ec;
}

void OnMissingLogo(uint64_t hash, const std::filesystem::path& file) {
  std::thread([hash, file] {
    const bool ok = FetchLogo(hash, file);
    REXLOG_INFO("online: logo {:016X} {}", hash, ok ? "downloaded" : "not on the server");
  }).detach();
}

// -- Community Creations downloads: checked, and what they replace backed up --
//
// The game's download (all content types) runs on one manager's state
// machine: [mgr+1176] state, [mgr+1172] content type (0 Created Superstar,
// 1 story, 3 Paint Tool logo, 4 highlight reel, 2/5 kept in SaveData.dat),
// [mgr+1424] the file size the server gave, the bytes in the 12 MB buffer at
// **0x82E3DD9C. State 46 (sub_824D8F58) has the whole file and only checks a
// 16-bit sum, then the game re-signs it and saves it over the slot the player
// picks. Here the file is checked first (a bad one gets the game's own
// "Download failed.", state 49, and nothing is written), and each save file a
// download replaces is copied to Saves\.backup first.

constexpr uint32_t kDownloadBuffer = 0x82E3DD9C;  // -> -> the 12 MB buffer
constexpr uint32_t kDownloadMax = 0xC00000;
constexpr uint32_t kCawFileSize = 1347532, kPaintFileSize = 394516, kReelFileSize = 1555802;
constexpr uint32_t kStoryMax = 0x6CCC94;

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32;
std::atomic<bool> g_importing{false};  // a download is being saved

// Copies a save file before a download replaces it: Saves\.backup\<name>.<time>
// (the last 10 of each kept).
void BackupSave(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return;
  const auto dir = g_saves / ".backup";
  std::filesystem::create_directories(dir, ec);
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const std::string name = file.filename().string();
  std::filesystem::copy_file(file, dir / (name + "." + stamp), std::filesystem::copy_options::overwrite_existing, ec);
  REXLOG_INFO("online: {} backed up before the download replaces it{}", name, ec ? " (failed: " + ec.message() + ")" : "");
  std::vector<std::filesystem::path> old;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().filename().string().rfind(name + ".", 0) == 0) old.push_back(e.path());
  }
  std::sort(old.begin(), old.end());
  for (size_t k = 0; k + 10 < old.size(); ++k) std::filesystem::remove(old[k], ec);
}

// Why a downloaded file can't be used, or "" if it can.
std::string BadDownload(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t buf, uint32_t size) {
  if (!buf || !size || size > kDownloadMax) return fmt::format("size {}", size);
  switch (type) {
    case 0:  // Created Superstar: its size and header (0x4B, 0x18, 7 in every .cas)
      if (size != kCawFileSize) return fmt::format("size {} (a Superstar is {})", size, kCawFileSize);
      if (Rd32(base, buf) != 0x4B || Rd32(base, buf + 4) != 0x18 || Rd32(base, buf + 8) != 7) {
        return fmt::format("not a Superstar (header {:08X} {:08X} {:08X})", Rd32(base, buf), Rd32(base, buf + 4),
                           Rd32(base, buf + 8));
      }
      return {};
    case 3:  // Paint Tool logo: one slot, 256x256
      if (size != kPaintFileSize) return fmt::format("size {} (a logo is {})", size, kPaintFileSize);
      if (Rd32(base, buf) != 3 || Rd32(base, buf + 36) != 256 || Rd32(base, buf + 40) != 256) {
        return "not a 256x256 Paint Tool logo";
      }
      return {};
    case 4:
      if (size != kReelFileSize) return fmt::format("size {} (a highlight reel is {})", size, kReelFileSize);
      return {};
    case 1:
      if (size > kStoryMax) return fmt::format("size {} (a story is at most {})", size, kStoryMax);
      return {};
    default:
      return {};
  }
}

// A downloaded Superstar's extra logos (caw_logos.cpp's 'XLG1' record, at the
// same place as in its .cas): fetched now, before it is first shown.
void PrefetchLogos(const uint8_t* caw) {
  constexpr uint32_t kExt = 20 + 160000;
  uint32_t magic;
  std::memcpy(&magic, caw + kExt, 4);
  if (magic != 0x584C4731) return;
  for (int k = 0; k < 10; ++k) {
    uint64_t hash;
    std::memcpy(&hash, caw + kExt + 24 + 8 * k, 8);
    if (!caw[kExt + 8 + k] || !hash) continue;
    char name[24];
    std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(hash));
    const auto file = g_saves / ".logos" / name;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) continue;
    OnMissingLogo(hash, file);
  }
}

// The Paint Tool page / file a logo download goes to (paint_pages.h calls
// this just before the game's write).
bool OnPaintDownload(uint8_t* base, uint32_t screen, int page) {
  const uint32_t slot = Rd32(base, screen + 0x8344) % 20;  // (0-199 over the 10 pages)
  char name[32];
  if (page == 0) {
    BackupSave(g_saves / "00PaintTool.pt");
  } else if (slot < 20) {
    std::snprintf(name, sizeof(name), "p%02d_s%02u.bin", page + 1, slot + 1);
    BackupSave(g_saves / ".paint" / name);
  }
  return true;
}
}  // namespace

uint32_t OnlineString(uint32_t id) {
  OnlineNoteString(id);  // (online_overlay.h: where the menus' cursors are)
  switch (id) {
    case 0x5339:  // VIEW GAMER CARD (button help)
    case 0xB09B:
    case 0xB0C8:
    case 0xB0AE:  // Xbox LIVE Party
    case 0xB0B2:  // Invite Xbox LIVE Party
    case 0xB15D:  // Invite Xbox LIVE Party Member
      return g_empty_string;
    // The trial period messages: what entering ONLINE shows while it is off
    // for the game files (the Online Axxess gate below) - why, instead.
    case 50866:
    case 50867: {
      const std::string why = GameFilesOnlineMessage();
      return why.empty() ? 0 : GuestText(g_online_off_text, why);
    }
    default:
      break;
  }
  if (id >= kAddCpuId && id <= kWaitingId && g_cpu_labels[id - kAddCpuId]) return g_cpu_labels[id - kAddCpuId];
  if (id == kModsRefusedId && !g_mods_reason.empty()) return GuestText(g_mods_text, g_mods_reason);  // (CC: uses mods)
  // (the match ended because its host left: say so - the game's own words were
  // "An error occurred during the match")
  if (id == 0xC6A2 && P2PHostLeft())
    return GuestText(g_hidden_text, "The host left the session.\nThe match is over.");
  // The search found only hosts this game can't play (another version or
  // other gameplay mods): the game's own message says so.
  if (id && (id == kNoSessionsId || id == kJoinFailedId)) {
    int sessions = 0;
    const std::string why = P2PHiddenReason(&sessions);
    if (why.empty()) return 0;
    // (the game's own wording - its C67E / C67C messages - then the details)
    const bool mods = why.rfind("Your mods", 0) == 0 || why.rfind("Your downloadable content", 0) == 0;
    const std::string head = mods ? "Unable to join the session due to a difference in downloadable content."
                                  : "Unable to join the session due to a difference in game versions.";
    if (id == kNoSessionsId)
      return GuestText(g_hidden_text, std::string(sessions == 1 ? "A game session was found. " : "Game sessions were found. ") +
                                          head + "\n" + why + "\nSearch again?");
    return GuestText(g_hidden_text, head + "\n" + why);
  }
  return 0;
}

// The game fetches the news (the server's Sake "Newsfeed" record) once a
// session into its online state - UTF-8 at *0x82E3DDFC + 0x2248 - and builds
// the panel from there at each visit to ONLINE. The dashboard's news (the
// server's online_news) is written there, so it shows the next time without a
// restart. Only over a news text from our server (its "N players online now."
// line): never into memory that isn't the news. (8 KB stored there by the game
// itself in a test; the server sends 600 characters at most.)
void SetOnlineNews(const std::string& text) {
  constexpr uint32_t kOnlineState = 0x82E3DDFC, kNewsAt = 0x2248, kNewsMax = 4096;
  if (!g_online_memory || text.empty() || text.size() >= kNewsMax) return;
  const uint8_t* p = g_online_memory->TranslateVirtual<const uint8_t*>(kOnlineState);
  const uint32_t state = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  if (state < 0x40000000 || state >= 0xF0000000) return;
  char* news = g_online_memory->TranslateVirtual<char*>(state + kNewsAt);
  const size_t now = strnlen(news, kNewsMax);
  if (now == kNewsMax || std::string_view(news, now).find(" online now. ") == std::string_view::npos) return;
  if (std::string_view(news, now) == text) return;
  std::memcpy(news + 1, text.data() + 1, text.size() - 1);  // (the first byte last: never half a text)
  news[text.size()] = 0;
  news[0] = text[0];
  REXLOG_INFO("online: news panel updated ({} bytes, shows at the next visit)", text.size());
}

void InstallOnline(rex::memory::Memory* memory, const std::filesystem::path& saves) {
  g_empty_string = memory->SystemHeapAlloc(4);
  if (g_empty_string) std::memset(memory->TranslateVirtual<char*>(g_empty_string), 0, 4);
  g_online_memory = memory;
  g_online_off_text = memory->SystemHeapAlloc(kOffTextSize);
  g_hidden_text = memory->SystemHeapAlloc(kOffTextSize);
  g_mods_text = memory->SystemHeapAlloc(kOffTextSize);
  g_scratch = memory->SystemHeapAlloc(4096);
  {
    static const char* const kLabels[3] = {"ADD CPU", "REMOVE CPU", "WAITING FOR ONE MORE PLAYER"};
    for (int i = 0; i < 3; ++i)
      if ((g_cpu_labels[i] = memory->SystemHeapAlloc(32)) != 0)
        std::strncpy(memory->TranslateVirtual<char*>(g_cpu_labels[i]), kLabels[i], 31);
  }
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  g_saves = saves;
  svr2011::StartGameFilesCheck();  // (online_files.h: the game files and the newest version)
  net::StartRelay();
  SetMissingLogoHandler(OnMissingLogo);
  SetPaintDownloadCheck(OnPaintDownload);
  std::thread(LogoUploads).detach();
  const bool url = Patch(memory, kAuthUrl, kAuthUrlHttps, kAuthUrlHttp, sizeof(kAuthUrlHttp));
  const bool key = Patch(memory, kCertKeyModulus, kGameSpyModulus, kServerModulus, sizeof(kServerModulus));
  REXLOG_INFO("online: on, server {} (auth over http: {}, server key: {})",
              rex::cvar::Query<std::string>("online_server"), url, key);
}

}  // namespace svr2011

// Community Creations upload of a Paint Tool logo: the game refuses logos
// flagged "not made here" (data+32) - pictures loaded from outside the Paint
// Tool, which includes the launcher's Paint Tool imports, and logos drawn
// from them. Here only logos downloaded from Community Creations (data+13)
// are refused: players share the art they bring in.
REX_EXTERN(__imp__sub_828D1648);
REX_HOOK_RAW(sub_828D1648) {
  const uint32_t data = ctx.r3.u32;
  ctx.r3.u64 = base[data + 13] == 0 ? 1 : 0;
}

// Community Creations' Created Superstar upload list: sub_8257BCD0(profile, r4) - the save's CAW record
// (sub_8257A450: save + 0x13F9C + k * 0x6BC) - is true for one that can be uploaded. It isn't when any of: the
// record's +1677 (an online-only CAW), +1716..1719 / +1720..1723 (per-attire marks), the CAW's roster entry (36
// bytes, runtime table at 0x82EBE9FC by the id at +32) +17 or +32, a DLC part (sub_8257ABB8 / sub_8257BAC0 - the
// game's own rule: "purchased content cannot be uploaded"). A player report (2.1.2: "it says I don't have any I can
// upload, and I made it on this version") is read through this: each CAW the list leaves out is logged once a run
// with the reason, so the next report says which.
REX_EXTERN(__imp__sub_8257BCD0);
REX_EXTERN(__imp__sub_8257ABB8);
REX_HOOK_RAW(sub_8257BCD0) {
  const uint32_t profile = ctx.r3.u32;
  const auto saved = ctx;
  __imp__sub_8257BCD0(ctx, base);
  if (ctx.r3.u32 || !profile) return;
  static std::set<uint32_t> said;
  const uint8_t* p = base + profile;
  const int16_t id = int16_t(uint16_t(p[32]) << 8 | p[33]);
  if (!said.insert(uint32_t(uint16_t(id))).second) return;
  std::string why;
  auto add = [&](const std::string& s) { why += (why.empty() ? "" : ", ") + s; };
  if (p[1677]) add("online-only CAW (+1677 = " + std::to_string(p[1677]) + ")");
  for (int a = 0; a < 4; ++a)
    if (p[1716 + a]) add("attire " + std::to_string(a + 1) + " mark +1716 = " + std::to_string(p[1716 + a]));
  for (int a = 0; a < 4; ++a)
    if (p[1720 + a]) add("attire " + std::to_string(a + 1) + " mark +1720 = " + std::to_string(p[1720 + a]));
  if (id >= 0 && id < 322) {
    const uint8_t* rec = base + 0x82EBE9FCu + uint32_t(id) * 36;
    if (rec[17]) add("downloaded (+17 = " + std::to_string(rec[17]) + ")");
    if (rec[32]) add("roster +32 = " + std::to_string(rec[32]));
    auto c = saved;
    c.r3.u64 = 0x82EBE9FCu + uint32_t(id) * 36, c.r4.u64 = uint64_t(id);  // (sub_8257ABB8(rec, id): a DLC part)
    __imp__sub_8257ABB8(c, base);
    if (c.r3.u32) add("a DLC part (purchased content)");
  }
  char name[64] = {};
  for (int i = 0; i < 31; ++i) {  // (its name: UTF-16BE at the record's start)
    const char ch = char(p[2 * i + 1]);
    if (!ch || p[2 * i]) break;
    name[i] = ch;
  }
  REXLOG_INFO("online: CAW id {} \"{}\" left out of the upload list: {}", id, name, why.empty() ? "a DLC part or a part the game marks" : why);
}


// Community Creations upload of a story: the game's check (sub_82629160,
// asked only by the upload confirm sub_82511C18) says "This Created Content
// was created by a different player" (string 50951) when the story's 15
// entries at +0x2801A5 (stride 0x4E9EC) have one present but not the
// player's own, when a scene (500 at +20112, stride 4056) has a created
// Superstar that isn't the player's (sub_8257BAC0: downloaded, other DLC
// parts...), or when the story itself was downloaded (+84+13). Stories made
// on another console or under another online identity - the PC's identity
// changes when the player signs in - were refused. Here only downloaded
// stories are.
REX_EXTERN(__imp__sub_82629160);
REX_HOOK_RAW(sub_82629160) {
  const uint32_t story = ctx.r3.u32;
  __imp__sub_82629160(ctx, base);
  if (ctx.r3.u32) return;
  const bool downloaded = base[story + 84 + 13] != 0;
  bool entries = true;
  for (uint32_t k = 0, p = story + 0x2801A5; k < 15; ++k, p += 0x4E9EC) {
    if (base[p - 0x4E9C7] && !base[p]) entries = false;
  }
  REXLOG_INFO("online: story upload: the game refused it ({}){}", downloaded ? "downloaded" :
              entries ? "a created Superstar in its scenes" : "one of its entries",
              downloaded ? "" : " - uploading it");
  if (!downloaded) ctx.r3.u64 = 1;
}

// The menus' button help bar: sub_82746358(table, bar id, out) fills out with
// three slots of two string ids (u16 at +0 and +2 of each 4 bytes: the
// button's and the label's; 0xFFFF: none), which sub_8274A458 shows. The PC's removed
// labels (OnlineString) go with their button.
REX_EXTERN(__imp__sub_82746358);
REX_HOOK_RAW(sub_82746358) {
  // Test aid SVR2011_TEST_CAW_UPLOAD=1: Community Creations' upload check (sub_8257BCD0, above) on every Created
  // Superstar of the loaded save, once, at a menu - offline, no account (the hook logs the left-out ones' reasons).
  if (static bool tested = false; !tested && std::getenv("SVR2011_TEST_CAW_UPLOAD")) {
    const uint32_t save = (uint32_t(base[0x82E407B4]) << 24) | (uint32_t(base[0x82E407B5]) << 16) |
                          (uint32_t(base[0x82E407B6]) << 8) | base[0x82E407B7];
    int n = 0;
    for (uint32_t k = 0; save && k < 50; ++k) n += base[save + 0x13F9C + k * 0x6BC + 34] != 0;
    if (n) {  // (the save loaded - not the empty default one)
      tested = true;
      n = 0;
      for (uint32_t k = 0; k < 50; ++k) {
        const uint32_t profile = save + 0x13F9C + k * 0x6BC;
        if (!base[profile + 34]) continue;  // (sub_8257B000: a CAW in this slot)
        // (SVR2011_TEST_CC_MODMOVE=<slot>:<move id>: that move into the CAW's first moveset slot - in memory)
        if (const char* v = std::getenv("SVR2011_TEST_CC_MODMOVE"); v && uint32_t(std::atoi(v)) == k)
          if (const char* c = std::strchr(v, ':')) {
            const int id = std::atoi(c + 1);
            base[profile + 0x104 + 2 * 3] = uint8_t(id >> 8), base[profile + 0x104 + 2 * 3 + 1] = uint8_t(id);
          }
        auto c = ctx;
        c.r3.u64 = profile, c.r4.u64 = 0;
        sub_8257BCD0(c, base);
        ++n;
        const auto uses = svr2011::CawModUses(base, profile);
        std::string list;
        for (const auto& u : uses) list += (list.empty() ? "" : ", ") + u;
        REXLOG_INFO("online test: CAW slot {} upload check: {}; mods: {}", k, c.r3.u32 ? "can be uploaded" : "left out",
                    list.empty() ? "none" : list);
      }
      REXLOG_INFO("online test: {} CAWs checked", n);
    }
  }
  const uint32_t out = ctx.r5.u32;
  // (the Player Match lobby's CPU slots put the Y label's state in the id's
  // high half - online_cpu.cpp's sub_824E3E60 hook: 1 + y + 3 * waiting)
  const uint32_t cpu = ctx.r4.u32 >> 16;
  ctx.r4.u64 = ctx.r4.u32 & 0xFFFF;
  {
    static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
    static uint32_t last = 0xFFFFFFFF;
    if (log && ctx.r4.u32 != last) REXLOG_INFO("online: help bar {} (lr {:08X})", ctx.r4.u32, uint32_t(ctx.lr));
    last = ctx.r4.u32;
    svr2011::OnlineNoteHelpBar(ctx.r4.u32);
  }
  __imp__sub_82746358(ctx, base);
  // (the bar ends at its first empty slot: the ones after a removed one
  // move up)
  uint8_t* slots = base + out;
  uint32_t kept = 0;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint8_t* p = slots + slot * 4;
    const uint32_t icon = (uint32_t(p[0]) << 8) | p[1], label = (uint32_t(p[2]) << 8) | p[3];
    if ((icon == 0xFFFF && label == 0xFFFF) ||  // (an empty slot: room for the CPU label)
        (icon != 0xFFFF && svr2011::OnlineString(icon)) || (label != 0xFFFF && svr2011::OnlineString(label))) {
      continue;
    }
    if (kept != slot) std::memmove(slots + kept * 4, p, 4);
    ++kept;
  }
  for (uint32_t k = kept; k < 3; ++k) std::memset(slots + k * 4, 0xFF, 4);
  // The Player Match lobby's CPU slots (online_cpu.cpp): Y ADD CPU / REMOVE
  // CPU for the cursor's slot, and the host alone with CPUs is told why the
  // match doesn't start.
  const int y = cpu ? int((cpu - 1) % 3) : 0;
  const bool waiting = cpu && (cpu - 1) / 3 != 0;
  auto put = [&](uint32_t icon, uint32_t label) {
    for (uint32_t k = 0; k < kept; ++k)
      if (((uint32_t(slots[k * 4]) << 8) | slots[k * 4 + 1]) == icon && icon != 0xFFFF) {
        slots[k * 4 + 2] = uint8_t(label >> 8), slots[k * 4 + 3] = uint8_t(label);
        return;
      }
    if (kept < 3) {
      uint8_t* p = slots + kept * 4;
      p[0] = uint8_t(icon >> 8), p[1] = uint8_t(icon), p[2] = uint8_t(label >> 8), p[3] = uint8_t(label);
      ++kept;
    }
  };
  // (Y's glyph id: taken from the game's own Y slot - PLAYER INFO - when a bar has it)
  static uint32_t y_icon = svr2011::kYIconGuess;
  for (uint32_t k = 0; k < kept; ++k)
    if (((uint32_t(slots[k * 4 + 2]) << 8) | slots[k * 4 + 3]) == svr2011::kPlayerInfoId)
      y_icon = (uint32_t(slots[k * 4]) << 8) | slots[k * 4 + 1];
  if (y) put(y_icon, y == 1 ? svr2011::kAddCpuId : svr2011::kRemoveCpuId);
  if (waiting) put(0xFFFF, svr2011::kWaitingId);
  static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  if (log && cpu) {
    static std::string last;
    std::string now = fmt::format("id {} y {} waiting {}:", ctx.r4.u32 & 0xFFFF, y, waiting);
    for (uint32_t k = 0; k < 3; ++k)
      now += fmt::format(" {:02X}{:02X}/{:02X}{:02X}", slots[k * 4], slots[k * 4 + 1], slots[k * 4 + 2], slots[k * 4 + 3]);
    if (now != last) REXLOG_INFO("online cpu: bar {}", now);
    last = now;
  }
}

// The WWE SHOP: the PC has no marketplace and needs no Online Axxess (all
// content is licensed). sub_8247E330 makes a list item from a catalog entry
// (r6: the item; its type at +24 - 6 is Online Axxess, the only one): that
// one is left out. sub_82480068 shows "Unable to update the SHOP content
// list" once the marketplace offers came back empty ([+308] == 1, [+320]
// != 0): the shop lists the game's own catalog without it.
REX_EXTERN(__imp__sub_8247E330);
REX_HOOK_RAW(sub_8247E330) {
  const uint32_t item = ctx.r6.u32;
  __imp__sub_8247E330(ctx, base);
  if (ctx.r3.u32 && item) {
    const uint8_t* p = base + item + 24;
    const uint32_t type = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (type == 6) ctx.r3.u64 = 0;
  }
}

// Online Axxess (THQ's online pass): always owned - it's 2026. sub_828C19D8(id)
// is the game's "is this unlocked" (ids 0, 1 and 5 through the DLC manager:
// category 5 = catalog types 5 and 6); id 5 is Online Axxess, asked by every
// gate: entering ONLINE (without it, a "trial period" from Xbox LIVE storage
// that can't be had: "Information related to your trial period couldn't be
// retrieved"), the ONLINE menu's greyed items, the shop.
// XShowPlayerReviewUI(user, xuid) - the lobby's PLAYER REVIEW (sub_82904D50,
// the import's thunk): there is no Xbox review screen here. Its caller
// (sub_823746F0) on success waits for the system UI to close - a
// notification that never comes, so the game locked up; an error and the
// game goes on (no screen).
REX_HOOK_RAW(sub_82904D50) {
  (void)base;
  REXLOG_INFO("online: PLAYER REVIEW - no review screen on the PC");
  ctx.r3.u64 = 0x1F;  // ERROR_GEN_FAILURE
}

// Test aid (SVR2011_TEST_IMG_LOG): a superstar picture asked for
// (sub_8273A270(holder, desc): desc +0 the id, +4 the kind - SSFA/SSFB/SSFC -,
// +8 the attire).
REX_EXTERN(__imp__sub_8273A270);
REX_HOOK_RAW(sub_8273A270) {
  static const bool log = std::getenv("SVR2011_TEST_IMG_LOG") != nullptr;
  if (log && ctx.r4.u32) {
    auto rd = [base](uint32_t a) {
      const uint8_t* p = base + a;
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    REXLOG_INFO("online: superstar picture id {} kind {} attire {} (lr {:08X})", int32_t(rd(ctx.r4.u32)),
                rd(ctx.r4.u32 + 4), int32_t(rd(ctx.r4.u32 + 8)), uint32_t(ctx.lr));
  }
  __imp__sub_8273A270(ctx, base);
}

// Test aid (SVR2011_TEST_IMG_LOG): a picture holder told what to show
// (sub_8273BB60(holder, desc): desc +0 the id - a Created Superstar < 50 gets
// a CAW picture object, others the SSFC loader -, +4, +8, +12).
REX_EXTERN(__imp__sub_8273BB60);
REX_HOOK_RAW(sub_8273BB60) {
  if (uint32_t(ctx.lr) == 0x8273C080 && ctx.r3.u32) svr2011::OnlineCawPictureFailed(base, ctx.r3.u32);
  static const bool log = std::getenv("SVR2011_TEST_IMG_LOG") != nullptr;
  if (log && ctx.r4.u32) {
    auto rd = [base](uint32_t a) {
      const uint8_t* p = base + a;
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    REXLOG_INFO("online: picture holder {:08X} id {} {} {} {} (lr {:08X})", ctx.r3.u32, int32_t(rd(ctx.r4.u32)),
                int32_t(rd(ctx.r4.u32 + 4)), int32_t(rd(ctx.r4.u32 + 8)), int32_t(rd(ctx.r4.u32 + 12)),
                uint32_t(ctx.lr));
  }
  __imp__sub_8273BB60(ctx, base);
}

namespace svr2011 { void MotionFileRead(PPCContext& ctx, uint8_t* base); }  // (model_mem.cpp)

// Test aid (SVR2011_TEST_IMG_LOG): the pictures the menus load by name
// (sub_826A2FA8(image, "/DSC1/R123", ...), 16 characters at most).
REX_EXTERN(__imp__sub_826A2FA8);
REX_HOOK_RAW(sub_826A2FA8) {
  svr2011::MotionFileRead(ctx, base);  // (a match's motion file that doesn't fit its buffer: model_mem.cpp)
  static const bool log = std::getenv("SVR2011_TEST_IMG_LOG") != nullptr;
  if (log && ctx.r4.u32) {
    const char* p = reinterpret_cast<const char*>(base + ctx.r4.u32);
    REXLOG_INFO("online: picture {:08X} <- {} (lr {:08X})", ctx.r3.u32, std::string(p, strnlen(p, 32)), uint32_t(ctx.lr));
  }
  __imp__sub_826A2FA8(ctx, base);
}

// A lobby chat message that arrived (sub_828A7C18(chat, msg): msg +0 the
// sender, +8 the text, UTF-8; message kind 4096, sub_828A7E40): the line
// "name : text" is made in a 92-byte buffer (sub_828A7B38 adds it). The
// lobby's chat keyboard takes 21 characters (its field: 64 bytes, 21 a
// line, one line); a longer text - an old or changed game's - is cut to
// that, as this game would have typed it.
constexpr size_t kChatChars = 21;
REX_EXTERN(__imp__sub_828A7C18);
REX_HOOK_RAW(sub_828A7C18) {
  static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  // (test aid SVR2011_TEST_CHAT_LONG: every message arrives as 150 characters)
  static const bool test_long = std::getenv("SVR2011_TEST_CHAT_LONG") != nullptr;
  const uint32_t msg = ctx.r4.u32;
  // (a port game's message: its whole text comes over P2P - online_chat.h - and shows instead)
  if (msg) {
    PPCContext c = ctx;
    const uint8_t* m = base + msg;
    const uint8_t* g = base + 0x82EDE910;
    c.r3.u64 = uint32_t(g[0]) << 24 | uint32_t(g[1]) << 16 | uint32_t(g[2]) << 8 | g[3];
    c.r4.u64 = uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
    if (c.r3.u32) {
      sub_8288EC28(c, base);
      // (this player's own: the whole text it sent - the field held its end)
      if (c.r3.u64 == std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16)) {
        if (auto own = svr2011::OnlineChatTakeOwn()) {
          svr2011::OnlineChatShow(rex::cvar::Query<std::string>("online_name"), *own);
          return;
        }
      }
      if (svr2011::OnlineChatFromPort(c.r3.u64)) {
        if (log) REXLOG_INFO("online: chat message of {:016X} left out (its whole one comes)", c.r3.u64);
        return;
      }
    }
  }
  if (msg && test_long && svr2011::g_scratch) {
    const uint32_t g_scratch = svr2011::g_scratch;
    std::string t;
    while (t.size() < 150) t += "LONGMESSAGE-";
    t.resize(150);
    std::memcpy(base + g_scratch, t.c_str(), t.size() + 1);
    uint8_t* q = base + msg + 8;
    q[0] = uint8_t(g_scratch >> 24), q[1] = uint8_t(g_scratch >> 16), q[2] = uint8_t(g_scratch >> 8), q[3] = uint8_t(g_scratch);
  }
  if (msg) {
    const uint8_t* p = base + msg + 8;
    const uint32_t text = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    if (text) {
      char* s = reinterpret_cast<char*>(base + text);
      const size_t bytes = strnlen(s, 4096);
      size_t at = 0, chars = 0;  // (whole UTF-8 characters)
      while (at < bytes && chars < kChatChars) {
        const uint8_t c = uint8_t(s[at]);
        at += c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        ++chars;
      }
      if (at < bytes) {
        s[std::min(at, bytes)] = 0;
        REXLOG_INFO("online: a chat message of {} bytes cut to {} characters", bytes, kChatChars);
      } else if (log) {
        REXLOG_INFO("online: chat message in: {} bytes", bytes);
      }
    }
  }
  __imp__sub_828A7C18(ctx, base);
}

REX_EXTERN(__imp__sub_828C19D8);
// Online is off while the game files aren't clean (online_files.h: modified,
// or still being checked at start): id 5 then answers no - entering ONLINE
// shows the trial period message, whose text says why (OnlineString).
REX_HOOK_RAW(sub_828C19D8) {
  if (ctx.r3.u32 == 5) {
    // (online off in the settings: as before - the files aren't checked then)
    const std::string why =
        rex::cvar::Query<bool>("online_enabled") ? svr2011::GameFilesOnlineMessage() : std::string();
    ctx.r3.u64 = why.empty() ? 1 : 0;
    static std::string told;
    if (!why.empty() && why != told) REXLOG_WARN("online: off - {}", why);
    told = why;
    return;
  }
  __imp__sub_828C19D8(ctx, base);
}

// -- Community Creations limits lifted ----------------------------------------

// "You are unable to upload due to this created content containing
// downloadable content" (string 50999): the upload confirm step
// (sub_82511C18) tests a DLC flag per kind of content first - a Created
// Superstar's parts (sub_82579EB0: the part table's DLC byte), the +116 /
// +120 kinds (also sub_82579EB0), +128 (sub_8257AB78: bit 0x4) and stories
// (sub_82628FE0: the Superstars its scenes use). Each is asked only there:
// no DLC, always - DLC content uploads like any other.
REX_HOOK_RAW(sub_82579EB0) {
  (void)base;
  ctx.r3.u64 = 0;
}
REX_HOOK_RAW(sub_8257AB78) {
  (void)base;
  ctx.r3.u64 = 0;
}
REX_HOOK_RAW(sub_82628FE0) {
  (void)base;
  ctx.r3.u64 = 0;
}

// "To use a Created Superstar with Paint Tool Data in a match, you must first
// upload the data to the server ... Do you want to upload these?" (string
// 50940) on entering ONLINE: not asked - the answer is always YES. The online
// manager's check (sub_824F27E8) shows it through sub_824FC010(box, 50940)
// (its only use) and goes to state 1, whose handler (sub_824F18D0) waits for
// the box (sub_824FC1E8) and takes its answer (sub_824FC290; 1 = YES: the
// uploads of every attire's Paint Tool data, then "Upload complete").
namespace {
std::atomic<bool> g_paint_auto_yes{false};   // (the question was skipped)
thread_local bool g_paint_answering = false;  // (inside its state-1 handler)
}  // namespace

REX_EXTERN(__imp__sub_824FC010);
REX_HOOK_RAW(sub_824FC010) {
  if (ctx.r4.u32 == 50940) {
    g_paint_auto_yes = true;
    REXLOG_INFO("online: Paint Tool data to upload - uploading it (no question)");
    return;
  }
  __imp__sub_824FC010(ctx, base);
}

REX_EXTERN(__imp__sub_824F18D0);
REX_HOOK_RAW(sub_824F18D0) {
  if (!g_paint_auto_yes.exchange(false)) {
    __imp__sub_824F18D0(ctx, base);
    return;
  }
  g_paint_answering = true;
  __imp__sub_824F18D0(ctx, base);
  g_paint_answering = false;
}

REX_EXTERN(__imp__sub_824FC1E8);
REX_HOOK_RAW(sub_824FC1E8) {  // (the box has an answer)
  if (g_paint_answering) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_824FC1E8(ctx, base);
}

REX_EXTERN(__imp__sub_824FC290);
REX_HOOK_RAW(sub_824FC290) {  // (its answer: YES)
  if (g_paint_answering) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_824FC290(ctx, base);
}

REX_EXTERN(__imp__sub_82480068);
REX_HOOK_RAW(sub_82480068) {
  uint8_t* shop = base + ctx.r3.u32;
  auto load = [](const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  };
  if (load(shop + 308) == 1 && load(shop + 320) != 0) {
    shop[308] = shop[309] = shop[310] = shop[311] = 0;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82480068(ctx, base);
}

// -- Downloads (see "Community Creations downloads" above) -------------------

// State 46: the downloaded file, complete.
REX_EXTERN(__imp__sub_824D8F58);
REX_HOOK_RAW(sub_824D8F58) {
  using namespace svr2011;
  const uint32_t mgr = ctx.r3.u32;
  {
    // (while its message box is up the game just waits: so do we)
    const auto saved = ctx;
    ctx.r3.u64 = mgr + 756;
    sub_824FC1C0(ctx, base);
    const bool busy = ctx.r3.u32 != 0;
    ctx = saved;
    if (busy) {
      __imp__sub_824D8F58(ctx, base);
      return;
    }
  }
  const uint32_t type = Rd32(base, mgr + 1172), size = Rd32(base, mgr + 1424);
  const uint32_t holder = Rd32(base, kDownloadBuffer);
  const uint32_t buf = holder ? Rd32(base, holder) : 0;
  const std::string bad = BadDownload(ctx, base, type, buf, size);
  if (!bad.empty()) {
    REXLOG_WARN("online: download (type {}) refused: {} - nothing written", type, bad);
    Wr32(base, mgr + 1176, 49);  // "Download failed."
    return;
  }
  REXLOG_INFO("online: download (type {}, {} bytes) checked", type, size);
  if (type == 0) PrefetchLogos(base + buf);
  __imp__sub_824D8F58(ctx, base);
}

// The save job of a download: sub_824DCA98(mgr, 1) starts it (0: the slot
// list), sub_824DCCE8 ends it.
REX_EXTERN(__imp__sub_824DCA98);
REX_HOOK_RAW(sub_824DCA98) {
  using namespace svr2011;
  g_importing = ctx.r4.u32 == 1;
  if (g_importing && Rd32(base, ctx.r3.u32 + 1172) == 0) {  // a Superstar: its entrance's song and movie
    const uint32_t holder = Rd32(base, kDownloadBuffer);
    PrepareDownloadedEntrance(base, holder ? Rd32(base, holder) : 0);
  }
  __imp__sub_824DCA98(ctx, base);
}

REX_EXTERN(__imp__sub_824DCCE8);
REX_HOOK_RAW(sub_824DCCE8) {
  __imp__sub_824DCCE8(ctx, base);
  svr2011::g_importing = false;
}

// The writes of a download (the slot the player picked at job+33604): the
// file they replace is backed up first.
namespace {
void BackupSlotFile(uint8_t* base, uint32_t job, const char* format) {
  if (!svr2011::g_importing) return;
  const uint32_t slot = svr2011::Rd32(base, job + 33604);
  if (slot >= 100) return;
  char name[48];
  std::snprintf(name, sizeof(name), format, slot);
  svr2011::BackupSave(svr2011::g_saves / name);
}
}  // namespace

REX_EXTERN(__imp__sub_82513240);  // Created Superstar
REX_HOOK_RAW(sub_82513240) {
  BackupSlotFile(base, ctx.r3.u32, "%02uCreateSuperStar.cas");
  __imp__sub_82513240(ctx, base);
}

REX_EXTERN(__imp__sub_82518B78);  // highlight reel
REX_HOOK_RAW(sub_82518B78) {
  BackupSlotFile(base, ctx.r3.u32, "%02uSceneDat.scn");
  __imp__sub_82518B78(ctx, base);
}

REX_EXTERN(__imp__sub_82518BF0);
REX_HOOK_RAW(sub_82518BF0) {
  BackupSlotFile(base, ctx.r3.u32, "%02uSceneDat.scn");
  __imp__sub_82518BF0(ctx, base);
}

REX_EXTERN(__imp__sub_82517AA8);  // kept in SaveData.dat
REX_HOOK_RAW(sub_82517AA8) {
  if (svr2011::g_importing) svr2011::BackupSave(svr2011::g_saves / "SaveData.dat");
  __imp__sub_82517AA8(ctx, base);
}

REX_EXTERN(__imp__sub_82519C60);
REX_HOOK_RAW(sub_82519C60) {
  if (svr2011::g_importing) svr2011::BackupSave(svr2011::g_saves / "SaveData.dat");
  __imp__sub_82519C60(ctx, base);
}

namespace svr2011 {
uint32_t OnlineScratch() { return g_scratch; }
}  // namespace svr2011

// The game's DLC check on joining (sub_824CDE90: attribute 5 of the session's
// search entry must be 113, else "Unable to join. You must have the same
// downloadable content..." C67E). The entry's attribute block (+24: word 0,
// then attribute k at word k + 1) comes from the found session's properties:
// attribute 5 is property 0x1000000B, the host's session settings' constant
// 113 (sub_824F3E00 +24, copied by sub_8288F098) - not a list of packs. p2p.cpp
// makes every match session carry it (2.1.8 report 38981a: 0). Logged with
// the block when it isn't 113.
REX_EXTERN(__imp__sub_824FDFC8);
REX_HOOK_RAW(sub_824FDFC8) {
  const uint32_t entry = ctx.r3.u32;
  __imp__sub_824FDFC8(ctx, base);
  static uint32_t last = 0xFFFFFFFF;  // (0 is logged too: "no attribute")
  if (ctx.r3.u32 != last) {
    last = ctx.r3.u32;
    REXLOG_INFO("online: a session's DLC attribute is {} (the game wants 113{})", ctx.r3.u32,
                ctx.r3.u32 == 113 ? "" : " - it says 'a difference in downloadable content'");
    if (ctx.r3.u32 != 113 && entry) {  // (the entry's +8 and its attribute block +24: word 0, attributes 0-11)
      auto rd = [&](uint32_t a) {
        const uint8_t* p = base + a;
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      };
      std::string words;
      for (uint32_t i = 0; i < 13; ++i) words += fmt::format(" {}", int32_t(rd(entry + 24 + 4 * i)));
      REXLOG_INFO("online: that session's entry +8 {}, attributes{}", rd(entry + 8), words);
    }
  }
}

// -- Community Creations: no uploads that need mods ---------------------------
//
// A creation naming a mod's move or entrance works only for players with that mod - CC refuses
// it at the upload's confirm step (sub_82511C18, paint_pages.cpp's hook calls
// CcUploadModsRefused first) with the game's own message box (sub_82510668(screen, id, 1, 51), as
// its "contains downloadable content" 50999) and the reason: string kModsRefusedId, answered by
// OnlineString. The upload's kind is its manager's type (*0x82E3DD9C + 12: 1 Superstar, 2 Story,
// 3 Finisher, 4 Paint logo, 5 highlight reel, 6 Move-set; the data: sub_824C5768(manager, 1)).
//  - Superstar: its profile record (screen+36484 -> sub_827ACA10 -> sub_8257A450): the moveset's
//    224 u16 slots at +0x104 (only CREATE A MOVE-SET's move slots, cam_mod_moves.cpp) - a move a
//    pack gives that the stock game can't play (PackMove && !StockPlays); its entrance (284 bytes
//    at +0x2C4) naming a pack's CREATE AN ENTRANCE part (10000 + 1000 k + n, ModEntrances).
//  - Move-set: its file (the manager's buffer, *(manager+4) - sub_824C5768's +952 is its 0x1C-byte trailer)
//    starts with the same 224 move slots (980 bytes: moves, name at +0x1C0, ..., trailer at +0x3B8).
//  - Finisher (CREATE A FINISHER's own motion parts: packs add none), Story (the whole .sas) and reel:
//    not checked (the confirm step logs their trailer's first bytes).
// Every upload this build lets through carries the relay's X-Svr-ModFree header (online_net.cpp):
// the server refuses unmarked ones once the release with this check is out (its cc_unmarked setting).
namespace {


constexpr uint8_t kCamSlot[224] = {  // (cam_mod_moves.cpp kSlotCategory != 128: a CREATE A MOVE-SET slot)
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1,
    1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1,
    0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1,
    1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1,
    1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0};


// The pack a move comes from (its folder's name), "" if none.
std::string PackOfMove(uint16_t id) {
  for (const auto& [kind, folder] : svr2011::MovePacks())
    for (uint16_t m : svr2011::MovePackMoves(kind, folder))
      if (m == id) return folder;
  return "";
}

void AddUse(std::vector<std::string>& out, const std::string& what) {
  if (std::find(out.begin(), out.end(), what) == out.end()) out.push_back(what);
}

}  // namespace

namespace svr2011 {

// A moveset's 224 u16 slots at `moves` (CREATE A MOVE-SET's move slots only): a pack's move the stock game
// can't play.
void MovesetModUses(uint8_t* base, uint32_t moves, std::vector<std::string>& out) {
  for (uint32_t slot = 0; slot < 224; ++slot) {
    if (!kCamSlot[slot]) continue;
    const uint16_t id = Rd16(base, moves + 2 * slot);
    if (!id || id == 0xFFFF || !PackMove(id) || StockPlays(id)) continue;
    const std::string pack = PackOfMove(id);
    AddUse(out, "move " + std::to_string(id) + (pack.empty() ? "" : " (" + pack + ")"));
  }
}

std::vector<std::string> CawModUses(uint8_t* base, uint32_t profile) {
  std::vector<std::string> out;
  if (!profile) return out;
  MovesetModUses(base, profile + 0x104, out);
  // its entrance: a CREATE AN ENTRANCE part number from a pack (10000 + 1000 k + n)
  const auto mods = ModEntrances();
  for (uint32_t at = 0; at + 2 <= 284; at += 2) {
    const uint16_t v = Rd16(base, profile + 0x2C4 + at);
    if (v < 10000 || v >= 20000) continue;
    for (const auto& e : mods)
      if (int(v) % 1000 == e.number && (e.parts >> ((v - 10000) / 1000) & 1)) AddUse(out, "entrance " + e.label);
  }
  return out;
}

bool CcUploadModsRefused(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  const uint32_t manager = Rd32(base, 0x82E3DD9C);
  if (!manager || !screen) return false;
  const uint32_t kind = Rd32(base, manager + 12);
  const auto saved = ctx;
  std::vector<std::string> uses;
  if (kind == 1) {
    ctx.r3.u64 = Rd32(base, screen + 36484);
    sub_827ACA10(ctx, base);
    ctx.r3.u64 = ctx.r3.u32;
    sub_8257A450(ctx, base);
    uses = CawModUses(base, ctx.r3.u32);
  } else if (kind == 6) {
    // a Move-set's file (its buffer, *(manager+4): sub_824C5768's +952 is the file's 0x1C-byte trailer):
    // the 224 move slots first, as a profile's +0x104 (then its name at +0x1C0)
    if (const uint32_t file = Rd32(base, manager + 4)) {
      // (test aid SVR2011_TEST_CC_MODMOVE=ms:<move id>: that move into the file's first move slot)
      if (const char* v = std::getenv("SVR2011_TEST_CC_MODMOVE"); v && std::strncmp(v, "ms:", 3) == 0) {
        const int id = std::atoi(v + 3);
        base[file + 2 * 3] = uint8_t(id >> 8), base[file + 2 * 3 + 1] = uint8_t(id);
      }
      MovesetModUses(base, file, uses);
    }
  } else if (kind == 2 || kind == 3 || kind == 5) {
    // (Finisher: parts of CREATE A FINISHER's own motions - packs don't add any; Story: the whole .sas,
    // its superstars / arenas not decoded yet; reel: not decoded yet)
    ctx.r3.u64 = manager, ctx.r4.u64 = 1;
    sub_824C5768(ctx, base);
    const uint32_t data = ctx.r3.u32;
    std::string head;
    for (uint32_t k = 0; data && k < 64; ++k) head += fmt::format("{:02X}", base[data + k]);
    REXLOG_INFO("online: CC upload kind {} - its data at {:08X}: {} (not checked for mods yet)", kind, data, head);
    // (test aid SVR2011_TEST_CC_DUMP=<folder>: 16 KB of it to <folder>/cc_kind<kind>.bin - its layout)
    if (const char* dir = std::getenv("SVR2011_TEST_CC_DUMP"); dir && *dir && data) {
      std::ofstream out(std::filesystem::path(dir) / fmt::format("cc_kind{}.bin", kind), std::ios::binary);
      out.write(reinterpret_cast<const char*>(base + data), 16384);
    }
  }
  ctx = saved;
  if (uses.empty()) return false;
  std::string list;
  for (size_t i = 0; i < uses.size() && i < 3; ++i) list += (i ? ", " : "") + uses[i];
  if (uses.size() > 3) list += fmt::format(" and {} more", uses.size() - 3);
  g_mods_reason = "This uses mods: " + list + ".\nPlayers without them couldn't use it, so it can't be uploaded.";
  REXLOG_INFO("online: CC upload refused (kind {}): uses mods - {}", kind, list);
  ctx.r3.u64 = screen, ctx.r4.u64 = kModsRefusedId, ctx.r5.u64 = 1, ctx.r6.u64 = 51;
  sub_82510668(ctx, base);
  ctx = saved;
  return true;
}

}  // namespace svr2011
