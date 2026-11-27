//! The unix-call ABI between `victory-bridge` (PE side, Rust) and the bridge
//! DLL's Unix half (`bridge/unix/`, native Linux code). Mirrored, field for
//! field, in `bridge/unix/abi.h`; any change here bumps [`VERSION`] and
//! changes that header in the same commit (`layout_is_fixed` below and the
//! header's `_Static_assert`s check both sides).
//!
//! How a call travels: `victory-bridge` calls the PE half's one export,
//! [`ENTRY`] (`NTSTATUS victorybridge_call(u32 code, void *args)`), which
//! forwards to `__wine_unix_call(handle, code, args)`. `args` points at the
//! call's argument struct below; arrays are passed by pointer and count,
//! all in the caller's memory (the Unix half reads and writes them in place:
//! same process, same address space). Every field is a fixed-size integer;
//! pointers travel as `u64`.
//!
//! Return value: 0 (`STATUS_SUCCESS`) when the call was understood and run;
//! per-operation outcomes are in the call's `results` array. Nonzero means
//! the call itself was refused (bad arguments, unknown pool, not set up):
//! the caller treats that as "bridge unusable" for the operation and falls
//! back where it can.
//!
//! Rules every implementation keeps:
//! - Addresses of `map`, `unmap`, `protect`, `watch_*` lie inside a window
//!   added with [`WINDOW_ADD`] (reserved through Wine with
//!   `VirtualAlloc(MEM_RESERVE)` beforehand); the Unix half refuses others.
//! - Only the bridge changes pages inside a window (never Wine's
//!   `VirtualProtect`/`VirtualFree`/`MapViewOfFile*` there).
//! - "Unmapped" inside a window is `PROT_NONE` anonymous private memory
//!   (`mmap(MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, PROT_NONE)`): the range
//!   stays reserved and ours.
//! - Lengths and addresses are multiples of 4 KiB; pool offsets too.

/// Bumped on any change to this file's layout or meaning.
pub const VERSION: u32 = 1;

/// The PE half's export that forwards a call to the Unix half.
pub const ENTRY: &str = "victorybridge_call";
/// The DLL, looked for beside `victory.exe` (then on the DLL search path).
pub const DLL: &str = "victorybridge.dll";

/// Granularity of every address, length and offset.
pub const GRANULE: u64 = 4096;

// Call numbers: indices into the Unix half's `__wine_unix_call_funcs`.
/// [`Version`]: the Unix half's ABI version and features.
pub const VERSION_CALL: u32 = 0;
/// [`PoolCreate`]: a pool of `bytes` physical pages (one memfd).
pub const POOL_CREATE: u32 = 1;
/// [`PoolDestroy`]: closes the pool's memfd (views of it stay until unmapped).
pub const POOL_DESTROY: u32 = 2;
/// [`Window`]: claims a range reserved through Wine as the bridge's.
pub const WINDOW_ADD: u32 = 3;
/// [`Window`]: gives a window back (made unmapped first; Wine then frees it).
pub const WINDOW_REMOVE: u32 = 4;
/// [`Batch`] of [`MapOp`]: views of a pool.
pub const MAP: u32 = 5;
/// [`Batch`] of [`Range`]: back to unmapped (`prot` ignored).
pub const UNMAP: u32 = 6;
/// [`Batch`] of [`Range`]: `mprotect`.
pub const PROTECT: u32 = 7;
/// [`Batch`] of [`Range`]: arms write tracking (uffd write-protect, async).
pub const WATCH_REGISTER: u32 = 8;
/// [`WatchTake`]: the pages written since the last take, re-armed.
pub const WATCH_TAKE: u32 = 9;
/// [`Copy`]: a copy that fails instead of faulting (`process_vm_readv` on
/// ourselves).
pub const COPY: u32 = 10;
/// One past the last call number.
pub const CALLS: u32 = 11;

// Protections ([`Range::prot`], [`MapOp::prot`]): Linux `PROT_*` bits.
pub const PROT_NONE: u32 = 0;
pub const PROT_READ: u32 = 1;
pub const PROT_WRITE: u32 = 2;
pub const PROT_EXEC: u32 = 4;

// Features ([`Version::features`]).
/// `WATCH_REGISTER`/`WATCH_TAKE` work (uffd WP-async and `PAGEMAP_SCAN`).
pub const FEATURE_WATCH: u64 = 1;
/// `COPY` works.
pub const FEATURE_COPY: u64 = 2;

/// Most operations in one batch; callers split larger ones.
pub const BATCH_MAX: u64 = 4096;

/// `VERSION_CALL`'s arguments (out only).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Version {
    pub version: u32,
    pub _pad: u32,
    pub features: u64,
}

