// Paint Tool pages - see paint_pages.h.
//
// Game side (docs/PAINT_TOOL_PAGES_PLAN.md):
//   Paint Tool file (.pt): 0x785FFC bytes, u32 magic 0x02A78E9A, u32 version
//   3, 20 slots of 0x604CC bytes at 8 + k * 0x604CC (slot +28 used flag), a
//   checksum per slot (slot +0x604C8, sub_827B3118) and the sum of them all
//   at the end (sub_827B35C8).
//   Storage jobs on it: sub_824AE6E8(st, 1, ...) open, sub_824AE830(st, 1,
//   buf, size) read, sub_82517CF8(st, 1, buf, size) write, sub_824AE790(st,
//   ...) close; only Paint Tool code calls them (the Paint Tool itself, the
//   Created Superstar logo list, the logo popup, Community Creations). The
//   caller polls st+32 (done), then st+36 (succeeded) / st+40 / st+44 (error).
//   Paint Tool manager (PTM) = sub_8286E4B8(*0x82EDE67C): +28 state (0 idle;
//   34 rebuilds the 20 thumbnails, 35 waits for them), +104 the file buffer
//   (0x785FFC bytes of file, then a scratch slot and u32 dirty[20]).
//   Grid menu (sub_827B4600 updates it): +776 state (0 idle; 8 waits for the
//   thumbnails, then redraws the cells), +56 row, +72 column.
//   Created Superstar logo list (sub_828D1BF8()): loaded by sub_828DC1A8(list,
//   42) and the steps of sub_828DBD90(list) (+16 step, 7 = done; it reads the
//   whole file through the jobs above), +28 logo count; its logos are read
//   through accessors (sub_828DB918 ... sub_828DBA28). Its picker (HEAD /
//   BODY -> TATTOOS -> PAINT TOOL DATA) scrolls through them.
//   Community Creations Paint Tool slot list (data-select screen, vtable
//   0x82027C20): +260 mode (0 pick a logo to upload, 1 pick where a download
//   goes, 2 write the download), +272 the Paint Tool file it read; while the
//   list has the controller sub_8250FFC0(screen) runs every frame. Its fills:
//   sub_82517B58 used flags, sub_82517B90 downloaded flags, sub_82518668
//   labels; sub_82510518 draws the list. Mode 2 copies the download into slot
//   +0x8344 of +272 and saves the whole file (sub_82518268).
#include "paint_pages.h"
#include "online.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

constexpr uint32_t kSlots = 20, kSlot = 0x604CC, kFile = 0x785FFC;
constexpr uint32_t kMagic = 0x02A78E9A, kVersion = 3;
constexpr uint32_t kUsed = 28, kSum = 0x604C8;  // slot-relative
constexpr uint32_t kContainer = 0x82EDE67C;     // -> sub_8286E4B8 -> PTM
constexpr uint32_t kPtmState = 28, kPtmFile = 104;
constexpr uint32_t kMenuState = 776;
constexpr uint32_t kPtmRebuildThumbs = 34, kMenuRefresh = 8;
constexpr uint32_t kStDone = 32, kStOk = 36, kStFail = 40, kStError = 44, kStExtra = 52;

rex::memory::Memory* g_memory = nullptr;
rex::input::InputSystem* g_input = nullptr;
std::filesystem::path g_dir;  // Saves\.paint
std::filesystem::path g_pt;   // Saves\00PaintTool.pt

std::atomic<int> g_page{0};            // the Paint Tool grid's page
std::atomic<int> g_slot_page{0};       // the page in the Community Creations slot list
std::atomic<uint32_t> g_slot_screen{0};  // that list (its storage is +284)
std::mutex g_mutex;
std::set<uint32_t> g_faked;          // storage objects with a job done here
std::vector<uint8_t> g_page1;        // page 1 as it was when the grid left it
bool g_page1_valid = false;
std::atomic<int64_t> g_grid_seen{0};  // ms; the grid's last update (label)
constexpr uint32_t kSlotScreenVtable = 0x82027C20, kSlotScreenMode = 260, kSlotScreenFile = 272;
std::atomic<svr2011::PaintDownloadCheck> g_download_check{nullptr};

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's slot checksum (sub_827B3118; launcher pt_slot_sum): every field
// of the slot added as the type the game reads it as. `b` = the slot's start
// minus 8 (offsets as the launcher counts them).
uint32_t SlotSum(const uint8_t* b) {
  constexpr uint32_t kCanvas = 52, kTga = 0x40034, kDds = 0x50448, kStamp = 0x604C8;
  const uint8_t *t = b + kTga, *d = b + kDds, *e = b + kStamp;
  static const int words[] = {8, 12, 16, 28, 32, 36, 40, 44, 48};
  static const int tb[] = {0, 1, 2, 7, 16, 17}, th[] = {3, 5, 8, 10, 12, 14, 18};
  uint32_t s = 0;
  for (int i = 20; i < 28; i++) s += uint32_t(int32_t(int8_t(b[i])));
  for (int w : words) s += Rd32(b + w);
  for (int i = 0; i < 256 * 256; i++) s += Rd32(b + kCanvas + 4 * i);
  for (int i : tb) s += t[i];
  for (int i : th) s += Rd16(t + i);
  for (int i = 0; i < 256; i++) s += Rd32(t + 20 + 4 * i);
  for (int i = 0; i < 256 * 256; i++) s += t[0x414 + i];
  for (int i = 0; i < 32; i++) s += Rd32(d + 4 * i);
  for (int j = 0; j < 2048; j++) {
    const uint8_t* q = d + 128 + 32 * j;
    s += Rd16(q) + Rd16(q + 2) + Rd32(q + 4) + Rd16(q + 8) + Rd16(q + 10) + Rd32(q + 12) +
         Rd16(q + 16) + Rd16(q + 18) + Rd32(q + 20) + Rd16(q + 24) + Rd16(q + 26) + Rd32(q + 28);
  }
  s += Rd16(e);
  for (int i = 2; i < 8; i++) s += e[i];
  return s;
}

uint8_t* Slot(uint8_t* f, uint32_t k) { return f + 8 + k * kSlot; }

std::filesystem::path SlotFile(int page, uint32_t k) {
  char name[32];
  std::snprintf(name, sizeof(name), "p%02d_s%02u.bin", page + 1, k + 1);
  return g_dir / name;
}

