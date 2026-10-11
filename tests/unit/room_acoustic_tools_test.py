"""Independent invariants for room preparation and acoustic measurement tools."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts/room' / file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


generate = module('room_generator', 'generate-room.py')
acoustics = module('room_acoustics', 'check-acoustics.py')


class AcousticTools(unittest.TestCase):
    def test_common_orthogonal_mix_preserves_instantaneous_energy_and_omni_sum(self):
        rng = np.random.Generator(np.random.PCG64(81))
        original = rng.normal(size=(8, 913)) * np.exp(-np.arange(913) / 500.)
        weights = rng.normal(size=8)
        normalized = weights * np.sqrt(8) / np.linalg.norm(weights)
        result, error = generate.orthogonal_mix(original, weights)
        np.testing.assert_allclose(np.sum(result ** 2, axis=0), np.sum(original ** 2, axis=0), rtol=1e-13)
        np.testing.assert_allclose(np.sum(result, axis=0), normalized @ original, atol=1e-13)
        self.assertLess(error, 1e-13)
        self.assertFalse(np.allclose(np.sum(result ** 2, axis=1), np.sum(original ** 2, axis=1)))
        # There is no independent per-direction normalization or shortened tail.
        self.assertEqual(result.shape, original.shape)
        identity, _ = generate.orthogonal_mix(original, np.ones(8))
        np.testing.assert_array_equal(identity, original)

    def test_decay_adjustment_is_shared_by_every_direction(self):
        original = np.arange(4 * 517, dtype=float).reshape(4, 517) / 1024.
        first, _ = generate.orthogonal_mix(original, [1, 2, 3, 4])
        tilted, _ = generate.orthogonal_mix(original, [1, 2, 3, 4], -.4)
        np.testing.assert_allclose(tilted, first * np.exp(.4 * np.arange(517) / 48000.))
        for weights, rate in [([0]*4, 0), ([1]*3, 0), ([1, 2, 3, np.nan], 0), ([1]*4, np.inf)]:
            with self.assertRaises(ValueError):
                generate.orthogonal_mix(original, weights, rate)

    def test_schroeder_fit_has_known_decay_and_does_not_depend_on_gain(self):
        time = np.arange(48000, dtype=float) / 48000.
        response = np.cos(2*np.pi*2000*time) * np.exp(-np.log(1000)*time/.28)
        for interval in [-25, -35]:
            fit = acoustics.fit_decay(response, high=interval)
            self.assertAlmostEqual(fit['rt60_seconds'], .28, delta=.001)
            self.assertGreater(fit['fit_r2'], .999)
            quieter = acoustics.fit_decay(response * .0001, high=interval)
            self.assertAlmostEqual(quieter['rt60_seconds'], fit['rt60_seconds'], places=10)
        self.assertIsNone(acoustics.fit_decay(np.zeros(48000)))

    def test_operational_response_retains_absolute_gain(self):
        response = np.zeros(65536)
        response[937] = 1
        np.testing.assert_allclose(acoustics.frequency_levels(response), 0, atol=1e-12)
        np.testing.assert_allclose(acoustics.frequency_levels(response * 2), 20*np.log10(2), atol=1e-12)

    def test_masks_use_the_stricter_published_high_frequency_limit(self):
        nominal, lower, upper, fr_lower = acoustics.limits(100)
        self.assertEqual(nominal, .25)
        self.assertEqual(lower[0], None)
        self.assertAlmostEqual(upper[0], .55)
        self.assertAlmostEqual(upper[-1], .30)
        self.assertAlmostEqual(lower[-1], .15)
        self.assertAlmostEqual(fr_lower[-1], -6.)


if __name__ == '__main__':
    unittest.main()
