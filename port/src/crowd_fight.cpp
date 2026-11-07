// WWE SmackDown vs. Raw 2011 - SvR 2008's crowd fighting, on arenas ported from SvR 2008.
//
// Every SvR 2008 arena's stage has two collision sets: HMD 996 (the match's) and HMD 999, the same with the back
// barricade (z -85.4, x -45..45) and two pieces at the ramp opened, outer walls at z -112 and a stepped corner of
// the stands (x 60..177, z -112..-58: risers of 10, a platform 38 high) - the crowd area. The arena port keeps
// both; 2011 only ever loads 996 (sub_82187258: ids 996 + 2k, one for an arena, eight for the backstage rooms).
// In an Extreme Rules match (rules 0x4D-0x50) on such an arena the loader is handed 999 instead.
// Which arenas: the custom arena in use, when its manifest says crowd=1 or it was ported from SvR 2008 (its
// author line) - crowd=0 turns it off. Every game in an online lobby plays the same arena mod and rule, so
// their collision stays the same. Test aid: SVR2011_TEST_CROWD=1 - any Extreme Rules match (an arena served
// by SVR2011_TEST_ARENA_REDIRECT has no mod to read).

#include "crowd_fight.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

#include "arena_mods.h"
#include "match_hud.h"
#include "match_types.h"
#include "p2p.h"
#include "pad_types.h"
#include "weapons_page.h"

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00;      // the match's settings: +0 its rule, +64 its arena
constexpr uint32_t kExtremeFirst = 0x4D, kExtremeLast = 0x50;
constexpr uint32_t kBackstageArena = 78;
constexpr uint32_t kMatchCollision = 996, kCrowdCollision = 999;
// (each collision's z-band index: the entry before it - 995 for 996, 997 for 999; svr08_stage.index_build)
constexpr uint32_t kMatchIndex = 995, kCrowdIndex = 997;
constexpr uint32_t kLoaderFirst = 0x82187258, kLoaderEnd = 0x821874C8;  // sub_82187258

std::atomic<bool> g_crowd{false};

using svrfmt::Rd32, svrfmt::RdF, svrfmt::Wr32, svrfmt::WrF;

// A fighter put somewhere: his place (+288..+296), facing (+276), the body's place (*(+304)) and its two other
// copies (+704, +768 - as hctp_room.cpp's PlaceBody: without them the body eased him back).
void PutBody(uint8_t* base, uint32_t f, float x, float y, float z, float yaw) {
  WrF(base + f + 288, x), WrF(base + f + 292, y), WrF(base + f + 296, z);
  WrF(base + f + 276, yaw);
  if (const uint32_t body = Rd32(base + f + 304))
    for (uint32_t at : {0u, 704u, 768u}) WrF(base + body + at, x), WrF(base + body + at + 4, y), WrF(base + body + at + 8, z);
}

// Test placements held (as the climb's landing) for kPlaceHold frames.
constexpr uint32_t kPlaceHold = 10;
struct TestHold { uint32_t f = 0, frames = 0; float x = 0, z = 0; };
TestHold g_test_hold[6];

// The fan's hand-off (crowd_fight.h). People: *0x82E3CC50 + 4i (+288 / +292 / +296 x y z: the floor y 0, the ring
// -12). A taunt asked (slots 9-12) by one of them on the floor at the barricade (beyond 62 from the ring's middle on
// x or z; the crowd area behind it too) - once per kCooldown match frames each, kMax a match.
constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
constexpr uint32_t kTauntFirst = 9, kTauntLast = 12;
constexpr uint32_t kCooldown = 600, kMax = 8;
constexpr float kBarricade = 62.f;
// The weapons handed over, in turn: models every Extreme Rules match makes (the trash can and guitar of its nine:
// OPTIONS -> WEAPONS picks in their place).
constexpr int kHanded[] = {4, 63, 72, 89, 28, 102};
std::atomic<uint32_t> g_asked[8];  // (the fighters whose taunt was asked this update - the update's jobs run on
                                   // several threads: a set, handled in person order after it)
uint32_t g_last_frames = 0, g_given = 0, g_turn = 0;
uint32_t g_fighter_at[6] = {};  // (the match frame of each person's last hand-off + 1)

bool HasKey(const std::string& text, const std::string& line) {
  size_t at = 0;
  while ((at = text.find(line, at)) != std::string::npos) {
    if (at == 0 || text[at - 1] == '\n') return true;
    at += line.size();
  }
  return false;
}

// The custom arena in use opens its crowd area.
bool CrowdArena() {
  const std::string manifest = svr2011::ArenaModManifest();
  if (manifest.empty() || HasKey(manifest, "crowd=0")) return false;
  return HasKey(manifest, "crowd=1") || manifest.find("SvR 2008 arena") != std::string::npos;
}

}  // namespace

namespace svr2011 {
void Handoff(PPCContext& ctx, uint8_t* base, int person, uint32_t asked, uint32_t frames);

// A weapon lying at (x, z) - on the floor, or the ring's mat: sub_821D2070(any free slot, position, rotation, model;
// stack: motion 15010 lying, place, flag 1) - as a backstage room's (hctp_room.cpp). The object, or 0 (no free slot,
// or the model isn't loaded).
uint32_t SpawnWeapon(PPCContext& ctx, uint8_t* base, int model, float x, float z, bool ring) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  Wr32(base + ctx.r1.u32 + 84, 15010);
  Wr32(base + ctx.r1.u32 + 92, ring ? 0 : 1);  // (place: 0 the ring, 1 the floor)
  Wr32(base + ctx.r1.u32 + 100, 1);
  ctx.r3.u64 = 0xFFFFFFFFu;
  ctx.f1.f64 = x, ctx.f2.f64 = ring ? -12.0 : 0.0, ctx.f3.f64 = z;
  ctx.f4.f64 = 0, ctx.f5.f64 = 0, ctx.f6.f64 = 0;
  ctx.r7.u64 = uint32_t(model);
  sub_821D2070(ctx, base);
  const uint32_t made = ctx.r3.u32;
  ctx = saved;
  return made;
}

