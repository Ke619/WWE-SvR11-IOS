// Online PORT RULES - see online_rules.h.
//
// The CREATE SESSION screen (class vtable 0x82023D50: made by sub_824EAD88,
// freed by sub_824EBB78 when the session is created): sub_824EBFF0 runs each
// frame of its life, sub_824EAAF8 twice a frame while it is on top (once
// while the MATCH TYPE list is open over it). The match picked in the game's
// MATCH TYPE list is the rule the screen reads its name for (the rule-name
// helper sub_824CCB38, called from 0x824EB3B8 as the screen is made and when
// the list returns). The lobby panel shows while a session's port rules are
// set and no CREATE SESSION is up.
// (Found by tracing: docs/MATCH_TYPES_RESEARCH.md.)
#include "online_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "match_hud.h"
#include "match_types.h"
#include "frame_rate.h"
#include "p2p.h"
#include "util.h"

namespace {

using svr2011::NowMs;

enum Mode : uint32_t { kNone = 0, kWeapons = 1, kStages = 2, kTablesLadders = 3, kWeaponsInside = 4, kMystery = 5, kGauntletElim = 6, kSlobber = 7, kRoof = 8 };
struct Option {
  Mode mode;
  int set;
  const char* name;
  const char* about;
};
constexpr Option kOff{kNone, 0, "NONE", "The match as the game has it."};
constexpr Option kWe{kWeapons, 0, "WEAPONS EVERYWHERE", "Extreme Rules with weapons lying in and around the ring."};
constexpr Option kStage[] = {
    {kStages, 0, "THREE STAGES: NORMAL / FCA / LMS", "First to two falls: Normal, Falls Count Anywhere, Last Man Standing."},
    {kStages, 1, "THREE STAGES: NORMAL / BLOOD / FINISHER", "First to two falls: Normal, First Blood, Finishers."},
    {kStages, 2, "THREE STAGES: NORMAL / LMS / SUBMISSION", "First to two falls: Normal, Last Man Standing, Submission."},
    {kStages, 4, "THREE STAGES: CELL BLOOD / LMS / SUB", "Inside the cell: First Blood, Last Man Standing, Submission."},
    {kStages, 5, "THREE STAGES: TABLE / LADDER / TABLE", "First to two falls: Table, Ladder (the prize), Table."},
    {kStages, 3, "THREE STAGES: TABLE / TLC / LADDER", "First to two falls: Table, TLC (the prize), Ladder (the prize)."}};
constexpr Option kTl{kTablesLadders, 0, "TABLES & LADDERS", "Tables, ladders and chairs lying inside the cell."};
constexpr Option kInside{kWeaponsInside, 0, "WEAPONS INSIDE", "Chairs and other weapons lying in the ring."};
constexpr Option kMysteryOpt{kMystery, 0, "MYSTERY OPPONENT",
                             "A hidden CPU superstar fills the last slot - revealed at their entrance."};
constexpr const char* kMysteryName = "MYSTERY OPPONENT";
constexpr Option kGeOpt{kGauntletElim, 0, "GAUNTLET ELIMINATOR",
                        "Two start; four CPU entrants come in one a minute. Last one standing wins."};
constexpr Option kRoofOpt{kRoof, 0, "START ON THE ROOF", "The match starts on top of the cell: no pins up there."};
constexpr Option kSkOpt{kSlobber, 0, "SLOBBER KNOCKER",
                        "You two as a team against a line of CPU opponents, one at a time, until one of you is beaten."};

// The mode's normal matches (one on one, two on two, 3-man, 4-man) and
// their Extreme Rules ones; 6-man tag and handicap have no Extreme Rules
// rule (Weapons Everywhere plays them as they are).
constexpr uint32_t kNormal[] = {0x00, 0x03, 0x0D, 0x0E}, kExtreme[] = {0x4D, 0x4E, 0x4F, 0x50};

std::vector<Option> OptionsFor(uint32_t rule) {
  std::vector<Option> o{kOff};
  const bool normal = std::find(std::begin(kNormal), std::end(kNormal), rule) != std::end(kNormal);
  if (normal || rule == 0x08 || rule == 0x0F) {
    o.push_back(kWe);
    // (not NORMAL / BLOOD / FINISHER: a FIRST BLOOD fall froze the match - match_types.cpp SetsFor)
    for (int k : {0, 2}) o.push_back(kStage[k]);
    if (rule == 0x0D) o.push_back(kMysteryOpt);  // (TRIPLE THREAT: 2 players + the hidden CPU)
    if (rule == 0x08) o.push_back(kGeOpt);       // (6-MAN: 2 players + 4 CPU entrants)
    if (rule == 0x08) o.push_back(kSkOpt);       // (6-MAN: the 2 players against 4 CPUs in turn)
  } else if (rule >= 0x4D && rule <= 0x50) {
    o.push_back(kWe);
  } else if (rule >= 0x48 && rule <= 0x4B) {  // (TLC: the table / ladder sets, played in it)
    o.push_back(kStage[4]);  // (as the other PORT RULES options: not by the switch mods - both sides must agree)
    o.push_back(kStage[5]);
  } else if (rule >= 0x31 && rule <= 0x34) {
    o.push_back(kTl);  // (not the cell's BLOOD / LMS / SUB: its FIRST BLOOD fall froze the match)
    o.push_back(kRoofOpt);
  } else if ((rule >= 0x3C && rule <= 0x3F) || rule == 0x51) {
    o.push_back(kInside);
  }
  return o;
}
uint32_t Value(const Option& o) { return o.mode == kNone ? 0 : uint32_t(o.mode) << 16 | uint32_t(o.set); }

std::atomic<int64_t> g_cs_at{0};                  // ms: CREATE SESSION last on top
std::atomic<bool> g_cs_alive{false};             // CREATE SESSION exists
std::atomic<int> g_cs_calls{0};                  // sub_824EAAF8 calls this frame
std::atomic<uint32_t> g_cs_rule{~0u};             // the match picked on CREATE SESSION
std::atomic<int> g_choice{0};                     // its row's option (index)
std::atomic<int> g_step{0};                       // presses / clicks to apply (-1 / +1)
uint32_t g_set_value = ~0u;                       // last given to SetPortRules
uint16_t g_last_buttons = 0;

bool CreateSessionShown() { return NowMs() - g_cs_at.load() < 300; }

// The picked match (CREATE SESSION's name read).
uint32_t PickedRule() { return g_cs_rule.load(); }
std::string Describe(uint32_t value) {
  for (uint32_t rule : {0x00u, 0x31u, 0x3Cu, 0x0Du, 0x08u, 0x48u})
    for (const Option& o : OptionsFor(rule))
      if (Value(o) == value) return o.name;
  return "UNKNOWN";
}

// The row, the lobby panel; the choice handed to the session.
class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 private:
  bool shown_ = false;

