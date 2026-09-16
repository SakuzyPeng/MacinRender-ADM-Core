# HpTF headphone compensation

Binaural monitoring puts the headphone's own frequency response between the
renderer and the listener. Judging a mix through uncompensated headphones means
judging it through that colouration. AutoEq publishes measured compensation
curves for a large number of models; this feature applies one of them, live, to
the headphone feed.

The first release supports **AutoEq's `ParametricEQ.txt`** only, on the **two
realtime paths**. Offline rendering and the GUI are deliberately out of scope.

## Scope: HpTF is device-bound

HpTF applies only to the final two-channel PCM handed to headphones. It is the
same boundary `StereoPeakGuard` draws, for the same reason:

| Path | HpTF | Why |
|---|---|---|
| `MonitorEngine`, 2ch | yes | this is the headphone feed |
| `SceneOutputSession`, stereo device | yes | same |
| Multichannel speaker output | no | not a headphone signal |
| System spatial (macOS ASBR / Windows ISpatialAudioClient) | no | we hand the OS a multichannel bed and it renders HRTF itself — the final 2ch never exists on our side |
| Bare `adm_scene_stream_pull` | no | the caller does its own output and must not be handed a silently EQ'd signal |

The unsupported cases return `ErrorCode::unsupported` rather than ignoring the
request, so a UI can surface why the control is unavailable. There is
deliberately **no `RenderOptions` field**: one would leak into `adm_render_file`
and the CLI and contaminate offline masters with a listener-side correction.

## Why ParametricEQ first, and not the minimum-phase `.wav`

AutoEq also ships a minimum-phase FIR. Measured on the Sony MDR-MV1 48 kHz file:
4800 taps, 2 channels, 16-bit, and L identical to R sample for sample. Truncation
error against the full response is 0.80 dB at 512 taps, 0.51 dB at 1024 and
0.10 dB at 2048 — concentrated at low frequencies, and windowing makes it worse,
not better (the error is low-frequency truncation, not edge ripple). Past tap
2048 the file is 16-bit quantisation noise at −77 dB.

So the shortest usable length is around 2048 taps, which needs partitioned FFT
convolution on the audio callback. `mr_adm_render_common` has zero third-party
dependencies and ADR 0003 confines SAF to the vbap/hoa/binaural modules, so that
path forces an architectural decision. A 10-biquad cascade is ~60 flops/frame
with no dependencies at all. The two are not in the same class, hence the split.

Note for whoever picks the FIR path up: the two formats are **not the same
curve**. The FIR and the 10-biquad cascade (with its −4.1 dB preamp) differ by
about 1 dB broadband and 3.2 dB at 20 kHz. Shipping both without level-matching
them will generate a steady stream of "format X sounds better" reports that are
really just level differences.

## Signal chain placement

### MonitorEngine

The cascade runs in `pull()`, after the mode branch and **before**
`apply_seek_transition`. That single position satisfies four separate
constraints:

- **Before the de-click bridge.** `seek_transition_anchor_` is taken from
  `last_output_frame_`, which `apply_seek_transition` captures from the output
  buffer itself. Filtering afterwards would leave the anchor holding pre-HpTF
  audio while post-HpTF audio is what plays, so every seek would step by the
  cascade's instantaneous response.
- **After both LUFS taps** — the worker-side `feed_meter` in single-stage mode,
  and `meter_ring_` inside `pull_output_stage` for output-stage mode. Program
  loudness must not change when you swap headphones, so the meter reads pre-HpTF
  in both modes. The placement gives this for free.
- **Before the peak/RMS loop**, which is correct: those are device clipping
  indicators and should reflect what the device actually receives.
- **Skipped while inactive.** That branch writes exact digital silence, and
  running a biquad cascade over zeros would leak its ring-out into it.

State is reset on seek (in `apply_seek_locked`, where the callback is already
parked by the `flushing_`/`in_pop_` handshake) but deliberately **not** on pause:
pause neither clears the ring nor moves the playhead, so the first sample after
resume is the immediate successor of the last filtered one and the filter must
stay continuous across it. Loop wrap is likewise not a reset — the loop point is
assumed musically continuous.

