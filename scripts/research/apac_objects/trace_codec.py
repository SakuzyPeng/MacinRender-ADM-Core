"""LLDB callbacks for the verified macOS 27.0 (26A428) arm64e component.

Default operation observes only. APAC_PROFILE_CEILING=1 or 2 explicitly enables
the documented profile-table substitution in the test encoder process alone.
"""

import hashlib
import json
import os
from pathlib import Path
import struct

import lldb

EXPECTED = "82fb858cdbcf9146b1740a5cccd25dbf843ab9fbcdb88e05bbbbaed3e8e9cdd2"
counts = {}
decode_metadata_index = None
active_metadata = None
counter_addresses = {}


def verify_component():
    component = Path("/System/Library/Components/AudioCodecs.component/Contents/MacOS/AudioCodecs")
    with component.open("rb") as stream:
        magic, count = struct.unpack(">II", stream.read(8))
        if magic != 0xCAFEBABE:
            raise RuntimeError("unexpected component container")
        slices = [struct.unpack(">IIIII", stream.read(20)) for _ in range(count)]
        entry = next(s for s in slices if s[:2] == (0x0100000C, 0x80000002))
        stream.seek(entry[2])
        if hashlib.sha256(stream.read(entry[3])).hexdigest() != EXPECTED:
            raise RuntimeError("component SHA-256 mismatch; no ABI-dependent operations allowed")


def read(process, pointer, count):
    error = lldb.SBError()
    value = process.ReadMemory(pointer, count, error)
    if not error.Success() or len(value) != count:
        raise RuntimeError(str(error))
    return value


def number(process, pointer, count):
    return int.from_bytes(read(process, pointer, count), "little")


def on_metadata_capacity(frame, location, internal_dict):
    print(json.dumps({"stage": "metadata_sink_capacity",
                      "required_bytes": frame.FindRegister("w0").GetValueAsUnsigned(),
                      "capacity_bytes": frame.FindRegister("x20").GetValueAsUnsigned()}), flush=True)
    location.GetBreakpoint().SetEnabled(False)
    return False


