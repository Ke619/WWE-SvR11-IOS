// WWE SmackDown vs. Raw 2011 - crash reports.
//
// On abort(), std::terminate (uncaught C++ exception) or an unhandled
// structured exception, writes <dir>/crash_<time>.dmp (minidump) and
// <dir>/crash_<time>.txt (reason + symbolized stack of the crashing thread),
// and the same lines to the log, then lets the process die as it would have.
// The reason, address and guest registers go to the file and the log first (on disk at once), the stack and the
// minidump after: a game closed while its report is being made still leaves why. A stack overflow is reported
// from a thread of its own (the crashed thread has no stack left for it).
// Android / Linux (crash_report_posix.cpp): fatal signals, the text report
// and the log lines (no minidump).

#pragma once

#include <cstdio>
#include <filesystem>

namespace svr2011 {

void InstallCrashReporter(const std::filesystem::path& dir);

// Called (once) when a crash is reported, before the log is flushed: lets a
// subsystem log what it knows (the native renderer logs GPU fault details).
void SetCrashHook(void (*hook)());
// Writes more into the crash report's text, after the stack and registers (the match's black box: match_box.h).
void SetCrashTextHook(void (*hook)(FILE* f));

#if !defined(_WIN32)
// The crash reporter takes the fatal signals again (the SDK's own handler
// replaced it for SIGSEGV / SIGBUS when guest memory was set up): the next
// fault is reported and ends the game (guest_faults.cpp - a fault retried
// forever).
void ReclaimCrashSignals();
#endif

}  // namespace svr2011
