/* Native test of the bridge's Unix half, no Wine: loads victorybridge.so with dlopen and calls
 * its __wine_unix_call_funcs table directly, as Wine's dispatcher would.
 *
 *   test_unix <path to victorybridge.so> [--speed] [--views N]
 *
 * Windows are reserved the way Wine reserves (PROT_NONE anonymous, MAP_NORESERVE). Exit status 0
 * when every check passed; each failed check prints a line. --speed adds the 60,000-view timings
 * (64 KiB views over a 512 MiB pool, view i at pool offset (i % 8192) * 64 KiB, as spike S1). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "abi.h"

typedef int32_t (*entry_t)(void *);
static const entry_t *funcs;
static int failures, checks;

#define KiB 1024ull
#define MiB (1024ull * KiB)
#define PAGE 4096ull
#define VIEW (64 * KiB)

#define CHECK(cond, ...)                                                                        \
    do {                                                                                        \
        checks++;                                                                               \
        if (!(cond)) {                                                                          \
            failures++;                                                                         \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);                              \
            printf(__VA_ARGS__);                                                                \
            printf("\n");                                                                       \
        }                                                                                       \
    } while (0)

static int32_t call(uint32_t code, void *args) { return funcs[code](args); }

static uint64_t ns_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- thin wrappers ---- */

static uint64_t features;

static uint64_t pool_create(uint64_t bytes)
{
    struct vb_pool_create a = { .bytes = bytes };
    return call(VB_POOL_CREATE, &a) ? 0 : a.pool;
}

static int32_t pool_destroy(uint64_t pool)
{
    struct vb_pool_destroy a = { pool };
    return call(VB_POOL_DESTROY, &a);
}

static uint8_t *reserve(uint64_t len)
{
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("reserve");
        exit(2);
    }
    return p;
}

static int32_t window(uint32_t code, void *addr, uint64_t len)
{
    struct vb_window a = { (uint64_t)(uintptr_t)addr, len };
    return call(code, &a);
}

/* A batch; returns the call's status, fills results and done. */
static int32_t batch(uint32_t code, uint64_t pool, const void *ops, uint64_t count, int32_t *results, uint64_t *done)
{
    struct vb_batch a = { pool, (uint64_t)(uintptr_t)ops, count, (uint64_t)(uintptr_t)results, 99 };
    int32_t s = call(code, &a);
    if (done) *done = a.done;
    return s;
}

static int map1(uint64_t pool, void *addr, uint64_t len, uint64_t offset, uint32_t prot, uint32_t flags)
{
    struct vb_map_op op = { (uint64_t)(uintptr_t)addr, len, offset, prot, flags };
    int32_t r = -1;
    uint64_t done;
    if (batch(VB_MAP, pool, &op, 1, &r, &done)) return -1;
    return r;
}

static int range1(uint32_t code, void *addr, uint64_t len, uint32_t prot)
{
    struct vb_range op = { (uint64_t)(uintptr_t)addr, len, prot, 0 };
    int32_t r = -1;
    if (batch(code, 0, &op, 1, &r, NULL)) return -1;
    return r;
}

/* Takes every written page of [addr, addr+len) with the given capacity per call, following
 * `more` as the PE side does; returns the count, or -1 when a call is refused. */
static long take(void *addr, uint64_t len, uint64_t *out, uint64_t out_cap, uint64_t capacity, int *calls)
{
    uint64_t at = (uint64_t)(uintptr_t)addr, end = at + len, buf[capacity];
    long n = 0;
    if (calls) *calls = 0;
    while (at < end) {
        struct vb_watch_take a = { at, end - at, (uint64_t)(uintptr_t)buf, capacity, 0, 0 };
        if (call(VB_WATCH_TAKE, &a)) return -1;
        if (calls) (*calls)++;
        for (uint64_t i = 0; i < a.count; i++)
            if ((uint64_t)n < out_cap) out[n++] = buf[i];
        if (!a.more || !a.count) break;
        at = buf[a.count - 1] + PAGE;
    }
    return n;
}

/* ---- probing memory without faulting the test ---- */

static sigjmp_buf jump;
static volatile sig_atomic_t probing;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)si;
    (void)ctx;
    if (probing) siglongjmp(jump, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* 0 ok, else the signal: reading / writing one byte at p. */
static int try_read(const volatile uint8_t *p, uint8_t *v)
{
    int sig;
    probing = 1;
    if (!(sig = sigsetjmp(jump, 1))) *v = *p;
    probing = 0;
    return sig;
}

static int try_write(volatile uint8_t *p, uint8_t v)
{
    int sig;
    probing = 1;
    if (!(sig = sigsetjmp(jump, 1))) *p = v;
    probing = 0;
    return sig;
}

/* What /proc/self/maps says about addr: its permissions ("rw-s") and whether anonymous. */
static int maps_at(const void *addr, char perms[5], int *anon, uint64_t *lo, uint64_t *hi)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    uint64_t a = (uint64_t)(uintptr_t)addr;
    int found = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long s, e, off, inode;
        char p[5], dev[16], path[256] = "";
        if (sscanf(line, "%lx-%lx %4s %lx %15s %lu %255[^\n]", &s, &e, p, &off, dev, &inode, path) < 6) continue;
        if (a >= s && a < e) {
            memcpy(perms, p, 5);
            *anon = inode == 0;
            if (lo) *lo = s;
            if (hi) *hi = e;
            found = 1;
            break;
        }
    }
    if (f) fclose(f);
    return found;
}

