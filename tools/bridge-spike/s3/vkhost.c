/* S3: does VK_EXT_external_memory_host take a memfd-backed MAP_SHARED mapping?
 *
 * Built twice from this one file:
 *   - natively (gcc, libvulkan.so.1): isolates NVIDIA's driver, no Wine;
 *   - for Windows (MinGW, vulkan-1.dll = winevulkan): the same under Proton, with the memory put in
 *     place by the S3 bridge (s3bridge.dll/.so) inside a range Wine reserved.
 *
 * Each case imports a host pointer (`gpu`) into a VkBuffer and checks, through another CPU view of
 * the same pages (`cpu`, an alias or the same address), that the GPU reads what the CPU wrote and
 * the CPU reads what the GPU wrote, twice (the second round catches stale copies). */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "s3bridge.h"
#else
#include <dlfcn.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define LEN (4u << 20)          /* bytes per case */
#define Q (LEN / 4)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDeviceSize host_align;
static int fails;

#define IFN(n) static PFN_##n n
IFN(vkCreateInstance); IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties2);
IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
IFN(vkCreateDevice); IFN(vkEnumerateDeviceExtensionProperties);
IFN(vkGetDeviceQueue); IFN(vkCreateCommandPool); IFN(vkAllocateCommandBuffers); IFN(vkFreeCommandBuffers);
IFN(vkBeginCommandBuffer); IFN(vkEndCommandBuffer); IFN(vkQueueSubmit); IFN(vkQueueWaitIdle);
IFN(vkCreateBuffer); IFN(vkDestroyBuffer); IFN(vkGetBufferMemoryRequirements); IFN(vkAllocateMemory);
IFN(vkFreeMemory); IFN(vkBindBufferMemory); IFN(vkCmdCopyBuffer); IFN(vkCmdFillBuffer);
IFN(vkCmdPipelineBarrier); IFN(vkGetMemoryHostPointerPropertiesEXT);

static const char *flags_str(VkMemoryPropertyFlags f)
{
    static char s[64];
    snprintf(s, sizeof s, "%s%s%s%s", f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DL " : "",
             f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HV " : "", f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "HC " : "",
             f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "HCa " : "");
    return s;
}

static int vk_init(void)
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("no vulkan-1.dll\n"); return 0; }
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("no libvulkan.so.1\n"); return 0; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "s3", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { printf("vkCreateInstance: %d\n", r); return 0; }
#define I(n) n = (PFN_##n)gipa(inst, #n)
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties2); I(vkGetPhysicalDeviceMemoryProperties);
    I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice); I(vkEnumerateDeviceExtensionProperties);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && !phys; i++) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp };
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (p2.properties.vendorID != 0x10de) continue;
        phys = pds[i];
        host_align = hp.minImportedHostPointerAlignment;
        printf("device: %s, driver %u.%u.%u, minImportedHostPointerAlignment %llu\n", p2.properties.deviceName,
               p2.properties.driverVersion >> 22, (p2.properties.driverVersion >> 14) & 0xff, (p2.properties.driverVersion >> 6) & 0xff,
               (unsigned long long)host_align);
    }
    if (!phys) { printf("no NVIDIA device\n"); return 0; }

    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne, sizeof *ext); vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ext);
    int has = 0;
    for (uint32_t i = 0; i < ne; i++) has |= !strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    free(ext);
    printf("VK_EXT_external_memory_host offered: %s\n", has ? "yes" : "NO");
    if (!has) return 0;

    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        printf("  memory type %u: heap %u %s\n", i, memprops.memoryTypes[i].heapIndex, flags_str(memprops.memoryTypes[i].propertyFlags));

    uint32_t nq = 16; VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t fam = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio };
    const char *exts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, exts, NULL };
    if ((r = vkCreateDevice(phys, &dci, NULL, &dev))) { printf("vkCreateDevice: %d\n", r); return 0; }
