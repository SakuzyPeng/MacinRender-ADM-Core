#!/usr/bin/env python3
"""Self-tests of the ray-energy-v1 late-field generator (scripts/room/ray_energy.py).

Covers docs/architecture/ROOM_RAY_ENERGY_LATE_FIELD.md, "生成器自测": conservation, diffuse
decay against Eyring, scattering groups, direction conventions, the onset energy account,
determinism, the tail rules and the error paths. Needs numpy and scipy; no build, no LFS.
"""
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "scripts/room"
spec = importlib.util.spec_from_file_location("ray_energy_under_test", TOOLS / "ray_energy.py")
ray = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ray)

ROOM = dict(dimensions=[7., 6., 3.2], listener=[3.4, 3., 1.2], speed_of_sound=344.02, minimum_distance=0.25,
            boundary_clearance=0.15, early_fade_seconds=[0.015, 0.025], late_predelay_seconds=0.018,
            sample_rate=48000)
VOLUME = 7 * 6 * 3.2
SURFACE = 2 * (7 * 6 + 7 * 3.2 + 6 * 3.2)
SOURCES = [{"azimuth": 30, "elevation": 0, "distance": 2.1}, {"azimuth": -120, "elevation": 10, "distance": 2.1}]


def sphere(count):
    index = np.arange(count) + 0.5
    z = 1 - 2 * index / count
    radius = np.sqrt(1 - z * z)
    angle = np.pi * (1 + 5 ** 0.5) * index
    return np.stack([radius * np.cos(angle), radius * np.sin(angle), z], axis=1)


DIRECTIONS = sphere(32)


def definition(**changes):
    late = dict(generator="ray-energy-v1", seed=3, duration_seconds=1.0, rays=40000, receiver_radius=0.3,
                histogram_seconds=0.002, source_positions=SOURCES,
                materials=dict(absorption=[[0.2] * 8] * 6, scattering=[[1.] * 8] * 6))
    late.update(changes)
    return late


def histogram(late, room=ROOM):
    config = ray.validate(late, room)
    positions = ray.source_positions(config, room, TOOLS)
    return ray.collect(config, room, DIRECTIONS, positions)[0]


def t30(result, band):
    return ray.decay_times(result.energy[:, band].sum(axis=0), result.width)["t30"]


def eyring(alpha):
    return 24 * math.log(10) * VOLUME / (ROOM["speed_of_sound"] * SURFACE * -math.log(1 - alpha))


