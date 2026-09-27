"""LLDB side of run_headless_rerender.py. Attach, queue one action, detach."""

import json
from pathlib import Path

import lldb


def _call(target, address: int, signature: str, argument: str) -> lldb.SBValue:
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeC_plus_plus)
    options.SetTimeoutInMicroSeconds(20_000_000)
    options.SetUnwindOnError(True)
    options.SetIgnoreBreakpoints(True)
    return target.EvaluateExpression(f"(({signature}){address:#x})({argument})", options)


def run(debugger, command, result, internal_dict):
    try:
        library_path, request_path = command.strip().split(" ", 1)
        if not Path(library_path).is_file() or not Path(request_path).is_file():
            raise RuntimeError("bridge library or request file does not exist")
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next(
            (entry.GetSymbol().GetStartAddress().GetLoadAddress(target)
             for entry in symbols if entry.GetSymbol().GetStartAddress().IsValid()),
            None,
        )
        if loader is None:
            raise RuntimeError("dlopen not found")
        loaded = _call(target, loader, "void*(*)(const char*,int)",
                       f"{json.dumps(library_path)},2")
        if not loaded.GetError().Success() or loaded.GetValue() in (None, "0x0"):
            raise RuntimeError(f"dlopen failed: {loaded.GetError()}")
        symbol_name = "dar_queue_batch_request"
        address = next(
            (symbol.GetStartAddress().GetLoadAddress(target)
             for module in target.modules
             if str(Path(module.GetFileSpec().GetDirectory()) / module.GetFileSpec().GetFilename()) == library_path
             for symbol in module
             if symbol.GetName() == symbol_name and symbol.GetStartAddress().IsValid()),
            None,
        )
        if address is None:
            raise RuntimeError("batch bridge function not found")
        queued = _call(target, address, "bool(*)(const char*)", json.dumps(request_path))
        if not queued.GetError().Success() or queued.GetValue() != "true":
            raise RuntimeError(f"request was not queued: {queued.GetError()} / {queued.GetValue()}")
        print(json.dumps({"scheduled": True}), flush=True)
    except Exception as exc:
        print(json.dumps({"scheduled": False, "error": str(exc)}), flush=True)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f dar_batch_lldb.run dar-batch-run")
