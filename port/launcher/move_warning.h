/* The custom moves warning (launcher, Mod Maker): a mod carrying moves of its own - a move pack, or a superstar mod
 * with a moves\ pack (ported from another game or made by hand) - is asked about before it is installed: such moves
 * are experimental and most likely broken (they land wrong, look odd or freeze a match). Not a moveset of the game's
 * own moves (moves.txt alone). */
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the unpacked mod in `dir` carries custom moves: pack.txt (a move pack) or moves\pack.txt (a superstar
   mod's own moves). */
int move_warning_dir_has_moves(const WCHAR *dir);

/* The question before a mod with custom moves is installed - 1: install it, 0: not. A Yes/No box (No the default)
   with "Don't warn me again" (<game>\Mods\.no_move_warning: asked no more, for every tool on that game folder).
   headless (an install with no one to ask - tests, command lines, bundled mods): the warning goes to
   <game>\Mods\.install.log and the install goes on.
   Test aid SVR2011_MOVE_WARNING_SHOT=<file.bmp>: the box is asked even headless, drawn off-screen (never shown or
   activated), saved there and answered No. */
int move_warning_ask(HWND owner, const WCHAR *game, const WCHAR *mod_name, int headless);

#ifdef __cplusplus
}
#endif
