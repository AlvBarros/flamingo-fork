/* The memory bridge's Unix half (WS1 of docs/memory-bridge-plan.md): the title's page pools
 * (memfd), views (MAP_FIXED mmap), protections (mprotect), write tracking (userfaultfd
 * write-protect in async mode + PAGEMAP_SCAN) and fault-free copies (process_vm_readv), done with
 * Linux calls. Wine loads it as the Unix half of victorybridge.dll and the PE half reaches it
 * through __wine_unix_call(handle, code, args); `code` indexes __wine_unix_call_funcs below.
 *
 * The ABI and every call's meaning: crates/victory-bridge/src/abi.rs (the authority), mirrored
 * in abi.h. What this file adds to it, the rules it keeps:
 *
 * - Never fault here. Arguments are checked before any syscall; copies go through
 *   process_vm_readv, which fails instead of faulting (spike S2: a fault in Unix code does not
 *   reach the title's handler as an ordinary access violation).
 * - Every syscall is retried on EINTR (S2: a thread Wine suspends is frozen inside the call by a
 *   signal; its syscalls then return EINTR). The uffd ioctls are retried on EAGAIN as well (the
 *   kernel's "mm is changing", during a fork).
 * - The hot paths take no lock: pool and window lookups read atomics. A small mutex serialises
 *   the writers (pool create/destroy, window add/remove) and is held over table updates only, so
 *   a thread suspended in a hot path never makes another wait; a thread suspended while it holds
 *   the mutex only delays another create/destroy/add/remove.
 * - Callers may call from several threads. The PE side serialises changes (maps, unmaps,
 *   protections, windows) but not takes and copies; nothing here relies on that except that
 *   a pool is not destroyed, nor a window removed, while an operation on it is running (the
 *   result is then the caller's race: a view of a closed pool's memfd, or an op refused).
 * - A failed MAP op leaves its range unmapped (PROT_NONE anonymous), whatever was there before:
 *   a MAP_FIXED mmap may already have dropped the old view when it fails, and a view whose write
 *   tracking could not be armed must not stay (writes through it would never be reported). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>

#include "abi.h"

typedef int32_t NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

#define GRANULE ((uint64_t)VB_GRANULE)
#define ALIGNED(x) (((x) & (GRANULE - 1)) == 0)
#define PROT_ALL (PROT_READ | PROT_WRITE | PROT_EXEC)

/* ---- retrying syscalls ---- */

static void *map_fixed(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    void *p;
    do p = mmap((void *)(uintptr_t)addr, len, prot, flags | MAP_FIXED, fd, (off_t)offset);
    while (p == MAP_FAILED && errno == EINTR);
    return p;
}

