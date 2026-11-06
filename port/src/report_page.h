// WWE SmackDown vs. Raw 2011 - REPORT A PROBLEM inside the game: MY WWE -> OPTIONS -> REPORT A PROBLEM and, in a
// match, the pause menu's REPORT A PROBLEM (a row above QUIT MATCH; not in online matches - they don't pause). The
// launcher's questions (what kind of problem, how often, what happened, what you did just before, a contact) with
// its rules, a screenshot of the game from before the menu opened (or "add pictures later in the launcher"), the
// match being played (or the last one) with its mods, the main save - sent as the launcher sends its reports
// (launcher/report_flow.c on Windows; on Android the app's ReportSend through GameReport.java) to the same
// /api/reports, its receipt kept in UserData\reports.json for the launcher's Reports tab and the app's My reports.
// Controller (D-pad: choose, left / right: change, A: type / send, B: back), keyboard (arrows, Enter, Esc, typing)
// or touch; text with the keyboard, the phone's keyboard or an on-screen one for the controller. In a match the
// game stays paused while it's open and B goes back to the pause menu.
// Test runs (SVR2011_INPUT_FILE / SVR2011_TEST_*) send only to a server on this PC (SVR2011_ALLOW_LIVE=1 lifts it).

#pragma once

#include <cstdint>
#include <filesystem>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
}  // namespace rex::ui
namespace rex::memory {
class Memory;
}
namespace rex::input {
class InputSystem;
}

namespace svr2011 {

void InstallReportPage(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::ui::Window* window,
                       rex::input::InputSystem* input, rex::memory::Memory* memory,
                       const std::filesystem::path& user_data);
void SetReportPageFonts(ImFont* menu, ImFont* title);
// from_pause: the pause menu's row (the match's recording, the screenshot from before the pause).
void OpenReportPage(bool from_pause);
bool ReportPageHoldsInput();

// A pad state the game is about to get (frame_rate.cpp's XamInputGetState hook; `state` the guest's
// XINPUT_STATE): A on the pause menu's REPORT A PROBLEM opens the page instead (the game never sees it); START in a
// match keeps a screenshot of the frame before the pause.
void ReportPagePad(uint8_t* base, uint8_t* state);
// The pause menu is up (online too). Not the game's pages opened from it: the match clock runs there (see the .cpp).
bool PauseMenuUp(uint8_t* base);

}  // namespace svr2011
