/* WWE SmackDown vs. Raw 2011 - the save cleaner (save_cleaner.h).
 *
 * A superstar mod plays on one of the 137 ids the game keeps as blank placeholders (superstar_mods.cpp kPool;
 * Mods\Superstars\slots.txt says which). SaveData.dat holds a record (260 bytes: names, attributes, abilities) and a
 * profile (1056: move-set, entrance, Threads) for every id, so a mod removed or turned off leaves its edits there,
 * with its Universe side entry (Saves\.universe\mods.bin) and its line in .universe\mod_profiles.txt. The game
 * writes each id's placeholder as it loads it from its own files - before any save, no mod on the id - to
 * .universe\pool_placeholders.bin (superstar_mods.cpp KeepPoolPlaceholders): an id whose saved record or profile
 * is none of its placeholders (DLC ids 59-69 have one per DLC) holds a mod's data. Taking it out puts the
 * placeholder back - the save as one that never had the mod; the game gives a mod installed there later its own.
 * The game's superstars, Created Superstars (their own ids and .cas files) and the mods that are on aren't
 * touched. With them goes each id's 40-byte entry (block B +0x3E648). The per-mod saves (per_mod_saves:
 * Saves\.mods\<folder>.dat, superstar_mods.cpp) leave the slots as placeholders - nothing to clean there; a mod
 * taken out loses its file as a whole, unread (the backup has it); .mods\.on stays. */
#include "save_cleaner.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define SD_SUM_FROM 0x1Cu
#define SD_SUM_TO 0x812DCu
#define REC_SELECTABLE 221u
#define REC_NAME 34u
#define MAX_VERSIONS 8

typedef struct {
    int n;
    int index;
    const uint8_t *rec[MAX_VERSIONS], *prof[MAX_VERSIONS], *ent[MAX_VERSIONS];
} Pool;

static uint8_t *read_all(const WCHAR *path, size_t *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    DWORD got = 0;
    *size = 0;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 30) && (buf = malloc((size_t)sz.QuadPart + 1)) &&
        ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
        buf[got] = 0;
        *size = got;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

/* In place (CreateFile on the existing file: the saves' hard links stay), or new with `create`. */
static int write_all(const WCHAR *path, const void *data, size_t size, int create)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, create ? CREATE_ALWAYS : TRUNCATE_EXISTING, 0, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, data, (DWORD)size, &put, NULL) && put == size;
    CloseHandle(h);
    return ok;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int exists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

/* The folder's state in Mods\Superstars: 2 installed and on, 1 installed and off, 0 not there. */
static int mod_state(const WCHAR *mods, const WCHAR *folder)
{
    WCHAR p[MAX_PATH];
    if (!folder[0])
        return 0;
    swprintf_s(p, MAX_PATH, L"%s\\%s\\ch.pac", mods, folder);
    if (!exists(p))
        return 0;
    swprintf_s(p, MAX_PATH, L"%s\\%s\\disabled", mods, folder);
    return exists(p) ? 1 : 2;
}

/* The game writes folder names as its narrow strings (the ANSI code page). */
static void widen(const char *s, size_t n, WCHAR *out, int out_n)
{
    int k = MultiByteToWideChar(CP_ACP, 0, s, (int)n, out, out_n - 1);
    out[k > 0 ? k : 0] = 0;
}

/* "<a>\t<b>..." lines: slots.txt ("<folder>\t<id>") or mod_profiles.txt ("<id>\t<folder>\t<fingerprint>") ->
 * folder per id. */
static void read_id_folders(const WCHAR *path, int id_first, WCHAR (*by_id)[MAX_PATH])
{
    size_t size;
    char *t = (char *)read_all(path, &size), *line, *next;
    if (!t)
        return;
    for (line = t; line && *line; line = next) {
        char *tab, *end;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if ((end = strchr(line, '\r')))
            *end = 0;
        if (!(tab = strchr(line, '\t')))
            continue;
        *tab = 0;
        if (id_first) {
            const int id = atoi(line);
            char *tab2 = strchr(tab + 1, '\t');
            if (tab2)
                *tab2 = 0;
            if (id > 0 && id < 512)
                widen(tab + 1, strlen(tab + 1), by_id[id], MAX_PATH);
        } else {
            char *last = strrchr(tab + 1, '\t');  /* (slots.txt: the id after the last tab) */
            const int id = atoi(last ? last + 1 : tab + 1);
            if (last)
                *last = 0;
            if (id > 0 && id < 512)
                widen(line, strlen(line), by_id[id], MAX_PATH);
        }
    }
    free(t);
}

