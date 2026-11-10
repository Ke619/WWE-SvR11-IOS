/* WWE SmackDown vs. Raw 2011 PC launcher - the Reports tab (reports_tab.h).
 *
 * The list: each report this game folder sent (its receipt in UserData\reports.json, report_receipts.h) - when,
 * the problem, what happened, and its state from the server (POST /api/reports/status, asked on a thread each
 * time the tab opens and on Refresh). Signed in online, the account's reports from other PCs are listed too.
 * The selected one's line below: "Fixed - in the next update (2.1.9): ...", "We need more information: ...".
 * A question can be answered here (text and up to 3 pictures: POST /api/reports/reply), and a report changed
 * until it's done or a duplicate - "Edit / add pictures...": text to add to what happened and pictures, 5 a report
 * in all (POST /api/reports/edit). Reports the game sent with "add pictures later" say "Add your pictures". */
#include "reports_tab.h"
#include "report_urls.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "report_receipts.h"

enum { R_INTRO, R_GROUP, R_LIST, R_EMPTY, R_DETAIL, R_ANS_L, R_ANSWER, R_PICS, R_PICS_L, R_SEND, R_REFRESH,
       R_STATUS, R_EDIT, R_COUNT };

static HWND s_wnd, s_c[R_COUNT];
static int s_base;
static reports_server_fn s_server;
static reports_token_fn s_token;
static WCHAR s_game[MAX_PATH];
static ReportReceipt s_items[RECEIPTS_MAX];
static int s_n, s_busy;
static WCHAR s_pics[RECEIPT_MAX_EDIT_IMAGES][MAX_PATH];
static int s_npics;
static char s_editing[40];   /* the report whose change box is open ("" none: the box answers a question) */

/* A thread's answer (WM_APP_REPORTS, wp: 1 the status, 2 an answer sent). */
typedef struct {
    int ok, n;
    ReportReceipt items[RECEIPTS_MAX];
    WCHAR why[300];
    char id[40];
} Job;

static void status(const WCHAR *t) { SetWindowTextW(s_c[R_STATUS], t); }

static int selected(void)
{
    const int i = ListView_GetNextItem(s_c[R_LIST], -1, LVNI_SELECTED);
    return i >= 0 && i < s_n ? i : -1;
}

/* The selected report while its change box is open, else NULL. */
static const ReportReceipt *editing(void)
{
    const int i = selected();
    return i >= 0 && s_editing[0] && !strcmp(s_items[i].id, s_editing) ? &s_items[i] : NULL;
}

/* How many pictures the box takes: an answer 3; a change what the report has room for (5 in all). */
static int pics_max(void)
{
    const ReportReceipt *r = editing();
    int room;
    if (!r) return RECEIPT_MAX_IMAGES;
    room = RECEIPT_MAX_EDIT_IMAGES - r->pictures;
    return room < 0 ? 0 : room;
}

static void pics_text(void)
{
    WCHAR t[400] = L"";
    int i;
    for (i = 0; i < s_npics; i++) {
        const WCHAR *name = wcsrchr(s_pics[i], L'\\');
        if (i) wcscat_s(t, 400, L", ");
        wcsncat_s(t, 400, name ? name + 1 : s_pics[i], _TRUNCATE);
    }
    if (!s_npics) {
        if (pics_max()) swprintf_s(t, 400, L"No pictures (up to %d).", pics_max());
        else wcscpy_s(t, 400, L"This report has all 5 pictures it can have.");
    }
    SetWindowTextW(s_c[R_PICS_L], t);
    EnableWindow(s_c[R_PICS], s_npics < pics_max());
}

