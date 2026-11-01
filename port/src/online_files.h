// WWE SmackDown vs. Raw 2011 - what online needs to trust: clean game files
// and the same gameplay mods on both sides (online_files.cpp).
//
// Game files: every file of the disc against the launcher's list
// (launcher/game_files_crc.inc, tools/make_game_file_crcs.py: size and
// CRC-32; menu.pac's menu table left out, string.pac the disc's or the port's
// patched one). Checked once at start on a thread; each file's result is
// cached by size and time (UserData/game_files_check.txt), so only a changed
// file is read again. A file that differs (or is missing): online is off.
//
// Mod set: the enabled mods that change gameplay - Mods\Superstars, Moves,
// Arenas, Backstage (id, version, a CRC of their files but pictures, sounds
// and movies). Cosmetic mods (texture packs, Signs, Media, menu backgrounds,
// music) and the match type switches (Mods\MatchTypes: they only change
// the menus; in an online match every type is on, match_types.h) are left
// out - the other side's "MatchTypes" lines (2.1.1 and older sent the
// switches off) too. Two players play online only with the same set.
//
// Version: only the newest release goes online. At start (on a thread) the
// newest release of the port's GitHub repository (as the launcher's updater:
// pre-releases included), cached for 6 hours (UserData/online_version.txt);
// an older version: online is off. GitHub out of reach: online stays on. In
// a match both players need the same version (the search reply carries it).
//
// (A modified client can lie; this is to stop mixed sets by accident.)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svr2011 {

// The app's paths (before the settings are read), then - with online on
// (InstallOnline) - the checks start (threads).
void SetGameFilesPaths(const std::filesystem::path& game_root, const std::filesystem::path& user_data);
const std::filesystem::path& UserDataFolder();

// A gameplay mod folder's CRC as the mod set has it (a downloaded copy is checked with it: mod_transfer.cpp),
// and plain CRC-32 (a running value, start 0; a file's: false if unreadable).
uint32_t ModFolderCrc(const std::filesystem::path& dir);
uint32_t Crc32(uint32_t crc, const void* data, size_t n);
bool FileCrc32(const std::filesystem::path& file, uint32_t* out);
void StartGameFilesCheck();

// At start, before the game runs: the disc's files (the launcher's list, the optional ones left out) that aren't in
// the game folder - only whether each is there, no reading. `essential`: one the game can't start without
// (default.xex, the .arc lists, pac\...): a folder without pac\_startup.pac / plist360.h crashed 0.3 s in, a read
// of guest 0x28 (2.1.7 report ea8202). Test aid SVR2011_TEST_MISSING_GAME_FILE=<path in the list> - as if missing.
struct MissingGameFile {
  std::string path;  // in the game folder (UTF-8, backslashes)
  bool essential = false;
};
std::vector<MissingGameFile> MissingGameFiles();

// default.xex against the disc's (the launcher's list: its size and CRC-32). The port's code is that xex recompiled,
// so another release's (the Japanese disc: 2.1.8 report 6fd821 - "No function registered at 8216B140", then out)
// can't run. 1: the supported one (or no xex / no list entry: MissingGameFiles' business), 0: another release's,
// -1: unreadable. *size / *crc: what the folder's is. Test aid SVR2011_TEST_OTHER_XEX=1: as if another release's.
int DefaultXexSupported(uint64_t* size, uint32_t* crc);
// The supported one's size and CRC-32 (the list's).
void SupportedXex(uint64_t* size, uint32_t* crc);

enum class GameFiles { kChecking, kClean, kModified };
// The check's result; `first_bad`: the first file that differs (when modified).
GameFiles GameFilesState(std::string* first_bad = nullptr);

// The message for the player when online is off or still checking ("" when
// online may go on): modified game files, an older version, or still checking.
std::string GameFilesOnlineMessage();

// This game's version (VERSION.txt; test aid SVR2011_TEST_VERSION=<x.y.z>).
const std::string& PortVersion();
// Why two players' versions keep them apart ("" when the same).
std::string VersionDifference(const std::string& theirs);

// The gameplay mod set, one line per mod ("kind id version crc name"),
// sorted; "" with none. Read once (mods change only with a restart).
const std::string& GameplayModSet();
// Its hash (FNV-1a 64; 0 with no mods).
uint64_t GameplayModSetHash();

// Why two sets differ, for both players: "Your mods don't match: they have
// Practice Arena 1.0, you don't; ..." (`theirs`: the other side's set;
// superstars = false leaves superstar and arena mods out: they are
// checked in the lobby, online_mods.h).
// The server's switch (its dashboard, with every friends answer): mods in
// online sessions allowed (default) or off - then this game shares no mod set
// and no mod superstar / arena can be picked online.
void SetOnlineModsAllowed(bool on);
bool OnlineModsAllowed();
// The friends poll got no answer (no server, an error): as mods_online off
// for ModsOnlineMessage until it answers again.
void OnlineServerUnanswered();
// The friends poll was refused for this account (2.1.6 servers: no re-activation code / confirmed email yet -
// the launcher's Online tab does the steps): the server's reason, "" when it answers again. ModsOnlineMessage
// says it first.
void SetOnlineAccountBlock(const std::string& why);

// The mods on in this run that change match data (Mods/<kind>/<folder>, not
// "disabled"; the kinds: Superstars - their pack moves are merged into the
// move tables -, Moves, Arenas, Backstage, Weapons, Attires, Media - an
// entrance's length) and the restored match types switched on (Mods/MatchTypes:
// on unless "disabled" - the user's call, 24 Oct). Cosmetic kinds (Backgrounds,
// VsStyles, Signs, texture packs, music) don't count. None with Start Without
// Mods (it turns the match types off too). Read once.
const std::vector<std::string>& ActiveMatchMods();
// Online matches with mods on while the server's mods_online is off (or
// unknown: no answer) are off - the message for the player (their language),
// "" when they may play. Asked where online is entered (GameFilesOnlineMessage:
// the ONLINE menu's gate) and before an invite walks the game to a session.
std::string ModsOnlineMessage();

std::string ModSetDifference(const std::string& mine, const std::string& theirs, bool superstars = true);
// A mod as the online checks know it (Mods/<kind>/<folder>, kind
// "Superstars" or "Arenas"): its key "<folder> <version> <CRC of its
// gameplay files>" (tab-separated) - the same key on two games means the
// same mod - and its name for messages ("Sabu 1.3"); false when it isn't in
// the set.
bool ModKey(const std::string& kind, const std::string& folder, std::string* key, std::string* label);

}  // namespace svr2011
