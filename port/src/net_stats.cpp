// WWE SmackDown vs. Raw 2011 - online match network statistics (net_stats.h).
//
// Online matches run the game's own lockstep (Yuke's "gss" library): every
// frame each human's game sends its packed pad (message type 16, 4 bytes) and
// the world steps only once every human's input for that frame has arrived.
// The match net object (*0x82EDE914, sub_828928C8) keeps a "waiting" flag at
// +2264: while it is set the game's task groups don't run, so the world stands
// still while rendering goes on at 60 fps - online "lag" is these waits.
//
// The object: 6 entries of 72 bytes at +0 (+0 the net player number, +4 the
// person / lobby slot, +44 its input for this frame arrived, +48 own input,
// +52 the received input), +2232 this game's entry, +2236 the entry count.
// The send (sub_82893BD0, once a frame, r3 = the object) is where this samples.
//
// Every 5 s of a match the log gets a line: frames, frames waited, the waits'
// count and longest, and which entries the waits were for. The ping indicator
// (fps_overlay.cpp) reads NetStatsNow().
#include "net_stats.h"
#include "match_diag.h"
#include "online.h"
#include "online_chat.h"
#include "pad_types.h"

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "net_sim.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kNetObject = 0x82EDE914;
constexpr uint32_t kEntrySize = 72, kMaxEntries = 6;
constexpr uint32_t kGotInput = 44, kLocal = 2232, kCount = 2236, kWaiting = 2264;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

struct Window {
  uint64_t frames = 0, waited = 0, waits = 0;
  double longest_ms = 0, waited_ms = 0;
  std::array<uint64_t, kMaxEntries> blamed{};  // frames waited for each entry's input
  // (report 6dde2f) frames with a press: this game's input as sent (+48), each entry's as applied (+52), and each
  // of this PC's controllers (player 1-4) as the input system has them
  uint64_t sent_pressed = 0;
  std::array<uint64_t, kMaxEntries> applied_pressed{};
  std::array<uint64_t, svr2011::kMaxPlayers> pad_pressed{};
};

std::mutex g_mutex;
Window g_window;                 // the current 5 s
Clock::time_point g_window_start{};
bool g_was_waiting = false;
Clock::time_point g_wait_start{}, g_last_frame{};
uint32_t g_entries = 0, g_local = 0;
uint32_t g_send_port = 0xFFFFFFFF;  // (the controller port the match reads this game's input from: its wrestler's +1152)
uint32_t g_told_net = 0;            // (the match net object whose entries were logged)
bool g_warned_pad = false;          // (the "another controller was used" warning, once a match)

// The character objects by lobby slot (the send reads this game's pad from its slot's: +1152 the controller port,
// +1156 the person).
constexpr uint32_t kSlotCharacters = 0x82E3CC50;

// Any press on a controller (buttons, triggers, sticks past a dead zone).
bool Pressed(const rex::input::X_INPUT_GAMEPAD& g) {
  auto tilted = [](int16_t v) { return v > 9000 || v < -9000; };
  return uint16_t(g.buttons) != 0 || g.left_trigger > 40 || g.right_trigger > 40 || tilted(int16_t(g.thumb_lx)) ||
         tilted(int16_t(g.thumb_ly)) || tilted(int16_t(g.thumb_rx)) || tilted(int16_t(g.thumb_ry));
}

// This PC's controllers with a press now (bit per player 1-4).
uint32_t PadsPressed() {
  auto* rt = rex::Runtime::instance();
  auto* input = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
  if (!input) return 0;
  uint32_t bits = 0;
  for (uint32_t p = 0; p < svr2011::kMaxPlayers; ++p) {
    rex::input::X_INPUT_STATE st{};
    if (input->GetState(p, &st) == 0 && Pressed(st.gamepad)) bits |= 1u << p;
  }
  return bits;
}

