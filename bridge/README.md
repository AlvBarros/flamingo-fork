# The memory bridge's DLL

`victorybridge.dll` (PE half) + `victorybridge.so` (Unix half): a Wine builtin DLL that does the
title's page pools, views, protections and write tracking with Linux calls. Plan and progress:
`docs/memory-bridge-plan.md`. The Rust side that uses it: `crates/victory-bridge/`.

| Path | What | Owner |
|---|---|---|
| `unix/abi.h` | the unix-call ABI, mirror of `crates/victory-bridge/src/abi.rs` (the authority) | WS0 |
| `unix/` | the Unix half: `__wine_unix_call_funcs[VB_CALLS]`, one entry per call number | WS1 |
| `pe/victorybridge_pe.c` | the PE half: gets the unix handle in `DllMain`, exports `victorybridge_call(u32 code, void *args)` forwarding to `__wine_unix_call` (nonzero status without a Unix half; call numbers past `VB_CALLS` refused) | WS2 |
| `pe/build.sh` | builds both halves into a directory (MinGW + `winebuild --builtin`; gcc over `unix/*.c`, files with "test" in the name left out) | WS9 |
| `pe/test/` | the loading test under Proton: a stub Unix half, `load.c` (does what `victory_bridge::loader` does), `run.sh` | WS2 |

The PE side loads it only when `VICTORY_BRIDGE` is set and not `0`, under Wine, from beside
`victory.exe`; it calls `VB_VERSION_CALL` first and uses nothing when the version differs. On
Windows, and with `VICTORY_BRIDGE=0` (the default), nothing here is loaded.

## Building and running

`scripts/linux/build.sh` builds the game and then, when `unix/` has its Unix half, this DLL
beside `victory.exe` (`target/x86_64-pc-windows-msvc/release/victorybridge.{dll,so}`); a bridge
that fails to build only warns (`FORK_BRIDGE=0` skips it). By hand: `bridge/pe/build.sh OUTDIR`.

`VICTORY_BRIDGE=1 scripts/linux/run.sh ...`: the log header gets a `bridge` line saying whether
both files are beside `victory.exe`, and `WINEDLLPATH` names that directory: **Wine finds a
builtin DLL's Unix half only through `WINEDLLPATH`** (beside the `.dll` is not enough: without it
the DLL loads, every call is refused and the bridge reports itself off). Wine does not search
`WINEDLLPATH` for a DLL loaded by bare name, so the DLL must be beside the program.
`VICTORY_BRIDGE=1 scripts/linux/test.sh`: `wine-runner.sh` copies the files build.sh made (or
`FORK_BRIDGE_DIR`'s) beside each test program and sets `WINEDLLPATH`. Unset or `0`: the scripts
change nothing.

`bridge/pe/test/run.sh` checks loading under Proton in a prefix of its own
(`target/bridge-pe-test/pfx`): the C loader with and without `WINEDLLPATH` and from elsewhere,
then `crates/victory-bridge`'s example `bridge_probe` (`victory_bridge::available()`) with
`VICTORY_BRIDGE` unset, `0` and `1` (`BRIDGE_PROBE=0` skips that part).
