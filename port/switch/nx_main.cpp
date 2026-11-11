// Switch start-up: services and folders before main, and a crash report for faults.
#include <switch.h>

#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sys/reent.h>
#include <sys/stat.h>

#include "nx_readahead.h"
#include <unistd.h>

// The game's folder on the SD card (rex::filesystem::GetExecutableFolder).
// (no device name: std::filesystem only treats paths from the root as absolute)
#define SVR_GAME_DIR "/SvR2011"

namespace {

extern "C" void _start(void);

// Read-ahead for read-only files (nx_readahead.h).
extern "C" off_t __real__lseek_r(struct _reent*, int, off_t, int);
extern "C" _ssize_t __real__read_r(struct _reent*, int, void*, size_t);
static struct _reent* RaReent() { return _REENT; }
struct RaOps {
  static int64_t Seek(int fd, int64_t off, int whence) { return __real__lseek_r(RaReent(), fd, off, whence); }
  static long Read(int fd, void* p, size_t n) { return __real__read_r(RaReent(), fd, p, n); }
};
struct RaLock {
  Mutex m = 0;
  void lock() { mutexLock(&m); }
  void unlock() { mutexUnlock(&m); }
};
static nx_ra::Table<RaOps, RaLock> g_ra;

// Return addresses on the stack, nearest first (nm / addr2line on svr2011.elf name them).
void DumpStack(FILE* f, const void* from) {
  const u64 base = reinterpret_cast<u64>(&_start);
  const u64* sp = static_cast<const u64*>(from);
  MemoryInfo info{};
  u32 page = 0;
  fprintf(f, "  stack words inside the image:\n");
  if (R_FAILED(svcQueryMemory(&info, &page, reinterpret_cast<u64>(sp))) || !(info.perm & Perm_R)) {
    return;
  }
  const u64 end = info.addr + info.size;
  int shown = 0;
  for (int i = 0; i < 16384 && reinterpret_cast<u64>(sp + i + 1) <= end && shown < 48; i++) {
    if (sp[i] > base + 0x1000 && sp[i] < base + (1ull << 28)) {
      fprintf(f, "  +%llx\n", (unsigned long long)(sp[i] - base));
      shown++;
    }
  }
}

// An uncaught C++ exception: its text, where, then the normal way out. (The stack
// isn't unwound when nothing catches it, so the thrower's frames are still there.)
void OnTerminate() {
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_crash.txt", "a");
  if (f) {
    DumpStack(f, __builtin_frame_address(0));
    fprintf(f, "terminate: ");
    if (std::exception_ptr e = std::current_exception()) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& ex) {
        fprintf(f, "%s\n", ex.what());
      } catch (...) {
        fprintf(f, "unknown exception\n");
      }
    } else {
      fprintf(f, "no exception\n");
    }
    fclose(f);
  }
  abort();
}

}  // namespace

extern "C" {

// A full title's memory: the loader gives the whole heap.
u32 __nx_applet_type = AppletType_Application;

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void NxProfileStart(void);
void NxAbStart(void);

// Settings 2 to 3: 30 fps became the Switch's default. Only that line changes (the rest are the
// player's), once; GRAPHICS -> FRAME RATE can set 60 again afterwards.
static void DefaultTo30Fps() {
  std::string text, line;
  if (FILE* old = fopen(SVR_GAME_DIR "/svr2011.toml", "r")) {
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), old)) {
      line = buffer;
      if (line.rfind("# switch settings 2", 0) == 0) {
        line = "# switch settings 3\n";
      } else if (line.rfind("frame_rate", 0) == 0) {
        continue;
      }
      text += line;
    }
    fclose(old);
  }
  if (!text.empty() && text.back() != '\n') text += '\n';
  text += "frame_rate = 30\n";
  if (FILE* f = fopen(SVR_GAME_DIR "/svr2011.toml", "w")) {
    fputs(text.c_str(), f);
    fclose(f);
  }
}

