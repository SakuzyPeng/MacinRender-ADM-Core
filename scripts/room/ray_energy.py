#!/usr/bin/env python3
"""`ray-energy-v1`: a preparation-time stochastic ray-traced late field for generate-room.py.

Traces energy-vector rays (eight bands share one geometric path) in the hybrid-shoebox room,
receives them on a volume receiver at the listener, aligns every source to its direct arrival
plus the late predelay, softly assigns arrivals to the template's world late directions and
synthesises the same `[directions, taps]` late resource as the other generators. The runtime is
unchanged: the field is shared by every source and only the send delay depends on distance.

Design and acceptance: docs/architecture/ROOM_RAY_ENERGY_LATE_FIELD.md. Dependencies: numpy, scipy.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

import numpy as np
from scipy.signal import fftconvolve

BANDS = 8
WALLS = 6
GENERATOR = "ray-energy-v1"
DEFAULTS = {"histogram_seconds": 0.001, "min_hits_per_bin": 32, "direction_kappa": 8.0,
            "source_positions": "reference-positions", "air_attenuation_db_per_m": [0.] * BANDS}
REQUIRED = ("generator", "seed", "duration_seconds", "rays", "receiver_radius", "materials")
RUN = 3  # consecutive bins that establish the field or start the sparse tail
FIT_POINTS = 10
TERMINATION = 1e-12  # -120 dB of a ray's initial energy in every band
SEAM_BINS = 8


class RayEnergyError(ValueError):
    """Invalid definition or a histogram that the chosen sampling cannot support."""


def _number(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise RayEnergyError(f"late.{name} must be a finite number")
    if not low <= value <= high:
        raise RayEnergyError(f"late.{name} must be within {low}..{high}")
    return float(value)


def _integer(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise RayEnergyError(f"late.{name} must be an integer within {low}..{high}")
    return value


def _table(value, name, rows, columns, low=0., high=1.):
    array = np.asarray(value, dtype=object)
    if array.shape != ((rows, columns) if rows else (columns,)):
        raise RayEnergyError(f"late.{name} must have shape {(rows, columns) if rows else (columns,)}")
    for item in array.ravel():
        _number(item, name, low, high)
    return np.asarray(value, dtype=np.float64)


def validate(late, room):
    """Resolve defaults and check every field before any output exists."""
    if not isinstance(late, dict) or late.get("generator") != GENERATOR:
        raise RayEnergyError("late.generator must be ray-energy-v1")
    unknown = set(late) - set(REQUIRED) - set(DEFAULTS)
    missing = [key for key in REQUIRED if key not in late]
    if unknown or missing:
        raise RayEnergyError(f"late fields: unknown {sorted(unknown)}, missing {missing}")
    config = {**DEFAULTS, **late}
    config["seed"] = _integer(late["seed"], "seed", 0, 2**63 - 1)
    config["duration_seconds"] = _number(late["duration_seconds"], "duration_seconds", 0.1, 10.)
    config["rays"] = _integer(late["rays"], "rays", 1000, 10_000_000)
    config["histogram_seconds"] = _number(config["histogram_seconds"], "histogram_seconds", 0.0005, 0.01)
    config["min_hits_per_bin"] = _integer(config["min_hits_per_bin"], "min_hits_per_bin", 1, 1_000_000)
    config["direction_kappa"] = _number(config["direction_kappa"], "direction_kappa", 0.1, 1000.)
    materials = late["materials"]
    if not isinstance(materials, dict) or set(materials) != {"absorption", "scattering"}:
        raise RayEnergyError("late.materials needs exactly absorption and scattering")
    config["materials"] = {"absorption": _table(materials["absorption"], "materials.absorption", WALLS, BANDS),
                           "scattering": _table(materials["scattering"], "materials.scattering", WALLS, BANDS)}
    config["air_attenuation_db_per_m"] = _table(config["air_attenuation_db_per_m"], "air_attenuation_db_per_m",
                                                0, BANDS, 0., 1.)
    dimensions = np.asarray(room["dimensions"], dtype=np.float64)
    listener = np.asarray(room["listener"], dtype=np.float64)
    clearance = float(min(listener.min(), (dimensions - listener).min()))
    radius = _number(late["receiver_radius"], "receiver_radius", 0.05, 1.)
    if radius >= clearance:
        raise RayEnergyError("late.receiver_radius must be smaller than the listener's distance to every wall")
    config["receiver_radius"] = radius
    fade = [float(v) for v in room["early_fade_seconds"]]
    predelay = float(room["late_predelay_seconds"])
    if (len(fade) != 2 or not all(map(math.isfinite, fade + [predelay])) or not 0 <= fade[0] < fade[1]
            or predelay < 0):
        raise RayEnergyError("early_fade_seconds and late_predelay_seconds must be finite and ordered")
    positions = config["source_positions"]
    if positions != "reference-positions":
        if not isinstance(positions, list) or not 1 <= len(positions) <= 256:
            raise RayEnergyError("late.source_positions must be reference-positions or 1..256 positions")
        for item in positions:
            if not isinstance(item, dict) or not {"azimuth", "elevation", "distance"} <= set(item):
                raise RayEnergyError("each source position needs azimuth, elevation and distance")
            _number(item["azimuth"], "source_positions.azimuth", -360., 360.)
            _number(item["elevation"], "source_positions.elevation", -90., 90.)
            _number(item["distance"], "source_positions.distance", 1e-6, 1000.)
    return config


def resolved(config):
    """The configuration with defaults, as recorded in the manifest."""
    result = dict(config)
    result["materials"] = {k: v.tolist() for k, v in config["materials"].items()}
    result["air_attenuation_db_per_m"] = config["air_attenuation_db_per_m"].tolist()
    return result


def _constrain(room, direction, requested):
    """`geometry::constrain`: clamp the radius to the minimum and to the clearance box."""
    dimensions, listener = room["dimensions"], room["listener"]
    clearance = float(room["boundary_clearance"])
    maximum = math.inf
    for axis, component in enumerate(direction):
        if component > 1e-15:
            maximum = min(maximum, (dimensions[axis] - clearance - listener[axis]) / component)
        elif component < -1e-15:
            maximum = min(maximum, (clearance - listener[axis]) / component)
    distance = min(max(requested, float(room["minimum_distance"])), maximum)
    return distance, distance != requested


def source_positions(config, room, tool_dir):
    """Requested polar positions converted as `room_omni` does, then clamped like the runtime."""
    spec = config["source_positions"]
    if spec == "reference-positions":
        spec = json.loads((Path(tool_dir) / "reference-positions.json").read_text(encoding="utf-8"))["positions"]
    result = []
    for index, item in enumerate(spec):
        azimuth, elevation = np.float32(item["azimuth"]), np.float32(item["elevation"])
        distance = float(item["distance"])
        a, e = math.radians(float(azimuth)), math.radians(float(elevation))
        unit = (math.cos(a) * math.cos(e), math.sin(a) * math.cos(e), math.sin(e))
        scene = [np.float32(-unit[1] * distance), np.float32(unit[0] * distance), np.float32(unit[2] * distance)]
        relative = (float(scene[1]), -float(scene[0]), float(scene[2]))
        length = math.sqrt(relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2])
        if not length > 1e-15:
            raise RayEnergyError("source position must not coincide with the listener")
        direction = tuple(v / length for v in relative)
        effective, constrained = _constrain(room, direction, length)
        point = [room["listener"][i] + direction[i] * effective for i in range(3)]
        result.append(dict(label=item.get("label", str(index)), azimuth=float(azimuth), elevation=float(elevation),
                           requested_distance=distance, distance=effective, constrained=constrained,
                           direction=list(direction), point=point))
    return result


def positions_sha256(positions):
    payload = np.asarray([p["point"] for p in positions], dtype="<f8").tobytes()
    return hashlib.sha256(payload).hexdigest()


def fade(relative, early):
    """`geometry::path`: raised-cosine early amplitude by time after the direct arrival."""
    start, end = early
    value = 0.5 * (1. + np.cos(np.pi * (np.asarray(relative, dtype=np.float64) - start) / (end - start)))
    return np.where(relative >= end, 0., np.where(relative <= start, 1., value))


def late_weight(relative, early):
    """Energy left to the late field: the early field keeps fade² of each arrival's energy."""
    return 1. - fade(relative, early) ** 2


