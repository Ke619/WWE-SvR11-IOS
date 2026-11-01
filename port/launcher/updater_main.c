/* SvR2011 Updater.exe - installs a release of the PC port over an install, for the launcher.
 *
 * The launcher itself never replaces program files: it asks this small program to (its Update / Finish the
 * update buttons start it and close). Antivirus heuristics judge a program that downloads, swaps running
 * executables and restarts itself as a dropper; keeping that in its own small program leaves the launcher
 * an ordinary UI.
 *
 *   SvR2011 Updater.exe --apply <game folder> <launcher folder> [--version <v>]
 *       a small window: download the newest release (or <v>), install it, start the launcher again
 *   SvR2011 Updater.exe --update [<game folder>] [--from <version>]      (tests, no window)
 *   SvR2011 Updater.exe --update-zip <release zip> [<game folder>]        (tests, no window)
 *
 * Install = all or nothing: every program file is copied beside its old one as <name>.new; only when all
 * are there are the old ones moved to <name>.old and the new ones moved in. A file that can't be replaced
 * (the game or the Mod Maker running from the folder, an antivirus holding it) leaves the old set whole.
 * This program's own file can't be replaced while it runs: its new copy stays as "SvR2011 Updater.exe.new"
 * and the launcher puts it in place at its next start. */
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "updater.h"
#include "unzip.h"
#include "release_check.h"

#define GAME_EXE     L"svr2011.exe"
#define LAUNCHER_EXE L"SvR2011 Launcher.exe"
#define UPDATER_EXE  L"SvR2011 Updater.exe"
#define TITLE        L"SvR 2011 PC port - Updater"

static WCHAR s_game[MAX_PATH], s_launcher[MAX_PATH], s_self[MAX_PATH];
static HWND s_wnd, s_text, s_bar;
static volatile LONG s_cancel;
static int s_console;
static DWORD s_parent;   /* (console modes: the launcher that started us and waits for us doesn't count) */

/* ── small helpers ─────────────────────────────────────────────────────── */

static int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int join(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    size_t la = wcslen(a);
    if (la + 1 + wcslen(b) >= MAX_PATH) {
        out[0] = 0;
        return 0;
    }
    swprintf_s(out, MAX_PATH, (la && a[la - 1] == L'\\') ? L"%s%s" : L"%s\\%s", a, b);
    return 1;
}

static int mkdirs(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH], *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + (tmp[0] && tmp[1] == L':' ? 3 : 2); *p; p++)
        if (*p == L'\\' || *p == L'/') {
            WCHAR c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int same_dir(const WCHAR *a, const WCHAR *b)
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

/* Deletes a folder and everything in it. */
static void remove_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!join(pat, dir, L"*") || (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(dir);
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !join(p, dir, fd.cFileName))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            remove_tree(p);
        else {
            SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

static int copy_tree(const WCHAR *from, const WCHAR *to)
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
static int copy_folders(const WCHAR *from, const WCHAR *to)
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

static void error_text(DWORD e, WCHAR *out, size_t n)
{
    size_t k;
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, out, (DWORD)n, NULL))
        swprintf_s(out, n, L"error %lu", e);
    k = wcslen(out);
    while (k && (out[k - 1] == L'\r' || out[k - 1] == L'\n' || out[k - 1] == L' ' || out[k - 1] == L'.'))
        out[--k] = 0;
}

static int same_file_bytes(const WCHAR *a, const WCHAR *b)
{
    static char ba[1 << 16], bb[1 << 16];
    FILE *fa = NULL, *fb = NULL;
    size_t na, nb;
    int same = 0;
    if (_wfopen_s(&fa, a, L"rb") || !fa || _wfopen_s(&fb, b, L"rb") || !fb)
        goto done;
    for (;;) {
        na = fread(ba, 1, sizeof ba, fa);
        nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na))
            break;
        if (!na) {
            same = 1;
            break;
        }
    }
