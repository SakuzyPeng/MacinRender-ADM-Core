# Stereo device peak protection

Custom HRTFs and coherent object sums can produce valid floating-point binaural
PCM above 0 dBFS. Miniaudio's default float32 device path clips callback output to
[-1, 1]. Without output protection, a render can therefore have zero underruns
while loud passages produce hard-clipping distortion. Device or OS volume after
that clip cannot restore the waveform.

`SceneOutputSession` protects its software stereo device path with
`StereoPeakGuard`. The rendered Scene stream remains floating point and unbounded
for offline callers. System-spatial output passes multichannel PCM to the system
spatializer and does not use this stereo stage.

The guard applies the current master volume before detecting overload. Signals
that remain below its -1 dBFS sample-peak ceiling pass through exactly at that
volume. Both ears share a gain envelope to preserve the stereo balance. Each
future overload imposes a linear attack ramp over a 5 ms lookahead; the minimum
of these ramps reaches the required gain at the peak without block-level gain
steps. Recovery uses a 100 ms exponential release. This is sample-peak protection,
not an oversampled true-peak limiter.

All buffers are allocated before the device starts. The callback only reads the
Scene ring and processes bounded buffers. Prefetch is limited to available input
to avoid counting speculative lookahead reads as underruns. Media consumption and
presentation count emitted frames, not prefetched frames. EOS flushes the final
lookahead without adding padding; pause retains it and a new epoch clears both
the buffered samples and gain envelope.

`mr_adm_stereo_peak_guard_tests` covers transparent low-volume playback, linked
and smooth gain, callback partitioning, mute, short EOS, reset, and actual output
session pause/epoch/presentation accounting through a capture device. Optional
`MR_ADM_TEST_STEREO_PCM` and `MR_ADM_TEST_STEREO_OUTPUT` paths allow Release tests
against local interleaved float32 stereo captures without committing media.