def on_entry(frame, location, internal_dict):
    global decode_metadata_index, active_metadata
    name = frame.GetFunctionName() or frame.GetSymbol().GetName()
    counts[name] = counts.get(name, 0) + 1
    process = frame.GetThread().GetProcess()
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    event = {"stage": "trace", "function": name, "count": counts[name]}
    try:
        if "APACMetadataSink::Process(" in name:
            address = frame.GetSymbol().GetStartAddress().GetLoadAddress(process.GetTarget())
            breakpoint = process.GetTarget().BreakpointCreateByAddress(address + 0x90)
            breakpoint.SetOneShot(True)
            breakpoint.SetThreadID(frame.GetThread().GetThreadID())
            breakpoint.SetScriptCallbackFunction("trace_codec.on_metadata_capacity")
        if "__cxa_throw" in name:
            code = int.from_bytes(read(process, reg("x0"), 4), "little", signed=True)
            event["exception_value"] = code
            event["stack"] = [{"function": item.GetFunctionName(), "pc": hex(item.GetPC())}
                              for item in list(frame.GetThread())[:12]]
            print(json.dumps(event), flush=True)
            return False
        if "ACAPACBaseEncoder::Initialize(" in name:
            level = os.environ.get("APAC_PROFILE_CEILING")
            if level is not None:
                if level not in ("1", "2"):
                    raise ValueError("diagnostic level must be 1 or 2")
                target = process.GetTarget()
                address = target.FindFirstGlobalVariable("g_apac_diagnostic_profiles").GetLoadAddress()
                if address == lldb.LLDB_INVALID_ADDRESS:
                    raise RuntimeError("test-owned diagnostic table is missing")
                error = lldb.SBError()
                data = struct.pack("<HBBHBB", 5, int(level), 0, 31, 6, 0)
                if process.WriteMemory(address, data, error) != len(data) or not error.Success():
                    raise RuntimeError(str(error))
                if process.WriteMemory(reg("x0") + 360, address.to_bytes(8, "little"), error) != 8 or not error.Success():
                    raise RuntimeError(str(error))
                event["diagnostic_profile_ceiling"] = int(level)
        if "ACAPACBaseEncoder::AppendInputData(" in name and counts[name] == 1:
            config = number(process, reg("x0") + 920, 8)
            event.update(profile=number(process, config + 32, 2), level=number(process, config + 34, 1))
        if "CreateEncoderElement(" in name:
            event["audio_data_type"] = reg("x1")
        if "PCMMetadataReader::GetMetadata(" in name:
            active_metadata = reg("x1")
            event.update(metadata_channels=number(process, reg("x0") + 64, 4), frame_samples=number(process, reg("x0") + 68, 4))
            metadata_frame = number(process, reg("x1") + 56, 8)
            renderer = number(process, metadata_frame + 48, 8)
            event.update(renderer_metadata_enabled=number(process, metadata_frame + 1, 1),
                         renderer_groups=number(process, renderer, 4) if renderer else None)
        if "MetadataBase::ParseMetadata(" in name:
            event.update(input_bytes=reg("x2"), prefix=read(process, reg("x1"), min(reg("x2"), 32)).hex())
        if "aia_format::Header::Parse(" in name:
            reader = reg("x1")
            data = number(process, reader, 8)
            event.update(header_bytes=read(process, data, 16).hex(), reader=read(process, reader, 28).hex())
        if "sceneposition::ItemPosition::DeriveAbsolutePosition(" in name:
            ptr = reg("x0")
            if number(process, ptr + 16, 1) != 1:
                return False
            event.update(derived_spherical=list(struct.unpack("<fff", read(process, ptr + 132, 12))),
                         quantized=list(struct.unpack("<III", read(process, ptr + 36, 12))))
            event["precision_bits"] = list(read(process, ptr + 114, 3))
            event["object_index"] = None
            if active_metadata:
                metadata_frame = number(process, active_metadata + 56, 8)
                renderer = number(process, metadata_frame + 48, 8)
                if renderer:
                    header = read(process, renderer, 24)
                    group_count = struct.unpack_from("<I", header)[0]
                    if group_count > 2048: raise RuntimeError("invalid renderer group count")
                    groups = struct.unpack_from("<Q", header, 8)[0]
                    group_data = read(process, groups, group_count * 32)
                    for index in range(group_count):
                        begin, end = struct.unpack_from("<QQ", group_data, index * 32 + 8)
                        if begin <= ptr < end:
                            event["object_index"] = index
                            event["group_id"] = struct.unpack_from("<H", group_data, index * 32 + 2)[0]
                            event["block_index"] = (ptr - begin) // 2944
                            break
            event["decode_metadata_index"] = decode_metadata_index
            for key in ("g_probe_decode_packet_index", "g_probe_pcm_output_frames"):
                if key not in counter_addresses:
                    variable = process.GetTarget().FindFirstGlobalVariable(key)
                    counter_addresses[key] = variable.GetLoadAddress() if variable.IsValid() else None
                if counter_addresses[key] is not None:
                    event[key] = number(process, counter_addresses[key], 8)
            print(json.dumps(event), flush=True)
            return False
        if "APACDecoder::Initialize(" in name:
            config = reg("x1")
            event.update(profile=number(process, config + 32, 2), level=number(process, config + 34, 1))
        if "MetadataBase::Deserialize(" in name:
            active_metadata = reg("x0")
            decode_metadata_index = number(process, reg("x0") + 72, 8)
            event["decode_metadata_index"] = decode_metadata_index
        if counts[name] <= 3:
            print(json.dumps(event), flush=True)
        if counts[name] >= 3 and "MetadataBase::Deserialize(" not in name:
            location.GetBreakpoint().SetEnabled(False)
        return False
    except Exception as error:
        print(json.dumps({**event, "error": str(error)}), flush=True)
        # Stop on an ABI/memory error rather than continuing a questionable diagnosis.
        return True


def __lldb_init_module(debugger, internal_dict):
    verify_component()
    patterns = [
        "^ACAPACBaseEncoder::Initialize\\(",
        "^ACAPACBaseEncoder::AppendInputData\\(",
        "^APACEncoder::CreateEncoderElement\\(",
        "^APACObjEncoder::ReadASCCodecParam\\(",
        "^APACObjEncoder::InitializeASCConfig\\(",
        "^PCMMetadataReader::GetMetadata\\(",
        "^BinaryMetadataReader::GetMetadata\\(",
        "^apac::obj::MetadataBase::ParseMetadata\\(",
        "^APACCustomModeEncoder::EncodeFrame\\(",
        "^aia_format::Header::Parse\\(",
        "^sceneposition::ItemPosition::DeriveAbsolutePosition\\(",
        "^apac::obj::MetadataBase::Deserialize\\(",
        "^APACDecoder::Initialize\\(",
    ]
    if os.environ.get("APAC_TRACE_ERRORS") == "1":
        patterns.append("__cxa_throw")
        patterns.append("^APACMetadataSink::Process\\(")
    if os.environ.get("APAC_TRACE_LFE") == "1":
        patterns.extend(["^BitstreamEncoder::WriteLFE\\(", "^APACCoreLBRBitstreamEncoder::WriteLFE\\(",
                         "^APACLFEElement::Deserialize\\("])
    target = debugger.GetSelectedTarget()
    for pattern in patterns:
        if "DeriveAbsolutePosition" in pattern and os.environ.get("APAC_TRACE_POSITIONS") == "0":
            continue
        breakpoint = target.BreakpointCreateByRegex(pattern)
        breakpoint.SetScriptCallbackFunction("trace_codec.on_entry")
