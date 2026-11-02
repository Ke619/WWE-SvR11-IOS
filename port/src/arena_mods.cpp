// Arena mods (arenas branch).
//
// How an arena file is opened: the game preloads its pac files through the
// XDK file cache (sub_826A7328 -> sub_8290A668 with the name table at
// 0x82DAC7C0) and opens an arena when it loads, as D:\PAC\BG\BGnn.PAC (not
// held open). That path resolves through symbolic links to
// \Device\Harddisk0\Partition1\PAC\BG\BGnn.PAC (the game folder). The
// file system resolves the directory (D:\PAC\BG) and then looks the file up
// in it, so the redirect works on the directory: \PAC\BG is linked to an
// overlay folder (<game>/Mods/ArenaOverlay) that holds hard links to every
// original arena file (copies where hard links are not possible), and a
// redirected arena's entry there is a link to the mod's file instead. Arena
// files are only open while they load, so entries can be swapped between
// matches. The originals are never written.
// (Changing the name table or hooking the game's CreateFile wrappers does not
// reach this open; the XDK file cache library code does it.)
//
// Arena select: the 5 x 4 grid is a generic grid widget updated by
// sub_823D4FC0(widget): +0 vtable, +28 row, +32 rows, +36 row wrap, +44
// column, +48 columns, +52 column wrap, +64 buttons, +88 moved (1 up, 2 down,
// 4 left, 8 right). The 20 banners are DXT5 256 x 128 textures from
// menu/MatchHD.pac (MENU/MASI), kept in guest physical memory (4 KB view)
// untiled, with 16-bit words byte-swapped.
#include "backgrounds_page.h"
#include "arena_lights.h"
#include "no_mods.h"
#include "weapon_mods.h"
#include "arena_mods.h"
#include "crowd_off.h"
#include "move_packs.h"
#include "ring_rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <atomic>
#include <cfloat>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/thread.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "native/native_renderer.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"
#include "../modmaker/svrfmt/jboy.h"

namespace {

constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
namespace fs = std::filesystem;

rex::memory::Memory* g_memory = nullptr;
rex::filesystem::VirtualFileSystem* g_fs = nullptr;
std::mutex g_mutex;
fs::path g_game, g_overlay;
bool g_overlay_ready = false;
std::string g_redirect[100];  // arena number -> relative file ("" = original)
int g_number_for[100];        // arena number -> the number a match there plays as (0: its own), see RedirectArena
// arena number -> the arena a match picked there plays as, + 1 (0: its own - stored + 1 so that a base of arena 0,
// SmackDown, isn't taken for none: a mod built on SmackDown played its tile's stock arena, 30 Oct report): a custom
// arena on another arena's tile plays as its base arena (FollowCursor), whose file is the custom one
int g_plays_as[100];
std::string g_default[100];   // arena number -> its file when no custom arena is on its tile (media mods: screens)
// The served file's STG entries whose size differs from the shipped file's (RedirectArena): the game reads an entry
// with the size its pac directory had when the pacs were mounted - the shipped file's (or plist360.arc's) - so a
// bigger stage was read cut short (its end left out: a crash 6 s into the match) and a smaller one read past its end.
// The file loader's read (model_mem.cpp) takes the served size instead (ServedStageUnits).
struct StgEntry {
  char name[5];
  uint32_t sector, units;  // (units: size / 0x100, as the pac table and the loader's request keep it)
};
std::mutex g_stg_mutex;
std::vector<StgEntry> g_served_stg[100];

// The STG group's entries of an EPAC's first 0x4000 bytes (with each entry's offset in `head`).
std::vector<std::pair<StgEntry, size_t>> StgEntries(const char* head, size_t n) {
  std::vector<std::pair<StgEntry, size_t>> out;
  if (n < 0x4000 || std::memcmp(head, "EPAC", 4)) return out;
  for (size_t p = 0x800; p + 12 <= n;) {
    if (!head[p] && !head[p + 1] && !head[p + 2] && !head[p + 3]) break;
    uint32_t words;
    std::memcpy(&words, head + p + 4, 4);  // (little-endian)
    const bool stg = !std::memcmp(head + p, "STG ", 4);
    const size_t count = (words & 0xFFFF) / 3;
    p += 12;
    for (size_t e = 0; e < count && p + 12 <= n; ++e, p += 12) {
      if (!stg) continue;
      StgEntry s{};
      std::memcpy(s.name, head + p, 4);
      std::memcpy(&s.sector, head + p + 4, 4);
      std::memcpy(&s.units, head + p + 8, 4);
      out.push_back({s, p});
    }
    if (stg) break;
  }
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Makes the file system see overlay/<name> as it is now: the host device
// indexes a folder created after startup lazily, and keeps a file's size
// from when it first saw it.
void Refresh(const std::string& name) {
  const std::string path = std::string(kGameDevice) + "\\Mods\\ArenaOverlay\\" + name;
  if (auto* e = g_fs->ResolvePath(path)) e->update();
}

std::map<std::string, fs::path> g_placed;  // overlay name -> the file asked for

// overlay/<name> := link to (or copy of) `source` - with the crowd off
// (crowd_off.h) its crowd-less copy once made.
bool Place(const fs::path& requested, const std::string& name) {
  g_placed[name] = requested;
  const fs::path source = svr2011::crowd::Serve(requested, g_game / "Mods" / "ArenaCrowdless", [requested, name] {
    std::lock_guard lock(g_mutex);
    if (g_placed[name] == requested) Place(requested, name);  // (now the copy)
  });
  std::error_code ec;
  const fs::path dst = g_overlay / name;
  // An overlay entry may be a hard link to an original: never write through
  // it. If it cannot be removed, give up instead of copying over it.
  fs::remove(dst, ec);
  if (fs::exists(dst, ec)) {
    REXLOG_WARN("[svr2011] arena mods: cannot replace {}", name);
    return false;
  }
  fs::create_hard_link(source, dst, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(source, dst, fs::copy_options::none, ec);  // (never over an entry: it could be a link)
    if (ec) {
      REXLOG_WARN("[svr2011] arena mods: cannot place {} ({})", name, ec.message());
      return false;
    }
  }
  Refresh(name);
  return true;
}

// The overlay mirrors pac/bg, then \PAC\BG is linked to it (once).
bool PrepareOverlay() {
  if (g_overlay_ready) return true;
  std::error_code ec;
  const fs::path bg = g_game / "pac" / "bg";
  fs::create_directories(g_overlay, ec);
  if (ec || !fs::is_directory(bg, ec)) return false;
  for (const auto& e : fs::directory_iterator(bg, ec)) {
    if (!e.is_regular_file()) continue;
    const std::string name = Upper(e.path().filename().string());
    const fs::path dst = g_overlay / name;
    // keep an existing link to the same original (same size and time)
    std::error_code e1, e2;
    if (fs::exists(dst, e1) && fs::file_size(dst, e1) == e.file_size() &&
        fs::last_write_time(dst, e2) == e.last_write_time()) {
      Refresh(name);
      continue;
    }
    if (!Place(e.path(), name)) return false;
  }
  g_fs->RegisterSymbolicLink(std::string(kGameDevice) + "\\PAC\\BG",
                             std::string(kGameDevice) + "\\Mods\\ArenaOverlay");
  g_overlay_ready = true;
  REXLOG_INFO("[svr2011] arena mods: overlay ready ({})", g_overlay.string());
  return true;
}

// Guest memory region that is committed (and readable).
bool Committed(uint32_t at, rex::memory::HeapAllocationInfo& info) {
  auto* heap = g_memory->LookupHeap(at);
  info = {};
  return heap && heap->QueryRegionInfo(at & ~0xFFFu, &info) && info.region_size &&
         (info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead);
}

// Calls fn(start, end) for each committed region in [lo, hi).
template <class F>
void ForEachRegion(uint64_t lo, uint64_t hi, F fn) {
  for (uint64_t at = lo; at < hi;) {
    rex::memory::HeapAllocationInfo info;
    if (!Committed(uint32_t(at), info)) {
      at = (at & ~0xFFFull) + (info.region_size ? info.region_size : 0x1000);
      continue;
    }
    const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
    fn(at, end);
    at = end;
  }
}

// Test aid: SVR2011_TEST_BPE_LOG=1 logs every BPE decode (the game's
// sub_826AEF10(src, dst)): caller, source, destination, unpacked size.
bool g_bpe_log = false;

// Test aid: SVR2011_TEST_SCAN=1 - each time <game>/scan.txt changes, its text
// is a request: "<decimal>" narrows a cheat-search for that value (big-endian
// u32 and byte) kept across requests; "hex:<bytes>" finds a byte pattern.
void StartScan() {
  std::thread([] {
    rex::thread::set_current_thread_name("arena scan");
    const fs::path trigger = g_game / "scan.txt";
    std::string last_text;
    std::vector<uint32_t> cand32, cand8;
    bool first = true;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::error_code ec;
      if (!fs::exists(trigger, ec)) continue;
      std::string text;
      if (FILE* f = std::fopen(trigger.string().c_str(), "rb")) {
        char b[256] = {};
        std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        text = b;
      }
      if (text.empty() || text == last_text) continue;
      last_text = text;
      uint8_t* b = g_memory->virtual_membase();
      if (text.rfind("hex:", 0) == 0) {
        std::vector<uint8_t> pat;
        for (size_t i = 4; i + 1 < text.size(); i += 2)
          pat.push_back(uint8_t(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
        std::string hits;
        int n = 0;
        if (!pat.empty())
          ForEachRegion(0x40000000, 0xFFFF0000, [&](uint64_t at, uint64_t end) {
            for (uint64_t a = at; a + pat.size() <= end; ++a)
              if (b[a] == pat[0] && !std::memcmp(b + a, pat.data(), pat.size()) && ++n <= 40)
                hits += fmt::format(" {:08X}", uint32_t(a));
          });
        REXLOG_INFO("[svr2011] find {}: {} hits [{}]", text, n, hits);
        continue;
      }
      const uint32_t want = uint32_t(std::strtoul(text.c_str(), nullptr, 10));
      auto be32 = [&](uint32_t a) { return uint32_t(b[a]) << 24 | b[a + 1] << 16 | b[a + 2] << 8 | b[a + 3]; };
      auto readable = [&](uint32_t a) {
        rex::memory::HeapAllocationInfo info;
        return Committed(a, info);
      };
      if (first) {
        ForEachRegion(0x40000000, 0xFFFF0000, [&](uint64_t at, uint64_t end) {
          for (uint64_t a = at; a + 4 <= end; a += 4)
            if (be32(uint32_t(a)) == want) cand32.push_back(uint32_t(a));
          for (uint64_t a = at; a < end; ++a)
            if (b[a] == want && cand8.size() < 20000000) cand8.push_back(uint32_t(a));
        });
        first = false;
      } else {
        std::vector<uint32_t> k32, k8;
        for (uint32_t a : cand32)
          if (readable(a) && be32(a) == want) k32.push_back(a);
        for (uint32_t a : cand8)
          if (readable(a) && b[a] == want) k8.push_back(a);
        cand32.swap(k32);
        cand8.swap(k8);
      }
      std::string s32, s8;
      for (size_t i = 0; i < cand32.size() && i < 40; ++i) s32 += fmt::format(" {:08X}", cand32[i]);
      for (size_t i = 0; i < cand8.size() && i < 40; ++i) s8 += fmt::format(" {:08X}", cand8[i]);
      REXLOG_INFO("[svr2011] scan {}: u32 {} [{}] u8 {} [{}]", want, cand32.size(), s32, cand8.size(), s8);
    }
  }).detach();
}

// ---- arena select pages
//
// Page 1 is the game's own 20 arenas. Pages 2+ show the installed custom
// arenas in the same tiles (Mods/Arenas/<id>/: arena.pac, banner.dds = DXT5
// 256 x 128, manifest.txt name=...; a "disabled" file turns one off).
// Moving past the right edge goes to the next page, past the left edge to
// the previous one (as the Paint Tool grid); LB / RB and LT / RT change the
// page too (keyboard: 1 / 3 and Q / E, the controller keys' defaults).
// A custom arena borrows its base arena as host: while its tile is the one
// under the cursor, the host's file is the custom arena's (RedirectArena) and
// a match on the tile plays as the host (PointAt), so the match loads it.

struct Banner {
  std::string name;    // "arena_SD"
  svrfmt::Bytes data;  // DXT5 blocks as the game keeps them (16-bit swapped)
  // where the game keeps it while the screen is open: every place holding the
  // whole banner (a 64-byte match alone can take a wrong place - a 2.0.3
  // player's pages showed banners on the wrong tiles, half drawn)
  std::vector<uint32_t> guest;
};
std::vector<Banner> g_banners;  // the originals (the arena select's and the online screen's)

struct CustomArena {
  std::string id, name, file;  // file: relative to the game folder
  std::string manifest;        // its text (ring.* keys: ring_rules.h)
  std::string base;            // the arena it was made from (manifest base=arena_SS)
  int base_arena = -1;         // its number (-1: no base - it plays as the tile's arena)
  bool tron_live = false;      // manifest titantron=live: its video screens show the match camera (TitantronFile)
  svrfmt::Bytes banner;        // DXT5 blocks, 16-bit swapped (empty = none)
  // its VS screen pictures (vs/<name>.dds): name -> DXT5 blocks, swapped
  std::vector<std::pair<std::string, svrfmt::Bytes>> vs;
  // its loading screen (load.dds, 1024 x 512 DXT5): blocks, swapped
  svrfmt::Bytes load;
  // its own VS screen (vs/theme.pac: VSTL / VSTI / VSTF, tools/vs_port.py theme): the file, and its group in the VS
  // styles' overlay pac once vs_styles.cpp merged it ("VA00" ...; empty: the game's / the VS style's theme)
  std::string theme_pac, theme_group;
  svrfmt::Image card;  // the entrance title card's logo (card.dds / card=; empty: the base arena's card)
};
std::vector<CustomArena> g_customs;

uint32_t g_select_widget = 0;
std::atomic<int64_t> g_select_seen{0};  // when the arena grid was last updated (ms)
std::atomic<int> g_cursor_custom{-1};    // custom arena under the cursor (label)
int g_locate_tries = 0;      // banners looked for again on the online screen
int64_t g_located_at = 0;
int g_page = 0;              // 0 = the game's arenas
// each player's page buttons as last read (ArenaSelectPad): LB 0x100, RB
// 0x200, LT 0x400, RT 0x800 (the triggers pressed past a quarter); A 0x1000
// and B 0x2000 (a pick / a cancel)
std::atomic<uint16_t> g_shoulders[8];
int g_redirected_host = -1;  // host arena currently pointed at a custom one

constexpr size_t kBannerBlocks = 64 * 32;  // 256 x 128 DXT5, 16 bytes a block
// The arena select grid widget: 5 columns, 5 rows (the 5th row is the
// DOWNLOADED ARENAS button).
constexpr uint32_t kSelectVtable = 0x8201D200;
// Tiles, row by row: banner name and arena number (the table at 0x8201D030
// pairs numbers with names; the order is the screen's).
struct Tile {
  const char* banner;
  int arena;
  const char* online;  // its picture on the online screen (menu/menuHD.pac MENU/OLKI)
};
constexpr Tile kTiles[20] = {
    {"arena_SD", 0, "PPV_sd"},       {"arena_RAW", 1, "PPV_raw"},   {"arena_WM26", 11, "PPV_WM26"},
    {"arena_slam", 4, "PPV_Slam"},   {"arena_SS", 17, "PPV_super"}, {"arena_HinC", 6, "PPV_HinC"},
    {"arena_Judg", 13, "PPV_JD"},    {"arena_BASH", 15, "PPV_BASH"}, {"arena_NoC", 14, "PPV_NoC"},
    {"arena_BP", 5, "PPV_BP"},       {"arena_RR", 9, "PPV_RR"},     {"arena_BR", 18, "PPV_BR"},
    {"arena_Series", 7, "PPV_SS"},   {"arena_TLC", 8, "PPV_TLC"},   {"arena_ttt", 3, "PPV_ttt"},
    {"arena_EC", 10, "PPV_EC"},      {"arena_back", 12, "PPV_back"}, {"arena_EXT", 16, "PPV_EXT"},
    {"arena_ECW", 2, "PPV_ECW"},     {"arena_drui", 19, "PPV_druid"},
};
// The online CREATE SESSION's arena select is a screen of its own (made by
// sub_82521D18, 1184 bytes; its base screen at +28 - vtable 0x82029020 - is
// updated through the same grid update, sub_823D4FC0(screen + 28), but is no
// 5 x 4 grid): its tiles come from a table (*(screen + 1176) +4: 8-byte
// entries, the arena number first; +24 the count) - the same banners in
// another order - and the cursor is the tile sub_82521EF8(screen, tile) sets
// (it makes that tile's arena the screen's, +1168). Its banners are not the
// arena select's: PPV_* in menu/menuHD.pac (SD: menuSD.pac) MENU/OLKI, the
// same 256 x 128 DXT5 (Survivor Series is PPV_SS there, Superstars
// PPV_super - the other way round from arena_SS / arena_Series).
constexpr uint32_t kOnlineSelectVtable = 0x82029020;
Tile g_online_tiles[20];
const Tile* g_tiles = kTiles;  // the open screen's order

// Tile k's picture on the open screen.
const char* PictureOf(int k) { return g_tiles == g_online_tiles ? g_tiles[k].online : g_tiles[k].banner; }
std::atomic<int> g_online_tile{0};  // (the online screen's cursor: sub_82521EF8)

// The online screen's order, from its table (false: not the 20 arenas).
bool ReadOnlineTiles(const uint8_t* base, uint32_t screen) {
  auto rd = [base](uint32_t a) {
    const uint8_t* q = base + a;
    return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  };
  const uint32_t desc = rd(screen + 1176);
  if (!desc || rd(desc + 24) < 20 || !rd(desc + 4)) return false;
  const uint32_t entries = rd(desc + 4);
  for (int k = 0; k < 20; ++k) {
    const int arena = int(rd(entries + 8 * k));
    const Tile* t = nullptr;
    for (const Tile& x : kTiles)
      if (x.arena == arena) t = &x;
    if (!t) return false;
    g_online_tiles[k] = *t;
  }
  return true;
}

// DDS block data -> the layout the game keeps (16-bit words byte-swapped)
svrfmt::Bytes Swap16(const uint8_t* p, size_t n) {
  svrfmt::Bytes o(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(o[i], o[i + 1]);
  return o;
}

// The top level of a 256 x 128 DXT5 DDS, swapped (empty if it is not that).
svrfmt::Bytes BannerBlocks(const svrfmt::Bytes& dds) {
  svrfmt::DdsInfo info;
  if (!svrfmt::DdsInfoOf(dds, info) || info.w != 256 || info.h != 128 || info.format != svrfmt::DxtFormat::kDxt5 ||
      dds.size() < 128 + kBannerBlocks * 16)
    return {};
  return Swap16(dds.data() + 128, kBannerBlocks * 16);
}

void LoadBanners() {
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "MatchHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != "MASI") continue;
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        const svrfmt::Bytes raw = svrfmt::Unpack(pe.data);
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(raw, texs)) continue;
        for (auto& t : texs)
          if (t.name.rfind("arena_", 0) == 0)
            if (svrfmt::Bytes blocks = BannerBlocks(t.data); !blocks.empty())
              g_banners.push_back({t.name, std::move(blocks), {}});
      }
    }
  // The online screen's: every version of each (the groups and the HD / SD
  // files hold differing copies; whichever the game loaded is found).
  for (const char* file : {"menuHD.pac", "menuSD.pac"}) {
    svrfmt::Bytes md;
    svrfmt::Epac me;
    if (!svrfmt::ReadFile((g_game / "pac" / "menu" / file).string(), md) || !svrfmt::EpacRead(md, me)) continue;
    for (const auto& g : me.groups)
      for (const auto& en : g.entries) {
        if (en.name != "OLKI") continue;
        std::vector<svrfmt::PachEntry> ents;
        if (!svrfmt::PachRead(en.data, ents)) continue;
        for (const auto& pe : ents) {
          std::vector<svrfmt::BundleTexture> texs;
          if (!svrfmt::BundleRead(svrfmt::Unpack(pe.data), texs)) continue;
          for (auto& t : texs) {
            if (std::none_of(std::begin(kTiles), std::end(kTiles), [&](const Tile& x) { return t.name == x.online; }))
              continue;
            svrfmt::Bytes blocks = BannerBlocks(t.data);
            if (blocks.empty() || std::any_of(g_banners.begin(), g_banners.end(),
                                              [&](const Banner& b) { return b.name == t.name && b.data == blocks; }))
              continue;
            g_banners.push_back({t.name, std::move(blocks), {}});
          }
        }
      }
  }
}

// Custom arenas on the pages: in install order (g_customs: Mods/Arenas/order.txt), left to right and row by row,
// no gaps - a page more only when one is full. Whatever tile one is on, a match on it plays as the arena it was
// made from (manifest base=: its memory room, VS screen style, the game's per-arena data) - FollowCursor points
// that arena's file at it and the match at that arena (g_plays_as). (Until 2.1.3 each sat on its base's tile:
// page 2's SmackDown tile empty when no mod was made from SmackDown, RAW's filled.)
std::vector<std::array<int, 20>> g_layout;  // pages 2.. : tile -> index in g_customs (-1)

int ArenaOfBanner(const std::string& banner) {
  for (const Tile& t : kTiles)
    if (banner == t.banner) return t.arena;
  return -1;
}

void LayOutPages() {
  g_layout.clear();
  for (size_t i = 0; i < g_customs.size(); ++i) {
    if (i % 20 == 0) g_layout.emplace_back().fill(-1);
    g_layout.back()[i % 20] = int(i);
  }
}

// The install order: Mods/Arenas/order.txt, a folder a line (the game's own file - written when a folder comes or
// goes). Folders not in it yet (installed since) go last, by name among themselves; folders gone leave it, so the
// rest close up. Switched-off ones keep their line (and their place when switched on again).
std::vector<fs::path> InInstallOrder(const fs::path& dir, std::vector<fs::path> folders) {
  std::vector<std::string> order, kept;
  if (std::ifstream in(dir / "order.txt"); in)
    for (std::string l; std::getline(in, l);) {
      while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
      if (!l.empty() && std::find(order.begin(), order.end(), l) == order.end()) order.push_back(l);
    }
  std::error_code ec;
  for (const auto& l : order)  // (folder names as written: the ANSI code page, as the mods' ids)
    if (fs::exists(dir / fs::path(l) / "arena.pac", ec)) kept.push_back(l);
  std::sort(folders.begin(), folders.end());
  for (const auto& f : folders)
    if (std::find(kept.begin(), kept.end(), f.filename().string()) == kept.end()) kept.push_back(f.filename().string());
  if (kept != order) {
    std::ofstream out(dir / "order.txt", std::ios::binary | std::ios::trunc);
    for (const auto& l : kept) out << l << '\n';
    REXLOG_INFO("[svr2011] arena mods: order.txt {} ({} folders, was {})", out ? "written" : "NOT written", kept.size(),
                order.size());
  }
  std::vector<fs::path> sorted;
  for (const auto& l : kept)
    for (const auto& f : folders)
      if (f.filename().string() == l) sorted.push_back(f);
  return sorted;
}

void LoadCustomArenas() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Arenas";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "arena.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());  // ("disabled": turned off in the launcher's Mods tab)
  folders = InInstallOrder(dir, folders);
  for (const auto& f : folders) {
    CustomArena c;
    c.id = f.filename().string();
    c.name = c.id;
    c.file = "Mods/Arenas/" + c.id + "/arena.pac";
    const fs::path manifest = fs::exists(f / "manifest.txt", ec) ? f / "manifest.txt" : f / "info.txt";
    if (FILE* t = std::fopen(manifest.string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) c.name = l.substr(5);
        if (l.rfind("base=", 0) == 0) c.base = l.substr(5);
        c.manifest += l;
        c.manifest += '\n';
      }
      std::fclose(t);
    }
    c.base_arena = ArenaOfBanner(c.base);
    c.tron_live = c.manifest.find("\ntitantron=live") != std::string::npos || c.manifest.rfind("titantron=live", 0) == 0;
    svrfmt::Bytes dds;
    if (svrfmt::ReadFile((f / "banner.dds").string(), dds)) c.banner = BannerBlocks(dds);
    if (c.banner.empty()) REXLOG_WARN("[svr2011] arena mods: {} has no 256 x 128 DXT5 banner.dds", c.id);
    if (fs::exists(f / "vs" / "theme.pac", ec)) c.theme_pac = (f / "vs" / "theme.pac").string();
    if (fs::is_directory(f / "vs", ec))
      for (const auto& v : fs::directory_iterator(f / "vs", ec)) {
        svrfmt::Bytes vdds;
        svrfmt::DdsInfo info;
        if (v.path().extension() != ".dds" || !svrfmt::ReadFile(v.path().string(), vdds) ||
            !svrfmt::DdsInfoOf(vdds, info) || info.format != svrfmt::DxtFormat::kDxt5)
          continue;
        const size_t n = size_t((info.w + 3) / 4) * ((info.h + 3) / 4) * 16;
        if (vdds.size() >= 128 + n) c.vs.push_back({v.path().stem().string(), Swap16(vdds.data() + 128, n)});
      }
    if (svrfmt::Bytes ldds; svrfmt::ReadFile((f / "load.dds").string(), ldds)) {
      svrfmt::DdsInfo info;
      if (svrfmt::DdsInfoOf(ldds, info) && info.w == 1024 && info.h == 512 && info.format == svrfmt::DxtFormat::kDxt5 &&
          ldds.size() >= 128 + 1024 * 512)
        c.load = Swap16(ldds.data() + 128, 1024 * 512);
      else
        REXLOG_WARN("[svr2011] arena mods: {} load.dds is not 1024 x 512 DXT5", c.id);
    }
    {  // the entrance title card's logo: card=<file> (card.dds without the key) - CardSwaps
      std::string card = "card.dds";
      for (size_t at = 0; at < c.manifest.size();) {
        size_t end = c.manifest.find('\n', at);
        if (end == std::string::npos) end = c.manifest.size();
        if (c.manifest.compare(at, 5, "card=") == 0) card = c.manifest.substr(at + 5, end - at - 5);
        at = end + 1;
      }
      while (!card.empty() && (card.back() == '\r' || card.back() == ' ')) card.pop_back();
      svrfmt::Bytes cdds;
      if (!card.empty() && svrfmt::ReadFile((f / fs::u8path(card)).string(), cdds) && !svrfmt::DdsDecode(cdds, c.card))
        REXLOG_WARN("[svr2011] arena mods: {} {} is not a DDS picture", c.id, card);
    }
    g_customs.push_back(std::move(c));
  }
  REXLOG_INFO("[svr2011] arena mods: {} custom arenas", g_customs.size());
  LayOutPages();
}

