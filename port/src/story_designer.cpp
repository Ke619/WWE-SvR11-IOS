// WWE SmackDown vs. Raw 2011 - Story Designer rules the port lifts.
//
// Divas and male superstars together (mixed_gender_matches, roster.cpp - the
// same setting as for matches). Story Designer has three checks of its own,
// all on the record's gender (+208):
// - a scene's cast (sub_82650418(editor), from sub_826517F0, which shows the
//   message): 1 "Cross-gender attacks cannot be set", 2 "A male Superstar
//   has been set in a casting slot for Divas", 3 "A Diva has been set in a
//   casting slot for male Superstars" (0 OK, 4 an unknown attack type);
// - a story match's people (sub_8266AE90(editor), from sub_8266E588): 1
//   "This match type does not permit male Superstars and Divas to face off"
//   (2 Mixed Tag's team balance and 3 no rule are kept);
// - the superstar list for a slot (sub_82675B30(editor)): with a man in the
//   match it leaves the Divas out, with a Diva the men. It asks each person
//   already in the match for a gender through sub_828B6950(id) /
//   sub_828B69B0(record); answering "none" (2) there while it gathers them
//   leaves nobody out. Its select screen (sub_82452428) and the rule check
//   it calls (sub_826356B0) get the real genders.
// (Diva-only / male-only match types and titles keep their rules.)
//
// Ctrl+C / Ctrl+V on its lists (the PC keyboard; keyboard_typing.cpp passes
// the keys on while no on-screen keyboard is open): the game's own copy and
// paste, without its menus, from each list screen's frame (its vtable+64),
// on the game thread:
// - moments (sub_82664C30, state +688: 0 browsing, 1 copy mode): copy =
//   SetState(screen, 1) (sub_82662EB8: the moment under the cursor to +696,
//   its clipboard icon, as MOMENT MANAGEMENT -> COPY/PASTE); paste = what the
//   confirm's YES does (sub_826609A8 checks the place; the ADD row inserts a
//   moment, sub_8263C768; sub_8263CA90 copies; SetState(screen, 0) rebuilds
//   the rows);
// - shows (calendar, sub_826682A8, state +700): copy = sub_82667330(screen,
//   1, 8) (the cursor's show to +688/692/696); paste = sub_8263E210 (the
//   show's content onto the cursor's show), checked by sub_82665910;
// - moment groups (sub_8265DB68, state +704 / +708): copy = the group under
//   the cursor; paste = sub_8263D188 onto the cursor's group (a new one on an
//   empty place, sub_8263AE30, removed again if the paste is refused),
//   checked by sub_82636740.
// The clipboard lasts until the game is closed (moments: while the copied one
// still exists).

#include "story_designer.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

bool Mixed() { return rex::cvar::Query<bool>("mixed_gender_matches"); }

thread_local bool g_gathering = false;  // (sub_82675B30 collecting the match's people)

}  // namespace

REX_EXTERN(__imp__sub_82650418);
REX_HOOK_RAW(sub_82650418) {
  __imp__sub_82650418(ctx, base);
  if (Mixed() && ctx.r3.u32 >= 1 && ctx.r3.u32 <= 3) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_8266AE90);
REX_HOOK_RAW(sub_8266AE90) {
  __imp__sub_8266AE90(ctx, base);
  if (Mixed() && ctx.r3.u32 == 1) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82675B30);
REX_HOOK_RAW(sub_82675B30) {
  g_gathering = Mixed();
  __imp__sub_82675B30(ctx, base);
  g_gathering = false;
}

REX_EXTERN(__imp__sub_828B6950);
REX_HOOK_RAW(sub_828B6950) {
  if (g_gathering) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_828B6950(ctx, base);
}

// Road to WrestleMania's Created Superstar story (VS. UNDERTAKER): its list
// of Created Superstars (sub_8246ADA0: each used slot that is male and not
// sub_828B6D40(id), "downloaded from Community Creations") leaves the
// downloaded ones out, and with none left says "A Created Superstar is needed
// to play this story" (string 0xC79E) - a player whose only one was
// downloaded can't play it. While that list is built none counts as
// downloaded.
static thread_local bool g_rtwm_list = false;

REX_EXTERN(__imp__sub_8246ADA0);
REX_HOOK_RAW(sub_8246ADA0) {
  g_rtwm_list = true;
  __imp__sub_8246ADA0(ctx, base);
  g_rtwm_list = false;
}

REX_EXTERN(__imp__sub_828B6D40);
REX_HOOK_RAW(sub_828B6D40) {
  const uint32_t id = ctx.r3.u32;
  __imp__sub_828B6D40(ctx, base);
  if (g_rtwm_list && ctx.r3.u32) {
    static uint64_t logged = 0;
    if (id < 64 && !(logged >> id & 1)) {
      logged |= 1ull << id;
      REXLOG_INFO("[svr2011] Road to WrestleMania: downloaded Created Superstar {} is in the story's list", id);
    }
    ctx.r3.u64 = 0;
  }
}

