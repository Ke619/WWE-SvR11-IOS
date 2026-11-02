/* WWE SmackDown vs. Raw 2011 - PC port launcher: updates.
 *
 * The newest release comes from the GitHub API
 * (api.github.com/repos/<UPDATE_REPO>/releases, newest first, pre-releases
 * included). Its SvR2011-PC-v*.zip is downloaded only from this repository's
 * own release downloads; the launcher unpacks it and copies the program files
 * over the install (svr2011_launcher.c, update_apply).
 *
 * GitHub's API answers 60 requests an hour per internet address without an account - a shared address (a
 * carrier's, a dorm's) can have it used up (2.1.8 report 7edf25: "an infinite download limit from GitHub"). The
 * check then goes without the API: github.com/<repo>/releases/latest names the newest release (a redirect to its
 * tag), and its zip is at releases/download/v<version>/SvR2011-PC-v<version>.zip (tools/package.ps1's name),
 * neither counted. The download (522 MB for 2.1.8) resumes where it stopped (Range) on an interruption or a
 * stall, a few times, instead of starting over. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "updater.h"

#pragma comment(lib, "winhttp.lib")

#define AGENT L"SvR2011-PC-Launcher"

typedef struct { HINTERNET session, connect, request; } Http;

static void http_close(Http *h)
{
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    memset(h, 0, sizeof *h);
}

/* The status of the last http_get (0: no answer). */
static DWORD s_last_status;

/* GitHub's "your limit is used up" (403 / 429 with X-RateLimit-Remaining: 0): when it resets, local time. */
static int rate_limited(HINTERNET request, WCHAR *when, size_t n)
{
    WCHAR v[32] = L"X-RateLimit-Remaining";
    DWORD vn = sizeof v;
    when[0] = 0;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, v, v, &vn, WINHTTP_NO_HEADER_INDEX) || _wtoi(v) != 0)
        return 0;
    {
        WCHAR r[32] = L"X-RateLimit-Reset";
        DWORD rn = sizeof r;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, r, r, &rn, WINHTTP_NO_HEADER_INDEX)) {
            ULARGE_INTEGER u;
            FILETIME ft, lt;
            SYSTEMTIME st;
            u.QuadPart = ((unsigned long long)_wtoi64(r) + 11644473600ull) * 10000000ull;
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
                swprintf_s(when, n, L" (it resets at %02u:%02u)", st.wHour, st.wMinute);
        }
    }
    return 1;
}

/* GET https://<host><path>; follows redirects. 1 when the answer is 200. */
static int http_get(Http *h, const WCHAR *url, const WCHAR *accept, WCHAR *err, size_t errn)
{
    URL_COMPONENTS uc;
    WCHAR host[256], path[2048];
    DWORD status = 0, len = sizeof status;
    s_last_status = 0;
    memset(h, 0, sizeof *h);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        swprintf_s(err, errn, L"Bad address: %s", url);
        return 0;
    }
    h->session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session)
        h->session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session
            || !(h->connect = WinHttpConnect(h->session, host, uc.nPort, 0))
            || !(h->request = WinHttpOpenRequest(h->connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))) {
        swprintf_s(err, errn, L"Could not reach %s (error %lu).", host, GetLastError());
        http_close(h);
        return 0;
    }
    WinHttpSetTimeouts(h->request, 10000, 10000, 15000, 30000);
    if (accept)
        WinHttpAddRequestHeaders(h->request, accept, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(h->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(h->request, NULL)
            || !WinHttpQueryHeaders(h->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        swprintf_s(err, errn, L"Could not reach %s (error %lu). Check your internet connection.", host, GetLastError());
        http_close(h);
        return 0;
    }
    s_last_status = status;
    if (status != 200 && status != 206) {   /* (206: a Range request's part) */
        WCHAR when[48];
        if ((status == 403 || status == 429) && rate_limited(h->request, when, 48))
            swprintf_s(err, errn, L"GitHub's limit for update checks from your internet address is used up (60 an "
                                  L"hour without a GitHub account)%s.", when);
        else if (status == 403 || status == 429)
            swprintf_s(err, errn, L"%s is limiting requests right now (HTTP %lu); try again later.", host, status);
        else
            swprintf_s(err, errn, L"%s answered HTTP %lu.", host, status);
        http_close(h);
        return 0;
    }
    return 1;
}

/* The whole answer, NUL-terminated (up to `max` bytes). */
static char *http_read_all(Http *h, size_t max, size_t *out_n)
{
    size_t n = 0, cap = 65536;
    char *buf = (char *)malloc(cap + 1);
    DWORD got;
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            char *p;
            if (cap >= max) break;
            cap *= 2;
            p = (char *)realloc(buf, cap + 1);
            if (!p) { free(buf); return NULL; }
            buf = p;
        }
        got = 0;
        if (!WinHttpReadData(h->request, buf + n, (DWORD)(cap - n), &got) || !got) break;
        n += got;
    }
    buf[n] = 0;
    if (out_n) *out_n = n;
    return buf;
}