int Pages() { return 1 + int(g_layout.size()); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Finds each banner in guest physical memory (4 KB view) by a run of blocks.
// A block of the banner to look for it by: one of two colours mixed (a flat
// block - black, a solid alpha half - is everywhere: block 300's alpha half,
// the same in each opaque picture, made 3 million candidates and a 1 s
// hitch once the online screen's banners were looked for too).
size_t KeyBlock(const svrfmt::Bytes& d) {
  for (size_t n = 0; n < kBannerBlocks; ++n) {
    const size_t i = (kBannerBlocks / 2 + n) % kBannerBlocks;
    const uint8_t* q = d.data() + 16 * i + 8;  // (the colour half)
    if (std::memcmp(q, q + 2, 2) && !(q[4] == q[5] && q[5] == q[6] && q[6] == q[7])) return i;
  }
  return 300;
}

void LocateBanners() {
  uint8_t* b = g_memory->virtual_membase();
  for (auto& bn : g_banners) bn.guest.clear();
  // one pass: a key block's colour half -> the banners (and where it is in them)
  std::unordered_map<uint64_t, std::vector<std::pair<Banner*, size_t>>> keys;
  for (auto& bn : g_banners) {
    const size_t blk = KeyBlock(bn.data);
    uint64_t k;
    std::memcpy(&k, bn.data.data() + 16 * blk + 8, 8);
    keys[k].push_back({&bn, blk});
  }
  int partial = 0;
  constexpr uint64_t hi = 0xFFFF0000;
  ForEachRegion(0xE0000000, hi, [&](uint64_t at, uint64_t end) {
    for (uint64_t a = at; a + 64 <= end; a += 16) {
      uint64_t k;
      std::memcpy(&k, b + a + 8, 8);
      const auto it = keys.find(k);
      if (it == keys.end()) continue;
      for (const auto& [bn, blk] : it->second) {
        if (a < at + 16 * blk) continue;
        const uint64_t start = a - 16 * blk;
        // (a texture can run on into the next region: the whole of it mapped)
        rex::memory::HeapAllocationInfo info;
        const uint64_t last = start + bn->data.size() - 1;
        const bool mapped = last < end || (last < hi && Committed(uint32_t(last), info));
        if (mapped && !std::memcmp(b + start, bn->data.data(), bn->data.size()))
          bn->guest.push_back(uint32_t(start));
        else if (!std::memcmp(b + a, bn->data.data() + 16 * blk, 16))
          ++partial;
      }
    }
  });
  int found = 0, copies = 0;
  for (const auto& bn : g_banners) found += !bn.guest.empty(), copies += int(bn.guest.size());
  REXLOG_INFO("[svr2011] arena select: {} of {} banners found ({} places, {} partial matches left alone)", found,
              g_banners.size(), copies, partial);
}

// A dark grey tile for unused places on custom pages.
const svrfmt::Bytes& EmptyTile() {
  static svrfmt::Bytes empty;
  if (empty.empty()) {
    const uint8_t blk[16] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0xE7, 0x39, 0xE7, 0x39, 0, 0, 0, 0};
    for (size_t k = 0; k < kBannerBlocks; ++k) empty.insert(empty.end(), blk, blk + 16);
    empty = Swap16(empty.data(), empty.size());
  }
  return empty;
}

