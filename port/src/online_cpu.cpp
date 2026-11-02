// WWE SmackDown vs. Raw 2011 - CPU players in online Player Matches.
//
// Netplay is lockstep with only the humans' pads exchanged; CPU characters
// are left out of the exchange and the AI runs the same on every machine
// (its random numbers are seeded the same online). So a CPU needs only the
// SETUP to be the same everywhere. docs/ONLINE_CPU.md.
//
// The Player Match lobby ("OMSL": constructor sub_824E41F8, per frame
// sub_824E6510, state at screen+1168, the cursor's slot at screen+1060) keeps
// its slots in L = **0x82EDE924: 280-byte records at L+204+j*280 (+0 state,
// +4 net player, +12 name, +136 superstar id, +140 attire, +160.. 120 bytes
// the game unpacks for the panel: +172 the id, +174 / +242 the superstar's
// name), L+24 joined, L+28 slots. Clients copy a record whole when the host
// sends it (sub_828AC700(L, j, 0)).
//
// Here, the host presses Y on a VACANT slot (Y's own action - sub_824E4C30 -
// does nothing on the host): the record becomes a CPU - state 26 (past every
// stage, nobody can claim it), net player 0xFFFFFFFE (the tag), name "CPU",
// a superstar nobody else picked - and goes to every client. Y on it again:
// vacant again. Then on every machine:
//  - the start checks (sub_828AC668: joined == slots and every slot at the
//    stage) count the CPU slots as joined;
//  - the host's wait for every superstar block to be acknowledged
//    (sub_828AD2F8) counts the CPU slots' bits as set;
//  - the match slot made from the CPU's lobby slot (sub_824F6AE0(obj,
//    match slot, lobby slot)) gets controller CPU (slot[-8] = 1).
//
// Test aid: SVR2011_TEST_LOBBY_LOG=1 logs the records, picks and match slots.

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "online.h"
#include "online_cas.h"
#include "online_chat.h"
#include "online_mods.h"
#include "online_overlay.h"
#include "online_rules.h"
#include "util.h"

REXCVAR_DEFINE_BOOL(online_cpu_slots, true, "Online",
                    "CPU players in online Player Matches: the host's Y on a vacant lobby slot");

namespace {

constexpr uint32_t kLobbyPtr = 0x82EDE924;  // -> -> L
constexpr uint32_t kSlots = 204, kSlotSize = 280;
constexpr uint32_t kBlocks = 3568, kBlockSize = 15044;  // the slots' superstar blocks (+3564: acknowledged mask)
constexpr uint32_t kCpuState = 26, kCpuTag = 0xFFFFFFFE;
constexpr uint32_t kMatch = 0x82EDEB48;  // -> the match setup: slots at +432, 2116 bytes each
constexpr uint32_t kMatchSlots = 432, kMatchSlotSize = 2116;

// Superstars a CPU can be (the stock roster's: every game has them).
struct Star {
  uint16_t id;
  const char* name;
};
constexpr Star kStars[] = {
    {102, "TRIPLE H"}, {103, "UNDERTAKER"}, {104, "CHRIS JERICHO"}, {107, "KANE"},     {110, "EDGE"},
    {123, "REY MYSTERIO"}, {125, "BIG SHOW"}, {131, "CM PUNK"},       {139, "JOHN CENA"}, {145, "SHAWN MICHAELS"},
    {160, "BATISTA"}, {161, "RANDY ORTON"},
};

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

bool LogOn() {
  static const bool on = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  return on;
}

uint32_t Lobby(uint8_t* base) {
  const uint32_t p = Rd32(base + kLobbyPtr);
  return p ? Rd32(base + p) : 0;
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15], s += (i % 4 == 3 ? " " : "");
  return s;
}

uint8_t* Record(uint8_t* base, uint32_t L, uint32_t j) { return base + L + kSlots + j * kSlotSize; }
bool IsCpu(const uint8_t* rec) { return Rd32(rec + 4) == kCpuTag && Rd32(rec) == kCpuState; }

uint32_t SlotCount(uint8_t* base, uint32_t L) { return std::min<uint32_t>(Rd32(base + L + 28), 6); }

int CpuCount(uint8_t* base, uint32_t L) {
  int n = 0;
  for (uint32_t j = 0; j < SlotCount(base, L); ++j) n += IsCpu(Record(base, L, j));
  return n;
}

bool IsHost(PPCContext& ctx, uint8_t* base) {
  PPCContext c = ctx;
  sub_82573F18(c, base);
  return c.r3.u32 && Rd32(base + c.r3.u32 + 20) != 0;
}

uint8_t g_vacant[6][kSlotSize];  // a slot's record before it became a CPU (Y again: back to it)

// The host choosing a CPU's superstar with the lobby's own list (A on the CPU
// slot): the list runs as for the host's own slot (OMSL state 5 opens it;
// the claim message is skipped, so the host's slot stays as it was), and its
// two messages - the pick (sub_828AE250) and the superstar block
// (sub_828AE378), which the host handles itself - land on the CPU slot:
// sub_828A5F08 (the sender's slot) answers the CPU's while they run.
int g_edit = -1;             // the CPU slot being chosen for (-1 none)
bool g_edit_open = false;    // (the list has opened: OMSL state 6 seen)
bool g_edit_msg = false;     // (one of the two handlers is running for it)
uint8_t g_panel_save[204];   // the host's own panel 0 (the list writes it)
constexpr uint32_t kSelect = 0x82E3DD98;  // -> S: +0 state (0 / 22 done), +4 result (1 back, 2 taken, 3 done, 4/5 failed)
constexpr uint32_t kPanels = 0x82EDE630;  // -> P: panel 0 = P + u32(P+13520) * 1352 + 120 (204 bytes)

uint32_t Panel0(uint8_t* base) {
  const uint32_t P = Rd32(base + kPanels);
  return P ? P + Rd32(base + P + 13520) * 1352 + 120 : 0;
}

// The lobby record j changed: send it to everyone (and the panel into the
// superstar block, unless the block was built and the panel made from it).
void SendRecord(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, bool panel_to_block = true) {
  PPCContext c = ctx;
  if (panel_to_block) {
    c.r3.u64 = L + kSlots + j * kSlotSize + 160, c.r4.u64 = L + kBlocks + j * kBlockSize;
    sub_828A5408(c, base);
    c = ctx;
  }
  c.r3.u64 = L, c.r4.u64 = j, c.r5.u64 = 0;
  sub_828AC700(c, base);
}

