// Crowd signs (arenas branch): new signs for the crowd to hold.
//
// The game's signs (scratchpad re_signs): audience.pac AUDE/BORD, a PACH
// whose entry 0 is a PACH of sign textures (a bundle with one DXT5
// "no01.dds", 64 x 128 or 64 x 64) by sign id, sorted (looked up by a binary
// search):
//   id*10 + 1..4   a character's signs - the roster record's +210 u16[4]
//                  says which a character's fans hold;
//   4001..4721     the Create-A-Superstar picker's signs;
//   5001..5151     the 16 general signs a match fills its free sign slots
//                  with (sub_821E9A48, hard-coded 500..515 * 10 + 1).
// A match shows at most 16 signs: the wrestlers' (from +210), then general
// ones. The match loads the whole bank (sub_821EA4E0(data, size) copies it);
// the Create-A-Superstar picker loads it on its own (sub_828DF0F0) and is
// tied to the game's 437 entries, so it keeps the original.
//
// Here:
//  - a character's own signs (superstar mods: sign1..4.dds) go in as
//    10000 + key*10 + 1..4 (the mod's key: whatever id it holds - superstar_mods.cpp);
//  - sign packs, <game>/Mods/Signs/<pack>/*.dds (a manifest.txt; a "disabled"
//    file turns one off), go in as 6001, 6011, ... and join the general
//    signs: the free slots of each match are drawn from the game's 16 and
//    these;
//  - the merged bank is handed to the match loader instead of the file's.
#include "crowd_signs.h"
#include "no_mods.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"
#include "../modmaker/svrfmt/bytes.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFirstPackSign = 6001, kLastSign = 9991;
constexpr uint32_t kSignTable = 0x82E36958, kSignCount = 0x82E369FC;  // 16 x {id, character}, the wrestlers' count
constexpr uint32_t kNoCharacter = 512;

std::map<uint32_t, fs::path> g_signs;  // sign id -> its DDS (ours)
std::vector<uint32_t> g_general;       // the general signs: the game's 16, then the packs'
uint32_t g_bank = 0, g_bank_size = 0;  // guest: the merged bank

using svrfmt::Rd32, svrfmt::Wr32;

// A sign's entry: a texture bundle with the DDS as "no01".
svrfmt::Bytes SignEntry(const fs::path& dds) {
  svrfmt::Bytes d;
  svrfmt::DdsInfo info;
  if (!svrfmt::ReadFile(dds.string(), d) || !svrfmt::DdsInfoOf(d, info)) return {};
  return svrfmt::BundleWrite({{"no01", "dds", std::move(d)}});
}

void LoadPacks() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "Signs";
  std::vector<fs::path> packs;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && !fs::exists(e.path() / "disabled", ec)) packs.push_back(e.path());
  std::sort(packs.begin(), packs.end());
  uint32_t id = kFirstPackSign;
  for (const auto& p : packs) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(p, ec))
      if (e.is_regular_file() && e.path().extension() == ".dds") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      if (id > kLastSign) break;
      g_signs[id] = f;
      g_general.push_back(id);
      id += 10;
    }
    REXLOG_INFO("[svr2011] crowd signs: pack {} ({} signs)", p.filename().string(), files.size());
  }
}

// audience.pac's AUDE/BORD with our signs in.
bool BuildBank(rex::memory::Memory* memory) {
  svrfmt::Bytes d;
  svrfmt::Epac e;
  const fs::path pac = rex::filesystem::GetExecutableFolder() / "pac" / "audience.pac";
  if (!svrfmt::ReadFile(pac.string(), d) || !svrfmt::EpacRead(d, e)) return false;
  const svrfmt::Bytes* bord = nullptr;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries)
      if (g.type == "AUDE" && en.name == "BORD") bord = &en.data;
  std::vector<svrfmt::PachEntry> outer, inner;
  if (!bord || !svrfmt::PachRead(*bord, outer) || outer.empty() ||
      !svrfmt::PachRead(svrfmt::Unpack(outer[0].data), inner))
    return false;
  std::map<uint32_t, svrfmt::Bytes> all;
  for (auto& x : inner) all[x.id] = std::move(x.data);
  int added = 0;
  for (const auto& [id, file] : g_signs) {
    svrfmt::Bytes entry = SignEntry(file);
    if (entry.empty()) {
      REXLOG_WARN("[svr2011] crowd signs: {} is not a DDS picture", file.string());
      continue;
    }
    all[id] = std::move(entry);
    ++added;
  }
  inner.clear();
  for (auto& [id, data] : all) inner.push_back({id, std::move(data)});  // (sorted: a map)
  outer[0].data = svrfmt::PachWrite(inner);
  const svrfmt::Bytes bank = svrfmt::PachWrite(outer);
  g_bank_size = uint32_t(bank.size());
  g_bank = memory->SystemHeapAlloc(g_bank_size, 0x100);
  if (!g_bank) return false;
  std::memcpy(memory->TranslateVirtual<uint8_t*>(g_bank), bank.data(), bank.size());
  REXLOG_INFO("[svr2011] crowd signs: {} added ({} in all, {} KB)", added, all.size(), g_bank_size >> 10);
  return true;
}

}  // namespace

namespace svr2011 {

void AddCharacterSigns(uint32_t first, const std::vector<fs::path>& dds) {
  for (size_t k = 0; k < dds.size() && k < 4; ++k) g_signs[first + 1 + uint32_t(k)] = dds[k];
}

void InstallCrowdSigns(rex::memory::Memory* memory) {
  for (uint32_t i = 500; i <= 515; ++i) g_general.push_back(i * 10 + 1);
  LoadPacks();
  if (g_signs.empty()) return;
  if (!BuildBank(memory)) {
    REXLOG_WARN("[svr2011] crowd signs: could not read pac/audience.pac");
    g_bank = 0;
  }
}

}  // namespace svr2011

// The match's sign bank arrives (sub_821EA4E0(data, size) copies it): ours
// instead.
REX_EXTERN(__imp__sub_821EA4E0);
REX_HOOK_RAW(sub_821EA4E0) {
  if (g_bank) {
    ctx.r3.u64 = g_bank;
    ctx.r4.u64 = g_bank_size;
  }
  __imp__sub_821EA4E0(ctx, base);
}

// The match's 16 signs (sub_821E9A48(pairs, n)): the wrestlers' n, then the
// general ones - drawn from the game's 16 and the packs'.
REX_EXTERN(__imp__sub_821E9A48);
REX_HOOK_RAW(sub_821E9A48) {
  __imp__sub_821E9A48(ctx, base);
  if (g_general.size() <= 16 && g_signs.empty()) return;
  static std::mt19937 rng{std::random_device{}()};
  std::vector<uint32_t> pool = g_general;
  std::shuffle(pool.begin(), pool.end(), rng);
  const uint32_t n = std::min<uint32_t>(Rd32(base + kSignCount), 16);
  for (uint32_t i = n, k = 0; i < 16 && k < pool.size(); ++i, ++k) {
    Wr32(base + kSignTable + i * 8, pool[k]);
    Wr32(base + kSignTable + i * 8 + 4, kNoCharacter);
  }
  std::string ids;
  for (uint32_t i = 0; i < 16; ++i) ids += (i ? " " : "") + std::to_string(Rd32(base + kSignTable + i * 8));
  REXLOG_INFO("[svr2011] crowd signs: this match's {}", ids);
}
