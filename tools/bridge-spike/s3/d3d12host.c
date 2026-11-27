/* S3, D3D12 part: the in-place heaps of crates/victory-gpu/src/inplace.rs
 * (ID3D12Device3::OpenExistingHeapFromAddress, ID3D12Device13::OpenExistingHeapFromAddress1) on
 * memory the S3 bridge placed inside a range Wine reserved, under Proton's vkd3d-proton.
 *
 * vkd3d-proton falls back to a plain host-visible allocation when the host-pointer import fails
 * (memory.c, vkd3d_import_host_memory), and the heap is then created anyway; so every opened heap
 * is checked: a buffer placed in it, the GPU copies out what the CPU wrote (through another view)
 * and copies in bytes the CPU must then see. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "s3bridge.h"

#define LEN (4u << 20)
#define Q (LEN / 4)

/* ID3D12Device13 (not in MinGW's d3d12.h): one method after ID3D12Device12's, slot 81 of the
 * vtable (counted from vkd3d-proton's d3d12_device_vtbl). */
static const GUID IID_ID3D12Device13_ = { 0x14eecffc, 0x4df8, 0x40f7, { 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e } };
typedef HRESULT (STDMETHODCALLTYPE *OPEN1)(void *, const void *, SIZE_T, REFIID, void **);

static ID3D12Device *dev;
static ID3D12Device3 *dev3;
static void *dev13;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE event;
static ID3D12Resource *upload, *readback;
static int fails;

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    return d;
}

static ID3D12Resource *committed(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC d = buffer_desc(size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) printf("CreateCommittedResource: %#lx\n", hr);
    return r;
}

