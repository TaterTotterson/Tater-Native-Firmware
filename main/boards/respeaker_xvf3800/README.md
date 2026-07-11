# ReSpeaker XVF3800

Native Tater target for the ReSpeaker XVF3800 USB 4-mic array with XIAO
ESP32S3.

- included in `platformio.ini` as environment `respeaker_xvf3800`
- uses an 8MB flash partition layout
- audio is 48 kHz stereo I2S between the XIAO ESP32S3 and XVF3800
- microphone audio is downsampled to 16 kHz mono for wake/STT streaming
- LEDs, mute, amplifier, DoA, and XVF firmware update are controlled through
  the XVF3800 I2C command interface
- bundled XVF3800 I2S firmware target: `1.0.7`
- XVF3800 firmware binary and host protocol reference live under `xmos/`

Board-specific pins live in:

```text
main/boards/respeaker_xvf3800/board_respeaker_xvf3800.h
```

Board-specific audio/control code lives in:

```text
main/boards/respeaker_xvf3800/audio_respeaker_xvf3800.c
```

The old ReSpeaker ESPHome host component has been copied into:

```text
main/boards/respeaker_xvf3800/xmos/host_reference/
```

That folder is reference-only. It documents the XVF3800 I2C/DFU protocol we
ported into the native implementation.
