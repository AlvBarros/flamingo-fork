/* S4 spike: the unix-call ABI shared by the PE half (s4_pe.c, s4.c) and the Unix half
 * (s4_unix.c). Every field has a fixed size so a Rust #[repr(C)] mirror is exact. */
#ifndef S4_ABI_H
#define S4_ABI_H
#include <stdint.h>

enum s4_func {
    S4_NOP = 0,          /* args ignored, returns 0 */
    S4_POOL_CREATE = 1,  /* struct s4_pool */
    S4_BATCH = 2,        /* struct s4_batch */
    S4_FUNCS
};

enum s4_kind { S4_MAP = 0, S4_UNMAP = 1, S4_PROTECT = 2 };
enum s4_prot { S4_R = 1, S4_W = 2 };      /* the Linux PROT_* bits */

struct s4_pool {                 /* in: bytes, window; out: id */
    uint64_t bytes;
    uint64_t window;             /* the range reserved through Wine; ops outside it are refused */
    uint64_t window_len;
    int32_t id;                  /* the memfd */
    int32_t pad;
};

struct s4_op {                   /* 32 bytes */
    uint64_t addr;
    uint64_t len;
    uint64_t offset;             /* in the pool, for S4_MAP */
    uint32_t prot;               /* S4_R | S4_W, for S4_MAP and S4_PROTECT */
    uint32_t kind;               /* enum s4_kind */
};

struct s4_batch {                /* 32 bytes */
    uint64_t ops;                /* const struct s4_op * */
    uint64_t results;            /* int32_t errno per op (0 = done), or 0: stop at the first failure */
    uint32_t count;
    uint32_t done;               /* out: ops done (all, or the index of the failing one) */
    int32_t pool;
    int32_t error;               /* out: errno of the failing op, 0 if none */
};

#endif
