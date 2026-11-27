//! Every frame's length, and what the long ones were made of
//! (`VICTORY_FRAME_LOG=1`; off by default in every build).
//!
//! The frame-rate line averages over a second or more, and its counters are
//! reset at each line: a single long frame is lost in it, and so is what that
//! frame was made of.
//!
//! Here every counter runs from the start and is never reset. At the end of
//! each present call the totals are read, and their differences since the
//! present before are that frame's. Every five seconds one line gives the
//! frame times (each one, the shortest, the median, the longest), one gives
//! the usual frame's make-up (the mean of the frames that were not long), and
//! each long frame gets a line of its own in the same form, so a long frame is
//! read against a usual one column by column (`scripts/linux/ab_runs.py`
//! reads them).
//!
//! The counters themselves are plain relaxed atomic adds in every build, with
//! the switch on or off; only the reading at each present and the lines need
//! the switch. Ported from the Flamingo-based branch (`linux-on-new-upstream`)
//! to the foxhound base: the columns are the ones this base has the code for.

use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering, Ordering::Relaxed};

/// Totals since the start (microseconds unless said otherwise).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(usize)]
pub enum T {
    /// The title's present call, all of it (`queue::present`); of it, our
    /// copy to the window with the host's present (`queue::show_planes`).
    PresentUs,
    PresentShowUs,
    /// The title's submit calls (`queue::execute`), on every thread.
    ExecuteUs,
    /// Submissions prepared; preparing them and waiting for the card, on
    /// whichever thread (`host::SUBMISSION_US`, the frame-rate line's
    /// "preparing" and "on the GPU").
    Submissions,
    PrepareUs,
    CardWaitUs,
    /// The card with nothing of ours queued, and how much of that while the
    /// submitting thread was working (`host::CARD_CLOCK_US`).
    CardIdleUs,
    CardIdleBehindUs,
    /// Copies of title memory for submissions (time, bytes), and what
    /// shaders wrote copied back (time, bytes) (`host::PREPARING`,
    /// `host::COPY_BACK`).
    CopyUs,
    CopyBytes,
    CopyBackUs,
    CopyBackBytes,
    /// Texture refreshes on every thread (`texture::REFRESH_WAITS`): in all,
    /// and of that the wait for a texture another thread is refreshing, the
    /// write watch and the guards asked, and the wait for title memory to
    /// hold still (the runtime remapping it); uploads recorded.
    RefreshUs,
    RefreshOtherUs,
    RefreshAskUs,
    RefreshStillUs,
    Uploads,
    /// Waiting for host pipelines being made (`pipeline::make::wait`).
    PipelineWaitUs,
    /// The runtime changing what title memory shows
    /// (`watch::title_memory_remapping`): times, the time from asking for the
    /// lock to letting it go, and of that the wait for readers.
    Remaps,
    RemapUs,
    RemapWaitUs,
    /// xmem's mapping calls (the title's `XMemMapPhysicalPages`, the page
    /// mappings copied for the GPU, scatter), calls and nanoseconds; the
    /// Windows calls xmem makes to change a mapping (unmap, split, map,
    /// commit), calls and nanoseconds; and the GPU side told of changes
    /// (`title_memory_remapped`, `_view`, `_unmapping`, `_made_real`), calls
    /// and nanoseconds; the memory bridge's calls xmem makes instead of the
    /// Windows calls (`VICTORY_BRIDGE`; a batch of views each), calls and
    /// nanoseconds. Summed over threads.
    XmemCalls,
    XmemNs,
    RemapOsCalls,
    RemapOsNs,
    RemapBridgeCalls,
    RemapBridgeNs,
    RemapNotes,
    RemapNoteNs,
    /// The memory watch's system calls (`crate::watch`), on every thread:
    /// write-watch askings (and of them the refused ones, and the bytes asked
    /// about), protection changes, and region queries, each with its
    /// nanoseconds.
    WatchAsks,
    WatchRefused,
    WatchBytes,
    WatchNs,
    ProtectCalls,
    ProtectNs,
    QueryCalls,
    QueryNs,
    /// Waits for the guards' lock that found it held (and the faults of the
    /// title's among them), and their nanoseconds; waits for the watch's
    /// other locks (ranges being asked about, the journal), nanoseconds.
    GuardWaits,
    GuardWaitNs,
    WatchLockWaitNs,
    /// Writes to guarded view memory opened by the fault handler.
    GuardFaults,
    /// The title's frame waits (`frame::wait`): time slept, on every thread.
    PaceUs,
    /// DirectStorage requests finished, their destination bytes, and the
    /// workers' time in them.
    DiskReads,
    DiskBytes,
    DiskUs,
    /// The memory watch's costs above taken on the frame's own threads
    /// alone: those that prepared a submission, presented or submitted
    /// ([`add`] marks them). What the frame waits for directly, against the
    /// totals summed over every thread.
    CritWatchNs,
    CritProtectNs,
    CritQueryNs,
    CritGuardWaitNs,
    CritLockWaitNs,
    CritGuardFaults,
    Count,
}

