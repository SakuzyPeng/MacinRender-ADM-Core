"""Read-only LLDB tracing of APAC's system playback/metadata handoff.

Only use in a self-owned probe process. No return values, properties, algorithms,
capabilities or media data are modified. Local symbols are version-specific.
"""

import json
import os
from pathlib import Path
import struct

import trace_codec

counts = {}
pending = {}


def observe_return(frame, kind, fields):
    process = frame.GetThread().GetProcess()
    bp = process.GetTarget().BreakpointCreateByAddress(frame.FindRegister("x30").GetValueAsUnsigned())
    bp.SetThreadID(frame.GetThread().GetThreadID())
    pending[bp.GetID()] = kind, {**fields, "entry_sp": frame.FindRegister("sp").GetValueAsUnsigned()}
    bp.SetScriptCallbackFunction("trace_playback_chain.on_return")


def on_return(frame, location, internal_dict):
    identifier = location.GetBreakpoint().GetID()
    if identifier not in pending:
        return False
    kind, fields = pending[identifier]
    if frame.FindRegister("sp").GetValueAsUnsigned() != fields["entry_sp"]:
        return False
    pending.pop(identifier)
    location.GetBreakpoint().SetEnabled(False)
    process = frame.GetThread().GetProcess()
    value = frame.FindRegister("x0").GetValueAsUnsigned()
    event = {"stage": "playback_return", "kind": kind, "return_value": value, **fields}
    if kind == "mdpf" and value == 0:
        event["metadata_format"] = trace_codec.number(process, fields["value_pointer"], 4)
    if kind == "parse_position":
        event["parsed_position_hex"] = trace_codec.read(process, fields["position_pointer"], 64).hex()
    if "CheckIfContentRequiresAUSpatialMixer" in kind:
        pointer = fields["queue_object"]
        event["requires_spatial_content_processing"] = trace_codec.number(process, pointer + 0x43c, 1)
        event["contains_metadata"] = trace_codec.number(process, pointer + 0x43d, 1)
    print(json.dumps(event), flush=True)
    return False


def fourcc(value):
    return value.to_bytes(4, "big").decode("ascii", errors="replace")


def constant_string(process, pointer):
    try:
        raw = trace_codec.read(process, pointer, 32)
        data, length = struct.unpack_from("<QQ", raw, 16)
        if 0 < length < 512:
            data &= (1 << 48) - 1
            return trace_codec.read(process, data, length).decode("utf-8", errors="replace")
    except Exception:
        pass
    return "unreadable"


def stack(frame, count=12):
    return [{"name": item.GetFunctionName(), "module": item.GetModule().GetFileSpec().GetFilename(),
             "file_address": hex(item.GetPCAddress().GetFileAddress())}
            for item in list(frame.GetThread())[:count]]