// The CPU's superstar block, built as the select screen builds a human's
// (sub_824C4F58) from the stock superstar's data (as an offline CPU's,
// sub_828B5BC0): online, every machine loads each person from the lobby's
// block. Then the panel is made from it (real stats). False: no scratch memory.
bool BuildBlock(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, uint32_t id) {
  const uint32_t tmp = svr2011::OnlineScratch();
  if (!tmp) return false;
  const uint32_t blk = L + kBlocks + j * kBlockSize, args = tmp + 1952 + 32;
  std::memset(base + blk, 0, kBlockSize - 4);
  PPCContext c = ctx;
  c.r3.u64 = blk;
  sub_824C4F08(c, base);
  std::memset(base + tmp, 0, 1952);
  Wr32(base + args, id), Wr32(base + args + 4, 1), Wr32(base + args + 8, 0);
  c = ctx;
  c.r3.u64 = tmp, c.r4.u64 = args;
  sub_828B5BC0(c, base);
  std::memcpy(base + blk, base + tmp, 1724);
  std::memcpy(base + blk + 14816, base + tmp + 1728, 224);
  c = ctx;
  c.r3.u64 = L + kSlots + j * kSlotSize + 160, c.r4.u64 = blk;
  sub_828A5380(c, base);
  if (LogOn()) REXLOG_INFO("online cpu: slot {} block built: {}", j, Hex(base + blk, 64));
  return true;
}

// (shown: the name the lobby shows instead of the superstar's - MYSTERY
// OPPONENT, online_rules.cpp; the superstar block is the real one)
void MakeCpu(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, const char* shown = nullptr) {
  uint8_t* rec = Record(base, L, j);
  std::memcpy(g_vacant[j], rec, kSlotSize);
  // a superstar nobody in the lobby has
  std::vector<const Star*> free;
  for (const Star& s : kStars) {
    bool taken = false;
    for (uint32_t k = 0; k < SlotCount(base, L); ++k)
      if (k != j && Rd32(Record(base, L, k)) >= 8 && Rd32(Record(base, L, k) + 136) == s.id) taken = true;
    if (!taken) free.push_back(&s);
  }
  static std::mt19937 rng{std::random_device{}()};
  const Star& s = *free[free.empty() ? 0 : rng() % free.size()];
  Wr32(rec, kCpuState);
  Wr32(rec + 4, kCpuTag);
  std::memset(rec + 12, 0, 16);
  std::memcpy(rec + 12, "CPU", 3);
  Wr32(rec + 136, s.id);
  // (the attire: +148, the alternate +152 (-1 none) - as a pick stores them;
  // +140 is a per-superstar byte the pick also carries, left as it was)
  Wr32(rec + 148, 0);
  Wr32(rec + 152, 0xFFFFFFFF);
  std::memset(rec + 160, 0, 120);
  rec[172] = uint8_t(s.id >> 8), rec[173] = uint8_t(s.id);
  std::strncpy(reinterpret_cast<char*>(rec + 174), shown ? shown : s.name, 40);
  std::strncpy(reinterpret_cast<char*>(rec + 242), shown ? shown : s.name, 36);
  REXLOG_INFO("online cpu: slot {} is a CPU - {} (id {}){}", j, s.name, s.id, shown ? " - shown as " + std::string(shown) : "");
  const bool built = BuildBlock(ctx, base, L, j, s.id);
  SendRecord(ctx, base, L, j, !built);
}

void MakeVacant(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j) {
  uint8_t* rec = Record(base, L, j);
  std::memcpy(rec, g_vacant[j], kSlotSize);
  if (IsCpu(rec)) Wr32(rec, 0), Wr32(rec + 4, 0xFFFFFFFF);  // (never a CPU again)
  REXLOG_INFO("online cpu: slot {} vacant again", j);
  SendRecord(ctx, base, L, j);
}

}  // namespace

namespace {
std::atomic<int64_t> g_lobby_seen{0};       // ms: the lobby's last frame
std::atomic<uint32_t> g_lobby_state{0}, g_lobby_cursor{0}, g_lobby_addr{0};
uint8_t* g_lobby_base = nullptr;
}  // namespace

namespace svr2011 {
bool OnlineLobbySeenWithin(int64_t ms) {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  return now - g_lobby_seen.load() <= ms;
}
// (online_rules.h: the port rules' CPUs)
bool OnlineLobbyView(uint32_t* state, uint32_t* cursor, uint8_t** base, uint32_t* lobby) {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now - g_lobby_seen.load() > 300) return false;
  *state = g_lobby_state.load(), *cursor = g_lobby_cursor.load();
  *base = g_lobby_base, *lobby = g_lobby_addr.load();
  if (!*base || !*lobby) return false;
  return true;
}
uint32_t OnlineLobby(uint8_t* base) { return Lobby(base); }
uint32_t OnlineLobbySlots(uint8_t* base, uint32_t L) { return SlotCount(base, L); }
const uint8_t* OnlineLobbyRecord(uint8_t* base, uint32_t L, uint32_t j) { return Record(base, L, j); }
bool OnlineLobbyIsCpu(uint8_t* base, uint32_t L, uint32_t j) { return IsCpu(Record(base, L, j)); }
void OnlineLobbyMakeCpu(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, const char* shown) {
  MakeCpu(ctx, base, L, j, shown);
}
}  // namespace svr2011

namespace {
void PicturesFrame(PPCContext& ctx, uint8_t* base, uint32_t L);  // (other players' Created Superstar pictures: below)
}  // namespace

