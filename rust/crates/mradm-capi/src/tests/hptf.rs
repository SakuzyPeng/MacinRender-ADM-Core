//! `adm_hptf_parse_parametric_eq` into caller buffers (from `hptf_parameters_test`): the sizing
//! query, capacity and stride failures that leave every output alone, the parsed bands as written,
//! future output strides and diagnostics. The setters on a Scene output need the null output
//! device and run in the C consumer.
use super::common::*;
use crate::context::adm_context_last_error_message;
use crate::monitor::adm_hptf_parse_parametric_eq;
use crate::scene::adm_hptf_band_t;
use std::ffi::CStr;
use std::ptr;

const PROFILE: &CStr = c"Preamp: -6 dB\nFilter 1: ON LSC Fc 105 Hz Gain 3 dB Q 0.7\nFilter 2: ON PK Fc 1000 Hz Gain -2 dB Q 1.5\nFilter 3: OFF HSC Fc 10000 Hz Gain 1 dB Q 0.7\n";
const PEAKING: i32 = 0;
const LOW_SHELF: i32 = 1;
const HIGH_SHELF: i32 = 2;

fn band(fc: f64) -> adm_hptf_band_t {
    adm_hptf_band_t {
        struct_size: size_of::<adm_hptf_band_t>() as u32,
        r#type: PEAKING,
        enabled: 1,
        reserved_v1_38: 0,
        fc_hz: fc,
        gain_db: 3.0,
        q: 1.0,
    }
}

fn parse(
    context: &Context,
    text: &CStr,
    preamp: &mut f64,
    bands: *mut adm_hptf_band_t,
    capacity: u32,
    count: &mut u32,
) -> i32 {
    unsafe {
        adm_hptf_parse_parametric_eq(context.0, text.as_ptr(), preamp, bands, capacity, count)
    }
}

#[test]
fn parametric_eq_text_parses_into_caller_buffers() {
    let context = Context::new();
    let (mut preamp, mut count) = (99.0, 99u32);
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            ptr::null_mut(),
            0,
            &mut count
        ),
        OK
    );
    assert_eq!(
        (count, preamp),
        (3, -6.0),
        "the sizing query counts disabled bands too"
    );
    let mut bands = [band(1000.0), band(1000.0), band(1000.0)];
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            bands.as_mut_ptr(),
            2,
            &mut count
        ),
        INVALID_ARGUMENT
    );
    assert_eq!(count, 3, "too small a capacity reports the size needed");
    assert!(
        bands[0].fc_hz == 1000.0 && bands[1].gain_db == 3.0,
        "without partial output"
    );
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            bands.as_mut_ptr(),
            3,
            &mut count
        ),
        OK
    );
    let expected = [
        (LOW_SHELF, 1, 105.0, 3.0, 0.7),
        (PEAKING, 1, 1000.0, -2.0, 1.5),
        (HIGH_SHELF, 0, 10000.0, 1.0, 0.7),
    ];
    for (actual, want) in bands.iter().zip(expected) {
        assert_eq!(actual.struct_size as usize, size_of::<adm_hptf_band_t>());
        assert_eq!(
            (
                actual.r#type,
                actual.enabled,
                actual.fc_hz,
                actual.gain_db,
                actual.q
            ),
            want
        );
    }
    bands[1].struct_size = 4;
    (count, preamp) = (77, 77.0);
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            bands.as_mut_ptr(),
            3,
            &mut count
        ),
        INVALID_ARGUMENT
    );
    assert_eq!(
        (count, preamp, bands[0].fc_hz),
        (77, 77.0, 105.0),
        "an invalid stride writes nothing"
    );
    assert_eq!(
        parse(
            &context,
            c"Preamp: bad",
            &mut preamp,
            ptr::null_mut(),
            0,
            &mut count
        ),
        INVALID_ARGUMENT
    );
    assert_eq!(
        (count, preamp),
        (77, 77.0),
        "a parse failure writes nothing"
    );
    assert_ne!(
        unsafe { *adm_context_last_error_message(context.0) },
        0,
        "with a diagnostic"
    );
    unsafe {
        assert_eq!(
            adm_hptf_parse_parametric_eq(
                ptr::null_mut(),
                PROFILE.as_ptr(),
                &mut preamp,
                ptr::null_mut(),
                0,
                &mut count
            ),
            INVALID_ARGUMENT
        );
    }
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            ptr::null_mut(),
            1,
            &mut count
        ),
        INVALID_ARGUMENT
    );
}

/// A caller built against a larger band record gets its stride honoured and its extension bytes
/// and sizes kept.
#[test]
fn parametric_eq_output_honours_a_future_stride() {
    #[repr(C)]
    struct Future {
        value: adm_hptf_band_t,
        tail: [u64; 2],
    }
    const TAIL: [u64; 2] = [0x1234_5678_9abc_def0, 0xfedc_ba98_7654_3210];
    let future = || {
        let mut value = band(0.0);
        value.struct_size = size_of::<Future>() as u32;
        Future { value, tail: TAIL }
    };
    let mut bands = [future(), future(), future()];
    let context = Context::new();
    let (mut preamp, mut count) = (0.0, 0u32);
    assert_eq!(
        parse(
            &context,
            PROFILE,
            &mut preamp,
            &mut bands[0].value,
            3,
            &mut count
        ),
        OK
    );
    for b in &bands {
        assert_eq!(
            (b.value.struct_size as usize, b.tail),
            (size_of::<Future>(), TAIL)
        );
    }
    assert_eq!((bands[2].value.fc_hz, count, preamp), (10000.0, 3, -6.0));
}
