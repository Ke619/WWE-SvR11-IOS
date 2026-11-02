// WWE SmackDown vs. Raw 2011 - texture dumps and texture packs (see
// texture_packs.h).
#include "native/texture_packs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/thread.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_STRING(native_texture_packs, "", "GPU",
                      "Texture packs in use: folders in the game folder's \"Texture Packs\", separated by ; "
                      "(the first that has a texture wins). The launchers' Texture packs list sets it.");
REXCVAR_DEFINE_BOOL(native_dump_textures, false, "GPU",
                    "For texture pack makers: every texture shown is written once to \"Texture Dumps\" as a "
                    "PNG named after the game's own texture (and its content hash, which packs match on)");

namespace svr2011::native::texture_packs {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// state

struct DumpItem {
  uint64_t hash;
  uint32_t width, height;
  std::vector<uint8_t> rgba;
  std::string format;
};

std::once_flag g_start_once;
std::atomic<bool> g_active{false}, g_dumping{false};
fs::path g_game;

std::mutex g_mutex;
std::condition_variable g_load_cv, g_dump_cv;
// packs
std::unordered_map<uint64_t, fs::path> g_files;  // hash -> the first enabled pack's file
std::unordered_map<uint64_t, std::shared_ptr<const Replacement>> g_loaded;
std::unordered_set<uint64_t> g_queued, g_failed;
std::deque<uint64_t> g_load_queue;
// dumps
std::unordered_set<uint64_t> g_dumped;
std::deque<DumpItem> g_dump_queue;
size_t g_dump_queue_bytes = 0;
uint32_t g_dump_dropped = 0;
// names (owner, name) by hash, once the index is ready
std::unordered_map<uint64_t, std::pair<std::string, std::string>> g_names;
bool g_names_ready = false;
std::vector<std::pair<uint64_t, fs::path>> g_unnamed;  // written before the names were ready

constexpr size_t kDumpQueueLimit = size_t(384) << 20;  // RGBA bytes waiting to be written
constexpr uint32_t kMaxSide = 8192;

void LowPriority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  if (n <= 0) return false;
  f.seekg(0, std::ios::beg);
  out.resize(size_t(n));
  return bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

bool WriteAll(const fs::path& p, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  return f && f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

std::string Hex16(uint64_t h) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

// A file name's hash: <anything>_<16 hex digits> or just the 16 digits.
bool HashOfStem(const std::string& stem, uint64_t* hash) {
  if (stem.size() < 16) return false;
  const size_t at = stem.size() - 16;
  if (at > 0 && stem[at - 1] != '_') return false;
  uint64_t h = 0;
  for (size_t i = at; i < stem.size(); ++i) {
    const char c = char(std::tolower(static_cast<unsigned char>(stem[i])));
    if (c >= '0' && c <= '9') h = h << 4 | uint64_t(c - '0');
    else if (c >= 'a' && c <= 'f') h = h << 4 | uint64_t(c - 'a' + 10);
    else return false;
  }
  *hash = h;
  return true;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Safe(const std::string& s) {
  std::string o;
  for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
  return o.empty() ? "x" : o;
}

// ---------------------------------------------------------------------------
// PNG writer: zlib with fixed-Huffman deflate (LZ77, hash chains), Sub filter

struct Bits {
  std::vector<uint8_t>& out;
  uint32_t acc = 0, n = 0;
  void Put(uint32_t bits, uint32_t count) {
    acc |= bits << n;
    n += count;
    while (n >= 8) {
      out.push_back(uint8_t(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void Flush() {
    if (n) out.push_back(uint8_t(acc));
    acc = n = 0;
  }
};

uint32_t Reverse(uint32_t code, uint32_t len) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < len; ++i, code >>= 1) r = r << 1 | (code & 1);
  return r;
}

void PutSymbol(Bits& w, uint32_t v) {  // literal / length symbol 0..287, fixed codes
  if (v <= 143) w.Put(Reverse(0x30 + v, 8), 8);
  else if (v <= 255) w.Put(Reverse(0x190 + v - 144, 9), 9);
  else if (v <= 279) w.Put(Reverse(v - 256, 7), 7);
  else w.Put(Reverse(0xC0 + v - 280, 8), 8);
}

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutMatch(Bits& w, uint32_t len, uint32_t dist) {
  int i = 28;
  while (kLenBase[i] > len) --i;
  PutSymbol(w, 257 + i);
  w.Put(len - kLenBase[i], kLenExtra[i]);
  int d = 29;
  while (kDistBase[d] > dist) --d;
  w.Put(Reverse(uint32_t(d), 5), 5);
  w.Put(dist - kDistBase[d], kDistExtra[d]);
}

void Deflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
  Bits w{out};
  w.Put(1, 1);  // final block
  w.Put(1, 2);  // fixed Huffman
  constexpr uint32_t kWindow = 32768, kHashBits = 15, kChain = 48;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(kWindow, -1);
  auto hash3 = [&](size_t i) {
    return ((uint32_t(data[i]) << 16 | uint32_t(data[i + 1]) << 8 | data[i + 2]) * 2654435761u) >> (32 - kHashBits);
  };
  auto insert = [&](size_t i) {
    if (i + 2 >= n) return;
    const uint32_t h = hash3(i);
    prev[i & (kWindow - 1)] = head[h];
    head[h] = int32_t(i);
  };
  size_t i = 0;
  while (i < n) {
    uint32_t best = 0, best_dist = 0;
    if (i + 2 < n) {
      int32_t cand = head[hash3(i)];
      const size_t max_len = std::min<size_t>(258, n - i);
      for (uint32_t chain = 0; cand >= 0 && chain < kChain; ++chain) {
        const size_t c = size_t(cand);
        if (c >= i || i - c > kWindow - 1) break;
        if (data[c + best] == data[i + best]) {
          size_t l = 0;
          while (l < max_len && data[c + l] == data[i + l]) ++l;
          if (l > best) {
            best = uint32_t(l);
            best_dist = uint32_t(i - c);
            if (l == max_len) break;
          }
        }
        const int32_t next = prev[c & (kWindow - 1)];
        if (next >= cand) break;  // (a stale slot from beyond the window)
        cand = next;
      }
    }
    if (best >= 3) {
      PutMatch(w, best, best_dist);
      for (size_t k = 0; k < best; ++k) insert(i + k);
      i += best;
    } else {
      PutSymbol(w, data[i]);
      insert(i);
      ++i;
    }
  }
  PutSymbol(w, 256);
  w.Flush();
}

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  });
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBe32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 24));
  o.push_back(uint8_t(v >> 16));
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBe32(png, uint32_t(data.size()));
  const size_t at = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  PutBe32(png, Crc32(&png[at], png.size() - at));
}

