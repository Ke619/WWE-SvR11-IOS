// WWE SmackDown vs. Raw 2011 - PlayStation and keyboard button pictures (see
// pad_icons.h).
//
// The game's fonts:
//   font +0 page count, +4 pages (24 bytes each: +0 texture width, +4 height,
//   +8 glyph count, +12 kerning count, +16 glyphs, +20 kerning);
//   glyph (24 bytes): +0 u0, +4 v0, +8 u1, +12 v1 (floats, of the page's
//   texture), +16 width, +18 height (pixels, u16), +20 code - sorted by code.
// Glyphs are found by sub_826AD450(font, code) -> the page, then a search of
// its glyphs (in sub_826AD608, the lookup, and inline in the text measuring
// and layout code). So the icon page's glyphs are rewritten where they are:
// each font keeps its versions (Xbox, PlayStation, pairs, keyboard; every v
// moved to its section of the taller picture) and the page holds the one the
// text being measured or drawn needs - set at each page search for an icon.
//
// Other pictures (the match HUD's prompts, character select's controllers):
// textures of their own, bound by SetTexture (sub_82917EC8: device, sampler,
// texture object). The native renderer tells which guest texture holds which
// picture (PadIconsTextureUploaded: by its data); for a PlayStation or
// keyboard player SetTexture gets a copy of the texture object pointing at
// that version's data instead.

#include "pad_icons.h"
#include "mystery_opponent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#include "generated/default/svr2011_init.h"
#include "keyboard_typing.h"
#include "pad_types.h"
#include "../modmaker/svrfmt/bytes.h"
#include "pad_map.h"

namespace svr2011 {
namespace {

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::RdF, svrfmt::Wr32, svrfmt::Wr16, svrfmt::WrF;

constexpr uint32_t kGlyph = 24;
constexpr uint32_t kFirstCode = 0xE000;

enum Kind : uint8_t { kXbox, kPlayStation, kPair, kKeyboard, kKinds };

rex::memory::Memory* g_memory = nullptr;
uint8_t* g_base = nullptr;
bool g_enabled = false;
thread_local int t_owner = -1;
// (character select's bottom bar being drawn: each controller picture's draws so far - record, count)
thread_local bool t_in_bar = false;
thread_local std::vector<std::pair<uint32_t, int>> t_bar_draws;

// The kind of pictures for the text / picture being drawn: its player's
// controller (the match HUD, the character select panels), or - shared by
// everyone: menus, their bars, the pause menu, tips - the controller of
// whoever pressed something last (a key, a pad button, a trigger or stick
// pushed: PadIconsInput), as most PC games do.
std::atomic<int> g_last_input{0};
Kind CurrentKind(bool) {
  auto of = [](PadType t) {
    return t == PadType::kPlayStation ? kPlayStation : t == PadType::kKeyboard ? kKeyboard : kXbox;
  };
  if (t_owner >= 0) return of(PlayerPadType(uint32_t(t_owner)));
  return of(PlayerPadType(uint32_t(g_last_input.load())));
}
// The controller layout (pad_map.h) the prompt being drawn shows: its player's or, shared, the last to press
// something's. Only in play: the menus get the real buttons, so they show them.
uint64_t CurrentLayout() {
  if (!PadMapActive()) return kPadMapStandard;
  return PadMapLayout(uint32_t(t_owner >= 0 ? t_owner : g_last_input.load()));
}

// ---------------------------------------------------------------------------
// Keyboard: the key each button is bound to (the SDK's keybind_* settings),
// shown as its keycap. A button bound to more than one key ("E,O") shows the
// one the player pressed last for it, the first until then (report 73e6ba:
// the right hand's O reverses, the prompt said E).

struct ButtonBind {
  uint32_t code;
  const char* setting;  // (or a special cap: WASD / ARROWS when bound so)
};
const ButtonBind kBinds[] = {
    {0xE000, "keybind_start"},          {0xE001, "keybind_back"},           {0xE002, "keybind_dpad_up"},
    {0xE003, "keybind_dpad_up"},        {0xE004, "keybind_dpad_down"},      {0xE005, "keybind_dpad_right"},
    {0xE006, "keybind_dpad_left"},      {0xE007, "keybind_y"},              {0xE008, "keybind_b"},
    {0xE009, "keybind_a"},              {0xE00A, "keybind_x"},              {0xE00B, "keybind_left_shoulder"},
    {0xE00C, "keybind_left_trigger"},   {0xE00D, "keybind_lstick_press"},   {0xE00E, "keybind_right_shoulder"},
    {0xE00F, "keybind_right_trigger"},  {0xE010, "keybind_rstick_press"},   {0xE011, "rstick"},
    {0xE012, "lstick"},                 {0xE013, "keybind_a"},              {0xE014, "keybind_b"},
    {0xE015, "keybind_y"},              {0xE016, "keybind_x"},              {0xE01B, "keybind_lstick_up"},
    {0xE01C, "keybind_lstick_down"},    {0xE01D, "keybind_lstick_right"},   {0xE01E, "keybind_lstick_left"},
    {0xE01F, "keybind_lstick_left"},    {0xE020, "keybind_lstick_up"},      {0xE021, "lstick"},
    {0xE022, "lstick"},                 {0xE023, "lstick"},                 {0xE024, "keybind_rstick_up"},
    {0xE025, "keybind_rstick_down"},    {0xE026, "keybind_rstick_right"},   {0xE027, "keybind_rstick_left"},
    {0xE028, "keybind_rstick_left"},    {0xE029, "keybind_rstick_up"},      {0xE02A, "rstick"},
    {0xE02B, "rstick"},                 {0xE02C, "rstick"},                 {0xE02D, "keybind_dpad_up"},
    {0xE02E, "keybind_dpad_down"},      {0xE02F, "keybind_dpad_right"},     {0xE030, "keybind_dpad_left"},
    {0xE031, "keybind_dpad_down"},      {0xE032, "keybind_dpad_up"},        {0xE033, "keybind_dpad_up"},
    {0xE034, "keybind_dpad_left"},      {0xE035, "keybind_dpad_up"},        {0xE036, "keybind_dpad_left"},
    {0xE037, "keybind_dpad_up"},
};

// A setting's keys, trimmed ("Q,I" -> Q, I).
std::vector<std::string> BoundKeys(const char* setting) {
  std::string v;
  try {
    v = rex::cvar::Query<std::string>(setting);
  } catch (...) {
    return {};
  }
  std::vector<std::string> keys;
  std::stringstream ss(v);
  for (std::string k; std::getline(ss, k, ',');) {
    while (!k.empty() && k.back() == ' ') k.pop_back();
    while (!k.empty() && k.front() == ' ') k.erase(0, 1);
    if (!k.empty()) keys.push_back(k);
  }
  return keys;
}

// setting -> the one of its keys the player pressed last (the window's key presses, PadIconsKeyDown)
std::mutex g_pressed_mutex;
std::map<std::string, std::string> g_pressed;

std::string ShownKey(const char* setting) {
  const std::vector<std::string> keys = BoundKeys(setting);
  if (keys.empty()) return {};
  if (keys.size() > 1) {
    std::lock_guard lock(g_pressed_mutex);
    if (auto it = g_pressed.find(setting); it != g_pressed.end())
      if (std::find(keys.begin(), keys.end(), it->second) != keys.end()) return it->second;
  }
  return keys[0];
}

// code -> key name (the keycap's), and a version bumped when they change
// (checked at most once a second).
std::mutex g_keys_mutex;
std::map<uint32_t, std::string> g_keys;
std::atomic<uint32_t> g_keys_version{0};
std::chrono::steady_clock::time_point g_keys_checked{};

void CheckBinds() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_keys_checked < std::chrono::seconds(1)) return;
  g_keys_checked = now;
  auto stick = [](const char* up, const char* left, const char* down, const char* right, const char* special) {
    const std::string u = ShownKey(up), l = ShownKey(left), d = ShownKey(down), r = ShownKey(right);
    if (u == "W" && l == "A" && d == "S" && r == "D") return std::string("WASD");
    if (u == "Up" && l == "Left" && d == "Down" && r == "Right") return std::string("ARROWS");
    return u.empty() ? std::string(special) : u;
  };
  std::map<uint32_t, std::string> keys;
  for (const ButtonBind& b : kBinds) {
    const std::string s = b.setting;
    keys[b.code] = s == "lstick" ? stick("keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down",
                                         "keybind_lstick_right", "WASD")
                 : s == "rstick" ? stick("keybind_rstick_up", "keybind_rstick_left", "keybind_rstick_down",
                                         "keybind_rstick_right", "ARROWS")
                                 : ShownKey(b.setting);
  }
  std::lock_guard lock(g_keys_mutex);
  if (keys != g_keys) {
    g_keys = std::move(keys);
    ++g_keys_version;
  }
}

std::atomic<const char*> g_mouse_click{nullptr};

