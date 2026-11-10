/* WWE SmackDown vs. Raw 2011 PC launcher - entrance number clashes between installed mods (entrance_clash.h). */
#include "entrance_clash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* 2011's own numbers: main entrance numbers (pac/evt/Nyujyo*.pac EVP*, below 10000) and victory scenes
   (pac/evt/Winning*.pac EVP*) - tools/entrance_port.py stock_2011 / stock_victories on the game's pacs. */
static const unsigned short k_stock_entrances[] = {
    50, 100, 101, 102, 103, 104, 107, 110, 112, 115, 117, 122, 123, 125, 131, 132,
    135, 137, 139, 143, 145, 158, 160, 161, 164, 165, 169, 174, 175, 176, 177, 178,
    179, 182, 201, 205, 208, 218, 219, 224, 261, 262, 263, 265, 267, 268, 271, 272,
    275, 276, 277, 278, 279, 281, 282, 283, 284, 290, 291, 292, 293, 294, 295, 297,
    305, 306, 321, 400, 401, 402, 410, 480, 481, 482, 483, 484, 485, 486, 487, 500,
    501, 502, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 518,
    519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534,
    535, 536, 537, 538, 539, 540, 541, 543, 544, 545, 546, 547, 548, 549, 552, 554,
    555, 556, 557, 558, 560, 562, 563, 600, 601, 602, 603, 604, 605, 606, 607, 608,
    609, 610, 611, 612, 613, 614, 615, 616, 617, 618, 620, 621, 622, 628, 629, 630,
    631, 632, 633, 634, 637, 638, 642, 643, 644, 645, 646, 647, 648, 649, 650, 651,
    652, 700, 701, 702, 703, 704, 705, 706, 707, 708, 709, 710, 711, 713, 714, 715,
    716, 720, 721, 722, 723, 724, 725, 726, 727, 728, 729, 730, 732, 733, 735, 736,
    750, 751, 752, 753, 754, 755, 756, 770, 771, 772, 773, 774, 775, 800, 803, 807,
    808, 809, 812, 813, 816, 817, 820, 821, 822, 823, 824, 830, 833, 840, 850
};
static const unsigned short k_stock_victories[] = {
    6050, 6100, 6101, 6102, 6103, 6104, 6107, 6110, 6112, 6123, 6125, 6131, 6135, 6139, 6145, 6160,
    6161, 6175, 6176, 6179, 6219, 6262, 6272, 6277, 6279, 6700, 6702, 7000, 7001, 7002, 7003, 7004,
    7005, 7006, 7007, 7008, 7009, 7010, 7011, 7012, 7013, 7014, 7015, 7100, 7101, 7102, 7103, 7104,
    7105, 7106, 7107, 7150, 7151, 7152, 7153, 7154, 7155, 7156, 7158, 7200, 7201, 7202, 7203, 7204,
    7205, 7206, 7207, 7208, 7300, 7301, 7400, 7401, 7402, 7403, 7404, 7500, 7501, 7502, 7503, 7600,
    7601, 7602, 7603, 7604, 7605, 8000, 8020, 8030, 8040, 8041, 8050, 8051, 8052, 8060, 8061, 8062,
    8100, 8101, 8102, 8103, 8200, 8201, 8202, 8203, 8300, 8301, 8400, 8401, 8410, 8411, 8412, 8500,
    9000, 9001, 9002, 9003, 9004, 9005, 9100, 9101, 9102, 9103, 9200, 9201, 9202, 9203, 9400
};

enum { K_ENTRANCE, K_VICTORY, MAX_LINES = 512, MAX_USES = 4096, MAX_MOVES = 32 };

typedef struct {
    char text[600];        /* the line as read (no line end) */
    int kind, base, num;   /* kind -1: not an entrance / victory line; num: its name number */
    int pyro;              /* (prtcls.pac PCE6 / LPE6 <0nnn>) */
    char pac[96], group[16], file[400];
} Line;

