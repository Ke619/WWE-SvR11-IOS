// WWE SmackDown vs. Raw 2011 - MY WWE -> OPTIONS -> BACKGROUNDS (see
// backgrounds_page.h; research: docs/MENU_BACKGROUNDS.md).
//
// The picture number lives in the game's data struct (*0x82E3DDFC + 8680),
// 0 at every start. The main menu's background object (sub_82458F58, its
// constructor) reads it into +68 and loads "/MENU/EB%02d"; the main menu's
// destructor moves it on with sub_8258A990 (+1, back to 0 at 12). Here that
// step skips the pictures that are off, and a new background object whose
// number is off (the first one after start, or after the page changed the
// set) takes the next one that is on.
//
// Custom pictures (the launcher's Backgrounds list): <game>/Mods/Backgrounds/
// <id>/picture.dds (1024 x 512 DXT5, no mips: EXHBG2's size and format) with
// manifest.txt (name=..., logo=0|1); a "disabled" file turns one off. A pack
// (the Mod Maker's Backgrounds page) is one folder with several pictures
// (NN_<name>.dds; manifest title.<file>= / logo.<file>= for each, a
// "<file>.off" file turns one off; "disabled" turns the whole pack off). They
// join the cycle after the 12 (numbers 12, 13, ... in the game's data: both
// 12-limits are the hooks below). A custom picture's background object loads
// EB00 (+68 = 0), and as the game unpacks that set (the BPE decoder,
// arena_mods.cpp's hook -> BackgroundsDecoded) its EXHBG2 gets the custom
// picture and, with logo=0, the "SmackDown vs RAW 2011" logo (EXHBG, 512 x
// 256 A8R8G8B8) turns transparent.

#include "backgrounds_page.h"
#include "no_mods.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "graphics_page.h"
#include "util.h"

REXCVAR_DEFINE_STRING(menu_backgrounds_off, "", "UI",
                      "MY WWE -> OPTIONS -> BACKGROUNDS: the main menu pictures that are off (their numbers 1-12, "
                      "comma-separated; all off = all on)");

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kCount = 12;  // menuHD.pac MENU/EB00-EB11 (the game's own limit, sub_8258A990)
constexpr size_t kPictureBytes = 1024 * 512;  // EXHBG2: 1024 x 512 DXT5, one level
constexpr size_t kLogoBytes = 512 * 256 * 4;  // EXHBG: 512 x 256 A8R8G8B8
// From the pictures (no names in the game's data); the two that can't be
// told for sure are numbered.
const char* const kName[kCount] = {"THE MIZ",        "BACKGROUND 2",  "JOHN CENA",     "CM PUNK",
                                   "EDGE",           "TRIPLE H",      "SHAWN MICHAELS", "REY MYSTERIO",
                                   "RANDY ORTON",    "BACKGROUND 10", "JACK SWAGGER",  "THE UNDERTAKER"};
const char* const kWhat[kCount] = {"With the microphone",       "Flying",
                                   "Close-up",                  "With the Money in the Bank briefcase",
                                   "With the WWE Championship", "Entrance",
                                   "Ladder dive",               "In the ring",
                                   "From behind",               "Shouting",
                                   "Arms up",                   "Entrance"};

constexpr uint32_t kGameData = 0x82E3DDFC;  // -> +8680: the picture number
constexpr uint32_t kPictureAt = 8680;
constexpr uint32_t kObjectPicture = 68;  // the background object's number

// A custom picture (Mods/Backgrounds/<id>/<file>).
struct Custom {
  std::string id, file, name, pack;  // pack: the pack's name (empty: a single picture)
  bool on = true, logo = false;
};

std::mutex g_mutex;
std::array<bool, kCount> g_on;  // under g_mutex
std::vector<Custom> g_customs;  // under g_mutex (by id)
uint8_t* g_base = nullptr;

