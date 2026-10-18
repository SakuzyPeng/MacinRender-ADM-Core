#!/usr/bin/env python3
"""Prepare one single-radius SOFA listener for the native room renderer.

No direction/ear normalization or energy-based trimming is performed. All
resampling and Data.Delay support is retained, with common causal guards reported
separately from the runtime interpolation guard. Runtime playback needs no Python.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile

import h5py
import numpy as np
import scipy
from scipy.signal import firwin, upfirdn
from scipy.spatial import ConvexHull, QhullError

RATE = 48000
MAX_BYTES = 256 * 1024 * 1024


def text(value):
    if isinstance(value, np.ndarray):
        if value.size != 1:
            raise ValueError("Expected a scalar SOFA attribute")
        value = value.reshape(-1)[0]
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


def finite(value, name):
    result = np.asarray(value)
    if result.dtype.kind not in "fiu":
        raise ValueError(f"{name} must contain real numeric values")
    result = result.astype(np.float64, copy=False)
    if not np.isfinite(result).all():
        raise ValueError(f"Nonfinite {name}")
    return result


def positions(file, name, default=None):
    if name not in file:
        if default is None:
            raise ValueError(f"Missing {name}")
        return np.asarray(default, dtype=np.float64).reshape(-1, 3)
    dataset = file[name]
    values = finite(dataset[...], name)
    # SOFA receiver coordinates have R,C,I dimensions; only a fixed listener is supported.
    if name == "ReceiverPosition" and values.ndim == 3:
        if values.shape[1:] != (3, 1):
            raise ValueError("ReceiverPosition must describe two fixed ears")
        values = values[:, :, 0]
    if values.ndim != 2 or values.shape[1] != 3:
        raise ValueError(f"{name} must have three coordinate columns")
    kind = text(dataset.attrs.get("Type", "cartesian")).lower()
    units = [unit.strip() for unit in text(dataset.attrs.get("Units", "metre")).lower().split(",")]
    metres = {"metre", "meter", "metres", "meters", "m"}
    if kind == "cartesian":
        if len(units) not in (1, 3) or any(unit not in metres for unit in units):
            raise ValueError(f"{name} Cartesian coordinates must use metres")
        return values
    if (kind != "spherical" or len(units) != 3 or any(unit not in {"degree", "degrees", "deg"} for unit in units[:2])
            or units[2] not in metres or np.any(values[:, 2] < 0)):
        raise ValueError(f"Unsupported {name} coordinates/units")
    az, el = np.radians(values[:, :2]).T
    return np.column_stack((np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el))) * values[:, 2:]


def fixed(values, name):
    if len(values) == 0 or not np.allclose(values, values[:1], rtol=0, atol=1e-10):
        raise ValueError(f"{name} varies: only one fixed listener is supported")
    return values[0]


def load_sofa(path, ear_order="auto"):
    if ear_order not in ("auto", "lr", "rl"):
        raise ValueError("Ear order must be auto, lr or rl")
    with h5py.File(path, "r") as file:
        convention = text(file.attrs.get("SOFAConventions", ""))
        if convention not in ("SimpleFreeFieldHRIR", "GeneralFIR") or text(file.attrs.get("DataType", "")) != "FIR":
            raise ValueError("Only SimpleFreeFieldHRIR/GeneralFIR FIR files are supported")
        data = file.get("Data.IR")
        if data is None or data.ndim != 3 or not 4 <= data.shape[0] <= 65536 or data.shape[1] != 2 or not 1 <= data.shape[2] <= 4096:
            raise ValueError("Data.IR must be M x 2 x N, with 4..65536 directions and 1..4096 taps")
        if math.prod(data.shape) * 8 > MAX_BYTES:
            raise ValueError("SOFA exceeds the preparation memory budget")
        ir = finite(data[...], "Data.IR")
        if "EmitterPosition" in file:
            emitter = finite(file["EmitterPosition"][...], "EmitterPosition")
            if emitter.size != 3 or np.any(emitter != 0):
                raise ValueError("Only a single emitter at SourcePosition is supported")
        rates = finite(file["Data.SamplingRate"][...], "Data.SamplingRate").reshape(-1)
        if not len(rates) or np.any(rates != rates[0]) or not 1 <= rates[0] <= 384000 or rates[0] != int(rates[0]):
            raise ValueError("One positive integer sampling rate up to 384 kHz is required")
        if text(file["Data.SamplingRate"].attrs.get("Units", "hertz")).lower() not in ("hertz", "hz"):
            raise ValueError("Data.SamplingRate must use hertz")
        listener = fixed(positions(file, "ListenerPosition", [[0, 0, 0]]), "ListenerPosition")
        forward = fixed(positions(file, "ListenerView", [[1, 0, 0]]), "ListenerView")
        up = fixed(positions(file, "ListenerUp", [[0, 0, 1]]), "ListenerUp")
        if np.linalg.norm(forward) < 1e-9 or np.linalg.norm(up) < 1e-9:
            raise ValueError("Invalid listener orientation")
        forward, up = forward / np.linalg.norm(forward), up / np.linalg.norm(up)
        if abs(np.dot(forward, up)) > 1e-6:
            raise ValueError("ListenerView and ListenerUp must be orthogonal")
        left = np.cross(up, forward)
        left /= np.linalg.norm(left)
        up = np.cross(forward, left)
        source = positions(file, "SourcePosition")
        if len(source) != len(ir):
            raise ValueError("SourcePosition must contain one direction per measurement")
        relative = (source - listener) @ np.stack((forward, left, up)).T
        radii = np.linalg.norm(relative, axis=1)
        if np.any(radii <= 1e-6) or not np.allclose(radii, radii[0], rtol=1e-5, atol=1e-6):
            raise ValueError("Only a single nonzero source radius is supported")
        directions = relative / radii[:, None]
        if len(np.unique(np.round(directions, 11), axis=0)) != len(ir):
            raise ValueError("Duplicate directions require an unambiguous source mesh; remove duplicates explicitly")
        if ear_order == "auto":
            default = [[0, .09, 0], [0, -.09, 0]] if convention == "SimpleFreeFieldHRIR" else None
            receiver = positions(file, "ReceiverPosition", default)
            if receiver.shape != (2, 3) or not (np.max(receiver[:, 1]) > 1e-6 and np.min(receiver[:, 1]) < -1e-6):
                raise ValueError("Cannot identify left/right ears; supply --ear-order lr or rl")
            order = np.argsort(-receiver[:, 1])
        else:
            order = np.array([0, 1] if ear_order == "lr" else [1, 0])
        if "Data.Delay" in file:
            delay = finite(file["Data.Delay"][...], "Data.Delay")
            unit = text(file["Data.Delay"].attrs.get("Units", "samples")).lower()
            if unit in ("second", "seconds", "s"):
                delay = delay * rates[0]
            elif unit not in ("sample", "samples"):
                raise ValueError("Data.Delay must use samples or seconds")
            if delay.shape == (2,):
                delay = delay[None, :]
            if delay.shape not in ((1, 2), (len(ir), 2)):
                raise ValueError("Data.Delay must have I,R or M,R dimensions")
            delay = np.broadcast_to(delay, (len(ir), 2)).copy()
        else:
            delay = np.zeros((len(ir), 2))
        return ir[:, order], directions, delay[:, order], int(rates[0]), float(radii[0]), order.tolist()


def complete_resample(ir, rate):
    if rate == RATE:
        return ir.copy(), 0
    ratio = Fraction(RATE, rate)
    up, down = ratio.numerator, ratio.denominator
    # Use a causal anti-alias filter whose group delay is an integer at 48 kHz.
    # upfirdn retains both ends, unlike resample_poly's nominal-length crop.
    half = math.ceil(10 * max(up, down) / down) * down
    if 2 * half + 1 > 1_000_001:
        raise ValueError("Sampling-rate ratio exceeds the preparation filter budget")
    # FIR coefficients represent a transfer function: compensate the sampling
    # interval (down/up relative to waveform resampling), preserving DC gain.
    taps = firwin(2 * half + 1, 1 / max(up, down), window=("kaiser", 8.6)) * down
    predicted = ((ir.shape[-1] - 1) * up + len(taps) - 1) // down + 1
    if predicted > 4096 or math.prod(ir.shape[:2]) * predicted * 8 > MAX_BYTES:
        raise ValueError("Complete resampled FIR exceeds the v1 resource budget")
    return upfirdn(taps, ir, up=up, down=down, axis=-1), half // down


def apply_delays(ir, delays):
    nearest = np.rint(delays)
    delays = np.where(np.abs(delays - nearest) < 1e-10, nearest, delays)
    fractional = np.any(delays != np.floor(delays))
    guard = (40 if fractional else 0) + max(0, -math.floor(float(delays.min())))
    maximum = math.ceil(float(delays.max())) + guard + ir.shape[-1] + (40 if fractional else 0)
    if maximum > 4096 or maximum <= 0 or math.prod(ir.shape[:2]) * maximum * 8 > MAX_BYTES:
        raise ValueError("Complete Data.Delay support exceeds the v1 resource budget")
    output = np.zeros((*ir.shape[:2], maximum))
    window = np.hanning(81)
    for index in np.ndindex(delays.shape):
        integer = math.floor(float(delays[index]))
        fraction = float(delays[index]) - integer
        if fraction == 0:
            start, values = integer + guard, ir[index]
        else:
            kernel = np.sinc(np.arange(81) - 40 - fraction) * window
            kernel /= kernel.sum()
            start = integer + guard - 40
            values = np.convolve(ir[index], kernel)
        output[index][start:start + len(values)] = values
    return output, guard


def mesh(directions, supplied=None, index_base=0):
    try:
        hull = ConvexHull(directions)
    except QhullError as error:
        raise ValueError("Directions do not form a usable spherical mesh") from error
    if np.any(hull.equations[:, -1] >= -1e-8):
        raise ValueError("Directional coverage does not enclose the listener")
    if supplied is None:
        faces = np.sort(hull.simplices, axis=1)
        faces = faces[np.lexsort(faces.T[::-1])]
    else:
        path = Path(supplied)
        values = np.load(path, allow_pickle=False) if path.suffix == ".npy" else (
            np.asarray(json.loads(path.read_text(encoding="utf-8"))) if path.suffix == ".json" else np.loadtxt(path))
        if not np.isfinite(values).all() or np.any(values != np.floor(values)):
            raise ValueError("Mesh indices must be finite integers")
        faces = np.asarray(values, dtype=np.int64) - index_base
    if faces.shape != (2 * len(directions) - 4, 3) or np.any(faces < 0) or np.any(faces >= len(directions)):
        raise ValueError("Mesh must be a closed triangulation using valid direction indices")
    if len(np.unique(np.sort(faces, axis=1), axis=0)) != len(faces) or len(np.unique(faces)) != len(directions):
        raise ValueError("Mesh has duplicate faces or unused directions")
    edges = {}
    for face in faces:
        points = directions[face]
        determinant = np.linalg.det(points)
        if abs(determinant) < 1e-12:
            raise ValueError("Degenerate spherical triangle")
        centre = points.sum(axis=0)
        centre /= np.linalg.norm(centre)
        if np.min(points @ centre) <= 0:
            raise ValueError("A triangle spans more than one hemisphere")
        sign = 1 if determinant > 0 else -1
        for a, b in zip(face, np.roll(face, -1)):
            edge = edges.setdefault((int(min(a, b)), int(max(a, b))), [0, 0])
            edge[0] += 1
            edge[1] += sign if a < b else -sign
    if any(value != [2, 0] for value in edges.values()):
        raise ValueError("Mesh has a hole or overlapping/inconsistent faces")
    return faces.astype("<u4")


def estimate_delays(ir):
    nfft = max(2048, 1 << (4 * ir.shape[-1] - 1).bit_length())
    lifter = np.zeros(nfft)
    lifter[0], lifter[nfft // 2] = 1, 1
    lifter[1:nfft // 2] = 2
    frequencies = np.fft.rfftfreq(nfft, 1 / RATE)
    selected = (frequencies >= 300) & (frequencies <= 1500)
    x = 2 * np.pi * frequencies[selected] / RATE
    x -= x.mean()
    rows = ir.reshape(-1, ir.shape[-1])
    delays = np.empty(len(rows))
    for start in range(0, len(rows), 32):
        spectrum = np.fft.rfft(rows[start:start + 32], n=nfft, axis=-1)
        magnitude = np.abs(spectrum)
        peak = magnitude.max(axis=-1, keepdims=True)
        if np.any(peak == 0):
            raise ValueError("Every direction/ear must contain a nonzero HRIR")
        cepstrum = np.fft.irfft(np.log(np.maximum(magnitude, peak * 1e-12)), n=nfft, axis=-1)
        minimum_phase = np.fft.rfft(cepstrum * lifter, axis=-1).imag
        phase = np.unwrap(np.angle(spectrum) - minimum_phase, axis=-1)
        delays[start:start + 32] = -np.sum(phase[..., selected] * x, axis=-1) / np.sum(x * x)
    if not np.isfinite(delays).all() or np.max(np.abs(delays)) > ir.shape[-1]:
        raise ValueError("Excess-phase delay estimate exceeds complete FIR support")
    return delays.reshape(ir.shape[:2])


def asset(root, name, values, dtype):
    array = np.asarray(values, dtype=dtype, order="C")
    if not np.isfinite(array).all():
        raise ValueError(f"Array cannot be represented as {dtype}: {name}")
    payload = array.tobytes()
    (root / name).write_bytes(payload)
    return dict(file=name, dtype=dtype, shape=list(array.shape), bytes=len(payload),
                sha256=hashlib.sha256(payload).hexdigest())


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepare(source, output, *, ear_order="auto", faces=None, index_base=0, checker=None):
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError("Output directory must not exist")
    source_hash = file_hash(source)
    ir, directions, extra, rate, radius, order = load_sofa(source, ear_order)
    triangles = mesh(directions, faces, index_base)
    ir, resample_guard = complete_resample(ir, rate)
    ir, delay_guard = apply_delays(ir, extra * RATE / rate)
    # The exact f32 samples are the runtime resource and the delay-estimation input.
    ir = ir.astype("<f4")
    if not np.isfinite(ir).all():
        raise ValueError("SOFA amplitude exceeds finite f32")
    delays = estimate_delays(ir.astype(np.float64))
    spread = math.ceil(float(delays.max() - delays.min()))
    guard = max(96, 40 + spread)
    length = math.ceil((ir.shape[-1] + 40 + guard + spread) / 64) * 64
    if guard > 4096 or length > 8192 or ir.nbytes > MAX_BYTES:
        raise ValueError("Complete aligned HRIR exceeds the v1 resource budget")
    if file_hash(source) != source_hash:
        raise ValueError("SOFA changed during preparation")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".prepare-hrir-", dir=output.parent))
    try:
        result = dict(schema="mradm.hrir", version=1, kind="delay-aligned-v1", sample_rate=RATE,
                      fractional_taps=81, guard_samples=guard, output_taps=length,
                      preprocessing_guard_samples=resample_guard + delay_guard,
                      source_sofa_sha256=source_hash,
                      delay_estimation="excess-phase slope, 300-1500 Hz; original response retained",
                      preparation=dict(generator="mradm-prepare-hrir-v1", numpy=np.__version__,
                                       scipy=scipy.__version__, h5py=h5py.__version__, source_sample_rate=rate,
                                       source_radius_metres=radius, source_ear_indices=order,
                                       resampling_coefficient_scale=rate / RATE,
                                       resampling_guard_samples=resample_guard, data_delay_guard_samples=delay_guard,
                                       normalization="none", trimming="none"),
                      assets={"impulses": asset(staging, "hrir.f32", ir, "<f4"),
                              "directions": asset(staging, "hrir-directions.f64", directions, "<f8"),
                              "faces": asset(staging, "hrir-faces.u32", triangles, "<u4"),
                              "delays": asset(staging, "hrir-delays.f64", delays, "<f8")})
        (staging / "hrir.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        if checker:
            completed = subprocess.run([str(Path(checker).resolve()), str(staging / "hrir.json")],
                                       capture_output=True, text=True, check=True)
            verification = json.loads(completed.stdout)
            if verification.get("passed") is not True:
                raise ValueError("Native node reconstruction failed")
            (staging / "node-validation.json").write_text(json.dumps(verification, indent=2) + "\n", encoding="utf-8")
        staging.rename(output)
        return result
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    global RATE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sofa", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ear-order", choices=("auto", "lr", "rl"), default="auto",
                        help="Order of the SOFA receivers; auto uses ReceiverPosition")
    parser.add_argument("--faces", type=Path, help="Original N x 3 mesh (.npy, JSON or text)")
    parser.add_argument("--faces-index-base", type=int, choices=(0, 1), default=0)
    parser.add_argument("--check-executable", type=Path, help="Release room_hrir_check executable")
    parser.add_argument("--sample-rate", type=int, choices=(48000, 96000), default=48000)
    args = parser.parse_args()
    RATE = args.sample_rate
    try:
        result = prepare(args.sofa, args.output, ear_order=args.ear_order, faces=args.faces,
                         index_base=args.faces_index_base, checker=args.check_executable)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"HRIR preparation failed: {error}\n")
    print(json.dumps(dict(manifest=str(args.output / "hrir.json"),
                         directions=result["assets"]["directions"]["shape"][0],
                         preprocessing_guard_samples=result["preprocessing_guard_samples"],
                         interpolation_guard_samples=result["guard_samples"],
                         node_validation="passed" if args.check_executable else "not_run")))


if __name__ == "__main__":
    main()
