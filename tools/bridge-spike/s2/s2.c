/* S2 spike: how Proton's Wine behaves on pages a builtin's Unix half maps, unmaps and protects
 * inside a window Wine holds reserved (docs/memory-bridge-plan.md, §5 S2, results in §10). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define KB 1024ull
#define VIEW (64 * KB)
#define WINDOW (16 * 1024 * KB)
#define R 1
#define W 2
#define X 4

typedef int (*POOL)(UINT64);
typedef int (*MAP)(void *, UINT64, UINT64, int);
typedef int (*UNMAP)(void *, UINT64);
typedef int (*PROTECT)(void *, UINT64, int);
typedef void (*MAPS)(void *, char *, int);
typedef void (*SLEEPU)(DWORD);
typedef int (*READY)(void);
typedef INT64 (*READV)(void *, const void *, UINT64, int *);
static POOL b_pool; static MAP b_map; static UNMAP b_unmap; static PROTECT b_protect;
static MAPS b_maps; static SLEEPU b_sleep; static READY b_ready; static READV b_readv;

static unsigned char *win;          /* the reserved window */
static int fails;

static void maps(const char *what, void *at)
{
    char line[512];
    b_maps(at, line, sizeof line);
    printf("    maps %-22s %p: %s\n", what, at, line);
}
static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

/* ---- the vectored handler -------------------------------------------------------------- */
enum { M_RECORD, M_FIX_WRITE, M_FIX_READ, M_SAFECOPY };
static volatile LONG mode = M_RECORD;
static volatile LONG hits;
static volatile DWORD last_code, last_tid;
static volatile ULONG_PTR last_info0, last_info1, last_rip;
static volatile ULONG last_n;

extern char s2_copy_at[], s2_copy_after[], s2_copy_failed[];
DWORD s2_copy(void *dst, const void *src, SIZE_T n);
__asm__(
    ".globl s2_copy\n.globl s2_copy_at\n.globl s2_copy_after\n.globl s2_copy_failed\n"
    "s2_copy:\n"
    "  mov %rsi, %r10\n  mov %rdi, %r11\n  mov %rcx, %rdi\n  mov %rdx, %rsi\n  mov %r8, %rcx\n"
    "s2_copy_at:\n  rep movsb\n"
    "s2_copy_after:\n  mov $1, %eax\n  mov %r10, %rsi\n  mov %r11, %rdi\n  ret\n"
    "s2_copy_failed:\n  xor %eax, %eax\n  mov %r10, %rsi\n  mov %r11, %rdi\n  ret\n");

