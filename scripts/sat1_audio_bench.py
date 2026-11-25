#!/usr/bin/env python3
"""Record and validate synchronized Sat1 lab audio; never flash a device.

Audio channels 0..7: raw E/W/N/S, playback L/R, processed AGC, pre-AGC NS.
Channels 8..11 carry markers, not audio. Do not play the complete WAV.
Only record/devices need sounddevice; monitor needs pyserial. Analysis uses
the Python standard library and never treats transport validity as audio quality.
"""
import argparse
from array import array
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import time
import wave

RATE = 16000
FRAME = 240
CHANNELS = 12
NAMES = ("raw_e", "raw_w", "raw_n", "raw_s", "reference_l", "reference_r",
         "processed_agc", "pre_agc_ns")
PROFILES = {1: ("beam_ic", 9), 2: ("legacy", 12), 3: ("aec4", 10), 4: ("aec4_beam", 11),
            5: ("aec4_track", 14)}
CASES = ("quiet", "far_field", "music_only", "double_talk", "moving", "tv", "reboot")
STAGES = ("capture", "reference_wait", "mic_wait", "raw_analysis", "aec",
          "intertile_tx", "intertile_rx", "ic", "ns", "agc", "i2s_output",
          "usb_output", "speaker_input", "reference_tx", "frame_pool_wait",
          "raw_beam_lock_wait", "direction_analysis", "post_beam_lock_wait", "post_beam",
          "voice_analysis")


