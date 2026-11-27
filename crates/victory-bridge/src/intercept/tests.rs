//! The drop-ins against a fake Unix half (the bridged branch, no DLL) and,
//! under Windows or Wine, against the system (everything else must be the
//! system's own answer).

use super::*;
use crate::{MapOp, PagePool, Prot};

/// A Unix half that does whatever it is asked.
unsafe extern "system" fn fake(code: u32, args: *mut core::ffi::c_void) -> i32 {
    // SAFETY: each arm reads the struct the ABI names for the code.
    unsafe {
        match code {
            abi::VERSION_CALL => {
                let v = &mut *(args as *mut abi::Version);
                (v.version, v.features) = (abi::VERSION, abi::FEATURE_WATCH | abi::FEATURE_COPY);
            }
            abi::POOL_CREATE => (*(args as *mut abi::PoolCreate)).pool = 7,
            abi::MAP | abi::UNMAP | abi::PROTECT | abi::WATCH_REGISTER => {
                let b = &mut *(args as *mut abi::Batch);
                if b.results != 0 {
                    std::slice::from_raw_parts_mut(b.results as *mut i32, b.count as usize).fill(0);
                }
                b.done = b.count;
            }
            _ => {}
        }
    }
    0
}

const K64: usize = 0x1_0000;

/// A bridge with one window of 16 blocks at `w`, whose blocks 2-5 are a
/// read-write view and block 3 read-only.
fn bridge_at(w: usize) -> &'static Bridge {
    let b: &'static Bridge = Box::leak(Box::new(Bridge::new(fake).expect("fake speaks our ABI")));
    assert!(b.window_add(w, 16 * K64));
    let pool = Box::leak(Box::new(b.pool(16 * K64 as u64).unwrap()));
    let op = MapOp {
        addr: w + 2 * K64,
        len: 4 * K64,
        offset: 0,
        prot: Prot::READ_WRITE,
        watch: false,
    };
    assert_eq!(pool.map(&[op]), vec![true]);
    assert!(b.protect(&[(w + 3 * K64, K64, Prot::READ)]));
    b
}

const W: usize = 0x3000_0000;

#[test]
fn ranges_meet_a_window_when_any_page_lies_in_it() {
    let b = bridge_at(W);
    assert!(b.meets(W, 1));
    assert!(b.meets(W + 16 * K64 - 1, 0));
    assert!(b.meets(W - 4096, 4097)); // ends in its first page
    assert!(b.meets(W - K64, 100 * K64)); // holds it whole
    assert!(!b.meets(W - K64, K64));
    assert!(!b.meets(W + 16 * K64, K64));
    assert!(!b.meets(usize::MAX - 10, 100)); // no overflow
    // No bridge: nothing is a window's.
    assert!(!meets(W, K64));
}

#[test]
fn commits_and_frees_in_a_window_are_refused_and_the_rest_is_wines() {
    let b = Some(bridge_at(W));
    const RESERVE: u32 = 0x2000;
    assert_eq!(
        alloc_verdict(b, W + 2 * K64, K64, MEM_COMMIT),
        Verdict::Refuse(REFUSED_COMMIT)
    );
    assert_eq!(
        alloc_verdict(b, W - K64, 2 * K64, MEM_COMMIT | RESERVE),
        Verdict::Refuse(REFUSED_COMMIT)
    );
    assert_eq!(
        alloc_verdict(b, W, K64, MEM_RESET),
        Verdict::Refuse(REFUSED_COMMIT)
    );
    // A reservation there Wine refuses itself; a new allocation is never
    // placed in a window.
    assert_eq!(alloc_verdict(b, W, K64, RESERVE), Verdict::Wine);
    assert_eq!(
        alloc_verdict(b, 0, K64, MEM_COMMIT | RESERVE),
        Verdict::Wine
    );
    assert_eq!(
        alloc_verdict(b, W + 16 * K64, K64, MEM_COMMIT),
        Verdict::Wine
    );
    // Decommit, release (size 0: from the address on), split: all refused.
    assert_eq!(
        free_verdict(b, W + 2 * K64, K64),
        Verdict::Refuse(REFUSED_FREE)
    );
    assert_eq!(free_verdict(b, W, 0), Verdict::Refuse(REFUSED_FREE));
    assert_eq!(free_verdict(b, W + 16 * K64, 0), Verdict::Wine);
    assert_eq!(
        watch_verdict(b, W - K64, 2 * K64),
        Verdict::Refuse(REFUSED_WATCH)
    );
    assert_eq!(watch_verdict(b, W - K64, K64), Verdict::Wine);
    // Without the bridge every call is Wine's.
    assert_eq!(alloc_verdict(None, W, K64, MEM_COMMIT), Verdict::Wine);
    assert_eq!(free_verdict(None, W, 0), Verdict::Wine);
    assert_eq!(watch_verdict(None, W, K64), Verdict::Wine);
}