// ---------------------------------------------------------------------------
// replacement files

std::vector<uint8_t> Downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh) {
  std::vector<uint8_t> dst(size_t(nw) * nh * 4);
  for (uint32_t y = 0; y < nh; ++y) {
    for (uint32_t x = 0; x < nw; ++x) {
      const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
      const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
      for (int c = 0; c < 4; ++c) {
        const uint32_t s = src[(size_t(y0) * w + x0) * 4 + c] + src[(size_t(y0) * w + x1) * 4 + c] +
                           src[(size_t(y1) * w + x0) * 4 + c] + src[(size_t(y1) * w + x1) * 4 + c];
        dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
      }
    }
  }
  return dst;
}

bool DecodePng(const std::vector<uint8_t>& file, Replacement& r) {
  int w = 0, h = 0, n = 0;
  stbi_uc* px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
  if (!px) return false;
  if (w <= 0 || h <= 0 || uint32_t(w) > kMaxSide || uint32_t(h) > kMaxSide) {
    stbi_image_free(px);
    return false;
  }
  r.width = uint32_t(w);
  r.height = uint32_t(h);
  r.format = Format::kRGBA8;
  r.levels.emplace_back(px, px + size_t(w) * h * 4);
  stbi_image_free(px);
  uint32_t lw = r.width, lh = r.height;
  while (lw > 1 || lh > 1) {
    const uint32_t nw = std::max(lw / 2, 1u), nh = std::max(lh / 2, 1u);
    r.levels.push_back(Downsample(r.levels.back(), lw, lh, nw, nh));
    lw = nw;
    lh = nh;
  }
  return true;
}

