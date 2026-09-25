"""Read rendered Float32 output from stock AUSpatialMixer calls in our AVPlayer.

No render settings, audio buffers, CPU registers, decoder behavior, or return
values are changed. Spatial units are identified by the system's own imrd setter.
"""

import json
import os
from pathlib import Path
import struct

import trace_codec

units = {}
pending = {}
formats = {}


def on_property(frame, location, internal_dict):
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    if reg("w1") == 8 and reg("w2") == 2 and reg("w5") == 40:
        raw = trace_codec.read(frame.GetThread().GetProcess(), reg("x4"), 40)
        values = struct.unpack("<d8I", raw)
        formats[reg("x0")] = {"sample_rate": values[0], "format_flags": values[2], "output_channels": values[6]}
    if reg("w1") == 3231:
        unit = reg("x0")
        if unit not in units:
            units[unit] = {"frames": 0, "calls": 0, "index": len(units)}
            print(json.dumps({"stage": "spatial_unit", "unit": hex(unit), "index": units[unit]["index"],
                              "descriptor_bytes": reg("w5")}), flush=True)
    return False


def on_render(frame, location, internal_dict):
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    unit = reg("x0")
    if unit not in units or units[unit]["frames"] >= 96000:
        return False
    process = frame.GetThread().GetProcess()
    bp = process.GetTarget().BreakpointCreateByAddress(reg("x30"))
    bp.SetThreadID(frame.GetThread().GetThreadID())
    pending[bp.GetID()] = {"unit": unit, "sp": reg("sp"), "abl": reg("x5"),
                           "requested_frames": reg("w4"),
                           "sample_time": struct.unpack("<d", trace_codec.read(process, reg("x2"), 8))[0]}
    bp.SetScriptCallbackFunction("trace_spatial_output.on_return")
    return False


def on_return(frame, location, internal_dict):
    identifier = location.GetBreakpoint().GetID()
    state = pending.get(identifier)
    if not state or frame.FindRegister("sp").GetValueAsUnsigned() != state["sp"]:
        return False
    pending.pop(identifier)
    location.GetBreakpoint().SetEnabled(False)
    if frame.FindRegister("w0").GetValueAsUnsigned() != 0:
        return False
    process = frame.GetThread().GetProcess()
    unit = units[state["unit"]]
    try:
        count = trace_codec.number(process, state["abl"], 4)
        if not 1 <= count <= 64:
            raise ValueError("unexpected render ABL")
        channels = []
        for i in range(count):
            number, size, pointer = struct.unpack("<IIQ", trace_codec.read(process, state["abl"] + 8 + 16 * i, 16))
            if not number or number > 64 or size != state["requested_frames"] * 4 * number:
                raise ValueError("render output is not packed Float32")
            raw = trace_codec.read(process, pointer, size)
            floats = struct.unpack("<" + "f" * (size // 4), raw)
            channels.extend([floats[c::number] for c in range(number)])
        output = Path(os.environ["APAC_SPATIAL_CAPTURE"])
        output.mkdir(parents=True, exist_ok=True)
        destination = output / ("unit" + str(unit["index"]) + ".f32")
        with destination.open("ab") as file:
            for row in zip(*channels):
                file.write(struct.pack("<" + "f" * len(row), *row))
        unit["frames"] += state["requested_frames"]
        unit["calls"] += 1
        unit["channels"] = len(channels)
        unit.update(formats.get(state["unit"], {}))
        with (output / ("unit" + str(unit["index"]) + ".blocks.jsonl")).open("a") as index:
            index.write(json.dumps({"sample_time": state["sample_time"], "frames": state["requested_frames"],
                                    "output_offset": unit["frames"] - state["requested_frames"]}) + "\n")
        (output / ("unit" + str(unit["index"]) + ".json")).write_text(json.dumps(unit) + "\n")
        if unit["calls"] == 1:
            print(json.dumps({"stage": "spatial_output", **unit}), flush=True)
        return False
    except Exception as error:
        print(json.dumps({"stage": "spatial_capture_error", "error": str(error)}), flush=True)
        return True


def __lldb_init_module(debugger, internal_dict):
    trace_codec.verify_component()
    target = debugger.GetSelectedTarget()
    for name, callback in (("AudioUnitSetProperty", "on_property"), ("AudioUnitRender", "on_render")):
        bp = target.BreakpointCreateByName(name, "AudioToolboxCore")
        bp.SetScriptCallbackFunction("trace_spatial_output." + callback)