/* The string value of "key" at or after p, before `end`; copied to out. */
static const char *json_string(const char *p, const char *end, const char *key, char *out, size_t outn)
{
    char pat[64];
    const char *k, *s;
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    k = strstr(p, pat);
    if (!k || k >= end) return NULL;
    s = k + strlen(pat);
    while (s < end && (*s == ' ' || *s == ':')) s++;
    if (s >= end || *s != '"') return NULL;
    s++;
    while (s < end && *s != '"' && n + 1 < outn) {
        if (*s == '\\' && s + 1 < end) s++;
        out[n++] = *s++;
    }
    out[n] = 0;
    return s;
}

static int json_bool(const char *p, const char *end, const char *key)
{
    char pat[64];
    const char *k;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    k = strstr(p, pat);
    if (!k || k >= end) return 0;
    k += strlen(pat);
    while (k < end && (*k == ' ' || *k == ':')) k++;
    return !strncmp(k, "true", 4);
}

int version_newer(const WCHAR *a, const WCHAR *b)
{
    while (*a || *b) {
        long x = wcstol(a, (WCHAR **)&a, 10), y = wcstol(b, (WCHAR **)&b, 10);
        if (x != y) return x > y;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
        if (*a && !iswdigit(*a)) break;
        if (*b && !iswdigit(*b)) break;
    }
    return 0;
}

/* The first release in GitHub's answer `url` (a list, newest first, or one release). */
static int update_read(const WCHAR *api, UpdateInfo *out, WCHAR *err, size_t errn)
{
    Http h;
    char *json, *rel, *end, tag[64], url[1024], page[512], name[256];
    const char *a;
    size_t n = 0;
    int ok = 0;
    char prefix[256];
    memset(out, 0, sizeof *out);
    if (!http_get(&h, api, L"Accept: application/vnd.github+json", err, errn))
        return 0;
    json = http_read_all(&h, 4u << 20, &n);
    http_close(&h);
    if (!json) { swprintf_s(err, errn, L"Out of memory."); return 0; }
    /* Releases come newest first; drafts are not listed. Each release object
       starts at its "url" of .../releases/<id> - split on "tag_name". */
    rel = strstr(json, "\"tag_name\"");
    if (!rel) { swprintf_s(err, errn, L"No releases found."); goto done; }
    end = strstr(rel + 10, "\"tag_name\"");
    if (!end) end = json + n;
    if (!json_string(rel, end, "tag_name", tag, sizeof tag)) goto bad;
    json_string(json, end, "html_url", page, sizeof page);
    out->prerelease = json_bool(json, end, "prerelease");
    /* its zip: the first asset named SvR2011-PC-*.zip */
    snprintf(prefix, sizeof prefix, "https://github.com/%ls/releases/download/", UPDATE_REPO);
    for (a = rel; (a = json_string(a, end, "name", name, sizeof name)) != NULL;) {
        if (!strncmp(name, "SvR2011-PC-", 11) && strlen(name) > 4 && !_stricmp(name + strlen(name) - 4, ".zip")) {
            const char *u = json_string(a, end, "browser_download_url", url, sizeof url);
            const char *sz = strstr(a, "\"size\"");
            if (!u || strncmp(url, prefix, strlen(prefix))) break;   /* only this repository's downloads */
            if (sz && sz < end) out->size = _strtoui64(sz + 7, NULL, 10);
            MultiByteToWideChar(CP_UTF8, 0, url, -1, out->url, 1024);
            break;
        }
    }
    if (!out->url[0]) { swprintf_s(err, errn, L"The newest release (%hs) has no download for Windows.", tag); goto done; }
    MultiByteToWideChar(CP_UTF8, 0, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, -1, out->version, 32);
    MultiByteToWideChar(CP_UTF8, 0, page, -1, out->page, 512);
    ok = 1;
    goto done;
bad:
    swprintf_s(err, errn, L"Could not read GitHub's answer.");
done:
    free(json);
    return ok;
}

