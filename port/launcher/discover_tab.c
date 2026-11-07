/* WWE SmackDown vs. Raw 2011 PC launcher - Mods -> Discovery (see discover_tab.h).
 *
 * Three views on one page: the grid (search by words, type and order; 24 a page, each with its first picture),
 * a mod's page (its four pictures, what it is, Download and install; Delete for its uploader) and the upload
 * form (name, version, types, tags, description, the .svrmod, four pictures - made 1024 x 768 JPEG here:
 * the server has no image library and only checks them). Every request runs on a thread of its own and
 * answers with WM_APP_DISCOVER; a page's late answers (an older search) are dropped by their number. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include "discover_tab.h"
#include "test_run.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

enum {
    D_SEARCH, D_TYPE, D_SORT, D_GO, D_GRID, D_PREV, D_PAGE, D_NEXT, D_UPLOAD, D_STATUS,
    D_PIC, D_PICPREV, D_PICLBL, D_PICNEXT, D_INFO, D_DESC, D_BACK, D_DOWNLOAD, D_DELETE,
    U_TITLE, U_NAME_L, U_NAME, U_VER_L, U_VER, U_TYPES_L, U_T0, U_T1, U_T2, U_T3, U_TAGS_L, U_TAGS, U_DESC_L, U_DESC,
    U_FILE_L, U_FILE, U_FILE_PICK, U_PICS_L, U_PICS, U_PICS_PICK, U_AUTHOR, U_SEND, U_CANCEL, U_PROGRESS,
    D_EDIT, D_NEWFILE, D_FIRSTPIC,   /* (the uploader's own: Edit..., New version..., Make first) */
    D_DLBAR, D_DLCANCEL, D_COUNT     /* (a download running: its bar and Cancel) */
};
enum { V_GRID, V_MOD, V_UPLOAD };
enum { R_LIST, R_THUMB, R_DETAIL, R_PIC, R_DOWNLOAD, R_UPLOAD, R_DELETE, R_PROGRESS, R_EDIT,
       R_RECONNECT, R_DLPROG };   /* (the server didn't answer: trying again; a download's progress) */
#define DL_TRIES 6   /* (a download that breaks: resumed / restarted this many times in all) */
#define PER_PAGE 24
#define THUMB_W 160
#define THUMB_H 120
#define PIC_W 400
#define PIC_H 300

typedef struct {
    int id, downloads;
    long long owner;   /* (the uploader's account id) */
    long long size, created, updated, edited;   /* (Unix seconds; updated / edited 0: never) */
    int pics;          /* (goes up with each picture change: img/n?v=pics) */
    WCHAR name[64], author[48], types[80], tags[220], version[24];
} Mod;

static HWND s_wnd, s_c[D_COUNT];
static int s_base, s_view = V_GRID, s_page, s_total, s_n, s_shown, s_busy, s_pic, s_cur = -1;
static volatile LONG s_gen;   /* (read by the threads: a newer page stops an older one's retries) */
/* The mod shown is the signed-in account's own (Edit / New version / Delete): only on the server's word ("mine" in
   its page, asked with the token) for the token signed in now - owner_now() checks it at every view. */
static int s_mine;
static char s_mine_token[160];   /* (the token the page was asked with) */
static volatile LONG s_dl_cancel;   /* (Cancel on a download) */
static int s_dl;                    /* (a download runs) */
static Mod s_dl_mod;                /* (the mod it is: the player may browse on meanwhile) */
static int s_edit;   /* (the form edits this mod - its id - instead of uploading a new one) */
static WCHAR s_cur_desc[2001], s_ini[MAX_PATH];
static long long s_last_visit;   /* (the previous visit to Discovery: "Updated" since then) */
static Mod s_mods[PER_PAGE];
static HIMAGELIST s_thumbs;
static HBITMAP s_pics[4];
static WCHAR s_game[MAX_PATH], s_file_name[MAX_PATH], s_up_file[MAX_PATH], s_up_pics[4][MAX_PATH];
static discover_server_fn s_server;
static discover_account_fn s_account;

static const WCHAR *const k_types[4] = { L"entrance", L"superstar", L"arena", L"moveset" };
static const WCHAR *const k_type_names[4] = { L"Entrance", L"Superstar", L"Arena", L"Move-set" };

/* ── small helpers ─────────────────────────────────────────────────────── */

static void status(const WCHAR *t) { SetWindowTextW(s_c[D_STATUS], t); }

static void fmt_bytes(WCHAR *out, size_t n, long long b)
{
    if (b >= (1ll << 30)) swprintf_s(out, n, L"%.2f GB", b / 1073741824.0);
    else if (b >= (1 << 20)) swprintf_s(out, n, L"%.1f MB", b / 1048576.0);
    else swprintf_s(out, n, L"%lld KB", (b + 1023) / 1024);
}

/* Unix seconds as the player's short date. */
static void fmt_date(WCHAR *out, size_t n, long long t)
{
    ULARGE_INTEGER u;
    FILETIME ft, lt;
    SYSTEMTIME st;
    out[0] = 0;
    u.QuadPart = (unsigned long long)(t + 11644473600ll) * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, out, (int)n, NULL);
}

/* Text as a JSON string (UTF-8; quotes, backslashes and control characters escaped), appended. */
static void json_text(char *out, size_t n, const WCHAR *w)
{
    char u[6000];
    const unsigned char *c;
    size_t k = strlen(out);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    if (k + 2 < n) out[k++] = '"';
    for (c = (const unsigned char *)u; *c && k + 8 < n; c++) {
        if (*c == '"' || *c == '\\') { out[k++] = '\\'; out[k++] = (char)*c; }
        else if (*c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (*c >= 0x20) out[k++] = (char)*c;
    }
    out[k++] = '"';
    out[k] = 0;
}

/* The version of a mod downloaded from Discovery (its "updated", else "created", at the download), or 0. */
static long long downloaded_stamp(int id)
{
    WCHAR k[32], v[32];
    if (!s_ini[0]) return 0;
    swprintf_s(k, 32, L"Mod%d", id);
    GetPrivateProfileStringW(L"Discovery", k, L"", v, 32, s_ini);
    return _wtoi64(v);
}

/* A JSON value's start for "key" inside [p, end), or NULL. */
static const char *jkey(const char *p, const char *end, const char *key)
{
    char k[48];
    size_t kl;
    sprintf_s(k, sizeof k, "\"%s\"", key);
    kl = strlen(k);
    for (; p && p + kl < end; p++)
        if (!memcmp(p, k, kl)) {
            p += kl;
            while (p < end && (*p == ' ' || *p == ':')) p++;
            return p;
        }
    return NULL;
}

/* A JSON string at p (its quote) as UTF-16 (\" \\ \n \uXXXX); the char after it. */
static const char *jstr(const char *p, const char *end, WCHAR *out, size_t n)
{
    char u[4096];
    size_t k = 0;
    out[0] = 0;
    if (!p || p >= end || *p != '"') return p;
    for (p++; p < end && *p != '"' && k + 4 < sizeof u; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (*p == 'n') u[k++] = '\n';
            else if (*p == 'u' && p + 4 < end) {
                unsigned c = 0;
                sscanf_s(p + 1, "%4x", &c);
                p += 4;
                if (c < 0x80) u[k++] = (char)c;
                else if (c < 0x800) { u[k++] = (char)(0xC0 | c >> 6); u[k++] = (char)(0x80 | (c & 63)); }
                else { u[k++] = (char)(0xE0 | c >> 12); u[k++] = (char)(0x80 | (c >> 6 & 63)); u[k++] = (char)(0x80 | (c & 63)); }
            } else u[k++] = *p;
        } else u[k++] = *p;
    }
    u[k] = 0;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)n);
    return p < end ? p + 1 : p;
}

static long long jnum(const char *p) { return p ? _atoi64(p) : 0; }

/* A JSON array of strings at p, joined with ", ". */
static void jlist(const char *p, const char *end, const WCHAR *const *names, WCHAR *out, size_t n)
{
    WCHAR one[64];
    out[0] = 0;
    if (!p || *p != '[') return;
    for (p++; p < end && *p != ']';) {
        if (*p == '"') {
            int i;
            p = jstr(p, end, one, 64);
            if (names)
                for (i = 0; i < 4; i++)
                    if (!wcscmp(one, k_types[i])) wcscpy_s(one, 64, names[i]);
            if (out[0]) wcscat_s(out, n, L", ");
            wcscat_s(out, n, one);
        } else p++;
    }
}

/* The end of the {...} starting at p (strings skipped). */
static const char *jobject_end(const char *p, const char *end)
{
    int depth = 0, in_str = 0;
    for (; p < end; p++) {
        if (in_str) { if (*p == '\\') p++; else if (*p == '"') in_str = 0; continue; }
        if (*p == '"') in_str = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p + 1;
    }
    return end;
}

static void parse_mod(const char *p, const char *end, Mod *m)
{
    ZeroMemory(m, sizeof *m);
    m->id = (int)jnum(jkey(p, end, "id"));
    m->owner = jkey(p, end, "owner") ? jnum(jkey(p, end, "owner")) : -2;
    m->created = jnum(jkey(p, end, "created"));
    m->updated = jnum(jkey(p, end, "updated"));   /* (null: 0) */
    m->edited = jnum(jkey(p, end, "edited"));
    m->pics = (int)jnum(jkey(p, end, "pics"));
    jstr(jkey(p, end, "name"), end, m->name, 64);
    jstr(jkey(p, end, "author"), end, m->author, 48);
    jstr(jkey(p, end, "version"), end, m->version, 24);
    jlist(jkey(p, end, "types"), end, k_type_names, m->types, 80);
    jlist(jkey(p, end, "tags"), end, NULL, m->tags, 220);
    m->size = jnum(jkey(p, end, "size"));
    m->downloads = (int)jnum(jkey(p, end, "downloads"));
}

/* ── pictures (WIC) ────────────────────────────────────────────────────── */

static IWICImagingFactory *wic(void)
{
    IWICImagingFactory *f = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    return f;
}

