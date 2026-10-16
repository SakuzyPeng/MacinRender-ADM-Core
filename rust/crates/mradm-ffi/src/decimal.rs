//! Private boundary for locale-independent decimal parameters (PCM consistency 3.5).
use crate::{boundary, checked::*};
use mradm_dsp::decimal;

/// Parses `length` UTF-8 bytes of `text` as a finite decimal into `value`. With `single` set the
/// text is rounded once to binary32 (and widened exactly); otherwise to binary64.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_parse_decimal(
    text: *const u8,
    length: usize,
    single: u32,
    value: *mut f64,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        let text = std::str::from_utf8(input(text, length)?).map_err(|_| invalid())?;
        let parsed = if single != 0 {
            decimal::parse_f32(text).map(f64::from)
        } else {
            decimal::parse_f64(text)
        };
        output(value, 1)?[0] = parsed.ok_or_else(invalid)?;
        Ok(())
    })
}
