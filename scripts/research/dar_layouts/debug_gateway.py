"""Renderer research commands: read-only inspection and event-loop gateway calls.

Mutating commands open an ADM, change temporary export settings, or export WAV.
They are hardcoded to this local probe and require an idle Renderer process.
Always detach LLDB so queued gateway calls can run on the app event loop.
"""

import json
from pathlib import Path

import lldb


def snapshot(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"symbols": [], "errors": []}
    try:
        target = debugger.GetSelectedTarget()
        process = target.GetProcess()
        for module in target.modules:
            if module.GetFileSpec().GetFilename() != "Dolby Atmos Renderer":
                continue
            for symbol in module:
                name = symbol.GetName() or ""
                if "ImportAtmosConfigGateway" not in name:
                    continue
                if not any(word in name for word in ("tpRegistered", "::tp", "::cnt", "staticMetaObject")):
                    continue
                address = symbol.GetStartAddress().GetLoadAddress(target)
                error = lldb.SBError()
                value = process.ReadMemory(address, 8, error)
                data["symbols"].append({"name": name, "address": hex(address),
                                        "value": hex(int.from_bytes(value, "little")) if error.Success() else None,
                                        "read_error": "" if error.Success() else str(error)})
    except Exception as exc:
        data["errors"].append(str(exc))
    output.write_text(json.dumps(data, indent=2) + "\n")
    print(json.dumps(data), flush=True)


def call_address(target, address, signature, argument):
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeC_plus_plus)
    options.SetTimeoutInMicroSeconds(20000000)
    options.SetUnwindOnError(True)
    options.SetIgnoreBreakpoints(True)
    expression = f"(({signature}){address:#x})({argument})"
    value = target.EvaluateExpression(expression, options)
    error = value.GetError()
    return {"expression": expression, "value": value.GetValue(),
            "error": "" if error.Success() else str(error)}