// A key went down: for each button bound to several keys, the one this is (its Shift+/Ctrl+/Alt+ held; a bare
// "Shift" is a key of its own), remembered. The prompts follow at the next CheckBinds.
void PadIconsKeyDown(rex::ui::VirtualKey vk, bool shift, bool ctrl, bool alt) {
  static const std::vector<const char*> settings = [] {
    std::vector<const char*> v;
    auto add = [&](const char* s) {
      if (std::strncmp(s, "keybind_", 8) == 0 &&
          std::find_if(v.begin(), v.end(), [&](const char* t) { return std::strcmp(s, t) == 0; }) == v.end())
        v.push_back(s);
    };
    for (const ButtonBind& b : kBinds) add(b.setting);
    for (const char* s : {"keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right",
                          "keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right"})
      add(s);
    return v;
  }();
  for (const char* setting : settings) {
    const std::vector<std::string> keys = BoundKeys(setting);
    if (keys.size() < 2) continue;
    const std::string* best = nullptr;
    int best_mods = -1;
    for (const std::string& k : keys) {
      std::string_view name(k);
      bool want_shift = false, want_ctrl = false, want_alt = false;
      for (size_t plus; (plus = name.find('+')) != std::string_view::npos && plus > 0;) {
        const std::string_view head = name.substr(0, plus);
        if (head == "Shift") want_shift = true;
        else if (head == "Ctrl" || head == "Control") want_ctrl = true;
        else if (head == "Alt") want_alt = true;
        else break;
        name.remove_prefix(plus + 1);
      }
      if (rex::ui::ParseVirtualKey(name == "Ctrl" ? std::string_view("Control") : name) != vk) continue;
      if ((want_shift && !shift) || (want_ctrl && !ctrl) || (want_alt && !alt)) continue;
      const int mods = int(want_shift) + int(want_ctrl) + int(want_alt);
      if (mods > best_mods) best = &k, best_mods = mods;
    }
    if (!best) continue;
    std::lock_guard lock(g_pressed_mutex);
    std::string& was = g_pressed[setting];
    if (was != *best) {
      REXLOG_INFO("[svr2011] pad icons: {} pressed as {} - its prompts show that key", setting, *best);
      was = *best;
    }
  }
}

// The window's key presses, after the game's own keyboard (typing a name: not a button) and the free camera.
class KeysListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (e.prev_state() || GameKeyboardOpen()) return;  // (held down: already seen)
    PadIconsKeyDown(e.virtual_key(), e.is_shift_pressed(), e.is_ctrl_pressed(), e.is_alt_pressed());
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    using B = rex::ui::MouseEvent::Button;
    const rex::ui::VirtualKey vk = e.button() == B::kLeft     ? rex::ui::VirtualKey::kLButton
                                   : e.button() == B::kRight  ? rex::ui::VirtualKey::kRButton
                                   : e.button() == B::kMiddle ? rex::ui::VirtualKey::kMButton
                                                              : rex::ui::VirtualKey::kNone;
    if (vk == rex::ui::VirtualKey::kNone) return;
    if (!GameKeyboardOpen()) PadIconsKeyDown(vk, false, false, false);
    PadIconsMouseClick(vk == rex::ui::VirtualKey::kLButton ? "LMB" : vk == rex::ui::VirtualKey::kRButton ? "RMB" : "MMB");
  }
};

// ---------------------------------------------------------------------------
// Fonts.

struct Rect {
  uint16_t x, y, w, h;
};
// One kind of icon page (pad_icons.txt "atlas" and the lines after it).
struct Atlas {
  uint64_t hash = 0;
  uint32_t width = 0, height = 0, total = 0;  // the page; the taller picture's height
  std::string file;
  std::map<uint32_t, Rect> xbox_id;  // (identifies the page in a font: code -> its Xbox rect)
  std::map<uint32_t, Rect> ps, pair;
  std::map<uint32_t, Rect> xbox;     // (every Xbox icon's place in the taller picture, from the fonts: Make)
  std::map<std::string, Rect> keys;  // keycaps by key name
  std::once_flag loaded;
  std::vector<uint8_t> rgba;
};
std::vector<std::unique_ptr<Atlas>> g_atlases;

// A font's icon page and its glyphs' versions (guest byte order). The page is
// known again by its glyphs' place and the first glyph's v1 (a font freed and
// another made at the same place is seen as new).
struct FontState {
  uint32_t page = 0, glyphs = 0, count = 0;
  const Atlas* atlas = nullptr;
  Kind applied = kXbox;
  uint64_t applied_layout = kPadMapStandard;
  uint32_t keys_version = ~0u;
  std::array<std::vector<uint8_t>, kKinds> sets;
  std::map<std::pair<Kind, uint64_t>, std::vector<uint8_t>> laid_out;  // (a kind's glyphs for a controller layout)
  const std::vector<uint8_t>& Set(Kind k, uint64_t layout) const {
    if (layout != kPadMapStandard)
      if (auto it = laid_out.find({k, layout}); it != laid_out.end()) return it->second;
    return sets[k];
  }
  float FirstV1() const { return RdF(Set(applied, applied_layout).data() + 12); }
};
std::mutex g_fonts_mutex;
std::unordered_map<uint32_t, FontState> g_fonts;

// The atlas a font's icon page is, by its size and a few Xbox glyphs' places.
const Atlas* Identify(uint32_t pw, uint32_t ph, uint32_t glyphs, uint32_t count) {
  for (const auto& a : g_atlases) {
    if (a->width != pw || a->height != ph) continue;
    bool match = true;
    for (const auto& [code, r] : a->xbox_id) {
      bool found = false;
      for (uint32_t i = 0; i < count && !found; ++i) {
        const uint8_t* g = g_base + glyphs + i * kGlyph;
        if (Rd32(g + 20) != code) continue;
        found = Rd16(g + 16) == r.w && Rd16(g + 18) == r.h && std::lround(RdF(g) * pw) == r.x &&
                std::lround(RdF(g + 4) * ph) == r.y;
      }
      match &= found;
    }
    if (match) return a.get();
  }
  // (once per kind: a page we have no other pictures for - a page with no
  // button glyphs, such as the 64x64 one with only the E018 logo, has
  // nothing to swap and is not logged)
  bool buttons = false;
  for (uint32_t i = 0; i < count && !buttons; ++i) {
    const uint32_t code = Rd32(g_base + glyphs + i * kGlyph + 20);
    for (const auto& b : kBinds) buttons |= b.code == code;
  }
  if (!buttons) return nullptr;
  static std::set<std::string> logged;
  std::string id = fmt::format("{}x{}", pw, ph);
  for (uint32_t i = 0; i < count && i < 12; ++i) {
    const uint8_t* g = g_base + glyphs + i * kGlyph;
    id += fmt::format(" {:04X}:{},{},{},{}", Rd32(g + 20), std::lround(RdF(g) * pw), std::lround(RdF(g + 4) * ph),
                      Rd16(g + 16), Rd16(g + 18));
  }
  if (logged.insert(id).second) REXLOG_INFO("[svr2011] pad icons: icon page not known: {}", id);
  return nullptr;
}

void SetRect(const Atlas& a, uint8_t* g, const Rect& r) {
  WrF(g + 0, float(r.x) / a.width);
  WrF(g + 4, float(r.y) / a.total);
  WrF(g + 8, float(r.x + r.w) / a.width);
  WrF(g + 12, float(r.y + r.h) / a.total);
  Wr16(g + 16, r.w);
  Wr16(g + 18, r.h);
}

// A new font's icon page: its versions (the keyboard one: MakeKeys).
void Make(FontState& st) {
  const Atlas& a = *st.atlas;
  auto& x = st.sets[kXbox];
  x.assign(g_base + st.glyphs, g_base + st.glyphs + st.count * kGlyph);
  const float scale = float(a.height) / float(a.total);
  for (uint32_t i = 0; i < st.count; ++i) {
    uint8_t* g = x.data() + i * kGlyph;
    WrF(g + 4, RdF(g + 4) * scale);
    WrF(g + 12, RdF(g + 12) * scale);
    const_cast<Atlas&>(a).xbox[Rd32(g + 20)] =
        Rect{uint16_t(std::lround(RdF(g) * a.width)), uint16_t(std::lround(RdF(g + 4) * a.total)), Rd16(g + 16), Rd16(g + 18)};
  }
  for (const Kind k : {kPlayStation, kPair}) {
    auto& set = st.sets[k];
    set = x;
    const auto& rects = k == kPlayStation ? a.ps : a.pair;
    for (uint32_t i = 0; i < st.count; ++i) {
      uint8_t* g = set.data() + i * kGlyph;
      if (auto it = rects.find(Rd32(g + 20)); it != rects.end()) SetRect(a, g, it->second);
    }
  }
}

void MakeKeys(FontState& st) {
  const Atlas& a = *st.atlas;
  auto& set = st.sets[kKeyboard];
  set = st.sets[kXbox];
  std::lock_guard lock(g_keys_mutex);
  for (uint32_t i = 0; i < st.count; ++i) {
    uint8_t* g = set.data() + i * kGlyph;
    auto key = g_keys.find(Rd32(g + 20));
    if (key == g_keys.end()) continue;
    if (auto it = a.keys.find(key->second); it != a.keys.end()) SetRect(a, g, it->second);
  }
  st.keys_version = g_keys_version;
}

