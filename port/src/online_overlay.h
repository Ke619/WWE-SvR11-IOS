// WWE SmackDown vs. Raw 2011 - the ONLINE overlay: friends, who's online and
// invites to a match, over the game (as the Xbox Guide was).
//
// A key (bind_online_menu, F1) or BACK + RB on the controller opens it, and so
// does the game's own INVITE FRIENDS (lobby, X: XamShowGameInviteUI). It shows
// the account's friends from the Community Creations server (/api/friends:
// In an online match / Online / Offline), who added the player, and invites
// received. In a match session (public or private) A invites an online friend
// (/api/invite: the session's XSESSION_INFO); an invite received is accepted
// with A - through the game's own Xbox LIVE invite path: the port reaches the
// host (p2p.h), posts XN_LIVE_INVITE_ACCEPTED and answers the game's
// XInviteGetAcceptedInfo (sub_8298BA80) with the session, and the game joins
// it itself. As on the console, the game then restarts itself
// (XamLoaderSetLaunchData + XamLoaderLaunchTitle) to join from its boot: the
// port saves the launch data and the invite (UserData\relaunch.bin), starts a
// new copy of the game with the same command line and lets this one end; the
// new one gives them back to the game. While it is open the game sees an idle
// controller. An invite arriving while it's closed shows a notice.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}
namespace rex::input {
class InputSystem;
}
namespace rex::system {
class KernelState;
}

namespace svr2011 {

// Once, when the dialogs are created. `input` may be null.
// (`window`: its touches - a phone; `user_data`: where the relaunch file goes.)
void InstallOnlineOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::input::InputSystem* input,
                          rex::system::KernelState* kernel, const std::filesystem::path& user_data);

// Fonts (null: ImGui's default).
void SetOnlineOverlayFonts(ImFont* menu, ImFont* title);

// Opens / closes it (any thread).
void ToggleOnlineOverlay();

// The game closing: the server shows this player offline now (not after its
// 60 s presence window). A short request; nothing when signed out.
void OnlineOverlaySayGoodbye();

// A notice at the top of the screen for `seconds` (any thread): two lines,
// e.g. "THE HOST LEFT" / "THE MATCH IS OVER".
void OnlineOverlayNotice(const std::string& line1, const std::string& line2, int seconds);

// True while it is open or the button that closed it is still held.
bool OnlineOverlayHoldsInput();

// The game's pad 1 buttons as it reads them (frame_rate.cpp's XInputGetState
// hook, on the game's thread): BACK + RB opens the overlay. Returns the
// buttons the game gets: with online on, BACK as it is, but not the BACK +
// RB combo (from both down until both are let go); while an accepted invite
// takes the player to the session, the walk's own presses.
uint16_t OnlineOverlayPad(uint16_t buttons);
// Pad 1's buttons as last read (XInput bits, before the combo filter).
uint16_t OnlineRawPad();

// What the screens show, for that walk: a string the game looked up
// (menu_hooks.cpp's string lookup: a menu row's description says where its
// cursor is) and the help bar it built (each screen has its own).
void OnlineNoteString(uint32_t id);
void OnlineNoteHelpBar(uint32_t id);

// Where the player is, for the presence poll (&place=: the dashboard's live view) - a short token:
// match_online / match_offline, lobby_host / lobby_join, cc / cc_download / cc_upload, leaderboards,
// matchmaking, online_menu; offline: menus, match_setup, universe, rtwm, shop, my_wwe, options and the
// creation suites (create_superstar, paint_tool, threads, create_story, create_finisher, create_entrance,
// highlight_reel, create_moveset). In an online match `kind` gets ranked / player (empty: not known).
// The modules that see the web / session activity note it (any thread).
enum class OnlineActivity { kSearch, kCcBrowse, kCcDownload, kCcUpload, kLeaderboards, kCount };
void NoteOnlineActivity(OnlineActivity what);
std::string PresencePlace(std::string* kind = nullptr);

}  // namespace svr2011
