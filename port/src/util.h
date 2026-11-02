// WWE SmackDown vs. Raw 2011 - small helpers many files had their own copy of
// (one copy here; each file names the ones it uses: using svr2011::NowMs, ...).

#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace svr2011 {

// Milliseconds on the steady clock (for intervals, not the time of day).
inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// An environment variable's value ("" when it isn't set).
inline std::string Env(const char* name) {
  char* value = nullptr;
  size_t len = 0;
  std::string result;
  if (_dupenv_s(&value, &len, name) == 0 && value) {
    result = value;
    free(value);
  }
  return result;
}

// An environment variable set to 1 (it starts with '1').
inline bool EnvFlag(const char* name) {
  char* v = nullptr;
  size_t n = 0;
  bool on = false;
  if (_dupenv_s(&v, &n, name) == 0 && v) {
    on = v[0] == '1';
    free(v);
  }
  return on;
}

// Lower / upper case, byte by byte (std::tolower / std::toupper: ASCII letters in the C locale).
inline std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
inline std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace svr2011
