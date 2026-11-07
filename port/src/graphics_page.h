// WWE SmackDown vs. Raw 2011 - the GRAPHICS page (MY WWE -> OPTIONS ->
// GRAPHICS, a menu entry the port adds: tools/patch_menu.py, menu_hooks.cpp).
//
// An in-game page drawn over the menu, on three tabs: DISPLAY (resolution,
// window / full screen, frame rate, VSync, FPS counter, the graphics API...),
// GRAPHICS (render resolution down to 360p, anti-aliasing, textures...) and
// ADV. GRAPHICS (crowd, shadows, effect detail, depth of field, motion blur...). Driven by the controller
// (D-pad / left stick: choose and change, A: change, B: back) or the keyboard
// (arrows, Enter, Esc). While it is open the game sees an idle controller.
// Changes are saved to the settings file (svr2011.toml, shared with the
// launcher); the FPS counter, VSync and fullscreen apply at once, the rest the
// next time the game starts.

#pragma once

#include <filesystem>
#include <string>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}

namespace svr2011 {

// Once, when the dialogs are created. `input` may be null (no input hold).
void InstallGraphicsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                         rex::input::InputSystem* input,
                         const std::filesystem::path& config_path);

// Fonts for the page (from the app's font setup; null: ImGui's default).
void SetGraphicsPageFonts(ImFont* menu, ImFont* title);

// One of the port's pages is up: the game gets no pad input.
bool GuestPagesHoldInput();

// Opens the page (any thread; the guest's menu hook).
void OpenGraphicsPage();

// Saves one setting ("key = value", value as TOML) in the config file.
void SaveConfigSetting(const std::string& key, const std::string& value);

// MY WWE -> OPTIONS -> CONTROLS: the page with only the keyboard bindings.
void OpenControlsPage();

// MY WWE -> OPTIONS -> LANGUAGE: the page with only the language row.
void OpenLanguagePage();

// Closes the game as the window's close button does (any thread).
void RequestExit();

}  // namespace svr2011
