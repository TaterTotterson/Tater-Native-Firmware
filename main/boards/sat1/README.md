# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

## Current status

Satellite1 uses one Tater Native target for Public Batch #1 Beta.1/rev4.1,
Public Batch #2, and later compatible hardware. The firmware is
`native-satellite1-0.3.15`, with factory and OTA artifacts published under the
`satellite1` release family.
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

The Sat1 target starts in the safe 5 V/mode-0 state and requests the highest
fixed USB-PD PDO
offered from 5 V through 20 V. It uses TAS2780 mode 2 only after the source
accepts a contract of at least 9 V and sends `PS_RDY`; a confirmed 5 V contract
continues to use mode 0. If the controller, cable, or power source cannot
establish a supported contract, it stays at 5 V/mode 0. Its startup negotiation
has bounded recovery and does not continually renegotiate while the satellite
is running.

The former `satellite1_beta_rev41` target has been retired because both public
hardware revisions use the same safe power policy. A device already on the old
beta OTA family needs one manual USB installation of `satellite1`; subsequent
updates use the normal `satellite1` OTA family.

- 48 kHz microphone capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback through the PCM5122/TAS2780 path
- guarded TAS2780 power policy described above
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

The PlatformIO environment is `sat1` for all supported Satellite1 hardware.
Shared releases use the native firmware version, while board-only updates can
add a `revN` suffix without forcing a new build for every other supported
board.

Unlike Voice PE, Sat1 updates XMOS by holding the XMOS in reset and directly
writing the external XMOS flash over SPI. The update happens before audio starts,
and each written page is read back for verification.

Hardware constants live in:

```text
main/boards/sat1/board_sat1.h
main/boards/sat1/board_sat1_common.h
```

The board-specific audio implementation lives in:

```text
main/boards/sat1/audio_sat1.c
```

The Sat1 firmware configures the FUSB302B at startup, selects the highest
supported fixed USB-PD contract up to 20 V, and exposes the resulting contract
and TAS2780 mode in runtime diagnostics.

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
