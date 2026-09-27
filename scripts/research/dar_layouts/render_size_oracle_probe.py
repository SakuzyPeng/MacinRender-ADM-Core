#!/usr/bin/env python3
"""Research-only upper bound: use independent measured gains with shared FIRs."""

import argparse
import json
import subprocess
from pathlib import Path

import numpy as np

from measure_static_bank import file_sha256, read_multichannel_adm

PAIRS = ((0, 1), (4, 5), (6, 7), (8, 9), (10, 11))
LABELS = {"quarter_front": "front_quarter", "quarter_origin": "origin_quarter",
          "full_front": "front_full"}
MAP_714_TO_916 = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7,
                  8: 10, 9: 11, 10: 14, 11: 15}
WAV_ORDER_714 = [0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11]


def filtered(source: np.ndarray, impulse: np.ndarray) -> np.ndarray:
    length = 1 << (len(source) + len(impulse) - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(source, length) *
                        np.fft.rfft(impulse, length), length)[:len(source)]


def write_float_wav(path: Path, pcm: np.ndarray) -> None:
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "f32le",
                    "-ar", "48000", "-ac", str(pcm.shape[1]), "-i", "pipe:0",
                    "-c:a", "pcm_f32le", str(path)],
                   input=np.asarray(pcm, dtype="<f4").tobytes(), check=True, capture_output=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--long-manifest", type=Path, required=True)
    parser.add_argument("--warm-report", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tail-gate", action="store_true",
                        help="Research a shared input-silence envelope on the FIR branches")
    parser.add_argument("--tail-gate-json", type=Path,
                        help="Use one measured 512-frame silence gate on every FIR branch")
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error("output directory exists")
    if args.tail_gate and args.tail_gate_json:
        parser.error("choose one tail gate")
    args.output_dir.mkdir(parents=True)
    case = json.loads(args.long_manifest.read_text())["cases"][0]
    adm = Path(case["adm"])
    if file_sha256(adm) != case["sha256"]:
        raise ValueError("final ADM changed")
    input_pcm = read_multichannel_adm(adm, case["num_channels"], case["duration_samples"])
    warm = json.loads(args.warm_report.read_text())
    responses = np.load(warm["fir_archive"])
    reference = responses["714_5_fir"].astype(np.float64)
    filters = {left: np.r_[0.0, reference[1:, left] / reference[0, left]] for left, _ in PAIRS}
    measured_gate = None
    if args.tail_gate_json:
        gate_report = json.loads(args.tail_gate_json.read_text())
        measured_gate = np.array(gate_report["gate"])
        if measured_gate.shape != (512,):
            raise ValueError("measured silence gate must have 512 samples")
    gains = {item["label"]: np.array(item["signed_direct_gain"], dtype=np.float64)
             for item in warm["layouts"]["7.1.4"]["points"]}
    canonical = np.zeros((case["duration_samples"], 12), dtype=np.float64)
    for item in case["objects"]:
        gain = gains[LABELS[item["label"]]]
        source = input_pcm[:, item["input_channel"]].astype(np.float64)
        gate = np.ones(len(source), dtype=np.float64)
        if args.tail_gate:
            stop = item["signal_stop_sample"]
            start = stop - 64
            cutoff = stop + 512
            gate[start:cutoff] = np.exp(-np.arange(cutoff - start) / 245.0)
            gate[cutoff:] = 0.0
        if measured_gate is not None:
            stop = item["signal_stop_sample"]
            gate[stop:stop + 512] = measured_gate
            gate[stop + 512:] = 0.0
        canonical[:, 2] += source * gain[2]
        for left, right in PAIRS:
            delayed = filtered(source, filters[left]) * gate
            canonical[:, left] += gain[left] * (source + delayed)
            canonical[:, right] += gain[right] * (source - delayed)
    out_714 = np.zeros_like(canonical)
    for original, named in enumerate(WAV_ORDER_714):
        out_714[:, original] = canonical[:, named]
    out_916 = np.zeros((len(canonical), 16), dtype=np.float64)
    for from_index, to_index in MAP_714_TO_916.items():
        out_916[:, to_index] = canonical[:, from_index]
    first = args.output_dir / "oracle-714.wav"
    second = args.output_dir / "oracle-916.wav"
    write_float_wav(first, out_714)
    write_float_wav(second, out_916)
    report = {"research_only": True, "model": "independent measured direct gains plus four shared FIRs",
              "tail_gate": args.tail_gate,
              "tail_gate_json": str(args.tail_gate_json.resolve()) if args.tail_gate_json else None,
              "adm": str(adm.resolve()), "adm_sha256": case["sha256"],
              "warm_report": str(args.warm_report.resolve()),
              "outputs": {"7.1.4": {"path": str(first.resolve()), "sha256": file_sha256(first)},
                          "9.1.6": {"path": str(second.resolve()), "sha256": file_sha256(second)}}}
    (args.output_dir / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(args.output_dir / "manifest.json")


if __name__ == "__main__":
    main()
