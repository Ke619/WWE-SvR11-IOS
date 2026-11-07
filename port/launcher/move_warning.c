/* The custom moves warning (move_warning.h). */
#include "move_warning.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* (TaskDialogIndirect is in Common Controls 6 only: the launcher asks for it already, the Mod Maker through this) */
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const WCHAR kTitle[] = L"Custom moves";
static const WCHAR kText[] =
    L"This mod has custom moves (ported or made by hand). Custom moves are experimental and most likely broken: they "
    L"can land wrong, look odd or freeze a match. Install anyway?";

static int file_exists(const WCHAR *path)
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int move_warning_dir_has_moves(const WCHAR *dir)
{
    WCHAR p[MAX_PATH];
    swprintf_s(p, MAX_PATH, L"%s\\pack.txt", dir);
    if (file_exists(p)) return 1;
    swprintf_s(p, MAX_PATH, L"%s\\moves\\pack.txt", dir);
    return file_exists(p);
}

/* A line in <game>\Mods\.install.log, as the launcher's installs write it. */
static void log_line(const WCHAR *game, const WCHAR *text)
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

/* The window as a 32-bit BMP (PrintWindow: it is drawn while off-screen). */
static void save_window(HWND w, const WCHAR *file)
{
    RECT r;
    HDC screen, dc;
    HBITMAP bmp;
    BITMAPINFO bi;
    void *bits = NULL;
    FILE *f;
    int cx, cy;
    GetWindowRect(w, &r);
    cx = r.right - r.left, cy = r.bottom - r.top;
    if (cx <= 0 || cy <= 0) return;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    screen = GetDC(NULL);
    dc = CreateCompatibleDC(screen);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(w, dc, 2 /* PW_RENDERFULLCONTENT */);
        SelectObject(dc, old);
        if (_wfopen_s(&f, file, L"wb") == 0 && f) {
            BITMAPFILEHEADER fh;
            const DWORD size = (DWORD)cx * (DWORD)cy * 4;
            ZeroMemory(&fh, sizeof fh);
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
            fh.bfSize = fh.bfOffBits + size;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
            fwrite(bits, 1, size, f);
            fclose(f);
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

typedef struct {
    const WCHAR *shot;  /* (the test aid's file, or NULL) */
    int ticks;
} AskState;

static HRESULT CALLBACK ask_proc(HWND w, UINT n, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    AskState *s = (AskState *)ref;
    (void)wp, (void)lp;
    if (!s->shot) return S_OK;
    if (n == TDN_CREATED)  /* (before it is shown: off-screen, never activated) */
        SetWindowPos(w, NULL, -8000, -8000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (n == TDN_TIMER && ++s->ticks == 3) {
        save_window(w, s->shot);
        SendMessageW(w, TDM_CLICK_BUTTON, IDNO, 0);
    }
    return S_OK;
}

typedef HRESULT(WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

int move_warning_ask(HWND owner, const WCHAR *game, const WCHAR *mod_name, int headless)
{
    WCHAR off[MAX_PATH], line[512], shot[MAX_PATH] = L"";
    const WCHAR *name = mod_name && mod_name[0] ? mod_name : L"the mod";
    TaskDialogIndirectFn task_dialog;
    TASKDIALOGCONFIG c;
    AskState s;
    int button = IDNO;
    BOOL never = FALSE;
    swprintf_s(off, MAX_PATH, L"%s\\Mods\\.no_move_warning", game);
    GetEnvironmentVariableW(L"SVR2011_MOVE_WARNING_SHOT", shot, MAX_PATH);
    if (!shot[0] && file_exists(off)) return 1;
    if (headless && !shot[0]) {
        swprintf_s(line, 512, L"\"%s\" has custom moves (ported or made by hand): experimental and most likely broken "
                              L"(installed - no one to ask).", name);
        log_line(game, line);
        return 1;
    }
    task_dialog = (TaskDialogIndirectFn)(void *)GetProcAddress(LoadLibraryW(L"comctl32.dll"), "TaskDialogIndirect");
    if (!task_dialog)  /* (no Common Controls 6: a plain box, No the default, no "don't warn me again") */
        return MessageBoxW(owner, kText, kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    ZeroMemory(&c, sizeof c);
    s.shot = shot[0] ? shot : NULL;
    s.ticks = 0;
    c.cbSize = sizeof c;
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | (s.shot ? TDF_CALLBACK_TIMER : 0);
    c.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = TD_WARNING_ICON;
    swprintf_s(line, 512, L"\"%s\" has custom moves", name);
    c.pszMainInstruction = line;
    c.pszContent = kText;
    c.nDefaultButton = IDNO;
    c.pszVerificationText = L"Don't warn me again";
    c.pfCallback = ask_proc;
    c.lpCallbackData = (LONG_PTR)&s;
    if (FAILED(task_dialog(&c, &button, NULL, &never)))
        return MessageBoxW(owner, kText, kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    if (never && button == IDYES && !s.shot) {  /* (asked no more: Yes with the tick) */
        FILE *f;
        if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
    }
    return button == IDYES;
}