const char* PadName(uint32_t port) {
  static const char* const kType[] = {"no controller", "Xbox", "PlayStation", "keyboard"};
  return port < svr2011::kMaxPlayers ? kType[int(svr2011::PlayerPadType(port))] : "no controller";
}
svr2011::NetStats g_now{};       // the last finished window (the indicator)
std::atomic<int64_t> g_last_sample_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void Report(Clock::time_point now) {
  const double secs = std::chrono::duration<double>(now - g_window_start).count();
  std::string blame;
  for (uint32_t k = 0; k < g_entries && k < kMaxEntries; ++k) {
    if (k == g_local) continue;
    blame += fmt::format("{}{}:{}", blame.empty() ? "" : " ", k, g_window.blamed[k]);
  }
  std::string rtts;
  for (const auto& l : svr2011::NetLinks()) {
    rtts += fmt::format("{}{}.{}: {:.0f} (worst {:.0f}){}", rtts.empty() ? "" : ", ", (l.peer >> 8) & 255,
                        l.peer & 255, l.rtt_ms, l.worst_ms, l.relayed ? " relay" : "");
  }
  std::string applied, pads;
  for (uint32_t k = 0; k < g_entries && k < kMaxEntries; ++k)
    applied += fmt::format("{}{}:{}", applied.empty() ? "" : " ", k, g_window.applied_pressed[k]);
  for (uint32_t p = 0; p < svr2011::kMaxPlayers; ++p)
    if (svr2011::PlayerPadType(p) != svr2011::PadType::kNone || g_window.pad_pressed[p])
      pads += fmt::format("{}{}:{}", pads.empty() ? "" : " ", p + 1, g_window.pad_pressed[p]);
  REXLOG_INFO("net: {:.1f} s, {} frames ({:.1f}/s), waited {} frames ({:.0f} ms) in {} waits, longest {:.0f} ms; "
              "waiting for entries [{}] (this game {} of {}); round trips ms [{}]; frames pressed: sent {} (player "
              "{}'s controller), applied [{}], this PC's players [{}]",
              secs, g_window.frames, g_window.frames / std::max(secs, 0.001), g_window.waited, g_window.waited_ms,
              g_window.waits, g_window.longest_ms, blame, g_local, g_entries, rtts, g_window.sent_pressed,
              g_send_port + 1, applied, pads);
  // (report 6dde2f: the player plays with one controller, the match takes another - the other games see this
  // wrestler stand still)
  if (!g_warned_pad && g_window.frames >= 200 && g_window.sent_pressed == 0 && g_send_port < svr2011::kMaxPlayers) {
    for (uint32_t p = 0; p < svr2011::kMaxPlayers; ++p)
      if (p != g_send_port && g_window.pad_pressed[p] >= 30) {
        REXLOG_WARN("net: player {}'s controller ({}) was used for {} frames, but this match takes this game's input "
                    "from player {}'s ({}) - the other players see this wrestler stand still (report 6dde2f)",
                    p + 1, PadName(p), g_window.pad_pressed[p], g_send_port + 1, PadName(g_send_port));
        g_warned_pad = true;
        break;
      }
  }
  g_now.active = true;
  g_now.waited_pct = g_window.frames ? float(100.0 * g_window.waited / g_window.frames) : 0.0f;
  g_now.longest_ms = float(g_window.longest_ms);
  g_now.waits = uint32_t(g_window.waits);
  g_window = {};
  g_window_start = now;
}

}  // namespace

// Debug: SVR2011_NET_INPUT_LOG=<file> (below).
FILE* InputLog() {
  static FILE* f = [] {
    const char* p = std::getenv("SVR2011_NET_INPUT_LOG");
    return p && *p ? std::fopen(p, "w") : nullptr;
  }();
  return f;
}