const N: usize = T::Count as usize;
static TOTAL: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];

thread_local! {
    /// This thread is one of the frame's own ([`T::CritWatchNs`]).
    static CRITICAL: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// The frame's-threads twin of a memory-watch total.
fn critical_twin(t: T) -> Option<T> {
    Some(match t {
        T::WatchNs => T::CritWatchNs,
        T::ProtectNs => T::CritProtectNs,
        T::QueryNs => T::CritQueryNs,
        T::GuardWaitNs => T::CritGuardWaitNs,
        T::WatchLockWaitNs => T::CritLockWaitNs,
        T::GuardFaults => T::CritGuardFaults,
        _ => return None,
    })
}

/// Adds to a total. Adding to preparing, presenting or submitting marks the
/// thread as one of the frame's own; the memory watch's costs on such a
/// thread go to their twins too. (A const thread-local: no allocation, safe
/// in the fault handler.)
pub fn add(t: T, n: u64) {
    TOTAL[t as usize].fetch_add(n, Relaxed);
    match t {
        T::PrepareUs | T::PresentUs | T::ExecuteUs => CRITICAL.with(|c| c.set(true)),
        _ => {
            if let Some(twin) = critical_twin(t)
                && CRITICAL.with(std::cell::Cell::get)
            {
                TOTAL[twin as usize].fetch_add(n, Relaxed);
            }
        }
    }
}

/// Takes from a total (a time that was summed over threads is corrected to
/// the time it took).
pub fn sub(t: T, n: u64) {
    TOTAL[t as usize].fetch_sub(n, Relaxed);
}

/// Runs `call`, counting it once in `calls` and its nanoseconds in `ns`.
pub fn timed<R>(calls: T, ns: T, call: impl FnOnce() -> R) -> R {
    let started = std::time::Instant::now();
    let r = call();
    add(calls, 1);
    add(ns, started.elapsed().as_nanos() as u64);
    r
}

/// A counter of a frame-rate line (reset by its line with `swap`) that also
/// adds what it is given to a total of the frame log, which no line resets.
/// It reads like the `AtomicU64` it stands for (`load`, `swap` through
/// `Deref`), so its sites stay as they were.
pub struct Counter {
    value: AtomicU64,
    to: Option<T>,
    /// A total counting the adds themselves.
    calls: Option<T>,
}

impl Counter {
    /// A counter that adds to nothing else.
    pub const fn new() -> Counter {
        Counter {
            value: AtomicU64::new(0),
            to: None,
            calls: None,
        }
    }

    /// A counter that adds to total `to` as well.
    pub const fn to(to: T) -> Counter {
        Counter {
            value: AtomicU64::new(0),
            to: Some(to),
            calls: None,
        }
    }

    /// A counter that adds to total `to`, and one to `calls` at every add.
    pub const fn counting(to: T, calls: T) -> Counter {
        Counter {
            value: AtomicU64::new(0),
            to: Some(to),
            calls: Some(calls),
        }
    }

    pub fn fetch_add(&self, n: u64, order: Ordering) -> u64 {
        if let Some(t) = self.to {
            add(t, n);
        }
        if let Some(t) = self.calls {
            add(t, 1);
        }
        self.value.fetch_add(n, order)
    }

    pub fn fetch_sub(&self, n: u64, order: Ordering) -> u64 {
        if let Some(t) = self.to {
            sub(t, n);
        }
        self.value.fetch_sub(n, order)
    }
}

impl Default for Counter {
    fn default() -> Counter {
        Counter::new()
    }
}

impl std::ops::Deref for Counter {
    type Target = AtomicU64;

    fn deref(&self) -> &AtomicU64 {
        &self.value
    }
}

/// What a thread did inside our calls since its last present: for the
/// thread that presents (the title's render thread).
#[derive(Clone, Copy)]
#[repr(usize)]
pub enum H {
    /// Submit calls, microseconds.
    ExecuteUs,
    /// Frame waits, microseconds slept.
    PaceUs,
    Count,
}

const HN: usize = H::Count as usize;

thread_local! {
    static HERE: std::cell::Cell<[u64; HN]> = const { std::cell::Cell::new([0; HN]) };
}

/// Adds to this thread's own count.
pub fn here(h: H, n: u64) {
    HERE.with(|c| {
        let mut v = c.get();
        v[h as usize] += n;
        c.set(v);
    });
}

/// Whether frames are logged: `VICTORY_FRAME_LOG` set and not `0`.
pub fn on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("VICTORY_FRAME_LOG").is_some_and(|v| v != "0"))
}

