// WWE SmackDown vs. Raw 2011 - native renderer textures (see textures.h).

#include "native/textures.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>  // xxHash (inline)
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/system/xmemory.h>

#include "native/texture_packs.h"
#include "pad_icons.h"

REXCVAR_DEFINE_BOOL(native_mat_fix, true, "GPU",
                    "Native renderer: the stock ring mats without their light bands (rows of light patches in "
                    "the game's mat art; false: the original). Applies from the next arena.");

namespace svr2011::native::textures {

namespace {

namespace xenos = rex::graphics::xenos;
using plume::RenderFormat;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;
using TF = xenos::TextureFormat;

constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint64_t kRecheckFrames = 120;  // re-hash guest data this often
// ... but more often while a texture is new: a video's texture is created
// with its first frame and must be seen changing right away (with 120 its
// start stayed frozen for up to 2 s).
constexpr uint64_t kNewFrames = 240, kNewRecheckFrames = 4;
// A texture drawn again after a few frames unused is checked at once: the
// game loads new content into the same memory while it isn't shown (on
// character select the next superstar appeared for a moment with the last
// one's textures - Jimmy Snuka in John Cena's jeans).
constexpr uint64_t kUnusedRecheckFrames = 2;
// Texture packs and dumps take a texture whose data changed up to this many
// times (new content in reused memory - CAWs' parts on character select and
// in matches never were in the dumps); more changes is a video or the like.
constexpr uint32_t kPackChanges = 8;
// Textures seen changing are "dynamic": re-hashed at every frame they're
// used, and each check that finds no change doubles the wait before the next
// (up to kRecheckFrames). Video frames keep being checked at every frame; a
// texture the game rewrote once (streamed into memory it had used for
// another) settles back - before, it stayed checked at every frame for good,
// and over a long session (Universe) that grew to 8-13 MB hashed per frame.

// ---------------------------------------------------------------------------
// formats

// How guest blocks become host data.
enum class Convert : uint8_t {
  kNone,   // unsupported
  kCopy,   // same layout (the guest block is the host block)
  k565,    // 16-bit packed -> RGBA8 (X in the low bits, like every Xenos format)
  k655,
  k1555,
  k4444,
  kDXT3A,  // 4x4 block of 4-bit alpha -> R8
  kCTX1,   // 4x4 block, two RG8 endpoints + 2-bit indices -> RG8
  // BC blocks decoded on the CPU (a GPU without BC sampling: Mali)
  kBC1,  // -> RGBA8
  kBC2,  // -> RGBA8
  kBC3,  // -> RGBA8
  kBC4,  // -> R8
  kBC5,  // -> RG8
  kUnorm16Half,  // 16-bit UNORM channels -> 16-bit float (a GPU that can't filter UNORM16: Mali)
};

struct HostFormat {
  RenderFormat format = RenderFormat::UNKNOWN;
  RenderFormat gamma = RenderFormat::UNKNOWN;  // format for gamma (sign 3) textures
  Convert convert = Convert::kNone;
  uint32_t components = 4;  // host channels: missing ones replicate the last
  bool block_compressed = false;
};

HostFormat GetHostFormat(TF f) {
  switch (f) {
    case TF::k_8:
    case TF::k_8_A:
    case TF::k_8_B:
      return {RenderFormat::R8_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_8_8:
      return {RenderFormat::R8G8_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
    case TF::k_8_8_8_8_AS_16_16_16_16:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::kCopy, 4};
    case TF::k_2_10_10_10:
    case TF::k_2_10_10_10_AS_16_16_16_16:
      return {RenderFormat::R10G10B10A2_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_5_6_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k565, 4};
    case TF::k_6_5_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k655, 4};
    case TF::k_1_5_5_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k1555, 4};
    case TF::k_4_4_4_4:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k4444, 4};
    case TF::k_DXT1:
    case TF::k_DXT1_AS_16_16_16_16:
      return {RenderFormat::BC1_UNORM, RenderFormat::BC1_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT2_3:
    case TF::k_DXT2_3_AS_16_16_16_16:
      return {RenderFormat::BC2_UNORM, RenderFormat::BC2_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT4_5:
    case TF::k_DXT4_5_AS_16_16_16_16:
      return {RenderFormat::BC3_UNORM, RenderFormat::BC3_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXN:
      return {RenderFormat::BC5_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2, true};
    case TF::k_DXT5A:
      return {RenderFormat::BC4_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1, true};
    case TF::k_DXT3A:
      return {RenderFormat::R8_UNORM, RenderFormat::UNKNOWN, Convert::kDXT3A, 1};
    case TF::k_CTX1:
      return {RenderFormat::R8G8_UNORM, RenderFormat::UNKNOWN, Convert::kCTX1, 2};
    case TF::k_16:
      return {RenderFormat::R16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16:
      return {RenderFormat::R16G16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16:
      return {RenderFormat::R16G16B16A16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_16_EXPAND:
    case TF::k_16_FLOAT:
      return {RenderFormat::R16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16_EXPAND:
    case TF::k_16_16_FLOAT:
      return {RenderFormat::R16G16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16_EXPAND:
    case TF::k_16_16_16_16_FLOAT:
      return {RenderFormat::R16G16B16A16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_32_FLOAT:
      return {RenderFormat::R32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_32_32_FLOAT:
      return {RenderFormat::R32G32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_32_32_32_32_FLOAT:
      return {RenderFormat::R32G32B32A32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    default:
      return {};
  }
}

uint32_t Log2(uint32_t v) {
  uint32_t n = 0;
  while ((1u << (n + 1)) <= v) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// state

struct Entry {
  std::shared_ptr<plume::RenderTexture> resource;
  std::shared_ptr<plume::RenderTextureView> view;
  uint32_t srv = UINT32_MAX;
  bool failed = false;
  bool dynamic = false;  // its guest data has changed after upload
  uint32_t changes = 0;  // how often (texture packs: a texture loaded into memory another had used - a
                         // CAW's parts, the next superstar - is still dumped and replaced; videos aren't)
  uint32_t interval = 1;  // (dynamic) frames until its next check
  uint64_t created_frame = 0;
  uint64_t hash = 0;
  uint64_t checked_frame = 0;
  uint64_t used_frame = 0;  // the last frame it was drawn with
  uint32_t base_address = 0, base_size = 0, mip_address = 0, mip_size = 0;
  uint64_t bytes = 0;  // the host resource's (texture quality: resident bytes)
  uint32_t skip = 0;   // guest mip levels left out (texture quality)
  // Texture packs (texture_packs.h): the content hash a pack has a file for,
  // until it's read (`ready`) and replaces the game's texture (`replaced`).
  uint64_t pending = 0;
  std::shared_ptr<const texture_packs::Replacement> ready;
  bool replaced = false;
  bool written = false;  // a resolve was written back over its data (GuestWritten): checked at its next use
};

rex::memory::Memory* g_memory = nullptr;
const uint8_t* g_physical = nullptr;
// (SetBlockCompressionSupported; SVR2011_NATIVE_NO_BC=1: as if not - tests the CPU decoding on a PC)
bool g_bc_supported = std::getenv("SVR2011_NATIVE_NO_BC") == nullptr;
bool g_unorm16_filterable = true;  // (SetUnorm16Filterable)
uint32_t g_quality_skip = 0;       // (SetQuality) 0 high, 1 medium, 2 low
uint64_t g_resident_bytes = 0;     // all the textures' host bytes
uint32_t g_reduced = 0;            // textures with levels left out
uint32_t g_replaced = 0;           // pack textures shown (texture_packs.h)

// Texture quality: the guest mip levels a texture leaves out - its host
// texture starts at guest level `skip` (the game's own smaller mipmap). Only
// big mipmapped 2D textures: UI pictures and fonts (no mips) stay sharp; cube
// maps, volumes and stacks stay whole. Never into the packed mip tail (levels
// of 16 texels or less): with a 256 px side, guest level 2 is 64 px or more.
// (Sampler shifts the fetch's LOD clamps by the same count.)
uint32_t SkipLevels(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (!g_quality_skip || fetch.dimension != xenos::DataDimension::k2DOrStacked) return 0;
  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (d1 != 0 || !mip_page || mip_max == 0 || std::min(w1, h1) + 1 < 256) return 0;
  return std::min(g_quality_skip, mip_max);  // (one level left at least)
}

// A 16-bit UNORM format's float stand-in (the GPU can't filter UNORM16).
HostFormat Unorm16AsHalf(const HostFormat& h) {
  using RF = RenderFormat;
  switch (h.format) {
    case RF::R16_UNORM:
      return {RF::R16_FLOAT, RF::UNKNOWN, Convert::kUnorm16Half, 1};
    case RF::R16G16_UNORM:
      return {RF::R16G16_FLOAT, RF::UNKNOWN, Convert::kUnorm16Half, 2};
    case RF::R16G16B16A16_UNORM:
      return {RF::R16G16B16A16_FLOAT, RF::UNKNOWN, Convert::kUnorm16Half, 4};
    default:
      return h;
  }
}

// [0, 65535] -> the half float nearest v / 65535 (11 significant bits).
uint16_t Unorm16ToHalf(uint32_t v) {
  if (v == 0) return 0;
  if (v == 65535) return 0x3C00;  // 1.0
  float f = float(v) / 65535.0f;
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {  // (subnormal half: below 2^-14)
    mantissa |= 0x800000;
    const uint32_t shift = uint32_t(14 - exponent);
    return uint16_t((mantissa + (1u << (shift - 1))) >> shift);
  }
  const uint32_t rounded = (uint32_t(exponent) << 10) + ((mantissa + 0x1000) >> 13);
  return uint16_t(rounded);  // (a carry into the exponent is still right)
}

// A BC format's CPU-decoded stand-in (the GPU can't sample BC).
HostFormat Decompressed(const HostFormat& h) {
  using RF = RenderFormat;
  switch (h.format) {
    case RF::BC1_UNORM:
      return {RF::R8G8B8A8_UNORM, RF::R8G8B8A8_UNORM_SRGB, Convert::kBC1, 4};
    case RF::BC2_UNORM:
      return {RF::R8G8B8A8_UNORM, RF::R8G8B8A8_UNORM_SRGB, Convert::kBC2, 4};
    case RF::BC3_UNORM:
      return {RF::R8G8B8A8_UNORM, RF::R8G8B8A8_UNORM_SRGB, Convert::kBC3, 4};
    case RF::BC4_UNORM:
      return {RF::R8_UNORM, RF::UNKNOWN, Convert::kBC4, 1};
    case RF::BC5_UNORM:
      return {RF::R8G8_UNORM, RF::UNKNOWN, Convert::kBC5, 2};
    default:
      return h;
  }
}

// BC1's color part (also BC2's and BC3's: four colors always there).
void DecodeBC1Colors(const uint8_t* b, bool four_colors, uint8_t out[16][4]) {
  const uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  uint8_t p[4][4];
  auto rgb565 = [](uint32_t v, uint8_t* d) {
    d[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
    d[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
    d[2] = uint8_t((v & 31) * 255 / 31);
    d[3] = 255;
  };
  rgb565(c0, p[0]);
  rgb565(c1, p[1]);
  if (four_colors || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      p[2][k] = uint8_t((2 * p[0][k] + p[1][k] + 1) / 3);
      p[3][k] = uint8_t((p[0][k] + 2 * p[1][k] + 1) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) p[2][k] = uint8_t((p[0][k] + p[1][k]) / 2);
    p[2][3] = 255;
    p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;  // (transparent black)
  }
  const uint32_t indices = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
  for (uint32_t i = 0; i < 16; ++i) std::memcpy(out[i], p[(indices >> (2 * i)) & 3], 4);
}

// A BC4 block (BC3's alpha, BC5's channels): 16 values.
void DecodeBC4(const uint8_t* b, uint8_t out[16]) {
  uint8_t v[8];
  v[0] = b[0];
  v[1] = b[1];
  if (v[0] > v[1]) {
    for (int k = 1; k < 7; ++k) v[k + 1] = uint8_t(((7 - k) * v[0] + k * v[1] + 3) / 7);
  } else {
    for (int k = 1; k < 5; ++k) v[k + 1] = uint8_t(((5 - k) * v[0] + k * v[1] + 2) / 5);
    v[6] = 0;
    v[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
  for (uint32_t i = 0; i < 16; ++i) out[i] = v[(bits >> (3 * i)) & 7];
}
uint32_t g_srv_next = 0, g_srv_end = 0;
uint32_t g_sampler_next = 0, g_sampler_end = 0;
std::unordered_map<uint64_t, Entry> g_textures;   // key hash -> texture
std::atomic<uint64_t> g_generation{1};             // (Generation)
struct SamplerEntry {
  uint32_t index;
  std::unique_ptr<plume::RenderSampler> sampler;
};
std::unordered_map<uint64_t, SamplerEntry> g_samplers;  // key -> table index
// Resolve destinations (base page -> the renderer's copy of the target).
struct Resolved {
  plume::RenderTexture* resource = nullptr;
  RenderFormat format = RenderFormat::UNKNOWN, gamma_format = RenderFormat::UNKNOWN;
  uint32_t components = 4;
  bool swap_rb = false;
  uint32_t generation = 0;  // changes with the resource (views are per generation)
  uint32_t bytes = 0;       // the guest span (fingerprinted)
  uint64_t fingerprint = 0;
  uint64_t checked_frame = ~0ull;
};
std::unordered_map<uint32_t, Resolved> g_resolved;
uint32_t g_resolved_generation = 0;
struct ResolvedViewEntry {
  uint32_t index;
  plume::RenderTexture* texture;
  std::shared_ptr<plume::RenderTextureView> view;
};
std::unordered_map<uint64_t, ResolvedViewEntry> g_resolved_views;  // (generation, swizzle, gamma) ->
Stats g_stats;
const bool g_no_cache = std::getenv("SVR2011_NATIVE_NOCACHE") != nullptr;  // (debug)
bool g_check_every_use = false;  // (SetCheckEveryUse: a Threads bake draw)
// (test aid: Threads bake draws never re-check their textures - a missed check, as a slow PC got it)
const bool g_test_bake_stale = std::getenv("SVR2011_TEST_BAKE_STALE") != nullptr;

// 16 slices of 256 bytes spread over the resolve's guest span.
uint64_t ResolvedFingerprint(uint32_t base_address, uint32_t bytes) {
  if (base_address >= 0x20000000u) return 0;
  bytes = std::max<uint32_t>(std::min<uint32_t>(bytes, 0x20000000u - base_address), 256);
  uint64_t h = 0;
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t offset = std::min<uint32_t>(uint32_t(uint64_t(bytes) * k / 16), bytes - 256);
    h = h * 31 + XXH3_64bits(g_physical + base_address + offset, 256);
  }
  return h;
}

bool IsBlockCompressed(uint32_t format) {
  switch (format) {
    case 18: case 19: case 20:  // k_DXT1, k_DXT2_3, k_DXT4_5
    case 49:                    // k_DXN
    case 51: case 52: case 53:  // k_DXT*_AS_16_16_16_16
    case 58: case 59: case 60: case 61:  // k_DXT3A, k_DXT5A, k_CTX1, k_DXT3A_AS_1_1_1_1
      return true;
    default:
      return false;
  }
}

uint64_t GuestHash(const Entry& e) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Done {
    std::chrono::steady_clock::time_point t0;
    ~Done() {
      g_stats.hash_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } done{t0};
  ++g_stats.hashes;
  g_stats.hash_bytes += uint64_t(e.base_size) + e.mip_size;
  uint64_t h = 0;
  if (e.base_size) h ^= XXH3_64bits(g_physical + e.base_address, e.base_size);
  if (e.mip_size) h ^= XXH3_64bits(g_physical + e.mip_address, e.mip_size) * 31;
  return h;
}

// Reads `bytes` guest bytes at physical `address` as the GPU sees them after
// the fetch constant's endian swap: byte o comes from o ^ mask.
void ReadGuest(uint8_t* out, uint32_t address, uint32_t bytes, uint32_t mask) {
  if (address >= kPhysicalSize || bytes > kPhysicalSize - address) {
    std::memset(out, 0, bytes);
    return;
  }
  if (!mask) {
    std::memcpy(out, g_physical + address, bytes);
    return;
  }
  for (uint32_t i = 0; i < bytes; ++i) {
    out[i] = g_physical[((address + i) ^ mask) & (kPhysicalSize - 1)];
  }
}

// ReadGuest for a whole run of bytes starting 4-byte aligned: the endian
// swap works a word at a time.
void ReadGuestRun(uint8_t* out, uint32_t address, uint32_t bytes, uint32_t mask) {
  if ((address & 3) || address >= kPhysicalSize || bytes > kPhysicalSize - address) {
    ReadGuest(out, address, bytes, mask);
    return;
  }
  const uint8_t* src = g_physical + address;
  const uint32_t words = bytes / 4;
  switch (mask) {
    case 0:
      std::memcpy(out, src, bytes);
      return;
    case 3:  // 8in32
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = _byteswap_ulong(w);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
    case 1:  // 8in16
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
    default:  // 2: 16in32
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = (w << 16) | (w >> 16);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
  }
  if (bytes & 3) ReadGuest(out + 4 * words, address + 4 * words, bytes & 3, mask);
}

inline uint8_t Expand(uint32_t v, uint32_t bits) {
  return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

// Writes one guest block (already endian-swapped) to the host subresource.
void StoreBlock(Convert convert, const uint8_t* block, uint32_t bpb, uint8_t* dst_base,
                uint32_t row_pitch, uint32_t host_width, uint32_t host_height, uint32_t bx,
                uint32_t by) {
  switch (convert) {
    case Convert::kCopy:
      std::memcpy(dst_base + size_t(by) * row_pitch + size_t(bx) * bpb, block, bpb);
      return;
    case Convert::k565:
    case Convert::k655:
    case Convert::k1555:
    case Convert::k4444: {
      if (bx >= host_width || by >= host_height) return;
      const uint32_t v = block[0] | (block[1] << 8);
      uint8_t* d = dst_base + size_t(by) * row_pitch + size_t(bx) * 4;
      switch (convert) {
        case Convert::k565:
          d[0] = Expand(v & 31, 5); d[1] = Expand((v >> 5) & 63, 6); d[2] = Expand(v >> 11, 5); d[3] = 255;
          break;
        case Convert::k655:
          d[0] = Expand(v & 63, 6); d[1] = Expand((v >> 6) & 31, 5); d[2] = Expand(v >> 11, 5); d[3] = 255;
          break;
        case Convert::k1555:
          d[0] = Expand(v & 31, 5); d[1] = Expand((v >> 5) & 31, 5); d[2] = Expand((v >> 10) & 31, 5);
          d[3] = (v >> 15) ? 255 : 0;
          break;
        default:
          d[0] = Expand(v & 15, 4); d[1] = Expand((v >> 4) & 15, 4); d[2] = Expand((v >> 8) & 15, 4);
          d[3] = Expand(v >> 12, 4);
          break;
      }
      return;
    }
    case Convert::kDXT3A:
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        dst_base[size_t(y) * row_pitch + x] = uint8_t(((block[i >> 1] >> ((i & 1) * 4)) & 15) * 17);
      }
      return;
    case Convert::kCTX1: {
      const uint8_t e[4][2] = {
          {block[0], block[1]},
          {block[2], block[3]},
          {uint8_t((2 * block[0] + block[2] + 1) / 3), uint8_t((2 * block[1] + block[3] + 1) / 3)},
          {uint8_t((block[0] + 2 * block[2] + 1) / 3), uint8_t((block[1] + 2 * block[3] + 1) / 3)}};
      const uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        const uint8_t* c = e[(indices >> (2 * i)) & 3];
        dst_base[size_t(y) * row_pitch + x * 2] = c[0];
        dst_base[size_t(y) * row_pitch + x * 2 + 1] = c[1];
      }
      return;
    }
    case Convert::kBC1:
    case Convert::kBC2:
    case Convert::kBC3: {
      // (BC2 / BC3: the alpha block first, then the color block)
      uint8_t texels[16][4];
      DecodeBC1Colors(convert == Convert::kBC1 ? block : block + 8, convert != Convert::kBC1, texels);
      if (convert == Convert::kBC2) {
        for (uint32_t i = 0; i < 16; ++i) texels[i][3] = uint8_t(((block[i >> 1] >> ((i & 1) * 4)) & 15) * 17);
      } else if (convert == Convert::kBC3) {
        uint8_t alpha[16];
        DecodeBC4(block, alpha);
        for (uint32_t i = 0; i < 16; ++i) texels[i][3] = alpha[i];
      }
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        std::memcpy(dst_base + size_t(y) * row_pitch + size_t(x) * 4, texels[i], 4);
      }
      return;
    }
    case Convert::kUnorm16Half: {
      // (one texel: bpb / 2 channels)
      if (bx >= host_width || by >= host_height) return;
      uint8_t* d = dst_base + size_t(by) * row_pitch + size_t(bx) * bpb;
      for (uint32_t c = 0; c < bpb / 2; ++c) {
        const uint16_t h = Unorm16ToHalf(uint32_t(block[2 * c]) | (uint32_t(block[2 * c + 1]) << 8));
        std::memcpy(d + 2 * c, &h, 2);
      }
      return;
    }
    case Convert::kBC4:
    case Convert::kBC5: {
      const uint32_t channels = convert == Convert::kBC5 ? 2 : 1;
      uint8_t values[2][16];
      for (uint32_t ch = 0; ch < channels; ++ch) DecodeBC4(block + 8 * ch, values[ch]);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        for (uint32_t ch = 0; ch < channels; ++ch)
          dst_base[size_t(y) * row_pitch + size_t(x) * channels + ch] = values[ch][i];
      }
      return;
    }
    default:
      return;
  }
}

// Debugging: SVR2011_NATIVE_TEXTURE_DUMP=<dir> writes the base level of each
// uploaded DXT texture as <dir>/<n>_<w>x<h>.dds.
void DumpLevel0(TF format, uint32_t width, uint32_t height, const uint8_t* data, uint32_t row_pitch,
                uint32_t rows, size_t row_bytes) {
  static const char* dir = std::getenv("SVR2011_NATIVE_TEXTURE_DUMP");
  static uint32_t count = 0;
  if (!dir) return;
  const char* fourcc = format == TF::k_DXT1 ? "DXT1" : format == TF::k_DXT2_3 ? "DXT3"
                       : format == TF::k_DXT4_5 ? "DXT5" : nullptr;
  if (!fourcc) return;
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%04u_%ux%u.dds", dir, count++, width, height);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  uint32_t header[32] = {};
  header[0] = 0x20534444;  // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;  // caps, height, width, pixel format, linear size
  header[3] = height;
  header[4] = width;
  header[5] = uint32_t(row_bytes * rows);
  header[19] = 32;
  header[20] = 0x4;  // fourcc
  std::memcpy(&header[21], fourcc, 4);
  header[27] = 0x1000;  // texture
  std::fwrite(header, 4, 32, f);
  for (uint32_t y = 0; y < rows; ++y) std::fwrite(data + size_t(y) * row_pitch, 1, row_bytes, f);
  std::fclose(f);
}

// Character select: the "?" tile opens the managers (src/managers.cpp), so its
// pictures (DXT5 textures packed in the menu files: the 64 x 64 grid tile and
// the 128 x 64 banner beside the name) become an "M" (art/managers_*.png,
// tools/make_managers_icon.py). Recognized by their data (FNV-1a of the
// converted base level).
#include "native/managers_icon.inc"

struct TileSwap {
  uint32_t width;
  uint64_t hash;
  const uint8_t* data;
};
const TileSwap kTileSwaps[] = {
    {64, 0xE4E1A919C5A0D7F9ull, kManagersTile},
    {128, 0x823D7E7ED0BC1410ull, kManagersBanner},
};
static_assert(sizeof(kManagersTile) == 64 * 64 && sizeof(kManagersBanner) == 128 * 64);

void ReplaceRandomTile(uint32_t width, uint8_t* data, uint32_t row_pitch) {
  const uint32_t row_bytes = width * 4, rows = 16;  // (4 x 4 blocks, 16 bytes each; 64 high)
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t y = 0; y < rows; ++y)
    for (uint32_t x = 0; x < row_bytes; ++x) h = (h ^ data[size_t(y) * row_pitch + x]) * 0x100000001B3ull;
  for (const TileSwap& t : kTileSwaps) {
    if (t.width != width || t.hash != h) continue;
    if (!rex::cvar::Query<bool>("managers_tile")) return;
    for (uint32_t y = 0; y < rows; ++y) std::memcpy(data + size_t(y) * row_pitch, t.data + y * row_bytes, row_bytes);
    return;
  }
}

plume::RenderSwizzle Swizzle(uint32_t c) {
  switch (c) {
    case 0: return plume::RenderSwizzle::R;
    case 1: return plume::RenderSwizzle::G;
    case 2: return plume::RenderSwizzle::B;
    case 3: return plume::RenderSwizzle::A;
    case 5: return plume::RenderSwizzle::ONE;
    default: return plume::RenderSwizzle::ZERO;
  }
}

uint32_t ResolvedView(const Context& ctx, const Resolved& res,
                      const xenos::xe_gpu_texture_fetch_t& fetch) {
  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma &&
                     res.gamma_format != RenderFormat::UNKNOWN;
  const uint64_t key = uint64_t(res.generation) | (uint64_t(fetch.swizzle) << 32) |
                       (uint64_t(gamma) << 44) | (uint64_t(res.swap_rb) << 45) |
                       (uint64_t(res.format) << 46);
  auto it = g_resolved_views.find(key);
  if (it != g_resolved_views.end()) return it->second.index;
  if (g_srv_next >= g_srv_end) return UINT32_MAX;
  const uint32_t srv = g_srv_next++;
  plume::RenderTextureViewDesc vd = plume::RenderTextureViewDesc::Texture2D(gamma ? res.gamma_format : res.format);
  vd.mipLevels = 1;
  vd.componentMapping = ComponentMapping(fetch.swizzle, res.components, res.swap_rb);
  std::shared_ptr<plume::RenderTextureView> view = res.resource->createTextureView(vd);
  ctx.texture_sets[0]->setTexture(ctx.texture_base[0] + srv, res.resource, plume::RenderTextureLayout::SHADER_READ,
                                  view.get());
  g_resolved_views.emplace(key, ResolvedViewEntry{srv, res.resource, std::move(view)});
  return srv;
}

// A texture's content hash (texture dumps and packs): XXH3 of its first
// level's blocks untiled, in order, as the GPU reads them (after the endian
// swap) - for DXT and A8R8G8B8 the bytes of a PC DDS's top level, so the
// game files' textures hash the same (their names: texture_names). Stable
// across runs and versions; no padding or other guest memory in it. 2D only.
// `data`: the blocks.
uint64_t ContentHash(const xenos::xe_gpu_texture_fetch_t& fetch,
                     const texture_util::TextureGuestLayout& layout, uint32_t base_page, uint32_t width,
                     uint32_t height, uint32_t gbw, uint32_t gbh, uint32_t bpb, uint32_t mask,
                     std::vector<uint8_t>& data) {
  if (!base_page) return 0;
  const uint32_t blocks_x = (width + gbw - 1) / gbw, blocks_y = (height + gbh - 1) / gbh;
  const uint32_t row_bytes = blocks_x * bpb;
  data.resize(size_t(row_bytes) * blocks_y);
  const uint32_t address = base_page << 12;
  const texture_util::TextureGuestLayout::Level& gl = layout.base;
  if (!fetch.tiled) {
    for (uint32_t by = 0; by < blocks_y; ++by)
      ReadGuestRun(data.data() + size_t(by) * row_bytes, address + by * gl.row_pitch_bytes, row_bytes, mask);
  } else {
    const uint32_t pitch_blocks = gl.row_pitch_bytes / bpb, bpb_log2 = Log2(bpb);
    uint8_t* out = data.data();
    for (uint32_t by = 0; by < blocks_y; ++by) {
      for (uint32_t bx = 0; bx < blocks_x; ++bx, out += bpb) {
        const int64_t offset = texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by), pitch_blocks, bpb_log2);
        ReadGuest(out, uint32_t(address + offset), bpb, mask);
      }
    }
  }
  return XXH3_64bits(data.data(), data.size());
}

// Texture packs and dumps handle these formats (4 channels, as PNG / DDS
// have them): the dump files' format tag, or null.
const char* PackFormat(TF f) {
  switch (f) {
    case TF::k_DXT1:
    case TF::k_DXT1_AS_16_16_16_16:
      return "dxt1";
    case TF::k_DXT2_3:
    case TF::k_DXT2_3_AS_16_16_16_16:
      return "dxt3";
    case TF::k_DXT4_5:
    case TF::k_DXT4_5_AS_16_16_16_16:
      return "dxt5";
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
    case TF::k_8_8_8_8_AS_16_16_16_16:
      return "rgba";
    default:
      return nullptr;
  }
}

void StoreBlock(Convert convert, const uint8_t* block, uint32_t bpb, uint8_t* dst_base, uint32_t row_pitch,
                uint32_t host_width, uint32_t host_height, uint32_t bx, uint32_t by);

// A dump's pixels: the top level's blocks (ContentHash) as RGBA8 rows, with
// the fetch constant's swizzle applied - the texture as it looks.
std::vector<uint8_t> DumpPixels(const std::vector<uint8_t>& blocks, TF format, uint32_t w, uint32_t h,
                                uint32_t swizzle) {
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  const char* tag = PackFormat(format);
  if (!std::strcmp(tag, "rgba")) {
    std::memcpy(rgba.data(), blocks.data(), std::min(rgba.size(), blocks.size()));
  } else {
    const Convert c = tag[3] == '1' ? Convert::kBC1 : tag[3] == '3' ? Convert::kBC2 : Convert::kBC3;
    const uint32_t bpb = c == Convert::kBC1 ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx)
        if ((size_t(by) * bw + bx + 1) * bpb <= blocks.size())
          StoreBlock(c, &blocks[(size_t(by) * bw + bx) * bpb], bpb, rgba.data(), w * 4, w, h, bx, by);
  }
  if ((swizzle & 0xFFF) != 0x688) {  // (not xyzw)
    uint8_t px[4];
    for (size_t i = 0; i < rgba.size(); i += 4) {
      std::memcpy(px, &rgba[i], 4);
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t sel = (swizzle >> (3 * k)) & 7;
        rgba[i + k] = sel <= 3 ? px[sel] : sel == 5 ? 255 : 0;
      }
    }
  }
  return rgba;
}

// The stock ring mats (native_mat_fix, on by default): the game's three ring mat diffuses (256 x 1024 DXT5:
// bg00-19 / 22 / 23 / 25 / 26, bg24 + bg29, bg20 - their content hashes; the same on the PS3) have a light
// patch every few texel rows, drawn as rows of faint rectangles on the mat. Replaced, as a texture pack would,
// by the same picture with each texel's brightness brought towards its neighbourhood's (+-8 texels, by at most
// 10%): the patches go, the canvas's grain and its marks (far darker) stay. Made here from the game's own data -
// no files.
constexpr uint64_t kStockMats[] = {0xe1ae7cdd82d3cfcbull, 0x25702b2a9c01e0e1ull, 0xcd7c25e82c51c985ull};
bool StockMat(uint64_t hash) {
  for (uint64_t m : kStockMats)
    if (m == hash) return true;
  return false;
}
std::shared_ptr<const texture_packs::Replacement> EvenMat(std::vector<uint8_t> rgba, uint32_t w, uint32_t h) {
  const size_t n = size_t(w) * h;
  std::vector<float> lum(n), tmp(n);
  double mean = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = &rgba[i * 4];
    mean += lum[i] = p[0] * 0.3f + p[1] * 0.59f + p[2] * 0.11f;
  }
  mean /= double(n);
  // (the neighbourhood: a box blur, wrapping as the mat's texture does)
  constexpr int kNear = 8;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      float sum = 0;
      for (int d = -kNear; d <= kNear; ++d) sum += lum[size_t(y) * w + (x + d + w) % w];
      tmp[size_t(y) * w + x] = sum / (2 * kNear + 1);
    }
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      float sum = 0;
      for (int d = -kNear; d <= kNear; ++d) sum += tmp[size_t((y + d + h) % h) * w + x];
      const double k = std::clamp(mean / std::max(sum / (2 * kNear + 1), 1.0f), 0.9, 1.1);
      uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::clamp(p[c] * k + 0.5, 0.0, 255.0));
    }
  auto r = std::make_shared<texture_packs::Replacement>();
  r->width = w, r->height = h;
  r->format = texture_packs::Format::kRGBA8;
  r->levels.push_back(std::move(rgba));
  // (its mips: 2 x 2 boxes, to 1 x 1)
  for (uint32_t lw = w, lh = h; lw > 1 || lh > 1;) {
    const std::vector<uint8_t>& src = r->levels.back();
    const uint32_t nw = std::max(lw / 2, 1u), nh = std::max(lh / 2, 1u);
    std::vector<uint8_t> dst(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x)
        for (int c = 0; c < 4; ++c) {
          const uint32_t x0 = std::min(x * 2, lw - 1), x1 = std::min(x * 2 + 1, lw - 1);
          const uint32_t y0 = std::min(y * 2, lh - 1), y1 = std::min(y * 2 + 1, lh - 1);
          dst[(size_t(y) * nw + x) * 4 + c] =
              uint8_t((src[(size_t(y0) * lw + x0) * 4 + c] + src[(size_t(y0) * lw + x1) * 4 + c] +
                       src[(size_t(y1) * lw + x0) * 4 + c] + src[(size_t(y1) * lw + x1) * 4 + c] + 2) / 4);
        }
    r->levels.push_back(std::move(dst));
    lw = nw, lh = nh;
  }
  return r;
}


// One subresource's place in the staging buffer (rows 256-byte aligned,
// subresources 512-byte aligned: valid copy sources for D3D12 and Vulkan).
struct Footprint {
  uint64_t offset;
  uint32_t width, height, depth;  // in texels (block-aligned for BC formats)
  uint32_t row_pitch, rows;       // bytes per row of blocks, rows of blocks
};

// PlayStation button pictures (pad_icons.h): the fonts' icon pages are shown
// as a taller picture, one resource for all the copies of a page (each font
// has its own). Recognized by the converted base level's FNV-1a 64.
struct PadPicture {
  std::shared_ptr<plume::RenderTexture> resource;
};
std::map<std::pair<const void*, bool>, PadPicture> g_pad_pictures;  // (picture, gamma)

bool IsPadPicture(const std::shared_ptr<plume::RenderTexture>& r) {
  for (const auto& [key, p] : g_pad_pictures)
    if (p.resource == r) return true;
  return false;
}

// The resource for a picture (created and uploaded the first time).
std::shared_ptr<plume::RenderTexture> PadPictureResource(const Context& ctx, const svr2011::PadIconsPicture& pic,
                                                         RenderFormat format) {
  PadPicture& p = g_pad_pictures[{pic.rgba, format == RenderFormat::R8G8B8A8_UNORM_SRGB}];
  if (p.resource) return p.resource;
  std::shared_ptr<plume::RenderTexture> resource = ctx.device->createTexture(plume::RenderTextureDesc::Texture(
      plume::RenderTextureDimension::TEXTURE_2D, pic.width, pic.height, 1, 1, 1, format, plume::RenderTextureFlag::NONE));
  if (!resource) return nullptr;
  resource->setName(fmt::format("pad icons {}x{}", pic.width, pic.height));
  const uint32_t row_pitch = (pic.width * 4 + 255) & ~255u;
  std::shared_ptr<plume::RenderBuffer> staging =
      ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row_pitch) * pic.height));
  if (!staging) return nullptr;
  auto* mapped = static_cast<uint8_t*>(staging->map());
  for (uint32_t y = 0; y < pic.height; ++y)
    std::memcpy(mapped + size_t(y) * row_pitch, pic.rgba->data() + size_t(y) * pic.width * 4, pic.width * 4);
  staging->unmap();
  ctx.list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::COPY_DEST));
  ctx.list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(resource.get(), 0, 0),
                              plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, pic.width,
                                                                                pic.height, 1, row_pitch / 4, 0));
  ctx.list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::SHADER_READ));
  ctx.retire(staging);
  p.resource = resource;
  REXLOG_INFO("[svr2011] pad icons: picture {}x{} uploaded", pic.width, pic.height);
  return p.resource;
}

