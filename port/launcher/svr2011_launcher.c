/**
 * WWE SmackDown vs. Raw 2011 -- PC port launcher.
 *
 * Six tabs:
 *
 *   Play      the game folder, Play (runs svr2011.exe there), close-on-start.
 *   Settings  display mode, resolution, vsync, controller API, mute --
 *             written to svr2011.toml in the game folder, the runtime's
 *             CVar file (unknown lines in it are kept).
 *   Install   the game from the Xbox 360 disc image (ISO or XISO): its
 *             XDVDFS file system is read directly and every file copied
 *             out, then the port's program files are copied beside them.
 *
 *   DLC       copies DLC packages into the game folder (unpacked at startup).
 *   Saves     the save files (<game>\Saves): back up, restore, export, import, delete.
 *   Paint Tool the Paint Tool's logos (10 pages of 20): see them, export as PNG, import images.
 *
 * Command line (tests):  --capture <play|settings|install|dlc|saves|paint|mods> <file.bmp> [--paint-page <1-10>]
 *                          [--report-window <1|2>]  (Report a problem's window instead: the questions / as after a
 *                          failed send)
 *                          [--online-mods show | <share 0|1> <KB/s> <joiners> <mod MB> <session MB>]  (online: the
 *                          Mod sharing fold open - as loaded, or those rows filled and saved first)
 *                        --paint-export <file.pt> <slot> <out.png>  /  --paint-import <file.pt> <slot> <image>
 *                          (slot 1-200: 1-20 page 1, 21-40 page 2, ...)
 *                        --install <image> <folder>   (no window; exit code)
 *                        --apk-package <game folder> <out folder>   (Create APK Package, no window)
 *                        --report <game folder> [description]   (Report a problem: the zip, no window)
 *                        --report-send <game folder> <answers file>   (Report a problem: zip + send to the server, no window)
 *                        --reports-status <game folder> <server>   (the Reports tab's list and states, no window)
 *                        --reports-reply <game folder> <server> <report id> <text> [<picture>...]   (an answer)
 *                        --reports-edit <game folder> <server> <report id> <text> [<picture>...]   (a change)
 *                        --import-360 <360 save folder> <saves folder>   (Xbox 360 saves -> port saves)
 *                        --import-caw <saves folder> <file.cas>...   (Created Superstars + their logos)
 *                        --clean-save <saves folder> <Mods\Superstars> [--with-off] [--apply]   (save cleaner)
 *                        --adb-install <game folder>   (Install to phone over USB, no window)
 *                        --setup-check <game folder>   (the Setup tab's rows, no window)
 *                        --nat-check <game folder>   (the Online tab's connection check, no window)
 *                        --setup-install <game folder> <python|ffmpeg|all> --yes   (its downloads, no window)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <wchar.h>

#include "apk_package.h"
#include "report_urls.h"
#include "mods_tab.h"
#include "report_receipts.h"
#include "reports_tab.h"
#include "suggestions_tab.h"
#include "music_tab.h"
#include "texpacks_tab.h"
#include "setup_tab.h"
#include "nat_check.h"
#include "discover_tab.h"
#include "test_run.h"
static void setup_go_install(void);
static void setup_console_progress(int permille, const WCHAR *text);
#include "roster_tab.h"
#include "ui_theme.h"
#include "report.h"
#include "report_flow.h"
#include "report_match.h"
#include "report_send.h"
#include "movie_maker.h"
#include "ascii_fold.h"
#include "unzip.h"
#include "updater.h"
#include "release_check.h"
#include "update_log.h"
#include "stfs.h"
#include "caw_import.h"
#include "save_cleaner.h"
#include "verify.h"

#include <winhttp.h>

#ifndef PORT_VERSION
#define PORT_VERSION L"0.0.0"   /* set from port/VERSION by CMake */
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define LAUNCHER_VERSION  L"1.0"
#define GAME_TITLE        L"WWE SmackDown vs. Raw 2011"
#define WINDOW_TITLE      GAME_TITLE L" \x2014 PC Port Launcher"
/* Who made it and where it's free - on the window and the top of every page
   (copies of the port were re-uploaded, sending players to other channels). */
#define REPO_URL          L"https://github.com/KaikoClanworth1/wwe-svr2011-pc"
#define KOFI_URL          L"https://ko-fi.com/kaikoclanworth"
#define PAYPAL_URL        L"https://www.paypal.com/donate/?hosted_button_id=MS4C2WC38N3W2"
#define CREDIT_LINE       L"Launcher - Version " PORT_VERSION L" - By KaikoClanworth"
#define CREDIT_LINK       L"Download free at github.com/KaikoClanworth1/wwe-svr2011-pc"
#define MAIN_TITLE        CREDIT_LINE L" - " CREDIT_LINK
#define GAME_EXE          L"svr2011.exe"
#define GAME_TOML         L"svr2011.toml"
#define TITLE_ID          0x5451085Du     /* from the disc's default.xex execution info */
#define GAME_BYTES_EST    5690000000ull   /* about 5.3 GB of disc files */
#define COPY_CHUNK        (8u << 20)

/* ── state ─────────────────────────────────────────────────────────────── */

enum { TAB_PLAY, TAB_SETTINGS, TAB_ONLINE, TAB_INSTALL, TAB_DLC, TAB_SAVES, TAB_PAINT, TAB_MOVIES, TAB_ANDROID, TAB_MODS, TAB_MUSIC, TAB_ROSTER, TAB_TEXPACKS, TAB_SETUP, TAB_DISCOVER, TAB_TOOLS, TAB_REPORTS, TAB_SUGGESTIONS, TAB_COUNT };
#define ID_MODS_BASE 900  /* the Mods tab's controls (mods_tab.c) */
#define ID_MUSIC_BASE 950 /* the Music tab's (music_tab.c) */
#define ID_ROSTER_BASE 1000 /* the Roster tab's (roster_tab.c) */
#define ID_TEXPACKS_BASE 1100 /* the Texture packs tab's (texpacks_tab.c) */
#define ID_SETUP_BASE 1200 /* the Setup tab's (setup_tab.c) */
#define ID_DISCOVER_BASE 1300 /* Mods -> Discovery's (discover_tab.c) */
#define ID_REPORTS_BASE 1400 /* the Reports tab's (reports_tab.c) */
#define ID_SUGGEST_BASE 1450 /* the Suggestions tab's (suggestions_tab.c) */

enum {
    ID_TAB = 100,
    /* play */
    ID_GAMEDIR, ID_GAMEDIR_CHANGE, ID_PLAY, ID_CLOSE_ON_PLAY, ID_PLAY_STATUS, ID_VERSION, ID_REPORT, ID_PLAYTIME,
    ID_SKIP_INTROS, ID_SKIP_TRAINING, ID_FAST_LOADS, ID_PLAY_NOMODS, ID_PL_EXPORT, ID_PL_IMPORT, ID_PL_NOTICE,
    /* settings */
    ID_WINDOWED, ID_FULLSCREEN, ID_RESOLUTION, ID_VSYNC, ID_INPUT, ID_AUDIO, ID_MUTE, ID_SHOWFPS, ID_MSAA, ID_RENDERER, ID_LANGUAGE, ID_FRAMERATE, ID_MUSIC_OPEN, ID_DEFAULTS, ID_SAVE,
    ID_SETTINGS_STATUS, ID_PREPARE, ID_PRIORITY, ID_UNLOCK_ALL, ID_TEXTURES, ID_ARENA_LIGHT, ID_UPSCALE, ID_MATCH_CAMERA,
    ID_FULLSCREEN_EX, ID_RENDER_RES, ID_RES_HINT,
    /* online */
    ID_ON_ENABLE, ID_ON_NAME, ID_ON_SERVER, ID_ON_SERVER_KIND, ID_ON_SAVE, ID_ON_STATUS,
    ID_ON_PASSWORD, ID_ON_SIGNIN, ID_ON_REGISTER, ID_ON_SIGNOUT, ID_ON_ACCOUNT, ID_ON_SERVER_LABEL, ID_ON_PEERS, ID_ON_ROUTE,
    ID_FR_LIST, ID_FR_NAME, ID_FR_ADD, ID_FR_REMOVE, ID_FR_STATUS, ID_FR_GROUP, ID_FR_INFO,
    ID_ON_NAT_CHECK, ID_ON_NAT_GROUP, ID_ON_NAT_TEXT,
    ID_ON_SHARE, ID_ON_MODS_FOLD, ID_ON_MODS_GROUP, ID_ON_UP_KBS, ID_ON_UP_PEERS, ID_ON_MOD_MB, ID_ON_MODS_MB,
    ID_ON_MODS_L1, ID_ON_MODS_L2, ID_ON_MODS_L3, ID_ON_MODS_L4, ID_ON_MODS_INFO,
    ID_ON_L_NAME, ID_ON_L_PASS, ID_ON_L_CODE, ID_ON_CODE, ID_ON_STEP, ID_ON_BACK, ID_ON_FORGOT, ID_ON_INFO,
    /* install */
    ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_FREE, ID_INSTALL, ID_CANCEL,
    ID_PROGRESS, ID_INSTALL_STATUS, ID_VERIFY, ID_VERIFY_REPAIR, ID_VERIFY_MARK, ID_VERIFY_STATUS,
    /* android */
    ID_ADB_INSTALL, ID_ANDROID_CANCEL, ID_APK_CREATE, ID_APK_PROGRESS, ID_APK_STATUS,
    /* dlc */
    ID_DLC_DIR, ID_DLC_BROWSE, ID_DLC_INSTALL, ID_DLC_STATUS, ID_DLC_LIST,
    /* saves */
    ID_SV_LIST, ID_SV_BACKUP, ID_SV_RESTORE, ID_SV_BACKUPS, ID_SV_EXPORT, ID_SV_IMPORT, ID_SV_DELETE, ID_SV_OPEN, ID_SV_CLEAN,
    ID_SV_STATUS, ID_SV_FOLDER, ID_SV_CHANGE, ID_SV_DEFAULT, ID_SV_PRESET, ID_SV_PRESET_NEW, ID_SV_IMPORT360, ID_SV_IMPORTCAW, ID_SV_PERMOD, ID_SV_BIGROSTER,
    /* paint tool */
    ID_PT_GRID, ID_PT_EXPORT, ID_PT_IMPORT, ID_PT_DELETE, ID_PT_EXPORTALL, ID_PT_REFRESH, ID_PT_STATUS,
    ID_PT_PREV, ID_PT_NEXT, ID_PT_PAGE,
    /* movies */
    ID_MV_VIDEO, ID_MV_VIDEO_BROWSE, ID_MV_BOTTOM, ID_MV_BOTTOM_BROWSE, ID_MV_BOTTOM_NONE, ID_MV_BOTTOM_STAR, ID_MV_FIT, ID_MV_FILL,
    ID_MV_STRETCH, ID_MV_LENGTH, ID_MV_NAME, ID_MV_VIEW, ID_MV_LIST, ID_MV_DELETE, ID_MV_OPEN, ID_MV_PREVIEW,
    ID_MV_CREATE, ID_MV_STOP, ID_MV_PROGRESS, ID_MV_STATUS,
    /* updates (Play tab) */
    ID_UP_STATUS, ID_UP_BUTTON, ID_UP_AUTO, ID_UP_PROGRESS,
};

#define WM_APP_PROGRESS (WM_APP + 1)     /* wParam permille, lParam heap WCHAR* or 0 */
#define WM_APP_DONE     (WM_APP + 2)     /* wParam 1 ok / 0 failed, lParam heap WCHAR* */
#define WM_APP_GAMEEND  (WM_APP + 3)     /* wParam exit code */
#define WM_APP_DLC      (WM_APP + 4)     /* wParam 1 finished / 0 progress, lParam heap WCHAR* */
#define WM_APP_MOVIE    (WM_APP + 5)     /* wParam percent */
#define WM_APP_MOVIE_DONE (WM_APP + 6)   /* wParam 1 ok / 0 failed */
#define WM_APP_UPD_CHECKED (WM_APP + 7)  /* wParam 1 ok, lParam heap error text */
#define WM_APP_UPD_PROGRESS (WM_APP + 8) /* wParam percent, lParam 1 = unpacking */
#define WM_APP_UPD_REPAIR (WM_APP + 9)   /* the missing files fetched by themselves */
#define UPLOG_DIR (s_game_dir[0] ? s_game_dir : s_launcher_dir)   /* (update_log.h: where the updater logs too) */
#define UPDATER_EXE L"SvR2011 Updater.exe"   /* (launcher/updater_main.c: installs updates for the launcher) */
#define WM_APP_APK      (WM_APP + 10)    /* wParam permille, or APK_OK / APK_FAILED; lParam heap WCHAR* or 0 */
#define WM_APP_UPD_FINISHED (WM_APP + 13) /* wParam 1 ok, lParam heap error text */
#define WM_APP_VERIFY   (WM_APP + 12)    /* wParam permille, or 1001: done (s_verify) */
#define WM_APP_FRIENDS  (WM_APP + 11)    /* wParam HTTP status (0: no answer), lParam heap char* answer */
#define WM_APP_NATCHECK (WM_APP + 16)    /* the connection check is done (s_nat_result) */
#define WM_APP_SIZE     (WM_APP + 14)    /* the Play page's game size / file check is in (size_thread) */
#define WM_APP_BUNDLED  (WM_APP + 15)    /* wParam n << 16 | total installing, or 0xFFFFFFFF: done (bundled_thread) */
#define HERO_TIMER      0x5F02           /* the Play page's picture scrolling */
#define FRIENDS_TIMER   0x5F01           /* the Friends list's refresh while the Online tab shows */
#define APK_OK          1001
#define APK_FAILED      1002

static HINSTANCE s_inst;
static HWND      s_wnd, s_tab;
static HFONT     s_font, s_big, s_title;
static HFONT     s_icons, s_nav, s_card, s_huge;
static HFONT     s_mark_font;                     /* Verify game files' mark (the icon font, large) */  /* the look: sidebar icons and labels, card titles, page title */
static int       s_cur_tab = -1, s_nav_hover = -1;
/* The Play page (paint_play): the game's size and its program files' check (size_thread), the picture. */
static volatile LONG s_size_busy;
static uint64_t  s_size_game, s_size_mods;
static int       s_size_known, s_files_listed, s_files_bad, s_play_ready;
static WCHAR     s_size_dir[MAX_PATH], s_hero_file[MAX_PATH];
static int       s_hero_scroll, s_hero_moving;
typedef struct { RECT r; int tab; } PlayLink;
static PlayLink  s_play_links[8];
static int       s_nplay_links;
static void play_counts(void);
/* default.xex of a game folder against the supported release (verify.h). */
static int verify_xex_supported_in(const WCHAR *dir)
{
    WCHAR p[MAX_PATH];
    swprintf_s(p, MAX_PATH, L"%s\\default.xex", dir);
    return verify_xex_supported(p);
}
static void play_notice(void);
/* The Play page's notice (play_notice): one line when something needs the player, its button's action. */
enum { NOTICE_NONE, NOTICE_FOLDER, NOTICE_SETUP, NOTICE_FILES, NOTICE_UPDATE };
static WCHAR     s_notice[220];
static int       s_notice_action;
static void steps_refresh(void);
static int setup_first_step(void);
static int step_at(POINT pt);
static int mod_tab_at(POINT pt);
static RECT      s_credit_link;            /* the credit's link (top right of every page), for clicks */
static RECT      s_kofi_link;              /* "Support me: ko-fi" (bottom of the sidebar), for clicks */
static RECT      s_paypal_link;            /* ... " / PayPal" after it */
static int       s_dpi = 96;
static HICON     s_icon;
#define TAB_CTL_MAX 64   /* a page's controls (more are created but never shown) */
static HWND      s_ctl[TAB_COUNT][TAB_CTL_MAX];
static int       s_nctl[TAB_COUNT];
static WCHAR     s_launcher_dir[MAX_PATH], s_launcher_exe[MAX_PATH], s_launcher_ini[MAX_PATH];
static WCHAR     s_game_dir[MAX_PATH];
static volatile LONG s_busy, s_cancel;
static HANDLE    s_game_proc, s_worker;
static int       s_settings_dirty;
static int       s_console;               /* --install: print instead of posting */
static HANDLE    s_out;
static uint64_t  s_image_bytes;           /* game bytes on the chosen image, 0 = unknown */

/* Every control, with its layout in 96-DPI units, so a DPI change can re-lay it. */
typedef struct { HWND h; int x, y, w, hh, big; } Placed;
static Placed s_placed[192];
static int    s_nplaced;

/* Window sizes: the ones the screen reports to Windows (EnumDisplaySettings on the launcher's monitor: distinct,
   at least 854 x 480, at most the desktop's, and the desktop's own) - 5K, 8K and ultrawide where the screen has
   them, as the game's GRAPHICS -> RESOLUTION row (display_modes.cpp); else (Proton, fewer than two) this fixed
   list. Test aid: SVR2011_TEST_DISPLAY_MODES=7680x4320,5120x2880,... */
static const int k_res_fixed[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
typedef struct { int w, h, scale; WCHAR label[48]; } ResRow;
static ResRow k_res[64];
static int N_RES;

static int res_cmp(const void *a, const void *b)
{
    const ResRow *x = (const ResRow *)a, *y = (const ResRow *)b;
    long long ax = (long long)x->w * x->h, ay = (long long)y->w * y->h;
    return ax != ay ? (ax < ay ? -1 : 1) : x->w - y->w;
}

/* Adds w x h (once); its label and the emulated renderer's scale for it. */
static void res_add(int w, int h)
{
    int i;
    for (i = 0; i < N_RES; i++)
        if (k_res[i].w == w && k_res[i].h == h)
            return;
    if (N_RES >= (int)(sizeof k_res / sizeof k_res[0]))
        return;
    k_res[N_RES].w = w;
    k_res[N_RES].h = h;
    k_res[N_RES].scale = h <= 720 ? 1 : h <= 1440 ? 2 : 3;
    swprintf_s(k_res[N_RES].label, 48, L"%d \x00D7 %d%s", w, h,
               w == 1280 && h == 720 ? L"  (the console's)" : w == 3840 && h == 2160 ? L"  (4K)"
               : w == 5120 && h == 2880 ? L"  (5K)" : w == 7680 && h == 4320 ? L"  (8K)" : L"");
    N_RES++;
}

static void res_list_build(HWND near_wnd)
{
    static const POINT k_origin = { 0, 0 };
    char test[512];
    size_t n = 0;
    N_RES = 0;
    if (!getenv_s(&n, test, sizeof test, "SVR2011_TEST_DISPLAY_MODES") && n > 1) {
        char *ctx = NULL, *tok = strtok_s(test, ",", &ctx);
        for (; tok; tok = strtok_s(NULL, ",", &ctx)) {
            int w = 0, h = 0;
            if (sscanf_s(tok, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360)
                res_add(w, h);
        }
    } else if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        HMONITOR mon = near_wnd ? MonitorFromWindow(near_wnd, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(k_origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW mi;
        DEVMODEW dm;
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth) {
            const int dw = (int)dm.dmPelsWidth, dh = (int)dm.dmPelsHeight;
            DWORD i;
            for (i = 0;; i++) {
                DEVMODEW m;
                ZeroMemory(&m, sizeof m);
                m.dmSize = sizeof m;
                if (!EnumDisplaySettingsW(mi.szDevice, i, &m))
                    break;
                if ((int)m.dmPelsWidth >= 854 && (int)m.dmPelsHeight >= 480 && (int)m.dmPelsWidth <= dw &&
                    (int)m.dmPelsHeight <= dh)
                    res_add((int)m.dmPelsWidth, (int)m.dmPelsHeight);
            }
            res_add(dw, dh);
        }
        if (N_RES < 2)
            N_RES = 0;
    }
    if (!N_RES) {
        int i;
        for (i = 0; i < (int)(sizeof k_res_fixed / sizeof k_res_fixed[0]); i++)
            res_add(k_res_fixed[i][0], k_res_fixed[i][1]);
    }
    qsort(k_res, (size_t)N_RES, sizeof k_res[0], res_cmp);
    {  /* (test: SVR2011_TEST_RES_LIST=<file> - the list's entries written there, one a line) */
        WCHAR path[MAX_PATH];
        if (GetEnvironmentVariableW(L"SVR2011_TEST_RES_LIST", path, MAX_PATH)) {
            FILE *f = NULL;
            if (!_wfopen_s(&f, path, L"w, ccs=UTF-8") && f) {
                int i;
                for (i = 0; i < N_RES; i++)
                    fwprintf(f, L"%s\n", k_res[i].label);
                fclose(f);
            }
        }
    }
}
/* The render resolution: the game's GRAPHICS -> RENDER RESOLUTION steps (graphics_page.cpp kScales), saved
   in the same keys - native_max_scale, native_render_scale, native_scene_height. */
static const struct { int max_scale; double res; int height; const WCHAR *label; } k_render[] = {
    { 8, 1.0, 0, L"Auto (follows the window)" }, { 1, 0.5, 0, L"360p" }, { 1, 2.0 / 3.0, 0, L"480p" },
    { 1, 1.0, 0, L"720p (Xbox 360)" }, { 2, 1.0, 900, L"900p" }, { 2, 1.0, 1080, L"1080p" },
    { 2, 1.0, 0, L"1440p" }, { 3, 1.0, 0, L"2160p (4K)" }, { 4, 1.0, 2880, L"2880p" },
    { 6, 1.0, 4320, L"4320p (8K)" },
};
#define N_RENDER ((int)(sizeof k_render / sizeof k_render[0]))

/* As the game's GraphicsPage::Load picks the row from the three keys. */
static int render_index(int max_scale, double res, int height)
{
    int i, best = 0;
    if (height > 0) {
        for (i = 0; i < N_RENDER; i++)
            if (k_render[i].height > 0 &&
                (k_render[best].height == 0 || abs(k_render[i].height - height) < abs(k_render[best].height - height)))
                best = i;
    } else if (res > 0.0 && res < 0.99) {
        for (i = 0; i < N_RENDER; i++)
            if (k_render[i].res < 0.99 && fabs(k_render[i].res - res) < fabs(k_render[best].res - res))
                best = i;
    } else {
        for (i = 0; i < N_RENDER; i++)
            if (k_render[i].max_scale == max_scale && k_render[i].res >= 0.99 && k_render[i].height == 0)
                best = i;
    }
    return best;
}

static int S(int v) { return MulDiv(v, s_dpi, 96); }
/* big_roster (Saves tab): the option's tooltip and the warning before it is turned on */
#define BIG_ROSTER_WARNING L"Experimental - still being worked on. More mod superstars is not finished. Turning it on " \
    L"may corrupt your save or even damage your game install. Back up your Saves folder first. This also turns on " \
    L"per-mod save files. Turn it on anyway?"
static int big_roster_confirm(const WCHAR *capture_file);
static void saves_permod_lock(void);

/* ── small helpers ─────────────────────────────────────────────────────── */

static int mkdirs(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH];
    WCHAR *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + (tmp[0] && tmp[1] == L':' ? 3 : 2); *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            WCHAR c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Where downloads go: Tools\.downloads beside the launcher (the program's own folder, not %TEMP% - a
 * program that downloads into %TEMP% and unpacks programs from there looks like a dropper to antivirus
 * heuristics); %TEMP% only when that folder can't be written. */
static void download_dir(WCHAR *out)
{
    if (join(out, s_launcher_dir, L"Tools\\.downloads") && mkdirs(out))
        return;
    GetTempPathW(MAX_PATH, out);
}

static WCHAR *wdup(const WCHAR *s)
{
    size_t n = wcslen(s) + 1;
    WCHAR *d = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (d)
        wcscpy_s(d, n, s);
    return d;
}

static int64_t file_size(const WCHAR *p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

/* "1.2 GB" style. */
static void fmt_size(WCHAR *out, size_t n, uint64_t bytes)
{
    double g = bytes / 1073741824.0;
    if (g >= 1024.0)
        swprintf_s(out, n, L"%.2f TB", g / 1024.0);
    else if (g >= 1.0)
        swprintf_s(out, n, L"%.1f GB", g);
    else
        swprintf_s(out, n, L"%.0f MB", bytes / 1048576.0);
}

/* Whole megabytes with thousands separators: "2,210". */
static void fmt_mb(WCHAR *out, size_t n, uint64_t bytes)
{
    WCHAR raw[32];
    unsigned long long mb = (unsigned long long)(bytes / 1048576u);
    size_t len, i, o = 0;
    swprintf_s(raw, 32, L"%llu", mb);
    len = wcslen(raw);
    for (i = 0; i < len && o + 2 < n; i++) {
        out[o++] = raw[i];
        if ((len - i - 1) % 3 == 0 && i + 1 < len)
            out[o++] = L',';
    }
    out[o] = 0;
}

static void con_print(const WCHAR *s)
{
    DWORD n, mode;
    if (!s_out || s_out == INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleMode(s_out, &mode)) {
        WriteConsoleW(s_out, s, (DWORD)wcslen(s), &n, NULL);
    } else {
        char u[2048];
        int k = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, (int)sizeof u, NULL, NULL);
        if (k > 1)
            WriteFile(s_out, u, (DWORD)(k - 1), &n, NULL);
    }
}

static void post_progress(int permille, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    if (fmt) {
        va_start(ap, fmt);
        vswprintf_s(buf, 1024, fmt, ap);
        va_end(ap);
    }
    if (s_console) {
        if (fmt) {
            con_print(buf);
            con_print(L"\n");
        }
        return;
    }
    PostMessageW(s_wnd, WM_APP_PROGRESS, (WPARAM)permille, fmt ? (LPARAM)wdup(buf) : 0);
}

static HWND ctl(int id)
{
    return GetDlgItem(s_wnd, id);
}

static void set_text(int id, const WCHAR *t)
{
    SetWindowTextW(ctl(id), t);
}

static int is_game_folder(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    return dir[0] && join(a, dir, L"default.xex") && file_exists(a);
}

/* The first file every install has that the game folder lacks, or NULL. default.xex alone isn't enough: a folder with
   it and none of the disc's files could press Play (2.1.9 report 181cb0, Android). Install again keeps what is there. */
static const WCHAR *install_missing(const WCHAR *dir)
{
    static const WCHAR *must[] = { L"plist360.arc", L"pac\\_startup.pac", L"pac\\m.pac", L"pac\\plist360.h",
                                   L"sound\\System.pck" };
    WCHAR a[MAX_PATH];
    int i;
    for (i = 0; i < (int)(sizeof must / sizeof must[0]); i++)
        if (!join(a, dir, must[i]) || !file_exists(a))
            return must[i];
    return NULL;
}

static int has_program(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    return dir[0] && join(a, dir, GAME_EXE) && file_exists(a);
}

static int game_running(void)
{
    return s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT;
}

/* ── launcher.ini ──────────────────────────────────────────────────────── */

static void load_launcher_ini(void)
{
    GetPrivateProfileStringW(L"Launcher", L"GameFolder", L"", s_game_dir, MAX_PATH, s_launcher_ini);
    if (!s_game_dir[0] || !dir_exists(s_game_dir))
        wcscpy_s(s_game_dir, MAX_PATH, s_launcher_dir);
}

static void save_launcher_ini(void)
{
    WritePrivateProfileStringW(L"Launcher", L"GameFolder", s_game_dir, s_launcher_ini);
    WritePrivateProfileStringW(L"Launcher", L"CloseOnPlay",
        IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED ? L"1" : L"0", s_launcher_ini);
}

/* ── the disc image: XDVDFS ────────────────────────────────────────────── */

typedef struct {
    WCHAR    path[MAX_PATH];     /* relative, backslashes */
    uint32_t sector, size;
    int      is_dir;
} DiscFile;

typedef struct {
    HANDLE    f;
    uint64_t  base;              /* partition offset in the image */
    DiscFile *files;             /* directories and files, in table order */
    int       count, cap, nfiles;
    uint64_t  total;             /* bytes in files */
} Disc;

static int disc_read(Disc *d, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)(d->base + off);
    return SetFilePointerEx(d->f, li, NULL, FILE_BEGIN) && ReadFile(d->f, buf, n, &got, NULL) && got == n;
}

static int disc_add(Disc *d, const WCHAR *path, uint32_t sector, uint32_t size, int is_dir)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        DiscFile *n = (DiscFile *)realloc(d->files, sizeof(DiscFile) * (size_t)cap);
        if (!n)
            return 0;
        d->files = n;
        d->cap = cap;
    }
    wcscpy_s(d->files[d->count].path, MAX_PATH, path);
    d->files[d->count].sector = sector;
    d->files[d->count].size = size;
    d->files[d->count].is_dir = is_dir;
    d->count++;
    if (!is_dir) {
        d->nfiles++;
        d->total += size;
    }
    return 1;
}

/* One directory table, entry by entry in the order they are stored:
 *   u16 left, u16 right, u32 sector, u32 size, u8 attributes, u8 name length, name
 * 4-byte aligned; 0xFFFF where an entry would start pads to the next sector. */
static int disc_dir(Disc *d, uint32_t sector, uint32_t size, const WCHAR *prefix, int depth)
{
    uint8_t *buf;
    uint32_t off = 0;
    int ok = 1;
    if (depth > 32 || size > (16u << 20) || d->count > 1000000)
        return 0;
    buf = (uint8_t *)malloc(size);
    if (!buf)
        return 0;
    if (!disc_read(d, (uint64_t)sector * 2048, buf, size)) {
        free(buf);
        return 0;
    }
    while (ok && off + 14 <= size) {
        const uint8_t *e = buf + off;
        uint16_t left = (uint16_t)(e[0] | e[1] << 8);
        uint32_t sec, sz;
        uint8_t attr = e[12], nlen = e[13];
        WCHAR name[256], path[MAX_PATH];
        int i;
        if (left == 0xFFFF || nlen == 0) {          /* padding to the next sector */
            off = (off / 2048 + 1) * 2048;
            continue;
        }
        if (off + 14 + nlen > size) {
            ok = 0;
            break;
        }
        memcpy(&sec, e + 4, 4);
        memcpy(&sz, e + 8, 4);
        for (i = 0; i < nlen; i++)
            name[i] = (WCHAR)e[14 + i];
        name[nlen] = 0;
        if (wcschr(name, L'\\') || wcschr(name, L'/') || wcschr(name, L':') || !wcscmp(name, L"..")
                || !wcscmp(name, L".")) {
            ok = 0;                                 /* unsafe or broken */
            break;
        }
        if (wcslen(prefix) + 1 + nlen >= MAX_PATH) {
            ok = 0;
            break;
        }
        swprintf_s(path, MAX_PATH, prefix[0] ? L"%s\\%s" : L"%s%s", prefix, name);
        if (attr & 0x10) {
            ok = disc_add(d, path, sec, sz, 1) && (!sz || disc_dir(d, sec, sz, path, depth + 1));
        } else {
            ok = disc_add(d, path, sec, sz, 0);
        }
        off = (off + 14 + nlen + 3) & ~3u;
    }
    free(buf);
    return ok;
}

static void disc_close(Disc *d)
{
    if (d->f && d->f != INVALID_HANDLE_VALUE)
        CloseHandle(d->f);
    free(d->files);
    memset(d, 0, sizeof *d);
}

/* Open an image and list its files; on failure err explains why. */
static int disc_open(Disc *d, const WCHAR *image, WCHAR *err, size_t errn)
{
    static const uint64_t bases[] = { 0, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
    uint8_t vd[2048];
    int i;
    memset(d, 0, sizeof *d);
    d->f = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (d->f == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"The disc image could not be opened (error %lu).", GetLastError());
        return 0;
    }
    for (i = 0; i < 4; i++) {
        d->base = bases[i];
        if (disc_read(d, 32 * 2048, vd, sizeof vd) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            uint32_t root, rsize;
            memcpy(&root, vd + 20, 4);
            memcpy(&rsize, vd + 24, 4);
            if (!disc_dir(d, root, rsize, L"", 0)) {
                swprintf_s(err, errn, L"The disc image is damaged: its file list could not be read.");
                return 0;
            }
            return 1;
        }
    }
    swprintf_s(err, errn, L"This is not an Xbox 360 disc image (no XDVDFS file system was found in it).");
    return 0;
}

static DiscFile *disc_find(Disc *d, const WCHAR *path)
{
    int i;
    for (i = 0; i < d->count; i++)
        if (!d->files[i].is_dir && !_wcsicmp(d->files[i].path, path))
            return &d->files[i];
    return NULL;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The title ID in default.xex's execution info (optional header 0x00040006). */
static int disc_title_id(Disc *d, DiscFile *x, uint32_t *id)
{
    uint8_t hdr[24], opt[64 * 8], exe[16];
    uint64_t base = (uint64_t)x->sector * 2048;
    uint32_t n, i;
    if (x->size < 24 || !disc_read(d, base, hdr, 24) || memcmp(hdr, "XEX2", 4))
        return 0;
    n = be32(hdr + 20);
    if (n == 0 || n > 64 || 24 + n * 8 > x->size || !disc_read(d, base + 24, opt, n * 8))
        return 0;
    for (i = 0; i < n; i++) {
        if (be32(opt + i * 8) == 0x00040006u) {
            uint32_t at = be32(opt + i * 8 + 4);
            if (at + 16 > x->size || !disc_read(d, base + at, exe, 16))
                return 0;
            *id = be32(exe + 12);
            return 1;
        }
    }
    return 0;
}

/* Check an image is this game; on failure err says why. Leaves d open on success. */
static int disc_check(Disc *d, const WCHAR *image, WCHAR *err, size_t errn)
{
    DiscFile *x;
    uint32_t tid = 0;
    if (!disc_open(d, image, err, errn))
        return 0;
    x = disc_find(d, L"default.xex");
    if (!x) {
        swprintf_s(err, errn, L"The disc image has no default.xex at its root. It is not an Xbox 360 game disc, "
                              L"or it is the wrong disc.");
        return 0;
    }
    if (!disc_title_id(d, x, &tid)) {
        swprintf_s(err, errn, L"The disc image's default.xex could not be read; the image may be incomplete.");
        return 0;
    }
    if (tid != TITLE_ID) {
        swprintf_s(err, errn, L"This disc is not " GAME_TITLE L" (its title ID is %08X; the game is %08X).",
                   tid, TITLE_ID);
        return 0;
    }
    /* the release the port was made from (verify.h): another one's default.xex can't run */
    if (!verify_xex_size_supported(x->size)) {
        swprintf_s(err, errn, L"%s", VERIFY_OTHER_RELEASE);
        return 0;
    }
    {
        uint8_t *buf = (uint8_t *)malloc(4u << 20);
        uint64_t at = 0, base = (uint64_t)x->sector * 2048;
        uint32_t c = 0;
        if (!buf) return 1;   /* (no memory to check it: the install goes on) */
        while (at < x->size) {
            const DWORD n = (DWORD)(x->size - at > (4u << 20) ? (4u << 20) : x->size - at);
            if (!disc_read(d, base + at, buf, n)) {
                free(buf);
                swprintf_s(err, errn, L"The disc image's default.xex could not be read; the image may be incomplete.");
                return 0;
            }
            c = verify_crc32(c, buf, n);
            at += n;
        }
        free(buf);
        if (!verify_xex_crc_supported(c)) {
            swprintf_s(err, errn, L"%s", VERIFY_OTHER_RELEASE);
            return 0;
        }
    }
    return 1;
}

/* ── install ───────────────────────────────────────────────────────────── */

typedef struct {
    WCHAR image[MAX_PATH], target[MAX_PATH];
} InstallJob;

typedef struct {
    uint64_t  done, total, copied;
    ULONGLONG t0, last;
    const WCHAR *cur;
} Progress;

static InstallJob s_job;
static WCHAR s_install_msg[1024];

static void progress_tick(Progress *pr, int force)
{
    ULONGLONG now = GetTickCount64();
    double secs, speed;
    int pct;
    WCHAR a[32], b[32], eta[64];
    if (!force && now - pr->last < (s_console ? 2000u : 150u))
        return;
    pr->last = now;
    secs = (now - pr->t0) / 1000.0;
    speed = secs > 0.01 ? pr->copied / secs : 0.0;
    pct = (int)(pr->total ? pr->done * 100 / pr->total : 100);
    fmt_mb(a, 32, pr->done);
    fmt_mb(b, 32, pr->total);
    eta[0] = 0;
    if (speed > 1048576.0 && pr->total > pr->done) {
        double left = (pr->total - pr->done) / speed;
        if (left >= 90)
            swprintf_s(eta, 64, L"  \x2014  about %d min left", (int)(left / 60 + 0.5));
        else
            swprintf_s(eta, 64, L"  \x2014  %d s left", (int)left + 1);
    }
    if (s_console)
        post_progress(0, L"%3d%%  %s / %s MB  %6.1f MB/s  %s", pct, a, b, speed / 1048576.0, pr->cur ? pr->cur : L"");
    else
        post_progress((int)(pr->total ? pr->done * 990 / pr->total : 990),
                      L"%d%%  \x2014  %s of %s MB  \x2014  %.1f MB/s%s\nCopying %s", pct, a, b, speed / 1048576.0, eta,
                      pr->cur ? pr->cur : L"");
}

static int copy_out(Disc *d, DiscFile *df, const WCHAR *target, uint8_t *buf, Progress *pr)
{
    WCHAR dst[MAX_PATH], part[MAX_PATH], dir[MAX_PATH], *slash;
    HANDLE out;
    uint32_t left = df->size;
    uint64_t off = (uint64_t)df->sector * 2048;
    LARGE_INTEGER li;

    if (!join(dst, target, df->path) || wcslen(dst) + 5 >= MAX_PATH) {
        swprintf_s(s_install_msg, 1024, L"The install folder's path is too long for %s.", df->path);
        return 0;
    }
    if (file_size(dst) == (int64_t)df->size) {       /* already there: resume */
        pr->done += df->size;
        return 1;
    }
    swprintf_s(part, MAX_PATH, L"%s.part", dst);
    wcscpy_s(dir, MAX_PATH, dst);
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    pr->cur = df->path;
    out = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s (error %lu).", part, GetLastError());
        return 0;
    }
    if (df->size) {                                  /* reserve the space up front */
        li.QuadPart = df->size;
        if (SetFilePointerEx(out, li, NULL, FILE_BEGIN))
            SetEndOfFile(out);
        li.QuadPart = 0;
        SetFilePointerEx(out, li, NULL, FILE_BEGIN);
    }
    while (left) {
        DWORD n = left > COPY_CHUNK ? COPY_CHUNK : left, wr = 0;
        if (s_cancel) {
            CloseHandle(out);
            DeleteFileW(part);
            wcscpy_s(s_install_msg, 1024, L"Installation cancelled. Files already copied are kept; "
                                          L"install again to continue where it stopped.");
            return 0;
        }
        if (!disc_read(d, off, buf, n)) {
            CloseHandle(out);
            DeleteFileW(part);
            swprintf_s(s_install_msg, 1024, L"The disc image could not be read at %s. It may be incomplete.", df->path);
            return 0;
        }
        if (!WriteFile(out, buf, n, &wr, NULL) || wr != n) {
            DWORD e = GetLastError();
            CloseHandle(out);
            DeleteFileW(part);
            swprintf_s(s_install_msg, 1024, L"Could not write %s (error %lu). Is the disk full?", dst, e);
            return 0;
        }
        off += n;
        left -= n;
        pr->done += n;
        pr->copied += n;
        progress_tick(pr, 0);
    }
    CloseHandle(out);
    if (!MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) {
        swprintf_s(s_install_msg, 1024, L"Could not rename %s (error %lu).", part, GetLastError());
        DeleteFileW(part);
        return 0;
    }
    return 1;
}

/* The port's own files, from the launcher's folder to the install. */
/* Copies the files of <launcher folder>\<name> into <target>\<name>. */
static void copy_folder_files(const WCHAR *name, const WCHAR *target)
{
    WCHAR sdir[MAX_PATH], ddir[MAX_PATH], pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!join(sdir, s_launcher_dir, name) || !dir_exists(sdir) || !join(ddir, target, name) || !join(pat, sdir, L"*"))
        return;
    CreateDirectoryW(ddir, NULL);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (join(src, sdir, fd.cFileName) && join(dst, ddir, fd.cFileName))
            CopyFileW(src, dst, FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void copy_program(const WCHAR *target)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    const WCHAR *exe_name;
    if (same_dir(target, s_launcher_dir))
        return;
    if (join(src, s_launcher_dir, GAME_EXE) && file_exists(src) && join(dst, target, GAME_EXE))
        CopyFileW(src, dst, FALSE);
    if (join(src, s_launcher_dir, GAME_TOML) && file_exists(src) && join(dst, target, GAME_TOML))
        CopyFileW(src, dst, TRUE);                   /* keep settings already in the target */
    if (join(src, s_launcher_dir, L"Read Me.txt") && file_exists(src) && join(dst, target, L"Read Me.txt"))
        CopyFileW(src, dst, FALSE);
    if (join(pat, s_launcher_dir, L"*.dll")) {
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                if (join(src, s_launcher_dir, fd.cFileName) && join(dst, target, fd.cFileName))
                    CopyFileW(src, dst, FALSE);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    exe_name = wcsrchr(s_launcher_exe, L'\\');
    exe_name = exe_name ? exe_name + 1 : s_launcher_exe;
    if (join(dst, target, exe_name))
        CopyFileW(s_launcher_exe, dst, FALSE);
    /* The native renderer's shaders: without them it draws a black screen. */
    copy_folder_files(L"native_shaders", target);
    /* The mods that come with the port (Bundled Mods\*.svrmod). */
    copy_folder_files(L"Bundled Mods", target);
}

/* The bundled mods of <dir> (Bundled Mods\*.svrmod) installed into the game. */
static void install_bundled(const WCHAR *game, const WCHAR *dir)
{
    WCHAR b[MAX_PATH];
    if (game && game[0] && dir && dir[0] && join(b, dir, L"Bundled Mods") && dir_exists(b))
        mods_install_bundled(game, b);
}

/* The bundled mods installed on a worker (the window already shown; an update can bring many): the Play page says
   "Installing the update's mods... (n of N)" and Play waits for it (refresh_play). */
static volatile LONG s_bundled_busy;
static WCHAR s_bundled_text[160];
static HWND s_bundled_wnd;
static int s_bundled_answered;  /* (the questions of this start asked: the next pass acts on the answers) */
static int s_play_pending;  /* Play (1) / Start Without Mods (2) clicked while they were installed: started after */
static int s_up_finishing;  /* (up_finish: the update's folders being fetched - the bundled install follows) */
static int s_bundled_again;  /* (start_bundled while one ran - up_finish's new folders: once more after it) */

static void bundled_progress(int n, int total)
{
    if (s_bundled_wnd) PostMessageW(s_bundled_wnd, WM_APP_BUNDLED, (WPARAM)((n & 0xFFFF) << 16 | (total & 0xFFFF)), 0);
}

static DWORD WINAPI bundled_thread(LPVOID arg)
{
    WCHAR *dirs = (WCHAR *)arg;  /* (game \0 launcher folder) */
    const WCHAR *game = dirs, *launcher = dirs + wcslen(dirs) + 1;
    mods_set_bundled_progress(bundled_progress);
    install_bundled(game, game);
    if (launcher[0] && !same_dir(game, launcher))
        install_bundled(game, launcher);
    mods_set_bundled_progress(NULL);
    free(dirs);
    PostMessageW(s_bundled_wnd, WM_APP_BUNDLED, (WPARAM)0xFFFFFFFF, 0);
    return 0;
}

static void refresh_play(void);
static void show_tab(int t);

static void start_bundled(void)
{
    WCHAR *dirs;
    size_t g, l;
    HANDLE t;
    if (!is_game_folder(s_game_dir))
        return;
    if (InterlockedCompareExchange(&s_bundled_busy, 1, 0)) {
        s_bundled_again = 1;
        return;
    }
    g = wcslen(s_game_dir), l = wcslen(s_launcher_dir);
    dirs = (WCHAR *)calloc(g + l + 2, sizeof(WCHAR));
    if (!dirs) {
        InterlockedExchange(&s_bundled_busy, 0);
        return;
    }
    memcpy(dirs, s_game_dir, g * sizeof(WCHAR));
    memcpy(dirs + g + 1, s_launcher_dir, l * sizeof(WCHAR));
    if (!s_bundled_answered) mods_bundled_reset();  /* (a new check: its own questions; the answered pass keeps them) */
    s_bundled_wnd = s_wnd;
    wcscpy_s(s_bundled_text, 160, L"Checking the mods that came with the port\x2026");
    refresh_play();
    t = CreateThread(NULL, 0, bundled_thread, dirs, 0, NULL);
    if (!t) {
        free(dirs);
        InterlockedExchange(&s_bundled_busy, 0);
        refresh_play();
        return;
    }
    CloseHandle(t);
}

static void play_with(int no_mods);
static int s_bundled_no_ask;  /* (--capture: never a message box - the questions are in Mods\.install.log) */

/* The bundled mods' questions (mods_tab.h): asked once a check is done, then acted on by a second pass. */
static int bundled_ask(void)
{
    static BundledQuestion q[32];
    const int n = mods_bundled_questions(q, 32);
    int i;
    if (!n || s_bundled_answered || s_bundled_no_ask) return 0;
    s_bundled_answered = 1;
    for (i = 0; i < n && i < 32; ++i) {
        WCHAR t[900];
        int a;
        if (q[i].kind == BUNDLED_ASK_UPDATE) {
            swprintf_s(t, 900, L"A newer version (%s) of the bundled mod \"%s\" came with the update, but your copy "
                               L"(version %s) has been edited.\n\nYes: update it - your edited copy is kept in "
                               L"Mods\\.backup.\nNo: keep your copy (you won't be asked again for this version).",
                       q[i].ship[0] ? q[i].ship : L"?", q[i].name, q[i].have[0] ? q[i].have : L"?");
            a = MessageBoxW(s_wnd, t, WINDOW_TITLE L" - bundled mods", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        } else {
            swprintf_s(t, 900, L"Files of the bundled mod \"%s\" are missing (an antivirus, or a copy that stopped "
                               L"half-way).\n\nFix it automatically (it's installed again, its on / off kept)?\n"
                               L"No: not now (you're asked again at the next start).", q[i].name);
            a = MessageBoxW(s_wnd, t, WINDOW_TITLE L" - bundled mods", MB_YESNO | MB_ICONWARNING);
        }
        mods_bundled_answer(q[i].file, a == IDYES ? BUNDLED_YES : BUNDLED_NO);
    }
    return 1;
}

static void bundled_message(WPARAM wp)
{
    if (wp == (WPARAM)0xFFFFFFFF) {
        const int pending = s_play_pending;
        InterlockedExchange(&s_bundled_busy, 0);
        if (bundled_ask()) {  /* (answered: a second pass acts on them - the click still waits) */
            start_bundled();
            if (s_bundled_busy) return;
        }
        s_bundled_answered = 0;  /* (this round over: a later check asks again) */
        if (s_bundled_again) {  /* (asked again meanwhile: the new folders' mods - the click still waits) */
            s_bundled_again = 0;
            start_bundled();
            if (s_bundled_busy) return;
        }
        s_play_pending = 0;
        refresh_play();
        if (s_cur_tab == TAB_MODS) mods_show(s_game_dir);
        if (pending) play_with(pending == 2);  /* (the click made while they were installed) */
        return;
    }
    if (wp & 0xFFFF)
        swprintf_s(s_bundled_text, 160, s_play_pending ? L"The game starts after the mods\x2026 (%u of %u)"
                                                        : L"Installing the update's mods\x2026 (%u of %u)",
                   (unsigned)(wp >> 16 & 0xFFFF), (unsigned)(wp & 0xFFFF));
    else
        swprintf_s(s_bundled_text, 160, s_play_pending ? L"The game starts after the mods\x2026 (%u)"
                                                        : L"Installing the update's mods\x2026 (%u)",
                   (unsigned)(wp >> 16 & 0xFFFF));
    refresh_play();
}

static int cmp_sector(const void *a, const void *b)
{
    const DiscFile *x = *(const DiscFile *const *)a, *y = *(const DiscFile *const *)b;
    return x->sector < y->sector ? -1 : x->sector > y->sector;
}

/* Free bytes on the drive a (maybe not yet existing) folder would be on. */
static int free_space(const WCHAR *path, uint64_t *out)
{
    WCHAR p[MAX_PATH], *s;
    ULARGE_INTEGER fb;
    if (!path[0] || !GetFullPathNameW(path, MAX_PATH, p, NULL))
        return 0;
    while (!dir_exists(p)) {
        s = wcsrchr(p, L'\\');
        if (!s || s - p < 2)
            break;
        *s = 0;
        if (wcslen(p) == 2 && p[1] == L':')
            wcscat_s(p, MAX_PATH, L"\\");
    }
    if (!GetDiskFreeSpaceExW(p, &fb, NULL, NULL))
        return 0;
    *out = fb.QuadPart;
    return 1;
}

/* ── Verify game files (verify.h) ──────────────────────────────────────── */

static int any_game_running(void);

static VerifyResult   s_verify;
static volatile LONG  s_verify_cancel;
static volatile LONG  s_verify_busy;
static int            s_verify_state;   /* 0 not checked, 1 running, 2 OK, 3 damaged / missing, 4 can't tell */
static WCHAR          s_verify_dir[MAX_PATH];

/* The mark beside the button: a tick, a cross, a warning or a clock. */
static void verify_mark(int state)
{
    static const WCHAR glyph[] = { 0, 0xE823, 0xE73E, 0xE711, 0xE7BA };
    WCHAR t[2] = { glyph[state], 0 };
    s_verify_state = state;
    SendMessageW(ctl(ID_VERIFY_MARK), WM_SETFONT, (WPARAM)s_mark_font, FALSE);
    set_text(ID_VERIFY_MARK, t);
    InvalidateRect(ctl(ID_VERIFY_MARK), NULL, TRUE);
}

static void verify_progress(uint64_t done, uint64_t total, const WCHAR *file, void *ctx)
{
    static DWORD last;
    const DWORD now = GetTickCount();
    (void)file; (void)ctx;
    if (now - last < 100 && done < total)
        return;
    last = now;
    PostMessageW(s_wnd, WM_APP_VERIFY, total ? (WPARAM)(done * 1000 / total) : 0, 0);
}

static DWORD WINAPI verify_thread(LPVOID unused)
{
    (void)unused;
    if (!verify_game_files(s_verify_dir, &s_verify_cancel, verify_progress, NULL, &s_verify))
        s_verify.files = -1;   /* (stopped) */
    PostMessageW(s_wnd, WM_APP_VERIFY, 1001, 0);
    return 0;
}

static void start_verify(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_verify_busy, 1, 0))
        return;
    if (!s_game_dir[0] || !is_game_folder(s_game_dir)) {
        InterlockedExchange(&s_verify_busy, 0);
        verify_mark(0);
        set_text(ID_VERIFY_STATUS, L"Install the game first: there are no game files to check yet.");
        return;
    }
    wcscpy_s(s_verify_dir, MAX_PATH, s_game_dir);
    s_verify_cancel = 0;
    verify_mark(1);
    ShowWindow(ctl(ID_VERIFY_REPAIR), SW_HIDE);
    EnableWindow(ctl(ID_VERIFY), FALSE);
    set_text(ID_VERIFY_STATUS, L"Checking the game files\x2026");
    t = CreateThread(NULL, 0, verify_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        PostMessageW(s_wnd, WM_APP_VERIFY, 1001, 0);
}

static void verify_done(void)
{
    WCHAR t[600];
    InterlockedExchange(&s_verify_busy, 0);
    EnableWindow(ctl(ID_VERIFY), TRUE);
    if (s_verify.files < 0) {
        verify_mark(0);
        set_text(ID_VERIFY_STATUS, L"The check was stopped.");
        return;
    }
    verify_describe(&s_verify, t, 600);
    set_text(ID_VERIFY_STATUS, t);
    if (s_verify.other_edition || !s_verify.files)
        verify_mark(4);
    else if (s_verify.missing || s_verify.damaged)
        verify_mark(3);
    else
        verify_mark(2);
    ShowWindow(ctl(ID_VERIFY_REPAIR), s_verify.damaged && !s_verify.other_edition ? SW_SHOW : SW_HIDE);
}

/* Repair: the damaged files set aside (renamed .damaged), then Install copies them again. */
static void verify_repair(void)
{
    WCHAR t[600];
    int n;
    if (any_game_running()) {
        set_text(ID_VERIFY_STATUS, L"Close the game first.");
        return;
    }
    swprintf_s(t, 600, L"Set aside the %d damaged game file(s)? They are renamed to .damaged, and Install then "
                       L"copies them again from your disc image.", s_verify.damaged);
    if (MessageBoxW(s_wnd, t, L"Repair game files", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    n = verify_set_aside(&s_verify);
    ShowWindow(ctl(ID_VERIFY_REPAIR), SW_HIDE);
    verify_mark(0);
    swprintf_s(t, 600, L"%d file(s) set aside. Choose your disc image above and click Install to copy them "
                       L"again, then Verify game files.", n);
    set_text(ID_VERIFY_STATUS, t);
    {
        WCHAR target[MAX_PATH] = L"";
        GetWindowTextW(ctl(ID_TARGET), target, MAX_PATH);
        if (!target[0])
            set_text(ID_TARGET, s_game_dir);
    }
}

static int install_run(InstallJob *job)
{
    Disc d;
    Progress pr;
    DiscFile **order;
    uint64_t need = 0, freeb = 0;
    uint8_t *buf;
    int i, n, ok = 1, present = 0;
    WCHAR err[512], a[32], b[32];

    s_install_msg[0] = 0;
    post_progress(0, L"Reading the disc image...");
    if (!disc_check(&d, job->image, err, 512)) {
        wcscpy_s(s_install_msg, 1024, err);
        disc_close(&d);
        return 0;
    }
    if (!mkdirs(job->target)) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s.", job->target);
        disc_close(&d);
        return 0;
    }
    for (i = 0; i < d.count; i++) {
        WCHAR p[MAX_PATH];
        if (d.files[i].is_dir)
            continue;
        if (join(p, job->target, d.files[i].path) && file_size(p) == (int64_t)d.files[i].size)
            present++;
        else
            need += d.files[i].size;
    }
    if (free_space(job->target, &freeb) && freeb < need + (64ull << 20)) {
        fmt_size(a, 32, need + (64ull << 20));
        fmt_size(b, 32, freeb);
        swprintf_s(s_install_msg, 1024, L"Not enough free space: the game needs about %s more, and the drive has %s.", a, b);
        disc_close(&d);
        return 0;
    }
    /* Folders first (some may be empty), then files in disc order. */
    for (i = 0; i < d.count; i++) {
        WCHAR p[MAX_PATH];
        if (d.files[i].is_dir && join(p, job->target, d.files[i].path))
            mkdirs(p);
    }
    order = (DiscFile **)malloc(sizeof(DiscFile *) * (size_t)(d.nfiles ? d.nfiles : 1));
    buf = (uint8_t *)malloc(COPY_CHUNK);
    if (!order || !buf) {
        free(order);
        free(buf);
        disc_close(&d);
        wcscpy_s(s_install_msg, 1024, L"Out of memory.");
        return 0;
    }
    for (i = 0, n = 0; i < d.count; i++)
        if (!d.files[i].is_dir)
            order[n++] = &d.files[i];
    qsort(order, (size_t)n, sizeof *order, cmp_sector);

    memset(&pr, 0, sizeof pr);
    pr.total = d.total;
    pr.t0 = GetTickCount64();
    fmt_size(a, 32, d.total);
    post_progress(0, L"Copying %d files (%s) from the disc image...", n, a);
    for (i = 0; i < n && ok; i++)
        ok = copy_out(&d, order[i], job->target, buf, &pr);
    if (ok)
        progress_tick(&pr, 1);
    free(buf);
    free(order);
    disc_close(&d);
    if (!ok)
        return 0;
    post_progress(995, L"Copying the PC port's program files...");
    copy_program(job->target);
    install_bundled(job->target, s_launcher_dir);
    if (s_cancel) {
        wcscpy_s(s_install_msg, 1024, L"Installation cancelled.");
        return 0;
    }
    {
        double secs = (GetTickCount64() - pr.t0) / 1000.0;
        fmt_size(a, 32, pr.total);
        swprintf_s(s_install_msg, 1024, L"%d files (%s) installed in %.0f s%s%s", n, a, secs,
                   present ? L"; files already there were kept." : L".",
                   has_program(job->target) ? L"" : L" Note: " GAME_EXE L" was not found beside the launcher, so "
                                                    L"copy the port's program files into the folder before playing.");
    }
    return 1;
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    int ok;
    (void)unused;
    ok = install_run(&s_job);
    PostMessageW(s_wnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)wdup(s_install_msg));
    return 0;
}

/* ── settings: svr2011.toml ────────────────────────────────────────────── */

typedef struct { char **v; int n, cap; } Lines;

static void lines_free(Lines *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int lines_insert(Lines *l, int at, const char *s)
{
    size_t len = strlen(s) + 1;
    char *c = (char *)malloc(len);
    if (!c)
        return 0;
    memcpy(c, s, len);
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **nv = (char **)realloc(l->v, sizeof(char *) * (size_t)cap);
        if (!nv) {
            free(c);
            return 0;
        }
        l->v = nv;
        l->cap = cap;
    }
    memmove(l->v + at + 1, l->v + at, sizeof(char *) * (size_t)(l->n - at));
    l->v[at] = c;
    l->n++;
    return 1;
}

/* 1 when a "key = value" line's value is a "basic string" with no closing quote. */
static int toml_open_string(const char *line)
{
    const char *p = line, *eq;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#' || *p == '[' || !(eq = strchr(p, '=')))
        return 0;
    p = eq + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"' || !strncmp(p, "\"\"\"", 3))
        return 0;
    for (p++; *p; p++) {
        if (*p == '\\' && p[1])
            p++;
        else if (*p == '"')
            return 0;
    }
    return 1;
}

/* Lines of any length (a long value - MY MUSIC's song list - is one line). Launchers up to 2.1.2 read 1023
 * bytes at a time and wrote a long line back cut into pieces, a blank line more at every save: a string
 * broken over lines, a settings file the game could no longer read (no gpu_plugin: a black screen). Such a
 * string's pieces are joined again (the blank lines between them dropped). */
static int toml_read(const WCHAR *path, Lines *l)
{
    FILE *f;
    char *text, *s, *e, *line = NULL;
    long size;
    memset(l, 0, sizeof *l);
    if (_wfopen_s(&f, path, L"rb") || !f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || !(text = (char *)malloc((size_t)size + 1))) {
        fclose(f);
        return 0;
    }
    size = (long)fread(text, 1, (size_t)size, f);
    fclose(f);
    text[size] = 0;
    s = text;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s += 3;
    for (; s; s = e ? e + 1 : NULL) {
        size_t k;
        e = strchr(s, '\n');
        if (e)
            *e = 0;
        k = strlen(s);
        while (k && (s[k - 1] == '\r' || s[k - 1] == '\n'))
            s[--k] = 0;
        if (!e && !k)
            break;
        if (line) {   /* (a broken string's next piece) */
            char *j;
            if (!k)
                continue;
            if (!(j = (char *)realloc(line, strlen(line) + k + 1)))
                break;
            line = j;
            strcat(line, s);
        } else {
            if (!(line = _strdup(s)))
                break;
        }
        if (toml_open_string(line) && e)
            continue;
        lines_insert(l, l->n, line);
        free(line);
        line = NULL;
    }
    if (line) {
        lines_insert(l, l->n, line);
        free(line);
    }
    free(text);
    return 1;
}

/* "key = value" at the top level: the key (lowercase-sensitive, as TOML) and value text. */
static int toml_get(const char *key, char *out, size_t n);

/* The last online match's connection, from the game's log (p2p.cpp: "p2p: session <id> plays direct" / "plays
   through the relay (direct failed)" / "(set so)", once a session): the newest such line in the newest logs
   (<game>\logs\svr2011_*.log, newest first). s_route_kind: 1 direct, 2 relay - direct failed, 3 relay - the
   server's choice, 0 none (no line shown). */
static int s_route_kind;
static int s_route_mapped;  /* (the last "router port mapping" line in the logs: 1 it opened the port, -1 not, 0 none) */
static void online_route_show(void)
{
    typedef struct { WCHAR name[MAX_PATH]; ULONGLONG t; } Log;
    static Log logs[64];
    WCHAR pat[MAX_PATH], path[MAX_PATH], t[400];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, i, k, mapping = 0;
    s_route_kind = 0;
    s_route_mapped = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\logs\\svr2011_*.log", s_game_dir);
    h = s_game_dir[0] ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n < 64 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wcscpy_s(logs[n].name, MAX_PATH, fd.cFileName);
                logs[n].t = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
                n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < n; i++)   /* (newest first) */
        for (k = i + 1; k < n; k++)
            if (logs[k].t > logs[i].t) { Log x = logs[i]; logs[i] = logs[k]; logs[k] = x; }
    for (i = 0; i < n && i < 6 && (!s_route_kind || !mapping); i++) {
        FILE *f = NULL;
        char *buf;
        long size;
        swprintf_s(path, MAX_PATH, L"%s\\logs\\%s", s_game_dir, logs[i].name);
        if (_wfopen_s(&f, path, L"rb") || !f) continue;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        if (size > (8l << 20)) { fseek(f, size - (8l << 20), SEEK_SET); size = 8l << 20; } else fseek(f, 0, SEEK_SET);
        if ((buf = (char *)malloc((size_t)size + 1)) != NULL) {
            const char *p, *last = NULL;
            size = (long)fread(buf, 1, (size_t)size, f);
            buf[size] = 0;
            for (p = buf; (p = strstr(p, "p2p: session ")) != NULL; p++) {
                const char *e = strchr(p, '\n'), *pl = strstr(p, " plays ");
                if (pl && (!e || pl < e)) last = pl + 7;
            }
            if (!mapping)
                for (p = buf; (p = strstr(p, "p2p: router port mapping")) != NULL; p++) {
                    mapping = 1;
                    /* (" UDP <port> -> ...": it opened; ": none", ": UPnP ... refused"; "removed" at the game's end) */
                    if (!strncmp(p + 24, " UDP ", 5)) s_route_mapped = 1;
                    else if (strncmp(p + 24, " removed", 8)) s_route_mapped = -1;
                }
            if (last && !s_route_kind)
                s_route_kind = !strncmp(last, "direct", 6) ? 1 : strstr(last, "(set so)") && (!strchr(last, '\n') ||
                               strstr(last, "(set so)") < strchr(last, '\n')) ? 3 : 2;
            free(buf);
        }
        fclose(f);
    }
    if (s_route_kind == 1)
        wcscpy_s(t, 400, L"Last online match: direct connection");
    else if (s_route_kind == 2) {
        char port[16] = "";
        if (!toml_get("p2p_port", port, sizeof port) || !port[0]) strcpy_s(port, sizeof port, "36000");
        swprintf_s(t, 400, L"Last online match: through the relay - a direct connection failed (open UDP %S on your "
                           L"router or turn on ROUTER PORT (UPNP) in the game's OPTIONS > GRAPHICS)", port);
    } else if (s_route_kind == 3)
        wcscpy_s(t, 400, L"Last online match: through the relay (the server's choice)");
    else
        t[0] = 0;
    set_text(ID_ON_ROUTE, t);
}
static int toml_set(const char *key, const char *value);

static int toml_kv(const char *line, char *key, size_t kn, char *val, size_t vn)
{
    const char *p = line, *eq, *e;
    size_t k;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#' || *p == '[')
        return 0;
    eq = strchr(p, '=');
    if (!eq)
        return 0;
    e = eq;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    k = (size_t)(e - p);
    if (!k || k >= kn)
        return 0;
    memcpy(key, p, k);
    key[k] = 0;
    p = eq + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        k = q ? (size_t)(q - p - 1) : strlen(p + 1);
        p++;
    } else {
        const char *h = strchr(p, '#');
        k = h ? (size_t)(h - p) : strlen(p);
        while (k && (p[k - 1] == ' ' || p[k - 1] == '\t'))
            k--;
    }
    if (k >= vn)
        k = vn - 1;
    memcpy(val, p, k);
    val[k] = 0;
    return 1;
}

static void settings_path(WCHAR *out)
{
    join(out, s_game_dir, GAME_TOML);
}

/* The graphics API list: automatic (D3D12, else Vulkan, else D3D11 - the game
   checks at start), Vulkan or D3D11 (the game always draws natively - no
   emulated renderer). */
enum { RENDERER_NATIVE, RENDERER_VULKAN, RENDERER_D3D11 };

/* The game's languages (its text; the voices are English) and their Xbox 360
 * language ids (user_language). */
static const struct { const WCHAR *label; int id; } k_languages[] = {
    { L"English", 1 }, { L"Fran\x00E7" L"ais (French)", 4 }, { L"Deutsch (German)", 3 },
    { L"Espa\x00F1" L"ol (Spanish)", 5 }, { L"Italiano (Italian)", 6 },
};
#define N_LANGUAGES ((int)(sizeof k_languages / sizeof k_languages[0]))

static int language_index(int id)
{
    int i;
    for (i = 0; i < N_LANGUAGES; i++)
        if (k_languages[i].id == id)
            return i;
    return 0;
}

/* An edit control's text as a TOML string (UTF-8, at most max characters;
 * without " and \, which toml_kv doesn't read back). */
static void toml_quote_ctl(int id, int max, char *out, size_t n)
{
    WCHAR w[160];
    char u[480];
    size_t k = 0;
    const char *s;
    GetWindowTextW(ctl(id), w, 160);
    if ((int)wcslen(w) > max)
        w[max] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    out[k++] = '"';
    for (s = u; *s && k + 3 < n; s++) {
        if ((unsigned char)*s >= 0x20 && *s != '"' && *s != '\\')
            out[k++] = *s;
    }
    out[k++] = '"';
    out[k] = 0;
}

#define ONLINE_DEFAULT_SERVER L"https://sho-ti.me/svr"
#define ONLINE_CUSTOM_SERVER  L"127.0.0.1:8411"

/* The Community Creations account: its session token and XUID (the server
 * hands them out at sign in; the game reads them from svr2011.toml). */
static char s_online_token[128], s_online_xuid[32];
static int s_tab_visible_online;  /* (the Online tab is showing) */

/* The account box's modes: sign in, create an account (name, password, invite code), forgot password (the name,
   the reset code the server owner made for it, a new password), signed in. Signed in, an account the server keeps
   off online (2.1.6 with its account checks on, server.py signup.py) shows its step instead: the re-activation code
   from the server owner (accounts from before 2.1.6). Codes only - no email. */
enum { ON_SIGNIN, ON_SIGNUP, ON_FORGOT };
static int s_on_mode;
static int s_on_needs_code;   /* (signed in: the server's answer) */
static int s_on_invite = -1;  /* (sign-up: the server needs an invite code 1, doesn't 0, didn't say -1) */
static WCHAR s_on_note[300];  /* (the status line, kept across a re-layout) */
/* A test run (--capture, SVR2011_CAPTURE_ONLINE, --sign-in / --register): no account request (sign-up, sign-in,
   reset, re-activation, sign-out, the account's own GETs) goes to a server that isn't on this PC unless
   SVR2011_ALLOW_LIVE=1. A test folder's svr2011.toml with its server under an [Online] header (the launcher reads
   top-level keys only) fell back to the live server, and a scripted sign-up made a real account there (30 Oct). */
static int s_test_run;

static void on_place(int id, int x, int y, int w, int h);  /* (below: the layout's X0, as add()) */

static void online_account_show(void)
{
    static const int box[] = { ID_ON_L_NAME, ID_ON_NAME, ID_ON_L_PASS, ID_ON_PASSWORD, ID_ON_L_CODE, ID_ON_CODE,
                               ID_ON_SIGNIN, ID_ON_REGISTER, ID_ON_SIGNOUT, ID_ON_STEP, ID_ON_BACK, ID_ON_FORGOT,
                               ID_ON_INFO };
    WCHAR name[64], text[300];
    const WCHAR *info = L"";
    const int signed_in = s_online_token[0] != 0;
    int bx = 0;  /* (a button in the box's last row from this x: the status line takes two lines left of it) */
    size_t i;
    for (i = 0; i < sizeof box / sizeof box[0]; i++)
        ShowWindow(ctl(box[i]), SW_HIDE);
    GetWindowTextW(ctl(ID_ON_NAME), name, 64);
    text[0] = 0;
    if (signed_in && s_on_needs_code) {
        set_text(ID_ON_L_CODE, L"Re-activation code");
        on_place(ID_ON_L_CODE, 16, 144, 130, 20), on_place(ID_ON_CODE, 150, 140, 220, 24);
        set_text(ID_ON_STEP, L"Re-activate");
        on_place(ID_ON_STEP, 384, 139, 162, 26);
        on_place(ID_ON_SIGNOUT, 384, 199, 162, 24), bx = 384;
        swprintf_s(text, 300, L"Signed in as %s. Online needs the re-activation code the server owner made for your "
                              L"account.", name);
    } else if (signed_in) {
        set_text(ID_ON_L_NAME, L"Name");
        on_place(ID_ON_L_NAME, 16, 144, 130, 20), on_place(ID_ON_NAME, 150, 140, 220, 24);
        on_place(ID_ON_SIGNOUT, 384, 139, 162, 26);
        swprintf_s(text, 300, L"Signed in as %s.", name);
        info = L"The name is what other players see. The same account works on any PC or phone.";
    } else if (s_on_mode == ON_SIGNUP || s_on_mode == ON_FORGOT) {
        const int signup = s_on_mode == ON_SIGNUP;
        set_text(ID_ON_L_NAME, L"Name"), set_text(ID_ON_L_PASS, signup ? L"Password" : L"New password");
        set_text(ID_ON_L_CODE, !signup ? L"Reset code" : s_on_invite < 0 ? L"Invite code*" : L"Invite code");
        on_place(ID_ON_L_NAME, 16, 144, 60, 20), on_place(ID_ON_NAME, 80, 140, 160, 24);
        if (!signup || s_on_invite != 0)  /* (no code box when the server says it needs none) */
            on_place(ID_ON_L_CODE, 252, 144, 76, 20), on_place(ID_ON_CODE, 330, 140, 216, 24);
        on_place(ID_ON_L_PASS, 16, 174, 90, 20), on_place(ID_ON_PASSWORD, 110, 170, 130, 24);
        set_text(ID_ON_STEP, signup ? L"Create" : L"Set password");
        on_place(ID_ON_STEP, 346, 199, 108, 24), on_place(ID_ON_BACK, 460, 199, 86, 24), bx = 346;
        wcscpy_s(text, 300, !signup ? L"Ask the server owner for a reset code for your account (it works once, for 24 "
                                      L"hours), then choose a new password."
                            : s_on_invite == 0 ? L"Name: 3-15 characters (what players see); password: 8 or more."
                            : s_on_invite == 1 ? L"An invite code from the server owner. Name: 3-15 characters (what "
                                                 L"players see); password: 8 or more."
                                               : L"*Only if the server asks for one: an invite code from the server "
                                                 L"owner. Name: 3-15 characters (what players see); password: 8 or more.");
    } else {
        set_text(ID_ON_L_NAME, L"Name"), set_text(ID_ON_L_PASS, L"Password");
        on_place(ID_ON_L_NAME, 16, 144, 130, 20), on_place(ID_ON_NAME, 150, 140, 220, 24);
        on_place(ID_ON_L_PASS, 16, 174, 130, 20), on_place(ID_ON_PASSWORD, 150, 170, 220, 24);
        on_place(ID_ON_SIGNIN, 384, 139, 162, 26), on_place(ID_ON_REGISTER, 384, 169, 162, 26);
        on_place(ID_ON_FORGOT, 384, 199, 162, 24), bx = 384;
        wcscpy_s(text, 300, L"Not signed in. Sign in, or create an account (some servers ask for an invite code). The same "
                            L"account works on any PC or phone.");
    }
    if (bx) {
        on_place(ID_ON_ACCOUNT, 16, 200, bx - 28, 38);
    } else {
        on_place(ID_ON_ACCOUNT, 16, 200, 530, 18);
        on_place(ID_ON_INFO, 16, 219, 530, 18);
    }
    set_text(ID_ON_ACCOUNT, s_on_note[0] ? s_on_note : text);
    set_text(ID_ON_INFO, info);
    EnableWindow(ctl(ID_ON_NAME), !signed_in);
    EnableWindow(ctl(ID_ON_PASSWORD), !signed_in);
    /* (an account is its server's: sign out to change the server) */
    EnableWindow(ctl(ID_ON_SERVER_KIND), !signed_in);
    EnableWindow(ctl(ID_ON_SERVER), !signed_in);
    InvalidateRect(s_wnd, NULL, TRUE);
}

/* The server: Default (ONLINE_DEFAULT_SERVER, its address not shown) or
   Custom (the address box, e.g. a server on this PC). */
static int online_custom_server(void)
{
    return SendMessageW(ctl(ID_ON_SERVER_KIND), CB_GETCURSEL, 0, 0) == 1;
}

static void online_server_text(WCHAR *out, int n)
{
    out[0] = 0;
    if (online_custom_server())
        GetWindowTextW(ctl(ID_ON_SERVER), out, n);
    if (!out[0])
        wcscpy_s(out, n, online_custom_server() ? ONLINE_CUSTOM_SERVER : ONLINE_DEFAULT_SERVER);
}

static void online_server_show(void)
{
    const int custom = online_custom_server();
    ShowWindow(ctl(ID_ON_SERVER), s_tab_visible_online && custom ? SW_SHOW : SW_HIDE);
    set_text(ID_ON_SERVER_LABEL, custom ? L"Your own server's address (one on this PC: " ONLINE_CUSTOM_SERVER L")."
                                        : L"The port's Community Creations server.");
}

static void online_show(int enabled, const char *name, const char *server)
{
    WCHAR w[160];
    CheckDlgButton(s_wnd, ID_ON_ENABLE, enabled ? BST_CHECKED : BST_UNCHECKED);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 160);
    set_text(ID_ON_NAME, w);
    MultiByteToWideChar(CP_UTF8, 0, server, -1, w, 160);
    {
        const int custom = w[0] && wcscmp(w, ONLINE_DEFAULT_SERVER) != 0;
        SendMessageW(ctl(ID_ON_SERVER_KIND), CB_SETCURSEL, custom ? 1 : 0, 0);
        set_text(ID_ON_SERVER, custom ? w : ONLINE_CUSTOM_SERVER);
    }
    online_server_show();
    online_account_show();
}

/* A JSON string value's start ("key": "...") in text, or NULL. */
static const char *json_find(const char *text, const char *key)
{
    char k[64];
    const char *p;
    sprintf_s(k, sizeof k, "\"%s\"", key);
    p = strstr(text, k);
    if (!p)
        return NULL;
    p += strlen(k);
    while (*p == ' ' || *p == ':')
        p++;
    return p;
}

static void json_copy(const char *text, const char *key, char *out, size_t n)
{
    const char *p = json_find(text, key);
    size_t k = 0;
    out[0] = 0;
    if (!p || *p != '"')
        return;
    for (p++; *p && *p != '"' && k + 1 < n; p++)
        out[k++] = *p;
    out[k] = 0;
}

/* The test-run guard (s_test_run; test_run.h): a test run's write to a host that isn't this PC - logged
   (launcher_test_guard.log beside the launcher); 1: refuse it. SVR2011_ALLOW_LIVE=1 lets it through. */
int test_run_refuses(const WCHAR *host, const char *what)
{
    static const WCHAR *const local[] = { L"127.0.0.1", L"localhost", L"::1", L"[::1]" };
    const char *allow = getenv("SVR2011_ALLOW_LIVE");
    WCHAR log[MAX_PATH], *slash;
    FILE *f = NULL;
    size_t i;
    if (!s_test_run || (allow && !strcmp(allow, "1")))
        return 0;
    for (i = 0; i < sizeof local / sizeof local[0]; i++)
        if (!_wcsicmp(host, local[i]))
            return 0;
    if (GetModuleFileNameW(NULL, log, MAX_PATH) && (slash = wcsrchr(log, L'\\')) != NULL) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - log), L"launcher_test_guard.log");
        if (!_wfopen_s(&f, log, L"a") && f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d test run: %S to %s refused (not this PC; SVR2011_ALLOW_LIVE=1 "
                        L"lets it through)\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what, host);
            fclose(f);
        }
    }
    return 1;
}

/* The same for a server address (a URL, or host[:port]). */
int test_run_refuses_url(const WCHAR *server, const char *what)
{
    WCHAR url[300], host[200];
    URL_COMPONENTS uc;
    swprintf_s(url, 300, wcsstr(server, L"://") ? L"%s" : L"http://%s", server);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 200;
    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return s_test_run != 0;  /* (an address that can't be read: not this PC either) */
    return test_run_refuses(host, what);
}

/* An account request (a body, or the account's token) in a test run, to a host off this PC: refused and answered
   as the server's error, so the Online tab's status line shows it. Plain GETs (api/status) still go. */
static int online_test_refused(const WCHAR *host, const char *path, int writes, char *out, size_t outn)
{
    if (!writes || !test_run_refuses(host, path))
        return 0;
    sprintf_s(out, outn, "{\"error\": \"Test run: an account request to %S was refused - not a server on this PC "
              "(SVR2011_ALLOW_LIVE=1 lets it through).\"}", host);
    return 403;
}

/* Sends a JSON body (POST; body NULL: a GET) to <server><path>; the
 * answer's body (UTF-8) in out. Returns the HTTP status, 0 if the server
 * didn't answer. */
static int online_request(const char *path, const char *body, const char *token, char *out, size_t outn)
{
    WCHAR server[200], url[260], host[200], upath[400], headers[256];
    URL_COMPONENTS uc;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    DWORD status = 0, size = sizeof status, got = 0;
    size_t total = 0;
    out[0] = 0;
    online_server_text(server, 200);
    while (wcslen(server) && server[wcslen(server) - 1] == L'/')
        server[wcslen(server) - 1] = 0;
    if (!wcsstr(server, L"://")) {
        WCHAR tmp[200];
        swprintf_s(tmp, 200, L"http://%s", server);
        wcscpy_s(server, 200, tmp);
    }
    swprintf_s(url, 260, L"%s%S", server, path);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 200;
    uc.lpszUrlPath = upath;
    uc.dwUrlPathLength = 400;
    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return 0;
    if ((status = online_test_refused(host, path, body || (token && token[0]), out, outn)) != 0)
        return (int)status;
    session = WinHttpOpen(L"SvR2011 Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        WinHttpSetTimeouts(session, 8000, 8000, 15000, 15000);
        connect = WinHttpConnect(session, host, uc.nPort, 0);
    }
    if (connect)
        request = WinHttpOpenRequest(connect, body ? L"POST" : L"GET", upath, NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
        if (token && token[0])
            swprintf_s(headers, 256, L"Content-Type: application/json\r\nAuthorization: Bearer %S\r\n", token);
        else
            wcscpy_s(headers, 256, L"Content-Type: application/json\r\n");
        if (!body)
            body = "";
        if (WinHttpSendRequest(request, headers, (DWORD)-1L, (void *)body, (DWORD)strlen(body), (DWORD)strlen(body), 0)
                && WinHttpReceiveResponse(request, NULL)) {
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            while (total + 1 < outn && WinHttpReadData(request, out + total, (DWORD)(outn - 1 - total), &got) && got)
                total += got;
            out[total] = 0;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return (int)status;
}

/* Discovery's (discover_tab.c): the server's address; the account's token and name ("" signed out). */
static void discover_server(WCHAR *out, int n)
{
    online_server_text(out, n);
    if (!wcsstr(out, L"://")) {
        WCHAR tmp[300];
        swprintf_s(tmp, 300, L"http://%s", out);
        wcscpy_s(out, n, tmp);
    }
}

/* The Reports tab's (reports_tab.c): the signed-in account's token ("" signed out). */
static void reports_token(char *out, size_t n) { strcpy_s(out, n, s_online_token); }

static void discover_account(char *token, size_t token_n, WCHAR *name, size_t name_n, long long *id)
{
    strcpy_s(token, token_n, s_online_token);
    name[0] = 0;
    *id = -1;
    if (s_online_token[0] && name_n > 1)
        GetWindowTextW(ctl(ID_ON_NAME), name, (int)name_n);
    /* (the account's id: its XUID less the server's base, server.py XUID_BASE 0x0009000100000000) */
    if (s_online_token[0] && s_online_xuid[0]) {
        const unsigned long long x = _strtoui64(s_online_xuid, NULL, 16);
        if (x > 0x0009000100000000ull && x < 0x0009000200000000ull)
            *id = (long long)(x - 0x0009000100000000ull);
    }
}

static int online_post(const char *path, const char *body, const char *token, char *out, size_t outn)
{
    return online_request(path, body ? body : "{}", token, out, outn);
}

/* Text as a JSON string (UTF-8, quotes and backslashes escaped). */
static void json_quote(const WCHAR *w, char *out, size_t n)
{
    char u[512];
    const char *s;
    size_t k = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    out[k++] = '"';
    for (s = u; *s && k + 4 < n; s++) {
        if (*s == '"' || *s == '\\')
            out[k++] = '\\';
        if ((unsigned char)*s >= 0x20)
            out[k++] = *s;
    }
    out[k++] = '"';
    out[k] = 0;
}

static int settings_save(void);
static void friends_enable(void);
static void friends_fetch(const char *body);

static int json_true(const char *text, const char *key)
{
    const char *p = json_find(text, key);
    return p && !strncmp(p, "true", 4);
}

/* The status line (kept until the mode changes: "" clears it). */
static void online_note(const WCHAR *t)
{
    wcscpy_s(s_on_note, 300, t ? t : L"");
    set_text(ID_ON_ACCOUNT, s_on_note);
}

/* The server's error (UTF-8 JSON) as the status line; "what" when it has none. */
static void online_note_answer(int status, const char *answer, const WCHAR *what)
{
    char error[300];
    WCHAR msg[300];
    json_copy(answer, "error", error, sizeof error);
    if (!error[0])
        json_copy(answer, "message", error, sizeof error);
    if (!status)
        wcscpy_s(msg, 300, L"The server didn't answer. Check the address below and your connection.");
    else if (error[0])
        MultiByteToWideChar(CP_UTF8, 0, error, -1, msg, 300);
    else if (what)
        wcscpy_s(msg, 300, what);
    else
        swprintf_s(msg, 300, L"The server refused (HTTP %d). Is the address right?", status);
    online_note(msg);
}

/* An account answer's step (api/login, api/account, ...): the account still needs its re-activation code. */
static void online_steps_read(const char *answer)
{
    s_on_needs_code = json_true(answer, "needs_code");
}

/* Sign in (register: create the account - name, password, invite code) with what's typed. */
static void online_sign_in(int register_account)
{
    WCHAR name[64], password[128], code[32];
    char qname[160], qpass[300], qcode[80], body[700], answer[2048], token[128], xuid[32], account[64];
    HCURSOR old;
    int status;
    GetWindowTextW(ctl(ID_ON_NAME), name, 64);
    GetWindowTextW(ctl(ID_ON_PASSWORD), password, 128);
    GetWindowTextW(ctl(ID_ON_CODE), code, 32);
    if (!name[0] || !password[0] || (register_account && s_on_invite == 1 && !code[0])) {
        online_note(register_account ? L"Type a name and a password (and the invite code, if the server owner gave you one)."
                                     : L"Type your account's name and password.");
        return;
    }
    json_quote(name, qname, sizeof qname);
    json_quote(password, qpass, sizeof qpass);
    json_quote(code, qcode, sizeof qcode);
    /* ("client": 2 - this launcher walks the account's step; older ones get a text to update) */
    if (register_account)
        sprintf_s(body, sizeof body, "{\"name\": %s, \"password\": %s, \"invite\": %s, \"client\": 2}", qname, qpass, qcode);
    else
        sprintf_s(body, sizeof body, "{\"name\": %s, \"password\": %s, \"client\": 2}", qname, qpass);
    online_note(register_account ? L"Creating the account\x2026" : L"Signing in\x2026");
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    status = online_post(register_account ? "/api/register" : "/api/login", body, NULL, answer, sizeof answer);
    SetCursor(old);
    SecureZeroMemory(qpass, sizeof qpass);
    SecureZeroMemory(body, sizeof body);
    json_copy(answer, "token", token, sizeof token);
    if (status != 200 || !token[0]) {
        online_note_answer(status, answer, NULL);
        return;
    }
    json_copy(answer, "xuid", xuid, sizeof xuid);
    json_copy(answer, "name", account, sizeof account);
    strcpy_s(s_online_token, sizeof s_online_token, token);
    strcpy_s(s_online_xuid, sizeof s_online_xuid, xuid);
    MultiByteToWideChar(CP_UTF8, 0, account, -1, name, 64);
    set_text(ID_ON_NAME, name);
    set_text(ID_ON_PASSWORD, L"");
    set_text(ID_ON_CODE, L"");
    online_steps_read(answer);
    s_on_mode = ON_SIGNIN;
    online_note(register_account ? L"Account made: you can play online." : L"");
    CheckDlgButton(s_wnd, ID_ON_ENABLE, BST_CHECKED);
    online_account_show();
    settings_save();
    friends_enable();
    friends_fetch(NULL);
}

/* The signed-in account's step, from the server (api/account): when the Online tab opens. A token the server
   ended (the password changed or was reset) signs out. */
static void online_account_refresh(void)
{
    char answer[2048];
    int status;
    if (!s_online_token[0])
        return;
    status = online_request("/api/account", NULL, s_online_token, answer, sizeof answer);
    if (status == 200) {
        online_steps_read(answer);
    } else if (status == 401) {
        s_online_token[0] = s_online_xuid[0] = 0;
        s_on_needs_code = 0;
        settings_save();
        online_note(L"Signed out: your password was changed or reset. Sign in again.");
    }
}

/* The step button: create the account, set a new password with the reset code - or, signed in, the account's
   re-activation code. */
static void online_step(void)
{
    WCHAR name[64], code[32], password[128];
    char qname[160], qcode[80], qpass[300], body[700], answer[2048];
    HCURSOR old;
    int status;
    GetWindowTextW(ctl(ID_ON_NAME), name, 64);
    GetWindowTextW(ctl(ID_ON_CODE), code, 32);
    GetWindowTextW(ctl(ID_ON_PASSWORD), password, 128);
    json_quote(name, qname, sizeof qname);
    json_quote(code, qcode, sizeof qcode);
    json_quote(password, qpass, sizeof qpass);
    if (!s_online_token[0]) {
        if (s_on_mode == ON_SIGNUP) {
            online_sign_in(1);
            return;
        }
        if (!name[0] || !code[0] || !password[0]) {
            online_note(L"Type your account's name, the reset code and a new password.");
            return;
        }
        sprintf_s(body, sizeof body, "{\"name\": %s, \"code\": %s, \"password\": %s}", qname, qcode, qpass);
        old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        status = online_post("/api/password/reset", body, NULL, answer, sizeof answer);
        SetCursor(old);
        SecureZeroMemory(qpass, sizeof qpass);
        SecureZeroMemory(body, sizeof body);
        if (status == 200) {
            s_on_mode = ON_SIGNIN;
            set_text(ID_ON_PASSWORD, L"");
            set_text(ID_ON_CODE, L"");
            online_account_show();
        }
        online_note_answer(status, answer, NULL);
        return;
    }
    if (!code[0]) {
        online_note(L"Type the code.");
        return;
    }
    sprintf_s(body, sizeof body, "{\"code\": %s}", qcode);
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    status = online_post("/api/reactivate", body, s_online_token, answer, sizeof answer);
    SetCursor(old);
    if (status == 200) {
        online_steps_read(answer);
        set_text(ID_ON_CODE, L"");
        online_note(s_on_needs_code ? L"" : L"Account re-activated: you can play online.");
        online_account_show();
        friends_fetch(NULL);
    } else {
        online_note_answer(status, answer, NULL);
    }
}

static void online_sign_out(void)
{
    char answer[256];
    if (s_online_token[0])
        online_post("/api/logout", "{}", s_online_token, answer, sizeof answer);
    s_online_token[0] = 0;
    s_online_xuid[0] = 0;
    s_on_needs_code = 0;
    s_on_mode = ON_SIGNIN;
    online_note(L"");
    online_account_show();
    settings_save();
    friends_enable();
}

/* -- Friends: the account's list on the server (/api/friends), who's online
 * (in the game's ONLINE menus) or in an online match (through the server's
 * relay), and who added this player. Fetched in the background: on the
 * Online tab, every 20 seconds while it shows. */

static volatile LONG s_fr_busy;
static char s_fr_job[256];  /* (the POST body; empty: a GET) */
static char s_fr_token[128];

static DWORD WINAPI friends_thread(void *arg)
{
    char *answer = (char *)malloc(32768);
    int status;
    (void)arg;
    if (!answer) {
        InterlockedExchange(&s_fr_busy, 0);
        return 0;
    }
    status = online_request("/api/friends", s_fr_job[0] ? s_fr_job : NULL, s_fr_token, answer, 32768);
    PostMessageW(s_wnd, WM_APP_FRIENDS, (WPARAM)status, (LPARAM)answer);
    return 0;
}

/* body NULL: just the list; else {"add": name} / {"remove": name}. */
static void friends_fetch(const char *body)
{
    if (!s_online_token[0] || InterlockedCompareExchange(&s_fr_busy, 1, 0))
        return;
    strcpy_s(s_fr_job, sizeof s_fr_job, body ? body : "");
    strcpy_s(s_fr_token, sizeof s_fr_token, s_online_token);
    CloseHandle(CreateThread(NULL, 0, friends_thread, NULL, 0, NULL));
}

static void friends_enable(void)
{
    int signed_in = s_online_token[0] != 0;
    EnableWindow(ctl(ID_FR_NAME), signed_in);
    EnableWindow(ctl(ID_FR_ADD), signed_in);
    EnableWindow(ctl(ID_FR_REMOVE), signed_in);
    if (!signed_in) {
        ListView_DeleteAllItems(ctl(ID_FR_LIST));
        set_text(ID_FR_STATUS, L"Sign in to see your friends and who's online.");
    }
}

/* The next JSON string after `at` (UTF-8 to UTF-16), or NULL. */
static const char *friends_string(const char *at, const char *end, WCHAR *out, int n)
{
    char u[128];
    size_t k = 0;
    const char *q = strchr(at, '"');
    out[0] = 0;
    if (!q || q >= end)
        return NULL;
    for (q++; *q && *q != '"' && q < end; q++)
        if (k + 1 < sizeof u) u[k++] = *q;
    u[k] = 0;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, n);
    return *q == '"' ? q + 1 : NULL;
}

static void friends_add_row(HWND list, const WCHAR *name, const WCHAR *status)
{
    LVITEMW it;
    ZeroMemory(&it, sizeof it);
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(list);
    it.pszText = (WCHAR *)name;
    it.iItem = ListView_InsertItem(list, &it);
    ListView_SetItemText(list, it.iItem, 1, (WCHAR *)status);
}

static void friends_show(int http, char *answer)
{
    HWND list = ctl(ID_FR_LIST);
    WCHAR msg[200], name[64], keep[64] = L"";
    int total = 0, online = 0, sel;
    const char *p, *end;
    InterlockedExchange(&s_fr_busy, 0);
    if (!s_online_token[0]) {
        friends_enable();
        return;
    }
    if (http != 200 || !strstr(answer, "\"friends\"")) {
        char error[160];
        json_copy(answer, "error", error, sizeof error);
        if (!http)
            wcscpy_s(msg, 200, L"The server didn't answer.");
        else if (error[0])
            swprintf_s(msg, 200, L"%S", error);
        else
            swprintf_s(msg, 200, http == 404 ? L"This server has no friends list yet." : L"The server refused (HTTP %d).", http);
        set_text(ID_FR_STATUS, msg);
        return;
    }
    sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel >= 0)
        ListView_GetItemText(list, sel, 0, keep, 64);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    p = strstr(answer, "\"friends\"");
    end = p ? strchr(p, ']') : NULL;
    while (p && end && (p = strchr(p, '{')) && p < end) {
        const char *close = strchr(p, '}');
        const char *v;
        WCHAR status[64];
        const WCHAR *label = L"Offline";
        if (!close)
            break;
        v = strstr(p, "\"name\"");
        if (v && v < close && friends_string(v + 6, close, name, 64)) {
            v = strstr(p, "\"status\"");
            status[0] = 0;
            if (v && v < close)
                friends_string(v + 8, close, status, 64);
            if (!wcscmp(status, L"playing"))
                label = L"In an online match", online++;
            else if (!wcscmp(status, L"online"))
                label = L"Online", online++;
            else if (!wcscmp(status, L"ingame"))
                label = L"In the game";
            v = strstr(p, "\"mutual\"");
            if (v && v < close && strncmp(v + 8, ": false", 7) == 0 && wcscmp(label, L"Offline") == 0)
                label = L"Offline (hasn't added you)";
            friends_add_row(list, name, label);
            total++;
        }
        p = close + 1;
    }
    p = strstr(answer, "\"added_you\"");
    end = p ? strchr(p, ']') : NULL;
    if (p && end) {
        const char *q = strchr(p, '[');
        while (q && (q = friends_string(q + 1, end, name, 64)) != NULL)
            friends_add_row(list, name, L"Added you (Add to add them back)");
    }
    if (keep[0]) {  /* (the selection stays across refreshes) */
        LVFINDINFOW f;
        int i;
        ZeroMemory(&f, sizeof f);
        f.flags = LVFI_STRING;
        f.psz = keep;
        i = ListView_FindItem(list, -1, &f);
        if (i >= 0)
            ListView_SetItemState(list, i, LVIS_SELECTED, LVIS_SELECTED);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    if (!total)
        wcscpy_s(msg, 200, L"No friends yet: type a player's name and press Add.");
    else
        swprintf_s(msg, 200, L"%d of %d friend%s online.", online, total, total == 1 ? L"" : L"s");
    {
        const char *now = strstr(answer, "\"online_now\"");
        if (now && (now = strchr(now, ':')) != NULL) {
            const int n = atoi(now + 1);
            const size_t len = wcslen(msg);
            swprintf_s(msg + len, 200 - len, L"   %d player%s playing now.", n, n == 1 ? L"" : L"s");
        }
    }
    set_text(ID_FR_STATUS, msg);
}

/* Add / Remove the player named in the box. */
static void friends_change(int add)
{
    WCHAR name[64];
    char q[160], body[256];
    GetWindowTextW(ctl(ID_FR_NAME), name, 64);
    if (!name[0]) {
        set_text(ID_FR_STATUS, add ? L"Type the player's name to add." : L"Select a friend (or type the name) to remove.");
        return;
    }
    json_quote(name, q, sizeof q);
    sprintf_s(body, sizeof body, "{\"%s\": %s}", add ? "add" : "remove", q);
    if (s_fr_busy) {
        set_text(ID_FR_STATUS, L"One moment\x2026");
        return;
    }
    friends_fetch(body);
    if (add)
        set_text(ID_FR_NAME, L"");
}

/* Frame rate choices (the game's frame_rate setting: frames a second at most;
 * the game runs at its normal speed at either). */
static const int k_frame_rates[] = { 30, 60 };
#define N_FRAME_RATES 2

static int frame_rate_index(int fps)
{
    int i;
    for (i = 0; i < N_FRAME_RATES; i++)
        if (k_frame_rates[i] == fps)
            return i;
    return 1;
}

/* An NVIDIA display adapter (DLSS: the game checks for an RTX GPU itself and falls back to FSR 2). */
static int nvidia_adapter(void)
{
    DISPLAY_DEVICEW d;
    DWORD i;
    for (i = 0;; i++) {
        memset(&d, 0, sizeof d);
        d.cb = sizeof d;
        if (!EnumDisplayDevicesW(NULL, i, &d, 0))
            return 0;
        if (wcsstr(d.DeviceID, L"VEN_10DE"))
            return 1;
    }
}

/* The Upscaling list (the game's GRAPHICS -> UPSCALING): off, FSR 1 ultra quality .. performance, FSR 2
   (experimental) - not with Direct3D 11 (no compute) - and DLSS (experimental) - Direct3D 12 (AUTO) or Vulkan, with
   nvngx_dlss.dll in the game folder and an NVIDIA GPU; `sel` chosen (one not offered: the same FSR 1 level). */
static void upscale_fill(int sel)
{
    static const WCHAR *const names[] = { L"Upscaling: off", L"FSR 1 Ultra Quality", L"FSR 1 Quality", L"FSR 1 Balanced",
                                          L"FSR 1 Performance", L"FSR 2 Ultra Quality (exp.)", L"FSR 2 Quality (exp.)",
                                          L"FSR 2 Balanced (exp.)", L"FSR 2 Performance (exp.)",
                                          L"DLSS Ultra Quality (exp.)", L"DLSS Quality (exp.)", L"DLSS Balanced (exp.)",
                                          L"DLSS Performance (exp.)" };
    HWND h = ctl(ID_UPSCALE);
    const int renderer = (int)SendMessageW(ctl(ID_RENDERER), CB_GETCURSEL, 0, 0);
    const int fsr2 = renderer != RENDERER_D3D11;
    WCHAR dll[MAX_PATH];
    const int dlss = renderer != RENDERER_D3D11 && s_game_dir[0] && join(dll, s_game_dir, L"nvngx_dlss.dll") &&
                     file_exists(dll) && nvidia_adapter();
    const int n = dlss ? 13 : fsr2 ? 9 : 5;
    int i;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)names[i]);
    if (sel < 0 || sel > 12)
        sel = 0;
    if (sel >= n)
        sel = (sel - 1) % 4 + 1;
    SendMessageW(h, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void settings_show(int fullscreen, int res, int vsync, int sdl, int sdl_audio, int mute, int fps, int msaa,
                          int renderer, int language)
{
    SendMessageW(ctl(ID_LANGUAGE), CB_SETCURSEL, (WPARAM)language_index(language), 0);
    SendMessageW(ctl(ID_MSAA), CB_SETCURSEL, (WPARAM)(msaa >= 1 && msaa <= 4 ? msaa - 1 : 0), 0);
    SendMessageW(ctl(ID_RENDERER), CB_SETCURSEL, (WPARAM)renderer, 0);
    /* (not CheckRadioButton: the exclusive one's id isn't next to the others) */
    CheckDlgButton(s_wnd, ID_WINDOWED, !fullscreen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_FULLSCREEN, fullscreen == 1 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_FULLSCREEN_EX, fullscreen == 2 ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(ctl(ID_RESOLUTION), CB_SETCURSEL, (WPARAM)res, 0);
    CheckDlgButton(s_wnd, ID_VSYNC, vsync ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(ctl(ID_INPUT), CB_SETCURSEL, (WPARAM)(sdl ? 1 : 0), 0);
    SendMessageW(ctl(ID_AUDIO), CB_SETCURSEL, (WPARAM)(sdl_audio ? 1 : 0), 0);
    CheckDlgButton(s_wnd, ID_MUTE, mute ? BST_CHECKED : BST_UNCHECKED);
    /* (the performance overlay: 0 off, 1 the frame rate, 2 everything - fps_overlay.h perf_overlay) */
    SendMessageW(ctl(ID_SHOWFPS), CB_SETCURSEL, (WPARAM)(fps >= 0 && fps <= 2 ? fps : 1), 0);
}

/* The window size row: only for a window (full screen is the screen's own size - in the game too, whose
   RESOLUTION row says FULL SCREEN then); the line under it says which. */
static void display_hint(void)
{
    const int windowed = IsDlgButtonChecked(s_wnd, ID_WINDOWED) == BST_CHECKED;
    WCHAR t[200];
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    EnableWindow(ctl(ID_RESOLUTION), windowed);
    if (windowed)
        wcscpy_s(t, 200, L"The window's size. Frame rate: at most; normal speed at either.");
    else if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth)
        swprintf_s(t, 200, L"Full screen: your screen's own size (%lu \x00D7 %lu now), not the window size.",
                   dm.dmPelsWidth, dm.dmPelsHeight);
    else
        wcscpy_s(t, 200, L"Full screen: your screen's own size, not the window size.");
    set_text(ID_RES_HINT, t);
}

/* A Steam Deck (Steam sets SteamDeck=1 for what it starts there). */
static int on_steam_deck(void)
{
    WCHAR v[8];
    return GetEnvironmentVariableW(L"SteamDeck", v, 8) == 1 && v[0] == L'1';
}

static void settings_load(void)
{
    WCHAR p[MAX_PATH];
    Lines l;
    int i, fullscreen = 0, vsync = 1, sdl = 0, sdl_audio = 0, mute = 0, fps = 1, perf = -1, w = 1280, h = 720, res = 0, in_section = 0;
    int msaa = 0, aa = 0, emulated = 0, api = 0, language = 1, online = 0, prepare = 1, frame_rate = 60;
    int priority = 0, unlock_all = 1, textures = 0, skip_intros = 0, skip_training = 0, fast_loads = 0, arena_light = 100;
    int match_camera = 0;
    int share = 1, up_kbs = 0, up_peers = 0, mod_mb = 0, mods_mb = 0;  /* (the game's online mod settings) */
    int exclusive = 0, max_scale = 4, scene_height = 0, upscale = 0, upscale_quality = 1;
    double render_scale = 1.0;
    char online_name[64] = "", online_server[128] = "", online_peers[256] = "";
    s_online_token[0] = s_online_xuid[0] = 0;
    settings_path(p);
    if (toml_read(p, &l)) {
        for (i = 0; i < l.n; i++) {
            char key[64], val[256];
            const char *s = l.v[i];
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == '[')
                in_section = 1;
            if (in_section || !toml_kv(l.v[i], key, sizeof key, val, sizeof val))
                continue;
            if (!strcmp(key, "fullscreen")) fullscreen = !strcmp(val, "true");
            else if (!strcmp(key, "fullscreen_exclusive")) exclusive = !strcmp(val, "true");
            else if (!strcmp(key, "native_max_scale")) max_scale = atoi(val);
            else if (!strcmp(key, "native_render_scale")) render_scale = atof(val);
            else if (!strcmp(key, "native_scene_height")) scene_height = atoi(val);
            else if (!strcmp(key, "vsync")) vsync = !strcmp(val, "true");
            else if (!strcmp(key, "audio_mute")) mute = !strcmp(val, "true");
            else if (!strcmp(key, "input_backend")) sdl = !_stricmp(val, "sdl");
            else if (!strcmp(key, "audio_backend")) sdl_audio = !_stricmp(val, "sdl");
            else if (!strcmp(key, "show_fps")) fps = !strcmp(val, "true");
            else if (!strcmp(key, "perf_overlay")) perf = atoi(val);
            else if (!strcmp(key, "native_2x_msaa")) msaa = !strcmp(val, "true");
            else if (!strcmp(key, "native_aa")) aa = atoi(val);
            else if (!strcmp(key, "native_upscale"))
                upscale = !_stricmp(val, "fsr1") ? 1 : !_stricmp(val, "fsr2") ? 2 : !_stricmp(val, "dlss") ? 3 : 0;
            else if (!strcmp(key, "native_upscale_quality")) upscale_quality = atoi(val);
            else if (!strcmp(key, "native_prepare_pipelines")) prepare = !strcmp(val, "true");
            else if (!strcmp(key, "process_priority")) priority = atoi(val) >= 1;
            else if (!strcmp(key, "unlock_everything")) unlock_all = strcmp(val, "false") != 0;
            else if (!strcmp(key, "skip_intros")) skip_intros = !strcmp(val, "true");
            else if (!strcmp(key, "skip_training")) skip_training = !strcmp(val, "true");
            else if (!strcmp(key, "fast_loads")) fast_loads = !strcmp(val, "true");
            else if (!strcmp(key, "arena_brightness")) arena_light = atoi(val);
            else if (!strcmp(key, "match_camera")) match_camera = atoi(val);
            else if (!strcmp(key, "native_texture_quality"))
                textures = !_stricmp(val, "low") ? 2 : !_stricmp(val, "medium") ? 1 : 0;
            else if (!strcmp(key, "native_renderer")) emulated = !_stricmp(val, "off");
            else if (!strcmp(key, "gpu_backend"))
                api = !_stricmp(val, "vulkan") ? RENDERER_VULKAN : !_stricmp(val, "d3d11") ? RENDERER_D3D11 : RENDERER_NATIVE;
            else if (!strcmp(key, "window_width")) w = atoi(val);
            else if (!strcmp(key, "window_height")) h = atoi(val);
            else if (!strcmp(key, "user_language")) language = atoi(val);
            else if (!strcmp(key, "frame_rate")) frame_rate = atoi(val);
            else if (!strcmp(key, "online_enabled")) online = !strcmp(val, "true");
            else if (!strcmp(key, "online_name")) strcpy_s(online_name, sizeof online_name, val);
            else if (!strcmp(key, "online_server")) strcpy_s(online_server, sizeof online_server, val);
            else if (!strcmp(key, "online_token")) strcpy_s(s_online_token, sizeof s_online_token, val);
            else if (!strcmp(key, "online_xuid")) strcpy_s(s_online_xuid, sizeof s_online_xuid, val);
            else if (!strcmp(key, "p2p_peers")) strcpy_s(online_peers, sizeof online_peers, val);
            else if (!strcmp(key, "online_mod_upload")) share = strcmp(val, "false") != 0;
            else if (!strcmp(key, "online_mod_upload_kbs")) up_kbs = atoi(val);
            else if (!strcmp(key, "online_mod_upload_peers")) up_peers = atoi(val);
            else if (!strcmp(key, "online_mod_max_mb")) mod_mb = atoi(val);
            else if (!strcmp(key, "online_mods_max_mb")) mods_mb = atoi(val);
        }
        lines_free(&l);
    } else if (on_steam_deck()) {
        fullscreen = 1;  /* no settings yet: the Deck's screen is the window */
    }
    if (w >= 640 && h >= 360) {  /* (a saved size the list lacks: kept as a choice) */
        int had = N_RES;
        res_add(w, h);
        if (N_RES != had) {
            qsort(k_res, (size_t)N_RES, sizeof k_res[0], res_cmp);
            SendMessageW(ctl(ID_RESOLUTION), CB_RESETCONTENT, 0, 0);
            for (i = 0; i < N_RES; i++)
                SendMessageW(ctl(ID_RESOLUTION), CB_ADDSTRING, 0, (LPARAM)k_res[i].label);
        }
    }
    for (i = 0; i < N_RES; i++)
        if (k_res[i].w == w && k_res[i].h == h)
            res = i;
    /* anti-aliasing: native_aa (1 off, 2-4), else the older on / off (on = 2x) */
    msaa = aa >= 1 && aa <= 4 ? aa : msaa ? 2 : 1;
    if (perf >= 0 && perf <= 2)  /* (perf_overlay wins over the older show_fps) */
        fps = perf;
    settings_show(fullscreen && exclusive ? 2 : fullscreen, res, vsync, sdl, sdl_audio, mute, fps, msaa,
                  api, language);
    SendMessageW(ctl(ID_RENDER_RES), CB_SETCURSEL, (WPARAM)render_index(max_scale, render_scale, scene_height), 0);
    display_hint();
    (void)emulated;  /* (an old native_renderer = "off": saved as "main" next time) */
    online_show(online, online_name, online_server);
    {   /* (the game's online mod settings: mod_transfer.cpp's defaults when missing) */
        WCHAR n[16];
        CheckDlgButton(s_wnd, ID_ON_SHARE, share ? BST_CHECKED : BST_UNCHECKED);
        swprintf_s(n, 16, L"%d", up_kbs > 0 ? up_kbs : 4096), set_text(ID_ON_UP_KBS, n);
        swprintf_s(n, 16, L"%d", up_peers > 0 ? up_peers : 2), set_text(ID_ON_UP_PEERS, n);
        swprintf_s(n, 16, L"%d", mod_mb > 0 ? mod_mb : 300), set_text(ID_ON_MOD_MB, n);
        swprintf_s(n, 16, L"%d", mods_mb > 0 && mods_mb != 1024 ? mods_mb : 4096), set_text(ID_ON_MODS_MB, n);  /* (1024: the old default) */
    }
    {
        WCHAR w[256];
        MultiByteToWideChar(CP_UTF8, 0, online_peers, -1, w, 256);
        set_text(ID_ON_PEERS, w);
    }
    CheckDlgButton(s_wnd, ID_PREPARE, prepare ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_PRIORITY, priority ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_UNLOCK_ALL, unlock_all ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_INTROS, skip_intros ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_TRAINING, skip_training ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_FAST_LOADS, fast_loads ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(ctl(ID_TEXTURES), CB_SETCURSEL, (WPARAM)textures, 0);
    /* (Off, then FSR 1 / FSR 2 / DLSS ultra quality .. performance: native_upscale_quality 0-3) */
    upscale_fill(upscale ? 1 + 4 * (upscale - 1) + (upscale_quality >= 0 && upscale_quality <= 3 ? upscale_quality : 1)
                         : 0);
    if (arena_light < 50 || arena_light > 200)
        arena_light = 100;
    SendMessageW(ctl(ID_ARENA_LIGHT), CB_SETCURSEL, (WPARAM)((arena_light - 50 + 5) / 10), 0);
    SendMessageW(ctl(ID_MATCH_CAMERA), CB_SETCURSEL, (WPARAM)(match_camera >= 0 && match_camera <= 5 ? match_camera : 0), 0);
    SendMessageW(ctl(ID_FRAMERATE), CB_SETCURSEL, (WPARAM)frame_rate_index(frame_rate), 0);
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, L"");
    set_text(ID_ON_STATUS, L"");
}

static int settings_save(void)
{
    enum { NK = 44 };
    static const char *keys[NK] = { "gpu_plugin", "input_backend", "resolution", "resolution_scale", "window_width",
                                    "window_height", "fullscreen", "vsync", "audio_mute", "audio_backend", "show_fps",
                                    "native_2x_msaa", "native_renderer", "gpu_backend", "user_language",
                                    "online_enabled", "online_name", "online_server", "native_prepare_pipelines",
                                    "online_token", "online_xuid", "frame_rate", "p2p_peers", "native_aa",
                                    "process_priority", "unlock_everything", "native_texture_quality",
                                    "skip_intros", "skip_training", "fullscreen_exclusive", "native_max_scale",
                                    "native_render_scale", "native_scene_height", "arena_brightness", "perf_overlay",
                                    "native_upscale", "native_upscale_quality", "match_camera",
                                    "online_mod_upload", "online_mod_upload_kbs", "online_mod_upload_peers",
                                    "online_mod_max_mb", "online_mods_max_mb", "fast_loads" };
    const int renderer = (int)SendMessageW(ctl(ID_RENDERER), CB_GETCURSEL, 0, 0);
    char vals[NK][160];
    int done[NK] = { 0 };
    WCHAR p[MAX_PATH], tmp[MAX_PATH];
    Lines l;
    FILE *f;
    int i, k, sel = (int)SendMessageW(ctl(ID_RESOLUTION), CB_GETCURSEL, 0, 0), insert_at, in_section = 0;
    int sdl = (int)SendMessageW(ctl(ID_INPUT), CB_GETCURSEL, 0, 0) == 1;

    if (!s_game_dir[0] || !dir_exists(s_game_dir)) {
        set_text(ID_SETTINGS_STATUS, L"The game folder does not exist; choose it on the Play tab first.");
        set_text(ID_ON_STATUS, L"The game folder does not exist; choose it on the Play tab first.");
        return 0;
    }
    if (sel < 0 || sel >= N_RES)
        sel = 0;
    strcpy_s(vals[0], 64, "\"xenos\"");
    strcpy_s(vals[1], 64, sdl ? "\"sdl\"" : "\"xinput\"");
    strcpy_s(vals[2], 64, "\"720p\"");
    sprintf_s(vals[3], 64, "%d", k_res[sel].scale);
    sprintf_s(vals[4], 64, "%d", k_res[sel].w);
    sprintf_s(vals[5], 64, "%d", k_res[sel].h);
    strcpy_s(vals[6], 64, IsDlgButtonChecked(s_wnd, ID_WINDOWED) == BST_CHECKED ? "false" : "true");
    strcpy_s(vals[29], 64, IsDlgButtonChecked(s_wnd, ID_FULLSCREEN_EX) == BST_CHECKED ? "true" : "false");
    {   /* (as the game writes them: graphics_page.cpp kRenderScale) */
        int ri = (int)SendMessageW(ctl(ID_RENDER_RES), CB_GETCURSEL, 0, 0);
        if (ri < 0 || ri >= N_RENDER) ri = 0;
        sprintf_s(vals[30], 64, "%d", k_render[ri].max_scale);
        sprintf_s(vals[31], 64, "%.4f", k_render[ri].res);
        sprintf_s(vals[32], 64, "%d", k_render[ri].height);
    }
    strcpy_s(vals[7], 64, IsDlgButtonChecked(s_wnd, ID_VSYNC) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[8], 64, IsDlgButtonChecked(s_wnd, ID_MUTE) == BST_CHECKED ? "true" : "false");
    {
        const int pi = (int)SendMessageW(ctl(ID_SHOWFPS), CB_GETCURSEL, 0, 0);
        strcpy_s(vals[10], 64, pi > 0 ? "true" : "false");
        sprintf_s(vals[34], 64, "%d", pi >= 0 && pi <= 2 ? pi : 1);
    }
    strcpy_s(vals[9], 64, SendMessageW(ctl(ID_AUDIO), CB_GETCURSEL, 0, 0) == 1 ? "\"sdl\"" : "\"xaudio2\"");
    {
        const int ai = (int)SendMessageW(ctl(ID_MSAA), CB_GETCURSEL, 0, 0);
        const int level = ai >= 0 && ai < 4 ? ai + 1 : 1;
        strcpy_s(vals[11], 64, level >= 2 ? "true" : "false");
        sprintf_s(vals[23], 64, "%d", level);
    }
    strcpy_s(vals[12], 64, "\"main\"");
    /* Vulkan / D3D11: the emulator's presentation and the native renderer on it. */
    strcpy_s(vals[13], 64, renderer == RENDERER_VULKAN ? "\"vulkan\"" : renderer == RENDERER_D3D11 ? "\"d3d11\"" : "\"any\"");
    {
        int li = (int)SendMessageW(ctl(ID_LANGUAGE), CB_GETCURSEL, 0, 0);
        sprintf_s(vals[14], 64, "%d", k_languages[li >= 0 && li < N_LANGUAGES ? li : 0].id);
    }
    strcpy_s(vals[15], 64, IsDlgButtonChecked(s_wnd, ID_ON_ENABLE) == BST_CHECKED ? "true" : "false");
    toml_quote_ctl(ID_ON_NAME, 15, vals[16], sizeof vals[16]);
    {
        WCHAR server[160];
        char u[400];
        online_server_text(server, 160);
        WideCharToMultiByte(CP_UTF8, 0, server, -1, u, sizeof u, NULL, NULL);
        sprintf_s(vals[17], sizeof vals[17], "\"%.150s\"", u);
    }
    strcpy_s(vals[18], 64, IsDlgButtonChecked(s_wnd, ID_PREPARE) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[24], 64, IsDlgButtonChecked(s_wnd, ID_PRIORITY) == BST_CHECKED ? "1" : "0");
    strcpy_s(vals[25], 64, IsDlgButtonChecked(s_wnd, ID_UNLOCK_ALL) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[27], 64, IsDlgButtonChecked(s_wnd, ID_SKIP_INTROS) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[28], 64, IsDlgButtonChecked(s_wnd, ID_SKIP_TRAINING) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[43], 64, IsDlgButtonChecked(s_wnd, ID_FAST_LOADS) == BST_CHECKED ? "true" : "false");
    {
        static const char *k_textures[3] = { "\"high\"", "\"medium\"", "\"low\"" };
        const int ti = (int)SendMessageW(ctl(ID_TEXTURES), CB_GETCURSEL, 0, 0);
        strcpy_s(vals[26], 64, k_textures[ti >= 0 && ti < 3 ? ti : 0]);
    }
    {
        const int li = (int)SendMessageW(ctl(ID_ARENA_LIGHT), CB_GETCURSEL, 0, 0);
        sprintf_s(vals[33], 64, "%d", li >= 0 && li <= 15 ? 50 + 10 * li : 100);
    }
    {   /* (as the game writes them: graphics_page.cpp kUpscaling) */
        const int ui = (int)SendMessageW(ctl(ID_UPSCALE), CB_GETCURSEL, 0, 0);
        strcpy_s(vals[35], 64, ui >= 1 && ui <= 4 ? "\"fsr1\"" : ui >= 5 && ui <= 8 ? "\"fsr2\""
                               : ui >= 9 && ui <= 12 ? "\"dlss\"" : "\"off\"");
        sprintf_s(vals[36], 64, "%d", ui >= 1 && ui <= 12 ? (ui - 1) % 4 : 1);
    }
    {   /* (the game's GRAPHICS -> MATCH CAMERA: free_cam.h, 0 NORMAL .. 5 RAMP) */
        const int ci = (int)SendMessageW(ctl(ID_MATCH_CAMERA), CB_GETCURSEL, 0, 0);
        sprintf_s(vals[37], 64, "%d", ci >= 0 && ci <= 5 ? ci : 0);
    }
    sprintf_s(vals[19], 160, "\"%s\"", s_online_token);
    sprintf_s(vals[20], 160, "\"%s\"", s_online_xuid);
    {
        const int fi = (int)SendMessageW(ctl(ID_FRAMERATE), CB_GETCURSEL, 0, 0);
        sprintf_s(vals[21], 64, "%d", k_frame_rates[fi >= 0 && fi < N_FRAME_RATES ? fi : 1]);
    }
    toml_quote_ctl(ID_ON_PEERS, 140, vals[22], sizeof vals[22]);
    {   /* (the game's online mod settings, kept in its ranges) */
        static const struct { int id, lo, hi, def; } k_mods[4] = {
            { ID_ON_UP_KBS, 64, 65536, 4096 }, { ID_ON_UP_PEERS, 1, 8, 2 },
            { ID_ON_MOD_MB, 1, 4096, 300 }, { ID_ON_MODS_MB, 1, 16384, 4096 } };
        strcpy_s(vals[38], 64, IsDlgButtonChecked(s_wnd, ID_ON_SHARE) == BST_CHECKED ? "true" : "false");
        for (k = 0; k < 4; k++) {
            WCHAR n[16];
            int v;
            GetWindowTextW(ctl(k_mods[k].id), n, 16);
            v = n[0] ? _wtoi(n) : k_mods[k].def;
            v = v < k_mods[k].lo ? k_mods[k].lo : v > k_mods[k].hi ? k_mods[k].hi : v;
            sprintf_s(vals[39 + k], 64, "%d", v);
            swprintf_s(n, 16, L"%d", v);  /* (the field shows what's saved) */
            set_text(k_mods[k].id, n);
        }
    }

    settings_path(p);
    if (!toml_read(p, &l))
        lines_insert(&l, 0, "# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)");
    /* Known top-level keys are rewritten where they are (a repeat is dropped);
     * the rest is kept. Missing keys go before the first [section]. */
    insert_at = -1;
    for (i = 0; i < l.n; i++) {
        char key[64], val[256], line[256];
        const char *s = l.v[i];
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '[' && !in_section) {
            in_section = 1;
            insert_at = i;
        }
        if (in_section || !toml_kv(l.v[i], key, sizeof key, val, sizeof val))
            continue;
        for (k = 0; k < NK; k++)
            if (!strcmp(key, keys[k]))
                break;
        if (k == NK)
            continue;
        free(l.v[i]);
        l.v[i] = NULL;
        if (!done[k]) {
            sprintf_s(line, 256, "%s = %s", keys[k], vals[k]);
            l.v[i] = _strdup(line);
            done[k] = 1;
        }
    }
    if (insert_at < 0)
        insert_at = l.n;
    while (insert_at > 0 && (!l.v[insert_at - 1] || !l.v[insert_at - 1][0]))
        insert_at--;                                 /* before the blank lines */
    for (k = 0; k < NK; k++) {
        char line[256];
        if (done[k])
            continue;
        sprintf_s(line, 256, "%s = %s", keys[k], vals[k]);
        lines_insert(&l, insert_at++, line);
    }
    if (in_section && insert_at < l.n && l.v[insert_at] && l.v[insert_at][0])
        lines_insert(&l, insert_at, "");

    swprintf_s(tmp, MAX_PATH, L"%s.tmp", p);
    if (_wfopen_s(&f, tmp, L"wb") || !f) {
        lines_free(&l);
        set_text(ID_SETTINGS_STATUS, L"The settings could not be saved (is the game folder read-only?).");
        return 0;
    }
    for (i = 0; i < l.n; i++)
        if (l.v[i])
            fprintf(f, "%s\n", l.v[i]);
    fclose(f);
    lines_free(&l);
    if (!MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        set_text(ID_SETTINGS_STATUS, L"The settings could not be saved.");
        return 0;
    }
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, game_running() ? L"Settings saved. They apply the next time the game starts."
                                                : L"Settings saved to " GAME_TOML L".");
    set_text(ID_ON_STATUS, game_running() ? L"Saved. It applies the next time the game starts." : L"Saved.");
    return 1;
}

static void settings_defaults(void)
{
    settings_show(0, 0, 1, 0, 0, 0, 1, 1, 0, 1);
    SendMessageW(ctl(ID_RENDER_RES), CB_SETCURSEL, 0, 0);
    display_hint();
    CheckDlgButton(s_wnd, ID_PREPARE, BST_CHECKED);
    CheckDlgButton(s_wnd, ID_PRIORITY, BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_UNLOCK_ALL, BST_CHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_INTROS, BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_TRAINING, BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_FAST_LOADS, BST_UNCHECKED);
    SendMessageW(ctl(ID_TEXTURES), CB_SETCURSEL, 0, 0);
    SendMessageW(ctl(ID_UPSCALE), CB_SETCURSEL, 0, 0);
    SendMessageW(ctl(ID_ARENA_LIGHT), CB_SETCURSEL, 5, 0);
    SendMessageW(ctl(ID_MATCH_CAMERA), CB_SETCURSEL, 0, 0);
    SendMessageW(ctl(ID_FRAMERATE), CB_SETCURSEL, 1, 0);
    s_settings_dirty = 1;
    set_text(ID_SETTINGS_STATUS, L"Defaults restored. Press Save or Play to keep them.");
}

/* ── play tab ──────────────────────────────────────────────────────────── */

/* Time played (the game's <game>\\playtime.txt, src/playtime.cpp). */
static void playtime_show(void)
{
    WCHAR p[MAX_PATH], t[128];
    FILE *f = NULL;
    char line[128];
    unsigned long long sec = 0, ses = 0;
    swprintf_s(p, MAX_PATH, L"%s\\playtime.txt", s_game_dir);
    if (!_wfopen_s(&f, p, L"r") && f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "seconds=", 8)) sec = _strtoui64(line + 8, NULL, 10);
            if (!strncmp(line, "sessions=", 9)) ses = _strtoui64(line + 9, NULL, 10);
        }
        fclose(f);
    }
    if (!sec)
        wcscpy_s(t, 128, L"Time played: none yet");
    else if (sec < 3600)
        swprintf_s(t, 128, L"Time played: %llu min  \x00B7  %llu session%s", sec / 60, ses, ses == 1 ? L"" : L"s");
    else
        swprintf_s(t, 128, L"Time played: %llu h %llu min  \x00B7  %llu session%s", sec / 3600, sec / 60 % 60, ses,
                   ses == 1 ? L"" : L"s");
    set_text(ID_PLAYTIME, t);
}

/* The game's size (its folder; its Mods part) and the program files against the release's list -
   in the background (a big folder takes a moment), WM_APP_SIZE when done. */
static void add_size(const WCHAR *dir, int depth, uint64_t *total, uint64_t *mods)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 24 || swprintf_s(pat, MAX_PATH, L"%s\\*", dir) < 0)
        return;
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            uint64_t t = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)   /* (links: not the game's own) */
                continue;
            if (swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0)
                continue;
            add_size(sub, depth + 1, &t, NULL);
            *total += t;
            if (mods && depth == 0 && !_wcsicmp(fd.cFileName, L"Mods"))
                *mods += t;
        } else {
            *total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI size_thread(LPVOID p)
{
    WCHAR *dir = (WCHAR *)p;
    ReleaseCheck rc;
    uint64_t total = 0, mods = 0;
    add_size(dir, 0, &total, &mods);
    s_size_game = total;
    s_size_mods = mods;
    ZeroMemory(&rc, sizeof rc);
    s_files_listed = release_check(dir, NULL, 0, &rc, NULL, NULL) ? rc.files : 0;
    s_files_bad = rc.bad;
    s_size_known = 1;
    free(dir);
    InterlockedExchange(&s_size_busy, 0);
    PostMessageW(s_wnd, WM_APP_SIZE, 0, 0);
    return 0;
}

static void play_size_start(void)
{
    WCHAR *dir;
    HANDLE t;
    if (!s_game_dir[0] || !dir_exists(s_game_dir) || (s_size_known && same_dir(s_size_dir, s_game_dir)))
        return;
    if (InterlockedCompareExchange(&s_size_busy, 1, 0))
        return;
    wcscpy_s(s_size_dir, MAX_PATH, s_game_dir);
    s_size_known = 0;
    dir = _wcsdup(s_game_dir);
    t = dir ? CreateThread(NULL, 0, size_thread, dir, 0, NULL) : NULL;
    if (t)
        CloseHandle(t);
    else {
        free(dir);
        InterlockedExchange(&s_size_busy, 0);
    }
}

static void refresh_play(void)
{
    const WCHAR *t;
    int ready = 0;
    set_text(ID_GAMEDIR, s_game_dir);
    if (!dir_exists(s_game_dir))
        t = L"The game folder does not exist. Choose another one, or install the game on the Install tab.";
    else if (!is_game_folder(s_game_dir))
        t = L"Game files not installed \x2014 use the Install tab.";
    else if (install_missing(s_game_dir))
        t = L"Your install isn't complete - install again";  /* (the status line's room; the start opens the Install tab) */
    else if (!has_program(s_game_dir))
        t = GAME_EXE L" is missing from the game folder \x2014 copy the port's program files into it, "
            L"or install again from the Install tab.";
    else if (game_running())
        t = L"Game running\x2026";
    else {
        t = L"Ready.";
        ready = 1;
    }
    s_play_ready = ready;
    set_text(ID_PLAY_STATUS, ready && s_bundled_busy ? s_bundled_text : ready ? L"All systems ready. Have fun!" : t);
    EnableWindow(ctl(ID_PLAY), ready && !s_busy);  /* (during the bundled install a click waits for it: play_with) */
    EnableWindow(ctl(ID_PLAY_NOMODS), ready && !s_busy);
    playtime_show();
    play_counts();
    play_size_start();
    play_notice();
    if (s_wnd)
        InvalidateRect(s_wnd, NULL, FALSE);
}

static DWORD WINAPI game_watch(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    DWORD code = 0;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    PostMessageW(s_wnd, WM_APP_GAMEEND, code, 0);
    return 0;
}

/* The game folder's native_shaders\shaders.dxil.pak: without it the game only shows "some of the game's files
 * are missing". Copied again from the launcher's folder when it has one; otherwise (or when that fails - a
 * 2.1.3 player had the game in Pictures) the player is told where and why. 1: start the game. */
static int shaders_ready(void)
{
    WCHAR dir[MAX_PATH], pack[MAX_PATH], src[MAX_PATH], m[1400], failed[80] = L"";
    const WCHAR *where = L"";
    WCHAR lower[MAX_PATH];
    if (!join(dir, s_game_dir, L"native_shaders") || !join(pack, dir, L"shaders.dxil.pak"))
        return 1;
    if (file_exists(pack))
        return 1;
    if (!same_dir(s_game_dir, s_launcher_dir) && join(src, s_launcher_dir, L"native_shaders\\shaders.dxil.pak") &&
        file_exists(src)) {
        CreateDirectoryW(dir, NULL);
        if (CopyFileW(src, pack, FALSE))
            return 1;
        swprintf_s(failed, 80, L" and copying it failed (Windows error %lu)", GetLastError());
    }
    wcscpy_s(lower, MAX_PATH, s_game_dir);
    _wcslwr_s(lower, MAX_PATH);
    if (wcsstr(lower, L"\\onedrive"))
        where = L"\n\nThe game is in a OneDrive folder: OneDrive can keep files online-only or remove them.";
    else if (wcsstr(lower, L"\\pictures") || wcsstr(lower, L"\\documents") || wcsstr(lower, L"\\desktop"))
        where = L"\n\nThe game is in your Pictures, Documents or Desktop folder: Windows Controlled folder access "
                L"(Windows Security, Ransomware protection) blocks programs from writing there.";
    swprintf_s(m, 1400,
               L"The game's shaders are missing: %s has no shaders.dxil.pak%s.%s\n\nExtract the release zip "
               L"again into a plain folder such as C:\\Games\\SvR2011, check that native_shaders\\shaders.dxil.pak is "
               L"there (an antivirus may remove it), and start the launcher from that folder.\n\nStart the game "
               L"anyway?",
               dir, failed, where);
    update_log(UPLOG_DIR, "launcher", L"Play: %s", m);
    return MessageBoxW(s_wnd, m, WINDOW_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

static void play_with(int no_mods);
static void play(void) { play_with(0); }
static int saves_backup_to(const WCHAR *suffix, WCHAR *out);
static int saves_dir(WCHAR *out);
/* no_mods: the game without any mod (Start Without Mods): SVR2011_NO_MODS=1 in its environment. */
static void play_with(int no_mods)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (game_running()) {
        set_text(ID_PLAY_STATUS, L"The game is already running.");
        return;
    }
    if (s_up_finishing) {  /* (finishing the update: its mods follow - the game starts once they're in) */
        s_play_pending = no_mods ? 2 : 1;
        set_text(ID_PLAY_STATUS, L"The game starts after the update\x2026");
        return;
    }
    if (s_bundled_busy) {  /* (the update's mods are being installed: the game starts once they're in) */
        WCHAR count[40] = L"";
        const WCHAR *n = wcschr(s_bundled_text, L'(');  /* (the "n of N" kept) */
        if (n) wcsncpy_s(count, 40, n, _TRUNCATE);
        s_play_pending = no_mods ? 2 : 1;
        swprintf_s(s_bundled_text, 160, L"The game starts after the mods\x2026%s%s", count[0] ? L" " : L"", count);
        set_text(ID_PLAY_STATUS, s_bundled_text);
        return;
    }
    if (s_busy) {  /* (an install or repair is running: Play is disabled then - a click that got through says why) */
        set_text(ID_PLAY_STATUS, L"An install is running - Play when it's done.");
        return;
    }
    if (is_game_folder(s_game_dir) && install_missing(s_game_dir)) {
        refresh_play();
        show_tab(TAB_INSTALL);
        return;
    }
    if (!is_game_folder(s_game_dir) || !has_program(s_game_dir)) {
        refresh_play();
        MessageBoxW(s_wnd, is_game_folder(s_game_dir)
                               ? GAME_EXE L" is missing from the game folder."
                               : L"The game folder has no default.xex. Install the game from your disc image on the "
                                 L"Install tab first.",
                    WINDOW_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    if (s_settings_dirty)
        settings_save();
    {  /* two attire mods on one attire: the newer moves to a free one (mods_tab.c) */
        static WCHAR clash[4096];
        if (mods_attire_guard(s_game_dir, NULL, clash, 4096))
            MessageBoxW(s_wnd, clash, WINDOW_TITLE L" - attire mods", MB_OK | MB_ICONINFORMATION);
    }
    {  /* the VC++ runtime the game needs: Windows' own (or a copy the player put next to the game) */
        WCHAR ver[40], q[700], own[MAX_PATH];
        int st;
        setup_vcrt_cleanup(s_game_dir, NULL, 0);
        join(own, s_game_dir, L"msvcp140.dll");
        st = file_exists(own) ? SETUP_VCRT_OK : setup_vcrt_system(ver, 40);
        if (st != SETUP_VCRT_OK) {
            int a;
            swprintf_s(q, 700, L"The game needs Microsoft's Visual C++ runtime (x64), version 14.%d or newer. "
                               L"This PC has %s%s.\n\nOpen Microsoft's download page (vc_redist.x64.exe) now? Install "
                               L"it, then press Play again.\n\nYes: open the page.  No: start the game anyway.",
                       SETUP_VCRT_MIN_MINOR, st == SETUP_VCRT_OLD ? L"an older one, " : L"none", ver);
            a = MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNOCANCEL | MB_ICONWARNING);
            if (a == IDYES) ShellExecuteW(s_wnd, L"open", SETUP_VCRT_URL, NULL, NULL, SW_SHOWNORMAL);
            if (a != IDNO) return;
        }
    }
    if (!shaders_ready())
        return;
    join(exe, s_game_dir, GAME_EXE);
    swprintf_s(cmd, MAX_PATH + 4, L"\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (no_mods) {   /* (a backup of the saves first, each time: the way back if that run changes them) */
        WCHAR saves[MAX_PATH], before[MAX_PATH];
        if (saves_dir(saves) && dir_exists(saves) && !saves_backup_to(L" (before Start Without Mods)", before) &&
            MessageBoxW(s_wnd, L"The saves could not be backed up first. Start without mods anyway?", WINDOW_TITLE,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    SetEnvironmentVariableW(L"SVR2011_NO_MODS", no_mods ? L"1" : NULL);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_game_dir, &si, &pi)) {
        WCHAR m[MAX_PATH + 64];
        SetEnvironmentVariableW(L"SVR2011_NO_MODS", NULL);
        swprintf_s(m, MAX_PATH + 64, L"Windows could not start the game (error %lu).", GetLastError());
        set_text(ID_PLAY_STATUS, m);
        return;
    }
    SetEnvironmentVariableW(L"SVR2011_NO_MODS", NULL);
    CloseHandle(pi.hThread);
    if (s_game_proc)
        CloseHandle(s_game_proc);
    s_game_proc = pi.hProcess;
    save_launcher_ini();
    if (IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED) {
        DestroyWindow(s_wnd);
        return;
    }
    {
        HANDLE dup;
        if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup,
                            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
            CloseHandle(CreateThread(NULL, 0, game_watch, dup, 0, NULL));
    }
    set_text(ID_PLAY_STATUS, L"Game running\x2026");
    EnableWindow(ctl(ID_PLAY), FALSE);
}

/* ── folder / file pickers ─────────────────────────────────────────────── */

static int pick_folder(const WCHAR *title, WCHAR *out)
{
    IFileOpenDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, title);
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

static int pick_image(WCHAR *out)
{
    OPENFILENAMEW of;
    WCHAR buf[MAX_PATH] = L"";
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Xbox 360 disc images (*.iso, *.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose your " GAME_TITLE L" disc image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of))
        return 0;
    wcscpy_s(out, MAX_PATH, buf);
    return 1;
}

/* ── window ────────────────────────────────────────────────────────────── */

#define HEADER_H 64      /* dark title band */
#define DY       64      /* page content sits this much lower than laid out */
#define CLIENT_W 780
#define PAD      80      /* the other pages: laid out 620 wide, centred */
#define CLIENT_H 600     /* below the band */
#define SB_W     196     /* the sidebar (the pages sit right of it) */

/* The look: dark - a near-black sidebar (the tabs), dark grey pages with
   slightly lighter cards (the group boxes, drawn by the window), red for the
   main action. */
#define C_SIDEBAR       RGB(14, 15, 19)
#define C_SIDEBAR_HOVER RGB(28, 30, 36)
#define C_SIDEBAR_SEL   RGB(56, 20, 27)
#define C_ACCENT        RGB(214, 22, 44)
#define C_ACCENT_HOT    RGB(234, 42, 62)
#define C_ACCENT_DOWN   RGB(170, 14, 34)
#define C_PAGE          RGB(20, 22, 27)
#define C_CARD          RGB(27, 30, 36)
#define C_CARD_LINE     RGB(46, 50, 58)
#define C_FIELD         RGB(16, 18, 22)
#define C_TEXT          RGB(236, 237, 241)
#define C_MUTED         RGB(150, 153, 163)
#define C_GOOD          RGB(52, 199, 89)
#define C_WARN          RGB(240, 160, 40)
static HBRUSH s_page_brush, s_card_brush, s_field_brush;

/* The sidebar: the tabs in this order, with their icons (Segoe icon font). */
static const struct { int tab; WCHAR icon; const WCHAR *name; } k_nav[] = {
    { TAB_PLAY, 0xE768, L"Play" },          { TAB_SETTINGS, 0xE713, L"Settings" },
    { TAB_SAVES, 0xE74E, L"Saves" },        { TAB_ROSTER, 0xE716, L"Roster" },
    { TAB_MODS, 0xEA86, L"Mods" },          { TAB_SETUP, 0xE90F, L"Setup" },
    /* (below the line) */
    { TAB_ONLINE, 0xE774, L"Online" },      { TAB_MUSIC, 0xE8D6, L"Music" },
    { TAB_MOVIES, 0xE714, L"Movies" },      { TAB_PAINT, 0xE790, L"Paint Tool" },
    { TAB_REPORTS, 0xED15, L"Reports" },    { TAB_SUGGESTIONS, 0xEA80, L"Suggestions" },
};

/* Mods, in three tabs: the installed mods (TAB_MODS), Discovery, Texture packs - a tab bar on top of each
   (paint_mod_tabs). The tab of a page, or -1. */
static const int k_mod_tabs[] = { TAB_MODS, TAB_DISCOVER, TAB_TEXPACKS };
#define N_MOD_TABS 3
static int mods_tab_of(int tab)
{
    int i;
    for (i = 0; i < N_MOD_TABS; i++)
        if (k_mod_tabs[i] == tab)
            return i;
    return -1;
}

/* Setup, step by step: this PC (TAB_SETUP), the game (TAB_INSTALL), DLC, the Mod Maker's tools, Android - one
   sidebar entry; a step bar on top of each (paint_steps). The step of a page, or -1. */
static const int k_steps[] = { TAB_SETUP, TAB_INSTALL, TAB_DLC, TAB_TOOLS, TAB_ANDROID };
#define N_STEPS 5
static int setup_step(int tab)
{
    int i;
    for (i = 0; i < N_STEPS; i++)
        if (k_steps[i] == tab)
            return i;
    return -1;
}
/* The sidebar entry a page belongs to (Setup's steps: Setup). */
static int nav_tab(int tab) { return setup_step(tab) >= 0 ? TAB_SETUP : mods_tab_of(tab) >= 0 ? TAB_MODS : tab; }
#define NAV_N    ((int)(sizeof k_nav / sizeof k_nav[0]))
#define NAV_MAIN 6       /* the main pages (big rows), then the line, the others (small rows) */
#define NAV_TOP  86
#define NAV_H    42
#define NAV_H2   31
#define NAV_LINE 15

/* A sidebar row's place. */
static RECT nav_rect(int k)
{
    RECT r;
    r.left = 0;
    r.right = SB_W;
    r.top = NAV_TOP + (k < NAV_MAIN ? k * NAV_H : NAV_MAIN * NAV_H + NAV_LINE + (k - NAV_MAIN) * NAV_H2);
    r.bottom = r.top + (k < NAV_MAIN ? NAV_H : NAV_H2);
    return r;
}

/* A group box: a card the window draws on the page. */
typedef struct { int tab, x, y, w, h, id; WCHAR title[64]; } Card;
static Card s_cards[96];
static int  s_ncards;
/* The Online tab's folds: their cards and rows in the Friends card's place while open (1 Mod sharing, 2 Connection). */
static int s_mods_fold;
static int card_hidden(const Card *cd)
{
    return (cd->id == ID_FR_GROUP && s_mods_fold) || (cd->id == ID_ON_MODS_GROUP && s_mods_fold != 1) ||
           (cd->id == ID_ON_NAT_GROUP && s_mods_fold != 2);
}
/* Push buttons, drawn by the window (rounded; red for the main actions). */
typedef struct { HWND h; int primary; } Btn;
static Btn  s_btns[256];
static int  s_nbtns;
static HWND s_hover_btn;

static int is_primary_id(int id)
{
    return id == ID_PLAY || id == ID_INSTALL || id == ID_SAVE || id == ID_DLC_INSTALL || id == ID_ON_SIGNIN ||
           id == ID_ON_STEP ||
           id == ID_MV_CREATE || id == ID_APK_CREATE || id == ID_ADB_INSTALL;
}

static LRESULT CALLBACK btn_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (m == WM_MOUSEMOVE && s_hover_btn != h) {
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        HWND old = s_hover_btn;
        s_hover_btn = h;
        TrackMouseEvent(&t);
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_MOUSELEAVE && s_hover_btn == h) {
        s_hover_btn = NULL;
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_SETCURSOR) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    } else if (m == WM_LBUTTONDBLCLK) {
        m = WM_LBUTTONDOWN;  /* (owner-drawn buttons: a quick second click is a click too) */
    }
    return DefSubclassProc(h, m, wp, lp);
}

/* The icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets. */
static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(int *)lp = 1;
    return 0;
}

static const WCHAR *icon_face(void)
{
    static const WCHAR *face;
    if (!face) {
        LOGFONTW lf;
        int found = 0;
        HDC dc = GetDC(NULL);
        ZeroMemory(&lf, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe Fluent Icons");
        EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
        ReleaseDC(NULL, dc);
        face = found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    return face;
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    if (s_font) DeleteObject(s_font);
    if (s_big) DeleteObject(s_big);
    if (s_title) DeleteObject(s_title);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)s_dpi);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(15, s_dpi, 72);
    s_big = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(17, s_dpi, 72);
    s_title = CreateFontIndirectW(&ncm.lfMessageFont);
    /* (the look) */
    if (s_icons) DeleteObject(s_icons);
    if (s_nav) DeleteObject(s_nav);
    if (s_card) DeleteObject(s_card);
    if (s_huge) DeleteObject(s_huge);
    s_icons = CreateFontW(-MulDiv(16, s_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, icon_face());
    if (s_mark_font) DeleteObject(s_mark_font);
    s_mark_font = CreateFontW(-MulDiv(28, s_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, icon_face());
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(10, s_dpi, 72);
    s_nav = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -MulDiv(10, s_dpi, 72);
    s_card = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(20, s_dpi, 72);
    s_huge = CreateFontIndirectW(&ncm.lfMessageFont);
}

/* A list's column headers: light text (the dark header theme leaves it dark). */
static LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (m == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(h) && ((NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
        if (cd->dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            SetTextColor(cd->hdc, C_TEXT);
            return CDRF_DODEFAULT;
        }
    }
    return DefSubclassProc(h, m, wp, lp);
}

static HWND add(int tab, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c;
    const int button = !_wcsicmp(cls, L"Button");
    const DWORD kind = style & BS_TYPEMASK;
    const int push = button && (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON);
    if (tab >= 0 && tab != TAB_PLAY)
        x += PAD;
    if (tab == TAB_SETUP || tab == TAB_TEXPACKS || tab == TAB_TOOLS)
        y += 44;   /* (below the step bar; the other steps' pages start lower already) */
    /* A group box becomes a card the window draws (the control is never shown). */
    if (button && kind == BS_GROUPBOX && tab >= 0 && s_ncards < (int)(sizeof s_cards / sizeof s_cards[0])) {
        Card *cd = &s_cards[s_ncards++];
        cd->tab = tab; cd->x = x; cd->y = y; cd->w = w; cd->h = h; cd->id = id;
        wcsncpy_s(cd->title, 64, text ? text : L"", _TRUNCATE);
        return CreateWindowExW(0, cls, text, WS_CHILD | style, S(x + SB_W), S(y + DY), S(w), S(h), s_wnd,
                               (HMENU)(INT_PTR)id, s_inst, NULL);
    }
    if (push)
        style = (style & ~(DWORD)BS_TYPEMASK) | BS_OWNERDRAW;
    c = CreateWindowExW(0, cls, text, WS_CHILD | style, S(x + SB_W), S(y + DY), S(w), S(h), s_wnd,
                        (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    /* Dark: check boxes and radio buttons unthemed (they take the text colour then: WM_CTLCOLORSTATIC),
       fields and lists in Windows' dark control themes, lists' colours set. */
    if (button && !push)
        SetWindowTheme(c, L"", L"");
    else if (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"ComboBox"))
        SetWindowTheme(c, L"DarkMode_CFD", NULL);
    else if (!_wcsicmp(cls, WC_LISTVIEWW) || !_wcsicmp(cls, L"ListBox") || !_wcsicmp(cls, WC_TREEVIEWW))
        SetWindowTheme(c, L"DarkMode_Explorer", NULL);
    if (!_wcsicmp(cls, WC_LISTVIEWW)) {
        HWND hd = ListView_GetHeader(c);
        ListView_SetBkColor(c, C_FIELD);
        ListView_SetTextBkColor(c, C_FIELD);
        ListView_SetTextColor(c, C_TEXT);
        if (hd) SetWindowTheme(hd, L"DarkMode_ItemsView", NULL);
        SetWindowSubclass(c, list_proc, 2, 0);
    }
    if (!_wcsicmp(cls, PROGRESS_CLASSW)) {   /* (unthemed: its colours) */
        SetWindowTheme(c, L"", L"");
        SendMessageW(c, PBM_SETBKCOLOR, 0, (LPARAM)C_FIELD);
        SendMessageW(c, PBM_SETBARCOLOR, 0, (LPARAM)C_ACCENT);
    }
    if (!_wcsicmp(cls, WC_TREEVIEWW)) {
        TreeView_SetBkColor(c, C_FIELD);
        TreeView_SetTextColor(c, C_TEXT);
    }
    if (push && s_nbtns < (int)(sizeof s_btns / sizeof s_btns[0])) {
        s_btns[s_nbtns].h = c;
        s_btns[s_nbtns].primary = kind == BS_DEFPUSHBUTTON || is_primary_id(id);
        s_nbtns++;
        SetWindowSubclass(c, btn_proc, 1, 0);
    }
    if (tab >= 0 && s_nctl[tab] < TAB_CTL_MAX)
        s_ctl[tab][s_nctl[tab]++] = c;
    if (s_nplaced < (int)(sizeof s_placed / sizeof s_placed[0])) {
        Placed *pl = &s_placed[s_nplaced++];
        pl->h = c; pl->x = x; pl->y = y; pl->w = w; pl->hh = h; pl->big = 0;
    }
    return c;
}

static void set_big(HWND c)
{
    int i;
    SendMessageW(c, WM_SETFONT, (WPARAM)s_big, TRUE);
    for (i = 0; i < s_nplaced; i++)
        if (s_placed[i].h == c)
            s_placed[i].big = 1;
}

/* After a DPI change: new fonts, every control moved and resized. */
static void relayout(void)
{
    int i;
    make_fonts();
    for (i = 0; i < s_nplaced; i++) {
        Placed *pl = &s_placed[i];
        SetWindowPos(pl->h, NULL, S(pl->x + SB_W), S(pl->y + DY), S(pl->w), S(pl->hh), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(pl->h, WM_SETFONT, (WPARAM)(pl->big ? s_big : s_font), TRUE);
    }
    InvalidateRect(s_wnd, NULL, TRUE);
}

static void update_free_space(void)
{
    WCHAR target[MAX_PATH], t[256], a[32], b[32];
    uint64_t freeb = 0, need = s_image_bytes ? s_image_bytes : GAME_BYTES_EST;
    GetWindowTextW(ctl(ID_TARGET), target, MAX_PATH);
    fmt_size(a, 32, need);
    if (!free_space(target, &freeb)) {
        swprintf_s(t, 256, L"The game needs about %s of free space.", a);
    } else {
        fmt_size(b, 32, freeb);
        swprintf_s(t, 256, freeb < need ? L"The game needs about %s; that drive has only %s free. (Files already "
                                          L"installed there are not copied again.)"
                                        : L"The game needs about %s of free space; that drive has %s free.", a, b);
    }
    set_text(ID_FREE, t);
}

static void saves_refresh(void);
static void saves_folder_show(void);
static void presets_fill(void);
static void pt_refresh(void);
static void mv_refresh(void);
static volatile LONG s_up_busy;

/* The Online tab's connection check (nat_check.h), off the window's thread. */
static NatCheck s_nat_result;
static volatile LONG s_nat_busy;
static HWND s_nat_notify;
static int nat_check_port(void)
{
    char v[16] = "";
    const int port = toml_get("p2p_port", v, sizeof v) && v[0] ? atoi(v) : 36000;
    return port > 0 && port < 65536 ? port : 36000;
}
static void nat_check_do(void)
{
    char stun[128] = "";
    WCHAR exe[MAX_PATH] = L"";
    if (!toml_get("p2p_stun", stun, sizeof stun)) stun[0] = 0;
    join(exe, s_game_dir, GAME_EXE);
    nat_check_run(stun, nat_check_port(), exe, &s_nat_result);
}
static DWORD WINAPI nat_check_thread(void *unused)
{
    (void)unused;
    nat_check_do();
    PostMessageW(s_nat_notify, WM_APP_NATCHECK, 0, 0);
    return 0;
}
static void nat_check_start(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_nat_busy, 1, 0)) return;
    set_text(ID_ON_NAT_TEXT, L"Checking your connection\x2026");
    EnableWindow(ctl(ID_ON_NAT_CHECK), FALSE);
    s_nat_notify = s_wnd;
    t = CreateThread(NULL, 0, nat_check_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else {
        InterlockedExchange(&s_nat_busy, 0);
        EnableWindow(ctl(ID_ON_NAT_CHECK), TRUE);
    }
}
static void nat_check_show(void)
{
    WCHAR t[1400];
    online_route_show();  /* (s_route_mapped: from the logs, as they are now) */
    nat_check_text(&s_nat_result, nat_check_port(), s_route_mapped, t, 1400);
    set_text(ID_ON_NAT_TEXT, t);
    EnableWindow(ctl(ID_ON_NAT_CHECK), TRUE);
    InterlockedExchange(&s_nat_busy, 0);
}

static void online_mods_fold_apply(void)
{
    static const int friends[] = { ID_FR_LIST, ID_FR_NAME, ID_FR_ADD, ID_FR_REMOVE, ID_FR_INFO, ID_FR_STATUS };
    static const int mods[] = { ID_ON_MODS_L1, ID_ON_UP_KBS, ID_ON_MODS_L2, ID_ON_UP_PEERS,
                                ID_ON_MODS_L3, ID_ON_MOD_MB, ID_ON_MODS_L4, ID_ON_MODS_MB, ID_ON_MODS_INFO };
    size_t i;
    const int shown = s_cur_tab == TAB_ONLINE;
    for (i = 0; i < sizeof friends / sizeof friends[0]; i++)
        ShowWindow(ctl(friends[i]), shown && !s_mods_fold ? SW_SHOW : SW_HIDE);
    for (i = 0; i < sizeof mods / sizeof mods[0]; i++)
        ShowWindow(ctl(mods[i]), shown && s_mods_fold == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(ctl(ID_ON_NAT_TEXT), shown && s_mods_fold == 2 ? SW_SHOW : SW_HIDE);
    SetWindowTextW(ctl(ID_ON_NAT_CHECK), s_mods_fold == 2 ? L"Check again" : L"Connection \x25B8");
    SetWindowTextW(ctl(ID_ON_MODS_FOLD), s_mods_fold ? L"Friends \x25B8" : L"Mod sharing \x25B8");
    InvalidateRect(s_wnd, NULL, TRUE);  /* (the cards: drawn by the window) */
}

static void show_tab(int t)
{
    int i, k;
    s_cur_tab = t;
    for (k = 0; k < TAB_COUNT; k++)
        for (i = 0; i < s_nctl[k]; i++)
            ShowWindow(s_ctl[k][i], k == t ? SW_SHOW : SW_HIDE);
    s_tab_visible_online = t == TAB_ONLINE;
    if (t == TAB_ONLINE) {  /* (the controls the account/server state hides) */
        static int refreshed;
        online_route_show();
        if (!refreshed && s_online_token[0])  /* (once a start: the account's steps, from the server) */
            refreshed = 1, online_account_refresh();
        online_account_show();
        online_server_show();
        friends_enable();
        online_mods_fold_apply();
        friends_fetch(NULL);
        SetTimer(s_wnd, FRIENDS_TIMER, 20000, NULL);
    } else {
        KillTimer(s_wnd, FRIENDS_TIMER);
    }
    if (t == TAB_PLAY)
        refresh_play();
    if (t == TAB_SETTINGS && !s_settings_dirty)
        settings_load();
    if (setup_step(t) >= 0)
        steps_refresh();
    if (t == TAB_INSTALL)
        update_free_space();
    if (t == TAB_SAVES)
        saves_refresh();
    if (t == TAB_PAINT)
        pt_refresh();
    if (t == TAB_MOVIES)
        mv_refresh();
    if (t == TAB_MODS && !s_bundled_busy)  /* (installing the bundled mods: shown when they're in) */
        mods_show(s_game_dir);
    if (t == TAB_MUSIC)
        music_show(s_game_dir);
    if (t == TAB_ROSTER)
        roster_show(s_game_dir);
    if (t == TAB_TEXPACKS)
        texpacks_show(s_game_dir);
    if (t == TAB_DISCOVER)
        discover_show(s_game_dir);
    if (t == TAB_REPORTS)
        reports_show(s_game_dir);
    if (t == TAB_SUGGESTIONS)
        suggestions_show(s_game_dir);
    if (t == TAB_SETUP || t == TAB_TOOLS)
        setup_show(s_game_dir, s_launcher_dir);
    if (t == TAB_PLAY && !s_up_busy)
        ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    TabCtrl_SetCurSel(s_tab, t);
    /* The page area in full: hidden controls (group boxes, the save list,
       the logo grid) otherwise leave their pixels behind. Without clipping
       the children: a group box doesn't paint its inside, and with
       WS_CLIPCHILDREN the background under it was never erased either - the
       last tab's controls stayed there (e.g. the Movies tab's inside the
       Play tab's "Game folder" and "Updates" boxes). */
    {
        const LONG_PTR style = GetWindowLongPtrW(s_wnd, GWL_STYLE);
        SetWindowLongPtrW(s_wnd, GWL_STYLE, style & ~(LONG_PTR)WS_CLIPCHILDREN);
        RedrawWindow(s_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW);
        SetWindowLongPtrW(s_wnd, GWL_STYLE, style);
        RedrawWindow(s_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

static void saves_setup(HWND list);
static void saves_refresh(void);
static void save_info_copy(const WCHAR *from_dir, const WCHAR *from_name, const WCHAR *to_dir, const WCHAR *to_name);
static int saves_make_writable(const WCHAR *saves);
static void list_columns(HWND list, const WCHAR *const *names, const int *widths, int n);
static HWND pt_setup_grid(void);
static void mv_setup(void);
static void up_setup(void);

/* ── Report a problem: a recorded match and its mods (report_match.h) - shared by the window and --report-send ── */

typedef ReportMatchSet RepMatch;   /* (report_flow.h) */

static void rep_cli_progress(void *ctx, uint64_t sent, uint64_t total)
{
    static int last = -1;
    const int pct = total ? (int)(sent * 100 / total) : 0;
    (void)ctx;
    if (pct / 10 != last / 10 || sent == total) wprintf(L"progress %d%% (%.1f of %.1f MB)\n", pct, sent / 1048576.0, total / 1048576.0);
    last = pct;
}

#define REP_MOD_MAX REPORT_MOD_MAX

/* The mods the match used, hashed, and (online) which the server lacks. */
static void rep_match_load(RepMatch *m, const WCHAR *server)
{
    report_match_set_load(m, s_game_dir, server);
}

/* Bytes to send / already on the server (the tick on: the mods the server lacks, in the server's limit). */
static void rep_match_sizes(const RepMatch *m, uint64_t *to_send, uint64_t *have, int *too_big)
{
    const uint64_t limit = m->max_upload ? m->max_upload : REP_MOD_MAX;
    int i;
    *to_send = *have = 0, *too_big = 0;
    for (i = 0; i < m->n_mods; i++) {
        const ReportMod *d = &m->mods[i];
        if (d->on_server) { *have += d->size; continue; }
        if (!d->hash[0]) continue;                        /* (not installed any more) */
        if (!d->send || *to_send + d->size > limit) { ++*too_big; continue; }
        *to_send += d->size;
    }
}

static void rep_mb(WCHAR *out, int n, uint64_t bytes)
{
    swprintf_s(out, n, bytes >= (10ull << 20) ? L"%.0f MB" : L"%.1f MB", bytes / 1048576.0);
}

/* "Mods used in this match: N (X MB to send, Y MB already on the server)". */
static void rep_match_mods_text(const RepMatch *m, WCHAR *out, int n)
{
    uint64_t to_send, have;
    int too_big, gone = 0, i;
    WCHAR a[32], b[32], extra[200] = L"";
    if (!m->has_match) { wcscpy_s(out, n, L"No match picked: the report goes without one."); return; }
    if (!m->n_mods) { wcscpy_s(out, n, L"Mods used in this match: none."); return; }
    if (m->checked && m->check_why[0]) {   /* (the server's "no mods now": nothing about sizes) */
        swprintf_s(out, n, L"Mods used in this match: %d. %s", m->n_mods, m->check_why);
        return;
    }
    rep_match_sizes(m, &to_send, &have, &too_big);
    for (i = 0; i < m->n_mods; i++) gone += !m->mods[i].hash[0];
    rep_mb(a, 32, to_send);
    rep_mb(b, 32, have);
    if (too_big) swprintf_s(extra, 200, L"; %d too big to send", too_big);
    if (gone) swprintf_s(extra + wcslen(extra), 200 - wcslen(extra), L"; %d not installed any more", gone);
    if (m->checked)
        swprintf_s(out, n, L"Mods used in this match: %d (%s to send, %s already on the server%s).%s%s", m->n_mods, a, b,
                   extra, m->check_why[0] ? L" " : L"", m->check_why);
    else
        swprintf_s(out, n, L"Mods used in this match: %d (%s to send%s; the server couldn't be asked which it has%s%s).",
                   m->n_mods, a, extra, m->check_why[0] ? L": " : L"", m->check_why);
}

/* The upload's match and mod parts into u: zips in <game>\Reports\.upload (rep_match_done deletes them). */
static void rep_match_parts(RepMatch *m, int with_mods, ReportUpload *u, char *name_utf8, int name_n)
{
    report_match_set_parts(m, s_game_dir, with_mods, u, name_utf8, name_n);
}

static void rep_match_done(RepMatch *m)
{
    report_match_set_done(m);
}

/* What the result page / the CLI says about the match and mods after a send. */
static void rep_match_result_text(const RepMatch *m, const ReportResult *r, WCHAR *out, int n)
{
    out[0] = 0;
    if (!m->has_match) return;
    swprintf_s(out, n, L"Match: %s.", r->match_stored ? L"sent" : L"not kept by the server");
    if (r->mods_sent || r->mods_dropped)
        swprintf_s(out + wcslen(out), n - wcslen(out), L" Mods: %d sent, %d kept%s%s.", r->mods_sent, r->mods_stored,
                   r->mods_why[0] ? L" - " : L"", r->mods_why);
}

#define X0 28

/* The Online tab's account box: a control moved to x (from X0), y - as add() places it - and shown with its tab. */
static void on_place(int id, int x, int y, int w, int h)
{
    MoveWindow(ctl(id), S(X0 + x + PAD + SB_W), S(y + DY), S(w), S(h), TRUE);
    ShowWindow(ctl(id), s_tab_visible_online ? SW_SHOW : SW_HIDE);
}
/* Report a problem: a window asks what happened (kind, how often, what and
   what just before), then the zip (report.c) is made with that description in
   its report.txt, and the window says where to post it: a new thread in the
   Discord's bug-svr11 forum (the description is copied, ready to paste). */
#define DISCORD_INVITE L"https://discord.com/invite/QhGSuDKHwk"
#define DISCORD_FORUM L"bug-svr11"
enum { REP_KIND = 300, REP_AGAIN, REP_WHAT, REP_BEFORE, REP_STATUS,
       REP_CONTACT, REP_ADDPIC, REP_SUMMARY, REP_SEND, REP_COPYID, REP_MATCH, REP_MODS, REP_INCMODS, REP_PROGRESS, REP_INCSAVE,
       REP_PIC0 = 340, REP_DEL0 = 350 };
#define WM_APP_REP_SENT (WM_APP + 70)   /* the report window's own: the send finished (s_rep_result) */
#define WM_APP_REP_MODS (WM_APP + 71)   /* ... the picked match's mods are hashed and checked (lParam: a RepMatch) */
#define WM_APP_REP_PROGRESS (WM_APP + 72)   /* ... the upload: wParam percent, lParam MB sent x10 */
#define REP_DY 122                      /* (page 1's rows below the match and save rows) */
static ReportMatch s_rep_matches[REPORT_MAX_MATCHES];   /* the picker's matches, newest first */
static int s_rep_nmatches;
static RepMatch s_rep_rm;          /* the picked match and its mods (checked) */
static int s_rep_mods_gen;         /* (a newer pick replaces an older check's result) */
static volatile LONG s_rep_mods_busy;
static WCHAR s_rep_pics[REPORT_MAX_IMAGES][MAX_PATH];  /* the pictures to send */
static int s_rep_npics;
static ReportResult s_rep_result;
static char s_rep_problem[120], s_rep_happens[40], s_rep_what[4096], s_rep_before[4096], s_rep_contact[200];
#define k_rep_kinds report_kinds    /* (report_flow.c: the game's own form asks the same) */
#define k_rep_again report_happens
static int s_rep_done;            /* 1: done, -1: closed */
static int s_rep_page;            /* 1: the questions, 2: where to post */
static WCHAR s_rep_zip[MAX_PATH];
static WCHAR s_rep_text[4096];    /* the description, as copied */

static HWND rep_add(HWND w, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int cw, int ch, int id,
                    HFONT font)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"Edit") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(cw), S(ch), w, (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : s_font), TRUE);
    return c;
}

static void rep_copy(HWND w)
{
    size_t n = (wcslen(s_rep_text) + 1) * sizeof(WCHAR);
    HGLOBAL g;
    if (!s_rep_text[0] || !OpenClipboard(w))
        return;
    EmptyClipboard();
    g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) {
        void *p = GlobalLock(g);
        memcpy(p, s_rep_text, n);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g))
            GlobalFree(g);
    }
    CloseClipboard();
}

static void rep_clear(HWND w)
{
    HWND c;
    while ((c = GetWindow(w, GW_CHILD)) != NULL)
        DestroyWindow(c);
}

/* Page 1's picture rows (a name and Remove per picture) and the line saying what Send sends. */
static void rep_pics_show(HWND w)
{
    ReportUpload u;
    WCHAR line[300];
    int i;
    for (i = 0; i < REPORT_MAX_IMAGES; i++) {
        const WCHAR *name = i < s_rep_npics ? wcsrchr(s_rep_pics[i], L'\\') : NULL;
        SetDlgItemTextW(w, REP_PIC0 + i, i < s_rep_npics ? (name ? name + 1 : s_rep_pics[i]) : L"");
        ShowWindow(GetDlgItem(w, REP_DEL0 + i), i < s_rep_npics ? SW_SHOW : SW_HIDE);
    }
    EnableWindow(GetDlgItem(w, REP_ADDPIC), s_rep_npics < REPORT_MAX_IMAGES);
    ZeroMemory(&u, sizeof u);
    u.zip = L"report.zip";
    u.n_images = s_rep_npics;
    if (IsDlgButtonChecked(w, REP_INCSAVE) == BST_CHECKED) u.save_bytes = report_main_save_size(s_game_dir);
    if (s_rep_rm.has_match && !s_rep_mods_busy) {   /* (the match and the mods Send would take, as rep_match_parts) */
        const uint64_t limit = s_rep_rm.max_upload ? s_rep_rm.max_upload : REP_MOD_MAX;
        uint64_t total = 0;
        u.match_name = "match";
        u.mods = s_rep_rm.mods;
        u.n_mods = s_rep_rm.n_mods;
        for (i = 0; IsDlgButtonChecked(w, REP_INCMODS) == BST_CHECKED && i < s_rep_rm.n_mods; i++) {
            const ReportMod *d = &s_rep_rm.mods[i];
            if (d->on_server || !d->hash[0] || !d->send || total + d->size > limit) continue;
            u.mod_zips[i] = L"";
            total += d->size;
        }
    }
    report_send_summary(&u, line, 300);
    SetDlgItemTextW(w, REP_SUMMARY, line);
}

/* Adds a picture (checked: png / jpeg / webp, 5 MB at most). 0 with the reason on the status line if not. */
static int rep_pic_add(HWND w, const WCHAR *path)
{
    WCHAR why[300];
    int i;
    if (s_rep_npics >= REPORT_MAX_IMAGES) return 0;
    for (i = 0; i < s_rep_npics; i++)
        if (!_wcsicmp(s_rep_pics[i], path)) return 1;
    if (!report_image_ok(path, why, 300)) {
        if (w) SetDlgItemTextW(w, REP_STATUS, why);
        return 0;
    }
    wcscpy_s(s_rep_pics[s_rep_npics++], MAX_PATH, path);
    return 1;
}

/* "Add pictures...": a file dialog, several at once. */
static void rep_pics_pick(HWND w)
{
    static WCHAR files[8192];
    OPENFILENAMEW of;
    ZeroMemory(&of, sizeof of);
    files[0] = 0;
    of.lStructSize = sizeof of;
    of.hwndOwner = w;
    of.lpstrFilter = L"Pictures (PNG, JPEG, WebP)\0*.png;*.jpg;*.jpeg;*.webp\0";
    of.lpstrFile = files;
    of.nMaxFile = 8192;
    of.lpstrTitle = L"Pictures for the report";
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    SetDlgItemTextW(w, REP_STATUS, L"");
    if (files[wcslen(files) + 1] == 0) {   /* (one file: its full path) */
        rep_pic_add(w, files);
    } else {                               /* (several: the folder, then the names) */
        const WCHAR *p = files + wcslen(files) + 1;
        for (; *p; p += wcslen(p) + 1) {
            WCHAR full[MAX_PATH];
            swprintf_s(full, MAX_PATH, L"%s\\%s", files, p);
            if (!rep_pic_add(w, full) && s_rep_npics >= REPORT_MAX_IMAGES) break;
        }
    }
    rep_pics_show(w);
}

/* The match picked (REP_MATCH): its mods hashed and checked with the server on a worker (a few seconds for big
   mods the first time - the hashes are kept in Reports\.modhash); Send waits for it. */
typedef struct { HWND w; int gen; RepMatch m; } RepModsJob;
static DWORD WINAPI rep_mods_thread(LPVOID p)
{
    RepModsJob *j = (RepModsJob *)p;
    WCHAR server[260];
    online_server_text(server, 260);
    rep_match_load(&j->m, server);
    if (!PostMessageW(j->w, WM_APP_REP_MODS, (WPARAM)j->gen, (LPARAM)j)) free(j);
    return 0;
}

static void rep_match_pick(HWND w)
{
    const int sel = (int)SendDlgItemMessageW(w, REP_MATCH, CB_GETCURSEL, 0, 0);
    RepModsJob *j;
    HANDLE t;
    ++s_rep_mods_gen;
    ZeroMemory(&s_rep_rm, sizeof s_rep_rm);
    if (sel < 0 || sel >= s_rep_nmatches) {
        WCHAR line[600];
        rep_match_mods_text(&s_rep_rm, line, 600);
        SetDlgItemTextW(w, REP_MODS, line);
        EnableWindow(GetDlgItem(w, REP_INCMODS), FALSE);
        InterlockedExchange(&s_rep_mods_busy, 0);
        rep_pics_show(w);
        return;
    }
    s_rep_rm.match = s_rep_matches[sel];
    s_rep_rm.has_match = 1;
    SetDlgItemTextW(w, REP_MODS, L"Mods used in this match: checking\x2026");
    if (!(j = (RepModsJob *)calloc(1, sizeof *j))) return;
    j->w = w, j->gen = s_rep_mods_gen, j->m = s_rep_rm;
    InterlockedExchange(&s_rep_mods_busy, 1);
    if ((t = CreateThread(NULL, 0, rep_mods_thread, j, 0, NULL)) != NULL) CloseHandle(t);
    else { free(j); InterlockedExchange(&s_rep_mods_busy, 0); }
}

/* Page 1: the questions. */
static void rep_page1(HWND w)
{
    HWND c;
    int i;
    s_rep_page = 1;
    rep_clear(w);
    rep_add(w, L"Static", L"Report a problem", 0, 20, 14, 480, 30, 0, s_big);
    rep_add(w, L"Static", L"Tell us what went wrong. The report also takes your newest game logs, crash reports and "
                          L"settings (your online password and token are left out). If you can, run the game and make "
                          L"it happen once more first: the newest log is the one that counts.",
            SS_NOPREFIX, 20, 48, 480, 52, 0, NULL);
    rep_add(w, L"Static", L"What kind of problem?", 0, 20, 112, 170, 20, 0, NULL);
    c = rep_add(w, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 196, 108, 304, 300, REP_KIND, NULL);
    for (i = 0; i < (int)(sizeof k_rep_kinds / sizeof *k_rep_kinds); i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)k_rep_kinds[i]);
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    rep_add(w, L"Static", L"Does it happen again?", 0, 20, 146, 170, 20, 0, NULL);
    c = rep_add(w, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 196, 142, 304, 200, REP_AGAIN, NULL);
    for (i = 0; i < 3; i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)k_rep_again[i]);
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    rep_add(w, L"Static", L"What happened?", 0, 20, 180, 480, 20, 0, NULL);
    c = rep_add(w, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 20, 200, 480,
                86, REP_WHAT, NULL);
    SendMessageW(c, EM_LIMITTEXT, 1500, 0);
    rep_add(w, L"Static", L"What were you doing just before? (menu or mode, match type, superstars, mods)", SS_NOPREFIX,
            20, 296, 480, 20, 0, NULL);
    c = rep_add(w, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 20, 316, 480,
                70, REP_BEFORE, NULL);
    SendMessageW(c, EM_LIMITTEXT, 1500, 0);
    rep_add(w, L"Static", L"Your Discord name (optional - so we can ask you):", SS_NOPREFIX, 20, 398, 290, 20, 0, NULL);
    c = rep_add(w, L"Edit", L"", ES_AUTOHSCROLL | WS_TABSTOP, 310, 394, 190, 24, REP_CONTACT, NULL);
    SendMessageW(c, EM_LIMITTEXT, 60, 0);
    /* the match (the game's recordings, logs\matches: report_match.h) - the newest picked - and its mods */
    rep_add(w, L"Static", L"Match:", 0, 20, 432, 60, 20, 0, NULL);
    c = rep_add(w, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 84, 428, 416, 300, REP_MATCH, NULL);
    s_rep_nmatches = report_match_list(s_game_dir, s_rep_matches, REPORT_MAX_MATCHES);
    for (i = 0; i < s_rep_nmatches; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s_rep_matches[i].label);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)(s_rep_nmatches ? L"None - send the report without a match"
                                                              : L"None (no recorded matches)"));
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    rep_add(w, L"Static", L"", SS_LEFT | SS_NOPREFIX, 20, 458, 480, 34, REP_MODS, NULL);
    c = rep_add(w, L"Button", L"Include the mods used in this match (helps us reproduce it)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 20, 494, 480, 22, REP_INCMODS, NULL);
    SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);   /* (on unless the player turns it off) */
    c = rep_add(w, L"Button", L"Include my main save (story / Universe progress and options - not my created superstars "
                              L"or Paint Tool)", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 20, 516, 480, 36, REP_INCSAVE, NULL);
    SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);   /* (on unless the player turns it off) */
    rep_add(w, L"Static", L"Pictures (up to 5): screenshots of the problem help a lot.", SS_NOPREFIX, 20, 432 + REP_DY, 360, 20, 0,
            NULL);
    rep_add(w, L"Button", L"Add pictures\x2026", WS_TABSTOP, 390, 428 + REP_DY, 110, 26, REP_ADDPIC, NULL);
    for (i = 0; i < REPORT_MAX_IMAGES; i++) {
        rep_add(w, L"Static", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 20, 462 + REP_DY + i * 26, 380, 20, REP_PIC0 + i, NULL);
        rep_add(w, L"Button", L"Remove", WS_TABSTOP, 410, 458 + REP_DY + i * 26, 90, 24, REP_DEL0 + i, NULL);
    }
    rep_add(w, L"Static", L"", SS_LEFT | SS_NOPREFIX, 20, 594 + REP_DY, 480, 34, REP_SUMMARY, NULL);
    c = rep_add(w, PROGRESS_CLASSW, L"", 0, 20, 602 + REP_DY, 480, 16, REP_PROGRESS, NULL);   /* (over the summary, while sending) */
    SendMessageW(c, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    ShowWindow(c, SW_HIDE);
    rep_add(w, L"Static", L"", SS_LEFT, 20, 640 + REP_DY, 180, 36, REP_STATUS, NULL);
    rep_add(w, L"Button", L"Send report", BS_DEFPUSHBUTTON | WS_TABSTOP, 300, 636 + REP_DY, 110, 32, REP_SEND, NULL);
    rep_add(w, L"Button", L"Cancel", WS_TABSTOP, 420, 636 + REP_DY, 80, 32, IDCANCEL, NULL);
    rep_pics_show(w);
    rep_match_pick(w);
    SetFocus(GetDlgItem(w, REP_WHAT));
}

/* The description from page 1 (s_rep_text, and UTF-8 for report.txt); 0 if "What happened?" is empty. */
static int rep_describe(HWND w, char *utf8, int n)
{
    WCHAR what[1600], before[1600];
    int k = (int)SendDlgItemMessageW(w, REP_KIND, CB_GETCURSEL, 0, 0);
    int a = (int)SendDlgItemMessageW(w, REP_AGAIN, CB_GETCURSEL, 0, 0);
    const WCHAR *p;
    GetDlgItemTextW(w, REP_WHAT, what, 1600);
    GetDlgItemTextW(w, REP_BEFORE, before, 1600);
    for (p = what; *p == L' ' || *p == L'\r' || *p == L'\n' || *p == L'\t'; p++)
        ;
    if (!*p)
        return 0;
    if (k < 0 || k >= (int)(sizeof k_rep_kinds / sizeof *k_rep_kinds)) k = 0;
    if (a < 0 || a > 2) a = 0;
    swprintf_s(s_rep_text, 4096,
               L"SvR 2011 PC port %s\r\nProblem: %s\r\nHappens: %s\r\n\r\nWhat happened:\r\n%s\r\n\r\n"
               L"Just before:\r\n%s\r\n",
               PORT_VERSION, k_rep_kinds[k], k_rep_again[a], what, before[0] ? before : L"(not said)");
    WideCharToMultiByte(CP_UTF8, 0, s_rep_text, -1, utf8, n, NULL, NULL);
    {   /* (the fields for the server's report.json) */
        WCHAR contact[64];
        GetDlgItemTextW(w, REP_CONTACT, contact, 64);
        WideCharToMultiByte(CP_UTF8, 0, k_rep_kinds[k], -1, s_rep_problem, sizeof s_rep_problem, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, k_rep_again[a], -1, s_rep_happens, sizeof s_rep_happens, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, what, -1, s_rep_what, sizeof s_rep_what, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, before, -1, s_rep_before, sizeof s_rep_before, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, contact, -1, s_rep_contact, sizeof s_rep_contact, NULL, NULL);
    }
    return 1;
}

/* The send (a worker: it can take a while on a slow line): report.json from the answers, the zip, the pictures,
   to the configured online server. The window gets WM_APP_REP_SENT. */
static WCHAR s_rep_server[260];
static int s_rep_with_mods;
static void rep_win_progress(void *ctx, uint64_t sent, uint64_t total)
{
    static DWORD last;
    const DWORD now = GetTickCount();
    if (now - last < 200 && sent < total) return;   /* (5 a second is plenty) */
    last = now;
    PostMessageW((HWND)ctx, WM_APP_REP_PROGRESS, (WPARAM)(total ? sent * 100 / total : 0), (LPARAM)(total / 104858));
}
static DWORD WINAPI rep_send_thread(LPVOID p)
{
    HWND w = (HWND)p;
    ReportUpload u;
    char sig[400], ver[40], match_name[300] = "";
    char *json;
    int i;
    WideCharToMultiByte(CP_UTF8, 0, PORT_VERSION, -1, ver, sizeof ver, NULL, NULL);
    report_signature(s_game_dir, sig, sizeof sig);
    {
        char upd[600];
        update_log_last(UPLOG_DIR, upd, sizeof upd);
        json = report_json("launcher", ver, s_rep_problem, s_rep_happens, s_rep_what, s_rep_before,
                           s_rep_contact, sig, upd);
    }
    ZeroMemory(&u, sizeof u);
    u.json = json;
    u.zip = s_rep_zip;
    for (i = 0; i < s_rep_npics; i++) u.images[i] = s_rep_pics[i];
    u.n_images = s_rep_npics;
    rep_match_parts(&s_rep_rm, s_rep_with_mods, &u, match_name, sizeof match_name);
    u.progress = rep_win_progress;
    u.ctx = w;
    if (test_run_refuses_url(s_rep_server, "/api/reports")) {  /* (a test run: never a server off this PC) */
        ZeroMemory(&s_rep_result, sizeof s_rep_result);
        wcscpy_s(s_rep_result.why, 300, L"Test run: a report to a server not on this PC was refused (SVR2011_ALLOW_LIVE=1 lets it through).");
    } else if (report_send(s_rep_server, &u, &s_rep_result) && s_rep_result.secret[0]) {
        receipt_add(s_game_dir, "launcher", s_rep_result.id, s_rep_result.secret, s_rep_problem, s_rep_what);
    }
    rep_match_done(&s_rep_rm);
    free(json);
    PostMessageW(w, WM_APP_REP_SENT, 0, 0);
    return 0;
}

/* The status line: its own small place left of the buttons, or (wide: a send failed - the server's reason can be
   long) the summary's full-width place above them, the summary hidden. */
static void rep_status_wide(HWND w, int wide)
{
    SetWindowPos(GetDlgItem(w, REP_STATUS), NULL, S(20), S(wide ? 594 + REP_DY : 640 + REP_DY), S(wide ? 480 : 180),
                 S(wide ? 40 : 36), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(GetDlgItem(w, REP_SUMMARY), wide ? SW_HIDE : SW_SHOW);
}

/* The questions page after a send that failed: as it was, with the reason and Send as "Try again" (reports are
   only sent - nothing is saved for posting elsewhere). */
static void rep_not_sent(HWND w, const WCHAR *why)
{
    WCHAR t[600];
    s_rep_page = 1;
    EnableWindow(GetDlgItem(w, REP_SEND), TRUE);
    EnableWindow(GetDlgItem(w, REP_MATCH), TRUE);
    EnableWindow(GetDlgItem(w, REP_INCMODS), s_rep_rm.n_mods > 0);
    ShowWindow(GetDlgItem(w, REP_PROGRESS), SW_HIDE);
    rep_status_wide(w, 1);
    SetDlgItemTextW(w, REP_SEND, L"Try again");
    swprintf_s(t, 600, L"Not sent: %s Check you're online, then try again.", why);
    SetDlgItemTextW(w, REP_STATUS, t);
    SetFocus(GetDlgItem(w, REP_SEND));
}

/* Page 3: sent (the id, how often seen before), or not (rep_not_sent). */
static void rep_page3(HWND w)
{
    WCHAR t[1200], id[64];
    if (!s_rep_result.id[0]) {
        rep_not_sent(w, s_rep_result.why[0] ? s_rep_result.why : L"no answer from the server.");
        return;
    }
    s_rep_page = 3;
    rep_clear(w);
    MultiByteToWideChar(CP_UTF8, 0, s_rep_result.id, -1, id, 64);
    {
        WCHAR seen[600] = L"";
        if (s_rep_result.seen_before > 0)
            swprintf_s(seen, 600, L"\n\nThis problem was already reported %d time%s - yours adds to it, thank you.",
                       s_rep_result.seen_before, s_rep_result.seen_before == 1 ? L"" : L"s");
        {
            WCHAR mm[400];
            rep_match_result_text(&s_rep_rm, &s_rep_result, mm, 400);
            if (mm[0]) swprintf_s(seen + wcslen(seen), 600 - wcslen(seen), L"\n\n%.150s", mm);
        }
        swprintf_s(t, 1200,
                   L"Your report reached us.\n\nReport id:  %s%s\n\nIf you post about it on our Discord (" DISCORD_FORUM
                   L" forum), give this id.",
                   id, seen);
    }
    rep_add(w, L"Static", L"Report sent", 0, 20, 14, 480, 30, 0, s_big);
    rep_add(w, L"Static", t, SS_LEFT | SS_NOPREFIX, 20, 52, 480, 290, 0, NULL);
    rep_add(w, L"Button", L"Copy the id", BS_DEFPUSHBUTTON | WS_TABSTOP, 20, 360, 120, 32, REP_COPYID, NULL);
    rep_add(w, L"Button", L"Close", WS_TABSTOP, 420, 360, 80, 32, IDCANCEL, NULL);
    rep_add(w, L"Static", L"", SS_LEFT, 20, 404, 480, 20, REP_STATUS, NULL);
    SetFocus(GetDlgItem(w, IDCANCEL));
}

/* The window closes: the report's zip goes (sent reports aren't kept for posting - the Discord gets no zips). */
static void rep_drop_zip(void)
{
    if (s_rep_zip[0]) DeleteFileW(s_rep_zip);
    s_rep_zip[0] = 0;
}

static LRESULT CALLBACK rep_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case REP_SEND: {
            static char utf8[8192];
            int logs = 0, crashes = 0;
            if (s_rep_mods_busy) {
                SetDlgItemTextW(w, REP_STATUS, L"One moment - the match's mods are still being checked.");
                return 0;
            }
            s_rep_with_mods = IsDlgButtonChecked(w, REP_INCMODS) == BST_CHECKED;
            if (!rep_describe(w, utf8, sizeof utf8)) {
                SetDlgItemTextW(w, REP_STATUS, L"Please say what happened first.");
                SetFocus(GetDlgItem(w, REP_WHAT));
                return 0;
            }
            if (report_text_has_url(s_rep_what) || report_text_has_url(s_rep_before) || report_text_has_url(s_rep_contact)) {
                SetDlgItemTextW(w, REP_STATUS, REPORT_NO_URLS_W);
                SetFocus(GetDlgItem(w, report_text_has_url(s_rep_what) ? REP_WHAT
                                       : report_text_has_url(s_rep_before) ? REP_BEFORE : REP_CONTACT));
                return 0;
            }
            /* (the match goes as its own part: not in the zip; a try again makes it again) */
            rep_drop_zip();
            if (!report_make_match(s_game_dir, PORT_VERSION, utf8, NULL, IsDlgButtonChecked(w, REP_INCSAVE) == BST_CHECKED,
                                   s_rep_zip, MAX_PATH, &logs, &crashes)) {
                SetDlgItemTextW(w, REP_STATUS, L"The report could not be written (is the game folder read-only?).");
                return 0;
            }
            online_server_text(s_rep_server, 260);
            rep_status_wide(w, 0);
            SetDlgItemTextW(w, REP_STATUS, L"Sending\x2026");
            EnableWindow(GetDlgItem(w, REP_SEND), FALSE);
            EnableWindow(GetDlgItem(w, REP_MATCH), FALSE);
            EnableWindow(GetDlgItem(w, REP_INCMODS), FALSE);
            ShowWindow(GetDlgItem(w, REP_SUMMARY), SW_HIDE);
            ShowWindow(GetDlgItem(w, REP_PROGRESS), SW_SHOW);
            {
                HANDLE t = CreateThread(NULL, 0, rep_send_thread, w, 0, NULL);
                if (t) CloseHandle(t);
                else { ZeroMemory(&s_rep_result, sizeof s_rep_result); wcscpy_s(s_rep_result.why, 300, L"No thread."); rep_page3(w); }
            }
            return 0;
        }
        case REP_ADDPIC:
            rep_pics_pick(w);
            return 0;
        case REP_MATCH:
            if (HIWORD(wp) == CBN_SELCHANGE) rep_match_pick(w);
            return 0;
        case REP_INCMODS:
        case REP_INCSAVE:
            rep_pics_show(w);   /* (the summary: with or without the mods / the save) */
            return 0;
        case REP_COPYID: {
            WCHAR id[64];
            MultiByteToWideChar(CP_UTF8, 0, s_rep_result.id, -1, id, 64);
            wcscpy_s(s_rep_text, 4096, id);
            rep_copy(w);
            SetDlgItemTextW(w, REP_STATUS, L"Report id copied.");
            return 0;
        }
        case IDCANCEL:
            rep_drop_zip();
            s_rep_done = -1;
            DestroyWindow(w);
            return 0;
        }
        if (LOWORD(wp) >= REP_DEL0 && LOWORD(wp) < REP_DEL0 + REPORT_MAX_IMAGES) {   /* (Remove a picture) */
            int k = LOWORD(wp) - REP_DEL0;
            if (k < s_rep_npics) {
                memmove(s_rep_pics[k], s_rep_pics[k + 1], (size_t)(s_rep_npics - k - 1) * sizeof s_rep_pics[0]);
                s_rep_npics--;
                rep_pics_show(w);
            }
            return 0;
        }
    }
    if (m == WM_APP_REP_SENT) {
        rep_page3(w);
        return 0;
    }
    if (m == WM_APP_REP_MODS) {   /* (a picked match's mods checked: shown, unless a newer pick is on its way) */
        RepModsJob *j = (RepModsJob *)lp;
        if ((int)wp == s_rep_mods_gen && s_rep_page == 1) {
            WCHAR line[600];
            s_rep_rm = j->m;
            rep_match_mods_text(&s_rep_rm, line, 600);
            SetDlgItemTextW(w, REP_MODS, line);
            EnableWindow(GetDlgItem(w, REP_INCMODS), s_rep_rm.n_mods > 0);
            InterlockedExchange(&s_rep_mods_busy, 0);
            rep_pics_show(w);
        }
        free(j);
        return 0;
    }
    if (m == WM_APP_REP_PROGRESS) {
        WCHAR t[120];
        const double mb = (double)lp / 10.0;
        SendDlgItemMessageW(w, REP_PROGRESS, PBM_SETPOS, wp, 0);
        swprintf_s(t, 120, L"Sending\x2026 %d%% of %.1f MB", (int)wp, mb);
        SetDlgItemTextW(w, REP_STATUS, t);
        return 0;
    }
    if (m == WM_CTLCOLORSTATIC && GetDlgCtrlID((HWND)lp) == REP_STATUS) {
        SetTextColor((HDC)wp, s_rep_page == 1 ? RGB(190, 30, 45) : RGB(30, 120, 60));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    if (m == WM_CLOSE) {
        rep_drop_zip();
        s_rep_done = -1;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* The report window (page 1 or 2). capture_file: drawn off-screen and never
   shown or activated, saved as a BMP and closed (--capture ... --report-window). */
static void capture_wnd(HWND wnd, const WCHAR *file);
static void report_window(int page, const WCHAR *capture_file)
{
    static int registered;
    WNDCLASSW wc;
    RECT r = { 0, 0, 0, 0 }, pr;
    HWND w;
    MSG msg;
    int quit = 0;
    if (!registered) {
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = rep_proc;
        wc.hInstance = s_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = s_icon;
        wc.lpszClassName = L"SvR2011Report";
        RegisterClassW(&wc);
        registered = 1;
    }
    r.right = S(520);
    r.bottom = S(684 + REP_DY);
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(s_wnd, &pr);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SvR2011Report", L"Report a problem", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        capture_file ? -8000 : pr.left + (pr.right - pr.left - (r.right - r.left)) / 2,
                        capture_file ? -8000 : pr.top + (pr.bottom - pr.top - (r.bottom - r.top)) / 2,
                        r.right - r.left, r.bottom - r.top, s_wnd, NULL, s_inst, NULL);
    if (!w)
        return;
    s_rep_done = 0;
    rep_page1(w);
    if (page == 2) rep_not_sent(w, L"the server didn't answer (a sample).");
    if (capture_file) {
        int k;
        ShowWindow(w, SW_SHOWNOACTIVATE);
        UpdateWindow(w);
        for (k = 0; k < 20; k++) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(15);
        }
        capture_wnd(w, capture_file);
        DestroyWindow(w);
        return;
    }
    EnableWindow(s_wnd, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(GetDlgItem(w, page == 2 ? REP_SEND : REP_WHAT));
    while (!s_rep_done) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
            quit = 1;
            break;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(s_wnd, TRUE);
    SetForegroundWindow(s_wnd);
    if (quit) {
        if (IsWindow(w))
            DestroyWindow(w);
        PostQuitMessage(0);
    }
}

static void report_problem(void)
{
    if (!is_game_folder(s_game_dir)) {
        MessageBoxW(s_wnd, L"Install the game first (Install tab): a report is made from its logs.", WINDOW_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    s_rep_zip[0] = 0;
    s_rep_text[0] = 0;
    s_rep_npics = 0;
    ZeroMemory(&s_rep_result, sizeof s_rep_result);
    report_window(1, NULL);
}

/* While big_roster is on, per-mod saves are on too (the indices from 242 up are in no table SaveData.dat holds):
   ticked and greyed out, its label says why. */
static void saves_permod_lock(void)
{
    const int big = IsDlgButtonChecked(s_wnd, ID_SV_BIGROSTER) == BST_CHECKED;
    if (big) CheckDlgButton(s_wnd, ID_SV_PERMOD, BST_CHECKED);
    EnableWindow(ctl(ID_SV_PERMOD), !big);
    SetWindowTextW(ctl(ID_SV_PERMOD), big ? L"Experimental: each mod's save data in its own file - on: big roster needs it"
                                          : L"Experimental: each superstar mod's save data in its own file (Saves\\.mods)");
}

/* big_roster's warning: a window of its own (so --capture can draw it), Turn on / Cancel (the default). Returns 1
   for Turn on. capture_file: drawn off-screen, saved as a BMP and closed (--capture saves <file> --big-roster-warning). */
static int s_br_done;
static LRESULT CALLBACK br_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        s_br_done = LOWORD(wp) == IDOK ? 1 : -1;
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_CLOSE) {
        s_br_done = -1;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
static int big_roster_confirm(const WCHAR *capture_file)
{
    static int registered;
    WNDCLASSW wc;
    RECT r = { 0, 0, 0, 0 }, pr;
    HWND w, c;
    MSG msg;
    int quit = 0, k;
    if (!registered) {
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = br_proc;
        wc.hInstance = s_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = s_icon;
        wc.lpszClassName = L"SvR2011BigRoster";
        RegisterClassW(&wc);
        registered = 1;
    }
    r.right = S(460);
    r.bottom = S(170);
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(s_wnd, &pr);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SvR2011BigRoster", L"\xD83E\xDDEA Experimental",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        capture_file ? -8000 : pr.left + (pr.right - pr.left - (r.right - r.left)) / 2,
                        capture_file ? -8000 : pr.top + (pr.bottom - pr.top - (r.bottom - r.top)) / 2,
                        r.right - r.left, r.bottom - r.top, s_wnd, NULL, s_inst, NULL);
    if (!w)
        return 0;
    c = CreateWindowExW(0, L"Static", NULL, WS_CHILD | WS_VISIBLE | SS_ICON, S(16), S(18), S(32), S(32), w, NULL,
                        s_inst, NULL);
    if (c) SendMessageW(c, STM_SETICON, (WPARAM)LoadIconW(NULL, IDI_WARNING), 0);
    c = CreateWindowExW(0, L"Static", BIG_ROSTER_WARNING, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, S(62), S(16),
                        S(384), S(96), w, NULL, s_inst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    c = CreateWindowExW(0, L"Button", L"Turn on", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, S(246),
                        S(124), S(100), S(30), w, (HMENU)(INT_PTR)IDOK, s_inst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    c = CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(354),
                        S(124), S(92), S(30), w, (HMENU)(INT_PTR)IDCANCEL, s_inst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    s_br_done = 0;
    if (capture_file) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        UpdateWindow(w);
        for (k = 0; k < 20; k++) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(15);
        }
        capture_wnd(w, capture_file);
        DestroyWindow(w);
        return 0;
    }
    EnableWindow(s_wnd, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(GetDlgItem(w, IDCANCEL));
    while (!s_br_done) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
            quit = 1;
            break;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(s_wnd, TRUE);
    SetForegroundWindow(s_wnd);
    if (quit) {
        if (IsWindow(w))
            DestroyWindow(w);
        PostQuitMessage(0);
    }
    return s_br_done == 1;
}

static void build_ui(void)
{
    TCITEMW ti;
    HWND h;
    WCHAR v[64];
    int i;
    static const WCHAR *names[TAB_COUNT] = { L"Play", L"Settings", L"Online", L"Install", L"DLC", L"Saves", L"Paint Tool", L"Movies", L"Android Install", L"Mods", L"Music", L"Roster", L"Texture packs", L"Setup", L"Discovery", L"Tools", L"Reports", L"Suggestions" };

    /* The tabs (never shown: the sidebar picks them; --capture and show_tab
       still use its selection). */
    s_tab = add(-1, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 12, 10, 596, 28, ID_TAB);
    for (i = 0; i < TAB_COUNT; i++) {
        ti.mask = TCIF_TEXT;
        ti.pszText = (WCHAR *)names[i];
        TabCtrl_InsertItem(s_tab, i, &ti);
    }

    /* Play: the game (status, time played, PLAY), its folder, help, updates. */
    /* (cards: the picture card, three tiles, Setup / Saves / Mods / Texture packs, the status line -
       paint_play draws what's on them) */
    add(TAB_PLAY, L"Button", L"", BS_GROUPBOX, X0, -8, 724, 400, 0);
    add(TAB_PLAY, L"Button", L"", BS_GROUPBOX, X0, 406, 724, 44, 0);
    add(TAB_PLAY, L"Button", L"\x01", BS_GROUPBOX, X0, 462, 724, 48, 0);   /* (the notice: drawn only with one) */
    add(TAB_PLAY, L"Button", L"", BS_GROUPBOX, X0, 544, 724, 48, 0);
    h = add(TAB_PLAY, L"Button", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, X0 + 28, 212, 190, 60, ID_PLAY);
    set_big(h);
    /* Start without mods: the game with no mods at all, saves untouched (SVR2011_NO_MODS) */
    add(TAB_PLAY, L"Button", L"Start Without Mods", BS_PUSHBUTTON | WS_TABSTOP, X0 + 230, 212, 190, 60, ID_PLAY_NOMODS);
    add(TAB_PLAY, L"Button", L"Close on start", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 28, 300, 126, 20, ID_CLOSE_ON_PLAY);
    /* the faster start (fast_start.cpp): settings like the Settings tab's, kept by Play */
    add(TAB_PLAY, L"Button", L"Skip intros", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 160, 300, 104, 20, ID_SKIP_INTROS);
    add(TAB_PLAY, L"Button", L"Skip practice ring", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 270, 300, 130, 20,
        ID_SKIP_TRAINING);
    /* (fast_loads.cpp: the Start Screen's loading screens at four updates a frame) */
    add(TAB_PLAY, L"Button", L"Faster loading screens (animations speed up)", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 406,
        300, 300, 20, ID_FAST_LOADS);
    add(TAB_PLAY, L"Button", L"", BS_PUSHBUTTON | WS_TABSTOP, X0 + 724 - 16 - 160, 472, 160, 28, ID_PL_NOTICE);
    add(TAB_PLAY, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, X0 + 48, 559, 236, 20, ID_PLAY_STATUS);
    add(-1, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 16, 82, 300, 22, ID_PLAYTIME);
    add(-1, L"Edit", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, X0 + 16, 206, 410, 26, ID_GAMEDIR);
    add(TAB_PLAY, L"Button", L"Report a problem\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 554, 128, 28, ID_REPORT);
    up_setup();

    /* Settings */
    add(TAB_SETTINGS, L"Button", L"Display", BS_GROUPBOX, X0, 14, 560, 224, 0);
    add(TAB_SETTINGS, L"Static", L"Mode", SS_LEFT, X0 + 16, 44, 120, 20, 0);
    add(TAB_SETTINGS, L"Button", L"Windowed", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, X0 + 150, 42, 96, 22, ID_WINDOWED);
    add(TAB_SETTINGS, L"Button", L"Full screen", BS_AUTORADIOBUTTON, X0 + 252, 42, 100, 22, ID_FULLSCREEN);
    add(TAB_SETTINGS, L"Button", L"Full screen (exclusive)", BS_AUTORADIOBUTTON, X0 + 358, 42, 188, 22, ID_FULLSCREEN_EX);
    add(TAB_SETTINGS, L"Static", L"Window size", SS_LEFT, X0 + 16, 76, 120, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
            X0 + 150, 72, 240, 200, ID_RESOLUTION);
    res_list_build(s_wnd);
    for (i = 0; i < N_RES; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_res[i].label);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 400, 72, 146, 200,
            ID_FRAMERATE);
    for (i = 0; i < N_FRAME_RATES; i++) {
        WCHAR t[32];
        swprintf_s(t, 32, L"%d FPS", k_frame_rates[i]);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)t);
    }
    add(TAB_SETTINGS, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 150, 98, 396, 18, ID_RES_HINT);
    /* (the game's GRAPHICS -> RENDER RESOLUTION: the same keys) */
    add(TAB_SETTINGS, L"Static", L"Render resolution", SS_LEFT, X0 + 16, 126, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 122, 240, 240,
            ID_RENDER_RES);
    for (i = 0; i < N_RENDER; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_render[i].label);
    /* native_upscale (the game's GRAPHICS -> UPSCALING): AMD FSR 1, in place of the render resolution */
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 400, 122, 146, 200,
            ID_UPSCALE);
    upscale_fill(0);  /* (filled again as the settings are read) */
    add(TAB_SETTINGS, L"Button", L"VSync (no tearing)", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 16, 154, 160, 24, ID_VSYNC);
    /* arena_brightness (the game's GRAPHICS -> ARENA BRIGHTNESS): the arenas' lights, 50-200% */
    add(TAB_SETTINGS, L"Static", L"Arena light", SS_LEFT, X0 + 190, 158, 76, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 270, 154, 90, 300,
            ID_ARENA_LIGHT);
    for (i = 50; i <= 200; i += 10) {
        WCHAR t[16];
        swprintf_s(t, 16, L"%d%%", i);  /* (100%: the game's own) */
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)t);
    }
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 400, 154, 150, 120, ID_SHOWFPS);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Overlay: off");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Overlay: FPS (F2)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Overlay: full (F2)");
    add(TAB_SETTINGS, L"Static", L"Anti-aliasing", SS_LEFT, X0 + 16, 186, 100, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 116, 182, 140, 200, ID_MSAA);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"2x (4 samples)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"3x (9 samples)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"4x (16 samples)");
    add(TAB_SETTINGS, L"Static", L"Graphics API", SS_LEFT, X0 + 270, 186, 80, 20, 0);
    add(TAB_SETTINGS, L"Button", L"Prepare graphics in the menus (no first-time stutter)",
        BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 16, 210, 330, 24, ID_PREPARE);
    /* native_texture_quality: big textures from the game's own smaller mipmaps */
    add(TAB_SETTINGS, L"Static", L"Textures", SS_LEFT, X0 + 350, 214, 60, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 410, 210, 136, 200,
            ID_TEXTURES);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"High");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Medium (half size)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Low (quarter size)");
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 350, 182, 196, 200,
            ID_RENDERER);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Automatic (Direct3D 12 recommended)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Vulkan");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Direct3D 11 (older GPUs)");
    add(TAB_SETTINGS, L"Button", L"Input and audio", BS_GROUPBOX, X0, 248, 560, 118, 0);
    add(TAB_SETTINGS, L"Static", L"Controller API", SS_LEFT, X0 + 16, 276, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 272, 300, 200, ID_INPUT);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"XInput (Xbox controllers)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"SDL (other gamepads)");
    add(TAB_SETTINGS, L"Static", L"Audio output", SS_LEFT, X0 + 16, 308, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 304, 300, 200, ID_AUDIO);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"XAudio2 (recommended)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"SDL");
    add(TAB_SETTINGS, L"Button", L"Mute", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 464, 306, 84, 24, ID_MUTE);
    add(TAB_SETTINGS, L"Button", L"High CPU priority (helps on busy or weak PCs)", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 334, 530, 24, ID_PRIORITY);
    /* Launcher: its own settings (launcher.ini) - the update check (up_setup puts it here). Entrance music is on
       the Music page. */
    add(TAB_SETTINGS, L"Button", L"Launcher", BS_GROUPBOX, X0, 374, 560, 68, 0);
    add(TAB_SETTINGS, L"Button", L"Game", BS_GROUPBOX, X0, 450, 560, 78, 0);
    add(TAB_SETTINGS, L"Static", L"Language", SS_LEFT, X0 + 16, 474, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 470, 200, 200,
            ID_LANGUAGE);
    for (i = 0; i < N_LANGUAGES; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_languages[i].label);
    add(TAB_SETTINGS, L"Static", L"Menus and text (commentary stays English). "
                                 L"Also in game: GRAPHICS.",
        SS_LEFT, X0 + 362, 464, 186, 34, 0);
    add(TAB_SETTINGS, L"Button", L"Everything unlocked from the start (DLC stays)",
        BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 16, 500, 340, 22, ID_UNLOCK_ALL);
    /* match_camera (the game's GRAPHICS -> MATCH CAMERA; BACK + R3 / F4 in a match) */
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 380, 498, 166, 200,
            ID_MATCH_CAMERA);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: Normal");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: Far");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: Close");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: High");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: Low");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Camera: Ramp");
    add(TAB_SETTINGS, L"Button", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, X0, 534, 140, 30, ID_DEFAULTS);
    add(TAB_SETTINGS, L"Button", L"Save", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 534, 108, 30, ID_SAVE);
    add(TAB_SETTINGS, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 568, 560, 32, ID_SETTINGS_STATUS);

    /* Online */
    /* the last online match's connection (online_route_show: the game's log) */
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 16, 440, 34, ID_ON_ROUTE);
    add(TAB_ONLINE, L"Button", L"Connection \x25B8", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 16, 108, 26, ID_ON_NAT_CHECK);
    add(TAB_ONLINE, L"Static", L"Community Creations and online matches (Player Match, Royal Rumble) through a "
                               L"Community Creations server - the original servers closed in 2014.",
        SS_LEFT, X0, 54, 560, 36, 0);
    add(TAB_ONLINE, L"Button", L"Play online", BS_AUTOCHECKBOX | WS_TABSTOP, X0, 92, 150, 22, ID_ON_ENABLE);
    /* (the game's online_mod_upload: mod_transfer.cpp - the host sends its gameplay mods to joiners who lack them) */
    add(TAB_ONLINE, L"Button", L"Share my mods with players who join me", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 160, 92,
        290, 22, ID_ON_SHARE);
    add(TAB_ONLINE, L"Button", L"Mod sharing \x25B8", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 90, 108, 26,
        ID_ON_MODS_FOLD);
    add(TAB_ONLINE, L"Button", L"Your account", BS_GROUPBOX, X0, 118, 560, 124, 0);
    /* (the account box's fields and buttons: placed by online_account_show, for its mode) */
    add(TAB_ONLINE, L"Static", L"Name", SS_LEFT, X0 + 16, 144, 130, 20, ID_ON_L_NAME);
    h = add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 150, 140, 220, 24, ID_ON_NAME);
    SendMessageW(h, EM_LIMITTEXT, 15, 0);
    add(TAB_ONLINE, L"Static", L"Password", SS_LEFT, X0 + 16, 174, 130, 20, ID_ON_L_PASS);
    h = add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | WS_TABSTOP, X0 + 150, 170, 220, 24,
            ID_ON_PASSWORD);
    SendMessageW(h, EM_LIMITTEXT, 100, 0);
    add(TAB_ONLINE, L"Static", L"Code", SS_LEFT, X0 + 16, 174, 130, 20, ID_ON_L_CODE);
    h = add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_BORDER | WS_TABSTOP, X0 + 150, 170, 220, 24,
            ID_ON_CODE);
    SendMessageW(h, EM_LIMITTEXT, 20, 0);
    add(TAB_ONLINE, L"Button", L"Sign in", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 139, 162, 26, ID_ON_SIGNIN);
    add(TAB_ONLINE, L"Button", L"Create account\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 169, 162, 26,
        ID_ON_REGISTER);
    add(TAB_ONLINE, L"Button", L"Sign out", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 139, 162, 26, ID_ON_SIGNOUT);
    add(TAB_ONLINE, L"Button", L"", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 139, 162, 26, ID_ON_STEP);
    add(TAB_ONLINE, L"Button", L"Back", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 169, 162, 26, ID_ON_BACK);
    add(TAB_ONLINE, L"Button", L"Forgot password?", BS_PUSHBUTTON | WS_TABSTOP, X0 + 384, 199, 162, 24, ID_ON_FORGOT);
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 16, 200, 530, 18, ID_ON_ACCOUNT);
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 16, 219, 530, 18, ID_ON_INFO);
    add(TAB_ONLINE, L"Button", L"Server", BS_GROUPBOX, X0, 248, 560, 74, 0);
    h = add(TAB_ONLINE, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 16, 270, 120, 200,
            ID_ON_SERVER_KIND);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Default");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Custom");
    SendMessageW(h, CB_SETCURSEL, 0, 0);
    add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 150, 270, 396, 24, ID_ON_SERVER);
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 150, 298, 396, 20, ID_ON_SERVER_LABEL);
    add(TAB_ONLINE, L"Button", L"Friends", BS_GROUPBOX, X0, 328, 560, 202, ID_FR_GROUP);
    {
        static const WCHAR *const names[] = { L"Player", L"Status" };
        static const int widths[] = { 150, 196 };
        list_columns(add(TAB_ONLINE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER |
                                                       WS_TABSTOP, X0 + 16, 350, 352, 124, ID_FR_LIST),
                     names, widths, 2);
    }
    h = add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 380, 350, 166, 24, ID_FR_NAME);
    SendMessageW(h, EM_LIMITTEXT, 15, 0);
    SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)L"Player name");
    add(TAB_ONLINE, L"Button", L"Add", BS_PUSHBUTTON | WS_TABSTOP, X0 + 380, 380, 80, 26, ID_FR_ADD);
    add(TAB_ONLINE, L"Button", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, X0 + 466, 380, 80, 26, ID_FR_REMOVE);
    add(TAB_ONLINE, L"Static", L"Online: in the game's ONLINE menus. Friends' matches show in the game's searches.",
        SS_LEFT | SS_NOPREFIX, X0 + 380, 414, 166, 60, ID_FR_INFO);
    /* Mod sharing (the fold: in the Friends box's place while open) - the game's online mod settings */
    add(TAB_ONLINE, L"Button", L"Your connection", BS_GROUPBOX, X0, 328, 560, 202, ID_ON_NAT_GROUP);
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 16, 352, 530, 142, ID_ON_NAT_TEXT);
    add(TAB_ONLINE, L"Button", L"Mods in online sessions", BS_GROUPBOX, X0, 328, 560, 202, ID_ON_MODS_GROUP);
    add(TAB_ONLINE, L"Static", L"Upload speed (KB/s, all joiners)", SS_LEFT, X0 + 16, 354, 250, 20, ID_ON_MODS_L1);
    h = add(TAB_ONLINE, L"Edit", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, X0 + 280, 350, 90, 24, ID_ON_UP_KBS);
    SendMessageW(h, EM_LIMITTEXT, 5, 0);
    add(TAB_ONLINE, L"Static", L"Joiners downloading at once", SS_LEFT, X0 + 16, 380, 250, 20, ID_ON_MODS_L2);
    h = add(TAB_ONLINE, L"Edit", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, X0 + 280, 376, 90, 24, ID_ON_UP_PEERS);
    SendMessageW(h, EM_LIMITTEXT, 1, 0);
    add(TAB_ONLINE, L"Static", L"Largest mod to download (MB)", SS_LEFT, X0 + 16, 406, 250, 20, ID_ON_MODS_L3);
    h = add(TAB_ONLINE, L"Edit", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, X0 + 280, 402, 90, 24, ID_ON_MOD_MB);
    SendMessageW(h, EM_LIMITTEXT, 4, 0);
    add(TAB_ONLINE, L"Static", L"Most to download for one session (MB)", SS_LEFT, X0 + 16, 432, 260, 20, ID_ON_MODS_L4);
    h = add(TAB_ONLINE, L"Edit", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, X0 + 280, 428, 90, 24, ID_ON_MODS_MB);
    SendMessageW(h, EM_LIMITTEXT, 5, 0);
    add(TAB_ONLINE, L"Static", L"Mods you lack come straight from the host's game (never the server or the relay), into "
                               L"UserData\\OnlineMods - removed when you leave online or close the game.",
        SS_LEFT | SS_NOPREFIX, X0 + 16, 458, 530, 36, ID_ON_MODS_INFO);
    add(TAB_ONLINE, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 16, 478, 530, 18, ID_FR_STATUS);
    add(TAB_ONLINE, L"Static", L"Addresses", SS_LEFT, X0 + 16, 502, 90, 20, 0);
    h = add(TAB_ONLINE, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 110, 498, 436, 24, ID_ON_PEERS);
    SendMessageW(h, EM_LIMITTEXT, 140, 0);
    add(TAB_ONLINE, L"Button", L"Save", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 538, 108, 30, ID_ON_SAVE);
    add(TAB_ONLINE, L"Static", L"Addresses: other players' IPs or names (comma-separated) to search besides your "
                               L"network. Without, players anywhere meet through the server.",
        SS_LEFT | SS_NOPREFIX, X0, 538, 440, 30, ID_ON_STATUS);

    /* Install */
    add(TAB_INSTALL, L"Static", L"1.  Your " GAME_TITLE L" disc image (Xbox 360 ISO or XISO)",
        SS_LEFT, X0, 54, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 78, 424, 24, ID_IMAGE);
    add(TAB_INSTALL, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 77, 92, 26, ID_IMAGE_BROWSE);
    add(TAB_INSTALL, L"Static", L"The disc image is only read, never changed.", SS_LEFT, X0 + 20, 106, 520, 20, 0);
    add(TAB_INSTALL, L"Static", L"2.  Install to", SS_LEFT, X0, 138, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 162, 424, 24, ID_TARGET);
    add(TAB_INSTALL, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 161, 92, 26, ID_TARGET_BROWSE);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 20, 190, 526, 36, ID_FREE);
    add(TAB_INSTALL, L"Button", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 236, 140, 36, ID_INSTALL);
    add(TAB_INSTALL, L"Button", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, X0 + 150, 236, 100, 36, ID_CANCEL);
    add(TAB_INSTALL, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 288, 560, 20, ID_PROGRESS);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 318, 560, 76, ID_INSTALL_STATUS);
    add(TAB_INSTALL, L"Static", L"Files already installed with the right size are skipped, so a stopped "
                                L"installation continues where it left off.", SS_LEFT, X0, 410, 560, 36, 0);
    add(TAB_INSTALL, L"Button", L"Game files", BS_GROUPBOX, X0, 452, 560, 118, 0);
    add(TAB_INSTALL, L"Button", L"Verify game files", BS_PUSHBUTTON | WS_TABSTOP, X0 + 16, 482, 170, 32, ID_VERIFY);
    add(TAB_INSTALL, L"Button", L"Repair", BS_PUSHBUTTON | WS_TABSTOP, X0 + 16, 524, 170, 32, ID_VERIFY_REPAIR);
    ShowWindow(ctl(ID_VERIFY_REPAIR), SW_HIDE);
    add(TAB_INSTALL, L"Static", L"", SS_CENTER | SS_NOPREFIX, X0 + 196, 478, 40, 40, ID_VERIFY_MARK);
    add(TAB_INSTALL, L"Static", L"Checks every game file against the game disc: a damaged one can make the game "
                                L"crash while it loads.", SS_LEFT | SS_NOPREFIX, X0 + 244, 476, 300, 84, ID_VERIFY_STATUS);

    /* Android */
    add(TAB_ANDROID, L"Static", L"Play on an Android phone or tablet (experimental): 64-bit ARM with Vulkan, such as a "
                                L"recent Snapdragon. It needs about 7 GB free and a controller.",
        SS_LEFT, X0, 50, 560, 36, 0);
    add(TAB_ANDROID, L"Button", L"Option 1: install over USB (recommended)", BS_GROUPBOX, X0, 92, 560, 214, 0);
    add(TAB_ANDROID, L"Static",
        L"1.  On the phone, open Settings \x2192 About phone (Samsung: \x2192 Software information) and tap "
        L"Build number 7 times, until it says developer mode is on.\n"
        L"2.  Settings \x2192 Developer options: turn on USB debugging.\n"
        L"3.  Connect the phone to this PC with a USB data cable and unlock it. When it asks \u201cAllow USB "
        L"debugging?\u201d, tick Always allow from this computer and tap Allow.\n"
        L"4.  Click Install to phone. The first time, the launcher downloads Google's adb tool (about 7 MB).",
        SS_LEFT | SS_NOPREFIX, X0 + 16, 114, 528, 112, 0);
    add(TAB_ANDROID, L"Button", L"Install to phone", BS_DEFPUSHBUTTON | WS_TABSTOP, X0 + 16, 232, 160, 34,
        ID_ADB_INSTALL);
    add(TAB_ANDROID, L"Button", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, X0 + 186, 232, 100, 34,
        ID_ANDROID_CANCEL);
    add(TAB_ANDROID, L"Static", L"It copies the app and the game to the phone's games folder (next time only what "
                                L"changed; the phone's own saves are kept) and starts it.",
        SS_LEFT, X0 + 16, 270, 528, 32, 0);
    add(TAB_ANDROID, L"Button", L"Option 2: a package to copy yourself", BS_GROUPBOX, X0, 316, 560, 104, 0);
    add(TAB_ANDROID, L"Static", L"A folder with the app (SvR2011.apk), the game (SvR2011-Game.zip) and instructions. "
                                L"Copy both files to the phone's Download folder and install the app: it installs "
                                L"the game on its first start.",
        SS_LEFT, X0 + 16, 338, 528, 40, 0);
    add(TAB_ANDROID, L"Button", L"Create APK Package\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 16, 380, 170, 30,
        ID_APK_CREATE);
    add(TAB_ANDROID, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 434, 560, 20, ID_APK_PROGRESS);
    SendMessageW(ctl(ID_APK_PROGRESS), PBM_SETRANGE32, 0, 1000);
    add(TAB_ANDROID, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, X0, 462, 560, 56, ID_APK_STATUS);

    /* DLC */
    add(TAB_DLC, L"Static", L"Downloadable content (DLC) and title updates as downloaded on the Xbox 360: "
                            L"the packages themselves, or .zip / .rar / .7z archives of them.",
        SS_LEFT, X0, 50, 560, 36, 0);
    add(TAB_DLC, L"Static", L"Folder with the DLC", SS_LEFT, X0, 96, 560, 20, 0);
    add(TAB_DLC, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 120, 424, 24, ID_DLC_DIR);
    add(TAB_DLC, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 119, 92, 26, ID_DLC_BROWSE);
    add(TAB_DLC, L"Button", L"Install DLC", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 160, 140, 36, ID_DLC_INSTALL);
    add(TAB_DLC, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 208, 560, 56, ID_DLC_STATUS);
    add(TAB_DLC, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 270, 560, 110, ID_DLC_LIST);
    add(TAB_DLC, L"Static", L"The packages are copied into the game folder's DLC folder and unpacked the next "
                            L"time the game starts. Title updates are not needed and are skipped.",
        SS_LEFT, X0, 396, 560, 40, 0);

    /* Saves */
    add(TAB_SAVES, L"Static", L"Your saves, one file each in the saves folder (below). The main save keeps "
                              L"settings, unlocks and progress and ties the rest together.",
        SS_LEFT, X0, 50, 560, 36, 0);
    saves_setup(add(TAB_SAVES, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                    X0, 92, 560, 176, ID_SV_LIST));
    add(TAB_SAVES, L"Button", L"Back up all", BS_PUSHBUTTON | WS_TABSTOP, X0, 276, 130, 30, ID_SV_BACKUP);
    add(TAB_SAVES, L"Button", L"Restore backup\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 140, 276, 140, 30, ID_SV_RESTORE);
    add(TAB_SAVES, L"Button", L"Backups folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 290, 276, 130, 30, ID_SV_BACKUPS);
    add(TAB_SAVES, L"Button", L"Open save folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 276, 130, 30, ID_SV_OPEN);
    add(TAB_SAVES, L"Button", L"Export selected\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0, 312, 130, 30, ID_SV_EXPORT);
    add(TAB_SAVES, L"Button", L"Import\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 140, 312, 140, 30, ID_SV_IMPORT);
    add(TAB_SAVES, L"Button", L"Import Superstar\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 290, 312, 130, 30,
        ID_SV_IMPORTCAW);
    add(TAB_SAVES, L"Button", L"Delete selected", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 312, 130, 30, ID_SV_DELETE);
    add(TAB_SAVES, L"Button", L"Clean save of unused mods\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0, 350, 200, 30,
        ID_SV_CLEAN);
    add(TAB_SAVES, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, X0 + 210, 350, 350, 36, ID_SV_STATUS);
    add(TAB_SAVES, L"Static", L"Save preset:", SS_LEFT, X0, 400, 84, 20, 0);
    add(TAB_SAVES, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 88, 396, 212, 240, ID_SV_PRESET);
    add(TAB_SAVES, L"Button", L"New\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 310, 394, 110, 30, ID_SV_PRESET_NEW);
    add(TAB_SAVES, L"Button", L"Import Xbox 360 save\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 394, 130, 30,
        ID_SV_IMPORT360);
    add(TAB_SAVES, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, X0, 442, 380, 20, ID_SV_FOLDER);
    add(TAB_SAVES, L"Button", L"Change\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 390, 436, 80, 30, ID_SV_CHANGE);
    add(TAB_SAVES, L"Button", L"Default", BS_PUSHBUTTON | WS_TABSTOP, X0 + 480, 436, 80, 30, ID_SV_DEFAULT);
    add(TAB_SAVES, L"Static", L"A save preset is a whole set of saves of its own (the game folder's Save Presets): "
                              L"switch between them here, or bring your Xbox 360 save over into one. The game and "
                              L"this tab use the folder set here (saved in " GAME_TOML L").",
        SS_LEFT, X0, 472, 560, 52, 0);
    /* per_mod_saves (experimental): the game's own key, written at once (the game reads it at its next start) */
    add(TAB_SAVES, L"Button", L"Experimental: each superstar mod's save data in its own file (Saves\\.mods)",
        BS_AUTOCHECKBOX | WS_TABSTOP, X0, 528, 560, 22, ID_SV_PERMOD);
    /* big_roster (experimental): the same; turning it on asks first (big_roster_confirm) */
    {
        HWND c = add(TAB_SAVES, L"Button", L"\xD83E\xDDEA Experimental: more superstar mods at once (big roster)",
                     BS_AUTOCHECKBOX | WS_TABSTOP, X0, 554, 560, 22, ID_SV_BIGROSTER);
        HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s_wnd, NULL, s_inst, NULL);
        if (c && tt) {
            TTTOOLINFOW ti;
            ZeroMemory(&ti, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = s_wnd;
            ti.uId = (UINT_PTR)c;
            ti.lpszText = (WCHAR *)BIG_ROSTER_WARNING;
            SendMessageW(tt, TTM_SETMAXTIPWIDTH, 0, S(420));
            SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }

    /* Paint Tool */
    add(TAB_PAINT, L"Static", L"The Paint Tool's logos: 10 pages of 20 (CREATE A SUPERSTAR > PAINT TOOL in the game; "
                              L"move past the grid's edge to change pages there). Export one as a PNG, or import any "
                              L"image into a logo; "
                              L"it is fitted into 256 \x00D7 256.",
        SS_LEFT, X0, 50, 560, 32, 0);
    pt_setup_grid();
    add(TAB_PAINT, L"Button", L"Export PNG\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 86, 130, 30, ID_PT_EXPORT);
    add(TAB_PAINT, L"Button", L"Import image\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 124, 130, 30, ID_PT_IMPORT);
    add(TAB_PAINT, L"Button", L"Delete logo", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 162, 130, 30, ID_PT_DELETE);
    add(TAB_PAINT, L"Button", L"Export all\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 220, 130, 30, ID_PT_EXPORTALL);
    add(TAB_PAINT, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 258, 130, 30, ID_PT_REFRESH);
    add(TAB_PAINT, L"Button", L"\x25C0 Page", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 316, 64, 30, ID_PT_PREV);
    add(TAB_PAINT, L"Button", L"Page \x25B6", BS_PUSHBUTTON | WS_TABSTOP, X0 + 496, 316, 64, 30, ID_PT_NEXT);
    add(TAB_PAINT, L"Static", L"Page 1 of 10", SS_CENTER | SS_NOPREFIX, X0 + 430, 352, 130, 22, ID_PT_PAGE);
    add(TAB_PAINT, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 424, 560, 48, ID_PT_STATUS);

    /* Movies */
    mv_setup();

    /* Mods (mods_tab.c) */
    mods_build(s_wnd, add, TAB_MODS, ID_MODS_BASE, s_title);
    for (i = 0; i < s_nctl[TAB_MODS]; i++) {  /* (its heading: the page shows the title) */
        WCHAR t[16], c[16];
        GetClassNameW(s_ctl[TAB_MODS][i], c, 16);
        GetWindowTextW(s_ctl[TAB_MODS][i], t, 16);
        if (!_wcsicmp(c, L"Static") && !wcscmp(t, L"Mods")) {
            DestroyWindow(s_ctl[TAB_MODS][i]);
            memmove(&s_ctl[TAB_MODS][i], &s_ctl[TAB_MODS][i + 1], (size_t)(s_nctl[TAB_MODS] - i - 1) * sizeof(HWND));
            s_nctl[TAB_MODS]--;
            break;
        }
    }
    /* Music (music_tab.c) */
    music_build(s_wnd, add, TAB_MUSIC, ID_MUSIC_BASE);
    /* Roster: the list menus' sort categories (roster_tab.c) */
    roster_build(s_wnd, add, TAB_ROSTER, ID_ROSTER_BASE);
    /* Texture packs (texpacks_tab.c) */
    texpacks_build(s_wnd, add, TAB_TEXPACKS, ID_TEXPACKS_BASE, toml_get, toml_set);
    discover_build(s_wnd, add, TAB_DISCOVER, ID_DISCOVER_BASE, discover_server, discover_account);
    receipts_set_guard(test_run_refuses_url);   /* (a test run: never a server off this PC) */
    reports_build(s_wnd, add, TAB_REPORTS, ID_REPORTS_BASE, discover_server, reports_token);
    suggestions_build(s_wnd, add, TAB_SUGGESTIONS, ID_SUGGEST_BASE, discover_server, reports_token, test_run_refuses_url,
                      PORT_VERSION);
    discover_set_ini(s_launcher_ini);
    /* Setup: what's needed, found or missing; the Mod Maker's tools (setup_tab.c) */
    setup_build(s_wnd, add, TAB_SETUP, TAB_TOOLS, ID_SETUP_BASE, setup_go_install);

    CheckDlgButton(s_wnd, ID_CLOSE_ON_PLAY,
                   GetPrivateProfileIntW(L"Launcher", L"CloseOnPlay", 0, s_launcher_ini) ? BST_CHECKED : BST_UNCHECKED);
    {
        WCHAR img[MAX_PATH];
        GetPrivateProfileStringW(L"Launcher", L"LastImage", L"", img, MAX_PATH, s_launcher_ini);
        set_text(ID_IMAGE, img);
    }
    set_text(ID_TARGET, s_launcher_dir);
    {
        /* DLC folder: the last one used, else "UpdatesAndDLC" beside the game folder. */
        WCHAR d[MAX_PATH], parent[MAX_PATH], *slash;
        GetPrivateProfileStringW(L"Launcher", L"LastDlcFolder", L"", d, MAX_PATH, s_launcher_ini);
        if (!d[0] && s_game_dir[0]) {
            wcscpy_s(parent, MAX_PATH, s_game_dir);
            slash = wcsrchr(parent, L'\\');
            if (slash && slash[1] == 0) {                 /* trailing backslash */
                *slash = 0;
                slash = wcsrchr(parent, L'\\');
            }
            if (slash) {
                *slash = 0;
                if (join(d, parent, L"UpdatesAndDLC") && !dir_exists(d))
                    d[0] = 0;
            }
        }
        set_text(ID_DLC_DIR, d);
    }
    settings_load();
}

/* ── DLC ──────────────────────────────────────────────────────────────── */

/* The DLC tab copies the game's Xbox 360 content packages (STFS: the files
 * as downloaded, or inside .zip / .rar / .7z archives, unpacked with Windows'
 * tar) into <game>\DLC; the game unpacks new ones there at startup (dlc.h). */

#define GAME_TITLE_ID 0x5451085Du

static struct {
    WCHAR src[MAX_PATH], game[MAX_PATH];
    int copied, present, updates, failed;
    WCHAR names[1200];
} s_dlc;
static HANDLE s_dlc_worker;

static void dlc_post(int done, const WCHAR *fmt, ...)
{
    WCHAR buf[1600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1600, fmt, ap);
    va_end(ap);
    if (s_console) {
        con_print(buf);
        if (done && s_dlc.names[0])
            con_print(s_dlc.names);
        con_print(L"\n");
        return;
    }
    PostMessageW(s_wnd, WM_APP_DLC, done, (LPARAM)wdup(buf));
}

/* 1: DLC for this game, 2: a title update for it, 0: anything else. */
static int stfs_kind(const WCHAR *path, WCHAR *name, size_t name_len)
{
    unsigned char h[0x491];
    DWORD got = 0;
    uint32_t type;
    size_t i;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(f, h, sizeof h, &got, NULL);
    CloseHandle(f);
    if (got < sizeof h || (memcmp(h, "LIVE", 4) && memcmp(h, "PIRS", 4) && memcmp(h, "CON ", 4)))
        return 0;
    if (be32(h + 0x360) != GAME_TITLE_ID)
        return 0;
    for (i = 0; i + 1 < name_len && i < 0x40; i++) {      /* display name, UTF-16BE */
        name[i] = (WCHAR)(h[0x411 + 2 * i] << 8 | h[0x412 + 2 * i]);
        if (!name[i])
            break;
    }
    name[i] = 0;
    type = be32(h + 0x344);
    return type == 0x00000002 ? 1 : type == 0x000B0000 ? 2 : 0;
}

static int is_archive(const WCHAR *name)
{
    const WCHAR *ext = wcsrchr(name, L'.');
    return ext && (!_wcsicmp(ext, L".zip") || !_wcsicmp(ext, L".rar") || !_wcsicmp(ext, L".7z"));
}

/* Unpacks an archive: a zip itself (unzip.c: Wine / Proton has no tar.exe),
 * anything else - or a zip it can't read - with Windows' tar (bsdtar reads
 * zip, rar and 7z). */
static int unpack(const WCHAR *archive, const WCHAR *to)
{
    WCHAR cmd[MAX_PATH * 3], sys[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    const WCHAR *ext = wcsrchr(archive, L'.');
    if (ext && !_wcsicmp(ext, L".zip") && unzip_file(archive, to))
        return 1;
    if (GetEnvironmentVariableW(L"SVR2011_NO_TAR", sys, MAX_PATH))
        return 0;  /* (tests: as on Wine) */
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf_s(cmd, MAX_PATH * 3, L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", sys, archive, to);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

static void remove_tree(const WCHAR *dir)
{
    mods_delete_tree(dir);  /* (directly: the shell's delete can take 30 s a folder - mods_tab.h) */
}

static void dlc_scan(const WCHAR *dir, int depth);

static void dlc_file(const WCHAR *path, const WCHAR *file_name)
{
    WCHAR name[80], dst[MAX_PATH], dlc_dir[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    int kind;
    if (is_archive(file_name)) {
        WCHAR base[MAX_PATH], tmp[MAX_PATH], sub[64];
        GetTempPathW(MAX_PATH, base);
        swprintf_s(sub, 64, L"svr2011_dlc_%lu", GetTickCount());
        if (!join(tmp, base, sub) || !mkdirs(tmp))
            return;
        dlc_post(0, L"Unpacking %s\x2026", file_name);
        if (unpack(path, tmp)) {
            dlc_scan(tmp, 0);
        } else {
            WCHAR sys[MAX_PATH], tar[MAX_PATH];
            s_dlc.failed++;
            /* Wine / Proton: no tar.exe for rar and 7z (zips unpack without it). */
            GetSystemDirectoryW(sys, MAX_PATH);
            if (join(tar, sys, L"tar.exe") && !file_exists(tar))
                dlc_post(0, L"%s: rar and 7z archives can't be unpacked here (no tar.exe, as under "
                            L"Linux / Proton). Unpack it yourself, or use a .zip.", file_name);
        }
        remove_tree(tmp);
        return;
    }
    kind = stfs_kind(path, name, 80);
    if (kind == 2) {
        s_dlc.updates++;
        return;
    }
    if (kind != 1)
        return;
    join(dlc_dir, s_dlc.game, L"DLC");
    mkdirs(dlc_dir);
    if (!join(dst, dlc_dir, file_name))
        return;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) &&
        GetFileAttributesExW(dst, GetFileExInfoStandard, &b) &&
        a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh) {
        s_dlc.present++;
        return;
    }
    dlc_post(0, L"Copying %s\x2026", name[0] ? name : file_name);
    if (CopyFileW(path, dst, FALSE)) {
        s_dlc.copied++;
        if (wcslen(s_dlc.names) + wcslen(name) + 4 < 1200) {
            wcscat_s(s_dlc.names, 1200, L"\n  ");
            wcscat_s(s_dlc.names, 1200, name[0] ? name : file_name);
        }
    } else {
        s_dlc.failed++;
    }
}

static void dlc_scan(const WCHAR *dir, int depth)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 8 || !join(pattern, dir, L"*"))
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (!join(path, dir, fd.cFileName))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dlc_scan(path, depth + 1);
        else
            dlc_file(path, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI dlc_thread(LPVOID unused)
{
    (void)unused;
    dlc_scan(s_dlc.src, 0);
    if (!s_dlc.copied && !s_dlc.present) {
        dlc_post(1, L"No DLC for " GAME_TITLE L" was found in %s.%s", s_dlc.src,
                 s_dlc.failed ? L" (An archive could not be unpacked.)" : L"");
    } else {
        dlc_post(1, L"%d DLC package%s installed%s%s%s. %s%s", s_dlc.copied, s_dlc.copied == 1 ? L"" : L"s",
                 s_dlc.present ? L", " : L"",
                 s_dlc.present ? L"others already there" : L"",
                 s_dlc.updates ? L" (title update skipped: not needed)" : L"",
                 s_dlc.failed ? L"Some files could not be copied. " : L"",
                 s_dlc.copied ? L"They are set up the next time the game starts:" : L"");
    }
    return 0;
}

static void start_dlc(void)
{
    WCHAR t[MAX_PATH + 100];
    if (s_dlc_worker && WaitForSingleObject(s_dlc_worker, 0) == WAIT_TIMEOUT)
        return;
    GetWindowTextW(ctl(ID_DLC_DIR), s_dlc.src, MAX_PATH);
    if (!s_dlc.src[0] || !dir_exists(s_dlc.src)) {
        set_text(ID_DLC_STATUS, L"Choose the folder with the DLC first.");
        return;
    }
    if (!s_game_dir[0] || !has_program(s_game_dir)) {
        set_text(ID_DLC_STATUS, L"Install the game first (Install tab).");
        return;
    }
    wcscpy_s(s_dlc.game, MAX_PATH, s_game_dir);
    s_dlc.copied = s_dlc.present = s_dlc.updates = s_dlc.failed = 0;
    s_dlc.names[0] = 0;
    WritePrivateProfileStringW(L"Launcher", L"LastDlcFolder", s_dlc.src, s_launcher_ini);
    EnableWindow(ctl(ID_DLC_INSTALL), FALSE);
    EnableWindow(ctl(ID_DLC_BROWSE), FALSE);
    swprintf_s(t, MAX_PATH + 100, L"Looking for DLC in %s\x2026", s_dlc.src);
    set_text(ID_DLC_STATUS, t);
    set_text(ID_DLC_LIST, L"");
    if (s_dlc_worker)
        CloseHandle(s_dlc_worker);
    s_dlc_worker = CreateThread(NULL, 0, dlc_thread, NULL, 0, NULL);
    if (!s_dlc_worker) {
        EnableWindow(ctl(ID_DLC_INSTALL), TRUE);
        EnableWindow(ctl(ID_DLC_BROWSE), TRUE);
        set_text(ID_DLC_STATUS, L"The DLC installation could not be started.");
    }
}

/* ── Saves ────────────────────────────────────────────────────────────── */

/* The game keeps each save as one plain file in <game>\Saves: SaveData.dat
 * is the main save (settings, unlocks, progress) that ties the rest
 * together; created superstars (.cas), Paint Tool logos (.pt), replays (.rec),
 * highlight reels (.scn) and other created content are files of their own.
 * The hidden Saves\.info holds their Xbox 360 content headers (optional).
 * Earlier builds kept them in UserData\<profile>\5451085D\00000001\<save>\
 * SaveData.Dat: those are moved to Saves the first time the tab or the game
 * looks. Backups go to <game>\SaveBackups\<date time>. */

#define SAVES_DIR      L"Saves"

static int any_game_running(void)
{
    PROCESSENTRY32W pe;
    HANDLE snap;
    int found = 0;
    if (game_running())
        return 1;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe))
        do {
            if (!_wcsicmp(pe.szExeFile, GAME_EXE))
                found = 1;
        } while (!found && Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}


/* The saves folder setting (svr2011.toml's saves_folder, which the game reads
 * too): "" for the default, <game>\Saves. Kept with '/' (a TOML string takes
 * '\' as an escape); relative paths are from the game folder. */
static void saves_folder_setting(WCHAR *out, size_t n)
{
    WCHAR p[MAX_PATH];
    Lines l;
    int i;
    out[0] = 0;
    if (!s_game_dir[0])
        return;
    settings_path(p);
    if (!toml_read(p, &l))
        return;
    for (i = 0; i < l.n; i++) {
        char key[64], val[MAX_PATH * 3];
        const char *t = l.v[i];
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[')
            break;
        if (toml_kv(l.v[i], key, sizeof key, val, sizeof val) && !strcmp(key, "saves_folder")) {
            WCHAR *c;
            MultiByteToWideChar(CP_UTF8, 0, val, -1, out, (int)n);
            for (c = out; *c; c++)
                if (*c == L'/')
                    *c = L'\\';
        }
    }
    lines_free(&l);
}

/* A top-level key of svr2011.toml, quotes removed (the Texture packs tab);
 * 0 when absent. */
static int toml_get(const char *key, char *out, size_t n)
{
    WCHAR p[MAX_PATH];
    Lines l;
    int i, found = 0;
    settings_path(p);
    if (!toml_read(p, &l))
        return 0;
    for (i = 0; i < l.n && !found; i++) {
        char k[64], v[4096];
        const char *t = l.v[i];
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[')
            break;
        if (toml_kv(l.v[i], k, sizeof k, v, sizeof v) && !strcmp(k, key)) {
            strcpy_s(out, n, v);
            found = 1;
        }
    }
    lines_free(&l);
    return found;
}

/* Sets one top-level key of svr2011.toml (value as written, e.g. "\"x\"");
 * the rest of the file is kept. */
static int toml_set(const char *key, const char *value)
{
    WCHAR p[MAX_PATH], tmp[MAX_PATH];
    Lines l;
    FILE *f;
    char line[MAX_PATH * 3 + 64];
    int i, done = 0, insert_at = -1;
    settings_path(p);
    if (!toml_read(p, &l))
        lines_insert(&l, 0, "# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)");
    sprintf_s(line, sizeof line, "%s = %s", key, value);
    for (i = 0; i < l.n; i++) {
        char k[64], v[MAX_PATH * 3];
        const char *t = l.v[i];
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[') {
            insert_at = i;
            break;
        }
        if (toml_kv(l.v[i], k, sizeof k, v, sizeof v) && !strcmp(k, key)) {
            free(l.v[i]);
            l.v[i] = done ? NULL : _strdup(line);
            done = 1;
        }
    }
    if (!done) {
        if (insert_at < 0)
            insert_at = l.n;
        while (insert_at > 0 && (!l.v[insert_at - 1] || !l.v[insert_at - 1][0]))
            insert_at--;
        lines_insert(&l, insert_at, line);
    }
    swprintf_s(tmp, MAX_PATH, L"%s.tmp", p);
    if (_wfopen_s(&f, tmp, L"wb") || !f) {
        lines_free(&l);
        return 0;
    }
    for (i = 0; i < l.n; i++)
        if (l.v[i])
            fprintf(f, "%s\n", l.v[i]);
    fclose(f);
    lines_free(&l);
    if (!MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

static int saves_dir(WCHAR *out)
{
    WCHAR s[MAX_PATH];
    if (!s_game_dir[0])
        return 0;
    saves_folder_setting(s, MAX_PATH);
    if (!s[0])
        return join(out, s_game_dir, SAVES_DIR);
    if (s[1] == L':' || (s[0] == L'\\' && s[1] == L'\\')) {  /* absolute */
        wcscpy_s(out, MAX_PATH, s);
        return 1;
    }
    return join(out, s_game_dir, s);
}

/* A shell copy / delete (whole folders), without dialogs. */
static int shell_op(UINT func, const WCHAR *from, const WCHAR *to, FILEOP_FLAGS extra)
{
    WCHAR f[MAX_PATH + 2], t[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    wcscpy_s(f, MAX_PATH, from);
    f[wcslen(f) + 1] = 0;                             /* double-terminated */
    if (to) {
        wcscpy_s(t, MAX_PATH, to);
        t[wcslen(t) + 1] = 0;
    }
    ZeroMemory(&op, sizeof op);
    op.hwnd = s_wnd;
    op.wFunc = func;
    op.pFrom = f;
    op.pTo = to ? t : NULL;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT | extra;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static int is_save_entry(const WIN32_FIND_DATAW *fd)
{
    return fd->cFileName[0] != L'.' && _wcsicmp(fd->cFileName, L"desktop.ini") != 0
        && !(fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

/* Moves one old-style package folder into Saves (as the game does). */
static void saves_migrate_package(const WCHAR *pkg, const WCHAR *name, const WCHAR *hdr_dir, const WCHAR *saves)
{
    WCHAR pat[MAX_PATH], file[MAX_PATH], only[MAX_PATH], dest[MAX_PATH], info[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int files = 0, dirs = 0;
    if (!join(dest, saves, name) || file_exists(dest) || dir_exists(dest) || !join(info, saves, L".info"))
        return;
    only[0] = 0;
    if (!join(pat, pkg, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dirs++;
        else if (!_wcsicmp(fd.cFileName, L"__thumbnail.png")) {
            swprintf_s(b, MAX_PATH, L"%s\\%s.png", info, name);
            if (join(a, pkg, fd.cFileName))
                MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
        } else {
            files++;
            wcscpy_s(only, MAX_PATH, fd.cFileName);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (files == 1 && !dirs) {
        if (!join(file, pkg, only) || !MoveFileExW(file, dest, 0))
            return;
        if (_wcsicmp(only, L"SaveData.Dat")) {       /* the file's own name, if not the usual */
            FILE *t;
            swprintf_s(a, MAX_PATH, L"%s\\%s.file", info, name);
            if (!_wfopen_s(&t, a, L"wb") && t) {
                char n8[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, only, -1, n8, sizeof n8, NULL, NULL);
                fwrite(n8, 1, strlen(n8), t);
                fclose(t);
            }
        }
        RemoveDirectoryW(pkg);
    } else if (!MoveFileExW(pkg, dest, 0))
        return;
    swprintf_s(a, MAX_PATH, L"%s\\%s.header", hdr_dir, name);
    swprintf_s(b, MAX_PATH, L"%s\\%s.header", info, name);
    MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}

static void saves_migrate(void)
{
    WCHAR ud[MAX_PATH], pat[MAX_PATH], saves[MAX_PATH], info[MAX_PATH], prof[MAX_PATH], pkgs[MAX_PATH],
        hdrs[MAX_PATH], t[MAX_PATH], pkg[MAX_PATH], pat2[MAX_PATH];
    WIN32_FIND_DATAW fd, fd2;
    HANDLE f, f2;
    if (!saves_dir(saves) || !join(ud, s_game_dir, L"UserData") || !join(pat, ud, L"*") || !join(info, saves, L".info"))
        return;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 16
                || !wcscmp(fd.cFileName, L"0000000000000000"))
            continue;
        swprintf_s(t, MAX_PATH, L"%08X", TITLE_ID);
        if (!join(prof, ud, fd.cFileName) || !join(pkgs, prof, t) || !join(hdrs, pkgs, L"Headers")
                || !join(t, pkgs, L"00000001") || !join(pat2, t, L"*"))
            continue;
        wcscpy_s(pkgs, MAX_PATH, t);
        join(t, hdrs, L"00000001");
        wcscpy_s(hdrs, MAX_PATH, t);
        f2 = FindFirstFileW(pat2, &fd2);
        if (f2 == INVALID_HANDLE_VALUE)
            continue;
        mkdirs(info);
        do {
            if ((fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd2.cFileName[0] != L'.'
                    && join(pkg, pkgs, fd2.cFileName))
                saves_migrate_package(pkg, fd2.cFileName, hdrs, saves);
        } while (FindNextFileW(f2, &fd2));
        FindClose(f2);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static const WCHAR *save_kind(const WCHAR *name)
{
    const WCHAR *ext = wcsrchr(name, L'.');
    if (!_wcsicmp(name, L"SaveData.dat"))
        return L"Main save (ties the rest together)";
    if (ext && !_wcsicmp(ext, L".cas"))
        return L"Created Superstars";
    if (ext && !_wcsicmp(ext, L".pt"))
        return L"Paint Tool logos";
    if (ext && !_wcsicmp(ext, L".rec"))
        return L"Replays";
    if (ext && !_wcsicmp(ext, L".scn"))
        return L"Highlight reels";
    return L"Created content";
}

/* Total size and newest change of a save (a file, or a folder of files). */
static void entry_info(const WCHAR *path, const WIN32_FIND_DATAW *self, uint64_t *bytes, FILETIME *newest)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    if (!(self->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *bytes += ((uint64_t)self->nFileSizeHigh << 32) | self->nFileSizeLow;
        if (CompareFileTime(&self->ftLastWriteTime, newest) > 0)
            *newest = self->ftLastWriteTime;
        return;
    }
    if (!join(pat, path, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (join(sub, path, fd.cFileName))
            entry_info(sub, &fd, bytes, newest);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void saves_status(const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1024, fmt, ap);
    va_end(ap);
    set_text(ID_SV_STATUS, buf);
}

static void list_columns(HWND list, const WCHAR *const *names, const int *widths, int n)
{
    LVCOLUMNW c;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < n; i++) {
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = S(widths[i]);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(list, i, &c);
    }
}

static void saves_setup(HWND list)
{
    static const WCHAR *const names[] = { L"Save file", L"Contents", L"Size", L"Changed" };
    static const int widths[] = { 190, 180, 66, 116 };
    list_columns(list, names, widths, 4);
}

static void fmt_when(WCHAR *out, size_t n, FILETIME ft)
{
    SYSTEMTIME st;
    FILETIME local;
    out[0] = 0;
    if ((ft.dwLowDateTime || ft.dwHighDateTime) && FileTimeToLocalFileTime(&ft, &local)
            && FileTimeToSystemTime(&local, &st))
        swprintf_s(out, n, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void saves_refresh(void)
{
    saves_folder_show();
    HWND list = ctl(ID_SV_LIST);
    WCHAR saves[MAX_PATH], pat[MAX_PATH], path[MAX_PATH], size[32], when[32];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0;
    uint64_t total = 0;
    ListView_DeleteAllItems(list);
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (!any_game_running())
        saves_migrate();
    if (!saves_dir(saves) || !join(pat, saves, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) {
        saves_status(L"No saves yet. The game makes them the first time it saves.");
        return;
    }
    do {
        LVITEMW it;
        uint64_t bytes = 0;
        FILETIME newest = { 0, 0 };
        if (!is_save_entry(&fd) || !join(path, saves, fd.cFileName))
            continue;
        entry_info(path, &fd, &bytes, &newest);
        total += bytes;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = !_wcsicmp(fd.cFileName, L"SaveData.dat") ? 0 : n;   /* the main save first */
        it.pszText = fd.cFileName;
        it.iItem = ListView_InsertItem(list, &it);
        ListView_SetItemText(list, it.iItem, 1, (WCHAR *)save_kind(fd.cFileName));
        fmt_size(size, 32, bytes);
        ListView_SetItemText(list, it.iItem, 2, size);
        fmt_when(when, 32, newest);
        ListView_SetItemText(list, it.iItem, 3, when);
        n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    fmt_size(size, 32, total);
    {   /* the game couldn't write its save last time (UserData\save_write_failed.txt, universe_welcome.cpp) */
        WCHAR ud[MAX_PATH], failed[MAX_PATH];
        if (join(ud, s_game_dir, L"UserData") && join(failed, ud, L"save_write_failed.txt") &&
            GetFileAttributesW(failed) != INVALID_FILE_ATTRIBUTES) {
            saves_status(L"WARNING: the game couldn't write its save last time it ran. Check that %s isn't read-only, "
                         L"blocked by Windows Controlled folder access, or kept online-only by OneDrive.", saves);
            return;
        }
    }
    if (n)
        saves_status(L"%d save file%s, %s, in %s.", n, n == 1 ? L"" : L"s", size, saves);
    else
        saves_status(L"No saves yet. The game makes them the first time it saves.");
}

/* Refuses (with a message) while the game runs: it keeps its saves open. */
static int saves_locked(void)
{
    if (!any_game_running())
        return 0;
    MessageBoxW(s_wnd, L"Close the game first: it keeps its save files open while it runs.", WINDOW_TITLE,
                MB_OK | MB_ICONINFORMATION);
    return 1;
}

/* Copies the Saves folder to <game>\SaveBackups\<date time><suffix>. */
static int saves_backup_to(const WCHAR *suffix, WCHAR *out)
{
    WCHAR saves[MAX_PATH], base[MAX_PATH], name[96], mnt[MAX_PATH];
    SYSTEMTIME st;
    if (!saves_dir(saves) || !dir_exists(saves))
        return 0;
    GetLocalTime(&st);
    swprintf_s(name, 96, L"%04u-%02u-%02u %02u-%02u-%02u%s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, suffix);
    if (!join(base, s_game_dir, L"SaveBackups") || !mkdirs(base) || !join(out, base, name) || dir_exists(out))
        return 0;
    if (!shell_op(FO_COPY, saves, out, 0))
        return 0;
    if (join(mnt, out, L".mount") && dir_exists(mnt))  /* the game's scratch, not saves */
        shell_op(FO_DELETE, mnt, NULL, 0);
    return 1;
}

static void saves_backup(void)
{
    WCHAR out[MAX_PATH];
    if (saves_locked())
        return;
    if (saves_backup_to(L"", out))
        saves_status(L"Backed up to %s.", out);
    else
        saves_status(L"Nothing was backed up (no saves yet, or the copy failed).");
}

/* A folder picker opening in `start`. */
static int pick_folder_in(const WCHAR *title, const WCHAR *start, WCHAR *out)
{
    IFileOpenDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, title);
    if (start && dir_exists(start) && SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&it))) {
        d->lpVtbl->SetFolder(d, it);
        it->lpVtbl->Release(it);
    }
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* Empties the Saves folder (its files, folders and .info; into the Recycle Bin). */
static void saves_clear(const WCHAR *saves)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    if (!join(pat, saves, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, L".mount"))
            continue;
        if (join(p, saves, fd.cFileName))
            shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

/* "Clean save of unused mods...": what the save keeps for superstar mods that aren't installed or are turned off
 * (save_cleaner.c), shown, then - after a backup of the whole Saves folder - taken out. */
static void saves_clean(void)
{
    static ScItem items[SC_MAX];
    static WCHAR t[16384];
    WCHAR saves[MAX_PATH], mods[MAX_PATH], err[600] = L"", before[MAX_PATH];
    int n, i, gone = 0, off = 0, with_off = 0, at = 0, shown = 0;
    if (saves_locked() || !saves_dir(saves) || !join(mods, s_game_dir, L"Mods\\Superstars"))
        return;
    if ((n = save_clean_scan(saves, mods, items, err, 600)) < 0) {
        MessageBoxW(s_wnd, err, WINDOW_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    for (i = 0; i < n; i++)
        items[i].off ? off++ : gone++;
    if (!n) {
        MessageBoxW(s_wnd, L"The save has nothing of mods that aren't installed or are turned off.", WINDOW_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (off) {
        at = swprintf_s(t, 16384, L"%d of these mods are installed but turned off:\n", off);
        for (i = 0; i < n && at < 15000; i++)
            if (items[i].off && shown++ < 25)
                at += swprintf_s(t + at, 16384 - at, L"  %s\n", items[i].name);
        if (shown > 25)
            at += swprintf_s(t + at, 16384 - at, L"  ... and %d more\n", shown - 25);
        swprintf_s(t + at, 16384 - at, L"\nTake their data out of the save too? (No: only the mods that aren't "
                                       L"installed.)");
        switch (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION)) {
        case IDYES: with_off = 1; break;
        case IDNO: break;
        default: return;
        }
        if (!gone && !with_off)
            return;
    }
    at = swprintf_s(t, 16384, L"This removes data for mods that aren't installed or are turned off. If you turn them "
                              L"back on, their edits are gone. A backup is made first.\n\nTaken out:\n");
    shown = 0;
    for (i = 0; i < n && at < 15000; i++) {
        const ScItem *m = &items[i];
        WCHAR what[160] = L"";
        if (m->off && !with_off)
            continue;
        if (shown++ >= 30)
            continue;
        if (m->profile) wcscat_s(what, 160, L"superstar, moves, entrance, Threads");
        if (m->universe) wcscat_s(what, 160, what[0] ? L", Universe" : L"Universe");
        if (m->mod_save) wcscat_s(what, 160, what[0] ? L", its mod save" : L"its mod save");
        if (m->refs) wcscat_s(what, 160, what[0] ? L", teams / titles / Universe cards" : L"teams / titles / Universe cards");
        if (!what[0]) wcscat_s(what, 160, L"a note of it");
        at += swprintf_s(t + at, 16384 - at, L"  %s%s%s%s (%s)\n", m->name,
                         m->folder[0] && _wcsicmp(m->folder, m->name) ? L" - " : L"",
                         m->folder[0] && _wcsicmp(m->folder, m->name) ? m->folder : L"",
                         m->off ? L", turned off" : L"", what);
    }
    if (shown > 30)
        at += swprintf_s(t + at, 16384 - at, L"  ... and %d more\n", shown - 30);
    swprintf_s(t + at, 16384 - at, L"\n%d mod%s. The game's own superstars, your Created Superstars and the mods "
                                   L"that are on stay as they are.", shown, shown == 1 ? L"" : L"s");
    if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    if (!saves_backup_to(L" (before cleaning)", before)) {
        saves_status(L"Could not back up the saves; nothing was cleaned.");
        return;
    }
    if ((n = save_clean_apply(saves, items, n, with_off, err, 600)) < 0) {
        saves_status(L"Not cleaned: %s Your saves are backed up in %s.", err, before);
        return;
    }
    saves_refresh();
    saves_status(L"Took out %d mod%s' data. Backup: %s (Restore backup\x2026 puts it back).", n, n == 1 ? L"" : L"s",
                 before);
}

static void saves_restore(void)
{
    WCHAR base[MAX_PATH], pick[MAX_PATH], saves[MAX_PATH], before[MAX_PATH], pat[MAX_PATH], from[MAX_PATH],
        to[MAX_PATH], t[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0, had;
    if (saves_locked() || !join(base, s_game_dir, L"SaveBackups") || !saves_dir(saves))
        return;
    if (!pick_folder_in(L"Choose the backup to restore (a folder in SaveBackups)", base, pick))
        return;
    if (!join(pat, pick, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do
        if (is_save_entry(&fd))
            n++;
    while (FindNextFileW(f, &fd));
    FindClose(f);
    if (!n) {
        saves_status(L"That folder has no saves in it.");
        return;
    }
    swprintf_s(t, MAX_PATH * 2, L"Replace your saves with the %d in\n%s?\n\nYour current saves are backed up first.",
               n, pick);
    if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    had = dir_exists(saves);
    if (had && !saves_backup_to(L" (before restore)", before)) {
        saves_status(L"Could not back up the current saves; nothing was restored.");
        return;
    }
    if (had)
        saves_clear(saves);
    mkdirs(saves);
    f = FindFirstFileW(pat, &fd);
    n = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, L".mount"))
            continue;
        if (join(from, pick, fd.cFileName) && join(to, saves, fd.cFileName) && shell_op(FO_COPY, from, to, 0)
                && fd.cFileName[0] != L'.')
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    saves_make_writable(saves);
    saves_refresh();
    if (had)
        saves_status(L"Restored %d save%s from %s. Your previous saves were backed up to %s.", n, n == 1 ? L"" : L"s",
                     pick, before);
    else
        saves_status(L"Restored %d save%s from %s.", n, n == 1 ? L"" : L"s", pick);
}

/* The selected rows' file names (col 0); returns the count. */
static int saves_selected(WCHAR names[][MAX_PATH], int max)
{
    HWND list = ctl(ID_SV_LIST);
    int i = -1, n = 0;
    while (n < max && (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0) {
        ListView_GetItemText(list, i, 0, names[n], MAX_PATH);
        n++;
    }
    return n;
}

static void saves_export(void)
{
    static WCHAR names[64][MAX_PATH];
    WCHAR dest[MAX_PATH], saves[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    int n, i, done = 0, skipped = 0;
    if (saves_locked())
        return;
    n = saves_selected(names, 64);
    if (!n) {
        saves_status(L"Choose the saves to export in the list first (Ctrl+click for several).");
        return;
    }
    if (!saves_dir(saves) || !pick_folder(L"Choose where to export the saves to", dest))
        return;
    for (i = 0; i < n; i++) {
        if (!join(from, saves, names[i]) || !join(to, dest, names[i]))
            continue;
        if (file_exists(to) || dir_exists(to)) {
            skipped++;
            continue;
        }
        if (shell_op(FO_COPY, from, to, 0)) {
            done++;
            save_info_copy(saves, names[i], dest, names[i]);
        }
    }
    saves_status(L"Exported %d save%s to %s.%s", done, done == 1 ? L"" : L"s", dest,
                 skipped ? L" Some were already there and were left as they were." : L"");
}

/* Picks save files to import (several at once). Returns the count; `buf`
 * gets them as full paths, one after the other (0-separated). */
static int pick_save_files(WCHAR *buf, size_t n)
{
    IFileOpenDialog *d;
    IShellItemArray *items;
    DWORD count = 0, i;
    size_t at = 0;
    int got = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    d->lpVtbl->SetTitle(d, L"Choose the save files to import");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResults(d, &items))) {
        items->lpVtbl->GetCount(items, &count);
        for (i = 0; i < count; i++) {
            IShellItem *it;
            PWSTR p = NULL;
            if (FAILED(items->lpVtbl->GetItemAt(items, i, &it)))
                continue;
            if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                size_t len = wcslen(p) + 1;
                if (at + len < n) {
                    wcscpy_s(buf + at, n - at, p);
                    at += len;
                    got++;
                }
                CoTaskMemFree(p);
            }
            it->lpVtbl->Release(it);
        }
        items->lpVtbl->Release(items);
    }
    d->lpVtbl->Release(d);
    return got;
}

/* ── importing saves: the right name, writable ───────────────────────── */
/* An imported save only works under the name the game looks for, and writable (a read-only one: the game's
   save mount fails and it says "Load Failed" with a blank save). Players' copies arrive renamed ("SaveData (1).dat"
   from a browser), as an older version's package folder (<name>\SaveData.Dat inside), as Xbox 360 packages, or
   read-only (from a zip, a disc, another PC). */

/* `name` without a browser's " (1)" / "(2)" before its extension. */
static void save_name_clean(WCHAR *name)
{
    WCHAR *dot = wcsrchr(name, L'.'), *open;
    if (!dot || dot == name || dot[-1] != L')')
        return;
    open = dot - 1;
    while (open > name && iswdigit(open[-1]))
        open--;
    if (open == dot - 1 || open == name || open[-1] != L'(')
        return;
    open--;
    if (open > name && open[-1] == L' ')
        open--;
    memmove(open, dot, (wcslen(dot) + 1) * sizeof(WCHAR));
}

static uint32_t be32_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* What the file is, by its bytes: 1 the main save (SaveData.dat: its checksum, caw_import.c's), 2 a Created
   Superstar (.cas), 3 Paint Tool logos (.pt), 4 an Xbox 360 package (STFS), 0 something else. */
static int save_file_kind(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    uint8_t *b = NULL;
    DWORD got = 0;
    int kind = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= 16 && size.QuadPart <= 0x900000 &&
        (b = (uint8_t *)malloc((size_t)size.QuadPart)) && ReadFile(f, b, (DWORD)size.QuadPart, &got, NULL) &&
        got == (DWORD)size.QuadPart) {
        if (!memcmp(b, "CON ", 4) || !memcmp(b, "LIVE", 4) || !memcmp(b, "PIRS", 4))
            kind = 4;
        else if (got == 7888892 && be32_at(b) == 0x02A78E9Au)
            kind = 3;
        else if (got == 1347532 && be32_at(b) == 0x4B && be32_at(b + 4) == 0x18 && be32_at(b + 8) == 7)
            kind = 2;
        else if (got >= 0x812DC) {
            uint32_t sum = 0, i;
            for (i = 0x1C; i < 0x812DC; i++)
                sum += b[i];
            if (be32_at(b + 0x18) == sum)
                kind = 1;
        }
    }
    free(b);
    CloseHandle(f);
    return kind;
}

/* The name `src` goes into the saves folder as (`out`): an older version's package folder (<name>\SaveData.Dat)
   gives its folder's name; a browser's " (1)" goes; the main save is always SaveData.dat. 0: not a save of the
   game's by its bytes or its name (asks first). */
static int save_import_name(const WCHAR *src, int kind, WCHAR *out)
{
    const WCHAR *name = wcsrchr(src, L'\\');
    name = name ? name + 1 : src;
    wcscpy_s(out, MAX_PATH, name);
    if (!_wcsicmp(name, L"SaveData.Dat") && kind != 1 && name > src + 1) {
        /* (inside a package folder: the folder is the save's name) */
        WCHAR dir[MAX_PATH];
        const WCHAR *slash;
        wcsncpy_s(dir, MAX_PATH, src, (size_t)(name - src - 1));
        slash = wcsrchr(dir, L'\\');
        if (slash && wcschr(slash + 1, L'.'))
            wcscpy_s(out, MAX_PATH, slash + 1);
    }
    save_name_clean(out);
    if (kind == 1)
        wcscpy_s(out, MAX_PATH, L"SaveData.dat");
    {   /* (replays and highlight reels have nothing to tell them by: their names) */
        const WCHAR *ext = wcsrchr(out, L'.');
        if (!kind && ext && (!_wcsicmp(ext, L".cas") || !_wcsicmp(ext, L".pt") || !_wcsicmp(ext, L".rec") ||
                             !_wcsicmp(ext, L".scn") || !_wcsicmp(ext, L".dat")))
            return 1;
    }
    return kind != 0;
}

/* Every file in the saves folder writable again (read-only ones: "Load Failed" in the game). The count. */
static int saves_make_writable(const WCHAR *saves)
{
    WCHAR pat[MAX_PATH], f[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    if (!join(pat, saves, L"*"))
        return 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) &&
            join(f, saves, fd.cFileName) && SetFileAttributesW(f, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY))
            n++;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* A save's files in .info (its header: the name the game finds it by; the list's picture), from one saves-like
   folder to another, under `to_name`. */
static void save_info_copy(const WCHAR *from_dir, const WCHAR *from_name, const WCHAR *to_dir, const WCHAR *to_name)
{
    static const WCHAR *const extra[] = { L".header", L".file", L".png" };
    WCHAR a[MAX_PATH], b[MAX_PATH], d[MAX_PATH];
    int j;
    if (!join(d, to_dir, L".info"))
        return;
    for (j = 0; j < 3; j++) {
        swprintf_s(a, MAX_PATH, L"%s\\.info\\%s%s", from_dir, from_name, extra[j]);
        swprintf_s(b, MAX_PATH, L"%s\\%s%s", d, to_name, extra[j]);
        if (file_exists(a)) {
            CreateDirectoryW(d, NULL);
            if (CopyFileW(a, b, FALSE))
                SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
        }
    }
}

static void saves_import(void)
{
    static WCHAR files[32768];
    static WCHAR names[256][MAX_PATH];
    static int kinds[256];
    WCHAR saves[MAX_PATH], to[MAX_PATH], before[MAX_PATH], t[512];
    const WCHAR *p;
    int n, i, exists = 0, done = 0, backed = 0, unknown = 0, renamed = 0;
    if (saves_locked() || !saves_dir(saves))
        return;
    n = pick_save_files(files, 32768);
    if (!n)
        return;
    for (p = files, i = 0; i < n && i < 256; i++, p += wcslen(p) + 1) {
        kinds[i] = save_file_kind(p);
        if (!save_import_name(p, kinds[i], names[i]))
            unknown++;
        if (join(to, saves, names[i]) && (file_exists(to) || dir_exists(to)))
            exists++;
    }
    if (n > 256)
        n = 256;
    if (unknown) {
        swprintf_s(t, 512, L"%d of the %d files don't look like saves of this game (not a main save, Created "
                           L"Superstar, Paint Tool or Xbox 360 save). Import them anyway?", unknown, n);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    if (exists) {
        swprintf_s(t, 512, L"%d of the %d files replace saves you have. Replace them?\n\n"
                           L"Your current saves are backed up first.", exists, n);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (!saves_backup_to(L" (before import)", before)) {
            saves_status(L"Could not back up the current saves; nothing was imported.");
            return;
        }
        backed = 1;
    }
    mkdirs(saves);
    for (p = files, i = 0; i < n; i++, p += wcslen(p) + 1) {
        WCHAR src_dir[MAX_PATH];
        const WCHAR *src_name = wcsrchr(p, L'\\');
        int ok = 0;
        if (!join(to, saves, names[i]))
            continue;
        if (kinds[i] == 4) {   /* (an Xbox 360 package: its save file, as the Import Xbox 360 save button does) */
            uint8_t *data = NULL;
            size_t size = 0;
            if (stfs_read_save(p, &data, &size)) {
                HANDLE f = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                DWORD put = 0;
                if (f != INVALID_HANDLE_VALUE) {
                    ok = WriteFile(f, data, (DWORD)size, &put, NULL) && put == size;
                    CloseHandle(f);
                }
                free(data);
            }
        } else {
            ok = CopyFileW(p, to, FALSE);
        }
        if (!ok)
            continue;
        done++;
        if (wcscmp(names[i], src_name ? src_name + 1 : p))
            renamed++;
        SetFileAttributesW(to, FILE_ATTRIBUTE_NORMAL);
        /* (its header beside it, an exported .info: the name the game finds it by) */
        if (src_name && kinds[i] != 4) {
            wcsncpy_s(src_dir, MAX_PATH, p, (size_t)(src_name - p));
            if (!wcscmp(names[i], src_name + 1))
                save_info_copy(src_dir, src_name + 1, saves, names[i]);
        }
    }
    saves_make_writable(saves);
    saves_refresh();
    swprintf_s(t, 512, renamed ? L" %d got the name the game looks for." : L"", renamed);
    if (backed)
        saves_status(L"Imported %d save%s.%s Your previous saves were backed up to %s.", done, done == 1 ? L"" : L"s",
                     t, before);
    else
        saves_status(L"Imported %d save%s.%s", done, done == 1 ? L"" : L"s", t);
}

static void saves_delete(void)
{
    static WCHAR names[64][MAX_PATH];
    static const WCHAR *const extra[] = { L".header", L".file", L".png" };
    WCHAR saves[MAX_PATH], p[MAX_PATH], t[512];
    int n, i, j, main = 0, done = 0;
    if (saves_locked())
        return;
    n = saves_selected(names, 64);
    if (!n) {
        saves_status(L"Choose the saves to delete in the list first.");
        return;
    }
    for (i = 0; i < n; i++)
        if (!_wcsicmp(names[i], L"SaveData.dat"))
            main = 1;
    swprintf_s(t, 512, L"Move %d save%s to the Recycle Bin?%s", n, n == 1 ? L"" : L"s",
               main ? L"\n\nThis includes the main save: the game starts over (settings, unlocks and progress)."
                    : L"");
    if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | (main ? MB_ICONWARNING : MB_ICONQUESTION)) != IDYES)
        return;
    if (!saves_dir(saves))
        return;
    for (i = 0; i < n; i++) {
        if (join(p, saves, names[i]) && shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO))
            done++;
        for (j = 0; j < 3; j++) {
            swprintf_s(p, MAX_PATH, L"%s\\.info\\%s%s", saves, names[i], extra[j]);
            if (file_exists(p))
                shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO);
        }
    }
    saves_refresh();
    saves_status(L"Moved %d save%s to the Recycle Bin.", done, done == 1 ? L"" : L"s");
}

/* The Saves folder row: where the saves are, and whether that's the default. */
static void saves_folder_show(void)
{
    WCHAR s[MAX_PATH], d[MAX_PATH], t[MAX_PATH + 64];
    saves_folder_setting(s, MAX_PATH);
    if (saves_dir(d))
        swprintf_s(t, MAX_PATH + 64, L"Saves folder: %s%s", d, s[0] ? L"" : L" (default)");
    else
        wcscpy_s(t, MAX_PATH + 64, L"Saves folder: (install the game first)");
    set_text(ID_SV_FOLDER, t);
    EnableWindow(ctl(ID_SV_DEFAULT), s[0] != 0);
    {
        char v[16];
        CheckDlgButton(s_wnd, ID_SV_PERMOD, toml_get("per_mod_saves", v, sizeof v) && !strcmp(v, "true") ? BST_CHECKED
                                                                                                           : BST_UNCHECKED);
        CheckDlgButton(s_wnd, ID_SV_BIGROSTER,
                       toml_get("big_roster", v, sizeof v) && !strcmp(v, "true") ? BST_CHECKED : BST_UNCHECKED);
        saves_permod_lock();
    }
    presets_fill();
}

static int folder_has_saves(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int has = 0;
    if (!join(pat, dir, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do
        has |= is_save_entry(&fd);
    while (!has && FindNextFileW(f, &fd));
    FindClose(f);
    return has;
}

/* Change... / Default: the new folder for the saves (NULL: the default). If it
 * has no saves and the current one does, offers to copy them over (the old
 * folder is left as it is). */
/* Stores the saves folder setting (NULL: the default); 0 with a message if not. */
static int saves_store_folder(const WCHAR *dir)
{
    char u[MAX_PATH * 3], v[MAX_PATH * 3 + 4];
    if (dir) {
        size_t i;
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, u, sizeof u, NULL, NULL);
        for (i = 0; u[i]; i++)
            if (u[i] == '\\')
                u[i] = '/';
        if (strchr(u, '"')) {
            saves_status(L"That folder name can't be used.");
            return 0;
        }
        sprintf_s(v, sizeof v, "\"%s\"", u);
    } else {
        strcpy_s(v, sizeof v, "\"\"");
    }
    if (!toml_set("saves_folder", v)) {
        saves_status(L"The setting could not be saved (is the game folder read-only?).");
        return 0;
    }
    return 1;
}

static void saves_set_folder(const WCHAR *dir)
{
    WCHAR old[MAX_PATH], now[MAX_PATH], pat[MAX_PATH], t[MAX_PATH * 3];
    if (!s_game_dir[0] || saves_locked() || !saves_dir(old))
        return;
    if (!saves_store_folder(dir))
        return;
    if (!saves_dir(now) || !mkdirs(now)) {
        saves_status(L"That folder could not be made.");
        saves_folder_show();
        return;
    }
    if (_wcsicmp(old, now) && folder_has_saves(old) && !folder_has_saves(now)) {
        swprintf_s(t, MAX_PATH * 3, L"Copy your current saves to the new folder?\n\nFrom: %s\nTo: %s\n\n"
                                    L"(The old folder is left as it is.)", old, now);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES && join(pat, old, L"*"))
            shell_op(FO_COPY, pat, now, 0);
    }
    saves_folder_show();
    saves_refresh();
}

static void saves_change_folder(void)
{
    WCHAR cur[MAX_PATH], pick[MAX_PATH];
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (!saves_dir(cur))
        cur[0] = 0;
    if (pick_folder_in(L"Choose the folder for the saves", dir_exists(cur) ? cur : s_game_dir, pick))
        saves_set_folder(pick);
}

static void saves_open(void)
{
    WCHAR saves[MAX_PATH];
    if (saves_dir(saves) && mkdirs(saves))
        ShellExecuteW(s_wnd, L"open", saves, NULL, NULL, SW_SHOWNORMAL);
}

static void saves_open_backups(void)
{
    WCHAR b[MAX_PATH];
    if (join(b, s_game_dir, L"SaveBackups") && mkdirs(b))
        ShellExecuteW(s_wnd, L"open", b, NULL, NULL, SW_SHOWNORMAL);
}

/* ── save presets ─────────────────────────────────────────────────────── */
/* A preset: a whole saves folder of its own, <game>\Save Presets\<name>;
 * using one sets the saves folder setting to it ("Save Presets/<name>"). */
#define PRESETS_DIR L"Save Presets"
#define PRESET_OTHER L"Other folder: "

/* A small modal box asking for a line of text. */
static WCHAR *s_ask_out;
static int s_ask_n, s_ask_done;

static LRESULT CALLBACK ask_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK)
            GetDlgItemTextW(w, 100, s_ask_out, s_ask_n);
        s_ask_done = LOWORD(wp) == IDOK ? 1 : -1;
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_CLOSE) {
        s_ask_done = -1;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static int ask_text(const WCHAR *prompt, WCHAR *out, int n)
{
    static int registered;
    WNDCLASSW wc;
    RECT r = { 0, 0, 0, 0 }, pr;
    HWND w, edit, c;
    MSG msg;
    int quit = 0;
    if (!registered) {
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = ask_proc;
        wc.hInstance = s_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SvR2011Ask";
        RegisterClassW(&wc);
        registered = 1;
    }
    r.right = S(380);
    r.bottom = S(144);
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(s_wnd, &pr);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SvR2011Ask", WINDOW_TITLE, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        pr.left + (pr.right - pr.left - (r.right - r.left)) / 2,
                        pr.top + (pr.bottom - pr.top - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top,
                        s_wnd, NULL, s_inst, NULL);
    if (!w)
        return 0;
    c = CreateWindowExW(0, L"Static", prompt, WS_CHILD | WS_VISIBLE, S(16), S(12), S(348), S(36), w, NULL, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", out, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, S(16),
                           S(52), S(348), S(26), w, (HMENU)(INT_PTR)100, s_inst, NULL);
    SendMessageW(edit, WM_SETFONT, (WPARAM)s_font, TRUE);
    SendMessageW(edit, EM_LIMITTEXT, (WPARAM)(n - 1), 0);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    c = CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(184), S(98), S(86),
                        S(30), w, (HMENU)(INT_PTR)IDOK, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    c = CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(278), S(98), S(86), S(30), w,
                        (HMENU)(INT_PTR)IDCANCEL, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    s_ask_out = out;
    s_ask_n = n;
    s_ask_done = 0;
    EnableWindow(s_wnd, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(edit);
    while (!s_ask_done) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
            quit = 1;
            break;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(s_wnd, TRUE);
    SetForegroundWindow(s_wnd);
    if (quit) {
        if (IsWindow(w))
            DestroyWindow(w);
        PostQuitMessage(0);
        return 0;
    }
    return s_ask_done == 1;
}

/* A preset name made safe as a folder name (0: nothing left). */
static int preset_clean(WCHAR *name)
{
    WCHAR *r = name, *o = name;
    for (; *r; r++)
        if (*r >= 32 && !wcschr(L"\\/:*?\"<>|", *r))
            *o++ = *r;
    *o = 0;
    while (o > name && (o[-1] == L' ' || o[-1] == L'.'))
        *--o = 0;
    for (r = name; *r == L' '; r++)
        ;
    if (r != name)
        memmove(name, r, (wcslen(r) + 1) * sizeof(WCHAR));
    return name[0] != 0;
}

static void presets_fill(void)
{
    HWND c = ctl(ID_SV_PRESET);
    WCHAR s[MAX_PATH], base[MAX_PATH], pat[MAX_PATH], rel[MAX_PATH], abs_[MAX_PATH], other[MAX_PATH + 32];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    LRESULT sel = 0;
    if (!c)
        return;
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Default (the game folder's Saves)");
    if (!s_game_dir[0])
        goto done;
    saves_folder_setting(s, MAX_PATH);
    if (join(base, s_game_dir, PRESETS_DIR) && join(pat, base, L"*")
            && (f = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            LRESULT i;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            i = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
            swprintf_s(rel, MAX_PATH, PRESETS_DIR L"\\%s", fd.cFileName);
            if (s[0] && (!_wcsicmp(s, rel) || (join(abs_, base, fd.cFileName) && !_wcsicmp(s, abs_))))
                sel = i;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (s[0] && !sel) {
        swprintf_s(other, MAX_PATH + 32, PRESET_OTHER L"%s", s);
        sel = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)other);
    }
done:
    SendMessageW(c, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* Switches the saves to `rel` (NULL: the default), without copying anything. */
static void presets_use(const WCHAR *rel, const WCHAR *name)
{
    WCHAR now[MAX_PATH];
    if (!saves_store_folder(rel)) {
        presets_fill();
        return;
    }
    if (saves_dir(now))
        mkdirs(now);
    saves_refresh();
    if (name)
        saves_status(L"Using the save preset \"%s\". The game uses it from its next start.", name);
    else
        saves_status(L"Using the default saves (the game folder's Saves).");
}

static void presets_pick(void)
{
    HWND c = ctl(ID_SV_PRESET);
    LRESULT i = SendMessageW(c, CB_GETCURSEL, 0, 0);
    WCHAR name[MAX_PATH + 32], rel[MAX_PATH];
    if (i == CB_ERR)
        return;
    if (saves_locked()) {
        presets_fill();
        return;
    }
    if (i == 0) {
        presets_use(NULL, NULL);
        return;
    }
    if (SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0) >= MAX_PATH + 32)
        return;
    SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)name);
    if (!wcsncmp(name, PRESET_OTHER, wcslen(PRESET_OTHER)))
        return;  /* (already in use) */
    swprintf_s(rel, MAX_PATH, PRESETS_DIR L"\\%s", name);
    presets_use(rel, name);
}

/* A new preset's folder from a name asked for; 0 if cancelled or taken. */
static int preset_make(const WCHAR *prompt, WCHAR *name, int n, WCHAR *dir, WCHAR *rel)
{
    WCHAR base[MAX_PATH];
    if (!ask_text(prompt, name, n))
        return 0;
    if (!preset_clean(name)) {
        saves_status(L"That name can't be used for a preset.");
        return 0;
    }
    if (!join(base, s_game_dir, PRESETS_DIR) || !join(dir, base, name))
        return 0;
    if (dir_exists(dir) && folder_has_saves(dir)) {
        saves_status(L"There is already a preset called \"%s\".", name);
        return 0;
    }
    if (!mkdirs(dir)) {
        saves_status(L"The preset's folder could not be made.");
        return 0;
    }
    swprintf_s(rel, MAX_PATH, PRESETS_DIR L"\\%s", name);
    return 1;
}

static void presets_new(void)
{
    WCHAR name[64] = L"", dir[MAX_PATH], rel[MAX_PATH], cur[MAX_PATH], pat[MAX_PATH], mnt[MAX_PATH], t[512];
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (saves_locked() || !preset_make(L"A name for the new save preset:", name, 64, dir, rel))
        return;
    if (saves_dir(cur) && folder_has_saves(cur)) {
        swprintf_s(t, 512, L"Start \"%s\" with a copy of the saves you use now?\n\nYes: a copy (your saves now stay as "
                           L"they are).\nNo: empty - the game starts over in it.", name);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES && join(pat, cur, L"*")) {
            shell_op(FO_COPY, pat, dir, 0);
            if (join(mnt, dir, L".mount") && dir_exists(mnt))  /* (the game's scratch) */
                shell_op(FO_DELETE, mnt, NULL, 0);
        }
    }
    presets_use(rel, name);
}

static void saves_import_360(void)
{
    WCHAR src[MAX_PATH], name[64] = L"Xbox 360 save", dir[MAX_PATH], rel[MAX_PATH], info[MAX_PATH];
    HCURSOR old;
    int n, skipped = 0;
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (saves_locked())
        return;
    if (!pick_folder(L"Choose your Xbox 360 save folder (5451085D - or a USB drive or folder with it in)", src))
        return;
    if (!preset_make(L"A name for the save preset your Xbox 360 save goes into:", name, 64, dir, rel))
        return;
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    n = stfs_import_saves(src, dir, &skipped);
    SetCursor(old);
    if (!n) {
        if (join(info, dir, L".info"))
            RemoveDirectoryW(info);
        RemoveDirectoryW(dir);
        saves_refresh();
        saves_status(L"No WWE SmackDown vs. Raw 2011 Xbox 360 saves were found in %s%s", src,
                     skipped ? L" (some packages could not be read)." : L".");
        return;
    }
    presets_use(rel, name);
    saves_status(L"Brought %d Xbox 360 save%s over into the preset \"%s\", now in use.%s", n, n == 1 ? L"" : L"s", name,
                 skipped ? L" Some packages could not be read." : L"");
}

/* ── Paint Tool ───────────────────────────────────────────────────────── */

/* Saves\00PaintTool.pt holds the Paint Tool's 20 logos (reverse-engineered
 * from the game: the loader sub_827B3630, the checksums sub_827B35C8 /
 * sub_827B3118). Big-endian:
 *   0        u32 magic 0x02A78E9A, u32 version 3
 *   8+k*S    slot k (S = 0x604CC); offsets below are from k*S:
 *     8..51      header (+36 used, +44 / +48 width / height = 256)
 *     52         the logo: 256 x 256 pixels, A R G B bytes (the editor's canvas)
 *     0x40034    the same as an 8-bit colour-mapped TGA (20-byte header,
 *                256 A R G B palette entries, 65536 indices, top-down)
 *     0x50448    the same as a DXT5 DDS (128-byte header, 64 KB), what the
 *                game shows
 *     0x604C8    saved at: u16 year, month, day, hour, minute, second, weekday+1 (UTC)
 *     0x604D0    u32 checksum: a sum of the slot's fields by type (pt_slot_sum)
 *   end-4    u32 magic + version + the 20 slot checksums
 * The game refuses the whole file when a checksum is off. */

#define PT_FILE    L"00PaintTool.pt"
#define PT_SLOTS   20
#define PT_SLOT    0x604CCu
#define PT_BYTES   (8u + PT_SLOTS * PT_SLOT + 4u)
#define PT_MAGIC   0x02A78E9Au
#define PT_W       256
#define PT_CANVAS  52u
#define PT_TGA     0x40034u
#define PT_DDS     0x50448u
#define PT_STAMP   0x604C8u
#define PT_SUM     0x604D0u

static const uint8_t k_pt_used_header[44] = {
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t k_pt_empty_header[44] = {
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
static const uint8_t k_pt_tga_header[20] = {
    0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x08, 0x00, 0x04, 0x01 };
static const uint8_t k_pt_dds_header[128] = {
    0x44, 0x44, 0x53, 0x20, 0x7C, 0x00, 0x00, 0x00, 0x07, 0x10, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, [76] = 0x20, [80] = 0x04, [84] = 0x44, 0x58, 0x54, 0x35,
    [108] = 0x02, 0x10 };

/* The port has 10 pages of 20 logos (src/paint_pages.h): page 1 is
 * 00PaintTool.pt, pages 2-10 are Saves\.paint\pNN_sMM.bin, one file per
 * used slot (the raw PT_SLOT bytes of that slot, as at 8 + k * PT_SLOT). */
#define PT_PAGES   10

static uint8_t *s_pt;                     /* the shown page as a whole file, PT_BYTES */
static int      s_pt_sel = -1;
static int      s_pt_page;                /* 0-based */
static HWND     s_pt_grid;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint8_t *pt_slot(uint8_t *f, int k) { return f + (size_t)k * PT_SLOT; }

/* The game's slot checksum (sub_827B3118): every field of the slot, added as
 * the type the game reads it as (signed bytes at +20..27). */
static uint32_t pt_slot_sum(const uint8_t *f, int k)
{
    const uint8_t *b = f + (size_t)k * PT_SLOT, *t = b + PT_TGA, *d = b + PT_DDS, *e = b + PT_STAMP;
    static const int words[] = { 8, 12, 16, 28, 32, 36, 40, 44, 48 };
    static const int tb[] = { 0, 1, 2, 7, 16, 17 }, th[] = { 3, 5, 8, 10, 12, 14, 18 };
    uint32_t s = 0;
    int i, j;
    for (i = 20; i < 28; i++)
        s += (uint32_t)(int32_t)(int8_t)b[i];
    for (i = 0; i < 9; i++)
        s += rd32(b + words[i]);
    for (i = 0; i < PT_W * PT_W; i++)
        s += rd32(b + PT_CANVAS + 4 * i);
    for (i = 0; i < 6; i++)
        s += t[tb[i]];
    for (i = 0; i < 7; i++)
        s += rd16(t + th[i]);
    for (i = 0; i < 256; i++)
        s += rd32(t + 20 + 4 * i);
    for (i = 0; i < PT_W * PT_W; i++)
        s += t[0x414 + i];
    for (i = 0; i < 32; i++)
        s += rd32(d + 4 * i);
    for (j = 0; j < 2048; j++) {
        const uint8_t *q = d + 128 + 32 * j;
        s += rd16(q) + rd16(q + 2) + rd32(q + 4) + rd16(q + 8) + rd16(q + 10) + rd32(q + 12)
           + rd16(q + 16) + rd16(q + 18) + rd32(q + 20) + rd16(q + 24) + rd16(q + 26) + rd32(q + 28);
    }
    s += rd16(e);
    for (i = 2; i < 8; i++)
        s += e[i];
    return s;
}

/* Recomputes every checksum (the game's sub_827B35C8). */
static void pt_fix_sums(uint8_t *f)
{
    uint32_t total = rd32(f) + rd32(f + 4);
    int k;
    for (k = 0; k < PT_SLOTS; k++) {
        uint32_t s = pt_slot_sum(f, k);
        wr32(pt_slot(f, k) + PT_SUM, s);
        total += s;
    }
    wr32(f + PT_BYTES - 4, total);
}

static int pt_valid(const uint8_t *f)
{
    uint32_t total;
    int k;
    if (rd32(f) != PT_MAGIC || rd32(f + 4) != 3)
        return 0;
    total = rd32(f) + rd32(f + 4);
    for (k = 0; k < PT_SLOTS; k++)
        total += rd32(f + (size_t)k * PT_SLOT + PT_SUM);
    return total == rd32(f + PT_BYTES - 4);
}

static int pt_used(const uint8_t *f, int k) { return rd32(f + (size_t)k * PT_SLOT + 36) != 0; }

static int pt_path(WCHAR *out)
{
    WCHAR saves[MAX_PATH];
    return saves_dir(saves) && join(out, saves, PT_FILE);
}

/* Reads a Paint Tool file (0 on failure, with a message in `err`). */
static uint8_t *pt_read(const WCHAR *path, WCHAR *err, size_t errn)
{
    uint8_t *buf;
    FILE *f;
    size_t got;
    if (_wfopen_s(&f, path, L"rb") || !f) {
        swprintf_s(err, errn, L"There is no Paint Tool save yet: open CREATE MODES > CREATE A SUPERSTAR > "
                              L"PAINT TOOL in the game once.");
        return NULL;
    }
    buf = (uint8_t *)malloc(PT_BYTES + 1);
    got = buf ? fread(buf, 1, PT_BYTES + 1, f) : 0;
    fclose(f);
    if (!buf || got != PT_BYTES || !pt_valid(buf)) {
        free(buf);
        swprintf_s(err, errn, L"%s is not a Paint Tool save the game accepts.", path);
        return NULL;
    }
    return buf;
}

/* Writes the file back (checksums fixed), through a temporary file. */
static int pt_write(const WCHAR *path, uint8_t *f)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *o;
    int ok;
    pt_fix_sums(f);
    swprintf_s(tmp, MAX_PATH + 8, L"%s.new", path);
    if (_wfopen_s(&o, tmp, L"wb") || !o)
        return 0;
    ok = fwrite(f, 1, PT_BYTES, o) == PT_BYTES;
    ok = fclose(o) == 0 && ok;
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

static void pt_clear(uint8_t *f, int k);

/* Page `page`'s (1-9, 0-based) slot k file, beside the Paint Tool file `pt`. */
static int pt_page_file(const WCHAR *pt, int page, int k, WCHAR *out)
{
    const WCHAR *a = wcsrchr(pt, L'\\'), *b = wcsrchr(pt, L'/');
    size_t n;
    if (b > a) a = b;
    if (!a) return 0;
    n = (size_t)(a - pt) + 1;
    if (n + 24 >= MAX_PATH) return 0;
    wmemcpy(out, pt, n);
    swprintf_s(out + n, MAX_PATH - n, L".paint\\p%02d_s%02d.bin", page + 1, k + 1);
    return 1;
}

/* Reads page `page` (0-based) as a whole Paint Tool file. Pages 2-10 need
 * page 1's file too (the game makes it before the Paint Tool can be used). */
static uint8_t *pt_read_page(const WCHAR *pt, int page, WCHAR *err, size_t errn)
{
    WCHAR file[MAX_PATH];
    uint8_t *buf;
    int k;
    if (page == 0 || !file_exists(pt))
        return pt_read(pt, err, errn);
    buf = (uint8_t *)calloc(1, PT_BYTES);
    if (!buf) {
        swprintf_s(err, errn, L"Out of memory.");
        return NULL;
    }
    wr32(buf, PT_MAGIC);
    wr32(buf + 4, 3);
    for (k = 0; k < PT_SLOTS; k++) {
        FILE *f;
        uint8_t *slot = pt_slot(buf, k) + 8;
        size_t got = 0;
        if (pt_page_file(pt, page, k, file) && !_wfopen_s(&f, file, L"rb") && f) {
            got = fread(slot, 1, PT_SLOT, f);
            fclose(f);
        }
        if (got != PT_SLOT || !pt_used(buf, k))
            pt_clear(buf, k);
    }
    pt_fix_sums(buf);
    return buf;
}

/* Writes page `page` (0-based): page 1 as pt_write, pages 2-10 one file per
 * used slot (empty slots' files removed). */
static int pt_write_page(const WCHAR *pt, int page, uint8_t *f)
{
    WCHAR file[MAX_PATH], tmp[MAX_PATH + 8], dir[MAX_PATH];
    int k, ok = 1;
    if (page == 0)
        return pt_write(pt, f);
    pt_fix_sums(f);
    if (!pt_page_file(pt, page, 0, dir))
        return 0;
    *wcsrchr(dir, L'\\') = 0;
    if (!mkdirs(dir))
        return 0;
    for (k = 0; k < PT_SLOTS; k++) {
        FILE *o;
        if (!pt_page_file(pt, page, k, file)) return 0;
        if (!pt_used(f, k)) {
            DeleteFileW(file);
            continue;
        }
        swprintf_s(tmp, MAX_PATH + 8, L"%s.new", file);
        if (_wfopen_s(&o, tmp, L"wb") || !o) { ok = 0; continue; }
        if (fwrite(pt_slot(f, k) + 8, 1, PT_SLOT, o) != PT_SLOT) ok = 0;
        if (fclose(o) != 0) ok = 0;
        if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp);
            ok = 0;
        }
    }
    return ok;
}

/* A logo's PNG name: "Logo 3.png" on page 1, "Page 2 logo 3.png" after. */
static void pt_png_name(int page, int k, WCHAR *out, size_t n)
{
    if (page == 0)
        swprintf_s(out, n, L"Logo %d.png", k + 1);
    else
        swprintf_s(out, n, L"Page %d logo %d.png", page + 1, k + 1);
}

/* ── images (WIC) ── */

static IWICImagingFactory *wic(void)
{
    static IWICImagingFactory *factory;
    if (!factory)
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                         (void **)&factory);
    return factory;
}

/* Saves 256 x 256 BGRA pixels as a PNG. */
static int png_write(const WCHAR *path, const uint8_t *bgra)
{
    IWICImagingFactory *fac = wic();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    int ok = 0;
    if (!fac || FAILED(fac->lpVtbl->CreateStream(fac, &stream))
            || FAILED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))
            || FAILED(fac->lpVtbl->CreateEncoder(fac, &GUID_ContainerFormatPng, NULL, &enc))
            || FAILED(enc->lpVtbl->Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache))
            || FAILED(enc->lpVtbl->CreateNewFrame(enc, &frame, NULL))
            || FAILED(frame->lpVtbl->Initialize(frame, NULL))
            || FAILED(frame->lpVtbl->SetSize(frame, PT_W, PT_W))
            || FAILED(frame->lpVtbl->SetPixelFormat(frame, &fmt))
            || !IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA)
            || FAILED(frame->lpVtbl->WritePixels(frame, PT_W, PT_W * 4, PT_W * PT_W * 4, (BYTE *)bgra))
            || FAILED(frame->lpVtbl->Commit(frame)) || FAILED(enc->lpVtbl->Commit(enc)))
        goto done;
    ok = 1;
done:
    if (frame) frame->lpVtbl->Release(frame);
    if (enc) enc->lpVtbl->Release(enc);
    if (stream) stream->lpVtbl->Release(stream);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

/* Loads any image Windows can read, fitted into 256 x 256 (aspect kept,
 * centred, transparent around it), as BGRA. */
static int image_read(const WCHAR *path, uint8_t *bgra, WCHAR *err, size_t errn)
{
    IWICImagingFactory *fac = wic();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *src;
    UINT w = 0, h = 0, sw, sh, x0, y0, y;
    uint8_t *tmp = NULL;
    int ok = 0;
    if (!fac || FAILED(fac->lpVtbl->CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec))
            || FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame))
            || FAILED(fac->lpVtbl->CreateFormatConverter(fac, &conv))
            || FAILED(conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))
            || FAILED(conv->lpVtbl->GetSize(conv, &w, &h)) || !w || !h) {
        swprintf_s(err, errn, L"Could not read %s as an image.", path);
        goto done;
    }
    if (w >= h) { sw = PT_W; sh = (UINT)(((uint64_t)h * PT_W + w / 2) / w); }
    else        { sh = PT_W; sw = (UINT)(((uint64_t)w * PT_W + h / 2) / h); }
    if (!sw) sw = 1;
    if (!sh) sh = 1;
    src = (IWICBitmapSource *)conv;
    if (sw != w || sh != h) {
        if (FAILED(fac->lpVtbl->CreateBitmapScaler(fac, &scaler))
                || FAILED(scaler->lpVtbl->Initialize(scaler, src, sw, sh, WICBitmapInterpolationModeHighQualityCubic))) {
            swprintf_s(err, errn, L"Could not scale %s.", path);
            goto done;
        }
        src = (IWICBitmapSource *)scaler;
    }
    tmp = (uint8_t *)malloc((size_t)sw * sh * 4);
    if (!tmp || FAILED(src->lpVtbl->CopyPixels(src, NULL, sw * 4, sw * sh * 4, tmp))) {
        swprintf_s(err, errn, L"Could not read the pixels of %s.", path);
        goto done;
    }
    memset(bgra, 0, PT_W * PT_W * 4);
    x0 = (PT_W - sw) / 2;
    y0 = (PT_W - sh) / 2;
    for (y = 0; y < sh; y++)
        memcpy(bgra + ((size_t)(y0 + y) * PT_W + x0) * 4, tmp + (size_t)y * sw * 4, (size_t)sw * 4);
    ok = 1;
done:
    free(tmp);
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (dec) dec->lpVtbl->Release(dec);
    return ok;
}

/* ── the slot's three copies of the logo ── */

/* 256-colour palette by median cut over the image's colours (A R G B). */
typedef struct { uint32_t argb, count; } PtColor;
typedef struct { int lo, hi; } PtBox;

static int pt_cmp_ch;
static int pt_cmp(const void *a, const void *b)
{
    uint32_t x = (((const PtColor *)a)->argb >> pt_cmp_ch) & 0xFF, y = (((const PtColor *)b)->argb >> pt_cmp_ch) & 0xFF;
    return (int)x - (int)y;
}
static int pt_cmp_argb(const void *a, const void *b)
{
    uint32_t x = ((const PtColor *)a)->argb, y = ((const PtColor *)b)->argb;
    return x < y ? -1 : x > y;
}

static void pt_make_tga(uint8_t *t, const uint32_t *argb)
{
    PtColor *c = (PtColor *)malloc(sizeof(PtColor) * PT_W * PT_W);
    PtBox box[256];
    uint32_t pal[256];
    int n = 0, nb = 1, i, j;
    if (!c)
        return;
    for (i = 0; i < PT_W * PT_W; i++) { c[i].argb = argb[i]; c[i].count = 1; }
    qsort(c, PT_W * PT_W, sizeof *c, pt_cmp_argb);
    for (i = 0; i < PT_W * PT_W; i++) {                 /* unique colours with counts */
        if (n && c[n - 1].argb == c[i].argb) c[n - 1].count++;
        else c[n++] = c[i];
    }
    box[0].lo = 0; box[0].hi = n;
    while (nb < 256) {                                   /* split the widest box */
        int best = -1, bestw = 0, bestch = 0;
        for (i = 0; i < nb; i++) {
            if (box[i].hi - box[i].lo < 2) continue;
            for (j = 0; j < 32; j += 8) {
                int lo = 255, hi = 0, k;
                for (k = box[i].lo; k < box[i].hi; k++) {
                    int v = (c[k].argb >> j) & 0xFF;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (hi - lo > bestw) { bestw = hi - lo; best = i; bestch = j; }
            }
        }
        if (best < 0) break;
        pt_cmp_ch = bestch;
        qsort(c + box[best].lo, box[best].hi - box[best].lo, sizeof *c, pt_cmp);
        {   /* at the median by pixel count */
            uint64_t total = 0, acc = 0;
            int k, mid = -1;
            for (k = box[best].lo; k < box[best].hi; k++) total += c[k].count;
            for (k = box[best].lo; k < box[best].hi - 1; k++) {
                acc += c[k].count;
                if (acc * 2 >= total) { mid = k + 1; break; }
            }
            if (mid < 0) mid = box[best].hi - 1;
            box[nb].lo = mid; box[nb].hi = box[best].hi; box[best].hi = mid; nb++;
        }
    }
    for (i = 0; i < 256; i++) pal[i] = 0;
    for (i = 0; i < nb; i++) {                           /* each box's average */
        uint64_t s[4] = { 0, 0, 0, 0 }, w = 0;
        int k;
        for (k = box[i].lo; k < box[i].hi; k++) {
            for (j = 0; j < 4; j++) s[j] += (uint64_t)((c[k].argb >> (8 * j)) & 0xFF) * c[k].count;
            w += c[k].count;
        }
        if (w) pal[i] = (uint32_t)((s[3] / w) << 24 | (s[2] / w) << 16 | (s[1] / w) << 8 | (s[0] / w));
    }
    free(c);
    memcpy(t, k_pt_tga_header, 20);
    for (i = 0; i < 256; i++) wr32(t + 20 + 4 * i, pal[i]);
    for (i = 0; i < PT_W * PT_W; i++) {                  /* nearest palette entry */
        uint32_t p = argb[i];
        int best = 0;
        uint32_t bd = 0xFFFFFFFF;
        for (j = 0; j < nb; j++) {
            int da = (int)(p >> 24) - (int)(pal[j] >> 24), dr = (int)((p >> 16) & 0xFF) - (int)((pal[j] >> 16) & 0xFF),
                dg = (int)((p >> 8) & 0xFF) - (int)((pal[j] >> 8) & 0xFF), db = (int)(p & 0xFF) - (int)(pal[j] & 0xFF);
            uint32_t dd = (uint32_t)(da * da + dr * dr + dg * dg + db * db);
            if (dd < bd) { bd = dd; best = j; if (!dd) break; }
        }
        t[0x414 + i] = (uint8_t)best;
    }
}

static uint16_t to565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
static void from565(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31;
}

/* DXT5, block by block (little-endian, as a PC DDS). */
static void pt_make_dds(uint8_t *d, const uint32_t *argb)
{
    int bx, by, i;
    uint8_t *o = d + 128;
    memcpy(d, k_pt_dds_header, 128);
    for (by = 0; by < PT_W / 4; by++)
        for (bx = 0; bx < PT_W / 4; bx++, o += 16) {
            uint32_t px[16];
            int amin = 255, amax = 0, mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
            uint8_t apal[8];
            uint64_t abits = 0;
            uint32_t cbits = 0;
            uint16_t c0, c1;
            int cr[4], cg[4], cb[4];
            for (i = 0; i < 16; i++) {
                uint32_t p = argb[(by * 4 + i / 4) * PT_W + bx * 4 + i % 4];
                int a = p >> 24, ch[3] = { (int)(p >> 16) & 0xFF, (int)(p >> 8) & 0xFF, (int)p & 0xFF }, k;
                px[i] = p;
                if (a < amin) amin = a;
                if (a > amax) amax = a;
                for (k = 0; k < 3; k++) { if (ch[k] < mn[k]) mn[k] = ch[k]; if (ch[k] > mx[k]) mx[k] = ch[k]; }
            }
            /* alpha: 8 steps between max and min */
            apal[0] = (uint8_t)amax; apal[1] = (uint8_t)amin;
            for (i = 1; i < 7; i++) apal[i + 1] = (uint8_t)(((7 - i) * amax + i * amin) / 7);
            for (i = 0; i < 16; i++) {
                int a = px[i] >> 24, best = 0, bd = 1 << 30, k;
                for (k = 0; k < 8; k++) { int dd = abs(a - apal[k]); if (dd < bd) { bd = dd; best = k; } }
                abits |= (uint64_t)best << (3 * i);
            }
            o[0] = (uint8_t)amax; o[1] = (uint8_t)amin;
            for (i = 0; i < 6; i++) o[2 + i] = (uint8_t)(abits >> (8 * i));
            /* colour: the block's two most different pixels (the bounding
               box's corners can be colours the block doesn't have), four-colour mode */
            {
                int best = -1, pa = 0, pb = 0, a2, b2;
                for (a2 = 0; a2 < 16; a2++)
                    for (b2 = a2 + 1; b2 < 16; b2++) {
                        int dr = (int)((px[a2] >> 16) & 0xFF) - (int)((px[b2] >> 16) & 0xFF),
                            dg = (int)((px[a2] >> 8) & 0xFF) - (int)((px[b2] >> 8) & 0xFF),
                            db = (int)(px[a2] & 0xFF) - (int)(px[b2] & 0xFF), dd = dr * dr + dg * dg + db * db;
                        if (dd > best) { best = dd; pa = a2; pb = b2; }
                    }
                c0 = to565((px[pa] >> 16) & 0xFF, (px[pa] >> 8) & 0xFF, px[pa] & 0xFF);
                c1 = to565((px[pb] >> 16) & 0xFF, (px[pb] >> 8) & 0xFF, px[pb] & 0xFF);
                (void)mn; (void)mx;
            }
            if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
            if (c0 == c1) { cbits = 0; }
            else {
                from565(c0, &cr[0], &cg[0], &cb[0]);
                from565(c1, &cr[1], &cg[1], &cb[1]);
                cr[2] = (2 * cr[0] + cr[1]) / 3; cg[2] = (2 * cg[0] + cg[1]) / 3; cb[2] = (2 * cb[0] + cb[1]) / 3;
                cr[3] = (cr[0] + 2 * cr[1]) / 3; cg[3] = (cg[0] + 2 * cg[1]) / 3; cb[3] = (cb[0] + 2 * cb[1]) / 3;
                for (i = 0; i < 16; i++) {
                    int r = (px[i] >> 16) & 0xFF, g = (px[i] >> 8) & 0xFF, b = px[i] & 0xFF, best = 0, bd = 1 << 30, k;
                    for (k = 0; k < 4; k++) {
                        int dd = (r - cr[k]) * (r - cr[k]) + (g - cg[k]) * (g - cg[k]) + (b - cb[k]) * (b - cb[k]);
                        if (dd < bd) { bd = dd; best = k; }
                    }
                    cbits |= (uint32_t)best << (2 * i);
                }
            }
            o[8] = (uint8_t)c0; o[9] = (uint8_t)(c0 >> 8); o[10] = (uint8_t)c1; o[11] = (uint8_t)(c1 >> 8);
            for (i = 0; i < 4; i++) o[12 + i] = (uint8_t)(cbits >> (8 * i));
        }
}

/* Puts a logo (BGRA, 256 x 256) in slot k, as the game would have saved it. */
static void pt_put(uint8_t *f, int k, const uint8_t *bgra)
{
    uint8_t *b = pt_slot(f, k);
    uint32_t *argb = (uint32_t *)malloc(PT_W * PT_W * 4);
    SYSTEMTIME st;
    int i;
    if (!argb)
        return;
    memcpy(b + 8, k_pt_used_header, 44);
    for (i = 0; i < PT_W * PT_W; i++) {
        const uint8_t *p = bgra + 4 * i;
        argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
        wr32(b + PT_CANVAS + 4 * i, argb[i]);
    }
    pt_make_tga(b + PT_TGA, argb);
    pt_make_dds(b + PT_DDS, argb);
    GetSystemTime(&st);
    b[PT_STAMP] = (uint8_t)(st.wYear >> 8); b[PT_STAMP + 1] = (uint8_t)st.wYear;
    b[PT_STAMP + 2] = (uint8_t)st.wMonth; b[PT_STAMP + 3] = (uint8_t)st.wDay;
    b[PT_STAMP + 4] = (uint8_t)st.wHour; b[PT_STAMP + 5] = (uint8_t)st.wMinute;
    b[PT_STAMP + 6] = (uint8_t)st.wSecond; b[PT_STAMP + 7] = (uint8_t)(st.wDayOfWeek + 1);
    free(argb);
}

static void pt_clear(uint8_t *f, int k)
{
    uint8_t *b = pt_slot(f, k);
    memset(b + 8, 0, PT_STAMP - 8);
    memcpy(b + 8, k_pt_empty_header, 44);
}

/* Slot k's logo as BGRA: its canvas (full colour), unless the canvas isn't
 * the picture the game shows - its 8-bit copy (TGA: palette + indices), made
 * from the canvas when the logo is saved. A slot can hold another logo's
 * canvas (seen on page 2: logo 1's canvas from page 1), and then that copy is
 * drawn instead. */
static void pt_get(const uint8_t *f, int k, uint8_t *bgra)
{
    const uint8_t *b = f + (size_t)k * PT_SLOT, *c = b + PT_CANVAS;
    const uint8_t *pal = b + PT_TGA + 20, *idx = b + PT_TGA + 0x414;
    int i, differ = 0;
    for (i = 0; i < PT_W * PT_W; i++)
        if (memcmp(c + 4 * i, pal + 4 * idx[i], 4) && ++differ > PT_W * PT_W / 16) {
            c = NULL;
            break;
        }
    for (i = 0; i < PT_W * PT_W; i++) {
        const uint8_t *p = c ? c + 4 * i : pal + 4 * idx[i];
        bgra[4 * i] = p[3]; bgra[4 * i + 1] = p[2]; bgra[4 * i + 2] = p[1]; bgra[4 * i + 3] = p[0];
    }
}

/* ── the Paint Tool tab ── */

static void pt_status(const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1024, fmt, ap);
    va_end(ap);
    set_text(ID_PT_STATUS, buf);
}

static void pt_refresh(void)
{
    WCHAR path[MAX_PATH], err[512];
    int k, n = 0;
    free(s_pt);
    s_pt = NULL;
    if (!s_game_dir[0]) {
        pt_status(L"Install the game first (Install tab).");
    } else {
        if (!any_game_running())
            saves_migrate();
        if (pt_path(path) && (s_pt = pt_read_page(path, s_pt_page, err, 512)) != NULL) {
            for (k = 0; k < PT_SLOTS; k++)
                n += pt_used(s_pt, k);
            pt_status(L"Page %d: %d of %d logos used. Choose a logo, then Export as PNG or Import an image into it.",
                      s_pt_page + 1, n, PT_SLOTS);
        } else
            pt_status(L"%s", err);
    }
    {
        WCHAR t[32];
        swprintf_s(t, 32, L"Page %d of %d", s_pt_page + 1, PT_PAGES);
        set_text(ID_PT_PAGE, t);
    }
    if (s_pt_sel >= PT_SLOTS) s_pt_sel = -1;
    InvalidateRect(s_pt_grid, NULL, TRUE);
}

#define PT_CELL 72
#define PT_GAP  10

static LRESULT CALLBACK pt_grid_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), cx = x / S(PT_CELL + PT_GAP), cy = y / S(PT_CELL + PT_GAP);
        if (cx < 5 && cy < 4 && x % S(PT_CELL + PT_GAP) < S(PT_CELL) && y % S(PT_CELL + PT_GAP) < S(PT_CELL)) {
            s_pt_sel = cy * 5 + cx;
            InvalidateRect(w, NULL, FALSE);
        }
        SetFocus(w);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        SendMessageW(s_wnd, WM_COMMAND, ID_PT_EXPORT, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        BITMAPINFO bi;
        uint8_t *px = (uint8_t *)malloc(PT_W * PT_W * 4);
        HBRUSH bg = GetSysColorBrush(COLOR_WINDOW), cell = CreateSolidBrush(RGB(40, 40, 46)),
               sel = CreateSolidBrush(RGB(200, 16, 32));
        int k;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, bg);
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = PT_W;
        bi.bmiHeader.biHeight = -PT_W;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, s_font);
        SetStretchBltMode(dc, HALFTONE);
        for (k = 0; k < PT_SLOTS; k++) {
            RECT r;
            WCHAR label[16];
            r.left = (k % 5) * S(PT_CELL + PT_GAP); r.top = (k / 5) * S(PT_CELL + PT_GAP);
            r.right = r.left + S(PT_CELL); r.bottom = r.top + S(PT_CELL);
            if (k == s_pt_sel) {
                RECT o = { r.left - S(4), r.top - S(4), r.right + S(4), r.bottom + S(4) };
                FillRect(dc, &o, sel);
            }
            FillRect(dc, &r, cell);
            if (s_pt && px && pt_used(s_pt, k)) {
                int i;
                pt_get(s_pt, k, px);
                for (i = 0; i < PT_W * PT_W; i++) {      /* over white, as the game shows it */
                    int a = px[4 * i + 3], j;
                    for (j = 0; j < 3; j++) px[4 * i + j] = (uint8_t)((px[4 * i + j] * a + 255 * (255 - a)) / 255);
                }
                StretchDIBits(dc, r.left, r.top, S(PT_CELL), S(PT_CELL), 0, 0, PT_W, PT_W, px, &bi, DIB_RGB_COLORS,
                              SRCCOPY);
                SetTextColor(dc, RGB(90, 90, 90));
            } else
                SetTextColor(dc, RGB(200, 200, 205));
            swprintf_s(label, 16, s_pt && pt_used(s_pt, k) ? L"%d" : L"%d  empty", k + 1);
            {
                RECT t = { r.left + S(4), r.top + S(2), r.right, r.top + S(20) };
                DrawTextW(dc, label, -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        free(px);
        DeleteObject(cell);
        DeleteObject(sel);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static HWND pt_setup_grid(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = pt_grid_proc;
    wc.hInstance = s_inst;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.lpszClassName = L"SvR2011PaintGrid";
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    return s_pt_grid = add(TAB_PAINT, L"SvR2011PaintGrid", L"", WS_TABSTOP, X0, 86,
                           5 * (PT_CELL + PT_GAP), 4 * (PT_CELL + PT_GAP), ID_PT_GRID);
}

static int pick_image_file(WCHAR *out, int save, const WCHAR *name)
{
    IFileDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    static const COMDLG_FILTERSPEC png[] = { { L"PNG image", L"*.png" } };
    static const COMDLG_FILTERSPEC img[] = { { L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff" },
                                             { L"All files", L"*.*" } };
    if (FAILED(CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                save ? &IID_IFileSaveDialog : &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    if (save) {
        d->lpVtbl->SetFileTypes(d, 1, png);
        d->lpVtbl->SetDefaultExtension(d, L"png");
        d->lpVtbl->SetFileName(d, name);
        d->lpVtbl->SetTitle(d, L"Export the logo as a PNG");
    } else {
        d->lpVtbl->SetFileTypes(d, 2, img);
        d->lpVtbl->SetTitle(d, L"Choose an image for the logo");
    }
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

static int pt_need_slot(int used)
{
    if (!s_pt) {
        pt_refresh();
        if (!s_pt)
            return 0;
    }
    if (s_pt_sel < 0) {
        pt_status(L"Choose a logo in the grid first.");
        return 0;
    }
    if (used && !pt_used(s_pt, s_pt_sel)) {
        pt_status(L"Logo %d is empty.", s_pt_sel + 1);
        return 0;
    }
    return 1;
}

static void pt_export(void)
{
    WCHAR out[MAX_PATH], name[64];
    uint8_t *px;
    if (!pt_need_slot(1))
        return;
    pt_png_name(s_pt_page, s_pt_sel, name, 64);
    if (!pick_image_file(out, 1, name))
        return;
    px = (uint8_t *)malloc(PT_W * PT_W * 4);
    if (!px)
        return;
    pt_get(s_pt, s_pt_sel, px);
    if (png_write(out, px))
        pt_status(L"Page %d logo %d exported to %s.", s_pt_page + 1, s_pt_sel + 1, out);
    else
        pt_status(L"Could not write %s.", out);
    free(px);
}

/* Exports every logo of every page. */
static void pt_export_all(void)
{
    WCHAR dir[MAX_PATH], out[MAX_PATH], name[64], path[MAX_PATH], err[512];
    uint8_t *px;
    int k, page, n = 0;
    if (!s_pt) {
        pt_refresh();
        if (!s_pt)
            return;
    }
    if (!pt_path(path) || !pick_folder(L"Choose where to export the logos to", dir))
        return;
    px = (uint8_t *)malloc(PT_W * PT_W * 4);
    if (!px)
        return;
    for (page = 0; page < PT_PAGES; page++) {
        uint8_t *f = page == s_pt_page ? s_pt : pt_read_page(path, page, err, 512);
        for (k = 0; f && k < PT_SLOTS; k++) {
            if (!pt_used(f, k))
                continue;
            pt_png_name(page, k, name, 64);
            pt_get(f, k, px);
            if (join(out, dir, name) && png_write(out, px))
                n++;
        }
        if (f != s_pt)
            free(f);
    }
    free(px);
    pt_status(L"Exported %d logo%s to %s.", n, n == 1 ? L"" : L"s", dir);
}

/* Changes slot s_pt_sel (after a backup of the whole Saves folder). */
static void pt_change(int import)
{
    WCHAR img[MAX_PATH], path[MAX_PATH], before[MAX_PATH], err[512], t[512];
    uint8_t *px = NULL;
    if (saves_locked() || !pt_need_slot(!import) || !pt_path(path))
        return;
    if (import) {
        if (!pick_image_file(img, 0, NULL))
            return;
        px = (uint8_t *)malloc(PT_W * PT_W * 4);
        if (!px || !image_read(img, px, err, 512)) {
            pt_status(L"%s", px ? err : L"Out of memory.");
            free(px);
            return;
        }
        if (pt_used(s_pt, s_pt_sel)) {
            swprintf_s(t, 512, L"Replace logo %d of page %d with %s?", s_pt_sel + 1, s_pt_page + 1, img);
            if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                free(px);
                return;
            }
        }
    } else {
        swprintf_s(t, 512, L"Delete logo %d of page %d?", s_pt_sel + 1, s_pt_page + 1);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    if (!saves_backup_to(L" (before Paint Tool change)", before)) {
        pt_status(L"Could not back up the saves first; nothing was changed.");
        free(px);
        return;
    }
    if (import)
        pt_put(s_pt, s_pt_sel, px);
    else
        pt_clear(s_pt, s_pt_sel);
    free(px);
    if (!pt_write_page(path, s_pt_page, s_pt)) {
        pt_status(L"Could not write page %d; nothing was changed.", s_pt_page + 1);
        pt_refresh();
        return;
    }
    {
        int keep = s_pt_sel;
        pt_refresh();
        s_pt_sel = keep;
    }
    InvalidateRect(s_pt_grid, NULL, FALSE);
    if (import)
        pt_status(L"Page %d logo %d is now %s. (Your saves were backed up to %s.)", s_pt_page + 1, s_pt_sel + 1,
                  img, before);
    else
        pt_status(L"Page %d logo %d deleted. (Your saves were backed up to %s.)", s_pt_page + 1, s_pt_sel + 1,
                  before);
}

/* --paint-export <file.pt> <slot 1-200> <out.png> / --paint-import <file.pt> <slot> <image> (tests):
 * slots 21-40 are page 2's, and so on. */
static int pt_console(int import, const WCHAR *file, int slot, const WCHAR *img)
{
    WCHAR err[512];
    const int page = (slot - 1) / PT_SLOTS, k = (slot - 1) % PT_SLOTS;
    uint8_t *f = slot >= 1 && page < PT_PAGES ? pt_read_page(file, page, err, 512) : NULL;
    uint8_t *px = (uint8_t *)malloc(PT_W * PT_W * 4);
    int ok = 0;
    if (!f || !px) {
        con_print(slot < 1 || page >= PT_PAGES ? L"bad slot\n" : err);
        goto done;
    }
    if (import) {
        if (!image_read(img, px, err, 512)) { con_print(err); goto done; }
        pt_put(f, k, px);
        ok = pt_write_page(file, page, f);
    } else {
        pt_get(f, k, px);
        ok = pt_used(f, k) && png_write(img, px);
    }
    con_print(ok ? L"ok\n" : L"failed\n");
done:
    free(f);
    free(px);
    return ok ? 0 : 1;
}

/* ── Created Superstar import ─────────────────────────────────────────── */

/* Puts a logo given as palette + pixels (as a Created Superstar keeps it)
 * in slot k - the Paint Tool's own 8-bit picture is those exact bytes. */
static void pt_put_indexed(uint8_t *f, int k, const uint8_t *palette, const uint8_t *pixels)
{
    uint8_t *b = pt_slot(f, k);
    uint32_t *argb = (uint32_t *)malloc(PT_W * PT_W * 4);
    SYSTEMTIME st;
    int i;
    if (!argb)
        return;
    memcpy(b + 8, k_pt_used_header, 44);
    for (i = 0; i < PT_W * PT_W; i++) {
        argb[i] = rd32(palette + 4 * pixels[i]);
        wr32(b + PT_CANVAS + 4 * i, argb[i]);
    }
    memcpy(b + PT_TGA, k_pt_tga_header, 20);
    memcpy(b + PT_TGA + 20, palette, 1024);
    memcpy(b + PT_TGA + 0x414, pixels, PT_W * PT_W);
    pt_make_dds(b + PT_DDS, argb);
    GetSystemTime(&st);
    b[PT_STAMP] = (uint8_t)(st.wYear >> 8); b[PT_STAMP + 1] = (uint8_t)st.wYear;
    b[PT_STAMP + 2] = (uint8_t)st.wMonth; b[PT_STAMP + 3] = (uint8_t)st.wDay;
    b[PT_STAMP + 4] = (uint8_t)st.wHour; b[PT_STAMP + 5] = (uint8_t)st.wMinute;
    b[PT_STAMP + 6] = (uint8_t)st.wSecond; b[PT_STAMP + 7] = (uint8_t)(st.wDayOfWeek + 1);
    free(argb);
}

/* Logos into free Paint Tool slots (page 1 first), leaving out the ones the
 * Paint Tool has already (a slot whose checksum is the logo's id). */
static void caw_logos_to_paint(const WCHAR *saves, const CawLogo *logos, int n, int *added, int *had, int *no_room)
{
    WCHAR pt[MAX_PATH], err[512];
    uint8_t *pages[PT_PAGES] = { 0 };
    int dirty[PT_PAGES] = { 0 }, page, k, i, j;
    uint32_t *ids = (uint32_t *)malloc(sizeof(uint32_t) * PT_PAGES * PT_SLOTS);
    int n_ids = 0;
    *added = *had = *no_room = 0;
    if (!n || !ids || !join(pt, saves, PT_FILE) || !(pages[0] = pt_read(pt, err, 512))) {
        *no_room = n;  /* (no Paint Tool save yet) */
        free(ids);
        return;
    }
    for (page = 1; page < PT_PAGES; page++)
        pages[page] = pt_read_page(pt, page, err, 512);
    for (page = 0; page < PT_PAGES; page++)
        for (k = 0; pages[page] && k < PT_SLOTS; k++)
            if (pt_used(pages[page], k))
                ids[n_ids++] = rd32(pt_slot(pages[page], k) + PT_SUM);
    for (i = 0; i < n; i++) {
        int found = 0, placed = 0;
        for (j = 0; j < n_ids && !found; j++)
            found = logos[i].id && ids[j] == logos[i].id;
        for (page = 0; page < PT_PAGES && !found; page++)  /* (or the same picture under another id) */
            for (k = 0; pages[page] && k < PT_SLOTS && !found; k++) {
                const uint8_t *t = pt_slot(pages[page], k) + PT_TGA;
                found = pt_used(pages[page], k) && !memcmp(t + 20, logos[i].palette, 1024)
                     && !memcmp(t + 0x414, logos[i].pixels, PT_W * PT_W);
            }
        if (found) {
            ++*had;
            continue;
        }
        for (page = 0; page < PT_PAGES && !placed; page++)
            for (k = 0; pages[page] && k < PT_SLOTS && !placed; k++)
                if (!pt_used(pages[page], k)) {
                    pt_put_indexed(pages[page], k, logos[i].palette, logos[i].pixels);
                    dirty[page] = placed = 1;
                }
        if (placed)
            ++*added;
        else
            ++*no_room;
    }
    for (page = 0; page < PT_PAGES; page++) {
        if (dirty[page])
            pt_write_page(pt, page, pages[page]);
        free(pages[page]);
    }
    free(ids);
}

/* Imports Created Superstars (files 0-separated in `files`) into the saves
 * folder; a summary in `msg`. Returns how many went in. */
static int caw_import_files(const WCHAR *saves, const WCHAR *files, int n, WCHAR *msg, size_t msg_n)
{
    const WCHAR *p;
    WCHAR name[40], err[512], names[512] = L"", fail[600] = L"";
    int i, done = 0, logos_added = 0, logos_had = 0, logos_lost = 0;
    for (p = files, i = 0; i < n; i++, p += wcslen(p) + 1) {
        CawLogo *logos = NULL;
        int n_logos = 0, a = 0, h = 0, l = 0;
        const int slot = caw_import(p, saves, name, 40, &logos, &n_logos, err, 512);
        if (slot < 0) {
            if (!fail[0])
                wcscpy_s(fail, 600, err);
            continue;
        }
        caw_logos_to_paint(saves, logos, n_logos, &a, &h, &l);
        free(logos);
        logos_added += a, logos_had += h, logos_lost += l;
        if (wcslen(names) + wcslen(name) + 16 < 512)
            swprintf_s(names + wcslen(names), 512 - wcslen(names), L"%s%s (slot %d)", done ? L", " : L"", name,
                       slot + 1);
        done++;
    }
    swprintf_s(msg, msg_n, L"Imported %d Created Superstar%s: %s. Logos: %d added to the Paint Tool%s%s.%s%s", done,
               done == 1 ? L"" : L"s", done ? names : L"none", logos_added,
               logos_had ? L", some you had already" : L"",
               logos_lost ? L", some didn't fit (the Paint Tool is full, or not made yet)" : L"",
               fail[0] ? L" " : L"", fail);
    return done;
}

static void saves_import_caw(void)
{
    static WCHAR files[32768];
    WCHAR saves[MAX_PATH], before[MAX_PATH], msg[1400];
    int n;
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (saves_locked() || !saves_dir(saves))
        return;
    n = pick_save_files(files, 32768);
    if (!n)
        return;
    if (!saves_backup_to(L" (before Superstar import)", before)) {
        saves_status(L"Could not back up your saves first; nothing was imported.");
        return;
    }
    caw_import_files(saves, files, n, msg, 1400);
    saves_refresh();
    pt_refresh();
    saves_status(L"%s (Your saves were backed up first.)", msg);
}

static void set_busy(int busy)
{
    int ids[] = { ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_INSTALL, ID_GAMEDIR_CHANGE, ID_APK_CREATE, ID_ADB_INSTALL };
    int i;
    s_busy = busy;
    for (i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++)
        EnableWindow(ctl(ids[i]), !busy);
    EnableWindow(ctl(ID_CANCEL), busy);
    EnableWindow(ctl(ID_ANDROID_CANCEL), busy);
    if (busy)
        EnableWindow(ctl(ID_PLAY), FALSE);
    else
        refresh_play();
}

static void start_install(void)
{
    WCHAR t[MAX_PATH + 200];
    if (s_busy)
        return;
    GetWindowTextW(ctl(ID_IMAGE), s_job.image, MAX_PATH);
    GetWindowTextW(ctl(ID_TARGET), s_job.target, MAX_PATH);
    if (!s_job.image[0] || !file_exists(s_job.image)) {
        set_text(ID_INSTALL_STATUS, L"Choose your " GAME_TITLE L" disc image (ISO or XISO) first.");
        return;
    }
    if (!s_job.target[0]) {
        set_text(ID_INSTALL_STATUS, L"Choose the folder to install the game to.");
        return;
    }
    if (game_running()) {
        set_text(ID_INSTALL_STATUS, L"Close the game before installing it again.");
        return;
    }
    WritePrivateProfileStringW(L"Launcher", L"LastImage", s_job.image, s_launcher_ini);
    s_cancel = 0;
    set_busy(1);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
    swprintf_s(t, MAX_PATH + 200, L"Installing to %s\x2026", s_job.target);
    set_text(ID_INSTALL_STATUS, t);
    if (s_worker)
        CloseHandle(s_worker);
    s_worker = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (!s_worker) {
        set_busy(0);
        set_text(ID_INSTALL_STATUS, L"The installation could not be started.");
    }
}

/* ── Android tab (apk_package.h) ──────────────────────────────────────── */

static WCHAR s_apk_src[MAX_PATH], s_apk_out[MAX_PATH], s_adb[MAX_PATH];
static int s_android_job;  /* 0 package, 1 install to phone */

static void apk_progress(int permille, const WCHAR *msg)
{
    PostMessageW(s_wnd, WM_APP_APK, (WPARAM)permille, msg ? (LPARAM)wdup(msg) : 0);
}

static void apk_console_progress(int permille, const WCHAR *msg)   /* (--apk-package, --adb-install) */
{
    static int last = -100;
    if (msg && s_console) {
        con_print(msg);
        con_print(L"\n");
    }
    if (permille >= last + 100) {
        last = permille - permille % 100;
        wprintf(L"%d%%\n", permille / 10);
    }
}

/* adb.exe, or Google's platform tools downloaded beside the launcher. */
static int adb_get(apk_progress_fn progress, WCHAR *err, int errn)
{
    WCHAR tmp[MAX_PATH], dir[MAX_PATH];
    if (adb_find(s_launcher_dir, s_adb, MAX_PATH))
        return 1;
    progress(0, L"Downloading Google's Android platform tools (adb)\x2026");
    download_dir(dir);
    join(tmp, dir, L"svr2011-platform-tools.zip");
    if (!update_download(ADB_TOOLS_URL, tmp, NULL, 0, &s_cancel, err, (size_t)errn))
        return 0;
    if (!unzip_file(tmp, s_launcher_dir)) {
        DeleteFileW(tmp);
        swprintf_s(err, errn, L"Could not unpack the platform tools into %s.", s_launcher_dir);
        return 0;
    }
    DeleteFileW(tmp);
    if (!adb_find(s_launcher_dir, s_adb, MAX_PATH)) {
        swprintf_s(err, errn, L"The platform tools download has no adb.exe.");
        return 0;
    }
    return 1;
}

static DWORD WINAPI apk_thread(LPVOID unused)
{
    WCHAR err[600];
    int ok;
    (void)unused;
    err[0] = 0;
    if (s_android_job == 1)
        ok = adb_get(apk_progress, err, 600) &&
             adb_install(s_adb, s_game_dir, s_apk_src, &s_cancel, apk_progress, err, 600);
    else
        ok = apk_package(s_game_dir, s_apk_src, s_apk_out, &s_cancel, apk_progress, err, 600);
    PostMessageW(s_wnd, WM_APP_APK, ok ? APK_OK : APK_FAILED, (LPARAM)wdup(err));
    return 0;
}

/* The installed game and the release's APK, or a message. */
static int android_ready(void)
{
    WCHAR xex[MAX_PATH];
    if (s_busy)
        return 0;
    swprintf_s(xex, MAX_PATH, L"%s\\default.xex", s_game_dir);
    if (!s_game_dir[0] || !file_exists(xex)) {
        set_text(ID_APK_STATUS, L"Install the game on this PC first (Install tab): the phone gets a copy of it.");
        return 0;
    }
    if (!apk_find(s_game_dir, s_launcher_dir, s_apk_src, MAX_PATH)) {
        set_text(ID_APK_STATUS, L"This release has no Android app (Android\\SvR2011.apk beside the launcher).");
        return 0;
    }
    return 1;
}

static void start_android(int job)
{
    s_android_job = job;
    s_cancel = 0;
    set_busy(1);
    SendMessageW(ctl(ID_APK_PROGRESS), PBM_SETPOS, 0, 0);
    set_text(ID_APK_STATUS, L"Starting\x2026");
    if (s_worker)
        CloseHandle(s_worker);
    s_worker = CreateThread(NULL, 0, apk_thread, NULL, 0, NULL);
    if (!s_worker) {
        set_busy(0);
        set_text(ID_APK_STATUS, L"It could not be started.");
    }
}

static void start_apk(void)
{
    WCHAR dir[MAX_PATH];
    if (!android_ready())
        return;
    if (!pick_folder(L"Choose where to put the Android package (about the size of the game)", dir))
        return;
    swprintf_s(s_apk_out, MAX_PATH, L"%s\\SvR2011 Android", dir);
    start_android(0);
}

static void start_adb(void)
{
    if (!android_ready())
        return;
    if (!adb_find(s_launcher_dir, s_adb, MAX_PATH) &&
        MessageBoxW(s_wnd,
                    L"Installing over USB uses adb, Google's Android platform tool (about 7 MB), which this "
                    L"PC doesn't have yet.\n\nDownload it from Google now? It is put in the launcher's "
                    L"platform-tools folder. By downloading it you agree to the Android SDK terms:\n"
                    L"https://developer.android.com/studio/terms",
                    WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    start_android(1);
}

static void apk_message(WPARAM wp, WCHAR *msg)
{
    WCHAR t[800];
    if (wp == APK_OK) {
        set_busy(0);
        SendMessageW(ctl(ID_APK_PROGRESS), PBM_SETPOS, 1000, 0);
        if (s_android_job == 1) {
            set_text(ID_APK_STATUS, msg && !wcscmp(msg, L"updated")
                ? L"Done: the game on the phone is up to date, and SvR 2011 is starting there."
                : L"Done: SvR 2011 is installed on the phone and starting there. You can unplug it.");
        } else {
            swprintf_s(t, 800, L"Done: %s. Copy its files to the phone (see \"Install on Android.txt\").", s_apk_out);
            set_text(ID_APK_STATUS, t);
            ShellExecuteW(s_wnd, L"open", s_apk_out, NULL, NULL, SW_SHOWNORMAL);
        }
    } else if (wp == APK_FAILED) {
        set_busy(0);
        SendMessageW(ctl(ID_APK_PROGRESS), PBM_SETPOS, 0, 0);
        swprintf_s(t, 800, L"%s %s", s_android_job == 1 ? L"Not installed." : L"The package was not made.",
                   msg ? msg : L"");
        set_text(ID_APK_STATUS, t);
    } else {
        SendMessageW(ctl(ID_APK_PROGRESS), PBM_SETPOS, wp, 0);
        if (msg && !s_cancel)
            set_text(ID_APK_STATUS, msg);
    }
    free(msg);
}

static void check_image(const WCHAR *p)
{
    Disc d;
    WCHAR err[512];
    s_image_bytes = 0;
    if (!disc_check(&d, p, err, 512)) {
        set_text(ID_INSTALL_STATUS, err);
    } else {
        WCHAR t[160], a[32];
        s_image_bytes = d.total;
        fmt_size(a, 32, d.total);
        swprintf_s(t, 160, GAME_TITLE L" disc image found: %d files, %s.", d.nfiles, a);
        set_text(ID_INSTALL_STATUS, t);
    }
    disc_close(&d);
    update_free_space();
}

/* ── Movies tab: USER MOVIES (movie_maker.c) ── */

static MovieJob s_mv_job;
static volatile LONG s_mv_busy;
static uint8_t *s_mv_preview;      /* 320 x 320 BGRA, or NULL */
static HWND s_mv_view;
static const int s_mv_lengths[] = { 180, 30, 60, 90 };

static int mv_folder(WCHAR *out)
{
    if (!s_game_dir[0] || !dir_exists(s_game_dir) || !join(out, s_game_dir, L"Custom Movies"))
        return 0;
    CreateDirectoryW(out, NULL);
    return 1;
}

static void mv_status(const WCHAR *fmt, ...)
{
    WCHAR buf[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 600, fmt, ap);
    va_end(ap);
    set_text(ID_MV_STATUS, buf);
}

static void mv_refresh(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], line[MAX_PATH + 32];
    HWND list = ctl(ID_MV_LIST);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (!mv_folder(dir) || !join(pat, dir, L"*.*"))
        return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR size[32], *dot, mp4[MAX_PATH];
        const WCHAR *ext = wcsrchr(fd.cFileName, L'.');
        /* (a movie is <name>.mp4 or <name>.bik; a .bik beside an .mp4 is its stub or fallback: one row) */
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ext || (_wcsicmp(ext, L".bik") && _wcsicmp(ext, L".mp4")))
            continue;
        if (!_wcsicmp(ext, L".bik") && join(mp4, dir, fd.cFileName) && (wcscpy_s(wcsrchr(mp4, L'.'), 5, L".mp4"), file_exists(mp4)))
            continue;
        fmt_size(size, 32, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        wcscpy_s(line, MAX_PATH + 32, fd.cFileName);
        dot = wcsrchr(line, L'.');
        if (dot) *dot = 0;
        wcscat_s(line, MAX_PATH + 32, L"   (");
        wcscat_s(line, MAX_PATH + 32, size);
        wcscat_s(line, MAX_PATH + 32, L")");
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int mv_fit(void)
{
    if (IsDlgButtonChecked(s_wnd, ID_MV_FILL) == BST_CHECKED) return MOVIE_FILL;
    if (IsDlgButtonChecked(s_wnd, ID_MV_STRETCH) == BST_CHECKED) return MOVIE_STRETCH;
    return MOVIE_FIT;
}

static int mv_pick(WCHAR *out, int bottom)
{
    IFileDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    static const COMDLG_FILTERSPEC video[] = {
        { L"Videos and pictures", L"*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.ts;*.bik;*.png;*.jpg;*.jpeg;*.bmp;*.gif" },
        { L"All files", L"*.*" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetFileTypes(d, 2, video);
    d->lpVtbl->SetTitle(d, bottom ? L"Choose a picture or video for the bottom strip" : L"Choose the video");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* The game's titantron movies (movies\titantron\<id>.bik) and whose they
 * are, as CREATE AN ENTRANCE lists them; the DLC superstars' movies are in
 * the installed DLC. Their bottom strips can be reused (Superstar...). */
static const struct { int id; const WCHAR *name; } k_star_movies[] = {
    { 297, L"ALICIA FOX" },
    { 160, L"BATISTA" },
    { 224, L"BETH PHOENIX" },
    { 125, L"BIG SHOW" },
    { 294, L"BRIE BELLA" },
    { 137, L"CHAVO GUERRERO" },
    { 104, L"CHRIS JERICHO" },
    { 276, L"CHRISTIAN" },
    { 902, L"CHRISTIAN OLD" },
    { 131, L"CM PUNK" },
    { 205, L"CODY RHODES" },
    { 424, L"CRYME TYME" },
    { 281, L"DAVID HART SMITH" },
    { 407, L"D-GENERATION X" },
    { 275, L"DOLPH ZIGGLER" },
    { 278, L"DREW McINTYRE" },
    { 110, L"EDGE" },
    { 423, L"EDGE & CHRISTIAN" },
    { 901, L"EDGE OLD" },
    { 265, L"EVAN BOURNE" },
    { 292, L"EVE" },
    { 268, L"EZEKIEL JACKSON" },
    { 158, L"FINLAY" },
    { 293, L"GAIL KIM" },
    { 277, L"GOLDUST" },
    { 401, L"THE HART DYNASTY" },
    { 267, L"JACK SWAGGER" },
    { 132, L"JAKE ROBERTS" },
    { 122, L"JIMMY SNUKA" },
    { 139, L"JOHN CENA" },
    { 175, L"JOHN MORRISON" },
    { 165, L"JTG" },
    { 107, L"KANE" },
    { 903, L"MASKED KANE" },
    { 164, L"KELLY KELLY" },
    { 176, L"KOFI KINGSTON" },
    { 321, L"LUKE GALLOWS" },
    { 179, L"MARK HENRY" },
    { 291, L"MARYSE" },
    { 112, L"MATT HARDY" },
    { 174, L"MELINA" },
    { 182, L"MICHELLE McCOOL" },
    { 143, L"MICKIE JAMES" },
    { 263, L"MIKE KNOX" },
    { 218, L"THE MIZ" },
    { 115, L"MR. McMAHON" },
    { 135, L"MVP" },
    { 295, L"NIKKI BELLA" },
    { 271, L"PRIMO" },
    { 161, L"RANDY ORTON" },
    { 123, L"REY MYSTERIO" },
    { 305, L"RICKY STEAMBOAT" },
    { 306, L"ROB VAN DAM" },
    { 100, L"THE ROCK" },
    { 272, L"R-TRUTH" },
    { 201, L"SANTINO MARELLA" },
    { 178, L"SHAD GASPARD" },
    { 145, L"SHAWN MICHAELS" },
    { 279, L"SHEAMUS" },
    { 208, L"SHELTON BENJAMIN" },
    { 422, L"SHOWMIZ" },
    { 101, L"STONE COLD" },
    { 131, L"THE STRAIGHT EDGE SOCIETY" },
    { 261, L"TED DIBIASE" },
    { 169, L"TERRY FUNK" },
    { 186, L"THEODORE LONG" },
    { 102, L"TRIPLE H" },
    { 103, L"UNDERTAKER" },
    { 284, L"VANCE ARCHER" },
    { 262, L"VLADIMIR KOZLOV" },
    { 117, L"WILLIAM REGAL" },
    { 283, L"YOSHI TATSU" },
    { 219, L"ZACK RYDER" },
    { 50, L"CHRIS MASTERS" },
    { 51, L"LEX LUGER" },
    { 52, L"BRITISH BULLDOG" },
    { 53, L"JUSTIN GABRIEL" },
    { 54, L"DAVID OTUNGA" },
    { 55, L"WADE BARRETT" },
    { 56, L"LAYLA" },
    { 255, L"WWE LOGO" },
    { 256, L"LEGENDS LOGO" }
};

/* The movie of k_star_movies[i]: in the game folder or in the installed DLC. */
static int mv_star_path(int i, WCHAR *out)
{
    WCHAR file[32], p[MAX_PATH], pat[MAX_PATH], dlc[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    swprintf_s(file, 32, L"%03d.bik", k_star_movies[i].id);
    swprintf_s(p, MAX_PATH, L"%s\\movies\\titantron\\%s", s_game_dir, file);
    if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); return 1; }
    swprintf_s(dlc, MAX_PATH, L"%s\\UserData\\0000000000000000\\5451085D\\00000002", s_game_dir);
    swprintf_s(pat, MAX_PATH, L"%s\\*", dlc);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        swprintf_s(p, MAX_PATH, L"%s\\%s\\movies\\titantron\\%s", dlc, fd.cFileName, file);
        if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); found = 1; }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void mv_preview(void);

/* A superstar's strip picked with Superstar...: the box shows its name. */
static WCHAR s_mv_star_label[96], s_mv_star_path[MAX_PATH];

/* The bottom strip's file: what the box says, or the superstar's movie. */
static void mv_bottom_path(WCHAR *out)
{
    GetWindowTextW(ctl(ID_MV_BOTTOM), out, MAX_PATH);
    if (s_mv_star_label[0] && !wcscmp(out, s_mv_star_label)) wcscpy_s(out, MAX_PATH, s_mv_star_path);
}

/* Uses k_star_movies[i]'s bottom strip. */
static void mv_star_pick(int i)
{
    WCHAR p[MAX_PATH];
    int found = 0, k;
    if (!mv_star_path(i, s_mv_star_path)) return;
    for (k = 0; k < (int)(sizeof k_star_movies / sizeof *k_star_movies); k++) found += mv_star_path(k, p);
    swprintf_s(s_mv_star_label, 96, L"%s (superstar's strip)", k_star_movies[i].name);
    set_text(ID_MV_BOTTOM, s_mv_star_label);
    mv_preview();
    if (s_mv_preview)   /* (else the preview's error stays) */
        mv_status(L"Bottom strip: %s's, from the game's movie (%d superstar movies found). It loops if your video "
                  L"is longer.", k_star_movies[i].name, found);
}

/* Superstar...: a menu of the superstars whose movies are installed; the
 * pick's bottom strip becomes this movie's strip. */
static void mv_star_menu(void)
{
    enum { FIRST = 1, PER_COLUMN = 24 };
    HMENU m = CreatePopupMenu();
    RECT rc;
    WCHAR p[MAX_PATH];
    int i, n = 0, cmd;
    if (!m) return;
    for (i = 0; i < (int)(sizeof k_star_movies / sizeof *k_star_movies); i++) {
        if (!mv_star_path(i, p)) continue;
        AppendMenuW(m, MF_STRING | (n && n % PER_COLUMN == 0 ? MF_MENUBARBREAK : 0), (UINT_PTR)(FIRST + i),
                    k_star_movies[i].name);
        n++;
    }
    if (!n) {
        DestroyMenu(m);
        mv_status(L"No superstar movies found - install the game on the Install tab first.");
        return;
    }
    GetWindowRect(ctl(ID_MV_BOTTOM_STAR), &rc);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, rc.left, rc.bottom, 0,
                              s_wnd, NULL);
    DestroyMenu(m);
    if (cmd >= FIRST) mv_star_pick(cmd - FIRST);
}

/* The movie's name as the game will list it: letters, digits, spaces, - and _
 * (accents taken off: the game skips names that aren't plain ASCII). */
static int mv_name(WCHAR *name, size_t n)
{
    WCHAR raw[64], *s;
    size_t k = 0;
    GetWindowTextW(ctl(ID_MV_NAME), raw, 64);
    for (s = raw; *s && k + 1 < n && k < 32; s++) {
        WCHAR c = ascii_fold_char(*s);
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L' ' || c == L'-' || c == L'_')
            name[k++] = c;
    }
    while (k && name[k - 1] == L' ') k--;
    name[k] = 0;
    s = name;
    while (*s == L' ') s++;
    if (s != name) memmove(name, s, (wcslen(s) + 1) * sizeof(WCHAR));
    return name[0] != 0;
}

static void mv_preview(void)
{
    WCHAR video[MAX_PATH], bottom[MAX_PATH], err[512];
    HCURSOR old;
    GetWindowTextW(ctl(ID_MV_VIDEO), video, MAX_PATH);
    mv_bottom_path(bottom);
    if (!video[0]) { mv_status(L"Choose a video first."); return; }
    if (!s_mv_preview) s_mv_preview = (uint8_t *)malloc(MOVIE_W * MOVIE_H * 4);
    if (!s_mv_preview) return;
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (movie_preview(video, bottom, mv_fit(), 2.0, s_mv_preview, err, 512))
        mv_status(L"The movie as the game stores it: the big screen on top (the game shows it wider, at 16:9), "
                  L"the stage and ramp strip below.");
    else {
        free(s_mv_preview); s_mv_preview = NULL;
        mv_status(L"%s", err);
    }
    SetCursor(old);
    InvalidateRect(s_mv_view, NULL, FALSE);
}

static DWORD WINAPI mv_thread(LPVOID arg)
{
    int ok;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok = movie_make(&s_mv_job);
    CoUninitialize();
    PostMessageW(s_wnd, WM_APP_MOVIE_DONE, (WPARAM)ok, 0);
    return 0;
}

static void mv_create(void)
{
    WCHAR dir[MAX_PATH], name[40], file[64], video[MAX_PATH];
    int len;
    if (s_mv_busy) return;
    GetWindowTextW(ctl(ID_MV_VIDEO), video, MAX_PATH);
    if (!video[0] || !file_exists(video)) { mv_status(L"Choose the video first."); return; }
    if (!mv_name(name, 40)) { mv_status(L"Give the movie a name (letters and digits); the game lists it by that name."); return; }
    if (!mv_folder(dir)) { mv_status(L"The game folder does not exist; choose it on the Play tab first."); return; }
    /* (an .mp4: the game plays it as an HD movie, a tenth of a .bik's size; movie_make writes the
     * .bik where Windows can't encode H.264) */
    swprintf_s(file, 64, L"%s.bik", name);
    memset(&s_mv_job, 0, sizeof s_mv_job);
    if (!join(s_mv_job.out, dir, file)) return;
    {
        const int had_bik = file_exists(s_mv_job.out);
        wcscpy_s(wcsrchr(s_mv_job.out, L'.'), 5, L".mp4");
        if (had_bik || file_exists(s_mv_job.out)) {
        WCHAR q[200];
        swprintf_s(q, 200, L"A movie named %s already exists. Replace it?", name);
        if (MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        }
    }
    wcscpy_s(s_mv_job.video, MAX_PATH, video);
    mv_bottom_path(s_mv_job.bottom);
    s_mv_job.fit = mv_fit();
    len = (int)SendMessageW(ctl(ID_MV_LENGTH), CB_GETCURSEL, 0, 0);
    s_mv_job.max_seconds = len >= 0 && len < 4 ? s_mv_lengths[len] : 180;
    s_mv_job.notify = s_wnd;
    s_mv_job.msg = WM_APP_MOVIE;
    InterlockedExchange(&s_mv_busy, 1);
    EnableWindow(ctl(ID_MV_CREATE), FALSE);
    EnableWindow(ctl(ID_MV_STOP), TRUE);
    SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 0, 0);
    mv_status(L"Making %s\x2026", name);
    CloseHandle(CreateThread(NULL, 0, mv_thread, NULL, 0, NULL));
}

static void mv_done(int ok)
{
    InterlockedExchange(&s_mv_busy, 0);
    EnableWindow(ctl(ID_MV_CREATE), TRUE);
    EnableWindow(ctl(ID_MV_STOP), FALSE);
    if (ok) {
        const WCHAR *slash = wcsrchr(s_mv_job.out, L'\\');
        WCHAR other[MAX_PATH];
        /* (the replaced movie's other half: an older <name>.bik beside the new .mp4 would be taken as
         * its fallback - or it is the game's stub, made again; an older .mp4 beside a new .bik would win) */
        wcscpy_s(other, MAX_PATH, s_mv_job.out);
        wcscpy_s(wcsrchr(other, L'.'), 5, s_mv_job.wrote_bik ? L".mp4" : L".bik");
        DeleteFileW(other);
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 100, 0);
        mv_status(L"Made %s (%d s). In the game: CREATE AN ENTRANCE \x2192 FINALIZE \x2192 MOVIE, after NONE.",
                  slash ? slash + 1 : s_mv_job.out, (s_mv_job.frames + 29) / 30);
    } else {
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 0, 0);
        mv_status(L"%s", s_mv_job.err[0] ? s_mv_job.err : L"The movie could not be made.");
    }
    mv_refresh();
}

static void mv_delete(void)
{
    WCHAR dir[MAX_PATH], line[MAX_PATH + 32], path[MAX_PATH], q[300], *cut;
    int sel = (int)SendMessageW(ctl(ID_MV_LIST), LB_GETCURSEL, 0, 0);
    if (sel < 0) { mv_status(L"Choose a movie in the list first."); return; }
    SendMessageW(ctl(ID_MV_LIST), LB_GETTEXT, (WPARAM)sel, (LPARAM)line);
    cut = wcsstr(line, L"   (");
    if (cut) *cut = 0;
    swprintf_s(q, 300, L"Delete the movie %s? Entrances that use it show no movie.", line);
    if (!mv_folder(dir) || MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    {
        /* (<name>.mp4 and / or <name>.bik - the .bik beside an .mp4 is its stub or fallback) */
        int gone = 0, failed = 0;
        static const WCHAR *const exts[] = { L".mp4", L".bik" };
        int e;
        for (e = 0; e < 2; e++) {
            WCHAR f[MAX_PATH + 32];
            swprintf_s(f, MAX_PATH + 32, L"%s%s", line, exts[e]);
            if (!join(path, dir, f) || !file_exists(path)) continue;
            if (shell_op(FO_DELETE, path, NULL, FOF_ALLOWUNDO)) gone++; else failed++;
        }
        if (gone && !failed)
            mv_status(L"Deleted %s (it is in the Recycle Bin).", line);
        else
            mv_status(L"Could not delete %s (is the game using it?).", line);
    }
    mv_refresh();
}

/* After choosing a video: its file name as the movie's name, if none yet. */
static void mv_name_from(const WCHAR *path)
{
    WCHAR name[40], n[40], *dot;
    const WCHAR *b = wcsrchr(path, L'\\');
    GetWindowTextW(ctl(ID_MV_NAME), name, 40);
    if (name[0]) return;
    wcsncpy_s(n, 40, b ? b + 1 : path, _TRUNCATE);
    dot = wcsrchr(n, L'.');
    if (dot) *dot = 0;
    set_text(ID_MV_NAME, n);
}

/* WM_COMMAND for the Movies tab; 1 if handled. */
static int mv_command(int id, int code)
{
    WCHAR f[MAX_PATH];
    switch (id) {
    case ID_MV_VIDEO_BROWSE:
        if (mv_pick(f, 0)) { set_text(ID_MV_VIDEO, f); mv_name_from(f); mv_preview(); }
        return 1;
    case ID_MV_BOTTOM_BROWSE:
        if (mv_pick(f, 1)) { set_text(ID_MV_BOTTOM, f); mv_preview(); }
        return 1;
    case ID_MV_BOTTOM_STAR:
        mv_star_menu();
        return 1;
    case ID_MV_BOTTOM_NONE:
        set_text(ID_MV_BOTTOM, L"");
        if (s_mv_preview) mv_preview();
        return 1;
    case ID_MV_FIT: case ID_MV_FILL: case ID_MV_STRETCH:
        if (code == BN_CLICKED && s_mv_preview) mv_preview();
        return 1;
    case ID_MV_PREVIEW: mv_preview(); return 1;
    case ID_MV_CREATE:  mv_create();  return 1;
    case ID_MV_STOP:
        if (s_mv_busy) { InterlockedExchange(&s_mv_job.cancel, 1); mv_status(L"Stopping\x2026"); }
        return 1;
    case ID_MV_DELETE:  mv_delete();  return 1;
    case ID_MV_OPEN:
        if (mv_folder(f)) ShellExecuteW(s_wnd, L"open", f, NULL, NULL, SW_SHOWNORMAL);
        else mv_status(L"The game folder does not exist; choose it on the Play tab first.");
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK mv_view_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        GetClientRect(w, &rc);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (s_mv_preview) {
            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = MOVIE_W;
            bi.bmiHeader.biHeight = -MOVIE_H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, MOVIE_W, MOVIE_H, s_mv_preview, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(170, 170, 176));
            SelectObject(dc, s_font);
            DrawTextW(dc, L"Preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(w, m, wp, lp);
}

static void mv_setup(void)
{
    WNDCLASSW wc;
    HWND h;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = mv_view_proc;
    wc.hInstance = s_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"SvR2011MoviePreview";
    RegisterClassW(&wc);
    add(TAB_MOVIES, L"Static", L"Entrance movies of your own, in CREATE AN ENTRANCE \x2192 FINALIZE \x2192 MOVIE "
                               L"\x2192 USER MOVIES. The video plays on the big screen, the strip on the stage and ramp.",
        SS_LEFT, X0, 50, 560, 40, 0);
    add(TAB_MOVIES, L"Static", L"Video", SS_LEFT, X0, 100, 100, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 110, 96, 336, 24, ID_MV_VIDEO);
    add(TAB_MOVIES, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 95, 106, 26, ID_MV_VIDEO_BROWSE);
    add(TAB_MOVIES, L"Static", L"Bottom strip", SS_LEFT, X0, 134, 100, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 110, 130, 146, 24, ID_MV_BOTTOM);
    add(TAB_MOVIES, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 262, 129, 92, 26, ID_MV_BOTTOM_BROWSE);
    add(TAB_MOVIES, L"Button", L"Superstar\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 360, 129, 100, 26, ID_MV_BOTTOM_STAR);
    add(TAB_MOVIES, L"Button", L"None (black)", BS_PUSHBUTTON | WS_TABSTOP, X0 + 466, 129, 94, 26, ID_MV_BOTTOM_NONE);
    add(TAB_MOVIES, L"Static", L"Big screen", SS_LEFT, X0, 168, 100, 20, 0);
    /* Fill by default: the video covers the whole big screen (black bars in
       the video itself are cut off first - movie_maker.c content_find). */
    h = add(TAB_MOVIES, L"Button", L"Fill the screen", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, X0 + 110, 164, 140, 24, ID_MV_FILL);
    SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    add(TAB_MOVIES, L"Button", L"Whole picture (bars)", BS_AUTORADIOBUTTON, X0 + 256, 164, 160, 24, ID_MV_FIT);
    add(TAB_MOVIES, L"Button", L"Stretch", BS_AUTORADIOBUTTON, X0 + 422, 164, 100, 24, ID_MV_STRETCH);
    add(TAB_MOVIES, L"Static", L"Length", SS_LEFT, X0, 202, 100, 20, 0);
    h = add(TAB_MOVIES, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, X0 + 110, 198, 190, 200, ID_MV_LENGTH);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Whole video (up to 3 min)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 30 seconds");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 60 seconds");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 90 seconds");
    SendMessageW(h, CB_SETCURSEL, 1, 0);
    add(TAB_MOVIES, L"Static", L"Name", SS_LEFT, X0 + 318, 202, 50, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 370, 198, 190, 24, ID_MV_NAME);
    s_mv_view = add(TAB_MOVIES, L"SvR2011MoviePreview", L"", 0, X0, 238, 200, 200, ID_MV_VIEW);
    add(TAB_MOVIES, L"Static", L"Your movies", SS_LEFT, X0 + 220, 238, 200, 20, 0);
    add(TAB_MOVIES, L"ListBox", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP | LBS_NOINTEGRALHEIGHT,
        X0 + 220, 260, 340, 138, ID_MV_LIST);
    add(TAB_MOVIES, L"Button", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, X0 + 220, 406, 110, 30, ID_MV_DELETE);
    add(TAB_MOVIES, L"Button", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 340, 406, 120, 30, ID_MV_OPEN);
    add(TAB_MOVIES, L"Button", L"Preview", BS_PUSHBUTTON | WS_TABSTOP, X0, 452, 110, 32, ID_MV_PREVIEW);
    add(TAB_MOVIES, L"Button", L"Make movie", BS_DEFPUSHBUTTON | WS_TABSTOP, X0 + 120, 452, 140, 32, ID_MV_CREATE);
    add(TAB_MOVIES, L"Button", L"Stop", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, X0 + 270, 452, 90, 32, ID_MV_STOP);
    add(TAB_MOVIES, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 494, 560, 18, ID_MV_PROGRESS);
    add(TAB_MOVIES, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 520, 560, 44, ID_MV_STATUS);
    {   /* tests (with --capture movies): SVR2011_MOVIE_VIDEO / _BOTTOM fill the fields and show a preview */
        WCHAR v[MAX_PATH];
        if (GetEnvironmentVariableW(L"SVR2011_MOVIE_VIDEO", v, MAX_PATH)) {
            set_text(ID_MV_VIDEO, v);
            if (GetEnvironmentVariableW(L"SVR2011_MOVIE_BOTTOM", v, MAX_PATH)) set_text(ID_MV_BOTTOM, v);
            mv_preview();
            if (GetEnvironmentVariableW(L"SVR2011_MOVIE_STAR", v, MAX_PATH)) {   /* as picked with Superstar... */
                int i;
                for (i = 0; i < (int)(sizeof k_star_movies / sizeof *k_star_movies); i++)
                    if (!_wcsicmp(k_star_movies[i].name, v)) mv_star_pick(i);
            }
        }
    }
}

/* ── updates (updater.c): check GitHub, download, copy the new files in ── */

static UpdateInfo s_up;
static volatile LONG s_up_cancel;
static int s_up_available, s_up_auto_run;

static void up_status(const WCHAR *fmt, ...)
{
    WCHAR buf[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 600, fmt, ap);
    va_end(ap);
    set_text(ID_UP_STATUS, buf);
    update_log(UPLOG_DIR, "launcher", L"%s", buf);   /* (the update row's messages: Report a problem has them) */
}

static DWORD WINAPI up_check_thread(LPVOID arg)
{
    WCHAR err[512] = L"";
    UpdateInfo info;
    int ok = update_check(&info, err, 512);
    (void)arg;
    if (ok) s_up = info;
    PostMessageW(s_wnd, WM_APP_UPD_CHECKED, (WPARAM)ok, ok ? 0 : (LPARAM)wdup(err));
    return 0;
}

static void up_check(int automatic)
{
    if (InterlockedCompareExchange(&s_up_busy, 1, 0))
        return;
    s_up_auto_run = automatic;
    up_status(L"Checking for updates\x2026");
    EnableWindow(ctl(ID_UP_BUTTON), FALSE);
    CloseHandle(CreateThread(NULL, 0, up_check_thread, NULL, 0, NULL));
}

/* ── finishing an update an older launcher made ───────────────────────────
 * Launchers up to 2.0.3 copied only the release's top-level files and its
 * native_shaders: after updating from one, the game folder lacks the
 * release's other folders (Bundled Mods, pad_icons). Started with --updated
 * (as the old launcher restarts the new one), the launcher fetches its own
 * release again and copies the folders in. */
static int up_finish_run(HWND notify, WCHAR *err, size_t errn)
{
    WCHAR tmp[MAX_PATH], work[MAX_PATH], zip[MAX_PATH], files[MAX_PATH];
    UpdateInfo info;
    int ok = 0;
    download_dir(tmp);
    work[0] = 0;
    if (!update_check_version(PORT_VERSION, &info, err, errn))
        goto done;
    if (!join(work, tmp, L"svr2011-update-finish") || !join(zip, work, L"release.zip") || !join(files, work, L"files"))
        goto done;
    remove_tree(work);
    CreateDirectoryW(work, NULL);
    CreateDirectoryW(files, NULL);
    if (!update_download(info.url, zip, notify, WM_APP_UPD_PROGRESS, &s_up_cancel, err, errn))
        goto done;
    if (!unpack(zip, files)) {
        swprintf_s(err, errn, L"Could not unpack the release.");
        goto done;
    }
    ok = copy_folders(files, s_game_dir);
    if (!same_dir(s_game_dir, s_launcher_dir))
        copy_folders(files, s_launcher_dir);
    if (!ok)
        swprintf_s(err, errn, L"Some files could not be copied. Is the game running?");
done:
    if (work[0])
        remove_tree(work);
    return ok;
}

static DWORD WINAPI up_finish_thread(LPVOID arg)
{
    WCHAR err[600] = L"";
    const int ok = up_finish_run(s_wnd, err, 600);
    (void)arg;
    PostMessageW(s_wnd, WM_APP_UPD_FINISHED, (WPARAM)ok, ok ? 0 : (LPARAM)wdup(err[0] ? err : L"failed"));
    return 0;
}

/* Whether the game folder lacks what a release brings beside the programs. */
static int up_unfinished(void)
{
    WCHAR p[MAX_PATH];
    if (!is_game_folder(s_game_dir))
        return 0;
    return (join(p, s_game_dir, L"Bundled Mods") && !dir_exists(p)) ||
           (join(p, s_game_dir, L"pad_icons") && !dir_exists(p));
}

static void up_finish(void)
{
    if (InterlockedCompareExchange(&s_up_busy, 1, 0))
        return;
    InterlockedExchange(&s_up_cancel, 0);
    EnableWindow(ctl(ID_UP_BUTTON), FALSE);
    ShowWindow(ctl(ID_UP_PROGRESS), SW_SHOW);
    SendMessageW(ctl(ID_UP_PROGRESS), PBM_SETPOS, 0, 0);
    up_status(L"Finishing the update: downloading the bundled mods and button pictures\x2026");
    s_up_finishing = 1;
    CloseHandle(CreateThread(NULL, 0, up_finish_thread, NULL, 0, NULL));
}

static void up_finished(int ok, WCHAR *err)
{
    ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    InterlockedExchange(&s_up_busy, 0);
    EnableWindow(ctl(ID_UP_BUTTON), TRUE);
    s_up_finishing = 0;
    if (!ok) {
        const int pending = s_play_pending;
        up_status(L"The update is done, but its mods could not be fetched (%s). Updating again later adds them.",
                  err ? err : L"");
        free(err);
        s_play_pending = 0;
        refresh_play();
        if (pending) play_with(pending == 2);  /* (the click made meanwhile) */
        return;
    }
    start_bundled();  /* (on a worker: the Play page says so; the Mods tab is rescanned when they're in) */
    up_status(L"Updated to version %s, with its bundled mods.", PORT_VERSION);
}

/* Starts "SvR2011 Updater.exe" (beside the launcher) to download and install a release - `version` that
 * release ("Finish the update"), NULL the newest - and closes the launcher: the updater replaces the program
 * files and starts the launcher again. The launcher never swaps its own files. 1 when it was started. */
static int up_run_updater(const WCHAR *version, int repair)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR exe[MAX_PATH], cmd[3 * MAX_PATH + 64];
    if (!join(exe, s_launcher_dir, UPDATER_EXE) || !file_exists(exe)) {
        up_status(L"%s is missing beside the launcher: download the newest release from GitHub instead.", UPDATER_EXE);
        return 0;
    }
    swprintf_s(cmd, 3 * MAX_PATH + 64, L"\"%s\" %s \"%s\" \"%s\"%s%s", exe, repair ? L"--repair" : L"--apply", s_game_dir[0] ? s_game_dir : s_launcher_dir,
               s_launcher_dir, version ? L" --version " : L"", version ? version : L"");
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_launcher_dir, &si, &pi)) {
        up_status(L"Could not start %s.", UPDATER_EXE);
        return 0;
    }
    update_log(UPLOG_DIR, "launcher", L"started the updater: %s%s%s", repair ? L"repair" : L"update",
               version ? L" to " : L"", version ? version : L"");
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    save_launcher_ini();
    DestroyWindow(s_wnd);
    return 1;
}

/* The updater's own new copy, which it couldn't put in place while it ran (SvR2011 Updater.exe.new). */
static void up_place_new_updater(void)
{
    WCHAR exe[MAX_PATH], nw[MAX_PATH + 8];
    if (!join(exe, s_launcher_dir, UPDATER_EXE))
        return;
    swprintf_s(nw, MAX_PATH + 8, L"%s.new", exe);
    if (file_exists(nw))
        MoveFileExW(nw, exe, MOVEFILE_REPLACE_EXISTING);
}

/* ── an update that didn't finish ─────────────────────────────────────────
 * The game's version (its exe's version information, 2.1.3 on; older ones say 1.0) against the launcher's:
 * another one means the last update replaced the launcher but not the game. "Finish the update" copies the
 * release in again - from the zip kept in Tools\.downloads, else downloaded again. */
static int s_up_finish;
static WCHAR s_up_game_version[32];

static int up_game_version(WCHAR *out, size_t n)
{
    WCHAR exe[MAX_PATH];
    DWORD h = 0, sz;
    void *buf;
    VS_FIXEDFILEINFO *fi;
    UINT fl;
    int ok = 0;
    out[0] = 0;
    if (!s_game_dir[0] || !join(exe, s_game_dir, GAME_EXE) || !file_exists(exe) ||
        !(sz = GetFileVersionInfoSizeW(exe, &h)) || !(buf = malloc(sz)))
        return 0;
    if (GetFileVersionInfoW(exe, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &fl) && fi) {
        /* (the fourth part only when it is set: 2.1.4.1, but 2.1.4 - as PORT_VERSION writes them) */
        if (LOWORD(fi->dwFileVersionLS))
            swprintf_s(out, n, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        else
            swprintf_s(out, n, L"%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS));
        ok = 1;
    }
    free(buf);
    return ok;
}

static WCHAR s_up_finish_msg[400], s_up_finish_btn[40];
static int s_repair_failed;   /* (started with --repair-failed: no repair by itself this time) */

/* The install against its release's file list (release_files.txt, 2.1.4 on): each file there, with its size. */
static int up_files_ok(const WCHAR *dir, WCHAR *msg, size_t n)
{
    ReleaseCheck rc;
    if (!dir[0] || !release_check(dir, NULL, 0, &rc, NULL, NULL) || !rc.bad || wcscmp(rc.version, PORT_VERSION))
        return 1;
    swprintf_s(msg, n, L"%d file(s) of version %s are missing or damaged (e.g. %s): repair the install.", rc.bad,
               rc.version, rc.first_bad);
    return 0;
}

static void up_check_finished(void)
{
    WCHAR gv[32];
    s_up_finish = 0;
    if (up_game_version(gv, 32) && (version_newer(gv, PORT_VERSION) || version_newer(PORT_VERSION, gv))) {
        s_up_finish = 1;
        wcscpy_s(s_up_game_version, 32, !wcscmp(gv, L"1.0.0") ? L"older" : gv);
        swprintf_s(s_up_finish_msg, 400, L"The game is %s, the launcher %s: the last update didn't finish.",
                   s_up_game_version, PORT_VERSION);
        wcscpy_s(s_up_finish_btn, 40, L"Finish the update");
    } else if (!up_files_ok(s_game_dir, s_up_finish_msg, 400) ||
               (!same_dir(s_game_dir, s_launcher_dir) && !up_files_ok(s_launcher_dir, s_up_finish_msg, 400))) {
        s_up_finish = 2;   /* (the same versions, but files an update or an unzip left out) */
        wcscpy_s(s_up_finish_btn, 40, L"Repair the install");
        if (s_repair_failed)
            wcscat_s(s_up_finish_msg, 400, L" (The last repair didn't finish.)");
    }
    if (s_up_finish) {
        up_status(L"%s", s_up_finish_msg);
        set_text(ID_UP_BUTTON, s_up_finish_btn);
    }
    /* missing files: fetched by themselves (the updater gets only those, then starts the launcher again) -
       unless the last repair failed or the game runs */
    if (s_up_finish == 2 && !s_repair_failed && !any_game_running() && s_wnd)
        PostMessageW(s_wnd, WM_APP_UPD_REPAIR, 0, 0);
}

static void up_complete(void)
{
    if (s_up_busy || s_busy)
        return;
    up_run_updater(PORT_VERSION, s_up_finish == 2);
}

static void up_apply(void)
{
    WCHAR q[400];
    if (!s_up_available || s_up_busy || s_busy)
        return;
    swprintf_s(q, 400, L"Update the PC port to version %s?\n\nThe updater downloads it (%.0f MB), replaces the "
                       L"program files and starts the launcher again. Close the game and the Mod Maker first. Your saves, "
                       L"settings, music and movies are kept.",
               s_up.version, s_up.size / 1048576.0);
    if (MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    up_run_updater(NULL, 0);
}

static void up_checked(int ok, WCHAR *err)
{
    InterlockedExchange(&s_up_busy, 0);
    EnableWindow(ctl(ID_UP_BUTTON), TRUE);
    s_up_available = ok && version_newer(s_up.version, PORT_VERSION);
    play_notice();
    if (!ok) {
        up_status(L"%s", err ? err : L"Could not check for updates.");
    } else if (s_up_available) {
        WCHAR b[64];
        up_status(L"Version %s is available%s.", s_up.version, s_up.prerelease ? L" (preview)" : L"");
        swprintf_s(b, 64, L"Update to %s", s_up.version);
        set_text(ID_UP_BUTTON, b);
        if (s_up_auto_run)
            up_apply();
    } else if (s_up_finish) {
        up_status(L"%s", s_up_finish_msg);
        set_text(ID_UP_BUTTON, s_up_finish_btn);
    } else {
        up_status(L"You have the newest version.");
    }
    free(err);
}

/* WM_COMMAND for the update controls; 1 if handled. */
static int up_command(int id, int code)
{
    (void)code;
    if (id == ID_UP_BUTTON) {
        if (s_up_available) up_apply();
        else if (s_up_finish) up_complete();
        else up_check(0);
        return 1;
    }
    if (id == ID_UP_AUTO) {
        WritePrivateProfileStringW(L"Launcher", L"CheckUpdates",
            IsDlgButtonChecked(s_wnd, ID_UP_AUTO) == BST_CHECKED ? L"1" : L"0", s_launcher_ini);
        return 1;
    }
    return 0;
}

static void up_setup(void)
{
    WCHAR v[64];
    swprintf_s(v, 64, L"Version %s", PORT_VERSION);
    add(-1, L"Static", v, SS_LEFT, X0 + 16, 354, 150, 20, ID_VERSION);
    add(TAB_PLAY, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, X0 + 290, 550, 156, 36, ID_UP_STATUS);
    add(TAB_PLAY, L"Button", L"Check for updates", BS_PUSHBUTTON | WS_TABSTOP, X0 + 724 - 16 - 160, 414, 160, 28, ID_UP_BUTTON);
    add(TAB_SETTINGS, L"Button", L"Check for updates when the launcher starts", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 404, 400, 22, ID_UP_AUTO);
    add(TAB_PLAY, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0 + 290, 584, 156, 4, ID_UP_PROGRESS);
    ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    CheckDlgButton(s_wnd, ID_UP_AUTO,
                   GetPrivateProfileIntW(L"Launcher", L"CheckUpdates", 1, s_launcher_ini) ? BST_CHECKED : BST_UNCHECKED);
}

/* Whether a control sits on a card of the page shown. */
static int card_at(HWND c)
{
    RECT r;
    int i;
    GetWindowRect(c, &r);
    MapWindowPoints(NULL, s_wnd, (POINT *)&r, 2);
    for (i = 0; i < s_ncards; i++) {
        const Card *cd = &s_cards[i];
        if (cd->tab != s_cur_tab || card_hidden(cd))
            continue;
        if (r.left >= S(cd->x + SB_W) && r.top >= S(cd->y + DY) && r.right <= S(cd->x + cd->w + SB_W) + 1 &&
            r.bottom <= S(cd->y + cd->h + DY) + 1)
            return 1;
    }
    return 0;
}

/* The sidebar item under (x, y), or -1. */
static int nav_at(int x, int y)
{
    int k;
    if (x < 0 || x >= S(SB_W))
        return -1;
    for (k = 0; k < NAV_N; k++) {
        const RECT r = nav_rect(k);
        if (y >= S(r.top) && y < S(r.bottom))
            return k;
    }
    return -1;
}

/* A push button: rounded, red for the main actions, lighter when the mouse
   is on it, darker pressed; PLAY big with its icon. */
static int draw_button(const DRAWITEMSTRUCT *d)
{
    int i, primary = -1;
    for (i = 0; i < s_nbtns; i++)
        if (s_btns[i].h == d->hwndItem)
            primary = s_btns[i].primary;
    if (primary < 0)
        return 0;
    {
        const int disabled = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0;
        const int hot = !disabled && s_hover_btn == d->hwndItem;
        const int play = GetDlgCtrlID(d->hwndItem) == ID_PLAY;
        RECT r = d->rcItem;
        WCHAR text[128];
        COLORREF fill, line, ink;
        HDC dc = d->hDC;
        HFONT font;
        HGDIOBJ old;
        FillRect(dc, &r, card_at(d->hwndItem) ? s_card_brush : s_page_brush);  /* (behind the corners) */
        if (primary) {
            /* (disabled: grey - a dim red with white text looked like a working button, and a click did nothing) */
            fill = disabled ? RGB(48, 51, 58) : down ? C_ACCENT_DOWN : hot ? C_ACCENT_HOT : C_ACCENT;
            line = THEME_NONE;
            ink = disabled ? RGB(122, 126, 136) : RGB(255, 255, 255);
        } else {
            fill = disabled ? RGB(30, 32, 38) : down ? RGB(24, 26, 31) : hot ? RGB(44, 48, 56) : RGB(34, 37, 44);
            line = disabled ? RGB(44, 47, 54) : hot ? RGB(98, 102, 114) : RGB(66, 70, 80);
            ink = disabled ? RGB(100, 103, 112) : C_TEXT;
        }
        theme_round_rect(dc, &r, S(play ? 8 : 6), fill, line, (float)S(1));
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
            RECT f = r;
            InflateRect(&f, -S(3), -S(3));
            theme_round_rect(dc, &f, S(4), THEME_NONE, primary ? RGB(255, 190, 200) : C_ACCENT, (float)S(1));
        }
        GetWindowTextW(d->hwndItem, text, 128);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        if (down)
            OffsetRect(&r, 0, S(1));
        if (play) {  /* (the play icon, then PLAY) */
            WCHAR icon[2] = { 0xE768, 0 };
            SIZE ts, is;
            RECT ir = r, tr = r;
            int x0;
            old = SelectObject(dc, s_huge);
            GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ts);
            SelectObject(dc, s_icons);
            GetTextExtentPoint32W(dc, icon, 1, &is);
            x0 = (r.left + r.right - (is.cx + S(10) + ts.cx)) / 2;
            ir.left = x0;
            ir.right = x0 + is.cx;
            DrawTextW(dc, icon, 1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, s_huge);
            tr.left = x0 + is.cx + S(10);
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
            return 1;
        }
        font = (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0);
        old = SelectObject(dc, font ? font : s_font);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    return 1;
}

/* draw_button drawn off-screen first, then copied: hovering a button no longer
   shows its background a moment before its face (flicker). */
static int draw_button_buffered(const DRAWITEMSTRUCT *d)
{
    DRAWITEMSTRUCT b = *d;
    const int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC mdc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = mdc ? CreateCompatibleBitmap(d->hDC, w, h) : NULL;
    HGDIOBJ old;
    int ok;
    if (!bmp) {
        if (mdc) DeleteDC(mdc);
        return draw_button(d);
    }
    old = SelectObject(mdc, bmp);
    b.hDC = mdc;
    SetRect(&b.rcItem, 0, 0, w, h);
    ok = draw_button(&b);
    if (ok)
        BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    return ok;
}

/* ── the Play page ─────────────────────────────────────────────────────── */
/* Its cards are group boxes (build_ui); what's on them is drawn here: the picture card (title, the
   game's size), the tiles (last session, sessions, version), Setup / Saves / Mods / Texture packs, the
   status line. Places are the page's (x from X0, y from DY). */
#define PX(x) S(SB_W + X0 + (x))
#define PY(y) S(DY + (y))

static int s_mods_n, s_mods_on, s_packs_n;

/* The picture behind the title: <launcher folder>\launcher_art\hero.(png|jpg|jpeg|bmp), the player's
   own (a wide one scrolls). None: the card without a picture. */
static void hero_find(void)
{
    static const WCHAR *const ext[] = { L"png", L"jpg", L"jpeg", L"bmp" };
    int i;
    s_hero_file[0] = 0;
    for (i = 0; i < 4; i++) {
        swprintf_s(s_hero_file, MAX_PATH, L"%s\\launcher_art\\hero.%s", s_launcher_dir, ext[i]);
        if (file_exists(s_hero_file))
            return;
    }
    s_hero_file[0] = 0;
}

static void hero_rect(RECT *r)
{
    r->left = PX(0); r->top = PY(-8); r->right = PX(724); r->bottom = PY(392);
}

/* The mods (as mods_tab.c's list counts them: folders under Mods\<type>, off with a "disabled" file) and
   the texture packs (folders in Texture Packs). */
static void play_counts(void)
{
    static const WCHAR *const types[] = { L"Arenas", L"Superstars", L"Signs", L"Media", L"Backstage", L"Moves",
                                         L"MatchTypes", L"Backgrounds", L"Attires" };
    WCHAR pat[MAX_PATH], off[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int t;
    s_mods_n = s_mods_on = s_packs_n = 0;
    if (!s_game_dir[0])
        return;
    for (t = 0; t < (int)(sizeof types / sizeof types[0]); t++) {
        swprintf_s(pat, MAX_PATH, L"%s\\Mods\\%s\\*", s_game_dir, types[t]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            s_mods_n++;
            swprintf_s(off, MAX_PATH, L"%s\\Mods\\%s\\%s\\disabled", s_game_dir, types[t], fd.cFileName);
            if (!file_exists(off))
                s_mods_on++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    swprintf_s(pat, MAX_PATH, L"%s\\Texture Packs\\*", s_game_dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                s_packs_n++;
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

/* The card link under pt (a card's top part, "Manage ..."): its page, or -1. */
static int play_link_at(POINT pt)
{
    int i;
    if (s_cur_tab != TAB_PLAY)
        return -1;
    for (i = 0; i < s_nplay_links; i++)
        if (PtInRect(&s_play_links[i].r, pt))
            return s_play_links[i].tab;
    return -1;
}

static void play_link(int x, int y, int w, int h, int tab)
{
    if (s_nplay_links < (int)(sizeof s_play_links / sizeof s_play_links[0])) {
        PlayLink *l = &s_play_links[s_nplay_links++];
        l->r.left = PX(x); l->r.top = PY(y); l->r.right = PX(x + w); l->r.bottom = PY(y + h);
        l->tab = tab;
    }
}

static void play_text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, const WCHAR *t, UINT flags)
{
    RECT r = { PX(x), PY(y), PX(x + w), PY(y + h) };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

static void play_glyph(HDC dc, HFONT f, COLORREF c, int x, int y, WCHAR g)
{
    WCHAR t[2] = { g, 0 };
    play_text(dc, f, c, x, y, 40, 40, t, DT_LEFT | DT_TOP);
}

static void play_line(HDC dc, int x, int y, int w, int h)
{
    RECT r = { PX(x), PY(y), PX(x + w), PY(y + h) };
    HBRUSH b = CreateSolidBrush(C_CARD_LINE);
    if (r.right == r.left) r.right++;
    if (r.bottom == r.top) r.bottom++;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* A card's head: its icon, title, line of help, the arrow; the head opens `tab`. */
static void play_card_head(HDC dc, int x, int y, WCHAR icon, const WCHAR *title, const WCHAR *help, int tab)
{
    play_glyph(dc, s_mark_font, C_TEXT, x + 18, y + 16, icon);
    play_text(dc, s_big, C_TEXT, x + 62, y + 10, 260, 24, title, DT_LEFT | DT_VCENTER);
    play_text(dc, s_font, C_MUTED, x + 62, y + 34, 262, 20, help, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    play_glyph(dc, s_icons, C_MUTED, x + 330, y + 24, 0xE76C);
    play_line(dc, x + 18, y + 64, 320, 0);
    play_link(x, y, 356, 62, tab);
}

/* What needs the player now, if anything (one line on the Play page, with its button): no game here, the
   program missing, program files missing (antivirus), the VC++ runtime, an update. */
static void play_notice(void)
{
    WCHAR v[32];
    const WCHAR *button = L"";
    s_notice[0] = 0;
    s_notice_action = NOTICE_NONE;
    if (!s_game_dir[0] || !is_game_folder(s_game_dir)) {
        wcscpy_s(s_notice, 220, L"No game in this folder: choose the game's folder, or install it (Setup).");
        s_notice_action = NOTICE_FOLDER; button = L"Choose folder\x2026";
    } else if (verify_xex_supported_in(s_game_dir) == 0) {
        wcscpy_s(s_notice, 220, L"Another release of the game (e.g. the Japanese disc): the port can't run it.");
        s_notice_action = NOTICE_SETUP; button = L"Open Setup";
    } else if (!has_program(s_game_dir)) {
        wcscpy_s(s_notice, 220, GAME_EXE L" is missing from the game folder (antivirus, or an unfinished install).");
        s_notice_action = NOTICE_FILES; button = L"Open Setup";
    } else if (s_size_known && s_files_listed && s_files_bad) {
        swprintf_s(s_notice, 220, L"%d of the port's program files %s missing - antivirus may have taken %s.",
                   s_files_bad, s_files_bad == 1 ? L"is" : L"are", s_files_bad == 1 ? L"it" : L"them");
        s_notice_action = NOTICE_FILES; button = L"Check files";
    } else if (setup_vcrt_system(v, 32) != SETUP_VCRT_OK) {
        wcscpy_s(s_notice, 220, L"Setup isn't finished: the game needs Microsoft's Visual C++ runtime.");
        s_notice_action = NOTICE_SETUP; button = L"Open Setup";
    } else if (s_up_available) {
        swprintf_s(s_notice, 220, L"Version %s of the port is available.", s_up.version);
        s_notice_action = NOTICE_UPDATE; button = L"Update\x2026";
    }
    set_text(ID_PL_NOTICE, button);
    ShowWindow(ctl(ID_PL_NOTICE), s_notice[0] && s_cur_tab == TAB_PLAY ? SW_SHOW : SW_HIDE);
    if (s_wnd && s_cur_tab == TAB_PLAY)
        InvalidateRect(s_wnd, NULL, FALSE);
}

static void paint_play(HDC dc)
{
    RECT hr;
    WCHAR t[200], a[32], when[64];
    WCHAR pt_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned long long sec = 0, ses = 0;
    int i;
    s_nplay_links = 0;
    SetBkMode(dc, TRANSPARENT);

    /* the picture card: the title, Play, Start Without Mods, the game's size */
    hero_rect(&hr);
    i = theme_hero(dc, &hr, S(10), s_hero_file, C_CARD, S(s_hero_scroll) / 2);
    if (i && !s_hero_moving)
        SetTimer(s_wnd, HERO_TIMER, 33, NULL);
    s_hero_moving = i;
    theme_round_rect(dc, &hr, S(10), THEME_NONE, C_CARD_LINE, (float)S(1));
    play_text(dc, s_huge, C_TEXT, 28, 84, 560, 44, GAME_TITLE, DT_LEFT | DT_VCENTER);
    play_text(dc, s_font, C_MUTED, 28, 132, 560, 22,
              L"The legendary roster. The biggest arenas. The ultimate showdown.", DT_LEFT | DT_VCENTER);
    play_line(dc, 436, 218, 0, 48);
    play_glyph(dc, s_mark_font, C_MUTED, 450, 226, 0xEDA2);
    play_text(dc, s_font, C_MUTED, 490, 212, 200, 18, L"Game size", DT_LEFT | DT_VCENTER);
    if (s_size_known && same_dir(s_size_dir, s_game_dir)) {
        fmt_size(a, 32, s_size_game);
        play_text(dc, s_big, C_TEXT, 490, 230, 200, 24, a, DT_LEFT | DT_VCENTER);
        fmt_size(a, 32, s_size_mods);
        swprintf_s(t, 200, L"(mods: %s)", a);
        play_text(dc, s_font, C_MUTED, 490, 254, 200, 18, t, DT_LEFT | DT_VCENTER);
    } else {
        play_text(dc, s_big, C_MUTED, 490, 230, 200, 24, s_game_dir[0] ? L"Measuring\x2026" : L"\x2014",
                  DT_LEFT | DT_VCENTER);
    }

    /* one slim row: last played, sessions and time, the version (Check for updates: build_ui) */
    swprintf_s(pt_path, MAX_PATH, L"%s\\playtime.txt", s_game_dir);
    {
        FILE *f = NULL;
        char line[128];
        if (!_wfopen_s(&f, pt_path, L"r") && f) {
            while (fgets(line, sizeof line, f)) {
                if (!strncmp(line, "seconds=", 8)) sec = _strtoui64(line + 8, NULL, 10);
                if (!strncmp(line, "sessions=", 9)) ses = _strtoui64(line + 9, NULL, 10);
            }
            fclose(f);
        }
    }
    wcscpy_s(when, 64, L"not yet");
    if (ses && GetFileAttributesExW(pt_path, GetFileExInfoStandard, &fa)) {
        FILETIME now;
        ULARGE_INTEGER u0, u1;
        unsigned long long m;
        GetSystemTimeAsFileTime(&now);
        u0.LowPart = fa.ftLastWriteTime.dwLowDateTime; u0.HighPart = fa.ftLastWriteTime.dwHighDateTime;
        u1.LowPart = now.dwLowDateTime; u1.HighPart = now.dwHighDateTime;
        m = u1.QuadPart > u0.QuadPart ? (u1.QuadPart - u0.QuadPart) / 600000000ull : 0;
        if (m < 1) wcscpy_s(when, 64, L"just now");
        else if (m < 60) swprintf_s(when, 64, L"%llu min ago", m);
        else if (m < 48 * 60) swprintf_s(when, 64, L"%llu h ago", m / 60);
        else swprintf_s(when, 64, L"%llu days ago", m / 1440);
    }
    if (sec >= 3600)
        swprintf_s(t, 200, L"Last played %s   \x00B7   %llu session%s, %llu h %llu min   \x00B7   Version %s", when, ses,
                   ses == 1 ? L"" : L"s", sec / 3600, sec / 60 % 60, PORT_VERSION);
    else
        swprintf_s(t, 200, L"Last played %s   \x00B7   %llu session%s, %llu min   \x00B7   Version %s", when, ses,
                   ses == 1 ? L"" : L"s", sec / 60, PORT_VERSION);
    play_glyph(dc, s_icons, C_MUTED, 18, 420, 0xE823);
    play_text(dc, s_font, C_MUTED, 46, 416, 480, 24, t, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    /* the notice (play_notice): only when something needs the player */
    if (s_notice[0]) {
        play_glyph(dc, s_icons, s_notice_action == NOTICE_UPDATE ? RGB(120, 170, 255) : C_WARN, 18, 478,
                   s_notice_action == NOTICE_UPDATE ? 0xE896 : 0xE7BA);
        play_text(dc, s_font, C_TEXT, 46, 472, 480, 28, s_notice, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    /* the status line: ready (ID_PLAY_STATUS), updates (ID_UP_STATUS), the program files' check */
    play_glyph(dc, s_icons, s_play_ready ? C_GOOD : C_WARN, 18, 560, s_play_ready ? 0xE930 : 0xE7BA);
    play_glyph(dc, s_icons, C_MUTED, 592, 560, 0xEA18);
    play_text(dc, s_font, C_MUTED, 616, 550, 104, 18, L"Antivirus", DT_LEFT | DT_VCENTER);
    if (!s_size_known)
        play_text(dc, s_font, C_MUTED, 616, 568, 104, 18, L"Checking files\x2026", DT_LEFT | DT_VCENTER);
    else if (!s_files_listed)
        play_text(dc, s_font, C_MUTED, 616, 568, 104, 18, L"No file list", DT_LEFT | DT_VCENTER);
    else if (!s_files_bad)
        play_text(dc, s_font, C_GOOD, 616, 568, 104, 18, L"No files blocked", DT_LEFT | DT_VCENTER);
    else {
        swprintf_s(t, 200, L"%d file%s missing", s_files_bad, s_files_bad == 1 ? L"" : L"s");
        play_text(dc, s_font, C_WARN, 616, 568, 104, 18, t, DT_LEFT | DT_VCENTER);
    }
}
#undef PX
#undef PY

/* ── Setup's step bar ──────────────────────────────────────────────────── */
/* Each step's state, found when a step's page is shown (s_step_done: 1 done, 0 to do, -1 optional). */
static int  s_step_done[N_STEPS];
static RECT s_step_rects[N_STEPS];

static void steps_refresh(void)
{
    WCHAR v[32], d[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    s_step_done[0] = setup_vcrt_system(v, 32) == SETUP_VCRT_OK;
    s_step_done[1] = is_game_folder(s_game_dir) && has_program(s_game_dir);
    s_step_done[2] = -1;
    swprintf_s(d, MAX_PATH, L"%s\\DLC\\*", s_game_dir);
    h = s_game_dir[0] ? FindFirstFileW(d, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (fd.cFileName[0] != L'.')
                s_step_done[2] = 1;
        while (s_step_done[2] < 0 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    s_step_done[3] = -1;
    s_step_done[4] = -1;
}

/* Setup's first step still to do (all done: the first). */
static int setup_first_step(void)
{
    int i;
    steps_refresh();
    for (i = 0; i < N_STEPS; i++)
        if (!s_step_done[i])
            return k_steps[i];
    return k_steps[0];
}

static int step_at(POINT pt)
{
    int i;
    if (setup_step(s_cur_tab) < 0)
        return -1;
    for (i = 0; i < N_STEPS; i++)
        if (PtInRect(&s_step_rects[i], pt))
            return i;
    return -1;
}

/* On top of each step's page: four steps, each a circle (its number; a tick when done) and its name, the
   page's own underlined in red. */
static void paint_steps(HDC dc)
{
    static const WCHAR *const names[N_STEPS] = { L"This PC", L"The game", L"DLC", L"Tools", L"Android" };
    const int cur = setup_step(s_cur_tab), x0 = SB_W + PAD + X0, w = 112;
    int i;
    RECT line = { S(x0), S(DY + 42), S(x0 + 560), S(DY + 43) };
    HBRUSH b = CreateSolidBrush(C_CARD_LINE);
    FillRect(dc, &line, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < N_STEPS; i++) {
        const int x = x0 + i * w, done = s_step_done[i] == 1, here = i == cur;
        RECT c = { S(x), S(DY + 4), S(x + 26), S(DY + 30) }, t;
        WCHAR num[4];
        const WCHAR *state = done ? L"Done" : s_step_done[i] < 0 ? L"Optional" : L"To do";
        theme_round_rect(dc, &c, S(13), done ? C_GOOD : here ? C_ACCENT : C_CARD,
                         done || here ? THEME_NONE : RGB(90, 94, 106), (float)S(1));
        SetTextColor(dc, done || here ? RGB(255, 255, 255) : C_MUTED);
        if (done) {
            WCHAR g[2] = { 0xE73E, 0 };
            SelectObject(dc, s_icons);
            DrawTextW(dc, g, 1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            swprintf_s(num, 4, L"%d", i + 1);
            SelectObject(dc, s_nav);
            DrawTextW(dc, num, -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, s_nav);
        SetTextColor(dc, here ? RGB(255, 255, 255) : RGB(200, 202, 210));
        SetRect(&t, S(x + 34), S(DY + 0), S(x + w - 4), S(DY + 18));
        DrawTextW(dc, names[i], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, s_font);
        SetTextColor(dc, done ? C_GOOD : C_MUTED);
        SetRect(&t, S(x + 34), S(DY + 18), S(x + w - 4), S(DY + 36));
        DrawTextW(dc, state, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (here) {
            RECT u = { S(x), S(DY + 41), S(x + w - 8), S(DY + 44) };
            b = CreateSolidBrush(C_ACCENT);
            FillRect(dc, &u, b);
            DeleteObject(b);
        }
        SetRect(&s_step_rects[i], S(x), S(DY), S(x + w - 8), S(DY + 44));
    }
}

/* ── Mods' tab bar ─────────────────────────────────────────────────────── */
static RECT s_mod_tab_rects[N_MOD_TABS];

static int mod_tab_at(POINT pt)
{
    int i;
    if (mods_tab_of(s_cur_tab) < 0)
        return -1;
    for (i = 0; i < N_MOD_TABS; i++)
        if (PtInRect(&s_mod_tab_rects[i], pt))
            return i;
    return -1;
}

/* On top of each Mods page: its three tabs, the page's own underlined in red. */
static void paint_mod_tabs(HDC dc)
{
    static const WCHAR *const names[N_MOD_TABS] = { L"Installed Mods", L"Discovery", L"Texture Packs" };
    static const WCHAR icons[N_MOD_TABS] = { 0xEA86, 0xE721, 0xE8B9 };
    const int cur = mods_tab_of(s_cur_tab), x0 = SB_W + PAD + X0;
    int i, x = x0;
    RECT line = { S(x0), S(DY + 38), S(x0 + 560), S(DY + 39) };
    HBRUSH b = CreateSolidBrush(C_CARD_LINE);
    FillRect(dc, &line, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < N_MOD_TABS; i++) {
        const int here = i == cur, w = 168;
        WCHAR g[2] = { icons[i], 0 };
        RECT t = { S(x), S(DY + 4), S(x + 26), S(DY + 34) };
        SelectObject(dc, s_icons);
        SetTextColor(dc, here ? C_ACCENT_HOT : C_MUTED);
        DrawTextW(dc, g, 1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, s_nav);
        SetTextColor(dc, here ? RGB(255, 255, 255) : RGB(190, 192, 202));
        SetRect(&t, S(x + 26), S(DY + 4), S(x + w), S(DY + 34));
        DrawTextW(dc, names[i], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (here) {
            RECT u = { S(x), S(DY + 37), S(x + w - 16), S(DY + 40) };
            b = CreateSolidBrush(C_ACCENT);
            FillRect(dc, &u, b);
            DeleteObject(b);
        }
        SetRect(&s_mod_tab_rects[i], S(x), S(DY), S(x + w - 16), S(DY + 40));
        x += w;
    }
}

/* The window: the sidebar, the page's title, its cards. */
static void paint_window(HDC dc)
{
    static const WCHAR *const titles[TAB_COUNT] = { L"Play", L"Settings", L"Online", L"Setup", L"Setup",
                                                    L"Saves", L"Paint Tool", L"Movies", L"Setup", L"Mods",
                                                    L"Music", L"Roster: sort categories", L"Mods",
                                                    L"Setup", L"Mods", L"Setup", L"Reports", L"Suggestions" };
    RECT cl, r;
    HBRUSH sb = CreateSolidBrush(C_SIDEBAR);
    HGDIOBJ old;
    int k, i;
    WCHAR v[64];
    GetClientRect(s_wnd, &cl);
    /* Sidebar: the game's icon and name, the tabs, the version. */
    r = cl;
    r.right = S(SB_W);
    FillRect(dc, &r, sb);
    DeleteObject(sb);
    SetBkMode(dc, TRANSPARENT);
    if (s_icon)
        DrawIconEx(dc, S(18), S(22), s_icon, S(40), S(40), 0, NULL, DI_NORMAL);
    old = SelectObject(dc, s_card);
    SetTextColor(dc, RGB(250, 250, 252));
    r.left = S(68); r.top = S(22); r.right = S(SB_W - 8); r.bottom = S(42);
    SelectObject(dc, s_title);
    r.top = S(20); r.bottom = S(44);
    DrawTextW(dc, L"SvR 2011", -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, s_card);
    SetTextColor(dc, RGB(208, 20, 44));
    r.top = S(44); r.bottom = S(64);
    DrawTextW(dc, L"PC PORT", -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    for (k = 0; k < NAV_N; k++) {
        const int sel = k_nav[k].tab == nav_tab(s_cur_tab), hot = k == s_nav_hover, big = k < NAV_MAIN;
        const RECT n = nav_rect(k);
        RECT it = { 0, S(n.top), S(SB_W), S(n.bottom) };
        WCHAR icon[2] = { k_nav[k].icon, 0 };
        if (sel || hot) {
            HBRUSH b = CreateSolidBrush(sel ? C_SIDEBAR_SEL : C_SIDEBAR_HOVER);
            FillRect(dc, &it, b);
            DeleteObject(b);
        }
        if (sel) {
            RECT bar = { 0, it.top, S(4), it.bottom };
            HBRUSH b = CreateSolidBrush(C_ACCENT);
            FillRect(dc, &bar, b);
            DeleteObject(b);
        }
        SetTextColor(dc, sel ? C_ACCENT_HOT : big ? RGB(178, 180, 190) : RGB(130, 132, 142));
        SelectObject(dc, s_icons);
        r = it;
        r.left += S(26);
        DrawTextW(dc, icon, 1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, sel ? RGB(255, 255, 255) : big ? RGB(214, 216, 224) : RGB(150, 152, 162));
        SelectObject(dc, big ? s_nav : s_font);
        r.left += S(34);
        DrawTextW(dc, k_nav[k].name, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (k == NAV_MAIN - 1) {   /* (the line under the main pages) */
            RECT line = { S(20), it.bottom + S(NAV_LINE / 2), S(SB_W - 20), it.bottom + S(NAV_LINE / 2) + 1 };
            HBRUSH b = CreateSolidBrush(RGB(40, 43, 50));
            FillRect(dc, &line, b);
            DeleteObject(b);
        }
    }
    SelectObject(dc, s_font);
    SetTextColor(dc, RGB(150, 152, 162));
    swprintf_s(v, 64, L"Launcher v%s", PORT_VERSION);
    {
        RECT dot = { S(20), cl.bottom - S(28), S(28), cl.bottom - S(20) };
        theme_round_rect(dc, &dot, S(4), C_GOOD, THEME_NONE, 0);
    }
    r.left = S(36); r.right = S(SB_W - 10); r.top = cl.bottom - S(34); r.bottom = cl.bottom - S(12);
    DrawTextW(dc, v, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    /* "Support me: ko-fi" above the version: the link opens the Ko-fi page. */
    {
        SIZE z;
        RECT k = { S(20), cl.bottom - S(56), S(SB_W - 10), cl.bottom - S(34) };
        DrawTextW(dc, L"Support me: ", -1, &k, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32W(dc, L"Support me: ", 12, &z);
        k.left += z.cx;
        SetTextColor(dc, C_ACCENT);
        DrawTextW(dc, L"ko-fi", -1, &k, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32W(dc, L"ko-fi", 5, &z);
        s_kofi_link.left = k.left; s_kofi_link.right = k.left + z.cx;
        s_kofi_link.top = k.top; s_kofi_link.bottom = k.top + z.cy;
        k.left += z.cx;
        SetTextColor(dc, RGB(150, 152, 162));
        DrawTextW(dc, L"  B7 ", -1, &k, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32W(dc, L"  B7 ", 3, &z);
        k.left += z.cx;
        SetTextColor(dc, C_ACCENT);
        DrawTextW(dc, L"PayPal", -1, &k, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32W(dc, L"PayPal", 6, &z);
        s_paypal_link.left = k.left; s_paypal_link.right = k.left + z.cx;
        s_paypal_link.top = k.top; s_paypal_link.bottom = k.top + z.cy;
        SetTextColor(dc, RGB(150, 152, 162));
    }
    /* The credit, top right of every page: the link opens the release page. */
    {
        RECT c;
        SIZE z;
        SelectObject(dc, s_font);
        c.left = S(SB_W + X0 + 200); c.right = cl.right - S(16); c.top = S(12); c.bottom = S(31);
        SetTextColor(dc, C_MUTED);
        DrawTextW(dc, CREDIT_LINE, -1, &c, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        c.top = S(31); c.bottom = S(50);
        SetTextColor(dc, C_ACCENT);
        DrawTextW(dc, CREDIT_LINK, -1, &c, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32W(dc, CREDIT_LINK, (int)wcslen(CREDIT_LINK), &z);
        s_credit_link.left = c.right - z.cx; s_credit_link.right = c.right;
        s_credit_link.top = c.top; s_credit_link.bottom = c.bottom;
    }
    /* The page: its title, its cards. */
    if (s_cur_tab >= 0 && s_cur_tab < TAB_COUNT) {
        SelectObject(dc, s_huge);
        SetTextColor(dc, C_TEXT);
        r.left = S(SB_W + X0 + PAD); r.right = cl.right - S(16); r.top = S(14); r.bottom = S(52);
        if (s_cur_tab != TAB_PLAY)
            DrawTextW(dc, titles[s_cur_tab], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (i = 0; i < s_ncards; i++) {
            const Card *cd = &s_cards[i];
            RECT c;
            if (cd->tab != s_cur_tab || card_hidden(cd) || (cd->title[0] == 1 && !s_notice[0]))
                continue;
            c.left = S(cd->x + SB_W); c.top = S(cd->y + DY); c.right = S(cd->x + cd->w + SB_W);
            c.bottom = S(cd->y + cd->h + DY);
            theme_round_rect(dc, &c, S(10), C_CARD, C_CARD_LINE, (float)S(1));
            if (cd->title[0] > 1) {
                SelectObject(dc, s_card);
                SetTextColor(dc, C_TEXT);
                r = c;
                r.left += S(14); r.top += S(6); r.bottom = r.top + S(20);
                DrawTextW(dc, cd->title, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        if (s_cur_tab == TAB_PLAY)
            paint_play(dc);
        if (setup_step(s_cur_tab) >= 0)
            paint_steps(dc);
        if (mods_tab_of(s_cur_tab) >= 0)
            paint_mod_tabs(dc);
    }
    SelectObject(dc, old);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_TAB && n->code == TCN_SELCHANGE)
            show_tab(TabCtrl_GetCurSel(s_tab));
        if (n->idFrom == ID_FR_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uNewState & LVIS_SELECTED) && !(v->uOldState & LVIS_SELECTED)) {
                WCHAR name[64];
                ListView_GetItemText(ctl(ID_FR_LIST), v->iItem, 0, name, 64);
                set_text(ID_FR_NAME, name);
            }
        }
        if (n->idFrom != ID_TAB && n->idFrom != ID_FR_LIST && !texpacks_notify(n) && !discover_notify(n) && !reports_notify(n) &&
            !suggestions_notify(n)) {
            const int r = mods_notify(n);
            if (r == 2 || r == 3) return r == 2;  /* (renaming a background: mods_tab.c) */
        }
        break;
    }
    case WM_COMMAND:
        if (mv_command(LOWORD(wp), HIWORD(wp)) || up_command(LOWORD(wp), HIWORD(wp)) ||
            mods_command(LOWORD(wp), HIWORD(wp)) || music_command(LOWORD(wp), HIWORD(wp)) ||
            reports_command(LOWORD(wp), HIWORD(wp)) || suggestions_command(LOWORD(wp), HIWORD(wp)) ||
            roster_command(LOWORD(wp), HIWORD(wp)) || texpacks_command(LOWORD(wp), HIWORD(wp)) ||
            discover_command(LOWORD(wp), HIWORD(wp)) ||
            setup_command(LOWORD(wp), HIWORD(wp)))
            return 0;
        switch (LOWORD(wp)) {
        case ID_REPORT:
            report_problem();
            break;
        case ID_PLAY:
            play();
            break;
        case ID_PLAY_NOMODS:
            play_with(1);
            break;
        case ID_PL_NOTICE:
            if (s_notice_action == NOTICE_FOLDER)
                SendMessageW(w, WM_COMMAND, ID_GAMEDIR_CHANGE, 0);
            else if (s_notice_action == NOTICE_UPDATE)
                SendMessageW(w, WM_COMMAND, ID_UP_BUTTON, 0);
            else if (s_notice_action == NOTICE_FILES)
                show_tab(TAB_INSTALL);   /* (Verify game files / Repair) */
            else if (s_notice_action == NOTICE_SETUP)
                show_tab(setup_first_step());
            break;
        case ID_PL_EXPORT:   /* (the Saves page: pick the saves there) */
            show_tab(TAB_SAVES);
            break;
        case ID_PL_IMPORT:
            show_tab(TAB_SAVES);
            saves_import();
            break;
        case ID_GAMEDIR_CHANGE: {
            WCHAR d[MAX_PATH];
            if (pick_folder(L"Choose the folder that holds the installed game", d)) {
                if (!is_game_folder(d)
                        && MessageBoxW(w, L"That folder does not have the game's files in it (default.xex). "
                                          L"To set the game up from your disc image, use the Install tab.\n\n"
                                          L"Use this folder anyway?",
                                       WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    break;
                wcscpy_s(s_game_dir, MAX_PATH, d);
                save_launcher_ini();
                settings_load();
                refresh_play();
            }
            break;
        }
        case ID_MUSIC_OPEN: {
            WCHAR m[MAX_PATH];
            if (!s_game_dir[0] || !dir_exists(s_game_dir)) {
                set_text(ID_SETTINGS_STATUS, L"The game folder does not exist; choose it on the Play tab first.");
                break;
            }
            join(m, s_game_dir, L"Music");
            CreateDirectoryW(m, NULL);
            ShellExecuteW(s_wnd, L"open", m, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_ARENA_LIGHT:
        case ID_RESOLUTION: case ID_RENDER_RES: case ID_INPUT: case ID_AUDIO: case ID_RENDERER: case ID_LANGUAGE: case ID_FRAMERATE: case ID_MSAA: case ID_TEXTURES: case ID_SHOWFPS:
        case ID_UPSCALE: case ID_MATCH_CAMERA:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (LOWORD(wp) == ID_RENDERER)  /* (FSR 2 offered or not) */
                    upscale_fill((int)SendMessageW(ctl(ID_UPSCALE), CB_GETCURSEL, 0, 0));
                s_settings_dirty = 1;
                set_text(ID_SETTINGS_STATUS, L"");
            }
            break;
        case ID_WINDOWED: case ID_FULLSCREEN: case ID_FULLSCREEN_EX: case ID_VSYNC: case ID_MUTE:
        case ID_PREPARE: case ID_PRIORITY: case ID_UNLOCK_ALL: case ID_SKIP_INTROS: case ID_SKIP_TRAINING:
            if (HIWORD(wp) == BN_CLICKED) {
                if (LOWORD(wp) == ID_WINDOWED || LOWORD(wp) == ID_FULLSCREEN || LOWORD(wp) == ID_FULLSCREEN_EX)
                    display_hint();
                s_settings_dirty = 1;
                set_text(ID_SETTINGS_STATUS, L"");
            }
            break;
        case ID_DEFAULTS:
            settings_defaults();
            break;
        case ID_ON_MODS_FOLD:
            s_mods_fold = !s_mods_fold;
            online_mods_fold_apply();
            break;
        case ID_ON_NAT_CHECK:
            s_mods_fold = 2;
            online_mods_fold_apply();
            nat_check_start();
            break;
        case ID_ON_ENABLE: case ID_ON_SHARE:
            if (HIWORD(wp) == BN_CLICKED) {
                s_settings_dirty = 1;
                set_text(ID_ON_STATUS, L"");
            }
            break;
        case ID_ON_NAME: case ID_ON_SERVER: case ID_ON_PEERS:
        case ID_ON_UP_KBS: case ID_ON_UP_PEERS: case ID_ON_MOD_MB: case ID_ON_MODS_MB:
            if (HIWORD(wp) == EN_CHANGE) {
                s_settings_dirty = 1;
                set_text(ID_ON_STATUS, L"");
            }
            break;
        case ID_ON_SERVER_KIND:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                online_server_show();
                s_settings_dirty = 1;
                set_text(ID_ON_STATUS, L"");
            }
            break;
        case ID_ON_SIGNIN:
            online_sign_in(0);
            break;
        case ID_ON_REGISTER:  /* (the sign-up form: first the server's word on invite codes) */
            {
                char st[512];
                const char *k;
                HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                s_on_invite = -1;  /* (an older server, or no answer: the box shown, sent as typed) */
                if (online_request("/api/status", NULL, NULL, st, sizeof st) == 200 &&
                        (k = strstr(st, "\"invite_needed\"")) != NULL) {
                    k += strlen("\"invite_needed\"");
                    while (*k == ' ' || *k == ':') k++;
                    s_on_invite = !strncmp(k, "true", 4) ? 1 : !strncmp(k, "false", 5) ? 0 : -1;
                }
                SetCursor(old);
                if (s_on_invite == 0)
                    set_text(ID_ON_CODE, L"");  /* (hidden: nothing typed earlier goes with the sign-up) */
            }
            s_on_mode = ON_SIGNUP;
            online_note(L"");
            online_account_show();
            break;
        case ID_ON_FORGOT:
            s_on_mode = ON_FORGOT;
            online_note(L"");
            online_account_show();
            break;
        case ID_ON_BACK:
            s_on_mode = ON_SIGNIN;
            online_note(L"");
            online_account_show();
            break;
        case ID_ON_STEP:
            online_step();
            break;
        case ID_ON_SIGNOUT:
            online_sign_out();
            break;
        case ID_FR_ADD:
            friends_change(1);
            break;
        case ID_FR_REMOVE:
            friends_change(0);
            break;
        case ID_ON_SAVE:
            settings_save();
            break;
        case ID_SAVE:
            settings_save();
            break;
        case ID_IMAGE_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_image(p)) {
                set_text(ID_IMAGE, p);
                check_image(p);
            }
            break;
        }
        case ID_IMAGE:
            if (HIWORD(wp) == EN_CHANGE)
                s_image_bytes = 0;
            break;
        case ID_TARGET:
            if (HIWORD(wp) == EN_CHANGE)
                update_free_space();
            break;
        case ID_TARGET_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_folder(L"Choose where to install the game", p))
                set_text(ID_TARGET, p);
            break;
        }
        case ID_INSTALL:
            start_install();
            break;
        case ID_VERIFY:
            start_verify();
            break;
        case ID_VERIFY_REPAIR:
            verify_repair();
            break;
        case ID_APK_CREATE:
            start_apk();
            break;
        case ID_ADB_INSTALL:
            start_adb();
            break;
        case ID_ANDROID_CANCEL:
            if (s_busy) {
                InterlockedExchange(&s_cancel, 1);
                set_text(ID_APK_STATUS, L"Cancelling\x2026");
            }
            break;
        case ID_DLC_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_folder(L"Choose the folder with the DLC", p))
                set_text(ID_DLC_DIR, p);
            break;
        }
        case ID_DLC_INSTALL:
            start_dlc();
            break;
        case ID_SV_BACKUP:   saves_backup();       break;
        case ID_SV_RESTORE:  saves_restore();      break;
        case ID_SV_BACKUPS:  saves_open_backups(); break;
        case ID_SV_EXPORT:   saves_export();       break;
        case ID_SV_IMPORT:   saves_import();       break;
        case ID_SV_DELETE:   saves_delete();       break;
        case ID_SV_CLEAN:    saves_clean();        break;
        case ID_SV_OPEN:     saves_open();         break;
        case ID_SV_CHANGE:   saves_change_folder(); break;
        case ID_SV_DEFAULT:  saves_set_folder(NULL); break;
        case ID_SV_PERMOD: {
            const int on = IsDlgButtonChecked(s_wnd, ID_SV_PERMOD) == BST_CHECKED;
            if (!toml_set("per_mod_saves", on ? "true" : "false")) {
                CheckDlgButton(s_wnd, ID_SV_PERMOD, on ? BST_UNCHECKED : BST_CHECKED);
                saves_status(L"Couldn't write the setting (" GAME_TOML L").");
            } else {
                saves_status(on ? L"Per-mod save files: on at the game's next start. Your saves are backed up first; "
                                  L"the next save moves the mods' data into Saves\\.mods."
                                : L"Per-mod save files: off at the game's next start. The mods' data goes back into "
                                  L"the main save at its next save; their files are kept.");
            }
            break;
        }
        case ID_SV_BIGROSTER: {
            const int on = IsDlgButtonChecked(s_wnd, ID_SV_BIGROSTER) == BST_CHECKED;
            if (on && !big_roster_confirm(NULL)) {  /* (ticked on: on only with Turn on) */
                CheckDlgButton(s_wnd, ID_SV_BIGROSTER, BST_UNCHECKED);
                saves_status(L"More superstar mods at once: left off.");
                break;
            }
            if (!toml_set("big_roster", on ? "true" : "false") || (on && !toml_set("per_mod_saves", "true"))) {
                CheckDlgButton(s_wnd, ID_SV_BIGROSTER, on ? BST_UNCHECKED : BST_CHECKED);
                saves_status(L"Couldn't write the setting (" GAME_TOML L").");
            } else {
                saves_status(on ? L"More superstar mods at once: on at the game's next start (experimental), with "
                                  L"per-mod save files."
                                : L"More superstar mods at once: off at the game's next start (per-mod save files "
                                  L"stay on until you untick them).");
            }
            saves_permod_lock();
            break;
        }
        case ID_SV_PRESET_NEW: presets_new();    break;
        case ID_SV_IMPORT360:  saves_import_360(); break;
        case ID_SV_IMPORTCAW:  saves_import_caw(); break;
        case ID_SV_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE)
                presets_pick();
            break;
        case ID_PT_EXPORT:    pt_export();         break;
        case ID_PT_IMPORT:    pt_change(1);        break;
        case ID_PT_DELETE:    pt_change(0);        break;
        case ID_PT_EXPORTALL: pt_export_all();     break;
        case ID_PT_REFRESH:   pt_refresh();        break;
        case ID_PT_PREV:      s_pt_page = (s_pt_page + PT_PAGES - 1) % PT_PAGES; pt_refresh(); break;
        case ID_PT_NEXT:      s_pt_page = (s_pt_page + 1) % PT_PAGES; pt_refresh(); break;
        case ID_CANCEL:
            if (s_busy) {
                InterlockedExchange(&s_cancel, 1);
                set_text(ID_INSTALL_STATUS, L"Cancelling\x2026");
            }
            break;
        }
        break;
    case WM_APP_PROGRESS:
        SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, wp, 0);
        if (lp) {
            if (!s_cancel)
                set_text(ID_INSTALL_STATUS, (WCHAR *)lp);
            free((void *)lp);
        }
        break;
    case WM_APP_APK:
        apk_message(wp, (WCHAR *)lp);
        return 0;
    case WM_APP_UPD_CHECKED:
        up_checked((int)wp, (WCHAR *)lp);
        return 0;
    case WM_APP_VERIFY:
        if (wp > 1000) {
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
            verify_done();
        } else {
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, wp, 0);
        }
        return 0;
    case WM_APP_NATCHECK:
        nat_check_show();
        return 0;
    case WM_APP_FRIENDS:
        friends_show((int)wp, (char *)lp);
        free((void *)lp);
        return 0;
    case WM_APP_SIZE:
        play_notice();
        return 0;
    case WM_TIMER:
        if (wp == HERO_TIMER) {
            RECT hr;
            if (s_cur_tab != TAB_PLAY || !s_hero_moving || !IsWindowVisible(w) || IsIconic(w)) {
                KillTimer(w, HERO_TIMER);
                return 0;
            }
            s_hero_scroll++;
            hero_rect(&hr);
            InvalidateRect(w, &hr, FALSE);
            return 0;
        }
        if (wp == FRIENDS_TIMER && s_tab_visible_online && IsWindowVisible(w) && !IsIconic(w))
            friends_fetch(NULL);
        return 0;
    case WM_APP_UPD_PROGRESS:
        SendMessageW(ctl(ID_UP_PROGRESS), PBM_SETPOS, wp, 0);
        if (lp) up_status(L"Installing version %s\x2026", s_up.version);
        return 0;
        return 0;
    case WM_APP_UPD_REPAIR:
        up_complete();
        return 0;
    case WM_APP_UPD_FINISHED:
        up_finished((int)wp, (WCHAR *)lp);
        return 0;
    case WM_APP_BUNDLED:
        bundled_message(wp);
        return 0;
    case WM_APP_MOVIE:
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, wp, 0);
        return 0;
    case WM_APP_MOVIE_DONE:
        mv_done((int)wp);
        return 0;
    case WM_APP_DLC: {
        WCHAR *msg = (WCHAR *)lp;
        if (msg)
            set_text(ID_DLC_STATUS, msg);
        if (wp) {
            WCHAR list[1300];
            swprintf_s(list, 1300, L"%s", s_dlc.names[0] ? s_dlc.names + 1 : L"");
            set_text(ID_DLC_LIST, list);
            EnableWindow(ctl(ID_DLC_INSTALL), TRUE);
            EnableWindow(ctl(ID_DLC_BROWSE), TRUE);
        }
        free(msg);
        break;
    }
    case WM_APP_DONE: {
        WCHAR *msg = (WCHAR *)lp, t[1400];
        set_busy(0);
        if (wp) {
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 1000, 0);
            wcscpy_s(s_game_dir, MAX_PATH, s_job.target);
            save_launcher_ini();
            settings_load();
            swprintf_s(t, 1400, L"Installed in %s. %s", s_game_dir, msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            show_tab(TAB_PLAY);
            if (has_program(s_game_dir))
                set_text(ID_PLAY_STATUS, L"Installed. Ready.");
        } else {
            swprintf_s(t, 1400, L"The game was not installed. %s", msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
        }
        free(msg);
        break;
    }
    case WM_APP_GAMEEND: {
        WCHAR t[200];
        refresh_play();
        if (wp == 0)
            swprintf_s(t, 200, L"Ready. The game exited normally (exit code 0).");
        else
            swprintf_s(t, 200, L"The game exited with code %lu (0x%08lX).", (unsigned long)wp, (unsigned long)wp);
        set_text(ID_PLAY_STATUS, t);
        break;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        s_dpi = HIWORD(wp);
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        relayout();
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)s_field_brush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* (text on a card: its colour; on the page: the page's) */
        const int on_card = card_at((HWND)lp), id = GetDlgCtrlID((HWND)lp);
        SetTextColor((HDC)wp, id == ID_PLAY_STATUS && s_play_ready ? C_GOOD : id == ID_UP_STATUS ? C_MUTED :
                              id == ID_ON_ROUTE ? (s_route_kind == 1 ? C_GOOD : s_route_kind == 2 ? C_WARN : C_MUTED) : C_TEXT);
        if ((GetWindowLongW((HWND)lp, GWL_STYLE) & ES_READONLY) && m == WM_CTLCOLORSTATIC) {
            WCHAR cn[16];
            if (GetClassNameW((HWND)lp, cn, 16) && !_wcsicmp(cn, L"Edit")) {   /* (read-only fields) */
                SetBkColor((HDC)wp, C_FIELD);
                return (LRESULT)s_field_brush;
            }
        }
        if (GetDlgCtrlID((HWND)lp) == ID_VERIFY_MARK) {   /* (green OK, red damaged, amber can't tell) */
            static const COLORREF c[] = { RGB(112, 112, 122), RGB(112, 112, 122), RGB(24, 150, 60),
                                          RGB(208, 20, 44), RGB(214, 140, 0) };
            SetTextColor((HDC)wp, c[s_verify_state]);
        }
        SetBkColor((HDC)wp, on_card ? C_CARD : C_PAGE);
        return (LRESULT)(on_card ? s_card_brush : s_page_brush);
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON && draw_button_buffered((DRAWITEMSTRUCT *)lp))
            return TRUE;
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_STATIC && discover_draw((DRAWITEMSTRUCT *)lp))
            return TRUE;
        break;
    case WM_APP_DISCOVER:
        discover_message(m, wp, lp);
        return 0;
    case WM_APP_REPORTS:
        reports_message(m, wp, lp);
        return 0;
    case WM_APP_SUGGESTIONS:
        suggestions_message(m, wp, lp);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int k = nav_at(pt.x, pt.y);
        if (PtInRect(&s_credit_link, pt)) {
            ShellExecuteW(w, L"open", REPO_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (PtInRect(&s_paypal_link, pt)) {
            ShellExecuteW(w, L"open", PAYPAL_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (PtInRect(&s_kofi_link, pt)) {
            ShellExecuteW(w, L"open", KOFI_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (k >= 0 && k_nav[k].tab == TAB_SETUP && setup_step(s_cur_tab) < 0)
            show_tab(setup_first_step());
        else if (k >= 0 && k_nav[k].tab != nav_tab(s_cur_tab))
            show_tab(k_nav[k].tab);
        else if (k < 0 && step_at(pt) >= 0)
            show_tab(k_steps[step_at(pt)]);
        else if (k < 0 && mod_tab_at(pt) >= 0)
            show_tab(k_mod_tabs[mod_tab_at(pt)]);
        else if (k < 0 && play_link_at(pt) >= 0)
            show_tab(play_link_at(pt) == TAB_SETUP ? setup_first_step() : play_link_at(pt));
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int k = nav_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (k != s_nav_hover) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, w, 0 };
            RECT sb = { 0, 0, S(SB_W), S(CLIENT_H + DY) };
            s_nav_hover = k;
            TrackMouseEvent(&t);
            InvalidateRect(w, &sb, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s_nav_hover >= 0) {
            RECT sb = { 0, 0, S(SB_W), S(CLIENT_H + DY) };
            s_nav_hover = -1;
            InvalidateRect(w, &sb, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if ((HWND)wp == w && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(w, &pt);
            if (nav_at(pt.x, pt.y) >= 0 || PtInRect(&s_credit_link, pt) || PtInRect(&s_kofi_link, pt) || PtInRect(&s_paypal_link, pt) ||
                play_link_at(pt) >= 0 || step_at(pt) >= 0 || mod_tab_at(pt) >= 0) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_PAINT: {
        /* (off-screen first, then copied: the sidebar's hover doesn't flicker) */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT cl;
        HDC mdc;
        HBITMAP bmp = NULL;
        GetClientRect(w, &cl);
        mdc = CreateCompatibleDC(dc);
        if (mdc)
            bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
        if (bmp) {
            HGDIOBJ old = SelectObject(mdc, bmp);
            FillRect(mdc, &cl, s_page_brush);
            paint_window(mdc);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, mdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(bmp);
        } else {
            paint_window(dc);
        }
        if (mdc)
            DeleteDC(mdc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (setup_busy() &&
            MessageBoxW(w, L"Stop the setup's download and close?", WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        if (s_busy) {
            if (MessageBoxW(w, L"Stop the installation and close?", WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            InterlockedExchange(&s_cancel, 1);
            if (s_worker)
                WaitForSingleObject(s_worker, 10000);
        }
        if (s_settings_dirty && dir_exists(s_game_dir)) {
            int r = MessageBoxW(w, L"Save the changed settings?", WINDOW_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES)
                settings_save();
        }
        save_launcher_ini();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* --capture: the window as a 24-bit BMP (for checking the layout). */
static void capture(const WCHAR *file)
{
    capture_wnd(s_wnd, file);
}

static void capture_wnd(HWND wnd, const WCHAR *file)
{
    RECT r;
    HDC wdc, mdc;
    HBITMAP bmp;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER fh;
    uint8_t *bits;
    int w, h, stride;
    FILE *f;
    GetWindowRect(wnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    wdc = GetDC(wnd);
    mdc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, w, h);
    SelectObject(mdc, bmp);
    PrintWindow(wnd, mdc, 2 /* PW_RENDERFULLCONTENT */);
    stride = (w * 3 + 3) & ~3;
    bits = (uint8_t *)malloc((size_t)stride * (size_t)h);
    if (bits) {
        memset(&bi, 0, sizeof bi);
        bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
        GetDIBits(mdc, bmp, 0, (UINT)h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
        memset(&fh, 0, sizeof fh);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof bi;
        fh.bfSize = fh.bfOffBits + (DWORD)(stride * h);
        if (!_wfopen_s(&f, file, L"wb") && f) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&bi, sizeof bi, 1, f);
            fwrite(bits, (size_t)stride * (size_t)h, 1, f);
            fclose(f);
        }
        free(bits);
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(wnd, wdc);
}

/* The Setup tab's "Install tab" button. */
static void setup_go_install(void) { show_tab(TAB_INSTALL); }

/* --setup-install's progress lines. */
static void setup_console_progress(int permille, const WCHAR *text)
{
    if (text) wprintf(L"[%3d%%] %s\n", permille / 10, text);
}

/* --install: print to the console the launcher was started from. */
static void console_setup(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    int inherited = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    AttachConsole(ATTACH_PARENT_PROCESS);
    if (inherited)
        s_out = h;                                   /* a pipe or file from the parent */
    else
        s_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    RECT r;
    int argc = 0, capture_tab = -1, capture_seq[8], capture_seq_n = 0, capture_verify = 0, updated = 0;
    int capture_report = 0;
    int capture_bundled = 0;  /* (--bundled-start: the start's bundled install too - timed, a shot while it runs) */
    const ULONGLONG t_start = GetTickCount64();
    ULONGLONG t_shown = 0;
    const WCHAR *capture_music = NULL, *const *capture_tag = NULL, *const *capture_mods = NULL;
    int capture_mods_show = 0;
    int capture_nat = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc), *slash, *capture_file = NULL, **capture_account = NULL;
    (void)prev; (void)cmd;

    s_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    GetModuleFileNameW(NULL, s_launcher_exe, MAX_PATH);
    wcscpy_s(s_launcher_dir, MAX_PATH, s_launcher_exe);
    slash = wcsrchr(s_launcher_dir, L'\\');
    if (slash)
        *slash = 0;
    join(s_launcher_ini, s_launcher_dir, L"launcher.ini");
    load_launcher_ini();
    if (argv && argc >= 2 && !wcscmp(argv[1], L"--updated"))   /* (restarted by an update) */
        updated = 1;
    if (argv && argc >= 2 && !wcscmp(argv[1], L"--repair-failed"))   /* (restarted by a repair that failed) */
        s_repair_failed = 1;
    if (updated || s_repair_failed)
        update_log(UPLOG_DIR, "launcher", L"started again by the updater (%s), version %s",
                   updated ? L"updated" : L"the repair failed", PORT_VERSION);
    {   /* the launcher an update replaced, and other program files it couldn't delete then (<name>.old) */
        WCHAR old[MAX_PATH + 8], pat[MAX_PATH], f[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        int k, d;
        swprintf_s(old, MAX_PATH + 8, L"%s.old", s_launcher_exe);
        for (k = 0; k < 20 && file_exists(old) && !DeleteFileW(old); k++)
            Sleep(250);
        for (d = 0; d < 2; d++) {
            const WCHAR *dir = d ? s_game_dir : s_launcher_dir;
            if (!dir[0] || (d && same_dir(s_game_dir, s_launcher_dir)) || !join(pat, dir, L"*.old") ||
                (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
                continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && join(f, dir, fd.cFileName))
                    DeleteFileW(f);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    /* The VC++ runtime DLLs 2.1.0 shipped next to the programs (14.29): an overlay built for the newer runtime
       players have (Discord, Steam, RTSS, NVIDIA...) failed in our processes there - "unable to start correctly
       (0xc0000142)". Gone from both folders at every start (so after an update too); Windows' own is used. */
    up_place_new_updater();   /* (the updater's own new copy from the last update) */
    setup_vcrt_cleanup(s_launcher_dir, NULL, 0);
    if (s_game_dir[0] && !same_dir(s_game_dir, s_launcher_dir))
        setup_vcrt_cleanup(s_game_dir, NULL, 0);
    /* --vcrt [<folder>]: the same for a folder, and the system runtime's state (tests) */
    if (argv && argc >= 2 && !wcscmp(argv[1], L"--vcrt")) {
        WCHAR log[300], ver[40];
        int n, st;
        console_setup();
        n = argc >= 3 ? setup_vcrt_cleanup(argv[2], log, 300) : 0;
        if (n) wprintf(L"removed %d: %s\n", n, log);
        else if (argc >= 3) wprintf(L"nothing to remove in %s\n", argv[2]);
        st = setup_vcrt_system(ver, 40);
        wprintf(L"system runtime: %s %s\n", st == SETUP_VCRT_OK ? L"ok" : st == SETUP_VCRT_OLD ? L"older than needed"
                                                                       : L"missing", ver);
        return st == SETUP_VCRT_OK ? 0 : 3;
    }

    /* --install <image> <folder>: install without a window (tests). */
    if (argv && argc >= 5 && (!wcscmp(argv[1], L"--paint-export") || !wcscmp(argv[1], L"--paint-import"))) {
        console_setup();
        return pt_console(argv[1][8] == L'i', argv[2], _wtoi(argv[3]), argv[4]);
    }
    /* --update [<game folder>] [--from <version>] / --update-zip <release zip> [<game folder>]: the updater's
       own console modes (tests), run from here as before - the launcher waits for it and returns its code. */
    if (argv && argc >= 2 && (!wcscmp(argv[1], L"--update") || !wcscmp(argv[1], L"--update-zip"))) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR exe[MAX_PATH], cmd[2048];
        DWORD code = 1;
        int i;
        if (!join(exe, s_launcher_dir, UPDATER_EXE) || !file_exists(exe))
            return 1;
        swprintf_s(cmd, 2048, L"\"%s\"", exe);
        for (i = 1; i < argc; i++)
            swprintf_s(cmd + wcslen(cmd), 2048 - wcslen(cmd), L" \"%s\"", argv[i]);
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        {   /* (the updater writes to the same pipe / file / console as this launcher would) */
            HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
            if (o && o != INVALID_HANDLE_VALUE && GetFileType(o) != FILE_TYPE_UNKNOWN) {
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = o;
                si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            } else
                AttachConsole(ATTACH_PARENT_PROCESS);
        }
        if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, s_launcher_dir, &si, &pi))
            return 1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)code;
    }
    /* --movie-make <video> <bottom or -> <fit 0-2> <seconds> <out.bik> [size 320|640|1024] (tests; the
     * HD .bik fallback of an .mp4 entrance movie) */
    if (argv && argc >= 7 && !wcscmp(argv[1], L"--movie-make")) {
        static MovieJob job;
        console_setup();
        wcscpy_s(job.video, MAX_PATH, argv[2]);
        if (wcscmp(argv[3], L"-")) wcscpy_s(job.bottom, MAX_PATH, argv[3]);
        job.fit = _wtoi(argv[4]);
        job.max_seconds = _wtoi(argv[5]);
        wcscpy_s(job.out, MAX_PATH, argv[6]);
        if (argc >= 8) job.size = _wtoi(argv[7]);
        if (!movie_make(&job)) { wprintf(L"failed: %s\n", job.err); return 1; }
        wprintf(L"ok: %d frames\n", job.frames);
        return 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--install-dlc")) {   /* <folder> <game folder> */
        s_console = 1;
        console_setup();
        wcscpy_s(s_dlc.src, MAX_PATH, argv[2]);
        wcscpy_s(s_dlc.game, MAX_PATH, argv[3]);
        dlc_thread(NULL);
        return s_dlc.failed ? 1 : 0;
    }
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--adb-install")) {   /* <game folder> */
        WCHAR err[600] = L"";
        int ok;
        s_console = 1;
        console_setup();
        wcscpy_s(s_game_dir, MAX_PATH, argv[2]);
        if (!apk_find(s_game_dir, s_launcher_dir, s_apk_src, MAX_PATH)) {
            wprintf(L"failed: no Android\\SvR2011.apk\n");
            return 1;
        }
        ok = adb_get(apk_console_progress, err, 600) &&
             adb_install(s_adb, s_game_dir, s_apk_src, &s_cancel, apk_console_progress, err, 600);
        wprintf(ok ? L"ok: %s\n" : L"failed: %s\n", err);
        return ok ? 0 : 1;
    }
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--setup-check")) {   /* <game folder>: the Setup tab's rows */
        static const WCHAR *const state[] = { L"found", L"missing", L"optional" };
        SetupRow rows[SETUP_N];
        int k;
        console_setup();
        setup_check(argv[2], s_launcher_dir, rows);
        for (k = 0; k < SETUP_N; k++)
            wprintf(L"%s: %s - %s\n", rows[k].name, state[rows[k].state], rows[k].detail);
        return 0;
    }
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--nat-check")) {   /* <game folder>: the Online tab's connection check */
        WCHAR t[1400];
        console_setup();
        wcscpy_s(s_game_dir, MAX_PATH, argv[2]);
        nat_check_do();
        online_route_show();
        nat_check_text(&s_nat_result, nat_check_port(), s_route_mapped, t, 1400);
        wprintf(L"kind=%d step=%d shared=%d v6=%d fw=%d mapped=%d\n%s\n", s_nat_result.kind, s_nat_result.step,
                s_nat_result.shared, s_nat_result.v6, s_nat_result.fw, s_route_mapped, t);
        return 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--setup-install")) {   /* <game folder> <python|ffmpeg|all> --yes */
        WCHAR err[600] = L"", list[1024];
        unsigned long long total;
        unsigned items = !wcscmp(argv[3], L"python") ? 1u << SETUP_PYTHON
                       : !wcscmp(argv[3], L"ffmpeg") ? 1u << SETUP_FFMPEG
                       : !wcscmp(argv[3], L"all") ? (1u << SETUP_PYTHON) | (1u << SETUP_FFMPEG) : 0;
        int ok;
        console_setup();
        setup_describe(items, list, 1024, &total);
        wprintf(L"%s", list);
        if (!items || argc < 5 || wcscmp(argv[4], L"--yes")) {   /* (the downloads need a yes) */
            wprintf(L"not downloaded: add --yes\n");
            return 1;
        }
        ok = setup_install(argv[2], items, setup_console_progress, &s_cancel, err, 600);
        wprintf(ok ? L"ok\n" : L"failed: %s\n", err);
        return ok ? 0 : 1;
    }
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--report")) {   /* <game folder>: Report a problem, no window */
        WCHAR zip[MAX_PATH];
        int logs = 0, crashes = 0, ok;
        console_setup();
        if (argc >= 4) {   /* (... [description]: as the window's, at the top of report.txt) */
            static char d[8192];
            WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, d, sizeof d, NULL, NULL);
            ok = report_make_ex(argv[2], PORT_VERSION, d, zip, MAX_PATH, &logs, &crashes);
        } else {
            ok = report_make(argv[2], PORT_VERSION, zip, MAX_PATH, &logs, &crashes);
        }
        if (ok) wprintf(L"ok %s (%d logs, %d crash reports)\n", zip, logs, crashes);
        else wprintf(L"failed\n");
        return ok ? 0 : 1;
    }
    /* --report-send <game folder> <answers file>: Report a problem's Send, no window (tests). The file: UTF-8 lines
       server= problem= happens= what= before= contact= image=<path> (up to 5), match=<folder | latest | none>
       (default latest: as the window preselects), mods=1|0 (the tick, default 1), save=1|0 (the main save tick, default 1). Prints the match, the mods line,
       progress, "sent <id> seen_before <n>" or "not sent: <why>", what happened to the match and mods, and the zip. */
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--report-send")) {
        static char what[4096], line[4096], problem[120] = "Something else", happens[40] = "Every time", before[4096],
                    contact[200], server[300], sig[400], ver[40], desc[16384];
        WCHAR zip[MAX_PATH], wserver[300], pics[REPORT_MAX_IMAGES][MAX_PATH], summary[300];
        ReportUpload u;
        ReportResult r;
        FILE *f = NULL;
        int logs = 0, crashes = 0, npics = 0, i, with_mods = 1, with_save = 1;
        char *json, match_name[300] = "";
        static WCHAR wmatch[MAX_PATH] = L"latest", line16[600];
        static RepMatch rm;
        console_setup();
        if (_wfopen_s(&f, argv[3], L"rb") || !f) { wprintf(L"not sent: can't read %s\n", argv[3]); return 1; }
        while (fgets(line, sizeof line, f)) {
            char *v;
            size_t n;
            if (!memcmp(line, "\xEF\xBB\xBF", 3)) memmove(line, line + 3, strlen(line + 3) + 1);   /* (a BOM) */
            v = strchr(line, '=');
            n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (!v) continue;
            *v++ = 0;
            if (!strcmp(line, "server")) strcpy_s(server, sizeof server, v);
            else if (!strcmp(line, "problem")) strcpy_s(problem, sizeof problem, v);
            else if (!strcmp(line, "happens")) strcpy_s(happens, sizeof happens, v);
            else if (!strcmp(line, "what")) strcpy_s(what, sizeof what, v);
            else if (!strcmp(line, "before")) strcpy_s(before, sizeof before, v);
            else if (!strcmp(line, "contact")) strcpy_s(contact, sizeof contact, v);
            else if (!strcmp(line, "image") && npics < REPORT_MAX_IMAGES)
                MultiByteToWideChar(CP_UTF8, 0, v, -1, pics[npics++], MAX_PATH);
            else if (!strcmp(line, "match")) MultiByteToWideChar(CP_UTF8, 0, v, -1, wmatch, MAX_PATH);
            else if (!strcmp(line, "mods")) with_mods = atoi(v) != 0;
            else if (!strcmp(line, "save")) with_save = atoi(v) != 0;
        }
        fclose(f);
        /* (a test aid: never the live server by default - server= must be given; 26 Oct a BOM hid it and a test
           report went live) */
        if (!server[0]) { wprintf(L"not sent: no server= line in %s (give the test server)\n", argv[3]); return 1; }
        s_test_run = 1;
        {
            WCHAR ws[300];
            MultiByteToWideChar(CP_UTF8, 0, server, -1, ws, 300);
            if (test_run_refuses_url(ws, "/api/reports")) {
                wprintf(L"not sent: %s is not a server on this PC (SVR2011_ALLOW_LIVE=1 lets it through)\n", ws);
                return 1;
            }
        }
        wcscpy_s(s_game_dir, MAX_PATH, argv[2]);
        MultiByteToWideChar(CP_UTF8, 0, server, -1, wserver, 300);
        ZeroMemory(&rm, sizeof rm);
        if (!_wcsicmp(wmatch, L"latest")) {
            static ReportMatch list[REPORT_MAX_MATCHES];
            if (report_match_list(argv[2], list, REPORT_MAX_MATCHES) > 0) rm.match = list[0], rm.has_match = 1;
        } else if (wmatch[0] && _wcsicmp(wmatch, L"none")) {
            wcscpy_s(rm.match.dir, MAX_PATH, wmatch);
            report_match_label(wmatch, rm.match.label, 200);
            rm.has_match = GetFileAttributesW(wmatch) != INVALID_FILE_ATTRIBUTES;
        }
        if (rm.has_match) {
            rep_match_load(&rm, wserver);
            rep_match_mods_text(&rm, line16, 600);
            wprintf(L"match: %s\n%s\n", rm.match.label, line16);
            for (i = 0; i < rm.n_mods; i++)
                wprintf(L"  mod %s\\%s %S %.1f MB %S%s\n", rm.mods[i].type, rm.mods[i].id, rm.mods[i].version,
                        rm.mods[i].size / 1048576.0, rm.mods[i].hash[0] ? rm.mods[i].hash : "(not installed)",
                        rm.mods[i].on_server ? L" (on the server)" : L"");
        } else {
            wprintf(L"match: none\n");
        }
        snprintf(desc, sizeof desc, "Problem: %s\r\nHappens: %s\r\n\r\nWhat happened:\r\n%s\r\n\r\nJust before:\r\n%s\r\n",
                 problem, happens, what, before[0] ? before : "(not said)");
        if (!report_make_match(argv[2], PORT_VERSION, desc, NULL, with_save, zip, MAX_PATH, &logs, &crashes)) {
            wprintf(L"not sent: the zip could not be written\n");
            return 1;
        }
        WideCharToMultiByte(CP_UTF8, 0, PORT_VERSION, -1, ver, sizeof ver, NULL, NULL);
        report_signature(argv[2], sig, sizeof sig);
        {
            char upd[600];
            update_log_last(argv[2], upd, sizeof upd);
            json = report_json("launcher", ver, problem, happens, what, before, contact, sig, upd);
        }
        ZeroMemory(&u, sizeof u);
        u.json = json;
        u.zip = zip;
        for (i = 0; i < npics; i++) u.images[i] = pics[i];
        u.n_images = npics;
        rep_match_parts(&rm, with_mods, &u, match_name, sizeof match_name);
        u.progress = rep_cli_progress;
        if (with_save) u.save_bytes = report_main_save_size(argv[2]);
        report_send_summary(&u, summary, 300);
        wprintf(L"%s\n", summary);
        i = report_send(wserver, &u, &r);
        rep_match_done(&rm);
        free(json);
        if (i && r.secret[0] && receipt_add(argv[2], "launcher", r.id, r.secret, problem, what))
            wprintf(L"receipt kept: %s\\UserData\\reports.json\n", argv[2]);
        if (i) wprintf(L"sent %S seen_before %d (zip %s)\n", r.id, r.seen_before, zip);
        else wprintf(L"not sent: %s (HTTP %d; the zip is kept: %s)\n", r.why, r.status, zip);
        rep_match_result_text(&rm, &r, line16, 600);
        if (line16[0]) wprintf(L"%s\n", line16);
        return i ? 0 : 1;
    }
    /* --reports-status <game folder> <server> / --reports-reply <game folder> <server> <id> <text> [<picture>...]:
       the Reports tab's asks (report_receipts.h), no window (tests: a server on this PC only). */
    if (argv && argc >= 4 && (!wcscmp(argv[1], L"--reports-status") ||
                              ((!wcscmp(argv[1], L"--reports-reply") || !wcscmp(argv[1], L"--reports-edit")) && argc >= 6))) {
        static ReportReceipt items[RECEIPTS_MAX];
        WCHAR why[300], badge[60], line[1400];
        int n, i, ok;
        console_setup();
        s_test_run = 1;
        receipts_set_guard(test_run_refuses_url);
        n = receipts_load(argv[2], items, RECEIPTS_MAX);
        if (!wcscmp(argv[1], L"--reports-reply") || !wcscmp(argv[1], L"--reports-edit")) {
            const int edit = !wcscmp(argv[1], L"--reports-edit");
            char id[40], text[8192];
            const WCHAR *pics[RECEIPT_MAX_EDIT_IMAGES];
            int k = 0, pictures = -1;
            WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, id, sizeof id, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, argv[5], -1, text, sizeof text, NULL, NULL);
            for (i = 6; i < argc && k < (edit ? RECEIPT_MAX_EDIT_IMAGES : RECEIPT_MAX_IMAGES); i++) pics[k++] = argv[i];
            for (i = 0; i < n && strcmp(items[i].id, id); i++)
                ;
            if (i == n) { wprintf(L"not sent: no receipt for %s\n", argv[4]); return 1; }
            if (edit) {
                ok = receipt_edit(argv[3], NULL, &items[i], text, pics, k, &pictures, why, 300);
                if (ok) wprintf(L"change saved (%d pictures now)\n", pictures);
                else wprintf(L"not saved: %s\n", why);
                return ok ? 0 : 1;
            }
            ok = receipt_reply(argv[3], NULL, &items[i], text, pics, k, why, 300);
            wprintf(ok ? L"answer sent\n" : L"not sent: %s\n", why);
            return ok ? 0 : 1;
        }
        ok = receipts_status(argv[3], NULL, items, &n, RECEIPTS_MAX, why, 300);
        if (!ok) { wprintf(L"not asked: %s\n", why); return 1; }
        wprintf(L"%d report(s)\n", n);
        for (i = 0; i < n; i++) {
            receipt_state_text(&items[i], badge, 60, line, 1400);
            if (receipt_wants_pictures(&items[i])) wcscpy_s(badge, 60, L"Add your pictures");
            wprintf(L"%S  %S  %S  [%s]  %s  (edits %d, pictures %d)\n", items[i].id, items[i].app, items[i].date, badge,
                    line, items[i].edits, items[i].pictures);
        }
        return 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--import-360")) {   /* <360 save folder> <saves folder> */
        int skipped = 0, n;
        console_setup();
        n = stfs_import_saves(argv[2], argv[3], &skipped);
        wprintf(L"imported %d, unreadable %d\n", n, skipped);
        return n ? 0 : 1;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--clean-save")) {   /* <saves> <Mods\Superstars> [--with-off] [--apply] */
        static ScItem items[SC_MAX];
        WCHAR err[600] = L"";
        int i, n, with_off = 0, apply = 0;
        console_setup();
        for (i = 4; i < argc; i++)
            with_off |= !wcscmp(argv[i], L"--with-off"), apply |= !wcscmp(argv[i], L"--apply");
        if ((n = save_clean_scan(argv[2], argv[3], items, err, 600)) < 0) {
            wprintf(L"failed: %s\n", err);
            return 1;
        }
        for (i = 0; i < n; i++)
            wprintf(L"id %d\t%s\t%s\t%s%s%s%s%s refs %d\n", items[i].id, items[i].name,
                    items[i].folder[0] ? items[i].folder : L"-", items[i].off ? L"off " : L"",
                    items[i].profile ? L"profile " : L"", items[i].universe ? L"universe " : L"",
                    items[i].mark ? L"mark " : L"", items[i].mod_save ? L"mod_save" : L"", items[i].refs);
        wprintf(L"%d mods\n", n);
        if (!apply)
            return 0;
        n = save_clean_apply(argv[2], items, n, with_off, err, 600);
        if (n < 0)
            wprintf(L"failed: %s\n", err);
        else
            wprintf(L"cleaned %d\n", n);
        return n < 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--import-caw")) {   /* <saves folder> <file.cas>... */
        static WCHAR list[32768];
        WCHAR msg[1400];
        size_t at = 0;
        int i, n = 0;
        console_setup();
        for (i = 3; i < argc && at + wcslen(argv[i]) + 1 < 32768; i++, n++) {
            wcscpy_s(list + at, 32768 - at, argv[i]);
            at += wcslen(argv[i]) + 1;
        }
        n = caw_import_files(argv[2], list, n, msg, 1400);
        wprintf(L"%s\n", msg);
        return n ? 0 : 1;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--apk-package")) {   /* <game folder> <out folder> */
        WCHAR err[600] = L"";
        int ok;
        console_setup();
        wcscpy_s(s_game_dir, MAX_PATH, argv[2]);
        if (!apk_find(s_game_dir, s_launcher_dir, s_apk_src, MAX_PATH)) {
            wprintf(L"failed: no Android\\SvR2011.apk\n");
            return 1;
        }
        ok = apk_package(s_game_dir, s_apk_src, argv[3], &s_cancel, apk_console_progress, err, 600);
        wprintf(ok ? L"ok\n" : L"failed: %s\n", err);
        return ok ? 0 : 1;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--install")) {
        int ok;
        s_console = 1;
        console_setup();
        wcscpy_s(s_job.image, MAX_PATH, argv[2]);
        if (!GetFullPathNameW(argv[3], MAX_PATH, s_job.target, NULL))
            wcscpy_s(s_job.target, MAX_PATH, argv[3]);
        post_progress(0, L"Installing %s", s_job.image);
        post_progress(0, L"        to %s", s_job.target);
        ok = install_run(&s_job);
        post_progress(0, L"%s: %s", ok ? L"installed" : L"failed", s_install_msg);
        return ok ? 0 : 1;
    }
    /* Tests: --mods-add <game folder> <file.svrmod> installs a mod as the Mods
       tab's + does (exit code 0 when installed). */
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--mods-add")) {
        int ok;
        console_setup();  /* (what the Mods tab would say, printed) */
        ok = mods_install(argv[2], argv[3]);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* Tests: --mods-shrink <game folder> - the Mods tab's Shrink movies (the result printed). */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--mods-shrink")) {
        int ok;
        console_setup();
        ok = mods_shrink(argv[2]);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* Tests: --mods-level <game folder> - the Mods tab's Level music (the gain per song printed). */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--mods-level")) {
        int ok;
        console_setup();
        ok = mods_level(argv[2]);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* Tests: --bg-add <game folder> <picture> [logo] makes a main menu
       background as the Mods tab's "Add background picture" does. */
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--bg-add")) {
        int ok;
        console_setup();
        ok = mods_add_background(argv[2], argv[3], argc >= 5 && !wcscmp(argv[4], L"logo"));
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* Tests: --mods-superstars <game folder> lists the superstar mods, those
       over the game's id limit marked, as the Mods tab does. */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--mods-superstars")) {
        console_setup();
        mods_print_superstars(argv[2]);
        LocalFree(argv);
        return 0;
    }
    /* Tests: --mods-attires <game folder> moves clashing attire mods as Play does and prints what it said. */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--mods-attires")) {
        console_setup();
        mods_print_attires(argv[2]);
        LocalFree(argv);
        return 0;
    }
    /* Tests: --mods-remove <game folder> <mod folder> removes a mod as the Mods
       list's Remove does (a superstar mod's slots.txt line goes too). */
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--mods-remove")) {
        const int ok = mods_remove(argv[2], argv[3]);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* Tests: --mods-rename <mod folder> <name> renames a background as editing
       its name in the Mods list does. */
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--mods-rename")) {
        const int ok = mods_rename(argv[2], argv[3]);
        LocalFree(argv);
        return ok ? 0 : 1;
    }
    /* --finish-update <game folder>: what launchers up to 2.0.3 left out of an
       update (Bundled Mods, pad_icons) fetched and installed, no window. */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--finish-update")) {
        WCHAR err[600] = L"";
        int ok;
        console_setup();
        wcscpy_s(s_game_dir, MAX_PATH, argv[2]);
        wprintf(L"unfinished: %d\n", up_unfinished());
        ok = up_finish_run(NULL, err, 600);
        if (ok)
            install_bundled(s_game_dir, s_game_dir);
        wprintf(ok ? L"ok\n" : L"failed: %s\n", err);
        return ok ? 0 : 1;
    }
    /* --verify <game folder>: Verify game files, no window (exit 0: OK). */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--verify")) {
        VerifyResult *r = (VerifyResult *)calloc(1, sizeof *r);
        WCHAR msg[600];
        int i, ok;
        console_setup();
        if (!r)
            return 1;
        ok = verify_game_files(argv[2], NULL, NULL, NULL, r);
        verify_describe(r, msg, 600);
        wprintf(L"%s\n", msg);
        for (i = 0; i < r->bad_count; i++)
            wprintf(L"  %s %s\n", r->bad_missing[i] ? L"missing" : L"damaged", r->bad[i]);
        ok = ok && r->files > 0 && !r->missing && !r->damaged && !r->other_edition;
        free(r);
        return ok ? 0 : 1;
    }
    /* Tests: --bundled <game folder> installs <game>\Bundled Mods as a start does. */
    if (argv && argc >= 3 && !wcscmp(argv[1], L"--bundled")) {
        /* ... --answer yes|no: the answer to its questions (an edited copy with a newer version: update / keep;
           files missing: fix / not now); without it they're only logged (Mods\.install.log) */
        mods_bundled_reset();
        if (argc >= 5 && !wcscmp(argv[3], L"--answer")) mods_bundled_answer(NULL, !wcscmp(argv[4], L"yes") ? BUNDLED_YES : BUNDLED_NO);
        install_bundled(argv[2], argv[2]);
        LocalFree(argv);
        return 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--capture")) {
        static const WCHAR *names[TAB_COUNT] = { L"play", L"settings", L"online", L"install", L"dlc", L"saves", L"paint", L"movies", L"android", L"mods", L"music", L"roster", L"texpacks", L"setup", L"discover", L"tools", L"reports", L"suggestions" };
        int i;
        const WCHAR *p = argv[2];
        /* A comma list: each tab is shown in turn (after 300 ms), the last captured. */
        capture_seq_n = 0;
        while (*p && capture_seq_n < 8) {
            const WCHAR *c = wcschr(p, L',');
            size_t len = c ? (size_t)(c - p) : wcslen(p);
            for (i = 0; i < TAB_COUNT; i++)
                if (wcslen(names[i]) == len && !_wcsnicmp(p, names[i], len))
                    capture_seq[capture_seq_n++] = i;
            p += len + (c ? 1 : 0);
        }
        if (capture_seq_n)
            capture_tab = capture_seq[0];
        capture_file = argv[3];
        s_test_run = 1;  /* (no account request to a server off this PC: online_test_refused) */
        /* ... --verify: Verify game files first (the Install tab's mark). */
        if (argc >= 5 && !wcscmp(argv[4], L"--verify"))
            capture_verify = 1;
        /* ... --paint-page <1-10>: the Paint Tool tab shows that page. */
        if (argc >= 6 && !wcscmp(argv[4], L"--paint-page"))
            s_pt_page = (_wtoi(argv[5]) - 1 + PT_PAGES) % PT_PAGES;
        /* ... --sign-in|--register <name> <password>: the Online tab's account
           buttons, against the configured server. */
        if (argc >= 7 && (!wcscmp(argv[4], L"--sign-in") || !wcscmp(argv[4], L"--register")))
            capture_account = argv + 4;
        /* ... --music-add <file>: the Music tab's Add songs with that file. */
        if (argc >= 6 && !wcscmp(argv[4], L"--music-add"))
            capture_music = argv[5];
        /* ... --roster-tag <id> <SMACKDOWN|RAW|NPC|MODS|DEFAULT>: the Roster tab tags it. */
        if (argc >= 7 && !wcscmp(argv[4], L"--roster-tag"))
            capture_tag = argv + 5;
        /* ... --online-mods <share 0|1> <kbs> <peers> <mod MB> <session MB>: the Online tab's Mod sharing rows
           filled and saved (svr2011.toml), then the fold captured open. */
        if (argc >= 10 && !wcscmp(argv[4], L"--online-mods"))
            capture_mods = argv + 5;
        if (argc >= 6 && !wcscmp(argv[4], L"--online-mods") && !wcscmp(argv[5], L"show"))  /* (only the fold, as loaded) */
            capture_mods_show = 1;
        /* ... --nat-check: the connection check run, its fold captured open. */
        if (argc >= 5 && !wcscmp(argv[4], L"--nat-check"))
            capture_nat = 1;
        /* ... --report-window <1|2>: Report a problem's window (questions / where to post) is captured instead. */
        if (argc >= 6 && !wcscmp(argv[4], L"--report-window"))
            capture_report = _wtoi(argv[5]);
        /* ... --big-roster-warning: the big roster's Turn on / Cancel warning is captured instead. */
        if (argc >= 5 && !wcscmp(argv[4], L"--big-roster-warning"))
            capture_report = -1;
        /* ... --bundled-start: the start's bundled-mod install runs as without --capture (after the window shows,
           on a worker); <file>.during.png is taken while it runs, <file>.times.txt says when the window showed and
           when the install was done (ms from the start), and the capture waits for it (2 minutes at most). */
        if (argc >= 5 && !wcscmp(argv[4], L"--bundled-start"))
            capture_bundled = 1;
        /* ... --bundled-start --click-play: Play is clicked as soon as the install starts (it waits for it, then
           starts the game - the folder's svr2011.exe; tests put a stand-in there). */
        if (capture_bundled && argc >= 6 && !wcscmp(argv[5], L"--click-play"))
            capture_bundled = 2;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    s_dpi = (int)GetDpiForSystem();
    make_fonts();
    theme_init();
    s_page_brush = CreateSolidBrush(C_PAGE);
    s_card_brush = CreateSolidBrush(C_CARD);
    s_field_brush = CreateSolidBrush(C_FIELD);
    hero_find();
    /* The game's own icon (launcher.rc, from its Xbox 360 title image). */
    s_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = s_page_brush;
    wc.lpszClassName = L"SvR2011Launcher";
    wc.hIcon = s_icon;
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wc);
    r.left = 0; r.top = 0; r.right = S(SB_W + CLIENT_W); r.bottom = S(CLIENT_H + DY);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, (UINT)s_dpi);
    s_wnd = CreateWindowExW(0, wc.lpszClassName, MAIN_TITLE,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!s_wnd)
        return 1;
    {   /* the dark title bar (Windows 10 2004+ / 11; older ones ignore it) - imported normally, not looked up
           (GetProcAddress lookups are one more thing antivirus heuristics weigh) */
        BOOL on = TRUE;
        DwmSetWindowAttribute(s_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
    }
    build_ui();
    refresh_play();
    /* Restarted by an update (--updated): what an older launcher left out. */
    if (!capture_file && updated && up_unfinished())
        up_finish();
    if (capture_music)
        music_add_files(s_game_dir, &capture_music, 1);
    if (capture_tag)
        roster_tag(s_game_dir, _wtoi(capture_tag[0]), capture_tag[1]);
    if (capture_mods) {
        CheckDlgButton(s_wnd, ID_ON_SHARE, _wtoi(capture_mods[0]) ? BST_CHECKED : BST_UNCHECKED);
        set_text(ID_ON_UP_KBS, capture_mods[1]), set_text(ID_ON_UP_PEERS, capture_mods[2]);
        set_text(ID_ON_MOD_MB, capture_mods[3]), set_text(ID_ON_MODS_MB, capture_mods[4]);
        settings_save();
        s_mods_fold = 1;
    }
    if (capture_mods_show)
        s_mods_fold = 1;
    if (capture_nat) {  /* (as its button does: the thread, its message; waiting here for the picture) */
        const ULONGLONG t0 = GetTickCount64();
        s_mods_fold = 2;
        SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(ID_ON_NAT_CHECK, BN_CLICKED), (LPARAM)ctl(ID_ON_NAT_CHECK));
        while (s_nat_busy && GetTickCount64() - t0 < 20000) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(10);
        }
    }
    /* The first start (launcher.ini SetupShown): the Setup tab, what's found and what's missing. */
    if (capture_tab < 0 && !GetPrivateProfileIntW(L"Launcher", L"SetupShown", 0, s_launcher_ini)) {
        WritePrivateProfileStringW(L"Launcher", L"SetupShown", L"1", s_launcher_ini);
        capture_tab = TAB_SETUP;
    }
    show_tab(capture_tab >= 0 ? capture_tab
                             : (is_game_folder(s_game_dir) && !install_missing(s_game_dir) ? TAB_PLAY : TAB_INSTALL));
    if (capture_file) {
        /* Drawn off-screen, never activated (tests run beside the user's work). */
        SetWindowPos(s_wnd, NULL, -8000, -8000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        show = SW_SHOWNOACTIVATE;
    }
    if (!capture_file) {
        /* Always shown normally, never the show state it was started with: a launcher started hidden
           (Start-Process -WindowStyle Hidden, a hidden parent's restart) ran with an invisible window. The
           first ShowWindow can take the starter's state over: shown again if it stayed hidden. On a place no
           monitor shows: onto the primary one. */
        show = SW_SHOWNORMAL;
        if (!MonitorFromWindow(s_wnd, MONITOR_DEFAULTTONULL))
            SetWindowPos(s_wnd, NULL, 80, 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(s_wnd, show);
    if (!capture_file && !IsWindowVisible(s_wnd))
        ShowWindow(s_wnd, SW_SHOWNORMAL);
    UpdateWindow(s_wnd);
    t_shown = GetTickCount64() - t_start;
    /* Mods that came with the port and aren't in the game yet (an update can bring new ones): once the window shows,
       on a worker (start_bundled; they used to be installed before the window was, which an update with many to
       replace left invisible for minutes). */
    s_bundled_no_ask = capture_file != NULL;
    if ((!capture_file || capture_bundled) && is_game_folder(s_game_dir))
        start_bundled();
    if (capture_bundled == 2)
        PostMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(ID_PLAY, BN_CLICKED), (LPARAM)ctl(ID_PLAY));
    up_check_finished();
    if (!capture_file && GetPrivateProfileIntW(L"Launcher", L"CheckUpdates", 1, s_launcher_ini))
        up_check(1);
    if (capture_account) {
        set_text(ID_ON_NAME, capture_account[1]);
        set_text(ID_ON_PASSWORD, capture_account[2]);
        online_sign_in(capture_account[0][2] == L'r');
    }
    {   /* (tests, with --capture online) SVR2011_CAPTURE_ONLINE=<step>;<step>...: the account box's fields and
           buttons - name=, password=, code= (typed), click=register|forgot|back|step|signin */
        const char *v = capture_file ? getenv("SVR2011_CAPTURE_ONLINE") : NULL;
        WCHAR w[1024], *ctx = NULL, *t;
        if (v && *v) {
            MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 1024);
            for (t = wcstok_s(w, L";", &ctx); t; t = wcstok_s(NULL, L";", &ctx)) {
                static const struct { const WCHAR *k; int id; } fields[] = {
                    { L"name=", ID_ON_NAME }, { L"password=", ID_ON_PASSWORD }, { L"code=", ID_ON_CODE } };
                static const struct { const WCHAR *k; int id; } clicks[] = {
                    { L"click=register", ID_ON_REGISTER }, { L"click=forgot", ID_ON_FORGOT }, { L"click=back", ID_ON_BACK },
                    { L"click=step", ID_ON_STEP }, { L"click=signin", ID_ON_SIGNIN } };
                size_t i;
                for (i = 0; i < sizeof fields / sizeof fields[0]; i++)
                    if (!wcsncmp(t, fields[i].k, wcslen(fields[i].k)))
                        set_text(fields[i].id, t + wcslen(fields[i].k));
                for (i = 0; i < sizeof clicks / sizeof clicks[0]; i++)
                    if (!wcscmp(t, clicks[i].k))
                        SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(clicks[i].id, BN_CLICKED), (LPARAM)ctl(clicks[i].id));
            }
        }
    }
    if (capture_file) {
        MSG pm;
        int k;
        if (capture_verify) {
            ULONGLONG t0 = GetTickCount64();
            start_verify();
            while (s_verify_busy && GetTickCount64() - t0 < 300000)
                if (MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
                    while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pm);
                        DispatchMessageW(&pm);
                    }
        }
        for (k = 0; k < (capture_seq_n ? capture_seq_n : 1); k++) {
            ULONGLONG t0 = GetTickCount64();
            {   /* SVR2011_CAPTURE_SIGNIN_AT=<k>|<name>|<password>: signed in as that account (name "-": signed out)
                   before the sequence's k-th tab (Discovery's owner buttons after an account change) */
                const char *v = getenv("SVR2011_CAPTURE_SIGNIN_AT");
                WCHAR w[300], *name, *pw;
                if (v && *v && atoi(v) == k && k) {
                    MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 300);
                    if ((name = wcschr(w, L'|')) != NULL) {
                        *name++ = 0;
                        if ((pw = wcschr(name, L'|')) != NULL) *pw++ = 0;
                        if (!wcscmp(name, L"-")) {
                            online_sign_out();
                        } else if (pw) {
                            set_text(ID_ON_NAME, name);
                            set_text(ID_ON_PASSWORD, pw);
                            online_sign_in(0);
                        }
                    }
                }
            }
            if (k) {
                /* as a click on the tab does */
                NMHDR n = { s_tab, ID_TAB, TCN_SELCHANGE };
                TabCtrl_SetCurSel(s_tab, capture_seq[k]);
                SendMessageW(s_wnd, WM_NOTIFY, ID_TAB, (LPARAM)&n);
            }
            /* (the Online tab: until the Friends list is in, 5 s at most) */
            while (GetTickCount64() - t0 < 400 || (s_fr_busy && GetTickCount64() - t0 < 5000) ||
                   (s_cur_tab == TAB_DISCOVER && GetTickCount64() - t0 < 4000) ||   /* (its list and pictures) */
                   (s_cur_tab == TAB_REPORTS && reports_test_state(NULL, NULL, 0) && GetTickCount64() - t0 < 10000) ||
                   (s_cur_tab == TAB_SUGGESTIONS && suggestions_test_state(NULL, NULL, 0) && GetTickCount64() - t0 < 10000))
                while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&pm);
                    DispatchMessageW(&pm);
                }
        }
        if (s_cur_tab == TAB_REPORTS) {
            /* Reports: the list and the status line in <file>.reports.txt; SVR2011_REPORTS_PICK=<row> selects one
               (its line, the answer box) before the shot */
            WCHAR log[MAX_PATH], line[400];
            FILE *f = NULL;
            int n = 0;
            const char *pick = getenv("SVR2011_REPORTS_PICK");
            if (pick) reports_test_pick(atoi(pick));
            if (pick && getenv("SVR2011_REPORTS_EDIT")) reports_test_edit();   /* (its change box open) */
            reports_test_state(&n, line, 400);
            swprintf_s(log, MAX_PATH, L"%s.reports.txt", capture_file);
            if (!_wfopen_s(&f, log, L"w, ccs=UTF-8") && f) {
                fwprintf(f, L"reports: %d\nstatus: %s\n", n, line);
                reports_test_dump(f);
                fclose(f);
            }
        }
        if (s_cur_tab == TAB_SUGGESTIONS) {
            /* Suggestions: SVR2011_SUGGEST_SEND=<title>|<text>[|<picture>] sends one as the Send button does (its
               answer waited for, 15 s at most), SVR2011_SUGGEST_PICK=<row> selects one; the list and the status line
               in <file>.suggestions.txt */
            WCHAR log[MAX_PATH], line[400];
            FILE *f = NULL;
            int n = 0;
            const char *send = getenv("SVR2011_SUGGEST_SEND"), *pick = getenv("SVR2011_SUGGEST_PICK");
            if (send) {
                char buf[2000], *text, *pic;
                WCHAR wpic[MAX_PATH] = L"";
                ULONGLONG t1;
                strcpy_s(buf, sizeof buf, send);
                if ((text = strchr(buf, '|')) != NULL) *text++ = 0;
                if (text && (pic = strchr(text, '|')) != NULL) {
                    *pic++ = 0;
                    MultiByteToWideChar(CP_UTF8, 0, pic, -1, wpic, MAX_PATH);
                }
                suggestions_test_send(buf, text ? text : "", wpic);
                for (t1 = GetTickCount64(); suggestions_test_state(NULL, NULL, 0) && GetTickCount64() - t1 < 15000;)
                    while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pm);
                        DispatchMessageW(&pm);
                    }
            }
            if (pick) suggestions_test_pick(atoi(pick));
            suggestions_test_state(&n, line, 400);
            swprintf_s(log, MAX_PATH, L"%s.suggestions.txt", capture_file);
            if (!_wfopen_s(&f, log, L"w, ccs=UTF-8") && f) {
                fwprintf(f, L"suggestions: %d\nstatus: %s\n", n, line);
                suggestions_test_dump(f);
                fclose(f);
            }
        }
        if (s_cur_tab == TAB_DISCOVER) {
            /* Discovery: a download running (SVR2011_DISCOVER_DOWNLOAD) is waited for (15 minutes at most), its
               status lines logged with times to <file>.discover.txt, shots <file>.during.bmp (past 30 %) and
               <file>.reconnect.bmp (the first "connection lost"); then whether Delete shows */
            WCHAR log[MAX_PATH], line[400], last[400] = L"", shot[MAX_PATH];
            ULONGLONG t0 = GetTickCount64();
            int permille = -1, owner = 0, during = 0, lost = 0;
            FILE *f = NULL;
            swprintf_s(log, MAX_PATH, L"%s.discover.txt", capture_file);
            _wfopen_s(&f, log, L"w, ccs=UTF-8");
            while (getenv("SVR2011_DISCOVER_DOWNLOAD") && !discover_test_state(NULL, NULL, NULL, 0) &&
                   GetTickCount64() - t0 < 15000)   /* (it starts when the mod's page is in) */
                if (MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
                    while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pm);
                        DispatchMessageW(&pm);
                    }
            t0 = GetTickCount64();
            while ((discover_test_state(&permille, &owner, line, 400) || !wcsncmp(line, L"Reconnecting", 12)) &&
                   GetTickCount64() - t0 < 900000) {   /* (a download, or the list waiting for the server) */
                if (MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
                    while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pm);
                        DispatchMessageW(&pm);
                    }
                discover_test_state(&permille, &owner, line, 400);
                if (wcscmp(line, last) && f) {
                    fwprintf(f, L"%6.1f s  %s\n", (GetTickCount64() - t0) / 1000.0, line);
                    wcscpy_s(last, 400, line);
                }
                if (!during && permille >= 300) {
                    swprintf_s(shot, MAX_PATH, L"%s.during.bmp", capture_file);
                    capture(shot);
                    during = 1;
                }
                if (!lost && (wcsstr(line, L"connection lost") || !wcsncmp(line, L"Reconnecting", 12))) {
                    swprintf_s(shot, MAX_PATH, L"%s.reconnect.bmp", capture_file);
                    capture(shot);
                    lost = 1;
                }
            }
            t0 = GetTickCount64();
            while (GetTickCount64() - t0 < 600)   /* (the answer's install, the page again) */
                while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&pm);
                    DispatchMessageW(&pm);
                }
            discover_test_state(&permille, &owner, line, 400);
            if (f) {
                fwprintf(f, L"end: %s\nDelete shown: %s\n", line, owner ? L"yes" : L"no");
                fclose(f);
            }
        }
        if (capture_bundled) {
            WCHAR during[MAX_PATH], times[MAX_PATH];
            ULONGLONG t0 = GetTickCount64(), t_done = 0;
            int shot = 0;
            FILE *f;
            swprintf_s(during, MAX_PATH, L"%s.during.png", capture_file);
            while (s_bundled_busy && GetTickCount64() - t0 < 120000) {
                if (MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT) == WAIT_OBJECT_0)
                    while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pm);
                        DispatchMessageW(&pm);
                    }
                if (!shot && s_bundled_busy && wcschr(s_bundled_text, L'(')) {  /* (the "n of N" status shown) */
                    capture(during);
                    shot = 1;
                }
            }
            t_done = GetTickCount64() - t_start;
            while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pm);
                DispatchMessageW(&pm);
            }
            swprintf_s(times, MAX_PATH, L"%s.times.txt", capture_file);
            if (_wfopen_s(&f, times, L"w") == 0 && f) {
                fwprintf(f, L"window shown %llu ms\nbundled install done %llu ms%s\n", t_shown, t_done,
                         s_bundled_busy ? L" (still running: stopped waiting)" : L"");
                fclose(f);
            }
        }
        if (capture_report < 0)
            big_roster_confirm(capture_file);
        else if (capture_report)
            report_window(capture_report, capture_file);
        else
            capture(capture_file);
        return 0;
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