// The custom arena on tile k of the current page (nullptr: none).
const CustomArena* CustomAt(int k) {
  if (g_page == 0 || g_page - 1 >= int(g_layout.size())) return nullptr;
  const int i = g_layout[g_page - 1][k];
  return i >= 0 ? &g_customs[i] : nullptr;
}

// The online screen's title over the grid (the tile's arena name, looked up
// from 0x828B5A00 as the cursor moves - "SUMMERSLAM" over a custom arena on
// its tile): the custom arena's name, in capitals as the game's.
uint32_t g_title_text = 0;
constexpr uint32_t kOnlineTitleCaller = 0x828B5A00;

// Draws the current page into the 20 banner textures.
const svrfmt::Bytes& EmptyTile();

// What tile k of the current page shows (nullptr: the game's own banner).
const svrfmt::Bytes* PageBanner(int k) {
  if (g_page == 0) return nullptr;
  const CustomArena* c = CustomAt(k);
  return c && !c->banner.empty() ? &c->banner : &EmptyTile();
}

// The page drawn into the banner textures is still there: the game has not
// loaded its banners anew (it reopened the screen). Only then may a gap in
// the grid's updates keep the page.
bool PageStillShown(const uint8_t* base) {
  if (g_page == 0) return false;
  int checked = 0;
  for (int k = 0; k < 20; ++k) {
    const svrfmt::Bytes* src = PageBanner(k);
    if (!src) continue;
    for (const Banner& bn : g_banners) {
      if (bn.name != PictureOf(k)) continue;
      for (const uint32_t at : bn.guest)
        if (std::memcmp(base + at, src->data(), src->size())) {
          size_t first = 0;
          while (first < src->size() && base[at + first] == (*src)[first]) ++first;
          REXLOG_INFO("[svr2011] arena select: page {} tile {} not shown at {:08X} (differs from byte {} of {})",
                      g_page + 1, k, at, first, src->size());
          return false;
        }
      checked += !bn.guest.empty();
    }
  }
  return checked > 0;
}

void ShowPage(uint8_t* base) {
  for (int k = 0; k < 20; ++k)
    for (const Banner& bn : g_banners) {  // (each version of the tile's picture found)
      if (bn.name != PictureOf(k) || bn.guest.empty()) continue;
      const svrfmt::Bytes* src = PageBanner(k);
      if (!src) src = &bn.data;
      for (const uint32_t at : bn.guest) {
        std::memcpy(base + at, src->data(), src->size());
        svr2011::native::GuestWritten(at, uint32_t(src->size()));  // (re-checked at once, not tile by tile)
      }
    }
  REXLOG_INFO("[svr2011] arena select: page {} / {}", g_page + 1, Pages());
}

// -- The VS screen theme ------------------------------------------------------
//
// Each arena has a VS screen theme (menu/MatchHD.pac group M<nn>I, DXT5
// textures); a custom arena's vs/<name>.dds replace its base arena's
// pictures of that name (see the loading and VS screens below).
const void* g_redirected_custom = nullptr;
// what the last pick (A) or the screen's opening left chosen (B restores it)
const void* g_committed_custom = nullptr;
int g_committed_host = -1;

// The theme textures of arena `number`: name -> top level blocks, swapped.
std::vector<std::pair<std::string, svrfmt::Bytes>> VsTheme(int number) {
  std::vector<std::pair<std::string, svrfmt::Bytes>> out;
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "MatchHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return out;
  char group[8];
  std::snprintf(group, sizeof group, "M%02dI", number);
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != group) continue;
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(pe.data), texs)) continue;
        for (const auto& t : texs) {
          svrfmt::DdsInfo info;
          if (!svrfmt::DdsInfoOf(t.data, info) || info.format != svrfmt::DxtFormat::kDxt5) continue;
          const size_t n = size_t((info.w + 3) / 4) * ((info.h + 3) / 4) * 16;
          if (t.data.size() >= 128 + n) out.push_back({t.name, Swap16(t.data.data() + 128, n)});
        }
      }
    }
  return out;
}

// -- The loading and VS screens ---------------------------------------------
//
// The NOW LOADING screens (menu/LoadHD.pac, LOAD group) are DXT5 pictures,
// either one 1024 x 512 or two 512 x 512 halves (the set's first half on the
// left); which set shows depends on the mode (a Universe show, an
// exhibition, a tip...). The VS screen theme (menu/MatchHD.pac M<nn>I) is
// picked in sub_8271D868: the match's arena number, or 60-66 for some match
// types, and M00I (SmackDown's) when that theme's file is missing.
//
// While a custom arena is chosen: its VS theme is always its base arena's
// (the theme path is rewritten when the game checks it exists, in
// sub_826A17D0), and every picture the game unpacks (sub_826AEF10, the BPE
// decoder) that is one of the loading pictures or of that theme gets the
// arena's own (load.dds, vs/<name>.dds) before the game uses it - no memory
// scan, so it can't be late or missed.
// The loading pictures are swapped only while a match loads (from its set-up
// to its people being placed, match_types.cpp): other screens show the same
// pictures - the WWE SHOP background is LoadHD's Online_tex00/01 - and got the
// arena's loading picture after a match on it. The VS theme's pictures (only
// on the VS screen) are swapped while the arena is chosen.
struct DecodeSwap {
  std::string name;                           // the texture's name in its set
  size_t size = 0;                            // its top level's size
  uint64_t hash = 0;                          // Fnv of its top level as in the file (not the bytes: 240 x 512 KB)
  std::shared_ptr<const svrfmt::Bytes> mine;  // (shared: one per part of the custom picture)
  bool loading = false;                       // a loading picture (only while a match loads)
};
std::atomic<bool> g_match_loading{false};
struct DecodeSwaps {
  std::vector<DecodeSwap> swaps;
  std::unordered_map<uint64_t, std::vector<size_t>> keys;  // middle 32 bytes -> swaps
  std::vector<size_t> sizes;                               // the swaps' top level sizes (distinct)
  int theme = -1;                                          // VS theme forced (-1 none)
  std::string arena;                                       // the arena mod's id (its own VS theme: theme_group)
  // the title card's pictures (CardSwaps): a whole DDS (header and mips) by its size and Fnv -> the arena's
  std::unordered_map<size_t, std::vector<std::pair<uint64_t, std::shared_ptr<const svrfmt::Bytes>>>> cards;
};
std::mutex g_decode_mutex;
std::shared_ptr<const DecodeSwaps> g_decode;

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// The entrance's title card (show logo + city, sub_8277E928): pac/evt/evtgdhd.pac ETEX <arena number, 4 digits> -
// a PACH of BPE parts; parts 0 and 10 are DDS sets (PACH) whose pictures layouts 10000 / 10010 (AG2D: u32 count at
// +0x10, its 16-byte names from the offset at +0x14) name in order. A custom arena shows its base's card; with a
// card picture the base's logo pictures ("logo") get it, fitted (RAW's hd_RAW01: a strip of 4 logo steps, one in
// each quarter), and its build-up / effect frames (fire, anime, spiral, loop, light, numbered frames) are cleared -
// plates and bars stay.
struct CardPic {
  size_t size = 0;
  uint64_t hash = 0;
  int role = 0;  // 1 logo, 2 cleared, 3 a strip of 4 logos (one under the other)
  svrfmt::DdsInfo info;
};
int CardRole(std::string n) {
  for (auto& ch : n) ch = char(std::tolower(uint8_t(ch)));
  if (n.find("logo") != std::string::npos) return 1;
  if (n == "hd_raw01") return 3;
  for (const char* p : {"fire", "anime", "spiral", "loop", "light"})
    if (n.find(p) != std::string::npos) return 2;
  if (n.size() == 2 && std::isdigit(uint8_t(n[0])) && std::isdigit(uint8_t(n[1]))) return 2;
  return 0;
}
std::vector<CardPic> CardPictures(int arena) {
  static std::mutex m;
  static std::map<int, std::vector<CardPic>> cache;
  std::lock_guard lock(m);
  if (auto it = cache.find(arena); it != cache.end()) return it->second;
  std::vector<CardPic> out;
  svrfmt::Bytes d;
  svrfmt::Epac e;
  char key[8];
  std::snprintf(key, sizeof key, "%04d", arena);
  if (arena >= 0 && arena < 10000 && svrfmt::ReadFile((g_game / "pac" / "evt" / "evtgdhd.pac").string(), d) &&
      svrfmt::EpacRead(d, e))
    for (const auto& g : e.groups) {
      if (g.type != "ETEX") continue;
      for (const auto& en : g.entries) {
        std::vector<svrfmt::PachEntry> parts;
        if (en.name != key || !svrfmt::PachRead(en.data, parts)) continue;
        auto part = [&](uint32_t id) {
          std::vector<svrfmt::PachEntry> in;
          for (const auto& p : parts)
            if (p.id == id && svrfmt::PachRead(svrfmt::Unpack(p.data), in)) return in;
          return std::vector<svrfmt::PachEntry>();
        };
        for (const uint32_t set : {0u, 10u}) {
          const auto pics = part(set), layout = part(10000 + set);
          if (pics.empty() || layout.empty()) continue;
          const svrfmt::Bytes& a = layout[0].data;
          if (a.size() < 0x18 || std::memcmp(a.data(), "AG2D", 4)) continue;
          const auto be = [&](size_t o) { return uint32_t(a[o]) << 24 | uint32_t(a[o + 1]) << 16 | uint32_t(a[o + 2]) << 8 | a[o + 3]; };
          const uint32_t n = be(0x10), names = be(0x14);
          for (uint32_t i = 0; i < n && i < pics.size() && names + 16 * (i + 1) <= a.size(); ++i) {
            const char* s = reinterpret_cast<const char*>(&a[names + 16 * i]);
            CardPic cp;
            cp.role = CardRole(std::string(s, strnlen(s, 16)));
            const svrfmt::Bytes& dds = pics[i].data;
            if (!cp.role || !svrfmt::DdsInfoOf(dds, cp.info) || cp.info.format != svrfmt::DxtFormat::kDxt5) continue;
            cp.size = dds.size(), cp.hash = Fnv(dds.data(), dds.size());
            out.push_back(cp);
          }
        }
      }
    }
  cache[arena] = out;
  return out;
}
// The card pictures of arena `arena` made from `logo` (fitted into each logo picture, the rest cleared).
void CardSwaps(DecodeSwaps& t, int arena, const svrfmt::Image& logo) {
  int made = 0;
  for (const auto& cp : CardPictures(arena)) {
    svrfmt::Image img;
    img.w = cp.info.w, img.h = cp.info.h;
    img.rgba.assign(size_t(img.w) * img.h * 4, 0);
    const int rows = cp.role == 3 ? 4 : 1, rh = img.h / rows;
    for (int r = 0; r < rows && cp.role != 2 && logo.w > 0 && logo.h > 0 && rh > 0; ++r) {
      const float k = std::min(img.w * 0.96f / logo.w, rh * 0.96f / logo.h);
      const int w = std::max(1, int(logo.w * k)), h = std::max(1, int(logo.h * k));
      const svrfmt::Image s = svrfmt::Resize(logo, w, h);
      const int x0 = (img.w - w) / 2, y0 = r * rh + (rh - h) / 2;
      for (int y = 0; y < h; ++y)
        std::memcpy(&img.rgba[(size_t(y0 + y) * img.w + x0) * 4], &s.rgba[size_t(y) * w * 4], size_t(w) * 4);
    }
    auto mine = std::make_shared<const svrfmt::Bytes>(svrfmt::DdsEncode(img, svrfmt::DxtFormat::kDxt5, cp.info.mips > 1));
    if (mine->size() != cp.size) continue;  // (another layout of the file: left as it is)
    t.cards[cp.size].push_back({cp.hash, std::move(mine)});
    ++made;
  }
  REXLOG_INFO("[svr2011] arena mods: title card logo - {} pictures of arena {}'s card", made, arena);
}

// A part the game just unpacked: the title card's pictures that have a swap (CardSwaps).
void CardDecoded(uint8_t* d, uint32_t size);

struct LoadPic {
  std::string name;
  size_t size = 0;    // the top level's size
  uint64_t key = 0;   // Fnv of its middle 32 bytes (DecodeSwaps::keys)
  uint64_t hash = 0;  // Fnv of the whole top level as in the file
  int part = 0;       // 0 whole, 1 left half, 2 right half
};
std::vector<LoadPic> g_load_pics;  // every loading picture of the game (distinct)
// (read on a thread of its own at start - LoadHD.pac whole, 2-3.5 s on a
// desktop - and only when an installed custom arena has a loading picture;
// SetDecodeSwaps waits for it)
std::mutex g_load_pics_mutex;
std::condition_variable g_load_pics_cv;
bool g_load_pics_ready = false;

LoadPic PicOf(const std::string& name, const uint8_t* top, size_t size, int part) {
  return {name, size, Fnv(top + size / 2, 32), Fnv(top, size), part};
}

