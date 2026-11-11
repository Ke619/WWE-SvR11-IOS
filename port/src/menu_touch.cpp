// WWE SmackDown vs. Raw 2011 - tapping the game's own menus (first: finding where things are drawn).
//
// The menus are guest layouts: sub_823D1C48(layout, node, ...) draws a layout, sub_823CE528(layout, node, ..) a group
// node (it adds the node's offset and multiplies its scale into the layout's running transform, layout +144 / +160),
// and sub_823D4008(layout, element, ..) each 256-byte element. An element's descriptor is at +192 (+0 id, +4 type: 1
// text - sub_823D1E98, 2 picture); +196 non-zero: not drawn. Its place, as the text draw works it out:
//   x = e[+160] + L[+144] + e[+4] * L[+160],  y = e[+164] + L[+148] + e[+8] * L[+164]
// Test aid SVR2011_TEST_MENU_TOUCH_LOG=1: each new element drawn, once.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "graphics_page.h"
#include "keyboard_typing.h"
#include "menu_touch.h"
#include "native/native_renderer.h"
#include "p2p.h"
#include "script_input.h"

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// (a guest C string, for the log: printable only)
std::string GuestText(const uint8_t* base, uint32_t address) {
  std::string out;
  if (address < 0x10000) return out;
  for (uint32_t k = 0; k < 48; ++k) {
    const uint8_t c = base[address + k];
    if (!c) break;
    out += c >= 0x20 && c < 0x7F ? char(c) : '.';
  }
  return out;
}

const bool g_log = [] {
  const char* v = std::getenv("SVR2011_TEST_MENU_TOUCH_LOG");
  return v && *v == '1';
}();

void LogElement(const uint8_t* base, uint32_t layout, uint32_t element) {
  const uint8_t* L = base + layout;
  const uint8_t* e = base + element;
  const uint32_t desc = Rd32(e + 192);
  if (desc < 0x10000 || Rd32(e + 196)) return;
  const uint32_t id = Rd32(base + desc), type = Rd32(base + desc + 4);
  if (type != 1 && type != 2) return;
  const float x = RdF(e + 160) + RdF(L + 144) + RdF(e + 4) * RdF(L + 160);
  const float y = RdF(e + 164) + RdF(L + 148) + RdF(e + 8) * RdF(L + 164);
  static std::mutex mutex;
  static std::set<std::tuple<uint32_t, uint32_t, int, int>> seen;
  {
    std::lock_guard lock(mutex);
    if (seen.size() > 20000) return;
    if (!seen.insert({layout, element, int(x), int(y)}).second) return;
  }
  uint32_t group = 0, row = 0;
  svr2011::ScriptMenuState(&group, &row);
  REXLOG_INFO("[svr2011] menu touch: L {:08X} e {:08X} id {} type {} at {:.1f},{:.1f} scale {:.3f},{:.3f} e4 {:.1f},{:.1f} size {:.1f},{:.1f} / {:.1f},{:.1f} menu {:X}/{} \"{}\"",
              layout, element, id, type, x, y, RdF(L + 160), RdF(L + 164), RdF(e + 4), RdF(e + 8), RdF(e + 176), RdF(e + 180),
              RdF(e + 12), RdF(e + 16), group, row, type == 1 ? GuestText(base, Rd32(e + 216)) : "");
}

std::atomic<bool> g_shot_again{false};

// Test aid SVR2011_TEST_MENU_TOUCH_SHOT=<path prefix>: a frame (PPM) 3 s after each menu group change - a hidden test
// game has no window to capture. Called on the draw thread.
void ShotAid() {
  static const char* prefix = std::getenv("SVR2011_TEST_MENU_TOUCH_SHOT");
  if (!prefix) return;
  static uint32_t shown = ~0u, pending = ~0u, taps = 0;
  static bool asked = false;
  if (g_shot_again.exchange(false)) shown = ~0u, pending = ~0u, ++taps;
  static std::chrono::steady_clock::time_point since;
  uint32_t group = 0, row = 0;
  svr2011::ScriptMenuState(&group, &row);
  const auto now = std::chrono::steady_clock::now();
  if (group != pending) pending = group, since = now, asked = false;
  if (pending == shown) return;
  if (!asked && now - since > std::chrono::seconds(3)) {
    svr2011::native::RequestFrameCapture();
    asked = true;
  }
  std::vector<uint8_t> rgb;
  uint32_t w = 0, h = 0;
  if (asked && svr2011::native::TakeFrameCapture(rgb, w, h)) {
    shown = pending;
    const std::string file = std::string(prefix) + "_" + std::to_string(pending) + "_" + std::to_string(taps) + ".ppm";
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
      REXLOG_INFO("[svr2011] menu touch: frame {}x{} -> {}", w, h, file);
    }
  }
}

