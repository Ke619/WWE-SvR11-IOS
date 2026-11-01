// WWE SmackDown vs. Raw 2011 - saving a video clip (pause menu > HIGHLIGHT
// REEL > VIDEO CLIP > SAVE) when the game's block heap is full.
//
// The block heap (sub_821DC338(size, align) -> a handle, 0 if no room): up to
// 8 regions, their bases at 0x82E368A8 (the init: sub_821DB908); region i's
// descriptor is 16 bytes at region 0's base + 16 * i (+0 first block, +4 size,
// +8 free bytes, +12 kind). Blocks are a list: +0 next, +4 previous, +8 size
// (bit 31: in use), +12 align, +16 the handle, data at +20. A handle is +0 the
// data, +4 the size asked for; the heap compacts (sub_821DC110 /
// sub_821DBF00) and tries again before it gives up. sub_821DB638(handle)
// frees. In a match: two 6.5 MB regions; region 0 holds ~3.6 MB (179 blocks,
// code at 0x82254000-0x82262000), region 1 is empty.
//
// The save (sub_8242D708, a state machine, state at +0x8CF4) in state 37 gets
// a 5.5 MB stream buffer from it (0x580000, align 16). For the post-match
// MATCH HIGHLIGHTS save (+2760 = 1) the game checks the handle and goes to
// its failure state 43 (frees what the save holds, a cancel sound, state 44
// leaves); for the in-match video clip it doesn't, and read through the null
// handle (a player's crash 12 minutes into a Match Creator match, CAW vs The
// Sandman, 2.0.6). Here state 37 gets the buffer first: if there's no room,
// it goes to 43 as the other save does and the screen says so; else the
// game's own call is handed that buffer. Both saves write NNRecordingDat.rec.
//
// The heap's state is logged at each video clip save, and at every failed
// allocation (once per caller): each region's size, free bytes and largest
// free block, and the biggest blocks in use with the code that asked for them
// (recorded here).
//
// Test aid: SVR2011_TEST_REEL_NO_ROOM=<n> - the n-th video clip save of the
// run finds no room for its buffer (1 = the first).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "fps_overlay.h"
#include "generated/default/svr2011_init.h"

namespace {
// (guest 0xE0000000 and up - physical pages, where the heap's regions are - sit 4 KB further on the host)
uint8_t* At(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

constexpr uint32_t kRegions = 0x82E368A8;
constexpr uint32_t kSaveState = 0x8CF4;     // the save's state
constexpr uint32_t kSaveKind = 2760;        // 1 = post-match highlights, else the in-match video clip
constexpr uint32_t kStreamSize = 0x580000;  // the save's stream buffer
constexpr uint32_t kReelCall = 0x8242EBD8;  // (return address of the clip save's own allocation)

struct Owner { uint32_t caller, size; };
std::mutex g_mutex;
std::unordered_map<uint32_t, Owner> g_owners;  // handle -> who asked for it (the latest use of a handle)
std::set<uint32_t> g_logged;                   // callers whose failure was logged
uint32_t g_stash = 0;                          // the clip buffer got ahead (sub_8242D708 below)

// The heap's state (a save, or a failed allocation).
void LogHeap(uint8_t* base) {
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0) return;
  struct Used { uint32_t size, handle; };
  std::vector<Used> used;
  for (int i = 0; i < 8; ++i) {
    const uint32_t region = Rd32(At(base, kRegions + 4 * i));
    if (!region) continue;
    const uint32_t d = desc0 + 16 * i;
    const uint32_t region_size = Rd32(At(base, d + 4));
    uint32_t blocks = 0, in_use = 0, largest = 0, used_bytes = 0;
    uint32_t b = Rd32(At(base, d));
    for (; b && blocks < 200000; b = Rd32(At(base, b)), ++blocks) {
      if (b < region || b + 20 > region + region_size) break;  // (not this region's: stop reading)
      const uint32_t sz = Rd32(At(base, b + 8));
      if (sz & 0x80000000) {
        ++in_use, used_bytes += sz & 0x7FFFFFFF;
        used.push_back({sz & 0x7FFFFFFF, Rd32(At(base, b + 16))});
      } else {
        largest = std::max(largest, sz);
      }
    }
    REXLOG_WARN("[svr2011] block heap: region {} at {:08X}: size {} KB, free {} KB (largest {} KB), {} blocks in use "
                "({} KB){}",
                i, region, region_size / 1024, Rd32(At(base, d + 8)) / 1024, largest / 1024, in_use, used_bytes / 1024,
                b ? fmt::format(" - list left the region at block {} ({:08X}; descriptor {:08X}: {:08X} {:08X} {:08X} {:08X})",
                                blocks, b, d, Rd32(At(base, d)), Rd32(At(base, d + 4)), Rd32(At(base, d + 8)), Rd32(At(base, d + 12)))
                  : std::string());
  }
  std::sort(used.begin(), used.end(), [](const Used& a, const Used& b) { return a.size > b.size; });
  // Totals per caller too: many small blocks add up.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> by_caller;  // caller -> (bytes, count)
  std::string top;
  {
    std::lock_guard lock(g_mutex);
    for (size_t k = 0; k < used.size(); ++k) {
      const auto it = g_owners.find(used[k].handle);
      const uint32_t caller = it != g_owners.end() ? it->second.caller : 0;
      auto& c = by_caller[caller];
      c.first += used[k].size, ++c.second;
      if (k < 16) top += fmt::format(" {}K@{:08X}", used[k].size / 1024, caller);
    }
  }
  REXLOG_WARN("[svr2011] block heap: biggest blocks (size@caller, 0 = before logging):{}", top);
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> callers(by_caller.begin(), by_caller.end());
  std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  std::string per;
  for (size_t k = 0; k < callers.size() && k < 12; ++k)
    per += fmt::format(" {:08X}:{}K/{}", callers[k].first, callers[k].second.first / 1024, callers[k].second.second);
  REXLOG_WARN("[svr2011] block heap: in use by caller (KB/blocks):{}", per);
}
}  // namespace

