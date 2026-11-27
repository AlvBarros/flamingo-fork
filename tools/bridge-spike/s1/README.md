# S1: userfaultfd async write-protect + PAGEMAP_SCAN inside a Proton process

Question and results: `docs/memory-bridge-plan.md`, §5 (S1) and §10 "S1 results".

- `s1.h` — the unix-call ABI (fixed-width structs, same layout under MinGW and gcc).
- `s1_unix.c` — the Linux half: memfd pool, views (`mmap MAP_FIXED|MAP_SHARED`), unmap to
  `PROT_NONE` anonymous, `UFFDIO_REGISTER(MODE_WP)` / `UFFDIO_WRITEPROTECT` per view or per range,
  `PAGEMAP_SCAN` (`PAGE_IS_WRITTEN`, optional `PM_SCAN_WP_MATCHING` / `PM_SCAN_CHECK_WPASYNC`).
  uffd opened with `UFFD_USER_MODE_ONLY`, features `WP_ASYNC | WP_UNPOPULATED | WP_HUGETLBFS_SHMEM`.
- `s1_pe.c` — the PE half (builtin DLL `s1bridge.dll`): one export `s1_call(code, args)`.
- `s1_driver.c` — the tests (T1-T5), built twice: as `s1.exe` (all writes made by PE threads,
  a vectored exception handler counts every exception) and natively as `s1-native` (links
  `s1_unix.c` directly; a SIGSEGV aborts it).

## Build and run

    sh tools/bridge-spike/s1/build.sh              # -> target/bridge-spike-s1/
    cd target/bridge-spike-s1
    ./s1-native [views]                            # default 60000 views of 64 KiB
    PROTON="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/bin"
    WINEPREFIX=/some/throwaway/pfx WINEDEBUG=-all "$PROTON/wine" wineboot -i
    WINEDLLPATH=$PWD WINEPREFIX=/some/throwaway/pfx WINEDEBUG=-all "$PROTON/wine" ./s1.exe [views]

`WINEDLLPATH` must name the directory holding `s1bridge.dll` + `s1bridge.so`: without it Wine
loads the DLL but not its Unix half (`MemoryWineUnixFuncs` fails, every call returns -1). The same
holds for the first spike (`../load.exe`).

## Tests

- T1 memfd views (64 views, views v and v+32 alias one pool offset), registered and armed per view
  at map time: written pages exact (4 threads), re-arm, aliases, reads, unmap/remap, a view mapped
  without registering, and what a failed `CHECK_WPASYNC` scan does.
- T2 register once over the reserved range, then map views inside it.
- T3 private memory from `VirtualAlloc(MEM_COMMIT)`: exact pages (6 threads), after
  `VirtualProtect`, after `MEM_DECOMMIT` + `MEM_COMMIT`.
- T4 6 writer threads against back-to-back scans for 300 ms: no write lost, none invented.
- T5 cost at 60,000 views of 64 KiB (3.66 GiB window, 512 MiB memfd, view i at offset
  (i % 8192) * 64 KiB): map/register/arm, churn of 2,000 views at full occupancy, scans with few /
  many written pages, first-write cost, private memory of the same size, Wine's `GetWriteWatch`.

## Numbers

See the table in `docs/memory-bridge-plan.md` §10 "S1 results" (Proton Experimental 11.0-20261112,
kernel 6.18.65, 16 threads; medians of three alternating native / Proton runs).
