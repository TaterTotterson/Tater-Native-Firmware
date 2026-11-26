"""Board-only releases must preserve every unaffected published artifact."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

SCRIPTS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load("build_native_firmware_manifest")
notes = load("render_release_notes")
flasher = load("flash_native_satellite_usb")


class ReleasePackagingTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.boards = copy.deepcopy(package.BOARDS)
        self.blob = zlib.compress(b"tested DSP payload" * 100)
        self.old_manifest = {
            "schema": 1, "kind": "tater_native_satellite_firmware",
            "version": "native-0.7.0-r3", "devices": [],
        }
        self.old_latest = {
            "schema": 1, "kind": "tater_native_satellite_firmware_latest",
            "version": "native-0.7.0-r3", "boards": {},
        }
        for key, board in self.boards.items():
            board["header"] = self.root / f"{key}.h"
            version = f"native-{key}-0.7.0" + ("-r4" if key == "satellite1" else "")
            board["header"].write_text(f'#define TATER_FIRMWARE_VERSION "{version}"\n')
            build = self.root / ".pio" / "build" / board["build_dir"]
            build.mkdir(parents=True)
            # Unselected builds are deliberately absent: packaging may not need them.
            if key == "satellite1":
                self.ota = build / "firmware.bin"
                self.ota.write_bytes(b"native-satellite1-0.7.0-r4\0" + self.blob)
                (build / "firmware.factory.bin").write_bytes(b"factory" + self.ota.read_bytes())
            if "xmos_firmware" in board:
                xmos = board["xmos_firmware"]
                xmos["source"] = self.root / f"{key}-dsp.bin"
                xmos["source"].write_bytes(zlib.decompress(self.blob))
                if "embedded_asset" in xmos:
                    xmos["embedded_asset"] = self.root / f"{key}-dsp.zlib"
                    xmos["embedded_asset"].write_bytes(self.blob)
            version = f"native-{key}-0.7.0" + ("-r3" if key == "satellite1" else "")
            row = {
                "key": key, "label": board["label"], "firmware_version": version,
                "artifacts": {kind: {"path": f"https://example.test/old/{key}-{kind}.bin", "sha256": "a" * 64, "size_bytes": 128}
                              for kind in ("ota", "factory")},
                "extra_preserved_metadata": {"anything": [1, 2, 3]},
            }
            self.old_manifest["devices"].append(row)
            self.old_latest["boards"][key] = {"version": version, "manifest": "https://example.test/old/manifest.json", "label": board["label"]}
        self.patcher = patch.multiple(package, FIRMWARE_ROOT=self.root, LOCAL_RELEASE_ROOT=self.root / "assets", BOARDS=self.boards)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)

    def invoke(self, board="satellite1", public=True, baseline=True):
        args = ["package", "--board", board, "--skip-build", "--release-dir", str(self.root / "release")]
        if public:
            args += ["--release-repo", "owner/repo", "--release-tag", "native-satellite1-0.7.0-r4"]
        if baseline:
            for name, data in (("latest", self.old_latest), ("manifest", self.old_manifest)):
                path = self.root / f"old-{name}.json"
                path.write_text(json.dumps(data))
                args += [f"--previous-{name}", str(path)]
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()):
            return package.main()

    def test_sat1_only_keeps_other_boards_exactly_and_needs_no_other_builds(self):
        self.assertEqual(self.invoke(), 0)
        release = self.root / "release"
        manifest = json.loads(next(release.glob("*-manifest.json")).read_text())
        latest = json.loads((release / "latest.json").read_text())
        self.assertEqual(manifest["updated_boards"], ["satellite1"])
        self.assertEqual(len(manifest["devices"]), 4)
        self.assertEqual(len(list(release.glob("*.bin"))), 2)
        rows = {row["key"]: row for row in manifest["devices"]}
        for row in self.old_manifest["devices"]:
            key = row["key"]
            if key != "satellite1":
                self.assertEqual(rows[key], row)
                self.assertEqual(latest["boards"][key], self.old_latest["boards"][key])
        sat1 = rows["satellite1"]
        self.assertEqual(sat1["firmware_version"], "native-satellite1-0.7.0-r4")
        self.assertIn("/native-satellite1-0.7.0-r4/", sat1["artifacts"]["ota"]["path"])
        self.assertEqual(sat1["artifacts"]["ota"]["sha256"], package.sha256(self.ota))
        # The USB consumer follows the top-level combined manifest, not boards[].
        with patch.object(flasher, "read_json_url", return_value=manifest):
            loaded, _ = flasher.load_manifest("", latest_file=str(release / "latest.json"))
        for key, row in rows.items():
            self.assertEqual(flasher.find_factory_artifact(loaded, key), row["artifacts"]["factory"])
        with patch.object(notes, "RELEASE_HIGHLIGHTS_PATH", self.root / "none"), patch.object(sys, "argv", ["notes", str(next(release.glob("*-manifest.json"))), str(release / "notes.md")]):
            notes.main()
        rendered = (release / "notes.md").read_text()
        self.assertIn("# Satellite1 Firmware 0.7.0-r4", rendered)
        self.assertIn("Updates only Satellite1", rendered)
        self.assertNotIn("one shared native firmware version", rendered)
        self.assertEqual(rendered.count("| `ota` |"), 1)
        self.assertIn("native-voicepe-0.7.0", rendered)

    def test_public_single_board_requires_previous_index(self):
        with self.assertRaisesRegex(SystemExit, "requires the previous"):
            self.invoke(baseline=False)

    def test_local_single_board_remains_supported(self):
        self.assertEqual(self.invoke(public=False, baseline=False), 0)

    def test_all_boards_still_package_without_baseline(self):
        for key, board in self.boards.items():
            if key != "satellite1":
                build = self.root / ".pio" / "build" / board["build_dir"]
                payload = self.blob if "embedded_asset" in board.get("xmos_firmware", {}) else b"app"
                (build / "firmware.bin").write_bytes(package.read_version(board).encode() + b"\0" + payload)
                (build / "firmware.factory.bin").write_bytes(b"factory")
        self.assertEqual(self.invoke(board="all", baseline=False), 0)
        self.assertEqual(len(list((self.root / "release").glob("*.bin"))), 8)

    def test_refuses_missing_or_corrupt_unaffected_board(self):
        cases = ("missing", "duplicate", "index_version", "release_version", "relative_url", "invalid_size", "missing_artifacts")
        for case in cases:
            with self.subTest(case=case):
                manifest, latest = copy.deepcopy(self.old_manifest), copy.deepcopy(self.old_latest)
                row = manifest["devices"][1]
                if case == "missing":
                    manifest["devices"].remove(row)
                elif case == "duplicate":
                    manifest["devices"].append(copy.deepcopy(row))
                elif case == "index_version":
                    latest["boards"][row["key"]]["version"] = "wrong"
                elif case == "release_version":
                    latest["version"] = "wrong"
                elif case == "relative_url":
                    row["artifacts"]["ota"]["path"] = "old.bin"
                elif case == "invalid_size":
                    row["artifacts"]["ota"]["size_bytes"] = "bad"
                else:
                    row["artifacts"] = None
                with self.assertRaises(SystemExit):
                    package.carry_forward(manifest, latest, {"satellite1"})

    def test_refuses_wrong_embedded_dsp_or_oversized_ota(self):
        self.ota.write_bytes(b"native-satellite1-0.7.0-r4\0wrong DSP")
        with self.assertRaisesRegex(SystemExit, "embedded XMOS"):
            self.invoke()
        self.ota.write_bytes(b"x" * (0x300000 + 1))
        with self.assertRaisesRegex(SystemExit, "3 MiB"):
            self.invoke()

    def test_refuses_stale_native_version(self):
        self.ota.write_bytes(b"native-satellite1-0.7.0-r3\0" + self.blob)
        with self.assertRaisesRegex(SystemExit, "board firmware version"):
            self.invoke()

    def test_voicepe_requires_the_matching_embedded_dsp(self):
        board = self.boards["voicepe"]
        build = self.root / ".pio" / "build" / board["build_dir"]
        prefix = package.read_version(board).encode() + b"\0"
        (build / "firmware.factory.bin").write_bytes(b"factory")
        (build / "firmware.bin").write_bytes(prefix + b"old Voice PE DSP")
        with self.assertRaisesRegex(SystemExit, "Voice PE's embedded XMOS"):
            self.invoke(board="voicepe", public=False, baseline=False)
        (build / "firmware.bin").write_bytes(prefix + self.blob)
        self.assertEqual(self.invoke(board="voicepe", public=False, baseline=False), 0)


if __name__ == "__main__":
    unittest.main()
