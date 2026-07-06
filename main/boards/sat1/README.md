# Satellite1 Native Board Scaffold

This folder captures the native board constants for Satellite1/Sat1 from the
old ESPHome configuration. It is intentionally not part of the release manifest
yet.

Required native driver work before this target can be published:

- Satellite1 XMOS control and DFU update path for `v1.0.4-dev.9` or newer.
- 48 kHz microphone capture with downsampling into the 16 kHz wake pipeline.
- Shared-duplex I2S speaker playback on GPIO7/GPIO8/GPIO9/GPIO16.
- PCM5122/TAS2780 DAC and amplifier setup.
- FUSB302B USB-C PD negotiation before high-power speaker mode.
- 24 LED ring mapping and action/volume button handling.

Keeping this as a scaffold makes the board split explicit without exposing a
flashable Sat1 image that has not been verified on hardware.