// First settings, written once; after that the GRAPHICS page owns the file.
static void WriteSettings() {
  char first[64] = {};
  if (FILE* old = fopen(SVR_GAME_DIR "/svr2011.toml", "r")) {
    fgets(first, sizeof(first), old);
    fclose(old);
    if (strstr(first, "switch settings 3")) return;  // the player's, from now on
    if (strstr(first, "switch settings 2")) {
      DefaultTo30Fps();
      return;
    }
  }
  FILE* f = fopen(SVR_GAME_DIR "/svr2011.toml", "w");
  if (!f) return;
  fputs(
      "# switch settings 3\n"
      "gpu_plugin = \"xenos\"\n"
      "gpu_backend = \"vulkan\"\n"
      "native_renderer = \"main\"\n"
      "input_backend = \"sdl\"\n"
      "audio_backend = \"sdl\"\n"
      "resolution = \"720p\"\n"
      "resolution_scale = 1\n"
      "fullscreen = true\n"
      "vsync = true\n"
      "window_width = 1280\n"
      "window_height = 720\n"
      "frame_rate = 30\n"
      "mnk_mode = false\n"
      "p2p_port_mapping = false\n"
      "native_aa = 1\n"
      "native_2x_msaa = false\n"
      "native_max_scale = 1\n"
      "native_scale_effects = false\n"
      "native_record_thread = true\n"
      "native_compact_tables = true\n"
      "show_fps = true\n"
      "perf_log_seconds = 10\n",
      f);
  fclose(f);
}

bool NxInstallCheck();  // (nx_install_check.cpp)

// Video recording, so the console's capture (and SysDVR) can take the game's picture: the NSP's control.nacp says
// VideoCapture Manual, and the game hands the system its recording buffer (96 MB of the game's own memory) here,
// before the game takes the rest. Off unless "switch_recording = true" is in svr2011.toml: the game keeps the memory
// (for SysDVR or the console's own recording, turn it on). Returns what happened.
static const char* StartRecording() {
  bool on = false;
  if (FILE* f = fopen(SVR_GAME_DIR "/svr2011.toml", "r")) {
    char line[256];
    while (fgets(line, sizeof(line), f))
      if (!strncmp(line, "switch_recording", 16)) on = strstr(line, "true") != nullptr;
    fclose(f);
  }
  if (!on) return "off (switch_recording = true in svr2011.toml turns it on)";
  static char why[96];
  const Result rc = appletInitializeGamePlayRecording();
  if (R_FAILED(rc)) {
    snprintf(why, sizeof(why), "not available (0x%X)", rc);
    return why;
  }
  const Result on = appletSetGamePlayRecordingState(true);
  snprintf(why, sizeof(why), R_SUCCEEDED(on) ? "on (96 MB buffer)" : "buffer given, state refused (0x%X)", on);
  return why;
}

