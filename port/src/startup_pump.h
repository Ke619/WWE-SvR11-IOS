// Keeps the game window answering Windows while the port prepares mods at
// start (move packs, arena and superstar mods): that work runs on the UI
// thread before the app's event loop, and a window that reads no messages
// for 5 s is "Not Responding" - clicking it then lets Windows close the game
// (a slow first start on a hard drive with a cold file cache looked like a
// crash). SDL_PumpEvents reads the OS messages into SDL's queue; the event
// loop handles them once it runs.
//
// The same calls mark the start-up as busy (StartupBusy): the GPU's stuck
// watch (gpu_driver.cpp) doesn't count that time as the graphics driver's.
// And a long step can say what it does (StartupNote): on Android a note the
// phone shows over the still black game - 2.1.9 report b7f723: the first
// start after adding mods merged 48 move packs for minutes with no picture,
// the player closed it every time (each start merged again from the top) and
// the launcher blamed the phone's graphics driver.
#pragma once

#include <SDL3/SDL_events.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace svr2011 {

// The UI thread (the one that runs the Install* calls), set by the app.
inline std::thread::id& StartupThread() {
  static std::thread::id id;
  return id;
}

namespace startup_detail {
inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline std::atomic<int64_t>& BusyAt() {  // (the last KeepWindowAlive on the startup thread)
  static std::atomic<int64_t> at{0};
  return at;
}
inline std::mutex& NoteMutex() {
  static std::mutex m;
  return m;
}
inline std::string& Note() {
  static std::string note;
  return note;
}
inline int64_t& NoteShownAt() {
  static int64_t at = 0;
  return at;
}
// (a long toast lasts 3.5 s: shown again every 3 s while the step runs, it stays up)
inline void ShowNote(bool force) {
#if defined(__ANDROID__)
  std::lock_guard lock(NoteMutex());
  if (Note().empty()) return;
  const int64_t now = NowMs();
  if (!force && now - NoteShownAt() < 3000) return;
  NoteShownAt() = now;
  SDL_ShowAndroidToast(Note().c_str(), 1, -1, 0, 0);
#else
  (void)force;
#endif
}
}  // namespace startup_detail

// True while the start-up prepares mods (a KeepWindowAlive in the last 5 s).
inline bool StartupBusy() {
  const int64_t at = startup_detail::BusyAt().load();
  return at && startup_detail::NowMs() - at < 5000;
}

// What a long start-up step does, for the player ("" when it's done). Shown at once, then kept up by
// KeepWindowAlive (Android only: elsewhere the window answers and the log says it).
inline void StartupNote(std::string text) {
  {
    std::lock_guard lock(startup_detail::NoteMutex());
    startup_detail::Note() = std::move(text);
  }
  startup_detail::ShowNote(true);
}

// Cheap enough to call per file: pumps at most every 100 ms, only on the
// startup thread.
inline void KeepWindowAlive() {
  if (std::this_thread::get_id() != StartupThread()) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point last;
  const auto now = clock::now();
  if (now - last < std::chrono::milliseconds(100)) return;
  last = now;
  startup_detail::BusyAt() = startup_detail::NowMs();
  SDL_PumpEvents();
  startup_detail::ShowNote(false);
}

}  // namespace svr2011
