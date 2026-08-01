- Updates Voice PE, Satellite1, ReSpeaker XVF3800, and S3 Box to firmware
  version 0.3.3; Satellite1 is the only board with functional changes in this
  release.
- Adds four-microphone fractional-delay, delay-and-sum beamforming to
  Satellite1, steered by the XMOS direction-of-arrival estimate for clearer
  voice pickup.
- Smooths steering changes and automatically falls back to omnidirectional
  capture when a reliable speech direction is unavailable.
- Improves Satellite1 direction estimation with adaptive room-noise
  calibration, confidence filtering, and directional smoothing.
- Bundles Satellite1 XMOS firmware 1.0.9 and makes its ESP-side XMOS updater
  use DMA-safe SPI transfers for reliable on-device updates.
