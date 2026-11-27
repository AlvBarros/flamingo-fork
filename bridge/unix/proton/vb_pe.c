/* Test-only PE half for checking the Unix half under Proton (WS2 owns the real one, bridge/pe/):
 * gets the unix handle in DllMain and forwards victorybridge_call(code, args) to it. */
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
        if (!qvm || !d || qvm(GetCurrentProcess(), inst, 1000 /* MemoryWineUnixFuncs */, &handle, sizeof handle, NULL))
            handle = 0;
        else
            dispatch = *d;
    }
    return TRUE;
}

__declspec(dllexport) LONG WINAPI victorybridge_call(unsigned int code, void *args)
{
    return handle ? dispatch(handle, code, args) : (LONG)0xC0000135; /* STATUS_DLL_NOT_FOUND */
}