// The text elements drawn lately, by element address (a slot per element; two sharing one take turns, and a tap only
// looks at the ones seen in the last moments).
using Clock = std::chrono::steady_clock;
struct Drawn {
  uint32_t element = 0, id = 0;
  float x = 0, y = 0;  // (of the text: its top left in a list, its middle on the VS screen)
  Clock::time_point seen, moved;
  char text[48] = {};
};
constexpr uint32_t kSlots = 512;
constexpr auto kRecent = std::chrono::milliseconds(300);   // (slow phones draw at 30 or less)
constexpr auto kSettled = std::chrono::milliseconds(100);  // (a column sliding in isn't a place to tap yet)
Drawn g_drawn[kSlots];
std::mutex g_drawn_mutex;

void NoteText(const uint8_t* base, uint32_t layout, uint32_t element) {
  const uint8_t* L = base + layout;
  const uint8_t* e = base + element;
  const uint32_t desc = Rd32(e + 192);
  if (desc < 0x10000 || Rd32(e + 196) || Rd32(base + desc + 4) != 1) return;
  const uint32_t text = Rd32(e + 216);
  if (text < 0x10000) return;
  const float x = RdF(e + 160) + RdF(L + 144) + RdF(e + 4) * RdF(L + 160);
  const float y = RdF(e + 164) + RdF(L + 148) + RdF(e + 8) * RdF(L + 164);
  const auto now = Clock::now();
  std::lock_guard lock(g_drawn_mutex);
  // (its slot, or the next free or stale one: two elements drawn every frame on one slot took turns, and one - the tag
  // and 6-man selects' "YES" - was never there when a tap looked)
  const uint32_t h = (element >> 8 ^ layout >> 6) & (kSlots - 1);
  Drawn* slot = &g_drawn[h];
  for (uint32_t k = 0; k < 8; ++k) {
    Drawn& s = g_drawn[(h + k) & (kSlots - 1)];
    if (s.element == element) {
      slot = &s;
      break;
    }
    if (slot->element && slot->element != element && (!s.element || now - s.seen > std::chrono::seconds(1))) slot = &s;
  }
  Drawn& d = *slot;
  if (d.element != element || std::fabs(d.x - x) > 0.5f || std::fabs(d.y - y) > 0.5f) d.moved = now;
  d.element = element, d.id = Rd32(base + desc), d.x = x, d.y = y, d.seen = now;
  // (as ScriptMenuRow's labels: control codes, such as MY WWE's logo, are left out)
  size_t n = 0;
  for (uint32_t k = 0; k < 48 && n + 1 < sizeof(d.text); ++k) {
    const uint8_t c = base[text + k];
    if (!c) break;
    if (c >= 0x20) d.text[n++] = char(c);
  }
  d.text[n] = 0;
}

// The character select's panels, by player. The tiles aren't layout elements: a fixed table, 21 x 3 tiles of 24 x 30
// from (66.7, 192) of 640 x 360; the wide ones are 2 columns of one kind.
constexpr uint32_t kSelectGroup = 0x7D0, kGridCols = 21, kGridRows = 3;
constexpr float kGridX = 66.7f, kGridY = 192, kTileW = 24, kTileH = 29.7f;
constexpr uint32_t kCharacterTile = 5;
constexpr uint32_t kPanelPlayer = 284, kPanelCol = 88, kPanelRow = 72, kPanelListMode = 28916, kPanelScreen = 272;
constexpr uint32_t kPanelState = 268;   // (2: it has a cursor; 0: not yet, or its pick is settled)
constexpr uint32_t kScreenStage = 16, kScreenVsButton = 72;  // (the select's screen object; stage 2: the VS screen)
std::atomic<uint32_t> g_select_screen{0};
constexpr uint32_t kPanelPicking = 84;  // (0 once a superstar is chosen: "READY / ADVANCED", no cursor on the grid)
struct Panel {
  uint32_t cursor = 0, col = 0, row = 0, mode = 0, state = 0, picking = 0;
  Clock::time_point seen, moved;
};
std::mutex g_panel_mutex;
Panel g_panels[6];
uint32_t g_kinds[kGridRows][kGridCols];
uint32_t g_kinds_screen = 0;
uint8_t* g_base = nullptr;
// A tile for a panel's cursor (done on the guest's thread, at the panel's next draw). wake: player 1 has no cursor yet -
// the first A brings it up on a tile of the game's choosing; this waits for that.
struct Pending {
  uint32_t cursor = 0, col = 0, row = 0;
  bool wake = false;
  Clock::time_point at;
  svr2011::MenuTouchPress press = nullptr;
} g_pending;

