"""Reconstruct CPU state and interrupt overlap in the two selected PMU windows."""
import argparse
import csv
import gzip
import json
import struct
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
p = parser.parse_args().directory
pmu = json.loads((p / "pmu-summary.json").read_text())
offset = pmu["clock"]["offset_ns"]
with (p / "flight.bin").open("rb") as f:
    meta = json.loads(f.readline())
    payload = f.read()
wanted = set(meta["workers"] + [meta["main_tid"]])
stacks = defaultdict(list)
ranges = defaultdict(list)
for tick, call, tid, stage, begin, _ in struct.iter_unpack("<QQIIII", payload):
    if call not in (16973, 19649):
        continue
    if begin:
        stacks[tid].append((tick, call, stage))
    else:
        start, cid, kind = stacks[tid].pop()
        assert (cid, kind) == (call, stage)
        ranges[call].append({
            "a": (start - meta["start_before"]) * 1_000_000_000 // meta["frequency"],
            "b": (tick - meta["start_before"]) * 1_000_000_000 // meta["frequency"],
            "tid": tid, "stage": stage,
        })

def overlap(a, b, x, y):
    return max(0, min(b, y) - max(a, x))

def union(values):
    result = []
    for a, b in sorted(values):
        if result and a <= result[-1][1]:
            result[-1][1] = max(result[-1][1], b)
        else:
            result.append([a, b])
    return result

results = []
for call in (16973, 19649):
    headers = {}
    inside = True
    active = {}
    inactive = {}
    running = defaultdict(list)
    off_cpu = defaultdict(list)
    interrupts = defaultdict(list)
    with gzip.open(p / f"focus-{call}.csv.gz", "rt", encoding="utf-8-sig", errors="replace") as f:
        for raw in csv.reader(f, skipinitialspace=True):
            if not raw:
                continue
            row = [v.strip() for v in raw]
            name = row[0]
            if name == "EndHeader":
                inside = False
                continue
            if inside:
                if name in ("CSwitch", "ReadyThread", "DPC", "DPCTmr", "Interrupt"):
                    assert name not in headers or headers[name] == row
                    headers[name] = row
                continue
            if name not in headers:
                continue
            assert len(row) == len(headers[name])
            v = dict(zip(headers[name], row))
            t = int(v["TimeStamp"]) * 1000 + offset
            if name in ("DPC", "DPCTmr", "Interrupt"):
                interrupts[int(v["CPU"])].append([t - int(v["ElapsedTime"]) * 1000, t])
            elif name == "ReadyThread":
                tid = int(v["Rdy TID"])
                if tid in inactive and inactive[tid]["ready"] is None:
                    inactive[tid]["ready"] = t
            else:
                old, new, cpu = int(v["Old TID"]), int(v["New TID"]), int(v["CPU"])
                if old in wanted:
                    prior = active.pop(old, None)
                    if prior:
                        a, cp = prior
                        assert cpu == cp
                        running[old].append([a, t, cpu])
                    state = v["OldState"]
                    inactive[old] = {
                        "start": t, "state": state, "reason": v["Wait Reason"],
                        "ready": t if state in ("Ready", "Standby", "DeferredReady") else None,
                        "priority": v["OPri"], "qos": v["OldThrdBamQosLevel"],
                    }
                if new in wanted:
                    prior = inactive.pop(new, None)
                    if prior:
                        prior["end"] = t
                        off_cpu[new].append(prior)
                    active[new] = (t, cpu)
    interrupts = {cpu: union(values) for cpu, values in interrupts.items()}

    def metrics(region):
        a, b, tid = region["a"], region["b"], region["tid"]
        on_cpu = irq = ready = blocked = unknown = 0
        states = defaultdict(int)
        cpus = defaultdict(int)
        priorities = set()
        qos = set()
        for x, y, cpu in running[tid]:
            duration = overlap(a, b, x, y)
            on_cpu += duration
            cpus[cpu] += duration
            if duration:
                for q, w in interrupts.get(cpu, []):
                    irq += overlap(max(a, x), min(b, y), q, w)
        for item in off_cpu[tid]:
            x, y = item["start"], item["end"]
            duration = overlap(a, b, x, y)
            if not duration:
                continue
            priorities.add(item["priority"])
            qos.add(item["qos"])
            states[item["state"] + "/" + item["reason"]] += duration
            if item["ready"] is None:
                unknown += duration
            else:
                ready += overlap(a, b, item["ready"], y)
                blocked += overlap(a, b, x, item["ready"])
        assert abs(on_cpu + ready + blocked + unknown - (b - a)) < 10000
        return {
            "wall_ms": (b - a) / 1e6, "on_cpu_ms": on_cpu / 1e6,
            "recorded_interrupt_overlap_ms": irq / 1e6, "ready_ms": ready / 1e6,
            "blocked_ms": blocked / 1e6, "unknown_off_ms": unknown / 1e6,
            "off_states_ms": {k: v / 1e6 for k, v in states.items()},
            "switch_out_priorities": sorted(priorities), "switch_out_qos": sorted(qos),
            "cpu_ms": {str(k): v / 1e6 for k, v in cpus.items() if v},
        }

    names = {19: "call", 8: "worker_execution", 13: "voice_batch", 11: "late_decoder",
             17: "gpu_submit", 18: "gpu_wait"}
    results.append({"call": call, "regions": {
        name: [metrics(v) for v in ranges[call] if v["stage"] == stage]
        for stage, name in names.items()
    }})
(p / "selected-cpu.json").write_text(json.dumps({"calls": results, "clock": pmu["clock"]}, indent=2) + "\n")
print("Wrote selected-cpu.json")
