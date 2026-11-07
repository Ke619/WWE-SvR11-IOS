// More High Resolution (256x256) Paint Tool logos on a Created Superstar -
// see caw_logos.h.
//
// The logo cache (sub_828DCDD8(caw) = caw + 3088; offsets below are from it):
//   +0      layers: 168 x 72 bytes, each {u16 logo index, u8 type, ...};
//           type 64 = 128x128 logo, 65 = 256x256 logo
//   +12168  u8 used[10] (128x128)      +12178  u8 used[2] (256x256)
//   +12180  palettes, 1024 bytes each (shared by both sizes)
//   +14228  256x256 pixels, 65536 bytes each (8-bit indices)
//   +22420  128x128 pixels, 16384 bytes each  (same memory as the above)
//   +145300 u32 id[2] + 28-byte info[2] of the 256x256 logos
//   +186260 u32 id[10] + 28-byte info[10] of the 128x128 logos (end 186580)
// With 256x256 logos, 145364..186260 is unused (it is the tail of the
// 128x128 pixels) and outside the CAW checksum (sub_828CB038): the Ext
// record below lives there. Every function that walks the 256x256 slots is
// hooked so slots 2..kMaxHigh-1 work like the game's own two.
#include "caw_logos.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(caw_logos_anywhere, true, "Gameplay",
                    "Created Superstars: logos show anywhere on clothing and skin (off: the game's own logo mask "
                    "keeps them off the crotch and chest areas)");

namespace {

constexpr uint32_t kLowUsed = 12168, kHighUsed = 12178, kPalettes = 12180, kHighPixels = 14228;
constexpr uint32_t kLayerCount = 168, kLayerSize = 72, kExtLayerCount = 8;
constexpr uint32_t kExtOffset = 160000;
constexpr uint8_t kHighType = 65;
constexpr uint32_t kGameHigh = 2, kMaxHigh = 10;  // the renderer has 10 texture slots
constexpr uint32_t kPaletteSize = 1024, kPixelSize = 65536, kInfoSize = 28;
constexpr uint32_t kRenderTextures = 2432, kRenderBuilt = 2472;
constexpr uint32_t kMagic = 0x584C4731;  // "XLG1"

// The record of slots kGameHigh..kMaxHigh-1, host byte order (the game only
// ever copies it).
struct Ext {
  uint32_t magic;
  uint32_t check;
  uint8_t used[kMaxHigh];
  uint8_t pad[6];
  uint64_t hash[kMaxHigh];
  uint32_t id[kMaxHigh];
  uint8_t info[kMaxHigh][kInfoSize];
};
static_assert(kExtOffset + sizeof(Ext) <= 186260, "Ext must stay in the unused tail");
static_assert(kExtOffset >= 145364, "Ext must stay in the unused tail");

rex::memory::Memory* g_memory = nullptr;
std::filesystem::path g_store;       // Saves\.logos
std::mutex g_mutex;
std::map<uint64_t, uint32_t> g_logos;  // hash -> guest palette + pixels
uint32_t g_blank = 0;                  // shown for a logo whose file is missing
svr2011::MissingLogoHandler g_missing = nullptr;  // (online.cpp: fetches it)
std::map<uint32_t, std::array<uint64_t, kMaxHigh>> g_built;  // render object -> hashes
thread_local int t_preview_index = -1;

uint8_t* Ptr(uint8_t* base, uint32_t addr) { return base + addr; }
uint8_t Rd8(uint8_t* base, uint32_t addr) { return base[addr]; }
using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr32;

uint32_t Fnv32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
uint64_t Fnv64(const uint8_t* p, size_t n, uint64_t h = 14695981039346656037ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

uint32_t ExtCheck(const Ext* e) {
  return Fnv32(reinterpret_cast<const uint8_t*>(e) + 8, sizeof(Ext) - 8);
}

// The record of a cache, or null if it has none (or the memory holds
// something else: 128x128 pixels).
Ext* GetExt(uint8_t* base, uint32_t cache, bool create = false) {
  if (cache < 0x10000) return nullptr;
  auto* e = reinterpret_cast<Ext*>(Ptr(base, cache + kExtOffset));
  if (e->magic == kMagic && e->check == ExtCheck(e)) return e;
  if (!create) return nullptr;
  std::memset(e, 0, sizeof(Ext));
  e->magic = kMagic;
  e->check = ExtCheck(e);
  return e;
}
void Commit(Ext* e) { e->check = ExtCheck(e); }

uint32_t ExtUsedCount(const Ext* e) {
  uint32_t n = 0;
  for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) n += e->used[k] ? 1 : 0;
  return n;
}

std::filesystem::path LogoFile(uint64_t hash) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(hash));
  return g_store / name;
}