// A kind's glyphs for a controller layout: each button's glyph shows the button that plays it.
void MakeLaidOut(FontState& st, Kind kind, uint64_t layout) {
  auto& set = st.laid_out[{kind, layout}];
  const auto& from = st.sets[kind];
  set = from;
  for (uint32_t i = 0; i < st.count; ++i) {
    uint8_t* g = set.data() + i * kGlyph;
    const uint32_t code = Rd32(g + 20);
    const int input = PadInputOfCode(code);
    if (input < 0 || PadMapPhysical(layout, input) == input) continue;
    const uint32_t shown = PadCodeOfInput(PadMapPhysical(layout, input), code);
    for (uint32_t j = 0; j < st.count; ++j)
      if (Rd32(from.data() + j * kGlyph + 20) == shown) {
        std::memcpy(g, from.data() + j * kGlyph, 20);  // (the place and size; the code stays)
        break;
      }
  }
}

// Before a page search for an icon code: the font's icon page known, and
// holding the version the current text needs.
void Prepare(uint32_t font) {
  const uint8_t* f = g_base + font;
  const uint32_t pages = Rd32(f), array = Rd32(f + 4);
  if (!array || pages == 0 || pages > 8) return;
  uint32_t page = 0;
  for (uint32_t i = 0; i < pages && !page; ++i) {
    const uint32_t pg = array + i * 24;
    const uint32_t glyphs = Rd32(g_base + pg + 16), count = Rd32(g_base + pg + 8);
    if (glyphs && count && Rd32(g_base + glyphs + 20) >= kFirstCode) page = pg;
  }
  if (!page) return;
  const uint32_t glyphs = Rd32(g_base + page + 16), count = Rd32(g_base + page + 8);
  const Kind want = CurrentKind(true);
  if (want == kKeyboard) CheckBinds();
  const uint64_t layout = want == kKeyboard ? kPadMapStandard : CurrentLayout();
  std::lock_guard lock(g_fonts_mutex);
  FontState& st = g_fonts[font];
  const bool known = st.page == page && st.glyphs == glyphs && st.count == count &&
                     (!st.atlas || st.applied == kKinds || RdF(g_base + glyphs + 12) == st.FirstV1());
  if (!known) {
    st = FontState{page, glyphs, count};
    st.atlas = Identify(Rd32(g_base + page), Rd32(g_base + page + 4), glyphs, count);
    if (!st.atlas) return;
    Make(st);
    st.applied = kKinds;  // (written below)
  }
  if (!st.atlas) return;
  if (want == kKeyboard && st.keys_version != g_keys_version) {
    MakeKeys(st);
    if (st.applied == kKeyboard) st.applied = kKinds;
  }
  if (st.applied == want && st.applied_layout == layout) return;
  if (layout != kPadMapStandard && !st.laid_out.count({want, layout})) MakeLaidOut(st, want, layout);
  const auto& set = st.Set(want, layout);
  std::memcpy(g_base + glyphs, set.data(), set.size());
  st.applied = want;
  st.applied_layout = layout;
}

// ---------------------------------------------------------------------------
// Other pictures.

// A button in a match prompt picture (pad_icons.txt kbbox): the keyboard version shows the key it is bound to
// there.
struct KeyBox {
  uint32_t code;
  int x, y, w, h;
};
struct Sprite {
  uint32_t width = 0, height = 0;
  bool dxt1 = false;
  std::array<std::vector<uint8_t>, kKinds> data;  // (by kind; empty: none. kXbox: the 360 picture, for KeyBoxes)
  std::vector<KeyBox> boxes;
  uint32_t keys_version = ~0u;  // (the bindings the keyboard version was made with)
  std::map<std::pair<Kind, uint64_t>, std::vector<uint8_t>> laid_out;  // (a kind's version for a controller layout)
};
std::unordered_map<uint64_t, Sprite> g_sprites;  // by the 360 picture's FNV-1a 64
std::set<std::pair<uint32_t, uint32_t>> g_sprite_sizes;
// What the renderer saw: physical base address -> the sprite there, and a print of the guest bytes it saw
// there. The game frees pictures and puts others in their place: CREATE A MOVE-SET's move-list buttons (a
// 1024 x 512 sheet) landed where the boot's legal screen (MENU/LEGL, 1024 x 512, a known picture) had been, and
// were swapped for the PlayStation legal text ("THQ Inc", "Ltd" slices, black squares). The swap is made before
// the new picture is ever bound - so before the renderer could report it - hence the print, checked at each swap.
struct Seen {
  const Sprite* sprite = nullptr;
  uint64_t print = 0;
};
std::mutex g_seen_mutex;
std::unordered_map<uint32_t, Seen> g_seen;
std::atomic<size_t> g_seen_count{0};

// A print of a picture's guest bytes (FNV-1a 64 of 2 KB at its start and 2 KB at its middle).
uint64_t Print(uint32_t physical, const Sprite& sp) {
  const size_t size = size_t(sp.width) * sp.height / (sp.dxt1 ? 2 : 1);
  const uint8_t* d = g_memory->TranslatePhysical<const uint8_t*>(physical);
  if (!d) return 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (const size_t at : {size_t(0), size / 2})
    for (size_t i = at; i < at + 2048 && i < size; ++i) h = (h ^ d[i]) * 0x100000001B3ull;
  return h;
}
// Copies of texture objects for the other versions: (object, kind) -> copy.
struct Copy {
  uint32_t object = 0, fetch1 = 0, data = 0;
  uint32_t keys_version = ~0u;  // (a keyboard version: the bindings it shows)
  const Sprite* sprite = nullptr;  // (the picture it is a version of)
};
std::mutex g_copies_mutex;
std::map<std::pair<uint32_t, Kind>, Copy> g_copies;
std::map<std::tuple<uint32_t, Kind, uint64_t>, Copy> g_laid_out_copies;

bool Load() {
  const auto dir = rex::filesystem::GetExecutableFolder() / "pad_icons";
  std::ifstream in(dir / "pad_icons.txt");
  if (!in) return false;
  std::ifstream bin(dir / "sprites.bin", std::ios::binary);
  const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
  std::string line;
  Atlas* a = nullptr;
  while (std::getline(in, line)) {
    std::istringstream s(line);
    std::string kind;
    if (!(s >> kind) || kind[0] == '#') continue;
    if (kind == "atlas") {
      g_atlases.push_back(std::make_unique<Atlas>());
      a = g_atlases.back().get();
      std::string hash;
      s >> hash >> a->width >> a->height >> a->total >> a->file;
      a->hash = std::stoull(hash, nullptr, 16);
      a->file = (dir / a->file).string();
    } else if (a && kind == "xbox") {
      std::string item;
      while (s >> item) {
        uint32_t code = 0;
        Rect r{};
        if (std::sscanf(item.c_str(), "%x:%hu,%hu,%hu,%hu", &code, &r.x, &r.y, &r.w, &r.h) == 5) a->xbox_id[code] = r;
      }
    } else if (a && (kind == "ps" || kind == "pair")) {
      std::string code;
      Rect r{};
      s >> code >> r.x >> r.y >> r.w >> r.h;
      (kind == "ps" ? a->ps : a->pair)[uint32_t(std::stoul(code, nullptr, 16))] = r;
    } else if (a && kind == "key") {
      std::string name;
      Rect r{};
      s >> name >> r.x >> r.y >> r.w >> r.h;
      a->keys[name] = r;
    } else if (kind == "sprite") {
      std::string version, hash, fourcc;
      uint32_t w = 0, h = 0;
      size_t offset = 0, size = 0;
      s >> version >> hash >> w >> h >> fourcc >> offset >> size;
      if (offset + size > blob.size()) continue;
      Sprite& sp = g_sprites[std::stoull(hash, nullptr, 16)];
      sp.width = w, sp.height = h, sp.dxt1 = fourcc == "DXT1";
      sp.data[version == "ps" ? kPlayStation : version == "xb" ? kXbox : kKeyboard].assign(blob.begin() + offset,
                                                                                            blob.begin() + offset + size);
      g_sprite_sizes.insert({w, h});
    } else if (kind == "kbbox") {
      std::string hash, code;
      KeyBox b{};
      s >> hash >> code >> b.x >> b.y >> b.w >> b.h;
      b.code = uint32_t(std::stoul(code, nullptr, 16));
      g_sprites[std::stoull(hash, nullptr, 16)].boxes.push_back(b);
    }
  }
  // (fail safe: an icon page whose taller picture is missing or short is left out - its glyphs are never
  // rewritten, the game's own icons stay; rewritten glyphs with no taller picture sample whatever is there)
  for (auto it = g_atlases.begin(); it != g_atlases.end();) {
    std::error_code ec;
    const auto size = std::filesystem::file_size((*it)->file, ec);
    const uint64_t want = 128 + uint64_t((*it)->width) * (*it)->total * 4;
    if (!ec && size >= want) {
      ++it;
      continue;
    }
    REXLOG_WARN("[svr2011] pad icons: {} is {} - its icon page ({}x{}) keeps the game's own pictures", (*it)->file,
                ec ? "missing" : fmt::format("short ({} of {} bytes)", size, want), (*it)->width, (*it)->height);
    it = g_atlases.erase(it);
  }
  return !g_atlases.empty() || !g_sprites.empty();
}

