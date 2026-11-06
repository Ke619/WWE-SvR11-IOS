/* SvR 2011 Switch port, console test 1: can Horizon give us what the runtime's
   guest memory needs?

   The SDK maps one physical range at several guest addresses (A0000000,
   C0000000, E0000000 and the raw range). This test tries the ways of doing that
   on a real console, plus the limits and CPU facts the port depends on.

   Run it from the Homebrew Menu in title mode (hold R while starting a game).
   Results: on screen and in sdmc:/svr_memtest.log. The log is flushed before
   each step, so after a crash its last line names the step that died. */
#include <switch.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KB 1024ull
#define MB (1024ull * 1024ull)
#define PAGE 0x1000ull
#define BLOCK (2ull * MB)

static FILE *g_log;
static Handle g_self;
static int g_pass, g_fail;

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    consoleUpdate(NULL);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
        fsync(fileno(g_log));
    }
}

static void result(const char *name, int ok, const char *note)
{
    say("[%s] %s%s%s", ok ? "PASS" : "FAIL", name, note ? ": " : "", note ? note : "");
    if (ok)
        g_pass++;
    else
        g_fail++;
}

static double ms_since(u64 t0)
{
    return (double)(armGetSystemTick() - t0) * 1000.0 / 19200000.0;
}

static u64 info(u32 id)
{
    u64 v = 0;
    Result rc = svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0);
    return R_SUCCEEDED(rc) ? v : ~0ull;
}

/* ---- an address window of our own, like the guest's 4.5 GB ---- */
static u8 *g_window;
#define WINDOW_SIZE 0x120000000ull

static int reserve_window(void)
{
    virtmemLock();
    g_window = (u8 *)virtmemFindAslr(WINDOW_SIZE + BLOCK, PAGE);
    if (g_window)
        virtmemAddReservation(g_window, WINDOW_SIZE + BLOCK);
    virtmemUnlock();
    /* Same 2 MB alignment on both sides lets the kernel map whole blocks. */
    g_window = (u8 *)(((u64)g_window + BLOCK - 1) & ~(BLOCK - 1));
    say("window %p size 0x%llx", g_window, WINDOW_SIZE);
    return g_window != NULL;
}

/* ---- the Sonic port's way of getting mappable memory ---- */
typedef struct Backing {
    void *heap;
    u64 alias;
    u64 size;
} Backing;

static Result backing_make(Backing *b, u64 size)
{
    Result rc;
    memset(b, 0, sizeof(*b));
    b->size = size;
    b->heap = memalign(BLOCK, size);
    if (!b->heap)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(b->heap, 0, size);
    virtmemLock();
    b->alias = (u64)virtmemFindCodeMemory(size + BLOCK, PAGE);
    b->alias = (b->alias + BLOCK - 1) & ~(BLOCK - 1);
    rc = b->alias ? svcMapProcessCodeMemory(g_self, b->alias, (u64)b->heap, size)
                  : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        free(b->heap);
        b->heap = NULL;
        return rc;
    }
    rc = svcSetProcessMemoryPermission(g_self, b->alias, size, Perm_Rw);
    if (R_FAILED(rc)) {
        svcUnmapProcessCodeMemory(g_self, b->alias, (u64)b->heap, size);
        free(b->heap);
        b->heap = NULL;
    }
    return rc;
}

static void backing_free(Backing *b)
{
    if (!b->heap)
        return;
    svcUnmapProcessCodeMemory(g_self, b->alias, (u64)b->heap, b->size);
    free(b->heap);
    b->heap = NULL;
}

/* Writes through each view and reads through all the others. */
static int views_agree(u8 **views, int n, u64 size)
{
    u64 off[3] = { 0, size / 2, size - sizeof(u32) };
    for (int w = 0; w < n; w++)
        for (int o = 0; o < 3; o++) {
            u32 want = 0xA5000000u ^ (u32)(w << 12) ^ (u32)o;
            *(volatile u32 *)(views[w] + off[o]) = want;
            __asm__("dsb sy");
            for (int r = 0; r < n; r++)
                if (*(volatile u32 *)(views[r] + off[o]) != want) {
                    say("   view %d wrote %08x, view %d read %08x at +0x%llx", w, want, r,
                        *(volatile u32 *)(views[r] + off[o]), off[o]);
                    return 0;
                }
        }
    return 1;
}

