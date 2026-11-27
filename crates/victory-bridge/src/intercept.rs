//! The other calls that must not reach Wine inside a window (WS4; spike S2),
//! beside the crate's [`virtual_query`](crate::virtual_query) and
//! [`virtual_protect`](crate::virtual_protect): drop-ins with the same
//! arguments and results as Windows' own, which answer or refuse for
//! windows and pass anything else to Wine unchanged. With the bridge off
//! (`VICTORY_BRIDGE` unset or `0`, no DLL, Windows) no window exists and
//! every one of them is exactly Wine's (or Windows') call.
//!
//! | Call | In a window (S2) | Here |
//! |---|---|---|
//! | `VirtualAlloc(MEM_COMMIT)` (also `MEM_RESET`, `MEM_RESET_UNDO`) | Wine succeeds, `mprotect`s our view and then believes the pages its own | refused, `ERROR_INVALID_ADDRESS` (as a placeholder or a mapped view refuses a commit today) |
//! | `VirtualAlloc(MEM_RESERVE)` at a window address | Wine refuses (reserved already) | Wine's |
//! | `VirtualFree` (decommit, release, placeholder split or join) | decommit replaces our view; release unmaps every view | refused, `ERROR_INVALID_PARAMETER` (as Wine refuses it on a mapped view); the window goes only through [`window_remove`](crate::window_remove), then Wine's release |
//! | `NtQueryVirtualMemory`, `QueryWorkingSetEx` | "reserved, no access"; working set: not valid | basic, region and working-set classes answered from the bridge's map |
//! | `GetWriteWatch` | sees nothing written | refused as Wine refuses memory that is not write-watched (`ERROR_INVALID_PARAMETER`); WS5 answers it with [`watch_take`](crate::watch_take) |
//! | `WriteProcessMemory` (own process) | always `ERROR_NOACCESS` (kernelbase asks `VirtualQuery` first) | `NtWriteVirtualMemory`, which writes the real pages |
//!
//! [`title_import`] gives the title's own imports of these (bound by name
//! through the host pass-through, `victory-hle`) when the bridge is on.

use crate::{Bridge, Kind, Region, abi, bridge};

const PAGE: usize = abi::GRANULE as usize;

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESET: u32 = 0x8_0000;
const MEM_RESET_UNDO: u32 = 0x100_0000;
/// `ERROR_INVALID_PARAMETER`.
pub const REFUSED_FREE: u32 = 87;
/// `ERROR_INVALID_ADDRESS`.
pub const REFUSED_COMMIT: u32 = 487;
/// `ERROR_INVALID_PARAMETER`, as Wine's `GetWriteWatch` on memory that is
/// not write-watched.
pub const REFUSED_WATCH: u32 = 87;

impl Bridge {
    /// Whether any page of `[addr, addr + len)` (one page at least) lies in
    /// one of the bridge's windows.
    pub fn meets(&self, addr: usize, len: usize) -> bool {
        if !self.any.load(std::sync::atomic::Ordering::Acquire) {
            return false;
        }
        let start = addr & !(PAGE - 1);
        let end = addr
            .saturating_add(len.max(1))
            .checked_next_multiple_of(PAGE)
            .unwrap_or(usize::MAX & !(PAGE - 1));
        let m = self.map_read();
        m.window(start).is_some() || (end > start && !m.window_fits(start, end - start))
    }
}

/// Whether any page of `[addr, addr + len)` lies in a bridge window (only
/// the bridge may change it or say what it holds). False with the bridge
/// off.
pub fn meets(addr: usize, len: usize) -> bool {
    bridge().is_some_and(|b| b.meets(addr, len))
}

/// What becomes of a call: Wine's own, or refused with this Win32 error.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Wine,
    Refuse(u32),
}

