//! The Unix half emulated with Win32 calls, for tests of the PE side's
//! logic (xmem's bridge path) under Wine or Windows without the real DLL.
//! Not a model of the real one's speed or of Linux: the real Unix half
//! (WS1) and the DLL (WS2) are tested on their own.
//!
//! - A pool is a pagefile-backed section (`SEC_COMMIT`); its id is the
//!   section's handle.
//! - A window: the range xmem reserved through Wine (`MEM_RESERVE`) is
//!   released and reserved again at once as a placeholder (another thread
//!   could take the address in between: tests only), and handed back as a
//!   plain reservation by `WINDOW_REMOVE`.
//! - A view is one `MapViewOfFile3` per 64 KiB of it over a placeholder of
//!   that size (cut out first; cutting a placeholder that is that size
//!   already fails, harmlessly), so that any part of a view can be replaced
//!   or unmapped on its own; unmapped is a placeholder again
//!   (`UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)`). `MAP` over a view
//!   unmaps it first. Protections: `VirtualProtect`.
//! - No write tracking and no copies (`Version::features` 0): callers take
//!   their own ways, as with a Unix half that cannot.
//! - Addresses, lengths and pool offsets of views must be multiples of
//!   64 KiB (Windows' granularity for views), protections 4 KiB.

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::c_void;
use std::sync::Mutex;

use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, INVALID_HANDLE_VALUE};
use windows_sys::Win32::System::Memory::{
    CreateFileMappingW, MEM_PRESERVE_PLACEHOLDER, MEM_RELEASE, MEM_REPLACE_PLACEHOLDER,
    MEM_RESERVE, MEM_RESERVE_PLACEHOLDER, MEMORY_MAPPED_VIEW_ADDRESS, MapViewOfFile3,
    PAGE_NOACCESS, PAGE_READWRITE, SEC_COMMIT, UnmapViewOfFile2, VirtualAlloc, VirtualAlloc2,
    VirtualFree, VirtualProtect,
};
use windows_sys::Win32::System::Threading::GetCurrentProcess;

use crate::{Prot, abi};

/// Windows' granularity of views and placeholders.
const UNIT: usize = 64 << 10;
/// `VirtualFree`'s flag to join placeholders (not in windows-sys).
const MEM_COALESCE_PLACEHOLDERS: u32 = 1;
/// The errno a refused op gets when Windows gives no error.
const EINVAL: i32 = 22;

#[derive(Default)]
struct State {
    /// Windows by base: length.
    windows: BTreeMap<usize, usize>,
    /// The 64 KiB units that hold a view now.
    views: BTreeSet<usize>,
}

static STATE: Mutex<State> = Mutex::new(State {
    windows: BTreeMap::new(),
    views: BTreeSet::new(),
});

impl State {
    fn inside(&self, addr: usize, len: usize) -> bool {
        len > 0
            && self
                .windows
                .range(..=addr)
                .next_back()
                .is_some_and(|(&base, &wlen)| {
                    addr.checked_add(len).is_some_and(|end| end <= base + wlen)
                })
    }

    /// The unit at `at` back to a placeholder of its own (unmapped).
    fn unmap_unit(&mut self, at: usize) -> Result<(), i32> {
        if !self.views.contains(&at) {
            return Ok(());
        }
        // SAFETY: a view we mapped; it becomes a placeholder again.
        let gone = unsafe {
            UnmapViewOfFile2(
                GetCurrentProcess(),
                MEMORY_MAPPED_VIEW_ADDRESS {
                    Value: at as *mut c_void,
                },
                MEM_PRESERVE_PLACEHOLDER,
            )
        };
        if gone == 0 {
            return Err(last_error());
        }
        self.views.remove(&at);
        Ok(())
    }

    fn map(&mut self, pool: u64, op: &abi::MapOp) -> Result<(), i32> {
        let (addr, len, offset) = (op.addr as usize, op.len as usize, op.offset as usize);
        if !addr.is_multiple_of(UNIT) || !len.is_multiple_of(UNIT) || !offset.is_multiple_of(UNIT) {
            return Err(EINVAL);
        }
        if !self.inside(addr, len) {
            return Err(EINVAL);
        }
        for k in (0..len).step_by(UNIT) {
            let at = addr + k;
            self.unmap_unit(at)?;
            // SAFETY (the block): acts only on placeholders of a window.
            unsafe {
                // Cut out of a larger placeholder (fails when it is the
                // unit's own already).
                VirtualFree(
                    at as *mut c_void,
                    UNIT,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER,
                );
                let view = MapViewOfFile3(
                    pool as usize as _,
                    GetCurrentProcess(),
                    at as *const c_void,
                    (offset + k) as u64,
                    UNIT,
                    MEM_REPLACE_PLACEHOLDER,
                    PAGE_READWRITE,
                    std::ptr::null_mut(),
                    0,
                );
                if view.Value as usize != at {
                    return Err(last_error());
                }
            }
            self.views.insert(at);
            if op.prot != Prot::READ_WRITE.0 {
                protect(at, UNIT, Prot(op.prot))?;
            }
        }
        Ok(())
    }