// Builds page `page` (1..9) as a complete, valid Paint Tool file at `f`.
void BuildPage(int page, uint8_t* f, bool log = true) {
  std::memset(f, 0, kFile);
  Wr32(f, kMagic);
  Wr32(f + 4, kVersion);
  uint32_t total = kMagic + kVersion, used = 0;
  for (uint32_t k = 0; k < kSlots; ++k) {
    uint8_t* s = Slot(f, k);
    std::ifstream in(SlotFile(page, k), std::ios::binary);
    if (in && in.read(reinterpret_cast<char*>(s), kSlot) && Rd32(s + kUsed)) {
      ++used;
    } else {
      // An empty slot as the game makes one.
      std::memset(s, 0, kSlot);
      s[3] = 3;
      s[12] = 1;
    }
    const uint32_t sum = SlotSum(s - 8);
    Wr32(s + kSum, sum);
    total += sum;
  }
  Wr32(f + kFile - 4, total);
  if (log) REXLOG_INFO("paint pages: page {} loaded ({} logos)", page + 1, used);
}

// Stores page `page` (1..9) from a Paint Tool file image.
void SavePage(int page, const uint8_t* f) {
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  uint32_t used = 0;
  for (uint32_t k = 0; k < kSlots; ++k) {
    const uint8_t* s = f + 8 + k * kSlot;
    const auto file = SlotFile(page, k);
    if (!Rd32(s + kUsed)) {
      std::filesystem::remove(file, ec);
      continue;
    }
    ++used;
    const auto tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(s), kSlot);
      if (!out) {
        REXLOG_WARN("paint pages: could not write {}", tmp);
        continue;
      }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) REXLOG_WARN("paint pages: could not write {}: {}", file.string(), ec.message());
  }
  REXLOG_INFO("paint pages: page {} saved ({} logos)", page + 1, used);
}

// -- Changes made outside the game ------------------------------------------
//
// The launcher (Paint Tool tab, imports) may change the stores while the
// game runs. The Paint Tool reads its page when it opens (manager states
// 26-27), so a change made while it is closed shows anyway; while it is
// open, its copy of the page (PTM +104) is what the game writes back when it
// leaves or switches pages. So the page as read is remembered (g_loaded), and
// - the idle grid checks the page's files once a second and takes in the
//   slots changed outside (then rebuilds the thumbnails, as a page switch);
// - a write of the page first takes in the slots changed outside;
// in both, a slot the game changed since it read the page stays the game's
// (in-game edits win; the outside change of that slot is dropped, logged).

struct Loaded {
  int page = -1;
  std::vector<uint8_t> image;  // the page as read from its store
  uint64_t stamp = 0;          // its files' sizes and times then
};
Loaded g_loaded;
uint64_t g_page1_stamp = 0;  // page 1's files when g_page1 was taken

// The sizes and write times of page `page`'s files.
uint64_t Stamp(int page) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const std::filesystem::path& p) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(p, ec);
    h = (h ^ (ec ? ~0ull : size)) * 1099511628211ull;
    const auto t = std::filesystem::last_write_time(p, ec);
    h = (h ^ (ec ? 0 : uint64_t(t.time_since_epoch().count()))) * 1099511628211ull;
  };
  if (page == 0) {
    mix(g_pt);
  } else {
    for (uint32_t k = 0; k < kSlots; ++k) mix(SlotFile(page, k));
  }
  return h;
}

// Page `page` as its store holds it now.
bool DiskImage(int page, std::vector<uint8_t>& out) {
  out.assign(kFile, 0);
  if (page != 0) {
    BuildPage(page, out.data(), false);
    return true;
  }
  std::ifstream in(g_pt, std::ios::binary);
  return in.read(reinterpret_cast<char*>(out.data()), kFile) && Rd32(out.data()) == kMagic;
}

void Remember(int page, const uint8_t* image) {
  g_loaded.page = page;
  g_loaded.image.assign(image, image + kFile);
  g_loaded.stamp = Stamp(page);
}

// The slots' checksums (ids) and the file's total, as the game makes them.
void FixSums(uint8_t* f) {
  uint32_t total = kMagic + kVersion;
  for (uint32_t k = 0; k < kSlots; ++k) {
    uint8_t* s = Slot(f, k);
    const uint32_t sum = SlotSum(s - 8);
    Wr32(s + kSum, sum);
    total += sum;
  }
  Wr32(f + kFile - 4, total);
}

// Into the game's image `f` of page `page`: the slots that changed in the
// store since the page was read (`disk` = the store now), except those the
// game changed. Returns how many were taken.
int TakeOutsideChanges(int page, uint8_t* f, const std::vector<uint8_t>& disk) {
  if (g_loaded.page != page || g_loaded.image.size() != kFile || disk.size() != kFile) return 0;
  int taken = 0;
  for (uint32_t k = 0; k < kSlots; ++k) {
    const uint8_t* was = g_loaded.image.data() + 8 + k * kSlot;
    const uint8_t* now = disk.data() + 8 + k * kSlot;
    if (std::memcmp(now, was, kSum) == 0) continue;  // (not changed outside)
    if (std::memcmp(Slot(f, k), was, kSum) != 0) {
      REXLOG_WARN("paint pages: page {} slot {} changed in the game and outside it - the game's kept", page + 1,
                  k + 1);
      continue;
    }
    std::memcpy(Slot(f, k), now, kSlot);
    ++taken;
  }
  if (taken) FixSums(f);
  return taken;
}

// Before the game's image `f` of page `page` is written: the outside
// changes taken in.
void BeforeWrite(int page, uint8_t* f) {
  if (g_loaded.page != page || Stamp(page) == g_loaded.stamp) return;
  std::vector<uint8_t> disk;
  if (!DiskImage(page, disk)) return;
  if (const int n = TakeOutsideChanges(page, f, disk))
    REXLOG_INFO("paint pages: page {}: {} slots changed outside the game kept", page + 1, n);
}

// Page 1 into `f`: the copy taken when the grid left it (if the file has
// not changed since), else the file.
bool LoadPage1(uint8_t* f) {
  if (g_page1_valid && g_page1.size() == kFile && Stamp(0) == g_page1_stamp) {
    std::memcpy(f, g_page1.data(), kFile);
    return true;
  }
  std::ifstream in(g_pt, std::ios::binary);
  std::vector<uint8_t> data(kFile);
  if (!in.read(reinterpret_cast<char*>(data.data()), kFile) || Rd32(data.data()) != kMagic) {
    REXLOG_WARN("paint pages: could not read {}", g_pt.string());
    return false;
  }
  std::memcpy(f, data.data(), kFile);
  return true;
}