// Creates the resource and SRV for a texture and records its upload.
bool Upload(const Context& ctx, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t dimension,
            Entry& e) {
  struct Timed {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Timed() {
      g_stats.upload_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } timed;
  const TF format = fetch.format;
  HostFormat host = g_bc_supported ? GetHostFormat(format) : Decompressed(GetHostFormat(format));
  if (!g_unorm16_filterable) host = Unorm16AsHalf(host);
  const FormatInfo* info = FormatInfo::Get(format);
  if (host.convert == Convert::kNone || !info) return false;

  const xenos::DataDimension dim = fetch.dimension;
  const bool is_3d = dim == xenos::DataDimension::k3D;
  const bool is_cube = dim == xenos::DataDimension::kCube;
  if (dim == xenos::DataDimension::k1D) return false;
  // The view the shader declares must match the texture's type.
  if ((dimension == 1) != is_3d || (dimension == 2) != is_cube) return false;

  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                 &mip_min, &mip_max);
  const uint32_t width = w1 + 1, height = h1 + 1, depth_or_array = d1 + 1;
  if (!base_page && !mip_page) return false;
  // Debugging: SVR2011_NATIVE_DUMP_BASE=<hex physical address> writes the guest bytes of the texture based
  // there (endian-swapped as the GPU reads them; its pitch x height in blocks) to tex_<base>.raw beside the log,
  // each time it is uploaded - the game's own data, for telling it from a conversion.
  static const uint32_t dump_base = [] {
    const char* v = std::getenv("SVR2011_NATIVE_DUMP_BASE");
    return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
  }();
  if (dump_base && (base_page << 12) == dump_base) {
    const FormatInfo* fi = FormatInfo::Get(format);
    const uint32_t bw = fi ? fi->block_width : 1, bh = fi ? fi->block_height : 1, bytes_pb = fi ? fi->bytes_per_block() : 4;
    const uint32_t pitch_blocks = (fetch.pitch * 32 + bw - 1) / bw, rows = (height + bh - 1) / bh;
    std::vector<uint8_t> raw(size_t(pitch_blocks) * bytes_pb * rows);
    const uint32_t mask = fetch.endianness == xenos::Endian::k8in16 ? 1 : fetch.endianness == xenos::Endian::k8in32 ? 3
                          : fetch.endianness == xenos::Endian::k16in32 ? 2 : 0;
    ReadGuest(raw.data(), base_page << 12, uint32_t(raw.size()), mask);
    const std::string path = fmt::format("tex_{:08X}.raw", base_page << 12);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(raw.data(), 1, raw.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("native texture dump: {:08X} format {} {}x{} pitch {} blocks, tiled {}, endian {} -> {} ({} bytes)",
                base_page << 12, uint32_t(format), width, height, pitch_blocks, uint32_t(fetch.tiled),
                uint32_t(fetch.endianness), path, raw.size());
  }
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      dim, fetch.pitch, width, height, depth_or_array, fetch.tiled, format, fetch.packed_mips,
      base_page != 0, mip_max);

  const uint32_t array_size = is_3d ? 1 : depth_or_array;
  const uint32_t depth = is_3d ? depth_or_array : 1;
  // A re-upload keeps the texture's layout (a quality change makes new ones).
  uint32_t skip = e.resource ? e.skip : SkipLevels(fetch);
  if (skip && skip >= layout.packed_level) skip = 0;  // (not into the packed tail)
  // the guest's levels, and the host texture's: guest level `skip` up
  const uint32_t guest_levels = mip_max + 1;
  const uint32_t levels = guest_levels - skip;
  // Only a 2D view of the first slice is possible for stacked textures.
  if (dimension == 0 && array_size != 1) return false;

  const uint32_t gbw = info->block_width, gbh = info->block_height;
  const uint32_t bpb = info->bytes_per_block();
  if (!bpb || (bpb & (bpb - 1))) return false;
  // Block-compressed host textures need the base size in whole blocks.
  // (with levels left out: the first host level's)
  const uint32_t first_w = std::max(width >> skip, 1u), first_h = std::max(height >> skip, 1u);
  const uint32_t host_width = host.block_compressed ? (first_w + 3) & ~3u : first_w;
  const uint32_t host_height = host.block_compressed ? (first_h + 3) & ~3u : first_h;

  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
  RenderFormat resource_format = gamma && host.gamma != RenderFormat::UNKNOWN ? host.gamma : host.format;
  // (an icon page with PlayStation pictures - pad_icons.h - recognized below)
  const bool pad_candidate = (format == TF::k_DXT4_5 || format == TF::k_DXT1) && !is_3d && !is_cube &&
                             array_size == 1 && guest_levels == 1 && svr2011::PadIconsCandidate(width, height);

  // A re-upload (guest data changed) reuses the texture and its view: the
  // entry's key fixes the layout, and frames still in flight keep a valid
  // descriptor (queue order serializes the copy after their reads).
  // (a texture shown as a pad icons picture before - its data has changed -
  // gets a resource of its own)
  const bool reuse = e.resource != nullptr && !IsPadPicture(e.resource);
  std::shared_ptr<plume::RenderTexture> resource = reuse ? e.resource : nullptr;
  auto create_resource = [&]() -> bool {
    if (reuse) return true;
    plume::RenderTextureDesc desc =
        is_3d ? plume::RenderTextureDesc::Texture3D(host_width, host_height, depth, levels, resource_format)
              : plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, host_width,
                                                  host_height, 1, levels, array_size, resource_format,
                                                  is_cube ? plume::RenderTextureFlag::CUBE
                                                          : plume::RenderTextureFlag::NONE);
    resource = ctx.device->createTexture(desc);
    if (!resource) return false;
    resource->setName(fmt::format("texture {:08X} {}x{}x{} fmt {}", base_page << 12, width, height,
                                  is_3d ? depth : array_size, uint32_t(format)));
    return true;
  };

  const uint32_t subresources = levels * array_size;
  const uint32_t host_bw = plume::RenderFormatBlockWidth(resource_format);
  const uint32_t host_bpb = plume::RenderFormatSize(resource_format);
  std::vector<Footprint> footprints(subresources);
  uint64_t total = 0;
  for (uint32_t slice = 0; slice < array_size; ++slice) {
    for (uint32_t level = 0; level < levels; ++level) {
      Footprint& fp = footprints[level + slice * levels];
      const uint32_t w = std::max(host_width >> level, 1u), h = std::max(host_height >> level, 1u);
      fp.width = (w + host_bw - 1) / host_bw * host_bw;
      fp.height = (h + host_bw - 1) / host_bw * host_bw;
      fp.depth = std::max(depth >> level, 1u);
      fp.row_pitch = ((fp.width / host_bw) * host_bpb + 255) & ~255u;
      fp.rows = fp.height / host_bw;
      fp.offset = (total + 511) & ~511ull;
      total = fp.offset + uint64_t(fp.row_pitch) * fp.rows * fp.depth;
    }
  }
  // Staging: the frame's upload ring (textures that change every frame -
  // videos - would otherwise create and free a buffer per frame), or a buffer
  // of its own when the ring has no room.
  std::shared_ptr<plume::RenderBuffer> staging;
  plume::RenderBuffer* staging_buffer = nullptr;
  uint64_t staging_offset = 0;
  uint8_t* mapped = nullptr;
  if (ctx.allocate) {
    const Context::UploadSpace space = ctx.allocate(total, 512);
    mapped = space.cpu;
    staging_buffer = space.buffer;
    staging_offset = space.offset;
  }
  if (!mapped) {
    staging = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    if (!staging) return false;
    mapped = static_cast<uint8_t*>(staging->map());
    staging_buffer = staging.get();
  }
  std::memset(mapped, 0, size_t(total));

  static const uint32_t kEndianMask[4] = {0, 1, 3, 2};
  const uint32_t mask = kEndianMask[uint32_t(fetch.endianness) & 3];
  const uint32_t bpb_log2 = Log2(bpb);
  uint8_t block[16];
  // Texture packs and dumps (texture_packs.h): a new 2D texture's content
  // hash, dumped once and replaced when a pack has it (Texture() swaps it in
  // once the file is read). (SVR2011_TEXTURE_HASH_LOG: each new texture's hash)
  static const bool hash_log = std::getenv("SVR2011_TEXTURE_HASH_LOG") != nullptr;
  const bool mat_shape = width == 256 && height == 1024 && format == TF::k_DXT4_5 && REXCVAR_GET(native_mat_fix);
  if ((!reuse || e.changes <= kPackChanges) && dimension == 0 && array_size == 1 && !is_3d && !is_cube &&
      !pad_candidate && base_page && (hash_log || texture_packs::Active() || mat_shape)) {
    const char* tag = PackFormat(format);
    if (tag || hash_log) {
      thread_local std::vector<uint8_t> blocks;
      const uint64_t h = ContentHash(fetch, layout, base_page, width, height, gbw, gbh, bpb, mask, blocks);
      if (hash_log) {
        REXLOG_INFO("texture hash {:016x} format {} {}x{} levels {} tiled {} endian {} base {:08X}", h,
                    uint32_t(format), width, height, guest_levels, uint32_t(fetch.tiled),
                    uint32_t(fetch.endianness), base_page << 12);
      }
      if (tag && h && texture_packs::WantDump(h))
        texture_packs::Dump(h, width, height, DumpPixels(blocks, format, width, height, fetch.swizzle), tag);
      if (tag && h && e.changes <= kPackChanges) {
        e.ready = nullptr;
        e.pending = texture_packs::Active() && texture_packs::Find(h, &e.ready) != texture_packs::Lookup::kNone ? h : 0;
        // (a stock ring mat, no pack of its own for it: the evened-out one)
        if (mat_shape && StockMat(h) && !e.pending) {
          e.ready = EvenMat(DumpPixels(blocks, format, width, height, fetch.swizzle), width, height);
          e.pending = h;
          static int logged = 0;
          if (logged++ < 8) REXLOG_INFO("native renderer: stock ring mat {:016x} - evened out (native_mat_fix)", h);
        }
      }
    }
  }

  const auto convert_t0 = std::chrono::steady_clock::now();
  for (uint32_t host_level = 0; host_level < levels; ++host_level) {
    const uint32_t level = host_level + skip;  // the guest's
    const uint32_t page = level ? mip_page : base_page;
    if (!page) continue;  // level not present (left black)
    const uint32_t stored = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& gl = level ? layout.mips[stored] : layout.base;
    const uint32_t level_address = (page << 12) + (level ? layout.mip_offsets_bytes[stored] : 0);
    uint32_t ox = 0, oy = 0, oz = 0;
    if (level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, depth, format, level, ox, oy, oz);
    }
    const uint32_t lw = std::max(host_width >> host_level, 1u), lh = std::max(host_height >> host_level, 1u);
    const uint32_t ld = std::max(depth >> level, 1u);
    const uint32_t blocks_x = (lw + gbw - 1) / gbw, blocks_y = (lh + gbh - 1) / gbh;
    const uint32_t pitch_blocks = gl.row_pitch_bytes / bpb;

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      const uint32_t sub = host_level + slice * levels;
      const Footprint& fp = footprints[sub];
      const uint32_t slice_address = level_address + slice * gl.array_slice_stride_bytes;
      for (uint32_t z = 0; z < ld; ++z) {
        uint8_t* dst = mapped + fp.offset + size_t(z) * fp.rows * fp.row_pitch;
        if (!fetch.tiled && host.convert == Convert::kCopy) {
          // Linear and copied as is: whole rows (videos update such
          // textures every frame; per block, 8-bit ones cost a call a texel).
          thread_local std::vector<uint8_t> row;
          const uint32_t row_bytes_n = blocks_x * bpb;
          if (row.size() < row_bytes_n) row.resize(row_bytes_n);
          for (uint32_t by = 0; by < blocks_y; ++by) {
            const int64_t offset = (int64_t(z + oz) * gl.z_slice_stride_block_rows + by + oy) *
                                       gl.row_pitch_bytes + int64_t(ox) * bpb;
            ReadGuestRun(row.data(), uint32_t(slice_address + offset), row_bytes_n, mask);
            std::memcpy(dst + size_t(by) * fp.row_pitch, row.data(), row_bytes_n);
          }
          continue;
        }
        for (uint32_t by = 0; by < blocks_y; ++by) {
          for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint32_t gx = bx + ox, gy = by + oy, gz = z + oz;
            int64_t offset;
            if (fetch.tiled) {
              offset = is_3d ? texture_util::GetTiledOffset3D(int32_t(gx), int32_t(gy), int32_t(gz),
                                                              pitch_blocks, gl.z_slice_stride_block_rows,
                                                              bpb_log2)
                             : texture_util::GetTiledOffset2D(int32_t(gx), int32_t(gy), pitch_blocks,
                                                              bpb_log2);
            } else {
              offset = (int64_t(gz) * gl.z_slice_stride_block_rows + gy) * gl.row_pitch_bytes +
                       int64_t(gx) * bpb;
            }
            ReadGuest(block, uint32_t(slice_address + offset), bpb, mask);
            StoreBlock(host.convert, block, bpb, dst, fp.row_pitch, lw, lh, bx, by);
          }
        }
      }
    }
  }
  g_stats.convert_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convert_t0).count();
  // (the replacement pictures are DXT5 blocks: not on a GPU without BC)
  if (format == TF::k_DXT4_5 && host.block_compressed && (width == 64 || width == 128) && height == 64 &&
      array_size == 1)
    ReplaceRandomTile(width, mapped + footprints[0].offset, footprints[0].row_pitch);
  // An icon page: the shared taller picture instead (pad_icons.h).
  uint64_t pad_hash = 0;
  if (pad_candidate) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint32_t row_bytes = (width / 4) * (format == TF::k_DXT1 ? 8 : 16);
    for (uint32_t y = 0; y < footprints[0].rows; ++y) {
      const uint8_t* row = mapped + footprints[0].offset + size_t(y) * footprints[0].row_pitch;
      for (uint32_t x = 0; x < row_bytes; ++x) h = (h ^ row[x]) * 0x100000001B3ull;
    }
    pad_hash = h;
    const svr2011::PadIconsPicture pic = svr2011::PadIconsAtlas(width, height, h);
    if (pic.rgba) {
      const RenderFormat rgba = gamma ? RenderFormat::R8G8B8A8_UNORM_SRGB : RenderFormat::R8G8B8A8_UNORM;
      if (auto shared = PadPictureResource(ctx, pic, rgba)) {
        if (staging) {
          staging->unmap();
          ctx.retire(staging);
        }
        resource = shared;
        resource_format = rgba;
        goto view;
      }
    }
  }
  // (a picture with PlayStation / keyboard versions: SetTexture swaps it)
  svr2011::PadIconsTextureUploaded(base_page << 12, width, height, pad_hash);
  if (!create_resource()) {
    if (staging) staging->unmap();
    return false;
  }
  ctx.list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t level = 0; level < levels; ++level) {
    DumpLevel0(format, footprints[level].width, footprints[level].height, mapped + footprints[level].offset,
               footprints[level].row_pitch, footprints[level].rows,
               size_t(footprints[level].width / host_bw) * host_bpb);
  }
  if (staging) staging->unmap();

  for (uint32_t sub = 0; sub < subresources; ++sub) {
    const Footprint& fp = footprints[sub];
    const auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
        staging_buffer, resource_format, fp.width, fp.height, fp.depth, fp.row_pitch / host_bpb * host_bw,
        staging_offset + fp.offset);
    const auto dst = plume::RenderTextureCopyLocation::Subresource(resource.get(), sub % levels, sub / levels);
    ctx.list->copyTextureRegion(dst, src);
  }
  ctx.list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::SHADER_READ));
  if (staging) ctx.retire(staging);

  // The view, with the fetch constant's swizzle (Xenos and D3D12 encode
  // component selection the same way: 0-3 xyzw, 4 zero, 5 one).
