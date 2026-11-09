/* WWE SmackDown vs. Raw 2011 PC launcher - the Suggestions tab (suggestions_tab.h).
 *
 * On top, "Send an idea": a title, the idea, up to 3 pictures, Send - sent on a thread as a report of kind
 * "suggestion" (report_send: the "report" part with "kind":"suggestion", the pictures, no zip). The server's
 * rate limit and checks are the reports' (5 in 10 minutes from one address). Its receipt is kept in
 * UserData\suggestions.json, apart from the reports'. Below, "Your suggestions": each one sent, and our answer
 * from the server (POST /api/reports/status, asked when the tab opens and on Refresh); the selected one's line
 * under the list ("Sure - we plan to add this: ...", "No, not planned: ...", "Done in 2.2.0: ..."). */
#include "suggestions_tab.h"
#include "report_urls.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "report_receipts.h"
#include "report_send.h"

enum { S_INTRO, S_NEW, S_TITLE_L, S_TITLE, S_TEXT_L, S_TEXT, S_PICS, S_PICS_L, S_SEND, S_MINE, S_LIST, S_EMPTY,
       S_DETAIL, S_REFRESH, S_STATUS, S_DISC_L, S_DISC, S_SIGNIN, S_COUNT };
enum { SUG_MAX_PICS = 3 };
/* (above the form, and again before Send: ideas are for the game and its tools, not mod requests) */
#define SUGGEST_ABOUT L"Suggestions are for the game and its tools (the launcher, the Mod Maker, the porters) - not " \
                      L"requests for mods. Ask mod makers for mods on Discord."

static HWND s_wnd, s_c[S_COUNT];
static int s_base;
static reports_server_fn s_server;
static reports_token_fn s_token;
static int (*s_refuses)(const WCHAR *server, const char *what);
static char s_version[40];
static WCHAR s_game[MAX_PATH];
static ReportReceipt s_items[RECEIPTS_MAX];
static int s_n, s_busy;
static WCHAR s_pics[SUG_MAX_PICS][MAX_PATH];
static int s_npics;
static void (*s_sign_in)(void);

void suggestions_set_sign_in(void (*go)(void)) { s_sign_in = go; }

/* The Sign in button: shown while signed out (2.2.1: an idea comes from an online account). */
static int signed_in(void)
{
    char token[200] = "";
    if (s_token) s_token(token, sizeof token);
    ShowWindow(s_c[S_SIGNIN], token[0] ? SW_HIDE : SW_SHOW);
    return token[0] != 0;
}

/* The status thread's answer (WM_APP_SUGGESTIONS, wp 1). */
typedef struct {
    int ok, n;
    ReportReceipt items[RECEIPTS_MAX];
    WCHAR why[300];
} Job;

/* The send thread's (wp 2). */
typedef struct {
    WCHAR game[MAX_PATH], server[300];
    char title[400], text[8192], discord[80], token[200];
    WCHAR pics[SUG_MAX_PICS][MAX_PATH];
    int npics, ok;
    ReportResult r;
} SendJob;

static void status(const WCHAR *t) { SetWindowTextW(s_c[S_STATUS], t); }

static int selected(void)
{
    const int i = ListView_GetNextItem(s_c[S_LIST], -1, LVNI_SELECTED);
    return i >= 0 && i < s_n ? i : -1;
}

static void pics_text(void)
{
    WCHAR t[MAX_PATH + 40];
    if (!s_npics) t[0] = 0;
    else if (s_npics == 1) swprintf_s(t, MAX_PATH + 40, L"%s", wcsrchr(s_pics[0], L'\\') ? wcsrchr(s_pics[0], L'\\') + 1 : s_pics[0]);
    else swprintf_s(t, MAX_PATH + 40, L"%d pictures", s_npics);
    SetWindowTextW(s_c[S_PICS_L], t);
    SetWindowTextW(s_c[S_PICS], s_npics ? L"Clear pictures" : L"Add a picture\x2026");
}

