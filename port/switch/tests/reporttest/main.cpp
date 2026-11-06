// Sends a report with nx_report.cpp and writes what happened to sdmc:/SvR2011_reporttest/result.txt.
// Runs on the console or in Eden. The server comes from sdmc:/SvR2011_reporttest/servers.txt, one per line:
// private servers only. Nothing is built into this program.
#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../../nx_report.h"

extern "C" void userAppInit(void) { socketInitializeDefault(); }
extern "C" void userAppExit(void) { socketExit(); }

int main() {
  const std::string dir = "/SvR2011_reporttest";
  FILE* log = std::fopen((dir + "/result.txt").c_str(), "w");
  if (!log) return 1;

  std::string text = "server 203.0.113.9 and 127.0.0.1 and version 1.2.3.4\nDriver version 1.3.1.1\n";
  nx_report::RedactIps(text);
  std::fprintf(log, "redact: %s\n", text.c_str());

  std::ifstream servers(dir + "/servers.txt");
  std::string server;
  while (std::getline(servers, server)) {
    if (!server.empty() && server.back() == '\r') server.pop_back();
    if (server.empty()) continue;
    nx_report::Input in;
    in.game_dir = dir;
    in.server = server;
    in.version = "2.2.0-test";
    in.problem = "Something else";
    in.happens = "It happened once";
    in.what = "Report test \"quoted\" from the Switch sender.\nSecond line.";
    in.before = "";
    in.contact = "";
    in.match_dir = dir + "/match";
    in.picture = dir + "/shot.png";
    in.with_save = true;
    const u64 start = armGetSystemTick();
    const nx_report::Output out = nx_report::Send(in);
    std::fprintf(log, "server %s: ok %d id %s secret %d seen %d why [%s] %.1f s\n", server.c_str(), int(out.ok), out.id.c_str(),
                 int(!out.secret.empty()), out.seen_before, out.why.c_str(), double(armTicksToNs(armGetSystemTick() - start)) / 1e9);
    std::fflush(log);
  }
  std::fprintf(log, "done\n");
  std::fclose(log);
  return 0;
}