// The Player Match lobby's frame (sub_824E6510(screen)). Y pressed (pad 1's
// own buttons: the lobby's Y listener isn't called on the host) on the host
// with the cursor on a vacant slot: a CPU there; on a CPU: vacant again.
REX_EXTERN(__imp__sub_824E6510);
REX_HOOK_RAW(sub_824E6510) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t L = Lobby(base);
  if (screen && L) {
    const uint32_t state = Rd32(base + screen + 1168), j = Rd32(base + screen + 1060);
    g_lobby_state = state, g_lobby_cursor = j, g_lobby_addr = L, g_lobby_base = base;  // (OnlineLobbyView: online_rules.cpp's covers)
    svr2011::OnlineModsFrame(ctx, base);
    svr2011::OnlineChatFrame(ctx, base);
    PicturesFrame(ctx, base, L);
    g_lobby_seen = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
    static uint32_t last_state = 0xFFFFFFFF, last_cursor = 0xFFFFFFFF;
    if (LogOn() && state != last_state) REXLOG_INFO("online cpu: lobby state {} (cursor slot {})", state, j);
    else if (LogOn() && j != last_cursor) REXLOG_INFO("online cpu: lobby cursor slot {} (state {})", j, state);
    last_state = state, last_cursor = j;
    if (LogOn()) {  // (the slot records when one changes: state, net player, superstar)
      static std::string last;
      std::string now = fmt::format("joined {} me {}:", Rd32(base + L + 24), Rd32(base + L + 20));
      for (uint32_t k = 0; k < SlotCount(base, L); ++k) {
        const uint8_t* rec = Record(base, L, k);
        now += fmt::format(" [{} {} {:08X} {}]", k, Rd32(rec), Rd32(rec + 4), Rd32(rec + 136));
      }
      if (now != last) REXLOG_INFO("online cpu: slots {}", now);
      last = now;
    }
    static bool y_down = false;
    const bool y = (svr2011::OnlineRawPad() & 0x8000) != 0;  // (XINPUT_GAMEPAD_Y)
    const bool pressed = y && !y_down;
    y_down = y;
    static bool a_down = false;
    const bool a = (svr2011::OnlineRawPad() & 0x1000) != 0;  // (XINPUT_GAMEPAD_A)
    const bool a_pressed = a && !a_down;
    a_down = a;
    // (not while the host's own slot is unclaimed: A then claims it - the
    // cursor field still said the CPU's slot, and the host's own pick went to
    // the CPU)
    const uint32_t mine = Rd32(base + L + 20);
    const bool own_claimed = mine < SlotCount(base, L) && Rd32(Record(base, L, mine)) >= 5;
    if (a_pressed && g_edit < 0 && REXCVAR_GET(online_cpu_slots) && (state == 11 || state == 22) && own_claimed &&
        j < SlotCount(base, L) && IsCpu(Record(base, L, j)) && IsHost(ctx, base)) {
      if (const uint32_t panel = Panel0(base)) {
        g_edit = int(j);
        std::memcpy(g_panel_save, base + panel, sizeof(g_panel_save));
        PPCContext c = ctx;
        c.r3.u64 = screen, c.r4.u64 = 5;  // (the list opens: sub_8251BE90 is the screen's own state setter)
        sub_8251BE90(c, base);
        REXLOG_INFO("online cpu: the host chooses slot {}'s superstar", j);
      }
    }
    if (g_edit >= 0) {
      const uint32_t S = Rd32(base + kSelect);
      const uint32_t done = S ? Rd32(base + S) : 1;
      // (backing out or failing: as done - the game's own answers to those
      // release or clear the HOST's slot)
      if (state == 6) g_edit_open = true;
      if (state == 6 && S && done == 22) {
        const uint32_t result = Rd32(base + S + 4);
        if (result == 1 || result == 4 || result == 5) Wr32(base + S + 4, 3);
      }
      if (g_edit_open && (state == 10 || state == 11 || state == 22)) {
        g_edit_open = false;
        if (const uint32_t panel = Panel0(base)) std::memcpy(base + panel, g_panel_save, sizeof(g_panel_save));
        REXLOG_INFO("online cpu: slot {} is {} (id {})", g_edit, reinterpret_cast<const char*>(Record(base, L, g_edit) + 174),
                    Rd32(Record(base, L, g_edit) + 136));
        g_edit = -1;
      }
    }
    if ((state == 11 || state == 22) && IsHost(ctx, base))
      svr2011::OnlineRulesLobby(ctx, base, L);  // (the port rules' CPUs: online_rules.h)
    if (pressed && REXCVAR_GET(online_cpu_slots)) {
      if (LogOn()) REXLOG_INFO("online cpu: Y (lobby state {}, cursor slot {})", state, j);
      if ((state == 11 || state == 22) && j < SlotCount(base, L) && IsHost(ctx, base)) {
        uint8_t* rec = Record(base, L, j);
        if (IsCpu(rec)) MakeVacant(ctx, base, L, j);
        else if (Rd32(rec) < 5) MakeCpu(ctx, base, L, j);
      }
    }
  }
  __imp__sub_824E6510(ctx, base);
}

// The lobby's start checks (sub_828AC668(L, stage): every slot joined and at
// the stage): the CPU slots count as joined.
REX_EXTERN(__imp__sub_828AC668);
REX_HOOK_RAW(sub_828AC668) {
  const uint32_t L = ctx.r3.u32;
  // (only with 2 humans or more: the network library won't start a match
  // with fewer consoles - a host alone with CPUs waits for a player)
  const int cpus = L && Rd32(base + L + 24) >= 2 ? CpuCount(base, L) : 0;
  if (!cpus) {
    __imp__sub_828AC668(ctx, base);
    if (ctx.r3.u32 && !svr2011::OnlineModsMayStart(base, L)) ctx.r3.u64 = 0;  // (online_mods.h)
    return;
  }
  const uint32_t joined = Rd32(base + L + 24);
  Wr32(base + L + 24, joined + cpus);
  const uint32_t stage = ctx.r4.u32;
  __imp__sub_828AC668(ctx, base);
  Wr32(base + L + 24, joined);
  if (ctx.r3.u32 && !svr2011::OnlineModsMayStart(base, L)) ctx.r3.u64 = 0;  // (online_mods.h)
  if (LogOn()) {
    static uint32_t last = 99;
    if (ctx.r3.u32 != last || stage != 0) {
      REXLOG_INFO("online cpu: start check stage {} with {} CPU(s) -> {}", stage, cpus, ctx.r3.u32);
      last = ctx.r3.u32;
    }
  }
}

