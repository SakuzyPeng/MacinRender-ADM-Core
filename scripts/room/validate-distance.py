#!/usr/bin/env python3
"""Create a deterministic ADM distance audition with a Release mradm executable.

The private HRTF collection is read in place. Small room assets are cloned into a new
validation directory; the source room's minimum distance is never modified.
Requires the existing NumPy environment, not a separate dependency cache.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

import numpy as np


def chunk(name, payload):
    return name + struct.pack("<I", len(payload)) + payload + b"\0" * (len(payload) % 2)


def source(path):
    rate, seconds = 48000, 14
    noise = np.random.default_rng(20261014).standard_normal(rate * seconds)
    hz = np.fft.rfftfreq(len(noise), 1 / rate)
    shape = np.sqrt(100 / np.maximum(hz, 100)) * np.exp(-(hz / 12000) ** 8)
    shape[hz < 60] = 0
    pcm = np.fft.irfft(np.fft.rfft(noise) * shape, n=len(noise))
    pcm *= .015 / np.max(np.abs(pcm))
    fade = np.linspace(0, 1, 2400)
    pcm[:2400] *= fade
    pcm[-2400:] *= fade[::-1]
    timeline = [(0, 1, 60, 1.6, 0), (1, 3, 60, .2, 3), (4, 3, 60, 1.6, 3),
                (7, 1, -60, 1.6, 0), (8, 3, -60, .2, 3), (11, 3, -60, 1.6, 3)]
    blocks = []
    for index, (start, duration, azimuth, distance, interpolation) in enumerate(timeline, 1):
        blocks.append(f'''<audioBlockFormat audioBlockFormatID="AB_00031001_{index:08x}"
 rtime="00:00:{start:09.6f}" duration="00:00:{duration:09.6f}">
<position coordinate="azimuth">{azimuth}</position><position coordinate="elevation">0</position>
<position coordinate="distance">{distance}</position>
<jumpPosition interpolationLength="{interpolation}">0</jumpPosition></audioBlockFormat>''')
    xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<ebuCoreMain><coreMetadata><format><audioFormatExtended version="ITU-R_BS.2076-2">
<audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="Distance validation"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme>
<audioContent audioContentID="ACO_1001" audioContentName="Noise"><audioObjectIDRef>AO_1001</audioObjectIDRef></audioContent>
<audioObject audioObjectID="AO_1001" audioObjectName="Distance sweep" start="00:00:00.000000" duration="00:00:14.000000"><audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef><audioTrackUIDRef>ATU_00000001</audioTrackUIDRef></audioObject>
<audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16"><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef><audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef></audioTrackUID>
<audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="Track" formatLabel="0001" formatDefinition="PCM"><audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef></audioTrackFormat>
<audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="Stream" formatLabel="0001" formatDefinition="PCM"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef></audioStreamFormat>
<audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="Pack" typeLabel="0003" typeDefinition="Objects"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioPackFormat>
<audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Channel" typeLabel="0003" typeDefinition="Objects">{''.join(blocks)}</audioChannelFormat>
</audioFormatExtended></format></coreMetadata></ebuCoreMain>'''.encode()
    chna = struct.pack("<HHH12s14s11sB", 1, 1, 1, b"ATU_00000001", b"AT_00031001_01", b"AP_00031001", 0)
    payload = chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16))
    payload += chunk(b"axml", xml) + chunk(b"chna", chna)
    payload += chunk(b"data", np.round(pcm * 32767).astype("<i2").tobytes())
    path.write_bytes(b"RIFF" + struct.pack("<I", len(payload) + 4) + b"WAVE" + payload)
    return timeline


def samples(path):
    data = path.read_bytes()
    offset, large_size = 12, None
    while offset + 8 <= len(data):
        name, size = struct.unpack_from("<4sI", data, offset)
        offset += 8
        if name == b"ds64":
            large_size = struct.unpack_from("<Q", data, offset + 8)[0]
        if name == b"data":
            if size == 0xffffffff:
                size = large_size
            return np.frombuffer(data[offset:offset + size], dtype="<f4").reshape(-1, 2)
        offset += size + size % 2
    raise ValueError("Missing rendered PCM")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mradm", type=Path, required=True)
    parser.add_argument("--room", type=Path, required=True)
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    executable, collection_path = args.mradm.resolve(), args.collection.resolve()
    if "debug" in executable.parts:
        parser.error("Audition renders require Release")
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output.resolve()
    room_path = args.room.resolve()
    room = json.loads(room_path.read_text())
    room["id"] += "-distance-validation"
    room["name"] = "Distance validation, 0.2 metre minimum"
    room["minimum_distance"] = .2
    for asset in room["assets"].values():
        source_asset, target = room_path.parent / asset["file"], root / asset["file"]
        if sys.platform != "darwin" or subprocess.run(
                ["/bin/cp", "-c", str(source_asset), str(target)], capture_output=True).returncode:
            shutil.copyfile(source_asset, target)
    (root / "room.json").write_text(json.dumps(room, indent=2) + "\n")
    timeline = source(root / "source.wav")
    collection = json.loads(collection_path.read_text())
    reference = next(layer for layer in collection["layers"] if layer["reference_layer"])
    report = dict(schema="mradm.distance-audition", version=1, profile="release", sample_rate_hz=48000,
                  material_seconds=14, seed=20261014, timeline=timeline, coordinate_metres_per_unit=1,
                  propagation="same physical distance in both renders", loudness_normalization=False,
                  peak_limiter=False, reference_radius_m=reference["radius_m"],
                  source_sha256=hashlib.sha256((root / "source.wav").read_bytes()).hexdigest(), renders={})
    for name, hrir in [("multi-distance", collection_path),
                       ("fixed-150cm", collection_path.parent / reference["primary"])]:
        prepared = json.loads(subprocess.check_output(
            [str(executable), "room-check", str(root / "room.json"), "--hrir", str(hrir)], text=True))
        command = [str(executable), "render", "-i", str(root / "source.wav"), "-o", str(root / f"{name}.wav"),
                   "--renderer", "room-binaural", "--room", str(root / "room.json"), "--room-hrir", str(hrir),
                   "--room-metres-per-unit", "1", "--output-bit-depth", "f32", "--no-peak-limit", "--interp-ms", "0",
                   "--write-semantic-report", str(root / f"{name}-semantics.json")]
        started = time.perf_counter()
        measured = ["/usr/bin/time", "-l", *command] if sys.platform == "darwin" else command
        result = subprocess.run(measured, capture_output=True, text=True)
        elapsed = time.perf_counter() - started
        (root / f"{name}.log").write_text(result.stdout + result.stderr)
        result.check_returncode()
        pcm = samples(root / f"{name}.wav")
        if not np.isfinite(pcm).all() or not np.any(pcm):
            raise ValueError("Invalid output PCM")
        expected_frames = report["material_seconds"] * report["sample_rate_hz"] + prepared["tail_frames"]
        if len(pcm) != expected_frames:
            raise ValueError(f"{name}: expected {expected_frames} frames, got {len(pcm)}")
        rss = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
        report["renders"][name] = dict(frames=len(pcm), peak=float(np.max(np.abs(pcm))),
                                       rms=float(np.sqrt(np.mean(pcm.astype(float) ** 2))),
                                       elapsed_seconds=elapsed, maximum_rss_bytes=int(rss[1]) if rss else None,
                                       hrir_interpolation_guard_frames=prepared["hrir_interpolation_guard_frames"],
                                       hrir_preprocessing_guard_frames=prepared["hrir_preprocessing_guard_frames"],
                                       sha256=hashlib.sha256((root / f"{name}.wav").read_bytes()).hexdigest())
    # Both renders use the same room. Only the declared HRIR protection delays
    # can differ; physical propagation and measured HRIR delays stay intact.
    guards = {name: render["hrir_interpolation_guard_frames"] + render["hrir_preprocessing_guard_frames"]
              for name, render in report["renders"].items()}
    common_guard = max(guards.values())
    leading = {name: common_guard - guard for name, guard in guards.items()}
    length = max(render["frames"] + leading[name] for name, render in report["renders"].items())
    aligned = {}
    padding = {}
    for name, render in report["renders"].items():
        trailing = length - render["frames"] - leading[name]
        aligned[name] = np.pad(samples(root / f"{name}.wav").astype(np.float64),
                               ((leading[name], trailing), (0, 0)))
        padding[name] = dict(leading_frames=leading[name], trailing_frames=trailing)
    a, b = aligned["multi-distance"], aligned["fixed-150cm"]
    report["relative_pcm_difference"] = float(np.linalg.norm(a - b) / np.linalg.norm(b))
    report["comparison"] = dict(alignment="declared-hrir-guards", common_hrir_guard_frames=common_guard,
                                frames=length, zero_padding=padding)
    report["scope"] = "Deterministic Release comparison; listening judgement and acoustic accuracy remain separate."
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
