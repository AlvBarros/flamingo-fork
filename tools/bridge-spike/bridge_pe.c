/* Windows half: finds its Linux half through ntdll and calls it. */
#include <windows.h>
typedef LONG (WINAPI *QVM)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *DISPATCH)(UINT64, unsigned int, void *);
static UINT64 handle; static DISPATCH *dispatcher;
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
struct cycles_args { void *at; int n; double us; };
__declspec(dllexport) double WINAPI bridge_map_cycles(void *at, int n)
{
    struct cycles_args a = { at, n, -1 };
    if (!handle) return -10;
    LONG s = (*dispatcher)(handle, 0, &a);
    return s ? -(double)s : a.us;
}
__declspec(dllexport) int WINAPI bridge_pid(void) { int p = -1; if (handle) (*dispatcher)(handle, 1, &p); return p; }