// A storage job finished here: done and succeeded, as the game's own jobs
// report it.
void Complete(uint8_t* base, uint32_t st) {
  Wr32(base + st + kStError, 0);
  Wr32(base + st + kStFail, 0);
  Wr32(base + st + kStExtra, 0);
  Wr32(base + st + kStOk, 1);
  Wr32(base + st + kStDone, 1);
}

uint32_t Ptm(PPCContext& ctx, uint8_t* base) {
  const uint32_t container = Rd32(base + kContainer);
  if (!container) return 0;
  const auto saved = ctx;
  ctx.r3.u64 = container;
  sub_8286E4B8(ctx, base);
  const uint32_t ptm = ctx.r3.u32;
  ctx = saved;
  return ptm;
}

bool AnyDirty(PPCContext& ctx, uint8_t* base, uint32_t file) {
  const auto saved = ctx;
  ctx.r3.u64 = file;
  sub_827B2F78(ctx, base);
  const bool dirty = ctx.r3.u32 != 0;
  ctx = saved;
  return dirty;
}

// Saves the page the grid shows now (copies and deletes in the grid are only
// saved when the player leaves the Paint Tool): the game's checksums
// (sub_827B35C8), then the page's store - page 1 is the game's own file,
// written as the launcher's Paint Tool tab writes it - and no slot left dirty
// (sub_827B3028).
bool SaveNow(PPCContext& ctx, uint8_t* base, uint32_t file) {
  const auto saved = ctx;
  ctx.r3.u64 = file;
  ctx.r4.u64 = 1;
  sub_827B35C8(ctx, base);
  ctx = saved;
  uint8_t* f = base + file;
  BeforeWrite(g_page.load(), f);
  if (g_page.load() != 0) {
    SavePage(g_page.load(), f);
  } else {
    const auto tmp = g_pt.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(f), kFile);
      if (!out) {
        REXLOG_WARN("paint pages: could not write {}", tmp);
        return false;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_pt, ec);
    if (ec) {
      REXLOG_WARN("paint pages: could not write {}: {}", g_pt.string(), ec.message());
      return false;
    }
    REXLOG_INFO("paint pages: page 1 saved");
  }
  Remember(g_page.load(), f);
  for (uint32_t k = 0; k < kSlots; ++k) {
    ctx.r3.u64 = file;
    ctx.r4.u64 = k;
    ctx.r5.u64 = 0;
    sub_827B3028(ctx, base);
    ctx = saved;
  }
  return true;
}

// Shows the previous / next page in the idle grid.
void SwitchPage(PPCContext& ctx, uint8_t* base, uint32_t menu, int delta) {
  const uint32_t ptm = Ptm(ctx, base);
  const uint32_t state = ptm ? Rd32(base + ptm + kPtmState) : 0xFFFFFFFF;
  const uint32_t file = ptm ? Rd32(base + ptm + kPtmFile) : 0;
  const uint32_t magic = file ? Rd32(base + file) : 0;
  const bool dirty = file && AnyDirty(ctx, base, file);
  if (!ptm || state != 0 || magic != kMagic) {
    REXLOG_INFO("paint pages: no page switch now (manager {:08X} state {} file {:08X} {:08X})", ptm, state,
                file, magic);
    return;
  }
  if (dirty) {
    std::lock_guard lock(g_mutex);
    if (!SaveNow(ctx, base, file)) return;
  }
  const int from = g_page.load(), to = (from + delta + svr2011::kPaintPages) % svr2011::kPaintPages;
  uint8_t* f = base + file;
  {
    std::lock_guard lock(g_mutex);
    if (from == 0) {
      g_page1.assign(f, f + kFile);
      g_page1_valid = true;
      g_page1_stamp = g_loaded.page == 0 ? g_loaded.stamp : 0;  // (stale if changed outside since read)
    }
    if (to == 0) {
      if (!LoadPage1(f)) return;
    } else {
      BuildPage(to, f);
    }
    Remember(to, f);
  }
  g_page = to;
  // The game's own refresh: rebuild the 20 thumbnails, then the cells.
  Wr32(base + ptm + kPtmState, kPtmRebuildThumbs);
  Wr32(base + menu + kMenuState, kMenuRefresh);
  REXLOG_INFO("paint pages: page {} of {}", to + 1, svr2011::kPaintPages);
}

// The idle grid: the slots of its page changed outside the game (checked
// once a second) are taken in and the thumbnails rebuilt.
void TakeOutsideChangesNow(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  const uint32_t ptm = Ptm(ctx, base);
  if (!ptm || Rd32(base + ptm + kPtmState) != 0) return;
  const uint32_t file = Rd32(base + ptm + kPtmFile);
  if (!file || Rd32(base + file) != kMagic) return;
  const int page = g_page.load();
  std::lock_guard lock(g_mutex);
  if (g_loaded.page != page) return;
  const uint64_t stamp = Stamp(page);
  if (stamp == g_loaded.stamp) return;
  std::vector<uint8_t> disk;
  if (!DiskImage(page, disk)) return;  // (tried again next time)
  const int n = TakeOutsideChanges(page, base + file, disk);
  g_loaded.image = std::move(disk);
  g_loaded.stamp = stamp;
  if (!n) return;
  Wr32(base + ptm + kPtmState, kPtmRebuildThumbs);
  Wr32(base + menu + kMenuState, kMenuRefresh);
  REXLOG_INFO("paint pages: page {}: {} slots changed outside the game - shown", page + 1, n);
}

// The page a Paint Tool storage job is for: the Paint Tool's own (manager
// +40) the grid's page, the Community Creations slot list's the page its file
// holds; everyone else (the Superstar logo list, Community Creations' own
// checks, ...) reads and writes page 1, the game's file.
int JobPage(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32 != 1) return 0;
  const uint32_t st = ctx.r3.u32;
  if (const uint32_t screen = g_slot_screen.load(); screen && st == screen + 284) return g_slot_page.load();
  if (const uint32_t ptm = Ptm(ctx, base); ptm && st == Rd32(base + ptm + 40)) return g_page.load();
  return 0;
}

}  // namespace

