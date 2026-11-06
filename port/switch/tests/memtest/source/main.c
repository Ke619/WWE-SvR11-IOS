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
    g_window = (u8 *)virtmemFindAslr(WINDOW_SIZE, PAGE);
    if (g_window)
        virtmemAddReservation(g_window, WINDOW_SIZE);
    virtmemUnlock();
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
    b->heap = memalign(PAGE, size);
    if (!b->heap)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(b->heap, 0, size);
    virtmemLock();
    b->alias = (u64)virtmemFindCodeMemory(size, PAGE);
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
    Result rc = svcCreateSharedMemory(&h, size, Perm_Rw, Perm_None);
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

/* C: transfer memory mapped twice in the same process. */
static void test_transfer_views(void)
{
    const u64 size = 16 * MB;
    void *heap = memalign(PAGE, size);
    if (!heap) {
        result("C transfer memory views", 0, "no heap");
        return;
    }
    Handle h = INVALID_HANDLE;
    Result rc = svcCreateTransferMemory(&h, heap, size, Perm_Rw);
    say("C: create rc=0x%x", rc);
    int ok = 0;
    if (R_SUCCEEDED(rc)) {
        u8 *v[2] = { g_window, g_window + 0x40000000ull };
        Result r0 = svcMapTransferMemory(h, v[0], size, Perm_Rw);
        Result r1 = R_SUCCEEDED(r0) ? svcMapTransferMemory(h, v[1], size, Perm_Rw) : r0;
        say("C: map rc=0x%x 0x%x", r0, r1);
        ok = R_SUCCEEDED(r0) && R_SUCCEEDED(r1) && views_agree(v, 2, size);
        if (R_SUCCEEDED(r1))
            svcUnmapTransferMemory(h, v[1], size);
        if (R_SUCCEEDED(r0))
            svcUnmapTransferMemory(h, v[0], size);
        svcCloseHandle(h);
    }
    free(heap);
    result("C transfer memory views", ok, NULL);
}

/* D: many small mappings with gaps, as on-demand commits make. How many fit? */
static void test_many_small(void)
{
    const u64 pool = 256 * MB, piece = 64 * KB;
    Backing b;
    Result rc = backing_make(&b, pool);
    if (R_FAILED(rc)) {
        result("D many small views", 0, "no backing");
        return;
    }
    const int want = 4000;
    int done = 0;
    u64 t0 = armGetSystemTick();
    for (; done < want; done++) {
        /* two views per piece, a hole between pieces so nothing can merge */
        u8 *a = g_window + (u64)done * piece * 2;
        u8 *c = g_window + 0x80000000ull + (u64)done * piece * 2;
        u64 src = b.alias + (u64)done * piece;
        rc = svcMapProcessMemory(a, g_self, src, piece);
        if (R_FAILED(rc))
            break;
        rc = svcMapProcessMemory(c, g_self, src, piece);
        if (R_FAILED(rc)) {
            svcUnmapProcessMemory(a, g_self, src, piece);
            break;
        }
    }
    double map_ms = ms_since(t0);
    say("D: %d pieces, 2 views each, %.1f ms, %.1f us per map, stop rc=0x%x", done, map_ms,
        done ? map_ms * 1000.0 / (done * 2) : 0.0, rc);
    t0 = armGetSystemTick();
    for (int i = done - 1; i >= 0; i--) {
        u64 src = b.alias + (u64)i * piece;
        svcUnmapProcessMemory(g_window + (u64)i * piece * 2, g_self, src, piece);
        svcUnmapProcessMemory(g_window + 0x80000000ull + (u64)i * piece * 2, g_self, src, piece);
    }
    say("D: unmapped in %.1f ms", ms_since(t0));
    backing_free(&b);
    char note[64];
    snprintf(note, sizeof(note), "%d of %d", done, want);
    result("D many small views", done == want, note);
}

