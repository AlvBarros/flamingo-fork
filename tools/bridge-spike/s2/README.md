# Spike S2: Wine's behaviour on the bridge's window

What Proton Experimental's Wine (wine-11.0 base, 2026-11-27) does with pages that a builtin's Unix
half maps (`mmap MAP_FIXED|MAP_SHARED` of a memfd), unmaps (`MAP_PRIVATE|MAP_ANONYMOUS, PROT_NONE`)
and `mprotect`s inside a range Wine holds as `VirtualAlloc(MEM_RESERVE)`. Question and decisions:
`docs/memory-bridge-plan.md` §5 (S2) and §10 (S2 results).

- `s2_unix.c` / `s2_pe.c` / `s2_abi.h`: the builtin `s2bridge.dll` + `s2bridge.so` (pool, map,
  unmap, protect, a `/proc/self/maps` line, a sleep inside the Unix call, `process_vm_readv`).
- `s2.c`: faults and the vectored handler, safecopy's `rep movsb` pattern, `VirtualQuery`,
  Read/WriteProcessMemory and the Nt calls, file I/O, write watch, thread suspension, and
  `VirtualProtect` / `VirtualAlloc(MEM_COMMIT)` / `VirtualFree` on the window.
- `s2d3d.c`: vkd3d-proton copying from bridge views; `OpenExistingHeapFromAddress` on one.

Run (builds into `target/bridge-spike-s2`, makes its own prefix there, stops only that prefix's
wineserver; do not run while the game uses the GPU):

    tools/bridge-spike/s2/build.sh            # logs: target/bridge-spike-s2/s2.log, s2d3d.log
    WINEDEBUG=+seh,+virtual tools/bridge-spike/s2/build.sh   # to see what Wine does on each call

Two things the run needs that the first spike's README did not say: Wine finds a builtin's Unix
half only through `WINEDLLPATH` (the directory of `s2bridge.so`; without it `__wine_unix_call`
has no handle), and vkd3d-proton's `d3d12core.dll` imports `dxgi.dll`, so DXVK's `dxgi.dll` goes
into the prefix with `dxgi=n,b`, as Proton's prefixes have it.

## Results (all checks pass)

| Operation on a bridge page | What Wine does |
|---|---|
| write to a page `mprotect`ed read-only | one `EXCEPTION_ACCESS_VIOLATION` straight to the first vectored handler, `info[0]=1`, `info[1]` = exact byte; nothing before it (`+seh`: no guard/stack/write-watch handling). Handler `mprotect`s via the Unix half, `CONTINUE_EXECUTION`: the write lands. Same from a second thread, from 8 threads at once, and with RIP inside the window |
| read of an unmapped (PROT_NONE) page, or of a never-mapped window page | AV, `info[0]=0`; handler maps the view, the read sees the pool |
| safecopy's `rep movsb` | out of a mapped view: no fault; into an unmapped page: AV at the first missing byte, failing exit works; into a read-only page: handler opens it, copy completes |
| `VirtualQuery` / `NtQueryVirtualMemory` | `MEM_RESERVE`, `Protect=0`, `AllocationProtect=PAGE_NOACCESS`, region = rest of the window, whatever is mapped. `MemoryWorkingSetExInformation`: not valid |
| `ReadProcessMemory` / `NtReadVirtualMemory` (own process) | read the real pages (memcpy under Wine's syscall fault handler): mapped RW/R views succeed with the right bytes; unmapped fails (`ERROR_PARTIAL_COPY` / `STATUS_PARTIAL_COPY`); destination in the window works |
| `WriteProcessMemory` | always fails `ERROR_NOACCESS` (998): kernelbase asks `VirtualQuery` first and sees reserved, even for an RW view |
| `NtWriteVirtualMemory` | real pages: RW view succeeds and lands; read-only and unmapped fail `STATUS_PARTIAL_COPY` |
| `process_vm_readv` (Unix half) | RW/R views read; unmapped `EFAULT` |
| `WriteFile` from / `ReadFile` into a view | mapped views work, data right; unmapped source `ERROR_INVALID_USER_BUFFER`, read-only/unmapped destination `ERROR_NOACCESS`; no fault reaches the handler; nothing remapped |
| `IsBadReadPtr` / `IsBadWritePtr` | probe for real (follow the actual protection; the handler sees the probe's AV first) |
| memcpy on 6 threads | fine |
| `GetWriteWatch` | window without `MEM_WRITE_WATCH`: `ERROR_INVALID_PARAMETER`; a `MEM_WRITE_WATCH` reserve with a bridge view written: 0 pages (Wine sees nothing) |
| `VirtualProtect` on the window | fails `ERROR_INVALID_ADDRESS` (not committed), view untouched |
| `VirtualAlloc(MEM_RESERVE)` at a window address | fails (already reserved) |
| `VirtualAlloc(MEM_COMMIT)` on a view | **succeeds**: Wine marks the pages committed and `mprotect`s our mapping (data kept); from then on Wine's `VirtualProtect` there succeeds and `mprotect`s our memfd mapping |
| `VirtualFree(MEM_DECOMMIT)` on a view | **succeeds and replaces our view with anonymous PROT_NONE** |
| `VirtualFree(MEM_RELEASE)` of the window | **succeeds and munmaps every view**; the next `VirtualAlloc` can be placed there |
| `SuspendThread` / `GetThreadContext` on a thread inside the Unix call | answer at once; context is the PE side (the dispatcher's return). The thread is frozen **inside** the Unix call (signal), its syscalls see `EINTR`, and it goes on after the resume |
| D3D12: upload buffer filled by memcpy from 16 read-only bridge views, copied to default and read back | bytes equal |
| `ID3D12Device3::OpenExistingHeapFromAddress` on a bridge view | `E_INVALIDARG` (vkd3d-proton's `VirtualQuery` sees reserved); the control on `VirtualAlloc`'d memory works |
