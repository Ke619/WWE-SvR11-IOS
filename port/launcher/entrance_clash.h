/* WWE SmackDown vs. Raw 2011 PC launcher - entrance number clashes between installed mods (entrance_clash.c).
   A superstar mod's (or a move pack's) pack.txt adds entrances by number - `pacentry evt\NyujyoN.pac EVPx|EVTx <n>`,
   Create-An-Entrance parts 10000 + 1000k + n, pyro `prtcls.pac PCE6|LPE6 <0nnn>` - and victory scenes
   (`evt\Winning0.pac EVPA|EVTA <n>`); its manifest picks them (entrance=, attire1..4_entrance=, victory=). Two mods
   adding the same number with different files: one superstar played the other's. Before a mod is put in place
   its clashing numbers move to free ones (the pack's lines, the scripts' own number, the manifest), recorded in
   the mod folder's .entrance_moves: an update keeps the number it was moved to while that is free. The same files
   under one number (a shared entrance) and the game's own numbers are left alone. */
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* incoming: the mod unpacked (its manifest.txt, pack.txt / moves\pack.txt), rewritten in place; dst: the folder it
   will replace or take (its old copy's .entrance_moves read; left out of the installed mods compared); game: the
   game folder (Mods under it). name: the mod's name for the lines. -> the numbers moved (0: none); report: one
   plain line per move (\r\n between). */
int entrance_clash_fix(const WCHAR *game, const WCHAR *incoming, const WCHAR *dst, const WCHAR *name, WCHAR *report,
                       size_t report_n);

#ifdef __cplusplus
}
#endif
