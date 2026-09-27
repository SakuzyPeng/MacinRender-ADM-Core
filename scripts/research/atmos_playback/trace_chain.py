"""Read-only LLDB observations in the self-owned optimized AVPlayer probe.

No decoder state, properties, registers, or return values are changed. Private
symbols are observed by name only; there are no hardcoded structure offsets.
"""

import json
import os
from pathlib import Path
import struct

import lldb

counts = {}
pending = {}


def emit(event):
    print(json.dumps(event, ensure_ascii=False), flush=True)


def read(process, address, size):
    error = lldb.SBError()
    result = process.ReadMemory(address, size, error)
    if not error.Success() or len(result) != size:
        raise RuntimeError(str(error))
    return result


def fourcc(value):
    return int(value).to_bytes(4, "big").decode("ascii", errors="replace")


def asbd(process, address):
    if not address:
        return None
    values = struct.unpack("<d8I", read(process, address, 40))
    return dict(sample_rate=values[0], format=fourcc(values[1]), flags=values[2],
                bytes_per_packet=values[3], frames_per_packet=values[4],
                bytes_per_frame=values[5], channels=values[6], bits=values[7])


def stack(frame):
    return [dict(name=f.GetFunctionName() or f.GetSymbol().GetName(),
                 module=f.GetModule().GetFileSpec().GetFilename(),
                 file_address=hex(f.GetPCAddress().GetFileAddress()))
            for f in list(frame.GetThread())[:22]]


def on_return(frame, location, internal_dict):
    identifier = location.GetBreakpoint().GetID()
    record = pending.get(identifier)
    if record is None or frame.FindRegister("sp").GetValueAsUnsigned() != record["sp"]:
        return False
    pending.pop(identifier)
    location.GetBreakpoint().SetEnabled(False)
    process = frame.GetThread().GetProcess()
    status = frame.FindRegister("w0").GetValueAsUnsigned() & 0xFFFFFFFF
    event = {"stage": "return", "status": status, **record["event"]}
    try:
        if status == 0 and record.get("size_pointer") and record.get("data_pointer"):
            size = int.from_bytes(read(process, record["size_pointer"], 4), "little")
            event["bytes"] = size
            if size <= 4096:
                event["value_hex"] = read(process, record["data_pointer"], size).hex()
            if size == 40 and record["event"].get("property") in ("ifmt", "ofmt", "acif", "acof"):
                event["asbd"] = asbd(process, record["data_pointer"])
    except Exception as error:
        event["read_error"] = str(error)
    emit(event)
    return False


def observe_return(frame, event, data=0, size=0):
    target = frame.GetThread().GetProcess().GetTarget()
    address = frame.FindRegister("x30").GetValueAsUnsigned() & ((1 << 48) - 1)
    breakpoint = target.BreakpointCreateByAddress(address)
    breakpoint.SetThreadID(frame.GetThread().GetThreadID())
    pending[breakpoint.GetID()] = dict(sp=frame.FindRegister("sp").GetValueAsUnsigned(),
                                      event=event, data_pointer=data, size_pointer=size)
    breakpoint.SetScriptCallbackFunction("trace_chain.on_return")


