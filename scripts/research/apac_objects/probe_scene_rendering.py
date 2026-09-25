#!/usr/bin/env python3
"""Check every object of one encoded scene in the stock real-time spatial mixer.

This small synthetic probe reuses the scene's saved ASC and AIA positions. Each
object gets a distinct tone; source audio is not copied or transmitted.
"""

import argparse
import json
import os
from pathlib import Path

import numpy as np

from run_case import run_case
from wrap_caf import wrap


def run(scene, output, codec, player, observer):
    output.mkdir(parents=True, exist_ok=False)
    expected = json.loads((scene / "input/expected.json").read_text())
    objects, channels = expected["objects"], expected["channels"]
    if expected["metadata_channels"] != 1 or expected["discard_diffuse"] is not True:
        raise ValueError("one-channel position-only scene expected")
    rate, frames = 48000, 49152
    time = np.arange(frames) / rate
    pcm = np.zeros((frames, channels + 1), dtype="<f4")
    frequency = []
    for channel in range(channels):
        wanted = 60 if channel == 3 else 250 + 71 * channel if channel < 10 else 1100 + 123 * (channel - 10)
        hz = round(wanted * frames / rate) * rate / frames
        pcm[:, channel] = (.004 if channel < 10 else .01) * np.sin(2 * np.pi * hz * time)
        frequency.append(hz)
    blob = (scene / "input/metadata.aia").read_bytes()
    words = np.frombuffer(blob + b"\0" * (len(blob) % 2), dtype="<i2").astype("<f4") / 32768
    if len(words) > 1024:
        raise ValueError("metadata exceeds one PCM frame")
    for first in range(0, frames, 1024):
        pcm[first:first + len(words), channels] = words
    pcm.tofile(output / "input.f32")
    (output / "settings.plist").write_bytes((scene / "input/settings.plist").read_bytes())
    prefix = output / "encode/stream"
    result = run_case([str(codec), "encode", str(output / "settings.plist"), str(output / "input.f32"),
                       str(channels + 1), str(channels), str(prefix), "0", "12000000"], output / "encode", 30)
    if result["returncode"]:
        raise RuntimeError("probe encode failed")
    caf, mp4 = output / "objects.caf", output / "objects.mp4"
    wrap(prefix, caf)
    result = run_case(["/usr/bin/afconvert", str(caf), str(mp4), "-f", "mp4f", "-d", "0"], output / "wrap", 30)
    if result["returncode"]:
        raise RuntimeError("probe MP4 wrap failed")
    os.environ["DYLD_INSERT_LIBRARIES"] = str(observer)
    os.environ["APAC_SPATIAL_OBSERVER"] = str(output / "player/capture")
    try:
        result = run_case([str(player), str(mp4), str(output / "player/format.plist")], output / "player", 30)
    finally:
        os.environ.pop("DYLD_INSERT_LIBRARIES", None)
        os.environ.pop("APAC_SPATIAL_OBSERVER", None)
    captures = list((output / "player").glob("capture-unit*.f32"))
    if result["returncode"] or len(captures) != 1:
        raise RuntimeError("spatial output observation failed")
    rendered = np.fromfile(captures[0], dtype="<f4").reshape(-1, 2).astype(float)
    steady = rendered[round(.2 * rate):round(.8 * rate)]
    if len(steady) != round(.6 * rate):
        raise RuntimeError("captured too few steady frames")
    object_hz = np.array(frequency[10:])
    phase = 2 * np.pi * np.arange(len(steady))[:, None] / rate * object_hz[None, :]
    basis = np.concatenate((np.cos(phase), np.sin(phase)), axis=1)
    coefficients = np.linalg.lstsq(basis, steady, rcond=None)[0]
    amplitudes = np.hypot(coefficients[:objects], coefficients[objects:])
    detected = np.flatnonzero(np.max(amplitudes, axis=1) > 8e-5).tolist()
    summary = {"scene": str(scene), "objects": objects, "bed_channels": 10,
               "object_frequencies_hz": object_hz.tolist(), "steady_window_seconds": [.2, .8],
               "detection_amplitude_threshold": 8e-5, "detected_object_indices": detected,
               "object_ear_amplitudes": amplitudes.tolist(), "all_objects_present": detected == list(range(objects)),
               "player_modified": False, "decoder_modified": False,
               "observation": "system AVPlayer and AUSpatialMixer output before self-owned probe mute"}
    (output / "verification.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"objects": objects, "detected": detected, "passed": summary["all_objects_present"]}))
    if not summary["all_objects_present"]:
        raise RuntimeError("not all objects reached the real-time spatial output")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--codec", type=Path, required=True)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--observer", type=Path, required=True)
    args = parser.parse_args()
    run(*[getattr(args, name).resolve() for name in ("scene", "output", "codec", "player", "observer")])