view:
  if (reuse && resource == e.resource) {
    e.hash = GuestHash(e);
    e.checked_frame = ctx.frame;
    ++g_stats.uploads;
    ++g_stats.uploads_total;
    g_stats.upload_bytes += total;
    return true;
  }
  if (e.srv == UINT32_MAX) {
    if (g_srv_next >= g_srv_end) {
      REXLOG_WARN("native renderer: texture descriptor heap full");
      return false;
    }
    e.srv = g_srv_next++;
  }
  plume::RenderTextureViewDesc vd;
  vd.format = resource_format;
  vd.dimension = is_3d ? plume::RenderTextureViewDimension::TEXTURE_3D
                 : is_cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                           : plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = levels;
  vd.componentMapping = ComponentMapping(fetch.swizzle, host.components);
  e.view = resource->createTextureView(vd);
  ctx.texture_sets[dimension]->setTexture(ctx.texture_base[dimension] + e.srv, resource.get(),
                                          plume::RenderTextureLayout::SHADER_READ,
                                          e.view.get());

  e.resource = resource;
  e.skip = skip;
  if (!IsPadPicture(resource)) {
    e.bytes = total;
    g_resident_bytes += total;
    if (skip) ++g_reduced;
  }
  e.base_address = base_page << 12;
  e.base_size = base_page ? std::min(layout.base.level_data_extent_bytes, kPhysicalSize - e.base_address) : 0;
  e.mip_address = mip_page << 12;
  e.mip_size = mip_page ? std::min(layout.mips_total_extent_bytes, kPhysicalSize - e.mip_address) : 0;
  e.hash = GuestHash(e);
  e.checked_frame = ctx.frame;
  ++g_stats.uploads;
  ++g_stats.uploads_total;
  g_stats.upload_bytes += total;
  static const bool log = std::getenv("SVR2011_NATIVE_TEXTURE_LOG") != nullptr;
  if (log) {
    static const char kSwizzle[] = "xyzw01??";
    char swizzle[5] = {};
    for (uint32_t i = 0; i < 4; ++i) swizzle[i] = kSwizzle[(fetch.swizzle >> (3 * i)) & 7];
    REXLOG_INFO(
        "native texture {}: {} {}x{}x{} dim {} levels {} (min {}, {} left out) tiled {} packed {} endian {} "
        "swizzle {} sign {}{}{}{} base {:08X} mips {:08X} pitch {} bias {} filter {}/{}/{} aniso {}",
        e.srv, info->name, width, height, depth_or_array, uint32_t(dim), guest_levels, mip_min, skip,
        uint32_t(fetch.tiled), uint32_t(fetch.packed_mips), uint32_t(fetch.endianness), swizzle,
        uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
        uint32_t(fetch.sign_w), e.base_address, e.mip_address, uint32_t(fetch.pitch),
        int32_t(fetch.lod_bias), uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
        uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter));
  }
  return true;
}

