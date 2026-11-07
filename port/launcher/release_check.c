/* The install against its release's file list (release_check.h). */
#include "release_check.h"
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")

/* A file's SHA-256 as 64 lowercase hex digits; 0 when it can't be read. */
static int sha256_hex(const WCHAR *path, char out[65])
{
    static unsigned char buf[1 << 16];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char digest[32];
    FILE *f = NULL;
    size_t n;
    int ok = 0, i;
    if (_wfopen_s(&f, path, L"rb") || !f)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) < 0)
        goto done;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (BCryptHashData(h, buf, (ULONG)n, 0) < 0)
            goto done;
    if (BCryptFinishHash(h, digest, sizeof digest, 0) < 0)
        goto done;
    for (i = 0; i < 32; i++)
        sprintf_s(out + i * 2, 3, "%02x", digest[i]);
    ok = 1;
done:
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    return ok;
}

int release_check(const WCHAR *dir, const WCHAR *list, int hashes, ReleaseCheck *out,
                  void (*each_bad)(const WCHAR *rel, void *ctx), void *ctx)
{
    WCHAR lp[MAX_PATH], path[MAX_PATH], rel[MAX_PATH];
    char line[2 * MAX_PATH + 128];
    FILE *f = NULL;
    ZeroMemory(out, sizeof *out);
    if (list)
        wcscpy_s(lp, MAX_PATH, list);
    else if (swprintf_s(lp, MAX_PATH, L"%s\\release_files.txt", dir) < 0)
        return 0;
    if (_wfopen_s(&f, lp, L"rb") || !f)
        return 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "SvR2011 release ", 16)) {
        fclose(f);
        return 0;
    }
    line[strcspn(line, "\r\n")] = 0;
    MultiByteToWideChar(CP_UTF8, 0, line + 16, -1, out->version, 32);
    while (fgets(line, sizeof line, f)) {
        char *t1, *t2, hex[65];
        unsigned long long size;
        WIN32_FILE_ATTRIBUTE_DATA a;
        int bad = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (!(t1 = strchr(line, '\t')) || !(t2 = strchr(t1 + 1, '\t')))
            continue;
        *t1 = *t2 = 0;
        size = _strtoui64(line, NULL, 10);
        if (!MultiByteToWideChar(CP_UTF8, 0, t2 + 1, -1, rel, MAX_PATH) ||
            swprintf_s(path, MAX_PATH, L"%s\\%s", dir, rel) < 0)
            continue;
        out->files++;
        /* (the launcher's check at each start: the Android app is the one file a PC player may delete on purpose -
           no nag and no 90 MB repair for that; a damaged one still counts, and the updater's own checks, with
           hashes, still want it) */
        if (!hashes && !_wcsicmp(rel, L"Android\\SvR2011.apk") && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) != size)
            bad = 1;
        else if (hashes && (!sha256_hex(path, hex) || _stricmp(hex, t1 + 1)))
            bad = 1;
        if (bad) {
            if (!out->bad++)
                wcscpy_s(out->first_bad, MAX_PATH, rel);
            if (each_bad)
                each_bad(rel, ctx);
        }
    }
    fclose(f);
    return 1;
}
