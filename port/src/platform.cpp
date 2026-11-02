// WWE SmackDown vs. Raw 2011 - platform detection (see platform.h).

#include "platform.h"

#include <cstdlib>
#include "util.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#endif
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif

namespace svr2011 {

namespace {

using svr2011::Env;

#if defined(_WIN32)
HMODULE Ntdll() { return GetModuleHandleW(L"ntdll.dll"); }
#endif

}  // namespace

bool IsWine() {
#if defined(_WIN32)
  static const bool wine = Ntdll() && GetProcAddress(Ntdll(), "wine_get_version");
  return wine;
#else
  return false;
#endif
}

bool IsSteamDeck() {
  static const bool deck = Env("SteamDeck") == "1";
  return deck;
}

uint32_t WindowsBuild() {
#if defined(_WIN32)
  if (IsWine()) return 0;  // (Wine reports the Windows it pretends to be)
  using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v = {sizeof v};
  const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(Ntdll(), "RtlGetVersion"));
  return get && get(&v) == 0 ? uint32_t(v.dwBuildNumber) : 0;
#else
  return 0;
#endif
}

std::string PlatformDescription() {
  std::string s;
#if !defined(_WIN32)
  // Android / Linux: the OS name, kernel release and (Android) API level.
  utsname u = {};
  s = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release : "POSIX";
#if defined(__ANDROID__)
  s = "Android API " + std::to_string(android_get_device_api_level()) + " (" + s + ")";
#endif
#else
  if (IsWine()) {
    using WineVersion = const char*(__cdecl*)();
    using HostVersion = void(__cdecl*)(const char** sysname, const char** release);
    const auto version = reinterpret_cast<WineVersion>(GetProcAddress(Ntdll(), "wine_get_version"));
    const auto host = reinterpret_cast<HostVersion>(GetProcAddress(Ntdll(), "wine_get_host_version"));
    s = std::string("Wine ") + (version ? version() : "?");
    if (host) {
      const char* sysname = nullptr;
      const char* release = nullptr;
      host(&sysname, &release);
      if (sysname) s += std::string(" on ") + sysname + (release ? std::string(" ") + release : "");
    }
    if (!Env("STEAM_COMPAT_DATA_PATH").empty()) s += " (Proton)";
  } else {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {sizeof v};
    const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(Ntdll(), "RtlGetVersion"));
    if (get && get(&v) == 0) {
      s = "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
          std::to_string(v.dwBuildNumber);
    } else {
      s = "Windows";
    }
  }
#endif
  if (IsSteamDeck()) s += " (Steam Deck)";
  return s;
}

}  // namespace svr2011
