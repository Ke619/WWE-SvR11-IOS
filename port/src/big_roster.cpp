// WWE SmackDown vs. Raw 2011 - the big roster (cvar big_roster, experimental, off by default).
//
// The game gives each character id (0-321) a record index (0x82DB3610, read by sub_828B7038 only; 242 indexed) and
// keeps one entry per index in about ten tables: the save's records, profiles and 40 B entries, the roster
// snapshot, the loaded CHAR/PRO tables, ... (docs/UNLIMITED_MOD_SLOTS.md, "Part 2"). Here those tables grow to
// 512 indices: indices 0-241 stay where they are (the stock game and SaveData.dat are unchanged), an index from 242
// up goes to an extension buffer of its table. The game's code reaches the tables at known instructions; each has
// a mid-instruction hook ([[midasm_hook]] in svr2011_config.toml, below "big roster") that does nothing while the
// cvar is off. Kinds of site:
// - an address: right after "add d, a, b" made table + index * size, an index from 242 moves d into the extension
//   (the extension sits "as if" after the table: d + shift, so a field offset added before or after still lands
//   right);
// - a bound: right after a compare with 242, the compare is made again against the live index count (a loop's
//   end, or a check that an index is valid).
// Each site logs once the first time it sends an index to an extension.
#include "big_roster.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(big_roster, false, "Gameplay",
                    "Experimental: the game's per-superstar tables grow from 242 to 512 entries (more superstar mods "
                    "at once)");

namespace {

rex::memory::Memory* g_memory = nullptr;
bool g_on = false;

constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id (0-321), 512 = no index
// The save's per-index tables (what an index taken from 242 up starts as: its old entry's copy)
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260, kProfiles = 0x82E7C920, kProfileSize = 1056,
                   kEntries = 0x82E7C918 + 0x3E648, kEntrySize = 40;

// An extension: entries 242-511 of the table at start (stride bytes each), with one entry of room before and after
// (a site's base may be a field before the table's start: matched within one entry).
struct Ext {
  uint32_t start, stride, guest;
};
std::mutex g_ext_mutex;
std::vector<Ext> g_ext;

uint16_t g_hi_index[svr2011::kMaxIds - 322];  // the record index of ids 322-511 (512 = none)
uint32_t g_count = svr2011::kStockIndices;     // the live index count: 242 + the indices given out from 242
uint16_t g_ext_id[svr2011::kMaxIndices - svr2011::kStockIndices];  // the id at index 242 + k
// A loop over the record indices that a bound site let past 241: its index (= its list position) this turn, which
// sub_828B7060 (position -> id) gives the index's id for - positions 242-291 are otherwise the 50 CAWs.
std::atomic<int32_t> g_ext_pos{-1};

uint8_t* Host(uint32_t guest) { return g_memory->TranslateVirtual<uint8_t*>(guest); }
uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// The shift from an address in the table (as if it went on past 241) to the same place in the extension.
int64_t Shift(uint32_t start, uint32_t stride) {
  std::lock_guard<std::mutex> lock(g_ext_mutex);
  for (const Ext& e : g_ext)
    if (e.stride == stride && int64_t(start) > int64_t(e.start) - stride && int64_t(start) < int64_t(e.start) + stride)
      return int64_t(e.guest) - (int64_t(e.start) + int64_t(svr2011::kStockIndices) * stride);
  const uint32_t room = (svr2011::kMaxIndices - svr2011::kStockIndices + 2) * stride;
  const uint32_t at = g_memory->SystemHeapAlloc(room, 0x40);
  if (!at) {
    REXLOG_ERROR("[svr2011] big roster: no memory for the extension of the table at {:08X} ({} B entries)", start,
                 stride);
    return 0;
  }
  std::memset(Host(at), 0, room);
  g_ext.push_back({start, stride, at + stride});
  REXLOG_INFO("[svr2011] big roster: table {:08X} ({} B entries) - entries 242-511 at {:08X}", start, stride,
              at + stride);
  return int64_t(at + stride) - (int64_t(start) + int64_t(svr2011::kStockIndices) * stride);
}

// An address site: d = the table address the add made, o = the add's other operand left (the table base when
// o_is_base, else index * size), post = what the code adds to the base after the hook (the table's start = base + post).
void AddressSite(std::atomic<bool>& logged, const char* name, PPCRegister& d, const PPCRegister& o, uint32_t size,
                 int32_t post, bool o_is_base) {
  if (!g_on) return;
  const uint32_t product = o_is_base ? d.u32 - o.u32 : o.u32;
  const uint32_t index = product / size;
  if (index < svr2011::kStockIndices || index >= svr2011::kMaxIndices || product % size) return;
  const uint32_t start = d.u32 - product + uint32_t(post);
  const int64_t shift = Shift(start, size);
  if (!shift) return;
  d.u64 = uint32_t(int64_t(d.u32) + shift);
  if (!logged.exchange(true))
    REXLOG_INFO("[svr2011] big roster: site {} sends index {} to the extension of {:08X}", name, index, start);
}

// A bound site: right after "cmpwi / cmplwi cr, i, 242": the compare again against the live count.
void BoundSite(std::atomic<bool>& logged, const char* name, PPCCRRegister& cr, const PPCXERRegister& xer,
               const PPCRegister& i, bool is_signed, bool loop) {
  if (!g_on || g_count == svr2011::kStockIndices) return;
  if (is_signed)
    cr.compare<int32_t>(i.s32, int32_t(g_count), xer);
  else
    cr.compare<uint32_t>(i.u32, g_count, xer);
  if (loop) g_ext_pos = i.u32 >= svr2011::kStockIndices && i.u32 < g_count ? int32_t(i.u32) : -1;
  if (i.u32 >= svr2011::kStockIndices && i.u32 < g_count && !logged.exchange(true))
    REXLOG_INFO("[svr2011] big roster: site {} lets index {} past 241", name, i.u32);
}

// A walk site: a loop over the indices stepping pointers through tables (and on into what follows them, the
// CAWs' part): right after its "cmpwi i, 242", the bound as above, and when i has just reached 242 each pointer
// jumps to its table's extension (the code after the loop starts its own pointers again).
void WalkSite(std::atomic<bool>& logged, const char* name, PPCCRRegister& cr, const PPCXERRegister& xer,
              const PPCRegister& i, PPCRegister* p, const uint32_t* size, int n) {
  BoundSite(logged, name, cr, xer, i, true, true);
  if (!g_on || g_count == svr2011::kStockIndices || i.u32 != svr2011::kStockIndices) return;
  for (int k = 0; k < n; ++k) {
    const uint32_t start = p[k].u32 - svr2011::kStockIndices * size[k];
    if (const int64_t shift = Shift(start, size[k])) p[k].u64 = uint32_t(int64_t(p[k].u32) + shift);
  }
}

}  // namespace

