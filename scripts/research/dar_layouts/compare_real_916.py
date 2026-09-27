#!/usr/bin/env python3
"""Fixed-map, fixed-level full-program 9.1.6 comparison (no alignment/fitting)."""

import argparse
import json
import math
import struct
import wave
from pathlib import Path

import numpy as np

from measure_point_suite import decode_wav
from measure_size_field import CENTERS, canonical_pcm
from measure_static_bank import file_sha256
from run_compat_suite import validate_reference

RATE = 48000
CHANNELS = 16
FFT = 4096
HOP = 1024


def wave_profile(path):
    result = {"chunks": []}
    data_size = None
    with path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[8:] != b"WAVE":
            raise ValueError("not a WAVE container")
        result["container"] = header[:4].decode("ascii")
        while header := stream.read(8):
            kind, size = struct.unpack("<4sI", header)
            result["chunks"].append(kind.decode("ascii"))
            if kind == b"ds64":
                payload = stream.read(size)
                data_size = struct.unpack_from("<Q", payload, 8)[0]
            elif kind == b"fmt ":
                payload = stream.read(size)
                tag, channels, rate = struct.unpack_from("<HHI", payload)
                result.update(format_tag=tag, channels=channels, sample_rate=rate,
                              bits_per_sample=struct.unpack_from("<H", payload, 14)[0],
                              mask=struct.unpack_from("<I", payload, 20)[0] if tag == 0xfffe else None)
            else:
                if kind == b"data" and size == 0xffffffff:
                    if data_size is None:
                        raise ValueError("RF64 data has no ds64 size")
                    size = data_size
                stream.seek(size, 1)
            stream.seek(size & 1, 1)
    result["dar_916_declaration"] = bool(result.get("format_tag") == 0xfffe and result.get("mask") == 0 and
        result.get("channels") == 16 and result.get("sample_rate") == RATE and
        "axml" not in result["chunks"] and "chna" not in result["chunks"])
    return result


def relative(reference, candidate):
    return float(np.linalg.norm(candidate - reference) / max(np.linalg.norm(reference), 1e-30))


def db(value):
    return float(10 * np.log10(max(float(value), 1e-30)))


def spectra(reference, candidate):
    # Batch Welch windows; only a few MB of temporary FFT storage at a time.
    window = np.hanning(FFT)
    frequencies = np.fft.rfftfreq(FFT, 1 / RATE)
    bands = [(frequencies >= center / 2 ** (1 / 6)) & (frequencies < center * 2 ** (1 / 6))
             for center in CENTERS]
    matrices = np.zeros((2, len(bands), CHANNELS, CHANNELS), dtype=np.complex128)
    starts = list(range(0, len(reference) - FFT + 1, HOP))
    for begin in range(0, len(starts), 16):
        selected = starts[begin:begin + 16]
        for engine, pcm in enumerate((reference, candidate)):
            frames = np.stack([pcm[start:start + FFT] for start in selected]).astype(float)
            values = np.fft.rfft(frames * window[None, :, None], axis=1)
            for band, active in enumerate(bands):
                flattened = values[:, active, :].reshape(-1, CHANNELS)
                matrices[engine, band] += flattened.conj().T @ flattened
    power = np.real(np.diagonal(matrices, axis1=-2, axis2=-1)).copy()
    trace = power.sum(axis=-1)
    normalized = matrices / np.maximum(trace[:, :, None, None], 1e-30)
    audible = power[0] >= trace[0, :, None] * 1e-4
    errors = 10 * np.log10(np.maximum(power[1], 1e-30) / np.maximum(power[0], 1e-30))
    cross_error = np.linalg.norm(normalized[1] - normalized[0], axis=(1, 2)) / np.maximum(
        np.linalg.norm(normalized[0], axis=(1, 2)), 1e-30)
    return {"fft_frames": FFT, "hop_frames": HOP, "window": "Hann", "windows": len(starts),
            "band_centers_hz": CENTERS, "effective_channel_threshold_db": -40,
            "max_effective_band_error_db": float(np.max(np.abs(errors[audible]))),
            "max_normalized_cross_spectrum_error": float(np.max(cross_error)),
            "band_error_db": errors.tolist(), "effective_channels": audible.tolist(),
            "normalized_cross_spectrum_error": cross_error.tolist()}, {
                "spectral_matrices": matrices, "spectral_power": power, "effective_channels": audible}


