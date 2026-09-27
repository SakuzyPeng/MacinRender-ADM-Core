#!/usr/bin/env python3
"""Extract signed object-to-speaker gains from the final ADM and Dolby WAVs."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def decode_wav(path: Path, channels: int, frames: int) -> np.ndarray:
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le", "-"],
        check=True, capture_output=True,
    )
    samples = np.frombuffer(result.stdout, dtype="<f4")
    if samples.size != frames * channels:
        raise ValueError(f"{path}: expected {frames * channels} samples, got {samples.size}")
    pcm = samples.reshape(frames, channels)
    if not np.isfinite(pcm).all():
        raise ValueError(f"{path}: nonfinite PCM")
    return pcm


def find_delay(input_signal: np.ndarray, output: np.ndarray, burst_frames: int) -> tuple[int, float]:
    start, stop = 256, min(burst_frames, 13_000)
    source = input_signal[start:stop].astype(np.float64)
    rendered = output[start:stop].astype(np.float64)
    n = 1 << (2 * len(source) - 1).bit_length()
    source_fft = np.fft.rfft(source, n)
    rendered_fft = np.fft.rfft(rendered, n, axis=0)
    correlation = np.fft.irfft(rendered_fft * np.conj(source_fft[:, None]), n, axis=0)
    limit = min(2048, len(source) // 4)
    lags = np.arange(-limit, limit + 1)
    values = correlation[lags % n]
    maxima = np.max(np.abs(values), axis=1)
    best = int(np.argmax(maxima))
    return int(lags[best]), float(maxima[best] / max(np.sum(source * source), 1e-30))


def fit_gains(source: np.ndarray, output: np.ndarray) -> tuple[np.ndarray, np.ndarray, float]:
    x = source.astype(np.float64)
    y = output.astype(np.float64)
    denominator = float(x @ x)
    if denominator < 1e-12:
        raise ValueError("probe source window has insufficient energy")
    gains = (x @ y) / denominator
    residual = y - x[:, None] * gains[None, :]
    output_power = np.mean(y * y, axis=0)
    residual_power = np.mean(residual * residual, axis=0)
    residual_ratio = np.sqrt(residual_power / np.maximum(output_power, 1e-15))
    power_ratio = float(np.sum(output_power) / np.mean(x * x))
    return gains, residual_ratio, power_ratio


def measure(path: Path, pcm: np.ndarray, source: np.ndarray, case: dict) -> dict:
    lag, peak = find_delay(source, pcm, case["burst_samples"])
    length = case["segment_samples"]
    samples = []
    for index, point in enumerate(case["points"]):
        first = index * length + 7200
        last = index * length + 14_400
        if first + lag < 0 or last + lag > len(pcm):
            raise ValueError(f"delay {lag} makes segment {index} leave the PCM timeline")
        x = source[first:last]
        y = pcm[first + lag:last + lag]
        gains, residuals, power = fit_gains(x, y)
        middle = (last - first) // 2
        early, _, _ = fit_gains(x[:middle], y[:middle])
        late, _, _ = fit_gains(x[middle:], y[middle:])
        drift = float(np.linalg.norm(early - late) / max(np.linalg.norm(gains), 1e-12))
        samples.append({
            "label": point["label"], "xyz": point["xyz"],
            "gains": [round(float(value), 8) for value in gains],
            "power_ratio": round(power, 8),
            "max_residual_ratio_active_channels": round(float(np.max(residuals[np.abs(gains) > 0.001])), 6)
            if np.any(np.abs(gains) > 0.001) else 0.0,
            "gain_drift": round(drift, 6),
            "stable": drift <= 0.01,
        })
    return {
        "wav": str(path.resolve()), "sha256": file_sha256(path),
        "delay_samples": lag, "delay_correlation_peak": round(peak, 6),
        "points": samples,
        "unstable_count": sum(not point["stable"] for point in samples),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--candidate", action="append", default=[], metavar="LAYOUT=PATH",
                        help="additional renderer WAV to measure; may be repeated")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")
    manifest = json.loads(args.suite_manifest.read_text(encoding="utf-8"))
    case = next((item for item in manifest["cases"] if item["case_id"] == args.case), None)
    if case is None:
        parser.error(f"case {args.case} is absent from suite manifest")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from suite manifest")
    run = json.loads(args.dar_run.read_text(encoding="utf-8"))
    if not run["success"] or Path(run["adm"]).resolve() != adm.resolve():
        raise ValueError("Dolby export did not complete for this exact ADM")
    frames = case["duration_samples"]
    input_pcm = decode_wav(adm, 11, frames)
    source = input_pcm[:, case["input_object_channel"]]
    report = {"case_id": args.case, "adm": case["adm"], "adm_sha256": case["sha256"],
              "layouts": {}}
    for artifact in run["outputs"]:
        path = Path(artifact["path"])
        if file_sha256(path) != artifact["sha256"]:
            raise ValueError(f"Dolby WAV differs from run report: {path}")
        pcm = decode_wav(path, artifact["channels"], frames)
        report["layouts"][artifact["layout"]] = measure(path, pcm, source, case)
    if args.candidate:
        report["candidates"] = {}
        for value in args.candidate:
            if "=" not in value:
                parser.error("--candidate must be LAYOUT=PATH")
            layout, file_name = value.split("=", 1)
            if layout not in ("7.1.4", "9.1.6") or layout in report["candidates"]:
                parser.error("candidate layout must be unique 7.1.4 or 9.1.6")
            path = Path(file_name)
            channels = 12 if layout == "7.1.4" else 16
            pcm = decode_wav(path, channels, frames)
            report["candidates"][layout] = measure(path, pcm, source, case)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
