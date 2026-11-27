/* The memory bridge's PE half (WS2): a Wine builtin DLL (`winebuild
 * --builtin`) whose one export forwards a call to the Unix half
 * (`victorybridge.so`, bridge/unix/) through Wine's unix-call dispatcher.
 *
 * ABI: crates/victory-bridge/src/abi.rs (the authority) and ../unix/abi.h.
 * The Rust side (victory_bridge::loader) takes the export as
 * `unsafe extern "system" fn(u32, *mut c_void) -> i32`: on x86_64 WINAPI and
 * "system" are the one Windows x64 convention, NTSTATUS is a 32-bit LONG.
 *
 * Without a Unix half (Wine did not find victorybridge.so: WINEDLLPATH must
 * name its directory, docs/memory-bridge-plan.md S1/S2/S4), or not under
 * Wine at all, every call returns STATUS_NOT_SUPPORTED and the Rust side
 * reports the bridge unavailable. Call numbers past the table are refused
 * here: Wine's dispatcher indexes the Unix half's table unchecked. */
#include <windows.h>
#include "../unix/abi.h"

#define VB_STATUS_NOT_SUPPORTED ((LONG)0xC00000BB)
#define VB_STATUS_INVALID_PARAMETER ((LONG)0xC000000D)
/* NtQueryVirtualMemory's information class for a builtin's unix handle. */
#define VB_MEMORY_WINE_UNIX_FUNCS 1000

typedef LONG (WINAPI *query_fn)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *dispatch_fn)(UINT64, unsigned int, void *);

static UINT64 handle;
static dispatch_fn dispatch;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    query_fn query = nt ? (query_fn)(void *)GetProcAddress(nt, "NtQueryVirtualMemory") : NULL;
    /* A variable in ntdll holding the dispatcher's address (Wine >= 7.x). */
    dispatch_fn *d = nt ? (dispatch_fn *)(void *)GetProcAddress(nt, "__wine_unix_call_dispatcher") : NULL;
    UINT64 h = 0;
    if (query && d && *d &&
        query(GetCurrentProcess(), inst, VB_MEMORY_WINE_UNIX_FUNCS, &h, sizeof h, NULL) == 0 && h) {
        dispatch = *d;
        handle = h;
    }
    return TRUE; /* loads either way: the Rust side asks VB_VERSION_CALL first */
}

/* abi.rs ENTRY: the Unix half's status (0 = run), or nonzero without one. */
__declspec(dllexport) LONG WINAPI victorybridge_call(UINT32 code, void *args)
{
    if (!handle)
        return VB_STATUS_NOT_SUPPORTED;
    if (code >= VB_CALLS)
        return VB_STATUS_INVALID_PARAMETER;
    return dispatch(handle, code, args);
}
