//! The memory bridge (docs/memory-bridge-plan.md): under Wine/Proton on
//! Linux, the title's page pools, views, protections and write tracking done
//! with Linux calls by the Unix half of a Wine builtin DLL
//! (`victorybridge.dll` + `victorybridge.so`, beside `victory.exe`), instead
//! of through Wine's memory bookkeeping (one list and one lock over every
//! view: 337 µs per map+unmap at 60,000 views, against ~1.5 µs here).
//!
//! # The contract
//!
//! - **Off unless asked.** [`available`] is true only when `VICTORY_BRIDGE`
//!   is set and not `0`, we run under Wine, the DLL loaded and its ABI
//!   version matches. Otherwise every function here reports "not mine"
//!   (`None`, `false`, empty) and does nothing, and callers keep the present
//!   path: with `VICTORY_BRIDGE=0` (the default) and always on Windows, the
//!   program behaves exactly as without this crate.
//! - **Windows.** The bridge works only inside *windows*: ranges reserved
//!   through Wine (`VirtualAlloc(MEM_RESERVE)`, one per title pool range,
//!   made by xmem) and then handed over with [`window_add`]. Wine keeps
//!   believing they are reserved and never places anything there.
//! - **Only the bridge changes pages inside a window.** Measured under Proton
//!   (spike S2), these must never reach Wine there: `VirtualAlloc(MEM_COMMIT)`
//!   (succeeds, `mprotect`s our view, and Wine then believes the pages are
//!   its own), `VirtualFree(MEM_DECOMMIT)` (replaces our view),
//!   `VirtualFree(MEM_RELEASE)` (unmaps every view: [`window_remove`] first),
//!   `VirtualProtect`/`NtProtectVirtualMemory`, `MapViewOfFile*`; nor these,
//!   whose answers are wrong there: `VirtualQuery`/`NtQueryVirtualMemory`
//!   ("reserved"), `GetWriteWatch` (sees nothing), `WriteProcessMemory`
//!   (always fails; `NtWriteVirtualMemory` works). Reads and faults are fine:
//!   a fault reaches the vectored handler with the right address and kind. Code that
//!   may meet a window asks [`bridged`] first, or uses the drop-ins
//!   [`virtual_query`] and [`virtual_protect`], which answer for windows and
//!   pass anything else to Wine unchanged.
//! - **Questions about a window are answered here** ([`query`]), from our
//!   own map ([`map::Map`]), never by Wine (whose answer is "reserved, no
//!   access" for the whole window).
//! - **Batches.** [`PagePool::map`], [`unmap`], [`protect`] take many
//!   ranges and make one unix call per [`abi::BATCH_MAX`] of them: a frame's
//!   remapping is ~2,100 operations.
//!
//! The ABI is in [`abi`] (mirrored in `bridge/unix/abi.h`).

pub mod abi;
pub mod intercept;
pub mod map;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock, RwLock};

pub use map::{Kind, Region};

/// A protection: Linux `PROT_*` bits ([`abi::PROT_READ`] ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Prot(pub u32);

impl Prot {
    pub const NONE: Prot = Prot(abi::PROT_NONE);
    pub const READ: Prot = Prot(abi::PROT_READ);
    pub const READ_WRITE: Prot = Prot(abi::PROT_READ | abi::PROT_WRITE);
    pub const READ_EXEC: Prot = Prot(abi::PROT_READ | abi::PROT_EXEC);
    pub const ALL: Prot = Prot(abi::PROT_READ | abi::PROT_WRITE | abi::PROT_EXEC);

    /// From a Windows `PAGE_*` protection; `None` for what the bridge does
    /// not do (guard pages, no-cache, write-copy).
    pub fn from_page(page: u32) -> Option<Prot> {
        Some(match page {
            0x01 => Prot::NONE,           // PAGE_NOACCESS
            0x02 => Prot::READ,           // PAGE_READONLY
            0x04 => Prot::READ_WRITE,     // PAGE_READWRITE
            0x10 => Prot(abi::PROT_EXEC), // PAGE_EXECUTE
            0x20 => Prot::READ_EXEC,      // PAGE_EXECUTE_READ
            0x40 => Prot::ALL,            // PAGE_EXECUTE_READWRITE
            _ => return None,
        })
    }