    fn unmap(&mut self, r: &abi::Range) -> Result<(), i32> {
        let (addr, len) = (r.addr as usize, r.len as usize);
        if !addr.is_multiple_of(UNIT) || !len.is_multiple_of(UNIT) || !self.inside(addr, len) {
            return Err(EINVAL);
        }
        (addr..addr + len)
            .step_by(UNIT)
            .try_for_each(|at| self.unmap_unit(at))
    }

    fn protect(&mut self, r: &abi::Range) -> Result<(), i32> {
        let (addr, len) = (r.addr as usize, r.len as usize);
        let page = abi::GRANULE as usize;
        if !addr.is_multiple_of(page) || !len.is_multiple_of(page) || !self.inside(addr, len) {
            return Err(EINVAL);
        }
        // A piece per view unit: a protection change stays inside one view.
        let mut at = addr;
        while at < addr + len {
            let unit = at - at % UNIT;
            if !self.views.contains(&unit) {
                return Err(EINVAL);
            }
            let end = (unit + UNIT).min(addr + len);
            protect(at, end - at, Prot(r.prot))?;
            at = end;
        }
        Ok(())
    }

    fn window_add(&mut self, addr: usize, len: usize) -> bool {
        if !addr.is_multiple_of(UNIT) || !len.is_multiple_of(UNIT) || len == 0 {
            return false;
        }
        // SAFETY (the block): the caller's own reservation, made a
        // placeholder of the same range.
        unsafe {
            if VirtualFree(addr as *mut c_void, 0, MEM_RELEASE) == 0 {
                return false;
            }
            let at = VirtualAlloc2(
                GetCurrentProcess(),
                addr as *const c_void,
                len,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                PAGE_NOACCESS,
                std::ptr::null_mut(),
                0,
            );
            if at as usize != addr {
                if !at.is_null() {
                    VirtualFree(at, 0, MEM_RELEASE);
                }
                // Back as it was, if the address is still free.
                VirtualAlloc(addr as *const c_void, len, MEM_RESERVE, PAGE_NOACCESS);
                return false;
            }
        }
        self.windows.insert(addr, len);
        true
    }

    fn window_remove(&mut self, addr: usize, len: usize) -> bool {
        if self.windows.get(&addr) != Some(&len) {
            return false;
        }
        for at in (addr..addr + len).step_by(UNIT) {
            if self.unmap_unit(at).is_err() {
                return false;
            }
        }
        self.windows.remove(&addr);
        // SAFETY (the block): placeholders of ours, made one plain
        // reservation again for the caller to release through Windows.
        unsafe {
            // (Fails when it is one placeholder already.)
            VirtualFree(
                addr as *mut c_void,
                len,
                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS,
            );
            VirtualFree(addr as *mut c_void, 0, MEM_RELEASE);
            VirtualAlloc(addr as *const c_void, len, MEM_RESERVE, PAGE_NOACCESS) as usize == addr
        }
    }
}

fn last_error() -> i32 {
    // SAFETY: plain call.
    match unsafe { GetLastError() } {
        0 => EINVAL,
        e => e as i32,
    }
}

fn protect(at: usize, len: usize, prot: Prot) -> Result<(), i32> {
    let mut old = 0;
    // SAFETY: part of a view we mapped.
    match unsafe { VirtualProtect(at as *const c_void, len, prot.to_page(), &mut old) } {
        0 => Err(last_error()),
        _ => Ok(()),
    }
}

/// Runs `one` on each op of the batch at `args`; per-op results.
///
/// # Safety
/// `args` is a [`abi::Batch`] whose ops are `T`s.
unsafe fn batch<T>(args: *mut c_void, mut one: impl FnMut(&T) -> Result<(), i32>) -> i32 {
    // SAFETY: as the caller says; the arrays are the caller's, `count` long.
    unsafe {
        let b = &mut *(args as *mut abi::Batch);
        let ops = std::slice::from_raw_parts(b.ops as *const T, b.count as usize);
        let mut done = 0;
        for (i, op) in ops.iter().enumerate() {
            let r = one(op).err().unwrap_or(0);
            done += u64::from(r == 0);
            if b.results != 0 {
                (b.results as *mut i32).add(i).write(r);
            }
        }
        b.done = done;
    }
    0
}

