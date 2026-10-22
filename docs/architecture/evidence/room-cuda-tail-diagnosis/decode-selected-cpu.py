"""Decode selected flight ranges directly from ETL without a large CSV intermediate."""
import argparse
import csv
import json
import struct
import subprocess
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("--xperf", required=True)
parser.add_argument("--call", type=int, action="append", default=[])
args = parser.parse_args()
out = args.directory
result = json.loads((out / "result.json").read_text())
wanted = {v["benchmark_call"] for v in result["slowest_calls"][:12]} | {32, 1000} | set(args.call)
with (out / "flight.bin").open("rb") as f:
    meta = json.loads(f.readline())
    payload = f.read()

def ns(tick):
    return (tick - meta["start_before"]) * 1_000_000_000 // meta["frequency"]

stacks = defaultdict(list)
ranges = defaultdict(list)
for tick, call, tid, stage, begin, _ in struct.iter_unpack("<QQIIII", payload):
    if call not in wanted:
        continue
    if begin:
        stacks[tid].append((tick, call, stage))
    else:
        start, cid, kind = stacks[tid].pop()
        assert (cid, kind) == (call, stage)
        ranges[call].append({"start": ns(start), "end": ns(tick), "tid": tid, "stage": stage})
assert all(not v for v in stacks.values())
windows = [(v["start"], v["end"]) for group in ranges.values() for v in group if v["stage"] == 19]
threads = set(meta["workers"] + [meta["main_tid"]])
headers = {}
anchors = {}
provisional = None
active = {}
inactive = {}
running = defaultdict(list)
off_cpu = defaultdict(list)

def keep(a, b):
    return provisional is not None and any(a + provisional < y and b + provisional > x for x, y in windows)

with (out / "selected-cpu-dump.log").open("w") as log:
    proc = subprocess.Popen(
        [args.xperf, "-i", str(out / "merged.etl"), "-a", "dumper"],
        stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8-sig", errors="replace",
    )
    in_header = True
    for raw in csv.reader(proc.stdout, skipinitialspace=True):
        if not raw:
            continue
        row = [v.strip() for v in raw]
        name = row[0]
        if name == "EndHeader":
            in_header = False
            continue
        if in_header:
            if name in ("CSwitch", "ReadyThread", "UnknownEvent/Crimson"):
                assert name not in headers or headers[name] == row
                headers[name] = row
            continue
        if name not in headers:
            continue
        assert len(row) == len(headers[name]), (name, len(row), len(headers[name]))
        fields = dict(zip(headers[name], row))
        t = int(fields["TimeStamp"]) * 1000
        if name == "UnknownEvent/Crimson":
            if "973c1dc0-11c4-4ae9-8f78-316c8d586f8a" not in ",".join(row).lower():
                continue
            ident = int(fields["Id"], 16)
            anchors[ident] = t
            if ident == 1:
                provisional = (ns(meta["start_before"]) + ns(meta["start_after"])) // 2 - t
        elif name == "ReadyThread":
            tid = int(fields["Rdy TID"])
            if tid in inactive and inactive[tid]["ready"] is None:
                inactive[tid]["ready"] = t
                inactive[tid]["waker"] = {
                    "process": fields["Process Name ( PID)"], "tid": fields["ThreadID"],
                    "reason": fields["AdjustReason"],
                }
        else:
            old, new = int(fields["Old TID"]), int(fields["New TID"])
            if old in threads:
                prior = active.pop(old, None)
                if prior is not None and keep(prior, t):
                    running[old].append([prior, t])
                state = fields["OldState"]
                inactive[old] = {
                    "start": t, "state": state, "reason": fields["Wait Reason"],
                    "ready": t if state in ("Ready", "DeferredReady", "Standby") else None,
                }
            if new in threads:
                prior = inactive.pop(new, None)
                if prior is not None and keep(prior["start"], t):
                    prior["end"] = t
                    off_cpu[new].append(prior)
                active[new] = t
    assert proc.wait() == 0
assert set(anchors) == {1, 2}, anchors
bounds = [
    (ns(meta[prefix + "_before"]) - anchors[ident] - 1000,
     ns(meta[prefix + "_after"]) - anchors[ident] + 1000)
    for ident, prefix in ((1, "start"), (2, "end"))
]
lo, hi = max(v[0] for v in bounds), min(v[1] for v in bounds)
assert lo <= hi, bounds
offset = (lo + hi) // 2

def overlap(a, b, x, y):
    return max(0, min(b, y) - max(a, x))

def metrics(region):
    a, b, tid = region["start"], region["end"], region["tid"]
    cpu = sum(overlap(a, b, x + offset, y + offset) for x, y in running[tid])
    ready = blocked = unknown = 0
    states = defaultdict(int)
    waits = []
    for item in off_cpu[tid]:
        x, y = item["start"] + offset, item["end"] + offset
        duration = overlap(a, b, x, y)
        if not duration:
            continue
        states[item["state"] + "/" + item["reason"]] += duration
        if item["ready"] is None:
            unknown += duration
        else:
            r = item["ready"] + offset
            ready += overlap(a, b, r, y)
            blocked += overlap(a, b, x, r)
        if duration > 100_000:
            waits.append({**item, "overlap_ms": duration / 1e6})
    coverage = cpu + ready + blocked + unknown
    assert abs(coverage - (b - a)) < 10_000, (region, coverage)
    return {
        "start_ms": a / 1e6, "wall_ms": (b - a) / 1e6, "tid": tid,
        "on_cpu_ms": cpu / 1e6, "ready_delay_ms": ready / 1e6,
        "blocked_ms": blocked / 1e6, "unknown_off_ms": unknown / 1e6,
        "off_states_ms": {k: v / 1e6 for k, v in states.items()}, "long_off_intervals": waits,
    }

names = {0: "session", 7: "worker_roundtrip", 8: "worker_execution",
         11: "late_decoder", 13: "voice_batch", 17: "gpu_submit", 18: "gpu_wait", 19: "call"}
report = {
    "source": "CSwitch/ReadyThread from ETL; no SCHED_EVENTS inference",
    "clock": {"offset_ns": offset, "uncertainty_bound_ns": max(hi - offset, offset - lo),
              "anchors_ns": anchors, "bounds_ns": bounds},
    "calls": [
        {"benchmark_call": call, "regions": {
            name: [metrics(v) for v in ranges[call] if v["stage"] == stage]
            for stage, name in names.items()
        }}
        for call in sorted(wanted)
    ],
}
(out / "selected-cpu.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"selected_calls": len(wanted), "clock": report["clock"]}))
