/* WWE SmackDown vs. Raw 2011 - the updates' own log (update_log.c): <game>\logs\updater.log, one line a step of
   the updater's runs (update, repair: what it did, what it said, how it ended) and of the launcher's update
   checks and file checks - for Report a problem (report.c puts its end in the zip, report.json its last result). */
#ifndef SVR2011_UPDATE_LOG_H
#define SVR2011_UPDATE_LOG_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "<date time> <who>: <text>" (who: "updater", "launcher"); kept under 256 KB (the older half goes). */
void update_log(const wchar_t *game_dir, const char *who, const wchar_t *fmt, ...);

/* The log's last "result" line (an update's / a repair's end), else its last line, UTF-8; 0 when there is none. */
int update_log_last(const wchar_t *game_dir, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
