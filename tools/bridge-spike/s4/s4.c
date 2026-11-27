/* S4 spike, the measuring program (PE). Modes:
 *   s4.exe nop      no-op unix call, 1/2/4/8 threads
 *   s4.exe bridge   batched map/unmap/mprotect through the bridge, at 0 and 60,000 views
 *   s4.exe wine     Wine's path (split, MapViewOfFile3, UnmapViewOfFile2, VirtualProtect), at 0 and 60,000 views
 * Times with rdtsc, calibrated against QueryPerformanceCounter. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#include "abi.h"

#define SLOT 65536ull
#define TEST 2048u            /* slots the measured ops run over */
#define POP 60000u            /* views made before the second measurement */
#define POP_AT 4096u          /* first populated slot */
#define SLOTS (POP_AT + POP)
#define POOL_SLOTS 1024u

typedef LONG (WINAPI *CALL)(unsigned int, void *);
static CALL s4;
static double ns_per_tick;

static inline unsigned long long now(void) { _mm_lfence(); unsigned long long t = __rdtsc(); _mm_lfence(); return t; }

static void calibrate(void)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a); unsigned long long t0 = now();
    Sleep(300);
    QueryPerformanceCounter(&b); unsigned long long t1 = now();
    double secs = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
    ns_per_tick = secs * 1e9 / (double)(t1 - t0);
}

static int cmp(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}
/* median and p99 of n samples, in ns, each divided by `per` */
static void stats(unsigned long long *s, size_t n, double per, double *med, double *p99)
{
    qsort(s, n, sizeof *s, cmp);
    *med = s[n / 2] * ns_per_tick / per;
    *p99 = s[(size_t)(n * 0.99)] * ns_per_tick / per;
}

/* ---------------------------------------------------------------- nop */

#define NOP_N 200000
static volatile LONG go, ready;
static unsigned long long loop_ticks[8];

static DWORD WINAPI nop_thread(void *arg)
{
    unsigned long long *s = arg;
    int me = (int)(s[0]);
    uint64_t count = 0;
    InterlockedIncrement(&ready);
    while (!go) _mm_pause();
    /* each call timed (the timer's own cost included), then a tight loop timed whole */
    for (int i = 0; i < NOP_N; i++) {
        unsigned long long t0 = now();
        s4(S4_NOP, &count);
        s[i] = now() - t0;
    }
    unsigned long long t0 = now();
    for (int i = 0; i < NOP_N; i++) s4(S4_NOP, &count);
    loop_ticks[me] = now() - t0;
    if (count != 2 * NOP_N) printf("only %llu of %d calls reached the Unix half\n", (unsigned long long)count, 2 * NOP_N);
    return 0;
}

static void mode_nop(void)
{
    unsigned long long *e = malloc(sizeof *e * NOP_N);
    for (int i = 0; i < NOP_N; i++) { unsigned long long t0 = now(); e[i] = now() - t0; }
    double tm, tp; stats(e, NOP_N, 1, &tm, &tp); free(e);
    printf("timer alone (rdtsc pair with lfences): median %.1f ns, p99 %.1f ns\n", tm, tp);
    int counts[] = { 1, 2, 4, 8 };
    for (int c = 0; c < 4; c++) {
        int n = counts[c];
        unsigned long long *s = malloc(sizeof *s * NOP_N * n);
        HANDLE h[8];
        go = 0; ready = 0;
        for (int t = 0; t < n; t++) { s[(size_t)t * NOP_N] = t; h[t] = CreateThread(NULL, 0, nop_thread, s + (size_t)t * NOP_N, 0, NULL); }
        while (ready < n) _mm_pause();
        go = 1;
        WaitForMultipleObjects(n, h, TRUE, INFINITE);
        for (int t = 0; t < n; t++) CloseHandle(h[t]);
        double med, p99, worst = 0, sum = 0;
        stats(s, (size_t)NOP_N * n, 1, &med, &p99);
        for (int t = 0; t < n; t++) { double v = loop_ticks[t] * ns_per_tick / NOP_N; sum += v; if (v > worst) worst = v; }
        printf("nop unix call, %d thread(s): tight loop %.1f ns per call (mean over threads; slowest thread %.1f) | timed one by one, timer included: median %.0f p99 %.0f ns\n",
               n, sum / n, worst, med, p99);
        free(s);
    }
}

/* ------------------------------------------------------------- bridge */

static struct s4_pool pool;
static char *win;

static struct s4_op op_at(unsigned slot, unsigned i, unsigned kind, unsigned prot)
{
    /* every other pool slot: neighbouring views never merge into one Linux mapping */
    struct s4_op o = { (uint64_t)(win + slot * SLOT), SLOT, ((i * 2u) % POOL_SLOTS) * SLOT, prot, kind };
    return o;
}

