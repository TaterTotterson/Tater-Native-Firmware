#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any
from urllib.parse import quote


FIRMWARE_ROOT = Path(__file__).resolve().parents[1]
PREBUILT_ROOT = FIRMWARE_ROOT / "prebuilt_firmware"
BOARDS: dict[str, dict[str, Any]] = {
    "satellite1": {
        "env": "sat1",
        "build_dir": "sat1",
        "prebuilt_dir": "satellite1",
        "key": "satellite1",
        "label": "Satellite1",
        "board": "satellite1",
        "header": FIRMWARE_ROOT / "main" / "boards" / "sat1" / "board_sat1.h",
        "flash_size": "16MB",
    },
    "voicepe": {
        "env": "voicepe",
        "build_dir": "voicepe",
        "prebuilt_dir": "voicepe",
        "key": "voicepe",
        "label": "Voice PE",
        "board": "voice-pe",
        "header": FIRMWARE_ROOT / "main" / "boards" / "voice_pe" / "board_voice_pe.h",
        "flash_size": "16MB",
        "xmos_firmware": {
            "version": "1.3.2",
            "source": FIRMWARE_ROOT / "main" / "boards" / "voice_pe" / "xmos" / "ffva_v1.3.2-vod_upgrade.bin",
            "repo_path": "main/boards/voice_pe/xmos/ffva_v1.3.2-vod_upgrade.bin",
        },
    },
}


