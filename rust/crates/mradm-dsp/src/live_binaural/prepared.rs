//! Bounded transaction-local replay of fully mixed HRTFs and their diagnostics.
//! Overflow disables replay for the entire frame; audio state still commits only
//! after the original complete preview succeeds.
use super::Report;

const BYTE_BUDGET: usize = 2 * 1024 * 1024;
const SLOT_LIMIT: usize = 24 * 3; // initialization plus two endpoints per object

pub(super) struct PreparedHrtfs {
    values: Box<[f32]>,
    reports: Box<[Report]>,
    width: usize,
    used: usize,
    cursor: usize,
    overflow: bool,
    replaying: bool,
}

impl PreparedHrtfs {
    pub(super) fn new(width: usize, objects: usize) -> Self {
        Self::with_limit(width, objects.min(24) * 3)
    }

    pub(super) fn with_limit(width: usize, limit: usize) -> Self {
        let count = width
            .checked_mul(size_of::<f32>())
            .and_then(|bytes| bytes.checked_add(size_of::<Report>()))
            .map_or(0, |bytes| {
                ((BYTE_BUDGET - size_of::<Self>()) / bytes).min(limit.min(SLOT_LIMIT))
            });
        Self {
            values: vec![0.; count * width].into_boxed_slice(),
            reports: vec![Report::default(); count].into_boxed_slice(),
            width,
            used: 0,
            cursor: 0,
            overflow: false,
            replaying: false,
        }
    }

    pub(super) fn begin(&mut self) {
        self.used = 0;
        self.cursor = 0;
        self.overflow = false;
        self.replaying = false;
    }

    pub(super) fn record(&mut self, values: &[f32], report: Report) {
        if self.overflow {
            return;
        }
        if self.used == self.reports.len() {
            self.overflow = true;
            self.used = 0;
            return;
        }
        let begin = self.used * self.width;
        self.values[begin..begin + self.width].copy_from_slice(values);
        self.reports[self.used] = report;
        self.used += 1;
    }

    pub(super) fn finish_preview(&mut self) {
        self.cursor = 0;
        self.replaying = !self.overflow;
    }

    pub(super) fn replay(&mut self, values: &mut [f32], report: &mut Report) -> bool {
        if !self.replaying {
            return false;
        }
        assert!(self.cursor < self.used, "prevalidated HRTF replay sequence");
        let begin = self.cursor * self.width;
        values.copy_from_slice(&self.values[begin..begin + self.width]);
        *report = self.reports[self.cursor];
        self.cursor += 1;
        true
    }

    pub(super) fn finish_commit(&self) {
        debug_assert!(!self.replaying || self.cursor == self.used);
    }

    #[cfg(test)]
    pub(super) fn recorded(&self) -> Option<usize> {
        self.replaying.then_some(self.used)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn overflow_discards_the_entire_tape_and_next_frame_recovers() {
        let mut tape = PreparedHrtfs::with_limit(4, 2);
        let mut report = Report::default();
        report.emit(7, 2);
        tape.begin();
        tape.record(&[1.; 4], report);
        tape.record(&[2.; 4], report);
        tape.record(&[3.; 4], report);
        tape.finish_preview();
        let mut out = [17.; 4];
        let mut actual = Report::default();
        assert!(!tape.replay(&mut out, &mut actual));
        assert_eq!(out, [17.; 4]);
        assert_eq!(actual, Report::default());
        tape.begin();
        tape.record(&[4.; 4], report);
        tape.finish_preview();
        assert!(tape.replay(&mut out, &mut actual));
        assert_eq!(out, [4.; 4]);
        assert_eq!(actual, report);
        tape.finish_commit();
        tape.begin();
        assert!(!tape.replay(&mut out, &mut actual));
    }

    #[test]
    fn storage_is_bounded_including_reports_and_long_spectra() {
        for width in [8, 4100, 8196, 131076, usize::MAX] {
            let tape = PreparedHrtfs::new(width, usize::MAX);
            assert!(tape.reports.len() <= SLOT_LIMIT);
            assert!(
                size_of::<PreparedHrtfs>()
                    + size_of_val(&*tape.values)
                    + size_of_val(&*tape.reports)
                    <= BYTE_BUDGET
            );
        }
        assert_eq!(PreparedHrtfs::new(4100, 24).reports.len(), 72);
        assert!(PreparedHrtfs::new(4100, 0).reports.is_empty());
    }
}
