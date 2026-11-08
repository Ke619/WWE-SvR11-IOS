/* WWE SmackDown vs. Raw 2011 - Report a problem: a recorded match and the mods it used (report_match.h). */

#include "report_match.h"
#include "report_send.h"

#include <bcrypt.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winhttp.h>

/* ── files ───────────────────────────────────────────────────────────────── */

static uint8_t *read_all(const WCHAR *path, size_t *n, size_t max)
{
    FILE *f = _wfsopen(path, L"rb", _SH_DENYNO);   /* (shared: the game may still be writing the match) */
    uint8_t *d;
    long long len;
    *n = 0;
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    len = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (len < 0 || (unsigned long long)len > max || !(d = (uint8_t *)malloc((size_t)len + 1))) {
        fclose(f);
        return NULL;
    }
    *n = fread(d, 1, (size_t)len, f);
    d[*n] = 0;
    fclose(f);
    return d;
}

/* The files of a folder (recursively): relative paths ('\') and sizes; dot files and "disabled" left out (the
   game's and launcher's own, not the mod's - as the launcher's bundle records). */
typedef struct {
    WCHAR rel[MAX_PATH];
    char key[MAX_PATH * 3];   /* rel in UTF-8, lower-case, '/' - the sort and hash key */
    uint64_t size, time;
} DirFile;

typedef struct {
    DirFile *f;
    int n, cap;
} DirList;

static void dir_walk(const WCHAR *root, const WCHAR *rel, DirList *l)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*", root, rel);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || !_wcsicmp(fd.cFileName, L"disabled")) continue;
        swprintf_s(sub, MAX_PATH, rel[0] ? L"%s\\%s" : L"%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dir_walk(root, sub, l);
            continue;
        }
        if (l->n == l->cap) {
            DirFile *m = (DirFile *)realloc(l->f, sizeof *l->f * (l->cap ? l->cap * 2 : 256));
            if (!m) break;
            l->f = m, l->cap = l->cap ? l->cap * 2 : 256;
        }
        {
            DirFile *e = &l->f[l->n++];
            char *p;
            wcscpy_s(e->rel, MAX_PATH, sub);
            WideCharToMultiByte(CP_UTF8, 0, sub, -1, e->key, sizeof e->key, NULL, NULL);
            for (p = e->key; *p; p++) *p = *p == '\\' ? '/' : (*p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p);
            e->size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            e->time = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int key_cmp(const void *a, const void *b) { return strcmp(((const DirFile *)a)->key, ((const DirFile *)b)->key); }

static void dir_list(const WCHAR *dir, DirList *l)
{
    ZeroMemory(l, sizeof *l);
    dir_walk(dir, L"", l);
    if (l->n) qsort(l->f, (size_t)l->n, sizeof *l->f, key_cmp);
}

/* ── the content hash ────────────────────────────────────────────────────── */

/* The hash cache, <game>\Reports\.modhash: "<listing key>\t<sha256>\t<size>" lines. The listing key - FNV-1a 64
   over each file's sorted path, size and write time - is cheap; a mod changed since gets a new key, so its hash
   is made again (never an old hash sent with new content). */
static WCHAR s_cache_file[MAX_PATH];

static uint64_t listing_key(const DirList *l)
{
    uint64_t h = 1469598103934665603ull;
    int i;
    for (i = 0; i < l->n; i++) {
        const unsigned char *p = (const unsigned char *)l->f[i].key;
        uint64_t v[2];
        size_t k;
        for (; *p; p++) h = (h ^ *p) * 1099511628211ull;
        v[0] = l->f[i].size, v[1] = l->f[i].time;
        for (k = 0; k < sizeof v; k++) h = (h ^ ((const unsigned char *)v)[k]) * 1099511628211ull;
    }
    return h;
}

static int cache_get(uint64_t key, char hex[65], uint64_t *size)
{
    size_t n;
    uint8_t *d = s_cache_file[0] ? read_all(s_cache_file, &n, 4u << 20) : NULL;
    char want[24], *p;
    int found = 0;
    if (!d) return 0;
    snprintf(want, sizeof want, "%016llx\t", (unsigned long long)key);
    for (p = (char *)d; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, want, 17) && strlen(p) > 17 + 64 && p[17 + 64] == '\t') {
            memcpy(hex, p + 17, 64);
            hex[64] = 0;
            *size = _strtoui64(p + 17 + 65, NULL, 10);
            found = 1;
        }
    free(d);
    return found;
}

