"""Tail scoring regressions: measured quiet tails and additional decay."""

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent


class TailScoreTest(unittest.TestCase):
    def score(self, additional_tail=False, bad_curve=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            power = np.zeros((25, 12))
            power[:, 0] = 1
            covariance = np.zeros((25, 12, 12), dtype=np.complex64)
            covariance[:, 0, 0] = 1
            archive = root / "spectra.npz"
            np.savez(archive, ref_band_power=power, test_band_power=power,
                     ref_covariance=covariance, test_covariance=covariance)
            expected = {"label": "small_measurable_size", "xyz": [0, 1, 0], "size": .0022,
                        "total_power": 1, "normalized_energy_share": [1] + [0] * 11,
                        "speaker_energy": [1] + [0] * 11, "spectrum_key": "ref",
                        "tail_decay_db_1ms": [0, -10, -120], "tail_measurable_1ms": [True, True, False],
                        "tail_total_power_ratio": 3.66e-12,
                        "tail_cumulative_power_ratio_1ms": [3.66e-12, 3.66e-13, 0],
                        "tail_noise_energy_ratio_1ms": [5e-13, 2e-13, 1e-13]}
            actual = copy.deepcopy(expected)
            actual["spectrum_key"] = "test"
            actual["tail_total_power_ratio"] = 3.65e-12
            if additional_tail:
                actual["tail_cumulative_power_ratio_1ms"][-1] = 3e-13
            if bad_curve:
                actual["tail_decay_db_1ms"][1] = -8
            field = {"case_id": "tail_floor", "adm_sha256": "fixture", "spectral_archive": str(archive),
                     "layouts": {"7.1.4": {"objects": [expected]}},
                     "candidates": {"7.1.4": {"objects": [actual]}}}
            source, target = root / "field.json", root / "score.json"
            source.write_text(json.dumps(field))
            subprocess.run([sys.executable, str(HERE / "score_size_field.py"), "--field-report", str(source),
                            "--output", str(target)], check=True, stdout=subprocess.DEVNULL)
            return json.loads(target.read_text())["layouts"]["7.1.4"]["objects"][0]

    def test_quiet_reference_above_noise_is_measured(self):
        result = self.score()
        self.assertTrue(result["passes"])
        self.assertIsNotNone(result["tail_total_power_error_db"])

    def test_extra_tail_is_rejected(self):
        result = self.score(additional_tail=True)
        self.assertFalse(result["passes"])
        self.assertFalse(result["extra_tail_below_reference_floor_ok"])

    def test_measurable_curve_threshold_is_enforced(self):
        self.assertFalse(self.score(bad_curve=True)["passes"])


if __name__ == "__main__":
    unittest.main()