// A copy of a texture object with its data replaced by `data` (the DDS's
// blocks, linear, little-endian): its rows padded to 256 bytes as the GPU
// wants them.
uint32_t MakeCopy(uint32_t object, const Sprite& sp, const std::vector<uint8_t>& data) {
  const uint32_t bpb = sp.dxt1 ? 8 : 16;
  const uint32_t blocks_x = std::max(1u, (sp.width + 3) / 4), blocks_y = std::max(1u, (sp.height + 3) / 4);
  const uint32_t row = blocks_x * bpb, pitch_bytes = (row + 255) & ~255u;
  // (the 0xE0000000 heap's physical address is a page on from its virtual
  // one: the data is written at the physical address and the fetch constant
  // gets that one too, as the game's own textures have - with the virtual
  // page the native renderer read the page before: the keyboard picture on
  // character select showed bits of a font page)
  const uint32_t at = g_memory->SystemHeapAlloc(pitch_bytes * blocks_y + 4096, 4096, rex::memory::kSystemHeapPhysical);
  const uint32_t copy = g_memory->SystemHeapAlloc(64, 32);
  if (!at || !copy) return 0;
  const uint32_t physical = g_memory->GetPhysicalAddress(at);
  uint8_t* dst = g_memory->TranslatePhysical<uint8_t*>(physical);
  for (uint32_t y = 0; y < blocks_y; ++y) std::memcpy(dst + y * pitch_bytes, data.data() + size_t(y) * row, row);
  std::memcpy(g_base + copy, g_base + object, 52);
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t* words = &fetch.dword_0;
  for (int i = 0; i < 6; ++i) words[i] = Rd32(g_base + copy + 0x1C + i * 4);
  fetch.tiled = 0;
  fetch.pitch = (pitch_bytes / bpb * 4) >> 5;
  fetch.endianness = rex::graphics::xenos::Endian::kNone;
  fetch.base_address = physical >> 12;
  fetch.mip_address = 0;
  fetch.mip_min_level = 0;
  fetch.mip_max_level = 0;
  fetch.packed_mips = 0;
  for (int i = 0; i < 6; ++i) Wr32(g_base + copy + 0x1C + i * 4, words[i]);
  REXLOG_INFO("[svr2011] pad icons: copy of {:08X} ({}x{}) -> {:08X}, data {:08X} (physical {:08X}), fetch {:08X} {:08X}",
              object, sp.width, sp.height, copy, at, g_memory->GetPhysicalAddress(at), words[0], words[1]);
  return copy;
}

}  // namespace

void InstallPadIconsKeys(rex::ui::Window* window) {
  if (!window) return;
  // (test aid: SVR2011_TEST_PAD_KEYS=O,Shift+Up - those keys pressed, as the window would send them)
  if (const char* v = std::getenv("SVR2011_TEST_PAD_KEYS")) {
    std::stringstream ss(v);
    for (std::string k; std::getline(ss, k, ',');) {
      std::string_view name(k);
      bool shift = false, ctrl = false, alt = false;
      for (size_t plus; (plus = name.find('+')) != std::string_view::npos && plus > 0;) {
        const std::string_view head = name.substr(0, plus);
        if (head == "Shift") shift = true;
        else if (head == "Ctrl") ctrl = true;
        else if (head == "Alt") alt = true;
        else break;
        name.remove_prefix(plus + 1);
      }
      PadIconsKeyDown(rex::ui::ParseVirtualKey(name), shift, ctrl, alt);
    }
  }
  static KeysListener listener;
  window->app_context().CallInUIThread([window] { window->AddInputListener(&listener, 995); });
}

const char* TakePadIconsMouseClick() { return g_mouse_click.exchange(nullptr); }
void PadIconsMouseClick(const char* name) { g_mouse_click = name; }

void InstallPadIcons(rex::memory::Memory* memory) {
  g_memory = memory;
  g_base = memory->TranslateVirtual<uint8_t*>(0);
  // (the native renderer draws them; it is the only renderer)
  if (!Load()) {
    REXLOG_INFO("[svr2011] pad icons: no pad_icons/pad_icons.txt - Xbox icons only");
    return;
  }
  g_enabled = true;
  REXLOG_INFO("[svr2011] pad icons: {} icon pages, {} pictures", g_atlases.size(), g_sprites.size());
}

bool PadIconsCandidate(uint32_t width, uint32_t height) {
  if (!g_enabled) return false;
  for (const auto& a : g_atlases)
    if (a->width == width && a->height == height) return true;
  return g_sprite_sizes.count({width, height}) != 0;
}

PadIconsPicture PadIconsAtlas(uint32_t page_width, uint32_t page_height, uint64_t hash) {
  if (!g_enabled) return {};
  for (const auto& a : g_atlases) {
    if (a->hash != hash || a->width != page_width || a->height != page_height) continue;
    Atlas* p = a.get();
    std::call_once(p->loaded, [p] {
      std::ifstream f(p->file, std::ios::binary);
      std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const size_t size = size_t(p->width) * p->total * 4;
      if (d.size() < 128 + size) {
        REXLOG_WARN("[svr2011] pad icons: {} is missing or short", p->file);
        return;
      }
      p->rgba.assign(d.begin() + 128, d.begin() + 128 + size);
      for (size_t i = 0; i < size; i += 4) std::swap(p->rgba[i], p->rgba[i + 2]);  // (BGRA -> RGBA)
    });
    if (p->rgba.empty()) return {};
    return {p->width, p->total, &p->rgba};
  }
  return {};
}

void PadIconsTextureUploaded(uint32_t physical_base, uint32_t width, uint32_t height, uint64_t hash) {
  if (!g_enabled) return;
  const auto it = hash ? g_sprites.find(hash) : g_sprites.end();
  const Sprite* sp = it != g_sprites.end() && it->second.width == width && it->second.height == height ? &it->second
                                                                                                     : nullptr;
  static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
  if (debug && g_sprite_sizes.count({width, height}))
    REXLOG_INFO("[svr2011] pad icons: {}x{} uploaded at {:08X}, hash {:016x}{}", width, height, physical_base, hash,
                sp ? " - a known picture" : "");
  std::lock_guard lock(g_seen_mutex);
  if (sp) {
    g_seen[physical_base] = Seen{sp, Print(physical_base, *sp)};
  } else {
    g_seen.erase(physical_base);
  }
  g_seen_count = g_seen.size();
}

void SetPadIconsOwner(int user_index) { t_owner = user_index; }

// A player's pad state read by the game (XInput layout, big-endian: +4
// buttons, +6 / +7 triggers, +8.. sticks LX LY RX RY): a new press makes it
// the last player to press something. (the keyboard player's right stick is
// the mouse: moving it isn't a press)
void PadIconsInput(uint32_t user_index, const uint8_t* state) {
  if (user_index >= 8) return;
  struct Last {
    uint16_t buttons = 0;
    bool lt = false, rt = false, ls = false, rs = false;
  };
  static Last last[8];
  Last now;
  now.buttons = uint16_t(state[4] << 8 | state[5]);
  now.lt = state[6] > 128, now.rt = state[7] > 128;
  auto out = [&](int at) {
    const int x = int16_t(state[at] << 8 | state[at + 1]), y = int16_t(state[at + 2] << 8 | state[at + 3]);
    return x * x + y * y > 16000 * 16000;
  };
  now.ls = out(8), now.rs = out(12);
  Last& was = last[user_index];
  const bool keyboard = PlayerPadType(user_index) == PadType::kKeyboard;
  const bool pressed = (now.buttons & ~was.buttons) || (now.lt && !was.lt) || (now.rt && !was.rt) ||
                       (now.ls && !was.ls) || (!keyboard && now.rs && !was.rs);
  was = now;
  if (pressed && g_last_input.exchange(int(user_index)) != int(user_index) && std::getenv("SVR2011_PAD_ICONS_LOG"))
    REXLOG_INFO("[svr2011] pad icons: player {} pressed - shared pictures follow them", user_index + 1);
}

}  // namespace svr2011

// The page search: sub_826AD450(font, code) -> the page holding code (or -1).
REX_EXTERN(__imp__sub_826AD450);
REX_HOOK_RAW(sub_826AD450) {
  if (svr2011::g_enabled && ctx.r4.u32 >= svr2011::kFirstCode) {
    // (debug: SVR2011_PAD_ICONS_LOG=2 - who looks icons up with no owner: the
    // guest call chain, each new one once)
    static const int chains = [] { const char* v = std::getenv("SVR2011_PAD_ICONS_LOG"); return v && *v >= '2' ? *v - '0' : 0; }();
    if (chains && (svr2011::t_owner < 0 || chains >= 3)) {
      std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
      uint32_t sp = ctx.r1.u32;
      for (int k = 0; k < 10; ++k) {  // (each frame a little above the last: the stack's own)
        const uint32_t next = svr2011::Rd32(base + sp);
        if (next <= sp || next - sp > 0x4000 || (next & 0xFFF) == 0) break;  // (page-aligned: the stack's top)
        sp = next;
        chain += fmt::format(" {:08X}", svr2011::Rd32(base + sp - 8));
      }
      static std::mutex m;
      static std::set<std::string> seen;
      std::lock_guard lock(m);
      if (seen.insert(fmt::format("{} {}", svr2011::t_owner, chain)).second) REXLOG_INFO("pad icons: owner {}, code {:04X}: {}", svr2011::t_owner, ctx.r4.u32, chain);
    }
    svr2011::Prepare(ctx.r3.u32);
  }
  __imp__sub_826AD450(ctx, base);
}

