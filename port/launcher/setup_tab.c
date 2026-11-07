/* WWE SmackDown vs. Raw 2011 PC launcher - the Setup tab.
 *
 * Shown on the launcher's first start (launcher.ini SetupShown) and from
 * its sidebar: each thing a player needs, found or missing -
 *   Game files     the game from the player's disc image (Install tab)
 *   VC++ runtime   MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1 / MSVCP140_ATOMIC_WAIT: Windows' own,
 *                  14.40 or newer (the release ships none; Microsoft's vc_redist.x64 page in the browser)
 *   Media Foundation  user music, movies, Movie Maker (Windows "N": the
 *                  Media Feature Pack, from Windows' Optional features)
 *   Python 3 + numpy + Pillow   the Mod Maker's converters ("Other games",
 *                  "Port"): installed into <game>\Tools\python
 *   ffmpeg         SvR 2010 theme songs: <game>\Tools\ffmpeg (optional)
 *   Bink 2 tool, adb   optional: RAD's download page / the Android tab
 * Downloads come only from the official sites, at pinned versions, sizes
 * and SHA-256s (k_files), after the player said yes to the list; no admin
 * rights, nothing outside the game folder. Licences: <game>\Tools\LICENSES.
 * The same works under Wine / Proton (the Windows builds run there; the
 * Windows-only rows say so).
 */
#include "setup_tab.h"
#include "verify.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "unzip.h"
#include "updater.h"

/* ---- the downloads (pinned: an update to them is a launcher release) */

typedef struct {
    const WCHAR *name, *url, *licence;
    unsigned long long size;
    const char *sha256;
} File;

enum { F_PYTHON, F_NUMPY, F_PILLOW, F_FFMPEG, F_N };
static const File k_files[F_N] = {
    { L"Python 3.14.8 (embeddable, 64-bit)", L"https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip",
      L"PSF licence", 12601679ull, "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310" },
    { L"numpy 2.5.4",
      L"https://files.pythonhosted.org/packages/52/9a/e1e512ebc948d5b9dd33b08736760f0ebbed2848fd4eda1f553088a6dcee/"
      L"numpy-2.5.4-cp314-cp314-win_amd64.whl",
      L"BSD-3-Clause", 12703552ull, "2ca144f15135b6212a5c47b1e2aeca6e412f102f95a2d5d88d8aec77eb255de3" },
    { L"Pillow 12.3.0",
      L"https://files.pythonhosted.org/packages/f1/e0/492879f69d94f91f60fc8cd05ba03650e9520afebb2fb7aa12777d7c7f38/"
      L"pillow-12.3.0-cp314-cp314-win_amd64.whl",
      L"MIT-CMU", 7237707ull, "fdafc9cce40277e0f7a0feabce0ee50dd2fa1800f3b38015e51296b5e814048d" },
    { L"ffmpeg 9.0.2 (gyan.dev essentials build)",
      L"https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-essentials_build.zip",
      L"GPLv3", 114768076ull, "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba" },
};

static const WCHAR *const k_vcrt[] = { L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll",
                                        L"msvcp140_atomic_wait.dll" };

static int is_wine(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version");
}

/* ---- the Visual C++ runtime
 * The game and the Mod Maker use Windows' own VC++ runtime (the launcher has it built in). 2.1.0 shipped
 * copies of the 14.29 DLLs next to the programs: an overlay (Discord, Steam, RTSS, NVIDIA...) built for the
 * newer runtime players have then got them in our processes and its DllMain failed - "unable to start
 * correctly (0xc0000142)". Ours (14.29.x) are deleted from the game folder; any other copy stays. Code built
 * with the 14.40+ toolsets needs a 14.40+ runtime (std::mutex). */

static int exists(const WCHAR *p);
static void setup_join(WCHAR *out, const WCHAR *a, const WCHAR *b);

/* "14.29.30157.0" of a DLL file; 0 when it has none. */
static int file_version(const WCHAR *path, WORD v[4])
{
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    void *buf;
    VS_FIXEDFILEINFO *fi;
    UINT fl;
    int ok = 0;
    if (!n || !(buf = malloc(n))) return 0;
    if (GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &fl) && fi) {
        v[0] = HIWORD(fi->dwFileVersionMS); v[1] = LOWORD(fi->dwFileVersionMS);
        v[2] = HIWORD(fi->dwFileVersionLS); v[3] = LOWORD(fi->dwFileVersionLS);
        ok = 1;
    }
    free(buf);
    return ok;
}

