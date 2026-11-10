// WWE SmackDown vs. Raw 2011 - PC keyboard typing (see keyboard_typing.h).

#include "keyboard_typing.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include <SDL3/SDL_clipboard.h>
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include "generated/default/svr2011_init.h"
#include "online_chat.h"
#include "story_designer.h"

namespace {

using rex::ui::VirtualKey;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFlagKeyboard = 0x02;    // XINPUT_FLAG_KEYBOARD
constexpr uint16_t kKeyDown = 0x0001;       // XINPUT_KEYSTROKE_KEYDOWN
constexpr uint16_t kKeyUp = 0x0002;         // XINPUT_KEYSTROKE_KEYUP
constexpr uint16_t kVkShift = 0x10;
constexpr uint32_t kErrorEmpty = 0x10D2;    // ERROR_EMPTY: no keystroke waiting
constexpr uint32_t kErrorNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
constexpr uint8_t kDevTypeKeyboard = 0x02;  // XINPUT_DEVTYPE_USB_KEYBOARD
// The game polls every frame while its keyboard is open; a longer gap means
// it closed and the window's keys go back to the controller emulation.
constexpr auto kOpenTimeout = std::chrono::milliseconds(300);

struct Stroke {
  uint16_t vk = 0;
  uint16_t unicode = 0;
  uint16_t flags = kKeyDown;
};

std::mutex g_mutex;
std::deque<Stroke> g_strokes;
std::atomic<int64_t> g_last_poll{0};  // Clock ticks of the game's last keyboard poll
rex::ui::Window* g_window = nullptr;
std::atomic<uint32_t> g_keyboard{0};  // the open keyboard object (sub_8274B0A8's r3)
uint8_t* g_base = nullptr;
bool g_text_input = false;  // UI thread: we turned the window's text input on
std::atomic<bool> g_overlay_text{false};  // an overlay's text field wants typing (SetOverlayTyping)

int64_t Now() { return Clock::now().time_since_epoch().count(); }

bool KeyboardOpen() {
  const int64_t last = g_last_poll.load();
  return last && Now() - last < std::chrono::duration_cast<Clock::duration>(kOpenTimeout).count();
}

void Push(Stroke s) {
  std::lock_guard lock(g_mutex);
  if (g_strokes.size() < 256) g_strokes.push_back(s);
}

// A typed character. The game sets a letter's case from its own Shift state
// (sub_8274ADC0), which an Xbox keyboard drives with Shift key events, so a
// letter goes with Shift down (capital) or up (small).
void PushChar(uint16_t cp) {
  const bool upper = cp >= 'A' && cp <= 'Z', lower = cp >= 'a' && cp <= 'z';
  if (upper) Push({kVkShift, 0, kKeyDown});
  if (lower) Push({kVkShift, 0, kKeyUp});
  Push({0, cp});
  if (upper) Push({kVkShift, 0, kKeyUp});
}

// Text input (SDL's, for OnKeyChar) is on only while the game's keyboard is.
void SetTextInput(bool on) {
  if (on == g_text_input || !g_window) return;
  g_text_input = on;
  g_window->SetTextInputActive(on);
}

// The keys the game handles itself (sub_8274B0A8); their character codes as
// an Xbox keyboard reports them.
bool GameKey(VirtualKey vk, uint16_t* unicode) {
  switch (vk) {
    case VirtualKey::kBack: *unicode = 8; return true;
    case VirtualKey::kReturn: *unicode = 13; return true;
    case VirtualKey::kEscape: *unicode = 27; return true;
    case VirtualKey::kLeft:
    case VirtualKey::kRight:
    case VirtualKey::kUp:
    case VirtualKey::kDown: *unicode = 0; return true;
    default: return false;
  }
}

// Keys that type a character (their text arrives as OnKeyChar).
bool CharacterKey(VirtualKey vk) {
  const auto v = uint16_t(vk);
  return v == 0x20 || (v >= 0x30 && v <= 0x39) || (v >= 0x41 && v <= 0x5A) ||
         (v >= 0x60 && v <= 0x6F) || (v >= 0xBA && v <= 0xC0) || (v >= 0xDB && v <= 0xDF) ||
         v == 0xE2;
}

// A character the game's keyboard can type (sub_8274ADC0 takes 32-125 but
// '\'): accented Latin letters as their plain ones, line breaks and tabs as
// spaces, anything else as '?'. 0: leave out.
uint16_t Typeable(uint32_t cp) {
  if (cp == 0x0D) return 0;
  if (cp == 0x0A || cp == 0x09) return ' ';
  if (cp == 0x5C) return '/';
  if (cp >= 32 && cp <= 125) return uint16_t(cp);
  if (cp >= 0xC0 && cp <= 0xFF) {
    static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    return uint16_t(kLatin1[cp - 0xC0]);
  }
  if (cp == 0x2018 || cp == 0x2019) return '\'';
  if (cp == 0x201C || cp == 0x201D) return '"';
  if (cp == 0x2013 || cp == 0x2014) return '-';
  return '?';
}

// Text typed into the field as Ctrl+V does (the game keeps it to the
// field's length). Up to 120 characters (the keystroke queue holds 256).
void PasteText(const char* text) {
  int typed = 0;
  for (size_t i = 0; text[i] && typed < 120;) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (size_t k = 1; k < n && text[i + k]; ++k) cp = cp << 6 | (uint8_t(text[i + k]) & 0x3F);
    i += n;
    if (const uint16_t t = Typeable(cp)) PushChar(t), ++typed;
  }
  REXLOG_INFO("[svr2011] keyboard: pasted {} characters", typed);
}