#define D(n) n = (PFN_##n)gdpa(dev, #n)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkFreeCommandBuffers);
    D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer);
    D(vkDestroyBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkFreeMemory); D(vkBindBufferMemory);
    D(vkCmdCopyBuffer); D(vkCmdFillBuffer); D(vkCmdPipelineBarrier); D(vkGetMemoryHostPointerPropertiesEXT);
    vkGetDeviceQueue(dev, fam, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam };
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    return 1;
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void host_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Q0 -> device buffer -> Q1, fill Q2 with `fill`; Q3 left alone. */
static int round_trip(VkBuffer host, VkBuffer devbuf, uint32_t fill)
{
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (vkAllocateCommandBuffers(dev, &ai, &cb)) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    VkBufferCopy in = { 0, 0, Q }, out = { 0, Q, Q };
    vkCmdCopyBuffer(cb, host, devbuf, 1, &in);
    barrier(cb);
    vkCmdCopyBuffer(cb, devbuf, host, 1, &out);
    vkCmdFillBuffer(cb, host, 2 * Q, Q, fill);
    host_barrier(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (!r) r = vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    if (r) printf("    submit/wait: %d\n", r);
    return r == VK_SUCCESS;
}

static int check(const uint8_t *cpu, uint32_t seed, uint32_t fill, const char *round)
{
    const uint32_t *w = (const uint32_t *)cpu;
    size_t bad_copy = 0, bad_fill = 0, bad_keep = 0;
    for (size_t i = 0; i < Q / 4; i++) {
        bad_copy += w[Q / 4 + i] != (uint32_t)(seed + i * 2654435761u);
        bad_fill += w[2 * Q / 4 + i] != fill;
        bad_keep += w[3 * Q / 4 + i] != 0x5e5e5e5eu;
    }
    int ok = !bad_copy && !bad_fill && !bad_keep;
    printf("    %s: GPU read CPU's bytes and wrote them back %s; GPU fill seen by CPU %s; untouched quarter %s\n", round,
           bad_copy ? "WRONG" : "ok", bad_fill ? "WRONG" : "ok", bad_keep ? "CHANGED" : "ok");
    if (!ok) printf("    (wrong words: copy %zu, fill %zu, kept %zu of %u each)\n", bad_copy, bad_fill, bad_keep, Q / 4);
    return ok;
}

static void cpu_fill(uint8_t *cpu, uint32_t seed)
{
    uint32_t *w = (uint32_t *)cpu;
    for (size_t i = 0; i < Q / 4; i++) w[i] = (uint32_t)(seed + i * 2654435761u);
    memset(cpu + Q, 0, 2 * Q);
    memset(cpu + 3 * Q, 0x5e, Q);
}

/* Import `gpu` (LEN bytes) and check it through `cpu`. `between`, if given, runs after the import
 * and before the GPU work (used to unmap the imported view). */
static int vk_case(const char *name, void *gpu, void *cpu, void (*between)(void))
{
    printf("case %s: import %p, CPU view %p\n", name, gpu, cpu);
    VkMemoryHostPointerPropertiesEXT hpp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, gpu, &hpp);
    printf("    vkGetMemoryHostPointerPropertiesEXT: %d, memoryTypeBits %#x", r, hpp.memoryTypeBits);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if (hpp.memoryTypeBits & (1u << i)) printf(" [%u: %s]", i, flags_str(memprops.memoryTypes[i].propertyFlags));
    printf("\n");
    if (r || !hpp.memoryTypeBits) { printf("    RESULT %s: refused at the properties query\n", name); fails++; return 0; }

    VkExternalMemoryBufferCreateInfo ebci = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebci, 0, LEN,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer host = VK_NULL_HANDLE, devbuf = VK_NULL_HANDLE;
    VkDeviceMemory hmem = VK_NULL_HANDLE, dmem = VK_NULL_HANDLE;
    int ok = 0;
    if ((r = vkCreateBuffer(dev, &bci, NULL, &host))) { printf("    vkCreateBuffer: %d\n", r); goto out; }
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, host, &req);
    uint32_t t = find_type(req.memoryTypeBits & hpp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) t = find_type(req.memoryTypeBits & hpp.memoryTypeBits, 0);
    if (t == UINT32_MAX) { printf("    no type both for the buffer (%#x) and the pointer\n", req.memoryTypeBits); goto out; }
    VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, gpu };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, LEN, t };
    r = vkAllocateMemory(dev, &mai, NULL, &hmem);
    printf("    vkAllocateMemory(import, type %u): %d\n", t, r);
    if (r) goto out;
    if ((r = vkBindBufferMemory(dev, host, hmem, 0))) { printf("    vkBindBufferMemory: %d\n", r); goto out; }

    bci.pNext = NULL; bci.size = Q;
    vkCreateBuffer(dev, &bci, NULL, &devbuf);
    vkGetBufferMemoryRequirements(dev, devbuf, &req);
    VkMemoryAllocateInfo dai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (vkAllocateMemory(dev, &dai, NULL, &dmem) || vkBindBufferMemory(dev, devbuf, dmem, 0)) { printf("    device buffer failed\n"); goto out; }

    if (between) between();
    cpu_fill(cpu, 0x1234567u);
    ok = round_trip(host, devbuf, 0xc0ffee11u) && check(cpu, 0x1234567u, 0xc0ffee11u, "round 1");
    cpu_fill(cpu, 0xabcdef9u);
    ok = ok && round_trip(host, devbuf, 0x0badf00du) && check(cpu, 0xabcdef9u, 0x0badf00du, "round 2");
