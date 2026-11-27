/* The memory bridge's unix-call ABI: the mirror of
 * crates/victory-bridge/src/abi.rs (the authority; read its comments for
 * what each call means). Any change there changes this file in the same
 * commit; abi.rs's tests check the numbers below, the asserts the layout. */
#ifndef VICTORY_BRIDGE_ABI_H
#define VICTORY_BRIDGE_ABI_H

#include <stdint.h>

#define VB_VERSION 1
#define VB_GRANULE 4096
#define VB_BATCH_MAX 4096

/* Call numbers: indices into __wine_unix_call_funcs. */
#define VB_VERSION_CALL 0
#define VB_POOL_CREATE 1
#define VB_POOL_DESTROY 2
#define VB_WINDOW_ADD 3
#define VB_WINDOW_REMOVE 4
#define VB_MAP 5
#define VB_UNMAP 6
#define VB_PROTECT 7
#define VB_WATCH_REGISTER 8
#define VB_WATCH_TAKE 9
#define VB_COPY 10
#define VB_CALLS 11

/* Protections are Linux PROT_* bits (PROT_NONE 0, READ 1, WRITE 2, EXEC 4). */

#define VB_FEATURE_WATCH 1
#define VB_FEATURE_COPY 2

struct vb_version { uint32_t version; uint32_t pad; uint64_t features; };
struct vb_pool_create { uint64_t bytes; uint64_t pool; };
struct vb_pool_destroy { uint64_t pool; };
struct vb_window { uint64_t addr; uint64_t len; };
/* A view replacing whatever is there (one MAP_FIXED mmap). */
struct vb_map_op { uint64_t addr; uint64_t len; uint64_t offset; uint32_t prot; uint32_t flags; };
#define VB_MAP_WATCH 1 /* register + arm write tracking right after the mmap */
struct vb_range { uint64_t addr; uint64_t len; uint32_t prot; uint32_t pad; };
/* ops: struct vb_map_op[count] (VB_MAP) or struct vb_range[count];
 * results: int32_t[count], 0 or errno; done: how many succeeded. */
struct vb_batch { uint64_t pool; uint64_t ops; uint64_t count; uint64_t results; uint64_t done; };
struct vb_watch_take { uint64_t addr; uint64_t len; uint64_t pages; uint64_t capacity; uint64_t count; uint64_t more; };
struct vb_copy { uint64_t to; uint64_t from; uint64_t len; uint64_t copied; };

_Static_assert(sizeof(struct vb_version) == 16, "vb_version");
_Static_assert(sizeof(struct vb_pool_create) == 16, "vb_pool_create");
_Static_assert(sizeof(struct vb_pool_destroy) == 8, "vb_pool_destroy");
_Static_assert(sizeof(struct vb_window) == 16, "vb_window");
_Static_assert(sizeof(struct vb_map_op) == 32, "vb_map_op");
_Static_assert(sizeof(struct vb_range) == 24, "vb_range");
_Static_assert(sizeof(struct vb_batch) == 40, "vb_batch");
_Static_assert(sizeof(struct vb_watch_take) == 48, "vb_watch_take");
_Static_assert(sizeof(struct vb_copy) == 32, "vb_copy");

#endif
