#!/usr/bin/env python3
"""Measure true speaker power, spectra, correlations and tails of long size probes."""

import argparse
import hashlib
import json
import math
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_static_bank import file_sha256, read_multichannel_adm

RATE = 48_000
FFT_FRAMES = 4096
FFT_HOP = 1024
CENTERS = [63.0 * (2.0 ** (index / 3.0)) for index in range(25)]


def canonical_pcm(pcm: np.ndarray, layout: str, mapping: dict, candidate: bool) -> np.ndarray:
    result = np.zeros_like(pcm)
    if candidate:
        indices = [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11] if layout == "7.1.4" else list(range(16))
    else:
        if not mapping["all_samples_exact"]:
            raise ValueError("unverified reference channel map")
        indices = [item["mono_index"] for item in mapping["interleaved_to_mono"]]
    for original, named in enumerate(indices):
        result[:, named] = pcm[:, original]
    return result


def spectral_field(x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    starts = range(0, len(x) - FFT_FRAMES + 1, FFT_HOP)
    window = np.hanning(FFT_FRAMES)
    sxx = np.zeros(FFT_FRAMES // 2 + 1)
    sxy = np.zeros((FFT_FRAMES // 2 + 1, y.shape[1]), dtype=np.complex128)
    syy = np.zeros((FFT_FRAMES // 2 + 1, y.shape[1], y.shape[1]), dtype=np.complex128)
    for start in starts:
        xx = np.fft.rfft(x[start:start + FFT_FRAMES] * window)
        yy = np.fft.rfft(y[start:start + FFT_FRAMES] * window[:, None], axis=0)
        sxx += np.abs(xx) ** 2
        sxy += np.conj(xx)[:, None] * yy
        syy += np.conj(yy)[:, :, None] * yy[:, None, :]
    frequencies = np.fft.rfftfreq(FFT_FRAMES, 1 / RATE)
    power = []
    covariance = []
    coherence = []
    for center in CENTERS:
        active = (frequencies >= center / (2.0 ** (1 / 6))) & (
            frequencies < center * (2.0 ** (1 / 6)))
        if not np.any(active):
            raise ValueError(f"empty one-third-octave band at {center} Hz")
        matrix = np.sum(syy[active], axis=0)
        band_power = np.real(np.diag(matrix))
        trace = float(np.sum(band_power))
        if trace <= 0:
            raise ValueError("zero band energy in size probe")
        power.append(band_power)
        covariance.append(matrix / trace)
        summed_xx = float(np.sum(sxx[active]))
        summed_xy = np.sum(sxy[active], axis=0)
        coherence.append(np.abs(summed_xy) ** 2 / np.maximum(summed_xx * band_power, 1e-30))
    return np.array(power), np.array(covariance), np.array(coherence)


def fir_prediction_error(x: np.ndarray, y: np.ndarray, taps: int = 512) -> float:
    # Fit one causal FIR on the first 2 s and predict the independent second 2 s.
    midpoint = len(x) // 2
    source = x[:midpoint].copy()
    target = y[:midpoint].copy()
    size = 1 << (2 * midpoint - 1).bit_length()
    source_fft = np.fft.rfft(source, size)
    source_auto = np.fft.irfft(source_fft * np.conj(source_fft), size)[:taps]
    toeplitz = source_auto[np.abs(np.arange(taps)[:, None] - np.arange(taps)[None, :])]
    toeplitz += np.eye(taps) * source_auto[0] * 1e-8
    target_fft = np.fft.rfft(target, size, axis=0)
    cross = np.fft.irfft(target_fft * np.conj(source_fft)[:, None], size, axis=0)[:taps]
    coefficients = np.linalg.solve(toeplitz, cross)
    length = 1 << (len(x) + taps - 1).bit_length()
    estimate = np.fft.irfft(np.fft.rfft(x, length)[:, None] *
                            np.fft.rfft(coefficients, length, axis=0), length, axis=0)[:len(x)]
    first = midpoint + taps
    last = len(x) - taps
    return float(np.linalg.norm(estimate[first:last] - y[first:last]) /
                 max(np.linalg.norm(y[first:last]), 1e-12))


def tail_curve(y: np.ndarray) -> list[float]:
    energy = np.sum(y * y, axis=1)
    accumulated = np.cumsum(energy[::-1])[::-1]
    if accumulated[0] <= 0:
        return [-120.0 for _ in range(0, len(y), 48)]
    return [float(10.0 * math.log10(max(accumulated[index] / accumulated[0], 1e-12)))
            for index in range(0, len(y), 48)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate-714", type=Path)
    parser.add_argument("--candidate-916", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".npz").exists():
        parser.error("output report or spectral archive already exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "pcm_sha256" not in case or "objects" not in case:
        parser.error("case is not a long size PRBS probe")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
    with wave.open(str(adm), "rb") as reader:
        if hashlib.sha256(reader.readframes(case["duration_samples"])).hexdigest() != case["pcm_sha256"]:
            raise ValueError("final ADM PCM changed")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run.get("restore_errors") or Path(run["adm"]).resolve() != adm.resolve() or (
        run["baseline_settings_sha256"] != run["settings_sha256_after"]
    ):
        raise ValueError("reference export or state restoration is incomplete")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    source = read_multichannel_adm(adm, case["num_channels"], case["duration_samples"])
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"],
              "pcm_sha256": case["pcm_sha256"], "fixed_delay_samples": 0,
              "global_level": 1.0, "band_centers_hz": CENTERS,
              "layouts": {}, "candidates": {}}
    arrays = {}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError(f"reference WAV changed: {wav}")
        paths = [("layouts", wav, False)]
        candidate = args.candidate_714 if layout == "7.1.4" else args.candidate_916
        if candidate is not None:
            paths.append(("candidates", candidate, True))
        for group, path, is_candidate in paths:
            rendered = canonical_pcm(decode_wav(path, artifact["channels"], case["duration_samples"]),
                                     layout, mapping[layout], is_candidate).astype(np.float64)
            rows = []
            for index, item in enumerate(case["objects"]):
                first, stop = item["signal_start_sample"], item["signal_stop_sample"]
                source_pcm = source[first:stop, item["input_channel"]].astype(np.float64)
                speaker_pcm = rendered[first:stop]
                steady_first, steady_last = 4800, len(source_pcm) - 4800
                x = source_pcm[steady_first:steady_last]
                y = speaker_pcm[steady_first:steady_last]
                input_power = float(np.mean(x * x))
                if input_power <= 1e-12:
                    raise ValueError(f"no source signal for {item['label']}")
                gain = x @ y / float(x @ x)
                channel_power = np.mean(y * y, axis=0) / input_power
                total = float(channel_power.sum())
                rms_amplitude = np.sqrt(channel_power)
                model_residual = float(np.linalg.norm(y - x[:, None] * gain[None, :]) /
                                       max(np.linalg.norm(y), 1e-12))
                band_power, covariance, coherence = spectral_field(x, y)
                key = f"{group}_{layout.replace('.', '')}_{index}"
                arrays[f"{key}_band_power"] = band_power.astype(np.float64)
                arrays[f"{key}_covariance"] = covariance.astype(np.complex64)
                arrays[f"{key}_coherence"] = coherence.astype(np.float32)
                tail = rendered[stop:stop + 36_000]
                # The explicit final quiet interval measures actual export noise.
                # Include the 24-bit reference quantization floor even when its
                # silence is exactly zero; do not score below that resolution.
                tail_indices = np.arange(0, len(tail), 48)
                noise_power = max(float(np.mean(np.sum(tail[-4800:] ** 2, axis=1))),
                                  tail.shape[1] * (2.0 ** -23) ** 2 / 12.0)
                cumulative = np.cumsum(np.sum(tail * tail, axis=1)[::-1])[::-1]
                floor_energy = noise_power * (len(tail) - tail_indices)
                tail_source_energy = max(float(np.sum(source_pcm * source_pcm)), 1e-12)
                tail_power_ratio = float(np.sum(tail * tail) /
                                         max(np.sum(source_pcm * source_pcm), 1e-12))
                rows.append({"label": item["label"], "xyz": item["xyz"], "size": item["size"],
                             "signed_pure_gain": gain.tolist(),
                             "rms_amplitude": rms_amplitude.tolist(),
                             "speaker_energy": channel_power.tolist(),
                             "normalized_energy_share": (channel_power / total).tolist(),
                             "total_power": total,
                             "pure_gain_residual_ratio": model_residual,
                             "stationary_fir_512_test_error": fir_prediction_error(x, y),
                             "mean_input_output_coherence": np.mean(coherence, axis=0).tolist(),
                             "tail_total_power_ratio": tail_power_ratio,
                             "tail_decay_db_1ms": tail_curve(tail),
                             "tail_measurable_1ms": (cumulative[tail_indices] > floor_energy).tolist(),
                             "tail_cumulative_power_ratio_1ms": (cumulative[tail_indices] / tail_source_energy).tolist(),
                             "tail_noise_energy_ratio_1ms": (floor_energy / tail_source_energy).tolist(),
                             "tail_noise_floor_power": noise_power,
                             "spectrum_key": key})
            report[group][layout] = {"wav": str(path.resolve()), "sha256": file_sha256(path),
                                     "objects": rows}
    archive = args.output.with_suffix(".npz")
    np.savez_compressed(archive, **arrays)
    report["spectral_archive"] = str(archive.resolve())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