/* A frame cut to 4:3 (its middle) and scaled to w x h, 32-bit BGRA. */
static IWICBitmapSource *fit_43(IWICImagingFactory *f, IWICBitmapFrameDecode *frame, int w, int h)
{
    UINT sw = 0, sh = 0;
    IWICBitmapClipper *clip = NULL;
    IWICBitmapScaler *scale = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    WICRect r;
    IWICBitmapFrameDecode_GetSize(frame, &sw, &sh);
    if (!sw || !sh) return NULL;
    r.X = 0; r.Y = 0; r.Width = (INT)sw; r.Height = (INT)sh;
    if ((double)sw / sh > 4.0 / 3.0) { r.Width = (INT)(sh * 4 / 3); r.X = ((INT)sw - r.Width) / 2; }
    else { r.Height = (INT)(sw * 3 / 4); r.Y = ((INT)sh - r.Height) / 2; }
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(f, &clip)) &&
        SUCCEEDED(IWICBitmapClipper_Initialize(clip, src, &r)))
        src = (IWICBitmapSource *)clip;
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(f, &scale)) ||
        FAILED(IWICBitmapScaler_Initialize(scale, src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)scale, &GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        if (conv) IWICFormatConverter_Release(conv);
        conv = NULL;
    }
    if (scale) IWICBitmapScaler_Release(scale);
    if (clip) IWICBitmapClipper_Release(clip);
    return (IWICBitmapSource *)conv;
}

/* JPEG / PNG bytes as a w x h bitmap (cut to 4:3), or NULL. */
static HBITMAP picture_bitmap(const uint8_t *data, size_t n, int w, int h)
{
    IWICImagingFactory *f = wic();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *src = NULL;
    HBITMAP bmp = NULL;
    if (!f) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                              &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && (src = fit_43(f, frame, w, h))) {
        BITMAPINFO bi;
        void *bits = NULL;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && FAILED(IWICBitmapSource_CopyPixels(src, NULL, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)bits))) {
            DeleteObject(bmp);
            bmp = NULL;
        }
        IWICBitmapSource_Release(src);
    }
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(f);
    return bmp;
}

/* A picture file as the 1024 x 768 JPEG the server takes (malloc'd), or NULL. */
static uint8_t *picture_jpeg(const WCHAR *path, size_t *n)
{
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *src = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *out = NULL;
    IPropertyBag2 *props = NULL;
    IStream *mem = NULL;
    uint8_t *bytes = NULL;
    *n = 0;
    if (!f) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && (src = fit_43(f, frame, 1024, 768)) &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &mem)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, mem, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &out, &props))) {
        PROPBAG2 opt;
        VARIANT v;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        IWICFormatConverter *rgb = NULL;
        ZeroMemory(&opt, sizeof opt);
        opt.pstrName = L"ImageQuality";
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.85f;
        IPropertyBag2_Write(props, 1, &opt, &v);
        if (SUCCEEDED(IWICBitmapFrameEncode_Initialize(out, props)) &&
            SUCCEEDED(IWICBitmapFrameEncode_SetSize(out, 1024, 768)) &&
            SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(out, &fmt)) &&
            SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &rgb)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(rgb, src, &fmt, WICBitmapDitherTypeNone, NULL, 0,
                                                     WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(IWICBitmapFrameEncode_WriteSource(out, (IWICBitmapSource *)rgb, NULL)) &&
            SUCCEEDED(IWICBitmapFrameEncode_Commit(out)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
            HGLOBAL g = NULL;
            if (SUCCEEDED(GetHGlobalFromStream(mem, &g))) {
                STATSTG st;
                if (SUCCEEDED(IStream_Stat(mem, &st, STATFLAG_NONAME)) && (bytes = (uint8_t *)malloc((size_t)st.cbSize.QuadPart))) {
                    memcpy(bytes, GlobalLock(g), (size_t)st.cbSize.QuadPart);
                    GlobalUnlock(g);
                    *n = (size_t)st.cbSize.QuadPart;
                }
            }
        }
        if (rgb) IWICFormatConverter_Release(rgb);
    }
    if (props) IPropertyBag2_Release(props);
    if (out) IWICBitmapFrameEncode_Release(out);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (mem) IStream_Release(mem);
    if (src) IWICBitmapSource_Release(src);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(f);
    return bytes;
}

/* ── requests (threads) ────────────────────────────────────────────────── */

typedef struct {
    int kind, gen, index, n_ids, ids[PER_PAGE], pics_v[PER_PAGE];
    WCHAR path[700], file[MAX_PATH], pics[4][MAX_PATH];
    char token[160], *fields;    /* (an upload: its text fields, "name\0value\0..." ) */
    char *json;                  /* (an edit: the changed details, a PATCH body; NULL: none) */
    size_t fields_n;
} Job;

typedef struct {
    int kind, gen, index, status;
    uint8_t *data;
    size_t n;
    WCHAR text[300];
} Result;

static void post(Job *j, int status, uint8_t *data, size_t n, int index, const WCHAR *text)
{
    Result *r = (Result *)calloc(1, sizeof *r);
    if (!r) { free(data); return; }
    r->kind = j->kind; r->gen = j->gen; r->index = index; r->status = status; r->data = data; r->n = n;
    if (text) wcscpy_s(r->text, 300, text);
    if (!PostMessageW(s_wnd, WM_APP_DISCOVER, 0, (LPARAM)r)) { free(data); free(r); }
}

