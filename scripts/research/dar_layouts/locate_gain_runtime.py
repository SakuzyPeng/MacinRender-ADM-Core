#!/usr/bin/env python3
"""Locate gain-path anchors, compact-unwind functions and RTTI vtables."""

import argparse
import bisect
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

ANCHORS = [
    "homeSpeakerGains", "cinemaSpeakerGains", "TorchLightPanner", "Spectral Decorrelator",
    "Decorrelation and size mismatch. Did you forget to call atmos_storage_iab_dyn_metadata_preprocessor_process?",
    "oar re-renderer", "oar renderer", "OarAdapter", "OAR query memory error", "OAR init failed",
    "Error initializing OAR",
    "N5Dolby5Sushi6Cinema16TorchLightPannerE",
    "N5Dolby5Sushi4Home10OarAdapterE", "N5Dolby5Sushi4Home11OarRendererE",
    "N5Dolby5Sushi4Home13OarRerendererE", "N5Dolby5Sushi4Home13OarRerenderer4NodeE",
    "N5Dolby5Sushi4Home18SingleOarProcessorE", "N5Dolby5Sushi4Home18SingleOarProcessor4NodeE",
    "N5Dolby5Sushi4Home20MultipleOarProcessorE", "N5Dolby5Sushi4Home20MultipleOarProcessor4NodeE",
    "N12DassidkDolby5Atmos4Home10OarAdapterE", "N12DassidkDolby5Atmos4Home11OarRendererE",
    "N5Dolby5Sushi4Home20SpeakersPreprocessorE", "N5Dolby5Sushi4Home20RendererPreprocessorE",
    "N5Dolby5Sushi4Home19PreprocessorManagerE", "N5Dolby5Sushi4Home18PreprocessorBypassE",
    "N5Dolby5Sushi4Home18PreprocessorBypass10SingleNodeE",
    "N5Dolby20SpectralDecorrelator16DecorrelatorImplE", "N5Dolby20SpectralDecorrelator10BypassImplE",
    "RendererPreprocessor::Factory", "Using PreprocessorMode::BYPASS", "Home::PreprocessorManager",
    "Starting Spectral Decorrelator", "Spectral Decorrelator Impl",
    "N5Dolby5Sushi4Home11OmoPlatformE", "N5Dolby5Sushi4Home11OmoPlatform10SingleNodeE",
    "N5Dolby5Sushi20SpatialCoderPlatformE", "N5Dolby5Sushi20SpatialCoderPlatform10SingleNodeE",
    "N5Dolby5Atmos12SizeToDecorr22ISizeToDecorrProcessorI24atmos_dyn_metadata_eventEE",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-manifest", type=Path, required=True)
    parser.add_argument("--runtime-trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--anchors-json", type=Path, help="Optional exact string anchors for a focused follow-up")
    args = parser.parse_args()
    identity = json.loads(args.binary_manifest.read_text())
    binary = Path(identity["arm64"]).read_bytes()
    if hashlib.sha256(binary).hexdigest() != identity["arm64_sha256"]:
        raise ValueError("arm64 input differs from the module identity")
    segments, sections = [], {}
    cursor = 32
    for _ in range(struct.unpack_from("<I", binary, 16)[0]):
        command, length = struct.unpack_from("<II", binary, cursor)
        if command == 25:
            name = binary[cursor + 8:cursor + 24].split(b"\0")[0].decode()
            va, size, offset, file_size = struct.unpack_from("<QQQQ", binary, cursor + 24)
            segments.append((name, va, offset, file_size))
            for index in range(struct.unpack_from("<I", binary, cursor + 64)[0]):
                position = cursor + 72 + 80 * index
                name = binary[position:position + 16].split(b"\0")[0].decode()
                va, size, offset = struct.unpack_from("<QQI", binary, position + 32)
                sections[name] = (va, size, offset)
        cursor += length
    def file_to_va(offset):
        segment = next((item for item in segments if item[2] <= offset < item[2] + item[3]), None)
        return segment[1] + offset - segment[2] if segment else None
    def va_to_file(va):
        segment = next((item for item in segments if item[1] <= va < item[1] + item[3]), None)
        return segment[2] + va - segment[1] if segment else None
    image_base = next(item[1] for item in segments if item[0] == "__TEXT")
    code_start, code_size, _ = sections["__text"]
    _, unwind_size, unwind_offset = sections["__unwind_info"]
    unwind = binary[unwind_offset:unwind_offset + unwind_size]
    version, _, _, _, _, index_offset, index_count = struct.unpack_from("<7I", unwind)
    if version != 1:
        raise ValueError("unsupported compact unwind version")
    starts = []
    for index in range(index_count):
        function, page, _ = struct.unpack_from("<III", unwind, index_offset + 12 * index)
        if not page:
            continue
        kind, entries, count = struct.unpack_from("<IHH", unwind, page)
        if kind == 3:
            starts.extend(image_base + function + (struct.unpack_from("<I", unwind, page + entries + 4 * i)[0] & 0xffffff)
                          for i in range(count))
        elif kind == 2:
            starts.extend(image_base + struct.unpack_from("<I", unwind, page + entries + 8 * i)[0]
                          for i in range(count))
        else:
            raise ValueError("unsupported compact unwind page")
    starts = sorted(set(starts))
    def owner(pc):
        index = bisect.bisect_right(starts, pc) - 1
        return hex(starts[index]) if index >= 0 else None
    events = [json.loads(line) for line in args.runtime_trace.read_text().splitlines()]
    snapshot = next(event for event in events if event["event"] == "snapshot" and int(event["address"], 0) == code_start)
    runtime = Path(snapshot["path"]).read_bytes()
    if hashlib.sha256(runtime).hexdigest() != snapshot["sha256"]:
        raise ValueError("runtime code snapshot differs from the trace")
    words = np.frombuffer(runtime, dtype="<u4")
    def pointer_refs(value):
        pattern = struct.pack("<Q", value)
        offset = 0
        result = []
        while True:
            offset = binary.find(pattern, offset)
            if offset < 0:
                break
            if offset % 8 == 0:
                va = file_to_va(offset)
                if va is not None:
                    result.append(va)
            offset += 1
        return result
    anchors = []
    for text in json.loads(args.anchors_json.read_text()) if args.anchors_json else ANCHORS:
        offset = binary.find(text.encode() + b"\0")
        if offset < 0:
            continue
        va = file_to_va(offset)
        entry = {"text": text, "address": hex(va), "code_references": [], "vtables": []}
        if text.startswith("N"):
            for name_pointer in pointer_refs(va) + pointer_refs(va | (1 << 63)):
                typeinfo = name_pointer - 8
                for type_reference in pointer_refs(typeinfo):
                    vtable = type_reference + 8
                    position = va_to_file(vtable)
                    methods = []
                    if position is None:
                        continue
                    for index in range(48):
                        target = struct.unpack_from("<Q", binary, position + index * 8)[0]
                        if not code_start <= target < code_start + code_size:
                            break
                        methods.append({"slot": hex(vtable + index * 8), "target": hex(target)})
                    if methods:
                        entry["vtables"].append({"typeinfo": hex(typeinfo), "address_point": hex(vtable),
                                                  "methods": methods})
        anchors.append(entry)
    wanted = {int(item["address"], 0): item for item in anchors}
    pages = {value & ~4095 for value in wanted}
    for index in np.flatnonzero((words & 0x9f000000) == 0x90000000):
        instruction = int(words[index])
        register = instruction & 31
        immediate = ((instruction >> 29) & 3) | (((instruction >> 5) & 0x7ffff) << 2)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        pc = code_start + int(index) * 4
        page = (pc & ~4095) + (immediate << 12)
        if page not in pages:
            continue
        for after in range(int(index) + 1, min(int(index) + 9, len(words))):
            candidate = int(words[after])
            if (candidate & 0xff000000) == 0x91000000 and ((candidate >> 5) & 31) == register:
                target = page + (((candidate >> 10) & 4095) << (12 if candidate & (1 << 22) else 0))
                if target in wanted:
                    site = code_start + after * 4
                    wanted[target]["code_references"].append({"site": hex(site), "function": owner(site)})
            if after > int(index) + 1 and candidate & 31 == register and (candidate & 0xff000000) != 0x91000000:
                break
    result = {"binary_identity": identity, "runtime_snapshot": snapshot,
              "image_base": hex(image_base), "unwind_function_count": len(starts), "anchors": anchors}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
