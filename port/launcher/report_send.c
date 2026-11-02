/* WWE SmackDown vs. Raw 2011 - "Report a problem" sent to the online server (report_send.h). */

#include "report_send.h"

#include <share.h>
#include <winhttp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── a growing buffer ─────────────────────────────────────────────────────── */

typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} Buf;

static void buf_add(Buf *b, const void *data, size_t n)
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
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

/* ── report.json ──────────────────────────────────────────────────────────── */

static void json_str(Buf *b, const char *key, const char *v, int first)
{
    const unsigned char *p = (const unsigned char *)(v ? v : "");
    char esc[8];
    if (!first) buf_str(b, ",");
    buf_str(b, "\"");
    buf_str(b, key);
    buf_str(b, "\":\"");
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') { esc[0] = '\\', esc[1] = (char)*p; buf_add(b, esc, 2); }
        else if (*p == '\n') buf_str(b, "\\n");
        else if (*p == '\r') buf_str(b, "\\r");
        else if (*p == '\t') buf_str(b, "\\t");
        else if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); buf_str(b, esc); }
        else buf_add(b, p, 1);
    }
    buf_str(b, "\"");
}

char *report_json(const char *app, const char *version, const char *problem, const char *happens, const char *what,
                  const char *before, const char *contact, const char *signature, const char *updater)
{
    Buf b = { 0 };
    char os[64];
    OSVERSIONINFOEXW vi;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (get && get(&vi) == 0)
        snprintf(os, sizeof os, "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    else
        strcpy(os, "?");
    buf_str(&b, "{\"format\":1,");
    json_str(&b, "app", app, 1);
    json_str(&b, "version", version, 0);
    json_str(&b, "platform", "windows", 0);
    json_str(&b, "os", os, 0);
    json_str(&b, "problem", problem, 0);
    json_str(&b, "happens", happens, 0);
    json_str(&b, "what", what, 0);
    json_str(&b, "before", before, 0);
    json_str(&b, "contact", contact, 0);
    json_str(&b, "signature", signature, 0);
    json_str(&b, "updater", updater, 0);
    buf_str(&b, "}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* ── the signature hint ───────────────────────────────────────────────────── */

static uint8_t *read_all(const WCHAR *path, size_t *n, size_t max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    uint8_t *d = NULL;
    DWORD got = 0;
    *n = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart <= (LONGLONG)max && (d = (uint8_t *)malloc((size_t)sz.QuadPart + 1)) &&
        ReadFile(h, d, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
        d[got] = 0;
        *n = got;
    } else {
        free(d);
        d = NULL;
    }
    CloseHandle(h);
    return d;
}

/* The newest file in dir matching pattern (its full path in out); 0 if none. */
static int newest(const WCHAR *dir, const WCHAR *pattern, WCHAR *out, int n)
{
    WCHAR glob[MAX_PATH];
    WIN32_FIND_DATAW fd;
    FILETIME best = { 0 };
    HANDLE f;
    int found = 0;
    swprintf_s(glob, MAX_PATH, L"%s\\%s", dir, pattern);
    if ((f = FindFirstFileW(glob, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            swprintf_s(out, n, L"%s\\%s", dir, fd.cFileName);
            found = 1;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

static void copy_line(const char *from, const char *end, char *out, int n)
{
    int k = 0;
    while (from < end && (*from == ' ' || *from == '\t')) from++;
    while (from < end && *from != '\r' && *from != '\n' && k + 1 < n) out[k++] = *from++;
    out[k] = 0;
}

/* Lines every session logs that say nothing about a problem (the server's _HARMLESS, reports.py): the emulator's
   start-up heap probing, the game's look-ups of optional files (DLC, SeasonVO), the XMA / kernel stubs. */
static int harmless(const char *line, const char *end)
{
    static const char *const k[] = { "BaseHeap::Alloc", "PhysicalHeap::Alloc", "ResolvePath(", "NtCreateFile",
                                     "XMA: Write to unknown register", "VFS: '", "IoDismountVolume", "Stub", "STUB",
                                     "SeasonVO", "catalog_hd.arc", "gamecontrollerdb" };
    size_t len = (size_t)(end - line), i, j;
    for (i = 0; i < sizeof k / sizeof *k; i++) {
        size_t kl = strlen(k[i]);
        for (j = 0; j + kl <= len; j++)
            if (line[j] == k[i][0] && !memcmp(line + j, k[i], kl)) return 1;
    }
    return 0;
}

void report_signature(const WCHAR *game_dir, char *out, int n)
{
    WCHAR dir[MAX_PATH], log[MAX_PATH], file[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA la, ca;
    uint8_t *d;
    size_t len;
    int have_log;
    out[0] = 0;
    swprintf_s(dir, MAX_PATH, L"%s\\logs", game_dir);
    have_log = newest(dir, L"svr2011_*.log", log, MAX_PATH) && GetFileAttributesExW(log, GetFileExInfoStandard, &la);
    /* a crash report of this session (written after its log began; an empty one - the game closed while writing -
       says nothing) */
    swprintf_s(dir, MAX_PATH, L"%s\\crashes", game_dir);
    if (newest(dir, L"crash_*.txt", file, MAX_PATH) && GetFileAttributesExW(file, GetFileExInfoStandard, &ca) &&
        (!have_log || CompareFileTime(&ca.ftLastWriteTime, &la.ftCreationTime) >= 0) &&
        (d = read_all(file, &len, 1u << 20)) != NULL) {
        copy_line((const char *)d, (const char *)d + len, out, n);  /* ("SvR 2011 crash: <reason> (thread ...)") */
        free(d);
        if (out[0]) return;
    }
    if (have_log && (d = read_all(log, &len, 64u << 20)) != NULL) {
        const char *p = (const char *)d, *end = p + len, *hit = NULL;
        for (; p < end; p++)  /* (the last "[error]" line that isn't a harmless one) */
            if (*p == '[' && end - p > 7 && !memcmp(p, "[error]", 7)) {
                const char *eol = p;
                while (eol < end && *eol != '\n') eol++;
                if (!harmless(p, eol)) hit = p;
            }
        if (hit) {
            while (hit < end && *hit != ']') hit++;      /* (after the level: "[core] [tNNN] text") */
            copy_line(hit + 1, end, out, n);
        }
        free(d);
    }
}

/* ── pictures ─────────────────────────────────────────────────────────────── */

static const char *image_type(const uint8_t *d, size_t n)
{
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    return NULL;
}

int report_image_ok(const WCHAR *path, WCHAR *why, int why_n)
{
    size_t n;
    uint8_t *d = read_all(path, &n, 5u << 20);
    const WCHAR *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (!d) {
        swprintf_s(why, why_n, L"%s can't be added: it can't be read, or it's over 5 MB.", name);
        return 0;
    }
    if (!image_type(d, n)) {
        free(d);
        swprintf_s(why, why_n, L"%s isn't a PNG, JPEG or WebP picture.", name);
        return 0;
    }
    free(d);
    return 1;
}

/* ── sending ──────────────────────────────────────────────────────────────── */

void report_send_summary(const ReportUpload *u, WCHAR *out, int n)
{
    WCHAR match[40] = L"", mods[80] = L"", sav[60] = L"";
    uint64_t bytes = 0;
    int i, k = 0;
    if (u->match_name) wcscpy_s(match, 40, L", the match recording");
    for (i = 0; i < u->n_mods && i < REPORT_MAX_MODS; i++)
        if (u->mod_zips[i]) k++, bytes += u->mods[i].size;
    if (k)
        swprintf_s(mods, 80, bytes >= (10ull << 20) ? L", %d mod%s (%.0f MB)" : L", %d mod%s (%.1f MB)", k,
                   k == 1 ? L"" : L"s", bytes / 1048576.0);
    if (u->save_bytes) swprintf_s(sav, 60, L", your main save (%.1f MB)", u->save_bytes / 1048576.0);
    swprintf_s(out, n, L"Sends: your answers%s%s%s%s%s, %d picture%s - no password, token or internet addresses.",
               u->zip ? L", the report zip (newest logs, crash reports, settings" : L"",
               u->zip ? L")" : L"", sav, match, mods, u->n_images, u->n_images == 1 ? L"" : L"s");
}

/* A JSON answer's string or number value for "key" (a flat object: no nesting needed here). */
static int json_get(const char *j, const char *key, char *out, int n)
{
    char pat[64];
    const char *p;
    int k = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!j || !(p = strstr(j, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k + 1 < n; p++) {
            if (*p == '\\' && p[1]) p++;
            out[k++] = *p;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && k + 1 < n) out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

/* ── the body, streamed: pieces of memory or whole files ─────────────────── */

typedef struct {
    char *mem;               /* (owned) */
    size_t mem_n;
    const WCHAR *file;
    uint64_t file_n;
} Piece;

typedef struct {
    Piece p[16 + 2 * REPORT_MAX_IMAGES + 2 * REPORT_MAX_MODS + 8];
    int n;
    uint64_t total;
} Body;

static void body_mem(Body *b, const char *s, size_t n)
{
    char *m = (char *)malloc(n ? n : 1);
    if (!m || b->n >= (int)(sizeof b->p / sizeof *b->p)) { free(m); return; }
    memcpy(m, s, n);
    b->p[b->n].mem = m, b->p[b->n].mem_n = n, b->p[b->n].file = NULL;
    b->n++;
    b->total += n;
}

static void body_head(Body *b, const char *bound, const char *name, const char *file, const char *ctype)
{
    char h[500];
    int n = snprintf(h, sizeof h, "--%s\r\nContent-Disposition: form-data; name=\"%s\"%s%s%s\r\nContent-Type: %s\r\n\r\n",
                     bound, name, file ? "; filename=\"" : "", file ? file : "", file ? "\"" : "", ctype);
    body_mem(b, h, (size_t)n);
}

static int body_file(Body *b, const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || b->n >= (int)(sizeof b->p / sizeof *b->p)) return 0;
    b->p[b->n].mem = NULL, b->p[b->n].file = path;
    b->p[b->n].file_n = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    b->total += b->p[b->n].file_n;
    b->n++;
    return 1;
}

static void body_free(Body *b)
{
    int i;
    for (i = 0; i < b->n; i++) free(b->p[i].mem);
    b->n = 0, b->total = 0;
}

/* report.json with "match" and "mods" put in (sent: the mod's zip goes with this attempt). */
static char *json_with_match(const ReportUpload *u, int with_mods)
{
    Buf b = { 0 };
    size_t n = strlen(u->json);
    int i;
    while (n && u->json[n - 1] != '}') n--;
    if (!n || (!u->match_name && !u->n_mods)) { buf_str(&b, u->json); return b.p; }
    buf_add(&b, u->json, n - 1);
    if (u->match_name) json_str(&b, "match", u->match_name, 0);
    if (u->n_mods) {
        buf_str(&b, ",\"mods\":[");
        for (i = 0; i < u->n_mods; i++) {
            const ReportMod *m = &u->mods[i];
            char type[80], id[300], size[32];
            WideCharToMultiByte(CP_UTF8, 0, m->type, -1, type, sizeof type, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, m->id, -1, id, sizeof id, NULL, NULL);
            snprintf(size, sizeof size, "%llu", (unsigned long long)m->size);
            buf_str(&b, i ? ",{" : "{");
            json_str(&b, "hash", m->hash, 1);
            json_str(&b, "name", m->name, 0);
            json_str(&b, "type", type, 0);
            json_str(&b, "id", id, 0);
            json_str(&b, "version", m->version, 0);
            buf_str(&b, ",\"size\":");
            buf_str(&b, size);
            buf_str(&b, with_mods && u->mod_zips[i] ? ",\"sent\":true}" : ",\"sent\":false}");
        }
        buf_str(&b, "]");
    }
    buf_str(&b, "}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* One POST: 1 when an HTTP answer came (*status, *answer malloc'd), 0 on no answer / a local error (why). */
static int post_once(const WCHAR *server, const ReportUpload *u, int with_mods, DWORD *status, char **answer,
                     int *mods_in, WCHAR *why, int why_n)
{
    static const char bound[] = "----svr2011report5d2e81";
    Body body;
    WCHAR url[1000], host[256], upath[900], headers[200];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD sz = sizeof *status, got = 0;
    size_t total = 0, cap = 0;
    uint64_t done = 0;
    char *json = json_with_match(u, with_mods);
    int i, ok = 0;
    static uint8_t chunk[64 << 10];
    *status = 0, *answer = NULL, *mods_in = 0;
    ZeroMemory(&body, sizeof body);
    if (!json) { wcscpy_s(why, why_n, L"Not enough memory to send the report."); return 0; }
    body_head(&body, bound, "report", NULL, "application/json; charset=utf-8");
    body_mem(&body, json, strlen(json));
    body_mem(&body, "\r\n", 2);
    free(json);
    if (u->zip) {
        body_head(&body, bound, "zip", "report.zip", "application/zip");
        if (!body_file(&body, u->zip)) { wcscpy_s(why, why_n, L"The report zip can't be read."); body_free(&body); return 0; }
        body_mem(&body, "\r\n", 2);
    }
    for (i = 0; i < u->n_images && i < REPORT_MAX_IMAGES; i++) {
        size_t n;
        uint8_t head[16];
        uint8_t *d = u->images[i] ? read_all(u->images[i], &n, 5u << 20) : NULL;
        const char *type = d ? image_type(d, n) : NULL;
        char name[16], file[24];
        (void)head;
        free(d);
        if (!type) {
            report_image_ok(u->images[i] ? u->images[i] : L"?", why, why_n);
            body_free(&body);
            return 0;
        }
        snprintf(name, sizeof name, "image%d", i);
        snprintf(file, sizeof file, "image%d.%s", i, type + 6);
        body_head(&body, bound, name, file, type);
        body_file(&body, u->images[i]);
        body_mem(&body, "\r\n", 2);
    }
    if (u->match_zip) {
        body_head(&body, bound, "match", "match.zip", "application/zip");
        if (body_file(&body, u->match_zip)) body_mem(&body, "\r\n", 2);
    }
    for (i = 0; with_mods && i < u->n_mods && i < REPORT_MAX_MODS; i++) {
        char name[16], file[80];
        if (!u->mod_zips[i]) continue;
        snprintf(name, sizeof name, "mod%d", *mods_in);
        snprintf(file, sizeof file, "%s.zip", u->mods[i].hash);
        body_head(&body, bound, name, file, "application/zip");
        if (body_file(&body, u->mod_zips[i])) { body_mem(&body, "\r\n", 2); ++*mods_in; }
    }
    {
        char end[64];
        int n = snprintf(end, sizeof end, "--%s--\r\n", bound);
        body_mem(&body, end, (size_t)n);
    }
    /* the request */
    swprintf_s(url, 1000, L"%s%s", wcsstr(server, L"://") ? L"" : L"http://", server);  /* (a custom server: host:port) */
    while (wcslen(url) && url[wcslen(url) - 1] == L'/') url[wcslen(url) - 1] = 0;
    wcscat_s(url, 1000, L"/api/reports");
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        swprintf_s(why, why_n, L"The server address isn't a web address: %s", server);
        body_free(&body);
        return 0;
    }
    ses = WinHttpOpen(L"SvR2011 Report", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 15000, 120000, 120000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"POST", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    swprintf_s(headers, 200, L"Content-Type: multipart/form-data; boundary=%S\r\n", bound);
    if (req && body.total < 0xFFFFFFFFull &&
        WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)body.total, 0)) {
        int sent_ok = 1;
        if (u->progress) u->progress(u->ctx, 0, body.total);
        for (i = 0; i < body.n && sent_ok; i++) {
            const Piece *p = &body.p[i];
            if (p->mem) {
                DWORD w = 0;
                sent_ok = WinHttpWriteData(req, p->mem, (DWORD)p->mem_n, &w) && w == p->mem_n;
                done += p->mem_n;
            } else {
                FILE *f = _wfsopen(p->file, L"rb", _SH_DENYNO);
                uint64_t left = p->file_n;
                if (!f) { sent_ok = 0; break; }
                while (left && sent_ok) {
                    size_t want = left < sizeof chunk ? (size_t)left : sizeof chunk, r = fread(chunk, 1, want, f);
                    DWORD w = 0;
                    if (r != want) { sent_ok = 0; break; }   /* (the file changed size since: Content-Length is fixed) */
                    sent_ok = WinHttpWriteData(req, chunk, (DWORD)r, &w) && w == r;
                    left -= r, done += r;
                    if (u->progress) u->progress(u->ctx, done, body.total);
                }
                fclose(f);
            }
        }
        if (sent_ok && u->progress) u->progress(u->ctx, body.total, body.total);
        if (WinHttpReceiveResponse(req, NULL)) {   /* (an early answer - 413 - arrives even if the write stopped) */
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                status, &sz, WINHTTP_NO_HEADER_INDEX);
            for (;;) {
                char *m;
                if (total + 4097 > cap) {
                    cap = cap ? cap * 2 : 8192;
                    if (cap > (1u << 20) || !(m = (char *)realloc(*answer, cap))) break;
                    *answer = m;
                }
                if (!WinHttpReadData(req, *answer + total, 4096, &got) || !got) break;
                total += got;
            }
            if (*answer) (*answer)[total] = 0;
            ok = 1;
        }
    }
    if (!ok && !why[0]) wcscpy_s(why, why_n, L"The server can't be reached (offline, or it's down).");
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    body_free(&body);
    return ok;
}

/* How many 64-hex hashes the array "key" holds in the answer. */
static int count_hashes(const char *j, const char *key)
{
    char pat[64];
    const char *p, *end;
    int n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!j || !(p = strstr(j, pat)) || !(p = strchr(p, '[')) || !(end = strchr(p, ']'))) return 0;
    for (; p < end; p++)
        if (*p == '"') {
            const char *q = p + 1;
            while (q < end && ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f'))) q++;
            if (q - p - 1 == 64 && *q == '"') n++;
            p = q;
        }
    return n;
}

int report_send(const WCHAR *server, const ReportUpload *u, ReportResult *r)
{
    DWORD status = 0;
    char *answer = NULL, value[300];
    int ok = 0, mods_in = 0, any_mods = 0, i;
    ZeroMemory(r, sizeof *r);
    if (!server || !server[0] || !u->json) {
        wcscpy_s(r->why, 300, L"No server is set (online_server).");
        return 0;
    }
    for (i = 0; i < u->n_mods && i < REPORT_MAX_MODS; i++) any_mods |= u->mod_zips[i] != NULL;
    post_once(server, u, 1, &status, &answer, &mods_in, r->why, 300);
    if (any_mods && status != 200) {
        /* (refused - too big, the mod store off - or cut off while sending: the report without the mods) */
        WCHAR first[300];
        swprintf_s(first, 300, status ? L"the server refused the report with the mods (HTTP %lu)"
                                      : L"the upload with the mods didn't get through", status);
        if (status && json_get(answer, "error", value, sizeof value) && value[0]) {
            WCHAR w[260];
            MultiByteToWideChar(CP_UTF8, 0, value, -1, w, 260);
            swprintf_s(first, 300, L"the server refused the mods: %s", w);
        }
        free(answer);
        answer = NULL;
        r->why[0] = 0;
        post_once(server, u, 0, &status, &answer, &mods_in, r->why, 300);
        r->mods_dropped = 1;
        wcscpy_s(r->mods_why, 300, first);
    }
    r->status = (int)status;
    if (status == 200 && json_get(answer, "id", r->id, sizeof r->id) && r->id[0]) {
        if (json_get(answer, "seen_before", value, sizeof value)) r->seen_before = atoi(value);
        r->match_stored = json_get(answer, "match", value, sizeof value) && !strcmp(value, "true");
        r->mods_sent = mods_in;
        r->mods_stored = count_hashes(answer, "mods_stored");
        if (!r->mods_why[0] && json_get(answer, "mods_refused", value, sizeof value)) {
            /* ({"<hash>": "why", ...}: the first reason) */
            const char *p = strstr(answer, "\"mods_refused\"");
            const char *colon = p ? strchr(p + 14, ':') : NULL;
            const char *q = colon ? strchr(colon + 1, ':') : NULL;
            const char *close = p ? strchr(p, '}') : NULL;
            if (q && (!close || q < close)) {
                char reason[200];
                int k = 0;
                for (q++; *q == ' ' || *q == '"'; q++)
                    ;
                while (*q && *q != '"' && k + 1 < (int)sizeof reason) reason[k++] = *q++;
                reason[k] = 0;
                MultiByteToWideChar(CP_UTF8, 0, reason, -1, r->mods_why, 300);
            }
        }
        ok = 1;
    } else if (!status) {
        if (!r->why[0]) wcscpy_s(r->why, 300, L"The server can't be reached (offline, or it's down).");
    } else if (json_get(answer, "error", value, sizeof value) && value[0]) {
        WCHAR w[260];
        MultiByteToWideChar(CP_UTF8, 0, value, -1, w, 260);
        swprintf_s(r->why, 300, L"The server refused it (%lu): %s", status, w);
    } else {
        swprintf_s(r->why, 300, L"The server refused it (HTTP %lu).", status);
    }
    free(answer);
    return ok;
}
