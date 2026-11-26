# A Vulkan path for Flamingo's GPU layer

Status: idea and plan, 2026-11-26. Nothing is implemented. Read with `docs/linux-native-port.md`.

## How the pieces fit today

```
Xbox Series title (its own x86-64 code, run as is)
  └─ Flamingo (victory.exe, a Windows program)
       ├─ shader translator: the console GPU's machine code -> DXIL (Direct3D 12 shaders)
       ├─ GPU layer (victory-gpu): the console's graphics commands and memory -> Direct3D 12
       └─ console runtime (victory-hle): the system functions the title calls
  └─ Proton
       ├─ vkd3d-proton: Direct3D 12 -> Vulkan (DXIL -> SPIR-V through dxil-spirv)
       ├─ DXVK: DXGI / Direct3D 11 -> Vulkan
       └─ Wine: every other Windows call (memory, threads, files, window, audio)
  └─ NVIDIA's Vulkan driver -> RTX 4070
```

## Where the time goes (measured, RTX 4070, Proton Experimental)

A gameplay frame takes about 550 ms (1.8 fps, ~4,900 draws). Upstream measures about 100 ms for a
world frame with the same Flamingo and the same card on Windows, and about 15 ms of it on the GPU.

| Part of a gameplay frame | Time |
|---|---|
| Waiting for the card (about 38 submissions, each waited for: `VICTORY_LATE_WAIT=0`) | ~290 ms |
| The card idle while we prepare the next submission | ~275 ms |
| Preparing submissions (copies of title memory, ~155 MB) | ~130 ms |
| Copying shader writes back to title memory | ~100 ms |
| Memory watch + remapping (Wine's memory calls) | ~80 ms |

The card reports itself 20-35% busy. Most of the 290 ms is not the card working: it is the round
trip of submit-and-wait, repeated per submission.

## Where the 290 ms could live

1. **Flamingo's waiting**: each submission is waited for. Cheap on Windows, dear here if each wait
   is slow. Settings test: `VICTORY_LATE_WAIT` 1 and 2 (submissions sent without a wait each) against
   0 (`scripts/linux/ab_runs.py`). (3 was measured: worse, its copy-back costs more than it saves.)
2. **Wine's synchronisation**: a fence's completion reaches Flamingo as a Windows event through
   Wine (ntsync is up). Measured by timing a wait on an empty submission.
3. **vkd3d-proton**: how it turns Direct3D 12 submissions and fences into Vulkan submissions and
   timeline semaphores, its queue and fence worker threads, its descriptor heap emulation.

To tell 2 from 3: a frame-log split of each wait into "submitted -> the card's timestamp at the
list's end" (the card's own time) and "the card's end -> our wait returned" (the delivery latency
through vkd3d-proton and Wine). Small; about half a day.

## The Vulkan path, if 3 is where the time is

### Option V1: a Vulkan backend inside Wine (recommended form)

Flamingo keeps running under Proton, and its GPU layer talks Vulkan instead of Direct3D 12. Wine's
`winevulkan` passes Vulkan calls through to the native driver with little more than a thunk, so
vkd3d-proton (and its translation) leaves the path while everything else stays as it is: Valve's
Wine performance work, the title runtime, the memory layer, upstream's daily changes outside the
GPU layer. Not Linux-only: the same Vulkan backend runs on Windows with NVIDIA's Windows driver,
so it could be offered upstream as an option.

### Option V2: Vulkan as part of the native port

The same backend, used by a Flamingo that runs without Wine (`docs/linux-native-port.md`,
milestone F would use it instead of native vkd3d-proton). Only worth it after the native port's own
go/no-go points.

### What the backend has to cover (victory-gpu, ~65,000 lines, ~1,000 Direct3D 12 calls)

| Direct3D 12 concept in victory-gpu | Vulkan counterpart | Notes |
|---|---|---|
| Device, queues, command lists/allocators | `VkDevice`, `VkQueue`, command pools/buffers | direct |
| Fences (`ID3D12Fence`, events) | timeline semaphores, `vkWaitSemaphores` | the thing being measured: a wait without Wine events |
| Shaders: DXIL from the translator | SPIR-V: run `dxil-spirv` (vkd3d-proton's own converter, a library) on our DXIL, or emit SPIR-V from the translator later | first step: dxil-spirv, the translator unchanged |
| Root signatures, root constants/CBVs | pipeline layouts, push constants, descriptor buffers | vkd3d-proton's design is the reference |
| Shader-visible descriptor heaps (1,000,000 views, bindless) | `VK_EXT_descriptor_buffer` or `VK_EXT_descriptor_heap` (both on this driver) | the largest piece: victory-gpu's heap mirroring is built around D3D12 heaps |
| Placed resources, heaps, aliasing | `VkDeviceMemory` + bind at offsets | direct |
| `OpenExistingHeapFromAddress` (in place) | `VK_EXT_external_memory_host` | what vkd3d-proton does underneath |
| ExecuteIndirect (draws, dispatches, mesh) | indirect draw/dispatch, `VK_EXT_device_generated_commands` | victory-gpu clamps and gathers indirect arguments itself |
| Mesh shaders | `VK_EXT_mesh_shader` | supported on the RTX 4070 |
| Ray tracing (DXR 1.1) | `VK_KHR_ray_tracing_pipeline`/`ray_query` | the title's builder is off in Flamingo today |
| Pipeline state objects, pipeline library | `VkPipeline`, pipeline cache | Flamingo's pipeline library becomes a Vulkan pipeline cache |
| Swap chain (DXGI) | `VK_KHR_swapchain` on the Win32 surface (under Wine) | |
| `D3DCompile` of Flamingo's own HLSL helpers | DXIL compiled ahead of time, through dxil-spirv | |
| Timestamps, queries, DRED | Vulkan queries, `VK_EXT_device_fault` | |

### Shape of the change

- A backend trait in victory-gpu (`gpu::Backend`: devices, lists, resources, descriptors, pipelines,
  sync), with the Direct3D 12 code as its first implementation (no change in behaviour, upstreamable).
- `VICTORY_GPU_BACKEND=vulkan` selects the second implementation; Direct3D 12 stays the default.
- Built in the order the frame needs it: device + swap chain + clears; then a menu frame (copies,
  simple pipelines, descriptors); then the world (indirect, mesh, heaps in place).

### Effort and risks

- Effort: 2-4 months for the backend to reach today's picture, the descriptor heap and indirect
  paths being most of it.
- Upstream: victory-gpu changes daily and is written against Direct3D 12; every GPU change upstream
  then needs its Vulkan twin, unless the backend trait is accepted upstream (worth proposing first).
- Correctness: two backends drawing the same frame differently; the frame log and
  `VICTORY_VERIFY` help, a picture comparison (captures at the same cutscene time) is needed too.
- It may not pay: if the wait latency is Wine's (2) or Flamingo's (1), Vulkan changes little.

## Go / no-go

1. `VICTORY_LATE_WAIT` 1 and 2 against 0: if one cuts the waiting, take it and re-measure first.
2. The wait split: if delivery latency (card done -> our wait returns) is most of each wait, find
   whether vkd3d-proton or Wine holds it. Only if vkd3d-proton's share is large is V1 worth its
   months; a prototype that times a Vulkan timeline-semaphore round trip under Wine against
   vkd3d-proton's fence round trip (a day) confirms it before any backend work.