 protected:
  void OnDraw(ImGuiIO& io) override {
    // (rule not known yet: NONE only; never in Ranked - its sessions carry no port rules)
    const bool row = CreateSessionShown() && !svr2011::P2PRanked().value_or(false);
    const uint32_t rule = PickedRule();
    if (row != shown_) {
      shown_ = row;
      REXLOG_INFO("online rules: the PORT RULES row {} (rule {:02X})", row ? "shown" : "hidden", rule);
    }
    if (row) {
      const std::vector<Option> options = OptionsFor(rule);
      int choice = std::clamp(g_choice.load(), 0, int(options.size()) - 1);
      if (const int step = g_step.exchange(0); step && options.size() > 1)
        choice = (choice + step + int(options.size())) % int(options.size());
      if (options.size() == 1) choice = 0;
      g_choice = choice;
      const Option& o = options[size_t(choice)];
      const svr2011::HudRowHits hits = svr2011::DrawHudRow(
          io.DisplaySize.x, io.DisplaySize.y, 0.715f, options.size() > 1 ? "PORT RULES  LB/RB" : "PORT RULES", o.name, "", "");
      // (mouse / touch: the arrows)
      if (ImGui::IsMouseClicked(0) && options.size() > 1) {
        const ImVec2 m = io.MousePos;
        const auto in = [&](const float (&r)[4]) { return m.x >= r[0] && m.x <= r[2] && m.y >= r[1] && m.y <= r[3]; };
        if (in(hits.left)) g_step = -1;
        if (in(hits.right)) g_step = 1;
      }
      const uint32_t value = Value(o);
      if (value != g_set_value) {
        g_set_value = value;
        svr2011::SetPortRules(value);
        REXLOG_INFO("online rules: PORT RULES {} ({:08X}) for rule {:02X}", o.name, value, rule);
      }
      return;
    }
    if (!g_cs_alive.load() && !svr2011::InMatch())
      if (const auto v = svr2011::PortRules(); v && *v) {
        svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, "PORT RULES", Describe(*v));
        if (Mode(*v >> 16) == kMystery) MysteryCovers(io);
      }
  }

  // MYSTERY OPPONENT in the lobby: the hidden CPU's slot box (its portrait and
  // name come from its superstar) and, with the cursor on it, the SUPERSTAR
  // INFO panel get a "?" - the 3-slot (TRIPLE THREAT) layout; not while the
  // superstar list is open (states 5, 6).
  void MysteryCovers(ImGuiIO& io) {
    uint32_t state = 0, cursor = 0, lobby = 0;
    uint8_t* base = nullptr;
    if (!svr2011::OnlineLobbyView(&state, &cursor, &base, &lobby) || state == 5 || state == 6) return;
    const uint32_t slots = svr2011::OnlineLobbySlots(base, lobby);
    if (slots != 3) return;
    int hidden = -1;
    for (uint32_t j = 0; j < slots; ++j)
      if (svr2011::OnlineLobbyIsCpu(base, lobby, j)) hidden = int(j);  // (in a MYSTERY OPPONENT session every CPU is hidden)
    if (hidden < 0) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f, s = gh / 720.0f;
    const float ox = (w - gw) * 0.5f, oy = (h - gh) * 0.5f;
    auto P = [&](float fx, float fy) { return ImVec2(ox + fx * gw, oy + fy * gh); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    auto cover = [&](ImVec2 a, ImVec2 b, const char* text, float size) {
      dl->AddRectFilled(a, b, IM_COL32(12, 12, 16, 255), 4 * s);
      dl->AddRect(a, b, IM_COL32(235, 235, 240, 255), 4 * s, 0, 2 * s);
      const ImVec2 z = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      dl->AddText(font, size, ImVec2((a.x + b.x - z.x) * 0.5f, (a.y + b.y - z.y) * 0.5f), IM_COL32(255, 255, 255, 255), text);
    };
    // (the slot boxes: 1P top, 2P left, 3P right)
    constexpr float kBox[3][4] = {{0.348f, 0.218f, 0.652f, 0.336f}, {0.161f, 0.383f, 0.466f, 0.501f}, {0.534f, 0.383f, 0.838f, 0.501f}};
    const float* k = kBox[hidden];
    cover(P(k[0], k[1]), P(k[2], k[3]), "? MYSTERY OPPONENT", 26 * s);
    if (cursor == uint32_t(hidden)) cover(P(0.129f, 0.593f), P(0.873f, 0.843f), "?", 80 * s);
  }
};

}  // namespace

