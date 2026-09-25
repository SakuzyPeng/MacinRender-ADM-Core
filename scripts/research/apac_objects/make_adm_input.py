#!/usr/bin/env python3
"""Stage a strictly checked static 7.1.2 BED + mono-object ADM for APAC research.

This is deliberately not a general ADM converter. Unknown fields, dynamic blocks,
non-unity gains, ambiguous routing and other layouts fail closed. Audio is copied
track-for-track; ADM Cartesian positions are geometrically converted to spherical
APAC coordinates. Cartesian width/height/depth and diffuse use separate parameters.
"""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import plistlib
import struct
import xml.etree.ElementTree as ET

import numpy as np

from make_inputs import Bits


BED = [
    ("RC_L", "Left", 1), ("RC_R", "Right", 2), ("RC_C", "Center", 3),
    ("RC_LFE", "LFEScreen", 4), ("RC_Lss", "LeftSurround", 5),
    ("RC_Rss", "RightSurround", 6), ("RC_Lrs", "RearSurroundLeft", 33),
    ("RC_Rrs", "RearSurroundRight", 34), ("RC_Lts", "LeftTopMiddle", 49),
    ("RC_Rts", "RightTopMiddle", 51),
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def seconds(value):
    h, m, s = value.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def inspect(path):
    chunks = {}
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(12)
        require(header[:4] == b"RIFF" and header[8:] == b"WAVE", "only RIFF/WAVE supported")
        require(struct.unpack_from("<I", header, 4)[0] + 8 == size, "RIFF size mismatch")
        while stream.tell() + 8 <= size:
            kind, length = struct.unpack("<4sI", stream.read(8))
            offset = stream.tell()
            require(offset + length <= size and kind not in chunks, "invalid/duplicate WAV chunk")
            chunks[kind] = (offset, length)
            stream.seek(offset + length + (length & 1))
        def read(kind):
            offset, length = chunks[kind]
            stream.seek(offset)
            return stream.read(length)
        fmt = struct.unpack("<HHIIHH", read(b"fmt ")[:16])
        require(fmt[0] == 1 and fmt[2] == 48000 and fmt[5] == 24, "48 kHz signed PCM24 required")
        require(fmt[4] == 3 * fmt[1] and fmt[3] == 48000 * fmt[4], "invalid PCM alignment")
        axml, chna = read(b"axml"), read(b"chna")
    root = ET.fromstring(axml)
    for element in root.iter():
        element.tag = element.tag.rsplit("}", 1)[-1]
    allowed = {
        "ebuCoreMain", "coreMetadata", "format", "audioFormatExtended", "audioProgramme",
        "audioContent", "audioContentIDRef", "audioObjectIDRef", "dialogue", "audioObject",
        "audioTrackUIDRef", "audioPackFormatIDRef", "audioPackFormat", "audioChannelFormatIDRef",
        "audioChannelFormat", "audioBlockFormat", "speakerLabel", "cartesian", "position",
        "jumpPosition", "width", "height", "depth", "diffuse", "audioStreamFormat",
        "audioTrackFormatIDRef", "audioTrackFormat", "audioStreamFormatIDRef", "audioTrackUID",
    }
    require(not ({e.tag for e in root.iter()} - allowed), "unsupported ADM fields present")
    tables = {kind: {e.get(attr).upper(): e for e in root.iter(kind)} for kind, attr in (
        ("audioTrackUID", "UID"), ("audioTrackFormat", "audioTrackFormatID"),
        ("audioStreamFormat", "audioStreamFormatID"), ("audioChannelFormat", "audioChannelFormatID"),
        ("audioObject", "audioObjectID"), ("audioPackFormat", "audioPackFormatID"),
    )}
    owners = {}
    frames = chunks[b"data"][1] // fmt[4]
    require(chunks[b"data"][1] % fmt[4] == 0, "partial PCM frame")
    for obj in tables["audioObject"].values():
        require(set(obj.attrib) <= {"audioObjectID", "audioObjectName", "start", "duration"},
                "unsupported object attributes")
        require(abs(seconds(obj.get("start", "00:00:00"))) < 1 / 48000, "object starts after sample zero")
        require(abs(seconds(obj.get("duration", "00:00:00")) * 48000 - frames) <= 1,
                "object duration differs from audio")
        for ref in obj.findall("audioTrackUIDRef"):
            key = ref.text.upper()
            require(key not in owners, "shared track UID")
            owners[key] = obj
    count, uids = struct.unpack_from("<HH", chna)
    require(count == uids == fmt[1] and len(chna) == 4 + 40 * uids, "ambiguous CHNA")
    tracks = []
    for index in range(uids):
        channel, uid, track_ref, pack_ref = struct.unpack_from("<H12s14s11sx", chna, 4 + 40 * index)
        uid = uid.decode().rstrip("\0").upper()
        track_ref = track_ref.decode().rstrip("\0").upper()
        pack_ref = pack_ref.decode().rstrip("\0").upper()
        track = tables["audioTrackUID"][uid]
        require(track.findtext("audioTrackFormatIDRef").upper() == track_ref, "CHNA track mismatch")
        require(track.findtext("audioPackFormatIDRef").upper() == pack_ref, "CHNA pack mismatch")
        stream_ref = tables["audioTrackFormat"][track_ref].findtext("audioStreamFormatIDRef").upper()
        channel_ref = tables["audioStreamFormat"][stream_ref].findtext("audioChannelFormatIDRef").upper()
        cf = tables["audioChannelFormat"][channel_ref]
        blocks = cf.findall("audioBlockFormat")
        require(len(blocks) == 1, "dynamic ADM is not supported by this fixture adapter")
        block = blocks[0]
        require(block.findtext("cartesian") == "1", "Cartesian ADM required")
        require(seconds(block.get("rtime", "00:00:00")) == 0, "nonzero block start")
        if "duration" in block.attrib:
            require(abs(seconds(block.get("duration")) * 48000 - frames) <= 1, "block duration mismatch")
        positions = {e.get("coordinate"): float(e.text) for e in block.findall("position")}
        require(set(positions) <= {"X", "Y", "Z"}, "unsupported position coordinate")
        xyz = [positions.get(axis, 0.0) for axis in "XYZ"]
        require(all(math.isfinite(v) for v in xyz), "nonfinite position")
        obj = owners[uid]
        tracks.append({"channel": channel - 1, "uid": uid, "object_id": obj.get("audioObjectID"),
                       "name": obj.get("audioObjectName"), "type": cf.get("typeDefinition"),
                       "speaker_label": block.findtext("speakerLabel"), "xyz": xyz,
                       "width": float(block.findtext("width", "0")),
                       "height": float(block.findtext("height", "0")),
                       "depth": float(block.findtext("depth", "0")),
                       "diffuse": float(block.findtext("diffuse", "0"))})
    tracks.sort(key=lambda t: t["channel"])
    require([t["channel"] for t in tracks] == list(range(count)), "channel routing is not bijective")
    require([t["speaker_label"] for t in tracks[:10]] == [row[0] for row in BED], "not the 7.1.2 BED order")
    require(all(t["type"] == "DirectSpeakers" for t in tracks[:10]), "invalid BED types")
    require(all(t["type"] == "Objects" for t in tracks[10:]), "unsupported remaining track type")
    require(len({t["object_id"] for t in tracks[:10]}) == 1, "BED is not one source object")
    require(len({t["object_id"] for t in tracks[10:]}) == count - 10, "objects must be mono")
    for group_id, track in enumerate(tracks[10:], 1):
        x, y, z = track["xyz"]
        radius = math.sqrt(x * x + y * y + z * z)
        require(0 < radius <= 2, "position outside fixture range")
        track["group_id"] = group_id
        track["spherical"] = [math.degrees(math.atan2(-x, y)), math.degrees(math.asin(z / radius)), radius]
        require(all(0 <= track[k] <= 1 for k in ("width", "height", "depth", "diffuse")), "invalid extent/diffuse")
    return {"source": str(path.resolve()), "source_bytes": size, "sample_rate": 48000, "channels": count,
            "source_frames": frames, "data_offset": chunks[b"data"][0], "tracks": tracks,
            "axml_sha256": hashlib.sha256(axml).hexdigest(), "chna_sha256": hashlib.sha256(chna).hexdigest(),
            "bed_tag": "kAudioChannelLayoutTag_Atmos_7_1_2", "bed_tag_value": (196 << 16) | 10,
            "bed_labels": [row[2] for row in BED], "objects": count - 10,
            "conversion": "Cartesian ADM -> geometric spherical APAC; no loudspeaker rendering or room warp",
            "source_bed_coordinates": "speaker identity uses standard labels, not the room-corner XYZ placeholders"}


def object_position(bits, spherical):
    bits.put(0, 1)
    bits.put(0, 6)
    bits.put(0, 1)
    bits.floating(2.0)  # all source radii fit this quantizer range
    bits.put(8, 5)
    bits.put(2, 5)
    bits.put(1, 1)
    bits.put(0, 1)
    for value in spherical:
        bits.floating(value)
    bits.put(0, 1)
    bits.put(0, 1)


def metadata(tracks):
    bits = Bits()
    bits.put(1 + len(tracks), 11)
    bits.put(0, 1)  # the bed has a fixed layout
    for track in tracks:
        bits.put(1, 1)
        bits.put(track["group_id"], 11)
        bits.put(0, 3)
        bits.put(0, 1)
        bits.put(0, 1)
        has_extent = any(track[k] for k in ("width", "height", "depth"))
        has_diffuse = track["diffuse"] != 0
        bits.put(1 + has_extent + has_diffuse, 11)
        bits.put(0, 11)
        object_position(bits, track["spherical"])
        if has_extent:
            bits.put(1, 11)  # Cartesian spread (RendererData switch selector 1)
            bits.put(0, 1)  # Cartesian rather than angular spread
            bits.put(0, 1)
            bits.floating(1.0)
            bits.put(8, 5)
            bits.floating(track["width"])
            bits.floating(track["height"])
            bits.put(1, 1)  # depth present
            bits.floating(track["depth"])
        if has_diffuse:
            bits.put(3, 11)  # ObjectDiffuse::Parse
            bits.put(1, 1)
            bits.floating(track["diffuse"])
    payload = bits.finish()
    body = b"\x00\x01apdd" + struct.pack(">H", len(payload)) + payload
    return b"\xff\xff" + struct.pack(">H", 7 + len(body)) + b"\x01\x04\x00" + body


def pcm24(raw, channels):
    values = np.frombuffer(raw, dtype=np.uint8).reshape(-1, channels, 3).astype(np.int32)
    values = values[:, :, 0] | (values[:, :, 1] << 8) | (values[:, :, 2] << 16)
    return ((values ^ 0x800000) - 0x800000).astype(np.float32) / 8388608.0


def component_bitrates(object_groups, total_bitrate=None):
    """Distribute a total budget by full-band track count, reserving 16 kbps for LFE.

    These are per-component budgets; they do not force the codec's internal
    per-track allocation. Integer remainders keep their sum exactly on target.
    """
    weights = [9, *object_groups]
    full_band_channels = sum(weights)
    target = 256000 * full_band_channels + 16000 if total_bitrate is None else total_bitrate
    require(isinstance(target, int) and 16000 + full_band_channels <= target <= 0xffffffff,
            "total bitrate must fit UInt32 and leave a positive budget for all full-band tracks")
    shares = [divmod((target - 16000) * weight, full_band_channels) for weight in weights]
    rates = [quotient for quotient, _ in shares]
    remainder = target - 16000 - sum(rates)
    order = sorted(range(len(weights)), key=lambda index: (-shares[index][1], index))
    for index in order[:remainder]:
        rates[index] += 1
    rates[0] += 16000
    require(sum(rates) == target, "component budget sum mismatch")
    return rates


def generate(source, output, start=0, frames=None, omit_fully_diffuse_extent=False, total_bitrate=None,
             objects_per_component=7, discard_diffuse=False):
    info = inspect(source)
    require(1 <= objects_per_component <= 7, "default-capability groups support 1..7 objects per component")
    require(0 <= start < info["source_frames"], "invalid excerpt start")
    frames = info["source_frames"] - start if frames is None else frames
    require(0 < frames <= info["source_frames"] - start, "invalid excerpt length")
    metadata_tracks = copy.deepcopy(info["tracks"][10:])
    omitted = []
    if omit_fully_diffuse_extent:
        for track in metadata_tracks:
            if any(track[k] for k in ("width", "height", "depth")):
                require(track["diffuse"] == 1, "extent fallback restricted to fully diffuse source objects")
                omitted.append({k: track[k] for k in ("channel", "object_id", "name", "width", "height", "depth")})
                for key in ("width", "height", "depth"):
                    track[key] = 0.0
    discarded_diffuse = []
    if discard_diffuse:
        for track in metadata_tracks:
            if track["diffuse"]:
                discarded_diffuse.append({k: track[k] for k in ("channel", "object_id", "name", "diffuse")})
            track["diffuse"] = 0.0
    blob = metadata(metadata_tracks)
    metadata_channels = (len(blob) + 2047) // 2048
    require(metadata_channels == 1, "this real-media adapter currently supports one metadata channel")
    channels = info["channels"]
    components = [{"key": "Channel Bed", "Channel Bed": [
        {"key": "ChannelLayoutLabel", "current value": ["kAudioChannelLabel_" + row[1] for row in BED]},
        {"key": "Channel Map", "current minimum range": 0, "current maximum range": 9}]}]
    groups = []
    for offset in range(10, channels, objects_per_component):
        count = min(objects_per_component, channels - offset)
        groups.append(count)
        components.append({"key": "Object", "Object": [
            {"key": "Object Count", "current value": count},
            {"key": "Channel Map", "current minimum range": offset, "current maximum range": offset + count - 1}]})
    rates = component_bitrates(groups, total_bitrate)
    codec = copy.deepcopy(components)
    for component, bitrate in zip(codec, rates):
        component[component["key"]].append({"key": "Bit Rate", "current value": bitrate})
    settings = {"version": 1, "sub version": 2, "parameters": [
        {"key": "Audio Scene Components", "ASComponents": components},
        {"key": "Codec Configurations", "ASComponents": codec},
        {"key": "APAC Metadata", "Metadata": [{"key": "Channel Map", "current minimum range": channels,
                                                "current maximum range": channels}]}]}
    output.mkdir(parents=True, exist_ok=False)
    words = np.frombuffer(blob + b"\0" * (len(blob) % 2), dtype="<i2").astype(np.float32) / 32768.0
    energy, peak = np.zeros(channels), np.zeros(channels)
    digest = hashlib.sha256()
    with source.open("rb") as stream, (output / "input.f32").open("xb") as target:
        stream.seek(info["data_offset"] + start * channels * 3)
        for offset in range(0, frames, 65536):
            count = min(65536, frames - offset)
            raw = stream.read(count * channels * 3)
            require(len(raw) == count * channels * 3, "source truncated")
            digest.update(raw)
            audio = pcm24(raw, channels)
            energy += np.sum(audio.astype(np.float64) ** 2, axis=0)
            peak = np.maximum(peak, np.max(np.abs(audio), axis=0))
            # Complete the final metadata frame even for a short PCM remainder.
            # The runner excludes this audio padding from container valid_frames.
            staged_count = ((count + 1023) // 1024) * 1024
            block = np.zeros((staged_count, channels + 1), dtype="<f4")
            block[:count, :channels] = audio
            for begin in range(0, staged_count, 1024):
                block[begin:begin + len(words), -1] = words
            block.tofile(target)
    info.update(start_sample=start, frames=frames, staged_frames=((frames + 1023) // 1024) * 1024,
                input_padding_frames=(-frames) % 1024, input_channels=channels + 1,
                metadata_channels=1, metadata_bytes=len(blob), metadata_frame_samples=1024,
                component_groups=groups, component_bitrates=rates, total_bitrate=sum(rates),
                objects_per_component=objects_per_component,
                bitrate_allocation="proportional to full-band track count; BED includes a 16000 bit/s LFE allowance",
                source_pcm_excerpt_sha256=digest.hexdigest(), source_rms=np.sqrt(energy / frames).tolist(),
                source_peak=peak.tolist(), encoder_capabilities="system defaults", decoder_capabilities="system defaults",
                omitted_extent=omitted, discarded_diffuse=discarded_diffuse, discard_diffuse=discard_diffuse,
                encoded_object_parameters=metadata_tracks, complete_adm_metadata=False,
                all_object_render_parameters_preserved=not omitted and not discarded_diffuse,
                unconverted_metadata=["source object names/IDs retained in sidecar", "absolute programme timecode",
                                      "Dolby dbmd chunk (not interpreted)"])
    (output / "settings.plist").write_bytes(plistlib.dumps(settings))
    (output / "metadata.aia").write_bytes(blob)
    (output / "expected.json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n")
    return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start-sample", type=int, default=0)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--bitrate-kbps", type=int,
                        help="total target in decimal kbps; explicitly redistributed across output components")
    parser.add_argument("--objects-per-component", type=int, choices=range(1, 8), default=7,
                        help="1 avoids the observed omission of group members in AVFoundation stereo downmix")
    parser.add_argument("--omit-fully-diffuse-extent", action="store_true",
                        help="omit extent only on fully diffuse source objects; log every omitted value")
    parser.add_argument("--discard-diffuse", action="store_true",
                        help="explicitly encode all objects as diffuse=0; preserve original values in the sidecar")
    args = parser.parse_args()
    bitrate = None if args.bitrate_kbps is None else args.bitrate_kbps * 1000
    info = generate(args.source, args.output, args.start_sample, args.frames, args.omit_fully_diffuse_extent, bitrate,
                    args.objects_per_component, args.discard_diffuse)
    print(json.dumps({k: info[k] for k in ("frames", "channels", "objects", "input_channels", "metadata_bytes", "total_bitrate")}))
