// WWE SmackDown vs. Raw 2011 - a Created Superstar's eyes, eyebrows, eyelashes and lips per attire
// (caw_face_per_attire, on by default; suggestion 437782).
//
// The game keeps a CAW's 8 base parts once (.cas 0x148A6C, 72-byte records: body 2, face 4, eyes 5,
// eyebrows 6, eyelashes 7, lips 8, teeth 9, hair 10) and only the hair per attire (record 168 of the
// attire block). On ATTIRE 1-3 its editor greys out every base row but hair: sub_827C4D48(widget, row)
// asks the row's item for its category (vfunc +232 = sub_827D0FB0, item+1212) while the flag
// *(0x82EDE688)+32 is set, and 2 and 4-9 are disabled.
//
// Here eyes, eyebrows, eyelashes and lips (base records 2-5) get the hair's treatment:
// - Saved in the attire's own picture slot: the last 1024 bytes of its 17408-byte thumbnail field
//   (slot +0x24414; the thumbnail uses 16384). The game saves, sums, copies (CAW copy, attire copy)
//   and clears (attire delete) the whole slot, so the copy goes wherever the attire goes. "CFA1",
//   an FNV-1a check, then the 4 records; anything else there = the attire uses the shared parts.
// - The editor: after it loads an attire (sub_828DD170) its copy goes into the working base
//   (ctx+1716); when it commits one (sub_828DD058) the working base goes into the slot and the shared
//   values are put back for the game's copy of the record into the save, so the shared parts stay
//   ORIGINAL's. Rows of categories 5-8 aren't greyed; teeth and templates stay as the game has them.
// - Every model of the CAW: the description (sub_828CE008), the fighter's CAS buffer (sub_82687FA8)
//   and the online match block (sub_824C4F58) get the attire's records in base 2-5, so the other
//   side of an online match sees them too (no format change).
// An old save (no "CFA1") looks and behaves as before; a game without this shows the shared parts.
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(caw_face_per_attire, true, "Gameplay",
                    "Created Superstars: eyes, eyebrows, eyelashes and lips can differ on ATTIRE 1-3");