/* How many VMAs /proc/self/maps lists inside [lo, hi). */
static int vmas_in(const void *lo, const void *hi)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long s, e;
        if (sscanf(line, "%lx-%lx", &s, &e) == 2 && s >= (uintptr_t)lo && e <= (uintptr_t)hi) n++;
    }
    if (f) fclose(f);
    return n;
}

/* ---- correctness ---- */

static void test_version(void)
{
    struct vb_version v = { 0, 7, 0 };
    CHECK(call(VB_VERSION_CALL, &v) == 0, "refused");
    CHECK(v.version == VB_VERSION, "version %u", v.version);
    CHECK(v.pad == 0, "pad %u", v.pad);
    CHECK(v.features & VB_FEATURE_COPY, "no copy feature: %lu", (unsigned long)v.features);
    CHECK(!(v.features & ~(uint64_t)(VB_FEATURE_COPY | VB_FEATURE_WATCH)), "unknown features %lu", (unsigned long)v.features);
    features = v.features;
    printf("version %u, features %lu (watch %s, copy %s)\n", v.version, (unsigned long)v.features,
           v.features & VB_FEATURE_WATCH ? "yes" : "no", v.features & VB_FEATURE_COPY ? "yes" : "no");
}

static void test_pools(void)
{
    uint64_t a = pool_create(MiB), b = pool_create(MiB);
    CHECK(a && b && a != b, "ids %lx %lx", (unsigned long)a, (unsigned long)b);
    CHECK(pool_create(0) == 0, "zero bytes accepted");
    CHECK(pool_create(PAGE + 1) == 0, "unaligned bytes accepted");
    CHECK(pool_destroy(a) == 0, "destroy refused");
    CHECK(pool_destroy(a) != 0, "destroyed twice");
    CHECK(pool_destroy(0) != 0, "destroyed id 0");
    CHECK(pool_destroy(12345) != 0, "destroyed unknown id");
    uint64_t c = pool_create(MiB); /* reuses a's slot */
    CHECK(c && c != a && c != b, "reused id %lx", (unsigned long)c);
    CHECK(pool_destroy(a) != 0, "old id names the new pool");
    pool_destroy(b);
    pool_destroy(c);
}

static void test_windows(void)
{
    uint8_t *w = reserve(4 * MiB);
    CHECK(window(VB_WINDOW_ADD, w + 1, MiB) != 0, "unaligned window");
    CHECK(window(VB_WINDOW_ADD, w, 0) != 0, "empty window");
    CHECK(window(VB_WINDOW_ADD, NULL, MiB) != 0, "window at 0");
    CHECK(window(VB_WINDOW_ADD, w, 2 * MiB) == 0, "add refused");
    CHECK(window(VB_WINDOW_ADD, w + MiB, 2 * MiB) != 0, "overlap accepted");
    CHECK(window(VB_WINDOW_ADD, w + 2 * MiB, 2 * MiB) == 0, "adjacent refused");
    CHECK(window(VB_WINDOW_REMOVE, w, MiB) != 0, "removed with the wrong length");
    CHECK(window(VB_WINDOW_REMOVE, w + PAGE, 2 * MiB) != 0, "removed at the wrong address");
    CHECK(window(VB_WINDOW_REMOVE, w, 2 * MiB) == 0, "remove refused");
    CHECK(window(VB_WINDOW_REMOVE, w, 2 * MiB) != 0, "removed twice");
    CHECK(window(VB_WINDOW_REMOVE, w + 2 * MiB, 2 * MiB) == 0, "remove refused");
    munmap(w, 4 * MiB);
}

