/* WWE SmackDown vs. Raw 2011 - the reports a player sent and what became of them (report_receipts.c).
   A send's answer carries a receipt (the report's id and a secret, server/reports.py): kept in
   <game>\UserData\reports.json - one report a line, shared by the launcher, the Mod Maker and the Android app -
   with the date, the problem and a short "what happened". The launcher's Reports tab asks the server for their
   state with them (POST /api/reports/status), sends an answer to a question (POST /api/reports/reply) and the
   player's changes to a report (POST /api/reports/edit: more text, pictures - 5 a report in all).
   Plain C: the launcher and the Mod Maker build it. */
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RECEIPTS_MAX = 200, RECEIPT_MAX_IMAGES = 3, RECEIPT_MAX_EDIT_IMAGES = 5 };

typedef struct {
    /* kept on this PC */
    char id[40], secret[80], date[24], app[16], problem[120], what[200];
    int pictures_later;      /* the game's form sent it without pictures, "add them later in the launcher" */
    /* the server's answer ("" until asked) */
    char state[16];          /* received / looking / fixed / needs_info / replied / done / duplicate / unknown */
    char note[1000];         /* fixed: how (reports.py mark --fixed) */
    char version[24];        /* fixed: the update it's in */
    char question[1000];     /* needs_info / replied: our question */
    int replies;             /* the player's answers so far */
    int edits, pictures;     /* the player's changes so far; the pictures it has (sent + added) */
    int from_account;        /* sent from another PC, listed by the signed-in account (no secret here) */
} ReportReceipt;

/* Keeps a sent report's receipt (newest last in the file). 1 on success. */
int receipt_add(const WCHAR *game_dir, const char *app, const char *id, const char *secret, const char *problem,
                const char *what);
/* ... pictures_later: the player chose to add pictures later in the launcher ("pictures_later":1 on its line - the
   Reports tab offers an edit with pictures; left out when 0). */
int receipt_add_ex(const WCHAR *game_dir, const char *app, const char *id, const char *secret, const char *problem,
                   const char *what, int pictures_later);

/* The kept receipts, newest first; how many. */
int receipts_load(const WCHAR *game_dir, ReportReceipt *out, int max);

/* A test run's guard for the writes (an answer): 1 refuses this server (the launcher's test_run_refuses_url). */
typedef int (*receipts_guard_fn)(const WCHAR *server, const char *what);
void receipts_set_guard(receipts_guard_fn guard);

/* Asks the server for their state (token: the signed-in account's, or NULL - its reports from other PCs are added
   after the kept ones, up to max). *n: how many there are now. 1 when it answered; 0 and why when not. */
int receipts_status(const WCHAR *server, const char *token, ReportReceipt *items, int *n, int max, WCHAR *why,
                    int why_n);

/* The player's answer to our question: text (UTF-8) and up to 3 pictures (PNG / JPEG / WebP). 1 when the server
   took it; 0 and why (its words) when not. */
int receipt_reply(const WCHAR *server, const char *token, const ReportReceipt *r, const char *text,
                  const WCHAR *const *images, int n_images, WCHAR *why, int why_n);

/* The player's changes to a report it sent (until it's done or a duplicate): text to add to what happened (UTF-8,
   may be ""), and up to 5 pictures - the report's 5 at most with the ones it has. *pictures: how many it has now.
   1 when the server took it; 0 and why (its words) when not. */
int receipt_edit(const WCHAR *server, const char *token, const ReportReceipt *r, const char *what,
                 const WCHAR *const *images, int n_images, int *pictures, WCHAR *why, int why_n);

/* 1 when the report can still be changed: not done, a duplicate or unknown to the server. */
int receipt_editable(const ReportReceipt *r);

/* 1 when it waits for pictures: sent from the game with "add pictures later", none added yet. */
int receipt_wants_pictures(const ReportReceipt *r);

/* The player's words for its state: a short badge ("Fixed") and the line under it ("Fixed - in the next update
   (2.1.9): <note>", "We need more information: <question>"). */
void receipt_state_text(const ReportReceipt *r, WCHAR *badge, int badge_n, WCHAR *line, int line_n);

#ifdef __cplusplus
}
#endif