done:
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* Where the release zip goes: Tools\.downloads beside the launcher (%TEMP% when that can't be written). */
static void download_dir(WCHAR *out)
{
    if (join(out, s_launcher, L"Tools\\.downloads") && mkdirs(out))
        return;
    GetTempPathW(MAX_PATH, out);
}

static void cprint(const WCHAR *fmt, ...);

static void say(const WCHAR *fmt, ...)
{
    WCHAR buf[700];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 700, fmt, ap);
    va_end(ap);
    if (s_console)
        cprint(L"%s\n", buf);
    else if (s_text)
        SetWindowTextW(s_text, buf);
}

/* ── the port's programs running from a folder ────────────────────────── */

/* "the Mod Maker and the game" running from `dir` into `out`; 0 when none. This program doesn't count. */
static int programs_running_in(const WCHAR *dir, WCHAR *out, size_t n)
{
    static const struct { const WCHAR *exe, *say; } k_progs[] = {
        { GAME_EXE, L"the game" }, { L"SvR2011 Mod Maker.exe", L"the Mod Maker" }, { LAUNCHER_EXE, L"the launcher" } };
    PROCESSENTRY32W pe;
    HANDLE snap;
    int found = 0, said[3] = { 0, 0, 0 };
    size_t i;
    out[0] = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe))
        do {
            for (i = 0; i < 3; i++) {
                WCHAR path[MAX_PATH], *slash;
                DWORD len = MAX_PATH;
                HANDLE p;
                if (said[i] || _wcsicmp(pe.szExeFile, k_progs[i].exe) || pe.th32ProcessID == GetCurrentProcessId() ||
                    (s_parent && pe.th32ProcessID == s_parent))
                    continue;
                if (!(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)))
                    continue;
                if (QueryFullProcessImageNameW(p, 0, path, &len) && (slash = wcsrchr(path, L'\\'))) {
                    *slash = 0;
                    if (same_dir(path, dir)) {
                        said[i] = 1;
                        swprintf_s(out + wcslen(out), n - wcslen(out), L"%s%s", found ? L" and " : L"",
                                   k_progs[i].say);
                        found++;
                    }
                }
                CloseHandle(p);
            }
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

/* 1 when nothing of the port runs from the folders; waits a few seconds for the launcher that started us. */
static int programs_closed(WCHAR *err, size_t errn)
{
    WCHAR who[160], also[160];
    int n = 0, tries;
    for (tries = 0; tries < 40; tries++) {   /* (10 s: the launcher closes after starting this) */
        n = s_game[0] ? programs_running_in(s_game, who, 160) : 0;
        also[0] = 0;
        if (!same_dir(s_game, s_launcher) && programs_running_in(s_launcher, also, 160))
            swprintf_s(who + wcslen(who), 160 - wcslen(who), L"%s%s", n++ ? L" and " : L"", also);
        if (!n)
            return 1;
        Sleep(250);
    }
    swprintf_s(err, errn, L"Close %s first, then update again - %s running from the port's folder.", who,
               n > 1 ? L"they are" : L"it is");
    return 0;
}

/* ── installing ───────────────────────────────────────────────────────── */

#define FILES_MAX 256

