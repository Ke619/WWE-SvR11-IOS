/* WWE SmackDown vs. Raw 2011 - "Report a problem" sent to the online server (the launcher and the Mod Maker):
   POST <online_server>/api/reports, multipart/form-data -
     "report"  application/json: {"format":1, "app", "version", "platform", "os", "problem", "happens", "what",
               "before", "contact", "signature", "updater"} (report_json)
     "zip"     the report zip (report.c / the Mod Maker's), already without tokens, passwords and addresses
     "image0".."image4"  png / jpeg / webp pictures the player picked
     "match"   application/zip (match.zip): a recorded match's folder (report_match.h), 5 MB at most
     "mod0".."modN"  application/zip (<hash>.zip): the match's mods the server lacks (report_mods_check)
   report.json then also has "match": "<folder name>" and "mods": [{hash, name, type, id, version, size, sent}].
   The answer: {"ok":true, "id", "signature", "seen_before":N, "match", "mods_stored", "mods_refused"} or
   {"ok":false, "error"}. Sent streamed (a few hundred MB with mods): Content-Length first, the parts in chunks;
   refused with mods attached (413, or any refusal) it is sent again without them. No account token is sent.
   Plain C and WinHTTP only: the launcher and the Mod Maker build the same file. */
#pragma once

#include <windows.h>
#include <stdint.h>

#include "report_match.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { REPORT_MAX_IMAGES = 5 };

typedef struct {
    const char *json;                              /* the "report" part (report_json), UTF-8 */
    const WCHAR *zip;                              /* the report zip, or NULL */
    const WCHAR *images[REPORT_MAX_IMAGES];        /* pictures, or NULL */
    int n_images;
    const WCHAR *match_zip;                        /* a recorded match (report_zip_dir), or NULL */
    const char *match_name;                        /* its folder's name (UTF-8) */
    const ReportMod *mods;                         /* the match's mods: all listed in report.json */
    int n_mods;
    const WCHAR *mod_zips[REPORT_MAX_MODS];        /* mods[i]'s zip to send, or NULL (on the server / not ticked) */
    void (*progress)(void *ctx, uint64_t sent, uint64_t total);   /* (called from the sending thread) */
    uint64_t save_bytes;                           /* the zip holds the main save (its size: the Sends line), 0 not */
    void *ctx;
} ReportUpload;

typedef struct {
    int status;              /* the HTTP status (0: no answer - offline, no server) */
    char id[64];             /* the report's id on the server */
    int seen_before;         /* earlier reports of the same problem (the server's signature) */
    WCHAR why[300];          /* when it failed: what to show */
    int match_stored;        /* the server kept the match */
    int mods_sent;           /* mod parts sent with the report that got in */
    int mods_stored;         /* ... and kept by the server */
    int mods_dropped;        /* 1: refused with the mods, sent again without them */
    WCHAR mods_why[300];     /* why mods weren't kept (the server's words), "" if all were */
} ReportResult;

/* The "report" part (malloc'd, the caller frees). Every text is JSON-escaped; NULL is "". updater: the updates'
   log's last result (update_log_last: an update's / a repair's end). */
char *report_json(const char *app, const char *version, const char *problem, const char *happens, const char *what,
                  const char *before, const char *contact, const char *signature, const char *updater);

/* A hint for the server's duplicate check: the first line of the newest crash report in <game>\crashes, else the
   newest log's last error line; "" if none (UTF-8). */
void report_signature(const WCHAR *game_dir, char *out, int n);

/* A picture the server takes: png / jpeg / webp by its first bytes, 5 MB at most. 0 with why if not. */
int report_image_ok(const WCHAR *path, WCHAR *why, int why_n);

/* One line saying what will be sent (shown before Send): the match (match_name set) and the mods with a
   mod_zips entry (any non-NULL - before Send, a mark for "will be sent") are named, with their size. */
void report_send_summary(const ReportUpload *u, WCHAR *out, int n);

/* Sends it to <server>/api/reports (server: online_server, e.g. https://sho-ti.me/svr). 1 if the server took it
   (r->id, r->seen_before), else 0 with r->why. Blocks: call it from a worker thread. */
int report_send(const WCHAR *server, const ReportUpload *u, ReportResult *r);

#ifdef __cplusplus
}
#endif
