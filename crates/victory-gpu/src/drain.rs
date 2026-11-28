//! The card's work run out before the process ends.
//!
//! Why: the process used to end (`TerminateProcess`, or Wine's
//! `wineserver -k` from the scripts) while the card was still running lists
//! that read and write memory of ours. The kernel then takes that memory away
//! under the card: the card faults (`NVRM: Xid 31 ... MMU Fault`), and once,
//! on 2026-11-27, NVIDIA's driver (620.73.04, open module) never came back
//! from releasing the dead process's mappings (`nvidia_close ->
//! intermapDelDmaMapping -> btreeUnlink`, a soft lockup for 17 minutes): the
//! screen went black and the PC had to be reset. A process that ends with the
//! card idle gives the driver nothing in flight to clean up.
//!
//! Each queue of ours that runs the game's work is [`track`]ed when made;
//! [`drain`] signals a fence of its own on every one and waits for all of
//! them (with a deadline: a hung card must not keep the process alive).

use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use windows::Win32::Graphics::Direct3D12::*;
use windows::core::Interface;
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

/// The queues [`drain`] waits for.
static QUEUES: Mutex<Vec<ID3D12CommandQueue>> = Mutex::new(Vec::new());

/// Set by [`drain`]: the process is ending, nothing more goes to the card.
static ENDING: AtomicBool = AtomicBool::new(false);

/// `queue.ExecuteCommandLists(lists)`, unless the process is ending
/// ([`drain`] began): then the lists are dropped, so nothing new runs on the
/// card when the process goes (measured 2026-11-27: with only a wait, our
/// threads sent more work after it and the card faulted at the end anyway).
/// Signals sent after it still complete; what waits for them reads results
/// that are never used, since the process ends.
///
/// # Safety
/// As `ID3D12CommandQueue::ExecuteCommandLists`.
pub(crate) unsafe fn execute(queue: &ID3D12CommandQueue, lists: &[Option<ID3D12CommandList>]) {
    if ENDING.load(Ordering::SeqCst) {
        return;
    }
    // SAFETY: the caller's.
    unsafe { queue.ExecuteCommandLists(lists) };
}

/// Adds `queue` to those [`drain`] waits for (a queue of the game's work,
/// kept for the rest of the run).
pub(crate) fn track(queue: &ID3D12CommandQueue) {
    QUEUES.lock().unwrap_or_else(|e| e.into_inner()).push(queue.clone());
}

/// Waits until every [`track`]ed queue has run what was sent to it so far,
/// or `deadline` has passed. Returns whether all of them finished. From the
/// first call on, lists sent through [`execute`] are dropped; one already
/// past that check when it was called may still go out, which a second call
/// waits for.
pub fn drain(deadline: Duration) -> bool {
    ENDING.store(true, Ordering::SeqCst);
    let start = Instant::now();
    // Twice: a list already past `execute`'s check when the flag was set
    // reaches its queue after the first fence.
    let first = wait_once(deadline);
    let all = wait_once(deadline.saturating_sub(start.elapsed())) && first;
    tracing::info!(
        ms = start.elapsed().as_millis() as u64,
        idle = all,
        "the card's work run out before the process ends"
    );
    if all && release_wanted() {
        release_device(Duration::from_secs(3));
    }
    all
}

/// `VICTORY_END_RELEASE` (on unless `0`): [`release_device`] at the end.
fn release_wanted() -> bool {
    std::env::var("VICTORY_END_RELEASE").map_or(true, |v| v.trim() != "0")
}