    /// As a Windows `PAGE_*` protection.
    pub fn to_page(self) -> u32 {
        let (r, w, x) = (
            self.0 & abi::PROT_READ != 0,
            self.0 & abi::PROT_WRITE != 0,
            self.0 & abi::PROT_EXEC != 0,
        );
        match (x, w, r) {
            (false, false, false) => 0x01,
            (false, false, true) => 0x02,
            (false, true, _) => 0x04,
            (true, false, false) => 0x10,
            (true, false, true) => 0x20,
            (true, true, _) => 0x40,
        }
    }
}

/// One view to map: `len` bytes of the pool from byte `offset`, at `addr`
/// (inside a window; all multiples of 4 KiB).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MapOp {
    pub addr: usize,
    pub len: usize,
    pub offset: u64,
    pub prot: Prot,
    /// Arm write tracking on the new view in the same call
    /// ([`abi::MAP_WATCH`]; ignored when the Unix half cannot track).
    pub watch: bool,
}

/// A pool of physical pages (one per xmem section): one memfd in the Unix
/// half. Its views stay valid after the pool is dropped, until unmapped.
pub trait PagePool {
    /// A pool of `bytes` (a multiple of 4 KiB); `None` when the bridge is not
    /// available (the caller keeps its own section).
    fn create(bytes: u64) -> Option<Self>
    where
        Self: Sized;
    /// Maps each op (replacing whatever was there: a view or unmapped); one
    /// bool per op, true when done.
    fn map(&self, ops: &[MapOp]) -> Vec<bool>;
    /// Returns each `(addr, len)` to unmapped (still reserved, still ours).
    fn unmap(&self, ranges: &[(usize, usize)]) -> Vec<bool>;
}

/// The bridge's [`PagePool`].
#[derive(Debug)]
pub struct Pool {
    id: u64,
    bytes: u64,
    bridge: &'static Bridge,
}

impl Pool {
    /// The Unix half's id of this pool (what [`Kind::View`] names).
    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn bytes(&self) -> u64 {
        self.bytes
    }
}

impl PagePool for Pool {
    fn create(bytes: u64) -> Option<Pool> {
        bridge()?.pool(bytes)
    }

    fn map(&self, ops: &[MapOp]) -> Vec<bool> {
        self.bridge.map(self, ops)
    }

    fn unmap(&self, ranges: &[(usize, usize)]) -> Vec<bool> {
        self.bridge.unmap(ranges)
    }
}

impl Drop for Pool {
    fn drop(&mut self) {
        let mut a = abi::PoolDestroy { pool: self.id };
        self.bridge.call(abi::POOL_DESTROY, &mut a);
    }
}

/// Whether `VICTORY_BRIDGE` asks for the bridge (set, and not `0`).
pub fn asked() -> bool {
    switch(std::env::var("VICTORY_BRIDGE").ok().as_deref())
}

fn switch(value: Option<&str>) -> bool {
    value.is_some_and(|v| !v.is_empty() && v != "0")
}

/// The bridge is loaded and on (see the crate's documentation).
pub fn available() -> bool {
    bridge().is_some()
}

/// Whether `addr` is inside one of the bridge's windows: then only this
/// crate may change it or say what it holds.
pub fn bridged(addr: usize) -> bool {
    bridge().is_some_and(|b| b.bridged(addr))
}

/// Hands `[addr, addr + len)`, just reserved through Wine
/// (`VirtualAlloc(MEM_RESERVE)`, `PAGE_NOACCESS`), to the bridge. False
/// (and nothing changed) when the bridge is not available or refuses.
pub fn window_add(addr: usize, len: usize) -> bool {
    bridge().is_some_and(|b| b.window_add(addr, len))
}

/// Takes back the window starting at `addr` (its views unmapped first); the
/// caller then frees the reservation through Wine. Its length, when it was
/// one.
pub fn window_remove(addr: usize) -> Option<usize> {
    bridge()?.window_remove(addr)
}

/// Returns each `(addr, len)` to unmapped (any pool's views).
pub fn unmap(ranges: &[(usize, usize)]) -> Vec<bool> {
    match bridge() {
        Some(b) => b.unmap(ranges),
        None => vec![false; ranges.len()],
    }
}

