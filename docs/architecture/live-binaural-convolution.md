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


## HRTF 准备与几何缓存

`hrtf_hull.cpp` 保留 SAF 3D hull 的扰动、插入及面顺序，以边邻接表代替 horizon 的全表成员扫描，
并复用分配缓冲。`hrtf_grid.cpp` 对完整 361 × 181 查询网格建立只读包围盒树，按原面顺序
对候选点调用 SAF 点积判定，直接形成三个测量方向及权重；dummy 极点、截断阈值及两次
归一化规则不变。失败的快速三角化可以回退原 SAF 路径。

压缩网格由独立进程内 LRU 持有，最多八组且键与表合计不超过 16 MiB，按有序 float 位模式
做哈希及完整比较。同一几何的并发 miss 通过单独构建锁合并，缓存锁不覆盖计算。
`BinauralState` 共享不可变表；原有完整 HRTF 缓存仍为四组、64 MiB，不共享卷积或对象历史。
超过完整缓存预算的大型 SOFA 仍可命中网格缓存，重新生成其频域及幅度数据。

`mr_adm_hrtf_grid_tests` 在相同随机序列下对照原 SAF 三角化和密集 VBAP，实现全部 65,341 格的
索引与权重比较，并验证并发命中、坐标顺序、LRU 预算和共享对象寿命。设置
`MR_ADM_TEST_SOFA_PATH` 可对本地 SOFA 运行同样的完整对照。性能以 Release 构建测量。
