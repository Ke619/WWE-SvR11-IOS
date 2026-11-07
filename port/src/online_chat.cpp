// WWE SmackDown vs. Raw 2011 - lobby chat longer than the game's (see
// online_chat.h).
#include "online_chat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/svr2011_init.h"
#include "online.h"
#include "online_overlay.h"
#include "online_rules.h"
#include "p2p.h"

namespace svr2011 {

namespace {

constexpr uint32_t kTag = 0x43485431;   // "CHT1"
constexpr uint32_t kSetupTag = 0x4D535431;  // "MST1": a match's setup (OnlineMatchSetup)
constexpr uint32_t kChatPtr = 0x82EDE92C;  // -> the lobby's chat box (sub_828A7B38(chat, line))
constexpr size_t kLineChars = 40;          // a chat box line, wrapped (characters)
constexpr size_t kMaxBytes = kChatMax * 4;  // (UTF-8)

using Clock = std::chrono::steady_clock;

uint64_t MyXuid() { return std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16); }
std::string MyName() { return rex::cvar::Query<std::string>("online_name"); }

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void Put64(std::vector<uint8_t>& b, uint64_t v) { Put32(b, uint32_t(v >> 32)), Put32(b, uint32_t(v)); }

// UTF-8: the byte length of n whole characters at most (and of `max_bytes` at most).
size_t Prefix(const std::string& s, size_t chars, size_t max_bytes = SIZE_MAX) {
  size_t i = 0, n = 0;
  while (i < s.size() && n < chars) {
    const uint8_t c = uint8_t(s[i]);
    const size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + len > max_bytes || i + len > s.size()) break;
    i += len, ++n;
  }
  return i;
}

size_t Chars(const std::string& s) {
  size_t n = 0;
  for (const char c : s) n += (uint8_t(c) & 0xC0) != 0x80;
  return n;
}

// A text as a message holds it: no control characters, kChatMax at most.
std::string Clean(const std::string& in) {
  std::string s;
  for (const char c : in) s += (uint8_t(c) < 0x20 || c == 0x7F) ? ' ' : c;
  return s.substr(0, Prefix(s, kChatMax, kMaxBytes));
}

std::vector<uint8_t> Payload(uint32_t id, const std::string& text) {
  std::vector<uint8_t> b;
  Put32(b, kTag), Put64(b, MyXuid()), Put32(b, id);
  const std::string name = MyName().substr(0, 32);
  b.push_back(uint8_t(name.size()));
  b.insert(b.end(), name.begin(), name.end());
  b.push_back(uint8_t(text.size() >> 8)), b.push_back(uint8_t(text.size()));
  b.insert(b.end(), text.begin(), text.end());
  return b;
}

struct Peer {
  Clock::time_point said{};       // its last message (a hello too)
  std::deque<uint32_t> ids;       // the last ids shown (a message comes twice)
  std::deque<Clock::time_point> recent;  // when its last messages came (the rate)
};
std::mutex g_mutex;
std::map<uint64_t, Peer> g_peers;  // by online id
struct Line {
  std::string name, text;
};
std::deque<Line> g_inbox;  // to show (the lobby's frame)
uint32_t g_next_id = 1;
Clock::time_point g_last_sent{};

// The chat keyboard's field: the start of the message scrolled out of it
// (this player's), and the whole one just sent (its own line in the box).
std::string g_prefix;
std::optional<std::string> g_own;
constexpr size_t kFieldChars = 21, kScroll = 10;

void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// Is this the lobby chat's field (its settings at +44: 64 bytes, 21 a line,
// one line; the lobby's screen just under it)?
bool ChatField(uint8_t* base, uint32_t field) {
  if (!field || !OnlineLobbySeenWithin(60000)) return false;
  const uint32_t cfg = Be32(base + field + 44);
  if (cfg < 0x40000000 || cfg >= 0xC0000000) return false;
  return Be32(base + cfg + 40) == 64 && Be32(base + cfg + 44) == kFieldChars && Be32(base + cfg + 52) == 1 &&
         Be32(base + field + 2956) == 0;
}

