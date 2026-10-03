"""Queue a version-pinned Logic bridge request and let the caller detach immediately."""
import json
from pathlib import Path
import lldb


def call(target, address, signature, argument):
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeC_plus_plus)
    options.SetTimeoutInMicroSeconds(10_000_000)
    options.SetUnwindOnError(True)
    options.SetIgnoreBreakpoints(True)
    value = target.EvaluateExpression(f"(({signature}){address:#x})({argument})", options)
    if not value.GetError().Success():
        raise RuntimeError(str(value.GetError()))
    return value


def run(debugger, command, result, internal_dict):
    try:
        library, request = command.split(" ", 1)
        if not Path(library).is_file() or not Path(request).is_file():
            raise ValueError("Library and request must exist")
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next(entry.GetSymbol().GetStartAddress().GetLoadAddress(target) for entry in symbols
                      if entry.GetSymbol().GetStartAddress().IsValid())
        loaded = call(target, loader, "void*(*)(const char*,int)", f"{json.dumps(library)},2")
        if loaded.GetValueAsUnsigned() == 0:
            raise RuntimeError("Bridge dlopen failed")
        address = next(symbol.GetStartAddress().GetLoadAddress(target)
                       for module in target.modules
                       if str(Path(module.GetFileSpec().GetDirectory()) / module.GetFileSpec().GetFilename()) == library
                       for symbol in module if symbol.GetName() == "logic_queue_request")
        queued = call(target, address, "bool(*)(const char*)", json.dumps(request))
        if queued.GetValueAsUnsigned() != 1:
            raise RuntimeError("Request was not queued")
        print(json.dumps({"scheduled": True}), flush=True)
    except Exception as exc:
        print(json.dumps({"scheduled": False, "error": str(exc)}), flush=True)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f logic_lldb.run logic-request")
