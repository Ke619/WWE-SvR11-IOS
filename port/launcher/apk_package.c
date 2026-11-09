/* WWE SmackDown vs. Raw 2011 launcher - the Android app: "Create APK Package"
   and "Install to phone" over USB (apk_package.h).

   The game zip is stored (not compressed: the disc files are compressed
   already) and always Zip64, since the game is over 4 GB. Each file is
   streamed once: its local header is written with placeholder CRC and sizes
   and patched after the data. The phone app (android/InstallActivity.java)
   extracts it into games/WWE SmackDown vs. Raw 2011. */

#include "apk_package.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APK_NAME     L"SvR2011.apk"
#define ZIP_NAME     L"SvR2011-Game.zip"
#define CHUNK        (8u << 20)

typedef struct {
    WCHAR    name[MAX_PATH];     /* relative, '/' separated (UTF-16) */
    uint64_t size, offset;
    uint32_t crc;
    uint16_t time, date;
    int      dir;
    WCHAR    src[MAX_PATH];      /* the file's path when not root\name (online.toml) */
} Entry;

typedef struct {
    const WCHAR    *root, *out_dir;
    Entry          *e;
    int             n, cap;
    uint64_t        total, done;
    volatile LONG  *cancel;
    apk_progress_fn progress;
    int             last;
    int             update;      /* (USB install over an installed game: the phone's own state stays) */
} Pack;

static uint32_t s_crc_table[256];

static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Left out of the package: the Windows programs and the PC's own state. */
static int skipped(const Pack *p, const WCHAR *rel, int dir)
{
    static const WCHAR *dirs[] = { L"logs", L"SaveBackups", L"Android", L"Switch", L"platform-tools", L"$SystemUpdate",
                                   L"UserData/cache", L"UserData/crashes",
                                   L"Mods/ArenaOverlay" /* (the game makes it: links to pac/bg) */,
                                   L"Mods/SuperstarOverlay" /* (the game makes it from Mods/Superstars) */,
                                   L"Texture Dumps" /* (for pack makers, on the PC) */ };
    static const WCHAR *files[] = { L"launcher.ini", L"svr2011.toml" };
    const WCHAR *ext = wcsrchr(rel, L'.');
    int i;
    if (dir) {
        if (p->update && (!_wcsicmp(rel, L"Saves") || !_wcsicmp(rel, L"UserData")))
            return 1;
        for (i = 0; i < (int)(sizeof dirs / sizeof dirs[0]); i++)
            if (!_wcsicmp(rel, dirs[i]))
                return 1;
        return 0;
    }
    for (i = 0; i < (int)(sizeof files / sizeof files[0]); i++)
        if (!_wcsicmp(rel, files[i]))
            return 1;
    return !wcschr(rel, L'/') && ext &&
           (!_wcsicmp(ext, L".exe") || !_wcsicmp(ext, L".dll") || !_wcsicmp(ext, L".pdb"));
}

static int add(Pack *p, const WCHAR *rel, uint64_t size, const FILETIME *ft, int dir)
{
    Entry *e;
    FILETIME local;
    WORD d = 0x21, t = 0;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 1024;
        Entry *ne = (Entry *)realloc(p->e, cap * sizeof(Entry));
        if (!ne)
            return 0;
        p->e = ne;
        p->cap = cap;
    }
    e = &p->e[p->n++];
    memset(e, 0, sizeof *e);
    swprintf_s(e->name, MAX_PATH, dir ? L"%s/" : L"%s", rel);
    e->size = size;
    e->dir = dir;
    if (FileTimeToLocalFileTime(ft, &local))
        FileTimeToDosDateTime(&local, &d, &t);
    e->date = d;
    e->time = t;
    p->total += size;
    return 1;
}

/* Lists root\rel into p (directories as entries too, so empty ones exist on
   the phone). */
