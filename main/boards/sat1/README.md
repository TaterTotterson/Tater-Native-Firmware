# Satellite1 Native Board

Native board implementation for Satellite1 / Sat1.

Current status:

- included in `platformio.ini` as environment `sat1`
- included in the combined native release manifest as board key `satellite1`
- firmware uses the shared native version plus a board-only `revN` revision
  when Sat1 changes independently
- 24 LED ring support through the shared LED implementation
- GPIO0 action button support through the shared button implementation
- 48 kHz mic capture downsampled to 16 kHz mono for wake/STT streaming
- shared-duplex I2S speaker playback
- PCM5122/TAS2780 speaker path setup
- FUSB302B USB-C PD negotiation up to 20 V, with the TAS2780 configured for
  high-voltage mode only after an explicit contract of at least 9 V
- four-microphone XMOS DoA estimation with adaptive room-noise calibration,
  confidence filtering, and directional smoothing
- four-microphone fractional-delay, delay-and-sum beamforming steered by the
  XMOS DoA estimate, with continuous direction tracking, per-microphone gain
  calibration and health fallback, and automatic omni steering during playback
- directional LEDs reject weak/noise-only updates, hold the last speech
  direction briefly, and then return to a neutral listening glow
- bundled production XMOS factory image `1.1.1`
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

USB-C power negotiation lives in:

```text
main/boards/sat1/sat1_power_delivery.c
```

The speaker always starts from the safe 5 V TAS2780 profile. A fixed USB-PD
source profile up to 20 V is requested through the FUSB302B; after `PS_RDY`,
contracts of 9 V or higher select TAS2780 power mode 2. Missing controllers,
5 V-only adapters, rejected requests, and negotiation timeouts remain on power
mode 0, so the same image is safe for newer board revisions. Early public
Beta.1/rev4.1 boards require the higher-voltage contract (or their documented
VBAT hardware modification) for the onboard speaker rail to operate.

The bundled XMOS source and factory image live in:

```text
main/boards/sat1/xmos/
```