/* Back to "unmapped": PROT_NONE anonymous private memory, still reserved and ours. */
static int unmapped(uint64_t addr, uint64_t len)
{
    void *p = map_fixed(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == (void *)(uintptr_t)addr ? 0 : errno;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* ---- write tracking: probed once ---- */

static pthread_once_t probe_once = PTHREAD_ONCE_INIT;
static int uffd = -1, pagemap = -1;
static uint64_t features;
static pid_t self;

static int uffd_open(void)
{
    int fd;
    do fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    while (fd < 0 && errno == EINTR);
    return fd;
}

static int wp_register(uint64_t addr, uint64_t len)
{
    struct uffdio_register r = { .range = { addr, len }, .mode = UFFDIO_REGISTER_MODE_WP };
    return xioctl(uffd, UFFDIO_REGISTER, &r) ? errno : 0;
}

static int wp_arm(uint64_t addr, uint64_t len)
{
    struct uffdio_writeprotect w = { .range = { addr, len }, .mode = UFFDIO_WRITEPROTECT_MODE_WP };
    return xioctl(uffd, UFFDIO_WRITEPROTECT, &w) ? errno : 0;
}

/* One PAGEMAP_SCAN step: written pages from `start`, re-armed (never PM_SCAN_CHECK_WPASYNC: S1,
 * a scan failing on an unregistered VMA has already re-armed pages whose writes are then lost;
 * without it unregistered VMAs are skipped). */
static long scan(uint64_t start, uint64_t end, struct page_region *vec, uint64_t vec_len,
                 uint64_t max_pages, uint64_t *walk_end)
{
    struct pm_scan_arg s;
    long r;
    for (;;) {
        memset(&s, 0, sizeof s);
        s.size = sizeof s;
        s.flags = PM_SCAN_WP_MATCHING;
        s.start = start;
        s.end = end;
        s.vec = (uint64_t)(uintptr_t)vec;
        s.vec_len = vec_len;
        s.max_pages = max_pages;
        s.category_mask = PAGE_IS_WRITTEN;
        s.return_mask = PAGE_IS_WRITTEN;
        r = ioctl(pagemap, PAGEMAP_SCAN, &s);
        if (r >= 0 || errno != EINTR) break;
        /* Interrupted: what it found up to walk_end is lost to us but re-armed only if returned;
         * the kernel writes walk_end back even then, so go on from there. */
        if (s.walk_end > start && s.walk_end <= end) start = s.walk_end;
    }
    *walk_end = s.walk_end;
    return r;
}

/* Whether async write-protect really works here: a one-page memfd view, registered and armed,
 * written once, must be found by one scan and not by the next. */
static int watch_works(void)
{
    int ok = 0, fd = memfd_create("victorybridge-probe", MFD_CLOEXEC);
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)GRANULE) == 0) {
        void *p = mmap(NULL, GRANULE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            uint64_t a = (uint64_t)(uintptr_t)p, we;
            struct page_region v[2];
            if (!wp_register(a, GRANULE) && !wp_arm(a, GRANULE)
                && scan(a, a + GRANULE, v, 2, 0, &we) == 0) {
                *(volatile char *)p = 1;
                ok = scan(a, a + GRANULE, v, 2, 0, &we) == 1 && v[0].start == a
                     && v[0].end == a + GRANULE && scan(a, a + GRANULE, v, 2, 0, &we) == 0;
            }
            munmap(p, GRANULE);
        }
    }
    close(fd);
    return ok;
}

static void probe(void)
{
    const uint64_t want = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    char byte = 1, got = 0;
    struct iovec to = { &got, 1 }, from = { &byte, 1 };
    int probe_fd;

    self = getpid();
    if (process_vm_readv(self, &to, 1, &from, 1, 0) == 1 && got == 1) features |= VB_FEATURE_COPY;

    /* UFFDIO_API may be called once per fd: ask what exists on one, enable on a fresh one. */
    probe_fd = uffd_open();
    if (probe_fd < 0) return;
    if (xioctl(probe_fd, UFFDIO_API, &api) || (api.features & want) != want) {
        close(probe_fd);
        return;
    }
    close(probe_fd);
    uffd = uffd_open();
    api = (struct uffdio_api){ .api = UFFD_API, .features = want };
    if (uffd < 0 || xioctl(uffd, UFFDIO_API, &api)) goto off;
    do pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    while (pagemap < 0 && errno == EINTR);
    if (pagemap < 0 || !watch_works()) goto off;
    features |= VB_FEATURE_WATCH;
    return;
off:
    if (uffd >= 0) close(uffd);
    if (pagemap >= 0) close(pagemap);
    uffd = pagemap = -1;
}

static uint64_t feature(uint64_t f)
{
    pthread_once(&probe_once, probe);
    return features & f;
}

/* ---- tables: lock-free readers, one mutex for writers ---- */

static pthread_mutex_t writers = PTHREAD_MUTEX_INITIALIZER;

/* Pools: id = generation << 32 | (slot + 1), so ids are nonzero and a destroyed pool's id is
 * never valid again while its slot is reused. Slots live in chunks allocated once, never freed,
 * so a reader may look at any slot at any time. */