void ReadLoadingPictures() {
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "LoadHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return;
  std::unordered_map<uint64_t, int> seen;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(pe.data), texs)) continue;
        std::vector<const svrfmt::BundleTexture*> halves;
        for (const auto& t : texs) {
          svrfmt::DdsInfo info;
          if (!svrfmt::DdsInfoOf(t.data, info) || info.format != svrfmt::DxtFormat::kDxt5) continue;
          if (info.w == 1024 && info.h == 512 && t.data.size() >= 128 + 1024 * 512) {
            LoadPic lp = PicOf(t.name, t.data.data() + 128, 1024 * 512, 0);
            if (!seen[lp.hash ^ Fnv(reinterpret_cast<const uint8_t*>(lp.name.data()), lp.name.size())]++)
              g_load_pics.push_back(std::move(lp));
          } else if (info.w == 512 && info.h == 512 && t.data.size() >= 128 + 512 * 512) {
            halves.push_back(&t);
          }
        }
        std::sort(halves.begin(), halves.end(), [](auto* a, auto* b) { return a->name < b->name; });
        for (size_t k = 0; k < halves.size() && k < 2; ++k) {
          LoadPic lp = PicOf(halves[k]->name, halves[k]->data.data() + 128, 512 * 512, int(k) + 1);
          if (!seen[lp.hash ^ Fnv(reinterpret_cast<const uint8_t*>(lp.name.data()), lp.name.size())]++)
            g_load_pics.push_back(std::move(lp));
        }
      }
    }
  REXLOG_INFO("[svr2011] arena mods: {} loading screen pictures", g_load_pics.size());
}

// A superstar mod with a loading picture (its load.dds, or a manifest loading= line): SetSuperstarLoading.
bool AnySuperstarLoading() {
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(g_game / "Mods" / "Superstars", ec)) {
    if (!d.is_directory(ec)) continue;
    if (fs::exists(d.path() / "load.dds", ec)) return true;
    svrfmt::Bytes man;
    if (svrfmt::ReadFile((d.path() / "manifest.txt").string(), man) &&
        std::string(man.begin(), man.end()).find("\nloading=") != std::string::npos)
      return true;
  }
  return false;
}

void LoadLoadingPictures() {
  const bool wanted = std::any_of(g_customs.begin(), g_customs.end(), [](const CustomArena& c) { return !c.load.empty(); }) ||
                      AnySuperstarLoading();
  if (!wanted) {
    REXLOG_INFO("[svr2011] arena mods: no custom loading screens - the game's own aren't read");
    std::lock_guard lock(g_load_pics_mutex);
    g_load_pics_ready = true;
    return;
  }
  // (plain host file reads and its own vectors only: nothing the game's threads use)
  std::thread([] {
    rex::thread::set_current_thread_name("arena loading pictures");
    ReadLoadingPictures();
    {
      std::lock_guard lock(g_load_pics_mutex);
      g_load_pics_ready = true;
    }
    g_load_pics_cv.notify_all();
  }).detach();
}

// The loading pictures read (at most `wait` for them; false: not yet).
bool LoadingPicturesReady(std::chrono::milliseconds wait) {
  std::unique_lock lock(g_load_pics_mutex);
  return g_load_pics_cv.wait_for(lock, wait, [] { return g_load_pics_ready; });
}

// The custom arena's picture cut to fit (whole, or a half: 1024 x 512 DXT5
// rows are 256 blocks of 16 bytes).
svrfmt::Bytes LoadPart(const svrfmt::Bytes& load, int part) {
  if (part == 0) return load;
  svrfmt::Bytes o;
  o.reserve(512 * 512);
  for (size_t row = 0; row < 128; ++row) {
    const auto from = load.begin() + row * 4096 + (part == 2 ? 2048 : 0);
    o.insert(o.end(), from, from + 2048);
  }
  return o;
}

svrfmt::Bytes g_star_load;   // a superstar mod's loading picture for the match being set up (kept swapped, as c->load)
std::string g_star_load_file;
const CustomArena* g_swaps_arena = nullptr;   // (SetDecodeSwaps' last arena and host: rebuilt with g_star_load)
int g_swaps_host = -1;

void SetDecodeSwaps(const CustomArena* c, int host) {
  g_swaps_arena = c, g_swaps_host = host;
  auto t = std::make_shared<DecodeSwaps>();
  const svrfmt::Bytes& load_pic = !g_star_load.empty() ? g_star_load : c ? c->load : g_star_load;   // (the superstar's wins)
  if (c || !load_pic.empty()) {
    t->theme = c ? host : -1;
    t->arena = c ? c->id : std::string();
    // (a game picture by its key / hash; the replacement shared - not 240
    // copies of the game's pictures and 240 of the custom one's)
    auto add = [&](const std::string& name, const LoadPic& orig, std::shared_ptr<const svrfmt::Bytes> mine, bool loading) {
      if (!mine || orig.size != mine->size() || orig.size < 64) return;
      t->keys[orig.key].push_back(t->swaps.size());
      if (std::find(t->sizes.begin(), t->sizes.end(), orig.size) == t->sizes.end()) t->sizes.push_back(orig.size);
      t->swaps.push_back({name, orig.size, orig.hash, std::move(mine), loading});
    };
    if (!load_pic.empty() && !LoadingPicturesReady(std::chrono::seconds(10))) {
      REXLOG_WARN("[svr2011] arena mods: the game's loading pictures still not read after 10 s - {} keeps the game's",
                  c ? c->id : g_star_load_file);
    } else if (!load_pic.empty()) {
      const svrfmt::Bytes load = Swap16(load_pic.data(), load_pic.size());  // (kept swapped: back to file order)
      std::shared_ptr<const svrfmt::Bytes> parts[3];
      for (int k = 0; k < 3; ++k) parts[k] = std::make_shared<const svrfmt::Bytes>(LoadPart(load, k));
      for (const auto& lp : g_load_pics)
        if (lp.part >= 0 && lp.part < 3) add(lp.name, lp, parts[lp.part], true);
    }
    if (c && !c->card.rgba.empty() && host >= 0) CardSwaps(*t, host, c->card);
    if (c && !c->vs.empty()) {
      const auto theme = VsTheme(host);
      for (const auto& [name, mine] : c->vs)
        for (const auto& [tname, orig] : theme)
          if (tname == name) {
            const svrfmt::Bytes o = Swap16(orig.data(), orig.size());
            add(tname, PicOf(tname, o.data(), o.size(), 0),
                std::make_shared<const svrfmt::Bytes>(Swap16(mine.data(), mine.size())), false);
          }
    }
  }
  REXLOG_INFO("[svr2011] arena mods: loading / VS pictures {} for {} (theme {:02}){}", t->swaps.size(),
              c ? c->id : std::string("-"), host, g_star_load.empty() ? "" : " - the loading picture " + g_star_load_file);
  std::lock_guard lock(g_decode_mutex);
  g_decode = c || !load_pic.empty() ? std::move(t) : nullptr;
}

void StarLoading(const std::string& file) {
  if (file == g_star_load_file) return;
  svrfmt::Bytes pic;
  if (!file.empty()) {
    svrfmt::Bytes dds;
    svrfmt::DdsInfo info;
    if (svrfmt::ReadFile(file, dds) && svrfmt::DdsInfoOf(dds, info) && info.w == 1024 && info.h == 512 &&
        info.format == svrfmt::DxtFormat::kDxt5 && dds.size() >= 128 + 1024 * 512)
      pic = Swap16(dds.data() + 128, 1024 * 512);
    else
      REXLOG_WARN("[svr2011] superstar mods: {} is not a 1024 x 512 DXT5 loading picture", file);
  }
  g_star_load = std::move(pic);
  g_star_load_file = file;
  SetDecodeSwaps(g_swaps_arena, g_swaps_host);
}

std::shared_ptr<const DecodeSwaps> CurrentDecodeSwaps() {
  std::lock_guard lock(g_decode_mutex);
  return g_decode;
}

// Texture names compare without case (the set holds up to 16 characters).
bool NameIs(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}

// A texture set the game just unpacked (u32 count, 16-byte header, 32-byte
// records: name[16], "dds", size, offset): its pictures that have a swap.
void SwapDecoded(uint8_t* d, uint32_t size) {
  const auto t = CurrentDecodeSwaps();
  if (!t || t->swaps.empty() || size < 48) return;
  const uint32_t n = d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24;
  if (!n || n > 512 || 16 + 32ull * n > size) return;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = d + 16 + 32 * i;
    if (std::memcmp(r + 16, "dds", 3)) return;  // not a texture set
    const uint32_t sz = r[20] | r[21] << 8 | r[22] << 16 | uint32_t(r[23]) << 24;
    const uint32_t off = r[24] | r[25] << 8 | r[26] << 16 | uint32_t(r[27]) << 24;
    if (uint64_t(off) + sz > size || sz < 128 + 64 || std::memcmp(d + off, "DDS ", 4)) continue;
    uint8_t* top = d + off + 128;
    const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
    for (size_t level0 : t->sizes) {
      if (128 + level0 > sz) continue;
      auto it = t->keys.find(Fnv(top + level0 / 2, 32));
      if (it == t->keys.end()) continue;
      uint64_t hash = 0;  // (the whole top level's: only once its key and name match)
      for (size_t k : it->second) {
        const auto& sw = t->swaps[k];
        if (sw.size != level0 || (sw.loading && !g_match_loading) || !NameIs(name, sw.name)) continue;
        if (!hash) hash = Fnv(top, level0);
        if (hash != sw.hash) continue;
        std::memcpy(top, sw.mine->data(), level0);
        static int logged = 0;
        if (logged++ < 40) REXLOG_INFO("[svr2011] arena mods: picture {} swapped as it was unpacked", name);
        break;
      }
    }
  }
}

void CardDecoded(uint8_t* d, uint32_t size) {
  if (size < 8 || std::memcmp(d, "PACH", 4)) return;
  const auto t = CurrentDecodeSwaps();
  if (!t || t->cards.empty()) return;
  const auto le = [](const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; };
  const uint32_t n = le(d + 4);
  const uint64_t base = 8 + 12ull * n;
  if (!n || n > 4096 || base > size) return;
  int swapped = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t off = base + le(d + 8 + 12 * i + 4), sz = le(d + 8 + 12 * i + 8);
    if (off + sz > size || sz < 128 || std::memcmp(d + off, "DDS ", 4)) continue;
    const auto it = t->cards.find(size_t(sz));
    if (it == t->cards.end()) continue;
    const uint64_t h = Fnv(d + off, size_t(sz));
    for (const auto& [hash, mine] : it->second)
      if (hash == h) {
        std::memcpy(d + off, mine->data(), size_t(sz));
        ++swapped;
        break;
      }
  }
  if (swapped) REXLOG_INFO("[svr2011] arena mods: title card - {} pictures swapped as they were unpacked", swapped);
}

// "/MENU/M<nn>I" or "/MENU/M<nn>L" (the VS theme): the custom arena's base theme.
void ForceTheme(char* path) {
  const auto t = CurrentDecodeSwaps();
  if (!t || t->theme < 0 || t->theme > 99) return;
  if (std::strncmp(path, "/MENU/M", 7) || !std::isdigit(uint8_t(path[7])) || !std::isdigit(uint8_t(path[8])) ||
      (path[9] != 'I' && path[9] != 'L') || path[10])
    return;
  const char a = char('0' + t->theme / 10), b = char('0' + t->theme % 10);
  if (path[7] == a && path[8] == b) return;
  REXLOG_INFO("[svr2011] arena mods: VS theme {} -> {}{}", path, a, b);
  path[7] = a;
  path[8] = b;
}

// The titantron. The game shows its videos on an arena's models 800-899 (every stock arena: 801 / 807 the arena's
// loop video, 803 / 806 / 808 the entrance video): during entrances the video, in a match the match camera (it
// resolves the camera into their texture). Stock arenas are made for that; a ported one (WWE 2K / '13) has its own
// screen art on other models, flagged g (shown in matches only), which those screens cover - so its match showed the
// camera over it (user report 20 Oct). Custom arenas therefore get their 800-899 models flagged e (0xC351: shown
// during entrances only) - the entrance video stays, the match shows the arena's own screens; manifest
// titantron=live keeps the camera. The flagged copy: Mods/ArenaOverlay/tron_<id>.pac, made once (again when the
// arena's file changes: size and time in tron_<id>.pac.src).
std::string TitantronFile(const CustomArena& c) {
  if (c.tron_live) return c.file;
  std::error_code ec;
  const fs::path src = g_game / fs::u8path(c.file);
  const std::string rel = "Mods/ArenaOverlay/tron_" + c.id + ".pac";
  const fs::path dst = g_game / fs::u8path(rel), stamp_file = dst.string() + ".src";
  // (" v2": announce table monitors kept - copies made by the first rule are made again)
  const std::string stamp = std::to_string(fs::file_size(src, ec)) + " " +
                            std::to_string(static_cast<long long>(fs::last_write_time(src, ec).time_since_epoch().count())) +
                            " v2";
  if (fs::exists(dst, ec)) {
    std::ifstream in(stamp_file);
    std::string had;
    std::getline(in, had);
    if (had == stamp) return rel;
    if (had == stamp + " unchanged") return c.file;
  }
  svrfmt::Bytes data;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile(src.string(), data) || !svrfmt::EpacRead(data, e)) {
    REXLOG_WARN("[svr2011] arena mods: {} - its file can't be read for the titantron (as it is)", c.id);
    return c.file;
  }
  int flagged = 0, kept = 0;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      const svrfmt::Bytes raw = svrfmt::Unpack(en.data);
      std::vector<svrfmt::PachEntry> kids;
      if (raw.size() < 4 || std::memcmp(raw.data(), "PACH", 4) || !svrfmt::PachRead(raw, kids)) continue;
      // Which of its 800-899 models are the hall's screens: an announce table's monitor (small, at the ring:
      // a 2K17 port's 820) keeps the game's feed in the match - it showed nothing once flagged (21 Oct report).
      std::set<int> monitors;
      for (const auto& k : kids) {
        if (k.id < 800 || k.id > 899) continue;
        svrfmt::Bytes m = svrfmt::Unpack(k.data);
        if (svrfmt::IsPach(m)) {
          std::vector<svrfmt::PachEntry> parts;
          svrfmt::Bytes jboy;
          if (svrfmt::PachRead(m, parts))
            for (const auto& p : parts)
              if (p.id == 0) jboy = svrfmt::Unpack(p.data);
          m = std::move(jboy);
        }
        svrfmt::Model model;
        if (!svrfmt::IsJboy(m) || !svrfmt::JboyRead(m, model) || model.meshes.empty()) continue;
        bool small_at_ring = true;
        for (const auto& s : model.meshes)
          small_at_ring = small_at_ring && s.sphere[3] < 60.f && std::fabs(s.sphere[0]) < 60.f && std::fabs(s.sphere[2]) < 60.f;
        if (small_at_ring) monitors.insert(int(k.id));
      }
      bool changed = false;
      for (auto& k : kids) {
        if (k.id != 0xC351) continue;
        const svrfmt::Bytes text = svrfmt::Unpack(k.data);
        std::string t(text.begin(), text.end()), out;
        size_t at = 0;
        while (at < t.size()) {  // (lines "<model id> <flags>"; ids 800-899 without e / g get e)
          size_t end = t.find('\n', at);
          if (end == std::string::npos) end = t.size();
          std::string line = t.substr(at, end - at);
          const bool cr = !line.empty() && line.back() == '\r';
          if (cr) line.pop_back();
          size_t p = line.find_first_not_of(" \t");
          if (p != std::string::npos && std::isdigit(uint8_t(line[p]))) {
            const int id = std::atoi(line.c_str() + p);
            std::istringstream words(line.substr(p));
            std::string w;
            bool has = false;
            words >> w;
            while (words >> w) has |= w == "e" || w == "g";
            if (id >= 800 && id <= 899 && !has) {
              if (monitors.count(id)) ++kept;
              else line += " e", ++flagged, changed = true;
            }
          }
          out += line + (cr ? "\r" : "") + (end < t.size() ? "\n" : "");
          at = end + 1;
        }
        if (changed) k.data = svrfmt::BpeEncode(svrfmt::Bytes(out.begin(), out.end()));
      }
      if (changed) {  // (packed again when it was: the arena's memory holds the file as it is)
        const bool packed = en.data.size() >= 4 && !std::memcmp(en.data.data(), "BPE ", 4);
        en.data = packed ? svrfmt::BpeEncode(svrfmt::PachWrite(kids)) : svrfmt::PachWrite(kids);
      }
    }
  fs::create_directories(dst.parent_path(), ec);
  if (!flagged) {  // (no video screens to change: its own file, remembered)
    std::ofstream(dst, std::ios::binary | std::ios::trunc);
    std::ofstream(stamp_file, std::ios::trunc) << stamp << " unchanged\n";
    return c.file;
  }
  const svrfmt::Bytes out = svrfmt::EpacWrite(e);
  const fs::path tmp = dst.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!o) {
      REXLOG_WARN("[svr2011] arena mods: {} - the titantron copy can't be written (as it is)", c.id);
      return c.file;
    }
  }
  fs::remove(dst, ec);
  fs::rename(tmp, dst, ec);
  if (ec) return c.file;
  std::ofstream(stamp_file, std::ios::trunc) << stamp << "\n";
  REXLOG_INFO("[svr2011] arena mods: {} - {} video screens shown during entrances only (its own screens in the "
              "match; titantron=live keeps the camera){}",
              c.id, flagged, kept ? fmt::format(", {} announce table monitor(s) as they were", kept) : std::string());
  return rel;
}