/// `POOL_CREATE` (`bytes` in, `pool` out) and `POOL_DESTROY` (`pool` in).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolCreate {
    pub bytes: u64,
    /// Nonzero id chosen by the Unix half.
    pub pool: u64,
}

/// `POOL_DESTROY`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolDestroy {
    pub pool: u64,
}

/// `WINDOW_ADD`, `WINDOW_REMOVE`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Window {
    pub addr: u64,
    pub len: u64,
}

/// One view: `len` bytes of the pool from `offset`, at `addr`, replacing
/// whatever is there (a view or unmapped: one `MAP_FIXED` mmap, no unmap
/// first; S4: 0.8-1.1 µs against 1.5 µs for unmap + map).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MapOp {
    pub addr: u64,
    pub len: u64,
    pub offset: u64,
    pub prot: u32,
    /// [`MAP_WATCH`] and nothing else yet.
    pub flags: u32,
}

/// [`MapOp::flags`]: register and arm write tracking on the new view right
/// after its mmap (a `MAP_FIXED` mmap drops any earlier registration: S1).
pub const MAP_WATCH: u32 = 1;

/// An address range and (for `PROTECT`) its new protection.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Range {
    pub addr: u64,
    pub len: u64,
    pub prot: u32,
    pub _pad: u32,
}

/// `MAP` (ops are [`MapOp`], `pool` names the pool), `UNMAP`, `PROTECT`,
/// `WATCH_REGISTER` (ops are [`Range`], `pool` 0).
///
/// `results` has room for `count` `i32`s: 0 or the operation's Linux errno.
/// `done` is set to the number of operations that succeeded. Operations run
/// in order; one failing does not stop the rest.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Batch {
    pub pool: u64,
    pub ops: u64,
    pub count: u64,
    pub results: u64,
    pub done: u64,
}

/// `WATCH_TAKE`: the 4 KiB pages written through `[addr, addr + len)` since
/// they were registered or last taken (the kernel reports a write only at
/// the view it went through, not at aliases of the same pool page: S1;
/// `victory-bridge` fans them out to the aliases from its map), as addresses into `pages` (room for
/// `capacity`); `count` is set to how many were written there. They are
/// re-armed. `more` is set nonzero when `capacity` ran out (the caller
/// takes again from the address after the last one).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WatchTake {
    pub addr: u64,
    pub len: u64,
    pub pages: u64,
    pub capacity: u64,
    pub count: u64,
    pub more: u64,
}

/// `COPY`: `len` bytes from `from` to `to`; `copied` is set to the bytes
/// copied before a fault (all of them when nothing faulted).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Copy {
    pub to: u64,
    pub from: u64,
    pub len: u64,
    pub copied: u64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// The same numbers as `bridge/unix/abi.h`'s `_Static_assert`s.
    #[test]
    fn layout_is_fixed() {
        assert_eq!(size_of::<Version>(), 16);
        assert_eq!(size_of::<PoolCreate>(), 16);
        assert_eq!(size_of::<PoolDestroy>(), 8);
        assert_eq!(size_of::<Window>(), 16);
        assert_eq!(size_of::<MapOp>(), 32);
        assert_eq!(offset_of!(MapOp, prot), 24);
        assert_eq!(size_of::<Range>(), 24);
        assert_eq!(offset_of!(Range, prot), 16);
        assert_eq!(size_of::<Batch>(), 40);
        assert_eq!(offset_of!(Batch, done), 32);
        assert_eq!(size_of::<WatchTake>(), 48);
        assert_eq!(offset_of!(WatchTake, more), 40);
        assert_eq!(size_of::<Copy>(), 32);
        assert_eq!(CALLS, 11);
    }

    /// The header states the same version and call numbers.
    #[test]
    fn header_mirrors_this_file() {
        let header = include_str!("../../../bridge/unix/abi.h");
        let want = [
            ("VB_VERSION", VERSION),
            ("VB_VERSION_CALL", VERSION_CALL),
            ("VB_POOL_CREATE", POOL_CREATE),
            ("VB_POOL_DESTROY", POOL_DESTROY),
            ("VB_WINDOW_ADD", WINDOW_ADD),
            ("VB_WINDOW_REMOVE", WINDOW_REMOVE),
            ("VB_MAP", MAP),
            ("VB_UNMAP", UNMAP),
            ("VB_PROTECT", PROTECT),
            ("VB_WATCH_REGISTER", WATCH_REGISTER),
            ("VB_WATCH_TAKE", WATCH_TAKE),
            ("VB_COPY", COPY),
            ("VB_CALLS", CALLS),
        ];
        for (name, value) in want {
            let line = format!("#define {name} {value}\n");
            assert!(header.contains(&line), "abi.h lacks `{}`", line.trim());
        }
        assert!(header.contains(&format!("#define VB_BATCH_MAX {BATCH_MAX}\n")));
    }
}
