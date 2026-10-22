# 可复用距离变化函数 v1

本工具提供可参数化的刚性球 DVF，以及结合目标听者 HRTF 的近场合成。
公共模型不包含 SkinRLC 的个人频谱系数。个人多距离数据用于开发验证和选择处理方式。

输出是复数频率响应 NPZ，可供算法研究或后续滤波器设计使用。
当前不生成可直接加载的 SOFA HRIR，也未接入 MacinRender 的渲染流程。
NPZ 中的复数响应不能直接当作因果 FIR；转换时还需要处理公共保护延时、
DC/Nyquist、频率采样、线性卷积长度和尾部能量，并进行独立验证。

## 方法

`S(f, Ω, r)` 表示相对于头中心自由场压力归一化的刚性球响应。
纯解析距离修正为：

```text
D(f, Ω, r; r0) = S(f, Ω, r) / S(f, Ω, r0)
```

默认等效头半径为 87.5 mm，耳位置为左 +90°、右 −90°，声速为 343.18 m/s。
头半径和双耳方位/仰角均可修改；默认值不来自用户的个人模型拟合。
API 直接计算连续距离和方向，无需在 9 个距离层之间插值。
解析实现使用缩放后的 Hankel 多项式递推，包含有限距离的 DC 极限，
不直接计算低频下可能溢出的高阶球 Hankel 函数。

输入一个目标听者在距离 `r0` 的 HRTF 后，可选三种模式：

- `sphere`：在原方向上乘解析 DVF。
- `parallax`：先除去参考刚性球响应，再按每只耳朵的入射方向插值目标听者的残余频谱，最后乘目标距离的刚性球响应。
- `hybrid`（有 SOFA 输入时默认）：2 kHz 以下采用 `sphere`，4 kHz 以上采用 `parallax`，中间以升余弦权重混合残余频谱的对数幅度和解缠后的相对相位。

`parallax` 的表达式为：

```text
H_est(f, Ω, r) = S(f, Ω, r) × interpolate[H_target(f, Ω, r0) / S(f, Ω, r0)](Π_e(Ω, r, r0))
```

`Π_e` 将目标源相对耳朵的射线与参考距离球面相交。
它使用实际参考距离；不会把 1.5 m 默认为无穷远。
角度插值使用封闭球面三角网格、对数幅度和局部对齐/解缠相位；不对缺失半球外推。
这与 SUpDEq 的球谐插值实现不同，不能将其精度结论直接移用到本工具。

模型负责归一化的头部近场变化。输入已有的公共增益、EQ 和延时保留；
不增加公共 `r0/r` 衰减或 `(r-r0)/c` 传播时间，也不做左右耳/逐方向响度归一化。
对其他 SOFA，参考距离从该文件读取，不应直接套用以 1.5 m 为参考的修正。

## 使用

使用已有 Python 环境即可。解析核心只依赖 NumPy；目标 HRTF 合成使用 SciPy，
SOFA 读取使用 h5py 和仓库现有的 `scripts/room/prepare-hrir.py`。
可视化验证额外需要 matplotlib。无需独立虚拟环境或大型缓存。

以下命令从仓库根目录执行，输出文件必须尚不存在。

生成不含个人数据的示例 DVF 包：

```sh
python3 -B scripts/research/hrtf_dvf/dvf.py \
  --output local/reusable-dvf-v1/reusable_dvf.npz
```

包中包含 9 个距离（0.2、0.3、0.4、0.5、0.75、1、1.3、1.5、1.6 m），
62 个方向，以及 DC 和 20 Hz–24 kHz 的 128 个对数频率采样点。
这是 API 的轻量演示采样，不能把这 62 个方向当成高频生产插值网格。

用另一个听者的单距离 HRIR SOFA 合成近场复数响应：

```sh
python3 -B scripts/research/hrtf_dvf/dvf.py \
  --source /absolute/path/to/reference_hrir.sofa \
  --distances 0.2 0.5 \
  --head-radius 0.0875 \
  --ear-azimuths 90 -90 \
  --mode hybrid \
  --output local/reusable-dvf-v1/target_hrtf.npz
```

输入支持单距离、固定听者的 `SimpleFreeFieldHRIR` / `GeneralFIR` FIR SOFA。
源位置坐标和听者坐标系由已有严格读取器解析，双耳顺序自动判定，
也可用 `--ear-order lr` 或 `--ear-order rl` 明确指定。
`Data.Delay` 必须采用样本单位，并完整转换为相位。
默认 FFT 长度为覆盖全部原始 HRIR 的最小 2 的幂，`--fft-size` 可指定其他长度，
但不能截掉输入尾部。没有重采样或能量裁剪。

不直接读取 TF SOFA：Mesh2HRTF 的数值频域文件可能使用与 NumPy FFT 相反的相位约定，
仅凭一般 SOFA 标头不能安全猜测。使用 FIR 输入可明确相位符号。

直接调用 Python API：