// A pack's texture (e.ready) in place of the game's: a new resource in a new
// descriptor slot (the old one may still be read by frames in flight).
bool Replace(const Context& ctx, Entry& e, const xenos::xe_gpu_texture_fetch_t& fetch) {
  const std::shared_ptr<const texture_packs::Replacement> r = std::move(e.ready);
  e.ready = nullptr;
  const uint64_t hash = e.pending;
  e.pending = 0;
  if (!r || r->levels.empty()) return false;
  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
  RenderFormat format;
  uint32_t block_bytes = 0;  // (0: RGBA8)
  switch (r->format) {
    case texture_packs::Format::kBC1:
      format = gamma ? RenderFormat::BC1_UNORM_SRGB : RenderFormat::BC1_UNORM, block_bytes = 8;
      break;
    case texture_packs::Format::kBC2:
      format = gamma ? RenderFormat::BC2_UNORM_SRGB : RenderFormat::BC2_UNORM, block_bytes = 16;
      break;
    case texture_packs::Format::kBC3:
      format = gamma ? RenderFormat::BC3_UNORM_SRGB : RenderFormat::BC3_UNORM, block_bytes = 16;
      break;
    default:
      format = gamma ? RenderFormat::R8G8B8A8_UNORM_SRGB : RenderFormat::R8G8B8A8_UNORM;
      break;
  }
  if (block_bytes && !g_bc_supported) {
    static bool logged = false;
    if (!logged) REXLOG_WARN("texture packs: DXT (DDS) textures need a GPU with BC; use PNG files on this one");
    logged = true;
    return false;
  }
  if (g_srv_next >= g_srv_end) return false;
  // Levels with their data (a DDS may stop short).
  std::vector<Footprint> fps;
  uint64_t total = 0;
  for (uint32_t l = 0; l < r->levels.size(); ++l) {
    const uint32_t w = std::max(r->width >> l, 1u), h = std::max(r->height >> l, 1u);
    Footprint fp;
    const uint32_t row_bytes = block_bytes ? (w + 3) / 4 * block_bytes : w * 4;
    fp.rows = block_bytes ? (h + 3) / 4 : h;
    if (r->levels[l].size() < size_t(row_bytes) * fp.rows) break;
    fp.width = block_bytes ? (w + 3) & ~3u : w;
    fp.height = block_bytes ? (h + 3) & ~3u : h;
    fp.depth = 1;
    fp.row_pitch = (row_bytes + 255) & ~255u;
    fp.offset = (total + 511) & ~511ull;
    total = fp.offset + uint64_t(fp.row_pitch) * fp.rows;
    fps.push_back(fp);
  }
  if (fps.empty()) return false;
  const uint32_t levels = uint32_t(fps.size());
  std::shared_ptr<plume::RenderTexture> resource = ctx.device->createTexture(plume::RenderTextureDesc::Texture(
      plume::RenderTextureDimension::TEXTURE_2D, r->width, r->height, 1, levels, 1, format,
      plume::RenderTextureFlag::NONE));
  if (!resource) return false;
  resource->setName(fmt::format("pack texture {:016x}", hash));
  std::shared_ptr<plume::RenderBuffer> staging;
  plume::RenderBuffer* staging_buffer = nullptr;
  uint64_t staging_offset = 0;
  uint8_t* mapped = nullptr;
  if (ctx.allocate) {
    const Context::UploadSpace space = ctx.allocate(total, 512);
    mapped = space.cpu;
    staging_buffer = space.buffer;
    staging_offset = space.offset;
  }
  if (!mapped) {
    staging = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    if (!staging) return false;
    mapped = static_cast<uint8_t*>(staging->map());
    staging_buffer = staging.get();
  }
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t row_bytes = uint32_t(r->levels[l].size() / fps[l].rows);
    for (uint32_t y = 0; y < fps[l].rows; ++y)
      std::memcpy(mapped + fps[l].offset + size_t(y) * fps[l].row_pitch, r->levels[l].data() + size_t(y) * row_bytes,
                  row_bytes);
  }
  if (staging) staging->unmap();
  const uint32_t bw = plume::RenderFormatBlockWidth(format), bpb = plume::RenderFormatSize(format);
  ctx.list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t l = 0; l < levels; ++l) {
    ctx.list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(resource.get(), l, 0),
                                plume::RenderTextureCopyLocation::PlacedFootprint(
                                    staging_buffer, format, fps[l].width, fps[l].height, 1,
                                    fps[l].row_pitch / bpb * bw, staging_offset + fps[l].offset));
  }
  ctx.list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::SHADER_READ));
  if (staging) ctx.retire(staging);
  plume::RenderTextureViewDesc vd;
  vd.format = format;
  vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = levels;
  vd.componentMapping = ComponentMapping(0x688, 4);  // (xyzw: the file is the texture as it looks)
  std::shared_ptr<plume::RenderTextureView> view = resource->createTextureView(vd);
  const uint32_t srv = g_srv_next++;
  ctx.texture_sets[0]->setTexture(ctx.texture_base[0] + srv, resource.get(), plume::RenderTextureLayout::SHADER_READ,
                                  view.get());
  if (e.view) ctx.retire(std::move(e.view));
  if (e.resource && !IsPadPicture(e.resource)) ctx.retire(std::move(e.resource));
  g_resident_bytes = g_resident_bytes - std::min(g_resident_bytes, e.bytes) + total;
  e.bytes = total;
  e.resource = std::move(resource);
  e.view = std::move(view);
  e.srv = srv;
  e.skip = 0;
  e.replaced = true;
  ++g_replaced;
  static uint32_t logged = 0;
  if (logged++ < 20) {
    REXLOG_INFO("texture packs: {:016x} replaced ({}x{}, {} levels{})", hash, r->width, r->height, levels,
                block_bytes ? ", DXT" : "");
  }
  return true;
}

