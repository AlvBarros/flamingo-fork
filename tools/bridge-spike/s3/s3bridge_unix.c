/* S3 bridge, Linux half: a memfd page pool and views of it placed with MAP_FIXED inside address
 * space the Windows side reserved through Wine (as the memory bridge will do). */
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
typedef int NTSTATUS;

struct pool_args { uint64_t bytes; int fd; };
struct map_args { void *at; uint64_t len; int fd; uint64_t offset; };
struct unmap_args { void *at; uint64_t len; };

static NTSTATUS pool(void *args)
{
    struct pool_args *a = args;
    a->fd = memfd_create("s3-pool", MFD_CLOEXEC);
    if (a->fd < 0) return 1;
    if (ftruncate(a->fd, (off_t)a->bytes)) return 2;
    return 0;
}

/* A view: the pool's pages at `offset` shown at `at`, read-write, shared. */
static NTSTATUS map(void *args)
{
    struct map_args *a = args;
    void *p = mmap(a->at, a->len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, a->fd, (off_t)a->offset);
    return p == a->at ? 0 : 3;
}

/* Back to reserved, no access; the range stays ours (Wine still thinks it reserved). */
static NTSTATUS unmap(void *args)
{
    struct unmap_args *a = args;
    void *p = mmap(a->at, a->len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p == a->at ? 0 : 4;
}

typedef NTSTATUS (*unixlib_entry_t)(void *);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = { pool, map, unmap };