// Test aid: SVR2011_TEST_GIVE_WEAPON=<model>[,<match frame>[,<person>]] - once a match (frame 30), that weapon lies at
// the person's feet (0; 1 the CPU opponent, who goes for a weapon that near): a weapon's hits and breaks without walking
// to one. The model must be loaded.
void TestGiveWeapon(PPCContext& ctx, uint8_t* base, uint32_t frames, bool new_match) {
  static const char* give = std::getenv("SVR2011_TEST_GIVE_WEAPON");
  static bool given = false;
  if (!give) return;
  if (new_match) given = false;
  int model = 0, at = 30, person = 0;
  std::sscanf(give, "%d,%d,%d", &model, &at, &person);
  const uint32_t c = Rd32(base + kChars + uint32_t(std::clamp(person, 0, 5)) * 4);
  if (given || !c || frames < uint32_t(at) || model <= 0) return;
  given = true;
  const float x = RdF(base + c + 288), y = RdF(base + c + 292), z = RdF(base + c + 296);
  const float d = std::max(std::sqrt(x * x + z * z), 1.f);
  const float wx = x - x / d * 4.f, wz = z - z / d * 4.f;  // (a step toward the ring's middle)
  const uint32_t made = SpawnWeapon(ctx, base, model, wx, wz, y < -6.f);
  REXLOG_INFO("[svr2011] test: weapon {} at person {}'s feet ({:.0f},{:.0f}), frame {} - {}", model, person, wx, wz,
              frames, made ? "made" : "not made");
}

bool CrowdMatch() { return g_crowd.load(); }

void CrowdMoveAsked(uint32_t fighter, uint32_t slot) {
  if (slot < kTauntFirst || slot > kTauntLast || !fighter || !g_crowd.load()) return;
  for (auto& a : g_asked) {
    uint32_t was = 0;
    if (a.compare_exchange_strong(was, fighter) || was == fighter) return;
  }
}

// Test aids (crowd fighting): SVR2011_TEST_PLACE="<person>:<x>,<z>[;<person>:<x>,<z>...]" - at match frame
// SVR2011_TEST_PLACE_AT (300) those people are put there on the floor (the real position vector *(+304), its copy
// +288 and the body's other copies, held 10 frames; y 0; the area +441 / +444 1, outside the ring), once a match; SVR2011_TEST_CROWD_LOG=1 - each half second people 0-3: place, area bytes (+441, +444),
// motion (+212). SVR2011_TEST_PLACE_ANY=<every s> - in any match, not only crowd ones, and again every <s> seconds
// (0: once) - CPUs fighting on the floor outside (item 10: a grapple outside the ring)
void CrowdTestAids(uint8_t* base, uint32_t frames, bool new_match) {
  static const std::string place = [] { const char* v = std::getenv("SVR2011_TEST_PLACE"); return std::string(v ? v : ""); }();
  static const uint32_t place_at = [] { const char* v = std::getenv("SVR2011_TEST_PLACE_AT"); return uint32_t(v ? std::atoi(v) : 300); }();
  static const char* any_env = std::getenv("SVR2011_TEST_PLACE_ANY");
  static const uint32_t place_every = any_env ? uint32_t(std::atoi(any_env)) * 60 : 0;
  static const bool log = std::getenv("SVR2011_TEST_CROWD_LOG") != nullptr;
  static bool placed = false;
  static uint32_t placed_frame = 0;
  if (new_match) placed = false;
  if (placed && place_every && frames >= placed_frame + place_every) placed = false;
  const bool crowd = g_crowd.load() || any_env;
  // (and only once people 0 and 1 are on their feet - the match's intro over: placed during it they were put back)
  auto ready = [&](uint32_t i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    const uint32_t m = c ? Rd32(base + c + 212) : 0;
    return !c || m == 0 || m == 20 || m == 20400 || m == 20500;
  };
  if (!place.empty() && !placed && frames >= place_at && crowd && ready(0) && ready(1)) {  // (crowd matches only)
    placed = true, placed_frame = frames;
    for (size_t at = 0; at < place.size();) {
      size_t semi = place.find(';', at);
      if (semi == std::string::npos) semi = place.size();
      int person = -1;
      float x = 0, z = 0;
      if (std::sscanf(place.substr(at, semi - at).c_str(), "%d:%f,%f", &person, &x, &z) == 3 && person >= 0 && person < 6)
        if (const uint32_t c = Rd32(base + kChars + uint32_t(person) * 4)) {
          PutBody(base, c, x, 0.f, z, RdF(base + c + 276));
          g_test_hold[person] = {c, kPlaceHold, x, z};
          base[c + 441] = 1, base[c + 444] = 1;  // (outside the ring: 0 is in it, 10 the cell's roof)
          REXLOG_INFO("crowd: test - person {} put at ({}, {})", person, x, z);
        }
      at = semi + 1;
    }
  }
  for (TestHold& h : g_test_hold)
    if (h.frames && h.f) --h.frames, PutBody(base, h.f, h.x, 0.f, h.z, RdF(base + h.f + 276));
  static uint32_t logged_at = ~0u;
  if (log && crowd && frames && frames % 30 == 0 && frames != logged_at) {
    logged_at = frames;
    std::string line;
    for (uint32_t i = 0; i < 4; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4))
        line += fmt::format(" | {}: ({:.1f}, {:.1f}, {:.1f}) area {} {} motion {}", i, RdF(base + c + 288),
                            RdF(base + c + 292), RdF(base + c + 296), base[c + 441], base[c + 444], Rd32(base + c + 212));
    REXLOG_INFO("crowd: frame {}{}", frames, line);
  }
}