#[cfg(windows)]
fn micros(t: windows_sys::Win32::Foundation::FILETIME) -> u64 {
    (u64::from(t.dwHighDateTime) << 32 | u64::from(t.dwLowDateTime)) / 10
}

/// This thread's processor time so far (user, kernel), in microseconds.
#[cfg(windows)]
fn thread_times() -> (u64, u64) {
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::System::Threading::{GetCurrentThread, GetThreadTimes};
    let zero = FILETIME {
        dwLowDateTime: 0,
        dwHighDateTime: 0,
    };
    let (mut created, mut exited, mut kernel, mut user) = (zero, zero, zero, zero);
    // SAFETY: our own thread and four valid out pointers.
    unsafe {
        GetThreadTimes(
            GetCurrentThread(),
            &mut created,
            &mut exited,
            &mut kernel,
            &mut user,
        )
    };
    (micros(user), micros(kernel))
}

/// The process's processor time so far (user, kernel), in microseconds.
#[cfg(windows)]
fn process_times() -> (u64, u64) {
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
    let zero = FILETIME {
        dwLowDateTime: 0,
        dwHighDateTime: 0,
    };
    let (mut created, mut exited, mut kernel, mut user) = (zero, zero, zero, zero);
    // SAFETY: our own process and four valid out pointers.
    unsafe {
        GetProcessTimes(
            GetCurrentProcess(),
            &mut created,
            &mut exited,
            &mut kernel,
            &mut user,
        )
    };
    (micros(user), micros(kernel))
}

/// The process's page faults so far (soft and hard).
#[cfg(windows)]
fn page_faults() -> u64 {
    use windows_sys::Win32::System::ProcessStatus::{
        K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS,
    };
    use windows_sys::Win32::System::Threading::GetCurrentProcess;
    // SAFETY: plain data the call fills in.
    let mut c: PROCESS_MEMORY_COUNTERS = unsafe { std::mem::zeroed() };
    c.cb = size_of::<PROCESS_MEMORY_COUNTERS>() as u32;
    // SAFETY: our own process and a sized structure.
    unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut c, c.cb) };
    u64::from(c.PageFaultCount)
}

/// The PC's free physical memory, in MiB.
#[cfg(windows)]
fn free_mb() -> u64 {
    use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
    // SAFETY: plain data the call fills in.
    let mut m: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    m.dwLength = size_of::<MEMORYSTATUSEX>() as u32;
    // SAFETY: a sized structure.
    unsafe { GlobalMemoryStatusEx(&mut m) };
    m.ullAvailPhys >> 20
}

/// Values read when a frame ends, all running totals but the last.
const S: usize = 7;
const S_THREAD_USER: usize = 0;
const S_THREAD_KERNEL: usize = 1;
const S_PROCESS_USER: usize = 2;
const S_PROCESS_KERNEL: usize = 3;
const S_PAGE_FAULTS: usize = 4;
const S_DRAWS: usize = 5;
/// Not a total: the free memory then.
const S_FREE_MB: usize = 6;

#[cfg(windows)]
fn sample() -> [u64; S] {
    let (tu, tk) = thread_times();
    let (pu, pk) = process_times();
    [
        tu,
        tk,
        pu,
        pk,
        page_faults(),
        crate::draw::drawn(),
        free_mb(),
    ]
}

#[cfg(not(windows))]
fn sample() -> [u64; S] {
    [0; S]
}

#[cfg(windows)]
fn thread_id() -> u32 {
    // SAFETY: a plain query.
    unsafe { windows_sys::Win32::System::Threading::GetCurrentThreadId() }
}

