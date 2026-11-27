/* S2 spike, D3D12 part: vkd3d-proton copying from bridge views, and OpenExistingHeapFromAddress on one. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

#define MB (1024 * 1024)
typedef int (*POOL)(UINT64);
typedef int (*MAP)(void *, UINT64, UINT64, int);
typedef int (*PROTECT)(void *, UINT64, int);
typedef void (*MAPS)(void *, char *, int);
typedef HRESULT (WINAPI *CREATE)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static int fails;
static void check(int ok, const char *what) { printf("  [%s] %s\n", ok ? "ok" : "FAIL", what); if (!ok) fails++; }

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp = { .Type = type };
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1,
                              .MipLevels = 1, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) printf("    CreateCommittedResource(type %d) = %08lx\n", type, hr);
    return r;
}
static void submit(void)
{
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, e);
    WaitForSingleObject(e, 10000);
    CloseHandle(e);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}
static int readback_equals(ID3D12Resource *rb, const unsigned char *want, UINT64 size)
{
    void *p = NULL;
    D3D12_RANGE whole = { 0, size };
    if (FAILED(ID3D12Resource_Map(rb, 0, &whole, &p))) return 0;
    int same = memcmp(p, want, size) == 0;
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Unmap(rb, 0, &none);
    return same;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryA("s2bridge.dll");
    if (!m) { printf("no s2bridge.dll\n"); return 1; }
    POOL b_pool = (POOL)GetProcAddress(m, "s2_pool");
    MAP b_map = (MAP)GetProcAddress(m, "s2_map");
    PROTECT b_protect = (PROTECT)GetProcAddress(m, "s2_protect");
    MAPS b_maps = (MAPS)GetProcAddress(m, "s2_maps");
    if (b_pool(16 * MB) < 0) { printf("pool failed\n"); return 1; }
    unsigned char *win = VirtualAlloc(NULL, 16 * MB, MEM_RESERVE, PAGE_NOACCESS);
    /* 1 MiB as 16 views of 64 KiB, in shuffled pool order, filled, then read-only */
    for (int i = 0; i < 16; i++) b_map(win + i * 65536, 65536, (UINT64)((i * 7) % 16) * 65536, 3);
    for (int i = 0; i < MB; i++) win[i] = (unsigned char)(i * 131 + (i >> 16));
    b_protect(win, MB, 1);
    /* a second 1 MiB, RW, for the heap from address */
    unsigned char *win2 = win + 4 * MB;
    b_map(win2, MB, 2 * MB, 3);
    for (int i = 0; i < MB; i++) win2[i] = (unsigned char)(i * 7 + 3);

    HMODULE d3d = LoadLibraryA("d3d12.dll");
    CREATE create = d3d ? (CREATE)GetProcAddress(d3d, "D3D12CreateDevice") : NULL;
    if (!create || FAILED(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) { printf("no D3D12 device\n"); return 1; }
    char name[MAX_PATH];
    GetModuleFileNameA(d3d, name, sizeof name);
    printf("d3d12 from %s\n", name);
    D3D12_COMMAND_QUEUE_DESC qd = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);

    printf("\n== 4a. upload buffer filled by memcpy from bridge views -> default -> readback\n");
    ID3D12Resource *up = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, MB);
    ID3D12Resource *def = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, MB);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, MB);
    void *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Map(up, 0, &none, &p);
    memcpy(p, win, MB);
    ID3D12Resource_Unmap(up, 0, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, def, 0, up, 0, MB);
    barrier(def, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, def, 0, MB);
    submit();
    check(readback_equals(rb, win, MB), "1 MiB from 16 read-only bridge views: through upload/default/readback, bytes equal");

    printf("\n== 4b. ID3D12Device3::OpenExistingHeapFromAddress\n");
    ID3D12Device3 *dev3 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void **)&dev3))) { printf("no ID3D12Device3\n"); return 1; }
    unsigned char *ctl = VirtualAlloc(NULL, MB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < MB; i++) ctl[i] = (unsigned char)(i * 7 + 3);
    struct { const char *what; unsigned char *at; } cases[] = { { "VirtualAlloc'd (control)", ctl }, { "bridge RW view", win2 } };
    for (int c = 0; c < 2; c++) {
        ID3D12Heap *heap = NULL;
        HRESULT hr = ID3D12Device3_OpenExistingHeapFromAddress(dev3, cases[c].at, &IID_ID3D12Heap, (void **)&heap);
        printf("    %s %p: hr=%08lx heap=%p\n", cases[c].what, cases[c].at, hr, (void *)heap);
        if (FAILED(hr)) continue;
        D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = MB, .Height = 1, .DepthOrArraySize = 1,
                                  .MipLevels = 1, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                  .Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER };
        ID3D12Resource *placed = NULL;
        hr = ID3D12Device_CreatePlacedResource(dev, heap, 0, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed);
        printf("      CreatePlacedResource = %08lx\n", hr);
        if (FAILED(hr)) { ID3D12Heap_Release(heap); continue; }
        ID3D12Resource *rb2 = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, MB);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb2, 0, placed, 0, MB);
        submit();
        int same = readback_equals(rb2, cases[c].at, MB);
        printf("      GPU copy out of the heap equals the memory: %d\n", same);
        char line[512];
        b_maps(cases[c].at, line, sizeof line);
        printf("      maps: %s\n", line);
        ID3D12Resource_Release(rb2);
        ID3D12Resource_Release(placed);
        ID3D12Heap_Release(heap);
    }
    printf("\n%d check(s) failed\n", fails);
    return fails != 0;
}