/* Letters and digits only, lower case, a leading "the" dropped: "The Sandman" and "sandman", "Bob Orton" and
 * "bob_orton" are the same. */
static void name_key(const WCHAR *s, WCHAR *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 1 < n; s++)
        if (iswalnum(*s))
            out[k++] = (WCHAR)towlower(*s);
    out[k] = 0;
    if (!wcsncmp(out, L"the", 3) && out[3])
        memmove(out, out + 3, (wcslen(out + 3) + 1) * sizeof(WCHAR));
}

static ScItem *item_for(ScItem *items, int *n, int id, const WCHAR *folder)
{
    int i;
    for (i = 0; i < *n; i++)
        if ((folder[0] && !_wcsicmp(items[i].folder, folder)) || (id && items[i].id == id))
            return &items[i];
    if (folder[0] && !id) {  /* a folder only (Universe, a mod save): the save's mod of that name, its folder unknown */
        WCHAR a[MAX_PATH], b[64];
        name_key(folder, a, MAX_PATH);
        for (i = 0; i < *n; i++) {
            if (items[i].folder[0] || !items[i].id)
                continue;
            name_key(items[i].name, b, 64);
            if (a[0] && !wcscmp(a, b)) {
                wcscpy_s(items[i].folder, MAX_PATH, folder);
                return &items[i];
            }
        }
    }
    if (*n >= SC_MAX)
        return NULL;
    ZeroMemory(&items[*n], sizeof items[*n]);
    items[*n].id = id;
    wcscpy_s(items[*n].folder, MAX_PATH, folder);
    return &items[(*n)++];
}

typedef struct {
    uint8_t *pool, *save, *univ;
    size_t pool_n, save_n, univ_n;
    uint32_t rec_size, prof_size, ent_size, rec_off, prof_off, ent_off;
    Pool ids[512];
} Data;

static void data_free(Data *d)
{
    free(d->pool);
    free(d->save);
    free(d->univ);
}

static int data_read(const WCHAR *saves, Data *d, WCHAR *err, size_t err_n)
{
    WCHAR p[MAX_PATH];
    size_t at;
    uint32_t i, count;
    ZeroMemory(d, sizeof *d);
    swprintf_s(p, MAX_PATH, L"%s\\SaveData.dat", saves);
    if (!(d->save = read_all(p, &d->save_n))) {
        swprintf_s(err, err_n, L"There's no main save (SaveData.dat) in %s.", saves);
        return 0;
    }
    swprintf_s(p, MAX_PATH, L"%s\\.universe\\pool_placeholders.bin", saves);
    d->pool = read_all(p, &d->pool_n);
    if (!d->pool || d->pool_n < 36 || memcmp(d->pool, "SVRPOOL2", 8)) {
        swprintf_s(err, err_n, L"Start the game once with these saves first: at its start it notes the game's own "
                               L"superstar data the cleaner compares the save with (version 2.1.9 and later).");
        return 0;
    }
    d->rec_size = le32(d->pool + 8), d->prof_size = le32(d->pool + 12), d->ent_size = le32(d->pool + 16);
    d->rec_off = le32(d->pool + 20), d->prof_off = le32(d->pool + 24), d->ent_off = le32(d->pool + 28);
    count = le32(d->pool + 32);
    if (d->rec_size != 260 || d->prof_size != 1056 || d->ent_size != 40 || d->save_n < SD_SUM_TO ||
        d->save_n < (size_t)d->prof_off + 512u * d->prof_size || d->save_n < (size_t)d->ent_off + 512u * d->ent_size) {
        swprintf_s(err, err_n, L"The save or the game's notes (.universe\\pool_placeholders.bin) aren't as expected.");
        return 0;
    }
    for (i = 0, at = 36; i < count && at + 4 + d->rec_size + d->prof_size + d->ent_size <= d->pool_n; i++) {
        const uint32_t id = d->pool[at] | d->pool[at + 1] << 8, index = d->pool[at + 2] | d->pool[at + 3] << 8;
        Pool *v = id < 512 && index < 512 ? &d->ids[id] : NULL;
        if (v && v->n < MAX_VERSIONS) {
            v->index = (int)index;
            v->rec[v->n] = d->pool + at + 4;
            v->prof[v->n] = d->pool + at + 4 + d->rec_size;
            v->ent[v->n] = d->pool + at + 4 + d->rec_size + d->prof_size;
            v->n++;
        }
        at += 4 + d->rec_size + d->prof_size + d->ent_size;
    }
    swprintf_s(p, MAX_PATH, L"%s\\.universe\\mods.bin", saves);
    d->univ = read_all(p, &d->univ_n);
    if (d->univ && (d->univ_n < 12 || memcmp(d->univ, "SVRUNIV1", 8)))
        free(d->univ), d->univ = NULL;
    return 1;
}