uint32_t MaskShift(uint32_t mask) {
  uint32_t s = 0;
  while (mask && !(mask & 1)) mask >>= 1, ++s;
  return s;
}

bool DecodeDds(const std::vector<uint8_t>& d, Replacement& r) {
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4)) return false;
  const uint32_t h = Le32(&d[12]), w = Le32(&d[16]);
  if (!w || !h || w > kMaxSide || h > kMaxSide) return false;
  const uint32_t mips = (Le32(&d[8]) & 0x20000) ? std::max(1u, Le32(&d[28])) : 1u;
  const uint32_t pf_flags = Le32(&d[80]);
  size_t at = 128;
  r.width = w;
  r.height = h;
  if (pf_flags & 4) {  // FourCC
    uint32_t bs;
    if (!std::memcmp(&d[84], "DXT1", 4)) r.format = Format::kBC1, bs = 8;
    else if (!std::memcmp(&d[84], "DXT3", 4)) r.format = Format::kBC2, bs = 16;
    else if (!std::memcmp(&d[84], "DXT5", 4)) r.format = Format::kBC3, bs = 16;
    else return false;
    for (uint32_t l = 0; l < mips; ++l) {
      const uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
      const size_t n = size_t(std::max((lw + 3) / 4, 1u)) * std::max((lh + 3) / 4, 1u) * bs;
      if (at + n > d.size()) break;
      r.levels.emplace_back(d.begin() + at, d.begin() + at + n);
      at += n;
    }
  } else if (Le32(&d[88]) == 32) {  // uncompressed 32-bit, by its channel masks
    const uint32_t masks[4] = {Le32(&d[92]), Le32(&d[96]), Le32(&d[100]), (pf_flags & 1) ? Le32(&d[104]) : 0};
    r.format = Format::kRGBA8;
    for (uint32_t l = 0; l < mips; ++l) {
      const uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
      const size_t n = size_t(lw) * lh * 4;
      if (at + n > d.size()) break;
      std::vector<uint8_t> px(n);
      for (size_t i = 0; i < size_t(lw) * lh; ++i) {
        const uint32_t v = Le32(&d[at + i * 4]);
        for (int c = 0; c < 4; ++c)
          px[i * 4 + c] = masks[c] ? uint8_t((v & masks[c]) >> MaskShift(masks[c])) : 255;
      }
      r.levels.push_back(std::move(px));
      at += n;
    }
  } else {
    return false;
  }
  return !r.levels.empty();
}

void LoaderThread() {
  rex::thread::set_current_thread_name("texture pack loader");
  LowPriority();
  for (;;) {
    uint64_t hash;
    fs::path file;
    {
      std::unique_lock lock(g_mutex);
      g_load_cv.wait(lock, [] { return !g_load_queue.empty(); });
      hash = g_load_queue.front();
      g_load_queue.pop_front();
      file = g_files[hash];
    }
    auto r = std::make_shared<Replacement>();
    std::vector<uint8_t> data;
    const std::string ext = Lower(file.extension().string());
    const bool ok = ReadAll(file, data) && (ext == ".dds" ? DecodeDds(data, *r) : DecodePng(data, *r));
    std::lock_guard lock(g_mutex);
    g_queued.erase(hash);
    if (ok) {
      g_loaded[hash] = std::move(r);
    } else {
      g_failed.insert(hash);
      REXLOG_WARN("texture packs: {} could not be read (PNG, or DDS in DXT1/3/5 or 32-bit)", file.string());
    }
  }
}

// ---------------------------------------------------------------------------
// the game files' texture names

using Names = std::unordered_map<uint64_t, std::pair<std::string, std::string>>;