namespace svr2011 {

int PaintPage() { return g_page.load(); }

void SetPaintDownloadCheck(PaintDownloadCheck check) { g_download_check = check; }

void InstallPaintPages(rex::memory::Memory* memory, rex::input::InputSystem* input,
                       const std::filesystem::path& saves) {
  g_memory = memory;
  g_input = input;
  g_dir = saves / ".paint";
  g_pt = saves / "00PaintTool.pt";
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  REXLOG_INFO("paint pages: {} pages of {} Paint Tool logos (pages 2-{} in {})", kPaintPages, kSlots,
              kPaintPages, g_dir.string());
}

}  // namespace svr2011

// -- Storage jobs on pages 2-10 ----------------------------------------------

// Open: sub_824AE6E8(st, 1, ...).
REX_EXTERN(__imp__sub_824AE6E8);
REX_HOOK_RAW(sub_824AE6E8) {
  if (std::getenv("SVR2011_TEST_PAINT_TRACE") && ctx.r4.u32 == 1)
    REXLOG_INFO("paint trace: open storage {:08X} (page {}) from {:08X}", ctx.r3.u32, JobPage(ctx, base) + 1,
                uint32_t(ctx.lr));
  if (JobPage(ctx, base) == 0) {
    {
      std::lock_guard lock(g_mutex);
      if (ctx.r4.u32 == 1) g_page1_valid = false;  // page 1 may change
    }
    __imp__sub_824AE6E8(ctx, base);
    return;
  }
  const uint32_t st = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    g_faked.insert(st);
  }
  Complete(base, st);
}

// The Paint Tool's own storage (manager +40).
bool IsPaintToolStorage(PPCContext& ctx, uint8_t* base, uint32_t st) {
  if (ctx.r4.u32 != 1) return false;
  const uint32_t ptm = Ptm(ctx, base);
  return ptm && st == Rd32(base + ptm + 40);
}