// The field's text (line 0: field + 2944 -> UTF-8) and cursor (+2616, characters).
char* FieldText(uint8_t* base, uint32_t field) {
  const uint32_t t = Be32(base + field + 2944);
  return t ? reinterpret_cast<char*>(base + t) : nullptr;
}

// The cursor set and the field shown again (as the game's LEFT does).
void SetCursor(PPCContext& ctx, uint8_t* base, uint32_t field, uint32_t cursor) {
  PPCContext c = ctx;
  c.r3.u64 = field, c.r4.u64 = cursor, c.r5.u64 = 0;
  sub_8272E568(c, base);
}

// -- the match-start setup check (OnlineMatchSetup) --
struct Setup {
  uint64_t session = 0;
  std::string text;
  Clock::time_point at{};
};
std::mutex g_setup_mutex;
Setup g_my_setup;
std::map<uint64_t, Setup> g_their_setup;  // by online id
std::map<uint64_t, std::string> g_setup_told;  // (each pair once: theirs at the last comparison)

// Mine against theirs, when both are this session's and from one match (90 s apart at most). Under the lock.
void CompareSetups(uint64_t from) {
  const auto it = g_their_setup.find(from);
  if (it == g_their_setup.end() || g_my_setup.text.empty() || it->second.session != g_my_setup.session) return;
  const auto gap = it->second.at > g_my_setup.at ? it->second.at - g_my_setup.at : g_my_setup.at - it->second.at;
  if (gap > std::chrono::seconds(90)) return;
  const std::string key = g_my_setup.text + "|" + it->second.text;
  if (g_setup_told[from] == key) return;
  g_setup_told[from] = key;
  if (it->second.text == g_my_setup.text) {
    REXLOG_INFO("online: match setup the same as player {:016X}'s", from);
    return;
  }
  REXLOG_WARN("online: match setup differs from player {:016X}'s - this game: {}; theirs: {}", from, g_my_setup.text,
              it->second.text);
  OnlineOverlayNotice("THE TWO GAMES HAVE DIFFERENT MATCH SETUPS",
                      "THIS MATCH MAY NOT PLAY THE SAME ON BOTH - SEND A REPORT AFTER IT", 20);
}

void OnSetup(const uint8_t* p, size_t n) {
  if (n < 22) return;
  const uint64_t from = uint64_t(Be32(p + 4)) << 32 | Be32(p + 8);
  const uint64_t session = uint64_t(Be32(p + 12)) << 32 | Be32(p + 16);
  const size_t len = size_t(p[20]) << 8 | p[21];
  if (22 + len > n || len > 600 || !from || from == MyXuid()) return;
  const P2PLobby lobby = P2PMatchLobby();
  if (!lobby.in || session != lobby.session ||
      std::find(lobby.members.begin(), lobby.members.end(), from) == lobby.members.end())
    return;
  std::lock_guard lock(g_setup_mutex);
  g_their_setup[from] = {session, std::string(reinterpret_cast<const char*>(p + 22), len), Clock::now()};
  CompareSetups(from);
}