/* The selected report's line, and the answer box when it waits for one. */
static void detail(void)
{
    const int i = selected();
    WCHAR badge[60], line[1400], text[1900], what[220], problem[130];
    int asked = 0, edit = 0, can_edit = 0;
    if (i < 0) {
        SetWindowTextW(s_c[R_DETAIL], s_n ? L"Pick a report to see what became of it." : L"");
    } else {
        const ReportReceipt *r = &s_items[i];
        receipt_state_text(r, badge, 60, line, 1400);
        MultiByteToWideChar(CP_UTF8, 0, r->what, -1, what, 220);
        MultiByteToWideChar(CP_UTF8, 0, r->problem, -1, problem, 130);
        swprintf_s(text, 1900, L"%s%s\r\n\r\nReport %S - %s%s%s%s",
                   receipt_wants_pictures(r) ? L"Add your pictures: you sent this from the game without them - press "
                                               L"Edit / add pictures below.\r\n" : L"",
                   line, r->id, problem, what[0] ? L": \x201C" : L"", what, what[0] ? L"\x201D" : L"");
        if (r->edits)
            swprintf_s(text + wcslen(text), 1900 - wcslen(text), L"\r\n(You changed it %d time%s - it has %d "
                       L"picture%s.)", r->edits, r->edits == 1 ? L"" : L"s", r->pictures, r->pictures == 1 ? L"" : L"s");
        if (r->from_account) wcscat_s(text, 1900, L"\r\n(Sent from another PC with your online account.)");
        SetWindowTextW(s_c[R_DETAIL], text);
        can_edit = r->state[0] && receipt_editable(r);
        edit = can_edit && editing() != NULL;
        asked = !edit && (!strcmp(r->state, "needs_info") || (!strcmp(r->state, "replied") && r->replies < 5));
        if (edit)
            SetWindowTextW(s_c[R_ANS_L], L"Add to your report (what you left out) - and pictures, 5 a report in all:");
        else if (!strcmp(r->state, "replied"))
            SetWindowTextW(s_c[R_ANS_L], L"Add to your answer (we have the first one):");
        else
            SetWindowTextW(s_c[R_ANS_L], L"Your answer (what you saw, what you did - pictures help):");
    }
    if (!edit) s_editing[0] = 0;
    ShowWindow(s_c[R_ANS_L], asked || edit ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[R_ANSWER], asked || edit ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[R_PICS], asked || edit ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[R_PICS_L], asked || edit ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[R_SEND], asked || edit ? SW_SHOW : SW_HIDE);
    SetWindowTextW(s_c[R_SEND], edit ? L"Save changes" : L"Send answer");
    EnableWindow(s_c[R_SEND], (asked || edit) && !s_busy);
    ShowWindow(s_c[R_EDIT], can_edit ? SW_SHOW : SW_HIDE);
    SetWindowTextW(s_c[R_EDIT], edit ? L"Cancel the change" : L"Edit / add pictures\x2026");
    EnableWindow(s_c[R_EDIT], can_edit && !s_busy);
    if (s_npics > pics_max()) s_npics = pics_max();
    if (asked || edit) pics_text();
}

