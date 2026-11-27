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

Done 2026-11-27, `tools/bridge-spike/s1/` (README there: build, run, what each test does). Proton
Experimental 11.0-20261112, kernel 6.18.65, 16 threads, a throwaway prefix, the game not running.
The same driver runs twice: as a Windows exe under Proton (the PE half calls the Unix half; **all
writes are made by PE threads**; a vectored handler counts every exception) and natively (links
the Unix half directly). uffd opened with `UFFD_USER_MODE_ONLY`; all of `WP_ASYNC`,
`WP_UNPOPULATED` and `WP_HUGETLBFS_SHMEM` are available and enabled. Window: 60,000 views of
64 KiB (3.66 GiB) over a 512 MiB memfd, view i at offset (i % 8192) × 64 KiB (aliasing). Numbers
are medians of three alternating runs started on an idle machine (one Proton run was disturbed by
other sessions' builds at about 2×; the median drops it).

| At 60,000 views of 64 KiB | Native | Under Proton |
|---|---|---|
| mmap of a view (no uffd) / unmap to PROT_NONE, in sequence | 0.72 / 0.87 µs | 0.76 / 0.92 µs |
| + `UFFDIO_REGISTER(WP)` per view / + `UFFDIO_WRITEPROTECT` per view | +0.49 / +0.28 µs | +0.47 / +0.28 µs |
| churn at full occupancy, random view unmapped + mapped again: no uffd / registered + armed | 2.23 / 3.27 µs | 2.49 / 3.15 µs |
| scan of the whole window, nothing written (unpopulated / 150k pages populated) | 1.62 / 1.80 ms | 1.67 / 2.33 ms |
| scan, 100 pages written | 1.73 ms | 2.10 ms |
| scan, 1 page written in each of the 60,000 views (56,251 regions) | 4.47 ms | 4.38 ms |
| scan, all 16 pages of every 10th view (96,000 pages, 6,000 regions) | 4.23 ms | 4.58 ms |
| first write to an armed populated page / same page writable | 443 / 10 ns | 495 / 12 ns |
| first write after a fresh map (pool page exists, no PTE): registered+armed / no uffd | 987 / 581 ns | 1210 / 646 ns |
| private 3.66 GiB, one range registered+armed: scan with nothing written | 1.34 ms | 1.64 ms |
| Wine's `GetWriteWatch(RESET)` on a 3.66 GiB `MEM_WRITE_WATCH` region: nothing / 100 / 48,000 written | — | 1.50 / 1.71 / 2.66 ms |
| first write to a page watched by Wine (`MEM_WRITE_WATCH`, after reset) | — | 454 ns |

Answers:

- **It works inside a Proton process**, on memfd `MAP_SHARED` views mapped `MAP_FIXED` over
  Wine-reserved space and on Wine's private memory (`VirtualAlloc(MEM_COMMIT)`). Writes from PE
  code (4-6 threads) into armed pages go through with no exception (the vectored handler saw 0 in
  every run, natively no SIGSEGV), costing one kernel minor fault (~0.45 µs, no signal). One scan
  returns exactly the written pages at 4 KiB granularity (no page missing, none extra, unpopulated
  and populated pages alike) and re-arms them: the next scan with no writes returns 0. Reads never
  count as writes. 6 writers against back-to-back scans for 300 ms (≈3 million writes, 13,000
  scans): no write lost, none invented. Nothing odd from Wine: no signals, `wineserver` untouched,
  threads fine; the Proton numbers equal the native ones on a quiet machine.
- **Registering once over the reserved range and mapping inside it does not work**: the `MAP_FIXED`
  mmap replaces the VMA and drops the registration (writes to such views are never reported). So
  each view must be registered (and armed) right after its mmap: +0.75 µs per view (≈+1 µs per
  unmap+map cycle at full occupancy, because unmapping a registered and armed view costs more).
  An unmapped view (back to PROT_NONE anon) is unregistered, which is fine.
- **Aliases are tracked per mapping, not per pool page**: a write through view A is reported at
  A's addresses only; the alias B of the same memfd offset sees the data but is not reported.
  Unmap + map again (registered + armed) starts clean, data kept.
- **`PM_SCAN_CHECK_WPASYNC` is dangerous**: a scan that meets an unregistered VMA fails with EPERM
  *after* having re-armed the pages it already walked, whose writes are then lost (checked: a write
  before such a failed scan was not reported by the next scan). Without the flag,
  `PM_SCAN_WP_MATCHING` silently skips unregistered VMAs.
- **Scan cost** is a page-table walk proportional to the address range scanned (~0.45 ms per GiB
  armed, nothing written) plus ~50 ns per region returned; whole-window scans every frame would
  cost 1.6-4.5 ms.
- **Private memory**: our registration works, survives `VirtualProtect`, but is **lost** on
  `MEM_DECOMMIT` + `MEM_COMMIT` (Wine re-mmaps). And Proton's Wine **already implements
  `MEM_WRITE_WATCH` with its own uffd in async mode** (our `UFFDIO_REGISTER` there gives EBUSY;
  first write 454 ns, no exception; `GetWriteWatch` costs the same as our scan).
- Wine loads a builtin DLL's Unix half from beside the exe only with `WINEDLLPATH` naming that
  directory (also true of the first spike); without it `MemoryWineUnixFuncs` fails (WS2/WS9).
- Not verified: the cost of today's path (64 KiB guards + vectored handler + `VirtualProtect`)
  measured the same way; memfd hugepages; `PAGEMAP_SCAN` racing an unmap of the same range from
  another thread; skipping the per-view arm (on an anonymous reserved range a registered but
  unarmed range reports every page as written; not checked on memfd views).

**Decides (WS5):** register and arm **per view at map time**, inside the bridge's map batch
(`UFFDIO_REGISTER(MODE_WP)` + `UFFDIO_WRITEPROTECT` on the view's range right after its mmap,
~0.75 µs; at today's ~2,100 remap calls per frame ≈ 1.5-2 ms per frame of extra Unix-side work,
against Wine's 337 µs per map+unmap it replaces). Never register "the window" and rely on it.
`watch_take(addr, len)` = one `PAGEMAP_SCAN` with `PM_SCAN_WP_MATCHING`, category
`PAGE_IS_WRITTEN`, **without** `PM_SCAN_CHECK_WPASYNC`, over only the ranges the watch asks about
(cost scales with the range, not with the window); results at 4 KiB, the journal may coarsen them.
The bridge must guarantee every mapped view in the window is registered (it is the only one that
maps there), and must translate written addresses to pool offsets and fan them out to all aliases
from xmem's page table (the kernel reports only the view written through). For **private memory**
keep `GetWriteWatch`: under Proton it is already uffd async (same cost, no faults), and our own
registration would silently drop on Wine's decommit/recommit. No blocker found.

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

Branch `memory-bridge-s4`, code in `tools/bridge-spike/s4/` (README has the full table and the
commands). Measured 2026-11-27 on this PC (i5-14400F, P-cores, kernel 6.18) under Proton
Experimental, from PE code, in a throwaway prefix. Four full runs plus three no-op-only runs; the
ranges cover the runs. 64 KiB views of one memfd in a `VirtualAlloc(MEM_RESERVE)` window. The
Wine path ran in the same session.

| Per op | 0 views | 60,000 views |
|---|---|---|
| No-op `__wine_unix_call`, 1 thread (tight loop) | 4.7 ns | |
| same, 4 / 8 threads calling at once | 5.3 / 6.5-8.4 ns | |
| Bridge map + unmap, 1 op per call | 1.59-1.77 µs | 1.75-1.88 µs |
| Bridge map + unmap, batches of 8 to 2048 | 1.42-1.62 µs | 1.61-1.82 µs |
| Bridge replace in place (`MAP_FIXED` over a view), any batch size | 0.79-1.12 µs | 0.95-1.07 µs |
| Bridge `mprotect`, 1 per call / batched | 222-246 / 192-219 ns | 220-241 / 192-222 ns |
| Per-op result array written back | no measurable cost (±5%, both ways) | same |
| Bridge map + unmap, 1 / 2 / 4 / 8 threads mapping at once (wall time per pair) | 1.5 / 2.2 / 2.4 / 2.8 µs | 1.6 / 2.1 / 2.3 / 2.8 µs |
| Wine `VirtualFree(MEM_PRESERVE_PLACEHOLDER)` split, median | 1.2 µs | 1.3-1.5 µs |
| Wine `MapViewOfFile3` + `UnmapViewOfFile2` | 15-16 µs | 300-590 µs |
| Wine `VirtualProtect`, no other thread busy | 0.56-0.58 µs | 0.57-0.65 µs |

Findings: a unix call costs about 5 ns. That is about 1/200 of one `mmap`, and it does not grow
with concurrent callers. Batching saves at most about 0.1 µs per op, and nothing beyond 8 ops per
call. Several threads mapping at once lower total throughput (the kernel's `mmap_lock`). At
60,000 views the bridge is 180-340 times faster than Wine's path. A view replaced in place costs
about 60% of an unmap followed by a map. Loading needs `WINEDLLPATH` naming the DLL's directory:
without it Wine loads the DLL as builtin, but `MemoryWineUnixFuncs` finds no Unix half. This also
applies to the parent spike's `load.exe`.

**Decides (WS0):**
- *Batch layout:* one op is 32 bytes: `{ addr: u64, len: u64, offset: u64 /* in the pool */,
  prot: u32 /* PROT_READ|PROT_WRITE bits */, kind: u32 /* MAP=0, UNMAP=1, PROTECT=2 */ }`. Ops of
  different kinds can mix in one batch. The header is 32 bytes: `{ ops: u64 /* *const Op */,
  results: u64 /* *mut i32, errno per op, or 0 */, count: u32, done: u32 /* out */, pool: i32,
  error: i32 /* out: errno of the first failure */ }`. Without `results` the Unix half stops at
  the first failure (`done`, `error`). With it, every op runs and gets its own errno. The Unix
  half checks every op against the window it was given at pool creation.
- *MAP means `MAP_FIXED` over whatever is there.* xmem's `had` + slice (unmap, then map the page
  again) becomes one MAP op, not UNMAP + MAP. Placeholder splits disappear entirely.
- *Batch size:* the size makes no difference to speed, so batch by structure: **one bridge call
  per `change` call** (typically up to ~2,100 ops per frame), one per `map`/`unmap`/alias
  operation, one per protect list. Cap a call at 4,096 ops (about 4 ms) and split longer lists
  into several calls. That cap only bounds latency; no minimum batch is needed. Single-op calls
  are fine for the rare paths, such as the title's own `VirtualProtect`.
- *Threads:* do the bridge batch on the calling thread. Do not hand it to `change`'s worker pool:
  extra threads cost throughput. `VICTORY_MAP_THREADS` stops mattering on the bridge path.
- *Per-op result reporting:* it costs nothing measurable, so keep it in the ABI. xmem's `change`
  passes `results`, which keeps the per-op `done` it uses today to decide what to note. Other
  callers pass 0 and read `done`/`error`.
