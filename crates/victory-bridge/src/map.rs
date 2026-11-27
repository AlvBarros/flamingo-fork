//! What the bridge's windows hold, page by page: the answer to `query`
//! without asking Wine (whose view of a window is "reserved, no access"
//! throughout). Pure bookkeeping, no system calls; kept in step with the
//! Unix half by `lib.rs` (changed only after the Unix half did the change).

use std::collections::BTreeMap;

use crate::Prot;

/// What a range of a window shows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Reserved and inaccessible (`PROT_NONE` anonymous memory).
    Unmapped,
    /// A view of `pool`'s pages from byte `offset`.
    View { pool: u64, offset: u64 },
}

/// A run of pages with the same kind (consecutive pool bytes for a view)
/// and protection, as `VirtualQuery` would describe it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    /// The asked address's page.
    pub base: usize,
    /// Bytes from `base` to the end of the run.
    pub len: usize,
    pub kind: Kind,
    pub prot: Prot,
    /// The window holding it: base and length (`AllocationBase`).
    pub window: (usize, usize),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Seg {
    len: usize,
    kind: Kind,
    prot: Prot,
}

impl Seg {
    /// `self` cut to start `skip` bytes in.
    fn skip(self, skip: usize) -> Seg {
        let kind = match self.kind {
            Kind::View { pool, offset } => Kind::View {
                pool,
                offset: offset + skip as u64,
            },
            k => k,
        };
        Seg {
            len: self.len - skip,
            kind,
            prot: self.prot,
        }
    }

    /// Whether `next`, right after `self`, continues the same run.
    fn continued_by(&self, next: &Seg) -> bool {
        self.prot == next.prot
            && match (self.kind, next.kind) {
                (Kind::Unmapped, Kind::Unmapped) => true,
                (Kind::View { pool: a, offset: o }, Kind::View { pool: b, offset: p }) => {
                    a == b && o + self.len as u64 == p
                }
                _ => false,
            }
    }
}

const PAGE: usize = crate::abi::GRANULE as usize;

/// The windows and what they hold. Every byte of a window is in exactly one
/// segment.
#[derive(Default, Debug)]
pub struct Map {
    windows: BTreeMap<usize, usize>,
    segs: BTreeMap<usize, Seg>,
}

impl Map {
    /// The window holding `addr`: base and length.
    pub fn window(&self, addr: usize) -> Option<(usize, usize)> {
        self.windows
            .range(..=addr)
            .next_back()
            .filter(|(base, len)| addr - **base < **len)
            .map(|(b, l)| (*b, *l))
    }

    /// Whether `[addr, addr + len)` lies in one window.
    pub fn inside(&self, addr: usize, len: usize) -> bool {
        len > 0
            && self.window(addr).is_some_and(|(base, wlen)| {
                addr.checked_add(len).is_some_and(|end| end <= base + wlen)
            })
    }

    /// Whether [`Map::add_window`] would take `[addr, addr + len)`.
    pub fn window_fits(&self, addr: usize, len: usize) -> bool {
        if len == 0
            || !addr.is_multiple_of(PAGE)
            || !len.is_multiple_of(PAGE)
            || addr.checked_add(len).is_none()
        {
            return false;
        }
        self.window(addr).is_none() && self.windows.range(addr..addr + len).next().is_none()
    }

    /// A new window, unmapped throughout. Refused when misaligned, empty or
    /// overlapping another.
    pub fn add_window(&mut self, addr: usize, len: usize) -> bool {
        if !self.window_fits(addr, len) {
            return false;
        }
        self.windows.insert(addr, len);
        self.segs.insert(
            addr,
            Seg {
                len,
                kind: Kind::Unmapped,
                prot: Prot::NONE,
            },
        );
        true
    }

    /// Forgets the window starting at `addr`; its length.
    pub fn remove_window(&mut self, addr: usize) -> Option<usize> {
        let len = self.windows.remove(&addr)?;
        let inside: Vec<usize> = self.segs.range(addr..addr + len).map(|(a, _)| *a).collect();
        for a in inside {
            self.segs.remove(&a);
        }
        Some(len)
    }

    /// Makes a segment start at `at` (inside a window).
    fn cut(&mut self, at: usize) {
        let Some((&start, &seg)) = self.segs.range(..=at).next_back() else {
            return;
        };
        if start == at || at >= start + seg.len {
            return;
        }
        let head = at - start;
        self.segs.insert(start, Seg { len: head, ..seg });
        self.segs.insert(at, seg.skip(head));
    }

    /// Sets `[addr, addr + len)` (inside one window) to `f` of each segment
    /// there and its distance from `addr`.
    fn set(&mut self, addr: usize, len: usize, f: impl Fn(Seg, usize) -> Seg) -> bool {
        if !self.inside(addr, len) || !addr.is_multiple_of(PAGE) || !len.is_multiple_of(PAGE) {
            return false;
        }
        self.cut(addr);
        self.cut(addr + len);
        let inside: Vec<(usize, Seg)> = self
            .segs
            .range(addr..addr + len)
            .map(|(a, s)| (*a, *s))
            .collect();
        for (a, s) in inside {
            self.segs.insert(a, f(s, a - addr));
        }
        self.join(addr, addr + len);
        true
    }

