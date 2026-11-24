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
