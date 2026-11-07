/* WWE SmackDown vs. Raw 2011 - the updates' own log (update_log.h). */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update_log.h"

#define UPDATE_LOG_MAX (256u << 10)

static void log_path(const wchar_t *game_dir, wchar_t *path)
{
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, MAX_PATH, L"%s\\logs", game_dir);
    CreateDirectoryW(dir, NULL);
    swprintf_s(path, MAX_PATH, L"%s\\updater.log", dir);
}

/* Over the cap: the newer half kept, from a line's start. */
static void trim(const wchar_t *path)
{
    FILE *f;
    long size;
    char *data;
    if (_wfopen_s(&f, path, L"rb") || !f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size <= (long)UPDATE_LOG_MAX) { fclose(f); return; }
    data = (char *)malloc((size_t)size);
    fseek(f, 0, SEEK_SET);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return; }
    fclose(f);
    {
        const char *from = data + size / 2, *nl = memchr(from, '\n', (size_t)(data + size - from));
        if (nl) from = nl + 1;
        if (!_wfopen_s(&f, path, L"wb") && f) {
            fwrite(from, 1, (size_t)(data + size - from), f);
            fclose(f);
        }
    }
    free(data);
}

void update_log(const wchar_t *game_dir, const char *who, const wchar_t *fmt, ...)
{
    wchar_t path[MAX_PATH], text[1200];
    char line[4000];
    SYSTEMTIME st;
    FILE *f;
    int n;
    va_list ap;
    if (!game_dir || !game_dir[0]) return;
    va_start(ap, fmt);
    vswprintf_s(text, 1200, fmt, ap);
    va_end(ap);
    for (n = 0; text[n]; n++)
        if (text[n] == L'\r' || text[n] == L'\n') text[n] = L' ';   /* (one line a step) */
    GetLocalTime(&st);
    n = sprintf_s(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d %s: ", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, who ? who : "?");
    if (n < 0) return;
    n += WideCharToMultiByte(CP_UTF8, 0, text, -1, line + n, (int)sizeof line - n - 3, NULL, NULL) - 1;
    if (n < 0) return;
    strcpy_s(line + n, sizeof line - (size_t)n, "\r\n");
    log_path(game_dir, path);
    if (_wfopen_s(&f, path, L"ab") || !f) return;
    fputs(line, f);
    fclose(f);
    trim(path);
}

int update_log_last(const wchar_t *game_dir, char *out, size_t n)
{
    wchar_t path[MAX_PATH];
    char line[4000], last[4000] = "", result[4000] = "";
    FILE *f;
    if (!out || !n) return 0;
    out[0] = 0;
    if (!game_dir || !game_dir[0]) return 0;
    swprintf_s(path, MAX_PATH, L"%s\\logs\\updater.log", game_dir);
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        strcpy_s(last, sizeof last, line);
        if (strstr(line, " result: ")) strcpy_s(result, sizeof result, line);
    }
    fclose(f);
    strncpy_s(out, n, result[0] ? result : last, _TRUNCATE);
    return out[0] != 0;
}
