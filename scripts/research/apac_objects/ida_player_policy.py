"""IDA -S script: export QuickTime callers of audio strategy/mix selectors."""

import json
import os
from pathlib import Path

import ida_auto
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import idautils

root = Path(os.environ["APAC_RESEARCH_OUTPUT"])
root.mkdir(parents=True, exist_ok=True)
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
needles = ("setMultichannelAudioStrategy:", "setAllowedAudioSpatializationFormats:",
           "setAudioSpatializationAllowed:", "setAudioMix:", "AVPlayerMultichannelAudioStrategy")
seeds = {address: name for address, name in idautils.Names() if any(x in name for x in needles)}
for item in idautils.Strings():
    if any(x in str(item) for x in needles):
        seeds[item.ea] = str(item)
functions = {}
references = []
for address, label in seeds.items():
    queue = [(address, 0)]
    seen = set()
    while queue:
        target, depth = queue.pop()
        if target in seen:
            continue
        seen.add(target)
        for ref in idautils.XrefsTo(target):
            function = ida_funcs.get_func(ref.frm)
            references.append({"seed": label, "target": hex(target), "from": hex(ref.frm), "depth": depth})
            if function:
                functions.setdefault(function.start_ea, set()).add(label)
            elif depth < 2:
                queue.append((ref.frm, depth + 1))
rows = []
for address, labels in functions.items():
    row = {"address": hex(address), "name": ida_funcs.get_func_name(address), "seeds": sorted(labels)}
    try:
        output = ida_hexrays.decompile(address)
        filename = f"fn_{address:x}.c"
        (root / filename).write_text(str(output))
        row["file"] = filename
    except Exception as error:
        row["error"] = str(error)
    rows.append(row)
(root / "index.json").write_text(json.dumps(rows, indent=2) + "\n")
(root / "references.json").write_text(json.dumps(references, indent=2) + "\n")
ida_pro.qexit(0)
