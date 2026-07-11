#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

FIRMWARE_ROOT = Path(__file__).resolve().parents[1]
RELEASE_HIGHLIGHTS_PATH = FIRMWARE_ROOT / "RELEASE_HIGHLIGHTS.md"


def text(value: Any) -> str:
    return str(value or "").strip()


def size_label(value: Any) -> str:
    try:
        size = int(value or 0)
    except Exception:
        size = 0
    if size <= 0:
        return "-"
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f} MB"
    if size >= 1024:
        return f"{size / 1024:.1f} KB"
    return f"{size} bytes"


def release_highlights() -> list[str]:
    if not RELEASE_HIGHLIGHTS_PATH.is_file():
        return []
    rows: list[str] = []
    for line in RELEASE_HIGHLIGHTS_PATH.read_text(encoding="utf-8").splitlines():
        clean = line.strip()
        if clean:
            rows.append(clean)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description="Render GitHub release notes for a Tater native firmware manifest.")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    version = text(manifest.get("version")) or args.manifest.stem
    display_version = text(manifest.get("display_version")) or version
    highlights = release_highlights()
    lines: list[str] = [
        f"# Tater Firmware {display_version}",
        "",
        "Tater Native Satellite firmware release.",
        "",
        "## What's New",
        "",
    ]
    if highlights:
        lines.extend([*highlights, ""])
    else:
        lines.extend(["- Maintenance firmware update.", ""])

    lines.extend(
        [
            "## Highlights",
            "",
            "- Ships one shared native firmware version across all supported satellite targets.",
            "- Includes OTA images for Tater-managed updates and factory images for USB recovery/first flash.",
            "- Includes bundled XMOS update payloads for boards that use XMOS audio firmware.",
            "",
            "## Devices",
            "",
        ]
    )

    devices = manifest.get("devices") if isinstance(manifest.get("devices"), list) else []
    for device in devices:
        if not isinstance(device, dict):
            continue
        label = text(device.get("label")) or text(device.get("key")) or "Device"
        board = text(device.get("board")) or text(device.get("key")) or "-"
        lines.extend(
            [
                f"### {label}",
                "",
                f"- Board: `{board}`",
                f"- Firmware: `{text(device.get('firmware_version')) or version}`",
                f"- Flash size: `{text(device.get('flash_size')) or '-'}`",
            ]
        )
        xmos = device.get("xmos_firmware") if isinstance(device.get("xmos_firmware"), dict) else {}
        if xmos:
            lines.append(f"- XMOS firmware: `{text(xmos.get('version')) or '-'}` included")
        lines.append("")

        artifacts = device.get("artifacts") if isinstance(device.get("artifacts"), dict) else {}
        if artifacts:
            lines.extend(
                [
                    "| Artifact | Size | SHA-256 |",
                    "| --- | ---: | --- |",
                ]
            )
            for key in ("ota", "factory"):
                artifact = artifacts.get(key) if isinstance(artifacts.get(key), dict) else None
                if not artifact:
                    continue
                sha = text(artifact.get("sha256")) or "-"
                lines.append(f"| `{key}` | {size_label(artifact.get('size_bytes'))} | `{sha}` |")
            lines.append("")

    lines.extend(
        [
            "## Update Paths",
            "",
            "- Tater OTA uses the `ota` artifact.",
            "- USB recovery and first flash use the `factory` artifact.",
            "- `latest.json` points Tater at this release manifest.",
            "",
        ]
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