/// New protections on bridged views. True when every range was done; false
/// at once (nothing done) when any range is not entirely bridged views.
pub fn protect(ranges: &[(usize, usize, Prot)]) -> bool {
    bridge().is_some_and(|b| b.protect(ranges))
}

/// What the bridge's window holds at `addr`; `None` when `addr` is not
/// bridged (ask Wine then, or use [`virtual_query`]).
pub fn query(addr: usize) -> Option<Region> {
    bridge()?.query(addr)
}

/// Arms write tracking on bridged views `[addr, addr + len)`: writes there
/// are noted by the kernel without any fault. False when not bridged or the
/// Unix half cannot (the caller keeps its guards then).
pub fn watch_register(addr: usize, len: usize) -> bool {
    bridge().is_some_and(|b| b.watch_register(addr, len))
}

/// Appends to `out` the 4 KiB pages of `[addr, addr + len)` written through
/// these addresses since they were registered or last taken, and re-arms
/// them. A write through an alias (another view of the same pool page) is
/// reported at the alias only (S1): callers that care fan it out (xmem's
/// page table; WS5). False when not
/// bridged or not tracked (`out` unchanged then).
pub fn watch_take(addr: usize, len: usize, out: &mut Vec<usize>) -> bool {
    bridge().is_some_and(|b| b.watch_take(addr, len, out))
}

/// Copies `len` bytes from `from` to `to` without faulting (the Unix half's
/// `process_vm_readv` on ourselves); the bytes copied before a fault.
/// `None` when the bridge cannot (the caller copies its own way).
///
/// # Safety
/// `to` must be valid for `len` bytes of writes that nothing else reads or
/// writes meanwhile.
pub unsafe fn copy(to: *mut u8, from: *const u8, len: usize) -> Option<usize> {
    bridge()?.copy(to as usize, from as usize, len)
}

/// The Unix half's entry, through the PE half's export.
type Entry = unsafe extern "system" fn(code: u32, args: *mut core::ffi::c_void) -> i32;

/// A loaded bridge: the entry, what it can do, and the windows' map.
pub struct Bridge {
    entry: Entry,
    features: u64,
    map: RwLock<map::Map>,
    /// Some window exists ([`bridged`]'s quick no).
    any: AtomicBool,
    /// Changes of windows, views and protections, one at a time, so the map
    /// follows the Unix half's order.
    change: Mutex<()>,
}

impl std::fmt::Debug for Bridge {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bridge")
            .field("features", &self.features)
            .finish_non_exhaustive()
    }
}

fn bridge() -> Option<&'static Bridge> {
    static BRIDGE: OnceLock<Option<Bridge>> = OnceLock::new();
    BRIDGE.get_or_init(load).as_ref()
}

fn load() -> Option<Bridge> {
    if !asked() {
        return None;
    }
    let bridge = loader::entry().and_then(Bridge::new);
    match &bridge {
        Some(b) => tracing::info!(features = b.features, "memory bridge on (VICTORY_BRIDGE)"),
        None => tracing::warn!(
            "VICTORY_BRIDGE is set but the memory bridge is not available (not under Wine, {} missing beside victory.exe, or another ABI version): the present path stays",
            abi::DLL
        ),
    }
    bridge
}

impl Bridge {
    /// A bridge on `entry`, when its ABI version is ours.
    pub fn new(entry: Entry) -> Option<Bridge> {
        let mut v = abi::Version::default();
        // SAFETY: the entry takes the call's argument struct.
        let status = unsafe { entry(abi::VERSION_CALL, (&raw mut v).cast()) };
        if status != 0 || v.version != abi::VERSION {
            tracing::warn!(
                status,
                version = v.version,
                want = abi::VERSION,
                "memory bridge: ABI version mismatch"
            );
            return None;
        }
        Some(Bridge {
            entry,
            features: v.features,
            map: RwLock::default(),
            any: AtomicBool::new(false),
            change: Mutex::new(()),
        })
    }

    fn call<T>(&self, code: u32, args: &mut T) -> bool {
        // SAFETY: `args` is the struct the ABI names for `code`; arrays it
        // points at are the caller's and outlive the call.
        unsafe { (self.entry)(code, (args as *mut T).cast()) == 0 }
    }