/* A release without the API: its zip's address made from the version (tools/package.ps1's name), checked to be
   there (its first byte, which also gives its size). */
static int update_direct(const WCHAR *version, UpdateInfo *out, WCHAR *err, size_t errn)
{
    char one;
    uint64_t total = 0;
    memset(out, 0, sizeof *out);
    swprintf_s(out->url, 1024, L"https://github.com/" UPDATE_REPO L"/releases/download/v%s/SvR2011-PC-v%s.zip",
               version, version);
    swprintf_s(out->page, 512, L"https://github.com/" UPDATE_REPO L"/releases/tag/v%s", version);
    wcsncpy_s(out->version, 32, version, _TRUNCATE);
    if (!update_read_range(out->url, 0, 1, &one, &total, err, errn)) {
        out->url[0] = 0;
        return 0;
    }
    out->size = total;
    return 1;
}

/* The newest release without the API: github.com/<repo>/releases/latest redirects to .../releases/tag/v<version>
   (not a pre-release - the API's list has those too). */
static int update_latest_web(UpdateInfo *out, WCHAR *err, size_t errn)
{
    HINTERNET ses, con = NULL, req = NULL;
    WCHAR loc[1024], *tag;
    DWORD ln = sizeof loc, policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, status = 0, sn = sizeof status;
    int ok = 0;
    loc[0] = 0;
    ses = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        ses = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses && (con = WinHttpConnect(ses, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) != NULL &&
        (req = WinHttpOpenRequest(con, L"GET", L"/" UPDATE_REPO L"/releases/latest", NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) != NULL) {
        WinHttpSetTimeouts(req, 10000, 10000, 15000, 30000);
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, NULL) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &sn, WINHTTP_NO_HEADER_INDEX) &&
            status >= 300 && status < 400 &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &ln,
                                WINHTTP_NO_HEADER_INDEX))
            ok = 1;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    tag = ok ? wcsstr(loc, L"/releases/tag/") : NULL;
    if (!tag) {
        swprintf_s(err, errn, L"Could not find the newest release on github.com (HTTP %lu).", status);
        return 0;
    }
    tag += 14;
    if (*tag == L'v' || *tag == L'V')
        tag++;
    return update_direct(tag, out, err, errn);
}

/* (test aid: SVR2011_TEST_UPDATE_API_FAIL=1 - the API as if its limit were used up) */
static int api_fails(WCHAR *err, size_t errn)
{
    WCHAR v[8];
    if (!GetEnvironmentVariableW(L"SVR2011_TEST_UPDATE_API_FAIL", v, 8) || v[0] != L'1')
        return 0;
    swprintf_s(err, errn, L"GitHub's limit for update checks from your internet address is used up (test).");
    return 1;
}

int update_check(UpdateInfo *out, WCHAR *err, size_t errn)
{
    WCHAR api_err[400] = L"";
    if (!api_fails(api_err, 400) &&
        update_read(L"https://api.github.com/repos/" UPDATE_REPO L"/releases?per_page=10", out, api_err, 400))
        return 1;
    /* the API refused (its limit, or it's down): the newest release by github.com's own page */
    if (update_latest_web(out, err, errn))
        return 1;
    swprintf_s(err + wcslen(err), errn - wcslen(err), L" (the API: %s) You can also download the newest "
               L"release yourself: github.com/" UPDATE_REPO L"/releases", api_err);
    return 0;
}

int update_check_version(const WCHAR *version, UpdateInfo *out, WCHAR *err, size_t errn)
{
    WCHAR api[256], api_err[400] = L"";
    swprintf_s(api, 256, L"https://api.github.com/repos/" UPDATE_REPO L"/releases/tags/v%s", version);
    if (!api_fails(api_err, 400) && update_read(api, out, api_err, 400))
        return 1;
    if (update_direct(version, out, err, errn))   /* (without the API: its zip's own address) */
        return 1;
    swprintf_s(err + wcslen(err), errn - wcslen(err), L" (the API: %s)", api_err);
    return 0;
}

