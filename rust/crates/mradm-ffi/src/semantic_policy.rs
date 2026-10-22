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
    }
}
