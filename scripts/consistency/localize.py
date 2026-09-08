#!/usr/bin/env python3
"""Collect Release checkpoints and controlled RNG/worker experiments, without changing defaults."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--suffix", default="")
    args = parser.parse_args()
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any((out / name).exists() for name in ("fixtures", "kernels", "checkpoints", "repeats")):
        raise RuntimeError("use a fresh output directory; mixing diagnostic runs invalidates provenance")
    binary = args.bin_dir.resolve()
    tools = {name: binary / (name + args.suffix) for name in
             ("mradm", "mr_adm_pcm_bits", "mr_adm_make_fixture", "mr_adm_repeat_render", "mr_adm_numeric_probe")}
    for tool in tools.values():
        if not tool.is_file():
            raise RuntimeError(f"missing executable: {tool}")

    def run(tool, arguments, log, environment=None, allowed=(0,)):
        env = os.environ.copy()
        for key in ("MR_ADM_TRACE_DIR", "MR_ADM_DIAGNOSTIC_WORKERS", "MR_ADM_DIAGNOSTIC_GROUP_BUDGET"):
            env.pop(key, None)
        env.update(environment or {})
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run([str(tools[tool]), *map(str, arguments)], env=env,
                                    stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode not in allowed:
            raise RuntimeError(f"{tool} failed ({result.returncode}): {log.read_text(encoding='utf-8', errors='replace')}")
        return result.returncode

    def extract(wav):
        pcm = wav.with_suffix(".pcmbits")
        run("mr_adm_pcm_bits", ["extract", wav, pcm], wav.with_suffix(".extract.log"))
        wav.unlink()
        return pcm

    fixtures = out / "fixtures"
    fixtures.mkdir(exist_ok=True)
    for kind in ("objects-point", "objects-extent", "objects-extent-multi"):
        run("mr_adm_make_fixture", [kind, fixtures / (kind + ".wav")], fixtures / (kind + ".log"))
    run("mr_adm_numeric_probe", [], out / "kernels.log", {"MR_ADM_TRACE_DIR": str(out / "kernels")})

    cases = {
        "ear-extent": ("objects-extent", ["--renderer", "ear", "--output-layout", "5.1"]),
        "hoa-point": ("objects-point", ["--renderer", "hoa", "--output-layout", "hoa3"]),
        "binaural-point": ("objects-point", ["--renderer", "saf-binaural", "--output-layout", "binaural"]),
        "binaural-cloud": ("objects-extent", ["--renderer", "saf-binaural", "--output-layout", "binaural", "--binaural-spread-mode", "cloud"]),
    }
    for name, (kind, options) in cases.items():
        wav = out / (name + ".wav")
        run("mradm", ["render", "-i", fixtures / (kind + ".wav"), "-o", wav,
                      "--output-bit-depth", "f32", "--no-peak-limit", *options], out / (name + ".log"),
            {"MR_ADM_TRACE_DIR": str(out / "checkpoints" / name)})
        extract(wav)
    # A normal production build must not quietly produce an empty diagnostic artifact.
    for case, checkpoint in (("ear-extent", "ear.02-direct.f64"), ("hoa-point", "hoa.04-coefficients.f32"),
                             ("binaural-cloud", "cloud.02-grid-1.i32")):
        if not (out / "checkpoints" / case / checkpoint).is_file():
            raise RuntimeError("missing checkpoint; configure with MR_ADM_CONSISTENCY_DIAGNOSTICS=ON")

    comparisons = []

    def compare(name, first, second, require_identical=False):
        code = run("mr_adm_pcm_bits", ["compare", first, second], out / "comparisons" / (name + ".txt"),
                   allowed=(0,) if require_identical else (0, 1))
        comparisons.append({"name": name, "identical": code == 0, "required": require_identical})
        print(name, "identical" if code == 0 else "different", flush=True)

    def repeat(name, kind, options, workers=1, groups=4, required=False):
        prefix = out / "repeats" / name
        prefix.parent.mkdir(exist_ok=True)
        run("mr_adm_repeat_render", [fixtures / (kind + ".wav"), prefix, *options], prefix.with_suffix(".log"),
            {"MR_ADM_DIAGNOSTIC_WORKERS": str(workers), "MR_ADM_DIAGNOSTIC_GROUP_BUDGET": str(groups)})
        # Prove the requested count reached the real pools; merely recording a request is insufficient.
        diagnostic = f"diagnostic pools: ola_workers={workers} spreader_workers={workers}"
        if diagnostic not in prefix.with_suffix(".log").read_text(encoding="utf-8", errors="replace"):
            raise RuntimeError("worker override did not reach the renderer")
        first, second = (extract(Path(str(prefix) + f"-{p}.wav")) for p in (1, 2))
        compare(name, first, second, required)
        return first

    for mode, kind, options in (("spreader", "objects-extent", []),
                                ("spreader-multi", "objects-extent-multi", []),
                                ("cloud", "objects-extent", ["--cloud"])):
        repeat(mode + "-continuous", kind, options)
        repeat(mode + "-reset", kind, [*options, "--reset-rng"], required=True)

    first = out / "repeats" / "spreader-multi-reset-1.pcmbits"
    for workers in (2, 4):
        pcm = repeat(f"workers-{workers}-groups-4", "objects-extent-multi", ["--reset-rng"], workers=workers, required=True)
        compare(f"workers-1-vs-{workers}-fixed-groups", first, pcm, require_identical=True)
    for groups in (1, 2):
        pcm = repeat(f"workers-1-groups-{groups}", "objects-extent-multi", ["--reset-rng"], groups=groups, required=True)
        compare(f"group-budget-4-vs-{groups}-fixed-worker", first, pcm)

    (out / "experiments.json").write_text(json.dumps(comparisons, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
