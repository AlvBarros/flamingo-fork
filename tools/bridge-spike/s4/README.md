# S4: the cost of `__wine_unix_call`, and of batches

Spike S4 of `docs/memory-bridge-plan.md`. It measures, from PE code under Proton Experimental:

- a no-op unix call, with 1 to 8 threads calling at once;
- batched map, unmap and `mprotect` of 64 KiB memfd views in a range reserved through Wine
  (`VirtualAlloc(MEM_RESERVE)`), at 0 and 60,000 views, with and without a per-op result array.
  It also measures replacing a view in place (`MAP_FIXED` over a mapped view);
- Wine's own path in the same session: the `VirtualFree(MEM_PRESERVE_PLACEHOLDER)` split,
  `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`, `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` and
  `VirtualProtect`, at 0 and 60,000 views.

Files: `abi.h` (the argument structs, `#[repr(C)]`-friendly), `s4_unix.c` (the Unix half),
`s4_pe.c` (the builtin PE half: one `s4_call(code, args)` export), `s4.c` (the measuring program),
`build.sh`.

    tools/bridge-spike/s4/build.sh [outdir]          # builds s4.so, s4.dll (winebuild --builtin), s4.exe
    RUN=1 tools/bridge-spike/s4/build.sh [outdir]    # and runs nop, bridge and wine modes

The default outdir is `target/bridge-spike-s4`. The run uses Proton Experimental's `wine` with its
own prefix (`$outdir/pfx-s4`, `WINEDEBUG=-all`), pinned to the P-cores (`taskset -c 0-11`). At the
end it stops only that prefix's wineserver.

**Loading needs `WINEDLLPATH`.** If `WINEDLLPATH` does not name the DLL's directory, Wine still
loads the DLL as builtin, but `NtQueryVirtualMemory(MemoryWineUnixFuncs)` finds no Unix half.
The parent spike's README leaves this out; its `load.exe` failed the same way here until
`WINEDLLPATH` was set. WS2 and WS9 need to set it, or install the `.so` where Wine looks.

## Numbers (2026-11-27, i5-14400F, kernel 6.18, Proton Experimental, P-cores)

Four full runs plus three no-op-only runs. The ranges below cover the runs; ns are per op unless
noted. In run 3, the bridge section at 60,000 views was disturbed by other load on the machine
and is left out.

| Measure | 0 views | 60,000 views |
|---|---|---|
| No-op unix call, tight loop, 1 thread | 4.6-4.8 ns | |
| same, 2 / 4 / 8 threads at once (per call, per thread) | 4.7-5.3 / 5.3 / 6.5-8.4 ns | |
| No-op timed one by one: median / p99, including the ~10 ns timer | 20-26 / 23-47 ns | |
| Bridge map + unmap, 1 op per call | 1.59-1.77 µs | 1.75-1.88 µs |
| same, batch 8 / 32 / 128 | 1.42-1.61 / 1.44-1.59 / 1.46-1.62 µs | 1.65-1.80 / 1.61-1.72 / 1.62-1.79 µs |
| same, batch 512 / 2048 | 1.44-1.62 / 1.52-1.59 µs | 1.66-1.76 / 1.72-1.82 µs |
| Bridge replace in place (one `MAP_FIXED` over a view), batch 1 / 128 / 2048 | 0.82-1.12 / 0.82-0.90 / 0.79-0.96 µs | 0.99-1.07 / 0.95-0.97 / 0.96-1.01 µs |
| Bridge `mprotect`, 1 per call / batched | 222-246 / 192-219 ns | 220-241 / 192-222 ns |
| Per-op result array (batches 32-2048) | within ±5% of no array, both directions | same |
| Bridge map + unmap, batch 32, 1 / 2 / 4 / 8 threads (wall time / all pairs) | 1.44-1.58 / 2.20-2.26 / 2.25-2.49 / 2.67-2.93 µs | 1.51-1.67 / 1.91-2.31 / 2.31-2.34 / 2.75-2.82 µs |
| Wine split `VirtualFree(MEM_PRESERVE_PLACEHOLDER)`, median | 1.2 µs (one run 6.7) | 1.3-1.5 µs |
| Wine `MapViewOfFile3` + `UnmapViewOfFile2`, mean | 15-16 µs (map 10-11, unmap 4.4-4.7) | 300-590 µs (map 155-234, unmap 146-354) |
| Wine `VirtualProtect`, no other thread busy | 555-578 ns | 572-646 ns |
| Making 60,000 views | 58-64 ms (bridge, batches of 2000) | 4.8-6.8 s (Wine: split + map) |

How to read it: a unix call costs about 5 ns, and that does not change with 4-8 threads calling
at once, so nothing in the call itself serialises. The kernel's `mmap` costs about 1,000 times as
much. Batching therefore saves at most about 0.1 µs per op (batch 1 against batch 8 or more), and
batches larger than 8 gain nothing measurable. Mapping from several threads makes total
throughput worse (the process's `mmap_lock`). At 60,000 views the bridge is 180-340 times faster
than Wine's path per map + unmap, and its cost barely grows with the number of views (+10%).
