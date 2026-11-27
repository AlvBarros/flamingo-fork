# S3: `VK_EXT_external_memory_host` and in-place D3D12 heaps on bridge memory

Spike S3 of docs/memory-bridge-plan.md (§5). Question: will NVIDIA's driver import a memfd-backed
`MAP_SHARED` mapping, placed with `MAP_FIXED` inside a range Wine reserved, as a host pointer? And
does the in-place path in `crates/victory-gpu/src/inplace.rs` work on such memory under Proton's
vkd3d-proton?

| File | What |
|---|---|
| `s3bridge_unix.c`, `s3bridge_pe.c`, `s3bridge.h` | a small bridge: a memfd pool, a view (`MAP_SHARED \| MAP_FIXED`), and an unmap back to `PROT_NONE` |
| `vkhost.c` | the Vulkan test, built natively (libvulkan) and for Windows (winevulkan) from the same file |
| `d3d12host.c` | `OpenExistingHeapFromAddress` / `OpenExistingHeapFromAddress1` on bridge views, under vkd3d-proton |
| `build.sh` | builds everything; with `RUN=1` it also runs it natively and under Proton Experimental |

Each case imports one address, then checks through a second CPU view of the same pages (an alias)
that the GPU reads what the CPU wrote and the CPU sees what the GPU wrote, in two rounds. The D3D12
check matters because vkd3d-proton quietly falls back to a plain allocation when the import fails
(`vkd3d_import_host_memory` in `libs/vkd3d/memory.c`). In that case the heap opens, but it is not
the title's memory.

## Build and run

    VK_INC=<dir with vulkan/vulkan.h> RUN=1 sh tools/bridge-spike/s3/build.sh <out dir>

The default out dir is `target/bridge-spike-s3`. The script sets up a throwaway prefix
`<out>/pfx-s3` and copies Proton's vkd3d-proton (`d3d12`, `d3d12core`) and DXVK (`dxgi`, `d3d11`)
into it. d3d12core needs a dxgi, and Wine's own dxgi pulls in wined3d and its libvkd3d, which
Proton does not ship. Two things to know:
- Wine finds the bridge's Unix half only through `WINEDLLPATH`. Putting the `.so` beside the
  `.dll` is not enough.
- This system has no `/usr/include/vulkan`. Any copy of the Vulkan headers works (header 1.4.321
  was used).

## Results (2026-11-27, RTX 4070, driver 620.73.04, kernel 6.18, Proton Experimental 11.0-20261112, vkd3d-proton 3.2.0 `2576c0b4`)

`minImportedHostPointerAlignment` = 4096. The memory types offered for an imported pointer are the
same for every kind of memory: `0xc`, which is types 2 (host-visible, coherent) and 3
(host-visible, coherent, cached). No device-local type is offered.

| Case | Native | Proton (winevulkan) |
|---|---|---|
| anonymous private / Wine `VirtualAlloc` commit (baseline) | OK | OK |
| memfd `MAP_SHARED` | OK | OK (bridge view in a Wine-reserved window) |
| alias: import view 1, CPU through view 2 (both directions natively) | OK | OK |
| bridge view placed over Wine-committed memory | — | OK |
| imported view unmapped (to `PROT_NONE`) after import, before GPU use | OK: the GPU keeps the pool's pages, visible through the other view | OK, same |
| import cost, 256 MiB, pages never touched / touched | anon 22-30 / 4-6 ms; memfd 54-67 / 7-13 ms | Wine memory 22-29 / 4-5 ms; bridge 52-54 / 7-11 ms |

winevulkan (now in `win32u/vulkan.c`) passes `VkImportMemoryHostPointerInfoEXT` and
`vkGetMemoryHostPointerPropertiesEXT` straight through to the host driver in a 64-bit process.
It does not translate or check the pointer, and it never asks Wine's view list. Its own use of
external memory host only applies to WoW64 mappings.

| D3D12 under vkd3d-proton | Result |
|---|---|
| `OpenExistingHeapFromAddress`, Wine-committed memory (baseline) | OK, the heap is this memory |
| `OpenExistingHeapFromAddress1`, Wine-committed memory | OK |
| `OpenExistingHeapFromAddress`, bridge view at the window's base | **E_INVALIDARG**: `VirtualQuery` reports `MEM_RESERVE` |
| `OpenExistingHeapFromAddress`, bridge view inside the window | **E_INVALIDARG**: `AllocationBase != address` and `MEM_RESERVE` |
| `OpenExistingHeapFromAddress1`, bridge view (also through an alias) | OK, the heap is this memory |
| `OpenExistingHeapFromAddress` and `...1`, bridge view over Wine-committed memory | OK |
| `...1` on a bridge view, the view unmapped by the bridge while the heap stands | OK: GPU reads and writes still reach the pool's pages, through the alias |

Code path (vkd3d-proton `libs/vkd3d/device.c`):
- `d3d12_device_OpenExistingHeapFromAddress` calls `VirtualQuery(address)`. It requires
  `AllocationBase == BaseAddress == address` and `State == MEM_COMMIT`, then calls `VirtualQuery`
  once more at the end to make sure the region has one protection. Then it calls `...1` with
  `RegionSize`.
- `d3d12_device_OpenExistingHeapFromAddress1` does not query at all. It builds a custom heap
  (L0, write-back, shared plus cross-adapter shared when the extension is present) and imports
  `address` with `VkImportMemoryHostPointerInfoEXT`. If that fails, it falls back without saying so.
- `OpenExistingHeapFromFileMapping` does `MapViewOfFile` and then the same as
  `OpenExistingHeapFromAddress`. It needs a Wine section handle, which a memfd pool does not have.