// The cache's key: everything but the sampler state (clamp, filters, LOD
// bias, border).
uint64_t TextureKey(const uint32_t fetch_dwords[6], uint32_t dimension) {
  const uint32_t key_dwords[7] = {fetch_dwords[0] & 0xFFC003FFu, fetch_dwords[1], fetch_dwords[2],
                                  fetch_dwords[3] & 0x7FFFFu,     fetch_dwords[4] & 0x3FCu,
                                  fetch_dwords[5] & 0xFFFFFE00u,  dimension};
  return XXH3_64bits(key_dwords, sizeof(key_dwords));
}

}  // namespace

void Initialize(rex::memory::Memory* memory, uint32_t srv_first, uint32_t srv_count,
                uint32_t sampler_first, uint32_t sampler_count) {
  g_memory = memory;
  g_physical = memory->TranslatePhysical(0);
  g_srv_next = srv_first;
  g_srv_end = srv_first + srv_count;
  g_sampler_next = sampler_first;
  g_sampler_end = sampler_first + sampler_count;
  if (!g_bc_supported) REXLOG_INFO("native renderer: BC (DXT) textures are decoded on the CPU");
  texture_packs::Start();
}

uint32_t Texture(const Context& ctx, const uint32_t fetch_dwords[6], uint32_t dimension) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  // Type 2 is a texture; the game also samples through type 0 ("invalid")
  // constants (entrances, finishers), which the Xbox 360 treats as textures.
  if (uint32_t(fetch.type) != 2 && uint32_t(fetch.type) != 0) {  // not a texture
    ++g_stats.unsupported;
    return UINT32_MAX;
  }
  // A block-compressed texture where a render target was copied: the game
  // reused that memory (the GPU can't resolve to DXT), so the copy is stale -
  // after Superstar Threads, Tyson Kidd's roster picture showed its editor
  // preview (a white square).
  if (IsBlockCompressed(uint32_t(fetch.format)) && g_resolved.erase(fetch.base_address)) ++g_generation;
  if (g_no_cache || g_check_every_use) ++g_generation;  // (every use checked: no answer kept)
  // Any texture the game loaded where a resolve went: a resolve leaves guest
  // memory alone, so changed memory is the game's new texture (after a match,
  // Superstar Threads' paint layers landed on the match's resolves and a
  // painted attire baked with pieces of those).
  if (auto it = g_resolved.find(fetch.base_address);
      it != g_resolved.end() && it->second.checked_frame != ctx.frame) {
    it->second.checked_frame = ctx.frame;
    if (ResolvedFingerprint(fetch.base_address << 12, it->second.bytes) != it->second.fingerprint) {
      static uint32_t logged = 0;
      if (logged++ < 16) {
        REXLOG_INFO("native renderer: resolve {:08X} forgotten (the game wrote a texture there)",
                    fetch.base_address << 12);
      }
      g_resolved.erase(it);
      ++g_generation;
    }
  }
  if (auto it = g_resolved.find(fetch.base_address); it != g_resolved.end()) {
    // A render target copy: sampled from the renderer's resource.
    if (dimension != 0) {
      ++g_stats.unsupported;
      return UINT32_MAX;
    }
    return ResolvedView(ctx, it->second, fetch);
  }
  Entry& e = g_textures[TextureKey(fetch_dwords, dimension)];
  if (e.failed) {
    ++g_stats.unsupported;
    return UINT32_MAX;
  }
  if (!e.resource) {
    ++g_generation;
    e.created_frame = ctx.frame;
    if (!Upload(ctx, fetch, dimension, e)) {
      e.failed = true;
      static uint32_t logged = 0;
      if (logged++ < 32) {
        REXLOG_INFO("native renderer: texture not supported: format {} dimension {}/{} {}x{}",
                    uint32_t(fetch.format), uint32_t(fetch.dimension), dimension,
                    fetch.size_2d.width + 1, fetch.size_2d.height + 1);
      }
      ++g_stats.unsupported;
      return UINT32_MAX;
    }
  } else if (g_check_every_use && g_test_bake_stale) {
  } else if (g_no_cache || g_check_every_use || e.written || ctx.frame > e.used_frame + kUnusedRecheckFrames ||
             ctx.frame >= e.checked_frame +
                              (e.dynamic ? e.interval
                               : ctx.frame < e.created_frame + kNewFrames ? kNewRecheckFrames
                                                                          : kRecheckFrames)) {
    // (a Threads bake draw: same frame as its last check - before, a layer drawn then was the previous one)
    const bool same_frame = e.checked_frame == ctx.frame;
    e.checked_frame = ctx.frame;
    e.written = false;
    const bool changed = GuestHash(e) != e.hash;
    if (g_check_every_use) {
      static uint32_t logged = 0;
      if (logged++ < 64) {
        REXLOG_INFO("native renderer: threads bake: texture {:08X} format {} {}x{} {} (frame {}{})", e.base_address,
                    uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                    changed ? "changed" : "same", ctx.frame, same_frame ? ", checked again in it" : "");
      }
    }
    if (changed) {
      static const bool log_changes = std::getenv("SVR2011_LOG_TEXTURE_CHANGES") != nullptr;  // (debug)
      if (log_changes) {
        REXLOG_INFO("native renderer: texture {:08X} changed (frame {})", e.base_address, ctx.frame);
      }
      if (!e.dynamic) {
        static uint32_t logged = 0;
        if (logged++ < 40) {
          REXLOG_INFO("native renderer: dynamic texture {:08X} format {} {}x{} tiled {} ({} KB)",
                      e.base_address, uint32_t(fetch.format), fetch.size_2d.width + 1,
                      fetch.size_2d.height + 1, uint32_t(fetch.tiled), e.base_size >> 10);
        }
      }
      if (e.replaced || e.pending) {  // (a pack's texture: the game's own again, now that it changes)
        if (e.view) ctx.retire(std::move(e.view));
        if (e.resource && !IsPadPicture(e.resource)) ctx.retire(std::move(e.resource));
        e.resource = nullptr;
        g_resident_bytes -= std::min(g_resident_bytes, e.bytes);
        e.bytes = 0;
        e.srv = UINT32_MAX;
        e.replaced = false;
        e.pending = 0;
        e.ready = nullptr;
      }
      e.dynamic = true;
      e.interval = 1;
      ++e.changes;
      ++g_generation;
      Upload(ctx, fetch, dimension, e);
    } else if (e.dynamic) {
      e.interval = uint32_t(std::min<uint64_t>(uint64_t(e.interval) * 2, kRecheckFrames));
    }
  }
  // A pack's texture, once read.
  if (e.pending && e.changes <= kPackChanges) {
    if (!e.ready && texture_packs::Find(e.pending, &e.ready) == texture_packs::Lookup::kNone) e.pending = 0;
    if (e.ready && !Replace(ctx, e, fetch)) e.pending = 0;
    ++g_generation;
  }
  e.used_frame = ctx.frame;
  return e.srv;
}

