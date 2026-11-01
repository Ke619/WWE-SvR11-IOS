/* WWE SmackDown vs. Raw 2011 PC launcher - Verify game files (verify.c): every
 * file the game's disc put in the game folder, checked against the disc's sizes
 * and CRC-32s (game_files_crc.inc, tools/make_game_file_crcs.py). A file that
 * differs is damaged (a bad copy or extraction, or a file changed by hand) and
 * can make the game crash while it loads; set aside, the next Install copies it
 * again from the disc image. */
#pragma once

#include <windows.h>
#include <stdint.h>

#define VERIFY_MAX_BAD 64

typedef struct {
    int files;                 /* checked */
    int missing, damaged;      /* (beyond VERIFY_MAX_BAD only counted) */
    int other_edition;         /* most files differ: not the disc the list was made from */
    int other_release;         /* default.xex isn't the supported one (verify_xex_supported) */
    int bad_count;             /* entries in bad[] */
    WCHAR bad[VERIFY_MAX_BAD][MAX_PATH];   /* paths in the game folder, missing or damaged */
    int bad_missing[VERIFY_MAX_BAD];
} VerifyResult;

/* Progress: bytes read of the total, and the file being read. */
typedef void (*VerifyProgress)(uint64_t done, uint64_t total, const WCHAR *file, void *ctx);

/* Checks the game folder `game`. 0 when cancelled (*cancel set) or the
   folder can't be read; the result is filled in either way. */
int verify_game_files(const WCHAR *game, volatile LONG *cancel, VerifyProgress progress, void *ctx,
                      VerifyResult *out);

/* The result in a sentence or two (for the status line and the console). */
void verify_describe(const VerifyResult *r, WCHAR *out, size_t n);

/* default.xex against the one the port was made from (the list's size and CRC-32: the US / Europe release,
   title 5451085D). The port's code is that xex recompiled, so another release's - the Japanese disc (2.1.8
   report 6fd821) - can't run. 1: the supported one, 0: another release's, -1: missing or unreadable. (Cached by
   the file's size and time.) */
int verify_xex_supported(const WCHAR *path);
/* The same for a default.xex's bytes read elsewhere (the Install's disc image): its size, and its CRC-32 when
   that size is the supported one's (verify_xex_size_supported first, then verify_crc32 over it). */
int verify_xex_size_supported(uint64_t size);
int verify_xex_crc_supported(uint32_t crc);
uint32_t verify_crc32(uint32_t crc, const void *data, size_t n);
#define VERIFY_OTHER_RELEASE                                                                                     \
    L"This is another release of the game: its default.xex isn't the one this port was made from (for example "  \
    L"the Japanese disc). The port only runs the US / Europe Xbox 360 release (title 5451085D) - install the "    \
    L"game from that disc image."

/* Renames the damaged files to <name>.damaged (missing ones need nothing), so
   Install copies them again. The number set aside. */
int verify_set_aside(const VerifyResult *r);