// The lockstep send, once a frame (r3 = the match net object).
REX_EXTERN(__imp__sub_82893BD0);
REX_HOOK_RAW(sub_82893BD0) {
  static const bool off = std::getenv("SVR2011_NET_STATS") && std::getenv("SVR2011_NET_STATS")[0] == '0';
  const uint32_t obj = ctx.r3.u32;
  if (!off && obj) {
    const uint8_t* o = base + obj;
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (g_window_start == Clock::time_point{}) g_window_start = now;
    g_entries = std::min(Rd32(o + kCount), kMaxEntries);
    g_local = Rd32(o + kLocal);  // (this game's lobby slot: its entry is the one with that slot at +4 - they differ with CPU slots)
    for (uint32_t k = 0; k < g_entries; ++k)
      if (Rd32(o + k * kEntrySize + 4) == Rd32(o + kLocal)) g_local = k;
    const bool waiting = Rd32(o + kWaiting) != 0;
    ++g_window.frames;
    if (waiting) {
      ++g_window.waited;
      if (!g_was_waiting) g_wait_start = now, ++g_window.waits;
      if (g_last_frame != Clock::time_point{})
        g_window.waited_ms += std::chrono::duration<double, std::milli>(now - g_last_frame).count();
      for (uint32_t k = 0; k < g_entries; ++k) {
        if (k != g_local && Rd32(o + k * kEntrySize + kGotInput) == 0) ++g_window.blamed[k];
      }
    } else if (g_was_waiting) {
      g_window.longest_ms =
          std::max(g_window.longest_ms, std::chrono::duration<double, std::milli>(now - g_wait_start).count());
    }
    {   // (report 6dde2f) who drives what: once a match, every entry's slot and person and this game's controller
      const uint32_t slot = Rd32(o + kLocal);
      const uint32_t ch = slot < 6 ? Rd32(base + kSlotCharacters + 4 * slot) : 0;
      g_send_port = ch ? Rd32(base + ch + 1152) : 0xFFFFFFFF;
      if (obj != g_told_net && ch) {   // (once its wrestler is there: before the match it isn't)
        g_told_net = obj;
        g_warned_pad = false;
        std::string entries;
        for (uint32_t k = 0; k < g_entries; ++k) {
          const uint32_t ks = Rd32(o + k * kEntrySize + 4);
          const uint32_t kc = ks < 6 ? Rd32(base + kSlotCharacters + 4 * ks) : 0;
          entries += fmt::format("{}{}: slot {}, person {}{}", entries.empty() ? "" : "; ", k, ks,
                                 kc ? int32_t(Rd32(base + kc + 1156)) : -1, k == g_local ? " (this game)" : "");
        }
        REXLOG_INFO("net: this game is entry {} of {} (slot {}); its wrestler is person {}, its input is player {}'s "
                    "controller ({}); entries: {}", g_local, g_entries, slot, ch ? int32_t(Rd32(base + ch + 1156)) : -1,
                    g_send_port + 1, PadName(g_send_port), entries);
      }
      const uint32_t pads = PadsPressed();
      for (uint32_t p = 0; p < svr2011::kMaxPlayers; ++p)
        if (pads >> p & 1) ++g_window.pad_pressed[p];
    }
    g_was_waiting = waiting;
    g_last_frame = now;
    g_last_sample_ms = NowMs();
    if (now - g_window_start >= std::chrono::seconds(5)) Report(now);
  }
  {
    svr2011::OnlineCpuPadScope pads(base, obj);  // (online.h: a human in a slot past the CPUs')
    __imp__sub_82893BD0(ctx, base);
  }
  if (!off && obj) {   // (the step check, online_chat.h: each time the match clock reaches a multiple of 300)
    static uint32_t last_clock = 0xFFFFFFFF;
    const uint32_t clock = svr2011::MatchClock(base);
    if (clock != last_clock && clock && clock % 300 == 0) {
      last_clock = clock;
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 255)) * 1099511628211ull;
      };
      auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
      std::string summary;
      for (const auto& m : svr2011::MatchPeople(base)) {
        mix(m.person), mix(m.motion), mix(bits(m.root_x)), mix(bits(m.root_y)), mix(bits(m.root_z));
        if (m.has_position) mix(bits(m.x)), mix(bits(m.y)), mix(bits(m.z));
        summary += fmt::format("{}{}:m{}@{:.1f},{:.1f}", summary.empty() ? "" : " ", m.person, m.motion, m.root_x,
                               m.root_z);
      }
      svr2011::OnlineMatchStep(clock, h, summary);
    }
  }
  if (!off && obj) {   // (the input just sent: the local entry's +48)
    const uint8_t* o = base + obj;
    std::lock_guard lock(g_mutex);
    if (g_local < kMaxEntries && Rd32(o + g_local * kEntrySize + 48) != 0) ++g_window.sent_pressed;
  }
  if (FILE* f = InputLog(); f && obj) {  // (the input just sent: the local entry's +48)
    const uint8_t* o = base + obj;
    for (uint32_t k = 0; k < std::min(Rd32(o + kCount), kMaxEntries); ++k)
      if (Rd32(o + k * kEntrySize + 4) == Rd32(o + kLocal))
        std::fprintf(f, "S %lld %08X\n", static_cast<long long>(NowMs()), Rd32(o + k * kEntrySize + 48));
  }
}