static int walk(Pack *p, const WCHAR *rel)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pattern, MAX_PATH, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*", p->root, rel);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        swprintf_s(child, MAX_PATH, rel[0] ? L"%s/%s" : L"%s%s", rel, fd.cFileName);
        if (skipped(p, child, dir))
            continue;
        if (dir) {
            swprintf_s(full, MAX_PATH, L"%s\\%s", p->root, child);
            if (p->out_dir && !_wcsicmp(full, p->out_dir))  /* (packaging into the game folder) */
                continue;
            if (!add(p, child, 0, &fd.ftLastWriteTime, 1) || !walk(p, child)) {
                FindClose(h);
                return 0;
            }
        } else if (!add(p, child, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                        &fd.ftLastWriteTime, 0)) {
            FindClose(h);
            return 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* ── little-endian record writing ───────────────────────────────────────── */

typedef struct { uint8_t b[512]; int n; } Rec;
static void r16(Rec *r, uint32_t v) { r->b[r->n++] = (uint8_t)v; r->b[r->n++] = (uint8_t)(v >> 8); }
static void r32(Rec *r, uint32_t v) { r16(r, v & 0xFFFF); r16(r, v >> 16); }
static void r64(Rec *r, uint64_t v) { r32(r, (uint32_t)v); r32(r, (uint32_t)(v >> 32)); }
static void rbytes(Rec *r, const void *p, int n) { memcpy(r->b + r->n, p, n); r->n += n; }

static int write_all(HANDLE f, const void *p, DWORD n)
{
    DWORD w;
    return WriteFile(f, p, n, &w, NULL) && w == n;
}

static uint64_t tell(HANDLE f)
{
    LARGE_INTEGER z = { 0 }, pos;
    SetFilePointerEx(f, z, &pos, FILE_CURRENT);
    return (uint64_t)pos.QuadPart;
}

static void seek(HANDLE f, uint64_t at)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)at;
    SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
}

static int utf8(const WCHAR *s, char *out, int outn)
{
    int k = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outn, NULL, NULL);
    return k > 0 ? k - 1 : -1;
}

static void progress(Pack *p, const WCHAR *name)
{
    int pm = p->total ? (int)(p->done * 1000 / p->total) : 0;
    WCHAR msg[MAX_PATH + 64];
    if (pm == p->last && !name)
        return;
    p->last = pm;
    if (name)
        swprintf_s(msg, MAX_PATH + 64, L"Packing %s\x2026", name);
    p->progress(pm, name ? msg : NULL);
}

/* One entry: local header, data (CRC while copying), header patched. */
static int write_entry(Pack *p, HANDLE out, Entry *e, uint8_t *buf, WCHAR *err, int errn)
{
    char name[MAX_PATH * 3];
    int nlen = utf8(e->name, name, sizeof name);
    Rec r = { { 0 }, 0 };
    uint64_t header, got = 0;
    if (nlen < 0) {
        swprintf_s(err, errn, L"Bad file name: %s", e->name);
        return 0;
    }
    e->offset = header = tell(out);
    r32(&r, 0x04034b50); r16(&r, 45); r16(&r, 0x0800); r16(&r, 0);
    r16(&r, e->time); r16(&r, e->date);
    r32(&r, 0); r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF);
    r16(&r, nlen); r16(&r, 20);
    if (!write_all(out, r.b, r.n) || !write_all(out, name, nlen))
        goto write_failed;
    r.n = 0;
    r16(&r, 0x0001); r16(&r, 16); r64(&r, 0); r64(&r, 0);
    if (!write_all(out, r.b, r.n))
        goto write_failed;
    if (!e->dir) {
        WCHAR full[MAX_PATH], *c;
        HANDLE in;
        swprintf_s(full, MAX_PATH, L"%s\\%s", p->root, e->name);
        if (e->src[0])
            wcscpy_s(full, MAX_PATH, e->src);
        for (c = full; *c; c++)
            if (*c == L'/')
                *c = L'\\';
        in = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (in == INVALID_HANDLE_VALUE) {
            swprintf_s(err, errn, L"Could not read %s", full);
            return 0;
        }
        for (;;) {
            DWORD n = 0;
            if (*p->cancel) {
                CloseHandle(in);
                swprintf_s(err, errn, L"Cancelled.");
                return 0;
            }
            if (!ReadFile(in, buf, CHUNK, &n, NULL)) {
                CloseHandle(in);
                swprintf_s(err, errn, L"Could not read %s", full);
                return 0;
            }
            if (!n)
                break;
            e->crc = crc_update(e->crc, buf, n);
            if (!write_all(out, buf, n)) {
                CloseHandle(in);
                goto write_failed;
            }
            got += n;
            p->done += n;
            progress(p, NULL);
        }
        CloseHandle(in);
        e->size = got;  /* (as read, should it differ from the listing) */
    }
    {
        uint64_t end = tell(out);
        Rec c = { { 0 }, 0 }, z = { { 0 }, 0 };
        r32(&c, e->crc);
        r64(&z, e->size); r64(&z, e->size);
        seek(out, header + 14);
        if (!write_all(out, c.b, c.n))
            goto write_failed;
        seek(out, header + 30 + nlen + 4);
        if (!write_all(out, z.b, z.n))
            goto write_failed;
        seek(out, end);
    }
    return 1;