static void do_batch(const struct s4_op *ops, unsigned n, int32_t *results)
{
    struct s4_batch b = { (uint64_t)(uintptr_t)ops, (uint64_t)(uintptr_t)results, n, 0, pool.id, 0 };
    LONG st = s4(S4_BATCH, &b);
    if (st || b.done != n || b.error) { printf("batch failed: status %ld done %u/%u errno %d\n", st, b.done, n, b.error); exit(1); }
}

#define ROUNDS 20
static struct s4_op map_ops[TEST], unmap_ops[TEST], ro_ops[TEST], rw_ops[TEST];
static int32_t results[TEST];

/* maps, protects (read-only, then read-write) and unmaps the TEST slots in calls of `per` ops,
 * ROUNDS times */
static void bridge_round(unsigned per, int with_results, const char *state)
{
    size_t n = 0, m = 0;
    unsigned long long tot_map = 0, tot_unmap = 0, tot_prot = 0;
    static unsigned long long smap[ROUNDS * TEST], sunmap[ROUNDS * TEST], sprot[ROUNDS * TEST * 2];
    size_t np = 0;
    for (int r = 0; r < ROUNDS; r++) {
        for (unsigned i = 0; i < TEST; i += per) {
            unsigned long long t0 = now(); do_batch(map_ops + i, per, with_results ? results : NULL);
            unsigned long long d = now() - t0; smap[n++] = d; tot_map += d;
        }
        for (unsigned i = 0; i < TEST; i += per) {
            unsigned long long t0 = now(); do_batch(ro_ops + i, per, with_results ? results : NULL);
            unsigned long long d = now() - t0; sprot[np++] = d; tot_prot += d;
        }
        for (unsigned i = 0; i < TEST; i += per) {
            unsigned long long t0 = now(); do_batch(rw_ops + i, per, with_results ? results : NULL);
            unsigned long long d = now() - t0; sprot[np++] = d; tot_prot += d;
        }
        for (unsigned i = 0; i < TEST; i += per) {
            unsigned long long t0 = now(); do_batch(unmap_ops + i, per, with_results ? results : NULL);
            unsigned long long d = now() - t0; sunmap[m++] = d; tot_unmap += d;
        }
    }
    double ops = (double)ROUNDS * TEST;
    double mm, mp, um, up, pm, pp;
    stats(smap, n, 1, &mm, &mp); stats(sunmap, m, 1, &um, &up); stats(sprot, np, 1, &pm, &pp);
    printf("bridge %-9s batch %4u%s: per op map %.0f unmap %.0f (map+unmap %.0f) mprotect %.0f ns | per call median map %.0f p99 %.0f, unmap %.0f p99 %.0f, mprotect %.0f p99 %.0f ns\n",
           state, per, with_results ? " +results" : "         ",
           tot_map * ns_per_tick / ops, tot_unmap * ns_per_tick / ops, (tot_map + tot_unmap) * ns_per_tick / ops,
           tot_prot * ns_per_tick / (2 * ops), mm, mp, um, up, pm, pp);
}

/* a view replaced in place: MAP_FIXED over a mapped view, one op instead of unmap + map */
static void bridge_replace(const char *state)
{
    static struct s4_op again[TEST];
    for (unsigned i = 0; i < TEST; i++) { again[i] = map_ops[i]; again[i].offset = ((i * 2u + 1) % POOL_SLOTS) * SLOT; }
    unsigned sizes[] = { 1, 128, 2048 };
    for (int k = 0; k < 3; k++) {
        unsigned per = sizes[k];
        do_batch(map_ops, TEST, NULL);
        unsigned long long tot = 0;
        for (int r = 0; r < ROUNDS; r++) {
            const struct s4_op *ops = r & 1 ? map_ops : again;
            unsigned long long t0 = now();
            for (unsigned i = 0; i < TEST; i += per) do_batch(ops + i, per, NULL);
            tot += now() - t0;
        }
        do_batch(unmap_ops, TEST, NULL);
        printf("bridge %-9s batch %4u: replace a view in place (MAP_FIXED over it) %.0f ns per op\n",
               state, per, tot * ns_per_tick / ((double)ROUNDS * TEST));
    }
}

static void bridge_suite(const char *state)
{
    bridge_replace(state);
    unsigned sizes[] = { 1, 8, 32, 128, 512, 2048 };
    for (int k = 0; k < 6; k++) bridge_round(sizes[k], 0, state);
    for (int k = 0; k < 6; k++) if (sizes[k] >= 32) bridge_round(sizes[k], 1, state);
}

