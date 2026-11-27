/* S2 spike, Linux half: memfd pool, views, unmaps and protections behind Wine's back. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "s2_abi.h"
typedef int NTSTATUS;
static int pool_fd = -1;

static NTSTATUS pool(void *p)
{
    struct s2_pool_args *a = p;
    pool_fd = memfd_create("s2pool", 0);
    if (pool_fd < 0 || ftruncate(pool_fd, a->bytes)) return 1;
    a->fd = pool_fd;
    return 0;
}
static NTSTATUS map(void *p)
{
    struct s2_map_args *a = p;
    void *at = (void *)(uintptr_t)a->at;
    a->err = 0;
    if (mmap(at, a->len, a->prot, MAP_SHARED | MAP_FIXED, pool_fd, a->off) != at) { a->err = errno; return 1; }
    return 0;
}
static NTSTATUS unmap(void *p)
{
    struct s2_map_args *a = p;
    void *at = (void *)(uintptr_t)a->at;
    a->err = 0;
    if (mmap(at, a->len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != at) { a->err = errno; return 1; }
    return 0;
}
static NTSTATUS protect(void *p)
{
    struct s2_map_args *a = p;
    a->err = 0;
    if (mprotect((void *)(uintptr_t)a->at, a->len, a->prot)) { a->err = errno; return 1; }
    return 0;
}
static NTSTATUS maps(void *p)
{
    struct s2_maps_args *a = p;
    char buf[1024];
    FILE *f = fopen("/proc/self/maps", "r");
    strcpy(a->line, "(no mapping)");
    if (!f) return 1;
    while (fgets(buf, sizeof buf, f)) {
        unsigned long lo, hi;
        if (sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && a->at >= lo && a->at < hi) {
            buf[strcspn(buf, "\n")] = 0;
            snprintf(a->line, sizeof a->line, "%s", buf);
            break;
        }
    }
    fclose(f);
    return 0;
}
static NTSTATUS do_sleep(void *p)
{
    struct s2_sleep_args *a = p;
    struct timespec t = { a->ms / 1000, (a->ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) {}
    return 0;
}
static NTSTATUS pid(void *p) { *(int *)p = getpid(); return 0; }
static NTSTATUS readv_self(void *p)
{
    struct s2_readv_args *a = p;
    struct iovec l = { (void *)(uintptr_t)a->dst, a->len }, r = { (void *)(uintptr_t)a->src, a->len };
    a->got = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    a->err = a->got < 0 ? errno : 0;
    return 0;
}
typedef NTSTATUS (*unixlib_entry_t)(void *);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = {
    pool, map, unmap, protect, maps, do_sleep, pid, readv_self,
};
