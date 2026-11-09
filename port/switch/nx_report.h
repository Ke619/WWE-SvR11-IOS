// Report a problem, sent from the console: the launcher's report (launcher/report_flow.c) rebuilt without Windows.
// No game code in here, so tests/reporttest can run it on its own.
#pragma once

#include <cstdint>
#include <string>

namespace nx_report {

struct Input {
  std::string game_dir;   // no trailing slash
  std::string server;     // online_server: http:// or https://
  std::string version;
  std::string problem, happens, what, before, contact;  // contact: the Discord name (2.2.1)
  std::string token;      // the online sign-in (online_token): sent as Authorization, never in the report
  std::string match_dir;  // a recorded match's folder, or ""
  std::string picture;    // a screenshot's file, or ""
  bool with_save = false;
  bool pictures_later = false;
  void (*progress)(uint64_t sent, uint64_t total) = nullptr;
};

struct Output {
  bool ok = false;
  bool account = false;   // refused for the sign-in or the Discord name (why says what to do)
  std::string id, secret, why;
  int seen_before = 0;
};

Output Send(const Input& in);

// Blanks IP addresses in a log, as launcher/report.c does; loopback and lines with "ersion" in them stay.
void RedactIps(std::string& text);

// Puts the receipt in <game>/UserData/reports.json, the launcher's format.
bool AddReceipt(const Input& in, const Output& out);

}  // namespace nx_report