namespace svr2011 {
namespace {

// Whether a guest address can be read (the game's heaps; a field that holds
// something else is skipped, not followed).
bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xFFFF0000u && (a & 3) == 0; }

// The texture object for a picture's other version (0: none / not known).
// ---------------------------------------------------------------------------
// Keyboard prompts: the match HUD's button pictures with the player's keys. The 360 picture (DXT5) is
// decoded, each button's box (KeyBox) cleared and the keycap of the key it is bound to drawn there (from the
// icon pages' keycaps), and encoded again.

void DecodeDxt5(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
  rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t by = 0; by < (h + 3) / 4; ++by)
    for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, src += 16) {
      uint8_t a[8] = {src[0], src[1]};
      if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i) a[1 + i] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
      } else {
        for (int i = 1; i < 5; ++i) a[1 + i] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0, a[7] = 255;
      }
      uint64_t ai = 0;
      for (int i = 0; i < 6; ++i) ai |= uint64_t(src[2 + i]) << (8 * i);
      const uint16_t c0 = uint16_t(src[8] | src[9] << 8), c1 = uint16_t(src[10] | src[11] << 8);
      uint8_t c[4][3];
      auto unpack = [](uint16_t v, uint8_t* o) {
        o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t(((v >> 5) & 63) * 255 / 63), o[2] = uint8_t((v & 31) * 255 / 31);
      };
      unpack(c0, c[0]), unpack(c1, c[1]);
      for (int k = 0; k < 3; ++k) c[2][k] = uint8_t((2 * c[0][k] + c[1][k]) / 3), c[3][k] = uint8_t((c[0][k] + 2 * c[1][k]) / 3);
      const uint32_t ci = uint32_t(src[12] | src[13] << 8 | src[14] << 16 | uint32_t(src[15]) << 24);
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
        if (x >= w || y >= h) continue;
        uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
        const uint8_t* cc = c[(ci >> (2 * i)) & 3];
        o[0] = cc[0], o[1] = cc[1], o[2] = cc[2], o[3] = a[(ai >> (3 * i)) & 7];
      }
    }
}

void EncodeDxt5(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  out.clear();
  for (uint32_t by = 0; by < (h + 3) / 4; ++by)
    for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
      uint8_t px[16][4] = {};
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = std::min(bx * 4 + i % 4, w - 1), y = std::min(by * 4 + i / 4, h - 1);
        std::memcpy(px[i], &rgba[(size_t(y) * w + x) * 4], 4);
      }
      uint8_t b[16] = {};
      // alpha: 8 steps between the block's highest and lowest
      uint8_t amax = 0, amin = 255;
      for (auto& q : px) amax = std::max(amax, q[3]), amin = std::min(amin, q[3]);
      b[0] = amax, b[1] = amin;
      uint64_t ai = 0;
      if (amax > amin) {
        uint8_t a[8] = {amax, amin};
        for (int i = 1; i < 7; ++i) a[1 + i] = uint8_t(((7 - i) * amax + i * amin) / 7);
        for (int i = 0; i < 16; ++i) {
          int best = 0;
          for (int k = 1; k < 8; ++k)
            if (std::abs(a[k] - px[i][3]) < std::abs(a[best] - px[i][3])) best = k;
          ai |= uint64_t(best) << (3 * i);
        }
      }
      for (int i = 0; i < 6; ++i) b[2 + i] = uint8_t(ai >> (8 * i));
      // colour: the brightest and darkest of the pixels that show, 4 steps
      int hi = -1, lo = -1, lhi = -1, llo = 1 << 30;
      for (int i = 0; i < 16; ++i) {
        if (px[i][3] < 16 && amax >= 16) continue;
        const int l = px[i][0] * 3 + px[i][1] * 6 + px[i][2];
        if (l > lhi) lhi = l, hi = i;
        if (l < llo) llo = l, lo = i;
      }
      if (hi < 0) hi = lo = 0;
      auto pack = [](const uint8_t* q) { return uint16_t((q[0] >> 3) << 11 | (q[1] >> 2) << 5 | (q[2] >> 3)); };
      uint16_t c0 = pack(px[hi]), c1 = pack(px[lo]);
      uint32_t ci = 0;
      if (c0 < c1) std::swap(c0, c1);
      if (c0 != c1) {
        uint8_t c[4][3];
        auto unpack = [](uint16_t v, uint8_t* o) {
          o[0] = uint8_t((v >> 11) * 255 / 31), o[1] = uint8_t(((v >> 5) & 63) * 255 / 63), o[2] = uint8_t((v & 31) * 255 / 31);
        };
        unpack(c0, c[0]), unpack(c1, c[1]);
        for (int k = 0; k < 3; ++k) c[2][k] = uint8_t((2 * c[0][k] + c[1][k]) / 3), c[3][k] = uint8_t((c[0][k] + 2 * c[1][k]) / 3);
        for (int i = 0; i < 16; ++i) {
          int best = 0, bd = 1 << 30;
          for (int k = 0; k < 4; ++k) {
            const int d = (c[k][0] - px[i][0]) * (c[k][0] - px[i][0]) + (c[k][1] - px[i][1]) * (c[k][1] - px[i][1]) +
                          (c[k][2] - px[i][2]) * (c[k][2] - px[i][2]);
            if (d < bd) bd = d, best = k;
          }
          ci |= uint32_t(best) << (2 * i);
        }
      }
      b[8] = uint8_t(c0), b[9] = uint8_t(c0 >> 8), b[10] = uint8_t(c1), b[11] = uint8_t(c1 >> 8);
      for (int i = 0; i < 4; ++i) b[12 + i] = uint8_t(ci >> (8 * i));
      out.insert(out.end(), b, b + 16);
    }
}

// The icon page with the biggest keycaps, its picture loaded.
Atlas* KeycapAtlas() {
  Atlas* best = nullptr;
  for (const auto& a : g_atlases) {
    const auto it = a->keys.find("A");
    if (it != a->keys.end() && (!best || it->second.h > best->keys.at("A").h)) best = a.get();
  }
  if (best) PadIconsAtlas(best->width, best->height, best->hash);  // (loads its picture)
  return best && !best->rgba.empty() ? best : nullptr;
}

// Draws source (RGBA, source_w wide) `from` into box of rgba (w x h), the box cleared first, scaled to fit.
void PaintBox(std::vector<uint8_t>& rgba, int w, int h, const KeyBox& box, const uint8_t* source, int source_w, const Rect& from) {
  for (int y = std::max(box.y, 0); y < std::min(box.y + box.h, h); ++y)
    for (int x = std::max(box.x, 0); x < std::min(box.x + box.w, w); ++x) rgba[(size_t(y) * w + x) * 4 + 3] = 0;
  const float scale = std::min(float(box.w) / from.w, float(box.h) / from.h);
  const int dw = std::max(1, int(from.w * scale)), dh = std::max(1, int(from.h * scale));
  const int ox = box.x + (box.w - dw) / 2, oy = box.y + (box.h - dh) / 2;
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x) {
      const int tx = ox + x, ty = oy + y;
      if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
      const int sx = from.x + std::min(int((x + 0.5f) / scale), from.w - 1);
      const int sy = from.y + std::min(int((y + 0.5f) / scale), from.h - 1);
      const uint8_t* src = &source[(size_t(sy) * source_w + sx) * 4];
      uint8_t* dst = &rgba[(size_t(ty) * w + tx) * 4];
      const int a = src[3];
      for (int c = 0; c < 3; ++c)
        dst[c] = uint8_t((src[c] * a + dst[c] * dst[3] * (255 - a) / 255) / std::max(1, a + dst[3] * (255 - a) / 255));
      dst[3] = uint8_t(a + dst[3] * (255 - a) / 255);
    }
}

// The keyboard version of a prompt picture for the current bindings (caller holds g_copies_mutex).
void BuildKeyboardVersion(Sprite& sp) {
  CheckBinds();
  const uint32_t version = g_keys_version.load();
  if (sp.keys_version == version && !sp.data[kKeyboard].empty()) return;
  Atlas* atlas = KeycapAtlas();
  if (!atlas || sp.data[kXbox].size() < size_t((sp.width + 3) / 4) * ((sp.height + 3) / 4) * 16) return;
  std::map<uint32_t, std::string> keys;
  {
    std::lock_guard lock(g_keys_mutex);
    keys = g_keys;
  }
  const int w = int(sp.width), h = int(sp.height);
  std::vector<uint8_t> rgba;
  DecodeDxt5(sp.data[kXbox].data(), sp.width, sp.height, rgba);
  for (const KeyBox& box : sp.boxes) {
    const auto k = keys.find(box.code);
    const auto r = k != keys.end() ? atlas->keys.find(k->second) : atlas->keys.end();
    if (r == atlas->keys.end()) continue;
    PaintBox(rgba, w, h, box, atlas->rgba.data(), int(atlas->width), r->second);
  }
  EncodeDxt5(rgba, sp.width, sp.height, sp.data[kKeyboard]);
  sp.keys_version = version;
  // (test aid: SVR2011_PAD_ICONS_DUMP=<folder> - each keyboard prompt as made, a TGA)
  if (const char* dir = std::getenv("SVR2011_PAD_ICONS_DUMP")) {
    static int n = 0;
    std::ofstream f(fmt::format("{}/kb_prompt_{:02d}_{}x{}.tga", dir, n++, w, h), std::ios::binary);
    const uint8_t head[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 0x28};
    f.write(reinterpret_cast<const char*>(head), 18);
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    f.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
  }
  static int logged = 0;
  if (logged++ < 40)
    REXLOG_INFO("[svr2011] pad icons: keyboard prompt {}x{} - {} button(s) as keys", sp.width, sp.height, sp.boxes.size());
}