/* E: how much real memory can we hold, with 2 views? Then give it all back. */
static void test_big(void)
{
    enum { MAX_CHUNKS = 200 };
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
        rc = svcMapProcessMemory(g_window + 0x80000000ull + n * chunk, g_self, chunks[n].alias, chunk);
        if (R_FAILED(rc)) {
            svcUnmapProcessMemory(g_window + n * chunk, g_self, chunks[n].alias, chunk);
            backing_free(&chunks[n]);
            break;
        }
    }
    say("E: held %llu MB in %.0f ms, stop rc=0x%x, used %llu MB", n * chunk / MB, ms_since(t0), rc,
        info(InfoType_UsedMemorySize) / MB);
    int good = 1;
    for (int i = 0; i < n; i++) {
        *(volatile u32 *)(g_window + i * chunk + 64) = 0xC0DE0000u + i;
        if (*(volatile u32 *)(g_window + 0x80000000ull + i * chunk + 64) != 0xC0DE0000u + (u32)i)
            good = 0;
    }
    for (int i = n - 1; i >= 0; i--) {
        svcUnmapProcessMemory(g_window + 0x80000000ull + i * chunk, g_self, chunks[i].alias, chunk);
        svcUnmapProcessMemory(g_window + i * chunk, g_self, chunks[i].alias, chunk);
        backing_free(&chunks[i]);
    }
    char note[64];
    snprintf(note, sizeof(note), "%llu MB", n * chunk / MB);
    result("E big memory", good && n * chunk >= 1536 * MB, note);
}

/* F: threads. Which cores? Do same-priority threads share a core by themselves? */
typedef struct Spin {
    volatile u64 count;
    volatile int *stop;
} Spin;

static void spin_fn(void *arg)
{
    Spin *s = (Spin *)arg;
    while (!*s->stop)
        s->count++;
}

static void test_spin(int prio)
{
    static volatile int stop;
    Spin a = { 0, &stop }, b = { 0, &stop };
    Thread ta, tb;
    stop = 0;
    Result r0 = threadCreate(&ta, spin_fn, &a, NULL, 0x4000, prio, 1);
    Result r1 = threadCreate(&tb, spin_fn, &b, NULL, 0x4000, prio, 1);
    if (R_FAILED(r0) || R_FAILED(r1)) {
        say("F: priority 0x%x: thread create rc=0x%x 0x%x", prio, r0, r1);
        return;
    }
    threadStart(&ta);
    threadStart(&tb);
    svcSleepThread(1000000000ll);
    stop = 1;
    threadWaitForExit(&ta);
    threadWaitForExit(&tb);
    threadClose(&ta);
    threadClose(&tb);
    say("F: priority 0x%x, 2 spinners on core 1: %llu and %llu loops (%s)", prio, a.count, b.count,
        b.count && a.count ? "shared" : "one starved");
}

static void noop_fn(void *arg)
{
    *(volatile int *)arg = svcGetCurrentProcessorNumber() + 1;
}

static void test_cores(void)
{
    u64 mask = 0;
    Result rm = svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    say("F: core mask rc=0x%x 0x%llx; main thread on core %d, priority range mask 0x%llx", rm, mask,
        svcGetCurrentProcessorNumber(), info(InfoType_PriorityMask));
    for (int core = 0; core < 4; core++) {
        volatile int ran = 0;
        Thread t;
        Result rc = threadCreate(&t, noop_fn, (void *)&ran, NULL, 0x4000, 0x3B, core);
        if (R_SUCCEEDED(rc)) {
            threadStart(&t);
            threadWaitForExit(&t);
            threadClose(&t);
        }
        say("F: thread on core %d: rc=0x%x, ran on core %d", core, rc, ran - 1);
    }
}

/* G: CPU and memory speed, single thread, for the frame budget sums. */
static void test_speed(void)
{
    u64 t0 = armGetSystemTick();
    volatile u32 sink = 0;
    u32 x = 1;
    for (u32 i = 0; i < 200000000u; i++)
        x = x * 3 + i;
    sink = x;
    say("G: integer loop %.0f ms (%u)", ms_since(t0), sink);

    const u64 size = 128 * MB;
    u8 *a = (u8 *)memalign(PAGE, size), *b = (u8 *)memalign(PAGE, size);
    if (a && b) {
        memset(a, 1, size);
        t0 = armGetSystemTick();
        memcpy(b, a, size);
        say("G: memcpy %.0f MB/s", (double)size / MB / (ms_since(t0) / 1000.0));
        u64 s = 0;
        t0 = armGetSystemTick();
        for (u64 i = 0; i < size; i += 8)
            s += *(volatile u64 *)(a + i);
        say("G: read %.0f MB/s (%llu)", (double)size / MB / (ms_since(t0) / 1000.0), s);
    }
    free(a);
    free(b);
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    g_log = fopen("sdmc:/svr_memtest.log", "w");

    say("SvR 2011 Switch memory test 1");
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
        test_transfer_views();
        test_many_small();
        test_big();
        test_cores();
        test_spin(0x2C);
        test_spin(0x3B);
        test_speed();
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
