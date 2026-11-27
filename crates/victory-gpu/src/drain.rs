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
use std::time::{Duration, Instant};

use windows::Win32::Graphics::Direct3D12::*;
#[cfg(test)]
use windows::core::Interface;
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

/// The queues [`drain`] waits for.
static QUEUES: Mutex<Vec<ID3D12CommandQueue>> = Mutex::new(Vec::new());

/// Adds `queue` to those [`drain`] waits for (a queue of the game's work,
/// kept for the rest of the run).
pub(crate) fn track(queue: &ID3D12CommandQueue) {
    QUEUES.lock().unwrap_or_else(|e| e.into_inner()).push(queue.clone());
}

/// Waits until every [`track`]ed queue has run what was sent to it so far,
/// or `deadline` has passed. Returns whether all of them finished. Lists sent
/// meanwhile by other threads are not waited for: call it when nothing sends
/// any more (the title suspended), or twice.
pub fn drain(deadline: Duration) -> bool {
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
    tracing::info!(
        queues = queues.len(),
        ms = start.elapsed().as_millis() as u64,
        idle = all,
        "the card's work run out before the process ends"
    );
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;

    /// A tracked queue with a list in flight is drained, well within the
    /// deadline; a removed or idle queue never holds the end.
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
            let start = Instant::now();
            assert!(drain(Duration::from_secs(5)));
            assert!(start.elapsed() < Duration::from_secs(5));
            // Again with nothing sent since: at once.
            assert!(drain(Duration::from_secs(5)));
        }
    }
}
