// My game discs: the user's other WWE games, set once - every game the converters know (Xbox 360 SvR 2007 - WWE
// 2K17, PS2 Just Bring It - SvR 2010) with its disc image (.iso) or extracted folder, recognised by the disc's id
// (the XEX title id / the PS2 serial: the converters' disc_hint). Kept in the Mod Maker's settings; the Port page,
// Other games, Simple superstar and Simple arena read their discs from here (Discs() in app.h is this list).
//
// For other pages (e.g. a Simple porting menu):
//   disc_list::PathFor("svr2008")      -> the disc image / folder set for SvR 2008 ("" if none or gone)
//   disc_list::Games()                 -> every known game (key, name, platform, ids), in the page's order
//   disc_list::GameById("SLES_520.36") -> the game a disc id belongs to
//   disc_list::Set / Remove            -> change the list (saved at once)
//   disc_list::Identify(path)          -> a .iso's or an extracted folder's disc id
#pragma once

#include <string>
#include <vector>

namespace mm {
namespace disc_list {

struct Game {
  const char* key;    // the converters' game id where they agree ("svr2008", "wwe2k14", "ps2_hctp" ...)
  const char* name;   // "SmackDown vs. Raw 2008"
  bool xbox;          // Xbox 360 (else PS2)
  std::vector<const char*> ids;  // its disc ids: XEX title id / PS2 serials (EU first, then US)
};
// Every game the converters support, Xbox 360 first, in release order.
const std::vector<Game>& Games();
const Game* GameByKey(const std::string& key);
const Game* GameById(const std::string& disc_id);
// "SmackDown vs. Raw 2008 (Xbox 360)".
std::string FullName(const Game& g);

// The disc image or extracted folder set for a game ("" when none is set or it is no longer there).
std::wstring PathFor(const std::string& key);
// The folder of the game's DLC packages ("" none or gone): superstar_port reads the DLC's superstars from it, beside
// the disc (tools/dlc_src.py). Set "" removes it; saved at once. A game needs its disc set first.
std::wstring DlcFor(const std::string& key);
void SetDlc(const std::string& key, const std::wstring& folder);
// Its disc id (the id read when it was added; the game's first id when it couldn't be read).
std::string IdFor(const std::string& key);
// Set (path "" removes) / remove a game's disc; saved in the settings at once.
void Set(const std::string& key, const std::wstring& path, const std::string& disc_id = "");
void Remove(const std::string& key);
// The game keys with a disc set (whether or not the path is still there), in Games() order.
std::vector<std::string> SetKeys();

// A game set as a folder (an extract, or only some of the game's files - a player without the disc any more): the
// files `what` reads there that aren't in it, as the player sees them ("pac\m.pac"); "moves" = the moveset and taunts
// (superstar_port moveset / moveset-pack). Empty: all there, not a folder, or nothing known for that game. A PS2 game
// needs its whole disc: SYSTEM.CNF is listed when it isn't there.
std::vector<std::string> MissingFiles(const std::string& key, const std::string& what = "moves");
// A game set as a folder that isn't a whole disc (no default.xex / SYSTEM.CNF): what it can do, in plain words ("" for
// a disc image or a whole extract).
std::string FolderNote(const std::string& key);

// A picked folder that holds none of a game's own files (DLC - an extracted DLC folder or the console's packages - or
// something else): why it can't be used and what to pick instead, in plain words; "" for a disc, an extract or a
// partial copy of a game. (2.1.8 report 20261103-040523-d343a2: a DLC folder picked for a game -> "the game files have
// no pac/ch/chEtc.pac" from the converter.)
std::string NotGameFiles(const std::wstring& folder);

// A disc image's or an extracted folder's disc id ("" when it isn't a disc / has none): the image's default.xex /
// SYSTEM.CNF, or the folder's.
std::string Identify(const std::wstring& path, bool* xbox = nullptr);

// "Find my discs": the folder and everything under it searched for .iso files and extracted game folders (in the
// background); each recognised one is set for its game unless that game already has a disc that is still there.
void FindIn(const std::wstring& folder);
void FindInFolders(const std::vector<std::wstring>& folders);
bool Scanning();

// The page (the rail's "My game discs").
void Draw();
// Test aid --discs-find <folder>[,<report.txt>]: Find my discs at start; with a report, the list is written there
// when the search ends, and the Mod Maker quits.
void TestFind(const std::wstring& spec);

}  // namespace disc_list
}  // namespace mm
