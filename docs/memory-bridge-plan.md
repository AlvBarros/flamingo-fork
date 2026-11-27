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

2026-11-27, Proton Experimental (wine-11.0 base, vkd3d-proton 3.2.0 build 2576c0b), RTX 4070,
own throwaway prefix; `tools/bridge-spike/s2/` (code, build/run, full table). A 16 MiB window
reserved through Wine; the Unix half maps memfd views, unmaps to anonymous PROT_NONE and
`mprotect`s inside it. All checks pass; `WINEDEBUG=+seh,+virtual` confirms what Wine does.

| Operation on the window | What Wine does | Verdict |
|---|---|---|
| write to a view `mprotect`ed read-only | one `EXCEPTION_ACCESS_VIOLATION` to the first vectored handler, `info[0]=1`, `info[1]` = exact byte; nothing first (no guard-page, stack or write-watch handling). Handler `mprotect`s through the Unix half + `CONTINUE_EXECUTION`: the write lands. Same on a second thread, 8 threads at once, RIP inside the window | OK |
| read of an unmapped (PROT_NONE) or never-mapped window page | AV, `info[0]=0`, exact address; handler maps the view, the read sees the pool | OK |
| safecopy's `rep movsb` + handler | from a view: no fault; into an unmapped page: AV at the first missing byte, failing exit works; into a read-only view: handler opens it, copy completes | OK |
| `VirtualQuery` / `NtQueryVirtualMemory(MemoryBasicInformation)` | `MEM_RESERVE`, `Protect=0`, `AllocationProtect=PAGE_NOACCESS`, `AllocationBase` = window, region = queried page to window end, whatever is mapped; `MemoryWorkingSetExInformation`: not valid | must intercept (WS4) |
| `ReadProcessMemory` / `NtReadVirtualMemory`, own process | memcpy of the real pages under Wine's syscall fault handler: RW and R views read right; unmapped fails `STATUS_PARTIAL_COPY` (299); destination in the window works | OK |
| `NtWriteVirtualMemory`, own process | real pages: RW view lands; R view and unmapped fail `STATUS_PARTIAL_COPY` | OK |
| `WriteProcessMemory` | always `ERROR_NOACCESS` (998), even for an RW view: kernelbase asks `VirtualQuery` first and sees reserved | must intercept (use `NtWriteVirtualMemory` or the bridge) |
| `process_vm_readv` in the Unix half | views read; unmapped `EFAULT` | OK (WS6 bulk path) |
| `WriteFile` from / `ReadFile` into a view | mapped views work, data right; unmapped source `ERROR_INVALID_USER_BUFFER`, read-only/unmapped destination `ERROR_NOACCESS`; no fault, nothing remapped | OK (as Windows) |
| `IsBadReadPtr` / `IsBadWritePtr`, memcpy on 6 threads | probe the real protection (our handler sees the probe's AV first and must decline it); memcpy fine | OK |
| `GetWriteWatch` | window without `MEM_WRITE_WATCH`: `ERROR_INVALID_PARAMETER`; a `MEM_WRITE_WATCH` reserve with a written bridge view: 0 pages | must intercept (WS5: uffd) |
| `VirtualProtect` on the window | fails `ERROR_INVALID_ADDRESS` (not committed); view untouched | must intercept |
| `VirtualAlloc(MEM_RESERVE)` at a window address | fails (already reserved) | OK |
| `VirtualAlloc(MEM_COMMIT)` on a view | **succeeds**: Wine marks the pages committed and `mprotect`s our mapping (bytes kept); afterwards Wine's `VirtualProtect` there succeeds and `mprotect`s our memfd mapping, and Wine's view and ours disagree | must intercept |
| `VirtualFree(MEM_DECOMMIT)` on a view | **succeeds, replaces our view with anonymous PROT_NONE** (pool bytes stay in the memfd) | must intercept |
| `VirtualFree(MEM_RELEASE)` of the window | **succeeds, munmaps every view**; the next `VirtualAlloc` lands there | must intercept; only the bridge releases its window |
| `SuspendThread` / `GetThreadContext` on a thread inside the Unix call | answer at once (0.1-0.3 ms); context is the PE side (dispatcher return). The thread is frozen **inside** the Unix call (signal): it stops mid-batch, its syscalls get `EINTR`, it finishes after the resume | OK with care (WS1) |
| D3D12: memcpy from 16 read-only bridge views into an upload buffer, copy to default, read back | bytes equal | OK |
| `ID3D12Device3::OpenExistingHeapFromAddress` on a bridge view | `E_INVALIDARG` (vkd3d-proton sizes it with `VirtualQuery` and sees reserved); control on `VirtualAlloc`'d memory works | blocker for WS7 as is (see S3) |

Run note for WS2/WS9: Wine finds a builtin's Unix half only through `WINEDLLPATH` (the directory
holding the `.so`); without it `NtQueryVirtualMemory(MemoryWineUnixFuncs)` fails and the bridge
must report itself unavailable. (The first spike's `load.exe` also needs it.)

**Decides.** WS4: inside the window every Wine answer about state is wrong (reserved, no access),
and three calls actively damage it, so the bridge must answer or refuse, for any address with
`bridged(addr)`: `VirtualQuery` / `NtQueryVirtualMemory` (basic and working-set classes),
`VirtualProtect` / `NtProtectVirtualMemory`, `VirtualAlloc(MEM_COMMIT)` (must not reach Wine:
it `mprotect`s our views and makes Wine think the pages are committed), `VirtualFree(MEM_DECOMMIT)`
and `VirtualFree(MEM_RELEASE)` (they destroy views: answer from the bridge, release the window
only when the pool goes), `GetWriteWatch` / `NtGetWriteWatch` (WS5), `WriteProcessMemory` (fails;
use `NtWriteVirtualMemory`). This covers our own sites and the title's imports bound by
`victory-loader`; anything else in the process (vkd3d-proton's `OpenExistingHeapFromAddress`,
other DLLs) still sees reserved, so in-place heaps stay off bridged ranges until WS7/S3. Calls
that need no interception: faults, `ReadProcessMemory` / `NtReadVirtualMemory`,
`NtWriteVirtualMemory`, file I/O, `IsBad*Ptr`, `VirtualAlloc(MEM_RESERVE)` (refused). WS6: safecopy's
`rep movsb` + vectored-handler design works unchanged on bridge pages (AV with exact address and
read/write flag, `CONTINUE_EXECUTION` after a bridge `mprotect`/map, RIP redirect to the failing
exit), and the `ReadProcessMemory` fallback (`VICTORY_SAFE_COPY=0`) and `host.rs`'s
`NtWriteVirtualMemory` work too, because Wine copies the real pages; only the `WriteProcessMemory`
fallback in `host.rs` fails. But `on_write`'s own checks `writable()`, `readable_view()` (and so
`came_back`) use `VirtualQuery`, which always says reserved there: they must ask
`victory_bridge::query` when bridged, or the handler sends those faults to `CONTINUE_SEARCH`.
WS1: a thread can be suspended in the middle of a Unix call, so the Unix half must retry on
`EINTR`, and must not hold a lock that a suspending thread could then wait for in its own bridge
call (or must make batches restartable); it must never fault itself (not tested here; Wine
does not hand a fault in Unix code to our handler as an ordinary AV), so copies that may meet an unmapped page use `process_vm_readv`.

### S3 results

(pending)

### S4 results

(pending)