/// Destroys the device of the tracked queues before the process ends, by
/// releasing every reference to it (ours, and those its objects hold, which
/// are never released: the process ends next). Why: ended with its device
/// alive, every run faulted the card at the end (`Xid 31`, the same address
/// run after run, also with every queue idle), the driver then tearing the
/// whole context down at once as the process's files close; that teardown
/// once hung the driver and the PC (2026-11-27). A device destroyed by
/// vkd3d waits for the card and frees its objects one by one, as a program
/// that closes cleanly does. Runs on a thread of its own, waited for until
/// `deadline`: whatever it does, the process ends after.
fn release_device(deadline: Duration) {
    let Some(queue) = QUEUES.lock().unwrap_or_else(|e| e.into_inner()).first().cloned() else {
        return;
    };
    // SAFETY: a raw pointer to a COM object, carried to the thread that
    // releases it; nothing of ours uses the device after (the process ends).
    let raw = unsafe {
        let mut device: Option<ID3D12Device> = None;
        if queue.GetDevice(&mut device).is_err() {
            return;
        }
        match device {
            Some(d) => d.into_raw() as usize,
            None => return,
        }
    };
    drop(queue);
    let start = Instant::now();
    let (tx, rx) = std::sync::mpsc::channel();
    let spawned = std::thread::Builder::new().name("end: device release".into()).spawn(move || {
        let raw = raw as *mut std::ffi::c_void;
        let mut count = 0u64;
        // SAFETY: IUnknown::Release on a live object until it is destroyed
        // (Release returns the references left).
        unsafe {
            let vtable = *(raw as *const *const windows::core::IUnknown_Vtbl);
            while count < 50_000_000 {
                count += 1;
                if ((*vtable).Release)(raw) == 0 {
                    break;
                }
            }
        }
        let _ = tx.send(count);
    });
    if spawned.is_err() {
        return;
    }
    match rx.recv_timeout(deadline) {
        Ok(count) => tracing::info!(references = count, ms = start.elapsed().as_millis() as u64, "the device destroyed before the process ends"),
        Err(_) => tracing::warn!(ms = deadline.as_millis() as u64, "the device was not destroyed in time; the process ends anyway"),
    }
}

/// Whether the process is ending ([`drain`] began): nothing more is sent
/// to the card, nor shown.
pub(crate) fn ending() -> bool {
    ENDING.load(Ordering::SeqCst)
}

/// One fence on every tracked queue, waited for until `deadline`.
fn wait_once(deadline: Duration) -> bool {
    let queues = QUEUES.lock().unwrap_or_else(|e| e.into_inner()).clone();
    if queues.is_empty() {
        return true;
    }
    let start = Instant::now();
    let mut all = true;
    // SAFETY: plain D3D12 and event calls on live objects of ours.
    unsafe {
        let Ok(event) = CreateEventW(None, false, false, None) else {
            return false;
        };
        for queue in &queues {
            let done = (|| -> windows::core::Result<bool> {
                let device: ID3D12Device = {
                    let mut device: Option<ID3D12Device> = None;
                    queue.GetDevice(&mut device)?;
                    device.ok_or_else(windows::core::Error::empty)?
                };
                let fence: ID3D12Fence = device.CreateFence(0, D3D12_FENCE_FLAG_NONE)?;
                queue.Signal(&fence, 1)?;
                if fence.GetCompletedValue() >= 1 {
                    return Ok(true);
                }
                fence.SetEventOnCompletion(1, event)?;
                let left = deadline.saturating_sub(start.elapsed());
                WaitForSingleObject(event, left.as_millis().min(u32::MAX as u128) as u32);
                Ok(fence.GetCompletedValue() >= 1)
            })();
            // A queue whose device is removed has nothing left to run.
            all &= done.unwrap_or(true);
        }
        let _ = windows::Win32::Foundation::CloseHandle(event);
    }
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;

    /// A tracked queue with a list in flight is waited for, well within the
    /// deadline.
    #[test]
    fn drains_a_tracked_queue() {
        // SAFETY: plain D3D12 object creation and an empty copy list.
        unsafe {
            let mut device: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_12_0, &mut device).is_err() {
                eprintln!("no D3D12 device here: skipped");
                return;
            }
            let device = device.unwrap();
            let queue: ID3D12CommandQueue = device
                .CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_COPY, ..Default::default() })
                .unwrap();
            let allocator: ID3D12CommandAllocator = device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY).unwrap();
            let list: ID3D12GraphicsCommandList =
                device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, &allocator, None).unwrap();
            list.Close().unwrap();
            track(&queue);
            queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            // `wait_once`, not `drain`: `drain` ends sending for the whole
            // test process, and other tests share it.
            let start = Instant::now();
            assert!(wait_once(Duration::from_secs(5)));
            assert!(start.elapsed() < Duration::from_secs(5));
            // Again with nothing sent since: at once.
            assert!(wait_once(Duration::from_secs(5)));
        }
    }
}
