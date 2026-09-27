"""Read-only identity and semantic checks for the small synthetic trace ADMs."""

import hashlib
import math
import struct
import wave
from pathlib import Path
from xml.etree import ElementTree

from make_calibration_suite import file_sha256, timecode_samples


def gain_field(element: ElementTree.Element) -> dict:
    field = element.find("gain")
    if field is None:
        return {"present": False, "unit": None, "text": None, "linear": 1.0}
    unit = field.attrib.get("gainUnit", "linear")
    value = float(field.text)
    if unit not in ("linear", "dB") or math.isnan(value):
        raise ValueError("invalid ADM gain")
    linear = 10.0 ** (value / 20.0) if unit == "dB" else value
    if not math.isfinite(linear) or linear < 0:
        raise ValueError("invalid normalized ADM gain")
    return {"present": True, "unit": unit, "unit_present": "gainUnit" in field.attrib,
            "text": field.text, "linear": linear}


def time_field(element: ElementTree.Element, name: str, default=None) -> dict:
    text = element.attrib.get(name)
    return {"present": text is not None, "text": text,
            "samples": timecode_samples(text) if text is not None else default}


def object_fields(element: ElementTree.Element) -> dict:
    mute = element.findtext("mute")
    if mute is not None and mute.strip() not in ("0", "1", "true", "false"):
        raise ValueError("invalid ADM mute")
    return {"id": element.attrib["audioObjectID"], "gain": gain_field(element),
            "mute": {"present": mute is not None, "text": mute,
                     "value": mute is not None and mute.strip() in ("1", "true")},
            "start": time_field(element, "start", 0), "duration": time_field(element, "duration"),
            "children": [item.text for item in element.findall("audioObjectIDRef")],
            "track_uids": [item.text for item in element.findall("audioTrackUIDRef")],
            "xml": ElementTree.tostring(element, encoding="unicode")}


def inspect_adm(path: Path) -> dict:
    chunks = {}
    pcm_hash = hashlib.sha256()
    with path.open("rb") as source:
        if source.read(4) != b"RIFF":
            raise ValueError("trace currently supports small RIFF ADM probes only")
        source.read(4)
        if source.read(4) != b"WAVE":
            raise ValueError("not an ADM WAVE")
        while header := source.read(8):
            if len(header) != 8:
                raise ValueError("truncated RIFF chunk header")
            kind, length = struct.unpack("<4sI", header)
            if kind == b"data":
                remaining = length
                while remaining:
                    data = source.read(min(remaining, 1 << 20))
                    if not data:
                        raise ValueError("truncated PCM payload")
                    pcm_hash.update(data)
                    remaining -= len(data)
            elif kind in (b"axml", b"chna", b"dbmd"):
                chunks[kind] = source.read(length)
                if len(chunks[kind]) != length:
                    raise ValueError("truncated ADM metadata")
            else:
                source.seek(length, 1)
            source.seek(length & 1, 1)
    if not all(key in chunks for key in (b"axml", b"chna")):
        raise ValueError("trace requires final ADM with both AXML and CHNA")
    with wave.open(str(path), "rb") as source:
        channels, rate, width, frames = (source.getnchannels(), source.getframerate(),
                                          source.getsampwidth(), source.getnframes())
    if rate != 48000 or width != 3:
        raise ValueError("trace is limited to 48 kHz 24-bit synthetic ADM")
    root = ElementTree.fromstring(chunks[b"axml"])
    for item in root.iter():
        item.tag = item.tag.split("}")[-1]
    formats = {item.attrib["audioChannelFormatID"]: item for item in root.iter("audioChannelFormat")}
    streams = {item.attrib["audioStreamFormatID"]: item.findtext("audioChannelFormatIDRef")
               for item in root.iter("audioStreamFormat")}
    tracks = {item.attrib["audioTrackFormatID"]: streams[item.findtext("audioStreamFormatIDRef")]
              for item in root.iter("audioTrackFormat")}
    source_objects = [object_fields(item) for item in root.iter("audioObject")]
    track_count, uid_count = struct.unpack_from("<HH", chunks[b"chna"])
    if track_count != channels or len(chunks[b"chna"]) != 4 + 40 * uid_count:
        raise ValueError("invalid CHNA channel count or length")
    bindings = []
    for i in range(uid_count):
        index, uid, track, pack, _ = struct.unpack_from("<H12s14s11sc", chunks[b"chna"], 4 + 40 * i)
        clean = lambda value: value.rstrip(b"\0").decode("ascii")
        track_id = clean(track)
        channel_id = tracks.get(track_id)
        if channel_id not in formats:
            raise ValueError("unresolved CHNA track binding")
        channel = formats[channel_id]
        if channel.attrib.get("typeDefinition") != "Objects":
            continue
        uid_id = clean(uid)
        owner_fields = [item for item in source_objects if uid_id in item["track_uids"]]
        owners = [item["id"] for item in owner_fields]
        starts = {item["start"]["samples"] for item in owner_fields}
        if len(starts) != 1:
            raise ValueError("trace requires an unambiguous object start for each PCM channel")
        object_start = next(iter(starts))
        events = []
        for block in channel.findall("audioBlockFormat"):
            xyz = {item.attrib["coordinate"]: float(item.text) for item in block.findall("position")}
            if set(xyz) - set("XYZ"):
                raise ValueError("unsupported polar coordinate in Cartesian trace")
            xyz = {axis: xyz.get(axis, 0.0) for axis in "XYZ"}
            extent = [float(block.findtext(name, "0")) for name in ("width", "height", "depth")]
            gain = gain_field(block)
            if (block.findtext("cartesian") != "1" or
                    not all(math.isfinite(x) for x in [*xyz.values(), *extent, gain["linear"]]) or
                    abs(xyz["X"]) > 1 or abs(xyz["Y"]) > 1 or not 0 <= xyz["Z"] <= 1 or
                    not 0 <= extent[0] <= 1 or max(extent) - min(extent) > 1e-7):
                raise ValueError("unsupported object coordinate/extent metadata")
            rtime = time_field(block, "rtime", 0)
            duration = time_field(block, "duration")
            events.append({"id": block.attrib["audioBlockFormatID"],
                           "start_sample": object_start + rtime["samples"],
                           "relative_start_sample": rtime["samples"],
                           "duration_samples": duration["samples"], "rtime_field": rtime,
                           "duration_field": duration, "gain_field": gain,
                           "xyz": [xyz[axis] for axis in "XYZ"], "size": extent[0], "gain": gain["linear"],
                           "xml": ElementTree.tostring(block, encoding="unicode")})
        if not events or not owners or not 1 <= index <= channels:
            raise ValueError("incomplete object binding or timeline")
        bindings.append({"input_channel": index - 1, "track_uid": uid_id, "track_format": track_id,
                         "channel_format": channel_id, "object_ids": owners, "source_objects": owner_fields,
                         "events": events})
    if not bindings:
        raise ValueError("no Cartesian Objects in the ADM")
    return {"path": str(path.resolve()), "sha256": file_sha256(path), "pcm_sha256": pcm_hash.hexdigest(),
            "axml_sha256": hashlib.sha256(chunks[b"axml"]).hexdigest(),
            "chna_sha256": hashlib.sha256(chunks[b"chna"]).hexdigest(),
            "dbmd_sha256": hashlib.sha256(chunks[b"dbmd"]).hexdigest() if b"dbmd" in chunks else None,
            "channels": channels, "sample_rate": rate, "frames": frames, "objects": bindings,
            "source_objects": source_objects, "event_time_basis": "file_absolute_samples"}