    /// Joins the segments of `[from, to)` and its two neighbours that
    /// continue each other, so a run is one segment and [`Map::query`] one
    /// lookup (WS4: 60,000 views of consecutive pool bytes left as 60,000
    /// segments cost 132 µs a query, 50x Wine's).
    fn join(&mut self, from: usize, to: usize) {
        let start = self
            .segs
            .range(..from)
            .next_back()
            .map_or(from, |(a, _)| *a);
        let mut at = start;
        while let Some(&seg) = self.segs.get(&at) {
            let next_at = at + seg.len;
            // Never across windows: each keeps its own segments.
            let next = self
                .segs
                .get(&next_at)
                .copied()
                .filter(|_| !self.windows.contains_key(&next_at));
            match next {
                Some(next) if seg.continued_by(&next) => {
                    self.segs.remove(&next_at);
                    self.segs.insert(
                        at,
                        Seg {
                            len: seg.len + next.len,
                            ..seg
                        },
                    );
                }
                _ if next_at > to => break,
                Some(_) => at = next_at,
                None => break,
            }
        }
    }

    /// Segments, for tests.
    #[cfg(test)]
    fn segments(&self) -> usize {
        self.segs.len()
    }

    /// A view of `pool` from `offset` at `[addr, addr + len)`.
    pub fn map(&mut self, addr: usize, len: usize, pool: u64, offset: u64, prot: Prot) -> bool {
        self.set(addr, len, |s, from| Seg {
            len: s.len,
            kind: Kind::View {
                pool,
                offset: offset + from as u64,
            },
            prot,
        })
    }

    /// Back to unmapped.
    pub fn unmap(&mut self, addr: usize, len: usize) -> bool {
        self.set(addr, len, |s, _| Seg {
            len: s.len,
            kind: Kind::Unmapped,
            prot: Prot::NONE,
        })
    }

    /// Whether every page of `[addr, addr + len)` is a view (only views take
    /// protections: an unmapped page made accessible would show zeros).
    pub fn all_views(&self, addr: usize, len: usize) -> bool {
        if !self.inside(addr, len) {
            return false;
        }
        let first = self.segs.range(..=addr).next_back();
        first
            .into_iter()
            .chain(self.segs.range(addr + 1..addr + len))
            .all(|(_, s)| matches!(s.kind, Kind::View { .. }))
    }

    /// New protection on views; refused (nothing changed) when part of the
    /// range is unmapped.
    pub fn protect(&mut self, addr: usize, len: usize, prot: Prot) -> bool {
        self.all_views(addr, len) && self.set(addr, len, |s, _| Seg { prot, ..s })
    }

    /// What `addr` is part of, from its page to the end of its run.
    pub fn query(&self, addr: usize) -> Option<Region> {
        let window = self.window(addr)?;
        let base = addr & !(PAGE - 1);
        let (&start, &seg) = self.segs.range(..=base).next_back()?;
        let first = seg.skip(base - start);
        let mut len = first.len;
        let mut last = first;
        for (_, next) in self.segs.range(start + seg.len..window.0 + window.1) {
            if !last.continued_by(next) {
                break;
            }
            len += next.len;
            last = *next;
        }
        Some(Region {
            base,
            len,
            kind: first.kind,
            prot: first.prot,
            window,
        })
    }

