"""Pair Pmc with its CSwitch and retain exact per-thread counter intervals."""
import argparse
import csv
import gzip
import json
import struct
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("--xperf", required=True)
args = parser.parse_args()
p = args.directory
with (p / "flight.bin").open("rb") as f:
    meta = json.loads(f.readline())
    payload = f.read()
freq = meta["frequency"]
q0 = meta["start_before"]
ns = lambda tick: (tick - q0) * 1_000_000_000 // freq
driver = {tid for _, _, tid, stage, _, _ in struct.iter_unpack("<QQIIII", payload) if stage == 8}
assert len(driver) == 1
driver = next(iter(driver))
main = meta["main_tid"]
workers = set(meta["workers"])
wanted = workers | {main}
headers = {}
inside = True
anchors = {}
offset = None
pending_pmc = None
last_pmc = {}
pair_counts = Counter()
corruption = []
target_spans = 0
totals = defaultdict(lambda: defaultdict(lambda: [0, 0, 0, 0, 0]))
period = 250_000_000

def role(tid):
    return "caller" if tid == main else "coordinator" if tid == driver else "other_workers"

with (p / "pmu-dump.log").open("w") as log, \
        gzip.open(p / "pmu-target.csv.gz", "wt", encoding="utf-8", newline="") as target_file:
    writer = csv.writer(target_file)
    writer.writerow(["start_etw_ns", "end_etw_ns", "tid", "cpu", "instructions", "core_cycles", "reference_cycles"])
    proc = subprocess.Popen(
        [args.xperf, "-i", str(p / "merged.etl"), "-a", "dumper"],
        stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8-sig", errors="replace",
    )
    for raw in csv.reader(proc.stdout, skipinitialspace=True):
        if not raw:
            continue
        row = [v.strip() for v in raw]
        name = row[0]
        if name == "EndHeader":
            inside = False
            continue
        if inside:
            if name in ("CSwitch", "Pmc", "PmcCorruptionStatus", "UnknownEvent/Crimson"):
                assert name not in headers or headers[name] == row
                headers[name] = row
            continue
        if name not in headers:
            continue
        if name == "PmcCorruptionStatus":
            corruption.append(row)
            continue
        assert len(row) == len(headers[name]), (name, len(row), len(headers[name]))
        fields = dict(zip(headers[name], row))
        t = int(fields["TimeStamp"]) * 1000
        if name == "UnknownEvent/Crimson":
            if "973c1dc0-11c4-4ae9-8f78-316c8d586f8a" in ",".join(row).lower():
                ident = int(fields["Id"], 16)
                anchors[ident] = t
                if ident == 1:
                    offset = (ns(meta["start_before"]) + ns(meta["start_after"])) // 2 - t
            continue
        if name == "Pmc":
            pair_counts["pmc_records"] += 1
            assert pending_pmc is None, ("unconsumed PMC", pending_pmc, row)
            pending_pmc = (t, int(fields["ThreadID"]), [
                int(fields[n]) for n in
                ("InstructionsRetiredFixed", "UnhaltedCoreCyclesFixed", "UnhaltedReferenceCyclesFixed")
            ])
            continue
        cpu, new, old = int(fields["CPU"]), int(fields["New TID"]), int(fields["Old TID"])
        if pending_pmc is None:
            pair_counts["switch_without_pmc"] += 1
            last_pmc.pop(cpu, None)
            continue
        assert (t, new) == pending_pmc[:2], ("ambiguous PMC/CSwitch", row, pending_pmc)
        values = pending_pmc[2]
        pending_pmc = None
        previous = last_pmc.get(cpu)
        if previous:
            a, owner, prior_values = previous
            if old == owner:
                delta = [b - a for a, b in zip(prior_values, values)]
                if all(v >= 0 for v in delta):
                    pair_counts["matched_monotonic_intervals"] += 1
                    if owner in wanted and offset is not None and t + offset >= 0:
                        writer.writerow([a, t, owner, cpu, *delta])
                        target_spans += 1
                        # Use wholly contained intervals; no proportional counter allocation.
                        bucket = (a + offset) // period
                        if a + offset >= 0 and (t + offset - 1) // period == bucket:
                            v = totals[bucket][role(owner)]
                            for i, d in enumerate(delta):
                                v[i] += d
                            v[3] += t - a
                            v[4] += 1
                else:
                    pair_counts["counter_decrease"] += 1
            else:
                pair_counts["missing_preceding_switch"] += 1
        last_pmc[cpu] = (t, new, values)
    assert proc.wait() == 0

assert set(anchors) == {1, 2}
bounds = [(ns(meta[prefix + "_before"]) - anchors[ident] - 1000,
           ns(meta[prefix + "_after"]) - anchors[ident] + 1000)
          for ident, prefix in ((1, "start"), (2, "end"))]
lo, hi = max(v[0] for v in bounds), min(v[1] for v in bounds)
assert lo <= hi
exact_offset = (lo + hi) // 2
report = {
    "source": "fixed hardware counters on CSwitch; matched by timestamp and incoming TID, then differenced per CPU",
    "clock": {"offset_ns": exact_offset, "uncertainty_bound_ns": max(exact_offset - lo, hi - exact_offset),
              "binning_provisional_offset_ns": offset},
    "threads": {"caller": main, "coordinator": driver, "workers": sorted(workers)},
    "pair_counts": dict(pair_counts), "corruption_status": corruption, "target_intervals": target_spans,
    "bins": [
        {"start_ms": b * period / 1e6, "roles": {
            r: {"instructions": v[0], "core_cycles": v[1], "reference_cycles": v[2],
                "on_cpu_ms": v[3] / 1e6, "intervals": v[4],
                "ipc": v[0] / v[1] if v[1] else None,
                "core_reference_ratio": v[1] / v[2] if v[2] else None,
                "core_ghz": v[1] / v[3] if v[3] else None,
                "reference_ghz": v[2] / v[3] if v[3] else None}
            for r, v in roles.items()
        }}
        for b, roles in sorted(totals.items())
    ],
}
(p / "pmu-summary.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"target_intervals": target_spans, "pair_counts": pair_counts, "clock": report["clock"]}), flush=True)