/* Map, aliases, unmap, replace, protect, destroy with views standing, window removal. */
static void test_views(void)
{
    uint64_t pool = pool_create(MiB);
    uint8_t *w = reserve(MiB), v;
    char perms[5];
    int anon;
    CHECK(window(VB_WINDOW_ADD, w, MiB) == 0, "add");
    uint8_t *A = w, *B = w + VIEW, *C = w + 2 * VIEW;

    CHECK(map1(pool, A, VIEW, 0, PROT_READ | PROT_WRITE, 0) == 0, "map A");
    CHECK(map1(pool, B, VIEW, 0, PROT_READ | PROT_WRITE, 0) == 0, "map B (alias of A)");
    CHECK(map1(pool, C, VIEW, VIEW, PROT_READ | PROT_WRITE, 0) == 0, "map C");
    memset(A, 0x11, VIEW);
    memset(C, 0x33, VIEW);
    CHECK(B[0] == 0x11 && B[VIEW - 1] == 0x11, "alias does not see A's write");
    B[100] = 0x22;
    CHECK(A[100] == 0x22, "A does not see the alias's write");
    CHECK(maps_at(A, perms, &anon, NULL, NULL) && !strcmp(perms, "rw-s") && !anon, "A: %s anon %d", perms, anon);

    /* unmap: PROT_NONE, anonymous, still there (reserved); the pool keeps the bytes */
    CHECK(range1(VB_UNMAP, B, VIEW, 0) == 0, "unmap B");
    CHECK(try_read(B, &v) == SIGSEGV, "unmapped B readable");
    uint64_t lo, hi;
    CHECK(maps_at(B, perms, &anon, &lo, &hi) && !strcmp(perms, "---p") && anon,
          "unmapped B: %s anon %d", perms, anon);
    CHECK(lo <= (uint64_t)(uintptr_t)B && hi >= (uint64_t)(uintptr_t)(B + VIEW), "unmapped B not covered");
    CHECK(A[100] == 0x22, "A lost the pool's bytes");
    CHECK(map1(pool, B, VIEW, 0, PROT_READ, 0) == 0, "map B again");
    CHECK(B[100] == 0x22 && B[0] == 0x11, "B again: %x %x", B[100], B[0]);

    /* map over an existing view replaces it */
    CHECK(map1(pool, A, VIEW, VIEW, PROT_READ | PROT_WRITE, 0) == 0, "replace A");
    CHECK(A[0] == 0x33 && A[100] == 0x33, "A not replaced: %x", A[0]);
    A[5] = 0x44;
    CHECK(C[5] == 0x44, "replaced A is not an alias of C");
    CHECK(B[5] == 0x11, "B changed by a write through the replaced A");

    /* protections */
    CHECK(try_write(B, 1) == SIGSEGV, "read-only B writable");
    CHECK(range1(VB_PROTECT, B, VIEW, PROT_READ | PROT_WRITE) == 0, "protect B rw");
    CHECK(try_write(B + 7, 0x55) == 0, "rw B not writable");
    CHECK(A[0] != 0x55 && C[7] == 0x33, "B's write landed elsewhere");
    CHECK(range1(VB_PROTECT, B, VIEW, PROT_NONE) == 0, "protect B none");
    CHECK(try_read(B, &v) == SIGSEGV, "PROT_NONE B readable");
    CHECK(range1(VB_PROTECT, B, VIEW, PROT_READ) == 0, "protect B r");
    CHECK(try_read(B + 7, &v) == 0 && v == 0x55, "B after protect: %x", v);
    CHECK(range1(VB_PROTECT, B, VIEW, 8) == EINVAL, "protect with an unknown bit");
    CHECK(range1(VB_PROTECT, B, PAGE, PROT_READ | PROT_WRITE) == 0, "protect one page");
    CHECK(try_write(B, 0x66) == 0 && try_write(B + PAGE, 0x66) == SIGSEGV, "only the first page");

    /* destroying the pool keeps its views; the id maps nothing any more */
    CHECK(pool_destroy(pool) == 0, "destroy");
    CHECK(A[5] == 0x44 && C[5] == 0x44, "views lost after destroy");
    C[9] = 0x77;
    CHECK(A[9] == 0x77, "aliases broken after destroy");
    CHECK(map1(pool, w + 4 * VIEW, VIEW, 0, PROT_READ, 0) == -1, "map of a destroyed pool not refused");

    /* window removal: everything back to PROT_NONE anonymous, ops refused afterwards */
    CHECK(window(VB_WINDOW_REMOVE, w, MiB) == 0, "remove");
    CHECK(try_read(A, &v) == SIGSEGV && try_read(C, &v) == SIGSEGV, "views survive the window");
    CHECK(maps_at(A, perms, &anon, &lo, &hi) && !strcmp(perms, "---p") && anon && lo <= (uint64_t)(uintptr_t)w
              && hi >= (uint64_t)(uintptr_t)(w + MiB),
          "removed window: %s anon %d", perms, anon);
    CHECK(range1(VB_UNMAP, A, VIEW, 0) == EFAULT, "unmap in a removed window");
    munmap(w, MiB);
}