/// [`Verdict`] of `VirtualAlloc(addr, size, kind, _)`.
pub fn alloc_verdict(b: Option<&Bridge>, addr: usize, size: usize, kind: u32) -> Verdict {
    // Wine places a new allocation where nothing is: never in a window.
    // A reservation at a window's address Wine refuses itself.
    let changes_pages = kind & (MEM_COMMIT | MEM_RESET | MEM_RESET_UNDO) != 0;
    if addr != 0 && changes_pages && b.is_some_and(|b| b.meets(addr, size)) {
        Verdict::Refuse(REFUSED_COMMIT)
    } else {
        Verdict::Wine
    }
}

/// [`Verdict`] of `VirtualFree(addr, size, _)`: anything of a window is
/// refused (`size` 0 frees from `addr` on, inside its own reservation).
pub fn free_verdict(b: Option<&Bridge>, addr: usize, size: usize) -> Verdict {
    if b.is_some_and(|b| b.meets(addr, size)) {
        Verdict::Refuse(REFUSED_FREE)
    } else {
        Verdict::Wine
    }
}

/// [`Verdict`] of `GetWriteWatch(_, addr, size, ...)`.
pub fn watch_verdict(b: Option<&Bridge>, addr: usize, size: usize) -> Verdict {
    if b.is_some_and(|b| b.meets(addr, size)) {
        Verdict::Refuse(REFUSED_WATCH)
    } else {
        Verdict::Wine
    }
}

/// The bytes of the window holding `r` that are views (`CommitSize`).
fn committed_in(b: &Bridge, r: &Region) -> usize {
    let (base, len) = r.window;
    let mut at = base;
    let mut views = 0;
    while at < base + len {
        let Some(q) = b.map_read().query(at) else {
            break;
        };
        if matches!(q.kind, Kind::View { .. }) {
            views += q.len;
        }
        at = q.base + q.len.max(PAGE);
    }
    views
}

/// `MEMORY_REGION_INFORMATION` (classes 6 and 7 of `NtQueryVirtualMemory`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RegionInfo {
    pub allocation_base: usize,
    pub allocation_protect: u32,
    /// Flags: 1 private, `1 << 8` placeholder reservation.
    pub region_type: u32,
    pub region_size: usize,
    pub commit_size: usize,
    pub partition_id: usize,
    pub node_preference: usize,
}

/// `REGION_TYPE` flags.
pub const REGION_PRIVATE: u32 = 1;
pub const REGION_PLACEHOLDER: u32 = 1 << 8;

/// What the region classes say of a window: one private reservation, no
/// access, of the window's size, in which pages appear only by mapping a
/// view (as a placeholder reservation's), committed as much as its views.
pub fn region_info(b: &Bridge, r: &Region) -> RegionInfo {
    RegionInfo {
        allocation_base: r.window.0,
        allocation_protect: 0x01, // PAGE_NOACCESS
        region_type: REGION_PRIVATE | REGION_PLACEHOLDER,
        region_size: r.window.1,
        commit_size: committed_in(b, r),
        partition_id: 0,
        node_preference: 0,
    }
}

/// The working-set attributes (`MEMORY_WORKING_SET_EX_BLOCK`) of a window's
/// page: valid, shared once, with its protection, for a view; nothing for
/// an unmapped page.
pub fn working_set_bits(r: &Region) -> usize {
    match r.kind {
        Kind::View { .. } => {
            let valid = 1;
            let share_count = 1 << 1;
            let protection = (r.prot.to_page() as usize & 0x7ff) << 4;
            let shared = 1 << 15;
            valid | share_count | protection | shared
        }
        Kind::Unmapped => 0,
    }
}

#[cfg(windows)]
pub use windows::*;

#[cfg(windows)]
mod windows {
    use core::ffi::c_void;

    use windows_sys::Win32::Foundation::{HANDLE, SetLastError};
    use windows_sys::Win32::System::Memory::{
        MEM_COMMIT, MEM_MAPPED, MEM_PRIVATE, MEM_RESERVE, MEMORY_BASIC_INFORMATION,
    };
    use windows_sys::Win32::System::Threading::{
        GetCurrentProcess, GetCurrentProcessId, GetProcessId,
    };
    use windows_sys::core::BOOL;

    use super::{
        Verdict, alloc_verdict, free_verdict, region_info, watch_verdict, working_set_bits,
    };
    use crate::{Bridge, Kind, Region, bridge};

