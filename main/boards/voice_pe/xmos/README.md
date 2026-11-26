# Voice PE XMOS Firmware

The Voice PE target embeds an XMOS DFU upgrade image so new devices can be
brought to the audio firmware version expected by Tater native firmware.

Current embedded image:

```text
ffva_v1.3.4-vod_upgrade.bin
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
cp build/example_ffva_int_fixed_delay_upgrade.bin ../ffva_v1.3.4-vod_upgrade.bin
python3 -c 'import pathlib,zlib; p=pathlib.Path("../ffva_v1.3.4-vod_upgrade.bin"); pathlib.Path(str(p)+".zlib").write_bytes(zlib.compress(p.read_bytes(),9))'
```

The build requires XMOS XTC Tools 15.3.1. If it is installed somewhere other
than `/Applications/XMOS_XTC_15.3.1`, set `XMOS_TOOL_PATH` before running the
build script.

When bumping the XMOS firmware version, update the embedded binary filename,
`main/CMakeLists.txt`, the XMOS target version constants in
`main/boards/voice_pe/audio_voice_pe.c`, and
`scripts/build_native_firmware_manifest.py`.

## Speech-aware direction tracking

The fixed-delay image estimates direction from both echo-cancelled microphone
channels before interference cancellation changes either channel. Its existing
VNR model supplies speech evidence for that frame. The direction gate recovers
the current inference from the SDK's before/after EMA values and its actual
coefficient. It does not wait for the slow adaptation EMA, modify that EMA, or
run another inference engine. Normalized, mean-centred microphone correlation
keeps broad speech peaks and microphone gain differences from producing zero
confidence. DC-only and incoherent inputs are rejected. A short 90 ms evidence hold
bridges speech gaps; it queues no audio and does not change voice endpointing.
AEC, interference cancellation, noise suppression, AGC and the 40 ms AEC
alignment delay retain their existing settings and signal paths.

The direction service runs on tile 0 alongside the IC/VNR stage. The other
reference profiles retain their original direction path on the microphone tile.
Direction snapshots and host hints use short RTOS critical sections; direction
analysis and inference run outside them.

Native firmware enables playback/listening hints after recognizing the XMOS
capability response. It sends changes promptly and refreshes unchanged hints
nominally every 250 ms during normal polling. XMOS ignores hints older than
750 ms and falls back to local reference and VNR evidence. A playback hint
does not disable directions. A listening hint still requires speech evidence
and a valid microphone correlation. Hints affect direction selection only in
this version; they do not change microphone gain or suppress command audio.
The native direction task polls at 20 Hz. Voice PE's listening animation renders
at 40 Hz and interpolates between observations; other boards keep their timing.

Compile the native host with `TATER_VOICEPE_AUDIO_HINTS=0` to disable capability
probes and hint writes for comparison. The 12-byte direction response remains
compatible with older hosts. All new commands use resource 231:

| Command | Payload |
| --- | --- |
| Read state (`0x80`) | Existing 12 bytes; flag bits described below |
| Read capabilities (`0x81`) | Four bytes: `V`, `P`, protocol version `1`, capabilities `3` |
| Write hints (`0x02`) | One byte: bit 0 playback, bit 1 listening |

Read commands have the existing leading status byte on the wire. State flag bit
0 remains direction-valid. New bits report speech evidence (1), playback (2),
listening (3) and fresh host hints (4). Unsupported images receive no hint
writes. Definitions are shared with the native host in
`voicepe_audio_protocol.h`.

Run the portable signal/protocol regressions from the native repository root:

```sh
bash main/scripts/test_voicepe_tracking_host.sh
```

Hardware comparison should cover immediate commands from both sides with and
without music, direction during the command, moving speech, command endpointing,
playback continuity and a full power cycle. Two microphones retain front/back
ambiguity; these changes do not provide full-ring angular localization.
