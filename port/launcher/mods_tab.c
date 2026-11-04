/* WWE SmackDown vs. Raw 2011 PC launcher - the Mods tab (arenas branch).
 *
 * Mods live in <game>/Mods/<Type>/<id>/ with a manifest.txt (key=value:
 * type, id, name, author, version). Arena mods hold arena.pac and banner.dds
 * (256 x 128 DXT5); the game lists them on the arena select pages after its
 * own arenas (src/arena_mods.cpp). Superstar mods hold ch.pac (and maybe a
 * theme song and an entrance movie); the game lists them under the M tile of
 * the character select (src/superstar_mods.cpp). Sign packs hold crowd signs
 * (*.dds, 128 x 64) the crowd holds up (src/crowd_signs.cpp); media packs replace
 * the game's videos, renders, arena screens and sounds (src/media_mods.cpp). Match type mods
 * (Mods/MatchTypes/<id>: a manifest only) switch on one of the match types the port adds -
 * Slobber Knocker, Three Stages of Hell, Elimination... (src/match_types.cpp); they come
 * switched on. Background mods (Mods/Backgrounds/<id>: picture.dds + manifest) are main menu
 * pictures (or a pack of them, NN_<name>.dds, from the Mod Maker); "Add background picture"
 * makes one from a PNG / JPG (backgrounds.c, src/backgrounds_page.cpp). A background's name
 * (what the game's BACKGROUNDS page shows) can be changed in the list (click it, or F2). A mod is turned off by a "disabled" file in its folder. A .svrmod file is a
 * zip of such a folder.
 *   +  installs a .svrmod/.zip   -  sends a mod's folder to the Recycle Bin
 */
#include "mods_tab.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <restartmanager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "backgrounds.h"
#include "move_warning.h"
#include "movie_maker.h"
#include "music_level.h"
#include "unzip.h"

enum { MODS_MAX = 512 };

typedef struct {
    WCHAR folder[MAX_PATH];  /* full path */
    WCHAR type[32], id[64], name[128], author[64], version[32];
    WCHAR made_with[64];  /* manifest made_with= (the Mod Maker stamps its mods); "" = not made with it */
    WCHAR needs[32];      /* manifest needs= : the oldest game version that plays it ("" = any) */
    unsigned long long bytes;
    int enabled;
    int left_out;  /* a superstar mod over the game's ids (s_star_ids): not in the game (superstar_limit) */
} Mod;

static Mod s_mods[MODS_MAX];
static int s_nmods;
static int s_base;                 /* first control id */
static HWND s_list, s_status, s_wnd;
static WCHAR s_game[MAX_PATH];
static int s_filling;              /* (checkbox notifications while filling the list) */

enum { M_LIST, M_ADD, M_REMOVE, M_FOLDER, M_MAKER, M_STATUS, M_BACKGROUND, M_ON, M_OFF, M_SHRINK, M_LEVEL };

static void status(const WCHAR *t)
{
    if (s_status) SetWindowTextW(s_status, t);
    else wprintf(L"%ls\n", t);  /* (the --mods-add tests: no window) */
}

/* The game has 137 ids for superstar mods (src/superstar_mods.cpp kPool; it
   writes the number to Mods\Superstars\limit.txt at each start), kept
   per mod folder in Mods\Superstars\slots.txt. As the game does: a mod that's
   on and holds an id keeps it; the ones without wait, in folder order, for
   the ids left (those of mods switched off or gone included). Marks the
   waiting ones that don't fit; returns how many superstar mods are on. */
enum { SUPERSTAR_IDS = 137 };  /* (until the game has written limit.txt: a fresh install) */
static int s_star_ids = SUPERSTAR_IDS;
static WCHAR s_over_label[64];
static int s_stars_on, s_stars_out;
static int folder_cmp(const void *a, const void *b)
{
    const Mod *x = *(const Mod *const *)a, *y = *(const Mod *const *)b;
    return wcscmp(wcsrchr(x->folder, L'\\') + 1, wcsrchr(y->folder, L'\\') + 1);
}
static void superstar_limit(void)
{
    WCHAR path[MAX_PATH];
    char line[1024];
    static char held[MODS_MAX * 2][MAX_PATH];
    int nheld = 0, holders = 0, i, room;
    Mod *waiting[MODS_MAX];
    int nwait = 0;
    FILE *f = NULL;
    s_stars_on = s_stars_out = 0;
    s_star_ids = SUPERSTAR_IDS;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\limit.txt", s_game);
    if (!_wfopen_s(&f, path, L"rb") && f) {
        int n = 0;
        if (fscanf_s(f, "%d", &n) == 1 && n > 0 && n < 1000) s_star_ids = n;
        fclose(f);
        f = NULL;
    }
    swprintf_s(s_over_label, 64, L"superstar - NOT IN GAME (over the %d limit)", s_star_ids);
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\slots.txt", s_game);
    if (!_wfopen_s(&f, path, L"rb") && f) {
        while (nheld < MODS_MAX * 2 && fgets(line, sizeof line, f)) {
            char *tab = strrchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            strncpy_s(held[nheld++], MAX_PATH, line, _TRUNCATE);
        }
        fclose(f);
    }
    for (i = 0; i < s_nmods; i++) {
        Mod *m = &s_mods[i];
        char acp[MAX_PATH * 2], utf8[MAX_PATH * 3];
        int k, has = 0;
        m->left_out = 0;
        if (_wcsicmp(m->type, L"superstar") || !m->enabled) continue;
        s_stars_on++;
        WideCharToMultiByte(CP_ACP, 0, wcsrchr(m->folder, L'\\') + 1, -1, acp, sizeof acp, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wcsrchr(m->folder, L'\\') + 1, -1, utf8, sizeof utf8, NULL, NULL);
        for (k = 0; k < nheld && !has; k++) has = !strcmp(held[k], acp) || !strcmp(held[k], utf8);
        if (has) holders++;
        else waiting[nwait++] = m;
    }
    qsort(waiting, nwait, sizeof *waiting, folder_cmp);
    room = s_star_ids - holders;
    for (i = 0; i < nwait; i++)
        if (i >= room) waiting[i]->left_out = 1, s_stars_out++;
}

static void trim(WCHAR *s)
{
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L'\r' || s[n - 1] == L'\n' || s[n - 1] == L' ')) s[--n] = 0;
}

/* manifest.txt (UTF-8) -> fields. 1 if it names a type and an id. */
static int read_manifest(const WCHAR *path, Mod *m)
{
    FILE *f = NULL;
    char line[512];
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    while (fgets(line, sizeof line, f)) {
        WCHAR w[512], *eq;
        if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 512)) continue;
        trim(w);
        eq = wcschr(w, L'=');
        if (!eq) continue;
        *eq++ = 0;
        if (!wcscmp(w, L"type")) wcsncpy_s(m->type, 32, eq, _TRUNCATE);
        else if (!wcscmp(w, L"id")) wcsncpy_s(m->id, 64, eq, _TRUNCATE);
        else if (!wcscmp(w, L"name")) wcsncpy_s(m->name, 128, eq, _TRUNCATE);
        else if (!wcscmp(w, L"author")) wcsncpy_s(m->author, 64, eq, _TRUNCATE);
        else if (!wcscmp(w, L"version")) wcsncpy_s(m->version, 32, eq, _TRUNCATE);
        else if (!wcscmp(w, L"made_with")) wcsncpy_s(m->made_with, 64, eq, _TRUNCATE);
        else if (!wcscmp(w, L"needs")) wcsncpy_s(m->needs, 32, eq, _TRUNCATE);
    }
    fclose(f);
    return m->type[0] && m->id[0];
}