/* The release's top-level files into `to`, all or none (see the top), and its folders. */
static int copy_into(const WCHAR *from, const WCHAR *to, WCHAR *err, size_t errn)
{
    static const WCHAR *keep[] = { L"launcher.ini", L"svr2011.toml" };
    static WCHAR names[FILES_MAX][MAX_PATH];
    WCHAR pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], nw[MAX_PATH + 8], old[MAX_PATH + 8], why[256];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t i;
    int n = 0, k, moved = 0, placed = 0, ok = 1;
    if (!join(pat, from, L"*") || (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        int skip = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        for (i = 0; i < sizeof keep / sizeof *keep; i++)
            if (!_wcsicmp(fd.cFileName, keep[i]))
                skip = 1;
        if (!skip && n < FILES_MAX)
            wcscpy_s(names[n++], MAX_PATH, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* this program's own file: beside it as .new, put in place by the launcher's next start */
    for (k = 0; k < n; k++)
        if (!_wcsicmp(names[k], UPDATER_EXE) && join(src, from, names[k]) && join(dst, to, names[k]) &&
            (same_dir(to, s_launcher) || !_wcsicmp(dst, s_self))) {
            swprintf_s(nw, MAX_PATH + 8, L"%s.new", dst);
            CopyFileW(src, nw, FALSE);
            names[k][0] = 0;
        }
    for (k = 0; k < n && ok; k++) {   /* 1: every new file beside its old one */
        if (!names[k][0] || !join(src, from, names[k]) || !join(dst, to, names[k]))
            continue;
        swprintf_s(nw, MAX_PATH + 8, L"%s.new", dst);
        if (!CopyFileW(src, nw, FALSE)) {
            error_text(GetLastError(), why, 256);
            swprintf_s(err, errn, L"The update couldn't write %s (%s). Nothing was changed.", nw, why);
            ok = 0;
        }
    }
    for (k = 0; k < n && ok; k++, moved++) {   /* 2: the old ones out of the way ... */
        if (!names[k][0] || !join(dst, to, names[k]) || !file_exists(dst))
            continue;
        swprintf_s(old, MAX_PATH + 8, L"%s.old", dst);
        DeleteFileW(old);
        if (!MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING)) {
            error_text(GetLastError(), why, 256);
            swprintf_s(err, errn, L"The update didn't finish: %s couldn't be replaced (%s). Is the Mod Maker or the "
                                  L"game running from that folder? Close it, then update again - nothing was changed.",
                       dst, why);
            ok = 0;
            break;
        }
    }
    for (k = 0; k < n && ok; k++, placed++) {   /* ... and the new ones in */
        if (!names[k][0] || !join(dst, to, names[k]))
            continue;
        swprintf_s(nw, MAX_PATH + 8, L"%s.new", dst);
        if (!MoveFileExW(nw, dst, 0)) {
            error_text(GetLastError(), why, 256);
            swprintf_s(err, errn, L"The update didn't finish: %s couldn't be put in place (%s). Nothing was changed.",
                       dst, why);
            ok = 0;
            break;
        }
    }
    if (!ok) {   /* (back as it was) */
        for (k = 0; k < n; k++) {
            if (!names[k][0] || !join(dst, to, names[k]))
                continue;
            swprintf_s(nw, MAX_PATH + 8, L"%s.new", dst);
            swprintf_s(old, MAX_PATH + 8, L"%s.old", dst);
            if (k < placed)
                DeleteFileW(dst);
            if (k < moved && file_exists(old))
                MoveFileExW(old, dst, MOVEFILE_REPLACE_EXISTING);
            DeleteFileW(nw);
        }
        return 0;
    }
    for (k = 0; k < n; k++) {   /* 3: the old ones gone, each file checked */
        if (!names[k][0] || !join(src, from, names[k]) || !join(dst, to, names[k]))
            continue;
        swprintf_s(old, MAX_PATH + 8, L"%s.old", dst);
        if (file_exists(old))
            DeleteFileW(old);
        if (ok && !same_file_bytes(src, dst)) {
            swprintf_s(err, errn, L"The update didn't finish: %s is not the new version.", dst);
            ok = 0;
        }
    }
    if (ok)
        copy_folders(from, to);
    return ok;
}

/* ── the install checked against the release's file list ─────────────── */

typedef struct Recopy { const WCHAR *from, *to; int failed; } Recopy;

/* A bad file copied in again from the unpacked release (this program's own file: its .new copy). */
static void recopy_bad(const WCHAR *rel, void *ctx)
{
    Recopy *r = (Recopy *)ctx;
    WCHAR src[MAX_PATH], dst[MAX_PATH], dir[MAX_PATH], *slash;
    if (!_wcsicmp(rel, UPDATER_EXE))
        return;
    if (!join(src, r->from, rel) || !join(dst, r->to, rel)) {
        r->failed = 1;
        return;
    }
    wcscpy_s(dir, MAX_PATH, dst);
    if ((slash = wcsrchr(dir, L'\\')))
        *slash = 0, mkdirs(dir);
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src, dst, FALSE))
        r->failed = 1;
}

static void count_bad(const WCHAR *rel, void *ctx)
{
    if (_wcsicmp(rel, UPDATER_EXE))
        ++*(int *)ctx;
}

/* Every file of the release in `to` with the right bytes: bad ones copied again once, then checked again.
 * 1 when all are right (or the release has no list). */