static LONG WINAPI handler(EXCEPTION_POINTERS *p)
{
    EXCEPTION_RECORD *r = p->ExceptionRecord;
    InterlockedIncrement(&hits);
    last_code = r->ExceptionCode;
    last_n = r->NumberParameters;
    last_info0 = r->NumberParameters > 0 ? r->ExceptionInformation[0] : ~0ull;
    last_info1 = r->NumberParameters > 1 ? r->ExceptionInformation[1] : ~0ull;
    last_rip = p->ContextRecord->Rip;
    last_tid = GetCurrentThreadId();
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    ULONG_PTR at = r->ExceptionInformation[1];
    if (at < (ULONG_PTR)win || at >= (ULONG_PTR)win + WINDOW) return EXCEPTION_CONTINUE_SEARCH;
    void *page = (void *)(at & ~(VIEW - 1));
    switch (mode) {
    case M_FIX_WRITE:
        if (r->ExceptionInformation[0] == 1 && !b_protect(page, VIEW, R | W)) return EXCEPTION_CONTINUE_EXECUTION;
        break;
    case M_FIX_READ:
        if (r->ExceptionInformation[0] == 0 && !b_map(page, VIEW, (ULONG_PTR)page - (ULONG_PTR)win, R | W))
            return EXCEPTION_CONTINUE_EXECUTION;
        break;
    case M_SAFECOPY:
        if (p->ContextRecord->Rip >= (ULONG_PTR)s2_copy_at && p->ContextRecord->Rip < (ULONG_PTR)s2_copy_after) {
            if (r->ExceptionInformation[0] == 1 && !b_protect(page, VIEW, R | W)) return EXCEPTION_CONTINUE_EXECUTION;
            p->ContextRecord->Rip = (ULONG_PTR)s2_copy_failed;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void reset(LONG m) { hits = 0; last_code = 0; last_info0 = last_info1 = last_rip = 0; mode = m; }
static void show_hit(void)
{
    printf("    handler: hits=%ld code=%08lx n=%lu info0=%llu info1=%p rip=%p tid=%lu\n", hits, last_code,
           last_n, (unsigned long long)last_info0, (void *)last_info1, (void *)last_rip, last_tid);
}

/* ---- 1: faults ------------------------------------------------------------------------- */
struct fault_job { unsigned char *at; unsigned char value; int read; unsigned char got; };
static DWORD WINAPI fault_thread(void *p)
{
    struct fault_job *j = p;
    if (j->read) j->got = *(volatile unsigned char *)j->at;
    else *(volatile unsigned char *)j->at = j->value;
    return 0;
}
static void run_on_thread(struct fault_job *j)
{
    HANDLE t = CreateThread(NULL, 0, fault_thread, j, 0, NULL);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
}

static void test_faults(void)
{
    printf("\n== 1. faults on bridge pages\n");
    unsigned char *a = win, *b = win + VIEW, *c = win + 2 * VIEW, *d = win + 3 * VIEW;
    /* a: view RW, then read-only; write fault, main thread */
    b_map(a, VIEW, 0, R | W);
    a[100] = 0x11;
    b_protect(a, VIEW, R);
    reset(M_FIX_WRITE);
    a[100] = 0x22;
    show_hit();
    check(hits == 1 && last_code == EXCEPTION_ACCESS_VIOLATION && last_info0 == 1 && last_info1 == (ULONG_PTR)(a + 100),
          "write to read-only bridge page: one AV, write flag, exact address");
    check(a[100] == 0x22, "after CONTINUE_EXECUTION the write landed");
    /* b: unmapped (PROT_NONE anon); read fault maps a view */
    b_map(b, VIEW, VIEW, R | W);
    b[5] = 0x33;
    b_unmap(b, VIEW);
    maps("unmapped b", b);
    reset(M_FIX_READ);
    unsigned char v = *(volatile unsigned char *)(b + 5);
    show_hit();
    check(hits == 1 && last_info0 == 0 && last_info1 == (ULONG_PTR)(b + 5), "read of unmapped bridge page: one AV, read flag, exact address");
    check(v == 0x33, "after the handler mapped the view, the read sees the pool's byte");
    /* c: write fault from a second thread */
    b_map(c, VIEW, 2 * VIEW, R);
    reset(M_FIX_WRITE);
    struct fault_job j = { c + 9, 0x44, 0, 0 };
    run_on_thread(&j);
    show_hit();
    check(hits == 1 && last_info0 == 1 && last_info1 == (ULONG_PTR)(c + 9) && last_tid != GetCurrentThreadId(),
          "write fault from a second thread reaches the handler on that thread");
    check(c[9] == 0x44, "and the write landed");
    /* d: read fault from a second thread on a never-touched window page (Wine's own PROT_NONE reserve) */
    reset(M_FIX_READ);
    struct fault_job k = { d + 1, 0, 1, 0xff };
    run_on_thread(&k);
    show_hit();
    check(hits == 1 && last_info0 == 0 && k.got == 0, "read fault on a never-mapped window page, second thread: AV, mapped, reads 0");
    /* several threads faulting at once on their own read-only pages */
    unsigned char *base = win + 8 * VIEW;
    b_map(base, 8 * VIEW, 8 * VIEW, R);
    reset(M_FIX_WRITE);
    HANDLE t[8];
    struct fault_job jobs[8];
    for (int i = 0; i < 8; i++) {
        jobs[i] = (struct fault_job){ base + i * VIEW + 3, (unsigned char)(0x60 + i), 0, 0 };
        t[i] = CreateThread(NULL, 0, fault_thread, &jobs[i], 0, NULL);
    }
    WaitForMultipleObjects(8, t, TRUE, 10000);
    int good = 1;
    for (int i = 0; i < 8; i++) { CloseHandle(t[i]); good &= base[i * VIEW + 3] == 0x60 + i; }
    printf("    8 threads: hits=%ld\n", hits);
    check(good && hits == 8, "8 threads faulting at once: 8 AVs, all writes landed");
    /* code running from the window faults on another window page (RIP inside the window) */
    unsigned char *code = win + 4 * VIEW, *target = win + 5 * VIEW;
    b_map(code, VIEW, 4 * VIEW, R | W);
    static const unsigned char body[] = { 0xC6, 0x01, 0x5A, 0xC3 };   /* mov byte [rcx], 0x5a; ret */
    memcpy(code, body, sizeof body);
    b_protect(code, VIEW, R | X);
    b_map(target, VIEW, 5 * VIEW, R);
    reset(M_FIX_WRITE);
    ((void (*)(void *))code)(target + 17);
    show_hit();
    check(hits == 1 && last_rip == (ULONG_PTR)code && target[17] == 0x5a, "a fault with RIP inside the window (code mapped by the bridge): handled the same");
    /* a fault nobody fixes: the handler declines, IsBadReadPtr's own frame catches it */
    b_unmap(b, VIEW);
    reset(M_RECORD);
    BOOL bad = IsBadReadPtr(b, 16);
    show_hit();
    printf("    IsBadReadPtr(unmapped bridge page) = %d\n", bad);
    check(bad && hits == 1, "IsBadReadPtr: probes for real (our handler sees one AV, declines), says bad");
    reset(M_RECORD);
    bad = IsBadReadPtr(a, 16);
    check(!bad && hits == 0, "IsBadReadPtr on a mapped bridge page: good, no fault");
    reset(M_RECORD);
    bad = IsBadWritePtr(c + 64 * 0 + 0, 1);   /* c is RW now (the handler opened it) */
    b_protect(c, VIEW, R);
    BOOL bad2 = IsBadWritePtr(c, 1);
    printf("    IsBadWritePtr(RW)=%d IsBadWritePtr(R)=%d hits=%ld\n", bad, bad2, hits);
    check(!bad && bad2, "IsBadWritePtr follows the real protection");
    check(c[9] == 0x44, "IsBadWritePtr's probe write did not change the byte (it writes back what it read)");
    reset(M_RECORD);
}

/* ---- 1b: safecopy's pattern ------------------------------------------------------------- */
static void test_safecopy(void)
{
    printf("\n== 1b. safecopy's rep movsb + vectored handler on bridge pages\n");
    unsigned char *s = win + 16 * VIEW;
    static unsigned char buf[3 * VIEW];
    b_map(s, VIEW, 16 * VIEW, R | W);
    memset(s, 0x7e, VIEW);
    b_unmap(s + VIEW, VIEW);
    reset(M_SAFECOPY);
    DWORD ok = s2_copy(buf, s, VIEW);
    check(ok && hits == 0 && buf[VIEW - 1] == 0x7e, "copy out of a mapped bridge view: done, no fault");
    reset(M_SAFECOPY);
    ok = s2_copy(buf, s, 2 * VIEW);
    show_hit();
    check(!ok && hits == 1 && last_info1 == (ULONG_PTR)(s + VIEW), "copy running into an unmapped bridge page: AV at the first missing byte, failing exit taken");
    b_map(s + VIEW, VIEW, 17 * VIEW, R);
    memset(buf, 0x3c, VIEW);
    reset(M_SAFECOPY);
    ok = s2_copy(s + VIEW, buf, VIEW);
    show_hit();
    check(ok && hits == 1 && last_info0 == 1 && s[VIEW + 500] == 0x3c, "copy into a read-only bridge page: write AV, handler opens it (bridge mprotect), copy completes");
    reset(M_RECORD);
}

/* ---- 2: queries ------------------------------------------------------------------------ */
typedef LONG (WINAPI *NTQVM)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
static void query(const char *what, void *at)
{
    MEMORY_BASIC_INFORMATION m;
    SIZE_T n = VirtualQuery(at, &m, sizeof m);
    printf("    VirtualQuery %-18s %p: n=%llu base=%p alloc=%p size=%llx state=%lx protect=%lx allocprot=%lx type=%lx\n", what, at,
           (unsigned long long)n, m.BaseAddress, m.AllocationBase, (unsigned long long)m.RegionSize, m.State, m.Protect,
           m.AllocationProtect, m.Type);
    struct { PVOID va; ULONG_PTR attr; } ws = { at, 0 };
    NTQVM q = (NTQVM)GetProcAddress(GetModuleHandleA("ntdll"), "NtQueryVirtualMemory");
    LONG st = q(GetCurrentProcess(), NULL, 4 /* MemoryWorkingSetExInformation */, &ws, sizeof ws, NULL);
    printf("    WorkingSetEx %-18s %p: status=%08lx attr=%llx (valid=%llu)\n", what, at, st, (unsigned long long)ws.attr,
           (unsigned long long)(ws.attr & 1));
}
static void test_queries(void)
{
    printf("\n== 2. VirtualQuery / NtQueryVirtualMemory\n");
    MEMORY_BASIC_INFORMATION m;
    b_protect(win, VIEW, R | W);
    query("RW view", win);
    query("R view", win + 2 * VIEW);
    query("unmapped", win + VIEW);
    query("never mapped", win + 100 * VIEW);
    VirtualQuery(win, &m, sizeof m);
    check(m.State == MEM_RESERVE && m.Protect == 0 && m.AllocationProtect == PAGE_NOACCESS && m.RegionSize == WINDOW && m.BaseAddress == win,
          "VirtualQuery on a mapped RW bridge view: MEM_RESERVE, region = whole window (Wine's view, not ours)");
}

/* ---- 3: does Wine touch the pages ----------------------------------------------------- */
static void rpm(const char *what, void *src)
{
    unsigned char out[64];
    memset(out, 0xEE, sizeof out);
    SIZE_T got = 0;
    reset(M_RECORD);
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), src, out, sizeof out, &got);
    DWORD e = GetLastError();
    printf("    ReadProcessMemory(src %-14s %p) = %d err=%lu got=%llu first=%02x handler_hits=%ld\n", what, src, ok, ok ? 0 : e,
           (unsigned long long)got, out[0], hits);
}
static void wpm(const char *what, void *dst)
{
    unsigned char in[64];
    memset(in, 0xA5, sizeof in);
    SIZE_T put = 0;
    reset(M_RECORD);
    BOOL ok = WriteProcessMemory(GetCurrentProcess(), dst, in, sizeof in, &put);
    DWORD e = GetLastError();
    printf("    WriteProcessMemory(dst %-13s %p) = %d err=%lu put=%llu handler_hits=%ld\n", what, dst, ok, ok ? 0 : e,
           (unsigned long long)put, hits);
}
struct mc { unsigned char *at; int i; int ok; };
static DWORD WINAPI memcpy_thread(void *p)
{
    struct mc *m = p;
    unsigned char local[VIEW];
    for (int round = 0; round < 200; round++) {
        memset(local, m->i + round, VIEW);
        memcpy(m->at, local, VIEW);
        memset(local, 0, VIEW);
        memcpy(local, m->at, VIEW);
        if (local[VIEW - 1] != (unsigned char)(m->i + round)) { m->ok = 0; return 0; }
    }
    m->ok = 1;
    return 0;
}
static void test_touch(void)
{
    printf("\n== 3. does Wine touch bridge pages\n");
    unsigned char *rw = win + 20 * VIEW, *ro = win + 21 * VIEW, *none = win + 22 * VIEW;
    b_map(rw, VIEW, 20 * VIEW, R | W);
    memset(rw, 0x5c, VIEW);
    b_map(ro, VIEW, 21 * VIEW, R | W);
    memset(ro, 0x6d, VIEW);
    b_protect(ro, VIEW, R);
    b_map(none, VIEW, 22 * VIEW, R | W);
    memset(none, 0x7f, VIEW);
    b_unmap(none, VIEW);
    /* controls: Wine-managed memory */
    unsigned char *c_rw = VirtualAlloc(NULL, VIEW, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char *c_na = VirtualAlloc(NULL, VIEW, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    unsigned char *c_res = VirtualAlloc(NULL, VIEW, MEM_RESERVE, PAGE_NOACCESS);
    memset(c_rw, 0x12, VIEW);
    rpm("bridge RW", rw);
    rpm("bridge R", ro);
    rpm("bridge unmapped", none);
    rpm("wine RW", c_rw);
    rpm("wine NOACCESS", c_na);
    rpm("wine reserved", c_res);
    wpm("bridge RW", rw);
    printf("      rw[0] now %02x\n", rw[0]);
    wpm("bridge R", ro);
    printf("      ro[0] now %02x\n", ro[0]);
    wpm("bridge unmapped", none);
    wpm("wine RW", c_rw);
    wpm("wine NOACCESS", c_na);
    {   /* the ntdll calls directly (host.rs writes title memory with NtWriteVirtualMemory) */
        typedef LONG (WINAPI *NTRW)(HANDLE, void *, void *, SIZE_T, SIZE_T *);
        HMODULE nt = GetModuleHandleA("ntdll");
        NTRW ntr = (NTRW)GetProcAddress(nt, "NtReadVirtualMemory"), ntw = (NTRW)GetProcAddress(nt, "NtWriteVirtualMemory");
        struct { const char *what; unsigned char *at; } cs[] = { { "bridge RW", rw }, { "bridge R", ro }, { "bridge unmapped", none } };
        for (int i = 0; i < 3; i++) {
            unsigned char buf[64];
            SIZE_T done = 0;
            memset(buf, 0xB7, sizeof buf);
            LONG st = ntw(GetCurrentProcess(), cs[i].at + 256, buf, 64, &done);
            int landed = i < 2 && cs[i].at[256] == 0xB7;
            printf("    NtWriteVirtualMemory(dst %-15s) = %08lx done=%llu landed=%d\n", cs[i].what, st, (unsigned long long)done, landed);
            done = 0;
            st = ntr(GetCurrentProcess(), cs[i].at, buf, 64, &done);
            printf("    NtReadVirtualMemory(src %-16s) = %08lx done=%llu first=%02x\n", cs[i].what, st, (unsigned long long)done, buf[0]);
        }
    }
    {   /* our own buffer for ReadProcessMemory inside the window */
        SIZE_T got = 0;
        BOOL ok = ReadProcessMemory(GetCurrentProcess(), c_rw, rw + 128, 64, &got);
        printf("    ReadProcessMemory(dst bridge RW) = %d err=%lu got=%llu rw[128]=%02x\n", ok, ok ? 0 : GetLastError(),
               (unsigned long long)got, rw[128]);
    }
    {   /* process_vm_readv from the Unix half: what a bridge copy path would do */
        unsigned char out[64]; int err;
        memset(out, 0xEE, 64);
        INT64 g = b_readv(out, rw, 64, &err);
        printf("    process_vm_readv(bridge RW) = %lld err=%d first=%02x\n", g, err, out[0]);
        g = b_readv(out, none, 64, &err);
        printf("    process_vm_readv(bridge unmapped) = %lld err=%d\n", g, err);
        g = b_readv(out, ro, 64, &err);
        printf("    process_vm_readv(bridge R) = %lld err=%d first=%02x\n", g, err, out[0]);
    }
    /* files */
    char path[MAX_PATH], dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, "s2", 0, path);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    DWORD n = 0;
    reset(M_RECORD);
    BOOL ok = WriteFile(f, rw, VIEW, &n, NULL);
    printf("    WriteFile(from bridge RW) = %d err=%lu n=%lu hits=%ld\n", ok, ok ? 0 : GetLastError(), n, hits);
    ok = WriteFile(f, ro, VIEW, &n, NULL);
    printf("    WriteFile(from bridge R) = %d err=%lu n=%lu hits=%ld\n", ok, ok ? 0 : GetLastError(), n, hits);
    n = 0;
    ok = WriteFile(f, none, VIEW, &n, NULL);
    printf("    WriteFile(from bridge unmapped) = %d err=%lu n=%lu hits=%ld\n", ok, ok ? 0 : GetLastError(), n, hits);
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    unsigned char *rw2 = win + 23 * VIEW;
    b_map(rw2, VIEW, 23 * VIEW, R | W);
    ok = ReadFile(f, rw2, VIEW, &n, NULL);
    printf("    ReadFile(into bridge RW) = %d err=%lu n=%lu byte=%02x hits=%ld\n", ok, ok ? 0 : GetLastError(), n, rw2[VIEW - 1], hits);
    check(ok && n == VIEW && rw2[VIEW - 1] == 0x5c, "WriteFile from / ReadFile into mapped bridge views: work, data right");
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    n = 0;
    reset(M_RECORD);
    ok = ReadFile(f, ro, VIEW, &n, NULL);
    printf("    ReadFile(into bridge R) = %d err=%lu n=%lu ro[0]=%02x hits=%ld\n", ok, ok ? 0 : GetLastError(), n, ro[0], hits);
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    n = 0;
    ok = ReadFile(f, none, VIEW, &n, NULL);
    printf("    ReadFile(into bridge unmapped) = %d err=%lu n=%lu hits=%ld\n", ok, ok ? 0 : GetLastError(), n, hits);
    maps("unmapped after ReadFile", none);
    CloseHandle(f);
    /* memcpy from several threads */
    HANDLE t[6];
    struct mc m[6];
    b_map(win + 24 * VIEW, 6 * VIEW, 24 * VIEW, R | W);
    for (int i = 0; i < 6; i++) { m[i] = (struct mc){ win + (24 + i) * VIEW, i * 31, 0 }; t[i] = CreateThread(NULL, 0, memcpy_thread, &m[i], 0, NULL); }
    WaitForMultipleObjects(6, t, TRUE, 30000);
    int good = 1;
    for (int i = 0; i < 6; i++) { CloseHandle(t[i]); good &= m[i].ok; }
    check(good, "memcpy into/out of bridge views on 6 threads, 200 rounds each");
    /* write watch */
    ULONG_PTR count = 16; PVOID addrs[16]; ULONG gran;
    UINT r = GetWriteWatch(0, win, VIEW, addrs, &count, &gran);
    printf("    GetWriteWatch(window, not MEM_WRITE_WATCH) = %u err=%lu\n", r, r ? GetLastError() : 0);
    unsigned char *ww = VirtualAlloc(NULL, 4 * VIEW, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    b_map(ww, VIEW, 30 * VIEW, R | W);
    ww[10] = 1;
    count = 16;
    r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, ww, 4 * VIEW, addrs, &count, &gran);
    printf("    GetWriteWatch(MEM_WRITE_WATCH reserve with a bridge view written) = %u err=%lu count=%llu\n", r,
           r ? GetLastError() : 0, (unsigned long long)count);
    maps("ww after GetWriteWatch", ww);
}

/* ---- 3b: the dangerous ones -------------------------------------------------------------- */
static void test_dangerous(void)
{
    printf("\n== 3b. VirtualProtect / VirtualAlloc(COMMIT) / VirtualFree on the window\n");
    unsigned char *p = win + 40 * VIEW;
    b_map(p, 4 * VIEW, 40 * VIEW, R | W);
    memset(p, 0x99, 4 * VIEW);
    DWORD old = 0;
    BOOL ok = VirtualProtect(p, VIEW, PAGE_READONLY, &old);
    printf("    VirtualProtect(bridge RW view, READONLY) = %d err=%lu old=%lx\n", ok, ok ? 0 : GetLastError(), old);
    maps("after VirtualProtect", p);
    check(!ok && p[0] == 0x99, "VirtualProtect on the window fails (not committed) and leaves our view alone");
    /* VirtualAlloc(MEM_RESERVE) at an address in the window */
    void *again = VirtualAlloc(p, VIEW, MEM_RESERVE, PAGE_READWRITE);
    printf("    VirtualAlloc(window addr, MEM_RESERVE) = %p err=%lu\n", again, again ? 0 : GetLastError());
    /* VirtualAlloc(MEM_COMMIT) on a view */
    unsigned char *q = p + VIEW;
    void *c = VirtualAlloc(q, VIEW, MEM_COMMIT, PAGE_READWRITE);
    printf("    VirtualAlloc(bridge view, MEM_COMMIT RW) = %p err=%lu\n", c, c ? 0 : GetLastError());
    maps("after MEM_COMMIT", q);
    printf("      q[0] now %02x (was 99)\n", q[0]);
    if (c) {
        ok = VirtualProtect(q, VIEW, PAGE_READONLY, &old);
        printf("    VirtualProtect(now committed, READONLY) = %d old=%lx\n", ok, old);
        maps("after VirtualProtect", q);
    }
    /* VirtualFree(MEM_DECOMMIT) on a view */
    unsigned char *d = p + 2 * VIEW;
    ok = VirtualFree(d, VIEW, MEM_DECOMMIT);
    printf("    VirtualFree(bridge view, MEM_DECOMMIT) = %d err=%lu\n", ok, ok ? 0 : GetLastError());
    maps("after MEM_DECOMMIT", d);
    /* the fault path after Wine has reprotected a page it thinks is committed */
    /* VirtualFree(MEM_RELEASE) on the whole window */
    unsigned char *e = p + 3 * VIEW;
    maps("before MEM_RELEASE", e);
    maps("before MEM_RELEASE", win);
    ok = VirtualFree(win, 0, MEM_RELEASE);
    printf("    VirtualFree(window, MEM_RELEASE) = %d err=%lu\n", ok, ok ? 0 : GetLastError());
    maps("after MEM_RELEASE", e);
    maps("after MEM_RELEASE", win);
    void *z = VirtualAlloc(win, VIEW, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    printf("    VirtualAlloc(window base, RESERVE|COMMIT) after release = %p\n", z);
}

/* ---- 5: suspension while in the Unix half ------------------------------------------------ */
static DWORD WINAPI sleeper(void *p) { b_sleep(*(DWORD *)p); return 0; }
static void test_suspend(void)
{
    printf("\n== 5. SuspendThread / GetThreadContext on a thread inside the Unix call\n");
    DWORD ms = 1000;
    LARGE_INTEGER f, t0, t1, t2, t3;
    QueryPerformanceFrequency(&f);
    HANDLE t = CreateThread(NULL, 0, sleeper, &ms, 0, NULL);
    Sleep(200);
    QueryPerformanceCounter(&t0);
    DWORD sc = SuspendThread(t);
    QueryPerformanceCounter(&t1);
    CONTEXT ctx = { .ContextFlags = CONTEXT_FULL };
    BOOL ok = GetThreadContext(t, &ctx);
    QueryPerformanceCounter(&t2);
    printf("    SuspendThread=%lu in %.2f ms; GetThreadContext=%d (err %lu) in %.2f ms, rip=%p\n", sc,
           (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart, ok, ok ? 0 : GetLastError(),
           (t2.QuadPart - t1.QuadPart) * 1e3 / f.QuadPart, (void *)ctx.Rip);
    ResumeThread(t);
    WaitForSingleObject(t, 5000);
    QueryPerformanceCounter(&t3);
    printf("    thread done %.0f ms after the suspension began (sleep was %lu ms from start)\n",
           (t3.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart, ms);
    check(ok, "GetThreadContext on a thread in the Unix call answers");
    CloseHandle(t);
    /* does a suspension that outlasts the Unix call hold the thread when the call returns? */
    ms = 300;
    t = CreateThread(NULL, 0, sleeper, &ms, 0, NULL);
    Sleep(100);
    SuspendThread(t);
    Sleep(1000);                      /* the Unix call ends ~200 ms into this */
    DWORD before = WaitForSingleObject(t, 0);
    QueryPerformanceCounter(&t0);
    ResumeThread(t);
    DWORD after = WaitForSingleObject(t, 5000);
    QueryPerformanceCounter(&t1);
    double tail = (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart;
    printf("    suspended across the end of the Unix call: finished while suspended=%d, after resume=%d in %.2f ms\n",
           before == WAIT_OBJECT_0, after == WAIT_OBJECT_0, tail);
    printf("    -> %s\n", tail > 150 ? "the thread was frozen INSIDE the Unix call (its sleep went on after the resume)"
                                     : "the Unix call ran on while suspended; the thread stopped on its way back");
    check(before == WAIT_TIMEOUT && after == WAIT_OBJECT_0, "a suspended thread in the Unix call stays suspended and finishes after resume");
    CloseHandle(t);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryA("s2bridge.dll");
    if (!m) { printf("LoadLibrary(s2bridge.dll) failed: %lu\n", GetLastError()); return 1; }
    b_pool = (POOL)GetProcAddress(m, "s2_pool"); b_map = (MAP)GetProcAddress(m, "s2_map");
    b_unmap = (UNMAP)GetProcAddress(m, "s2_unmap"); b_protect = (PROTECT)GetProcAddress(m, "s2_protect");
    b_maps = (MAPS)GetProcAddress(m, "s2_maps"); b_sleep = (SLEEPU)GetProcAddress(m, "s2_sleep");
    b_ready = (READY)GetProcAddress(m, "s2_ready"); b_readv = (READV)GetProcAddress(m, "s2_readv");
    if (!b_ready || !b_ready()) { printf("bridge Unix half not found\n"); return 1; }
    if (b_pool(WINDOW) < 0) { printf("memfd pool failed\n"); return 1; }
    win = VirtualAlloc(NULL, WINDOW, MEM_RESERVE, PAGE_NOACCESS);
    printf("window %p..%p (%llu MiB), reserved through Wine\n", win, win + WINDOW, WINDOW >> 20);
    maps("window base", win);
    AddVectoredExceptionHandler(1, handler);
    test_faults();
    test_safecopy();
    test_queries();
    test_touch();
    test_suspend();
    test_dangerous();
    printf("\n%d check(s) failed\n", fails);
    return fails != 0;
}
