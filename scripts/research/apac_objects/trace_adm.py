"""Read-only decoded position/extent/diffuse capture, guarded by trace_codec's hash.

Capture the metadata sink's serialization input, after native Deserialize/Decode.
No decoder configuration, capabilities, buffers, or media bytes are changed.
"""

import json
import os
import struct

import trace_codec

pending = {}
wire_counts = {}


def wire_state(process, ptr):
    value = lambda offset: struct.unpack("<f", trace_codec.read(process, ptr + offset, 4))[0]
    return {"flags": list(trace_codec.read(process, ptr + 0x2f8, 2)),
            "spread_bytes": trace_codec.read(process, ptr + 0x324, 48).hex(),
            "spread": [value(offset) for offset in (0x334, 0x340, 0x34c)], "diffuse": value(0x35c)}


def on_wire_return(frame, location, internal_dict):
    key = location.GetBreakpoint().GetID()
    ptr, name = pending.pop(key)
    process = frame.GetThread().GetProcess()
    print(json.dumps({"stage": "metadata_wire_return", "function": name,
                      "status": frame.FindRegister("w0").GetValueAsUnsigned(),
                      **wire_state(process, ptr)}), flush=True)
    return False


def on_wire(frame, location, internal_dict):
    name = frame.GetFunctionName()
    wire_counts[name] = wire_counts.get(name, 0) + 1
    ptr = frame.FindRegister("x0").GetValueAsUnsigned()
    process = frame.GetThread().GetProcess()
    print(json.dumps({"stage": "metadata_wire_entry", "function": name,
                      "sync": frame.FindRegister("w2").GetValueAsUnsigned(),
                      **wire_state(process, ptr)}), flush=True)
    if "Deserialize" in name:
        bp = process.GetTarget().BreakpointCreateByAddress(frame.FindRegister("x30").GetValueAsUnsigned())
        bp.SetOneShot(True)
        bp.SetThreadID(frame.GetThread().GetThreadID())
        pending[bp.GetID()] = ptr, name
        bp.SetScriptCallbackFunction("trace_adm.on_wire_return")
    if wire_counts[name] >= 4:
        location.GetBreakpoint().SetEnabled(False)
    return False


def on_renderer(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    ptr = frame.FindRegister("x0").GetValueAsUnsigned()
    try:
        counter = process.GetTarget().FindFirstGlobalVariable("g_probe_decode_packet_index")
        packet = trace_codec.number(process, counter.GetLoadAddress(), 8)
        if packet >= int(os.environ.get("APAC_ADM_TRACE_PACKETS", "8")):
            location.GetBreakpoint().SetEnabled(False)
            return False
        if not trace_codec.active_metadata:
            raise RuntimeError("decoded metadata context missing")
        metadata_frame = trace_codec.number(process, trace_codec.active_metadata + 56, 8)
        renderer = trace_codec.number(process, metadata_frame + 48, 8)
        count = trace_codec.number(process, renderer, 4)
        groups = trace_codec.number(process, renderer + 8, 8)
        if count > 2048:
            raise RuntimeError("invalid group count")
        group_id = None
        for index in range(count):
            group = trace_codec.read(process, groups + index * 32, 32)
            begin, end = struct.unpack_from("<QQ", group, 8)
            if begin <= ptr < end:
                group_id = struct.unpack_from("<H", group, 2)[0]
                break
        value = lambda offset: struct.unpack("<f", trace_codec.read(process, ptr + offset, 4))[0]
        event = {"stage": "decoded_renderer_metadata", "packet": packet, "group_id": group_id,
                 "spherical": [value(0x208 + 132 + i * 4) for i in range(3)],
                 "cartesian_spread": [value(offset) for offset in (0x334, 0x340, 0x34c)],
                 "spread_flags": list(trace_codec.read(process, ptr + 0x2f8, 2)),
                 "spread_range_precision": trace_codec.read(process, ptr + 0x324, 9).hex(),
                 "diffuse": value(0x35c),
                 "position_precision_bits": list(trace_codec.read(process, ptr + 0x208 + 114, 3))}
        print(json.dumps(event), flush=True)
        return False
    except Exception as error:
        print(json.dumps({"stage": "decoded_renderer_metadata_error", "error": str(error)}), flush=True)
        return True


def __lldb_init_module(debugger, internal_dict):
    # Import trace_codec with LLDB's `command script import` before this module,
    # so its callbacks are registered in LLDB's interpreter namespace as well.
    trace_codec.verify_component()
    breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex(
        r"^metadata_bsfmt::RendererData::PackMetadataToByteArrays\(")
    breakpoint.SetScriptCallbackFunction("trace_adm.on_renderer")
    if os.environ.get("APAC_ADM_TRACE_WIRE") == "1":
        for name in ("Serialize", "Deserialize"):
            bp = debugger.GetSelectedTarget().BreakpointCreateByRegex(
                r"^metadata_bsfmt::RendererData::" + name + r"\(")
            bp.SetScriptCallbackFunction("trace_adm.on_wire")
