// WWE SmackDown vs. Raw 2011 - a controller layout per player (see pad_map.h).

#include "pad_map.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "frame_rate.h"
#include "pad_types.h"
#include "report_page.h"
#include "touch_controls.h"

REXCVAR_DEFINE_STRING(pad_map_p1, "", "Input", "Player 1's controller layout (OPTIONS -> CONTROLS -> CONTROLLER)");
REXCVAR_DEFINE_STRING(pad_map_p2, "", "Input", "Player 2's controller layout");
REXCVAR_DEFINE_STRING(pad_map_p3, "", "Input", "Player 3's controller layout");
REXCVAR_DEFINE_STRING(pad_map_p4, "", "Input", "Player 4's controller layout");

namespace svr2011 {
namespace {

constexpr PadInputInfo kInputs[kPadInputs] = {
    {"a", "A", "ACTION", 0x1000, 0xE009},
    {"b", "B", "IRISH WHIP / PIN", 0x2000, 0xE008},
    {"x", "X", "STRIKE", 0x4000, 0xE00A},
    {"y", "Y", "FINISHER / SIGNATURE", 0x8000, 0xE007},
    {"lb", "LB", "RUN", 0x0100, 0xE00B},
    {"rb", "RB", "TURN", 0x0200, 0xE00E},
    {"lt", "LT", "DRAG", 0, 0xE00C},
    {"rt", "RT", "REVERSE", 0, 0xE00F},
    {"l3", "L3", "TARGET", 0x0040, 0xE00D},
    {"r3", "R3", "SUBMIT", 0x0080, 0xE010},
    {"up", "D-PAD UP", "TAUNT", 0x0001, 0xE003},
    {"down", "D-PAD DOWN", "TAUNT", 0x0002, 0xE004},
    {"left", "D-PAD LEFT", "TAUNT", 0x0004, 0xE006},
    {"right", "D-PAD RIGHT", "TAUNT", 0x0008, 0xE005},
    {"start", "START", "PAUSE", 0x0010, 0xE000},
    {"back", "BACK", "BACK", 0x0020, 0xE001},
};
constexpr int kLt = 6, kRt = 7;

// The other icons of an input, and the one in the same style for each.
struct AltCode {
  uint32_t code;
  int input;
};
constexpr AltCode kAlt[] = {{0xE013, 0},  {0xE014, 1},  {0xE016, 2},  {0xE015, 3},
                            {0xE002, 10}, {0xE02D, 10}, {0xE02E, 11}, {0xE030, 12}, {0xE02F, 13}};

std::array<std::atomic<uint64_t>, kPadMapPlayers> g_layouts = {kPadMapStandard, kPadMapStandard, kPadMapStandard,
                                                               kPadMapStandard};
std::atomic<bool> g_active{false};
// Back in play: a button still held from the menu (the A of RETURN TO GAME) waits to be released - with a layout it
// would be a new press of another button there.
uint32_t g_play_count = 0;
struct HeldOver {
  uint32_t play = 0;
  uint32_t mask = 0;  // (physical inputs)
};
HeldOver g_held_over[kPadMapPlayers];

std::string PadMapSetting(uint32_t player) {
  switch (player) {
    case 0: return REXCVAR_GET(pad_map_p1);
    case 1: return REXCVAR_GET(pad_map_p2);
    case 2: return REXCVAR_GET(pad_map_p3);
    default: return REXCVAR_GET(pad_map_p4);
  }
}

// The settings, at the start and once a second (a change by the page takes at once too: SetPadMapLayout).
void CheckSettings() {
  using Clock = std::chrono::steady_clock;
  static std::array<std::string, kPadMapPlayers> seen;
  static Clock::time_point next{};
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  for (uint32_t p = 0; p < kPadMapPlayers; ++p) {
    std::string text = PadMapSetting(p);
    if (text == seen[p]) continue;
    seen[p] = text;
    uint64_t layout = kPadMapStandard;
    if (!PadMapParse(text, &layout)) {
      REXLOG_WARN("[svr2011] pad map: player {}'s layout \"{}\" isn't one - the standard layout", p + 1, text);
      layout = kPadMapStandard;
    }
    if (g_layouts[p].exchange(layout) != layout)
      REXLOG_INFO("[svr2011] pad map: player {}: {}", p + 1, text.empty() ? "standard" : text);
  }
}

}  // namespace

const PadInputInfo& PadInput(int input) { return kInputs[input]; }

int PadInputOfCode(uint32_t code) {
  for (int i = 0; i < kPadInputs; ++i)
    if (kInputs[i].code == code) return i;
  for (const AltCode& a : kAlt)
    if (a.code == code) return a.input;
  return -1;
}

uint32_t PadCodeOfInput(int input, uint32_t like_code) {
  bool alt = false;
  for (const AltCode& a : kAlt) alt |= a.code == like_code;
  if (alt)
    for (const AltCode& a : kAlt)
      if (a.input == input) return a.code;
  return kInputs[input].code;
}

uint64_t PadMapLayout(uint32_t player) {
  return player < kPadMapPlayers ? g_layouts[player].load(std::memory_order_relaxed) : kPadMapStandard;
}

bool PadMapParse(const std::string& text, uint64_t* layout) {
  uint64_t l = kPadMapStandard;
  std::stringstream ss(text);
  auto find = [](const std::string& t) {
    for (int i = 0; i < kPadInputs; ++i)
      if (t == kInputs[i].token) return i;
    return -1;
  };
  for (std::string item; std::getline(ss, item, ',');) {
    item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) return false;
    const int input = find(item.substr(0, eq)), physical = find(item.substr(eq + 1));
    if (input < 0 || physical < 0) return false;
    l = (l & ~(uint64_t(0xF) << (input * 4))) | uint64_t(physical) << (input * 4);
  }
  uint32_t seen = 0;
  for (int i = 0; i < kPadInputs; ++i) seen |= 1u << PadMapPhysical(l, i);
  if (seen != 0xFFFF) return false;  // (two actions on one input and another with none)
  *layout = l;
  return true;
}

