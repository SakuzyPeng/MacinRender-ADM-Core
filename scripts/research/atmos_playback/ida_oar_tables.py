"""Export OAR speaker tables and instruction evidence from the local IDA DB."""

import json
import os
from pathlib import Path
import struct

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_pro
import ida_ua
import idautils

root = Path(os.environ["ATMOS_IDA_TABLE_OUTPUT"])
root.mkdir(exist_ok=True)
ida_auto.auto_wait()
names = dict((name, ea) for ea, name in idautils.Names())
rows = {}
for name, address in names.items():
    if "SPEAKER_CONFIG_SPK_POS" in name:
        value = ida_bytes.get_bytes(address, 35 * 16)
        rows[name] = dict(address=hex(address), values=[list(v) for v in struct.iter_unpack("<4i", value)])
    elif "A_ROOM_CONFIG_SPKS" in name:
        rows[name] = dict(address=hex(address), prefix_hex=ida_bytes.get_bytes(address, 64).hex())
(root / "speaker-tables.json").write_text(json.dumps(rows, indent=2) + "\n")
address = names["_speaker_config_init"]
lines = []
for ea in idautils.FuncItems(address):
    lines.append(dict(address=hex(ea), text=ida_ua.print_insn_mnem(ea)))
(root / "speaker-config-init-bytes.json").write_text(json.dumps(dict(
    address=hex(address), end=hex(ida_funcs.get_func(address).end_ea),
    bytes=ida_bytes.get_bytes(address, ida_funcs.get_func(address).end_ea-address).hex(), instructions=lines), indent=2) + "\n")
ida_pro.qexit(0)