/* Per-op refusals, counts, results, done; the call's own refusals. */
static void test_batches(void)
{
    uint64_t pool = pool_create(MiB);
    uint8_t *space = reserve(4 * MiB), *w = space + MiB, *outside = space + 3 * MiB;
    int32_t r[8];
    uint64_t done;
    CHECK(window(VB_WINDOW_ADD, w, MiB) == 0, "add");
    /* a sentinel just outside the window, which no refused op may touch */
    CHECK(mprotect(outside, PAGE, PROT_READ | PROT_WRITE) == 0, "sentinel");
    outside[0] = 0x5a;

    struct vb_map_op m[8] = {
        { (uint64_t)(uintptr_t)w, VIEW, 0, PROT_READ | PROT_WRITE, 0 },                       /* ok */
        { (uint64_t)(uintptr_t)outside, PAGE, 0, PROT_READ | PROT_WRITE, 0 },                 /* outside */
        { (uint64_t)(uintptr_t)(w + MiB - PAGE), 2 * PAGE, 0, PROT_READ, 0 },                 /* straddles the end */
        { (uint64_t)(uintptr_t)(w + VIEW + 1), PAGE, 0, PROT_READ, 0 },                       /* unaligned addr */
        { (uint64_t)(uintptr_t)(w + VIEW), PAGE, 3, PROT_READ, 0 },                           /* unaligned offset */
        { (uint64_t)(uintptr_t)(w + VIEW), 2 * PAGE, MiB - PAGE, PROT_READ, 0 },              /* past the pool */
        { (uint64_t)(uintptr_t)(w + 2 * VIEW), VIEW, VIEW, PROT_READ, 0x80 },                 /* unknown flag */
        { (uint64_t)(uintptr_t)(w + 3 * VIEW), VIEW, MiB - VIEW, PROT_READ | PROT_WRITE, 0 }, /* ok, last pool bytes */
    };
    for (int i = 0; i < 8; i++) r[i] = -7;
    CHECK(batch(VB_MAP, pool, m, 8, r, &done) == 0, "map batch refused");
    int want[8] = { 0, EFAULT, EFAULT, EINVAL, EINVAL, EINVAL, EINVAL, 0 };
    for (int i = 0; i < 8; i++) CHECK(r[i] == want[i], "map op %d: %d, want %d", i, r[i], want[i]);
    CHECK(done == 2, "done %lu", (unsigned long)done);
    CHECK(outside[0] == 0x5a, "sentinel touched");
    w[0] = 1;
    w[3 * VIEW] = 2;

    struct vb_range u[4] = {
        { (uint64_t)(uintptr_t)(space), PAGE, 0, 0 },    /* before the window */
        { (uint64_t)(uintptr_t)w, VIEW, 0, 0 },          /* ok */
        { (uint64_t)(uintptr_t)w, 0, 0, 0 },             /* empty */
        { (uint64_t)(uintptr_t)(w + VIEW), VIEW, 0, 0 }, /* ok: unmapping unmapped is fine */
    };
    CHECK(batch(VB_UNMAP, 0, u, 4, r, &done) == 0 && done == 2, "unmap batch: done %lu", (unsigned long)done);
    CHECK(r[0] == EFAULT && r[1] == 0 && r[2] == EINVAL && r[3] == 0, "unmap results %d %d %d %d", r[0], r[1], r[2], r[3]);

    struct vb_range p[2] = { { (uint64_t)(uintptr_t)outside, PAGE, PROT_NONE, 0 },
                             { (uint64_t)(uintptr_t)(w + 3 * VIEW), VIEW, PROT_READ, 0 } };
    CHECK(batch(VB_PROTECT, 0, p, 2, r, &done) == 0 && done == 1 && r[0] == EFAULT && r[1] == 0, "protect batch");
    CHECK(outside[0] == 0x5a, "sentinel protected");

    /* the call's own refusals */
    static struct vb_range big[VB_BATCH_MAX + 1];
    static int32_t big_r[VB_BATCH_MAX + 1];
    for (int i = 0; i <= VB_BATCH_MAX; i++) big[i] = (struct vb_range){ (uint64_t)(uintptr_t)w, PAGE, 0, 0 };
    CHECK(batch(VB_UNMAP, 0, big, VB_BATCH_MAX + 1, big_r, &done) != 0, "over BATCH_MAX accepted");
    CHECK(batch(VB_UNMAP, 0, big, VB_BATCH_MAX, big_r, &done) == 0 && done == VB_BATCH_MAX, "BATCH_MAX: done %lu",
          (unsigned long)done);
    CHECK(batch(VB_UNMAP, 0, big, 0, NULL, &done) == 0 && done == 0, "empty batch");
    CHECK(batch(VB_MAP, 0, m, 1, r, &done) != 0, "map with pool 0 accepted");
    CHECK(batch(VB_MAP, pool + 1, m, 1, r, &done) != 0, "map with an unknown pool accepted");
    CHECK(batch(VB_MAP, pool, m, 1, NULL, &done) != 0, "map without results accepted");

    window(VB_WINDOW_REMOVE, w, MiB);
    pool_destroy(pool);
    munmap(space, 4 * MiB);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void test_watch(void)
{
    if (!(features & VB_FEATURE_WATCH)) {
        printf("watch: feature off, skipped (calls must be refused)\n");
        struct vb_watch_take t = { 0 };
        CHECK(call(VB_WATCH_TAKE, &t) != 0, "take accepted without the feature");
        return;
    }
    enum { VIEWS = 16 };
    uint64_t pool = pool_create(8 * VIEW), got[4096];
    uint8_t *w = reserve(VIEWS * VIEW), v;
    CHECK(window(VB_WINDOW_ADD, w, VIEWS * VIEW) == 0, "add");
    /* views 0-7: pool slots 0-7, watched at map time; views 8-15: aliases of 0-7, watched later */
    for (int i = 0; i < VIEWS; i++)
        CHECK(map1(pool, w + i * VIEW, VIEW, (i % 8) * VIEW, PROT_READ | PROT_WRITE, i < 8 ? VB_MAP_WATCH : 0) == 0,
              "map %d", i);
    CHECK(range1(VB_WATCH_REGISTER, w + 8 * VIEW, 8 * VIEW, 0) == 0, "register 8-15");
    CHECK(take(w, VIEWS * VIEW, got, 4096, 4096, NULL) == 0, "fresh views report writes");

    /* exactly the written pages; reads do not count; unpopulated pages count when written */
    for (int i = 0; i < VIEWS; i++) v = w[i * VIEW + PAGE];
    (void)v;
    uint64_t want[] = { 0, 3 * PAGE, VIEW + 15 * PAGE, 9 * VIEW + 2 * PAGE, 15 * VIEW };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) w[want[i] + 17] = (uint8_t)i;
    long n = take(w, VIEWS * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 5, "took %ld pages, want 5", n);
    for (long i = 0; i < n && i < 5; i++)
        CHECK(got[i] == (uint64_t)(uintptr_t)(w + want[i]), "page %ld at +%lx, want +%lx", i,
              (unsigned long)(got[i] - (uint64_t)(uintptr_t)w), (unsigned long)want[i]);
    /* aliases reported only where written: view 9 aliases view 1, view 15 view 7 */
    CHECK(w[VIEW + 2 * PAGE + 17] == 3, "alias did not see the data");

    /* re-armed */
    CHECK(take(w, VIEWS * VIEW, got, 4096, 4096, NULL) == 0, "not re-armed");
    w[3 * PAGE] = 9;
    n = take(w, VIEWS * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 1 && got[0] == (uint64_t)(uintptr_t)(w + 3 * PAGE), "second write: %ld pages", n);

    /* a sub-range takes only its own pages, and leaves the rest for later */
    w[2 * VIEW] = 1;
    w[5 * VIEW] = 1;
    n = take(w + 4 * VIEW, 4 * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 1 && got[0] == (uint64_t)(uintptr_t)(w + 5 * VIEW), "sub-range: %ld", n);
    n = take(w, VIEWS * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 1 && got[0] == (uint64_t)(uintptr_t)(w + 2 * VIEW), "the rest: %ld", n);

    /* small capacity pages through `more`, nothing lost or doubled */
    uint64_t written[200];
    int nw = 0;
    for (int i = 0; i < VIEWS * 16; i++)
        if ((i * 7) % 5 == 0 && nw < 200) {
            w[(uint64_t)i * PAGE + 1] = 1;
            written[nw++] = (uint64_t)(uintptr_t)(w + (uint64_t)i * PAGE);
        }
    for (uint64_t cap = 1; cap <= 64; cap = cap * 3 + 1) {
        int calls;
        if (cap > 1)
            for (int i = 0; i < nw; i++) *(volatile uint8_t *)(uintptr_t)(written[i] + 1) = 2;
        n = take(w, VIEWS * VIEW, got, 4096, cap, &calls);
        qsort(got, n > 0 ? (size_t)n : 0, sizeof *got, cmp_u64);
        int same = n == nw;
        for (int i = 0; same && i < nw; i++) same = got[i] == written[i];
        CHECK(same, "capacity %lu: %ld pages of %d", (unsigned long)cap, n, nw);
        CHECK(calls >= (nw + (int)cap - 1) / (int)cap, "capacity %lu: %d calls", (unsigned long)cap, calls);
        CHECK(take(w, VIEWS * VIEW, got, 4096, 4096, NULL) == 0, "capacity %lu: left armed pages", (unsigned long)cap);
    }
    /* exactly capacity pages written, the last at the range's end */
    w[VIEWS * VIEW - PAGE] = 1;
    w[0] = 1;
    {
        uint64_t buf[2];
        struct vb_watch_take t = { (uint64_t)(uintptr_t)w, VIEWS * VIEW, (uint64_t)(uintptr_t)buf, 2, 0, 0 };
        CHECK(call(VB_WATCH_TAKE, &t) == 0 && t.count == 2 && !t.more, "two at both ends: %lu more %lu",
              (unsigned long)t.count, (unsigned long)t.more);
        t = (struct vb_watch_take){ (uint64_t)(uintptr_t)w, VIEWS * VIEW, (uint64_t)(uintptr_t)buf, 0, 0, 0 };
        CHECK(call(VB_WATCH_TAKE, &t) != 0, "capacity 0 accepted");
        t = (struct vb_watch_take){ (uint64_t)(uintptr_t)(w - PAGE), PAGE, (uint64_t)(uintptr_t)buf, 2, 0, 0 };
        CHECK(call(VB_WATCH_TAKE, &t) != 0, "take outside a window accepted");
    }

    /* remap (watched) starts clean, data kept; unwatched remap reports nothing */
    w[4 * VIEW] = 0x42;
    CHECK(map1(pool, w + 4 * VIEW, VIEW, 4 * VIEW, PROT_READ | PROT_WRITE, VB_MAP_WATCH) == 0, "remap");
    CHECK(take(w, VIEWS * VIEW, got, 4096, 4096, NULL) == 0, "remap not clean");
    CHECK(w[4 * VIEW] == 0x42, "remap lost data");
    CHECK(map1(pool, w + 4 * VIEW, VIEW, 4 * VIEW, PROT_READ | PROT_WRITE, 0) == 0, "remap unwatched");
    w[4 * VIEW] = 1;
    CHECK(take(w, VIEWS * VIEW, got, 4096, 4096, NULL) == 0, "unwatched view reported");
    /* registering twice is fine; the take over unmapped pieces skips them */
    CHECK(range1(VB_WATCH_REGISTER, w, 2 * VIEW, 0) == 0, "register again");
    CHECK(range1(VB_UNMAP, w + VIEW, VIEW, 0) == 0, "unmap");
    w[0] = 1;
    n = take(w, VIEWS * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 1 && got[0] == (uint64_t)(uintptr_t)w, "with a hole: %ld", n);
    /* read-only watched view: writable again after protect, the write reported */
    CHECK(range1(VB_PROTECT, w + 2 * VIEW, VIEW, PROT_READ) == 0, "protect r");
    CHECK(try_write(w + 2 * VIEW, 1) == SIGSEGV, "read-only written");
    CHECK(range1(VB_PROTECT, w + 2 * VIEW, VIEW, PROT_READ | PROT_WRITE) == 0, "protect rw");
    w[2 * VIEW + PAGE] = 1;
    n = take(w, VIEWS * VIEW, got, 4096, 4096, NULL);
    CHECK(n == 1 && got[0] == (uint64_t)(uintptr_t)(w + 2 * VIEW + PAGE), "after protect: %ld", n);

    window(VB_WINDOW_REMOVE, w, VIEWS * VIEW);
    pool_destroy(pool);
    munmap(w, VIEWS * VIEW);
}