// Debug: SVR2011_NET_INPUT_LOG=<file> - one line a lockstep frame after the
// inputs are taken (sub_82893E18): ms, then per entry "slot:got own applied"
// (+4, +44, +48 the input sampled and sent, +52 the input this frame applies)
// - for measuring how many frames a press takes to apply, locally and remotely.
REX_EXTERN(__imp__sub_82893E18);
REX_HOOK_RAW(sub_82893E18) {
  const uint32_t net = ctx.r3.u32;   // (the match net object: r3 doesn't survive the call)
  {
    svr2011::OnlineCpuPadScope pads(base, ctx.r3.u32);  // (online.h)
    __imp__sub_82893E18(ctx, base);
  }
  if (net) {   // (each entry's input as this frame applies it: +52)
    const uint8_t* o = base + net;
    std::lock_guard lock(g_mutex);
    for (uint32_t k = 0; k < std::min(Rd32(o + kCount), kMaxEntries); ++k)
      if (Rd32(o + k * kEntrySize + 52) != 0) ++g_window.applied_pressed[k];
  }
  FILE* f = InputLog();
  const uint32_t obj = Rd32(base + kNetObject);
  if (!f || !obj) return;
  const uint8_t* o = base + obj;
  const uint32_t n = std::min(Rd32(o + kCount), kMaxEntries);
  std::string line = fmt::format("{} local {}", NowMs(), Rd32(o + kLocal));
  for (uint32_t k = 0; k < n; ++k) {
    const uint8_t* e = o + k * kEntrySize;
    line += fmt::format(" | {}:{} {:08X} {:08X}", Rd32(e + 4), Rd32(e + kGotInput), Rd32(e + 48), Rd32(e + 52));
  }
  std::fprintf(f, "%s\n", line.c_str());
}

// (with the input log: every packet the game hands its socket - the guest's
// sendto thunk sub_8298E3D8, r5 = its length)
REX_EXTERN(__imp__sub_8298E3D8);
REX_HOOK_RAW(sub_8298E3D8) {
  if (FILE* f = InputLog()) {  // (+ the caller and the first bytes)
    std::string head;
    for (uint32_t i = 0; i < std::min<uint32_t>(ctx.r5.u32, 24); ++i) head += fmt::format("{:02X}", base[ctx.r4.u32 + i]);
    std::fprintf(f, "P %lld %u %08X %s\n", static_cast<long long>(NowMs()), ctx.r5.u32, uint32_t(ctx.lr), head.c_str());
  }
  __imp__sub_8298E3D8(ctx, base);
}

// (with the input log: every packet the game reads - the recvfrom thunk
// sub_8298E3A0: r4 the buffer, r3 back the length)
REX_EXTERN(__imp__sub_8298E3A0);
REX_HOOK_RAW(sub_8298E3A0) {
  const uint32_t buf = ctx.r4.u32;
  __imp__sub_8298E3A0(ctx, base);
  FILE* f = InputLog();
  const int32_t n = ctx.r3.s32;
  if (!f || n <= 0) return;
  std::string head;
  for (int32_t i = 0; i < std::min<int32_t>(n, 24); ++i) head += fmt::format("{:02X}", base[buf + i]);
  std::fprintf(f, "R %lld %d %s\n", static_cast<long long>(NowMs()), n, head.c_str());
}

REXCVAR_DECLARE(bool, p2p_send_every_frame);  // (p2p.cpp)

// A player whose game is gone (closed, crashed, cut off) froze every other
// game ~15 s: gss drops a silent peer after its match timeout (session +744 =
// 15000 ms, set through sub_829B60C8 when the session enters state 7 - by
// sub_829B3A48 and sub_829B43C8; the lobby's +740 by sub_829B3918). The match
// timeout is now p2p_drop_seconds: the same drop (the game's own "has left and
// will be replaced by AI", or - the host - the session's end), sooner.
REXCVAR_DEFINE_UINT32(p2p_drop_seconds, 5, "Online",
                      "Seconds a silent player freezes an online match before the game drops them (3-15)");
