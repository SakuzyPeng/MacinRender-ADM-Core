#!/usr/bin/env python3
"""Validate a fixed generic DVF against a labelled multi-distance collection.

No parameters are fitted to the collection. Virtual rows and inherited temporal
review directions are excluded. Mesh2HRTF TF files are explicitly conjugated to
the NumPy FFT convention, and cross-checked against their corresponding HRIRs.
"""
import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path

import h5py
import numpy as np

from dvf import HeadModel, Synthesizer, spherical_directions


BANDS = {"100-2000": (100, 2000), "2100-8000": (2100, 8000),
         "8100-16000": (8100, 16000), "16100-20000": (16100, 20000)}


def stats(values):
    return dict(median=float(np.median(values)), p95=float(np.percentile(values, 95)),
                maximum=float(np.max(values)))


def read_layer(root, layer):
    path = root/layer["files"]["HRTF_RAW"]["path"]
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != layer["files"]["HRTF_RAW"]["sha256"]:
        raise ValueError(f"SHA-256 mismatch: {path}")
    with h5py.File(path, "r") as file:
        h = (file["Data.Real"][...] + 1j*file["Data.Imag"][...]).conj()
        positions, f = file["SourcePosition"][...], file["N"][...]
    ir_path = root/layer["files"]["HRIR_RAW"]["path"]
    ir_digest = hashlib.sha256(ir_path.read_bytes()).hexdigest()
    if ir_digest != layer["files"]["HRIR_RAW"]["sha256"]:
        raise ValueError(f"SHA-256 mismatch: {ir_path}")
    with h5py.File(ir_path, "r") as file:
        ir = file["Data.IR"][...]
        if np.any(file["Data.Delay"][...] != 0) or file["Data.SamplingRate"][0] != 48000:
            raise ValueError("Validation expects the documented 48 kHz / zero Data.Delay collection")
    # Mesh2HRTF applies the documented common 42-sample shift and forces the
    # Nyquist bin real in the FIR export. Compare all solved non-Nyquist bins.
    fft = np.fft.rfft(np.roll(ir, -42, axis=-1), axis=-1)[..., 1:]
    if not np.allclose(np.fft.rfftfreq(ir.shape[-1], 1/48000)[1:], f, rtol=0, atol=1e-8):
        raise ValueError("TF and FIR frequency grids differ")
    phase_check = float(np.max(abs(fft[..., :-1]-h[..., :-1]))/np.max(abs(h[..., :-1])))
    if phase_check > 1e-10:
        raise ValueError("TF/FIR phase convention check failed")
    return h, positions, f, dict(hrtf_sha256=digest, hrir_sha256=ir_digest, fft_relative_error=phase_check)