static void test_copy(void)
{
    uint64_t pool = pool_create(4 * VIEW);
    uint8_t *w = reserve(4 * VIEW), *dst = malloc(4 * VIEW);
    CHECK(window(VB_WINDOW_ADD, w, 4 * VIEW) == 0, "add");
    for (int i = 0; i < 4; i++) CHECK(map1(pool, w + i * VIEW, VIEW, i * VIEW, PROT_READ | PROT_WRITE, 0) == 0, "map");
    for (uint64_t i = 0; i < 4 * VIEW; i++) w[i] = (uint8_t)(i * 31 + 7);

    struct vb_copy c = { (uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)w, 4 * VIEW, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 4 * VIEW && !memcmp(dst, w, 4 * VIEW), "whole: %lu", (unsigned long)c.copied);
    /* across an unmapped view: stops at it */
    CHECK(range1(VB_UNMAP, w + 2 * VIEW, VIEW, 0) == 0, "unmap");
    memset(dst, 0, 4 * VIEW);
    c = (struct vb_copy){ (uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)(w + 100), 4 * VIEW - 200, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 2 * VIEW - 100, "across a hole: %lu, want %lu", (unsigned long)c.copied,
          (unsigned long)(2 * VIEW - 100));
    CHECK(!memcmp(dst, w + 100, c.copied), "bytes before the hole");
    /* from a PROT_NONE view, and into one: nothing, no fault */
    CHECK(range1(VB_PROTECT, w + VIEW, VIEW, PROT_NONE) == 0, "protect none");
    c = (struct vb_copy){ (uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)(w + VIEW), PAGE, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 0, "from PROT_NONE: %lu", (unsigned long)c.copied);
    c = (struct vb_copy){ (uint64_t)(uintptr_t)(w + 2 * VIEW - 10), (uint64_t)(uintptr_t)dst, 100, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 0, "into PROT_NONE: %lu", (unsigned long)c.copied);
    /* into a read-only view (process_vm_readv writes locally: refused by the page's protection) */
    CHECK(range1(VB_PROTECT, w, VIEW, PROT_READ) == 0, "protect r");
    c = (struct vb_copy){ (uint64_t)(uintptr_t)w, (uint64_t)(uintptr_t)dst, 100, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 0, "into read-only: %lu", (unsigned long)c.copied);
    /* into a destination that ends early: stops there */
    c = (struct vb_copy){ (uint64_t)(uintptr_t)(w + 3 * VIEW + VIEW - 300), (uint64_t)(uintptr_t)dst, 1000, 0 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 300, "destination ends: %lu", (unsigned long)c.copied);
    c = (struct vb_copy){ (uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)w, 0, 5 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 0, "zero bytes");
    c = (struct vb_copy){ (uint64_t)(uintptr_t)dst, 8, 10, 5 };
    CHECK(call(VB_COPY, &c) == 0 && c.copied == 0, "from address 8");

    window(VB_WINDOW_REMOVE, w, 4 * VIEW);
    pool_destroy(pool);
    munmap(w, 4 * VIEW);
    free(dst);
}