/* One request to <server><path>: the body (sent in pieces: progress for uploads); the answer (up to `max`). */
static int http(const Job *j, const WCHAR *method, const WCHAR *path, const char *ctype, const uint8_t *body,
                size_t body_n, uint8_t **out, size_t *out_n, size_t max)
{
    WCHAR base[260], url[1000], host[256], upath[900], headers[400];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status, got = 0;
    size_t total = 0, cap = 0;
    *out = NULL;
    *out_n = 0;
    s_server(base, 260);
    while (wcslen(base) && base[wcslen(base) - 1] == L'/') base[wcslen(base) - 1] = 0;
    swprintf_s(url, 1000, L"%s%s", base, path);
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    /* (a test run's upload / edit / new version / delete: never a server off this PC - test_run.h) */
    if (wcscmp(method, L"GET") && test_run_refuses(host, "discovery write")) return 403;
    ses = WinHttpOpen(L"SvR2011 Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 15000, 600000, 600000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, method, upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        headers[0] = 0;
        if (j->token[0]) swprintf_s(headers, 400, L"Authorization: Bearer %S\r\n", j->token);
        if (ctype) { wcscat_s(headers, 400, L"Content-Type: "); { WCHAR c[160]; swprintf_s(c, 160, L"%S\r\n", ctype); wcscat_s(headers, 400, c); } }
        if (WinHttpSendRequest(req, headers[0] ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, (DWORD)body_n, 0)) {
            size_t sent = 0;
            int ok = 1, last = -1;
            while (ok && sent < body_n) {
                DWORD put = 0, piece = (DWORD)(body_n - sent > (256u << 10) ? (256u << 10) : body_n - sent);
                ok = WinHttpWriteData(req, body + sent, piece, &put) && put;
                sent += put;
                if (body_n > (1u << 20) && (int)(sent * 1000 / body_n) / 10 != last) {
                    last = (int)(sent * 1000 / body_n) / 10;
                    PostMessageW(s_wnd, WM_APP_DISCOVER, R_PROGRESS, (LPARAM)(sent * 1000 / body_n));
                }
            }
            if (ok && WinHttpReceiveResponse(req, NULL)) {
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                for (;;) {
                    if (total + 65536 > cap) {
                        uint8_t *m;
                        cap = cap ? cap * 2 : 262144;
                        if (cap > max + 65536) cap = max + 65536;
                        if (total + 65536 > cap || !(m = (uint8_t *)realloc(*out, cap + 1))) break;
                        *out = m;
                    }
                    if (!WinHttpReadData(req, *out + total, 65536, &got) || !got) break;
                    total += got;
                }
                if (*out) (*out)[total] = 0;
                *out_n = total;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return (int)status;
}

/* The same answer of another kind (a reconnect's or a download's progress). */
static void post_as(Job *j, int kind, int status, const WCHAR *text)
{
    const int was = j->kind;
    j->kind = kind;
    post(j, status, NULL, 0, 0, text);
    j->kind = was;
}

/* Waits ms, a little at a time; 0 when the page moved on meanwhile (gen) or a download was cancelled. */
static int wait_while(const Job *j, int ms, int download)
{
    for (; ms > 0; ms -= 100) {
        if (download ? s_dl_cancel != 0 : j->gen != s_gen) return 0;
        Sleep(100);
    }
    return 1;
}

static const int k_backoff[] = { 2, 5, 10, 30 };   /* (seconds between tries; then 30 s each) */
static int backoff(int tries) { return k_backoff[tries < 4 ? tries : 3]; }

/* A GET for a page (the list, a mod's page): while the server doesn't answer at all, tried again quietly - 2, 5,
   10, then every 30 s - with "Reconnecting..." shown, until it does or the player goes elsewhere (gen). */
static int http_retry(Job *j, const WCHAR *path, uint8_t **out, size_t *out_n, size_t max)
{
    int st, tries = 0;
    for (;;) {
        st = http(j, L"GET", path, NULL, NULL, 0, out, out_n, max);
        if (st > 0 || j->gen != s_gen) return st;
        free(*out);
        *out = NULL;
        {
            WCHAR t[200];
            swprintf_s(t, 200, L"Reconnecting\x2026 (the online server didn't answer - trying again in %d s)", backoff(tries));
            post_as(j, R_RECONNECT, 0, t);
        }
        if (!wait_while(j, backoff(tries++) * 1000, 0)) return st;
    }
}

/* A download's progress line: "45% - 90.0 of 200.0 MB - 12.3 MB/s". */
static void dl_progress(Job *j, long long have, long long total, double speed, const WCHAR *extra)
{
    WCHAR t[300], a[32], b[32], rate[32] = L"";
    fmt_bytes(a, 32, have);
    if (speed > 0) swprintf_s(rate, 32, L" - %.1f MB/s", speed / 1048576.0);
    if (total > 0) {
        fmt_bytes(b, 32, total);
        swprintf_s(t, 300, L"Downloading %d%% - %s of %s%s%s", (int)(have * 100 / total), a, b, rate, extra ? extra : L"");
    } else {
        swprintf_s(t, 300, L"Downloading - %s%s%s", a, rate, extra ? extra : L"");
    }
    post_as(j, R_DLPROG, total > 0 ? (int)(have * 1000 / total) : 0, t);
}

/* The mod's file into `part` as it comes, with progress. A connection that breaks is tried again (DL_TRIES in all,
   2 / 5 / 10 / 30 s apart): from where it broke when the server answers a Range with 206, else from the start.
   1 when every byte is in (the Content-Length's, when the server gave one); why: why not. */
static int download_to(Job *j, const WCHAR *part, WCHAR *why)
{
    WCHAR base[260], url[1000], host[256], upath[900], headers[600];
    URL_COMPONENTS uc;
    long long have = 0, total = -1;
    int tries = 0, done = 0;
    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { wcscpy_s(why, 200, L"the download folder (Tools\\.downloads) can't be written"); return 0; }
    s_server(base, 260);
    while (wcslen(base) && base[wcslen(base) - 1] == L'/') base[wcslen(base) - 1] = 0;
    swprintf_s(url, 1000, L"%s%s", base, j->path);
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { CloseHandle(f); wcscpy_s(why, 200, L"the server's address is wrong"); return 0; }
    while (!done && !s_dl_cancel) {
        HINTERNET ses = NULL, con = NULL, req = NULL;
        DWORD status = 0, sz = sizeof status;
        int broke = 1;   /* (no answer, or it stopped part way: worth another try) */
        int busy = 0;    /* (502 / 503 / 504: worth another try too) */
        ses = WinHttpOpen(L"SvR2011 Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) {
            WinHttpSetTimeouts(ses, 10000, 15000, 30000, 30000);   /* (30 s without a byte: broken) */
            con = WinHttpConnect(ses, host, uc.nPort, 0);
        }
        if (con)
            req = WinHttpOpenRequest(con, L"GET", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (req) {
            headers[0] = 0;
            if (j->token[0]) swprintf_s(headers, 600, L"Authorization: Bearer %S\r\n", j->token);
            if (have > 0) swprintf_s(headers + wcslen(headers), 600 - wcslen(headers), L"Range: bytes=%lld-\r\n", have);
            if (WinHttpSendRequest(req, headers[0] ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
                WCHAR len[32] = L"";
                DWORD ln = sizeof len;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &ln,
                                         WINHTTP_NO_HEADER_INDEX)) len[0] = 0;
                if (status == 206 && have > 0) {   /* (the rest, from where it broke) */
                    WCHAR range[96] = L"";
                    DWORD rn = sizeof range;
                    long long from = -1;
                    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range, &rn,
                                            WINHTTP_NO_HEADER_INDEX))
                        swscanf_s(range, L"bytes %lld-", &from);
                    if (from != have) { status = 0; have = 0; }   /* (not the part asked for: from the start) */
                    else if (len[0]) total = have + _wtoi64(len);
                }
                if (status == 200 || (status == 206 && have == 0)) {   /* (the whole file: from the start) */
                    LARGE_INTEGER zero = { 0 };
                    SetFilePointerEx(f, zero, NULL, FILE_BEGIN);
                    SetEndOfFile(f);
                    have = 0;
                    total = len[0] ? _wtoi64(len) : -1;
                }
                if (status == 200 || status == 206) {
                    static uint8_t buf[1 << 16];   /* (one download at a time: s_dl) */
                    ULONGLONG t0 = GetTickCount64(), last = 0;
                    long long at0 = have;
                    DWORD got = 0, put = 0;
                    for (;;) {
                        if (s_dl_cancel) break;
                        if (!WinHttpReadData(req, buf, sizeof buf, &got)) break;   /* (broken) */
                        if (!got) { broke = total >= 0 && have < total; break; }   /* (the end - all of it?) */
                        if (!WriteFile(f, buf, got, &put, NULL) || put != got) {
                            wcscpy_s(why, 200, L"the file couldn't be written (disk full?)");
                            broke = 0;
                            status = 0;
                            break;
                        }
                        have += got;
                        if (GetTickCount64() - last >= 200) {
                            const double secs = (GetTickCount64() - t0) / 1000.0;
                            last = GetTickCount64();
                            dl_progress(j, have, total, secs > 0.2 ? (have - at0) / secs : 0, NULL);
                        }
                    }
                    if (!s_dl_cancel && !broke && !why[0]) done = 1;
                } else if (status == 502 || status == 503 || status == 504) {
                    busy = 1;   /* (the server is there but its storage is slow just now: tried again) */
                } else if (status) {
                    broke = 0;   /* (the server said no - 404, 403...: not worth trying again) */
                    swprintf_s(why, 200, L"the server said %d", (int)status);
                }
            }
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
        if (done || s_dl_cancel || !broke || why[0]) break;
        if (++tries >= DL_TRIES) {
            wcscpy_s(why, 200, L"the connection kept breaking");
            break;
        }
        {
            WCHAR t[120];
            swprintf_s(t, 120, L" - %s, %s in %d s (try %d of %d)", busy ? L"the server is busy" : L"connection lost",
                       have > 0 ? L"resuming" : L"starting again", backoff(tries - 1), tries + 1, DL_TRIES);
            dl_progress(j, have, total, 0, t);
        }
        wait_while(j, backoff(tries - 1) * 1000, 1);
    }
    CloseHandle(f);
    if (s_dl_cancel && !done) wcscpy_s(why, 200, L"cancelled");
    return done && !why[0];
}

/* A multipart request: the text fields, the four pictures (with_pics: made here from j->pics), the .svrmod
   (j->file). The HTTP status; why: what went wrong here. */
static int send_multipart(Job *j, int with_pics, const WCHAR *method, const WCHAR *url, uint8_t **data, size_t *n,
                          WCHAR *why)
{
    static const char bound[] = "----svr2011discovery7f3a9c";
    uint8_t *pics[4] = { 0 }, *file = NULL, *body = NULL;
    size_t pic_n[4] = { 0 }, file_n = 0, at = 0, cap;
    const char *f;
    HANDLE h;
    int i, st = -1;
    for (i = 0; i < 4 && with_pics && !why[0]; i++)
        if (!(pics[i] = picture_jpeg(j->pics[i], &pic_n[i])) || pic_n[i] > (1u << 20))
            swprintf_s(why, 200, L"Picture %d could not be made into a 1024 x 768 JPEG.", i + 1);
    h = why[0] ? INVALID_HANDLE_VALUE : CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (!why[0]) {
        LARGE_INTEGER sz;
        DWORD got = 0;
        if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz) || sz.QuadPart > (250ll << 20) ||
            !(file = (uint8_t *)malloc((size_t)sz.QuadPart + 1)) ||
            !ReadFile(h, file, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)
            wcscpy_s(why, 200, L"The .svrmod could not be read (250 MB at most).");
        else file_n = (size_t)sz.QuadPart;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    cap = file_n + j->fields_n * 2 + 4 * (1u << 20) + 8192;
    if (!why[0] && (body = (uint8_t *)malloc(cap))) {
        char head[512], fname[MAX_PATH * 3];
        const WCHAR *slash = wcsrchr(j->file, L'\\');
#define PUT(src, len) (memcpy(body + at, (src), (len)), at += (len))
        for (f = j->fields; f && f < j->fields + j->fields_n; f += strlen(f) + 1) {
            const char *v = f + strlen(f) + 1;
            int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n", bound, f);
            PUT(head, (size_t)k);
            PUT(v, strlen(v));
            PUT("\r\n", 2);
            f = v;
        }
        for (i = 0; i < 4 && with_pics; i++) {
            int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"img%d\"; "
                              "filename=\"%d.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", bound, i, i);
            PUT(head, (size_t)k);
            PUT(pics[i], pic_n[i]);
            PUT("\r\n", 2);
        }
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : j->file, -1, fname, sizeof fname, NULL, NULL);
        {
            char *q;
            for (q = fname; *q; q++) if (*q == '"') *q = '_';
        }
        {
            int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"svrmod\"; "
                              "filename=\"%.300s\"\r\nContent-Type: application/octet-stream\r\n\r\n", bound, fname);
            PUT(head, (size_t)k);
        }
        PUT(file, file_n);
        {
            int k = sprintf_s(head, sizeof head, "\r\n--%s--\r\n", bound);
            PUT(head, (size_t)k);
        }
#undef PUT
        free(file);
        file = NULL;
        {
            char ctype[96];
            sprintf_s(ctype, sizeof ctype, "multipart/form-data; boundary=%s", bound);
            st = http(j, method, url, ctype, body, at, data, n, 1 << 16);
        }
    } else if (!why[0]) {
        wcscpy_s(why, 200, L"Not enough memory for the upload.");
    }
    free(body);
    free(file);
    for (i = 0; i < 4; i++) free(pics[i]);
    return st;
}

