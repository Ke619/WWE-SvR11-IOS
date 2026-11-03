/* WWE SmackDown vs. Raw 2011 - the reports a player sent and what became of them (report_receipts.h). */

#include "report_receipts.h"

#include <share.h>
#include <winhttp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static receipts_guard_fn s_guard;

void receipts_set_guard(receipts_guard_fn guard) { s_guard = guard; }

/* ── a growing buffer, JSON in and out ───────────────────────────────────── */

typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} Buf;

static void add(Buf *b, const void *data, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *m;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(m = (char *)realloc(b->p, cap))) { b->bad = 1; return; }
        b->p = m, b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void add_s(Buf *b, const char *s) { add(b, s, strlen(s)); }

/* "key":"value" (JSON-escaped), with a comma before it unless first. */
static void add_str(Buf *b, const char *key, const char *v, int first)
{
    const unsigned char *p = (const unsigned char *)(v ? v : "");
    char esc[8];
    add_s(b, first ? "\"" : ",\"");
    add_s(b, key);
    add_s(b, "\":\"");
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') { esc[0] = '\\', esc[1] = (char)*p; add(b, esc, 2); }
        else if (*p == '\n') add_s(b, "\\n");
        else if (*p == '\r') add_s(b, "\\r");
        else if (*p == '\t') add_s(b, "\\t");
        else if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); add_s(b, esc); }
        else add(b, p, 1);
    }
    add_s(b, "\"");
}

/* The value of "key" in one flat JSON object [obj, end): a string unescaped (\uXXXX as UTF-8), a number / true /
   false as written. 0 when it has none. (A key inside a string value is escaped there - \"key\" - so the
   unescaped quote before it tells a key.) */
static int field(const char *obj, const char *end, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key), k = 0;
    const char *p;
    if (n) out[0] = 0;
    for (p = obj; p + kl + 2 < end; p++) {
        const char *q;
        if (*p != '"' || (p > obj && p[-1] == '\\') || memcmp(p + 1, key, kl) || p[kl + 1] != '"') continue;
        q = p + kl + 2;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') continue;
        for (q++; q < end && (*q == ' ' || *q == '\t'); q++)
            ;
        if (q < end && *q == '"') {
            for (q++; q < end && *q != '"' && k + 5 < n; q++) {
                if (*q != '\\' || q + 1 >= end) { out[k++] = *q; continue; }
                q++;
                if (*q == 'n') out[k++] = '\n';
                else if (*q == 'r') out[k++] = '\r';
                else if (*q == 't') out[k++] = '\t';
                else if (*q == 'u' && q + 4 < end) {
                    char hex[5];
                    unsigned c;
                    memcpy(hex, q + 1, 4);
                    hex[4] = 0;
                    c = (unsigned)strtoul(hex, NULL, 16);
                    q += 4;
                    if (c < 0x80) out[k++] = (char)c;
                    else if (c < 0x800) out[k++] = (char)(0xC0 | c >> 6), out[k++] = (char)(0x80 | (c & 0x3F));
                    else out[k++] = (char)(0xE0 | c >> 12), out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)),
                         out[k++] = (char)(0x80 | (c & 0x3F));
                } else out[k++] = *q;
            }
        } else {
            while (q < end && *q != ',' && *q != '}' && *q != ' ' && *q != '\r' && *q != '\n' && k + 1 < n)
                out[k++] = *q++;
        }
        if (n) out[k] = 0;
        return 1;
    }
    return 0;
}

/* The next {...} object from *p (strings skipped whole): [*obj, *end) and *p after it; 0 when there's none. */
static int next_object(const char **p, const char **obj, const char **end)
{
    const char *s = *p;
    int depth = 0, in_str = 0;
    while (*s && *s != '{') s++;
    if (!*s) return 0;
    *obj = s;
    for (; *s; s++) {
        if (in_str) {
            if (*s == '\\' && s[1]) s++;
            else if (*s == '"') in_str = 0;
        } else if (*s == '"') in_str = 1;
        else if (*s == '{') depth++;
        else if (*s == '}' && --depth == 0) {
            *end = s + 1;
            *p = s + 1;
            return 1;
        }
    }
    return 0;
}

/* ── the file ────────────────────────────────────────────────────────────── */

