#!/usr/bin/env python3
"""Reusable, head-centre-normalized rigid-sphere distance variation functions.

The NumPy-only analytic model contains no personal HRTF coefficients. All complex
responses use the NumPy FFT convention: a positive delay has negative phase.
The optional parallax synthesizer uses the input listener's spectral residual.
This is a preparation/research API, not a realtime rendering implementation.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import importlib.util
import json
from pathlib import Path

import numpy as np


def finite(value, name):
    result = np.asarray(value)
    if result.dtype.kind not in "fiu" or not np.isfinite(result).all():
        raise ValueError(f"{name} must contain finite real numbers")
    return result.astype(np.float64, copy=False)


def unit_vectors(value):
    vectors = finite(value, "directions")
    if vectors.ndim != 2 or vectors.shape[1] != 3 or len(vectors) == 0:
        raise ValueError("Directions must be a nonempty M x 3 array")
    lengths = np.linalg.norm(vectors, axis=1)
    if not np.allclose(lengths, 1, rtol=0, atol=1e-8):
        raise ValueError("Directions must be unit vectors")
    return vectors / lengths[:, None]


def spherical_directions(azimuth_elevation):
    angles = finite(azimuth_elevation, "angles")
    if angles.ndim != 2 or angles.shape[1] != 2 or np.any(abs(angles[:, 1]) > 90):
        raise ValueError("Angles must be M x 2, azimuth/elevation in degrees")
    az, el = np.radians(angles).T
    return np.column_stack((np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)))


def frequencies(value):
    f = finite(value, "frequencies")
    if f.ndim != 1 or len(f) == 0 or np.any(f < 0) or np.any(f > 48000) or np.any(np.diff(f) <= 0):
        raise ValueError("Frequencies must increase strictly within 0..48000 Hz")
    return f


@dataclass(frozen=True)
class HeadModel:
    radius_m: float = .0875
    sound_speed_m_s: float = 343.18
    left_azimuth_deg: float = 90.
    left_elevation_deg: float = 0.
    right_azimuth_deg: float = -90.
    right_elevation_deg: float = 0.

    def __post_init__(self):
        values = finite(list(asdict(self).values()), "head parameters")
        if not .04 <= self.radius_m <= .12 or not 300 <= self.sound_speed_m_s <= 380:
            raise ValueError("Head radius must be 0.04..0.12 m; sound speed 300..380 m/s")
        if np.any(abs(values[[3, 5]]) > 90):
            raise ValueError("Ear elevations must be within -90..90 degrees")
        ears = self.ears
        if ears[0, 1] <= 0 or ears[1, 1] >= 0:
            raise ValueError("Left/right ears must lie on the positive/negative y sides")

    @property
    def ears(self):
        return spherical_directions([[self.left_azimuth_deg, self.left_elevation_deg],
                                     [self.right_azimuth_deg, self.right_elevation_deg]])

    def check_distance(self, distance_m):
        distance = float(distance_m)
        if not np.isfinite(distance) or not max(.15, 1.25 * self.radius_m) <= distance <= 100:
            raise ValueError("Distance must be max(0.15 m, 1.25 head radii)..100 m from the head centre")
        return distance


def sphere_response(frequency_hz, directions, distance_m, model=HeadModel(), *, extra_order=0):
    """Return normalized complex surface pressure, shape M x 2 x F.

    The Neumann partial-wave solution is evaluated using scaled Hankel
    polynomials. For x=ka and q=a/r, s0=1, s1=1+ix and u0=1, u1=q+ix:
      s[n+1] = s[n] - x^2 s[n-1]/((2n-1)(2n+1))
      u[n+1] = q u[n] - x^2 u[n-1]/((2n-1)(2n+1))
      c[n] = -(2n+1) exp(ix) u[n] / (n s[n]-(2n+1) s[n+1]).
    H=sum c[n] P[n](cos(theta)). This avoids singular intermediate Bessel
    functions at DC and includes the static finite-distance limit directly.
    Common 1/r gain and r/c propagation have already been divided out.
    """
    f, directions = frequencies(frequency_hz), unit_vectors(directions)
    distance = model.check_distance(distance_m)
    if not isinstance(extra_order, int) or not 0 <= extra_order <= 64:
        raise ValueError("extra_order must be an integer within 0..64")
    x = 2 * np.pi * f * model.radius_m / model.sound_speed_m_s
    q = model.radius_m / distance
    order = max(int(np.ceil(x.max() + 4 * np.cbrt(x.max()) + 32)),
                int(np.ceil(np.log(1e-14 * (1 - q)) / np.log(q)))) + extra_order
    s0, s1 = np.ones(len(f), complex), 1 + 1j * x
    u0, u1 = s0.copy(), q + 1j * x
    coefficients = np.empty((order + 1, len(f)), complex)
    phase = np.exp(1j * x)
    for n in range(order + 1):
        coefficients[n] = -(2*n + 1) * phase * u0 / (n*s0 - (2*n + 1)*s1)
        factor = x*x / ((2*n + 1) * (2*n + 3))
        s0, s1 = s1, s1 - factor*s0
        u0, u1 = u1, q*u1 - factor*u0
    cosines = np.clip(directions @ model.ears.T, -1, 1)
    result = np.polynomial.legendre.legval(cosines, coefficients).transpose(1, 2, 0)
    if not np.isfinite(result).all() or np.min(abs(result)) < 1e-10:
        raise ValueError("Unstable sphere response within the requested parameters")
    return result


def distance_variation(frequency_hz, directions, target_distance_m, reference_distance_m=1.5,
                       model=HeadModel()):
    """Complex DVF, M x 2 x F; unity when target equals reference distance."""
    near = sphere_response(frequency_hz, directions, target_distance_m, model)
    far = sphere_response(frequency_hz, directions, reference_distance_m, model)
    if float(target_distance_m) == float(reference_distance_m):
        return np.ones_like(near)
    return near / far


def parallax_directions(directions, target_distance_m, reference_distance_m, model=HeadModel()):
    """M x 2 x 3 reference directions with the same ear-relative incident ray.

    The finite reference radius is respected; 1.5 m is not treated as infinity.
    """
    directions = unit_vectors(directions)
    target, reference = model.check_distance(target_distance_m), model.check_distance(reference_distance_m)
    ear = model.radius_m * model.ears
    rays = target * directions[:, None, :] - ear[None, :, :]
    rays /= np.linalg.norm(rays, axis=-1, keepdims=True)
    projected = np.einsum("mec,ec->me", rays, ear)
    length = -projected + np.sqrt(projected**2 + reference**2 - model.radius_m**2)
    return (ear[None, :, :] + length[:, :, None] * rays) / reference


class DirectionMesh:
    """Closed spherical triangulation with local, phase-aligned interpolation."""

    def __init__(self, directions):
        from scipy.spatial import ConvexHull, QhullError
        self.directions = unit_vectors(directions)
        if len(np.unique(np.round(self.directions, 11), axis=0)) != len(self.directions):
            raise ValueError("Duplicate directions are unsupported")
        try:
            hull = ConvexHull(self.directions)
        except QhullError as error:
            raise ValueError("Directions must cover a full sphere") from error
        if np.any(hull.equations[:, -1] >= -1e-8):
            raise ValueError("Directions must enclose the head centre; no hemisphere extrapolation")
        self.faces = np.sort(hull.simplices, axis=1)
        self.faces = self.faces[np.lexsort(self.faces.T[::-1])]
        self.inverse = np.linalg.inv(self.directions[self.faces].transpose(0, 2, 1))

    def weights(self, queries):
        queries = unit_vectors(queries)
        indices, gains = [], []
        # Bound temporary allocation independently of the size of the source grid.
        chunk = max(1, min(128, 1_000_000 // (len(self.faces) * 3)))
        for begin in range(0, len(queries), chunk):
            values = np.einsum("tij,qj->qti", self.inverse, queries[begin:begin + chunk])
            inside = np.min(values, axis=-1) >= -1e-10
            if not np.all(np.any(inside, axis=-1)):
                raise ValueError("Query falls outside the triangulated directions")
            face = np.argmax(inside, axis=1)
            weight = np.maximum(values[np.arange(len(face)), face], 0)
            weight /= weight.sum(axis=1, keepdims=True)
            indices.append(self.faces[face])
            gains.append(weight)
        return np.concatenate(indices), np.concatenate(gains)

    def interpolate(self, data, queries):
        data = np.asarray(data, dtype=complex)
        if data.ndim != 2 or len(data) != len(self.directions) or not np.isfinite(data).all():
            raise ValueError("Interpolation data must be finite M x F complex spectra")
        indices, weights = self.weights(queries)
        values = data[indices]
        anchor = values[np.arange(len(values)), np.argmax(weights, axis=1)]
        log_magnitude = np.log(np.maximum(abs(values), 1e-15))
        phase_difference = np.unwrap(np.angle(values * anchor[:, None, :].conj()), axis=-1)
        result = np.exp(np.sum(weights[:, :, None] * log_magnitude, axis=1)
                        + 1j * (np.angle(anchor) + np.sum(weights[:, :, None] * phase_difference, axis=1)))
        exact = np.max(weights, axis=1) >= 1 - 1e-12
        result[exact] = anchor[exact]
        silent = np.all((abs(values) == 0) | (weights[:, :, None] == 0), axis=1)
        result[silent] = 0
        return result


class Synthesizer:
    """Hold one listener's reference HRTF and synthesize other source distances.

    mode='parallax': divide out the reference sphere, interpolate the listener's
    residual at ear-relative directions, then restore the target sphere.
    mode='sphere': apply the analytic DVF at the unchanged directions.
    mode='hybrid': sphere below 2 kHz, parallax above 4 kHz, raised-cosine blend
    of residual log magnitude and unwrapped relative phase in between.
    Neither mode applies propagation gain, propagation delay, EQ or normalization.
    """

    def __init__(self, frequency_hz, directions, reference_hrtf, reference_distance_m, model=HeadModel()):
        self.frequencies = frequencies(frequency_hz).copy()
        self.directions = unit_vectors(directions).copy()
        self.reference_distance = model.check_distance(reference_distance_m)
        self.model = model
        self.reference = np.array(reference_hrtf, dtype=complex, copy=True)
        if self.reference.shape != (len(self.directions), 2, len(self.frequencies)):
            raise ValueError("Reference HRTF must have shape M x 2 x F")
        if not np.isfinite(self.reference).all():
            raise ValueError("Reference HRTF contains nonfinite values")
        self.reference_sphere = sphere_response(self.frequencies, self.directions, self.reference_distance, model)
        self.residual = self.reference / self.reference_sphere
        self.mesh = None

    def at(self, distance_m, mode="hybrid"):
        distance = self.model.check_distance(distance_m)
        if mode not in ("hybrid", "parallax", "sphere"):
            raise ValueError("Mode must be hybrid, parallax or sphere")
        if distance == self.reference_distance:
            return self.reference.copy()
        near = sphere_response(self.frequencies, self.directions, distance, self.model)
        if mode == "sphere":
            return near * self.residual
        if self.mesh is None:
            self.mesh = DirectionMesh(self.directions)
        queries = parallax_directions(self.directions, distance, self.reference_distance, self.model)
        residual = np.stack([self.mesh.interpolate(self.residual[:, ear], queries[:, ear])
                             for ear in range(2)], axis=1)
        if mode == "hybrid":
            weight = .5 - .5*np.cos(np.pi*np.clip((self.frequencies-2000)/2000, 0, 1))
            phase = np.unwrap(np.angle(residual*self.residual.conj()), axis=-1)
            blended = np.exp((1-weight)*np.log(np.maximum(abs(self.residual), 1e-15))
                             + weight*np.log(np.maximum(abs(residual), 1e-15))
                             + 1j*(np.angle(self.residual) + weight*phase))
            blended[..., weight == 0] = self.residual[..., weight == 0]
            blended[..., weight == 1] = residual[..., weight == 1]
            blended[(abs(self.residual) == 0) & (abs(residual) == 0)] = 0
            residual = blended
        return near * residual


def load_hrir(path, fft_size=None, ear_order="auto"):
    """Use the repository's strict FIR SOFA reader; preserve all declared delays.

    TF SOFA input is intentionally unsupported: its external phase convention
    must not be guessed. Convert FIR by FFT for an unambiguous phase convention.
    """
    reader_path = Path(__file__).resolve().parents[2] / "room/prepare-hrir.py"
    spec = importlib.util.spec_from_file_location("dvf_sofa_reader", reader_path)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    ir, directions, delays, rate, radius, order = reader.load_sofa(path, ear_order)
    size = fft_size if fft_size is not None else 1 << (ir.shape[-1] - 1).bit_length()
    if not isinstance(size, int) or size < ir.shape[-1] or size > 16384 or size < 2:
        raise ValueError("FFT size must retain every HRIR tap and be within 2..16384")
    if rate > 96000 or len(ir)*2*(size//2 + 1)*16 > 256*1024*1024:
        raise ValueError("Spectrum exceeds the 96 kHz / 256 MiB preparation limit")
    f = np.fft.rfftfreq(size, 1/rate)
    spectrum = np.fft.rfft(ir, n=size, axis=-1)
    spectrum *= np.exp(-2j*np.pi*f[None, None, :] * delays[:, :, None]/rate)
    metadata = {"source": str(Path(path).resolve()), "source_sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                "sample_rate_hz": rate, "fft_size": size, "source_ear_indices": order,
                "data_delay_applied": True, "input_hrir_samples": ir.shape[-1]}
    return f, directions, spectrum, radius, metadata


def write_archive(output, arrays, metadata):
    """Exclusive creation; arrays never contain pickle objects or personal defaults."""
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as handle:
        np.savez_compressed(handle, **arrays, metadata_json=np.array(json.dumps(metadata, ensure_ascii=False)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New NPZ file (complex frequency responses)")
    parser.add_argument("--distances", type=float, nargs="+", default=[.2, .3, .4, .5, .75, 1., 1.3, 1.5, 1.6])
    parser.add_argument("--head-radius", type=float, default=.0875, help="Equivalent head radius in metres")
    parser.add_argument("--ear-azimuths", type=float, nargs=2, default=[90, -90], metavar=("LEFT", "RIGHT"))
    parser.add_argument("--ear-elevations", type=float, nargs=2, default=[0, 0], metavar=("LEFT", "RIGHT"))
    parser.add_argument("--sound-speed", type=float, default=343.18)
    parser.add_argument("--source", type=Path, help="Optional single-radius FIR SOFA; synthesize this listener's HRTFs")
    parser.add_argument("--mode", choices=["hybrid", "sphere", "parallax"], help="Default: hybrid with --source; sphere otherwise")
    parser.add_argument("--ear-order", choices=["auto", "lr", "rl"], default="auto")
    parser.add_argument("--fft-size", type=int, help="FFT size for FIR SOFA input")
    parser.add_argument("--reference-distance", type=float, help="Reference distance for analytic-only export (default 1.5 m)")
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise ValueError(f"Output already exists: {args.output}")
        model = HeadModel(args.head_radius, args.sound_speed, args.ear_azimuths[0], args.ear_elevations[0],
                          args.ear_azimuths[1], args.ear_elevations[1])
        radii = np.array([model.check_distance(r) for r in args.distances])
        if len(np.unique(radii)) != len(radii):
            raise ValueError("Duplicate distances are unsupported")
        metadata = {"schema": "mradm.reusable-dvf", "version": 1, "model": asdict(model),
                    "phase_convention": "numpy_fft; delay t means exp(-i*2*pi*f*t)",
                    "normalization": "correction is head-centre free-field referenced; input reference gain/delay retained; no added common 1/r gain or r/c delay",
                    "coordinate_frame": "x=front, y=left, z=up", "ear_order": ["left", "right"],
                    "response_axes": ["distance", "direction", "ear", "frequency"],
                    "scope": "Complex-response research/preparation artifact; not a causal FIR or renderer bank"}
        if args.source:
            if args.reference_distance is not None:
                raise ValueError("With --source the reference distance comes from SOFA; omit --reference-distance")
            f, directions, h, reference, source_metadata = load_hrir(args.source, args.fft_size, args.ear_order)
            if len(radii)*h.nbytes > 512*1024*1024:
                raise ValueError("Output would exceed 512 MiB; export fewer distances per call")
            synth = Synthesizer(f, directions, h, reference, model)
            mode = args.mode or "hybrid"
            responses = np.stack([synth.at(r, mode) for r in radii])
            metadata.update(source_metadata, mode=mode, response_kind="synthesized_hrtf",
                            hybrid_transition_hz=[2000, 4000] if mode == "hybrid" else None)
        else:
            if args.fft_size is not None or args.ear_order != "auto":
                raise ValueError("--fft-size and --ear-order require --source")
            if args.mode not in (None, "sphere"):
                raise ValueError("Hybrid and parallax modes require a target listener's --source SOFA")
            angles = [[0, -90]] + [[az, el] for el in [-60, -30, 0, 30, 60] for az in range(0, 360, 30)] + [[0, 90]]
            directions = spherical_directions(angles)
            f = np.r_[0., np.geomspace(20, 24000, 128)]
            reference = model.check_distance(args.reference_distance if args.reference_distance is not None else 1.5)
            responses = np.stack([distance_variation(f, directions, r, reference, model) for r in radii])
            metadata.update(mode="sphere", response_kind="dvf", grid_note="Demonstration samples; API evaluates continuous distances/directions")
        metadata["reference_distance_m"] = reference
        if not np.isfinite(responses).all() or np.max(abs(responses)) > np.finfo(np.float32).max:
            raise ValueError("Output cannot be represented as finite complex64 values")
        write_archive(args.output, {"frequency_hz": f, "directions_xyz": directions, "distance_m": radii,
                                    "response": responses.astype(np.complex64)}, metadata)
        print(json.dumps({"output": str(args.output.resolve()), "bytes": args.output.stat().st_size,
                          "shape": list(responses.shape), "metadata": metadata}, ensure_ascii=False, indent=2))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
