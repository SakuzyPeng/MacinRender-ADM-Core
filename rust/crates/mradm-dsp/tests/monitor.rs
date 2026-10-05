use mradm_dsp::monitor::{Callback, Crossfade, Output};
fn call(frames: usize, produced: usize, active: bool, generation: u64) -> Callback {
    Callback {
        frames,
        produced_frames: produced,
        active,
        generation,
    }
}
fn run(s: &mut Output, pcm: &mut [f32], c: Callback) -> ([f32; 64], [f32; 64]) {
    let mut p = [17.0; 64];
    let mut r = [18.0; 64];
    s.process(pcm, c, &mut p, &mut r).unwrap();
    (p, r)
}
#[test]
fn crossfade_endpoints_chunks_reset_and_isolation() {
    let mut s = Crossfade::new(2, 2048).unwrap();
    let initial = s.clone();
    assert!(!s.process(&mut [], &[], 0).unwrap());
    assert_eq!(s, initial);
    let mut actual = vec![0.25; 4100];
    let incoming = vec![0.75; 4100];
    assert!(s.process(&mut actual, &incoming, 2048).unwrap());
    for f in 0..2048 {
        assert_eq!(actual[f * 2], 0.25 + f as f32 / 4096.0);
        assert_eq!(actual[f * 2 + 1], actual[f * 2]);
    }
    assert_eq!(&actual[4096..], &[0.25; 4]);
    assert_eq!(initial, Crossfade::new(2, 2048).unwrap());
    let mut split = initial;
    let mut out = vec![0.25; 4096];
    let mut done = 0;
    for frames in [1, 0, 37, 511, 1024, 475] {
        let end = done + frames * 2;
        assert_eq!(
            split
                .process(&mut out[done..end], &incoming[done..end], frames)
                .unwrap(),
            end == 4096
        );
        done = end;
    }
    assert_eq!(out, actual[..4096]);
    let mut tail = [0.25, -0.25];
    s.process(&mut tail, &[0.75, -0.75], 1).unwrap();
    assert_eq!(tail, [0.75, -0.75]);
    s.reset();
    assert_eq!(s, Crossfade::new(2, 2048).unwrap());
}
#[test]
fn seek_endpoints_retarget_single_frame_and_all_channels() {
    let mut s = Output::new(2, 1000, true).unwrap();
    run(&mut s, &mut [0.25, -0.5], call(1, 1, true, 0));
    let mut pcm = [0.75; 20];
    run(&mut s, &mut pcm, call(10, 10, true, 1));
    assert_eq!(&pcm[..2], &[0.25, -0.5]);
    assert_eq!(&pcm[18..], &[0.75, 0.75]);
    for f in 0..10 {
        let t = f as f32 / 9.0;
        assert_eq!(pcm[f * 2], (0.25 * (1.0 - t)) + (0.75 * t));
    }
    let mut first = [0.125; 6];
    run(&mut s, &mut first, call(3, 3, true, 2));
    let mut next = [-0.25; 4];
    run(&mut s, &mut next, call(2, 2, true, u64::MAX));
    assert_eq!(&next[..2], &first[4..]);
    let mut wrap = [0.0; 2];
    run(&mut s, &mut wrap, call(1, 1, true, 0));
    assert_eq!(&wrap, &next[2..]);
    let mut one = Output::new(1, 1, true).unwrap();
    run(&mut one, &mut [0.25], call(1, 1, true, 0));
    let mut v = [0.75];
    run(&mut one, &mut v, call(1, 1, true, 1));
    assert_eq!(v, [0.75]);
    let mut many = Output::new(96, 1000, false).unwrap();
    let mut v = [0.75; 96];
    let (p, r) = run(&mut many, &mut v, call(1, 1, true, 1));
    assert_eq!(v, [0.0; 96]);
    assert_eq!(p, [0.0; 64]);
    assert_eq!(r, [0.0; 64]);
}
#[test]
fn empty_paused_and_short_callbacks_preserve_device_rules() {
    for realtime in [false, true] {
        let mut s = Output::new(1, 1000, realtime).unwrap();
        run(&mut s, &mut [0.25], call(1, 1, true, 0));
        let old = s.clone();
        let (p, r) = run(&mut s, &mut [], call(0, 0, false, 1));
        assert_eq!(s, old);
        assert_eq!((p[0], r[0]), (0.0, 0.0));
        // A post-HpTF underrun tail can contain ring-out, which is remembered by realtime sinks.
        let mut pcm = [0.75, 0.75, 0.125];
        run(&mut s, &mut pcm, call(3, 2, true, 1));
        assert_eq!(pcm[0], if realtime { 0.25 } else { 0.0 });
        let mut next = [0.75];
        run(&mut s, &mut next, call(1, 1, true, 1));
        assert_eq!(next[0], if realtime { 0.75 } else { 0.75 * (2.0 / 9.0) });
        let saved = s.clone();
        run(&mut s, &mut [], call(0, 0, true, 2));
        assert_eq!(s, saved);
        run(&mut s, &mut [0.0; 3], call(3, 0, false, 2));
        let mut post = [0.75];
        run(&mut s, &mut post, call(1, 1, true, 2));
        assert_eq!(post, [0.0]);
        // Inactive callbacks cancel a partial bridge even without another generation.
        run(&mut s, &mut [0.0], call(1, 0, false, 2));
        run(&mut s, &mut post, call(1, 1, true, 2));
        assert_eq!(post, [0.0]); // input remained zero; no synthetic fade is started
        post[0] = 0.75;
        run(&mut s, &mut post, call(1, 1, true, 2));
        assert_eq!(post, [0.75]);
        s.reset();
        assert_eq!(s, Output::new(1, 1000, realtime).unwrap());
    }
    let mut push = Output::new(1, 1000, false).unwrap();
    run(&mut push, &mut [0.75], call(1, 1, true, 1));
    let saved = push.clone();
    run(&mut push, &mut [0.0; 9], call(9, 0, true, 2));
    assert_eq!(push, saved); // neither consumes generation nor advances the pending bridge
    let mut rt = Output::new(1, 1000, true).unwrap();
    run(&mut rt, &mut [0.25], call(1, 1, true, 0));
    run(&mut rt, &mut [0.125; 2], call(2, 0, true, 1));
    let mut fresh = [0.75];
    run(&mut rt, &mut fresh, call(1, 1, true, 1));
    assert_eq!(fresh, [0.125]); // generation was deferred; anchor is what the device emitted
}
#[test]
fn metrics_padding_nonfinite_and_rejection_atomicity() {
    let mut s = Output::new(2, 48000, true).unwrap();
    let (p, r) = run(
        &mut s,
        &mut [1.0, -0.5, -1.0, 0.5, 0.0, 0.0, 0.0, 0.0],
        call(4, 2, true, 0),
    );
    assert_eq!(&p[..2], &[1.0, 0.5]);
    assert_eq!(r[0], 0.5_f64.sqrt() as f32);
    assert_eq!(r[1], 0.125_f64.sqrt() as f32);
    let (p, r) = run(
        &mut s,
        &mut [f32::NAN, f32::INFINITY, -0.5, -0.0],
        call(2, 2, true, 0),
    );
    assert_eq!(&p[..2], &[0.5, f32::INFINITY]);
    assert!(r[0].is_nan());
    assert!(r[1].is_infinite());
    s.reset();
    let before = s.clone();
    for (length, c, pn, rn) in [
        (3, call(1, 1, true, 1), 2, 2),
        (4, call(3, 3, true, 1), 2, 2),
        (4, call(2, 3, true, 1), 2, 2),
        (4, call(2, 2, true, 1), 1, 2),
        (4, call(2, 2, true, 1), 2, 1),
        (4, call(usize::MAX, 0, true, 1), 2, 2),
    ] {
        let mut pcm = [0.75; 4];
        let mut p = [17.0; 2];
        let mut r = [18.0; 2];
        assert!(
            s.process(&mut pcm[..length], c, &mut p[..pn], &mut r[..rn])
                .is_err()
        );
        assert_eq!(pcm, [0.75; 4]);
        assert_eq!(p, [17.0; 2]);
        assert_eq!(r, [18.0; 2]);
        assert_eq!(s, before);
    }
    assert!(Output::new(0, 48000, true).is_err());
    assert!(Output::new(2, 0, true).is_err());
    assert!(Output::new(usize::MAX, 48000, true).is_err());
    assert!(Crossfade::new(2, 0).is_err());
    let mut f = Crossfade::new(2, 2048).unwrap();
    let saved = f.clone();
    let mut v = [0.25; 4];
    assert!(f.process(&mut v, &[0.75; 3], 1).is_err());
    assert!(f.process(&mut v, &[0.75; 4], usize::MAX).is_err());
    assert_eq!(f, saved);
    assert_eq!(v, [0.25; 4]);
    f.process(&mut v, &[0.75; 4], 2).unwrap();
    assert_eq!(v[0], 0.25);
}
#[test]
fn seek_chunk_identity_and_independent_instances() {
    for realtime in [false, true] {
        for rate in [8000, 44100, 48000, 96000, 192000] {
            let mut s = Output::new(2, rate, realtime).unwrap();
            run(&mut s, &mut [0.125, -0.25], call(1, 1, true, 0));
            let mut other = s.clone();
            let mut full: Vec<f32> = (0..8192).map(|i| (i % 63) as f32 / 64.0 - 0.5).collect();
            let mut split = full.clone();
            run(&mut s, &mut full, call(4096, 4096, true, 1));
            let mut offset = 0;
            for frames in [0, 1, 7, 37, 511, 512, 1024, 2004] {
                let end = offset + frames * 2;
                run(
                    &mut other,
                    &mut split[offset..end],
                    call(frames, frames, true, 1),
                );
                offset = end;
            }
            assert_eq!(offset, 8192);
            assert_eq!(full, split);
            assert_eq!(s, other);
            other.reset();
            assert_ne!(s, other);
        }
    }
}