static void detail(void)
{
    const int i = selected();
    WCHAR badge[60], line[1400], text[1900], what[220], title[130];
    if (i < 0) {
        SetWindowTextW(s_c[S_DETAIL], s_n ? L"Pick one to see our answer." : L"");
        return;
    }
    receipt_state_text(&s_items[i], badge, 60, line, 1400);
    MultiByteToWideChar(CP_UTF8, 0, s_items[i].what, -1, what, 220);
    MultiByteToWideChar(CP_UTF8, 0, s_items[i].problem, -1, title, 130);
    swprintf_s(text, 1900, L"%s\r\n\r\n%s%s%s%s%s", line, title, what[0] ? L": \x201C" : L"", what,
               what[0] ? L"\x201D" : L"", s_items[i].from_account ? L"\r\n(Sent from another PC with your online "
                                                                    L"account.)" : L"");
    SetWindowTextW(s_c[S_DETAIL], text);
}

static void fill(void)
{
    int i, keep = selected();
    char keep_id[40] = "";
    if (keep >= 0) strcpy_s(keep_id, sizeof keep_id, s_items[keep].id);
    SendMessageW(s_c[S_LIST], WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_c[S_LIST]);
    for (i = 0; i < s_n; i++) {
        const ReportReceipt *r = &s_items[i];
        WCHAR date[40], title[130], badge[60], line[1400];
        LVITEMW it;
        strcpy_s(s_items[i].kind, sizeof s_items[i].kind, "suggestion");   /* (its words: "Done", not "Fixed") */
        MultiByteToWideChar(CP_UTF8, 0, r->date, -1, date, 40);
        MultiByteToWideChar(CP_UTF8, 0, r->problem, -1, title, 130);
        receipt_state_text(r, badge, 60, line, 1400);
        if (!strcmp(r->state, "fixed") && r->version[0]) {
            WCHAR v[24];
            MultiByteToWideChar(CP_UTF8, 0, r->version, -1, v, 24);
            swprintf_s(badge, 60, L"Done in %s", v);
        }
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = date;
        ListView_InsertItem(s_c[S_LIST], &it);
        ListView_SetItemText(s_c[S_LIST], i, 1, title);
        ListView_SetItemText(s_c[S_LIST], i, 2, badge);
        if (keep_id[0] && !strcmp(keep_id, r->id))
            ListView_SetItemState(s_c[S_LIST], i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(s_c[S_LIST], WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_c[S_LIST], NULL, TRUE);
    ShowWindow(s_c[S_LIST], s_n ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[S_EMPTY], s_n ? SW_HIDE : SW_SHOW);
    detail();
}

static void busy(int on)
{
    s_busy = on;
    EnableWindow(s_c[S_SEND], !on);
    EnableWindow(s_c[S_REFRESH], !on);
}

static DWORD WINAPI status_thread(void *arg)
{
    Job *j = (Job *)arg;
    WCHAR server[300];
    char token[200];
    s_server(server, 300);
    s_token(token, sizeof token);
    j->ok = receipts_status_kind(server, token, "suggestion", j->items, &j->n, RECEIPTS_MAX, j->why, 300);
    PostMessageW(s_wnd, WM_APP_SUGGESTIONS, 1, (LPARAM)j);
    return 0;
}

static void ask(void)
{
    Job *j;
    HANDLE h;
    if (s_busy) return;
    if (!s_n) {   /* (none kept: the account's, when signed in, else nothing to ask) */
        char token[200];
        s_token(token, sizeof token);
        if (!token[0]) { status(L""); return; }
    }
    if (!(j = (Job *)calloc(1, sizeof *j))) return;
    memcpy(j->items, s_items, sizeof s_items);
    j->n = s_n;
    busy(1);
    status(L"Asking the server\x2026");
    if ((h = CreateThread(NULL, 0, status_thread, j, 0, NULL)) != NULL) CloseHandle(h);
    else { free(j); busy(0); }
}

static DWORD WINAPI send_thread(void *arg)
{
    SendJob *j = (SendJob *)arg;
    ReportUpload u;
    char *json, *full = NULL;
    int i;
    json = report_json("launcher", s_version, j->title, "", j->text, "", j->discord, "", "");
    if (json && json[0] == '{' && (full = (char *)malloc(strlen(json) + 32)) != NULL)
        sprintf_s(full, strlen(json) + 32, "{\"kind\":\"suggestion\",%s", json + 1);
    ZeroMemory(&u, sizeof u);
    u.json = full;
    for (i = 0; i < j->npics; i++) u.images[i] = j->pics[i];
    u.n_images = j->npics;
    u.token = j->token;   /* (2.2.1: the account it's from) */
    if (!full) {
        wcscpy_s(j->r.why, 300, L"Not enough memory.");
    } else if (s_refuses && s_refuses(j->server, "/api/reports")) {
        wcscpy_s(j->r.why, 300, L"Test run: a suggestion to a server not on this PC was refused (SVR2011_ALLOW_LIVE=1 "
                                L"lets it through).");
    } else if (report_send(j->server, &u, &j->r) && j->r.secret[0]) {
        j->ok = 1;
        receipt_add_file(j->game, RECEIPTS_SUGGESTIONS, "launcher", j->r.id, j->r.secret, j->title, j->text);
        report_discord_save(j->game, j->discord);
    }
    free(full);
    free(json);
    PostMessageW(s_wnd, WM_APP_SUGGESTIONS, 2, (LPARAM)j);
    return 0;
}

static void send_idea(void)
{
    SendJob *j;
    WCHAR title[130], text[4000];
    HANDLE h;
    if (s_busy) return;
    GetWindowTextW(s_c[S_TITLE], title, 130);
    GetWindowTextW(s_c[S_TEXT], text, 4000);
    if (!title[0]) { status(L"Give your idea a short title first."); SetFocus(s_c[S_TITLE]); return; }
    if (!text[0]) { status(L"Write a line or two about your idea: what it would do, and where."); SetFocus(s_c[S_TEXT]); return; }
    if (!(j = (SendJob *)calloc(1, sizeof *j))) return;
    wcscpy_s(j->game, MAX_PATH, s_game);
    s_server(j->server, 300);
    if (!j->server[0]) { free(j); status(L"No online server is set (Online tab)."); return; }
    WideCharToMultiByte(CP_UTF8, 0, title, -1, j->title, sizeof j->title, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, j->text, sizeof j->text, NULL, NULL);
    if (report_text_has_url(j->title) || report_text_has_url(j->text)) {
        SetFocus(s_c[report_text_has_url(j->title) ? S_TITLE : S_TEXT]);
        free(j);
        status(SUGGEST_NO_URLS_W);
        return;
    }
    {   /* (2.2.1: the Discord name - the server's rule - and a sign-in) */
        WCHAR disc[64], why[200];
        GetWindowTextW(s_c[S_DISC], disc, 64);
        if (!report_discord_check(disc, j->discord, sizeof j->discord, why, 200)) {
            free(j);
            status(why);
            SetFocus(s_c[S_DISC]);
            return;
        }
        if (s_token) s_token(j->token, sizeof j->token);
        if (!j->token[0]) {
            free(j);
            signed_in();
            status(L"Sign in first: ideas come from an online account (Sign in, or Create account, on the Online tab).");
            return;
        }
    }
    memcpy(j->pics, s_pics, sizeof s_pics);
    j->npics = s_npics;
    if (!getenv("SVR2011_SUGGEST_SEND") &&   /* (the reminder again before it goes; not in a test send) */
        MessageBoxW(s_wnd, L"Before you send it:\n\n" SUGGEST_ABOUT L"\n\nSend this idea?", L"Send an idea",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        free(j);
        status(L"Not sent.");
        return;
    }
    busy(1);
    status(L"Sending your idea\x2026");
    if ((h = CreateThread(NULL, 0, send_thread, j, 0, NULL)) != NULL) CloseHandle(h);
    else { free(j); busy(0); }
}

static void add_pictures(void)
{
    static WCHAR files[8192];
    OPENFILENAMEW of;
    const WCHAR *p;
    WCHAR why[300];
    if (s_npics) { s_npics = 0; pics_text(); return; }   /* (the button clears them) */
    ZeroMemory(&of, sizeof of);
    files[0] = 0;
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Pictures (PNG, JPEG, WebP)\0*.png;*.jpg;*.jpeg;*.webp\0";
    of.lpstrFile = files;
    of.nMaxFile = 8192;
    of.lpstrTitle = L"Pictures for your idea (3 at most)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    if (files[wcslen(files) + 1] == 0) {
        wcscpy_s(s_pics[s_npics++], MAX_PATH, files);
    } else {
        for (p = files + wcslen(files) + 1; *p && s_npics < SUG_MAX_PICS; p += wcslen(p) + 1)
            swprintf_s(s_pics[s_npics++], MAX_PATH, L"%s\\%s", files, p);
    }
    while (s_npics && !report_image_ok(s_pics[s_npics - 1], why, 300)) s_npics--;   /* (a picture the server won't take) */
    if (!s_npics) status(why);
    pics_text();
}

void suggestions_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server,
                       reports_token_fn token, int (*refuses)(const WCHAR *server, const char *what),
                       const WCHAR *version)
{
    static const WCHAR *const names[] = { L"Sent", L"Idea", L"Our answer" };
    static const int widths[] = { 110, 262, 156 };   /* (the list is 532 wide) */
    HDC dc;
    int i, dpi;
#define C(i, cls, text, style, x, y, w, h) (s_c[i] = add(tab, cls, text, style, x, y, w, h, id_base + (i)))
    s_wnd = wnd;
    s_base = id_base;
    s_server = server;
    s_token = token;
    s_refuses = refuses;
    WideCharToMultiByte(CP_UTF8, 0, version ? version : L"", -1, s_version, sizeof s_version, NULL, NULL);
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    C(S_INTRO, L"Static", SUGGEST_ABOUT, SS_LEFT, 28, 10, 560, 36);
    C(S_NEW, L"Button", L"Send an idea", BS_GROUPBOX, 28, 50, 560, 206);
    C(S_TITLE_L, L"Static", L"Title:", SS_LEFT | SS_NOPREFIX, 42, 76, 70, 20);
    C(S_TITLE, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 112, 72, 462, 24);
    SendMessageW(s_c[S_TITLE], EM_LIMITTEXT, 110, 0);
    C(S_TEXT_L, L"Static", L"Your idea - what it would do, and where:", SS_LEFT | SS_NOPREFIX, 42, 104, 532, 20);
    C(S_TEXT, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 42,
      124, 532, 50);
    SendMessageW(s_c[S_TEXT], EM_LIMITTEXT, 3000, 0);
    C(S_DISC_L, L"Static", L"Your Discord name:", SS_LEFT | SS_NOPREFIX, 42, 186, 130, 20);
    C(S_DISC, L"Edit", L"", ES_AUTOHSCROLL | ES_LOWERCASE | WS_BORDER | WS_TABSTOP, 176, 182, 190, 24);
    SendMessageW(s_c[S_DISC], EM_LIMITTEXT, 32, 0);
    C(S_SIGNIN, L"Button", L"Sign in\x2026", BS_PUSHBUTTON | WS_TABSTOP, 374, 180, 90, 28);
    C(S_PICS, L"Button", L"Add a picture\x2026", BS_PUSHBUTTON | WS_TABSTOP, 42, 214, 130, 28);
    C(S_PICS_L, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 180, 220, 260, 20);
    C(S_SEND, L"Button", L"Send idea", BS_DEFPUSHBUTTON | WS_TABSTOP, 454, 212, 120, 32);
    C(S_MINE, L"Button", L"Your suggestions", BS_GROUPBOX, 28, 264, 560, 196);
    C(S_LIST, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 42, 290,
      532, 156);
    ListView_SetExtendedListViewStyle(s_c[S_LIST], LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 3; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_c[S_LIST], i, &c);
    }
    C(S_EMPTY, L"Static", L"Ideas you send show up here, with our answer.", SS_CENTER | SS_NOPREFIX, 60, 350, 496, 40);
    C(S_DETAIL, L"Edit", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 28, 468, 560, 60);
    C(S_REFRESH, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 28, 536, 90, 28);
    C(S_STATUS, L"Static", L"", SS_LEFT | SS_NOPREFIX, 128, 536, 460, 40);
#undef C
    pics_text();
    detail();
}

