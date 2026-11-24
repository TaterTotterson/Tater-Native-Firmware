# IC/VNR-enabled Tater Sat1 update

Native Satellite1 **0.7.0-r2** and RPi Sat1 **0.1.19** select the same XMOS
**1.2.0-dev.8** factory image. The update enables IC/VNR in Tater's own
pre-AEC beam pipeline, following the processing topology exercised in the
FPH ESPHome dev.4 companion. This is a newly built Tater image, retaining
Tater's application glue and host controls.

## Processing and identity

Four raw microphones → calibrated steered beam plus omni → stereo-reference
2Y2X AEC → IC/VNR → noise suppression → AGC. Both processed and pre-AGC
outputs remain available. fwk_voice 0.9.0, protocol 0x11 and DoA resource 233
are retained. The earlier failed dev.2/dev.3 four-mic AEC images are excluded.

- Factory image: `sat1_xmos_1_2_0_dev8_factory.bin`
- Factory bytes: 258048
- Factory SHA-256: `da5d2bb570d8165e02f746dc35896a3954a09e07f3131e3530582f0b975a4ac4`
- Factory MD5: `ad0409f5215e6bf5477d7599ba4f953a`
- Compressed bytes: 144877
- Local source digest: `5ed3cb316926376d9a577d7dd059975a21f80636ac02775d38cf1fc8c2cd9f45`

## Verification before tagging

- Both tile compilations use two processed microphone channels and
  `appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR=0`; four-mic AEC is not selected.
- The linked DFU identity is `(1, 2, 0, 4, 8)`.
- Both tile memory checks pass. The simulator passes 24 serial AEC frames,
  pool bounds/reset checks, eight output-VNR frames and Boolean AGC metadata.
- Host direction/beamforming and native regressions pass. The USB boundary
  check required the pinned managed component from the ESP32 build, then
  passed together with the remaining USB, wake-policy and XMOS-updater checks.
- The native Sat1 firmware builds and contains the selected compressed XMOS
  payload exactly once, with an exact decompression round trip.
- The RPi payload is byte-for-byte identical; its local suite passes 113 tests
  with four environment-dependent shared-runtime skips. It retains both older
  XMOS image paths for installed signed OTA verifiers.
- Sat1 alone changes to `native-satellite1-0.7.0-r2`; the other three native
  board versions remain `0.7.0`.

Tater-host physical tests follow the tagged firmware/image builds: installed
identity, wake/reply, playback interruption, direction LEDs, volume/mute and
restart recovery. The earlier FPH ESPHome dev.4 observations and dev.1 native
checks are recorded separately and do not qualify this new image.

## Rebuild

From the native repository root, materialize the pinned dependencies first:

```sh
bash main/boards/sat1/xmos/source/scripts/fetch_dependencies.sh
SAT1_XMOS_PIPELINE=beam_ic SAT1_XMOS_RUN_SIM_TESTS=1 bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
python3.11 main/boards/sat1/xmos/source/scripts/package_candidates.py beam_ic
```

The `legacy` profile retains the IC-bypassed dev.1 comparison; it is not the
release default. The RPi source pin and factory checksum are in that repo's
`upstreams.toml` and `xmos_firmware.py`.

## Native recognition-input correction

Physical checks found a no-reply regression with Native `0.7.0-r2`: wake
detection worked and audio reached Tater, but recognition returned empty
transcripts. Restarting Tater did not resolve it. Reinstalling published
`0.7.0-r1` with XMOS `1.2.0-dev.1` restored replies on the same Tater instance
and settings.

The candidate Native `0.7.0-r3-dev` retains the exact dev.8 XMOS image and uses
its processed channel 0 for recognition, as FPH ESPHome does. The previous
Native input selected pre-AGC channel 1 with an additional 8× software gain and
leveler. The candidate takes channel 0 at unity gain and relies on XMOS AGC,
keeping live recognition consistent with buffered wake/command audio. Host
checks cover the selected channel with a silent alternate channel and the
full PCM16 range without added gain; those checks reject the old input path.
The Sat1 ESP32 firmware builds successfully and embeds the unchanged dev.8
compressed payload exactly once.

On November 24, the candidate was installed on the same Sat1. We confirmed
clear replies from opposite sides and another clear reply after a full power
cycle, with no Tater setting changes. Runtime status after that restart reports
`native-satellite1-0.7.0-r3-dev`, XMOS `1.2.0-dev.8`, reset reason `poweron`,
and no core dump. The candidate remains unpublished. Its OTA image SHA-256 is
`6773533501beaba729a7637630067f78c0c353ba8cf1732bbdde6f53ba3d9094`.
