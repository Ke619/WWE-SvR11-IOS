// WWE SmackDown vs. Raw 2011 - lobby chat longer than the game's.
//
// The game's lobby chat message holds 64 bytes of text (sub_828A7A28: kind
// 4096, to each player) and its keyboard 21 characters. Here a message is up
// to kChatMax characters: the whole of it goes to the lobby over P2P (p2p.h,
// its own message kind) and is word-wrapped into the chat box's lines; the
// game's own message still goes, with the start (older games show that). A
// game that sends whole ones says so (an empty message every 2 s in a lobby):
// its game messages are then left out here, its whole ones shown instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/ppc.h>

namespace svr2011 {

constexpr size_t kChatMax = 300;  // characters a message (the rest is cut)

void InstallOnlineChat();

// This player's message, whole: sent to the lobby (at most one a 1.5 s:
// nullopt, too soon - it isn't sent). Returns the start for the game's own
// message (63 bytes at most, whole characters).
std::optional<std::string> OnlineChatSend(const std::string& whole);

// A chat message from the game's network (sub_828A7C18) of this sender: left
// out (the sender's whole one comes over P2P)?
bool OnlineChatFromPort(uint64_t sender);

// The chat box's lines for a message ("name : text", wrapped).
std::vector<std::string> OnlineChatLines(const std::string& name, const std::string& text);

// The lobby's frame: messages that came, into the chat box (*0x82EDE92C).
void OnlineChatFrame(PPCContext& ctx, uint8_t* base);

// Lines to show in the chat box from the lobby's frame (this player's own).
void OnlineChatShow(const std::string& name, const std::string& text);

// The lobby chat's keyboard field (21 characters: the game's) scrolls - the
// start of a longer message is kept here, the field shows its end. Before a
// character goes in (keyboard_typing.cpp's sub_82730110 hook): true, leave it
// out (the message is kChatMax already).
bool OnlineChatBeforeInsert(PPCContext& ctx, uint8_t* base, uint32_t field);

// This player's own message as the chat box shows it, once (sub_828A7C18 on
// this game: the whole text instead of the field's end).
std::optional<std::string> OnlineChatTakeOwn();

// An online match's setup as this game placed it (match_types.cpp, once at its start: "rule 2B, arena 4,
// 0:58 1:57 ..." - person number : superstar id, DLC ids too). Sent to the lobby's players over the chat's
// channel; when another game's differs, both are logged and a notice shows (report 6dde2f: two friends each saw
// the other idle - lockstep ran, the games didn't play the same match). The match goes on: a diagnostic only.
void OnlineMatchSetup(const std::string& setup);

// The match's step check (net_stats.cpp, from the lockstep send, each time the match clock reaches a multiple of
// 300): a hash of every wrestler's current motion and exact positions, and a short summary of them. Sent to the
// lobby's players over the chat's channel; when another game's hash for the same clock differs, the first
// difference is logged with both summaries and a notice shows - the games no longer play the same match
// (report 6dde2f). The match goes on: a diagnostic only.
void OnlineMatchStep(uint32_t clock, uint64_t hash, const std::string& summary);

}  // namespace svr2011