    #[link(name = "ntdll", kind = "raw-dylib")]
    unsafe extern "system" {
        fn NtQueryVirtualMemory(
            process: HANDLE,
            at: *const c_void,
            class: u32,
            out: *mut c_void,
            len: usize,
            returned: *mut usize,
        ) -> i32;
        fn NtWriteVirtualMemory(
            process: HANDLE,
            at: *mut c_void,
            from: *const c_void,
            len: usize,
            written: *mut usize,
        ) -> i32;
        fn RtlNtStatusToDosError(status: i32) -> u32;
    }

    #[link(name = "kernel32", kind = "raw-dylib")]
    unsafe extern "system" {
        fn WriteProcessMemory(
            process: HANDLE,
            at: *const c_void,
            from: *const c_void,
            len: usize,
            written: *mut usize,
        ) -> BOOL;
        fn GetWriteWatch(
            flags: u32,
            at: *const c_void,
            len: usize,
            found: *mut *mut c_void,
            count: *mut usize,
            granularity: *mut u32,
        ) -> u32;
        fn VirtualAlloc(at: *const c_void, len: usize, kind: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(at: *mut c_void, len: usize, kind: u32) -> BOOL;
        fn K32QueryWorkingSetEx(process: HANDLE, info: *mut c_void, len: u32) -> BOOL;
    }

    /// `NtQueryVirtualMemory`'s classes answered here.
    pub const MEMORY_BASIC_INFORMATION_CLASS: u32 = 0;
    pub const MEMORY_WORKING_SET_EX_INFORMATION: u32 = 4;
    pub const MEMORY_REGION_INFORMATION: u32 = 6;
    pub const MEMORY_REGION_INFORMATION_EX: u32 = 7;
    const STATUS_INFO_LENGTH_MISMATCH: i32 = 0xC000_0004_u32 as i32;

    /// Whether `process` is this process.
    fn ours(process: HANDLE) -> bool {
        // SAFETY: plain calls; a handle that is no process's gives 0.
        unsafe { process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId() }
    }

    fn refuse(error: u32) {
        // SAFETY: plain call.
        unsafe { SetLastError(error) };
    }

    /// What `VirtualQuery` says of a window's region.
    pub fn basic(r: &Region) -> MEMORY_BASIC_INFORMATION {
        let view = matches!(r.kind, Kind::View { .. });
        MEMORY_BASIC_INFORMATION {
            BaseAddress: r.base as *mut _,
            AllocationBase: r.window.0 as *mut _,
            AllocationProtect: 0x01, // PAGE_NOACCESS, as reserved
            PartitionId: 0,
            RegionSize: r.len,
            State: if view { MEM_COMMIT } else { MEM_RESERVE },
            Protect: if view { r.prot.to_page() } else { 0 },
            Type: if view { MEM_MAPPED } else { MEM_PRIVATE },
        }
    }

    /// `VirtualAlloc`, refused for commits in a window (see the module).
    ///
    /// # Safety
    /// As `VirtualAlloc`.
    pub unsafe fn virtual_alloc(
        at: *const c_void,
        len: usize,
        kind: u32,
        protect: u32,
    ) -> *mut c_void {
        // SAFETY: the caller's.
        unsafe { virtual_alloc_on(bridge(), at, len, kind, protect) }
    }

    /// [`virtual_alloc`] against `b`.
    ///
    /// # Safety
    /// As `VirtualAlloc`.
    pub unsafe fn virtual_alloc_on(
        b: Option<&Bridge>,
        at: *const c_void,
        len: usize,
        kind: u32,
        protect: u32,
    ) -> *mut c_void {
        match alloc_verdict(b, at as usize, len, kind) {
            // SAFETY: the caller's arguments, as given.
            Verdict::Wine => unsafe { VirtualAlloc(at, len, kind, protect) },
            Verdict::Refuse(e) => {
                refuse(e);
                std::ptr::null_mut()
            }
        }
    }

    /// `VirtualFree`, refused for anything of a window (see the module).
    ///
    /// # Safety
    /// As `VirtualFree`.
    pub unsafe fn virtual_free(at: *mut c_void, len: usize, kind: u32) -> BOOL {
        // SAFETY: the caller's.
        unsafe { virtual_free_on(bridge(), at, len, kind) }
    }

    /// [`virtual_free`] against `b`.
    ///
    /// # Safety
    /// As `VirtualFree`.
    pub unsafe fn virtual_free_on(
        b: Option<&Bridge>,
        at: *mut c_void,
        len: usize,
        kind: u32,
    ) -> BOOL {
        match free_verdict(b, at as usize, len) {
            // SAFETY: the caller's arguments, as given.
            Verdict::Wine => unsafe { VirtualFree(at, len, kind) },
            Verdict::Refuse(e) => {
                refuse(e);
                0
            }
        }
    }

    /// `GetWriteWatch`, never asked about a window (see the module).
    ///
    /// # Safety
    /// As `GetWriteWatch`.
    pub unsafe fn get_write_watch(
        flags: u32,
        at: *const c_void,
        len: usize,
        found: *mut *mut c_void,
        count: *mut usize,
        granularity: *mut u32,
    ) -> u32 {
        // SAFETY: the caller's.
        unsafe { get_write_watch_on(bridge(), flags, at, len, found, count, granularity) }
    }

    /// [`get_write_watch`] against `b`.
    ///
    /// # Safety
    /// As `GetWriteWatch`.
    pub unsafe fn get_write_watch_on(
        b: Option<&Bridge>,
        flags: u32,
        at: *const c_void,
        len: usize,
        found: *mut *mut c_void,
        count: *mut usize,
        granularity: *mut u32,
    ) -> u32 {
        match watch_verdict(b, at as usize, len) {
            // SAFETY: the caller's arguments, as given.
            Verdict::Wine => unsafe { GetWriteWatch(flags, at, len, found, count, granularity) },
            Verdict::Refuse(e) => {
                refuse(e);
                u32::MAX
            }
        }
    }

    /// `WriteProcessMemory`, by `NtWriteVirtualMemory` when the range meets
    /// a window of ours (see the module).
    ///
    /// # Safety
    /// As `WriteProcessMemory`.
    pub unsafe fn write_process_memory(
        process: HANDLE,
        at: *const c_void,
        from: *const c_void,
        len: usize,
        written: *mut usize,
    ) -> BOOL {
        // SAFETY: the caller's.
        unsafe { write_process_memory_on(bridge(), process, at, from, len, written) }
    }

    /// [`write_process_memory`] against `b`.
    ///
    /// # Safety
    /// As `WriteProcessMemory`.
    pub unsafe fn write_process_memory_on(
        b: Option<&Bridge>,
        process: HANDLE,
        at: *const c_void,
        from: *const c_void,
        len: usize,
        written: *mut usize,
    ) -> BOOL {
        if !b.is_some_and(|b| b.meets(at as usize, len)) || !ours(process) {
            // SAFETY: the caller's arguments, as given.
            return unsafe { WriteProcessMemory(process, at, from, len, written) };
        }
        let mut done = 0;
        // SAFETY: the caller's arguments; the kernel checks both ranges.
        let status = unsafe { NtWriteVirtualMemory(process, at as *mut _, from, len, &mut done) };
        if !written.is_null() {
            // SAFETY: the caller's out pointer.
            unsafe { written.write(done) };
        }
        if status < 0 {
            // SAFETY: plain call.
            refuse(unsafe { RtlNtStatusToDosError(status) });
            return 0;
        }
        1
    }

    /// `NtQueryVirtualMemory`, answered from the bridge's map for a window
    /// of this process (basic, region and working-set classes; any other
    /// class is Wine's).
    ///
    /// # Safety
    /// As `NtQueryVirtualMemory`.
    pub unsafe fn nt_query_virtual_memory(
        process: HANDLE,
        at: *const c_void,
        class: u32,
        out: *mut c_void,
        len: usize,
        returned: *mut usize,
    ) -> i32 {
        // SAFETY: the caller's.
        unsafe { nt_query_virtual_memory_on(bridge(), process, at, class, out, len, returned) }
    }

    /// Writes `value` to `out` (room for `len` bytes) and its size to
    /// `returned`; a length mismatch when it does not fit.
    ///
    /// # Safety
    /// `out` writable for `len` bytes, `returned` null or writable.
    unsafe fn answer<T: Copy>(value: T, out: *mut c_void, len: usize, returned: *mut usize) -> i32 {
        if !returned.is_null() {
            // SAFETY: the caller's out pointer.
            unsafe { returned.write(size_of::<T>()) };
        }
        if len < size_of::<T>() {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        // SAFETY: checked large enough; the caller's buffer.
        unsafe { out.cast::<T>().write_unaligned(value) };
        0
    }

    /// [`nt_query_virtual_memory`] against `b`.
    ///
    /// # Safety
    /// As `NtQueryVirtualMemory`.
    pub unsafe fn nt_query_virtual_memory_on(
        b: Option<&Bridge>,
        process: HANDLE,
        at: *const c_void,
        class: u32,
        out: *mut c_void,
        len: usize,
        returned: *mut usize,
    ) -> i32 {
        // SAFETY: the caller's arguments, as given.
        let wine = || unsafe { NtQueryVirtualMemory(process, at, class, out, len, returned) };
        let Some(b) = b.filter(|b| b.any.load(std::sync::atomic::Ordering::Acquire)) else {
            return wine();
        };
        if !ours(process) {
            return wine();
        }
        match class {
            MEMORY_BASIC_INFORMATION_CLASS
            | MEMORY_REGION_INFORMATION
            | MEMORY_REGION_INFORMATION_EX => {
                let Some(r) = b.query(at as usize) else {
                    return wine();
                };
                if class == MEMORY_BASIC_INFORMATION_CLASS {
                    // SAFETY: the caller's buffer and its length.
                    return unsafe { answer(basic(&r), out, len, returned) };
                }
                // SAFETY: as above.
                unsafe { answer(region_info(b, &r), out, len, returned) }
            }
            MEMORY_WORKING_SET_EX_INFORMATION => {
                // Wine answers every entry, then the window's are ours.
                let status = wine();
                if status >= 0 {
                    // SAFETY: the caller's buffer and its length.
                    unsafe { working_set_of_windows(b, out, len) };
                }
                status
            }
            _ => wine(),
        }
    }

    /// Answers the entries of a working-set question (`{ address,
    /// attributes }` pairs, `len` bytes) that lie in a window.
    ///
    /// # Safety
    /// `out` holds `len` bytes of such pairs.
    unsafe fn working_set_of_windows(b: &Bridge, out: *mut c_void, len: usize) {
        for i in 0..len / (2 * size_of::<usize>()) {
            // SAFETY: the caller's pairs.
            unsafe {
                let entry = out.cast::<usize>().add(2 * i);
                if let Some(r) = b.query(entry.read_unaligned()) {
                    entry.add(1).write_unaligned(working_set_bits(&r));
                }
            }
        }
    }

    /// `QueryWorkingSetEx` (`K32QueryWorkingSetEx`): the system's answer,
    /// with a window's pages answered from the bridge's map (Wine says "not
    /// valid" there).
    ///
    /// # Safety
    /// As `QueryWorkingSetEx`.
    pub unsafe fn query_working_set_ex(process: HANDLE, info: *mut c_void, len: u32) -> BOOL {
        // SAFETY: the caller's.
        unsafe { query_working_set_ex_on(bridge(), process, info, len) }
    }

    /// [`query_working_set_ex`] against `b`.
    ///
    /// # Safety
    /// As `QueryWorkingSetEx`.
    pub unsafe fn query_working_set_ex_on(
        b: Option<&Bridge>,
        process: HANDLE,
        info: *mut c_void,
        len: u32,
    ) -> BOOL {
        // SAFETY: the caller's arguments, as given.
        let done = unsafe { K32QueryWorkingSetEx(process, info, len) };
        if let Some(b) = b.filter(|b| b.any.load(std::sync::atomic::Ordering::Acquire))
            && done != 0
            && ours(process)
        {
            // SAFETY: the caller's buffer and its length.
            unsafe { working_set_of_windows(b, info, len as usize) };
        }
        done
    }

    /// The title's own imports of the calls above (and the crate's
    /// [`virtual_query`](crate::virtual_query) /
    /// [`virtual_protect`](crate::virtual_protect)) by name, when the bridge
    /// is on; `None` otherwise (and for any other name): the import stays
    /// the host's.
    pub fn title_import(name: &str) -> Option<usize> {
        if !crate::available() {
            return None;
        }
        Some(match name {
            "VirtualAlloc" => title::virtual_alloc as *const () as usize,
            "VirtualFree" => title::virtual_free as *const () as usize,
            "VirtualProtect" => title::virtual_protect as *const () as usize,
            "VirtualQuery" => title::virtual_query as *const () as usize,
            "GetWriteWatch" => title::get_write_watch as *const () as usize,
            "WriteProcessMemory" => title::write_process_memory as *const () as usize,
            "NtQueryVirtualMemory" | "ZwQueryVirtualMemory" => {
                title::nt_query_virtual_memory as *const () as usize
            }
            "QueryWorkingSetEx" | "K32QueryWorkingSetEx" => {
                title::query_working_set_ex as *const () as usize
            }
            _ => return None,
        })
    }

    /// The functions [`title_import`] binds: the title's arguments, as given,
    /// to the drop-ins.
    pub mod title {
        use core::ffi::c_void;

        use windows_sys::Win32::Foundation::HANDLE;
        use windows_sys::Win32::System::Memory::MEMORY_BASIC_INFORMATION;
        use windows_sys::core::BOOL;

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn virtual_alloc(
            at: *const c_void,
            len: usize,
            kind: u32,
            protect: u32,
        ) -> *mut c_void {
            // SAFETY: the title's own arguments.
            unsafe { super::virtual_alloc(at, len, kind, protect) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn virtual_free(at: *mut c_void, len: usize, kind: u32) -> BOOL {
            // SAFETY: the title's own arguments.
            unsafe { super::virtual_free(at, len, kind) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn virtual_protect(
            at: *const c_void,
            len: usize,
            new: u32,
            old: *mut u32,
        ) -> BOOL {
            // SAFETY: the title's own arguments.
            unsafe { crate::virtual_protect(at, len, new, old) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn virtual_query(
            at: *const c_void,
            info: *mut MEMORY_BASIC_INFORMATION,
            len: usize,
        ) -> usize {
            // SAFETY: the title's own arguments.
            unsafe { crate::virtual_query(at, info, len) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn get_write_watch(
            flags: u32,
            at: *const c_void,
            len: usize,
            found: *mut *mut c_void,
            count: *mut usize,
            granularity: *mut u32,
        ) -> u32 {
            // SAFETY: the title's own arguments.
            unsafe { super::get_write_watch(flags, at, len, found, count, granularity) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn write_process_memory(
            process: HANDLE,
            at: *const c_void,
            from: *const c_void,
            len: usize,
            written: *mut usize,
        ) -> BOOL {
            // SAFETY: the title's own arguments.
            unsafe { super::write_process_memory(process, at, from, len, written) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn query_working_set_ex(
            process: HANDLE,
            info: *mut c_void,
            len: u32,
        ) -> BOOL {
            // SAFETY: the title's own arguments.
            unsafe { super::query_working_set_ex(process, info, len) }
        }

        /// # Safety
        /// As the system's call of that name: the title's arguments.
        pub unsafe extern "system" fn nt_query_virtual_memory(
            process: HANDLE,
            at: *const c_void,
            class: u32,
            out: *mut c_void,
            len: usize,
            returned: *mut usize,
        ) -> i32 {
            // SAFETY: the title's own arguments.
            unsafe { super::nt_query_virtual_memory(process, at, class, out, len, returned) }
        }
    }
}

#[cfg(test)]
mod tests;
