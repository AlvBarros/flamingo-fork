/* Linux half of the bridge: native code, called through __wine_unix_call. */
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
typedef int NTSTATUS;
struct cycles_args { void *at; int n; double us; };
static NTSTATUS map_cycles(void *args)
{
    struct cycles_args *a = args;
    int fd = memfd_create("bridge", 0);
    if (fd < 0 || ftruncate(fd, 65536)) return 1;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < a->n; i++) {
        if (mmap(a->at, 65536, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != a->at) return 2;
        ((volatile char *)a->at)[7] = 1;
        mmap(a->at, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1); close(fd);
    a->us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / a->n;
    return 0;
}
static NTSTATUS pid(void *args) { *(int *)args = getpid(); return 0; }
typedef NTSTATUS (*unixlib_entry_t)(void *);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = { map_cycles, pid };
