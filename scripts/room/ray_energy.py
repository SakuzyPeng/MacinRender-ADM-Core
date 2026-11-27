#!/usr/bin/env python3
"""`ray-energy-v1`: a preparation-time stochastic ray-traced late field for generate-room.py.

Traces rays in the shoebox with a shared path for bands of equal scattering,
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
from scipy.signal import fftconvolve, resample_poly

BANDS = 8
WALLS = 6
GENERATOR = "ray-energy-v1"
DEFAULTS = {"histogram_seconds": 0.001, "min_hits_per_bin": 32, "direction_kappa": 8.0,
            "source_positions": "reference-positions", "air_attenuation_db_per_m": [0.] * BANDS}
REQUIRED = ("generator", "seed", "duration_seconds", "rays", "receiver_radius", "materials")
RUN = 3  # consecutive bins that establish the field or start the sparse tail
FIT_POINTS = 10
TERMINATION = 1e-12  # -120 dB of a ray's initial energy in every band
NEGLIGIBLE_TAIL_FRACTION = 1e-6  # -60 dB of a band's integrated energy
TAIL_FADE_SECONDS = 0.005


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
    zoned = room.get("kind") == "hybrid-shoebox-v2"
    required = [key for key in REQUIRED if not zoned or key != "materials"]
    unknown = set(late) - set(required) - set(DEFAULTS)
    missing = [key for key in required if key not in late]
    if unknown or missing:
        raise RayEnergyError(f"late fields: unknown {sorted(unknown)}, missing {missing}")
    config = {**DEFAULTS, **late}
    config["seed"] = _integer(late["seed"], "seed", 0, 2**63 - 1)
    config["duration_seconds"] = _number(late["duration_seconds"], "duration_seconds", 0.1, 10.)
    config["rays"] = _integer(late["rays"], "rays", 1000, 10_000_000)
    config["histogram_seconds"] = _number(config["histogram_seconds"], "histogram_seconds", 0.0005, 0.01)
    config["min_hits_per_bin"] = _integer(config["min_hits_per_bin"], "min_hits_per_bin", 1, 1_000_000)
    config["direction_kappa"] = _number(config["direction_kappa"], "direction_kappa", 0.1, 1000.)
    if zoned:
        from room_surfaces import SurfaceMap
        surface_map = SurfaceMap(room["surface_model"], room["dimensions"])
        config["_surfaces"] = surface_map
        config["materials"] = {"absorption": surface_map.absorption, "scattering": surface_map.scattering}
    else:
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
    if result.pop("_surfaces", None) is not None:
        del result["materials"]
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
    stalled = np.zeros(rays, dtype=np.int64)
    times, arrivals, energies = [], [], []
    rows = np.arange(rays)
    surfaces = config.get("_surfaces")
    while len(rows):
        with np.errstate(divide="ignore", invalid="ignore"):
            span = np.where(direction > 0, (dimensions - origin) / direction,
                            np.where(direction < 0, -origin / direction, np.inf))
        axis = np.argmin(span, axis=1)
        index = np.arange(len(rows))
        step = span[index, axis]
        # A box corner or edge gives at most two zero-length steps in a row.
        stalled = np.where(step > 0, 0, stalled + 1)
        if not np.all(np.isfinite(step) & (step >= 0)) or stalled.max(initial=0) > 2:
            raise RayEnergyError("a ray left the room; the reflection geometry is inconsistent")
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
        slot = surfaces.bindings(wall, origin) if surfaces is not None else wall
        energy = energy * reflection[slot]
        diffuse = rng.random(len(rows)) < scattering[slot]
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
            if surfaces is not None:
                fan = surfaces.fans[slot[chosen]]
                fi = chosen[fan]
                fresh[fan] = surfaces.scatter_fan(direction[fi], slot[fi], axis[fi], upper[fi], second[fan])
            direction[chosen] = fresh
        reflected[:] = True
        alive = (elapsed <= limit) & (energy.max(axis=1) >= TERMINATION * initial)
        rows, origin, direction = rows[alive], origin[alive], direction[alive]
        energy, elapsed, reflected, stalled = energy[alive], elapsed[alive], reflected[alive], stalled[alive]
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
    pooled = np.zeros((len(bands), bins))  # squares of whole arrivals, before the direction split
    for column in range(len(bands)):
        pooled[column] = np.bincount(index_bins, weighted[:, column] ** 2, minlength=bins)
    for start in range(0, len(late), chunk):
        stop = start + chunk
        weights = soft_directions(arrivals[start:stop], directions, config["direction_kappa"])
        index = (np.arange(count)[None, :] * bins + index_bins[start:stop, None]).ravel()
        for column in range(len(bands)):
            values = (weights * weighted[start:stop, column, None]).ravel()
            energy[:, column] += np.bincount(index, values, minlength=count * bins).reshape(count, bins)
            squares[:, column] += np.bincount(index, values * values, minlength=count * bins).reshape(count, bins)
    result.update(energy=energy, squares=squares, pooled_squares=pooled, allocated=float(weighted.sum()),
                  hits=len(late))
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
        self.pooled_squares = np.zeros((BANDS, self.bins))
        self.raw, self.windowed, self.uncovered = np.zeros(BANDS), np.zeros(BANDS), np.zeros(BANDS)
        self.allocated, self.hits = 0., 0

    def merge(self, part):
        bands = part["bands"]
        self.raw[bands] += part["raw"]
        self.windowed[bands] += part["windowed"]
        self.uncovered[bands] += part["uncovered"]
        self.energy[:, bands] += part["energy"]
        self.squares[:, bands] += part["squares"]
        self.pooled_squares[bands] += part["pooled_squares"]
        self.allocated += part["allocated"]
        self.hits += part["hits"]

    def effective_hits(self):
        return effective_hits(self.energy, self.squares)

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


def effective_hits(energy, squares):
    return np.divide(energy ** 2, squares, out=np.zeros_like(energy), where=squares > 0)


def _seam(energy, hits, minimum):
    """First sustained sparse run after the reliable peak, or None."""
    reliable = hits >= minimum
    established = _runs(reliable)
    if not established.any():
        raise RayEnergyError(
            f"晚场直方图没有连续 {RUN} 个分箱达到 {minimum} 次有效命中：提高 rays 或 histogram_seconds")
    onset = int(np.argmax(established))
    peak = int(np.argmax(np.where(reliable & (np.arange(len(energy)) >= onset), energy, -np.inf)))
    sparse = _runs(~reliable)
    candidates = np.flatnonzero(sparse[peak + 1:]) + peak + 1 if len(sparse) > peak + 1 else np.zeros(0, int)
    return int(candidates[0]) if len(candidates) else None


def _geometric_sum(rate, count):
    """Σ e^(-rate j) for j = 0..count-1."""
    return float(count) if rate == 0 else float(-np.expm1(-rate * count) / -np.expm1(-rate))


def _pair_rate(first, second):
    """Decay per bin of one exponential that gives both windows' mean energy densities.

    For windows [a, a + n) the mean is e^(-k a) S(k, n) / n with S the geometric sum, so the
    log ratio of the second to the first mean falls monotonically in k; k follows by bisection
    and is held at zero when the later window is not quieter.
    """
    (a1, b1, e1), (a2, b2, e2) = first, second
    n1, n2 = b1 - a1, b2 - a2
    target = math.log(e2) - math.log(e1) + math.log(n1 / n2)
    if target >= 0:
        return 0.

    def ratio(rate):
        return -rate * (a2 - a1) + math.log(_geometric_sum(rate, n2) / n2) - math.log(_geometric_sum(rate, n1) / n1)

    low, high = 0., 1.
    while ratio(high) > target and high < 1e3:
        high *= 2
    for _ in range(100):
        middle = 0.5 * (low + high)
        low, high = (middle, high) if ratio(middle) > target else (low, middle)
    return 0.5 * (low + high)


def _enough_hits(energy, square_sum, minimum):
    return square_sum > 0. and energy * energy >= minimum * square_sum


def windowed_tail(energy, squares, width, minimum):
    """Return the pooled band envelope, its diagnostics and the start bin of each budget window."""
    count = len(energy)
    occupied = np.flatnonzero(energy)
    if occupied.size == 0:
        return energy.copy(), dict(status="zero"), np.arange(count)
    seam = _seam(energy, effective_hits(energy, squares), minimum)
    end = int(occupied[-1]) + 1
    if seam is None or seam >= end:
        return energy.copy(), dict(status="measured"), np.arange(count)
    windows, start = [], seam
    while start < end:
        stop, total, square = start, 0., 0.
        while stop < end:
            total, square, stop = total + energy[stop], square + squares[stop], stop + 1
            if _enough_hits(total, square, minimum):
                break
        windows.append([start, stop, total, square])
        start = stop
    # A dominant ray in the remainder can lower the effective count even after a merge.
    while len(windows) > 1 and not _enough_hits(windows[-1][2], windows[-1][3], minimum):
        last = windows.pop()
        windows[-1] = [windows[-1][0], last[1], windows[-1][2] + last[2], windows[-1][3] + last[3]]
    result = energy.copy()
    total, square = windows[-1][2:]
    if not _enough_hits(total, square, minimum):
        fraction = float(total / energy.sum())
        hits = float(total ** 2 / square) if square > 0. else 0.
        if fraction > NEGLIGIBLE_TAIL_FRACTION:
            raise RayEnergyError(f"晚场尾段采样不足：剩余频带能量占比 {fraction:.3g}，"
                                 f"有效命中 {hits:.2f} < {minimum}，请提高 rays 或 histogram_seconds")
        # Confine the unresolved energy to a short cosine amplitude fade at the seam.
        length = min(count - seam, math.ceil(TAIL_FADE_SECONDS / width))
        weights = np.cos((np.arange(length) + 0.5) * (np.pi / (2 * length))) ** 2
        result[seam:] = 0.
        result[seam:seam + length] = total * (weights / weights.sum())
        info = dict(status="tapered", start_seconds=float(seam * width), fade_bins=length,
                    residual_energy_fraction=fraction, effective_hits=hits)
        return result, info, np.arange(seam + 1)

    spans = [(first, stop, total) for first, stop, total, _ in windows]
    for i, (first, stop, total) in enumerate(spans):
        rates = [_pair_rate(spans[j], spans[j + 1]) for j in (i - 1, i) if 0 <= j < len(spans) - 1]
        # A single supported window still has a measured neighbour just before the seam.
        rate = float(np.mean(rates)) if rates else _pair_rate((seam - 1, seam, energy[seam - 1]), spans[i])
        weights = np.exp(-rate * np.arange(stop - first, dtype=np.float64))
        result[first:stop] = total * (weights / weights.sum())
    starts = [*range(seam), *(first for first, _, _ in spans)]
    if end < count:
        starts.append(end)  # keep the observed trailing silence
    info = dict(status="merged", start_seconds=float(seam * width), windows=len(windows),
                last_window_seconds=float((end - spans[-1][0]) * width))
    return result, info, np.asarray(starts)


def tail(energy, squares, width, minimum):
    """A single band's envelope and diagnostics, also used independently of FIR synthesis."""
    shape, info, _ = windowed_tail(energy, squares, width, minimum)
    return shape, info


