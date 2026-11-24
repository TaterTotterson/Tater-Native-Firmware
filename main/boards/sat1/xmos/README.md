# Sat1 XMOS Firmware

The standard native `sat1` environment in **0.7.0-r2** embeds
`sat1_xmos_1_2_0_dev8_factory.bin.zlib` and installs XMOS **1.2.0-dev.8** when
another version is present. The same uncompressed image is bundled by RPi
Sat1 **0.1.19**.

The signal path is four raw microphones → calibrated steered beam plus omni →
stereo-reference 2Y2X AEC → **IC/VNR enabled** → noise suppression → AGC.
It retains fwk_voice 0.9.0, protocol 0x11, DoA resource 233, microphone health
and calibration, and Tater's buttons, LED feedback and steering controls.
The IC-bypassed dev.1 image remains available for comparison/recovery; failed
dev.2/dev.3 four-mic AEC experiments are excluded from release images.

See [IC-VNR-UPDATE.md](IC-VNR-UPDATE.md) for the dev.8 identity and verification,
and [INTEGRATION.md](INTEGRATION.md) for the historical integration variants.
The IC-enabled FPH ESPHome topology was tested earlier; Tater-host hardware
checks follow these builds.

The lab-only `sat1_xmos_1_1_0_raw4_usb_factory.bin` exposes the physical
microphones independently and remains outside ESP firmware payloads. Rebuilding
that target uses the source wrapper with
`SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4` and the `legacy` configuration.
Its USB target has independent version-generation settings; it is a lab tool,
not one of the native test images.