def _basis(axis):
    helper = np.array([0., 0., 1.]) if abs(axis[2]) < 0.9 else np.array([1., 0., 0.])
    first = np.cross(axis, helper)
    first /= np.linalg.norm(first)
    return first, np.cross(axis, first)


def emit(direction, rays, rng):
    """Latin-hypercube directions with density ∝ beam⁴ about the source's axis toward the listener.

    `geometry::path` gives a source amplitude of beam² with beam = max(0, cos θ), θ measured from
    the source-to-listener direction, so the emitted energy density is cos⁴ θ on the front
    hemisphere. Importance sampling gives every ray the same energy (1 / rays).
    """
    axis = -np.asarray(direction, dtype=np.float64)
    first, second = _basis(axis)
    u = (rng.permutation(rays) + rng.random(rays)) / rays
    phi = 2 * np.pi * (rng.permutation(rays) + rng.random(rays)) / rays
    mu = u ** 0.2
    sine = np.sqrt(np.maximum(0., 1. - mu * mu))
    return (mu[:, None] * axis + (sine * np.cos(phi))[:, None] * first + (sine * np.sin(phi))[:, None] * second)


def scattering_groups(scattering):
    """Bands whose six wall scattering coefficients are identical share one set of paths.

    A shared path with per-band diffuse/specular weights (s_b / s̄, (1 - s_b) / (1 - s̄)) keeps
    each band's expectation but its variance grows geometrically with the bounce count when the
    bands' coefficients differ, so differing bands are traced separately.
    """
    groups = {}
    for band in range(BANDS):
        groups.setdefault(tuple(scattering[:, band].tolist()), []).append(band)
    return sorted(groups.values())


