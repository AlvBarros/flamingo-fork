/* S1 spike: the unix-call ABI shared by the Linux half (s1_unix.c), the PE half (s1_pe.c) and the
 * test driver (s1_driver.c, built both as a Windows exe and natively). Fixed-width fields only, so
 * the layout is the same under LLP64 (MinGW) and LP64 (gcc). */
#ifndef S1_H
#define S1_H
#include <stdint.h>

enum { S1_INIT, S1_POOL, S1_MAP, S1_UNMAP, S1_REGISTER, S1_ARM, S1_SCAN, S1_UNREGISTER, S1_PID, S1_COUNT };

struct s1_init {          /* opens the uffd (UFFD_USER_MODE_ONLY) and /proc/self/pagemap */
    uint64_t features_avail, features_got; /* out */
    int64_t err;          /* out: errno, 0 if fine */
};
struct s1_pool {          /* (re)creates the memfd pool */
    uint64_t bytes;
    int64_t err;
};
#define S1_MAP_REGISTER 1 /* UFFDIO_REGISTER(MODE_WP) each view right after its mmap */
#define S1_MAP_ARM 2      /* UFFDIO_WRITEPROTECT each view right after registering */
struct s1_map {           /* view i at base + i*view, pool offset (i % slots) * view; S1_UNMAP: back to PROT_NONE anon */
    uint64_t base, nviews, view, slots, flags;
    uint64_t ns_mmap, ns_reg, ns_arm; /* out: totals */
    int64_t err;          /* out: errno of the first failure */
    uint64_t failed_at;   /* out: index of the first failure (or nviews) */
};
struct s1_range {         /* S1_REGISTER / S1_ARM / S1_UNREGISTER */
    uint64_t addr, len;
    uint64_t ns;          /* out */
    int64_t err;          /* out */
};
#define S1_SCAN_WP 1      /* PM_SCAN_WP_MATCHING: re-arm what is returned */
#define S1_SCAN_CHECK 2   /* PM_SCAN_CHECK_WPASYNC: fail (EPERM) on a VMA without async WP */
struct s1_region { uint64_t start, end, categories; }; /* = struct page_region */
struct s1_scan {
    uint64_t addr, len, flags;
    uint64_t out, max_regions; /* struct s1_region[max_regions] */
    uint64_t n_regions, pages, ns, ioctls; /* out */
    int64_t ret, err;     /* out: last ioctl's return and errno */
};
#endif
