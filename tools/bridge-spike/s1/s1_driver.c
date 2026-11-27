/* S1 spike driver: the same tests built as a Windows exe (runs under Proton, calls the Linux half
 * through s1bridge.dll; all writes are made by PE code) and natively (links s1_unix.c directly).
 *   s1.exe [views]      (default 60000 views of 64 KiB for the cost part) */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "s1.h"

#define PAGE 4096u
#define VIEW 65536u

#ifdef _WIN32
#include <windows.h>
typedef LONG (WINAPI *CALL)(unsigned int, void *);
static CALL call_p;
static void bcall(int c, void *a) { LONG s = call_p(c, a); if (s) { printf("unix call %d failed: %ld\n", c, s); exit(1); } }
static uint8_t *reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }
static uint8_t *alloc_rw(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
static void release(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
static void set_readonly(void *p, size_t n, int ro) { DWORD o; VirtualProtect(p, n, ro ? PAGE_READONLY : PAGE_READWRITE, &o); }
static void decommit_recommit(void *p, size_t n) { VirtualFree(p, n, MEM_DECOMMIT); VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE); }
static double now_us(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e6 / (double)f.QuadPart; }
typedef HANDLE thr_t;
typedef void (*job_fn)(int, void *);
struct thr_arg { job_fn fn; int idx; void *ctx; };
static DWORD WINAPI thr_main(void *p) { struct thr_arg *a = p; a->fn(a->idx, a->ctx); return 0; }
static thr_t thr_start(struct thr_arg *a) { return CreateThread(NULL, 0, thr_main, a, 0, NULL); }
static void thr_join(thr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static volatile LONG faults;
static LONG WINAPI veh(EXCEPTION_POINTERS *e) { (void)e; InterlockedIncrement(&faults); return EXCEPTION_CONTINUE_SEARCH; }
static void platform_init(void)
{
    HMODULE m = LoadLibraryA("s1bridge.dll");
    if (!m || !(call_p = (CALL)GetProcAddress(m, "s1_call"))) { printf("s1bridge.dll not loaded (%lu)\n", GetLastError()); exit(1); }
    AddVectoredExceptionHandler(1, veh);
    printf("platform: Windows exe (Wine); vectored handler counting exceptions\n");
}
#else
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
typedef int (*unixlib_entry_t)(void *);
extern const unixlib_entry_t __wine_unix_call_funcs[];
static void bcall(int c, void *a) { __wine_unix_call_funcs[c](a); }
static uint8_t *reserve(size_t n) { void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
static uint8_t *alloc_rw(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
static void release(void *p, size_t n) { munmap(p, n); }
static void set_readonly(void *p, size_t n, int ro) { mprotect(p, n, ro ? PROT_READ : PROT_READ | PROT_WRITE); }
static void decommit_recommit(void *p, size_t n) { mmap(p, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0); mprotect(p, n, PROT_READ | PROT_WRITE); }
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
typedef pthread_t thr_t;
typedef void (*job_fn)(int, void *);
struct thr_arg { job_fn fn; int idx; void *ctx; };
static void *thr_main(void *p) { struct thr_arg *a = p; a->fn(a->idx, a->ctx); return NULL; }
static thr_t thr_start(struct thr_arg *a) { pthread_t t; pthread_create(&t, NULL, thr_main, a); return t; }
static void thr_join(thr_t t) { pthread_join(t, NULL); }
static volatile long faults;
static void on_segv(int s) { (void)s; faults++; static const char m[] = "SIGSEGV in native run\n"; write(2, m, sizeof m - 1); _exit(3); }
static void platform_init(void) { signal(SIGSEGV, on_segv); printf("platform: native Linux\n"); }
#endif
static void atomic_or8(volatile uint8_t *p, uint8_t v) { __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }

static struct s1_region *regions;
#define MAX_REGIONS (2u << 20)
static int failures;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("  [%s] ", ok_ ? "ok" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) failures++; } while (0)

static struct s1_scan scan(uint8_t *base, size_t len, int flags, uint8_t *mark)
{
    struct s1_scan s = { (uint64_t)(uintptr_t)base, len, (uint64_t)flags, (uint64_t)(uintptr_t)regions, MAX_REGIONS };
    bcall(S1_SCAN, &s);
    if (mark)
        for (uint64_t r = 0; r < s.n_regions; r++)
            for (uint64_t a = regions[r].start; a < regions[r].end; a += PAGE) mark[(a - (uintptr_t)base) / PAGE] = 1;
    return s;
}
static void map(uint8_t *base, uint64_t n, uint64_t slots, int flags, struct s1_map *m)
{
    *m = (struct s1_map){ (uint64_t)(uintptr_t)base, n, VIEW, slots, (uint64_t)flags };
    bcall(S1_MAP, m);
    if (m->err) { printf("  map failed at view %llu: errno %lld\n", (unsigned long long)m->failed_at, (long long)m->err); exit(1); }
}
static void unmap(uint8_t *base, uint64_t n, struct s1_map *m)
{
    *m = (struct s1_map){ (uint64_t)(uintptr_t)base, n, VIEW, 1, 0 };
    bcall(S1_UNMAP, m);
    if (m->err) { printf("  unmap failed: errno %lld\n", (long long)m->err); exit(1); }
}
static int64_t range_op(int op, void *p, size_t len)
{
    struct s1_range r = { (uint64_t)(uintptr_t)p, len };
    bcall(op, &r);
    return r.err;
}
static void compare(const uint8_t *want, const uint8_t *got, size_t n, size_t *missing, size_t *extra)
{
    *missing = *extra = 0;
    for (size_t i = 0; i < n; i++) { if (want[i] && !got[i]) ++*missing; if (!want[i] && got[i]) ++*extra; }
}
static uint64_t rng = 88172645463325252ull;
static uint64_t xs(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* --- writers (PE code under Wine) --- */
struct wjob { uint8_t *base; uint8_t *want; size_t npages; int nthreads; int mode; };
static void writer(int idx, void *ctx)
{
    struct wjob *j = ctx;
    for (size_t p = (size_t)idx; p < j->npages; p += (size_t)j->nthreads)
        if (j->want[p]) j->base[p * PAGE + 100 + (size_t)idx] = (uint8_t)(p + 1);
}
static void write_pages(uint8_t *base, uint8_t *want, size_t npages, int nthreads)
{
    struct wjob j = { base, want, npages, nthreads, 0 };
    struct thr_arg a[16]; thr_t t[16];
    for (int i = 0; i < nthreads; i++) { a[i] = (struct thr_arg){ writer, i, &j }; t[i] = thr_start(&a[i]); }
    for (int i = 0; i < nthreads; i++) thr_join(t[i]);
}

/* --- T1: memfd views, registered and armed per view at map time --- */
static void t1_views(void)
{
    enum { N = 64, SLOTS = 32, NP = N * VIEW / PAGE };
    printf("T1 memfd views: %d views of 64 KiB, views v and v+%d alias one pool offset, register+arm per view\n", N, SLOTS);
    struct s1_pool pl = { SLOTS * VIEW }; bcall(S1_POOL, &pl);
    uint8_t *base = reserve(N * VIEW), want[NP], got[NP];
    struct s1_map m; struct s1_scan s; size_t miss, extra;
    map(base, N, SLOTS, S1_MAP_REGISTER | S1_MAP_ARM, &m);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.ret >= 0 && s.pages == 0, "fresh armed views: scan finds %llu pages (ret %lld errno %lld)", (unsigned long long)s.pages, (long long)s.ret, (long long)s.err);

    memset(want, 0, NP);
    for (int v = 0; v < SLOTS; v++) want[v * 16 + (v * 7) % 16] = 1;          /* one page in each view 0..31 */
    for (int p = 0; p < 16; p += 3) want[5 * 16 + p] = 1;                        /* several in view 5 */
    write_pages(base, want, NP, 4);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    compare(want, got, NP, &miss, &extra);
    size_t alias = 0; for (int i = SLOTS * 16; i < NP; i++) alias += got[i];
    CHECK(miss == 0 && extra == 0, "first writes (unpopulated pages, 4 threads): %llu pages reported in %llu regions, missing %zu, extra %zu",
          (unsigned long long)s.pages, (unsigned long long)s.n_regions, miss, extra);
    int same = 1; for (int v = 0; v < SLOTS; v++) { int p = v * 16 + (v * 7) % 16; if (base[(size_t)(p + SLOTS * 16) * PAGE + 100 + (p % 4)] != (uint8_t)(p + 1)) same = 0; }
    printf("  aliases: the views not written through see the data: %s; their pages reported as written: %zu\n", same ? "yes" : "NO", alias);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.pages == 0, "second scan, no writes: %llu pages", (unsigned long long)s.pages);
    write_pages(base, want, NP, 4);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    compare(want, got, NP, &miss, &extra);
    CHECK(miss == 0 && extra == 0, "writes to re-armed populated pages: missing %zu, extra %zu", miss, extra);
    memset(want, 0, NP);
    for (int v = 0; v < SLOTS; v++) want[v * 16 + (v * 7) % 16] = 1;
    write_pages(base + SLOTS * VIEW, want, NP - SLOTS * 16, 4);                  /* now through the aliases */
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    size_t lo = 0, hi = 0; for (int i = 0; i < NP; i++) *(i < SLOTS * 16 ? &lo : &hi) += got[i];
    CHECK(hi == SLOTS && lo == 0, "writes through the alias views only: alias pages reported %zu (want %d), first views' pages reported %zu", hi, SLOTS, lo);

    /* reads */
    volatile uint8_t sink = 0; (void)sink;
    for (int v = 40; v < 48; v++) { sink += base[(size_t)v * VIEW + 3 * PAGE]; sink += base[(size_t)v * VIEW + 15 * PAGE]; }
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    printf("  reads only (16 pages of views 40..47, some never written in any alias): %llu pages reported as written\n", (unsigned long long)s.pages);

    /* unmap and remap */
    unmap(base, 8, &m);
    map(base, 8, SLOTS, S1_MAP_REGISTER | S1_MAP_ARM, &m);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.ret >= 0 && s.pages == 0, "views 0..7 unmapped and mapped again (register+arm): %llu pages; data kept: %s",
          (unsigned long long)s.pages, base[(size_t)(7 * 16 + 1) * PAGE + 100 + 1] == (uint8_t)(7 * 16 + 2) ? "yes" : "NO");
    base[3 * VIEW + 5 * PAGE] = 9;
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.pages == 1 && got[3 * 16 + 5], "one write to a remapped view: %llu pages", (unsigned long long)s.pages);
    unmap(base + 8 * VIEW, 8, &m);
    s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  views 8..15 unmapped (PROT_NONE anon, not registered): scan with CHECK_WPASYNC ret %lld errno %lld\n", (long long)s.ret, (long long)s.err);
    map(base + 8 * VIEW, 8, SLOTS, 0, &m);                                      /* mapped again without register */
    base[8 * VIEW] = 1; base[0] = 1;
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP, got);
    CHECK(s.ret >= 0 && got[0] && !got[8 * 16], "views 8..15 mapped again WITHOUT register, one write in view 0 and one in view 8: scan without CHECK_WPASYNC skips them: %llu pages (view 0: %d, unregistered view 8: %d)",
          (unsigned long long)s.pages, got[0], got[8 * 16]);
    base[0] = 2;
    s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  another write in view 0, then a scan WITH CHECK_WPASYNC: ret %lld errno %lld (EPERM=1)\n", (long long)s.ret, (long long)s.err);
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP, got);
    printf("  then a scan without CHECK_WPASYNC reports view 0's write: %s (no = the failed scan re-armed the pages it walked before failing, and the write is lost)\n", got[0] ? "yes" : "NO");
    unmap(base, N, &m);
    release(base, N * VIEW);
}

