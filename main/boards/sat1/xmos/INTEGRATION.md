# Sat1 XMOS integration candidate

This branch is for bench evaluation. The normal `sat1` PlatformIO environment
still embeds production XMOS 1.1.1. On November 24, 2026, both four-mic AEC
candidates failed the physical startup test: their DSP frame counters froze and
the wake microphone repeated a constant sample. **Do not promote dev.2 or dev.3.**
The legacy topology with the new library/protocol passed a basic voice check;
its wider qualification remains pending. See [HARDWARE-VALIDATION.md](HARDWARE-VALIDATION.md).

## Images

| Native environment | XMOS identity | Processing |
| --- | --- | --- |
| `sat1` | 1.1.1 | Existing production image and rollback |
| `sat1_xmos_legacy` | 1.2.0-dev.1 | fwk_voice 0.9.0 and protocol 0x11, retaining the pre-AEC beam and two stereo echo references |
| `sat1_xmos_aec4` | 1.2.0-dev.2 | Individual four-mic AEC, mono echo reference, residual averaging; raw DoA/health telemetry |
| `sat1_xmos_aec4_beam` | 1.2.0-dev.3 | Individual four-mic AEC followed by our calibrated, steered residual beam |

All candidates preserve noise suppression, AGC, the two ESP output channels,
button/LED control, USB DFU, and the raw USB lab target source. Calibration and
mic health use raw microphone levels; AEC residual levels never train the
calibration. Capture and AEC tasks serialize access to the calibration/beam
state. Direction follows valid talkers and requests omni steering during
playback, as in production.

We deliberately retain Tater's microphone gain arrangement rather than adding
FPH's fixed 10x digital microphone gain. `aec4` is therefore a topology comparison
at our existing input gain, not a byte-for-byte FPH build. Its residual average
preserves upstream's unweighted mixing. Mic exclusion and gain compensation
apply in the beam variant. The four-mic variants downmix stereo echo references
to mono; stereo AUX/Both output needs its own measurements.

The candidate DoA resource is **233**, reserving FPH's speaker/EQ resource 231
and mic-input settings resource 232. Production 1.1.1 remains on resource 231;
the ESP environment selects the matching resource and image together.
The native reader handles old 0x10 and new 0x11 read framing, validates handler
status, drains write acknowledgments, retries default-buffer responses with a
bound, and locks a complete command/response exchange.

## Provenance

- Library migration: FPH #120, commit `a9e6f6d`; fwk_voice
  `74b9e2e2c4b747efe124f35a52e654e0a19b6c87`.
- Protocol 0x11 and DFU status/serial support: FPH #113, merge `6d3ac98`.
- Four-mic AEC: FPH #127, develop
  `ace8050265f134c1e0442661c8a16866467c9e53`.
- Compact frame layout and explicit I/O helpers: unmerged FPH #132 at
  `208e0d3ea07c3d61ae306b0ddd980b45977b2a76`.

These imports retain the corresponding source notices. PR #132 reduces frame
copying and memory use; it has not established a fix for reported issue #129.
The integration does not import experimental speaker EQ or remove USB support.

## Rebuild

From the native firmware repository root, materialize dependencies with
`bash main/boards/sat1/xmos/source/scripts/fetch_dependencies.sh`. The wrapper
expects XTC 15.3.1, Python 3.11, CMake and Ninja; their paths can be overridden.
Use a fresh dependency checkout when changing voice versions: old generated
`modules/voice/modules/lib_vnr/src/inference/` files from 0.8.0 must be moved
aside because 0.9.0 generates the model in its build directory.

Build **sequentially**: the version generator writes a shared header.

```sh
SAT1_XMOS_PIPELINE=legacy SAT1_XMOS_RUN_SIM_TESTS=1 bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_PIPELINE=aec4 SAT1_XMOS_RUN_SIM_TESTS=1 bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_PIPELINE=aec4_beam SAT1_XMOS_RUN_SIM_TESTS=1 bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
python3 main/boards/sat1/xmos/source/scripts/package_candidates.py legacy aec4 aec4_beam
pio run -e sat1_xmos_legacy -e sat1_xmos_aec4 -e sat1_xmos_aec4_beam
```

Each XMOS configuration has its own `build_<variant>` directory. Use
`XMOS_CLEAN_BUILD=0` for incremental rebuilds. Compressed images, identities and
checksums are under `candidates/`; the standard production payload is unchanged.
The packaging script verifies factory MD5s and compression round trips.
For any source edit, rebuild and repackage before rebuilding the ESP image.

Use the ESP32 USB port for native uploads; the XMOS USB capture port is separate.
For a paired bench device, use the selected PlatformIO environment's upload task
with its ESP32 serial port rather than writing a padded merged factory image.
A full factory-image write can erase setup data in padded regions.

Native binaries and the corresponding bootloader/partition files are in
`.pio/build/<environment>/`. `firmware.bin` is the application; `firmware.factory.bin`
is the merged USB installation image at address 0. Check the actual generated
filename before flashing. The embedded XMOS image installs on first boot.
Returning to the standard `sat1` native firmware reinstalls XMOS 1.1.1.

## Bench validation before release or upstream PRs

Compare production, legacy, aec4, then aec4_beam on the same board, supply,
speaker route, volume and recordings. Save native serial logs and microphone
recordings with the manifest checksum and installed version for each run.
Do not infer live DSP from working SPI alone: check changing microphone samples
and the DoA frame counter through playback and restarts.

| Check | Acceptance | Result |
| --- | --- | --- |
| Installed identity and both mic streams | Expected dev version; live samples on both streams | dev.2/dev.3 fail DSP liveness; dev.1 basic voice pass |
| Ten cold power cycles | Audio capture and playback recover every time | Pending |
| Twenty ESP-only restarts | No stale constant mic stream or silent playback | Pending |
| Playback starts/stops and digital silence | Both streams remain live before/during/after nonzero reference | dev.1 basic tone check passes; dev.2/dev.3 stalled before playback; wider checks pending |
| Thirty minutes of music | No stalls, watchdog reset or degrading mic stream | Pending |
| Wake phrase and commands at 1 m and 3 m, four directions | Record success rates and comparable audio levels | Pending |
| Music plus barge-in | Record command intelligibility, false wakes and echo leakage | Pending |
| Internal, AUX stereo and Both routes | No reference-downmix regression | Pending |
| Moving talker, noise, silence | Stable direction, sensible confidence and neutral fallback | Pending |
| Mic health and recovery | No false fault during echo suppression; controlled lab fault recovers | Pending |
| Buttons, LEDs, USB and rollback | Existing controls work; production image restores successfully | Pending |

Host/simulator tests validate arithmetic, bounds, API compatibility, channel
routing and DSP processing. They do not validate physical I2S startup, reboot
reliability, real-time scheduling under RTOS load, acoustic quality, or USB
hardware enumeration. Keep hardware results pending until measured.

## Upstream contribution sequence

After hardware results are acceptable, base contributions on FPH's then-current
`develop` and account for #132 if it lands. First contribute DoA and diagnostics
without altering default audio outputs. Follow with optional post-AEC
beamforming, calibration and health handling, including recorded comparisons
and memory/timing results. Keep Tater's native updater, gain policy and packaging
in this repository. Do not replace FPH's current library, protocol or AEC with
our older topology.
