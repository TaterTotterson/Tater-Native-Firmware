# Satellite1 audio bench

This first milestone adds synchronized diagnostic capture and stage telemetry.
It does not replace the production audio pipeline or fix the four-mic AEC
hardware stalls. The normal `sat1` target still embeds dev.8. Bench images are
excluded from release packaging.

## Build

From the repository root, with the existing XMOS XTC 15.3.1 dependencies:

```sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=beam_ic SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=aec4 SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=aec4_beam SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
python3 main/boards/sat1/xmos/source/scripts/package_candidates.py \
  --bench beam_ic aec4 aec4_beam
pio run -e sat1_audio_bench_beam_ic
pio run -e sat1_audio_bench_aec4
pio run -e sat1_audio_bench_aec4_beam
```

Run firmware builds sequentially because version generation shares a source
header. Each uses its own `build_<profile>_bench` directory. Packaging verifies
the linked identity, CMake bench setting, factory MD5 and compression round trip.
Outputs and provenance go into ignored `xmos/bench_candidates/`; the matching
native host embeds that image and identity so it survives the host's boot-time
XMOS version check.

Boot normally compares version bytes, not an image hash. When rebuilding a lab
image with the same dev counter, install the normal native image between bench
installs so the version changes and page-verified XMOS programming runs again.
Save those installation logs. In-band markers verify profile/version; the
manifest's factory hash is expected provenance, not a hash read from hardware.

| Profile | Lab identity | Marker profile |
| --- | --- | --- |
| beam_ic | 1.2.0-dev.9 | 1 |
| legacy (optional) | 1.2.0-dev.12 | 2 |
| aec4 | 1.2.0-dev.10 | 3 |
| aec4_beam | 1.2.0-dev.11 | 4 |

These compare complete pipelines, not microphone count alone. `beam_ic` keeps
two independent playback references and enables IC/VNR. The four-mic variants
mix playback references to mono and bypass IC/VNR. Those differences must be
retained in any interpretation; stereo-reference four-mic AEC is later work.

These commands build only. Installing a bench image is a separate hardware
action. Retain a working native recovery/OTA image. After installation, connect
the ESP32 USB port for native logs and the XMOS USB port for audio/CDC. Play
stimuli through Tater's normal I2S route. The bench USB interface is capture
only and cannot replace the playback references with USB audio.

## Record and analyze

```sh
python3 -m venv out/sat1-audio-bench-venv
out/sat1-audio-bench-venv/bin/python -m pip install \
  -r scripts/requirements-sat1-audio-bench.txt
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py devices

# Replace 3 with the SATELLITE1 Audio Bench input device index.
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py record \
  --device 3 --seconds 30 --profile beam_ic --case double_talk \
  --manifest main/boards/sat1/xmos/bench_candidates/manifest.json \
  --notes 'Record distance, direction, room, playback level and stimulus' \
  --output audio_bench_results/beam_ic/double_talk.wav

# Run alongside recording; choose the XMOS CDC port, not the ESP32 port.
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py monitor \
  --port /dev/cu.REPLACE_WITH_XMOS_CDC --seconds 40 \
  --output audio_bench_results/beam_ic/stages.jsonl

python3 scripts/sat1_audio_bench.py analyze \
  audio_bench_results/beam_ic/double_talk.wav
```

Files are created without overwriting existing recordings. WAV is 16 kHz PCM32
with twelve channels. **Do not play the complete WAV:** its final four channels
are digital markers. Extract only audio channels for listening. Select native
format, unity capture gain and no host audio enhancements.

| Zero-based channel | Contents |
| --- | --- |
| 0–3 | Raw East/West/North/South, before calibration and beamforming |
| 4–5 | Original left/right I2S playback references, before mono mixing/AEC |
| 6 | Processed AGC audio used for wake and recognition |
| 7 | Noise-suppressed audio before AGC |
| 8 | Low 16 bits of the DSP frame sequence |
| 9–10 | Low/high 16-bit halves of the 100 MHz capture tick |
| 11 | Flags: validity, format version, profile, image dev counter |

Each marker occupies bits 16–31 of its PCM word, surviving PCM24/float32 host
paths. Sequence wraps at 65,536 frames; tick wraps at 2^32. Flags use bits 0/1
for raw/reference validity, bits 2–3 for format version 1, bits 4–6 for profile,
bit 7 reserved, and bits 8–15 for the dev counter. Markers repeat across all
240 samples of their originating processing frame.

