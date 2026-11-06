// Switch start-up: services and folders before main, and a crash report for faults.
#include <switch.h>

#include <cerrno>
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

// An uncaught C++ exception: its text, then the normal way out.
void OnTerminate() {
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_crash.txt", "a");
  if (f) {
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

void userAppInit(void) {
  socketInitializeDefault();
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

// A path naming no device here ("C:\\Windows\\...") fails with ENODEV, and
// std::filesystem throws on anything but "not found". It is simply not there.
int __real_stat(const char* path, struct stat* out);
int __wrap_stat(const char* path, struct stat* out) {
  int r = __real_stat(path, out);
  if (r != 0 && errno == ENODEV) errno = ENOENT;
  return r;
}
int __real_lstat(const char* path, struct stat* out);
int __wrap_lstat(const char* path, struct stat* out) {
  int r = __real_lstat(path, out);
  if (r != 0 && errno == ENODEV) errno = ENOENT;
  return r;
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
