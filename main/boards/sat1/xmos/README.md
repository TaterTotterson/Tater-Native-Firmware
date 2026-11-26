# Sat1 XMOS Firmware

Lab capture and stall instrumentation are documented in [AUDIO-BENCH.md](AUDIO-BENCH.md).
They use separate images and do not change the release default.

The speech-aware post-AEC tracking and protected cleanup profile is documented
in [TRACKING-CLEANUP.md](TRACKING-CLEANUP.md), including its build identities and
hardware qualification scope.

The standard native `sat1` environment in **0.7.0-r4** embeds
`candidates/sat1_xmos_aec4_track.bin.zlib` and installs XMOS **1.2.0-dev.13**
when another version is present. Its uncompressed image is
`sat1_xmos_1_2_0_dev13_factory.bin`, SHA-256
`2af046f6d95372f332cdc48236c74098ce4f8bcf8608efe8c04925122c75865f`.

The signal path is four raw microphones → four-channel AEC with a mono playback
reference → speech-gated direction tracking and calibrated fractional-delay beam
→ protected IC and residual echo attenuation → noise suppression → AGC.
It retains fwk_voice 0.9.0, protocol 0x11, DoA resource 233, microphone health
and calibration, and Tater's buttons, LED feedback and steering controls.
The dedicated DSP communication link and bounded speaker frame ownership address
the stalls encountered in earlier four-mic experiments. Dev.1 and dev.8 remain
explicit comparison/recovery profiles; historical failed dev.2/dev.3 images do
not qualify the current image.

See [IC-VNR-UPDATE.md](IC-VNR-UPDATE.md) for the dev.8 identity and verification,
and [INTEGRATION.md](INTEGRATION.md) for the historical integration variants.
The dev.8 pre-AEC beam/stereo-reference topology remains in `sat1_xmos_beam_ic`.
The release change applies to native Satellite1 only; it does not update RPi
Sat1 or other native satellite boards.

The lab-only `sat1_xmos_1_1_0_raw4_usb_factory.bin` exposes the physical
microphones independently and remains outside ESP firmware payloads. Rebuilding
that target uses the source wrapper with
`SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4` and the `legacy` configuration.
Its USB target has independent version-generation settings; it is a lab tool,
not one of the native test images.
