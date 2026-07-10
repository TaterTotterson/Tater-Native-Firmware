# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

Current status:

- included in `platformio.ini` as environment `sat1`
- included in the combined native release manifest as board key `satellite1`
- firmware version is kept in step with the shared native release version
- 24 LED ring support through the shared LED implementation
- GPIO0 action button support through the shared button implementation
- 48 kHz mic capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback
- PCM5122/TAS2780 speaker path setup
- FUSB302B USB-C PD setup path
- XMOS DoA telemetry
- bundled XMOS factory image `1.0.6`
- boot-time XMOS auto-update when the installed image does not match the bundled image
- line-out capability advertised to Tater
- firmware-side AEC shared with Voice PE

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

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
