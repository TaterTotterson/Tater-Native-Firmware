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
- XMOS firmware version/status reporting for target `1.0.4`
- line-out capability advertised to Tater
- firmware-side AEC shared with Voice PE

Unlike Voice PE, Sat1 does not currently auto-flash the XMOS firmware image from
the ESP32. It reports the installed XMOS version and whether it differs from the
target so Tater can surface the state.

Hardware constants live in:

```text
main/boards/sat1/board_sat1.h
```

The board-specific audio implementation lives in:

```text
main/boards/sat1/audio_sat1.c
```
