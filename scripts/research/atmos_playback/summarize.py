"""Summarize one recorded experiment; package five seconds of captured PCM as CAF.

This does not decode or render media. The reference comes from a real-time pre
effects tap. Requires the trace/capture directory described in README.md.
"""

import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np


def events(path):
    result = []
    for line in path.read_text().splitlines():
        try:
            result.append(json.loads(line))
        except ValueError:
            pass
    return result


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_caf(path, pcm, layout_tag):
    """CoreAudio writes its native channel order and layout, without conversion."""
    class ASBD(C.Structure):
        _fields_ = [("rate", C.c_double)] + [(name, C.c_uint32) for name in
            ("format", "flags", "packet_bytes", "packet_frames", "frame_bytes", "channels", "bits", "reserved")]

    cf = C.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    audio = C.CDLL("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox")
    cf.CFURLCreateFromFileSystemRepresentation.argtypes = [C.c_void_p, C.c_char_p, C.c_long, C.c_ubyte]
    cf.CFURLCreateFromFileSystemRepresentation.restype = C.c_void_p
    cf.CFRelease.argtypes = [C.c_void_p]
    audio.AudioFileCreateWithURL.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(ASBD), C.c_uint32, C.POINTER(C.c_void_p)]
    audio.AudioFileSetProperty.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_void_p]
    audio.AudioFileWritePackets.argtypes = [C.c_void_p, C.c_ubyte, C.c_uint32, C.c_void_p,
                                           C.c_int64, C.POINTER(C.c_uint32), C.c_void_p]
    audio.AudioFileClose.argtypes = [C.c_void_p]
    code = lambda text: int.from_bytes(text.encode("ascii"), "big")
    encoded = str(path.resolve()).encode()
    url = cf.CFURLCreateFromFileSystemRepresentation(None, encoded, len(encoded), False)
    handle = C.c_void_p()
    channels = pcm.shape[1]
    asbd = ASBD(48000, code("lpcm"), 9, 4 * channels, 1, 4 * channels, channels, 32, 0)

    def check(status):
        if status:
            raise RuntimeError(f"AudioFile OSStatus {status}")

    try:
        # Flags=0 refuses to erase an existing file.
        check(audio.AudioFileCreateWithURL(url, code("caff"), C.byref(asbd), 0, C.byref(handle)))
        layout = (C.c_uint32 * 3)(layout_tag, 0, 0)
        check(audio.AudioFileSetProperty(handle, code("cmap"), C.sizeof(layout), layout))
        count = C.c_uint32(len(pcm))
        check(audio.AudioFileWritePackets(handle, False, pcm.nbytes, None, 0, C.byref(count), pcm.ctypes.data))
        if count.value != len(pcm):
            raise RuntimeError("Short CAF write")
    finally:
        if handle:
            check(audio.AudioFileClose(handle))
        if url:
            cf.CFRelease(url)