// ---- the sites (named in svr2011_config.toml's [[midasm_hook]] entries; registers d, o / cr6, xer, i)
#define BIG_ROSTER_ADDRESS(name, size, post, o_is_base)                     \
  void name(PPCRegister& d, PPCRegister& o) {                               \
    static std::atomic<bool> logged;                                        \
    AddressSite(logged, #name, d, o, size, post, o_is_base);                \
  }
#define BIG_ROSTER_BOUND(name, is_signed)                                   \
  void name(PPCCRRegister& cr, PPCXERRegister& xer, PPCRegister& i) {       \
    static std::atomic<bool> logged;                                        \
    BoundSite(logged, #name, cr, xer, i, is_signed, true);                  \
  }
#define BIG_ROSTER_WALK1(name, size)                                        \
  void name(PPCCRRegister& cr, PPCXERRegister& xer, PPCRegister& i, PPCRegister& p) {\
    static std::atomic<bool> logged;                                        \
    const uint32_t sizes[] = {size};                                        \
    WalkSite(logged, #name, cr, xer, i, &p, sizes, 1);                      \
  }
#define BIG_ROSTER_WALK2(name, size_a, size_b)                              \
  void name(PPCCRRegister& cr, PPCXERRegister& xer, PPCRegister& i, PPCRegister& a, PPCRegister& b) {\
    static std::atomic<bool> logged;                                        \
    PPCRegister p[] = {a, b};                                               \
    const uint32_t sizes[] = {size_a, size_b};                              \
    WalkSite(logged, #name, cr, xer, i, p, sizes, 2);                       \
    a = p[0], b = p[1];                                                     \
  }
#define BIG_ROSTER_CHECK(name)                                              \
  void name(PPCCRRegister& cr, PPCXERRegister& xer, PPCRegister& i) {       \
    static std::atomic<bool> logged;                                        \
    BoundSite(logged, #name, cr, xer, i, true, false);                      \
  }

// The save's records (save + 0, 260 B): by index (sub_8257A238, the copy in sub_8257A2C0), by id (sub_8257AF18).
BIG_ROSTER_ADDRESS(BigRosterRecord_8257A244, 260, 0, false)
BIG_ROSTER_ADDRESS(BigRosterRecord_8257A2D0, 260, 0, false)
BIG_ROSTER_ADDRESS(BigRosterRecord_8257AF84, 260, 0, false)
// The save's profiles (block B + 8, 1056 B) by id (sub_8257BB48).
BIG_ROSTER_ADDRESS(BigRosterProfile_8257BBBC, 1056, 0, false)
// The save's 40 B entries (block B + 0x3E648) by id (sub_8257BBD8) and the defaults' build (sub_8257BDB8).
BIG_ROSTER_ADDRESS(BigRosterEntry_8257BC58, 40, 0, false)
BIG_ROSTER_ADDRESS(BigRosterEntry_8257BE68, 40, 0, true)
// The PRO loaders (sub_82594C78, sub_82595240: the disc's CHAR/PRO into the save's profiles, block B + 8 + i x 1056):
// the profile's address and the loop over the indices.
BIG_ROSTER_ADDRESS(BigRosterProfile_82594CFC, 1056, 8, true)
BIG_ROSTER_BOUND(BigRosterBound_82594D34, false)
BIG_ROSTER_ADDRESS(BigRosterProfile_825952B0, 1056, 8, true)
BIG_ROSTER_BOUND(BigRosterBound_825952E8, false)
// The loaded CHAR tables (an object: u32, 242 x 260 DAT at +4, 242 x 1056 PRO at +0xF5CC; two of them, at *(o) and
// *(o + 4)): sub_8259C7D0 / C838 / C8A8 / C9A8 / CAA8 / CBF8.
BIG_ROSTER_ADDRESS(BigRosterChar_8259C804, 260, 4, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259C86C, 1056, 0xF5CC, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259C928, 260, 4, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259C958, 1056, 0xF5CC, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259CA28, 260, 4, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259CA58, 1056, 0xF5CC, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259CB1C, 260, 4, false)
BIG_ROSTER_ADDRESS(BigRosterChar_8259CB4C, 1056, 0xF5CC, true)
BIG_ROSTER_ADDRESS(BigRosterChar_8259CC30, 260, 4, true)
// The roster snapshot (*0x82E3DB34; per list position 8 B at +0 and 18 B at +2336, then the CAWs' 50; 40 B at
// +26548; records 260 B at +0x9554): taken (sub_8243A380, A9E0), put back (A488, AAA0), compared (A590, AB60,
// AC98), a record by id (A920, AD48, C068).
BIG_ROSTER_WALK2(BigRosterSnapWalk_8243A3F4, 8, 18)
BIG_ROSTER_WALK2(BigRosterSnapWalk_8243A4F0, 8, 18)
BIG_ROSTER_WALK2(BigRosterSnapWalk_8243A624, 8, 18)
BIG_ROSTER_WALK1(BigRosterSnapWalk_8243AD34, 18)
BIG_ROSTER_WALK1(BigRosterSnapWalk_8243AA3C, 40)
BIG_ROSTER_WALK1(BigRosterSnapWalk_8243AAFC, 40)
BIG_ROSTER_WALK1(BigRosterSnapWalk_8243ABB8, 40)
BIG_ROSTER_ADDRESS(BigRosterSnap_8243A99C, 260, 0x9554, true)
BIG_ROSTER_ADDRESS(BigRosterSnap_8243ADB8, 260, 0x9554, true)
BIG_ROSTER_ADDRESS(BigRosterSnap_8243C130, 260, 0x9554, false)
// A 64 B list per index (*0x82E3DB30 + 52: 14 values, count +56, cursor +60 - written back; the CAWs' 50 at
// +15540, 18848 B in all): sub_82438BA0, C30.
BIG_ROSTER_ADDRESS(BigRosterList_82438BE0, 64, 52, true)
BIG_ROSTER_ADDRESS(BigRosterList_82438C70, 64, 52, true)
// (their "index < 242" checks before the address)
BIG_ROSTER_CHECK(BigRosterCheck_8259C7F4)
BIG_ROSTER_CHECK(BigRosterCheck_8259C85C)
BIG_ROSTER_CHECK(BigRosterCheck_8259C918)
BIG_ROSTER_CHECK(BigRosterCheck_8259C948)
BIG_ROSTER_CHECK(BigRosterCheck_8259CA18)
BIG_ROSTER_CHECK(BigRosterCheck_8259CA48)
BIG_ROSTER_CHECK(BigRosterCheck_8259CB0C)
BIG_ROSTER_CHECK(BigRosterCheck_8259CB3C)
BIG_ROSTER_CHECK(BigRosterCheck_8259CC20)

// The select screen's portrait cache (sub_82741FC0: 268 slots of 8 B at cache + 48, slot = record index; 242 = none,
// 243 up the CAW tiles, filled when first shown): an index from 242 up takes a slot from 268 up, in the extension
// of the slots (slot 268 + k = index 242 + k).
constexpr uint32_t kPortraitSlots = 268;
void BigRosterPortraitIndex_82741FE4(PPCRegister& slot) {  // (after the id -> index call)
  if (g_on && slot.u32 >= svr2011::kStockIndices && slot.u32 < svr2011::kMaxIndices)
    slot.u64 = kPortraitSlots + slot.u32 - svr2011::kStockIndices;
}
void BigRosterPortraitBound_82742024(PPCCRRegister& cr, PPCXERRegister& xer, PPCRegister& slot) {  // (slot < 268)
  if (g_on && slot.u32 >= kPortraitSlots && slot.u32 < kPortraitSlots + svr2011::kMaxIndices - svr2011::kStockIndices)
    cr.compare<int32_t>(0, 1, xer);
}
namespace {
void PortraitSlot(std::atomic<bool>& logged, const char* name, PPCRegister& d, const PPCRegister& cache,
                  const PPCRegister& slot) {
  if (!g_on || slot.u32 < kPortraitSlots || slot.u32 >= kPortraitSlots + svr2011::kMaxIndices - svr2011::kStockIndices)
    return;
  const uint32_t index = svr2011::kStockIndices + slot.u32 - kPortraitSlots;
  d.u64 = svr2011::RosterEntry(cache.u32 + 48, 8, index);
  if (!logged.exchange(true))
    REXLOG_INFO("[svr2011] big roster: site {} sends index {} to the portrait slots' extension", name, index);
}
}  // namespace
void BigRosterPortrait_82742034(PPCRegister& d, PPCRegister& cache, PPCRegister& slot) {
  static std::atomic<bool> logged;
  PortraitSlot(logged, "BigRosterPortrait_82742034", d, cache, slot);
}
void BigRosterPortrait_8274205C(PPCRegister& d, PPCRegister& cache, PPCRegister& slot) {
  static std::atomic<bool> logged;
  PortraitSlot(logged, "BigRosterPortrait_8274205C", d, cache, slot);
}

namespace svr2011 {

bool BigRosterOn() { return g_on; }

uint32_t RosterEntry(uint32_t start, uint32_t stride, uint32_t index) {
  if (index < kStockIndices || !g_on || index >= kMaxIndices) return start + index * stride;
  return uint32_t(int64_t(start + index * stride) + Shift(start, stride));
}

uint32_t RosterIndex(const uint8_t* base, uint32_t id) {
  if (id < 322) return Rd16(base + kIdToIndex + id * 2);
  if (g_on && id < kMaxIds) return g_hi_index[id - 322];
  return 512;
}

uint32_t BigRosterGiveIndex(uint8_t* base, uint32_t id, uint32_t like) {
  if (!g_on || id < kFirstHighId || id >= kMaxIds || g_count >= kMaxIndices) return 512;
  if (g_hi_index[id - 322] != 512) return g_hi_index[id - 322];
  const uint32_t to = g_count++, from = like < 322 ? Rd16(base + kIdToIndex + like * 2) : 512;
  g_hi_index[id - 322] = uint16_t(to);
  g_ext_id[to - kStockIndices] = uint16_t(id);
  // (its entries start as its base's: the mod's own data goes over them as a pool id's does)
  if (from < kStockIndices)
    for (const auto& [start, size] : {std::pair{kRecords, kRecordSize}, {kProfiles, kProfileSize}, {kEntries, kEntrySize}})
      std::memcpy(base + RosterEntry(start, size, to), base + start + from * size, size);
  return to;
}

void BigRosterTestMove(uint8_t* base, const uint32_t* ids, int count) {
  if (!g_on) return;
  const char* env = std::getenv("SVR2011_TEST_HIGH_INDEX");
  int n = env ? std::atoi(env) : 0;
  n = std::min({n, count, int(kMaxIndices - kStockIndices)});
  if (n <= 0) return;
  // (the id -> index table is in the image: writable for it)
  auto* heap = g_memory->LookupHeap(kIdToIndex);
  uint32_t old = 0;
  if (!heap || !heap->QueryProtect(kIdToIndex, &old) ||
      !heap->Protect(kIdToIndex, 322 * 2, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_WARN("[svr2011] big roster: test aid - the id table can't be made writable");
    return;
  }
  for (int k = 0; k < n; ++k) {
    const uint32_t id = ids[k];
    if (id >= 322) continue;
    const uint32_t from = Rd16(base + kIdToIndex + id * 2), to = g_count;
    if (to >= kMaxIndices) break;
    base[kIdToIndex + id * 2] = uint8_t(to >> 8);
    base[kIdToIndex + id * 2 + 1] = uint8_t(to);
    g_count = to + 1;
    g_ext_id[to - kStockIndices] = uint16_t(id);
    for (const auto& [start, size] : {std::pair{kRecords, kRecordSize}, {kProfiles, kProfileSize}, {kEntries, kEntrySize}})
      std::memcpy(base + RosterEntry(start, size, to), base + start + from * size, size);
    REXLOG_INFO("[svr2011] big roster: test aid - id {} index {} -> {}", id, from, to);
  }
  heap->Protect(kIdToIndex, 322 * 2, old);
}

void BigRosterTestCard(uint8_t* base) {
  const char* env = std::getenv("SVR2011_TEST_UNIVERSE_CARD");
  if (!env) return;
  const uint32_t save = uint32_t(base[0x82E407B4]) << 24 | uint32_t(base[0x82E407B5]) << 16 |
                       uint32_t(base[0x82E407B6]) << 8 | base[0x82E407B7];
  if (!save) return;
  const uint32_t cards = save + 0x364B4 + 14942, end = save + 0x364B4 + 20808;  // (88 B blocks: 16 B, 6 x 12 B people)
  int changed = 0;
  for (const char* at = env; *at;) {
    unsigned from = 0, to = 0;
    if (std::sscanf(at, "%u:%u", &from, &to) != 2) break;
    for (uint32_t b = cards; b + 88 <= end; b += 88)
      for (uint32_t k = 0; k < 6; ++k) {
        uint8_t* id = base + b + 16 + k * 12;
        if (Rd16(id) != from) continue;
        id[0] = uint8_t(to >> 8), id[1] = uint8_t(to);
        ++changed;
      }
    const char* comma = std::strchr(at, ',');
    if (!comma) break;
    at = comma + 1;
  }
  REXLOG_INFO("[svr2011] big roster: test aid - {} Universe card place(s) given another person ({})", changed, env);
}

void InstallBigRoster(rex::memory::Memory* memory) {
  g_memory = memory;
  g_on = REXCVAR_GET(big_roster);
  std::fill(std::begin(g_hi_index), std::end(g_hi_index), uint16_t(512));
  if (g_on) REXLOG_INFO("[svr2011] big roster: on (record indices up to 511)");
}

}  // namespace svr2011

// id -> record index: ids 322-511 from our table (the game's ends at 321).
REX_EXTERN(__imp__sub_828B7038);
REX_HOOK_RAW(sub_828B7038) {
  if (g_on && ctx.r3.u32 >= 322 && ctx.r3.u32 < svr2011::kMaxIds) {
    ctx.r3.u64 = g_hi_index[ctx.r3.u32 - 322];
    return;
  }
  __imp__sub_828B7038(ctx, base);
}

// list position -> id: a loop a bound site let past 241 gets the ids of the indices from 242.
// Test aid SVR2011_TEST_SNAPSHOT_CHECK (big_roster on): right after the roster snapshot is taken (sub_8243B6F0:
// sub_8243A380 + A9E0), it's compared with the roster (sub_8243A590, AB60: 0 = the same), put back (A488, AAA0) and
// compared again - the compare and put-back walks past 241 run, and a wrong walk shows as a difference.
REX_EXTERN(__imp__sub_8243B6F0);
REX_HOOK_RAW(sub_8243B6F0) {
  const uint32_t snap = ctx.r3.u32;
  __imp__sub_8243B6F0(ctx, base);
  if (!g_on || !std::getenv("SVR2011_TEST_SNAPSHOT_CHECK")) return;
  const auto saved = ctx;
  auto call = [&](void (*fn)(PPCContext&, uint8_t*)) {
    ctx.r3.u64 = snap;
    fn(ctx, base);
    return ctx.r3.u32;
  };
  const uint32_t a = call(sub_8243A590), b = call(sub_8243AB60);
  call(sub_8243A488);
  call(sub_8243AAA0);
  const uint32_t c = call(sub_8243A590), d = call(sub_8243AB60);
  ctx = saved;
  REXLOG_INFO("[svr2011] big roster: test aid - snapshot {:08X} compared {} / {} (0 = the same), put back, compared {} / {}",
              snap, a, b, c, d);
}

REX_EXTERN(__imp__sub_828B7060);
REX_HOOK_RAW(sub_828B7060) {
  if (g_on && int32_t(ctx.r3.u32) == g_ext_pos.load() && ctx.r3.u32 >= svr2011::kStockIndices && ctx.r3.u32 < g_count) {
    ctx.r3.u64 = g_ext_id[ctx.r3.u32 - svr2011::kStockIndices];
    return;
  }
  __imp__sub_828B7060(ctx, base);
}
