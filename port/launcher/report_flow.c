/* WWE SmackDown vs. Raw 2011 - Report a problem's questions and its send as one call (see report_flow.h). */

#include "report_flow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "report.h"
#include "report_receipts.h"
#include "update_log.h"

const WCHAR *const report_kinds[REPORT_KINDS] = { L"The game crashed (closed by itself)", L"The game froze or hung",
                                                  L"Graphics look wrong", L"Sound or music", L"Controller or keyboard",
                                                  L"Online", L"Mods", L"Saves or created content", L"The launcher",
                                                  L"Something else" };
const WCHAR *const report_happens[REPORT_HAPPENS] = { L"Every time", L"Sometimes", L"It happened once" };

const WCHAR *report_flow_check(const ReportAnswers *a)
{
    const char *p = a->what;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    return *p ? NULL : L"Please say what happened first.";
}

void report_flow_describe(const ReportAnswers *a, const WCHAR *version, const char *platform, char *out, int n)
{
    char ver[40];
    WideCharToMultiByte(CP_UTF8, 0, version, -1, ver, sizeof ver, NULL, NULL);
    snprintf(out, (size_t)n,
             "SvR 2011 PC port %s%s%s%s\r\nProblem: %s\r\nHappens: %s\r\n\r\nWhat happened:\r\n%s\r\n\r\nJust before:\r\n%s\r\n",
             ver, platform ? " (" : "", platform ? platform : "", platform ? ")" : "", a->problem, a->happens, a->what,
             a->before[0] ? a->before : "(not said)");
}

void report_match_set_load(ReportMatchSet *m, const WCHAR *game_dir, const WCHAR *server)
{
    int i;
    m->n_mods = m->has_match ? report_match_mods(game_dir, m->match.dir, m->mods, REPORT_MAX_MODS) : 0;
    m->checked = 0, m->max_upload = 0, m->check_why[0] = 0;
    for (i = 0; i < m->n_mods; i++)
        if (m->mods[i].size > REPORT_MOD_MAX) m->mods[i].send = 0;   /* (too big for the server) */
    if (m->n_mods && server && server[0])
        m->checked = report_mods_check(server, m->mods, m->n_mods, &m->max_upload, m->check_why, 200);
}

void report_match_set_parts(ReportMatchSet *m, const WCHAR *game_dir, int with_mods, ReportUpload *u,
                            char *name_utf8, int name_n)
{
    WCHAR dir[MAX_PATH];
    const uint64_t limit = m->max_upload ? m->max_upload : REPORT_MOD_MAX;
    uint64_t total = 0;
    int i;
    if (!m->has_match) return;
    swprintf_s(dir, MAX_PATH, L"%s\\Reports", game_dir);
    CreateDirectoryW(dir, NULL);
    wcscat_s(dir, MAX_PATH, L"\\.upload");
    CreateDirectoryW(dir, NULL);
    swprintf_s(m->match_zip, MAX_PATH, L"%s\\match.zip", dir);
    if (report_zip_dir(m->match.dir, m->match_zip, "")) {
        const WCHAR *name = wcsrchr(m->match.dir, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : m->match.dir, -1, name_utf8, name_n, NULL, NULL);
        u->match_zip = m->match_zip;
        u->match_name = name_utf8;
    }
    u->mods = m->mods;
    u->n_mods = m->n_mods;
    for (i = 0; with_mods && i < m->n_mods; i++) {
        ReportMod *d = &m->mods[i];
        if (d->on_server || !d->hash[0] || !d->send || total + d->size > limit) continue;
        swprintf_s(m->mod_zip[i], MAX_PATH, L"%s\\%S.zip", dir, d->hash);
        if (report_zip_dir(d->folder, m->mod_zip[i], "")) {
            u->mod_zips[i] = m->mod_zip[i];
            total += d->size;
        } else {
            m->mod_zip[i][0] = 0;
        }
    }
}

void report_match_set_done(ReportMatchSet *m)
{
    int i;
    if (m->match_zip[0]) DeleteFileW(m->match_zip), m->match_zip[0] = 0;
    for (i = 0; i < REPORT_MAX_MODS; i++)
        if (m->mod_zip[i][0]) DeleteFileW(m->mod_zip[i]), m->mod_zip[i][0] = 0;
}

int report_flow_send(const ReportFlow *f, ReportResult *r)
{
    static char text[16384];   /* (report.txt's top; one send at a time) */
    WCHAR zip[MAX_PATH] = L"";
    ReportMatchSet *m;
    ReportUpload u;
    char sig[400], ver[40], upd[600], match_name[300] = "";
    char *json;
    int logs = 0, crashes = 0, i, ok = 0;
    ZeroMemory(r, sizeof *r);
    if (f->guard && f->guard(f->server, r)) return 0;
    if ((m = (ReportMatchSet *)calloc(1, sizeof *m)) == NULL) {
        wcscpy_s(r->why, 300, L"Out of memory.");
        return 0;
    }
    report_flow_describe(f->answers, f->version, f->platform, text, sizeof text);
    if (!report_make_match(f->game_dir, f->version, text, NULL, f->with_save, zip, MAX_PATH, &logs, &crashes)) {
        wcscpy_s(r->why, 300, L"The report could not be written (is the game folder read-only?).");
        free(m);
        return 0;
    }
    if (f->match_dir && f->match_dir[0]) {
        wcscpy_s(m->match.dir, MAX_PATH, f->match_dir);
        m->has_match = 1;
        report_match_set_load(m, f->game_dir, f->with_mods ? f->server : NULL);
    }
    WideCharToMultiByte(CP_UTF8, 0, f->version, -1, ver, sizeof ver, NULL, NULL);
    report_signature(f->game_dir, sig, sizeof sig);
    update_log_last(f->game_dir, upd, sizeof upd);
    json = report_json(f->app, ver, f->answers->problem, f->answers->happens, f->answers->what, f->answers->before,
                       f->answers->contact, sig, upd);
    ZeroMemory(&u, sizeof u);
    u.json = json;
    u.zip = zip;
    for (i = 0; i < f->n_images && i < REPORT_MAX_IMAGES; i++) u.images[i] = f->images[i];
    u.n_images = i;
    report_match_set_parts(m, f->game_dir, f->with_mods, &u, match_name, sizeof match_name);
    u.progress = f->progress;
    u.ctx = f->ctx;
    if (json && report_send(f->server, &u, r) && r->secret[0]) {
        receipt_add_ex(f->game_dir, f->app, r->id, r->secret, f->answers->problem, f->answers->what, f->pictures_later);
        ok = 1;
    } else if (json && r->id[0]) {
        ok = 1;   /* (taken, no receipt in the answer) */
    }
    report_match_set_done(m);
    DeleteFileW(zip);
    free(json);
    free(m);
    return ok;
}
