#!/usr/bin/env python3
"""Verify static ADM trial PCM routing and decoded position/diffuse observations."""

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np

from make_adm_input import pcm24, require


def verify_audio(expected_path, prefix, decoded_path, output):
    expected = json.loads(expected_path.read_text())
    timing = json.loads(Path(str(prefix) + ".timing.json").read_text())
    channels, frames = expected["channels"], expected["frames"]
    decoded = np.memmap(decoded_path, mode="r", dtype="<f4").reshape(-1, channels)
    require(len(decoded) == sum(timing.values()), "decoded frame count differs from packet timing")
    require(timing["valid_frames"] == frames, "container valid frames differ from source")
    leading = timing["leading_frames"]
    source_energy = np.zeros(channels)
    decoded_energy = np.zeros(channels)
    error_energy = np.zeros(channels)
    cross = np.zeros((channels, channels))
    peaks = np.zeros(channels)
    digest = hashlib.sha256()
    with Path(expected["source"]).open("rb") as source:
        source.seek(expected["data_offset"] + expected["start_sample"] * channels * 3)
        for begin in range(0, frames, 65536):
            count = min(frames - begin, 65536)
            raw = source.read(count * channels * 3)
            digest.update(raw)
            a = pcm24(raw, channels).astype(np.float64)
            b = decoded[leading + begin:leading + begin + count].astype(np.float64)
            require(a.shape == b.shape and np.isfinite(b).all(), "invalid decoded PCM")
            source_energy += np.sum(a * a, axis=0)
            decoded_energy += np.sum(b * b, axis=0)
            error_energy += np.sum((a - b) ** 2, axis=0)
            cross += a.T @ b
            peaks = np.maximum(peaks, np.max(np.abs(b), axis=0))
    require(digest.hexdigest() == expected["source_pcm_excerpt_sha256"], "source PCM changed since staging")
    covariance = cross / np.sqrt(np.maximum(source_energy[:, None] * decoded_energy[None, :], 1e-300))
    rows = []
    silent_source_channels = []
    silent_tolerance = max(1e-8, 1e-3 * math.sqrt(float(np.sum(source_energy)) / (frames * channels)))
    for channel, track in enumerate(expected["tracks"]):
        diagonal = float(covariance[channel, channel])
        best = int(np.argmax(np.abs(covariance[:, channel])))
        rms = math.sqrt(decoded_energy[channel] / frames)
        if source_energy[channel] == 0:
            require(rms <= silent_tolerance, f"unexpected audio in silent source channel {channel}")
            silent_source_channels.append(channel)
            rows.append({"channel": channel, "object_id": track["object_id"], "name": track["name"],
                         "speaker_label": track["speaker_label"], "source_rms": 0.0, "decoded_rms": rms,
                         "decoded_peak": float(peaks[channel]), "correlation": None, "gain_db": None,
                         "snr_db": None, "best_correlated_source_channel": None,
                         "silent_at_source": True})
            continue
        require(rms > 0, f"missing track {channel}")
        # Full-program fidelity criterion; a short near-silent excerpt is not suitable.
        require(diagonal >= (0.90 if channel == 3 else 0.98), f"low waveform correlation on channel {channel}")
        require(abs(covariance[best, channel]) - diagonal < 1e-3, f"suspected track swap at {channel}")
        rows.append({"channel": channel, "object_id": track["object_id"], "name": track["name"],
                     "speaker_label": track["speaker_label"], "source_rms": math.sqrt(source_energy[channel] / frames),
                     "decoded_rms": rms, "decoded_peak": float(peaks[channel]), "correlation": diagonal,
                     "gain_db": float(10 * np.log10(decoded_energy[channel] / source_energy[channel])),
                     "snr_db": float(10 * np.log10(source_energy[channel] / max(error_energy[channel], 1e-300))),
                     "best_correlated_source_channel": best, "silent_at_source": False})
    result = {"passed": True, "sample_rate": 48000, "valid_frames": frames, "decoded_frames": len(decoded),
              "timing": timing, "channels": channels, "channel_results": rows,
              "minimum_non_lfe_correlation": min(row["correlation"] for row in rows
                                                 if row["channel"] != 3 and row["correlation"] is not None),
              "minimum_non_lfe_snr_db": min(row["snr_db"] for row in rows
                                           if row["channel"] != 3 and row["snr_db"] is not None),
              "lfe": rows[3], "complete_adm_metadata": expected["complete_adm_metadata"],
              "omitted_extent": expected["omitted_extent"],
              "discarded_diffuse": expected.get("discarded_diffuse", []),
              "silent_source_channels": silent_source_channels, "silent_channel_rms_tolerance": silent_tolerance,
              "note": "Audio fidelity/routing check only; no system spatial-playback equivalence claim."}
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    return result


