//! The PE side's marshalling against a fake Unix half (no Wine, no DLL):
//! batches, refusals, and the map kept in step. The real Unix half has its
//! own tests (WS1); the real DLL is tested under Proton (WS2).

use std::cell::RefCell;

use super::*;

/// What the fake Unix half saw, per test thread.
#[derive(Default)]
struct Seen {
    calls: Vec<(u32, u64)>,
    /// Ops at these addresses fail.
    fail_at: Vec<u64>,
    /// Pages `WATCH_TAKE` reports written.
    written: Vec<u64>,
    version: u32,
    features: u64,
}

thread_local! {
    static SEEN: RefCell<Seen> = RefCell::new(Seen {
        version: abi::VERSION,
        features: abi::FEATURE_WATCH | abi::FEATURE_COPY,
        ..Seen::default()
    });
}

unsafe extern "system" fn fake(code: u32, args: *mut core::ffi::c_void) -> i32 {
    SEEN.with_borrow_mut(|seen| {
        // SAFETY: each arm reads the struct the ABI names for the code.
        unsafe {
            match code {
                abi::VERSION_CALL => {
                    let v = &mut *(args as *mut abi::Version);
                    (v.version, v.features) = (seen.version, seen.features);
                    seen.calls.push((code, 0));
                }
                abi::POOL_CREATE => {
                    (*(args as *mut abi::PoolCreate)).pool = 42;
                    seen.calls.push((code, 0));
                }
                abi::MAP | abi::UNMAP | abi::PROTECT | abi::WATCH_REGISTER => {
                    let b = &mut *(args as *mut abi::Batch);
                    assert!(b.count <= abi::BATCH_MAX);
                    let results =
                        std::slice::from_raw_parts_mut(b.results as *mut i32, b.count as usize);
                    for (i, r) in results.iter_mut().enumerate() {
                        let addr = if code == abi::MAP {
                            (*(b.ops as *const abi::MapOp).add(i)).addr
                        } else {
                            (*(b.ops as *const abi::Range).add(i)).addr
                        };
                        *r = if seen.fail_at.contains(&addr) { 12 } else { 0 };
                    }
                    b.done = results.iter().filter(|&&r| r == 0).count() as u64;
                    seen.calls.push((code, b.count));
                }
                abi::WATCH_TAKE => {
                    let t = &mut *(args as *mut abi::WatchTake);
                    let there: Vec<u64> = seen
                        .written
                        .iter()
                        .copied()
                        .filter(|&p| p >= t.addr && p < t.addr + t.len)
                        .collect();
                    let n = there.len().min(t.capacity as usize);
                    std::ptr::copy_nonoverlapping(there.as_ptr(), t.pages as *mut u64, n);
                    (t.count, t.more) = (n as u64, (there.len() > n) as u64);
                    seen.written.retain(|p| !there[..n].contains(p));
                    seen.calls.push((code, n as u64));
                }
                _ => {
                    seen.calls.push((code, 0));
                }
            }
        }
    });
    0
}

const W: usize = 0x2000_0000;
const K64: usize = 0x1_0000;

fn bridge() -> &'static Bridge {
    let b = Box::leak(Box::new(Bridge::new(fake).expect("fake speaks our ABI")));
    assert!(b.window_add(W, 64 * K64));
    b
}

fn calls(code: u32) -> Vec<u64> {
    SEEN.with_borrow(|s| {
        s.calls
            .iter()
            .filter(|c| c.0 == code)
            .map(|c| c.1)
            .collect()
    })
}

#[test]
fn the_switch_is_off_unless_set_to_something_else_than_0() {
    assert!(!switch(None));
    assert!(!switch(Some("")));
    assert!(!switch(Some("0")));
    assert!(switch(Some("1")));
}

#[test]
fn protections_convert_both_ways() {
    for page in [0x01, 0x02, 0x04, 0x10, 0x20, 0x40] {
        assert_eq!(Prot::from_page(page).unwrap().to_page(), page);
    }
    assert_eq!(Prot::from_page(0x104), None); // PAGE_READWRITE | PAGE_GUARD
    assert_eq!(Prot(abi::PROT_WRITE).to_page(), 0x04);
}