int setup_vcrt_cleanup(const WCHAR *dir, WCHAR *log, size_t logn)
{
    WCHAR p[MAX_PATH], old[MAX_PATH + 8];
    WORD v[4];
    int i, n = 0;
    if (log && logn) log[0] = 0;
    if (!dir || !dir[0]) return 0;
    for (i = 0; i < 4; i++) {
        setup_join(p, dir, k_vcrt[i]);
        swprintf_s(old, MAX_PATH + 8, L"%s.old", p);
        if (exists(old)) DeleteFileW(old);   /* (one put aside at an earlier start) */
        if (!exists(p) || !file_version(p, v) || v[0] != 14 || v[1] != 29) continue;   /* (not ours: kept) */
        if (!DeleteFileW(p)) {   /* (in use: put aside now, deleted at a later start - no delete-at-reboot) */
            if (!MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING)) continue;
        }
        n++;
        if (log) swprintf_s(log + wcslen(log), logn - wcslen(log), L"%s%s %u.%u.%u", log[0] ? L", " : L"",
                            k_vcrt[i], v[0], v[1], v[2]);
    }
    return n;
}

int setup_vcrt_system(WCHAR *ver, size_t vern)
{
    WCHAR sys[MAX_PATH], p[MAX_PATH];
    WORD v[4] = { 0 };
    int i;
    if (ver && vern) ver[0] = 0;
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return SETUP_VCRT_MISSING;
    for (i = 0; i < 4; i++) {
        HMODULE m = LoadLibraryExW(k_vcrt[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return SETUP_VCRT_MISSING;
        FreeLibrary(m);
    }
    setup_join(p, sys, L"msvcp140.dll");
    if (!file_version(p, v)) return is_wine() ? SETUP_VCRT_OK : SETUP_VCRT_OLD;   /* (Wine's builtins: no version) */
    if (ver) swprintf_s(ver, vern, L"%u.%u.%u", v[0], v[1], v[2]);
    return v[0] > 14 || (v[0] == 14 && v[1] >= SETUP_VCRT_MIN_MINOR) ? SETUP_VCRT_OK : SETUP_VCRT_OLD;
}

static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void setup_join(WCHAR *out, const WCHAR *a, const WCHAR *b) { swprintf_s(out, MAX_PATH, L"%s\\%s", a, b); }

static void mb_text(unsigned long long b, WCHAR *out, size_t n)
{
    swprintf_s(out, n, b >= 10000000ull ? L"%.0f MB" : L"%.1f MB", b / 1e6);   /* (decimal MB, as the buttons say) */
}

static const WCHAR *host_of(const WCHAR *url, WCHAR *out, size_t n)
{
    const WCHAR *h = wcsstr(url, L"://");
    size_t k = 0;
    h = h ? h + 3 : url;
    while (h[k] && h[k] != L'/' && k + 1 < n) { out[k] = h[k]; k++; }
    out[k] = 0;
    return out;
}

/* A program on PATH (SearchPathW) - not the Microsoft Store's 0-byte "python" alias. */
static int on_path(const WCHAR *exe, WCHAR *out)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!SearchPathW(NULL, exe, NULL, MAX_PATH, out, NULL)) return 0;
    return GetFileAttributesExW(out, GetFileExInfoStandard, &a) && (a.nFileSizeLow || a.nFileSizeHigh);
}

static int python_works(const WCHAR *python, DWORD wait_ms);

/* ---- checking */

