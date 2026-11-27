# Memory bridge: plan (Linux fork)

Status: plan, 2026-11-27. Base: this repository's `main` (trustno1-coder/VIctory-NVIDIA with the
Linux fork's scripts and fixes). Written to be picked up by separate sessions or agents: each
workstream below states what it owns, what it may assume and how it is checked.

## 1. Why

On an RTX 4070 under Proton the game runs at 16-17 fps when nothing streams (cutscene, ~4,900
draws, frame median 58 ms), but about one moment in six takes 250 ms to 5 s. In those frames the
copies of title memory slow from ~1.7 GB/s to ~0.12 GB/s (2026-11-27 session: 918 ms for 109 MB
against 46 ms for 78 MB). The bytes are the same; the cost is Wine's bookkeeping of the title's
memory, which grows with the number of views the title maps (~60,000):

| Operation (measured on this PC) | Proton's Wine | Native Linux |
|---|---|---|
| Map + unmap a 64 KiB view at 60,000 views | 337 µs (16 µs with none) | 1.9 µs |
| Change of protection, 6 other threads busy | 6.4 µs | 0.15 µs |
| Map + unmap through the bridge spike, under Proton | — | 1.5 µs |

Wine keeps its own list of views and one lock over it; every `MapViewOfFile3`,
`UnmapViewOfFile2`, `VirtualFree` (placeholder split), `VirtualProtect`, `VirtualQuery` and
`GetWriteWatch` goes through it. Valve's Proton work does not change this and we do not rebuild
Proton.

## 2. The idea

A Wine **builtin DLL** with a **Unix half** (`__wine_unix_call`), loaded by `victory.exe` at start.
Its Unix half does the title's page pool and views with Linux calls directly:

- the pool of physical pages: one `memfd` (instead of a `SEC_COMMIT` section);
- a view: `mmap(MAP_FIXED | MAP_SHARED, memfd, offset)` over address space Wine holds reserved;
- an unmap: `mmap(MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, PROT_NONE)` (the range stays ours);
- protections: `mprotect`;
- write tracking: `userfaultfd` write-protect in asynchronous mode plus `PAGEMAP_SCAN`
  (`PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC`): the kernel clears a page's write-protect bit on
  the first write without any fault, and one scan returns and re-arms the written pages. It works
  on shared views, so the 64 KiB guards and the vectored handler's faults go away for them;