def trace_source(config, room, position, rng, bands=tuple(range(BANDS))):
    """One source's reflected receiver crossings: times, arrival directions and band energies.

    `bands` share the same scattering on every wall. Every wall hit multiplies by 1 - α_b and is
    diffuse (Lambert) with probability s, otherwise specular. Rays stop below -120 dB in every
    traced band or after the histogram window.
    """
    dimensions = np.asarray(room["dimensions"], dtype=np.float64)
    centre = np.asarray(room["listener"], dtype=np.float64)
    speed = float(room["speed_of_sound"])
    radius = config["receiver_radius"]
    volume = 4. / 3. * math.pi * radius ** 3
    bands = list(bands)
    reflection = 1. - config["materials"]["absorption"][:, bands]
    scattering = config["materials"]["scattering"][:, bands]
    if np.any(scattering != scattering[:, :1]):
        raise RayEnergyError("a traced band group must share its scattering coefficients")
    scattering = scattering[:, 0]
    air = config["air_attenuation_db_per_m"][bands] * (math.log(10.) / 10.)
    rays = config["rays"]
    limit = position["distance"] / speed + float(room["late_predelay_seconds"]) + config["duration_seconds"]
    initial = 1. / rays
    origin = np.tile(np.asarray(position["point"], dtype=np.float64), (rays, 1))
    direction = emit(position["direction"], rays, rng)
    energy = np.full((rays, len(bands)), initial)
    elapsed = np.zeros(rays)
    reflected = np.zeros(rays, dtype=bool)
    times, arrivals, energies = [], [], []
    rows = np.arange(rays)
    while len(rows):
        with np.errstate(divide="ignore", invalid="ignore"):
            span = np.where(direction > 0, (dimensions - origin) / direction,
                            np.where(direction < 0, -origin / direction, np.inf))
        axis = np.argmin(span, axis=1)
        index = np.arange(len(rows))
        step = span[index, axis]
        # Volume receiver: chord of the segment inside the sphere, reflected paths only.
        offset = origin - centre
        b = np.einsum("ij,ij->i", offset, direction)
        disc = b * b - (np.einsum("ij,ij->i", offset, offset) - radius * radius)
        hit = reflected & (disc > 0)
        if hit.any():
            root = np.sqrt(disc[hit])
            low = np.maximum(-b[hit] - root, 0.)
            high = np.minimum(-b[hit] + root, step[hit])
            inside = high > low
            if inside.any():
                selected = np.flatnonzero(hit)[inside]
                middle = 0.5 * (low[inside] + high[inside])
                chord = high[inside] - low[inside]
                band = energy[selected] * (chord / volume)[:, None]
                if air.any():
                    band = band * np.exp(-np.outer(middle, air))
                times.append(elapsed[selected] + middle / speed)
                arrivals.append(-direction[selected])
                energies.append(band)
        origin = origin + step[:, None] * direction
        elapsed = elapsed + step / speed
        if air.any():
            energy = energy * np.exp(-np.outer(step, air))
        upper = direction[index, axis] > 0
        origin = np.clip(origin, 0., dimensions)
        origin[index, axis] = np.where(upper, dimensions[axis], 0.)
        wall = axis * 2 + upper
        energy = energy * reflection[wall]
        diffuse = rng.random(len(rows)) < scattering[wall]
        specular = ~diffuse
        direction[index[specular], axis[specular]] *= -1.
        if diffuse.any():
            chosen = np.flatnonzero(diffuse)
            first, second = rng.random(len(chosen)), rng.random(len(chosen))
            radial, azimuth = np.sqrt(first), 2 * np.pi * second
            normal_axis = axis[chosen]
            fresh = np.zeros((len(chosen), 3))
            fresh[np.arange(len(chosen)), normal_axis] = np.where(upper[chosen], -1., 1.) * np.sqrt(1. - first)
            fresh[np.arange(len(chosen)), (normal_axis + 1) % 3] = radial * np.cos(azimuth)
            fresh[np.arange(len(chosen)), (normal_axis + 2) % 3] = radial * np.sin(azimuth)
            direction[chosen] = fresh
        reflected[:] = True
        alive = (elapsed <= limit) & (energy.max(axis=1) >= TERMINATION * initial)
        rows, origin, direction = rows[alive], origin[alive], direction[alive]
        energy, elapsed, reflected = energy[alive], elapsed[alive], reflected[alive]
    if not times:
        return np.zeros(0), np.zeros((0, 3)), np.zeros((0, len(bands)))
    return np.concatenate(times), np.concatenate(arrivals), np.concatenate(energies)