/* EINTR: a thread runs batches while another floods it with a signal whose handler is installed
 * without SA_RESTART; every op must still succeed and the watch stay exact. */
static volatile int stop;
static volatile long signals;
static void on_usr1(int s) { (void)s; signals++; }

struct churn { uint64_t pool; uint8_t *w; int views, rounds, bad; };

static void *churn_thread(void *arg)
{
    struct churn *c = arg;
    static struct vb_map_op m[256];
    static struct vb_range r[256];
    static int32_t res[256];
    uint64_t got[512], done;
    int watch = (features & VB_FEATURE_WATCH) != 0;
    for (int round = 0; round < c->rounds; round++) {
        for (int i = 0; i < c->views; i++)
            m[i] = (struct vb_map_op){ (uint64_t)(uintptr_t)(c->w + i * VIEW), VIEW, (uint64_t)(i % 4) * VIEW,
                                       PROT_READ | PROT_WRITE, watch ? VB_MAP_WATCH : 0 };
        if (batch(VB_MAP, c->pool, m, c->views, res, &done) || done != (uint64_t)c->views) c->bad++;
        if (watch) {
            c->w[(round % c->views) * VIEW + PAGE] = 1;
            long n = take(c->w, c->views * VIEW, got, 512, 512, NULL);
            if (n != 1 || got[0] != (uint64_t)(uintptr_t)(c->w + (round % c->views) * VIEW + PAGE)) c->bad++;
        }
        for (int i = 0; i < c->views; i++) r[i] = (struct vb_range){ (uint64_t)(uintptr_t)(c->w + i * VIEW), VIEW, PROT_READ, 0 };
        if (batch(VB_PROTECT, 0, r, c->views, res, &done) || done != (uint64_t)c->views) c->bad++;
        struct vb_copy cp = { (uint64_t)(uintptr_t)got, (uint64_t)(uintptr_t)c->w, sizeof got, 0 };
        if (call(VB_COPY, &cp) || cp.copied != sizeof got) c->bad++;
        if (batch(VB_UNMAP, 0, r, c->views, res, &done) || done != (uint64_t)c->views) c->bad++;
    }
    stop = 1;
    return NULL;
}

static void test_eintr(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_usr1; /* no SA_RESTART */
    sigaction(SIGUSR1, &sa, NULL);
    struct churn c = { pool_create(4 * VIEW), reserve(256 * VIEW), 256, 300, 0 };
    CHECK(window(VB_WINDOW_ADD, c.w, 256 * VIEW) == 0, "add");
    pthread_t t;
    stop = 0;
    signals = 0;
    pthread_create(&t, NULL, churn_thread, &c);
    while (!stop) {
        pthread_kill(t, SIGUSR1);
        for (volatile int i = 0; i < 200; i++) {}
    }
    pthread_join(t, NULL);
    CHECK(c.bad == 0, "%d bad rounds of %d under %ld signals", c.bad, c.rounds, signals);
    printf("eintr: %d rounds of 256 map/take/protect/copy/unmap under %ld signals, %d bad\n", c.rounds, signals, c.bad);
    window(VB_WINDOW_REMOVE, c.w, 256 * VIEW);
    pool_destroy(c.pool);
    munmap(c.w, 256 * VIEW);
    signal(SIGUSR1, SIG_DFL);
}