#[cfg(not(windows))]
fn thread_id() -> u32 {
    0
}

/// One frame: its number, when it ended (seconds since the first), its
/// length in microseconds, and the differences of the totals, the sampled
/// values and the presenting thread's own counts over it.
#[derive(Clone, Debug)]
struct Frame {
    number: u64,
    at: f64,
    us: f64,
    t: [f64; N],
    s: [f64; S],
    h: [f64; HN],
}

/// The frame between two readings of the totals and samples (`then`, `now`),
/// `us` long. A total that went down (a correction landing after the
/// reading) gives a negative difference rather than a huge one; the free
/// memory is taken as it is now; the thread times are zero if another thread
/// presented.
fn difference(
    number: u64,
    at: f64,
    us: f64,
    then: (&[u64; N], &[u64; S]),
    now: (&[u64; N], &[u64; S]),
    here: [u64; HN],
    same_thread: bool,
) -> Frame {
    let mut f = Frame {
        number,
        at,
        us,
        t: std::array::from_fn(|i| now.0[i].wrapping_sub(then.0[i]) as i64 as f64),
        s: std::array::from_fn(|i| now.1[i].wrapping_sub(then.1[i]) as i64 as f64),
        h: here.map(|v| v as f64),
    };
    f.s[S_FREE_MB] = now.1[S_FREE_MB] as f64;
    if !same_thread {
        (f.s[S_THREAD_USER], f.s[S_THREAD_KERNEL]) = (0.0, 0.0);
    }
    f
}

struct State {
    first: std::time::Instant,
    /// The last frame's end: when, the totals and samples then, and the
    /// thread that presented.
    last: Option<(std::time::Instant, [u64; N], [u64; S], u32)>,
    number: u64,
    frames: Vec<Frame>,
    since: std::time::Instant,
    /// The lengths of the last frames (microseconds), for the usual one.
    recent: std::collections::VecDeque<f64>,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);

fn median(values: impl Iterator<Item = f64>) -> f64 {
    let mut v: Vec<f64> = values.collect();
    if v.is_empty() {
        return 0.0;
    }
    v.sort_by(f64::total_cmp);
    v[v.len() / 2]
}

