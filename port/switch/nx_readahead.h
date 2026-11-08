// Read-ahead for files opened read-only. Each read on Horizon is a request to the file
// system service, and C++ streams and stdio read 1 KB at a time here, so a 30 MB file took
// 30,000 requests. Sequential small reads are served from a buffer instead.
//
// The file position of a tracked file is kept here: lseek only moves `pos`, and the real
// position is set just before a real read. So the pread stand-in's seek away and back
// keeps the buffer. A write anywhere drops all buffers (`generation`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace nx_ra {

constexpr int kMaxFds = 256;
constexpr size_t kMinFill = 16 * 1024, kMaxFill = 128 * 1024;

// Ops: static int64_t Seek(int fd, int64_t off, int whence); static long Read(int fd, void*, size_t);
// Lock: a lock with lock() / unlock().
template <typename Ops, typename Lock>
class Table {
 public:
  bool Tracked(int fd) const { return fd >= 0 && fd < kMaxFds && files_[fd].open; }

  void Opened(int fd) {
    if (fd < 0 || fd >= kMaxFds) return;
    File& f = files_[fd];
    f.lock.lock();
    f.pos = 0;
    f.next = 0;
    f.start = 0;
    f.have = 0;
    f.fill = kMinFill;
    f.open = true;
    f.lock.unlock();
  }

  void Closed(int fd) {
    if (fd < 0 || fd >= kMaxFds || !files_[fd].open) return;
    File& f = files_[fd];
    f.lock.lock();
    f.open = false;
    std::free(f.buffer);
    f.buffer = nullptr;
    f.have = 0;
    f.lock.unlock();
  }

  void Wrote() { __atomic_fetch_add(&generation_, 1, __ATOMIC_RELAXED); }

  int64_t Seek(int fd, int64_t off, int whence) {
    File& f = files_[fd];
    f.lock.lock();
    int64_t now = -1;
    if (whence == 0) {  // SEEK_SET
      now = off;
    } else if (whence == 1) {  // SEEK_CUR
      now = f.pos + off;
    } else {  // SEEK_END, and anything else: the file system's answer
      now = Ops::Seek(fd, off, whence);
    }
    if (now >= 0) {
      f.pos = now;
    } else {
      now = -1;
    }
    f.lock.unlock();
    return now;
  }

  long Read(int fd, void* out, size_t n) {
    File& f = files_[fd];
    f.lock.lock();
    const long got = ReadLocked(f, fd, static_cast<char*>(out), n);
    f.lock.unlock();
    return got;
  }

 private:
  struct File {
    Lock lock;
    bool open = false;
    int64_t pos = 0;    // the file's position
    int64_t next = 0;   // where the last read ended: a read from here is sequential
    int64_t start = 0;  // the buffer holds [start, start + have)
    size_t have = 0;
    size_t fill = kMinFill;
    uint64_t seen = 0;  // the generation the buffer was filled in
    char* buffer = nullptr;
  };

  long Direct(File& f, int fd, char* out, size_t n) {
    if (Ops::Seek(fd, f.pos, 0) < 0) return -1;
    const long got = Ops::Read(fd, out, n);
    if (got > 0) f.pos += got;
    return got;
  }

  long ReadLocked(File& f, int fd, char* out, size_t n) {
    if (n == 0) return 0;
    const uint64_t generation = __atomic_load_n(&generation_, __ATOMIC_RELAXED);
    if (f.seen != generation) {
      f.have = 0;
      f.seen = generation;
    }
    size_t done = 0;
    // What the buffer holds.
    if (f.have && f.pos >= f.start && f.pos < f.start + static_cast<int64_t>(f.have)) {
      const size_t at = static_cast<size_t>(f.pos - f.start);
      done = f.have - at < n ? f.have - at : n;
      std::memcpy(out, f.buffer + at, done);
      f.pos += static_cast<int64_t>(done);
      f.next = f.pos;
      if (done == n) return static_cast<long>(done);
    }
    const size_t left = n - done;
    const bool sequential = f.pos == f.next;
    // Large reads, and reads that jump about, go straight to the file.
    if (left >= kMaxFill / 2 || !sequential) {
      if (!sequential) f.fill = kMinFill;
      const long got = Direct(f, fd, out + done, left);
      if (got < 0) return done ? static_cast<long>(done) : got;
      f.next = f.pos;
      return static_cast<long>(done) + got;
    }
    if (!f.buffer) {
      f.buffer = static_cast<char*>(std::malloc(kMaxFill));
      if (!f.buffer) {
        const long got = Direct(f, fd, out + done, left);
        if (got < 0) return done ? static_cast<long>(done) : got;
        f.next = f.pos;
        return static_cast<long>(done) + got;
      }
    }
    size_t want = f.fill;
    if (want < left) want = left;
    if (Ops::Seek(fd, f.pos, 0) < 0) return done ? static_cast<long>(done) : -1;
    const long got = Ops::Read(fd, f.buffer, want);
    if (got <= 0) {
      f.have = 0;
      return done ? static_cast<long>(done) : got;
    }
    f.start = f.pos;
    f.have = static_cast<size_t>(got);
    if (f.fill < kMaxFill) f.fill *= 2;
    const size_t take = f.have < left ? f.have : left;
    std::memcpy(out + done, f.buffer, take);
    f.pos += static_cast<int64_t>(take);
    f.next = f.pos;
    return static_cast<long>(done + take);
  }

  File files_[kMaxFds];
  uint64_t generation_ = 0;
};

}  // namespace nx_ra