def on_call(frame, location, internal_dict):
    process = frame.GetThread().GetProcess()
    name = frame.GetFunctionName() or frame.GetSymbol().GetName()
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    if name.startswith(("AudioMetadataFrame_", "AudioBufferList_")) and frame.GetModule().GetFileSpec().GetFilename() != "AudioToolboxCore":
        return False  # skip import trampolines
    key = name
    event = {"stage": "playback_chain", "function": name}
    try:
        if name == "___lldb_unnamed_symbol_194923ed0":
            event["media_key"] = constant_string(process, reg("x2"))
            key += ":" + event["media_key"]
        if "SetProperty" in name or "GetProperty" in name:
            prop = reg("w1")
            key = name + ":" + str(prop)
            event.update(property_id=prop, property=fourcc(prop))
            if "SetProperty" in name:
                is_unit = name == "AudioUnitSetProperty"
                is_queue = name == "AudioQueueSetProperty"
                size = reg("w5") if is_unit else reg("w3") if is_queue else reg("w2")
                pointer = reg("x4") if is_unit else reg("x2") if is_queue else reg("x3")
                event["target"] = hex(reg("x0"))
                if is_unit:
                    event.update(scope=reg("w2"), element=reg("w3"))
                event["bytes"] = size
                if pointer and 0 < size <= 256:
                    event["value_hex"] = trace_codec.read(process, pointer, size).hex()
        counts[key] = counts.get(key, 0) + 1
        limit = 8 if "MetadataBitStreamParser::parsePosition(" in name else 2
        if os.environ.get("APAC_CHAIN_POLICY") == "1":
            limit = 12
        if counts[key] > limit:
            if "Property" not in name:
                location.GetBreakpoint().SetEnabled(False)
            return False
        event["count"] = counts[key]
        event["stack"] = stack(frame)
        if name.startswith("AudioMetadataUtilities::"):
            values = struct.unpack("<d8I", trace_codec.read(process, reg("x0"), 40))
            event["queried_format"] = fourcc(values[1])
            observe_return(frame, name, {})
        if "CheckIfContentRequiresAUSpatialMixer" in name:
            observe_return(frame, name, {"queue_object": reg("x0")})
        if name == "AudioBufferList_GetMetadataFrame":
            observe_return(frame, "AudioBufferList_GetMetadataFrame", {})
        if name.startswith("ACAPACBaseDecoder::GetProperty") and event.get("property") == "mdpf":
            event["config_metadata_format"] = trace_codec.number(process, reg("x0") + 505, 1)
            event["global_config_initialized"] = trace_codec.number(process, reg("x0") + 584, 1)
        if name == "AudioCodecGetProperty" and event.get("property") == "mdpf":
            observe_return(frame, "mdpf", {"value_pointer": reg("x3")})
        if "MetadataBitStreamParser::parsePosition(" in name:
            observe_return(frame, "parse_position", {"position_pointer": reg("x1")})
        if "MetadataBitStreamParser::parse(" in name:
            pointer, size = reg("x0"), reg("w1")
            if size > 65536:
                pointer, size = reg("x1"), reg("w2")
            event["parser_bytes"] = size
            if 0 < size < 65536:
                event["parser_prefix"] = trace_codec.read(process, pointer, min(size, 64)).hex()
            observe_return(frame, "metadata_parser", {})
        if name.startswith("AudioMetadataTimeline_AP::addEvent("):
            event["event_hex"] = trace_codec.read(process, reg("x1"), 40).hex()
        if "GetOutputBufferList" in name:
            pointer = reg("x1")
            count = trace_codec.number(process, pointer, 4)
            event["buffers"] = count
            if count <= 260:
                data = trace_codec.read(process, pointer + 8, count * 16)
                event["buffer_channels_and_bytes"] = [list(struct.unpack_from("<II", data, i * 16))
                                                      for i in range(count)]
        if name.startswith("ACAPACBaseDecoder::Initialize"):
            event["decoder_object"] = hex(reg("x0"))
            for label, pointer in (("input", reg("x1")), ("output", reg("x2"))):
                if pointer:
                    values = struct.unpack("<d8I", trace_codec.read(process, pointer, 40))
                    event[label] = {"sample_rate": values[0], "format": fourcc(values[1]),
                                    "flags": values[2], "channels": values[6]}
        print(json.dumps(event), flush=True)
        return False
    except Exception as error:
        print(json.dumps({**event, "error": str(error)}), flush=True)
        return True


def save_symbols(debugger, command, result, internal_dict):
    output = Path(command)
    wanted = ("AudioToolboxCore", "AudioToolbox", "AudioCodecs", "AudioDSP", "AVFCore", "MediaToolbox", "AudioSession")
    needles = ("Metadata", "Spatial", "Renderer", "RenderContext", "ChannelMapper", "ChannelMapping",
               "AudioConverter", "AudioQueue", "Immersive", "ObjectAudio")
    rows = []
    for module in debugger.GetSelectedTarget().modules:
        filename = module.GetFileSpec().GetFilename()
        if not any(x in filename for x in wanted):
            continue
        symbols = []
        for symbol in module:
            name = symbol.GetName()
            if name and any(x in name for x in needles):
                symbols.append({"name": name, "address": hex(symbol.GetStartAddress().GetFileAddress())})
        rows.append({"module": str(module.GetFileSpec()), "uuid": module.GetUUIDString(), "symbols": symbols})
    output.write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps({"stage": "playback_symbols", "output": str(output), "modules": len(rows)}), flush=True)


