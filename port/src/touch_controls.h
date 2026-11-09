// WWE SmackDown vs. Raw 2011 - on-screen touch controller (phones, tablets
// and touch-screen PCs).
//
// A virtual Xbox 360 pad drawn over the game: its buttons and sticks merge
// with player 1's controller. Two layouts:
//  - MENU: D-pad, A B X Y, bumpers, triggers, START / BACK - a simple layout
//    for the menus.
//  - MATCH: movement and grapple sticks, the face buttons, triggers and
//    bumpers, the D-pad (taunts) and one-touch moves (finisher, run, pin ...)
//    from the game's controls, plus a controls sheet (the "?" button).
// The layout follows the game (a match starting: MATCH; a menu choice:
// MENU) or is switched with its top-left button. The gear button opens the
// editor: drag controls, resize, hide, change what a button presses, the
// opacity, give a control a picture of your own (touch_icons.h) - saved in
// UserData/touch_layout.txt.
//
// Settings: touch_controls (on by default on Android), touch_auto_layout.
// A real controller connected hides it (CreateControllerWatch).

#pragma once

#include <filesystem>
#include <memory>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class DeviceAssignment;
class InputDriver;
class InputSystem;
}  // namespace rex::input

namespace svr2011 {

// The virtual pad, for the input system (made in the input factory).
std::unique_ptr<rex::input::InputDriver> CreateTouchDriver();

// The input system's device assignment (the SDK's SlotAssignment) that also
// counts the real controllers: with one connected the on-screen controller
// hides (a touch shows it again for 15 s). SVR2011_SCRIPT_IS_PAD=1: the
// tests' scripted controller counts as one.
std::unique_ptr<rex::input::DeviceAssignment> CreateControllerWatch();

// Once, when the dialogs are created: the overlay, the editor and the
// window's touches. `user_data`: where touch_layout.txt and touch_icons live.
// `immediate`: makes the textures of the icons (null: no icons).
void InstallTouchControls(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                          const std::filesystem::path& user_data,
                          rex::ui::ImmediateDrawer* immediate = nullptr);

// Font for the labels (null: ImGui's default, scaled).
void SetTouchControlsFont(ImFont* font);

// How far from the left (right) edge something drawn in the band top..bottom
// (the overlay's display units) has to start to keep clear of the visible
// touch controls on that half - 0 when none are there (the performance
// overlay: fps_overlay.cpp). UI thread.
float TouchControlsSideInset(bool right, float top, float bottom);

// True while the editor or the controls sheet is open (the game then sees
// an idle controller).
bool TouchControlsHoldInput();
// What the on-screen controller holds down now (pad_map.h leaves it out of a controller layout).
void TouchControlsHeld(uint16_t* buttons, uint8_t* left_trigger, uint8_t* right_trigger);

// Game state (any thread): a match (entrances, then the match) or a menu.
void TouchGameInMatch(bool in_match);

// Tests (script_input.h "touch"): a touch at x, y (fractions of the window)
// by `pointer`; action 0 down, 1 move, 2 up.
void TouchInject(uint32_t pointer, int action, float x, float y);
constexpr uint32_t kTouchMousePointer = 0xFFFFFFF0u;  // (TouchInject's pointer for the mouse: tests' "click")

}  // namespace svr2011