// Guest palette + pixels of a stored logo (loaded on first use).
uint32_t LogoData(uint8_t* base, uint64_t hash) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_logos.find(hash); it != g_logos.end()) return it->second;
  const uint32_t data = g_memory->SystemHeapAlloc(kPaletteSize + kPixelSize);
  if (!data) return g_blank;
  std::ifstream in(LogoFile(hash), std::ios::binary);
  if (!in.read(reinterpret_cast<char*>(Ptr(base, data)), kPaletteSize + kPixelSize)) {
    REXLOG_WARN("caw logos: {:016X} is missing from {}", hash, g_store.string());
    g_memory->SystemHeapFree(data);
    static std::map<uint64_t, bool> asked;
    if (g_missing && !asked[hash]) {
      asked[hash] = true;
      g_missing(hash, LogoFile(hash));
    }
    return g_blank;
  }
  g_logos[hash] = data;
  return data;
}

// Stores a logo (palette + pixels copied from the game) and returns its hash.
uint64_t StoreLogo(uint8_t* base, uint32_t palette, uint32_t pixels) {
  uint64_t hash = Fnv64(Ptr(base, palette), kPaletteSize);
  hash = Fnv64(Ptr(base, pixels), kPixelSize, hash);
  std::lock_guard lock(g_mutex);
  if (g_logos.count(hash)) return hash;
  const uint32_t data = g_memory->SystemHeapAlloc(kPaletteSize + kPixelSize);
  if (data) {
    std::memcpy(Ptr(base, data), Ptr(base, palette), kPaletteSize);
    std::memcpy(Ptr(base, data + kPaletteSize), Ptr(base, pixels), kPixelSize);
    g_logos[hash] = data;
  }
  const auto file = LogoFile(hash);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    std::filesystem::create_directories(g_store, ec);
    const auto tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(Ptr(base, palette)), kPaletteSize);
      out.write(reinterpret_cast<const char*>(Ptr(base, pixels)), kPixelSize);
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) REXLOG_WARN("caw logos: could not write {}: {}", file.string(), ec.message());
  }
  return hash;
}

// Whether any layer of the cache (or of the 8 extra layers the game passes
// along, if any) shows 256x256 logo `index`.
bool Referenced(uint8_t* base, uint32_t cache, uint32_t ext_layers, uint32_t index) {
  auto shows = [&](uint32_t layer) {
    return Rd8(base, layer + 2) == kHighType && Rd16(base, layer) == index;
  };
  for (uint32_t i = 0; ext_layers && i < kExtLayerCount; ++i) {
    if (shows(ext_layers + i * kLayerSize)) return true;
  }
  for (uint32_t i = 0; i < kLayerCount; ++i) {
    if (shows(cache + i * kLayerSize)) return true;
  }
  return false;
}

// The editor's logo cache: sub_828DCDD8(sub_828D1AA0()), or 0 outside the
// editor.
uint32_t EditorCache(PPCContext& ctx, uint8_t* base) {
  const auto saved = ctx;
  sub_828D1AA0(ctx, base);
  uint32_t cache = 0;
  if (ctx.r3.u32) {
    sub_828DCDD8(ctx, base);
    cache = ctx.r3.u32;
  }
  ctx = saved;
  return cache;
}

}  // namespace

namespace svr2011 {

void SetMissingLogoHandler(MissingLogoHandler handler) { g_missing = handler; }

void InstallCawLogos(rex::memory::Memory* memory, const std::filesystem::path& saves) {
  g_memory = memory;
  g_store = saves / ".logos";
  g_blank = memory->SystemHeapAlloc(kPaletteSize + kPixelSize);
  if (g_blank) std::memset(memory->TranslateVirtual<uint8_t*>(g_blank), 0, kPaletteSize + kPixelSize);
  std::error_code ec;
  std::filesystem::create_directories(g_store, ec);
  // Not hidden: copy tools skip hidden folders, and logos 3-10 live here.
#if defined(_WIN32)
  const DWORD attr = GetFileAttributesW(g_store.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_HIDDEN))
    SetFileAttributesW(g_store.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_HIDDEN));
#endif
  REXLOG_INFO("caw logos: up to {} High Resolution logos per Created Superstar (store {})", kMaxHigh,
              g_store.string());
}

}  // namespace svr2011

// -- The cache's slot bookkeeping ---------------------------------------------