static void cache_put(uint64_t key, const char *hex, uint64_t size)
{
    FILE *f;
    if (!s_cache_file[0] || _wfopen_s(&f, s_cache_file, L"ab") || !f) return;
    fprintf(f, "%016llx\t%s\t%llu\n", (unsigned long long)key, hex, (unsigned long long)size);
    fclose(f);
}

int report_mod_hash(const WCHAR *folder, char hex[65], uint64_t *size)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    UCHAR digest[32];
    DirList l;
    uint64_t key;
    int i, ok = 0;
    hex[0] = 0;
    *size = 0;
    dir_list(folder, &l);
    if (!l.n) { free(l.f); return 0; }
    key = listing_key(&l);
    if (cache_get(key, hex, size)) { free(l.f); return 1; }
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static uint8_t buf[1 << 20];
        ok = 1;
        for (i = 0; i < l.n && ok; i++) {
            WCHAR full[MAX_PATH];
            uint8_t le[8];
            FILE *f;
            int k;
            size_t got;
            for (k = 0; k < 8; k++) le[k] = (uint8_t)(l.f[i].size >> (8 * k));
            BCryptHashData(h, (PUCHAR)l.f[i].key, (ULONG)strlen(l.f[i].key) + 1, 0);   /* (with its 0) */
            BCryptHashData(h, le, 8, 0);
            swprintf_s(full, MAX_PATH, L"%s\\%s", folder, l.f[i].rel);
            if (!(f = _wfsopen(full, L"rb", _SH_DENYNO))) { ok = 0; break; }
            while ((got = fread(buf, 1, sizeof buf, f)) > 0) BCryptHashData(h, buf, (ULONG)got, 0);
            fclose(f);
            *size += l.f[i].size;
        }
        if (ok && BCryptFinishHash(h, digest, 32, 0) == 0)
            for (i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        else
            ok = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(l.f);
    if (ok) cache_put(key, hex, *size);
    return ok;
}

/* ── a stored zip of a folder (no entry limit; files read one at a time) ─── */

