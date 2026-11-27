/* Under Proton: loads victorybridge.dll, asks VB_VERSION_CALL, then one round trip through a
 * window reserved with VirtualAlloc: pool, aliased views (two watched), a write from PE code, a
 * take, a copy across an unmapped view, window removal. Prints what it saw; exit 0 when all is
 * as expected. */
#include <windows.h>
#include <stdio.h>
#include "../abi.h"

typedef LONG (WINAPI *CALL)(unsigned int, void *);
static CALL vb;
static int bad;
#define EXPECT(c, what)                                                                         \
    do {                                                                                        \
        if (!(c)) {                                                                             \
            bad++;                                                                              \
            printf("FAIL: %s\n", what);                                                         \
        } else                                                                                  \
            printf("ok: %s\n", what);                                                           \
    } while (0)

int main(void)
{
    HMODULE dll = LoadLibraryA("victorybridge.dll");
    if (!dll || !(vb = (CALL)GetProcAddress(dll, "victorybridge_call"))) {
        printf("FAIL: cannot load victorybridge.dll (%lu)\n", GetLastError());
        return 1;
    }
    struct vb_version v = { 0 };
    LONG s = vb(VB_VERSION_CALL, &v);
    printf("VB_VERSION_CALL: status 0x%lx, version %u, features %llu (watch %s, copy %s)\n", s, v.version,
           (unsigned long long)v.features, v.features & VB_FEATURE_WATCH ? "yes" : "no",
           v.features & VB_FEATURE_COPY ? "yes" : "no");
    EXPECT(s == 0 && v.version == VB_VERSION, "version");
    EXPECT(v.features & VB_FEATURE_COPY, "copy feature");
    EXPECT(v.features & VB_FEATURE_WATCH, "watch feature");

    const UINT64 view = 64 * 1024, len = 4 * view;
    unsigned char *w = VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS), dst[3 * 4096];
    struct vb_pool_create pc = { 2 * view, 0 };
    EXPECT(w && vb(VB_POOL_CREATE, &pc) == 0 && pc.pool, "pool");
    struct vb_window win = { (UINT64)(ULONG_PTR)w, len };
    EXPECT(vb(VB_WINDOW_ADD, &win) == 0, "window over VirtualAlloc(MEM_RESERVE)");
    struct vb_map_op m[3] = {
        { (UINT64)(ULONG_PTR)w, view, 0, 3, VB_MAP_WATCH },
        { (UINT64)(ULONG_PTR)(w + view), view, 0, 3, 0 },
        { (UINT64)(ULONG_PTR)(w + 2 * view), view, view, 3, VB_MAP_WATCH },
    };
    INT32 r[3] = { -1, -1, -1 };
    struct vb_batch b = { pc.pool, (UINT64)(ULONG_PTR)m, 3, (UINT64)(ULONG_PTR)r, 0 };
    EXPECT(vb(VB_MAP, &b) == 0 && b.done == 3 && !r[0] && !r[1] && !r[2], "map 3 views");
    w[5 * 4096 + 3] = 0x5a;
    EXPECT(w[view + 5 * 4096 + 3] == 0x5a, "alias sees the PE write");
    UINT64 pages[8];
    struct vb_watch_take t = { (UINT64)(ULONG_PTR)w, len, (UINT64)(ULONG_PTR)pages, 8, 0, 0 };
    EXPECT(vb(VB_WATCH_TAKE, &t) == 0 && t.count == 1 && pages[0] == (UINT64)(ULONG_PTR)(w + 5 * 4096) && !t.more,
           "take: exactly the written page");
    EXPECT(vb(VB_WATCH_TAKE, &t) == 0 && t.count == 0, "take again: re-armed");
    struct vb_range u = { (UINT64)(ULONG_PTR)(w + 2 * view), view, 0, 0 };
    b = (struct vb_batch){ 0, (UINT64)(ULONG_PTR)&u, 1, (UINT64)(ULONG_PTR)r, 0 };
    EXPECT(vb(VB_UNMAP, &b) == 0 && b.done == 1, "unmap the third view");
    struct vb_copy c = { (UINT64)(ULONG_PTR)dst, (UINT64)(ULONG_PTR)(w + 2 * view - 4096), sizeof dst, 0 };
    EXPECT(vb(VB_COPY, &c) == 0 && c.copied == 4096 && dst[0] == w[2 * view - 4096], "copy stops at the unmapped view");
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(w, &mbi, sizeof mbi);
    printf("Wine's VirtualQuery on the window: state 0x%lx (MEM_RESERVE is 0x2000), as S2 found\n", mbi.State);
    EXPECT(vb(VB_WINDOW_REMOVE, &win) == 0, "window remove");
    EXPECT(VirtualFree(w, 0, MEM_RELEASE), "Wine releases the reservation");
    struct vb_pool_destroy pd = { pc.pool };
    EXPECT(vb(VB_POOL_DESTROY, &pd) == 0, "pool destroy");
    printf("%s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
