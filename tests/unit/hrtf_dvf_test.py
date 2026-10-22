#!/usr/bin/env python3
"""Physical limits, independent partial waves, parallax geometry and SOFA phase."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import h5py
import numpy as np
from scipy.special import eval_legendre, spherical_jn, spherical_yn

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "scripts/research/hrtf_dvf/dvf.py"
spec = importlib.util.spec_from_file_location("dvf", MODULE)
dvf = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = dvf
spec.loader.exec_module(dvf)
DIRECTIONS = np.array([[1, 0, 0], [0, 1, 0], [-1, 0, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float)


def independent_sphere(frequencies, directions, radius, distance, sound_speed):
    """Incident + scattered partial waves, independent of the scaled recurrence."""
    cosines = directions @ np.array([[0, 1, 0], [0, -1, 0]], float).T
    result = np.empty((*cosines.shape, len(frequencies)), complex)
    for i, frequency in enumerate(frequencies):
        ka = 2*np.pi*frequency*radius/sound_speed
        kr = ka*distance/radius
        q = radius/distance
        order = max(int(np.ceil(ka + 4*np.cbrt(ka) + 36)),
                    int(np.ceil(np.log(1e-14*(1-q))/np.log(q))))
        n = np.arange(order + 1)
        ha = spherical_jn(n, ka) - 1j*spherical_yn(n, ka)
        derivative = spherical_jn(n, ka, True) - 1j*spherical_yn(n, ka, True)
        hr = spherical_jn(n, kr) - 1j*spherical_yn(n, kr)
        surface = spherical_jn(n, ka) - spherical_jn(n, ka, True)*ha/derivative
        angular = eval_legendre(n, cosines[:, :, None])
        result[:, :, i] = -1j*kr*np.exp(1j*kr)*np.sum((2*n + 1)*hr*surface*angular, axis=-1)
    return result


class DistanceVariation(unittest.TestCase):
    def test_independent_scattering_solution_and_order_convergence(self):
        f = np.array([20, 100, 1000, 8000, 24000, 48000])
        for radius, distance in [(.04, .15), (.0875, .2), (.12, .2), (.0875, 1.5), (.12, 100)]:
            model = dvf.HeadModel(radius_m=radius)
            expected = independent_sphere(f, DIRECTIONS, radius, distance, model.sound_speed_m_s)
            actual = dvf.sphere_response(f, DIRECTIONS, distance, model)
            np.testing.assert_allclose(actual, expected, rtol=2e-9, atol=2e-10)
            np.testing.assert_allclose(actual, dvf.sphere_response(f, DIRECTIONS, distance, model, extra_order=24),
                                       rtol=2e-11, atol=2e-11)

    def test_dc_closed_form_and_small_frequency_continuity(self):
        for model, distance in [(dvf.HeadModel(), .2), (dvf.HeadModel(radius_m=.12), .15)]:
            q = model.radius_m/distance
            expected_near = 2/(1-q) + np.log1p(-q)/q
            expected_far = 2/(1+q) - np.log1p(q)/q
            response = dvf.sphere_response([0, 1e-8], DIRECTIONS[[1]], distance, model)[0]
            np.testing.assert_allclose(response[:, 0], [expected_near, expected_far], rtol=1e-12)
            np.testing.assert_allclose(response[:, 1], response[:, 0], atol=1e-9)

    def test_identity_composition_and_bilateral_symmetry(self):
        f = [0, 100, 500, 2000, 16000]
        identity = dvf.distance_variation(f, DIRECTIONS, 1.5)
        np.testing.assert_array_equal(identity, 1)
        step1 = dvf.distance_variation(f, DIRECTIONS, .5, 1.5)
        step2 = dvf.distance_variation(f, DIRECTIONS, .2, .5)
        direct = dvf.distance_variation(f, DIRECTIONS, .2, 1.5)
        np.testing.assert_allclose(step1*step2, direct, rtol=1e-13)
        np.testing.assert_allclose(direct[1, 0], direct[3, 1], atol=1e-13)
        self.assertGreater(abs(direct[1, 0, 1]), 1)
        self.assertLess(abs(direct[1, 1, 1]), 1)

    def test_finite_reference_parallax_has_matching_ear_rays(self):
        model = dvf.HeadModel(left_azimuth_deg=100, right_azimuth_deg=-95, right_elevation_deg=3)
        directions = dvf.spherical_directions([[0, 0], [40, 20], [200, -30], [0, 90]])
        queries = dvf.parallax_directions(directions, .2, 1.5, model)
        ears = model.radius_m*model.ears
        a = .2*directions[:, None, :] - ears
        b = 1.5*queries - ears
        a /= np.linalg.norm(a, axis=-1, keepdims=True)
        b /= np.linalg.norm(b, axis=-1, keepdims=True)
        np.testing.assert_allclose(a, b, atol=1e-13)
        np.testing.assert_allclose(np.linalg.norm(queries, axis=-1), 1, atol=1e-13)
        same = dvf.parallax_directions(directions, .2, .2, model)
        np.testing.assert_allclose(same, np.broadcast_to(directions[:, None], same.shape), atol=1e-13)

    def test_parallax_reproduces_spherical_head_and_preserves_shared_equalization(self):
        f = np.array([0, 100, 500, 2000, 8000, 16000])
        reference = dvf.sphere_response(f, DIRECTIONS, 1.5)
        eq = np.exp(-2j*np.pi*f*42/48000) * np.array([.5, .8, 1.2, .4, .3, .2])
        synth = dvf.Synthesizer(f, DIRECTIONS, reference*eq, 1.5)
        expected = dvf.sphere_response(f, DIRECTIONS, .2)*eq
        np.testing.assert_allclose(synth.at(.2), expected, atol=1e-12)
        np.testing.assert_array_equal(synth.at(1.5), reference*eq)

    def test_target_listener_features_survive_and_silence_stays_silent(self):
        f = [100, 200, 300]
        sphere = dvf.sphere_response(f, DIRECTIONS, 1.5)
        features = np.array([[1, .01, 1], [1, 1, .05]])
        synth = dvf.Synthesizer(f, DIRECTIONS, sphere*features, 1.5)
        np.testing.assert_allclose(synth.at(.2)/dvf.sphere_response(f, DIRECTIONS, .2),
                                   np.broadcast_to(features, sphere.shape), atol=1e-12)
        zeros = dvf.Synthesizer(f, DIRECTIONS, np.zeros_like(sphere), 1.5)
        np.testing.assert_array_equal(zeros.at(.2), 0)

    def test_hybrid_preserves_low_frequency_cues_and_uses_high_frequency_parallax(self):
        f = np.array([0, 100, 1500, 2000, 3000, 4000, 8000, 16000])
        reference = dvf.sphere_response(f, DIRECTIONS, 1.5)
        colour = np.exp(DIRECTIONS[:, 0, None, None] * (.2 + 1j*f/30000))
        synth = dvf.Synthesizer(f, DIRECTIONS, reference*colour, 1.5)
        hybrid, sphere, parallax = synth.at(.2), synth.at(.2, "sphere"), synth.at(.2, "parallax")
        np.testing.assert_array_equal(hybrid[..., :4], sphere[..., :4])
        np.testing.assert_array_equal(hybrid[..., 5:], parallax[..., 5:])
        self.assertGreater(np.max(abs(sphere-parallax)), .01)

    def test_triangle_phase_interpolation_avoids_wrap_cancellation(self):
        mesh = dvf.DirectionMesh(DIRECTIONS)
        values = np.ones((6, 1), complex)
        values[0], values[1] = np.exp(1j*np.radians([179, -179]))[:, None]
        result = mesh.interpolate(values, np.array([[1, 1, 0]])/np.sqrt(2))
        np.testing.assert_allclose(result, -1, atol=1e-12)
        np.testing.assert_array_equal(mesh.interpolate(values, DIRECTIONS), values)
        with self.assertRaises(ValueError):
            dvf.DirectionMesh(dvf.spherical_directions([[0, 0], [90, 0], [180, 0], [270, 0], [0, 90]]))

    def test_reject_invalid_geometry_frequencies_and_parameters(self):
        for changes in [dict(radius_m=0), dict(radius_m=np.nan), dict(sound_speed_m_s=np.inf),
                        dict(left_azimuth_deg=-90), dict(right_elevation_deg=91)]:
            with self.assertRaises(ValueError):
                dvf.HeadModel(**changes)
        for distance in [.1, np.nan, np.inf, 101]:
            with self.assertRaises(ValueError):
                dvf.distance_variation([100], DIRECTIONS, distance)
        for f in [[-1], [np.nan], [500, 100], [100, 100], [48001], []]:
            with self.assertRaises(ValueError):
                dvf.sphere_response(f, DIRECTIONS, .2)

    def test_hrir_reader_retains_delay_phase_and_all_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"input.sofa"
            ir = np.zeros((6, 2, 32))
            ir[:, 0, 2], ir[:, 1, 31] = .8, .6
            with h5py.File(path, "w") as file:
                file.attrs["SOFAConventions"], file.attrs["DataType"] = "SimpleFreeFieldHRIR", "FIR"
                file.create_dataset("Data.IR", data=ir)
                file.create_dataset("Data.SamplingRate", data=[48000])
                source = file.create_dataset("SourcePosition", data=DIRECTIONS*1.5)
                source.attrs["Type"], source.attrs["Units"] = "cartesian", "metre"
                file.create_dataset("Data.Delay", data=[[-.25, 2.5]])
            f, directions, h, radius, meta = dvf.load_hrir(path, fft_size=64)
            expected = np.array([.8, .6])[:, None]*np.exp(-2j*np.pi*f*np.array([1.75, 33.5])[:, None]/48000)
            np.testing.assert_allclose(h, np.broadcast_to(expected, h.shape), atol=1e-13)
            np.testing.assert_array_equal(directions, DIRECTIONS)
            self.assertEqual(radius, 1.5)
            self.assertTrue(meta["data_delay_applied"])
            with self.assertRaises(ValueError):
                dvf.load_hrir(path, fft_size=16)
            output = Path(tmp)/"synthesized.npz"
            subprocess.run([sys.executable, "-B", str(MODULE), "--source", str(path), "--fft-size", "64",
                            "--distances", ".2", "1.5", "--output", str(output)], check=True, capture_output=True)
            with np.load(output, allow_pickle=False) as archive:
                metadata = json.loads(str(archive["metadata_json"]))
                self.assertEqual(metadata["mode"], "hybrid")
                self.assertEqual(metadata["reference_distance_m"], 1.5)
                np.testing.assert_allclose(archive["response"][1], h, rtol=1e-7, atol=1e-7)
                self.assertTrue(np.isfinite(archive["response"]).all())

    def test_cli_archive_is_portable_and_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/"model.npz"
            command = [sys.executable, "-B", str(MODULE), "--output", str(output), "--distances", ".2", "1.5"]
            subprocess.run(command, check=True, capture_output=True)
            original = output.read_bytes()
            with np.load(output, allow_pickle=False) as data:
                metadata = json.loads(str(data["metadata_json"]))
                self.assertEqual(metadata["response_kind"], "dvf")
                self.assertNotIn("source", metadata)
                np.testing.assert_array_equal(data["response"][1], 1)
                self.assertTrue(np.isfinite(data["response"]).all())
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(original, output.read_bytes())
            invalid = subprocess.run([sys.executable, "-B", str(MODULE), "--output", str(Path(tmp)/"invalid.npz"),
                                      "--mode", "hybrid"], capture_output=True)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn(b"require a target listener", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