/* --- T2: register once over the reserved range, then map views inside it --- */
static void t2_register_then_map(void)
{
    enum { N = 16, NP = N * VIEW / PAGE };
    printf("T2 register once over a reserved range, then map views inside it\n");
    uint8_t *base = reserve(N * VIEW), got[NP];
    int64_t e = range_op(S1_REGISTER, base, N * VIEW);
    printf("  UFFDIO_REGISTER(WP) over the Wine-reserved PROT_NONE range: errno %lld\n", (long long)e);
    struct s1_scan s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  scan of the registered reserved range: ret %lld errno %lld pages %llu\n", (long long)s.ret, (long long)s.err, (unsigned long long)s.pages);
    struct s1_map m; map(base, 4, 4, 0, &m);
    s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  after mapping 4 views inside it: scan with CHECK_WPASYNC ret %lld errno %lld\n", (long long)s.ret, (long long)s.err);
    base[0] = 1; base[VIEW + PAGE] = 1;
    memset(got, 0, NP); s = scan(base, N * VIEW, S1_SCAN_WP, got);
    CHECK(s.pages == 0, "2 writes into those views: %llu pages reported (0 = the MAP_FIXED mmap dropped the registration)", (unsigned long long)s.pages);
    unmap(base, N, &m);
    release(base, N * VIEW);
}