void setup_check(const WCHAR *game, const WCHAR *launcher, SetupRow *rows)
{
    WCHAR p[MAX_PATH], q[MAX_PATH], miss[200];
    const int wine = is_wine();
    int i;
    memset(rows, 0, sizeof(SetupRow) * SETUP_N);

    rows[SETUP_GAME].name = L"Game files";
    setup_join(p, game, L"default.xex");
    if (exists(p) && verify_xex_supported(p) == 0) {   /* (another release: the Japanese disc - it can't run) */
        rows[SETUP_GAME].state = SETUP_MISSING;
        swprintf_s(rows[SETUP_GAME].detail, 400, L"another release of the game in %s (for example the Japanese "
                   L"disc): the port only runs the US / Europe release (title 5451085D) - install from that disc",
                   game);
    } else if (exists(p)) {
        rows[SETUP_GAME].state = SETUP_FOUND;
        swprintf_s(rows[SETUP_GAME].detail, 400, L"in %s", game);
    } else {
        rows[SETUP_GAME].state = SETUP_MISSING;
        wcscpy_s(rows[SETUP_GAME].detail, 400, L"install them from your own disc image (Install tab)");
    }

    rows[SETUP_VCRT].name = L"Visual C++ runtime";
    {
        WCHAR ver[40];
        int st = setup_vcrt_system(ver, 40);
        (void)miss; (void)i;
        if (st == SETUP_VCRT_OK) {
            rows[SETUP_VCRT].state = SETUP_FOUND;
            swprintf_s(rows[SETUP_VCRT].detail, 400, L"%s%s%s", wine ? L"Wine / Proton's own" : L"Windows' own",
                       ver[0] ? L", version " : L"", ver);
        } else {
            rows[SETUP_VCRT].state = SETUP_MISSING;
            swprintf_s(rows[SETUP_VCRT].detail, 400,
                       st == SETUP_VCRT_OLD
                           ? L"version %s is too old: the game and the Mod Maker need 14.%d or newer - install "
                             L"Microsoft's VC++ redistributable (x64), then Check again"
                           : L"%snot installed: the game and the Mod Maker need it (14.%d or newer) - install "
                             L"Microsoft's VC++ redistributable (x64), then Check again",
                       ver, SETUP_VCRT_MIN_MINOR);
        }
    }

    rows[SETUP_MF].name = L"Media Foundation";
    {
        HMODULE a = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE b = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (a && b) {
            rows[SETUP_MF].state = SETUP_FOUND;
            wcscpy_s(rows[SETUP_MF].detail, 400, L"your own music, HD movies and the Movie Maker work");
        } else {
            rows[SETUP_MF].state = SETUP_MISSING;
            wcscpy_s(rows[SETUP_MF].detail, 400,
                     wine ? L"not in this Wine / Proton prefix: the game plays, without your own music and MP4 "
                            L"movies (.bik movies work)"
                          : L"a Windows \"N\" edition: install the Media Feature Pack from Optional "
                            L"features for your own music, MP4 movies and the Movie Maker");
        }
        if (a) FreeLibrary(a);
        if (b) FreeLibrary(b);
    }

    rows[SETUP_PYTHON].name = L"Python 3 + numpy + Pillow";
    setup_join(p, game, L"Tools\\python\\python.exe");
    setup_join(q, game, L"Tools\\python\\Lib\\site-packages");
    {
        WCHAR np[MAX_PATH], pil[MAX_PATH];
        setup_join(np, q, L"numpy");
        setup_join(pil, q, L"PIL");
        if (exists(p) && exists(np) && exists(pil)) {
            rows[SETUP_PYTHON].state = SETUP_FOUND;
            wcscpy_s(rows[SETUP_PYTHON].detail, 400, L"in Tools\\python (for the Mod Maker's converters)");
        } else if (on_path(L"python.exe", q) || on_path(L"python3.exe", q) || on_path(L"py.exe", q)) {
            /* the player's own Python: fine when it has numpy and Pillow (asked once per launcher run -
               a few seconds at most, no window) */
            static WCHAR asked[MAX_PATH];
            static int works;
            if (_wcsicmp(asked, q)) {
                wcscpy_s(asked, MAX_PATH, q);
                works = python_works(q, 8000);
            }
            rows[SETUP_PYTHON].state = works ? SETUP_FOUND : SETUP_MISSING;
            swprintf_s(rows[SETUP_PYTHON].detail, 400,
                       works ? L"your own Python, with numpy and Pillow: %s"
                             : L"%s is on PATH, but without numpy and Pillow: install them into it "
                               L"(pip install numpy pillow), or the Mod Maker's own below", q);
        } else {
            rows[SETUP_PYTHON].state = SETUP_MISSING;
            wcscpy_s(rows[SETUP_PYTHON].detail, 400, L"only the Mod Maker's \"Other games\" and \"Port\" pages need it");
        }
    }

    rows[SETUP_FFMPEG].name = L"ffmpeg";
    setup_join(p, game, L"Tools\\ffmpeg\\ffmpeg.exe");
    if (exists(p)) {
        rows[SETUP_FFMPEG].state = SETUP_FOUND;
        wcscpy_s(rows[SETUP_FFMPEG].detail, 400, L"in Tools\\ffmpeg");
    } else if (on_path(L"ffmpeg.exe", q) || exists(wcscpy(q, L"C:\\ffmpeg\\bin\\ffmpeg.exe"))) {
        rows[SETUP_FFMPEG].state = SETUP_FOUND;
        swprintf_s(rows[SETUP_FFMPEG].detail, 400, L"%s", q);
    } else {
        rows[SETUP_FFMPEG].state = SETUP_OPTIONAL;
        wcscpy_s(rows[SETUP_FFMPEG].detail, 400, L"only for SvR 2010 superstars' theme songs (Mod Maker)");
    }

    rows[SETUP_BINK2].name = L"Bink 2 tool";
    {
        static const WCHAR *const env[] = { L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"LOCALAPPDATA" };
        static const WCHAR *const sub[] = { L"RADVideo", L"RAD Video Tools", L"RAD Game Tools\\RADVideo" };
        int e, s, found = on_path(L"radvideo64.exe", q);
        for (e = 0; e < 4 && !found; e++) {
            WCHAR base[MAX_PATH];
            if (!GetEnvironmentVariableW(env[e], base, MAX_PATH)) continue;
            for (s = 0; s < 3 && !found; s++) {
                swprintf_s(q, MAX_PATH, L"%s\\%s\\radvideo64.exe", base, sub[s]);
                found = exists(q);
            }
        }
        rows[SETUP_BINK2].state = found ? SETUP_FOUND : SETUP_OPTIONAL;
        if (found) swprintf_s(rows[SETUP_BINK2].detail, 400, L"%s", q);
        else wcscpy_s(rows[SETUP_BINK2].detail, 400, L"only for .bk2 videos - RAD Video Tools, your own "
                      L"download from RAD's site");
    }

    rows[SETUP_ADB].name = L"adb (Android)";
    setup_join(p, launcher, L"platform-tools\\adb.exe");
    if (exists(p) || on_path(L"adb.exe", q)) {
        rows[SETUP_ADB].state = SETUP_FOUND;
        swprintf_s(rows[SETUP_ADB].detail, 400, L"%s", exists(p) ? p : q);
    } else {
        rows[SETUP_ADB].state = SETUP_OPTIONAL;
        wcscpy_s(rows[SETUP_ADB].detail, 400, L"downloaded when you install to a phone (Android tab)");
    }
}

