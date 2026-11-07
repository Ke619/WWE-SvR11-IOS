// WWE SmackDown vs. Raw 2011 - MYSTERY OPPONENT (see mystery_opponent.h).
//
// A normal one on one (rule 0x00). The select screen still has its two
// panels (no match rule has a select layout of one pick), but the opponent's
// is never really chosen:
// - the player picks their superstar as usual; then the COM panel completes
//   by itself (B26: its cursor's A handler, sub_82463FD0 state 4, played
//   twice at once - pick and attire - whatever the COM cursor is on; the
//   cursor's per-frame update sub_82465038 starts it);
// - a "?" covers the COM panel's model and name (select screen) and the
//   opponent's model, name and the ticker (VS screen, then the loading screen);
// - when the people are built (sub_828BBEF0) person 1's slot gets the hidden
//   pick (CPU, team 1, the first attire);
// - the COM cursor's own pick can land in a later slot (a third wrestler):
//   slots 2-5 are emptied;
// - entrances are on for the match (live rules +38), the opponent enters
//   last (sub_828C3AC8's team order) and the controller is held from the
//   first entrance to the bell (nothing skips them): the reveal is the
//   opponent's entrance. The "?" goes when the entrances start (at the
//   latest: the match running, the entrances' end, the bell).
//
// The pick: any selectable superstar (the roster records), the installed
// superstar mods, the managers with the M tile on; never the player's own
// pick (by id, name or same person), the player's gender unless
// mixed_gender_matches is on. Test aids: SVR2011_TEST_MYSTERY=<id> picks that
// id; SVR2011_TEST_SELECT_LOG=1 logs the select cursors' A presses;
// SVR2011_TEST_MYSTERY_NO_EVENT=1 ignores the entrances' sound events (the
// reveal's fallbacks).

#include "big_roster.h"
#include "mystery_opponent.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "rtwm_streak.h"
#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr uint32_t kRule = 0x00;  // ONE ON ONE -> NORMAL
constexpr uint32_t kLive = 0x82E3DE00;  // the live rules (+0: the rule)
constexpr uint32_t kSlots = 432, kSlotSize = 2116;
constexpr uint32_t kSelect = 0x82EDE630;  // the select manager: +13520 page, panels +120 + page * 1352 + i * 204
constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kOwnId = 32, kName = 34, kGender = 208, kSelectable = 221, kSamePerson = 228, kDlc = 257;
constexpr uint32_t kManagers[] = {147, 186, 251, 296, 274};  // (managers.cpp)
// The select cursor (sub_82463FD0's object is cursor + 44): its state, the
// player it picks for.
constexpr uint32_t kCursorState = 4904, kCursorPlayer = 240;
constexpr uint32_t kStatePick = 4, kStateAttire = 6;

using Clock = std::chrono::steady_clock;

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32, svrfmt::Wr16;

uint8_t* g_base = nullptr;
std::atomic<bool> g_on{false};
uint32_t g_pick = 0;                     // the opponent (0: not chosen yet)
std::atomic<int64_t> g_select_seen{0};  // the select screen's last frame (ms)
std::atomic<bool> g_player_ready{false};  // the player's panel is decided
std::atomic<bool> g_vs{false};           // the COM panel done: the VS screen (under it the select's panels still draw)
std::atomic<bool> g_loading{false};      // the people built: the loading screen, until the entrances
std::atomic<int64_t> g_vs_since{0};      // (ms) the COM panel's pick
std::atomic<int64_t> g_entrances_since{0};  // (ms) the entrances started (0: not now) - until the bell
std::atomic<uint32_t> g_player_id{0};    // the player's pick (the VS screen's name)
std::atomic<bool> g_auto_done{false};
std::atomic<bool> g_online{false};         // an online MYSTERY OPPONENT match (MysteryOpponentOnline)      // the COM panel completed for the player (B26)

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

const uint8_t* Record(uint8_t* base, uint32_t id) {
  if (id == 0 || id >= 1000) return nullptr;
  const uint32_t index = svr2011::RosterIndex(base, id);
  if (index >= 1000) return nullptr;
  const uint8_t* rec = base + svr2011::RosterEntry(kRecords, kRecordSize, index);
  return Rd16(rec + kOwnId) == id ? rec : nullptr;
}