typedef struct {
    WCHAR dir[MAX_PATH];   /* the pack's folder */
    Line *lines;
    int n;
} Pack;

typedef struct {
    int kind, base;
    unsigned long long hash;
    WCHAR owner[MAX_PATH];   /* the mod folder */
} Use;

static unsigned long long fnv(const void *p, size_t n, unsigned long long h)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static unsigned char *read_all(const WCHAR *path, size_t *n)
{
    FILE *f;
    unsigned char *d;
    long sz;
    *n = 0;
    if (_wfopen_s(&f, path, L"rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || !(d = (unsigned char *)malloc((size_t)sz + 1))) { fclose(f); return NULL; }
    *n = fread(d, 1, (size_t)sz, f);
    d[*n] = 0;
    fclose(f);
    return d;
}

static int write_all(const WCHAR *path, const void *d, size_t n)
{
    FILE *f;
    int ok;
    if (_wfopen_s(&f, path, L"wb") || !f) return 0;
    ok = fwrite(d, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* "evt\NyujyoN.pac" (either slash, any case) -> 1 */
static int pac_is(const char *pac, const char *stem)
{
    char low[96];
    size_t i;
    for (i = 0; pac[i] && i + 1 < sizeof low; i++) low[i] = (char)(pac[i] == '/' ? '\\' : (pac[i] >= 'A' && pac[i] <= 'Z' ? pac[i] + 32 : pac[i]));
    low[i] = 0;
    return !strncmp(low, stem, strlen(stem));
}

static void classify(Line *l)
{
    char kw[16] = "", name[24] = "";
    int got;
    l->kind = -1;
    l->pyro = 0;
    got = sscanf_s(l->text, " %15s %95s %15s %23s %399[^\r\n]", kw, (unsigned)sizeof kw, l->pac, (unsigned)sizeof l->pac,
                   l->group, (unsigned)sizeof l->group, name, (unsigned)sizeof name, l->file, (unsigned)sizeof l->file);
    if (got < 5 || _stricmp(kw, "pacentry") || !digits(name)) return;
    l->num = atoi(name);
    if (pac_is(l->pac, "evt\\nyujyo") && (!_strnicmp(l->group, "EVP", 3) || !_strnicmp(l->group, "EVT", 3)) &&
        strlen(l->group) == 4) {
        l->kind = K_ENTRANCE;
        l->base = l->num >= 10000 ? l->num % 1000 : l->num;
    } else if (pac_is(l->pac, "evt\\winning") && (!_stricmp(l->group, "EVPA") || !_stricmp(l->group, "EVTA"))) {
        l->kind = K_VICTORY;
        l->base = l->num;
    } else if (pac_is(l->pac, "prtcls.pac") && (!_stricmp(l->group, "PCE6") || !_stricmp(l->group, "LPE6")) &&
               strlen(name) == 4) {
        l->kind = K_ENTRANCE;
        l->base = l->num;
        l->pyro = 1;
    }
}

static int load_pack(const WCHAR *dir, Pack *p)
{
    WCHAR path[MAX_PATH];
    size_t n, a = 0;
    unsigned char *d;
    swprintf_s(path, MAX_PATH, L"%s\\pack.txt", dir);
    ZeroMemory(p, sizeof *p);
    int cap = MAX_LINES;
    if (!(d = read_all(path, &n))) return 0;
    if (!(p->lines = (Line *)calloc((size_t)cap, sizeof(Line)))) { free(d); return 0; }
    wcscpy_s(p->dir, MAX_PATH, dir);
    while (a < n) {   /* (every line: a superstar's pack has its motions' lines before the entrance's) */
        size_t b = a, len;
        Line *l;
        if (p->n == cap) {
            Line *more = (Line *)realloc(p->lines, (size_t)cap * 2 * sizeof(Line));
            if (!more) break;
            p->lines = more;
            cap *= 2;
        }
        l = &p->lines[p->n];
        ZeroMemory(l, sizeof *l);
        while (b < n && d[b] != '\n') b++;
        len = b - a;
        if (len && d[a + len - 1] == '\r') len--;
        if (len >= sizeof l->text) len = sizeof l->text - 1;
        memcpy(l->text, d + a, len);
        l->text[len] = 0;
        classify(l);
        p->n++;
        a = b + 1;
    }
    free(d);
    return 1;
}

/* A line's share of its number's hash: its group and part (the name past the number) and its file's bytes. */
static unsigned long long line_hash(const Pack *p, const Line *l)
{
    WCHAR f[MAX_PATH], wf[400];
    size_t n, i;
    unsigned char *d;
    unsigned long long h = 1469598103934665603ull;
    char g[8];
    const int part = l->num >= 10000 ? (l->num - 10000) / 1000 : -1;
    for (i = 0; i < 3 && l->group[i]; i++) g[i] = (char)(l->group[i] & ~0x20);   /* (EVP / EVT / PCE / LPE: not the range letter) */
    g[i] = 0;
    h = fnv(g, strlen(g), h);
    h = fnv(&part, sizeof part, h);
    MultiByteToWideChar(CP_UTF8, 0, l->file, -1, wf, 400);
    for (i = 0; wf[i]; i++) if (wf[i] == L'/') wf[i] = L'\\';
    swprintf_s(f, MAX_PATH, L"%s\\%s", p->dir, wf);
    if ((d = read_all(f, &n)) != NULL) {
        if (n > 0x22 && !memcmp(d, "0FOP", 4)) {   /* (the script's own number aside: a moved copy is the same entrance) */
            d[0x20] = d[0x21] = 0;
            {
                const unsigned tp = (unsigned)d[0x1C] << 24 | (unsigned)d[0x1D] << 16 | (unsigned)d[0x1E] << 8 | d[0x1F];
                if (tp && tp + 10 <= n) d[tp + 8] = d[tp + 9] = 0;
            }
        }
        h = fnv(d, n, h);
        free(d);
    }
    return h;
}

/* The packs of a mod folder: its pack.txt (a move pack) and moves\pack.txt (a superstar mod's). */
static int mod_packs(const WCHAR *folder, Pack out[2])
{
    WCHAR d[MAX_PATH];
    int k = 0;
    if (load_pack(folder, &out[k])) k++;
    swprintf_s(d, MAX_PATH, L"%s\\moves", folder);
    if (load_pack(d, &out[k])) k++;
    return k;
}

static void free_packs(Pack *p, int n)
{
    int i;
    for (i = 0; i < n; i++) free(p[i].lines);
}

/* Adds a folder's numbers (kind, base -> the sum of its lines' hashes) to uses. */
static void add_uses(const WCHAR *folder, Pack *packs, int np, Use *uses, int *nu)
{
    int i, j, k;
    for (i = 0; i < np; i++)
        for (j = 0; j < packs[i].n; j++) {
            const Line *l = &packs[i].lines[j];
            if (l->kind < 0) continue;
            for (k = 0; k < *nu; k++)
                if (uses[k].kind == l->kind && uses[k].base == l->base && !_wcsicmp(uses[k].owner, folder)) break;
            if (k == *nu) {
                if (*nu >= MAX_USES) continue;
                uses[k].kind = l->kind;
                uses[k].base = l->base;
                uses[k].hash = 0;
                wcscpy_s(uses[k].owner, MAX_PATH, folder);
                (*nu)++;
            }
            uses[k].hash += line_hash(&packs[i], l);   /* (a sum: the lines' order doesn't matter) */
        }
}

static void manifest_name(const WCHAR *folder, WCHAR *out, size_t n)
{
    WCHAR path[MAX_PATH], line[400];
    FILE *f;
    const WCHAR *slash = wcsrchr(folder, L'\\');
    wcscpy_s(out, n, slash ? slash + 1 : folder);
    swprintf_s(path, MAX_PATH, L"%s\\manifest.txt", folder);
    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") || !f) return;
    while (fgetws(line, 400, f))
        if (!wcsncmp(line, L"name=", 5)) {
            size_t k = wcslen(line);
            while (k && (line[k - 1] == L'\n' || line[k - 1] == L'\r')) line[--k] = 0;
            if (line[5]) wcscpy_s(out, n, line + 5);
            break;
        }
    fclose(f);
}

static int stock(int kind, int n)
{
    const unsigned short *t = kind == K_ENTRANCE ? k_stock_entrances : k_stock_victories;
    const size_t c = kind == K_ENTRANCE ? sizeof k_stock_entrances / sizeof *k_stock_entrances
                                        : sizeof k_stock_victories / sizeof *k_stock_victories;
    size_t i;
    for (i = 0; i < c; i++) if (t[i] == n) return 1;
    return 0;
}

/* The number's range: entrances by kind (tag 700-769, trio 770-799, singles 100-999 outside them), victory
   scenes 6300-6999 (past the superstars' own 6000 + id). */
static void range_of(int kind, int base, int *lo, int *hi)
{
    if (kind == K_VICTORY) { *lo = 6300; *hi = 6999; return; }
    if (base >= 700 && base <= 769) { *lo = 700; *hi = 769; return; }
    if (base >= 770 && base <= 799) { *lo = 770; *hi = 799; return; }
    *lo = 100; *hi = 999;
}

typedef struct { int kind, from, to, clash; WCHAR other[128]; } Move;

static int taken(int kind, int n, const Use *uses, int nu, const Use *mine, int nm, const Move *mv, int nmv, int self)
{
    int i;
    if (stock(kind, n)) return 1;
    for (i = 0; i < nu; i++) if (uses[i].kind == kind && uses[i].base == n) return 1;
    for (i = 0; i < nm; i++) if (mine[i].kind == kind && mine[i].base == n && n != self) return 1;
    for (i = 0; i < nmv; i++) if (mv[i].kind == kind && mv[i].to == n) return 1;
    return 0;
}

static int free_number(int kind, int base, const Use *uses, int nu, const Use *mine, int nm, const Move *mv, int nmv)
{
    int lo, hi, n;
    range_of(kind, base, &lo, &hi);
    for (n = lo; n <= hi; n++) {
        if (kind == K_ENTRANCE && lo == 100 && n >= 700 && n <= 799) continue;
        if (!taken(kind, n, uses, nu, mine, nm, mv, nmv, -1)) return n;
    }
    return 0;
}

/* The pac and range letter of an entrance number (Yuke's Nyujyo ranges: tools/hctp_entrance.py nyujyo_for). */
static void nyujyo_for(int n, char *pac_digit, char *group_letter)
{
    static const struct { int lo, hi; char f, g; } r[] = {
        { 100, 134, '0', '1' }, { 135, 178, '1', '2' }, { 179, 271, '2', '3' }, { 272, 290, '3', '4' },
        { 291, 479, '4', '5' }, { 480, 511, '5', 'E' }, { 512, 531, '6', 'F' }, { 532, 551, '7', 'G' },
        { 552, 599, '8', 'H' }, { 600, 999, '9', 'I' } };
    size_t i;
    *pac_digit = '9';
    *group_letter = 'I';
    for (i = 0; i < sizeof r / sizeof *r; i++)
        if (n >= r[i].lo && n <= r[i].hi) { *pac_digit = r[i].f; *group_letter = r[i].g; return; }
}

/* An EVP's own number (u16 at 0x20; the tail block's first u16), old -> new. */
static void patch_evp(const WCHAR *path, int old_num, int new_num)
{
    size_t n;
    unsigned char *d = read_all(path, &n);
    int changed = 0;
    if (!d) return;
    if (n > 0x22 && !memcmp(d, "0FOP", 4)) {
        const unsigned tp = (unsigned)d[0x1C] << 24 | (unsigned)d[0x1D] << 16 | (unsigned)d[0x1E] << 8 | d[0x1F];
        if ((d[0x20] << 8 | d[0x21]) == old_num) {
            d[0x20] = (unsigned char)(new_num >> 8);
            d[0x21] = (unsigned char)new_num;
            changed = 1;
        }
        if (tp && tp + 10 <= n && (d[tp + 8] << 8 | d[tp + 9]) == old_num) {
            d[tp + 8] = (unsigned char)(new_num >> 8);
            d[tp + 9] = (unsigned char)new_num;
            changed = 1;
        }
    }
    if (changed) write_all(path, d, n);
    free(d);
}

/* The pack's lines of a moved number rewritten (name, pac and range letters), its scripts' numbers patched. */
static void move_in_pack(Pack *p, const Move *m)
{
    int j, any = 0;
    for (j = 0; j < p->n; j++) {
        Line *l = &p->lines[j];
        int nn;
        if (l->kind != m->kind || l->base != m->from) continue;
        nn = l->pyro ? m->to : l->num >= 10000 ? 10000 + 1000 * ((l->num - 10000) / 1000) + m->to : m->to;
        if (!l->pyro && (!_strnicmp(l->group, "EVP", 3))) {
            WCHAR f[MAX_PATH], wf[400];
            size_t i;
            MultiByteToWideChar(CP_UTF8, 0, l->file, -1, wf, 400);
            for (i = 0; wf[i]; i++) if (wf[i] == L'/') wf[i] = L'\\';
            swprintf_s(f, MAX_PATH, L"%s\\%s", p->dir, wf);
            patch_evp(f, l->num, nn);
        }
        if (l->kind == K_ENTRANCE && !l->pyro) {
            char f, g, grp[8];
            nyujyo_for(m->to, &f, &g);
            sprintf_s(l->pac, sizeof l->pac, "evt\\Nyujyo%c.pac", f);
            sprintf_s(grp, sizeof grp, "%.3s%c", l->group, g);
            strcpy_s(l->group, sizeof l->group, grp);
        }
        if (l->pyro) sprintf_s(l->text, sizeof l->text, "pacentry %s %s %04d %s", l->pac, l->group, nn, l->file);
        else sprintf_s(l->text, sizeof l->text, "pacentry %s %s %d %s", l->pac, l->group, nn, l->file);
        l->num = nn;
        any = 1;
    }
    if (any) {
        WCHAR path[MAX_PATH];
        FILE *f;
        swprintf_s(path, MAX_PATH, L"%s\\pack.txt", p->dir);
        if (!_wfopen_s(&f, path, L"wb") && f) {
            for (j = 0; j < p->n; j++) fprintf(f, "%s\r\n", p->lines[j].text);
            fclose(f);
        }
    }
}

/* manifest.txt: entrance= / attire1..4_entrance= (entrances), victory= (victory scenes) of a moved number. */
static void move_in_manifest(const WCHAR *folder, const Move *mv, int nmv)
{
    WCHAR path[MAX_PATH];
    size_t n, a = 0;
    unsigned char *d;
    char *out;
    size_t o = 0;
    int changed = 0;
    swprintf_s(path, MAX_PATH, L"%s\\manifest.txt", folder);
    if (!(d = read_all(path, &n))) return;
    if (!(out = (char *)malloc(n + 64 * MAX_MOVES + 16))) { free(d); return; }
    while (a < n) {
        size_t b = a, len;
        char line[512];
        while (b < n && d[b] != '\n') b++;
        len = b - a;
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, d + a, len);
        line[len] = 0;
        {
            char *eq = strchr(line, '=');
            if (eq) {
                const size_t kl = (size_t)(eq - line);
                int kind = -1, i;
                if ((kl == 8 && !strncmp(line, "entrance", 8)) ||
                    (kl == 15 && !strncmp(line, "attire", 6) && line[6] >= '1' && line[6] <= '4' && !strncmp(line + 7, "_entrance", 8)))
                    kind = K_ENTRANCE;
                else if (kl == 7 && !strncmp(line, "victory", 7))
                    kind = K_VICTORY;
                if (kind >= 0) {
                    const int v = atoi(eq + 1);
                    for (i = 0; i < nmv; i++)
                        if (mv[i].kind == kind && mv[i].from == v) {
                            const int cr = len && line[len - 1] == '\r';
                            sprintf_s(eq + 1, sizeof line - kl - 1, "%d%s", mv[i].to, cr ? "\r" : "");
                            changed = 1;
                            break;
                        }
                }
            }
        }
        memcpy(out + o, line, strlen(line));
        o += strlen(line);
        if (b < n) out[o++] = '\n';
        a = b + 1;
    }
    if (changed) write_all(path, out, o);
    free(out);
    free(d);
}

/* The old copy's moves (dst\.entrance_moves: "entrance <from> <to>" / "victory <from> <to>"). */
static int read_record(const WCHAR *dst, Move *rec, int max)
{
    WCHAR path[MAX_PATH];
    size_t n;
    unsigned char *d;
    char *s, *ctx = NULL;
    int k = 0;
    if (!dst || !dst[0]) return 0;
    swprintf_s(path, MAX_PATH, L"%s\\.entrance_moves", dst);
    if (!(d = read_all(path, &n))) return 0;
    for (s = strtok_s((char *)d, "\r\n", &ctx); s && k < max; s = strtok_s(NULL, "\r\n", &ctx)) {
        char w[16];
        int from, to;
        if (sscanf_s(s, "%15s %d %d", w, (unsigned)sizeof w, &from, &to) == 3) {
            ZeroMemory(&rec[k], sizeof rec[k]);
            rec[k].kind = !strcmp(w, "victory") ? K_VICTORY : K_ENTRANCE;
            rec[k].from = from;
            rec[k].to = to;
            k++;
        }
    }
    free(d);
    return k;
}

/* Every installed mod folder with a pack (Mods\<type>\<mod>), dst and disabled ones left out. */
static void installed_uses(const WCHAR *game, const WCHAR *dst, Use *uses, int *nu)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW t, m;
    HANDLE ht, hm;
    swprintf_s(pat, MAX_PATH, L"%s\\Mods\\*", game);
    if ((ht = FindFirstFileW(pat, &t)) == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR tdir[MAX_PATH];
        if (!(t.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || t.cFileName[0] == L'.' ||
            !_wcsicmp(t.cFileName, L"PacOverlay"))
            continue;
        swprintf_s(tdir, MAX_PATH, L"%s\\Mods\\%s", game, t.cFileName);
        swprintf_s(pat, MAX_PATH, L"%s\\*", tdir);
        if ((hm = FindFirstFileW(pat, &m)) == INVALID_HANDLE_VALUE) continue;
        do {
            WCHAR folder[MAX_PATH], off[MAX_PATH];
            Pack packs[2];
            int np;
            if (!(m.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || m.cFileName[0] == L'.') continue;
            swprintf_s(folder, MAX_PATH, L"%s\\%s", tdir, m.cFileName);
            swprintf_s(off, MAX_PATH, L"%s\\disabled", folder);
            if ((dst && !_wcsicmp(folder, dst)) || GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) continue;
            if ((np = mod_packs(folder, packs)) > 0) {
                add_uses(folder, packs, np, uses, nu);
                free_packs(packs, np);
            }
        } while (FindNextFileW(hm, &m));
        FindClose(hm);
    } while (FindNextFileW(ht, &t));
    FindClose(ht);
}

int entrance_clash_fix(const WCHAR *game, const WCHAR *incoming, const WCHAR *dst, const WCHAR *name, WCHAR *report,
                       size_t report_n)
{
    Pack packs[2];
    Use *uses, *mine;
    int nu = 0, nm = 0, np, i, j, nmv = 0, nrec;
    Move mv[MAX_MOVES], rec[MAX_MOVES];
    if (report && report_n) report[0] = 0;
    if (!game || !game[0] || !incoming || (np = mod_packs(incoming, packs)) == 0) return 0;
    uses = (Use *)calloc(MAX_USES, sizeof(Use));
    mine = (Use *)calloc(256, sizeof(Use));
    if (!uses || !mine) { free(uses); free(mine); free_packs(packs, np); return 0; }
    add_uses(incoming, packs, np, mine, &nm);
    if (!nm) { free(uses); free(mine); free_packs(packs, np); return 0; }
    installed_uses(game, dst, uses, &nu);
    nrec = read_record(dst, rec, MAX_MOVES);
    for (i = 0; i < nm && nmv < MAX_MOVES; i++) {
        const Use *u = &mine[i];
        int clash = -1, prev = 0, to = 0;
        for (j = 0; j < nu; j++)
            if (uses[j].kind == u->kind && uses[j].base == u->base && uses[j].hash != u->hash) { clash = j; break; }
        for (j = 0; j < nrec; j++)
            if (rec[j].kind == u->kind && rec[j].from == u->base) prev = rec[j].to;
        if (prev && prev != u->base && !taken(u->kind, prev, uses, nu, mine, nm, mv, nmv, -1))
            to = prev;   /* (moved before, still free: kept - the player's saved entrance points there) */
        else if (clash >= 0)
            to = free_number(u->kind, u->base, uses, nu, mine, nm, mv, nmv);
        if (!to) continue;
        ZeroMemory(&mv[nmv], sizeof mv[nmv]);
        mv[nmv].kind = u->kind;
        mv[nmv].from = u->base;
        mv[nmv].to = to;
        mv[nmv].clash = clash >= 0 ? (to == prev ? 2 : 1) : 3;   /* (1 new number, 2 kept for a clash, 3 kept) */
        if (prev && to != prev) mv[nmv].clash = 4;                 /* (its old number was taken since) */
        if (clash >= 0) manifest_name(uses[clash].owner, mv[nmv].other, 128);
        nmv++;
    }
    for (i = 0; i < nmv; i++)
        for (j = 0; j < np; j++) move_in_pack(&packs[j], &mv[i]);
    if (nmv) move_in_manifest(incoming, mv, nmv);
    {   /* (the record, for the next update) */
        WCHAR path[MAX_PATH];
        FILE *f;
        swprintf_s(path, MAX_PATH, L"%s\\.entrance_moves", incoming);
        if (nmv && !_wfopen_s(&f, path, L"wb") && f) {
            for (i = 0; i < nmv; i++)
                fprintf(f, "%s %d %d\r\n", mv[i].kind == K_VICTORY ? "victory" : "entrance", mv[i].from, mv[i].to);
            fclose(f);
        }
    }
    for (i = 0; i < nmv && report; i++) {
        WCHAR t[600];
        const WCHAR *what = mv[i].kind == K_VICTORY ? L"victory scene" : L"entrance";
        if (mv[i].clash == 1 || mv[i].clash == 4)
            swprintf_s(t, 600, L"\"%s\" used %s %d, which \"%s\" already uses for a different %s - it now uses %d, so "
                               L"both play their own.%s", name, what, mv[i].from, mv[i].other, what, mv[i].to,
                       mv[i].clash == 4 ? L" (Its earlier number was taken since.)" : L"");
        else
            swprintf_s(t, 600, L"\"%s\" keeps %s %d, the number it was moved to before (its own %d %s).", name, what,
                       mv[i].to, mv[i].from, mv[i].clash == 2 ? L"is another mod's" : L"was moved");
        if (report[0]) wcscat_s(report, report_n, L"\r\n");
        wcsncat_s(report, report_n, t, _TRUNCATE);
    }
    free(uses);
    free(mine);
    free_packs(packs, np);
    return nmv;
}
