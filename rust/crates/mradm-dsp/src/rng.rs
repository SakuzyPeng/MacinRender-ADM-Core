//! PCG-XSH-RR, with explicit integer and float conversion rules. State belongs
//! to the source/geometry instance; neither process history nor workers seed it.
pub struct Pcg32 {
    state: u64,
    increment: u64,
}
impl Pcg32 {
    pub fn new(seed: u64, stream: u64) -> Self {
        let mut result = Self {
            state: 0,
            increment: (stream << 1) | 1,
        };
        result.next_u32();
        result.state = result.state.wrapping_add(seed);
        result.next_u32();
        result
    }
    pub fn next_u32(&mut self) -> u32 {
        let old = self.state;
        self.state = old
            .wrapping_mul(6364136223846793005)
            .wrapping_add(self.increment);
        let bits = (((old >> 18) ^ old) >> 27) as u32;
        bits.rotate_right((old >> 59) as u32)
    }
    pub fn unit(&mut self) -> f32 {
        (self.next_u32() >> 8) as f32 * (1.0 / 16777216.0)
    }
    pub fn bounded(&mut self, bound: u32) -> u32 {
        assert!(bound != 0);
        let threshold = bound.wrapping_neg() % bound;
        loop {
            let value = self.next_u32();
            if value >= threshold {
                return value % bound;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn published_pcg_reference_sequence() {
        let mut rng = Pcg32::new(42, 54);
        for value in [
            0xa15c02b7, 0x7b47f409, 0xba1d3330, 0x83d2f293, 0xbfa4784b, 0xcbed606e,
        ] {
            assert_eq!(rng.next_u32(), value);
        }
    }
}