REX_EXTERN(__imp__sub_829B60C8);
REX_HOOK_RAW(sub_829B60C8) {
  const uint32_t from = uint32_t(ctx.lr);
  if (from == 0x829B3A98 || from == 0x829B49E8) {  // (the match's timeout)
    const uint32_t ms = std::clamp<uint32_t>(REXCVAR_GET(p2p_drop_seconds), 3, 15) * 1000;
    if (ctx.r3.u32 > ms) {
      REXLOG_INFO("net: a silent player is dropped after {} s (the game's {} ms)", ms / 1000, ctx.r3.u32);
      ctx.r3.u64 = ms;
    }
  }
  __imp__sub_829B60C8(ctx, base);
}

// Lower lag on long links (p2p_send_every_frame). The game's lockstep (gss)
// has no frame numbers: each game queues every frame's input to every player
// and takes one from each player's queue a frame. Its input lead (frames from
// a press to its frame, the same for every input of that player) is set once
// before a match from the game's latency estimate (sub_829B3918: at least 6,
// then a send interval of lead / 3 frames); inputs then go out only every
// interval-th frame (the gate sub_829B4D30), and the lead has to cover that
// wait as well as the trip. Measured at 120 / 180 / 250 ms one way: 9 / 16 /
// 20 frames, presses applied 153 / 253 / 336 ms on; on a LAN 6 (87 ms).
// Here: every frame's input goes out at once (interval 1), and the lead is
// what the trip needs: the worst peer's measured one-way time + jitter + a
// margin, in frames (never more than the game's own; at least 3). Each game
// sets its own lead; gss gives its peers the frames to match.
REX_EXTERN(__imp__sub_829CAD08);
REX_HOOK_RAW(sub_829CAD08) {  // (the send interval getter: +3208, low byte)
  __imp__sub_829CAD08(ctx, base);
  if (REXCVAR_GET(p2p_send_every_frame) && ctx.r3.u32 > 1) ctx.r3.u64 = 1;
}

namespace {

// The lead for the worst peer measured: one way + jitter + 12 ms, in frames
// (0: no measurement yet).
uint32_t PingLead() {
  float one_way = -1, jitter = 0;
  for (const auto& l : svr2011::NetLinks()) {
    one_way = std::max(one_way, l.rtt_ms / 2);
    jitter = std::max(jitter, (l.worst_ms - l.rtt_ms) / 2);
  }
  if (one_way < 0) return 0;
  return uint32_t(std::ceil((one_way + jitter + 12.0f) / (1000.0f / 60.0f))) + 1;
}

}  // namespace

// gss's lead setter (sub_829CAD20: +2920, frames); before a match it's called
// from its "set latency" (sub_829B6018, lr 0x829B604C).
REX_EXTERN(__imp__sub_829CAD20);
REX_HOOK_RAW(sub_829CAD20) {
  // (test: SVR2011_TEST_NET_LEAD=<frames> - that lead instead)
  static const int test_lead = [] { const char* v = std::getenv("SVR2011_TEST_NET_LEAD"); return v ? std::atoi(v) : 0; }();
  const uint32_t asked = ctx.r4.u32;
  if (uint32_t(ctx.lr) == 0x829B604C) {
    if (test_lead > 0) {
      ctx.r4.u64 = uint32_t(test_lead);
    } else if (REXCVAR_GET(p2p_send_every_frame)) {
      if (const uint32_t lead = PingLead()) ctx.r4.u64 = std::clamp<uint32_t>(lead, 3, std::max<uint32_t>(asked, 3));
    }
    REXLOG_INFO("net: input lead {} frames (the game's {}){}", ctx.r4.u32, asked,
                REXCVAR_GET(p2p_send_every_frame) ? ", inputs sent every frame" : "");
  }
  __imp__sub_829CAD20(ctx, base);
}

namespace svr2011 {

NetStats NetStatsNow() {
  std::lock_guard lock(g_mutex);
  NetStats s = g_now;
  // (no lockstep frames for 2 s: not in an online match)
  s.active = s.active && NowMs() - g_last_sample_ms.load() < 2000;
  return s;
}

}  // namespace svr2011
