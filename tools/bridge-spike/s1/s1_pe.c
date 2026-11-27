/* S1 spike, Windows half: one export that forwards an ABI call to the Linux half. */
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
__declspec(dllexport) LONG WINAPI s1_call(unsigned int code, void *args)
{
    if (!handle) return -1;
    return (*dispatcher)(handle, code, args);
}
