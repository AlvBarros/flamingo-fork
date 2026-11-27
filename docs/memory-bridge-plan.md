# Memory bridge: plan (Linux fork)

Status: plan, 2026-11-27. Base: this repository's `main` (trustno1-coder/VIctory-NVIDIA with the
Linux fork's scripts and fixes). Written to be picked up by separate sessions or agents: each
workstream below states what it owns, what it may assume and how it is checked.

> **Re-scope first (2026-11-27).** This plan (sections 1-9) was written for the previous base
> (the old VIctory-NVIDIA tree). `main` now stands on readmet3xt-lab/foxhound via the new
> VIctory-NVIDIA, whose `VICTORY_SOFT_VIEWS=copies` keeps the title's page mappings out of Windows
> views. Measured under Proton (cutscene stretch, warm): frame mean 193 -> 82 ms, frames over
> 250 ms 17% -> 5%, none over 1 s. Milestone M1 (remapping) is largely done by that. Before more
> bridge work: measure where the remaining slow frames go on this base (foxhound's notes: mostly
> pipeline creation), then keep only what still pays (likely WS5, write tracking; maybe nothing).
> File and line references point at the old tree and must be re-checked; xmem.rs here has soft
> views. The bridge crate and spikes (WS0, S1-S4, section 10) came over unchanged from the branch
> `memory-bridge`; the old base is on `main-before-foxhound-base`.

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
| WS0 contract | coordinator | `memory-bridge` | done (1463fc3; MAP_WATCH and replace-in-place from S1/S4) |
| S1 uffd WP-async + PAGEMAP_SCAN under Proton | agent | `memory-bridge-s1` | done, merged (5a8aa73): works, no blocker |
| S2 Wine's behaviour on the window | agent | `memory-bridge-s2` | done, merged (21318ee): faults OK; 7 Wine calls must be intercepted |
| S3 external_memory_host on memfd (NVIDIA) | agent | `memory-bridge-s3` | done, merged (301052b): yes, via `OpenExistingHeapFromAddress1` only |
| S4 cost of `__wine_unix_call` and batches | agent | `memory-bridge-s4` | done, merged (81d59b4): ~5 ns a call; 180-340x Wine at 60k views |
| WS1-WS9 | — | — | M0 reached; waiting for the go to start |

### Runs

(Announce each game run here, with start time and who, before starting it.)

- none yet

### WS0 results

Done 2026-11-27 on `memory-bridge` (commit 1463fc3, plus the S1/S4 follow-ups in the same
crate). What is fixed for everyone else:

- `crates/victory-bridge/src/abi.rs` (authority) and `bridge/unix/abi.h` (mirror; a Rust test
  checks its numbers, `_Static_assert`s its layout). One PE export, `victorybridge_call(u32 code,
  void *args)`, forwarding to `__wine_unix_call`. Calls 0-10: `VERSION`, `POOL_CREATE/DESTROY`,
  `WINDOW_ADD/REMOVE`, `MAP`, `UNMAP`, `PROTECT`, `WATCH_REGISTER`, `WATCH_TAKE`, `COPY`. Batches:
  `{pool, ops*, count, results* (i32 errno per op), done}`, at most 4,096 ops per call (S4: no
  minimum needed, results cost nothing). `MAP` is one `MAP_FIXED` mmap over whatever is there
  (S4: 0.8-1.1 µs, no unmap first) and `MAP_WATCH` arms write tracking on the new view in the same
  call (S1: a `MAP_FIXED` mmap drops earlier registrations). Protections are Linux `PROT_*` bits.