REX_EXTERN(__imp__sub_828B69B0);
REX_HOOK_RAW(sub_828B69B0) {
  if (g_gathering) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_828B69B0(ctx, base);
}

REX_EXTERN(__imp__sub_826356B0);
REX_HOOK_RAW(sub_826356B0) {
  const bool was = g_gathering;
  g_gathering = false;
  __imp__sub_826356B0(ctx, base);
  g_gathering = was;
}

REX_EXTERN(__imp__sub_82452428);
REX_HOOK_RAW(sub_82452428) {
  g_gathering = false;  // (the select screen: from here on, the real genders)
  __imp__sub_82452428(ctx, base);
}


// ---- Ctrl+C / Ctrl+V on the lists

namespace {

using Clock = std::chrono::steady_clock;
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::atomic<int> g_key{0};             // 1 copy, 2 paste: waiting for a list's frame
std::atomic<int64_t> g_key_at{0};
std::atomic<int64_t> g_list_seen{0};   // a list's last frame
std::mutex g_notice_mutex;
std::string g_notice;
int64_t g_notice_until = 0;

uint32_t g_clip_moment = 0xFFFF;       // a moment slot
uint32_t g_clip_show[3] = {};          // a show (its location words)
bool g_have_show = false;
uint32_t g_clip_group = 0xFFFF;

void Notice(const char* text) {
  std::lock_guard lock(g_notice_mutex);
  g_notice = text;
  g_notice_until = NowMs() + 1800;
  REXLOG_INFO("[svr2011] story designer: {}", text);
}

// A key pressed in the last half second (else it went nowhere).
int TakeKey() {
  const int k = g_key.exchange(0);
  return k && NowMs() - g_key_at.load() < 500 ? k : 0;
}

using svrfmt::Rd32, svrfmt::Wr32;
void Wr16(uint8_t* base, uint32_t a, uint32_t v) { base[a] = uint8_t(v >> 8), base[a + 1] = uint8_t(v); }

// A game function with these arguments (r3...), on a stack frame below the
// caller's; the caller's registers back afterwards. -> r3
template <class F>
uint32_t Call(PPCContext& ctx, uint8_t* base, F fn, std::initializer_list<uint32_t> args) {
  const PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x400;
  uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64, &ctx.r7.u64};
  size_t i = 0;
  for (uint32_t a : args) *regs[i++] = a;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

uint32_t Story(PPCContext& ctx, uint8_t* base) { return Call(ctx, base, sub_82634138, {}); }

// A moment slot that still holds a moment (S+1220: 8 bytes each, type +2: 2
// scene, 3 match).
bool MomentExists(const uint8_t* base, uint32_t S, uint32_t slot) {
  if (slot >= 500) return false;
  const uint8_t type = base[S + 1220 + slot * 8 + 2];
  return type == 2 || type == 3;
}

}  // namespace

REX_EXTERN(__imp__sub_82664C30);
REX_HOOK_RAW(sub_82664C30) {  // the moment list's frame
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_82664C30(ctx, base);
  if (!screen || Rd32(base, screen + 672) != 1) return;
  g_list_seen = NowMs();
  const int key = TakeKey();
  if (!key) return;
  const uint32_t state = Rd32(base, screen + 688);
  const uint32_t S = Story(ctx, base);
  if (!S || state > 1) return;
  const uint32_t cursor = Call(ctx, base, sub_826608D0, {screen}) & 0xFFFF;
  if (key == 1) {
    if (cursor == 0xFFFF) return Notice("SELECT A MOMENT TO COPY");
    Call(ctx, base, sub_82662EB8, {screen, 1});  // (copy mode: +696 = the cursor's moment)
    g_clip_moment = cursor;
    return Notice("MOMENT COPIED - CTRL+V PASTES");
  }
  if (!MomentExists(base, S, g_clip_moment)) return Notice("NO MOMENT COPIED");
  Wr16(base, screen + 696, g_clip_moment);
  if (Call(ctx, base, sub_826609A8, {screen}) != 0) return Notice("THE MOMENT CAN'T BE PASTED HERE");
  Call(ctx, base, sub_826650E0, {screen});
  uint32_t dst = cursor;
  if (dst == 0xFFFF) {  // the ADD row: a new moment at the end
    const uint32_t last = Call(ctx, base, sub_8265E4F0, {screen});
    dst = Call(ctx, base, sub_8263C768, {S, 1, last, 0, 0}) & 0xFFFF;
    if (dst == 0xFFFF) return Notice("THE MOMENT CAN'T BE PASTED HERE");
  }
  Call(ctx, base, sub_8263CA90, {S, dst, g_clip_moment});
  Wr32(base, S + 16, 1);
  Call(ctx, base, sub_82662EB8, {screen, 0});  // (rows rebuilt)
  Notice("MOMENT PASTED");
}

