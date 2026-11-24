# Sat1 XMOS Firmware

The standard native `sat1` environment in **0.7.0-r1** embeds
`sat1_xmos_1_2_0_dev1_factory.bin.zlib` and installs XMOS **1.2.0-dev.1** when
another version is present. This is the exact working legacy-topology image
tested on the bench; its development identity is retained to preserve the
tested binary. The uncompressed image and MD5 are included alongside it.

It combines fwk_voice 0.9.0 and protocol 0x11 with Tater's calibrated four-mic
beam before AEC and stereo echo references. The failed dev.2/dev.3 four-mic AEC
experiments remain explicit test targets and are excluded from release images.
See [INTEGRATION.md](INTEGRATION.md) for provenance, builds and hardware results.
The previous 1.1.1 factory image remains available for rollback.

The lab-only `sat1_xmos_1_1_0_raw4_usb_factory.bin` exposes the physical
microphones independently and remains outside ESP firmware payloads. Rebuilding
that target uses the source wrapper with
`SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4` and the `legacy` configuration.
Its USB target has independent version-generation settings; it is a lab tool,
not one of the native test images.