def dump_function(debugger, command, result, internal_dict):
    import shlex
    module_name, address_text, output = shlex.split(command)
    target = debugger.GetSelectedTarget()
    module = next(m for m in target.modules if m.GetFileSpec().GetFilename() == module_name)
    address = module.ResolveFileAddress(int(address_text, 0))
    symbol = address.GetSymbol()
    instructions = symbol.GetInstructions(target)
    rows = [f"{hex(i.GetAddress().GetFileAddress())}: {i.GetMnemonic(target)} {i.GetOperands(target)} ; {i.GetComment(target)}"
            for i in instructions]
    Path(output).write_text("\n".join(rows) + "\n")
    print(json.dumps({"stage": "disassembly", "module": module_name, "uuid": module.GetUUIDString(),
                      "symbol": symbol.GetName(), "instructions": len(rows), "output": output}), flush=True)


def __lldb_init_module(debugger, internal_dict):
    trace_codec.verify_component()
    patterns = [
        r"^ACAPACBaseDecoder::Initialize\(", r"^ACAPACBaseDecoder::SetProperty\(",
        r"^ACAPACBaseDecoder::GetProperty\(", r"^ACAPACBaseDecoder::GetOutputBufferList\(",
        r"^ACAPACBaseDecoder::GetOutputBufferListWithMetadata\(", r"^APACMetadataSink::Process\(",
        r"^apac::obj::MetadataBase::Deserialize\(", r"^AudioConverter(SetProperty|New|NewSpecific|FillComplexBuffer)$",
        r"^AudioUnit(SetProperty|Render)$", r"^AudioQueueNewOutput$",
        r"^AudioMetadataUtilities::contains(Metadata|SpatialContent)\(",
        r"^AudioQueueObject::CheckIfContentRequiresAUSpatialMixer\(",
        r"^AudioBufferList_GetMetadataFrame$", r"^AudioMetadataFrame_(AppendEvent|BeginNew|Clear)$",
        r"^APAC::ImmersiveRendererData_V2_Manager::getMetadataConfig\(",
        r"^AudioCodecGetProperty$",
    ]
    if os.environ.get("APAC_CHAIN_CONSUMERS") == "1":
        patterns = [
            r"^APAC::MetadataBitStreamParser::parse\(", r"^APAC::MetadataBitStreamParser::parsePosition\(",
            r"^APAC::MetadataBitStreamParser::parseRendererData\(",
            r"^AudioMetadataTimeline_AP::(addEvent|retrieveMetadataForTimeframe)\(",
            r"^APAC::ImmersiveRendererData_V2_Manager::(getMetadataConfig|getNumRendererDescriptions)\(",
            r"^ScheduledSlicePlayer2::ScheduleMetadata\(", r"^AUSpatialMixerV2Factory$",
        ]
    if os.environ.get("APAC_CHAIN_POLICY") == "1":
        patterns = [r"^AudioUnitSetProperty$", r"^AudioQueueSetProperty$",
                    r"^___lldb_unnamed_symbol_194923ed0$", r"^FigAudioFormatDescriptionRequiresImmersiveRendering$"]
    for pattern in patterns:
        breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex(pattern)
        breakpoint.SetScriptCallbackFunction("trace_playback_chain.on_call")
    debugger.HandleCommand("command script add -f trace_playback_chain.save_symbols playback-save-symbols")
    debugger.HandleCommand("command script add -f trace_playback_chain.dump_function playback-dump-function")