void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// Before anyone picks (g_panel_mutex held): no panel has a cursor or has moved one - on the tag and 6-man selects player
// 1's pad still on a slot bar (taps choose the slot), on 1v1 before the first A.
bool SlotPhase() {
  const auto now = Clock::now();
  bool any = false;
  for (const Panel& p : g_panels) {
    if (!p.cursor || now - p.seen >= kRecent) continue;
    any = true;
    if (p.state == 2 || p.moved != Clock::time_point{}) return false;
  }
  return any;
}

// The tile under a tap on the grid: first the cursor goes there, a second tap on it presses A.
bool GridTap(float x, float y, svr2011::MenuTouchPress press) {
  if (x < kGridX || y < kGridY) return false;
  const uint32_t col = uint32_t((x - kGridX) / kTileW), row = uint32_t((y - kGridY) / kTileH);
  if (col >= kGridCols || row >= kGridRows) return false;
  std::lock_guard lock(g_panel_mutex);
  // The panel player 1's pad works: the one with a cursor (on 1v1 their own, panel 0, then a computer's slot once theirs
  // is settled; on the tag and 6-man selects the slot they took, then their partners' and the other team's).
  const auto now = Clock::now();
  const auto here = [&](const Panel& p) { return p.cursor && now - p.seen < kRecent; };
  if (!g_base) return false;
  bool any = false;
  Panel* in_play = nullptr;
  for (Panel& p : g_panels) {
    if (!here(p)) continue;
    any = true;
    if (p.state == 2 && (!in_play || &p == &g_panels[0])) in_play = &p;
  }
  if (g_log) {
    std::string s;
    for (uint32_t k = 0; k < 6; ++k) {
      const Panel& p = g_panels[k];
      if (here(p)) s += fmt::format(" [{} st {} pick {} mode {} at {},{}]", k, p.state, p.picking, p.mode, p.col, p.row);
    }
    REXLOG_INFO("[svr2011] menu touch: grid tap {},{} panels{}", col, row, s);
  }
  if (!any) return false;
  const uint32_t kind = g_kinds[row][col];
  if (!in_play) {
    // (no cursor yet, nothing picked: player 1's first A brings one up - on their slot's panel; the move waits for it)
    if (!SlotPhase()) return false;
    REXLOG_INFO("[svr2011] menu touch: tile {},{} kind {}: A brings the cursor up", col, row, kind);
    g_pending = Pending{0, col, row, true, now, press};
    if (press) press(0x1000);  // (A)
    return true;
  }
  if (in_play->mode != 0) return false;  // (a list is open: not yet)
  if (!in_play->picking) return false;  // (READY: the pad's A or X)
  bool same = in_play->row == row && in_play->col == col;
  if (!same && in_play->row == row && in_play->col < kGridCols && kind != kCharacterTile && g_kinds[row][in_play->col] == kind) {
    const uint32_t lo = std::min(col, in_play->col), hi = std::max(col, in_play->col);
    same = hi - lo == 1;  // (the other half of a wide tile)
  }
  REXLOG_INFO("[svr2011] menu touch: tile {},{} kind {} panel {:08X}: {}", col, row, kind, in_play->cursor, same ? "A" : "moved there");
  if (same) {
    if (press) press(0x1000);  // (A)
    return true;
  }
  g_pending = Pending{in_play->cursor, col, row, false, now, press};
  return true;
}