class Physics(unittest.TestCase):
    def test_lossless_room_keeps_its_energy(self):
        result = histogram(definition(duration_seconds=0.2, rays=50000, histogram_seconds=0.005,
                                      materials=dict(absorption=[[0.] * 8] * 6, scattering=[[1.] * 8] * 6)))
        series = result.energy.sum(axis=(0, 1))
        established = series[6:]  # after the early fade and the first reflections
        early, late = established[:len(established) // 2].mean(), established[len(established) // 2:].mean()
        self.assertAlmostEqual(late / early, 1., delta=0.06)

    def test_diffuse_decay_follows_eyring_in_every_band(self):
        # Eyring omits the path-length variance, so it reads shorter as absorption grows (Kuttruff);
        # at α ≤ 0.3 that bias stays under 8 %.
        alpha = [0.1, 0.12, 0.15, 0.18, 0.2, 0.22, 0.25, 0.3]
        result = histogram(definition(rays=50000, materials=dict(absorption=[alpha] * 6, scattering=[[1.] * 8] * 6)))
        for band, value in enumerate(alpha):
            self.assertAlmostEqual(t30(result, band) / eyring(value), 1., delta=0.10, msg=f"band {band}")
        self.assertGreater(t30(result, 0), t30(result, 7))

    def test_bands_with_different_scattering_keep_their_own_paths(self):
        walls, slabs = [0.15] * 8, [0.7] * 8
        absorption = [walls, walls, walls, walls, slabs, slabs]
        mixed = histogram(definition(materials=dict(absorption=absorption,
                                                    scattering=[[0., .15, .3, .45, .55, .7, .85, 1.]] * 6)))
        specular = histogram(definition(materials=dict(absorption=absorption, scattering=[[0.] * 8] * 6)))
        diffuse = histogram(definition(seed=4, materials=dict(absorption=absorption, scattering=[[1.] * 8] * 6)))
        # Grazing specular rays between the weakly absorbing walls decay far more slowly.
        self.assertGreater(t30(specular, 0), 2.5 * t30(diffuse, 7))
        self.assertAlmostEqual(t30(mixed, 0) / t30(specular, 0), 1., delta=0.10)
        self.assertAlmostEqual(t30(mixed, 7) / t30(diffuse, 7), 1., delta=0.10)
        self.assertEqual(ray.scattering_groups(np.array([[0., 0., .5, .5, .5, 1., 1., 1.]] * 6)),
                         [[0, 1], [2, 3, 4], [5, 6, 7]])
        config = ray.validate(definition(materials=dict(absorption=absorption, scattering=[[0.] * 7 + [1.]] * 6)),
                              ROOM)
        position = ray.source_positions(config, ROOM, TOOLS)[0]
        with self.assertRaisesRegex(ray.RayEnergyError, "share its scattering"):
            ray.trace_source(config, ROOM, position, np.random.Generator(np.random.PCG64(1)), [0, 7])

    def test_diffuse_field_is_isotropic(self):
        result = histogram(definition(duration_seconds=0.4,
                                      materials=dict(absorption=[[0.05] * 8] * 6, scattering=[[1.] * 8] * 6)))
        share = result.energy[:, 0].sum(axis=1)
        share = share / share.sum()
        self.assertLess(share.std() / share.mean(), 0.08)

    def test_a_strongly_absorbing_front_wall_quietens_arrivals_from_the_front(self):
        absorption = [[0.1] * 8 for _ in range(6)]
        absorption[1] = [0.9] * 8  # east = the +X (front) wall
        result = histogram(definition(duration_seconds=0.4,
                                      materials=dict(absorption=absorption, scattering=[[1.] * 8] * 6)))
        energy = result.energy.sum(axis=(1, 2))
        front = energy[DIRECTIONS[:, 0] > 0.5].mean()
        back = energy[DIRECTIONS[:, 0] < -0.5].mean()
        self.assertLess(front / back, 0.7)


class Accounting(unittest.TestCase):
    def test_late_weight_complements_the_early_amplitude_window(self):
        early = ROOM["early_fade_seconds"]
        np.testing.assert_allclose(ray.late_weight(np.array([0., 0.015, 0.02, 0.025, 0.1]), early),
                                   [0., 0., 0.75, 1., 1.], atol=1e-15)

    def test_onset_account_closes_and_vanishes_when_predelay_precedes_the_fade(self):
        late = definition(duration_seconds=0.3, rays=20000)
        result = histogram(late)
        account = result.account()
        for band in range(8):
            total = account["window_suppressed"][band] + account["uncovered"][band] + account["histogram"][band]
            self.assertAlmostEqual(total / account["raw"][band], 1., delta=1e-12)
        self.assertLessEqual(account["direction_allocation_relative_error"], 1e-12)
        self.assertGreater(account["uncovered_onset_energy_ratio"], 0)
        for predelay in (0.015, 0.01):
            room = dict(ROOM, late_predelay_seconds=predelay)
            self.assertEqual(histogram(late, room).account()["uncovered"], [0.] * 8)

    def test_direct_sound_never_enters_the_account(self):
        result = histogram(definition(duration_seconds=0.2, rays=5000,
                                      materials=dict(absorption=[[1.] * 8] * 6, scattering=[[1.] * 8] * 6)))
        self.assertEqual(result.account()["raw"], [0.] * 8)

    def test_emission_follows_the_runtime_conversion_and_clamp(self):
        config = ray.validate(definition(source_positions=[
            {"azimuth": 90, "elevation": 0, "distance": 2.1},
            {"azimuth": 0, "elevation": 90, "distance": 5.},
            {"azimuth": 0, "elevation": 0, "distance": 0.1}]), ROOM)
        left, up, near = ray.source_positions(config, ROOM, TOOLS)
        np.testing.assert_allclose(left["direction"], [0, 1, 0], atol=1e-7)  # +Y is left
        self.assertFalse(left["constrained"])
        self.assertTrue(up["constrained"])
        self.assertAlmostEqual(up["distance"], 3.2 - 0.15 - 1.2)
        self.assertEqual((near["distance"], near["constrained"]), (0.25, True))
        rays = ray.emit(left["direction"], 20000, np.random.Generator(np.random.PCG64(1)))
        beam = rays @ -np.asarray(left["direction"])
        self.assertGreaterEqual(beam.min(), -1e-12)  # nothing toward the back hemisphere
        self.assertAlmostEqual(beam.mean(), 5 / 6, delta=0.005)  # E[cos θ] for density ∝ cos⁴ θ


class Tail(unittest.TestCase):
    WIDTH = 0.001

    def decay(self, count=600, slope=-120.):
        times = (np.arange(count) + 0.5) * self.WIDTH
        return 10 ** (slope * times / 10)

    @staticmethod
    def squares(energy, hits):
        """Per-bin squared contributions that give `hits` effective arrivals."""
        return np.divide(energy ** 2, hits, out=np.zeros_like(energy), where=hits > 0)

    def test_leading_silence_is_kept(self):
        energy = np.r_[np.zeros(5), self.decay(300)]
        hits = np.r_[np.zeros(5), np.full(300, 100.)]
        result, info = ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)
        self.assertEqual(info["status"], "measured")
        np.testing.assert_array_equal(result, energy)

    def test_a_short_sparse_dip_does_not_start_the_tail(self):
        energy, hits = self.decay(), np.full(600, 100.)
        hits[200:202] = 3
        self.assertEqual(ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)[1]["status"], "measured")

    def test_sustained_sparsity_merges_into_reliable_windows(self):
        energy, hits = self.decay(), np.full(600, 100.)
        hits[300:] = 4
        noisy = energy.copy()
        noisy[300:] *= np.random.Generator(np.random.PCG64(5)).uniform(0, 2, 300)
        result, info = ray.tail(noisy, self.squares(noisy, hits), self.WIDTH, 32)
        self.assertEqual(info["status"], "merged")
        self.assertAlmostEqual(info["start_seconds"], 0.3)
        self.assertGreater(info["windows"], 10)
        np.testing.assert_array_equal(result[:300], noisy[:300])
        ratio = result[300:] / energy[300:]
        self.assertTrue(np.all((ratio > 0.5) & (ratio < 2)))
        self.assertAlmostEqual(result[300:].sum() / noisy[300:].sum(), 1., delta=0.05)
        times = (np.arange(600) + 0.5) * self.WIDTH
        slope = np.polyfit(times[300:], 10 * np.log10(result[300:]), 1)[0]
        self.assertAlmostEqual(slope / -120., 1., delta=0.1)

    def test_long_windows_keep_an_exponential_decay(self):
        # Half an arrival per bin: windows span tens of bins, where placing a window mean at any one
        # time (its centre or its centroid) misreads the decay by several dB.
        energy, hits = self.decay(1000), np.full(1000, 100.)
        hits[300:] = 0.5
        result, info = ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)
        self.assertGreater(info["last_window_seconds"], 0.05)
        np.testing.assert_allclose(result, energy, rtol=1e-9)
        rising = np.r_[np.full(300, 1.), np.linspace(1., 2., 100)]
        hits = np.r_[np.full(300, 100.), np.full(100, 0.5)]
        flat, _ = ray.tail(rising, self.squares(rising, hits), self.WIDTH, 32)
        np.testing.assert_allclose(flat[300:], rising[300:].mean(), rtol=1e-12)  # no rising tail

    def test_a_short_last_window_joins_the_one_before(self):
        energy, hits = np.full(40, 1e-3), np.full(40, 100.)
        hits[20:] = 4  # windows of 8 bins reach 32 arrivals; the last 4 bins join the second window
        squares = self.squares(energy, hits)
        result, info = ray.tail(energy, squares, self.WIDTH, 32)
        self.assertEqual(info["status"], "merged")
        self.assertEqual(info["windows"], 2)
        self.assertAlmostEqual(info["last_window_seconds"], 0.012)
        self.assertAlmostEqual(result[20:].sum() / energy[20:].sum(), 1., delta=1e-12)

    def test_directions_with_different_seams_share_one_budget_and_follow_later_arrivals(self):
        i = np.arange(80)
        band = np.exp(-0.045 * i)
        share = np.interp(i, [20, 36], [0.6, 0.2])
        share[52:] = 0.5
        share[61] = 0.15
        energy = np.stack([share * band, (1 - share) * band])
        hits = np.stack([np.where(i < 20, 64., 24.), np.where(i < 44, 96., 24.)])
        hits[0, 61] = 64.
        squares = self.squares(energy, hits)
        # Disjoint directional arrivals. The pooled band is well sampled throughout;
        # freezing each share at its seam would spend 60% + 80% of the band.
        shape, info, starts = ray.windowed_tail(energy.sum(axis=0), squares.sum(axis=0), 0.002, 32)
        self.assertEqual(info["status"], "measured")
        result, seams, _ = ray.distribute_directions(energy, squares, hits, shape, starts, 32)
        np.testing.assert_array_equal(seams, [20, 44])
        np.testing.assert_allclose(result.sum(axis=0), band, rtol=1e-12, atol=0)
        np.testing.assert_array_equal(result[1, :44], energy[1, :44])
        # Both directions have 24/32 effective hits. Update from 20% toward the new 50%.
        self.assertAlmostEqual(result[0, 52] / band[52], 0.425)
        self.assertEqual(result[0, 61], energy[0, 61])  # a later well-supported observation
        self.assertAlmostEqual(result[0, -1] / band[-1], 0.5)

    def test_merged_windows_keep_reliable_direction_totals_and_silence(self):
        i = np.arange(150)
        band = self.decay(len(i))
        band[50:] *= np.resize([1.2, 0.8], 100)
        share = np.interp(i, [50, 100], [0.2, 0.35])
        remainder = (1 - share) * band
        split = np.interp(i, [60, 100], [0.25, 0.75])
        energy = np.stack([share * band, split * remainder, (1 - split) * remainder, np.zeros_like(band)])
        energy[:, -8:] = 0.
        counts = np.stack([np.full(len(i), 64.), np.where(i < 50, 64., 2.),
                           np.where(i < 50, 64., 1.), np.zeros(len(i))])
        squares = self.squares(energy, counts)
        shape, info, starts = ray.windowed_tail(energy.sum(axis=0), squares.sum(axis=0), self.WIDTH, 32)
        self.assertEqual(info["status"], "merged")
        result, _, _ = ray.distribute_directions(energy, squares, ray.effective_hits(energy, squares),
                                                shape, starts, 32)
        np.testing.assert_array_equal(result[:, :50], energy[:, :50])
        np.testing.assert_array_equal(result[3], 0.)
        np.testing.assert_array_equal(result[:, -8:], 0.)
        np.testing.assert_allclose(result.sum(axis=0), shape, rtol=1e-12, atol=0)
        np.testing.assert_allclose(np.add.reduceat(result[0], starts), np.add.reduceat(energy[0], starts),
                                   rtol=1e-12, atol=0)

    def test_single_supported_window_uses_the_measured_decline(self):
        energy = self.decay(90)
        hits = np.r_[np.full(70, 64.), np.full(20, 2.)]
        result, info = ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)
        self.assertEqual(info["windows"], 1)
        np.testing.assert_allclose(result, energy, rtol=1e-10, atol=0)

    def test_unresolved_residue_fades_only_when_its_energy_is_negligible(self):
        head = self.decay(64)
        hits = np.r_[np.full(64, 64.), 1., np.zeros(199)]
        energy = np.r_[head, head.sum() * 1e-8, np.zeros(199)]
        result, info = ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)
        self.assertEqual(info["status"], "tapered")
        self.assertEqual(info["effective_hits"], 1.)
        np.testing.assert_array_equal(result[:64], head)
        self.assertTrue(np.all(np.diff(result[64:70]) < 0))
        np.testing.assert_array_equal(result[69:], 0.)
        np.testing.assert_allclose(result[64:].sum(), energy[64], rtol=1e-12, atol=0)
        energy[64] = head.sum() * 1e-3
        with self.assertRaisesRegex(ray.RayEnergyError, "尾段采样不足.*rays"):
            ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)

    def test_a_dominant_last_arrival_invalidates_the_merged_window(self):
        energy = np.r_[np.ones(16), np.full(12, 0.01), 0.8]
        hits = np.r_[np.full(16, 64.), np.full(12, 8.), 1.]
        with self.assertRaisesRegex(ray.RayEnergyError, "尾段采样不足"):
            ray.tail(energy, self.squares(energy, hits), self.WIDTH, 32)

    def test_zero_and_unestablished_series(self):
        result, info = ray.tail(np.zeros(50), np.zeros(50), self.WIDTH, 32)
        self.assertEqual(info["status"], "zero")
        self.assertFalse(result.any())
        energy = self.decay(50)
        with self.assertRaisesRegex(ray.RayEnergyError, "没有连续"):
            ray.tail(energy, self.squares(energy, np.full(50, 10.)), self.WIDTH, 32)
        hits = np.full((1, 50), 10.)
        with self.assertRaisesRegex(ray.RayEnergyError, "没有连续"):
            ray.distribute_directions(energy[None, :], self.squares(energy[None, :], hits), hits,
                                      energy, np.arange(50), 32)