/* ---- installing */

static unsigned item_files(unsigned items)
{
    return (items & (1u << SETUP_PYTHON) ? (1u << F_PYTHON) | (1u << F_NUMPY) | (1u << F_PILLOW) : 0) |
           (items & (1u << SETUP_FFMPEG) ? 1u << F_FFMPEG : 0);
}

void setup_describe(unsigned items, WCHAR *out, size_t n, unsigned long long *total)
{
    const unsigned files = item_files(items);
    int f;
    out[0] = 0;
    *total = 0;
    for (f = 0; f < F_N; f++)
        if (files & (1u << f)) {
            WCHAR mb[32], host[128];
            mb_text(k_files[f].size, mb, 32);
            swprintf_s(out + wcslen(out), n - wcslen(out), L"%s - %s - from %s (%s)\n", k_files[f].name, mb,
                       host_of(k_files[f].url, host, 128), k_files[f].licence);
            *total += k_files[f].size;
        }
}

/* The file's SHA-256 as 64 lowercase hex digits. */
static int sha256_file(const WCHAR *path, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char dig[32], *buf;
    HANDLE f;
    DWORD got;
    int ok = 0, i;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    buf = (unsigned char *)malloc(1 << 20);
    if (f == INVALID_HANDLE_VALUE || !buf) goto out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) < 0)
        goto out;
    while (ReadFile(f, buf, 1 << 20, &got, NULL) && got)
        if (BCryptHashData(h, buf, got, 0) < 0) goto out;
    if (BCryptFinishHash(h, dig, 32, 0) < 0) goto out;
    for (i = 0; i < 32; i++) sprintf_s(out + 2 * i, 3, "%02x", dig[i]);
    ok = 1;
out:
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(buf);
    return ok;
}

/* The bar and the line while a file comes: its share lo..hi of the bar, "12 of 115 MB" (the pinned size when
   the server sends no length), at most once a percent. */
typedef struct {
    setup_progress_fn progress;
    int f, lo, hi, last;
} FetchCtx;

static void fetch_progress(uint64_t done, uint64_t total, void *ctx)
{
    FetchCtx *c = (FetchCtx *)ctx;
    WCHAR msg[256], a[32], b[32];
    int pct;
    if (!total) total = k_files[c->f].size;
    pct = total ? (int)(done * 100 / total) : 0;
    if (pct > 100) pct = 100;
    if (pct == c->last) return;
    c->last = pct;
    mb_text(done, a, 32);
    mb_text(total, b, 32);
    swprintf_s(msg, 256, L"Downloading %s: %s of %s (%d%%)\x2026", k_files[c->f].name, a, b, pct);
    c->progress(c->lo + (c->hi - c->lo) * pct / 100, msg);
}

/* Downloads k_files[f] to `path` (the bar from lo to hi permille) and checks its size and SHA-256. */
static int fetch(int f, const WCHAR *path, setup_progress_fn progress, int lo, int hi, volatile LONG *cancel,
                 WCHAR *err, size_t errn)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    char sha[65];
    WCHAR msg[256], mb[32];
    FetchCtx ctx = { progress, f, lo, hi, -1 };
    mb_text(k_files[f].size, mb, 32);
    /* (a download left by an earlier try that failed later, e.g. unpacking: used again when it checks out) */
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) &&
        (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) == k_files[f].size && sha256_file(path, sha) &&
        !strcmp(sha, k_files[f].sha256))
        return 1;
    swprintf_s(msg, 256, L"Downloading %s (%s): connecting\x2026", k_files[f].name, mb);
    if (progress) progress(lo, msg);
    if (!update_download_cb(k_files[f].url, path, progress ? fetch_progress : NULL, &ctx, cancel, err, errn))
        return 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) ||
        (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) != k_files[f].size ||
        !sha256_file(path, sha) || strcmp(sha, k_files[f].sha256)) {
        DeleteFileW(path);
        swprintf_s(err, errn, L"%s didn't match its checksum (a changed or damaged download): nothing was installed "
                              L"from it.", k_files[f].name);
        return 0;
    }
    return 1;
}