// Read: sub_824AE830(st, 1, buf, size).
REX_EXTERN(__imp__sub_824AE830);
REX_HOOK_RAW(sub_824AE830) {
  const uint32_t st = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32;
  const int page = JobPage(ctx, base);
  const bool paint_tool = size == kFile && IsPaintToolStorage(ctx, base, st);
  if (page == 0 || size != kFile) {
    if (paint_tool) {  // (the read is the game's own: remember the file as it is now)
      std::lock_guard lock(g_mutex);
      std::vector<uint8_t> disk;
      if (DiskImage(0, disk)) Remember(0, disk.data());
    }
    __imp__sub_824AE830(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    BuildPage(page, base + buf);
    if (paint_tool) Remember(page, base + buf);
  }
  Complete(base, st);
}

// Write: sub_82517CF8(st, 1, buf, size).
REX_EXTERN(__imp__sub_82517CF8);
REX_HOOK_RAW(sub_82517CF8) {
  const uint32_t st = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32;
  const int page = JobPage(ctx, base);
  const bool paint_tool = size == kFile && IsPaintToolStorage(ctx, base, st);
  if (paint_tool) {
    std::lock_guard lock(g_mutex);
    BeforeWrite(page, base + buf);
    Remember(page, base + buf);
  }
  if (page == 0 || size != kFile) {
    __imp__sub_82517CF8(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    SavePage(page, base + buf);
    if (paint_tool) g_loaded.stamp = Stamp(page);
  }
  Complete(base, st);
}

// Close: sub_824AE790(st, ...) - ours if the open was.
REX_EXTERN(__imp__sub_824AE790);
REX_HOOK_RAW(sub_824AE790) {
  const uint32_t st = ctx.r3.u32;
  bool ours;
  {
    std::lock_guard lock(g_mutex);
    ours = g_faked.erase(st) != 0;
  }
  if (!ours) {
    __imp__sub_824AE790(ctx, base);
    return;
  }
  Complete(base, st);
}

// -- The grid ---------------------------------------------------------------

namespace {

constexpr uint32_t kMenuRow = 56, kMenuColumn = 72, kColumns = 5;
int g_last_row = -1, g_last_column = -1;  // the idle grid's cursor last frame

}  // namespace

// Grid menu update: sub_827B4600(menu). Pages work like the game's own list
// pages: moving off the right edge of the grid (which wraps to the left
// column) shows the next page, off the left edge the previous one.
REX_EXTERN(__imp__sub_827B4600);
REX_HOOK_RAW(sub_827B4600) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_827B4600(ctx, base);
  if (Rd32(base + menu + kMenuState) != 0) {
    g_last_row = g_last_column = -1;
    return;
  }
  g_grid_seen = NowMs();
  static int64_t last_check = 0;
  if (g_grid_seen.load() - last_check >= 1000) {
    last_check = g_grid_seen.load();
    TakeOutsideChangesNow(ctx, base, menu);
    if (Rd32(base + menu + kMenuState) != 0) return;
  }
  const int row = int(Rd32(base + menu + kMenuRow)), column = int(Rd32(base + menu + kMenuColumn));
  if (row == g_last_row && g_last_column == int(kColumns - 1) && column == 0)
    SwitchPage(ctx, base, menu, +1);
  else if (row == g_last_row && g_last_column == 0 && column == int(kColumns - 1))
    SwitchPage(ctx, base, menu, -1);
  g_last_row = row;
  g_last_column = column;
}

// -- The Created Superstar logo picker --------------------------------------
//
// The picker (HEAD / BODY -> TATTOOS -> PAINT TOOL DATA) scrolls through the
// logo list, which the game sizes for 20 logos. Here the list holds the used
// logos of all 10 pages: its count is their number, what the game asks of a
// logo by number (type, id, slot, header) comes from an index of all pages,
// and the 20 places for pictures (texture + 8-bit copy) are a window: logo n
// is loaded into place n % 20 when the game asks for its picture.

namespace {

constexpr uint32_t kListState = 16, kListCount = 28, kListTypes = 32;
constexpr uint32_t kListTextures256 = 112, kListTextures128 = 192, kListPictures = 272;
constexpr uint32_t kPictureSize = 0x10414, kTga = 0x4002C, kHeaderSize = 28;

struct Logo {
  int page;
  uint32_t slot;
  uint32_t type;  // 1 = 256x256, 2 = 128x128 (the list's own values)
  uint32_t id;
  uint8_t header[kHeaderSize];
};
std::vector<Logo> g_logos;        // the used logos of all pages, in page order
uint32_t g_list = 0;              // the list g_logos was made for
uint32_t g_headers = 0;           // guest: the headers of g_logos
int g_window[kSlots];             // the logo in each picture place (-1 none)
uint64_t g_index_stamp = 0;       // all pages' files when g_logos was made

uint64_t AllPagesStamp() {
  uint64_t h = 0;
  for (int page = 0; page < svr2011::kPaintPages; ++page) h = h * 31 + Stamp(page);
  return h;
}

// The first `n` bytes of page `page`'s slot `k`.
bool ReadSlot(int page, uint32_t k, uint8_t* out, size_t n, size_t offset = 0) {
  if (page == 0 && g_page1_valid && g_page1.size() == kFile && Stamp(0) == g_page1_stamp) {
    std::memcpy(out, g_page1.data() + 8 + k * kSlot + offset, n);
    return true;
  }
  std::ifstream in(page == 0 ? g_pt : SlotFile(page, k), std::ios::binary);
  in.seekg(std::streamoff((page == 0 ? 8 + k * kSlot : 0) + offset));
  return bool(in.read(reinterpret_cast<char*>(out), std::streamsize(n)));
}

// The list has finished loading: index every page's logos.
void IndexLogos(uint8_t* base, uint32_t list) {
  std::lock_guard lock(g_mutex);
  g_logos.clear();
  g_index_stamp = AllPagesStamp();
  for (int page = 0; page < svr2011::kPaintPages; ++page) {
    for (uint32_t k = 0; k < kSlots; ++k) {
      uint8_t head[48] = {};
      if (!ReadSlot(page, k, head, sizeof(head)) || !Rd32(head + kUsed)) continue;
      Logo logo{page, k, Rd32(head + 36) == 128 ? 2u : 1u, 0, {}};
      std::memcpy(logo.header, head, kHeaderSize);
      uint8_t id[4] = {};
      ReadSlot(page, k, id, 4, kSum);
      logo.id = Rd32(id);
      g_logos.push_back(logo);
    }
  }
  if (!g_headers) g_headers = g_memory->SystemHeapAlloc(svr2011::kPaintPages * kSlots * kHeaderSize);
  for (size_t i = 0; g_headers && i < g_logos.size(); ++i)
    std::memcpy(base + g_headers + i * kHeaderSize, g_logos[i].header, kHeaderSize);
  for (int& w : g_window) w = -1;
  g_list = list;
  Wr32(base + list + kListCount, uint32_t(g_logos.size()));
  REXLOG_INFO("paint pages: Superstar logo list holds {} logos of all pages", g_logos.size());
}

bool Indexed(uint8_t* base, uint32_t list, uint32_t n) {
  return list && list == g_list && Rd32(base + list + kListState) == 7 && n < g_logos.size();
}

// Logo n's picture into its place (n % 20): the 8-bit copy, then the texture
// made from it (as the list's own loader does).
void ShowLogo(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t n) {
  const uint32_t place = n % kSlots;
  if (g_window[place] == int(n)) return;
  const Logo& logo = g_logos[n];
  const uint32_t picture = list + kListPictures + place * kPictureSize;
  {
    std::lock_guard lock(g_mutex);
    if (!ReadSlot(logo.page, logo.slot, base + picture, kPictureSize, kTga)) return;
  }
  Wr32(base + list + kListTypes + place * 4, logo.type);
  const uint32_t texture =
      Rd32(base + list + (logo.type == 2 ? kListTextures128 : kListTextures256) + place * 4);
  if (texture) {
    const auto saved = ctx;
    ctx.r3.u64 = texture;
    ctx.r4.u64 = picture + 20;          // palette (after the 20-byte TGA header)
    ctx.r5.u64 = picture + 20 + 1024;   // indices
    sub_828D1080(ctx, base);
    ctx = saved;
  }
  g_window[place] = int(n);
}

// The list holds all pages; the pictures the picker shows first loaded.
void Reindex(PPCContext& ctx, uint8_t* base, uint32_t list) {
  IndexLogos(base, list);
  // The picker shows the first logos and (wrapping) the last ones when it
  // opens: their pictures now, not when first drawn.
  const uint32_t n = uint32_t(g_logos.size());
  for (uint32_t i : {n - 1, n - 2, 0u, 1u, 2u})
    if (i < n && g_window[i % kSlots] < 0) ShowLogo(ctx, base, list, i);
}

}  // namespace

// A step of the list's loader: sub_828DBD90(list). When it is done, the list
// is made to hold all pages.
REX_EXTERN(__imp__sub_828DBD90);
REX_HOOK_RAW(sub_828DBD90) {
  const uint32_t list = ctx.r3.u32;
  const uint32_t before = Rd32(base + list + kListState);
  __imp__sub_828DBD90(ctx, base);
  if (before != 7 && Rd32(base + list + kListState) == 7) Reindex(ctx, base, list);
}

// The picker opens: sub_827E7100(picker) (it reads the list's count). The
// list was loaded when the editor started; logos added or changed outside
// the game since (the launcher) are indexed now.
REX_EXTERN(__imp__sub_827E7100);
REX_HOOK_RAW(sub_827E7100) {
  const uint32_t list = g_list;
  if (list && Rd32(base + list + kListState) == 7 && AllPagesStamp() != g_index_stamp) {
    REXLOG_INFO("paint pages: Paint Tool logos changed outside the game - the Superstar logo list is made again");
    Reindex(ctx, base, list);
  }
  __imp__sub_827E7100(ctx, base);
}

// The accessors of logo n: type sub_828DB918(list, n), id sub_828DB9F8,
// slot sub_828DBA10, header sub_828DBA28; texture sub_828DB928(list, n) and
// sub_828DB970(out, list, n), 8-bit copy sub_828DB9E0(list, n).
REX_EXTERN(__imp__sub_828DB918);
REX_HOOK_RAW(sub_828DB918) {
  if (!Indexed(base, ctx.r3.u32, ctx.r4.u32)) return __imp__sub_828DB918(ctx, base);
  ctx.r3.u64 = g_logos[ctx.r4.u32].type;
}

REX_EXTERN(__imp__sub_828DB9F8);
REX_HOOK_RAW(sub_828DB9F8) {
  if (!Indexed(base, ctx.r3.u32, ctx.r4.u32)) return __imp__sub_828DB9F8(ctx, base);
  ctx.r3.u64 = g_logos[ctx.r4.u32].id;
}

REX_EXTERN(__imp__sub_828DBA10);
REX_HOOK_RAW(sub_828DBA10) {
  if (!Indexed(base, ctx.r3.u32, ctx.r4.u32)) return __imp__sub_828DBA10(ctx, base);
  ctx.r3.u64 = g_logos[ctx.r4.u32].slot;
}

REX_EXTERN(__imp__sub_828DBA28);
REX_HOOK_RAW(sub_828DBA28) {
  if (!g_headers || !Indexed(base, ctx.r3.u32, ctx.r4.u32)) return __imp__sub_828DBA28(ctx, base);
  ctx.r3.u64 = g_headers + ctx.r4.u32 * kHeaderSize;
}

REX_EXTERN(__imp__sub_828DB928);
REX_HOOK_RAW(sub_828DB928) {
  const uint32_t list = ctx.r3.u32, n = ctx.r4.u32;
  if (!Indexed(base, list, n)) return __imp__sub_828DB928(ctx, base);
  ShowLogo(ctx, base, list, n);
  ctx.r4.u64 = n % kSlots;
  __imp__sub_828DB928(ctx, base);
}

REX_EXTERN(__imp__sub_828DB970);
REX_HOOK_RAW(sub_828DB970) {
  const uint32_t list = ctx.r4.u32, n = ctx.r5.u32;
  if (!Indexed(base, list, n)) return __imp__sub_828DB970(ctx, base);
  ShowLogo(ctx, base, list, n);
  ctx.r5.u64 = n % kSlots;
  __imp__sub_828DB970(ctx, base);
}

REX_EXTERN(__imp__sub_828DB9E0);
REX_HOOK_RAW(sub_828DB9E0) {
  const uint32_t list = ctx.r3.u32, n = ctx.r4.u32;
  if (!Indexed(base, list, n)) return __imp__sub_828DB9E0(ctx, base);
  ShowLogo(ctx, base, list, n);
  ctx.r4.u64 = n % kSlots;
  __imp__sub_828DB9E0(ctx, base);
}

// -- Community Creations: the Paint Tool slot list -------------------------
//
// The list shows all 200 slots: its own pages ("1/20", D-pad LEFT / RIGHT)
// with 10 rows each, so slot n is on Paint Tool page n / 20. The list keeps
// the slot number (0-199) in screen+0x8344; the Paint Tool file it reads
// (+272) holds the page of the slot under the cursor (loaded here as the
// cursor moves), and every function that turns the number into a place in
// that file sees n % 20 while it runs.

namespace {

constexpr uint32_t kListSlots = svr2011::kPaintPages * kSlots;  // 200
constexpr uint32_t kSlotIndex = 0x8344, kSlotCount = 0x8340;
constexpr uint32_t kUsedFlags = 0x873C, kDownloadedFlags = 0x8AD4, kLabels = 0x9010, kLabelSize = 70;
constexpr uint32_t kListWidget = 28, kWidgetRow = 56, kWidgetPage = 72;
constexpr uint32_t kCcManager = 0x82E3DD9C;  // -> +12 content type (4 Paint Tool), +16 slot
uint32_t g_labels = 0;                       // guest: 200 labels, kLabelSize each

// Page `page` into the file image at `f` (page 1 from the file).
bool LoadPage(int page, uint8_t* f) {
  if (page == 0) return LoadPage1(f);
  BuildPage(page, f);
  return true;
}

bool IsSlotScreen(uint8_t* base, uint32_t screen) {
  return screen > 0x10000 && Rd32(base + screen) == kSlotScreenVtable;
}

// Used / downloaded flags of the 20 slots of `page`; the page the screen
// holds comes from its file, the others from their stores.
void PageFlags(uint8_t* base, uint32_t file, int page, uint32_t used[kSlots], uint32_t downloaded[kSlots]) {
  for (uint32_t k = 0; k < kSlots; ++k) used[k] = downloaded[k] = 0;
  auto header = [&](uint32_t k, const uint8_t* h) {
    used[k] = Rd32(h + kUsed);
    downloaded[k] = uint32_t(int32_t(int8_t(h[13])));
  };
  if (page == g_slot_page.load() && file && Rd32(base + file) == kMagic) {
    for (uint32_t k = 0; k < kSlots; ++k) header(k, base + file + 8 + k * kSlot);
    return;
  }
  if (page == 0 && g_page1_valid && g_page1.size() == kFile) {
    for (uint32_t k = 0; k < kSlots; ++k) header(k, g_page1.data() + 8 + k * kSlot);
    return;
  }
  for (uint32_t k = 0; k < kSlots; ++k) {
    uint8_t h[32] = {};
    std::ifstream in(page == 0 ? g_pt : SlotFile(page, k), std::ios::binary);
    if (page == 0) in.seekg(8 + k * kSlot);
    if (in.read(reinterpret_cast<char*>(h), sizeof(h))) header(k, h);
  }
}

// Shows the page of slot `n` in the screen's file (if it does not already).
void ShowPageOf(uint8_t* base, uint32_t screen, uint32_t n) {
  const uint32_t file = Rd32(base + screen + kSlotScreenFile);
  const int page = int(n / kSlots);
  if (!file || Rd32(base + file) != kMagic) return;
  if (g_slot_screen.load() == screen && page == g_slot_page.load()) return;
  std::lock_guard lock(g_mutex);
  if (LoadPage(page, base + file)) {
    g_slot_screen = screen;
    g_slot_page = page;
  }
}

// Runs fn(screen) with the slot number as the game's 0-19 on the page the
// file holds.
void WithPageSlot(PPCContext& ctx, uint8_t* base, uint32_t screen, void (*fn)(PPCContext&, uint8_t*)) {
  const uint32_t n = Rd32(base + screen + kSlotIndex);
  if (n < kSlots || n >= kListSlots) {
    fn(ctx, base);
    return;
  }
  ShowPageOf(base, screen, n);
  Wr32(base + screen + kSlotIndex, n % kSlots);
  fn(ctx, base);
  Wr32(base + screen + kSlotIndex, n);
}

}  // namespace

// The number of slots: 200 (the game: 20).
REX_EXTERN(__imp__sub_82517B40);
REX_HOOK_RAW(sub_82517B40) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_82517B40(ctx, base);
  if (IsSlotScreen(base, screen)) Wr32(base + screen + kSlotCount, kListSlots);
}