void userAppInit(void) {
  socketInitializeDefault();
  romfsMountFromCurrentProcess("romfs");  // (fails harmlessly without one)
  // Before anything reads the game's files: are they on the card? If not, a message instead of a crash.
  if (!NxInstallCheck()) exit(0);
  const char* recording = StartRecording();
  mkdir(SVR_GAME_DIR, 0777);
  mkdir(SVR_GAME_DIR "/logs", 0777);
  setenv("REX_APP_FOLDER", SVR_GAME_DIR, 1);
  setenv("HOME", SVR_GAME_DIR, 1);  // (the SDK asks for a user folder)
  setenv("SVR2011_TEST_TASK_LOG", "1", 1);  // (bring-up: which screens the loads spend their time on)
  chdir(SVR_GAME_DIR);
  // The driver's and the runtime's own messages, written as they come.
  if (freopen(SVR_GAME_DIR "/logs/nx_stderr.txt", "w", stderr)) {
    setvbuf(stderr, nullptr, _IOLBF, 1024);
  }
  std::set_terminate(OnTerminate);
  WriteSettings();
  NxProfileStart();
  NxAbStart();  // (clock readings in the log)
  // Start-up marks, flushed at once: this works even if logging never starts.
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_start.txt", "w");
  if (f) {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(f, "started: applet %d, total %llu MB, used %llu MB\n", appletGetAppletType(),
            (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
    fprintf(f, "video recording: %s\n", recording);
    fclose(f);
  }
}

void userAppExit(void) { socketExit(); }

// The game's files are read-only inside the NSP (romfs:/game); what the game writes
// goes to the SD card. A file on the card wins, so mods and small updates need no
// reinstall. Everything asks for /SvR2011/... or a relative path (the folder is the
// current one), which these wrappers look for on the card first, then in the NSP.
static bool InRomfs(const char* path, char* out, size_t size) {
  static const char kRoot[] = SVR_GAME_DIR "/";
  const char* rest = nullptr;
  if (!strncmp(path, kRoot, sizeof(kRoot) - 1)) {
    rest = path + sizeof(kRoot) - 1;
  } else if (!strcmp(path, SVR_GAME_DIR)) {
    rest = "";
  } else if (path[0] != '/' && !strchr(path, ':')) {
    rest = path;
  } else {
    return false;
  }
  while (rest[0] == '.' && rest[1] == '/') rest += 2;
  return snprintf(out, size, "romfs:/game/%s", rest) < static_cast<int>(size);
}

// A write to a folder that so far exists only in the NSP.
static void MakeParents(const char* path) {
  char dir[768];
  if (snprintf(dir, sizeof(dir), "%s", path) >= static_cast<int>(sizeof(dir))) return;
  for (char* c = dir + 1; *c; c++) {
    if (*c == '/') {
      *c = 0;
      mkdir(dir, 0777);
      *c = '/';
    }
  }
}

// Per file, for the log (bring-up): which files the small reads are, and the thread reading.
struct NxFileStat {
  char path[96];
  u64 ticks, bytes, calls;
};
NxFileStat g_nx_files[256];
volatile Handle g_nx_reader = 0;
int __real__open_r(struct _reent* r, const char* path, int flags, int mode);
int __wrap__open_r(struct _reent* r, const char* path, int flags, int mode) {
  const bool writes = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND));
  int fd = __real__open_r(r, path, flags, mode);
  if (fd < 0) {
    char other[768];
    if (r->_errno != ENOENT || !InRomfs(path, other, sizeof(other))) return fd;
    if (writes) {
      MakeParents(path);
      fd = __real__open_r(r, path, flags, mode);
    } else {
      fd = __real__open_r(r, other, flags, mode);
    }
  }
  if (fd >= 0) {
    if (fd < 256) {
      NxFileStat& file = g_nx_files[fd];
      file.ticks = file.bytes = file.calls = 0;
      snprintf(file.path, sizeof(file.path), "%s", strlen(path) < sizeof(file.path) ? path : path + strlen(path) - (sizeof(file.path) - 1));
    }
    if (writes) {
      g_ra.Wrote();  // (it may empty a file another handle is reading)
    } else {
      g_ra.Opened(fd);
    }
  }
  return fd;
}

int __real__close_r(struct _reent* r, int fd);
int __wrap__close_r(struct _reent* r, int fd) {
  g_ra.Closed(fd);
  return __real__close_r(r, fd);
}

off_t __wrap__lseek_r(struct _reent* r, int fd, off_t off, int whence) {
  if (g_ra.Tracked(fd)) return g_ra.Seek(fd, off, whence);
  return __real__lseek_r(r, fd, off, whence);
}

_ssize_t __real__write_r(struct _reent* r, int fd, const void* p, size_t n);
_ssize_t __wrap__write_r(struct _reent* r, int fd, const void* p, size_t n) {
  g_ra.Wrote();
  return __real__write_r(r, fd, p, n);
}