/* --- T3: anonymous private memory (VirtualAlloc'd by Wine) --- */
static void t3_private(void)
{
    const size_t len = 64u << 20, NP = len / PAGE;
    printf("T3 private memory: 64 MiB VirtualAlloc(MEM_COMMIT) (native: anon mmap), registered + armed once\n");
    uint8_t *p = alloc_rw(len), *want = calloc(NP, 1), *got = calloc(NP, 1);
    size_t miss, extra; struct s1_scan s;
    for (size_t i = 0; i < NP; i += 2) p[i * PAGE] = 1;                          /* half populated before registering */
    int64_t e = range_op(S1_REGISTER, p, len);
    int64_t e2 = range_op(S1_ARM, p, len);
    CHECK(e == 0 && e2 == 0, "register errno %lld, arm errno %lld", (long long)e, (long long)e2);
    s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.ret >= 0 && s.pages == 0, "after arm: %llu pages", (unsigned long long)s.pages);
    for (size_t i = 0; i < NP; i += 13) want[i] = 1;                              /* populated and unpopulated ones */
    write_pages(p, want, NP, 6);
    memset(got, 0, NP); s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, got);
    compare(want, got, NP, &miss, &extra);
    CHECK(miss == 0 && extra == 0, "every 13th page written by 6 threads: %llu pages, missing %zu, extra %zu", (unsigned long long)s.pages, miss, extra);
    s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    CHECK(s.pages == 0, "second scan: %llu", (unsigned long long)s.pages);

    set_readonly(p, len / 2, 1); set_readonly(p, len / 2, 0);
    s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  after VirtualProtect(READONLY) then (READWRITE) on the first half: scan ret %lld errno %lld pages %llu\n", (long long)s.ret, (long long)s.err, (unsigned long long)s.pages);
    p[5 * PAGE] = 2;
    memset(got, 0, NP); s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, got);
    CHECK(s.pages == 1 && got[5], "one write after the protection change: %llu pages", (unsigned long long)s.pages);

    decommit_recommit(p + len / 2, len / 4);
    s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  after VirtualFree(MEM_DECOMMIT)+VirtualAlloc(MEM_COMMIT) of a quarter: scan with CHECK ret %lld errno %lld\n", (long long)s.ret, (long long)s.err);
    p[len / 2 + PAGE] = 1; p[PAGE] = 1;
    memset(got, 0, NP); s = scan(p, len, S1_SCAN_WP, got);
    printf("  a write in the recommitted part and one outside: reported %d and %d (0 = registration lost there)\n", got[NP / 2 + 1], got[1]);
    release(p, len); free(want); free(got);
}

