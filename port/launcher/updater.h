/* Updates from the port's GitHub releases (updater.c). */
#pragma once
#include <windows.h>
#include <stdint.h>

#define UPDATE_REPO L"KaikoClanworth1/wwe-svr2011-pc"

typedef struct UpdateInfo {
    WCHAR version[32];         /* the newest release, "0.2.0" (tag without the v) */
    WCHAR url[1024];           /* its SvR2011-PC-*.zip */
    WCHAR page[512];           /* its release page */
    uint64_t size;             /* the zip's size in bytes */
    int prerelease;
} UpdateInfo;

/* Asks GitHub for the newest release (pre-releases included). 1 on success. */
int update_check(UpdateInfo *out, WCHAR *err, size_t errn);

/* The release of `version` ("2.0.4": tag v2.0.4), as update_check. */
int update_check_version(const WCHAR *version, UpdateInfo *out, WCHAR *err, size_t errn);

/* a > b as dotted versions ("0.10.0" > "0.9.1"). */
int version_newer(const WCHAR *a, const WCHAR *b);

/* Downloads `url` to `path`, posting `msg` (wParam = percent) to `notify`. */
int update_download(const WCHAR *url, const WCHAR *path, HWND notify, UINT msg, volatile LONG *cancel,
                    WCHAR *err, size_t errn);

/* As update_download, calling `progress` (from the downloading thread) as bytes come: done and the size the
   server gave (0 when it gave none). */
typedef void (*update_progress_fn)(uint64_t done, uint64_t total, void *ctx);
int update_download_cb(const WCHAR *url, const WCHAR *path, update_progress_fn progress, void *ctx,
                       volatile LONG *cancel, WCHAR *err, size_t errn);

/* `len` bytes of `url` from `offset` (an HTTP Range request) into `out`; offset < 0 = the last `len` bytes.
   `total` (may be NULL) gets the whole file's size the server gave. 1 when all `len` bytes came. */
int update_read_range(const WCHAR *url, int64_t offset, uint32_t len, void *out, uint64_t *total,
                      WCHAR *err, size_t errn);

/* File helpers the launcher and the updater share (MAX_PATH paths). */
int file_exists(const WCHAR *p);
int dir_exists(const WCHAR *p);
/* a\b into out (MAX_PATH); 0 when it would not fit. */
int join(WCHAR *out, const WCHAR *a, const WCHAR *b);
/* The same folder (full paths, case and a trailing \ ignored). */
int same_dir(const WCHAR *a, const WCHAR *b);
/* Copies the folder `from` into `to` with everything in it (files replaced). */
int copy_tree(const WCHAR *from, const WCHAR *to);
/* Copies the folders of `from` (Bundled Mods, native_shaders, pad_icons, Android ...) into `to`. */
int copy_folders(const WCHAR *from, const WCHAR *to);