write_failed:
    swprintf_s(err, errn, L"Could not write the package (is the drive full?).");
    return 0;
}

static int write_central(Pack *p, HANDLE out, WCHAR *err, int errn)
{
    uint64_t cd = tell(out), cd_size, eocd64;
    Rec r;
    int i;
    for (i = 0; i < p->n; i++) {
        Entry *e = &p->e[i];
        char name[MAX_PATH * 3];
        int nlen = utf8(e->name, name, sizeof name);
        r.n = 0;
        r32(&r, 0x02014b50); r16(&r, 45); r16(&r, 45); r16(&r, 0x0800); r16(&r, 0);
        r16(&r, e->time); r16(&r, e->date);
        r32(&r, e->crc); r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF);
        r16(&r, nlen); r16(&r, 28); r16(&r, 0); r16(&r, 0); r16(&r, 0);
        r32(&r, e->dir ? 0x10 : 0); r32(&r, 0xFFFFFFFF);
        rbytes(&r, name, nlen);
        r16(&r, 0x0001); r16(&r, 24); r64(&r, e->size); r64(&r, e->size); r64(&r, e->offset);
        if (!write_all(out, r.b, r.n))
            goto write_failed;
    }
    eocd64 = tell(out);
    cd_size = eocd64 - cd;
    r.n = 0;
    r32(&r, 0x06064b50); r64(&r, 44); r16(&r, 45); r16(&r, 45); r32(&r, 0); r32(&r, 0);
    r64(&r, (uint64_t)p->n); r64(&r, (uint64_t)p->n); r64(&r, cd_size); r64(&r, cd);
    r32(&r, 0x07064b50); r32(&r, 0); r64(&r, eocd64); r32(&r, 1);
    r32(&r, 0x06054b50); r16(&r, 0); r16(&r, 0); r16(&r, 0xFFFF); r16(&r, 0xFFFF);
    r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF); r16(&r, 0);
    if (!write_all(out, r.b, r.n))
        goto write_failed;
    return 1;
write_failed:
    swprintf_s(err, errn, L"Could not write the package (is the drive full?).");
    return 0;
}

int apk_find(const WCHAR *game_dir, const WCHAR *launcher_dir, WCHAR *out, int outn)
{
    const WCHAR *dirs[2] = { game_dir, launcher_dir };
    int i;
    for (i = 0; i < 2; i++) {
        if (!dirs[i] || !dirs[i][0])
            continue;
        swprintf_s(out, outn, L"%s\\Android\\" APK_NAME, dirs[i]);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    out[0] = 0;
    return 0;
}

/* The PC's online settings and account (online_enabled, online_name, online_server,
 * online_token, online_xuid from
 * svr2011.toml - the rest of that file is the PC's) as online.toml, which
 * the game on the phone merges into its own settings (src/online.cpp). */
static void add_online_settings(Pack *p)
{
    WCHAR toml[MAX_PATH], tmp[MAX_PATH];
    FILE *in, *out;
    char line[512];
    int n = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    swprintf_s(toml, MAX_PATH, L"%s\\svr2011.toml", p->root);
    if (_wfopen_s(&in, toml, L"rb") || !in)
        return;
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, MAX_PATH, L"svr2011_online.toml");
    if (_wfopen_s(&out, tmp, L"wb") || !out) {
        fclose(in);
        return;
    }
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '[')
            break;
        if (!strncmp(line, "online_enabled", 14) || !strncmp(line, "online_name", 11) ||
            !strncmp(line, "online_server", 13) || !strncmp(line, "online_token", 12) ||
            !strncmp(line, "online_xuid", 11)) {
            fputs(line, out);
            n++;
        }
    }
    fclose(in);
    fclose(out);
    if (!n || !GetFileAttributesExW(tmp, GetFileExInfoStandard, &fa))
        return;
    if (add(p, L"online.toml", fa.nFileSizeLow, &fa.ftLastWriteTime, 0))
        wcscpy_s(p->e[p->n - 1].src, MAX_PATH, tmp);
}

