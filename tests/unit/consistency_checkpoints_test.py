#!/usr/bin/env python3
"""Protect the raw checkpoint comparator's evidence and missing-data handling."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "checkpoints", Path(__file__).resolve().parents[2] / "scripts/consistency/compare-checkpoints.py")
checkpoints = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checkpoints)


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.a, self.b = (Path(self.temp.name) / name for name in ("a", "b"))
        self.a.mkdir()
        self.b.mkdir()

    def test_signed_zero_and_first_ulp_are_preserved(self):
        (self.a / "sample.f32").write_bytes(struct.pack("<III", 0x80000000, 0x3F800000, 0x3F800000))
        (self.b / "sample.f32").write_bytes(struct.pack("<III", 0, 0x3F800001, 0x3F800000))
        row, = checkpoints.compare(self.a, self.b)
        self.assertEqual(row["different_words"], 2)
        self.assertEqual(row["first_word"], 0)
        self.assertEqual(row["bits"], ["0x80000000", "0x00000000"])

    def test_double_precision_is_not_narrowed(self):
        (self.a / "gain.f64").write_bytes(struct.pack("<Q", 0x3FF0000000000000))
        (self.b / "gain.f64").write_bytes(struct.pack("<Q", 0x3FF0000000000001))
        row, = checkpoints.compare(self.a, self.b)
        self.assertEqual(row["different_words"], 1)
        self.assertNotEqual(*row["values"])

    def test_missing_and_length_mismatch_are_not_equality(self):
        (self.a / "gain.i32").write_bytes(struct.pack("<ii", 1, 2))
        (self.b / "gain.i32").write_bytes(struct.pack("<i", 1))
        (self.a / "extra.f32").write_bytes(struct.pack("<f", 0))
        rows = checkpoints.compare(self.a, self.b)
        self.assertEqual([row["status"] for row in rows], ["missing", "different"])
        self.assertEqual(rows[1]["different_words"], 1)

    def test_empty_and_truncated_inputs_fail(self):
        with self.assertRaises(ValueError):
            checkpoints.compare(self.a, self.b)
        (self.a / "gain.f32").write_bytes(b"123")
        (self.b / "gain.f32").write_bytes(b"1234")
        with self.assertRaises(ValueError):
            checkpoints.compare(self.a, self.b)


if __name__ == "__main__":
    unittest.main()