// Custom arena c picked on the tile of arena `tile` (nullptr: none): it plays as its base arena (the tile's when it
// has none) - that arena's file is c's, and a match on the tile plays as that arena (g_plays_as).
int g_redirected_file = -1;  // (the arena whose file is the custom one: its base)
void PointAt(const CustomArena* c, int tile) {
  if (g_redirected_file >= 0) svr2011::RedirectArena(g_redirected_file, "");
  if (g_redirected_host >= 0) g_plays_as[g_redirected_host] = 0;
  g_redirected_custom = c;
  g_redirected_host = g_redirected_file = -1;
  const int as = c ? (c->base_arena >= 0 ? c->base_arena : tile) : -1;
  SetDecodeSwaps(c, as);
  svr2011::SetRingRules(c ? c->manifest : std::string());
  if (!c || tile < 0) return;
  svr2011::RedirectArena(as, TitantronFile(*c));
  g_redirected_host = tile, g_redirected_file = as;
  if (as != tile) g_plays_as[tile] = as + 1;  // (+1: 0 is none - a base of 0, SmackDown, is 1)
}

// The custom arena on the tile under the cursor is the one played.
void FollowCursor(int row, int col) {
  const CustomArena* c = row < 4 ? CustomAt(row * 5 + col) : nullptr;
  g_cursor_custom = c ? int(c - g_customs.data()) : -1;
  const int tile = c ? g_tiles[row * 5 + col].arena : -1;
  if (c == g_redirected_custom && tile == g_redirected_host) return;
  PointAt(c, tile);
  if (c)
    REXLOG_INFO("[svr2011] arena select: {} on {} (arena {}, plays as arena {})", c->name, g_tiles[row * 5 + col].banner,
                tile, g_redirected_file);
}

}  // namespace

// The match record takes the match screen's arena (sub_82773F00: screen
// *0x82EDEB48 +244 via sub_8247B6C8 -> the live record +64). A slot that plays
// as another arena (RedirectArena: the stock practice ring) gets that number
// here, on the screen, before the copy - as if picked in SELECT ARENA. (Not on
// the live record after it: parts of the load keep the number they read first.)
// The crowd's flip-flop buffer index (0x82E35380: sub_8217D070 flips it 0 <-> 1
// once a frame; the crowd renderer sub_8217DCE8 picks buffer 0x82D61290 +
// index * 0x1E00C by it). Ported WWE 2K14+ arenas now and then crashed at load
// with it holding garbage (~8133: lr 8217FF84, a null count array) - under load,
// in the full pass, not in repeat runs of the same files. It has two valid
// states: anything else is reset to 0, logged once per match with the arena and
// the crowd frame count.
std::atomic<int> g_crowd_arena{-1};
std::atomic<uint32_t> g_crowd_frames{0};
std::atomic<bool> g_crowd_guard_logged{false};

void CrowdIndexGuard(uint8_t* base, const char* where) {
  constexpr uint32_t kCrowdIndex = 0x82E35380;
  uint8_t* q = base + kCrowdIndex;
  const uint32_t v = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  if (v <= 1) return;
  q[0] = q[1] = q[2] = q[3] = 0;
  if (!g_crowd_guard_logged.exchange(true))
    REXLOG_WARN("[svr2011] crowd guard: buffer index {:08X} (not 0 / 1) reset to 0 in {} - arena {}, crowd frame {}", v,
                where, g_crowd_arena.load(), g_crowd_frames.load());
}

REX_EXTERN(__imp__sub_8217D070);
REX_HOOK_RAW(sub_8217D070) {
  CrowdIndexGuard(base, "the flip");
  g_crowd_frames.fetch_add(1);
  __imp__sub_8217D070(ctx, base);
}

REX_EXTERN(__imp__sub_8217DCE8);
REX_HOOK_RAW(sub_8217DCE8) {
  CrowdIndexGuard(base, "the crowd renderer");
  __imp__sub_8217DCE8(ctx, base);
}

// The live match settings' arena (0x82E3DE00 +0x40) holds the tile's number from the pick until the match record
// copies the screen's: the first load's show branding, the host announcer and the interfaces read it there (a RAW
// port on the Judgment Day tile: its first entrance as Judgment Day, 21 Oct report) - it plays as its base too.
int g_loads_since_pick = 0;  // (a match's loads, for the log: counted from the match screen's arena read)

void LiveArenaPlaysAs(uint8_t* base) {
  uint8_t* q = base + 0x82E3DE40;
  const uint32_t arena = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  if (arena >= 100 || !g_plays_as[arena]) return;
  const uint32_t as = uint32_t(g_plays_as[arena] - 1);
  q[0] = uint8_t(as >> 24), q[1] = uint8_t(as >> 16), q[2] = uint8_t(as >> 8), q[3] = uint8_t(as);
  REXLOG_INFO("[svr2011] arena mods: the live settings' arena {} plays as arena {}", arena, as);
}

REX_EXTERN(__imp__sub_82773F00);
REX_HOOK_RAW(sub_82773F00) {
  constexpr uint32_t kMatchScreen = 0x82EDEB48, kScreenArena = 244;
  const auto rd = [&](uint32_t a) {
    const uint8_t* q = base + a;
    return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  };
  g_crowd_guard_logged = false, g_crowd_frames = 0;  // (the crowd guard: once per match)
  LiveArenaPlaysAs(base);
  if (const uint32_t screen = rd(kMatchScreen)) {
    const uint32_t arena = rd(screen + kScreenArena);
    g_crowd_arena = int(arena);
    const int as = svr2011::ArenaNumberFor(int(arena));
    REXLOG_INFO("[svr2011] arena mods: a match loads in arena {}{}", arena,
                g_redirected_custom ? " - " + static_cast<const CustomArena*>(g_redirected_custom)->name + " chosen" : "");
    // Test aid: SVR2011_TEST_ARENA_SCAN=1 - where else the tile's arena number sits (the match screen, the live settings)
    if (std::getenv("SVR2011_TEST_ARENA_SCAN") && g_redirected_host >= 0) {
      const uint32_t tile = uint32_t(g_redirected_host);
      std::string s;
      for (uint32_t o = 0; o < 0x1000; ++o) {
        if (o % 4 == 0 && rd(screen + o) == tile) s += fmt::format(" screen+{:X}(w)", o);
        if (base[screen + o] == tile) s += fmt::format(" screen+{:X}(b)", o);
      }
      for (uint32_t o = 0; o < 0x800; ++o) {
        if (o % 4 == 0 && rd(0x82E3DE00 + o) == tile) s += fmt::format(" live+{:X}(w)", o);
        if (base[0x82E3DE00 + o] == tile) s += fmt::format(" live+{:X}(b)", o);
      }
      REXLOG_INFO("[svr2011] arena mods: test - tile arena {} at:{}", tile, s);
    }
    if (as != int(arena)) {
      uint8_t* q = base + screen + kScreenArena;
      q[0] = uint8_t(uint32_t(as) >> 24), q[1] = uint8_t(uint32_t(as) >> 16), q[2] = uint8_t(uint32_t(as) >> 8), q[3] = uint8_t(as);
      REXLOG_INFO("[svr2011] arena mods: the match in arena {} plays as arena {}", arena, as);
    }
    // (for reports: each load since the pick - an entrance, the match - with the arena file it reads)
    {
      std::string file;
      if (as >= 0 && as < 100) {
        std::lock_guard lock(g_mutex);
        file = g_redirect[as];
      }
      REXLOG_INFO("[svr2011] arena mods: load {} since the pick - arena {} (screen {}, live {}), BG{:02} -> {}", ++g_loads_since_pick,
                  as, arena, rd(0x82E3DE40), as, file.empty() ? "original" : file);
    }
    // (for reports: a custom arena chosen whose file the match doesn't load - the stock arena plays)
    if (const auto* c = static_cast<const CustomArena*>(g_redirected_custom); c && g_redirected_file >= 0) {
      if (as != g_redirected_file && svr2011::ArenaNumberFor(g_redirected_file) != as)
        REXLOG_WARN("[svr2011] arena mods: {} chosen (arena {} plays as {}), but the match is in arena {} (as {}) - not it",
                    c->name, g_redirected_host, g_redirected_file, arena, as);
      else
        REXLOG_INFO("[svr2011] arena mods: the match plays {} (BG{:02})", c->name, g_redirected_file);
    }
  } else {
    REXLOG_INFO("[svr2011] arena mods: a match loads with no match screen (arena not checked)");
  }
  __imp__sub_82773F00(ctx, base);
}

REX_EXTERN(__imp__sub_826AEF10);
REX_HOOK_RAW(sub_826AEF10) {
  if (g_bpe_log) {
    const uint8_t* s = base + ctx.r3.u32;
    const uint32_t size = s[12] | s[13] << 8 | s[14] << 16 | uint32_t(s[15]) << 24;
    REXLOG_INFO("[svr2011] bpe: lr {:08X} src {:08X} dst {:08X} size {:X} end {:08X}", uint32_t(ctx.lr),
                ctx.r3.u32, ctx.r4.u32, size, ctx.r4.u32 + size);
  }
  const uint32_t src = ctx.r3.u32, dst = ctx.r4.u32;
  const uint8_t* s = base + src;
  const uint32_t size = s[12] | s[13] << 8 | s[14] << 16 | uint32_t(s[15]) << 24;
  __imp__sub_826AEF10(ctx, base);
  if (dst && size) SwapDecoded(base + dst, size);
  if (dst && size) CardDecoded(base + dst, size);
  if (dst && size) svr2011::BackgroundsDecoded(base + dst, size);  // (custom menu pictures: backgrounds_page.h)
  if (dst && size) {  // (an arena's light bank: ARENA BRIGHTNESS, the arena mod's light keys - arena_lights.h)
    const auto* chosen = static_cast<const CustomArena*>(g_redirected_custom);
    svr2011::ArenaLightsDecoded(base + dst, size, chosen ? chosen->manifest : std::string());
  }
}

// The entrance's title card (show logo + city): sub_8277E928 (card state +196 == 4) picks the city's string by the
// arena's number - the base arena's for a custom one (King of the Ring '98 on Hell in a Cell said "NEWARK, NJ") -
// and copies it to card +2248 (97 bytes). The chosen arena's manifest city= is written there instead.
REX_EXTERN(__imp__sub_8277E928);
REX_HOOK_RAW(sub_8277E928) {
  const uint32_t card = ctx.r3.u32;
  const uint8_t* s = base + card + 196;
  const bool city_card = (uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | s[3]) == 4;
  __imp__sub_8277E928(ctx, base);
  const auto* c = static_cast<const CustomArena*>(g_redirected_custom);
  if (!city_card) return;
  static int shown = 0;
  if (shown++ < 4) REXLOG_INFO("[svr2011] arena mods: title card shown{}", c ? " (" + c->id + ")" : std::string());
  if (!c) return;
  std::string city;
  for (size_t at = 0; at < c->manifest.size();) {
    size_t end = c->manifest.find('\n', at);
    if (end == std::string::npos) end = c->manifest.size();
    if (c->manifest.compare(at, 5, "city=") == 0) city = c->manifest.substr(at + 5, end - at - 5);
    at = end + 1;
  }
  while (!city.empty() && (city.back() == '\r' || city.back() == ' ' || city.back() == '\t')) city.pop_back();
  if (city.empty()) return;
  city.resize(std::min<size_t>(city.size(), 96));
  std::memcpy(base + card + 2248, city.c_str(), city.size() + 1);
  static int logged = 0;
  if (logged++ < 4) REXLOG_INFO("[svr2011] arena mods: title card city '{}' ({})", city, c->id);
}