static void mkdirs(const WCHAR *dir)
{
    WCHAR p[MAX_PATH];
    WCHAR *s;
    wcscpy_s(p, MAX_PATH, dir);
    for (s = p + 3; *s; s++)
        if (*s == L'\\') { *s = 0; CreateDirectoryW(p, NULL); *s = L'\\'; }
    CreateDirectoryW(p, NULL);
}

/* Deletes a folder tree directly - no shell (its FO_DELETE took 30 s on a just-renamed folder: mods_tab.h
   mods_delete_tree, which the Mod Maker, using this file, doesn't link). A junction: the link only. */
static void remove_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    const DWORD a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    SetFileAttributesW(dir, FILE_ATTRIBUTE_NORMAL);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileW(dir);
        return;
    }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT) && swprintf_s(pat, MAX_PATH, L"%s\\*", dir) > 0) {
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (swprintf_s(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName) > 0) remove_tree(p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    RemoveDirectoryW(dir);
}

/* The embeddable Python ignores site-packages unless its ._pth lists it (and "import site"). */
static int fix_pth(const WCHAR *pydir)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    FILE *f;
    char text[4096] = "", *line, *ctx = NULL, out[4096] = "";
    size_t n;
    int has_sp = 0, has_site = 0;
    setup_join(pat, pydir, L"python*._pth");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    setup_join(path, pydir, fd.cFileName);
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    for (line = strtok_s(text, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx)) {
        if (!strcmp(line, "#import site")) line = "import site";
        if (!strcmp(line, "import site")) has_site = 1;
        if (!_stricmp(line, "Lib\\site-packages")) has_sp = 1;
        strcat_s(out, sizeof out, line);
        strcat_s(out, sizeof out, "\r\n");
    }
    if (!has_sp) strcat_s(out, sizeof out, "Lib\\site-packages\r\n");
    if (!has_site) strcat_s(out, sizeof out, "import site\r\n");
    if (_wfopen_s(&f, path, L"wb") || !f) return 0;
    fwrite(out, 1, strlen(out), f);
    fclose(f);
    return 1;
}

/* Runs `python -c "import numpy, PIL; print('ok')"` (no window): 1 when it says ok. */
static int python_works(const WCHAR *python, DWORD wait_ms)
{
    WCHAR cmd[MAX_PATH + 96];
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char buf[256] = "";
    DWORD got = 0;
    int ok = 0;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    swprintf_s(cmd, MAX_PATH + 96, L"\"%s\" -c \"import numpy, PIL; print('ok')\"", python);
    if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(wr);
        wr = NULL;
        if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_OBJECT_0 && ReadFile(rd, buf, sizeof buf - 1, &got, NULL)) {
            buf[got] = 0;
            ok = strstr(buf, "ok") != NULL;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return ok;
}

static void licences_readme(const WCHAR *game)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    setup_join(dir, game, L"Tools\\LICENSES");
    mkdirs(dir);
    setup_join(path, dir, L"README.txt");
    if (_wfopen_s(&f, path, L"wb") || !f) return;
    fputs("Tools installed by the launcher's Setup tab (WWE SmackDown vs. Raw 2011 PC port).\r\n"
          "They are only for the Mod Maker's converters; the game doesn't use them.\r\n\r\n"
          "Python 3.14.8 (embeddable package) - https://www.python.org\r\n"
          "  PSF License Agreement: Tools\\python\\LICENSE.txt\r\n"
          "numpy 2.5.4 - https://pypi.org/project/numpy/\r\n"
          "  BSD-3-Clause, with the licences of the libraries it bundles (OpenBLAS, ...):\r\n"
          "  Tools\\python\\Lib\\site-packages\\numpy-2.5.4.dist-info\r\n"
          "Pillow 12.3.0 - https://pypi.org/project/pillow/\r\n"
          "  MIT-CMU (HPND), with the licences of the libraries it bundles:\r\n"
          "  Tools\\python\\Lib\\site-packages\\pillow-12.3.0.dist-info\r\n"
          "ffmpeg 9.0.2 (essentials build by gyan.dev, linked from https://ffmpeg.org/download.html)\r\n"
          "  GNU GPL v3: Tools\\ffmpeg\\LICENSE.txt; source: https://ffmpeg.org and https://www.gyan.dev/ffmpeg/builds/\r\n",
          f);
    fclose(f);
}

