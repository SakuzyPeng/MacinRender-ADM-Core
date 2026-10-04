#!/usr/bin/env python3
"""Run the opt-in meter evaluation using the repository's shared Cargo cache."""

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import statistics
import subprocess


def main():
    repo_root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-lib-dir", type=Path, required=True)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--target-dir", type=Path)
    parser.add_argument("--reuse-results", action="store_true", help="Only summarize existing JSONL files")
    parser.add_argument("--output-prefix", type=Path, default=repo_root / "out/ebur128-eval")
    args = parser.parse_args()
    prefix = args.output_prefix.resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    reference_dir = args.reference_lib_dir.resolve()
    archives = [reference_dir / name for name in ("libebur128.a", "ebur128.lib")]
    archive = next((path for path in archives if path.is_file()), None)
    if archive is None:
        parser.error("No existing libebur128 static archive in --reference-lib-dir")

    def command(argv, **kwargs):
        return subprocess.check_output(argv, cwd=repo_root / "rust", text=True, **kwargs).strip()

    cargo = shutil.which(args.cargo)
    if cargo is None:
        parser.error("Cargo not found")
    rustc = str(Path(cargo).with_name("rustc.exe" if os.name == "nt" else "rustc"))
    rust_version = command([rustc, "-Vv"])
    target = next(line[6:] for line in rust_version.splitlines() if line.startswith("host: "))
    # Same normalized manifest-path hash as cmake/MRRust.cmake.
    manifest_hash = hashlib.sha1((repo_root / "rust/Cargo.toml").as_posix().encode()).hexdigest()[:5]
    target_dir = args.target_dir or repo_root / f"build/rust/cargo/rust_{manifest_hash}"
    manifest = Path(__file__).with_name("Cargo.toml").resolve()
    env = dict(os.environ, EBUR128_REFERENCE_LIB_DIR=str(reference_dir))
    if not args.reuse_results:
        for name, release, action in [("debug", False, "verify"), ("release", True, "verify"),
                                      ("bench", True, "bench")]:
            argv = [cargo, "run", "--locked", "--manifest-path", str(manifest),
                    "--target-dir", str(target_dir), "--target", target]
            if release:
                argv.append("--release")
            argv += ["--", action]
            with Path(f"{prefix}-{name}.jsonl").open("w", encoding="utf-8") as output:
                subprocess.run(argv, cwd=repo_root / "rust", env=env, stdout=output, check=True)

    def rows(name):
        return [json.loads(line) for line in Path(f"{prefix}-{name}.jsonl").read_text().splitlines()]

    debug, release, bench = rows("debug"), rows("release"), rows("bench")
    for verification in (debug, release):
        expected_counts = {"comparison": 49, "analytic_checks": 1, "state_checks": 1,
                           "long_monitor": 2, "prepared_processing": 4, "unused_api_probe": 1}
        for kind, count in expected_counts.items():
            assert sum(row["kind"] == kind for row in verification) == count, f"Incomplete {kind} results"
        assert all(row.get("passed", True) for row in verification)
    assert len(bench) == 60
    summaries = []
    for key in sorted({(row["name"], row["channels"], row["mapping"]) for row in bench}):
        group = [row for row in bench if (row["name"], row["channels"], row["mapping"]) == key]
        rust_us = statistics.median(row["rust_us"] for row in group)
        c_us = statistics.median(row["c_us"] for row in group)
        summaries.append(dict(name=key[0], channels=key[1], mapping=key[2], rust_median_us=rust_us,
                              c_median_us=c_us, rust_over_c=rust_us / c_us))
    metadata = json.loads(command([cargo, "metadata", "--locked", "--format-version", "1",
                                   "--manifest-path", str(manifest)]))
    cases = [row for row in release if row["kind"] == "comparison"]
    source_files = [manifest, manifest.with_name("Cargo.lock"), manifest.with_name("build.rs")]
    source_files.append(Path(__file__).resolve())
    source_files += sorted(manifest.parent.glob("src/*.rs"))
    cpu = command(["sysctl", "-n", "machdep.cpu.brand_string"]) if platform.system() == "Darwin" else platform.processor()
    report = {
        "scope": "Suitability evaluation only; production integration and EBU certification not performed",
        "base_commit": command(["git", "rev-parse", "HEAD"]),
        "system": platform.system(), "system_release": platform.release(),
        "architecture": platform.machine(), "cpu": cpu, "rustc": rust_version,
        "rustflags": os.environ.get("RUSTFLAGS", ""),
        "reference": {"version": "1.2.6", "profile": "Release", "archive": archive.name,
                      "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()},
        "source_sha256": {str(path.relative_to(repo_root)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in source_files},
        "dependencies": [{"name": pkg["name"], "version": pkg["version"], "license": pkg["license"]}
                         for pkg in metadata["packages"] if pkg["source"] is not None],
        "selected_ebur128_features": [],
        "debug_comparison_cases_passed": len(cases),
        "release_summary": {
            "comparison_cases_passed": len(cases),
            "max_lufs_error": max(row["max_lufs_error"] for row in cases),
            "max_true_peak_db_error": max(row["max_true_peak_db_error"] for row in cases),
            "numerical_silence_floor_lufs": -300,
            "numerical_silence_differences": sum(row["numerical_silence_differences"] for row in cases),
        },
        "release_verification": release,
        "benchmark_scope": "5 s synthetic PCM, 48 kHz, 512-frame blocks; processing and queries only; "
                           "initialization/destruction excluded; 5 alternating runs after warmup",
        "benchmark_summary": summaries,
        "benchmark_runs": bench,
    }
    destination = Path(f"{prefix}-report.json")
    destination.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(destination)


if __name__ == "__main__":
    main()