// Ctrl+V: the clipboard's text.
void Paste() {
  char* text = SDL_GetClipboardText();
  if (!text) return;
  PasteText(text);
  SDL_free(text);
}

// The open field's text: the keyboard's editor (*(*(keyboard + 268) + 16),
// whose sub_8274ADC0 types a character) points at it from +0xB80 (UTF-8).
std::string FieldText() {
  const uint32_t kb = g_keyboard.load();
  if (!kb || !g_base || !KeyboardOpen()) return {};
  auto rd = [&](uint32_t a) { const uint8_t* p = g_base + a; return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
  const uint32_t mid = rd(kb + 268);
  const uint32_t ed = mid ? rd(mid + 16) : 0;
  const uint32_t text = ed ? rd(ed + 0xB80) : 0;
  // (in the keyboard's heap, as the editor is)
  if (!text || (text & 0xFF000000u) != (ed & 0xFF000000u) || (text & 0xFFFFFFu) > 0xFFF000u) return {};
  return std::string(reinterpret_cast<const char*>(g_base + text), strnlen(reinterpret_cast<const char*>(g_base + text), 256));
}

// Ctrl+C: the field's text to the clipboard.
void Copy() {
  const std::string t = FieldText();
  if (t.empty()) return;
  SDL_SetClipboardText(t.c_str());
  REXLOG_INFO("[svr2011] keyboard: copied {} characters", t.size());
}

// Ahead of the controller emulation (keyboard -> pad) and the overlays: while
// the game's keyboard is open, typing goes to it and nowhere else.
class TypingListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!KeyboardOpen() && g_overlay_text) {
      // (an overlay's field: its characters come as text; the keys that type
      // them don't also drive the keyboard's controller emulation)
      SetTextInput(true);
      const auto vk = uint16_t(e.virtual_key());
      if (!e.is_ctrl_pressed() && !e.is_alt_pressed() && CharacterKey(e.virtual_key()) && vk != 0x0D && vk != 0x1B)
        e.set_handled(true);
      return;
    }
    if (!KeyboardOpen()) {
      SetTextInput(false);
      // Ctrl+C / Ctrl+V on a Story Designer list (story_designer.h)
      const auto vk = uint16_t(e.virtual_key());
      if (e.is_ctrl_pressed() && !e.is_alt_pressed() && (vk == 0x43 || vk == 0x56) && svr2011::StoryListActive()) {
        svr2011::StoryListKey(vk == 0x56);
        e.set_handled(true);
      }
      return;
    }
    SetTextInput(true);
    if (e.is_ctrl_pressed() && !e.is_alt_pressed() && e.virtual_key() == VirtualKey(0x56)) {  // Ctrl+V
      Paste();
      e.set_handled(true);
      return;
    }
    if (e.is_ctrl_pressed() && !e.is_alt_pressed() && e.virtual_key() == VirtualKey(0x43)) {  // Ctrl+C
      Copy();
      e.set_handled(true);
      return;
    }
    if (e.is_ctrl_pressed() || e.is_alt_pressed() || e.is_super_pressed()) return;
    uint16_t unicode = 0;
    if (GameKey(e.virtual_key(), &unicode)) {
      Push({uint16_t(e.virtual_key()), unicode});
      e.set_handled(true);
    } else if (CharacterKey(e.virtual_key())) {
      e.set_handled(true);
    }
  }

  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!KeyboardOpen()) return;
    const auto cp = uint32_t(e.virtual_key());
    if (cp < 0x20 || cp == 0x7F || cp > 0xFFFF) return;
    PushChar(uint16_t(cp));
    e.set_handled(true);
  }
};

}  // namespace

