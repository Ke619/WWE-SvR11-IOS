// WWE SmackDown vs. Raw 2011 - text the game can't hang on (see text_guard.h).
//
// The game walks its text a character at a time by a table of UTF-8 character lengths (kLengths: a
// u32 per lead byte; sub_826A4710 strlen, sub_826A47D8 strchr and 35 more). A byte that can't start
// a character (a stray continuation byte, a cp1252 quote or accent) has length 0 there: the walk
// stops moving, strlen gives up after 32M steps and says 0, and the text draw's line count
// (sub_823D1E98: "while (*p) p += strlen-or-line(p)") never ends - the screen freezes in the draw
// pass with the logic thread at 100%. 2.1.7 report 20261031-162305-79e156: CREATE A MOVE-SET froze
// on replacing a signature (a pack move's name); a name cut mid-character by our own 63-byte copies
// did the same. The table's 0 entries are set to 1 (such a byte is one character); names we write
// are cleaned too (CleanUtf8: move_packs.cpp, cam_mod_moves.cpp).
#include "text_guard.h"

#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>

namespace {

constexpr uint32_t kLengths = 0x82DADDF0;  // (lis -32037 / addi -8720 in sub_826A4710)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

namespace svr2011 {

void InstallTextGuard(rex::memory::Memory* memory) {
  if (std::getenv("SVR2011_TEST_TEXT_GUARD_OFF")) {  // (test aid: the game's own table - the freeze)
    REXLOG_INFO("[svr2011] text guard: off (SVR2011_TEST_TEXT_GUARD_OFF)");
    return;
  }
  uint8_t* table = memory->TranslateVirtual<uint8_t*>(kLengths);
  // (a sanity check: ASCII is 1 byte a character, 0xC2 starts 2 and 0xE3 3 - else this isn't the table)
  if (Rd32(table + 'A' * 4) != 1 || Rd32(table + 0xC2 * 4) != 2 || Rd32(table + 0xE3 * 4) != 3) {
    REXLOG_WARN("[svr2011] text guard: no UTF-8 length table at {:08X} (not patched)", kLengths);
    return;
  }
  int zero = 0, first = -1, last = -1;
  for (int b = 1; b < 256; ++b)
    if (!Rd32(table + b * 4)) ++zero, last = b, first = first < 0 ? b : first;
  if (!zero) {
    REXLOG_INFO("[svr2011] text guard: the UTF-8 length table has no 0 entries");
    return;
  }
  // (the image is read-only: writable for the table)
  auto* heap = memory->LookupHeap(kLengths);
  uint32_t old = 0;
  if (!heap || !heap->QueryProtect(kLengths, &old) ||
      !heap->Protect(kLengths, 1024, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_WARN("[svr2011] text guard: {:08X} can't be made writable (not patched)", kLengths);
    return;
  }
  for (int b = 1; b < 256; ++b)
    if (!Rd32(table + b * 4)) table[b * 4 + 3] = 1;
  heap->Protect(kLengths, 1024, old);
  REXLOG_INFO("[svr2011] text guard: {} bytes that start no UTF-8 character (0x{:02X}-0x{:02X}) count as one", zero, first,
              last);
}

std::string CleanUtf8(const std::string& text, size_t max) {
  static const bool off = std::getenv("SVR2011_TEST_UTF8_CLEAN_OFF") != nullptr;  // (test aid: names cut as before)
  if (off) return text.substr(0, max);
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i]);
    const size_t n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    bool ok = n && i + n <= text.size();
    for (size_t k = 1; ok && k < n; ++k) ok = (uint8_t(text[i + k]) & 0xC0) == 0x80;
    const size_t len = ok ? n : 1;
    if (out.size() + len > max) break;
    if (ok)
      out.append(text, i, n);
    else
      out += '?';
    i += len;
  }
  return out;
}

}  // namespace svr2011