/// A frame's make-up as text (`f` may be a mean of frames). The sections are
/// split by ` | `; `scripts/linux/ab_runs.py` reads them by their words, so
/// a change here is a change there.
fn text(f: &Frame) -> String {
    let t = |x: T| f.t[x as usize];
    let ms = |x: T| f.t[x as usize] / 1e3;
    let ns_ms = |x: T| f.t[x as usize] / 1e6;
    let s = |i: usize| f.s[i];
    let h = |x: H| f.h[x as usize];
    format!(
        "{:.0} ms | render thread: ran {:.1} ms (user {:.1}, kernel {:.1}); in the present call {:.1} (copy and show {:.1}), \
         in submit calls {:.1}, paced {:.1} | submissions: {:.0}, preparing {:.1} ms, waiting for the card {:.1}; copies \
         {:.1} ms {:.1} MB; copy-back {:.1} ms {:.2} MB | card: idle {:.1} ms ({:.1} behind us) | all submit calls {:.1} ms \
         | textures: refresh {:.1} ms ({:.1} waiting for another thread's refresh, {:.1} asking the watch, {:.1} waiting \
         for memory to hold still), {:.0} uploads | remapping: {:.0} calls {:.1} ms ({:.1} waiting for readers) | xmem: \
         {:.0} mapping calls {:.1} ms; Windows calls {:.0} {:.1} ms; bridge calls {:.0} {:.1} ms; the GPU side told {:.0} {:.1} ms | memory watch, all \
         threads: {:.0} write-watch askings ({:.0} refused) {:.1} ms over {:.0} MB; {:.0} protection changes {:.1} ms; {:.0} \
         region queries {:.1} ms; waits for the guards {:.0} {:.1} ms, for its other locks {:.1} ms; {:.0} guard faults | \
         memory watch, the frame's threads: asking {:.1} ms, protecting {:.1} ms, querying {:.1} ms, guard-waiting {:.1} ms, \
         lock-waiting {:.1} ms, {:.0} faults taken | pipelines: {:.1} ms waited | disk: {:.0} reads {:.1} MB {:.1} worker ms | pacing, all threads {:.1} ms | \
         process: {:.0} user {:.0} kernel ms, {:.0} page faults | {:.0} draws | {:.0} MB free",
        f.us / 1e3,
        (s(S_THREAD_USER) + s(S_THREAD_KERNEL)) / 1e3,
        s(S_THREAD_USER) / 1e3,
        s(S_THREAD_KERNEL) / 1e3,
        ms(T::PresentUs),
        ms(T::PresentShowUs),
        h(H::ExecuteUs) / 1e3,
        h(H::PaceUs) / 1e3,
        t(T::Submissions),
        ms(T::PrepareUs),
        ms(T::CardWaitUs),
        ms(T::CopyUs),
        t(T::CopyBytes) / 1e6,
        ms(T::CopyBackUs),
        t(T::CopyBackBytes) / 1e6,
        ms(T::CardIdleUs),
        ms(T::CardIdleBehindUs),
        ms(T::ExecuteUs),
        ms(T::RefreshUs),
        ms(T::RefreshOtherUs),
        ms(T::RefreshAskUs),
        ms(T::RefreshStillUs),
        t(T::Uploads),
        t(T::Remaps),
        ms(T::RemapUs),
        ms(T::RemapWaitUs),
        t(T::XmemCalls),
        ns_ms(T::XmemNs),
        t(T::RemapOsCalls),
        ns_ms(T::RemapOsNs),
        t(T::RemapBridgeCalls),
        ns_ms(T::RemapBridgeNs),
        t(T::RemapNotes),
        ns_ms(T::RemapNoteNs),
        t(T::WatchAsks),
        t(T::WatchRefused),
        ns_ms(T::WatchNs),
        t(T::WatchBytes) / 1e6,
        t(T::ProtectCalls),
        ns_ms(T::ProtectNs),
        t(T::QueryCalls),
        ns_ms(T::QueryNs),
        t(T::GuardWaits),
        ns_ms(T::GuardWaitNs),
        ns_ms(T::WatchLockWaitNs),
        t(T::GuardFaults),
        ns_ms(T::CritWatchNs),
        ns_ms(T::CritProtectNs),
        ns_ms(T::CritQueryNs),
        ns_ms(T::CritGuardWaitNs),
        ns_ms(T::CritLockWaitNs),
        t(T::CritGuardFaults),
        ms(T::PipelineWaitUs),
        t(T::DiskReads),
        t(T::DiskBytes) / 1e6,
        ms(T::DiskUs),
        ms(T::PaceUs),
        s(S_PROCESS_USER) / 1e3,
        s(S_PROCESS_KERNEL) / 1e3,
        s(S_PAGE_FAULTS),
        s(S_DRAWS),
        s(S_FREE_MB),
    )
}

/// The mean of `frames` (not empty).
fn mean(frames: &[&Frame]) -> Frame {
    let n = frames.len() as f64;
    let mut out = Frame {
        number: 0,
        at: 0.0,
        us: 0.0,
        t: [0.0; N],
        s: [0.0; S],
        h: [0.0; HN],
    };
    for f in frames {
        out.us += f.us / n;
        for i in 0..N {
            out.t[i] += f.t[i] / n;
        }
        for i in 0..S {
            out.s[i] += f.s[i] / n;
        }
        for i in 0..HN {
            out.h[i] += f.h[i] / n;
        }
    }
    out
}

/// Over how many microseconds a frame is long, the usual one being `usual`:
/// half as long again, and at least 40 ms more.
fn long_over(usual: f64) -> f64 {
    (usual * 1.5).max(usual + 40_000.0)
}

/// Frames whose lengths give the usual one, and the long frames listed in a
/// reading at most (the rest are counted).
const RECENT: usize = 64;
const LONG_MOST: usize = 32;

