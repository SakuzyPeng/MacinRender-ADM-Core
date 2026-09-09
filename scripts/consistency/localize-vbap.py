#!/usr/bin/env python3
"""Capture actual 3D VBAP inputs, verify stage replays, and measure RNG sensitivity."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--suffix", default="")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    binary = args.bin_dir.resolve()
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any((out / name).exists() for name in ("fixtures", "checkpoints", "kernels", "pcm")):
        raise RuntimeError("use a fresh output directory")
    for name in ("fixtures", "checkpoints", "kernels", "pcm"):
        (out / name).mkdir()
    spec = importlib.util.spec_from_file_location("checkpoints", Path(__file__).with_name("compare-checkpoints.py"))
    checkpoints = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checkpoints)

    def run(tool, arguments, log, trace=None, allowed=(0,)):
        env = os.environ.copy()
        for key in ("MR_ADM_TRACE_DIR", "MR_ADM_DIAGNOSTIC_WORKERS", "MR_ADM_DIAGNOSTIC_GROUP_BUDGET"):
            env.pop(key, None)
        if trace is not None:
            env["MR_ADM_TRACE_DIR"] = str(trace)
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run([str(binary / (tool + args.suffix)), *map(str, arguments)],
                                    env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode not in allowed:
            raise RuntimeError(f"{tool} failed: {log.read_text(encoding='utf-8', errors='replace')}")
        return result.returncode

    results = []
    for case, kind in (("saf-5_1_4-extent", "objects-extent"),
                       ("saf-5_1_4-cartesian", "objects-cartesian")):
        fixture = out / "fixtures" / (kind + ".wav")
        run("mr_adm_make_fixture", [kind, fixture], out / (kind + ".log"))
        if args.baseline is not None:
            original = args.baseline / "fixtures" / fixture.name
            if fixture.read_bytes() != original.read_bytes():
                raise RuntimeError(f"baseline fixture differs: {kind}")
        trace = out / "checkpoints" / case
        wav = out / (case + ".wav")
        pcm = out / "pcm" / (case + ".pcmbits")
        run("mradm", ["render", "-i", fixture, "-o", wav, "--renderer", "saf", "--output-layout", "5.1.4",
                      "--output-bit-depth", "f32", "--no-peak-limit"], out / (case + ".log"), trace)
        run("mr_adm_pcm_bits", ["extract", wav, pcm], out / (case + ".extract.log"))
        wav.unlink()
        for checkpoint in ("vbap.01-source.f32", "vbap.02-speakers.f32", "vbap.03-gains.f32"):
            if not (trace / checkpoint).is_file():
                raise RuntimeError("missing VBAP checkpoint; configure MR_ADM_CONSISTENCY_DIAGNOSTICS=ON")
        row = {"case": case, "input_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest()}
        if args.baseline is not None:
            code = run("mr_adm_pcm_bits", ["compare", args.baseline / "pcm" / pcm.name, pcm],
                       out / (case + ".baseline-comparison.txt"), allowed=(0, 1))
            row["unmodified_baseline_identical"] = code == 0
        for name, seed in (("seed-1", 1), ("seed-2", 2), ("seed-17", 17), ("seed-1-repeat", 1)):
            run("mr_adm_vbap_probe", [trace, seed], out / (case + "-" + name + ".log"),
                out / "kernels" / case / name)
        first = out / "kernels" / case / "seed-1"
        repeated = checkpoints.compare(first, out / "kernels" / case / "seed-1-repeat")
        if any(item["status"] != "identical" for item in repeated):
            raise RuntimeError(f"fixed-seed replay changed within the same build: {case}")
        row["fixed_seed_repeat_identical"] = True
        row["seed_1_vs_2"] = checkpoints.compare(first, out / "kernels" / case / "seed-2")
        row["seed_1_vs_17"] = checkpoints.compare(first, out / "kernels" / case / "seed-17")
        results.append(row)
        print(case, "stage checks passed", flush=True)
    (out / "experiments.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