// Does a file exist (r3 path): a custom arena's VS theme instead of the one
// the game would pick (see SetDecodeSwaps).
REX_EXTERN(__imp__sub_826A17D0);
REX_HOOK_RAW(sub_826A17D0) {
  if (ctx.r3.u32) {
    char* path = reinterpret_cast<char*>(base + ctx.r3.u32);
    ForceTheme(path);
    // (the VS screen opening: its theme - test scripts wait for this line, vs_styles.h)
    static int logged = 0;
    if (!std::strncmp(path, "/MENU/M", 7) && logged++ < 100) REXLOG_INFO("[svr2011] VS screen: theme {:.16}", path);
    static const bool evp_log = std::getenv("SVR2011_TEST_EVP_LOG") != nullptr;
    if (evp_log && !std::strncmp(path, "/EV", 3))
      REXLOG_INFO("evp: {:.20} (lr {:08X})", path, uint32_t(ctx.lr));
    // An arena mod with its own VS theme (vs/theme.pac): "/MENU/M<nn>x" -> "/VA<k>/VSTx" (the same length), its group
    // in the VS styles' overlay pac. Decided here, as the screen opens: the game finds the file by this name now
    // (the boot-time names are only the game's own).
    const auto t = CurrentDecodeSwaps();
    const CustomArena* own = nullptr;
    for (const auto& c : g_customs)
      if (t && !t->arena.empty() && c.id == t->arena && c.theme_group.size() == 4) own = &c;
    if (own && !std::strncmp(path, "/MENU/M", 7) && std::isdigit(uint8_t(path[7])) && std::isdigit(uint8_t(path[8])) &&
        (path[9] == 'L' || path[9] == 'I' || path[9] == 'F') && !path[10]) {
      std::memcpy(path + 1, own->theme_group.data(), 4);
      std::memcpy(path + 6, "VST", 3);
      static int told = 0;
      if (told++ < 30) REXLOG_INFO("[svr2011] arena mods: VS screen -> the arena mod's own theme {}", path);
    }
  }
  __imp__sub_826A17D0(ctx, base);
}

// The grid widget update: arena select pages.
REX_EXTERN(__imp__sub_823D4FC0);
REX_EXTERN(__imp__sub_82521EF8);
REX_HOOK_RAW(sub_823D4FC0) {
  const uint32_t w = ctx.r3.u32;
  uint8_t* p = base + w;
  auto rd = [&](uint32_t o) { return uint32_t(p[o]) << 24 | p[o + 1] << 16 | p[o + 2] << 8 | p[o + 3]; };
  const bool online_grid = rd(0) == kOnlineSelectVtable && ReadOnlineTiles(base, w - 28);
  if (g_customs.empty() || (rd(0) != kSelectVtable && !online_grid)) {
    // Test aid (SVR2011_TEST_LOBBY_LOG): other 5-column grids (the online
    // CREATE SESSION's arena select?), once per vtable.
    static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
    if (log) {
      static std::set<uint32_t> seen;
      if (seen.insert(rd(0)).second)
        REXLOG_INFO("[svr2011] arena mods: a grid with vtable {:08X} ({} x {}, lr {:08X})", rd(0), rd(48), rd(32), uint32_t(ctx.lr));
    }
    __imp__sub_823D4FC0(ctx, base);
    return;
  }
  // The screen opened: page 1, find the banners. (Also when the widget is at
  // the same address as last time - a gap in its updates: the game reopened
  // the screen and loaded its banners anew, elsewhere; the old page and
  // places stayed, the banners were written to stale memory - stock tiles on
  // the wrong arenas, "2 / 2" over page 1's pictures; 2.0.4 report.) A gap
  // alone is not a reopen while the page drawn is still in the banners: a
  // hitch or the pick's own pause (A on a page 2 tile) then sent the screen
  // back to page 1 and the stock arena was played (2.0.5 player reports).
  if (w != g_select_widget || (NowMs() - g_select_seen.load() > 300 && !PageStillShown(base))) {
    if (w != g_select_widget || g_page > 0)
      REXLOG_INFO("[svr2011] arena select: screen (re)opened (widget {:08X} was {:08X}, {} ms since the last update, page {})",
                  w, g_select_widget, NowMs() - g_select_seen.load(), g_page + 1);
    g_select_widget = w;
    g_page = 0;
    // (the online screen's tile order: the pages laid out for it)
    {
      const Tile* t = online_grid ? g_online_tiles : kTiles;
      g_tiles = t;
      LayOutPages();  // (each custom arena on its base's tile, in this screen's order)
      if (online_grid) REXLOG_INFO("[svr2011] arena select: the online screen ({} pages)", Pages());
    }
    if (g_banners.empty()) LoadBanners();
    LocateBanners();
    g_locate_tries = 0;
    g_located_at = NowMs();
    // A custom arena already chosen (the screen opened again from the match
    // screen): its page again, with it still played - the game puts the
    // cursor on the match's arena's tile. Going back to page 1 here
    // dropped the pick: a stock arena was played (2.0.5 player reports).
    g_committed_custom = g_redirected_custom;
    g_committed_host = g_redirected_host;
    const auto* chosen = static_cast<const CustomArena*>(g_redirected_custom);
    int page = 0;
    if (chosen)
      for (size_t pg = 0; pg < g_layout.size() && !page; ++pg)
        for (int k = 0; k < 20; ++k)
          if (g_layout[pg][k] == int(chosen - g_customs.data())) page = int(pg) + 1;
    if (page) {
      g_page = page;
      ShowPage(base);
      REXLOG_INFO("[svr2011] arena select: {} still chosen (page {})", chosen->name, page + 1);
    } else {
      FollowCursor(4, 0);  // (nothing redirected)
    }
  }
  // The online screen loads its banners after it opens (none in memory at
  // its first update - page 2 showed page 1's pictures): looked for again
  // every 500 ms for 15 s, the page drawn once they are there.
  if (online_grid && g_locate_tries < 30 && NowMs() - g_located_at > 500 &&
      std::none_of(g_banners.begin(), g_banners.end(), [](const Banner& bn) { return !bn.guest.empty(); })) {
    ++g_locate_tries;
    g_located_at = NowMs();
    LocateBanners();
    const bool found = std::any_of(g_banners.begin(), g_banners.end(), [](const Banner& bn) { return !bn.guest.empty(); });
    if (found && g_page > 0) ShowPage(base);
    if (!found && g_locate_tries == 30)
      REXLOG_WARN("[svr2011] arena select: the online screen's banners not found in 15 s (pages keep the game's pictures)");
  }
  g_select_seen = NowMs();
  // Test aid: SVR2011_TEST_SELECT_HITCH=<ms>: the grid stalls that long every
  // 2 s (a slow PC's hitches, which sent page 2 back to page 1).
  static const int hitch = [] { const char* v = std::getenv("SVR2011_TEST_SELECT_HITCH"); return v ? std::atoi(v) : 0; }();
  if (hitch > 0) {
    static int64_t last = 0;
    if (NowMs() - last > 2000) {
      last = NowMs();
      std::this_thread::sleep_for(std::chrono::milliseconds(hitch));
    }
  }
  __imp__sub_823D4FC0(ctx, base);
  // (the cursor: the offline grid's row and column; the online screen's tile)
  const uint32_t now_at = uint32_t(g_online_tile.load());
  const uint32_t new_col = online_grid ? now_at % 5 : rd(44), new_row = online_grid ? now_at / 5 : rd(28);
  // Pages turn with any player's LB / RB (or LT / RT - pads, the touch MENU layout's buttons, keys 1 / 3: read
  // with the game's pad reads). LEFT / RIGHT past an edge wrap the cursor within its row, on the same page (the
  // game's own wrap): turning the page there put a stock tile under the cursor by accident (a player's
  // report, 23 Oct - the match was then in a stock arena).
  static uint32_t held = 0;
  uint32_t shoulders = 0;
  for (const auto& s : g_shoulders) shoulders |= s.load();
  const uint32_t pressed = shoulders & ~held;
  held = shoulders;
  int page = g_page;
  // a page button turns one page per press: a press within 250 ms of the last
  // turn is the same press read again (a hitch's repeated pad reads turned
  // page 2 straight back to page 1)
  static int64_t last_turn = 0;
  const bool button_ok = NowMs() - last_turn > 250;
  const char* why = nullptr;
  if (button_ok && (pressed & 0xA00)) why = pressed & 0x800 ? "RT" : "RB";
  else if (button_ok && (pressed & 0x500)) why = pressed & 0x400 ? "LT" : "LB";
  if (why) page = (why[0] == 'r' || why[0] == 'R' ? g_page + 1 : g_page + Pages() - 1) % Pages();
  // A picks the tile under the cursor (kept); B leaves the screen with the
  // arena chosen before, so a custom arena chosen then is played again (the
  // cursor moving had pointed its host back at the stock file)
  if (pressed & 0x1000) {
    g_committed_custom = g_redirected_custom;
    g_committed_host = g_redirected_host;
    // (for reports: the tile the cursor was on as seen here - the match's own arena follows at its load)
    if (new_row < 4 && new_col < 5)
      REXLOG_INFO("[svr2011] arena select: A on page {} row {} col {} - {} (tile arena {})", g_page + 1, new_row, new_col,
                  g_redirected_custom ? static_cast<const CustomArena*>(g_redirected_custom)->name : std::string("stock"),
                  g_page == 0 ? g_tiles[new_row * 5 + new_col].arena : g_redirected_host);
  } else if (pressed & 0x2000) {
    if (g_committed_custom != g_redirected_custom || g_committed_host != g_redirected_host) {
      const auto* c = static_cast<const CustomArena*>(g_committed_custom);
      PointAt(c && g_committed_host >= 0 ? c : nullptr, g_committed_host);
      REXLOG_INFO("[svr2011] arena select: cancelled, {} kept",
                  g_redirected_custom ? static_cast<const CustomArena*>(g_redirected_custom)->name : std::string("the stock arena"));
    }
    return;  // (the screen is closing: the cursor no longer picks)
  }
  if (page != g_page) {
    last_turn = NowMs();
    g_page = page;
    REXLOG_INFO("[svr2011] arena select: page turned ({})", why);
    ShowPage(base);
    // (the online screen's title over the grid - the tile's arena name - is
    // set by its cursor: set again, for the name on this page)
    if (online_grid && now_at < 20) {
      PPCContext c = ctx;
      c.r3.u64 = w - 28;
      c.r4.u64 = now_at;
      __imp__sub_82521EF8(c, base);
    }
  }
  FollowCursor(int(new_row), int(new_col));
}

// The online arena select's cursor (sub_82521EF8(screen, tile): that tile's
// arena becomes the screen's).
REX_EXTERN(__imp__sub_82521EF8);
REX_HOOK_RAW(sub_82521EF8) {
  g_online_tile = int(ctx.r4.u32 < 25 ? ctx.r4.u32 : 0);
  __imp__sub_82521EF8(ctx, base);
}

namespace svr2011 {

uint32_t ArenaSelectString(uint32_t caller) {
  if (caller != kOnlineTitleCaller || g_tiles != g_online_tiles || NowMs() - g_select_seen.load() > 500) return 0;
  const CustomArena* c = CustomAt(g_online_tile.load() % 20);
  if (!c || !g_memory) return 0;
  constexpr uint32_t kSize = 128;
  if (!g_title_text) g_title_text = g_memory->SystemHeapAlloc(kSize);
  if (!g_title_text) return 0;
  char* t = g_memory->TranslateVirtual<char*>(g_title_text);
  size_t n = 0;
  for (; n + 1 < kSize && n < c->name.size(); ++n) t[n] = char(std::toupper(uint8_t(c->name[n])));
  t[n] = 0;
  return g_title_text;
}

// Backstage areas (Mods/Backstage/<id>: manifest type=backstage, area=<0-6>,
// arena.pac = bg78 with that room rebuilt; Mod Maker): bg78 holds all seven
// backstage brawl rooms, so the mod's file is bg78 from then on. One at a
// time: a later one (by folder name) wins.
// A mod with row=<label> in its manifest is an area of its own instead: it
// gets a menu row (BACKSTAGE, after its room's row; match_types.cpp) and its
// bg78 plays only in matches started from that row (UseBackstageRow).
std::vector<BackstageRow> g_backstage_rows;

// A weapon in a backstage row's load_weapons= / weapons=: a model number, or a weapon mod's folder (weapon_mods.h,
// pool=backstage; 0: none such).
int BackstageWeaponModel(std::string token, const std::string& row) {
  while (!token.empty() && token.front() == ' ') token.erase(0, 1);
  while (!token.empty() && token.back() == ' ') token.pop_back();
  if (token.empty()) return 0;
  if (std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); })) return std::atoi(token.c_str());
  for (const auto& m : svr2011::WeaponMods())
    if (m.folder == token && m.backstage) return m.id;
  REXLOG_WARN("[svr2011] arena mods: backstage row '{}' - weapon '{}' is no weapon mod for backstage areas: left out", row,
              token);
  return 0;
}

