"""Observe private OAR configuration in the self-owned diagnostic process.

Only reads argument memory; the fixed config span is bounded by the caller's
observed object layout on the hash-checked AudioCodecs arm64e component.
"""

import hashlib
import json
import os
from pathlib import Path
import struct

import lldb

counts = {}


def read(process, address, size):
    error = lldb.SBError()
    value = process.ReadMemory(address, size, error)
    if not error.Success() or len(value) != size:
        raise RuntimeError(str(error))
    return value


def on_call(frame, location, internal_dict):
    name = frame.GetFunctionName() or frame.GetSymbol().GetName()
    counts[name] = counts.get(name, 0) + 1
    if counts[name] > 2:
        location.SetEnabled(False)
        return False
    process = frame.GetThread().GetProcess()
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    event = dict(function=name, count=counts[name], x0=hex(reg("x0")), x1=hex(reg("x1")),
                 x2=reg("x2"), stack=[f.GetFunctionName() for f in list(frame.GetThread())[:8]])
    if name in ("oar_query_memory", "oar_query_scratch", "oar_init_safe"):
        value = read(process, reg("x0"), 48)
        event.update(config_hex=value.hex(), config_u32=list(struct.unpack("<12I", value)))
    print(json.dumps(event), flush=True)
    return False


def __lldb_init_module(debugger, internal_dict):
    component = Path("/System/Library/Components/AudioCodecs.component/Contents/MacOS/AudioCodecs")
    with component.open("rb") as stream:
        magic, count = struct.unpack(">II", stream.read(8))
        if magic != 0xCAFEBABE:
            raise RuntimeError("Unexpected component container")
        entries = [struct.unpack(">IIIII", stream.read(20)) for _ in range(count)]
        entry = next(v for v in entries if v[:2] == (0x0100000C, 0x80000002))
        stream.seek(entry[2])
        if hashlib.sha256(stream.read(entry[3])).hexdigest() != "82fb858cdbcf9146b1740a5cccd25dbf843ab9fbcdb88e05bbbbaed3e8e9cdd2":
            raise RuntimeError("AudioCodecs version mismatch")
    patterns = [r"^oar_(query_memory|query_scratch|init_safe)$"]
    for pattern in patterns:
        breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex(pattern)
        breakpoint.SetScriptCallbackFunction("trace_oar.on_call")