static const char kHowTo[] =
    "WWE SmackDown vs. Raw 2011 on Android\r\n"
    "=====================================\r\n"
    "\r\n"
    "1. Copy both files to your phone (USB cable: the phone's \"games\" or \"Download\" folder):\r\n"
    "     SvR2011.apk\r\n"
    "     SvR2011-Game.zip\r\n"
    "2. On the phone, open SvR2011.apk to install it (allow installing apps from your file manager).\r\n"
    "3. Start SvR 2011 and allow access to all files when asked: the game lives in\r\n"
    "     games/WWE SmackDown vs. Raw 2011\r\n"
    "   The first start installs SvR2011-Game.zip there; delete the zip afterwards to free the space.\r\n"
    "\r\n"
    "Needs a phone with Vulkan 1.1 and an arm64 processor (a recent Snapdragon is best).\r\n"
    "A controller is recommended.\r\n";

int apk_package(const WCHAR *game_dir, const WCHAR *apk, const WCHAR *out_dir,
                volatile LONG *cancel, apk_progress_fn progress_fn, WCHAR *err, int errn)
{
    Pack p;
    WCHAR zip[MAX_PATH], part[MAX_PATH], dst[MAX_PATH];
    HANDLE out;
    uint8_t *buf;
    int i, ok = 0;

    memset(&p, 0, sizeof p);
    p.root = game_dir;
    p.out_dir = out_dir;
    p.cancel = cancel;
    p.progress = progress_fn;
    p.last = -1;
    crc_init();

    CreateDirectoryW(out_dir, NULL);
    swprintf_s(dst, MAX_PATH, L"%s\\" APK_NAME, out_dir);
    if (!CopyFileW(apk, dst, FALSE)) {
        swprintf_s(err, errn, L"Could not copy the app to %s", dst);
        return 0;
    }
    {
        WCHAR txt[MAX_PATH];
        HANDLE f;
        swprintf_s(txt, MAX_PATH, L"%s\\Install on Android.txt", out_dir);
        f = CreateFileW(txt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            write_all(f, kHowTo, (DWORD)(sizeof kHowTo - 1));
            CloseHandle(f);
        }
    }

    progress_fn(0, L"Listing the game files\x2026");
    if (!walk(&p, L"")) {
        swprintf_s(err, errn, L"Out of memory listing the game files.");
        free(p.e);
        return 0;
    }
    add_online_settings(&p);
    {
        int has_xex = 0;
        for (i = 0; i < p.n; i++)
            if (!_wcsicmp(p.e[i].name, L"default.xex"))
                has_xex = 1;
        if (!has_xex) {
            swprintf_s(err, errn, L"%s has no default.xex: install the game first.", game_dir);
            free(p.e);
            return 0;
        }
    }

    swprintf_s(zip, MAX_PATH, L"%s\\" ZIP_NAME, out_dir);
    swprintf_s(part, MAX_PATH, L"%s.part", zip);
    buf = (uint8_t *)malloc(CHUNK);
    out = CreateFileW(part, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!buf || out == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"Could not create %s", part);
        goto done;
    }
    for (i = 0; i < p.n; i++) {
        if (!p.e[i].dir)
            progress(&p, p.e[i].name);
        if (!write_entry(&p, out, &p.e[i], buf, err, errn))
            goto done;
    }
    if (!write_central(&p, out, err, errn))
        goto done;
    ok = 1;
done:
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    free(buf);
    free(p.e);
    if (ok) {
        if (!MoveFileExW(part, zip, MOVEFILE_REPLACE_EXISTING)) {
            swprintf_s(err, errn, L"Could not rename %s", part);
            ok = 0;
        }
    }
    if (!ok)
        DeleteFileW(part);
    return ok;
}

/* ── Install to phone (USB debugging, Google's adb) ─────────────────────── */

#define ADB_PACKAGE  L"io.github.kaikoclanworth1.svr2011"
#define ADB_REMOTE   L"/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
#define ADB_CMD      32767
#define ADB_OUT      16384

