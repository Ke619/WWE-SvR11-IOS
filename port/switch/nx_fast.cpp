// Guest routines and file system calls done natively on the Switch, where every cycle counts.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

namespace {

// Guest addresses from 0xE0000000 up sit 4 KB further on in host memory (the recompiled
// code's REX_PHYS_HOST_OFFSET does the same).
constexpr uint32_t kHighView = 0xE0000000u;
inline uint8_t* Host(uint8_t* base, uint32_t address) { return base + address + (address >= kHighView ? 0x1000u : 0u); }
inline bool Straddles(uint32_t address, uint32_t size) { return address < kHighView && address + size > kHighView; }

}  // namespace

// The game's memcpy (8.5 KB of recompiled AltiVec, 6% of the game thread's busy time). It
// copies forwards and returns the destination, which stays in r3. Copies that overlap or
// cross into the high view stay with the game's own code.
REX_EXTERN(__imp__sub_8215A000);
REX_HOOK_RAW(sub_8215A000) {
  const uint32_t to = ctx.r3.u32, from = ctx.r4.u32, size = ctx.r5.u32;
  if (!size) return;
  const bool overlap = to < from + size && from < to + size;
  if (overlap || Straddles(to, size) || Straddles(from, size)) {
    __imp__sub_8215A000(ctx, base);
    return;
  }
  std::memcpy(Host(base, to), Host(base, from), size);
}

// One request for many entries, with their sizes. stat() is about five requests a file, and
// the game's folder holds thousands. False: not read this way (ListFiles then does it the
// slow way), and that includes a folder that lists as empty.
extern "C" bool NxListSdFolder(const char* path, void (*entry)(void*, const char*, bool, uint64_t), void* user) {
  if (path[0] != '/') return false;
  char clean[FS_MAX_PATH];
  const int length = std::snprintf(clean, sizeof(clean), "%s", path);
  if (length <= 0 || length >= static_cast<int>(sizeof(clean))) return false;
  for (int i = length - 1; i > 0 && clean[i] == '/'; i--) clean[i] = 0;
  FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs) return false;
  FsDir dir;
  const Result opened = fsFsOpenDirectory(fs, clean, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
  if (R_FAILED(opened)) {
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_WARN("[nx] fast listing: {} can't be opened ({:X})", clean, opened);
    }
    return false;
  }
  constexpr s64 kBatch = 64;
  auto* entries = static_cast<FsDirectoryEntry*>(std::malloc(sizeof(FsDirectoryEntry) * kBatch));
  bool ok = entries != nullptr;
  s64 total = 0;
  while (ok) {
    s64 got = 0;
    if (R_FAILED(fsDirRead(&dir, &got, kBatch, entries))) {
      ok = false;
      break;
    }
    if (got <= 0) break;
    for (s64 i = 0; i < got; i++) {
      entry(user, entries[i].name, entries[i].type == FsDirEntryType_Dir, static_cast<uint64_t>(entries[i].file_size));
    }
    total += got;
  }
  std::free(entries);
  fsDirClose(&dir);
  return ok && total > 0;
}