REX_EXTERN(__imp__sub_826682A8);
REX_HOOK_RAW(sub_826682A8) {  // the calendar's frame
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_826682A8(ctx, base);
  if (!screen) return;
  g_list_seen = NowMs();
  const int key = TakeKey();
  if (!key) return;
  const uint32_t state = Rd32(base, screen + 700);
  const uint32_t S = Story(ctx, base);
  if (!S || state > 1) return;
  if (key == 1) {
    Call(ctx, base, sub_82667330, {screen, 1, 8});
    for (int k = 0; k < 3; ++k) g_clip_show[k] = Rd32(base, screen + 688 + 4 * k);
    g_have_show = true;
    return Notice("SHOW COPIED - CTRL+V PASTES ITS CONTENT ONTO ANOTHER SHOW");
  }
  if (!g_have_show) return Notice("NO SHOW COPIED");
  for (int k = 0; k < 3; ++k) Wr32(base, screen + 688 + 4 * k, g_clip_show[k]);
  const uint32_t ok = Call(ctx, base, sub_82665910, {screen});
  if (ok != 0 && ok != 1 && ok != 6) return Notice("THE SHOW CAN'T BE PASTED HERE");
  const uint32_t loc = ctx.r1.u32 - 0x200;  // (4 words above the calls' frames, which start at r1 - 0x400)
  Call(ctx, base, sub_826341B8, {loc, g_clip_show[0], g_clip_show[1], g_clip_show[2], 0});
  Call(ctx, base, sub_8263E210, {S, S + 24, loc});
  Wr32(base, S + 16, 1);
  Call(ctx, base, sub_82667330, {screen, 0, 8});
  Notice("SHOW PASTED");
}

REX_EXTERN(__imp__sub_8265DB68);
REX_HOOK_RAW(sub_8265DB68) {  // the moment group list's frame
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_8265DB68(ctx, base);
  if (!screen) return;
  g_list_seen = NowMs();
  const int key = TakeKey();
  if (!key) return;
  const uint32_t state = Rd32(base, screen + 704), sub = Rd32(base, screen + 708);
  const uint32_t S = Story(ctx, base);
  if (!S || !(state == 0 || state == 6) || sub != 1) return;
  const uint32_t cursor = Call(ctx, base, sub_82634278, {S, S + 24, S + 40}) & 0xFFFF;
  if (key == 1) {
    if (cursor == 0xFFFF) return Notice("SELECT A MOMENT GROUP TO COPY");
    g_clip_group = cursor;
    return Notice("MOMENT GROUP COPIED - CTRL+V PASTES");
  }
  if (g_clip_group >= 500) return Notice("NO MOMENT GROUP COPIED");
  uint32_t dst = cursor;
  bool created = false;
  if (dst == 0xFFFF) {
    if (Call(ctx, base, sub_826345A8, {S}) >= 500 || Call(ctx, base, sub_82634530, {S, S + 24}) >= 30)
      return Notice("NO ROOM FOR ANOTHER MOMENT GROUP");
    dst = Call(ctx, base, sub_8263AE30, {S, S + 24, S + 40, 1}) & 0xFFFF;
    created = true;
  }
  const uint32_t ok = Call(ctx, base, sub_82636740, {S, dst, g_clip_group});
  if (ok != 0 && ok != 6) {
    if (created) Call(ctx, base, sub_8263C190, {S, dst, 0});
    return Notice("THE MOMENT GROUP CAN'T BE PASTED HERE");
  }
  Call(ctx, base, sub_8263D188, {S, dst, g_clip_group});
  Wr32(base, S + 16, 1);
  Call(ctx, base, sub_82658D50, {screen});
  Wr32(base, screen + 704, 0);
  Wr32(base, screen + 708, 0);  // (its entry runs again: the list redrawn)
  Notice("MOMENT GROUP PASTED");
}

namespace {

class StoryNotice final : public rex::ui::ImGuiDialog {
 public:
  explicit StoryNotice(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::string text;
    {
      std::lock_guard lock(g_notice_mutex);
      if (NowMs() > g_notice_until) return;
      text = g_notice;
    }
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gh = std::min(h, w * 9.0f / 16.0f), scale = gh / 720.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 24.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 c(w * 0.5f, (h + gh) * 0.5f - gh * 0.16f);
    const ImVec2 pad(16.0f * scale, 8.0f * scale);
    const ImVec2 a(c.x - ts.x * 0.5f - pad.x, c.y - ts.y * 0.5f - pad.y);
    const ImVec2 b(c.x + ts.x * 0.5f + pad.x, c.y + ts.y * 0.5f + pad.y);
    dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 220), 6.0f * scale);
    dl->AddRect(a, b, IM_COL32(220, 220, 225, 255), 6.0f * scale, 0, 2.0f * scale);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), text.c_str());
  }
};

}  // namespace

namespace svr2011 {

bool StoryListActive() { return NowMs() - g_list_seen.load() < 250; }

void StoryListKey(bool paste) {
  g_key_at = NowMs();
  g_key = paste ? 2 : 1;
}

void InstallStoryDesignerOverlay(rex::ui::ImGuiDrawer* drawer) { new StoryNotice(drawer); }

}  // namespace svr2011