def json_write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def analyze(path, expected=None):
    """Validate markers before using channel statistics. Allow trimmed edge frames."""
    errors = []
    error_count = 0

    def error(message):
        nonlocal error_count
        error_count += 1
        if message not in errors and len(errors) < 20:
            errors.append(message)

    stats = [dict(minimum=None, maximum=None, sum=0, square_sum=0, clipped=0) for _ in NAMES]
    total = valid_samples = startup = complete = runs = 0
    last_marker = None
    run_length = 0
    profile_seen = None
    intervals = dict(count=0, minimum=None, maximum=None, sum=0, over_30ms=0)

    def finish_run(last=False):
        nonlocal runs, complete
        if last_marker is None:
            return
        if run_length > FRAME or (runs > 0 and not last and run_length != FRAME):
            error(f"Frame marker run has {run_length} samples; expected {FRAME}")
        if run_length == FRAME:
            complete += 1
        runs += 1

    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (CHANNELS, 4, RATE, "NONE"):
            raise ValueError("Expected 12-channel, 16 kHz PCM32 bench WAV")
        while data := wav.readframes(4096):
            words = array("i")
            words.frombytes(data)
            if sys.byteorder != "little":
                words.byteswap()
            if len(words) % CHANNELS:
                raise ValueError("Truncated sample frame")
            for offset in range(0, len(words), CHANNELS):
                total += 1
                markers = words[offset + 8:offset + CHANNELS]
                if any(word & 0xffff for word in markers):
                    error("Marker low bits changed: host applied gain, dither or format conversion")
                sequence, low, high, flags = ((word >> 16) & 0xffff for word in markers)
                if flags == 0 and last_marker is None:
                    startup += 1
                    continue
                profile, version, counter = (flags >> 4) & 7, (flags >> 2) & 3, flags >> 8
                identity = (profile, counter)
                if version != 1 or flags & 3 != 3 or flags & 0x80:
                    error("Missing raw/reference data or unsupported marker format")
                    continue
                if PROFILES.get(profile, (None, None))[1] != counter:
                    error(f"Unknown lab image identity: profile={profile}, dev={counter}")
                if expected and (profile != expected["bench_profile"] or counter != expected["linked_version"][4]):
                    error("Captured image does not match the supplied firmware manifest")
                if profile_seen is None:
                    profile_seen = identity
                elif profile_seen != identity:
                    error("Firmware identity changed during capture")
                tick = low | high << 16
                marker = (sequence, tick, flags)
                if last_marker is not None and marker != last_marker:
                    finish_run()
                    step = (sequence - last_marker[0]) & 0xffff
                    if step != 1:
                        error(f"Frame sequence discontinuity: {last_marker[0]} -> {sequence}")
                    delta = ((tick - last_marker[1]) & 0xffffffff) / 100000.0
                    intervals["count"] += 1
                    intervals["sum"] += delta
                    intervals["minimum"] = delta if intervals["minimum"] is None else min(intervals["minimum"], delta)
                    intervals["maximum"] = delta if intervals["maximum"] is None else max(intervals["maximum"], delta)
                    intervals["over_30ms"] += delta > 30
                    run_length = 0
                last_marker = marker
                run_length += 1
                valid_samples += 1
                for channel, s in enumerate(stats):
                    value = words[offset + channel]
                    s["minimum"] = value if s["minimum"] is None else min(s["minimum"], value)
                    s["maximum"] = value if s["maximum"] is None else max(s["maximum"], value)
                    s["sum"] += value
                    s["square_sum"] += value * value
                    s["clipped"] += value in (-2147483648, 2147483647)
    finish_run(last=True)
    if complete < 3:
        error("Fewer than three complete DSP frames; insufficient capture")
    audio = {}
    for name, s in zip(NAMES, stats):
        rms = math.sqrt(s["square_sum"] / valid_samples) if valid_samples else 0
        audio[name] = dict(minimum=s["minimum"], maximum=s["maximum"],
                           mean=s["sum"] / valid_samples if valid_samples else None,
                           rms_dbfs=20 * math.log10(rms / 2147483648) if rms else None,
                           clipped_samples=s["clipped"],
                           constant=s["minimum"] == s["maximum"])
    report = dict(transport_valid=error_count == 0, error_count=error_count, errors=errors,
                  total_samples=total, startup_samples_ignored=startup,
                  marker_valid_samples=valid_samples, complete_dsp_frames=complete,
                  firmware_profile=PROFILES.get(profile_seen[0], (None,))[0] if profile_seen else None,
                  image_counter=profile_seen[1] if profile_seen else None,
                  capture_intervals_ms=dict(minimum=intervals["minimum"], maximum=intervals["maximum"],
                      mean=intervals["sum"] / intervals["count"] if intervals["count"] else None,
                      over_30ms=intervals["over_30ms"]), audio=audio,
                  limitations="Transport and level statistics only. DSP stages have algorithmic delay; this is not an ERLE, speech accuracy or listening-quality score.")
    return report


def expected_image(path, profile):
    entry = json.loads(path.read_text())[profile]
    identity = next((key, counter) for key, (name, counter) in PROFILES.items() if name == profile)
    if (not entry.get("audio_bench") or entry.get("bench_format") != 1 or
            entry.get("capture_channels") != CHANNELS or entry.get("capture_sample_rate") != RATE or
            entry.get("bench_profile") != identity[0] or
            entry.get("linked_version") != [1, 2, 0, 4, identity[1]]):
        raise ValueError("A packaged audio-bench manifest is required")
    return entry


