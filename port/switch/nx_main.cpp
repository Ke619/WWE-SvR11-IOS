// Switch start-up: services and folders before main, and a crash report for faults.
#include <switch.h>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sys/stat.h>
#include <unistd.h>

// The game's folder on the SD card (rex::filesystem::GetExecutableFolder).
// (no device name: std::filesystem only treats paths from the root as absolute)
#define SVR_GAME_DIR "/SvR2011"

namespace {

extern "C" void _start(void);

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

// Bring-up settings: written at each start, so a test build needs no card access.
static void WriteSettings() {
  FILE* f = fopen(SVR_GAME_DIR "/svr2011.toml", "w");
  if (!f) return;
  fputs(
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
      "show_fps = true\n"
      "perf_log_seconds = 10\n",
      f);
  fclose(f);
}

void userAppInit(void) {
  socketInitializeDefault();
  romfsMountFromCurrentProcess("romfs");  // (fails harmlessly without one)
  mkdir(SVR_GAME_DIR, 0777);
  mkdir(SVR_GAME_DIR "/logs", 0777);
  setenv("REX_APP_FOLDER", SVR_GAME_DIR, 1);
  setenv("HOME", SVR_GAME_DIR, 1);  // (the SDK asks for a user folder)
  chdir(SVR_GAME_DIR);
  // The driver's and the runtime's own messages, written as they come.
  if (freopen(SVR_GAME_DIR "/logs/nx_stderr.txt", "w", stderr)) {
    setvbuf(stderr, nullptr, _IOLBF, 1024);
  }
  std::set_terminate(OnTerminate);
  WriteSettings();
  NxProfileStart();
  // Start-up marks, flushed at once: this works even if logging never starts.
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_start.txt", "w");
  if (f) {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(f, "started: applet %d, total %llu MB, used %llu MB\n", appletGetAppletType(),
            (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
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

int __real__open_r(struct _reent* r, const char* path, int flags, int mode);
int __wrap__open_r(struct _reent* r, const char* path, int flags, int mode) {
  const bool writes = (flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT);
  int fd = __real__open_r(r, path, flags, mode);
  if (fd >= 0) return fd;
  char other[768];
  if (r->_errno != ENOENT || !InRomfs(path, other, sizeof(other))) return fd;
  if (writes) {
    MakeParents(path);
    return __real__open_r(r, path, flags, mode);
  }
  return __real__open_r(r, other, flags, mode);
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
  return r;
}

// Folders the NSP has are listed from the NSP (the card's copy of one would hide its
// files); the rest (Saves, Mods, logs) from the card.
DIR* __real_opendir(const char* path);
DIR* __wrap_opendir(const char* path) {
  char other[768];
  if (InRomfs(path, other, sizeof(other))) {
    if (DIR* d = __real_opendir(other)) return d;
  }
  DIR* d = __real_opendir(path);
  if (!d && errno == ENODEV) errno = ENOENT;
  return d;
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