void OnChat(const uint8_t* p, size_t n) {
  if (n >= 4 && Be32(p) == kSetupTag) return OnSetup(p, n);
  if (n < 19 || Be32(p) != kTag) return;
  const uint64_t from = uint64_t(Be32(p + 4)) << 32 | Be32(p + 8);
  const uint32_t id = Be32(p + 12);
  size_t at = 16;
  const size_t nl = p[at++];
  if (at + nl + 2 > n) return;
  const std::string name(reinterpret_cast<const char*>(p + at), nl);
  at += nl;
  const size_t tl = size_t(p[at]) << 8 | p[at + 1];
  at += 2;
  if (at + tl > n || tl > kMaxBytes || !from || from == MyXuid()) return;
  const std::string text = Clean(std::string(reinterpret_cast<const char*>(p + at), tl));
  // (only the lobby's players)
  const P2PLobby lobby = P2PMatchLobby();
  if (std::find(lobby.members.begin(), lobby.members.end(), from) == lobby.members.end()) return;
  std::lock_guard lock(g_mutex);
  Peer& peer = g_peers[from];
  const auto now = Clock::now();
  peer.said = now;
  if (!id || text.empty()) return;  // (a hello)
  if (std::find(peer.ids.begin(), peer.ids.end(), id) != peer.ids.end()) return;
  peer.ids.push_back(id);
  if (peer.ids.size() > 16) peer.ids.pop_front();
  while (!peer.recent.empty() && now - peer.recent.front() > std::chrono::seconds(10)) peer.recent.pop_front();
  if (peer.recent.size() >= 5) {
    REXLOG_INFO("online chat: {} sends too fast - a message left out", name);
    return;
  }
  peer.recent.push_back(now);
  g_inbox.push_back({name, text});
}

void Hellos() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    uint32_t state = 0, cursor = 0, lobby = 0;
    uint8_t* base = nullptr;
    if (OnlineLobbyView(&state, &cursor, &base, &lobby)) P2PSendChat(Payload(0, ""));
  }
}

}  // namespace

void InstallOnlineChat() {
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  SetP2PChatHandler(OnChat);
  std::thread(Hellos).detach();
}

std::optional<std::string> OnlineChatSend(const std::string& whole) {
  const std::string text = Clean(whole);
  if (text.empty()) return std::string();
  uint32_t id;
  {
    std::lock_guard lock(g_mutex);
    const auto now = Clock::now();
    if (now - g_last_sent < std::chrono::milliseconds(1500)) return std::nullopt;
    g_last_sent = now;
    id = g_next_id++;
  }
  const auto payload = Payload(id, text);
  P2PSendChat(payload);
  P2PSendChat(payload);  // (twice: one lost, the other shows; the same id isn't shown again)
  REXLOG_INFO("online chat: sent {} characters", Chars(text));
  return text.substr(0, Prefix(text, SIZE_MAX, 63));
}

void OnlineMatchSetup(const std::string& setup) {
  const P2PLobby lobby = P2PMatchLobby();
  if (!lobby.in || !rex::cvar::Query<bool>("online_enabled")) return;
  std::string text = setup.substr(0, 600);
  // (test aid: SVR2011_TEST_SETUP_SKEW=1 - this game's setup told as another one: the difference's notice)
  if (const char* v = std::getenv("SVR2011_TEST_SETUP_SKEW"); v && *v == '1') text += " (test)";
  {
    std::lock_guard lock(g_setup_mutex);
    g_my_setup = {lobby.session, text, Clock::now()};
    for (const auto& [from, theirs] : g_their_setup) CompareSetups(from);
  }
  std::vector<uint8_t> b;
  Put32(b, kSetupTag), Put64(b, MyXuid()), Put64(b, lobby.session);
  b.push_back(uint8_t(text.size() >> 8)), b.push_back(uint8_t(text.size()));
  b.insert(b.end(), text.begin(), text.end());
  REXLOG_INFO("online: match setup sent - {}", text);
  // (now and twice more: a packet can be lost, and the other game may still be loading)
  std::thread([b] {
    for (int i = 0; i < 3; ++i) {
      P2PSendChat(b);
      std::this_thread::sleep_for(std::chrono::seconds(4));
    }
  }).detach();
}

bool OnlineChatFromPort(uint64_t sender) {
  std::lock_guard lock(g_mutex);
  const auto it = g_peers.find(sender);
  return it != g_peers.end() && Clock::now() - it->second.said < std::chrono::seconds(10);
}