typedef struct { HWND notify; UINT msg; int last; } NotifyCtx;

static void notify_progress(uint64_t done, uint64_t total, void *ctx)
{
    NotifyCtx *n = (NotifyCtx *)ctx;
    int pct;
    if (!n->notify || !total) return;
    pct = (int)(done * 100 / total);
    if (pct != n->last) { n->last = pct; PostMessageW(n->notify, n->msg, (WPARAM)pct, 0); }
}

int update_download(const WCHAR *url, const WCHAR *path, HWND notify, UINT msg, volatile LONG *cancel,
                    WCHAR *err, size_t errn)
{
    NotifyCtx n = { notify, msg, -1 };
    return update_download_cb(url, path, notify_progress, &n, cancel, err, errn);
}

int update_download_cb(const WCHAR *url, const WCHAR *path, update_progress_fn progress, void *ctx,
                       volatile LONG *cancel, WCHAR *err, size_t errn)
{
    /* Up to 6 tries; after the first, from where it stopped (Range: bytes=<done>-) - a server that answers the
       whole file again (200) starts it over. (Test aid SVR2011_TEST_DOWNLOAD_CUT=<bytes>: the first try stops
       there, as an interruption.) */
    enum { kTries = 6 };
    Http h;
    HANDLE f;
    WCHAR len_text[32], hdr[160], cut_v[32];
    DWORD got, wrote;
    uint64_t total = 0, done = 0, cut = 0;
    char *buf;
    int ok = 0, attempt;
    memset(&h, 0, sizeof h);
    if (GetEnvironmentVariableW(L"SVR2011_TEST_DOWNLOAD_CUT", cut_v, 32))
        cut = _wcstoui64(cut_v, NULL, 10);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    buf = (char *)malloc(1 << 20);
    if (f == INVALID_HANDLE_VALUE || !buf) {
        swprintf_s(err, errn, L"Could not create %s.", path);
        goto out;
    }
    for (attempt = 0; attempt < kTries && !ok; attempt++) {
        int failed = 0;
        if (attempt) {   /* (a short wait, longer each time) */
            DWORD k;
            for (k = 0; k < (DWORD)attempt * 20 && !(cancel && *cancel); k++)
                Sleep(100);
        }
        if (cancel && *cancel) { swprintf_s(err, errn, L"Stopped."); goto out; }
        if (done)
            swprintf_s(hdr, 160, L"Accept: application/octet-stream\r\nRange: bytes=%llu-", (unsigned long long)done);
        else
            wcscpy_s(hdr, 160, L"Accept: application/octet-stream");
        if (!http_get(&h, url, hdr, err, errn)) {
            if (s_last_status == 404 || s_last_status == 403 || s_last_status == 429)
                goto out;   /* (no such file / refused: trying again won't help) */
            continue;
        }
        if (done && s_last_status != 206) {   /* (the whole file again: start over) */
            done = 0;
            SetFilePointer(f, 0, NULL, FILE_BEGIN);
            SetEndOfFile(f);
        }
        {
            DWORD len_size = sizeof len_text;
            if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len_text,
                                    &len_size, WINHTTP_NO_HEADER_INDEX))
                total = done + _wcstoui64(len_text, NULL, 10);
        }
        for (;;) {
            if (cancel && *cancel) { swprintf_s(err, errn, L"Stopped."); http_close(&h); goto out; }
            got = 0;
            if (cut && done >= cut) {   /* (the test aid's interruption, once) */
                cut = 0;
                swprintf_s(err, errn, L"The download was interrupted (test).");
                failed = 1;
                break;
            }
            if (!WinHttpReadData(h.request, buf, 1 << 20, &got)) {
                DWORD e = GetLastError();
                if (e == ERROR_WINHTTP_TIMEOUT)   /* (no data for the receive timeout, 30 s) */
                    swprintf_s(err, errn, L"The download stalled (nothing came for 30 seconds). Check your internet "
                                          L"connection and try again.");
                else
                    swprintf_s(err, errn, L"The download was interrupted (error %lu).", e);
                failed = 1;
                break;
            }
            if (!got) break;
            if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
                swprintf_s(err, errn, L"Could not write the download (disk full?).");
                http_close(&h);
                goto out;
            }
            done += got;
            if (progress) progress(done, total, ctx);
        }
        http_close(&h);
        if (!failed && total && done != total) {
            swprintf_s(err, errn, L"The download was incomplete (%llu of %llu bytes).", (unsigned long long)done,
                       (unsigned long long)total);
            failed = 1;
        }
        if (!failed)
            ok = 1;
    }
    if (!ok && !(cancel && *cancel))
        swprintf_s(err + wcslen(err), errn - wcslen(err), L" It stopped %d times; try again later, or download the "
                   L"release yourself: github.com/" UPDATE_REPO L"/releases", kTries);
