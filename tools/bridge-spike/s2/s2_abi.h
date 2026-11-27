/* S2 spike: the unix-call ABI shared by the PE half and the Unix half. */
#ifndef S2_ABI_H
#define S2_ABI_H
#include <stdint.h>
enum { S2_POOL, S2_MAP, S2_UNMAP, S2_PROTECT, S2_MAPS, S2_SLEEP, S2_PID, S2_READV };
/* prot: Linux PROT_* bits (1 read, 2 write, 4 exec). */
struct s2_pool_args { uint64_t bytes; int32_t fd; };
struct s2_map_args { uint64_t at, len, off; int32_t prot; int32_t err; };
struct s2_maps_args { uint64_t at; char line[512]; };  /* the /proc/self/maps line holding at */
struct s2_sleep_args { uint32_t ms; };
struct s2_readv_args { uint64_t dst, src, len; int64_t got; int32_t err; }; /* process_vm_readv on ourselves */
#endif