/* A: process memory, several views of one block (what the SDK wants). */
static int test_process_views(void)
{
    const u64 size = 16 * MB;
    Backing b;
    u8 *v[4] = { NULL, g_window, g_window + 0x40000000ull, g_window + 0x80000000ull };
    Result rc = backing_make(&b, size);
    say("A: make backing rc=0x%x alias=0x%llx", rc, b.alias);
    if (R_FAILED(rc)) {
        result("A process memory views", 0, "no backing");
        return 0;
    }
    v[0] = (u8 *)b.alias;
    int mapped = 0;
    for (int i = 1; i < 4; i++) {
        rc = svcMapProcessMemory(v[i], g_self, b.alias, size);
        say("A: view %d at %p rc=0x%x", i, v[i], rc);
        if (R_FAILED(rc))
            break;
        mapped = i;
    }
    int ok = mapped == 3 && views_agree(v, 4, size);
    /* The E0000000 view of a page-offset host: one view 0x1000 in. */
    if (ok) {
        u8 *shifted = g_window + 0xC0000000ull;
        rc = svcMapProcessMemory(shifted, g_self, b.alias + PAGE, size - PAGE);
        say("A: offset view rc=0x%x", rc);
        if (R_SUCCEEDED(rc)) {
            *(volatile u32 *)(shifted + 16) = 0x12345678;
            if (*(volatile u32 *)(v[1] + PAGE + 16) != 0x12345678)
                ok = 0;
            svcUnmapProcessMemory(shifted, g_self, b.alias + PAGE, size - PAGE);
        } else
            ok = 0;
    }
    for (int i = mapped; i >= 1; i--)
        svcUnmapProcessMemory(v[i], g_self, b.alias, size);
    backing_free(&b);
    result("A process memory views", ok, ok ? "one block at 4 addresses" : NULL);
    return ok;
}

/* B: shared memory mapped twice. Cleanest if the kernel allows it. */
static void test_shared_views(void)
{
    const u64 size = 16 * MB;
    Handle h = INVALID_HANDLE;
    Result rc = svcCreateSharedMemory(&h, size, Perm_Rw, Perm_Rw);
    say("B: create shared rc=0x%x", rc);
    if (R_FAILED(rc)) {
        result("B shared memory views", 0, "create refused");
        return;
    }
    u8 *v[2] = { g_window, g_window + 0x40000000ull };
    Result r0 = svcMapSharedMemory(h, v[0], size, Perm_Rw);
    Result r1 = R_SUCCEEDED(r0) ? svcMapSharedMemory(h, v[1], size, Perm_Rw) : r0;
    say("B: map rc=0x%x 0x%x", r0, r1);
    int ok = R_SUCCEEDED(r0) && R_SUCCEEDED(r1) && views_agree(v, 2, size);
    if (R_SUCCEEDED(r1))
        svcUnmapSharedMemory(h, v[1], size);
    if (R_SUCCEEDED(r0))
        svcUnmapSharedMemory(h, v[0], size);
    svcCloseHandle(h);
    result("B shared memory views", ok, NULL);
}

/* D: the design we want. One big pool made once, then small pieces mapped at
   3 views on demand, as the guest's commits would do, and given back. */
static void test_pool(u64 pool, int want)
{
    const u64 piece = 64 * KB;
    Backing b;
    u64 t0 = armGetSystemTick();
    Result rc = backing_make(&b, pool);
    say("D: pool %llu MB rc=0x%x in %.0f ms, used %llu MB", pool / MB, rc, ms_since(t0),
        info(InfoType_UsedMemorySize) / MB);
    if (R_FAILED(rc)) {
        result("D pool", 0, "no pool");
        return;
    }
    const u64 base[3] = { 0, 0x40000000ull, 0x80000000ull };
    int ok = 1;
    for (int round = 0; round < 3 && ok; round++) {
        int done = 0;
        t0 = armGetSystemTick();
        for (; done < want; done++) {
            u64 src = b.alias + (u64)done * piece * 3 % pool;
            int v = 0;
            for (; v < 3; v++) {
                rc = svcMapProcessMemory(g_window + base[v] + (u64)done * piece * 2, g_self, src, piece);
                if (R_FAILED(rc))
                    break;
            }
            if (v < 3) {
                while (v-- > 0)
                    svcUnmapProcessMemory(g_window + base[v] + (u64)done * piece * 2, g_self, src, piece);
                break;
            }
        }
        double map_ms = ms_since(t0);
        for (int i = 0; i < done && ok; i += 97) {
            u8 *v[3];
            for (int k = 0; k < 3; k++)
                v[k] = g_window + base[k] + (u64)i * piece * 2;
            ok = views_agree(v, 3, piece);
        }
        for (int i = done - 1; i >= 0; i--) {
            u64 src = b.alias + (u64)i * piece * 3 % pool;
            for (int k = 0; k < 3; k++)
                svcUnmapProcessMemory(g_window + base[k] + (u64)i * piece * 2, g_self, src, piece);
        }
        say("D: round %d: %d of %d pieces x3 views, %.1f us per map, stop rc=0x%x, agree %d", round, done, want,
            done ? map_ms * 1000.0 / (done * 3) : 0.0, rc, ok);
        if (done < want)
            ok = 0;
    }
    backing_free(&b);
    char note[64];
    snprintf(note, sizeof(note), "%llu MB pool, %d pieces, 3 rounds", pool / MB, want);
    result("D pool", ok, note);
}

