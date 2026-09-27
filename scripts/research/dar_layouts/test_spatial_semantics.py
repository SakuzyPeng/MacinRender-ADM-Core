import json
import tempfile
import unittest
from pathlib import Path

from analyze_spatial_trace import analyze

HERE = Path(__file__).resolve().parent


class SpatialCaptureTest(unittest.TestCase):
    def make_fixture(self, directory):
        trace = json.loads((HERE / "fixtures/spatial_semantics.json").read_text())
        (directory / "trace.json").write_text(json.dumps(trace))
        summary = {"success": True, "state_restored": True, "hook_restored": True,
                   "pcm_comparison": {"all_samples_exact": True}, "adm_sha256": "captured-fixture",
                   "layout": "9.1.6"}
        (directory / "summary.json").write_text(json.dumps(summary))
        return summary

    def test_real_parser_dispatch_distinguishes_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_fixture(root)
            result = analyze(root)
            fields = {item["element"]: item for item in result["field_dispatch"]}
            self.assertTrue(fields["diffuse"]["has_delegate"])
            self.assertEqual(fields["diffuse"]["converter_vtable"], "0x103d365d8")
            self.assertFalse(fields["objectDivergence"]["has_delegate"])
            self.assertIsNone(fields["objectDivergence"]["converter_vtable"])

    def test_real_diffuse_storage_and_downstream_clear(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_fixture(root)
            result = analyze(root)
            self.assertEqual({item["stored_bool_offset44"] for item in result["parsed_diffuse"]}, {0, 1})
            self.assertTrue(all(item["presence_mask_offset22"] & 0x200 for item in result["parsed_diffuse"]))
            self.assertTrue(result["omo_metadata"])
            self.assertTrue(all(item["direct_diffuse_bool"] == 0 and item["direct_size"] == 0
                                for item in result["omo_metadata"]))

    def test_unverified_capture_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            summary = self.make_fixture(root)
            summary["pcm_comparison"]["all_samples_exact"] = False
            (root / "summary.json").write_text(json.dumps(summary))
            with self.assertRaises(ValueError):
                analyze(root)


if __name__ == "__main__":
    unittest.main()
