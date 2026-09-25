"""Read-only, fingerprint-guarded observation of APAC component rate selection."""

import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from trace_codec import number, verify_component


def on_append(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    try:
        config = number(process, frame.FindRegister("x0").GetValueAsUnsigned() + 920, 8)
        event = {"stage": "asc_bitrates", "profile": number(process, config + 32, 2),
                 "level": number(process, config + 34, 1)}
        for key, offset in (("input_components", 0x148), ("codec_components", 0x160)):
            begin, end = number(process, config + offset, 8), number(process, config + offset + 8, 8)
            if end < begin or (end - begin) % 8 or end - begin > 2048:
                raise ValueError("unexpected ASC vector")
            rows = []
            for address in range(begin, end, 8):
                component = number(process, address, 8)
                rows.append({"audio_data_type": number(process, component + 8, 4),
                             "serialized_bitrate_bps": number(process, component + 0x34, 4)})
            event[key] = rows
        print(json.dumps(event), flush=True)
        location.GetBreakpoint().SetEnabled(False)
        return False
    except Exception as error:
        print(json.dumps({"stage": "asc_bitrates_error", "error": str(error)}), flush=True)
        return True


def on_config(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    parameter = frame.FindRegister("x1").GetValueAsUnsigned()
    print(json.dumps({"stage": "component_rate_before_selection", "bitrate_bps": number(process, parameter + 0x10, 4)}), flush=True)
    address = frame.GetSymbol().GetStartAddress().GetLoadAddress(process.GetTarget())
    breakpoint = process.GetTarget().BreakpointCreateByAddress(address + 156)
    breakpoint.SetOneShot(True)
    breakpoint.SetThreadID(frame.GetThread().GetThreadID())
    breakpoint.SetScriptCallbackFunction("trace_bitrate.after_selection")
    return False


def after_selection(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    parameter = frame.FindRegister("x20").GetValueAsUnsigned()
    print(json.dumps({"stage": "component_rate_after_selection", "bitrate_bps": number(process, parameter + 0x10, 4)}), flush=True)
    location.GetBreakpoint().SetEnabled(False)
    return False


def __lldb_init_module(debugger, internal_dict):
    verify_component()
    breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex("^ACAPACBaseEncoder::AppendInputData\\(")
    breakpoint.SetScriptCallbackFunction("trace_bitrate.on_append")
    breakpoint = debugger.GetSelectedTarget().BreakpointCreateByName("APACCoreEncoder::Initialize(apac::ConfigParam&)")
    breakpoint.SetScriptCallbackFunction("trace_bitrate.on_config")