// A kind's version of a match prompt for a controller layout (caller holds g_copies_mutex): each button in it shows
// the button that plays it - from another prompt that has that button (the same art), else the icon pages. Null: the
// layout changes nothing here, or the art isn't there.
const std::vector<uint8_t>* LaidOutVersion(Sprite& sp, Kind kind, uint64_t layout) {
  if (sp.boxes.empty() || sp.dxt1) return nullptr;
  bool changes = false;
  for (const KeyBox& box : sp.boxes) {
    const int input = PadInputOfCode(box.code);
    changes |= input >= 0 && PadMapPhysical(layout, input) != input;
  }
  if (!changes) return nullptr;
  if (auto it = sp.laid_out.find({kind, layout}); it != sp.laid_out.end()) return it->second.empty() ? nullptr : &it->second;
  auto& made = sp.laid_out[{kind, layout}];
  const std::vector<uint8_t>& base_data = sp.data[kind].empty() ? sp.data[kXbox] : sp.data[kind];
  const size_t want = size_t((sp.width + 3) / 4) * ((sp.height + 3) / 4) * 16;
  if (base_data.size() < want) return nullptr;
  const int w = int(sp.width), h = int(sp.height);
  std::vector<uint8_t> rgba;
  DecodeDxt5(base_data.data(), sp.width, sp.height, rgba);
  // (a PlayStation button can be wider than its box, which is the Xbox one's: what's outside it and clear on the Xbox
  // picture goes, or slivers of the old button stay beside the new one)
  if (&base_data != &sp.data[kXbox] && sp.data[kXbox].size() >= want) {
    std::vector<uint8_t> xbox;
    DecodeDxt5(sp.data[kXbox].data(), sp.width, sp.height, xbox);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const bool in_box = std::any_of(sp.boxes.begin(), sp.boxes.end(), [&](const KeyBox& b) {
          return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
        });
        if (!in_box && xbox[(size_t(y) * w + x) * 4 + 3] < 16) rgba[(size_t(y) * w + x) * 4 + 3] = 0;
      }
  }
  int painted = 0;
  for (const KeyBox& box : sp.boxes) {
    const int input = PadInputOfCode(box.code);
    if (input < 0 || PadMapPhysical(layout, input) == input) continue;
    const uint32_t shown = PadInput(PadMapPhysical(layout, input)).code;
    // (the biggest box with that button in another prompt)
    const Sprite* source = nullptr;
    KeyBox from{};
    for (const auto& [hash, other] : g_sprites) {
      if (other.dxt1 || (other.data[kind].empty() && other.data[kXbox].empty())) continue;
      if (kind == kPlayStation && other.data[kPlayStation].empty()) continue;
      for (const KeyBox& b : other.boxes)
        if (b.code == shown && b.w * b.h > from.w * from.h) source = &other, from = b;
    }
    if (source) {
      std::vector<uint8_t> picture;
      DecodeDxt5((source->data[kind].empty() ? source->data[kXbox] : source->data[kind]).data(), source->width, source->height, picture);
      PaintBox(rgba, w, h, box, picture.data(), int(source->width), Rect{uint16_t(from.x), uint16_t(from.y), uint16_t(from.w), uint16_t(from.h)});
      ++painted;
      continue;
    }
    if (Atlas* atlas = KeycapAtlas()) {
      Rect r{};
      bool found = false;
      if (kind == kPlayStation) {
        if (auto it = atlas->ps.find(shown); it != atlas->ps.end()) r = it->second, found = true;
      } else {
        std::lock_guard fonts(g_fonts_mutex);
        if (auto it = atlas->xbox.find(shown); it != atlas->xbox.end()) r = it->second, found = true;
      }
      if (found) {
        PaintBox(rgba, w, h, box, atlas->rgba.data(), int(atlas->width), r);
        ++painted;
      }
    }
  }
  if (!painted) return nullptr;
  EncodeDxt5(rgba, sp.width, sp.height, made);
  // (test aid: SVR2011_PAD_ICONS_DUMP=<folder> - each one as made, a TGA)
  if (const char* dir = std::getenv("SVR2011_PAD_ICONS_DUMP")) {
    static int n = 0;
    std::ofstream f(fmt::format("{}/layout_prompt_{:02d}_{}_{}x{}.tga", dir, n++, kind == kPlayStation ? "ps" : "xbox", w, h), std::ios::binary);
    const uint8_t head[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 0x28};
    f.write(reinterpret_cast<const char*>(head), 18);
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    f.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
  }
  static int logged = 0;
  if (logged++ < 40)
    REXLOG_INFO("[svr2011] pad icons: prompt {}x{} made for a controller layout ({} of {} button(s))", sp.width, sp.height, painted, sp.boxes.size());
  return &made;
}

uint32_t VersionOf(uint32_t object, Kind kind, uint64_t layout = kPadMapStandard) {
  if (!Readable(object) || !g_seen_count.load(std::memory_order_relaxed)) return 0;
  const uint8_t* o = g_base + object;
  if ((Rd32(o) & 0xF) != 3) return 0;  // (D3D resource type: texture)
  const uint32_t size = Rd32(o + 0x1C + 8);  // (fetch dword 2: width - 1, height - 1)
  const uint32_t w = (size & 0x1FFF) + 1, h = ((size >> 13) & 0x1FFF) + 1;
  if (!g_sprite_sizes.count({w, h})) return 0;
  const uint32_t fetch1 = Rd32(o + 0x1C + 4);
  const uint32_t physical = g_memory->GetPhysicalAddress(fetch1 & 0xFFFFF000u);
  const Sprite* sp = nullptr;
  {
    std::lock_guard lock(g_seen_mutex);
    if (auto it = g_seen.find(physical); it != g_seen.end()) {
      // (another picture there now: not the one seen - no swap, until the renderer sees it again)
      if (Print(physical, *it->second.sprite) == it->second.print) {
        sp = it->second.sprite;
      } else {
        static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
        if (debug)
          REXLOG_INFO("[svr2011] pad icons: {}x{} at {:08X} is another picture now - not swapped", w, h, physical);
        g_seen.erase(it);
        g_seen_count = g_seen.size();
      }
    }
  }
  if (!sp) return 0;
  std::lock_guard lock(g_copies_mutex);
  // (a match prompt for a player with a controller layout)
  if (kind != kKeyboard && layout != kPadMapStandard)
    if (const std::vector<uint8_t>* data = LaidOutVersion(const_cast<Sprite&>(*sp), kind, layout)) {
      Copy& c = g_laid_out_copies[{object, kind, layout}];
      if (c.object && (c.fetch1 != fetch1 || c.sprite != sp)) c = Copy{};
      if (!c.object) c = Copy{MakeCopy(object, *sp, *data), fetch1, 0, sp->keys_version, sp};
      return c.object;
    }
  // (a match prompt for a keyboard player: made for the current key bindings)
  if (kind == kKeyboard && !sp->boxes.empty()) BuildKeyboardVersion(const_cast<Sprite&>(*sp));
  if (sp->data[kind].empty()) return 0;
  Copy& c = g_copies[{object, kind}];
  // (the object now holds another picture: at another address, or another one of the same size at the same
  // address - the copy made for the first one would draw it in the second one's place)
  if (c.object && (c.fetch1 != fetch1 || c.sprite != sp)) {
    static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
    static int logged = 0;
    if (debug && c.fetch1 == fetch1 && logged++ < 40)
      REXLOG_INFO("[svr2011] pad icons: {:08X} holds another {}x{} picture now - its copy made again", object, w, h);
    c = Copy{};
  }
  if (c.object && kind == kKeyboard && !sp->boxes.empty() && c.keys_version != sp->keys_version) c = Copy{};
  if (!c.object) c = Copy{MakeCopy(object, *sp, sp->data[kind]), fetch1, 0, sp->keys_version, sp};
  return c.object;
}

// The game's 2D texture records (vtable 0x8205CF48, 0x70 bytes; +80 the D3D
// texture object).
std::mutex g_records_mutex;

