# The bridge's Unix half

`victorybridge.so`: the Unix half of the Wine builtin `victorybridge.dll` (WS1 of
`docs/memory-bridge-plan.md`). It exports `__wine_unix_call_funcs[VB_CALLS]`, one entry per call
number in `abi.h`, doing exactly what `crates/victory-bridge/src/abi.rs` (the authority) says.

| File | What |
|---|---|
| `abi.h` | the ABI, mirror of `abi.rs` (WS0's; not edited here) |
| `victorybridge_unix.c` | the Unix half |
| `test_unix.c` | native test, no Wine: `dlopen`s the `.so` and calls the table directly; `--speed` adds the 60,000-view timings |
| `build.sh [outdir]` | builds `victorybridge.so` and `test_unix` into `target/bridge/`; `RUN=1` runs the test, `SPEED=1` with timings |
| `proton/run.sh [outdir]` | builds a test-only PE half (`proton/vb_pe.c`, WS2 owns the real one) and `vb_check.exe`, runs it under Proton Experimental in its own prefix (`target/bridge/pfx-ws1`), stops only that prefix's wineserver |

    bridge/unix/build.sh                       # gcc -shared -fPIC; -Werror
    RUN=1 bridge/unix/build.sh                 # + correctness (~2 s)
    RUN=1 SPEED=1 bridge/unix/build.sh         # + speed at 60,000 views of 64 KiB (~10 s, 3.7 GiB of address space)
    bridge/unix/proton/run.sh                  # loads under Proton, VERSION + one round trip

Wine finds the `.so` only through `WINEDLLPATH` naming its directory (S1, S4).

## Behaviour beyond the ABI text

- **Refusals.** A call returns nonzero for: `count > VB_BATCH_MAX`, missing `ops`/`results` with
  `count > 0`, an unknown or destroyed pool (`STATUS_INVALID_HANDLE`), a malformed window or pool
  size, an overlapping `WINDOW_ADD`, a `WINDOW_REMOVE` whose `(addr, len)` is not exactly a
  window, `WATCH_REGISTER`/`WATCH_TAKE`/`COPY` without their feature, a `WATCH_TAKE` with
  `capacity` 0 or a range not inside one window. Per op: `EINVAL` for zero/unaligned lengths,
  addresses or offsets, unknown `prot` bits or `flags`, a view past the pool's end; `EFAULT` for
  a range not entirely inside one window (nothing is touched then); otherwise the syscall's errno.
- **A failed `MAP` op leaves its range unmapped** (PROT_NONE anonymous), whatever was there
  before: a `MAP_FIXED` mmap may already have dropped the old view when it fails, and a view
  whose tracking could not be armed must not stay. The PE side's map should record a failed
  `MAP` op as unmapped, not as the old view.
- `VB_MAP_WATCH` is ignored (the view is mapped, not tracked) when `VB_FEATURE_WATCH` is off.
- `PROTECT` is a plain `mprotect`: on an unmapped range it makes zero-filled private memory
  appear. The caller checks it protects views only (`victory-bridge` does: `all_views`).
- `WATCH_TAKE` passes `max_pages = capacity` to `PAGEMAP_SCAN`, so the kernel re-arms only pages
  it hands back; `more` is set when `capacity` filled and range is left after the last page (at
  worst one empty extra take). If a scan fails after pages were found, they are returned with
  `more` set (the next take is then refused) instead of being lost.
- `COPY` stops at the first byte that faults on either side (an unmapped or PROT_NONE source,
  an unmapped or read-only destination); `copied` is what landed.
- Pools are `memfd_create(MFD_CLOEXEC | MFD_EXEC)` (falling back to `MFD_CLOEXEC` before Linux
  6.3): `MFD_EXEC` keeps `PROT_EXEC` views possible under `vm.memfd_noexec` and avoids the kernel's
  warning. Ids are `generation << 32 | slot + 1`: nonzero, never valid again once destroyed.
- `VB_FEATURE_WATCH` is set only after a self-test at the first call: a one-page memfd view is
  registered, armed, written, and must be found exactly once. `VB_FEATURE_COPY` after one
  `process_vm_readv` of our own byte.
- **Threads.** No lock on map/unmap/protect/take/copy: pool and window lookups read atomics. One
  mutex serialises pool create/destroy and window add/remove, held over table updates only (and
  over `WINDOW_REMOVE`'s one mmap). Every syscall is retried on `EINTR` (the uffd ioctls on
  `EAGAIN` too). The caller must not destroy a pool or remove a window while an op on it runs.
- Limits: 1,024 windows, about a million live pools.
