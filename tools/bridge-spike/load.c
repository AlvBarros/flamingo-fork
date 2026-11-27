#include <windows.h>
#include <stdio.h>
typedef double (WINAPI *CYC)(void *, int);
typedef int (WINAPI *PID)(void);
int main(void) {
    HMODULE m = LoadLibraryA("bridge.dll");
    if (!m) { printf("LoadLibrary(bridge.dll) failed: %lu\n", GetLastError()); return 1; }
    CYC cyc = (CYC)GetProcAddress(m, "bridge_map_cycles");
    PID pid = (PID)GetProcAddress(m, "bridge_pid");
    if (!cyc || !pid) { printf("exports missing\n"); return 1; }
    printf("bridge loaded; Linux pid seen from it: %d\n", pid());
    void *at = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_NOACCESS);
    double r = cyc(at, 2000);
    if (r < 0) printf("native map failed: %.0f\n", r);
    else printf("native map+unmap through the bridge: %.2f us per cycle\n", r);
    return 0;
}