def distribute_directions(energy, squares, hits, shape, starts, minimum):
    """Allocate each pooled window's energy across directions, keeping reliable observations."""
    directions, bins = energy.shape
    seams = np.full(directions, bins)
    for d in range(directions):
        if energy[d].any():
            seam = _seam(energy[d], hits[d], minimum)
            if seam is not None:
                seams[d] = seam

    observed = np.add.reduceat(energy, starts, axis=1)
    support = effective_hits(observed, np.add.reduceat(squares, starts, axis=1))
    previous = np.zeros(directions)
    smoothed = np.zeros(directions, dtype=int)
    finished = np.zeros_like(energy)
    for i, (first, stop) in enumerate(zip(starts, np.append(starts[1:], bins))):
        amount = observed[:, i].copy()
        total = amount.sum()
        if total == 0.:
            continue
        sparse = (seams <= first) & (support[:, i] < minimum)
        if sparse.any():
            confidence = support[sparse, i] / minimum
            shares = confidence * (amount[sparse] / total) + (1. - confidence) * previous[sparse]
            # Only redistribute the energy left by the reliable directions.
            share_sum = shares.sum()
            if share_sum > 0.:
                amount[sparse] = amount[sparse].sum() * (shares / share_sum)
            smoothed[sparse] += 1
        previous = amount / total
        if stop == first + 1:
            finished[:, first] = amount
        else:
            finished[:, first:stop] = previous[:, None] * shape[first:stop]
    return finished, seams, smoothed