### SceneOutputSession

Applied in `pull_stereo()` between the stream pull and `peak_guard_->push()`.
Being **upstream of the peak guard** is the whole point: a boosted band is then
caught by the −1 dBFS ceiling instead of escaping it. State resets in
`begin_epoch`, next to the guard's own reset and under the same `park()`.

## Preamp safety

AutoEq's `Preamp:` line exists to keep the combined response from clipping, but a
hand-edited or non-AutoEq file carries no such guarantee. `HptfPreampMode`
controls what happens when the designed cascade still peaks above 0 dB:

- `warn_only` (**default**) uses the file's value verbatim and logs a warning.
- `auto_trim` attenuates against a conservative bound on the audible-band magnitude
  response. This is a frequency-response bound, not a transient/sample-peak limiter.

`max_response_db` is searched over the **audible band, 20 Hz – 20 kHz**, and this
is a deliberate choice rather than an oversight. On the MDR-MV1 the +9.7 dB
105 Hz low shelf keeps rising once the 46 Hz dip stops opposing it: −0.52 dB at
20 Hz, +2.93 dB at 10 Hz, +4.75 dB at 5 Hz. Counting the infrasonic region would
make `auto_trim` pull the whole curve down further. The final stereo peak guard
still sees and protects against infrasonic overloads. AutoEq computes its own
preamp over the audible band; matching that convention is what makes our measured
−0.098 dB agree with its nominal −0.1 dB.

The peak search includes the extrema of every quantised biquad: squared magnitude
is a ratio of quadratics in `sin(w/2)^2`, whose derivative has at most two roots.
The shifted variable avoids cancellation near DC when float coefficients move a
low-frequency peak away from its requested center.
Within each interval, summing the individual section maxima gives a conservative
bound for the cascade. The highest remaining interval is bisected until the bound
is within 0.001 dB of an evaluated response, or the preparation work budget is
reached. In the latter case the remaining upper bound is retained. This covers
narrow peaks between grid points and overlapping bands without relying on a fixed
512-point scan. The resulting attenuation is rounded downward.

Numeric tokens must be consumed in full. Missing optional fields may use defaults;
an explicitly malformed `Gain`, `Q`, or `Preamp` is an error. Design validates finite
representable gains and coefficients, and checks the Jury stability conditions on
the float coefficients that will actually run. Failure preserves the existing DSP.

## Hot-swapping without clicks

Changing profile runs the outgoing and incoming cascades **in parallel** over a
2048-frame (≈43 ms) window and blends linearly between them.

Coefficient interpolation is not an option: two profiles need not have the same
section count, interpolating `a1`/`a2` can traverse the unstable region, and
more fundamentally there is no correspondence between the states of two different
transfer functions, so state cannot be directly migrated. Both branches process the
same new input. The incoming branch starts with zero history and zero weight;
high-Q filters may take longer than the fade window to settle, so the transition
does not claim to reconstruct an unlimited pre-switch input history.

The blend is **linear, not equal-power**. Both cascades see the same input so
their outputs are strongly correlated; a square-root law would bulge about +3 dB
at the midpoint. This is the opposite of the usual rule for crossfading two
unrelated sources.

The window length is set by the ring-down of the bands being faded, not by click
perception — any continuous blend is click-free, but a 46 Hz low-Q band and a
105 Hz shelf ring for tens of milliseconds, and a shorter window produces an
audible spectral wobble instead of a smooth morph. The value matches
`MonitorEngine`'s stream crossfade constant so a simultaneous backend + profile
switch behaves predictably.

Bypass is modelled as zero bands **and unit preamp gain**, so enabling, disabling
and switching profiles are one code path with one blend. The steady bypass state
is an **exact short-circuit** (no multiply by 1.0), which keeps an unused HpTF
stage bit-identical to no HpTF stage at all.

A profile with no enabled bands and a non-unit preamp still applies that gain.
Its status is enabled, and the reported preamp belongs to the applied revision.
On seek, reset adopts the latest pending profile, or the incoming branch if a fade
was already in flight, then clears both histories. It never consumes and loses a
profile merely because playback changed epoch.