uint64_t Generation() { return g_generation.load(std::memory_order_relaxed); }

uint32_t Sampler(const Context& ctx, const uint32_t fetch[6]) {
  const uint64_t base_key = uint64_t((fetch[0] >> 10) & 0x1FF) | (uint64_t((fetch[3] >> 19) & 0x7FF) << 9) |
                       (uint64_t((fetch[4] >> 2) & 0xFF) << 20) |
                       (uint64_t((fetch[4] >> 12) & 0x3FF) << 28) | (uint64_t(fetch[5] & 3) << 38);
  // Texture quality: the texture starts at guest level `skip`, so the LOD
  // clamps move down as many (guest LOD 1 is host LOD 0).
  xenos::xe_gpu_texture_fetch_t tf;
  std::memcpy(&tf, fetch, sizeof(tf));
  uint32_t skip = SkipLevels(tf);
  // (a pack's texture keeps its own levels)
  if (skip && texture_packs::Active()) {
    auto t = g_textures.find(TextureKey(fetch, 0));
    if (t != g_textures.end() && t->second.replaced) skip = 0;
  }
  const uint64_t key = base_key | (uint64_t(skip) << 40);
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second.index;
  if (g_sampler_next >= g_sampler_end) return 0;  // the default linear-wrap sampler

  using plume::RenderTextureAddressMode;
  static const RenderTextureAddressMode kAddress[8] = {
      RenderTextureAddressMode::WRAP,   RenderTextureAddressMode::MIRROR,
      RenderTextureAddressMode::CLAMP,  RenderTextureAddressMode::MIRROR_ONCE,
      RenderTextureAddressMode::CLAMP,  RenderTextureAddressMode::MIRROR_ONCE,
      RenderTextureAddressMode::BORDER, RenderTextureAddressMode::MIRROR_ONCE};
  const uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3;
  const uint32_t aniso = (fetch[3] >> 25) & 7;
  auto filter = [](uint32_t f) {  // point / linear (+ fetch-const)
    return f != 0 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  };
  plume::RenderSamplerDesc sd;
  if (aniso >= 2 && aniso <= 5) {
    sd.minFilter = sd.magFilter = plume::RenderFilter::LINEAR;
    sd.mipmapMode = plume::RenderMipmapMode::LINEAR;
    sd.anisotropyEnabled = true;
    sd.maxAnisotropy = 1u << (aniso - 1);
  } else {
    sd.minFilter = filter(min);
    sd.magFilter = filter(mag);
    sd.mipmapMode = mip == 1 || mip == 3 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
    sd.anisotropyEnabled = false;
    sd.maxAnisotropy = 1;
  }
  sd.addressU = kAddress[(fetch[0] >> 10) & 7];
  sd.addressV = kAddress[(fetch[0] >> 13) & 7];
  sd.addressW = kAddress[(fetch[0] >> 16) & 7];
  const int32_t bias = int32_t((fetch[4] >> 12) & 0x3FF) << 22 >> 22;
  sd.mipLODBias = std::clamp(bias / 32.0f, -16.0f, 15.99f);
  const uint32_t min_lod = (fetch[4] >> 2) & 15, max_lod = (fetch[4] >> 6) & 15;
  sd.minLOD = float(min_lod > skip ? min_lod - skip : 0);
  sd.maxLOD = mip == 2 ? sd.minLOD : float(max_lod > skip ? max_lod - skip : 0);
  static const bool lod0 = std::getenv("SVR2011_NATIVE_LOD0") != nullptr;  // debug
  if (lod0) sd.minLOD = sd.maxLOD = 0.0f;
  sd.comparisonEnabled = false;
  sd.borderColor = (fetch[5] & 3) == 0   ? plume::RenderBorderColor::TRANSPARENT_BLACK
                   : (fetch[5] & 3) == 1 ? plume::RenderBorderColor::OPAQUE_WHITE
                                         : plume::RenderBorderColor::OPAQUE_BLACK;
  std::unique_ptr<plume::RenderSampler> sampler = ctx.device->createSampler(sd);
  if (!sampler) return 0;
  const uint32_t index = g_sampler_next++;
  ctx.sampler_set->setSampler(index, sampler.get());
  g_samplers.emplace(key, SamplerEntry{index, std::move(sampler)});
  return index;
}