std::vector<std::string> OnlineChatLines(const std::string& name, const std::string& text) {
  std::vector<std::string> lines;
  std::string rest = text;
  std::string head = name.empty() ? "" : name + " : ";
  while (!rest.empty()) {
    const size_t room = kLineChars > Chars(head) + 4 ? kLineChars - Chars(head) : 4;
    size_t cut = Prefix(rest, room);
    if (cut < rest.size()) {  // (at a space when there's one)
      const size_t space = rest.rfind(' ', cut);
      if (space != std::string::npos && space > 0) cut = space;
    }
    std::string piece = rest.substr(0, cut);
    rest = rest.substr(cut);
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    lines.push_back(head + piece);
    head = "   ";
  }
  return lines;
}

void OnlineChatShow(const std::string& name, const std::string& text) {
  std::lock_guard lock(g_mutex);
  g_inbox.push_back({name, text});
}

void OnlineChatFrame(PPCContext& ctx, uint8_t* base) {
  std::deque<Line> lines;
  {
    std::lock_guard lock(g_mutex);
    lines.swap(g_inbox);
  }
  if (lines.empty()) return;
  const uint32_t chat = Be32(base + kChatPtr);
  const uint32_t tmp = OnlineScratch();
  if (!chat || !tmp) return;
  for (const Line& l : lines)
    for (const std::string& s : OnlineChatLines(l.name, l.text)) {
      const size_t n = std::min<size_t>(s.size(), 90);
      std::memcpy(base + tmp + 3072, s.data(), n);
      base[tmp + 3072 + n] = 0;
      PPCContext c = ctx;
      c.r3.u64 = chat, c.r4.u64 = tmp + 3072;
      sub_828A7B38(c, base);
    }
}

}  // namespace svr2011

namespace svr2011 {

bool OnlineChatBeforeInsert(PPCContext& ctx, uint8_t* base, uint32_t field) {
  if (!ChatField(base, field)) return false;
  char* text = FieldText(base, field);
  if (!text) return false;
  std::string now(text, strnlen(text, 64));
  const size_t chars = Chars(now), cursor = Be32(base + field + 2616);
  const size_t cut = Prefix(now, kScroll);
  {
    std::lock_guard lock(g_mutex);
    if (Chars(g_prefix) + chars >= kChatMax) return true;  // (the message is full)
    if (chars < kFieldChars || cursor != chars) return false;  // (room, or editing inside: the game's way)
    // full, at its end: its first characters to the kept start, the rest moved up
    g_prefix += now.substr(0, cut);
  }
  const std::string rest = now.substr(cut);
  std::memcpy(text, rest.c_str(), rest.size() + 1);
  SetCursor(ctx, base, field, uint32_t(Chars(rest)));
  return false;
}

std::optional<std::string> OnlineChatTakeOwn() {
  std::lock_guard lock(g_mutex);
  auto own = std::move(g_own);
  g_own.reset();
  return own;
}

}  // namespace svr2011

// The field's cursor set (sub_8272E568(field, cursor, ?)): from 0x82731298 the
// field opened (or was cleared) - the kept start goes. It also sets the
// keyboard's "LEFT n" (the characters left: *(field + 1304) + 0x1ABF0, its
// "changed" + 0x1ABEC - by the layout, +96 of the settings: 1, 2 or 3 lines
// shown): the chat's is what's left of the whole message, not of the field.
REX_EXTERN(__imp__sub_8272E568);
REX_HOOK_RAW(sub_8272E568) {
  using namespace svr2011;
  const uint32_t field = ctx.r3.u32;
  if (uint32_t(ctx.lr) == 0x82731298 && ctx.r4.u32 == 0) {
    std::lock_guard lock(g_mutex);
    g_prefix.clear();
  }
  __imp__sub_8272E568(ctx, base);
  if (!ChatField(base, field)) return;
  const char* text = FieldText(base, field);
  const uint32_t ui = Be32(base + field + 1304), layout = Be32(base + Be32(base + field + 44) + 96);
  if (!text || !ui || layout < 1 || layout > 3) return;
  size_t used;
  {
    std::lock_guard lock(g_mutex);
    used = Chars(g_prefix) + Chars(std::string(text, strnlen(text, 64)));
  }
  static constexpr uint32_t kLeft[] = {0x1ABF0, 0x1B9F0, 0x1C8F0};  // (44016, 47600, 51440)
  const uint32_t left = uint32_t(used < kChatMax ? kChatMax - used : 0);
  Wr32(base + ui + kLeft[layout - 1], left);
  Wr32(base + ui + kLeft[layout - 1] - 4, 1);
}

