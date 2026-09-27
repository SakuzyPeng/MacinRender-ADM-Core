import unittest

import numpy as np

from analyze_bed_trace import link_input_pcm
from trace_bed_semantics import encode24, impulse_measurement, source_signal


class BedMeasurementTests(unittest.TestCase):
    def test_lfe_first_adapter_order_uses_actual_pcm_pointers(self):
        source = {"0x100": 0, "0x200": 1, "0x300": 2, "0x400": 3}
        consumer = [{"name": f"input_channel_{i}", "address": address}
                    for i, address in enumerate(("0x400", "0x100", "0x200", "0x300"))]
        self.assertEqual(link_input_pcm(source, consumer, 4), [3, 0, 1, 2])
        consumer[0]["address"] = "0x500"
        with self.assertRaisesRegex(ValueError, "does not alias"):
            link_input_pcm(source, consumer, 4)

    def test_mapping_and_polarity_are_measured_without_level_fitting(self):
        source, pulses = source_signal("impulses")
        matrix = np.zeros((10, 12))
        matrix[:8, :8] = np.eye(8)
        matrix[8, [8, 10]] = [.5, -.5]
        matrix[9, [9, 11]] = [.25, .75]
        result = impulse_measurement(source[:, :10] @ matrix, {"pulses": pulses})
        np.testing.assert_array_equal(result["matrix_input_by_output"], matrix)
        self.assertTrue(result["zero_delay_gain_prediction"]["all_samples_exact"])
        self.assertEqual(result["repeat_gain_max_abs"], 0)

    def test_delayed_or_filtered_energy_is_not_misreported_as_pure_gain(self):
        source, pulses = source_signal("impulses")
        delayed = source[:, :10].copy()
        delayed[1:] += .125 * source[:-1, :10]
        result = impulse_measurement(delayed, {"pulses": pulses})
        self.assertGreater(result["zero_delay_gain_prediction"]["max_abs_pcm"], .03)
        self.assertFalse(result["zero_delay_gain_prediction"]["all_samples_exact"])

    def test_signal_and_final_pcm_are_exact_and_object_stays_silent(self):
        for kind in ("impulses", "prbs"):
            source, _ = source_signal(kind)
            raw = np.frombuffer(encode24(source), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            integers = raw[:, 0] | raw[:, 1] << 8 | raw[:, 2] << 16
            integers = (integers ^ 0x800000) - 0x800000
            decoded = integers.reshape(source.shape) / (1 << 23)
            np.testing.assert_array_equal(decoded, source)
            self.assertFalse(np.any(decoded[:, 10]))


if __name__ == "__main__":
    unittest.main()
