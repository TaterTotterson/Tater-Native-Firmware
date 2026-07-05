# Voice PE XMOS Firmware

The Voice PE target embeds an XMOS DFU upgrade image so new devices can be
brought to the audio firmware version expected by Tater native firmware.

Current embedded image:

```text
ffva_v1.3.2-vod_upgrade.bin
```

The editable source used to build that image lives in:

```text
main/boards/voice_pe/xmos/source/
```

To rebuild after changing the XMOS source:

```sh
cd main/boards/voice_pe/xmos/source
bash scripts/fetch_dependencies.sh
./build_voicepe_fixed_delay.sh
cp build/example_ffva_int_fixed_delay_upgrade.bin ../ffva_v1.3.2-vod_upgrade.bin
```

The build requires XMOS XTC Tools 15.3.1. If it is installed somewhere other
than `/Applications/XMOS_XTC_15.3.1`, set `XMOS_TOOL_PATH` before running the
build script.

When bumping the XMOS firmware version, update the embedded binary filename,
`main/CMakeLists.txt`, the XMOS target version constants in
`main/boards/voice_pe/audio_voice_pe.c`, and
`scripts/build_native_firmware_manifest.py`.