#[test]
fn a_window_reads_as_one_placeholder_reservation_committed_as_its_views() {
    let b = bridge_at(W);
    let r = b.query(W + 2 * K64 + 5).unwrap();
    let info = region_info(b, &r);
    assert_eq!(
        info,
        RegionInfo {
            allocation_base: W,
            allocation_protect: 0x01,
            region_type: REGION_PRIVATE | REGION_PLACEHOLDER,
            region_size: 16 * K64,
            commit_size: 4 * K64,
            partition_id: 0,
            node_preference: 0,
        }
    );
    // Working set: a view's page is valid and shared, with its protection.
    assert_eq!(working_set_bits(&r), 1 | 1 << 1 | 0x04 << 4 | 1 << 15);
    let ro = b.query(W + 3 * K64).unwrap();
    assert_eq!(working_set_bits(&ro) >> 4 & 0x7ff, 0x02);
    assert_eq!(working_set_bits(&b.query(W).unwrap()), 0);
}

#[cfg(windows)]
mod system {
    use core::ffi::c_void;

    use windows_sys::Win32::Foundation::{GetLastError, HANDLE};
    use windows_sys::Win32::System::Memory::{
        MEM_COMMIT, MEM_DECOMMIT, MEM_MAPPED, MEM_RELEASE, MEM_RESERVE, MEMORY_BASIC_INFORMATION,
        PAGE_NOACCESS, PAGE_READWRITE, VirtualAlloc, VirtualFree, VirtualQuery,
    };
    use windows_sys::Win32::System::Threading::GetCurrentProcess;

    use super::super::*;
    use super::{K64, bridge_at};

    const MEM_WRITE_WATCH: u32 = 0x20_0000;

    unsafe fn nt_query(
        b: Option<&Bridge>,
        at: usize,
        class: u32,
        out: *mut c_void,
        len: usize,
    ) -> (i32, usize) {
        let mut returned = 0;
        // SAFETY: the caller's buffer.
        let status = unsafe {
            nt_query_virtual_memory_on(
                b,
                GetCurrentProcess(),
                at as *const _,
                class,
                out,
                len,
                &mut returned,
            )
        };
        (status, returned)
    }