std::string Name(const uint8_t* rec) {
  return rec ? std::string(reinterpret_cast<const char*>(rec + kName), strnlen(reinterpret_cast<const char*>(rec + kName), 32))
             : std::string();
}

// The same superstar as `player` (the id, the name, the same person).
bool SamePerson(uint8_t* base, uint32_t id, uint32_t player) {
  const uint8_t* a = Record(base, id);
  const uint8_t* b = Record(base, player);
  if (id == player) return true;
  if (!a || !b) return false;
  return Name(a) == Name(b) || (Rd16(a + kSamePerson) && Rd16(a + kSamePerson) == Rd16(b + kSamePerson)) ||
         Rd16(a + kSamePerson) == player || Rd16(b + kSamePerson) == id;
}

uint32_t Choose(uint8_t* base, uint32_t player) {
  if (const char* v = std::getenv("SVR2011_TEST_MYSTERY")) {
    const uint32_t id = uint32_t(std::atoi(v));
    if (Record(base, id)) return id;
  }
  const uint8_t* me = Record(base, player);
  const bool any_gender = rex::cvar::GetFlagByName("mixed_gender_matches") != "false";
  auto fits = [&](uint32_t id) {
    const uint8_t* rec = Record(base, id);
    return rec && !SamePerson(base, id, player) && (any_gender || !me || rec[kGender] == me[kGender]);
  };
  std::vector<uint32_t> pool;
  for (uint32_t id = 1; id < 1000; ++id) {
    const uint8_t* rec = Record(base, id);
    if (rec && rec[kSelectable] == 1 && !rec[kDlc] && fits(id)) pool.push_back(id);
  }
  for (uint32_t id : svr2011::SuperstarModIds(false))
    if (fits(id) && std::find(pool.begin(), pool.end(), id) == pool.end()) pool.push_back(id);
  if (rex::cvar::GetFlagByName("managers_tile") != "false")
    for (uint32_t id : kManagers)
      if (fits(id) && std::find(pool.begin(), pool.end(), id) == pool.end()) pool.push_back(id);
  if (pool.empty()) return 0;
  static std::mt19937 rng{std::random_device{}()};
  return pool[rng() % pool.size()];
}

// The "?": over the COM panel on the select screen; over the opponent's
// model, name and the ticker on the VS screen and the loading screen after it.
class Hidden final : public rex::ui::ImGuiDialog {
 public:
  explicit Hidden(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_on.load() || !g_base) return;
    // (the VS layout from the COM panel's pick to the entrances - the VS and
    // loading screens; the select layout while its panels draw)
    const bool vs = g_vs.load() || g_loading.load();
    const bool select = !vs && NowMs() - g_select_seen.load() < 300;
    if (!select && !vs) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float s = gh / 720.0f, ox = (w - gw) * 0.5f, oy = (h - gh) * 0.5f;
    auto P = [&](float fx, float fy) { return ImVec2(ox + fx * gw, oy + fy * gh); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 kDark = IM_COL32(8, 8, 12, 250), kRed = IM_COL32(200, 30, 32, 255), kWhite = IM_COL32(255, 255, 255, 255);
    auto centred = [&](float size, ImVec2 a, ImVec2 b, const char* text, ImU32 colour) {
      const ImVec2 z = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      dl->AddText(font, size, ImVec2((a.x + b.x - z.x) * 0.5f, (a.y + b.y - z.y) * 0.5f), colour, text);
    };
    auto question = [&](ImVec2 a, ImVec2 b) {
      dl->AddRectFilledMultiColor(a, b, kDark, IM_COL32(28, 6, 8, 250), IM_COL32(28, 6, 8, 250), kDark);
      const float qs = std::min(b.y - a.y, b.x - a.x) * 0.9f;
      centred(qs, ImVec2(a.x + 5 * s, a.y + 5 * s), ImVec2(b.x + 5 * s, b.y + 5 * s), "?", IM_COL32(0, 0, 0, 200));
      centred(qs, a, b, "?", kRed);
    };
    if (select) {
      // The COM panel (picked by itself: B26): a "?" over its model and its
      // name - only those, in the panel's own look (no plate of ours).
      question(P(0.60f, 0.03f), P(0.86f, 0.315f));
      const ImVec2 a = P(0.522f, 0.318f), b = P(0.852f, 0.372f);
      dl->AddRectFilled(a, b, IM_COL32(12, 12, 16, 255), 3 * s);
      centred(32 * s, a, b, "?", kWhite);
    } else {
      // The VS screen: the opponent's model, their half of the names, the ticker.
      question(P(0.50f, 0.08f), P(0.82f, 0.515f));
      const ImVec2 a = P(0.50f, 0.555f), b = P(0.84f, 0.70f);  // (down over the names line: "VS. <name>" showed under it)
      dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
      centred(44 * s, a, b, "MYSTERY OPPONENT", kWhite);
      dl->AddRectFilled(P(0.135f, 0.79f), P(0.80f, 0.858f), IM_COL32(24, 24, 28, 255));
      const std::string me = Name(Record(g_base, g_player_id.load()));
      const std::string ticker = me.empty() ? "MAIN EVENT - MYSTERY OPPONENT" : "MAIN EVENT - " + me + " vs. MYSTERY OPPONENT";
      centred(24 * s, P(0.135f, 0.79f), P(0.80f, 0.858f), ticker.c_str(), IM_COL32(255, 200, 60, 255));
    }
  }
};

}  // namespace

