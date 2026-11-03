/* WWE SmackDown vs. Raw 2011 PC launcher - the Reports tab (reports_tab.c): the reports this game folder sent
   (Report a problem here, in the Mod Maker or on Android: their receipts, report_receipts.h) and what became of
   them - received, being looked at, fixed for the next update, or a question for the player, who can answer it
   here. A report can be changed here until it's done or a duplicate (text to add, pictures). Asked when the tab
   opens. */
#pragma once

#include <windows.h>
#include <stdio.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* The online server's address; the signed-in account's token ("" signed out). */
typedef void (*reports_server_fn)(WCHAR *out, int n);
typedef void (*reports_token_fn)(char *out, size_t n);

/* Builds the page's controls (ids id_base .. id_base + 15). */
void reports_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server, reports_token_fn token);
/* The page shown: the kept receipts, then their state from the server. */
void reports_show(const WCHAR *game_dir);
int reports_command(int id, int code);
int reports_notify(const NMHDR *nm);
/* Its threads' answers (WM_APP + 21); 1 when it was its. */
#define WM_APP_REPORTS (WM_APP + 21)
int reports_message(UINT m, WPARAM wp, LPARAM lp);
/* Tests (--capture reports): 1 while the server is being asked; the list's size and the status line (each may be
   NULL). A row selected (its line, the answer box); the list, each with its state's words, into f. */
int reports_test_state(int *n, WCHAR *line, size_t line_n);
void reports_test_pick(int row);
/* The selected row's change box opened (SVR2011_REPORTS_EDIT). */
void reports_test_edit(void);
void reports_test_dump(FILE *f);