static unsigned long long folder_bytes(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unsigned long long n = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            n += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* a mod the game doesn't load whatever its switch: backstage mods (src/no_mods.h BackstageModsOff) and the removed
   FREE-ROAMING BACKSTAGE match type (src/match_types.cpp MatchTypeSwitches) */
static int removed_mod(const Mod *m)
{
    const WCHAR *leaf = wcsrchr(m->folder, L'\\');
    return !_wcsicmp(m->type, L"backstage") || wcsstr(m->folder, L"\\Mods\\Backstage\\") ||
           (leaf && !_wcsicmp(leaf + 1, L"free_roaming_backstage"));
}
static int maker_present(void);

static void scan(void)
{
    static const WCHAR *const types[] = { L"Arenas", L"Superstars", L"Signs", L"Media", L"Backstage", L"Moves", L"MatchTypes",
                                         L"Backgrounds", L"Attires", L"Titles" };
    int t;
    s_nmods = 0;
    if (!s_game[0]) return;
    for (t = 0; t < (int)(sizeof types / sizeof types[0]); t++) {
        WCHAR pat[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        swprintf_s(pat, MAX_PATH, L"%s\\Mods\\%s\\*", s_game, types[t]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            Mod *m;
            WCHAR man[MAX_PATH], off[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || s_nmods >= MODS_MAX)
                continue;
            m = &s_mods[s_nmods];
            ZeroMemory(m, sizeof *m);
            swprintf_s(m->folder, MAX_PATH, L"%s\\Mods\\%s\\%s", s_game, types[t], fd.cFileName);
            swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", m->folder);
            if (!read_manifest(man, m)) {  /* a folder without a manifest (older test mods: info.txt) */
                swprintf_s(man, MAX_PATH, L"%s\\info.txt", m->folder);
                read_manifest(man, m);
                if (!m->type[0]) wcsncpy_s(m->type, 32, L"arena", _TRUNCATE);
                if (!m->id[0]) wcsncpy_s(m->id, 64, fd.cFileName, _TRUNCATE);
            }
            if (!m->name[0]) wcsncpy_s(m->name, 128, m->id, _TRUNCATE);
            swprintf_s(off, MAX_PATH, L"%s\\disabled", m->folder);
            /* backstage mods are switched off in the game (src/no_mods.h BackstageModsOff), and the FREE-ROAMING
               BACKSTAGE match type is removed (src/match_types.cpp MatchTypeSwitches): shown off */
            m->enabled = !exists(off) && !removed_mod(m);
            m->bytes = folder_bytes(m->folder);
            s_nmods++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static int version_newer(const WCHAR *a, const WCHAR *b);
/* The mod needs a newer game than this one (manifest needs=: a big model or arena, docs/SVRMOD_FORMAT.md). The
   launcher installs the game of its own release, so its version is the game's. */
static int needs_newer(const Mod *m) { return m->needs[0] && version_newer(m->needs, PORT_VERSION); }

static void fill(void)
{
    int i;
    s_filling = 1;
    superstar_limit();
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_nmods; i++) {
        LVITEMW it;
        WCHAR size[32];
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = s_mods[i].name;
        it.lParam = i;
        ListView_InsertItem(s_list, &it);
        WCHAR type[96];
        if (s_mods[i].left_out) wcsncpy_s(type, 96, s_over_label, _TRUNCATE);
        else if (needs_newer(&s_mods[i])) swprintf_s(type, 96, L"%s - NEEDS GAME %s", s_mods[i].type, s_mods[i].needs);
        else wcsncpy_s(type, 96, s_mods[i].type, _TRUNCATE);
        ListView_SetItemText(s_list, i, 1, type);
        ListView_SetItemText(s_list, i, 2, s_mods[i].version);
        ListView_SetItemText(s_list, i, 3, s_mods[i].author);
        swprintf_s(size, 32, L"%.1f MB", s_mods[i].bytes / 1048576.0);
        ListView_SetItemText(s_list, i, 4, size);
        /* made with the Mod Maker (its stamp), or not - a flag, not a warning */
        ListView_SetItemText(s_list, i, 5, s_mods[i].made_with[0] ? s_mods[i].made_with : L"non-Mod Maker");
        ListView_SetCheckState(s_list, i, s_mods[i].enabled);
    }
    s_filling = 0;
    {
        WCHAR t[160];
        if (!s_game[0]) status(L"Install the game first (Install tab).");
        else if (!s_nmods) status(maker_present() ? L"No mods yet. + adds a .svrmod file; Open Mod Maker makes one."
                                                  : L"No mods yet. + adds a .svrmod file.");
        else if (s_stars_out) {
            swprintf_s(t, 160, L"%d of %d superstar mods fit - %d aren't in the game (%d is the limit). Switch some "
                               L"off to choose which play.", s_stars_on - s_stars_out, s_stars_on, s_stars_out, s_star_ids);
            status(t);
        } else {
            swprintf_s(t, 160, L"%d mod%s. They load the next time the game starts; your Game Files are never "
                               L"changed.", s_nmods, s_nmods == 1 ? L"" : L"s");
            status(t);
        }
    }
}

void mods_show(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    scan();
    fill();
}

int mods_delete_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    const DWORD a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(dir, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(dir) != 0;
    }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT) && swprintf_s(pat, MAX_PATH, L"%s\\*", dir) > 0) {
        h = FindFirstFileW(pat, &fd);  /* (a junction: the link only, never what it points at) */
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (swprintf_s(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName) > 0) mods_delete_tree(p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    SetFileAttributesW(dir, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(dir) != 0 || GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES;
}

/* Removes a folder tree: to the Recycle Bin when `recycle` (a mod the player removes), else directly
   (mods_delete_tree: the launcher's own temporary and replaced copies). */
static int remove_tree(const WCHAR *dir, int recycle)
{
    WCHAR from[MAX_PATH + 2] = { 0 };
    SHFILEOPSTRUCTW op;
    if (!recycle) return mods_delete_tree(dir);
    wcsncpy_s(from, MAX_PATH, dir, _TRUNCATE);  /* (double NUL terminated) */
    ZeroMemory(&op, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | (recycle ? FOF_ALLOWUNDO : 0);
    return SHFileOperationW(&op) == 0;
}

/* A removed superstar mod gives its id back: its line ("<folder>\t<id>") leaves
   Mods\Superstars\slots.txt. (The game keeps a mod's id there so saves stay
   with it; a line left behind held one of its 72 ids for good, and new mods
   no longer showed once they were all taken.) The game writes the folder in
   the ANSI code page; both that and UTF-8 match. */
static void free_superstar_slot(const WCHAR *game, const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    const WCHAR *name = wcsrchr(folder, L'\\');
    char acp[MAX_PATH * 2], utf8[MAX_PATH * 3], line[1024];
    char *out = NULL;
    size_t used = 0, cap = 0;
    int dropped = 0;
    FILE *f = NULL;
    name = name ? name + 1 : folder;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\slots.txt", game);
    if (_wfopen_s(&f, path, L"rb") || !f) return;
    WideCharToMultiByte(CP_ACP, 0, name, -1, acp, sizeof acp, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL);
    while (fgets(line, sizeof line, f)) {
        const char *tab = strchr(line, '\t');
        size_t n = strlen(line), k = tab ? (size_t)(tab - line) : 0;
        if (tab && ((strlen(acp) == k && !strncmp(line, acp, k)) || (strlen(utf8) == k && !strncmp(line, utf8, k)))) {
            dropped = 1;
            continue;
        }
        if (used + n + 1 > cap) {
            char *grown = (char *)realloc(out, cap = (used + n + 1) * 2);
            if (!grown) { free(out); fclose(f); return; }
            out = grown;
        }
        memcpy(out + used, line, n);
        used += n;
    }
    fclose(f);
    if (dropped && !_wfopen_s(&f, path, L"wb") && f) {
        if (used) fwrite(out, 1, used, f);
        fclose(f);
    }
    free(out);
}

/* ---- Two attire mods on one attire (Mods\Attires\<folder>: type=attire, superstar=<id>, attire=<1-4>|new).
   The rule the game, this tab, the phone's Mods page and the Mod Maker share: a superstar's attire is
   free when his own pac (pac\ch\ch<id>.pac, EMD entries "%06d%d2") doesn't have it and no switched-on
   attire mod asks for it. Of two switched-on mods asking for the same attire the one installed first
   keeps it (a mod just installed is the newer one); the other moves to the superstar's next free attire
   (1-4) - its manifest's attire= is rewritten, with a "# moved" line - or, with none free, it stays and
   the player is told to switch one off (the game then leaves it out: src/superstar_mods.cpp
   LoadAttireMods). Run when an attire mod is installed and when Play starts the game. */
static const struct { int id; const WCHAR *name; const WCHAR *note; } k_roster[] = {
#include "roster_names.inc"
};

static const WCHAR *roster_name(int id, WCHAR *buf, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof k_roster / sizeof k_roster[0]); i++)
        if (k_roster[i].id == id) return k_roster[i].name;
    swprintf_s(buf, n, L"superstar %d", id);
    return buf;
}

/* One model pac's attires 1-4 for superstar id (bits 0-3) and whether it has other model kinds for attire 1. */
static unsigned pac_attires(const WCHAR *path, int id, int *gear)
{
    unsigned char d[0x4000];
    unsigned mask = 0;
    char want[8];
    size_t p;
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    p = fread(d, 1, sizeof d, f);
    fclose(f);
    if (p != sizeof d || memcmp(d, "EPK8", 4)) return 0;
    sprintf_s(want, sizeof want, "%06d", id);
    for (p = 0x800; p + 12 <= sizeof d;) {
        unsigned k, entries;
        int emd;
        if (!memcmp(d + p, "\0\0\0\0", 4)) break;
        emd = !memcmp(d + p, "EMD ", 4);
        entries = (d[p + 4] | d[p + 5] << 8) / 4;
        p += 12;
        for (k = 0; k < entries && p + 16 <= sizeof d; k++, p += 16)
            if (emd && !memcmp(d + p, want, 6) && d[p + 6] >= '0' && d[p + 6] <= '3') {
                if (d[p + 7] == '2') mask |= 1u << (d[p + 6] - '0');
                else if (d[p + 6] == '0' && d[p + 7] >= '0' && d[p + 7] <= '9') *gear = 1;
            }
    }
    return mask;
}

/* Attires 1-4 (bits 0-3) the superstar's own pacs have - the game's pac\ch\chNNN.pac and the installed DLC's
   pac\ch\ch<id>_dlcNN.pac (UserData\0000000000000000\5451085D\00000002\<package>: a DLC superstar's own, ids
   51-58, and DLC attires of disc superstars); *gear: one has other model kinds for attire 1 (the entrance gear
   superstars - Undertaker, CM Punk, Cena, Shawn Michaels, Morrison: kind 4). A new attire for them doesn't load in
   the game yet (src/superstar_mods.cpp ExtraKinds): only their own can be replaced. */
static unsigned own_attires(const WCHAR *game, int id, int *gear)
{
    WCHAR path[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW pk, fd;
    HANDLE hp, hf;
    unsigned mask = 0;
    *gear = 0;
    if (id >= 100) {
        swprintf_s(path, MAX_PATH, L"%s\\pac\\ch\\ch%03d.pac", game, id);
        mask |= pac_attires(path, id, gear);
    }
    swprintf_s(pat, MAX_PATH, L"%s\\UserData\\0000000000000000\\5451085D\\00000002\\*", game);
    hp = FindFirstFileW(pat, &pk);
    if (hp == INVALID_HANDLE_VALUE) return mask;
    do {
        if (!(pk.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pk.cFileName[0] == L'.') continue;
        swprintf_s(pat, MAX_PATH, L"%s\\UserData\\0000000000000000\\5451085D\\00000002\\%s\\pac\\ch\\ch%d_dlc*.pac", game,
                   pk.cFileName, id);
        hf = FindFirstFileW(pat, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            int g = 0;
            swprintf_s(path, MAX_PATH, L"%s\\UserData\\0000000000000000\\5451085D\\00000002\\%s\\pac\\ch\\%s", game,
                       pk.cFileName, fd.cFileName);
            mask |= pac_attires(path, id, &g);
            if (g) *gear = 1;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    } while (FindNextFileW(hp, &pk));
    FindClose(hp);
    return mask;
}

/* A DLC superstar the player has installed (ids below 100: only DLC model pacs). */
static int dlc_superstar(const WCHAR *game, int id)
{
    int gear = 0;
    return id > 0 && id < 100 && own_attires(game, id, &gear) != 0;
}

typedef struct {
    WCHAR folder[MAX_PATH], name[128];
    int id, attire;            /* attire 1-4, 0 = new */
    unsigned long long born;   /* folder creation time: the order they were installed in */
    int fresh;                 /* the mod just installed */
} AttireMod;

static int attire_older(const AttireMod *a, const AttireMod *b)
{
    if (a->fresh != b->fresh) return b->fresh;
    if (a->born != b->born) return a->born < b->born;
    return wcscmp(a->folder, b->folder) < 0;
}

/* attire=<n> into the mod's manifest, the other lines kept, and a note why. */
static int set_attire(const WCHAR *folder, int from, int to, const WCHAR *keeper)
{
    WCHAR man[MAX_PATH], tmp[MAX_PATH];
    FILE *in = NULL, *out = NULL;
    char line[1024], k8[256];
    int done = 0;
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", folder);
    swprintf_s(tmp, MAX_PATH, L"%s\\manifest.new", folder);
    if (_wfopen_s(&in, man, L"rb") || !in) return 0;
    if (_wfopen_s(&out, tmp, L"wb") || !out) { fclose(in); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, keeper, -1, k8, sizeof k8, NULL, NULL);
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, "attire=", 7) && !done) {
            fprintf(out, "attire=%d\n# moved from attire %d by the launcher (%s has attire %d)\n", to, from, k8, from);
            done = 1;
            continue;
        }
        fputs(line, out);
        if (line[0] && line[strlen(line) - 1] != '\n') fputs("\n", out);
    }
    fclose(in);
    fclose(out);
    return done && MoveFileExW(tmp, man, MOVEFILE_REPLACE_EXISTING);
}

/* -> moves + warnings made (report: one line each, may be NULL). */
int mods_attire_guard(const WCHAR *game, const WCHAR *fresh, WCHAR *report, size_t report_n)
{
    static AttireMod mods[MODS_MAX];
    int n = 0, i, j, said = 0;
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (report && report_n) report[0] = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\Mods\\Attires\\*", game);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR man[MAX_PATH], off[MAX_PATH];
        FILE *f = NULL;
        char line[512], model[MAX_PATH] = "attire.pac";
        WCHAR wmodel[MAX_PATH], mpath[MAX_PATH];
        int attire_mod = 0;
        AttireMod *a = &mods[n];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || n >= MODS_MAX) continue;
        ZeroMemory(a, sizeof *a);
        swprintf_s(a->folder, MAX_PATH, L"%s\\Mods\\Attires\\%s", game, fd.cFileName);
        swprintf_s(off, MAX_PATH, L"%s\\disabled", a->folder);
        if (exists(off)) continue;
        swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", a->folder);
        if (_wfopen_s(&f, man, L"rb") || !f) continue;
        a->attire = -1;
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *e;
            if (!eq || line[0] == '#') continue;
            *eq++ = 0;
            for (e = eq + strlen(eq); e > eq && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ');) *--e = 0;
            if (!strcmp(line, "type")) attire_mod = !strcmp(eq, "attire");
            else if (!strcmp(line, "superstar")) a->id = atoi(eq);
            else if (!strcmp(line, "attire")) a->attire = !strcmp(eq, "new") ? 0 : atoi(eq);
            else if (!strcmp(line, "name")) MultiByteToWideChar(CP_UTF8, 0, eq, -1, a->name, 128);
            else if (!strcmp(line, "model") && *eq) strncpy_s(model, MAX_PATH, eq, _TRUNCATE);
        }
        fclose(f);
        if (!attire_mod || ((a->id < 100 || a->id > 321) && !dlc_superstar(game, a->id)) || a->attire < 0 || a->attire > 4)
            continue;
        /* no model: only an entrance for one of the superstar's own attires (Simple entrance porting) - it holds no
           attire, so it isn't moved (the game: the first folder A-Z keeps an attire's entrance) */
        MultiByteToWideChar(CP_UTF8, 0, model, -1, wmodel, MAX_PATH);
        swprintf_s(mpath, MAX_PATH, L"%s\\%s", a->folder, wmodel);
        if (!exists(mpath)) continue;
        if (!a->name[0]) wcsncpy_s(a->name, 128, fd.cFileName, _TRUNCATE);
        a->born = ((unsigned long long)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
        a->fresh = fresh && !_wcsicmp(a->folder, fresh);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (i = 0; i < n; i++) {
        unsigned used, own;
        int k, news = 0, gear = 0;
        if (mods[i].id < 0) continue;  /* (a superstar already done) */
        own = used = own_attires(game, mods[i].id, &gear);
        /* the attires asked for, the oldest mod on each keeping it */
        for (j = 0; j < n; j++) {
            AttireMod *a = &mods[j];
            int best = 1;
            if (a->id != mods[i].id || !a->attire) continue;
            for (k = 0; k < n; k++)
                if (k != j && mods[k].id == a->id && mods[k].attire == a->attire && attire_older(&mods[k], a)) best = 0;
            if (best) used |= 1u << (a->attire - 1);
        }
        if (gear) used = 0xF;  /* (no new attires for them: nothing free to move to) */
        for (j = 0; j < n; j++) {
            AttireMod *a = &mods[j], *keeper = NULL;
            WCHAR who[48], t[512];
            int to = 0;
            if (a->id != mods[i].id) continue;
            if (!a->attire) { news++; continue; }
            if (gear && !(own >> (a->attire - 1) & 1)) {  /* (a new attire for a gear superstar: left out by the game) */
                swprintf_s(t, 512, L"\"%s\" gives %s an attire %d, but %s wears entrance gear and new attires for them "
                                   L"don't load in the game yet: the game leaves it out (replacing attire 1-%d works).\r\n",
                           a->name, roster_name(a->id, who, 48), a->attire, roster_name(a->id, who, 48),
                           own & 4 ? 3 : own & 2 ? 2 : 1);
                if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
                said++;
                continue;
            }
            for (k = 0; k < n; k++)
                if (k != j && mods[k].id == a->id && mods[k].attire == a->attire && attire_older(&mods[k], a))
                    if (!keeper || attire_older(&mods[k], keeper)) keeper = &mods[k];
            if (!keeper) continue;
            for (k = 0; k < 4 && !to; k++)
                if (!(used >> k & 1)) to = k + 1;
            if (to && set_attire(a->folder, a->attire, to, keeper->name)) {
                used |= 1u << (to - 1);
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d: \"%s\" moved to attire %d.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, a->name, to);
                a->attire = to;
            } else if (gear)
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d. %s wears entrance gear, and new attires "
                                   L"for them don't load in the game yet, so neither can move: switch one off, or the "
                                   L"game leaves \"%s\" out.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, roster_name(a->id, who, 48),
                           a->name);
            else
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d and there is no free attire to "
                                   L"move one to (4 at most): switch one off, or the game leaves \"%s\" out.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, a->name);
            if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
            said++;
        }
        /* attire=new ones take what is left (the game hands them out in folder order) */
        for (k = 0; k < 4; k++)
            if (!(used >> k & 1) && news) news--;
        if (news) {
            WCHAR who[48], t[256];
            swprintf_s(t, 256, gear ? L"%s wears entrance gear: new attires for them don't load in the game yet, so "
                                      L"%d attire=new mod%s stay%s out: switch %s off.\r\n"
                                    : L"%s has no free attire for %d attire=new mod%s (4 at most): switch some off.\r\n",
                       roster_name(mods[i].id, who, 48), news, news == 1 ? L"" : L"s", news == 1 ? L"s" : L"",
                       news == 1 ? L"it" : L"them");
            if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
            said++;
        }
        for (j = n - 1; j >= i; j--)
            if (mods[j].id == mods[i].id && j != i) mods[j].id = -1;
        mods[i].id = -1;
    }
    return said;
}

/* Tests: --mods-attires <game folder> runs the guard as Play does and prints what it did. */
int mods_print_attires(const WCHAR *game)
{
    static WCHAR report[8192];
    const int n = mods_attire_guard(game, NULL, report, 8192);
    wprintf(L"%s%d\n", report, n);
    return 1;
}

static int pick_file(WCHAR *out)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    int ok = 0;
    COMDLG_FILTERSPEC spec[] = { { L"SvR2011 mods (*.svrmod, *.zip)", L"*.svrmod;*.zip" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetTitle(dlg, L"Add a mod");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        wcsncpy_s(out, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        ok = 1;
    }
    if (item) item->lpVtbl->Release(item);
    dlg->lpVtbl->Release(dlg);
    return ok;
}

/* id -> a safe folder name */
static void safe_id(WCHAR *id)
{
    for (; *id; id++)
        if (!(iswalnum(*id) || *id == L'_' || *id == L'-' || *id == L'.')) *id = L'_';
}

static int install_file(const WCHAR *zip);
static void install_log(const WCHAR *game, const WCHAR *text);

/* + : one or several .svrmod / .zip files, installed one after the other. */
static void add_mod(void)
{
    IFileOpenDialog *dlg = NULL;
    IShellItemArray *items = NULL;
    DWORD n = 0, i;
    int done = 0;
    COMDLG_FILTERSPEC spec[] = { { L"SvR2011 mods (*.svrmod, *.zip)", L"*.svrmod;*.zip" } };
    if (!s_game[0]) { status(L"Install the game first (Setup \x2192 The game)."); return; }
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetOptions(dlg, FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"Add mods (several at once: Ctrl+click)");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResults(dlg, &items))) {
        items->lpVtbl->GetCount(items, &n);
        for (i = 0; i < n; i++) {
            IShellItem *it = NULL;
            PWSTR path = NULL;
            if (SUCCEEDED(items->lpVtbl->GetItemAt(items, i, &it)) &&
                SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
                WCHAR one[MAX_PATH];
                wcsncpy_s(one, MAX_PATH, path, _TRUNCATE);
                CoTaskMemFree(path);
                install_file(one);   /* (its own status line; the last one's stays) */
                done++;
            }
            if (it) it->lpVtbl->Release(it);
        }
        items->lpVtbl->Release(items);
    }
    dlg->lpVtbl->Release(dlg);
    if (done > 1) {
        WCHAR t[96];
        swprintf_s(t, 96, L"Installed %d mods (they load the next time the game starts).", done);
        status(t);
    }
}

/* A picture for the main menu (backgrounds.h). */
static int pick_image(WCHAR *out)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    int ok = 0;
    COMDLG_FILTERSPEC spec[] = { { L"Pictures (*.png, *.jpg, *.jpeg, *.bmp, *.webp)", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetTitle(dlg, L"Add a main menu background picture");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        wcsncpy_s(out, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        ok = 1;
    }
    if (item) item->lpVtbl->Release(item);
    dlg->lpVtbl->Release(dlg);
    return ok;
}

static void add_background(void)
{
    WCHAR img[MAX_PATH], err[600] = L"", t[700];
    HCURSOR old;
    int ok;
    if (!s_game[0]) { status(L"Install the game first (Install tab)."); return; }
    if (!pick_image(img)) return;
    status(L"Converting the picture\x2026");
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ok = bg_make(s_game, img, NULL, 0, NULL, err, 600);
    SetCursor(old);
    scan();
    fill();
    if (!ok) { status(err); return; }
    swprintf_s(t, 700, L"Added. The main menu shows it after its own 12 pictures (MY WWE \x2192 OPTIONS \x2192 "
                       L"BACKGROUNDS turns pictures on or off, X there shows the game's logo on it).");
    status(t);
}

int mods_add_background(const WCHAR *game_dir, const WCHAR *image, int logo)
{
    WCHAR err[600] = L"", dir[MAX_PATH] = L"";
    const int ok = bg_make(game_dir, image, NULL, logo, dir, err, 600);
    if (ok) wprintf(L"added: %s\n", dir);
    else wprintf(L"failed: %s\n", err);
    return ok;
}

static WCHAR s_installed[MAX_PATH];  /* the folder install_file put the last mod in */
static int s_installed_existed;      /* ... and whether it was installed already */
static int s_bundle_install;         /* (mods_install_bundled: never an older version over a newer one) */
static void (*s_bundled_progress)(int n, int total);
static int s_bundled_n, s_bundled_total;  /* (this call's: the progress) */

void mods_set_bundled_progress(void (*progress)(int n, int total)) { s_bundled_progress = progress; }

/* a > b as dotted versions ("1.10" > "1.9"). */
static int version_newer(const WCHAR *a, const WCHAR *b)
{
    while (*a || *b) {
        long x = wcstol(a, (WCHAR **)&a, 10), y = wcstol(b, (WCHAR **)&b, 10);
        if (x != y) return x > y;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
        if ((*a && !iswdigit(*a)) || (*b && !iswdigit(*b))) break;
    }
    return 0;
}

/* A .svrmod's manifest.txt read from the zip (its central directory and that
   one entry, not an unpack of the whole mod) -> type and id. */
static int zip_manifest(const WCHAR *zip, Mod *m)
{
    FILE *f;
    unsigned char tail[65536 + 22], *cd = NULL, hdr[30], *data = NULL, *raw = NULL;
    long long size;
    size_t got, i, at;
    unsigned cd_size, cd_off, n;
    int ok = 0;
    if (_wfopen_s(&f, zip, L"rb") || !f) return 0;
    _fseeki64(f, 0, SEEK_END);
    size = _ftelli64(f);
    got = (size_t)(size < (long long)sizeof tail ? size : (long long)sizeof tail);
    _fseeki64(f, size - (long long)got, SEEK_SET);
    if (fread(tail, 1, got, f) != got) goto done;
    for (i = got >= 22 ? got - 22 : 0;; --i) {   /* the end-of-central-directory record */
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 5 && tail[i + 3] == 6) break;
        if (!i) goto done;
    }
    n = tail[i + 10] | tail[i + 11] << 8;
    cd_size = tail[i + 12] | tail[i + 13] << 8 | tail[i + 14] << 16 | (unsigned)tail[i + 15] << 24;
    cd_off = tail[i + 16] | tail[i + 17] << 8 | tail[i + 18] << 16 | (unsigned)tail[i + 19] << 24;
    if (!(cd = (unsigned char *)malloc(cd_size)) || _fseeki64(f, cd_off, SEEK_SET) || fread(cd, 1, cd_size, f) != cd_size)
        goto done;
    for (at = 0; n-- && at + 46 <= cd_size;) {
        const unsigned method = cd[at + 10] | cd[at + 11] << 8;
        const unsigned csize = cd[at + 20] | cd[at + 21] << 8 | cd[at + 22] << 16 | (unsigned)cd[at + 23] << 24;
        const unsigned usize = cd[at + 24] | cd[at + 25] << 8 | cd[at + 26] << 16 | (unsigned)cd[at + 27] << 24;
        const unsigned nlen = cd[at + 28] | cd[at + 29] << 8, xlen = cd[at + 30] | cd[at + 31] << 8;
        const unsigned clen = cd[at + 32] | cd[at + 33] << 8;
        const unsigned loc = cd[at + 42] | cd[at + 43] << 8 | cd[at + 44] << 16 | (unsigned)cd[at + 45] << 24;
        if (at + 46 + nlen > cd_size) break;
        if (nlen == 12 && !memcmp(cd + at + 46, "manifest.txt", 12) && usize < (1u << 20) && csize < (1u << 20)) {
            if (_fseeki64(f, loc, SEEK_SET) || fread(hdr, 1, 30, f) != 30 ||
                _fseeki64(f, loc + 30 + (hdr[26] | hdr[27] << 8) + (hdr[28] | hdr[29] << 8), SEEK_SET))
                goto done;
            if (!(raw = (unsigned char *)malloc(csize + 1)) || fread(raw, 1, csize, f) != csize) goto done;
            if (method == 0 && csize == usize) data = raw, raw = NULL;
            else if (method == 8 && (data = (unsigned char *)malloc(usize + 1)) && unzip_inflate(raw, csize, data, usize)) {}
            else goto done;
            data[usize] = 0;
            {   /* "key=value" lines, UTF-8 */
                char *line = (char *)data, *next;
                for (; line && *line; line = next) {
                    WCHAR w[512], *eq;
                    next = strchr(line, '\n');
                    if (next) *next++ = 0;
                    if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 512)) continue;
                    trim(w);
                    if (!(eq = wcschr(w, L'='))) continue;
                    *eq++ = 0;
                    if (!wcscmp(w, L"type")) wcsncpy_s(m->type, 32, eq, _TRUNCATE);
                    else if (!wcscmp(w, L"id")) wcsncpy_s(m->id, 64, eq, _TRUNCATE);
                    else if (!wcscmp(w, L"version")) wcsncpy_s(m->version, 32, eq, _TRUNCATE);
                }
            }
            ok = m->type[0] && m->id[0];
            break;
        }
        at += 46 + nlen + xlen + clen;
    }
