# Sat1 XMOS Firmware Source

This folder is the Git-trackable source copy for the Satellite1 XMOS firmware used by the Sat1 ESPHome package.

This branch also contains isolated integration configurations. See
[../INTEGRATION.md](../INTEGRATION.md) for version identities and bench testing.
The normal native `sat1` target continues to embed the released 1.1.1 image.

## What Is Included

- Satellite1 application source: `satellite-xmos-firmware/`
- Top-level CMake/build files
- XMOS CMake toolchain helper
- Custom FPH support modules under `modules/fph/`
- Wrapper CMake files needed around external XMOS dependency modules
- Upstream README preserved as `UPSTREAM_README.md`

## What Is Not Included

This folder intentionally excludes generated and heavy local files:

- `build/`
- `.venv/`
- `.git/`
- generated `.bin`, `.xe`, `.fs`, `.o`, and similar outputs
- external XMOS dependency module checkouts

The dependency commit pins are recorded in `submodules.lock`. To materialize the external XMOS dependencies inside this folder, run:

```bash
bash scripts/fetch_dependencies.sh
```

Then build with:

```bash
./build_sat1_fixed_delay.sh
```

The build script expects XMOS XTC Tools to be installed. Set `XMOS_TOOL_PATH` if XTC is not installed at `/Applications/XMOS_XTC_15.3.1`.

Run the host-side direction estimator and beamformer checks with:

```bash
scripts/test_doa_estimator_host.sh
```

Build the lab-only four-channel USB capture image with:

```bash
XMOS_CLEAN_BUILD=0 SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4 ./build_sat1_fixed_delay.sh
```

It enumerates as `SATELLITE1 Raw 4-Mic` and exposes four 16 kHz, 16-bit
channels in East, West, North, South order. This target bypasses the production
AEC/NS/AGC path so the physical microphones can be measured independently.

## Current Source Base

This source started from `FutureProofHomes/Satellite1-XMOS` at:

```text
86cf1ee Merge pull request #106 from FutureProofHomes/align_branches
```

The released version 1.1.1 includes the Sat1 four-microphone DoA estimator, host-controlled
steering modes, a four-tap fractional-delay beamformer, slow per-microphone gain
calibration, failed/noisy/clipping microphone fallback, and extended device
control diagnostics. Production firmware continuously follows valid talker
directions and requests omni steering while speaker playback is active. The
steered beam is generated before the existing two-channel AEC because the
bundled XMOS voice library supports at most two microphone channels; its second
microphone-side channel remains an omni mix.


## Integration source

The source now uses fwk_voice 0.9.0 and device-control protocol 0x11. The
`legacy` configuration retains the two-channel AEC topology above; `aec4` and
`aec4_beam` use the separately retained four-mic compact-frame pipeline under
`audio_pipelines/reference/fixed_delay4/`. The beam variant applies calibrated
delays to AEC residuals while estimating direction and microphone health from
raw capture. The newer source uses DoA resource 233.

The wrapper defaults to building the legacy integration candidate in
`build_legacy/`, with version 1.2.0-dev.1. Explicit `SAT1_XMOS_PIPELINE=aec4` and
`aec4_beam` builds use their own directories and dev identities. They do not
replace the production embedded payload automatically.