// A texture record's picture for a player's kind: the version's copy for
// PlayStation / keyboard. The copy is only put in the record while the
// record is bound for drawing (sub_826E6060, below): a copy left in the
// record was freed by the game with the record (a match with a keyboard
// player crashed at load: the game's heap free of an object from the port's
// system heap).
// Per command, in order: the game records its 2D "set texture" commands
// (sub_826DE6B0) and plays them back later, binding each one's record - and
// one record can be drawn for several players in a frame (character select's
// bottom bar: a controller picture per player, players in the same state
// share it; the COM panel shares one with it). So every command on a texture
// record queues the version it wants (0: the game's own) and each bind of the
// record takes the oldest (record -> {the game's object, our copy}, ...).
struct QueuedWant {
  uint32_t original, copy;
  bool own;  // (a player's Xbox picture: the game's own, not the last pressed controller's - SetTexture below)
};
thread_local bool t_keep_own = false;
std::unordered_map<uint32_t, std::deque<QueuedWant>> g_record_queue;
constexpr size_t kMaxQueued = 64;  // (commands recorded and never played back: dropped)
std::atomic<uint64_t> g_queued{0}, g_bound{0};  // (debug: SVR2011_PAD_ICONS_LOG - the two keep step)
std::atomic<uint64_t> g_drawn_xbox{0}, g_drawn_version{0}, g_drawn_kb{0};  // (debug: at SetTexture, below)

void QueueRecordObject(uint32_t record, Kind kind, bool owned) {
  constexpr uint32_t kTextureRecord = 0x8205CF48;  // (vtable)
  if (!Readable(record) || Rd32(g_base + record) != kTextureRecord || Rd32(g_base + record + 52) != 0) return;
  const uint32_t original = Rd32(g_base + record + 80);
  const uint64_t layout = kind == kKeyboard ? kPadMapStandard : CurrentLayout();
  const uint32_t want = kind == kPlayStation || kind == kKeyboard || layout != kPadMapStandard ? VersionOf(original, kind, layout) : 0;
  std::lock_guard lock(g_records_mutex);
  if (g_record_queue.size() > 4096)  // (records long gone: their queues emptied by now)
    for (auto it = g_record_queue.begin(); it != g_record_queue.end();) it = it->second.empty() ? g_record_queue.erase(it) : std::next(it);
  auto& q = g_record_queue[record];
  if (q.size() >= kMaxQueued) {
    static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
    static std::set<uint32_t> logged;
    if (debug && logged.size() < 32 && logged.insert(record).second)
      REXLOG_INFO("[svr2011] pad icons: record {:08X} (object {:08X}): {} commands never bound - dropped", record, original,
                  q.size());
    q.clear();
  }
  q.push_back(QueuedWant{original, want, owned && !want});
  ++g_queued;
}

// Character select's bottom bar draws its controller pictures in layers - the
// outline for each player with a controller, then the filled one for each, in
// player order: a picture's k-th draw in the bar is that player's (-1: none).
int BarIconOwner(int k) {
  for (uint32_t player = 0; player < kMaxPlayers; ++player)
    if (PlayerPadType(player) != PadType::kNone && k-- == 0) return int(player);
  return -1;
}

// The player behind a character select slot: its setup record's controller
// port (+56; -1 for COM) - mgr *0x82EDE630 + 120 + *(mgr+13520) * 1352 +
// slot * 204.
int SlotPort(uint32_t slot) {
  const uint32_t mgr = Rd32(g_base + 0x82EDE630);
  if (mgr < 0x40000000u || mgr >= 0xF0000000u || slot > 5) return -1;
  const uint32_t mode = Rd32(g_base + mgr + 13520);
  if (mode > 8) return -1;  // (not set up)
  const uint32_t rec = mgr + 120 + mode * 1352 + slot * 204;
  const int32_t port = int32_t(Rd32(g_base + rec + 56));
  return port >= 0 && port < 4 ? port : -1;
}

// A wrestler HUD's player: its character (+80)'s controller port (+1152; 4
// and up: the CPU).
int HudOwner(uint32_t hud) {
  const uint32_t ch = Readable(hud) ? Rd32(g_base + hud + 80) : 0;
  const uint32_t port = Readable(ch) ? Rd32(g_base + ch + 1152) : ~0u;
  return port < 4 ? int(port) : -1;
}

// 2D layouts made by a wrestler's HUD for its prompts (drawn later, by the
// 2D layer): layout -> the HUD (its player is looked up at each draw - the
// HUD gets its character after it makes them, and tag partners swap).
thread_local uint32_t t_making = 0;
thread_local uint32_t t_drawing_hud = 0;  // (debug)
std::mutex g_layouts_mutex;
std::unordered_map<uint32_t, uint32_t> g_layouts;
// ... and their sprites (sub_82416618 makes one; its texture pair at +0x110 is
// what the "set texture" command gets): sprite -> the HUD.
std::unordered_map<uint32_t, uint32_t> g_sprites_hud;
// The wrestler HUDs drawn lately and their players (-1: the CPU's).
struct HudSeen {
  int owner;
  std::chrono::steady_clock::time_point at;
};
std::unordered_map<uint32_t, HudSeen> g_huds;

// A HUD's player; a CPU wrestler's HUD shows its prompts (the submission
// meter, ...) to the human against it: the match's only human, if one.
int ResolveHud(uint32_t hud) {
  const int owner = HudOwner(hud);
  if (owner >= 0) return owner;
  const auto now = std::chrono::steady_clock::now();
  int human = -1;
  for (const auto& [h, seen] : g_huds) {
    if (seen.owner < 0 || now - seen.at > std::chrono::seconds(2)) continue;
    if (human >= 0 && human != seen.owner) return -1;  // (more than one human: no one in particular)
    human = seen.owner;
  }
  return human;
}

struct OwnerScope {
  int saved;
  explicit OwnerScope(int owner) : saved(t_owner) { t_owner = owner; }
  ~OwnerScope() { t_owner = saved; }
};

}  // namespace
}  // namespace svr2011

// The 2D command "set texture" (render list type 24): sub_826DE6B0(cmd,
// pair = {handle, record}, sampler) - written now, played back on the render
// thread (SetTexture there knows nothing of whose picture it is). For a
// player's picture the command gets a copy of the record with the version
// for that player's controller.
REX_EXTERN(__imp__sub_826DE6B0);
REX_HOOK_RAW(sub_826DE6B0) {
  using namespace svr2011;
  // (debug: SVR2011_PAD_ICONS_LOG=1 - each record seen with an owner, once)
  static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
  if (debug && ctx.r4.u32 && Readable(ctx.r4.u32)) {
    static std::set<uint64_t> logged;
    const uint32_t record = Rd32(base + ctx.r4.u32 + 4);
    const uint32_t object = Readable(record) ? Rd32(base + record + 80) : 0;
    if (Readable(record) && (t_owner >= 0 || VersionOf(object, kPlayStation)) && logged.insert(uint64_t(record) << 8 | uint8_t(t_owner + 1)).second &&
        logged.size() < 2000) {
      REXLOG_INFO("[svr2011] pad icons: record {:08X} pair {:08X} owner {} (object {:08X} size {:08X} caller {:08X}){}", record, ctx.r4.u32,
                  t_owner, object, Readable(object) ? Rd32(base + object + 0x24) : 0, uint32_t(ctx.lr),
                  VersionOf(object, kPlayStation) ? " - has a PlayStation version" : "");
    }
  }
  // (a sprite a wrestler HUD made, drawn later by the 2D layer: its player)
  std::optional<OwnerScope> sprite_owner;
  if (g_enabled && t_owner < 0 && ctx.r4.u32) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_sprites_hud.find(ctx.r4.u32 - 0x110); it != g_sprites_hud.end())
      sprite_owner.emplace(ResolveHud(it->second));
  }
  // (the player's version goes into the record itself, +80: playback binds
  // the record the game keeps, not a copy put in the command - a copy left
  // the picture with no texture of its own, drawn with the font page bound
  // before it: the "glitched" keyboard picture on character select; a copy
  // of the record in the sprite's pair - +0x110, its own - isn't bound at
  // playback either, the picture showed the font page again)
  if (g_enabled && ctx.r4.u32) {
    const uint32_t record = Readable(ctx.r4.u32) ? Rd32(base + ctx.r4.u32 + 4) : 0;
    // (the bottom bar's controller pictures: each its own player's - the
    // keyboard player's showed as an Xbox pad, the bar's player 1's)
    std::optional<OwnerScope> bar_owner;
    if (record && t_in_bar && Readable(record) && VersionOf(Rd32(base + record + 80), kKeyboard)) {
      auto it = std::find_if(t_bar_draws.begin(), t_bar_draws.end(), [&](const auto& d) { return d.first == record; });
      if (it == t_bar_draws.end()) it = t_bar_draws.insert(t_bar_draws.end(), {record, 0});
      const int k = it->second++;
      const int owner = BarIconOwner(k);
      if (owner >= 0) bar_owner.emplace(owner);
      if (debug) {
        static std::set<uint64_t> logged;
        const uint64_t key = uint64_t(record) << 16 | uint64_t(k) << 8 | uint8_t(owner + 1) << 4 |
                             uint8_t(PlayerPadType(uint32_t(std::max(owner, 0))));
        if (logged.size() < 64 && logged.insert(key).second)
          REXLOG_INFO("[svr2011] pad icons: bottom bar record {:08X} draw {} is player {}'s (type {})", record, k, owner + 1,
                      int(PlayerPadType(uint32_t(std::max(owner, 0)))));
      }
    }
    if (record) QueueRecordObject(record, t_owner >= 0 ? CurrentKind(false) : kXbox, t_owner >= 0);
  }
  __imp__sub_826DE6B0(ctx, base);
}