    fn map_read(&self) -> std::sync::RwLockReadGuard<'_, map::Map> {
        self.map.read().unwrap_or_else(|e| e.into_inner())
    }

    fn map_write(&self) -> std::sync::RwLockWriteGuard<'_, map::Map> {
        self.map.write().unwrap_or_else(|e| e.into_inner())
    }

    fn changing(&self) -> std::sync::MutexGuard<'_, ()> {
        self.change.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn bridged(&self, addr: usize) -> bool {
        self.any.load(Ordering::Acquire) && self.map_read().window(addr).is_some()
    }

    pub fn pool(&'static self, bytes: u64) -> Option<Pool> {
        if bytes == 0 || !bytes.is_multiple_of(abi::GRANULE) {
            return None;
        }
        let mut a = abi::PoolCreate { bytes, pool: 0 };
        (self.call(abi::POOL_CREATE, &mut a) && a.pool != 0).then_some(Pool {
            id: a.pool,
            bytes,
            bridge: self,
        })
    }

    pub fn window_add(&self, addr: usize, len: usize) -> bool {
        let _change = self.changing();
        if !self.map_read().window_fits(addr, len) {
            return false;
        }
        let mut a = abi::Window {
            addr: addr as u64,
            len: len as u64,
        };
        if !self.call(abi::WINDOW_ADD, &mut a) {
            return false;
        }
        let added = self.map_write().add_window(addr, len);
        self.any.store(true, Ordering::Release);
        added
    }

    pub fn window_remove(&self, addr: usize) -> Option<usize> {
        let _change = self.changing();
        let (base, len) = self.map_read().window(addr).filter(|w| w.0 == addr)?;
        let mut a = abi::Window {
            addr: base as u64,
            len: len as u64,
        };
        if !self.call(abi::WINDOW_REMOVE, &mut a) {
            return None;
        }
        self.map_write().remove_window(base)
    }

    /// One batch call per [`abi::BATCH_MAX`] ops; the per-op results.
    fn batch<T>(&self, code: u32, pool: u64, ops: &[T]) -> Vec<bool> {
        let mut done = Vec::with_capacity(ops.len());
        for chunk in ops.chunks(abi::BATCH_MAX as usize) {
            let mut results = vec![-1i32; chunk.len()];
            let mut a = abi::Batch {
                pool,
                ops: chunk.as_ptr() as u64,
                count: chunk.len() as u64,
                results: results.as_mut_ptr() as u64,
                done: 0,
            };
            if self.call(code, &mut a) {
                done.extend(results.iter().map(|&r| r == 0));
            } else {
                done.extend(std::iter::repeat_n(false, chunk.len()));
            }
        }
        done
    }

    pub fn map(&self, pool: &Pool, ops: &[MapOp]) -> Vec<bool> {
        let _change = self.changing();
        let fits: Vec<bool> = {
            let m = self.map_read();
            ops.iter()
                .map(|op| {
                    m.inside(op.addr, op.len)
                        && op
                            .offset
                            .checked_add(op.len as u64)
                            .is_some_and(|e| e <= pool.bytes)
                })
                .collect()
        };
        let wire: Vec<abi::MapOp> = ops
            .iter()
            .zip(&fits)
            .filter(|(_, fits)| **fits)
            .map(|(op, _)| abi::MapOp {
                addr: op.addr as u64,
                len: op.len as u64,
                offset: op.offset,
                prot: op.prot.0,
                flags: if op.watch && self.features & abi::FEATURE_WATCH != 0 {
                    abi::MAP_WATCH
                } else {
                    0
                },
            })
            .collect();
        let mut sent = self.batch(abi::MAP, pool.id, &wire).into_iter();
        let done: Vec<bool> = fits
            .iter()
            .map(|&f| f && sent.next().unwrap_or(false))
            .collect();
        let mut m = self.map_write();
        for ((op, fits), done) in ops.iter().zip(&fits).zip(&done) {
            if *done {
                m.map(op.addr, op.len, pool.id, op.offset, op.prot);
            } else if *fits {
                // Sent and failed: a failed MAP_FIXED may already have
                // dropped what was there (WS1), so it is unmapped now.
                m.unmap(op.addr, op.len);
            }
        }
        done
    }

    pub fn unmap(&self, ranges: &[(usize, usize)]) -> Vec<bool> {
        let _change = self.changing();
        let fits: Vec<bool> = {
            let m = self.map_read();
            ranges.iter().map(|&(a, l)| m.inside(a, l)).collect()
        };
        let wire: Vec<abi::Range> = ranges
            .iter()
            .zip(&fits)
            .filter(|(_, f)| **f)
            .map(|(&(addr, len), _)| abi::Range {
                addr: addr as u64,
                len: len as u64,
                prot: 0,
                _pad: 0,
            })
            .collect();
        let mut sent = self.batch(abi::UNMAP, 0, &wire).into_iter();
        let done: Vec<bool> = fits
            .iter()
            .map(|&f| f && sent.next().unwrap_or(false))
            .collect();
        let mut m = self.map_write();
        for (&(addr, len), _) in ranges.iter().zip(&done).filter(|(_, d)| **d) {
            m.unmap(addr, len);
        }
        done
    }

    pub fn protect(&self, ranges: &[(usize, usize, Prot)]) -> bool {
        let _change = self.changing();
        if !ranges
            .iter()
            .all(|&(a, l, _)| self.map_read().all_views(a, l))
        {
            return false;
        }
        let wire: Vec<abi::Range> = ranges
            .iter()
            .map(|&(addr, len, p)| abi::Range {
                addr: addr as u64,
                len: len as u64,
                prot: p.0,
                _pad: 0,
            })
            .collect();
        let done = self.batch(abi::PROTECT, 0, &wire);
        let mut m = self.map_write();
        for (&(addr, len, p), _) in ranges.iter().zip(&done).filter(|(_, d)| **d) {
            m.protect(addr, len, p);
        }
        done.iter().all(|&d| d)
    }

    pub fn query(&self, addr: usize) -> Option<Region> {
        if !self.any.load(Ordering::Acquire) {
            return None;
        }
        self.map_read().query(addr)
    }

    pub fn watch_register(&self, addr: usize, len: usize) -> bool {
        if self.features & abi::FEATURE_WATCH == 0 || !self.map_read().all_views(addr, len) {
            return false;
        }
        let r = [abi::Range {
            addr: addr as u64,
            len: len as u64,
            prot: 0,
            _pad: 0,
        }];
        self.batch(abi::WATCH_REGISTER, 0, &r) == [true]
    }

    pub fn watch_take(&self, addr: usize, len: usize, out: &mut Vec<usize>) -> bool {
        if self.features & abi::FEATURE_WATCH == 0 || !self.map_read().inside(addr, len) {
            return false;
        }
        let (mut at, end) = (addr, addr + len);
        let mut pages = vec![0u64; 512];
        let before = out.len();
        while at < end {
            let mut a = abi::WatchTake {
                addr: at as u64,
                len: (end - at) as u64,
                pages: pages.as_mut_ptr() as u64,
                capacity: pages.len() as u64,
                count: 0,
                more: 0,
            };
            if !self.call(abi::WATCH_TAKE, &mut a) {
                out.truncate(before);
                return false;
            }
            let n = (a.count as usize).min(pages.len());
            out.extend(pages[..n].iter().map(|&p| p as usize));
            match (a.more != 0, pages[..n].last()) {
                (true, Some(&last)) => at = last as usize + abi::GRANULE as usize,
                _ => break,
            }
        }
        true
    }

    pub fn copy(&self, to: usize, from: usize, len: usize) -> Option<usize> {
        if self.features & abi::FEATURE_COPY == 0 {
            return None;
        }
        let mut a = abi::Copy {
            to: to as u64,
            from: from as u64,
            len: len as u64,
            copied: 0,
        };
        self.call(abi::COPY, &mut a).then_some(a.copied as usize)
    }
}

