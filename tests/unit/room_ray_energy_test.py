#!/usr/bin/env python3
"""Self-tests of the ray-energy-v1 late-field generator (scripts/room/ray_energy.py).

Covers docs/architecture/ROOM_RAY_ENERGY_LATE_FIELD.md, "生成器自测": conservation, diffuse
decay against Eyring, scattering groups, direction conventions, the onset energy account,
determinism, the tail rules and the error paths. Needs numpy and scipy; no build, no LFS.
"""
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

    def test_leading_silence_is_kept(self):
        energy = np.r_[np.zeros(5), self.decay(300)]
        hits = np.r_[np.zeros(5), np.full(300, 100.)]
        result, info = ray.tail(energy, hits, self.WIDTH, 32)
        self.assertEqual(info["status"], "measured")
        np.testing.assert_array_equal(result, energy)

    def test_a_short_sparse_dip_does_not_start_the_tail(self):
        energy, hits = self.decay(), np.full(600, 100.)
        hits[200:202] = 3
        self.assertEqual(ray.tail(energy, hits, self.WIDTH, 32)[1]["status"], "measured")

    def test_sustained_sparsity_extrapolates_along_the_fitted_decay(self):
        energy, hits = self.decay(), np.full(600, 100.)
        hits[300:] = 5
        noisy = energy.copy()
        noisy[300:] *= np.random.Generator(np.random.PCG64(5)).uniform(0, 3, 300)
        result, info = ray.tail(noisy, hits, self.WIDTH, 32)
        self.assertEqual(info["status"], "extrapolated")
        self.assertAlmostEqual(info["start_seconds"], 0.3)
        self.assertAlmostEqual(info["slope_db_per_second"] / -120., 1., delta=0.03)
        np.testing.assert_array_equal(result[:300], noisy[:300])
        np.testing.assert_allclose(result[300:] / energy[300:], 1., rtol=0.08)

    def test_zero_unestablished_and_unfit_series(self):
        result, info = ray.tail(np.zeros(50), np.zeros(50), self.WIDTH, 32)
        self.assertEqual(info["status"], "zero")
        self.assertFalse(result.any())
        with self.assertRaisesRegex(ray.RayEnergyError, "没有连续"):
            ray.tail(self.decay(50), np.full(50, 10.), self.WIDTH, 32)
        hits = np.full(600, 100.)
        hits[8:] = 2  # sparse long before -25 dB
        with self.assertRaisesRegex(ray.RayEnergyError, "不足以拟合"):
            ray.tail(self.decay(), hits, self.WIDTH, 32)


class Generator(unittest.TestCase):
    """End to end through generate-room.py on the synthetic room fixture."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.template = self.root / "template"
        shutil.copytree(ROOT / "tests/fixtures/room/synthetic", self.template)
        self.template_room = json.loads((self.template / "room.json").read_text())

    def tearDown(self):
        shutil.rmtree(self.root)

    def run_generator(self, name, late, *extra, **room_changes):
        definition_file = self.root / f"{name}.json"
        room = {**self.template_room, **room_changes}
        content = dict(schema="mradm.room-definition", version=1, id="candidate", name="Candidate", late=late,
                       late_energy=0.2, **{key: room[key] for key in (
                           "dimensions", "listener", "speed_of_sound", "reference_distance", "minimum_distance",
                           "boundary_clearance", "reflection_order", "absorption", "early_fade_seconds",
                           "late_predelay_seconds")})
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
        first, a = self.run_generator("a", self.small())
        second, b = self.run_generator("b", self.small(), "--jobs", "2")
        other, c = self.run_generator("c", self.small(seed=11))
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
        self.assertIn("seconds", json.loads((a / "generation-report.json").read_text()))
        variant = json.loads((c / "room.json").read_text())["generation"]["ray_energy"]
        for band in range(8):
            self.assertAlmostEqual(variant["late_t30_seconds"][band] / summary["late_t30_seconds"][band], 1.,
                                   delta=0.1)

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