int adb_find(const WCHAR *launcher_dir, WCHAR *out, int outn)
{
    WCHAR local[MAX_PATH];
    DWORD n;
    swprintf_s(out, outn, L"%s\\platform-tools\\adb.exe", launcher_dir);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 1;
    if (SearchPathW(NULL, L"adb.exe", NULL, (DWORD)outn, out, NULL))
        return 1;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n && n < MAX_PATH) {
        swprintf_s(out, outn, L"%s\\Android\\Sdk\\platform-tools\\adb.exe", local);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    out[0] = 0;
    return 0;
}

/* Runs a command line without a window; its output (stdout and stderr) in
   out. The exit code, or -1 if it could not start / was cancelled. */
static int run(WCHAR *cmd, char *out, int outn, volatile LONG *cancel)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    DWORD code = (DWORD)-1, got;
    int n = 0;
    out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    for (;;) {
        char buf[4096];
        if (cancel && *cancel) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (!ReadFile(rd, buf, sizeof buf, &got, NULL) || !got)
            break;
        if (n + (int)got >= outn) {  /* keep the end (errors come last) */
            int keep = outn / 2;
            if (n > keep) {
                memmove(out, out + n - keep, keep);
                n = keep;
            }
            if (n + (int)got >= outn)
                got = (DWORD)(outn - 1 - n);
        }
        memcpy(out + n, buf, got);
        n += got;
        out[n] = 0;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (cancel && *cancel) ? -1 : (int)code;
}