// A row tapped: the game only draws a cursor it moved itself (written alone, the highlight stays on the old row until the
// next move, and a port page opens without a redraw to hide that). So the cursor goes next to the row, one press of the
// d-pad moves it on, and A follows when it has arrived. A row it can't stop on gets no A.
std::atomic<bool> g_row_moving{false};
struct RowMove {
  uint32_t group = 0, row = 0;
  Clock::time_point at;
  svr2011::MenuTouchPress press = nullptr;
} g_row_move;
std::mutex g_row_mutex;

void RowMoveTick() {
  if (!g_row_moving.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_row_mutex);
  if (!g_row_moving) return;
  const auto waited = Clock::now() - g_row_move.at;
  uint32_t group = 0, cursor = 0;
  if (waited > std::chrono::seconds(1) || !svr2011::ScriptMenuState(&group, &cursor) || group != g_row_move.group) {
    g_row_moving = false;
    return;
  }
  if (cursor != g_row_move.row || waited < std::chrono::milliseconds(120)) return;  // (past the d-pad's own press)
  g_row_moving = false;
  if (g_row_move.press) g_row_move.press(0x1000);  // (A)
}

// The VS screen, after the select and in its group: PLAY, MATCH CREATOR, SELECT ARENA, SIMULATE side by side, in any
// language, so by place: the texts on the buttons' line, in order, their bars meeting half way. A tap selects, as in a list.
bool VsTap(float x, float y, svr2011::MenuTouchPress press) {
  constexpr uint32_t kVsTextId = 1000;
  constexpr float kVsLine = 276;
  const uint32_t screen = g_select_screen;
  if (!screen || !g_base || Rd32(g_base + screen + kScreenStage) != 2 || std::fabs(y - kVsLine) > 11) return false;
  std::vector<float> buttons;
  {
    const auto now = Clock::now();
    std::lock_guard lock(g_drawn_mutex);
    for (const Drawn& d : g_drawn)
      if (d.element && d.text[0] && d.id == kVsTextId && std::fabs(d.y - kVsLine) < 1 && now - d.seen < kRecent && now - d.moved > kSettled)
        buttons.push_back(d.x);
  }
  if (buttons.size() < 2) return false;
  std::sort(buttons.begin(), buttons.end());
  const float half = (buttons[1] - buttons[0]) / 2;
  for (size_t k = 0; k < buttons.size(); ++k) {
    if (std::fabs(x - buttons[k]) > half) continue;
    REXLOG_INFO("[svr2011] menu touch: VS button {} of {}, was {}", k, buttons.size(), Rd32(g_base + screen + kScreenVsButton));
    Wr32(g_base + screen + kScreenVsButton, uint32_t(k));
    if (press) press(0x1000);  // (A)
    return true;
  }
  return false;
}

// The tag and 6-man selects (their own menu groups; the grid, the panels and the VS screen as 1v1's). Before the first
// pick player 1's pad sits on one of the slot bars - 2 or 3 a team, left and right of VS - and the d-pad moves it (left /
// right: the other team; up / down: the team's next bar, down past the last one to the tray of no slot); A takes it. The
// bars' labels are texts (id 1002: "1P" on player 1's, "COM" on the others) at the left or right end of each bar.
constexpr uint32_t kTagGroup = 0x7D3, kSixGroup = 0x7D8, kSlotLabelId = 1002;
constexpr float kMiddle = 320;

struct Label {
  float x, y;
  bool p1;
};
std::vector<Label> SlotLabels() {
  std::vector<Label> out;
  const auto now = Clock::now();
  std::lock_guard lock(g_drawn_mutex);
  for (const Drawn& d : g_drawn)
    if (d.element && d.id == kSlotLabelId && d.text[0] && d.y < kGridY && now - d.seen < kRecent && now - d.moved > kSettled)
      out.push_back({d.x, d.y, std::strcmp(d.text, "1P") == 0});
  return out;
}

// A bar tapped: the pad goes there one d-pad press at a time, each after the last has shown ("1P" moved): the moves
// wrap and pass the tray, so the way is found by looking, not counted. It stops on the bar (A is a second tap), after 8
// presses, or after 4 s.
struct SlotMove {
  bool left = false;
  float y = 0;
  int presses = 0;
  Clock::time_point at, last;
  svr2011::MenuTouchPress press = nullptr;
} g_slot_move;
std::atomic<bool> g_slot_moving{false};
std::mutex g_slot_mutex;