static int install_python(const WCHAR *game, const WCHAR *tmp, setup_progress_fn progress, volatile LONG *cancel,
                          WCHAR *err, size_t errn)
{
    WCHAR py[MAX_PATH], sp[MAX_PATH], zip[MAX_PATH], exe[MAX_PATH];
    int f;
    setup_join(py, game, L"Tools\\python");
    setup_join(sp, py, L"Lib\\site-packages");
    for (f = F_PYTHON; f <= F_PILLOW; f++) {   /* (all three first: nothing changes unless all arrive) */
        swprintf_s(zip, MAX_PATH, L"%s\\svr2011-setup-%d.zip", tmp, f);
        if (!fetch(f, zip, progress, (f - F_PYTHON) * 250, (f - F_PYTHON + 1) * 250, cancel, err, errn)) return 0;
        if (progress) progress((f - F_PYTHON + 1) * 250, NULL);
    }
    if (progress) progress(800, L"Unpacking Python, numpy and Pillow\x2026");
    remove_tree(py);
    mkdirs(sp);
    for (f = F_PYTHON; f <= F_PILLOW; f++) {
        swprintf_s(zip, MAX_PATH, L"%s\\svr2011-setup-%d.zip", tmp, f);
        if (!unzip_file(zip, f == F_PYTHON ? py : sp)) {   /* (the downloads stay for another try) */
            swprintf_s(err, errn, L"Could not unpack %s into %s.", k_files[f].name, f == F_PYTHON ? py : sp);
            return 0;
        }
    }
    if (!fix_pth(py)) {
        swprintf_s(err, errn, L"Python's ._pth file in %s could not be set up.", py);
        return 0;
    }
    if (progress) progress(950, L"Checking Python\x2026");
    setup_join(exe, py, L"python.exe");
    if (!python_works(exe, 60000)) {
        swprintf_s(err, errn, L"Python was installed, but \"import numpy, PIL\" didn't work in it.");
        return 0;
    }
    for (f = F_PYTHON; f <= F_PILLOW; f++) {
        swprintf_s(zip, MAX_PATH, L"%s\\svr2011-setup-%d.zip", tmp, f);
        DeleteFileW(zip);
    }
    return 1;
}

static int install_ffmpeg(const WCHAR *game, const WCHAR *tmp, setup_progress_fn progress, volatile LONG *cancel,
                          WCHAR *err, size_t errn)
{
    WCHAR zip[MAX_PATH], unp[MAX_PATH], dir[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    swprintf_s(zip, MAX_PATH, L"%s\\svr2011-setup-ffmpeg.zip", tmp);
    swprintf_s(unp, MAX_PATH, L"%s\\svr2011-setup-ffmpeg", tmp);
    if (!fetch(F_FFMPEG, zip, progress, 0, 800, cancel, err, errn)) return 0;
    if (progress) progress(800, L"Unpacking ffmpeg\x2026");
    remove_tree(unp);
    mkdirs(unp);
    if (!unzip_file(zip, unp)) {   /* (the download stays for another try) */
        remove_tree(unp);
        swprintf_s(err, errn, L"Could not unpack ffmpeg.");
        return 0;
    }
    setup_join(dir, game, L"Tools\\ffmpeg");
    mkdirs(dir);
    setup_join(from, unp, L"ffmpeg-9.0.2-essentials_build\\bin\\ffmpeg.exe");
    setup_join(to, dir, L"ffmpeg.exe");
    if (!MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        remove_tree(unp);
        swprintf_s(err, errn, L"Could not put ffmpeg.exe into %s (error %lu).", dir, GetLastError());
        return 0;
    }
    setup_join(from, unp, L"ffmpeg-9.0.2-essentials_build\\LICENSE");
    setup_join(to, dir, L"LICENSE.txt");
    MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    remove_tree(unp);
    DeleteFileW(zip);
    return 1;
}

int setup_install(const WCHAR *game, unsigned items, setup_progress_fn progress, volatile LONG *cancel, WCHAR *err,
                  size_t errn)
{
    WCHAR tmp[MAX_PATH];
    err[0] = 0;
    /* (the downloads in the game folder's Tools\.downloads, not %TEMP%: a program that downloads into %TEMP%
       and unpacks programs from there looks like a dropper to antivirus heuristics) */
    setup_join(tmp, game, L"Tools\\.downloads");
    mkdirs(tmp);
    if (!exists(tmp)) {
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (!n || n >= MAX_PATH) wcscpy_s(tmp, MAX_PATH, game);
        else if (tmp[n - 1] == L'\\') tmp[n - 1] = 0;
    }
    if ((items & (1u << SETUP_PYTHON)) && !install_python(game, tmp, progress, cancel, err, errn)) return 0;
    if ((items & (1u << SETUP_FFMPEG)) && !install_ffmpeg(game, tmp, progress, cancel, err, errn)) return 0;
    licences_readme(game);
    if (progress) progress(1000, L"Done.");
    return 1;
}

/* ---- the tab */

enum { S_INTRO, S_ROW0, S_INSTALL = S_ROW0 + SETUP_N, S_FFMPEG, S_RECHECK, S_GOTO_INSTALL, S_FEATURES, S_RAD,
       S_STOP, S_PROGRESS, S_STATUS, S_DONE, S_VCRT, S_COUNT };

static HWND s_wnd, s_ctl[S_COUNT];
static int s_base;
static setup_goto_fn s_go_install;
static WCHAR s_game[MAX_PATH], s_launcher[MAX_PATH];
static SetupRow s_rows[SETUP_N];
static volatile LONG s_busy, s_cancel;
static unsigned s_items;
static WCHAR s_err[600];
static int s_ok;

static void refresh(void)
{
    static const WCHAR *const state[] = { L"\x2714 found", L"\x2716 missing", L"optional" };
    int i;
    setup_check(s_game, s_launcher, s_rows);
    for (i = 0; i < SETUP_N; i++) {
        WCHAR t[512];
        swprintf_s(t, 512, L"%s:  %s\n%s", s_rows[i].name, state[s_rows[i].state], s_rows[i].detail);
        SetWindowTextW(s_ctl[S_ROW0 + i], t);
    }
    EnableWindow(s_ctl[S_INSTALL], !s_busy && s_rows[SETUP_PYTHON].state != SETUP_FOUND);
    EnableWindow(s_ctl[S_FFMPEG], !s_busy && s_rows[SETUP_FFMPEG].state != SETUP_FOUND);
    EnableWindow(s_ctl[S_RECHECK], !s_busy);
    EnableWindow(s_ctl[S_STOP], s_busy != 0);
    ShowWindow(s_ctl[S_GOTO_INSTALL], s_rows[SETUP_GAME].state == SETUP_FOUND ? SW_HIDE : SW_SHOW);
    ShowWindow(s_ctl[S_FEATURES], s_rows[SETUP_MF].state == SETUP_FOUND || is_wine() ? SW_HIDE : SW_SHOW);
    ShowWindow(s_ctl[S_RAD], s_rows[SETUP_BINK2].state == SETUP_FOUND ? SW_HIDE : SW_SHOW);
    ShowWindow(s_ctl[S_VCRT], s_rows[SETUP_VCRT].state == SETUP_FOUND ? SW_HIDE : SW_SHOW);
}

static void tab_progress(int permille, const WCHAR *text)
{
    PostMessageW(s_ctl[S_PROGRESS], PBM_SETPOS, (WPARAM)permille, 0);
    if (text) SetWindowTextW(s_ctl[S_STATUS], text);
}

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    s_ok = setup_install(s_game, s_items, tab_progress, &s_cancel, s_err, 600);
    PostMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(s_base + S_DONE, 0), 0);
    return 0;
}

