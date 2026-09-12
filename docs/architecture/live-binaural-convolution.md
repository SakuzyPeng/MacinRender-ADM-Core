# Live binaural motion and convolution

The producer-neutral Scene renderer uses the same dynamic HRTF path for listener
rotation and object motion. Its convolution state is private to each element;
only immutable HRTF preparation is shared between renderers.

## Direction interpolation

The magnitude-preserving HRTF response at each integer angular grid point is
unchanged. Live lookup bilinearly interpolates the complex responses of adjacent
one-degree grid points, wrapping azimuth and clamping elevation. This removes the
half-degree rounding jumps without changing the batch renderer's lookup policy.
Measured-spectrum magnitudes are prepared once off the render thread and counted
against the existing 64 MiB HRTF cache budget.

## Complete filter support

Magnitude/phase interpolation is nonlinear. Its inverse N-point FFT generally
has nonzero coefficients throughout N samples, even if the measured HRIR is
shorter. The live convolver treats that complete result as an N-tap causal FIR,
zero-pads it to the next power of two at least N + maximum_block - 1, and uses
overlap-save with N - 1 samples of input history. This retains the late
coefficients and prevents circular wrap or dropped tails at render boundaries.
The conversion adds no leading delay. Draining includes N - 1 input frames plus
the diffuse delay, rather than using the original measured HRIR length.

Both filters in a transition process the same input FFT and therefore the same
history. Copying an old *filtered output tail* into a new filter's state does not
provide this property. Gain and diffuse ramps are applied per sample before the
input is saved in history; already weighted historical samples keep their values.

## Control transitions

Explicit spatial metadata ramps supply each segment's endpoint filter. A
discontinuous target, including a new head pose, uses a persistent 10 ms output
crossfade. A newer target starts from the filter currently audible, and a fade
can continue across short render calls or finish within a longer call. Initial
poses apply before the first sample. Head-locked sources bypass head rotation.

## Regression coverage

`mr_adm_live_binaural_continuity_tests` checks full FIR support against direct
convolution, arbitrary partitions and silent tails, interrupted filter fades,
sub-degree lookup and azimuth wrapping, stationary off-grid output, and pure-tone
head/object/gain motion. `MR_ADM_TEST_SOFA` optionally repeats the renderer tests
with a real SOFA dataset. The existing Scene C API and binaural fixture tests
cover reference-frame changes, independent metadata ramp deadlines, spatial
geometry, and the batch renderer's HRTF invariants.

Use Release builds for audio comparisons and timing. The pure-tone second-order
residual in the continuity test detects discontinuities; it is not a loudness or
perceptual noise measurement.