#define POOL_CHUNK 1024
#define POOL_CHUNKS 1024
struct pool_slot { _Atomic uint32_t gen; _Atomic int fd; _Atomic uint64_t bytes; };
static struct pool_slot *_Atomic pool_chunks[POOL_CHUNKS];
static uint32_t pool_slots_used;             /* writers only: slots ever handed out */
static uint32_t *pool_free, pool_free_n, pool_free_cap; /* writers only: freed slot indices */

static struct pool_slot *pool_slot(uint32_t slot)
{
    struct pool_slot *chunk;
    if (slot >= (uint32_t)POOL_CHUNK * POOL_CHUNKS) return NULL;
    chunk = atomic_load_explicit(&pool_chunks[slot / POOL_CHUNK], memory_order_acquire);
    return chunk ? &chunk[slot % POOL_CHUNK] : NULL;
}

/* The pool's fd and size, or -1 when `id` names no live pool. */
static int pool_find(uint64_t id, uint64_t *bytes)
{
    uint32_t low = (uint32_t)id, gen = (uint32_t)(id >> 32);
    struct pool_slot *s;
    int fd;
    if (!low || !(gen & 1) || !(s = pool_slot(low - 1))) return -1;
    if (atomic_load_explicit(&s->gen, memory_order_acquire) != gen) return -1;
    fd = atomic_load_explicit(&s->fd, memory_order_acquire);
    *bytes = atomic_load_explicit(&s->bytes, memory_order_relaxed);
    if (atomic_load_explicit(&s->gen, memory_order_acquire) != gen) return -1;
    return fd;
}

/* Windows: a fixed table; a slot is live when its addr is nonzero. Readers scan up to the
 * high-water mark. */
#define WINDOWS 1024
struct window_slot { _Atomic uint64_t addr, len; };
static struct window_slot windows[WINDOWS];
static _Atomic uint32_t windows_top;

/* Whether [addr, addr + len) lies inside one window (len > 0, no overflow: checked by callers). */
static int in_window(uint64_t addr, uint64_t len)
{
    uint32_t top = atomic_load_explicit(&windows_top, memory_order_acquire);
    for (uint32_t i = 0; i < top; i++) {
        uint64_t a = atomic_load_explicit(&windows[i].addr, memory_order_acquire);
        uint64_t l = atomic_load_explicit(&windows[i].len, memory_order_relaxed);
        if (a && addr >= a && addr - a < l && len <= l - (addr - a)) return 1;
    }
    return 0;
}

/* 0, or the errno for a range: EINVAL when malformed, EFAULT when outside every window. */
static int check_range(uint64_t addr, uint64_t len)
{
    if (!len || !ALIGNED(addr) || !ALIGNED(len) || addr + len < addr) return EINVAL;
    return in_window(addr, len) ? 0 : EFAULT;
}

/* ---- the calls ---- */

static NTSTATUS vb_version(void *args)
{
    struct vb_version *a = args;
    a->version = VB_VERSION;
    a->pad = 0;
    a->features = feature(VB_FEATURE_WATCH | VB_FEATURE_COPY);
    return STATUS_SUCCESS;
}