// The top level of a DDS as the GPU reads it (the same as ContentHash).
uint64_t DdsBaseHash(const uint8_t* d, size_t n) {
  if (n < 128 || std::memcmp(d, "DDS ", 4)) return 0;
  const uint32_t h = Le32(d + 12), w = Le32(d + 16), flags = Le32(d + 80);
  size_t bytes;
  if (flags & 4) {
    const uint32_t bs = !std::memcmp(d + 84, "DXT1", 4) ? 8
                        : (!std::memcmp(d + 84, "DXT3", 4) || !std::memcmp(d + 84, "DXT5", 4)) ? 16
                                                                                               : 0;
    if (!bs) return 0;
    bytes = size_t(std::max((w + 3) / 4, 1u)) * std::max((h + 3) / 4, 1u) * bs;
  } else {
    const uint32_t bits = Le32(d + 88);
    if (bits != 32 && bits != 16 && bits != 8) return 0;
    bytes = size_t(w) * h * (bits / 8);
  }
  if (!bytes || 128 + bytes > n) return 0;
  return XXH3_64bits(d + 128, bytes);
}

void WalkPach(const svrfmt::Bytes& pach, const std::string& owner, const std::string& id_path, int depth,
              Names& out) {
  std::vector<svrfmt::PachEntry> entries;
  if (!svrfmt::PachRead(pach, entries)) return;
  for (const svrfmt::PachEntry& e : entries) {
    char id[16];
    std::snprintf(id, sizeof id, "%X", e.id);
    const std::string path = id_path.empty() ? id : id_path + "-" + id;
    const svrfmt::Bytes raw = svrfmt::Unpack(e.data);
    if (svrfmt::IsTextureBundle(raw)) {
      std::vector<svrfmt::BundleTexture> texs;
      if (!svrfmt::BundleRead(raw, texs)) continue;
      for (const svrfmt::BundleTexture& t : texs)
        if (const uint64_t h = DdsBaseHash(t.data.data(), t.data.size())) out.emplace(h, std::make_pair(owner, Safe(t.name)));
    } else if (svrfmt::IsPach(raw) && depth < 3) {
      WalkPach(raw, owner, path, depth + 1, out);
    } else if (raw.size() >= 128 && !std::memcmp(raw.data(), "DDS ", 4)) {
      if (const uint64_t h = DdsBaseHash(raw.data(), raw.size())) out.emplace(h, std::make_pair(owner, "e" + path));
    }
  }
}

void IndexPac(const fs::path& file, Names& out) {
  svrfmt::Bytes d;
  if (!ReadAll(file, d) || d.size() < 0x4000) return;
  const std::string stem = Safe(file.stem().string());
  if (!std::memcmp(d.data(), "EPAC", 4)) {
    svrfmt::Epac epac;
    if (!svrfmt::EpacRead(d, epac)) return;
    for (const svrfmt::EpacGroup& g : epac.groups)
      for (const svrfmt::EpacEntry& e : g.entries)
        if (svrfmt::IsPach(e.data)) WalkPach(e.data, stem, "", 0, out);
  } else if (!std::memcmp(d.data(), "EPK8", 4)) {
    // (characters: groups of 16-byte entries {name[8], sector, size / 256};
    // the count is in dwords; EMD entries "%06d%02d" - id, attire * 10 + kind)
    for (size_t p = 0x800; p + 12 <= 0x4000;) {
      if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
      const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
      p += 12;
      for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16) {
        const size_t off = 0x4000 + size_t(Le32(&d[p + 8])) * 0x800, size = size_t(Le32(&d[p + 12])) * 0x100;
        if (off + 8 > d.size()) continue;
        const svrfmt::Bytes blob(d.begin() + off, d.begin() + std::min(d.size(), off + size));
        if (!svrfmt::IsPach(blob)) continue;
        std::string name(reinterpret_cast<const char*>(&d[p]), 8);
        name = name.substr(0, name.find('\0'));
        const bool attire = name.size() == 8 && std::all_of(name.begin(), name.end(), ::isdigit);
        WalkPach(blob, attire ? stem + "_a" + name.substr(6, 1) : stem + "_" + Safe(name), "", 0, out);
      }
    }
  }
}