out:
    if (devbuf) vkDestroyBuffer(dev, devbuf, NULL);
    if (dmem) vkFreeMemory(dev, dmem, NULL);
    if (host) vkDestroyBuffer(dev, host, NULL);
    if (hmem) vkFreeMemory(dev, hmem, NULL);
    printf("    RESULT %s: %s\n", name, ok ? "OK" : "FAILED");
    fails += !ok;
    return ok;
}

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}

/* What one import costs (the driver pins every page): `bytes` at `p`, first with pages never
 * touched, then after the CPU touched them. */
static void import_cost(const char *name, uint8_t *p, size_t bytes)
{
    for (int pass = 0; pass < 2; pass++) {
        if (pass) for (size_t i = 0; i < bytes; i += 4096) p[i] = 1;
        VkMemoryHostPointerPropertiesEXT hpp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        double t0 = now_ms();
        VkResult r = vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hpp);
        double t1 = now_ms();
        VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, bytes,
                                     find_type(hpp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (!r) r = vkAllocateMemory(dev, &mai, NULL, &m);
        double t2 = now_ms();
        if (m) vkFreeMemory(dev, m, NULL);
        double t3 = now_ms();
        printf("import cost, %s, %zu MiB, pages %s: %d; properties %.2f ms, import %.2f ms, free %.2f ms\n", name, bytes >> 20,
               pass ? "touched" : "never touched", r, t1 - t0, t2 - t1, t3 - t2);
    }
}

/* ---- the cases ---- */