namespace svr2011 {

uint32_t MysteryOpponentString(uint32_t id, rex::memory::Memory* memory) {
  static const char* const kText[] = {
      "MYSTERY OPPONENT",
      "Pick your Superstar - your opponent is a surprise, revealed only when they make their entrance."};
  static uint32_t text[2] = {};
  if ((id != kMysteryLabel && id != kMysteryText) || !memory) return 0;
  const uint32_t k = id - kMysteryLabel;
  if (!text[k]) {
    const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
    text[k] = memory->SystemHeapAlloc(n);
    if (text[k]) std::memcpy(memory->TranslateVirtual<char*>(text[k]), kText[k], n);
  }
  return text[k];
}

void MysteryOpponentSetup(uint8_t* base, bool on) {
  g_base = base;
  g_on = on;
  g_pick = 0;
  g_vs = false;
  g_loading = false;
  g_entrances_since = 0;
  g_player_ready = false;
  g_player_id = 0;
  g_auto_done = false;
  if (on) REXLOG_INFO("mystery opponent: on");
}

bool MysteryOpponentMatch() { return g_on.load(); }

void MysteryOpponentOnline(bool on) {
  if (g_online.exchange(on) != on && on) REXLOG_INFO("mystery opponent: online - the lobby's CPU enters last");
}

void MysteryOpponentFill(uint8_t* base, uint32_t match) {
  if (!g_on.load() || !match || base[kLive] != kRule) return;
  uint8_t* me = base + match + kSlots;
  uint8_t* slot = base + match + kSlots + kSlotSize;
  const uint32_t player = Rd32(me + 8) / 100;
  if (!g_pick || SamePerson(base, g_pick, player)) {
    g_pick = Choose(base, player);
    if (!g_pick) {
      REXLOG_WARN("mystery opponent: nobody to choose from");
      return;
    }
    REXLOG_INFO("mystery opponent: {} (id {})", Name(Record(base, g_pick)), g_pick);
  }
  // (the COM panel's pick, whoever it was, becomes the hidden one)
  Wr32(slot + 8, g_pick * 100 + 2);  // (attire: the first, as the select screen gives)
  Wr16(slot + 54, uint16_t(g_pick));
  slot[5] = 1;   // (team)
  slot[4] = 0;   // (a wrestler)
  slot[-8] = 1;  // (controller: the CPU)
  // (the COM cursor's own pick can land in a slot after it - a third
  // wrestler: the match is a one on one, every later slot empty)
  for (uint32_t i = 2; i < 6; ++i) {
    uint8_t* other = base + match + kSlots + i * kSlotSize;
    if (Rd32(other + 8) != 51200) {
      REXLOG_INFO("mystery opponent: slot {} emptied ({})", i, Name(Record(base, Rd32(other + 8) / 100)));
      Wr32(other + 8, 51200);
    }
  }
  if (g_vs.load()) g_loading = true;
}

void MysteryOpponentEvent(const char* e) {
  if (!g_on.load()) return;
  if (!std::strncmp(e, "Play_Bgn_match_bell", 19)) {
    MysteryOpponentReveal("the bell");
    if (g_entrances_since.exchange(0)) REXLOG_INFO("mystery opponent: the bell - the controller back");
    return;
  }
  if (!g_loading.load() && !g_vs.load()) return;
  static const bool no_event = std::getenv("SVR2011_TEST_MYSTERY_NO_EVENT") != nullptr;  // (tests: the fallbacks)
  if (no_event) return;
  if (std::strncmp(e, "Play_MUS_", 9) != 0 && std::strncmp(e, "Play_Ent_", 9) != 0) return;
  g_vs = false;
  g_loading = false;
  g_entrances_since = NowMs();
  REXLOG_INFO("mystery opponent: the entrances start ({:.40}) - revealed; no skipping", e);
}

void MysteryOpponentReveal(const char* why) {
  if (!g_on.load()) return;
  const bool vs = g_vs.exchange(false), loading = g_loading.exchange(false);
  if (vs || loading) REXLOG_INFO("mystery opponent: revealed ({}) - the \"?\" goes", why);
}

bool MysteryOpponentHoldsInput() {
  const int64_t since = g_entrances_since.load();
  return since && g_on.load() && NowMs() - since < 4 * 60 * 1000;  // (4 minutes at most)
}

void MysteryOpponentLive(uint8_t* base, uint32_t live) {
  if (!g_on.load() || !live) return;
  // (entrances on: the reveal. A standard-ring one on one with entrances on
  // already has 1 here as built, so this changes nothing then)
  base[live + 38] = 1;
}

void MysterySelectFrame(uint8_t* base) {
  if (!g_on.load()) return;
  g_select_seen = NowMs();
  // (the panels drawn again well after the COM pick: back from the VS screen)
  if (g_vs.load() && !g_loading.load() && NowMs() - g_vs_since.load() > 1500) {
    g_vs = false;
    REXLOG_INFO("mystery opponent: back on the select screen");
  }
  const uint32_t mgr = Rd32(base + kSelect);
  if (!mgr) return;
  const uint32_t page = Rd32(base + mgr + 13520);
  if (page > 8) return;
  const uint8_t* panel = base + mgr + 120 + page * 1352;  // (the player's)
  g_player_ready = Rd32(panel + 0xB8) == 1;
  g_player_id = Rd32(panel);
}

void InstallMysteryOpponentOverlay(rex::ui::ImGuiDrawer* drawer) { new Hidden(drawer); }

}  // namespace svr2011

