// WWE SmackDown vs. Raw 2011 - Bink movies keep up with their sound.
//
// The game's movies (Road to WrestleMania's in-ring cutscenes, entrances'
// titantron, ...) run on a movie thread (sub_82594230): each time the game
// kicks it, it runs the movies' commands (sub_82593858) and then steps every
// movie (sub_82592EA8): when BinkWait (sub_82D4B270) says the next frame is
// due, BinkNextFrame (sub_82D4BCF8) - one frame a kick at most. The kicks
// come with the game's frames, so where the game draws fewer frames a second
// than the movie has (an integrated GPU under ~30 fps), the picture falls
// further behind the sound every second: 4-6 s by the end of a cutscene.
//
// After the game's step, a movie over a frame behind its own clock (real
// time since its first frame x its frame rate - the sound plays in real time;
// BinkWait is no help: it says "go" again right after every step) steps
// again, up to kMaxCatchUp more frames a kick: BinkNextFrame on a frame nobody
// decoded (bink +828 0) asks sub_82D4AC88(bink, 0) whether it's late. Late, it
// marks the frame skipped (+828 2) and nothing decodes it - and a Bink frame is
// built on the one before it, so every frame after a skip showed blocky colour
// noise until the next key frame (the A51's titantron, 8 Nov: 113 of 180 frames
// skipped; with the catch-up off the picture was clean). Bink skips at most 4 in a
// row: with +840 (the skips in a row) at 4, the check resets it and says "not
// late" - but BinkNextFrame then decodes only a frame whose data its reads have
// brought in (+832 or the planes' +80), and a burst of steps runs ahead of them:
// with +840 at 4 the A51's titantron was still noise. So the catch-up is off: a
// movie on a device too slow for it falls behind its sound (as before 2.1.0)
// instead of showing noise; a PC at 60 fps never needed it. Test aids:
// SVR2011_TEST_MOVIE_CATCH_UP=1 the catch-up as in 2.1.0-2.2.1 (Bink's skips);
// =decode with +840 at 4 before each step (the try above).
//
// Movie manager (r3): +4 the movies, +8 their count. Movie: +8 0 when live,
// +12 the Bink, +20 1 when it has a frame to step (the step clears it), +24
// paused. Bink: +8 frame count, +12 the current frame, +840 skips in a row. 0x82E3BE94 is 1 while
// in Bink (the game sets it around each call).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kInBink = 0x82E3BE94;
constexpr uint32_t kSkipsInARow = 840, kMaxSkipsInARow = 4;  // (Bink: sub_82D4AC88)
constexpr int kMaxCatchUp = 4;  // (more frames a kick: a long stall shouldn't stop the movie thread)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// Each movie playing: its clock (started at its first frame; a gap of over
// kGap between steps - paused, a load - moves the start on by the gap) and
// the frames skipped. Its end is logged with how long it took against its own
// length (Bink +20 / +24: frame rate and divider).
constexpr auto kGap = std::chrono::milliseconds(250);
struct Playing {
  std::chrono::steady_clock::time_point start, last;
  uint32_t skipped = 0;
};
std::unordered_map<uint32_t, Playing> g_playing;

using Clock = std::chrono::steady_clock;

// The frame the movie's clock is at (0: no clock).
double DueFrame(uint8_t* base, uint32_t bink, Clock::time_point now) {
  const uint32_t frame = Rd32(base + bink + 12);
  auto [it, fresh] = g_playing.try_emplace(bink);
  Playing& p = it->second;
  if (fresh || frame <= 2) p = Playing{now, now, 0};
  if (now - p.last > kGap) p.start += now - p.last;  // (not stepped a while: no catching up for that)
  p.last = now;
  const uint32_t rate = Rd32(base + bink + 20), div = std::max<uint32_t>(Rd32(base + bink + 24), 1);
  if (!rate) return 0;
  return 1.0 + std::chrono::duration<double>(now - p.start).count() * rate / div;
}

void Ended(uint8_t* base, uint32_t bink, uint32_t skipped, Clock::time_point now) {
  auto it = g_playing.find(bink);
  if (it == g_playing.end()) return;
  it->second.skipped += skipped;
  const uint32_t frames = Rd32(base + bink + 8);
  if (Rd32(base + bink + 12) < frames) return;
  const uint32_t rate = Rd32(base + bink + 20), div = std::max<uint32_t>(Rd32(base + bink + 24), 1);
  const double took = std::chrono::duration<double>(now - it->second.start).count();
  const double length = rate ? double(frames) * div / rate : 0.0;
  REXLOG_INFO("movies: bink {:08X} ended - {} frames in {:.1f} s (its length {:.1f} s), {} caught up", bink,
              frames, took, length, it->second.skipped);
  g_playing.erase(it);
}

uint32_t Call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  Wr32(base + kInBink, 1);
  fn(ctx, base);
  Wr32(base + kInBink, 0);
  return ctx.r3.u32;
}

}  // namespace

REX_EXTERN(__imp__sub_82592EA8);
REX_HOOK_RAW(sub_82592EA8) {
  // (off unless asked: see the top. SVR2011_TEST_MOVIE_NO_CATCH_UP=1 still means off)
  static const char* mode = std::getenv("SVR2011_TEST_MOVIE_CATCH_UP");
  static const bool off = !mode || !*mode || std::getenv("SVR2011_TEST_MOVIE_NO_CATCH_UP") != nullptr;
  static const bool skip = !mode || std::string(mode) != "decode";
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t count = mgr ? std::min<uint32_t>(Rd32(base + mgr + 8), 8) : 0;
  const uint32_t list = mgr ? Rd32(base + mgr + 4) : 0;
  uint32_t stepped = 0;  // (bit k: movie k had a decoded frame to step)
  for (uint32_t k = 0; k < count && list; ++k) {
    const uint32_t m = Rd32(base + list + 4 * k);
    if (m && Rd32(base + m + 8) == 0 && Rd32(base + m + 12) && Rd32(base + m + 20) == 1 && Rd32(base + m + 24) == 0)
      stepped |= 1u << k;
  }
  __imp__sub_82592EA8(ctx, base);
  if (!stepped) return;
  const auto saved = ctx;
  for (uint32_t k = 0; k < count; ++k) {
    if (!(stepped & (1u << k))) continue;
    const uint32_t m = Rd32(base + list + 4 * k);
    const uint32_t bink = Rd32(base + m + 12);
    if (!bink || Rd32(base + m + 20) != 0) continue;  // (not stepped after all)
    const auto now = Clock::now();
    const double due = DueFrame(base, bink, now);
    int n = 0;
    while (!off && n < kMaxCatchUp && Rd32(base + bink + 12) < Rd32(base + bink + 8) &&
           Rd32(base + bink + 12) + 1.0 < due) {  // (over a frame behind its clock)
      if (!skip) Wr32(base + bink + kSkipsInARow, kMaxSkipsInARow);  // (decoded, not skipped: see the top)
      Call(ctx, base, sub_82D4BCF8, bink);  // (BinkNextFrame)
      ++n;
    }
    if (n) {
      static uint32_t frames = 0, logged = 0;
      frames += n;
      if (++logged <= 5 || logged % 500 == 0)
        REXLOG_INFO("movies: {} frame(s) {} to keep up with the sound (bink {:08X} frame {}; {} so far)", n,
                    skip ? "skipped" : "decoded at once", bink, Rd32(base + bink + 12), frames);
    }
    Ended(base, bink, uint32_t(n), now);
  }
  ctx = saved;
}