static void fill(void)
{
    int i, keep = selected();
    char keep_id[40] = "";
    if (keep >= 0) strcpy_s(keep_id, sizeof keep_id, s_items[keep].id);
    SendMessageW(s_c[R_LIST], WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_c[R_LIST]);
    for (i = 0; i < s_n; i++) {
        const ReportReceipt *r = &s_items[i];
        WCHAR date[40], problem[130], what[220], badge[60], line[1400];
        LVITEMW it;
        MultiByteToWideChar(CP_UTF8, 0, r->date, -1, date, 40);
        MultiByteToWideChar(CP_UTF8, 0, r->problem, -1, problem, 130);
        MultiByteToWideChar(CP_UTF8, 0, r->what, -1, what, 220);
        receipt_state_text(r, badge, 60, line, 1400);
        if (receipt_wants_pictures(r)) wcscpy_s(badge, 60, L"Add your pictures");
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = date;
        ListView_InsertItem(s_c[R_LIST], &it);
        ListView_SetItemText(s_c[R_LIST], i, 1, (WCHAR *)(!strcmp(r->app, "modmaker") ? L"Mod Maker" :
                                                          !strcmp(r->app, "android") ? L"Android" :
                                                          !strcmp(r->app, "game") ? L"In the game" :
                                                          r->from_account ? L"Another PC" : L"Launcher"));
        ListView_SetItemText(s_c[R_LIST], i, 2, problem);
        ListView_SetItemText(s_c[R_LIST], i, 3, what);
        ListView_SetItemText(s_c[R_LIST], i, 4, badge);
        if (keep_id[0] && !strcmp(keep_id, r->id))
            ListView_SetItemState(s_c[R_LIST], i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(s_c[R_LIST], WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_c[R_LIST], NULL, TRUE);
    /* (none: the line in the list's place - a list paints over a static on it) */
    ShowWindow(s_c[R_LIST], s_n ? SW_SHOW : SW_HIDE);
    ShowWindow(s_c[R_EMPTY], s_n ? SW_HIDE : SW_SHOW);
    detail();
}

static DWORD WINAPI status_thread(void *arg)
{
    Job *j = (Job *)arg;
    WCHAR server[300];
    char token[200];
    s_server(server, 300);
    s_token(token, sizeof token);
    j->ok = receipts_status(server, token, j->items, &j->n, RECEIPTS_MAX, j->why, 300);
    PostMessageW(s_wnd, WM_APP_REPORTS, 1, (LPARAM)j);
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
    s_busy = 1;
    EnableWindow(s_c[R_REFRESH], FALSE);
    status(L"Asking the server\x2026");
    if ((h = CreateThread(NULL, 0, status_thread, j, 0, NULL)) != NULL) CloseHandle(h);
    else { s_busy = 0; free(j); EnableWindow(s_c[R_REFRESH], TRUE); }
}

typedef struct {
    Job job;
    ReportReceipt r;
    char text[8192];
    WCHAR pics[RECEIPT_MAX_EDIT_IMAGES][MAX_PATH];
    int npics;
} ReplyJob;

static DWORD WINAPI reply_thread(void *arg)
{
    ReplyJob *j = (ReplyJob *)arg;
    WCHAR server[300];
    char token[200];
    const WCHAR *pics[RECEIPT_MAX_EDIT_IMAGES];
    int i;
    s_server(server, 300);
    s_token(token, sizeof token);
    for (i = 0; i < j->npics; i++) pics[i] = j->pics[i];
    j->job.ok = receipt_reply(server, token, &j->r, j->text, pics, j->npics, j->job.why, 300);
    strcpy_s(j->job.id, sizeof j->job.id, j->r.id);
    PostMessageW(s_wnd, WM_APP_REPORTS, 2, (LPARAM)j);
    return 0;
}

static DWORD WINAPI edit_thread(void *arg)
{
    ReplyJob *j = (ReplyJob *)arg;
    WCHAR server[300];
    char token[200];
    const WCHAR *pics[RECEIPT_MAX_EDIT_IMAGES];
    int i;
    s_server(server, 300);
    s_token(token, sizeof token);
    for (i = 0; i < j->npics; i++) pics[i] = j->pics[i];
    j->job.n = -1;   /* (the pictures it has now, when the server says) */
    j->job.ok = receipt_edit(server, token, &j->r, j->text, pics, j->npics, &j->job.n, j->job.why, 300);
    strcpy_s(j->job.id, sizeof j->job.id, j->r.id);
    PostMessageW(s_wnd, WM_APP_REPORTS, 3, (LPARAM)j);
    return 0;
}

static void send_answer(void)
{
    const int i = selected();
    const int edit = editing() != NULL;
    ReplyJob *j;
    WCHAR text[4096];
    HANDLE h;
    if (i < 0 || s_busy) return;
    GetWindowTextW(s_c[R_ANSWER], text, 4096);
    if (!text[0] && !s_npics) {
        status(edit ? L"Write what to add, or add a picture." : L"Write your answer first.");
        SetFocus(s_c[R_ANSWER]);
        return;
    }
    if (!(j = (ReplyJob *)calloc(1, sizeof *j))) return;
    j->r = s_items[i];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, j->text, sizeof j->text, NULL, NULL);
    if (report_text_has_url(j->text)) {
        free(j);
        status(REPORT_NO_URLS_W);
        SetFocus(s_c[R_ANSWER]);
        return;
    }
    memcpy(j->pics, s_pics, sizeof s_pics);
    j->npics = s_npics;
    s_busy = 1;
    EnableWindow(s_c[R_SEND], FALSE);
    EnableWindow(s_c[R_REFRESH], FALSE);
    EnableWindow(s_c[R_EDIT], FALSE);
    status(edit ? L"Saving your changes\x2026" : L"Sending your answer\x2026");
    if ((h = CreateThread(NULL, 0, edit ? edit_thread : reply_thread, j, 0, NULL)) != NULL) CloseHandle(h);
    else { s_busy = 0; free(j); detail(); EnableWindow(s_c[R_REFRESH], TRUE); }
}

static void add_pictures(void)
{
    static WCHAR files[8192];
    OPENFILENAMEW of;
    const WCHAR *p;
    ZeroMemory(&of, sizeof of);
    files[0] = 0;
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Pictures (PNG, JPEG, WebP)\0*.png;*.jpg;*.jpeg;*.webp\0";
    of.lpstrFile = files;
    of.nMaxFile = 8192;
    of.lpstrTitle = editing() ? L"Pictures for your report" : L"Pictures for your answer";
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    if (files[wcslen(files) + 1] == 0) {
        if (s_npics < pics_max()) wcscpy_s(s_pics[s_npics++], MAX_PATH, files);
    } else {
        for (p = files + wcslen(files) + 1; *p && s_npics < pics_max(); p += wcslen(p) + 1)
            swprintf_s(s_pics[s_npics++], MAX_PATH, L"%s\\%s", files, p);
    }
    pics_text();
}

void reports_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server, reports_token_fn token)
{
    static const WCHAR *const names[] = { L"Sent", L"From", L"Problem", L"What happened", L"Status" };
    static const int widths[] = { 102, 82, 118, 104, 116 };   /* (the list is 532 wide) */
    HDC dc;
    int i, dpi;
#define C(i, cls, text, style, x, y, w, h) (s_c[i] = add(tab, cls, text, style, x, y, w, h, id_base + (i)))
    s_wnd = wnd;
    s_base = id_base;
    s_server = server;
    s_token = token;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    C(R_INTRO, L"Static", L"The problems you reported (Report a problem below or on Play, in the Mod Maker or on Android) and what "
                          L"became of them: fixed for the next update, or a question for you.", SS_LEFT, 28, 10, 560, 40);
    C(R_GROUP, L"Button", L"Your reports", BS_GROUPBOX, 28, 56, 560, 250);
    C(R_LIST, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 42, 84, 532,
      208);
    ListView_SetExtendedListViewStyle(s_c[R_LIST], LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 5; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_c[R_LIST], i, &c);
    }
    C(R_EMPTY, L"Static", L"Reports you send from Report a problem show up here.", SS_CENTER | SS_NOPREFIX, 60, 170,
      496, 40);
    C(R_DETAIL, L"Edit", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 28, 314, 560, 72);
    C(R_ANS_L, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 394, 560, 20);
    C(R_ANSWER, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 28,
      414, 560, 66);
    C(R_PICS, L"Button", L"Add pictures\x2026", BS_PUSHBUTTON | WS_TABSTOP, 28, 488, 120, 28);
    C(R_PICS_L, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 156, 494, 300, 20);
    C(R_SEND, L"Button", L"Send answer", BS_DEFPUSHBUTTON | WS_TABSTOP, 468, 486, 120, 32);
    C(R_REFRESH, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 28, 532, 90, 28);
    C(R_EDIT, L"Button", L"Edit / add pictures\x2026", BS_PUSHBUTTON | WS_TABSTOP, 126, 532, 160, 28);
    C(R_STATUS, L"Static", L"", SS_LEFT | SS_NOPREFIX, 296, 532, 154, 40);   /* (Report a problem sits right of it) */