static DWORD WINAPI job_thread(LPVOID p)
{
    Job *j = (Job *)p;
    uint8_t *data = NULL;
    size_t n = 0;
    int st, i;
    WCHAR path[64];
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    switch (j->kind) {
    case R_LIST:
        st = http_retry(j, j->path, &data, &n, 1 << 20);
        if (st == 200 && data) {   /* (the page's ids and picture counters, for its pictures) */
            const char *q = jkey((const char *)data, (const char *)data + n, "mods"), *end = (const char *)data + n;
            while (q && q < end && j->n_ids < PER_PAGE && (q = strchr(q, '{')) && q < end) {
                const char *e = jobject_end(q, end);
                j->pics_v[j->n_ids] = (int)jnum(jkey(q, e, "pics"));
                j->ids[j->n_ids++] = (int)jnum(jkey(q, e, "id"));
                q = e;
            }
        }
        post(j, st, data, n, 0, NULL);
        /* then the pictures of the page, one after the other */
        for (i = 0; i < j->n_ids && st == 200; i++) {
            if (j->gen != s_gen) break;
            swprintf_s(path, 64, L"/api/mods/%d/img/0?v=%d", j->ids[i], j->pics_v[i]);
            data = NULL;
            if ((st = http_retry(j, path, &data, &n, 2 << 20)) == 200) {
                j->kind = R_THUMB;
                post(j, 200, data, n, i, NULL);
                j->kind = R_LIST;
            } else free(data);
            st = 200;   /* (a picture missing: the next one still) */
        }
        break;
    case R_DETAIL:
        st = http_retry(j, j->path, &data, &n, 1 << 20);
        post(j, st, data, n, 0, NULL);
        j->kind = R_PIC;
        for (i = 0; i < 4 && st == 200 && j->gen == s_gen; i++) {
            swprintf_s(path, 64, L"%s/img/%d?v=%d", j->path, i, j->index);
            data = NULL;
            if (http_retry(j, path, &data, &n, 2 << 20) == 200) post(j, 200, data, n, i, NULL);
            else free(data);
        }
        break;
    case R_DOWNLOAD: {
        /* into <id>.part, renamed to discovery.svrmod only when it is all in: a broken or cancelled download never
           reaches the install (the .part is deleted) */
        WCHAR dir[MAX_PATH], part[MAX_PATH], why[200] = L"";
        swprintf_s(dir, MAX_PATH, L"%s\\Tools", s_game);
        CreateDirectoryW(dir, NULL);
        wcscat_s(dir, MAX_PATH, L"\\.downloads");
        CreateDirectoryW(dir, NULL);
        swprintf_s(part, MAX_PATH, L"%s\\discovery_%d.part", dir, j->index);
        swprintf_s(j->file, MAX_PATH, L"%s\\discovery.svrmod", dir);
        if (download_to(j, part, why) && MoveFileExW(part, j->file, MOVEFILE_REPLACE_EXISTING)) {
            st = 200;
        } else {
            DeleteFileW(part);
            if (!why[0]) wcscpy_s(why, 200, L"the file couldn't be put in place");
            st = s_dl_cancel ? -2 : -1;
        }
        post(j, st, NULL, 0, 0, st == 200 ? j->file : why);
        break;
    }
    case R_DELETE:
        st = http(j, L"DELETE", j->path, NULL, NULL, 0, &data, &n, 1 << 16);
        post(j, st, data, n, 0, NULL);
        break;
    case R_UPLOAD: {
        WCHAR why[200] = L"";
        st = send_multipart(j, 1, L"POST", L"/api/mods", &data, &n, why);
        post(j, st, data, n, 0, why[0] ? why : NULL);
        break;
    }
    case R_EDIT: {
        /* the details (PATCH), then the replaced pictures, then a new version of the file - stopping at a failure */
        WCHAR why[200] = L"", url[96];
        st = 200;
        if (j->json) {
            swprintf_s(url, 96, L"/api/mods/%d", j->index);
            st = http(j, L"PATCH", url, "application/json", (const uint8_t *)j->json, strlen(j->json), &data, &n, 1 << 16);
        }
        for (i = 0; i < 4 && st == 200; i++) {
            uint8_t *jpg;
            size_t jn = 0;
            if (!j->pics[i][0]) continue;
            if (!(jpg = picture_jpeg(j->pics[i], &jn)) || jn > (1u << 20)) {
                swprintf_s(why, 200, L"Picture %d could not be made into a 1024 x 768 JPEG.", i + 1);
                st = -1;
                free(jpg);
                break;
            }
            free(data);
            data = NULL;
            swprintf_s(url, 96, L"/api/mods/%d/pictures/%d", j->index, i);
            st = http(j, L"PUT", url, "image/jpeg", jpg, jn, &data, &n, 1 << 16);
            free(jpg);
        }
        if (st == 200 && j->file[0]) {
            free(data);
            data = NULL;
            swprintf_s(url, 96, L"/api/mods/%d/file", j->index);
            st = send_multipart(j, 0, L"PUT", url, &data, &n, why);
        }
        post(j, st, data, n, 0, why[0] ? why : NULL);
        break;
    }
    }
    free(j->fields);
    free(j->json);
    free(j);
    CoUninitialize();
    return 0;
}

static void start(Job *j)
{
    HANDLE t;
    WCHAR name[8];
    j->gen = s_gen;
    if (!j->token[0]) {
        long long id;
        s_account(j->token, sizeof j->token, name, 1, &id);
    }
    t = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (t) CloseHandle(t);
    else { free(j->fields); free(j); }
}

static Job *job(int kind)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    if (j) j->kind = kind;
    return j;
}

/* ── the views ─────────────────────────────────────────────────────────── */

static const int k_grid_ids[] = { D_SEARCH, D_TYPE, D_SORT, D_GO, D_GRID, D_PREV, D_PAGE, D_NEXT, D_UPLOAD };
static const int k_mod_ids[] = { D_PIC, D_PICPREV, D_PICLBL, D_PICNEXT, D_INFO, D_DESC, D_BACK, D_DOWNLOAD, D_DELETE,
                                 D_EDIT, D_NEWFILE, D_FIRSTPIC, D_DLBAR, D_DLCANCEL };

/* The mod's page shows the player's own mod: the server said so ("mine") for the token signed in now - not for one
   from before a sign-out or another account's sign-in (then its Delete was refused: 403). */
static int owner_now(void)
{
    char token[160];
    WCHAR me[8];
    long long my_id = -1;
    if (s_view != V_MOD || s_cur < 0 || !s_mine) return 0;
    s_account(token, sizeof token, me, 8, &my_id);
    return token[0] && !strcmp(token, s_mine_token);
}

static void set_view(int v)
{
    int i, mine;
    s_view = v;
    mine = owner_now();
    for (i = 0; i < D_COUNT; i++) {
        int show = i == D_STATUS;
        size_t k;
        for (k = 0; k < sizeof k_grid_ids / sizeof k_grid_ids[0]; k++) if (k_grid_ids[k] == i) show = v == V_GRID;
        for (k = 0; k < sizeof k_mod_ids / sizeof k_mod_ids[0]; k++) if (k_mod_ids[k] == i) show = v == V_MOD;
        if (i >= U_TITLE && i <= U_PROGRESS) show = v == V_UPLOAD;
        if (i == D_DELETE || i == D_EDIT || i == D_NEWFILE || i == D_FIRSTPIC)
            show = show && mine && !s_dl;   /* (only on the player's own mod - checked again at every view) */
        if (i == D_DLBAR || i == D_DLCANCEL) show = show && s_dl;   /* (a download: its bar, in the buttons' place) */
        if (i == D_DOWNLOAD) show = show && !s_dl;
        if (s_c[i]) ShowWindow(s_c[i], show ? SW_SHOW : SW_HIDE);
    }
}

static void search(void)
{
    static const char *const types[] = { "", "entrance", "superstar", "arena", "moveset" };
    static const char *const sorts[] = { "new", "popular", "name", "updated" };
    WCHAR q[120], qe[400];
    char u[400];
    const char *s;
    size_t k = 0;
    int ti = (int)SendMessageW(s_c[D_TYPE], CB_GETCURSEL, 0, 0), si = (int)SendMessageW(s_c[D_SORT], CB_GETCURSEL, 0, 0);
    Job *j = job(R_LIST);
    if (!j) return;
    GetWindowTextW(s_c[D_SEARCH], q, 120);
    WideCharToMultiByte(CP_UTF8, 0, q, -1, u, sizeof u, NULL, NULL);
    for (s = u; *s && k + 4 < 400; s++) {   /* (percent-encoded) */
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            qe[k++] = c;
        else { swprintf_s(qe + k, 400 - k, L"%%%02X", c); k += 3; }
    }
    qe[k] = 0;
    s_gen++;
    swprintf_s(j->path, 700, L"/api/mods?q=%s&type=%S&sort=%S&page=%d", qe, types[ti > 0 && ti < 5 ? ti : 0],
               sorts[si > 0 && si < 4 ? si : 0], s_page);
    status(L"Looking\x2026");
    start(j);
}

static void open_mod(int i)
{
    Job *j;
    int k;
    if (i < 0 || i >= s_n) return;
    s_cur = i;
    s_pic = 0;
    for (k = 0; k < 4; k++) if (s_pics[k]) { DeleteObject(s_pics[k]); s_pics[k] = NULL; }
    SetWindowTextW(s_c[D_INFO], L"");
    SetWindowTextW(s_c[D_DESC], L"");
    SetWindowTextW(s_c[D_PICLBL], L"1 / 4");
    if (!(j = job(R_DETAIL))) return;
    {   /* the player's own: only when the page's answer says so ("mine", asked with this token) */
        WCHAR me[8];
        long long my_id = -1;
        s_account(j->token, sizeof j->token, me, 8, &my_id);
        strcpy_s(s_mine_token, sizeof s_mine_token, j->token);
        s_mine = 0;
        set_view(V_MOD);
    }
    InvalidateRect(s_c[D_PIC], NULL, TRUE);
    InterlockedIncrement(&s_gen);
    swprintf_s(j->path, 700, L"/api/mods/%d", s_mods[i].id);
    j->index = s_mods[i].pics;
    start(j);
}

static void upload_view(void)
{
    char token[160];
    WCHAR me[64], t[160];
    long long my_id;
    s_account(token, sizeof token, me, 64, &my_id);
    set_view(V_UPLOAD);
    SetWindowTextW(s_c[U_TITLE], s_edit ? L"Edit your mod: change what you like - the pictures and the file only when you "
                                          L"choose new ones."
                                        : L"Upload a mod: every field but tags and the description is needed. Only .svrmod files.");
    SetWindowTextW(s_c[U_SEND], s_edit ? L"Save" : L"Upload");
    SetWindowTextW(s_c[U_FILE_PICK], s_edit ? L"New version\x2026" : L"Choose\x2026");
    SetWindowTextW(s_c[U_PICS_PICK], s_edit ? L"Replace\x2026" : L"Choose 4\x2026");
    if (token[0] && me[0]) swprintf_s(t, 160, L"Author: %s (your online name)", me);
    else wcscpy_s(t, 160, L"Sign in online first (the Online page): your online name is the author.");
    SetWindowTextW(s_c[U_AUTHOR], t);
    EnableWindow(s_c[U_SEND], token[0] && me[0] && !s_busy);
}

