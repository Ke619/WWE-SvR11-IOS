/* WWE SmackDown vs. Raw 2011 PC launcher - Verify game files (see verify.h). */

#include "verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define VERIFY_MENU_TABLE 1   /* menu.pac: its menu table (the port adds rows) is left out */

typedef struct {
    const WCHAR *path;
    uint64_t size;
    uint32_t crc, crc_ported, flags;
} GameFile;

static const GameFile k_files[] = {
#include "game_files_crc.inc"
};
#define N_FILES ((int)(sizeof k_files / sizeof k_files[0]))

/* menu.pac's MFLO/0000 slot (src/game_files.cpp kMflo, kMfloSlot). */
#define MENU_TABLE_AT   0x25F000u
#define MENU_TABLE_SIZE 0x6800u

#define READ_CHUNK (4u << 20)

static uint32_t s_crc_table[8][256];

static void crc_init(void)
{
    uint32_t i, j;
    if (s_crc_table[0][1])
        return;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (j = 0; j < 8; j++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[0][i] = c;
    }
    for (i = 0; i < 256; i++)
        for (j = 1; j < 8; j++)
            s_crc_table[j][i] = (s_crc_table[j - 1][i] >> 8) ^ s_crc_table[0][s_crc_table[j - 1][i] & 0xFF];
}

/* CRC-32 (zlib's), slicing by 8. `c`: the CRC so far (0 to start). */
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    while (n >= 8) {
        const uint32_t a = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        const uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        c = s_crc_table[7][a & 0xFF] ^ s_crc_table[6][(a >> 8) & 0xFF] ^ s_crc_table[5][(a >> 16) & 0xFF] ^
            s_crc_table[4][a >> 24] ^ s_crc_table[3][b & 0xFF] ^ s_crc_table[2][(b >> 8) & 0xFF] ^
            s_crc_table[1][(b >> 16) & 0xFF] ^ s_crc_table[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--)
        c = s_crc_table[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* Files a game folder may lack: the disc's system update and dashboard art. */
static int optional_file(const WCHAR *path)
{
    return !wcsncmp(path, L"$SystemUpdate\\", 14) || !wcscmp(path, L"nxeart");
}

static void add_bad(VerifyResult *r, const WCHAR *path, int missing)
{
    if (missing)
        r->missing++;
    else
        r->damaged++;
    if (r->bad_count < VERIFY_MAX_BAD) {
        wcscpy_s(r->bad[r->bad_count], MAX_PATH, path);
        r->bad_missing[r->bad_count] = missing;
        r->bad_count++;
    }
}

uint32_t verify_crc32(uint32_t crc, const void *data, size_t n)
{
    crc_init();
    return crc_update(crc, (const uint8_t *)data, n);
}

/* (the list's first entry is default.xex) */
int verify_xex_size_supported(uint64_t size) { return size == k_files[0].size; }
int verify_xex_crc_supported(uint32_t crc) { return crc == k_files[0].crc; }

int verify_xex_supported(const WCHAR *path)
{
    static WCHAR cached[MAX_PATH];
    static FILETIME cached_t;
    static uint64_t cached_size;
    static int cached_answer = -1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    uint64_t size;
    HANDLE h;
    uint8_t *buf;
    uint32_t c = 0;
    DWORD got;
    int answer = -1;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa))
        return -1;
    size = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    if (!wcscmp(cached, path) && size == cached_size && !CompareFileTime(&fa.ftLastWriteTime, &cached_t))
        return cached_answer;
    if (!verify_xex_size_supported(size)) {
        answer = 0;   /* (another size: no need to read it) */
    } else if ((h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, NULL)) != INVALID_HANDLE_VALUE) {
        if ((buf = (uint8_t *)malloc(READ_CHUNK)) != NULL) {
            uint64_t at = 0;
            crc_init();
            while (ReadFile(h, buf, READ_CHUNK, &got, NULL) && got) {
                c = crc_update(c, buf, got);
                at += got;
            }
            free(buf);
            answer = at == size ? verify_xex_crc_supported(c) : -1;
        }
        CloseHandle(h);
    }
    if (answer >= 0) {
        wcscpy_s(cached, MAX_PATH, path);
        cached_t = fa.ftLastWriteTime;
        cached_size = size;
        cached_answer = answer;
    }
    return answer;
}

