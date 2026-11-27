/* S1 spike, Linux half: memfd views, userfaultfd write-protect in async mode, PAGEMAP_SCAN.
 * Called through __wine_unix_call under Wine, or directly by the native build of the driver. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include "s1.h"

typedef int NTSTATUS;
static int uffd = -1, pagemap = -1, pool = -1;

static uint64_t ns_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int uffd_open(void)
{
    return (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
}

static NTSTATUS s1_init(void *args)
{
    struct s1_init *a = args;
    const uint64_t want = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
    a->err = 0;
    if (uffd >= 0) close(uffd);
    /* UFFDIO_API may be called once per fd: ask what exists on one fd, then enable on a fresh one */
    int probe = uffd_open();
    if (probe < 0) { a->err = errno; return 0; }
    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(probe, UFFDIO_API, &api)) { a->err = errno; close(probe); return 0; }
    a->features_avail = api.features;
    close(probe);
    uffd = uffd_open();
    api = (struct uffdio_api){ .api = UFFD_API, .features = want & a->features_avail };
    if (uffd < 0 || ioctl(uffd, UFFDIO_API, &api)) { a->err = errno; return 0; }
    a->features_got = api.features;
    if (pagemap < 0) pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) a->err = errno;
    return 0;
}

static NTSTATUS s1_pool(void *args)
{
    struct s1_pool *a = args;
    a->err = 0;
    if (pool >= 0) close(pool);
    pool = memfd_create("s1-pool", MFD_CLOEXEC);
    if (pool < 0 || ftruncate(pool, (off_t)a->bytes)) a->err = errno;
    return 0;
}

static int reg(uint64_t addr, uint64_t len)
{
    struct uffdio_register r = { .range = { addr, len }, .mode = UFFDIO_REGISTER_MODE_WP };
    return ioctl(uffd, UFFDIO_REGISTER, &r);
}

static int arm(uint64_t addr, uint64_t len)
{
    struct uffdio_writeprotect w = { .range = { addr, len }, .mode = UFFDIO_WRITEPROTECT_MODE_WP };
    return ioctl(uffd, UFFDIO_WRITEPROTECT, &w);
}

static NTSTATUS s1_map(void *args)
{
    struct s1_map *a = args;
    a->ns_mmap = a->ns_reg = a->ns_arm = 0; a->err = 0; a->failed_at = a->nviews;
    for (uint64_t i = 0; i < a->nviews; i++) {
        uint64_t at = a->base + i * a->view, t0 = ns_now(), t1, t2, t3;
        void *p = mmap((void *)at, a->view, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, pool,
                       (off_t)((i % a->slots) * a->view));
        t1 = ns_now();
        if (p != (void *)at) goto fail;
        if ((a->flags & S1_MAP_REGISTER) && reg(at, a->view)) goto fail;
        t2 = ns_now();
        if ((a->flags & S1_MAP_ARM) && arm(at, a->view)) goto fail;
        t3 = ns_now();
        a->ns_mmap += t1 - t0; a->ns_reg += t2 - t1; a->ns_arm += t3 - t2;
        continue;
    fail:
        a->err = errno; a->failed_at = i; return 0;
    }
    return 0;
}

static NTSTATUS s1_unmap(void *args)
{
    struct s1_map *a = args;
    uint64_t t0 = ns_now();
    a->err = 0; a->failed_at = a->nviews;
    for (uint64_t i = 0; i < a->nviews; i++) {
        uint64_t at = a->base + i * a->view;
        if (mmap((void *)at, a->view, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != (void *)at) {
            a->err = errno; a->failed_at = i; break;
        }
    }
    a->ns_mmap = ns_now() - t0;
    return 0;
}

static NTSTATUS s1_range_op(void *args, int op)
{
    struct s1_range *a = args;
    uint64_t t0 = ns_now();
    int r;
    if (op == S1_REGISTER) r = reg(a->addr, a->len);
    else if (op == S1_ARM) r = arm(a->addr, a->len);
    else { struct uffdio_range u = { a->addr, a->len }; r = ioctl(uffd, UFFDIO_UNREGISTER, &u); }
    a->ns = ns_now() - t0;
    a->err = r ? errno : 0;
    return 0;
}
static NTSTATUS s1_register(void *a) { return s1_range_op(a, S1_REGISTER); }
static NTSTATUS s1_arm(void *a) { return s1_range_op(a, S1_ARM); }
static NTSTATUS s1_unregister(void *a) { return s1_range_op(a, S1_UNREGISTER); }

/* Written pages in [addr, addr+len), as regions at 4 KiB granularity; with S1_SCAN_WP they are
 * write-protected again by the same walk. Loops while the output vector fills. */
static NTSTATUS s1_scan(void *args)
{
    struct s1_scan *a = args;
    struct s1_region *out = (struct s1_region *)(uintptr_t)a->out;
    uint64_t t0 = ns_now(), start = a->addr, end = a->addr + a->len;
    a->n_regions = a->pages = a->ioctls = 0; a->err = 0; a->ret = 0;
    while (start < end && a->n_regions < a->max_regions) {
        struct pm_scan_arg s = {
            .size = sizeof s, .start = start, .end = end,
            .flags = ((a->flags & S1_SCAN_WP) ? PM_SCAN_WP_MATCHING : 0) |
                     ((a->flags & S1_SCAN_CHECK) ? PM_SCAN_CHECK_WPASYNC : 0),
            .vec = (uint64_t)(uintptr_t)(out + a->n_regions), .vec_len = a->max_regions - a->n_regions,
            .category_mask = PAGE_IS_WRITTEN, .return_mask = PAGE_IS_WRITTEN,
        };
        long r = ioctl(pagemap, PAGEMAP_SCAN, &s);
        a->ioctls++;
        a->ret = r;
        if (r < 0) { a->err = errno; break; }
        for (long k = 0; k < r; k++) a->pages += (out[a->n_regions + k].end - out[a->n_regions + k].start) >> 12;
        a->n_regions += (uint64_t)r;
        if (s.walk_end >= end) break;
        start = s.walk_end;
    }
    a->ns = ns_now() - t0;
    return 0;
}

static NTSTATUS s1_pid(void *args) { *(int64_t *)args = getpid(); return 0; }

typedef NTSTATUS (*unixlib_entry_t)(void *);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[S1_COUNT] = {
    s1_init, s1_pool, s1_map, s1_unmap, s1_register, s1_arm, s1_scan, s1_unregister, s1_pid,
};
