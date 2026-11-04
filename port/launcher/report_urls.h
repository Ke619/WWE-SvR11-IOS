/* WWE SmackDown vs. Raw 2011 - no web addresses in what players write in a report or a suggestion: the launcher's
   forms, the game's REPORT A PROBLEM page, the Mod Maker's report. The server's rule (server/reports.py has_url; the
   app's ReportSend.hasUrl the same, test_reports.py has the test list): a scheme (two letters or more before ://),
   www.<something>, or a bare domain: a word of letters, digits, dots and dashes ending in a web address's ending.
   Endings that are also file types (mp4, zip, pac, dat...) or short words a missing space makes are not on the list, so
   a file name, a version or a path never counts; nor does an e-mail address. Header only: Android's game builds it too. */
#pragma once

#include <string.h>

#define REPORT_NO_URLS "Please dont add urls to bug reports"
#define REPORT_NO_URLS_W L"Please dont add urls to bug reports"
#define SUGGEST_NO_URLS_W L"Please dont add urls to suggestions"

static int report_url_alpha_(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int report_url_word_(char c)
{
    return report_url_alpha_(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}
/* Is [s, s + n) one of the list, whatever its case? */
static int report_url_in_(const char *s, int n, const char *const *list)
{
    for (; *list; list++) {
        int i;
        if ((int)strlen(*list) != n) continue;
        for (i = 0; i < n; i++) {
            const char c = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
            if (c != (*list)[i]) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static int report_text_has_url(const char *t)
{
    static const char *const endings[] = { "com", "net", "org", "gg", "io", "tv", "xyz", "co", "uk", "ru", "eu", "nz",
                                           "br", "au", "biz", NULL };
    static const char *const with_path[] = { "app", "dev", "site", "online", "link", "live", "club", "shop", "store",
                                             "top", "me", "be", "to", "info", "fun", "cc", "ly", "gl", "gd", NULL };
    static const char *const sites[] = { "youtu.be", "t.me", "wa.me", "discord.me", "bit.ly", "goo.gl", "is.gd", NULL };
    static const char *const www[] = { "www", NULL };
    int i = 0;
    if (!t) return 0;
    while (t[i]) {
        int start, end, k, dot = -1, dots2 = 0, lower = 0, upper = 0;
        char before, after;
        if (t[i] == ':' && t[i + 1] == '/' && t[i + 2] == '/' && i >= 2 && report_url_alpha_(t[i - 1]) &&
            report_url_alpha_(t[i - 2]))
            return 1;
        if (!report_url_word_(t[i])) { i++; continue; }
        start = i;
        while (report_url_word_(t[i])) i++;
        end = i;
        while (end > start && (t[end - 1] == '.' || t[end - 1] == '-')) end--;
        if (end == start) continue;
        before = start ? t[start - 1] : 0;
        after = t[end];
        if (before == '@' || before == '/' || before == '\\' || before == '_' || after == '@' || after == '_' ||
            after == '\\')
            continue;   /* (an e-mail address; a folder's file) */
        for (k = start; k < end; k++) {
            if (t[k] != '.') continue;
            if (k + 1 < end && t[k + 1] == '.') dots2 = 1;
            dot = k;
        }
        if (dot < 0 || dots2 || t[start] == '.') continue;
        for (k = start; k < end && t[k] != '.'; k++)
            ;
        if (report_url_in_(t + start, k - start, www)) return 1;
        for (k = dot + 1; k < end; k++) {
            if (t[k] >= 'a' && t[k] <= 'z') lower = 1;
            if (t[k] >= 'A' && t[k] <= 'Z') upper = 1;
        }
        if (lower && upper) continue;   /* (a missing space before a new sentence: "the game.Net play") */
        if (report_url_in_(t + dot + 1, end - dot - 1, endings) || report_url_in_(t + start, end - start, sites) ||
            (after == '/' && report_url_in_(t + dot + 1, end - dot - 1, with_path)))
            return 1;
    }
    return 0;
}