#undef C
    detail();
}

void reports_show(const WCHAR *game_dir)
{
    wcscpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"");
    if (!s_busy) {
        s_n = receipts_load(s_game, s_items, RECEIPTS_MAX);
        fill();
        ask();
    } else {
        fill();
    }
}

int reports_command(int id, int code)
{
    const int k = id - s_base;
    if (k < 0 || k >= R_COUNT) return 0;
    if (k == R_REFRESH && code == BN_CLICKED) {
        s_n = receipts_load(s_game, s_items, RECEIPTS_MAX);
        fill();
        ask();
    } else if (k == R_SEND && code == BN_CLICKED) {
        send_answer();
    } else if (k == R_PICS && code == BN_CLICKED) {
        add_pictures();
    } else if (k == R_EDIT && code == BN_CLICKED && !s_busy) {
        const int i = selected();
        if (editing() || i < 0) {
            s_editing[0] = 0;
        } else {
            strcpy_s(s_editing, sizeof s_editing, s_items[i].id);
            SetWindowTextW(s_c[R_ANSWER], L"");
            s_npics = 0;
        }
        detail();
        if (editing()) SetFocus(s_c[R_ANSWER]);
    }
    return 1;
}

int reports_notify(const NMHDR *nm)
{
    if (!s_c[R_LIST] || nm->hwndFrom != s_c[R_LIST]) return 0;
    if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
        if (lv->uChanged & LVIF_STATE) detail();
    }
    return 1;
}