def verify_metadata(expected_path, trace_path, output, leading_packets=2):
    expected = json.loads(expected_path.read_text())
    trace = json.loads(trace_path.read_text())
    require(trace["returncode"] == 0 and not trace["timed_out"], "metadata observer did not complete")
    profile = next(e for e in trace["events"] if "profile" in e)
    require((profile["profile"], profile["level"]) == (5, 0), "unexpected profile/level")
    labels = [c["label"] for c in next(e for e in trace["events"] if e["stage"] == "decoder_input_layout_value")["descriptions"]]
    require(labels[:10] == expected["bed_labels"] and len(labels) == expected["channels"], "BED layout mismatch")
    require(all(label >= 262144 for label in labels[10:]), "missing object track labels")
    rows = [e for e in trace["events"] if e["stage"] == "decoded_renderer_metadata"
            and e["packet"] >= leading_packets and e["group_id"] not in (None, 0)]
    require(bool(rows), "no post-prime object metadata captured")
    seen, errors = set(), []
    for row in rows:
        track = expected["tracks"][row["group_id"] + 9]
        require(track["group_id"] == row["group_id"], "unstable group identity")
        a, b = row["spherical"], track["spherical"]
        error = [abs((a[0] - b[0] + 180) % 360 - 180), abs(a[1] - b[1]), abs(a[2] - b[2])]
        precision = row.get("position_precision_bits", [9, 8, 7])
        tolerances = [180 / (1 << precision[0]), 90 / (1 << precision[1]), 2 / (1 << precision[2])]
        require(all(x <= y + 1e-5 for x, y in zip(error, tolerances)), f"position mismatch: {row}")
        effective_diffuse = 0.0 if expected.get("discard_diffuse", False) else track["diffuse"]
        require(row["diffuse"] == effective_diffuse, "diffuse differs from the requested conversion")
        errors.append(error)
        seen.add((row["packet"], row["group_id"]))
    packets = {packet for packet, _ in seen}
    require(seen == {(p, g) for p in packets for g in range(1, expected["objects"] + 1)}, "missing object metadata")
    result = {"passed": True, "profile": 5, "level": 0, "native_layout": labels,
              "object_metadata_rows": len(rows), "observed_packet_indices": sorted(packets),
              "maximum_errors_azimuth_elevation_distance": np.max(errors, axis=0).tolist(),
              "position_precision_bits": rows[0].get("position_precision_bits", "9/8/7 from prior position probe"),
              "all_diffuse_values_match": True, "extent_preserved": not expected["omitted_extent"],
              "diffuse_compared_to": "requested conversion; source values retained separately",
              "all_decoded_diffuse_zero": all(row["diffuse"] == 0 for row in rows),
              "source_diffuse_preserved": not expected.get("discarded_diffuse", []),
              "decoder_modified": False,
              "dedicated_lfe_decode_observed": any("APACLFEElement::Deserialize" in e.get("function", "") for e in trace["events"])}
    output.write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    audio = sub.add_parser("audio")
    for name in ("expected", "prefix", "decoded", "output"):
        audio.add_argument(name, type=Path)
    meta = sub.add_parser("metadata")
    for name in ("expected", "trace", "output"):
        meta.add_argument(name, type=Path)
    meta.add_argument("--leading-packets", type=int, default=2)
    args = parser.parse_args()
    if args.command == "audio":
        result = verify_audio(args.expected, args.prefix, args.decoded, args.output)
        print(json.dumps({k: result[k] for k in ("passed", "valid_frames", "channels", "minimum_non_lfe_correlation", "minimum_non_lfe_snr_db", "lfe")}))
    else:
        print(json.dumps(verify_metadata(args.expected, args.trace, args.output, args.leading_packets)))
