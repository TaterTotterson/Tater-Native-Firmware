# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

## Current status

Satellite1 has two deliberately separate Tater Native targets. Public Batch #2
and later hardware uses `native-satellite1-0.3.13`, with factory and OTA
artifacts published under the `satellite1` release family. Public Batch #1 /
Beta.1 HAT and Core rev4.1 uses `native-satellite1-beta-rev41-0.3.13`, published
under `satellite1_beta_rev41` with board and OTA family
`satellite1-beta-rev41`.
Users can install it through Tater's Local USB or Browser USB flasher, complete
first-boot setup through `Tater-Setup-XXXX`, pair it with Tater, and perform
normal firmware updates from Tater's firmware page.

The released firmware includes:

- local Hey Tater and configurable microWakeWord models from Tater
- native microphone streaming, STT/TTS, continued chat, timers, trainer hooks,
  wake verification, and push-to-talk
- WAV, MP3, and FLAC playback; persistent music sessions; ducked TTS overlays;
  and synchronized stereo and multi-room playback
- a Tater-controlled 24-LED state ring with directional listening and reply
  effects, configurable colors, brightness, and animations
- action-button playback/timer control and the physical setup-reset gesture
- verified OTA, persistent pairing/settings, device logs, and runtime diagnostics

### Satellite1 hardware support

The production target supports Public Batch #2 hardware (HAT rev6.1 with Core
rev5.1) and later compatible revisions. It leaves the FUSB302B unconfigured and
uses the board's standard 5 V power path with TAS2780 mode 0.

The legacy target is only for Public Batch #1 / Beta.1 HAT and Core rev4.1. It
starts in the safe 5 V/mode-0 state, requests an exact fixed 9 V USB-PD PDO, and
uses TAS2780 mode 2 only after the source accepts that request and sends
`PS_RDY`. If the controller, cable, or power source cannot establish that exact
contract, it stays at 5 V/mode 0. Its startup negotiation has bounded recovery
and does not continually renegotiate while the satellite is running.

The hardware revision is not reliably detectable in firmware. The first USB
install therefore requires an explicit choice between `satellite1` and
`satellite1_beta_rev41`. Once installed, distinct board IDs, manifest entries,
and embedded OTA-family validation prevent normal OTA updates from crossing
between the production and legacy images.

- 48 kHz microphone capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback through the PCM5122/TAS2780 path
- board-specific TAS2780 power policy described above
- four-microphone XMOS DoA estimation with adaptive room-noise calibration,
  confidence filtering, and directional smoothing
- four-microphone fractional-delay, delay-and-sum beamforming with continuous
  talker tracking, per-microphone gain calibration, unhealthy-microphone
  fallback, and automatic omni steering during playback
- wake-word detection that remains active during speaker playback using the
  XMOS hardware-referenced AEC channel, independent of the barge-in setting;
  barge-in only decides whether a confirmed wake stops the playing audio
- sensitivity settings that adjust the effective detector threshold, plus a
  `tv_nearby` profile that admits stronger candidates and requires Tater wake
  verification before opening a voice session (with fail-open handling when
  verification is unavailable)
- directional LEDs that reject weak/noise-only updates, briefly hold the last
  speech direction, and then return to a neutral listening state
- bundled production XMOS firmware `1.1.1`, automatically installed at boot
  when the detected XMOS image differs
- firmware-side adaptive AEC and line-out capability reporting to Tater

The PlatformIO environments are `sat1` for production hardware and
`sat1_beta_rev41` for the legacy Beta.1/rev4.1 hardware. Shared releases use the
native firmware version, while board-only updates can add a `revN` suffix
without forcing a new build for every other supported board.

Unlike Voice PE, Sat1 updates XMOS by holding the XMOS in reset and directly
writing the external XMOS flash over SPI. The update happens before audio starts,
and each written page is read back for verification.

Hardware constants live in:

```text
main/boards/sat1/board_sat1.h
main/boards/sat1/board_sat1_beta_rev41.h
main/boards/sat1/board_sat1_common.h
```

The board-specific audio implementation lives in:

```text
main/boards/sat1/audio_sat1.c
```

Production firmware leaves the FUSB302B unconfigured and does not request a
non-default USB-C voltage. The separate legacy image owns all FUSB302B and
9 V negotiation code; none of that path is initialized by the production image.

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