/* threads mapping disjoint slots at once, batches of 32 */
struct mt_arg { unsigned first, count; unsigned long long ticks; };
static DWORD WINAPI mt_thread(void *p)
{
    struct mt_arg *a = p;
    struct s4_op *mo = malloc(sizeof *mo * a->count), *uo = malloc(sizeof *uo * a->count);
    for (unsigned i = 0; i < a->count; i++) { mo[i] = op_at(a->first + i, i, S4_MAP, S4_R | S4_W); uo[i] = op_at(a->first + i, i, S4_UNMAP, 0); }
    InterlockedIncrement(&ready);
    while (!go) _mm_pause();
    unsigned long long t0 = now();
    for (int r = 0; r < ROUNDS; r++) {
        for (unsigned i = 0; i < a->count; i += 32) do_batch(mo + i, 32, NULL);
        for (unsigned i = 0; i < a->count; i += 32) do_batch(uo + i, 32, NULL);
    }
    a->ticks = now() - t0;
    free(mo); free(uo);
    return 0;
}
static void bridge_threads(const char *state)
{
    int counts[] = { 1, 2, 4, 8 };
    for (int c = 0; c < 4; c++) {
        int n = counts[c];
        struct mt_arg a[8]; HANDLE h[8];
        go = 0; ready = 0;
        for (int t = 0; t < n; t++) { a[t].first = t * (TEST / 8); a[t].count = TEST / 8; h[t] = CreateThread(NULL, 0, mt_thread, &a[t], 0, NULL); }
        while (ready < n) _mm_pause();
        unsigned long long w0 = now(); go = 1;
        WaitForMultipleObjects(n, h, TRUE, INFINITE);
        double wall = (now() - w0) * ns_per_tick;
        for (int t = 0; t < n; t++) CloseHandle(h[t]);
        double pairs = (double)ROUNDS * (TEST / 8) * n;
        printf("bridge %-9s %d thread(s), batch 32: %.0f ns per map+unmap pair (wall / all pairs), %.2f M pairs/s\n",
               state, n, wall / pairs, pairs / wall * 1e3);
    }
}

static void mode_bridge(void)
{
    win = VirtualAlloc(NULL, SLOTS * SLOT, MEM_RESERVE, PAGE_NOACCESS);
    if (!win) { printf("reserve failed %lu\n", GetLastError()); exit(1); }
    pool.bytes = POOL_SLOTS * SLOT; pool.window = (uint64_t)(uintptr_t)win; pool.window_len = SLOTS * SLOT;
    if (s4(S4_POOL_CREATE, &pool)) { printf("pool create failed\n"); exit(1); }
    for (unsigned i = 0; i < TEST; i++) {
        map_ops[i] = op_at(i, i, S4_MAP, S4_R | S4_W);
        unmap_ops[i] = op_at(i, i, S4_UNMAP, 0);
        ro_ops[i] = op_at(i, i, S4_PROTECT, S4_R);
        rw_ops[i] = op_at(i, i, S4_PROTECT, S4_R | S4_W);
    }
    /* correctness: aliases see each other's writes, the window stays Wine's reserve */
    do_batch(map_ops, 2, NULL);
    struct s4_op alias = op_at(2, 0, S4_MAP, S4_R | S4_W); do_batch(&alias, 1, NULL);
    win[5] = 42;
    MEMORY_BASIC_INFORMATION mbi; VirtualQuery(win, &mbi, sizeof mbi);
    printf("check: alias sees write %s; Wine says state %#lx protect %#lx size %zu MiB\n",
           win[2 * SLOT + 5] == 42 ? "yes" : "NO", mbi.State, mbi.Protect, mbi.RegionSize >> 20);
    do_batch(unmap_ops, 3, NULL);

    bridge_suite("0 views");
    bridge_threads("0 views");
    struct s4_op *pop = malloc(sizeof *pop * POP);
    for (unsigned i = 0; i < POP; i++) pop[i] = op_at(POP_AT + i, i, S4_MAP, S4_R | S4_W);
    unsigned long long t0 = now();
    for (unsigned i = 0; i < POP; i += 2000) do_batch(pop + i, 2000, NULL);
    printf("populated %u views in %.1f ms\n", POP, (now() - t0) * ns_per_tick / 1e6);
    bridge_suite("60k views");
    bridge_threads("60k views");
}

/* --------------------------------------------------------------- wine */

typedef PVOID (WINAPI *VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *MV3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *UV2)(HANDLE, PVOID, ULONG);
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

