#!/usr/bin/env python3
"""Recover independent static object gains from one simultaneous ADM bank."""

import argparse
import hashlib
import json
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav, find_delay


def read_multichannel_adm(path: Path, channels: int, frames: int) -> np.ndarray:
    # FFmpeg's automatic channel-layout resampler cannot handle this 71–120ch
    # ADM input. Read its 24-bit PCM payload directly through Python's WAVE parser.
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
            channels, 48_000, 3, frames
        ):
            raise ValueError(f"unexpected ADM PCM format: {path}")
        payload = reader.readframes(frames)
    octets = np.frombuffer(payload, dtype=np.uint8)
    if octets.size != frames * channels * 3:
        raise ValueError("ADM PCM payload is incomplete")
    bytes3 = octets.reshape(-1, 3).astype(np.int32)
    unsigned = bytes3[:, 0] | (bytes3[:, 1] << 8) | (bytes3[:, 2] << 16)
    signed = (unsigned ^ 0x800000) - 0x800000
    return (signed.astype(np.float32) / 8388608.0).reshape(frames, channels)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def solve_gains(source: np.ndarray, output: np.ndarray) -> tuple[np.ndarray, float, float, np.ndarray]:
    gram = source.T @ source
    condition = float(np.linalg.cond(gram))
    if condition > 5.0:
        raise ValueError(f"PRBS bank is poorly conditioned: {condition}")
    gains = np.linalg.solve(gram, source.T @ output)
    residual = output - source @ gains
    residual_ratio = float(np.linalg.norm(residual) / max(np.linalg.norm(output), 1e-12))
    middle = len(source) // 2
    early = np.linalg.solve(source[:middle].T @ source[:middle], source[:middle].T @ output[:middle])
    late = np.linalg.solve(source[middle:].T @ source[middle:], source[middle:].T @ output[middle:])
    drift = np.linalg.norm(early - late, axis=1) / np.maximum(np.linalg.norm(gains, axis=1), 1e-12)
    return gains, residual_ratio, condition, drift


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--candidate", action="append", default=[], metavar="LAYOUT=PATH")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    manifest = json.loads(args.suite_manifest.read_text())
    case = next((item for item in manifest["cases"] if item["case_id"] == args.case), None)
    if case is None or "objects" not in case:
        parser.error("case is not a static bank")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("ADM differs from suite manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or Path(run["adm"]).resolve() != adm.resolve():
        raise ValueError("Dolby export is incomplete or used a different ADM")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    num_inputs = case["input_object_first_channel"] + len(case["objects"])
    input_pcm = read_multichannel_adm(adm, num_inputs, frames)
    object_channels = [item["input_channel"] for item in case["objects"]]
    first, last = 4800, frames - 4800
    source = input_pcm[first:last, object_channels].astype(np.float64)
    report = {"case_id": args.case, "adm": str(adm.resolve()), "adm_sha256": case["sha256"],
              "objects": len(object_channels), "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        output_path = Path(artifact["path"])
        if file_sha256(output_path) != artifact["sha256"]:
            raise ValueError(f"Dolby WAV changed: {output_path}")
        output_pcm = decode_wav(output_path, artifact["channels"], frames)
        lag, peak = find_delay(input_pcm[:, object_channels[0]], output_pcm, frames)
        if first + lag < 0 or last + lag > frames:
            raise ValueError("Dolby output delay exceeds valid measurement window")
        rendered = output_pcm[first + lag:last + lag].astype(np.float64)
        gains, residual, condition, drift = solve_gains(source, rendered)
        if not mapping[layout]["all_samples_exact"]:
            raise ValueError(f"unverified Dolby channel map for {layout}")
        canonical = np.zeros_like(gains)
        for item in mapping[layout]["interleaved_to_mono"]:
            canonical[:, item["mono_index"]] = gains[:, item["interleaved_index"]]
        report["layouts"][layout] = {
            "channel_order": "Dolby numbered multi-mono",
            "wav": str(output_path.resolve()),
            "delay_samples": lag,
            "delay_correlation_peak": round(peak, 6),
            "gram_condition": round(condition, 6),
            "linear_residual_ratio": round(residual, 8),
            "max_gain_drift": round(float(np.max(drift)), 7),
            "points": [
                {"label": item["label"], "xyz": item["xyz"], "size": item.get("size", 0.0),
                 "gains": [round(float(value), 8) for value in canonical[index]],
                 "power": round(float(canonical[index] @ canonical[index]), 8),
                 "gain_drift": round(float(drift[index]), 7)}
                for index, item in enumerate(case["objects"])
            ],
        }
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
            output_pcm = decode_wav(path, channels, frames)
            lag, peak = find_delay(input_pcm[:, object_channels[0]], output_pcm, frames)
            if first + lag < 0 or last + lag > frames:
                raise ValueError("candidate delay exceeds valid measurement window")
            gains, residual, condition, drift = solve_gains(
                source, output_pcm[first + lag:last + lag].astype(np.float64))
            if layout == "7.1.4":
                # mradm writes WAVEFORMATEXTENSIBLE mask-bit order: rear pair
                # precedes side pair. Convert back to speaker-layout order.
                gains = gains[:, [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11]]
            report["candidates"][layout] = {
                "channel_order": "canonical speaker labels",
                "wav": str(path.resolve()), "sha256": file_sha256(path),
                "delay_samples": lag, "delay_correlation_peak": round(peak, 6),
                "gram_condition": round(condition, 6),
                "linear_residual_ratio": round(residual, 8),
                "max_gain_drift": round(float(np.max(drift)), 7),
                "points": [{"label": item["label"], "xyz": item["xyz"], "size": item.get("size", 0.0),
                            "gains": [round(float(number), 8) for number in gains[index]],
                            "power": round(float(gains[index] @ gains[index]), 8)}
                           for index, item in enumerate(case["objects"])],
            }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