/* The id's record, profile and entry in the save are one of its placeholders (the selectable byte aside: the game
 * turns it off for an id no mod holds). */
static int is_placeholder(const Data *d, int id)
{
    const Pool *v = &d->ids[id];
    const uint8_t *rec = d->save + d->rec_off + (size_t)v->index * d->rec_size;
    const uint8_t *prof = d->save + d->prof_off + (size_t)v->index * d->prof_size;
    const uint8_t *ent = d->save + d->ent_off + (size_t)v->index * d->ent_size;
    int k, rec_ok = 0, prof_ok = 0, ent_ok = 0;
    for (k = 0; k < v->n; k++) {
        rec_ok |= !memcmp(rec, v->rec[k], REC_SELECTABLE) &&
                  !memcmp(rec + REC_SELECTABLE + 1, v->rec[k] + REC_SELECTABLE + 1, d->rec_size - REC_SELECTABLE - 1);
        prof_ok |= !memcmp(prof, v->prof[k], d->prof_size);
        ent_ok |= !memcmp(ent, v->ent[k], d->ent_size);
    }
    return rec_ok && prof_ok && ent_ok;
}

/* mods.bin: "SVRUNIV1", u32 n, per mod: u8 length + folder, 24 entry bytes, 150 place bytes; then n x n bytes.
 * Calls fn for each mod's folder; returns n (-1: cut short). */
static int univ_each(const Data *d, void (*fn)(void *, int, const WCHAR *), void *ctx)
{
    size_t at = 12;
    uint32_t i, n;
    if (!d->univ)
        return 0;
    n = le32(d->univ + 8);
    for (i = 0; i < n; i++) {
        WCHAR f[MAX_PATH];
        size_t len;
        if (at >= d->univ_n || at + 1 + (len = d->univ[at]) + 24 + 150 > d->univ_n)
            return -1;
        widen((const char *)d->univ + at + 1, len, f, MAX_PATH);
        if (fn)
            fn(ctx, (int)i, f);
        at += 1 + len + 24 + 150;
    }
    return at + (size_t)n * n <= d->univ_n ? (int)n : -1;
}

typedef struct {
    const WCHAR *mods;
    ScItem *items;
    int *n;
} UnivScan;

static void univ_scan_one(void *ctx, int i, const WCHAR *folder)
{
    UnivScan *s = (UnivScan *)ctx;
    ScItem *it;
    const int st = mod_state(s->mods, folder);
    (void)i;
    if (st == 2 || !(it = item_for(s->items, s->n, 0, folder)))
        return;
    it->universe = 1;
    it->off = st == 1;
    if (!it->name[0])
        wcscpy_s(it->name, 40, folder);
}

