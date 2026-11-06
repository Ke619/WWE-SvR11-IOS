/* WWE SmackDown vs. Raw 2011 launcher - the Nintendo Switch export: the small program (svr2011.nsp, from this release)
   and the game as loose files, written to the SD card or a folder to copy there:
       <out>\svr2011.nsp
       <out>\SvR2011\...              the game's files; saves, mods and music if ticked
       <out>\Install on Switch.txt
   No keys and no game file goes inside the NSP. Only what changed is copied, so doing it again is quick. */
#pragma once

#include <windows.h>

typedef void (*switch_progress_fn)(int permille, const WCHAR *msg);

typedef struct {
    int saves;              /* replaces the card's saves; the ones it replaces go to SvR2011\SaveBackups\<date> */
    int mods;               /* Mods */
    int music;              /* Music, Custom Movies */
    const WCHAR *saves_dir; /* this PC's saves folder (saves) */
} SwitchOptions;

/* The NSP the release ships: <launcher>\Switch\svr2011.nsp, or <game>\Switch\svr2011.nsp. 1 if found. */
int switch_find(const WCHAR *game_dir, const WCHAR *launcher_dir, WCHAR *out, int outn);

/* Returns 1, or 0 with err filled (cancelled: err is "Cancelled."). */
int switch_package(const WCHAR *game_dir, const WCHAR *nsp, const WCHAR *out_dir, const SwitchOptions *opt,
                   volatile LONG *cancel, switch_progress_fn progress, WCHAR *err, int errn);