def soft_directions(arrivals, directions, kappa):
    """Normalised von Mises–Fisher weights of each arrival over the template directions."""
    logits = kappa * (arrivals @ directions.T)
    logits -= logits.max(axis=1, keepdims=True)
    weights = np.exp(logits)
    return weights / weights.sum(axis=1, keepdims=True)


def bin_count(config):
    return int(math.ceil(config["duration_seconds"] / config["histogram_seconds"] - 1e-9))


def binned(config, room, directions, times, arrivals, energies, direct_time, bands, chunk=20000):
    """One traced group's contribution: energy, squared contributions and the onset account."""
    predelay = float(room["late_predelay_seconds"])
    width, bins = config["histogram_seconds"], bin_count(config)
    relative = times - direct_time
    late = relative - predelay
    keep = late < config["duration_seconds"]
    relative, late, arrivals, energies = relative[keep], late[keep], arrivals[keep], energies[keep]
    weighted = energies * late_weight(relative, room["early_fade_seconds"])[:, None]
    result = dict(bands=list(bands), raw=energies.sum(axis=0), windowed=weighted.sum(axis=0),
                  uncovered=weighted[late < 0].sum(axis=0))
    inside = late >= 0
    late, arrivals, weighted = late[inside], arrivals[inside], weighted[inside]
    count = len(directions)
    index_bins = np.minimum((late / width).astype(np.int64), bins - 1)
    energy = np.zeros((count, len(bands), bins))
    squares = np.zeros_like(energy)
    for start in range(0, len(late), chunk):
        stop = start + chunk
        weights = soft_directions(arrivals[start:stop], directions, config["direction_kappa"])
        index = (np.arange(count)[None, :] * bins + index_bins[start:stop, None]).ravel()
        for column in range(len(bands)):
            values = (weights * weighted[start:stop, column, None]).ravel()
            energy[:, column] += np.bincount(index, values, minlength=count * bins).reshape(count, bins)
            squares[:, column] += np.bincount(index, values * values, minlength=count * bins).reshape(count, bins)
    result.update(energy=energy, squares=squares, allocated=float(weighted.sum()), hits=len(late))
    return result