// A few presses in a row, 200 ms apart (SlotMoveTick sends them).
std::vector<uint16_t> g_queue;
svr2011::MenuTouchPress g_queue_press = nullptr;
Clock::time_point g_queue_last;

void QueueTick() {
  if (g_queue.empty() || Clock::now() - g_queue_last < std::chrono::milliseconds(200)) return;
  const uint16_t b = g_queue.front();
  g_queue.erase(g_queue.begin());
  g_queue_last = Clock::now();
  if (g_queue_press) g_queue_press(b);
  if (g_queue.empty()) g_slot_moving = false;
}

void SlotMoveTick() {
  if (!g_slot_moving.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_slot_mutex);
  if (!g_slot_moving) return;
  if (!g_queue.empty()) {
    QueueTick();
    return;
  }
  const auto now = Clock::now();
  if (now - g_slot_move.last < std::chrono::milliseconds(250)) return;  // (the last press's move shown first)
  const auto labels = SlotLabels();
  const auto p1 = std::find_if(labels.begin(), labels.end(), [](const Label& l) { return l.p1; });
  const bool there = p1 != labels.end() && (p1->x < kMiddle) == g_slot_move.left && std::fabs(p1->y - g_slot_move.y) < 2;
  if (there || g_slot_move.presses >= 8 || now - g_slot_move.at > std::chrono::seconds(4) || labels.empty()) {
    REXLOG_INFO("[svr2011] menu touch: slot {} ({} presses)", there ? "reached" : "not reached - stopped", g_slot_move.presses);
    g_slot_moving = false;
    return;
  }
  uint16_t button;
  if (p1 == labels.end())
    button = 0x0001;  // (in the tray: up onto the bars)
  else if ((p1->x < kMiddle) != g_slot_move.left)
    button = g_slot_move.left ? 0x0004 : 0x0008;  // (d-pad left, right: the other team)
  else
    button = g_slot_move.y > p1->y ? 0x0002 : 0x0001;  // (down, up)
  ++g_slot_move.presses, g_slot_move.last = now;
  if (g_slot_move.press) g_slot_move.press(button);
}

// After player 1's own pick and READY the game asks on their bar "CONTROL PARTNERS - YES / NO". The lit answer isn't in
// the words' elements (the red bar is drawn apart), but the team's other bars show it: "1P" while YES is lit, "COM" while
// NO is. A tap on the lit answer is A; on the other, left / right to it, then A.
bool PromptTap(float x, float y, svr2011::MenuTouchPress press) {
  float yes_x = -1, yes_y = 0, no_x = -1, no_y = 0;
  {
    const auto now = Clock::now();
    std::lock_guard lock(g_drawn_mutex);
    for (const Drawn& d : g_drawn) {
      if (!d.element || d.id != 1000 || d.y >= kGridY || now - d.seen >= kRecent || now - d.moved <= kSettled) continue;
      if (std::strcmp(d.text, "YES") == 0) yes_x = d.x, yes_y = d.y;
      if (std::strcmp(d.text, "NO") == 0) no_x = d.x, no_y = d.y;
    }
  }
  if (yes_x < 0 || no_x < 0) return false;
  const auto on_word = [&](float tx, float ty, int letters) { return std::fabs(y - (ty + 5)) < 11 && std::fabs(x - (tx + letters * 4)) < 26; };
  const bool yes = on_word(yes_x, yes_y, 3), no = !yes && on_word(no_x, no_y, 2);
  if (!yes && !no) return false;
  const bool left = yes_x < kMiddle;
  bool yes_lit = false;
  for (const Label& l : SlotLabels())
    if (l.p1 && (l.x < kMiddle) == left) yes_lit = true;
  REXLOG_INFO("[svr2011] menu touch: control partners - {} ({} lit)", yes ? "YES" : "NO", yes_lit ? "YES" : "NO");
  std::lock_guard lock(g_slot_mutex);
  if (yes == yes_lit)
    g_queue = {0x1000};  // (A)
  else
    g_queue = {uint16_t(yes ? 0x0004 : 0x0008), 0x1000};  // (d-pad left / right, A)
  g_queue_press = press, g_queue_last = Clock::time_point{};
  g_slot_moving = true;
  return true;
}

