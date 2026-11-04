// Move packs (arenas branch): moves the game doesn't have (ported from
// SvR 2010) added from mods, without touching the game's files.
// docs/MOVE_PACKS.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace svr2011 {

// At start-up, before the game mounts its pacs: merges every enabled move
// pack (Mods/Superstars/<mod>/moves/pack.txt, Mods/Moves/<pack>/pack.txt)
// into copies of m.pac, misc.pac and mpsp.pac in <game>/Mods/PacOverlay
// (rebuilt only when a pack or a game file changed), with a pac list naming
// them there. dlc_root: the installed DLC packages (dlc.cpp, only read) - those bringing
// their own m / mpsp / misc pac (Vol. 1, 2) get a copy with the packs in Mods/PacOverlay/dlc.
void InstallMovePacks(rex::filesystem::VirtualFileSystem* fs, const std::filesystem::path& dlc_root = {});

// The folder (under the game folder) the game reads its pac list from, or ""
// for the game's own. With an overlay the pacs are mounted from their own
// tables (the pre-built directory plist360.arc only knows the originals).
const std::string& PacListFolder();

// Can a moveset name this move: 1 if the game's move records (the last
// MOVS/WAZE loaded, the packs' moves in) have it with category bits, 0 if not
// (no record, or one 2011 kept for a cut move: no motion either), -1 before
// the records are loaded.
int MoveUsable(uint16_t id);

// Is `id` one of the packs' own moves (a waze / wazename / wazecopy line names it)? CREATE A MOVE-SET
// shows those as "MOD: <name>" rows at the end of its lists (cam_mod_moves.cpp).
bool PackMove(uint16_t id);
// The move packs installed (kind "Moves" or "Superstars" - a superstar mod's moves/ - and the mod's folder),
// each pack's moves, and whether the stock game plays a move without any pack: online lobbies agree which
// packs every player has, and a match's move from any other pack plays its category's default
// (online_mods.cpp, cam_mod_moves.cpp) - the same on every game.
std::vector<std::pair<std::string, std::string>> MovePacks();
std::vector<uint16_t> MovePackMoves(const std::string& kind, const std::string& folder);
bool StockPlays(uint16_t id);

// The game move a slot of `category` (0-127) falls back to in a match when its own has no record:
// computed at each MOVS/WAZE load (0 = none / not loaded).
uint16_t CategoryDefaultMove(int category);

// Has a match's move `id` nothing to play: a record with category bits but no motion in the game's banks (its
// DLC's too) or a pack (NoMotionMove), a pack's move whose motions are missing or left out of a bank, or a move a
// pack gave earlier (its id kept in Mods\PacOverlay\pack_moves.txt) that no pack gives now and the stock game
// can't play (no stock record with category bits, no stock EXH data). Other stock moves are never "missing"
// (stock movesets name moves without records, bits or EXH that play fine).
bool MoveMissing(uint16_t id);
// A move record with category bits whose id no bank of the game (m.pac, mpsp.pac, the DLC's) and no pack has a
// motion for: it plays as nothing - a save holding one (the stock 30073 of Sin Cara's old port) froze the match.
// False while the game's motion ids aren't known (Mods\PacOverlay\motion_ids.bin, made by a merge).
bool NoMotionMove(uint16_t id);

// The entrances the move packs add (pacentry EVP? <n>, n < 10000), for CREATE AN ENTRANCE's MODS
// rows (cae_mod_motions.cpp): label = the superstar mod's name, else the script's own name and the
// game it was converted from; parts: bit k set when CAE part 10000 + 1000k + n is in the pack too.
struct ModEntrance {
  int number = 0;
  std::string label;
  uint8_t parts = 0;
};
std::vector<ModEntrance> ModEntrances();

// Is there an entrance script `n` (an entrance or a CAE part) - the game's own (pac\evt) or a pack's?
bool EntranceScriptExists(int n);

}  // namespace svr2011