// "Block j done" (sub_828AD2F8(L, j): the bits of the block's mask at
// L+3564+j*15044 - the host needs one per slot, its own and every joiner's
// acknowledgement; a joiner exactly one, its own, set when the block
// arrives): on the HOST the CPU slots' bits stand in for the acknowledgements
// they never send. Not on joiners - the host sends them every block, the
// CPUs' too (with them set, a joiner never counted exactly one: "EXCHANGING
// SUPERSTAR INFORMATION" forever with 2 CPUs).
REX_EXTERN(__imp__sub_828AD2F8);
REX_HOOK_RAW(sub_828AD2F8) {
  const uint32_t L = ctx.r3.u32;
  if (L && IsHost(ctx, base)) {
    uint32_t bits = 0;
    for (uint32_t j = 0; j < SlotCount(base, L); ++j)
      if (IsCpu(Record(base, L, j))) bits |= 1u << j;
    if (bits)
      for (uint32_t k = 0; k < SlotCount(base, L); ++k) {
        uint8_t* mask = base + L + kBlocks - 4 + k * kBlockSize;
        Wr32(mask, Rd32(mask) | bits);
      }
  }
  __imp__sub_828AD2F8(ctx, base);
}

// A match slot made from a lobby slot (sub_824F6AE0(obj, match slot, lobby
// slot), on every machine): a CPU's is controlled by the CPU.
REX_EXTERN(__imp__sub_824F6AE0);
REX_HOOK_RAW(sub_824F6AE0) {
  const uint32_t to = ctx.r4.u32, from = ctx.r5.u32;
  __imp__sub_824F6AE0(ctx, base);
  const uint32_t L = Lobby(base), match = Rd32(base + kMatch);
  if (!L || !match || from >= 6 || to >= 6) return;
  uint8_t* slot = base + match + kMatchSlots + to * kMatchSlotSize;
  if (LogOn())
    REXLOG_INFO("online cpu: match slot {} from lobby slot {}: {}", to, from, Hex(slot - 8, 72));
  if (!IsCpu(Record(base, L, from))) return;
  // (joiners build the CPU's block too - the same stock data from the synced
  // id - so it doesn't hang on the host's block broadcast reaching them)
  // (only when its copy isn't that superstar's - the host's block, e.g. a
  // superstar and attire the host chose, arrived otherwise; block +32 = id)
  if (!IsHost(ctx, base)) {
    const uint32_t id = Rd32(Record(base, L, from) + 136);
    const uint8_t* blk = base + L + kBlocks + from * kBlockSize;
    if ((uint32_t(blk[32]) << 8 | blk[33]) != id) BuildBlock(ctx, base, L, from, id);
  }
  // (+56 stays as the game set it, "use the copied block": the host built the
  // CPU's block (BuildBlock) and every machine has it - a cleared +56 left
  // sub_828B92A0 returning 0 to a caller that reads it on joiners)
  REXLOG_INFO("online cpu: match slot {} is the CPU (id {}): {}; block {}", to, Rd32(Record(base, L, from) + 136), Hex(slot - 8, 72),
              Hex(base + L + kBlocks + from * kBlockSize, 32));
}

// The match's people entries from the lobby (sub_824F64A8(obj): 30 entries
// at obj+96 + 20j - id, attire, two more, the controller: this machine's pad
// for its own slot, 4 for the others): a CPU slot's controller is -1, as the
// game's own COM entrants have (the match slot then stays a CPU's).
REX_EXTERN(__imp__sub_824F64A8);
REX_HOOK_RAW(sub_824F64A8) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824F64A8(ctx, base);
  const uint32_t L = Lobby(base);
  if (!L || !obj) return;
  for (uint32_t j = 0; j < SlotCount(base, L); ++j) {
    uint8_t* e = base + obj + 96 + 20 * j;
    if (LogOn()) REXLOG_INFO("online cpu: people entry {}: {}", j, Hex(e, 20));
    if (!IsCpu(Record(base, L, j))) continue;
    Wr32(e + 16, 0xFFFFFFFF);
    REXLOG_INFO("online cpu: people entry {} is a CPU (controller -1)", j);
  }
}

// The online select's pick to the host (sub_828ACCB8(L, ...)): its payload.
REX_EXTERN(__imp__sub_828ACCB8);
REX_HOOK_RAW(sub_828ACCB8) {
  if (LogOn())
    REXLOG_INFO("online cpu: pick sent (r4 {:08X}): {}", ctx.r4.u32,
                ctx.r4.u32 > 0x10000 ? Hex(base + ctx.r4.u32, 24) : std::string());
  __imp__sub_828ACCB8(ctx, base);
}

// Test aid (SVR2011_TEST_LOBBY_LOG): the match start's connection check
// (sub_82891FA8(net, i): -1 = go on; else "Your opponent has left") and its parts.
REX_EXTERN(__imp__sub_82891FA8);
REX_HOOK_RAW(sub_82891FA8) {
  const uint32_t i = ctx.r4.u32;
  __imp__sub_82891FA8(ctx, base);
  if (LogOn()) {
    static int n = 0;
    if (n++ < 30) {
      const uint32_t table = Rd32(base + 0x82EDE930);
      REXLOG_INFO("online cpu: connection check {} -> {} (table {:08X}: {})", i, int32_t(ctx.r3.u32), table,
                  table ? Hex(base + table, 6 * 20) : std::string());
    }
  }
}
REX_EXTERN(__imp__sub_82990FD8);
REX_HOOK_RAW(sub_82990FD8) {
  const uint32_t a = ctx.r4.u32;
  __imp__sub_82990FD8(ctx, base);
}
// The network library's sync (sub_82991010(gss, entry)): logged with CPU slots.
REX_EXTERN(__imp__sub_82991010);
REX_HOOK_RAW(sub_82991010) {
  const uint32_t gss = ctx.r3.u32, e = ctx.r4.u32;
  __imp__sub_82991010(ctx, base);
  const uint32_t L = Lobby(base);
  if (L && CpuCount(base, L)) {
    static int n = 0;
    if (n++ < 40)
      REXLOG_INFO("online cpu: gss sync (gss {:08X}, entry {:08X}: {}) -> {}", gss, e, e ? Hex(base + e, 20) : std::string(),
                  int32_t(ctx.r3.u32));
  }
}
REX_EXTERN(__imp__sub_82891380);
REX_HOOK_RAW(sub_82891380) {
  const uint32_t i = ctx.r4.u32;
  __imp__sub_82891380(ctx, base);
  if (LogOn()) {
    static int n = 0;
    if (n++ < 30) REXLOG_INFO("online cpu: net check {} -> {}", i, int32_t(ctx.r3.u32));
  }
}