static void *unmap_at;
#ifdef _WIN32
static void unmap_view(void) { int s = s3_unmap(unmap_at, LEN); printf("    imported view unmapped by the bridge (status %d)\n", s); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!s3_load()) { printf("s3bridge missing\n"); return 1; }
    if (!vk_init()) return 1;

    /* (a) baseline: memory Wine allocated and committed. */
    void *wine_mem = VirtualAlloc(NULL, LEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    vk_case("wine-committed (VirtualAlloc)", wine_mem, wine_mem, NULL);

    /* The bridge's window: reserved through Wine, pages placed by the bridge's Linux half. */
    int fd = s3_pool(2 * LEN);
    uint8_t *win = VirtualAlloc(NULL, 4 * LEN, MEM_RESERVE, PAGE_NOACCESS);
    printf("bridge pool fd %d, window %p (%u MiB reserved through Wine)\n", fd, win, 4 * LEN >> 20);
    if (fd < 0 || !win || s3_map(win, LEN, fd, 0) || s3_map(win + LEN, LEN, fd, 0) || s3_map(win + 2 * LEN, LEN, fd, LEN)) {
        printf("bridge map failed\n"); return 1;
    }
    say_query("bridge view", win);

    /* (b) a bridge view, CPU through the same address. */
    vk_case("bridge view (memfd MAP_SHARED in a Wine-reserved range)", win + 2 * LEN, win + 2 * LEN, NULL);
    /* (c) import one view, check through the other view of the same pages. */
    vk_case("bridge alias (import view 1, CPU through view 2 of the same memfd offset)", win, win + LEN, NULL);

    /* (d) Wine-committed memory with a bridge view put over it (Wine then reports MEM_COMMIT). */
    uint8_t *over = VirtualAlloc(NULL, LEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int fd2 = s3_pool(LEN);
    if (s3_map(over, LEN, fd2, 0) || s3_map(win + 3 * LEN, LEN, fd2, 0)) { printf("overlay map failed\n"); return 1; }
    say_query("overlay", over);
    vk_case("bridge view over Wine-committed memory (CPU through a second view)", over, win + 3 * LEN, NULL);

    /* (e) the imported view is unmapped by the bridge before the GPU uses it: do the GPU's pages
     * stay the pool's pages (seen through the other alias)? */
    unmap_at = win;
    vk_case("bridge alias, imported view unmapped after import", win, win + LEN, unmap_view);

    const size_t big = 256u << 20;
    import_cost("wine-committed", VirtualAlloc(NULL, big, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), big);
    int fd3 = s3_pool(big);
    uint8_t *win3 = VirtualAlloc(NULL, big, MEM_RESERVE, PAGE_NOACCESS);
    if (fd3 >= 0 && win3 && !s3_map(win3, big, fd3, 0)) import_cost("bridge view", win3, big);

    printf("\n%d case(s) failed\n", fails);
    return fails != 0;
}
#else
static void unmap_view(void)
{
    void *p = mmap(unmap_at, LEN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    printf("    imported view replaced by PROT_NONE anonymous memory: %s\n", p == unmap_at ? "done" : "FAILED");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!vk_init()) return 1;

    /* (a) anonymous private memory: the baseline the extension is made for. */
    void *anon = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    vk_case("anonymous private", anon, anon, NULL);

    /* (b) a memfd mapped shared. */
    int fd = memfd_create("s3", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 2 * LEN)) { perror("memfd"); return 1; }
    uint8_t *view = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LEN);
    vk_case("memfd MAP_SHARED", view, view, NULL);

    /* (c) bridge-like: a reserved PROT_NONE range, two views of memfd offset 0 placed with
     * MAP_FIXED inside it; import view 1, CPU through view 2. */
    uint8_t *win = mmap(NULL, 4 * LEN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mmap(win, LEN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != win ||
        mmap(win + 2 * LEN, LEN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != win + 2 * LEN) { perror("fixed"); return 1; }
    vk_case("memfd alias (MAP_FIXED in a reserved range, CPU through the other view)", win, win + 2 * LEN, NULL);
    /* and the other way round */
    vk_case("memfd alias, reversed", win + 2 * LEN, win, NULL);

    /* (d) the imported view unmapped after import. */
    unmap_at = win;
    vk_case("memfd alias, imported view unmapped after import", win, win + 2 * LEN, unmap_view);

    /* (e) misaligned pointer (alignment rule) */
    if (host_align > 1) {
        VkMemoryHostPointerPropertiesEXT hpp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        VkResult r = vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                         (uint8_t *)view + host_align / 2, &hpp);
        printf("misaligned pointer (+%llu): properties %d bits %#x\n", (unsigned long long)host_align / 2, r, hpp.memoryTypeBits);
    }

    const size_t big = 256u << 20;
    import_cost("anonymous private", mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), big);
    int fd3 = memfd_create("s3-big", MFD_CLOEXEC);
    if (fd3 >= 0 && !ftruncate(fd3, big))
        import_cost("memfd MAP_SHARED", mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_SHARED, fd3, 0), big);
    printf("\n%d case(s) failed\n", fails);
    return fails != 0;
}
#endif