static int pick_files(const WCHAR *title, int pictures, WCHAR out[][MAX_PATH], int max)
{
    IFileOpenDialog *dlg = NULL;
    IShellItemArray *items = NULL;
    DWORD n = 0, i;
    int got = 0;
    COMDLG_FILTERSPEC mod[] = { { L"SvR2011 mods (*.svrmod)", L"*.svrmod" } };
    COMDLG_FILTERSPEC pic[] = { { L"Pictures (*.png, *.jpg, *.jpeg, *.bmp)", L"*.png;*.jpg;*.jpeg;*.bmp" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    IFileOpenDialog_SetFileTypes(dlg, 1, pictures ? pic : mod);
    IFileOpenDialog_SetOptions(dlg, FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (pictures ? FOS_ALLOWMULTISELECT : 0));
    IFileOpenDialog_SetTitle(dlg, title);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, s_wnd)) && SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items))) {
        IShellItemArray_GetCount(items, &n);
        for (i = 0; i < n && got < max; i++) {
            IShellItem *it = NULL;
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &it)) &&
                SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                wcsncpy_s(out[got++], MAX_PATH, p, _TRUNCATE);
                CoTaskMemFree(p);
            }
            if (it) IShellItem_Release(it);
        }
        IShellItemArray_Release(items);
    }
    IFileOpenDialog_Release(dlg);
    return got;
}

static void form_clear(void)
{
    int k;
    static const int clear[] = { U_NAME, U_VER, U_TAGS, U_DESC };
    for (k = 0; k < 4; k++) SetWindowTextW(s_c[clear[k]], L"");
    for (k = 0; k < 4; k++) { SendMessageW(s_c[U_T0 + k], BM_SETCHECK, BST_UNCHECKED, 0); s_up_pics[k][0] = 0; }
    s_up_file[0] = 0;
    SetWindowTextW(s_c[U_FILE], L"(none chosen)");
    SetWindowTextW(s_c[U_PICS], L"(four, any size: made 1024 x 768)");
}