def read_version(board: dict[str, Any]) -> str:
    header = board["header"]
    text = header.read_text(encoding="utf-8")
    match = re.search(r'#define\s+TATER_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise SystemExit(f"Could not find TATER_FIRMWARE_VERSION in {header}")
    return match.group(1)


def display_version(version: str) -> str:
    match = re.search(r"(\d+\.\d+\.\d+(?:[-+][A-Za-z0-9_.-]+)?)$", version)
    return match.group(1) if match else version


def release_version(board_versions: list[str]) -> str:
    displays = {display_version(version) for version in board_versions if version}
    if len(displays) != 1:
        joined = ", ".join(board_versions)
        raise SystemExit(f"All selected boards must share a display version. Got: {joined}")
    return f"native-{next(iter(displays))}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def release_asset_url(repo: str, tag: str, asset_name: str) -> str:
    clean_repo = repo.strip().strip("/")
    if not clean_repo or "/" not in clean_repo:
        raise SystemExit("--release-repo must use OWNER/REPO format.")
    return f"https://github.com/{clean_repo}/releases/download/{quote(tag, safe='')}/{quote(asset_name, safe='')}"


def artifact(path: Path, *, kind: str, repo_path: str, flash_size: str) -> dict[str, Any]:
    return {
        "kind": kind,
        "path": repo_path,
        "size_bytes": path.stat().st_size,
        "sha256": sha256(path),
        "flash_size": flash_size,
        "flash_mode": "dio",
        "flash_freq": "40m",
    }


def run_build(board: dict[str, Any]) -> None:
    subprocess.run(["platformio", "run", "-e", board["env"]], cwd=FIRMWARE_ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package Tater Native satellite firmware artifacts and manifest.")
    parser.add_argument(
        "--board",
        choices=["all", *sorted(BOARDS)],
        default="voicepe",
        help="Board key to package, or all for a combined release.",
    )
    parser.add_argument("--skip-build", action="store_true", help="Use existing PlatformIO build outputs.")
    parser.add_argument("--release-repo", help="OWNER/REPO for GitHub Release asset URLs.")
    parser.add_argument("--release-tag", help="GitHub Release tag for asset URLs.")
    parser.add_argument(
        "--release-dir",
        type=Path,
        help="Optional directory to receive flat release assets for GitHub upload.",
    )
    args = parser.parse_args()

    selected_boards = list(BOARDS.values()) if args.board == "all" else [BOARDS[args.board]]
    if not args.skip_build:
        for board in selected_boards:
            run_build(board)

    board_versions = [read_version(board) for board in selected_boards]
    version = board_versions[0] if len(board_versions) == 1 else release_version(board_versions)
    display = display_version(version)
    manifest_path = PREBUILT_ROOT / f"{version}.json"
    local_manifest_repo_path = f"prebuilt_firmware/{version}.json"
    release_mode = bool(args.release_repo or args.release_tag or args.release_dir)
    if release_mode and (not args.release_repo or not args.release_tag or not args.release_dir):
        raise SystemExit("--release-repo, --release-tag, and --release-dir must be provided together.")

    release_dir = args.release_dir.resolve() if args.release_dir else None
    if release_dir:
        release_dir.mkdir(parents=True, exist_ok=True)

    manifest_asset_name = f"{version}-manifest.json"
    latest_asset_name = "latest.json"
    release_manifest_url = release_asset_url(args.release_repo, args.release_tag, manifest_asset_name) if release_mode else ""

    devices: list[dict[str, Any]] = []
    release_devices: list[dict[str, Any]] = []
    latest_boards: dict[str, dict[str, Any]] = {}
    for board in selected_boards:
        board_version = read_version(board)
        board_display = display_version(board_version)
        build_root = FIRMWARE_ROOT / ".pio" / "build" / board["build_dir"]
        ota_src = build_root / "firmware.bin"
        factory_src = build_root / "firmware.factory.bin"
        for path in (ota_src, factory_src):
            if not path.is_file():
                raise SystemExit(f"Missing build output: {path}")

        prebuilt_dir = board["prebuilt_dir"]
        version_dir = PREBUILT_ROOT / prebuilt_dir / board_version
        version_dir.mkdir(parents=True, exist_ok=True)
        ota_dst = version_dir / "firmware.bin"
        factory_dst = version_dir / "firmware.factory.bin"
        shutil.copy2(ota_src, ota_dst)
        shutil.copy2(factory_src, factory_dst)

        ota_asset_name = f"{board_version}-{board['key']}-ota.bin"
        factory_asset_name = f"{board_version}-{board['key']}-factory.bin"
        local_ota_repo_path = f"prebuilt_firmware/{prebuilt_dir}/{board_version}/firmware.bin"
        local_factory_repo_path = f"prebuilt_firmware/{prebuilt_dir}/{board_version}/firmware.factory.bin"
        release_ota_url = release_asset_url(args.release_repo, args.release_tag, ota_asset_name) if release_mode else ""
        release_factory_url = release_asset_url(args.release_repo, args.release_tag, factory_asset_name) if release_mode else ""
        flash_size = board["flash_size"]
        xmos = board.get("xmos_firmware") if isinstance(board.get("xmos_firmware"), dict) else None
        xmos_payload = None
        if xmos:
            xmos_source = xmos["source"]
            xmos_payload = {
                "included": True,
                "version": xmos["version"],
                "path": xmos["repo_path"],
                "size_bytes": xmos_source.stat().st_size,
                "sha256": sha256(xmos_source),
            }
        device = {
            "key": board["key"],
            "label": board["label"],
            "board": board["board"],
            "firmware_version": board_version,
            "display_version": board_display,
            "project": "tater.native_satellite",
            "flash_size": flash_size,
            "artifacts": {
                "ota": artifact(ota_dst, kind="ota", repo_path=local_ota_repo_path, flash_size=flash_size),
                "factory": artifact(factory_dst, kind="factory", repo_path=local_factory_repo_path, flash_size=flash_size),
            },
        }
        if xmos_payload:
            device["xmos_firmware"] = xmos_payload
        devices.append(device)

        release_device = json.loads(json.dumps(device))
        release_device["artifacts"]["ota"]["path"] = release_ota_url
        release_device["artifacts"]["factory"]["path"] = release_factory_url
        release_devices.append(release_device)

        latest_boards[board["key"]] = {
            "label": board["label"],
            "board": board["board"],
            "version": board_version,
            "display_version": board_display,
            "manifest": local_manifest_repo_path,
        }

    manifest = {
        "schema": 1,
        "kind": "tater_native_satellite_firmware",
        "version": version,
        "display_version": display,
        "project": "tater.native_satellite",
        "generated_from": ".",
        "devices": devices,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    latest = {
        "schema": 1,
        "kind": "tater_native_satellite_firmware_latest",
        "version": version,
        "display_version": display,
        "manifest": local_manifest_repo_path,
        "boards": latest_boards,
    }
    latest_path = PREBUILT_ROOT / "latest.json"
    latest_path.write_text(json.dumps(latest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if release_dir:
        release_manifest = {
            **manifest,
            "devices": release_devices,
        }
        release_latest = {
            **latest,
            "manifest": release_manifest_url,
            "boards": {
                key: {
                    **row,
                    "manifest": release_manifest_url,
                }
                for key, row in latest["boards"].items()
            },
        }
        release_manifest_path = release_dir / manifest_asset_name
        release_latest_path = release_dir / latest_asset_name
        release_manifest_path.write_text(json.dumps(release_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        release_latest_path.write_text(json.dumps(release_latest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        for device in devices:
            artifacts = device.get("artifacts") if isinstance(device.get("artifacts"), dict) else {}
            for artifact_kind in ("ota", "factory"):
                row = artifacts.get(artifact_kind) if isinstance(artifacts.get(artifact_kind), dict) else None
                if not row:
                    continue
                source = FIRMWARE_ROOT / row["path"]
                asset_name = f"{device['firmware_version']}-{device['key']}-{artifact_kind}.bin"
                shutil.copy2(source, release_dir / asset_name)

    print(f"Packaged {version}")
    print(f"OTA:     {local_ota_repo_path}")
    print(f"Factory: {local_factory_repo_path}")
    print(f"Manifest: {local_manifest_repo_path}")
    if release_dir:
        print(f"Release assets: {release_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