def main():
    root = Path(sys.argv[1]).resolve()
    if (root / "summary-final.json").exists():
        raise RuntimeError("Refusing to replace existing summary")
    report = {"os": "macOS 27.0 (26A428)", "reference_interval_seconds": [57, 62],
              "channel_order": "L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr".split(),
              "layout_tag": 0xC0000C, "traces": {}, "captures": {}}
    for mode in ("none", "pre"):
        generic = events(root / f"trace-{mode}.log")
        dolby = events(root / f"dolby-v2-{mode}.log")
        layouts = [struct.unpack_from("<I", bytes.fromhex(e["value_hex"]))[0] for e in generic
                   if e.get("stage") == "call" and e.get("property") == "ocl "]
        if not layouts or any(tag != report["layout_tag"] for tag in layouts):
            raise RuntimeError(f"Unexpected codec output layout: {layouts}")
        report["traces"][mode] = dict(output_layout_tags=layouts, functions=[e["function"] for e in dolby
            if e.get("stage") == "call" and e.get("count") == 1], errors=[e for e in dolby if "read_error" in e],
            player_finished=[e for e in dolby if e.get("stage") == "player_finished"])

    arrays = {}
    metadata_by_mode = {}
    for mode in ("pre", "post", "audible-pre", "audible-pre-repeat"):
        metadata = json.loads((root / mode / "tap.json").read_text())
        metadata_by_mode[mode] = metadata
        if metadata["rejected_blocks"] or metadata["overflow_blocks"]:
            raise RuntimeError("Invalid capture")
        if any(f["channels"] != 12 or f["sample_rate"] != 48000 for f in metadata["formats"]):
            raise RuntimeError("Unexpected tap format")
        arrays[mode] = pcm = np.fromfile(root / mode / "tap.f32", dtype="<f4").reshape(-1, 12)
        if not np.isfinite(pcm).all():
            raise RuntimeError("Non-finite capture")
        report["captures"][mode] = dict(frames=len(pcm), formats=metadata["formats"],
            pcm_sha256=digest(root / mode / "tap.f32"), callbacks=len(metadata["blocks"]))
        if mode.startswith("audible-"):
            player = next(e for e in events(root / f"{mode}.log") if e.get("stage") == "player_created")
            if player["muted"]:
                raise RuntimeError("Quantitative reference must use normal audible playback")

    a, b = arrays["pre"], arrays["post"]
    # A distinctive common peak proposes the delay; every overlapping sample is
    # then checked, so a wrong peak cannot be accepted as a successful match.
    lag = int(np.argmax(abs(b[:, 0]))) - int(np.argmax(abs(a[:, 0])))
    first, last = max(0, -lag), min(len(a), len(b) - lag)
    x, y = a[first:last], b[first + lag:last + lag]
    report["pre_post_comparison"] = dict(post_delay_frames=lag, overlap_frames=len(x),
        different_samples=int(np.count_nonzero(x != y)), max_abs_error=float(np.max(abs(x - y))),
        limitation="Muted runs; equality does not prove complete source audio beyond initial prefetch")

    # Normal playback covers 56--63 seconds. Retain 57--62, excluding both
    # startup/prefetch and the player's end-of-playback muting/fade.
    clips = []
    for mode in ("audible-pre", "audible-pre-repeat"):
        clip = np.zeros((5 * 48000, 12), dtype="<f4")
        coverage = np.zeros(len(clip), dtype=np.uint8)
        for block in metadata_by_mode[mode]["blocks"]:
            if block["status"] or block["start"] is None or not block["duration"] or block["duration"] <= 0:
                continue
            offset = round((block["start"] - 57) * 48000)
            count = min(block["frames"], round(block["duration"] * 48000))
            begin, end = max(0, offset), min(len(clip), offset + count)
            if begin >= end:
                continue
            source = block["sample_offset"] // 12 + begin - offset
            clip[begin:end] = arrays[mode][source:source + end - begin]
            coverage[begin:end] += 1
        if not np.all(coverage == 1):
            raise RuntimeError("Reference timestamps overlap or leave a gap")
        clips.append(clip)
    clip, repeated = clips
    report["audible_repeat_comparison"] = dict(frames=len(clip),
        different_samples=int(np.count_nonzero(clip != repeated)),
        max_abs_error=float(np.max(abs(clip - repeated))),
        active_frames=int(np.count_nonzero(np.any(clip != 0, axis=1))))
    if report["audible_repeat_comparison"]["different_samples"]:
        raise RuntimeError("Normal playback is not repeatable; investigate before publishing a reference")
    destination = root / "tsuioku-57s-62s-apple-atmos-714.caf"
    write_caf(destination, np.ascontiguousarray(clip), report["layout_tag"])
    report["reference"] = dict(path=str(destination), frames=len(clip), sample_rate=48000, channels=12,
        pcm_sha256=hashlib.sha256(clip.tobytes()).hexdigest(), caf_sha256=digest(destination),
        rms=np.sqrt(np.mean(clip.astype(np.float64) ** 2, axis=0)).tolist(),
        peak=np.max(abs(clip), axis=0).tolist(),
        method="Repackage real-time pre-effects tap PCM; no decode, render, normalization, or resampling")
    asset = next(e for e in events(root / "pre.log") if e.get("stage") == "asset")
    report["source"] = {**asset, "sha256": digest(Path(asset["path"]))}
    (root / "summary-final.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({k: report[k] for k in ("pre_post_comparison", "audible_repeat_comparison", "reference")},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