// The list guard. Two players' crashes (25 + 30 Oct, a 6-man with four superstar mods in an arena mod: lr 821DBCA8 /
// 821DBD14) walked into a block whose list link held data - AAAAAAAA, 95D515D5: the block before it in memory had
// written past its end over the header. The allocation's walk (sub_821DC338 -> sub_821DBCA0(descriptor, block) for
// each block) then read through it. Here a region's first block outside the region is put back (it's always the
// region's start), or the region skipped (free bytes 0) if that isn't a block; and a block whose next link leaves its
// region is mended - the block after it found again by its previous link, the broken one kept out of use - or, with
// none found, the list ends there: some room lost instead of a crash. Logged with the header and who asked for the
// block before it (the likely writer).
namespace {
std::atomic<int> g_cut{0};
thread_local uint32_t t_walked = 0;  // (the block the walk saw before this one)

bool GuardOn() {  // (test aid SVR2011_TEST_BLOCK_GUARD_OFF=1: the game's own walk, as before)
  static const bool on = std::getenv("SVR2011_TEST_BLOCK_GUARD_OFF") == nullptr;
  return on;
}

// (descriptor d's region: its base and size; false if d isn't one)
bool RegionOf(uint8_t* base, uint32_t d, uint32_t& lo, uint32_t& size) {
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0 || d < desc0 || d >= desc0 + 128 || (d - desc0) % 16) return false;
  lo = Rd32(At(base, kRegions + (d - desc0) / 4)), size = Rd32(At(base, d + 4));
  return lo != 0;
}
bool InRegion(uint32_t b, uint32_t lo, uint32_t size) { return b >= lo && b - lo <= size - 20 && (b & 3) == 0; }

std::string Owner(uint32_t block, uint8_t* base) {
  if (!block) return "none";
  const uint32_t handle = Rd32(At(base, block + 16));
  std::lock_guard lock(g_mutex);
  const auto it = g_owners.find(handle);
  return it == g_owners.end() ? fmt::format("block {:08X}, handle {:08X} (caller not seen)", block, handle)
                              : fmt::format("block {:08X}, {} bytes asked by {:08X}", block, it->second.size, it->second.caller);
}