void CrowdHandoffUpdate(PPCContext& ctx, uint8_t* base) {
  uint32_t asked_set[8];
  for (int i = 0; i < 8; ++i) asked_set[i] = g_asked[i].exchange(0);
  const uint32_t frames = Rd32(base + kMatchFrames);
  const bool new_match = frames < g_last_frames;
  CrowdTestAids(base, frames, new_match);
  if (new_match) {  // (a new match)
    g_given = g_turn = 0;
    for (auto& f : g_fighter_at) f = 0;
  }
  g_last_frames = frames;
  TestGiveWeapon(ctx, base, frames, new_match);
  CrowdVaultUpdate(ctx, base, frames, new_match);
  if (!g_crowd.load()) return;
  for (int person = 0; person < 6 && g_given < kMax; ++person) {
    const uint32_t c = Rd32(base + kChars + uint32_t(person) * 4);
    if (c && std::find(std::begin(asked_set), std::end(asked_set), c) != std::end(asked_set)) Handoff(ctx, base, person, c, frames);
  }
}

void Handoff(PPCContext& ctx, uint8_t* base, int person, uint32_t asked, uint32_t frames) {
  if (g_fighter_at[person] && frames < g_fighter_at[person] - 1 + kCooldown) return;
  const float x = RdF(base + asked + 288), y = RdF(base + asked + 292), z = RdF(base + asked + 296);
  // (test aid: SVR2011_TEST_HANDOFF_ANYWHERE=1 - wherever they taunt, the ring too)
  static const bool anywhere = std::getenv("SVR2011_TEST_HANDOFF_ANYWHERE") != nullptr;
  if (!anywhere && (y < -6.f || (std::fabs(x) < kBarricade && std::fabs(z) < kBarricade))) return;  // (not by it)
  // At their feet, a step toward the ring's middle (out of the barricade).
  const float d = std::max(std::sqrt(x * x + z * z), 1.f);
  const float wx = x - x / d * 6.f, wz = z - z / d * 6.f;
  const int model = svr2011::ExtremeWeaponsPlacedModel(1, kHanded[g_turn % std::size(kHanded)]);
  const uint32_t made = SpawnWeapon(ctx, base, model, wx, wz, y < -6.f);
  ++g_turn;
  g_fighter_at[person] = frames + 1;
  if (made) ++g_given;
  REXLOG_INFO("[svr2011] crowd: a fan hands person {} a weapon (model {}) at ({:.0f},{:.0f}), frame {} - {}", person, model,
              wx, wz, frames, made ? "made" : "not made (no free slot)");
}

bool CrowdAreaFor(uint8_t* base) {
  static const bool test = std::getenv("SVR2011_TEST_CROWD") != nullptr;
  const uint32_t rule = base[kLive], arena = Rd32(base + kLive + 64);  // (the rule: the settings' first byte)
  return rule >= kExtremeFirst && rule <= kExtremeLast && arena != kBackstageArena && (test || CrowdArena());
}

uint32_t CrowdCollisionId(uint8_t* base, uint32_t lr, uint32_t id) {
  if (lr < kLoaderFirst || lr >= kLoaderEnd) return id;
  static const bool log = std::getenv("SVR2011_TEST_CROWD_LOG") != nullptr;  // (the stage loader's look-ups, once each)
  if (log) {
    static uint64_t told = 0;
    if (id >= 990 && id < 1054 && !(told >> (id - 990) & 1)) {
      told |= 1ull << (id - 990);
      REXLOG_INFO("crowd: stage loader asks {} (lr {:08X})", id, lr);
    }
  }
  // (the index: asked right after the collision (996, then 995) - that look-up's decision, not a second one)
  if (id == kMatchIndex) return g_crowd.load() ? kCrowdIndex : id;
  if (id != kMatchCollision) return id;
  const uint32_t rule = base[kLive], arena = Rd32(base + kLive + 64);
  static const bool test = std::getenv("SVR2011_TEST_CROWD") != nullptr;
  const bool crowd = rule >= kExtremeFirst && rule <= kExtremeLast && arena != kBackstageArena &&
                     (test || CrowdArena());
  g_crowd = crowd;
  if (lr == 0x821872BC && crowd)  // (once a stage: its first look-up)
    REXLOG_INFO("crowd fight: the crowd area is open - SvR 2008's crowd collision (rule {:02X}, arena {})", rule, arena);
  // (the collision and its index together: 999's triangles looked up through 996's index found the wrong ones -
  // the crowd area's walls and floors were missing from it)
  return crowd ? kCrowdCollision : id;
}

}  // namespace svr2011