static int init(void)
{
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) { printf("D3D12CreateDevice: %#lx\n", hr); return 0; }
    hr = ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void **)&dev3);
    printf("ID3D12Device3: %s\n", SUCCEEDED(hr) ? "yes" : "no");
    hr = ID3D12Device_QueryInterface(dev, &IID_ID3D12Device13_, &dev13);
    printf("ID3D12Device13 (OpenExistingHeapFromAddress1): %s\n", SUCCEEDED(hr) ? "yes" : "no");
    if (FAILED(hr)) dev13 = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)) ||
        FAILED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc)) ||
        FAILED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list)) ||
        FAILED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence))) {
        printf("queue/list/fence failed\n"); return 0;
    }
    ID3D12GraphicsCommandList_Close(list);
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    upload = committed(D3D12_HEAP_TYPE_UPLOAD, Q, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = committed(D3D12_HEAP_TYPE_READBACK, Q, D3D12_RESOURCE_STATE_COPY_DEST);
    return upload && readback;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static void pattern(uint32_t *w, uint32_t seed) { for (size_t i = 0; i < Q / 4; i++) w[i] = (uint32_t)(seed + i * 2654435761u); }
static size_t differ(const uint32_t *w, uint32_t seed)
{
    size_t bad = 0;
    for (size_t i = 0; i < Q / 4; i++) bad += w[i] != (uint32_t)(seed + i * 2654435761u);
    return bad;
}

/* Placed buffer over the whole heap: GPU copies [0,Q) out (must be what the CPU wrote through
 * `cpu`), copies upload bytes into [Q,2Q) (the CPU must see them through `cpu`), [3Q,4Q) untouched. */
static int verify(ID3D12Heap *heap, uint8_t *cpu, void (*between)(void))
{
    ID3D12Resource *placed = NULL;
    D3D12_RESOURCE_DESC d = buffer_desc(LEN, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = ID3D12Device_CreatePlacedResource(dev, heap, 0, &d, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed);
    if (FAILED(hr)) { printf("    CreatePlacedResource: %#lx\n", hr); return 0; }
    if (between) between();
    int ok = 1;
    for (int round = 1; round <= 2 && ok; round++) {
        uint32_t seed_cpu = round * 0x1111111u, seed_gpu = round * 0x7777777u;
        pattern((uint32_t *)cpu, seed_cpu);
        memset(cpu + Q, 0, 2 * Q);
        memset(cpu + 3 * Q, 0x5e, Q);
        void *m; D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Map(upload, 0, &none, &m); pattern(m, seed_gpu); ID3D12Resource_Unmap(upload, 0, NULL);

        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        transition(placed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, placed, 0, Q);
        transition(placed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, placed, Q, upload, 0, Q);
        transition(placed, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
        ID3D12Fence_SetEventOnCompletion(fence, fence_value, event);
        WaitForSingleObject(event, 10000);

        D3D12_RANGE all = { 0, Q };
        ID3D12Resource_Map(readback, 0, &all, &m);
        size_t bad_out = differ(m, seed_cpu);
        ID3D12Resource_Unmap(readback, 0, &none);
        size_t bad_in = differ((uint32_t *)(cpu + Q), seed_gpu);
        size_t bad_keep = 0;
        for (size_t i = 0; i < Q; i++) bad_keep += cpu[3 * Q + i] != 0x5e;
        printf("    round %d: GPU read the CPU's bytes %s; CPU saw the GPU's write %s; untouched quarter %s\n", round,
               bad_out ? "NO (heap is not this memory)" : "ok", bad_in ? "NO" : "ok", bad_keep ? "CHANGED" : "ok");
        ok = !bad_out && !bad_in && !bad_keep;
    }
    ID3D12Resource_Release(placed);
    return ok;
}

/* how: 0 OpenExistingHeapFromAddress(at), 1 OpenExistingHeapFromAddress1(at, LEN). */
static void d3d_case(const char *name, int how, void *at, void *cpu, void (*between)(void))
{
    ID3D12Heap *heap = NULL;
    HRESULT hr;
    printf("case %s, %s(%p), CPU view %p\n", name, how ? "OpenExistingHeapFromAddress1" : "OpenExistingHeapFromAddress", at, cpu);
    if (how == 0) hr = ID3D12Device3_OpenExistingHeapFromAddress(dev3, at, &IID_ID3D12Heap, (void **)&heap);
    else if (!dev13) { printf("    RESULT: no ID3D12Device13\n"); fails++; return; }
    else hr = ((OPEN1)(*(void ***)dev13)[81])(dev13, at, LEN, &IID_ID3D12Heap, (void **)&heap);
    if (FAILED(hr)) { printf("    open: %#lx\n    RESULT %s: REFUSED\n", hr, name); fails++; return; }
    D3D12_HEAP_DESC hd = ID3D12Heap_GetDesc(heap);
    printf("    open: ok, heap %llu bytes, flags %#x\n", (unsigned long long)hd.SizeInBytes, hd.Flags);
    int ok = verify(heap, cpu, between);
    ID3D12Heap_Release(heap);
    printf("    RESULT %s: %s\n", name, ok ? "OK (heap is this memory)" : "FAILED");
    fails += !ok;
}

static uint8_t *unmap_at;
static void unmap_view(void) { printf("    imported view unmapped by the bridge (status %d)\n", s3_unmap(unmap_at, LEN)); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!s3_load() || !init()) return 1;

    /* baseline: memory Wine allocated and committed (one allocation, as inplace needs) */
    uint8_t *wine_mem = VirtualAlloc(NULL, LEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    d3d_case("wine-committed", 0, wine_mem, wine_mem, NULL);
    d3d_case("wine-committed", 1, wine_mem, wine_mem, NULL);

    int fd = s3_pool(2 * LEN);
    uint8_t *win = VirtualAlloc(NULL, 4 * LEN, MEM_RESERVE, PAGE_NOACCESS);
    if (fd < 0 || !win || s3_map(win, LEN, fd, 0) || s3_map(win + LEN, LEN, fd, 0) || s3_map(win + 2 * LEN, LEN, fd, LEN)) {
        printf("bridge map failed\n"); return 1;
    }
    say_query("bridge view at the window's base", win);
    say_query("bridge view inside the window", win + 2 * LEN);

    /* the bridge's view, at the base of Wine's reservation and inside it */
    d3d_case("bridge view at window base", 0, win, win, NULL);
    d3d_case("bridge view inside window", 0, win + 2 * LEN, win + 2 * LEN, NULL);
    d3d_case("bridge view inside window", 1, win + 2 * LEN, win + 2 * LEN, NULL);
    d3d_case("bridge alias (open view 1, CPU through view 2)", 1, win, win + LEN, NULL);

    /* Wine-committed memory with a bridge view put over it: Wine says MEM_COMMIT */
    uint8_t *over = VirtualAlloc(NULL, LEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int fd2 = s3_pool(LEN);
    if (s3_map(over, LEN, fd2, 0) || s3_map(win + 3 * LEN, LEN, fd2, 0)) { printf("overlay map failed\n"); return 1; }
    say_query("overlay", over);
    d3d_case("bridge view over Wine-committed memory (CPU through a second view)", 0, over, win + 3 * LEN, NULL);
    d3d_case("bridge view over Wine-committed memory (CPU through a second view)", 1, over, win + 3 * LEN, NULL);

    /* the opened view unmapped by the bridge while the heap stands (Wine refuses this for its own
     * views; the bridge cannot be stopped): do the GPU's pages stay the pool's? */
    unmap_at = win;
    d3d_case("bridge alias, opened view unmapped while the heap stands", 1, win, win + LEN, unmap_view);

    printf("\n%d case(s) failed or refused\n", fails);
    return 0;
}