std::vector<fs::path> PacFiles() {
  std::vector<fs::path> files;
  for (const char* dir : {"pac", "DLC"}) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(g_game / dir, fs::directory_options::follow_directory_symlink, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".pac") files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void NamesThread() {
  rex::thread::set_current_thread_name("texture pack names");
  LowPriority();
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<fs::path> files = PacFiles();
  uint64_t total = 0;
  for (const fs::path& f : files) {
    std::error_code ec;
    total += fs::file_size(f, ec);
  }
  const std::string sig = "texture names v1 " + std::to_string(files.size()) + " " + std::to_string(total);
  const fs::path cache = g_game / "UserData" / "cache" / "texture_names.txt";
  Names names;
  bool cached = false;
  {
    std::ifstream in(cache);
    std::string line;
    if (in && std::getline(in, line) && line == sig) {
      while (std::getline(in, line)) {
        uint64_t h;
        const size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == 16 && b != std::string::npos && HashOfStem(line.substr(0, 16), &h))
          names.emplace(h, std::make_pair(line.substr(a + 1, b - a - 1), line.substr(b + 1)));
      }
      cached = !names.empty();
    }
  }
  if (!cached) {
    for (const fs::path& f : files) IndexPac(f, names);
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    std::ofstream out(cache, std::ios::trunc);
    out << sig << '\n';
    for (const auto& [h, n] : names) out << Hex16(h) << '\t' << n.first << '\t' << n.second << '\n';
  }
  REXLOG_INFO("texture dumps: {} texture names from {} pac files ({}, {:.1f} s)", names.size(), files.size(),
              cached ? "cached" : "indexed",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  std::lock_guard lock(g_mutex);
  g_names = std::move(names);
  g_names_ready = true;
  g_dump_cv.notify_all();
}

// ---------------------------------------------------------------------------
// dumps

fs::path DumpPath(uint64_t hash, uint32_t w, uint32_t h, const std::string& format, bool* named) {
  const fs::path root = g_game / "Texture Dumps";
  auto it = g_names.find(hash);
  *named = it != g_names.end();
  if (*named)
    return root / it->second.first / (it->second.first + "_" + it->second.second + "_" + Hex16(hash) + ".png");
  return root / "other" / ("tex_" + std::to_string(w) + "x" + std::to_string(h) + "_" + format + "_" + Hex16(hash) + ".png");
}

void DumpThread() {
  rex::thread::set_current_thread_name("texture dump");
  LowPriority();
  uint32_t written = 0;
  for (;;) {
    DumpItem item;
    bool rename = false;
    {
      std::unique_lock lock(g_mutex);
      g_dump_cv.wait(lock, [] { return !g_dump_queue.empty() || (g_names_ready && !g_unnamed.empty()); });
      if (g_names_ready && !g_unnamed.empty()) {
        rename = true;
      } else {
        item = std::move(g_dump_queue.front());
        g_dump_queue.pop_front();
        g_dump_queue_bytes -= item.rgba.size();
      }
    }
    if (rename) {  // (files written before the names were ready)
      std::vector<std::pair<uint64_t, fs::path>> list;
      {
        std::lock_guard lock(g_mutex);
        list.swap(g_unnamed);
      }
      uint32_t renamed = 0;
      for (const auto& [hash, from] : list) {
        bool named;
        fs::path to;
        {
          std::lock_guard lock(g_mutex);
          to = DumpPath(hash, 0, 0, "", &named);
        }
        if (!named) continue;
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        renamed += !ec;
      }
      if (renamed) REXLOG_INFO("texture dumps: {} files named after the game's textures", renamed);
      continue;
    }
    bool named;
    fs::path path;
    bool ready;
    {
      std::lock_guard lock(g_mutex);
      path = DumpPath(item.hash, item.width, item.height, item.format, &named);
      ready = g_names_ready;
    }
    if (WriteAll(path, EncodePng(item.rgba.data(), item.width, item.height))) {
      ++written;
      if (written == 1 || written % 100 == 0)
        REXLOG_INFO("texture dumps: {} written ({})", written, (g_game / "Texture Dumps").string());
      if (!ready && !named) {
        std::lock_guard lock(g_mutex);
        g_unnamed.emplace_back(item.hash, path);
      }
    }
  }
}

}  // namespace

