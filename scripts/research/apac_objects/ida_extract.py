"""Run with IDA's -S option; keep generated pseudocode in an ignored local directory.

APAC_RESEARCH_OUTPUT selects the output directory. APAC_RESEARCH_PATTERNS is a
JSON array of symbol substrings, or a JSON file containing that array.
This script reads the database and never patches the input component.
"""

import json
import os
from pathlib import Path
import traceback

import ida_auto
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import idautils


def main():
    output = Path(os.environ["APAC_RESEARCH_OUTPUT"])
    output.mkdir(parents=True, exist_ok=True)
    selection = os.environ["APAC_RESEARCH_PATTERNS"]
    patterns = json.loads(selection if selection.startswith("[") else Path(selection).read_text())
    ida_auto.auto_wait()
    if not ida_hexrays.init_hexrays_plugin():
        raise RuntimeError("Hex-Rays is unavailable")
    results = []
    for address in idautils.Functions():
        name = ida_name.get_name(address)
        if not any(pattern in name for pattern in patterns):
            continue
        item = {"address": hex(address), "name": name, "demangled": ida_name.demangle_name(name, 0)}
        try:
            function = ida_hexrays.decompile(address)
            if function is None:
                raise RuntimeError("decompile returned no function")
            filename = f"fn_{address:x}.c"
            (output / filename).write_text(str(function), encoding="utf-8")
            item["file"] = filename
            item["end"] = hex(ida_funcs.get_func(address).end_ea)
        except Exception as error:
            item["error"] = str(error)
        results.append(item)
    (output / "index.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


try:
    main()
except Exception:
    output = Path(os.environ["APAC_RESEARCH_OUTPUT"])
    output.mkdir(parents=True, exist_ok=True)
    (output / "error.txt").write_text(traceback.format_exc(), encoding="utf-8")
    ida_pro.qexit(1)
else:
    ida_pro.qexit(0)