// The character select cursor's A button: sub_82463FD0(cursor + 44), by the
// cursor's state (4: on the grid - picks the character under it and goes to
// 6, the attire; 6: confirms). For a MYSTERY OPPONENT match the COM panel's
// cursor (player 1) does both at once on its first A: the pick is never
// seen or used (sub_828BBEF0 puts the hidden one in its place).
REX_EXTERN(__imp__sub_82463FD0);
REX_HOOK_RAW(sub_82463FD0) {
  const uint32_t self = ctx.r3.u32;
  if (g_on.load()) {
    const uint32_t state = Rd32(base + self + kCursorState), player = Rd32(base + self + kCursorPlayer);
    static const bool log = std::getenv("SVR2011_TEST_SELECT_LOG") != nullptr;
    if (log) REXLOG_INFO("mystery opponent: select cursor {:08X} state {} player {}", self - 44, state, player);
    if (player == 1 && state == kStatePick) {
      g_vs = false;
      const auto saved = ctx;
      __imp__sub_82463FD0(ctx, base);
      if (Rd32(base + self + kCursorState) == kStateAttire) {
        ctx = saved;
        __imp__sub_82463FD0(ctx, base);
      }
      g_vs = true;
      g_vs_since = NowMs();
      REXLOG_INFO("mystery opponent: the COM panel done (cursor state {})", Rd32(base + self + kCursorState));
      return;
    }
  }
  __imp__sub_82463FD0(ctx, base);
}

