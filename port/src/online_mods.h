// WWE SmackDown vs. Raw 2011 - superstar and arena mods in online lobbies.
//
// A session is no longer hidden for superstar or arena mods (only the other
// gameplay mods - move packs, backstage, match types - must match:
// online_files.h). In the lobby each game tells the others which superstar
// and arena mods it has, by key (folder, version, CRC of the gameplay files:
// a different version is a different mod):
//   - a superstar mod can be picked when every player in the lobby has it
//     (the host refuses the pick otherwise; the picker is told who lacks
//     what); a player who joins later without a superstar picked holds the
//     match back (START waits) until that pick changes or the player leaves;
//   - the host's custom arena plays on every game that has it; a player
//     without it holds the match back the same way;
//   - a mod superstar's id is each PC's own (slots.txt): ids are translated
//     where the lobby's records, picks and superstar blocks arrive;
//   - the lobby's CHAT says what each player lacks;
//   - weapon mods (weapon_mods.h) by their own keys: the host's mod weapons
//     that every player has play in its Extreme Rules matches.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct PPCContext;

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallOnlineMods(rex::memory::Memory* memory);

// The host's lobby, a pick arriving (online_cpu.cpp's sub_828AE250 hook,
// before the game handles it: msg +0 the sender's net player, +8 the pick, +0
// its superstar): its id in this game's numbering; a superstar someone lacks
// is refused (the game's own "taken"). Done() after the game's handler.
void OnlineModsPick(PPCContext& ctx, uint8_t* base, uint32_t msg);
void OnlineModsPickDone();
// The host's lobby, a superstar block arriving (sub_828AE378, msg +0 the
// sender's net player): the next block stored (sub_828A6078's data) is in
// the sender's numbering.
void OnlineModsBlockFrom(PPCContext& ctx, uint8_t* base, uint32_t msg);
void OnlineModsBlockStore(uint8_t* base, uint32_t data);
// The lobby's frame (online_cpu.cpp's sub_824E6510 hook).
void OnlineModsFrame(PPCContext& ctx, uint8_t* base);
// The lobby's start checks (sub_828AC668): false while a player lacks a
// picked superstar or the session's arena.
bool OnlineModsMayStart(uint8_t* base, uint32_t L);
// A superstar (this game's id) every player in the lobby has - a stock one,
// or a mod all have: for the host's own picks (CPU slots, Mystery Opponent).
bool OnlineModEveryoneHas(uint32_t id);
// A match's move in an online session: false when it's from a move pack not
// every player of the lobby has (and the stock game can't play it) - then it
// plays its category's default, the same on every game (cam_mod_moves.cpp).
bool OnlinePackMoveAllowed(uint16_t id);
// The host's mod weapons for this lobby's Extreme Rules matches, those every player has (weapon_mods.h; the host
// decides from its WEAPONS page, the others echo it before START): {this game's weapon id, count}.
std::vector<std::pair<int, int>> OnlineModWeapons();
// (a client) What the lobby's tables say this game lacks of the host's superstar mods and the session's arena:
// their labels (mod_transfer.h's offer when the link goes through the relay and nothing can come).
std::vector<std::string> OnlineModsLackedFromHost();
// (a client) The session's host's name, as its mod table says ("" none yet).
std::string OnlineModsHostName();

}  // namespace svr2011