// The custom picture of the background being made (the unpacked set gets it).
struct Active {
  std::vector<uint8_t> picture;  // the top level, as in a PC DDS (the unpacked set's order)
  bool logo = false;
  std::string name;
};
std::mutex g_active_mutex;
std::shared_ptr<const Active> g_active;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

fs::path Folder() { return rex::filesystem::GetExecutableFolder() / "Mods" / "Backgrounds"; }

using svr2011::Upper;

std::vector<std::string> Lines(const fs::path& file) {
  std::vector<std::string> out;
  std::ifstream m(file);
  for (std::string line; std::getline(m, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

// A folder's pictures: picture.dds (a single one) or a pack's *.dds, by name.
std::vector<std::string> PicturesIn(const fs::path& dir) {
  std::error_code ec;
  if (fs::is_regular_file(dir / "picture.dds", ec)) return {"picture.dds"};
  std::vector<std::string> out;
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    std::string n = f.path().filename().string(), low = n;
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (f.is_regular_file(ec) && low.size() > 4 && low.compare(low.size() - 4, 4, ".dds") == 0) out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// A pack picture's name from its file: "03_john_cena.dds" -> "JOHN CENA".
std::string NameFromFile(std::string f) {
  if (f.size() > 4) f.resize(f.size() - 4);
  if (f.size() > 3 && std::isdigit(static_cast<unsigned char>(f[0])) && std::isdigit(static_cast<unsigned char>(f[1])) &&
      f[2] == '_')
    f = f.substr(3);
  for (char& c : f)
    if (c == '_') c = ' ';
  return f;
}

// The custom pictures on disk, by folder name and file.
void ScanCustoms() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::vector<Custom> out;
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(Folder(), ec))
    if (e.is_directory(ec)) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& dir : dirs) {
    const auto files = PicturesIn(dir);
    if (files.empty()) continue;
    std::string name = dir.filename().string();
    bool logo = false;
    std::vector<std::string> man = Lines(dir / "manifest.txt");
    for (const auto& line : man) {
      if (line.rfind("name=", 0) == 0 && line.size() > 5) name = line.substr(5);
      if (line.rfind("logo=", 0) == 0) logo = line.substr(5) == "1";
    }
    const bool all_off = fs::exists(dir / "disabled", ec);
    for (const auto& f : files) {
      Custom c;
      c.id = dir.filename().string();
      c.file = f;
      c.logo = logo;
      if (files.size() == 1 && f == "picture.dds") {
        c.name = name;
      } else {
        c.pack = name;
        c.name = NameFromFile(f);
        for (const auto& line : man) {
          if (line.rfind("title." + f + "=", 0) == 0) c.name = line.substr(7 + f.size());
          if (line.rfind("logo." + f + "=", 0) == 0) c.logo = line.substr(6 + f.size()) == "1";
        }
      }
      c.name = Upper(c.name);
      c.on = !all_off && !fs::exists(dir / (f + ".off"), ec);
      out.push_back(std::move(c));
    }
  }
  std::lock_guard lock(g_mutex);
  g_customs = std::move(out);
}

// A custom picture's on / off and logo to disk: a single picture's
// "disabled" file and manifest logo=; a pack picture's "<file>.off" and
// logo.<file>= (turning one on in a pack that is off leaves the pack's
// others off).
void SaveCustom(const Custom& c) {
  const fs::path dir = Folder() / c.id;
  std::error_code ec;
  const bool pack = !c.pack.empty();
  if (!pack) {
    if (c.on)
      fs::remove(dir / "disabled", ec);
    else
      std::ofstream(dir / "disabled") << "off\n";
  } else if (c.on) {
    if (fs::exists(dir / "disabled", ec)) {
      for (const auto& f : PicturesIn(dir))
        if (f != c.file) std::ofstream(dir / (f + ".off")) << "off\n";
      fs::remove(dir / "disabled", ec);
    }
    fs::remove(dir / (c.file + ".off"), ec);
  } else {
    std::ofstream(dir / (c.file + ".off")) << "off\n";
  }
  const std::string key = pack ? "logo." + c.file + "=" : "logo=";
  std::vector<std::string> lines;
  for (const auto& line : Lines(dir / "manifest.txt"))
    if (line.rfind(key, 0) != 0) lines.push_back(line);
  lines.push_back(key + (c.logo ? "1" : "0"));
  std::ofstream m(dir / "manifest.txt", std::ios::trunc);
  for (const auto& l : lines) m << l << "\n";
}

void LoadSettings() {
  ScanCustoms();
  std::array<bool, kCount> on;
  on.fill(true);
  std::stringstream ss(rex::cvar::Query<std::string>("menu_backgrounds_off"));
  for (std::string n; std::getline(ss, n, ',');) {
    const int i = std::atoi(n.c_str()) - 1;
    if (i >= 0 && i < kCount) on[i] = false;
  }
  std::lock_guard lock(g_mutex);
  g_on = on;
}

void SaveSettings() {
  std::string off;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kCount; ++i)
      if (!g_on[i]) off += (off.empty() ? "" : ",") + std::to_string(i + 1);
  }
  rex::cvar::SetFlagByName("menu_backgrounds_off", off);
  svr2011::SaveConfigSetting("menu_backgrounds_off", "\"" + off + "\"");
}