```python
import sys
import numpy as np
sys.path.insert(0, "scripts/research/hrtf_dvf")
from dvf import HeadModel, distance_variation, spherical_directions, Synthesizer

head = HeadModel(radius_m=0.0875)
directions = spherical_directions([[0, 0], [90, 0], [-90, 0]])
f = np.array([0., 100., 500., 1000., 4000., 16000.])
D = distance_variation(f, directions, target_distance_m=0.35,
                       reference_distance_m=1.0, model=head)
# D.shape == (3, 2, 6)，耳顺序为左、右。

# 对完整球面目标数据：
# synth = Synthesizer(frequency_hz, directions_xyz, target_H, reference_distance_m, head)
# H_at_035m = synth.at(0.35, mode="hybrid")
```

数组契约：

| 字段 | 内容 |
|---|---|
| `frequency_hz` | 严格递增的频率，包含或不包含 DC 均可 |
| `directions_xyz` | 单位向量，x 向前、y 向左、z 向上 |
| `distance_m` | 从头中心起算的目标距离 |
| `response` | `complex64`，维度为距离 × 方向 × 耳 × 频率 |
| `metadata_json` | Unicode JSON；模式、头模型、参考距离、相位约定和源文件哈希等 |

通过 `np.load(path, allow_pickle=False)` 读取。无 `--source` 时响应为 DVF；
有 `--source` 时响应为合成 HRTF，具体由 `metadata_json.response_kind` 区分。
相位采用 NumPy FFT 约定：正延时 `t` 对应 `exp(-i 2π f t)`。
这些频率响应不一定满足某个有限长度实 FIR 的 Nyquist/因果约束。

数值接口范围：等效头半径 0.04–0.12 m，声速 300–380 m/s，
源距离 `max(0.15 m, 1.25 × 头半径)` 至 100 m，频率 0–48 kHz。
这只是实现允许范围；当前真实几何开发验证覆盖 0.2–1.6 m、100 Hz–20 kHz。
没有躯干碰撞检测，因而较近的下方声源可能落入真实身体内部。

## 验证

```sh
python3 -B tests/unit/hrtf_dvf_test.py
ctest --preset debug -R '^mr_adm_hrtf_dvf_tests$' --output-on-failure
```

CTest 使用已有的 `MR_ADM_ROOM_PREP_PYTHON` 环境，在 NumPy/SciPy/h5py 可用时注册。
测试包括独立入射/散射分波展开、DC 闭式极限、额外阶数收敛、
参考距离恒等性、距离变换组合、左右镜像、有限参考距离视差几何、
目标频谱保留、相位绕回、混合频段边界、SOFA 延时以及导出/覆盖保护。

用原始多距离数据进行开发验证：

```sh
python3 -B scripts/research/hrtf_dvf/validate.py \
  --collection /absolute/path/to/SkinRLC_48k_AllDistances_v1 \
  --output local/reusable-dvf-v1/validation
```

验证读取数据本身，不复制 SOFA。检查每个 RAW HRTF/HRIR 的 SHA-256，
利用已记录的 42 样本公共偏移验证两者的相位对应关系；Nyquist 的实数化单独排除。
0.2 m 的 157 个及 0.3 m 的 37 个已有 DVF 合成方向不参与评估，
0.3 m 的 3 个已有尾部能量复查方向也排除。

2026-10-14，使用固定 87.5 mm 头模型在 SkinRLC 上得到以下开发集结果。
每个方向/耳朵等权，频段内每个 100 Hz 采样点等权；数值为每方向/耳频谱 RMS 误差的中位数。

| 距离 | 100 Hz–2 kHz：不修正 → hybrid | 8.1–16 kHz：不修正 → hybrid |
|---|---|---|
| 0.2 m | 3.68 → 1.96 dB | 7.63 → 3.48 dB |
| 0.5 m | 1.39 → 1.00 dB | 3.64 → 2.58 dB |
| 1.0 m | 0.35 → 0.27 dB | 1.68 → 1.54 dB |

0.2 m 下 hybrid 的相应 95 分位误差仍约为 4.69 dB 和 8.50 dB，
不能以中位数推断所有方向都有相同改善。
低频相位斜率产生的耳间时差误差中位数约从 50.2 μs 降至 30.1 μs；
该量不是基于到达时刻或听觉实验的 ITD 测量。

hybrid 的处理方式是在查看这套数据后选择的，因此报告明确标为开发集证据。
没有拟合个人频率系数，但也尚无未见听者、多人体数据或试听验证。
耳廓细节由目标 HRTF 提供；头形、躯干和肩部的近场变化仍是模型误差来源。

## 参考

- Kan, Jin, van Schaik (2009), *A psychophysical evaluation of near-field head-related transfer functions synthesized using a distance variation function*, DOI [10.1121/1.3081395](https://doi.org/10.1121/1.3081395)。
- Arend & Pörschmann (2019), [Synthesis of Near-Field HRTFs by Directional Equalization of Far-Field Datasets](https://audiogroup.web.th-koeln.de/PUBLIKATIONEN/Arend_DAGA2019.pdf)，含勘误版本。
- [SUpDEq v5.0.0 DVF 接口](https://audiogroup.web.th-koeln.de/SUpDEq_doc/SUpDEq-v5.0.0/supdeq_dvf.html)，用于方法和参数对照。本工具是 Python/NumPy 独立实现，角度插值不使用该实现的球谐算法。
