# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

## Current status

Satellite1 is a fully supported Tater Native target, not an experimental board
port. The firmware version in this tree is `native-satellite1-0.3.11`, with
checksum-verified factory and OTA artifacts published under the `satellite1`
release family.
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

Tater Native supports Public Batch #2 hardware (HAT rev6.1 with Core rev5.1)
and later compatible revisions. Early Public Batch #1 / Beta.1 HAT and Core
rev4.1 hardware uses a different speaker-power path that requires a 9 V USB-PD
contract or the documented VBAT hardware modification, so it remains outside the
production firmware support range.

- 48 kHz microphone capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback through the PCM5122/TAS2780 path
- fixed TAS2780 power mode 0 using the default 5 V USB-C supply
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

The PlatformIO environment remains `sat1`. Shared releases use the native
firmware version, while Sat1-only updates add a `revN` suffix without forcing a
new build for every other supported board.

Unlike Voice PE, Sat1 updates XMOS by holding the XMOS in reset and directly
writing the external XMOS flash over SPI. The update happens before audio starts,
and each written page is read back for verification.

Hardware constants live in:

```text
main/boards/sat1/board_sat1.h
```

The board-specific audio implementation lives in:

```text
main/boards/sat1/audio_sat1.c
```

Production firmware leaves the FUSB302B unconfigured and does not request a
non-default USB-C voltage. This uses the standard 5 V power path of supported
production Satellite1 hardware without changing supply contracts during boot.

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