int Total() {
  std::lock_guard lock(g_mutex);
  return kCount + int(g_customs.size());
}

// Picture i (0-11 the game's, 12+ the custom ones) is on (under g_mutex).
bool IsOn(int i) {
  if (i < 0) return false;
  if (i < kCount) return g_on[i];
  return i - kCount < int(g_customs.size()) && g_customs[i - kCount].on;
}

// Picture i plays (every one when all are off).
bool Allowed(int i) {
  std::lock_guard lock(g_mutex);
  const int total = kCount + int(g_customs.size());
  if (i < 0 || i >= total) return false;
  for (int k = 0; k < total; ++k)
    if (IsOn(k)) return IsOn(i);
  return true;
}

// The first picture from `from` on (wrapping) that plays.
int FirstAllowed(int from) {
  const int total = Total();
  for (int k = 0; k < total; ++k) {
    const int i = ((from % total) + total + k) % total;
    if (Allowed(i)) return i;
  }
  return 0;
}

// The picture on screen now (-1: unknown).
int Current() {
  if (!g_base) return -1;
  const uint32_t data = Rd32(g_base + kGameData);
  if (!data) return -1;
  const int i = int(Rd32(g_base + data + kPictureAt));
  return i >= 0 && i < Total() ? i : -1;
}

std::string NameOf(int i) {
  if (i >= 0 && i < kCount) return kName[i];
  std::lock_guard lock(g_mutex);
  return i >= kCount && i - kCount < int(g_customs.size()) ? g_customs[i - kCount].name : std::string("-");
}

// The background being made shows picture i: a custom one's picture is read
// for the set about to be unpacked (none for the game's own).
void Activate(int i) {
  std::shared_ptr<Active> a;
  if (i >= kCount) {
    Custom c;
    {
      std::lock_guard lock(g_mutex);
      if (i - kCount < int(g_customs.size())) c = g_customs[i - kCount];
    }
    std::ifstream f(Folder() / c.id / c.file, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() >= 128 + kPictureBytes && !std::memcmp(d.data(), "DDS ", 4) && Le32(&d[12]) == 512 &&
        Le32(&d[16]) == 1024 && !std::memcmp(&d[84], "DXT5", 4)) {
      a = std::make_shared<Active>();
      a->picture.assign(d.begin() + 128, d.begin() + 128 + kPictureBytes);
      a->logo = c.logo;
      a->name = c.name;
    } else {
      REXLOG_WARN("[svr2011] backgrounds: {}/{} is not a 1024 x 512 DXT5 DDS", c.id, c.file);
    }
  }
  std::lock_guard lock(g_active_mutex);
  g_active = std::move(a);
}

}  // namespace