static void mode_wine(void)
{
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    VA2 va2 = (VA2)GetProcAddress(kb, "VirtualAlloc2");
    MV3 mv3 = (MV3)GetProcAddress(kb, "MapViewOfFile3");
    UV2 uv2 = (UV2)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!va2 || !mv3 || !uv2) { printf("placeholder API missing\n"); exit(1); }
    HANDLE proc = GetCurrentProcess();
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, (DWORD)(POOL_SLOTS * SLOT), NULL);
    /* the window, plus a fresh area per split measurement */
    char *w = va2(proc, NULL, (SLOTS + 2 * TEST) * SLOT, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!sec || !w) { printf("section/reserve failed %lu\n", GetLastError()); exit(1); }
    static unsigned long long sm[TEST * 5], su[TEST * 5], ss[TEST], sp[TEST * 10];
    const char *states[] = { "0 views", "60k views" };
    for (int st = 0; st < 2; st++) {
        /* the split: carve TEST own placeholders out of a large one (like xmem's split) */
        char *area = w + (SLOTS + st * TEST) * SLOT;
        unsigned long long tot = 0;
        for (unsigned i = 0; i < TEST; i++) {
            unsigned long long t0 = now();
            BOOL ok = VirtualFree(area + i * SLOT, SLOT, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            ss[i] = now() - t0; tot += ss[i];
            if (!ok && i + 1 < TEST) { printf("split failed %lu\n", GetLastError()); exit(1); }
        }
        double med, p99;
        stats(ss, TEST, 1, &med, &p99);
        printf("wine %-9s VirtualFree split: mean %.0f median %.0f p99 %.0f ns\n", states[st], tot * ns_per_tick / TEST, med, p99);
        int rounds = st ? 3 : 5;
        size_t n = 0, np = 0; unsigned long long tm = 0, tu = 0, tp = 0;
        for (int r = 0; r < rounds; r++) {
            for (unsigned i = 0; i < TEST; i++) {
                unsigned long long t0 = now();
                PVOID v = mv3(sec, proc, area + i * SLOT, ((i * 2u) % POOL_SLOTS) * SLOT, SLOT, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
                unsigned long long d = now() - t0; sm[n] = d; tm += d;
                if (v != area + i * SLOT) { printf("MapViewOfFile3 failed %lu\n", GetLastError()); exit(1); }
                n++;
            }
            for (int pr = 0; pr < 2; pr++)
                for (unsigned i = 0; i < TEST; i++) {
                    DWORD old;
                    unsigned long long t0 = now();
                    BOOL ok = VirtualProtect(area + i * SLOT, SLOT, pr ? PAGE_READWRITE : PAGE_READONLY, &old);
                    unsigned long long d = now() - t0; sp[np++] = d; tp += d;
                    if (!ok) { printf("VirtualProtect failed %lu\n", GetLastError()); exit(1); }
                }
            for (unsigned i = 0; i < TEST; i++) {
                unsigned long long t0 = now();
                BOOL ok = uv2(proc, area + i * SLOT, MEM_PRESERVE_PLACEHOLDER);
                unsigned long long d = now() - t0; su[n - TEST + i] = d; tu += d;
                if (!ok) { printf("UnmapViewOfFile2 failed %lu\n", GetLastError()); exit(1); }
            }
        }
        double mm, mp, um, up, pm, pp;
        stats(sm, n, 1, &mm, &mp); stats(su, n, 1, &um, &up); stats(sp, np, 1, &pm, &pp);
        printf("wine %-9s per op mean: map %.0f unmap %.0f (map+unmap %.0f) VirtualProtect %.0f ns | median/p99 map %.0f/%.0f unmap %.0f/%.0f protect %.0f/%.0f ns\n",
               states[st], tm * ns_per_tick / n, tu * ns_per_tick / n, (tm + tu) * ns_per_tick / n, tp * ns_per_tick / np,
               mm, mp, um, up, pm, pp);
        if (st == 0) {
            unsigned long long t0 = now();
            for (unsigned i = 0; i < POP; i++) {
                char *at = w + (POP_AT + i) * SLOT;
                VirtualFree(at, SLOT, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
                if (mv3(sec, proc, at, ((i * 2u) % POOL_SLOTS) * SLOT, SLOT, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != at) {
                    printf("populate failed at %u: %lu\n", i, GetLastError()); exit(1);
                }
            }
            printf("populated %u views through Wine in %.1f s\n", POP, (now() - t0) * ns_per_tick / 1e9);
        }
    }
}

int main(int argc, char **argv)
{
    HMODULE m = LoadLibraryA("s4.dll");
    if (!m || !(s4 = (CALL)GetProcAddress(m, "s4_call"))) { printf("s4.dll not loaded: %lu\n", GetLastError()); return 1; }
    if (s4(S4_NOP, NULL)) { printf("no Unix half\n"); return 1; }
    calibrate();
    printf("tsc: %.4f ns per tick\n", ns_per_tick);
    const char *mode = argc > 1 ? argv[1] : "nop";
    if (!strcmp(mode, "nop")) mode_nop();
    else if (!strcmp(mode, "bridge")) mode_bridge();
    else if (!strcmp(mode, "wine")) mode_wine();
    else { printf("modes: nop bridge wine\n"); return 1; }
    return 0;
}