def inspect(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"steps": [], "errors": []}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/gateway_shim.dylib"
        load = call_address(target, address, "void*(*)(const char*,int)", f'"{library}",2')
        data["steps"].append({"action": "dlopen", **load})
        if load["error"] or not load["value"] or load["value"] == "0x0":
            raise RuntimeError("diagnostic library did not load")
        symbols = target.FindSymbols("dar_gateway_inspect", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("inspection symbol not found after dlopen")
        record = output.with_suffix(".result.json")
        probe = call_address(target, address, "bool(*)(const char*)", f'"{record}"')
        data["steps"].append({"action": "inspect", **probe})
    except Exception as exc:
        data["errors"].append(str(exc))
    output.write_text(json.dumps(data, indent=2) + "\n")
    print(json.dumps(data), flush=True)


def open_config(debugger, command, result, internal_dict):
    data = {"error": "", "call": {}, "load": {}}
    try:
        variant = command.strip()
        if variant not in ("control-714", "target-222"):
            raise ValueError("Allowed variants: control-714 or target-222")
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dar_gateway_open_config", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
            loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                           for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
            if loader is None:
                raise RuntimeError("dlopen symbol not found")
            library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/gateway_shim_next.dylib"
            data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
            if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
                raise RuntimeError("diagnostic library did not load")
            symbols = target.FindSymbols("dar_gateway_open_config", lldb.eSymbolTypeCode)
            address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                            for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
            if address is None:
                raise RuntimeError("new diagnostic method not found after dlopen")
        root = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925"
        config = f"{root}/dac-probes/{variant}.dac"
        record = f"{root}/gateway-{variant}-call.json"
        data["call"] = call_address(target, address, "bool(*)(const char*,const char*)", f'"{config}","{record}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def inspect_exports(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "call": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/export_gateway_inventory3.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("inventory library did not load")
        symbols = target.FindSymbols("dar_export_gateway_inventory2", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("inventory method not found")
        data["call"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def snapshot_export_singletons(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"symbols": [], "errors": []}
    wanted = ("MasterFileGateway", "RerenderExporterGateway", "RerenderConfigGateway")
    try:
        target = debugger.GetSelectedTarget()
        process = target.GetProcess()
        for module in target.modules:
            if module.GetFileSpec().GetFilename() != "Dolby Atmos Renderer":
                continue
            for symbol in module:
                name = symbol.GetName() or ""
                if not any(item in name for item in wanted):
                    continue
                if not any(item in name for item in ("::tpRegistered", "::tp", "::cnt")):
                    continue
                address = symbol.GetStartAddress().GetLoadAddress(target)
                error = lldb.SBError()
                value = process.ReadMemory(address, 8, error)
                data["symbols"].append({"name": name,
                                        "value": hex(int.from_bytes(value, "little")) if error.Success() else None,
                                        "read_error": "" if error.Success() else str(error)})
    except Exception as exc:
        data["errors"].append(str(exc))
    output.write_text(json.dumps(data, indent=2) + "\n")
    print(json.dumps({"count": len(data["symbols"]), "errors": data["errors"]}), flush=True)


def inspect_export_resources(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "call": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/export_gateway_resources.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("resource library did not load")
        symbols = target.FindSymbols("dar_export_resource_inventory", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("resource method not found")
        data["call"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def dump_export_qml(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "call": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/export_resource_dump.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("dump library did not load")
        symbols = target.FindSymbols("dar_dump_resource", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("dump method not found")
        resource = ":/imports/Fugu/Exporters/RerenderExporterDialog.qml"
        data["call"] = call_address(target, address, "bool(*)(const char*,const char*)",
                                    f'"{resource}","{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def dump_rerender_config_qml(debugger, command, result, internal_dict):
    data = {"load": {}, "call": {}, "error": ""}
    try:
        variant, output_text = command.strip().split(" ", 1)
        names = {"config": "RerendersConfigDialog.qml",
                 "properties": "RerenderPropertiesDialog.qml",
                 "info": "RerenderInfo.qml"}
        resource = ":/imports/Fugu/Rerenders/" + names[variant]
        output = Path(output_text)
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/export_resource_dump.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("QML dump library did not load")
        symbols = target.FindSymbols("dar_dump_resource", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("QML dump method not found")
        data["call"] = call_address(target, address, "bool(*)(const char*,const char*)",
                                    f'"{resource}","{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_headless_export_gateway(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_export_gateway_queued.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("queued gateway library did not load")
        symbols = target.FindSymbols("dar_headless_queue_create_export_gateway", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("queue method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def scan_open_master_qml(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "call": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/qml_open_master_scan.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("QML scan library did not load")
        symbols = target.FindSymbols("dar_scan_open_master_qml", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("QML scan method not found")
        data["call"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_open_probe_adm(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_open_master.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("open-master library did not load")
        symbols = target.FindSymbols("dar_headless_queue_open_probe_adm", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("open-master method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_snapshot_exporter(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_export_snapshot.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("snapshot library did not load")
        symbols = target.FindSymbols("dar_headless_queue_snapshot_exporter", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("snapshot method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def headless_export_action(debugger, command, result, internal_dict):
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        action, output_text = command.strip().split(" ", 1)
        symbols_by_action = {
            "configure": "dar_queue_configure_probe_916",
            "start": "dar_queue_start_probe_916",
            "start714": "dar_queue_start_probe_714",
            "restore": "dar_queue_restore_export_settings",
        }
        symbol_name = symbols_by_action[action]
        output = Path(output_text)
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        suffix = "headless_rerender_run_714.dylib" if action == "start714" else "headless_rerender_run.dylib"
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/" + suffix
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("headless re-render library did not load")
        address = next((symbol.GetStartAddress().GetLoadAddress(target)
                        for module in target.modules
                        if module.GetFileSpec().GetFilename() == suffix
                        for symbol in module
                        if symbol.GetName() == symbol_name and symbol.GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError(f"{symbol_name} not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_snapshot_rerender_config(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_config_snapshot.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("config snapshot library did not load")
        symbols = target.FindSymbols("dar_headless_queue_snapshot_rerender_config", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("config snapshot method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def headless_layout_action(debugger, command, result, internal_dict):
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        action, output_text = command.strip().split(" ", 1)
        symbols_by_action = {
            "stage714": "dar_queue_stage_714",
            "apply714": "dar_queue_apply_714",
            "stage916": "dar_queue_stage_916",
            "apply916": "dar_queue_apply_916",
        }
        symbol_name = symbols_by_action[action]
        output = Path(output_text)
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_layout_switch.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("layout switch library did not load")
        symbols = target.FindSymbols(symbol_name, lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError(f"{symbol_name} not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_refresh_exporter(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_refresh_exporter.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("refresh library did not load")
        symbols = target.FindSymbols("dar_headless_queue_refresh_export_gateway", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("refresh method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def queue_release_exporter(debugger, command, result, internal_dict):
    output = Path(command.strip())
    data = {"load": {}, "queue": {}, "error": ""}
    try:
        target = debugger.GetSelectedTarget()
        symbols = target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen symbol not found")
        library = "/Users/Sakuzy/code/cpp/MacinRender-ADM-Core/local/dar-222-20260925/bin/headless_release_exporter.dylib"
        data["load"] = call_address(target, loader, "void*(*)(const char*,int)", f'"{library}",2')
        if data["load"]["error"] or not data["load"]["value"] or data["load"]["value"] == "0x0":
            raise RuntimeError("release library did not load")
        symbols = target.FindSymbols("dar_headless_queue_release_export_gateway", lldb.eSymbolTypeCode)
        address = next((item.GetSymbol().GetStartAddress().GetLoadAddress(target)
                        for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if address is None:
            raise RuntimeError("release method not found")
        data["queue"] = call_address(target, address, "bool(*)(const char*)", f'"{output}"')
    except Exception as exc:
        data["error"] = str(exc)
    print(json.dumps(data), flush=True)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f debug_gateway.snapshot gateway-snapshot")
    debugger.HandleCommand("command script add -f debug_gateway.inspect gateway-inspect")
    debugger.HandleCommand("command script add -f debug_gateway.open_config gateway-open-config")
    debugger.HandleCommand("command script add -f debug_gateway.inspect_exports gateway-inspect-exports")
    debugger.HandleCommand("command script add -f debug_gateway.snapshot_export_singletons gateway-snapshot-exports")
    debugger.HandleCommand("command script add -f debug_gateway.inspect_export_resources gateway-inspect-export-resources")
    debugger.HandleCommand("command script add -f debug_gateway.dump_export_qml gateway-dump-export-qml")
    debugger.HandleCommand("command script add -f debug_gateway.dump_rerender_config_qml gateway-dump-rerender-qml")
    debugger.HandleCommand("command script add -f debug_gateway.queue_headless_export_gateway gateway-queue-headless-export")
    debugger.HandleCommand("command script add -f debug_gateway.scan_open_master_qml gateway-scan-open-master-qml")
    debugger.HandleCommand("command script add -f debug_gateway.queue_open_probe_adm gateway-queue-open-probe-adm")
    debugger.HandleCommand("command script add -f debug_gateway.queue_snapshot_exporter gateway-queue-snapshot-exporter")
    debugger.HandleCommand("command script add -f debug_gateway.headless_export_action gateway-headless-export-action")
    debugger.HandleCommand("command script add -f debug_gateway.queue_snapshot_rerender_config gateway-queue-snapshot-rerender-config")
    debugger.HandleCommand("command script add -f debug_gateway.headless_layout_action gateway-headless-layout-action")
    debugger.HandleCommand("command script add -f debug_gateway.queue_refresh_exporter gateway-queue-refresh-exporter")
    debugger.HandleCommand("command script add -f debug_gateway.queue_release_exporter gateway-queue-release-exporter")