// The used flags of all 200 slots (the game: of the 20 in its file).
REX_EXTERN(__imp__sub_82517B58);
REX_HOOK_RAW(sub_82517B58) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_82517B58(ctx, base);
    return;
  }
  const uint32_t file = Rd32(base + screen + kSlotScreenFile);
  for (int page = 0; page < svr2011::kPaintPages; ++page) {
    uint32_t used[kSlots], downloaded[kSlots];
    PageFlags(base, file, page, used, downloaded);
    for (uint32_t k = 0; k < kSlots; ++k) Wr32(base + screen + kUsedFlags + (page * kSlots + k) * 4, used[k]);
  }
}

// The downloaded flags of all 200 slots.
REX_EXTERN(__imp__sub_82517B90);
REX_HOOK_RAW(sub_82517B90) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_82517B90(ctx, base);
    return;
  }
  const uint32_t file = Rd32(base + screen + kSlotScreenFile);
  for (int page = 0; page < svr2011::kPaintPages; ++page) {
    uint32_t used[kSlots], downloaded[kSlots];
    PageFlags(base, file, page, used, downloaded);
    for (uint32_t k = 0; k < kSlots; ++k)
      Wr32(base + screen + kDownloadedFlags + (page * kSlots + k) * 4, downloaded[k]);
  }
}

