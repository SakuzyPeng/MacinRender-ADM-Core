"""Batch IDAPython extraction for Renderer gain-path research.

DAR_IDA_OUTPUT selects the output directory. DAR_IDA_FUNCTIONS optionally lists
comma-separated function addresses for focused follow-up dumps in the same DB.
"""

import hashlib
import json
import os
import traceback
from pathlib import Path

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_kernwin
import ida_lines
import ida_nalt
import ida_name
import ida_pro
import idautils

ANCHORS = (
    "homeSpeakerGains", "cinemaSpeakerGains", "TorchLightPanner",
    "Spectral Decorrelator", "atmos_storage_iab_dyn_metadata_preprocessor_process",
    "SpectralDecorrelator", "hasPanner",
)


def address(value):
    return hex(int(value))


def function_at(ea):
    function = ida_funcs.get_func(ea)
    if function is None:
        return None
    return {"start": address(function.start_ea), "end": address(function.end_ea),
            "rva": address(function.start_ea - ida_nalt.get_imagebase()),
            "name": ida_funcs.get_func_name(function.start_ea)}


def incoming(ea):
    return [{"from": address(xref.frm), "to": address(xref.to),
             "from_rva": address(xref.frm - ida_nalt.get_imagebase()),
             "type": int(xref.type), "function": function_at(xref.frm)}
            for xref in idautils.XrefsTo(ea)]


def dump_function(ea, destination, decompiler):
    function = ida_funcs.get_func(ea)
    if function is None:
        ida_funcs.add_func(ea)
        ida_auto.auto_wait()
        function = ida_funcs.get_func(ea)
        if function is None:
            return {"requested": address(ea), "error": "no function at address"}
    ea = function.start_ea
    result = function_at(ea)
    result["incoming"] = incoming(ea)
    result["calls"] = []
    assembly = []
    for item in idautils.FuncItems(ea):
        assembly.append({"address": address(item),
                         "rva": address(item - ida_nalt.get_imagebase()),
                         "text": ida_lines.tag_remove(ida_lines.generate_disasm_line(item, 0) or ""),
                         "bytes": (ida_bytes.get_bytes(item, ida_bytes.get_item_size(item)) or b"").hex()})
        for target in idautils.CodeRefsFrom(item, False):
            called = ida_funcs.get_func(target)
            if called is not None and called.start_ea != ea and target == called.start_ea:
                result["calls"].append({"at": address(item), "target": address(target),
                                        "name": ida_funcs.get_func_name(target)})
    prefix = destination / f"{ea:x}"
    prefix.with_suffix(".asm.json").write_text(json.dumps(assembly, indent=2) + "\n")
    if decompiler:
        try:
            decompiled = ida_hexrays.decompile(ea)
            if decompiled is not None:
                prefix.with_suffix(".c").write_text(str(decompiled) + "\n")
                result["pseudocode"] = str(prefix.with_suffix(".c"))
        except Exception as error:
            result["decompile_error"] = str(error)
    return result


def main():
    output = Path(os.environ["DAR_IDA_OUTPUT"])
    output.mkdir(parents=True, exist_ok=True)
    ida_auto.auto_wait()
    overlay_path = os.environ.get("DAR_IDA_OVERLAY_JSON")
    overlay = None
    if overlay_path:
        overlay = json.loads(Path(overlay_path).read_text())
        for region in overlay["regions"]:
            data = Path(region["path"]).read_bytes()
            if hashlib.sha256(data).hexdigest() != region["sha256"]:
                raise ValueError("runtime code snapshot changed")
            start = int(region["address"], 0)
            end = start + len(data)
            for ea in list(idautils.Functions(start, end)):
                ida_funcs.del_func(ea)
            ida_bytes.del_items(start, ida_bytes.DELIT_SIMPLE, len(data))
            # Changes only the local analysis database. The installed binary and
            # the hashed on-disk arm64 slice remain untouched.
            ida_bytes.put_bytes(start, data)
        for ea in overlay.get("entry_functions", []):
            ida_funcs.add_func(int(ea, 0))
        ida_auto.auto_wait()
    functions_dir = output / "functions"
    functions_dir.mkdir(exist_ok=True)
    input_path = Path(ida_nalt.get_input_file_path())
    report = {"input": str(input_path), "ida_version": ida_kernwin.get_kernel_version(),
              "image_base": address(ida_nalt.get_imagebase()), "anchors": [], "functions": []}
    report["function_count"] = ida_funcs.get_func_qty()
    if overlay is not None:
        report["runtime_overlay"] = overlay
    if input_path.is_file():
        report["input_sha256"] = hashlib.sha256(input_path.read_bytes()).hexdigest()
    requested = os.environ.get("DAR_IDA_FUNCTIONS", "")
    anchors = json.loads(os.environ["DAR_IDA_ANCHORS_JSON"]) if os.environ.get("DAR_IDA_ANCHORS_JSON") else ANCHORS
    functions = set()
    if requested:
        functions = {int(item.strip(), 0) for item in requested.split(",") if item.strip()}
    else:
        for item in idautils.Strings():
            value = str(item)
            if not any(anchor.lower() in value.lower() for anchor in anchors):
                continue
            entry = {"address": address(item.ea), "text": value, "incoming": incoming(item.ea)}
            frontier = [(item.ea, 0)]
            visited = set()
            links = []
            while frontier:
                current, depth = frontier.pop(0)
                if current in visited or depth > 3:
                    continue
                visited.add(current)
                for ref in incoming(current):
                    links.append({"depth": depth, **ref})
                    if ref["function"]:
                        functions.add(int(ref["function"]["start"], 0))
                    elif depth < 3:
                        frontier.append((int(ref["from"], 0), depth + 1))
            entry["reference_chain"] = links
            report["anchors"].append(entry)
    decompiler = ida_hexrays.init_hexrays_plugin()
    for ea in sorted(functions):
        report["functions"].append(dump_function(ea, functions_dir, decompiler))
    (output / "locator.json").write_text(json.dumps(report, indent=2) + "\n")


try:
    main()
except Exception:
    output = Path(os.environ.get("DAR_IDA_OUTPUT", "."))
    output.mkdir(parents=True, exist_ok=True)
    (output / "error.txt").write_text(traceback.format_exc())
    ida_pro.qexit(1)
else:
    ida_pro.qexit(0)