/// With no DLL (unless test.sh put one beside the test) everything says "not
/// mine", whatever VICTORY_BRIDGE is: the present path stays in charge.
#[test]
fn without_the_dll_nothing_is_bridged() {
    // With VICTORY_BRIDGE=1 and the DLL beside the test (test.sh,
    // FORK_BRIDGE_DIR) the bridge is real: nothing to check here then.
    if available() {
        return;
    }
    assert!(!bridged(W));
    assert!(Pool::create(1 << 20).is_none());
    assert!(!window_add(W, K64));
    assert_eq!(window_remove(W), None);
    assert!(query(W).is_none());
    assert!(!protect(&[(W, K64, Prot::READ)]));
    assert_eq!(unmap(&[(W, K64)]), vec![false]);
    assert!(!watch_register(W, K64));
    let mut out = vec![];
    assert!(!watch_take(W, K64, &mut out) && out.is_empty());
    // SAFETY: nothing is copied without the bridge.
    assert_eq!(
        unsafe { copy(std::ptr::null_mut(), std::ptr::null(), 0) },
        None
    );
}

#[test]
fn another_abi_version_is_refused() {
    SEEN.with_borrow_mut(|s| s.version = abi::VERSION + 1);
    assert!(Bridge::new(fake).is_none());
}

#[test]
fn views_map_in_batches_and_the_map_follows() {
    let b = bridge();
    let pool = b.pool(64 * K64 as u64).unwrap();
    assert_eq!(pool.id(), 42);
    let ops: Vec<MapOp> = (0..5000)
        .map(|i| MapOp {
            watch: false,
            addr: W + (i % 64) * K64,
            len: K64,
            offset: ((i % 64) * K64) as u64,
            prot: Prot::READ_WRITE,
        })
        .collect();
    let done = b.map(&pool, &ops);
    assert!(done.iter().all(|&d| d));
    assert_eq!(calls(abi::MAP), vec![4096, 904]);
    let r = b.query(W + 3 * K64 + 5).unwrap();
    assert_eq!(
        r.kind,
        Kind::View {
            pool: 42,
            offset: 3 * K64 as u64
        }
    );
    assert_eq!(r.len, 61 * K64); // consecutive pool bytes: one run
    assert!(b.bridged(W) && !b.bridged(W + 64 * K64) && !b.bridged(W - 1));
    std::mem::forget(pool);
}

#[test]
fn ops_outside_windows_or_pools_are_refused_without_a_call() {
    let b = bridge();
    let pool = b.pool(2 * K64 as u64).unwrap();
    let ops = [
        MapOp {
            watch: false,
            addr: W + 64 * K64,
            len: K64,
            offset: 0,
            prot: Prot::READ_WRITE,
        }, // past the window
        MapOp {
            watch: false,
            addr: W,
            len: K64,
            offset: K64 as u64 * 2,
            prot: Prot::READ_WRITE,
        }, // past the pool
        MapOp {
            watch: false,
            addr: W,
            len: K64,
            offset: 0,
            prot: Prot::READ_WRITE,
        },
    ];
    assert_eq!(b.map(&pool, &ops), vec![false, false, true]);
    assert_eq!(calls(abi::MAP), vec![1]);
    std::mem::forget(pool);
}

#[test]
fn a_failed_op_leaves_its_range_unmapped() {
    let b = bridge();
    let pool = b.pool(64 * K64 as u64).unwrap();
    // A view there first: a failed MAP_FIXED over it may have dropped it.
    let first = MapOp {
        watch: false,
        addr: W + K64,
        len: K64,
        offset: 9 * K64 as u64,
        prot: Prot::READ,
    };
    assert_eq!(b.map(&pool, &[first]), vec![true]);
    SEEN.with_borrow_mut(|s| s.fail_at.push((W + K64) as u64));
    let ops = [
        MapOp {
            watch: false,
            addr: W,
            len: K64,
            offset: 0,
            prot: Prot::READ_WRITE,
        },
        MapOp {
            watch: false,
            addr: W + K64,
            len: K64,
            offset: K64 as u64,
            prot: Prot::READ_WRITE,
        },
    ];
    assert_eq!(b.map(&pool, &ops), vec![true, false]);
    assert_eq!(
        b.query(W).unwrap().kind,
        Kind::View {
            pool: 42,
            offset: 0
        }
    );
    let r = b.query(W + K64).unwrap();
    assert_eq!((r.kind, r.prot), (Kind::Unmapped, Prot::NONE));
    std::mem::forget(pool);
}