// Free slot: sub_828CB5D0(cache, is_low) -> index or -1.
REX_EXTERN(__imp__sub_828CB5D0);
REX_HOOK_RAW(sub_828CB5D0) {
  const uint32_t cache = ctx.r3.u32, is_low = ctx.r4.u32;
  __imp__sub_828CB5D0(ctx, base);
  if (is_low || ctx.r3.s32 != -1) return;
  const Ext* e = GetExt(base, cache);
  for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
    if (!e || !e->used[k]) {
      ctx.r3.s64 = k;
      return;
    }
  }
}

// Slot already holding a logo: sub_828CB630(cache, is_low, id) -> index or -1.
REX_EXTERN(__imp__sub_828CB630);
REX_HOOK_RAW(sub_828CB630) {
  const uint32_t cache = ctx.r3.u32, is_low = ctx.r4.u32, id = ctx.r5.u32;
  __imp__sub_828CB630(ctx, base);
  if (is_low || ctx.r3.s32 != -1) return;
  if (const Ext* e = GetExt(base, cache)) {
    for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
      if (e->used[k] && e->id[k] == id) {
        ctx.r3.s64 = k;
        return;
      }
    }
  }
}

// Slots in use: sub_828CB500(cache, is_low) -> count.
REX_EXTERN(__imp__sub_828CB500);
REX_HOOK_RAW(sub_828CB500) {
  const uint32_t cache = ctx.r3.u32, is_low = ctx.r4.u32;
  __imp__sub_828CB500(ctx, base);
  if (is_low) return;
  if (const Ext* e = GetExt(base, cache)) ctx.r3.u64 = ctx.r3.u32 + ExtUsedCount(e);
}

// Any logo at all: sub_828CB340(cache).
REX_EXTERN(__imp__sub_828CB340);
REX_HOOK_RAW(sub_828CB340) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_828CB340(ctx, base);
  if (ctx.r3.u32) return;
  if (const Ext* e = GetExt(base, cache); e && ExtUsedCount(e)) ctx.r3.u64 = 1;
}

// Any logo whose info byte 13 is set: sub_828CB390(cache).
REX_EXTERN(__imp__sub_828CB390);
REX_HOOK_RAW(sub_828CB390) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_828CB390(ctx, base);
  if (ctx.r3.u32) return;
  if (const Ext* e = GetExt(base, cache)) {
    for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
      if (e->used[k] && e->info[k][13]) ctx.r3.u64 = 1;
    }
  }
}

// Any logo whose info has byte 13 set and byte 12 clear: sub_828CB430(cache).
REX_EXTERN(__imp__sub_828CB430);
REX_HOOK_RAW(sub_828CB430) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_828CB430(ctx, base);
  if (ctx.r3.u32) return;
  if (const Ext* e = GetExt(base, cache)) {
    for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
      if (e->used[k] && e->info[k][13] && !e->info[k][12]) ctx.r3.u64 = 1;
    }
  }
}

// Id of a slot: sub_828CB4D0(cache, is_low, index).
REX_EXTERN(__imp__sub_828CB4D0);
REX_HOOK_RAW(sub_828CB4D0) {
  const uint32_t cache = ctx.r3.u32, is_low = ctx.r4.u32, index = ctx.r5.u32;
  if (is_low || index < kGameHigh) {
    __imp__sub_828CB4D0(ctx, base);
    return;
  }
  const Ext* e = GetExt(base, cache);
  ctx.r3.u64 = (e && index < kMaxHigh && e->used[index]) ? e->id[index] : 0;
}

// Put a logo in a slot: sub_828CB230(cache, is_low, index, palette, pixels,
// id, info[28]).
REX_EXTERN(__imp__sub_828CB230);
REX_HOOK_RAW(sub_828CB230) {
  const uint32_t cache = ctx.r3.u32, is_low = ctx.r4.u32, index = ctx.r5.u32;
  if (is_low || index < kGameHigh) {
    __imp__sub_828CB230(ctx, base);
    return;
  }
  if (index >= kMaxHigh) return;
  const uint64_t hash = StoreLogo(base, ctx.r6.u32, ctx.r7.u32);
  Ext* e = GetExt(base, cache, true);
  e->used[index] = 1;
  e->hash[index] = hash;
  e->id[index] = ctx.r8.u32;
  std::memcpy(e->info[index], Ptr(base, ctx.r9.u32), kInfoSize);
  Commit(e);
  REXLOG_INFO("caw logos: slot {} = {:016X}", index, hash);
}

// Reset the cache: sub_828CC728(cache).
REX_EXTERN(__imp__sub_828CC728);
REX_HOOK_RAW(sub_828CC728) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_828CC728(ctx, base);
  std::memset(Ptr(base, cache + kExtOffset), 0, sizeof(Ext));
}

