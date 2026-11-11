"""Release metadata must reject accidental external-volume distributions."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("installer_builder", ROOT / "tools/build_installer.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class SingleFileReleaseTests(unittest.TestCase):
    def test_one_executable_is_the_entire_distribution(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "artifacts") as temporary:
            folder = Path(temporary)
            (folder / "RvcFeatureInjector-1.1.8-Setup.exe").write_bytes(b"fixture executable")
            builder.write_release_metadata(folder, "1.1.8", 12)
            release = json.loads((folder / "RELEASE.json").read_text())
            self.assertEqual([entry["name"] for entry in release["setup_files"]], ["RvcFeatureInjector-1.1.8-Setup.exe"])
            self.assertIn("single self-contained offline", release["distribution"])
            self.assertIn("non-solid", release["compression"])

    def test_external_volume_or_missing_executable_fails(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "artifacts") as temporary:
            folder = Path(temporary)
            with self.assertRaises(RuntimeError):
                builder.write_release_metadata(folder, "1.1.8", 12)
            (folder / "RvcFeatureInjector-1.1.8-Setup.exe").write_bytes(b"fixture executable")
            (folder / "RvcFeatureInjector-1.1.8-Setup-1.bin").write_bytes(b"external volume")
            with self.assertRaises(RuntimeError):
                builder.write_release_metadata(folder, "1.1.8", 12)


if __name__ == "__main__":
    unittest.main()