// The card reports file id 0 for every file, so std::filesystem took any two files for
// the same one. An id from the path tells them apart.
static void SetFileId(const char* path, struct stat* out) {
  u64 h = 1469598103934665603ull;
  for (const char* c = path; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ull;
  out->st_ino = h | 1;
}

// The card has no permissions and fchmod fails, which made std::filesystem::copy_file
// stop after creating an empty target: a save opened through its working copy read as
// empty ("Load Failed"), and the game then wrote a blank one.
int __wrap_fchmod(int, mode_t) { return 0; }

// How much time loading spends in file reads (nx_ab.cpp logs it).
volatile u64 g_nx_read_ticks = 0, g_nx_read_bytes = 0, g_nx_read_calls = 0;
_ssize_t __wrap__read_r(struct _reent* r, int fd, void* buf, size_t n) {
  const u64 start = armGetSystemTick();
  const _ssize_t got = g_ra.Tracked(fd) ? g_ra.Read(fd, buf, n) : __real__read_r(r, fd, buf, n);
  if (fd >= 0 && fd < 256) {
    NxFileStat& file = g_nx_files[fd];
    file.ticks += armGetSystemTick() - start;
    if (got > 0) file.bytes += static_cast<u64>(got);
    file.calls++;
    g_nx_reader = threadGetCurHandle();
  }
  __atomic_fetch_add(&g_nx_read_ticks, armGetSystemTick() - start, __ATOMIC_RELAXED);
  if (got > 0) __atomic_fetch_add(&g_nx_read_bytes, static_cast<u64>(got), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_nx_read_calls, 1, __ATOMIC_RELAXED);
  return got;
}

// POSIX rename replaces the target; Horizon refuses. Saves and settings rely on it.
int __real_rename(const char* from, const char* to);
int __wrap_rename(const char* from, const char* to) {
  int r = __real_rename(from, to);
  if (r != 0 && errno == EEXIST) {
    remove(to);
    r = __real_rename(from, to);
  }
  return r;
}

// A path naming no device here ("C:\\Windows\\...") fails with ENODEV, and
// std::filesystem throws on anything but "not found". It is simply not there.
int __real_stat(const char* path, struct stat* out);
int __wrap_stat(const char* path, struct stat* out) {
  int r = __real_stat(path, out);
  if (r != 0 && errno == ENOENT) {
    char other[768];
    if (InRomfs(path, other, sizeof(other))) r = __real_stat(other, out);
  }
  if (r != 0 && errno == ENODEV) errno = ENOENT;
  if (r == 0) SetFileId(path, out);
  return r;
}
int __real_lstat(const char* path, struct stat* out);
int __wrap_lstat(const char* path, struct stat* out) {
  int r = __real_lstat(path, out);
  if (r != 0 && errno == ENOENT) {
    char other[768];
    if (InRomfs(path, other, sizeof(other))) r = __real_stat(other, out);
  }
  if (r != 0 && errno == ENODEV) errno = ENOENT;
  if (r == 0) SetFileId(path, out);
  return r;
}

// A folder's listing is the card's and the NSP's together, the card's first; a name in
// both is listed once.
namespace {
struct MergedDir {
  std::string second;              // the NSP's folder, still to list
  DIR* second_dir = nullptr;
  std::unordered_set<std::string> seen;
};
std::mutex g_dirs_lock;
std::unordered_map<DIR*, MergedDir> g_dirs;
}  // namespace

DIR* __real_opendir(const char* path);
struct dirent* __real_readdir(DIR* dir);
int __real_closedir(DIR* dir);

DIR* __wrap_opendir(const char* path) {
  char other[768];
  const bool has_other = InRomfs(path, other, sizeof(other));
  DIR* d = __real_opendir(path);
  if (!d && errno == ENODEV) errno = ENOENT;
  if (!has_other) return d;
  if (!d) {
    return __real_opendir(other);
  }
  // Only worth tracking when the NSP has this folder too.
  if (DIR* o = __real_opendir(other)) {
    std::lock_guard<std::mutex> lock(g_dirs_lock);
    MergedDir& m = g_dirs[d];
    m.second_dir = o;
  }
  return d;
}

struct dirent* __wrap_readdir(DIR* dir) {
  MergedDir* m = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_dirs_lock);
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) m = &it->second;
  }
  if (!m) return __real_readdir(dir);
  if (struct dirent* e = __real_readdir(dir)) {
    m->seen.insert(e->d_name);
    return e;
  }
  while (struct dirent* e = __real_readdir(m->second_dir)) {
    if (!m->seen.count(e->d_name)) return e;
  }
  return nullptr;
}