// The main menu moves on to the next picture (sub_8258A990(&number): +1, 0
// at 12): the next one that is on, the custom ones after the 12.
REX_EXTERN(__imp__sub_8258A990);
REX_HOOK_RAW(sub_8258A990) {
  g_base = base;
  const uint32_t at = ctx.r3.u32;
  if (!at) return;
  const int now = int(Rd32(base + at));
  const int next = FirstAllowed(now + 1);
  Wr32(base + at, uint32_t(next));
}

// A main-menu background is made (sub_82458F58(object, ...)): its picture
// number (the game's data; the constructor's copy at +68 is clamped to
// 0-11) is one that is on; a custom one loads EB00 and gets its picture as
// that set is unpacked (BackgroundsDecoded).
REX_EXTERN(__imp__sub_82458F58);
REX_HOOK_RAW(sub_82458F58) {
  g_base = base;
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82458F58(ctx, base);
  if (!object) return;
  const uint32_t data = Rd32(base + kGameData);
  int i = int(data ? Rd32(base + data + kPictureAt) : Rd32(base + object + kObjectPicture));
  if (!Allowed(i)) {
    const int next = FirstAllowed(i + 1);
    REXLOG_INFO("[svr2011] backgrounds: picture {} is off - {} ({}) instead", i + 1, next + 1, NameOf(next));
    i = next;
    if (data) Wr32(base + data + kPictureAt, uint32_t(i));
  }
  Wr32(base + object + kObjectPicture, uint32_t(i < kCount ? i : 0));
  Activate(i);
  if (i >= kCount) REXLOG_INFO("[svr2011] backgrounds: custom picture {} ({})", i + 1, NameOf(i));
}

// ---------------------------------------------------------------------------
// The page (the JUKEBOX page's look).

namespace {

rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open{false};
std::atomic<bool> g_wait_release{false};

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// A controller button glyph (a coloured disc with its letter) and a label.
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

class BackgroundsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit BackgroundsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t PadButtons();
  void Set(int i, bool on);
  void SetLogo(int i);

  int sel_ = 0, top_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{};
};

uint16_t BackgroundsPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  constexpr int kDead = 16000;
  if (s.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  return b;
}

// i < 0: every picture.
void BackgroundsPage::Set(int i, bool on) {
  std::vector<Custom> changed;
  {
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < kCount; ++k)
      if (i < 0 || k == i) g_on[k] = on;
    for (int k = 0; k < int(g_customs.size()); ++k)
      if ((i < 0 || kCount + k == i) && g_customs[k].on != on) {
        g_customs[k].on = on;
        changed.push_back(g_customs[k]);
      }
  }
  SaveSettings();
  for (const auto& c : changed) SaveCustom(c);
}

// A custom picture's "SmackDown vs RAW 2011" logo shown / hidden (from its
// next showing).
void BackgroundsPage::SetLogo(int i) {
  Custom c;
  {
    std::lock_guard lock(g_mutex);
    if (i < kCount || i - kCount >= int(g_customs.size())) return;
    g_customs[i - kCount].logo = !g_customs[i - kCount].logo;
    c = g_customs[i - kCount];
  }
  SaveCustom(c);
}

void BackgroundsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  constexpr int kVisible = 7;
  if (g_open_requested.exchange(false)) {
    ScanCustoms();  // (the launcher may have added some)
    sel_ = top_ = 0;
    wait_release_ = true;  // (the A that opened the page is still down)
    opened_at_ = Clock::now();
    g_open = true;
    REXLOG_INFO("[svr2011] backgrounds: page open ({} custom pictures)", Total() - kCount);
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }

  // Input: controller (edges, auto-repeat for held directions), keys, touch.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;
  uint16_t act = pressed;
  const auto now = Clock::now();
  if (pressed & kRepeat) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kRepeat) && now >= repeat_at_) {
    act |= buttons & kRepeat;
    repeat_at_ = now + std::chrono::milliseconds(110);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  const int total = Total();
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) sel_ = (sel_ + total - 1) % total;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % total;
  sel_ = std::clamp(sel_, 0, total - 1);
  // The rows as they are now (names, on / off, the custom ones' logo).
  std::vector<bool> on(total), logo(total);
  std::vector<std::string> names(total), whats(total);
  auto snapshot = [&] {
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < total; ++k) {
      on[k] = IsOn(k);
      if (k < kCount) {
        names[k] = kName[k], whats[k] = kWhat[k];
      } else if (k - kCount < int(g_customs.size())) {
        const Custom& c = g_customs[k - kCount];
        names[k] = c.name, logo[k] = c.logo;
        whats[k] = (c.pack.empty() ? std::string("Your picture") : "From " + c.pack) +
                   (c.logo ? " - with the game's logo" : "");
      }
    }
  };
  snapshot();
  bool toggle = (pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
  bool all = (pressed & X_INPUT_GAMEPAD_Y) || ImGui::IsKeyPressed(ImGuiKey_Y, false);
  const bool logo_key = sel_ >= kCount && ((pressed & X_INPUT_GAMEPAD_X) || ImGui::IsKeyPressed(ImGuiKey_X, false));
  bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
              ImGui::IsKeyPressed(ImGuiKey_Escape, false);

  // Layout in the game's 1280 x 720 frame, letterboxed into the window.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  auto inside = [&](ImVec2 a, ImVec2 b) {
    const ImVec2 m = io.MousePos;
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  };
  const bool tap = !wait_release_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(90, 200, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255);
  const float row_h = 62, gap = 6, list_top = 134;
  // (touch: a row chooses and turns on / off; the footer's buttons work too)
  if (tap) {
    for (int r = 0; r < kVisible && top_ + r < total; ++r) {
      const float y = list_top + r * (row_h + gap);
      if (inside(P(80, y), P(744, y + row_h))) {
        sel_ = top_ + r;
        toggle = true;
      }
    }
  }
  if (toggle) Set(sel_, !on[sel_]);
  const bool some_off = std::find(on.begin(), on.end(), false) != on.end();
  if (all) Set(-1, some_off);
  if (logo_key) SetLogo(sel_);
  snapshot();
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
  top_ = std::clamp(top_, 0, std::max(0, total - kVisible));
  const int showing = Current();
  const int n_on = int(std::count(on.begin(), on.end(), true));

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 235), 10 * s);
  dl->AddRectFilledMultiColor(P(62, 42), P(1218, 260), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white-rimmed tab with the title, the page name.
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7)
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const float ps = 22 * s;
    const ImVec2 psz = TextSize(g_menu_font, ps, "BACKGROUNDS");
    Text(dl, g_menu_font, ps, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "BACKGROUNDS");
  }
  // Summary: pictures on, and a bar.
  {
    const float ss = 21 * s, cy = P(0, 108).y;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d OF %d PICTURES ON", n_on, total);
    Text(dl, g_menu_font, ss, ImVec2(P(84, 0).x, cy - TextSize(g_menu_font, ss, buf).y * 0.5f), kWhite, buf);
    const char* note = n_on ? "THE MAIN MENU SHOWS THE PICTURES THAT ARE ON" : "ALL OFF: THE MAIN MENU SHOWS THEM ALL";
    const ImVec2 nz = TextSize(g_menu_font, 17 * s, note);
    Text(dl, g_menu_font, 17 * s, ImVec2(P(1196, 0).x - nz.x, cy - nz.y * 0.5f), n_on ? kGrey : kGold, note);
    const ImVec2 a = P(360, 102), b(P(1196, 0).x - nz.x - 24 * s, P(0, 114).y);
    if (b.x > a.x) {
      dl->AddRectFilled(a, b, IM_COL32(40, 40, 48, 255), 4 * s);
      if (n_on)
        dl->AddRectFilledMultiColor(a, ImVec2(a.x + (b.x - a.x) * n_on / total, b.y), IM_COL32(200, 30, 32, 255),
                                    IM_COL32(240, 70, 60, 255), IM_COL32(240, 70, 60, 255), IM_COL32(200, 30, 32, 255));
    }
  }
  // The list.
  for (int r = 0; r < kVisible && top_ + r < total; ++r) {
    const int i = top_ + r;
    const float y = list_top + r * (row_h + gap);
    const bool sel = i == sel_;
    const ImVec2 a = P(80, y), b = P(744, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    const ImVec2 sa = P(94, y + 18), sb = P(150, y + 44);
    dl->AddRectFilled(sa, sb, on[i] ? kGreen : IM_COL32(70, 70, 78, 255), 13 * s);
    const float kx = on[i] ? sb.x - 13 * s : sa.x + 13 * s;
    dl->AddCircleFilled(ImVec2(kx, (sa.y + sb.y) * 0.5f), 10 * s, kWhite, 20);
    const float ns = 21 * s, ds = 16 * s;
    char num[8];
    std::snprintf(num, sizeof num, "%02d", i + 1);
    Text(dl, g_menu_font, ns, P(166, y + 7), on[i] ? kGrey : kDim, num);
    Text(dl, g_menu_font, ns, P(206, y + 7), on[i] || sel ? kWhite : kGrey, names[i].c_str());
    Text(dl, g_menu_font, ds, P(206, y + 35), sel ? IM_COL32(235, 205, 205, 255) : kDim, whats[i].c_str());
    if (i == showing) {
      const ImVec2 tz = TextSize(g_menu_font, ds, "ON SCREEN");
      Text(dl, g_menu_font, ds, ImVec2(P(726, 0).x - tz.x, P(0, y + 22).y), kGold, "ON SCREEN");
    } else if (!on[i]) {
      const ImVec2 oz = TextSize(g_menu_font, ds, "OFF");
      Text(dl, g_menu_font, ds, ImVec2(P(726, 0).x - oz.x, P(0, y + 22).y), kDim, "OFF");
    }
  }
  {  // scroll bar
    const float tp = list_top, h = kVisible * (row_h + gap) - gap;
    dl->AddRectFilled(P(750, tp), P(754, tp + h), IM_COL32(40, 40, 48, 255), 2 * s);
    const float bh = h * std::min(1.0f, float(kVisible) / float(total));
    const float by = total > kVisible ? tp + (h - bh) * top_ / float(total - kVisible) : tp;
    dl->AddRectFilled(P(750, by), P(754, by + bh), IM_COL32(200, 200, 208, 255), 2 * s);
  }
  // The side panel: how it works (the picture itself is behind the page).
  const ImVec2 da = P(766, list_top), db = P(1200, list_top + kVisible * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  {
    Text(dl, g_menu_font, 16 * s, P(790, list_top + 14), kDim, "MAIN MENU");
    auto line = [&](float y, float size, const char* text, ImU32 colour) {
      Text(dl, g_menu_font, size, P(790, y), colour, text);
    };
    line(list_top + 52, 24 * s, names[sel_].c_str(), kWhite);
    line(list_top + 84, 17 * s, on[sel_] ? "ON - IN THE CYCLE" : "OFF - SKIPPED", on[sel_] ? kGreen : kGold);
    line(list_top + 140, 16 * s, "The main menu shows one picture and", kGrey);
    line(list_top + 164, 16 * s, "moves on to the next every time you", kGrey);
    line(list_top + 188, 16 * s, "leave it. Pictures that are off are", kGrey);
    line(list_top + 212, 16 * s, "skipped; with all off, all play.", kGrey);
    char buf[80];
    std::snprintf(buf, sizeof buf, "ON SCREEN NOW: %.40s", showing >= 0 ? names[showing].c_str() : "-");
    line(list_top + 270, 16 * s, buf, kGold);
    if (sel_ >= kCount) {
      line(list_top + 310, 16 * s, logo[sel_] ? "GAME LOGO: SHOWN" : "GAME LOGO: HIDDEN", kGrey);
      line(list_top + 334, 16 * s, "(X changes it - from its next showing)", kDim);
    } else if (total == kCount) {
      line(list_top + 310, 16 * s, "Add your own pictures with the", kDim);
      line(list_top + 334, 16 * s, "launcher: Mods -> Backgrounds.", kDim);
    }
  }
  // Footer: the buttons (tappable).
  {
    const float cy = P(0, 660).y;
    float x = P(84, 0).x;
    const float x_b = x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    const float x_a = x;
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255), on[sel_] ? "TURN OFF" : "TURN ON");
    const float x_y = x;
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255), some_off ? "ALL ON" : "ALL OFF");
    const float x_x = x;
    if (sel_ >= kCount) x = Hint(dl, x, cy, s, "X", IM_COL32(40, 110, 210, 255), logo[sel_] ? "HIDE LOGO" : "SHOW LOGO");
    if (tap && io.MousePos.y > cy - 16 * s && io.MousePos.y < cy + 16 * s) {
      if (io.MousePos.x >= x_b && io.MousePos.x < x_a) back = true;
      else if (io.MousePos.x >= x_a && io.MousePos.x < x_y) Set(sel_, !on[sel_]);
      else if (io.MousePos.x >= x_y && io.MousePos.x < x_x) Set(-1, some_off);
      else if (io.MousePos.x >= x_x && io.MousePos.x < x) SetLogo(sel_);
    }
  }

  if (back) {
    REXLOG_INFO("[svr2011] backgrounds: page closed");
    g_wait_release = true;
    g_open = false;
  }
}

}  // namespace

