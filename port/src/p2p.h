// WWE SmackDown vs. Raw 2011 - peer-to-peer online matches (Player Match /
// Ranked Match lobbies and play) with no server: games find each other on
// the network themselves.
//
// The game uses Xbox LIVE's session API (XSessionCreate / Search / Join ...,
// app 0xFB) and XNet (its own address, QoS probes, peers' addresses) over
// plain UDP sockets. Here (through the SDK's online_hooks.h):
//   - each game has an address of its own: its LAN IP, its public IP when
//     known, its port base (p2p_port) and a hardware ID from the player's
//     online id;
//   - each peer gets a private address 10.64.x.y the game uses in its socket
//     calls, mapped to the peer's real IP and ports (its own port base: two
//     games on one PC are two peers);
//   - a host advertises its sessions on its port base (UDP); a search asks
//     the LAN (broadcast) and the addresses in p2p_peers, a QoS probe asks the
//     host directly. A joining game says hello first, so the host knows it.
// The online server (online_server) is never needed to play: it only keeps
// the leaderboards.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallP2P(rex::memory::Memory* memory);

// Files games fetch from each other (online_cas.cpp: the Created Superstars'
// Paint Tool data and extra logos): a file is a kind and a key. The source
// gives this game's copy (nullopt: it hasn't); P2PFetch asks the peers (the
// games this one knows) and returns the first that has it, whole.
using P2PFileSource = std::optional<std::string> (*)(uint8_t kind, const std::string& key);
void SetP2PFileSource(P2PFileSource source);
std::optional<std::string> P2PFetch(uint8_t kind, const std::string& key, std::chrono::milliseconds timeout);

// Invites (online_overlay.h): the match session this game is in (hosts, or
// else joined) as an XSESSION_INFO (60 bytes: id, host XNADDR, key) and its
// slots; false when there's none.
bool P2PSessionInfo(uint8_t* out, uint32_t* slots = nullptr);
// The match session this game is in: 0 none, 1 it hosts it, 2 it joined it (the presence poll's place).
int P2PSessionRole();
// An invited game, before the game joins: reaches the session's host (directly
// or through the relay) so its packets get through; true if it still has it.
bool P2PReachHost(const uint8_t* info);
// An invite accepted: for 15 minutes the game's session searches list that
// session first, with its private slots open to this game.
void P2PExpectInvite(const uint8_t* info);
// Why the last session search (or invite) left hosts out - another version or
// other gameplay mods (online_files.h) - and how many sessions ("" when none,
// or over 2 minutes ago): for the game's own "no sessions" / join messages.
std::string P2PHiddenReason(int* sessions = nullptr);

// The session's host left during a match in the last minute (its game stopped
// answering): the game's "An error occurred during the match" says so (online.cpp).
bool P2PHostLeft();

// Online mode: the player in the ONLINE menus (the game's presence session) or
// an online session / match - or was, in the last 30 s. Presence, the relay's
// stream: only then (the offline menus show nobody online).
bool P2POnlineMode();
// A port session property (an int32: XUSER_PROPERTY type 1, e.g. 0x1000F001
// the port's match mode): the host sets it before the game creates its
// session (it goes into every session it hosts from then on, as
// XUserSetProperty's); the match session this game hosts or joined has it
// (nullopt: none).
void P2PSetHostProperty(uint32_t id, uint32_t value);
std::optional<uint32_t> P2PSessionProperty(uint32_t id);
// The port's match rules for online Player Matches (match_types.cpp: mode
// << 16 | set): the host sets them before CREATE SESSION (0: none); the
// current match session's (hosted or joined) - nullopt: none. Ranked
// sessions never carry them.
void SetPortRules(uint32_t value);
std::optional<uint32_t> PortRules();
// The server's match network (its match_network setting, from the presence poll): "auto",
// "relay" (every peer through the relay) or "p2p" (match traffic direct only). A session this game
// hosts from then on carries it (property 0x1000F004); joiners follow the host's.
void P2PSetServerNetwork(const std::string& mode);
// The server's netcode (its netcode setting, from the presence poll): "lockstep", "lockstep_vclock" or
// "rollback" (unknown: lockstep). A match session this game hosts from then on carries it (property 0x1000F005);
// joiners follow the host's (a host without it: lockstep).
void P2PSetServerNetcode(const std::string& mode);
// This match session's netcode: 0 lockstep, 1 rollback, 2 lockstep_vclock; 0 outside a match session.
int P2PNetcode();
// The match kind the game set for its online sessions (XUserSetContext
// X_CONTEXT_GAME_TYPE, set as the player enters RANKED MATCH / PLAYER MATCH,
// before CREATE SESSION): true Ranked, false Player Match; nullopt: not set yet.
std::optional<bool> P2PRanked();

// The match session this game is in (hosted, or else joined): its id and
// members (online ids, this game's too); in = false when there's none.
struct P2PLobby {
  bool in = false, host = false;
  uint64_t session = 0;
  std::vector<uint64_t> members;
};
P2PLobby P2PMatchLobby();
// The match session's way to the far-off players, decided in its lobby (the relay warning,
// online_overlay.cpp): kUnknown until then, in a match, or with no session.
struct P2PRelayPathInfo {
  enum Kind : uint8_t { kUnknown, kDirect, kRelayed, kForced } kind = kUnknown;  // (kRelayed: direct failed)
  uint64_t session = 0;
  bool forced_by_server = false;  // (kForced: the server's match network; else p2p_force_relay)
  int side = 0;                   // whose router: 0 can't tell, 1 this player's (symmetric), 2 the other's (open here)
  uint16_t port = 0;              // this game's UDP port (p2p_port, or the next free one)
};
P2PRelayPathInfo P2PRelayPath();
// The lobby's superstar mods (online_mods.cpp): a message to every peer this
// game knows, and the handler of those that come (on the network thread).
using P2PModHandler = void (*)(const uint8_t* data, size_t size);
void SetP2PModHandler(P2PModHandler handler);
void P2PSendMods(const std::vector<uint8_t>& payload);

// Mod files (mod_transfer.cpp): kModXfer messages to one peer (a peer = its MAC as a number), over a DIRECT
// link only - false when there's none (the relay never carries files: the user's rule). The handler gets the
// sender. The session this game joined: its host (0: none) and id; or the one it hosts.
using P2PModXferHandler = void (*)(uint64_t peer, const uint8_t* data, size_t size);
void SetP2PModXferHandler(P2PModXferHandler handler);
bool P2PSendModXfer(uint64_t peer, const std::vector<uint8_t>& payload);
bool P2PPeerDirect(uint64_t peer);
uint64_t P2PSessionHost(uint64_t* session = nullptr);
bool P2PHosting(uint64_t* session = nullptr);
// Lobby chat longer than the game's 64 bytes (online_chat.cpp): the same, a
// message kind of its own.
using P2PChatHandler = void (*)(const uint8_t* data, size_t size);
void SetP2PChatHandler(P2PChatHandler handler);
void P2PSendChat(const std::vector<uint8_t>& payload);

}  // namespace svr2011
