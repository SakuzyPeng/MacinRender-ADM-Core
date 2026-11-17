//! Native ADM block levels on the file's rtime clock, independent of spatial control grids.
//! Envelopes are generated only for the current render block; seeks use absolute sample time.

#[derive(Clone, Copy)]
pub(crate) struct Event {
    pub start: u64,
    pub end: u64,
    pub gain: f32,
    pub jump: bool,
    pub interpolation: Option<u64>,
}

#[derive(Default)]
pub struct BlockGains {
    curves: Vec<(usize, Vec<Event>)>,
}

impl BlockGains {
    pub(crate) fn push(&mut self, channel: usize, mut events: Vec<Event>) {
        events.sort_by_key(|e| e.start);
        // Keep the last event at each timestamp, including its interpolation predecessor.
        let mut curve: Vec<Event> = Vec::with_capacity(events.len());
        for event in events {
            if let Some(last) = curve.last_mut()
                && last.start == event.start
            {
                *last = event;
            } else {
                curve.push(event);
            }
        }
        if curve.iter().any(|e| e.gain != 1.) {
            self.curves.push((channel, curve));
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.curves.is_empty()
    }

    /// Multiply a bounded interleaved unity/live envelope. Spatial and filter state is untouched.
    pub(crate) fn apply(&self, levels: &mut [f32], inputs: usize, start: u64, default: u64) {
        for (channel, curve) in &self.curves {
            let mut next = curve.partition_point(|e| e.start <= start);
            for (f, frame) in levels.chunks_exact_mut(inputs).enumerate() {
                let at = start + f as u64;
                while next < curve.len() && curve[next].start <= at {
                    next += 1;
                }
                if next == 0 {
                    continue;
                }
                let i = next - 1;
                let event = curve[i];
                let end = curve
                    .get(next)
                    .map_or(event.end, |e| event.end.min(e.start));
                let length = if i == 0 || event.jump {
                    0
                } else {
                    event
                        .interpolation
                        .unwrap_or(default)
                        .min(end.saturating_sub(event.start))
                };
                let delta = at - event.start;
                let gain = if delta < length {
                    let t = delta as f64 / length as f64;
                    (f64::from(curve[i - 1].gain) * (1. - t) + f64::from(event.gain) * t) as f32
                } else {
                    event.gain
                };
                frame[*channel] *= gain;
            }
        }
    }
}