// Test aid (walk-in research): SVR2011_TEST_COLL_LOG=1 - in crowd matches, the stage collision's walk test
// (sub_826AB2E8: hmd, mover, the band's record list, its count) for movers with +16 or +24 < SVR2011_TEST_COLL_X
// (-55, toward the crowd pocket): the mover's place before / after, and each record that answered (sub_826AAFE0 != 0):
// its number, type byte (+93), +88, the answer. At most one line per mover every 20 calls, 600 lines a run.
namespace {
thread_local std::string* t_hits = nullptr;
thread_local uint32_t t_hmd = 0;
}  // namespace

REX_EXTERN(__imp__sub_826AAFE0);
REX_HOOK_RAW(sub_826AAFE0) {
  const uint32_t rec = ctx.r3.u32;
  __imp__sub_826AAFE0(ctx, base);
  if (!t_hits || !ctx.r3.u32 || t_hits->size() > 900) return;
  int k = -1;
  const uint32_t n = Rd32(base + t_hmd + 176), arr = Rd32(base + t_hmd + 180);
  for (uint32_t i = 0; i < n && i < 4096; ++i)
    if (Rd32(base + arr + i * 4) == rec) { k = int(i); break; }
  *t_hits += fmt::format(" #{} t{} s{:.1f} ={:08X}", k, base[rec + 93], RdF(base + rec + 88), ctx.r3.u32);
}

REX_EXTERN(__imp__sub_826AB2E8);
REX_HOOK_RAW(sub_826AB2E8) {
  static const bool log = std::getenv("SVR2011_TEST_COLL_LOG") != nullptr;
  static const float edge = [] { const char* v = std::getenv("SVR2011_TEST_COLL_X"); return v ? float(std::atof(v)) : -55.f; }();
  const uint32_t hmd = ctx.r3.u32, mover = ctx.r4.u32, list = ctx.r5.u32;
  const int32_t count = ctx.r6.s32;
  if (!log || !svr2011::CrowdMatch() || !mover ||
      (RdF(base + mover + 16) >= edge && RdF(base + mover + 24) >= edge)) {
    __imp__sub_826AB2E8(ctx, base);
    return;
  }
  static std::atomic<uint32_t> lines{0};
  static std::atomic<uint32_t> calls[8]{};
  static std::atomic<uint32_t> movers[8]{};
  int m = 0;
  for (; m < 8; ++m) {
    uint32_t was = 0;
    if (movers[m].compare_exchange_strong(was, mover) || was == mover) break;
  }
  const bool tell = m < 8 && calls[m]++ % 20 == 0 && lines.load() < 600;
  const float x0 = RdF(base + mover + 16), y0 = RdF(base + mover + 20), z0 = RdF(base + mover + 24);
  std::string hits;
  if (tell) t_hits = &hits, t_hmd = hmd;
  __imp__sub_826AB2E8(ctx, base);
  t_hits = nullptr;
  if (!tell) return;
  ++lines;
  REXLOG_INFO("crowd: walk test mover {:08X} ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}) list {} of {} flags {:08X} = {:08X}{}",
              mover, x0, y0, z0, RdF(base + mover + 16), RdF(base + mover + 20), RdF(base + mover + 24),
              list ? count : -1, Rd32(base + hmd + 176), Rd32(base + mover + 36), ctx.r3.u32, hits);
}

// Test aid (walk-in research): SVR2011_TEST_GRID_DUMP=<folder> - each wall grid sub_8281E540 marks from the stage
// collision (walls into 10-byte cells, byte +3 = blocked): its size (+36 x +40), cell size (+44, +48), origin (+64,
// +68) and blocked count before / after in the log, and the blocked map as grid_<n>.pgm in that folder (first 16).
REX_EXTERN(__imp__sub_8281E540);
REX_HOOK_RAW(sub_8281E540) {
  static const char* dump = std::getenv("SVR2011_TEST_GRID_DUMP");
  const uint32_t g = ctx.r3.u32;
  const int32_t w = dump && g ? int32_t(Rd32(base + g + 36)) : 0, h = dump && g ? int32_t(Rd32(base + g + 40)) : 0;
  const uint32_t cells = dump && g ? Rd32(base + g + 32) : 0;
  const bool look = cells && w > 0 && h > 0 && w <= 4096 && h <= 4096;
  auto blocked = [&] {
    uint32_t n = 0;
    for (int32_t i = 0; i < w * h; ++i) n += base[cells + uint32_t(i) * 10 + 3] != 0;
    return n;
  };
  const uint32_t before = look ? blocked() : 0;
  __imp__sub_8281E540(ctx, base);
  if (!look) return;
  static std::atomic<uint32_t> told{0};
  const uint32_t k = told++;
  if (k >= 16) return;
  REXLOG_INFO("crowd: wall grid {} at {:08X}: {} x {} cells of ({:.2f}, {:.2f}) from ({:.1f}, {:.1f}), blocked {} -> {}", k, g,
              w, h, RdF(base + g + 44), RdF(base + g + 48), RdF(base + g + 64), RdF(base + g + 68), before, blocked());
  if (FILE* f = std::fopen(fmt::format("{}/grid_{}.pgm", dump, k).c_str(), "wb")) {
    std::fprintf(f, "P5\n%d %d\n255\n", h, w);  // (rows = the first index (+36), columns = the second (+40))
    for (int32_t i = 0; i < w * h; ++i) std::fputc(base[cells + uint32_t(i) * 10 + 3] ? 255 : 0, f);
    std::fclose(f);
  }
}