// Drop logos no layer shows any more: sub_828CC840(cache, extra_layers[8]).
REX_EXTERN(__imp__sub_828CC840);
REX_HOOK_RAW(sub_828CC840) {
  const uint32_t cache = ctx.r3.u32, ext_layers = ctx.r4.u32;
  __imp__sub_828CC840(ctx, base);
  Ext* e = GetExt(base, cache);
  if (!e) return;
  bool changed = false;
  for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
    if (e->used[k] && !Referenced(base, cache, ext_layers, k)) {
      e->used[k] = 0;
      e->hash[k] = 0;
      e->id[k] = 0;
      std::memset(e->info[k], 0, kInfoSize);
      changed = true;
    }
  }
  if (changed) Commit(e);
}

// -- The editor's logo textures -------------------------------------------------

// Which sizes are in use (0 none, 1 256x256, 2 128x128, 3 both):
// sub_827A0578(render).
REX_EXTERN(__imp__sub_827A0578);
REX_HOOK_RAW(sub_827A0578) {
  const uint32_t cache = EditorCache(ctx, base);
  __imp__sub_827A0578(ctx, base);
  if (ctx.r3.u32 & 1) return;
  if (const Ext* e = GetExt(base, cache); e && ExtUsedCount(e)) ctx.r3.u64 = ctx.r3.u32 | 1;
}

// Id of a slot if used: sub_827A0630(render, index, is_high).
REX_EXTERN(__imp__sub_827A0630);
REX_HOOK_RAW(sub_827A0630) {
  const uint32_t index = ctx.r4.u32, is_high = ctx.r5.u32;
  if (!is_high || index < kGameHigh) {
    __imp__sub_827A0630(ctx, base);
    return;
  }
  const Ext* e = GetExt(base, EditorCache(ctx, base));
  ctx.r3.u64 = (e && index < kMaxHigh && e->used[index]) ? e->id[index] : 0;
}

std::array<uint64_t, kMaxHigh> ExtHashes(const Ext* e) {
  std::array<uint64_t, kMaxHigh> h{};
  for (uint32_t k = kGameHigh; e && k < kMaxHigh; ++k) h[k] = e->used[k] ? e->hash[k] : 0;
  return h;
}

// Textures out of date: sub_827A06B0(render) -> 1.
REX_EXTERN(__imp__sub_827A06B0);
REX_HOOK_RAW(sub_827A06B0) {
  const uint32_t render = ctx.r3.u32;
  const uint32_t cache = EditorCache(ctx, base);
  __imp__sub_827A06B0(ctx, base);
  if (ctx.r3.u32) return;
  const auto now = ExtHashes(GetExt(base, cache));
  std::lock_guard lock(g_mutex);
  auto it = g_built.find(render);
  const std::array<uint64_t, kMaxHigh> none{};
  if ((it == g_built.end() ? none : it->second) != now) ctx.r3.u64 = 1;
}

// Rebuild the textures: sub_827A0D20(render). The game frees all 10 texture
// slots and builds the ones it knows; this builds slots kGameHigh.. the same
// way (texture memory from the same pool, 256x256, filled from the store).
REX_EXTERN(__imp__sub_827A0D20);
REX_HOOK_RAW(sub_827A0D20) {
  const uint32_t render = ctx.r3.u32;
  const uint32_t cache = EditorCache(ctx, base);
  __imp__sub_827A0D20(ctx, base);
  const Ext* e = GetExt(base, cache);
  const auto hashes = ExtHashes(e);
  {
    std::lock_guard lock(g_mutex);
    g_built[render] = hashes;
  }
  if (!e) return;
  const auto saved = ctx;
  for (uint32_t k = kGameHigh; k < kMaxHigh; ++k) {
    if (!e->used[k]) continue;
    const uint32_t data = LogoData(base, e->hash[k]);
    sub_8269A178(ctx, base);
    const uint32_t pool = ctx.r3.u32;
    sub_8269A478(ctx, base);
    const uint32_t previous = ctx.r3.u32;
    ctx.r3.u64 = pool;
    sub_8269AC60(ctx, base);
    sub_8269ACE0(ctx, base);
    ctx.r4.u64 = 1;
    if (ctx.r3.u32) {
      ctx.r3.u64 = 64;
      sub_8269A8D8(ctx, base);
    } else {
      ctx.r3.u64 = 4;
      sub_8269BA30(ctx, base);
    }
    const uint32_t texture = ctx.r3.u32;
    if (texture && data) {
      ctx.r4.u64 = 256;
      ctx.r5.u64 = 256;
      sub_828D10E0(ctx, base);
      Wr32(base, render + kRenderTextures + k * 4, texture);
      ctx.r3.u64 = texture;
      ctx.r4.u64 = data;                 // palette
      ctx.r5.u64 = data + kPaletteSize;  // pixels
      sub_828D1080(ctx, base);
      Wr32(base, render + kRenderBuilt, 1);
    }
    ctx.r3.u64 = previous;
    sub_8269AC60(ctx, base);
  }
  ctx = saved;
}