out:
    free(buf);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) DeleteFileW(path);
    http_close(&h);
    return ok;
}

int update_read_range(const WCHAR *url, int64_t offset, uint32_t len, void *out, uint64_t *total,
                      WCHAR *err, size_t errn)
{
    Http h;
    WCHAR hdr[160], cr[96];
    DWORD crn = sizeof cr, got;
    uint32_t done = 0;
    int ok = 0;
    if (offset < 0)   /* (the last `len` bytes) */
        swprintf_s(hdr, 160, L"Accept: application/octet-stream\r\nRange: bytes=-%u", len);
    else
        swprintf_s(hdr, 160, L"Accept: application/octet-stream\r\nRange: bytes=%lld-%lld", offset,
                   offset + (int64_t)len - 1);
    if (!http_get(&h, url, hdr, err, errn))
        return 0;
    if (total) {   /* (Content-Range: bytes a-b/total) */
        WCHAR *slash;
        *total = 0;
        if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &crn,
                                WINHTTP_NO_HEADER_INDEX) && (slash = wcschr(cr, L'/')))
            *total = _wcstoui64(slash + 1, NULL, 10);
    }
    while (done < len) {
        got = 0;
        if (!WinHttpReadData(h.request, (char *)out + done, len - done, &got)) {
            swprintf_s(err, errn, L"The download was interrupted (error %lu).", GetLastError());
            goto out;
        }
        if (!got)
            break;
        done += got;
    }
    if (done != len) {
        swprintf_s(err, errn, L"The server sent %u of %u bytes.", done, len);
        goto out;
    }
    ok = 1;
out:
    http_close(&h);
    return ok;
}

/* ── file helpers (the launcher and the updater) ───────────────────────── */

int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* a\b into out (MAX_PATH); 0 when it would not fit. */
int join(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    size_t la = wcslen(a);
    if (la + 1 + wcslen(b) >= MAX_PATH) {
        out[0] = 0;
        return 0;
    }
    swprintf_s(out, MAX_PATH, (la && a[la - 1] == L'\\') ? L"%s%s" : L"%s\\%s", a, b);
    return 1;
}

int same_dir(const WCHAR *a, const WCHAR *b)
{
    WCHAR fa[MAX_PATH], fb[MAX_PATH];
    size_t n;
    if (!GetFullPathNameW(a, MAX_PATH, fa, NULL) || !GetFullPathNameW(b, MAX_PATH, fb, NULL))
        return 0;
    n = wcslen(fa);
    if (n > 3 && fa[n - 1] == L'\\') fa[n - 1] = 0;
    n = wcslen(fb);
    if (n > 3 && fb[n - 1] == L'\\') fb[n - 1] = 0;
    return !_wcsicmp(fa, fb);
}

/* Copies the folder `from` into `to` with everything in it (files replaced). */
int copy_tree(const WCHAR *from, const WCHAR *to)
{
    WCHAR pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    CreateDirectoryW(to, NULL);
    if (!join(pat, from, L"*") || (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !join(src, from, fd.cFileName) ||
            !join(dst, to, fd.cFileName))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = copy_tree(src, dst) && ok;
        else if (!CopyFileW(src, dst, FALSE))
            ok = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The release's folders (Bundled Mods, native_shaders, pad_icons, Android ...) into `to`. */
int copy_folders(const WCHAR *from, const WCHAR *to)
{
    WCHAR pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    if (!join(pat, from, L"*") || (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") &&
            wcscmp(fd.cFileName, L"..") && join(src, from, fd.cFileName) && join(dst, to, fd.cFileName))
            ok = copy_tree(src, dst) && ok;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