void suggestions_show(const WCHAR *game_dir)
{
    wcscpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"");
    {   /* (the Discord name sent last from any form here, unless one is being typed; the sign-in state) */
        WCHAR saved[80], now[80];
        GetWindowTextW(s_c[S_DISC], now, 80);
        if (!now[0]) {
            report_discord_load(s_game, saved, 80);
            SetWindowTextW(s_c[S_DISC], saved);
        }
        signed_in();
    }
    if (!s_busy) {
        s_n = receipts_load_file(s_game, RECEIPTS_SUGGESTIONS, s_items, RECEIPTS_MAX);
        fill();
        ask();
    } else {
        fill();
    }
}

int suggestions_command(int id, int code)
{
    const int k = id - s_base;
    if (k < 0 || k >= S_COUNT) return 0;
    if (code != BN_CLICKED) return 1;
    if (k == S_REFRESH && !s_busy) {
        s_n = receipts_load_file(s_game, RECEIPTS_SUGGESTIONS, s_items, RECEIPTS_MAX);
        fill();
        ask();
    } else if (k == S_SIGNIN) {
        if (s_sign_in) s_sign_in();
        return 1;
    } else if (k == S_SEND) {
        send_idea();
    } else if (k == S_PICS) {
        add_pictures();
    }
    return 1;
}