/* Several threads at once: takes and copies on one range while another thread maps elsewhere. */
struct par { uint64_t pool; uint8_t *w; int id, bad; };
static void *par_thread(void *arg)
{
    struct par *p = arg;
    uint8_t *mine = p->w + (uint64_t)p->id * 64 * VIEW;
    static __thread struct vb_map_op m[64];
    static __thread int32_t res[64];
    uint64_t got[64], done;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 64; i++)
            m[i] = (struct vb_map_op){ (uint64_t)(uintptr_t)(mine + i * VIEW), VIEW, (uint64_t)i * VIEW, PROT_READ | PROT_WRITE,
                                       (features & VB_FEATURE_WATCH) ? VB_MAP_WATCH : 0 };
        if (batch(VB_MAP, p->pool, m, 64, res, &done) || done != 64) p->bad++;
        if (features & VB_FEATURE_WATCH) {
            mine[(round % 64) * VIEW] = 1;
            long n = take(mine, 64 * VIEW, got, 64, 64, NULL);
            if (n != 1 || got[0] != (uint64_t)(uintptr_t)(mine + (round % 64) * VIEW)) p->bad++;
        }
    }
    return NULL;
}

static void test_threads(void)
{
    enum { T = 6 };
    uint64_t pool = pool_create(64 * VIEW);
    uint8_t *w = reserve(T * 64 * VIEW);
    CHECK(window(VB_WINDOW_ADD, w, T * 64 * VIEW) == 0, "add");
    pthread_t t[T];
    struct par p[T];
    for (int i = 0; i < T; i++) {
        p[i] = (struct par){ pool, w, i, 0 };
        pthread_create(&t[i], NULL, par_thread, &p[i]);
    }
    /* meanwhile: pools and windows come and go */
    for (int i = 0; i < 2000; i++) {
        uint64_t q = pool_create(VIEW);
        uint8_t *x = reserve(VIEW);
        if (!q || window(VB_WINDOW_ADD, x, VIEW) || window(VB_WINDOW_REMOVE, x, VIEW) || pool_destroy(q)) p[0].bad++;
        munmap(x, VIEW);
    }
    int bad = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(t[i], NULL);
        bad += p[i].bad;
    }
    CHECK(bad == 0, "%d bad rounds", bad);
    window(VB_WINDOW_REMOVE, w, T * 64 * VIEW);
    pool_destroy(pool);
    munmap(w, T * 64 * VIEW);
}

/* ---- speed ---- */

static void takes(uint8_t *w, uint64_t len, int views, uint64_t *got, int *badp)
{
    int bad = 0;
    uint64_t t0;
    if (!(features & VB_FEATURE_WATCH)) return;
    {
        long n;
        int calls;
        take(w, len, got, 200000, 4096, NULL);
        t0 = ns_now();
        n = take(w, len, got, 200000, 4096, &calls);
        printf("  %-40s %7.3f ms (%ld pages)\n", "take whole window, nothing written", (ns_now() - t0) / 1e6, n);
        for (int i = 0; i < 100; i++) w[(uint64_t)(i * 599 % views) * VIEW + (uint64_t)(i % 16) * PAGE] = 1;
        t0 = ns_now();
        n = take(w, len, got, 200000, 4096, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "take whole window, 100 pages written", (ns_now() - t0) / 1e6, n, calls);
        if (n != 100) bad++;
        for (int i = 0; i < views; i++) w[(uint64_t)i * VIEW + (uint64_t)(i % 16) * PAGE] = 1;
        t0 = ns_now();
        n = take(w, len, got, 200000, 4096, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "take whole window, 1 page in every view", (ns_now() - t0) / 1e6, n, calls);
        if (n != views) bad++;
        for (int i = 0; i < views; i += 10) memset(w + (uint64_t)i * VIEW, 1, VIEW);
        t0 = ns_now();
        n = take(w, len, got, 200000, 4096, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "take whole window, every 10th view all", (ns_now() - t0) / 1e6, n, calls);
        if (n != (views + 9) / 10 * 16) bad++;
        for (int i = 0; i < views; i++) w[(uint64_t)i * VIEW + (uint64_t)(i % 16) * PAGE] = 1;
        t0 = ns_now();
        n = take(w, len, got, 200000, 512, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "same, 1 page per view, capacity 512 (PE)", (ns_now() - t0) / 1e6, n, calls);
        if (n != views) bad++;
        for (int i = 0; i < views; i++) w[(uint64_t)i * VIEW + (uint64_t)(i % 16) * PAGE] = 1;
        t0 = ns_now();
        n = take(w, len, got, 200000, 200000, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "same, capacity 200,000", (ns_now() - t0) / 1e6, n, calls);
        if (n != views) bad++;
        t0 = ns_now();
        n = take(w, len, got, 200000, 200000, &calls);
        printf("  %-40s %7.3f ms (%ld pages, %d calls)\n", "nothing written, capacity 200,000", (ns_now() - t0) / 1e6, n, calls);
        t0 = ns_now();
        for (int i = 0; i < 1000; i++) take(w + (uint64_t)(i * 37 % views) * VIEW, VIEW, got, 16, 16, NULL);
        printf("  %-40s %7.3f us\n", "take one 64 KiB view, nothing written", (ns_now() - t0) / 1000.0 / 1000);
    }
    *badp += bad;
}

static volatile int busy_stop;
static void *busy(void *arg)
{
    (void)arg;
    while (!busy_stop) {}
    return NULL;
}

