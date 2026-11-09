/* WWE SmackDown vs. Raw 2011 PC launcher - Mods -> Discovery (discover_tab.c): players' .svrmod mods on the
   online server (server/discovery.py) - search them, see their pictures, download and install them, upload your
   own (signed in online: its name is the author). */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* The online server's base address (e.g. https://sho-ti.me/svr) and the signed-in account: its token, name
   ("" signed out) and id (*id: -1 signed out) - a mod is the player's own when its owner is that id. */
typedef void (*discover_server_fn)(WCHAR *out, int n);
typedef void (*discover_account_fn)(char *token, size_t token_n, WCHAR *name, size_t name_n, long long *id);

/* Builds the page's controls (ids id_base .. id_base + 63). */
void discover_build(HWND wnd, mods_add_fn add, int tab, int id_base, discover_server_fn server,
                    discover_account_fn account);
/* Where Discovery remembers the last visit and the versions downloaded from it (launcher.ini). */
void discover_set_ini(const WCHAR *ini);
/* The page shown (the first time: the newest mods). */
void discover_show(const WCHAR *game_dir);
int discover_command(int id, int code);
int discover_notify(const NMHDR *nm);
/* Its owner-drawn picture (WM_DRAWITEM); 1 when it was its. */
int discover_draw(const DRAWITEMSTRUCT *d);
/* Installed from Discovery (2.2.2): the launcher's "is the game running" (asked before an update replaces a mod). */
void discover_set_running(int (*fn)(void));
/* An installed mod's folder (full path): 0 not from Discovery (or not checked yet), 1 from Discovery and current,
   2 a newer version is on Discovery (discover_check_installed asked). */
int discover_folder_state(const WCHAR *folder);
/* Updates that folder's mod from Discovery (the same download and install as its page's). 1 if it started. */
int discover_update_folder(const WCHAR *folder);
/* Asks the server about every mod installed from Discovery (once a minute at most); the Installed Mods list is told
   (mods_updates_changed) as the answers come. */
void discover_check_installed(const WCHAR *game_dir);
/* A download or the check runs (tests wait for it). */
int discover_busy(void);
/* The grid's tiles (NM_CUSTOMDRAW from its list): 1 if it was the grid's, *result the window's answer. */
int discover_custom_draw(const NMHDR *nm, LRESULT *result);
/* Tests (--capture): 1 while a download runs; its bar (0.1 %, -1: none), whether Delete shows, the status line. */
int discover_test_state(int *permille, int *owner_buttons, WCHAR *line, size_t n);
/* Its threads' answers (WM_APP + 20); 1 when it was its. */
#define WM_APP_DISCOVER (WM_APP + 20)
int discover_message(UINT m, WPARAM wp, LPARAM lp);
