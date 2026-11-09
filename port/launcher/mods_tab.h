/* WWE SmackDown vs. Raw 2011 PC launcher - the Mods tab (mods_tab.c). */
#pragma once

#include <windows.h>

/* The launcher's control helper: add(tab, class, text, style, x, y, w, h, id)
 * in 96-DPI units, registered with the tab's show/hide list. */
typedef HWND (*mods_add_fn)(int tab, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h,
                            int id);

/* Builds the tab's controls (ids id_base .. id_base + 5). */
void mods_build(HWND wnd, mods_add_fn add, int tab, int id_base, HFONT title_font);
/* The tab was shown: rescans <game_dir>/Mods. */
void mods_show(const WCHAR *game_dir);
/* WM_COMMAND / WM_NOTIFY: 1 when handled. */
int mods_command(int id, int code);
/* 1: handled; 2 / 3: the window answers TRUE / FALSE (renaming a background). */
int mods_notify(const NMHDR *nm);
/* A background's name (manifest.txt name=). 1 on success. */
int mods_rename(const WCHAR *folder, const WCHAR *name);
int mods_remove(const WCHAR *game, const WCHAR *folder);
int mods_print_superstars(const WCHAR *game);
/* Two switched-on attire mods on one superstar's attire: the newer moves to a free attire (its manifest
   rewritten), or the player is told to switch one off. fresh: a mod just installed (the newer), or NULL.
   -> lines said (report: the text). */
int mods_attire_guard(const WCHAR *game, const WCHAR *fresh, WCHAR *report, size_t report_n);
int mods_print_attires(const WCHAR *game);
/* Installs a .svrmod into <game_dir>/Mods (the + button without its dialog;
   tests: --mods-add <file>). 1 on success; the tab's status says why not. */
int mods_install(const WCHAR *game_dir, const WCHAR *zip);
/* The folder the last mods_install put its mod in (full path). */
const WCHAR *mods_last_installed(void);
/* Installed mods (manifest.txt) whose name - and author, unless NULL - is the one given, whatever the case: how many;
   the first's folder and version. (Discovery: copies installed before it kept a record.) */
int mods_find_installed(const WCHAR *game_dir, const WCHAR *name, const WCHAR *author, WCHAR *folder, size_t folder_n,
                        WCHAR *version, size_t version_n);
/* A mod folder's manifest version ("" none). */
void mods_folder_version(const WCHAR *folder, WCHAR *version, size_t n);
/* a newer than b (manifest versions: 1.10 > 1.9). */
int mods_version_newer(const WCHAR *a, const WCHAR *b);
/* The Installed Mods status line (Discovery's update progress). */
void mods_set_status(const WCHAR *t);
/* Discovery learnt which mods have updates: the list shown again (its status line kept). */
void mods_updates_changed(void);
/* The Shrink movies button without its window (tests: --mods-shrink <game folder>): every superstar / media
   mod's .bik movie (not the bundled mods') and Custom Movies' .bik -> .mp4; the result on the status line. */
int mods_shrink(const WCHAR *game_dir);
/* The Level music button without its window (tests: --mods-level <game folder>): the mods' themes (a gain the
   author left out) and the Music folder's songs at the Mod Maker's -24 LUFS (music_level.h); the dB per song printed. */
int mods_level(const WCHAR *game_dir);

/* A main menu background made from a picture (backgrounds.h; tests:
   --bg-add <game folder> <picture> [logo]). 1 on success; prints the folder. */
int mods_add_background(const WCHAR *game_dir, const WCHAR *image, int logo);
/* Checks the mods shipped with the port (<bundle_dir>\*.svrmod) against what it installed (Mods\.bundled_state):
   installs one only when it isn't there (and the player didn't remove it), or updates an unedited copy to a newer
   version; an edited copy with a newer version shipped, or an unedited one with files missing, is a question
   (mods_bundled_questions), acted on at the next call with its answer (mods_bundled_answer). Never a silent
   reinstall over the player's edits. */
void mods_install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir);
enum { BUNDLED_ASK_UPDATE = 1, BUNDLED_ASK_FIX = 2 };   /* (BundledQuestion.kind) */
enum { BUNDLED_YES = 1, BUNDLED_NO = 2 };   /* update (a backup of theirs) / keep theirs; fix it / not now */
typedef struct {
    int kind;
    WCHAR file[MAX_PATH];  /* the bundle (its .svrmod) */
    WCHAR name[128];       /* its id */
    WCHAR have[32], ship[32];  /* versions: installed, shipped */
} BundledQuestion;
/* The questions of the mods_install_bundled calls since mods_bundled_reset (how many; up to max copied). */
int mods_bundled_questions(BundledQuestion *out, int max);
/* An answer for the next mods_install_bundled (file NULL: for every question - the --bundled tests). */
void mods_bundled_answer(const WCHAR *file, int answer);
void mods_bundled_reset(void);
/* Told before each bundled mod mods_install_bundled installs: n of total (from any thread - it may run on a worker). */
void mods_set_bundled_progress(void (*progress)(int n, int total));
/* Deletes a folder tree directly (no shell, no Recycle Bin). The shell's FO_DELETE took 30 s on each folder a mod
   update had just renamed into Mods\.old (2.1.4: an update installing four match types ran for two minutes before the
   window showed). 1 when it is gone. */
int mods_delete_tree(const WCHAR *dir);
