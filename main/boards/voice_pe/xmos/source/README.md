# Voice PE XMOS Firmware Source

This folder is the Git-trackable source copy for the Voice PE XMOS firmware
embedded by the Tater native Voice PE firmware.

Generated local build files stay inside this folder and are ignored:

```bash
main/boards/voice_pe/xmos/source/build/
main/boards/voice_pe/xmos/source/.venv/
```

When an XMOS change is proven good, commit the source edit here and copy the
resulting upgrade binary to:

```bash
main/boards/voice_pe/xmos/ffva_v1.3.2-vod_upgrade.bin
```

If the XMOS version changes, update the binary filename, the
`target_add_binary_data()` entry in `main/CMakeLists.txt`, the embedded symbol
names and `XMOS_TARGET_VERSION_*` constants in
`main/boards/voice_pe/audio_voice_pe.c`, and the XMOS firmware metadata in
`scripts/build_native_firmware_manifest.py`.

## What Is Included

- VoicePE FFVA application source under `src/`
- Top-level CMake/build files
- XMOS CMake toolchain helper
- Small first-party modules under `modules/asr/` and `modules/audio_pipelines/`
- Wrapper CMake files needed around external XMOS dependency modules
- Dependency patches under `patches/`
- Upstream README preserved as `UPSTREAM_README.md`

## What Is Not Included

This folder intentionally excludes generated and heavy local files:

- `build/`
- `.venv/`
- `.git/`
- generated `.bin`, `.xe`, `.fs`, `.o`, and similar outputs
- external XMOS dependency module checkouts

The dependency commit pins are recorded in `submodules.lock`. To materialize the external XMOS dependencies inside this folder and apply local dependency patches, run:

```bash
bash scripts/fetch_dependencies.sh
```

Then build the fixed-delay upgrade image with:

```bash
./build_voicepe_fixed_delay.sh
```

The build script expects XMOS XTC Tools to be installed. Set `XMOS_TOOL_PATH` if XTC is not installed at `/Applications/XMOS_XTC_15.3.1`.

## Current Source Base

This source started from `esphome/voice-kit-xmos-firmware` at:

```text
ef04d4b Pin GitHub Actions to commit SHAs (#9)
```

The current source includes the Voice PE DoA/VoD XMOS additions used by the
Tater native Voice PE firmware.