def metrics(prediction, truth, f, mask):
    error = 20*np.log10(np.maximum(abs(prediction[mask]), 1e-12)/np.maximum(abs(truth[mask]), 1e-12))
    ild = error[:, 0] - error[:, 1]
    bands = {}
    for label, (lo, hi) in BANDS.items():
        selected = (f >= lo) & (f <= hi)
        bands[label] = dict(spectral_rms_db=stats(np.sqrt(np.mean(error[..., selected]**2, axis=-1))),
                            ild_rms_db=stats(np.sqrt(np.mean(ild[..., selected]**2, axis=-1))))
    selected = (f >= 200) & (f <= 1500)
    phase = np.unwrap(np.angle(prediction[mask]*truth[mask].conj()), axis=-1)
    x = f[selected] - np.mean(f[selected])
    delays = -(phase[..., selected] @ x)/(np.sum(x*x)*2*np.pi)
    itd_error = abs(delays[:, 1]-delays[:, 0])*1e6
    return dict(directions=int(mask.sum()), bands=bands, phase_slope_itd_error_us=stats(itd_error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--head-radius", type=float, default=.0875)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    index = json.loads((args.collection/"distance_index.json").read_text())
    provenance = json.loads((args.collection/"provenance/point_provenance.json").read_text())
    quality = json.loads((args.collection/"provenance/source_nearfield_quality_report.json").read_text())
    reference = next(layer for layer in index["layers"] if layer["reference_layer"])
    h0, positions0, f, source_audit = read_layer(args.collection, reference)
    directions = spherical_directions(positions0[:, :2])
    model = HeadModel(radius_m=args.head_radius)
    synthesizer = Synthesizer(f, directions, h0, reference["radius_m"], model)
    exclusions = {layer["radius_m"]: set() for layer in index["layers"]}
    for point in provenance["points"]:
        if "dvf" in point["method"]:
            exclusions[point["radius_m"]].add(point["direction_index"])
    for layer in quality["radii"]:
        for row in layer["inherited_temporal_review_rows"]:
            exclusions[layer["radius_m"]].add(row["direction_index"])
    report = dict(schema="mradm.reusable-dvf-validation", version=1, model=asdict(model),
                  personal_coefficients_fitted=False, hybrid_transition_hz=[2000, 4000],
                  algorithm_selection_note="Hybrid chosen after inspecting this collection; this is development-set evidence, not an unseen-listener test.",
                  reference_radius_m=reference["radius_m"], reference_audit=source_audit,
                  scope="One personal BEM model; not a cross-listener or perceptual validation. Equal weight per direction/ear and per 100-Hz frequency bin. Phase-slope ITD uses 200..1500 Hz and is not an onset-time measurement.",
                  layers=[])
    for layer in index["layers"]:
        radius = layer["radius_m"]
        truth, positions, current_f, audit = read_layer(args.collection, layer)
        if not np.array_equal(current_f, f) or not np.allclose(spherical_directions(positions[:, :2]), directions, atol=1e-10):
            raise ValueError("Collection layers have inconsistent grids")
        valid = np.ones(len(directions), bool)
        valid[list(exclusions[radius])] = False
        predictions = {"unchanged_reference": h0, "sphere": synthesizer.at(radius, "sphere"),
                       "parallax": synthesizer.at(radius, "parallax"), "hybrid": synthesizer.at(radius, "hybrid")}
        scopes = {"all_valid": valid, "elevation_ge_minus30": valid & (positions[:, 1] >= -30.000001)}
        results = {scope: {name: metrics(prediction, truth, f, mask) for name, prediction in predictions.items()}
                   for scope, mask in scopes.items()}
        row = dict(radius_m=radius, excluded_direction_count=int((~valid).sum()), audit=audit, metrics=results)
        report["layers"].append(row)
        print(json.dumps({"radius_m": radius, "low_band_median_db": {
            name: value["bands"]["100-2000"]["spectral_rms_db"]["median"]
            for name, value in results["all_valid"].items()}}, ensure_ascii=False), flush=True)
    report["passed_numerical_audit"] = True
    (args.output/"report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)+"\n")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.2), constrained_layout=True)
    radii = [row["radius_m"] for row in report["layers"]]
    for ax, band in zip(axes, ["100-2000", "2100-8000", "8100-16000"]):
        for name, label, color in [("unchanged_reference", "Unchanged 1.5 m", "#64748b"),
                                   ("sphere", "Sphere DVF", "#cc8540"), ("hybrid", "Hybrid DVF", "#137c8b")]:
            values = [row["metrics"]["all_valid"][name]["bands"][band]["spectral_rms_db"]["median"]
                      for row in report["layers"]]
            ax.plot(radii, values, "o-", label=label, color=color)
        ax.set(title=band+" Hz", xlabel="Distance (m)", ylabel="Median spectral RMS error (dB)")
        ax.grid(alpha=.2)
        ax.spines[["right", "top"]].set_visible(False)
    axes[0].legend(frameon=False, fontsize=9)
    fig.suptitle(f"Generic DVF development validation | {model.radius_m*1000:g} mm sphere | synthetic rows excluded")
    fig.savefig(args.output/"validation.png", dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    main()