namespace {

using svrfmt::Rd32, svrfmt::Wr32;

// The .cas as the game holds it in memory (the "image": file offsets)
constexpr uint32_t kBase = 0x148A6C;    // the 8 shared base records
constexpr uint32_t kSlots = 0xB6364;    // the 4 attire picture slots
constexpr uint32_t kSlot = 0x24818;
constexpr uint32_t kTail = 0x24414;     // the free end of a slot's thumbnail field (1024 bytes)
constexpr uint32_t kRec = 72;
constexpr uint32_t kFirst = 2, kCount = 4;  // base records 2-5: eyes, eyebrows, eyelashes, lips
constexpr uint32_t kSize = kRec * kCount;
constexpr uint32_t kMagic = 0x43464131;    // "CFA1"
constexpr uint8_t kCats[kCount] = {5, 6, 7, 8};
// The editor's state (sub_828D1AA0)
constexpr uint32_t kCtxBase = 1716;       // working base records (the record copy at ctx+12, +1704)
constexpr uint32_t kCtxImage = 0x52CFC;   // its image of the .cas
constexpr uint32_t kCtxSlot = 0x2E4E4;    // working attire slot
// The editor's rows
constexpr uint32_t kAltFlag = 0x82EDE688;  // *(this)+32: an ATTIRE 1-3 is open
constexpr uint32_t kCategoryGetter = 0x827D0FB0;

bool On() { return REXCVAR_GET(caw_face_per_attire); }

uint32_t Fnv32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

// The records are the 4 categories in order (a stray block isn't taken for parts).
bool RecordsOk(const uint8_t* r) {
  for (uint32_t i = 0; i < kCount; ++i)
    if (r[i * kRec + 2] != kCats[i]) return false;
  return true;
}

// An attire slot's own records (guest address of the tail), or null.
const uint8_t* Own(uint8_t* base, uint32_t tail) {
  const uint8_t* t = base + tail;
  if (Rd32(base, tail) != kMagic || Rd32(base, tail + 4) != Fnv32(t + 8, kSize) || !RecordsOk(t + 8)) return nullptr;
  return t + 8;
}

void WriteOwn(uint8_t* base, uint32_t tail, const uint8_t* records) {
  std::memmove(base + tail + 8, records, kSize);
  Wr32(base, tail, kMagic);
  Wr32(base, tail + 4, Fnv32(records, kSize));
}

uint32_t ImageTail(uint32_t image, uint32_t attire) { return image + kSlots + attire * kSlot + kTail; }

// One log line per CAW image + attire + path
void Tell(const char* path, uint32_t image, uint32_t attire) {
  static std::mutex m;
  static std::set<std::pair<uint64_t, const char*>> told;
  std::lock_guard lock(m);
  if (told.size() < 256 && told.insert({uint64_t(image) << 8 | attire, path}).second)
    REXLOG_INFO("[svr2011] caw face: ATTIRE {} uses its own eyes, eyebrows, eyelashes and lips ({})", attire, path);
}

// The image's base records 2-5 swapped with an attire's own for the length of a game call.
struct ImageSwap {
  uint8_t* base;
  uint32_t at = 0;
  std::array<uint8_t, kSize> keep{};
  ImageSwap(uint8_t* b, uint32_t image, uint32_t attire, const char* path) : base(b) {
    if (!On() || !image || attire < 1 || attire > 3) return;
    const uint8_t* own = Own(base, ImageTail(image, attire));
    if (!own) return;
    at = image + kBase + kFirst * kRec;
    std::memcpy(keep.data(), base + at, kSize);
    std::memcpy(base + at, own, kSize);
    Tell(path, image, attire);
  }
  ~ImageSwap() {
    if (at) std::memcpy(base + at, keep.data(), kSize);
  }
};

}  // namespace

// The editor's row check: sub_827C4D48(widget, row) -> 0 = greyed. Rows of eyes, eyebrows, eyelashes
// and lips are open on ATTIRE 1-3 too (with the flag clear the game opens every row the same way).
REX_EXTERN(__imp__sub_827C4D48);
REX_HOOK_RAW(sub_827C4D48) {
  const uint32_t widget = ctx.r3.u32;
  const int32_t row = int32_t(ctx.r4.u32);
  __imp__sub_827C4D48(ctx, base);
  if (ctx.r3.u32 != 0 || !On() || row < 0) return;
  const uint32_t flag = Rd32(base, kAltFlag);
  if (!flag || !Rd32(base, flag + 32)) return;
  const uint32_t rows = Rd32(base, widget + 1148), items = Rd32(base, widget + 1160);
  if (!items || uint32_t(row) >= rows) return;
  const uint32_t item = Rd32(base, items + uint32_t(row) * 4);
  if (!item) return;
  const uint32_t vtable = Rd32(base, item);
  if (Rd32(base, vtable + 232) != kCategoryGetter) return;
  const uint32_t cat = Rd32(base, item + 1212);
  if (cat >= 5 && cat <= 8) ctx.r3.u64 = 1;
}

// The editor loads an attire: sub_828DD170(ctx, attire). ATTIRE 1-3 with its own records: those
// into the working base (else the shared ones the game just loaded stay).
REX_EXTERN(__imp__sub_828DD170);
REX_HOOK_RAW(sub_828DD170) {
  const uint32_t ed = ctx.r3.u32, attire = ctx.r4.u32;
  __imp__sub_828DD170(ctx, base);
  if (!On() || attire < 1 || attire > 3) return;
  if (const uint8_t* own = Own(base, ed + kCtxSlot + kTail)) {
    std::memmove(base + ed + kCtxBase + kFirst * kRec, own, kSize);
    REXLOG_INFO("[svr2011] caw face: editor loaded ATTIRE {} with its own eyes, eyebrows, eyelashes and lips",
                attire);
  }
}