void GuardHeads(uint8_t* base) {
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0) return;
  for (int i = 0; i < 8; ++i) {
    const uint32_t d = desc0 + 16 * i;
    uint32_t lo, size;
    if (!RegionOf(base, d, lo, size)) continue;
    const uint32_t head = Rd32(At(base, d));
    if (!head || InRegion(head, lo, size) || !GuardOn()) continue;
    const uint32_t first = lo + (i == 0 ? 0x8080 : 0);  // (sub_821DB908: region 0 starts with the descriptors and slots)
    const bool block = Rd32(At(base, first + 4)) == 0 && (Rd32(At(base, first + 8)) & 0x7FFFFFFF) <= size;
    if (block) Wr32(At(base, d), first);
    else Wr32(At(base, d + 8), 0);
    if (g_cut++ < 20)
      REXLOG_ERROR("[svr2011] block heap: region {} ({:08X}) first block {:08X} - overwritten; {}", i, lo, head,
                   block ? fmt::format("its start {:08X} put back", first) : std::string("the region skipped"));
  }
}

// (block b's header overwritten: the block after it - its previous link is b - found again; b rebuilt as in use (never
// handed out: its room may still be written) up to it. 0 if none: the list then ends at b)
uint32_t Mend(uint8_t* base, uint32_t b, uint32_t lo, uint32_t size) {
  for (uint32_t p = b + 20; p + 20 <= lo + size; p += 4) {
    if (Rd32(At(base, p + 4)) != b) continue;
    const uint32_t pn = Rd32(At(base, p));
    if (pn && !InRegion(pn, lo, size)) continue;
    Wr32(At(base, b), p);
    if (t_walked && InRegion(t_walked, lo, size)) Wr32(At(base, b + 4), t_walked);
    Wr32(At(base, b + 8), 0x80000000u | (p - b - 20));
    return p;
  }
  Wr32(At(base, b), 0);
  return 0;
}

// Test aid: SVR2011_TEST_BLOCK_CHECK=1 - every allocation first checks each block's header (next in the region, its
// previous link back, past the block's room); the first bad one logged with the block before it - who asked for it,
// how much, and the room it had: the writer of an overrun, found soon after it ran over.
void CheckAll(uint8_t* base) {
  static const bool on = std::getenv("SVR2011_TEST_BLOCK_CHECK") != nullptr;
  static std::atomic<int> logged{0};
  if (!on || logged.load() >= 10) return;
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0) return;
  for (int i = 0; i < 8; ++i) {
    uint32_t lo, size;
    if (!RegionOf(base, desc0 + 16 * i, lo, size)) continue;
    uint32_t b = Rd32(At(base, desc0 + 16 * i));
    for (int n = 0; b && n < 200000; ++n) {
      const uint32_t next = Rd32(At(base, b)), room = Rd32(At(base, b + 8)) & 0x7FFFFFFF;
      const char* bad = !next ? nullptr
                        : !InRegion(next, lo, size)       ? "next outside the region"
                        : Rd32(At(base, next + 4)) != b   ? "its previous link not back"
                        : next < b + 20 + room            ? "inside the block before"
                                                          : nullptr;
      if (bad) {
        if (logged++ < 10)
          REXLOG_ERROR("[svr2011] block heap check: block {:08X} after {:08X}: {} (header {:08X} {:08X} {:08X} {:08X} {:08X});"
                       " the block before: {}, room {} bytes{}", next, b, bad, Rd32(At(base, next)), Rd32(At(base, next + 4)),
                       Rd32(At(base, next + 8)), Rd32(At(base, next + 12)), Rd32(At(base, next + 16)), Owner(b, base), room,
                       Rd32(At(base, b + 8)) & 0x80000000 ? ", in use" : ", free");
        break;
      }
      b = next;
    }
  }
}

// Test aid: SVR2011_TEST_BLOCK_SMASH=<n> - the n-th allocation in a match first writes 95D515D5 (a 30 Oct crash's
// link) over region 0's third block's header (as the block before it running past its end would).
void Smash(uint8_t* base) {
  static const int at = [] { const char* v = std::getenv("SVR2011_TEST_BLOCK_SMASH"); return v ? std::atoi(v) : 0; }();
  static std::atomic<int> n{0};
  if (!at || n.load() > at) return;
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0 || ++n != at) return;
  uint32_t b = Rd32(At(base, desc0));
  for (int k = 0; k < 2 && b; ++k) b = Rd32(At(base, b));
  if (!b) return;
  for (uint32_t k = 0; k < 20; k += 4) Wr32(At(base, b + k), 0x95D515D5);
  REXLOG_WARN("[svr2011] block heap: test - block {:08X}'s header overwritten (allocation {})", b, at);
}
}  // namespace