int reports_message(UINT m, WPARAM wp, LPARAM lp)
{
    if (m != WM_APP_REPORTS) return 0;
    s_busy = 0;
    EnableWindow(s_c[R_REFRESH], TRUE);
    if (wp == 1) {
        Job *j = (Job *)lp;
        if (j->ok) {
            int i, asked = 0, fixed = 0;
            WCHAR t[200];
            memcpy(s_items, j->items, sizeof s_items);
            s_n = j->n;
            for (i = 0; i < s_n; i++) {
                asked += !strcmp(s_items[i].state, "needs_info");
                fixed += !strcmp(s_items[i].state, "fixed");
            }
            int pictures = 0;
            for (i = 0; i < s_n; i++) pictures += receipt_wants_pictures(&s_items[i]);
            if (asked)
                swprintf_s(t, 200, L"%d report%s need%s your answer - pick %s to see the question.", asked,
                           asked == 1 ? L"" : L"s", asked == 1 ? L"s" : L"", asked == 1 ? L"it" : L"one");
            else if (pictures)
                swprintf_s(t, 200, L"%d report%s wait%s for your pictures - pick %s, then Edit / add pictures.",
                           pictures, pictures == 1 ? L"" : L"s", pictures == 1 ? L"s" : L"", pictures == 1 ? L"it" : L"one");
            else if (fixed)
                swprintf_s(t, 200, L"%d of your reports %s fixed.", fixed, fixed == 1 ? L"is" : L"are");
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
        ReplyJob *j = (ReplyJob *)lp;
        if (j->job.ok) {
            int i;
            SetWindowTextW(s_c[R_ANSWER], L"");
            s_npics = 0;
            for (i = 0; i < s_n; i++)
                if (!strcmp(s_items[i].id, j->job.id)) strcpy_s(s_items[i].state, sizeof s_items[i].state, "replied"),
                    s_items[i].replies++;
            status(L"Thanks - your answer was sent.");
            fill();
        } else {
            WCHAR t[400];
            swprintf_s(t, 400, L"Not sent: %s Check you're online, then try again.", j->job.why);
            status(t);
            detail();
        }
        free(j);
    } else if (wp == 3) {
        ReplyJob *j = (ReplyJob *)lp;
        if (j->job.ok) {
            int i;
            WCHAR t[200];
            SetWindowTextW(s_c[R_ANSWER], L"");
            s_npics = 0;
            s_editing[0] = 0;
            for (i = 0; i < s_n; i++)
                if (!strcmp(s_items[i].id, j->job.id)) {
                    s_items[i].edits++;
                    if (j->job.n >= 0) s_items[i].pictures = j->job.n;
                }
            swprintf_s(t, 200, L"Thanks - your report was updated%s.", j->npics ? L" with your pictures" : L"");
            status(t);
            fill();
        } else {
            WCHAR t[400];
            swprintf_s(t, 400, L"Not saved: %s", j->job.why);
            status(t);
            detail();
        }
        free(j);
    }
    return 1;
}

int reports_test_state(int *n, WCHAR *line, size_t line_n)
{
    if (n) *n = s_n;
    if (line && line_n) GetWindowTextW(s_c[R_STATUS], line, (int)line_n);
    return s_busy;
}

void reports_test_pick(int row)
{
    if (row < 0 || row >= s_n) return;
    ListView_SetItemState(s_c[R_LIST], row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(s_c[R_LIST], row, FALSE);
    detail();
}

void reports_test_edit(void)
{
    if (!editing()) reports_command(s_base + R_EDIT, BN_CLICKED);
}

void reports_test_dump(FILE *f)
{
    int i;
    for (i = 0; i < s_n; i++) {
        WCHAR badge[60], line[1400];
        receipt_state_text(&s_items[i], badge, 60, line, 1400);
        if (receipt_wants_pictures(&s_items[i])) wcscpy_s(badge, 60, L"Add your pictures");
        fwprintf(f, L"%S  %S  %S  [%s]  %s  (edits %d, pictures %d)\n", s_items[i].id, s_items[i].app, s_items[i].date,
                 badge, line, s_items[i].edits, s_items[i].pictures);
    }
    fwprintf(f, L"answer box: %s%s\n", IsWindowVisible(s_c[R_ANSWER]) ? L"shown" : L"hidden",
             editing() ? L" (a change)" : L"");
    fwprintf(f, L"edit button: %s\n", IsWindowVisible(s_c[R_EDIT]) ? L"shown" : L"hidden");
}
