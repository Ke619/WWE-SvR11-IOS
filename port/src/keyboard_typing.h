// WWE SmackDown vs. Raw 2011 - typing on the PC keyboard into the game's
// on-screen keyboard (names in Create-a-Superstar, Story Designer text, ...).
//
// The game already supports a USB keyboard there: while its keyboard is open
// it polls XInputGetKeystroke(XINPUT_FLAG_KEYBOARD) and handles Backspace,
// Enter, Esc, the arrows and typed characters itself (sub_8274B0A8), for the
// user whose XInputGetCapabilities(XINPUT_FLAG_KEYBOARD) says keyboard. The
// runtime never reports a keyboard, so the port answers both calls: user 0
// has one, and its keystrokes are the keys pressed in the game window. The
// controller keeps working as before.

#pragma once

#include <cstdint>
#include <string>

namespace rex::ui {
class Window;
}

namespace svr2011 {

// Once, when the window exists.
void InstallKeyboardTyping(rex::ui::Window* window);

// Automated tests (script_input.cpp): as if typed in the window. `vk` is a
// Windows virtual key (VK_BACK, VK_RETURN, VK_ESCAPE, VK_LEFT, ...).
void TypeText(const std::string& utf8);
void TypeKey(uint16_t vk);
// Test aids (script input "paste <text>" / "copy"): Ctrl+V with this text as
// the clipboard's, and Ctrl+C (its text logged, the clipboard left alone).
void PasteForTest(const std::string& utf8);
void CopyForTest();

// The game's on-screen keyboard is up (it reads the keyboard).
bool GameKeyboardOpen();

// The system's keyboard (a phone's): shown / hidden for typing into the
// game's keyboard (the touch controller's KEYBOARD button). Closed again
// with the game's keyboard (UpdateSystemKeyboard, every frame).
void ToggleSystemKeyboard();
void UpdateSystemKeyboard();

// An overlay's text field (the online overlay's ADD FRIEND) is being typed in:
// the window's text input is on (a phone's keyboard shows), and the keys that
// type characters don't also act as the controller.
void SetOverlayTyping(bool on);
// An overlay's text field is being typed in (SetOverlayTyping on).
bool OverlayTyping();

}  // namespace svr2011
