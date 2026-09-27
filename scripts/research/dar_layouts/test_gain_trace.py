"""Small regressions from real, unperturbed Renderer capture, plus input guards."""

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from gain_trace_adm import inspect_adm
from measure_gain_trace import floats, predict_omo, size_mix
from run_gain_probe import build_batch

HERE = Path(__file__).resolve().parent


class GainTraceTest(unittest.TestCase):
    def setUp(self):
        self.fixture = json.loads((HERE / "fixtures/omo_gain_mix.json").read_text())

    def test_captured_gain_transform(self):
        row = self.fixture["gain_transform"]
        dry, gains = size_mix(row["size_raw_gains"], row["omo_size"])
        np.testing.assert_allclose(gains, row["size_mix_gains"], rtol=0, atol=1e-7)
        self.assertLess(abs(dry - row["dry_gain"]), 1e-7)

    def test_captured_mix_with_signed_filter_branches(self):
        dry, wet, group = predict_omo(self.fixture["record"])
        self.assertEqual(group, 3)
        np.testing.assert_allclose(dry, floats(bytes.fromhex(self.fixture["expected"]["direct"])), rtol=0, atol=2e-8)
        expected = np.stack([floats(bytes.fromhex(value)) for value in self.fixture["expected"]["branches"]])
        np.testing.assert_allclose(wet, expected, rtol=0, atol=2e-8)

    def test_missing_filtered_pcm_is_not_silently_substituted(self):
        record = self.fixture["record"]
        record["regions"] = [item for item in record["regions"] if not item["name"].startswith("filtered_pcm_")]
        with self.assertRaisesRegex(ValueError, "missing live filtered PCM"):
            predict_omo(record)

    def test_size_zero_and_full_size_dry_path(self):
        dry, wet = size_mix(np.ones(11), 0)
        self.assertEqual(dry, 1)
        self.assertFalse(np.any(wet))
        dry, _ = size_mix(np.ones(11), 1)
        self.assertLess(dry, 1e-12)

    def test_encoded_or_non_adm_input_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "not-adm.wav"
            path.write_bytes(b"not a RIFF ADM")
            with self.assertRaisesRegex(ValueError, "RIFF ADM"):
                inspect_adm(path)

    def test_invalid_batch_rejected_before_generator(self):
        for item in ({"sample_rate": 44100, "cases": []},
                     {"cases": [{"id": "nan", "xyz": [0, 0, 0], "size": float("nan"), "start_sample": 0}]},
                     {"cases": [{"id": "late", "xyz": [0, 0, 0], "size": 0, "start_sample": 1}]}):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / "cases.json"
                path.write_text(json.dumps(item))
                with self.assertRaises(ValueError):
                    build_batch(path, root / "probe")
                self.assertFalse((root / "probe").exists())


if __name__ == "__main__":
    unittest.main()