static int verify_install(const WCHAR *from, const WCHAR *to, WCHAR *err, size_t errn)
{
    WCHAR list[MAX_PATH];
    ReleaseCheck rc;
    Recopy r = { from, to, 0 };
    int still = 0;
    if (!join(list, from, L"release_files.txt") || !file_exists(list))
        return 1;   /* (releases before 2.1.4 have no list) */
    say(L"Checking the installed files\x2026");
    if (!release_check(to, list, 1, &rc, recopy_bad, &r) || !rc.bad)
        return 1;
    release_check(to, list, 1, &rc, count_bad, &still);
    if (!still)
        return 1;
    swprintf_s(err, errn, L"The update didn't finish: %d file(s) of version %s are missing or damaged in %s (first: "
                          L"%s). Free some disk space or allow the folder in your antivirus, then update again.",
               still, rc.version, to, rc.first_bad);
    return 0;
}

/* A release zip's files over the install (the game folder, and the launcher's folder if it is another one). */
static int install_zip(const WCHAR *zip, WCHAR *err, size_t errn)
{
    WCHAR tmp[MAX_PATH], work[MAX_PATH], files[MAX_PATH], probe[MAX_PATH];
    int ok = 0;
    download_dir(tmp);
    if (!join(work, tmp, L"svr2011-update") || !join(files, work, L"files"))
        return 0;
    remove_tree(work);
    CreateDirectoryW(work, NULL);
    CreateDirectoryW(files, NULL);
    say(L"Unpacking the update\x2026");
    if (!unzip_file(zip, files)) {
        swprintf_s(err, errn, L"Could not unpack the update.");
        goto done;
    }
    if (!join(probe, files, GAME_EXE) || !file_exists(probe)) {
        swprintf_s(err, errn, L"The update has no %s; nothing was changed.", GAME_EXE);
        goto done;
    }
    if (!programs_closed(err, errn))
        goto done;
    say(L"Installing the update\x2026");
    ok = 1;
    if (s_game[0] && dir_exists(s_game))
        ok = copy_into(files, s_game, err, errn) && verify_install(files, s_game, err, errn);
    if (ok && !same_dir(s_game, s_launcher))
        ok = copy_into(files, s_launcher, err, errn) && verify_install(files, s_launcher, err, errn);
done:
    remove_tree(work);
    return ok;
}

static void on_progress(uint64_t done, uint64_t total, void *ctx)
{
    (void)ctx;
    if (s_bar && total)
        PostMessageW(s_bar, PBM_SETPOS, (WPARAM)(done * 100 / total), 0);
}

