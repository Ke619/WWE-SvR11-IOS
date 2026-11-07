// WWE SmackDown vs. Raw 2011 - a controller layout per player (OPTIONS -> CONTROLS -> CONTROLLER).
//
// In play (a match, no pause menu) the buttons and triggers a player's controller reports are swapped around as
// the player chose, in the game's pad read (frame_rate.cpp, XamInputGetState) - before the game, the match recording
// and, online, the game's own input messages see them: peers get the game's buttons, and never need the layout (see
// docs/CONTROLS.md). Menus and the port's pages always get the real buttons, so A, B and START there can't be lost.
//
// A layout is a permutation of the 16 inputs (each game input is on one physical input, a clash is a swap), so every
// action always stays reachable. The sticks aren't remapped. Settings: pad_map_p1 .. pad_map_p4, e.g. "a=x,x=a" -
// game input = physical input, only those that differ. The keyboard's and the mouse's keys are the SDK's keybind_*.

#pragma once

#include <cstdint>
#include <string>

namespace svr2011 {

constexpr int kPadInputs = 16;
constexpr int kPadMapPlayers = 4;
// A layout: 4 bits per game input (the physical input it is on). As a number, it is read and swapped whole.
constexpr uint64_t kPadMapStandard = 0xFEDCBA9876543210ull;

struct PadInputInfo {
  const char* token;   // (in the setting)
  const char* label;   // (on the page)
  const char* action;  // (what it does in a match)
  uint16_t bit;        // (XInput; 0: a trigger)
  uint32_t code;       // (its icon in the game's text)
};
const PadInputInfo& PadInput(int input);

inline int PadMapPhysical(uint64_t layout, int input) { return int((layout >> (input * 4)) & 0xF); }
// The game input an icon code shows (-1: none of these - a stick, a logo), and the code showing `input` in the same
// style as `code`.
int PadInputOfCode(uint32_t code);
uint32_t PadCodeOfInput(int input, uint32_t like_code);

// The player's layout (kPadMapStandard: none, or not a player with one).
uint64_t PadMapLayout(uint32_t player);
// Text <-> layout; a text that isn't a whole permutation: false.
bool PadMapParse(const std::string& text, uint64_t* layout);
std::string PadMapText(uint64_t layout);
// The game input `input` on physical input `physical` (whichever was there takes `input`'s old one). Returns that
// other game input, or -1 when nothing moved.
int PadMapAssign(uint64_t* layout, int input, int physical);
// Takes a layout at once (the page saves the setting).
void SetPadMapLayout(uint32_t player, uint64_t layout);

// Whether layouts apply now (a match, no pause menu), as of the last pad read.
bool PadMapActive();
// The game's pad read: `state` the XINPUT_STATE (buttons at +4, triggers at +6, +7; guest byte order) of user. Called
// with the state every pad read gets; only the local real controllers have a layout.
void PadMapApply(uint8_t* base, uint32_t user, uint8_t* state);

}  // namespace svr2011