/// The lines of a reading: the frame times, the usual frame, the long ones.
fn lines(frames: &[Frame], usual: f64, seconds: f64) -> Vec<String> {
    let over = long_over(usual);
    let (long, plain): (Vec<&Frame>, Vec<&Frame>) = frames.iter().partition(|f| f.us > over);
    let each: Vec<String> = frames
        .iter()
        .map(|f| format!("{:.0}", f.us / 1e3))
        .collect();
    let shortest = frames.iter().map(|f| f.us).fold(f64::INFINITY, f64::min);
    let longest = frames.iter().map(|f| f.us).fold(0.0, f64::max);
    let mut out = vec![format!(
        "frame times: {} frames in {seconds:.1} s: shortest {:.0} ms, median {:.0}, longest {:.0}; {} long (over {:.0} ms, \
         the usual frame of the last {RECENT} being {:.0}); each: {}",
        frames.len(),
        shortest / 1e3,
        median(frames.iter().map(|f| f.us)) / 1e3,
        longest / 1e3,
        long.len(),
        over / 1e3,
        usual / 1e3,
        each.join(" ")
    )];
    if !plain.is_empty() {
        out.push(format!(
            "usual frame (the mean of {} not long): {}",
            plain.len(),
            text(&mean(&plain))
        ));
    }
    for f in long.iter().take(LONG_MOST) {
        out.push(format!(
            "long frame {} at {:.1} s: {}",
            f.number,
            f.at,
            text(f)
        ));
    }
    if long.len() > LONG_MOST {
        out.push(format!(
            "long frames: {} more in this reading are not listed",
            long.len() - LONG_MOST
        ));
    }
    out
}

/// The usual frame after `frames` join the recent ones: the median of the
/// last [`RECENT`] lengths, this reading's with those before it.
fn usual_after(recent: &mut std::collections::VecDeque<f64>, frames: &[Frame]) -> f64 {
    for f in frames {
        if recent.len() == RECENT {
            recent.pop_front();
        }
        recent.push_back(f.us);
    }
    median(recent.iter().copied())
}

