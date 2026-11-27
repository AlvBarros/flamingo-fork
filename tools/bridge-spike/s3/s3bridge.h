/* The S3 bridge as the Windows test programs see it (loaded from s3bridge.dll beside them). */
#ifndef S3BRIDGE_H
#define S3BRIDGE_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef int (WINAPI *S3_POOL)(uint64_t);
typedef int (WINAPI *S3_MAP)(void *, uint64_t, int, uint64_t);
typedef int (WINAPI *S3_UNMAP)(void *, uint64_t);
static S3_POOL s3_pool;
static S3_MAP s3_map;
static S3_UNMAP s3_unmap;

static int s3_load(void)
{
    HMODULE m = LoadLibraryA("s3bridge.dll");
    if (!m) { printf("LoadLibrary(s3bridge.dll) failed: %lu\n", GetLastError()); return 0; }
    s3_pool = (S3_POOL)GetProcAddress(m, "s3_pool");
    s3_map = (S3_MAP)GetProcAddress(m, "s3_map");
    s3_unmap = (S3_UNMAP)GetProcAddress(m, "s3_unmap");
    return s3_pool && s3_map && s3_unmap;
}

static const char *state_name(DWORD s)
{
    return s == MEM_COMMIT ? "MEM_COMMIT" : s == MEM_RESERVE ? "MEM_RESERVE" : s == MEM_FREE ? "MEM_FREE" : "?";
}

/* What Wine says about an address (the bridge's pages are invisible to it). */
static void say_query(const char *what, const void *at)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(at, &mbi, sizeof mbi)) { printf("  VirtualQuery(%s %p) failed: %lu\n", what, at, GetLastError()); return; }
    printf("  VirtualQuery(%s %p): base %p alloc base %p size %#zx state %s protect %#lx type %#lx\n", what, at,
           mbi.BaseAddress, mbi.AllocationBase, (size_t)mbi.RegionSize, state_name(mbi.State), mbi.Protect, mbi.Type);
}
#endif
