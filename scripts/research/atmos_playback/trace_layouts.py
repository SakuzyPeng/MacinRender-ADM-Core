"""Read-only layout selection observations for the self-owned offline probe."""

import json
import os
from pathlib import Path

counts = {}
related_saved = False


def save_symbol(symbol, target, destination):
    instructions = symbol.GetInstructions(target)
    lines = [f"{hex(i.GetAddress().GetFileAddress())}: {i.GetMnemonic(target)} {i.GetOperands(target)} ; {i.GetComment(target)}"
             for i in instructions]
    destination.write_text("\n".join(lines) + "\n")


def on_call(frame, location, internal_dict):
    global related_saved
    name = frame.GetFunctionName() or frame.GetSymbol().GetName()
    counts[name] = counts.get(name, 0) + 1
    if counts[name] > 2:
        location.SetEnabled(False)
        return False
    reg = lambda key: frame.FindRegister(key).GetValueAsUnsigned()
    target = frame.GetThread().GetProcess().GetTarget()
    event = dict(function=name, count=counts[name], x0=hex(reg("x0")),
                 w1=reg("w1"), w2=reg("w2"), x3=hex(reg("x3")),
                 stack=[f.GetFunctionName() or f.GetSymbol().GetName() for f in list(frame.GetThread())[:8]])
    print(json.dumps(event), flush=True)
    root = Path(os.environ["ATMOS_LAYOUT_TRACE_DIR"])
    stem = name.split("::")[-1].split("(")[0]
    destination = root / (stem + ".asm")
    if not destination.exists():
        save_symbol(frame.GetSymbol(), target, destination)
    if not related_saved:
        prefixes = tuple("ACDDPAtmosDecoder::" + value + "(" for value in
                         ("Initialize", "UpdateChannelMappingMatrix", "CopyUDCOutputToABL", "SetCurrentOutputFormat"))
        for symbol in frame.GetModule():
            symbol_name = symbol.GetName() or ""
            if symbol_name.startswith(prefixes):
                path = root / (symbol_name.split("::")[-1].split("(")[0] + ".asm")
                if not path.exists():
                    save_symbol(symbol, target, path)
        related_saved = True
    return False


def __lldb_init_module(debugger, internal_dict):
    for pattern in (r"^ACDDPAtmosDecoder::UpdateChannelMappingMatrix\(",
                    r"^ACDDPAtmosDecoder::ResetChannelMappingMatrix\(",
                    r"^ACDDPAtmosDecoder::SetProperty\("):
        breakpoint = debugger.GetSelectedTarget().BreakpointCreateByRegex(pattern)
        breakpoint.SetScriptCallbackFunction("trace_layouts.on_call")