// Backspace (sub_8272EDF0(field, ...)): the field near empty, the kept start
// comes back into it.
REX_EXTERN(__imp__sub_8272EDF0);
REX_HOOK_RAW(sub_8272EDF0) {
  const uint32_t field = ctx.r3.u32;
  PPCContext saved = ctx;
  __imp__sub_8272EDF0(ctx, base);
  using namespace svr2011;
  if (!ChatField(base, field)) return;
  char* text = FieldText(base, field);
  if (!text) return;
  std::string now(text, strnlen(text, 64));
  std::string back;
  {
    std::lock_guard lock(g_mutex);
    if (g_prefix.empty() || Chars(now) > kFieldChars - kScroll) return;
    // (the last characters of the kept start, as many as fit: 63 bytes a field)
    size_t take = Chars(g_prefix) < kScroll ? Chars(g_prefix) : kScroll;
    size_t at = 0;
    while (take > 0) {
      at = Prefix(g_prefix, Chars(g_prefix) - take);
      if (g_prefix.size() - at + now.size() <= 63) break;
      --take;
    }
    if (!take) return;
    back = g_prefix.substr(at);
    g_prefix.resize(at);
  }
  const uint32_t cursor = Be32(base + field + 2616);
  const std::string joined = back + now;
  std::memcpy(text, joined.c_str(), joined.size() + 1);
  SetCursor(saved, base, field, uint32_t(cursor + Chars(back)));
}

// A lobby chat message sent (sub_828A7A28(chat, text) - the field's text, to
// each player as 64 bytes): the whole one (the kept start and the field's) to
// the lobby over P2P, the game's with its start. Sent too soon (a 1.5 s
// rate): not sent, a note in the chat box instead.
REX_EXTERN(__imp__sub_828A7A28);
REX_HOOK_RAW(sub_828A7A28) {
  using namespace svr2011;
  const uint32_t text = ctx.r4.u32;
  if (!text || !OnlineLobbySeenWithin(60000)) {
    __imp__sub_828A7A28(ctx, base);
    return;
  }
  char* t = reinterpret_cast<char*>(base + text);
  std::string whole;
  {
    std::lock_guard lock(g_mutex);
    whole = g_prefix + std::string(t, strnlen(t, 64));
    g_prefix.clear();
  }
  const auto start = OnlineChatSend(whole);
  if (!start) {
    REXLOG_INFO("online chat: a message too soon after the last - not sent");
    OnlineChatShow("", "(One message every 1.5 seconds - that one wasn't sent.)");
    ctx.r3.u64 = 0;
    return;
  }
  if (start->size() <= 63 && *start != std::string(t, strnlen(t, 64))) std::memcpy(t, start->c_str(), start->size() + 1);
  {
    std::lock_guard lock(g_mutex);
    g_own = whole;
  }
  // (test aid SVR2011_TEST_CHAT_TWICE: every message is sent again at once - too soon, the note)
  static const bool twice = std::getenv("SVR2011_TEST_CHAT_TWICE") != nullptr;
  PPCContext again = ctx;
  __imp__sub_828A7A28(ctx, base);
  static bool in_again = false;
  if (twice && !in_again) {
    in_again = true;
    sub_828A7A28(again, base);
    in_again = false;
  }
}