// The match's net table (sub_824E5398: *0x82EDE914, +2236 / +2240 the number
// of entries - the lobby's slots - one per human's net player number, +2232
// this machine's): with CPU slots there are fewer humans, so fewer entries
// (else the match waits for players that never connect: "Your opponent has
// left the game session"). The humans' numbers stay 0..n-1.
REX_EXTERN(__imp__sub_824E5398);
REX_HOOK_RAW(sub_824E5398) {
  __imp__sub_824E5398(ctx, base);
  const uint32_t L = Lobby(base), net = Rd32(base + 0x82EDE914);
  if (!L || !net) return;
  const int cpus = CpuCount(base, L);
  if (!cpus) return;
  const uint32_t entries = Rd32(base + net + 2236);
  if (entries <= uint32_t(cpus)) return;
  // (only +2236, the entry count: +2240 stays the lobby's slot count (its
  // reader, sub_82893080, loops over lobby slots) and +2232 stays this
  // machine's LOBBY SLOT - every reader maps it to an entry through the
  // entries' +4 (sub_82892CF0); an entry index there made a joiner not in
  // slot 0 take the CPU for itself and leave the match)
  Wr32(base + net + 2236, entries - cpus);
  REXLOG_INFO("online cpu: net table {} -> {} entries, this machine slot {}: {}", entries, entries - cpus,
              Rd32(base + net + 2232), Hex(base + net, 72 * 3));
}

// The match's cross-console sync points (sub_82894AB0(N, k) waits until
// sub_82893958(N, k) says every entry reached point k: N + (9k+110+i)*4 set
// for each entry i < +2236). Its "k < +2236" guard assumes no more points
// than entries - with CPUs there are more people than entries, and the wait
// after the entrances (a point past the humans' count) never ended: both
// games sat on NOW LOADING. With a CPU slot: the same check for those points.
REX_EXTERN(__imp__sub_82893958);
REX_HOOK_RAW(sub_82893958) {
  const uint32_t N = ctx.r3.u32, k = ctx.r4.u32;
  __imp__sub_82893958(ctx, base);
  const uint32_t L = Lobby(base);
  if (ctx.r3.u32 || !N || !L || !CpuCount(base, L)) return;
  const uint32_t entries = Rd32(base + N + 2236);
  if (k < entries || k >= 6) return;
  bool ready = entries > 0;
  for (uint32_t i = 0; i < entries; ++i) ready = ready && Rd32(base + N + (9 * k + 110 + i) * 4) != 0;
  static uint32_t logged = 0xFFFFFFFF;
  if (k != logged || ready) {
    REXLOG_INFO("online cpu: sync point {} ({} entries): {}", k, entries, ready ? "everyone" : "waiting");
    logged = ready ? 0xFFFFFFFF : k;
  }
  if (ready) ctx.r3.u64 = 1;
}

// The lobby's help bar: the screen picks its bar id every frame (sub_824E5578,
// then sub_824E3E60(screen, &id) adjusts it), and the bar is only rebuilt
// when the id changes (the host's lobby has a few - 1286, and others by what
// the cursor is on - none of them telling a CPU from a player). So the Y label's state rides in the id's high half (1 + y + 3 *
// waiting: y 1 ADD CPU, 2 REMOVE CPU, waiting for one more player), which
// changes the id with it; online.cpp's bar hook (sub_82746358) takes it
// off and puts the labels in.
REX_EXTERN(__imp__sub_824E3E60);
REX_HOOK_RAW(sub_824E3E60) {
  const uint32_t screen = ctx.r3.u32, idp = ctx.r4.u32;
  __imp__sub_824E3E60(ctx, base);
  if (!screen || !idp || !REXCVAR_GET(online_cpu_slots)) return;
  const uint32_t id = Rd32(base + idp);
  if (!id || id > 0xFFFF) return;
  const uint32_t L = Lobby(base);
  if (!L) return;
  const uint32_t state = Rd32(base + screen + 1168), j = Rd32(base + screen + 1060);
  if (!((state == 11 || state == 22) && j < SlotCount(base, L) && IsHost(ctx, base))) return;
  const uint32_t y = IsCpu(Record(base, L, j)) ? 2 : Rd32(Record(base, L, j)) < 5 ? 1 : 0;
  const uint32_t waiting = Rd32(base + L + 24) < 2 && CpuCount(base, L) > 0 ? 1 : 0;
  static uint32_t logged = 0;
  if (LogOn() && id != logged) REXLOG_INFO("online cpu: lobby bar {} (cursor slot {}, Y {}, waiting {})", id, j, y, waiting);
  logged = id;
  if (y || waiting) Wr32(base + idp, id | ((1 + y + 3 * waiting) << 16));
}

// The result screen's kinds (sub_828A7F18, at the match's end): a person who
// left (sub_828932D8(N, slot): its entry's +32) shows DISCONNECT, one with
// entry +64 set (sub_82893460) LOSE. A CPU slot has no entry (sub_82892CF0:
// 6), so both read sync flags instead - set on joiners: the CPU showed
// DISCONNECT there. A CPU never left: its real result.
bool CpuSlot(uint8_t* base, uint32_t slot) {
  const uint32_t L = Lobby(base);
  return L && slot < SlotCount(base, L) && IsCpu(Record(base, L, slot));
}

