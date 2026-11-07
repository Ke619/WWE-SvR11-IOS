/* WWE SmackDown vs. Raw 2011 - the save cleaner (the Saves tab's "Clean save of unused mods..."): what a save
 * keeps for superstar mods that aren't installed, or are turned off, and taking it out. */
#pragma once

#include <windows.h>

#define SC_MAX 400

/* One mod with data in the save. `id` is the superstar id the save holds it at (0: only Universe or a mod save
 * file of its own), `folder` its Mods\Superstars folder when known (slots.txt, .universe\mod_profiles.txt,
 * .universe\mods.bin), `name` the name the save has for it. `off`: installed but turned off. */
typedef struct {
    int id;
    int index;     /* its place in the save's records and profiles (pool_placeholders.bin) */
    int off;
    int profile;   /* its record / profile in SaveData.dat */
    int universe;  /* an entry in .universe\mods.bin */
    int mark;      /* a line in .universe\mod_profiles.txt */
    int mod_save;  /* Saves\.mods\<folder>.dat (per_mod_saves) */
    int refs;      /* how many teams / titles / Universe card places name it in Saves\.mods\refs.bin */
    WCHAR folder[MAX_PATH];
    WCHAR name[40];
} ScItem;

/* What `saves` (the saves folder) keeps for mods that aren't installed or are off in `mods` (the game's
 * Mods\Superstars). Returns the count (up to SC_MAX), or -1 with a message in err: no SaveData.dat, or no
 * .universe\pool_placeholders.bin yet (the game writes it at its start - 2.1.9 and later). */
int save_clean_scan(const WCHAR *saves, const WCHAR *mods, ScItem *items, WCHAR *err, size_t err_n);

/* Takes out the items whose `off` is 0, and the turned-off ones too with with_off: their ids' records and
 * profiles (and 40-byte entries) back to the game's placeholders (SaveData.dat's checksum set again), their mods.bin entries and
 * mod_profiles.txt lines, their Saves\.mods\<folder>.dat and their refs in .mods\refs.bin (and refs.bin.bak). The caller makes the backup first. Returns how many mods were
 * taken out, or -1 with a message in err (nothing changed). */
int save_clean_apply(const WCHAR *saves, const ScItem *items, int n, int with_off, WCHAR *err, size_t err_n);
