#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any


FIRMWARE_ROOT = Path(__file__).resolve().parents[1]
LOCAL_LATEST = FIRMWARE_ROOT / "prebuilt_firmware" / "latest.json"
DEFAULT_LATEST_URL = "https://github.com/TaterTotterson/Tater-Native-Firmware/releases/latest/download/latest.json"
CACHE_ROOT = Path.home() / ".taterassistant" / "native_firmware_cache"
APP_PARTITION_OFFSETS = ["0x20000", "0x320000", "0x620000"]
APP_PARTITION_SIZE = 0x300000
APP_PARTITION_OFFSETS_BY_FLASH_SIZE = {
    "8mb": ["0x20000", "0x320000"],
    "16mb": APP_PARTITION_OFFSETS,
}


def text(value: Any) -> str:
    return str(value or "").strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json_path(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def read_json_url(url: str) -> dict[str, Any]:
    req = urllib.request.Request(url, headers={"User-Agent": "TaterNativeFlasher/1.0"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.loads(response.read().decode("utf-8", errors="replace"))


def raw_url(path_or_url: str, latest_url: str) -> str:
    if path_or_url.startswith(("http://", "https://")):
        return path_or_url
    if "raw.githubusercontent.com" in latest_url:
        parts = latest_url.split("/")
        try:
            base = "/".join(parts[:7])
            return f"{base}/{path_or_url.lstrip('/')}"
        except Exception:
            pass
    return f"https://raw.githubusercontent.com/TaterTotterson/Tater-Native-Firmware/main/{path_or_url.lstrip('/')}"


def local_repo_path(path_or_url: str) -> Path | None:
    if path_or_url.startswith(("http://", "https://")):
        return None
    clean = path_or_url.lstrip("/")
    if clean.startswith("firmware/native_satellite/"):
        clean = clean[len("firmware/native_satellite/") :]
    return FIRMWARE_ROOT / clean


def load_manifest(latest_url: str, *, prefer_local: bool = False) -> tuple[dict[str, Any], str]:
    if prefer_local and LOCAL_LATEST.is_file():
        latest = read_json_path(LOCAL_LATEST)
        manifest_ref = text(latest.get("manifest"))
        manifest_path = local_repo_path(manifest_ref)
        if manifest_path and manifest_path.is_file():
            return read_json_path(manifest_path), str(manifest_path)

    try:
        latest = read_json_url(latest_url)
        manifest_ref = text(latest.get("manifest"))
        if not manifest_ref:
            raise SystemExit("Native firmware latest.json is missing manifest.")
        manifest_url = raw_url(manifest_ref, latest_url)
        return read_json_url(manifest_url), manifest_url
    except Exception as exc:
        if prefer_local or not LOCAL_LATEST.is_file():
            raise
        print(f"Remote manifest unavailable ({exc}); falling back to local prebuilt firmware.")
        latest = read_json_path(LOCAL_LATEST)
        manifest_ref = text(latest.get("manifest"))
        manifest_path = local_repo_path(manifest_ref)
        if manifest_path and manifest_path.is_file():
            return read_json_path(manifest_path), str(manifest_path)
        raise


def normalize_board_key(board: str) -> str:
    clean = text(board).lower().replace("_", "-").replace(" ", "-")
    aliases = {
        "sat1": "satellite1",
        "sat-1": "satellite1",
        "satellite-1": "satellite1",
        "voice-pe": "voicepe",
        "voice-pe-s3": "voicepe",
        "respeaker": "respeaker_xvf3800",
        "respeaker-xvf3800": "respeaker_xvf3800",
        "respeaker_xvf3800": "respeaker_xvf3800",
        "xvf3800": "respeaker_xvf3800",
    }
    return aliases.get(clean, clean)


def find_factory_artifact(manifest: dict[str, Any], board: str) -> dict[str, Any]:
    target = normalize_board_key(board)
    for row in manifest.get("devices") or []:
        if not isinstance(row, dict):
            continue
        row_key = normalize_board_key(text(row.get("key")))
        row_board = normalize_board_key(text(row.get("board")))
        if row_key != target and row_board != target:
            continue
        artifacts = row.get("artifacts") if isinstance(row.get("artifacts"), dict) else {}
        factory = artifacts.get("factory") if isinstance(artifacts.get("factory"), dict) else None
        if isinstance(factory, dict) and text(factory.get("path")):
            return dict(factory)
    raise SystemExit(f"No factory artifact found for board {board!r}.")


def download_or_copy_artifact(artifact: dict[str, Any], manifest_source: str) -> Path:
    path_ref = text(artifact.get("path"))
    local_path = local_repo_path(path_ref)
    if local_path and local_path.is_file():
        path = local_path
    else:
        CACHE_ROOT.mkdir(parents=True, exist_ok=True)
        target = CACHE_ROOT / Path(path_ref).name
        url = raw_url(path_ref, manifest_source)
        print(f"Downloading {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "TaterNativeFlasher/1.0"})
        with urllib.request.urlopen(req, timeout=120) as response:
            target.write_bytes(response.read())
        path = target

    expected_size = int(artifact.get("size_bytes") or 0)
    if expected_size and path.stat().st_size != expected_size:
        raise SystemExit(f"Size check failed for {path}: expected {expected_size}, got {path.stat().st_size}.")
    expected_sha = text(artifact.get("sha256")).lower()
    if expected_sha:
        actual_sha = sha256(path).lower()
        if actual_sha != expected_sha:
            raise SystemExit(f"SHA-256 check failed for {path}: expected {expected_sha}, got {actual_sha}.")
    return path


def find_ports() -> list[str]:
    candidates: list[str] = []
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.SLAB_USBtoUART*"):
        candidates.extend(str(path) for path in sorted(Path("/").glob(pattern.lstrip("/"))))
    return candidates


def find_esptool_python() -> list[str]:
    candidates = [
        [sys.executable, "-m", "esptool"],
        [str(Path.home() / ".platformio" / "penv" / "bin" / "python"), "-m", "esptool"],
    ]
    for command in candidates:
        try:
            subprocess.run(command + ["version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            return command
        except Exception:
            continue
    esptool = shutil.which("esptool.py") or shutil.which("esptool")
    if esptool:
        return [esptool]
    raise SystemExit("Could not find esptool. Install PlatformIO or run: python3 -m pip install esptool")


def parse_offsets(value: str, flash_size: str = "16MB") -> list[str]:
    clean = text(value)
    if not clean or clean.lower() == "all":
        return list(APP_PARTITION_OFFSETS_BY_FLASH_SIZE.get(text(flash_size).lower(), APP_PARTITION_OFFSETS))
    offsets: list[str] = []
    for item in clean.split(","):
        offset = text(item).lower()
        if not offset:
            continue
        if not offset.startswith("0x"):
            raise SystemExit(f"Invalid app offset {item!r}; use hex offsets like 0x20000.")
        int(offset, 16)
        offsets.append(offset)
    if not offsets:
        raise SystemExit("No app offsets supplied.")
    return offsets


def infer_local_flash_size(path: Path, explicit_flash_size: str = "") -> str:
    clean = text(explicit_flash_size)
    if clean:
        return clean
    name = str(path).lower()
    if "respeaker" in name or "xvf3800" in name:
        return "8MB"
    return "16MB"


def main() -> int:
    parser = argparse.ArgumentParser(description="Flash Tater Native satellite firmware over USB without a browser.")
    parser.add_argument("port", nargs="?", help="Serial port, for example /dev/cu.usbmodem4101.")
    parser.add_argument("--board", help="Native board key, for example satellite1 or voicepe. Required unless --image is used.")
    parser.add_argument("--image", help="Use a local factory image instead of the manifest artifact.")
    parser.add_argument("--app-image", help="Use a local app/OTA image and write app partitions only, preserving setup data.")
    parser.add_argument("--app-offsets", default="all", help="Comma-separated app offsets for --app-image. Default: all app slots.")
    parser.add_argument("--flash-size", default="", help="Flash size for local --image/--app-image, for example 8MB or 16MB.")
    parser.add_argument("--latest-url", default=os.getenv("TATER_NATIVE_FIRMWARE_LATEST_URL", DEFAULT_LATEST_URL))
    parser.add_argument("--local", action="store_true", help="Use this repo's local prebuilt_firmware folder instead of the GitHub release manifest.")
    parser.add_argument("--baud", default="921600")
    parser.add_argument("--no-erase", action="store_true", help="Do not erase flash before writing the factory image.")
    args = parser.parse_args()

    if args.image and args.app_image:
        raise SystemExit("Use either --image for a factory image or --app-image for an app/OTA image, not both.")

    port = text(args.port)
    if not port:
        ports = find_ports()
        if len(ports) == 1:
            port = ports[0]
        elif ports:
            print("Available serial ports:")
            for candidate in ports:
                print(f"  {candidate}")
            raise SystemExit(
                "Pass the port to flash, for example: "
                "./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101"
            )
        else:
            raise SystemExit("No USB serial ports found. Plug in the satellite and pass the port path.")

    app_offsets: list[str] = []
    if args.app_image:
        image = Path(args.app_image).expanduser().resolve()
        if not image.is_file():
            raise SystemExit(f"App image not found: {image}")
        if "factory" in image.name:
            raise SystemExit("--app-image expects firmware.bin, not firmware.factory.bin.")
        if image.stat().st_size > APP_PARTITION_SIZE:
            raise SystemExit(f"App image is too large for the app partition: {image.stat().st_size} > {APP_PARTITION_SIZE}.")
        local_flash_size = infer_local_flash_size(image, args.flash_size)
        artifact = {"flash_size": local_flash_size, "flash_mode": "dio", "flash_freq": "80m"}
        app_offsets = parse_offsets(args.app_offsets, local_flash_size)
    elif args.image:
        image = Path(args.image).expanduser().resolve()
        if not image.is_file():
            raise SystemExit(f"Image not found: {image}")
        artifact = {"flash_size": infer_local_flash_size(image, args.flash_size), "flash_mode": "dio", "flash_freq": "40m"}
        if "factory" not in image.name:
            raise SystemExit("Local --image expects a factory image. Use --app-image for .pio/build/<env>/firmware.bin.")
    else:
        board = text(args.board)
        if not board:
            raise SystemExit("Pass the board to flash, for example: --board satellite1 or --board voicepe.")
        manifest, manifest_source = load_manifest(text(args.latest_url) or DEFAULT_LATEST_URL, prefer_local=bool(args.local))
        artifact = find_factory_artifact(manifest, board)
        image = download_or_copy_artifact(artifact, manifest_source)

    esptool_cmd = find_esptool_python()
    flash_size = text(artifact.get("flash_size")) or "16MB"
    flash_mode = text(artifact.get("flash_mode")) or "dio"
    flash_freq = text(artifact.get("flash_freq")) or "40m"

    if app_offsets:
        print(f"Flashing app image {image} to {port} at {', '.join(app_offsets)}...")
        flash_pairs: list[str] = []
        for offset in app_offsets:
            flash_pairs.extend([offset, str(image)])
        subprocess.run(
            esptool_cmd
            + [
                "--chip",
                "esp32s3",
                "--port",
                port,
                "--baud",
                text(args.baud),
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "write_flash",
                "-z",
                "--flash_mode",
                flash_mode,
                "--flash_freq",
                flash_freq,
                "--flash_size",
                flash_size,
            ]
            + flash_pairs,
            check=True,
        )
        print("Done. The satellite should reboot with existing setup data preserved.")
        return 0

    if not args.no_erase:
        print(f"Erasing flash on {port}...")
        subprocess.run(esptool_cmd + ["--chip", "esp32s3", "--port", port, "--baud", text(args.baud), "erase_flash"], check=True)
    else:
        print("Warning: factory images still overwrite low flash regions, including provisioning data. Use --app-image to preserve setup.")

    print(f"Flashing {image} to {port}...")
    subprocess.run(
        esptool_cmd
        + [
            "--chip",
            "esp32s3",
            "--port",
            port,
            "--baud",
            text(args.baud),
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "write_flash",
            "-z",
            "--flash_mode",
            flash_mode,
            "--flash_freq",
            flash_freq,
            "--flash_size",
            flash_size,
            "0x0",
            str(image),
        ],
        check=True,
    )
    print("Done. After reboot, connect to Tater-Setup-XXXX and provision the satellite with an Add Satellite pairing code.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