- many operations per call (a frame's remapping is ~2,100 calls today).

Wine sees one reserved range and nothing inside it changes for Wine. Proven so far
(`tools/bridge-spike/`): such a DLL, built with MinGW + gcc + Wine's `winebuild`, loads and runs
under **Proton Experimental's Wine** (and system Wine 11.22), and its Unix half maps a memfd over a
range reserved through Wine (1.5 µs per map+unmap). This kernel (6.18) has userfaultfd write-protect
in async mode and `PAGEMAP_SCAN`; `vm.unprivileged_userfaultfd` is 0, so the bridge opens it with
`UFFD_USER_MODE_ONLY`.

What the bridge does not change: everything else Wine does (Valve's work stays), the title's own
code, the GPU side's translation, the Windows build (the bridge is optional and off on Windows).

## 3. Where the title's memory goes through Wine today (this tree)

| Place | What | Calls |
|---|---|---|
| `crates/victory-hle/src/modules/xmem.rs` (2,551 lines) | physical page pool (`CreateFileMapping`, `SEC_COMMIT` under Wine), views (`map`/`show`/`split`/`unmap`, lines ~1599-1760: `MapViewOfFile3` with `MEM_REPLACE_PLACEHOLDER`, `VirtualFree` `MEM_PRESERVE_PLACEHOLDER`, `UnmapViewOfFile2`), batches (`change`, `Op`, a worker pool), aliases (`map_alias(es)`, `scatter`), the page table (`Pages`, `Shown`), costs (`map_costs`) | the remapping |
| `crates/victory-gpu/src/watch.rs` (4,385 lines) | write watch for private title memory (`GetWriteWatch` resets as it asks), 64 KiB guards on views (`VirtualProtect` read-only + vectored handler `on_write`, line ~2565), every-view guarding (`VICTORY_VIEW_GUARD`, now 0 here), `NtQueryVirtualMemory` / `VirtualQuery` | the watch |
| `crates/victory-gpu/src/safecopy.rs` | copies that fail instead of faulting: one `rep movsb`, faults taken by `watch::on_write`; `VICTORY_SAFE_COPY=0`: `ReadProcessMemory` | the copies |
| `crates/victory-gpu/src/inplace.rs` | heaps opened on title memory (`OpenExistingHeapFromAddress(1)`, `...FromFileMapping`): while one stands Wine refuses `VirtualFree`/`VirtualProtect`/unmap there | in place |
| `shadow.rs`, `upload.rs`, `host.rs`, `texture.rs`, `copystat.rs` | queries and copies around the above | |
| title imports | the title's own `VirtualAlloc/Free/Protect/Query` on its memory (bound by `victory-loader`) | rare, but must stay right |

## 4. Workstreams

Contract first (WS0), then WS1-WS9 in parallel, then milestones in order. Every workstream keeps
the present path as the fallback (`VICTORY_BRIDGE=0`, and always on Windows), so `main` is never
broken by a half-done workstream.

### WS0 — the contract (serial, first; 1-2 days, one person)

Writes `crates/victory-bridge/` with the interface every other workstream codes against, plus
stubs that call the present Windows path:

```rust
pub trait PagePool {                       // one per xmem physical pool
    fn create(bytes: u64) -> Option<Self> where Self: Sized;
    fn map(&self, ops: &[MapOp]) -> Vec<bool>;   // view at addr, len, pool offset, prot
    fn unmap(&self, ranges: &[(usize, usize)]);   // back to reserved, ours
}
pub fn protect(ranges: &[(usize, usize, Prot)]) -> bool;
pub fn query(addr: usize) -> Option<Region>;      // from our range map, not Wine's
pub fn watch_register(addr: usize, len: usize) -> bool;
pub fn watch_take(addr: usize, len: usize, out: &mut Vec<usize>) -> bool; // written pages, re-armed
pub fn bridged(addr: usize) -> bool;              // is this range the bridge's
pub fn available() -> bool;                       // DLL loaded, VICTORY_BRIDGE not 0
```

The unix-call ABI (function numbers, argument structs, `#[repr(C)]`, batch layout) is fixed in
`crates/victory-bridge/src/abi.rs` and mirrored in `bridge/unix/abi.h`. Also fixed here: the
reserved window (one `VirtualAlloc(MEM_RESERVE)` per title pool range, made by xmem through Wine
so Wine never places anything inside), and the rule that **only the bridge** changes pages inside it.

Done when: the crate builds for Windows, the stubs pass xmem's and watch's existing tests unchanged.

### WS1 — the Unix half (independent; ~1 week)

`bridge/unix/` (C or a Rust `cdylib` for `x86_64-unknown-linux-gnu`) exporting
`__wine_unix_call_funcs`: pool create/destroy (memfd), batched map/unmap, mprotect, uffd setup
(`UFFD_USER_MODE_ONLY`, `UFFD_FEATURE_WP_ASYNC`, `UFFDIO_REGISTER_MODE_WP`), `PAGEMAP_SCAN` take,
`process_vm_readv` copy. Tested natively on Linux (no Wine) with a C/Rust test harness: correctness
(aliases see each other's writes, unmap leaves PROT_NONE, scan finds exactly the written pages,
re-arm works, many views), and speed at 60,000 views.

### WS2 — the PE half and loading (independent; 3-4 days)

`bridge/pe/` (MinGW C, as the spike) or a Rust `cdylib` for the Windows target marked builtin with
`winebuild --builtin`: `DllMain` gets the unix handle (`NtQueryVirtualMemory(MemoryWineUnixFuncs)`),
exports thin functions per ABI entry. `victory-bridge` loads it with `LoadLibraryW` from beside
`victory.exe`, checks an ABI version call, and falls back if anything is missing. Unit test: the
spike's `load.c` turned into a test that runs under Proton.

### WS3 — xmem on the bridge (needs WS0; ~1 week)

`xmem.rs`: the physical pool becomes a `PagePool`; `show`/`split`/`unmap`/`commit` and `change`'s
batches go through it (one bridge call per batch, not per view); aliases (`map_alias(es)`,
`scatter`) likewise. The page table, `Shown`, `title_memory_remapped` / `title_memory_view` notices
stay as they are. `map_costs` counts bridge calls too. Existing tests (`physical_pages_map_alias_and_unmap`,
`a_read_in_place_never_meets_the_gap_of_a_remapping`, `pages_mapped_again_hold_what_they_held`, ...)
run against both paths.

### WS4 — queries and protections that must not ask Wine (needs WS0; ~1 week)

Inside the bridge's window Wine's answers are wrong (it sees reserved, no access). Classify every
`VirtualQuery` (40 sites), `VirtualProtect` (51), `NtQueryVirtualMemory`, `ReadProcessMemory` (8)
site as *title memory* or *our own*; title-memory ones ask `victory_bridge::query/protect` when
`bridged(addr)`. The title's own imports of `VirtualQuery/VirtualProtect/VirtualFree` get thin
wrappers bound by `victory-loader` that do the same. Deliverable includes the classified list in the
PR description.

### WS5 — write watch without guards (needs WS0, spike S1; ~1-2 weeks)

`watch.rs`: for bridged views, replace the 64 KiB guards, `cover`/`open`, the every-view logic and
the faults with `watch_register` at map time and `watch_take` where `GetWriteWatch` / the guard bits
are read today; for private write-watched title memory, the same through uffd when bridged,
`GetWriteWatch` otherwise. The journal (`ask`/numbering per asking) stays. `on_write` stays for
what is not bridged and for safecopy.

### WS6 — copies (needs WS0; 2-3 days)

`safecopy.rs` keeps `rep movsb` + the handler (views are real pages); a fault inside the window
must still reach `on_write` (Wine raises an access violation for a page it thinks reserved: check
with S2). Optional `process_vm_readv` path for bulk copies.

### WS7 — heaps in place (later; after M2)

`inplace.rs` on bridged memory: vkd3d-proton imports host pointers through
`VK_EXT_external_memory_host`; whether NVIDIA's driver takes a memfd-backed shared mapping is spike S3.
Until then: no in-place heap on bridged ranges (`inplace` asks `bridged()`).

### WS8 — measurement (independent, start now; 3-4 days)

This tree has no per-frame memory counters (the Flamingo-based branch had them: "memory watch, all
threads", "remapping: N calls M ms (Windows calls …)"). Port them (`crates/victory-gpu/src/framelog.rs` and xmem's `timed` wrapper
from the `linux-on-new-upstream` branch) so each frame says: views mapped/unmapped and the
time, protections and time, watch askings and time, copies MB and time. Extend
`scripts/linux/ab_runs.py` to this tree's frame line. Baseline: the cutscene stretch and a walk,
unattended (`FORK_KEYS`), three runs each.

### WS9 — build and packaging (independent; 2 days)

`scripts/linux/build.sh` builds the bridge (MinGW for the PE half, gcc/cargo for the Unix half,
`winebuild --builtin`) and places `victorybridge.dll` + `victorybridge.so` beside `victory.exe`;
`run.sh` prints whether it loaded; `VICTORY_BRIDGE=0` turns it off. The xtask policy and
`.gitignore` know the new files.

## 5. Spikes (do first, in parallel with WS0; half a day to a day each)

| | Question | How | Decides |
|---|---|---|---|
| S1 | uffd WP-async + `PAGEMAP_SCAN` inside a Proton process, on memfd views and on anonymous private memory: works, and its cost at 60,000 views | extend `tools/bridge-spike` | WS5's design |
| S2 | Wine's behaviour on our window: a fault there reaches our vectored handler as an access violation; `VirtualQuery` says reserved; Wine never touches the pages; vkd3d copies from it fine | spike program under Proton | WS4, WS6 |
| S3 | `VK_EXT_external_memory_host` on a memfd mapping with NVIDIA 620 | small Vulkan test under Proton (`winevulkan`) | WS7 |
| S4 | cost of one `__wine_unix_call`, and of batches | spike | batch sizes in WS0 |

## 6. Milestones and go/no-go

| | What | Check |
|---|---|---|
| M0 | WS0 + S1-S4 | contract merged; spikes answered |
| M1 | remapping through the bridge (WS1, WS2, WS3, WS8, WS9) | frame log: remap time per frame 10x lower; stutter frames' copy rate up; no new stale data (`VICTORY_VERIFY`-style check on) |
| M2 | write watch through uffd (WS5) | guards and their faults gone on views; watch time per frame down; picture unchanged |
| M3 | queries/protections right everywhere (WS4, WS6) | a 30-minute session, no crash; the title's own memory calls served |
| M4 | in place on bridged memory (WS7), if S3 allows | |

Go/no-go after M1: if the stutter frames' copy rate does not rise severalfold, stop and measure
where those frames go before M2. Each milestone lands on `main` behind `VICTORY_BRIDGE` (default
off until M1 measures better, then on).

## 7. Parallel plan

```
day 1-2   WS0 contract ─────────────┐   S1 S2 S3 S4 spikes (4 people/agents)   WS8 measurement   WS9 build
day 3-8   WS1 unix half   WS2 PE half   WS3 xmem   WS4 queries   (WS8, WS9 finishing)
day 8-12  M1 integration + measure ─── WS5 watch (from S1) ─── WS6 copies
day 12-16 M2, M3; WS7 if S3 said yes
```

Up to five people or agents at once after WS0. Rules for parallel work: one branch per workstream
off `main`; nothing outside the files a workstream owns changes without the WS0 owner; every PR
keeps `VICTORY_BRIDGE=0` behaving exactly as before (tests run both ways); runs on the game are
announced and never overlap (one Wine prefix).

## 8. Settings to measure again (WS8 owns this)

`scripts/linux/play.env` holds the measured best set for this base under Proton (frame median 58 ms,
mean 193 ms in the cutscene stretch). Several of its values were chosen *against Wine's memory
costs*; once the bridge works the balance moves, so each is re-measured with `VICTORY_BRIDGE=1`
(A/B against the same settings with the bridge off, same unattended stretch):

| Setting (now) | Why it may change with the bridge |
|---|---|
| `VICTORY_VIEW_GUARD=0` | the bridge's write tracking replaces the guards on views: the switch may stop mattering, or `=1` may be right again |
| `VICTORY_MAP_THREADS=1` | one mapping thread because Wine's memory lock serialises mapping; natively, `auto` (several) may win |
| `VICTORY_TEXTURE_NO_WAIT=1` | the stalls it avoids came from slow copies at streaming; if they go, waiting (`=0`) may cost little and remove texture pop-in |
| `VICTORY_THIN_THREADS=8`, `UNDER_THREADS=6`, `LAND_THREADS=4` | sized while threads queued on Wine's lock |
| `VICTORY_CARD_BUFFERS_MB=1536`, `VICTORY_INDIRECT_WINDOW=1`, `VICTORY_RELAY=2` | how much is copied to the card against read from title memory |

Also not yet measured on this base at all (clear wins on the Flamingo base, same switches exist
here): `VICTORY_LATE_WAIT=2` (Flamingo: median -37%), `VICTORY_CARD_BUFFERS_MIN_KB=64` (with
1536 MB, Flamingo: -25%; this base has `VICTORY_CARD_BUFFERS_MIN_MB=1`), `VICTORY_SHADER_OPT=0`.
Measure them once without the bridge first (baseline), so their gain is not mixed into the
bridge's.

A setting becomes the default only after an A/B on drawing frames shows it better (and no crash,
no new dropped draws); the measurement goes into the comment above it in `play.env`.

## 9. Risks

- **Wine assumptions inside the window** (S2): if Wine reads or protects pages there behind our
  back (e.g. on `NtReadVirtualMemory` from another thread, or exception handling), results are wrong.
  Mitigation: the window holds only the title's xmem views; S2 checks the paths.
- **uffd and Wine's signal handling**: async WP raises no fault, so no signal; the sync mode must not
  be used.
- **Two notions of "committed"**: Wine's commit accounting does not see memfd pages (good for the
  commit limit, but `GlobalMemoryStatus`-style answers change); the title asks the runtime, not Wine.
- **Windows build**: the bridge crate compiles to stubs there; CI must keep building it.
- **Upstream merges**: xmem and watch change upstream often; keep the bridge behind the trait so
  merges touch the Windows path only.

## 10. Progress

Live log of the implementation on branch `memory-bridge` (started 2026-11-27). The coordinating
session owns this section's structure and merges; parallel agents append only inside their own
subsection below and commit on their own branch (`memory-bridge-<ws>`), never on `memory-bridge`.

Rules in force: `VICTORY_BRIDGE` defaults to off and `=0` behaves exactly as before; tests pass
both ways; `cargo xtask policy` passes before every push; game runs are announced in **Runs** below
before they start and never overlap (spikes use their own throwaway Wine prefixes, not the game's).

### Status

| Item | Owner | Branch | State |
|---|---|---|---|
| WS0 contract | coordinator | `memory-bridge` | in progress |
| S1 uffd WP-async + PAGEMAP_SCAN under Proton | agent | `memory-bridge-s1` | in progress |
| S2 Wine's behaviour on the window | agent | `memory-bridge-s2` | in progress |
| S3 external_memory_host on memfd (NVIDIA) | agent | `memory-bridge-s3` | in progress |
| S4 cost of `__wine_unix_call` and batches | agent | `memory-bridge-s4` | in progress |
| WS1-WS9 | — | — | waiting for spike results |

### Runs

(Announce each game run here, with start time and who, before starting it.)

- none yet

### WS0 results

(pending)

### S1 results

(pending)

### S2 results

(pending)

### S3 results

2026-11-27. Setup: RTX 4070, driver 620.73.04, Proton Experimental 11.0-20261112, vkd3d-proton
3.2.0 (`2576c0b4`). Code, how to run it and the full output are in `tools/bridge-spike/s3/`.
Every case checks the result through a second view of the same memfd pages: the GPU reads bytes
the CPU wrote, the CPU sees bytes the GPU wrote, and this is done in two rounds.

| Case | Result |
|---|---|
| Native: import of anonymous memory, memfd `MAP_SHARED`, or a memfd alias placed with `MAP_FIXED` in a `PROT_NONE` range | OK. `minImportedHostPointerAlignment` is 4096. Offered types `0xc`: host-visible coherent, and the same cached; no device-local type |
| Proton, winevulkan: import of a bridge view in a Wine-reserved window, of an alias of one, and of Wine-committed memory | OK, same types. winevulkan (`win32u/vulkan.c`) passes the host pointer straight to the driver in a 64-bit process and never asks Wine's view list |
| Imported view unmapped by the bridge after import (natively and under Proton) | The GPU keeps the pool's pages (they are pinned) and they stay coherent through the other views. The heap holds the pages it was given, not the address |
| Import cost, 256 MiB, pages never touched / touched | memfd 52-67 / 7-13 ms; anonymous or Wine memory 22-30 / 4-6 ms |
| vkd3d-proton `OpenExistingHeapFromAddress` on a bridge view | **E_INVALIDARG**. The function runs `VirtualQuery` and requires `AllocationBase == address` and `MEM_COMMIT`; Wine reports `MEM_RESERVE` for the window |
| vkd3d-proton `OpenExistingHeapFromAddress1` (`ID3D12Device13`) on a bridge view or an alias | **OK, the heap is the bridge's memory**. This function never queries; it imports `address` directly |
| Both functions on a bridge view placed over memory Wine committed (Wine reports `MEM_COMMIT`) | OK |
| `OpenExistingHeapFromFileMapping` | Not usable: it needs a Wine section handle, and a memfd pool has none |

Caveat: vkd3d-proton falls back without any message to an ordinary allocation when a host import
fails. The heap still opens, but the GPU no longer sees the title's memory. Code on bridged ranges
must not take a successful open as proof that the import worked.

**Decides (WS7): yes, through `OpenExistingHeapFromAddress1` only.** NVIDIA 620 imports
memfd-backed shared views, and so do winevulkan and vkd3d-proton. The bridge does not need to
answer `VirtualQuery` for this, and does not need to hook it. Changes needed in `inplace.rs`:
- On bridged ranges, use `From::Part` (`OpenExistingHeapFromAddress1`) and never `From::Region`
  or `From::Section`. Skip the `VirtualQuery` checks (`private_whole`, `kind_of`) and ask
  `victory_bridge::query` instead.
- Wine no longer guards a heap's pages, so the bridge has to. Today Wine refuses to unmap or
  protect pages under a heap. The bridge does not refuse: an unmap or remap under a standing heap
  succeeds, and the GPU keeps the old pool pages.
  - Rule 1: before the bridge remaps a view inside an opened part, drop the heap or re-open it.
    The existing `views_gone` / `title_memory_freeing` notices are the hook for this.
  - Rule 2: never punch holes in, or decommit, pool pages a heap holds. Otherwise the GPU and the
    CPU silently diverge. This rule is untested.
- An import pins every page, about twice the cost of anonymous memory for pages never touched.
  Open heaps over parts that are already resident, not over whole pools.

If `From::Region` is ever needed on bridged memory, two routes exist. Both are workable, but
neither is needed:
- Place the bridge's view over a range Wine committed. This case was measured OK, but Wine then
  keeps a view list entry for the range.
- Patch the `VirtualQuery` entry in vkd3d-proton's import table.

### S4 results

(pending)