// The texture of the logo being placed: sub_828DFE90(preview) reads the
// layer's slot straight from the cache; for slots kGameHigh.. its image
// (sub_828E3818(texture, pixels, palette, w, h)) comes from the store.
REX_EXTERN(__imp__sub_828DFE90);
REX_HOOK_RAW(sub_828DFE90) {
  const uint32_t layer = Rd32(base, ctx.r3.u32 + 40);
  const bool extra = layer && Rd8(base, layer + 2) == kHighType && Rd16(base, layer) >= kGameHigh &&
                     Rd16(base, layer) < kMaxHigh;
  t_preview_index = extra ? Rd16(base, layer) : -1;
  __imp__sub_828DFE90(ctx, base);
  t_preview_index = -1;
}

REX_EXTERN(__imp__sub_828E3818);
REX_HOOK_RAW(sub_828E3818) {
  if (t_preview_index >= 0) {
    const uint32_t index = uint32_t(t_preview_index);
    const uint32_t cache = ctx.r4.u32 - kHighPixels - index * kPixelSize;
    const Ext* e = GetExt(base, cache);
    const uint32_t data = (e && e->used[index]) ? LogoData(base, e->hash[index]) : g_blank;
    if (data) {
      ctx.r4.u64 = data + kPaletteSize;
      ctx.r5.u64 = data;
    }
  }
  __imp__sub_828E3818(ctx, base);
}

// The template a new layer copies (sub_828DDC00(catalog, index) -> entry of
// the catalog's current category, 60 bytes each): the 256x256 logo category
// has entries for the game's two slots, so slots kGameHigh.. use slot 0's
// (reading past them gave the layer junk: file ids to load that never
// finish loading, the editor waited on NOW LOADING forever).
REX_EXTERN(__imp__sub_828DDC00);
REX_HOOK_RAW(sub_828DDC00) {
  if (Rd32(base, ctx.r3.u32 + 1640) == kHighType && ctx.r4.u32 >= kGameHigh && ctx.r4.u32 < kMaxHigh) {
    ctx.r4.u64 = 0;
  }
  // (a saved CAW's record past the category's list, its CAW part mod gone - no overlay, another machine: the first record)
  const uint32_t at = ctx.r3.u32 + Rd32(base, ctx.r3.u32 + 1640) * 16 + 56;
  if (ctx.r4.u32 >= (uint32_t(base[at]) << 8 | base[at + 1])) ctx.r4.u64 = 0;
  __imp__sub_828DDC00(ctx, base);
}

// Logos anywhere (caw_logos_anywhere, default on): the game keeps logos off parts of clothing and skin -
// the crotch / chest areas of trunks, underwear and tops, the skin under them. The logo texture's builder
// (sub_828EB8E8) draws each layer pixel by pixel and first reads a mask image at that spot (its palette
// entry's alpha, byte 0): 255 skips the pixel, less is taken off the logo's alpha. The mask
// (sub_828E9A58(builder, n) builds mask n into the image at builder +156; +100 its pixels, +104 its
// 256-entry palette) is made see-through here: every logo pixel is drawn where the editor put it. Only
// how the logo texture is built changes - not the saved Created Superstar, nor what goes online (other
// players' games build their own textures, with or without the mask).
REX_EXTERN(__imp__sub_828E9A58);
REX_HOOK_RAW(sub_828E9A58) {
  const uint32_t builder = ctx.r3.u32, n = ctx.r4.u32;
  __imp__sub_828E9A58(ctx, base);
  if (!REXCVAR_GET(caw_logos_anywhere)) return;
  const uint32_t mask = Rd32(base, builder + 156);
  const uint32_t palette = mask ? Rd32(base, mask + 104) : 0;
  if (!palette) return;
  bool changed = false;
  for (uint32_t k = 0; k < 256; ++k)
    if (base[palette + k * 4]) base[palette + k * 4] = 0, changed = true;
  static int logged = 0;
  if (changed && ++logged <= 5) REXLOG_INFO("caw logos: logo mask {} cleared - logos show anywhere", n);
}