done:
    free(cd);
    free(raw);
    free(data);
    fclose(f);
    return ok;
}

/* A mod type's folder under Mods (as install_file places it); NULL for one it doesn't install. */
static const WCHAR *kind_folder(const WCHAR *type)
{
    static const struct { const WCHAR *type, *folder; } k[] = {
        {L"superstar", L"Superstars"}, {L"arena", L"Arenas"}, {L"backstage", L"Backstage"}, {L"signs", L"Signs"},
        {L"media", L"Media"}, {L"matchtype", L"MatchTypes"}, {L"background", L"Backgrounds"}, {L"attire", L"Attires"},
        {L"moves", L"Moves"}, {L"title", L"Titles"},
    };
    int i;
    for (i = 0; i < (int)(sizeof k / sizeof *k); ++i)
        if (!_wcsicmp(type, k[i].type)) return k[i].folder;
    return NULL;
}

int mods_install(const WCHAR *game_dir, const WCHAR *zip)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    return s_game[0] && install_file(zip);
}

/* ── the bundled mods' record (Mods\.bundled_state) ───────────────────────────────────────────────────────
 * What the launcher installed of each bundle, so it only CHECKS them at a start and never puts a player's edits
 * over: per bundle its version, folder (kind\id) and state - "on" (installed by the launcher), "removed" (the
 * player removed its folder: it stays removed), "kept <version>" (the player kept their edited copy over that
 * version) - and per installed file its size, time and hash (FNV-1a 64). (The old Mods\.bundled -
 * "name|size|time" lines, one more for each update - is read only once, to carry its installs over: it was read
 * into 8192 characters, and past that, after a few updates, its newest lines were cut off and those bundles
 * installed again at every start, over the player's edits - 2.1.4.1 report.) */
typedef struct {
    WCHAR rel[MAX_PATH];
    unsigned long long size, time, hash;
} BundleFile;
typedef struct {
    WCHAR file[MAX_PATH], version[32], folder[MAX_PATH], state[48];
    BundleFile *files;
    int n_files, cap_files;
    int carried_old;   /* (carried over at this start: another version than the shipped one, dated unedited) */
} BundleRec;
static BundleRec *s_recs;
static int s_n_recs, s_cap_recs;
static BundledQuestion s_questions[64];
static int s_n_questions;
static struct { WCHAR file[MAX_PATH]; int answer; } s_answers[64];
static int s_n_answers, s_default_answer;

static unsigned long long file_hash(const WCHAR *path)
{
    static unsigned char buf[1 << 20];
    unsigned long long h = 1469598103934665603ull;
    FILE *f;
    size_t got, i;
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        for (i = 0; i < got; ++i) h = (h ^ buf[i]) * 1099511628211ull;
    fclose(f);
    return h;
}

static BundleRec *rec_find(const WCHAR *file)
{
    int i;
    for (i = 0; i < s_n_recs; ++i)
        if (!_wcsicmp(s_recs[i].file, file)) return &s_recs[i];
    return NULL;
}

