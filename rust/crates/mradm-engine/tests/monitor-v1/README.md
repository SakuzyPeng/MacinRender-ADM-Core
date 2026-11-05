# monitor-v1

Frozen output of the C++ monitor stream factory at main `5af83cc`, before PCM consistency 5.3 B
moved its opening rules to `mradm-engine` `stream::open_monitor`. A scratchpad generator (not
committed) compiled `RealtimeStreamFactory` cut verbatim from `src/adm_engine/monitor_session.cpp`
against the `ADMEngine` of a Linux x64 config A Release build (Apple unavailable) and opened one
factory per case:

- `cases.jsonl`: 2,960 cases in a fixed order. Each names a scene of `tests/common/monitor.rs`
  (`<scene>-<rate>.wav`, one silent second) and the `RenderOptions` the factory reads: renderer
  (`RendererSelection` value), output layout, DirectSpeakers routing and matrix (a JSON text or a
  path of `../request-v1/matrices`, a missing file included), Triple Balance D mode, the reference
  and reproduction screens, system-spatial monitoring with a listener orientation, smoothing,
  default interpolation, binaural and speaker spread, a SOFA fixture, the phase-2 room package,
  split-power LFE routing, the Apple speaker geometry and the internal speaker-stereo switch.
- `expected.jsonl`: per case the log lines (level, module, message) in order, and the error (code,
  message, context) or the opened stream's channels, rate, layout, pre-roll hint, appended tail and
  whether it has a room control. The input, matrices and fixtures directories are written `<IN>`,
  `<MATRICES>` and `<FIXTURES>`.

The PCM a stream renders is not recorded (the inputs are silent); the 5.3 B verification compared
it separately between the old and new factories.