REX_EXTERN(__imp__sub_828932D8);
REX_HOOK_RAW(sub_828932D8) {
  if (CpuSlot(base, ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828932D8(ctx, base);
}

REX_EXTERN(__imp__sub_82893460);
REX_HOOK_RAW(sub_82893460) {
  if (CpuSlot(base, ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82893460(ctx, base);
}

// The host's handlers for a slot's pick (sub_828AE250(L, msg)) and its
// superstar block (sub_828AE378(L, msg)) - msg +0 the sender's net player,
// +8 the data: from the host itself while it chooses a CPU's superstar, they
// go to the CPU slot (sub_828A5F08 below), which stays a CPU (state 26: the
// stores set 8 / 10 and the handler sends the record right after).
bool EditMessage(uint8_t* base, uint32_t L, uint32_t msg) {
  if (g_edit < 0 || !L || !msg) return false;
  const uint32_t mine = Rd32(base + L + 20);  // (this machine's slot)
  return mine < SlotCount(base, L) && Rd32(base + msg) == Rd32(Record(base, L, mine) + 4);
}

REX_EXTERN(__imp__sub_828AE250);
REX_HOOK_RAW(sub_828AE250) {
  const uint32_t L = ctx.r3.u32;
  // (a mod superstar: in this game's numbering, refused if someone lacks it - online_mods.h)
  svr2011::OnlineModsPick(ctx, base, ctx.r4.u32);
  struct Done {
    ~Done() { svr2011::OnlineModsPickDone(); }
  } done;
  if (!EditMessage(base, L, ctx.r4.u32)) {
    __imp__sub_828AE250(ctx, base);
    return;
  }
  // (its own superstar doesn't count as taken: an attire-only change)
  uint8_t* rec = Record(base, L, g_edit);
  const uint32_t had = Rd32(rec + 136);
  Wr32(rec + 136, 512);
  g_edit_msg = true;
  __imp__sub_828AE250(ctx, base);
  g_edit_msg = false;
  if (Rd32(rec + 136) == 512) Wr32(rec + 136, had);  // (refused: taken by another)
  REXLOG_INFO("online cpu: the host's pick for slot {}: id {} (was {})", g_edit, Rd32(rec + 136), had);
}

REX_EXTERN(__imp__sub_828AE378);
REX_HOOK_RAW(sub_828AE378) {
  svr2011::OnlineModsBlockFrom(ctx, base, ctx.r4.u32);  // (online_mods.h: the sender's numbering)
  if (!EditMessage(base, ctx.r3.u32, ctx.r4.u32)) {
    __imp__sub_828AE378(ctx, base);
    return;
  }
  g_edit_msg = true;
  __imp__sub_828AE378(ctx, base);
  g_edit_msg = false;
  REXLOG_INFO("online cpu: slot {}'s superstar block from the host's pick", g_edit);
}

REX_EXTERN(__imp__sub_828A5F08);
REX_HOOK_RAW(sub_828A5F08) {
  if (g_edit_msg && g_edit >= 0) {
    ctx.r3.u64 = uint32_t(g_edit);
    return;
  }
  __imp__sub_828A5F08(ctx, base);
}

REX_EXTERN(__imp__sub_828A6008);
REX_HOOK_RAW(sub_828A6008) {
  const uint32_t L = ctx.r3.u32, j = ctx.r4.u32;
  __imp__sub_828A6008(ctx, base);
  if (g_edit_msg && int(j) == g_edit) Wr32(Record(base, L, j), kCpuState);
}

REX_EXTERN(__imp__sub_828A6078);
REX_HOOK_RAW(sub_828A6078) {
  const uint32_t L = ctx.r3.u32, j = ctx.r4.u32;
  svr2011::OnlineModsBlockStore(base, ctx.r5.u32);  // (online_mods.h: in this game's numbering)
  __imp__sub_828A6078(ctx, base);
  if (g_edit_msg && int(j) == g_edit) Wr32(Record(base, L, j), kCpuState);
}

// The lobby's superstar pictures: each slot's card (sub_8251E6F8(cards, ?,
// slot, superstar, attire): its picture cards + 48 + slot * 352) and the
// SUPERSTAR INFO panel (sub_8251DB28(item, panel): one item, +0 the slot,
// +8 the superstar, +12 the attire; its picture item + 160) first show the
// vacant silhouette (holder 0), then by the superstar's kind (sub_828B78C0:
// 1 stock, 2 an added one, 3 a Created Superstar 0..49) ask for its picture
// (sub_8251BA48(slot + 1, {id, 2}, {attire + 10, 0}, 0)) and, answered
// "ready" (3), show the slot's holder (sub_8251BB28 -> sub_827023A0); still
// loading, the lobby's pollers show it when it is. A Created Superstar's was
// never asked for (the panel's switch, panel + 22724, turned off): the
// silhouette. Here this player's own is asked for as the stock ones are (and
// the host's CPU's: his own too) - by both, and shown at once when ready (a
// picture already loaded - after another player's pick - had stayed the
// silhouette).
//
// Another player's is his, not ours (the same number is another Superstar
// here, or none): its render comes from its owner (online_cas.h). The
// picture is asked for with one of our Created Superstars (a stand-in) - the
// picture object reads that one's .cas (sub_8273B868: the attire's
// 149528-byte block, the 256 x 512 render first, to object + 0x2DC7C) -
// tagged (+20 of the request: kPictureTag | n), and the render is put in its
// block before it is shown (sub_828DB5C8(image, block)). Until the render has
// come the card keeps the silhouette; the lobby's frame then asks again.
namespace {
constexpr uint32_t kGss = 0x82EDE910;  // -> the network library (sub_8288EC28: a net player's online id)
constexpr uint32_t kPictureTag = 0x50490000;
constexpr uint32_t kObjectBlock = 0x2DC7C;
constexpr uint32_t kRenderSize = 256 * 512;
constexpr uint32_t kPictureOn = 22724;     // panel + : the panel's picture polled (the game clears it)
constexpr uint32_t kPanelShown = 22492;    // panel + : the picture it polls
constexpr uint32_t kCardPicture = 48, kCardSize = 352;
std::vector<const std::string*> g_given;          // renders given out (the tag's n)
std::map<uint32_t, const std::string*> g_tagged;  // picture object -> the render to show
std::map<uint32_t, uint32_t> g_object_stand_in;   // picture object -> the stand-in it reads
std::vector<uint32_t> g_unusable;                 // stand-ins whose picture failed (this lobby; the superstar list loading the same .cas then)
// The Created Superstar of ours each slot shows another player's with: one
// no other slot uses (two slots' pictures on the same .cas: blank cards).
int g_stand_in[6] = {-1, -1, -1, -1, -1, -1};

uint32_t TagFor(const std::string* render) {
  for (size_t n = 0; n < g_given.size(); ++n)
    if (g_given[n] == render) return kPictureTag | uint32_t(n);
  g_given.push_back(render);
  return kPictureTag | uint32_t(g_given.size() - 1);
}

using svr2011::NowMs;

// A slot's player's online id (the network library's: the lobby's table at
// L + 0x2CF20 was empty on the host for a joiner's slot).
uint64_t OwnerOf(PPCContext ctx, uint8_t* base, const uint8_t* rec) {
  ctx.r3.u64 = Rd32(base + kGss), ctx.r4.u64 = Rd32(rec + 4);
  if (!ctx.r3.u32) return 0;
  sub_8288EC28(ctx, base);
  return ctx.r3.u64;
}

// Is slot j this game's own Superstar (its player's, or the host's CPU's)?
bool Ours(PPCContext ctx, uint8_t* base, uint32_t L, uint32_t j) {
  const uint8_t* rec = Record(base, L, j);
  return j == Rd32(base + L + 20) || (IsCpu(rec) && IsHost(ctx, base));
}

// A stand-in for slot j (-1: none free).
int StandIn(PPCContext ctx, uint8_t* base, uint32_t L, uint32_t j) {
  std::vector<uint32_t> mine = svr2011::OnlineLocalCaws();
  for (const uint32_t bad : g_unusable) mine.erase(std::remove(mine.begin(), mine.end(), bad), mine.end());
  auto used = [&](uint32_t id) {
    for (uint32_t k = 0; k < SlotCount(base, L); ++k) {
      if (k == j) continue;
      if (g_stand_in[k] == int(id)) return true;
      const uint8_t* rec = Record(base, L, k);
      if (Rd32(rec) >= 5 && Rd32(rec + 136) == id && Ours(ctx, base, L, k)) return true;
    }
    return false;
  };
  if (g_stand_in[j] >= 0 && std::find(mine.begin(), mine.end(), uint32_t(g_stand_in[j])) != mine.end() &&
      !used(uint32_t(g_stand_in[j])))
    return g_stand_in[j];
  for (const uint32_t id : mine)
    if (!used(id)) return g_stand_in[j] = int(id);
  return g_stand_in[j] = -1;
}

// A slot's card as the game last set it (asked again from the lobby's
// frame: its render come, or its stand-in picked here since).
struct Card {
  uint32_t cards = 0, r4 = 0, id = 0, attire = 0, L = 0;
  bool again = false;
  int64_t since = 0;
};
Card g_cards[6];

// What a Created Superstar's picture on slot j is asked with: {its own
// number, 0} ours, {a stand-in, its tag} another player's; nullopt: none (yet).
std::optional<std::pair<uint32_t, uint32_t>> PictureAsk(PPCContext ctx, uint8_t* base, uint32_t L, uint32_t j,
                                                        uint32_t id, uint32_t attire) {
  const uint8_t* rec = Record(base, L, j);
  if (Ours(ctx, base, L, j)) {
    g_stand_in[j] = -1;
    // (another slot shown with this one: asked again, with another)
    for (uint32_t k = 0; k < 6; ++k)
      if (k != j && g_stand_in[k] == int(id)) g_stand_in[k] = -1, g_cards[k].again = true;
    return std::pair<uint32_t, uint32_t>{id, 0};
  }
  const uint64_t owner = IsCpu(rec) ? 0 : OwnerOf(ctx, base, rec);
  if (!owner) return std::nullopt;
  const std::string* render = svr2011::OnlinePicture(owner, id, attire);
  const int local = StandIn(ctx, base, L, j);
  if (!render || local < 0) {
    if (LogOn())
      REXLOG_INFO("online cpu: picture of slot {}: {:016X}'s Created Superstar {} attire {}: {}", j, owner, id, attire,
                  local < 0 ? "no Created Superstar here to show it with" : "its render is on its way");
    if (local >= 0) g_cards[j].again = true;
    return std::nullopt;
  }
  return std::pair<uint32_t, uint32_t>{uint32_t(local), TagFor(render)};
}

uint32_t AskPicture(PPCContext c, uint8_t* base, uint32_t slot, uint32_t id, uint32_t attire, uint32_t tag) {
  c.r3.u64 = slot + 1;
  c.r4.u64 = (uint64_t(id) << 32) | 2;
  c.r5.u64 = uint64_t(attire + 10) << 32;
  c.r6.u64 = tag;  // (+16 stays 0: non-zero is another kind of picture)
  sub_8251BA48(c, base);
  return c.r3.u32;  // (3: ready)
}

// Created Superstar pictures load one at a time: the picture objects read
// their .cas through one job queue, and of two loading at once one failed
// (the holder then shows its "no picture", 322 - sub_8273BF28). Another is
// asked for once the one loading is ready (or after 8 s); 0: not yet.
struct Loading {
  int slot = -1;
  uint32_t id = 0, attire = 0, tag = 0;
  int64_t since = 0;
};
Loading g_loading;

uint32_t AskCaw(PPCContext c, uint8_t* base, uint32_t slot, uint32_t id, uint32_t attire, uint32_t tag) {
  if (g_loading.slot >= 0 && g_loading.slot != int(slot)) {
    const uint32_t was = AskPicture(c, base, uint32_t(g_loading.slot), g_loading.id, g_loading.attire, g_loading.tag);
    if (was != 3 && NowMs() - g_loading.since < 8000) return 0;
    g_loading.slot = -1;
  }
  const uint32_t answer = AskPicture(c, base, slot, id, attire, tag);
  if (answer == 3) {
    if (g_loading.slot == int(slot)) g_loading.slot = -1;
  } else {
    const bool same = g_loading.slot == int(slot) && g_loading.id == id && g_loading.attire == attire && g_loading.tag == tag;
    g_loading = {int(slot), id, attire, tag, same ? g_loading.since : NowMs()};
  }
  return answer;
}

// A picture (a card's, the panel's) shown from the slot's holder.
void ShowFromHolder(PPCContext c, uint8_t* base, uint32_t picture, uint32_t slot) {
  const uint32_t tmp = svr2011::OnlineScratch();
  if (!tmp) return;
  c.r3.u64 = tmp + 2048, c.r4.u64 = slot + 1, c.r5.u64 = 1;
  sub_8251BB28(c, base);
  const uint32_t handle = c.r3.u32;
  if (!handle) return;
  c.r3.u64 = picture;
  c.r4.u64 = uint64_t(Rd32(base + handle)) << 32 | Rd32(base + handle + 4);
  sub_827023A0(c, base);
}
}  // namespace

// A slot's card.
REX_EXTERN(__imp__sub_8251E6F8);
REX_HOOK_RAW(sub_8251E6F8) {
  const PPCContext saved = ctx;
  const uint32_t cards = ctx.r3.u32, slot = ctx.r5.u32, id = ctx.r6.u32, attire = ctx.r7.u32;
  __imp__sub_8251E6F8(ctx, base);
  const uint32_t L = Lobby(base);
  if (!cards || !L || slot >= 6) return;
  g_cards[slot] = {cards, saved.r4.u32, id, attire, L, false, NowMs()};
  if (id >= 50) {
    g_stand_in[slot] = -1;
    if (g_loading.slot == int(slot)) g_loading.slot = -1;
    return;
  }
  const auto ask = PictureAsk(saved, base, L, slot, id, attire);
  if (LogOn()) {  // (when it changes: the cards are set again and again)
    static std::string last[6];
    const std::string now = "Created Superstar " + std::to_string(id) +
                            (!ask ? " - not yet" : ask->second ? " (another player's, with ours " + std::to_string(ask->first) + ")" : " (ours)");
    if (now != last[slot]) REXLOG_INFO("online cpu: card of slot {}: {}", slot, now);
    last[slot] = now;
  }
  if (!ask) return;
  const uint32_t answer = AskCaw(saved, base, slot, ask->first, attire, ask->second);
  if (answer == 3) ShowFromHolder(saved, base, cards + kCardPicture + slot * kCardSize, slot);
  if (!answer) g_cards[slot].again = true;  // (another loading: after it)
}

// The SUPERSTAR INFO panel.
REX_EXTERN(__imp__sub_8251DB28);
REX_HOOK_RAW(sub_8251DB28) {
  const uint32_t item = ctx.r3.u32, ctl = ctx.r4.u32;
  const PPCContext saved = ctx;
  // (the switch as it was: put back when this shows none)
  const uint32_t panel = ctl ? Rd32(base + ctl + 8) : 0;
  const uint32_t was_on = panel ? Rd32(base + panel + kPictureOn) : 0;
  __imp__sub_8251DB28(ctx, base);
  if (!item || !panel) return;
  const uint32_t slot = Rd32(base + item);
  const uint32_t id = Rd32(base + item + 8), attire = Rd32(base + item + 12);
  const uint32_t L = Lobby(base);
  if (!L || slot >= 6 || id >= 50) return;
  const auto ask = PictureAsk(saved, base, L, slot, id, attire);
  if (!ask) {
    Wr32(base + panel + kPictureOn, was_on);
    return;
  }
  const uint32_t answer = AskCaw(saved, base, slot, ask->first, attire, ask->second);
  if (!answer) {
    Wr32(base + panel + kPictureOn, was_on);
    return;
  }
  if (answer == 3) {
    ShowFromHolder(saved, base, item + 160, slot);
    Wr32(base + panel + kPanelShown, item + 160);
  }
  Wr32(base + panel + kPictureOn, 1);
}

namespace {
// The lobby's frame: cards to ask again (their render come, their stand-in
// picked here) - the card's update again, as the game made it.
void PicturesFrame(PPCContext& ctx, uint8_t* base, uint32_t L) {
  static uint32_t lobby = 0;
  if (L != lobby) lobby = L, g_unusable.clear();  // (a stand-in that failed is tried again in the next lobby)
  static int64_t last = 0;
  if (NowMs() - last < 250) return;
  last = NowMs();
  for (uint32_t j = 0; j < 6; ++j) {
    Card& c = g_cards[j];
    if (!c.again || !c.cards) continue;
    const uint8_t* rec = Record(base, L, j);
    if (c.L != L || NowMs() - c.since > 60000 || Rd32(rec + 136) != c.id) {
      c = {};
      continue;
    }
    const uint64_t owner = Ours(ctx, base, L, j) || IsCpu(rec) ? 0 : OwnerOf(ctx, base, rec);
    if (owner && !svr2011::OnlinePicture(owner, c.id, c.attire)) continue;
    PPCContext p = ctx;
    p.r3.u64 = c.cards, p.r4.u64 = c.r4, p.r5.u64 = j, p.r6.u64 = c.id, p.r7.u64 = c.attire;
    c.again = false;
    sub_8251E6F8(p, base);  // (the hook above)
  }
}
}  // namespace

// A picture object told what to show (sub_8273A0B0(object, request)): a
// tagged request's object gets the render given.
REX_EXTERN(__imp__sub_8273A0B0);
REX_HOOK_RAW(sub_8273A0B0) {
  const uint32_t obj = ctx.r3.u32, req = ctx.r4.u32;
  if (obj && req) {
    const uint32_t tag = Rd32(base + req + 20);
    if ((tag & 0xFFFF0000) == kPictureTag && (tag & 0xFFFF) < g_given.size()) {
      g_tagged[obj] = g_given[tag & 0xFFFF];
      g_object_stand_in[obj] = Rd32(base + req);
    } else {
      g_tagged.erase(obj);
      g_object_stand_in.erase(obj);
    }
  }
  __imp__sub_8273A0B0(ctx, base);
}

// The picture object's attire block read, about to be shown (sub_828DB5C8(
// image, object + 0x2DC7C)): another player's render in it.
REX_EXTERN(__imp__sub_828DB5C8);
REX_HOOK_RAW(sub_828DB5C8) {
  const uint32_t block = ctx.r4.u32;
  if (block > kObjectBlock && !g_tagged.empty()) {
    const auto it = g_tagged.find(block - kObjectBlock);
    if (it != g_tagged.end()) {
      if (it->second && it->second->size() == kRenderSize) std::memcpy(base + block, it->second->data(), kRenderSize);
      if (LogOn()) REXLOG_INFO("online cpu: another player's Created Superstar render put in picture {:08X}", it->first);
      g_tagged.erase(it);
    }
  }
  __imp__sub_828DB5C8(ctx, base);
}

namespace svr2011 {
void OnlineCawPictureFailed(uint8_t* base, uint32_t holder) {
  const uint32_t obj = Rd32(base + holder + 24);
  const auto it = g_object_stand_in.find(obj);
  if (it == g_object_stand_in.end()) return;
  const uint32_t id = it->second;
  g_object_stand_in.erase(it);
  g_tagged.erase(obj);
  if (std::find(g_unusable.begin(), g_unusable.end(), id) == g_unusable.end()) g_unusable.push_back(id);
  REXLOG_INFO("online cpu: our Created Superstar {} can't stand in for another player's picture (it didn't load) - another", id);
  for (uint32_t k = 0; k < 6; ++k)
    if (g_stand_in[k] == int(id)) g_stand_in[k] = -1, g_cards[k].again = true;
  if (g_loading.slot >= 0 && g_loading.id == id) g_loading.slot = -1;
}
}  // namespace svr2011