static void start(unsigned items)
{
    WCHAR list[1024], text[1400], mb[32];
    unsigned long long total;
    HANDLE t;
    if (s_busy) return;
    setup_describe(items, list, 1024, &total);
    mb_text(total, mb, 32);
    swprintf_s(text, 1400, L"This downloads %s from the official sites and installs it into %s\\Tools "
               L"(no admin rights; nothing changes outside the game folder):\n\n%s\n"
               L"Each file is checked against its known checksum. The licences go into Tools\\LICENSES.\n\n"
               L"Download now?", mb, s_game, list);
    if (MessageBoxW(s_wnd, text, L"First time setup", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    s_items = items;
    s_cancel = 0;
    s_busy = 1;
    SendMessageW(s_ctl[S_PROGRESS], PBM_SETPOS, 0, 0);
    refresh();
    t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else { s_busy = 0; refresh(); }
}

void setup_build(HWND wnd, mods_add_fn add, int tab, int tools_tab, int id_base, setup_goto_fn go_install)
{
    const int x = 28;
    int i, y;
    s_wnd = wnd;
    s_base = id_base;
    s_go_install = go_install;
    /* This PC: the game, the VC++ runtime, Media Foundation (rows 0-2); Tools: the rest (optional) */
    add(tab, L"Button", L"What this PC has", BS_GROUPBOX, x, 4, 560, 240, 0);
    s_ctl[S_INTRO] = add(tab, L"Static", L"What the game needs on this PC. The Mod Maker's tools (Python, ffmpeg) "
                         L"are an optional step of their own: Tools.",
                         SS_LEFT | SS_NOPREFIX, x + 16, 34, 528, 34, id_base + S_INTRO);
    add(tools_tab, L"Button", L"Mod Maker tools (optional)", BS_GROUPBOX, x, 4, 560, 318, 0);
    add(tools_tab, L"Static", L"Only for making mods: Python and ffmpeg run the Mod Maker's converters, Bink 2 makes "
                              L"HD entrance videos, adb installs the game to a phone (Android step). Playing needs none.",
        SS_LEFT | SS_NOPREFIX, x + 16, 34, 528, 34, 0);
    for (i = 0; i < SETUP_N; i++) {
        const int tools = i > SETUP_MF;
        y = 72 + (tools ? i - SETUP_MF - 1 : i) * 42;
        s_ctl[S_ROW0 + i] = add(tools ? tools_tab : tab, L"Static", L"", SS_LEFT | SS_NOPREFIX, x + 16, y,
                                i == SETUP_VCRT ? 384 : 528, 40, id_base + S_ROW0 + i);
    }
    /* (the VC++ runtime's download page in the player's browser - Microsoft's own link, nothing downloaded here) */
    s_ctl[S_VCRT] = add(tab, L"Button", L"Get the VC++ runtime", BS_PUSHBUTTON | WS_TABSTOP, x + 404, 72 + SETUP_VCRT * 42 + 4,
                        140, 28, id_base + S_VCRT);
    y = 72 + (SETUP_N - SETUP_MF - 1) * 42 + 4;
    s_ctl[S_INSTALL] = add(tools_tab, L"Button", L"Install Python + numpy + Pillow (33 MB)", BS_DEFPUSHBUTTON | WS_TABSTOP,
                           x + 16, y, 270, 32, id_base + S_INSTALL);
    s_ctl[S_FFMPEG] = add(tools_tab, L"Button", L"Install ffmpeg (115 MB)", BS_PUSHBUTTON | WS_TABSTOP, x + 296, y, 170, 32,
                          id_base + S_FFMPEG);
    s_ctl[S_STOP] = add(tools_tab, L"Button", L"Stop", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, x + 476, y, 68, 32,
                        id_base + S_STOP);
    y += 38;
    s_ctl[S_RAD] = add(tools_tab, L"Button", L"RAD's download page", BS_PUSHBUTTON | WS_TABSTOP, x + 16, y, 150, 28,
                       id_base + S_RAD);
    add(tools_tab, L"Button", L"Check again", BS_PUSHBUTTON | WS_TABSTOP, x + 176, y, 120, 28, id_base + S_RECHECK);
    /* This PC's buttons */
    y = 72 + (SETUP_MF + 1) * 42 + 4;
    s_ctl[S_RECHECK] = add(tab, L"Button", L"Check again", BS_PUSHBUTTON | WS_TABSTOP, x + 16, y, 120, 28,
                           id_base + S_RECHECK);
    s_ctl[S_GOTO_INSTALL] = add(tab, L"Button", L"Next: the game", BS_PUSHBUTTON | WS_TABSTOP, x + 146, y, 130, 28,
                                id_base + S_GOTO_INSTALL);
    s_ctl[S_FEATURES] = add(tab, L"Button", L"Optional features", BS_PUSHBUTTON | WS_TABSTOP, x + 286, y, 140, 28,
                            id_base + S_FEATURES);
    s_ctl[S_PROGRESS] = add(tools_tab, PROGRESS_CLASSW, L"", PBS_SMOOTH, x, 330, 560, 18, id_base + S_PROGRESS);
    SendMessageW(s_ctl[S_PROGRESS], PBM_SETRANGE32, 0, 1000);
    s_ctl[S_STATUS] = add(tools_tab, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, x, 354, 560, 36,
                          id_base + S_STATUS);
}

void setup_show(const WCHAR *game, const WCHAR *launcher)
{
    wcscpy_s(s_game, MAX_PATH, game);
    wcscpy_s(s_launcher, MAX_PATH, launcher);
    refresh();
}

int setup_busy(void) { return s_busy != 0; }

int setup_command(int id, int code)
{
    (void)code;
    if (id < s_base || id >= s_base + S_COUNT) return 0;
    switch (id - s_base) {
    case S_INSTALL: start(1u << SETUP_PYTHON); break;
    case S_FFMPEG: start(1u << SETUP_FFMPEG); break;
    case S_STOP: s_cancel = 1; break;
    case S_RECHECK: refresh(); SetWindowTextW(s_ctl[S_STATUS], L""); break;
    case S_GOTO_INSTALL: if (s_go_install) s_go_install(); break;
    case S_FEATURES: ShellExecuteW(s_wnd, L"open", L"ms-settings:optionalfeatures", NULL, NULL, SW_SHOWNORMAL); break;
    case S_RAD: ShellExecuteW(s_wnd, L"open", L"https://www.radgametools.com/bnkdown.htm", NULL, NULL, SW_SHOWNORMAL); break;
    case S_VCRT: ShellExecuteW(s_wnd, L"open", SETUP_VCRT_URL, NULL, NULL, SW_SHOWNORMAL); break;
    case S_DONE:
        s_busy = 0;
        SendMessageW(s_ctl[S_PROGRESS], PBM_SETPOS, s_ok ? 1000 : 0, 0);
        SetWindowTextW(s_ctl[S_STATUS], s_ok ? L"Installed. The Mod Maker finds them in Tools next time it checks."
                                             : s_err);
        refresh();
        break;
    default: return 0;
    }
    return 1;
}
