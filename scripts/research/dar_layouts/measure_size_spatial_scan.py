#!/usr/bin/env python3
"""Recover stable coherent gains from a moving ADM size/position grid."""

import argparse
import json
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import canonical_pcm
from measure_static_bank import file_sha256, read_multichannel_adm


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--dar-run", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists")
    suite = json.loads(args.suite_manifest.read_text())
    case = next((item for item in suite["cases"] if item["case_id"] == args.case), None)
    if case is None or "events" not in case or not all("size" in event for event in case["events"]):
        parser.error("case is not a spatial size scan")
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM differs from manifest")
    run = json.loads(args.dar_run.read_text())
    if not run["success"] or run.get("restore_errors") or Path(run["adm"]).resolve() != adm.resolve() or (
        run["baseline_settings_sha256"] != run["settings_sha256_after"]
    ):
        raise ValueError("Dolby export or state restoration incomplete")
    mapping = json.loads(args.channel_map.read_text())["layouts"]
    frames = case["duration_samples"]
    source = read_multichannel_adm(adm, 11, frames)[:, case["input_object_channel"]].astype(np.float64)
    report = {"case_id": case["case_id"], "adm_sha256": case["sha256"], "layouts": {}}
    for artifact in run["outputs"]:
        layout = artifact["layout"]
        wav = Path(artifact["path"])
        if file_sha256(wav) != artifact["sha256"]:
            raise ValueError("Dolby WAV changed")
        pcm = canonical_pcm(decode_wav(wav, artifact["channels"], frames),
                            layout, mapping[layout], False).astype(np.float64)
        rows = []
        for index, event in enumerate(case["events"]):
            first, last = event["start_samples"] + 24_000, event["start_samples"] + 67_200
            x = source[first:last]
            y = pcm[first:last]
            denominator = float(x @ x)
            if denominator < 1e-12:
                raise ValueError(f"silent input during size grid point {index}")
            gain = (x @ y) / denominator
            input_power = float(np.mean(x * x))
            channel_power = np.mean(y * y, axis=0) / input_power
            total = float(channel_power.sum())
            residual = float(np.linalg.norm(y - x[:, None] * gain[None, :]) /
                             max(np.linalg.norm(y), 1e-12))
            half = len(x) // 2
            early = x[:half] @ y[:half] / float(x[:half] @ x[:half])
            late = x[half:] @ y[half:] / float(x[half:] @ x[half:])
            drift = float(np.linalg.norm(early - late) / max(np.linalg.norm(gain), 1e-12))
            rows.append({"label": f"grid_{index:02d}", "xyz": event["position"],
                         "size": event["size"], "start_sample": event["start_samples"],
                         "signed_pure_gain": gain.tolist(),
                         "rms_amplitude": np.sqrt(channel_power).tolist(),
                         "speaker_energy": channel_power.tolist(),
                         "normalized_energy_share": (channel_power / total).tolist(),
                         "total_power": total, "pure_gain_residual_ratio": residual,
                         "gain_drift": drift})
        report["layouts"][layout] = {"wav": str(wav.resolve()), "sha256": artifact["sha256"],
                                      "points": rows}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
