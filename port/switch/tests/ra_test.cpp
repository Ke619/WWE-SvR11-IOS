// nx_readahead.h on a PC: random reads and seeks, compared with the file's real bytes.
// g++ -std=c++17 -O1 -fsanitize=undefined -I.. ra_test.cpp -o ra_test && ./ra_test
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <random>
#include <vector>

#include "nx_readahead.h"

static long g_real_reads = 0;
struct Ops {
  static int64_t Seek(int fd, int64_t off, int whence) { return _lseeki64(fd, off, whence); }
  static long Read(int fd, void* p, size_t n) {
    g_real_reads++;
    return _read(fd, p, static_cast<unsigned>(n));
  }
};
struct NoLock {
  void lock() {}
  void unlock() {}
};

static nx_ra::Table<Ops, NoLock> g_table;

int main() {
  const char* path = "ra_test.bin";
  std::mt19937 rng(1234);
  std::vector<unsigned char> truth(5 * 1024 * 1024 + 321);
  for (auto& b : truth) b = static_cast<unsigned char>(rng());
  FILE* w = fopen(path, "wb");
  fwrite(truth.data(), 1, truth.size(), w);
  fclose(w);

  int bad = 0;
  std::vector<unsigned char> out(1024 * 1024);
  const int64_t size = static_cast<int64_t>(truth.size());
  for (int round = 0; round < 200; round++) {
    const int fd = _open(path, _O_RDONLY | _O_BINARY);
    g_table.Opened(fd);
    int64_t pos = 0;
    for (int op = 0; op < 3000; op++) {
      const unsigned kind = rng() % 100;
      if (kind < 70) {  // a read: mostly small, sometimes large
        size_t n = (rng() % 10 == 0) ? rng() % (512 * 1024) : rng() % 2048;
        const long got = g_table.Read(fd, out.data(), n);
        const int64_t expect = std::min<int64_t>(static_cast<int64_t>(n), std::max<int64_t>(0, size - pos));
        if (got < 0 || got > expect || (got == 0 && expect > 0)) bad++;
        if (got > 0 && memcmp(out.data(), truth.data() + pos, got)) bad++;
        if (got > 0) pos += got;
      } else if (kind < 80) {  // the pread stand-in's pattern
        const int64_t saved = g_table.Seek(fd, 0, 1);
        if (saved != pos) bad++;
        const int64_t at = rng() % size;
        g_table.Seek(fd, at, 0);
        const size_t n = rng() % 4096;
        const long got = g_table.Read(fd, out.data(), n);
        if (got > 0 && memcmp(out.data(), truth.data() + at, got)) bad++;
        g_table.Seek(fd, saved, 0);
      } else if (kind < 90) {
        const int64_t at = rng() % (size + 100);
        if (g_table.Seek(fd, at, 0) != at) bad++;
        pos = at;
      } else if (kind < 95) {
        const int64_t off = static_cast<int64_t>(rng() % 4096) - 2048;
        if (pos + off >= 0) {
          if (g_table.Seek(fd, off, 1) != pos + off) bad++;
          pos += off;
        }
      } else if (kind < 98) {
        const int64_t got = g_table.Seek(fd, -5, 2);
        if (got != size - 5) bad++;
        pos = got;
      } else {
        g_table.Wrote();
      }
    }
    g_table.Closed(fd);
    _close(fd);
  }
  // A whole file 1 KB at a time: the point of it all.
  const int fd = _open(path, _O_RDONLY | _O_BINARY);
  g_table.Opened(fd);
  g_real_reads = 0;
  int64_t pos = 0;
  for (;;) {
    const long got = g_table.Read(fd, out.data(), 1024);
    if (got <= 0) break;
    if (memcmp(out.data(), truth.data() + pos, got)) bad++;
    pos += got;
  }
  if (pos != size) bad++;
  printf("sequential: %lld bytes in %ld real reads\n", static_cast<long long>(pos), g_real_reads);
  g_table.Closed(fd);
  _close(fd);
  remove(path);
  printf(bad ? "FAILED: %d\n" : "PASS\n", bad);
  return bad != 0;
}