def input_metrics(path, reference, candidate, channels, frames):
    energy = np.zeros(channels)
    peak = np.zeros(channels)
    lfe_error = np.zeros(2)
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getframerate(), reader.getsampwidth(), reader.getnframes()) != (
                channels, RATE, 3, frames):
            raise ValueError("this comparison expects the original 48 kHz PCM24 ADM")
        start = 0
        while payload := reader.readframes(16384):
            octets = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            values = octets[:, 0] | (octets[:, 1] << 8) | (octets[:, 2] << 16)
            values = ((values ^ 0x800000) - 0x800000).reshape(-1, channels).astype(float) / (1 << 23)
            energy += np.sum(values * values, axis=0)
            peak = np.maximum(peak, np.max(np.abs(values), axis=0))
            stop = start + len(values)
            for engine, pcm in enumerate((reference, candidate)):
                lfe_error[engine] = max(lfe_error[engine], float(np.max(np.abs(pcm[start:stop, 3] - values[:, 3]))))
            start = stop
    return {"pcm_channel_energy": energy.tolist(), "pcm_channel_rms": np.sqrt(energy / frames).tolist(),
            "pcm_channel_peak": peak.tolist(), "bed_nonzero_channels": np.flatnonzero(energy[:10] > 0).tolist(),
            "object_nonzero_channels": (np.flatnonzero(energy[10:] > 0) + 10).tolist(),
            "lfe_source_max_abs_error": {"reference": lfe_error[0], "candidate": lfe_error[1]}}