Inputs and outputs travel in the same processing frame. Internal AEC/NS/AGC
delays still apply: sample zero is not necessarily the same acoustic instant
in every channel. Align measured impulse responses before waveform comparison.
The analyzer deliberately does not call raw-to-AGC attenuation an ERLE score.

The capture sidecar saves expected firmware, case and host overflows. In-band
identity must match the manifest. Reports reject missing/repeated frames,
malformed markers, absent data, unsupported identities, host overflows and
interrupted recordings. Initial warm-up zeros and partial edge frames are
allowed. Reports include channel levels, constant channels and capture timing;
transport validity is not a speech-quality or recognition pass.

## Diagnose a stall

Separate low-priority tasks report per-tile counters once per second. Audio
callbacks contain no CDC logging calls and USB capture uses zero-timeout
enqueue operations. Entries, completions, active status,
active age, last/max elapsed ticks and completed calls exceeding 15 ms are
reported. Times include scheduling waits, not just CPU execution. Ages wrap
after about 42.95 seconds. Heap/minimum-ever heap and USB drops are reported.

| Stage | Boundary |
| --- | --- |
| 0 | Capture callback, including allocation/input preparation |
| 1 | Speaker-reference queue wait |
| 2 | Microphone driver wait |
| 3 | Raw direction/health/beam processing |
| 4 | AEC and optional post-AEC beam |
| 5–6 | Intertile audio send/receive |
| 7–9 | IC/VNR, noise suppression, AGC |
| 10–11 | ESP32 I2S output, USB packing/enqueue |
| 12–13 | Speaker I2S input, reference enqueue |

Compare several snapshots: normal waits may be active. An entered-but-not-ended
AEC differs from advancing AEC with blocked I2S output. CDC itself shares MCU
scheduling/intertile resources; if logs stop too, retain ESP32 logs and collect
a debugger trace. The last printed stage is not proof of a failing instruction.

Bench capture increases frame size, bandwidth, heap and scheduling load. It can
expose or change a timing fault. Reproduce a fix with instrumentation disabled
before changing a release image.

## Qualification and following milestones

Use identical stimuli, positions, route, supply and playback level per profile.
Record quiet, distant speech, music only, speech over music, moving talker, TV,
and startup/restart cases. Save both logs and Tater transcripts. Count missed
wakes, false wakes per hour, correct commands, interruptions and response time.
Silence and playback-only cases distinguish echo leakage from desired speech.

For restart tests, capture separate sessions after each boot. A USB disconnect
during reset invalidates a continuous recording as expected; assess recovery
from boot logs and a fresh capture instead.

Next: diagnose physical stalls; establish runtime memory/deadline margins;
verify repeated cold/ESP-only restarts and speaker routes; compare against
dev.8. Then evaluate stereo-reference four-mic AEC, adaptive directional
processing during playback, residual-echo suppression and dereverberation,
followed by wake/turn-taking tuning. This milestone does not establish HomePod
parity or resolve issue #129.

Host regression command: `bash main/scripts/test_sat1_audio_bench_host.sh`.

## Software verification, November 25, 2026

- Fifteen new host tests pass, including the actual C snapshot/packing code,
  counter/timer wraparound, corrupt/drop/repeat detection, host overflow,
  exclusive recording files and sanitized USB partial/empty-read handling.
- All eleven existing channel-contract tests pass, including four xcore cases.
  Direction/beam, native protocol, recognition routing, updater and playback
  wake-policy regressions pass.
- XMOS voice compatibility passes 24 serial AEC and eight output-VNR frames.
  Four-mic AEC passes its canary/pool/schedule checks and 32 reference/scheduled
  frames. The combined AEC/DoA/calibration/beam simulator passes 48 frames and
  three initializations, with maximum reference ticks of 919,626. These are
  synthetic checks, not physical RTOS deadline measurements.
- All three main bench XMOS profiles and matching ESP32 hosts build. Each host
  fits the existing 3 MiB app partition and embeds exactly its matching compressed
  image, excluding the production dev.8 payload. The default bench-disabled
  XMOS profile also builds. Production assets/version/release selection are
  unchanged.
- Physical USB enumeration, capture, acoustic quality and restart/stall behavior
  remain untested for these bench images. No device was flashed in this run.
