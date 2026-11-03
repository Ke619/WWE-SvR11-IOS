// WWE SmackDown vs. Raw 2011 - PlayStation button pictures.
//
// The button icons in the game's text (menu help bars, tips, the move list,
// ...) are glyphs of its fonts: U+E000.. in the strings, drawn from each
// font's icon page. tools/make_pad_icons.py makes, from the user's PS3 copy,
// a taller picture of each icon page: the Xbox icons (as they are), the
// PlayStation ones, and "Xbox / PlayStation" pairs; and pad_icons.txt (in
// <game>/pad_icons). Then:
//  - the native renderer shows the taller picture in place of the page
//    (PadIconsAtlas: recognized by the page's data);
//  - the fonts' icon glyphs are moved to the Xbox section, and each button
//    glyph has a PlayStation and a pair version (the glyph lookup,
//    sub_826AD608, returns the one the prompt needs).
// Which: the prompt's player's controller (PadIconsOwner) or, for prompts
// shared by every player, the controllers in use - the PlayStation icons if
// all are PlayStation, the pairs ("RT / R2") if the players mix them.
// Without the files (or without the native renderer) the icons stay Xbox.

#pragma once

#include <cstdint>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Window;
}

namespace svr2011 {

void InstallPadIcons(rex::memory::Memory* memory);
// The window's key presses: a button bound to several keys ("E,O") shows the
// one the player pressed last in its keyboard prompts.
void InstallPadIconsKeys(rex::ui::Window* window);

// The taller picture for an icon page (the FNV-1a 64 of its base level as the
// renderer converted it), or null: RGBA8, width x height.
struct PadIconsPicture {
  uint32_t width = 0, height = 0;
  const std::vector<uint8_t>* rgba = nullptr;
};
PadIconsPicture PadIconsAtlas(uint32_t page_width, uint32_t page_height, uint64_t hash);
// Whether a texture of that size could be an icon page or a picture with
// other versions (before hashing it).
bool PadIconsCandidate(uint32_t width, uint32_t height);
// The renderer uploaded a texture at physical_base: its base level's FNV-1a
// 64 (0: not hashed) - a picture with other versions, or not (any longer).
void PadIconsTextureUploaded(uint32_t physical_base, uint32_t width, uint32_t height, uint64_t hash);

// The player the prompts being drawn belong to (-1: shared by everyone).
void SetPadIconsOwner(int user_index);

// A player's pad state as the game reads it (XInput layout): pictures shared
// by everyone follow the last player to press something.
void PadIconsInput(uint32_t user_index, const uint8_t* state);

}  // namespace svr2011