int __wrap_closedir(DIR* dir) {
  {
    std::lock_guard<std::mutex> lock(g_dirs_lock);
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) {
      if (it->second.second_dir) __real_closedir(it->second.second_dir);
      g_dirs.erase(it);
    }
  }
  return __real_closedir(dir);
}

// Every deliberate stop goes through here: aborts in libnx, newlib and the driver
// (these leave no CPU fault for the handler below).
Result __real_svcBreak(u32 reason, uintptr_t address, uintptr_t size);
Result __wrap_svcBreak(u32 reason, uintptr_t address, uintptr_t size) {
  extern void _start(void);
  const u64 base = reinterpret_cast<u64>(&_start);
  if (!(reason & BreakReason_NotificationOnlyFlag)) {
    FILE* f = fopen(SVR_GAME_DIR "/logs/nx_crash.txt", "a");
    if (f) {
      u64 lr = reinterpret_cast<u64>(__builtin_return_address(0));
      fprintf(f, "break: reason %x, value %llx, size %llx, called from image+%llx\n", reason,
              (unsigned long long)address, (unsigned long long)size, (unsigned long long)(lr - base));
      // diagAbortWithResult passes a pointer to the result.
      if (address && size == sizeof(Result)) {
        fprintf(f, "  result 0x%x\n", *reinterpret_cast<Result*>(address));
      }
      const u64* sp = reinterpret_cast<const u64*>(__builtin_frame_address(0));
      fprintf(f, "  stack words inside the image:\n");
      MemoryInfo info{};
      u32 page = 0;
      if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<u64>(sp)))) {
        const u64 end = info.addr + info.size;
        int shown = 0;
        for (int i = 0; i < 8192 && reinterpret_cast<u64>(sp + i + 1) <= end && shown < 40; i++) {
          if (sp[i] > base + 0x1000 && sp[i] < base + (1ull << 28)) {
            fprintf(f, "  +%llx\n", (unsigned long long)(sp[i] - base));
            shown++;
          }
        }
      }
      fclose(f);
    }
  }
  return __real_svcBreak(reason, address, size);
}

// A fault in any thread: where it was, for the first test runs.
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  extern void _start(void);
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_crash.txt", "a");
  if (f) {
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    u64 base = reinterpret_cast<u64>(&_start);
    fprintf(f, "crash: type %u, thread %llu, pc %llx (image+%llx), lr %llx (image+%llx), sp %llx, far %llx, esr %x\n",
            ctx->error_desc, (unsigned long long)tid, (unsigned long long)ctx->pc.x,
            (unsigned long long)(ctx->pc.x - base), (unsigned long long)ctx->lr.x,
            (unsigned long long)(ctx->lr.x - base), (unsigned long long)ctx->sp.x, (unsigned long long)ctx->far.x,
            ctx->esr);
    for (int i = 0; i < 29; i++) {
      fprintf(f, "x%d %llx%s", i, (unsigned long long)ctx->cpu_gprs[i].x, (i % 4 == 3) ? "\n" : "  ");
    }
    // Return addresses on the stack, nearest first.
    fprintf(f, "\nstack words inside the image:\n");
    const u64* sp = reinterpret_cast<const u64*>(ctx->sp.x);
    MemoryInfo info{};
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, ctx->sp.x)) && (info.perm & Perm_R)) {
      u64 stack_end = info.addr + info.size;
      int shown = 0;
      for (int i = 0; i < 4096 && reinterpret_cast<u64>(sp + i) < stack_end && shown < 48; i++) {
        u64 v = sp[i];
        if (v > base + 0x1000 && v < base + (1ull << 28)) {
          fprintf(f, "  +%llx\n", (unsigned long long)(v - base));
          shown++;
        }
      }
    }
    fclose(f);
  }
  svcExitProcess();
}

}  // extern "C"
