// The Mod Maker's session log and "Report a problem" (as the launcher's,
// launcher/report.c).
//
// Session log: every Log() line (pages, projects, converter command lines and
// their whole output, exit codes and times, errors, the tools check, the disc
// images found) with the time, written as the Mod Maker runs - flushed line by
// line, so a crash keeps it - to <game>\logs\modmaker_<date-time>.log (the
// settings folder's logs before a game folder is set). It starts with the
// version, the system and the settings. The 10 newest are kept.
//
// Report a problem (title bar): asks what happened, as the launcher does, then
// one zip in <game>\Reports with this and the last session logs, the open
// project's text files, the converters' work folder's newest report.json and
// log, the tools check and the mod doctor's results. Internet addresses, the
// Windows user folder's name and secrets are blanked; no disc images or game
// files go in (text only, each capped).
#pragma once

#include <functional>
#include <string>

namespace mm::session {

// A Log() line (any thread).
void Line(const std::string& s);
// At start: a crash (an exception nothing caught, std::terminate, abort, a pure call, a bad CRT parameter) leaves a
// "CRASH" line in the session log and <log folder>\modmaker_crash_<date-time>.txt (what, where as module+offset, the
// stack, the last log lines); Report a problem sends the newest two. Then Windows handles it as before.
void InstallCrashHandler();
// The swap chain's Present answer (UI thread): a lost GPU device is logged once, with why.
void PresentResult(long hr);
// Text for the file only (long converter answers), each line indented.
void Raw(const std::string& label, const std::string& text);
// Each frame (UI thread): opens the file once the game folder is known, notes page changes, draws the report window.
void Tick();
// The title bar's button: opens the report window. `project_state` gives the open project's state text.
void OpenReport(std::function<std::string()> project_state);
// Test aid (--test-report <what happened>): the report made without the window, its path logged, then the Mod Maker closes.
void TestReport(const std::string& what);
// Test aid: --test-report-send <answers file> - Send report with the file's answers (what= before= contact= problem=<n>
// image=<path>; the server: the game's online_server, or SVR2011_REPORT_SERVER), logs the answer, then quits.
void TestReportSend(const std::string& answers_file);

}  // namespace mm::session