// Entrance order: sub_828C3AC8(order, ...) turns the match's teams into
// entrance units in the order order[team] gives (int32 x 6; the lowest
// enters first - an identity order for an exhibition). For a MYSTERY
// OPPONENT match the opponent's team comes last: the reveal ends the
// entrances.
REX_EXTERN(__imp__sub_828C3AC8);
REX_HOOK_RAW(sub_828C3AC8) {
  // (online: the hidden one is the match's CPU - its match slot's controller,
  // [-8], is 1; the others are the players)
  int hidden = 1;
  if (g_online.load())
    if (const uint32_t match = Rd32(base + 0x82EDEB48)) {
      hidden = -1;
      for (int i = 0; i < 6; ++i)
        if (base[match + kSlots + uint32_t(i) * kSlotSize - 8] == 1) hidden = i;
    }
  if (((g_on.load() && base[kLive] == kRule) || (g_online.load() && hidden >= 0)) && ctx.r3.u32) {
    const uint32_t order = ctx.r3.u32, people = Rd32(base + 0x82E3DDFC);
    if (people) {
      // (the last-entering team: the one with the highest order - swapped with the hidden one's)
      int last = 0;
      for (int k = 0; k < 6; ++k)
        if (int32_t(Rd32(base + order + uint32_t(k) * 4)) > int32_t(Rd32(base + order + uint32_t(last) * 4))) last = k;
      const int8_t me = g_online.load() ? int8_t(last) : int8_t(base[people + 132 + 0 * 52 + 13]),
                   them = int8_t(base[people + 132 + uint32_t(hidden) * 52 + 13]);
      if (me >= 0 && me < 6 && them >= 0 && them < 6 && me != them) {
        const int32_t a = int32_t(Rd32(base + order + me * 4)), b = int32_t(Rd32(base + order + them * 4));
        if (a >= 0 && b >= 0 && b < a) {
          Wr32(base + order + me * 4, uint32_t(b));
          Wr32(base + order + them * 4, uint32_t(a));
          REXLOG_INFO("mystery opponent: the opponent enters last");
        }
      }
    }
  }
  __imp__sub_828C3AC8(ctx, base);
}

// The select cursor's per-frame update: sub_82465038(cursor) (its state at
// +4948, its player at +284, a list or roulette open at +28916, the screen at
// +272 - its fade at +92, 255 when the screen takes input). For a MYSTERY
// OPPONENT match the COM panel is completed by itself once the player's own
// pick is decided (B26: the opponent can't be chosen): its A handler is run
// as if A was pressed (above: pick and attire at once) - whatever superstar
// the cursor rests on, the hidden pick takes its place (sub_828BBEF0).
// Backing out of the COM panel (the player's panel open again) arms it again.
REX_EXTERN(__imp__sub_82465038);
REX_HOOK_RAW(sub_82465038) {
  const uint32_t cursor = ctx.r3.u32;
  const bool mod_shown = svr2011::RtwmShowModBegin(base);  // (RTWM's Streak list: its panel shows the mod)
  __imp__sub_82465038(ctx, base);
  svr2011::RtwmShowModEnd(base, mod_shown);
  if (!g_on.load() || !cursor) return;
  if (!g_player_ready.load()) {
    g_auto_done = false;
    return;
  }
  if (g_auto_done.load() || Rd32(base + cursor + 284) != 1 || Rd32(base + cursor + 4948) != kStatePick ||
      Rd32(base + cursor + 28916) != 0)
    return;
  const uint32_t screen = Rd32(base + cursor + 272);
  if (!screen) return;
  const uint32_t fade_bits = Rd32(base + screen + 92);
  float fade;
  std::memcpy(&fade, &fade_bits, 4);
  if (!(fade >= 255.0f)) return;  // (the screen not taking input yet: next frame)
  g_auto_done = true;
  REXLOG_INFO("mystery opponent: the COM panel completed for the player");
  const auto saved = ctx;
  ctx.r3.u64 = cursor + 44;
  sub_82463FD0(ctx, base);  // (the hooked A handler: pick and attire)
  ctx = saved;
}