Only one swap is in flight at a time: a profile arriving mid-blend replaces the
pending slot and is picked up when the current blend finishes. Worst-case apply
latency is therefore two blend windows.

## Threading

Parsing and coefficient design run synchronously on the caller's thread, so a bad
file returns an error and never disturbs audio. The callback only ever reads
prepared coefficients.

Each direction uses a preallocated three-slot mailbox. Producer and consumer own
one private slot each; an atomic exchange transfers ownership of the middle slot.
The producer can replace a pending update without overwriting the consumer's slot.
No audio-thread retry, allocation, mutex or spin is needed.

One mailbox carries requested coefficients to the callback. A second carries a
complete applied snapshot back: coefficients, original preamp and revision move
together. Control queries consume this snapshot rather than read the live cascade
while it is being swapped. Small control-side mutexes serialize multiple publishers
or pollers; the audio thread never acquires them. Seek reset publishes through the
same path while the callback is parked by the existing output handshake.

## Numerics

Transposed direct form II, **float coefficients with double state**. AutoEq
profiles routinely carry bands at 20–46 Hz; at 48 kHz those poles sit close
enough to the unit circle that float32 state loses precision and can accumulate
DC under a large positive gain. Double state costs 320 bytes for a 10-section
stereo cascade and removes that whole class of bug.

Two per-block (not per-sample) guards: the state is zeroed if any word is
non-finite, since one NaN poisons an IIR permanently, and zeroed if every word is
below 1e-20, to sweep out denormals — a decaying high-Q low-frequency band sits
in denormal range for a long time and denormal arithmetic can cost ~100× on some
x86 paths. FTZ/DAZ is deliberately not touched: `pull()` runs on a host-owned
thread.

HpTF is monitor-only and never reaches an offline master, so **bit-exactness
across platforms is not a requirement** and the consistency tooling does not need
a checkpoint for it.

## API

C++: `MonitorSession::set_hptf` / `hptf_info`, and
`SceneOutputSession::set_hptf_profile` / `hptf_info`. An empty path disables.

C ABI v1.37: `adm_hptf_config_t`, `adm_hptf_info_t`, and
`adm_monitor_set_hptf` / `adm_monitor_get_hptf_info` /
`adm_scene_output_set_hptf` / `adm_scene_output_get_hptf_info`. `NULL` or `""`
for `profile_path` disables, matching the `sofa_path` convention. Availability is
reported through the `hptf` flag in the `features` object of both
`mradm.output-formats` and `mradm.render-support-matrix`, so a GUI never hard-codes
it.

## Tests

`mr_adm_hptf_tests` covers parsing (including `OFF` lines, missing `Preamp`,
absent `Gain` on `LP`/`HP` lines, unknown types, CRLF and a C-locale decimal
point), frequency response, the preamp switch, bypass bit-identity and the
hot-swap. Response correctness is asserted in the strong form: the running
filter's impulse response, evaluated by a single-point DFT, must match the
closed-form `cascade_magnitude_db` — which is the same formula that drives
`auto_trim` — and the absolute scale is anchored on externally verified MDR-MV1
values.

`mr_adm_realtime_tests` adds the MonitorEngine regression locks: the cascade
reaches the device feed, an unused HpTF stage is bit-identical to none, LUFS is
unaffected while peak moves, seeking does not click, paused output is exact
silence, and the compensation survives a backend switch.

`mr_adm_stereo_peak_guard_tests` pins the Scene insertion point: with a +12 dB
shelf on hot material every captured sample still satisfies the guard's ceiling,
which fails immediately if the stage is ever moved downstream of the limiter.

`mr_adm_hptf_regression_tests` covers preamp-only processing, seek during an active
fade and with a newer pending target, malformed/overflowing inputs, off-grid narrow
peaks, independent impulse-response validation, and concurrent mailbox publication
and applied-state queries. The concurrency cases also run under ThreadSanitizer;
Apple's runtime needs `ignore_interceptors_accesses=0` to inspect the POD memcpy
operations used by the mailboxes.