def trace_task(task):
    """A picklable (source, scattering group) unit; its seed stream depends only on the task."""
    config, room, directions, position, index, bands = task
    stream = np.random.SeedSequence(config["seed"], spawn_key=(0, index, bands[0]))
    times, arrivals, energies = trace_source(config, room, position, np.random.Generator(np.random.PCG64(stream)),
                                             bands)
    return binned(config, room, directions, times, arrivals, energies,
                  position["distance"] / float(room["speed_of_sound"]), bands)


class Histogram:
    """Energy and squared contributions per (direction, band, bin), with the onset energy account."""

    def __init__(self, config, room, directions):
        self.config, self.room = config, room
        self.directions = np.asarray(directions, dtype=np.float64)
        self.width = config["histogram_seconds"]
        self.bins = bin_count(config)
        shape = (len(self.directions), BANDS, self.bins)
        self.energy, self.squares = np.zeros(shape), np.zeros(shape)
        self.raw, self.windowed, self.uncovered = np.zeros(BANDS), np.zeros(BANDS), np.zeros(BANDS)
        self.allocated, self.hits = 0., 0

    def merge(self, part):
        bands = part["bands"]
        self.raw[bands] += part["raw"]
        self.windowed[bands] += part["windowed"]
        self.uncovered[bands] += part["uncovered"]
        self.energy[:, bands] += part["energy"]
        self.squares[:, bands] += part["squares"]
        self.allocated += part["allocated"]
        self.hits += part["hits"]

    def effective_hits(self):
        return np.divide(self.energy ** 2, self.squares, out=np.zeros_like(self.energy), where=self.squares > 0)

    def account(self):
        histogram = self.energy.sum(axis=(0, 2))
        total = float(histogram.sum())
        error = abs(total - self.allocated) / max(self.allocated, 1e-300)
        ratio = [float(u / w) if w > 0 else None for u, w in zip(self.uncovered, self.windowed)]
        windowed = float(self.windowed.sum())
        return dict(raw=self.raw.tolist(), window_suppressed=(self.raw - self.windowed).tolist(),
                    uncovered=self.uncovered.tolist(), histogram=histogram.tolist(),
                    uncovered_onset_energy_ratio=float(self.uncovered.sum() / windowed) if windowed > 0 else None,
                    uncovered_onset_energy_ratio_by_band=ratio, direction_allocation_relative_error=error)


def _runs(mask, length=RUN):
    if len(mask) < length:
        return np.zeros(0, dtype=bool)
    result = mask[:len(mask) - length + 1].copy()
    for shift in range(1, length):
        result &= mask[shift:len(mask) - length + 1 + shift]
    return result


def fit_decay(times, energy, low, high, compensation=0.):
    """Least squares on the backward-integrated decay between `low` and `high` dB."""
    remaining = np.cumsum(energy[::-1])[::-1] + compensation
    if not remaining[0] > 0:
        return None
    level = 10 * np.log10(np.maximum(remaining / remaining[0], 1e-300))
    use = (level <= low) & (level >= high)
    if use.sum() < FIT_POINTS:
        return None
    slope, intercept = np.polyfit(times[use], level[use], 1)
    if not slope < 0:
        return None
    chosen = np.flatnonzero(use)
    return dict(slope=float(slope), first=int(chosen[0]), last=int(chosen[-1]))