// Whose pictures are being drawn:
// - a wrestler's HUD (pins, submissions, button prompts, finishers, ...):
//   sub_82404F60(hud) draws, sub_824050C0(hud) updates - and makes the
//   prompts' layouts (sub_82402818 -> the layout), drawn later through their
//   node (+192, sub_82418B70);
REX_EXTERN(__imp__sub_82404F60);
REX_HOOK_RAW(sub_82404F60) {
  using namespace svr2011;
  int owner = -1;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    g_huds[ctx.r3.u32] = HudSeen{HudOwner(ctx.r3.u32), std::chrono::steady_clock::now()};
    owner = ResolveHud(ctx.r3.u32);
  }
  OwnerScope scope(owner);
  const uint32_t saved = t_drawing_hud;
  t_drawing_hud = ctx.r3.u32;
  __imp__sub_82404F60(ctx, base);
  t_drawing_hud = saved;
}

// (the HUD made: sub_82403E48(hud, match manager + 376); updated, making
// more on demand: sub_824050C0(hud))
REX_EXTERN(__imp__sub_82403E48);
REX_HOOK_RAW(sub_82403E48) {
  using namespace svr2011;
  const uint32_t saved = t_making;
  t_making = g_enabled ? ctx.r3.u32 : 0;
  __imp__sub_82403E48(ctx, base);
  t_making = saved;
}

REX_EXTERN(__imp__sub_824050C0);
REX_HOOK_RAW(sub_824050C0) {
  using namespace svr2011;
  const uint32_t saved = t_making;
  t_making = g_enabled ? ctx.r3.u32 : 0;
  OwnerScope scope(g_enabled ? HudOwner(ctx.r3.u32) : -1);
  __imp__sub_824050C0(ctx, base);
  t_making = saved;
}

REX_EXTERN(__imp__sub_82402818);
REX_HOOK_RAW(sub_82402818) {
  using namespace svr2011;
  __imp__sub_82402818(ctx, base);
  if (!g_enabled || !ctx.r3.u32) return;
  std::lock_guard lock(g_layouts_mutex);
  if (t_making) {
    static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
    if (debug) REXLOG_INFO("[svr2011] pad icons: layout {:08X} made by wrestler HUD {:08X}", ctx.r3.u32, t_making);
    g_layouts[ctx.r3.u32] = t_making;
  } else {
    g_layouts.erase(ctx.r3.u32);  // (a layout of no one's, perhaps where one was)
  }
}

// (a sprite made: sub_82416618(sprite))
REX_EXTERN(__imp__sub_82416618);
REX_HOOK_RAW(sub_82416618) {
  using namespace svr2011;
  const uint32_t sprite = ctx.r3.u32;
  __imp__sub_82416618(ctx, base);
  if (!g_enabled) return;
  std::lock_guard lock(g_layouts_mutex);
  if (t_making) {
    g_sprites_hud[sprite] = t_making;
  } else {
    g_sprites_hud.erase(sprite);
  }
}

// (a layout drawn whole: sub_823D1C48(layout, node, ...))
REX_EXTERN(__imp__sub_823D1C48);
REX_HOOK_RAW(sub_823D1C48) {
  using namespace svr2011;
  int owner = t_owner;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_layouts.find(ctx.r3.u32); it != g_layouts.end()) owner = ResolveHud(it->second);
  }
  OwnerScope scope(owner);
  __imp__sub_823D1C48(ctx, base);
}

REX_EXTERN(__imp__sub_82418B70);
REX_HOOK_RAW(sub_82418B70) {
  using namespace svr2011;
  int owner = t_owner;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_layouts.find(ctx.r3.u32 - 192); it != g_layouts.end()) owner = ResolveHud(it->second);
  }
  OwnerScope scope(owner);
  __imp__sub_82418B70(ctx, base);
}

// A texture record bound for drawing: sub_826E6060(record, sampler) -
// SetTexture(record + 80) (kind 0 records, +52 == 0). A record with a
// version for its player draws the copy, then gets the game's object back.
REX_EXTERN(__imp__sub_826E6060);
REX_HOOK_RAW(sub_826E6060) {
  using namespace svr2011;
  const uint32_t record = ctx.r3.u32;
  uint32_t original = 0, copy = 0;
  bool own = false;
  if (g_enabled) {
    std::lock_guard lock(g_records_mutex);
    if (auto it = g_record_queue.find(record); it != g_record_queue.end() && !it->second.empty()) {
      auto& q = it->second;
      const uint32_t now = Rd32(base + record + 80);
      while (!q.empty() && q.front().original != now) q.pop_front();  // (the record holds another picture now)
      if (!q.empty()) {
        if (q.front().copy && Rd32(base + record + 52) == 0) original = q.front().original, copy = q.front().copy;
        own = q.front().own;
        static const bool debug_bind = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
        static std::set<uint64_t> logged;
        if (debug_bind && (copy || own) && logged.size() < 64 && logged.insert(uint64_t(record) << 32 | copy).second)
          REXLOG_INFO("[svr2011] pad icons: bind of record {:08X}: {}", record,
                      copy ? fmt::format("copy {:08X}", copy) : std::string("the game's own (an owner's Xbox picture)"));
        q.pop_front();
        ++g_bound;
      }
    }
    static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
    static auto last = std::chrono::steady_clock::now();
    if (debug && std::chrono::steady_clock::now() - last > std::chrono::seconds(10)) {
      last = std::chrono::steady_clock::now();
      size_t waiting = 0;
      for (const auto& [r, q] : g_record_queue) waiting += q.size();
      REXLOG_INFO("[svr2011] pad icons: set-texture commands queued {}, bound {}, waiting {}; drawn: Xbox pictures "
                  "that have a version {}, PlayStation versions {}, keyboard versions {}", g_queued.load(),
                  g_bound.load(), waiting, g_drawn_xbox.load(), g_drawn_version.load(), g_drawn_kb.load());
    }
  }
  if (copy) Wr32(base + record + 80, copy);
  t_keep_own = own;
  __imp__sub_826E6060(ctx, base);
  t_keep_own = false;
  if (copy) Wr32(base + record + 80, original);
}

// - a character select panel: sub_82467C18(cursor + 28), the slot at +256;
REX_EXTERN(__imp__sub_82467C18);
REX_HOOK_RAW(sub_82467C18) {
  using namespace svr2011;
  MysterySelectFrame(base);  // (mystery_opponent.h)
  OwnerScope scope(g_enabled ? SlotPort(Rd32(base + ctx.r3.u32 + 256)) : -1);
  __imp__sub_82467C18(ctx, base);
}

// - character select's bottom bar ("contro02_1P"): player 1's slot; its
//   controller pictures, one per player with a controller: each that
//   player's (in the set-texture hook above).
REX_EXTERN(__imp__sub_8244D158);
REX_HOOK_RAW(sub_8244D158) {
  using namespace svr2011;
  OwnerScope scope(g_enabled ? SlotPort(0) : -1);
  t_in_bar = g_enabled;
  t_bar_draws.clear();
  __imp__sub_8244D158(ctx, base);
  t_in_bar = false;
}

// D3DDevice_SetTexture(device, sampler, texture object), on the render
// thread: pictures shared by everyone follow the players' controllers when
// they all use the same kind.
#ifndef SVR2011_D3D_TRACE  // (the census build hooks it to count calls)
REX_EXTERN(__imp__sub_82917EC8);
REX_HOOK_RAW(sub_82917EC8) {
  using namespace svr2011;
  if (g_enabled && ctx.r5.u32 && !t_keep_own) {  // (a player's own Xbox picture: as it is)
    const Kind kind = CurrentKind(false);
    const uint64_t layout = kind == kKeyboard ? kPadMapStandard : CurrentLayout();
    if (kind == kPlayStation || kind == kKeyboard || layout != kPadMapStandard)
      if (const uint32_t version = VersionOf(ctx.r5.u32, kind, layout)) ctx.r5.u64 = version;
  }
  static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
  if (debug && g_enabled && ctx.r5.u32) {
    bool ours = false, kb = false;
    {
      std::lock_guard lock(g_copies_mutex);
      for (const auto& [key, c] : g_copies)
        if (c.object == ctx.r5.u32) ours = true, kb = key.second == kKeyboard;
    }
    // (an Xbox picture that has a PlayStation version drawn as it is / a version drawn)
    if (ours) ++(kb ? g_drawn_kb : g_drawn_version);
    else if (VersionOf(ctx.r5.u32, kPlayStation)) ++g_drawn_xbox;
    static std::set<uint32_t> logged;
    if (ours && logged.insert(ctx.r5.u32).second && logged.size() < 64)
      REXLOG_INFO("[svr2011] pad icons: SetTexture sampler {} gets copy {:08X} fetch {:08X} {:08X} kind {}", ctx.r4.u32,
                  ctx.r5.u32, Rd32(base + ctx.r5.u32 + 0x1C), Rd32(base + ctx.r5.u32 + 0x20), int(CurrentKind(false)));
  }
  __imp__sub_82917EC8(ctx, base);
}
#endif


