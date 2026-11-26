# Running Flamingo natively on Linux (no Wine): what it would take

Status: plan, 2026-11-26. Nothing here is implemented yet except where stated.

## Why

Measured on this machine (RTX 4070, i5-14400F, kernel 6.18 LTS), the layer's bookkeeping of title
memory costs far more under Wine than natively, and the gap grows with the number of views the
title maps (about 60,000 single-page views in the opening cutscene):

| Operation at 60,000 views | Proton's Wine | Native Linux |
|---|---|---|
| Map + unmap a 64 KiB view | 337 µs (16 µs with no views) | 1.9 µs |
| Protection change, 6 other threads busy | 6.4 µs | 0.15 µs |
| In the game: remapping per heavy cutscene frame | 0.8-1.7 s (~2,100 Windows calls) | — |

Wine's view bookkeeping scales with the number of views, and its single memory lock serialises the
title's and the layer's threads. System Wine 11.22 measures the same as Proton Experimental.

## What a native port has to provide

### The title's host imports (today passed through to Wine)

The exe binds 88 imports to Flamingo's own functions, 427 to the host, 72 between the title's
modules. Across the minimal set of modules (exe, MS `msvcp140`, `vcruntime140(_1)`, Bink,
BvhBuilder, the FidelityFX DLLs) about 400 host functions are not implemented by Flamingo. The
dump ships Microsoft's own `msvcp140`/`vcruntime140` (they import only `api-ms-win-*`): map them as
title DLLs instead of implementing C++ runtime functions. libHttpClient/XCurl can be replaced by an
offline implementation of the 38 `HC*` calls.

| Area | Count | Examples | Difficulty |
|---|---|---|---|
| UCRT (`api-ms-win-crt-*`) | ~204 | math, string, stdio (`__stdio_common_vfprintf`), runtime (`_initterm`, onexit), time, locale, heap | Hard by volume (Windows printf semantics, 16-bit `wchar_t`) |
| Threads / sync | ~58 | critical sections, SRW, condition variables, events, `WaitForMultipleObjects`, `InitOnce`, `WaitOnAddress`, threadpool | Hard |
| TLS / fibers | 4 + TEB | `Fls*`; static TLS through `gs:[0x58]` | Hard |
| Exceptions / unwinding | 16 | `RtlVirtualUnwind`, `RtlUnwindEx`, `RaiseException`, vectored handlers; vcruntime's `__C_specific_handler`, `__CxxFrameHandler3/4`, `longjmp` | Hardest |
| Memory | ~4 (+13 xmem in Flamingo) | `VirtualAlloc/Free/QueryEx`, heaps | Medium |
| Files / IO | ~22 | `WriteFile`, `SetFilePointerEx`, `FindNextFileW`, pipes | Medium (case-insensitive paths) |
| Time, strings, locale | ~29 | QPC, timezones, `MultiByteToWideChar`, `LCMapStringEx`, `FormatMessage` | Easy-medium |
| Networking / crypto | ~40 | ws2_32 by ordinal, winhttp, `BCryptGenRandom` | Easy as offline stubs |
| COM / media / window | ~21 | WASAPI (`CoCreateInstance`), Media Foundation, `XAudio2_9` (Bink), window messages | Hard (COM audio) |

### Flamingo's own Windows dependencies

| Crate | Windows use |
|---|---|
| victory-loader | image mapping through `LoadLibraryExW`, relocations, `.pdata`, static TLS patching, thread notices, the crash handler (VEH, unwinding, AMD-only instruction emulation) |
| victory-hle (558 `extern "system"` functions) | xmem (placeholders, sections, views, write watch), files, windows and messages, GameInput/XInput/HID, WASAPI, timers |
| victory-gpu (~1,000 D3D12/DXGI calls in 25 files) | Direct3D 12 and DXGI through the `windows` crate; `D3DCompile` at run time; `OpenExistingHeapFromAddress`; the memory watch (write watch, guards, VEH) |
| victory-shader, core, pe, formats, pipeline | portable already (shader tests aside) |