#[test]
fn protect_and_unmap_change_the_map() {
    let b = bridge();
    let pool = b.pool(64 * K64 as u64).unwrap();
    let ops = [MapOp {
        watch: false,
        addr: W,
        len: 4 * K64,
        offset: 0,
        prot: Prot::READ_WRITE,
    }];
    assert_eq!(b.map(&pool, &ops), vec![true]);
    assert!(!b.protect(&[(W, 5 * K64, Prot::READ)])); // reaches unmapped pages
    assert!(calls(abi::PROTECT).is_empty());
    assert!(b.protect(&[(W + K64, K64, Prot::READ)]));
    assert_eq!(b.query(W + K64).unwrap().prot, Prot::READ);
    assert_eq!(
        b.unmap(&[(W, 2 * K64), (W + 64 * K64, K64)]),
        vec![true, false]
    );
    let r = b.query(W).unwrap();
    assert_eq!(
        (r.kind, r.prot, r.len),
        (Kind::Unmapped, Prot::NONE, 2 * K64)
    );
    assert_eq!(
        b.query(W + 2 * K64).unwrap().kind,
        Kind::View {
            pool: 42,
            offset: 2 * K64 as u64
        }
    );
    std::mem::forget(pool);
}

#[test]
fn windows_come_and_go() {
    let b = bridge();
    assert!(!b.window_add(W + K64, K64)); // overlaps
    assert_eq!(b.window_remove(W + K64), None); // not a window's start
    assert_eq!(b.window_remove(W), Some(64 * K64));
    assert!(!b.bridged(W));
    assert!(b.query(W).is_none());
}

#[test]
fn watch_take_pages_through_a_full_buffer() {
    let b = bridge();
    let pool = b.pool(64 * K64 as u64).unwrap();
    assert_eq!(
        b.map(
            &pool,
            &[MapOp {
                watch: false,
                addr: W,
                len: 64 * K64,
                offset: 0,
                prot: Prot::READ_WRITE
            }]
        ),
        vec![true]
    );
    assert!(b.watch_register(W, 64 * K64));
    assert!(!b.watch_register(W + 64 * K64, K64));
    let written: Vec<u64> = (0..1000).map(|i| (W + i * 0x1000) as u64).collect();
    SEEN.with_borrow_mut(|s| s.written = written.clone());
    let mut out = vec![];
    assert!(b.watch_take(W, 64 * K64, &mut out));
    assert_eq!(out, written.iter().map(|&p| p as usize).collect::<Vec<_>>());
    assert_eq!(calls(abi::WATCH_TAKE), vec![512, 488]);
    out.clear();
    assert!(b.watch_take(W, 64 * K64, &mut out) && out.is_empty());
    std::mem::forget(pool);
}

#[test]
fn features_the_unix_half_lacks_are_not_used() {
    SEEN.with_borrow_mut(|s| s.features = 0);
    let b = bridge();
    let pool = b.pool(64 * K64 as u64).unwrap();
    assert_eq!(
        b.map(
            &pool,
            &[MapOp {
                watch: false,
                addr: W,
                len: K64,
                offset: 0,
                prot: Prot::READ_WRITE
            }]
        ),
        vec![true]
    );
    assert!(!b.watch_register(W, K64));
    assert!(!b.watch_take(W, K64, &mut vec![]));
    assert_eq!(b.copy(0, 0, 0), None);
    std::mem::forget(pool);
}

/// Outside windows the drop-ins are Wine's (or Windows') own calls.
#[cfg(windows)]
#[test]
fn drop_ins_pass_other_memory_through() {
    use windows_sys::Win32::System::Memory::{
        MEM_COMMIT, MEM_RELEASE, MEMORY_BASIC_INFORMATION, PAGE_READONLY, PAGE_READWRITE,
        VirtualAlloc, VirtualFree, VirtualQuery,
    };
    // SAFETY: our own allocation, queried and freed.
    unsafe {
        let p = VirtualAlloc(
            std::ptr::null(),
            4 * K64,
            MEM_COMMIT | 0x2000,
            PAGE_READWRITE,
        );
        assert!(!p.is_null());
        let mut old = 0;
        assert_ne!(virtual_protect(p, K64, PAGE_READONLY, &mut old), 0);
        assert_eq!(old, PAGE_READWRITE);
        let size = size_of::<MEMORY_BASIC_INFORMATION>();
        let (mut a, mut b) = (
            MEMORY_BASIC_INFORMATION::default(),
            MEMORY_BASIC_INFORMATION::default(),
        );
        assert_eq!(
            virtual_query(p, &mut a, size),
            VirtualQuery(p, &mut b, size)
        );
        assert_eq!(
            (a.BaseAddress, a.RegionSize, a.Protect),
            (b.BaseAddress, b.RegionSize, PAGE_READONLY)
        );
        assert_ne!(VirtualFree(p, 0, MEM_RELEASE), 0);
    }
}