def synthesize(energy, bands, sample_rate, duration, width, rng, ultrasonic_rng=None):
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
            length = frames + bands.shape[1] - 1
            if ultrasonic_rng is None:
                noise = rng.standard_normal(length)
            else:
                # Share the audible excitation with the 48 kHz pack. The independent
                # upper octave is shifted to 24..48 kHz. Filter each at its native rate.
                base_length = round(duration * 48000) + (bands.shape[1] + 1) // 2 - 1
                low = resample_poly(rng.standard_normal(base_length), 2, 1, window=('kaiser', 8.6))
                high = resample_poly(ultrasonic_rng.standard_normal(base_length), 2, 1, window=('kaiser', 8.6))
                high[1::2] *= -1.
                noise = np.pad((low + high) / np.sqrt(2.), (1, max(0, length - len(low) - 1)))[:length]
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
    minimum = config["min_hits_per_bin"]
    pooled_tails = []
    finished = np.zeros_like(histogram.energy)
    continuation = [[None] * BANDS for _ in range(len(directions))]
    for b in range(BANDS):
        observed = histogram.energy[:, b]
        shape, info, starts = windowed_tail(observed.sum(axis=0), histogram.pooled_squares[b],
                                            histogram.width, minimum)
        finished[:, b], seams, smoothed = distribute_directions(observed, histogram.squares[:, b], hits[:, b],
                                                               shape, starts, minimum)
        difference = np.abs(finished[:, b].sum(axis=0) - shape)
        # Subnormal energies have an absolute rounding floor. Check exact silence separately.
        relative = difference / np.maximum(shape, np.finfo(shape.dtype).tiny)
        info["max_bin_energy_relative_error"] = float(relative.max())
        if info["max_bin_energy_relative_error"] > 1e-12 or np.any(difference[shape == 0.]):
            raise RayEnergyError("晚场合成方向的分箱能量与频带能量不符")
        pooled_tails.append(info)
        for d in np.flatnonzero(seams < histogram.bins):
            continuation[d][b] = [float(seams[d] * histogram.width), int(smoothed[d])]
    noise = np.random.Generator(np.random.PCG64(np.random.SeedSequence(seed, spawn_key=(1,))))
    extra = {}
    if "_surfaces" in config and room["sample_rate"] == 96000:
        extra["ultrasonic_rng"] = np.random.Generator(np.random.PCG64(np.random.SeedSequence(seed, spawn_key=(2,))))
    field = synthesize(finished, bands, int(room["sample_rate"]), config["duration_seconds"], histogram.width, noise, **extra)
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
        pooled_tail=pooled_tails,
        continuation=dict(columns=["start_seconds", "smoothed_windows"], by_direction_band=continuation),
        zero_series=int(np.count_nonzero(~histogram.energy.any(axis=2))), energy=account,
        decay_scope="Late-field component only: arrivals after the direct sound plus the predelay, "
                    "weighted by 1 - fade(u)^2; not a complete room RT.")
    report = dict(positions=[{k: p[k] for k in ("label", "azimuth", "elevation", "requested_distance", "distance",
                                                "constrained", "point")} for p in positions],
                  hits_by_source_and_group=per_source, effective_hits=dict(
                      minimum=float(hits[histogram.energy > 0].min()) if (histogram.energy > 0).any() else 0.,
                      median=float(np.median(hits[histogram.energy > 0])) if (histogram.energy > 0).any() else 0.))
    if "_surfaces" in config:
        report["band_bin_seconds"] = histogram.width
        report["band_envelopes"] = finished.sum(axis=0).tolist()
        report["direction_moments"] = []
        centres = (np.arange(histogram.bins) + .5) * histogram.width
        for lo, hi in ((0., .05), (.05, .15), (.15, .35), (.35, config["duration_seconds"])):
            hi = min(hi, config["duration_seconds"])
            if hi <= lo:
                break
            weights = finished[:, :, (centres >= lo) & (centres < hi)].sum(axis=2)
            weights = np.divide(weights, weights.sum(axis=0), out=np.zeros_like(weights), where=weights.sum(axis=0) > 0)
            tensor = np.einsum("db,di,dj->bij", weights, directions, directions)
            report["direction_moments"].append(dict(seconds=[lo, hi], tensor=tensor.tolist()))
    return field, summary, report
