#!/usr/bin/env python3
"""Reject misleading device evidence and preserve the simulated source clock."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('device_probe', ROOT / 'scripts/room/device-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class DeviceProbeTests(unittest.TestCase):
    def test_slow_sender_cannot_pass_by_applying_every_received_packet(self):
        report = {'requested_seconds': 1800, 'poses_applied': 113400,
                  'osc': {'pose_packets': 113400, 'rejected_packets': 0, 'missing_tx_packets': 0}}
        self.assertFalse(probe.control_acceptance(report)['passed'])
        report['poses_applied'] = report['osc']['pose_packets'] = 180000
        self.assertTrue(probe.control_acceptance(report)['passed'])
        report['poses_applied'] = 178000
        self.assertFalse(probe.control_acceptance(report)['passed'])
        report['poses_applied'] = 180000
        report['osc']['missing_tx_packets'] = 1
        self.assertFalse(probe.control_acceptance(report)['passed'])

    def test_source_clock_advances_despite_equal_host_timestamps(self):
        # Locate the logical sample clock relative to the fixed-size OSC tail;
        # equal host timestamps must never create duplicate source timestamps.
        first, second = probe.pose_packet(1, 17), probe.pose_packet(2, 17)
        sample_offset = len(first) - struct.calcsize('>Q Q 3f')
        self.assertEqual(struct.unpack_from('>Q', first, sample_offset)[0], 0)
        self.assertEqual(struct.unpack_from('>Q', second, sample_offset)[0], 10)
        self.assertNotEqual(first, second)

    def test_memory_plateau_remains_distinct_from_an_earlier_step(self):
        rows = [{'seconds': n, 'rss_bytes': 100 if n < 1000 else 200} for n in range(1800)]
        result = probe.summarize_memory(rows)
        self.assertGreater(result['rss_slope_bytes_per_minute'], 0)
        self.assertEqual(result['final_windows']['10_minutes']['rss_slope_bytes_per_minute'], 0)
        self.assertEqual(result['final_windows']['5_minutes']['rss_min_bytes'], 200)
        self.assertFalse(probe.summarize_memory(rows[:10])['evaluated'])


if __name__ == '__main__':
    unittest.main()
