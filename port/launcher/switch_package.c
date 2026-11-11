/* WWE SmackDown vs. Raw 2011 launcher - the Nintendo Switch export (see switch_package.h). */

#include "switch_package.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define FAT32_MAX 0xFFFFFFFFull

typedef struct {
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    uint64_t size;
    FILETIME t;
    int is_save;
} Item;

typedef struct {
    const WCHAR *out;
    Item *items;
    int n, cap;
    uint64_t to_copy, done;
    int big;                  /* a file a FAT32 card can't hold */
    WCHAR big_name[MAX_PATH];
    volatile LONG *cancel;
    switch_progress_fn progress;
    WCHAR backup[MAX_PATH];   /* where replaced saves go */
} Job;

static int is_dir(const WCHAR *p)
{
    const DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Folders on the way to a file. */
static void make_parents(const WCHAR *path)
{
    WCHAR b[MAX_PATH];
    WCHAR *p;
    wcscpy_s(b, MAX_PATH, path);
    for (p = wcschr(b + 3, L'\\'); p; p = wcschr(p + 1, L'\\')) {
        *p = 0;
        CreateDirectoryW(b, NULL);
        *p = L'\\';
    }
}

/* The same file? FAT keeps times to 2 seconds. */
static int same_file(const WCHAR *dst, uint64_t size, const FILETIME *t)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    ULARGE_INTEGER x, y;
    uint64_t diff;
    if (!GetFileAttributesExW(dst, GetFileExInfoStandard, &a))
        return 0;
    if ((((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) != size)
        return 0;
    x.LowPart = a.ftLastWriteTime.dwLowDateTime, x.HighPart = a.ftLastWriteTime.dwHighDateTime;
    y.LowPart = t->dwLowDateTime, y.HighPart = t->dwHighDateTime;
    diff = x.QuadPart > y.QuadPart ? x.QuadPart - y.QuadPart : y.QuadPart - x.QuadPart;
    return diff <= 30000000ull;   /* 3 seconds */
}

static int add_item(Job *j, const WCHAR *src, const WCHAR *dst, uint64_t size, const FILETIME *t, int is_save)
{
    Item *it;
    if (same_file(dst, size, t))
        return 1;
    if (j->n == j->cap) {
        const int cap = j->cap ? j->cap * 2 : 1024;
        Item *ni = (Item *)realloc(j->items, (size_t)cap * sizeof(Item));
        if (!ni)
            return 0;
        j->items = ni;
        j->cap = cap;
    }
    it = &j->items[j->n++];
    wcscpy_s(it->src, MAX_PATH, src);
    wcscpy_s(it->dst, MAX_PATH, dst);
    it->size = size;
    it->t = *t;
    it->is_save = is_save;
    j->to_copy += size;
    if (size > FAT32_MAX && !j->big) {
        j->big = 1;
        wcscpy_s(j->big_name, MAX_PATH, src);
    }
    return 1;
}

static int skip_name(const WCHAR *name, const WCHAR *const *skip)
{
    for (; skip && *skip; skip++)
        if (!_wcsicmp(name, *skip))
            return 1;
    return 0;
}

/* Everything in src into dst. skip: names left out, at any depth. */
static int add_folder(Job *j, const WCHAR *src, const WCHAR *dst, const WCHAR *const *skip, int is_save)
{
    WCHAR pattern[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    if (swprintf_s(pattern, MAX_PATH, L"%s\\*", src) < 0)
        return 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || skip_name(fd.cFileName, skip))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (swprintf_s(s, MAX_PATH, L"%s\\%s", src, fd.cFileName) < 0 || swprintf_s(d, MAX_PATH, L"%s\\%s", dst, fd.cFileName) < 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = add_folder(j, s, d, skip, is_save);
        else
            ok = add_item(j, s, d, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, &fd.ftLastWriteTime, is_save);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The compiled shaders only: native_shaders also has PC builds. */
static int add_shaders(Job *j, const WCHAR *game, const WCHAR *to)
{
    static const WCHAR *const pats[] = { L"*.spv", L"*.spvc", L"*.spv.pak", L"*.spvc.pak", L"*.inputs", L"*.textures", L"pipelines.list" };
    WCHAR pattern[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    int i, ok = 1;
    for (i = 0; ok && i < (int)(sizeof pats / sizeof pats[0]); i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h;
        swprintf_s(pattern, MAX_PATH, L"%s\\native_shaders\\%s", game, pats[i]);
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            /* (*.spv also finds .spv.pak on some volumes: the same name twice is a no-op after the first pass) */
            swprintf_s(s, MAX_PATH, L"%s\\native_shaders\\%s", game, fd.cFileName);
            swprintf_s(d, MAX_PATH, L"%s\\native_shaders\\%s", to, fd.cFileName);
            ok = add_item(j, s, d, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, &fd.ftLastWriteTime, 0);
        } while (ok && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return ok;
}

static int copy_one(Job *j, const Item *it, uint8_t *buf, WCHAR *err, int errn)
{
    enum { kBuf = 4 << 20 };
    WCHAR part[MAX_PATH];
    HANDLE in, out;
    int ok = 1;
    if (swprintf_s(part, MAX_PATH, L"%s.part", it->dst) < 0) {
        swprintf_s(err, errn, L"The path is too long: %s", it->dst);
        return 0;
    }
    make_parents(it->dst);
    in = CreateFileW(it->src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"Could not read %s", it->src);
        return 0;
    }
    out = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        swprintf_s(err, errn, L"Could not write %s", it->dst);
        return 0;
    }
    for (;;) {
        DWORD got = 0, put = 0;
        if (*j->cancel) {
            swprintf_s(err, errn, L"Cancelled.");
            ok = 0;
            break;
        }
        if (!ReadFile(in, buf, kBuf, &got, NULL)) {
            swprintf_s(err, errn, L"Could not read %s", it->src);
            ok = 0;
            break;
        }
        if (!got)
            break;
        if (!WriteFile(out, buf, got, &put, NULL) || put != got) {
            swprintf_s(err, errn, L"Could not write %s. Is the card full or was it removed?", it->dst);
            ok = 0;
            break;
        }
        j->done += got;
        if (j->progress && j->to_copy)
            j->progress((int)(j->done * 1000 / j->to_copy), NULL);
    }
    if (ok)
        SetFileTime(out, NULL, NULL, &it->t);
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(part);
        return 0;
    }
    /* A save the card already has: kept, in case the console's was the newer. */
    if (it->is_save && GetFileAttributesW(it->dst) != INVALID_FILE_ATTRIBUTES && j->backup[0]) {
        const WCHAR *rel = it->dst + wcslen(j->out) + wcslen(L"\\SvR2011\\Saves\\");
        WCHAR keep[MAX_PATH];
        if (swprintf_s(keep, MAX_PATH, L"%s\\%s", j->backup, rel) > 0) {
            make_parents(keep);
            MoveFileExW(it->dst, keep, MOVEFILE_REPLACE_EXISTING);
        }
    }
    if (!MoveFileExW(part, it->dst, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part);
        swprintf_s(err, errn, L"Could not write %s", it->dst);
        return 0;
    }
    return 1;
}

/* The PC's online sign-in for the console (Report a problem needs it): online_name, online_server, online_token and
   online_xuid from svr2011.toml as SvR2011\online.toml, which the game merges into its own settings and deletes
   (src/online.cpp). Not online_enabled: online play isn't tested on the console. Written only with a sign-in. */
static void write_online(const WCHAR *game_dir, const WCHAR *to)
{
    WCHAR toml[MAX_PATH], dst[MAX_PATH];
    FILE *in = NULL, *out = NULL;
    char line[512], kept[2048] = "";
    int token = 0;
    swprintf_s(toml, MAX_PATH, L"%s\\svr2011.toml", game_dir);
    if (_wfopen_s(&in, toml, L"rb") || !in)
        return;
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '[')
            break;
        if (!strncmp(line, "online_name", 11) || !strncmp(line, "online_server", 13) ||
            !strncmp(line, "online_token", 12) || !strncmp(line, "online_xuid", 11)) {
            if (!strncmp(line, "online_token", 12) && strchr(line, '"') && strchr(line, '"')[1] != '"')
                token = 1;
            if (strlen(kept) + strlen(line) + 2 < sizeof kept) {
                strcat_s(kept, sizeof kept, line);
                if (kept[strlen(kept) - 1] != '\n')
                    strcat_s(kept, sizeof kept, "\n");
            }
        }
    }
    fclose(in);
    if (!token)
        return;
    swprintf_s(dst, MAX_PATH, L"%s\\online.toml", to);
    make_parents(dst);
    if (!_wfopen_s(&out, dst, L"wb") && out) {
        fputs(kept, out);
        fclose(out);
    }
}

/* What the console's install check (port/switch/nx_install_check.cpp) needs, looked at on the card: "" when all
   is there, else the first missing ones. */
static void card_check(const WCHAR *to, WCHAR *out, int n)
{
    static const struct { const WCHAR *path; int folder; uint64_t min; } needs[] = {
        { L"default.xex", 0, 1 << 20 }, { L"plist360.arc", 0, 64 << 10 }, { L"pac\\_startup.pac", 0, 64 << 10 },
        { L"pac\\menu", 1, 0 }, { L"sound\\Game.pck", 0, 1 << 20 }, { L"movies", 1, 0 } };
    int i, found = 0;
    out[0] = 0;
    for (i = 0; i < (int)(sizeof needs / sizeof needs[0]); i++) {
        WCHAR p[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA a;
        int ok;
        swprintf_s(p, MAX_PATH, L"%s\\%s", to, needs[i].path);
        ok = GetFileAttributesExW(p, GetFileExInfoStandard, &a) != 0;
        if (ok && needs[i].folder) {
            WIN32_FIND_DATAW fd;
            HANDLE h;
            wcscat_s(p, MAX_PATH, L"\\*.*");
            ok = 0;
            if ((h = FindFirstFileW(p, &fd)) != INVALID_HANDLE_VALUE) {
                do ok |= fd.cFileName[0] != L'.'; while (!ok && FindNextFileW(h, &fd));
                FindClose(h);
            }
        } else if (ok) {
            ok = !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                 ((((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) >= needs[i].min);
        }
        if (!ok && found++ < 4) {
            if (out[0]) wcscat_s(out, n, L", ");
            wcscat_s(out, n, needs[i].path);
        }
    }
}

static void write_howto(const WCHAR *out_dir)
{
    static const char text[] =
        "WWE SmackDown vs. Raw 2011 on a modded Nintendo Switch\r\n"
        "\r\n"
        "If you wrote this straight to the SD card, skip step 1.\r\n"
        "\r\n"
        "1. Copy svr2011.nsp and the SvR2011 folder to the top of the SD card.\r\n"
        "2. Put the card back and start DBI. Choose \"Browse SD card\", then select svr2011.nsp and install it.\r\n"
        "3. Start the game from the home menu. The first start takes longer.\r\n"
        "\r\n"
        "svr2011.nsp can be deleted from the card once it is installed.\r\n"
        "\r\n"
        "To add saves, mods or music later, use the launcher again, or copy them into the SvR2011 folder:\r\n"
        "SvR2011\\Saves, SvR2011\\Mods, SvR2011\\Music.\r\n"
        "\r\n"
        "To report a problem: MY WWE, OPTIONS, REPORT A PROBLEM, or the pause menu in a match.\r\n";
    WCHAR p[MAX_PATH];
    FILE *f = NULL;
    if (swprintf_s(p, MAX_PATH, L"%s\\Install on Switch.txt", out_dir) < 0)
        return;
    if (!_wfopen_s(&f, p, L"wb") && f) {
        fwrite(text, 1, sizeof text - 1, f);
        fclose(f);
    }
}

int switch_find(const WCHAR *game_dir, const WCHAR *launcher_dir, WCHAR *out, int outn)
{
    const WCHAR *const dirs[] = { launcher_dir, game_dir };
    int i;
    for (i = 0; i < 2; i++) {
        if (!dirs[i] || !dirs[i][0])
            continue;
        swprintf_s(out, outn, L"%s\\Switch\\svr2011.nsp", dirs[i]);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    out[0] = 0;
    return 0;
}

int switch_package(const WCHAR *game_dir, const WCHAR *nsp, const WCHAR *out_folder, const SwitchOptions *opt,
                   volatile LONG *cancel, switch_progress_fn progress, WCHAR *err, int errn)
{
    WCHAR out_dir[MAX_PATH];
    static const WCHAR *const files[] = { L"default.xex", L"plist360.arc", L"plist360_4x3.arc", L"nxeart" };
    static const WCHAR *const folders[] = { L"pac", L"sound", L"movies", L"DLC", L"pad_icons" };
    static const WCHAR *const no_mods[] = { L"ArenaOverlay", L"SuperstarOverlay", L".backup", L".install.log", NULL };
    static const WCHAR *const no_saves[] = { L".mount", NULL };
    Job j;
    WCHAR to[MAX_PATH], s[MAX_PATH], d[MAX_PATH], root[MAX_PATH], fs[32] = L"";
    ULARGE_INTEGER free_bytes;
    uint8_t *buf = NULL;
    int i, ok = 0;

    /* (The top of a card comes with a slash on the end: without its slash, like any other folder.) */
    wcscpy_s(out_dir, MAX_PATH, out_folder);
    while (wcslen(out_dir) > 2 && out_dir[wcslen(out_dir) - 1] == L'\\')
        out_dir[wcslen(out_dir) - 1] = 0;
    ZeroMemory(&j, sizeof j);
    j.out = out_dir;
    j.cancel = cancel;
    j.progress = progress;
    err[0] = 0;

    /* The game's own folder can't be the target: the copy would feed itself. */
    i = (int)wcslen(game_dir);
    if (!_wcsnicmp(out_dir, game_dir, (size_t)i) && (out_dir[i] == 0 || out_dir[i] == L'\\')) {
        swprintf_s(err, errn, L"Choose a place outside the game folder.");
        return 0;
    }
    swprintf_s(to, MAX_PATH, L"%s\\SvR2011", out_dir);
    swprintf_s(s, MAX_PATH, L"%s\\default.xex", game_dir);
    if (GetFileAttributesW(s) == INVALID_FILE_ATTRIBUTES) {
        swprintf_s(err, errn, L"The game isn't installed in %s.", game_dir);
        return 0;
    }

    if (progress) progress(0, L"Comparing with what's already there\x2026");
    for (i = 0; i < (int)(sizeof files / sizeof files[0]); i++) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        swprintf_s(s, MAX_PATH, L"%s\\%s", game_dir, files[i]);
        swprintf_s(d, MAX_PATH, L"%s\\%s", to, files[i]);
        if (GetFileAttributesExW(s, GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !add_item(&j, s, d, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow, &a.ftLastWriteTime, 0))
            goto out_of_memory;
    }
    for (i = 0; i < (int)(sizeof folders / sizeof folders[0]); i++) {
        swprintf_s(s, MAX_PATH, L"%s\\%s", game_dir, folders[i]);
        swprintf_s(d, MAX_PATH, L"%s\\%s", to, folders[i]);
        if (!add_folder(&j, s, d, NULL, 0))
            goto out_of_memory;
    }
    if (!add_shaders(&j, game_dir, to))
        goto out_of_memory;
    if (opt && opt->mods) {
        swprintf_s(s, MAX_PATH, L"%s\\Mods", game_dir);
        swprintf_s(d, MAX_PATH, L"%s\\Mods", to);
        if (!add_folder(&j, s, d, no_mods, 0))
            goto out_of_memory;
    }
    if (opt && opt->music) {
        swprintf_s(s, MAX_PATH, L"%s\\Music", game_dir);
        swprintf_s(d, MAX_PATH, L"%s\\Music", to);
        if (!add_folder(&j, s, d, NULL, 0))
            goto out_of_memory;
        swprintf_s(s, MAX_PATH, L"%s\\Custom Movies", game_dir);
        swprintf_s(d, MAX_PATH, L"%s\\Custom Movies", to);
        if (!add_folder(&j, s, d, NULL, 0))
            goto out_of_memory;
    }
    if (opt && opt->saves && opt->saves_dir && opt->saves_dir[0]) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf_s(j.backup, MAX_PATH, L"%s\\SaveBackups\\%04d-%02d-%02d %02d%02d", to, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        swprintf_s(d, MAX_PATH, L"%s\\Saves", to);
        if (!add_folder(&j, opt->saves_dir, d, no_saves, 1))
            goto out_of_memory;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        swprintf_s(d, MAX_PATH, L"%s\\svr2011.nsp", out_dir);
        if (nsp && nsp[0] && GetFileAttributesExW(nsp, GetFileExInfoStandard, &a) &&
            !add_item(&j, nsp, d, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow, &a.ftLastWriteTime, 0))
            goto out_of_memory;
    }

    /* What the card can take. */
    swprintf_s(root, MAX_PATH, L"%s\\", out_dir);
    make_parents(to);
    if (GetVolumePathNameW(root, root, MAX_PATH))
        GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32);
    if (j.big && !_wcsicmp(fs, L"FAT32")) {
        swprintf_s(err, errn, L"This card is FAT32, which can't hold a file over 4 GB: %s", j.big_name);
        goto done;
    }
    if (GetDiskFreeSpaceExW(root, &free_bytes, NULL, NULL) && free_bytes.QuadPart < j.to_copy) {
        swprintf_s(err, errn, L"Not enough space: %llu MB to copy, %llu MB free.", j.to_copy >> 20,
                   (unsigned long long)(free_bytes.QuadPart >> 20));
        goto done;
    }

    if ((buf = (uint8_t *)malloc(4 << 20)) == NULL)
        goto out_of_memory;
    for (i = 0; i < j.n; i++) {
        WCHAR line[MAX_PATH + 40];
        if (progress) {
            const WCHAR *name = wcsrchr(j.items[i].src, L'\\');
            swprintf_s(line, MAX_PATH + 40, L"Copying %s", name ? name + 1 : j.items[i].src);
            progress(j.to_copy ? (int)(j.done * 1000 / j.to_copy) : 1000, line);
        }
        if (!copy_one(&j, &j.items[i], buf, err, errn))
            goto done;
    }
    write_online(game_dir, to);
    write_howto(out_dir);
    {   /* (the card as the console will see it) */
        WCHAR miss[400];
        card_check(to, miss, 400);
        if (miss[0]) {
            swprintf_s(err, errn, L"The files were copied, but the card still lacks: %s. Make Switch files again.", miss);
            goto done;
        }
    }
    if (progress) progress(1000, NULL);
    ok = 1;
    goto done;

out_of_memory:
    swprintf_s(err, errn, L"Out of memory.");
done:
    free(buf);
    free(j.items);
    return ok;
}