void SetUnorm16Filterable(bool filterable) { g_unorm16_filterable = filterable; }

void SetQuality(uint32_t skip) {
  skip = std::min(skip, 2u);
  if (skip == g_quality_skip) return;
  g_quality_skip = skip;
  ++g_generation;
  // Uploaded again as they're used, into their own descriptor slots.
  uint32_t dropped = 0;
  for (auto& [key, e] : g_textures) {
    if (!e.resource && !e.failed) continue;
    e.resource = nullptr;
    e.view = nullptr;
    e.failed = false;
    e.dynamic = false;
    e.skip = 0;
    e.bytes = 0;
    e.replaced = false;
    e.pending = 0;
    e.ready = nullptr;
    ++dropped;
  }
  g_resident_bytes = 0;
  g_reduced = 0;
  REXLOG_INFO("native renderer: texture quality {} ({} mip levels left out of big textures); {} textures "
              "to upload again",
              skip == 0 ? "high" : skip == 1 ? "medium" : "low", skip, dropped);
}

void SetBlockCompressionSupported(bool supported) {
  g_bc_supported = supported && std::getenv("SVR2011_NATIVE_NO_BC") == nullptr;
}

void ForgetResolved() {
  g_resolved.clear();
  ++g_generation;
}

void ForgetResolved(uint32_t base_address) {
  g_resolved.erase(base_address >> 12);
  ++g_generation;
}

