/* S4 spike, Unix half: native code reached through __wine_unix_call. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "abi.h"

typedef int NTSTATUS;
static uint64_t window, window_len;

/* counts calls into *args when given one, so the caller can check every call arrived */
static NTSTATUS nop(void *args) { if (args) ++*(uint64_t *)args; return 0; }

static NTSTATUS pool_create(void *args)
{
    struct s4_pool *p = args;
    int fd = memfd_create("s4-pool", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)p->bytes)) return 1;
    window = p->window; window_len = p->window_len;
    p->id = fd;
    return 0;
}

static int one(const struct s4_op *op, int fd)
{
    if (op->addr < window || op->len > window_len || op->addr - window > window_len - op->len) return EFAULT;
    void *at = (void *)(uintptr_t)op->addr;
    switch (op->kind) {
    case S4_MAP:
        return mmap(at, op->len, (int)op->prot, MAP_SHARED | MAP_FIXED, fd, (off_t)op->offset) == at ? 0 : errno;
    case S4_UNMAP:
        return mmap(at, op->len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == at ? 0 : errno;
    case S4_PROTECT:
        return mprotect(at, op->len, (int)op->prot) ? errno : 0;
    }
    return EINVAL;
}

static NTSTATUS batch(void *args)
{
    struct s4_batch *b = args;
    const struct s4_op *ops = (const struct s4_op *)(uintptr_t)b->ops;
    int32_t *results = (int32_t *)(uintptr_t)b->results;
    uint32_t i;
    b->error = 0;
    if (results) {
        for (i = 0; i < b->count; i++) results[i] = one(&ops[i], b->pool);
    } else {
        for (i = 0; i < b->count; i++) {
            int e = one(&ops[i], b->pool);
            if (e) { b->error = e; break; }
        }
    }
    b->done = i;
    return 0;
}

typedef NTSTATUS (*unixlib_entry_t)(void *);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[S4_FUNCS] = { nop, pool_create, batch };
