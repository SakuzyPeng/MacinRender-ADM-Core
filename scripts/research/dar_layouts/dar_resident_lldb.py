"""Install a resident capture server, then let LLDB detach immediately."""

import json
from pathlib import Path

import lldb


def invoke(target, address, signature, arguments):
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeC_plus_plus)
    options.SetTimeoutInMicroSeconds(20_000_000)
    options.SetIgnoreBreakpoints(True)
    options.SetUnwindOnError(True)
    result = target.EvaluateExpression(f"(({signature}){address:#x})({arguments})", options)
    if result.GetError().Fail():
        raise RuntimeError(str(result.GetError()))
    return result


def run(debugger, command, result, internal_dict):
    try:
        config_path = Path(command.strip())
        config = json.loads(config_path.read_text())
        path = config["library"]
        target = debugger.GetSelectedTarget()
        main_module = target.GetModuleAtIndex(0)
        if main_module.GetUUIDString().upper() != config["module_uuid"].upper():
            raise RuntimeError("loaded main-image UUID differs from the identified Renderer")
        if target.GetTriple().split("-")[0] != "arm64":
            raise RuntimeError("capture ABI requires the arm64 Renderer process")
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen not found")
        handle = invoke(target, loader, "void*(*)(const char*,int)", f"{json.dumps(path)},2")
        if handle.GetValueAsUnsigned() == 0:
            raise RuntimeError("resident library did not load")
        address = next((symbol.GetStartAddress().GetLoadAddress(target)
                        for module in target.modules
                        if str(Path(module.GetFileSpec().GetDirectory()) / module.GetFileSpec().GetFilename()) == path
                        for symbol in module if symbol.GetName() == "dar_resident_start"), None)
        if address is None:
            raise RuntimeError("resident start function missing")
        scheduled = invoke(target, address, "bool(*)(const char*)", json.dumps(str(config_path)))
        if scheduled.GetValueAsUnsigned() == 0:
            raise RuntimeError("resident start was not scheduled")
        print(json.dumps({"resident_scheduled": True}), flush=True)
    except Exception as error:
        print(json.dumps({"resident_scheduled": False, "error": str(error)}), flush=True)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f dar_resident_lldb.run dar-resident-start")