static uint32_t s_crc[256];

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    if (!s_crc[1]) {
        uint32_t i, k, v;
        for (i = 0; i < 256; i++) {
            for (v = i, k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            s_crc[i] = v;
        }
    }
    while (n--) c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void w16(FILE *f, uint32_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

int report_zip_dir(const WCHAR *dir, const WCHAR *zip_path, const char *prefix)
{
    typedef struct { char name[MAX_PATH * 3 + 64]; uint32_t crc, size, offset; } Entry;
    DirList l;
    Entry *e;
    FILE *z;
    SYSTEMTIME st;
    uint16_t t, d;
    int i, ok = 1;
    uint32_t start, end;
    dir_list(dir, &l);
    if (!l.n || !(e = (Entry *)calloc((size_t)l.n, sizeof *e))) { free(l.f); return 0; }
    if (_wfopen_s(&z, zip_path, L"wb") || !z) { free(e); free(l.f); return 0; }
    GetLocalTime(&st);
    t = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    d = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    for (i = 0; i < l.n && ok; i++) {
        WCHAR full[MAX_PATH];
        size_t n, nl;
        uint8_t *data;
        char *p;
        swprintf_s(full, MAX_PATH, L"%s\\%s", dir, l.f[i].rel);
        if (!(data = read_all(full, &n, 0x7FFFFFFF))) { ok = 0; break; }
        snprintf(e[i].name, sizeof e[i].name, "%s", prefix ? prefix : "");
        nl = strlen(e[i].name);
        WideCharToMultiByte(CP_UTF8, 0, l.f[i].rel, -1, e[i].name + nl, (int)(sizeof e[i].name - nl), NULL, NULL);
        for (p = e[i].name; *p; p++) if (*p == '\\') *p = '/';
        nl = strlen(e[i].name);
        e[i].crc = crc32_of(data, n);
        e[i].size = (uint32_t)n;
        e[i].offset = (uint32_t)_ftelli64(z);
        w32(z, 0x04034b50); w16(z, 10); w16(z, 0x0800); w16(z, 0); w16(z, t); w16(z, d);
        w32(z, e[i].crc); w32(z, e[i].size); w32(z, e[i].size); w16(z, (uint32_t)nl); w16(z, 0);
        fwrite(e[i].name, 1, nl, z);
        if (fwrite(data, 1, n, z) != n) ok = 0;
        free(data);
    }
    start = (uint32_t)_ftelli64(z);
    for (i = 0; i < l.n && ok; i++) {
        size_t nl = strlen(e[i].name);
        w32(z, 0x02014b50); w16(z, 20); w16(z, 10); w16(z, 0x0800); w16(z, 0); w16(z, t); w16(z, d);
        w32(z, e[i].crc); w32(z, e[i].size); w32(z, e[i].size); w16(z, (uint32_t)nl);
        w16(z, 0); w16(z, 0); w16(z, 0); w16(z, 0); w32(z, 0); w32(z, e[i].offset);
        fwrite(e[i].name, 1, nl, z);
    }
    end = (uint32_t)_ftelli64(z);
    w32(z, 0x06054b50); w16(z, 0); w16(z, 0); w16(z, (uint32_t)l.n); w16(z, (uint32_t)l.n);
    w32(z, end - start); w32(z, start); w16(z, 0);
    if (ferror(z)) ok = 0;
    fclose(z);
    if (!ok) DeleteFileW(zip_path);
    free(e);
    free(l.f);
    return ok;
}

/* ── match.json / mods.txt ───────────────────────────────────────────────── */

/* The string value of "key" in a JSON text at or after `from` (first match; escapes kept simple). */
static int json_str(const char *j, const char *key, char *out, int n)
{
    char pat[64];
    const char *p;
    int k = 0;
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!j || !(p = strstr(j, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k + 1 < n; p++) {
            if (*p == '\\' && p[1]) p++;
            out[k++] = *p;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && k + 1 < n)
            out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

/* The picker's line: "THE SANDMAN vs EDGE - RAW, ONE ON ONE NORMAL - 26 Oct 01:30, 5:12 (won: EDGE)". */
static void match_label(const WCHAR *dir, const WCHAR *name, WCHAR *out, int n);
void report_match_label(const WCHAR *dir, WCHAR *out, int n)
{
    const WCHAR *name = wcsrchr(dir, L'\\');
    match_label(dir, name ? name + 1 : dir, out, n);
}

static void match_label(const WCHAR *dir, const WCHAR *name, WCHAR *out, int n)
{
    WCHAR json_path[MAX_PATH], done[MAX_PATH];
    size_t len;
    uint8_t *j;
    char people[160] = "", arena[80] = "", rule[80] = "", start[40] = "", length[20] = "", result[60] = "", line[400];
    swprintf_s(json_path, MAX_PATH, L"%s\\match.json", dir);
    swprintf_s(done, MAX_PATH, L"%s\\done.txt", dir);
    j = read_all(json_path, &len, 4u << 20);
    if (j) {
        const char *pp = strstr((const char *)j, "\"people\"");
        const char *ar = strstr((const char *)j, "\"arena\"");
        const char *ru = strstr((const char *)j, "\"rule\"");
        if (pp && (pp = strchr(pp, '[')) != NULL) {
            /* (each entry {index, person, name, ...} of the array - the wrestlers only: "person" 6 is the referee,
               7 and 8 the announcers) */
            const char *p = pp + 1;
            while (*p && *p != ']') {
                const char *open = strchr(p, '{'), *q;
                char obj[1024], nm[60], person[16];
                int depth = 0;
                size_t k = 0;
                if (!open) break;
                for (q = open; *q; q++) {
                    if (*q == '{') depth++;
                    else if (*q == '}' && --depth == 0) break;
                    if (k + 1 < sizeof obj) obj[k++] = *q;
                }
                obj[k] = 0;
                if (!*q) break;
                p = q + 1;
                while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
                if (json_str(obj, "person", person, sizeof person) && atoi(person) >= 6) continue;
                if (!json_str(obj, "name", nm, sizeof nm) || !nm[0]) continue;
                if (people[0]) strncat_s(people, sizeof people, " vs ", _TRUNCATE);
                strncat_s(people, sizeof people, nm, _TRUNCATE);
            }
        }
        if (ar) json_str(ar, "name", arena, sizeof arena);
        if (ru) json_str(ru, "name", rule, sizeof rule);
        json_str((const char *)j, "start", start, sizeof start);
        json_str((const char *)j, "length_s", length, sizeof length);
        json_str((const char *)j, "result", result, sizeof result);
        free(j);
    }
    if (!people[0]) {   /* (no match.json yet: the folder's name) */
        WideCharToMultiByte(CP_UTF8, 0, name, -1, people, sizeof people, NULL, NULL);
    }
    {
        char when[40] = "";
        int secs = atoi(length);
        if (strlen(start) >= 16)   /* 2026-10-26T01:30:00 -> 10-26 01:30 */
            snprintf(when, sizeof when, "%.5s %.5s", start + 5, start + 11);
        snprintf(line, sizeof line, "%s%s%s%s%s - %s%s%s%s", people, arena[0] ? " - " : "", arena,
                 rule[0] ? ", " : "", rule, when[0] ? when : "?", secs > 0 ? ", " : "", "", "");
        if (secs > 0) {
            char dur[16];
            snprintf(dur, sizeof dur, "%d:%02d", secs / 60, secs % 60);
            strncat_s(line, sizeof line, dur, _TRUNCATE);
        }
        /* (result: "running", "ended" or "crash" - docs/MATCH_RECORDINGS.md; no done.txt: not complete) */
        if (!strcmp(result, "crash")) strncat_s(line, sizeof line, " (crashed)", _TRUNCATE);
        else if (GetFileAttributesW(done) == INVALID_FILE_ATTRIBUTES) strncat_s(line, sizeof line, " (unfinished)", _TRUNCATE);
        else if (result[0] && strcmp(result, "ended") && strcmp(result, "running")) {
            strncat_s(line, sizeof line, " (", _TRUNCATE);
            strncat_s(line, sizeof line, result, _TRUNCATE);
            strncat_s(line, sizeof line, ")", _TRUNCATE);
        }
    }
    MultiByteToWideChar(CP_UTF8, 0, line, -1, out, n);
}

/* (newest first by the folder's name - it starts with the match's start, YYYYMMDD-HHMMSS - then by its time) */
static int match_newer(const void *a, const void *b)
{
    const ReportMatch *x = (const ReportMatch *)a, *y = (const ReportMatch *)b;
    const WCHAR *nx = wcsrchr(x->dir, L'\\'), *ny = wcsrchr(y->dir, L'\\');
    const int c = wcscmp(ny ? ny : y->dir, nx ? nx : x->dir);
    return c ? c : CompareFileTime(&y->time, &x->time);
}

int report_match_list(const WCHAR *game_dir, ReportMatch *out, int max)
{
    static ReportMatch all[256];
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, i;
    swprintf_s(pat, MAX_PATH, L"%s\\logs\\matches\\*", game_dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || n >= 256) continue;
        swprintf_s(all[n].dir, MAX_PATH, L"%s\\logs\\matches\\%s", game_dir, fd.cFileName);
        all[n].time = fd.ftCreationTime;
        all[n].label[0] = 0;
        wcsncpy_s(all[n].label, 200, fd.cFileName, _TRUNCATE);   /* (the name: the label below needs the folder) */
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(all, (size_t)n, sizeof *all, match_newer);
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        WCHAR name[200];
        wcscpy_s(name, 200, all[i].label);
        out[i] = all[i];
        match_label(all[i].dir, name, out[i].label, 200);
    }
    return n;
}

/* mods.txt: "<Type>\t<id>\t<version>\t<on|off>\t<used|->\t<fnv64>\t<name>" a line, in load order; only the
   ones the match used. (A line of the first proposal, "<Type>\t<id>\t<version>\t<name>", counts as used.) */
int report_match_mods(const WCHAR *game_dir, const WCHAR *match_dir, ReportMod *out, int max)
{
    WCHAR path[MAX_PATH];
    size_t len;
    uint8_t *d;
    char *line, *next;
    int n = 0;
    swprintf_s(path, MAX_PATH, L"%s\\mods.txt", match_dir);
    swprintf_s(s_cache_file, MAX_PATH, L"%s\\Reports", game_dir);
    CreateDirectoryW(s_cache_file, NULL);
    wcscat_s(s_cache_file, MAX_PATH, L"\\.modhash");
    if (!(d = read_all(path, &len, 1u << 20))) return 0;
    for (line = (char *)d; line && *line && n < max; line = next) {
        char *col[7] = {0};
        int k = 0;
        char *p, *cr;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if ((cr = strchr(line, '\r')) != NULL) *cr = 0;
        if (!*line || *line == '#') continue;
        for (p = line; k < 7; k++) {
            col[k] = p;
            if (!(p = strchr(p, '\t'))) { k++; break; }
            *p++ = 0;
        }
        if (k < 3) continue;
        if (k >= 7 && strcmp(col[4], "used")) continue;   /* (loaded, not used by this match) */
        {
            ReportMod *m = &out[n];
            ZeroMemory(m, sizeof *m);
            MultiByteToWideChar(CP_UTF8, 0, col[0], -1, m->type, 32);
            MultiByteToWideChar(CP_UTF8, 0, col[1], -1, m->id, 128);
            strncpy_s(m->version, sizeof m->version, col[2], _TRUNCATE);
            strncpy_s(m->name, sizeof m->name, k >= 7 ? col[6] : (k >= 4 ? col[3] : col[1]), _TRUNCATE);
            if (wcschr(m->type, L'\\') || wcschr(m->id, L'\\') || wcsstr(m->id, L"..") || !m->type[0] || !m->id[0])
                continue;   /* (a name, not a path) */
            swprintf_s(m->folder, MAX_PATH, L"%s\\Mods\\%s\\%s", game_dir, m->type, m->id);
            if (GetFileAttributesW(m->folder) == INVALID_FILE_ATTRIBUTES || !report_mod_hash(m->folder, m->hash, &m->size))
                m->folder[0] = 0, m->hash[0] = 0;
            m->send = m->hash[0] != 0;
            n++;
        }
    }
    free(d);
    return n;
}

/* ── the hash check ──────────────────────────────────────────────────────── */

static void jesc(char *out, size_t n, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < n; s++) {
        if (*s == '"' || *s == '\\') out[k++] = '\\', out[k++] = *s;
        else if ((unsigned char)*s < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", (unsigned char)*s);
        else out[k++] = *s;
    }
    out[k] = 0;
}

int report_mods_check(const WCHAR *server, ReportMod *mods, int n, uint64_t *max_upload, WCHAR *why, int why_n)
{
    char *body, *answer = NULL;
    size_t at = 0, cap = 256 + (size_t)n * 700, total = 0, acap = 0;
    WCHAR url[1000], host[256], upath[900], headers[600];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status, got = 0;
    int i, asked = 0;
    *max_upload = 0;
    why[0] = 0;
    for (i = 0; i < n; i++) mods[i].on_server = 0;
    if (!server || !server[0] || !(body = (char *)malloc(cap))) { wcscpy_s(why, why_n, L"No server is set."); return 0; }
    at += (size_t)snprintf(body + at, cap - at, "{\"mods\":[");
    for (i = 0; i < n; i++) {
        char name[300], type[80], id[300], ver[80], t[260];
        if (!mods[i].hash[0]) continue;
        WideCharToMultiByte(CP_UTF8, 0, mods[i].type, -1, t, sizeof t, NULL, NULL); jesc(type, sizeof type, t);
        WideCharToMultiByte(CP_UTF8, 0, mods[i].id, -1, t, sizeof t, NULL, NULL); jesc(id, sizeof id, t);
        jesc(name, sizeof name, mods[i].name);
        jesc(ver, sizeof ver, mods[i].version);
        at += (size_t)snprintf(body + at, cap - at,
                               "%s{\"hash\":\"%s\",\"size\":%llu,\"name\":\"%s\",\"type\":\"%s\",\"id\":\"%s\",\"version\":\"%s\"}",
                               at > 9 ? "," : "", mods[i].hash, (unsigned long long)mods[i].size, name, type, id, ver);
    }
    at += (size_t)snprintf(body + at, cap - at, "]}");
    swprintf_s(url, 1000, L"%s%s", wcsstr(server, L"://") ? L"" : L"http://", server);
    while (wcslen(url) && url[wcslen(url) - 1] == L'/') url[wcslen(url) - 1] = 0;
    wcscat_s(url, 1000, L"/api/reports/mods/check");
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (WinHttpCrackUrl(url, 0, 0, &uc) &&
        (ses = WinHttpOpen(L"SvR2011 Report", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0)) != NULL) {
        WinHttpSetTimeouts(ses, 10000, 15000, 30000, 30000);
        if ((con = WinHttpConnect(ses, host, uc.nPort, 0)) != NULL)
            req = WinHttpOpenRequest(con, L"POST", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    {   /* (2.2.1: the sign-in - the server asks a signed-out sender for no mods: its report would be refused) */
        const char *token = report_auth();
        swprintf_s(headers, 600, L"Content-Type: application/json; charset=utf-8\r\n%s%S%s",
                   token[0] ? L"Authorization: Bearer " : L"", token, token[0] ? L"\r\n" : L"");
    }
    if (req && WinHttpSendRequest(req, headers, (DWORD)-1L, body, (DWORD)at, (DWORD)at, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            char *m;
            if (total + 4097 > acap) {
                acap = acap ? acap * 2 : 8192;
                if (acap > (1u << 20) || !(m = (char *)realloc(answer, acap))) break;
                answer = m;
            }
            if (!WinHttpReadData(req, answer + total, 4096, &got) || !got) break;
            total += got;
        }
        if (answer) answer[total] = 0;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(body);
    if (status == 200 && answer) {
        const char *missing = strstr(answer, "\"missing\"");
        char v[40];
        asked = 1;
        if (json_str(answer, "max_upload", v, sizeof v)) *max_upload = _strtoui64(v, NULL, 10);
        if (json_str(answer, "mods_off", v, sizeof v) && !strcmp(v, "true")) {   /* (the server's mod store is full / off) */
            for (i = 0; i < n; i++) mods[i].on_server = 0, mods[i].send = 0;
            wcscpy_s(why, why_n, L"The server takes no mods right now.");
            free(answer);
            return 1;
        }
        for (i = 0; i < n; i++) {   /* (on the server: a hash not in "missing") */
            const char *end = missing ? strchr(missing, ']') : NULL;
            const char *hit = mods[i].hash[0] && missing ? strstr(missing, mods[i].hash) : NULL;
            mods[i].on_server = mods[i].hash[0] && missing && !(hit && (!end || hit < end));
        }
    } else if (!status) {
        wcscpy_s(why, why_n, L"The server can't be reached.");
    } else {
        swprintf_s(why, why_n, L"The server didn't answer the mod check (HTTP %lu).", status);
    }
    free(answer);
    return asked;
}