void GuestWritten(uint32_t address, uint32_t size) {
  ++g_generation;
  const uint64_t end = uint64_t(address) + size;
  for (auto& [key, e] : g_textures) {
    const bool base = e.base_size && e.base_address < end && address < uint64_t(e.base_address) + e.base_size;
    const bool mips = e.mip_size && e.mip_address < end && address < uint64_t(e.mip_address) + e.mip_size;
    if (base || mips) e.written = true;
  }
}

void SetCheckEveryUse(bool on) {  // (set around every draw: only a change moves the generation)
  if (g_check_every_use == on) return;
  g_check_every_use = on;
  ++g_generation;
}

void RegisterResolved(uint32_t base_address, plume::RenderTexture* resource, RenderFormat format,
                      RenderFormat gamma_format, uint32_t components, bool swap_rb, uint32_t bytes) {
  ++g_generation;
  Resolved& r = g_resolved[base_address >> 12];
  r.bytes = bytes;
  r.fingerprint = ResolvedFingerprint(base_address, bytes);
  r.checked_frame = ~0ull;
  if (r.resource == resource && r.format == format && r.swap_rb == swap_rb) return;
  // Views are per resource generation (and format/swap, in their key), so a
  // target alternating formats reuses its views.
  const uint32_t generation = r.resource == resource ? r.generation : ++g_resolved_generation;
  r = {resource, format, gamma_format, components, swap_rb, generation, r.bytes, r.fingerprint};
}

void ReleaseResolved(const Context& ctx, plume::RenderTexture* texture) {
  ++g_generation;
  for (auto it = g_resolved_views.begin(); it != g_resolved_views.end();) {
    if (it->second.texture == texture) {
      ctx.retire(std::move(it->second.view));
      it = g_resolved_views.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_resolved.begin(); it != g_resolved.end();)
    it = it->second.resource == texture ? g_resolved.erase(it) : std::next(it);
}

plume::RenderComponentMapping ComponentMapping(uint32_t swizzle, uint32_t components, bool swap_rb) {
  plume::RenderSwizzle out[4];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    if (swap_rb && (c == 0 || c == 2)) c ^= 2;
    if (c <= 3) c = std::min(c, components - 1);
    else if (c > 5) c = 4;
    static const bool alpha1 = std::getenv("SVR2011_NATIVE_ALPHA1") != nullptr;  // debug
    if (alpha1 && i == 3) c = 5;
    out[i] = Swizzle(c);
  }
  return plume::RenderComponentMapping(out[0], out[1], out[2], out[3]);
}

uint64_t ResidentBytes() { return g_resident_bytes; }
uint32_t ResidentCount() {
  uint32_t n = 0;
  for (const auto& [key, e] : g_textures) n += e.resource != nullptr;
  return n;
}

Stats TakePerf() {
  Stats s = g_stats;
  s.textures = 0;  // (with a resource: a texture quality change drops them)
  for (const auto& [key, e] : g_textures) s.textures += e.resource != nullptr;
  s.resident_bytes = g_resident_bytes;
  s.reduced = g_reduced;
  g_stats.hash_bytes = g_stats.upload_bytes = 0;
  g_stats.hashes = g_stats.uploads_total = 0;
  g_stats.hash_ms = g_stats.upload_ms = g_stats.convert_ms = 0;
  return s;
}

Stats FrameStats() {
  Stats s = g_stats;
  s.textures = uint32_t(g_textures.size());
  g_stats.uploads = g_stats.unsupported = 0;
  return s;
}

}  // namespace svr2011::native::textures