static BundleRec *rec_add(const WCHAR *file)
{
    BundleRec *r = rec_find(file);
    if (r) return r;
    if (s_n_recs == s_cap_recs) {
        BundleRec *more = (BundleRec *)realloc(s_recs, sizeof *s_recs * (s_cap_recs ? s_cap_recs * 2 : 64));
        if (!more) return NULL;
        s_recs = more;
        s_cap_recs = s_cap_recs ? s_cap_recs * 2 : 64;
    }
    r = &s_recs[s_n_recs++];
    ZeroMemory(r, sizeof *r);
    wcsncpy_s(r->file, MAX_PATH, file, _TRUNCATE);
    return r;
}

static void rec_file_add(BundleRec *r, const WCHAR *rel, unsigned long long size, unsigned long long time,
                         unsigned long long hash)
{
    if (r->n_files == r->cap_files) {
        BundleFile *more = (BundleFile *)realloc(r->files, sizeof *r->files * (r->cap_files ? r->cap_files * 2 : 16));
        if (!more) return;
        r->files = more;
        r->cap_files = r->cap_files ? r->cap_files * 2 : 16;
    }
    wcsncpy_s(r->files[r->n_files].rel, MAX_PATH, rel, _TRUNCATE);
    r->files[r->n_files].size = size, r->files[r->n_files].time = time, r->files[r->n_files].hash = hash;
    ++r->n_files;
}

static void recs_free(void)
{
    int i;
    for (i = 0; i < s_n_recs; ++i) free(s_recs[i].files);
    free(s_recs);
    s_recs = NULL, s_n_recs = s_cap_recs = 0;
}

/* A whole UTF-8 text file (no length limit) as a malloc'd wide string; NULL if none. */
static WCHAR *read_text(const WCHAR *path)
{
    FILE *f;
    long long n;
    char *b;
    WCHAR *w = NULL;
    int wn;
    if (_wfopen_s(&f, path, L"rb") || !f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n < 0 || n > (64ll << 20) || !(b = (char *)malloc((size_t)n + 1))) {
        fclose(f);
        return NULL;
    }
    n = (long long)fread(b, 1, (size_t)n, f);
    fclose(f);
    b[n] = 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, b, (int)n + 1, NULL, 0);
    if (wn > 0 && (w = (WCHAR *)malloc(sizeof(WCHAR) * wn))) MultiByteToWideChar(CP_UTF8, 0, b, (int)n + 1, w, wn);
    free(b);
    if (w && w[0] == 0xFEFF) memmove(w, w + 1, wcslen(w) * sizeof(WCHAR));
    return w;
}

static void state_load(const WCHAR *mods)
{
    WCHAR path[MAX_PATH], *text, *line, *next;
    recs_free();
    swprintf_s(path, MAX_PATH, L"%s\\.bundled_state", mods);
    if (!(text = read_text(path))) return;
    for (line = text; line && *line; line = next) {
        WCHAR *f[7] = {0};
        int k = 0;
        WCHAR *p;
        next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        if ((p = wcschr(line, L'\r')) != NULL) *p = 0;
        for (p = line; k < 7; ++k) {   /* (tab-separated) */
            f[k] = p;
            if (!(p = wcschr(p, L'\t'))) break;
            *p++ = 0;
        }
        if (!wcscmp(f[0], L"B") && f[4]) {
            BundleRec *r = rec_add(f[1]);
            if (!r) continue;
            wcsncpy_s(r->version, 32, f[2], _TRUNCATE);
            wcsncpy_s(r->folder, MAX_PATH, f[3], _TRUNCATE);
            wcsncpy_s(r->state, 48, f[4], _TRUNCATE);
        } else if (!wcscmp(f[0], L"F") && f[5]) {
            BundleRec *r = rec_find(f[1]);
            if (r) rec_file_add(r, f[2], _wcstoui64(f[3], NULL, 10), _wcstoui64(f[4], NULL, 10), _wcstoui64(f[5], NULL, 16));
        }
    }
    free(text);
}

static void state_save(const WCHAR *mods)
{
    WCHAR path[MAX_PATH], part[MAX_PATH];
    FILE *f;
    int i, k;
    swprintf_s(path, MAX_PATH, L"%s\\.bundled_state", mods);
    swprintf_s(part, MAX_PATH, L"%s.part", path);
    if (_wfopen_s(&f, part, L"w, ccs=UTF-8") || !f) return;
    fwprintf(f, L"# The bundled mods the launcher installed: their versions, folders and files (don't edit)\n");
    for (i = 0; i < s_n_recs; ++i) {
        const BundleRec *r = &s_recs[i];
        fwprintf(f, L"B\t%s\t%s\t%s\t%s\n", r->file, r->version, r->folder, r->state);
        for (k = 0; k < r->n_files; ++k)
            fwprintf(f, L"F\t%s\t%s\t%llu\t%llu\t%llx\n", r->file, r->files[k].rel, r->files[k].size, r->files[k].time,
                     r->files[k].hash);
    }
    fclose(f);
    MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING);
}

/* (the files that are the mod's own: not its on / off "disabled" or the game's and launcher's dot files) */
static int bundle_file_counts(const WCHAR *name)
{
    return name[0] != L'.' && _wcsicmp(name, L"disabled") != 0;
}

