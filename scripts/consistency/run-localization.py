#!/usr/bin/env python3
"""Run diagnostic variants after recording the unmodified A/B baseline build."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--suffix", default="")
    parser.add_argument("--vbap-only", action="store_true", help="run only the focused 3D VBAP attribution experiment")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    variants = [("native", False, []), ("portable-rng", True, [])]
    if sys.platform == "darwin":
        prefix = subprocess.check_output(["brew", "--prefix", "openblas"], text=True).strip()
        blas = ["-DSAF_PERFORMANCE_LIB=SAF_USE_OPEN_BLAS_AND_LAPACKE",
                f"-DOPENBLAS_LIBRARY={prefix}/lib/libopenblas.dylib",
                f"-DLAPACKE_LIBRARY={prefix}/lib/libopenblas.dylib",
                f"-DOPENBLAS_HEADER_PATH={prefix}/include", f"-DLAPACKE_HEADER_PATH={prefix}/include"]
        variants.extend([("openblas", False, blas), ("openblas-portable-rng", True, blas)])
    for name, portable, extra in variants:
        destination = out / name
        destination.mkdir(exist_ok=True)
        targets = ["mradm_exe", "mr_adm_pcm_bits", "mr_adm_make_fixture", "mr_adm_vbap_probe"]
        if not args.vbap_only:
            targets.extend(["mr_adm_repeat_render", "mr_adm_numeric_probe", "mr_adm_fft_lifecycle_probe"])
        commands = [
            ["cmake", "-S", str(root), "-B", str(build), "-DMR_ADM_CONSISTENCY_DIAGNOSTICS=ON",
             "-DMR_ADM_DIAGNOSTIC_PORTABLE_RNG=" + ("ON" if portable else "OFF"), *extra],
            ["cmake", "--build", str(build), "--target", *targets],
        ]
        if not args.vbap_only:
            commands.extend([
                [sys.executable, str(root / "scripts/consistency/localize.py"), str(build), str(destination),
                 "--suffix=" + args.suffix],
                [str(build / ("mr_adm_fft_lifecycle_probe" + args.suffix)),
                 str(destination / "checkpoints/binaural-point/binaural.01-hrir.f32")],
            ])
        commands.extend([
            [sys.executable, str(root / "scripts/consistency/localize-vbap.py"), str(build), str(destination / "vbap"),
             "--baseline", str(out.parent), "--suffix=" + args.suffix],
            [sys.executable, str(root / "scripts/consistency/build_info.py"), str(build), str(destination / "build-info.txt")],
        ])
        for index, command in enumerate(commands):
            print(name, command[0], flush=True)
            with (destination / f"step-{index}.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f"{name} step {index} failed; see {destination / f'step-{index}.log'}")


if __name__ == "__main__":
    main()
