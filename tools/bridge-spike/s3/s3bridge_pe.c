/* S3 bridge, Windows half: finds its Linux half through ntdll (as tools/bridge-spike/bridge_pe.c)
 * and exports one thin function per Unix entry. */
#include <windows.h>
#include <stdint.h>
typedef LONG (WINAPI *QVM)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *DISPATCH)(UINT64, unsigned int, void *);
static UINT64 handle;
static DISPATCH *dispatcher;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        QVM qvm = (QVM)GetProcAddress(nt, "NtQueryVirtualMemory");
        dispatcher = (DISPATCH *)GetProcAddress(nt, "__wine_unix_call_dispatcher");
        if (!qvm || !dispatcher || qvm(GetCurrentProcess(), inst, 1000 /* MemoryWineUnixFuncs */, &handle, sizeof handle, NULL))
            handle = 0;
    }
    return TRUE;
}

struct pool_args { uint64_t bytes; int fd; };
struct map_args { void *at; uint64_t len; int fd; uint64_t offset; };
struct unmap_args { void *at; uint64_t len; };

/* Returns the memfd (a Linux descriptor, opaque here), or a negative status (-1000: no Unix half). */
__declspec(dllexport) int WINAPI s3_pool(uint64_t bytes)
{
    struct pool_args a = { bytes, -1 };
    LONG s;
    if (!handle) return -1000;
    if ((s = (*dispatcher)(handle, 0, &a))) return -(int)s;
    return a.fd;
}
__declspec(dllexport) int WINAPI s3_map(void *at, uint64_t len, int fd, uint64_t offset)
{
    struct map_args a = { at, len, fd, offset };
    return handle ? (int)(*dispatcher)(handle, 1, &a) : -1;
}
__declspec(dllexport) int WINAPI s3_unmap(void *at, uint64_t len)
{
    struct unmap_args a = { at, len };
    return handle ? (int)(*dispatcher)(handle, 2, &a) : -1;
}