def record(args):
    import sounddevice as sd
    expected = expected_image(args.manifest, args.profile)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifacts = (args.output, args.output.with_suffix(".capture.json"), args.output.with_suffix(".report.json"))
    if any(path.exists() for path in artifacts):
        raise ValueError(f"Refusing to overwrite an existing capture or sidecar for {args.output}")
    metadata = dict(started_utc=datetime.now(timezone.utc).isoformat(), case=args.case,
                    notes=args.notes, expected_firmware=expected,
                    sample_rate=RATE, channels=list(NAMES) + ["sequence16", "tick_low16", "tick_high16", "flags16"],
                    host_overflows=0, recording_complete=False)
    device = int(args.device) if args.device.isdecimal() else args.device
    remaining = round(args.seconds * RATE)
    capture_error = None
    try:
        with args.output.open("xb") as raw_file, wave.open(raw_file, "wb") as wav:
            wav.setparams((CHANNELS, 4, RATE, 0, "NONE", "not compressed"))
            with sd.RawInputStream(device=device, samplerate=RATE, channels=CHANNELS,
                                   dtype="int32", blocksize=FRAME, dither_off=True) as stream:
                while remaining:
                    count = min(FRAME, remaining)
                    data, overflow = stream.read(count)
                    metadata["host_overflows"] += bool(overflow)
                    samples = array("i")
                    samples.frombytes(bytes(data))
                    if sys.byteorder != "little":
                        samples.byteswap()
                    wav.writeframesraw(samples.tobytes())
                    remaining -= count
        metadata["recording_complete"] = True
    except (KeyboardInterrupt, Exception) as exc:
        capture_error = str(exc) or type(exc).__name__
        metadata["recording_error"] = capture_error
    finally:
        json_write(args.output.with_suffix(".capture.json"), metadata)
    if not args.output.exists():
        raise ValueError(capture_error or "No capture was written")
    report = analyze(args.output, expected)
    if metadata["host_overflows"] or not metadata["recording_complete"]:
        report["transport_valid"] = False
        report["errors"].append("Host overflow or interrupted recording; repeat this case")
        report["error_count"] += 1
    json_write(args.output.with_suffix(".report.json"), report)
    print(json.dumps(report, indent=2))
    return 0 if report["transport_valid"] else 2


def monitor(args):
    import serial
    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    count = 0
    with args.output.open("x") as log, serial.Serial(args.port, 115200, timeout=1) as port:
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("SAT1_BENCH "):
                continue
            fields = dict(field.split("=", 1) for field in line.split()[1:] if "=" in field)
            if "s" in fields:
                try:
                    stage = int(fields["s"])
                except ValueError:
                    stage = -1
                fields["stage_name"] = STAGES[stage] if 0 <= stage < len(STAGES) else "unknown"
            entry = dict(fields)
            entry["time_utc"] = datetime.now(timezone.utc).isoformat()
            log.write(json.dumps(entry) + "\n")
            log.flush()
            print(line)
            count += 1
    return 0 if count else 2


def positive(value):
    number = float(value)
    if not math.isfinite(number) or not 0 < number <= 3600:
        raise argparse.ArgumentTypeError("Choose a duration greater than zero and at most 3600 seconds")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("devices", help="List audio input devices")
    p = commands.add_parser("record", help="Record the bench USB interface; play stimuli through Tater")
    p.add_argument("--device", required=True)
    p.add_argument("--seconds", type=positive, default=30)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--profile", choices=[v[0] for v in PROFILES.values()], required=True)
    p.add_argument("--case", choices=CASES, required=True)
    p.add_argument("--notes", default="")
    p = commands.add_parser("analyze", help="Check a previously recorded WAV")
    p.add_argument("wav", type=Path)
    p.add_argument("--output", type=Path)
    p = commands.add_parser("monitor", help="Read stage telemetry from XMOS USB CDC")
    p.add_argument("--port", required=True)
    p.add_argument("--seconds", type=positive, default=30)
    p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "devices":
            import sounddevice as sd
            print(sd.query_devices())
            return 0
        if args.command == "record":
            return record(args)
        if args.command == "monitor":
            return monitor(args)
        metadata_path = args.wav.with_suffix(".capture.json")
        metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
        report = analyze(args.wav, metadata.get("expected_firmware"))
        if metadata.get("host_overflows") or (metadata and not metadata.get("recording_complete")):
            report["transport_valid"] = False
            report["errors"].append("Host overflow or interrupted recording; repeat this case")
            report["error_count"] += 1
        if args.output:
            json_write(args.output, report)
        print(json.dumps(report, indent=2))
        return 0 if report["transport_valid"] else 2
    except (ValueError, OSError, KeyError, wave.Error, ImportError) as exc:
        parser.exit(2, f"{exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