std::vector<uint8_t> EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height) {
  // rows with filter 1 (Sub: each byte minus the pixel to its left)
  const size_t row = size_t(width) * 4;
  std::vector<uint8_t> raw((row + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* o = &raw[(row + 1) * y];
    const uint8_t* s = rgba + row * y;
    o[0] = 1;
    for (size_t i = 0; i < row; ++i) o[1 + i] = uint8_t(s[i] - (i >= 4 ? s[i - 4] : 0));
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  Deflate(raw.data(), raw.size(), z);
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  PutBe32(z, b << 16 | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, width);
  PutBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, adaptive filters, no interlace
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  return png;
}

void Start() {
  std::call_once(g_start_once, [] {
    g_game = rex::filesystem::GetExecutableFolder();
    // Packs: in the order given; the first that has a texture keeps it.
    std::vector<std::string> packs;
    {
      std::stringstream ss(REXCVAR_GET(native_texture_packs));
      std::string name;
      while (std::getline(ss, name, ';')) {
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (!name.empty()) packs.push_back(name);
      }
    }
    uint32_t used = 0;
    for (const std::string& pack : packs) {
      const fs::path dir = g_game / "Texture Packs" / fs::u8path(pack);
      std::error_code ec;
      uint32_t n = 0;
      for (fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink, ec), end;
           !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = Lower(it->path().extension().string());
        uint64_t h;
        if ((ext == ".png" || ext == ".dds") && HashOfStem(it->path().stem().string(), &h) &&
            g_files.emplace(h, it->path()).second)
          ++n;
      }
      if (ec) REXLOG_WARN("texture packs: \"{}\" is not in {}", pack, (g_game / "Texture Packs").string());
      REXLOG_INFO("texture packs: \"{}\": {} textures", pack, n);
      used += n > 0;
    }
    g_dumping = REXCVAR_GET(native_dump_textures);
    g_active = !g_files.empty() || g_dumping;
    if (!g_files.empty()) std::thread(LoaderThread).detach();
    if (g_dumping) {
      // (textures already in the dump folder aren't written again)
      std::error_code ec;
      for (fs::recursive_directory_iterator it(g_game / "Texture Dumps", ec), end; !ec && it != end; it.increment(ec)) {
        uint64_t h;
        if (HashOfStem(it->path().stem().string(), &h)) g_dumped.insert(h);
      }
      REXLOG_INFO("texture dumps: on ({} already dumped)", g_dumped.size());
      std::thread(NamesThread).detach();
      std::thread(DumpThread).detach();
    }
    if (!packs.empty()) REXLOG_INFO("texture packs: {} of {} enabled packs have textures", used, packs.size());
  });
}

bool Active() { return g_active.load(std::memory_order_relaxed); }
bool Dumping() { return g_dumping.load(std::memory_order_relaxed); }

Lookup Find(uint64_t hash, std::shared_ptr<const Replacement>* out) {
  std::lock_guard lock(g_mutex);
  if (g_files.find(hash) == g_files.end() || g_failed.count(hash)) return Lookup::kNone;
  if (auto it = g_loaded.find(hash); it != g_loaded.end()) {
    *out = std::move(it->second);
    g_loaded.erase(it);
    return Lookup::kReady;
  }
  if (g_queued.insert(hash).second) {
    g_load_queue.push_back(hash);
    g_load_cv.notify_one();
  }
  return Lookup::kPending;
}

bool WantDump(uint64_t hash) {
  if (!Dumping()) return false;
  std::lock_guard lock(g_mutex);
  return !g_dumped.count(hash);
}

void Dump(uint64_t hash, uint32_t width, uint32_t height, std::vector<uint8_t> rgba, const char* format) {
  std::lock_guard lock(g_mutex);
  if (!g_dumped.insert(hash).second) return;
  if (g_dump_queue_bytes + rgba.size() > kDumpQueueLimit) {  // (the writer is behind: shown again later, it's retried)
    g_dumped.erase(hash);
    if (g_dump_dropped++ % 100 == 0) REXLOG_WARN("texture dumps: the writer is behind; some textures wait for later");
    return;
  }
  g_dump_queue_bytes += rgba.size();
  g_dump_queue.push_back({hash, width, height, std::move(rgba), format});
  g_dump_cv.notify_one();
}

}  // namespace svr2011::native::texture_packs