/* Downloads `info`'s zip into Tools\.downloads (older ones removed) and installs it. */
static int install_release(const UpdateInfo *info, WCHAR *err, size_t errn)
{
    WCHAR zip[MAX_PATH], dir[MAX_PATH], name[80], pat[MAX_PATH], other[MAX_PATH], part[MAX_PATH + 8];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    download_dir(dir);
    swprintf_s(name, 80, L"SvR2011-PC-v%s.zip", info->version);
    join(zip, dir, name);
    if (!file_exists(zip)) {
        say(L"Downloading version %s\x2026", info->version);
        swprintf_s(part, MAX_PATH + 8, L"%s.part", zip);
        if (!update_download_cb(info->url, part, on_progress, NULL, &s_cancel, err, errn))
            return 0;
        if (!MoveFileExW(part, zip, MOVEFILE_REPLACE_EXISTING)) {
            swprintf_s(err, errn, L"Could not save the update in %s.", zip);
            return 0;
        }
    }
    if (join(pat, dir, L"SvR2011-PC-v*.zip") && (h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {   /* (older releases' zips) */
            if (join(other, dir, fd.cFileName) && _wcsicmp(other, zip))
                DeleteFileW(other);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (s_bar)
        PostMessageW(s_bar, PBM_SETPOS, 100, 0);
    return install_zip(zip, err, errn);
}

/* The launcher again (shown normally, whatever started us); `updated` = after a good update. */
static void start_launcher(int updated)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 32];
    if (!join(exe, s_launcher, LAUNCHER_EXE) || !file_exists(exe))
        return;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    swprintf_s(cmd, MAX_PATH + 32, updated == 2 ? L"\"%s\" --repair-failed" : updated ? L"\"%s\" --updated" : L"\"%s\"",
               exe);
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_launcher, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ── repair: only the files that are missing or damaged ───────────────── *
 * The install's own release_files.txt names every file; the bad ones are read out of the release zip - the
 * one kept in Tools\.downloads, else straight from GitHub with HTTP Range requests (a few MB instead of the
 * whole zip). */

typedef struct ZipSrc { HANDLE file; const WCHAR *url; uint64_t size; } ZipSrc;

static int src_read(ZipSrc *z, uint64_t off, uint32_t len, void *buf, WCHAR *err, size_t errn)
{
    if (z->file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li;
        DWORD got = 0;
        li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(z->file, li, NULL, FILE_BEGIN) || !ReadFile(z->file, buf, len, &got, NULL) || got != len) {
            swprintf_s(err, errn, L"Could not read the saved release zip.");
            return 0;
        }
        return 1;
    }
    return update_read_range(z->url, (int64_t)off, len, buf, NULL, err, errn);
}

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

#define REPAIR_MAX 512
typedef struct BadList { int n; WCHAR rel[REPAIR_MAX][MAX_PATH]; } BadList;

static void collect_bad(const WCHAR *rel, void *ctx)
{
    BadList *b = (BadList *)ctx;
    if (b->n < REPAIR_MAX && _wcsicmp(rel, UPDATER_EXE))
        wcscpy_s(b->rel[b->n++], MAX_PATH, rel);
}

/* One entry of the zip (its central directory record at `e`) written to dir\rel. */
static int extract_entry(ZipSrc *z, const unsigned char *e, const WCHAR *dir, const WCHAR *rel, WCHAR *err, size_t errn)
{
    uint16_t method = rd16(e + 10);
    uint32_t csize = rd32(e + 20), usize = rd32(e + 24), lho = rd32(e + 42);
    unsigned char lh[30], *in = NULL, *out = NULL;
    WCHAR dst[MAX_PATH], folder[MAX_PATH], tmp[MAX_PATH + 8], why[256], *slash;
    HANDLE f;
    DWORD wrote;
    int ok = 0;
    if (!src_read(z, lho, 30, lh, err, errn) || rd32(lh) != 0x04034b50)
        return 0;
    if (!(in = (unsigned char *)malloc(csize ? csize : 1)) || !(out = (unsigned char *)malloc(usize ? usize : 1)) ||
        !src_read(z, (uint64_t)lho + 30 + rd16(lh + 26) + rd16(lh + 28), csize, in, err, errn))
        goto done;
    if (method == 0 && csize == usize)
        memcpy(out, in, usize);
    else if (method != 8 || !unzip_inflate(in, csize, out, usize)) {
        swprintf_s(err, errn, L"Could not unpack %s from the release.", rel);
        goto done;
    }
    if (!join(dst, dir, rel))
        goto done;
    wcscpy_s(folder, MAX_PATH, dst);
    if ((slash = wcsrchr(folder, L'\\')))
        *slash = 0, mkdirs(folder);
    swprintf_s(tmp, MAX_PATH + 8, L"%s.part", dst);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, out, usize, &wrote, NULL) || wrote != usize) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(tmp);
        swprintf_s(err, errn, L"Could not write %s (disk full, or an antivirus blocking the folder?).", dst);
        goto done;
    }
    CloseHandle(f);
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
        error_text(GetLastError(), why, 256);
        DeleteFileW(tmp);
        swprintf_s(err, errn, L"Could not replace %s (%s). Close the game and the Mod Maker, then try again.", dst, why);
        goto done;
    }
    ok = 1;
done:
    free(in);
    free(out);
    return ok;
}