namespace svr2011 {

void InstallOnlineRulesOverlay(rex::ui::ImGuiDrawer* drawer) { new Overlay(drawer); }

void OnlineRulesPad(uint16_t buttons) {
  const uint16_t pressed = buttons & ~g_last_buttons;
  g_last_buttons = buttons;
  if (!CreateSessionShown()) return;
  if (pressed & 0x0100) g_step = -1;  // LB
  if (pressed & 0x0200) g_step = 1;   // RB
}

// MYSTERY OPPONENT: the host fills the lobby's last vacant slot with a CPU
// (online_cpu.cpp: a stock superstar nobody has, its superstar block built
// and sent) - once: with a CPU in the lobby nothing is rolled again, and a
// joiner (early or late) gets the record. Every machine covers the CPU's
// slot (MysteryCovers).
void OnlineRulesLobby(PPCContext& ctx, uint8_t* base, uint32_t lobby) {
  const auto v = PortRules();
  if (!v) return;
  const uint32_t slots = OnlineLobbySlots(base, lobby);
  auto vacant_slot = [&](uint32_t j) {
    const uint8_t* rec = OnlineLobbyRecord(base, lobby, j);
    return (uint32_t(rec[0]) << 24 | uint32_t(rec[1]) << 16 | uint32_t(rec[2]) << 8 | rec[3]) < 5;
  };
  // GAUNTLET ELIMINATOR (6-MAN): slots 2-5 the CPU entrants - each vacant one
  // made a CPU once (a CPU slot is never rolled again); slots 0-1 the players.
  if (Mode(*v >> 16) == kGauntletElim || Mode(*v >> 16) == kSlobber) {  // (SLOBBER KNOCKER: the opponents, the same way)
    for (uint32_t j = 2; j < slots; ++j)
      if (vacant_slot(j)) {
        REXLOG_INFO("online rules: GAUNTLET ELIMINATOR - CPU entrant in slot {}", j);
        OnlineLobbyMakeCpu(ctx, base, lobby, j, nullptr);
      }
    return;
  }
  if (Mode(*v >> 16) != kMystery) return;
  int vacant = -1;
  for (uint32_t j = 0; j < slots; ++j) {
    const uint8_t* rec = OnlineLobbyRecord(base, lobby, j);
    // (a CPU there: rolled already - its record's names are the superstar's
    // once its block is built, so any CPU counts)
    if (OnlineLobbyIsCpu(base, lobby, j)) return;
    const uint32_t state = uint32_t(rec[0]) << 24 | uint32_t(rec[1]) << 16 | uint32_t(rec[2]) << 8 | rec[3];
    if (state < 5) vacant = int(j);
  }
  if (vacant < 0) return;  // (every slot taken by players)
  REXLOG_INFO("online rules: MYSTERY OPPONENT - the hidden CPU in slot {}", vacant);
  OnlineLobbyMakeCpu(ctx, base, lobby, uint32_t(vacant), kMysteryName);
}

OnlineRulesSetup OnlineRulesFor(uint32_t rule) {
  OnlineRulesSetup s;
  // (not while CREATE SESSION is up: the MATCH TYPE list sets up matches as it's browsed)
  if (g_cs_alive.load()) return s;
  const auto v = PortRules();
  if (!v || !*v) return s;
  const Mode mode = Mode(*v >> 16);
  const int set = int(*v & 0xFFFF);
  bool valid = false;
  for (const Option& o : OptionsFor(rule)) valid = valid || (o.mode == mode && o.set == set && mode != kNone);
  if (!valid) {
    REXLOG_WARN("online rules: {:08X} doesn't fit rule {:02X} - played as it is", *v, rule);
    return s;
  }
  s.on = true;
  s.rule = rule;
  s.set = set;
  if (mode == kWeapons) {
    s.weapons = true;
    for (int i = 0; i < 4; ++i)
      if (rule == kNormal[i]) s.rule = kExtreme[i];  // (Weapons Everywhere: the Extreme Rules match)
  } else if (mode == kStages) {
    s.stages = true;
  } else if (mode == kMystery) {
    s.mystery = true;  // (the lobby's CPU is the opponent: nothing to change in the rule)
  } else if (mode == kRoof) {
    s.roof = true;  // (the cell's match itself: everyone on its roof at the bell, match_types.cpp)
  } else if (mode == kSlobber) {
    s.slobber = true;
    s.rule = 0x52;  // (the gauntlet, set up as SLOBBER KNOCKER: match_types.cpp)
  } else if (mode == kGauntletElim) {
    s.ge = true;
    s.rule = 0x52;  // (the gauntlet, set up as GAUNTLET ELIMINATOR: match_types.cpp)
  } else {
    s.weapons = true;  // (Tables & Ladders, Weapons Inside: the structure's weapons)
  }
  REXLOG_INFO("online rules: {} - rule {:02X} played as {:02X}", Describe(*v), rule, s.rule);
  return s;
}

uint32_t OnlineCreateSessionRule() { return PickedRule(); }

}  // namespace svr2011