bool SlotTap(float x, float y, svr2011::MenuTouchPress press) {
  {
    std::lock_guard lock(g_panel_mutex);
    if (!SlotPhase()) return false;
  }
  const auto labels = SlotLabels();
  const Label* bar = nullptr;
  for (const Label& l : labels)
    if ((l.x < kMiddle) == (x < kMiddle) && std::fabs(y - l.y) < 13 && (!bar || std::fabs(y - l.y) < std::fabs(y - bar->y)))
      bar = &l;
  if (!bar) return false;
  if (bar->p1) {
    REXLOG_INFO("[svr2011] menu touch: slot bar {:.0f},{:.0f} - player 1's: A", bar->x, bar->y);
    if (press) press(0x1000);  // (A)
    return true;
  }
  REXLOG_INFO("[svr2011] menu touch: slot bar {:.0f},{:.0f} - the pad goes there", bar->x, bar->y);
  std::lock_guard lock(g_slot_mutex);
  g_slot_move = SlotMove{bar->x < kMiddle, bar->y, 0, Clock::now(), Clock::time_point{}, press};
  g_slot_moving = true;
  return true;
}

}  // namespace

namespace svr2011 {

// A character select panel about to draw (pad_icons' hook: on the guest's thread). cursor: its player object.
void MenuTouchPanel(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  g_base = base;
  const uint8_t* c = base + cursor;
  const uint32_t player = Rd32(c + kPanelPlayer), col = Rd32(c + kPanelCol), row = Rd32(c + kPanelRow);
  const uint32_t mode = Rd32(c + kPanelListMode), state = Rd32(c + kPanelState);
  g_select_screen = Rd32(c + kPanelScreen);
  const auto now = Clock::now();
  bool apply = false;
  uint32_t apply_col = 0, apply_row = 0;
  MenuTouchPress apply_press = nullptr;
  {
    std::lock_guard lock(g_panel_mutex);
    if (player < 6) {
      Panel& p = g_panels[player];
      if (p.cursor != cursor || now - p.seen > std::chrono::seconds(1)) {  // (a new screen)
        p = Panel{};
        p.cursor = cursor, p.col = col, p.row = row, p.mode = mode;
      }
      if (p.col != col || p.row != row || p.mode != mode) {
        if (g_log)
          REXLOG_INFO("[svr2011] menu touch: panel of player {} at {},{} list mode {} (was {},{} / {})", player, col, row, mode, p.col, p.row, p.mode);
        p.col = col, p.row = row, p.mode = mode, p.moved = now;
      }
      p.seen = now;
      p.state = state, p.picking = Rd32(c + kPanelPicking);
    }
    // (a wake has no panel yet: the first one whose cursor comes up - player 1's slot)
    const bool waking = g_pending.wake && !g_pending.cursor && g_pending.at != Clock::time_point{};
    if (g_pending.cursor == cursor || waking) {
      if (now - g_pending.at > std::chrono::seconds(2)) {
        g_pending = Pending{};
      } else if (!g_pending.wake || state == 2) {
        apply = true, apply_col = g_pending.col, apply_row = g_pending.row, apply_press = g_pending.press;
        g_pending = Pending{};
      }
    }
  }
  // The cursor's place alone leaves the last superstar as the pick: only the game's own move brings the tile's. So the
  // cursor goes next to the tile (above or below it: those are never wide), and one press of the d-pad moves it on.
  if (apply) {
    const bool from_above = apply_row > 0;
    Wr32(base + cursor + kPanelCol, apply_col);
    Wr32(base + cursor + kPanelRow, from_above ? apply_row - 1 : apply_row + 1);
    if (apply_press) apply_press(from_above ? 0x0002 : 0x0001);  // (d-pad down, up)
  }
  // What each tile is (sub_8244C710: wide tiles are several columns of one kind), once a screen.
  const uint32_t screen = Rd32(c + kPanelScreen);
  if (screen != g_kinds_screen) {
    g_kinds_screen = screen;
    const auto saved = ctx;
    for (uint32_t r = 0; r < kGridRows; ++r) {
      std::string line;
      for (uint32_t k = 0; k < kGridCols; ++k) {
        ctx.r3.u64 = screen, ctx.r4.u64 = k, ctx.r5.u64 = r;
        sub_8244C710(ctx, base);
        g_kinds[r][k] = ctx.r3.u32;
        line += std::to_string(ctx.r3.u32) + " ";
      }
      if (g_log) REXLOG_INFO("[svr2011] menu touch: tile kinds row {}: {}", r, line);
    }
    ctx = saved;
  }
}

bool MenuTouchTap(float fx, float fy, MenuTouchPress press) {
  if (fx < 0 || fx > 1 || fy < 0 || fy > 1) return false;
  g_shot_again = true;
  if (GuestPagesHoldInput() || GameKeyboardOpen() || g_row_moving || g_slot_moving) return false;
  if (P2POnlineMode()) return false;  // (first version: online screens may be synced by input; steer with the pad)
  uint32_t group = 0, cursor = 0;
  if (!ScriptMenuState(&group, &cursor)) return false;
  const float x = fx * 640, y = fy * 360;
  if (group == kSelectGroup) return VsTap(x, y, press) || GridTap(x, y, press);
  if (group == kTagGroup || group == kSixGroup)
    return VsTap(x, y, press) || PromptTap(x, y, press) || SlotTap(x, y, press) || GridTap(x, y, press);
  std::vector<Drawn> drawn;
  {
    const auto now = Clock::now();
    std::lock_guard lock(g_drawn_mutex);
    for (const Drawn& d : g_drawn)
      if (d.element && d.text[0] && now - d.seen < kRecent && now - d.moved > kSettled) drawn.push_back(d);
  }
  // The nearest row of this menu under the finger: rows are 18 apart, and text elsewhere isn't one of its labels. A row's
  // bar is as wide as its column, whatever its text (MY is short): the column is the labels with one element id, the
  // preview column beside it has another. (Text runs about 8 a letter here.)
  struct Row {
    const Drawn* d;
    int row;
  };
  struct Column {
    uint32_t id;
    float lo, hi;
  };
  std::vector<Row> rows;
  std::vector<Column> columns;
  for (const Drawn& d : drawn) {
    const int row = ScriptMenuRow(group, d.text, nullptr);
    if (row < 0) continue;
    rows.push_back({&d, row});
    const float right = d.x + float(std::strlen(d.text)) * 8;
    auto it = std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.id == d.id; });
    if (it == columns.end()) {
      columns.push_back({d.id, d.x, right});
    } else {
      it->lo = std::min(it->lo, d.x), it->hi = std::max(it->hi, right);
    }
  }
  int best = -1;
  float best_d = 11, best_gap = 0;
  std::string label;
  for (const Row& r : rows) {
    const auto column = std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.id == r.d->id; });
    if (x < column->lo - 16 || x > column->hi + 16) continue;
    const float distance = std::fabs(y - (r.d->y + 8));
    // (rows side by side: the one whose text is nearest the finger)
    const float gap = std::max({r.d->x - x, x - (r.d->x + float(std::strlen(r.d->text)) * 8), 0.0f});
    const bool same_line = best >= 0 && std::fabs(distance - best_d) < 2;
    if (same_line ? gap >= best_gap : distance >= best_d) continue;
    best = r.row, best_d = distance, best_gap = gap, label = r.d->text;
  }
  if (best < 0) return false;
  REXLOG_INFO("[svr2011] menu touch: tap {:.0f},{:.0f} -> group {:X} row {} ({}), cursor was {}", x, y, group, best, label, cursor);
  if (uint32_t(best) == cursor) {
    if (press) press(0x1000);  // (A)
    return true;
  }
  {
    std::lock_guard lock(g_row_mutex);
    g_row_move = RowMove{group, uint32_t(best), Clock::now(), press};
    g_row_moving = true;
  }
  ScriptMenuSetRow(best > 0 ? uint32_t(best - 1) : 1);
  if (press) press(best > 0 ? 0x0002 : 0x0001);  // (d-pad down, up)
  return true;
}

}  // namespace svr2011

REX_EXTERN(__imp__sub_823D4008);
REX_HOOK_RAW(sub_823D4008) {
  if (g_log) LogElement(base, ctx.r3.u32, ctx.r4.u32);
  NoteText(base, ctx.r3.u32, ctx.r4.u32);
  RowMoveTick();
  SlotMoveTick();
  ShotAid();
  __imp__sub_823D4008(ctx, base);
}