int verify_game_files(const WCHAR *game, volatile LONG *cancel, VerifyProgress progress, void *ctx,
                      VerifyResult *out)
{
    uint8_t *buf;
    uint64_t total = 0, done = 0;
    int i, ok = 1, present = 0;
    memset(out, 0, sizeof *out);
    crc_init();
    buf = (uint8_t *)malloc(READ_CHUNK);
    if (!buf)
        return 0;
    for (i = 0; i < N_FILES; i++)
        total += k_files[i].size;
    for (i = 0; i < N_FILES && ok; i++) {
        const GameFile *g = &k_files[i];
        WCHAR p[MAX_PATH];
        HANDLE h;
        LARGE_INTEGER sz;
        uint32_t c = 0;
        uint64_t at = 0;
        swprintf_s(p, MAX_PATH, L"%s\\%s", game, g->path);
        h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (!optional_file(g->path)) {
                out->files++;
                add_bad(out, p, 1);
            }
            done += g->size;
            continue;
        }
        out->files++;
        present++;
        if (progress)
            progress(done, total, g->path, ctx);
        if (!GetFileSizeEx(h, &sz) || (uint64_t)sz.QuadPart != g->size) {
            CloseHandle(h);
            if (i == 0)   /* (default.xex: another release's) */
                out->other_release = 1;
            add_bad(out, p, 0);
            done += g->size;
            continue;
        }
        for (;;) {
            DWORD got = 0;
            if (cancel && *cancel) {
                ok = 0;
                break;
            }
            if (!ReadFile(h, buf, READ_CHUNK, &got, NULL) || !got)
                break;
            if (g->flags & VERIFY_MENU_TABLE) {   /* (the bytes of the menu table are left out) */
                const uint64_t lo = MENU_TABLE_AT, hi = (uint64_t)MENU_TABLE_AT + MENU_TABLE_SIZE;
                const uint64_t a = at, b = at + got;
                if (b <= lo || a >= hi) {
                    c = crc_update(c, buf, got);
                } else {
                    if (a < lo)
                        c = crc_update(c, buf, (size_t)(lo - a));
                    if (b > hi)
                        c = crc_update(c, buf + (hi - a), (size_t)(b - hi));
                }
            } else {
                c = crc_update(c, buf, got);
            }
            at += got;
            done += got;
            if (progress)
                progress(done, total, g->path, ctx);
        }
        CloseHandle(h);
        if (!ok)
            break;
        if (at != g->size || (c != g->crc && (!g->crc_ported || c != g->crc_ported))) {
            if (i == 0 && at == g->size)   /* (default.xex: another release's) */
                out->other_release = 1;
            add_bad(out, p, 0);
        }
    }
    free(buf);
    /* Most of the files different: another edition of the game, not damage. */
    if (present >= 20 && out->damaged > present / 2)
        out->other_edition = 1;
    return ok;
}

void verify_describe(const VerifyResult *r, WCHAR *out, size_t n)
{
    const WCHAR *name;
    if (!r->files) {
        swprintf_s(out, n, L"No game files were found in the game folder: install the game first.");
        return;
    }
    if (r->other_release) {   /* (Repair can't help: the disc is another release) */
        swprintf_s(out, n, L"%s", VERIFY_OTHER_RELEASE);
        return;
    }
    if (r->other_edition) {
        swprintf_s(out, n, L"Most game files differ from the ones this launcher knows (%d of %d): this looks like "
                           L"another edition of the game, so they can't be checked.",
                   r->damaged, r->files);
        return;
    }
    if (!r->missing && !r->damaged) {
        swprintf_s(out, n, L"Your game files are OK: all %d match the game disc.", r->files);
        return;
    }
    name = r->bad_count ? wcsrchr(r->bad[0], L'\\') : NULL;
    name = name ? name + 1 : L"";
    if (r->damaged && r->missing)
        swprintf_s(out, n, L"%d damaged and %d missing game file(s) (first: %s). Click Repair, then Install from "
                           L"your disc image.", r->damaged, r->missing, name);
    else if (r->damaged)
        swprintf_s(out, n, L"%d damaged game file(s) (first: %s): they can make the game crash. Click Repair, "
                           L"then Install from your disc image.", r->damaged, name);
    else
        swprintf_s(out, n, L"%d missing game file(s) (first: %s). Install from your disc image to add them.",
                   r->missing, name);
}

int verify_set_aside(const VerifyResult *r)
{
    int i, n = 0;
    for (i = 0; i < r->bad_count; i++) {
        WCHAR to[MAX_PATH + 16];
        if (r->bad_missing[i])
            continue;
        swprintf_s(to, MAX_PATH + 16, L"%s.damaged", r->bad[i]);
        if (MoveFileExW(r->bad[i], to, MOVEFILE_REPLACE_EXISTING))
            n++;
    }
    return n;
}
