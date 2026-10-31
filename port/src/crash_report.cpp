// WWE SmackDown vs. Raw 2011 - crash reports (see crash_report.h).

#include "crash_report.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

#include <windows.h>
#include <dbghelp.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

namespace svr2011 {

namespace {

std::filesystem::path g_dir;
std::atomic<bool> g_reported{false};
void (*g_hook)() = nullptr;
void (*g_text_hook)(FILE*) = nullptr;

std::wstring Stamp() {
  wchar_t buf[64];
  const std::time_t t = std::time(nullptr);
  std::tm tm = {};
  localtime_s(&tm, &t);
  wcsftime(buf, 64, L"%Y%m%d_%H%M%S", &tm);
  return buf;
}

// Symbolized frames of the current thread (or of `context`).
void WriteStack(FILE* f, CONTEXT* context) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  void* frames[64];
  USHORT count = 0;
  if (context) {
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = context->Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = context->Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = context->Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    CONTEXT copy = *context;
    while (count < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &sf,
                                     &copy, nullptr, SymFunctionTableAccess64,
                                     SymGetModuleBase64, nullptr) &&
           sf.AddrPC.Offset) {
      frames[count++] = reinterpret_cast<void*>(sf.AddrPC.Offset);
    }
  } else {
    count = CaptureStackBackTrace(1, 64, frames, nullptr);
  }
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  for (USHORT i = 0; i < count; ++i) {
    const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    char module[MAX_PATH] = "?";
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &hm)) {
      GetModuleFileNameA(hm, module, MAX_PATH);
    }
    const char* base = std::strrchr(module, '\\');
    if (SymFromAddr(process, address, &displacement, symbol)) {
      IMAGEHLP_LINE64 line = {sizeof(line)};
      DWORD line_disp = 0;
      if (SymGetLineFromAddr64(process, address, &line_disp, &line)) {
        std::fprintf(f, "  %2u %s!%s+0x%llx (%s:%lu)\n", i, base ? base + 1 : module, symbol->Name,
                     static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
      } else {
        std::fprintf(f, "  %2u %s!%s+0x%llx\n", i, base ? base + 1 : module, symbol->Name,
                     static_cast<unsigned long long>(displacement));
      }
    } else {
      std::fprintf(f, "  %2u %s+0x%llx\n", i, base ? base + 1 : module,
                   static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(hm)));
    }
  }
}


// The crashed guest thread's PowerPC registers (as last stored by the
// recompiled code - current at the last call boundary).
void WriteGuestContext(FILE* f) {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->thread_state() || !thread->thread_state()->context()) return;
  const PPCContext& c = *thread->thread_state()->context();
  const uint64_t r[32] = {c.r0.u64,  c.r1.u64,  c.r2.u64,  c.r3.u64,  c.r4.u64,  c.r5.u64,  c.r6.u64,
                          c.r7.u64,  c.r8.u64,  c.r9.u64,  c.r10.u64, c.r11.u64, c.r12.u64, c.r13.u64,
                          c.r14.u64, c.r15.u64, c.r16.u64, c.r17.u64, c.r18.u64, c.r19.u64, c.r20.u64,
                          c.r21.u64, c.r22.u64, c.r23.u64, c.r24.u64, c.r25.u64, c.r26.u64, c.r27.u64,
                          c.r28.u64, c.r29.u64, c.r30.u64, c.r31.u64};
  std::fprintf(f, "guest thread %X: lr %08X ctr %08X\n", thread->thread_id(), uint32_t(c.lr),
               c.ctr.u32);
  for (int i = 0; i < 32; i++)
    std::fprintf(f, "  r%-2d %08X%s", i, uint32_t(r[i]), i % 4 == 3 ? "\n" : "");
}

// The pads' rumble off - first thing: the dump below takes seconds, and an XInput pad keeps its last motor speeds
// after the game is gone, until it is unplugged (2.1.7 report 20261031-022513-dca504: "controller vibrated non stop").
// XInputSetState looked up at run time (no import of its own).
void StopRumble() {
  using SetState = DWORD(WINAPI*)(DWORD, void*);
  for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
    if (HMODULE m = GetModuleHandleW(dll) ? GetModuleHandleW(dll) : LoadLibraryW(dll))
      if (auto set = reinterpret_cast<SetState>(GetProcAddress(m, "XInputSetState"))) {
        WORD speeds[2] = {0, 0};  // (XINPUT_VIBRATION: left, right motor)
        for (DWORD pad = 0; pad < 4; ++pad) set(pad, speeds);
        return;
      }
}

void Report(const char* reason, EXCEPTION_POINTERS* info) {
  if (g_reported.exchange(true) || g_dir.empty()) return;
  StopRumble();
  if (g_hook) g_hook();
  rex::FlushLogging();  // the log's last lines usually say why
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const std::wstring stamp = Stamp();
  const std::filesystem::path dump = g_dir / (L"crash_" + stamp + L".dmp");
  const std::filesystem::path text = g_dir / (L"crash_" + stamp + L".txt");
  HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
  }
  FILE* f = nullptr;
  if (_wfopen_s(&f, text.c_str(), L"w") == 0 && f) {
    std::fprintf(f, "SvR 2011 crash: %s (thread %lu)\n", reason, GetCurrentThreadId());
    if (info && info->ExceptionRecord) {
      std::fprintf(f, "exception 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode,
                   info->ExceptionRecord->ExceptionAddress);
    }
    WriteStack(f, info ? info->ContextRecord : nullptr);
    WriteGuestContext(f);
    if (g_text_hook) g_text_hook(f);
    std::fclose(f);
  }
  // The same lines in the log (players send the log; the report beside it
  // is often missed).
  if (FILE* r = nullptr; _wfopen_s(&r, text.c_str(), L"r") == 0 && r) {
    char line[512];
    while (std::fgets(line, sizeof(line), r)) {
      size_t n = std::strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
      REXLOG_ERROR("crash: {}", line);
    }
    std::fclose(r);
    REXLOG_ERROR("crash: report in {}", text.string());
    rex::FlushLogging();
  }
  std::fprintf(stderr, "SvR 2011 crash: %s - report in %ls\n", reason, text.c_str());
}

void OnAbort(int) { Report("abort()", nullptr); }

void OnTerminate() {
  const char* what = "std::terminate";
  static std::string message;
  if (auto e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      message = std::string("uncaught exception: ") + ex.what();
      what = message.c_str();
    } catch (...) {
      what = "uncaught non-standard exception";
    }
  }
  Report(what, nullptr);
  std::abort();
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  Report("unhandled exception", info);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void SetCrashHook(void (*hook)()) { g_hook = hook; }
void SetCrashTextHook(void (*hook)(FILE*)) { g_text_hook = hook; }

void InstallCrashReporter(const std::filesystem::path& dir) {
  g_dir = dir;
  std::signal(SIGABRT, OnAbort);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  std::set_terminate(OnTerminate);
  SetUnhandledExceptionFilter(OnUnhandled);
}

}  // namespace svr2011
