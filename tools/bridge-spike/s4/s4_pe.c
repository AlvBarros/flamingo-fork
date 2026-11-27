/* S4 spike, PE half (builtin DLL): one thin export over __wine_unix_call. */
#include <windows.h>
typedef LONG (WINAPI *QVM)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *DISPATCH)(UINT64, unsigned int, void *);
static UINT64 handle;
static DISPATCH dispatch;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        QVM qvm = (QVM)GetProcAddress(nt, "NtQueryVirtualMemory");
        DISPATCH *d = (DISPATCH *)GetProcAddress(nt, "__wine_unix_call_dispatcher");
        if (!qvm || !d || qvm(GetCurrentProcess(), inst, 1000 /* MemoryWineUnixFuncs */, &handle, sizeof handle, NULL)) handle = 0;
        else dispatch = *d;
    }
    return TRUE;
}

/* What WS2's exports would be: the status of the unix function, or -1 without the Unix half. */
__declspec(dllexport) LONG WINAPI s4_call(unsigned int code, void *args)
{
    return handle ? dispatch(handle, code, args) : -1;
}