/* The bad files of `dir` from the release zip `z` into `dir`. */
static int repair_from(ZipSrc *z, const WCHAR *dir, BadList *bad, WCHAR *err, size_t errn)
{
    unsigned char *tail = NULL, *cd = NULL;
    uint32_t tail_n, cd_size, cd_off, i, entries, p;
    int ok = 0, k;
    if (z->file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li;
        if (!GetFileSizeEx(z->file, &li))
            return 0;
        z->size = (uint64_t)li.QuadPart;
        tail_n = (uint32_t)(z->size < 65557 ? z->size : 65557);
    } else   /* (the size GitHub gave for the asset: its storage doesn't take "the last N bytes" ranges) */
        tail_n = (uint32_t)(z->size && z->size < 65557 ? z->size : 65557);
    if (!z->size) {
        swprintf_s(err, errn, L"The release zip's size isn't known.");
        return 0;
    }
    if (!(tail = (unsigned char *)malloc(tail_n)))
        return 0;
    if (!src_read(z, z->size - tail_n, tail_n, tail, err, errn))
        goto done;
    for (p = tail_n - 22;; p--) {   /* (the end of central directory record) */
        if (rd32(tail + p) == 0x06054b50)
            break;
        if (!p) {
            swprintf_s(err, errn, L"The release zip has no file directory.");
            goto done;
        }
    }
    entries = rd16(tail + p + 10);
    cd_size = rd32(tail + p + 12);
    cd_off = rd32(tail + p + 16);
    if (!(cd = (unsigned char *)malloc(cd_size)) || !src_read(z, cd_off, cd_size, cd, err, errn))
        goto done;
    for (k = 0; k < bad->n; k++) {
        int found = 0;
        for (i = 0, p = 0; i < entries && p + 46 <= cd_size && !found; i++) {
            uint16_t nlen = rd16(cd + p + 28), xlen = rd16(cd + p + 30), clen = rd16(cd + p + 32);
            WCHAR name[MAX_PATH], *c;
            int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)cd + p + 46, nlen, name, MAX_PATH - 1);
            name[wn > 0 ? wn : 0] = 0;
            for (c = name; *c; c++)
                if (*c == L'/') *c = L'\\';
            if (!_wcsicmp(name, bad->rel[k])) {
                found = 1;
                say(L"Getting %s (%d of %d)\x2026", bad->rel[k], k + 1, bad->n);
                if (!extract_entry(z, cd + p, dir, bad->rel[k], err, errn))
                    goto done;
            }
            p += 46 + nlen + xlen + clen;
        }
        if (!found) {
            swprintf_s(err, errn, L"%s isn't in the release zip.", bad->rel[k]);
            goto done;
        }
    }
    ok = 1;
done:
    free(tail);
    free(cd);
    return ok;
}

/* Repairs the install (`s_game`, and `s_launcher` if it is another folder): 1 when every listed file is right
 * after it; `*count` = how many were got. Without a file list (or one of another version than `version`) the
 * whole release again instead. */
