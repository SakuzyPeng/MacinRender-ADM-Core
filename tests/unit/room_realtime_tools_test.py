#!/usr/bin/env python3
"""The rounding gate rejects altered timing, tails, silence and nonfinite PCM."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


compare = module('compare', ROOT / 'scripts/room/compare-realtime.py')
benchmark = module('benchmark', ROOT / 'scripts/room/benchmark-windows.py')


class Gates(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def replay(self, name, values, **changes):
        root = self.root / name
        root.mkdir()
        payload = struct.pack('<' + str(len(values)) + 'f', *values)
        (root / 'session.f32').write_bytes(payload)
        manifest = dict(schema='mradm.room-replay', version=1, processing_policy='room-realtime-v2',
                        frames=len(values)//2, latency_frames=256, tail_frames=10,
                        room_hash='same', hrir_hash='same', prepared=dict(short_taps=512), lfe=dict(taps=256),
                        files={'session.f32': dict(samples=len(values), sha256=hashlib.sha256(payload).hexdigest())})
        manifest.update(changes)
        path = root / 'manifest.json'
        path.write_text(json.dumps(manifest))
        return path

    def test_rounding_is_bounded_without_gain_fit(self):
        reference = self.replay('a', [0., 0.2, -0.8, 0.])
        self.assertTrue(compare.compare(reference, self.replay('b', [0., 0.200001, -0.8, 0.]))['passed'])
        self.assertFalse(compare.compare(reference, self.replay('c', [0., 0.2001, -0.8004, 0.]))['passed'])

    def test_silence_tail_frames_and_exact_cannot_be_relaxed(self):
        reference = self.replay('a', [0.] * 20)
        self.assertTrue(compare.compare(reference, self.replay('b', [0.] * 20))['passed'])
        self.assertFalse(compare.compare(reference, self.replay('c', [0.] * 19 + [1e-20]))['passed'])
        for name, changes in [('tail', dict(tail_frames=9)), ('latency', dict(latency_frames=255)),
                              ('frames', dict(frames=9)), ('exact', dict(processing_policy='room-exact-v1'))]:
            with self.assertRaises(ValueError):
                compare.compare(reference, self.replay(name, [0.] * 20, **changes))

    def test_nonfinite_and_tampered_payloads_fail(self):
        reference = self.replay('a', [0., 1.])
        for i, value in enumerate([float('nan'), float('inf'), -float('inf')]):
            with self.assertRaises(ValueError):
                compare.compare(reference, self.replay(str(i), [0., value]))
        candidate = self.replay('changed', [0., 1.])
        (candidate.parent/'session.f32').write_bytes(struct.pack('<2f', 0., 0.5))
        with self.assertRaises(ValueError):
            compare.compare(reference, candidate)

    def test_deadline_gate_requires_all_six_rounds_and_zero_misses(self):
        cases = [dict(variant='candidate', scenario=scenario, repetition=repetition,
                      host_before=dict(background_cpu_percent=[8.4, 4.0, 6.5]),
                      result=dict(blocks=22500, deadline_misses=0))
                 for scenario in ['ramps', 'distance-ramps'] for repetition in [1, 2, 3]]
        self.assertTrue(benchmark.acceptance(cases)['passed'])
        self.assertFalse(benchmark.acceptance([])['passed'])
        self.assertFalse(benchmark.acceptance(cases[:-1])['passed'])
        duplicate = copy.deepcopy(cases); duplicate[-1] = duplicate[0]
        self.assertFalse(benchmark.acceptance(duplicate)['passed'])
        changed = copy.deepcopy(cases); changed[-1]['result']['deadline_misses'] = 1
        self.assertFalse(benchmark.acceptance(changed)['passed'])
        # Legacy idle flags and background CPU samples cannot overrule measured deadlines.
        legacy = copy.deepcopy(cases)
        for case in legacy:
            case['idle_before'] = dict(eligible=False)
            del case['host_before']
        self.assertTrue(benchmark.acceptance(legacy)['passed'])
        legacy[-1]['result']['deadline_misses'] = 1
        self.assertFalse(benchmark.acceptance(legacy)['passed'])


if __name__ == '__main__':
    unittest.main()
