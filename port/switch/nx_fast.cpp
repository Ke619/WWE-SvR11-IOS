// File system calls done natively on the Switch, where every request counts.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>

// The caller's saved register for a hook: src/caller_nv.h.
namespace svr2011 {
thread_local uint32_t t_caller_nv = 0;
}
void NxCallerNv(PPCRegister& r) { svr2011::t_caller_nv = r.u32; }

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
