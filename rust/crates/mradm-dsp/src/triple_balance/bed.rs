//! Routing of the ten RC 7.1.2 bed channels (L R C LFE Lss Rss Lrs Rrs Lts Rts) into a
//! Triple Balance output layout. File, file-stream and Scene rendering share these
//! coefficients; C++ only identifies the bed channel and the layout.
use super::{Layout, invalid};
use crate::Result;

pub const BED_CHANNELS: usize = 10;
const ROOM_222_DESTINATIONS: [usize; BED_CHANNELS] = [6, 7, 2, 3, 10, 11, 4, 5, 18, 19];
const LFE: usize = 3;

/// Writes the gains of bed channel `index` for every output channel of `layout`.
/// `split_lfe` divides the LFE equally in power between both 22.2 LFE channels.
pub fn gains(layout: Layout, index: usize, split_lfe: bool, out: &mut [f32]) -> Result<()> {
    if index >= BED_CHANNELS || out.len() != layout.channels() {
        return Err(invalid());
    }
    out.fill(0.);
    match layout {
        Layout::Room222 if index == LFE && split_lfe => {
            let equal_power = 0.5f32.sqrt();
            out[3] = equal_power;
            out[9] = equal_power;
        }
        Layout::Room222 => out[ROOM_222_DESTINATIONS[index]] = 1.,
        // 7.1.4 has no side tops: each top source feeds front and rear tops equally.
        Layout::Seven if index >= 8 => {
            let equal_power = 0.5f32.sqrt();
            out[index] = equal_power;
            out[index + 2] = equal_power;
        }
        Layout::Nine if index >= 8 => out[index + 4] = 1.,
        Layout::Seven | Layout::Nine => out[index] = 1.,
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn route(layout: Layout, index: usize, split_lfe: bool) -> Vec<(usize, f32)> {
        let mut out = vec![0f32; layout.channels()];
        gains(layout, index, split_lfe, &mut out).unwrap();
        out.into_iter()
            .enumerate()
            .filter(|(_, g)| *g != 0.)
            .collect()
    }

    #[test]
    fn routes_every_bed_channel() {
        let half = std::f32::consts::FRAC_1_SQRT_2;
        assert_eq!(half.to_bits(), 0.5f32.sqrt().to_bits());
        assert_eq!(route(Layout::Seven, 1, true), [(1, 1.)]);
        assert_eq!(route(Layout::Seven, 9, false), [(9, half), (11, half)]);
        assert_eq!(route(Layout::Nine, 3, true), [(3, 1.)]);
        assert_eq!(route(Layout::Nine, 8, false), [(12, 1.)]);
        assert_eq!(route(Layout::Room222, 0, false), [(6, 1.)]);
        assert_eq!(route(Layout::Room222, 3, false), [(3, 1.)]);
        assert_eq!(route(Layout::Room222, 3, true), [(3, half), (9, half)]);
        assert_eq!(route(Layout::Room222, 9, true), [(19, 1.)]);
        let mut out = [0f32; 12];
        assert!(gains(Layout::Seven, 10, false, &mut out).is_err());
        assert!(gains(Layout::Nine, 0, false, &mut out).is_err());
    }
}