/* The last non-empty line of adb's output, for error messages. */
static void last_line(const char *out, WCHAR *line, int n)
{
    const char *end = out + strlen(out), *start;
    while (end > out && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        end--;
    start = end;
    while (start > out && start[-1] != '\n')
        start--;
    {
        char tmp[512];
        int len = (int)(end - start) < 511 ? (int)(end - start) : 511;
        memcpy(tmp, start, len);
        tmp[len] = 0;
        if (!MultiByteToWideChar(CP_UTF8, 0, tmp, -1, line, n))
            line[0] = 0;
    }
}

/* 'text' for the phone's shell. */
static void shell_quote(const WCHAR *s, WCHAR *out, int n)
{
    int k = 0;
    out[k++] = L'\'';
    for (; *s && k < n - 6; s++) {
        if (*s == L'\'') {
            out[k++] = L'\''; out[k++] = L'\\'; out[k++] = L'\''; out[k++] = L'\'';
        } else {
            out[k++] = *s;
        }
    }
    out[k++] = L'\'';
    out[k] = 0;
}

typedef struct {
    const WCHAR    *adb;
    WCHAR           serial[128];
    WCHAR          *cmd;
    char           *out;
    volatile LONG  *cancel;
} Adb;

/* adb -s <serial> <args>: the exit code (output in a->out). */
static int adb_run(Adb *a, const WCHAR *fmt, ...)
{
    int k;
    va_list ap;
    k = swprintf_s(a->cmd, ADB_CMD, L"\"%s\" -s %s ", a->adb, a->serial);
    va_start(ap, fmt);
    vswprintf_s(a->cmd + k, ADB_CMD - k, fmt, ap);
    va_end(ap);
    return run(a->cmd, a->out, ADB_OUT, a->cancel);
}

/* The one phone ready for USB debugging. */
static int adb_device(Adb *a, WCHAR *err, int errn)
{
    char *line;
    int unauthorized = 0, found = 0;
    swprintf_s(a->cmd, ADB_CMD, L"\"%s\" devices", a->adb);
    if (run(a->cmd, a->out, ADB_OUT, a->cancel) != 0) {
        swprintf_s(err, errn, L"adb did not start (%s).", a->adb);
        return 0;
    }
    for (line = strtok(a->out, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        if (!strcmp(tab + 1, "device") && !found) {
            MultiByteToWideChar(CP_UTF8, 0, line, -1, a->serial, 128);
            found = 1;
        } else if (!strcmp(tab + 1, "unauthorized")) {
            unauthorized = 1;
        }
    }
    if (found)
        return 1;
    swprintf_s(err, errn, unauthorized
        ? L"The phone hasn't allowed this PC yet: unlock it, tap Allow on the \"Allow USB debugging?\" message, then try again."
        : L"No phone found. Turn on USB debugging (steps above), connect the phone with a USB data cable and unlock it.");
    return 0;
}

/* mkdir -p for every folder of the listing (a few per command). */
static int make_folders(Adb *a, const Pack *p, WCHAR *err, int errn)
{
    WCHAR *list = (WCHAR *)malloc(ADB_CMD * sizeof(WCHAR)), path[MAX_PATH + 64], q[MAX_PATH * 2], line[512];
    int i, k = 0, ok = 1;
    if (!list) {
        swprintf_s(err, errn, L"Out of memory.");
        return 0;
    }
    k = swprintf_s(list, ADB_CMD, L"'%s'", ADB_REMOTE);
    for (i = 0; i <= p->n && ok; i++) {
        int last = i == p->n;
        if (!last) {
            if (!p->e[i].dir)
                continue;
            swprintf_s(path, MAX_PATH + 64, ADB_REMOTE L"/%s", p->e[i].name);
            shell_quote(path, q, MAX_PATH * 2);
        }
        if (last || k + (int)wcslen(q) + 2 >= 8000) {
            if (adb_run(a, L"shell mkdir -p %s", list) != 0) {
                last_line(a->out, line, 512);
                swprintf_s(err, errn, *a->cancel ? L"Cancelled." : L"Could not make the game folder on the phone: %s",
                           line);
                ok = 0;
            }
            k = 0;
            list[0] = 0;
        }
        if (!last)
            k += swprintf_s(list + k, ADB_CMD - k, L" %s", q);
    }
    free(list);
    return ok;
}

/* Files pushed together: one folder's, at most 48 or 256 MB at a time. */
typedef struct {
    Adb      *a;
    Pack     *p;
    WCHAR    *files, folder[MAX_PATH];
    int       k, count;
    uint64_t  bytes;
} Batch;

static int batch_push(Batch *b, WCHAR *err, int errn)
{
    WCHAR msg[MAX_PATH + 32], line[512];
    const WCHAR *sep = b->folder[0] ? L"/" : L"";
    if (!b->count)
        return 1;
    swprintf_s(msg, MAX_PATH + 32, L"Copying %s%s\x2026", b->folder[0] ? b->folder : L"the game folder", sep);
    b->p->progress(b->p->total ? (int)(b->p->done * 1000 / b->p->total) : 0, msg);
    if (adb_run(b->a, L"push --sync%s \"" ADB_REMOTE L"/%s%s\"", b->files, b->folder, sep) != 0) {
        last_line(b->a->out, line, 512);
        swprintf_s(err, errn, *b->a->cancel ? L"Cancelled." : L"Copying to the phone failed: %s", line);
        return 0;
    }
    b->p->done += b->bytes;
    b->k = b->count = 0;
    b->bytes = 0;
    b->files[0] = 0;
    return 1;
}

static int batch_add(Batch *b, const Entry *e, const WCHAR *game_dir, WCHAR *err, int errn)
{
    WCHAR folder[MAX_PATH], full[MAX_PATH], *c;
    const WCHAR *slash = wcsrchr(e->name, L'/');
    wcsncpy_s(folder, MAX_PATH, e->name, slash ? (size_t)(slash - e->name) : 0);
    if (b->count && (wcscmp(folder, b->folder) || b->count >= 48 || b->bytes >= (256ull << 20) ||
                     b->k + MAX_PATH * 2 > ADB_CMD - 1024) &&
        !batch_push(b, err, errn))
        return 0;
    if (!b->count)
        wcscpy_s(b->folder, MAX_PATH, folder);
    swprintf_s(full, MAX_PATH, L"%s\\%s", game_dir, e->name);
    if (e->src[0])
        wcscpy_s(full, MAX_PATH, e->src);
    for (c = full; *c; c++)
        if (*c == L'/')
            *c = L'\\';
    b->k += swprintf_s(b->files + b->k, ADB_CMD - b->k, L" \"%s\"", full);
    b->count++;
    b->bytes += e->size;
    return 1;
}

int adb_install(const WCHAR *adb, const WCHAR *game_dir, const WCHAR *apk,
                volatile LONG *cancel, apk_progress_fn progress_fn, WCHAR *err, int errn)
{
    Adb a;
    Pack p;
    WCHAR line[512];
    int i, ok = 0, has_game;
    Entry *xex = NULL;

    memset(&a, 0, sizeof a);
    memset(&p, 0, sizeof p);
    a.adb = adb;
    a.cancel = cancel;
    a.cmd = (WCHAR *)malloc(ADB_CMD * sizeof(WCHAR));
    a.out = (char *)malloc(ADB_OUT);
    if (!a.cmd || !a.out) {
        swprintf_s(err, errn, L"Out of memory.");
        goto done;
    }

    progress_fn(0, L"Looking for the phone\x2026");
    if (!adb_device(&a, err, errn))
        goto done;

    /* The app (over an older one: its data stays). */
    progress_fn(0, L"Installing the app on the phone\x2026");
    adb_run(&a, L"shell am force-stop " ADB_PACKAGE);
    if (adb_run(&a, L"install -r \"%s\"", apk) != 0 || !strstr(a.out, "Success")) {
        last_line(a.out, line, 512);
        if (strstr(a.out, "UPDATE_INCOMPATIBLE"))
            swprintf_s(err, errn, L"The phone has SvR 2011 from a different build. Uninstall it on the phone "
                                  L"(the game folder and saves stay) and try again.");
        else if (*cancel)
            swprintf_s(err, errn, L"Cancelled.");
        else
            swprintf_s(err, errn, L"The app did not install: %s", line);
        goto done;
    }
    /* All-files access (the game folder is in shared storage), so the app
       doesn't have to ask. */
    adb_run(&a, L"shell appops set " ADB_PACKAGE L" MANAGE_EXTERNAL_STORAGE allow");

    /* A phone with the game already: only the game files (its saves,
       created content and settings stay). */
    has_game = adb_run(&a, L"shell ls '" ADB_REMOTE L"/default.xex'") == 0;
    p.root = game_dir;
    p.update = has_game;
    p.cancel = cancel;
    p.progress = progress_fn;
    p.last = -1;
    progress_fn(0, L"Listing the game files\x2026");
    if (!walk(&p, L"")) {
        swprintf_s(err, errn, L"Out of memory listing the game files.");
        goto done;
    }
    add_online_settings(&p);
    for (i = 0; i < p.n; i++)
        if (!_wcsicmp(p.e[i].name, L"default.xex"))
            xex = &p.e[i];
    if (!xex) {
        swprintf_s(err, errn, L"%s has no default.xex: install the game first.", game_dir);
        goto done;
    }
    if (!has_game && adb_run(&a, L"shell df -k /storage/emulated/0") == 0) {
        /* Filesystem 1K-blocks Used Available Use% Mounted on */
        char *l2 = strchr(a.out, '\n');
        unsigned long long blocks, used, avail;
        if (l2 && sscanf_s(l2 + 1, "%*s %llu %llu %llu", &blocks, &used, &avail) == 3 &&
            avail * 1024 < p.total + (256ull << 20)) {
            swprintf_s(err, errn, L"The phone has %.1f GB free; the game needs %.1f GB.",
                       avail / 1048576.0, p.total / 1073741824.0);
            goto done;
        }
    }

    /* Folders (empty ones too), then the files in batches per folder, only
       those that differ (--sync), default.xex last: the app treats the game
       as installed once it's there. */
    if (!make_folders(&a, &p, err, errn))
        goto done;
    {
        Batch b;
        memset(&b, 0, sizeof b);
        b.a = &a;
        b.p = &p;
        b.files = (WCHAR *)malloc(ADB_CMD * sizeof(WCHAR));
        if (!b.files) {
            swprintf_s(err, errn, L"Out of memory.");
            goto done;
        }
        b.files[0] = 0;
        for (i = 0; i < p.n; i++) {
            if (p.e[i].dir || &p.e[i] == xex)
                continue;
            if (!batch_add(&b, &p.e[i], game_dir, err, errn)) {
                free(b.files);
                goto done;
            }
        }
        if (!batch_push(&b, err, errn) || !batch_add(&b, xex, game_dir, err, errn) ||
            !batch_push(&b, err, errn)) {
            free(b.files);
            goto done;
        }
        free(b.files);
    }

    progress_fn(1000, L"Starting SvR 2011 on the phone\x2026");
    adb_run(&a, L"shell am start -n " ADB_PACKAGE L"/.InstallActivity");
    swprintf_s(err, errn, has_game ? L"updated" : L"installed");
    ok = 1;
done:
    free(p.e);
    free(a.cmd);
    free(a.out);
    return ok;
}
