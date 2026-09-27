import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree

import numpy as np

from gain_trace_adm import gain_field, inspect_adm, object_fields
from make_calibration_suite import timecode_samples
from make_semantic_suite import FRAMES, timecode, two_object_dbmd, validate_probe_topology, write_riff
from run_semantic_suite import cached_reference
from score_semantic_pcm import score


class SemanticIdentityTest(unittest.TestCase):
    def test_dbmd_must_match_verified_topology(self):
        channels = 12
        fmt = struct.pack("<HHIIHH", 1, channels, 48000, 48000 * channels * 3, channels * 3, 24)
        chna = struct.pack("<HH", channels, channels) + b"".join(
            struct.pack("<H12s14s11sc", index + 1, f"ATU_{index + 1:08x}".encode(),
                        b"AT_00031001_01", b"AP_00031001", b"\0") for index in range(channels))
        parts = [(b"fmt ", fmt), (b"data", bytes(FRAMES * channels * 3)),
                 (b"chna", chna), (b"dbmd", b"verified-12-channel-payload")]
        validate_probe_topology(parts, 12, b"verified-12-channel-payload")
        stale = [(key, b"old-11-channel-payload" if key == b"dbmd" else value) for key, value in parts]
        with self.assertRaisesRegex(ValueError, "DBMD"):
            validate_probe_topology(stale, 12, b"verified-12-channel-payload")
        with self.assertRaisesRegex(ValueError, "PCM format"):
            validate_probe_topology(parts, 11, b"verified-12-channel-payload")
        bad_chna = bytearray(chna)
        struct.pack_into("<H", bad_chna, 4 + 40, 1)
        with self.assertRaisesRegex(ValueError, "exactly once"):
            validate_probe_topology([(key, bytes(bad_chna) if key == b"chna" else value) for key, value in parts],
                                    12, b"verified-12-channel-payload")

    def test_wrong_dbmd_cache_fails_before_using_renderer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "dbmd-12.bin").write_bytes(b"incorrect-11-channel-payload")
            (root / "dbmd-12-provenance.json").write_text(json.dumps({
                "channels": 12, "object_channels": [10, 11],
                "dbmd_sha256": hashlib.sha256(b"correct-12-channel-payload").hexdigest()}))
            with self.assertRaisesRegex(ValueError, "cached DBMD"):
                two_object_dbmd(root)

    def test_gain_units_and_omission(self):
        missing = gain_field(ElementTree.fromstring("<x/>"))
        explicit = gain_field(ElementTree.fromstring('<x><gain gainUnit="dB">0</gain></x>'))
        self.assertFalse(missing["present"])
        self.assertTrue(explicit["present"])
        self.assertEqual(missing["linear"], explicit["linear"])
        half = gain_field(ElementTree.fromstring('<x><gain gainUnit="dB">-6.020599913279624</gain></x>'))
        self.assertAlmostEqual(half["linear"], .5)
        for value in ("NaN", "-1", "INF"):
            with self.assertRaises(ValueError):
                gain_field(ElementTree.fromstring(f"<x><gain>{value}</gain></x>"))

    def test_zero_and_absent_time_are_distinct(self):
        absent = object_fields(ElementTree.fromstring('<audioObject audioObjectID="AO_1001"/>'))
        explicit = object_fields(ElementTree.fromstring(
            '<audioObject audioObjectID="AO_1001" start="00:00:00.00000"><mute>0</mute></audioObject>'))
        self.assertFalse(absent["start"]["present"])
        self.assertTrue(explicit["start"]["present"])
        self.assertFalse(absent["mute"]["present"])
        self.assertTrue(explicit["mute"]["present"])
        self.assertEqual(absent["start"]["samples"], explicit["start"]["samples"])

    def test_reference_precision_retains_sample_boundaries(self):
        for frame in range(4096):
            self.assertEqual(timecode_samples(timecode(frame)), frame)

    def test_final_bwf_retains_relative_and_absolute_times(self):
        xml = b'''<audioFormatExtended>
          <audioObject audioObjectID="AO_1001" start="00:00:01.00000">
            <gain gainUnit="dB">-6.020599913279624</gain><mute>1</mute>
            <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef></audioObject>
          <audioTrackFormat audioTrackFormatID="AT_00031001_01"><audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef></audioTrackFormat>
          <audioStreamFormat audioStreamFormatID="AS_00031001"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioStreamFormat>
          <audioChannelFormat audioChannelFormatID="AC_00031001" typeDefinition="Objects">
            <audioBlockFormat audioBlockFormatID="AB_00031001_00000001" rtime="00:00:00.01000">
              <cartesian>1</cartesian><position coordinate="X">0</position><position coordinate="Y">0</position><position coordinate="Z">0</position>
            </audioBlockFormat></audioChannelFormat></audioFormatExtended>'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.wav"
            chna = struct.pack("<HHH12s14s11sc", 1, 1, 1, b"ATU_00000001", b"AT_00031001_01", b"AP_00031001", b"\0")
            write_riff(path, [(b"fmt ", struct.pack("<HHIIHH", 1, 1, 48000, 144000, 3, 24)),
                              (b"axml", xml), (b"chna", chna), (b"data", bytes(48))])
            identity = inspect_adm(path)
            event = identity["objects"][0]["events"][0]
            self.assertEqual(event["relative_start_sample"], 480)
            self.assertEqual(event["start_sample"], 48480)
            self.assertIsNone(event["duration_samples"])
            self.assertFalse(event["gain_field"]["present"])
            self.assertAlmostEqual(identity["source_objects"][0]["gain"]["linear"], .5)

    def test_pruned_reference_requires_exact_pcm_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pcm = np.ones((12, 12), dtype="<f4")
            digest = hashlib.sha256(pcm.tobytes()).hexdigest()
            (root / "reference-result.json").write_text(json.dumps({"adm_sha256": "adm", "specification": {},
                "layouts": {"7.1.4": {"decoded_pcm_sha256": digest}}}))
            np.savez_compressed(root / "reference-pcm.npz", **{"7.1.4_pcm": pcm})
            self.assertTrue(np.array_equal(cached_reference(root, {"layout": "7.1.4"}, "adm"), pcm))
            pcm[0, 0] = 0
            np.savez_compressed(root / "reference-pcm.npz", **{"7.1.4_pcm": pcm})
            with self.assertRaises(ValueError):
                cached_reference(root, {"layout": "7.1.4"}, "adm")


class SemanticScoringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.x = np.random.default_rng(137).normal(0, .1, 240000)
        cls.x[192000:] = 0
        cls.y = np.zeros((240000, 12))
        cls.y[:, 2] = cls.x
        cls.case = {"signal_stop_sample": 192000, "identity": {"objects": [
            {"events": [{"relative_start_sample": 0, "size": 0}]}]}}

    def test_identical_passes(self):
        result, _ = score(self.x, self.y, self.y.copy(), self.case)
        self.assertTrue(result["passes"])

    def test_bed_lfe_requires_the_explicit_bound_source(self):
        reference = self.y.copy()
        reference[:, 3] = self.x * .1
        result, _ = score(self.x, reference, reference.copy(), self.case, expected_lfe=reference[:, 3])
        self.assertTrue(result["passes"])
        changed = reference.copy()
        changed[:, 3] += self.x * .001
        result, _ = score(self.x, reference, changed, self.case, expected_lfe=reference[:, 3])
        self.assertFalse(result["passes"])
        self.assertGreater(result["lfe_route_max_abs_error"], 1e-7)

    def test_effective_object_silence_is_a_failure_not_an_exception(self):
        result, _ = score(self.x, self.y, np.zeros_like(self.y), self.case)
        self.assertFalse(result["passes"])
        self.assertEqual(result["failure"], "unexpected_candidate_silence")

    def test_added_tail_is_not_scored_as_reference_silence(self):
        candidate = self.y.copy()
        candidate[193000, 2] = .01
        result, _ = score(self.x, self.y, candidate, self.case)
        self.assertFalse(result["passes"])
        self.assertFalse(result["tail"]["extra_tail_ok"])

    def test_single_frame_dropout_is_not_hidden_by_whole_file_average(self):
        candidate = self.y.copy()
        candidate[20000] = 0
        result, _ = score(self.x, self.y, candidate, self.case)
        self.assertFalse(result["passes"])
        self.assertEqual(result["unexpected_silent_frames"], 1)


if __name__ == "__main__":
    unittest.main()