// The labels ("PAINT TOOL LOGO SLOT 01"): the game makes the first 20; the
// port's table numbers all 200 the same way.
REX_EXTERN(__imp__sub_82518668);
REX_HOOK_RAW(sub_82518668) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_82518668(ctx, base);
  if (!IsSlotScreen(base, screen)) return;
  if (!g_labels) g_labels = g_memory->SystemHeapAlloc(kListSlots * kLabelSize);
  if (!g_labels) return;
  // The game's text without its number (single-byte text, as it writes it).
  const char* first = reinterpret_cast<const char*>(base + screen + kLabels);
  std::string prefix(first, strnlen(first, kLabelSize));
  if (const auto space = prefix.find_last_of(' '); space != std::string::npos) prefix.resize(space);
  for (uint32_t n = 0; n < kListSlots; ++n) {
    char text[kLabelSize];
    std::snprintf(text, sizeof(text), "%s %02u", prefix.c_str(), n + 1);
    std::memcpy(base + g_labels + n * kLabelSize, text, kLabelSize);
  }
}

// A slot's label: sub_82517F40(screen, n) - "EMPTY" for an empty slot.
REX_EXTERN(__imp__sub_82517F40);
REX_HOOK_RAW(sub_82517F40) {
  const uint32_t screen = ctx.r3.u32, n = ctx.r4.u32;
  if (IsSlotScreen(base, screen) && g_labels && n < kListSlots && Rd32(base + screen + kUsedFlags + n * 4)) {
    ctx.r3.u64 = g_labels + n * kLabelSize;
    return;
  }
  __imp__sub_82517F40(ctx, base);
}

// The list's update while it has the controller (sets the slot number from
// the cursor): the file follows the page of the slot under the cursor.
REX_EXTERN(__imp__sub_8250FFC0);
REX_HOOK_RAW(sub_8250FFC0) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_8250FFC0(ctx, base);
  if (IsSlotScreen(base, screen) && Rd32(base + screen + kSlotScreenMode) <= 1) {
    const uint32_t n = Rd32(base + screen + kSlotIndex);
    if (n < kListSlots) ShowPageOf(base, screen, n);
  }
}

// The slot preview of an upload: sub_82517D78(screen) reads the cursor (list
// page * 10 + row) as the place in the file.
REX_EXTERN(__imp__sub_82517D78);
REX_HOOK_RAW(sub_82517D78) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_82517D78(ctx, base);
    return;
  }
  const uint32_t page = Rd32(base + screen + kWidgetPage), row = Rd32(base + screen + kWidgetRow);
  const uint32_t n = page * 10 + row;
  if (n < kSlots || n >= kListSlots) {
    __imp__sub_82517D78(ctx, base);
    return;
  }
  ShowPageOf(base, screen, n);
  Wr32(base + screen + kWidgetPage, (n % kSlots) / 10);
  Wr32(base + screen + kWidgetRow, n % 10);
  __imp__sub_82517D78(ctx, base);
  Wr32(base + screen + kWidgetPage, page);
  Wr32(base + screen + kWidgetRow, row);
}

// Uploads: the slot is copied out with its label (sub_825180F0), and
// stamped and saved (sub_825181C0); and the upload's checks (sub_82511C18).
REX_EXTERN(__imp__sub_825180F0);
REX_HOOK_RAW(sub_825180F0) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_825180F0(ctx, base);
    return;
  }
  const uint32_t n = Rd32(base + screen + kSlotIndex);
  WithPageSlot(ctx, base, screen, __imp__sub_825180F0);
  // The label that goes with it: the slot's own number.
  const uint32_t record = Rd32(base + screen + 268);
  if (record && g_labels && n < kListSlots)
    std::memcpy(base + record + kSlot, base + g_labels + n * kLabelSize, kLabelSize);
}

REX_EXTERN(__imp__sub_825181C0);
REX_HOOK_RAW(sub_825181C0) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_825181C0(ctx, base);
    return;
  }
  WithPageSlot(ctx, base, screen, __imp__sub_825181C0);
}

REX_EXTERN(__imp__sub_82511C18);
REX_HOOK_RAW(sub_82511C18) {
  const uint32_t screen = ctx.r3.u32;
  if (svr2011::CcUploadModsRefused(ctx, base, screen)) return;  // (online.h: uses mods - not uploaded)
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_82511C18(ctx, base);
    return;
  }
  WithPageSlot(ctx, base, screen, __imp__sub_82511C18);
}

// The upload's data and thumbnail: sub_824C5768(manager, which) and
// sub_824C5BA0(manager, ...) take the slot from manager+16.
template <void (*Fn)(PPCContext&, uint8_t*)>
void WithManagerSlot(PPCContext& ctx, uint8_t* base) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t slot = manager ? Rd32(base + manager + 16) : 0;
  if (!manager || Rd32(base + manager + 12) != 4 || slot < kSlots || slot >= kListSlots) {
    Fn(ctx, base);
    return;
  }
  Wr32(base + manager + 16, slot % kSlots);
  Fn(ctx, base);
  Wr32(base + manager + 16, slot);
}

REX_EXTERN(__imp__sub_824C5768);
REX_HOOK_RAW(sub_824C5768) { WithManagerSlot<__imp__sub_824C5768>(ctx, base); }

REX_EXTERN(__imp__sub_824C5BA0);
REX_HOOK_RAW(sub_824C5BA0) { WithManagerSlot<__imp__sub_824C5BA0>(ctx, base); }