void LoadBackstage() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  if (svr2011::BackstageModsOff()) {  // (no_mods.h: the stock backstage only - each installed one named)
    for (const auto& e : fs::directory_iterator(g_game / "Mods" / "Backstage", ec))
      if (e.is_directory() && fs::exists(e.path() / "arena.pac", ec))
        REXLOG_INFO("[svr2011] arena mods: backstage mod '{}' skipped - backstage mods are off, the stock backstage "
                    "plays", e.path().filename().string());
    return;
  }
  std::vector<fs::path> found;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(g_game / "Mods" / "Backstage", ec))
    if (e.is_directory() && fs::exists(e.path() / "arena.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    BackstageRow r;
    r.file = fs::relative(d / "arena.pac", g_game, ec).generic_string();
    r.dir = d.string();  // (full path)
    if (FILE* t = std::fopen((d / "manifest.txt").string().c_str(), "rb")) {
      char line[4096];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("row=", 0) == 0) r.label = l.substr(4);
        if (l.rfind("area=", 0) == 0) r.area = std::atoi(l.c_str() + 5);
        if (l.rfind("gimmick=", 0) == 0) r.gimmick = l.substr(8);
        if (l.rfind("camera=", 0) == 0) r.camera = float(std::atof(l.c_str() + 7));
        if (l.rfind("camera_height=", 0) == 0) r.camera_height = float(std::atof(l.c_str() + 14));
        if (l.rfind("box=", 0) == 0)
          r.has_box = std::sscanf(l.c_str() + 4, "%f,%f,%f,%f", &r.box[0], &r.box[1], &r.box[2], &r.box[3]) == 4;
        if (l.rfind("hctp=", 0) == 0) r.hctp = std::atoi(l.c_str() + 5);
        if (l.rfind("load_weapons=", 0) == 0) {
          std::string v = l.substr(13);
          for (size_t at = 0; at < v.size();) {
            const size_t end = std::min(v.find(',', at), v.size());
            if (const int m = BackstageWeaponModel(v.substr(at, end - at), r.label); m > 0) r.load_weapons.push_back(m);
            at = end + 1;
          }
        }
        if (l.rfind("start=", 0) == 0) {
          size_t at = 6;
          while (at < l.size()) {
            const size_t end = std::min(l.find(';', at), l.size());
            BackstageRow::Start s{};
            if (std::sscanf(l.substr(at, end - at).c_str(), "%f,%f,%f,%f", &s.x, &s.z, &s.facing, &s.y) >= 3)
              r.starts.push_back(s);
            at = end + 1;
          }
        }
        if (l == "weapons=none") r.no_weapons = true;
        if (l.rfind("weapons=", 0) == 0) {
          size_t at = 8;
          while (at < l.size()) {
            const size_t end = std::min(l.find(';', at), l.size());
            BackstageRow::Weapon w{};
            const std::string item = l.substr(at, end - at);
            const size_t comma = item.find(',');
            w.model = BackstageWeaponModel(item.substr(0, comma), r.label);
            if (comma != std::string::npos &&
                std::sscanf(item.c_str() + comma + 1, "%f,%f,%f,%f", &w.x, &w.y, &w.z, &w.yaw) == 4 && w.model > 0 &&
                w.model < 256)
              r.weapons.push_back(w);
            at = end + 1;
          }
        }
      }
      std::fclose(t);
    }
    if (r.label.empty()) {
      found.push_back(d);
    } else if (r.area >= 0 && r.area < 7 && g_backstage_rows.size() < 8) {
      REXLOG_INFO("[svr2011] arena mods: backstage area '{}' ({}; its own row after room {})", r.label, r.file, r.area);
      // Its own gimmicks (cars and their hot spots): gimmick.pac, a small pac
      // with GMGB/<gimmick> (4 chars, e.g. 78P8), mounted as one more pac
      // (move_packs.cpp) and played in place of the room's GMGB package in its
      // matches (sub_824B0090 below). One mod per entry brings it - the first
      // by folder name, as move_packs.cpp mounts them (HCTP rooms and worlds
      // each have their own entry).
      static std::set<std::string> have_gm;
      if (r.gimmick.size() == 4 && fs::exists(d / "gimmick.pac", ec) && have_gm.insert(r.gimmick).second) {
        REXLOG_INFO("[svr2011] arena mods: {} plays its gimmicks GMGB/{}", r.label, r.gimmick);
      } else {
        if (!r.gimmick.empty()) REXLOG_WARN("[svr2011] arena mods: {}: gimmicks ignored", r.label);
        r.gimmick.clear();
      }
      g_backstage_rows.push_back(std::move(r));
    }
  }
  if (found.empty()) return;
  std::sort(found.begin(), found.end());
  if (found.size() > 1)
    REXLOG_WARN("[svr2011] arena mods: {} backstage areas installed, only {} plays (one at a time)", found.size(),
                found.back().filename().string());
  const fs::path rel = fs::relative(found.back() / "arena.pac", g_game, ec);
  svr2011::SetArenaDefault(78, rel.generic_string());
}

const std::vector<BackstageRow>& BackstageRows() { return g_backstage_rows; }

int g_active_row = -1;

void UseBackstageRow(int i) {
  if (i >= int(g_backstage_rows.size())) i = -1;
  if (i == g_active_row) return;
  g_active_row = i;
  RedirectArena(78, i < 0 ? "" : g_backstage_rows[size_t(i)].file);
}

void SetMatchLoading(bool loading) { g_match_loading = loading; }
void SetSuperstarLoading(const std::string& file) { StarLoading(file); }

std::string ArenaModInUse(int* tile) {
  // (the arena redirected is the one the match loads - the online screen's A
  // closes it before a commit is seen)
  const auto* c = static_cast<const CustomArena*>(g_redirected_custom);
  if (!c || g_redirected_host < 0) return "";
  if (tile) *tile = g_redirected_host;
  return c->id;
}

std::string ArenaModManifest() {
  const auto* c = static_cast<const CustomArena*>(g_redirected_custom);
  return c && g_redirected_host >= 0 ? c->manifest : std::string();
}

// (tile: the arena of the host's tile - the session's arena; this game finds the mod's base itself, so the pages'
// order may differ between the two games)
bool UseArenaMod(const std::string& folder, int tile) {
  static int used = -1;  // (the tile this game plays another's arena on)
  static std::string used_folder;
  if (folder == used_folder && (folder.empty() ? used < 0 : tile == used)) return true;  // (as it is)
  used_folder = folder;
  if (folder.empty() || tile < 0 || tile >= 100) {
    if (used >= 0) {
      PointAt(nullptr, -1);
      REXLOG_INFO("[svr2011] arena mods: the online session's custom arena off");
    }
    used = -1;
    return true;
  }
  for (const auto& c : g_customs) {
    if (c.id != folder) continue;
    PointAt(&c, tile);
    if (used != tile)
      REXLOG_INFO("[svr2011] arena mods: the online session's custom arena {} on arena {} (plays as arena {})", c.name,
                  tile, g_redirected_file);
    used = tile;
    return true;
  }
  return false;
}

int ArenaNumberFor(int arena) {
  if (arena >= 0 && arena < 100 && g_plays_as[arena]) arena = g_plays_as[arena] - 1;
  return arena >= 0 && arena < 100 && g_number_for[arena] ? g_number_for[arena] : arena;
}

// (The match record copies the screen's arena when the match screen opens - sub_82773F00 - and again from this read
// once one is picked: a WWE Universe match picked on page 2 played the tile's stock arena without it.)
namespace {
void ScreenPlaysAs(uint8_t* base, uint32_t screen) {
  constexpr uint32_t kScreenArena = 244;
  if (!screen) return;
  uint8_t* q = base + screen + kScreenArena;
  const uint32_t arena = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  if (arena >= 100 || !g_plays_as[arena]) return;
  const uint32_t as = uint32_t(g_plays_as[arena] - 1);
  q[0] = uint8_t(as >> 24), q[1] = uint8_t(as >> 16), q[2] = uint8_t(as >> 8), q[3] = uint8_t(as);
  REXLOG_INFO("[svr2011] arena mods: the match screen's arena {} plays as arena {}", arena, as);
}
}  // namespace

void ScreenArenaRead(uint8_t* base, uint32_t screen) {
  LiveArenaPlaysAs(base);
  g_loads_since_pick = 0;
  ScreenPlaysAs(base, screen);
}

// (A match's load, sub_823EEB98 - before anything of it reads the arena: the INFERNO's fire set up at the load's
// start with the tile's own number still in the live settings - a mod on another arena's tile had no ringside fire
// and the temperature never rose, report 7a6b96; the screen read that turns it into the plays-as arena came ~2 s
// later.)
void ArenaPlaysAsAtLoad(uint8_t* base) {
  LiveArenaPlaysAs(base);
  const uint8_t* m = base + 0x82EDEB48;  // (the match screen)
  ScreenPlaysAs(base, uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3]);
}

void ArenaSelectPad(uint32_t user, uint16_t buttons) {
  if (user < 8) g_shoulders[user] = buttons & 0x3F00;  // (+ A 0x1000, B 0x2000: pick / cancel)
}

void SetArenaDefault(int arena, const std::string& relative_file) {
  if (arena < 0 || arena >= 100) return;
  g_default[arena] = relative_file;
  RedirectArena(arena, relative_file);
}

uint32_t ServedStageUnits(const char* entry) {
  if (std::strncmp(entry, "/STG/", 5) != 0 || std::strlen(entry) != 9) return 0;
  std::lock_guard lock(g_stg_mutex);
  for (const auto& list : g_served_stg)
    for (const auto& e : list)
      if (!std::memcmp(e.name, entry + 5, 4)) return e.units;
  return 0;
}

void RedirectArena(int arena, const std::string& file) {
  if (!g_fs || arena < 0 || arena >= 100) return;
  const std::string& relative_file = file.empty() ? g_default[arena] : file;
  std::lock_guard lock(g_mutex);
  if (!PrepareOverlay()) return;
  char name[16];
  std::snprintf(name, sizeof name, "BG%02d.PAC", arena);
  const fs::path source = relative_file.empty() ? g_game / "pac" / "bg" / name : g_game / fs::u8path(relative_file);
  std::error_code ec;
  if (!fs::exists(source, ec)) {
    REXLOG_WARN("[svr2011] arena mods: {} not found", source.string());
    return;
  }
  // The file's stage entry (STG/<nnnn>) is looked up by the arena's number
  // when the pacs are mounted from their own tables (a move pack installed:
  // move_packs.cpp): an arena made for another slot (SmackDown 1999's
  // STG/0000 on BG01) loaded as an empty hall. Such a file is placed as a
  // copy with its stage entry renamed - only when no entry of the STG group
  // has the slot's name already, and only the stage (the group's one
  // all-digit name): bg78's group is T178 ... TD78 (texture variants) then
  // 0078, and renaming its first entry made two STG/0078 (the match load
  // crashed, every backstage mod).
  g_number_for[arena] = 0;
  char want[5];
  std::snprintf(want, sizeof want, "%04d", arena);
  char head[0x4000] = {};
  long stage_at = -1;  // (file offset of the entry's name to rename)
  if (!relative_file.empty()) {
    if (FILE* f = std::fopen(source.string().c_str(), "rb")) {
      const size_t got = std::fread(head, 1, sizeof head, f);
      std::fclose(f);
      if (got == sizeof head && !std::memcmp(head, "EPAC", 4)) {
        // TOC from 0x800: groups {type, u32 entries * 3, u32 0} each followed
        // by its entries {name[4], u32 sector, u32 size}
        size_t p = 0x800;
        while (p + 12 <= sizeof head) {
          uint32_t words;
          std::memcpy(&words, head + p + 4, 4);  // (little-endian)
          if (!head[p] && !head[p + 1] && !head[p + 2] && !head[p + 3]) break;
          const bool stg = !std::memcmp(head + p, "STG ", 4);
          const size_t count = words / 3;
          p += 12;
          bool has = false;
          int digits_n = 0;
          long digits_at = -1;
          for (size_t e = 0; e < count && p + 12 <= sizeof head; ++e, p += 12) {
            if (!stg) continue;
            if (!std::memcmp(head + p, want, 4)) has = true;
            if (std::isdigit(uint8_t(head[p])) && std::isdigit(uint8_t(head[p + 1])) &&
                std::isdigit(uint8_t(head[p + 2])) && std::isdigit(uint8_t(head[p + 3])))
              ++digits_n, digits_at = long(p);
          }
          if (stg && !has && digits_n == 1) stage_at = digits_at;
          if (stg) break;
        }
      }
    }
  }
  {  // the stage entries' sizes against the shipped file's (g_served_stg)
    std::vector<StgEntry> differ;
    if (!relative_file.empty()) {
      std::vector<char> stock(0x4000);
      size_t got = 0;
      if (FILE* f = std::fopen((g_game / "pac" / "bg" / name).string().c_str(), "rb")) {
        got = std::fread(stock.data(), 1, stock.size(), f);
        std::fclose(f);
      }
      const auto shipped = StgEntries(stock.data(), got);
      for (auto [e, at] : StgEntries(head, sizeof head)) {
        if (long(at) == stage_at) std::memcpy(e.name, want, 4);  // (renamed into the slot below)
        for (const auto& [s, s_at] : shipped) {
          if (std::memcmp(s.name, e.name, 4) || s.units == e.units) continue;
          if (s.sector != e.sector)
            REXLOG_WARN("[svr2011] arena mods: {} STG/{} is at sector {}, the shipped file's at {}: the game reads it "
                        "there - keep the stage entries in the shipped order (a bigger entry last)",
                        relative_file, e.name, e.sector, s.sector);
          else
            differ.push_back(e);
        }
      }
      for (const auto& e : differ)
        REXLOG_INFO("[svr2011] arena mods: {} STG/{} is {} bytes (the shipped BG{:02}'s {}): read at its own size",
                    relative_file, e.name, uint64_t(e.units) << 8, arena, [&] {
                      for (const auto& [s, s_at] : shipped)
                        if (!std::memcmp(s.name, e.name, 4)) return uint64_t(s.units) << 8;
                      return uint64_t(0);
                    }());
    }
    std::lock_guard stg_lock(g_stg_mutex);
    g_served_stg[arena] = std::move(differ);
  }
  // The game's own practice ring as it is (the Practice Arena mod: bg29 on
  // SummerSlam's tile): not renamed into the slot. The game draws arena 29
  // its own way, so in another arena's slot (BG04) the match cameras showed
  // black beyond the ring (its floor and stands culled: the per-arena
  // visibility data and paths are chosen by the arena number). The slot keeps
  // its own file and a match picked there plays as arena 29 (the match screen's
  // arena, set as the match record copies it: sub_82773F00 below).
  if (stage_at >= 0 && !std::memcmp(head + stage_at, "0029", 4)) {
    const fs::path stock = g_game / "pac" / "bg" / "BG29.PAC";
    std::error_code e1, e2;
    bool same = fs::file_size(source, e1) == fs::file_size(stock, e2) && !e1 && !e2;
    if (same) {
      std::ifstream a(source, std::ios::binary), b(stock, std::ios::binary);
      std::vector<char> x(1 << 20), y(1 << 20);
      while (same && a && b) {
        a.read(x.data(), long(x.size()));
        b.read(y.data(), long(y.size()));
        same = a.gcount() == b.gcount() && !std::memcmp(x.data(), y.data(), size_t(a.gcount()));
      }
    }
    if (same) {
      if (!Place(g_game / "pac" / "bg" / name, name)) return;
      g_redirect[arena] = relative_file;
      g_number_for[arena] = 29;
      REXLOG_INFO("[svr2011] arena mods: BG{:02} -> {} (the stock practice ring: a match there plays as arena 29)", arena,
                  relative_file);
      return;
    }
  }
  const bool renamed = stage_at >= 0;
  if (renamed) {
    std::error_code ec2;
    const fs::path dst = g_overlay / name;
    // The entry may be a hard link to the original (pacg): it must be gone
    // before the copy - an overwrite or the rename below would write through
    // it into the game's own file (BG06.PAC was changed so on 2026-10-06).
    fs::remove(dst, ec2);
    if (fs::exists(dst, ec2)) {
      REXLOG_WARN("[svr2011] arena mods: cannot replace {}", name);
      return;
    }
    fs::copy_file(source, dst, fs::copy_options::none, ec2);
    FILE* f = ec2 ? nullptr : std::fopen(dst.string().c_str(), "r+b");
    if (!f) {
      REXLOG_WARN("[svr2011] arena mods: cannot place {} ({})", name, ec2.message());
      return;
    }
    std::fseek(f, stage_at, SEEK_SET);
    std::fwrite(want, 1, 4, f);
    std::fclose(f);
    Refresh(name);
    REXLOG_INFO("[svr2011] arena mods: {} stage STG/{} renamed STG/{} for BG{:02}", relative_file,
                std::string(head + stage_at, 4), want, arena);
  } else if (!Place(source, name)) {
    return;
  }
  g_redirect[arena] = relative_file;
  REXLOG_INFO("[svr2011] arena mods: BG{:02} -> {}", arena, relative_file.empty() ? "original" : relative_file);
}