/// The emulated `victorybridge_call`.
unsafe extern "system" fn entry(code: u32, args: *mut c_void) -> i32 {
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    // SAFETY (the block): each arm reads the struct the ABI names for the
    // code, which the caller passes.
    unsafe {
        match code {
            abi::VERSION_CALL => {
                *(args as *mut abi::Version) = abi::Version {
                    version: abi::VERSION,
                    _pad: 0,
                    features: 0,
                };
                0
            }
            abi::POOL_CREATE => {
                let a = &mut *(args as *mut abi::PoolCreate);
                let handle = CreateFileMappingW(
                    INVALID_HANDLE_VALUE,
                    std::ptr::null(),
                    PAGE_READWRITE | SEC_COMMIT,
                    (a.bytes >> 32) as u32,
                    a.bytes as u32,
                    std::ptr::null(),
                );
                a.pool = handle as usize as u64;
                i32::from(handle.is_null())
            }
            abi::POOL_DESTROY => {
                let a = &*(args as *mut abi::PoolDestroy);
                i32::from(CloseHandle(a.pool as usize as _) == 0)
            }
            abi::WINDOW_ADD => {
                let a = &*(args as *mut abi::Window);
                i32::from(!state.window_add(a.addr as usize, a.len as usize))
            }
            abi::WINDOW_REMOVE => {
                let a = &*(args as *mut abi::Window);
                i32::from(!state.window_remove(a.addr as usize, a.len as usize))
            }
            abi::MAP => {
                let pool = (*(args as *mut abi::Batch)).pool;
                batch(args, |op: &abi::MapOp| state.map(pool, op))
            }
            abi::UNMAP => batch(args, |r: &abi::Range| state.unmap(r)),
            abi::PROTECT => batch(args, |r: &abi::Range| state.protect(r)),
            // Write tracking and copies: not offered (features 0).
            _ => 1,
        }
    }
}

/// Makes the emulated Unix half the process's bridge, whatever
/// `VICTORY_BRIDGE` says ([`crate::available`] is true from then on). True
/// once it is (again or for the first time); false when another bridge was
/// installed before.
pub fn install() -> bool {
    static ONCE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ONCE.get_or_init(|| crate::install(entry))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Kind, MapOp, PagePool};

    const K64: usize = UNIT;

    fn reserve(len: usize) -> usize {
        // SAFETY: a plain reservation.
        let at =
            unsafe { VirtualAlloc(std::ptr::null(), len, MEM_RESERVE, PAGE_NOACCESS) } as usize;
        assert_ne!(at, 0);
        at
    }

    fn op(addr: usize, offset: usize) -> MapOp {
        MapOp {
            addr,
            len: K64,
            offset: offset as u64,
            prot: Prot::READ_WRITE,
            watch: false,
        }
    }

    /// Views of one pool see each other's bytes, a remap replaces what is
    /// there in one op, unmapped is reserved again, and the window goes
    /// back to Windows as a plain reservation. (On a bridge of its own: the
    /// crate's other tests want none installed.)
    #[test]
    fn the_emulated_half_maps_aliases_and_gives_the_window_back() {
        let b: &'static crate::Bridge =
            Box::leak(Box::new(crate::Bridge::new(entry).expect("our ABI")));
        let pool = b.pool(4 * K64 as u64).expect("a pool");
        let w = reserve(8 * K64);
        assert!(b.window_add(w, 8 * K64));
        assert!(b.bridged(w) && !b.bridged(w + 8 * K64));
        // Two views of page 1, and one of page 2.
        assert_eq!(
            pool.map(&[op(w, K64), op(w + 3 * K64, K64), op(w + K64, 2 * K64)]),
            [true; 3]
        );
        // SAFETY (the block): mapped read-write just above.
        unsafe {
            (w as *mut u8).write(7);
            assert_eq!(((w + 3 * K64) as *const u8).read(), 7);
            ((w + K64) as *mut u8).write(9);
        }
        // A remap in place: page 2 where page 1 was; a refused op (not
        // inside the window) leaves the others done.
        assert_eq!(
            pool.map(&[op(w, 2 * K64), op(w + 8 * K64, 0)]),
            [true, false]
        );
        // SAFETY: mapped.
        assert_eq!(unsafe { (w as *const u8).read() }, 9);
        assert!(
            matches!(b.query(w).unwrap().kind, Kind::View { offset, .. } if offset == 2 * K64 as u64)
        );
        // Read-only, then unmapped.
        assert!(b.protect(&[(w, K64, Prot::READ)]));
        assert_eq!(b.query(w).unwrap().prot, Prot::READ);
        assert_eq!(pool.unmap(&[(w, K64), (w + 3 * K64, K64)]), [true, true]);
        assert_eq!(b.query(w).unwrap().kind, Kind::Unmapped);
        assert!(!b.protect(&[(w, K64, Prot::READ_WRITE)]), "no view there");
        // The window goes with its views; Windows releases the range.
        drop(pool);
        assert_eq!(b.window_remove(w), Some(8 * K64));
        assert!(!b.bridged(w));
        // SAFETY: the plain reservation handed back.
        assert_ne!(unsafe { VirtualFree(w as *mut c_void, 0, MEM_RELEASE) }, 0);
    }
}