int suggestions_notify(const NMHDR *nm)
{
    if (!s_c[S_LIST] || nm->hwndFrom != s_c[S_LIST]) return 0;
    if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
        if (lv->uChanged & LVIF_STATE) detail();
    }
    return 1;
}

int suggestions_message(UINT m, WPARAM wp, LPARAM lp)
{
    if (m != WM_APP_SUGGESTIONS) return 0;
    busy(0);
    if (wp == 1) {
        Job *j = (Job *)lp;
        if (j->ok) {
            int i, answered = 0;
            WCHAR t[200];
            memcpy(s_items, j->items, sizeof s_items);
            s_n = j->n;
            for (i = 0; i < s_n; i++) answered += s_items[i].state[0] && strcmp(s_items[i].state, "received");
            if (answered)
                swprintf_s(t, 200, L"%d of your ideas %s an answer - pick one to read it.", answered,
                           answered == 1 ? L"has" : L"have");
            else
                t[0] = 0;
            status(t);
        } else {
            WCHAR t[400];
            swprintf_s(t, 400, L"Couldn't ask the server: %s", j->why);
            status(t);
        }
        free(j);
        fill();
    } else if (wp == 2) {
        SendJob *j = (SendJob *)lp;
        if (j->ok) {
            SetWindowTextW(s_c[S_TITLE], L"");
            SetWindowTextW(s_c[S_TEXT], L"");
            s_npics = 0;
            pics_text();
            status(j->r.seen_before ? L"Thanks - your idea was sent. Others asked for it too: that helps it." :
                                      L"Thanks - your idea was sent. Our answer shows below.");
            s_n = receipts_load_file(s_game, RECEIPTS_SUGGESTIONS, s_items, RECEIPTS_MAX);
            fill();
            ListView_SetItemState(s_c[S_LIST], 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            detail();
            free(j);
            ask();   /* (its state from the server: Received) */
            SetWindowTextW(s_c[S_STATUS], L"Thanks - your idea was sent. Our answer shows below.");
            return 1;
        } else {
            WCHAR t[400];
            swprintf_s(t, 400, L"Not sent: %s", j->r.why[0] ? j->r.why : L"no answer from the server - check you're online.");
            status(t);
            if (report_needs_sign_in(&j->r)) ShowWindow(s_c[S_SIGNIN], SW_SHOW);
        }
        free(j);
    }
    return 1;
}

int suggestions_test_state(int *n, WCHAR *line, size_t line_n)
{
    if (n) *n = s_n;
    if (line && line_n) GetWindowTextW(s_c[S_STATUS], line, (int)line_n);
    return s_busy;
}

void suggestions_test_send(const char *title, const char *text, const WCHAR *picture)
{
    WCHAR t[400], x[4000];
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, t, 400);
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, x, 4000);
    SetWindowTextW(s_c[S_TITLE], t);
    SetWindowTextW(s_c[S_TEXT], x);
    {   /* (tests: SVR2011_SUGGEST_DISCORD - the Discord field as typed) */
        const char *d = getenv("SVR2011_SUGGEST_DISCORD");
        if (d) {
            WCHAR w[80];
            MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 80);
            SetWindowTextW(s_c[S_DISC], w);
        }
    }
    s_npics = 0;
    if (picture && picture[0]) wcscpy_s(s_pics[s_npics++], MAX_PATH, picture);
    pics_text();
    send_idea();
}

void suggestions_test_pick(int row)
{
    if (row < 0 || row >= s_n) return;
    ListView_SetItemState(s_c[S_LIST], row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(s_c[S_LIST], row, FALSE);
    detail();
}

void suggestions_test_dump(FILE *f)
{
    int i;
    for (i = 0; i < s_n; i++) {
        WCHAR badge[60], line[1400];
        receipt_state_text(&s_items[i], badge, 60, line, 1400);
        fwprintf(f, L"%S  %S  %S  [%s]  %s\n", s_items[i].id, s_items[i].app, s_items[i].date, badge, line);
    }
}