// The editor commits an attire: sub_828DD058(ctx, attire, ...) copies the working slot into the
// attire's and the working record (the base among it) into the save's. ATTIRE 1-3: the working base
// records 2-5 go into the slot when they differ from the shared ones (else the slot's copy is
// cleared: the attire follows ORIGINAL again), and the save's record keeps the shared ones.
REX_EXTERN(__imp__sub_828DD058);
REX_HOOK_RAW(sub_828DD058) {
  const uint32_t ed = ctx.r3.u32, attire = ctx.r4.u32;
  if (!On() || attire < 1 || attire > 3) {
    __imp__sub_828DD058(ctx, base);
    return;
  }
  const uint32_t work = ed + kCtxBase + kFirst * kRec, shared = ed + kCtxImage + kBase + kFirst * kRec;
  const uint32_t tail = ed + kCtxSlot + kTail;
  std::array<uint8_t, kSize> mine;
  std::memcpy(mine.data(), base + work, kSize);
  const bool own = std::memcmp(mine.data(), base + shared, kSize) != 0 && RecordsOk(mine.data());
  const bool had = Own(base, tail) != nullptr;
  if (own) {
    WriteOwn(base, tail, mine.data());
  } else if (had) {
    std::memset(base + tail, 0, 8 + kSize);
  }
  if (own || had)
    REXLOG_INFO("[svr2011] caw face: ATTIRE {} saved with {} eyes, eyebrows, eyelashes and lips", attire,
                own ? "its own" : "the shared");
  std::memcpy(base + work, base + shared, kSize);
  __imp__sub_828DD058(ctx, base);
  std::memcpy(base + work, mine.data(), kSize);
}

// A CAW's model description: sub_828CE008(desc, image, attire) - the 8 base records at desc+0.
REX_EXTERN(__imp__sub_828CE008);
REX_HOOK_RAW(sub_828CE008) {
  const uint32_t desc = ctx.r3.u32, image = ctx.r4.u32, attire = ctx.r5.u32;
  __imp__sub_828CE008(ctx, base);
  if (!On() || attire < 1 || attire > 3) return;
  if (const uint8_t* own = Own(base, ImageTail(image, attire))) {
    std::memmove(base + desc + kFirst * kRec, own, kSize);
    Tell("model", image, attire);
  }
}

// A fighter's CAS buffer from a CAW: sub_82687FA8(obj) - image *(obj+44), attire *(obj+32).
REX_EXTERN(__imp__sub_82687FA8);
REX_HOOK_RAW(sub_82687FA8) {
  const ImageSwap swap(base, Rd32(base, ctx.r3.u32 + 44), Rd32(base, ctx.r3.u32 + 32), "fighter");
  __imp__sub_82687FA8(ctx, base);
}

// The online match block of this game's CAW: sub_824C4F58(state) - image *(state+0x10D38), the attire
// from the selection *(0x82EDE630) as the function itself asks (sub_82733828: the entry's +140, or
// sub_82733810's +136 when that is -1; entry = selection + *(selection+13520) * 1352).
REX_EXTERN(__imp__sub_824C4F58);
REX_HOOK_RAW(sub_824C4F58) {
  uint32_t image = 0, attire = 0;
  if (On()) {
    const uint32_t selection = Rd32(base, 0x82EDE630);
    image = Rd32(base, ctx.r3.u32 + 0x10D38);
    if (selection && image) {
      const uint32_t entry = selection + Rd32(base, selection + 13520) * 1352;
      attire = Rd32(base, entry + 140);
      if (int32_t(attire) == -1) attire = Rd32(base, entry + 136);
    }
  }
  const ImageSwap swap(base, image, attire, "online");
  __imp__sub_824C4F58(ctx, base);
}
