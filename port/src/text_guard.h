// WWE SmackDown vs. Raw 2011 - text the game can't hang on (text_guard.cpp).
#pragma once

#include <string>

#include <rex/system/xmemory.h>

namespace svr2011 {

// The game's UTF-8 length table made safe: no byte stalls its text walks. At image load.
void InstallTextGuard(rex::memory::Memory* memory);

// `text` cut to at most `max` bytes on a character boundary, with any byte that isn't part of a
// well-formed UTF-8 character replaced by '?' - for names written into the game's records.
std::string CleanUtf8(const std::string& text, size_t max);

}  // namespace svr2011
