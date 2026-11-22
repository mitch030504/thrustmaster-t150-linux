import hashlib
from pathlib import Path
import runpy
import tempfile
import unittest

panel = runpy.run_path(str(Path(__file__).resolve().parents[1] / "files/usr/bin/t150-control"))


class PanelTests(unittest.TestCase):
    def test_profiles_round_trip_and_reject_out_of_range(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "profiles" / "track.json"
            limits = panel["LIMITS"]
            settings = {key: low for key, (low, _) in limits.items()}
            panel["save_profile"](path, settings)
            loaded = panel["json"].loads(path.read_text())
            self.assertEqual(panel["validate_settings"](loaded, set(settings), 1080), settings)
            loaded["range"] = 1081
            with self.assertRaises(ValueError):
                panel["validate_settings"](loaded, set(settings), 1080)

    def test_every_panel_force_is_bounded(self):
        for name in panel["TESTS"]:
            with self.subTest(name=name):
                effect = panel["build_effect"](name, 5)
                self.assertEqual(effect.id, -1)
                self.assertEqual(effect.ff_replay.length, 500)
        with self.assertRaises(ValueError):
            panel["build_effect"]("Sine", 11)

    def test_package_inspection_checks_format_and_is_read_only(self):
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / "fixture.exe"
            package.write_bytes(b"MZinvalid")
            with self.assertRaises(ValueError):
                panel["inspect_package"](package)
            content = b"MZ" + bytes(58) + (64).to_bytes(4, "little") + b"PE\0\0fixture"
            package.write_bytes(content)
            report = panel["inspect_package"](package)
            self.assertIn(hashlib.sha256(content).hexdigest(), report)
            self.assertEqual(package.read_bytes(), content)
