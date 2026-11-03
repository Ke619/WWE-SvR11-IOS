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

namespace svr2011 {

bool BigRosterOn() { return g_on; }

uint32_t RosterEntry(uint32_t start, uint32_t stride, uint32_t index) {
  if (index < kStockIndices || !g_on || index >= kMaxIndices) return start + index * stride;
  return uint32_t(int64_t(start + index * stride) + Shift(start, stride));
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
    const uint32_t from = Rd16(base + kIdToIndex + id * 2), to = kStockIndices + uint32_t(k);
    base[kIdToIndex + id * 2] = uint8_t(to >> 8);
    base[kIdToIndex + id * 2 + 1] = uint8_t(to);
    g_count = std::max(g_count, to + 1);
    g_ext_id[k] = uint16_t(id);
    for (const auto& [start, size] : {std::pair{kRecords, kRecordSize}, {kProfiles, kProfileSize}, {kEntries, kEntrySize}})
      std::memcpy(base + RosterEntry(start, size, to), base + start + from * size, size);
    REXLOG_INFO("[svr2011] big roster: test aid - id {} index {} -> {}", id, from, to);
  }
  heap->Protect(kIdToIndex, 322 * 2, old);
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
REX_EXTERN(__imp__sub_828B7060);
REX_HOOK_RAW(sub_828B7060) {
  if (g_on && int32_t(ctx.r3.u32) == g_ext_pos.load() && ctx.r3.u32 >= svr2011::kStockIndices && ctx.r3.u32 < g_count) {
    ctx.r3.u64 = g_ext_id[ctx.r3.u32 - svr2011::kStockIndices];
    return;
  }
  __imp__sub_828B7060(ctx, base);
}