- `crates/victory-bridge/src/lib.rs`: `available()`, `bridged(addr)`, `window_add/remove`,
  `PagePool` + `Pool` (`create/map/unmap`, one call per batch), `unmap`, `protect`, `query` (from
  the bridge's own map, `map.rs`, never Wine's), `watch_register`, `watch_take` (pages written
  *through* the range; aliases are the caller's, S1), `copy`, and drop-ins `virtual_query` /
  `virtual_protect` that answer for windows and pass anything else to Wine unchanged. The loader
  looks for `victorybridge.dll` beside `victory.exe` under Wine only and checks the ABI version
  (WS2's Rust side is therefore done; WS2 is the C PE half).
- Off unless `VICTORY_BRIDGE` is set and not `0`; then still off without Wine, the DLL or the
  right ABI version (a warning says so). Off, every function answers "not mine" and nothing
  changes. `victory-hle` and `victory-gpu` depend on the crate; nothing calls it yet.
- Tests: 19 in the crate (the map; the marshalling against a fake Unix half: batch splitting,
  refusals, map kept in step, version check, missing features; the drop-ins passing other memory
  through, Windows only). Natively and under Proton.
- Whole suite under Proton (`scripts/linux/test.sh`), `VICTORY_BRIDGE=0` and `=1`, against the
  base without WS0 (9d6f4df, run twice): the base fails the same 12 tests every time (7 in
  `victory-gpu` incl. 3 in `watch`, 2 in `victory-hle`, 2 in `victory-shader`, the loader's probe
  fixture, xtask's policy test under Wine); with WS0 both ways fail those 12 and nothing else
  except one timing-sensitive test per run (`other_memory_at_a_guarded_address_keeps_its_protection`,
  `views_off_their_elements_get_a_copy_of_their_bytes`), which pass 12/12 alone both ways (the
  full runs shared the machine with the four spikes). Pre-existing failures are not WS0's to fix.
- Found on the way, fixed in a separate commit (9d6f4df): two stale test call sites in
  `victory-gpu` (`descriptor.rs`) and `victory-shader` (`valu_tests.rs`) stopped the suite from
  compiling on this base. Not fixed: `scripts/linux/build.sh` (and so `test.sh`) defaults to the
  `victory-hle/keys` feature this base lacks; tests run with `FORK_FEATURES=""`.

### M0: what the spikes decide (coordinator's summary)

- **Go.** Nothing blocks the bridge: uffd async works under Proton (S1), faults in the window
  reach our handler and safecopy works unchanged (S2), the bridge maps 180-340x faster than Wine at
  60,000 views and a unix call costs ~5 ns (S4), and the GPU imports bridge memory (S3).
- **WS1** (Unix half): one `MAP_FIXED` mmap per `MAP` op, register + arm per view when
  `MAP_WATCH` (+0.75 µs); scan with `PM_SCAN_WP_MATCHING`, category `PAGE_IS_WRITTEN`, **never
  `PM_SCAN_CHECK_WPASYNC`** (it loses writes on EPERM); retry on `EINTR` (a suspended thread is
  frozen inside the call); hold no lock a suspended thread could leave held across a syscall that
  can block for long; never fault in the Unix half (copies use `process_vm_readv`).
- **WS2/WS9**: Wine finds a builtin's Unix half only through **`WINEDLLPATH`** naming the `.so`'s
  directory (beside the `.dll` is not enough); `run.sh` must set it. vkd3d-proton's `d3d12core`
  needs DXVK's `dxgi` in a fresh prefix (the game prefix has it already).
- **WS3**: one bridge `MAP` call per xmem `change` on the calling thread (not the worker pool;
  several mapping threads lose to the kernel's `mmap_lock`, S4); a remap is one `MAP` op (no
  unmap, no placeholder split); per-op results kept for `change`'s `done`.
- **WS4**: inside a window, these must never reach Wine (S2): `VirtualQuery` /
  `NtQueryVirtualMemory`, `VirtualProtect` / `NtProtectVirtualMemory`, `VirtualAlloc(MEM_COMMIT)`
  (Wine succeeds and then believes the pages are its own), `VirtualFree(MEM_DECOMMIT)` (replaces
  our view), `VirtualFree(MEM_RELEASE)` (unmaps every view; only `window_remove` then Wine's
  release), `GetWriteWatch` (sees nothing), `WriteProcessMemory` (always fails: use
  `NtWriteVirtualMemory`). Our call sites and the title's imports.
- **WS5**: per-view arming at map time (`MAP_WATCH`); `watch_take` scans only the asked ranges
  (~0.45 ms per GiB); aliases fanned out from xmem's page table; private memory keeps
  `GetWriteWatch` (Proton already does it with uffd async).
- **WS6**: safecopy and the `ReadProcessMemory` fallback work as they are; `on_write`'s
  `writable()` / `readable_view()` / `came_back` must ask `victory_bridge::query` for bridged
  addresses (Wine says reserved there); `host.rs`'s `WriteProcessMemory` fallback must become
  `NtWriteVirtualMemory`.
- **WS7**: yes, but only through `OpenExistingHeapFromAddress1` (`From::Part`); plain
  `OpenExistingHeapFromAddress` and `...FromFileMapping` cannot work on bridged memory (S2 and S3
  agree). A failed host import silently falls back to an ordinary allocation in vkd3d-proton:
  verify contents, not the open. Heaps pin pages: drop/re-open before the bridge remaps inside one.

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

## 11. Progress on the foxhound base

Branch `memory-bridge-foxhound` (from `linux-foxhound-base`, b3fc71c), started 2026-11-27. Same
rules as §10: `VICTORY_BRIDGE` off by default and `=0` unchanged; tests both ways; `cargo xtask
policy` before every push; game runs announced here and never overlapping another session's.
Parallel agents work on `memory-bridge-foxhound-<ws>` and append their own subsection at the end.

### Status

| Item | Owner | Branch | State |
|---|---|---|---|
| Re-scope: where do this base's slow frames go | coordinator | `memory-bridge-foxhound` | done: go (17 ms a usual frame, ~119 of a long frame's 145 on the critical path) |
| WS8 frame log ported to this base | agent | `memory-bridge-foxhound-ws8` | done, merged (76498ca) |
| WS1 Unix half | agent | `memory-bridge-foxhound-ws1` | done, merged (9a7419f) |
| WS2+WS9 PE half, loading, build and packaging | agent | `memory-bridge-foxhound-ws2` | done, merged (5569d78) |
| WS3 xmem on the bridge | agent | `memory-bridge-foxhound-ws3` | done, merged (8826ac1) |
| WS4 queries and protections | agent | `memory-bridge-foxhound-ws4` | done, merged (4778063) |
| WS5, WS6 | — | — | after WS3 |
| WS7 | — | — | later (S3: `OpenExistingHeapFromAddress1` only) |

### Runs

- 2026-11-27 ~12:10: another session is running `flamingo-main` (prebuild, then a 10-minute play
  run, prefix `flamingo-fork-foxhound`). No run of ours meanwhile; its log, with this base's
  current settings, is the re-scope's first data.
- 2026-11-27 (this session): **run announced** — `memory-bridge-foxhound` at 76498ca (WS8 frame
  log, `VICTORY_FRAME_LOG=1`, `VICTORY_BRIDGE` unset), `play.env` as committed, unattended
  (`FORK_KEYS`), up to 11 min, home `flamingo-fork-foxhound` (warm caches, as the 11:12 run).
  Done 13:24-13:35, no crash (`logs/20261127-132421-play.log`).
- 2026-11-27 (this session): **run announced** — the same, plus the frame log's split of the
  memory watch's costs on the frame's own threads (preparing, presenting, submitting), to measure
  what the bridge could take off the frame's critical path. Done 13:39-13:50, no crash.
- 2026-11-27 (this session): **run announced** — e8f663f, **`VICTORY_BRIDGE=1`** (first game run on the bridge), otherwise as the 13:39 run; 5 min unattended (short runs from now on).
  Done 14:18-14:24: `memory bridge on (VICTORY_BRIDGE) features=3`, no crash.

- 2026-11-27 18:47 (another session): an A/B meant as bridge off vs on, 10 min each, prefix
  `flamingo-fork-foxhound`. **Both runs had the bridge on**: `scripts/linux/play.local.env` in this
  folder sets `VICTORY_BRIDGE=1`, and unsetting it in the environment does not override the file
  (the log header says which applies). **The PC froze**: when the first run was ended with
  `wineserver -k` (18:57:33), the card reported `Xid 31` (MMU fault, `title-main`) in the same
  second, and NVIDIA's driver then spun in its cleanup of the dying process
  (`nvidia_close -> ... intermapDelDmaMapping -> btreeUnlink`, soft lockup on one CPU for 17+
  minutes, thread `vkd3d_fence`), which held the GPU: black screen, no input, a hard reboot.
  Xid 31 at a forced close happened often before the bridge (2026-11-25/26) without a lockup; the
  lockup in the DMA-mapping teardown is new, so the bridge's memfd-backed title views are suspect
  (not proven: one occurrence). **Until understood:** no `wineserver -k` on a bridge run while the
  card is busy (end runs gracefully), and the bridge stays off by default. To find out: which title
  pages the GPU has DMA-mapped (vkd3d host-pointer imports, `OpenExistingHeapFromAddress1`, upload
  heaps over title memory) and whether a bridge view is unmapped or its memfd closed while they stand.

### Re-scope

- The last two logs that ran this base (`b190242`, 2026-11-27 09:20 and 09:30) ran **without**
  `VICTORY_SOFT_VIEWS` and at 720p, so they do not represent the tuned base. For the record, in
  them write tracking was still large: per frame, all threads, median / p90: kernel askings
  6-9 / 80-160 ms, waits for the guards 11-12 / 56-90 ms, protection changes 7-9 / 43-45 ms; on
  the submitting thread 4-5 / 36-46 ms asking the write watch; copies 22-25 / 250-885 ms.
- **This base, tuned settings** (`logs/20261127-111246-play.log`, another session's run, soft
  views on, 1080p; whole run, periodic per-frame averages, not yet the cutscene stretch alone):
  frame median 64 ms, mean 119, p90 207, max 1.9 s. Per frame, all threads, median / p90: **waits
  for the guards 32 / 144 ms** (max 906), protection changes 11 / 41 ms, kernel askings 5 / 49 ms,
  refreshes' watch+guards 4 / 39 ms; copies 32 / 129 ms. Remapping no longer shows (soft views).
  First reading: write tracking (guards + protections + askings, ~50 ms a frame summed over
  threads at the median) is what is left of Wine's memory costs, i.e. **WS5 still pays; WS3 (xmem
  remapping) likely does not**. To confirm per long frame with the WS8 frame log before starting.

- **Measured with the WS8 frame log** (`logs/20261127-132421-play.log`, 76498ca + doc, 11 min
  unattended, warm, no crash; `ab_runs.py`, cutscene stretch 60-480 s, 7,463 frames): frame mean
  56 ms, median 55, p90 68, **p99 81, max 483; none over 250 ms; 35 long frames, 1.1% of the
  time**. This base no longer stutters the way §1 describes; what is left is throughput.
  - Usual frame (55.6 ms): present 18.5 ms, preparing 34.2 (copies 24.0 ms, 34 MB), card idle
    13.2 (11.4 of it behind us). Memory costs, **summed over all threads**: waits for the guards
    74.9 ms (804 waits), region queries 28.6 ms (**11,143 `VirtualQuery` a frame**), protection
    changes 9.2 ms (553), write-watch askings 3.9 ms (434, 1.97 GB asked), 319 guard faults;
    remapping 1.0 ms (soft views).
  - Long frames (median 115 ms): remapping bursts (median 97 calls, 50 ms; 59 Windows calls,
    36 ms: soft views made real), region queries 203 ms, guard waits 178 ms, copies 90 ms. The
    35 long frames hold 26% of all remap calls and 32% of the Windows mapping calls.
  - What the bridge could still take, by workstream: **WS5** (guards, protections, write watch:
    ~88 ms a frame over threads, the largest) and **WS4** (region queries: 29 ms a frame; for
    bridged memory answered from the bridge's own map, no Wine lock); **WS3** only for the long
    frames' remapping bursts (1% of the time). Not yet known: how much of the summed waits lies
    on the submitting/render thread's critical path (the log sums threads; the frame is bound by
    preparing 34 ms + present 18.5 ms). No switch turns the guards off for a cheap upper bound.

- **On the frame's critical path** (`logs/20261127-133935-play.log`, c463e1a: the frame log's
  split of the memory watch's costs on the threads that prepare, present or submit; same settings;
  no crash). This run was rougher than the 13:24 one (p99 194 ms against 81, max 1.06 s, 64 long
  frames, 4.5% of the time; run-to-run variance, more remapping: 3.1 ms a usual frame, 101 ms
  median in long frames), the usual frame alike (58 against 56 ms).
  - Usual frame (58.3 ms), on the frame's threads: region queries 8.0 ms, guard waits 5.0,
    write-watch askings 2.6, protections 0.7, other watch locks 0.9: **17.2 ms, ~30% of the
    frame**. Guard faults: none there (the title's threads take them).
  - Long frames (median 145.5 ms), on the frame's threads: region queries 65.0 ms, guard waits
    34.7, askings 16.1, protections 3.5: **~119 ms of the 145**; plus remapping (101 ms median,
    all threads: soft views made real).
  - **Decision: go** (the bar set before measuring was 10 ms a usual frame). Region queries come
    first (WS4, answered from the bridge's map once title memory is in bridge windows: WS3), then
    the guards and askings (WS5), and the long frames' remapping (WS3). Order: WS1, WS2+WS9, WS3,
    WS4 in parallel now; WS5, WS6 after WS3.

- **Merged** (coordinator): WS2+WS9, WS1, WS4, WS3. Fixed on the way in `victory-bridge`: runs in
  the map are joined (WS4 measured 132 µs a query at 60,000 views before), but never across
  windows (WS3's tests have adjacent windows: the first fix panicked there); a failed `MAP` op
  leaves its range unmapped (WS1); two tests that assumed the DLL never loads skip themselves when
  it does. With the real DLL beside the tests (`VICTORY_BRIDGE=1`, `test.sh`), `victory-bridge`
  30/30 and `victory-hle` 188 passed + the base's 2 failures, the same as `=0`. Open: one full
  `victory-bridge` run in about seven died with the real DLL and the emulator in one process
  (WS3's note: the emulator re-reserves a window as a placeholder, racy; tests only).

- **First game run on the bridge (M1)** (`logs/20261127-141835-play.log`, e8f663f, `VICTORY_BRIDGE=1`,
  5 min; against the 13:39 run without it, same settings, both under Proton; `ab_runs.py ... 0 300`,
  the first 300 s after drawing starts, i.e. the cutscene's loading of the cafeteria and prison):
  frames over 250 ms 3.3% -> **0.4%**, p99 491 -> **186 ms**, time in long frames 17.3% -> 7.3%,
  the worst minute's median 230 -> 67 ms; the usual frame unchanged (56 -> 59 ms). Per frame: region
  queries 48.6 ms (all threads; 9.3 on the frame's threads) -> **0.1** (0.0); Windows mapping calls
  gone; remapping in long frames 406 -> 13 ms median; copies 32 -> 18 ms usual, long -83%; write
  watch -60%. Unchanged: **guards** (waits 81 -> 114 ms all threads, ~5 ms on the frame's threads)
  and protection changes: WS5 next. Disk workers busier (13 -> 30 ms; streaming faster, to see).
  The user, watching: "the best the cutscene has been by far ... ~15 fps all the way, loading the
  cafeteria and prison and other NPCs". Next: the user's 15-minute hand-played session
  (`scripts/linux/play-session.sh 15`, `VICTORY_BRIDGE=1`), then WS5.

- **The user's 15-minute hand-played session on the bridge** (`logs/session-20261127-142939/`,
  report there): bridge on, no crash. Prison and cafeteria loading smooth (median 56-76 ms, worst
  586 ms after start). The laggy bank and police minutes (14:35-14:39, median 80-110 ms) coincide
  with the machine out of RAM (free ~0.3 GB, swap-out up to 240 MB/s, 10.8 GB of swap used) and
  VRAM full (11.9 of 12 GB); their long frames are DirectStorage disk work, guard waits and the
  card idle. Open: the frame log's free memory read ~3.5 GB lower on the bridge than without it
  at the same point, uncontrolled (other programs); not found by reading the code (Wine's
  sections are tmpfs here too, pool teardown looks right). The user accepts more RAM for now;
  measure later with the game process's own RssShmem/RssAnon, bridge off vs on, 4 min each.
  Next after that: WS5 (guards).

### WS8 results (foxhound base)

Done 2026-11-27 on `memory-bridge-foxhound-ws8`. The per-frame log of the Flamingo-based branch
(`linux-on-new-upstream`: `crates/victory-gpu/src/framelog.rs` and xmem's `timed`) is ported to
this base as `crates/victory-gpu/src/framelog.rs`, with this base's hook sites.

**Switch.** `VICTORY_FRAME_LOG=1` (any value but `0`) prints the lines; **off by default in every
build**, diagnostics included (the old branch had it on in diagnostics builds). The counters are
plain relaxed atomic adds (plus a clock read around the timed calls) in every build, switch on or
off; with the switch off nothing else changes. Every counter runs from the start and is never reset;
at the end of each present call the totals are read and their differences are that frame's.

**Lines** (target `victory_gpu::framelog`, every 5 s):

    frame times: <n> frames in <s> s: shortest <ms> ms, median <ms>, longest <ms>; <k> long (over <ms> ms, the usual frame of the last 64 being <ms>); each: <ms> <ms> ...
    usual frame (the mean of <n> not long): <make-up>
    long frame <number> at <s> s: <make-up>          (one line per long frame, at most 32 a reading)
    long frames: <k> more in this reading are not listed

A frame is long when it is over 1.5 times the usual frame and at least 40 ms over it (the usual
frame: the median of the last 64). `<make-up>` is, in one line, sections split by ` | `:

    <ms> ms | render thread: ran <ms> ms (user <ms>, kernel <ms>); in the present call <ms> (copy and show <ms>), in submit calls <ms>, paced <ms>
    | submissions: <n>, preparing <ms> ms, waiting for the card <ms>; copies <ms> ms <MB> MB; copy-back <ms> ms <MB> MB
    | card: idle <ms> ms (<ms> behind us) | all submit calls <ms> ms
    | textures: refresh <ms> ms (<ms> waiting for another thread's refresh, <ms> asking the watch, <ms> waiting for memory to hold still), <n> uploads
    | remapping: <n> calls <ms> ms (<ms> waiting for readers)
    | xmem: <n> mapping calls <ms> ms; Windows calls <n> <ms> ms; the GPU side told <n> <ms> ms
    | memory watch, all threads: <n> write-watch askings (<n> refused) <ms> ms over <MB> MB; <n> protection changes <ms> ms; <n> region queries <ms> ms; waits for the guards <n> <ms> ms, for its other locks <ms> ms; <n> guard faults
    | pipelines: <ms> ms waited | disk: <n> reads <MB> MB <ms> worker ms | pacing, all threads <ms> ms
    | process: <ms> user <ms> kernel ms, <n> page faults | <n> draws | <MB> MB free

"Render thread" columns are the presenting thread's own; all others are summed over threads (so
they can exceed the frame's length).

**Where each column is counted** (all through `framelog::add`/`timed`, or `framelog::Counter`,
which stands in for the `AtomicU64` of an existing frame-rate-line counter and adds to a frame-log
total as well, so the sites stayed as they were):

| Column | Counted in |
|---|---|
| present call; copy and show; frame end | `queue.rs`: `present` (wraps `present_now`), `show_planes` call in `present_now`; `framelog::frame` |
| submit calls (all, render thread's) | `queue.rs`: `execute` (wraps `execute_now`) |
| paced | `frame.rs`: `wait` |
| submissions, preparing, waiting for the card | `host.rs`: `SUBMISSION_US` (Counter; adds at the submission sites as before) |
| copies ms/MB; copy-back ms/MB | `host.rs`: `PREPARING[0..2]`, `COPY_BACK` (Counters) |
| card idle (behind us) | `host.rs`: `CARD_CLOCK_US[0..2]` (Counter), `Host::card_sent` |
| texture refresh and its waits | `texture.rs`: `REFRESH_WAITS` (Counter; `waited`, `refresh_now`) |
| uploads | `texture.rs`: `upload_from`, `finish_refresh` |
| pipelines waited | `pipeline/make.rs`: `wait` |
| remapping calls, ms, wait for readers | `watch.rs`: `title_memory_remapping`, `Remapping::drop` |
| xmem mapping calls | `xmem.rs`: `Costed` (now always made): `map_pages` (`XMemMapPhysicalPages`), `map_alias`, `map_aliases`, `scatter` |
| Windows calls | `xmem.rs`: `counted` (unmap, split/coalesce, map, commit; every such call of xmem's, including views made real after a fault with `VICTORY_SOFT_VIEWS` and commits) |
| the GPU side told | `xmem.rs`: `told` around `title_memory_remapped/_view/_unmapping/_splitting/_made_real` |
| write-watch askings (refused, MB, ms) | `watch.rs`: `kernel_written` (the `GetWriteWatch` call) |
| protection changes; region queries | `watch.rs`: `sys::VirtualProtect`/`sys::VirtualQuery`, imported in place of windows-sys' by every non-test function of watch.rs (`open_unlocked`, `protect_alias`, `protect_here`, `unguard`; `is_view`, `uncommitted_end`, `readable_view`, the allocation-end query, `writable`) |
| waits for the guards; for its other locks | `watch.rs`: `waited` (now in every build): `what == 3` is the guards' lock found held (the fault handler's waits among them), the rest the ranges held and the journal |
| guard faults | `watch.rs`: `install`'s `on_write`, both places a write fault opens a guarded block |
| disk | `dstorage.rs`: `Lanes::count` |
| process, page faults, draws, free MB, render thread ran | sampled at each present (`framelog::sample`) |

**Not on this base, left out:** the card's own clock by kind of list (no `cardmeter.rs`); the
looks at slow cores (no `spread.rs`; `cores.rs` differs); the submit thread's own time
(`VICTORY_ASYNC_SUBMIT`'s worker here is `queue/behind.rs`, not hooked); recording calls (here
`copystat`, diagnostics only); pipelines made/loaded and staging made/reused (no
`frame_log_counts`/`SPARE_STATS`); the hot set and `VICTORY_VERIFY` counts; the present's
"lines and settling" split (this base's present is laid out differently: the whole call and its
copy-and-show only).

**Reading them.** `scripts/linux/ab_runs.py <log A> [<log B>] [from s] [to s] [min draws]` now
reads all of the make-up's columns. It prints the frame times of the stretch (mean, median, p90,
p99, max, share over 250 ms and 1 s, long frames, the share of the time spent in long frames) and,
per column, the mean in usual frames (weighted by the frames each line stands for), the median and
sum over the long frames listed, and the long frames' share of the column's total; with two logs,
how B differs from A. For the bridge: compare the memory columns (remapping, xmem, memory watch)
between long and usual frames; a column whose long-frame median is small against the frame says
the bridge would not shorten those frames.

**Tests.** Eight new unit tests in `framelog` (no GPU): line form and the usual/long split,
the long-frame limit, more long frames than listed, a reading with only long frames, the usual
frame's median over the last 64, a frame as the difference of two readings (negative after a
correction, free memory not a difference, thread times dropped when another thread presented),
the memory columns in the line, totals/`Counter`/`timed` arithmetic. `scripts/linux/test.sh`
(`FORK_FEATURES=""`, under Proton, while another session ran the game on the same GPU), lib tests,
one thread, against the base b3fc71c built the same way:
- `victory-gpu`: four tests kill the test process on both (skipped:
  `host::tests::warp_adapter_accepts_only_software`, `indirect::tests::card_runs_wide_indirect_dispatches`,
  `indirect::tests::warp_a_group_charges_its_records_in_lane_order`,
  `indirect::tests::warp_a_record_that_does_not_fit_may_take_nothing`). Base: 408 passed, 4 failed
  (`watch::many_askers_tests::{bits_and_holes_with_faults_that_take_no_lock,
  holes_are_known_until_the_runtime_tells, no_mapping_is_lost_between_askers}`,
  `watch::tests::what_is_left_alone_is_no_known_hole`). With WS8: 413 passed (the 8 new ones
  among them), the same 4 failed, and 3 more in the full run
  (`descriptor::host::tests::views_are_made_beside_the_heap_and_copied_in`,
  `indirect::tests::host_list_budget_starts_empty_at_each_submission`,
  `pipeline::library::tests::a_rejected_blob_gives_an_empty_library`), which fail the same way
  on the base when run alone (twice each), so they depend on the machine's state, not on this change.
- `victory-hle`: 169 passed, 2 failed on both (`modules::net::tests::only_loopback_is_reachable`,
  `modules::xgamesave::tests::a_kept_blob_past_the_link_limit_is_copied`).
- `FORK_FEATURES=diagnostics scripts/linux/build.sh` builds; `cargo xtask policy` passes; no new
  clippy warnings in the touched files; `framelog.rs` is rustfmt-clean (the tree as a whole is not).
- Not verified: the lines in a game run (no game run by this workstream; the coordinator owns
  them). `ab_runs.py` was checked against lines printed by the unit test's own output.

### WS2+WS9 results (foxhound base)

Done 2026-11-27 on `memory-bridge-foxhound-ws2`. Proton Experimental (wine-11.0,
experimental-11.0-20261123b), MinGW-w64 gcc, system `winebuild` 11.22; no game run.

**Built.**
- `bridge/pe/victorybridge_pe.c`: `DllMain` takes the unix handle
  (`NtQueryVirtualMemory(..., MemoryWineUnixFuncs = 1000, ...)`) and `__wine_unix_call_dispatcher`
  as the spikes did; one export, `LONG WINAPI victorybridge_call(UINT32 code, void *args)`
  (undecorated name, checked with `objdump -p`; on x86_64 `WINAPI` is the one Windows x64
  convention, so it matches `lib.rs`'s `unsafe extern "system" fn(u32, *mut c_void) -> i32`).
  Without a Unix half it returns `STATUS_NOT_SUPPORTED` (0xC00000BB) for every call; call numbers
  `>= VB_CALLS` are refused with `STATUS_INVALID_PARAMETER` (Wine's dispatcher indexes the Unix
  half's table unchecked). The DLL always loads; the Rust side's VERSION call decides.
- `bridge/pe/build.sh OUTDIR [unix sources]`: MinGW `-shared` + `winebuild --builtin` for the
  `.dll`, gcc `-shared -fPIC -I bridge/unix` over `bridge/unix/*.c` (files with "test" in the
  name left out) for the `.so`; both made in a scratch directory inside OUTDIR and moved in only
  when both built. No Unix half in `bridge/unix/`: nothing built, a note (exit 0).
- `scripts/linux/build.sh` runs it after the game, into `target/x86_64-pc-windows-msvc/release/`
  beside `victory.exe`; a failure warns and the build still succeeds (`FORK_BRIDGE=0` skips it).
- `scripts/linux/env.sh` `fork_bridge DIR`: only when `VICTORY_BRIDGE` is set and not `0` (the
  crate's rule), and both files are in DIR, `WINEDLLPATH=DIR[:previous]`; sets the header line.
  `run.sh` calls it with `victory.exe`'s directory; `wine-runner.sh` copies the bridge from
  `FORK_BRIDGE_DIR` (test.sh sets it to build.sh's output when asked; may be preset) beside each
  test program and calls it with that directory.
- `.gitignore`: `*.so`, `*.o` (all outputs are under `target/` anyway).
- `crates/victory-bridge/examples/bridge_probe.rs`: prints `asked()` and `available()`; exit 0
  when available (or, with the argument `off`, when not). `lib.rs` unchanged.

**How.** `scripts/linux/build.sh` (as before); `VICTORY_BRIDGE=1 scripts/linux/run.sh ...`;
`VICTORY_BRIDGE=1 scripts/linux/test.sh [-p crate]`; the loading test:
`bridge/pe/test/run.sh [outdir]` (default `target/bridge-pe-test`, own prefix `pfx` there, stops
only its own wineserver; `BRIDGE_PROBE=0` skips the Rust part).

**Header line** (after `fork_home`), only with `VICTORY_BRIDGE` set and not `0`:

    bridge     asked (VICTORY_BRIDGE=1): victorybridge.dll + .so present in <dir> (built <date>), WINEDLLPATH=<dir>; whether it loaded: 'memory bridge on' or a warning in the log
    bridge     asked (VICTORY_BRIDGE=1) but victorybridge.dll/.so missing in <dir> (scripts/linux/build.sh builds them once bridge/unix/ has its Unix half): the present path stays

Whether it actually loaded is known only inside the process: the crate logs `memory bridge on
(VICTORY_BRIDGE)` or its warning.

**Unchanged when off, checked.** `run.sh check` of the committed scripts against the new ones,
with a stand-in `wine` that dumps its environment (`FORK_WINE`, a scratch `FORK_HOME`, so no game
and no real prefix): with `VICTORY_BRIDGE` unset and `=0` the headers are identical and the
environments handed to Wine identical; `=` (empty) adds only the caller's own variable. With
`=1` the header gains the one line and the environment exactly `WINEDLLPATH` (prepended to a
preset one).

**Verified under Proton** (`bridge/pe/test/run.sh`, all pass, stub Unix half
`bridge/pe/test/stub_unix.c`: VERSION answers `VB_VERSION`, the rest refuse):
- C loader (`load.c`, what `victory_bridge::loader::entry()` does): DLL beside the program,
  `WINEDLLPATH` set: loads as builtin, VERSION status 0, version 1, the Unix half's pid differs
  from the Windows one (it ran on the Linux side); call 11 refused 0xC000000D; POOL_CREATE gets
  the stub's 0xC0000002.
- Same without `WINEDLLPATH`: the DLL loads, every call returns 0xC00000BB (no Unix half found),
  so the crate reports the bridge off.
- **New finding:** `LoadLibraryW(L"victorybridge.dll")` (the loader's fallback by bare name) from
  a program elsewhere, run from elsewhere, does **not** find the DLL on `WINEDLLPATH` (error 126;
  also not with an `x86_64-windows/` subdirectory). It is found by name only on the ordinary search
  path (the program's directory, the current directory). So the files must be beside the program:
  build.sh puts them beside `victory.exe`, wine-runner.sh beside each test program.
- Rust side end to end (`bridge_probe.exe`, cargo-xwin, beside the DLL): `VICTORY_BRIDGE=1` +
  `WINEDLLPATH`: `available()` true; unset, `0`, no `WINEDLLPATH`, or the program elsewhere: false.
- `VICTORY_BRIDGE=1 FORK_BRIDGE_DIR=target/bridge-pe-test scripts/linux/test.sh -p victory-bridge`
  (own scratch prefix): the bridge loads in the test process (19 passed), and so
  `tests::without_the_dll_nothing_is_bridged` fails (`!available()`): it assumes tests never have
  the DLL beside them. Without `VICTORY_BRIDGE`: 20 passed, the files still beside the test.
- `FORK_FEATURES=diagnostics scripts/linux/build.sh` builds `victory.exe` (bridge: the note, no
  Unix half yet); with a stand-in Unix half in `bridge/unix/` both files land beside
  `victory.exe`; with a broken one: a warning, exit 0, the earlier files left as they were.
  `cargo xtask policy` passes.

**Open.**
- `without_the_dll_nothing_is_bridged` (WS0's test) should skip itself when `available()` is
  true, or the suite with `VICTORY_BRIDGE=1` always has one failure.
- The loader's bare-name fallback in `lib.rs` never finds the bridge on `WINEDLLPATH`; harmless
  (the beside path is what is used) but its comment "(then on the DLL search path)" is all it
  does.
- `WINEDLLPATH` names `victory.exe`'s directory (`target/.../release`), so Wine would also take
  any other builtin-marked DLL placed there as builtin; nothing else there is one today.
- Not tried with the real Unix half (WS1) or in a game run.

### WS1 results (foxhound base)

Done 2026-11-27 on `memory-bridge-foxhound-ws1`. `bridge/unix/victorybridge_unix.c` (C, gcc
`-shared -fPIC`, `-Werror`) exports `__wine_unix_call_funcs[VB_CALLS]` with the semantics of
`abi.rs`; ABI unchanged. `bridge/unix/README.md` lists what it does beyond the ABI's text.
`bridge/unix/build.sh` builds `target/bridge/victorybridge.so` and the native test
`target/bridge/test_unix` (no Wine: `dlopen`s the `.so` and calls the table);
`bridge/unix/proton/run.sh` checks it under Proton with a test-only PE half.

**Built.**
- Pools: memfd (`MFD_CLOEXEC | MFD_EXEC`, falling back without `MFD_EXEC`). Ids are
  `generation << 32 | slot + 1`: nonzero, and a destroyed id is never valid again.
- Windows: a table; overlaps refused. `WINDOW_REMOVE` needs the exact `(addr, len)` and first
  maps the whole window back to PROT_NONE anonymous.
- `MAP`: one `MAP_FIXED | MAP_SHARED` mmap per op, then, with `VB_MAP_WATCH`,
  `UFFDIO_REGISTER(WP)` + `UFFDIO_WRITEPROTECT`. `UNMAP`: PROT_NONE anonymous (`MAP_NORESERVE`).
  `PROTECT`: `mprotect`.
- `WATCH_TAKE`: `PAGEMAP_SCAN` with `PM_SCAN_WP_MATCHING`, category `PAGE_IS_WRITTEN`,
  `max_pages = capacity`, never `PM_SCAN_CHECK_WPASYNC`.
- `COPY`: `process_vm_readv` on our own pid.
- uffd: `UFFD_USER_MODE_ONLY`, `WP_ASYNC | WP_UNPOPULATED | WP_HUGETLBFS_SHMEM`.
  `VB_FEATURE_WATCH` is set only after a self-test at the first call: a one-page memfd view is
  registered, armed and written, and must be found by one scan and not by the next.
- Threads: no lock on the hot paths (pool and window lookups read atomics). One mutex covers pool
  create/destroy and window add/remove, held over table updates only. Every syscall is retried on
  `EINTR` (the uffd ioctls on `EAGAIN` too).

**Tests** (`RUN=1 bridge/unix/build.sh`): 163 checks, all pass natively and under UBSan. A
mutation (no `max_pages`) makes 7 fail. Covered:
- Version and features; pool ids, slot reuse, double destroy; window add, overlap, remove.
- Views: aliases see each other's writes. Unmap leaves PROT_NONE anonymous memory, still in
  `/proc/self/maps`, with the pool's bytes kept. A map over a view replaces it.
- Protect: a write to a read-only view faults, a read of PROT_NONE faults, one page of a view
  can be protected, unknown bits are refused.
- A destroyed pool's views stay and still alias; its id maps nothing. Window removal empties the
  window.
- Batches: per-op results for ops outside a window, straddling its end, with an unaligned address
  or offset, past the pool, or with an unknown flag (a sentinel page outside the window stays
  untouched); `done`; `count` 0, 4,096 and 4,097; an unknown pool.
- Watch: exactly the written pages (reads not counted, unpopulated pages counted), re-armed after
  a take, aliases reported only where written, sub-ranges. Capacities 1, 4, 13 and 40 paged through `more`
  lose or double nothing and leave no page armed. A page at the range's very end. A remap starts
  clean with its data kept; an unwatched remap reports nothing; a hole of unmapped views is
  skipped; protect R then RW keeps tracking.
- Copy: whole; across an unmapped view (stops at it); from PROT_NONE; into PROT_NONE, read-only,
  or a destination that ends early; from address 8. None of them faults.
- **EINTR**: 300 rounds of 256-op map/take/protect/copy/unmap batches on a thread flooded with a
  signal installed without `SA_RESTART` (1-18 million signals per run): no op failed and every
  take was exact.
- Threads: 6 threads map and take at once while pools and windows are created and removed 2,000
  times.

Under **Proton Experimental** (own prefix, `WINEDLLPATH`), the `.so` loads and `VERSION` answers
1 with features 3 (watch, copy). A round trip from PE code through a `VirtualAlloc(MEM_RESERVE)`
window passes: aliases, a PE write found by one take and then re-armed, a copy stopping at an
unmapped view, window removal, and Wine then releasing the reservation.

**Speed** (`SPEED=1`). Setup: 60,000 views of 64 KiB (3.66 GiB) over a 512 MiB pool, view i at
offset (i % 8192) × 64 KiB as in S1, batches of 4,096, P-cores, native. Two runs on a machine
shared with other sessions (load 3-5).

| Per op, 60,000 views | WS1 | S1 / S4 |
|---|---|---|
| map into unmapped | 0.74-0.86 µs | 0.70-0.72 µs (S1 mmap) |
| replace in place | 0.76-0.97 µs | 0.95-1.07 µs (S4) |
| unmap | 0.72-0.86 µs | 0.84-0.87 µs (S1) |
| map + `MAP_WATCH` into unmapped / replace in place + `MAP_WATCH` | 1.38-1.65 / 1.82-2.15 µs | 1.41 µs (S1, map+reg+arm) |
| protect | 0.32-0.37 µs | 0.19-0.22 µs (S4, batched) |
| churn: random view unmap + map (watched), 1 op per call | 3.3-4.8 µs | 3.03-3.27 µs (S1) |

| Take of the whole window | right after mapping (8 VMAs) | after protect + churn (24,357 VMAs) | same, 8 other threads busy |
|---|---|---|---|
| nothing written | 1.4-1.8 ms | 3.9-7.0 ms | 6.6-8.8 ms |
| 100 pages written | 1.3-1.8 ms | 3.5-7.5 ms | 6.7-9.0 ms |
| 1 page in each of the 60,000 views | 2.3-3.2 ms | 18-25 ms | 57-110 ms |
| all 16 pages of every 10th view (96,000 pages) | 2.9-3.5 ms | 11-14 ms | 23 ms |
| one 64 KiB view, nothing written | 0.29-0.49 µs | 0.48-0.81 µs | 0.76-1.0 µs |

Capacity per call (512, 4,096 or 200,000) and the size of the Unix half's region buffer (256 to
65,536) make no measurable difference. The mapping numbers match S1/S4. Takes right after mapping
match S1 (1.54 ms empty, 4.2 ms one page per view), but S1's contiguous offsets let the kernel
merge its views into a few VMAs. **In the game every view is its own VMA** (random offsets,
per-view protections). A take then costs about 50-100 ns per VMA walked, plus 0.3 µs per VMA
with written pages, or 1-1.8 µs per such VMA when other threads of the process are running: the
scan's re-arm flushes the TLB per VMA, and the flush sends IPIs to the CPUs those threads run on.

**For the PE side and the ABI's users:**
- **WS5:** take only the ranges the watch asks about, never the whole window every frame. A take
  that finds many written views costs about 1 µs per view in a busy process. That is far below
  today's guards, but it is not free.
- **`victory-bridge` (lib.rs, `Bridge::map`):** a failed `MAP` op leaves its range *unmapped*,
  not as it was. A `MAP_FIXED` mmap may already have dropped the old view, and a view whose
  tracking could not be armed must not stay. `Bridge::map` records only the ops that succeeded,
  so its map would keep showing the old view. It should record the failed ops it sent as
  unmapped. This is a PE-side change; WS1 did not touch the crate.
- `PROTECT` on an unmapped range succeeds and makes zero-filled private pages appear. Callers
  must protect views only; `Bridge::protect` already checks this.
- `WINDOW_REMOVE` needs the window's exact `(addr, len)`; `Bridge::window_remove` passes it.
- `VB_MAP_WATCH` is silently ignored when `VB_FEATURE_WATCH` is off; `Bridge::map` sends it only
  when the feature is on.
- A take's `more` may cost one extra, empty call. If a scan fails after some pages were found,
  they are returned with `more` set, and the next take is refused.

**Not verified:**
- A thread actually suspended by Wine (`SuspendThread`) in the middle of a batch. The EINTR test
  uses plain signals, natively.
- The Unix half in the game.
- memfd hugepages.
- `PAGEMAP_SCAN` racing an unmap of the same range from another thread. This can happen, because
  the PE side serialises changes but not takes. The kernel takes `mmap_lock` for reading at each
  2 MiB step, so at worst a take reports a page of a view that has just gone.

### WS4 results (foxhound base)

Done 2026-11-27 on `memory-bridge-foxhound-ws4` (from 63edacb). No game run.

**What changed.** A new module `crates/victory-bridge/src/intercept.rs` (one `pub mod` line in
`lib.rs`) with the drop-ins S2 asked for beside WS0's `virtual_query`/`virtual_protect`, each with
Windows' own arguments and results, each answering or refusing for a window and passing anything
else to Wine unchanged: `virtual_alloc` (a commit, `MEM_RESET` or `MEM_RESET_UNDO` that meets a
window: refused, `ERROR_INVALID_ADDRESS`, as a placeholder or a mapped view refuses a commit today;
a reservation there Wine refuses itself), `virtual_free` (anything of a window: refused,
`ERROR_INVALID_PARAMETER`, as Wine refuses a free of a mapped view; the window goes only by
`window_remove`, then Wine's release), `nt_query_virtual_memory` (classes 0 basic; 6/7 region: one
private reservation flagged *placeholder*, `CommitSize` = its views; 4 working set: Wine answers the
entries, a window's are rewritten from the map), `query_working_set_ex` (the same for
`K32QueryWorkingSetEx`), `get_write_watch` (never asked about a window: refused as Wine refuses
memory that is not write-watched, `ERROR_INVALID_PARAMETER`; WS5 answers it with `watch_take`),
`write_process_memory` (own process and meets a window: `NtWriteVirtualMemory`; else
`WriteProcessMemory`), `meets(addr, len)` (any page in a window) and `title_import(name)`. Each
has an `_on(Option<&Bridge>, ...)` form so the bridged branch is tested with a fake Unix half.
`victory-gpu` now depends on `victory-bridge` (Cargo.toml and Cargo.lock, one line each).

**Classification** (every non-test site in the workspace; the tests' own allocations, ~140 sites,
are our own and not listed). *Title* = may meet a bridge window (xmem's pools); *own* = never does
(our allocations, the image, code, IAT, stacks).

| Site (after this change) | Call | Class | Now |
|---|---|---|---|
| `victory-gpu/src/watch.rs:287,302` (`sys`) | `VirtualProtect`, `VirtualQuery` | title | through `victory_bridge::virtual_protect`/`virtual_query`, still counted and timed for the frame log. Every non-test user in watch.rs goes through `sys`: `open_unlocked` (1215, 1220), `protect_alias` (2293, 2308), `protect_here` (2324, 2335), `unguard` (2462, 2467), `is_view` (2172), `uncommitted_end` (2398), `readable_view` (2819), `allocation_end` (2970), `writable` (3276) |
| `watch.rs:335` `kernel_written` | `GetWriteWatch` | title | never asked when the range meets a window: `None` (the callers: not write-watched; views are then guarded through the routed protections). Marked "WS5: `watch_take` here" |
| `watch.rs:1427` `placeholder` | `NtQueryVirtualMemory` (class 7) | title | through `intercept::nt_query_virtual_memory`: a window reads as a placeholder reservation (pages only by mapping), so its holes are "known holes" as xmem's placeholders are |
| `watch.rs` `on_write` → `came_back` → `readable_view`/`writable` | `VirtualQuery` | title | routed (above): S2/M0's WS6 item for the handler is done here |
| `victory-gpu/src/host.rs:8886` `read_sparse` | `VirtualQuery` | title | `victory_bridge::virtual_query` |
| `host.rs:9030` `read_plain` (`VICTORY_SAFE_COPY=0`) | `ReadProcessMemory` | title | unchanged: works on bridge views (S2) |
| `host.rs:9083` `write_mapped` (default) | `NtWriteVirtualMemory` | title | unchanged: works on bridge views (S2) |
| `host.rs:9088` `write_mapped` (`VICTORY_DIRECT_WRITES=0`) | `WriteProcessMemory` | title | `intercept::write_process_memory`: `NtWriteVirtualMemory` when the range meets a window, `WriteProcessMemory` otherwise (identical off the bridge) |
| `victory-gpu/src/inplace.rs:651` `private_whole`, `:1148` `kind_of` | `VirtualQuery` | title | `victory_bridge::virtual_query`: a bridged view reads `MEM_MAPPED`, so no `From::Part` heap is opened on it (WS7 changes this, S3) |
| `inplace.rs:914-1073` (the in-place probe) | `VirtualAlloc(2)`, `VirtualFree`, `VirtualProtect`, `GetWriteWatch`, `ResetWriteWatch` | own | unchanged (its own reservations) |
| `victory-gpu/src/texture.rs:2019` `readable` | `K32QueryWorkingSetEx` | title | `intercept::query_working_set_ex` (Wine says "not valid" for every window page; the fallback kernel read would still be right, one read per page) |
| `texture.rs:2051` `committed_by_query`, `:2119` `mapped_bytes` | `VirtualQuery` | title | `victory_bridge::virtual_query` (`VICTORY_COMMITTED_QUERY=1`, and the walk of `mapped_bytes`) |
| `victory-gpu/src/port.rs:236` `committed` | `VirtualQuery` | title | `victory_bridge::virtual_query` |
| `victory-gpu/src/copystat.rs:543, 602, 1406` (diagnostics) | `K32QueryWorkingSetEx`, `VirtualQuery` | title (the address-space walk) | routed likewise |
| `victory-gpu/src/safecopy.rs` | `rep movsb` + handler; `ReadProcessMemory` (`VICTORY_SAFE_COPY=0`) | title | unchanged (S2: both work on bridge views; its faults reach `on_write`, now answered right) |
| `victory-gpu/src/upload.rs:77` | `VirtualAlloc2` | own | unchanged |
| `victory-gpu/src/replay/hook.rs:68` (diagnostics) | `VirtualQuery` | own (module data) | unchanged |
| `victory-gpu/src/timeline/hooks.rs:185-200` (diagnostics) | wrappers of the title's `GetWriteWatch`, `VirtualProtect`, `ReadProcessMemory`, `VirtualQuery`, `VirtualAlloc`, `VirtualFree` | title | unchanged; with the bridge on, the names `title_import` takes are bound to the drop-ins first and are no longer timed by the timeline |
| `timeline/hooks.rs:312,314` | `VirtualProtect` | own (IAT slot) | unchanged |
| `victory-gpu/src/titlewrite.rs` `import` | (the title's imports by name) | title | asks `intercept::title_import` first (below) |
| `victory-hle/src/image_watch.rs:38` | `ReadProcessMemory` | own (the image) | unchanged |
| `victory-hle/src/modules/xmem.rs:147` `XMemVirtualAlloc` | `VirtualAlloc2` (placeholders) | title | **WS3**: reserves the window (`MEM_RESERVE`, then `window_add`) instead |
| `xmem.rs:248` `XMemVirtualQuery` | `VirtualQueryEx` | title | **WS3**: `victory_bridge::virtual_query` (own process) |
| `xmem.rs:379` `XMemFreeDefault`, `:1509` `one_placeholder`, `:1990` `split` | `VirtualFree` (release, coalesce, split) | title | **WS3**: never on a window (`window_remove` first; no splits) |
| `xmem.rs:2053` `commit` | `VirtualAlloc(MEM_COMMIT)` | title | **WS3**: a bridge `MAP`, never Wine's commit in a window |
| `xmem.rs:2614` `one_placeholder_there` | `VirtualQuery` | title | **WS3**: `victory_bridge::query` |
| `xmem.rs:2782, 2792, 2800` `title_virtual_free/query/protect` (the title's own, HLE exports) | `VirtualFree`, `VirtualQuery`, `VirtualProtect` | title | **WS3**: call `victory_bridge::intercept::virtual_free`, `victory_bridge::virtual_query`, `victory_bridge::virtual_protect` in place of windows-sys' (same arguments); tell `title_memory_freed` only of a free that was done |
| `victory-loader/src/`: `windows.rs:289,299`; `tls.rs:55,66,132,158`; `thunks.rs:96,139`; `console_insn.rs:122,198,218,307,336,404,406`; `frame_rate.rs:207,211`; `write_sites.rs:269,274`; `crash.rs:743` | `VirtualProtect`, `VirtualAlloc`, `VirtualQuery` | own (image code and IAT, TLS, thunks, stubs; the crash report's faulting address) | unchanged. `crash.rs:743` may be asked about a window address in a crash report: it would say "reserved" (cosmetic; could use the drop-in later) |

**The title's own imports.** `victory-loader` binds what `victory-hle`'s resolver gives: xmem's
HLE exports first (`VirtualFree` always; `VirtualQuery`/`VirtualProtect` with soft views: WS3,
above), then the host pass-through (`victory-hle/src/host.rs`), which already asks
`victory_gpu::titlewrite::import(name, address)`. That function now returns
`intercept::title_import(name)` first: with the bridge on (`available()`), `VirtualAlloc`,
`VirtualFree`, `VirtualProtect`, `VirtualQuery`, `GetWriteWatch`, `WriteProcessMemory`,
`NtQueryVirtualMemory`/`ZwQueryVirtualMemory` and `QueryWorkingSetEx`/`K32QueryWorkingSetEx` are
bound to thin `extern "system"` wrappers (`intercept::title`) of the drop-ins; off, it is `None`
and nothing changes (no wrapper is ever bound). No victory-loader file needed a change. Not wrapped
(open): the `...Ex` variants taking a process, `VirtualAlloc2`/`MapViewOfFile3`,
`NtAllocate/Free/ProtectVirtualMemory`, `ResetWriteWatch` (Wine fails it on a window without
`MEM_WRITE_WATCH`, harmless), `IsBad*Ptr` (works, S2).

**`VICTORY_BRIDGE=0` unchanged.** With no bridge every drop-in's first step is `bridge()` (a
`OnceLock` read) giving `None`, then Wine's own call with the caller's arguments (the working-set
drop-in calls `K32QueryWorkingSetEx`, the same export as before; `placeholder`'s
`NtQueryVirtualMemory` is the same ntdll export); `meets` is false, so `kernel_written` and
`write_mapped` do what they did; `title_import` is `None`, so every import is bound as before.
`VICTORY_BRIDGE=1` without the DLL (all tests) takes the same path. Checked by the suites both ways
(below) and by `outside_windows_the_drop_ins_are_the_systems_calls` (Wine's answers, with no bridge
and with a bridge whose windows lie elsewhere).

**Tests.** 7 new in `victory-bridge` (`intercept/tests.rs`): `meets` at the edges and without a
bridge; the commit/free/watch verdicts in, across and outside a window and with no bridge; the
region and working-set answers of a window; under Proton: outside windows every drop-in equals Wine
(query, region size, write, the write watch finds the page, decommit, release, release of free
memory refused by Wine); in a window (at addresses free to Wine) commit, decommit, release and
write watch are refused with the errors above, basic, region and working-set questions are answered
from the map (`STATUS_INFO_LENGTH_MISMATCH` for a short buffer; Wine's own entries kept beside the
window's); another process's question is Wine's; `title_import` is `None` with the bridge off.
`scripts/linux/test.sh` (`FORK_FEATURES=""`, Proton, `--lib`, one thread, the 4 process-killing
GPU tests skipped as in WS8), the base 63edacb against this branch, `VICTORY_BRIDGE=0` and `=1`:

| Crate | Base | `=0` | `=1` |
|---|---|---|---|
| `victory-bridge` | 20 passed | 27 passed | 27 passed |
| `victory-loader` | 43 passed | 43 passed | 43 passed |
| `victory-hle` | 169 passed, 2 failed | same | same |
| `victory-gpu` | 414 passed, 7 failed | same | same |

Pre-existing failures, the same in all three: `victory-hle`:
`modules::net::tests::only_loopback_is_reachable`,
`modules::xgamesave::tests::a_kept_blob_past_the_link_limit_is_copied`; `victory-gpu`:
`watch::many_askers_tests::{bits_and_holes_with_faults_that_take_no_lock,
holes_are_known_until_the_runtime_tells, no_mapping_is_lost_between_askers}`,
`watch::tests::what_is_left_alone_is_no_known_hole`,
`descriptor::host::tests::views_are_made_beside_the_heap_and_copied_in`,
`indirect::tests::host_list_budget_starts_empty_at_each_submission`,
`pipeline::library::tests::a_rejected_blob_gives_an_empty_library` (WS8's lists). `cargo xtask
policy` passes; clippy (`victory-bridge`, and `victory-gpu` with `diagnostics`, all targets): no
warning in the new module or the changed lines; the new files are rustfmt-clean (the touched old
files are not as a whole and were not reformatted).

**Why so many queries, and how many our books could answer** (§11: 11,143 `VirtualQuery` a usual
frame, 28.6 ms over all threads, ~2.6 µs each through Wine's lock; on the frame's own threads
8.0 ms a usual frame, 65 ms median in long frames). All of the counted ones are watch.rs's (only
its `sys` is counted; texture.rs's and inplace's queries come on top, uncounted):
- `guard` → `is_view(start, end)` for a range whose first block the guards do not know: with
  `VICTORY_GUARD_HOLES` on (the default) it walks region by region through every hole of the
  range, one query per region; then, per absent block of the range, `uncommitted_end` (one per
  region of holes) and, for each block that came back, `is_view` again. A frame asks about
  ~150,000 blocks (watch.rs's own comment), so a few percent of them in holey stretches is
  thousands of queries.
- `protect_here`: when protecting a run fails, block by block, `uncommitted_end` per failed region
  and `placeholder` (an `NtQueryVirtualMemory`, not counted) per absent block.
- `on_write`/`came_back`: `readable_view`/`writable` per fault (319 guard faults a usual frame: at
  most ~600 queries); `allocation_end` per title free (rare); `look_at_holes` (1% of the held holes
  a frame).

Proposal, not implemented (none is trivially behaviour-preserving off the bridge):
1. **Measure first**: tag `sys::VirtualQuery` with its caller (5 counters: `is_view`,
   `uncommitted_end`, `readable_view`/`writable`, `allocation_end`, other) and print them in the
   frame log's memory section; one coordinator run says which dominates.
2. **On the bridge** (after WS3 puts the pools in windows): every one of these queries about title
   view memory is answered by the bridge's map (this change routes them there), with no Wine lock.
   **But see the first open issue: today that map's `query` is slower than Wine's on long runs.**
   Once it is O(log n), ~10,000 queries at ~0.1 µs take ~1 ms a frame over all threads instead of
   28.6, and on the frame's threads ~0.3 ms instead of 8 (usual frames) and a few ms instead of 65
   (long frames), if the count stays the same.
3. **Off the bridge, from our books**: `is_view` from `inplace::cover` (the views xmem tells with
   `title_memory_view`: `Run`/`Scattered`/`Holes` mean a view; only `None` asks Wine: private heaps
   and foreign memory); `uncommitted_end`/`placeholder` from xmem's page table (`Shown`: each
   64 KiB page of its reservations is unmapped, a view or committed; needs a read-only hook from
   xmem, WS3, like `SoftViews::runs`); `readable_view`/`writable` for blocks in `g.protected`
   (read-only by our hand: a view, `PAGE_READONLY`) without asking. Wine stays the answer for
   anything the books do not cover (the title's own `VirtualProtect`, through xmem's
   `title_virtual_protect`, would then have to tell the watch). Expected: most of the count, if (1)
   confirms that `is_view` and `uncommitted_end` dominate.

**Open issues.**
- **Blocker for the gain on the bridge (WS0/coordinator, `map.rs`): the map never coalesces
  segments, and `Map::query` walks forward through every following segment of the same run** to
  find its length. Measured natively (release build, this PC): one window of 60,000 views of
  64 KiB of consecutive pool bytes, mapped one by one: **132 µs per `query`** at the run's start
  (50x Wine's ~2.6 µs); after unmapping them one by one, 122 µs for the unmapped run; `window()`
  7 ns. Fix: merge a segment with its neighbours in `Map::set` when `continued_by` holds (a run is
  then one segment and `query` O(log n)), or bound the walk. `intercept::committed_in` (the region
  classes) walks the window by `query`, so it is quadratic until then (a rare call).
- WS3: the xmem sites above (its title exports must use the drop-ins; `title_memory_freed` only
  after a free that was done).
- WS5: `kernel_written` returns `None` for windows (guards then); `watch_take` goes there.
  `inplace.rs`'s probe and the title's `ResetWriteWatch` are untouched.
- WS6: the handler's queries are routed; `safecopy` and the `ReadProcessMemory` /
  `NtWriteVirtualMemory` paths are unchanged per S2.
- WS7: `private_whole`/`kind_of` see bridged views as `MEM_MAPPED` (no in-place heap there yet).
- The drop-ins' bridged answers still count as "region queries" / "protection changes" in the frame
  log (cheap, but counted): after WS3 the count stays and the time falls.
- Not verified: anything in a game run (none by this workstream), and the bridged branch against a
  real Unix half (tested against a fake only; WS1/WS2 are not merged here).

### WS3 results (foxhound base)

Done 2026-11-27 on `memory-bridge-foxhound-ws3`. xmem maps the title's physical pages through
the bridge when `victory_bridge::available()`. Without it (`VICTORY_BRIDGE` unset or `0`, no DLL,
Windows), every bridged branch sits behind `victory_bridge::bridged(addr)` or `bridge_on()`, both
false, and the present code runs unchanged. Not run in the game; the real DLL is WS1+WS2's.

**Emulated Unix half for tests** (`crates/victory-bridge/src/emulate.rs`, feature `emulate`,
Windows only; `victory-hle` turns it on in its dev-dependencies only). An `Entry` speaking the ABI
with Win32 calls:
- a pool is a pagefile section (`SEC_COMMIT`); its id is the handle;
- `WINDOW_ADD` releases the plain reservation and reserves it again as a placeholder (another
  thread's allocation could take the address in between; acceptable in tests); `WINDOW_REMOVE`
  unmaps, joins and hands back a plain reservation;
- a view is one `MapViewOfFile3` per 64 KiB over a placeholder cut to that size, so any part of a
  view can be replaced or unmapped alone; `MAP` over a view replaces it;
- `PROTECT` is `VirtualProtect`; features 0 (no watch, no copy).

`victory_bridge::emulate::install()` makes it the process's bridge whatever `VICTORY_BRIDGE` says,
through a small hook in `lib.rs` (`INSTALLED`, checked before the loaded bridge, set by
`install(entry)`). The crate's own test of it runs on a private `Bridge`, because the crate's other
tests need none installed. Views must be 64 KiB-aligned there; xmem's always are.

**Design decisions.**
- *Which path.* Decided per object, not per call. A pool is the bridge's if made while
  `bridge_on()`: a `Section` with `pool: Some(victory_bridge::Pool)` and a null `handle`. A
  reservation is a window if made while `bridge_on()`. Every later operation asks
  `victory_bridge::bridged(addr)`. In tests `bridge_on()` is also gated by a thread-local, so both
  paths run in one process.
- *The GPU side's number for a section.* A bridged pool has no handle; `Section::id()` returns
  `1<<62 | pool id` (no handle has that bit), and that is what `title_memory_view` gets.
  `title_memory_section` and `title_memory_section_gone` are **not** called for bridged pools.
  `inplace` uses that registration only to open the section (`OpenExistingHeapFromFileMapping`,
  impossible on a memfd: S3) and to map a view of its own (`beside`, `MapViewOfFile` on the handle).
  So `inplace::beside`/`beside_page` answer `None` for bridged views, and in place never takes the
  section path there (S3: only `OpenExistingHeapFromAddress1`).
- *xmem's own view of a whole pool* (`Section::window`, used by DirectStorage writes,
  `write_physical`, `page_window`, `soft_read`). For a bridged pool it is a reservation of its own,
  handed to the bridge, with the pool mapped there in one `MAP`. It is made on first use as before,
  and always, even with `VICTORY_SECTION_WINDOW=0`, because a pool has no `MapViewOfFile`.
  `View::map` returns a borrowed piece of it (holding the section). Bridged pools get no commits:
  `commit`/`commit_shown` skip the call but still keep the commit bits for `arrange`.
- *One call per batch, on the calling thread.* Every change of mapping goes in three steps:
  1. The books first (`unmap_bridged`): views and `Shown` forgotten, `title_memory_splitting` and
     `_unmapping` told as before. What stays of a cut view is noted again with
     `title_memory_remapped`/`_view` but makes **no** bridge call, since the bridge unmaps only the
     part inside. It returns the ranges the bridge still shows.
  2. One `Pool::map` per pool for all new views. `MAP` replaces what is there: no unmap first, no
     placeholder split.
  3. One `unmap` for the old ranges that no new view covers (`minus`).

  The new views are then noted and told, in that order. Per-op results come back and decide what
  is noted: a refused op is not noted and makes the call fail.
- *Reservations.* `XMemVirtualAlloc` of 64 KiB pages becomes a plain `MEM_RESERVE` (no
  `MEM_WRITE_WATCH`) followed by `window_add`; so does the title's own `MEM_RESERVE_PLACEHOLDER`
  kind (the 32-bit heap). If the bridge refuses the window, the reservation is released and null
  returned. Inside a window, a `MEM_REPLACE_PLACEHOLDER` or reserve at a base is "nothing to do"
  (base returned), and `MEM_COMMIT` is refused (S2), as it fails in a placeholder.
- *Release* (`title_virtual_free` in a window: `bridged_free`):
  - the split and join flags (`MEM_PRESERVE_PLACEHOLDER`, `MEM_COALESCE_PLACEHOLDERS`) are
    bookkeeping only and return 1;
  - `MEM_DECOMMIT` fails with `ERROR_INVALID_ADDRESS`;
  - `MEM_RELEASE` of the window's base: `title_memory_freed`, the window's views unmapped through
    the books (notices as for an unmapping), `window_remove`, then Wine's `VirtualFree(MEM_RELEASE)`;
  - `MEM_RELEASE` of a piece the title cut out: unmapped, and it stays reserved until the whole
    window goes (shortcut).
- *Counting.* `bridge_call` adds to two new frame-log totals, `RemapBridgeCalls` and
  `RemapBridgeNs`. The line's xmem section now reads `xmem: <n> mapping calls <ms> ms; Windows
  calls <n> <ms> ms; bridge calls <n> <ms> ms; the GPU side told ...` (`framelog.rs`: two `T`s, the
  line, its test). `Costed` (xmem mapping calls) and `told` are unchanged; creating a pool counts as
  a bridge call too. `ab_runs.py` (WS8's file) does not read the new column yet; the regexes would
  be `bridge calls {N} ` and `bridge calls -?[0-9.]+ {N} ms`.

**Per function.**

| Function | Bridged |
|---|---|
| `XMemAllocatePhysicalPages` | `allocate_bridged`: `Pool::create`; page numbering shared with the present path (`give_pages`) |
| `Section::window` / `View::map` / `Drop` | its own window + one `MAP` / a borrowed piece of it / `window_remove` + `VirtualFree` |
| `XMemVirtualAlloc` | windows as above; a commit inside one refused |
| `XMemVirtualQuery`, `title_virtual_query`, `title_virtual_protect` | `victory_bridge::virtual_query`/`virtual_protect` for bridged addresses |
| `title_virtual_free` | `bridged_free` |
| `map_pages` (`XMemMapPhysicalPages`) | `map_pages_bridged`: the books, one `MAP` per pool for all runs (or `map_lazy` with soft views), one unmap; all runs are tried, and an unknown page stops there as before |
| `XMemMapPhysicalPagesScatter` / `scatter` | batched through `change` whenever the first address is bridged (not only with `VICTORY_MAP_THREADS` > 1), unless the pool's views are soft |
| `map_aliases` | batched through `change` when bridged, unless the copies' views are soft |
| `map_alias` / `alias_as` | `alias_bridged`: a piece per run (`VICTORY_MAP_RUNS=1`) or per page, one `MAP` + one unmap; overlapping ranges and `from == 0` as before (a bridge call per page, or an unmap) |
| `change` | `change_bridged`: the same books per page, one `MAP` per pool, one unmap. No worker pool (`pool::all` runs only on the present path), so `VICTORY_MAP_THREADS` does not matter on the bridge |
| `unmap` | `unmap_bridged` + one unmap, of only the overlapped part of a cut view (the rest stays mapped) |
| `show`/`show_as` (also `materialize_for`) | one `MAP` |
| `split`, `one_placeholder`, `one_placeholder_there`, `Op::run`, `commit` | never reached for bridged memory |
| `map_lazy` | no split or placeholder check in a window (the caller unmapped what was there) |

**Soft views** (`VICTORY_SOFT_VIEWS`) work on both paths. On the bridge, a view kept in the books
alone is unmapped (`PROT_NONE`), faults as before, and is "made real" by one bridge `MAP`
(`materialize_for` -> `show`); `soft_read` reads through the bridged pool's own view.

Whether making views real stays worth it: soft views avoid Wine's 45 + 93 µs per view, and a view
on the bridge costs ~1 µs (S4). What they still cost there is a fault, the handler and a map per
view touched, the GPU side's reads through `soft_read`, and the long frames' "made real" bursts. So
on the bridge they likely stop paying. **A/B `VICTORY_SOFT_VIEWS` unset against `copies` with
`VICTORY_BRIDGE=1`** (WS8) before keeping them. With them off, `map_aliases` and scatter take the
batched path (one call per group). Nothing here turns them off automatically.

**Tests** (`FORK_FEATURES="" scripts/linux/test.sh`, under Proton, this machine).
- **xmem.** 15 existing mapping tests get a twin `<name>_on_the_bridge`: `on_bridge(<name>)` runs
  the same body with the emulated bridge installed and the thread's new pools and reservations on
  it. Four new tests:
  - `ranges_less_others`;
  - `a_reservation_on_the_bridge_is_its_window_until_released`: window add and remove, and the
    title's split, join, commit, decommit and release;
  - `a_list_on_the_bridge_keeps_each_pages_result`: a scatter batch's per-op results. One page is
    refused and the others are mapped; the old view is cut, and unmapped only where nothing new is;
  - `page_mapping_copies_on_the_bridge_are_aliases_of_the_pool`: `map_aliases`/`map_alias` on the
    bridge, whose map shows the same pool page at alias and source.

  Two existing tests changed slightly to run on both paths. The title's join of placeholders now
  goes through `title_virtual_free` (it was a raw `VirtualFree`; same effect on the present path).
  "What the host has" now asks `victory_bridge::virtual_query`, which is Wine's answer for
  memory that is not bridged.
- **`victory-hle` lib.** Base 63edacb, built the same way, one thread: 169 passed, 2 failed
  (`net::tests::only_loopback_is_reachable`, `xgamesave::tests::a_kept_blob_past_the_link_limit_is_copied`).
  With WS3: 188 passed (19 new), the same 2 failed. Three more runs with parallel threads gave the
  same result.
- **`victory-bridge`.** 21 passed (20 + the emulator's).
- **`victory-gpu` lib.** One thread, the four process-killing tests skipped as in WS8: 414 passed,
  7 failed. The failures are WS8's 4 base failures (`watch::many_askers_tests::*` x3,
  `watch::tests::what_is_left_alone_is_no_known_hole`) and its 3 machine-state ones. Framelog's 9
  tests pass.
- **Checks.** `cargo xtask policy` passes. `cargo xwin clippy --tests` gives no warnings in the
  changed code; the three in xmem's tests are in old lines. `emulate.rs`, the bridge's `lib.rs` and
  `framelog.rs` are rustfmt-clean. xmem.rs was not rustfmt-clean before (100 diffs) and is not
  reformatted.

**Open issues.**
- **WS4.** `title_virtual_query`/`title_virtual_protect` (xmem's hooks of the title's imports) now
  ask the bridge for bridged addresses, but they are bound only with `VICTORY_SOFT_VIEWS`. With the
  bridge they should be bound always, or WS4's loader-level wrappers should replace them (do not
  bind two `VirtualQuery` hooks). `title_virtual_free` is always bound and handles windows here.
  Our own `VirtualQuery` sites on title memory (watch.rs and others) still ask Wine.
- **WS5.** `MAP_WATCH` is false everywhere; the one place to set it is the `MapOp` built in
  `bridge_map` (and in `Section::bridge_window`, which needs none). For aliases, `Shown` (pool page
  -> addresses) is what fans writes out.
- **WS6/WS7.**
  - `inplace::beside` answers `None` for bridged views, because no section is registered. If the
    GPU side wants a view beside the title's for bridged pools, xmem has one per pool
    (`Section::window`); a notice handing it over (`id`, base) would do.
  - S3's rule 1 (drop or re-open a heap before remapping under it) is not done here.
    `title_memory_unmapping`/`_splitting` are still told before every bridge unmap or replace, and
    they are the hook for it.
- **Behaviour differences on the bridge.** A `MAP` op the bridge refuses no longer stops the
  following runs of the same call: they are mapped and noted, and the call still returns failure.
  The title's release of a piece of a window keeps it reserved until the whole is released.
- **Not verified.** Anything against the real Unix half and DLL (emulator only); the bridge's speed
  in xmem's shape (batch sizes per frame); a game run.

## 12. Next: WS5 brief (for any session to pick up)

Branch `memory-bridge-foxhound-ws5` from `memory-bridge-foxhound`; commit there, the coordinator
merges. Goal: the guards (64 KiB read-only blocks on title views + `watch::on_write` faults) and
the protection changes go away for bridged views, replaced by uffd async write tracking.
Measured on the bridge (§11): guard waits ~114 ms a frame over all threads (~5 on the frame's
threads), 330 ms median in long frames; protections ~13 ms. Read first: §10 S1 results and "M0"
(per-view arming at map time; scan only asked ranges; never `PM_SCAN_CHECK_WPASYNC`; aliases are
reported only where written), §11 WS1 results (a take costs per VMA walked: take only what the
watch asks, never the whole window), WS3 results (`MAP_WATCH` is set in the `MapOp` built in
xmem's `bridge_map`; `Shown` maps pool pages to all their addresses, for fanning writes out to
aliases), WS4 results (`watch::kernel_written` returns `None` for windows: "WS5: watch_take here").
Steps: set `MAP_WATCH` for bridged title views; in `watch.rs`, for bridged ranges, skip guarding
(`cover`/`open`/protect) and answer "what was written" with `victory_bridge::watch_take` over the
asked range, fanned out to aliases via xmem; keep everything as is when not bridged. Tests both
ways (`VICTORY_BRIDGE=0/1`, real DLL via `test.sh`), compare with the base's known failures.
Check in the game with a 5-minute run (announce first in §11 Runs) and the frame log columns
"guard waits", "protection changes", "write-watch askings". Don't run the game while another
session does (`pgrep -f victory.exe`).

## 13. Ray tracing: plan (later)

The title traces indirect light, reflections and lamp/sun shadows in its own compute shaders
(`docs/fork-ray-tracing.md`): its own node format, 52 builder programs, 64 trace programs; only
the per-node test (`image_bvh*_intersect_ray`) is hardware, emulated here in DXIL. Off by default
(`VICTORY_BVH_BUILDER`); the stand-in is `VICTORY_AMBIENT` (raised to 1.2 locally: everything
off the sun's direct light is near black otherwise). Figures in that doc are upstream's (likely
Windows): **measure our own under Proton before deciding anything**.

1. **Measure (one 5-minute run, frame log on):** `VICTORY_BVH_BUILDER=1` under Proton on the
   RTX 4070. Does it run, hang, or crash; fps; GPU time per frame by the builder, the traversal
   (trace programs) and the emulated node test (needs per-pipeline GPU timing: add timestamp
   queries around those dispatches, or use the frame log's pipeline columns), and the copies of
   title memory the trace programs read by raw address.
2. **If the emulated node test dominates:** hardware ray tracing through DXR 1.1 inline ray
   queries (vkd3d-proton maps them to `VK_KHR_ray_query` on NVIDIA): convert the title's
   structures (triangle and instance nodes hold the geometry) to DXR acceleration structures each
   frame, and replace the traversal loops in the 64 trace programs with `RayQuery` returning the
   title's hit form. Research-grade (pattern-matching traversal loops; keeping two sets of
   structures in step); spike first on one trace program.
3. **If memory copies dominate:** the bridge already attacks them (WS5 write tracking; WS7 heaps
   in place on bridged memory through `OpenExistingHeapFromAddress1`, S3).
4. **If the builder dominates:** look at refitting less often or caching the static world's
   structures (they ship pre-built).
Linux-only fork: no upstream route needed; keep the work isolated so pulling upstream stays easy.