REX_EXTERN(__imp__sub_821DBCA0);
REX_HOOK_RAW(sub_821DBCA0) {
  const uint32_t d = ctx.r3.u32, b = ctx.r4.u32;
  uint32_t lo, size;
  if (GuardOn() && RegionOf(base, d, lo, size) && InRegion(b, lo, size)) {
    const uint32_t next = Rd32(At(base, b));
    if (next && !InRegion(next, lo, size)) {
      const uint32_t h[5] = {next, Rd32(At(base, b + 4)), Rd32(At(base, b + 8)), Rd32(At(base, b + 12)), Rd32(At(base, b + 16))};
      const uint32_t mended = Mend(base, b, lo, size);
      if (g_cut++ < 20)
        REXLOG_ERROR("[svr2011] block heap: block {:08X} (region {:08X}) overwritten - header {:08X} {:08X} {:08X} {:08X} "
                     "{:08X}; {}. The block before it: {}", b, lo, h[0], h[1], h[2], h[3], h[4],
                     mended ? fmt::format("the next block {:08X} found again, it kept out of use", mended)
                            : std::string("the list ends there"), Owner(t_walked, base));
      t_walked = b;
      ctx.r3.u64 = 0;  // (no room here: the walk goes on to the next block)
      return;
    }
  }
  t_walked = b;
  __imp__sub_821DBCA0(ctx, base);
}

REX_EXTERN(__imp__sub_821DC338);
REX_HOOK_RAW(sub_821DC338) {
  const uint32_t caller = ctx.lr;
  const uint32_t size = ctx.r3.u32, align = ctx.r4.u32;
  Smash(base);
  CheckAll(base);
  GuardHeads(base);
  t_walked = 0;
  if (caller == kReelCall && size == kStreamSize) {
    std::lock_guard lock(g_mutex);
    if (g_stash) {
      ctx.r3.u64 = g_stash;
      g_stash = 0;
      return;
    }
  }
  __imp__sub_821DC338(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (handle) {
    std::lock_guard lock(g_mutex);
    g_owners[handle] = {caller, size};
    return;
  }
  bool first;
  {
    std::lock_guard lock(g_mutex);
    first = g_logged.insert(caller).second;
  }
  REXLOG_WARN("[svr2011] block heap: no room for {} KB (align {}), asked by {:08X}{}", size / 1024, align, caller,
              first ? "" : " (again)");
  if (first) LogHeap(base);
}

// The save, each frame.
REX_EXTERN(__imp__sub_8242D708);
REX_EXTERN(__imp__sub_821DB638);
REX_HOOK_RAW(sub_8242D708) {
  const uint32_t save = ctx.r3.u32;
  if (Rd32(At(base, save + kSaveState)) != 37 || Rd32(At(base, save + kSaveKind)) == 1) {
    __imp__sub_8242D708(ctx, base);
    return;
  }
  static const int no_room_at = [] { const char* v = std::getenv("SVR2011_TEST_REEL_NO_ROOM"); return v ? std::atoi(v) : 0; }();
  static int saves = 0;
  const bool no_room = ++saves == no_room_at;
  REXLOG_INFO("[svr2011] video clip: saving; the block heap before its buffer:");
  LogHeap(base);
  const auto saved = ctx;
  uint32_t handle = 0;
  if (!no_room) {
    ctx.r3.u64 = kStreamSize, ctx.r4.u64 = 16;
    ctx.lr = 0x8242D708;  // (not the save's own call)
    sub_821DC338(ctx, base);
    handle = ctx.r3.u32;
  }
  ctx = saved;
  if (!handle) {
    REXLOG_WARN("[svr2011] video clip: no room for its {} KB buffer - not saved", kStreamSize / 1024);
    Wr32(At(base, save + kSaveState), 43);  // (the game's failure state, as for a replay)
    svr2011::ShowNotice("The video clip couldn't be saved: not enough memory left.", 8.0f);
    __imp__sub_8242D708(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stash = handle;
  }
  __imp__sub_8242D708(ctx, base);
  uint32_t left;
  {
    std::lock_guard lock(g_mutex);
    left = g_stash;
    g_stash = 0;
  }
  if (left) {  // (the state didn't get to the allocation)
    const auto after = ctx;
    ctx.r3.u64 = left;
    __imp__sub_821DB638(ctx, base);
    ctx = after;
  }
}
