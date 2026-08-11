# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

## Current status

Satellite1 is a fully supported Tater Native target, not an experimental board
port. The current public build is `native-satellite1-0.3.7`, with
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

- 48 kHz microphone capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback through the PCM5122/TAS2780 path
- FUSB302B USB-C PD negotiation up to 20 V, with the TAS2780 configured for
  high-voltage mode only after an explicit contract of at least 9 V
- four-microphone XMOS DoA estimation with adaptive room-noise calibration,
  confidence filtering, and directional smoothing
- four-microphone fractional-delay, delay-and-sum beamforming with continuous
  talker tracking, per-microphone gain calibration, unhealthy-microphone
  fallback, and automatic omni steering during playback
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

USB-C power negotiation lives in:

```text
main/boards/sat1/sat1_power_delivery.c
```

The speaker always starts from the safe 5 V TAS2780 profile. After the official
firmware's two-second startup stabilization window, a fixed USB-PD source
profile up to 20 V is requested through the FUSB302B; after `PS_RDY`,
contracts of 9 V or higher select TAS2780 power mode 2. Missing controllers,
5 V-only adapters, rejected requests, and negotiation timeouts remain on power
mode 0, preserving the existing amplifier profile on newer revisions. Early public
Beta.1/rev4.1 boards require the higher-voltage contract (or their documented
VBAT hardware modification) for the onboard speaker rail to operate.

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