// Arena memory: the game splits one block (physical 0x03A00000-0x0F000000)
// into 54 heaps from a table of (heap, KB) pairs at 0x82DAC2F8, read once at
// start-up by sub_8258ED50 -> sub_8269AD08. Heap 16 (20 MB, its size at
// 0x82DAC37C) holds the loaded arena file beside other stage data: an arena
// file much bigger than shipped doesn't fit (the load retries forever). Heap
// 53 is the rest of the block (25 MB). Experiment (SVR2011_ARENA_HEAP_EXTRA_MB):
// moving room from 53 to 16 did not let a bigger arena load - heap 16 is
// shared with menus and other stage data, which then failed instead - so
// the default is off and custom arenas keep their slot's size.
constexpr uint32_t kHeapTable = 0x82DAC2F8, kHeap16Size = 0x82DAC37C;
uint32_t g_arena_heap_extra_kb = 0;

void GrowArenaHeap(rex::memory::Memory* memory) {
  if (const char* v = std::getenv("SVR2011_ARENA_HEAP_EXTRA_MB")) g_arena_heap_extra_kb = uint32_t(std::atoi(v)) * 1024;
  uint8_t* b = memory->virtual_membase();
  auto rd = [&](uint32_t a) { return uint32_t(b[a]) << 24 | b[a + 1] << 16 | b[a + 2] << 8 | b[a + 3]; };
  if (rd(kHeap16Size - 4) != 16 || rd(kHeap16Size) != 20480) {
    REXLOG_WARN("[svr2011] arena mods: heap table not as expected, arena heap left as is");
    return;
  }
  if (!g_arena_heap_extra_kb) return;
  if (auto* heap = memory->LookupHeap(kHeapTable))
    heap->Protect(kHeapTable & ~0xFFFu, 0x2000, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  const uint32_t kb = 20480 + g_arena_heap_extra_kb;
  b[kHeap16Size] = uint8_t(kb >> 24), b[kHeap16Size + 1] = uint8_t(kb >> 16), b[kHeap16Size + 2] = uint8_t(kb >> 8),
  b[kHeap16Size + 3] = uint8_t(kb);
  REXLOG_INFO("[svr2011] arena mods: arena heap {} MB (+{} MB)", kb / 1024, g_arena_heap_extra_kb / 1024);
}

void InstallArenaMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* fs) {
  g_memory = memory;
  GrowArenaHeap(memory);
  g_fs = fs;
  g_game = rex::filesystem::GetExecutableFolder();
  g_overlay = g_game / "Mods" / "ArenaOverlay";
  g_bpe_log = std::getenv("SVR2011_TEST_BPE_LOG") != nullptr;
  if (crowd::Off()) {
    // (the arenas come through the overlay from the start: their crowd-less copies)
    std::lock_guard lock(g_mutex);
    PrepareOverlay();
    REXLOG_INFO("[svr2011] crowd: off - arenas without their crowd");
  }
  LoadCustomArenas();
  // Test aid: SVR2011_TEST_ARENA_MOD=<custom arena folder> - it is picked from the start, on its base arena's tile
  // (SVR2011_TEST_MATCH arena=<that base> plays it); SVR2011_TEST_ARENA_TILE=<n> - on arena n's tile instead (as on
  // a page where it sits on another arena's tile: SVR2011_TEST_MATCH arena=<n>). Tile 0 crashes at the load with a mod
  // (FATAL call 0xA9AAAAAA, 2.1.8 too; a real pick on tile 0 plays) - use another tile.
  if (const char* v = std::getenv("SVR2011_TEST_ARENA_MOD"); v && *v) {
    const char* t = std::getenv("SVR2011_TEST_ARENA_TILE");
    for (const auto& c : g_customs)
      if (c.id == v) {
        PointAt(&c, t && *t ? std::atoi(t) : c.base_arena >= 0 ? c.base_arena : 1);
        REXLOG_INFO("[svr2011] arena mods: test - {} picked on arena {}'s tile (plays as arena {})", c.name,
                    g_redirected_host, g_redirected_file);
      }
  }
  LoadBackstage();
  LoadLoadingPictures();
  // Test aid: SVR2011_TEST_LOADING=<custom arena id>:<theme>: its loading and
  // VS screens from the start (as when it is chosen on its base arena's tile)
  if (const char* v = std::getenv("SVR2011_TEST_LOADING"); v && *v) {
    const std::string sv = v;
    const size_t colon = sv.find(':');
    for (const auto& c : g_customs)
      if (c.id == sv.substr(0, colon)) SetDecodeSwaps(&c, colon == std::string::npos ? 1 : std::atoi(sv.c_str() + colon + 1));
  }
  if (std::getenv("SVR2011_TEST_SCAN")) StartScan();
  // Test aid: SVR2011_TEST_ARENA_REDIRECT=<nn>=<file relative to the game folder> (a custom arena's arena.pac:
  // chosen as if picked on the arena select - its manifest's keys apply, e.g. the light keys)
  if (const char* v = std::getenv("SVR2011_TEST_ARENA_REDIRECT"); v && *v) {
    const std::string s = v;
    const size_t eq = s.find('=');
    if (eq != std::string::npos) {
      const std::string file = s.substr(eq + 1);
      RedirectArena(std::atoi(s.substr(0, eq).c_str()), file);
      for (const auto& c : g_customs)
        if (c.file == file) g_redirected_custom = &c, svr2011::SetRingRules(c.manifest);
    }
  }
  // Test aid: SVR2011_TEST_FILE_LINK=<path>=<path>;... (game folder relative,
  // e.g. PAC\M.PAC=Mods\PacOverlay\m.pac): file-level symbolic links.
  if (const char* v = std::getenv("SVR2011_TEST_FILE_LINK"); v && *v) {
    std::string s = v;
    for (size_t a = 0; a < s.size();) {
      size_t e = s.find(';', a);
      if (e == std::string::npos) e = s.size();
      const std::string item = s.substr(a, e - a);
      if (const size_t eq = item.find('='); eq != std::string::npos) {
        const std::string from = std::string(kGameDevice) + "\\" + item.substr(0, eq);
        const std::string to = std::string(kGameDevice) + "\\" + item.substr(eq + 1);
        fs->RegisterSymbolicLink(from, to);
        REXLOG_INFO("[svr2011] file link: {} -> {}", from, to);
      }
      a = e + 1;
    }
  }
}

}  // namespace svr2011

// -- The page label ---------------------------------------------------------

namespace {

// "<  2 / 3  >" above the arena grid (as the Paint Tool's), and on custom
// pages the name of the arena under the cursor beside it.
class ArenaPageLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit ArenaPageLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (g_customs.empty() || NowMs() - g_select_seen.load() > 250) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float x0 = (w - gw) * 0.5f, y0 = (h - gh) * 0.5f;
    const float scale = gh / 720.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    char text[16];
    std::snprintf(text, sizeof(text), "%d / %d", g_page + 1, Pages());
    const float size = 28.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 c(x0 + gw * 0.5f, y0 + gh * 0.075f);
    const float arrow = 11.0f * scale, gap = 26.0f * scale;
    const float half = ts.x * 0.5f + gap + arrow;
    const ImVec2 pad(16.0f * scale, 6.0f * scale);
    const ImVec2 a(c.x - half - pad.x, c.y - ts.y * 0.5f - pad.y);
    const ImVec2 b(c.x + half + pad.x, c.y + ts.y * 0.5f + pad.y);
    dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 210), 6.0f * scale);
    dl->AddRect(a, b, IM_COL32(220, 220, 225, 255), 6.0f * scale, 0, 2.0f * scale);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), white, text);
    const float lx = c.x - half, rx = c.x + half;
    dl->AddTriangleFilled(ImVec2(lx, c.y), ImVec2(lx + arrow, c.y - arrow), ImVec2(lx + arrow, c.y + arrow), white);
    dl->AddTriangleFilled(ImVec2(rx, c.y), ImVec2(rx - arrow, c.y + arrow), ImVec2(rx - arrow, c.y - arrow), white);
    // the custom arena under the cursor
    const int ci = g_cursor_custom.load();
    if (ci < 0 || ci >= int(g_customs.size())) return;
    const std::string& name = g_customs[ci].name;
    const float ns = 22.0f * scale;
    const ImVec2 nt = font->CalcTextSizeA(ns, FLT_MAX, 0.0f, name.c_str());
    // beside the page label (below the grid is the DOWNLOADED ARENAS bar)
    const ImVec2 nc(b.x + 14.0f * scale + pad.x + nt.x * 0.5f, c.y);
    const ImVec2 na(nc.x - nt.x * 0.5f - pad.x, nc.y - nt.y * 0.5f - pad.y);
    const ImVec2 nb(nc.x + nt.x * 0.5f + pad.x, nc.y + nt.y * 0.5f + pad.y);
    dl->AddRectFilled(na, nb, IM_COL32(10, 12, 18, 220), 6.0f * scale);
    dl->AddText(font, ns, ImVec2(nc.x - nt.x * 0.5f, nc.y - nt.y * 0.5f), IM_COL32(255, 210, 60, 255), name.c_str());
  }
};

}  // namespace

namespace svr2011 {

void InstallArenaModsOverlay(rex::ui::ImGuiDrawer* drawer) { new ArenaPageLabel(drawer); }

std::vector<std::pair<std::string, std::string>> ArenaThemePacs() {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& c : g_customs)
    if (!c.theme_pac.empty()) out.push_back({c.id, c.theme_pac});
  return out;
}

void SetArenaThemeGroup(const std::string& id, const std::string& group) {
  for (auto& c : g_customs)
    if (c.id == id) c.theme_group = group;
}

}  // namespace svr2011

// -- The pac list from another folder (move packs) ---------------------------
//
// With a pac overlay (move_packs.h: PacListFolder) the game reads its pac list
// (D:\pac\plist360.h - sub_826B9280(obj, path)) from that folder instead: a
// list there names other files for some pacs ("mods\pacoverlay\m.pac").
// (The pre-built directory GAME:\PLIST360.ARC is not used then: the pacs are
// mounted from their own tables, superstar_mods.cpp.)
namespace {
uint32_t TestPlistPath(uint32_t guest_path) {
  const std::string& f = svr2011::PacListFolder();
  const char* folder = f.c_str();
  if (f.empty() || !g_memory || !guest_path) return 0;
  const char* p = reinterpret_cast<const char*>(g_memory->TranslateVirtual(guest_path));
  std::string s(p, strnlen(p, 260));
  const size_t slash = s.rfind('\\');
  const std::string name = Upper(slash == std::string::npos ? s : s.substr(slash + 1));
  if (name != "PLIST360.H" && name != "PLIST360_4X3.H" && name != "PLIST360.ARC" && name != "PLIST360_4X3.ARC")
    return 0;
  const std::string to = (s.rfind("GAME:", 0) == 0 ? std::string("GAME:\\") : std::string("D:\\")) + folder + "\\" +
                         (slash == std::string::npos ? s : s.substr(slash + 1));
  static std::vector<std::pair<std::string, uint32_t>> made;
  for (const auto& m : made)
    if (m.first == to) return m.second;
  const uint32_t g = g_memory->SystemHeapAlloc(uint32_t(to.size() + 1));
  if (!g) return 0;
  std::memcpy(g_memory->TranslateVirtual(g), to.c_str(), to.size() + 1);
  made.push_back({to, g});
  REXLOG_INFO("[svr2011] pac list: {} -> {}", s, to);
  return g;
}
}  // namespace
REX_EXTERN(__imp__sub_826B9280);
REX_HOOK_RAW(sub_826B9280) {
  if (const uint32_t g = TestPlistPath(ctx.r4.u32)) ctx.r4.u64 = g;
  __imp__sub_826B9280(ctx, base);
}
REX_EXTERN(__imp__sub_826A7108);
REX_HOOK_RAW(sub_826A7108) {
  if (const uint32_t g = TestPlistPath(ctx.r4.u32)) ctx.r4.u64 = g;
  __imp__sub_826A7108(ctx, base);
}

// A stage's gimmick package (cars, crates and their hot spots): sub_824B0090
// (kind, arena, out path) writes its name, "/GMGB/78PK" for the parking lot.
// In a match of a backstage row with its own gimmicks, the room's package
// name points at the mod's entry instead (same length; the group must stay
// GMGB - the loader unwraps the two-part package only for that group).
REX_EXTERN(__imp__sub_824B0090);
REX_HOOK_RAW(sub_824B0090) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_824B0090(ctx, base);
  const int k = svr2011::g_active_row;
  const auto& rows = svr2011::BackstageRows();
  if (k < 0 || size_t(k) >= rows.size() || rows[size_t(k)].gimmick.size() != 4 || !out) return;
  char* path = reinterpret_cast<char*>(base + out);
  if (std::strncmp(path, "/GMGB/78", 8) || std::strlen(path) != 10) return;
  const std::string before(path);
  std::memcpy(path + 6, rows[size_t(k)].gimmick.data(), 4);
  REXLOG_INFO("[svr2011] arena mods: gimmicks {} -> {}", before, path);
}

// Gimmick element type 25 (a prop the package places; init sub_824BB4E0,
// factory sub_824B8AB8): idx = int(param 1) - 11500 picks a prop in the
// table at [0x82E3BEC0] + (idx + 14652) * 4, written through without a
// check. A package from another game (SvR 2008's parking lot) can name a
// prop 2011 doesn't have: an empty slot, a write at 0x24 - the element is
// skipped instead.
REX_EXTERN(__imp__sub_824BB4E0);
REX_HOOK_RAW(sub_824BB4E0) {
  const auto rd = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  const uint32_t p = ctx.r3.u32 ? rd(ctx.r3.u32 + 84) : 0;
  const uint32_t tbl = rd(0x82E3BEC0);
  if (p && tbl) {
    const uint32_t bits = rd(p + 4);
    float f;
    std::memcpy(&f, &bits, 4);
    const int idx = int(f) - 11500;
    if (idx < 0 || idx > 60 || !rd(tbl + uint32_t(idx + 14652) * 4)) {
      static int warned = 0;
      if (warned++ < 8) REXLOG_WARN("[svr2011] arena mods: gimmick prop {} not in the game - skipped", int(f));
      return;
    }
  }
  __imp__sub_824BB4E0(ctx, base);
}
