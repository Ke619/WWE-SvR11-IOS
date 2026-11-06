// What Windows and Android code provide that Switch doesn't have yet (music from the
// user's files, the crash reporter), and the few C library calls newlib lacks on
// Horizon. Crash reports for now: __libnx_exception_handler in nx_main.cpp.
#include <fcntl.h>
#include <pthread.h>
#include <switch.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "crash_report.h"
#include "music.h"

namespace svr2011 {

std::vector<std::filesystem::path> UserMusicSongs() { return {}; }
void SetSongGain(const std::filesystem::path&, float) {}
void PlayClip(const std::filesystem::path&, float) {}
int HostSoundStart(const std::filesystem::path&, bool, float) { return 0; }
void HostSoundStop(int) {}
void HostSoundPause(int, bool) {}
void HostSoundVolume(int, float) {}
bool HostSoundActive(int) { return false; }

void InstallCrashReporter(const std::filesystem::path&) {}
void SetCrashHook(void (*)()) {}
void SetCrashTextHook(void (*)(FILE*)) {}
void ReclaimCrashSignals() {}

}  // namespace svr2011

namespace {
std::mutex g_positional_io;  // pread / pwrite move the shared file offset here
}

extern "C" {

long sysconf(int name) {
#ifdef _SC_NPROCESSORS_ONLN
  if (name == _SC_NPROCESSORS_ONLN) return 3;
#endif
#ifdef _SC_NPROCESSORS_CONF
  if (name == _SC_NPROCESSORS_CONF) return 3;
#endif
#ifdef _SC_PAGESIZE
  if (name == _SC_PAGESIZE) return 0x1000;
#endif
  errno = EINVAL;
  return -1;
}

ssize_t pread(int fd, void* buffer, size_t size, off_t offset) {
  std::lock_guard<std::mutex> lock(g_positional_io);
  off_t saved = lseek(fd, 0, SEEK_CUR);
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t got = read(fd, buffer, size);
  lseek(fd, saved, SEEK_SET);
  return got;
}

ssize_t pwrite(int fd, const void* buffer, size_t size, off_t offset) {
  std::lock_guard<std::mutex> lock(g_positional_io);
  off_t saved = lseek(fd, 0, SEEK_CUR);
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t done = write(fd, buffer, size);
  lseek(fd, saved, SEEK_SET);
  return done;
}

// libnx has no thread detach: pthread_detach fails, and std::thread::detach throws.
// The finished thread keeps its small stack until the game exits.
int __wrap_pthread_detach(pthread_t) { return 0; }

int creat(const char* path, mode_t mode) { return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode); }

uid_t getuid(void) { return 0; }
int getpwuid_r(uid_t, void*, char*, size_t, void** result) {
  if (result) *result = nullptr;
  return ENOENT;
}

int execvp(const char*, char* const*) {
  errno = ENOSYS;
  return -1;
}
pid_t waitpid(pid_t, int*, int) {
  errno = ECHILD;
  return -1;
}

int _getentropy_r(struct _reent*, void* buffer, size_t size) {
  randomGet(buffer, size);
  return 0;
}

}  // extern "C"