/// `VirtualQuery`, answered by the bridge for its windows and by Wine for
/// anything else. Same arguments and result as `VirtualQuery`.
///
/// # Safety
/// As `VirtualQuery`: `info` is writable for `size` bytes.
#[cfg(windows)]
pub unsafe fn virtual_query(
    addr: *const core::ffi::c_void,
    info: *mut windows_sys::Win32::System::Memory::MEMORY_BASIC_INFORMATION,
    size: usize,
) -> usize {
    use windows_sys::Win32::System::Memory::{
        MEM_COMMIT, MEM_MAPPED, MEM_PRIVATE, MEM_RESERVE, MEMORY_BASIC_INFORMATION, VirtualQuery,
    };
    let Some(r) = query(addr as usize) else {
        // SAFETY: the caller's arguments, as given.
        return unsafe { VirtualQuery(addr, info, size) };
    };
    if size < size_of::<MEMORY_BASIC_INFORMATION>() {
        return 0;
    }
    let view = matches!(r.kind, Kind::View { .. });
    let mbi = MEMORY_BASIC_INFORMATION {
        BaseAddress: r.base as *mut _,
        AllocationBase: r.window.0 as *mut _,
        AllocationProtect: 0x01, // PAGE_NOACCESS, as reserved
        PartitionId: 0,
        RegionSize: r.len,
        State: if view { MEM_COMMIT } else { MEM_RESERVE },
        Protect: if view { r.prot.to_page() } else { 0 },
        Type: if view { MEM_MAPPED } else { MEM_PRIVATE },
    };
    // SAFETY: checked large enough; the caller's buffer.
    unsafe { info.write(mbi) };
    size_of::<MEMORY_BASIC_INFORMATION>()
}