static int repair_install(const WCHAR *version, int *count, WCHAR *err, size_t errn)
{
    static BadList bad;
    const WCHAR *dirs[2] = { s_game, s_launcher };
    WCHAR zip[MAX_PATH], ddir[MAX_PATH], name[80];
    UpdateInfo info;
    ReleaseCheck rc;
    int d, have_info = 0;
    *count = 0;
    download_dir(ddir);
    for (d = 0; d < 2; d++) {
        ZipSrc z = { INVALID_HANDLE_VALUE, NULL, 0 };
        int ok;
        if (d && same_dir(s_game, s_launcher))
            break;
        bad.n = 0;
        say(L"Checking the installed files\x2026");
        if (!release_check(dirs[d], NULL, 1, &rc, collect_bad, &bad) || (version && wcscmp(rc.version, version))) {
            UpdateInfo full;   /* (no list: the whole release again) */
            if (!(version ? update_check_version(version, &full, err, errn) : update_check(&full, err, errn)))
                return 0;
            return install_release(&full, err, errn);
        }
        if (!bad.n)
            continue;
        swprintf_s(name, 80, L"SvR2011-PC-v%s.zip", rc.version);
        join(zip, ddir, name);
        if (file_exists(zip))
            z.file = CreateFileW(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (z.file == INVALID_HANDLE_VALUE) {
            if (!have_info && !update_check_version(rc.version, &info, err, errn))
                return 0;
            have_info = 1;
            z.url = info.url;
            z.size = info.size;
        }
        say(L"Repairing the install: %d file(s) of version %s\x2026", bad.n, rc.version);
        ok = repair_from(&z, dirs[d], &bad, err, errn);
        if (z.file != INVALID_HANDLE_VALUE)
            CloseHandle(z.file);
        if (!ok)
            return 0;
        *count += bad.n;
        bad.n = 0;
        release_check(dirs[d], NULL, 1, &rc, collect_bad, &bad);
        if (bad.n) {
            swprintf_s(err, errn, L"%d file(s) are still missing or damaged after the repair (first: %s).", bad.n,
                       bad.rel[0]);
            return 0;
        }
    }
    return 1;
}

/* ── the window (--apply) ─────────────────────────────────────────────── */

static WCHAR s_want[32];   /* --version: that release (Finish the update), else the newest */
static int s_repair;        /* --repair: only the missing / damaged files */

static int s_repaired;

static DWORD WINAPI apply_thread(LPVOID arg)
{
    static WCHAR err[700];
    UpdateInfo info;
    int ok;
    (void)arg;
    err[0] = 0;
    if (s_repair) {
        ok = repair_install(s_want[0] ? s_want : NULL, &s_repaired, err, 700);
    } else {
        say(L"Looking for the update\x2026");
        ok = s_want[0] ? update_check_version(s_want, &info, err, 700) : update_check(&info, err, 700);
        if (ok)
            ok = install_release(&info, err, 700);
    }
    PostMessageW(s_wnd, WM_APP + 1, (WPARAM)ok, (LPARAM)err);
    return 0;
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_APP + 1:
        if (wp && s_repair) {   /* (a quiet success: no box, the launcher just comes back) */
            start_launcher(1);
        } else if (wp) {
            MessageBoxW(w, L"The PC port is updated. The launcher starts again now.", TITLE, MB_OK | MB_ICONINFORMATION);
            start_launcher(1);
        } else {
            WCHAR msg[800];
            swprintf_s(msg, 800, L"%s", ((const WCHAR *)lp)[0] ? (const WCHAR *)lp
                                       : s_repair ? L"The repair failed." : L"The update failed.");
            MessageBoxW(w, msg, TITLE, MB_OK | MB_ICONWARNING);
            start_launcher(s_repair ? 2 : 0);
        }
        DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&s_cancel, 1);   /* (the download stops; the install itself is short) */
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static int run_window(HINSTANCE inst)
{
    WNDCLASSW wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    InitCommonControlsEx(&icc);
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SvR2011Updater";
    RegisterClassW(&wc);
    s_wnd = CreateWindowW(wc.lpszClassName, TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                          CW_USEDEFAULT, CW_USEDEFAULT, 460, 150, NULL, NULL, inst, NULL);
    s_text = CreateWindowW(L"Static", s_repair ? L"Checking the installed files\x2026" : L"Looking for the update\x2026", WS_CHILD | WS_VISIBLE, 16, 16, 420, 40,
                           s_wnd, NULL, inst, NULL);
    s_bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, 16, 64, 420, 18, s_wnd, NULL, inst, NULL);
    SendMessageW(s_text, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(s_wnd, SW_SHOWNORMAL);
    UpdateWindow(s_wnd);
    CloseHandle(CreateThread(NULL, 0, apply_thread, NULL, 0, NULL));
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ── start ────────────────────────────────────────────────────────────── */

static HANDLE s_out = INVALID_HANDLE_VALUE;

/* Console modes: the parent's pipe or file (tests redirect it), else the parent's console. */
static void console_setup(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE), snap;
    PROCESSENTRY32W pe;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);   /* (our parent) */
    pe.dwSize = sizeof pe;
    if (snap != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snap, &pe))
            do {
                if (pe.th32ProcessID == GetCurrentProcessId())
                    s_parent = pe.th32ParentProcessID;
            } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
    }
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
        s_out = h;
    else if (AttachConsole(ATTACH_PARENT_PROCESS))
        s_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    s_console = 1;
}