    /// The views of `pool` (address, length), for checks and teardown.
    pub fn views_of(&self, pool: u64) -> Vec<(usize, usize)> {
        self.segs
            .iter()
            .filter(|(_, s)| matches!(s.kind, Kind::View { pool: p, .. } if p == pool))
            .map(|(a, s)| (*a, s.len))
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const W: usize = 0x1000_0000;
    const K64: usize = 0x1_0000;

    fn map() -> Map {
        let mut m = Map::default();
        assert!(m.add_window(W, 16 * K64));
        m
    }

    /// Views mapped one by one over consecutive pool bytes become one
    /// segment, and unmapping them again one by one leaves one: a query is a
    /// lookup, not a walk (WS4's 132 µs at 60,000 views).
    #[test]
    fn runs_are_joined_so_a_query_does_not_walk() {
        let mut m = Map::default();
        let n = 60_000;
        assert!(m.add_window(W, n * K64));
        for i in 0..n {
            assert!(m.map(W + i * K64, K64, 3, (i * K64) as u64, Prot::READ_WRITE));
        }
        assert_eq!(m.segments(), 1);
        let started = std::time::Instant::now();
        let r = m.query(W + 5 * K64).unwrap();
        assert!(started.elapsed() < std::time::Duration::from_millis(1));
        assert_eq!(r.len, (n - 5) * K64);
        for i in 0..n {
            assert!(m.unmap(W + i * K64, K64));
        }
        assert_eq!(m.segments(), 1);
        assert_eq!(m.query(W).unwrap().len, n * K64);
        // An alias in the middle splits the run in three, and goes again.
        assert!(m.map(W, 3 * K64, 3, 0, Prot::READ_WRITE));
        assert!(m.map(W + K64, K64, 3, 0, Prot::READ_WRITE));
        assert_eq!(m.query(W).unwrap().len, K64);
        assert!(m.map(W + K64, K64, 3, K64 as u64, Prot::READ_WRITE));
        assert_eq!(m.query(W).unwrap().len, 3 * K64);
    }

    /// Adjacent windows stay apart (joining runs must not cross them).
    #[test]
    fn adjacent_windows_are_never_joined() {
        let mut m = map();
        assert!(m.add_window(W + 16 * K64, 4 * K64));
        assert!(m.unmap(W + 15 * K64, K64));
        assert!(m.unmap(W + 16 * K64, K64));
        assert_eq!(m.query(W).unwrap().len, 16 * K64);
        assert_eq!(
            m.query(W + 16 * K64).unwrap().window,
            (W + 16 * K64, 4 * K64)
        );
        assert_eq!(m.remove_window(W), Some(16 * K64));
        assert_eq!(m.query(W + 17 * K64).unwrap().len, 3 * K64);
    }

    #[test]
    fn a_new_window_is_unmapped_throughout() {
        let m = map();
        let r = m.query(W + 5).unwrap();
        assert_eq!(
            (r.base, r.len, r.kind, r.prot),
            (W, 16 * K64, Kind::Unmapped, Prot::NONE)
        );
        assert_eq!(r.window, (W, 16 * K64));
        assert!(m.query(W - 1).is_none());
        assert!(m.query(W + 16 * K64).is_none());
    }

    #[test]
    fn windows_do_not_overlap_and_are_aligned() {
        let mut m = map();
        assert!(!m.add_window(W + K64, K64));
        assert!(!m.add_window(W - K64, 2 * K64));
        assert!(!m.add_window(W + 16 * K64 + 1, K64));
        assert!(!m.add_window(W + 32 * K64, 0));
        assert!(m.add_window(W + 16 * K64, K64));
        assert_eq!(m.remove_window(W), Some(16 * K64));
        assert!(m.query(W).is_none());
        assert!(m.query(W + 16 * K64).is_some());
    }

    #[test]
    fn views_split_and_report_their_pool_offsets() {
        let mut m = map();
        assert!(m.map(W + K64, 4 * K64, 7, 0x40_0000, Prot::READ_WRITE));
        let r = m.query(W + 2 * K64 + 0x1234).unwrap();
        assert_eq!(r.base, W + 2 * K64 + 0x1000);
        assert_eq!(r.len, 3 * K64 - 0x1000);
        assert_eq!(
            r.kind,
            Kind::View {
                pool: 7,
                offset: 0x40_0000 + K64 as u64 + 0x1000
            }
        );
        assert_eq!(m.query(W).unwrap().len, K64);
        assert_eq!(m.query(W + 5 * K64).unwrap().len, 11 * K64);
        assert_eq!(m.views_of(7), vec![(W + K64, 4 * K64)]);
    }

    #[test]
    fn adjacent_views_of_consecutive_bytes_read_as_one_run() {
        let mut m = map();
        assert!(m.map(W, K64, 1, 0, Prot::READ_WRITE));
        assert!(m.map(W + K64, K64, 1, K64 as u64, Prot::READ_WRITE));
        assert!(m.map(W + 2 * K64, K64, 1, 0, Prot::READ_WRITE)); // an alias: a new run
        assert_eq!(m.query(W).unwrap().len, 2 * K64);
        assert_eq!(m.query(W + 2 * K64).unwrap().len, K64);
    }

    #[test]
    fn protections_apply_to_views_only() {
        let mut m = map();
        assert!(m.map(W, 2 * K64, 1, 0, Prot::READ_WRITE));
        assert!(!m.protect(W + K64, 2 * K64, Prot::READ)); // reaches unmapped pages
        assert_eq!(m.query(W).unwrap().prot, Prot::READ_WRITE);
        assert!(m.protect(W + K64, 0x1000, Prot::READ));
        assert_eq!(m.query(W).unwrap().len, K64);
        let r = m.query(W + K64).unwrap();
        assert_eq!((r.len, r.prot), (0x1000, Prot::READ));
        assert_eq!(m.query(W + K64 + 0x1000).unwrap().prot, Prot::READ_WRITE);
    }

    #[test]
    fn unmapping_leaves_the_range_ours_and_unmapped() {
        let mut m = map();
        assert!(m.map(W, 4 * K64, 1, 0, Prot::READ_WRITE));
        assert!(m.unmap(W + K64, 2 * K64));
        assert_eq!(m.query(W + K64).unwrap().kind, Kind::Unmapped);
        assert_eq!(m.query(W + K64).unwrap().len, 2 * K64);
        assert_eq!(
            m.query(W + 3 * K64).unwrap().kind,
            Kind::View {
                pool: 1,
                offset: 3 * K64 as u64
            }
        );
        assert!(!m.unmap(W + 15 * K64, 2 * K64)); // past the window
        assert!(!m.map(W + 1, K64, 1, 0, Prot::READ)); // misaligned
    }
}