/* Every file under dir (rel paths from root) into r - size, time and (with `hash`) its hash. */
static void record_files(BundleRec *r, const WCHAR *root, const WCHAR *rel, int hash)
{
    WCHAR pat[MAX_PATH], full[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*", root, rel);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !bundle_file_counts(fd.cFileName)) continue;
        swprintf_s(sub, MAX_PATH, rel[0] ? L"%s\\%s" : L"%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) record_files(r, root, sub, hash);
            continue;
        }
        swprintf_s(full, MAX_PATH, L"%s\\%s", root, sub);
        rec_file_add(r, sub, (unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
                     (unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime,
                     hash ? file_hash(full) : 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void rec_set_files(BundleRec *r, const WCHAR *dir)
{
    r->n_files = 0;
    record_files(r, dir, L"", 1);
}

static volatile LONG s_bundled_running;   /* (mods_install_bundled: its record in use - Level music waits) */

/* A bundled mod's manifest.txt changed by the launcher itself (Level music's theme_gain=): its record takes the new
   file, so the copy still counts as unedited (updated at the next version, not asked about). */
static void bundle_rerecord_manifest(const WCHAR *game, const WCHAR *dir)
{
    WCHAR mods[MAX_PATH], full[MAX_PATH];
    const WCHAR *rel = wcsstr(dir, L"\\Mods\\");
    WIN32_FILE_ATTRIBUTE_DATA a;
    int i, k;
    if (!rel || !GetFileAttributesExW((swprintf_s(full, MAX_PATH, L"%s\\manifest.txt", dir), full), GetFileExInfoStandard, &a))
        return;
    swprintf_s(mods, MAX_PATH, L"%s\\Mods", game);
    state_load(mods);
    for (i = 0; i < s_n_recs; ++i) {
        BundleRec *r = &s_recs[i];
        if (_wcsicmp(r->folder, rel + 6)) continue;
        for (k = 0; k < r->n_files; ++k)
            if (!_wcsicmp(r->files[k].rel, L"manifest.txt")) {
                r->files[k].size = (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
                r->files[k].time = (unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
                r->files[k].hash = file_hash(full);
            }
        state_save(mods);
        break;
    }
    recs_free();
}

/* dir against r's files: *edited - a file changed, or one added (the Mod Maker's save, the player's own);
   *missing - files gone. A file whose size and time are the same is taken as unchanged (no rehash at every
   start); one whose time changed but not its content is too (its time is updated in r). */
static void rec_check(BundleRec *r, const WCHAR *dir, int *edited, int *missing)
{
    BundleRec now;
    int i, k;
    *edited = *missing = 0;
    ZeroMemory(&now, sizeof now);
    for (i = 0; i < r->n_files; ++i) {
        WCHAR full[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA a;
        unsigned long long size, time;
        swprintf_s(full, MAX_PATH, L"%s\\%s", dir, r->files[i].rel);
        if (!GetFileAttributesExW(full, GetFileExInfoStandard, &a)) {
            ++*missing;
            continue;
        }
        size = (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
        time = (unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
        if (size == r->files[i].size && time == r->files[i].time) continue;
        if (size == r->files[i].size && file_hash(full) == r->files[i].hash) {
            r->files[i].time = time;  /* (touched, not changed) */
            continue;
        }
        ++*edited;
    }
    record_files(&now, dir, L"", 0);   /* (files the bundle didn't install: names only) */
    for (k = 0; k < now.n_files; ++k) {
        int known = 0;
        for (i = 0; i < r->n_files && !known; ++i) known = !_wcsicmp(now.files[k].rel, r->files[i].rel);
        if (!known) ++*edited;
    }
    free(now.files);
}

static int answer_for(const WCHAR *file)
{
    int i;
    for (i = 0; i < s_n_answers; ++i)
        if (!_wcsicmp(s_answers[i].file, file)) return s_answers[i].answer;
    return s_default_answer;
}

static void ask(const WCHAR *game_dir, int kind, const WCHAR *file, const WCHAR *name, const WCHAR *have,
                const WCHAR *ship)
{
    BundledQuestion *q;
    WCHAR msg[600];
    if (kind == BUNDLED_ASK_UPDATE)
        swprintf_s(msg, 600, L"bundled %s: edited, version %s shipped (%s installed) - asking", file, ship, have);
    else
        swprintf_s(msg, 600, L"bundled %s: files missing - asking to fix it", file);
    install_log(game_dir, msg);
    if (s_n_questions >= (int)(sizeof s_questions / sizeof *s_questions)) return;
    q = &s_questions[s_n_questions++];
    q->kind = kind;
    wcsncpy_s(q->file, MAX_PATH, file, _TRUNCATE);
    wcsncpy_s(q->name, 128, name, _TRUNCATE);
    wcsncpy_s(q->have, 32, have, _TRUNCATE);
    wcsncpy_s(q->ship, 32, ship, _TRUNCATE);
}

int mods_bundled_questions(BundledQuestion *out, int max)
{
    int i;
    for (i = 0; i < s_n_questions && i < max; ++i) out[i] = s_questions[i];
    return s_n_questions;
}

void mods_bundled_answer(const WCHAR *file, int answer)
{
    int i;
    if (!file) {
        s_default_answer = answer;
        return;
    }
    for (i = 0; i < s_n_answers; ++i)
        if (!_wcsicmp(s_answers[i].file, file)) {
            s_answers[i].answer = answer;
            return;
        }
    if (s_n_answers < (int)(sizeof s_answers / sizeof *s_answers)) {
        wcsncpy_s(s_answers[s_n_answers].file, MAX_PATH, file, _TRUNCATE);
        s_answers[s_n_answers++].answer = answer;
    }
}

void mods_bundled_reset(void)
{
    s_n_questions = s_n_answers = 0;
    s_default_answer = 0;
}

static int bundle_install(const WCHAR *game_dir, const WCHAR *zip, const WCHAR *dir, BundleRec *r, const Mod *bm,
                          int fresh, int was_off);

/* The copy dir to Mods\.backup and the bundle installed in its place (the copy put back if that fails): 1 if done,
   backup = where the copy is. */
static int bundle_replace_keeping(const WCHAR *game_dir, const WCHAR *mods, const WCHAR *kf, const WCHAR *zip,
                                  const WCHAR *dir, BundleRec *r, const Mod *bm, int was_off, WCHAR *backup)
{
    WCHAR parent[MAX_PATH], msg[700];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(backup, MAX_PATH, L"%s\\.backup\\%s-%s-%04d%02d%02d-%02d%02d%02d", mods, kf, bm->id, st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf_s(parent, MAX_PATH, L"%s\\.backup", mods);
    SHCreateDirectoryExW(NULL, parent, NULL);
    if (!MoveFileW(dir, backup)) {
        swprintf_s(msg, 700, L"bundled %s: couldn't back the copy up (Windows error %lu) - left as it is", r->file,
                   GetLastError());
        install_log(game_dir, msg);
        return 0;
    }
    if (bundle_install(game_dir, zip, dir, r, bm, 0, was_off)) return 1;
    MoveFileW(backup, dir);
    swprintf_s(msg, 700, L"bundled %s: the update failed - the copy put back", r->file);
    install_log(game_dir, msg);
    return 0;
}

/* Installs the bundle zip into dir (kept on / off; a fresh one off unless a match type), recorded in r. 1 if done. */
static int bundle_install(const WCHAR *game_dir, const WCHAR *zip, const WCHAR *dir, BundleRec *r, const Mod *bm,
                          int fresh, int was_off)
{
    int ok;
    s_installed[0] = 0;
    s_installed_existed = 0;
    s_bundle_install = 1;
    if (s_bundled_progress) s_bundled_progress(++s_bundled_n, s_bundled_total);
    ok = mods_install(game_dir, zip) && s_installed[0];
    s_bundle_install = 0;
    if (!ok) return 0;
    if (fresh || was_off) {   /* (match types too: any mod that's on keeps a player out of online matches) */
        WCHAR off[MAX_PATH];
        FILE *f;
        swprintf_s(off, MAX_PATH, L"%s\\disabled", dir);
        if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
    }
    wcsncpy_s(r->version, 32, bm->version, _TRUNCATE);
    wcscpy_s(r->state, 48, L"on");
    rec_set_files(r, dir);
    scan();
    fill();
    return 1;
}

/* Mods shipped with the port (<bundle_dir>\*.svrmod, the release's "Bundled Mods"), checked at each start against
 * Mods\.bundled_state (above) - nothing is installed again unless it's needed:
 * - not installed, and never: installed switched off ("disabled" in its folder) - match types too (24 Oct: a mod
 *   that's on keeps the player out of online matches while the server doesn't allow mods);
 * - its folder gone (the player removed it): recorded as removed, never installed again;
 * - a newer version shipped: an unedited copy is updated (its on / off kept); an edited one is asked about
 *   (mods_bundled_questions: update - the player's copy to Mods\.backup - or keep theirs, not asked again for that
 *   version);
 * - files missing from an unedited copy (a damaged install): asked about (fix / not now) - never fixed silently;
 * - the player's copy a newer version than the shipped one: left alone.
 * The questions are answered (mods_bundled_answer) and acted on at the next call. */
static void install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir);
void mods_install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir)
{
    InterlockedIncrement(&s_bundled_running);
    install_bundled(game_dir, bundle_dir);
    InterlockedDecrement(&s_bundled_running);
}

static void install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir)
{
    WCHAR pat[MAX_PATH], zip[MAX_PATH], list[MAX_PATH], mods[MAX_PATH], *legacy = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int changed = 0, carried = 0;
    if (!game_dir || !game_dir[0] || !bundle_dir || !bundle_dir[0])
        return;
    swprintf_s(pat, MAX_PATH, L"%s\\*.svrmod", bundle_dir);
    swprintf_s(mods, MAX_PATH, L"%s\\Mods", game_dir);
    swprintf_s(list, MAX_PATH, L"%s\\.bundled", mods);
    SHCreateDirectoryExW(NULL, mods, NULL);
    state_load(mods);
    legacy = read_text(list);  /* (the old record, whole: what it says was installed is carried over) */
    /* Bundles no longer shipped: the copy the launcher installed is switched off once ("disabled" in its folder;
       the player may switch it back on). hurricane.svrmod (2.0.5): The Hurricane is the playable manager on the M
       tile already - two tiles, neither with an entrance video, theme or announcer clip (the game has none). */
    {
        static const struct { const WCHAR *file, *folder; } retired[] = {
            {L"hurricane.svrmod", L"Superstars\\the_hurricane"},
        };
        int i;
        for (i = 0; i < (int)(sizeof retired / sizeof *retired); ++i) {
            WCHAR key[MAX_PATH], off[MAX_PATH];
            BundleRec *r = rec_find(retired[i].file);
            int was;
            FILE *g;
            swprintf_s(key, MAX_PATH, L"%s|", retired[i].file);
            was = (r && !wcscmp(r->state, L"on")) || (!r && legacy && wcsstr(legacy, key));
            if (!was) continue;
            swprintf_s(off, MAX_PATH, L"%s\\%s", mods, retired[i].folder);
            if (GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
                wcscat_s(off, MAX_PATH, L"\\disabled");
                if (_wfopen_s(&g, off, L"wb") == 0 && g) fclose(g);
            }
            if ((r = rec_add(retired[i].file)) != NULL) wcscpy_s(r->state, 48, L"retired");
            changed = 1;
        }
    }
    /* Old copies a replace left (swap_in: one held then): gone now. */
    {
        WCHAR olds[MAX_PATH];
        swprintf_s(olds, MAX_PATH, L"%s\\.old", mods);
        if (exists(olds)) remove_tree(olds, 0);
    }
    s_bundled_total = s_bundled_n = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (changed) state_save(mods);
        free(legacy);
        return;
    }
    do {
        WCHAR dir[MAX_PATH], folder[MAX_PATH], key[MAX_PATH + 2], msg[700], off[MAX_PATH];
        const WCHAR *kf;
        BundleRec *r;
        Mod bm, have;
        ZeroMemory(&have, sizeof have);
        int edited = 0, missing = 0, was_off;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ZeroMemory(&bm, sizeof bm);
        swprintf_s(zip, MAX_PATH, L"%s\\%s", bundle_dir, fd.cFileName);
        if (!zip_manifest(zip, &bm) || !(kf = kind_folder(bm.type))) continue;
        safe_id(bm.id);
        swprintf_s(folder, MAX_PATH, L"%s\\%s", kf, bm.id);
        swprintf_s(dir, MAX_PATH, L"%s\\%s", mods, folder);
        swprintf_s(off, MAX_PATH, L"%s\\disabled", dir);
        r = rec_find(fd.cFileName);
        if (r && (!wcscmp(r->state, L"removed") || !wcscmp(r->state, L"retired"))) continue;  /* (the player's say) */
        if (!r) {
            /* not in the record: the old one's installs carried over, or a new bundle */
            swprintf_s(key, MAX_PATH + 2, L"%s|", fd.cFileName);
            if (!(r = rec_add(fd.cFileName))) continue;
            wcsncpy_s(r->folder, MAX_PATH, folder, _TRUNCATE);
            changed = 1;
            if (!exists(dir)) {
                if (legacy && wcsstr(legacy, key)) {   /* (installed before, its folder gone: the player removed it) */
                    wcscpy_s(r->state, 48, L"removed");
                    swprintf_s(msg, 700, L"bundled %s: removed by the player - not installed again", fd.cFileName);
                    install_log(game_dir, msg);
                    continue;
                }
                if (bundle_install(game_dir, zip, dir, r, &bm, 1, 0)) {
                    swprintf_s(msg, 700, L"bundled %s: installed (%s)", fd.cFileName, folder);
                    install_log(game_dir, msg);
                } else {
                    s_n_recs--;  /* (not recorded: tried again next start) */
                }
                continue;
            }
            /* installed before this record (its copy never replaced to find out): the same version as the shipped
               one - its record is the bundle's files (unpacked into Mods\.compare; the copy's extra ones, e.g. its
               movie fallbacks, as they are), so a file changed or added shows as an edit and one gone as missing (Fix
               it); another version - unedited when none of its files is newer than its install (the old .bundled,
               written after each; no .bundled: unedited too - its update keeps the copy in Mods\.backup), else
               edited (asked about) */
            {
                WCHAR man[MAX_PATH], cmp[MAX_PATH];
                const WCHAR *how = L"as edited";
                BundleRec shipped;
                int i, k, same = 0;
                ZeroMemory(&have, sizeof have);
                ZeroMemory(&shipped, sizeof shipped);
                swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", dir);
                read_manifest(man, &have);
                wcsncpy_s(r->version, 32, have.version, _TRUNCATE);
                wcscpy_s(r->state, 48, L"on");
                rec_set_files(r, dir);
                swprintf_s(cmp, MAX_PATH, L"%s\\.compare", mods);
                remove_tree(cmp, 0);
                SHCreateDirectoryExW(NULL, cmp, NULL);
                if (!wcscmp(have.version, bm.version) && unzip_file(zip, cmp)) {
                    record_files(&shipped, cmp, L"", 1);
                    same = shipped.n_files > 0;
                    for (k = 0; k < shipped.n_files; ++k) {   /* (time 0: compared by its hash at the check) */
                        const BundleFile *b = &shipped.files[k];
                        int at = -1;
                        for (i = 0; i < r->n_files && at < 0; ++i)
                            if (!_wcsicmp(r->files[i].rel, b->rel)) at = i;
                        if (at < 0)
                            rec_file_add(r, b->rel, b->size, 0, b->hash);   /* (gone from the copy: missing) */
                        else if (r->files[at].hash != b->hash)
                            r->files[at].size = b->size, r->files[at].time = 0, r->files[at].hash = b->hash;
                    }
                    if (same) how = L"as the bundle's";
                } else if (wcscmp(have.version, bm.version)) {
                    WIN32_FILE_ATTRIBUTE_DATA a;
                    unsigned long long at = 0;
                    if (GetFileAttributesExW(list, GetFileExInfoStandard, &a))
                        at = (unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
                    same = 1;
                    for (i = 0; i < r->n_files && same && at; ++i)
                        same = r->files[i].time <= at + 20000000ull;   /* (2 s: the file system's times) */
                    if (same) r->carried_old = 1, how = L"as unedited (no file newer than its install)";
                }
                remove_tree(cmp, 0);
                free(shipped.files);
                if (!same)   /* (an edited copy: its record no longer matches it) */
                    for (i = 0; i < r->n_files; ++i) r->files[i].hash ^= 1, r->files[i].time = 0;
                swprintf_s(msg, 700, L"bundled %s: installed before (version %s) - carried over %s", fd.cFileName,
                           have.version[0] ? have.version : L"?", how);
                install_log(game_dir, msg);
                ++carried;
            }
        }
        if (!exists(dir)) {   /* (the player removed it) */
            wcscpy_s(r->state, 48, L"removed");
            changed = 1;
            swprintf_s(msg, 700, L"bundled %s: removed by the player - not installed again", fd.cFileName);
            install_log(game_dir, msg);
            continue;
        }
        rec_check(r, dir, &edited, &missing);
        was_off = exists(off);
        if (version_newer(bm.version, r->version)) {
            WCHAR from[32];
            wcsncpy_s(from, 32, r->version, _TRUNCATE);
            WCHAR kept[48];
            swprintf_s(kept, 48, L"kept %s", bm.version);
            if (!wcscmp(r->state, kept)) continue;   /* (the player kept theirs over this version) */
            if (!edited && !missing && r->carried_old) {   /* (unedited by its dates: its copy kept) */
                WCHAR backup[MAX_PATH];
                if (bundle_replace_keeping(game_dir, mods, kf, zip, dir, r, &bm, was_off, backup)) {
                    changed = 1;
                    swprintf_s(msg, 700, L"bundled %s: updated %s -> %s (carried over unedited), the old copy kept in %s",
                               fd.cFileName, from[0] ? from : L"?", bm.version, backup);
                    install_log(game_dir, msg);
                }
                r->carried_old = 0;
                continue;
            }
            if (!edited && !missing) {
                if (bundle_install(game_dir, zip, dir, r, &bm, 0, was_off)) {
                    changed = 1;
                    swprintf_s(msg, 700, L"bundled %s: updated %s -> %s (unedited)", fd.cFileName, from[0] ? from : L"?", bm.version);
                    install_log(game_dir, msg);
                }
                continue;
            }
            switch (answer_for(fd.cFileName)) {
            case BUNDLED_YES: {   /* update: the player's copy to Mods\.backup first */
                WCHAR backup[MAX_PATH];
                if (bundle_replace_keeping(game_dir, mods, kf, zip, dir, r, &bm, was_off, backup)) {
                    changed = 1;
                    swprintf_s(msg, 700, L"bundled %s: updated to %s, the edited copy kept in %s", fd.cFileName, bm.version, backup);
                    install_log(game_dir, msg);
                }
                break;
            }
            case BUNDLED_NO:   /* keep theirs - not asked again for this version */
                wcscpy_s(r->state, 48, kept);
                changed = 1;
                swprintf_s(msg, 700, L"bundled %s: the player kept their edited copy over version %s", fd.cFileName, bm.version);
                install_log(game_dir, msg);
                break;
            default:
                ask(game_dir, BUNDLED_ASK_UPDATE, fd.cFileName, bm.id, r->version, bm.version);
            }
            continue;
        }
        if (missing && !edited) {   /* a damaged install: a question, never a silent reinstall */
            switch (answer_for(fd.cFileName)) {
            case BUNDLED_YES:
                if (bundle_install(game_dir, zip, dir, r, &bm, 0, was_off)) {
                    changed = 1;
                    swprintf_s(msg, 700, L"bundled %s: %d missing file(s) put back (installed again)", fd.cFileName, missing);
                    install_log(game_dir, msg);
                }
                break;
            case BUNDLED_NO:
                break;   /* (not now: asked again at the next start) */
            default:
                ask(game_dir, BUNDLED_ASK_FIX, fd.cFileName, bm.id, r->version, bm.version);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (carried) {
        WCHAR msg[200];
        swprintf_s(msg, 200, L"bundled mods: %d installed copies carried over to Mods\\.bundled_state as they are", carried);
        install_log(game_dir, msg);
    }
    state_save(mods);
    free(legacy);
}

/* ── a mod's (and Custom Movies') Bink movies -> .mp4 (movie_maker.c movie_shrink; tools/movie_shrink.py
 * with ffmpeg): about half the size, played by the game as HD movies (src/hd_movies.cpp). A mod's
 * movie= / video.<id>= .bik becomes <name>.mp4 - the manifest line rewritten, the .bik deleted once the .mp4
 * is checked. Where the PC can't play the .mp4 (Windows "N", Proton) the game shows the superstar's own movie
 * (no .bik fallback is made: it would be the size saved). Not for the bundled mods (Mods\.bundled_state
 * hashes their files: they come shrunk). After an install (a worker, a progress line) and the Shrink movies
 * button (every mod and Custom Movies). ── */

typedef struct { const WCHAR *what; unsigned long long before, after; int done, failed; WCHAR err[300]; } Shrink;

static void shrink_progress(void *ctx, int pct)
{
    WCHAR t[300];
    if (!s_wnd) return;   /* (the console tests: the result only) */
    swprintf_s(t, 300, L"Shrinking movies: %s %d%%\x2026", ((Shrink *)ctx)->what, pct);
    status(t);
}

static unsigned long long file_size(const WCHAR *p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(p, GetFileExInfoStandard, &a) ? (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow : 0;
}

/* bik -> mp4 (same name), the .bik deleted (if the game holds it, kept: it is then the .mp4's fallback) */
static int shrink_movie(const WCHAR *bik, Shrink *s)
{
    WCHAR mp4[MAX_PATH], err[300];
    const unsigned long long before = file_size(bik);
    wcscpy_s(mp4, MAX_PATH, bik);
    wcscpy_s(mp4 + wcslen(mp4) - 4, 5, L".mp4");
    if (!movie_shrink(bik, mp4, NULL, shrink_progress, s, err, 300)) {
        s->failed++;
        swprintf_s(s->err, 300, L"%s: %s", s->what, err);
        return 0;
    }
    s->done++;
    s->before += before;
    s->after += file_size(mp4);
    if (!DeleteFileW(bik)) s->after += before;
    return 1;
}

static void shrink_mod(const WCHAR *dir, Shrink *s)
{
    WCHAR man[MAX_PATH], tmp[MAX_PATH];
    FILE *f, *o;
    char line[1024];
    int changed = 0;
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", dir);
    swprintf_s(tmp, MAX_PATH, L"%s\\manifest.new", dir);
    if (_wfopen_s(&f, man, L"rb") || !f) return;
    if (_wfopen_s(&o, tmp, L"wb") || !o) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '='), *e;
        WCHAR file[MAX_PATH], bik[MAX_PATH];
        int n;
        if (v && (!strncmp(line, "movie=", 6) || !strncmp(line, "video.", 6))) {
            ++v;
            for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) --e;
            if (e - v > 4 && !_strnicmp(e - 4, ".bik", 4) &&
                (n = MultiByteToWideChar(CP_UTF8, 0, v, (int)(e - v), file, MAX_PATH - 1)) > 0) {
                file[n] = 0;
                swprintf_s(bik, MAX_PATH, L"%s\\%s", dir, file);
                if (exists(bik) && shrink_movie(bik, s)) {
                    memcpy(e - 4, ".mp4", 4);   /* (the rest of the line - its end - as it was) */
                    changed = 1;
                }
            }
        }
        fputs(line, o);
    }
    fclose(f);
    fclose(o);
    if (changed) MoveFileExW(tmp, man, MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp);
}

/* Custom Movies\<name>.bik (the movie maker's older ones, downloads): <name>.mp4; the game lists it under the
   same name, so entrances keep it (src/user_movies.cpp). Not one with an HD stub (Custom Movies\.hd\<name>.bik):
   that name is an .mp4's - a mod's own movie - and the .bik the game's copy of its fallback or of the stub (all
   frames empty): made into "<name>.mp4" here it took the mod's tron over, black for a stub (2.1.8 reports) */
static void shrink_user_movies(const WCHAR *game, Shrink *s)
{
    WCHAR pat[MAX_PATH], bik[MAX_PATH], mp4[MAX_PATH], stub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\Custom Movies\\*.bik", game);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        const WCHAR *c;
        int ascii = 1;
        for (c = fd.cFileName; *c; ++c) ascii &= *c >= 32 && *c < 127;
        swprintf_s(bik, MAX_PATH, L"%s\\Custom Movies\\%s", game, fd.cFileName);
        wcscpy_s(mp4, MAX_PATH, bik);
        wcscpy_s(mp4 + wcslen(mp4) - 4, 5, L".mp4");
        swprintf_s(stub, MAX_PATH, L"%s\\Custom Movies\\.hd\\%s", game, fd.cFileName);
        if (!ascii || exists(mp4) || exists(stub)) continue;   /* (an .mp4's .bik: its stub or fallback) */
        s->what = fd.cFileName;
        shrink_movie(bik, s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int is_bundled(const WCHAR *game, const WCHAR *folder)
{
    WCHAR path[MAX_PATH], key[MAX_PATH + 4], *text;
    const WCHAR *rel = wcsstr(folder, L"\\Mods\\");
    int found;
    if (!rel) return 0;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\.bundled_state", game);
    swprintf_s(key, MAX_PATH + 4, L"\t%s\t", rel + 6);
    if (!(text = read_text(path))) return 0;
    found = wcsstr(text, key) != NULL;   /* (both written by the launcher: same case) */
    free(text);
    return found;
}

static void shrink_report(const Shrink *s, int all)
{
    WCHAR t[600];
    t[0] = 0;
    if (s->done)
        swprintf_s(t, 600, L"Movies shrunk: %d, %.1f MB -> %.1f MB (%.1f MB saved).%s%s", s->done, s->before / 1048576.0,
                   s->after / 1048576.0, (s->before - s->after) / 1048576.0, s->failed ? L" Left as it was: " : L"",
                   s->failed ? s->err : L"");
    else if (s->failed)
        swprintf_s(t, 600, L"No movie shrunk: %s", s->err);
    else if (all)
        wcscpy_s(t, 600, L"No movies to shrink: every mod and Custom Movies movie is an .mp4 already (bundled mods "
                         L"come shrunk).");
    if (t[0]) status(t);
    if (s->done || s->failed) install_log(s_game, t);
}

/* the worker: one mod folder, or ("") every superstar / media mod and Custom Movies */
static volatile LONG s_shrink_busy;
static WCHAR s_shrink_dir[MAX_PATH];

static void shrink_run(const WCHAR *dir)
{
    Shrink s;
    ZeroMemory(&s, sizeof s);
    if (dir[0]) {
        s.what = wcsrchr(dir, L'\\') ? wcsrchr(dir, L'\\') + 1 : dir;
        shrink_mod(dir, &s);
    } else {
        static const WCHAR *const kinds[] = { L"Superstars", L"Media" };
        int k;
        for (k = 0; k < 2; k++) {
            WCHAR pat[MAX_PATH];
            static WCHAR sub[MAX_PATH];
            WIN32_FIND_DATAW fd;
            HANDLE h;
            swprintf_s(pat, MAX_PATH, L"%s\\Mods\\%s\\*", s_game, kinds[k]);
            if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                swprintf_s(sub, MAX_PATH, L"%s\\Mods\\%s\\%s", s_game, kinds[k], fd.cFileName);
                if (is_bundled(s_game, sub)) continue;
                s.what = fd.cFileName;
                shrink_mod(sub, &s);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        shrink_user_movies(s_game, &s);
    }
    shrink_report(&s, !dir[0]);
}

static DWORD WINAPI shrink_thread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    shrink_run(s_shrink_dir);
    CoUninitialize();
    InterlockedExchange(&s_shrink_busy, 0);
    return 0;
}

/* dir: one mod ("" = all). With a window: on a worker (not a second one while one runs - the button does the
   rest later); without (the --mods-add / --mods-shrink tests): here. */
static void shrink_start(const WCHAR *dir)
{
    if (!s_wnd) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        shrink_run(dir);
        return;
    }
    if (InterlockedCompareExchange(&s_shrink_busy, 1, 0)) {
        if (!dir[0]) status(L"Movies are being shrunk already.");
        return;
    }
    wcscpy_s(s_shrink_dir, MAX_PATH, dir);
    if (!dir[0]) status(L"Shrinking movies\x2026");
    CloseHandle(CreateThread(NULL, 0, shrink_thread, NULL, 0, NULL));
}

int mods_shrink(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    if (!s_game[0]) return 0;
    shrink_start(L"");
    return 1;
}

/* Level music: every superstar / media mod's theme whose gain the author left out (music_level.h; a bundled mod's
   record takes its new manifest), then the Music folder's own songs. The dB per song in a window (and the log). */
static volatile LONG s_level_busy;
static WCHAR s_level_report[16384];

static void level_run(void)
{
    static const WCHAR *const kinds[] = { L"Superstars", L"Media" };
    WCHAR pat[MAX_PATH], sub[MAX_PATH], music[MAX_PATH], t[400];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int k, mods = 0, songs = 0, failed = 0, n;
    s_level_report[0] = 0;
    for (k = 0; k < 2; k++) {
        swprintf_s(pat, MAX_PATH, L"%s\\Mods\\%s\\*", s_game, kinds[k]);
        if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            swprintf_s(sub, MAX_PATH, L"%s\\Mods\\%s\\%s", s_game, kinds[k], fd.cFileName);
            n = level_mod(sub, s_level_report, 16384);
            if (n > 0) {
                mods++;
                songs += n;
                if (is_bundled(s_game, sub)) bundle_rerecord_manifest(s_game, sub);
            }
            if (n < 0) failed++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    swprintf_s(music, MAX_PATH, L"%s\\Music", s_game);
    n = level_music_folder(music, s_level_report, 16384);
    if (n > 0) songs += n;
    if (n < 0) failed++;
    if (songs)
        swprintf_s(t, 400, L"Music levelled: %d song%s (%d mod%s) at the Mod Maker's volume, -24 LUFS - the game "
                           L"applies it, the files stay as they were.%s", songs, songs == 1 ? L"" : L"s", mods,
                   mods == 1 ? L"" : L"s", failed ? L" Some manifests couldn't be written." : L"");
    else
        wcscpy_s(t, 400, L"Nothing to level: every theme and song has its volume already (an author's own is kept).");
    status(t);
    if (s_level_report[0]) {
        for (n = (int)wcslen(s_level_report); n && (s_level_report[n - 1] == L'\n' || s_level_report[n - 1] == L'\r'); --n)
            s_level_report[n - 1] = 0;
        install_log(s_game, s_level_report);
        if (s_wnd) MessageBoxW(s_wnd, s_level_report, L"Level music - the gain per song", MB_OK | MB_ICONINFORMATION);
        else wprintf(L"%s\n", s_level_report);
    }
}

static DWORD WINAPI level_thread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    level_run();
    CoUninitialize();
    InterlockedExchange(&s_level_busy, 0);
    return 0;
}

/* With a window: on a worker (not while movies are shrunk: both write manifests); without (--mods-level): here. */
static void level_start(void)
{
    if (!s_wnd) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        level_run();
        return;
    }
    if (s_shrink_busy) { status(L"Movies are being shrunk - level the music when that's done."); return; }
    if (s_bundled_running) { status(L"The bundled mods are being checked - level the music when that's done."); return; }
    if (InterlockedCompareExchange(&s_level_busy, 1, 0)) { status(L"The music is being levelled already."); return; }
    status(L"Measuring the mods' themes and the Music folder's songs\x2026");
    CloseHandle(CreateThread(NULL, 0, level_thread, NULL, 0, NULL));
}

int mods_level(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    if (!s_game[0]) return 0;
    level_start();
    return 1;
}

/* A line in <game>\Mods\.install.log (UTF-8): what an install or the bundle
   repair did when it didn't simply work - the launcher may have no window to
   say it in (installs at start-up, after an update, --bundled). */
static void install_log(const WCHAR *game, const WCHAR *text)
{
    WCHAR path[MAX_PATH];
    SYSTEMTIME st;
    FILE *f;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\.install.log", game);
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") || !f) return;
    GetLocalTime(&st);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, text);
    fclose(f);
}

static void collect_files(const WCHAR *dir, WCHAR (*files)[MAX_PATH], int *n, int cap)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || *n >= cap) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WCHAR sub[MAX_PATH];
            swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            collect_files(sub, files, n, cap);
        } else {
            swprintf_s(files[(*n)++], MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The programs holding files of `dir` open (Restart Manager), "" if none is found. */
static void who_holds(const WCHAR *dir, WCHAR *out, size_t cap)
{
    static WCHAR files[256][MAX_PATH];
    static LPCWSTR names[256];
    int n = 0, i;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = { 0 };
    out[0] = 0;
    collect_files(dir, files, &n, 256);
    if (!n || RmStartSession(&session, 0, key) != ERROR_SUCCESS) return;
    for (i = 0; i < n; ++i) names[i] = files[i];
    if (RmRegisterResources(session, (UINT)n, names, 0, NULL, 0, NULL) == ERROR_SUCCESS) {
        RM_PROCESS_INFO info[16];
        UINT need = 0, have = 16;
        DWORD reason = 0;
        if (RmGetList(session, &need, &have, info, &reason) == ERROR_SUCCESS)
            for (i = 0; i < (int)have; ++i)
                if (!wcsstr(out, info[i].strAppName)) {
                    if (out[0]) wcsncat_s(out, cap, L", ", _TRUNCATE);
                    wcsncat_s(out, cap, info[i].strAppName, _TRUNCATE);
                }
    }
    RmEndSession(session);
}

/* MoveFileW, tried a few times over ~2 s: an antivirus scanning freshly
   unpacked files, or OneDrive, holds them for a moment. */
static int move_retry(const WCHAR *from, const WCHAR *to)
{
    int i;
    for (i = 0; i < 8; ++i) {
        if (MoveFileW(from, to)) return 1;
        Sleep(250);
    }
    return 0;
}

/* The new copy of a mod (tmp) in place of the installed one (dst), never
   leaving a half: the old folder is renamed into Mods\.old first (a rename
   fails whole when a file in it is held, and then nothing changed), the new
   one moved in (on failure the old one goes back), and only then the old one
   is deleted (directly - the shell's delete took 30 s a folder here; if it
   fails, Mods\.old is emptied at the next start). It used to be recycled first: a held file left a folder with its
   manifest but no model - listed, but left out by the game (2.1.2 bundled-mod
   reports). 1 on success; else `why` says what to do. */
static int swap_in(const WCHAR *tmp, const WCHAR *dst, const WCHAR *kind, const WCHAR *id, WCHAR *why, size_t cap)
{
    WCHAR old[MAX_PATH], olds[MAX_PATH], who[512];
    int n = 0;
    if (!exists(dst)) {
        if (move_retry(tmp, dst)) return 1;
        swprintf_s(why, cap, L"The mod could not be moved into %s (Windows error %lu): close the game and the Mod Maker "
                             L"and install again.", dst, GetLastError());
        return 0;
    }
    swprintf_s(olds, MAX_PATH, L"%s\\Mods\\.old", s_game);
    SHCreateDirectoryExW(NULL, olds, NULL);
    do swprintf_s(old, MAX_PATH, L"%s\\%s-%s-%d", olds, kind, id, n++);
    while (exists(old) && n < 1000);
    if (!move_retry(dst, old)) {
        const DWORD err = GetLastError();
        who_holds(dst, who, 512);
        if (who[0]) swprintf_s(why, cap, L"The installed copy of \"%s\" is in use by %s: close that program and install "
                                         L"again (the installed copy is unchanged).", id, who);
        else swprintf_s(why, cap, L"The installed copy of \"%s\" couldn't be replaced (Windows error %lu - the game, the "
                                  L"Mod Maker, an antivirus or OneDrive may be using it): it is unchanged; close them and "
                                  L"install again.", id, err);
        return 0;
    }
    if (!move_retry(tmp, dst)) {
        const DWORD err = GetLastError();
        if (!MoveFileW(old, dst)) {
            swprintf_s(why, cap, L"\"%s\" couldn't be installed (Windows error %lu) and its old copy couldn't be put back: "
                                 L"it is in %s.", id, err, old);
            return 0;
        }
        swprintf_s(why, cap, L"\"%s\" couldn't be installed (Windows error %lu - an antivirus or OneDrive may be holding "
                             L"the new files): the installed copy is unchanged; install again.", id, err);
        return 0;
    }
    remove_tree(old, 0);  /* (the replaced copy, directly: a held file leaves it in Mods\.old for the next start) */
    return 1;
}

static int install_file(const WCHAR *zip)
{
    WCHAR tmp[MAX_PATH], man[MAX_PATH], dst[MAX_PATH], parent[MAX_PATH], t[512], kind[32];
    int shrink_now = 0;
    Mod m;
    swprintf_s(tmp, MAX_PATH, L"%s\\Mods\\.incoming", s_game);
    remove_tree(tmp, 0);
    SHCreateDirectoryExW(NULL, tmp, NULL);
    if (!unzip_file(zip, tmp)) {
        status(L"That file is not a mod (it could not be unpacked).");
        remove_tree(tmp, 0);
        return 0;
    }
    ZeroMemory(&m, sizeof m);
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", tmp);
    if (!read_manifest(man, &m)) {
        status(L"That file has no manifest.txt (type and id): it is not an SvR2011 mod.");
        remove_tree(tmp, 0);
        return 0;
    }
    {
        const int backstage = !_wcsicmp(m.type, L"backstage");
        const int arena = !_wcsicmp(m.type, L"arena") || backstage, star = !_wcsicmp(m.type, L"superstar");
        const int signs = !_wcsicmp(m.type, L"signs"), media = !_wcsicmp(m.type, L"media");
        const int moves = !_wcsicmp(m.type, L"moves");
        const int match_type = !_wcsicmp(m.type, L"matchtype");
        const int background = !_wcsicmp(m.type, L"background");
        const int attire = !_wcsicmp(m.type, L"attire");
        const int title = !_wcsicmp(m.type, L"title");  /* a championship's name / belt / pictures (Mods/Titles) */
        WCHAR need1[MAX_PATH], need2[MAX_PATH];
        if (signs || media || match_type || title) {
            wcsncpy_s(kind, 32, signs ? L"Signs" : media ? L"Media" : title ? L"Titles" : L"MatchTypes", _TRUNCATE);
            goto place;
        }
        if (background) {  /* main menu pictures: picture.dds or a pack's *.dds (1024 x 512 DXT5) */
            WIN32_FIND_DATAW pf;
            HANDLE ph;
            swprintf_s(need1, MAX_PATH, L"%s\\*.dds", tmp);
            ph = FindFirstFileW(need1, &pf);
            if (ph != INVALID_HANDLE_VALUE) FindClose(ph);
            if (ph == INVALID_HANDLE_VALUE) {
                status(L"That background mod is incomplete (it has no pictures: picture.dds).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Backgrounds", _TRUNCATE);
            goto place;
        }
        if (attire) {  /* an attire for a superstar (Mods/Attires/<id>: manifest + attire.pac) */
            swprintf_s(need1, MAX_PATH, L"%s\\attire.pac", tmp);
            if (!exists(need1)) {
                status(L"That attire mod is incomplete (it needs attire.pac).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Attires", _TRUNCATE);
            goto place;
        }
        if (moves) {  /* a move pack: pack.txt + motions (docs/MOVE_PACKS.md) */
            swprintf_s(need1, MAX_PATH, L"%s\\pack.txt", tmp);
            if (!exists(need1)) {
                status(L"That move pack is incomplete (it needs pack.txt).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Moves", _TRUNCATE);
            goto place;
        }
        if (!arena && !star) {
            swprintf_s(t, 512, L"\"%s\" is a %s mod; this version installs arena, backstage, superstar, sign, media, "
                               L"move, match type, background and attire mods.", m.name, m.type);
            status(t);
            remove_tree(tmp, 0);
            return 0;
        }
        swprintf_s(need1, MAX_PATH, L"%s\\%s", tmp, arena ? L"arena.pac" : L"ch.pac");
        swprintf_s(need2, MAX_PATH, L"%s\\%s", tmp, arena && !backstage ? L"banner.dds" : L"manifest.txt");
        if (!exists(need1) || !exists(need2)) {
            status(arena ? L"That arena mod is incomplete (it needs arena.pac and banner.dds)."
                         : L"That superstar mod is incomplete (it needs ch.pac).");
            remove_tree(tmp, 0);
            return 0;
        }
        wcsncpy_s(kind, 32, backstage ? L"Backstage" : arena ? L"Arenas" : L"Superstars", _TRUNCATE);
    }
place:
    /* (a mod with custom moves: asked first - move_warning.h; not the port's own bundled mods, and with no window to
       ask in - a test, a command line - only logged) */
    if (!s_bundle_install && move_warning_dir_has_moves(tmp)) {
        const HWND top = s_wnd ? GetAncestor(s_wnd, GA_ROOT) : NULL;
        const int headless = !top || !IsWindowVisible(top);
        if (headless) {  /* (printed by --mods-add; the log line is move_warning's) */
            swprintf_s(t, 512, L"Warning: \"%s\" has custom moves (ported or made by hand) - experimental and most likely "
                               L"broken.", m.name[0] ? m.name : m.id);
            status(t);
        }
        if (!move_warning_ask(top, s_game, m.name[0] ? m.name : m.id, headless)) {
            swprintf_s(t, 512, L"\"%s\" was not installed (it has custom moves).", m.name[0] ? m.name : m.id);
            status(t);
            remove_tree(tmp, 0);
            return 0;
        }
    }
    safe_id(m.id);
    swprintf_s(parent, MAX_PATH, L"%s\\Mods\\%s", s_game, kind);
    SHCreateDirectoryExW(NULL, parent, NULL);
    swprintf_s(dst, MAX_PATH, L"%s\\%s", parent, m.id);
    s_installed_existed = exists(dst);
    if (s_installed_existed && s_bundle_install) {
        /* (the player's copy is newer than the port's: kept) */
        WCHAR old_man[MAX_PATH];
        Mod old;
        ZeroMemory(&old, sizeof old);
        swprintf_s(old_man, MAX_PATH, L"%s\\manifest.txt", dst);
        if (read_manifest(old_man, &old) && version_newer(old.version, m.version)) {
            remove_tree(tmp, 0);
            wcscpy_s(s_installed, MAX_PATH, dst);
            return 1;
        }
    }
    {
        WCHAR off[MAX_PATH];
        int was_off;
        swprintf_s(off, MAX_PATH, L"%s\\disabled", dst);
        was_off = exists(off);  /* (a new version keeps the old one's on / off) */
        if (!swap_in(tmp, dst, kind, m.id, t, 512)) {
            status(t);
            install_log(s_game, t);
            remove_tree(tmp, 0);
            return 0;
        }
        if (was_off) {
            FILE *f;
            if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
        }
    }
    wcscpy_s(s_installed, MAX_PATH, dst);
    /* (its .bik movies -> .mp4, after the status below; not a bundled mod's - they come shrunk) */
    shrink_now = !s_bundle_install && (!wcscmp(kind, L"Superstars") || !wcscmp(kind, L"Media"));
    if (!wcscmp(kind, L"Superstars") || !wcscmp(kind, L"Media")) {
        /* its theme's volume levelled (a gain the author left out: theme_gain=), before the shrink - both write its
           manifest; a bundled mod's too (its files are recorded after this: the levelled manifest is the shipped one) */
        static WCHAR rep[2048];
        size_t n;
        rep[0] = 0;
        if (level_mod(dst, rep, 2048) > 0) {
            for (n = wcslen(rep); n && (rep[n - 1] == L'\n' || rep[n - 1] == L'\r'); --n) rep[n - 1] = 0;
            install_log(s_game, rep);
        }
    }
    scan();
    fill();
    swprintf_s(t, 512, !wcscmp(kind, L"Backstage") ? L"Installed \"%s\". Backstage mods are switched off for now: Backstage Brawl plays the game's own backstage, so it isn't loaded."
                       : !wcscmp(kind, L"Media") ? L"Installed \"%s\". It takes effect the next time the game starts."
                       : !wcscmp(kind, L"Moves") ? L"Installed \"%s\". The game merges its moves into its files when it next starts (a few seconds)."
                       : !wcscmp(kind, L"MatchTypes") ? L"Installed \"%s\". The match type is in the game's menus the "
                                                        L"next time the game starts."
                       : !wcscmp(kind, L"Backgrounds") ? L"Installed \"%s\". The main menu shows it after its own "
                                                         L"12 pictures."
                       : !wcscmp(kind, L"Signs") ? L"Installed \"%s\". The crowd holds these signs up in every match."
                       : !wcscmp(kind, L"Titles") ? L"Installed \"%s\". The game shows the title's new name, belt and "
                                                    L"picture the next time it starts."
                       : !wcscmp(kind, L"Attires") ? L"Installed \"%s\". The superstar has it on CHANGE ATTIRE the next "
                                                     L"time the game starts."
                       : wcscmp(kind, L"Arenas") ? L"Installed \"%s\". It is under the M tile of the character "
                                                   L"select (up to 137 superstar mods)."
                                                 : L"Installed \"%s\". It is on the arena select pages after the "
                                                   L"game's own arenas.",
               m.name[0] ? m.name : m.id);
    if (needs_newer(&m)) {  /* (older games leave a big model out, and crash loading a big arena) */
        WCHAR n[200];
        swprintf_s(n, 200, L" It needs game version %s or newer - this one is %s: update the game first.", m.needs,
                   PORT_VERSION);
        wcsncat_s(t, 512, n, _TRUNCATE);
    }
    if (!wcscmp(kind, L"Attires")) {  /* (two attire mods on one attire: the new one moves) */
        static WCHAR clash[2048];
        if (mods_attire_guard(s_game, dst, clash, 2048)) {
            scan();
            fill();
            wcsncat_s(t, 512, L" ", _TRUNCATE);
            wcsncat_s(t, 512, clash, _TRUNCATE);
            if (s_wnd) MessageBoxW(s_wnd, clash, L"Attire mods", MB_OK | MB_ICONINFORMATION);
        }
    }
    status(t);
    if (shrink_now) shrink_start(dst);
    return 1;
}

static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }
static void set_enabled(int i, int on);

/* The selected rows' mods (indices into s_mods), at most `max`. */
static int selected_all(int *out, int max)
{
    int row = -1, n = 0;
    while (n < max && (row = ListView_GetNextItem(s_list, row, LVNI_SELECTED)) >= 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = row;
        if (ListView_GetItem(s_list, &it) && (int)it.lParam >= 0 && (int)it.lParam < s_nmods)
            out[n++] = (int)it.lParam;
    }
    return n;
}

/* On / Off: the selected ones (through their tick, as a click on it does: the superstar limit's count too). */
static void switch_selected(int on)
{
    int row = -1, n = 0;
    while ((row = ListView_GetNextItem(s_list, row, LVNI_SELECTED)) >= 0) {
        ListView_SetCheckState(s_list, row, on);
        n++;
    }
    if (!n) status(L"Select mods in the list first (Ctrl / Shift + click for several, Ctrl+A for all).");
}

static void remove_one(int i);
static void remove_mod(void)
{
    static int sel[MODS_MAX];
    WCHAR t[512];
    int n = selected_all(sel, MODS_MAX), k, gone = 0, kept = 0;
    if (n > 1) {
        swprintf_s(t, 512, L"Remove these %d mods? Their folders go to the Recycle Bin (match types are switched off "
                           L"instead).", n);
        if (MessageBoxW(s_wnd, t, L"Remove mods", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        for (k = 0; k < n; k++) {
            const int i = sel[k];
            if (!_wcsicmp(s_mods[i].type, L"matchtype")) { set_enabled(i, 0); kept++; continue; }
            if (remove_tree(s_mods[i].folder, 1)) {
                if (!_wcsicmp(s_mods[i].type, L"superstar")) free_superstar_slot(s_game, s_mods[i].folder);
                gone++;
            }
        }
        scan();
        fill();
        swprintf_s(t, 512, L"Removed %d (in the Recycle Bin)%s%s.", gone, kept ? L"; match types switched off" : L"",
                   gone + kept < n ? L"; some could not be removed (is the game running?)" : L"");
        status(t);
        return;
    }
    remove_one(n ? sel[0] : -1);
}

static void remove_one(int i)
{
    WCHAR t[512];
    if (i < 0 || i >= s_nmods) { status(L"Select a mod in the list first."); return; }
    if (!_wcsicmp(s_mods[i].type, L"matchtype")) {
        /* (the game has a match type on unless its folder says "disabled": removing it would turn it back on) */
        set_enabled(i, 0);
        fill();
        status(L"Match types can't be removed - switched off instead (the game leaves it out of its menus).");
        return;
    }
    swprintf_s(t, 512, L"Remove \"%s\"? Its folder goes to the Recycle Bin.", s_mods[i].name);
    if (MessageBoxW(s_wnd, t, L"Remove mod", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (!remove_tree(s_mods[i].folder, 1)) { status(L"It could not be removed (is the game running?)."); return; }
    if (!_wcsicmp(s_mods[i].type, L"superstar")) free_superstar_slot(s_game, s_mods[i].folder);
    scan();
    fill();
    status(L"Removed (it is in the Recycle Bin).");
}

static void set_enabled(int i, int on)
{
    WCHAR off[MAX_PATH];
    if (i < 0 || i >= s_nmods || s_mods[i].enabled == on) return;
    if (on && removed_mod(&s_mods[i])) {  /* (switched off in the game: src/no_mods.h, src/match_types.cpp) */
        status(!_wcsicmp(s_mods[i].type, L"backstage")
                   ? L"Backstage mods are switched off for now: Backstage Brawl plays the game's own backstage."
                   : L"FREE-ROAMING BACKSTAGE is removed from the game for now.");
        return;
    }
    swprintf_s(off, MAX_PATH, L"%s\\disabled", s_mods[i].folder);
    if (on) DeleteFileW(off);
    else {
        HANDLE h = CreateFileW(off, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    s_mods[i].enabled = on;
    status(on ? L"On: it shows in the game next time it starts." : L"Off: the game leaves it out.");
}

static void open_folder(void)
{
    WCHAR dir[MAX_PATH];
    if (!s_game[0]) return;
    swprintf_s(dir, MAX_PATH, L"%s\\Mods", s_game);
    SHCreateDirectoryExW(NULL, dir, NULL);
    ShellExecuteW(s_wnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* The Mod Maker beside the launcher (left out of a release while it isn't
   ready: then the tab has no Open Mod Maker button). */
static int maker_present(void)
{
    WCHAR exe[MAX_PATH], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    wcscat_s(exe, MAX_PATH, L"\\SvR2011 Mod Maker.exe");
    return exists(exe);
}

static void open_maker(void)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], args[MAX_PATH + 16], *slash;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf_s(exe, MAX_PATH, L"%s\\SvR2011 Mod Maker.exe", dir);
    if (!exists(exe)) { status(L"SvR2011 Mod Maker.exe is not next to the launcher."); return; }
    swprintf_s(args, MAX_PATH + 16, L"--game \"%s\"", s_game);  /* (where the arenas are read from) */
    if ((INT_PTR)ShellExecuteW(s_wnd, L"open", exe, s_game[0] ? args : NULL, dir, SW_SHOWNORMAL) <= 32)
        status(L"The Mod Maker could not be started.");
}

void mods_build(HWND wnd, mods_add_fn add, int tab, int id_base, HFONT title_font)
{
    static const WCHAR *const names[] = { L"Name", L"Type", L"Version", L"Author", L"Size", L"Made with" };
    static const int widths[] = { 150, 60, 52, 84, 56, 104 };  /* (the list is 512 wide) */
    HWND h;
    HDC dc;
    int i, dpi;
    s_wnd = wnd;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    s_base = id_base;
    (void)h; (void)title_font;   /* (the page's title is the window's: "Mods" above the tab bar) */
    add(tab, L"Static", L"Arenas, superstars and more. + adds .svrmod files, \x2212 removes the selected mods, "
                        L"On / Off or the tick turns them on or off (Ctrl / Shift + click: several, Ctrl+A: all).\n"
                        L"Bundled mods are off at first - turn them on here. While the server doesn't allow mods "
                        L"online, any mod that's on keeps you out of online matches (use Start Without Mods).",
        SS_LEFT, 28, 50, 560, 72, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS | WS_BORDER | WS_TABSTOP,
                 28, 126, 512, 250, id_base + M_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 6; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 4 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_list, i, &c);
    }
    add(tab, L"Button", L"+", BS_PUSHBUTTON | WS_TABSTOP, 548, 126, 40, 34, id_base + M_ADD);
    add(tab, L"Button", L"\x2212", BS_PUSHBUTTON | WS_TABSTOP, 548, 166, 40, 34, id_base + M_REMOVE);
    /* (the selected ones - several with Ctrl / Shift, all with Ctrl+A - on or off) */
    add(tab, L"Button", L"On", BS_PUSHBUTTON | WS_TABSTOP, 548, 214, 40, 34, id_base + M_ON);
    add(tab, L"Button", L"Off", BS_PUSHBUTTON | WS_TABSTOP, 548, 254, 40, 34, id_base + M_OFF);
    /* (Shrink movies: the mods' and Custom Movies' .bik movies -> .mp4, shrink_start) */
    /* (Level music: the mods' themes and the Music folder's songs at the Mod Maker's volume, level_start) */
    if (maker_present()) {
        add(tab, L"Button", L"Open Mod Maker", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 110, 32, id_base + M_MAKER);
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 142, 386, 112, 32, id_base + M_FOLDER);
        add(tab, L"Button", L"Add background\x2026", BS_PUSHBUTTON | WS_TABSTOP, 258, 386, 112, 32, id_base + M_BACKGROUND);
        add(tab, L"Button", L"Shrink movies", BS_PUSHBUTTON | WS_TABSTOP, 374, 386, 104, 32, id_base + M_SHRINK);
        add(tab, L"Button", L"Level music", BS_PUSHBUTTON | WS_TABSTOP, 482, 386, 106, 32, id_base + M_LEVEL);
    } else {
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 130, 32, id_base + M_FOLDER);
        add(tab, L"Button", L"Add background picture\x2026", BS_PUSHBUTTON | WS_TABSTOP, 164, 386, 176, 32,
            id_base + M_BACKGROUND);
        add(tab, L"Button", L"Shrink movies", BS_PUSHBUTTON | WS_TABSTOP, 346, 386, 118, 32, id_base + M_SHRINK);
        add(tab, L"Button", L"Level music", BS_PUSHBUTTON | WS_TABSTOP, 470, 386, 118, 32, id_base + M_LEVEL);
    }
    s_status = add(tab, L"Static", L"", SS_LEFT, 28, 428, 560, 40, id_base + M_STATUS);
}

int mods_command(int id, int code)
{
    (void)code;
    if (id == s_base + M_ADD) { add_mod(); return 1; }
    if (id == s_base + M_ON || id == s_base + M_OFF) { switch_selected(id == s_base + M_ON); return 1; }
    if (id == s_base + M_REMOVE) { remove_mod(); return 1; }
    if (id == s_base + M_FOLDER) { open_folder(); return 1; }
    if (id == s_base + M_MAKER) { open_maker(); return 1; }
    if (id == s_base + M_BACKGROUND) { add_background(); return 1; }
    if (id == s_base + M_SHRINK) { shrink_start(L""); return 1; }
    if (id == s_base + M_LEVEL) { level_start(); return 1; }
    return 0;
}

/* A background's new name into its manifest.txt (name=), the other lines kept. */
/* Tests: prints the superstar mods as the Mods list shows them (left out =
   over the game's ids), and the status line's counts. */
int mods_print_superstars(const WCHAR *game)
{
    int i;
    wcsncpy_s(s_game, MAX_PATH, game, _TRUNCATE);
    scan();
    superstar_limit();
    for (i = 0; i < s_nmods; i++)
        if (!_wcsicmp(s_mods[i].type, L"superstar"))
            wprintf(L"%s\t%s\t%s\n", wcsrchr(s_mods[i].folder, L'\\') + 1, s_mods[i].enabled ? L"on" : L"off",
                    s_mods[i].left_out ? L"NOT IN GAME" : L"");
    wprintf(L"on %d, left out %d\n", s_stars_on, s_stars_out);
    return 1;
}

/* Tests: removes an installed mod as the list's Remove does (its folder to the
   Recycle Bin, a superstar mod's id freed). */
int mods_remove(const WCHAR *game, const WCHAR *folder)
{
    const int star = wcsstr(folder, L"\\Superstars\\") != NULL;
    if (!remove_tree(folder, 1)) return 0;
    if (star) free_superstar_slot(game, folder);
    return 1;
}

int mods_rename(const WCHAR *folder, const WCHAR *name)
{
    WCHAR man[MAX_PATH], tmp[MAX_PATH];
    FILE *in = NULL, *out = NULL;
    char line[1024], u[512];
    int done = 0;
    if (!name[0]) return 0;
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", folder);
    swprintf_s(tmp, MAX_PATH, L"%s\\manifest.new", folder);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    if (_wfopen_s(&out, tmp, L"wb") || !out) return 0;
    if (_wfopen_s(&in, man, L"rb") == 0 && in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, "name=", 5)) {
                if (!done) fprintf(out, "name=%s\n", u);
                done = 1;
            } else {
                fputs(line, out);
                if (line[0] && line[strlen(line) - 1] != '\n') fputs("\n", out);
            }
        }
        fclose(in);
    }
    if (!done) fprintf(out, "name=%s\n", u);
    fclose(out);
    return MoveFileExW(tmp, man, MOVEFILE_REPLACE_EXISTING) != 0;
}

int mods_notify(const NMHDR *nm)
{
    if (!s_list || nm->hwndFrom != s_list || s_filling) return 0;
    /* Renaming in place (backgrounds only: the others' names come with the
       mod). 2: the window answers TRUE, 3: FALSE (svr2011_launcher.c). */
    if (nm->code == LVN_BEGINLABELEDITW || nm->code == LVN_ENDLABELEDITW) {
        const NMLVDISPINFOW *d = (const NMLVDISPINFOW *)nm;
        LVITEMW it;
        int i;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = d->item.iItem;
        i = ListView_GetItem(s_list, &it) ? (int)it.lParam : -1;
        if (i < 0 || i >= s_nmods || _wcsicmp(s_mods[i].type, L"background")) {
            if (nm->code == LVN_BEGINLABELEDITW) status(L"Only backgrounds can be renamed here.");
            return nm->code == LVN_BEGINLABELEDITW ? 2 : 3;  /* (cancel / keep the old name) */
        }
        if (nm->code == LVN_BEGINLABELEDITW) return 3;
        if (!d->item.pszText || !d->item.pszText[0]) return 3;  /* (Esc, or an empty name) */
        if (!mods_rename(s_mods[i].folder, d->item.pszText)) {
            status(L"It could not be renamed (is the game running?).");
            return 3;
        }
        wcsncpy_s(s_mods[i].name, 128, d->item.pszText, _TRUNCATE);
        status(L"Renamed: the game's BACKGROUNDS page shows the new name from the next start.");
        return 2;
    }
    if (nm->code == LVN_KEYDOWN && ((const NMLVKEYDOWN *)nm)->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) {
        ListView_SetItemState(s_list, -1, LVIS_SELECTED, LVIS_SELECTED);   /* (Ctrl+A: all) */
        return 1;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
        {
            const int i = (int)lv->lParam, was = s_mods[i].enabled;
            set_enabled(i, ListView_GetCheckState(s_list, lv->iItem));
            if (ListView_GetCheckState(s_list, lv->iItem) != s_mods[i].enabled)  /* (refused: a backstage mod) */
                ListView_SetCheckState(s_list, lv->iItem, s_mods[i].enabled);
            if (was != s_mods[i].enabled && !_wcsicmp(s_mods[i].type, L"superstar")) {
                const int out = s_stars_out;
                superstar_limit();
                if (out || s_stars_out) {  /* (the rows' text in place: no refill inside the notification) */
                    WCHAR t[160];
                    int k;
                    for (k = 0; k < s_nmods; k++)
                        if (!_wcsicmp(s_mods[k].type, L"superstar"))
                            ListView_SetItemText(s_list, k, 1, s_mods[k].left_out ? s_over_label : s_mods[k].type);
                    if (s_stars_out)
                        swprintf_s(t, 160, L"%d of %d superstar mods fit - %d aren't in the game (%d is the limit). "
                                           L"Switch some off to choose which play.", s_stars_on - s_stars_out, s_stars_on, s_stars_out, s_star_ids);
                    else
                        swprintf_s(t, 160, L"All %d superstar mods fit (%d is the limit). They load the next time the game starts.", s_stars_on, s_star_ids);
                    status(t);
                }
            }
        }
        return 1;
    }
    return 0;
}