    /// Memory of Wine's own: every drop-in gives Wine's answer, with the
    /// bridge off and with a bridge whose windows lie elsewhere.
    #[test]
    fn outside_windows_the_drop_ins_are_the_systems_calls() {
        let elsewhere = bridge_at(0x6000_0000_0000);
        for b in [None, Some(elsewhere)] {
            // SAFETY: our own memory, allocated, asked about, written and freed.
            unsafe {
                let p = virtual_alloc_on(
                    b,
                    std::ptr::null(),
                    4 * K64,
                    MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                    PAGE_READWRITE,
                );
                assert!(!p.is_null());
                let size = size_of::<MEMORY_BASIC_INFORMATION>();
                let (mut ours, mut wine) = (
                    MEMORY_BASIC_INFORMATION::default(),
                    MEMORY_BASIC_INFORMATION::default(),
                );
                let (status, returned) = nt_query(
                    b,
                    p as usize,
                    MEMORY_BASIC_INFORMATION_CLASS,
                    (&raw mut ours).cast(),
                    size,
                );
                assert_eq!((status, returned), (0, size));
                assert_eq!(VirtualQuery(p, &mut wine, size), size);
                assert_eq!(
                    (
                        ours.BaseAddress,
                        ours.AllocationBase,
                        ours.RegionSize,
                        ours.State,
                        ours.Protect,
                        ours.Type
                    ),
                    (
                        wine.BaseAddress,
                        wine.AllocationBase,
                        wine.RegionSize,
                        wine.State,
                        wine.Protect,
                        wine.Type
                    )
                );
                // Written through the drop-in, seen by the write watch's drop-in.
                let bytes = [7u8; 16];
                let mut done = 0;
                assert_ne!(
                    write_process_memory_on(
                        b,
                        GetCurrentProcess(),
                        p,
                        bytes.as_ptr().cast(),
                        16,
                        &mut done
                    ),
                    0
                );
                assert_eq!((done, *p.cast::<u8>().add(15)), (16, 7));
                let mut found = [std::ptr::null_mut::<c_void>(); 64];
                let (mut count, mut granularity) = (found.len(), 0u32);
                assert_eq!(
                    get_write_watch_on(
                        b,
                        0,
                        p,
                        4 * K64,
                        found.as_mut_ptr(),
                        &mut count,
                        &mut granularity
                    ),
                    0
                );
                assert_eq!((count, found[0]), (1, p));
                assert_ne!(
                    virtual_free_on(b, p.cast::<u8>().add(K64).cast(), K64, MEM_DECOMMIT),
                    0
                );
                assert_ne!(virtual_free_on(b, p, 0, MEM_RELEASE), 0);
                // What is free stays Wine's to refuse.
                assert_eq!(virtual_free_on(b, p, 0, MEM_RELEASE), 0);
            }
        }
    }