// The list's storage jobs (screen+284) open: sub_825180B0(screen) before the
// list reads the file - page 1 first (the cursor starts on slot 1) - and
// sub_82518248(screen) before a download is written: the chosen slot's page.
REX_EXTERN(__imp__sub_825180B0);
REX_HOOK_RAW(sub_825180B0) {
  const uint32_t screen = ctx.r3.u32;
  if (IsSlotScreen(base, screen)) {
    g_slot_screen = screen;
    g_slot_page = 0;
  }
  __imp__sub_825180B0(ctx, base);
}

REX_EXTERN(__imp__sub_82518248);
REX_HOOK_RAW(sub_82518248) {
  const uint32_t screen = ctx.r3.u32;
  if (IsSlotScreen(base, screen)) {
    const uint32_t n = Rd32(base + screen + kSlotIndex);
    g_slot_screen = screen;
    g_slot_page = n < kListSlots ? int(n / kSlots) : 0;
  }
  __imp__sub_82518248(ctx, base);
}

// Whether there is a Paint Tool logo to upload (made here, not downloaded):
// sub_824E1FD0() looks at page 1 (the file Community Creations read); the
// other pages count too.
REX_EXTERN(__imp__sub_824E1FD0);
REX_HOOK_RAW(sub_824E1FD0) {
  __imp__sub_824E1FD0(ctx, base);
  if (ctx.r3.u32) return;
  for (int page = 1; page < svr2011::kPaintPages; ++page) {
    for (uint32_t k = 0; k < kSlots; ++k) {
      uint8_t h[32] = {};
      std::ifstream in(SlotFile(page, k), std::ios::binary);
      if (in.read(reinterpret_cast<char*>(h), sizeof(h)) && Rd32(h + kUsed) && h[13] == 0) {
        ctx.r3.u64 = 1;
        return;
      }
    }
  }
}

// A download is written into its slot: sub_82518268(screen). The page of
// the chosen slot is loaded into the file first (the game freed and
// allocated that buffer again since the slot was chosen), then the online
// code's check runs, then the game's write (stored by the page jobs above).
REX_EXTERN(__imp__sub_82518268);
REX_HOOK_RAW(sub_82518268) {
  const uint32_t screen = ctx.r3.u32;
  if (!IsSlotScreen(base, screen)) {
    __imp__sub_82518268(ctx, base);
    return;
  }
  const uint32_t n = Rd32(base + screen + kSlotIndex);
  const uint32_t file = Rd32(base + screen + kSlotScreenFile);
  if (n < kListSlots && file) {
    std::lock_guard lock(g_mutex);
    const int page = int(n / kSlots);
    g_slot_screen = screen;
    g_slot_page = page;
    if (!LoadPage(page, base + file)) REXLOG_WARN("paint pages: could not load page {} for the download", page + 1);
  }
  REXLOG_INFO("paint pages: download goes to page {} slot {}", g_slot_page.load() + 1, n % kSlots + 1);
  if (auto check = g_download_check.load(); check && !check(base, screen, g_slot_page.load())) {
    REXLOG_INFO("paint pages: the download was not written (Community Creations check)");
    return;
  }
  WithPageSlot(ctx, base, screen, __imp__sub_82518268);
}

// -- The page label ---------------------------------------------------------

namespace {

// "<  3 / 10  >" over the grid, like the game's own list pages.
class PageLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit PageLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (NowMs() - g_grid_seen.load() > 250) return;
    // The game's 16:9 picture, centred in the window; the label sits on the
    // top edge of the grid, centred.
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float x0 = (w - gw) * 0.5f, y0 = (h - gh) * 0.5f;
    const float scale = gh / 720.0f;
    char text[16];
    std::snprintf(text, sizeof(text), "%d / %d", g_page.load() + 1, svr2011::kPaintPages);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 28.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 c(x0 + gw * 0.5f, y0 + gh * 0.088f);
    const float arrow = 11.0f * scale, gap = 26.0f * scale;
    const float half = ts.x * 0.5f + gap + arrow;
    const ImVec2 pad(16.0f * scale, 6.0f * scale);
    const ImVec2 a(c.x - half - pad.x, c.y - ts.y * 0.5f - pad.y);
    const ImVec2 b(c.x + half + pad.x, c.y + ts.y * 0.5f + pad.y);
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 210), 6.0f * scale);
    dl->AddRect(a, b, IM_COL32(220, 220, 225, 255), 6.0f * scale, 0, 2.0f * scale);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), white, text);
    const float lx = c.x - half, rx = c.x + half;  // the arrows' outer points
    dl->AddTriangleFilled(ImVec2(lx, c.y), ImVec2(lx + arrow, c.y - arrow), ImVec2(lx + arrow, c.y + arrow), white);
    dl->AddTriangleFilled(ImVec2(rx, c.y), ImVec2(rx - arrow, c.y + arrow), ImVec2(rx - arrow, c.y - arrow), white);
  }
};

}  // namespace

namespace svr2011 {

void InstallPaintPagesOverlay(rex::ui::ImGuiDrawer* drawer) { new PageLabel(drawer); }

}  // namespace svr2011

// Test aid: SVR2011_TEST_PAINT_TRACE=1 - the Paint Tool manager's state
// changes (sub_82794D30(ptm) runs it) and the Paint Tool file opens.
REX_EXTERN(__imp__sub_82794D30);
REX_HOOK_RAW(sub_82794D30) {
  static const bool trace = [] {
    const char* v = std::getenv("SVR2011_TEST_PAINT_TRACE");
    return v && *v == '1';
  }();
  const uint32_t ptm = ctx.r3.u32;
  const uint32_t before = trace ? Rd32(base + ptm + kPtmState) : 0;
  __imp__sub_82794D30(ctx, base);
  if (trace && Rd32(base + ptm + kPtmState) != before)
    REXLOG_INFO("paint trace: manager {:08X} state {} -> {}", ptm, before, Rd32(base + ptm + kPtmState));
}

// (SVR2011_TEST_PAINT_TRACE: the Created Superstar logo list starts loading:
// sub_828DC1A8(list, ?).)
REX_EXTERN(__imp__sub_828DC1A8);
REX_HOOK_RAW(sub_828DC1A8) {
  if (std::getenv("SVR2011_TEST_PAINT_TRACE"))
    REXLOG_INFO("paint trace: logo list {:08X} loads ({}) from {:08X}", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_828DC1A8(ctx, base);
}
