use mradm_dsp::scene_transition::Transitions;
#[test]
fn backend_starts_at_one_and_completes_after_whole_slice() {
    let mut t = Transitions::new(2, 48000, 4).unwrap();
    let mut old = [0.; 12];
    let new = [1.; 12];
    assert!(!t.mix(&mut old, &new, 0).unwrap());
    assert!(t.mix(&mut old, &new, 6).unwrap());
    assert_eq!(
        old,
        [0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1., 1., 1., 1., 1., 1.]
    );
    assert_eq!(t.status().backend_position, 6);
    t.reset_backend();
    assert_eq!(t.status().backend_position, 0);
}
#[test]
fn generation_anchor_silence_hidden_frames_and_zero() {
    let mut t = Transitions::new(2, 400, 2048).unwrap();
    let mut first = [2., -2.];
    t.process_output(&mut first, 1, false).unwrap();
    t.begin_generation();
    let mut pcm = [0.; 8];
    t.process_output(&mut pcm, 1, false).unwrap();
    assert_eq!(&pcm[..2], &[1.5, -1.5]);
    let status = t.status();
    t.process_output(&mut [], 0, true).unwrap();
    assert_eq!(status, t.status());
    t.process_output(&mut pcm, 1, true).unwrap();
    assert_eq!(&pcm[..2], &[0., 0.]);
    assert_eq!(t.status().generation_remaining, 2);
    t.process_output(&mut pcm, 2, false).unwrap();
    assert_eq!(&pcm[..4], &[0.5, -0.5, 0., 0.]);
    t.reset();
    t.begin_generation();
    let mut next = [1.; 8];
    t.process_output(&mut next, 4, false).unwrap();
    assert_eq!(next, [0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1., 1.]);
}
#[test]
fn invalid_buffers_do_not_change_pcm_or_history() {
    let mut t = Transitions::new(3, 48000, 2048).unwrap();
    t.begin_generation();
    let before = t.status();
    let mut pcm = [13.; 7];
    assert!(t.process_output(&mut pcm, 2, false).is_err());
    assert!(t.mix(&mut pcm, &[0.; 6], 2).is_err());
    assert_eq!(pcm, [13.; 7]);
    assert_eq!(t.status(), before);
    let mut last = [1.; 3];
    let mut anchor = [2.; 3];
    t.snapshot(&mut last, &mut anchor).unwrap();
    assert_eq!(last, [0.; 3]);
    assert_eq!(anchor, [0.; 3]);
    assert!(t.process_output(&mut pcm, usize::MAX, false).is_err());
    assert_eq!(t.status(), before);
}
#[test]
fn one_frame_rate_and_nonfinite_classification() {
    let mut t = Transitions::new(1, 1, 1).unwrap();
    t.begin_generation();
    let mut pcm = [-0.];
    t.process_output(&mut pcm, 1, false).unwrap();
    assert_eq!(t.status().generation_remaining, 0);
    let mut old = [f32::INFINITY];
    t.mix(&mut old, &[1.], 1).unwrap();
    assert!(old[0].is_nan());
    t.process_output(&mut old, 1, true).unwrap();
    assert_eq!(old[0].to_bits(), 0);
}
