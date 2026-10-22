"""Report full-span PMU metrics; never apportion counters at a stage boundary."""
import argparse
import csv
import gzip
import json
import struct
from bisect import bisect_left
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
p = parser.parse_args().directory
summary = json.loads((p / "pmu-summary.json").read_text())
offset = summary["clock"]["offset_ns"]
with (p / "flight.bin").open("rb") as f:
    header = json.loads(f.readline())
    payload = f.read()
main = header["main_tid"]
coordinator = summary["threads"]["coordinator"]
ns = lambda tick: (tick - header["start_before"]) * 1_000_000_000 // header["frequency"]
stacks = defaultdict(list)
calls = defaultdict(list)
for tick, call, tid, stage, begin, _ in struct.iter_unpack("<QQIIII", payload):
    if begin:
        stacks[tid].append((tick, call, stage))
    else:
        a, cid, kind = stacks[tid].pop()
        assert (cid, kind) == (call, stage)
        calls[call].append({"start": ns(a), "end": ns(tick), "tid": tid, "stage": stage})
spans = defaultdict(list)
with gzip.open(p / "pmu-target.csv.gz", "rt", encoding="utf-8") as f:
    for row in csv.DictReader(f):
        v = {k: int(x) for k, x in row.items()}
        v["start"] = v.pop("start_etw_ns") + offset
        v["end"] = v.pop("end_etw_ns") + offset
        spans[v["tid"]].append(v)
ends = {tid: [v["end"] for v in group] for tid, group in spans.items()}

def metric(a, b, tid):
    group = spans[tid]
    at = bisect_left(ends[tid], a)
    on_cpu = included = instructions = cycles = reference = 0
    cpus = defaultdict(int)
    selected = []
    while at < len(group) and group[at]["start"] < b:
        v = group[at]
        at += 1
        duration = max(0, min(v["end"], b) - max(v["start"], a))
        on_cpu += duration
        cpus[v["cpu"]] += duration
        if v["start"] >= a and v["end"] <= b:
            instructions += v["instructions"]
            cycles += v["core_cycles"]
            reference += v["reference_cycles"]
            included += duration
            selected.append(v)
    return {
        "wall_ms": (b - a) / 1e6, "on_cpu_ms": on_cpu / 1e6,
        "counter_covered_on_cpu_ms": included / 1e6,
        "counter_coverage_fraction": included / on_cpu if on_cpu else 0,
        "instructions": instructions, "core_cycles": cycles, "reference_cycles": reference,
        "core_ghz": cycles / included if included else None,
        "reference_ghz": reference / included if included else None,
        "ipc": instructions / cycles if cycles else None,
        "cpu_ms": {str(k): v / 1e6 for k, v in cpus.items()},
    }

results = []
names = {19: "call", 7: "roundtrip", 8: "worker_execution", 13: "voice_batch", 11: "late_decoder",
         17: "gpu_submit", 18: "gpu_wait"}
for call in (1000, 15000, 16973, 19649, 19648, 19650):
    values = calls[call]
    results.append({"call": call, "regions": {
        name: [{"start_ms": v["start"] / 1e6, **metric(v["start"], v["end"], v["tid"])}
               for v in values if v["stage"] == stage]
        for stage, name in names.items()
    }})
windows = []
for a, b in [(975, 1025), (14975, 15025), (16948, 16998), (19624, 19674)]:
    start = next(v["start"] for v in calls[a] if v["stage"] == 19)
    end = next(v["end"] for v in calls[b - 1] if v["stage"] == 19)
    caller = metric(start, end, main)
    driver = metric(start, end, coordinator)
    windows.append({
        "first_call": a, "end_call_exclusive": b, "start_ms": start / 1e6, "end_ms": end / 1e6,
        "caller": caller, "coordinator": driver,
    })
report = {"clock": summary["clock"], "method": "Only wholly contained CSwitch intervals contribute hardware counters",
          "selected_calls": results, "windows": windows}
(p / "pmu-selected.json").write_text(json.dumps(report, indent=2) + "\n")
print("Wrote pmu-selected.json")
