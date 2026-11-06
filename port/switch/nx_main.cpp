// Switch start-up: services and folders before main, and a crash report for faults.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

// The game's folder on the SD card (rex::filesystem::GetExecutableFolder).
#define SVR_GAME_DIR "sdmc:/SvR2011"

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
  chdir(SVR_GAME_DIR);
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

// A fault in any thread: where it was, for the first test runs.
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  extern char __start__;
  FILE* f = fopen(SVR_GAME_DIR "/logs/nx_crash.txt", "a");
  if (f) {
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    u64 base = reinterpret_cast<u64>(&__start__);
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
        if (v >= base && v < base + (1ull << 30)) {
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
