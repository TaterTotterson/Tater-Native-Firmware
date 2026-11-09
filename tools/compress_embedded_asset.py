#!/usr/bin/env python3
"""Create a deterministic zlib stream for a firmware asset."""

from __future__ import annotations

import argparse
import pathlib
import zlib


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    args = parser.parse_args()

    raw = args.source.read_bytes()
    compressed = zlib.compress(raw, level=9)
    args.destination.write_bytes(compressed)
    print(f"{args.source}: {len(raw)} -> {len(compressed)} bytes")


if __name__ == "__main__":
    main()