/// A frame ends: called at the end of the title's present call, on the
/// thread that made it. Nothing without the switch.
pub fn frame() {
    if !on() {
        return;
    }
    let now = std::time::Instant::now();
    let totals: [u64; N] = std::array::from_fn(|i| TOTAL[i].load(Relaxed));
    let sampled = sample();
    let here = HERE.with(|c| c.replace([0; HN]));
    let thread = thread_id();
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    let state = state.get_or_insert_with(|| State {
        first: now,
        last: None,
        number: 0,
        frames: Vec::new(),
        since: now,
        recent: std::collections::VecDeque::new(),
    });
    state.number += 1;
    if let Some((then, t0, s0, thread0)) = state.last.replace((now, totals, sampled, thread)) {
        let f = difference(
            state.number,
            (now - state.first).as_secs_f64(),
            (now - then).as_micros() as f64,
            (&t0, &s0),
            (&totals, &sampled),
            here,
            thread == thread0,
        );
        state.frames.push(f);
    }
    let elapsed = state.since.elapsed();
    if elapsed < std::time::Duration::from_secs(5) || state.frames.is_empty() {
        return;
    }
    let frames = std::mem::take(&mut state.frames);
    state.since = now;
    let usual = usual_after(&mut state.recent, &frames);
    for line in lines(&frames, usual, elapsed.as_secs_f64()) {
        tracing::info!("{line}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(number: u64, ms: f64) -> Frame {
        let mut f = Frame {
            number,
            at: number as f64,
            us: ms * 1e3,
            t: [0.0; N],
            s: [0.0; S],
            h: [0.0; HN],
        };
        f.t[T::PresentUs as usize] = ms * 100.0;
        f.t[T::Uploads as usize] = ms;
        f
    }

    #[test]
    fn a_long_frame_is_listed_beside_the_usual_one() {
        let frames: Vec<Frame> = [150.0, 160.0, 140.0, 400.0, 150.0]
            .iter()
            .enumerate()
            .map(|(i, &ms)| frame(i as u64, ms))
            .collect();
        let out = lines(&frames, 150_000.0, 1.0);
        assert_eq!(out.len(), 3, "{out:#?}");
        assert!(
            out[0].starts_with("frame times: 5 frames in 1.0 s: shortest 140 ms, median 150, longest 400; 1 long (over 225 ms"),
            "{}",
            out[0]
        );
        assert!(out[0].ends_with("each: 150 160 140 400 150"), "{}", out[0]);
        // The usual frame is the mean of the four that are not long.
        assert!(
            out[1].starts_with("usual frame (the mean of 4 not long): 150 ms |"),
            "{}",
            out[1]
        );
        assert!(out[1].contains("in the present call 15.0 "), "{}", out[1]);
        assert!(out[1].contains(" 150 uploads"), "{}", out[1]);
        assert!(
            out[2].starts_with("long frame 3 at 3.0 s: 400 ms |"),
            "{}",
            out[2]
        );
        assert!(out[2].contains("in the present call 40.0 "), "{}", out[2]);
        assert!(out[2].contains(" 400 uploads"), "{}", out[2]);
    }

    #[test]
    fn long_is_half_as_long_again_and_at_least_40_ms_more() {
        assert_eq!(long_over(150_000.0), 225_000.0);
        assert_eq!(long_over(30_000.0), 70_000.0);
        // Exactly at the limit is not long.
        let frames = [frame(0, 70.0), frame(1, 70.1)];
        let out = lines(&frames, 30_000.0, 1.0);
        assert!(out[0].contains("; 1 long (over 70 ms"), "{}", out[0]);
        assert!(out[2].starts_with("long frame 1 "), "{out:#?}");
    }

    #[test]
    fn more_long_frames_than_are_listed_are_counted() {
        let mut frames: Vec<Frame> = (0..LONG_MOST as u64 + 4).map(|i| frame(i, 500.0)).collect();
        frames.push(frame(99, 100.0));
        let out = lines(&frames, 100_000.0, 5.0);
        assert_eq!(
            out.iter().filter(|l| l.starts_with("long frame ")).count(),
            LONG_MOST
        );
        assert!(
            out.last().unwrap().starts_with("long frames: 4 more"),
            "{out:#?}"
        );
    }

    #[test]
    fn only_long_frames_give_no_usual_line() {
        let frames = [frame(0, 500.0), frame(1, 600.0)];
        let out = lines(&frames, 100_000.0, 5.0);
        assert!(
            !out.iter().any(|l| l.starts_with("usual frame")),
            "{out:#?}"
        );
        assert_eq!(
            out.iter().filter(|l| l.starts_with("long frame ")).count(),
            2
        );
    }

    #[test]
    fn the_usual_frame_is_the_median_of_the_recent_ones() {
        let mut recent = std::collections::VecDeque::new();
        let frames: Vec<Frame> = [100.0, 300.0, 200.0]
            .iter()
            .enumerate()
            .map(|(i, &ms)| frame(i as u64, ms))
            .collect();
        assert_eq!(usual_after(&mut recent, &frames), 200_000.0);
        // Only the last RECENT count: 64 frames of 50 ms push those out.
        let short: Vec<Frame> = (0..RECENT as u64).map(|i| frame(i, 50.0)).collect();
        assert_eq!(usual_after(&mut recent, &short), 50_000.0);
        assert_eq!(recent.len(), RECENT);
    }

    #[test]
    fn a_frame_is_the_difference_of_two_readings() {
        let mut t0 = [0u64; N];
        let mut t1 = [0u64; N];
        let mut s0 = [0u64; S];
        let mut s1 = [0u64; S];
        t0[T::RemapOsCalls as usize] = 10;
        t1[T::RemapOsCalls as usize] = 35;
        // A correction that took more than was added since: negative.
        t0[T::CopyUs as usize] = 500;
        t1[T::CopyUs as usize] = 400;
        s0[S_THREAD_USER] = 1_000;
        s1[S_THREAD_USER] = 4_000;
        s0[S_FREE_MB] = 9_000;
        s1[S_FREE_MB] = 8_000;
        let f = difference(7, 2.5, 33_000.0, (&t0, &s0), (&t1, &s1), [5, 6], true);
        assert_eq!(f.t[T::RemapOsCalls as usize], 25.0);
        assert_eq!(f.t[T::CopyUs as usize], -100.0);
        assert_eq!(f.s[S_THREAD_USER], 3_000.0);
        assert_eq!(
            f.s[S_FREE_MB], 8_000.0,
            "the free memory as it is, not a difference"
        );
        assert_eq!(f.h, [5.0, 6.0]);
        // Another thread presented: its processor time is not this frame's.
        let g = difference(8, 2.6, 33_000.0, (&t0, &s0), (&t1, &s1), [0, 0], false);
        assert_eq!(g.s[S_THREAD_USER], 0.0);
    }

    #[test]
    fn the_memory_columns_are_in_the_line() {
        let mut f = frame(1, 300.0);
        f.t[T::Remaps as usize] = 12.0;
        f.t[T::RemapUs as usize] = 45_600.0;
        f.t[T::XmemCalls as usize] = 3.0;
        f.t[T::XmemNs as usize] = 40_000_000.0;
        f.t[T::RemapOsCalls as usize] = 2_100.0;
        f.t[T::RemapOsNs as usize] = 31_500_000.0;
        f.t[T::RemapBridgeCalls as usize] = 40.0;
        f.t[T::RemapBridgeNs as usize] = 2_500_000.0;
        f.t[T::WatchAsks as usize] = 90.0;
        f.t[T::WatchNs as usize] = 4_500_000.0;
        f.t[T::WatchBytes as usize] = 64e6;
        f.t[T::ProtectCalls as usize] = 400.0;
        f.t[T::ProtectNs as usize] = 2_000_000.0;
        f.t[T::QueryCalls as usize] = 50.0;
        f.t[T::QueryNs as usize] = 1_000_000.0;
        f.t[T::GuardWaits as usize] = 4.0;
        f.t[T::GuardWaitNs as usize] = 7_300_000.0;
        f.t[T::GuardFaults as usize] = 33.0;
        f.t[T::CritWatchNs as usize] = 1_500_000.0;
        f.t[T::CritGuardWaitNs as usize] = 2_200_000.0;
        f.t[T::CritGuardFaults as usize] = 5.0;
        let line = text(&f);
        for want in [
            "remapping: 12 calls 45.6 ms",
            "xmem: 3 mapping calls 40.0 ms; Windows calls 2100 31.5 ms; bridge calls 40 2.5 ms",
            "memory watch, all threads: 90 write-watch askings (0 refused) 4.5 ms over 64 MB",
            "400 protection changes 2.0 ms",
            "50 region queries 1.0 ms",
            "waits for the guards 4 7.3 ms",
            "33 guard faults",
            "memory watch, the frame's threads: asking 1.5 ms, protecting 0.0 ms, querying 0.0 ms, guard-waiting 2.2 ms",
            "5 faults taken",
        ] {
            assert!(line.contains(want), "{want:?} not in {line}");
        }
    }

    /// Memory costs count as the frame's own only on threads that prepared,
    /// presented or submitted.
    #[test]
    fn the_frames_threads_are_counted_apart() {
        let crit = |t: T| TOTAL[t as usize].load(Relaxed);
        let (q, g) = (crit(T::CritQueryNs), crit(T::CritGuardFaults));
        std::thread::spawn(|| {
            add(T::QueryNs, 5);
            add(T::GuardFaults, 1);
        })
        .join()
        .unwrap();
        assert_eq!(
            (crit(T::CritQueryNs) - q, crit(T::CritGuardFaults) - g),
            (0, 0)
        );
        std::thread::spawn(|| {
            add(T::PrepareUs, 1);
            add(T::QueryNs, 7);
            add(T::GuardFaults, 2);
            add(T::DiskReads, 1); // no twin
        })
        .join()
        .unwrap();
        assert_eq!(
            (crit(T::CritQueryNs) - q, crit(T::CritGuardFaults) - g),
            (7, 2)
        );
    }

    #[test]
    fn totals_counters_and_a_threads_own_counts_add_up() {
        let before = TOTAL[T::DiskReads as usize].load(Relaxed);
        add(T::DiskReads, 3);
        sub(T::DiskReads, 1);
        assert_eq!(TOTAL[T::DiskReads as usize].load(Relaxed) - before, 2);
        here(H::PaceUs, 5);
        here(H::PaceUs, 6);
        assert_eq!(HERE.with(|c| c.get())[H::PaceUs as usize], 11);
        // A frame-rate line's counter: its line resets it, the total runs on.
        static C: Counter = Counter::counting(T::DiskBytes, T::DiskUs);
        let (bytes, calls) = (
            TOTAL[T::DiskBytes as usize].load(Relaxed),
            TOTAL[T::DiskUs as usize].load(Relaxed),
        );
        C.fetch_add(100, Relaxed);
        C.fetch_sub(30, Relaxed);
        assert_eq!(C.swap(0, Relaxed), 70);
        C.fetch_add(5, Relaxed);
        assert_eq!(C.load(Relaxed), 5);
        assert_eq!(TOTAL[T::DiskBytes as usize].load(Relaxed) - bytes, 75);
        assert_eq!(TOTAL[T::DiskUs as usize].load(Relaxed) - calls, 2);
        // (Totals no other test of this crate touches.)
        let timed_before = TOTAL[T::DiskReads as usize].load(Relaxed);
        assert_eq!(timed(T::DiskReads, T::DiskBytes, || 4), 4);
        assert_eq!(TOTAL[T::DiskReads as usize].load(Relaxed) - timed_before, 1);
    }
}