/// `VirtualProtect`, done by the bridge for its windows (views only) and by
/// Wine for anything else. Same arguments and result.
///
/// # Safety
/// As `VirtualProtect`.
#[cfg(windows)]
pub unsafe fn virtual_protect(
    addr: *const core::ffi::c_void,
    len: usize,
    new: u32,
    old: *mut u32,
) -> windows_sys::core::BOOL {
    use windows_sys::Win32::Foundation::{
        ERROR_INVALID_ADDRESS, ERROR_INVALID_PARAMETER, SetLastError,
    };
    use windows_sys::Win32::System::Memory::VirtualProtect;
    let at = addr as usize;
    let Some(r) = query(at) else {
        // SAFETY: the caller's arguments, as given.
        return unsafe { VirtualProtect(addr, len, new, old) };
    };
    let Some(prot) = Prot::from_page(new) else {
        // SAFETY: plain call.
        unsafe { SetLastError(ERROR_INVALID_PARAMETER) };
        return 0;
    };
    // Whole pages, as Windows rounds them.
    let base = at & !(abi::GRANULE as usize - 1);
    let end = (at + len.max(1)).next_multiple_of(abi::GRANULE as usize);
    if !protect(&[(base, end - base, prot)]) {
        // SAFETY: plain call.
        unsafe { SetLastError(ERROR_INVALID_ADDRESS) };
        return 0;
    }
    if !old.is_null() {
        // SAFETY: the caller's out pointer.
        unsafe { old.write(r.prot.to_page()) };
    }
    1
}

/// Finding the PE half: `victorybridge.dll` beside `victory.exe`, under Wine
/// only.
mod loader {
    use super::Entry;

    #[cfg(windows)]
    pub fn entry() -> Option<Entry> {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::System::LibraryLoader::{
            GetModuleHandleA, GetProcAddress, LoadLibraryW,
        };

        // SAFETY: plain lookups in ntdll, always loaded.
        let wine = unsafe {
            let ntdll = GetModuleHandleA(c"ntdll.dll".as_ptr().cast());
            !ntdll.is_null() && GetProcAddress(ntdll, c"wine_get_version".as_ptr().cast()).is_some()
        };
        if !wine {
            return None;
        }
        let beside = std::env::current_exe()
            .ok()
            .and_then(|exe| Some(exe.parent()?.join(super::abi::DLL)));
        let names = beside
            .into_iter()
            .chain([std::path::PathBuf::from(super::abi::DLL)]);
        for name in names {
            let wide: Vec<u16> = name.as_os_str().encode_wide().chain([0]).collect();
            // SAFETY: a NUL-terminated path.
            let module = unsafe { LoadLibraryW(wide.as_ptr()) };
            if module.is_null() {
                continue;
            }
            let symbol = std::ffi::CString::new(super::abi::ENTRY).ok()?;
            // SAFETY: a module we loaded, a NUL-terminated name.
            let found = unsafe { GetProcAddress(module, symbol.as_ptr().cast()) }?;
            // SAFETY: the export has the ABI's signature (abi.rs, ENTRY).
            return Some(unsafe {
                std::mem::transmute::<unsafe extern "system" fn() -> isize, Entry>(found)
            });
        }
        None
    }

    #[cfg(not(windows))]
    pub fn entry() -> Option<Entry> {
        None
    }
}

#[cfg(test)]
mod tests;