/* --- T4: no write lost while scans run concurrently with writers --- */
struct cjob { uint8_t *base; volatile uint8_t *written; size_t npages; volatile int stop; uint64_t writes[8]; };
static void cwriter(int idx, void *ctx)
{
    struct cjob *j = ctx; uint64_t r = 0x9E3779B97F4A7C15ull * (uint64_t)(idx + 1), n = 0;
    while (!j->stop) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        size_t pg = r % j->npages;
        ((volatile uint8_t *)j->base)[pg * PAGE + (size_t)idx * 8] = (uint8_t)n;
        atomic_or8(&j->written[pg], 1);
        n++;
    }
    j->writes[idx] = n;
}
static void t4_concurrent(void)
{
    enum { N = 256, NP = N * VIEW / PAGE, W = 6 };
    printf("T4 %d writer threads vs back-to-back scans for 300 ms on %d registered views\n", W, N);
    struct s1_pool pl = { N * VIEW }; bcall(S1_POOL, &pl);
    uint8_t *base = reserve(N * VIEW), *got = calloc(NP, 1);
    struct cjob j = { base, calloc(NP, 1), NP, 0, {0} };
    struct s1_map m; map(base, N, N, S1_MAP_REGISTER | S1_MAP_ARM, &m);
    struct thr_arg a[W]; thr_t t[W];
    for (int i = 0; i < W; i++) { a[i] = (struct thr_arg){ cwriter, i, &j }; t[i] = thr_start(&a[i]); }
    double t0 = now_us(); uint64_t scans = 0, pages = 0;
    while (now_us() - t0 < 300000) { struct s1_scan s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got); scans++; pages += s.pages; if (s.ret < 0) { printf("  scan failed errno %lld\n", (long long)s.err); break; } }
    j.stop = 1;
    for (int i = 0; i < W; i++) thr_join(t[i]);
    struct s1_scan s = scan(base, N * VIEW, S1_SCAN_WP | S1_SCAN_CHECK, got); pages += s.pages;
    size_t miss, extra; compare((const uint8_t *)j.written, got, NP, &miss, &extra);
    uint64_t writes = 0; for (int i = 0; i < W; i++) writes += j.writes[i];
    CHECK(miss == 0 && extra == 0, "%llu writes, %llu scans, %llu pages reported in all; pages written but never reported %zu, reported but never written %zu",
          (unsigned long long)writes, (unsigned long long)scans, (unsigned long long)pages, miss, extra);
    unmap(base, N, &m); release(base, N * VIEW); free(got); free((void *)j.written);
}