def tail(energy, hits, width, minimum):
    """Keep the measured series or extrapolate its sparse tail; see the design §7."""
    count = len(energy)
    times = (np.arange(count) + 0.5) * width
    if not energy.any():
        return energy.copy(), dict(status="zero")
    reliable = hits >= minimum
    established = _runs(reliable)
    if not established.any():
        raise RayEnergyError(
            f"晚场直方图没有连续 {RUN} 个分箱达到 {minimum} 次有效命中：提高 rays 或 histogram_seconds")
    onset = int(np.argmax(established))
    peak = int(np.argmax(np.where(reliable & (np.arange(count) >= onset), energy, -np.inf)))
    sparse = _runs(~reliable)
    candidates = np.flatnonzero(sparse[peak + 1:]) + peak + 1 if len(sparse) > peak + 1 else np.zeros(0, int)
    if not len(candidates):
        return energy.copy(), dict(status="measured")
    seam = int(candidates[0])
    observed = energy[onset:seam]
    fit = fit_decay(times[onset:seam], observed, -5., -25.)
    compensation = 0.
    for _ in range(8 if fit is not None else 0):
        # Truncation correction: the bins after the seam, continued along the current slope.
        ratio = 10 ** (fit["slope"] * width / 10)
        compensation_next = float(energy[seam - 1] * ratio / (1 - ratio))
        refit = fit_decay(times[onset:seam], observed, -5., -25., compensation_next)
        if refit is None:
            break
        fit, converged = refit, abs(compensation_next - compensation) <= 1e-9 * compensation_next
        compensation = compensation_next
        if converged:
            break
    if fit is None:
        raise RayEnergyError(
            f"晚场尾段在 {seam * width:.3f} s 变稀疏，之前的可靠观测不足以拟合 -5 至 -25 dB"
            f"（需要至少 {FIT_POINTS} 个分箱）：提高 rays")
    before = np.flatnonzero(reliable[onset:seam])[-SEAM_BINS:] + onset
    slope = fit["slope"]
    level = float(np.mean(energy[before] * 10 ** (slope * (times[seam] - times[before]) / 10)))
    result = energy.copy()
    result[seam:] = level * 10 ** (slope * (times[seam:] - times[seam]) / 10)
    return result, dict(status="extrapolated", start_seconds=float(seam * width), slope_db_per_second=slope,
                        fit_seconds=[float(times[onset + fit["first"]]), float(times[onset + fit["last"]])])