namespace svr2011 {

void InstallBackgroundsPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input) {
  g_input = input;
  LoadSettings();
  new BackgroundsPage(drawer);  // lives for the whole run
}

void SetBackgroundsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenBackgroundsPage() { g_open_requested = true; }

bool BackgroundsPageHoldsInput() { return g_open.load() || g_wait_release.load(); }

void BackgroundsDecoded(uint8_t* d, uint32_t size) {
  std::shared_ptr<const Active> a;
  {
    std::lock_guard lock(g_active_mutex);
    a = g_active;
  }
  if (!a || size < 48) return;
  // A texture set (u32 count, 16-byte header, 32-byte records: name[16],
  // "dds", size, offset) with EXHBG2 in it: the main menu's picture.
  const uint32_t n = Le32(d);
  if (!n || n > 16 || 16 + 32ull * n > size) return;
  bool picture = false, logo = false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = d + 16 + 32 * i;
    if (std::memcmp(r + 16, "dds", 3)) return;  // not a texture set
    const uint32_t sz = Le32(r + 20), off = Le32(r + 24);
    if (uint64_t(off) + sz > size || sz < 128 || std::memcmp(d + off, "DDS ", 4)) continue;
    const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
    if (name == "EXHBG2" && sz >= 128 + kPictureBytes && !std::memcmp(d + off + 84, "DXT5", 4)) {
      std::memcpy(d + off + 128, a->picture.data(), kPictureBytes);
      picture = true;
    } else if (name == "EXHBG" && !a->logo && sz >= 128 + kLogoBytes) {
      std::memset(d + off + 128, 0, kLogoBytes);  // (transparent)
      logo = true;
    }
  }
  if (picture)
    REXLOG_INFO("[svr2011] backgrounds: {} in place of EB00's picture{}", a->name, logo ? " (game logo hidden)" : "");
}

}  // namespace svr2011