// CREATE SESSION made / freed.
REX_EXTERN(__imp__sub_824EAD88);
REX_HOOK_RAW(sub_824EAD88) {
  g_cs_rule = ~0u;  // (before: the screen reads its match's name as it's made)
  g_choice = 0;
  g_cs_alive = true;
  __imp__sub_824EAD88(ctx, base);
  REXLOG_INFO("online rules: CREATE SESSION made (rule {:02X})", g_cs_rule.load());
}
REX_EXTERN(__imp__sub_824EBB78);
REX_HOOK_RAW(sub_824EBB78) {
  g_cs_alive = false;
  __imp__sub_824EBB78(ctx, base);
}

// Each frame of CREATE SESSION: on top when sub_824EAAF8 ran twice since the last.
REX_EXTERN(__imp__sub_824EBFF0);
REX_HOOK_RAW(sub_824EBFF0) {
  if (g_cs_calls.exchange(0) >= 2) g_cs_at = NowMs();
  __imp__sub_824EBFF0(ctx, base);
}
REX_EXTERN(__imp__sub_824EAAF8);
REX_HOOK_RAW(sub_824EAAF8) {
  ++g_cs_calls;
  __imp__sub_824EAAF8(ctx, base);
}

// The game's rule-name helper (rule in r3): CREATE SESSION reads its match's
// name through it at 0x824EB3B8 - the match picked (as it's made, and again
// when the MATCH TYPE list returns).
REX_EXTERN(__imp__sub_824CCB38);
REX_HOOK_RAW(sub_824CCB38) {
  if (uint32_t(ctx.lr) == 0x824EB3B8 && g_cs_alive.load()) {
    const uint32_t rule = ctx.r3.u32;
    if (rule != g_cs_rule.exchange(rule)) {
      g_choice = 0;  // (another match: NONE again)
      REXLOG_INFO("online rules: CREATE SESSION match is rule {:02X}", rule);
    }
  }
  __imp__sub_824CCB38(ctx, base);
}