static void receipts_path(const WCHAR *game_dir, WCHAR *out)
{
    swprintf_s(out, MAX_PATH, L"%s\\UserData", game_dir);
    CreateDirectoryW(out, NULL);
    wcscat_s(out, MAX_PATH, L"\\reports.json");
}

static char *read_file(const WCHAR *path)
{
    FILE *f = _wfsopen(path, L"rb", _SH_DENYNO);
    long n;
    char *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20) || !(d = (char *)malloc((size_t)n + 1))) { fclose(f); return NULL; }
    d[fread(d, 1, (size_t)n, f)] = 0;
    fclose(f);
    return d;
}

int receipt_add(const WCHAR *game_dir, const char *app, const char *id, const char *secret, const char *problem,
                const char *what)
{
    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    char *old, date[24], short_what[200];
    const char *p, *obj, *end;
    Buf b = { 0 };
    SYSTEMTIME t;
    FILE *f;
    int kept = 0;
    size_t k;
    if (!game_dir || !game_dir[0] || !id || !id[0] || !secret || !secret[0]) return 0;
    receipts_path(game_dir, path);
    add_s(&b, "[\n");
    /* (the ones kept: their lines as they are, the oldest dropped past RECEIPTS_MAX) */
    if ((old = read_file(path)) != NULL) {
        int total = 0, skip;
        for (p = old; next_object(&p, &obj, &end);) total++;
        skip = total >= RECEIPTS_MAX ? total - RECEIPTS_MAX + 1 : 0;
        for (p = old; next_object(&p, &obj, &end);) {
            if (skip > 0) { skip--; continue; }
            if (kept++) add_s(&b, ",\n");
            add(&b, obj, (size_t)(end - obj));
        }
        free(old);
    }
    GetLocalTime(&t);
    snprintf(date, sizeof date, "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    /* (a short "what": its first 160 bytes, not cutting a UTF-8 letter) */
    strncpy_s(short_what, sizeof short_what, what ? what : "", _TRUNCATE);
    if (strlen(short_what) > 160) {
        k = 160;
        while (k && ((unsigned char)short_what[k] & 0xC0) == 0x80) k--;
        short_what[k] = 0;
    }
    if (kept) add_s(&b, ",\n");
    add_s(&b, "{");
    add_str(&b, "id", id, 1);
    add_str(&b, "secret", secret, 0);
    add_str(&b, "date", date, 0);
    add_str(&b, "app", app, 0);
    add_str(&b, "problem", problem, 0);
    add_str(&b, "what", short_what, 0);
    add_s(&b, "}\n]\n");
    if (b.bad) { free(b.p); return 0; }
    swprintf_s(tmp, MAX_PATH, L"%s.tmp", path);
    if (_wfopen_s(&f, tmp, L"wb") || !f) { free(b.p); return 0; }
    k = fwrite(b.p, 1, b.n, f);
    fclose(f);
    free(b.p);
    return k == b.n && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

int receipts_load(const WCHAR *game_dir, ReportReceipt *out, int max)
{
    WCHAR path[MAX_PATH];
    char *d;
    const char *p, *obj, *end;
    int n = 0, i;
    if (!game_dir || !game_dir[0]) return 0;
    receipts_path(game_dir, path);
    if (!(d = read_file(path))) return 0;
    for (p = d; n < max && next_object(&p, &obj, &end);) {
        ReportReceipt *r = &out[n];
        ZeroMemory(r, sizeof *r);
        field(obj, end, "id", r->id, sizeof r->id);
        field(obj, end, "secret", r->secret, sizeof r->secret);
        field(obj, end, "date", r->date, sizeof r->date);
        field(obj, end, "app", r->app, sizeof r->app);
        field(obj, end, "problem", r->problem, sizeof r->problem);
        field(obj, end, "what", r->what, sizeof r->what);
        {
            char v[8];
            r->pictures_later = field(obj, end, "pictures_later", v, sizeof v) && (!strcmp(v, "1") || !strcmp(v, "true"));
        }
        if (r->id[0] && r->secret[0]) n++;
    }
    free(d);
    for (i = 0; i < n / 2; i++) {   /* (newest first) */
        ReportReceipt t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    return n;
}

/* ── the requests ────────────────────────────────────────────────────────── */

/* One POST to <server><path>: 1 when an answer came (*status, *answer malloc'd). */
static int post(const WCHAR *server, const WCHAR *path, const char *ctype, const char *body, size_t n,
                const char *token, DWORD *status, char **answer)
{
    WCHAR url[1000], host[256], upath[900], headers[600];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD sz = sizeof *status, got = 0;
    size_t total = 0, cap = 0;
    int ok = 0;
    *status = 0, *answer = NULL;
    swprintf_s(url, 1000, L"%s%s", wcsstr(server, L"://") ? L"" : L"http://", server);
    while (wcslen(url) && url[wcslen(url) - 1] == L'/') url[wcslen(url) - 1] = 0;
    wcscat_s(url, 1000, path);
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    ses = WinHttpOpen(L"SvR2011 Reports", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 15000, 60000, 60000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"POST", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    swprintf_s(headers, 600, L"Content-Type: %S\r\n%s%S%s", ctype, token && token[0] ? L"Authorization: Bearer " : L"",
               token && token[0] ? token : "", token && token[0] ? L"\r\n" : L"");
    if (req && WinHttpSendRequest(req, headers, (DWORD)-1L, (LPVOID)body, (DWORD)n, (DWORD)n, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            char *m;
            if (total + 4097 > cap) {
                cap = cap ? cap * 2 : 8192;
                if (cap > (4u << 20) || !(m = (char *)realloc(*answer, cap))) break;
                *answer = m;
            }
            if (!WinHttpReadData(req, *answer + total, 4096, &got) || !got) break;
            total += got;
        }
        if (*answer) (*answer)[total] = 0;
        ok = 1;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

/* The server's "error" (else its HTTP status) as the reason. */
static void refused(DWORD status, const char *answer, WCHAR *why, int why_n)
{
    char err[400];
    WCHAR w[400];
    if (!status) { wcscpy_s(why, why_n, L"The server can't be reached (offline, or it's down)."); return; }
    if (answer && field(answer, answer + strlen(answer), "error", err, sizeof err) && err[0]) {
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 400);
        swprintf_s(why, why_n, L"%s", w);
    } else {
        swprintf_s(why, why_n, L"The server said no (HTTP %lu).", status);
    }
}

int receipts_status(const WCHAR *server, const char *token, ReportReceipt *items, int *n, int max, WCHAR *why,
                    int why_n)
{
    Buf b = { 0 };
    DWORD status = 0;
    char *answer = NULL;
    const char *p, *obj, *end;
    int i, ok = 0;
    why[0] = 0;
    if (!server || !server[0]) { wcscpy_s(why, why_n, L"No online server is set."); return 0; }
    if (s_guard && s_guard(server, "/api/reports/status")) {
        wcscpy_s(why, why_n, L"Test run: not a server on this PC (SVR2011_ALLOW_LIVE=1 lets it through).");
        return 0;
    }
    add_s(&b, "{\"reports\":[");
    for (i = 0; i < *n; i++) {
        if (items[i].from_account || !items[i].secret[0]) continue;
        add_s(&b, b.n > 12 ? ",{" : "{");
        add_str(&b, "id", items[i].id, 1);
        add_str(&b, "secret", items[i].secret, 0);
        add_s(&b, "}");
    }
    add_s(&b, "]}");
    if (b.bad) { free(b.p); wcscpy_s(why, why_n, L"Not enough memory."); return 0; }
    post(server, L"/api/reports/status", "application/json; charset=utf-8", b.p, b.n, token, &status, &answer);
    free(b.p);
    if (status == 200 && answer && (p = strstr(answer, "\"reports\"")) != NULL) {
        int keep = *n;
        for (i = 0; i < keep; i++) items[i].state[0] = 0;
        for (; next_object(&p, &obj, &end);) {
            char id[40], v[24];
            ReportReceipt *r = NULL;
            field(obj, end, "id", id, sizeof id);
            for (i = 0; i < *n; i++)
                if (!strcmp(items[i].id, id)) { r = &items[i]; break; }
            if (!r && field(obj, end, "from_account", v, sizeof v) && !strcmp(v, "true") && *n < max) {
                r = &items[(*n)++];   /* (sent from another PC: what the server says of it) */
                ZeroMemory(r, sizeof *r);
                strcpy_s(r->id, sizeof r->id, id);
                r->from_account = 1;
                field(obj, end, "received", r->date, sizeof r->date);
                if (strlen(r->date) > 16) r->date[16] = 0;
                field(obj, end, "app", r->app, sizeof r->app);
                field(obj, end, "problem", r->problem, sizeof r->problem);
                field(obj, end, "what", r->what, sizeof r->what);
            }
            if (!r) continue;
            field(obj, end, "state", r->state, sizeof r->state);
            field(obj, end, "note", r->note, sizeof r->note);
            field(obj, end, "version", r->version, sizeof r->version);
            field(obj, end, "question", r->question, sizeof r->question);
            r->replies = field(obj, end, "replies", v, sizeof v) ? atoi(v) : 0;
            r->edits = field(obj, end, "edits", v, sizeof v) ? atoi(v) : 0;
            r->pictures = field(obj, end, "pictures", v, sizeof v) ? atoi(v) : 0;
        }
        ok = 1;
    } else {
        refused(status, answer, why, why_n);
    }
    free(answer);
    return ok;
}

static const char *image_type(const unsigned char *d, size_t n)
{
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    return NULL;
}

/* A form POST to <server><path> (an answer, a change): the JSON part "report" (freed here) and the pictures
   image0.. (max of them). 1 when it went out (*status, *answer malloc'd); 0 and why when it couldn't. */
static int post_form(const WCHAR *server, const WCHAR *path, const char *token, Buf *json, const WCHAR *const *images,
                     int n_images, int max, DWORD *status, char **answer, WCHAR *why, int why_n)
{
    static const char bound[] = "----svr2011reply7f3a90";
    Buf b = { 0 };
    char head[300], guard_path[80];
    int i;
    why[0] = 0;
    *status = 0, *answer = NULL;
    WideCharToMultiByte(CP_UTF8, 0, path, -1, guard_path, sizeof guard_path, NULL, NULL);
    if (!server || !server[0]) { free(json->p); wcscpy_s(why, why_n, L"No online server is set."); return 0; }
    if (s_guard && s_guard(server, guard_path)) {
        free(json->p);
        wcscpy_s(why, why_n, L"Test run: not a server on this PC (SVR2011_ALLOW_LIVE=1 lets it through).");
        return 0;
    }
    snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"report\"\r\nContent-Type: "
                                "application/json; charset=utf-8\r\n\r\n", bound);
    add_s(&b, head);
    add(&b, json->p ? json->p : "{}", json->p ? json->n : 2);
    add_s(&b, "\r\n");
    free(json->p);
    for (i = 0; i < n_images && i < max; i++) {
        FILE *f = _wfsopen(images[i], L"rb", _SH_DENYNO);
        unsigned char *d;
        long len;
        const char *type;
        const WCHAR *name = wcsrchr(images[i], L'\\');
        if (!f) { swprintf_s(why, why_n, L"%s can't be read.", name ? name + 1 : images[i]); free(b.p); return 0; }
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > (5 << 20) || !(d = (unsigned char *)malloc((size_t)len))) {
            fclose(f);
            swprintf_s(why, why_n, L"%s can't be added: it's over 5 MB.", name ? name + 1 : images[i]);
            free(b.p);
            return 0;
        }
        len = (long)fread(d, 1, (size_t)len, f);
        fclose(f);
        if (!(type = image_type(d, (size_t)len))) {
            free(d);
            swprintf_s(why, why_n, L"%s isn't a PNG, JPEG or WebP picture.", name ? name + 1 : images[i]);
            free(b.p);
            return 0;
        }
        snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"image%d\"; filename=\"image%d.%s\""
                                    "\r\nContent-Type: %s\r\n\r\n", bound, i, i, type + 6, type);
        add_s(&b, head);
        add(&b, d, (size_t)len);
        add_s(&b, "\r\n");
        free(d);
    }
    snprintf(head, sizeof head, "--%s--\r\n", bound);
    add_s(&b, head);
    if (b.bad) { free(b.p); wcscpy_s(why, why_n, L"Not enough memory."); return 0; }
    snprintf(head, sizeof head, "multipart/form-data; boundary=%s", bound);
    post(server, path, head, b.p, b.n, token, status, answer);
    free(b.p);
    return 1;
}

int receipt_reply(const WCHAR *server, const char *token, const ReportReceipt *r, const char *text,
                  const WCHAR *const *images, int n_images, WCHAR *why, int why_n)
{
    Buf json = { 0 };
    DWORD status = 0;
    char *answer = NULL;
    int ok;
    add_s(&json, "{");
    add_str(&json, "id", r->id, 1);
    add_str(&json, "secret", r->secret, 0);
    add_str(&json, "text", text, 0);
    add_s(&json, "}");
    if (!post_form(server, L"/api/reports/reply", token, &json, images, n_images, RECEIPT_MAX_IMAGES, &status, &answer,
                   why, why_n))
        return 0;
    ok = status == 200;
    if (!ok) refused(status, answer, why, why_n);
    free(answer);
    return ok;
}

int receipt_edit(const WCHAR *server, const char *token, const ReportReceipt *r, const char *what,
                 const WCHAR *const *images, int n_images, int *pictures, WCHAR *why, int why_n)
{
    Buf json = { 0 };
    DWORD status = 0;
    char *answer = NULL, v[16];
    int ok;
    add_s(&json, "{");
    add_str(&json, "id", r->id, 1);
    add_str(&json, "secret", r->secret, 0);
    add_str(&json, "what", what, 0);
    add_s(&json, "}");
    if (!post_form(server, L"/api/reports/edit", token, &json, images, n_images, RECEIPT_MAX_EDIT_IMAGES, &status,
                   &answer, why, why_n))
        return 0;
    ok = status == 200;
    if (ok && pictures && answer && field(answer, answer + strlen(answer), "pictures", v, sizeof v)) *pictures = atoi(v);
    if (!ok) refused(status, answer, why, why_n);
    free(answer);
    return ok;
}

int receipt_editable(const ReportReceipt *r)
{
    return strcmp(r->state, "done") && strcmp(r->state, "duplicate") && strcmp(r->state, "unknown");
}

int receipt_wants_pictures(const ReportReceipt *r)
{
    return r->pictures_later && !r->pictures && receipt_editable(r);
}

void receipt_state_text(const ReportReceipt *r, WCHAR *badge, int badge_n, WCHAR *line, int line_n)
{
    WCHAR note[1000], ver[24];
    const char *s = r->state;
    MultiByteToWideChar(CP_UTF8, 0, r->note, -1, note, 1000);
    MultiByteToWideChar(CP_UTF8, 0, r->version, -1, ver, 24);
    line[0] = 0;
    if (!s[0]) {
        wcscpy_s(badge, badge_n, L"\x2026");
        wcscpy_s(line, line_n, L"Not checked yet.");
    } else if (!strcmp(s, "received")) {
        wcscpy_s(badge, badge_n, L"Received");
        wcscpy_s(line, line_n, L"We have your report and will look at it.");
    } else if (!strcmp(s, "looking")) {
        wcscpy_s(badge, badge_n, L"Looking at it");
        wcscpy_s(line, line_n, L"We're looking at this one now.");
    } else if (!strcmp(s, "fixed")) {
        wcscpy_s(badge, badge_n, L"Fixed");
        if (ver[0]) swprintf_s(line, line_n, L"Fixed - in the next update (%s)%s%s", ver, note[0] ? L": " : L".", note);
        else swprintf_s(line, line_n, L"Fixed - in the next update%s%s", note[0] ? L": " : L".", note);
    } else if (!strcmp(s, "needs_info")) {
        WCHAR q[1000];
        MultiByteToWideChar(CP_UTF8, 0, r->question, -1, q, 1000);
        wcscpy_s(badge, badge_n, L"Needs your answer");
        swprintf_s(line, line_n, L"We need more information: %s", q);
    } else if (!strcmp(s, "replied")) {
        WCHAR q[1000];
        MultiByteToWideChar(CP_UTF8, 0, r->question, -1, q, 1000);
        wcscpy_s(badge, badge_n, L"Answered");
        swprintf_s(line, line_n, L"Thanks - we have your answer to: %s", q);
    } else if (!strcmp(s, "done")) {
        wcscpy_s(badge, badge_n, L"Done");
        wcscpy_s(line, line_n, L"We've dealt with this report.");
    } else if (!strcmp(s, "duplicate")) {
        wcscpy_s(badge, badge_n, L"Already reported");
        wcscpy_s(line, line_n, L"Someone reported the same problem first - we're on it there.");
    } else {
        wcscpy_s(badge, badge_n, L"Not found");
        wcscpy_s(line, line_n, L"The server doesn't know this report any more.");
    }
}