/* Edit... on the player's own mod: the form with its details. */
static void edit_view(void)
{
    const Mod *m;
    WCHAR crlf[4100];
    size_t a, b2 = 0;
    int k;
    if (s_cur < 0) return;
    m = &s_mods[s_cur];
    form_clear();
    s_edit = m->id;
    SetWindowTextW(s_c[U_NAME], m->name);
    SetWindowTextW(s_c[U_VER], m->version);
    SetWindowTextW(s_c[U_TAGS], m->tags);
    for (a = 0; s_cur_desc[a] && b2 + 3 < 4100; a++) {
        if (s_cur_desc[a] == L'\n') crlf[b2++] = L'\r';
        crlf[b2++] = s_cur_desc[a];
    }
    crlf[b2] = 0;
    SetWindowTextW(s_c[U_DESC], crlf);
    for (k = 0; k < 4; k++)
        SendMessageW(s_c[U_T0 + k], BM_SETCHECK, wcsstr(m->types, k_type_names[k]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(s_c[U_FILE], L"(keep the current file - or a new version: then a new version number too)");
    SetWindowTextW(s_c[U_PICS], L"(keep the current pictures - or replace 1-4 of them, from the first)");
    upload_view();
}

/* Save on the edit form: what changed (PATCH), replaced pictures, a new file. */
static void send_edit(void)
{
    static const int ids[] = { U_NAME, U_VER, U_TAGS, U_DESC };
    static const char *const keys[] = { "name", "version", "tags", "description" };
    const Mod *m = s_cur >= 0 ? &s_mods[s_cur] : NULL;
    const WCHAR *was[4];
    WCHAR w[2001], types[80] = L"";
    char *json, typ[64] = "";
    int k, changed = 0, pics = 0;
    Job *j;
    if (!m || m->id != s_edit) return;
    was[0] = m->name; was[1] = m->version; was[2] = m->tags; was[3] = s_cur_desc;
    if (!(j = job(R_EDIT)) || !(json = (char *)malloc(16384))) { free(j); return; }
    strcpy_s(json, 16384, "{");
    for (k = 0; k < 4; k++) {
        GetWindowTextW(s_c[ids[k]], w, 2001);
        {   /* (the edit control's line ends back to \n) */
            WCHAR *r, *q;
            for (r = q = w; *r; r++) if (*r != L'\r') *q++ = *r;
            *q = 0;
        }
        if (k == 0 && !w[0]) { status(L"The name can't be empty."); free(json); free(j); return; }
        if (k == 1 && !w[0]) { status(L"The version can't be empty."); free(json); free(j); return; }
        if (k == 1 && s_up_file[0]) {   /* (a new file: its version goes with it) */
            if (!wcscmp(w, m->version)) { status(L"A new version of the file needs a new version number."); free(json); free(j); return; }
            j->fields = (char *)malloc(64);
            if (j->fields) {
                int n2 = sprintf_s(j->fields, 64, "version") + 1;
                n2 += WideCharToMultiByte(CP_UTF8, 0, w, -1, j->fields + n2, 64 - n2, NULL, NULL);
                j->fields_n = (size_t)n2;
            }
            continue;
        }
        if (!wcscmp(w, was[k])) continue;
        if (changed++) strcat_s(json, 16384, ",");
        sprintf_s(json + strlen(json), 16384 - strlen(json), "\"%s\":", keys[k]);
        json_text(json, 16384, w);
    }
    for (k = 0; k < 4; k++)
        if (SendMessageW(s_c[U_T0 + k], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (types[0]) wcscat_s(types, 80, L", ");
            wcscat_s(types, 80, k_type_names[k]);
            if (typ[0]) strcat_s(typ, sizeof typ, ",");
            WideCharToMultiByte(CP_UTF8, 0, k_types[k], -1, typ + strlen(typ), (int)(sizeof typ - strlen(typ)), NULL, NULL);
        }
    if (!typ[0]) { status(L"Choose the mod's type."); free(json); free(j->fields); free(j); return; }
    if (wcscmp(types, m->types)) {
        if (changed++) strcat_s(json, 16384, ",");
        sprintf_s(json + strlen(json), 16384 - strlen(json), "\"types\":\"%s\"", typ);
    }
    strcat_s(json, 16384, "}");
    if (changed) j->json = json; else free(json);
    for (k = 0; k < 4; k++) if (s_up_pics[k][0]) { wcscpy_s(j->pics[k], MAX_PATH, s_up_pics[k]); pics++; }
    if (s_up_file[0]) wcscpy_s(j->file, MAX_PATH, s_up_file);
    if (!changed && !pics && !s_up_file[0]) { status(L"Nothing changed."); free(j->fields); free(j->json); free(j); return; }
    j->index = m->id;
    s_busy = 1;
    EnableWindow(s_c[U_SEND], FALSE);
    SendMessageW(s_c[U_PROGRESS], PBM_SETPOS, 0, 0);
    ShowWindow(s_c[U_PROGRESS], s_up_file[0] ? SW_SHOW : SW_HIDE);
    status(s_up_file[0] ? L"Sending the new version\x2026" : L"Saving\x2026");
    start(j);
}

static void send_upload(void)
{
    if (s_edit) { send_edit(); return; }
    {
    static const char *const keys[] = { "name", "version", "tags", "description" };
    static const int ids[] = { U_NAME, U_VER, U_TAGS, U_DESC };
    Job *j;
    char types[64] = "", *fields;
    size_t at = 0;
    int i, k;
    WCHAR w[2001];
    for (i = 0; i < 4; i++)
        if (SendMessageW(s_c[U_T0 + i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (types[0]) strcat_s(types, sizeof types, ",");
            WideCharToMultiByte(CP_UTF8, 0, k_types[i], -1, types + strlen(types), (int)(sizeof types - strlen(types)), NULL, NULL);
        }
    GetWindowTextW(s_c[U_NAME], w, 61);
    if (!w[0] || !GetWindowTextLengthW(s_c[U_VER]) || !types[0]) { status(L"A name, a version and the mod's type are needed."); return; }
    if (!s_up_file[0]) { status(L"Choose the .svrmod file."); return; }
    for (i = 0; i < 4; i++) if (!s_up_pics[i][0]) { status(L"Choose four pictures of the mod."); return; }
    if (!(j = job(R_UPLOAD)) || !(fields = (char *)malloc(16384))) { free(j); return; }
    for (k = 0; k < 4; k++) {
        GetWindowTextW(s_c[ids[k]], w, 2001);
        at += (size_t)sprintf_s(fields + at, 16384 - at, "%s", keys[k]) + 1;
        at += (size_t)WideCharToMultiByte(CP_UTF8, 0, w, -1, fields + at, (int)(16384 - at - 100), NULL, NULL);
    }
    at += (size_t)sprintf_s(fields + at, 16384 - at, "types") + 1;
    at += (size_t)sprintf_s(fields + at, 16384 - at, "%s", types) + 1;
    j->fields = fields;
    j->fields_n = at;
    wcscpy_s(j->file, MAX_PATH, s_up_file);
    for (i = 0; i < 4; i++) wcscpy_s(j->pics[i], MAX_PATH, s_up_pics[i]);
    s_busy = 1;
    EnableWindow(s_c[U_SEND], FALSE);
    SendMessageW(s_c[U_PROGRESS], PBM_SETPOS, 0, 0);
    ShowWindow(s_c[U_PROGRESS], SW_SHOW);
    status(L"Making the pictures and sending the mod\x2026");
    start(j);
    }
}

/* ── the launcher's side ───────────────────────────────────────────────── */

void discover_build(HWND wnd, mods_add_fn add, int tab, int id_base, discover_server_fn server,
                    discover_account_fn account)
{
    static const WCHAR *const types[] = { L"All types", L"Entrance", L"Superstar", L"Arena", L"Move-set" };
    static const WCHAR *const sorts[] = { L"Newest", L"Most downloaded", L"By name", L"Recently updated" };
    int i, y;
#define C(i, cls, text, style, x, y, w, h) (s_c[i] = add(tab, cls, text, style, x, y, w, h, id_base + (i)))
    s_wnd = wnd;
    s_base = id_base;
    s_server = server;
    s_account = account;
    /* the grid */
    C(D_SEARCH, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 28, 54, 214, 26);
    SendMessageW(s_c[D_SEARCH], EM_SETCUEBANNER, TRUE, (LPARAM)L"Search by name, author or tag");
    C(D_TYPE, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 250, 54, 120, 200);
    for (i = 0; i < 5; i++) SendMessageW(s_c[D_TYPE], CB_ADDSTRING, 0, (LPARAM)types[i]);
    SendMessageW(s_c[D_TYPE], CB_SETCURSEL, 0, 0);
    C(D_SORT, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 378, 54, 120, 200);
    for (i = 0; i < 4; i++) SendMessageW(s_c[D_SORT], CB_ADDSTRING, 0, (LPARAM)sorts[i]);
    SendMessageW(s_c[D_SORT], CB_SETCURSEL, 0, 0);
    C(D_GO, L"Button", L"Search", BS_DEFPUSHBUTTON | WS_TABSTOP, 506, 53, 82, 28);
    C(D_GRID, WC_LISTVIEWW, L"", LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 28, 88, 560, 392);
    ListView_SetExtendedListViewStyle(s_c[D_GRID], LVS_EX_DOUBLEBUFFER);
    s_thumbs = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR32, PER_PAGE, 0);
    ListView_SetImageList(s_c[D_GRID], s_thumbs, LVSIL_NORMAL);
    ListView_SetIconSpacing(s_c[D_GRID], THUMB_W + 22, THUMB_H + 46);
    C(D_PREV, L"Button", L"\x2190 Previous", BS_PUSHBUTTON | WS_TABSTOP, 28, 488, 100, 28);
    C(D_PAGE, L"Static", L"", SS_CENTER | SS_NOPREFIX, 132, 494, 170, 20);
    C(D_NEXT, L"Button", L"Next \x2192", BS_PUSHBUTTON | WS_TABSTOP, 306, 488, 100, 28);
    C(D_UPLOAD, L"Button", L"Upload a mod\x2026", BS_PUSHBUTTON | WS_TABSTOP, 444, 486, 144, 32);
    C(D_STATUS, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 528, 560, 36);
    /* a mod's page */
    C(D_PIC, L"Static", L"", SS_OWNERDRAW, 28, 54, PIC_W, PIC_H);
    C(D_PICPREV, L"Button", L"\x2190", BS_PUSHBUTTON | WS_TABSTOP, 28, 360, 44, 28);
    C(D_PICLBL, L"Static", L"1 / 4", SS_CENTER, 80, 366, 268, 20);
    C(D_PICNEXT, L"Button", L"\x2192", BS_PUSHBUTTON | WS_TABSTOP, 384, 360, 44, 28);
    C(D_INFO, L"Static", L"", SS_LEFT | SS_NOPREFIX, 442, 54, 146, 300);
    C(D_DESC, L"Edit", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 28, 396, 560, 84);
    C(D_BACK, L"Button", L"\x2190 Back", BS_PUSHBUTTON | WS_TABSTOP, 28, 488, 100, 30);
    C(D_DOWNLOAD, L"Button", L"Download and install", BS_DEFPUSHBUTTON | WS_TABSTOP, 136, 486, 176, 32);
    /* (the uploader's own: Edit, New version, Delete) */
    C(D_EDIT, L"Button", L"Edit\x2026", BS_PUSHBUTTON | WS_TABSTOP, 320, 488, 70, 30);
    C(D_NEWFILE, L"Button", L"New version\x2026", BS_PUSHBUTTON | WS_TABSTOP, 396, 488, 112, 30);
    C(D_DELETE, L"Button", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 514, 488, 74, 30);
    C(D_FIRSTPIC, L"Button", L"Make first", BS_PUSHBUTTON | WS_TABSTOP, 442, 360, 146, 28);
    /* (a download: its bar and Cancel in the buttons' place; the line under them says how far) */
    C(D_DLBAR, PROGRESS_CLASSW, L"", PBS_SMOOTH, 136, 496, 340, 14);
    SendMessageW(s_c[D_DLBAR], PBM_SETRANGE32, 0, 1000);
    C(D_DLCANCEL, L"Button", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 488, 488, 100, 30);
    /* the upload form */
    C(U_TITLE, L"Static", L"Upload a mod: every field but tags and the description is needed. Only .svrmod files.",
      SS_LEFT | SS_NOPREFIX, 28, 54, 560, 20);
    y = 82;
    C(U_NAME_L, L"Static", L"Name", SS_LEFT, 28, y + 4, 90, 20);
    C(U_NAME, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 280, 26);
    SendMessageW(s_c[U_NAME], EM_LIMITTEXT, 60, 0);
    C(U_VER_L, L"Static", L"Version", SS_LEFT, 412, y + 4, 60, 20);
    C(U_VER, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 474, y, 114, 26);
    SendMessageW(s_c[U_VER], EM_LIMITTEXT, 20, 0);
    y += 36;
    C(U_TYPES_L, L"Static", L"Type", SS_LEFT, 28, y + 2, 90, 20);
    for (i = 0; i < 4; i++)
        C(U_T0 + i, L"Button", k_type_names[i], BS_AUTOCHECKBOX | WS_TABSTOP, 120 + i * 116, y, 112, 22);
    y += 32;
    C(U_TAGS_L, L"Static", L"Tags", SS_LEFT, 28, y + 4, 90, 20);
    C(U_TAGS, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 468, 26);
    SendMessageW(s_c[U_TAGS], EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. attitude era, cage, classic (commas between)");
    y += 36;
    C(U_DESC_L, L"Static", L"Description", SS_LEFT, 28, y + 4, 90, 20);
    C(U_DESC, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 468, 76);
    SendMessageW(s_c[U_DESC], EM_LIMITTEXT, 2000, 0);
    y += 86;
    C(U_FILE_L, L"Static", L".svrmod", SS_LEFT, 28, y + 6, 90, 20);
    C(U_FILE, L"Static", L"(none chosen)", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 120, y + 6, 330, 20);
    C(U_FILE_PICK, L"Button", L"Choose\x2026", BS_PUSHBUTTON | WS_TABSTOP, 468, y, 120, 30);
    y += 38;
    C(U_PICS_L, L"Static", L"Pictures", SS_LEFT, 28, y + 6, 90, 20);
    C(U_PICS, L"Static", L"(four, any size: made 1024 x 768)", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 120, y + 6, 330, 20);
    C(U_PICS_PICK, L"Button", L"Choose 4\x2026", BS_PUSHBUTTON | WS_TABSTOP, 468, y, 120, 30);
    y += 40;
    C(U_AUTHOR, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, y, 560, 20);
    y += 28;
    C(U_CANCEL, L"Button", L"\x2190 Back", BS_PUSHBUTTON | WS_TABSTOP, 28, y, 100, 30);
    C(U_SEND, L"Button", L"Upload", BS_DEFPUSHBUTTON | WS_TABSTOP, 136, y - 1, 140, 32);
    C(U_PROGRESS, PROGRESS_CLASSW, L"", PBS_SMOOTH, 288, y + 8, 300, 14);
    SendMessageW(s_c[U_PROGRESS], PBM_SETRANGE32, 0, 1000);
#undef C
    set_view(V_GRID);
}

void discover_set_ini(const WCHAR *ini) { wcscpy_s(s_ini, MAX_PATH, ini ? ini : L""); }

int discover_test_state(int *permille, int *owner_buttons, WCHAR *line, size_t n)
{
    if (permille) *permille = s_dl ? (int)SendMessageW(s_c[D_DLBAR], PBM_GETPOS, 0, 0) : -1;
    if (owner_buttons) *owner_buttons = s_c[D_DELETE] && IsWindowVisible(s_c[D_DELETE]);
    if (line) GetWindowTextW(s_c[D_STATUS], line, (int)n);
    return s_dl;
}

void discover_show(const WCHAR *game_dir)
{
    if (!s_shown && s_ini[0]) {   /* (this run's first visit: what's newer than the last one is "Updated") */
        WCHAR v[32];
        GetPrivateProfileStringW(L"Discovery", L"LastVisit", L"0", v, 32, s_ini);
        s_last_visit = _wtoi64(v);
        swprintf_s(v, 32, L"%lld", (long long)time(NULL));
        WritePrivateProfileStringW(L"Discovery", L"LastVisit", v, s_ini);
    }
    wcscpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"");
    set_view(s_view);   /* (owner_now: a sign-out or another account since hides the owner's buttons here) */
    if (s_view == V_MOD && s_cur >= 0) {   /* signed in with another token since the page was asked: ask again */
        char token[160];
        WCHAR me[8];
        long long my_id = -1;
        s_account(token, sizeof token, me, 8, &my_id);
        if (token[0] && strcmp(token, s_mine_token)) open_mod(s_cur);
    }
    if (s_view == V_UPLOAD) upload_view();
    if (s_view == V_UPLOAD && !s_busy) ShowWindow(s_c[U_PROGRESS], SW_HIDE);
    if (!s_shown) {
        s_shown = 1;
        search();
    }
}

int discover_command(int id, int code)
{
    const int i = id - s_base;
    if (i < 0 || i >= D_COUNT) return 0;
    if (code != BN_CLICKED && i != D_SEARCH && i != D_TYPE && i != D_SORT) return 0;
    switch (i) {
    case D_TYPE: case D_SORT:
        if (code == CBN_SELCHANGE) { s_page = 0; search(); }
        return 1;
    case D_GO: s_page = 0; search(); return 1;
    case D_PREV: if (s_page > 0) { s_page--; search(); } return 1;
    case D_NEXT: if ((s_page + 1) * PER_PAGE < s_total) { s_page++; search(); } return 1;
    case D_UPLOAD:
        if (s_edit) { s_edit = 0; form_clear(); }
        upload_view();
        return 1;
    case D_BACK: s_gen++; s_mine = 0; set_view(V_GRID); return 1;
    case U_CANCEL:   /* (an edit: back to its page) */
        if (s_edit && s_cur >= 0 && !s_busy) { s_edit = 0; form_clear(); open_mod(s_cur); return 1; }
        s_gen++; s_mine = 0; set_view(V_GRID);
        return 1;
    case D_EDIT: if (owner_now()) edit_view(); return 1;
    case D_NEWFILE:
        if (!owner_now()) return 1;
        edit_view();
        SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(s_base + U_FILE_PICK, BN_CLICKED), 0);
        return 1;
    case D_FIRSTPIC: {   /* the picture shown becomes the first (PATCH picture_order) */
        Job *j;
        int k, o = 1;
        if (s_cur < 0 || s_pic == 0 || s_busy || !owner_now()) return 1;
        if (!(j = job(R_EDIT)) || !(j->json = (char *)malloc(64))) { free(j); return 1; }
        sprintf_s(j->json, 64, "{\"picture_order\":[%d", s_pic);
        for (k = 0; k < 4; k++) if (k != s_pic) { sprintf_s(j->json + strlen(j->json), 64 - strlen(j->json), ",%d", k); o++; }
        strcat_s(j->json, 64, "]}");
        j->index = s_mods[s_cur].id;
        s_busy = 1;
        status(L"Saving the picture order\x2026");
        start(j);
        return 1;
    }
    case D_PICPREV: case D_PICNEXT: {
        WCHAR t[16];
        s_pic = (s_pic + (i == D_PICNEXT ? 1 : 3)) % 4;
        swprintf_s(t, 16, L"%d / 4", s_pic + 1);
        SetWindowTextW(s_c[D_PICLBL], t);
        InvalidateRect(s_c[D_PIC], NULL, TRUE);
        return 1;
    }
    case D_DOWNLOAD: {
        Job *j;
        if (s_cur < 0 || s_busy) return 1;
        if (!s_game[0]) { status(L"Install the game first (Setup)."); return 1; }
        if (!(j = job(R_DOWNLOAD))) return 1;
        s_busy = 1;
        s_dl = 1;
        s_dl_cancel = 0;
        s_dl_mod = s_mods[s_cur];
        j->index = s_dl_mod.id;
        swprintf_s(j->path, 700, L"/api/mods/%d/file", s_dl_mod.id);
        SendMessageW(s_c[D_DLBAR], PBM_SETPOS, 0, 0);
        EnableWindow(s_c[D_DLCANCEL], TRUE);
        set_view(s_view);
        status(L"Downloading\x2026");
        start(j);
        return 1;
    }
    case D_DLCANCEL:
        if (s_dl) {
            InterlockedExchange(&s_dl_cancel, 1);
            EnableWindow(s_c[D_DLCANCEL], FALSE);
            status(L"Cancelling the download\x2026");
        }
        return 1;
    case D_DELETE: {
        Job *j;
        WCHAR t[200];
        if (s_cur < 0 || !owner_now()) return 1;
        swprintf_s(t, 200, L"Delete \"%s\" from Discovery for everyone?", s_mods[s_cur].name);
        if (MessageBoxW(s_wnd, t, L"Delete my mod", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 1;
        if (!(j = job(R_DELETE))) return 1;
        swprintf_s(j->path, 700, L"/api/mods/%d", s_mods[s_cur].id);
        start(j);
        return 1;
    }
    case U_FILE_PICK: {
        WCHAR f[1][MAX_PATH];
        if (pick_files(L"Choose the .svrmod to upload", 0, f, 1)) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            WCHAR t[MAX_PATH + 40], sz[32] = L"";
            wcscpy_s(s_up_file, MAX_PATH, f[0]);
            if (GetFileAttributesExW(f[0], GetFileExInfoStandard, &fa))
                fmt_bytes(sz, 32, ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
            swprintf_s(t, MAX_PATH + 40, L"%s  (%s)", wcsrchr(f[0], L'\\') ? wcsrchr(f[0], L'\\') + 1 : f[0], sz);
            SetWindowTextW(s_c[U_FILE], t);
        }
        return 1;
    }
    case U_PICS_PICK: {
        WCHAR p[4][MAX_PATH], t[600] = L"";
        int n = pick_files(s_edit ? L"Choose 1-4 new pictures (they replace the first ones; Ctrl+click)"
                                  : L"Choose four pictures of the mod (Ctrl+click)", 1, p, 4), k;
        if (!n) return 1;
        if (n < 4 && !s_edit) { status(L"Choose four pictures at once (Ctrl+click them)."); return 1; }
        for (k = 0; k < 4; k++) s_up_pics[k][0] = 0;
        for (k = 0; k < n; k++) {
            wcscpy_s(s_up_pics[k], MAX_PATH, p[k]);
            if (t[0]) wcscat_s(t, 600, L", ");
            wcscat_s(t, 600, wcsrchr(p[k], L'\\') ? wcsrchr(p[k], L'\\') + 1 : p[k]);
        }
        SetWindowTextW(s_c[U_PICS], t);
        return 1;
    }
    case U_SEND: send_upload(); return 1;
    }
    return 0;
}

int discover_notify(const NMHDR *nm)
{
    if (!s_c[D_GRID] || nm->hwndFrom != s_c[D_GRID]) return 0;
    if (nm->code == NM_CLICK || nm->code == NM_RETURN) {
        const int i = nm->code == NM_CLICK ? ((const NMITEMACTIVATE *)nm)->iItem
                                           : ListView_GetNextItem(s_c[D_GRID], -1, LVNI_SELECTED);
        if (i >= 0) {
            LVITEMW it;
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (ListView_GetItem(s_c[D_GRID], &it)) open_mod((int)it.lParam);
        }
        return 1;
    }
    return 0;
}

int discover_draw(const DRAWITEMSTRUCT *d)
{
    RECT r = d->rcItem;
    HDC mdc;
    if (!s_c[D_PIC] || d->hwndItem != s_c[D_PIC]) return 0;
    FillRect(d->hDC, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (s_pics[s_pic] && (mdc = CreateCompatibleDC(d->hDC))) {
        HGDIOBJ old = SelectObject(mdc, s_pics[s_pic]);
        SetStretchBltMode(d->hDC, HALFTONE);
        StretchBlt(d->hDC, r.left, r.top, r.right - r.left, r.bottom - r.top, mdc, 0, 0, PIC_W, PIC_H, SRCCOPY);
        SelectObject(mdc, old);
        DeleteDC(mdc);
    } else {
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, RGB(150, 153, 163));
        DrawTextW(d->hDC, L"Loading the pictures\x2026", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return 1;
}

/* An error answer's text ({"error": "..."}), or a general one. */
static void answer_error(const Result *r, const WCHAR *what, WCHAR *out, size_t n)
{
    WCHAR e[200] = L"";
    if (r->data) jstr(jkey((const char *)r->data, (const char *)r->data + r->n, "error"), (const char *)r->data + r->n, e, 200);
    if (r->text[0]) swprintf_s(out, n, L"%s: %s", what, r->text);
    else if (e[0]) swprintf_s(out, n, L"%s: %s", what, e);
    else if (r->status <= 0) swprintf_s(out, n, L"%s: the server didn't answer (online server, internet?).", what);
    else swprintf_s(out, n, L"%s: the server said %d.", what, r->status);
}

int discover_message(UINT m, WPARAM wp, LPARAM lp)
{
    Result *r;
    WCHAR t[400];
    if (m != WM_APP_DISCOVER) return 0;
    if (wp == R_PROGRESS) {
        SendMessageW(s_c[U_PROGRESS], PBM_SETPOS, (WPARAM)lp, 0);
        return 1;
    }
    r = (Result *)lp;
    if (!r) return 1;
    if ((r->kind == R_LIST || r->kind == R_THUMB || r->kind == R_DETAIL || r->kind == R_PIC || r->kind == R_RECONNECT) &&
        r->gen != s_gen) {
        free(r->data);
        free(r);
        return 1;
    }
    switch (r->kind) {
    case R_RECONNECT:   /* (the list / page comes back by itself when the server answers) */
        if (!s_dl) status(r->text);
        break;
    case R_DLPROG:
        if (s_dl) {
            const char *v = getenv("SVR2011_DISCOVER_CANCEL_AT");   /* (test aid: Cancel at that many 0.1 %) */
            SendMessageW(s_c[D_DLBAR], PBM_SETPOS, (WPARAM)r->status, 0);
            if (!s_dl_cancel) status(r->text);
            if (v && *v && r->status >= atoi(v) && !s_dl_cancel)
                SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(s_base + D_DLCANCEL, BN_CLICKED), 0);
        }
        break;
    case R_LIST: {
        const char *p, *end = (const char *)r->data + r->n;
        ListView_DeleteAllItems(s_c[D_GRID]);
        ImageList_RemoveAll(s_thumbs);
        s_n = 0;
        if (r->status != 200 || !r->data) { answer_error(r, L"Discovery", t, 400); status(t); break; }
        s_total = (int)jnum(jkey((const char *)r->data, end, "total"));
        p = jkey((const char *)r->data, end, "mods");
        while (p && p < end && s_n < PER_PAGE && (p = strchr(p, '{')) && p < end) {
            const char *e = jobject_end(p, end);
            LVITEMW it;
            WCHAR label[140];
            parse_mod(p, e, &s_mods[s_n]);
            {
                const Mod *m = &s_mods[s_n];
                const long long got = downloaded_stamp(m->id), now_v = m->updated ? m->updated : m->created;
                if (got && now_v > got)
                    swprintf_s(label, 140, L"\x2191 Update: %s", m->name);   /* (downloaded here; newer now) */
                else if (m->updated && s_last_visit && m->updated > s_last_visit)
                    swprintf_s(label, 140, L"\x2022 Updated: %s", m->name);
                else
                    wcscpy_s(label, 140, m->name);
            }
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            it.iItem = s_n;
            it.iImage = -1;
            it.lParam = s_n;
            it.pszText = label;
            ListView_InsertItem(s_c[D_GRID], &it);
            s_n++;
            p = e;
        }
        swprintf_s(t, 400, s_total ? L"Page %d of %d" : L"", s_page + 1, (s_total + PER_PAGE - 1) / PER_PAGE);
        SetWindowTextW(s_c[D_PAGE], t);
        EnableWindow(s_c[D_PREV], s_page > 0);
        EnableWindow(s_c[D_NEXT], (s_page + 1) * PER_PAGE < s_total);
        swprintf_s(t, 400, s_total ? L"%d mod%s found. Click one for its page." : L"No mods found yet - be the first: Upload a mod.",
                   s_total, s_total == 1 ? L"" : L"s");
        status(t);
        static int tested;
        if (!tested++) {   /* test aids (--capture discover), after the first list only: SVR2011_DISCOVER_OPEN=<n> opens
                              the n-th mod's page, SVR2011_DISCOVER_UPLOAD=1 the upload form */
            const char *v = getenv("SVR2011_DISCOVER_OPEN");
            if (v && *v && atoi(v) < s_n) open_mod(atoi(v));
            else if ((v = getenv("SVR2011_DISCOVER_UPLOAD")) && *v == '1') upload_view();
            else if ((v = getenv("SVR2011_DISCOVER_SEND")) && *v && !s_busy) {
                /* SVR2011_DISCOVER_SEND=<file.svrmod>|<picture>: the form filled (the picture four times), sent */
                WCHAR w[2 * MAX_PATH], *bar;
                int k;
                MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 2 * MAX_PATH);
                if ((bar = wcschr(w, L'|')) != NULL) {
                    *bar = 0;
                    upload_view();
                    SetWindowTextW(s_c[U_NAME], L"Upload test");
                    SetWindowTextW(s_c[U_VER], L"0.1");
                    SetWindowTextW(s_c[U_TAGS], L"Test, Launcher");
                    SendMessageW(s_c[U_T2], BM_SETCHECK, BST_CHECKED, 0);
                    wcscpy_s(s_up_file, MAX_PATH, w);
                    for (k = 0; k < 4; k++) wcscpy_s(s_up_pics[k], MAX_PATH, bar + 1);
                    send_upload();
                }
            }
        }
        break;
    }
    case R_THUMB: {
        HBITMAP b = picture_bitmap(r->data, r->n, THUMB_W, THUMB_H);
        if (b && r->index < s_n) {
            LVITEMW it;
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_IMAGE;
            it.iItem = r->index;
            it.iImage = ImageList_Add(s_thumbs, b, NULL);
            ListView_SetItem(s_c[D_GRID], &it);
        }
        if (b) DeleteObject(b);
        break;
    }
    case R_DETAIL: {
        const char *end = (const char *)r->data + r->n;
        WCHAR desc[2001], sz[32];
        Mod *md = &s_mods[s_cur >= 0 ? s_cur : 0];
        if (r->status != 200 || !r->data) { answer_error(r, L"The mod's page", t, 400); status(t); break; }
        parse_mod((const char *)r->data, end, md);
        jstr(jkey((const char *)r->data, end, "description"), end, desc, 2001);
        wcscpy_s(s_cur_desc, 2001, desc);
        {   /* the server's word on whose it is (asked with the token), when it gives one */
            const char *mine = jkey((const char *)r->data, end, "mine");
            s_mine = mine && !strncmp(mine, "true", 4);
            if (s_view == V_MOD) set_view(V_MOD);
        }
        {
            const long long got = downloaded_stamp(md->id), now_v = md->updated ? md->updated : md->created;
            SetWindowTextW(s_c[D_DOWNLOAD], got && now_v > got ? L"Download the update" : L"Download and install");
        }
        {   /* test aid (--capture discover with SVR2011_DISCOVER_OPEN), once: SVR2011_DISCOVER_DOWNLOAD=1 downloads it
               (the capture waits for the download: discover_test_state) */
            static int downloaded;
            const char *v = getenv("SVR2011_DISCOVER_DOWNLOAD");
            if (v && *v == '1' && !downloaded++)
                PostMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(s_base + D_DOWNLOAD, BN_CLICKED), 0);
        }
        {   /* test aid (--capture discover with SVR2011_DISCOVER_OPEN), once: SVR2011_DISCOVER_EDIT=1 edits the mod -
               " (edited)" on its name, SVR2011_DISCOVER_EDIT_PIC=<picture> as its 2nd picture,
               SVR2011_DISCOVER_EDIT_FILE=<svrmod> as a new version "<version>.1"; =first: the 3rd picture first */
            static int edited;
            const char *v = getenv("SVR2011_DISCOVER_EDIT");
            if (v && *v && !edited++ && owner_now()) {
                if (!strcmp(v, "view")) {   /* (=view: the edit form only) */
                    edit_view();
                } else if (!strcmp(v, "first")) {
                    s_pic = 2;
                    SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(s_base + D_FIRSTPIC, BN_CLICKED), 0);
                } else {
                    WCHAR w[MAX_PATH], nv[40];
                    const char *pic = getenv("SVR2011_DISCOVER_EDIT_PIC"), *file = getenv("SVR2011_DISCOVER_EDIT_FILE");
                    edit_view();
                    swprintf_s(w, MAX_PATH, L"%s (edited)", md->name);
                    SetWindowTextW(s_c[U_NAME], w);
                    if (pic && *pic) MultiByteToWideChar(CP_UTF8, 0, pic, -1, s_up_pics[1], MAX_PATH);
                    if (file && *file) {
                        MultiByteToWideChar(CP_UTF8, 0, file, -1, s_up_file, MAX_PATH);
                        swprintf_s(nv, 40, L"%s.1", md->version);
                        SetWindowTextW(s_c[U_VER], nv);
                    }
                    send_edit();
                }
            }
        }
        jstr(jkey((const char *)r->data, end, "file_name"), end, s_file_name, MAX_PATH);
        fmt_bytes(sz, 32, md->size);
        {
            WCHAR when[48], date[32];
            fmt_date(date, 32, md->updated ? md->updated : md->created);
            swprintf_s(when, 48, md->updated ? L"Updated %s" : L"New (%s)", date);
            swprintf_s(t, 400, L"%s\nby %s\n\n%s\nVersion %s\n%s\n%s\n%d download%s%s%s", md->name, md->author, md->types,
                       md->version, when, sz, md->downloads, md->downloads == 1 ? L"" : L"s",
                       md->tags[0] ? L"\n\nTags: " : L"", md->tags);
        }
        SetWindowTextW(s_c[D_INFO], t);
        {   /* (the description's line ends as the edit control wants them) */
            WCHAR crlf[4100];
            size_t a = 0, b2 = 0;
            for (; desc[a] && b2 + 3 < 4100; a++) {
                if (desc[a] == L'\n') crlf[b2++] = L'\r';
                crlf[b2++] = desc[a];
            }
            crlf[b2] = 0;
            SetWindowTextW(s_c[D_DESC], crlf[0] ? crlf : L"(no description)");
        }
        status(L"");
        break;
    }
    case R_PIC:
        if (r->index >= 0 && r->index < 4) {
            if (s_pics[r->index]) DeleteObject(s_pics[r->index]);
            s_pics[r->index] = picture_bitmap(r->data, r->n, PIC_W, PIC_H);
            if (r->index == s_pic) InvalidateRect(s_c[D_PIC], NULL, TRUE);
        }
        break;
    case R_DOWNLOAD:
        s_busy = 0;
        s_dl = 0;
        set_view(s_view);
        if (r->status == -2) { swprintf_s(t, 400, L"Download of \"%s\" cancelled - nothing was installed.", s_dl_mod.name); status(t); break; }
        if (r->status != 200) {
            swprintf_s(t, 400, L"Download of \"%s\" failed (%s) - nothing was installed. Try again in a moment.",
                       s_dl_mod.name, r->text);
            status(t);
            break;
        }
        if (mods_install(s_game, r->text)) {
            if (s_ini[0]) {
                WCHAR k[32], v[32];
                swprintf_s(k, 32, L"Mod%d", s_dl_mod.id);
                swprintf_s(v, 32, L"%lld", s_dl_mod.updated ? s_dl_mod.updated : s_dl_mod.created);
                WritePrivateProfileStringW(L"Discovery", k, v, s_ini);
                if (s_view == V_MOD && s_cur >= 0 && s_mods[s_cur].id == s_dl_mod.id)
                    SetWindowTextW(s_c[D_DOWNLOAD], L"Download and install");
            }
            swprintf_s(t, 400, L"Installed \"%s\" - it's on the Installed Mods tab and loads the next time the game starts.",
                       s_dl_mod.name);
            status(t);
        } else {
            status(L"Downloaded, but it could not be installed - the Installed Mods tab says why.");
        }
        DeleteFileW(r->text);
        break;
    case R_DELETE:
        if (r->status != 200) { answer_error(r, L"Delete", t, 400); status(t); break; }
        status(L"Deleted from Discovery.");
        set_view(V_GRID);
        search();
        break;
    case R_EDIT:
        s_busy = 0;
        ShowWindow(s_c[U_PROGRESS], SW_HIDE);
        EnableWindow(s_c[U_SEND], TRUE);
        if (r->status != 200) { answer_error(r, L"Saving", t, 400); status(t); break; }
        if (s_view == V_UPLOAD) { s_edit = 0; form_clear(); }
        {   /* the row as it is now: its page again, the list behind it too */
            const char *end = (const char *)r->data + r->n, *row = r->data ? jkey((const char *)r->data, end, "mod") : NULL;
            if (row && *row == '{' && s_cur >= 0) parse_mod(row, jobject_end(row, end), &s_mods[s_cur]);
        }
        status(L"Saved - everyone sees it now.");
        if (s_cur >= 0) open_mod(s_cur);
        break;
    case R_UPLOAD:
        s_busy = 0;
        ShowWindow(s_c[U_PROGRESS], SW_HIDE);
        upload_view();
        if (r->status != 200) { answer_error(r, L"Upload", t, 400); status(t); break; }
        {
            int k;
            static const int clear[] = { U_NAME, U_VER, U_TAGS, U_DESC };
            for (k = 0; k < 4; k++) SetWindowTextW(s_c[clear[k]], L"");
            for (k = 0; k < 4; k++) { SendMessageW(s_c[U_T0 + k], BM_SETCHECK, BST_UNCHECKED, 0); s_up_pics[k][0] = 0; }
            s_up_file[0] = 0;
            SetWindowTextW(s_c[U_FILE], L"(none chosen)");
            SetWindowTextW(s_c[U_PICS], L"(four, any size: made 1024 x 768)");
        }
        status(L"Uploaded - it's in Discovery for everyone now.");
        set_view(V_GRID);
        s_page = 0;
        search();
        break;
    }
    free(r->data);
    free(r);
    return 1;
}
