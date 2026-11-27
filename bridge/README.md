# The memory bridge's DLL

`victorybridge.dll` (PE half) + `victorybridge.so` (Unix half): a Wine builtin DLL that does the
title's page pools, views, protections and write tracking with Linux calls. Plan and progress:
`docs/memory-bridge-plan.md`. The Rust side that uses it: `crates/victory-bridge/`.

| Path | What | Owner |
|---|---|---|
| `unix/abi.h` | the unix-call ABI, mirror of `crates/victory-bridge/src/abi.rs` (the authority) | WS0 |
| `unix/` | the Unix half: `__wine_unix_call_funcs[VB_CALLS]`, one entry per call number | WS1 |
| `pe/` | the PE half: gets the unix handle in `DllMain`, exports `victorybridge_call(u32 code, void *args)` forwarding to `__wine_unix_call` | WS2 |

The PE side loads it only when `VICTORY_BRIDGE` is set and not `0`, under Wine, from beside
`victory.exe`; it calls `VB_VERSION_CALL` first and uses nothing when the version differs. On
Windows, and with `VICTORY_BRIDGE=0` (the default), nothing here is loaded.
