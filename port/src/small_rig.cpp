// WWE SmackDown vs. Raw 2011 - small rig retarget (Hornswoggle).
//
// Hornswoggle's skeleton (ch195) is the only one with its own proportions:
// legs 0.42x and arms 0.51x the standard rig's. Every motion is made for the
// standard rig and sets the root (pelvis) height and the four IK targets
// (hands, feet) as absolute values, so on his rig he floated ~4.6 units above
// the mat with straight limbs. Before the IK solve (sub_823A4DD8) the pose is
// retargeted for him:
// - root height: scaled by the leg ratio above the lower foot while a foot is
//   near the mat (fading out above it); x/z stay (positions against the
//   opponent);
// - each IK target: T' = S_small + (T - S_std) * k around its shoulder / hip
//   (bind offsets from the root, turned to the character's facing).
// Offline (scratchpad hornswoggle/mid.py) this lands within 0.2 units (legs)
// and 0.9 (arms) of the full FK version.
//
// The pose (sub_823A70F8 writes it each frame): character + 1760, root
// translation at +64, IK targets at +208 / +224 / +240 / +256 (left hand,
// right hand, left foot, right foot; root-relative, -Y up, floor y = 0).
// Who the character is: its scene node's 3rd parent (CMOP; +16 = parent)
// +32 is the game's CHAR object, +1156 its person number; the roster lookup
// (sub_828B5A18) gives the record, +32 the character id. Id 195, or a
// superstar mod with base=195.
// SVR2011_TEST_RIG_LOG=1: logs his root and feet heights before / after.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "superstar_mods.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(small_rig_retarget, true, "Gameplay",
                    "Fit the standard motions to Hornswoggle's small skeleton (no floating, bent limbs)");

namespace {

constexpr uint32_t kRoot = 1760 + 64, kIk = 1760 + 208;
constexpr uint32_t kRoster = 0x82EDEA88, kHornswoggle = 195;
constexpr float kLeg = 0.419f, kArm = 0.512f;  // (his limb lengths / the standard rig's)

using svrfmt::Rd32, svrfmt::RdF, svrfmt::WrF;
bool Pointer(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && !(a & 3); }

struct Known {
  uint32_t game_char = 0, person = ~0u;
  bool small = false;
  int age = 0;
};
std::map<uint32_t, Known> g_known;

// Looked up again when the character's CHAR object or person number changes,
// and every 300 frames.
bool IsSmall(PPCContext& ctx, uint8_t* base, uint32_t ch) {
  uint32_t n = ch;
  for (int i = 0; i < 3 && Pointer(n); ++i) n = Rd32(base + n + 16);
  if (!Pointer(n)) return false;
  const uint32_t game_char = Rd32(base + n + 32);
  if (!Pointer(game_char) || std::memcmp(base + game_char + 4, "CHAR", 4)) return false;
  const uint32_t person = Rd32(base + game_char + 1156);
  Known& k = g_known[ch];
  if (k.game_char == game_char && k.person == person && ++k.age < 300) return k.small;
  const bool was = k.small && k.game_char == game_char && k.person == person;
  k = {game_char, person, false, 0};
  const uint32_t roster = Rd32(base + kRoster);
  if (!roster || person >= 64) return false;
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, lr = ctx.lr;
  ctx.r3.u64 = roster;
  ctx.r4.u64 = person;
  sub_828B5A18(ctx, base);
  const uint32_t rec = ctx.r3.u32;
  ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.lr = lr;
  if (!rec) return false;
  const uint32_t id = uint32_t(base[rec + 32]) << 8 | base[rec + 33];
  k.small = id == kHornswoggle || svr2011::SuperstarModBase(id) == kHornswoggle;
  if (k.small && !was) REXLOG_INFO("[svr2011] small rig: person {} (id {}) gets the small rig retarget", person, id);
  return k.small;
}

void Retarget(uint8_t* base, uint32_t ch) {
  uint8_t* root = base + ch + kRoot;
  uint8_t* ik = base + ch + kIk;
  const float ry = RdF(root + 4);
  float t[4][3];
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 3; ++c) t[i][c] = RdF(ik + i * 16 + c * 4);
  if (!std::isfinite(ry)) return;
  // root height above the lower foot, scaled while a foot is near the mat
  const float foot_h = std::min(-(ry + t[2][1]), -(ry + t[3][1]));
  const float support = std::clamp((3.5f - foot_h) / 1.5f, 0.0f, 1.0f);
  const float d = -ry - foot_h;
  const float new_h = d > 0 ? foot_h + d * kLeg : -ry;
  const float rh = -ry * (1 - support) + new_h * support;
  // the character's left, from the feet (else the hands; else +x)
  float lx = t[2][0] - t[3][0], lz = t[2][2] - t[3][2];
  if (lx * lx + lz * lz < 0.25f) lx = t[0][0] - t[1][0], lz = t[0][2] - t[1][2];
  float len = std::sqrt(lx * lx + lz * lz);
  if (len < 0.5f) lx = 1, lz = 0, len = 1;
  lx /= len, lz /= len;
  // per limb: side, standard bind offset (x along the left, y), small one, ratio
  struct Limb {
    float side, std_x, std_y, small_x, small_y, k;
  };
  static constexpr Limb kLimbs[4] = {{1, 2.0f, -5.86f, 1.6f, -4.68f, kArm},
                                     {-1, 2.0f, -5.86f, 1.6f, -4.68f, kArm},
                                     {1, 1.14f, 0, 0.91f, 0, kLeg},
                                     {-1, 1.14f, 0, 0.91f, 0, kLeg}};
  // T' - new root = S_small + (T - root - S_std) * k (S: bind offsets from the root)
  for (int i = 0; i < 4; ++i) {
    const Limb& l = kLimbs[i];
    const float sx = l.side * l.std_x * lx, sz = l.side * l.std_x * lz;
    const float hx = l.side * l.small_x * lx, hz = l.side * l.small_x * lz;
    WrF(ik + i * 16 + 0, hx + (t[i][0] - sx) * l.k);
    WrF(ik + i * 16 + 4, l.small_y + (t[i][1] - l.std_y) * l.k);
    WrF(ik + i * 16 + 8, hz + (t[i][2] - sz) * l.k);
  }
  WrF(root + 4, -rh);
  static const bool log = std::getenv("SVR2011_TEST_RIG_LOG") != nullptr;
  static int n = 0;
  if (log && ++n % 120 == 0)
    REXLOG_INFO("[svr2011] small rig: root {:.2f} -> {:.2f}, feet {:.2f} {:.2f} -> {:.2f} {:.2f}", -ry, rh,
                -(ry + t[2][1]), -(ry + t[3][1]), rh - RdF(ik + 32 + 4), rh - RdF(ik + 48 + 4));
}

}  // namespace

namespace svr2011 {
void PoseDump(uint8_t* base, uint32_t ch, uint32_t pass);  // (pose_dump.cpp: a test aid, off unless asked)
}

// (one hook for the IK solve: the small rig's retarget before it, the pose dump test aid after)
REX_EXTERN(__imp__sub_823A4DD8);
REX_HOOK_RAW(sub_823A4DD8) {
  const uint32_t ch = ctx.r3.u32, pass = ctx.r4.u32;
  if (REXCVAR_GET(small_rig_retarget) && ch && IsSmall(ctx, base, ch)) Retarget(base, ch);
  __imp__sub_823A4DD8(ctx, base);
  svr2011::PoseDump(base, ch, pass);
}