/* E: how big can one pool be, with 3 full views at aligned addresses? */
static u64 test_one_block(void)
{
    static const u64 sizes[] = { 2816, 2560, 2304, 2048, 1792, 1536, 1024 };
    u64 best = 0;
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]) && !best; n++) {
        const u64 size = sizes[n] * MB;
        Backing b;
        u64 t0 = armGetSystemTick();
        Result rc = backing_make(&b, size);
        say("E: %llu MB block rc=0x%x, %.0f ms", sizes[n], rc, ms_since(t0));
        if (R_FAILED(rc))
            continue;
        /* the window has room for one 2.8 GB view plus aliases of the top part */
        u8 *v0 = g_window;
        rc = svcMapProcessMemory(v0, g_self, b.alias, size);
        say("E: view rc=0x%x", rc);
        if (R_SUCCEEDED(rc)) {
            u8 *v[2] = { (u8 *)b.alias, v0 };
            if (views_agree(v, 2, size))
                best = sizes[n];
            svcUnmapProcessMemory(v0, g_self, b.alias, size);
        }
        backing_free(&b);
    }
    char note[64];
    snprintf(note, sizeof(note), "%llu MB", best);
    result("E one big block", best >= 1536, note);
    return best;
}

/* H: on-demand chunks, aligned this time (test 1 stopped at 1488 MB unaligned). */
static void test_chunks(void)
{
    enum { MAX_CHUNKS = 176 };
    static Backing chunks[MAX_CHUNKS];
    const u64 chunk = 16 * MB;
    int n = 0;
    Result rc = 0;
    u64 t0 = armGetSystemTick();
    for (; n < MAX_CHUNKS; n++) {
        rc = backing_make(&chunks[n], chunk);
        if (R_FAILED(rc))
            break;
        rc = svcMapProcessMemory(g_window + n * chunk, g_self, chunks[n].alias, chunk);
        if (R_FAILED(rc)) {
            backing_free(&chunks[n]);
            break;
        }
    }
    say("H: aligned chunks, 1 view: %llu MB in %.0f ms, stop rc=0x%x", n * chunk / MB, ms_since(t0), rc);
    for (int i = n - 1; i >= 0; i--) {
        svcUnmapProcessMemory(g_window + i * chunk, g_self, chunks[i].alias, chunk);
        backing_free(&chunks[i]);
    }
    char note[64];
    snprintf(note, sizeof(note), "%llu MB", n * chunk / MB);
    result("H aligned chunks", n * chunk > 1488 * MB, note);
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    g_log = fopen("sdmc:/svr_memtest.log", "w");

    say("SvR 2011 Switch memory test 1, round 2");
    say("applet type %d (Application = title mode, needed for full memory)", appletGetAppletType());
    say("total %llu MB, used %llu MB, system resource %llu MB", info(InfoType_TotalMemorySize) / MB,
        info(InfoType_UsedMemorySize) / MB, info(InfoType_SystemResourceSizeTotal) / MB);
    say("aslr 0x%llx+0x%llx heap 0x%llx+0x%llx", info(InfoType_AslrRegionAddress), info(InfoType_AslrRegionSize),
        info(InfoType_HeapRegionAddress), info(InfoType_HeapRegionSize));
    say("alias 0x%llx+0x%llx stack 0x%llx+0x%llx", info(InfoType_AliasRegionAddress), info(InfoType_AliasRegionSize),
        info(InfoType_StackRegionAddress), info(InfoType_StackRegionSize));
    say("svc hints: 73:%d 74:%d 75:%d 77:%d 78:%d 50:%d 2C:%d", envIsSyscallHinted(0x73), envIsSyscallHinted(0x74),
        envIsSyscallHinted(0x75), envIsSyscallHinted(0x77), envIsSyscallHinted(0x78), envIsSyscallHinted(0x50),
        envIsSyscallHinted(0x2C));

    g_self = envGetOwnProcessHandle();
    say("own process handle 0x%x", g_self);

    if (appletGetAppletType() != AppletType_Application)
        say("NOT TITLE MODE: go back and hold R while starting a game.");
    else if (!reserve_window())
        result("window", 0, "no 4.5 GB address range");
    else {
        test_process_views();
        test_shared_views();
        test_one_block();
        test_pool(2048 * MB, 8000);
        test_chunks();
    }

    say("DONE: %d passed, %d failed. Log: sdmc:/svr_memtest.log", g_pass, g_fail);
    say("Press + to exit.");
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
