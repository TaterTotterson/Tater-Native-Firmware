#!/usr/bin/env python3
"""Generate Tater's original, deterministic zen timer chime."""

from __future__ import annotations

import argparse
import math
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 16_000
DURATION_SECONDS = 3.4


def _bell(time_s: float, start_s: float, frequency_hz: float, gain: float) -> float:
    elapsed = time_s - start_s
    if elapsed < 0.0:
        return 0.0
    attack = min(1.0, elapsed / 0.018)
    decay = math.exp(-2.35 * elapsed)
    shimmer = (
        math.sin(2.0 * math.pi * frequency_hz * elapsed)
        + 0.31 * math.sin(2.0 * math.pi * frequency_hz * 2.01 * elapsed + 0.3)
        + 0.13 * math.sin(2.0 * math.pi * frequency_hz * 3.96 * elapsed + 0.8)
    )
    return gain * attack * decay * shimmer


def generate(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frame_count = int(SAMPLE_RATE * DURATION_SECONDS)
    pcm = bytearray()
    for index in range(frame_count):
        time_s = index / SAMPLE_RATE
        sample = (
            _bell(time_s, 0.06, 440.00, 0.26)
            + _bell(time_s, 0.62, 554.37, 0.23)
            + _bell(time_s, 1.18, 659.25, 0.21)
        )
        # Leave a genuinely quiet listening gap before the chime repeats.
        if time_s > 2.48:
            sample *= max(0.0, (2.78 - time_s) / 0.30)
        sample = math.tanh(sample * 2.05) * 0.86
        pcm.extend(struct.pack("<h", int(max(-1.0, min(1.0, sample)) * 32767)))

    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(pcm)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=Path("main/timer_sounds/zen_timer_alarm.wav"),
    )
    args = parser.parse_args()
    generate(args.output)


if __name__ == "__main__":
    main()