class ZonedSurfaces(unittest.TestCase):
    def setUp(self):
        if str(TOOLS) not in sys.path:
            sys.path.insert(0, str(TOOLS))
        from room_surfaces import SurfaceMap
        self.compile = SurfaceMap
        self.base = json.loads((ROOT / "tests/fixtures/room/synthetic/zoned-room.json").read_text())
        self.model = self.base["surface_model"]

    def test_shared_concave_boundaries_and_rejection(self):
        mapping = self.compile(self.model, self.base["dimensions"])
        for wall, (u, v), name in self.base["cases"]:
            points = np.zeros((1, 3))
            points[0, [1, 2]] = [u, v]  # shared cases on X-normal walls
            slot = mapping.bindings(np.array([wall]), points)[0]
            material = next(m for m in self.model["materials"] if m["id"] == name)
            np.testing.assert_array_equal(mapping.absorption[slot], material["absorption"])
        self.model["faces"][0]["regions"][0]["polygon"] = [[0, 0], [3, 2], [0, 2], [2, 0]]
        with self.assertRaises(ValueError):
            self.compile(self.model, self.base["dimensions"])

    def test_native_rate_pair_shares_the_audible_carrier(self):
        from scipy.signal import resample_poly, butter, sosfilt
        energy = np.ones((1, 1, 64))
        low = np.zeros((1, 16))
        high = np.zeros((1, 32))
        low[0, 8], high[0, 16] = 1., 1.
        def rng(seed):
            return np.random.default_rng(seed)
        a = ray.synthesize(energy, low, 48000, .256, .004, rng(4))[0]
        b = ray.synthesize(energy, high, 96000, .256, .004, rng(4), rng(9))[0]
        sos = butter(6, 12000, fs=48000, output='sos')
        mid = slice(500, -500)
        self.assertGreater(np.corrcoef(sosfilt(sos, a)[mid], sosfilt(sos, resample_poly(b, 1, 2))[mid])[0, 1], .999)

    def test_fan_rotation_conserves_energy_and_rotates_dispersion(self):
        material = self.model["faces"][0]["regions"][0]
        u = np.linspace(0., 1., 1001)
        for angle, spread, groove in [(0, 1, 2), (90, 2, 1)]:
            material["angle_deg"] = angle
            mapping = self.compile(self.model, self.base["dimensions"])
            slot = mapping.bindings(np.array([0]), np.array([[0., 2.5, 0.2]]))[0]
            incoming = np.zeros((len(u), 3))
            incoming[:, 0], incoming[:, groove] = -0.8, 0.6
            out = mapping.scatter_fan(incoming, np.full(len(u), slot), np.zeros(len(u), int),
                                      np.zeros(len(u), bool), u)
            np.testing.assert_allclose(np.linalg.norm(out, axis=1), 1., atol=1e-14)
            np.testing.assert_allclose(out[:, groove], 0.6, atol=1e-14)
            self.assertAlmostEqual(np.ptp(out[:, spread]), 1.6)
            self.assertGreaterEqual(out[:, 0].min(), 0.)


