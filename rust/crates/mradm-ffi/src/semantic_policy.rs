use crate::{boundary, checked::*};
use mradm_dsp::semantic_policy::{
    self as policy, PolicyBlock, PolicyDirect, PolicyDirectBlock, PolicyGain, PolicyObject,
};

/// Applies one gain rule to each value in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_policy_gain(
    rule: *const PolicyGain,
    values: *mut f32,
    n: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(rule, 1, values, n)?;
        let rule = &input(rule, 1)?[0];
        for value in output(values, n)? {
            *value = policy::apply_gain(rule, *value);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_policy_object(
    rule: *const PolicyObject,
    sample_rate: u32,
    block: *mut PolicyBlock,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(rule, 1, block, 1)?;
        let block = &mut output(block, 1)?[0];
        flag(block.cartesian)?;
        flag(block.has_interpolation)?;
        policy::apply_object(&input(rule, 1)?[0], sample_rate, block);
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_policy_direct(
    rule: *const PolicyDirect,
    block: *mut PolicyDirectBlock,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(rule, 1, block, 1)?;
        let block = &mut output(block, 1)?[0];
        flag(block.has_position)?;
        flag(block.user_position_override)?;
        policy::apply_direct(&input(rule, 1)?[0], block);
        Ok(())
    })
}

/// Scene state finish: `src` is block gain, object gain, then azimuth/elevation/distance;
/// `dst` receives the linear gain and, when `polar != 0`, x/y/z.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_policy_scene_state(
    src: *const f32,
    n: usize,
    polar: u32,
    dst: *mut f32,
    m: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, m)?;
        let polar = flag(polar)?;
        if n != 5 || m != if polar { 4 } else { 1 } {
            return Err(invalid());
        }
        let s = input(src, n)?;
        let d = output(dst, m)?;
        d[0] = policy::scene_level(s[0], s[1]);
        if polar {
            d[1..].copy_from_slice(&policy::scene_cartesian([s[2], s[3], s[4]]));
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use mradm_dsp::semantic_policy::PolicyValue;

    #[test]
    fn rejects_overlap_and_bad_flags() {
        let rule = PolicyGain {
            scale: PolicyValue::some(0.5),
            gain_db: PolicyValue::default(),
        };
        let mut values = [1f32, 2.];
        assert_eq!(
            unsafe { mradm_dsp_policy_gain(&rule, values.as_mut_ptr(), 2) },
            0
        );
        assert_eq!(values, [0.5, 1.]);
        let mut block = PolicyBlock {
            cartesian: 2,
            ..PolicyBlock::default()
        };
        assert_eq!(
            unsafe { mradm_dsp_policy_object(&PolicyObject::default(), 48_000, &mut block) },
            1
        );
        let src = [0.5f32, 0.25, 0., 0., 2.];
        let mut dst = [0f32; 4];
        assert_eq!(
            unsafe { mradm_dsp_policy_scene_state(src.as_ptr(), 5, 1, dst.as_mut_ptr(), 4) },
            0
        );
        assert_eq!(dst[0], 0.125);
        assert_eq!(
            unsafe { mradm_dsp_policy_scene_state(src.as_ptr(), 5, 1, dst.as_mut_ptr(), 1) },
            1
        );
    }
}
