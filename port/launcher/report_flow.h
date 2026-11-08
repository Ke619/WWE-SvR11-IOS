/* WWE SmackDown vs. Raw 2011 - Report a problem's questions and its send as one call (report_flow.c): the
   launcher's window and the game's own REPORT A PROBLEM page (src/report_page.cpp) ask the same questions and send
   the same report - the zip (report.c), the recorded match and its mods (report_match.h), report.json, the
   pictures (report_send.h) - and keep the same receipt (report_receipts.h). Plain C and WinHTTP. */
#pragma once

#include <windows.h>
#include <stdint.h>

#include "report_match.h"
#include "report_send.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The questions' choices (the launcher's lists): what kind of problem, how often. */
enum { REPORT_KINDS = 10, REPORT_HAPPENS = 3 };
extern const WCHAR *const report_kinds[REPORT_KINDS];
extern const WCHAR *const report_happens[REPORT_HAPPENS];

/* The answers, UTF-8. "What happened" is the one that must be said (report_flow_check). */
typedef struct {
    char problem[120], happens[40], what[6000], before[6000], contact[200];
} ReportAnswers;

/* What's wrong with the answers before Send (the launcher's words), or NULL when they can go. 2.2.1: the contact
   is the player's Discord name and needed (report_discord_check). */
const WCHAR *report_flow_check(const ReportAnswers *a);

/* report.txt's top: the version, the answers. */
void report_flow_describe(const ReportAnswers *a, const WCHAR *version, const char *platform, char *out, int n);

/* A recorded match with the mods it used, as one report sends it (the launcher's picker and the game). */
typedef struct {
    ReportMatch match;       /* the match picked */
    int has_match;
    ReportMod mods[REPORT_MAX_MODS];  /* the mods it used */
    int n_mods;
    int checked;             /* the server said which it has */
    uint64_t max_upload;     /* the server's limit for new mod bytes (0: not said) */
    WCHAR check_why[200];    /* why the check didn't work, or the server's "no mods now" */
    WCHAR match_zip[MAX_PATH];
    WCHAR mod_zip[REPORT_MAX_MODS][MAX_PATH];
} ReportMatchSet;

#define REPORT_MOD_MAX (300ull << 20)   /* (the server's cap per mod and per report) */

/* The mods the match (m->match, m->has_match) used, hashed (all ticked, but those too big for the server), and
   (server not NULL / "") which the server lacks. */
void report_match_set_load(ReportMatchSet *m, const WCHAR *game_dir, const WCHAR *server);
/* The match's zip and the mods the server lacks (with_mods) into u, under <game>\Reports\.upload; name_utf8: the
   match folder's name. report_match_set_done deletes them after the send. */
void report_match_set_parts(ReportMatchSet *m, const WCHAR *game_dir, int with_mods, ReportUpload *u,
                            char *name_utf8, int name_n);
void report_match_set_done(ReportMatchSet *m);

/* One whole send (blocks: a worker thread): the zip with logs / crashes (and the main save), the match and its
   mods, report.json, the pictures, to <server>/api/reports; the receipt kept (pictures_later: the player adds
   pictures later from the launcher - "pictures_later":1 on it). guard (or NULL): a test run's refusal of a server
   off this PC - 1 refuses, r->why set by it. 1 when the server took it. */
typedef struct {
    const WCHAR *game_dir;
    const WCHAR *version;        /* PORT_VERSION */
    const char *app;             /* "launcher", "game" */
    const char *platform;        /* report.txt's "(Android)" etc., or NULL */
    const ReportAnswers *answers;
    const WCHAR *server;         /* online_server */
    const WCHAR *match_dir;      /* a recorded match, or NULL */
    int with_mods;               /* the match's mods the server lacks */
    int with_save;               /* the main save */
    const WCHAR *images[REPORT_MAX_IMAGES];
    int n_images;
    int pictures_later;
    int (*guard)(const WCHAR *server, ReportResult *r);
    void (*progress)(void *ctx, uint64_t sent, uint64_t total);
    void *ctx;
    const char *token;           /* the saved online sign-in (2.2.1: needed - report_send.h), NULL / "" signed out */
} ReportFlow;
int report_flow_send(const ReportFlow *f, ReportResult *r);

#ifdef __cplusplus
}
#endif