static NTSTATUS vb_pool_create(void *args)
{
    struct vb_pool_create *a = args;
    uint32_t slot;
    struct pool_slot *s;
    int fd, r;

    a->pool = 0;
    if (!a->bytes || !ALIGNED(a->bytes) || a->bytes > (uint64_t)INT64_MAX) return STATUS_INVALID_PARAMETER;
    /* MFD_EXEC keeps PROT_EXEC views possible under vm.memfd_noexec and spares the kernel's
     * warning; kernels before 6.3 refuse the flag. */
    do fd = memfd_create("victorybridge-pool", MFD_CLOEXEC | MFD_EXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0 && errno == EINVAL) {
        do fd = memfd_create("victorybridge-pool", MFD_CLOEXEC);
        while (fd < 0 && errno == EINTR);
    }
    if (fd < 0) return STATUS_NO_MEMORY;
    do r = ftruncate(fd, (off_t)a->bytes);
    while (r && errno == EINTR);
    if (r) {
        close(fd);
        return STATUS_NO_MEMORY;
    }

    pthread_mutex_lock(&writers);
    if (pool_free_n) slot = pool_free[--pool_free_n];
    else {
        slot = pool_slots_used;
        if (slot >= (uint32_t)POOL_CHUNK * POOL_CHUNKS) goto full;
        if (!pool_chunks[slot / POOL_CHUNK]) {
            struct pool_slot *chunk = calloc(POOL_CHUNK, sizeof *chunk);
            if (!chunk) goto full;
            atomic_store_explicit(&pool_chunks[slot / POOL_CHUNK], chunk, memory_order_release);
        }
        pool_slots_used++;
    }
    s = pool_slot(slot);
    {
        uint32_t gen = atomic_load_explicit(&s->gen, memory_order_relaxed);
        /* Free slots have an even generation, live ones an odd one: never 0 for a live pool. */
        gen++;
        atomic_store_explicit(&s->fd, fd, memory_order_relaxed);
        atomic_store_explicit(&s->bytes, a->bytes, memory_order_relaxed);
        atomic_store_explicit(&s->gen, gen, memory_order_release);
        a->pool = (uint64_t)gen << 32 | (slot + 1);
    }
    pthread_mutex_unlock(&writers);
    return STATUS_SUCCESS;
full:
    pthread_mutex_unlock(&writers);
    close(fd);
    return STATUS_NO_MEMORY;
}

static NTSTATUS vb_pool_destroy(void *args)
{
    struct vb_pool_destroy *a = args;
    uint32_t low = (uint32_t)a->pool, gen = (uint32_t)(a->pool >> 32);
    struct pool_slot *s;
    int fd = -1;

    pthread_mutex_lock(&writers);
    if (low && (s = pool_slot(low - 1)) && (gen & 1)
        && atomic_load_explicit(&s->gen, memory_order_relaxed) == gen) {
        if (pool_free_n == pool_free_cap) {
            uint32_t cap = pool_free_cap ? pool_free_cap * 2 : 256;
            uint32_t *grown = realloc(pool_free, cap * sizeof *grown);
            if (!grown) {
                pthread_mutex_unlock(&writers);
                return STATUS_NO_MEMORY;
            }
            pool_free = grown;
            pool_free_cap = cap;
        }
        fd = atomic_load_explicit(&s->fd, memory_order_relaxed);
        atomic_store_explicit(&s->gen, gen + 1, memory_order_release);
        atomic_store_explicit(&s->fd, -1, memory_order_relaxed);
        pool_free[pool_free_n++] = low - 1;
    }
    pthread_mutex_unlock(&writers);
    if (fd < 0) return STATUS_INVALID_HANDLE;
    close(fd); /* views of it stay: they hold the memfd's pages */
    return STATUS_SUCCESS;
}

static NTSTATUS vb_window_add(void *args)
{
    struct vb_window *a = args;
    uint32_t top, free_slot = WINDOWS;
    NTSTATUS status = STATUS_SUCCESS;

    if (!a->len || !ALIGNED(a->addr) || !ALIGNED(a->len) || !a->addr || a->addr + a->len < a->addr)
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&writers);
    top = atomic_load_explicit(&windows_top, memory_order_relaxed);
    for (uint32_t i = 0; i < top; i++) {
        uint64_t w = atomic_load_explicit(&windows[i].addr, memory_order_relaxed);
        uint64_t l = atomic_load_explicit(&windows[i].len, memory_order_relaxed);
        if (!w) {
            if (free_slot == WINDOWS) free_slot = i;
        } else if (a->addr < w + l && w < a->addr + a->len) {
            status = STATUS_INVALID_PARAMETER; /* overlaps a window */
            goto out;
        }
    }
    if (free_slot == WINDOWS) {
        if (top == WINDOWS) {
            status = STATUS_NO_MEMORY;
            goto out;
        }
        free_slot = top;
    }
    atomic_store_explicit(&windows[free_slot].len, a->len, memory_order_relaxed);
    atomic_store_explicit(&windows[free_slot].addr, a->addr, memory_order_release);
    if (free_slot == top) atomic_store_explicit(&windows_top, top + 1, memory_order_release);
