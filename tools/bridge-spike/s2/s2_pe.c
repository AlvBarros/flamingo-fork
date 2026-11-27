/* S2 spike, Windows half (builtin): thin exports over the Unix half. */
#include <windows.h>
#include "s2_abi.h"
typedef LONG (WINAPI *QVM)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *DISPATCH)(UINT64, unsigned int, void *);
static UINT64 handle;
static DISPATCH *dispatcher;
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        QVM qvm = (QVM)GetProcAddress(nt, "NtQueryVirtualMemory");
        dispatcher = (DISPATCH *)GetProcAddress(nt, "__wine_unix_call_dispatcher");
        if (!qvm || !dispatcher || qvm(GetCurrentProcess(), inst, 1000 /* MemoryWineUnixFuncs */, &handle, sizeof handle, NULL)) handle = 0;
    }
    return TRUE;
}
static LONG call(unsigned code, void *args) { return handle ? (*dispatcher)(handle, code, args) : -1; }
__declspec(dllexport) int s2_ready(void) { return handle != 0; }
__declspec(dllexport) int s2_pool(UINT64 bytes) { struct s2_pool_args a = { bytes, -1 }; return call(S2_POOL, &a) ? -1 : a.fd; }
__declspec(dllexport) int s2_map(void *at, UINT64 len, UINT64 off, int prot)
{ struct s2_map_args a = { (UINT64)at, len, off, prot, 0 }; return call(S2_MAP, &a) ? (a.err ? a.err : -1) : 0; }
__declspec(dllexport) int s2_unmap(void *at, UINT64 len)
{ struct s2_map_args a = { (UINT64)at, len, 0, 0, 0 }; return call(S2_UNMAP, &a) ? (a.err ? a.err : -1) : 0; }
__declspec(dllexport) int s2_protect(void *at, UINT64 len, int prot)
{ struct s2_map_args a = { (UINT64)at, len, 0, prot, 0 }; return call(S2_PROTECT, &a) ? (a.err ? a.err : -1) : 0; }
__declspec(dllexport) void s2_maps(void *at, char *out, int n)
{ struct s2_maps_args a = { (UINT64)at }; call(S2_MAPS, &a); lstrcpynA(out, a.line, n); }
__declspec(dllexport) void s2_sleep(DWORD ms) { struct s2_sleep_args a = { ms }; call(S2_SLEEP, &a); }
__declspec(dllexport) int s2_pid(void) { int p = -1; call(S2_PID, &p); return p; }
__declspec(dllexport) INT64 s2_readv(void *dst, const void *src, UINT64 len, int *err)
{ struct s2_readv_args a = { (UINT64)dst, (UINT64)src, len, 0, 0 }; call(S2_READV, &a); *err = a.err; return a.got; }