def synthesize(energy, bands, sample_rate, duration, width, rng):
    """Seeded band noise under each (direction, band) envelope; one common energy calibration.

    The template bands sum to a delta, so filtered unit noise keeps each band's share of a flat
    spectrum, as `noise_field` and the early material filters do: equal band energies give a
    white late field.
    """
    directions, band_count, bins = energy.shape
    frames = int(round(duration * sample_rate))
    samples = np.arange(frames, dtype=np.float64) / sample_rate
    centres = (np.arange(bins) + 0.5) * width
    field = np.zeros((directions, frames), dtype=np.float64)
    for channel in range(directions):
        for band in range(band_count):
            noise = rng.standard_normal(frames + bands.shape[1] - 1)
            filtered = fftconvolve(noise, bands[band], mode="valid")
            envelope = np.interp(samples, centres, np.sqrt(energy[channel, band] / width))
            field[channel] += filtered * envelope
    fade_frames = min(sample_rate // 200, frames)
    field[:, -fade_frames:] *= np.linspace(1., 0., fade_frames, dtype=np.float64)
    mean_energy = np.mean(np.sum(field * field, axis=1))
    if not np.isfinite(mean_energy) or mean_energy <= 0:
        raise RayEnergyError("late field has no finite energy")
    return field / np.sqrt(mean_energy)


def decay_times(energy, width):
    """EDT, T20 and T30 of a band envelope from its full Schroeder integral (None when unfit)."""
    times = (np.arange(len(energy)) + 0.5) * width
    result = {}
    for name, low, high in (("edt", 0., -10.), ("t20", -5., -25.), ("t30", -5., -35.)):
        fit = fit_decay(times, energy, low, high)
        result[name] = None if fit is None else float(-60. / fit["slope"])
    return result


def collect(config, room, directions, positions, jobs=1):
    """Trace every (source, scattering group) and merge in that fixed order for any `jobs`."""
    histogram = Histogram(config, room, directions)
    groups = scattering_groups(config["materials"]["scattering"])
    tasks = [(config, room, histogram.directions, position, index, bands)
             for index, position in enumerate(positions) for bands in groups]
    hits = []
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            for wave in range(0, len(tasks), 2 * jobs):  # bounded memory: results merge in task order
                for part in pool.map(trace_task, tasks[wave:wave + 2 * jobs]):
                    histogram.merge(part)
                    hits.append(part["hits"])
    else:
        for task in tasks:
            part = trace_task(task)
            histogram.merge(part)
            hits.append(part["hits"])
    per_source = [hits[i * len(groups):(i + 1) * len(groups)] for i in range(len(positions))]
    return histogram, per_source


def generate(late, room, bands, directions, tool_dir, jobs=1):
    """Validate, trace every source, finish the histogram and synthesise the late resource.

    Returns the field, the deterministic manifest summary and the per-run report.
    """
    config = validate(late, room)
    directions = np.asarray(directions, dtype=np.float64)
    bands = np.asarray(bands, dtype=np.float64)
    if bands.ndim != 2 or bands.shape[0] != BANDS:
        raise RayEnergyError("template bands must have eight rows")
    positions = source_positions(config, room, tool_dir)
    histogram, per_source = collect(config, room, directions, positions, jobs)
    seed = config["seed"]
    account = histogram.account()
    if not sum(account["histogram"]) > 0:
        raise RayEnergyError("no reflected energy reached the late-field histogram")
    hits = histogram.effective_hits()
    finished = np.zeros_like(histogram.energy)
    extrapolation = [[None] * BANDS for _ in range(len(directions))]
    zero = 0
    for d in range(len(directions)):
        for b in range(BANDS):
            finished[d, b], info = tail(histogram.energy[d, b], hits[d, b], histogram.width,
                                        config["min_hits_per_bin"])
            zero += info["status"] == "zero"
            extrapolation[d][b] = None if info["status"] != "extrapolated" else \
                [info["start_seconds"], info["slope_db_per_second"]] + info["fit_seconds"]
    noise = np.random.Generator(np.random.PCG64(np.random.SeedSequence(seed, spawn_key=(1,))))
    field = synthesize(finished, bands, int(room["sample_rate"]), config["duration_seconds"], histogram.width, noise)
    band_energy = finished.sum(axis=(0, 2))
    decays = [decay_times(finished[:, b].sum(axis=0), histogram.width) for b in range(BANDS)]
    summary = dict(
        generator=GENERATOR, resolved=resolved(config), sources=len(positions),
        scattering_groups=scattering_groups(config["materials"]["scattering"]),
        emission_positions_sha256=positions_sha256(positions),
        histogram_bins=histogram.bins,
        late_edt_seconds=[d["edt"] for d in decays], late_t20_seconds=[d["t20"] for d in decays],
        late_t30_seconds=[d["t30"] for d in decays],
        direction_energy_fraction=[(finished[:, b].sum(axis=1) / band_energy[b]).tolist()
                                   if band_energy[b] > 0 else None for b in range(BANDS)],
        extrapolation=dict(columns=["start_seconds", "slope_db_per_second", "fit_start_seconds", "fit_end_seconds"],
                           by_direction_band=extrapolation),
        zero_series=zero, energy=account,
        decay_scope="Late-field component only: arrivals after the direct sound plus the predelay, "
                    "weighted by 1 - fade(u)^2; not a complete room RT.")
    report = dict(positions=[{k: p[k] for k in ("label", "azimuth", "elevation", "requested_distance", "distance",
                                                "constrained", "point")} for p in positions],
                  hits_by_source_and_group=per_source, effective_hits=dict(
                      minimum=float(hits[histogram.energy > 0].min()) if (histogram.energy > 0).any() else 0.,
                      median=float(np.median(hits[histogram.energy > 0])) if (histogram.energy > 0).any() else 0.))
    return field, summary, report