def compare(reference, candidate, labels):
    count = len(reference)
    er, ec, ed, cross, sr, sc, maximum = (np.zeros(CHANNELS) for _ in range(7))
    envelopes = [[], []]
    unexpected_silence = 0
    equal_samples = 0
    # 20 ms windows, 1 ms hop, absolute channel energy (no input normalization).
    envelope_window, envelope_hop = 960, 48
    block = 48000
    for start in range(0, count, block):
        r = reference[start:start + block].astype(float)
        c = candidate[start:start + block].astype(float)
        d = c - r
        er += np.sum(r * r, axis=0)
        ec += np.sum(c * c, axis=0)
        ed += np.sum(d * d, axis=0)
        cross += np.sum(r * c, axis=0)
        sr += np.sum(r, axis=0)
        sc += np.sum(c, axis=0)
        maximum = np.maximum(maximum, np.max(np.abs(d), axis=0))
        unexpected_silence += int(np.count_nonzero((np.max(np.abs(r), axis=1) > 2 ** -22) &
                                                   (np.max(np.abs(c), axis=1) <= 2 ** -23)))
        equal_samples += int(np.count_nonzero(r == c))
        starts = np.arange(0, min(block, count - start - envelope_window + 1), envelope_hop)
        if len(starts):
            for engine, pcm in enumerate((reference, candidate)):
                signal = pcm[start:min(count, start + block + envelope_window)].astype(float)
                cumulative = np.vstack([np.zeros((1, CHANNELS)), np.cumsum(signal * signal, axis=0)])
                envelopes[engine].append(cumulative[starts + envelope_window] - cumulative[starts])
    envelopes = np.stack([np.concatenate(items) for items in envelopes])
    rows = []
    for channel, label in enumerate(labels):
        variance = (er[channel] - sr[channel] ** 2 / count) * (ec[channel] - sc[channel] ** 2 / count)
        correlation = float((cross[channel] - sr[channel] * sc[channel] / count) / math.sqrt(variance)) if variance > 0 else None
        rows.append({"channel": label, "reference_energy": er[channel], "candidate_energy": ec[channel],
                     "power_error_db": db(ec[channel] / er[channel]) if er[channel] > 0 else None,
                     "reference_rms_dbfs": db(er[channel] / count), "candidate_rms_dbfs": db(ec[channel] / count),
                     "residual_rms_dbfs": db(ed[channel] / count), "max_abs_sample_error": maximum[channel],
                     "residual_relative_l2": float(np.sqrt(ed[channel] / er[channel])) if er[channel] else None,
                     "correlation": correlation})
    # Diagnostic lag on the first eight seconds only; never applied to the comparison.
    length = min(count, 8 * RATE)
    fft_length = 1 << (2 * length - 1).bit_length()
    spectrum = np.zeros(fft_length // 2 + 1, dtype=complex)
    for channel in range(CHANNELS):
        a = np.fft.rfft(reference[:length, channel], fft_length)
        b = np.fft.rfft(candidate[:length, channel], fft_length)
        spectrum += a.conj() * b
    correlation = np.fft.irfft(spectrum, fft_length)
    lags = np.arange(-2048, 2049)
    lag = int(lags[np.argmax(correlation[lags % fft_length])])
    result = {"frames": count, "duration_seconds": count / RATE, "fixed_delay_samples": 0, "global_level": 1,
              "diagnostic_best_lag_samples": lag, "diagnostic_lag_applied": False,
              "residual_relative_l2": float(np.sqrt(ed.sum() / er.sum())),
              "residual_relative_db": db(ed.sum() / er.sum()), "total_power_error_db": db(ec.sum() / er.sum()),
              "energy_share_relative_l2": relative(er / er.sum(), ec / ec.sum()),
              "max_abs_sample_error": float(maximum.max()), "equal_sample_fraction": equal_samples / (count * CHANNELS),
              "unexpected_silent_frames": unexpected_silence,
              "envelope_window_frames": envelope_window, "envelope_hop_frames": envelope_hop,
              "energy_envelope_nrmse": relative(envelopes[0], envelopes[1]), "channels": rows}
    return result, {"envelopes": envelopes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--channel-map", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    identity = json.loads((root / "source-identity.json").read_text())
    adm = Path(identity["path"])
    validate_reference(root / "reference/run.json", adm, {"9.1.6"})
    run = json.loads((root / "reference/run.json").read_text())
    mapping = json.loads(args.channel_map.read_text())["layouts"]["9.1.6"]
    labels = [row["label"] for row in sorted(mapping["interleaved_to_mono"], key=lambda row: row["mono_index"])]
    reference_path = Path(run["outputs"][0]["path"])
    candidate_path = root / "room-compat-916.wav"
    reference = canonical_pcm(decode_wav(reference_path, CHANNELS, identity["frames"]), "9.1.6", mapping, False)
    candidate = canonical_pcm(decode_wav(candidate_path, CHANNELS, identity["frames"]), "9.1.6", mapping, True)
    report, arrays = compare(reference, candidate, labels)
    print("whole-program metrics", report["residual_relative_l2"], flush=True)
    report["spectral"], spectral_arrays = spectra(reference, candidate)
    arrays.update(spectral_arrays)
    report["source"] = input_metrics(adm, reference, candidate, identity["channels"], identity["frames"])
    report["source_adm_sha256"] = identity["sha256"]
    report["reference"] = {"path": str(reference_path), "sha256": file_sha256(reference_path)}
    report["candidate"] = {"path": str(candidate_path), "sha256": file_sha256(candidate_path), "build": "Release"}
    report["container_profiles"] = {"reference": wave_profile(reference_path), "candidate": wave_profile(candidate_path)}
    report["container_behavior_matches"] = all(row["dar_916_declaration"] for row in report["container_profiles"].values())
    audit_path = root / "layout-audit/result.json"
    if audit_path.exists():
        audit = json.loads(audit_path.read_text())
        if (audit["files"]["fixed"]["file_sha256"] == report["candidate"]["sha256"] and
                audit["files"]["reference_wav"]["file_sha256"] == report["reference"]["sha256"]):
            report["layout_correction"] = {"pcm_unchanged": audit["pcm_unchanged"], "audit": str(audit_path),
                                           "candidate_caf": audit["files"]["candidate_caf"]["path"],
                                           "reference_caf": audit["files"]["reference_caf"]["path"]}
    report["analyzer_sha256"] = file_sha256(Path(__file__))
    report["mapping_sha256"] = file_sha256(args.channel_map)
    report["state_restored"] = (run["initial"]["master"] == run["final"]["master"] and
                                 run["initial"]["config"] == run["final"]["config"] and
                                 run["initial"]["exporter"] == run["final"]["exporter"] and
                                 not run["restore_errors"] and run["baseline_settings_sha256"] == run["settings_sha256_after"])
    report["programme_comparison_passes"] = bool(report["energy_share_relative_l2"] <= .05 and
        abs(report["total_power_error_db"]) <= .1 and report["spectral"]["max_effective_band_error_db"] <= .5 and
        report["spectral"]["max_normalized_cross_spectrum_error"] <= .05 and report["energy_envelope_nrmse"] <= .02 and
        report["unexpected_silent_frames"] == 0 and report["source"]["lfe_source_max_abs_error"]["candidate"] <= 1e-7 and
        report["state_restored"])
    report["complete_comparison_passes"] = report["programme_comparison_passes"] and report["container_behavior_matches"]
    report["limits"] = "One real programme with static metadata; no dedicated silent-tail or dynamic-event acceptance."
    np.savez_compressed(root / "measurements.npz", **arrays)
    (root / "comparison.json").write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    print(json.dumps({key: report[key] for key in ("programme_comparison_passes", "residual_relative_l2",
                    "total_power_error_db", "energy_envelope_nrmse", "state_restored")}, indent=2))


if __name__ == "__main__":
    main()