// The crowd area in the people's wall grids. sub_8281B118 sets up the stage's wall grids (people's walls: sub_8281E540
// puts the stage collision's walls into cells - 10 bytes each at +32, byte +3 blocked; +36 x +40 cells of +44 x +48
// from +64, +68, in stage coordinates: the HMD's x and z). The first grid is the ringside's: (-70, -100)..(70, 90) in
// 35 x 35 cells - an SvR 2008 crowd area beyond it gets no walls in any grid and people walk through them. In crowd
// matches that grid is stretched over the arena mod's crowd_area=<x0>,<z0>,<x1>,<z1> too (stage x / z, from the
// arena's crowd collision - tools/crowd_walkway.py): the same cells, larger (origin and cell size only - every
// reader takes them from the grid), before its walls go in. Online the grid depends only on the arena (its mod's
// manifest) and the rule (crowd matches: g_crowd, the same decision as the crowd collision's), so every peer builds
// the same grids.
namespace {

bool CrowdAreaRect(float r[4]) {
  const std::string manifest = svr2011::ArenaModManifest();
  size_t at = 0;
  while ((at = manifest.find("crowd_area=", at)) != std::string::npos) {
    if (at == 0 || manifest[at - 1] == '\n')
      if (std::sscanf(manifest.c_str() + at + 11, "%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4 && r[0] < r[2] &&
          r[1] < r[3])
        return true;
    at += 11;
  }
  return false;
}

}  // namespace

REX_EXTERN(__imp__sub_8281B118);
REX_HOOK_RAW(sub_8281B118) {
  const uint32_t g = ctx.r3.u32;
  float r[4];
  if (g && g_crowd.load() && CrowdAreaRect(r)) {
    const int32_t w = int32_t(Rd32(base + g + 36)), h = int32_t(Rd32(base + g + 40));
    const float ox = RdF(base + g + 64), oz = RdF(base + g + 68), cx = RdF(base + g + 44), cz = RdF(base + g + 48);
    // (the ringside's grid only - around (0, 0); not the aisle's)
    if (w > 0 && h > 0 && cx > 0 && cz > 0 && ox < 0 && oz < 0 && ox + w * cx > 0 && oz + h * cz > 0 &&
        (r[0] < ox || r[1] < oz || r[2] > ox + w * cx || r[3] > oz + h * cz)) {
      const float x0 = std::min(ox, r[0]), z0 = std::min(oz, r[1]);
      const float x1 = std::max(ox + w * cx, r[2]), z1 = std::max(oz + h * cz, r[3]);
      WrF(base + g + 64, x0), WrF(base + g + 68, z0);
      WrF(base + g + 44, (x1 - x0) / float(w)), WrF(base + g + 48, (z1 - z0) / float(h));
      REXLOG_INFO("crowd fight: the ringside wall grid ({}, {})..({}, {}) stretched over the crowd area: ({}, {})..({}, {})",
                  ox, oz, ox + w * cx, oz + h * cz, x0, z0, x1, z1);
    }
  }
  __imp__sub_8281B118(ctx, base);
}

// Test aid (walk-in research): SVR2011_TEST_CLAMP_LOG=1 - in crowd matches, each time the per-body clamp
// sub_821E5408 (r4 the body: place +16 / +24; r5 its result: +0 / +8) moves a body beyond 40 from the middle: before ->
// after (at most 300 lines); and once, the clamp's half sizes (0x82D9E9D4..E0) and the wall grids' table 0x82D9E0B0.
REX_EXTERN(__imp__sub_821E5408);
REX_HOOK_RAW(sub_821E5408) {
  static const bool log = std::getenv("SVR2011_TEST_CLAMP_LOG") != nullptr;
  if (!log || !g_crowd.load() || !ctx.r4.u32 || !ctx.r5.u32) {
    __imp__sub_821E5408(ctx, base);
    return;
  }
  static std::atomic<bool> told{false};
  if (!told.exchange(true)) {
    REXLOG_INFO("crowd: clamp half sizes {:.1f} {:.1f} {:.1f} {:.1f}", RdF(base + 0x82D9E9D4), RdF(base + 0x82D9E9D8),
                RdF(base + 0x82D9E9DC), RdF(base + 0x82D9E9E0));
    for (uint32_t k = 0; k < 64 && Rd32(base + 0x82D9E0B0 + k * 20) != 0xFFFFFFFFu; ++k) {
      const uint32_t r = 0x82D9E0B0 + k * 20;
      REXLOG_INFO("crowd: area table {} type {:08X} {:.1f} {:.1f} {:.1f} {:.1f}", k, Rd32(base + r), RdF(base + r + 4),
                  RdF(base + r + 8), RdF(base + r + 12), RdF(base + r + 16));
    }
  }
  const uint32_t body = ctx.r4.u32, out = ctx.r5.u32;
  const float bx = RdF(base + body + 16), bz = RdF(base + body + 24), x0 = RdF(base + out), z0 = RdF(base + out + 8);
  __imp__sub_821E5408(ctx, base);
  const float x1 = RdF(base + out), z1 = RdF(base + out + 8);
  static std::atomic<uint32_t> lines{0};
  if ((x1 != x0 || z1 != z0) && (std::fabs(bx) > 40 || std::fabs(bz) > 40) && lines++ < 300)
    REXLOG_INFO("crowd: clamp body {:08X} at ({:.1f}, {:.1f}): ({:.1f}, {:.1f}) -> ({:.1f}, {:.1f})", body, bx, bz, x0, z0, x1,
                z1);
}

// Test aid (walk-in research): SVR2011_TEST_BOX_HUNT=1 - in crowd matches, while person 0 is near the edge of the box
// that keeps people at the ring (|x| or |z| 52..70), each watched function that moves them (their place: the vector
// *(+304) or its copy +288 / +296) is logged with before / after and the area bytes (+441, +444), at most 400 lines.
namespace {

void BoxHunt(const char* name, void (*imp)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base) {
  static const bool on = std::getenv("SVR2011_TEST_BOX_HUNT") != nullptr;
  const uint32_t c = on && g_crowd.load() ? Rd32(base + kChars) : 0;
  const uint32_t v = c ? Rd32(base + c + 304) : 0;
  if (!v) {
    imp(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const float x0 = RdF(base + v), z0 = RdF(base + v + 8), cx0 = RdF(base + c + 288), cz0 = RdF(base + c + 296);
  const uint8_t a0 = base[c + 441], b0 = base[c + 444];
  imp(ctx, base);
  const float x1 = RdF(base + v), z1 = RdF(base + v + 8), cx1 = RdF(base + c + 288), cz1 = RdF(base + c + 296);
  auto edge = [](float x, float z) {
    const float m = std::max(std::fabs(x), std::fabs(z));
    return m > 52.f && m < 70.f;
  };
  static std::atomic<uint32_t> lines{0};
  if ((x1 != x0 || z1 != z0 || cx1 != cx0 || cz1 != cz0) && (edge(x0, z0) || edge(x1, z1) || edge(cx0, cz0)) &&
      lines++ < 400)
    REXLOG_INFO("crowd: box hunt {} moved person 0 *304 ({:.2f}, {:.2f}) -> ({:.2f}, {:.2f}), +288 ({:.2f}, {:.2f}) -> ({:.2f}, "
                "{:.2f}), area {} {} -> {} {} (lr {:08X})", name, x0, z0, x1, z1, cx0, cz0, cx1, cz1, a0, b0, base[c + 441],
                base[c + 444], lr);
}

}  // namespace

#define SVR_BOX_HUNT(fn)                                     \
  REX_EXTERN(__imp__##fn);                                   \
  REX_HOOK_RAW(fn) { BoxHunt(#fn, __imp__##fn, ctx, base); }
SVR_BOX_HUNT(sub_822CBD00)
SVR_BOX_HUNT(sub_822C9F48)
SVR_BOX_HUNT(sub_82249588)
SVR_BOX_HUNT(sub_822E35E8)
SVR_BOX_HUNT(sub_822AABC8)
SVR_BOX_HUNT(sub_826FA880)
SVR_BOX_HUNT(sub_826F75C8)
SVR_BOX_HUNT(sub_8231FCB8)
SVR_BOX_HUNT(sub_8224F1A8)
SVR_BOX_HUNT(sub_8224F338)
SVR_BOX_HUNT(sub_8224F4C8)
SVR_BOX_HUNT(sub_8224F678)
SVR_BOX_HUNT(sub_8281FA58)
SVR_BOX_HUNT(sub_8281FAE0)
SVR_BOX_HUNT(sub_82BB8348)
SVR_BOX_HUNT(sub_82BB83A0)
#undef SVR_BOX_HUNT

// -- Into the crowd: the climb over the barricade --------------------------------------------------------------------
//
// The game keeps people who are at the ring inside a box around it (about +-64, one way: whoever is outside it
// walks freely - the crowd area's floor and walls hold them there, and the walkway's opening leads back out), so
// nobody walks into the crowd area. A player at the barricade in front of it (inside the box near its edge, within
// kVaultReach of the arena mod's crowd_area=) sees "Y: HOLD - CLIMB INTO THE CROWD"; Y held kVaultPress frames plays a climb (kVaultMotion, the
// stock climb up from the floor) and then puts him on the crowd area's floor - its nearest point kVaultInset inside
// crowd_area= - with the body's three place copies, held kVaultHold frames, outside the ring (area +441 / +444 1),
// standing (motion 0). Players (port < 4, or every wrestler online - each game reads the same synced pad,
// FighterInput: the climb is an input like any move) hold Y; a CPU climbs only while someone else is in the crowd,
// after kVaultCpu frames walking at the box's edge (after him - the AI grid has the crowd area's walls).
// Test aid: SVR2011_TEST_VAULT_MOTION=<motion> - another climb motion; SVR2011_TEST_VAULT=<person> - that person
// climbs once at match frame SVR2011_TEST_PLACE_AT + 120 if he is in the spot (as if Y).
namespace {

constexpr float kBox = 54.f;         // (inside the box near its edge: max(|x|, |z|) beyond this)
constexpr float kVaultReach = 25.f;  // (the crowd area this near)
constexpr float kVaultInset = 25.f;  // (landing this far inside crowd_area=)
constexpr uint32_t kVaultMotionDefault = 15180, kVaultFrames = 50, kVaultHold = 10;
// (Y held this long: a press is 2011's own action button - picking up the weapon a fan handed over at the barricade
// - and its move breaks the hold, so the climb never takes a press from it)
constexpr uint32_t kVaultPress = 20;
constexpr uint32_t kVaultCpu = 30;  // (a CPU walking into the spot this long climbs too - as at HCTP's doors)
constexpr uint32_t kButtonY = 0x10;  // (the pad word at FighterInput + 52)

struct Vault {
  uint32_t f = 0;
  uint32_t frames = 0;  // (since the climb started; 0 none)
  uint32_t held = 0;    // (frames Y has been held in the spot)
  float x = 0, z = 0, yaw = 0;
};
Vault g_vault[8];

// Kept in: the game's own box holds people at the ring (inside it, one way), but outside it nothing holds them -
// a stage's walls don't stop a walk in these matches (a fence round the crowd area was walked through), so they
// walked off along the back of the arena into the void. Whoever has stood in the crowd area (crowd_area=, outside
// the box) is kept on it and the strip between it and the box until he is back in the box: put back on their
// nearest point when a step leaves them. Others outside the box (the aisle, the referee, the announcers) are left be.
constexpr float kArenaBox = 64.5f;
bool g_in_crowd[8];
// (put back: a walking person's place written once was walked on from the game's own the next frame - he is
// stopped (standing, motion 0) and held there kKeepHold frames, as the climb's landing)
constexpr uint32_t kKeepHold = 6;
struct KeepHold { uint32_t frames = 0; float x = 0, z = 0; };
KeepHold g_keep[8];

bool InRect(float x, float z, const float r[4]) { return x >= r[0] && x <= r[1] && z >= r[2] && z <= r[3]; }

void Nearest(float x, float z, const float r[4], float& bx, float& bz, float& best) {
  const float nx = std::clamp(x, r[0], r[1]), nz = std::clamp(z, r[2], r[3]);
  const float d = (nx - x) * (nx - x) + (nz - z) * (nz - z);
  if (d < best) best = d, bx = nx, bz = nz;
}

std::mutex g_vault_prompt_mutex;
uint32_t g_vault_prompt_port = 0;
std::chrono::steady_clock::time_point g_vault_prompt_at;

uint32_t VaultMotion() {
  static const uint32_t m = [] {
    const char* v = std::getenv("SVR2011_TEST_VAULT_MOTION");
    return v ? uint32_t(std::strtoul(v, nullptr, 10)) : kVaultMotionDefault;
  }();
  return m;
}

// The crowd area (x0, x1, z0, z1): crowd_area= is the stage's x / z, which are the game's (the aisle - the wall grids'
// second axis, 80..300 - is where people stand at the match's start, z +300; read as z / x it put the climb in the
// mirrored corner, not SvR 2008's own).
bool VaultArea(float a[4]) {
  float r[4];
  if (!CrowdAreaRect(r)) return false;
  a[0] = r[0], a[1] = r[2], a[2] = r[1], a[3] = r[3];
  return true;
}

void StartMotion(PPCContext& ctx, uint8_t* base, uint32_t f, uint32_t motion) {
  PPCContext c = ctx;
  c.r3.u64 = f, c.r4.u64 = motion, c.r5.u64 = 0, c.r6.u64 = 0, c.f1.f64 = 0.0;
  sub_82396E80(c, base);
}

std::string VaultButton(uint32_t port) {
  switch (svr2011::PlayerPadType(port)) {
    case svr2011::PadType::kPlayStation: return "TRIANGLE";
    case svr2011::PadType::kKeyboard: {
      std::string k = rex::cvar::Query<std::string>("keybind_y");
      k = k.substr(0, k.find(','));
      std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      return k.empty() ? "Y" : k;
    }
    default: return "Y";
  }
}

class CrowdPromptOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit CrowdPromptOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    uint32_t port = 0;
    {
      std::lock_guard<std::mutex> lock(g_vault_prompt_mutex);
      if (std::chrono::steady_clock::now() - g_vault_prompt_at > std::chrono::milliseconds(300)) return;
      port = g_vault_prompt_port;
    }
    if (!svr2011::MatchHudVisible()) return;
    svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, VaultButton(port), "HOLD - CLIMB INTO THE CROWD");
  }
};

}  // namespace

namespace svr2011 {

void InstallCrowdOverlay(rex::ui::ImGuiDrawer* drawer) { new CrowdPromptOverlay(drawer); }

void CrowdVaultUpdate(PPCContext& ctx, uint8_t* base, uint32_t frames, bool new_match) {
  if (new_match) {
    for (Vault& v : g_vault) v = {};
    std::fill(std::begin(g_in_crowd), std::end(g_in_crowd), false);
    for (KeepHold& k : g_keep) k = {};
  }
  float a[4];
  if (!g_crowd.load() || !VaultArea(a)) return;
  static const int test_person = [] {
    const char* v = std::getenv("SVR2011_TEST_VAULT");
    return v ? std::atoi(v) : -1;
  }();
  static const uint32_t test_at = [] {
    const char* v = std::getenv("SVR2011_TEST_PLACE_AT");
    return uint32_t(v ? std::atoi(v) : 300) + 120;
  }();
  bool prompt = false;
  uint32_t prompt_port = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    Vault& v = g_vault[i];
    if (!f) {
      v = {};
      g_in_crowd[i] = false;
      continue;
    }
    if (v.frames) {  // (climbing: then the landing, held)
      ++v.frames;
      if (v.frames == kVaultFrames || (v.frames > 2 && v.frames < kVaultFrames && Rd32(base + f + 212) != VaultMotion())) {
        v.frames = kVaultFrames;
        StartMotion(ctx, base, f, 0);
        base[f + 441] = 1, base[f + 444] = 1;
        REXLOG_INFO("crowd fight: person {} lands in the crowd at ({:.0f}, {:.0f})", i, v.x, v.z);
      }
      if (v.frames >= kVaultFrames) PutBody(base, f, v.x, 0.f, v.z, v.yaw);
      if (v.frames >= kVaultFrames + kVaultHold) v = {};
      continue;
    }
    if (g_keep[i].frames) {  // (put back: held there)
      --g_keep[i].frames;
      PutBody(base, f, g_keep[i].x, RdF(base + f + 292), g_keep[i].z, RdF(base + f + 276));
      continue;
    }
    const float x = RdF(base + f + 288), z = RdF(base + f + 296);
    {  // (kept in: the box, the crowd area and the strip between them)
      const float box[4] = {-kArenaBox, kArenaBox, -kArenaBox, kArenaBox};
      float bridge[4] = {std::max(a[0], box[0]), std::min(a[1], box[1]), std::max(a[2], box[2]), std::min(a[3], box[3])};
      if (bridge[0] > bridge[1]) bridge[0] = std::min(a[1], box[1]), bridge[1] = std::max(a[0], box[0]);
      if (bridge[2] > bridge[3]) bridge[2] = std::min(a[3], box[3]), bridge[3] = std::max(a[2], box[2]);
      if (InRect(x, z, box))
        g_in_crowd[i] = false;
      else if (InRect(x, z, a))
        g_in_crowd[i] = true;
      else if (g_in_crowd[i] && !InRect(x, z, bridge)) {
        float bx = x, bz = z, best = 1e30f;
        Nearest(x, z, a, bx, bz, best), Nearest(x, z, bridge, bx, bz, best), Nearest(x, z, box, bx, bz, best);
        StartMotion(ctx, base, f, 0);
        PutBody(base, f, bx, RdF(base + f + 292), bz, RdF(base + f + 276));
        g_keep[i] = {kKeepHold, bx, bz};
        static uint32_t told = 0, told_at = ~0u;
        if (told < 40 && frames != told_at && frames % 30 == 0) {
          told_at = frames;
          ++told;
          REXLOG_INFO("crowd fight: person {} kept in: ({:.1f}, {:.1f}) -> ({:.1f}, {:.1f}), motion {}", i, x, z, bx, bz,
                      Rd32(base + f + 212));
        }
      }
    }
    // (Y's hold: kept only while it stays held in the spot, on his feet)
    const uint32_t held = v.held;
    v.held = 0;
    if (std::max(std::fabs(x), std::fabs(z)) < kBox || InRect(x, z, a)) continue;  // (in the crowd already: no climb)
    const uint32_t port = Rd32(base + f + 1152);
    const bool player = port < 4 || P2POnlineMode();  // (online every wrestler is a player - his synced pad)
    const float nx = std::clamp(x, a[0], a[1]), nz = std::clamp(z, a[2], a[3]);
    // (a CPU anywhere at the box's edge while someone is in the crowd: his way after them ends there)
    // (a CPU climbs only after someone else in the crowd - his opponent went in; walking along the barricade after
    // one at the ringside he climbed in again and again, 29 Oct H11)
    bool someone_in = false;
    for (uint32_t k = 0; k < 8; ++k) someone_in |= k != i && g_in_crowd[k];
    if (!player && !someone_in) continue;
    if ((nx - x) * (nx - x) + (nz - z) * (nz - z) > kVaultReach * kVaultReach && player) continue;
    const uint32_t m = Rd32(base + f + 212);
    const bool walking = m == 20400 || m == 20500 || base[f + 1286];
    if (!(m == 0 || m == 20 || walking)) continue;  // (on his feet, not in a move)
    if (player) {
      if (!prompt && port < 4) prompt = true, prompt_port = port;  // (this PC's own players see it)
      const uint32_t input = FighterInput(f);
      if (input && (Rd32(base + input + 52) & kButtonY)) v.held = held + 1;
    } else if (walking) {  // (a CPU: walking into the barricade there, after his opponent)
      v.held = held + 1;
    }
    const bool test = test_person == int(i) && frames == test_at;
    if (v.held < (player ? kVaultPress : kVaultCpu) && !test) continue;
    // (the landing: the crowd area's nearest point, kVaultInset inside it; facing away from the ring)
    const float lx = std::clamp(x, std::min(a[0] + kVaultInset, a[1]), std::max(a[1] - kVaultInset, a[0]));
    const float lz = std::clamp(z, std::min(a[2] + kVaultInset, a[3]), std::max(a[3] - kVaultInset, a[2]));
    v = {f, 1, 0, lx, lz, std::atan2(lx - x, lz - z)};
    WrF(base + f + 276, v.yaw);
    StartMotion(ctx, base, f, VaultMotion());
    REXLOG_INFO("crowd fight: person {} climbs into the crowd from ({:.0f}, {:.0f}) (motion {} -> {}{})", i, x, z,
                VaultMotion(), Rd32(base + f + 212), test ? ", test" : "");
  }
  static bool shown = false;  // (the prompt coming on screen, logged)
  if (prompt != shown) {
    shown = prompt;
    if (prompt) REXLOG_INFO("crowd fight: prompt on screen - player {} at the barricade: HOLD {} - CLIMB INTO THE CROWD", prompt_port + 1,
                            VaultButton(prompt_port));
  }
  std::lock_guard<std::mutex> lock(g_vault_prompt_mutex);
  if (prompt) g_vault_prompt_port = prompt_port, g_vault_prompt_at = std::chrono::steady_clock::now();
}

}  // namespace svr2011