static void speed(int views)
{
    const uint64_t slots = 8192, pool_bytes = slots * VIEW, len = (uint64_t)views * VIEW;
    uint64_t pool = pool_create(pool_bytes), t0, done;
    uint8_t *w = reserve(len);
    static struct vb_map_op m[VB_BATCH_MAX];
    static struct vb_range r[VB_BATCH_MAX];
    static int32_t res[VB_BATCH_MAX];
    uint64_t *got = malloc(sizeof *got * 200000);
    int bad = 0;

    if (window(VB_WINDOW_ADD, w, len)) {
        printf("speed: window refused\n");
        return;
    }
    printf("\nspeed: %d views of 64 KiB (%.2f GiB) over a %lu MiB pool, batches of %d\n", views,
           len / (double)(1ull << 30), (unsigned long)(pool_bytes / MiB), VB_BATCH_MAX);

#define ALL(code_body)                                                                          \
    do {                                                                                        \
        for (int at = 0; at < views; at += VB_BATCH_MAX) {                                      \
            int k = views - at < VB_BATCH_MAX ? views - at : VB_BATCH_MAX;                      \
            for (int j = 0; j < k; j++) {                                                       \
                int i = at + j;                                                                 \
                (void)i;                                                                        \
                code_body;                                                                      \
            }                                                                                   \
            BATCH_CALL;                                                                         \
        }                                                                                       \
    } while (0)

    int watch_flag = (features & VB_FEATURE_WATCH) ? VB_MAP_WATCH : 0;
    struct { const char *what; uint32_t flags; int replace; } maps[] = {
        { "map (into unmapped)", 0, 0 },
        { "replace in place (map over a view)", 0, 1 },
        { "unmap", 0, -1 },
        { "map + MAP_WATCH (into unmapped)", (uint32_t)watch_flag, 0 },
        { "replace in place + MAP_WATCH", (uint32_t)watch_flag, 1 },
    };
    for (unsigned s = 0; s < sizeof maps / sizeof *maps; s++) {
        if (maps[s].flags == 0 && strstr(maps[s].what, "WATCH")) continue;
        t0 = ns_now();
        if (maps[s].replace < 0) {
#define BATCH_CALL if (batch(VB_UNMAP, 0, r, k, res, &done) || done != (uint64_t)k) bad++
            ALL(r[j] = ((struct vb_range){ (uint64_t)(uintptr_t)(w + (uint64_t)i * VIEW), VIEW, 0, 0 }));
#undef BATCH_CALL
        } else {
#define BATCH_CALL if (batch(VB_MAP, pool, m, k, res, &done) || done != (uint64_t)k) bad++
            ALL(m[j] = ((struct vb_map_op){ (uint64_t)(uintptr_t)(w + (uint64_t)i * VIEW), VIEW,
                                            (uint64_t)((i + maps[s].replace) % slots) * VIEW, PROT_READ | PROT_WRITE,
                                            maps[s].flags }));
#undef BATCH_CALL
        }
        printf("  %-40s %7.3f us/op\n", maps[s].what, (ns_now() - t0) / 1000.0 / views);
        if (maps[s].flags && !maps[s].replace) {
            printf("  takes right after the watched map (%d VMAs):\n", vmas_in(w, w + len));
            takes(w, len, views, got, &bad);
            printf("  then:\n");
        }
    }

    /* protect: all views to read-only, then back */
    t0 = ns_now();
#define BATCH_CALL if (batch(VB_PROTECT, 0, r, k, res, &done) || done != (uint64_t)k) bad++
    ALL(r[j] = ((struct vb_range){ (uint64_t)(uintptr_t)(w + (uint64_t)i * VIEW), VIEW, PROT_READ, 0 }));
    ALL(r[j] = ((struct vb_range){ (uint64_t)(uintptr_t)(w + (uint64_t)i * VIEW), VIEW, PROT_READ | PROT_WRITE, 0 }));
#undef BATCH_CALL
    printf("  %-40s %7.3f us/op\n", "protect", (ns_now() - t0) / 1000.0 / (2.0 * views));

    /* churn: a random view unmapped and mapped again, at full occupancy */
    {
        unsigned seed = 1;
        int n = 20000;
        t0 = ns_now();
        for (int i = 0; i < n; i++) {
            int v = (int)((seed = seed * 1103515245u + 12345u) >> 8) % views;
            uint8_t *at = w + (uint64_t)v * VIEW;
            if (range1(VB_UNMAP, at, VIEW, 0) || map1(pool, at, VIEW, (v % slots) * VIEW, PROT_READ | PROT_WRITE, (uint32_t)watch_flag))
                bad++;
        }
        printf("  %-40s %7.3f us/pair\n", watch_flag ? "churn, unmap + map (watched), 1 op/call" : "churn, unmap + map, 1 op/call",
               (ns_now() - t0) / 1000.0 / n);
    }

    printf("  takes after protect and churn (%d VMAs):\n", vmas_in(w, w + len));
    takes(w, len, views, got, &bad);
    {
        enum { BUSY = 8 };
        pthread_t t[BUSY];
        busy_stop = 0;
        for (int i = 0; i < BUSY; i++) pthread_create(&t[i], NULL, busy, NULL);
        printf("  the same with %d other threads busy (TLB flushes reach them):\n", BUSY);
        takes(w, len, views, got, &bad);
        busy_stop = 1;
        for (int i = 0; i < BUSY; i++) pthread_join(t[i], NULL);
    }
    CHECK(bad == 0, "%d failed batches or wrong counts in the speed run", bad);
    window(VB_WINDOW_REMOVE, w, len);
    pool_destroy(pool);
    munmap(w, len);
    free(got);
}

int main(int argc, char **argv)
{
    int do_speed = 0, views = 60000;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <victorybridge.so> [--speed] [--views N]\n", argv[0]);
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--speed")) do_speed = 1;
        else if (!strcmp(argv[i], "--views") && i + 1 < argc) views = atoi(argv[++i]);
    }
    void *so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so || !(funcs = dlsym(so, "__wine_unix_call_funcs"))) {
        fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror());
        return 2;
    }
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    test_version();
    test_pools();
    test_windows();
    test_views();
    test_batches();
    test_watch();
    test_copy();
    test_eintr();
    test_threads();
    if (do_speed) speed(views);
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