    /// In a window (the fake's, at addresses that are free to Wine): every
    /// question is answered from the map and nothing that would change
    /// pages reaches Wine.
    #[test]
    fn in_a_window_the_bridge_answers_and_refuses() {
        // A free range of Wine's, so that a call that wrongly reached Wine
        // would be told something else.
        // SAFETY: reserved and released at once: free afterwards.
        let w = unsafe {
            let p = VirtualAlloc(std::ptr::null(), 32 * K64, MEM_RESERVE, PAGE_NOACCESS);
            assert!(!p.is_null());
            assert_ne!(VirtualFree(p, 0, MEM_RELEASE), 0);
            p as usize
        };
        let b = Some(bridge_at(w));
        // SAFETY: refused calls touch nothing; answers go to our locals.
        unsafe {
            assert!(
                virtual_alloc_on(
                    b,
                    (w + 2 * K64) as *const _,
                    K64,
                    MEM_COMMIT,
                    PAGE_READWRITE
                )
                .is_null()
            );
            assert_eq!(GetLastError(), REFUSED_COMMIT);
            assert_eq!(
                virtual_free_on(b, (w + 2 * K64) as *mut _, K64, MEM_DECOMMIT),
                0
            );
            assert_eq!(GetLastError(), REFUSED_FREE);
            assert_eq!(virtual_free_on(b, w as *mut _, 0, MEM_RELEASE), 0);
            let (mut count, mut granularity) = (0usize, 0u32);
            assert_eq!(
                get_write_watch_on(
                    b,
                    0,
                    w as *const _,
                    K64,
                    std::ptr::null_mut(),
                    &mut count,
                    &mut granularity
                ),
                u32::MAX
            );
            assert_eq!(GetLastError(), REFUSED_WATCH);

            let mut info = MEMORY_BASIC_INFORMATION::default();
            let size = size_of::<MEMORY_BASIC_INFORMATION>();
            let (status, _) = nt_query(
                b,
                w + 3 * K64 + 9,
                MEMORY_BASIC_INFORMATION_CLASS,
                (&raw mut info).cast(),
                size,
            );
            assert_eq!(status, 0);
            assert_eq!(
                (
                    info.BaseAddress as usize,
                    info.AllocationBase as usize,
                    info.RegionSize,
                    info.State,
                    info.Type,
                    info.Protect
                ),
                (w + 3 * K64, w, K64, MEM_COMMIT, MEM_MAPPED, 0x02)
            );
            let (status, returned) = nt_query(
                b,
                w,
                MEMORY_BASIC_INFORMATION_CLASS,
                (&raw mut info).cast(),
                size - 1,
            );
            assert_eq!((status as u32, returned), (0xC000_0004, size));

            let mut region = RegionInfo::default();
            let (status, _) = nt_query(
                b,
                w + 9 * K64,
                MEMORY_REGION_INFORMATION_EX,
                (&raw mut region).cast(),
                size_of::<RegionInfo>(),
            );
            assert_eq!(
                (status, region.allocation_base, region.commit_size),
                (0, w, 4 * K64)
            );
            assert_ne!(region.region_type & REGION_PLACEHOLDER, 0);

            // Working set: Wine answers its own entries, the window's are ours.
            let own = Box::new(0u64);
            let mut entries = [(w + 2 * K64, 0usize), (w, 0), (&raw const *own as usize, 0)];
            let (status, _) = nt_query(
                b,
                0,
                MEMORY_WORKING_SET_EX_INFORMATION,
                entries.as_mut_ptr().cast(),
                size_of_val(&entries),
            );
            assert_eq!(status, 0);
            assert_eq!(entries[0].1 & 1, 1);
            assert_eq!(entries[1].1, 0);
            assert_eq!(entries[2].1 & 1, 1, "Wine's own entry, a valid page");
            let mut again = [
                (w + 3 * K64, 0usize),
                (w + 9 * K64, 0),
                (&raw const *own as usize, 0),
            ];
            assert_ne!(
                query_working_set_ex_on(
                    b,
                    GetCurrentProcess(),
                    again.as_mut_ptr().cast(),
                    size_of_val(&again) as u32
                ),
                0
            );
            assert_eq!(
                (
                    again[0].1 & 1,
                    again[0].1 >> 4 & 0x7ff,
                    again[1].1,
                    again[2].1 & 1
                ),
                (1, 0x02, 0, 1)
            );
        }
    }

    /// Another process's memory is never answered here.
    #[test]
    fn other_processes_are_wines() {
        let b = bridge_at(0x6100_0000_0000);
        let mut info = MEMORY_BASIC_INFORMATION::default();
        let size = size_of::<MEMORY_BASIC_INFORMATION>();
        let none: HANDLE = std::ptr::null_mut();
        // SAFETY: a call that fails on the null handle.
        let status = unsafe {
            nt_query_virtual_memory_on(
                Some(b),
                none,
                0x6100_0000_0000 as *const _,
                MEMORY_BASIC_INFORMATION_CLASS,
                (&raw mut info).cast(),
                size,
                std::ptr::null_mut(),
            )
        };
        assert!(status < 0);
    }

    #[test]
    fn title_imports_stay_the_hosts_with_the_bridge_off() {
        // With VICTORY_BRIDGE=1 and the DLL beside the test the bridge is on.
        if crate::available() {
            return;
        }
        for name in [
            "VirtualAlloc",
            "VirtualFree",
            "VirtualQuery",
            "NtQueryVirtualMemory",
            "Sleep",
        ] {
            assert_eq!(title_import(name), None);
        }
    }
}