/* A line to the console (UTF-8). */
static void cprint(const WCHAR *fmt, ...)
{
    WCHAR buf[1200];
    char u8[3600];
    DWORD done;
    int n;
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1200, fmt, ap);
    va_end(ap);
    if (s_out == INVALID_HANDLE_VALUE || !s_out)
        return;
    n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof u8, NULL, NULL);
    if (n > 1)
        WriteFile(s_out, u8, (DWORD)(n - 1), &done, NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc = 0, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc), *slash;
    (void)prev; (void)cmdline; (void)show;
    GetModuleFileNameW(NULL, s_self, MAX_PATH);
    wcscpy_s(s_launcher, MAX_PATH, s_self);
    if ((slash = wcsrchr(s_launcher, L'\\')))
        *slash = 0;
    wcscpy_s(s_game, MAX_PATH, s_launcher);
    if (!argv || argc < 2) {
        MessageBoxW(NULL, L"Start the SvR 2011 launcher and use its Update button.", TITLE, MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (!wcscmp(argv[1], L"--repair-cli") && argc >= 3) {   /* <game folder> [<version>] (tests, no window) */
        WCHAR err[700] = L"";
        int n = 0;
        console_setup();
        wcscpy_s(s_game, MAX_PATH, argv[2]);
        if (!repair_install(argc >= 4 ? argv[3] : NULL, &n, err, 700)) { cprint(L"repair failed: %s\n", err); return 1; }
        cprint(L"repaired %d file(s) in %s\n", n, s_game);
        return 0;
    }
    if (!wcscmp(argv[1], L"--repair") && argc >= 4)
        s_repair = 1;
    if ((!wcscmp(argv[1], L"--apply") || s_repair) && argc >= 4) {
        wcscpy_s(s_game, MAX_PATH, argv[2]);
        wcscpy_s(s_launcher, MAX_PATH, argv[3]);
        for (i = 4; i + 1 < argc; i++)
            if (!wcscmp(argv[i], L"--version")) wcscpy_s(s_want, 32, argv[++i]);
        return run_window(inst);
    }
    if (!wcscmp(argv[1], L"--check")) {   /* [<version>]: the newest release (or that one) - nothing downloaded */
        WCHAR err[700] = L"";
        UpdateInfo info;
        console_setup();
        if (!(argc >= 3 ? update_check_version(argv[2], &info, err, 700) : update_check(&info, err, 700))) {
            cprint(L"check failed: %s\n", err);
            return 1;
        }
        cprint(L"release %s, %llu bytes: %s\n", info.version, (unsigned long long)info.size, info.url);
        return 0;
    }
    if (!wcscmp(argv[1], L"--fetch") && argc >= 4) {   /* <url> <file>: a download as an update's (tests) */
        WCHAR err[700] = L"";
        console_setup();
        if (!update_download_cb(argv[2], argv[3], NULL, NULL, NULL, err, 700)) {
            cprint(L"download failed: %s\n", err);
            return 1;
        }
        cprint(L"downloaded %s\n", argv[3]);
        return 0;
    }
    if (!wcscmp(argv[1], L"--update")) {   /* [<game folder>] [--from <version>] */
        WCHAR err[700] = L"";
        const WCHAR *from = PORT_VERSION;
        UpdateInfo info;
        console_setup();
        for (i = 2; i < argc; i++) {
            if (!wcscmp(argv[i], L"--from") && i + 1 < argc) from = argv[++i];
            else wcscpy_s(s_game, MAX_PATH, argv[i]);
        }
        if (!update_check(&info, err, 700)) { cprint(L"check failed: %s\n", err); return 1; }
        cprint(L"newest release %s (%s), this is %s\n", info.version, info.url, from);
        if (!version_newer(info.version, from)) { cprint(L"up to date\n"); return 0; }
        if (!install_release(&info, err, 700)) { cprint(L"update failed: %s\n", err); return 1; }
        cprint(L"updated %s to %s\n", s_game, info.version);
        return 0;
    }
    if (!wcscmp(argv[1], L"--update-zip") && argc >= 3) {   /* <release zip> [<game folder>] */
        WCHAR err[700] = L"";
        console_setup();
        if (argc >= 4)
            wcscpy_s(s_game, MAX_PATH, argv[3]);
        if (!install_zip(argv[2], err, 700)) { cprint(L"update failed: %s\n", err); return 1; }
        cprint(L"updated %s from %s\n", s_game, argv[2]);
        return 0;
    }
    MessageBoxW(NULL, L"Start the SvR 2011 launcher and use its Update button.", TITLE, MB_OK | MB_ICONINFORMATION);
    return 1;
}