Direct3D 12 on Linux: native vkd3d-proton (`libvkd3d-proton-d3d12.so`) plus DXVK-native's DXGI on
SDL3, keeping victory-gpu's code; the `windows` crate's bindings may build on Linux
(`windows-link` emits plain `extern` blocks), but the COM calling convention must be checked
against the native vkd3d-proton build first (a mismatch corrupts the stack silently). `D3DCompile`
becomes DXIL compiled ahead of time with `dxc` (Flamingo's own shaders). A direct Vulkan backend
would fork the 65,000-line GPU crate: not recommended.

### The hard runtime pieces (Wine source to read)

| Piece | Native work | Wine reference |
|---|---|---|
| PE mapping | sections, protections, relocations, our own function table, DLL init order | `dlls/ntdll/unix/virtual.c`, `dlls/ntdll/loader.c` |
| TEB / GS / TLS | one TEB per thread (GS base via `arch_prctl`/`wrgsbase`), TLS templates per thread, TEBs for our own threads that call into the title | `dlls/ntdll/unix/thread.c`, `signal_x86_64.c`, `loader.c` |
| Calling convention | every title-facing function `extern "win64"` (833 sites today; identical on Windows, so upstreamable) | — |
| SEH dispatch | signals to `EXCEPTION_RECORD`/`CONTEXT`, vectored handlers, frame walk with language handlers, unhandled filter, `RtlRestoreContext` | `dlls/ntdll/unix/signal_x86_64.c`, `dlls/ntdll/signal_x86_64.c`, `dlls/ntdll/exception.c` |
| x64 unwinding | `RtlVirtualUnwind` (chained, machine frames, v2 epilogs), `RtlUnwindEx` (collided, consolidate); exceptions must never cross Rust frames | `dlls/ntdll/unwind.c`, `dlls/msvcrt/except_x86_64.c` |
| Stacks | sizes from the image header, `GetCurrentThreadStackLimits`, stack overflow on `sigaltstack` | `virtual.c` |
| AMD-only instructions | the crash handler's emulation as a SIGILL handler (runs on this Intel CPU) | — |
| Threads | `pthread_create` + TEB + TLS + `DLL_THREAD_ATTACH` | `thread.c` |
| Synchronisation | Windows semantics on futexes inside the title's own structs; a handle table for events/mutexes/semaphores | `dlls/ntdll/sync.c` |
| Virtual memory | our own range map over `mmap`; xmem's physical pages as one memfd, views `mmap(MAP_FIXED\|MAP_SHARED)`; write watch with userfaultfd write-protect (async) + `PAGEMAP_SCAN` (works on shared views, so the guards and their faults go away); safe copies with `process_vm_readv` | `virtual.c` |

This machine supports all of it: userfaultfd WP-async and `PAGEMAP_SCAN` (kernel 6.18; unprivileged
userfaultfd needs `UFFD_USER_MODE_ONLY`), `vm.max_map_count` 1,048,576.

## Staged plan

Shared code: a `victory-os` platform layer (`mem`, `thread`, `sync`, `fault`, `time`, `fs`,
`window/input`, `audio`). Its Windows backend is today's code moved behind it with no change in
behaviour (offered upstream in small pieces, with the `"win64"` change), so upstream Flamingo keeps
merging; Linux code lives behind it.

| # | Milestone | Effort | Testable result |
|---|---|---|---|
| **A** | **Memory bridge inside Wine**: a winelib DLL (built with `winegcc`, run on system Wine 11.22) whose Linux side does the title's views (memfd + `mmap`), protections, write watch (userfaultfd + `PAGEMAP_SCAN`) and safe copies; Wine sees one reserved range | 2-3 weeks | remapping and write-watch askings 10x+ cheaper in the frame log; cutscene fps with `ab_runs.py` |
| B | Platform layer + the crates compile for Linux; spike: `windows` crate + native vkd3d-proton ABI | 2-3 weeks | `cargo check` on Linux; portable tests pass natively |
| C | Native loader to the title's first import call (mapping, TEB/PEB, GS, TLS, fault reporter) | 1-2 weeks | "first import: dll!name" |
| D | Exceptions and unwinding | 3-4 weeks | a conformance PE of our own (`__try`, C++ throw/catch, `longjmp`, access violations) behaves as on Windows |
| E | Core runtime + UCRT; MS `msvcp140`/`vcruntime140` mapped as title DLLs; offline net/HTTP | 4-6 weeks | the title reaches `XGameRuntimeInitialize` |
| F | Graphics, window, input: native vkd3d-proton, DXVK-native DXGI, SDL3, DXIL blobs | 3-5 weeks | first presented frame, then the world |
| G | Audio and media: XAudio2 on FAudio, a WASAPI subset on PipeWire/SDL | 2-3 weeks | the opening cutscene with sound |

B to G: about 4-6 months for one developer.

## Go / no-go

1. **Do A first, and only A.** It targets the measured cost with about 1% of a full port's work.
2. **After A**: profile the cutscene. If Wine's own code is under ~10% of the render and submit
   threads' time, stop and keep the bridge. Go on to B-C only if Wine still costs over ~15-20%, or
   the bridge cannot live with Wine's view bookkeeping (in-place heaps, `VirtualQuery` users).
3. **After B + C + D**: the exception conformance suite must pass entirely and the title must get
   through CRT initialisation; unreliable unwinding is where a native port corrupts state quietly.
4. **After F**: keep the native build only if it is clearly faster (10% or more) than Wine + bridge
   in the same scene, or much easier to debug; two D3D12 paths against a daily-changing upstream are
   a lasting cost.