namespace svr2011 {

bool GameKeyboardOpen() { return KeyboardOpen(); }

void ToggleSystemKeyboard() {
  if (!g_window) return;
  g_window->app_context().CallInUIThread([] { SetTextInput(!g_text_input); });
}

void UpdateSystemKeyboard() {
  if (g_text_input && !KeyboardOpen() && !g_overlay_text) SetTextInput(false);
}

bool OverlayTyping() { return g_overlay_text.load(); }

void SetOverlayTyping(bool on) {
  if (g_overlay_text.exchange(on) == on || !g_window) return;
  g_window->app_context().CallInUIThread([on] {
    if (!KeyboardOpen()) SetTextInput(on);  // (a phone shows its keyboard)
  });
}

void InstallKeyboardTyping(rex::ui::Window* window) {
  if (!window) return;
  g_window = window;
  static TypingListener listener;
  window->app_context().CallInUIThread([window] { window->AddInputListener(&listener, 1000); });
}

void TypeText(const std::string& utf8) {
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (size_t k = 1; k < n && i + k < utf8.size(); ++k) cp = cp << 6 | (uint8_t(utf8[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x20 && cp <= 0xFFFF) PushChar(uint16_t(cp));
  }
}

void PasteForTest(const std::string& utf8) { PasteText(utf8.c_str()); }

void CopyForTest() { REXLOG_INFO("[svr2011] keyboard: field text \"{}\" (test: clipboard left alone)", FieldText()); }

void TypeKey(uint16_t vk) {
  uint16_t unicode = 0;
  GameKey(VirtualKey(vk), &unicode);
  Push({vk, unicode});
}

}  // namespace svr2011

// The game's XInputGetKeystroke(user, flags, keystroke) wrapper. Keyboard
// queries (only its on-screen keyboard makes them) get the window's keys.
REX_EXTERN(__imp__sub_82905130);
REX_EXTERN(__imp__sub_8274B0A8);
thread_local uint32_t t_handling = 0;  // (sub_8274B0A8's object while it runs)
REX_HOOK_RAW(sub_8274B0A8) {  // a keyboard's key handling, each frame
  const uint32_t prev = t_handling;
  t_handling = ctx.r3.u32;
  g_base = base;
  __imp__sub_8274B0A8(ctx, base);
  t_handling = prev;
}

// Test aid (SVR2011_TEST_LOBBY_LOG): a text field's limits as a character
// goes in (sub_82730110(field, utf8, ...): its settings at +44 - +40 bytes,
// +44 per line, +52 more lines allowed).
REX_EXTERN(__imp__sub_82730110);
REX_HOOK_RAW(sub_82730110) {
  if (svr2011::OnlineChatBeforeInsert(ctx, base, ctx.r3.u32)) return;  // (online_chat.h: the lobby chat's field)
  static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  static uint32_t logged = 0;
  if (log && ctx.r3.u32 && ctx.r3.u32 != logged) {
    logged = ctx.r3.u32;
    auto rd = [base](uint32_t a) {
      const uint8_t* p = base + a;
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t cfg = rd(ctx.r3.u32 + 44);
    if (cfg >= 0x40000000 && cfg < 0xC0000000)
      REXLOG_INFO("[svr2011] text field {:08X}: bytes {}, per line {}, lines {}", ctx.r3.u32, rd(cfg + 40), rd(cfg + 44),
                  rd(cfg + 52));
  }
  __imp__sub_82730110(ctx, base);
}

REX_HOOK_RAW(sub_82905130) {
  if ((ctx.r4.u32 & 0xFF) != kFlagKeyboard) {
    __imp__sub_82905130(ctx, base);
    return;
  }
  if (!KeyboardOpen() && g_window) {  // just opened: characters on
    g_window->app_context().CallInUIThread([] { SetTextInput(true); });
  }
  g_last_poll = Now();
  if (t_handling) g_keyboard = t_handling;  // (the open keyboard: it reads keys)
  Stroke s;
  {
    std::lock_guard lock(g_mutex);
    if (g_strokes.empty()) {
      ctx.r3.u64 = kErrorEmpty;
      return;
    }
    s = g_strokes.front();
    g_strokes.pop_front();
  }
  uint8_t* k = base + ctx.r5.u32;  // X_INPUT_KEYSTROKE, big-endian
  k[0] = uint8_t(s.vk >> 8), k[1] = uint8_t(s.vk);
  k[2] = uint8_t(s.unicode >> 8), k[3] = uint8_t(s.unicode);
  k[4] = uint8_t(s.flags >> 8), k[5] = uint8_t(s.flags);
  k[6] = ctx.r3.u32 == 0xFF ? 0 : uint8_t(ctx.r3.u32);
  k[7] = 0;
  ctx.r3.u64 = 0;
}

// XInputGetCapabilities(user, flags, caps): its keyboard finds the player's
// keyboard by asking each user for one (sub_8274AD48); user 0 has it.
REX_EXTERN(__imp__sub_82905050);
REX_HOOK_RAW(sub_82905050) {
  if ((ctx.r4.u32 & 0xFF) != kFlagKeyboard) {
    __imp__sub_82905050(ctx, base);
    return;
  }
  if (ctx.r3.u32 != 0 && ctx.r3.u32 != 0xFF) {
    ctx.r3.u64 = kErrorNotConnected;
    return;
  }
  uint8_t* caps = base + ctx.r5.u32;  // X_INPUT_CAPABILITIES
  std::memset(caps, 0, 20);
  caps[0] = kDevTypeKeyboard;
  ctx.r3.u64 = 0;
}
