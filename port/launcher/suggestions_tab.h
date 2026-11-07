/* WWE SmackDown vs. Raw 2011 PC launcher - the Suggestions tab (suggestions_tab.c).
   A player sends an idea (a title, what it would do, up to 3 pictures) to the online server - a report of kind
   "suggestion" (POST /api/reports, no zip, server/reports.py) - and sees the ones this game folder sent (their
   receipts in UserData\suggestions.json, report_receipts.h; signed in, the account's from other PCs too) with our
   answer: Sure (planned), No (not planned), or Done in <version> - each with an optional note. */
#pragma once

#include <windows.h>
#include <stdio.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */
#include "reports_tab.h" /* (reports_server_fn, reports_token_fn) */

/* refuses: a test run's guard for the send (test_run_refuses_url), version: the launcher's ("2.1.9"). */
void suggestions_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server,
                       reports_token_fn token, int (*refuses)(const WCHAR *server, const char *what),
                       const WCHAR *version);
void suggestions_show(const WCHAR *game_dir);
int suggestions_command(int id, int code);
int suggestions_notify(const NMHDR *nm);
/* Its threads' answers (WM_APP + 22); 1 when it was its. */
#define WM_APP_SUGGESTIONS (WM_APP + 22)
int suggestions_message(UINT m, WPARAM wp, LPARAM lp);

/* Tests (--capture suggestions): 1 while it asks / sends; how many it lists and the status line. A send as the Send
   button does (title, text UTF-8; a picture or NULL). A row selected; the list with each answer into f. */
int suggestions_test_state(int *n, WCHAR *line, size_t line_n);
void suggestions_test_send(const char *title, const char *text, const WCHAR *picture);
void suggestions_test_pick(int row);
void suggestions_test_dump(FILE *f);