def on_call(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    name = frame.GetFunctionName() or frame.GetSymbol().GetName()
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    event = dict(stage="call", function=name, module=frame.GetModule().GetFileSpec().GetFilename())
    key = name
    try:
        if name == "AudioComponentRegister":
            values = struct.unpack("<5I", read(process, reg("x0"), 20))
            event["component"] = dict(zip(("type", "subtype", "manufacturer"), [fourcc(v) for v in values[:3]]))
            event["component_flags"] = values[3:]
            factory = process.GetTarget().ResolveLoadAddress(reg("x3") & ((1 << 48) - 1))
            event["factory"] = factory.GetSymbol().GetName()
            event["factory_module"] = factory.GetModule().GetFileSpec().GetFilename()
            key += ":" + event["component"]["subtype"]
        public_property = name in ("AudioUnitSetProperty", "AudioConverterSetProperty", "AudioCodecSetProperty",
                                   "AudioQueueSetProperty", "AudioUnitGetProperty", "AudioCodecGetProperty",
                                   "AudioConverterGetProperty")
        if public_property:
            prop = reg("w1")
            event.update(target=hex(reg("x0")), property_id=prop, property=fourcc(prop))
            key += ":" + event["target"] + ":" + str(prop)
            if name.startswith("AudioUnit"):
                event.update(scope=reg("w2"), element=reg("w3"))
                key += ":" + str(reg("w2")) + ":" + str(reg("w3"))
        counts[key] = counts.get(key, 0) + 1
        limit = 6 if name.startswith(("ACDDPAtmosDecoder::Initialize(",
                                      "ACDDPAtmosDecoder::SetCurrentOutputFormat(")) else 2
        if counts[key] > limit:
            if not public_property:
                location.SetEnabled(False)
            return False
        event["count"] = counts[key]
        event["stack"] = stack(frame)
        if public_property and "SetProperty" in name:
            unit = name == "AudioUnitSetProperty"
            queue = name == "AudioQueueSetProperty"
            data = reg("x4") if unit else reg("x2") if queue else reg("x3")
            size = reg("w5") if unit else reg("w3") if queue else reg("w2")
            event["bytes"] = size
            if data and size <= 4096:
                event["value_hex"] = read(process, data, size).hex()
            if size == 40 and (event["property_id"] == 8 or event["property"] in ("ifmt", "ofmt", "acif", "acof")):
                event["asbd"] = asbd(process, data)
            observe_return(frame, {k: event[k] for k in ("function", "target", "property_id", "property")})
        elif public_property and "GetProperty" in name:
            unit = name == "AudioUnitGetProperty"
            observe_return(frame, {k: event[k] for k in ("function", "target", "property_id", "property")},
                           reg("x4") if unit else reg("x3"), reg("x5") if unit else reg("x2"))
        elif name == "AudioCodecInitialize" or name.startswith("ACDDPAtmosDecoder::Initialize("):
            event.update(input=asbd(process, reg("x1")), output=asbd(process, reg("x2")))
        elif name.startswith("ACDDPAtmosDecoder::SetCurrentOutputFormat("):
            event["output"] = asbd(process, reg("x1"))
        elif name.startswith("ACDDPAtmosDecoder::CopyUDCOutputToABL("):
            pointer = reg("x2")
            count = int.from_bytes(read(process, pointer, 4), "little")
            event.update(copy_argument=reg("w1"), buffer_count=count)
            if count <= 64:
                data = read(process, pointer + 8, count * 16)
                event["buffers"] = [dict(zip(("channels", "bytes"), struct.unpack_from("<II", data, i * 16)))
                                    for i in range(count)]
        elif name in ("AudioConverterNew", "AudioConverterNewSpecific"):
            event.update(input=asbd(process, reg("x0")), output=asbd(process, reg("x1")))
            if name == "AudioConverterNewSpecific":
                count = reg("w2")
                event["codec_count"] = count
                if count <= 16:
                    data = read(process, reg("x3"), count * 12)
                    event["codecs"] = [dict(zip(("type", "subtype", "manufacturer"),
                        [fourcc(v) for v in struct.unpack_from("<III", data, i * 12)])) for i in range(count)]
        emit(event)
    except Exception as error:
        emit({**event, "read_error": str(error)})
    return False


def save_symbols(debugger, command, result, internal_dict):
    needles = ("metadata", "spatial", "render", "pann", "vbap", "eac3", "ec3", "ac3", "joc", "dolby",
               "oamd", "objectaudio", "object_audio", "ddp", "ddplus", "atmos", "channelmap")
    rows = []
    for module in debugger.GetSelectedTarget().modules:
        filename = module.GetFileSpec().GetFilename() or ""
        if not any(x in filename for x in ("Audio", "MediaToolbox", "AVFCore")):
            continue
        symbols = []
        for symbol in module:
            name = symbol.GetName()
            if name and any(x in name.lower() for x in needles):
                symbols.append(dict(name=name, address=hex(symbol.GetStartAddress().GetFileAddress())))
        rows.append(dict(module=str(module.GetFileSpec()), uuid=module.GetUUIDString(), symbols=symbols))
    Path(command).write_text(json.dumps(rows, indent=2, ensure_ascii=False) + "\n")
    emit(dict(stage="saved_symbols", output=command, modules=len(rows)))


def __lldb_init_module(debugger, internal_dict):
    patterns = [
        r"^AudioUnitSetProperty$", r"^AudioConverterSetProperty$", r"^AudioCodecSetProperty$",
        r"^AudioCodecInitialize$", r"^AudioConverterNew(Specific)?$",
        r"^AudioCodecProduceOutputBufferList$", r"^AudioUnitRender$",
        r"^AudioQueueObject::CheckIfContentRequiresAUSpatialMixer\(",
        r"^AudioMetadataUtilities::contains(Metadata|SpatialContent)\(",
        r"^MTAudioProcessingTapGetSourceAudio$",
    ]
    if os.environ.get("ATMOS_TRACE_PATTERNS"):
        patterns = json.loads(os.environ["ATMOS_TRACE_PATTERNS"])
    for pattern in patterns:
        breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex(pattern)
        breakpoint.SetScriptCallbackFunction("trace_chain.on_call")
    debugger.HandleCommand("command script add -f trace_chain.save_symbols atmos-save-symbols")