std::string PadMapText(uint64_t layout) {
  std::string out;
  for (int i = 0; i < kPadInputs; ++i) {
    const int p = PadMapPhysical(layout, i);
    if (p == i) continue;
    if (!out.empty()) out += ',';
    out += std::string(kInputs[i].token) + "=" + kInputs[p].token;
  }
  return out;
}

int PadMapAssign(uint64_t* layout, int input, int physical) {
  const int old = PadMapPhysical(*layout, input);
  if (old == physical) return -1;
  int other = -1;
  for (int i = 0; i < kPadInputs; ++i)
    if (PadMapPhysical(*layout, i) == physical) other = i;
  *layout = (*layout & ~(uint64_t(0xF) << (input * 4))) | uint64_t(physical) << (input * 4);
  *layout = (*layout & ~(uint64_t(0xF) << (other * 4))) | uint64_t(old) << (other * 4);
  return other;
}

void SetPadMapLayout(uint32_t player, uint64_t layout) {
  if (player < kPadMapPlayers) g_layouts[player] = layout;
}

bool PadMapActive() {
  // (test aid: SVR2011_TEST_PAD_MAP_ICONS=1 - the prompts in the menus too, to see the text's button icons follow)
  static const bool icons = std::getenv("SVR2011_TEST_PAD_MAP_ICONS") != nullptr;
  return icons || g_active.load(std::memory_order_relaxed);
}

void PadMapApply(uint8_t* base, uint32_t user, uint8_t* state) {
  CheckSettings();
  const bool active = InMatch() && !PauseMenuUp(base);
  if (g_active.exchange(active) != active) {
    if (active) ++g_play_count;
    static const bool log = std::getenv("SVR2011_TEST_PAD_MAP_LOG") != nullptr;
    if (log) REXLOG_INFO("[svr2011] pad map: {}", active ? "in play - layouts apply" : "not in play - the real buttons");
  }
  if (!active) return;
  const uint64_t layout = PadMapLayout(user);
  if (layout == kPadMapStandard) return;
  const PadType type = PlayerPadType(user);
  if (type == PadType::kKeyboard || type == PadType::kNone) return;  // (the keyboard's keys: keybind_*)

  // (the on-screen controller, merged into player 1's, has its own editor: left as it is)
  uint16_t touch_buttons = 0;
  uint8_t touch_lt = 0, touch_rt = 0;
  if (user == 0) TouchControlsHeld(&touch_buttons, &touch_lt, &touch_rt);

  const uint16_t raw = uint16_t((state[4] << 8 | state[5]) & ~touch_buttons);
  const uint8_t lt = touch_lt ? 0 : state[6], rt = touch_rt ? 0 : state[7];
  constexpr uint8_t kPressed = 0x20;  // (a trigger as a button)
  auto held = [&](int physical) {
    return physical == kLt ? lt > kPressed : physical == kRt ? rt > kPressed : (raw & kInputs[physical].bit) != 0;
  };
  uint32_t held_now = 0;
  for (int i = 0; i < kPadInputs; ++i)
    if (held(i)) held_now |= 1u << i;
  HeldOver& over = g_held_over[user];
  if (over.play != g_play_count) over.play = g_play_count, over.mask = held_now;
  over.mask &= held_now;
  auto down = [&](int physical) { return held(physical) && !(over.mask >> physical & 1); };
  auto trigger = [&](int physical) -> uint8_t {
    if (over.mask >> physical & 1) return 0;
    return physical == kLt ? lt : physical == kRt ? rt : down(physical) ? 0xFF : 0;
  };
  uint16_t out = uint16_t((state[4] << 8 | state[5]) & 0x0400);  // (the guide button stays)
  for (int i = 0; i < kPadInputs; ++i)
    if (kInputs[i].bit && down(PadMapPhysical(layout, i))) out |= kInputs[i].bit;
  out |= touch_buttons;
  state[4] = uint8_t(out >> 8), state[5] = uint8_t(out);
  state[6] = std::max(trigger(PadMapPhysical(layout, kLt)), touch_lt);
  state[7] = std::max(trigger(PadMapPhysical(layout, kRt)), touch_rt);
}

}  // namespace svr2011