out:
    pthread_mutex_unlock(&writers);
    return status;
}

static NTSTATUS vb_window_remove(void *args)
{
    struct vb_window *a = args;
    uint32_t top;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    pthread_mutex_lock(&writers);
    top = atomic_load_explicit(&windows_top, memory_order_relaxed);
    for (uint32_t i = 0; i < top; i++) {
        if (!a->addr || atomic_load_explicit(&windows[i].addr, memory_order_relaxed) != a->addr
            || atomic_load_explicit(&windows[i].len, memory_order_relaxed) != a->len)
            continue;
        /* The whole window back to unmapped first (views and registrations go with it); kept
         * when that fails, so the caller does not free a range with our views in it. */
        if (unmapped(a->addr, a->len)) {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        atomic_store_explicit(&windows[i].addr, 0, memory_order_release);
        atomic_store_explicit(&windows[i].len, 0, memory_order_relaxed);
        status = STATUS_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&writers);
    return status;
}

/* Common batch checks; the ops and results arrays, or a refusal. */
static NTSTATUS batch_start(struct vb_batch *a)
{
    a->done = 0;
    if (a->count > VB_BATCH_MAX) return STATUS_INVALID_PARAMETER;
    if (a->count && (!a->ops || !a->results)) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS vb_map(void *args)
{
    struct vb_batch *a = args;
    const struct vb_map_op *ops = (const struct vb_map_op *)(uintptr_t)a->ops;
    int32_t *results = (int32_t *)(uintptr_t)a->results;
    uint64_t bytes = 0, done = 0;
    int fd, watch;
    NTSTATUS status = batch_start(a);

    if (status) return status;
    if ((fd = pool_find(a->pool, &bytes)) < 0) return STATUS_INVALID_HANDLE;
    watch = feature(VB_FEATURE_WATCH) != 0;
    for (uint64_t i = 0; i < a->count; i++) {
        struct vb_map_op op = ops[i];
        int err = check_range(op.addr, op.len);
        if (!err && (!ALIGNED(op.offset) || op.offset > bytes || op.len > bytes - op.offset
                     || (op.prot & ~PROT_ALL) || (op.flags & ~VB_MAP_WATCH)))
            err = EINVAL;
        if (!err) {
            if (map_fixed(op.addr, op.len, (int)op.prot, MAP_SHARED, fd, op.offset) != (void *)(uintptr_t)op.addr)
                err = errno;
            else if (watch && (op.flags & VB_MAP_WATCH))
                (void)((err = wp_register(op.addr, op.len)) || (err = wp_arm(op.addr, op.len)));
            if (err) unmapped(op.addr, op.len);
        }
        results[i] = err;
        done += !err;
    }
    a->done = done;
    return STATUS_SUCCESS;
}

enum range_op { UNMAP, PROTECT, WATCH };

static NTSTATUS range_batch(void *args, enum range_op what)
{
    struct vb_batch *a = args;
    const struct vb_range *ops = (const struct vb_range *)(uintptr_t)a->ops;
    int32_t *results = (int32_t *)(uintptr_t)a->results;
    uint64_t done = 0;
    NTSTATUS status = batch_start(a);

    if (status) return status;
    if (what == WATCH && !feature(VB_FEATURE_WATCH)) return STATUS_NOT_SUPPORTED;
    for (uint64_t i = 0; i < a->count; i++) {
        struct vb_range op = ops[i];
        int err = check_range(op.addr, op.len);
        if (!err) switch (what) {
        case UNMAP:
            err = unmapped(op.addr, op.len);
            break;
        case PROTECT:
            if (op.prot & ~PROT_ALL) err = EINVAL;
            else if (mprotect((void *)(uintptr_t)op.addr, op.len, (int)op.prot)) err = errno;
            break;
        case WATCH:
            (void)((err = wp_register(op.addr, op.len)) || (err = wp_arm(op.addr, op.len)));
            break;
        }
        results[i] = err;
        done += !err;
    }
    a->done = done;
    return STATUS_SUCCESS;
}

static NTSTATUS vb_unmap(void *args) { return range_batch(args, UNMAP); }
static NTSTATUS vb_protect(void *args) { return range_batch(args, PROTECT); }
static NTSTATUS vb_watch_register(void *args) { return range_batch(args, WATCH); }

static NTSTATUS vb_watch_take(void *args)
{
    struct vb_watch_take *a = args;
    uint64_t *pages = (uint64_t *)(uintptr_t)a->pages;
    uint64_t at = a->addr, end = a->addr + a->len, n = 0;
    struct page_region vec[256];

    a->count = a->more = 0;
    if (!feature(VB_FEATURE_WATCH)) return STATUS_NOT_SUPPORTED;
    if (check_range(a->addr, a->len) || !a->capacity || !pages) return STATUS_INVALID_PARAMETER;
    while (at < end && n < a->capacity) {
        uint64_t left = a->capacity - n, walk_end = end;
        /* max_pages bounds what the kernel re-arms to what we can hand back: nothing written is
         * re-armed without being reported. */
        long r = scan(at, end, vec, left < 256 ? left : 256, left, &walk_end);
        if (r < 0) {
            /* Pages already found are re-armed: hand them back (with `more`, so the caller
             * asks again and gets the refusal then) rather than lose their writes. */
            if (!n) return STATUS_UNSUCCESSFUL;
            a->count = n;
            a->more = 1;
            return STATUS_SUCCESS;
        }
        for (long k = 0; k < r; k++)
            for (uint64_t p = vec[k].start; p < vec[k].end && n < a->capacity; p += GRANULE) pages[n++] = p;
        if (walk_end <= at || walk_end >= end) {
            at = end;
            break;
        }
        at = walk_end;
    }
    a->count = n;
    /* Out of room with range left after the last page given: the caller takes again from
     * there (at worst once for nothing). */
    a->more = n == a->capacity && pages[n - 1] + GRANULE < end;
    return STATUS_SUCCESS;
}

static NTSTATUS vb_copy(void *args)
{
    struct vb_copy *a = args;
    uint64_t copied = 0;

    a->copied = 0;
    if (!feature(VB_FEATURE_COPY)) return STATUS_NOT_SUPPORTED;
    while (copied < a->len) {
        struct iovec to = { (void *)(uintptr_t)(a->to + copied), a->len - copied };
        struct iovec from = { (void *)(uintptr_t)(a->from + copied), a->len - copied };
        ssize_t r = process_vm_readv(self, &to, 1, &from, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break; /* EFAULT: the first byte here faults, on either side */
        copied += (uint64_t)r;
    }
    a->copied = copied;
    return STATUS_SUCCESS;
}

typedef NTSTATUS (*unixlib_entry_t)(void *args);

__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[VB_CALLS] = {
    [VB_VERSION_CALL] = vb_version,
    [VB_POOL_CREATE] = vb_pool_create,
    [VB_POOL_DESTROY] = vb_pool_destroy,
    [VB_WINDOW_ADD] = vb_window_add,
    [VB_WINDOW_REMOVE] = vb_window_remove,
    [VB_MAP] = vb_map,
    [VB_UNMAP] = vb_unmap,
    [VB_PROTECT] = vb_protect,
    [VB_WATCH_REGISTER] = vb_watch_register,
    [VB_WATCH_TAKE] = vb_watch_take,
    [VB_COPY] = vb_copy,
};
