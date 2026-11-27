/* Loads the memory bridge the way victory_bridge::loader::entry() does and
 * asks its version (bridge/pe/test/run.sh runs it under Proton).
 *
 *   load.exe [beside|name] [expect-on|expect-off|expect-missing]
 *
 * beside (default): LoadLibraryW(<this exe's directory>\victorybridge.dll);
 * name: LoadLibraryW(L"victorybridge.dll") (the loader's fallback: the DLL
 * search path; Wine does not look on WINEDLLPATH for it). expect-on
 * (default): exit 0 only when the VERSION call succeeds with VB_VERSION and a
 * call past the table is refused; expect-off: exit 0 only when the DLL loads
 * but every call is refused (no Unix half found: WINEDLLPATH not set);
 * expect-missing: exit 0 only when the DLL does not load. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "abi.h"

typedef LONG (WINAPI *entry_fn)(UINT32, void *);

int main(int argc, char **argv)
{
    int by_name = argc > 1 && !strcmp(argv[1], "name");
    const char *expect = argc > 2 ? argv[2] : "expect-on";
    int expect_on = !strcmp(expect, "expect-on");
    int expect_missing = !strcmp(expect, "expect-missing");

    HMODULE nt = GetModuleHandleA("ntdll.dll");
    FARPROC wine = nt ? GetProcAddress(nt, "wine_get_version") : NULL;
    printf("under Wine: %s\n", wine ? ((const char *(*)(void))(void *)wine)() : "no");

    WCHAR path[MAX_PATH + 32];
    if (by_name) {
        wcscpy(path, L"victorybridge.dll");
    } else {
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (!n || n >= MAX_PATH) { printf("FAIL: GetModuleFileNameW\n"); return 1; }
        WCHAR *slash = wcsrchr(path, L'\\');
        wcscpy(slash ? slash + 1 : path, L"victorybridge.dll");
    }
    printf("LoadLibraryW(%ls)\n", path);
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        DWORD e = GetLastError();
        printf("%s: LoadLibraryW: error %lu\n", expect_missing ? "PASS (not found, as expected)" : "FAIL", e);
        return expect_missing ? 0 : 1;
    }
    if (expect_missing) { printf("FAIL: expected the DLL not to load\n"); return 1; }
    WCHAR loaded[MAX_PATH];
    if (GetModuleFileNameW(m, loaded, MAX_PATH)) printf("loaded: %ls\n", loaded);

    entry_fn entry = (entry_fn)(void *)GetProcAddress(m, "victorybridge_call");
    if (!entry) { printf("FAIL: no export victorybridge_call\n"); return 1; }

    struct vb_version v = { 0, 0, 0 };
    LONG status = entry(VB_VERSION_CALL, &v);
    printf("VERSION: status 0x%08lx, version %u (want %u), features %llu, Unix-side pid %u (ours %lu)\n",
           (unsigned long)status, v.version, VB_VERSION, (unsigned long long)v.features, v.pad,
           GetCurrentProcessId());
    LONG past = entry(VB_CALLS, &v);
    printf("call %u (past the table): status 0x%08lx\n", VB_CALLS, (unsigned long)past);
    struct vb_pool_create pc = { 1 << 20, 0 };
    LONG pool = entry(VB_POOL_CREATE, &pc);
    printf("POOL_CREATE: status 0x%08lx\n", (unsigned long)pool);

    int on = status == 0 && v.version == VB_VERSION;
    if (expect_on) {
        if (!on || past == 0) { printf("FAIL: expected the bridge on\n"); return 1; }
        printf("PASS: bridge on\n");
    } else {
        if (status == 0 || past == 0 || pool == 0) { printf("FAIL: expected every call refused\n"); return 1; }
        printf("PASS: bridge loads, no Unix half, every call refused\n");
    }
    return 0;
}