/* --- T5: cost at many views --- */
static double us_per(uint64_t ns, uint64_t n) { return n ? (double)ns / 1e3 / (double)n : 0; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double scan_median(uint8_t *base, size_t len, int flags, int reps, uint64_t *pages)
{
    double v[16];
    for (int i = 0; i < reps; i++) { struct s1_scan s = scan(base, len, flags, NULL); v[i] = s.ns / 1e3; if (pages) *pages = s.pages; }
    qsort(v, (size_t)reps, sizeof *v, cmpd);
    return v[reps / 2];
}
static double touch_ns(uint8_t *base, const uint32_t *pages, size_t n, int off)
{
    double t0 = now_us();
    for (size_t i = 0; i < n; i++) ((volatile uint8_t *)base)[(size_t)pages[i] * PAGE + (size_t)off] = (uint8_t)i;
    return (now_us() - t0) * 1e3 / (double)n;
}
/* 2,000 views of the upper half unmapped and mapped again at full occupancy */
static void churn(uint8_t *base, uint64_t nviews, uint64_t slots, int flags)
{
    const uint64_t k = 2000, first = nviews / 2;
    uint64_t mm = 0, rg = 0, ar = 0, um = 0;
    struct s1_map m, u;
    for (uint64_t i = 0; i < k; i++) {
        uint8_t *v = base + (first + (xs() % (nviews / 2))) * VIEW;
        unmap(v, 1, &u); map(v, 1, slots, flags, &m);
        um += u.ns_mmap; mm += m.ns_mmap; rg += m.ns_reg; ar += m.ns_arm;
    }
    printf("  churn at %llu views (%s): unmap %.2f + mmap %.2f + register %.2f + arm %.2f = %.2f us per view cycle\n", (unsigned long long)nviews,
           flags == 0 ? "no uffd" : flags == S1_MAP_REGISTER ? "registered" : "registered+armed",
           us_per(um, k), us_per(mm, k), us_per(rg, k), us_per(ar, k), us_per(um + mm + rg + ar, k));
}
static void t5_cost(uint64_t nviews)
{
    const uint64_t slots = 8192;                                                 /* 512 MiB memfd, views alias i % 8192 */
    const size_t len = nviews * VIEW, NP = len / PAGE;
    printf("T5 cost: %llu views of 64 KiB (%.2f GiB of address space), pool memfd 512 MiB (view i at offset (i %% 8192) * 64 KiB)\n",
           (unsigned long long)nviews, len / 1073741824.0);
    struct s1_pool pl = { slots * VIEW }; bcall(S1_POOL, &pl);
    uint8_t *base = reserve(len);
    if (!base) { printf("  reserve failed\n"); return; }
    struct s1_map m, u;
    map(base, nviews, slots, 0, &m);
    churn(base, nviews, slots, 0);
    unmap(base, nviews, &u);
    printf("  map only:              mmap %.2f us/view; unmap %.2f us/view\n", us_per(m.ns_mmap, nviews), us_per(u.ns_mmap, nviews));
    map(base, nviews, slots, S1_MAP_REGISTER, &m);
    churn(base, nviews, slots, S1_MAP_REGISTER);
    unmap(base, nviews, &u);
    printf("  map + register:        mmap %.2f, register %.2f us/view; unmap %.2f us/view\n", us_per(m.ns_mmap, nviews), us_per(m.ns_reg, nviews), us_per(u.ns_mmap, nviews));
    map(base, nviews, slots, S1_MAP_REGISTER | S1_MAP_ARM, &m);
    printf("  map + register + arm:  mmap %.2f, register %.2f, arm %.2f us/view (total %.2f)\n", us_per(m.ns_mmap, nviews), us_per(m.ns_reg, nviews),
           us_per(m.ns_arm, nviews), us_per(m.ns_mmap + m.ns_reg + m.ns_arm, nviews));

    churn(base, nviews, slots, S1_MAP_REGISTER | S1_MAP_ARM);
    uint64_t pages;
    double t_empty = scan_median(base, len, S1_SCAN_WP | S1_SCAN_CHECK, 7, &pages);
    printf("  scan whole window, nothing written (views unpopulated): %.0f us (pages %llu)\n", t_empty, (unsigned long long)pages);
    t_empty = scan_median(base, len, S1_SCAN_WP, 7, &pages);
    printf("  same without CHECK_WPASYNC: %.0f us\n", t_empty);

    uint32_t *pg = malloc(sizeof *pg * 100000);
    for (int i = 0; i < 100; i++) pg[i] = (uint32_t)(xs() % NP);
    touch_ns(base, pg, 100, 1);
    struct s1_scan s = scan(base, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  scan after 100 random page writes: %.0f us, %llu pages\n", s.ns / 1e3, (unsigned long long)s.pages);

    for (uint64_t v = 0; v < nviews; v++) base[v * VIEW + (v % 16) * PAGE] = 1;
    s = scan(base, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  scan after 1 page written in each of the %llu views: %.0f us, %llu pages, %llu regions\n", (unsigned long long)nviews, s.ns / 1e3,
           (unsigned long long)s.pages, (unsigned long long)s.n_regions);
    uint64_t many = nviews / 10;
    for (uint64_t v = 0; v < many; v++) for (int p = 0; p < 16; p++) base[(v * 10) * VIEW + (size_t)p * PAGE + 8] = 1;
    s = scan(base, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
    printf("  scan after all 16 pages of every 10th view (%llu pages): %.0f us, %llu pages, %llu regions\n", (unsigned long long)(many * 16), s.ns / 1e3,
           (unsigned long long)s.pages, (unsigned long long)s.n_regions);
    t_empty = scan_median(base, len, S1_SCAN_WP | S1_SCAN_CHECK, 7, &pages);
    printf("  scan whole window, nothing written, %llu pages populated: %.0f us\n", (unsigned long long)(nviews + many * 15), t_empty);

    /* first write to an armed (populated) page vs the same pages once writable */
    size_t nt = 0;
    for (uint64_t v = 0; v < many && nt < 100000; v++) for (int p = 0; p < 16 && nt < 100000; p += 2) pg[nt++] = (uint32_t)((v * 10) * 16 + (uint64_t)p);
    scan(base, len, S1_SCAN_WP, NULL);                                          /* arm them */
    double armed = touch_ns(base, pg, nt, 16), plain = touch_ns(base, pg, nt, 24);
    s = scan(base, len, S1_SCAN_WP, NULL);
    printf("  first write to an armed populated page: %.0f ns; same pages again (writable): %.0f ns  [%zu pages; scan then saw %llu]\n", armed, plain, nt, (unsigned long long)s.pages);
    /* first write to never-populated pages: registered vs not */
    size_t nf = 0;
    for (uint64_t v = 1; v < nviews && nf < 20000; v += 10) pg[nf++] = (uint32_t)(v * 16 + 15 - (v % 16 == 15));
    double fresh_reg = touch_ns(base, pg, nf, 32);
    unmap(base, nviews, &u); map(base, nviews, slots, 0, &m);                    /* same pool pages, no uffd */
    double reread = touch_ns(base, pg, nf, 40);
    unmap(base, nviews, &u); map(base, nviews, slots, S1_MAP_REGISTER | S1_MAP_ARM, &m);
    double reread_reg = touch_ns(base, pg, nf, 48);
    printf("  first write after a fresh map (page in the pool, no PTE yet): registered %.0f ns, unregistered %.0f ns  (first-ever %.0f ns registered)\n",
           reread_reg, reread, fresh_reg);
    unmap(base, nviews, &u);
    release(base, len);

    /* private memory of the same size, one registered range */
    uint8_t *p = alloc_rw(len);
    if (p) {
        int64_t e = range_op(S1_REGISTER, p, len); int64_t e2 = range_op(S1_ARM, p, len);
        double te = scan_median(p, len, S1_SCAN_WP | S1_SCAN_CHECK, 7, NULL);
        for (size_t i = 0; i < 100; i++) pg[i] = (uint32_t)(xs() % NP);
        for (size_t i = 0; i < 100000; i++) p[(xs() % NP) * PAGE] = 1;
        double tp = scan_median(p, len, S1_SCAN_WP | S1_SCAN_CHECK, 7, NULL);
        touch_ns(p, pg, 100, 3);
        s = scan(p, len, S1_SCAN_WP | S1_SCAN_CHECK, NULL);
        printf("  private %.2f GiB, one range (register errno %lld, arm errno %lld): scan empty %.0f us; ~95k pages populated, none written %.0f us; 100 written %.0f us (%llu pages)\n",
               len / 1073741824.0, (long long)e, (long long)e2, te, tp, s.ns / 1e3, (unsigned long long)s.pages);
        release(p, len);
    }
    free(pg);

#ifdef _WIN32
    /* Wine's GetWriteWatch on a MEM_WRITE_WATCH region of the same size */
    {
        uint8_t *w = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!w) { printf("  MEM_WRITE_WATCH alloc failed %lu\n", GetLastError()); return; }
        int64_t busy = range_op(S1_REGISTER, w, PAGE);
        printf("  MEM_WRITE_WATCH region: our UFFDIO_REGISTER on it gives errno %lld (16 = EBUSY: Wine already tracks it with its own uffd)\n", (long long)busy);
        if (!busy) range_op(S1_UNREGISTER, w, PAGE);
        void **addrs = malloc(sizeof *addrs * 1100000); ULONG_PTR cnt; DWORD gran;
        double v[7];
        for (int i = 0; i < 7; i++) { cnt = 1100000; double t0 = now_us(); GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, len, addrs, &cnt, &gran); v[i] = now_us() - t0; }
        qsort(v, 7, sizeof *v, cmpd);
        printf("  GetWriteWatch(reset) whole region, nothing written: %.0f us\n", v[3]);
        for (int i = 0; i < 100; i++) w[(xs() % NP) * PAGE] = 1;
        cnt = 1100000; double t0 = now_us(); GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, len, addrs, &cnt, &gran);
        printf("  GetWriteWatch(reset) after 100 random writes: %.0f us, %lu pages\n", now_us() - t0, (unsigned long)cnt);
        uint32_t *q = malloc(sizeof *q * 100000); size_t nq = 0;
        for (uint64_t vv = 0; vv < nviews / 10 && nq < 100000; vv++) for (int pp = 0; pp < 16 && nq < 100000; pp += 2) q[nq++] = (uint32_t)((vv * 10) * 16 + (uint64_t)pp);
        touch_ns(w, q, nq, 0);                                                    /* populate */
        cnt = 1100000; t0 = now_us(); GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, len, addrs, &cnt, &gran);
        printf("  GetWriteWatch(reset) after %zu pages written: %.0f us, %lu pages\n", nq, now_us() - t0, (unsigned long)cnt);
        LONG f0 = faults;
        double ww = touch_ns(w, q, nq, 8), wp = touch_ns(w, q, nq, 16);
        printf("  first write to a watched (reset) page: %.0f ns; again: %.0f ns (vectored handler saw %ld)\n", ww, wp, faults - f0);
        for (int i = 0; i < 7; i++) { cnt = 1100000; t0 = now_us(); GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, len, addrs, &cnt, &gran); v[i] = now_us() - t0; }
        qsort(v, 7, sizeof *v, cmpd);
        printf("  GetWriteWatch(reset), nothing written, %zu pages populated: %.0f us\n", nq, v[3]);
        VirtualFree(w, 0, MEM_RELEASE); free(addrs); free(q);
    }
#endif
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint64_t nviews = argc > 1 ? strtoull(argv[1], NULL, 10) : 60000;
    platform_init();
    int64_t pid; bcall(S1_PID, &pid);
    struct s1_init in; bcall(S1_INIT, &in);
    printf("linux pid %lld; uffd: errno %lld, features available 0x%llx, enabled 0x%llx\n", (long long)pid, (long long)in.err,
           (unsigned long long)in.features_avail, (unsigned long long)in.features_got);
    if (in.err) return 1;
    regions = (struct s1_region *)alloc_rw(sizeof *regions * MAX_REGIONS);
    t1_views();
    t2_register_then_map();
    t3_private();
    t4_concurrent();
    t5_cost(nviews);
    printf("exceptions seen by the vectored handler / SIGSEGV: %ld; checks failed: %d\n", (long)faults, failures);
    return failures != 0;
}