int save_clean_scan(const WCHAR *saves, const WCHAR *mods, ScItem *items, WCHAR *err, size_t err_n)
{
    static WCHAR slots[512][MAX_PATH], marks[512][MAX_PATH];
    WCHAR p[MAX_PATH];
    Data *d = calloc(1, sizeof(Data));
    int n = 0, id;
    if (!d) return -1;
    if (!data_read(saves, d, err, err_n)) {
        data_free(d);
        free(d);
        return -1;
    }
    ZeroMemory(slots, sizeof slots);
    ZeroMemory(marks, sizeof marks);
    swprintf_s(p, MAX_PATH, L"%s\\slots.txt", mods);
    read_id_folders(p, 0, slots);
    swprintf_s(p, MAX_PATH, L"%s\\.universe\\mod_profiles.txt", saves);
    read_id_folders(p, 1, marks);
    for (id = 1; id < 512; id++) {
        const WCHAR *folder = slots[id][0] ? slots[id] : marks[id];
        int st, leftover, marked;
        ScItem *it;
        if (slots[id][0] && mod_state(mods, slots[id]) == 2)
            continue;  /* (a mod that's on holds it) */
        leftover = d->ids[id].n && !is_placeholder(d, id);
        marked = marks[id][0] != 0;
        if (!leftover && !marked)
            continue;
        st = mod_state(mods, folder);
        if (!(it = item_for(items, &n, id, folder)))
            break;
        it->id = id;
        it->index = d->ids[id].index;
        it->off = st == 1;
        it->profile |= leftover;
        it->mark |= marked;
        if (leftover) {
            const uint8_t *name = d->save + d->rec_off + (size_t)d->ids[id].index * d->rec_size + REC_NAME;
            widen((const char *)name, strnlen((const char *)name, 32), it->name, 40);
        }
        if (!it->name[0])
            wcscpy_s(it->name, 40, folder[0] ? folder : L"(a mod)");
    }
    {
        UnivScan s = {mods, items, &n};
        univ_each(d, univ_scan_one, &s);
    }
    {   /* Saves\.mods\<folder>.dat (not .on: the option's marker) */
        WIN32_FIND_DATAW fd;
        HANDLE f;
        swprintf_s(p, MAX_PATH, L"%s\\.mods\\*.dat", saves);
        if ((f = FindFirstFileW(p, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                WCHAR folder[MAX_PATH];
                ScItem *it;
                int st;
                wcscpy_s(folder, MAX_PATH, fd.cFileName);
                folder[wcslen(folder) - 4] = 0;
                if ((st = mod_state(mods, folder)) == 2 || !(it = item_for(items, &n, 0, folder)))
                    continue;
                it->mod_save = 1;
                it->off = st == 1;
                if (!it->name[0])
                    wcscpy_s(it->name, 40, folder);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    data_free(d);
    free(d);
    return n;
}

static int taken(const ScItem *items, int n, int with_off, int id, const WCHAR *folder)
{
    int i;
    for (i = 0; i < n; i++)
        if ((!items[i].off || with_off) &&
            ((id && items[i].id == id) || (folder && folder[0] && !_wcsicmp(items[i].folder, folder))))
            return 1;
    return 0;
}

int save_clean_apply(const WCHAR *saves, const ScItem *items, int n, int with_off, WCHAR *err, size_t err_n)
{
    WCHAR p[MAX_PATH];
    Data *d = calloc(1, sizeof(Data));
    int i, done = 0, ok = 1;
    if (!d) return -1;
    if (!data_read(saves, d, err, err_n)) {
        data_free(d);
        free(d);
        return -1;
    }
    /* SaveData.dat: the placeholders back */
    {
        int changed = 0;
        for (i = 0; i < n; i++) {
            const Pool *v;
            if (!items[i].profile || (items[i].off && !with_off) || items[i].id <= 0 || items[i].id >= 512)
                continue;
            v = &d->ids[items[i].id];
            if (!v->n)
                continue;
            memcpy(d->save + d->rec_off + (size_t)v->index * d->rec_size, v->rec[v->n - 1], d->rec_size);
            memcpy(d->save + d->prof_off + (size_t)v->index * d->prof_size, v->prof[v->n - 1], d->prof_size);
            memcpy(d->save + d->ent_off + (size_t)v->index * d->ent_size, v->ent[v->n - 1], d->ent_size);
            changed = 1;
        }
        if (changed) {
            uint32_t sum = 0, k;
            for (k = SD_SUM_FROM; k < SD_SUM_TO; k++)
                sum += d->save[k];
            d->save[0x18] = (uint8_t)(sum >> 24), d->save[0x19] = (uint8_t)(sum >> 16);
            d->save[0x1A] = (uint8_t)(sum >> 8), d->save[0x1B] = (uint8_t)sum;
            swprintf_s(p, MAX_PATH, L"%s\\SaveData.dat", saves);
            if (!write_all(p, d->save, d->save_n, 0)) {
                swprintf_s(err, err_n, L"SaveData.dat couldn't be written (read-only, or open in another program?).");
                ok = 0;
            }
        }
    }
    /* .universe\mods.bin: the others' entries, their rivalry bytes re-indexed */
    if (ok && d->univ) {
        const int un = univ_each(d, NULL, NULL);
        if (un > 0) {
            int *keep = calloc((size_t)un, sizeof(int)), kn = 0, j;
            size_t at = 12, matrix;
            uint8_t *out = malloc(d->univ_n);
            size_t on = 12;
            if (keep && out) {
                memcpy(out, d->univ, 12);
                for (i = 0; i < un; i++) {
                    WCHAR f[MAX_PATH];
                    const size_t len = d->univ[at], sz = 1 + len + 24 + 150;
                    widen((const char *)d->univ + at + 1, len, f, MAX_PATH);
                    if (!taken(items, n, with_off, 0, f)) {
                        memcpy(out + on, d->univ + at, sz);
                        on += sz;
                        keep[kn++] = i;
                    }
                    at += sz;
                }
                matrix = at;
                if (kn < un) {
                    for (i = 0; i < kn; i++)
                        for (j = 0; j < kn; j++)
                            out[on++] = d->univ[matrix + (size_t)keep[i] * un + keep[j]];
                    out[8] = (uint8_t)kn, out[9] = (uint8_t)(kn >> 8), out[10] = (uint8_t)(kn >> 16), out[11] = (uint8_t)(kn >> 24);
                    swprintf_s(p, MAX_PATH, L"%s\\.universe\\mods.bin", saves);
                    if (!write_all(p, out, on, 1)) {
                        swprintf_s(err, err_n, L".universe\\mods.bin couldn't be written.");
                        ok = 0;
                    }
                }
            }
            free(keep);
            free(out);
        }
    }
    /* .universe\mod_profiles.txt: the others' lines */
    if (ok) {
        size_t size;
        char *t;
        swprintf_s(p, MAX_PATH, L"%s\\.universe\\mod_profiles.txt", saves);
        if ((t = (char *)read_all(p, &size))) {
            char *out = malloc(size + 1), *line, *next;
            size_t on = 0;
            if (out) {
                for (line = t; line && *line; line = next) {
                    WCHAR f[MAX_PATH] = L"";
                    const char *tab = strchr(line, '\t'), *tab2;
                    const int id = atoi(line);
                    next = strchr(line, '\n');
                    next = next ? next + 1 : NULL;
                    if (tab && (tab2 = strchr(tab + 1, '\t')))
                        widen(tab + 1, (size_t)(tab2 - tab - 1), f, MAX_PATH);
                    if (taken(items, n, with_off, id, f))
                        continue;
                    {
                        const size_t len = next ? (size_t)(next - line) : strlen(line);
                        memcpy(out + on, line, len);
                        on += len;
                    }
                }
                if (on != size && !write_all(p, out, on, 1)) {
                    swprintf_s(err, err_n, L".universe\\mod_profiles.txt couldn't be written.");
                    ok = 0;
                }
                free(out);
            }
            free(t);
        }
    }
    /* Saves\.mods\<folder>.dat (the backup has it) */
    for (i = 0; ok && i < n; i++) {
        if (!items[i].mod_save || (items[i].off && !with_off))
            continue;
        swprintf_s(p, MAX_PATH, L"%s\\.mods\\%s.dat", saves, items[i].folder);
        DeleteFileW(p);
    }
    for (i = 0; i < n; i++)
        done += !items[i].off || with_off;
    data_free(d);
    free(d);
    return ok ? done : -1;
}