class Generator(unittest.TestCase):
    """End to end through generate-room.py on the synthetic room fixture."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.template = self.root / "template"
        shutil.copytree(ROOT / "tests/fixtures/room/synthetic", self.template)
        self.template_room = json.loads((self.template / "room.json").read_text())

    def tearDown(self):
        shutil.rmtree(self.root)

    def run_generator(self, name, late, *extra, surface_model=None, **room_changes):
        definition_file = self.root / f"{name}.json"
        room = {**self.template_room, **room_changes}
        content = dict(schema="mradm.room-definition", version=1, id="candidate", name="Candidate", late=late,
                       late_energy=0.2, **{key: room[key] for key in (
                           "dimensions", "listener", "speed_of_sound", "reference_distance", "minimum_distance",
                           "boundary_clearance", "reflection_order", "absorption", "early_fade_seconds",
                           "late_predelay_seconds")})
        if surface_model is not None:
            del content["absorption"]
            content.update(version=2, surface_model=surface_model, common_eq=[[0, 0], [100, -1], [1000, 0]])
        definition_file.write_text(json.dumps(content))
        output = self.root / name
        process = subprocess.run([sys.executable, "-B", str(TOOLS / "generate-room.py"), "--template-package",
                                  str(self.template / "room.json"), "--definition", str(definition_file),
                                  "--output", str(output), *extra], capture_output=True, text=True)
        return process, output

    def small(self, **changes):
        late = definition(duration_seconds=0.3, rays=20000, histogram_seconds=0.005, min_hits_per_bin=8)
        late.update(changes)
        return late

    def test_rebuild_is_byte_identical_for_any_job_count_and_seed_variation_is_statistical(self):
        # Three sources and two scattering groups: three or more parts per band, so the merge order
        # would change the rounding if --jobs changed it.
        late = self.small(source_positions=SOURCES + [{"azimuth": 170, "elevation": -20, "distance": 1.5}],
                          materials=dict(absorption=[[0.2] * 8] * 6, scattering=[[0.3] * 4 + [0.8] * 4] * 6))
        first, a = self.run_generator("a", late)
        second, b = self.run_generator("b", late, "--jobs", "3")
        other, c = self.run_generator("c", {**late, "seed": 11})
        for process in (first, second, other):
            self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual((a / "late.f32").read_bytes(), (b / "late.f32").read_bytes())
        self.assertEqual((a / "room.json").read_bytes(), (b / "room.json").read_bytes())
        manifest = json.loads((a / "room.json").read_text())
        late = manifest["assets"]["late"]
        self.assertEqual(late["shape"], [6, int(round(0.3 * 48000))])
        np.testing.assert_array_equal(np.fromfile(self.template / "late-directions.f64", "<f8"),
                                      np.fromfile(a / "late-directions.f64", "<f8"))
        field = np.fromfile(a / "late.f32", "<f4").reshape(late["shape"])
        self.assertAlmostEqual(float(np.mean(np.sum(field.astype(np.float64) ** 2, axis=1))), 1., delta=1e-5)
        summary = manifest["generation"]["ray_energy"]
        self.assertEqual(summary["resolved"]["rays"], 20000)
        for tail in summary["pooled_tail"]:
            self.assertLessEqual(tail["max_bin_energy_relative_error"], 1e-12)
        self.assertIn("seconds", json.loads((a / "generation-report.json").read_text()))
        variant = json.loads((c / "room.json").read_text())["generation"]["ray_energy"]
        for band in range(8):
            self.assertAlmostEqual(variant["late_t30_seconds"][band] / summary["late_t30_seconds"][band], 1.,
                                   delta=0.1)

    def test_zoned_package_shares_the_table_and_is_reproducible_across_jobs(self):
        model = json.loads((self.template / "zoned-room.json").read_text())["surface_model"]
        late = self.small()
        del late["materials"]
        one, a = self.run_generator("one", late, surface_model=model)
        two, b = self.run_generator("two", late, "--jobs", "2", surface_model=model)
        for process in (one, two):
            self.assertEqual(process.returncode, 0, process.stderr)
        manifest = json.loads((a / "room.json").read_text())
        self.assertEqual(manifest["kind"], "hybrid-shoebox-v2")
        self.assertEqual(manifest["surface_model"], model)
        self.assertNotIn("absorption", manifest)
        self.assertGreater(manifest["assets"]["eq"]["shape"][0], 1)
        self.assertEqual((a / "SHA256SUMS").read_bytes(), (b / "SHA256SUMS").read_bytes())
        for row in (a / "SHA256SUMS").read_text().splitlines():
            digest, name = row.split("  ")
            self.assertEqual(digest, hashlib.sha256((a / name).read_bytes()).hexdigest())
        duplicate, output = self.run_generator("duplicate", self.small(), surface_model=model)
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertFalse(output.exists())

    def test_missing_optional_hrir_leaves_the_destination_available_for_retry(self):
        (self.template / self.template_room["default_hrir"]).unlink()
        run, output = self.run_generator("retry", self.small(), "--include-hrir")
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(output.exists())
        run, output = self.run_generator("retry", self.small())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue((output / "room.json").exists())

    def test_invalid_definitions_fail_before_any_output_exists(self):
        cases = dict(
            material=self.small(materials=dict(absorption=[[1.5] * 8] * 6, scattering=[[1.] * 8] * 6)),
            rays=self.small(rays=10),
            unknown=self.small(rays_per_source=10),
            radius=self.small(receiver_radius=0.6),
            unestablished=self.small(min_hits_per_bin=100000))
        for name, late in cases.items():
            changes = dict(listener=[0.5, 3., 1.2]) if name == "radius" else {}
            process, output = self.run_generator(name, late, **changes)
            self.assertNotEqual(process.returncode, 0, name)
            self.assertIn("RayEnergyError" if name != "material" else "absorption", process.stderr, name)
            self.assertFalse(output.exists(), name)


if __name__ == "__main__":
    unittest.main()
